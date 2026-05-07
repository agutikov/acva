#pragma once

#include "memory/db.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acva::memory {

using SessionId = std::int64_t;
using TurnId    = std::int64_t;
using SummaryId = std::int64_t;
using FactId    = std::int64_t;

using UnixMs = std::int64_t; // milliseconds since epoch

inline UnixMs now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

enum class TurnRole : std::uint8_t { User, Assistant };
enum class TurnStatus : std::uint8_t { InProgress, Committed, Interrupted, Discarded };

[[nodiscard]] std::string_view to_string(TurnRole r) noexcept;
[[nodiscard]] std::string_view to_string(TurnStatus s) noexcept;

struct SessionRow {
    SessionId id = 0;
    UnixMs started_at = 0;
    std::optional<UnixMs> ended_at;
    std::optional<std::string> title;
};

struct TurnRow {
    TurnId id = 0;
    SessionId session_id = 0;
    TurnRole role = TurnRole::User;
    std::optional<std::string> text;
    std::optional<std::string> lang;
    UnixMs started_at = 0;
    std::optional<UnixMs> ended_at;
    TurnStatus status = TurnStatus::InProgress;
    std::optional<std::int64_t> interrupted_at_sentence;
    std::optional<std::string> audio_path;
};

struct SummaryRow {
    SummaryId id = 0;
    SessionId session_id = 0;
    TurnId range_start_turn = 0;
    TurnId range_end_turn = 0;
    std::string summary;
    std::string lang;
    std::string source_hash;
    UnixMs created_at = 0;
};

// M8A Step 4 — singleton runtime checkpoint row. Persisted via
// `Repository::upsert_runtime_state` (idempotent overwrite — there's
// only ever one row, with id=1) and consulted on startup to decide
// "warm resume" vs "cold open".
struct RuntimeStateRow {
    SessionId session_id = 0;
    std::optional<TurnId> active_turn_id;
    std::string fsm_state;
    std::optional<std::string> last_partial;
    std::optional<std::string> config_hash;
    UnixMs checkpoint_at = 0;
};

struct FactRow {
    FactId id = 0;
    std::string key;
    std::string value;
    std::optional<std::string> lang;
    std::optional<TurnId> source_turn_id;
    double confidence = 0.0;
    UnixMs updated_at = 0;
};

// Repository: typed CRUD on top of Database. All methods are synchronous and
// expect to run on the memory thread (Database is single-thread-by-policy).
class Repository {
public:
    explicit Repository(Database& db) : db_(db) {}

    [[nodiscard]] Database& database() noexcept { return db_; }

    // ----- sessions -----
    [[nodiscard]] Result<SessionId> insert_session(UnixMs started_at,
                                                    std::optional<std::string> title);
    [[nodiscard]] std::optional<DbError> close_session(SessionId id, UnixMs ended_at);
    [[nodiscard]] Result<std::vector<SessionRow>> sessions_open();
    [[nodiscard]] Result<std::vector<SessionRow>> sessions_open_no_ended_at();

    // M8A — privacy commands. delete_session removes one session row;
    // turns + summaries cascade via the FK definitions in
    // memory/schema.hpp, and facts.source_turn_id goes NULL via the
    // turns→facts ON DELETE SET NULL link (the fact rows themselves
    // stay — `wipe_all` is the one that drops everything).
    [[nodiscard]] std::optional<DbError> delete_session(SessionId id);

    // M8A — drops every user-data table and re-creates the schema in a
    // single transaction. Settings rows go too. Idempotent: safe to
    // call against an empty DB.
    [[nodiscard]] std::optional<DbError> wipe_all();

    // M8A Step 3 — listings used by `acva memory <subcommand>` for
    // forensics + cleanup. `all_sessions` returns rows in newest-first
    // order regardless of `ended_at` (sessions_open_no_ended_at filters
    // by NULL ended_at; that's not what an operator browsing history
    // wants). `get_session` is the single-row equivalent.
    [[nodiscard]] Result<std::vector<SessionRow>> all_sessions(int limit);
    [[nodiscard]] Result<std::optional<SessionRow>> get_session(SessionId id);

