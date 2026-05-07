#include "orchestrator/boot/model_controller_handoff.hpp"

#include "llm/model_controller_client.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>
#include <iostream>
#include <variant>

namespace acva::orchestrator {

bool run_model_controller_handoff(const config::Config& cfg) {
    if (cfg.llm.model_controller_url.empty()
        || cfg.llm.model_file.empty()) {
        return true;
    }

    llm::ModelControllerClient mcc(cfg.llm.model_controller_url);
    const auto cur = mcc.status();
    const std::string cur_loaded =
        std::holds_alternative<llm::ControllerStatus>(cur)
            ? std::get<llm::ControllerStatus>(cur).loaded_file
            : std::string{};
    if (cur_loaded == cfg.llm.model_file) {
        return true;
    }

    log::info("main", fmt::format(
        "model-controller: requesting {} (current: '{}')",
        cfg.llm.model_file, cur_loaded));

    auto load_res = mcc.load(cfg.llm.model_file, std::chrono::seconds(60));
    if (auto* err = std::get_if<llm::ClientError>(&load_res)) {
        if (cfg.supervisor.strict_startup) {
            std::cerr << "acva: model-controller load failed: "
                      << err->message << "\n";
            return false;
        }
        log::warn("main", fmt::format(
            "model-controller load failed (continuing in tolerant mode): {}",
            err->message));
        return true;
    }
    const auto& s = std::get<llm::ControllerStatus>(load_res);
    log::info("main", fmt::format(
        "model-controller: now serving {} ({})",
        s.loaded_file, s.health));
    return true;
}

} // namespace acva::orchestrator
