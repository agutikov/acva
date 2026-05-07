#pragma once

#include "dialogue/barge_in.hpp"
#include "metrics/registry.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace acva::orchestrator {

// M7 — small poller that mirrors BargeInDetector counters into the
// metrics gauges. Runs on a dedicated 1 Hz thread so it doesn't have
// to share locks with the capture / TTS pollers (each of which owns
// non-trivial mutexes for its own subsystem).
//
// RAII: stop()+join() in the destructor; idempotent stop().
class BargeInMetricsPoller {
public:
    BargeInMetricsPoller(dialogue::BargeInDetector* detector,
                         std::shared_ptr<metrics::Registry> registry);
    ~BargeInMetricsPoller();

    BargeInMetricsPoller(const BargeInMetricsPoller&)            = delete;
    BargeInMetricsPoller& operator=(const BargeInMetricsPoller&) = delete;
    BargeInMetricsPoller(BargeInMetricsPoller&&)                 = delete;
    BargeInMetricsPoller& operator=(BargeInMetricsPoller&&)      = delete;

    void start();
    void stop();

private:
    dialogue::BargeInDetector* detector_;
    std::shared_ptr<metrics::Registry> registry_;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};

} // namespace acva::orchestrator
