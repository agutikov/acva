#include "audio/wake_word.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <filesystem>

#ifdef ACVA_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace acva::audio {

#ifdef ACVA_HAVE_ONNXRUNTIME

struct WakeWord::Impl {
    Ort::Env       env{ORT_LOGGING_LEVEL_WARNING, "acva-wake-word"};
    Ort::SessionOptions opts;
    std::vector<Ort::Session> sessions;

    Impl() {
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    }
};

#else  // stub

struct WakeWord::Impl {};

#endif

WakeWord::WakeWord(const config::WakeWordConfig& cfg)
    : cfg_(cfg), impl_(std::make_unique<Impl>()) {

    if (!cfg_.enabled) {
        // Engine is constructed but inert — pipeline checks
        // cfg.enabled before consulting us, so this is just defensive.
        return;
    }

#ifdef ACVA_HAVE_ONNXRUNTIME
    for (const auto& raw : cfg_.model_paths) {
        std::filesystem::path p(raw);
        if (!std::filesystem::exists(p)) {
            log::warn("wake_word", fmt::format(
                "model not found: {} — skipping", raw));
            continue;
        }
        try {
            impl_->sessions.emplace_back(
                impl_->env, p.c_str(), impl_->opts);
            log::info("wake_word", fmt::format(
                "loaded model: {}", raw));
        } catch (const Ort::Exception& ex) {
            log::warn("wake_word", fmt::format(
                "failed to load {}: {}", raw, ex.what()));
        }
    }
    if (impl_->sessions.empty()) {
        log::warn("wake_word",
            "wake_word.enabled=true but no models loaded — gate "
            "will behave as if wake-word is disabled");
    } else {
        log::info("wake_word", fmt::format(
            "{} model(s) loaded; threshold={:.2f} window_ms={}",
            impl_->sessions.size(), cfg_.threshold,
            cfg_.followup_window_ms));
    }
#else
    if (!cfg_.model_paths.empty()) {
        log::warn("wake_word",
            "ACVA_HAVE_ONNXRUNTIME not defined — wake_word is a "
            "no-op stub. Install onnxruntime + rebuild to enable.");
    }
#endif
}

WakeWord::~WakeWord() = default;

bool WakeWord::loaded() const noexcept {
#ifdef ACVA_HAVE_ONNXRUNTIME
    return impl_ && !impl_->sessions.empty();
#else
    return false;
#endif
}

float WakeWord::push_frame(std::span<const std::int16_t> /*samples*/) {
    if (test_score_ >= 0.0F) {
        return test_score_;
    }
    if (!cfg_.enabled || !loaded()) return 0.0F;

    // Real ONNX inference is a follow-up — openWakeWord's published
    // models use a 3-stage pipeline (Mel spectrogram → embedding
    // model → per-word classifier) and the right Mel preprocessor
    // depends on which model variant the operator picked. The gate
    // framework + tests are exercised via set_test_score for v1;
    // landing the actual inference is the next slice.
    return 0.0F;
}

} // namespace acva::audio
