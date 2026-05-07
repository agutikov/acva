#include "memory/db.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <variant>

namespace mem = acva::memory;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("acva-repo-") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

mem::Database open_or_die(const fs::path& p) {
    auto r = mem::Database::open(p);
    REQUIRE(std::holds_alternative<mem::Database>(r));
    return std::move(std::get<mem::Database>(r));
}

} // namespace

TEST_CASE("repo: turn lifecycle (insert → status update)") {
    auto p = tmp_db("turn-lifecycle");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));

    auto tid_or = repo.insert_turn(sid, mem::TurnRole::User, std::string("hello"),
                                    std::string("en"), mem::now_ms(),
                                    mem::TurnStatus::InProgress);
    REQUIRE(std::holds_alternative<mem::TurnId>(tid_or));
    const auto tid = std::get<mem::TurnId>(tid_or);

    // List in_progress finds it.
    auto in_prog = repo.turns_in_progress();
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(in_prog));
    REQUIRE(std::get<std::vector<mem::TurnRow>>(in_prog).size() == 1);

    // Mark committed with text.
    auto err = repo.set_turn_status(tid, mem::TurnStatus::Committed, mem::now_ms(),
                                     std::nullopt, std::string("hello world"));
    CHECK_FALSE(err.has_value());

    auto recent = repo.recent_turns(sid, 10);
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(recent));
    const auto& rows = std::get<std::vector<mem::TurnRow>>(recent);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == mem::TurnStatus::Committed);
    REQUIRE(rows[0].text.has_value());
    CHECK(*rows[0].text == "hello world");
}

TEST_CASE("repo: facts upsert is keyed on (key, lang)") {
    auto p = tmp_db("facts");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    CHECK_FALSE(repo.upsert_fact("name", std::string_view{"en"}, "Alex",
                                  std::nullopt, 0.9, mem::now_ms()).has_value());
    CHECK_FALSE(repo.upsert_fact("name", std::string_view{"ru"}, "Алекс",
                                  std::nullopt, 0.9, mem::now_ms()).has_value());

    // Upsert overwrites the same (key, lang).
    CHECK_FALSE(repo.upsert_fact("name", std::string_view{"en"}, "Alexei",
                                  std::nullopt, 0.95, mem::now_ms()).has_value());

    auto facts = repo.facts_with_min_confidence(0.5);
    REQUIRE(std::holds_alternative<std::vector<mem::FactRow>>(facts));
    const auto& fs_rows = std::get<std::vector<mem::FactRow>>(facts);
    REQUIRE(fs_rows.size() == 2);

    // Both should be present, en updated to "Alexei".
    bool found_en = false, found_ru = false;
    for (const auto& f : fs_rows) {
        if (f.lang && *f.lang == "en") {
            found_en = true;
            CHECK(f.value == "Alexei");
            CHECK(f.confidence == doctest::Approx(0.95));
        }
        if (f.lang && *f.lang == "ru") {
            found_ru = true;
        }
    }
    CHECK(found_en);
    CHECK(found_ru);
}

TEST_CASE("repo: settings get/set round-trip") {
    auto p = tmp_db("settings");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto missing = repo.get_setting("nope");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(missing));
    CHECK_FALSE(std::get<std::optional<std::string>>(missing).has_value());

    CHECK_FALSE(repo.set_setting("k", "v1", mem::now_ms()).has_value());
    auto got = repo.get_setting("k");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(got));
    REQUIRE(std::get<std::optional<std::string>>(got).has_value());
    CHECK(*std::get<std::optional<std::string>>(got) == "v1");

    // Update.
    CHECK_FALSE(repo.set_setting("k", "v2", mem::now_ms()).has_value());
    got = repo.get_setting("k");
    CHECK(*std::get<std::optional<std::string>>(got) == "v2");
}

