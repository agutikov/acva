#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/pipeline.hpp"
#include "audio/wake_word.hpp"
#include "config/config.hpp"
#include "event/bus.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

namespace acva::demos {

namespace {

// `acva demo wake-word [--duration <s>]` — operator-side threshold
// tuning aid for the configured wake-word model(s). Streams mic
// audio through the AudioPipeline (so the demo exercises the same
// resample → APM → WakeWord → VAD code path the production stack
// uses), polls `WakeWord::last_score()` at 10 Hz, prints a per-tick
// confidence row, and finishes with a summary (max score, samples
// above threshold, total detections that crossed the threshold).
//
// Defaults to a 5 s capture; `--duration <s>` overrides. Honors the
// loaded `cfg.audio.wake_word.threshold` so the operator can edit
// `~/.config/acva/default.yaml` and re-run without recompiling.
constexpr std::chrono::milliseconds kPollPeriod{100};

int parse_duration_seconds(std::span<const std::string> args, int fallback) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--duration") {
            const auto& v = args[i + 1];
            int seconds = 0;
            const auto* begin = v.data();
            const auto* end   = begin + v.size();
            auto [p, ec] = std::from_chars(begin, end, seconds);
            if (ec == std::errc{} && p == end && seconds > 0) {
                return seconds;
            }
            std::fprintf(stderr,
                "demo[wake-word] WARNING: --duration '%s' is not a "
                "positive integer; using default %d s\n",
                v.c_str(), fallback);
            return fallback;
        }
    }
    return fallback;
}

} // namespace

int run_wake_word(const config::Config& cfg,
                   std::span<const std::string> args) {
    const int duration_s = parse_duration_seconds(args, 5);

    if (cfg.audio.wake_word.model_paths.empty()) {
        std::fprintf(stderr,
            "demo[wake-word] cfg.audio.wake_word.model_paths is empty — "
            "set `audio.wake_word.enabled: true` and add at least one "
            "alias (e.g. `hey-jarvis`) under `audio.wake_word.model_paths` "
            "in your config. Run `tools/acva-models install hey-jarvis` "
            "first.\n");
        return EXIT_FAILURE;
    }

    std::printf(
        "demo[wake-word] device='%s' duration=%ds threshold=%.2f models=",
        cfg.audio.input_device.c_str(),
        duration_s,
        static_cast<double>(cfg.audio.wake_word.threshold));
    for (std::size_t i = 0; i < cfg.audio.wake_word.model_paths.size(); ++i) {
        std::printf("%s%s", (i ? "," : ""),
                     cfg.audio.wake_word.model_paths[i].c_str());
    }
    std::printf("\n");

    event::EventBus bus;
    audio::MonotonicAudioClock clock;
    audio::CaptureRing ring;
    audio::CaptureEngine capture(cfg.audio, ring, clock);
    if (!capture.start()) {
        std::fprintf(stderr, "demo[wake-word] capture.start() failed\n");
        return EXIT_FAILURE;
    }

    audio::AudioPipeline::Config apc;
    apc.input_sample_rate           = cfg.audio.sample_rate_hz;
    apc.output_sample_rate          = 16000;
    apc.endpointer.onset_threshold  = cfg.vad.onset_threshold;
    apc.endpointer.offset_threshold = cfg.vad.offset_threshold;
    apc.endpointer.min_speech_ms    = std::chrono::milliseconds{cfg.vad.min_speech_ms};
    apc.endpointer.hangover_ms      = std::chrono::milliseconds{cfg.vad.hangover_ms};
    apc.endpointer.pre_padding_ms   = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
    apc.endpointer.post_padding_ms  = std::chrono::milliseconds{cfg.vad.post_padding_ms};
    apc.pre_padding_ms              = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
    apc.post_padding_ms             = std::chrono::milliseconds{cfg.vad.post_padding_ms};
    apc.max_in_flight               = cfg.utterance.max_in_flight;
    apc.max_duration_ms             = std::chrono::milliseconds{cfg.utterance.max_duration_ms};
    apc.vad_model_path              = cfg.vad.model_path;
    apc.wake_word                   = cfg.audio.wake_word;
    // Force enabled — the demo's whole reason to exist.
    apc.wake_word.enabled           = true;

    audio::AudioPipeline pipeline(std::move(apc), ring, clock, bus);
    pipeline.start();

    audio::WakeWord* ww = pipeline.wake_word();
    if (!ww || !ww->loaded()) {
        std::fprintf(stderr,
            "demo[wake-word] WakeWord engine did not load — check the "
            "model paths above and the warn lines emitted at startup.\n");
        capture.stop();
        pipeline.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }
    std::printf("demo[wake-word] %zu classifier(s) ready; warm-up ~2.6 s, then say the wake word…\n",
                 ww->model_count());

    const float    threshold = ww->threshold();
    const auto     t0        = std::chrono::steady_clock::now();
    const auto     deadline  = t0 + std::chrono::seconds(duration_s);
    float          max_score = 0.0F;
    std::size_t    above     = 0;
    std::size_t    ticks     = 0;
    std::uint64_t  detections_at_start = ww->detections_total();

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPollPeriod);
        const float s = ww->last_score();
        if (s > max_score) max_score = s;
        if (s >= threshold) ++above;
        ++ticks;
        const double elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
        std::printf("  t=%.1fs  score=%.3f%s\n",
                     elapsed,
                     static_cast<double>(s),
                     s >= threshold ? "  *** ABOVE THRESHOLD ***" : "");
        std::fflush(stdout);
    }

    capture.stop();
    pipeline.stop();
    bus.shutdown();

    const std::uint64_t detections_total =
        ww->detections_total() - detections_at_start;

    std::printf(
        "demo[wake-word] done: ticks=%zu max=%.3f threshold=%.2f "
        "above=%zu (%.1f%% of ticks) detections=%llu\n",
        ticks,
        static_cast<double>(max_score),
        static_cast<double>(threshold),
        above,
        ticks > 0
            ? 100.0 * static_cast<double>(above) / static_cast<double>(ticks)
            : 0.0,
        static_cast<unsigned long long>(detections_total));

    if (capture.headless()) {
        std::printf(
            "demo[wake-word] NOTE: capture ran in headless mode — no audio "
            "reached the pipeline. Set audio.input_device to a real device "
            "and re-run.\n");
    }

    return EXIT_SUCCESS;
}

} // namespace acva::demos
