#include "observability/speaches_wedge.hpp"

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>

using acva::observability::ComputeApp;
using acva::observability::classify_speaches;
using acva::observability::parse_compute_apps;

namespace {

// Build a CmdlineLookup that resolves a fixed map (PID → cmdline)
// and returns "" for missing PIDs (mirrors /proc behaviour on a
// dead process).
auto fixed_lookup(std::unordered_map<int, std::string> m) {
    return [m = std::move(m)](int pid) -> std::string {
        auto it = m.find(pid);
        return it == m.end() ? std::string{} : it->second;
    };
}

} // namespace

TEST_CASE("speaches_wedge: parse_compute_apps reads two-column CSV") {
    const auto rows = parse_compute_apps(
        "1234, 1190\n"
        "5678, 4500\n");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].pid == 1234);
    CHECK(rows[0].used_mib == 1190);
    CHECK(rows[1].pid == 5678);
    CHECK(rows[1].used_mib == 4500);
}

TEST_CASE("speaches_wedge: parse_compute_apps tolerates whitespace + blanks") {
    const auto rows = parse_compute_apps(
        "  9999 ,  100  \n"
        "\n"
        "   \n"
        "1234,200\n");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].pid == 9999);
    CHECK(rows[1].pid == 1234);
}

TEST_CASE("speaches_wedge: parse_compute_apps rejects malformed lines") {
    const auto rows = parse_compute_apps(
        "garbage\n"
        "1234,200\n"
        "5678 (no comma)\n");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].pid == 1234);
}

TEST_CASE("speaches_wedge: classify finds the speaches process") {
    const ComputeApp apps[] = {
        {.pid = 1234, .used_mib = 4500},  // llama-server, no match
        {.pid = 5678, .used_mib = 1190},  // speaches, under threshold
    };
    auto lookup = fixed_lookup({
        {1234, "/usr/bin/llama-server --model /models/foo.gguf"},
        {5678, "python -m speaches.main"},
    });
    auto p = classify_speaches(apps, lookup, /*threshold_mib*/ 2000);
    CHECK(p.pid == 5678);
    CHECK(p.used_mib == 1190);
    CHECK_FALSE(p.wedged);
}

TEST_CASE("speaches_wedge: classify flags wedged when over threshold") {
    const ComputeApp apps[] = {
        {.pid = 5678, .used_mib = 2600},
    };
    auto lookup = fixed_lookup({
        {5678, "uvicorn speaches.main:app --host 0.0.0.0"},
    });
    auto p = classify_speaches(apps, lookup, 2000);
    CHECK(p.pid == 5678);
    CHECK(p.used_mib == 2600);
    CHECK(p.wedged);
}

TEST_CASE("speaches_wedge: classify ignores non-speaches PIDs over threshold") {
    const ComputeApp apps[] = {
        {.pid = 1234, .used_mib = 7000},  // a big llama job, not speaches
    };
    auto lookup = fixed_lookup({
        {1234, "/usr/bin/llama-server --model big.gguf"},
    });
    auto p = classify_speaches(apps, lookup, 2000);
    CHECK(p.pid == -1);
    CHECK(p.used_mib == 0);
    CHECK_FALSE(p.wedged);
}

TEST_CASE("speaches_wedge: classify returns no-match on empty input") {
    auto p = classify_speaches({}, fixed_lookup({}), 2000);
    CHECK(p.pid == -1);
    CHECK(p.used_mib == 0);
    CHECK_FALSE(p.wedged);
}

TEST_CASE("speaches_wedge: classify treats threshold as inclusive") {
    const ComputeApp apps[] = {
        {.pid = 5678, .used_mib = 2000},  // exactly at threshold
    };
    auto lookup = fixed_lookup({{5678, "speaches"}});
    auto p = classify_speaches(apps, lookup, 2000);
    CHECK(p.wedged);
}

TEST_CASE("speaches_wedge: classify picks first speaches match in order") {
    const ComputeApp apps[] = {
        {.pid = 1111, .used_mib = 100},   // not speaches, comes first
        {.pid = 2222, .used_mib = 1500},  // speaches, second
        {.pid = 3333, .used_mib = 4000},  // also speaches, third
    };
    auto lookup = fixed_lookup({
        {1111, "/usr/bin/python /unrelated.py"},
        {2222, "uvicorn speaches.main:app"},
        {3333, "speaches-secondary"},
    });
    auto p = classify_speaches(apps, lookup, 2000);
    CHECK(p.pid == 2222);
    CHECK(p.used_mib == 1500);
    CHECK_FALSE(p.wedged);
}
