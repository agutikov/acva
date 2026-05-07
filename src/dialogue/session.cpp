#include "dialogue/session.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

namespace acva::dialogue {

SessionManager::SessionManager(memory::MemoryThread& memory) noexcept
    : memory_(&memory) {}

void SessionManager::register_subscriber(std::string label, Subscriber sub) {
    memory::SessionId snapshot;
    {
        std::lock_guard lk(subs_mtx_);
        subs_.emplace_back(std::move(label), std::move(sub));
        snapshot = current_.load(std::memory_order_acquire);
    }
    // Catch up late subscribers: if a session is already open, push
    // the current id straight away. Outside the mutex so the callback
    // can call back into the manager without deadlocking.
    if (snapshot != 0) {
        const auto& just_added = subs_.back();
        try {
            just_added.second(snapshot);
        } catch (const std::exception& ex) {
            log::warn("session", fmt::format(
                "subscriber '{}' threw on catch-up: {}",
                just_added.first, ex.what()));
        }
    }
}

void SessionManager::notify_subscribers_locked(memory::SessionId id) {
    std::vector<std::pair<std::string, Subscriber>> snapshot;
    {
        std::lock_guard lk(subs_mtx_);
        snapshot = subs_;
    }
    for (auto& [label, sub] : snapshot) {
        try {
            sub(id);
        } catch (const std::exception& ex) {
            log::warn("session", fmt::format(
                "subscriber '{}' threw on session change: {}",
                label, ex.what()));
        }
    }
}

memory::Result<memory::SessionId> SessionManager::open_initial() {
    std::lock_guard lk(op_mtx_);
    if (auto cur = current_.load(std::memory_order_acquire); cur != 0) {
        return cur;
    }
    auto sid_or = memory_->read([](memory::Repository& repo) {
        return repo.insert_session(memory::now_ms(), std::nullopt);
    });
    if (auto* err = std::get_if<memory::DbError>(&sid_or)) {
        return *err;
    }
    const auto sid = std::get<memory::SessionId>(sid_or);
    current_.store(sid, std::memory_order_release);
    log::event("session", "session_opened", event::kNoTurn,
               {{"session_id", std::to_string(sid)}});
    notify_subscribers_locked(sid);
    return sid;
}

memory::Result<memory::SessionId>
SessionManager::adopt(memory::SessionId session_id) {
    std::lock_guard lk(op_mtx_);
    auto found = memory_->read([session_id](memory::Repository& repo) {
        return repo.get_session(session_id);
    });
    if (auto* err = std::get_if<memory::DbError>(&found)) {
        return *err;
    }
    auto& opt = std::get<std::optional<memory::SessionRow>>(found);
    if (!opt.has_value()) {
        return memory::DbError{
            fmt::format("adopt: session id {} not found", session_id)};
    }
    current_.store(session_id, std::memory_order_release);
    log::event("session", "session_adopted", event::kNoTurn,
               {{"session_id", std::to_string(session_id)}});
    notify_subscribers_locked(session_id);
    return session_id;
}

memory::Result<memory::SessionId> SessionManager::roll_over() {
    std::lock_guard lk(op_mtx_);
    const auto prev = current_.load(std::memory_order_acquire);
    if (prev != 0) {
        auto err_opt = memory_->read([prev](memory::Repository& repo) {
            return repo.close_session(prev, memory::now_ms());
        });
        if (err_opt.has_value()) {
            return *err_opt;
        }
        log::event("session", "session_closed", event::kNoTurn,
                   {{"session_id", std::to_string(prev)}});
    }
    auto sid_or = memory_->read([](memory::Repository& repo) {
        return repo.insert_session(memory::now_ms(), std::nullopt);
    });
    if (auto* err = std::get_if<memory::DbError>(&sid_or)) {
        return *err;
    }
    const auto sid = std::get<memory::SessionId>(sid_or);
    current_.store(sid, std::memory_order_release);
    log::event("session", "session_opened", event::kNoTurn,
               {{"session_id", std::to_string(sid)},
                {"reason", prev == 0 ? "initial" : "rollover"}});
    notify_subscribers_locked(sid);
    return sid;
}

std::optional<memory::DbError>
SessionManager::wipe_session(memory::SessionId target_id) {
    std::lock_guard lk(op_mtx_);
    auto err_opt = memory_->read([target_id](memory::Repository& repo) {
        return repo.delete_session(target_id);
    });
    if (err_opt.has_value()) {
        return err_opt;
    }
    log::event("session", "session_wiped", event::kNoTurn,
               {{"session_id", std::to_string(target_id)}});
    if (target_id != current_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    // The active session was wiped — open a fresh one so subsequent
    // turns have somewhere to live.
    auto sid_or = memory_->read([](memory::Repository& repo) {
        return repo.insert_session(memory::now_ms(), std::nullopt);
    });
    if (auto* err = std::get_if<memory::DbError>(&sid_or)) {
        return *err;
    }
    const auto sid = std::get<memory::SessionId>(sid_or);
    current_.store(sid, std::memory_order_release);
    log::event("session", "session_opened", event::kNoTurn,
               {{"session_id", std::to_string(sid)},
                {"reason", "post_wipe"}});
    notify_subscribers_locked(sid);
    return std::nullopt;
}

memory::Result<memory::SessionId> SessionManager::wipe_all() {
    std::lock_guard lk(op_mtx_);
    auto err_opt = memory_->read([](memory::Repository& repo) {
        return repo.wipe_all();
    });
    if (err_opt.has_value()) {
        return *err_opt;
    }
    log::event("session", "wipe_all", event::kNoTurn, {});
    auto sid_or = memory_->read([](memory::Repository& repo) {
        return repo.insert_session(memory::now_ms(), std::nullopt);
    });
    if (auto* err = std::get_if<memory::DbError>(&sid_or)) {
        return *err;
    }
    const auto sid = std::get<memory::SessionId>(sid_or);
    current_.store(sid, std::memory_order_release);
    log::event("session", "session_opened", event::kNoTurn,
               {{"session_id", std::to_string(sid)},
                {"reason", "post_wipe_all"}});
    notify_subscribers_locked(sid);
    return sid;
}

} // namespace acva::dialogue
