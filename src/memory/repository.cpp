#include "memory/repository.hpp"

#include "memory/schema.hpp"

#include <sqlite3.h>

#include <algorithm>

namespace acva::memory {

std::string_view to_string(TurnRole r) noexcept {
    return r == TurnRole::User ? "user" : "assistant";
}

std::string_view to_string(TurnStatus s) noexcept {
    switch (s) {
        case TurnStatus::InProgress:  return "in_progress";
        case TurnStatus::Committed:   return "committed";
        case TurnStatus::Interrupted: return "interrupted";
        case TurnStatus::Discarded:   return "discarded";
    }
    return "unknown";
}

namespace {

TurnRole parse_role(std::string_view s) {
    return s == "assistant" ? TurnRole::Assistant : TurnRole::User;
}

TurnStatus parse_status(std::string_view s) {
    if (s == "committed")   return TurnStatus::Committed;
    if (s == "interrupted") return TurnStatus::Interrupted;
    if (s == "discarded")   return TurnStatus::Discarded;
    return TurnStatus::InProgress;
}

} // namespace

// ----- sessions -----

Result<SessionId> Repository::insert_session(UnixMs started_at,
                                              std::optional<std::string> title) {
    Statement stmt(db_.raw(),
        "INSERT INTO sessions(started_at, title) VALUES(?1, ?2);");
    if (!stmt.ok()) return DbError{"prepare insert_session"};
    stmt.bind(1, static_cast<std::int64_t>(started_at));
    stmt.bind(2, title);
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"insert_session step"};
    }
    return db_.last_insert_rowid();
}

std::optional<DbError> Repository::close_session(SessionId id, UnixMs ended_at) {
    Statement stmt(db_.raw(),
        "UPDATE sessions SET ended_at = ?1 WHERE id = ?2;");
    if (!stmt.ok()) return DbError{"prepare close_session"};
    stmt.bind(1, static_cast<std::int64_t>(ended_at));
    stmt.bind(2, id);
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"close_session step"};
    }
    return std::nullopt;
}

Result<std::vector<SessionRow>> Repository::sessions_open_no_ended_at() {
    Statement stmt(db_.raw(),
        "SELECT id, started_at, ended_at, title FROM sessions WHERE ended_at IS NULL;");
    if (!stmt.ok()) return DbError{"prepare sessions_open_no_ended_at"};
    std::vector<SessionRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(SessionRow{
            .id         = stmt.column_int64(0),
            .started_at = stmt.column_int64(1),
            .ended_at   = stmt.column_int64_opt(2),
            .title      = stmt.column_text_opt(3),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"sessions_open_no_ended_at step"};
    return rows;
}

Result<std::vector<SessionRow>> Repository::sessions_open() {
    return sessions_open_no_ended_at();
}

std::optional<DbError> Repository::delete_session(SessionId id) {
    Statement stmt(db_.raw(), "DELETE FROM sessions WHERE id = ?1;");
    if (!stmt.ok()) return DbError{"prepare delete_session"};
    stmt.bind(1, id);
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"delete_session step"};
    }
    return std::nullopt;
}

std::optional<DbError> Repository::wipe_all() {
    // Single-transaction wipe: drop all user-data tables and re-create
    // the schema. The DROP order matters under foreign_keys=ON — drop
    // the dependents (turns, summaries, facts) before sessions so the
    // FK enforcement doesn't reject the parent's removal. settings is
    // independent. We rely on the same kSchemaSql block the database
    // uses on first open, so the result is byte-for-byte the cold
    // schema.
    Database::Transaction tx(db_, /*immediate*/ true);
    constexpr const char* kDrop =
        "DROP TABLE IF EXISTS facts;"
        "DROP TABLE IF EXISTS summaries;"
        "DROP TABLE IF EXISTS turns;"
        "DROP TABLE IF EXISTS sessions;"
        "DROP TABLE IF EXISTS settings;"
        "DROP INDEX IF EXISTS idx_turns_session;"
        "DROP INDEX IF EXISTS idx_summaries_session;"
        "DROP INDEX IF EXISTS idx_facts_key;";
    if (auto err = db_.exec(kDrop)) {
        tx.rollback();
        return err;
    }
    if (auto err = db_.exec(kSchemaSql)) {
        tx.rollback();
        return err;
    }
    if (auto err = tx.commit()) {
        return err;
    }
    return std::nullopt;
}

// ----- turns -----

