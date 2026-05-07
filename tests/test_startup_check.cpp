#include "config/config.hpp"
#include "supervisor/startup_check.hpp"

#include <doctest/doctest.h>

#include <httplib.h>

#include <functional>
#include <string>
#include <thread>

namespace cfgns = acva::config;
namespace sup = acva::supervisor;
using namespace std::chrono_literals;

namespace {

// Starts an httplib server bound to 127.0.0.1:0 (kernel-chosen port)
// in a background thread. RAII: stops the server when destroyed.
class FakeServer {
public:
    using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

    FakeServer() {
        // bind_to_any_port returns the actual port.
        port_ = server_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        // Tiny delay so the listener is ready.
        std::this_thread::sleep_for(20ms);
    }
    ~FakeServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] int port() const noexcept { return port_; }

    void on_post(const std::string& path, Handler h) {
        server_.Post(path, std::move(h));
    }
    void on_get(const std::string& path, Handler h) {
        server_.Get(path, std::move(h));
    }

private:
    httplib::Server server_;
    std::thread     thread_;
    int             port_ = 0;
};

cfgns::Config make_cfg(int llm_port, int stt_port, int tts_port) {
    cfgns::Config cfg{};
    cfg.supervisor.startup_force_load = true;
    if (llm_port > 0) {
        cfg.llm.base_url = "http://127.0.0.1:" + std::to_string(llm_port) + "/v1";
        cfg.llm.model = "test-model";
    }
    if (stt_port > 0) {
        cfg.stt.base_url = "http://127.0.0.1:" + std::to_string(stt_port);
        cfg.stt.model = "test-stt";
    }
    if (tts_port > 0) {
        cfg.tts.base_url = "http://127.0.0.1:" + std::to_string(tts_port);
        cfgns::TtsVoice v;
        v.model_id = "test-tts-model";
        v.voice_id = "default";
        cfg.tts.voices_resolved.emplace("en", v);
        cfg.tts.fallback_lang = "en";
    }
    cfg.audio.capture_enabled = false;  // tests don't open the mic
    return cfg;
}

} // namespace

TEST_CASE("startup_check: skipped entirely when force_load is off") {
    cfgns::Config cfg = make_cfg(0, 0, 0);
    cfg.supervisor.startup_force_load = false;
    cfg.llm.base_url = "http://127.0.0.1:1";  // unreachable; should be ignored
    auto failures = sup::run_startup_checks(cfg);
    CHECK(failures.empty());
}

TEST_CASE("startup_check: passes when all probed backends respond 200") {
    FakeServer llm;
    llm.on_post("/v1/chat/completions",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(R"({"choices":[{"message":{"content":"ok"}}]})",
                             "application/json");
        });

    cfgns::Config cfg = make_cfg(llm.port(), 0, 0);
    auto failures = sup::run_startup_checks(cfg);
    CHECK(failures.empty());
}

TEST_CASE("startup_check: collects llm failure on HTTP 500") {
    FakeServer llm;
    llm.on_post("/v1/chat/completions",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
            res.set_content("model not found", "text/plain");
        });

    cfgns::Config cfg = make_cfg(llm.port(), 0, 0);
    auto failures = sup::run_startup_checks(cfg);
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].component == "llm");
    CHECK(failures[0].error.find("HTTP 500") != std::string::npos);
    CHECK_FALSE(failures[0].remediation.empty());
}

TEST_CASE("startup_check: llm failure when nothing listens at the port") {
    cfgns::Config cfg = make_cfg(/*llm*/ 1, /*stt*/ 0, /*tts*/ 0);
    auto failures = sup::run_startup_checks(cfg);
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].component == "llm");
}

TEST_CASE("startup_check: tts failure surfaces voice details") {
    FakeServer tts;
    tts.on_post("/v1/audio/speech",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 404;
            res.set_content("voice not found", "text/plain");
        });

    cfgns::Config cfg = make_cfg(0, 0, tts.port());
    auto failures = sup::run_startup_checks(cfg);
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].component == "tts");
}

TEST_CASE("startup_check: combined failures returned together") {
    cfgns::Config cfg{};
    cfg.supervisor.startup_force_load = true;
    cfg.llm.base_url = "http://127.0.0.1:1";   // unreachable
    cfg.tts.base_url = "http://127.0.0.1:1";
    cfgns::TtsVoice v;
    v.model_id = "x"; v.voice_id = "y";
    cfg.tts.voices_resolved.emplace("en", v);
    cfg.tts.fallback_lang = "en";
    auto failures = sup::run_startup_checks(cfg);
    REQUIRE(failures.size() == 2);
    bool saw_llm = false, saw_tts = false;
    for (const auto& f : failures) {
        if (f.component == "llm") saw_llm = true;
        if (f.component == "tts") saw_tts = true;
    }
    CHECK(saw_llm);
    CHECK(saw_tts);
}
