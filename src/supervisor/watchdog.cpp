#include "supervisor/watchdog.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <variant>

namespace acva::supervisor {

namespace {

std::int64_t to_ns(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        t.time_since_epoch()).count();
}

std::chrono::steady_clock::time_point from_ns(std::int64_t ns) {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(ns));
}

} // namespace

Watchdog::Watchdog(const config::SupervisorConfig& cfg,
                   event::EventBus& bus,
                   const dialogue::Fsm& fsm,
                   std::shared_ptr<metrics::Registry> registry)
    : cfg_(cfg), bus_(&bus), fsm_(&fsm), registry_(std::move(registry)) {
    last_activity_ns_.store(to_ns(std::chrono::steady_clock::now()),
                              std::memory_order_release);
}

Watchdog::~Watchdog() {
    stop();
    subs_.clear();
}

bool Watchdog::state_expects_activity(dialogue::State s) noexcept {
    switch (s) {
        case dialogue::State::Idle:
        case dialogue::State::Listening:
        case dialogue::State::UserSpeaking:
        case dialogue::State::Completed:
            return false;
        case dialogue::State::Transcribing:
        case dialogue::State::Thinking:
        case dialogue::State::Speaking:
        case dialogue::State::Interrupted:
            return true;
    }
    return false;
}

void Watchdog::note_activity(std::chrono::steady_clock::time_point now) {
    last_activity_ns_.store(to_ns(now), std::memory_order_release);
    fired_state_.store(-1, std::memory_order_release);
}

void Watchdog::start() {
    // One subscription per relevant event type. We use `subscribe_all`
    // (full Event variant) once and switch in the lambda — keeps the
    // overhead to one bus subscriber slot, and the variant visit is
    // cheaper than four separate handler queues.
    event::SubscribeOptions opts;
    opts.name = "watchdog.activity";
    opts.queue_capacity = 256;
    opts.policy = event::OverflowPolicy::DropOldest;
    auto sub = bus_->subscribe_all(opts, [this](const event::Event& e) {
        const auto now = std::chrono::steady_clock::now();
        std::visit([this, now]<class T>(const T& /*ev*/) {
            using Et = std::decay_t<T>;
            if constexpr (std::is_same_v<Et, event::LlmToken>
                       || std::is_same_v<Et, event::TtsAudioChunk>
                       || std::is_same_v<Et, event::SpeechStarted>
                       || std::is_same_v<Et, event::FinalTranscript>) {
                note_activity(now);
            }
        }, e);
    });
    subs_.push_back(std::move(sub));

    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    worker_ = std::thread([this] { run_loop(); });
}

void Watchdog::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (worker_.joinable()) worker_.join();
}

void Watchdog::run_loop() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(check_interval_);
        run_check_for_test(std::chrono::steady_clock::now());
    }
}

void Watchdog::run_check_for_test(std::chrono::steady_clock::time_point now) {
    if (cfg_.stuck_threshold_seconds == 0) return;

    const auto snap = fsm_->snapshot();
    if (!state_expects_activity(snap.state)) {
        // FSM transitioned to a quiet state — clear the latch so the
        // next noisy state can fire fresh.
        fired_state_.store(-1, std::memory_order_release);
        return;
    }

    const auto last_activity = from_ns(
        last_activity_ns_.load(std::memory_order_acquire));
    const auto inactive = now - last_activity;
    const auto threshold = std::chrono::seconds{cfg_.stuck_threshold_seconds};
    if (inactive < threshold) return;

    const int state_int = static_cast<int>(snap.state);
    int prev = fired_state_.load(std::memory_order_acquire);
    if (prev == state_int) return; // already fired for this state-episode

    // Try to claim the latch; if a concurrent call beat us, abort.
    if (!fired_state_.compare_exchange_strong(
            prev, state_int,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    fires_total_.fetch_add(1, std::memory_order_relaxed);
    const auto state_str = std::string(dialogue::to_string(snap.state));
    if (registry_) registry_->on_stuck(state_str.c_str());

    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(inactive).count();
    log::event("watchdog", "stuck", event::kNoTurn,
               {{"state", state_str},
                {"inactive_s", std::to_string(secs)},
                {"auto_restart", cfg_.auto_restart_on_stuck ? "1" : "0"}});

    if (cfg_.auto_restart_on_stuck && restart_fn_) {
        // Caller's closure is responsible for thread-safety; the
        // common pattern is a flag the main loop drains.
        restart_fn_(state_str.c_str());
    }
}

} // namespace acva::supervisor
