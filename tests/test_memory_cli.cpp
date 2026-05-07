#include "cli/memory_cli.hpp"
#include "memory/db.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
namespace mem = acva::memory;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path()
           / (std::string{"acva-clitest-"} + name + "-"
              + std::to_string(::getpid()) + "-"
              + std::to_string(std::rand()) + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

// Seed a tmp DB with one session + a few turns + a fact + a summary.
struct SeedIds {
    mem::SessionId session = 0;
    mem::TurnId user_turn = 0;
    mem::TurnId asst_turn = 0;
    mem::FactId fact_id = 0;
};

SeedIds seed(const fs::path& p) {
    SeedIds out;
    auto r = mem::Database::open(p);
    REQUIRE(std::holds_alternative<mem::Database>(r));
    auto db = std::move(std::get<mem::Database>(r));
    mem::Repository repo(db);

    out.session = std::get<mem::SessionId>(
        repo.insert_session(mem::now_ms(), std::nullopt));
    out.user_turn = std::get<mem::TurnId>(repo.insert_turn(
        out.session, mem::TurnRole::User,
        std::string("hello"), std::string("en"),
        mem::now_ms(), mem::TurnStatus::Committed));
    out.asst_turn = std::get<mem::TurnId>(repo.insert_turn(
        out.session, mem::TurnRole::Assistant,
        std::string("hi there"), std::string("en"),
        mem::now_ms(), mem::TurnStatus::Committed));

    REQUIRE_FALSE(repo.upsert_fact("name", std::string_view("en"),
                                    "Alice", out.user_turn, 0.9,
                                    mem::now_ms()).has_value());
    auto facts = repo.facts_with_min_confidence(0.0);
    REQUIRE(std::holds_alternative<std::vector<mem::FactRow>>(facts));
    REQUIRE_FALSE(std::get<std::vector<mem::FactRow>>(facts).empty());
    out.fact_id = std::get<std::vector<mem::FactRow>>(facts).front().id;

    (void)repo.insert_summary(out.session, 1, 2, "first chat", "en", "abc",
                               mem::now_ms());
    return out;
}

// Capture stdout for the duration of a callback. Used to assert on the
// content of `acva memory <cmd> [...]` invocations.
class CaptureStdout {
public:
    CaptureStdout() {
        std::fflush(stdout);
        saved_fd_ = ::dup(STDOUT_FILENO);
        REQUIRE(saved_fd_ >= 0);
        path_ = fs::temp_directory_path()
              / (std::string{"acva-clicap-"}
                 + std::to_string(::getpid()) + "-"
                 + std::to_string(std::rand()));
        fd_ = ::open(path_.c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
        REQUIRE(fd_ >= 0);
        REQUIRE(::dup2(fd_, STDOUT_FILENO) >= 0);
    }
    ~CaptureStdout() {
        std::fflush(stdout);
        if (saved_fd_ >= 0) {
            ::dup2(saved_fd_, STDOUT_FILENO);
            ::close(saved_fd_);
        }
        if (fd_ >= 0) ::close(fd_);
        fs::remove(path_);
    }
    [[nodiscard]] std::string read() const {
        std::fflush(stdout);
        std::FILE* f = std::fopen(path_.c_str(), "r");
        if (!f) return {};
        std::string s;
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            s.append(buf, n);
        }
        std::fclose(f);
        return s;
    }
    CaptureStdout(const CaptureStdout&)            = delete;
    CaptureStdout& operator=(const CaptureStdout&) = delete;

private:
    int saved_fd_ = -1;
    int fd_ = -1;
    fs::path path_;
};

// Build a contiguous mutable argv from a list of strings.
class ArgvBuilder {
public:
    explicit ArgvBuilder(std::initializer_list<std::string> args) {
        storage_.reserve(args.size());
        ptrs_.reserve(args.size());
        for (auto& s : args) {
            storage_.push_back(s);
        }
        for (auto& s : storage_) ptrs_.push_back(s.data());
    }
    [[nodiscard]] int    argc() const noexcept { return static_cast<int>(ptrs_.size()); }
    [[nodiscard]] char** argv()       noexcept { return ptrs_.data(); }
private:
    std::vector<std::string> storage_;
    std::vector<char*>        ptrs_;
};

// Count rows by re-opening the DB after the CLI has dropped its handle.
struct DbCounts {
    int sessions = -1;
    int turns    = -1;
};
DbCounts count_rows(const fs::path& p) {
    DbCounts c{};
    auto r = mem::Database::open(p);
    if (!std::holds_alternative<mem::Database>(r)) return c;
    auto db = std::move(std::get<mem::Database>(r));
    mem::Repository repo(db);
    auto s = repo.all_sessions(1000);
    if (auto* rows = std::get_if<std::vector<mem::SessionRow>>(&s)) {
        c.sessions = static_cast<int>(rows->size());
    }
    auto t = repo.all_turns(1000);
    if (auto* rows = std::get_if<std::vector<mem::TurnRow>>(&t)) {
        c.turns = static_cast<int>(rows->size());
    }
    return c;
}

} // namespace

