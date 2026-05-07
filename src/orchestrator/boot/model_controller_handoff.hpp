#pragma once

#include "config/config.hpp"

namespace acva::orchestrator {

// M8A Step 5 — pre-runtime hand-off to the model-controller sidecar.
//
// When `cfg.llm.model_controller_url` and `cfg.llm.model_file` are
// both non-empty, contacts the controller at
// `cfg.llm.model_controller_url`, compares the currently-loaded GGUF
// against `cfg.llm.model_file`, and (if they differ) issues
// `POST /llm/load`. Blocks until the sidecar reports the swap done
// or the deadline (60 s) elapses.
//
// Returns:
//   - true on success or skip (no-op when the controller URL or model
//     filename is empty, or when the controller already serves the
//     requested file).
//   - false ONLY under `cfg.supervisor.strict_startup` AND when the
//     controller call failed (network error, HTTP 5xx, or load
//     timeout). The caller exits non-zero on false; in tolerant mode
//     this function logs a warning and still returns true so the
//     orchestrator continues with whatever model llama already had.
[[nodiscard]] bool run_model_controller_handoff(const config::Config& cfg);

} // namespace acva::orchestrator
