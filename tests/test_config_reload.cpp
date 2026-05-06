#include "config/config.hpp"
#include "config/reload.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

using acva::config::Config;
using acva::config::ConfigReloader;
using acva::config::ReloadDiff;
using acva::config::ReloadOk;
using acva::config::ReloadParseError;
using acva::config::ReloadRejected;
using acva::config::ReloadResult;
using acva::config::apply_hot_fields;
using acva::config::diff_configs;
using acva::config::load_from_string;

namespace {

Config parse_or_die(const std::string& yaml) {
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    return std::get<Config>(std::move(r));
}

// Minimal config text; tweak the hot/restart fields below to drive the diff.
std::string base_yaml() {
    return R"(
logging:
  level: info
control:
  port: 9876
llm:
  base_url: http://127.0.0.1:8081/v1
  model: dialog
  temperature: 0.7
  max_tokens: 400
dialogue:
  max_assistant_sentences: 6
  max_tts_queue_sentences: 3
vad:
  onset_threshold: 0.5
  offset_threshold: 0.35
  hangover_ms: 600
tts:
  tempo_wpm: 200
)";
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

fs::path fresh_tmp_file(const char* tag) {
    auto p = fs::temp_directory_path()
           / (std::string{"acva-reload-"} + tag + "-"
              + std::to_string(::getpid()) + "-"
              + std::to_string(std::rand()) + ".yaml");
    fs::remove(p);
    return p;
}

void write_file(const fs::path& p, std::string_view content) {
    std::ofstream f(p);
    f << content;
}

} // namespace

TEST_CASE("reload: identical configs produce empty diff") {
    auto a = parse_or_die(base_yaml());
    auto b = parse_or_die(base_yaml());
    auto d = diff_configs(a, b);
    CHECK(d.empty());
    CHECK(d.ok_to_apply());
}

