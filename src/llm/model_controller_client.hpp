#pragma once

#include <chrono>
#include <string>
#include <variant>

namespace acva::llm {

// M8A Step 5 — thin C++ client for the model-controller sidecar
// (see `packaging/model-controller/main.go`). The sidecar holds the
// docker socket privilege and is responsible for actually recreating
// the llama-server container with a different GGUF; acva calls it on
// startup when `cfg.llm.model_file` doesn't match what's currently
// loaded.
//
// Pillar #1 stands: acva itself never touches the docker socket. All
// it does here is HTTP request/response against the sidecar's
// `/llm/load` and `/llm/status` endpoints.

struct ControllerStatus {
    std::string loaded_file;        // GGUF filename currently served
    std::string alias;              // OpenAI-API model id (informational)
    std::string health;             // "healthy" | "loading" | "unhealthy"
};

struct ClientError {
    std::string message;
};

template <class T>
using Result = std::variant<T, ClientError>;

class ModelControllerClient {
public:
    // `base_url` is `http://host:port` (no trailing slash). Empty
    // disables the client — every method returns ClientError.
    explicit ModelControllerClient(std::string base_url);

    [[nodiscard]] bool enabled() const noexcept { return !base_url_.empty(); }

    // GET /llm/status. Tight 2 s read timeout — the sidecar serves
    // this from cached state, so a slow response signals a problem.
    [[nodiscard]] Result<ControllerStatus> status();

    // POST /llm/load {"file": <file>}. The sidecar recreates llama
    // and blocks until the new model is healthy, then returns 200.
    // `deadline` caps how long we wait — model-load + container-up
    // can run 10–30 s on the dev box, so callers typically set 60 s.
    [[nodiscard]] Result<ControllerStatus>
        load(const std::string& gguf_filename,
             std::chrono::seconds deadline);

private:
    std::string base_url_;
};

} // namespace acva::llm
