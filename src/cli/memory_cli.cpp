#include "cli/memory_cli.hpp"

#include "config/config.hpp"
#include "config/paths.hpp"
#include "memory/db.hpp"
#include "memory/repository.hpp"

#include <httplib.h>

#include <fmt/format.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace acva::cli {

namespace fs = std::filesystem;

namespace {

// ----- argument parsing -----

struct Options {
    std::optional<fs::path> config_path;
    std::optional<fs::path> db_path;
    std::optional<std::int64_t> session_filter;
    int  limit    = 50;
    bool json     = false;
    bool yes      = false;
    bool dry_run  = false;
    bool show_turns = false;
    // M8A Step 4 — `acva memory restart` HTTP target.
    std::string host = "127.0.0.1";
    int         port = 9876;
    std::vector<std::string> positional;  // non-flag args, e.g. <id>
};

[[nodiscard]] bool parse_int(std::string_view s, std::int64_t& out) {
    try {
        std::size_t consumed = 0;
        out = std::stoll(std::string(s), &consumed);
        return consumed == s.size();
    } catch (...) {
        return false;
    }
}

// Returns true on success, false if a value flag was missing its arg.
bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 0; i < argc; ++i) {
        std::string_view a{argv[i]};
        auto need_value = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "acva memory: %s requires a value\n",
                             std::string(name).c_str());
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--json") {
            out.json = true;
        } else if (a == "--yes") {
            out.yes = true;
        } else if (a == "--dry-run") {
            out.dry_run = true;
        } else if (a == "--turns") {
            out.show_turns = true;
        } else if (a == "--limit") {
            const char* v = need_value("--limit");
            if (!v) return false;
            std::int64_t n = 0;
            if (!parse_int(v, n) || n <= 0) {
                std::fprintf(stderr, "acva memory: --limit must be a positive integer\n");
                return false;
            }
            out.limit = static_cast<int>(n);
        } else if (a == "--session") {
            const char* v = need_value("--session");
            if (!v) return false;
            std::int64_t n = 0;
            if (!parse_int(v, n)) {
                std::fprintf(stderr, "acva memory: --session must be an integer\n");
                return false;
            }
            out.session_filter = n;
        } else if (a == "--config") {
            const char* v = need_value("--config");
            if (!v) return false;
            out.config_path = v;
        } else if (a.starts_with("--config=")) {
            out.config_path = std::string(a.substr(9));
        } else if (a == "--db") {
            const char* v = need_value("--db");
            if (!v) return false;
            out.db_path = v;
        } else if (a.starts_with("--db=")) {
            out.db_path = std::string(a.substr(5));
        } else if (a == "--host") {
            const char* v = need_value("--host");
            if (!v) return false;
            out.host = v;
        } else if (a == "--port") {
            const char* v = need_value("--port");
            if (!v) return false;
            std::int64_t n = 0;
            if (!parse_int(v, n) || n <= 0 || n > 65535) {
                std::fprintf(stderr, "acva memory: --port must be 1..65535\n");
                return false;
            }
            out.port = static_cast<int>(n);
        } else if (a == "-h" || a == "--help") {
            print_memory_help();
            std::exit(EXIT_SUCCESS);
        } else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "acva memory: unknown flag '%s'\n",
                         std::string(a).c_str());
            return false;
        } else {
            out.positional.emplace_back(a);
        }
    }
    return true;
}

// ----- DB resolution -----

