#include "config/reload.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <utility>

namespace acva::config {

namespace {

enum class Class : std::uint8_t { Hot, Restart };

// Hand-written field catalog. Each entry compares one scalar (or
// container) leaf and, on mismatch, pushes the path string into the
// appropriate diff bucket. Keep this list in sync with the M8A plan §1
// hot-field enumeration; restart-required entries cover the operator-
// reachable surface (endpoints, models, paths, ports, devices).
//
// Why hand-written rather than reflective: Config has nested optionals
// and maps that don't compose cleanly with a generic diff, and the
// hot/restart classification is a policy decision that belongs in
// source rather than declarative metadata.
void diff_one(const Config& cur, const Config& cand,
              ReloadDiff& out) {

    auto check = [&](bool changed, std::string_view path, Class cls) {
        if (!changed) return;
        if (cls == Class::Hot) out.changed_hot.emplace_back(path);
        else                   out.changed_restart.emplace_back(path);
    };

    // ----- Hot-reloadable (M8A §1) -----
    check(cur.llm.temperature                  != cand.llm.temperature,
          "llm.temperature",                  Class::Hot);
    check(cur.llm.max_tokens                   != cand.llm.max_tokens,
          "llm.max_tokens",                   Class::Hot);
    check(cur.dialogue.max_assistant_sentences != cand.dialogue.max_assistant_sentences,
          "dialogue.max_assistant_sentences", Class::Hot);
    check(cur.dialogue.max_tts_queue_sentences != cand.dialogue.max_tts_queue_sentences,
          "dialogue.max_tts_queue_sentences", Class::Hot);
    check(cur.vad.onset_threshold              != cand.vad.onset_threshold,
          "vad.onset_threshold",              Class::Hot);
    check(cur.vad.offset_threshold             != cand.vad.offset_threshold,
          "vad.offset_threshold",             Class::Hot);
    check(cur.vad.hangover_ms                  != cand.vad.hangover_ms,
          "vad.hangover_ms",                  Class::Hot);
    check(cur.tts.tempo_wpm                    != cand.tts.tempo_wpm,
          "tts.tempo_wpm",                    Class::Hot);
    check(cur.logging.level                    != cand.logging.level,
          "logging.level",                    Class::Hot);

    // ----- Restart-required -----
    // Endpoints + model identities — picking up a new URL or model id
    // mid-flight requires reconnecting clients that hold long-lived
    // sockets or per-request state, so we refuse the reload.
    check(cur.llm.base_url      != cand.llm.base_url,      "llm.base_url",      Class::Restart);
    check(cur.llm.model         != cand.llm.model,         "llm.model",         Class::Restart);
    check(cur.stt.base_url      != cand.stt.base_url,      "stt.base_url",      Class::Restart);
    check(cur.stt.model         != cand.stt.model,         "stt.model",         Class::Restart);
    check(cur.stt.streaming     != cand.stt.streaming,     "stt.streaming",     Class::Restart);
    check(cur.stt.language      != cand.stt.language,      "stt.language",      Class::Restart);
    check(cur.tts.base_url      != cand.tts.base_url,      "tts.base_url",      Class::Restart);
    check(cur.tts.voices        != cand.tts.voices,        "tts.voices",        Class::Restart);
    check(cur.tts.fallback_lang != cand.tts.fallback_lang, "tts.fallback_lang", Class::Restart);

    // Filesystem + DB paths.
    check(cur.memory.db_path != cand.memory.db_path, "memory.db_path", Class::Restart);
    check(cur.vad.model_path != cand.vad.model_path, "vad.model_path", Class::Restart);

    // Audio device + pipeline graph.
    check(cur.audio.input_device     != cand.audio.input_device,     "audio.input_device",     Class::Restart);
    check(cur.audio.output_device    != cand.audio.output_device,    "audio.output_device",    Class::Restart);
    check(cur.audio.capture_enabled  != cand.audio.capture_enabled,  "audio.capture_enabled",  Class::Restart);
    check(cur.audio.sample_rate_hz   != cand.audio.sample_rate_hz,   "audio.sample_rate_hz",   Class::Restart);
    check(cur.audio.buffer_frames    != cand.audio.buffer_frames,    "audio.buffer_frames",    Class::Restart);

    // Control plane — the listening port can't be moved without
    // reopening the socket, and the bind address change is similar.
    check(cur.control.bind != cand.control.bind, "control.bind", Class::Restart);
    check(cur.control.port != cand.control.port, "control.port", Class::Restart);

    // Personality switching changes the system prompt + voice +
    // sampling shape mid-conversation, which the operator would expect
    // to take effect on the next turn — but it's an overlay applied at
    // config load (config::resolve_aliases). Re-applying that overlay
    // safely without an orchestrator-wide restart is M8A Step 5
    // territory; for now, treat it as restart-required.
    check(cur.active_personality != cand.active_personality,
          "active_personality", Class::Restart);

    // Logging sink topology (level is hot, the rest aren't).
    check(cur.logging.sink      != cand.logging.sink,      "logging.sink",      Class::Restart);
    check(cur.logging.file_path != cand.logging.file_path, "logging.file_path", Class::Restart);
    check(cur.logging.dir_path  != cand.logging.dir_path,  "logging.dir_path",  Class::Restart);
}

// Deduplicate while preserving first-seen order. The catalog above
// doesn't currently emit duplicates but a future refactor might, and
// log/HTTP output is friendlier without them.
void dedupe(std::vector<std::string>& v) {
    std::vector<std::string> out;
    out.reserve(v.size());
    for (auto& s : v) {
        bool seen = false;
        for (const auto& o : out) {
            if (o == s) { seen = true; break; }
        }
        if (!seen) out.push_back(std::move(s));
    }
    v = std::move(out);
}

} // namespace

ReloadDiff diff_configs(const Config& current, const Config& candidate) {
    ReloadDiff d;
    diff_one(current, candidate, d);
    dedupe(d.changed_hot);
    dedupe(d.changed_restart);
    return d;
}

void apply_hot_fields(Config& live,
                      const Config& candidate,
                      const ReloadDiff& diff) {
    for (const auto& f : diff.changed_hot) {
        if      (f == "llm.temperature")                  live.llm.temperature                  = candidate.llm.temperature;
        else if (f == "llm.max_tokens")                   live.llm.max_tokens                   = candidate.llm.max_tokens;
        else if (f == "dialogue.max_assistant_sentences") live.dialogue.max_assistant_sentences = candidate.dialogue.max_assistant_sentences;
        else if (f == "dialogue.max_tts_queue_sentences") live.dialogue.max_tts_queue_sentences = candidate.dialogue.max_tts_queue_sentences;
        else if (f == "vad.onset_threshold")              live.vad.onset_threshold              = candidate.vad.onset_threshold;
        else if (f == "vad.offset_threshold")             live.vad.offset_threshold             = candidate.vad.offset_threshold;
        else if (f == "vad.hangover_ms")                  live.vad.hangover_ms                  = candidate.vad.hangover_ms;
        else if (f == "tts.tempo_wpm")                    live.tts.tempo_wpm                    = candidate.tts.tempo_wpm;
        else if (f == "logging.level")                    live.logging.level                    = candidate.logging.level;
        // Unknown name in changed_hot would be a bug in diff_configs;
        // silently ignore here since the catalog above is the one
        // truth-source.
    }
}

ConfigReloader::ConfigReloader(Config& live, std::filesystem::path config_path)
    : live_(&live), path_(std::move(config_path)) {}

void ConfigReloader::register_callback(std::string label, ReloadCallback cb) {
    callbacks_.push_back({std::move(label), std::move(cb)});
}

ReloadResult ConfigReloader::reload() {
    auto loaded = load_from_file(path_);
    if (auto* err = std::get_if<LoadError>(&loaded)) {
        return ReloadParseError{err->message};
    }
    auto candidate = std::get<Config>(std::move(loaded));

    if (auto v = validate(candidate); v.has_value()) {
        return ReloadParseError{v->message};
    }

    auto d = diff_configs(*live_, candidate);
    if (!d.ok_to_apply()) {
        return ReloadRejected{std::move(d)};
    }

    apply_hot_fields(*live_, candidate, d);

    for (const auto& e : callbacks_) {
        try {
            e.cb(*live_, d);
        } catch (const std::exception& ex) {
            log::warn("config",
                fmt::format("reload callback '{}' threw: {}", e.label, ex.what()));
        } catch (...) {
            log::warn("config",
                fmt::format("reload callback '{}' threw a non-std exception", e.label));
        }
    }

    log::info("config", fmt::format(
        "reloaded {} hot field(s); 0 restart-required",
        d.changed_hot.size()));

    return ReloadOk{std::move(d)};
}

} // namespace acva::config