Result<TurnId> Repository::insert_turn(SessionId session, TurnRole role,
                                        std::optional<std::string> text,
                                        std::optional<std::string> lang,
                                        UnixMs started_at,
                                        TurnStatus status) {
    Statement stmt(db_.raw(),
        "INSERT INTO turns(session_id, role, text, lang, started_at, status) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
    if (!stmt.ok()) return DbError{"prepare insert_turn"};
    stmt.bind(1, session);
    stmt.bind(2, std::string_view{to_string(role)});
    stmt.bind(3, text);
    stmt.bind(4, lang);
    stmt.bind(5, static_cast<std::int64_t>(started_at));
    stmt.bind(6, std::string_view{to_string(status)});
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"insert_turn step"};
    }
    return db_.last_insert_rowid();
}

std::optional<DbError> Repository::set_turn_status(
    TurnId id, TurnStatus status, std::optional<UnixMs> ended_at,
    std::optional<std::int64_t> interrupted_at_sentence,
    std::optional<std::string> text) {

    Statement stmt(db_.raw(),
        "UPDATE turns SET status = ?1, "
        "ended_at = COALESCE(?2, ended_at), "
        "interrupted_at_sentence = ?3, "
        "text = COALESCE(?4, text) "
        "WHERE id = ?5;");
    if (!stmt.ok()) return DbError{"prepare set_turn_status"};
    stmt.bind(1, std::string_view{to_string(status)});
    if (ended_at) stmt.bind(2, static_cast<std::int64_t>(*ended_at)); else stmt.bind_null(2);
    stmt.bind(3, interrupted_at_sentence);
    stmt.bind(4, text);
    stmt.bind(5, id);
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"set_turn_status step"};
    }
    return std::nullopt;
}

Result<std::vector<TurnRow>> Repository::recent_turns(SessionId session, int limit) {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, role, text, lang, started_at, ended_at, "
        "       status, interrupted_at_sentence, audio_path "
        "FROM turns WHERE session_id = ?1 "
        "ORDER BY id DESC LIMIT ?2;");
    if (!stmt.ok()) return DbError{"prepare recent_turns"};
    stmt.bind(1, session);
    stmt.bind(2, static_cast<std::int64_t>(limit));

    std::vector<TurnRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(TurnRow{
            .id                      = stmt.column_int64(0),
            .session_id              = stmt.column_int64(1),
            .role                    = parse_role(stmt.column_text(2)),
            .text                    = stmt.column_text_opt(3),
            .lang                    = stmt.column_text_opt(4),
            .started_at              = stmt.column_int64(5),
            .ended_at                = stmt.column_int64_opt(6),
            .status                  = parse_status(stmt.column_text(7)),
            .interrupted_at_sentence = stmt.column_int64_opt(8),
            .audio_path              = stmt.column_text_opt(9),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"recent_turns step"};
    // Caller wants oldest-first.
    std::reverse(rows.begin(), rows.end());
    return rows;
}

Result<std::vector<TurnRow>> Repository::turns_in_progress() {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, role, text, lang, started_at, ended_at, "
        "       status, interrupted_at_sentence, audio_path "
        "FROM turns WHERE status = 'in_progress';");
    if (!stmt.ok()) return DbError{"prepare turns_in_progress"};

    std::vector<TurnRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(TurnRow{
            .id                      = stmt.column_int64(0),
            .session_id              = stmt.column_int64(1),
            .role                    = parse_role(stmt.column_text(2)),
            .text                    = stmt.column_text_opt(3),
            .lang                    = stmt.column_text_opt(4),
            .started_at              = stmt.column_int64(5),
            .ended_at                = stmt.column_int64_opt(6),
            .status                  = parse_status(stmt.column_text(7)),
            .interrupted_at_sentence = stmt.column_int64_opt(8),
            .audio_path              = stmt.column_text_opt(9),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"turns_in_progress step"};
    return rows;
}

Result<std::optional<UnixMs>> Repository::max_turn_ended_at(SessionId session) {
    Statement stmt(db_.raw(),
        "SELECT MAX(ended_at) FROM turns WHERE session_id = ?1;");
    if (!stmt.ok()) return DbError{"prepare max_turn_ended_at"};
    stmt.bind(1, session);
    if (stmt.step() != Statement::StepResult::Row) return DbError{"max_turn_ended_at step"};
    if (stmt.column_is_null(0)) return std::optional<UnixMs>{};
    return std::optional<UnixMs>{stmt.column_int64(0)};
}

// ----- summaries -----

