#include "config/config.hpp"
#include "config/reload.hpp"
#include "demos/demo.hpp"
#include "http/server.hpp"
#include "metrics/registry.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

namespace acva::demos {

namespace {

namespace fs = std::filesystem;

// Picked a high non-default port so the demo doesn't collide with a
// real `acva` instance bound on 9876. The demo binds the same port for
// both ControlServer and the reload-time parse, so the diff doesn't
// flag control.port as restart-required.
constexpr int kDemoPort = 19876;

constexpr const char* kBaseYaml = R"(
logging:
  level: info
control:
  bind: 127.0.0.1
  port: 19876
llm:
  base_url: http://127.0.0.1:8081/v1
  model: dialog
  temperature: 0.7
  max_tokens: 400
dialogue:
  max_assistant_sentences: 6
vad:
  onset_threshold: 0.5
  offset_threshold: 0.35
  hangover_ms: 600
tts:
  tempo_wpm: 200
)";

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    f << content;
}

bool body_contains(const std::string& body, std::string_view needle) {
    return body.find(needle) != std::string::npos;
}

} // namespace

int run_reload(const config::Config& /*outer*/, std::span<const std::string> /*args*/) {
    // The demo intentionally builds its own ControlServer + Config so
    // `acva demo reload` is self-contained and doesn't poke at whatever
    // services the user has running.
    //
    // We bind the control plane on port 0 (kernel-chosen) and pull the
    // actual port back via ControlServer::port(). The cfg passed to the
    // server still says `port: 9876` (the parser default) — that's the
    // value Config carries; the bound port comes from the local port-0
    // override we plug into ControlConfig below.
    auto path = fs::temp_directory_path()
              / (std::string{"acva-demo-reload-"}
                 + std::to_string(::getpid()) + ".yaml");
    write_file(path, kBaseYaml);

    auto loaded = config::load_from_file(path);
    if (auto* err = std::get_if<config::LoadError>(&loaded)) {
        std::fprintf(stderr, "demo[reload] FAIL: base YAML failed to parse: %s\n",
                     err->message.c_str());
        fs::remove(path);
        return EXIT_FAILURE;
    }
    auto cfg = std::get<config::Config>(std::move(loaded));

    config::ConfigReloader reloader(cfg, path);
    std::mutex reload_mtx;

    auto run_reload = [&reloader, &reload_mtx]() {
        std::lock_guard lk(reload_mtx);
        return reloader.reload();
    };

    // Minimal scaffolding for ControlServer — it requires a registry
    // and (optionally) an Fsm. We build the bare pieces needed to bring
    // /reload up; /metrics + /status are also live but we don't query
    // them.
    auto registry = std::make_shared<metrics::Registry>();

    std::unique_ptr<http::ControlServer> server;
    try {
        server = std::make_unique<http::ControlServer>(
            cfg.control, registry, /*fsm*/ nullptr,
            http::ControlServer::StatusExtra{}, run_reload);
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
            "demo[reload] FAIL: control server failed to bind: %s\n",
            ex.what());
        fs::remove(path);
        return EXIT_FAILURE;
    }
    const int port = kDemoPort;
    (void)server->port();

    // Give the listener a beat to come up.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(5);

    int failures = 0;

    auto post_reload = [&](std::string_view step) {
        std::printf("  step: %s\n", std::string(step).c_str());
        const auto t0 = std::chrono::steady_clock::now();
        auto res = client.Post("/reload", "", "application/json");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!res) {
            std::fprintf(stderr, "    FAIL: POST /reload returned no response\n");
            ++failures;
            return std::pair<int, std::string>{0, std::string{}};
        }
        std::printf("    -> HTTP %d  (%lld ms)  body=%s\n",
                    res->status, static_cast<long long>(ms), res->body.c_str());
        return std::pair<int, std::string>{res->status, res->body};
    };

    // Step 1 — same content twice → 200 + empty applied list.
    {
        auto [code, body] = post_reload("baseline (no change)");
        if (code != 200 || !body_contains(body, "\"applied\":[]")) {
            std::fprintf(stderr,
                "    FAIL: expected 200 + empty applied; got %d\n", code);
            ++failures;
        }
    }

    // Step 2 — flip logging.level.
    write_file(path, std::string(kBaseYaml) + "logging:\n  level: debug\n");
    {
        auto [code, body] = post_reload("hot-reload logging.level info → debug");
        if (code != 200 || !body_contains(body, "logging.level")) {
            std::fprintf(stderr,
                "    FAIL: expected 200 with logging.level applied; got %d\n", code);
            ++failures;
        }
        if (cfg.logging.level != "debug") {
            std::fprintf(stderr,
                "    FAIL: live cfg.logging.level = '%s', expected 'debug'\n",
                cfg.logging.level.c_str());
            ++failures;
        }
    }

    // Step 3 — restart-required field rejected (409, live cfg untouched).
    write_file(path, std::string(kBaseYaml)
        + "logging:\n  level: debug\n"
        + "memory:\n  db_path: /tmp/other.db\n");
    {
        auto [code, body] = post_reload("restart-required memory.db_path");
        if (code != 409
            || !body_contains(body, "memory.db_path")
            || !body_contains(body, "rejected")) {
            std::fprintf(stderr,
                "    FAIL: expected 409 + restart_required; got %d\n", code);
            ++failures;
        }
        // live cfg.memory.db_path stays empty (the base YAML didn't set it).
        if (!cfg.memory.db_path.empty()
            && cfg.memory.db_path == "/tmp/other.db") {
            std::fprintf(stderr,
                "    FAIL: rejected reload still mutated live db_path to '%s'\n",
                cfg.memory.db_path.c_str());
            ++failures;
        }
    }

    // Step 4 — malformed YAML returns 400.
    write_file(path, "logging: { level: info\ncontrol: { port: ");
    {
        auto [code, body] = post_reload("malformed YAML");
        if (code != 400 || !body_contains(body, "parse_error")) {
            std::fprintf(stderr,
                "    FAIL: expected 400 + parse_error; got %d\n", code);
            ++failures;
        }
    }

    fs::remove(path);
    server.reset();

    if (failures > 0) {
        std::printf("demo[reload] FAIL (%d step(s) failed)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("demo[reload] PASS\n");
    return EXIT_SUCCESS;
}

} // namespace acva::demos
