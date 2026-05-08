// M8C Step 1 Tier 2 — live ONNX smoke test for the wake-word
// 3-stage inference pipeline.
//
// Resolves models the same way main.cpp does:
//   ${XDG_DATA_HOME:-$HOME/.local/share}/acva/models/wake_word/
//   {hey_jarvis_v0.1.onnx, melspectrogram.onnx, embedding_model.onnx}
//
// `tools/acva-models install hey-jarvis` puts them there. When the
// classifier asset is missing the test skips cleanly — the
// integration suite contract.

#include "audio/wake_word.hpp"
#include "config/config.hpp"
#include "config/paths.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <vector>

using acva::audio::WakeWord;

namespace {

std::filesystem::path classifier_path_or_empty() {
    auto p = acva::config::resolve_data_path(
        "", "models/wake_word/hey_jarvis_v0.1.onnx");
    return std::filesystem::exists(p) ? p : std::filesystem::path{};
}

acva::config::WakeWordConfig wake_cfg() {
    acva::config::WakeWordConfig c;
    c.enabled    = true;
    c.threshold  = 0.6F;
    c.followup_window_ms = 8000;
    c.model_paths = {classifier_path_or_empty().string()};
    return c;
}

std::vector<std::int16_t> white_noise(std::size_t n, std::uint32_t seed = 42) {
    std::vector<std::int16_t> out(n);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(-3000, 3000);  // ~-20 dBFS
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::int16_t>(dist(rng));
    }
    return out;
}

std::vector<std::int16_t> silence(std::size_t n) {
    return std::vector<std::int16_t>(n, 0);
}

} // namespace

TEST_CASE("WakeWord: real ONNX pipeline loads hey-jarvis + shared infra"
          * doctest::skip(classifier_path_or_empty().empty())) {
    WakeWord ww(wake_cfg());
    REQUIRE(ww.loaded());
    CHECK(ww.model_count() == 1);
}

TEST_CASE("WakeWord: white noise produces valid scores in [0, 1] "
          "and does not falsely trigger"
          * doctest::skip(classifier_path_or_empty().empty())) {
    WakeWord ww(wake_cfg());
    REQUIRE(ww.loaded());

    // 4 s of noise at 16 kHz, fed in 16 ms (256-sample) chunks to
    // mimic the live pipeline cadence. The classifier window needs
    // 76 mel frames (≈1.5 s with our 5-frames-per-chunk cadence) +
    // 16 embeddings (≈1.3 s further) = ~2.8 s of warm-up before
    // the first score, so 2 s is too short to even start.
    const auto buf = white_noise(64000);
    constexpr std::size_t step = 256;

    float top = 0.0F;
    bool  positive_seen = false;
    for (std::size_t i = 0; i + step <= buf.size(); i += step) {
        const float s = ww.push_frame(
            {buf.data() + i, step});
        CHECK(s >= 0.0F);
        CHECK(s <= 1.0F);
        if (s > top) top = s;
        if (s > 0.0F) positive_seen = true;
    }
    // After warm-up the classifier must produce a real score, not
    // just zeros. White noise should NOT cross the 0.6 threshold —
    // if it does, either the model file is corrupt or our
    // normalization is wrong.
    CHECK(positive_seen);
    CHECK(top < 0.6F);
}

TEST_CASE("WakeWord: pure silence stays well below threshold"
          * doctest::skip(classifier_path_or_empty().empty())) {
    WakeWord ww(wake_cfg());
    REQUIRE(ww.loaded());

    const auto buf = silence(64000);
    constexpr std::size_t step = 256;

    float top = 0.0F;
    for (std::size_t i = 0; i + step <= buf.size(); i += step) {
        const float s = ww.push_frame(
            {buf.data() + i, step});
        CHECK(s >= 0.0F);
        CHECK(s <= 1.0F);
        if (s > top) top = s;
    }
    CHECK(top < 0.3F);
}

TEST_CASE("WakeWord: last_score reflects most recent inference"
          * doctest::skip(classifier_path_or_empty().empty())) {
    WakeWord ww(wake_cfg());
    REQUIRE(ww.loaded());

    const auto buf = white_noise(64000, 7);
    constexpr std::size_t step = 1280;  // matches kAudioStep — one mel run / call

    float final_score = 0.0F;
    for (std::size_t i = 0; i + step <= buf.size(); i += step) {
        final_score = ww.push_frame(
            {buf.data() + i, step});
    }
    CHECK(ww.last_score() == doctest::Approx(static_cast<double>(final_score)));
}
