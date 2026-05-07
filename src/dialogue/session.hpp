#pragma once

#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acva::dialogue {

// M8A Step 2 — owns the current session id and orchestrates the
// privacy-command surface (rollover, single-session wipe, full wipe).
//
// The pre-M8A behaviour (single startup-time `insert_session` in
// `dialogue_stack`) folded into this component: instead of three
// `set_session(...)` call sites, consumers register subscribers on the
// SessionManager and the manager fans out the new id whenever the
// session rolls over.
//
// All DB calls go through `MemoryThread::read` so they execute on the
// memory thread; we don't open a second sqlite handle. Roll-overs are
// serialised by an internal mutex so concurrent /new-session +
// SIGHUP / /wipe?all=true can't race each other.
//
// Idle-timeout-driven rollover (cfg.dialogue.session_idle_minutes,
// default 30) lands separately; this slice covers the explicit-command
// path that the HTTP endpoints need.

class SessionManager {
public:
    // A subscriber is notified whenever the active session id changes
    // (initial open, rollover, post-wipe). `label` is a short
    // identifier for diagnostics (e.g. "manager", "turn_writer").
    using Subscriber = std::function<void(memory::SessionId)>;

    explicit SessionManager(memory::MemoryThread& memory) noexcept;

    // Snapshot of the current session id. Returns 0 before
    // `open_initial` has run.
    [[nodiscard]] memory::SessionId id() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    // Register a subscriber. Subscribers added after `open_initial`
    // are immediately notified with the current id so they catch up
    // without a separate manual `set_session` call.
    void register_subscriber(std::string label, Subscriber sub);

    // Open the very first session. Idempotent: a second call with a
    // current id returns the current id without inserting another
    // row. Notifies every registered subscriber with the new id.
    [[nodiscard]] memory::Result<memory::SessionId> open_initial();

    // Close the current session (sets ended_at) and open a new one.
    // Notifies subscribers with the new id. If no current session
    // exists, equivalent to open_initial.
    [[nodiscard]] memory::Result<memory::SessionId> roll_over();

    // Delete one session and its cascading rows. If `target_id`
    // matches the current session, the manager rolls over to a fresh
    // session afterwards; otherwise the current session is untouched.
    [[nodiscard]] std::optional<memory::DbError>
        wipe_session(memory::SessionId target_id);

    // Drop + recreate the schema (every session, every turn, every
    // summary, every fact, every setting), then open a fresh session.
    [[nodiscard]] memory::Result<memory::SessionId> wipe_all();

private:
    memory::MemoryThread* memory_;
    std::atomic<memory::SessionId> current_{0};

    std::mutex op_mtx_;       // serialises rollovers / wipes
    std::mutex subs_mtx_;
    std::vector<std::pair<std::string, Subscriber>> subs_;

    void notify_subscribers_locked(memory::SessionId id);
};

} // namespace acva::dialogue
