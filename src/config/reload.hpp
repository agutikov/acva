#pragma once

#include "config/config.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace acva::config {

// M8A Step 1 — config hot-reload.
//
// `diff_configs` is a pure function: compare a "current" Config against
// a "candidate" Config and classify every observable scalar change as
// either hot-reloadable (safe to apply at runtime) or restart-required.
// The catalog is hand-written; fields not listed are silently permitted
// to differ. Documented limitation: a v1 reload only acts on the
// fields enumerated below, so operators tweaking less-common fields
// (e.g. `supervisor.probe_timeout_ms`) need to restart for them to
// take effect. The hot list mirrors the M8A milestone plan §1.

struct ReloadDiff {
    std::vector<std::string> changed_hot;
    std::vector<std::string> changed_restart;

    [[nodiscard]] bool empty() const noexcept {
        return changed_hot.empty() && changed_restart.empty();
    }
    // Whole reload is acceptable iff no restart-required field changed.
    [[nodiscard]] bool ok_to_apply() const noexcept {
        return changed_restart.empty();
    }
};

[[nodiscard]] ReloadDiff diff_configs(const Config& current,
                                      const Config& candidate);

// Apply each hot field that the diff reported as changed by copying
// the value from `candidate` into `live`. Untouched fields are not
// rewritten, so subsequent restart-required diffs against `live` keep
// reporting the original mismatch. Caller must guarantee that the diff
// was produced from these two Configs and that ok_to_apply() is true.
void apply_hot_fields(Config& live,
                      const Config& candidate,
                      const ReloadDiff& diff);

// Reload-time callback. Invoked once per /reload after `apply_hot_fields`
// has mutated the live Config. Implementations push hot values into
// components that hold a private copy (Endpointer, log level) — most
// other components reach into `live` via const reference and pick up
// the change without further work.
using ReloadCallback = std::function<void(const Config& live,
                                          const ReloadDiff& diff)>;

// Outcome of a single ConfigReloader::reload() call.
struct ReloadOk {
    ReloadDiff diff;     // changed_hot is what we applied; changed_restart is empty
};
struct ReloadRejected {
    ReloadDiff diff;     // changed_restart is what blocked the reload
};
struct ReloadParseError {
    std::string message; // what config::load_from_file or validate returned
};
using ReloadResult = std::variant<ReloadOk, ReloadRejected, ReloadParseError>;

// Orchestrates a /reload: parse from disk, diff against `live`, and
// either apply (hot only) or reject (restart-required). Holds a
// non-owning reference to the live Config — the caller's main owns it.
class ConfigReloader {
public:
    ConfigReloader(Config& live, std::filesystem::path config_path);

    // Replaces the on-disk config path the next reload() will read.
    void set_config_path(std::filesystem::path p) { path_ = std::move(p); }
    [[nodiscard]] const std::filesystem::path& config_path() const noexcept { return path_; }

    // Register a hot-reload callback. Each registered callback runs in
    // registration order on a successful reload. `label` is a short
    // identifier used in log lines (e.g. "log", "endpointer").
    void register_callback(std::string label, ReloadCallback cb);

    // Read the file, diff, and apply hot fields if all changes are
    // hot-reloadable. Returns the structured result so callers
    // (HTTP handler, SIGHUP path) can shape their response.
    [[nodiscard]] ReloadResult reload();

private:
    Config* live_;
    std::filesystem::path path_;
    struct Entry {
        std::string label;
        ReloadCallback cb;
    };
    std::vector<Entry> callbacks_;
};

} // namespace acva::config
