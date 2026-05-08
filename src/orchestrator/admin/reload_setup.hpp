#pragma once

#include "audio/endpointer.hpp"
#include "audio/wake_word.hpp"
#include "config/config.hpp"
#include "config/reload.hpp"

#include <filesystem>
#include <functional>
#include <mutex>

namespace acva::orchestrator {

// M8A Step 1 — owns the ConfigReloader plus the synchronisation that
// serialises HTTP-driven reloads against SIGHUP-driven ones.
//
// Constructed early in main() with a non-owning ref to the live
// Config; the log callback registers immediately, the endpointer
// callback registers later (post-capture-build) when the
// AudioPipeline's Endpointer pointer is known. Use the `run_reload`
// closure as the ControlServer's RestartHandler — it grabs the
// internal mutex before delegating to ConfigReloader::reload().
class ReloadSetup {
public:
    ReloadSetup(config::Config& live, std::filesystem::path config_path);

    // Convenience: a copyable closure that locks the internal mutex
    // and calls `reloader.reload()`. Hand straight to ControlServer
    // and to the SIGHUP drain in the main loop.
    using RunFn = std::function<config::ReloadResult()>;
    [[nodiscard]] RunFn run_reload();

    // Register the log-level reload callback. Must be called once.
    void register_log_callback();

    // Register the VAD-thresholds reload callback for the given
    // Endpointer. No-op when ep is nullptr (capture disabled).
    void register_endpointer_callback(audio::Endpointer* ep);

    // M8C Step 1 follow-up — register the wake-word threshold
    // reload callback for the given engine. No-op when ww is
    // nullptr (capture disabled).
    void register_wake_word_callback(audio::WakeWord* ww);

private:
    config::ConfigReloader reloader_;
    std::mutex mtx_;
};

} // namespace acva::orchestrator
