#pragma once

#include "event/bus.hpp"

#include <string>
#include <thread>

namespace acva::orchestrator {

// M1 — text-input mode (`acva --stdin`). Reads lines from stdin and
// publishes each non-empty line as a `FinalTranscript` on the bus,
// stamped with the configured `lang`. EOF on stdin (Ctrl-D) calls
// `acva::cli::request_shutdown(SIGTERM)` to wake the main loop.
//
// RAII: the destructor closes STDIN_FILENO and joins the worker if
// it's still in `getline()`. Idempotent join.
class StdinReader {
public:
    StdinReader(event::EventBus& bus, std::string lang);
    ~StdinReader();

    StdinReader(const StdinReader&)            = delete;
    StdinReader& operator=(const StdinReader&) = delete;
    StdinReader(StdinReader&&)                 = delete;
    StdinReader& operator=(StdinReader&&)      = delete;

    void start();

    // Wakes the reader (closes stdin) and joins the thread. Safe to
    // call multiple times.
    void stop();

private:
    event::EventBus* bus_;
    std::string      lang_;
    std::thread      thread_;
};

} // namespace acva::orchestrator
