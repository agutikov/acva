#pragma once

#include "dialogue/fsm.hpp"
#include "dialogue/session.hpp"
#include "memory/db.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace acva::orchestrator {

// M8A Step 4 — owns the warm-restart request flag with built-in
// debounce. Two callers flip the flag:
//   1. The HTTP /restart handler (via ControlServer::RestartHandler).
//   2. The Watchdog when `auto_restart_on_stuck` fires.
// The main loop drains the flag and runs the orderly shutdown ->
// checkpoint -> execv path.
class RestartRequester {
public:
    // Debounce window: refuse to flip again within this many ms of
    // a previous accepted request, so a flapping watchdog can't drive
    // an exec loop. 5 s matches the M8A spec.
    explicit RestartRequester(std::int64_t debounce_ms = 5000) noexcept
        : debounce_ms_(debounce_ms) {}

    // Returns nullopt on accept (flag now set), or a reason string on
    // rejection. Suitable as a `ControlServer::RestartHandler`.
    [[nodiscard]] std::optional<std::string> request();

    // Was a restart accepted since the last `clear()`? Read from any
    // thread.
    [[nodiscard]] bool is_requested() const noexcept {
        return flag_.load(std::memory_order_acquire);
    }

    void clear() noexcept {
        flag_.store(false, std::memory_order_release);
    }

private:
    std::int64_t              debounce_ms_;
    std::atomic<bool>         flag_{false};
    std::atomic<std::int64_t> last_request_ms_{0};
};

// M8A Step 4 — write the runtime checkpoint synchronously via a
// fresh DB connection (`memory::checkpoint_runtime_sync`). Called
// from the warm-restart driver in main.cpp after the orchestrator's
// own MemoryThread has been torn down. Returns the DbError unchanged
// on failure so the caller can decide whether to abort the exec.
[[nodiscard]] std::optional<memory::DbError>
write_warm_restart_checkpoint(const std::filesystem::path& db_path,
                               const dialogue::FsmSnapshot& snap,
                               const dialogue::SessionManager& sessions,
                               const std::string& config_hash);

} // namespace acva::orchestrator
