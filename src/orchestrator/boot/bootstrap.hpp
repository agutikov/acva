#pragma once

#include "config/config.hpp"
#include "metrics/registry.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

// orchestrator:: — host-side glue that wires the per-subsystem
// modules (audio/, dialogue/, stt/, ...) into a running orchestrator.
// Each builder returns a struct holding the constructed objects with
// RAII teardown so main.cpp can stay thin.
namespace acva::orchestrator {

// Resolve `--config PATH` into an absolute filesystem path, then load
// + post-process the YAML into a Config. Side effects:
//   * cfg.memory.db_path → resolved against XDG_DATA_HOME.
//   * cfg.vad.model_path → resolved against XDG_DATA_HOME if non-empty
//     OR auto-detected at the default Silero path if empty + present.
//   * cfg.pipeline.fake_driver_enabled → forced false in stdin mode.
//
// On any failure (bad path, parse error, validation failure) returns
// the LoadError message; main.cpp prints it to stderr and exits.
struct LoadedConfig {
    config::Config        cfg;
    std::filesystem::path config_path;
};
[[nodiscard]] std::variant<LoadedConfig, config::LoadError>
load_and_resolve_config(const std::filesystem::path& cli_config_path,
                         bool stdin_mode);

// Sidestep PortAudio's full ALSA-PCM probe by writing a minimal
// asound.conf to a tmpfile and pointing ALSA_CONFIG_PATH at it.
// Stops alsa-pipewire-jack glue from deadlocking pipewire's
// thread-loop (~4 minute startup stall). No-op when
// cfg.skip_alsa_full_probe is false. Logs the action via log::info /
// log::warn — call AFTER log::init.
void install_alsa_sidestep(const config::AudioConfig& audio);

// M8B Step 1 — current Speaches wedge state, refreshed on each
// VramMonitor tick. Read by /status to surface a remediation hint.
struct SpeachesWedgeState {
    bool          known     = false;  // false until the first probe runs
    int           pid       = -1;
    std::int64_t  used_mib  = 0;
    bool          wedged    = false;
    std::int64_t  threshold_mib = 0;
};

// RAII periodic VRAM probe. Spawns a worker that shells nvidia-smi
// every cfg.vram_monitor_interval_ms. Two responsibilities:
//
//  1. Total-VRAM edge-triggered logging: emits `vram_low` / `vram_recovered`
//     when free VRAM crosses cfg.logging.vram_low_threshold_mib.
//  2. M8B Step 1 — Speaches CUDA-OOM wedge detection: per-process
//     nvidia-smi + /proc/<pid>/cmdline scan to identify the speaches
//     process; pushes voice_speaches_vram_used_mib +
//     voice_speaches_wedged metrics; updates `wedge_state()` for the
//     /status closure to read.
//
// `registry` and `supervisor_cfg` may be null/default — the wedge
// detection short-circuits in that case (kept optional so existing
// code paths and tests don't need to wire metrics). Stop via the
// destructor; safe to construct with interval = 0 (disabled —
// destructor is a no-op).
class VramMonitor {
public:
    VramMonitor(const config::LoggingConfig& logging,
                const config::SupervisorConfig& supervisor_cfg,
                std::shared_ptr<metrics::Registry> registry);

    // Convenience overload retained for existing call sites that
    // don't have a registry handy (tests + the M0-era demos).
    explicit VramMonitor(const config::LoggingConfig& logging);

    ~VramMonitor();

    VramMonitor(const VramMonitor&)            = delete;
    VramMonitor& operator=(const VramMonitor&) = delete;
    VramMonitor(VramMonitor&&)                 = delete;
    VramMonitor& operator=(VramMonitor&&)      = delete;

    // Snapshot of the most-recent wedge probe, used by /status.
    [[nodiscard]] SpeachesWedgeState wedge_state() const;

private:
    std::atomic<bool> stop_{false};
    std::thread       thread_;

    // Last wedge probe — guarded by `wedge_mu_`; copied out by
    // wedge_state() on demand.
    mutable std::mutex   wedge_mu_;
    SpeachesWedgeState   wedge_{};
};

} // namespace acva::orchestrator
