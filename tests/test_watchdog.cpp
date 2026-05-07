#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "metrics/registry.hpp"
#include "supervisor/watchdog.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace cfgns = acva::config;
namespace dlg = acva::dialogue;
namespace evt = acva::event;
namespace mtr = acva::metrics;
namespace sup = acva::supervisor;

namespace {

using namespace std::chrono_literals;

// Drive the FSM through SpeechStarted → SpeechEnded → FinalTranscript
// so it lands in Thinking. Each publish goes through the FSM's bus
// subscription, so we sleep briefly to let the worker thread drain.
void drive_fsm_to_thinking(evt::EventBus& bus, dlg::Fsm& fsm) {
    bus.publish(evt::SpeechStarted{});
    std::this_thread::sleep_for(20ms);
    bus.publish(evt::SpeechEnded{});
    std::this_thread::sleep_for(20ms);
    bus.publish(evt::FinalTranscript{
        .turn = 0,
        .text = "hello",
        .lang = "en",
        .confidence = 1.0F,
        .audio_duration = {},
        .processing_duration = {},
    });
    // Wait long enough for both the FSM and the watchdog's own
    // bus subscription to observe the event.
    for (int i = 0; i < 20; ++i) {
        if (fsm.snapshot().state == dlg::State::Thinking) break;
        std::this_thread::sleep_for(10ms);
    }
}

} // namespace

TEST_CASE("Watchdog: fires once when inactive past threshold in Thinking") {
    cfgns::SupervisorConfig cfg{};
    cfg.stuck_threshold_seconds = 1;

    evt::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();
    auto registry = std::make_shared<mtr::Registry>();

    sup::Watchdog wd(cfg, bus, fsm, registry);
    wd.start();

    drive_fsm_to_thinking(bus, fsm);
    REQUIRE(fsm.snapshot().state == dlg::State::Thinking);

    // Push the synthetic clock past the threshold + the activity bump
    // the FinalTranscript caused above. Real-time threshold is 1 s, so
    // using "now + 5 s" leaves room for any subscription jitter.
    const auto far_future =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    wd.run_check_for_test(far_future);

    CHECK(wd.fires_total() == 1);

    // A second check at the same/later time does NOT fire again — the
    // latch holds for the same state-episode.
    wd.run_check_for_test(far_future + 1s);
    CHECK(wd.fires_total() == 1);

    wd.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("Watchdog: does not fire while FSM state is quiet (Listening)") {
    cfgns::SupervisorConfig cfg{};
    cfg.stuck_threshold_seconds = 1;

    evt::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();
    auto registry = std::make_shared<mtr::Registry>();

    sup::Watchdog wd(cfg, bus, fsm, registry);
    wd.start();
    REQUIRE(fsm.snapshot().state == dlg::State::Listening);

    const auto far_future =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    wd.run_check_for_test(far_future);
    CHECK(wd.fires_total() == 0);

    wd.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("Watchdog: auto_restart_on_stuck invokes the registered closure") {
    cfgns::SupervisorConfig cfg{};
    cfg.stuck_threshold_seconds = 1;
    cfg.auto_restart_on_stuck = true;

    evt::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();
    auto registry = std::make_shared<mtr::Registry>();

    sup::Watchdog wd(cfg, bus, fsm, registry);
    std::atomic<int> restart_calls{0};
    std::string last_reason;
    wd.set_restart_fn([&](const char* reason) {
        ++restart_calls;
        last_reason = reason;
    });
    wd.start();

    drive_fsm_to_thinking(bus, fsm);
    REQUIRE(fsm.snapshot().state == dlg::State::Thinking);

    const auto far_future =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    wd.run_check_for_test(far_future);

    CHECK(wd.fires_total() == 1);
    CHECK(restart_calls.load() == 1);
    CHECK(last_reason == "thinking");

    wd.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("Watchdog: zero threshold disables the check") {
    cfgns::SupervisorConfig cfg{};
    cfg.stuck_threshold_seconds = 0;

    evt::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();
    auto registry = std::make_shared<mtr::Registry>();

    sup::Watchdog wd(cfg, bus, fsm, registry);
    wd.start();

    drive_fsm_to_thinking(bus, fsm);

    const auto far_future =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    wd.run_check_for_test(far_future);
    CHECK(wd.fires_total() == 0);

    wd.stop();
    fsm.stop();
    bus.shutdown();
}
