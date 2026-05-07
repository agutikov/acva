#pragma once

#include "dialogue/session.hpp"
#include "http/server.hpp"
#include "orchestrator/stacks/capture_stack.hpp"

#include <memory>

namespace acva::orchestrator {

// M8A Step 2 — build the ControlServer::PrivacyHandlers struct from
// the live SessionManager and the (possibly-not-yet-built) capture
// stack. The mute closure captures `capture` by reference because
// the capture stack is constructed AFTER the ControlServer in main();
// until that happens, /mute is a no-op log line. The session/wipe
// closures capture `sessions` directly.
[[nodiscard]] http::ControlServer::PrivacyHandlers
make_privacy_handlers(std::unique_ptr<CaptureStack>& capture,
                       dialogue::SessionManager& sessions);

} // namespace acva::orchestrator
