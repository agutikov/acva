#include "llm/model_controller_client.hpp"

#include <doctest/doctest.h>

#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>
#include <variant>

using namespace std::chrono_literals;

namespace {

class FakeController {
public:
    using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

    FakeController() {
        port_ = server_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        std::this_thread::sleep_for(20ms);
    }
    ~FakeController() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] int port() const noexcept { return port_; }
    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    void on_get(const std::string& path, Handler h)  { server_.Get(path, std::move(h)); }
    void on_post(const std::string& path, Handler h) { server_.Post(path, std::move(h)); }

private:
    httplib::Server server_;
    std::thread     thread_;
    int             port_ = 0;
};

} // namespace

TEST_CASE("ModelControllerClient: status() parses healthy payload") {
    FakeController ctl;
    ctl.on_get("/llm/status",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(
                R"({"loaded_file":"foo.gguf","alias":"foo","health":"healthy"})",
                "application/json");
        });

    acva::llm::ModelControllerClient mcc(ctl.base_url());
    auto r = mcc.status();
    REQUIRE(std::holds_alternative<acva::llm::ControllerStatus>(r));
    const auto& s = std::get<acva::llm::ControllerStatus>(r);
    CHECK(s.loaded_file == "foo.gguf");
    CHECK(s.alias == "foo");
    CHECK(s.health == "healthy");
}

TEST_CASE("ModelControllerClient: status() returns ClientError on HTTP 500") {
    FakeController ctl;
    ctl.on_get("/llm/status",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
            res.set_content("internal", "text/plain");
        });
    acva::llm::ModelControllerClient mcc(ctl.base_url());
    auto r = mcc.status();
    REQUIRE(std::holds_alternative<acva::llm::ClientError>(r));
}

TEST_CASE("ModelControllerClient: status() against unreachable port → ClientError") {
    acva::llm::ModelControllerClient mcc("http://127.0.0.1:1");
    auto r = mcc.status();
    REQUIRE(std::holds_alternative<acva::llm::ClientError>(r));
}

TEST_CASE("ModelControllerClient: load() success returns parsed status") {
    FakeController ctl;
    ctl.on_post("/llm/load",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(
                R"({"loaded_file":"newer.gguf","alias":"dialog","health":"healthy"})",
                "application/json");
        });

    acva::llm::ModelControllerClient mcc(ctl.base_url());
    auto r = mcc.load("newer.gguf", std::chrono::seconds(5));
    REQUIRE(std::holds_alternative<acva::llm::ControllerStatus>(r));
    CHECK(std::get<acva::llm::ControllerStatus>(r).loaded_file == "newer.gguf");
}

TEST_CASE("ModelControllerClient: load() with empty filename → ClientError") {
    FakeController ctl;
    acva::llm::ModelControllerClient mcc(ctl.base_url());
    auto r = mcc.load("", std::chrono::seconds(1));
    REQUIRE(std::holds_alternative<acva::llm::ClientError>(r));
}

TEST_CASE("ModelControllerClient: empty base_url disables") {
    acva::llm::ModelControllerClient mcc("");
    CHECK_FALSE(mcc.enabled());
    auto r = mcc.status();
    REQUIRE(std::holds_alternative<acva::llm::ClientError>(r));
}
