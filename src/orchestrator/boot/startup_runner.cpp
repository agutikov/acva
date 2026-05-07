#include "orchestrator/boot/startup_runner.hpp"

#include "log/log.hpp"
#include "supervisor/startup_check.hpp"

#include <fmt/format.h>

#include <iostream>

namespace acva::orchestrator {

bool run_startup_pass(const config::Config& cfg) {
    const auto failures = supervisor::run_startup_checks(cfg);
    for (const auto& f : failures) {
        const auto msg = fmt::format(
            "{} startup gate failed: {} — {}",
            f.component, f.error, f.remediation);
        if (cfg.supervisor.strict_startup) {
            log::error("startup_check", msg);
        } else {
            log::warn("startup_check", msg);
        }
    }
    if (!failures.empty() && cfg.supervisor.strict_startup) {
        std::cerr << "acva: " << failures.size()
                  << " startup gate(s) failed under strict_startup; "
                     "exiting (see structured logs above)\n";
        return true;
    }
    return false;
}

} // namespace acva::orchestrator