Result<SummaryId> Repository::insert_summary(SessionId session,
                                              TurnId range_start, TurnId range_end,
                                              std::string summary,
                                              std::string lang,
                                              std::string source_hash,
                                              UnixMs created_at) {
    Statement stmt(db_.raw(),
        "INSERT INTO summaries(session_id, range_start_turn, range_end_turn, "
        "                     summary, lang, source_hash, created_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    if (!stmt.ok()) return DbError{"prepare insert_summary"};
    stmt.bind(1, session);
    stmt.bind(2, range_start);
    stmt.bind(3, range_end);
    stmt.bind(4, std::string_view{summary});
    stmt.bind(5, std::string_view{lang});
    stmt.bind(6, std::string_view{source_hash});
    stmt.bind(7, static_cast<std::int64_t>(created_at));
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"insert_summary step"};
    }
    return db_.last_insert_rowid();
}

Result<std::optional<SummaryRow>> Repository::latest_summary(SessionId session) {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, range_start_turn, range_end_turn, summary, "
        "       lang, source_hash, created_at "
        "FROM summaries WHERE session_id = ?1 "
        "ORDER BY range_end_turn DESC LIMIT 1;");
    if (!stmt.ok()) return DbError{"prepare latest_summary"};
    stmt.bind(1, session);
    auto r = stmt.step();
    if (r == Statement::StepResult::Done) return std::optional<SummaryRow>{};
    if (r == Statement::StepResult::Error) return DbError{"latest_summary step"};
    return std::optional<SummaryRow>{SummaryRow{
        .id                = stmt.column_int64(0),
        .session_id        = stmt.column_int64(1),
        .range_start_turn  = stmt.column_int64(2),
        .range_end_turn    = stmt.column_int64(3),
        .summary           = stmt.column_text(4),
        .lang              = stmt.column_text(5),
        .source_hash       = stmt.column_text(6),
        .created_at        = stmt.column_int64(7),
    }};
}

Result<std::vector<SummaryRow>> Repository::all_summaries() {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, range_start_turn, range_end_turn, summary, "
        "       lang, source_hash, created_at FROM summaries;");
    if (!stmt.ok()) return DbError{"prepare all_summaries"};
    std::vector<SummaryRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(SummaryRow{
            .id                = stmt.column_int64(0),
            .session_id        = stmt.column_int64(1),
            .range_start_turn  = stmt.column_int64(2),
            .range_end_turn    = stmt.column_int64(3),
            .summary           = stmt.column_text(4),
            .lang              = stmt.column_text(5),
            .source_hash       = stmt.column_text(6),
            .created_at        = stmt.column_int64(7),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"all_summaries step"};
    return rows;
}

// ----- facts -----

std::optional<DbError> Repository::upsert_fact(std::string_view key,
                                                std::optional<std::string_view> lang,
                                                std::string_view value,
                                                std::optional<TurnId> source_turn,
                                                double confidence,
                                                UnixMs updated_at) {
    Statement stmt(db_.raw(),
        "INSERT INTO facts(key, value, lang, source_turn_id, confidence, updated_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(key, lang) DO UPDATE SET "
        "  value = excluded.value, "
        "  source_turn_id = excluded.source_turn_id, "
        "  confidence = excluded.confidence, "
        "  updated_at = excluded.updated_at;");
    if (!stmt.ok()) return DbError{"prepare upsert_fact"};
    stmt.bind(1, key);
    stmt.bind(2, value);
    if (lang) stmt.bind(3, *lang); else stmt.bind_null(3);
    if (source_turn) stmt.bind(4, *source_turn); else stmt.bind_null(4);
    stmt.bind(5, confidence);
    stmt.bind(6, static_cast<std::int64_t>(updated_at));
    if (stmt.step() != Statement::StepResult::Done) return DbError{"upsert_fact step"};
    return std::nullopt;
}

