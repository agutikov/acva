#include "orchestrator/admin/restart_driver.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

namespace acva::orchestrator {

std::optional<std::string> RestartRequester::request() {
    const auto now  = memory::now_ms();
    const auto last = last_request_ms_.load(std::memory_order_acquire);
    if (last != 0 && (now - last) < debounce_ms_) {
        return fmt::format("debounced; previous request {} ms ago",
                           now - last);
    }
    last_request_ms_.store(now, std::memory_order_release);
    flag_.store(true, std::memory_order_release);
    log::info("main", "restart requested");
    return std::nullopt;
}

std::optional<memory::DbError>
write_warm_restart_checkpoint(const std::filesystem::path& db_path,
                               const dialogue::FsmSnapshot& snap,
                               const dialogue::SessionManager& sessions,
                               const std::string& config_hash) {
    memory::RuntimeStateRow row{
        .session_id     = sessions.id(),
        .active_turn_id = (snap.active_turn != event::kNoTurn
                              ? std::optional<std::int64_t>{snap.active_turn}
                              : std::nullopt),
        .fsm_state      = std::string(dialogue::to_string(snap.state)),
        .last_partial   = std::nullopt,
        .config_hash    = config_hash.empty()
                              ? std::optional<std::string>{}
                              : std::optional<std::string>{config_hash},
        .checkpoint_at  = memory::now_ms(),
    };
    return memory::checkpoint_runtime_sync(db_path, row);
}

} // namespace acva::orchestrator