// Resolve the DB path the same way `acva` itself does:
//   1. --db (explicit override) — used as-is.
//   2. --config PATH → load that YAML, take cfg.memory.db_path.
//   3. Default: load_from_file(resolve_config_path()) → cfg.memory.db_path.
//   4. Fall back to the XDG default ($XDG_DATA_HOME/acva/acva.db).
[[nodiscard]] std::optional<fs::path>
resolve_db_path(const Options& opts) {
    if (opts.db_path) return *opts.db_path;

    fs::path cfg_path;
    if (opts.config_path) {
        cfg_path = *opts.config_path;
    } else {
        auto auto_resolved = config::resolve_config_path({});
        if (auto* p = std::get_if<fs::path>(&auto_resolved)) {
            cfg_path = *p;
        } else {
            // No config on disk — fall back to the default DB path.
            return config::resolve_data_path({}, "acva.db");
        }
    }

    auto loaded = config::load_from_file(cfg_path);
    if (auto* err = std::get_if<config::LoadError>(&loaded)) {
        std::fprintf(stderr,
            "acva memory: failed to parse %s: %s\n",
            cfg_path.c_str(), err->message.c_str());
        return std::nullopt;
    }
    auto cfg = std::get<config::Config>(std::move(loaded));
    return config::resolve_data_path(cfg.memory.db_path, "acva.db");
}

[[nodiscard]] std::optional<memory::Database>
open_db_or_die(const Options& opts) {
    auto p_opt = resolve_db_path(opts);
    if (!p_opt) return std::nullopt;
    if (!fs::exists(*p_opt)) {
        std::fprintf(stderr,
            "acva memory: database not found at %s\n", p_opt->c_str());
        return std::nullopt;
    }
    auto r = memory::Database::open(*p_opt);
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr,
            "acva memory: open %s failed: %s\n",
            p_opt->c_str(), err->message.c_str());
        return std::nullopt;
    }
    return std::move(std::get<memory::Database>(r));
}

// ----- formatting -----

std::string format_unix_ms(memory::UnixMs t) {
    if (t == 0) return "-";
    using namespace std::chrono;
    const auto sec = static_cast<std::time_t>(t / 1000);
    std::tm tm{};
    ::localtime_r(&sec, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string ellipsise(std::string_view s, std::size_t n) {
    if (s.size() <= n) return std::string(s);
    return std::string(s.substr(0, n)) + "…";
}

// JSON-escape minimal — keys + values pass-through ASCII; control
// characters get \uXXXX. Used for the --json output mode.
std::string json_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out.append(fmt::format("\\u{:04x}",
                                           static_cast<unsigned char>(c)));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string json_quote_opt(const std::optional<std::string>& s) {
    if (!s.has_value()) return "null";
    return json_quote(*s);
}

std::string json_int_opt(const std::optional<std::int64_t>& v) {
    if (!v.has_value()) return "null";
    return std::to_string(*v);
}

// ----- subcommands -----

int cmd_sessions(memory::Repository& repo, const Options& opts) {
    auto r = repo.all_sessions(opts.limit);
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr, "acva memory sessions: %s\n", err->message.c_str());
        return 2;
    }
    const auto& rows = std::get<std::vector<memory::SessionRow>>(r);
    if (opts.json) {
        for (const auto& s : rows) {
            std::printf(
                R"({"id":%lld,"started_at":%lld,"ended_at":%s,"title":%s})" "\n",
                static_cast<long long>(s.id),
                static_cast<long long>(s.started_at),
                json_int_opt(s.ended_at).c_str(),
                json_quote_opt(s.title).c_str());
        }
        return 0;
    }
    std::printf("%-6s %-19s %-19s %s\n", "id", "started_at", "ended_at", "title");
    for (const auto& s : rows) {
        std::printf("%-6lld %-19s %-19s %s\n",
                    static_cast<long long>(s.id),
                    format_unix_ms(s.started_at).c_str(),
                    s.ended_at ? format_unix_ms(*s.ended_at).c_str() : "-",
                    s.title.value_or("").c_str());
    }
    return 0;
}

