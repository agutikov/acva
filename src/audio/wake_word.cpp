#include "audio/wake_word.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <vector>

#ifdef ACVA_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace acva::audio {

#ifdef ACVA_HAVE_ONNXRUNTIME

namespace {

// openWakeWord 3-stage pipeline framing (verified empirically against
// the v0.5.1 model release).
//
//   melspectrogram.onnx
//     in  [batch, samples]               float32 (audio in [-1, 1])
//     out [1, 1, frames, 32]             where frames = (samples − 480) / 160
//                                        i.e. 480-sample window, 160-sample hop
//                                        → 100 fps in theory; we run with
//                                        non-overlapping 1280-sample chunks for
//                                        streaming-stateless simplicity, giving
//                                        5 mel frames per call (62.5 fps).
//
//   embedding_model.onnx
//     in  [batch, 76, 32, 1]             float32 (mel features, normalized)
//     out [batch, 1, 1, 96]              one 96-dim embedding per 76-frame window
//
//   <wake-word>.onnx (e.g. hey_jarvis_v0.1.onnx)
//     in  [1, 16, 96]                    float32
//     out [1, 1]                         confidence in [0, 1]
//
// Mel features need openWakeWord's documented normalization
// (`mel / 10 + 2`) before feeding the embedding model. The classifier
// expects a sliding window of the latest 16 embeddings.
constexpr std::size_t kAudioStep         = 1280;        // 80 ms @ 16 kHz
constexpr std::size_t kMelBins           = 32;
constexpr std::size_t kMelFramesPerStep  = 5;           // (1280 − 480) / 160
constexpr std::size_t kMelFramesNeeded   = 76;          // embedding input window
constexpr std::size_t kEmbeddingDim      = 96;
constexpr std::size_t kClassifierWindow  = 16;          // embeddings per classify

// Locate the shared mel + embedding ONNX files. openWakeWord ships
// them as `melspectrogram.onnx` and `embedding_model.onnx` in the
// same directory as each classifier; `tools/acva-models install
// <wake-alias>` auto-pulls both. Returns the directory if both are
// present alongside `classifier_path`; empty path otherwise.
std::filesystem::path
locate_shared_models_dir(const std::filesystem::path& classifier_path) {
    auto dir = classifier_path.parent_path();
    if (std::filesystem::exists(dir / "melspectrogram.onnx")
        && std::filesystem::exists(dir / "embedding_model.onnx")) {
        return dir;
    }
    return {};
}

struct ClassifierState {
    std::string  alias;                                 // for log lines
    std::unique_ptr<Ort::Session> session;
    // Rolling 16-embedding window. New embeddings push_back; the head
    // pops once the window reaches kClassifierWindow.
    std::deque<std::array<float, kEmbeddingDim>> embeddings;
};

} // namespace

struct WakeWord::Impl {
    Ort::Env       env{ORT_LOGGING_LEVEL_WARNING, "acva-wake-word"};
    Ort::SessionOptions opts;

    // Shared backbone — first classifier's parent dir is the source.
    // Subsequent classifiers must live alongside the same melspec /
    // embedding files; reusing the loaded sessions saves VRAM/RAM.
    std::unique_ptr<Ort::Session> melspectrogram;
    std::unique_ptr<Ort::Session> embedding;

    std::vector<ClassifierState> classifiers;

    // Streaming buffers — shared across classifiers.
    std::vector<float>           audio_buf;             // accumulating int16→float32 input
    std::deque<std::array<float, kMelBins>> mel_buf;    // rolling window of normalized mel frames

    Impl() {
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    }

    [[nodiscard]] bool ready() const noexcept {
        return melspectrogram && embedding && !classifiers.empty();
    }

    void run_mel_step(std::span<const float> audio_chunk);
    std::array<float, kEmbeddingDim> run_embedding();
    static float run_classifier(ClassifierState& state);
    float run_inference_step(std::span<const float> audio_chunk);
};

#else  // stub path

struct WakeWord::Impl {};

#endif