TEST_CASE("reload: each hot field is detected on its own") {
    struct Case { const char* name; const char* yaml_diff; };
    const std::vector<Case> cases{
        {"llm.temperature",                  "llm:\n  temperature: 0.4\n"},
        {"llm.max_tokens",                   "llm:\n  max_tokens: 256\n"},
        {"dialogue.max_assistant_sentences", "dialogue:\n  max_assistant_sentences: 4\n"},
        {"dialogue.max_tts_queue_sentences", "dialogue:\n  max_tts_queue_sentences: 5\n"},
        {"vad.onset_threshold",              "vad:\n  onset_threshold: 0.6\n"},
        {"vad.offset_threshold",             "vad:\n  offset_threshold: 0.25\n"},
        {"vad.hangover_ms",                  "vad:\n  hangover_ms: 800\n"},
        {"tts.tempo_wpm",                    "tts:\n  tempo_wpm: 160\n"},
        {"logging.level",                    "logging:\n  level: debug\n"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto cur = parse_or_die(base_yaml());
        auto cand = parse_or_die(base_yaml() + c.yaml_diff);
        auto d = diff_configs(cur, cand);
        CHECK(contains(d.changed_hot, c.name));
        CHECK(d.changed_restart.empty());
        CHECK(d.ok_to_apply());
    }
}

TEST_CASE("reload: each restart-required field rejects the whole reload") {
    struct Case { const char* name; const char* yaml_diff; };
    const std::vector<Case> cases{
        {"llm.base_url",         "llm:\n  base_url: http://other:9000/v1\n"},
        {"llm.model",            "llm:\n  model: other-model\n"},
        {"memory.db_path",       "memory:\n  db_path: /tmp/other.db\n"},
        {"audio.input_device",   "audio:\n  input_device: foobar\n"},
        {"audio.sample_rate_hz", "audio:\n  sample_rate_hz: 44100\n"},
        {"control.port",         "control:\n  port: 12000\n"},
        {"vad.model_path",       "vad:\n  model_path: /tmp/silero.onnx\n"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto cur = parse_or_die(base_yaml());
        auto cand = parse_or_die(base_yaml() + c.yaml_diff);
        auto d = diff_configs(cur, cand);
        CHECK(contains(d.changed_restart, c.name));
        CHECK(d.changed_hot.empty());
        CHECK_FALSE(d.ok_to_apply());
    }
}

TEST_CASE("reload: mixed hot + restart change rejects whole reload") {
    auto cur = parse_or_die(base_yaml());
    auto cand = parse_or_die(base_yaml()
        + "llm:\n  temperature: 0.2\n"
        + "audio:\n  sample_rate_hz: 44100\n");
    auto d = diff_configs(cur, cand);
    CHECK(contains(d.changed_hot, "llm.temperature"));
    CHECK(contains(d.changed_restart, "audio.sample_rate_hz"));
    CHECK_FALSE(d.ok_to_apply());
}

TEST_CASE("reload: apply_hot_fields copies only the diffed hot fields") {
    auto live = parse_or_die(base_yaml());
    auto cand = parse_or_die(base_yaml()
        + "llm:\n  temperature: 0.2\n"
        + "vad:\n  hangover_ms: 1000\n");
    auto d = diff_configs(live, cand);
    REQUIRE(d.ok_to_apply());

    apply_hot_fields(live, cand, d);
    CHECK(live.llm.temperature == doctest::Approx(0.2));
    CHECK(live.vad.hangover_ms == 1000U);
    // Untouched fields keep their original values.
    CHECK(live.llm.max_tokens == 400U);
    CHECK(live.tts.tempo_wpm == 200U);
}

TEST_CASE("ConfigReloader: happy path applies hot fields and runs callbacks") {
    auto path = fresh_tmp_file("happy");
    write_file(path, base_yaml());

    auto live = parse_or_die(base_yaml());
    ConfigReloader r(live, path);

    int cb_calls = 0;
    std::vector<std::string> cb_seen_hot;
    r.register_callback("test", [&](const Config& live_now,
                                    const ReloadDiff& d) {
        ++cb_calls;
        for (const auto& f : d.changed_hot) cb_seen_hot.push_back(f);
        CHECK(live_now.llm.temperature == doctest::Approx(0.4));
    });

    write_file(path, base_yaml() + "llm:\n  temperature: 0.4\n");
    auto result = r.reload();
    REQUIRE(std::holds_alternative<ReloadOk>(result));
    const auto& ok = std::get<ReloadOk>(result);
    CHECK(contains(ok.diff.changed_hot, "llm.temperature"));
    CHECK(cb_calls == 1);
    CHECK(contains(cb_seen_hot, "llm.temperature"));
    CHECK(live.llm.temperature == doctest::Approx(0.4));

    fs::remove(path);
}

TEST_CASE("ConfigReloader: restart-required field returns ReloadRejected; live stays put") {
    auto path = fresh_tmp_file("rejected");
    write_file(path, base_yaml());

    auto live = parse_or_die(base_yaml());
    ConfigReloader r(live, path);

    int cb_calls = 0;
    r.register_callback("test", [&](const Config&, const ReloadDiff&) { ++cb_calls; });

    write_file(path, base_yaml() + "audio:\n  sample_rate_hz: 44100\n");
    auto result = r.reload();
    REQUIRE(std::holds_alternative<ReloadRejected>(result));
    const auto& rej = std::get<ReloadRejected>(result);
    CHECK(contains(rej.diff.changed_restart, "audio.sample_rate_hz"));
    CHECK(cb_calls == 0);
    // live is untouched.
    CHECK(live.audio.sample_rate_hz == 48000U);

    fs::remove(path);
}

TEST_CASE("ConfigReloader: malformed YAML returns ReloadParseError") {
    auto path = fresh_tmp_file("parse-error");
    write_file(path, base_yaml());

    auto live = parse_or_die(base_yaml());
    ConfigReloader r(live, path);

    int cb_calls = 0;
    r.register_callback("test", [&](const Config&, const ReloadDiff&) { ++cb_calls; });

    // Leave the YAML mid-mapping so glaze fails to parse.
    write_file(path, "logging: { level: info\ncontrol: { port: ");
    auto result = r.reload();
    REQUIRE(std::holds_alternative<ReloadParseError>(result));
    CHECK_FALSE(std::get<ReloadParseError>(result).message.empty());
    CHECK(cb_calls == 0);

    fs::remove(path);
}

TEST_CASE("ConfigReloader: callback exceptions are caught, reload still reports ok") {
    auto path = fresh_tmp_file("cb-throws");
    write_file(path, base_yaml());

    auto live = parse_or_die(base_yaml());
    ConfigReloader r(live, path);

    r.register_callback("throws",
        [](const Config&, const ReloadDiff&) {
            throw std::runtime_error("boom");
        });

    write_file(path, base_yaml() + "tts:\n  tempo_wpm: 160\n");
    auto result = r.reload();
    REQUIRE(std::holds_alternative<ReloadOk>(result));
    CHECK(live.tts.tempo_wpm == 160U);

    fs::remove(path);
}
