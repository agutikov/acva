#include "audio/utterance.hpp"
#include "audio/wake_word.hpp"
#include "audio/wav.hpp"
#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "stt/openai_stt_client.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

// `acva demo wake-word-offline --wav <path>` — replays a 16 kHz mono
// int16 WAV through both the wake-word engine (offline, no live mic,
// no AEC) and Speaches STT, then prints both verdicts side by side.
//
// Useful when the live `acva demo wake-word` reports near-zero scores
// and you don't know whether the mic is silent, AEC is over-cancelling,
// or the model genuinely missed the phrase. Pair with
// scripts/wake-word-offline.sh, which records raw + AEC for you and
// runs the demo on each.
struct Args {
    std::string wav_path;
    bool        skip_stt   = false;
    bool        skip_wake  = false;
    int         exit_code  = 0;
    bool        parsed_ok  = true;
};

Args parse_args(std::span<const std::string> args) {
    Args a;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& s = args[i];
        if (s == "--wav" && i + 1 < args.size()) {
            a.wav_path = args[++i];
        } else if (s == "--no-stt") {
            a.skip_stt = true;
        } else if (s == "--no-wake") {
            a.skip_wake = true;
        } else {
            std::fprintf(stderr,
                "demo[wake-word-offline] unknown arg '%s'\n", s.c_str());
            a.parsed_ok = false;
        }
    }
    if (a.wav_path.empty()) {
        std::fprintf(stderr,
            "demo[wake-word-offline] usage: acva demo wake-word-offline "
            "--wav <path> [--no-stt] [--no-wake]\n");
        a.parsed_ok = false;
    }
    return a;
}

// 80 ms at 16 kHz — the openWakeWord step size. Matches WakeWord's
// internal kAudioStep so each push_frame call drains one inference
// step and we get a fresh per-step score, not the buffered max from
// last_score().
constexpr std::size_t kStepSamples = 1280;

void score_wav(const std::vector<std::int16_t>& samples,
                std::uint32_t sample_rate_hz,
                const config::WakeWordConfig& cfg) {
    if (sample_rate_hz != 16000) {
        std::fprintf(stderr,
            "demo[wake-word-offline] WAV is %u Hz; wake-word expects 16 kHz. "
            "Skipping wake-word pass.\n", sample_rate_hz);
        return;
    }

    config::WakeWordConfig forced = cfg;
    forced.enabled = true;
    audio::WakeWord ww(forced);
    if (!ww.loaded()) {
        std::fprintf(stderr,
            "demo[wake-word-offline] WakeWord engine did not load — check "
            "audio.wake_word.model_paths in the config.\n");
        return;
    }

    const float threshold = ww.threshold();
    std::printf("[WAKE] models=%zu  threshold=%.2f\n",
                 ww.model_count(),
                 static_cast<double>(threshold));

    // Per-second max score so the operator can see when in the recording
    // the model fired (or didn't). 80 ms steps × ~12-13 per second.
    float        max_overall  = 0.0F;
    std::size_t  steps_total  = 0;
    std::size_t  steps_above  = 0;
    int          cur_sec      = 0;
    float        cur_sec_max  = 0.0F;
    std::size_t  cur_sec_above= 0;

    auto flush_second = [&](int sec) {
        std::printf("  t=%2ds..%2ds  max=%.3f%s\n",
                     sec, sec + 1,
                     static_cast<double>(cur_sec_max),
                     cur_sec_above > 0 ? "  *** ABOVE THRESHOLD ***" : "");
        cur_sec_max   = 0.0F;
        cur_sec_above = 0;
    };

    for (std::size_t off = 0; off + kStepSamples <= samples.size();
         off += kStepSamples) {
        std::span<const std::int16_t> chunk(samples.data() + off, kStepSamples);
        const float s = ww.push_frame(chunk);
        ++steps_total;
        if (s > max_overall) max_overall = s;
        if (s >= threshold)  ++steps_above;

        // Roll up per-second.
        const int sec = static_cast<int>(off / sample_rate_hz);
        if (sec != cur_sec) {
            flush_second(cur_sec);
            cur_sec = sec;
        }
        if (s > cur_sec_max) cur_sec_max = s;
        if (s >= threshold)  ++cur_sec_above;
    }
    if (steps_total > 0) flush_second(cur_sec);

    const double pct = steps_total > 0
        ? 100.0 * static_cast<double>(steps_above) / static_cast<double>(steps_total)
        : 0.0;
    std::printf("[WAKE] summary: steps=%zu  max=%.3f  above=%zu (%.1f%%)\n",
                 steps_total,
                 static_cast<double>(max_overall),
                 steps_above, pct);
}

void transcribe_wav(const std::vector<std::int16_t>& samples,
                     std::uint32_t sample_rate_hz,
                     const config::SttConfig& cfg) {
    if (cfg.base_url.empty()) {
        std::fprintf(stderr,
            "[STT] cfg.stt.base_url is empty — Speaches not configured. "
            "Skipping STT pass.\n");
        return;
    }
    if (sample_rate_hz != 16000) {
        // OpenAiSttClient WAV-wraps and ships at the slice's rate; Whisper
        // accepts arbitrary rates and resamples server-side, so just warn.
        std::fprintf(stderr,
            "[STT] WAV is %u Hz; Whisper will resample on its end.\n",
            sample_rate_hz);
    }

    auto slice = std::make_shared<audio::AudioSlice>(
        std::vector<std::int16_t>(samples), sample_rate_hz,
        std::chrono::steady_clock::now(),
        std::chrono::steady_clock::now());

    stt::OpenAiSttClient client(cfg);
    std::string text;
    std::string err;
    stt::SttCallbacks cb;
    cb.on_final = [&](event::FinalTranscript ft) { text = std::move(ft.text); };
    cb.on_error = [&](std::string e) { err = std::move(e); };

    const auto t0 = std::chrono::steady_clock::now();
    client.submit(stt::SttRequest{
        .turn      = 1,
        .slice     = slice,
        .cancel    = std::make_shared<dialogue::CancellationToken>(),
        .lang_hint = cfg.language,
    }, cb);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    if (!err.empty()) {
        std::fprintf(stderr, "[STT] FAIL: %s\n", err.c_str());
        return;
    }
    std::printf("[STT] %lld ms  transcript=\"%s\"\n",
                 static_cast<long long>(ms), text.c_str());
}

} // namespace

int run_wake_word_offline(const config::Config& cfg,
                           std::span<const std::string> args) {
    const Args a = parse_args(args);
    if (!a.parsed_ok) return EXIT_FAILURE;

    std::uint32_t sample_rate_hz = 0;
    auto samples = audio::read_wav_file(a.wav_path, sample_rate_hz);
    if (samples.empty()) {
        std::fprintf(stderr,
            "demo[wake-word-offline] could not read mono int16 PCM WAV from "
            "'%s' — is it 16-bit mono?\n", a.wav_path.c_str());
        return EXIT_FAILURE;
    }
    const double duration_s = sample_rate_hz > 0
        ? static_cast<double>(samples.size())
              / static_cast<double>(sample_rate_hz)
        : 0.0;
    std::printf(
        "demo[wake-word-offline] wav='%s' samples=%zu rate=%u Hz duration=%.2fs\n",
        a.wav_path.c_str(), samples.size(), sample_rate_hz, duration_s);

    if (!a.skip_stt) {
        transcribe_wav(samples, sample_rate_hz, cfg.stt);
    }
    if (!a.skip_wake) {
        score_wav(samples, sample_rate_hz, cfg.audio.wake_word);
    }

    return EXIT_SUCCESS;
}

} // namespace acva::demos