WakeWord::WakeWord(const config::WakeWordConfig& cfg)
    : cfg_(cfg),
      threshold_(cfg.threshold),
      impl_(std::make_unique<Impl>()) {

    if (!cfg_.enabled) {
        return;
    }

#ifdef ACVA_HAVE_ONNXRUNTIME
    Ort::AllocatorWithDefaultOptions default_alloc;

    for (const auto& raw : cfg_.model_paths) {
        std::filesystem::path p(raw);
        if (!std::filesystem::exists(p)) {
            log::warn("wake_word", fmt::format(
                "model not found: {} — skipping", raw));
            continue;
        }

        // Lazy-load the shared mel + embedding sessions on the first
        // classifier we successfully open. They live in the same
        // directory as each `_v*.onnx` classifier head.
        if (!impl_->melspectrogram || !impl_->embedding) {
            const auto dir = locate_shared_models_dir(p);
            if (dir.empty()) {
                log::warn("wake_word", fmt::format(
                    "{}: missing melspectrogram.onnx or "
                    "embedding_model.onnx alongside the classifier — "
                    "run `tools/acva-models install {}`",
                    raw, p.stem().string()));
                continue;
            }
            try {
                if (!impl_->melspectrogram) {
                    impl_->melspectrogram = std::make_unique<Ort::Session>(
                        impl_->env, (dir / "melspectrogram.onnx").c_str(),
                        impl_->opts);
                }
                if (!impl_->embedding) {
                    impl_->embedding = std::make_unique<Ort::Session>(
                        impl_->env, (dir / "embedding_model.onnx").c_str(),
                        impl_->opts);
                }
            } catch (const Ort::Exception& ex) {
                log::warn("wake_word", fmt::format(
                    "failed to load shared infra graphs from {}: {}",
                    dir.string(), ex.what()));
                continue;
            }
        }

        try {
            ClassifierState st;
            st.alias = p.stem().string();
            st.session = std::make_unique<Ort::Session>(
                impl_->env, p.c_str(), impl_->opts);
            impl_->classifiers.push_back(std::move(st));
            log::info("wake_word", fmt::format(
                "loaded classifier: {}", raw));
        } catch (const Ort::Exception& ex) {
            log::warn("wake_word", fmt::format(
                "failed to load {}: {}", raw, ex.what()));
        }
    }

    if (!impl_->ready()) {
        log::warn("wake_word",
            "wake_word.enabled=true but the inference pipeline didn't "
            "come up — gate will behave as if wake-word is disabled");
    } else {
        log::info("wake_word", fmt::format(
            "{} classifier(s) loaded; threshold={:.2f} window_ms={}",
            impl_->classifiers.size(), cfg_.threshold,
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
    return impl_ && impl_->ready();
#else
    return false;
#endif
}

#ifdef ACVA_HAVE_ONNXRUNTIME

// Run the mel preprocessor over the buffered audio chunk and append
// the resulting frames (with openWakeWord's `/10 + 2` normalization)
// to the rolling mel ring.
void WakeWord::Impl::run_mel_step(std::span<const float> audio_chunk) {
    const std::array<std::int64_t, 2> in_shape = {
        1, static_cast<std::int64_t>(audio_chunk.size())};
    auto memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);
    auto input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        const_cast<float*>(audio_chunk.data()),
        audio_chunk.size(),
        in_shape.data(),
        in_shape.size());

    const char* in_names[]  = {"input"};
    const char* out_names[] = {"output"};
    auto out = melspectrogram->Run(
        Ort::RunOptions{},
        in_names, &input_tensor, 1,
        out_names, 1);

    // out[0] shape: [1, 1, frames, 32]
    auto info = out[0].GetTensorTypeAndShapeInfo();
    auto shape = info.GetShape();
    const std::size_t frames = (shape.size() == 4)
        ? static_cast<std::size_t>(shape[2])
        : 0;
    const float* data = out[0].GetTensorData<float>();

    for (std::size_t f = 0; f < frames; ++f) {
        std::array<float, kMelBins> frame{};
        for (std::size_t b = 0; b < kMelBins; ++b) {
            // openWakeWord normalization: raw_mel / 10 + 2.
            frame[b] = data[f * kMelBins + b] / 10.0F + 2.0F;
        }
        mel_buf.push_back(frame);
    }

    while (mel_buf.size() > kMelFramesNeeded + 8) {
        mel_buf.pop_front();
    }
}

// Compute one fresh embedding from the latest 76 mel frames.
std::array<float, kEmbeddingDim> WakeWord::Impl::run_embedding() {
    std::array<float, kMelFramesNeeded * kMelBins> emb_in{};
    const std::size_t base = mel_buf.size() - kMelFramesNeeded;
    for (std::size_t f = 0; f < kMelFramesNeeded; ++f) {
        const auto& frame = mel_buf[base + f];
        std::memcpy(&emb_in[f * kMelBins], frame.data(),
                    kMelBins * sizeof(float));
    }

    const std::array<std::int64_t, 4> in_shape = {
        1,
        static_cast<std::int64_t>(kMelFramesNeeded),
        static_cast<std::int64_t>(kMelBins),
        1};
    auto memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);
    auto in_tensor = Ort::Value::CreateTensor<float>(
        memory_info, emb_in.data(), emb_in.size(),
        in_shape.data(), in_shape.size());

    const char* in_names[]  = {"input_1"};
    const char* out_names[] = {"conv2d_19"};
    auto out = embedding->Run(
        Ort::RunOptions{},
        in_names, &in_tensor, 1,
        out_names, 1);

    std::array<float, kEmbeddingDim> result{};
    std::memcpy(result.data(),
                out[0].GetTensorData<float>(),
                kEmbeddingDim * sizeof(float));
    return result;
}