TEST_CASE("repo: delete_session cascades to turns and summaries") {
    auto p = tmp_db("delete-session");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto a = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));
    auto b = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));

    // Three turns split across the two sessions.
    (void)repo.insert_turn(a, mem::TurnRole::User, std::string("hi"),
                            std::string("en"), mem::now_ms(),
                            mem::TurnStatus::Committed);
    (void)repo.insert_turn(a, mem::TurnRole::Assistant, std::string("hello"),
                            std::string("en"), mem::now_ms(),
                            mem::TurnStatus::Committed);
    (void)repo.insert_turn(b, mem::TurnRole::User, std::string("bye"),
                            std::string("en"), mem::now_ms(),
                            mem::TurnStatus::Committed);

    // One summary on session a.
    (void)repo.insert_summary(a, /*range_start*/ 1, /*range_end*/ 2,
                               "summary text", "en", "deadbeef", mem::now_ms());

    auto err = repo.delete_session(a);
    CHECK_FALSE(err.has_value());

    // Session a's two turns are gone; b's one turn survives.
    auto a_turns = repo.recent_turns(a, 100);
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(a_turns));
    CHECK(std::get<std::vector<mem::TurnRow>>(a_turns).empty());

    auto b_turns = repo.recent_turns(b, 100);
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(b_turns));
    CHECK(std::get<std::vector<mem::TurnRow>>(b_turns).size() == 1);

    // Session a's summary is gone.
    auto a_sum = repo.latest_summary(a);
    REQUIRE(std::holds_alternative<std::optional<mem::SummaryRow>>(a_sum));
    CHECK_FALSE(std::get<std::optional<mem::SummaryRow>>(a_sum).has_value());
}

TEST_CASE("repo: wipe_all empties everything and is idempotent") {
    auto p = tmp_db("wipe-all");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));
    (void)repo.insert_turn(sid, mem::TurnRole::User, std::string("hi"),
                            std::string("en"), mem::now_ms(),
                            mem::TurnStatus::Committed);
    (void)repo.set_setting("k", "v", mem::now_ms());

    CHECK_FALSE(repo.wipe_all().has_value());

    auto opens = repo.sessions_open();
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(opens));
    CHECK(std::get<std::vector<mem::SessionRow>>(opens).empty());

    auto setting = repo.get_setting("k");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(setting));
    CHECK_FALSE(std::get<std::optional<std::string>>(setting).has_value());

    // Idempotent: a second wipe on the now-empty schema also succeeds.
    CHECK_FALSE(repo.wipe_all().has_value());

    // Schema is intact after wipe — we can insert a new session.
    auto sid2_or = repo.insert_session(mem::now_ms(), std::nullopt);
    REQUIRE(std::holds_alternative<mem::SessionId>(sid2_or));
}

TEST_CASE("repo: all_sessions newest-first, get_session round trip") {
    auto p = tmp_db("all-sessions");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto a = std::get<mem::SessionId>(repo.insert_session(1000, std::nullopt));
    auto b = std::get<mem::SessionId>(repo.insert_session(2000, std::nullopt));
    auto c = std::get<mem::SessionId>(repo.insert_session(3000, std::nullopt));

    auto rows_or = repo.all_sessions(10);
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(rows_or));
    const auto& rows = std::get<std::vector<mem::SessionRow>>(rows_or);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].id == c);
    CHECK(rows[1].id == b);
    CHECK(rows[2].id == a);

    // limit honored.
    auto top1_or = repo.all_sessions(1);
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(top1_or));
    CHECK(std::get<std::vector<mem::SessionRow>>(top1_or).size() == 1);

    auto found = repo.get_session(b);
    REQUIRE(std::holds_alternative<std::optional<mem::SessionRow>>(found));
    REQUIRE(std::get<std::optional<mem::SessionRow>>(found).has_value());
    CHECK(std::get<std::optional<mem::SessionRow>>(found)->started_at == 2000);

    auto miss = repo.get_session(99999);
    REQUIRE(std::holds_alternative<std::optional<mem::SessionRow>>(miss));
    CHECK_FALSE(std::get<std::optional<mem::SessionRow>>(miss).has_value());
}