int cmd_session(memory::Repository& repo, const Options& opts) {
    if (opts.positional.empty()) {
        std::fprintf(stderr, "acva memory session: missing <id>\n");
        return 1;
    }
    std::int64_t id = 0;
    if (!parse_int(opts.positional.front(), id)) {
        std::fprintf(stderr, "acva memory session: <id> must be an integer\n");
        return 1;
    }
    auto r = repo.get_session(id);
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr, "acva memory session: %s\n", err->message.c_str());
        return 2;
    }
    auto& opt = std::get<std::optional<memory::SessionRow>>(r);
    if (!opt.has_value()) {
        std::fprintf(stderr, "acva memory session: id %lld not found\n",
                     static_cast<long long>(id));
        return 1;
    }
    const auto& s = *opt;
    if (opts.json) {
        std::printf(
            R"({"id":%lld,"started_at":%lld,"ended_at":%s,"title":%s})" "\n",
            static_cast<long long>(s.id),
            static_cast<long long>(s.started_at),
            json_int_opt(s.ended_at).c_str(),
            json_quote_opt(s.title).c_str());
    } else {
        std::printf("session %lld\n", static_cast<long long>(s.id));
        std::printf("  started_at  %s\n", format_unix_ms(s.started_at).c_str());
        std::printf("  ended_at    %s\n",
                    s.ended_at ? format_unix_ms(*s.ended_at).c_str() : "-");
        std::printf("  title       %s\n", s.title.value_or("-").c_str());
    }
    if (!opts.show_turns) return 0;

    auto tr = repo.recent_turns(id, opts.limit);
    if (auto* err = std::get_if<memory::DbError>(&tr)) {
        std::fprintf(stderr, "acva memory session: turns: %s\n", err->message.c_str());
        return 2;
    }
    const auto& turns = std::get<std::vector<memory::TurnRow>>(tr);
    if (opts.json) {
        for (const auto& t : turns) {
            std::printf(
                R"({"turn":%lld,"role":%s,"status":%s,"text":%s})" "\n",
                static_cast<long long>(t.id),
                json_quote(memory::to_string(t.role)).c_str(),
                json_quote(memory::to_string(t.status)).c_str(),
                json_quote_opt(t.text).c_str());
        }
    } else {
        std::printf("\nturns (oldest first):\n");
        std::printf("%-6s %-9s %-12s %s\n", "id", "role", "status", "text");
        for (const auto& t : turns) {
            std::printf("%-6lld %-9.*s %-12.*s %s\n",
                        static_cast<long long>(t.id),
                        static_cast<int>(memory::to_string(t.role).size()),
                        memory::to_string(t.role).data(),
                        static_cast<int>(memory::to_string(t.status).size()),
                        memory::to_string(t.status).data(),
                        ellipsise(t.text.value_or(""), 80).c_str());
        }
    }
    return 0;
}

int cmd_turns(memory::Repository& repo, const Options& opts) {
    memory::Result<std::vector<memory::TurnRow>> r =
        opts.session_filter
            ? repo.recent_turns(*opts.session_filter, opts.limit)
            : repo.all_turns(opts.limit);
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr, "acva memory turns: %s\n", err->message.c_str());
        return 2;
    }
    const auto& rows = std::get<std::vector<memory::TurnRow>>(r);
    if (opts.json) {
        for (const auto& t : rows) {
            std::printf(
                R"({"id":%lld,"session_id":%lld,"role":%s,"status":%s,"lang":%s,"text":%s})" "\n",
                static_cast<long long>(t.id),
                static_cast<long long>(t.session_id),
                json_quote(memory::to_string(t.role)).c_str(),
                json_quote(memory::to_string(t.status)).c_str(),
                json_quote_opt(t.lang).c_str(),
                json_quote_opt(t.text).c_str());
        }
        return 0;
    }
    std::printf("%-6s %-7s %-9s %-12s %-5s %s\n",
                "id", "session", "role", "status", "lang", "text");
    for (const auto& t : rows) {
        std::printf("%-6lld %-7lld %-9.*s %-12.*s %-5s %s\n",
                    static_cast<long long>(t.id),
                    static_cast<long long>(t.session_id),
                    static_cast<int>(memory::to_string(t.role).size()),
                    memory::to_string(t.role).data(),
                    static_cast<int>(memory::to_string(t.status).size()),
                    memory::to_string(t.status).data(),
                    t.lang.value_or("").c_str(),
                    ellipsise(t.text.value_or(""), 80).c_str());
    }
    return 0;
}

