// M8B Step 4 — precompiled header for the test suites.
//
// Wired via `target_precompile_headers` in tests/CMakeLists.txt for
// both `acva_unit_tests` and `acva_integration_tests`. Compiling
// this once and reusing the AST cuts the typical 13–19 s test TU
// down by roughly half — biggest impact on the heavy test files
// that pull in `glaze`-driven config types and httplib.
//
// Membership policy: include only headers that are
//   1. used by ≥ 25 % of test TUs, AND
//   2. stable (changes here trigger a full PCH rebuild).
//
// Test-internal helpers (CaptureStdout, ArgvBuilder, etc.) STAY
// per-TU — they're small and editable without rebuilding everyone.

#pragma once

// Note — `<doctest/doctest.h>` is intentionally NOT pre-compiled
// here. test_main.cpp must `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`
// BEFORE the include, and PCH-including the header anywhere defeats
// that. Doctest itself is a small header anyway; the saving from
// pre-including it would be marginal next to the wins below.

// ----- top-quartile stdlib usage in the test corpus -----
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

// ----- most-shared acva_core public surface -----
// Each of these appears in ≥ 6 test TUs per `grep -c '^#include'`.
// `config/config.hpp` is the heaviest individual test include (it
// cascades into Glaze types via the YAML reader symbols, even though
// glaze itself isn't pulled in here — Config's nested struct
// definitions alone are deep) and benefits the most.
#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "memory/db.hpp"
#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"
