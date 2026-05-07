#include "dialogue/session.hpp"
#include "memory/db.hpp"
#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mem = acva::memory;
namespace dlg = acva::dialogue;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path()
           / (std::string{"acva-session-"} + name + "-"
              + std::to_string(std::rand()) + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

std::unique_ptr<mem::MemoryThread> open_or_die(const fs::path& p) {
    auto r = mem::MemoryThread::open(p, /*queue_capacity*/ 64);
    REQUIRE(std::holds_alternative<std::unique_ptr<mem::MemoryThread>>(r));
    return std::move(std::get<std::unique_ptr<mem::MemoryThread>>(r));
}

mem::SessionId expect_id(mem::Result<mem::SessionId> r) {
    REQUIRE(std::holds_alternative<mem::SessionId>(r));
    return std::get<mem::SessionId>(r);
}

std::int64_t count_sessions(mem::MemoryThread& m) {
    auto rows_or = m.read([](mem::Repository& repo) {
        return repo.sessions_open();
    });
    if (auto* rows = std::get_if<std::vector<mem::SessionRow>>(&rows_or)) {
        return static_cast<std::int64_t>(rows->size());
    }
    return -1;
}

} // namespace

TEST_CASE("SessionManager: open_initial inserts and notifies") {
    auto path = tmp_db("open-initial");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    std::atomic<mem::SessionId> seen{0};
    sm.register_subscriber("test", [&](mem::SessionId id) {
        seen.store(id, std::memory_order_relaxed);
    });

    auto id = expect_id(sm.open_initial());
    CHECK(id != 0);
    CHECK(sm.id() == id);
    CHECK(seen.load() == id);

    // Idempotent — calling again returns the same id, no new row.
    auto id2 = expect_id(sm.open_initial());
    CHECK(id2 == id);
}

TEST_CASE("SessionManager: roll_over closes prior + opens new + notifies") {
    auto path = tmp_db("rollover");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    std::vector<mem::SessionId> seen;
    sm.register_subscriber("collect", [&](mem::SessionId id) { seen.push_back(id); });

    auto first = expect_id(sm.open_initial());
    auto second = expect_id(sm.roll_over());
    CHECK(second != first);
    CHECK(sm.id() == second);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == first);
    CHECK(seen[1] == second);

    // The first session has ended_at set; the new one does not.
    auto opens = memory->read([](mem::Repository& repo) {
        return repo.sessions_open();
    });
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(opens));
    const auto& rows = std::get<std::vector<mem::SessionRow>>(opens);
    CHECK(rows.size() == 1);
    CHECK(rows.front().id == second);
}

TEST_CASE("SessionManager: late subscriber catches up to current id") {
    auto path = tmp_db("late-subscriber");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    auto id = expect_id(sm.open_initial());

    std::atomic<mem::SessionId> seen{0};
    sm.register_subscriber("late", [&](mem::SessionId v) {
        seen.store(v, std::memory_order_relaxed);
    });
    CHECK(seen.load() == id);
}

TEST_CASE("SessionManager: wipe_session(non-current) leaves current intact") {
    auto path = tmp_db("wipe-non-current");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    auto first = expect_id(sm.open_initial());
    auto second = expect_id(sm.roll_over());
    REQUIRE(first != second);

    int notify_count = 0;
    sm.register_subscriber("count", [&](mem::SessionId) { ++notify_count; });
    notify_count = 0; // discard the catch-up notification

    auto err = sm.wipe_session(first);
    CHECK_FALSE(err.has_value());
    CHECK(sm.id() == second); // current session unchanged
    CHECK(notify_count == 0); // no rollover, no notification
    CHECK(count_sessions(*memory) == 1);
}

TEST_CASE("SessionManager: wipe_session(current) rolls over") {
    auto path = tmp_db("wipe-current");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    auto first = expect_id(sm.open_initial());

    int notify_count = 0;
    mem::SessionId latest = 0;
    sm.register_subscriber("count", [&](mem::SessionId v) {
        ++notify_count;
        latest = v;
    });
    notify_count = 0; // discard the catch-up notification

    auto err = sm.wipe_session(first);
    CHECK_FALSE(err.has_value());
    CHECK(sm.id() != first);
    CHECK(sm.id() != 0);
    CHECK(notify_count == 1);
    CHECK(latest == sm.id());
    CHECK(count_sessions(*memory) == 1);
}

TEST_CASE("SessionManager: adopt picks up an existing id and notifies subscribers") {
    auto path = tmp_db("adopt");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    // Pre-create a session row directly so adopt has a target.
    auto sid = memory->read([](mem::Repository& repo) {
        return repo.insert_session(mem::now_ms(), std::nullopt);
    });
    REQUIRE(std::holds_alternative<mem::SessionId>(sid));
    const auto target = std::get<mem::SessionId>(sid);

    std::atomic<mem::SessionId> seen{0};
    sm.register_subscriber("test", [&](mem::SessionId v) {
        seen.store(v, std::memory_order_relaxed);
    });

    auto adopt_or = sm.adopt(target);
    REQUIRE(std::holds_alternative<mem::SessionId>(adopt_or));
    CHECK(sm.id() == target);
    CHECK(seen.load() == target);
}

TEST_CASE("SessionManager: adopt fails when the id is missing") {
    auto path = tmp_db("adopt-missing");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    auto adopt_or = sm.adopt(99999);
    REQUIRE(std::holds_alternative<mem::DbError>(adopt_or));
    CHECK(sm.id() == 0);
}

TEST_CASE("SessionManager: wipe_all clears DB and opens fresh session") {
    auto path = tmp_db("wipe-all");
    auto memory = open_or_die(path);
    dlg::SessionManager sm(*memory);

    (void)expect_id(sm.open_initial());
    (void)expect_id(sm.roll_over()); // 2 sessions in DB now

    auto sid_or = sm.wipe_all();
    auto fresh = expect_id(sid_or);
    // wipe_all DROPs and re-CREATEs the tables, so AUTOINCREMENT resets;
    // the new session's id equals 1, which can collide with the very
    // first id from before the wipe. The semantic invariant is that
    // exactly one session is now open and it matches sm.id().
    CHECK(fresh != 0);
    CHECK(sm.id() == fresh);
    CHECK(count_sessions(*memory) == 1);

    auto opens = memory->read([](mem::Repository& repo) {
        return repo.sessions_open();
    });
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(opens));
    const auto& rows = std::get<std::vector<mem::SessionRow>>(opens);
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().id == fresh);
}
