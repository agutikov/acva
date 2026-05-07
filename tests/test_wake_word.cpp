#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/pipeline.hpp"
#include "audio/wake_word.hpp"
#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using acva::audio::AudioPipeline;
using acva::audio::CaptureEngine;
using acva::audio::CaptureRing;
using acva::audio::MonotonicAudioClock;
using acva::audio::WakeWord;
using namespace std::chrono_literals;

namespace {

acva::config::AudioConfig audio_cfg() {
    acva::config::AudioConfig c;
    c.sample_rate_hz   = 48000;
    c.buffer_frames    = 480;
    c.input_device     = "none";
    c.capture_enabled  = true;
    return c;
}

AudioPipeline::Config pipeline_cfg_with_wake_word(
        bool enabled, std::uint32_t followup_window_ms = 8000) {
    AudioPipeline::Config c;
    c.input_sample_rate           = 48000;
    c.output_sample_rate          = 16000;
    c.endpointer.min_speech_ms    = 60ms;
    c.endpointer.hangover_ms      = 100ms;
    c.endpointer.pre_padding_ms   = 50ms;
    c.endpointer.post_padding_ms  = 30ms;
    c.pre_padding_ms              = 50ms;
    c.post_padding_ms             = 30ms;
    c.max_in_flight               = 3;
    c.max_duration_ms             = 5000ms;
    c.vad_model_path              = "";
    c.wake_word.enabled           = enabled;
    c.wake_word.threshold         = 0.6F;
    c.wake_word.followup_window_ms = followup_window_ms;
    return c;
}

std::size_t inject_and_pump(CaptureEngine& cap,
                              AudioPipeline& pipe,
                              std::size_t frames,
                              std::size_t sample_count = 480) {
    std::vector<std::int16_t> buf(sample_count, 1000);
    for (std::size_t i = 0; i < frames; ++i) {
        cap.inject_for_test(buf);
    }
    return pipe.pump_for_test(frames);
}

} // namespace

TEST_CASE("WakeWord: stub returns 0 with no models loaded") {
    acva::config::WakeWordConfig wcfg;
    wcfg.enabled = true;  // but no model_paths — engine reports loaded()=false
    WakeWord ww(wcfg);
    CHECK_FALSE(ww.loaded());

    std::vector<std::int16_t> samples(160, 0);
    CHECK(ww.push_frame(samples) == doctest::Approx(0.0));
}

TEST_CASE("WakeWord: set_test_score overrides the model output") {
    acva::config::WakeWordConfig wcfg;
    wcfg.enabled = true;
    WakeWord ww(wcfg);

    std::vector<std::int16_t> samples(160, 0);
    ww.set_test_score(0.85F);
    CHECK(ww.push_frame(samples) == doctest::Approx(0.85));
    ww.set_test_score(-1.0F);  // disable override
    CHECK(ww.push_frame(samples) == doctest::Approx(0.0));
}

TEST_CASE("AudioPipeline: gate closed → SpeechStarted suppressed") {
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    AudioPipeline pipe(pipeline_cfg_with_wake_word(/*enabled*/ true),
                        ring, clock, bus);

    std::atomic<int> speech_started{0};
    auto sub = bus.subscribe<acva::event::SpeechStarted>({},
        [&](const acva::event::SpeechStarted&) { speech_started.fetch_add(1); });

    // Wake-word stays at 0 (default test_score) — gate should be closed.
    REQUIRE(pipe.wake_word() != nullptr);
    pipe.wake_word()->set_test_score(0.0F);

    // Drive forced VAD probability that WOULD trigger SpeechStarted
    // if the gate were open. With the gate closed, the VAD path is
    // skipped entirely and no event fires.
    pipe.set_test_probability(0.9F);
    inject_and_pump(cap, pipe, 12);  // 120 ms of "speech"

    std::this_thread::sleep_for(50ms);  // bus dispatch settle
    CHECK(speech_started.load() == 0);
}

TEST_CASE("AudioPipeline: positive wake-word opens gate → SpeechStarted fires") {
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    AudioPipeline pipe(pipeline_cfg_with_wake_word(/*enabled*/ true),
                        ring, clock, bus);

    std::atomic<int> speech_started{0};
    auto sub = bus.subscribe<acva::event::SpeechStarted>({},
        [&](const acva::event::SpeechStarted&) { speech_started.fetch_add(1); });

    REQUIRE(pipe.wake_word() != nullptr);
    pipe.wake_word()->set_test_score(0.9F);  // above threshold = 0.6

    pipe.set_test_probability(0.9F);
    inject_and_pump(cap, pipe, 12);  // 120 ms of "speech" past min_speech_ms

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline
           && speech_started.load() == 0) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(speech_started.load() == 1);
}

TEST_CASE("AudioPipeline: gate disabled → behaves exactly like M5") {
    // With `wake_word.enabled = false`, the pipeline should NEVER
    // consult the wake-word state — even setting test_score to 0
    // shouldn't suppress SpeechStarted.
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    AudioPipeline pipe(pipeline_cfg_with_wake_word(/*enabled*/ false),
                        ring, clock, bus);

    std::atomic<int> speech_started{0};
    auto sub = bus.subscribe<acva::event::SpeechStarted>({},
        [&](const acva::event::SpeechStarted&) { speech_started.fetch_add(1); });

    // Force wake-word "low" — the gate-disabled path should ignore it.
    REQUIRE(pipe.wake_word() != nullptr);
    pipe.wake_word()->set_test_score(0.0F);

    pipe.set_test_probability(0.9F);
    inject_and_pump(cap, pipe, 12);

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline
           && speech_started.load() == 0) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(speech_started.load() == 1);
}
