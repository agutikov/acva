#pragma once

#include "config/config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace acva::audio {

// M8C Step 1 — wake-word inference wrapper.
//
// Mirrors the SileroVad surface (push_frame returns a probability)
// so the audio pipeline can call both with the same loop. Two
// implementations:
//
//   1. **Real ONNX path** (gated on `ACVA_HAVE_ONNXRUNTIME`).
//      Loads each `cfg.audio.wake_word.model_paths` entry as an
//      ONNX session and runs them on every resampled 16 kHz int16
//      frame. The current implementation is a placeholder that
//      always reports 0.0 — the openWakeWord pipeline (Mel
//      spectrogram + embedding model + per-word classifier head)
//      lands in a follow-up. Until then the gate framework is
//      testable via `set_test_score()`.
//
//   2. **Stub path** (no ONNX Runtime). Always reports 0.0.
//
// Either way the class is cheap to construct + cheap to call from
// the audio pipeline worker thread; the per-frame allocation cost
// is amortized across runs.
class WakeWord {
public:
    explicit WakeWord(const config::WakeWordConfig& cfg);
    ~WakeWord();

    WakeWord(const WakeWord&)            = delete;
    WakeWord& operator=(const WakeWord&) = delete;
    WakeWord(WakeWord&&)                 = delete;
    WakeWord& operator=(WakeWord&&)      = delete;

    // True when the WakeWord engine has at least one loaded model.
    // When `cfg.enabled` is true but no models loaded successfully
    // (paths missing, ONNX import failed), the engine logs a warn
    // at construction and `loaded()` returns false — the pipeline
    // gate then treats the wake-word path as a no-op (gate stays
    // open / closed per its initial state).
    [[nodiscard]] bool loaded() const noexcept;

    // Push one 16 kHz int16 frame. Returns the highest confidence
    // score across all loaded models, in [0..1]. Returns 0 when no
    // models are loaded OR when there's no positive evidence yet.
    // Thread-safe only insofar as one thread calls this at a time
    // (matching SileroVad's contract — the pipeline worker owns
    // the call site).
    [[nodiscard]] float push_frame(std::span<const std::int16_t> samples);

    // Tests inject a forced score that overrides the inference
    // result on the next push_frame call. -1 disables the override
    // and returns to the model's actual output. Used by
    // tests/test_wake_word.cpp + the pipeline-gate tests.
    void set_test_score(float score) noexcept { test_score_ = score; }

    // M8C Step 1 follow-up — observability surface.
    // The pipeline reads `threshold()` each frame; M8A reload
    // pushes new values via `update_threshold(...)` from the HTTP
    // /reload + SIGHUP path.
    [[nodiscard]] float threshold() const noexcept {
        return threshold_.load(std::memory_order_acquire);
    }
    void update_threshold(float v) noexcept {
        threshold_.store(v, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t detections_total() const noexcept {
        return detections_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float last_score() const noexcept {
        return last_score_.load(std::memory_order_acquire);
    }
    // Steady-clock instant of the last positive detection, default
    // construction (epoch) when none has fired yet.
    [[nodiscard]] std::chrono::steady_clock::time_point last_detection_at() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::nanoseconds{last_detection_ns_.load(std::memory_order_acquire)}};
    }

    // Number of loaded models. Useful for /status output.
    [[nodiscard]] std::size_t model_count() const noexcept;

private:
    struct Impl;

    config::WakeWordConfig cfg_;
    float                  test_score_ = -1.0F;

    // M8A-reloadable threshold + observability counters. Atomic so
    // the pipeline worker thread can read while the reload + status
    // threads write.
    std::atomic<float>        threshold_;
    std::atomic<std::uint64_t> detections_total_{0};
    std::atomic<float>         last_score_{0.0F};
    std::atomic<std::int64_t>  last_detection_ns_{0};

    std::unique_ptr<Impl>  impl_;
};

} // namespace acva::audio
