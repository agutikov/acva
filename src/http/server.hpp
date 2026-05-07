#pragma once

#include "config/config.hpp"
#include "config/reload.hpp"
#include "dialogue/fsm.hpp"
#include "metrics/registry.hpp"

#include <functional>
#include <memory>
#include <string>

namespace acva::http {

// Tiny HTTP control plane built on cpp-httplib.
//
// Routes:
//   GET /metrics  — Prometheus exposition format from metrics::Registry
//   GET /status   — JSON snapshot: fsm state, active turn, subscriber count
//   GET /health   — plain-text "ok\n"
//
// More routes (/mute, /unmute, /reload, /new-session, /wipe) land in later
// milestones. The implementation is pimpl'd to keep the cpp-httplib mega-
// header out of every translation unit that includes this.
class ControlServer {
public:
    // Optional JSON-fragment supplier for /status. The fragment is
    // substituted into the top-level JSON object as additional fields,
    // so it must NOT include the surrounding braces — return text like
    //   "\"pipeline_state\":\"ok\",\"services\":[...]"
    // ControlServer prepends a comma when the fragment is non-empty.
    // Decoupled from supervisor.hpp so http/server.hpp doesn't pull
    // supervisor headers into every TU that needs the control plane.
    using StatusExtra = std::function<std::string()>;

    // M8A — invoked when a client POSTs /reload. The server shapes
    // the HTTP response from the returned variant: ReloadOk → 200,
    // ReloadRejected → 409, ReloadParseError → 400. Optional;
    // omitted means /reload returns 503 ("not configured"). Calls
    // run on the httplib request thread, so the handler must be
    // self-synchronising (ConfigReloader::reload uses internal
    // serialisation).
    using ReloadHandler = std::function<config::ReloadResult()>;

    // M8A Step 4 — POST /restart handler. Empty optional = accepted
    // (server returns 202 and the request connection drops as the
    // orchestrator exec's a fresh process). Non-empty optional = the
    // restart was rejected (e.g. debounced because a previous request
    // is still in-flight); the string is surfaced as a 409 message.
    using RestartHandler = std::function<std::optional<std::string>()>;

    // M8A Step 2 — privacy command handlers. Each closure converts
    // its underlying component (AudioPipeline mute flag, SessionManager
    // operations) into a flat shape so the server header stays free
    // of audio/memory dependencies. Any handler may be empty; routes
    // that depend on an empty handler return 503.
    struct PrivacyHandlers {
        // POST /mute  → set_muted(true);  POST /unmute → set_muted(false).
        std::function<void(bool)> set_muted;
        // POST /new-session: success → int64 (new session id);
        //                     failure → string (error message).
        std::function<std::variant<std::int64_t, std::string>()> new_session;
        // POST /wipe?session=<id>: success → empty optional;
        //                          failure → optional<string> (error message).
        std::function<std::optional<std::string>(std::int64_t)> wipe_session;
        // POST /wipe?all=true: same shape as new_session — returns the
        // freshly opened session id on success.
        std::function<std::variant<std::int64_t, std::string>()> wipe_all;
    };

    ControlServer(const config::ControlConfig& cfg,
                  std::shared_ptr<metrics::Registry> registry,
                  const dialogue::Fsm* fsm,
                  StatusExtra status_extra = {},
                  ReloadHandler reload_handler = {},
                  PrivacyHandlers privacy = {},
                  RestartHandler restart_handler = {});
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&&) = delete;
    ControlServer& operator=(ControlServer&&) = delete;

    [[nodiscard]] int port() const noexcept { return bound_port_; }

private:
    struct Impl;

    std::shared_ptr<metrics::Registry> registry_;
    const dialogue::Fsm* fsm_; // not owned; nullable
    StatusExtra status_extra_;
    ReloadHandler reload_handler_;
    PrivacyHandlers privacy_;
    RestartHandler restart_handler_;
    std::unique_ptr<Impl> impl_;
    int bound_port_ = 0;
};

} // namespace acva::http
