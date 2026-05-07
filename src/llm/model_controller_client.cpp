#include "llm/model_controller_client.hpp"

#include <httplib.h>

#include <fmt/format.h>

#include <utility>

namespace acva::llm {

namespace {

// Parse a small JSON object with three string fields. We don't pull
// glaze in here — the controller's status payload is fixed-shape and
// trivially scannable. `find_value` returns the value for `key`
// (between the surrounding quotes); empty on miss.
std::string find_value(const std::string& body, std::string_view key) {
    const std::string needle = std::string{"\""} + std::string(key) + "\":";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    if (pos >= body.size() || body[pos] != '"') return {};
    ++pos;
    auto end = body.find('"', pos);
    if (end == std::string::npos) return {};
    return body.substr(pos, end - pos);
}

ControllerStatus parse_status(const std::string& body) {
    return ControllerStatus{
        .loaded_file = find_value(body, "loaded_file"),
        .alias       = find_value(body, "alias"),
        .health      = find_value(body, "health"),
    };
}

// Slice "http://host:port" (possibly with a trailing path the caller
// shouldn't include but we tolerate) into the (authority, scheme)
// pair httplib::Client expects. Path components after the authority
// are dropped; the controller's surface is rooted at /.
std::string authority_of(const std::string& base_url) {
    auto pos = base_url.find("://");
    auto start = (pos == std::string::npos) ? 0 : pos + 3;
    auto path = base_url.find('/', start);
    if (path == std::string::npos) return base_url;
    return base_url.substr(0, path);
}

} // namespace

ModelControllerClient::ModelControllerClient(std::string base_url)
    : base_url_(std::move(base_url)) {}

Result<ControllerStatus> ModelControllerClient::status() {
    if (base_url_.empty()) {
        return ClientError{"model_controller: base_url empty"};
    }
    httplib::Client cli(authority_of(base_url_));
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Get("/llm/status");
    if (!res) {
        return ClientError{
            fmt::format("model_controller: GET /llm/status: no response ({})",
                        httplib::to_string(res.error()))};
    }
    if (res->status != 200) {
        return ClientError{
            fmt::format("model_controller: GET /llm/status: HTTP {} {}",
                        res->status, res->body)};
    }
    return parse_status(res->body);
}

Result<ControllerStatus>
ModelControllerClient::load(const std::string& gguf_filename,
                             std::chrono::seconds deadline) {
    if (base_url_.empty()) {
        return ClientError{"model_controller: base_url empty"};
    }
    if (gguf_filename.empty()) {
        return ClientError{"model_controller: file is required"};
    }
    httplib::Client cli(authority_of(base_url_));
    cli.set_connection_timeout(2);
    // The sidecar blocks until the new container is healthy, so the
    // read deadline must cover the whole load flow. We give it the
    // caller's full window plus a small slack so we hear the
    // controller's own timeout response if it fires first.
    cli.set_read_timeout(static_cast<time_t>(deadline.count() + 5));

    const auto body = fmt::format(R"({{"file":"{}"}})", gguf_filename);
    auto res = cli.Post("/llm/load", body, "application/json");
    if (!res) {
        return ClientError{
            fmt::format("model_controller: POST /llm/load: no response ({})",
                        httplib::to_string(res.error()))};
    }
    if (res->status != 200 && res->status != 202) {
        return ClientError{
            fmt::format("model_controller: POST /llm/load: HTTP {} {}",
                        res->status, res->body)};
    }
    return parse_status(res->body);
}

} // namespace acva::llm