int cmd_turn(memory::Repository& repo, const Options& opts) {
    if (opts.positional.empty()) {
        std::fprintf(stderr, "acva memory turn: missing <id>\n");
        return 1;
    }
    std::int64_t id = 0;
    if (!parse_int(opts.positional.front(), id)) {
        std::fprintf(stderr, "acva memory turn: <id> must be an integer\n");
        return 1;
    }
    auto r = repo.get_turn(id);
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr, "acva memory turn: %s\n", err->message.c_str());
        return 2;
    }
    auto& opt = std::get<std::optional<memory::TurnRow>>(r);
    if (!opt.has_value()) {
        std::fprintf(stderr, "acva memory turn: id %lld not found\n",
                     static_cast<long long>(id));
        return 1;
    }
    const auto& t = *opt;
    if (opts.json) {
        std::printf(
            R"({"id":%lld,"session_id":%lld,"role":%s,"status":%s,"lang":%s,)"
            R"("started_at":%lld,"ended_at":%s,"interrupted_at_sentence":%s,)"
            R"("text":%s})" "\n",
            static_cast<long long>(t.id),
            static_cast<long long>(t.session_id),
            json_quote(memory::to_string(t.role)).c_str(),
            json_quote(memory::to_string(t.status)).c_str(),
            json_quote_opt(t.lang).c_str(),
            static_cast<long long>(t.started_at),
            json_int_opt(t.ended_at).c_str(),
            json_int_opt(t.interrupted_at_sentence).c_str(),
            json_quote_opt(t.text).c_str());
    } else {
        std::printf("turn %lld (session %lld)\n",
                    static_cast<long long>(t.id),
                    static_cast<long long>(t.session_id));
        std::printf("  role        %.*s\n",
                    static_cast<int>(memory::to_string(t.role).size()),
                    memory::to_string(t.role).data());
        std::printf("  status      %.*s\n",
                    static_cast<int>(memory::to_string(t.status).size()),
                    memory::to_string(t.status).data());
        std::printf("  lang        %s\n", t.lang.value_or("-").c_str());
        std::printf("  started_at  %s\n", format_unix_ms(t.started_at).c_str());
        std::printf("  ended_at    %s\n",
                    t.ended_at ? format_unix_ms(*t.ended_at).c_str() : "-");
        if (t.interrupted_at_sentence) {
            std::printf("  interrupted_at_sentence  %lld\n",
                        static_cast<long long>(*t.interrupted_at_sentence));
        }
        std::printf("  text:\n");
        std::printf("    %s\n", t.text.value_or("(none)").c_str());
    }
    return 0;
}

int cmd_facts(memory::Repository& repo, const Options& opts) {
    auto r = repo.facts_with_min_confidence(0.0);
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr, "acva memory facts: %s\n", err->message.c_str());
        return 2;
    }
    auto rows = std::get<std::vector<memory::FactRow>>(r);
    // --session filters in code (no SQL helper for it in v1).
    if (opts.session_filter) {
        std::vector<memory::FactRow> filtered;
        for (auto& f : rows) {
            if (f.source_turn_id) {
                auto t = repo.get_turn(*f.source_turn_id);
                if (auto* tt = std::get_if<std::optional<memory::TurnRow>>(&t);
                    tt && tt->has_value() && (*tt)->session_id == *opts.session_filter) {
                    filtered.push_back(std::move(f));
                }
            }
        }
        rows = std::move(filtered);
    }
    if (opts.json) {
        for (const auto& f : rows) {
            std::printf(
                R"({"id":%lld,"key":%s,"value":%s,"lang":%s,"source_turn_id":%s,"confidence":%.3f})" "\n",
                static_cast<long long>(f.id),
                json_quote(f.key).c_str(),
                json_quote(f.value).c_str(),
                json_quote_opt(f.lang).c_str(),
                json_int_opt(f.source_turn_id).c_str(),
                f.confidence);
        }
        return 0;
    }
    std::printf("%-6s %-20s %-5s %-7s %s\n",
                "id", "key", "lang", "conf", "value");
    for (const auto& f : rows) {
        std::printf("%-6lld %-20s %-5s %-7.3f %s\n",
                    static_cast<long long>(f.id),
                    ellipsise(f.key, 20).c_str(),
                    f.lang.value_or("").c_str(),
                    f.confidence,
                    ellipsise(f.value, 60).c_str());
    }
    return 0;
}

