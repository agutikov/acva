#include "demos/demo.hpp"

#include "audio/loopback.hpp"
#include "dialogue/barge_in.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/manager.hpp"
#include "dialogue/tts_bridge.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/client.hpp"
#include "llm/prompt_builder.hpp"
#include "memory/memory_thread.hpp"
#include "metrics/registry.hpp"
#include "orchestrator/stacks/capture_stack.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"
#include "tts/openai_tts_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <variant>

namespace acva::demos {

namespace {

std::filesystem::path tmp_db_path() {
    namespace fs = std::filesystem;
    auto p = fs::temp_directory_path()
           / (std::string{"acva-demo-bargein-validation-"}
              + std::to_string(::getpid()) + ".db");
    fs::remove(p);
    return p;
}

} // namespace

// `acva demo bargein-validation` — drives the full M7 barge-in path
// (capture → AudioPipeline → BargeInDetector → cascade) using a
// pre-rendered WAV fixture instead of a live mic. Used by the M7B
// validation harness (`scripts/validate-bargein.py`).
//
// The pipeline construction mirrors `demo bargein` for the LLM + TTS +
// playback half, then adds the capture stack and BargeInDetector. The
// fixture WAV is fed by `audio::CaptureEngine`'s wav-source mode (M7B
// Step 2), gated by `cfg.audio.test_input_wav`.
//
// Timing alignment: the WAV starts pumping when capture.start() is
// called. To make the manifest's `start_s: 1.8` align with "1.8 s
// after the assistant began speaking", the demo defers
// capture.start() until the FIRST `TtsAudioChunk` arrives on the bus.
// At that point the FSM is in `Speaking` and the BargeInDetector is
// armed.
//
// Done-line format (machine-readable; harness parses it):
//   demo[bargein-validation] done: cancellation=Y latency_ms=X.X
//       sentences_played=P sentences_dropped=D
//       barge_in_fires=F barge_in_suppressed_aec=A barge_in_suppressed_cooldown=C
//       outcome=interrupted|completed
int run_bargein_validation(const config::Config& orig_cfg,
                            std::span<const std::string> args) {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    std::string fixture_path;
    long timeout_ms = 30000;
    long prompt_max_sentences = 8;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--fixture" && i + 1 < args.size()) {
            fixture_path = args[++i];
        } else if (a.starts_with("--fixture=")) {
            fixture_path = a.substr(10);
        } else if (a == "--timeout-ms" && i + 1 < args.size()) {
            timeout_ms = std::stol(args[++i]);
        } else if (a == "-h" || a == "--help") {
            std::printf(
                "demo[bargein-validation] options:\n"
                "  --fixture <path>     WAV fixture to feed into capture (required)\n"
                "  --timeout-ms <N>     overall wall-clock cap (default 30000)\n");
            return EXIT_SUCCESS;
        }
    }
    if (fixture_path.empty()) {
        std::fprintf(stderr,
            "demo[bargein-validation] FAIL: --fixture <path> required\n");
        return EXIT_FAILURE;
    }
    if (!std::filesystem::exists(fixture_path)) {
        std::fprintf(stderr,
            "demo[bargein-validation] FAIL: fixture not found: %s\n",
            fixture_path.c_str());
        return EXIT_FAILURE;
    }
    if (orig_cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[bargein-validation] FAIL: cfg.tts.voices is empty\n");
        return EXIT_FAILURE;
    }

    const auto db_path = tmp_db_path();
    config::Config cfg = orig_cfg;
    cfg.memory.db_path                = db_path.string();
    cfg.audio.test_input_wav          = fixture_path;
    cfg.audio.capture_enabled         = true;
    cfg.audio.half_duplex_while_speaking = false;   // M7 requires full duplex
    cfg.barge_in.enabled              = true;
    // Synthetic fixtures don't drive a real AEC; tell the detector
    // system AEC is "active" so the convergence gate is satisfied.
    // The fixture's gain attenuation simulates post-AEC residual; the
    // cooldown + min_real_utterance_chars filters do the real
    // suppression work.
    cfg.apm.use_system_aec            = true;
    cfg.dialogue.sentence_splitter.max_sentence_chars = 1500;
    cfg.dialogue.max_assistant_sentences =
        static_cast<std::uint32_t>(prompt_max_sentences);

    // Long single-sentence prompt — keeps the assistant in `Speaking`
    // for at least the fixture's full duration so the user track lands
    // mid-sentence.
    constexpr std::string_view kPrompt =
        "Describe the Moon in one extremely long English sentence with "
        "many descriptive clauses joined by commas, covering its origin, "
        "its distance from Earth, its gravitational pull, the lunar phases, "
        "tidal influence on the oceans, its surface features, the Apollo "
        "missions, and its role in human culture, please do not use any "
        "periods, only commas to keep all of those ideas linked together.";

    std::printf(
        "demo[bargein-validation] fixture=%s timeout_ms=%ld\n",
        fixture_path.c_str(), timeout_ms);

    event::EventBus bus;
    auto registry = std::make_shared<metrics::Registry>();

    auto mem_or = memory::MemoryThread::open(cfg.memory.db_path,
                                              cfg.memory.write_queue_capacity);
    if (auto* err = std::get_if<memory::DbError>(&mem_or)) {
        std::fprintf(stderr,
            "demo[bargein-validation] memory open: %s\n",
            err->message.c_str());
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<memory::MemoryThread>>(mem_or));

    llm::LlmClient client(cfg, bus);
    if (!client.probe()) {
        std::fprintf(stderr,
            "demo[bargein-validation] FAIL: llama /health probe failed at %s\n",
            cfg.llm.base_url.c_str());
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }

    llm::PromptBuilder       pb(cfg, *memory);
    dialogue::TurnFactory    turns;
    dialogue::Fsm            fsm(bus, turns);
    dialogue::Manager        manager(cfg, bus, pb, client, turns);

    playback::PlaybackQueue  queue(cfg.playback.max_queue_chunks);
    tts::OpenAiTtsClient     tts_client(cfg.tts);

    std::atomic<event::TurnId> playback_turn{event::kNoTurn};
    auto sub_started = bus.subscribe<event::LlmStarted>({},
        [&](const event::LlmStarted& e) {
            playback_turn.store(e.turn, std::memory_order_release);
        });

    audio::LoopbackSink loopback(cfg.audio.sample_rate_hz,
                                  cfg.audio.loopback.ring_seconds);
    playback::PlaybackEngine engine(cfg.audio, cfg.playback, queue, bus,
        [&]{ return playback_turn.load(std::memory_order_acquire); });
    engine.set_loopback_sink(&loopback);
    dialogue::TtsBridge bridge(cfg, bus,
        [&](tts::TtsRequest r, tts::TtsCallbacks cb) {
            tts_client.submit(std::move(r), std::move(cb));
        }, queue);

    // BargeInDetector — wired to FSM, system-AEC mode (gate trusted).
    dialogue::BargeInDetector barge_in(bus, fsm, /*apm=*/nullptr,
                                        /*system_aec_active=*/true,
                                        cfg.barge_in);
    std::atomic<bool> bi_fired{false};
    std::atomic<event::TurnId> bi_turn{event::kNoTurn};
    std::atomic<steady_clock::time_point> bi_at{steady_clock::time_point{}};
    barge_in.set_on_fired([&](event::TurnId t, steady_clock::time_point ts) {
        bi_turn.store(t, std::memory_order_release);
        bi_at.store(ts, std::memory_order_release);
        bi_fired.store(true, std::memory_order_release);
        engine.note_barge_in(t, ts);
    });

    // Counters for the done-line.
    std::atomic<std::size_t> sentences_emitted{0};
    auto sub_emitted = bus.subscribe<event::LlmSentence>({},
        [&](const event::LlmSentence&) {
            sentences_emitted.fetch_add(1, std::memory_order_relaxed);
        });
    std::atomic<std::size_t> sentences_played{0};
    auto sub_pf = bus.subscribe<event::PlaybackFinished>({},
        [&](const event::PlaybackFinished&) {
            sentences_played.fetch_add(1, std::memory_order_relaxed);
        });
    std::atomic<bool> llm_finished{false};
    std::atomic<bool> llm_cancelled{false};
    auto sub_finished = bus.subscribe<event::LlmFinished>({},
        [&](const event::LlmFinished& e) {
            llm_cancelled.store(e.cancelled, std::memory_order_relaxed);
            llm_finished.store(true, std::memory_order_release);
        });

    // First-chunk anchor — defer capture.start() until TTS audio
    // begins, so the fixture's t=0 aligns with assistant Speaking.
    std::atomic<bool> first_chunk_seen{false};
    auto sub_chunk = bus.subscribe<event::TtsAudioChunk>({},
        [&](const event::TtsAudioChunk&) {
            first_chunk_seen.store(true, std::memory_order_release);
        });

    // Build capture stack but defer its internal start until anchor.
    // build_capture_stack constructs CaptureEngine + AudioPipeline but
    // does not auto-start; main.cpp / orchestrator owns the start.
    // (capture_stack constructs them; we explicitly start the engine
    // after the anchor.)
    auto capture = orchestrator::build_capture_stack(cfg, bus, registry, fsm, &loopback);
    if (!capture->enabled()) {
        std::fprintf(stderr,
            "demo[bargein-validation] FAIL: capture stack not enabled\n");
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }
    // build_capture_stack auto-starts the engine — stop it immediately
    // so the WAV pump doesn't run before the assistant is Speaking.
    // We restart it after the first-chunk anchor below; the pump
    // re-reads from sample 0, so the manifest's `start_s` aligns with
    // "seconds after assistant began speaking".
    capture->capture()->stop();

    fsm.start();
    if (!engine.start()) {
        std::fprintf(stderr,
            "demo[bargein-validation] engine.start() failed\n");
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }
    bridge.start();
    manager.start();
    barge_in.start();

    // Drive the FSM through a normal user turn:
    // SpeechStarted → SpeechEnded → FinalTranscript. The FSM mints
    // a turn on SpeechStarted (Listening → UserSpeaking) and only
    // transitions to Thinking via FinalTranscript when it sees one
    // from Transcribing — so we have to take the long way round, not
    // jump straight from Listening with FinalTranscript.
    const auto t_start = steady_clock::now();
    bus.publish(event::SpeechStarted{.turn = event::kNoTurn, .ts = t_start});
    std::this_thread::sleep_for(20ms);
    bus.publish(event::SpeechEnded{
        .turn = event::kNoTurn, .ts = steady_clock::now()});
    bus.publish(event::FinalTranscript{
        .turn                = event::kNoTurn,
        .text                = std::string{kPrompt},
        .lang                = cfg.tts.fallback_lang,
        .confidence          = 1.0F,
        .audio_duration      = {},
        .processing_duration = {},
    });

    // Wait for first TtsAudioChunk — that's the anchor for the WAV
    // timeline. Once it arrives we restart the capture engine; its
    // pump thread re-reads the WAV from sample 0, so the manifest's
    // `start_s: 1.8` lands 1.8 s into actual playback.
    const auto chunk_deadline = t_start + 30s;
    while (!first_chunk_seen.load(std::memory_order_acquire)
           && steady_clock::now() < chunk_deadline) {
        std::this_thread::sleep_for(20ms);
    }
    if (!first_chunk_seen.load()) {
        std::fprintf(stderr,
            "demo[bargein-validation] FAIL: no TtsAudioChunk within 30s — TTS slow or wedged\n");
        manager.stop(); bridge.stop(); engine.stop(); fsm.stop();
        bus.shutdown();
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }
    const auto t_speaking = steady_clock::now();
    capture->capture()->start();

    // Wait for either: (a) BargeInDetector fires + cascade drains,
    // (b) wav source finishes, (c) overall timeout.
    const auto deadline = t_start + std::chrono::milliseconds(timeout_ms);
    while (steady_clock::now() < deadline) {
        if (bi_fired.load(std::memory_order_acquire)
            && (llm_finished.load(std::memory_order_acquire)
                || queue.size() == 0)) {
            // Give the cascade one more tick to drain.
            std::this_thread::sleep_for(50ms);
            break;
        }
        std::this_thread::sleep_for(20ms);
    }

    // Extract latency. BargeInDetector publish_ts → first silent buffer
    // is captured by PlaybackEngine via note_barge_in/consume_pending.
    double latency_ms = engine.consume_pending_barge_in_latency_ms();
    if (latency_ms < 0.0 && bi_fired.load()) {
        latency_ms =
            duration<double, std::milli>(steady_clock::now()
                                          - bi_at.load(std::memory_order_acquire)).count();
    }
    const auto played   = sentences_played.load(std::memory_order_relaxed);
    const auto emitted  = sentences_emitted.load(std::memory_order_relaxed);
    const auto dropped  = (emitted > played) ? (emitted - played) : 0U;
    const bool fired    = bi_fired.load(std::memory_order_acquire);
    const bool cancelled = llm_cancelled.load(std::memory_order_relaxed);

    barge_in.stop();
    manager.stop();
    bridge.stop();
    engine.stop();
    capture->stop();
    sub_started->stop();
    sub_emitted->stop();
    sub_pf->stop();
    sub_chunk->stop();
    sub_finished->stop();
    fsm.stop();
    bus.shutdown();

    // Persistence check: leave the SQLite db on disk and print its
    // path. The harness opens it via Python's sqlite3 to verify the
    // M7 §6 acceptance — that an interrupted turn persists only the
    // played-out text, not the full LLM stream.
    std::printf(
        "demo[bargein-validation] done: cancellation=%c latency_ms=%.1f "
        "sentences_played=%zu sentences_dropped=%zu "
        "barge_in_fires=%llu barge_in_suppressed_aec=%llu "
        "barge_in_suppressed_cooldown=%llu outcome=%s db=%s\n",
        fired ? 'Y' : 'N', latency_ms,
        played, dropped,
        static_cast<unsigned long long>(barge_in.fires_total()),
        static_cast<unsigned long long>(barge_in.suppressed_aec()),
        static_cast<unsigned long long>(barge_in.suppressed_cooldown()),
        cancelled ? "interrupted" : "completed",
        db_path.string().c_str());

    (void)t_speaking;       // anchor logged for human eyes via stderr
    return EXIT_SUCCESS;
}

} // namespace acva::demos