float WakeWord::Impl::run_classifier(ClassifierState& state) {
    if (state.embeddings.size() < kClassifierWindow) return 0.0F;

    std::array<float, kClassifierWindow * kEmbeddingDim> cls_in{};
    for (std::size_t i = 0; i < kClassifierWindow; ++i) {
        std::memcpy(&cls_in[i * kEmbeddingDim],
                    state.embeddings[i].data(),
                    kEmbeddingDim * sizeof(float));
    }

    const std::array<std::int64_t, 3> in_shape = {
        1,
        static_cast<std::int64_t>(kClassifierWindow),
        static_cast<std::int64_t>(kEmbeddingDim)};
    auto memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);
    auto in_tensor = Ort::Value::CreateTensor<float>(
        memory_info, cls_in.data(), cls_in.size(),
        in_shape.data(), in_shape.size());

    const char* in_names[]  = {"x.1"};
    const char* out_names[] = {"53"};
    auto out = state.session->Run(
        Ort::RunOptions{},
        in_names, &in_tensor, 1,
        out_names, 1);

    return out[0].GetTensorData<float>()[0];
}

float WakeWord::Impl::run_inference_step(std::span<const float> audio_chunk) {
    run_mel_step(audio_chunk);
    if (mel_buf.size() < kMelFramesNeeded) return 0.0F;

    const auto fresh = run_embedding();

    float top_score = 0.0F;
    for (auto& cls : classifiers) {
        cls.embeddings.push_back(fresh);
        while (cls.embeddings.size() > kClassifierWindow) {
            cls.embeddings.pop_front();
        }
        if (cls.embeddings.size() < kClassifierWindow) continue;
        const float score = run_classifier(cls);
        if (score > top_score) top_score = score;
    }
    return top_score;
}

#endif  // ACVA_HAVE_ONNXRUNTIME

float WakeWord::push_frame(std::span<const std::int16_t> samples) {
    float score = 0.0F;
    if (test_score_ >= 0.0F) {
        score = test_score_;
    } else if (cfg_.enabled && loaded()) {
#ifdef ACVA_HAVE_ONNXRUNTIME
        auto& I = *impl_;
        // int16 → float32 in [-1, 1].
        I.audio_buf.reserve(I.audio_buf.size() + samples.size());
        for (auto s : samples) {
            I.audio_buf.push_back(static_cast<float>(s) / 32768.0F);
        }
        // Drain in non-overlapping 1280-sample steps. Each step
        // produces 5 mel frames + (when warm) 1 fresh embedding.
        // We track the highest score seen across this push so the
        // gate sees the freshest signal even if `samples` was big
        // enough for multiple steps.
        while (I.audio_buf.size() >= kAudioStep) {
            std::span<const float> chunk(I.audio_buf.data(), kAudioStep);
            const float s = I.run_inference_step(chunk);
            if (s > score) score = s;
            I.audio_buf.erase(I.audio_buf.begin(),
                              I.audio_buf.begin() + kAudioStep);
        }
#endif
    }

    last_score_.store(score, std::memory_order_release);
    if (score >= threshold_.load(std::memory_order_acquire)) {
        detections_total_.fetch_add(1, std::memory_order_relaxed);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_detection_ns_.store(ns, std::memory_order_release);
    }
    return score;
}

std::size_t WakeWord::model_count() const noexcept {
#ifdef ACVA_HAVE_ONNXRUNTIME
    return impl_ ? impl_->classifiers.size() : 0;
#else
    return 0;
#endif
}

} // namespace acva::audio