int cmd_summaries(memory::Repository& repo, const Options& opts) {
    memory::Result<std::vector<memory::SummaryRow>> r =
        opts.session_filter
            ? repo.summaries_by_session(*opts.session_filter)
            : repo.all_summaries();
    if (auto* err = std::get_if<memory::DbError>(&r)) {
        std::fprintf(stderr, "acva memory summaries: %s\n", err->message.c_str());
        return 2;
    }
    const auto& rows = std::get<std::vector<memory::SummaryRow>>(r);
    if (opts.json) {
        for (const auto& s : rows) {
            std::printf(
                R"({"id":%lld,"session_id":%lld,"range":[%lld,%lld],"lang":%s,"summary":%s})" "\n",
                static_cast<long long>(s.id),
                static_cast<long long>(s.session_id),
                static_cast<long long>(s.range_start_turn),
                static_cast<long long>(s.range_end_turn),
                json_quote(s.lang).c_str(),
                json_quote(s.summary).c_str());
        }
        return 0;
    }
    std::printf("%-6s %-7s %-10s %-5s %s\n",
                "id", "session", "range", "lang", "summary");
    for (const auto& s : rows) {
        std::printf("%-6lld %-7lld %-10s %-5s %s\n",
                    static_cast<long long>(s.id),
                    static_cast<long long>(s.session_id),
                    fmt::format("{}..{}", s.range_start_turn, s.range_end_turn).c_str(),
                    s.lang.c_str(),
                    ellipsise(s.summary, 80).c_str());
    }
    return 0;
}

int cmd_delete_turn(memory::Repository& repo, const Options& opts) {
    if (opts.positional.empty()) {
        std::fprintf(stderr, "acva memory delete-turn: missing <id>\n");
        return 1;
    }
    std::int64_t id = 0;
    if (!parse_int(opts.positional.front(), id)) {
        std::fprintf(stderr, "acva memory delete-turn: <id> must be an integer\n");
        return 1;
    }
    auto exists = repo.get_turn(id);
    if (auto* err = std::get_if<memory::DbError>(&exists)) {
        std::fprintf(stderr, "acva memory delete-turn: %s\n", err->message.c_str());
        return 2;
    }
    if (!std::get<std::optional<memory::TurnRow>>(exists).has_value()) {
        std::fprintf(stderr, "acva memory delete-turn: id %lld not found\n",
                     static_cast<long long>(id));
        return 1;
    }
    if (opts.dry_run) {
        std::printf("would delete 1 turn (id=%lld). Re-run without --dry-run to apply.\n",
                    static_cast<long long>(id));
        return 0;
    }
    if (auto err = repo.delete_turn(id); err.has_value()) {
        std::fprintf(stderr, "acva memory delete-turn: %s\n", err->message.c_str());
        return 2;
    }
    std::printf("deleted turn %lld\n", static_cast<long long>(id));
    return 0;
}

