#pragma once

#include "config/config.hpp"
#include "dialogue/session.hpp"
#include "memory/memory_thread.hpp"

#include <string>

namespace acva::orchestrator {

// M8A Step 4 — warm-restart resume gate. Reads the runtime_state
// singleton row; if its age is within
// `cfg.supervisor.checkpoint_max_age_seconds` AND its `config_hash`
// matches the freshly-loaded config, calls `sessions.adopt(...)` and
// returns true. Otherwise (no row, stale, hash mismatch, adopt
// failure) clears the row and returns false; the caller should
// then call `sessions.open_initial()` to mint a cold session.
//
// The row is cleared in BOTH the adopt-success and discard cases so
// a subsequent crash-before-checkpoint doesn't re-adopt the same
// state on next startup.
[[nodiscard]] bool try_warm_resume(memory::MemoryThread& memory,
                                    dialogue::SessionManager& sessions,
                                    const config::Config& cfg,
                                    const std::string& config_hash);

} // namespace acva::orchestrator
