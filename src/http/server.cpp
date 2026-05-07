#include "http/server.hpp"

#include "log/log.hpp"

#include <prometheus/text_serializer.h>

#include <httplib.h>

#include <fmt/format.h>

#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace acva::http {

namespace {

std::string serialize_metrics(const prometheus::Registry& reg) {
    prometheus::TextSerializer serializer;
    std::ostringstream os;
    serializer.Serialize(os, reg.Collect());
    return os.str();
}

// JSON-escape a string for embedding inside a quoted JSON value. The
// reload diff fields are dotted ASCII paths (`vad.onset_threshold`)
// plus parser error messages, which can contain anything; escape
// conservatively. We don't pull glaze in for this tiny formatter
// because /reload's response is fixed-shape and emitted manually.
std::string escape_json(std::string_view s) {
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

std::string serialize_string_array(const std::vector<std::string>& items) {
    std::string out;
    out.push_back('[');
    bool first = true;
    for (const auto& s : items) {
        if (!first) out.push_back(',');
        first = false;
        out.append(escape_json(s));
    }
    out.push_back(']');
    return out;
}

std::string serialize_status(const dialogue::Fsm* fsm,
                              const ControlServer::StatusExtra& extra) {
    std::string body;
    if (!fsm) {
        body = R"("state":"unknown")";
    } else {
        const auto snap = fsm->snapshot();
        body = fmt::format(
            R"("state":"{}","active_turn":{},"outcome":"{}",)"
            R"("sentences_played":{},"turns_completed":{},)"
            R"("turns_interrupted":{},"turns_discarded":{})",
            dialogue::to_string(snap.state),
            snap.active_turn,
            dialogue::to_string(snap.outcome),
            snap.sentences_played,
            snap.turns_completed,
            snap.turns_interrupted,
            snap.turns_discarded);
    }

    if (extra) {
        try {
            auto frag = extra();
            if (!frag.empty()) {
                body.push_back(',');
                body.append(frag);
            }
        } catch (...) {
            // Never fail /status because of an extra-field supplier.
        }
    }

    return "{" + body + "}\n";
}

} // namespace

// PIMPL-ish — keep httplib symbols out of the header so users of
// http/server.hpp don't pull a 10k-line include.
struct ControlServer::Impl {
    httplib::Server server;
    std::thread listen_thread;
};

ControlServer::ControlServer(const config::ControlConfig& cfg,
                             std::shared_ptr<metrics::Registry> registry,
                             const dialogue::Fsm* fsm,
                             StatusExtra status_extra,
                             ReloadHandler reload_handler,
                             PrivacyHandlers privacy)
    : registry_(std::move(registry)),
      fsm_(fsm),
      status_extra_(std::move(status_extra)),
      reload_handler_(std::move(reload_handler)),
      privacy_(std::move(privacy)),
      impl_(std::make_unique<Impl>()) {

    auto& server = impl_->server;

    server.Get("/metrics", [reg = registry_->registry()](const httplib::Request&, httplib::Response& res) {
        res.set_content(serialize_metrics(*reg), "text/plain; version=0.0.4");
    });

    server.Get("/status", [fsm = fsm_, extra = status_extra_]
               (const httplib::Request&, httplib::Response& res) {
        res.set_content(serialize_status(fsm, extra), "application/json");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok\n", "text/plain");
    });

    // M8A — config hot-reload. The handler runs on httplib's request
    // thread; ConfigReloader::reload is itself blocking (parses YAML +
    // calls each registered callback synchronously) but takes <100 ms
    // for the realistic config sizes we ship.
    server.Post("/reload",
        [handler = reload_handler_]
        (const httplib::Request&, httplib::Response& res) {
            if (!handler) {
                res.status = 503;
                res.set_content(R"({"error":"reload not configured"})" "\n",
                                "application/json");
                return;
            }
            const auto result = handler();
            if (auto* ok = std::get_if<config::ReloadOk>(&result)) {
                res.status = 200;
                res.set_content(
                    fmt::format(
                        R"({{"status":"ok","applied":{}}})" "\n",
                        serialize_string_array(ok->diff.changed_hot)),
                    "application/json");
            } else if (auto* rej = std::get_if<config::ReloadRejected>(&result)) {
                res.status = 409;
                res.set_content(
                    fmt::format(
                        R"({{"status":"rejected","restart_required":{},"applied":{}}})" "\n",
                        serialize_string_array(rej->diff.changed_restart),
                        serialize_string_array(rej->diff.changed_hot)),
                    "application/json");
            } else if (auto* err = std::get_if<config::ReloadParseError>(&result)) {
                res.status = 400;
                res.set_content(
                    fmt::format(R"({{"status":"parse_error","error":{}}})" "\n",
                                escape_json(err->message)),
                    "application/json");
            } else {
                res.status = 500;
                res.set_content(R"({"status":"internal_error"})" "\n",
                                "application/json");
            }
        });

    // ----- M8A Step 2: privacy commands -----

    auto set_muted_handler = privacy_.set_muted;
    auto mute_route = [set_muted_handler](bool target,
                                            httplib::Response& res) {
        if (!set_muted_handler) {
            res.status = 503;
            res.set_content(R"({"error":"mute not configured"})" "\n",
                            "application/json");
            return;
        }
        set_muted_handler(target);
        res.status = 200;
        res.set_content(
            fmt::format(R"({{"status":"ok","muted":{}}})" "\n",
                        target ? "true" : "false"),
            "application/json");
    };
    server.Post("/mute",   [mute_route](const httplib::Request&, httplib::Response& res) {
        mute_route(true, res);
    });
    server.Post("/unmute", [mute_route](const httplib::Request&, httplib::Response& res) {
        mute_route(false, res);
    });

    server.Post("/new-session",
        [handler = privacy_.new_session]
        (const httplib::Request&, httplib::Response& res) {
            if (!handler) {
                res.status = 503;
                res.set_content(R"({"error":"new-session not configured"})" "\n",
                                "application/json");
                return;
            }
            const auto result = handler();
            if (auto* sid = std::get_if<std::int64_t>(&result)) {
                res.status = 200;
                res.set_content(
                    fmt::format(R"({{"status":"ok","session_id":{}}})" "\n", *sid),
                    "application/json");
            } else {
                res.status = 500;
                res.set_content(
                    fmt::format(R"({{"status":"error","error":{}}})" "\n",
                                escape_json(std::get<std::string>(result))),
                    "application/json");
            }
        });

    // POST /wipe — distinguishes session vs all by query parameter.
    // - ?session=<id>  → wipe one session (and roll over if active).
    // - ?all=true      → wipe everything and open a fresh session.
    // Missing both is a 400 to keep the endpoint from being a footgun
    // ("POST /wipe" with no qualifier should not nuke the whole DB).
    server.Post("/wipe",
        [wipe_session = privacy_.wipe_session,
         wipe_all     = privacy_.wipe_all]
        (const httplib::Request& req, httplib::Response& res) {
            const bool has_all     = req.has_param("all");
            const bool has_session = req.has_param("session");

            if (has_all) {
                if (req.get_param_value("all") != "true") {
                    res.status = 400;
                    res.set_content(
                        R"({"error":"all must be exactly 'true'"})" "\n",
                        "application/json");
                    return;
                }
                if (!wipe_all) {
                    res.status = 503;
                    res.set_content(
                        R"({"error":"wipe-all not configured"})" "\n",
                        "application/json");
                    return;
                }
                const auto result = wipe_all();
                if (auto* sid = std::get_if<std::int64_t>(&result)) {
                    res.status = 200;
                    res.set_content(
                        fmt::format(
                            R"({{"status":"ok","scope":"all","session_id":{}}})" "\n",
                            *sid),
                        "application/json");
                } else {
                    res.status = 500;
                    res.set_content(
                        fmt::format(R"({{"status":"error","error":{}}})" "\n",
                                    escape_json(std::get<std::string>(result))),
                        "application/json");
                }
                return;
            }

            if (has_session) {
                if (!wipe_session) {
                    res.status = 503;
                    res.set_content(
                        R"({"error":"wipe-session not configured"})" "\n",
                        "application/json");
                    return;
                }
                std::int64_t target = 0;
                try {
                    target = std::stoll(req.get_param_value("session"));
                } catch (...) {
                    res.status = 400;
                    res.set_content(
                        R"({"error":"session must be an integer"})" "\n",
                        "application/json");
                    return;
                }
                const auto err = wipe_session(target);
                if (!err.has_value()) {
                    res.status = 200;
                    res.set_content(
                        fmt::format(
                            R"({{"status":"ok","scope":"session","session_id":{}}})" "\n",
                            target),
                        "application/json");
                } else {
                    res.status = 500;
                    res.set_content(
                        fmt::format(R"({{"status":"error","error":{}}})" "\n",
                                    escape_json(*err)),
                        "application/json");
                }
                return;
            }

            res.status = 400;
            res.set_content(
                R"({"error":"specify ?session=<id> or ?all=true"})" "\n",
                "application/json");
        });

    if (!server.bind_to_port(cfg.bind, cfg.port)) {
        throw std::runtime_error(
            fmt::format("control server: failed to bind {}:{}", cfg.bind, cfg.port));
    }
    bound_port_ = cfg.port;

    impl_->listen_thread = std::thread([this] {
        impl_->server.listen_after_bind();
    });

    log::info("http", fmt::format("control plane listening on {}:{} (port {})",
                                    cfg.bind, cfg.port, bound_port_));
}

ControlServer::~ControlServer() {
    if (impl_) {
        impl_->server.stop();
        if (impl_->listen_thread.joinable()) {
            impl_->listen_thread.join();
        }
    }
}

} // namespace acva::http