int cmd_delete_session(memory::Repository& repo, const Options& opts) {
    if (opts.positional.empty()) {
        std::fprintf(stderr, "acva memory delete-session: missing <id>\n");
        return 1;
    }
    std::int64_t id = 0;
    if (!parse_int(opts.positional.front(), id)) {
        std::fprintf(stderr, "acva memory delete-session: <id> must be an integer\n");
        return 1;
    }
    auto exists = repo.get_session(id);
    if (auto* err = std::get_if<memory::DbError>(&exists)) {
        std::fprintf(stderr, "acva memory delete-session: %s\n", err->message.c_str());
        return 2;
    }
    if (!std::get<std::optional<memory::SessionRow>>(exists).has_value()) {
        std::fprintf(stderr, "acva memory delete-session: id %lld not found\n",
                     static_cast<long long>(id));
        return 1;
    }
    auto turns = repo.recent_turns(id, 1000);
    int turn_count = 0;
    if (auto* rows = std::get_if<std::vector<memory::TurnRow>>(&turns)) {
        turn_count = static_cast<int>(rows->size());
    }
    if (opts.dry_run) {
        std::printf("would delete session %lld and %d cascading turn(s). "
                    "Re-run without --dry-run to apply.\n",
                    static_cast<long long>(id), turn_count);
        return 0;
    }
    if (auto err = repo.delete_session(id); err.has_value()) {
        std::fprintf(stderr, "acva memory delete-session: %s\n", err->message.c_str());
        return 2;
    }
    std::printf("deleted session %lld (%d turn(s) cascaded)\n",
                static_cast<long long>(id), turn_count);
    return 0;
}

int cmd_delete_fact(memory::Repository& repo, const Options& opts) {
    if (opts.positional.empty()) {
        std::fprintf(stderr, "acva memory delete-fact: missing <id>\n");
        return 1;
    }
    std::int64_t id = 0;
    if (!parse_int(opts.positional.front(), id)) {
        std::fprintf(stderr, "acva memory delete-fact: <id> must be an integer\n");
        return 1;
    }
    if (opts.dry_run) {
        std::printf("would delete fact id=%lld (no existence check; --dry-run skips it). "
                    "Re-run without --dry-run to apply.\n",
                    static_cast<long long>(id));
        return 0;
    }
    if (auto err = repo.delete_fact(id); err.has_value()) {
        std::fprintf(stderr, "acva memory delete-fact: %s\n", err->message.c_str());
        return 2;
    }
    std::printf("deleted fact %lld\n", static_cast<long long>(id));
    return 0;
}

int cmd_wipe(memory::Repository& repo, const Options& opts) {
    if (!opts.yes) {
        std::fprintf(stderr,
            "acva memory wipe: refusing to drop the schema without --yes "
            "(this drops every session, turn, summary, fact, and setting).\n");
        return 1;
    }
    if (auto err = repo.wipe_all(); err.has_value()) {
        std::fprintf(stderr, "acva memory wipe: %s\n", err->message.c_str());
        return 2;
    }
    std::printf("schema dropped and recreated.\n");
    return 0;
}

int cmd_restart(const Options& opts) {
    httplib::Client cli(opts.host, opts.port);
    cli.set_read_timeout(5);
    cli.set_connection_timeout(2);
    auto res = cli.Post("/restart", "", "application/json");
    if (!res) {
        std::fprintf(stderr,
            "acva memory restart: no response from %s:%d (is acva running?)\n",
            opts.host.c_str(), opts.port);
        return 2;
    }
    if (res->status == 202) {
        std::printf("restart accepted (HTTP 202)\n");
        return 0;
    }
    if (res->status == 409) {
        std::fprintf(stderr,
            "acva memory restart: rejected (HTTP 409): %s\n",
            res->body.c_str());
        return 1;
    }
    std::fprintf(stderr,
        "acva memory restart: unexpected HTTP %d: %s\n",
        res->status, res->body.c_str());
    return 2;
}