Result<std::vector<FactRow>> Repository::facts_with_min_confidence(double min) {
    Statement stmt(db_.raw(),
        "SELECT id, key, value, lang, source_turn_id, confidence, updated_at "
        "FROM facts WHERE confidence >= ?1 ORDER BY key, lang;");
    if (!stmt.ok()) return DbError{"prepare facts_with_min_confidence"};
    stmt.bind(1, min);
    std::vector<FactRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(FactRow{
            .id              = stmt.column_int64(0),
            .key             = stmt.column_text(1),
            .value           = stmt.column_text(2),
            .lang            = stmt.column_text_opt(3),
            .source_turn_id  = stmt.column_int64_opt(4),
            .confidence      = stmt.column_double(5),
            .updated_at      = stmt.column_int64(6),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"facts_with_min_confidence step"};
    return rows;
}

// ----- M8A Step 3 CLI listings + targeted deletes -----

Result<std::vector<SessionRow>> Repository::all_sessions(int limit) {
    Statement stmt(db_.raw(),
        "SELECT id, started_at, ended_at, title FROM sessions "
        "ORDER BY id DESC LIMIT ?1;");
    if (!stmt.ok()) return DbError{"prepare all_sessions"};
    stmt.bind(1, static_cast<std::int64_t>(limit));
    std::vector<SessionRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(SessionRow{
            .id         = stmt.column_int64(0),
            .started_at = stmt.column_int64(1),
            .ended_at   = stmt.column_int64_opt(2),
            .title      = stmt.column_text_opt(3),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"all_sessions step"};
    return rows;
}

Result<std::optional<SessionRow>> Repository::get_session(SessionId id) {
    Statement stmt(db_.raw(),
        "SELECT id, started_at, ended_at, title FROM sessions WHERE id = ?1;");
    if (!stmt.ok()) return DbError{"prepare get_session"};
    stmt.bind(1, id);
    auto r = stmt.step();
    if (r == Statement::StepResult::Done) return std::optional<SessionRow>{};
    if (r == Statement::StepResult::Error) return DbError{"get_session step"};
    SessionRow row{
        .id         = stmt.column_int64(0),
        .started_at = stmt.column_int64(1),
        .ended_at   = stmt.column_int64_opt(2),
        .title      = stmt.column_text_opt(3),
    };
    return std::optional<SessionRow>{row};
}

Result<std::vector<TurnRow>> Repository::all_turns(int limit) {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, role, text, lang, started_at, ended_at, "
        "       status, interrupted_at_sentence, audio_path "
        "FROM turns ORDER BY id DESC LIMIT ?1;");
    if (!stmt.ok()) return DbError{"prepare all_turns"};
    stmt.bind(1, static_cast<std::int64_t>(limit));
    std::vector<TurnRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(TurnRow{
            .id                      = stmt.column_int64(0),
            .session_id              = stmt.column_int64(1),
            .role                    = parse_role(stmt.column_text(2)),
            .text                    = stmt.column_text_opt(3),
            .lang                    = stmt.column_text_opt(4),
            .started_at              = stmt.column_int64(5),
            .ended_at                = stmt.column_int64_opt(6),
            .status                  = parse_status(stmt.column_text(7)),
            .interrupted_at_sentence = stmt.column_int64_opt(8),
            .audio_path              = stmt.column_text_opt(9),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"all_turns step"};
    return rows;
}

Result<std::optional<TurnRow>> Repository::get_turn(TurnId id) {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, role, text, lang, started_at, ended_at, "
        "       status, interrupted_at_sentence, audio_path "
        "FROM turns WHERE id = ?1;");
    if (!stmt.ok()) return DbError{"prepare get_turn"};
    stmt.bind(1, id);
    auto r = stmt.step();
    if (r == Statement::StepResult::Done) return std::optional<TurnRow>{};
    if (r == Statement::StepResult::Error) return DbError{"get_turn step"};
    TurnRow row{
        .id                      = stmt.column_int64(0),
        .session_id              = stmt.column_int64(1),
        .role                    = parse_role(stmt.column_text(2)),
        .text                    = stmt.column_text_opt(3),
        .lang                    = stmt.column_text_opt(4),
        .started_at              = stmt.column_int64(5),
        .ended_at                = stmt.column_int64_opt(6),
        .status                  = parse_status(stmt.column_text(7)),
        .interrupted_at_sentence = stmt.column_int64_opt(8),
        .audio_path              = stmt.column_text_opt(9),
    };
    return std::optional<TurnRow>{row};
}

Result<std::vector<SummaryRow>> Repository::summaries_by_session(SessionId session) {
    Statement stmt(db_.raw(),
        "SELECT id, session_id, range_start_turn, range_end_turn, summary, "
        "       lang, source_hash, created_at FROM summaries "
        "WHERE session_id = ?1 ORDER BY range_end_turn DESC;");
    if (!stmt.ok()) return DbError{"prepare summaries_by_session"};
    stmt.bind(1, session);
    std::vector<SummaryRow> rows;
    Statement::StepResult r;
    while ((r = stmt.step()) == Statement::StepResult::Row) {
        rows.push_back(SummaryRow{
            .id                = stmt.column_int64(0),
            .session_id        = stmt.column_int64(1),
            .range_start_turn  = stmt.column_int64(2),
            .range_end_turn    = stmt.column_int64(3),
            .summary           = stmt.column_text(4),
            .lang              = stmt.column_text(5),
            .source_hash       = stmt.column_text(6),
            .created_at        = stmt.column_int64(7),
        });
    }
    if (r == Statement::StepResult::Error) return DbError{"summaries_by_session step"};
    return rows;
}

std::optional<DbError> Repository::delete_turn(TurnId id) {
    Statement stmt(db_.raw(), "DELETE FROM turns WHERE id = ?1;");
    if (!stmt.ok()) return DbError{"prepare delete_turn"};
    stmt.bind(1, id);
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"delete_turn step"};
    }
    return std::nullopt;
}

