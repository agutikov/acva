#include "orchestrator/admin/privacy_handlers.hpp"

#include "audio/pipeline.hpp"
#include "log/log.hpp"
#include "memory/repository.hpp"

#include <variant>

namespace acva::orchestrator {

http::ControlServer::PrivacyHandlers
make_privacy_handlers(std::unique_ptr<CaptureStack>& capture,
                       dialogue::SessionManager& sessions) {
    http::ControlServer::PrivacyHandlers privacy{};

    privacy.set_muted = [&capture](bool m) {
        if (!capture) return;
        if (auto* ap = capture->pipeline(); ap != nullptr) {
            ap->set_muted(m);
            log::info("privacy", m ? "muted" : "unmuted");
        }
    };

    privacy.new_session =
        [&sessions]() -> std::variant<std::int64_t, std::string> {
            auto r = sessions.roll_over();
            if (auto* err = std::get_if<memory::DbError>(&r)) {
                return err->message;
            }
            return std::get<memory::SessionId>(r);
        };

    privacy.wipe_session =
        [&sessions](std::int64_t id) -> std::optional<std::string> {
            auto err = sessions.wipe_session(id);
            if (err.has_value()) return err->message;
            return std::nullopt;
        };

    privacy.wipe_all =
        [&sessions]() -> std::variant<std::int64_t, std::string> {
            auto r = sessions.wipe_all();
            if (auto* err = std::get_if<memory::DbError>(&r)) {
                return err->message;
            }
            return std::get<memory::SessionId>(r);
        };

    return privacy;
}

} // namespace acva::orchestrator
