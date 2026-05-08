#include "orchestrator/observability/barge_in_metrics.hpp"

#include <chrono>
#include <utility>

namespace acva::orchestrator {

BargeInMetricsPoller::BargeInMetricsPoller(
        dialogue::BargeInDetector* detector,
        audio::WakeWord* wake_word,
        std::shared_ptr<metrics::Registry> registry)
    : detector_(detector),
      wake_word_(wake_word),
      registry_(std::move(registry)) {}

BargeInMetricsPoller::~BargeInMetricsPoller() { stop(); }

void BargeInMetricsPoller::start() {
    // No-op when nothing observable is registered.
    if (detector_ == nullptr && wake_word_ == nullptr) return;
    if (thread_.joinable()) return;
    thread_ = std::thread([this] {
        using namespace std::chrono_literals;
        while (!stop_.load(std::memory_order_acquire)) {
            if (detector_ != nullptr) {
                registry_->set_barge_in_fires_total(
                    static_cast<double>(detector_->fires_total()));
                registry_->set_barge_in_suppressed_total(
                    static_cast<double>(detector_->suppressed_total()));
                registry_->set_barge_in_suppressed_cooldown(
                    static_cast<double>(detector_->suppressed_cooldown()));
                registry_->set_barge_in_suppressed_aec(
                    static_cast<double>(detector_->suppressed_aec()));
            }
            if (wake_word_ != nullptr) {
                registry_->set_wake_word_detections_total(
                    static_cast<double>(wake_word_->detections_total()));
                registry_->set_wake_word_last_score(
                    static_cast<double>(wake_word_->last_score()));
            }
            std::this_thread::sleep_for(1s);
        }
    });
}

void BargeInMetricsPoller::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

} // namespace acva::orchestrator