std::optional<DbError> Repository::delete_fact(FactId id) {
    Statement stmt(db_.raw(), "DELETE FROM facts WHERE id = ?1;");
    if (!stmt.ok()) return DbError{"prepare delete_fact"};
    stmt.bind(1, id);
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"delete_fact step"};
    }
    return std::nullopt;
}

std::optional<DbError> Repository::vacuum() {
    return db_.exec("VACUUM;");
}

// ----- M8A Step 4: runtime checkpoint singleton -----

std::optional<DbError>
Repository::upsert_runtime_state(const RuntimeStateRow& row) {
    Statement stmt(db_.raw(),
        "INSERT INTO runtime_state(id, session_id, active_turn_id, fsm_state, "
        "                          last_partial, config_hash, checkpoint_at) "
        "VALUES(1, ?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  session_id = excluded.session_id, "
        "  active_turn_id = excluded.active_turn_id, "
        "  fsm_state = excluded.fsm_state, "
        "  last_partial = excluded.last_partial, "
        "  config_hash = excluded.config_hash, "
        "  checkpoint_at = excluded.checkpoint_at;");
    if (!stmt.ok()) return DbError{"prepare upsert_runtime_state"};
    stmt.bind(1, row.session_id);
    stmt.bind(2, row.active_turn_id);
    stmt.bind(3, std::string_view{row.fsm_state});
    stmt.bind(4, row.last_partial);
    stmt.bind(5, row.config_hash);
    stmt.bind(6, static_cast<std::int64_t>(row.checkpoint_at));
    if (stmt.step() != Statement::StepResult::Done) {
        return DbError{"upsert_runtime_state step"};
    }
    return std::nullopt;
}

Result<std::optional<RuntimeStateRow>> Repository::read_runtime_state() {
    Statement stmt(db_.raw(),
        "SELECT session_id, active_turn_id, fsm_state, last_partial, "
        "       config_hash, checkpoint_at "
        "FROM runtime_state WHERE id = 1;");
    if (!stmt.ok()) return DbError{"prepare read_runtime_state"};
    auto r = stmt.step();
    if (r == Statement::StepResult::Done) return std::optional<RuntimeStateRow>{};
    if (r == Statement::StepResult::Error) return DbError{"read_runtime_state step"};
    RuntimeStateRow row{
        .session_id     = stmt.column_int64(0),
        .active_turn_id = stmt.column_int64_opt(1),
        .fsm_state      = stmt.column_text(2),
        .last_partial   = stmt.column_text_opt(3),
        .config_hash    = stmt.column_text_opt(4),
        .checkpoint_at  = stmt.column_int64(5),
    };
    return std::optional<RuntimeStateRow>{row};
}

std::optional<DbError> Repository::clear_runtime_state() {
    return db_.exec("DELETE FROM runtime_state;");
}

std::optional<DbError>
checkpoint_runtime_sync(const std::filesystem::path& db_path,
                         const RuntimeStateRow& row) {
    auto db_or = Database::open(db_path);
    if (auto* err = std::get_if<DbError>(&db_or)) return *err;
    auto db = std::move(std::get<Database>(db_or));
    Repository repo(db);
    return repo.upsert_runtime_state(row);
}

// ----- settings -----

std::optional<DbError> Repository::set_setting(std::string_view key,
                                                std::string_view value,
                                                UnixMs updated_at) {
    Statement stmt(db_.raw(),
        "INSERT INTO settings(key, value, updated_at) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;");
    if (!stmt.ok()) return DbError{"prepare set_setting"};
    stmt.bind(1, key);
    stmt.bind(2, value);
    stmt.bind(3, static_cast<std::int64_t>(updated_at));
    if (stmt.step() != Statement::StepResult::Done) return DbError{"set_setting step"};
    return std::nullopt;
}

Result<std::optional<std::string>> Repository::get_setting(std::string_view key) {
    Statement stmt(db_.raw(),
        "SELECT value FROM settings WHERE key = ?1;");
    if (!stmt.ok()) return DbError{"prepare get_setting"};
    stmt.bind(1, key);
    auto r = stmt.step();
    if (r == Statement::StepResult::Done) return std::optional<std::string>{};
    if (r == Statement::StepResult::Error) return DbError{"get_setting step"};
    return std::optional<std::string>{stmt.column_text(0)};
}

} // namespace acva::memory
