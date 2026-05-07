#pragma once

#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "metrics/registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace acva::supervisor {

// M8A Step 4 — passive watchdog. Subscribes to a small set of bus
// events that mark "the orchestrator is making progress" (LlmToken,
// TtsAudioChunk, SpeechStarted, FinalTranscript) and refreshes a
// last_activity_ timestamp on each. A periodic check thread compares
// now - last_activity_ against `cfg.supervisor.stuck_threshold_seconds`
// while the FSM is in a state that EXPECTS activity (Transcribing,
// Thinking, Speaking, Interrupted) — Listening / Idle / Completed are
// legitimately quiet and never fire.
//
// On firing, the watchdog:
//   1. Increments `voice_stuck_total{state=...}` via the Registry.
//   2. Emits a structured "stuck" log line.
//   3. If `auto_restart_on_stuck` is true AND a restart closure is
//      registered, invokes it.
// Each stuck episode fires once: the `fired_` latch resets only when
// new activity arrives or the FSM transitions to a different state,
// so a permanent stuck condition doesn't spam the log or trigger
// repeated restart attempts.
class Watchdog {
public:
    using RestartFn = std::function<void(const char* reason)>;

    Watchdog(const config::SupervisorConfig& cfg,
             event::EventBus& bus,
             const dialogue::Fsm& fsm,
             std::shared_ptr<metrics::Registry> registry);
    ~Watchdog();

    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&)                 = delete;
    Watchdog& operator=(Watchdog&&)      = delete;

    // Optional auto-restart closure. Invoked from the watchdog's
    // periodic-check thread when a stuck episode fires AND
    // `cfg.auto_restart_on_stuck` is true. Caller is responsible for
    // making the closure thread-safe (typically it posts a
    // request_restart flag to the main loop).
    void set_restart_fn(RestartFn fn) { restart_fn_ = std::move(fn); }

    void start();
    void stop();

    // Cumulative stuck-fire count since startup. Mirrors the metric
    // family for tests / status output.
    [[nodiscard]] std::uint64_t fires_total() const noexcept {
        return fires_total_.load(std::memory_order_relaxed);
    }

    // For tests: pump the periodic check synchronously. The default
    // run-loop sleeps `check_interval_` between calls; tests can call
    // this directly with a synthetic `now`.
    void run_check_for_test(std::chrono::steady_clock::time_point now);

private:
    void run_loop();
    void note_activity(std::chrono::steady_clock::time_point now);

    // Returns true when `state` is one in which sustained inactivity
    // means "stuck". Idle/Listening/Completed are legitimately quiet.
    static bool state_expects_activity(dialogue::State s) noexcept;

    config::SupervisorConfig cfg_;
    event::EventBus*         bus_;
    const dialogue::Fsm*     fsm_;
    std::shared_ptr<metrics::Registry> registry_;

    std::atomic<std::int64_t> last_activity_ns_{0};
    std::atomic<std::uint64_t> fires_total_{0};

    // Per-state latch — resets when the FSM transitions OUT of the
    // currently-tracked state, so a fresh stuck-window can fire on the
    // next state's mismatch. Stored as an int (state enum value) so we
    // don't need a mutex around dialogue::State.
    std::atomic<int> fired_state_{-1};

    std::vector<event::SubscriptionHandle> subs_;
    std::thread       worker_;
    std::atomic<bool> running_{false};
    std::chrono::milliseconds check_interval_{500};

    RestartFn restart_fn_;
};

} // namespace acva::supervisor
