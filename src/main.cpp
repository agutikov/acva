#include "cli/args.hpp"
#include "cli/memory_cli.hpp"
#include "config/config.hpp"
#include "config/paths.hpp"
#include "demos/demo.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/session.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "http/server.hpp"
#include "log/log.hpp"
#include "memory/memory_thread.hpp"
#include "memory/recovery.hpp"
#include "memory/repository.hpp"
#include "metrics/registry.hpp"
#include "orchestrator/observability/barge_in_metrics.hpp"
#include "orchestrator/boot/bootstrap.hpp"
#include "orchestrator/stacks/capture_stack.hpp"
#include "orchestrator/stacks/dialogue_stack.hpp"
#include "orchestrator/observability/event_tracer.hpp"
#include "orchestrator/boot/model_controller_handoff.hpp"
#include "orchestrator/admin/privacy_handlers.hpp"
#include "orchestrator/admin/reload_setup.hpp"
#include "orchestrator/admin/restart_driver.hpp"
#include "orchestrator/admin/session_resume.hpp"
#include "orchestrator/boot/startup_runner.hpp"
#include "orchestrator/observability/status_extra.hpp"
#include "orchestrator/io/stdin_reader.hpp"
#include "orchestrator/stacks/stt_stack.hpp"
#include "orchestrator/observability/supervisor_setup.hpp"
#include "orchestrator/boot/system_aec.hpp"
#include "orchestrator/stacks/tts_stack.hpp"
#include "audio/apm.hpp"
#include "audio/pipeline.hpp"
#include "stt/openai_stt_client.hpp"
#include "supervisor/supervisor.hpp"
#include "supervisor/watchdog.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <variant>

