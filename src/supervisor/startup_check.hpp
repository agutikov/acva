#pragma once

#include "config/config.hpp"

#include <string>
#include <vector>

namespace acva::supervisor {

// M8A Step 5 — boot-time gates. Each entry in the returned vector
// corresponds to one failed check; the strings are user-facing
// (component, error, remediation hint). Empty vector = all checks
// passed. The caller decides whether failures are fatal based on
// `cfg.supervisor.strict_startup`.
//
// Checks that depend on configured state (e.g. STT base_url empty)
// are skipped; they neither pass nor fail. The pure-pass case
// returns an empty vector regardless of which gates ran.

struct StartupFailure {
    std::string component;     // "llm", "stt", "tts", "capture"
    std::string error;         // raw error
    std::string remediation;   // a hint the operator can act on
};

// Force-load each configured backend (LLM / STT / TTS) by issuing a
// minimal round-trip that pulls the model into VRAM, then run the
// capture-readiness probe (when `cfg.audio.capture_enabled`).
//
// Skipped entirely when `cfg.supervisor.startup_force_load` is false
// — the caller still gets an empty vector, indicating "no checks ran".
[[nodiscard]] std::vector<StartupFailure>
run_startup_checks(const config::Config& cfg);

} // namespace acva::supervisor
