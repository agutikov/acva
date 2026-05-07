#pragma once

#include "config/config.hpp"

namespace acva::orchestrator {

// M8A Step 5 — runs the boot-time gates (force-load each backend
// + capture-readiness probe via supervisor::run_startup_checks),
// logs each failure with a remediation hint, and classifies the
// overall outcome:
//
//   - returns true when running under `cfg.supervisor.strict_startup`
//     AND at least one gate failed. The caller should skip the main
//     loop, run the orderly shutdown chain, and exit non-zero.
//   - returns false otherwise (no failures, or failures but tolerant
//     mode — log + continue).
//
// Skipped (returns false, no logging) when
// `cfg.supervisor.startup_force_load` is false; the gate runner
// itself short-circuits in that case.
[[nodiscard]] bool run_startup_pass(const config::Config& cfg);

} // namespace acva::orchestrator
