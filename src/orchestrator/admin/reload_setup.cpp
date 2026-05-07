#include "orchestrator/admin/reload_setup.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>
#include <utility>

namespace acva::orchestrator {

ReloadSetup::ReloadSetup(config::Config& live,
                         std::filesystem::path config_path)
    : reloader_(live, std::move(config_path)) {}

ReloadSetup::RunFn ReloadSetup::run_reload() {
    return [this]() {
        std::lock_guard lk(mtx_);
        return reloader_.reload();
    };
}

void ReloadSetup::register_log_callback() {
    reloader_.register_callback("log",
        [](const config::Config& live,
           const config::ReloadDiff& diff) {
            for (const auto& f : diff.changed_hot) {
                if (f == "logging.level") {
                    log::set_level(live.logging.level);
                    log::info("config", fmt::format(
                        "logging.level → {}", live.logging.level));
                }
            }
        });
}

void ReloadSetup::register_endpointer_callback(audio::Endpointer* ep) {
    if (ep == nullptr) return;
    reloader_.register_callback("endpointer",
        [ep](const config::Config& live,
             const config::ReloadDiff& diff) {
            bool any = false;
            for (const auto& f : diff.changed_hot) {
                if (f == "vad.onset_threshold"
                    || f == "vad.offset_threshold"
                    || f == "vad.hangover_ms") {
                    any = true;
                    break;
                }
            }
            if (!any) return;
            audio::EndpointerConfig ec;
            ec.onset_threshold  = live.vad.onset_threshold;
            ec.offset_threshold = live.vad.offset_threshold;
            ec.hangover_ms      = std::chrono::milliseconds(live.vad.hangover_ms);
            ep->update_thresholds(ec);
            log::info("config", fmt::format(
                "vad thresholds → onset={:.3f} offset={:.3f} hangover_ms={}",
                live.vad.onset_threshold,
                live.vad.offset_threshold,
                live.vad.hangover_ms));
        });
}

} // namespace acva::orchestrator
