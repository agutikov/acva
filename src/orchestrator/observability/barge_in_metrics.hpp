#pragma once

#include "audio/wake_word.hpp"
#include "dialogue/barge_in.hpp"
#include "metrics/registry.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace acva::orchestrator {

// M7 + M8C Step 1 — small poller that mirrors audio-pipeline
// observability counters (barge-in + wake-word) into the metrics
// gauges. Runs on a dedicated 1 Hz thread so it doesn't have to
// share locks with the capture / TTS pollers (each of which owns
// non-trivial mutexes for its own subsystem). Either pointer may be
// null — only the live ones get polled.
//
// RAII: stop()+join() in the destructor; idempotent stop().
class BargeInMetricsPoller {
public:
    BargeInMetricsPoller(dialogue::BargeInDetector* detector,
                         audio::WakeWord* wake_word,
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
    audio::WakeWord*           wake_word_;
    std::shared_ptr<metrics::Registry> registry_;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};

} // namespace acva::orchestrator