int main(int argc, char** argv) {
    // M8A Step 3 — `acva memory <subcommand>` short-circuits the
    // normal startup. The CLI is a separate process from any running
    // orchestrator: it opens the DB directly via memory::Database
    // (no MemoryThread, no http server, no audio device) and exits.
    // Detected before parse_args because the memory subcommand has
    // its own flag grammar.
    if (argc >= 2 && std::string{argv[1]} == "memory") {
        return acva::cli::run_memory_subcommand(argc - 2, argv + 2);
    }

    auto args = acva::cli::parse_args(argc, argv);
    if (args.show_help) {
        acva::cli::print_help();
        return EXIT_SUCCESS;
    }

    // ----- 1. Load + resolve config -----
    auto loaded = acva::orchestrator::load_and_resolve_config(
        args.config_path, args.stdin_mode);
    if (auto* err = std::get_if<acva::config::LoadError>(&loaded)) {
        std::cerr << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto& bundle      = std::get<acva::orchestrator::LoadedConfig>(loaded);
    auto& cfg         = bundle.cfg;
    const auto config_path = bundle.config_path;
    const auto config_hash = acva::config::config_file_hash(config_path);

    // ----- 2. Initialize logging + ALSA sidestep -----
    acva::log::init(cfg.logging);
    acva::log::info("main", "acva starting");
    acva::log::info("main", fmt::format("config loaded: {}", config_path.string()));
    acva::log::info("main", fmt::format("memory db: {}", cfg.memory.db_path));
    acva::orchestrator::install_alsa_sidestep(cfg.audio);

    // Validate --stdin-lang against the configured maps. Empty maps
    // mean the feature is not configured (early-bring-up state) — only
    // gate when a map has entries at all, mirroring config::validate.
    if (args.stdin_mode && !args.stdin_lang.empty()) {
        std::vector<std::string> errs;
        if (!cfg.dialogue.system_prompts.empty()
            && !cfg.dialogue.system_prompts.contains(args.stdin_lang)) {
            errs.push_back(fmt::format(
                "dialogue.system_prompts has no entry for '{}'",
                args.stdin_lang));
        }
        if (!cfg.tts.voices.empty()
            && !cfg.tts.voices.contains(args.stdin_lang)) {
            errs.push_back(fmt::format(
                "tts.voices has no entry for '{}'", args.stdin_lang));
        }
        if (!errs.empty()) {
            std::cerr << "acva: --stdin-lang " << args.stdin_lang
                      << " is not configured:\n";
            for (const auto& e : errs) std::cerr << "  - " << e << "\n";
            std::cerr << "Add the missing entries to "
                      << config_path.string()
                      << " or pick a configured lang.\n";
            return EXIT_FAILURE;
        }
    }

    // RAII: optionally load PipeWire's module-echo-cancel and route
    // this process through it (set PULSE_SINK / PULSE_SOURCE before
    // any PortAudio init). No-op when cfg.apm.use_system_aec=false.
    // Lives at function scope so its destructor unloads the module on
    // exit (including the demo short-circuit below).
    acva::orchestrator::SystemAec system_aec(cfg.apm);
    if (const auto& err = system_aec.startup_error()) {
        std::cerr << "acva: system AEC setup failed: " << *err << "\n";
        return EXIT_FAILURE;
    }

    // M8A Step 5 — model-controller hand-off. No-op unless the
    // operator wired up the sidecar. Returns false ONLY under
    // strict_startup with a real failure; tolerant mode logs +
    // continues.
    if (!acva::orchestrator::run_model_controller_handoff(cfg)) {
        return EXIT_FAILURE;
    }

    // ----- 2.5. Demo subcommand short-circuit -----
    // Demos build only the subsystems they need from `cfg` and exit
    // when done. They MUST run before the full runtime (memory thread,
    // fsm, supervisor, ...) is constructed — otherwise we'd be paying
    // SQLite + bus + threads for a fast smoke check.
    if (!args.demo.empty()) {
        if (args.demo == "list") {
            acva::demos::print_list();
            return EXIT_SUCCESS;
        }
        const auto* d = acva::demos::find(args.demo);
        if (!d) {
            std::cerr << "acva: unknown demo '" << args.demo
                       << "' — try `acva demo` for the list.\n";
            return EXIT_FAILURE;
        }
        return d->run(cfg, args.demo_args);
    }

    acva::cli::install_signal_handlers();

    // Production-only periodic VRAM probe. Spawned AFTER the demo
    // dispatch above — demos do their own VRAM probing and a joinable
    // thread destructor on demo-return would call std::terminate.
    acva::orchestrator::VramMonitor vram_monitor(cfg.logging);

    // ----- 3. Build the runtime -----
    acva::event::EventBus bus;
    auto registry = std::make_shared<acva::metrics::Registry>();
    auto metric_subs = registry->subscribe(bus); // keep-alive

    // Memory layer: open the database (or create it) and run the recovery
    // sweep so any in-flight turns from a previous crash get cleaned up
    // before we accept new traffic.
    auto mem_or = acva::memory::MemoryThread::open(
        cfg.memory.db_path, cfg.memory.write_queue_capacity);
    if (auto* err = std::get_if<acva::memory::DbError>(&mem_or)) {
        std::cerr << "memory: " << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<acva::memory::MemoryThread>>(mem_or));

    {
        auto sweep = memory->read([](acva::memory::Repository& repo) {
            return acva::memory::run_recovery(repo, repo.database());
        });
        if (auto* err = std::get_if<acva::memory::DbError>(&sweep)) {
            acva::log::error("main",
                fmt::format("recovery sweep failed: {}", err->message));
            return EXIT_FAILURE;
        }
        const auto s = std::get<acva::memory::RecoverySummary>(sweep);
        acva::log::info("main", fmt::format(
            "recovery: closed {} sessions, marked {} turns interrupted, "
            "{} stale of {} summaries",
            s.sessions_closed, s.turns_marked_interrupted,
            s.summaries_stale, s.summaries_total));
    }

    // M8A Step 2 — SessionManager owns the active session id and
    // fans out /new-session + /wipe rollovers to every registered
    // subscriber (Manager, TurnWriter, Summarizer). Constructed
    // BEFORE the dialogue stack so the stack can register its
    // consumers; the actual open / adopt / open_initial call runs
    // AFTER the dialogue stack returns so all three subscribers
    // pick up the first id in one fan-out.
    acva::dialogue::SessionManager sessions(*memory);

    acva::dialogue::TurnFactory turns;
    acva::dialogue::Fsm fsm(bus, turns);
    fsm.set_turn_outcome_observer([registry](const char* outcome) {
        registry->on_turn_outcome(outcome);
    });
    fsm.start();

    // FSM-state metric updater.
    acva::event::SubscribeOptions fsm_metric_opts;
    fsm_metric_opts.name = "metrics.fsm_state";
    fsm_metric_opts.queue_capacity = 256;
    fsm_metric_opts.policy = acva::event::OverflowPolicy::DropOldest;
    auto fsm_metric_sub = bus.subscribe_all(fsm_metric_opts,
        [registry, &fsm](const acva::event::Event& /*e*/) {
            const auto snap = fsm.snapshot();
            registry->set_fsm_state(std::string(acva::dialogue::to_string(snap.state)).c_str());
        });
    metric_subs.push_back(fsm_metric_sub);

    // ----- Event tracer (cfg.logging.trace_events) -----
    if (auto sub = acva::orchestrator::install_event_tracer(bus, cfg.logging)) {
        metric_subs.push_back(std::move(sub));
    }

    // ----- M2: Supervisor -----
    auto supervisor_ptr = acva::orchestrator::build_supervisor(
        cfg, bus, registry, metric_subs);
    auto& supervisor = *supervisor_ptr;

    // ----- M3: TTS + playback stack -----
    //
    // Track the turn id the Manager mints for each LLM run. The
    // PlaybackEngine reads this to filter "live" chunks. Manager
    // updates it synchronously via its turn-started hook (set in
    // the dialogue stack below) BEFORE LlmStarted publishes — the
    // earlier async bus subscriber was racy and stranded the FSM
    // in Speaking when TTS chunks raced the LlmStarted subscriber.
    auto playback_active_turn =
        std::make_shared<std::atomic<acva::event::TurnId>>(acva::event::kNoTurn);

    auto tts = acva::orchestrator::build_tts_stack(
        cfg, bus, registry, playback_active_turn);
    auto* loopback_sink   = tts->loopback();

    // Forward-declared so the /status closure can read APM stats at
    // request time. The capture stack is actually built below (M4
    // section) — we declare the unique_ptr early and the lambda picks
    // up its value lazily on every /status hit.
    std::unique_ptr<acva::orchestrator::CaptureStack> capture;

    // M8A Step 1 — config hot-reload. Owns the ConfigReloader + the
    // mutex serialising HTTP-driven reloads against SIGHUP-driven
    // ones. The endpointer callback is registered later (post-capture)
    // when its target Endpointer pointer is known.
    acva::orchestrator::ReloadSetup reload(cfg, config_path);
    reload.register_log_callback();
    auto run_reload = reload.run_reload();

    // M8A Step 4 — warm-restart request flag with built-in 5 s
    // debounce. /restart and the Watchdog auto-restart both call
    // requester.request(); the main loop drains is_requested().
    acva::orchestrator::RestartRequester restart_requester;

    // M8A Step 2 — privacy handlers. The mute closure captures
    // `capture` by reference because the capture stack is built
    // BELOW; until then /mute is a no-op log line.
    auto privacy = acva::orchestrator::make_privacy_handlers(
        capture, sessions);

    // HTTP control plane (/metrics, /status, /health, POST /reload,
    // /mute /unmute /new-session /wipe /restart).
    std::unique_ptr<acva::http::ControlServer> control;
    try {
        control = std::make_unique<acva::http::ControlServer>(
            cfg.control, registry, &fsm,
            acva::orchestrator::make_status_extra(supervisor, capture),
            run_reload, std::move(privacy),
            [&restart_requester]() { return restart_requester.request(); });
    } catch (const std::exception& ex) {
        acva::log::error("main", fmt::format("control server failed to start: {}", ex.what()));
        supervisor.stop();
        fsm.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }

    // ----- M4 + M6: capture + VAD + APM pipeline -----
    // STT warm-up sits between TTS and capture: it loads the Whisper
    // model into VRAM before the mic opens so the first user turn
    // doesn't pay the model-load cost. Best-effort — failure logs
    // a warning and continues.
    if (cfg.audio.capture_enabled
        && cfg.stt.warmup_on_startup
        && !cfg.stt.base_url.empty()) {
        acva::log::info("main", fmt::format(
            "warming up STT (model={}) — blocking until ready…",
            cfg.stt.model));
        const auto r = acva::stt::warmup(cfg.stt);
        if (r.ok) {
            acva::log::info("main", fmt::format(
                "STT warm-up complete in {} ms", r.ms));
        } else {
            acva::log::warn("main", fmt::format(
                "STT warm-up failed in {} ms ({}); first user turn "
                "will pay the model-load cost", r.ms, r.error));
        }
    }
    capture = acva::orchestrator::build_capture_stack(
        cfg, bus, registry, fsm, loopback_sink);
    auto* audio_pipeline = capture->pipeline();

    // M8A Step 1 — register the endpointer reload callback now that
    // the capture pipeline (and its Endpointer) exists. No-op when
    // capture is disabled.
    reload.register_endpointer_callback(
        audio_pipeline ? audio_pipeline->endpointer() : nullptr);

    // ----- M4B + M5 STT path -----
    auto stt_stack = acva::orchestrator::build_stt_stack(
        cfg, bus, audio_pipeline, metric_subs);

    // ----- M1 + M2 + M5 dialogue + LLM stack -----
    // M7 — pass the AudioPipeline's APM (when available) into the
    // dialogue stack so the BargeInDetector can gate on convergence.
    // Null when capture is disabled or APM was built as a stub.
    const acva::audio::Apm* apm_for_barge_in =
        audio_pipeline ? audio_pipeline->apm() : nullptr;
    auto dialogue_or = acva::orchestrator::build_dialogue_stack(
        cfg, bus, registry, *memory, fsm, supervisor, turns, sessions,
        apm_for_barge_in, playback_active_turn, metric_subs);
    if (auto* err = std::get_if<acva::memory::DbError>(&dialogue_or)) {
        acva::log::error("main", fmt::format("dialogue stack failed: {}", err->message));
        fsm.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }
    auto dialogue = std::move(std::get<std::unique_ptr<acva::orchestrator::DialogueStack>>(dialogue_or));

    // Open the initial session — the dialogue stack registered Manager,
    // TurnWriter, and Summarizer as SessionManager subscribers above; this
    // call fans out the first session id to all three before traffic flows.
    // Skipped when the LLM stack is disabled (synthetic-only fake-driver
    // runs don't write to memory).
    //
    // M8A Step 4 — warm restart. Try to adopt a recent runtime_state
    // checkpoint (matching age + config_hash). Cold-open path runs
    // when adoption is impossible or unsafe.
    if (dialogue->has_llm()) {
        const bool adopted = acva::orchestrator::try_warm_resume(
            *memory, sessions, cfg, config_hash);
        if (!adopted) {
            auto sid_or = sessions.open_initial();
            if (auto* err = std::get_if<acva::memory::DbError>(&sid_or)) {
                acva::log::error("main",
                    fmt::format("session open failed: {}", err->message));
                dialogue->stop();
                fsm.stop();
                bus.shutdown();
                return EXIT_FAILURE;
            }
        }
    }

    // M8A Step 4 — passive watchdog. Subscribes to LlmToken,
    // TtsAudioChunk, SpeechStarted, FinalTranscript; on prolonged
    // inactivity in a noisy FSM state emits voice_stuck_total + a
    // structured log line, and optionally calls the restart requester.
    acva::supervisor::Watchdog watchdog(cfg.supervisor, bus, fsm, registry);
    watchdog.set_restart_fn([&restart_requester](const char* reason) {
        const auto reject = restart_requester.request();
        if (reject.has_value()) {
            acva::log::warn("watchdog", fmt::format(
                "auto-restart rejected: {} (reason={})", *reject, reason));
        } else {
            acva::log::warn("watchdog", fmt::format(
                "auto-restart requested (reason={})", reason));
        }
    });
    watchdog.start();

    // M7 — wire BargeInDetector → PlaybackEngine and start it. Wire
    // BEFORE start() so the first SpeechStarted observed during
    // Speaking can't slip past the callback.
    if (auto* bi = dialogue->barge_in(); bi != nullptr) {
        if (tts && tts->engine()) {
            bi->set_on_fired(
                [engine = tts->engine()](acva::event::TurnId turn,
                                          std::chrono::steady_clock::time_point ts) {
                    engine->note_barge_in(turn, ts);
                });
        }
        bi->start();
    }

    // M7 — RAII poller mirroring BargeInDetector counters into the
    // metrics gauges. Tiny standalone thread (1 Hz cadence).
    acva::orchestrator::BargeInMetricsPoller bi_metrics(
        dialogue->barge_in(), registry);
    bi_metrics.start();

    // ----- stdin text-input mode (M1 era) — only the line reader -----
    acva::orchestrator::StdinReader stdin_reader(bus,
        args.stdin_lang.empty() ? cfg.dialogue.fallback_language
                                 : args.stdin_lang);
    if (args.stdin_mode) {
        stdin_reader.start();
    }

    // ----- M8A Step 5: boot-time gates -----
    const bool strict_startup_failed =
        acva::orchestrator::run_startup_pass(cfg);

    // ----- 4. Main loop -----
    bool warm_restart_requested = false;
    while (acva::cli::signal_received() == 0 && !strict_startup_failed) {
        // M8A Step 4 — warm restart drain. Done on the main thread so
        // shutdown ordering against capture / dialogue / tts owners
        // (all locals here) is straightforward.
        if (restart_requester.is_requested()) {
            warm_restart_requested = true;
            break;
        }
        // SIGHUP-driven reload: the signal handler set the flag from
        // an async-signal context; we drain it here on the main loop
        // thread so reload.run_reload()() runs in a sane state.
        if (acva::cli::signal_reload_requested()) {
            acva::cli::clear_reload_request();
            const auto result = run_reload();
            using namespace acva::config;
            if (auto* ok = std::get_if<ReloadOk>(&result)) {
                acva::log::info("main", fmt::format(
                    "reload via SIGHUP: applied {} hot field(s)",
                    ok->diff.changed_hot.size()));
            } else if (auto* rej = std::get_if<ReloadRejected>(&result)) {
                acva::log::warn("main", fmt::format(
                    "reload via SIGHUP rejected: {} restart-required field(s) "
                    "changed (e.g. {})",
                    rej->diff.changed_restart.size(),
                    rej->diff.changed_restart.empty()
                        ? std::string("(none)")
                        : rej->diff.changed_restart.front()));
            } else if (auto* err = std::get_if<ReloadParseError>(&result)) {
                acva::log::warn("main", fmt::format(
                    "reload via SIGHUP failed: {}", err->message));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int sig = acva::cli::signal_received();
    if (warm_restart_requested) {
        acva::log::info("main", "warm restart requested, capturing checkpoint");
    } else {
        acva::log::info("main", fmt::format("received signal {}, shutting down", sig));
    }

    // M8A Step 4 — snapshot the FSM BEFORE we stop it so the
    // checkpoint reflects the in-flight state, not the post-stop one.
    // For non-restart shutdowns this snapshot is unused.
    const auto fsm_snapshot_before_stop = fsm.snapshot();

    // Wake the stdin reader if it's still in getline().
    stdin_reader.stop();

    // Orderly shutdown: stop producers first, then drain.
    // Each stack's stop() is idempotent and respects internal
    // teardown ordering (capture before pipeline, bridge before
    // self-listen before engine, etc.).
    if (capture)  capture->stop();
    if (stt_stack) stt_stack->stop();
    if (dialogue) dialogue->stop();
    if (tts)      tts->stop();
    bi_metrics.stop();
    watchdog.stop();
    supervisor.stop();
    fsm.stop();
    control.reset();

    // M8A Step 4 — write the runtime checkpoint AFTER all producers
    // are quiesced and BEFORE the memory thread closes; we use a
    // fresh DB connection (`checkpoint_runtime_sync`) so this works
    // even if memory.reset() has already torn down the orchestrator's
    // own connection. SQLite WAL allows the second connection.
    if (warm_restart_requested) {
        // Drain + close the orchestrator's MemoryThread before we
        // open a second connection. SQLite WAL would tolerate the
        // overlap, but closing first keeps the on-disk state
        // unambiguous for post-mortems.
        memory.reset();

        if (auto err = acva::orchestrator::write_warm_restart_checkpoint(
                cfg.memory.db_path, fsm_snapshot_before_stop, sessions,
                config_hash); err.has_value()) {
            acva::log::error("main", fmt::format(
                "warm restart: checkpoint failed: {}; aborting exec — "
                "next start will fall back to cold recovery",
                err->message));
            bus.shutdown();
            return EXIT_FAILURE;
        }
        acva::log::event("main", "warm_restart_checkpoint",
            acva::event::kNoTurn,
            {{"session_id", std::to_string(sessions.id())},
             {"fsm_state",  std::string(acva::dialogue::to_string(
                              fsm_snapshot_before_stop.state))}});
        bus.shutdown();

        // execv replaces this process. argv stays valid because main()
        // is still on the stack — no return past this call. On execv
        // failure we surface the error and exit; an external supervisor
        // (systemd) can choose to restart cold.
        ::execv(argv[0], argv);
        std::perror("execv");
        return EXIT_FAILURE;
    }
    bus.shutdown();
    // VramMonitor stops in its destructor when main returns.

    if (strict_startup_failed) {
        acva::log::error("main", "acva exiting non-zero due to strict_startup gate");
        return EXIT_FAILURE;
    }
    acva::log::info("main", "acva exited cleanly");
    return EXIT_SUCCESS;
}