TEST_CASE("memory_cli: sessions lists rows in a tabular layout") {
    auto p = tmp_db("sessions");
    seed(p);

    std::string captured;
    int rc = 0;
    {
        CaptureStdout cap;
        ArgvBuilder argv{"sessions", "--db", p.string()};
        rc = acva::cli::run_memory_subcommand(argv.argc(), argv.argv());
        captured = cap.read();
    }
    CHECK(rc == 0);
    CHECK(captured.find("started_at") != std::string::npos);
    CHECK(captured.find("title") != std::string::npos);
}

TEST_CASE("memory_cli: sessions --json emits one object per line") {
    auto p = tmp_db("sessions-json");
    seed(p);

    std::string captured;
    {
        CaptureStdout cap;
        ArgvBuilder argv{"sessions", "--json", "--db", p.string()};
        CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);
        captured = cap.read();
    }
    CHECK(captured.starts_with("{"));
    CHECK(captured.find(R"("id":1)") != std::string::npos);
    CHECK(captured.find(R"("started_at")") != std::string::npos);
}

TEST_CASE("memory_cli: turn <id> shows the full text") {
    auto p = tmp_db("turn-show");
    auto seeded = seed(p);

    std::string captured;
    {
        CaptureStdout cap;
        ArgvBuilder argv{"turn", std::to_string(seeded.user_turn),
                          "--db", p.string()};
        CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);
        captured = cap.read();
    }
    CHECK(captured.find("hello") != std::string::npos);
    CHECK(captured.find("user") != std::string::npos);
}

TEST_CASE("memory_cli: turn <missing> returns 1") {
    auto p = tmp_db("turn-miss");
    seed(p);
    ArgvBuilder argv{"turn", "99999", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 1);
}

TEST_CASE("memory_cli: delete-turn --dry-run leaves rows intact") {
    auto p = tmp_db("dt-dry");
    auto seeded = seed(p);
    const auto before = count_rows(p);

    ArgvBuilder argv{"delete-turn", std::to_string(seeded.user_turn),
                      "--dry-run", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);

    const auto after = count_rows(p);
    CHECK(before.turns == after.turns);
}

TEST_CASE("memory_cli: delete-turn applies the removal") {
    auto p = tmp_db("dt-apply");
    auto seeded = seed(p);

    ArgvBuilder argv{"delete-turn", std::to_string(seeded.user_turn),
                      "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);

    const auto after = count_rows(p);
    CHECK(after.turns == 1);
}

TEST_CASE("memory_cli: delete-session cascades to turns") {
    auto p = tmp_db("ds-cascade");
    auto seeded = seed(p);

    ArgvBuilder argv{"delete-session", std::to_string(seeded.session),
                      "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);

    const auto after = count_rows(p);
    CHECK(after.sessions == 0);
    CHECK(after.turns == 0);
}

TEST_CASE("memory_cli: wipe without --yes refuses") {
    auto p = tmp_db("wipe-noyes");
    seed(p);
    ArgvBuilder argv{"wipe", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 1);

    const auto after = count_rows(p);
    CHECK(after.sessions == 1);  // unchanged
}

TEST_CASE("memory_cli: wipe --yes drops everything") {
    auto p = tmp_db("wipe-yes");
    seed(p);
    ArgvBuilder argv{"wipe", "--yes", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);

    const auto after = count_rows(p);
    CHECK(after.sessions == 0);
    CHECK(after.turns == 0);
}

TEST_CASE("memory_cli: vacuum runs cleanly") {
    auto p = tmp_db("vacuum");
    seed(p);
    ArgvBuilder argv{"vacuum", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 0);
}

TEST_CASE("memory_cli: unknown subcommand returns 1") {
    auto p = tmp_db("unknown");
    seed(p);
    ArgvBuilder argv{"frobnicate", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 1);
}

TEST_CASE("memory_cli: missing DB returns 2 (system error)") {
    auto p = tmp_db("missing");
    fs::remove(p);
    ArgvBuilder argv{"sessions", "--db", p.string()};
    CHECK(acva::cli::run_memory_subcommand(argv.argc(), argv.argv()) == 2);
}
