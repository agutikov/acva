#include "config/config.hpp"
#include "demos/demo.hpp"
#include "dialogue/session.hpp"
#include "http/server.hpp"
#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"
#include "metrics/registry.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>

namespace acva::demos {

namespace {

namespace fs = std::filesystem;

constexpr int kDemoPort = 19877;

struct Counts {
    std::int64_t sessions  = -1;
    std::int64_t turns     = -1;
    std::int64_t summaries = -1;
};

Counts read_counts(memory::MemoryThread& m) {
    return m.read([](memory::Repository& repo) {
        Counts c{};
        if (auto s = repo.sessions_open();
            auto* rows = std::get_if<std::vector<memory::SessionRow>>(&s)) {
            c.sessions = static_cast<std::int64_t>(rows->size());
        }
        // Sum turns across all open sessions. We use sessions_open as
        // the truth-source for which sessions exist; iterating their
        // turns gives an exact count for the demo.
        c.turns = 0;
        if (auto s = repo.sessions_open();
            auto* rows = std::get_if<std::vector<memory::SessionRow>>(&s)) {
            for (const auto& sr : *rows) {
                if (auto tr = repo.recent_turns(sr.id, 1000);
                    auto* trs = std::get_if<std::vector<memory::TurnRow>>(&tr)) {
                    c.turns += static_cast<std::int64_t>(trs->size());
                }
            }
        }
        if (auto s = repo.all_summaries();
            auto* rows = std::get_if<std::vector<memory::SummaryRow>>(&s)) {
            c.summaries = static_cast<std::int64_t>(rows->size());
        }
        return c;
    });
}

} // namespace

int run_wipe(const config::Config& /*outer*/, std::span<const std::string> /*args*/) {
    auto path = fs::temp_directory_path()
              / (std::string{"acva-demo-wipe-"}
                 + std::to_string(::getpid()) + ".db");
    fs::remove(path);
    fs::remove(fs::path(path.string() + "-wal"));
    fs::remove(fs::path(path.string() + "-shm"));
    std::printf("demo[wipe] tmp db=%s\n", path.c_str());

    auto mem_or = memory::MemoryThread::open(path.string(), 64);
    if (auto* err = std::get_if<memory::DbError>(&mem_or)) {
        std::fprintf(stderr, "demo[wipe] FAIL: open db: %s\n", err->message.c_str());
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<memory::MemoryThread>>(mem_or));

    dialogue::SessionManager sessions(*memory);
    auto sid_or = sessions.open_initial();
    if (auto* err = std::get_if<memory::DbError>(&sid_or)) {
        std::fprintf(stderr, "demo[wipe] FAIL: open session: %s\n", err->message.c_str());
        return EXIT_FAILURE;
    }
    const auto active_session = std::get<memory::SessionId>(sid_or);

    // Insert 3 turns into the active session.
    for (int i = 0; i < 3; ++i) {
        auto err = memory->read([active_session](memory::Repository& repo) {
            return repo.insert_turn(active_session, memory::TurnRole::User,
                                     std::string("hi"), std::string("en"),
                                     memory::now_ms(),
                                     memory::TurnStatus::Committed);
        });
        if (std::holds_alternative<memory::DbError>(err)) {
            std::fprintf(stderr, "demo[wipe] FAIL: insert turn\n");
            return EXIT_FAILURE;
        }
    }
    {
        auto c = read_counts(*memory);
        std::printf("  step 1: 1 session + 3 turns inserted "
                    "→ sessions=%lld turns=%lld summaries=%lld\n",
                    static_cast<long long>(c.sessions),
                    static_cast<long long>(c.turns),
                    static_cast<long long>(c.summaries));
        if (c.sessions != 1 || c.turns != 3 || c.summaries != 0) {
            std::fprintf(stderr, "  FAIL: expected sessions=1 turns=3 summaries=0\n");
            return EXIT_FAILURE;
        }
    }

    // ----- HTTP scaffold -----
    config::Config cfg{};
    cfg.control.bind = "127.0.0.1";
    cfg.control.port = kDemoPort;

    http::ControlServer::PrivacyHandlers privacy{};
    privacy.new_session = [&sessions]() -> std::variant<std::int64_t, std::string> {
        auto r = sessions.roll_over();
        if (auto* err = std::get_if<memory::DbError>(&r)) return err->message;
        return std::get<memory::SessionId>(r);
    };
    privacy.wipe_session = [&sessions](std::int64_t id) -> std::optional<std::string> {
        auto err = sessions.wipe_session(id);
        if (err.has_value()) return err->message;
        return std::nullopt;
    };
    privacy.wipe_all = [&sessions]() -> std::variant<std::int64_t, std::string> {
        auto r = sessions.wipe_all();
        if (auto* err = std::get_if<memory::DbError>(&r)) return err->message;
        return std::get<memory::SessionId>(r);
    };

    auto registry = std::make_shared<metrics::Registry>();
    std::unique_ptr<http::ControlServer> server;
    try {
        server = std::make_unique<http::ControlServer>(
            cfg.control, registry, /*fsm*/ nullptr,
            http::ControlServer::StatusExtra{},
            http::ControlServer::ReloadHandler{},
            std::move(privacy));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "demo[wipe] FAIL: bind: %s\n", ex.what());
        fs::remove(path);
        return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client("127.0.0.1", kDemoPort);
    client.set_read_timeout(5);

    int failures = 0;

    // ----- step 2: /wipe?session=<active> -----
    {
        std::string url = std::string{"/wipe?session="}
                        + std::to_string(active_session);
        auto res = client.Post(url, "", "application/json");
        if (!res || res->status != 200) {
            std::fprintf(stderr,
                "  step 2: FAIL: POST /wipe?session=%lld returned %d\n",
                static_cast<long long>(active_session),
                res ? res->status : 0);
            ++failures;
        }
        // Wiping the active session rolls over to a fresh one; turns
        // and summaries should be empty, exactly one session live.
        auto c = read_counts(*memory);
        std::printf("  step 2: POST /wipe?session=%lld → "
                    "sessions=%lld turns=%lld summaries=%lld\n",
                    static_cast<long long>(active_session),
                    static_cast<long long>(c.sessions),
                    static_cast<long long>(c.turns),
                    static_cast<long long>(c.summaries));
        if (c.sessions != 1 || c.turns != 0 || c.summaries != 0) {
            std::fprintf(stderr, "    FAIL: expected sessions=1 turns=0\n");
            ++failures;
        }
        if (sessions.id() == active_session) {
            std::fprintf(stderr, "    FAIL: SessionManager did not roll over\n");
            ++failures;
        }
    }

    // Repopulate one turn so step 3's "all gone" check has something to clear.
    auto current = sessions.id();
    (void)memory->read([current](memory::Repository& repo) {
        return repo.insert_turn(current, memory::TurnRole::User,
                                 std::string("y"), std::string("en"),
                                 memory::now_ms(),
                                 memory::TurnStatus::Committed);
    });

    // ----- step 3: /wipe?all=true -----
    {
        auto res = client.Post("/wipe?all=true", "", "application/json");
        if (!res || res->status != 200) {
            std::fprintf(stderr,
                "  step 3: FAIL: POST /wipe?all=true returned %d\n",
                res ? res->status : 0);
            ++failures;
        }
        auto c = read_counts(*memory);
        // wipe_all opens a fresh session, so 1 session + 0 turns + 0
        // summaries — the user-data tables (turns/summaries) are
        // empty, which is the spirit of the plan's "rows=0" assertion.
        std::printf("  step 3: POST /wipe?all=true → "
                    "sessions=%lld turns=%lld summaries=%lld\n",
                    static_cast<long long>(c.sessions),
                    static_cast<long long>(c.turns),
                    static_cast<long long>(c.summaries));
        if (c.sessions != 1 || c.turns != 0 || c.summaries != 0) {
            std::fprintf(stderr,
                "    FAIL: expected sessions=1 turns=0 summaries=0 after wipe-all\n");
            ++failures;
        }
    }

    // ----- step 4: invalid /wipe (no qualifier) -----
    {
        auto res = client.Post("/wipe", "", "application/json");
        if (!res || res->status != 400) {
            std::fprintf(stderr,
                "  step 4: FAIL: POST /wipe (no qualifier) returned %d, expected 400\n",
                res ? res->status : 0);
            ++failures;
        } else {
            std::printf("  step 4: POST /wipe (no qualifier) → 400 (expected) ✓\n");
        }
    }

    server.reset();
    fs::remove(path);
    fs::remove(fs::path(path.string() + "-wal"));
    fs::remove(fs::path(path.string() + "-shm"));

    if (failures > 0) {
        std::printf("demo[wipe] FAIL (%d step(s) failed)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("demo[wipe] PASS\n");
    return EXIT_SUCCESS;
}

} // namespace acva::demos