    // `all_turns` is the cross-session listing for `acva memory turns`.
    // `get_turn` is the single-row lookup; both are consumed by the
    // CLI and tests.
    [[nodiscard]] Result<std::vector<TurnRow>> all_turns(int limit);
    [[nodiscard]] Result<std::optional<TurnRow>> get_turn(TurnId id);

    // Per-session summary listing for `acva memory summaries --session ID`.
    [[nodiscard]] Result<std::vector<SummaryRow>> summaries_by_session(SessionId session);

    // Targeted deletes for `acva memory delete-turn`/`delete-fact`.
    [[nodiscard]] std::optional<DbError> delete_turn(TurnId id);
    [[nodiscard]] std::optional<DbError> delete_fact(FactId id);

    // SQLite VACUUM — reclaim space from a heavily-edited DB. Cannot
    // run inside a transaction; the wrapper opens a fresh connection
    // implicitly via Database::exec.
    [[nodiscard]] std::optional<DbError> vacuum();

    // M8A Step 4 — runtime checkpoint singleton.
    [[nodiscard]] std::optional<DbError> upsert_runtime_state(const RuntimeStateRow& row);
    [[nodiscard]] Result<std::optional<RuntimeStateRow>> read_runtime_state();
    [[nodiscard]] std::optional<DbError> clear_runtime_state();

    // ----- turns -----
    [[nodiscard]] Result<TurnId> insert_turn(SessionId session, TurnRole role,
                                              std::optional<std::string> text,
                                              std::optional<std::string> lang,
                                              UnixMs started_at,
                                              TurnStatus status);
    [[nodiscard]] std::optional<DbError> set_turn_status(
        TurnId id, TurnStatus status, std::optional<UnixMs> ended_at,
        std::optional<std::int64_t> interrupted_at_sentence,
        std::optional<std::string> text);
    [[nodiscard]] Result<std::vector<TurnRow>> recent_turns(SessionId session, int limit);
    [[nodiscard]] Result<std::vector<TurnRow>> turns_in_progress();
    [[nodiscard]] Result<std::optional<UnixMs>> max_turn_ended_at(SessionId session);

    // ----- summaries -----
    [[nodiscard]] Result<SummaryId> insert_summary(SessionId session,
                                                    TurnId range_start, TurnId range_end,
                                                    std::string summary,
                                                    std::string lang,
                                                    std::string source_hash,
                                                    UnixMs created_at);
    [[nodiscard]] Result<std::optional<SummaryRow>> latest_summary(SessionId session);
    [[nodiscard]] Result<std::vector<SummaryRow>> all_summaries();

    // ----- facts -----
    [[nodiscard]] std::optional<DbError> upsert_fact(std::string_view key,
                                                      std::optional<std::string_view> lang,
                                                      std::string_view value,
                                                      std::optional<TurnId> source_turn,
                                                      double confidence,
                                                      UnixMs updated_at);
    [[nodiscard]] Result<std::vector<FactRow>> facts_with_min_confidence(double min);

    // ----- settings -----
    [[nodiscard]] std::optional<DbError> set_setting(std::string_view key,
                                                      std::string_view value,
                                                      UnixMs updated_at);
    [[nodiscard]] Result<std::optional<std::string>> get_setting(std::string_view key);

private:
    Database& db_;
};

// M8A Step 4 — pre-execv checkpoint. Opens a fresh, short-lived
// Database connection (no MemoryThread), upserts the runtime_state
// row, and commits before returning. Used inside the /restart
// handler immediately before the process replaces itself with
// `execv` — by then the orchestrator's own MemoryThread has been
// stopped, so we can't go through it. Returns DbError on open or
// write failure; the caller should NOT execv on failure (the
// session would be lost without its checkpoint anchor).
[[nodiscard]] std::optional<DbError>
checkpoint_runtime_sync(const std::filesystem::path& db_path,
                         const RuntimeStateRow& row);

} // namespace acva::memory
