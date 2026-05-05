#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/wav.hpp"
#include "config/config.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

using acva::audio::CaptureEngine;
using acva::audio::CaptureRing;
using acva::audio::MonotonicAudioClock;
using namespace std::chrono_literals;

namespace {

std::filesystem::path tmp_wav(std::string_view tag,
                              std::uint32_t rate_hz,
                              std::size_t  sample_count,
                              std::int16_t fill = 0) {
    std::vector<std::int16_t> samples(sample_count, fill);
    auto path = std::filesystem::temp_directory_path()
                / (std::string("acva-capture-wav-") + std::string{tag} + ".wav");
    std::filesystem::remove(path);
    REQUIRE(acva::audio::write_wav_file(path.string(), samples, rate_hz));
    return path;
}

acva::config::AudioConfig wav_cfg(const std::filesystem::path& wav,
                                  double mult = 4.0) {
    acva::config::AudioConfig c;
    c.sample_rate_hz                = 48000;
    c.buffer_frames                 = 480;            // 10 ms slice
    c.input_device                  = "none";
    c.capture_enabled               = true;
    c.test_input_wav                = wav.string();
    c.test_input_rate_multiplier    = mult;
    return c;
}

} // namespace

TEST_CASE("capture wav-source pumps all samples into the ring") {
    // 1 second of audio at 48 kHz → 48000 samples → 100 slices of 480.
    const auto path = tmp_wav("basic", 48000, 48000, /*fill=*/123);
    auto cfg = wav_cfg(path, /*mult=*/8.0);  // compress wall-clock for the test

    CaptureRing ring;
    MonotonicAudioClock clock;
    CaptureEngine eng(cfg, ring, clock);
    REQUIRE(eng.start());
    CHECK(eng.wav_source());
    CHECK_FALSE(eng.headless());

    // 1 s of audio at mult=8 → ~125 ms wall-clock; allow generous slack.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!eng.wav_finished()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(eng.wav_finished());

    // Drain the ring and count frames; every slice has count=480.
    std::size_t total_samples = 0;
    while (auto frame = ring.pop()) {
        total_samples += frame->count;
    }
    // All 48000 samples should have been pushed (no overruns at this
    // ring capacity for a 1 s file at 8× speedup).
    CHECK(total_samples == 48000);
    CHECK(eng.frames_captured() == 48000);
    CHECK(eng.ring_overruns() == 0);

    eng.stop();
    std::filesystem::remove(path);
}

TEST_CASE("capture wav-source rejects rate mismatch (falls back to headless)") {
    // 16 kHz fixture but cfg.sample_rate_hz = 48 kHz → reject + headless.
    const auto path = tmp_wav("ratemismatch", 16000, 16000);
    auto cfg = wav_cfg(path);

    CaptureRing ring;
    MonotonicAudioClock clock;
    CaptureEngine eng(cfg, ring, clock);
    REQUIRE(eng.start());
    // Falls back to headless rather than wav-source.
    CHECK_FALSE(eng.wav_source());
    CHECK(eng.headless());
    eng.stop();
    std::filesystem::remove(path);
}

TEST_CASE("capture wav-source stop joins the pump thread cleanly") {
    // 5 s file at mult=1 → never reaches eof inside the test window.
    // Stop should join immediately without waiting for the file to finish.
    const auto path = tmp_wav("stop", 48000, 48000 * 5);
    auto cfg = wav_cfg(path, /*mult=*/1.0);

    CaptureRing ring;
    MonotonicAudioClock clock;
    CaptureEngine eng(cfg, ring, clock);
    REQUIRE(eng.start());
    std::this_thread::sleep_for(50ms);

    const auto t0 = std::chrono::steady_clock::now();
    eng.stop();
    const auto stop_dur = std::chrono::steady_clock::now() - t0;
    CHECK(stop_dur < 200ms);
    CHECK_FALSE(eng.running());
    std::filesystem::remove(path);
}
