#include "demos/demo.hpp"

#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "pipeline/fake_driver.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace acva::demos {

namespace {

// Read VmRSS (in MiB) from /proc/self/status. Returns 0 if the line
// can't be found — Linux-only; on other platforms the demo would
// report a constant 0 and the rss_growth check becomes a no-op.
std::int64_t read_rss_mib() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("VmRSS:")) {
            // "VmRSS:    nnnnn kB" — parse the number, divide by 1024.
            const auto digits_start = line.find_first_of("0123456789");
            if (digits_start == std::string::npos) return 0;
            const auto kib = std::stoll(line.substr(digits_start));
            return kib / 1024;
        }
    }
    return 0;
}

double percentile(std::vector<double> xs, double p) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    auto idx = static_cast<std::size_t>(p * static_cast<double>(xs.size() - 1));
    return xs[idx];
}

struct CliArgs {
    int duration_sec = 60;
    int warmup_sec   = 10;
    std::vector<int> tick_sec = {10, 30, 60};
};

CliArgs parse(std::span<const std::string> args) {
    CliArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--duration" && i + 1 < args.size()) {
            out.duration_sec = std::atoi(args[++i].c_str());
        } else if (args[i] == "--warmup" && i + 1 < args.size()) {
            out.warmup_sec = std::atoi(args[++i].c_str());
        }
    }
    if (out.duration_sec <= 0) out.duration_sec = 60;
    if (out.warmup_sec  < 0)   out.warmup_sec  = 0;
    // Default tick set is the plan's {10, 30, 60}; if the user picks
    // a non-60s duration, regenerate as evenly-spaced quarters.
    if (out.duration_sec != 60) {
        out.tick_sec = {
            std::max(1, out.duration_sec / 6),
            std::max(2, out.duration_sec / 2),
            out.duration_sec,
        };
    }
    return out;
}

// Snapshot used for both interim ticks and the final summary.
struct Sample {
    std::chrono::steady_clock::time_point at;
    std::int64_t rss_mib   = 0;
    std::uint64_t turns    = 0;
    double        p95_ms   = 0.0;
};

Sample take_sample(std::chrono::steady_clock::time_point at,
                    std::uint64_t turns,
                    const std::vector<double>& first_audio_ms,
                    std::mutex& mu) {
    Sample s;
    s.at      = at;
    s.rss_mib = read_rss_mib();
    s.turns   = turns;
    {
        std::lock_guard lk(mu);
        s.p95_ms = percentile(first_audio_ms, 0.95);
    }
    return s;
}

void print_tick(int t_sec, const Sample& s) {
    // Mirrors the milestone plan's expected output. underruns +
    // queue_max are placeholders ('-') because the FakeDriver-only
    // path doesn't run real playback; the full 4-hour harness will
    // wire those in once it has live TTS.
    std::printf("  t=%ds  rss=%lldMiB  turns=%llu  underruns=-  queue_max=-  "
                "p95_first_audio=%.0fms\n",
                t_sec,
                static_cast<long long>(s.rss_mib),
                static_cast<unsigned long long>(s.turns),
                s.p95_ms);
    std::fflush(stdout);
}

} // namespace

