#include "orchestrator/io/stdin_reader.hpp"

#include "cli/args.hpp"
#include "event/event.hpp"

#include <csignal>
#include <iostream>
#include <unistd.h>
#include <utility>

namespace acva::orchestrator {

StdinReader::StdinReader(event::EventBus& bus, std::string lang)
    : bus_(&bus), lang_(std::move(lang)) {}

StdinReader::~StdinReader() { stop(); }

void StdinReader::start() {
    if (thread_.joinable()) return;
    std::cout << "acva stdin mode (lang=" << lang_
              << ") — type a line and press enter. Ctrl-D or Ctrl-C to exit.\n";
    thread_ = std::thread([bus = bus_, lang = lang_] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            bus->publish(event::FinalTranscript{
                .turn = 0,
                .text = line,
                .lang = lang,
                .confidence = 1.0F,
                .audio_duration = {},
                .processing_duration = {},
            });
        }
        // EOF on stdin (Ctrl-D or stdin closed) — request shutdown.
        cli::request_shutdown(SIGTERM);
    });
}

void StdinReader::stop() {
    if (!thread_.joinable()) return;
    // Wake the reader if it's still blocked in getline().
    ::close(STDIN_FILENO);
    thread_.join();
}

} // namespace acva::orchestrator
