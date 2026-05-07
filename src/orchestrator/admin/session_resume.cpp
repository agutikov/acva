#include "orchestrator/admin/session_resume.hpp"

#include "event/event.hpp"
#include "log/log.hpp"
#include "memory/repository.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <variant>

namespace acva::orchestrator {

bool try_warm_resume(memory::MemoryThread& memory,
                      dialogue::SessionManager& sessions,
                      const config::Config& cfg,
                      const std::string& config_hash) {
    auto rt_or = memory.read([](memory::Repository& repo) {
        return repo.read_runtime_state();
    });

    auto* row_opt = std::get_if<std::optional<memory::RuntimeStateRow>>(&rt_or);
    if (!row_opt || !row_opt->has_value()) {
        return false;
    }
    const auto& row = **row_opt;
    const auto age_ms = memory::now_ms() - row.checkpoint_at;
    const std::int64_t max_age_ms =
        static_cast<std::int64_t>(cfg.supervisor.checkpoint_max_age_seconds) * 1000;
    const bool fresh = age_ms >= 0 && age_ms <= max_age_ms;
    const bool hash_ok = !config_hash.empty()
                      && row.config_hash.has_value()
                      && *row.config_hash == config_hash;

    bool adopted = false;
    if (fresh && hash_ok) {
        auto adopt_or = sessions.adopt(row.session_id);
        if (std::holds_alternative<memory::SessionId>(adopt_or)) {
            log::event("main", "warm_restart_adopted", event::kNoTurn,
                       {{"session_id", std::to_string(row.session_id)},
                        {"age_ms",     std::to_string(age_ms)},
                        {"fsm_state_at_checkpoint", row.fsm_state}});
            adopted = true;
        } else {
            log::warn("main", fmt::format(
                "warm restart: adopt failed: {}",
                std::get<memory::DbError>(adopt_or).message));
        }
    } else {
        log::info("main", fmt::format(
            "warm restart: discarding checkpoint "
            "(fresh={}, hash_ok={}, age_ms={})",
            fresh, hash_ok, age_ms));
    }

    // Whether we adopted or not, clear the row — a stale row
    // shouldn't survive into another startup, and an adopted row
    // has done its job.
    (void)memory.read([](memory::Repository& repo) {
        return repo.clear_runtime_state();
    });
    return adopted;
}

} // namespace acva::orchestrator