TEST_CASE("repo: all_turns + get_turn") {
    auto p = tmp_db("all-turns");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));
    auto t1 = std::get<mem::TurnId>(repo.insert_turn(sid, mem::TurnRole::User,
                                     std::string("hi"), std::string("en"),
                                     mem::now_ms(), mem::TurnStatus::Committed));
    auto t2 = std::get<mem::TurnId>(repo.insert_turn(sid, mem::TurnRole::Assistant,
                                     std::string("hello"), std::string("en"),
                                     mem::now_ms(), mem::TurnStatus::Committed));

    auto rows = repo.all_turns(10);
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(rows));
    REQUIRE(std::get<std::vector<mem::TurnRow>>(rows).size() == 2);
    CHECK(std::get<std::vector<mem::TurnRow>>(rows)[0].id == t2);

    auto found = repo.get_turn(t1);
    REQUIRE(std::holds_alternative<std::optional<mem::TurnRow>>(found));
    REQUIRE(std::get<std::optional<mem::TurnRow>>(found).has_value());
    CHECK(*std::get<std::optional<mem::TurnRow>>(found)->text == "hi");
}

TEST_CASE("repo: delete_turn / delete_fact targeted removal") {
    auto p = tmp_db("delete-targeted");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));
    auto t1 = std::get<mem::TurnId>(repo.insert_turn(sid, mem::TurnRole::User,
                                     std::string("a"), std::string("en"),
                                     mem::now_ms(), mem::TurnStatus::Committed));
    auto t2 = std::get<mem::TurnId>(repo.insert_turn(sid, mem::TurnRole::User,
                                     std::string("b"), std::string("en"),
                                     mem::now_ms(), mem::TurnStatus::Committed));

    CHECK_FALSE(repo.delete_turn(t1).has_value());
    auto rows = repo.all_turns(10);
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(rows));
    REQUIRE(std::get<std::vector<mem::TurnRow>>(rows).size() == 1);
    CHECK(std::get<std::vector<mem::TurnRow>>(rows)[0].id == t2);

    CHECK_FALSE(repo.upsert_fact("name", std::string_view("en"),
                                   "Bob", t2, 0.9, mem::now_ms()).has_value());
    auto facts = repo.facts_with_min_confidence(0.0);
    REQUIRE(std::holds_alternative<std::vector<mem::FactRow>>(facts));
    REQUIRE(std::get<std::vector<mem::FactRow>>(facts).size() == 1);
    auto fid = std::get<std::vector<mem::FactRow>>(facts).front().id;

    CHECK_FALSE(repo.delete_fact(fid).has_value());
    facts = repo.facts_with_min_confidence(0.0);
    CHECK(std::get<std::vector<mem::FactRow>>(facts).empty());
}

TEST_CASE("repo: summaries_by_session filters by session") {
    auto p = tmp_db("summaries-by-session");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto a = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));
    auto b = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));

    (void)repo.insert_summary(a, 1, 5, "summary-a-1", "en", "h1", mem::now_ms());
    (void)repo.insert_summary(a, 6, 10, "summary-a-2", "en", "h2", mem::now_ms());
    (void)repo.insert_summary(b, 1, 3, "summary-b", "en", "h3", mem::now_ms());

    auto a_rows = repo.summaries_by_session(a);
    REQUIRE(std::holds_alternative<std::vector<mem::SummaryRow>>(a_rows));
    CHECK(std::get<std::vector<mem::SummaryRow>>(a_rows).size() == 2);

    auto b_rows = repo.summaries_by_session(b);
    REQUIRE(std::holds_alternative<std::vector<mem::SummaryRow>>(b_rows));
    CHECK(std::get<std::vector<mem::SummaryRow>>(b_rows).size() == 1);
}

TEST_CASE("repo: vacuum is a no-op success on a healthy DB") {
    auto p = tmp_db("vacuum");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    (void)repo.insert_session(mem::now_ms(), std::nullopt);
    CHECK_FALSE(repo.vacuum().has_value());
}