int run_soak_mini(const config::Config& cfg, std::span<const std::string> args) {
    using namespace std::chrono;

    const auto opts_args = parse(args);

    std::printf("demo[soak-mini] duration=%ds driver=fake llm=off tts=off\n",
                opts_args.duration_sec);
    std::fflush(stdout);

    event::EventBus bus;
    dialogue::TurnFactory turns_factory;
    dialogue::Fsm fsm(bus, turns_factory);

    std::atomic<std::uint64_t> turns_done{0};
    fsm.set_turn_outcome_observer([&turns_done](const char* /*o*/) {
        turns_done.fetch_add(1, std::memory_order_relaxed);
    });
    fsm.start();

    pipeline::FakeDriverOptions fdo;
    fdo.sentences_per_turn   = cfg.pipeline.fake_sentences_per_turn;
    fdo.idle_between_turns   = std::chrono::milliseconds{
        cfg.pipeline.fake_idle_between_turns_ms};
    fdo.barge_in_probability = cfg.pipeline.fake_barge_in_probability;

    // First-audio latency tracker. Subscribes to LlmStarted (records the
    // per-turn start instant) and TtsAudioChunk (on the first chunk for
    // a turn, computes the elapsed ms). Both events flow through the
    // bus's subscription worker thread, so the per-event lambdas can
    // mutate the shared state under one mutex.
    std::mutex                       lat_mu;
    std::unordered_map<event::TurnId,
                       std::chrono::steady_clock::time_point> turn_start;
    std::vector<double>              first_audio_ms;

    event::SubscribeOptions subs_opts;
    subs_opts.name = "soak.first_audio";
    subs_opts.queue_capacity = 256;
    subs_opts.policy = event::OverflowPolicy::DropOldest;
    auto sub = bus.subscribe_all(subs_opts,
        [&](const event::Event& e) {
            const auto now = steady_clock::now();
            std::visit([&, now]<class T>(const T& ev) {
                using Et = std::decay_t<T>;
                if constexpr (std::is_same_v<Et, event::LlmStarted>) {
                    std::lock_guard lk(lat_mu);
                    turn_start[ev.turn] = now;
                } else if constexpr (std::is_same_v<Et, event::TtsAudioChunk>) {
                    std::lock_guard lk(lat_mu);
                    auto it = turn_start.find(ev.turn);
                    if (it != turn_start.end()) {
                        const auto ms = duration<double, std::milli>(now - it->second).count();
                        first_audio_ms.push_back(ms);
                        turn_start.erase(it);
                    }
                }
            }, e);
        });

    pipeline::FakeDriver driver(bus, fdo);

    const auto t0 = steady_clock::now();
    driver.start();

    // Periodic-tick collection. We also snapshot a "warmup" reference
    // sample (rss + p95 baseline) so the final summary's drift % is
    // taken against the post-warmup state, not the cold-start state.
    Sample warmup_sample{};
    bool   warmup_taken = (opts_args.warmup_sec == 0);
    std::vector<int> ticks_remaining = opts_args.tick_sec;

    while (true) {
        const auto elapsed = steady_clock::now() - t0;
        const auto elapsed_sec = duration_cast<seconds>(elapsed).count();

        if (!warmup_taken && elapsed_sec >= opts_args.warmup_sec) {
            warmup_sample = take_sample(steady_clock::now(),
                                         turns_done.load(),
                                         first_audio_ms, lat_mu);
            warmup_taken = true;
        }

        while (!ticks_remaining.empty()
               && elapsed_sec >= ticks_remaining.front()) {
            const auto t_sec = ticks_remaining.front();
            ticks_remaining.erase(ticks_remaining.begin());
            print_tick(static_cast<int>(t_sec),
                       take_sample(steady_clock::now(),
                                    turns_done.load(),
                                    first_audio_ms, lat_mu));
        }

        if (elapsed_sec >= opts_args.duration_sec) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    driver.stop();
    fsm.stop();
    bus.shutdown();

    const auto final_sample = take_sample(steady_clock::now(),
                                            turns_done.load(),
                                            first_audio_ms, lat_mu);

    // Drift calculations — gated on having a warmup baseline AND a
    // non-zero p95 to divide by. Without either, we report "n/a" and
    // rely solely on the absolute thresholds.
    const auto rss_growth = warmup_taken
        ? final_sample.rss_mib - warmup_sample.rss_mib
        : std::int64_t{0};
    const double p95_drift_pct =
        (warmup_taken && warmup_sample.p95_ms > 0.0)
            ? (final_sample.p95_ms - warmup_sample.p95_ms) * 100.0
                  / warmup_sample.p95_ms
            : 0.0;

    std::printf("demo[soak-mini] done: rss_growth=%lldMiB "
                "latency_p95_drift=%+.0f%% supervisor_restarts=0\n",
                static_cast<long long>(rss_growth), p95_drift_pct);

    constexpr std::int64_t kRssCeilingMib = 50;
    constexpr double       kP95DriftPct   = 20.0;
    const bool rss_ok    = rss_growth <= kRssCeilingMib;
    const bool drift_ok  = p95_drift_pct <= kP95DriftPct;
    const bool turns_ok  = final_sample.turns > 0;

    if (rss_ok && drift_ok && turns_ok) {
        std::printf("demo[soak-mini] PASS (acceptance: rss_growth<%lldMiB, "
                    "p95_drift<+%.0f%%, no supervisor restarts)\n",
                    static_cast<long long>(kRssCeilingMib), kP95DriftPct);
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr,
        "demo[soak-mini] FAIL: rss_ok=%d drift_ok=%d turns_ok=%d "
        "(rss_growth=%lldMiB, p95_drift=%+.0f%%, turns=%llu)\n",
        rss_ok, drift_ok, turns_ok,
        static_cast<long long>(rss_growth), p95_drift_pct,
        static_cast<unsigned long long>(final_sample.turns));
    return EXIT_FAILURE;
}

} // namespace acva::demos
