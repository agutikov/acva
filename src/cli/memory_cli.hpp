#pragma once

namespace acva::cli {

// M8A Step 3 — `acva memory <subcommand> [options]` dispatcher.
//
// The CLI is a separate process from the running orchestrator: it
// opens the SQLite DB directly via `memory::Database` (no
// MemoryThread). Reads coexist with a live orchestrator thanks to
// SQLite's WAL mode; writes contend, and we surface SQLite's BUSY
// errors with a friendly "stop the orchestrator first" message.
//
// `argv` is the post-`acva` slice — the entry-point in main.cpp pops
// the program name and "memory" tokens before calling this. argv[0]
// is the subcommand name; argv[1..] are flags + positional args.
//
// Returns the exit code: 0 on success, 1 on user error (bad args,
// missing target row), 2 on system error (DB open failure, SQLite
// I/O / busy).
[[nodiscard]] int run_memory_subcommand(int argc, char** argv);

// Print the `acva memory` help text. Called either by
// `acva memory --help` or after an unrecognised subcommand.
void print_memory_help();

} // namespace acva::cli