int cmd_vacuum(memory::Repository& repo, const Options& /*opts*/) {
    if (auto err = repo.vacuum(); err.has_value()) {
        std::fprintf(stderr, "acva memory vacuum: %s\n", err->message.c_str());
        return 2;
    }
    std::printf("vacuum complete.\n");
    return 0;
}

} // namespace

void print_memory_help() {
    std::fputs(
        "acva memory <subcommand> [options]\n"
        "\n"
        "  Offline access to the SQLite store. Operates on the live DB\n"
        "  but the orchestrator must NOT be running for write subcommands\n"
        "  (delete-*, wipe, vacuum) — SQLite WAL allows readers but not\n"
        "  concurrent writers across processes.\n"
        "\n"
        "Subcommands:\n"
        "  sessions       [--limit N] [--json]\n"
        "                          list sessions, newest first\n"
        "  session <id>   [--turns]\n"
        "                          show one session, optionally with its turns\n"
        "  turns          [--session ID] [--limit N] [--json]\n"
        "                          list turns, newest first\n"
        "  turn <id>               show one turn (full text, lang, status)\n"
        "  facts          [--session ID] [--json]\n"
        "                          list facts (M1 schema)\n"
        "  summaries      [--session ID] [--json]\n"
        "                          list summaries\n"
        "  delete-turn <id>     [--dry-run]\n"
        "  delete-session <id>  [--dry-run]\n"
        "                          cascade-deletes turns + summaries\n"
        "  delete-fact <id>     [--dry-run]\n"
        "  wipe                 --yes\n"
        "                          drops and recreates the schema\n"
        "  vacuum                  SQLite VACUUM to reclaim space\n"
        "  restart        [--host HOST] [--port N]\n"
        "                          POST /restart to a running orchestrator\n"
        "\n"
        "Common options:\n"
        "  --config PATH    YAML config (default: same XDG search as `acva`)\n"
        "  --db PATH        Override the DB path (skips config parsing)\n"
        "  --json           Emit one JSON object per line for jq piping\n"
        "  --yes            Required for `wipe`; no prompt\n"
        "  --dry-run        Preview a delete without applying\n"
        "  --host HOST      Orchestrator host for `restart` (default 127.0.0.1)\n"
        "  --port N         Orchestrator port for `restart` (default 9876)\n"
        "  -h, --help       Show this help and exit\n",
        stdout);
}

int run_memory_subcommand(int argc, char** argv) {
    if (argc <= 0) {
        print_memory_help();
        return 1;
    }
    const std::string sub{argv[0]};
    if (sub == "-h" || sub == "--help") {
        print_memory_help();
        return 0;
    }

    Options opts;
    if (!parse_options(argc - 1, argv + 1, opts)) {
        return 1;
    }

    // `restart` talks to a running orchestrator over HTTP — no DB
    // open required.
    if (sub == "restart") return cmd_restart(opts);

    auto db_opt = open_db_or_die(opts);
    if (!db_opt.has_value()) return 2;
    memory::Repository repo(*db_opt);

    if (sub == "sessions")        return cmd_sessions(repo, opts);
    if (sub == "session")         return cmd_session(repo, opts);
    if (sub == "turns")           return cmd_turns(repo, opts);
    if (sub == "turn")            return cmd_turn(repo, opts);
    if (sub == "facts")           return cmd_facts(repo, opts);
    if (sub == "summaries")       return cmd_summaries(repo, opts);
    if (sub == "delete-turn")     return cmd_delete_turn(repo, opts);
    if (sub == "delete-session")  return cmd_delete_session(repo, opts);
    if (sub == "delete-fact")     return cmd_delete_fact(repo, opts);
    if (sub == "wipe")            return cmd_wipe(repo, opts);
    if (sub == "vacuum")          return cmd_vacuum(repo, opts);

    std::fprintf(stderr, "acva memory: unknown subcommand '%s'\n\n", sub.c_str());
    print_memory_help();
    return 1;
}

} // namespace acva::cli
