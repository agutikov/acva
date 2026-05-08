# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

**acva** — Autonomous Conversational Voice Agent. A local, production-grade C++ voice assistant for multi-hour conversations on a single workstation (RTX 4060). All inference is local; model backends (llama.cpp, whisper.cpp, Piper) run as **Docker Compose containers** in dev (with systemd as an alternative production path); the C++ orchestrator runs as a host CLI binary and owns control plane, audio I/O, state, memory, and observability.

Target hardware: Linux x86_64, RTX 4060 8 GB. Speakers + AEC is the primary UX (not headphones).
Multilingual STT/TTS/LLM with auto language detection per turn.
Streaming partial STT with speculative LLM start (M5 — three options on the table; see `plans/open_questions.md` L1).

## Status

**M0 + M1 + M2 + M3 + M4 + M4B + M5 + M6 + M6B + M7 + M7B + M8A + M8B (code-complete) complete.** Test suite split into `acva_unit_tests` (365 cases, no external deps) and `acva_integration_tests` (13 cases, real Silero model + live Speaches). Both pass green on the dev workstation without any env vars; integration tests resolve assets via the XDG defaults main.cpp uses. **Compose stack is `llama` + `speaches` only** — Speaches replaces standalone `whisper.cpp/server` (STT) and `piper.http_server` (TTS) behind one OpenAI-API-compatible surface. TTS goes through `OpenAiTtsClient` (libcurl streaming PCM); STT through `OpenAiSttClient` (multipart `POST /v1/audio/transcriptions`, blocking, request/response — M5 swaps for streaming/realtime). `PlaybackEngine` carries a per-turn pre-buffer threshold (`cfg.playback.prefill_ms`, default 100 ms) that absorbs streaming-TTS chunk-arrival jitter — measured 56–71% fewer underruns without any total-latency regression. Demos `acva demo {tts,chat,stt}` exercise the new wiring; `acva demo stt` is a self-contained TTS-fixture-audio → STT round-trip. STT model is `deepdml/faster-whisper-large-v3-turbo-ct2` (turbo) with `WHISPER__COMPUTE_TYPE=int8_float16` and `WHISPER__TTL=-1` (set in compose env); the TTL pin is non-negotiable on the 8 GB RTX 4060 — faster-whisper #992 leaks ~300 MB per unload cycle, and Speaches' default 5-min auto-evict otherwise compounds across consecutive runs until inference OOMs. See memory note `project_gpu_cdi_and_vram.md` for the budget rationale. **Model registry (post-M7).** `config/default.yaml` carries a top-level `models:` block — LLM / STT / TTS / VAD catalogs of short aliases (`dialog`, `large-v3-turbo`, `en-amy`, `silero-v5` …). Subsystem fields (`cfg.llm.model`, `cfg.stt.model`, `cfg.tts.voices.<lang>`, `cfg.vad.model_path`) accept aliases that `config::resolve_aliases()` rewrites to backend-specific locators at config-load (HF id for STT/TTS, filename for VAD; LLM stays the alias as the OpenAI-endpoint label until M8A makes it load-bearing). `cfg.tts.voices` is now a `map<lang, alias_string>` parsed from YAML; `cfg.tts.voices_resolved` is the `map<lang, TtsVoice>` callers read. Aliases must resolve through the registry — pre-registry `voices: { en: { model_id, voice_id } }` no longer parses. `tools/acva-models` (Python 3, PyYAML) reads the same registry and installs assets via `list`/`install`/`sync`/`verify` subcommands; the old `scripts/download-{llm,stt,tts,vad,assets}.sh` were deleted.

**M8A (admin/state) closed code-complete 2026-05-07.** Five steps shipped:
**Step 1 — config hot-reload:** `src/config/reload.{hpp,cpp}` carries the
`ReloadDiff` + field catalog (hot fields: `llm.{temperature,max_tokens}`,
`dialogue.{max_assistant_sentences,max_tts_queue_sentences}`,
`vad.{onset_threshold,offset_threshold,hangover_ms}`, `tts.tempo_wpm`,
`logging.level`); restart-required surface covers endpoints, model ids,
paths, ports, devices. `POST /reload` returns 200/409/400; SIGHUP raises
the same path (split off the shutdown handler in `cli/args`). Endpointer
gains `update_thresholds()` (mutex-guarded; on_frame snapshots the cfg
under the same lock so concurrent reloads don't tear float reads).
`acva demo reload` exercises the HTTP roundtrip end to end.
**Step 2 — privacy commands:** new `dialogue::SessionManager` owns the
active session id and fans out to subscribers (Manager / TurnWriter /
Summarizer); `dialogue_stack` no longer opens a session inline. `POST
/mute|/unmute|/new-session|/wipe?session=<id>|/wipe?all=true` on
`ControlServer`; bare `/wipe` returns 400. `AudioPipeline::set_muted()`
short-circuits process_frame and force-endpoints any in-progress
utterance. `Repository::delete_session` cascades via FK; `wipe_all`
DROPs + re-execs `kSchemaSql` in one transaction. `acva demo wipe`
covers session + all-wipe + the no-qualifier guard.
**Step 3 — memory CRUD CLI:** `src/cli/memory_cli.{hpp,cpp}` implements
`acva memory <subcommand>` with sessions/session/turns/turn/facts/
summaries/delete-*/wipe/vacuum/restart. Process-isolated from the
orchestrator (opens `Database` directly; no MemoryThread); read paths
coexist with a live acva via SQLite WAL, write paths block on
`busy_timeout=5000`. `--json` for jq; `--yes` for wipe; `--dry-run` on
deletes.
**Step 4 — watchdog + checkpointed restart:** new `runtime_state`
singleton table; `Repository::{upsert,read,clear}_runtime_state` plus
the free `memory::checkpoint_runtime_sync(db_path, row)` helper for the
pre-execv path. `src/supervisor/watchdog.{hpp,cpp}` subscribes to
LlmToken / TtsAudioChunk / SpeechStarted / FinalTranscript and fires
`voice_stuck_total{state}` once per state-episode when the FSM is in
{Transcribing, Thinking, Speaking, Interrupted} for longer than
`cfg.supervisor.stuck_threshold_seconds` (default 90 s). `POST /restart`
+ `acva memory restart` route through `RestartRequester` (5 s
debounce); the main loop drains the flag, snapshots the FSM, runs the
shutdown chain, writes the checkpoint, and `execv`s. Resume gate on
startup gated by age + FNV-1a config hash (`config_file_hash`); SessionManager
gains `adopt(id)`. Warm restart preserves session continuity (same
`session_id`, prior turns visible to the LLM); the FSM starts fresh in
Listening (no `last_partial` replay yet). Auto-restart on stuck is
opt-in via `cfg.supervisor.auto_restart_on_stuck`.
**Step 5 — boot-time model orchestration:** `src/llm/model_controller_client.{hpp,cpp}`
talks to a Go sidecar at `cfg.llm.model_controller_url` (default
`http://127.0.0.1:9877`); when `cfg.llm.model_file` differs from the
sidecar's currently-loaded GGUF, acva calls `POST /llm/load` and waits.
Pillar #1 stands — the sidecar (in `packaging/model-controller/`,
single-file Go + Dockerfile) holds the docker socket privilege. The
service is **not** auto-added to `packaging/compose/docker-compose.yml`
because the bind-mount is a deliberate operator opt-in; the README has
the paste-in stanza. `src/supervisor/startup_check.{hpp,cpp}` runs the
force-load gates (LLM 1-token, STT silent-WAV, TTS 5-char, capture
PortAudio probe); under `cfg.supervisor.strict_startup` failures abort
with `EXIT_FAILURE` after orderly shutdown. Both `strict_startup` and
`startup_force_load` default false to preserve M0–M7 dogfooding
ergonomics.
**Refactor pass closing M8A:** main.cpp's bookkeeping moved into ten
new `src/orchestrator/` helpers — `model_controller_handoff`,
`reload_setup`, `privacy_handlers`, `restart_driver`, `session_resume`,
`startup_runner`, `barge_in_metrics`, `stdin_reader` (joining
`bootstrap`, `tts_stack`, `capture_stack`, `stt_stack`, `dialogue_stack`,
`event_tracer`, `status_extra`, `supervisor_setup`, `system_aec`).
main.cpp shrunk from 776 → 526 lines while picking up all five M8A
steps (vs the M0/M1-era ~280 baseline; the residual growth is genuine
new wiring — warm-restart, watchdog, reload setup, startup gates).

End-to-end smoke of the model-controller (set `cfg.llm.model_file` →
sidecar recreates llama → acva picks up the new model) is deferred to
a manual dogfood pass once the sidecar is wired into the user's
compose stack; the C++ side is unit-tested against an in-process fake
controller.

**M8B (observability/soak) closed code-complete 2026-05-07.** Four steps shipped:
**Step 1 — soak harness:** `acva demo soak-mini` (60 s FakeDriver-only
pre-flight) + the full 4-hour harness (`scripts/soak.sh` + stdlib-only
`scripts/soak-driver.py` + `tests/soak/prompts.txt`). Driver spawns
`acva --stdin`, feeds random prompts at 5–30 s intervals, polls
`/metrics` + `/status` every 5 s into CSV, runs `docker compose
restart speaches` on the `voice_speaches_wedged` rising edge (60 s
cooldown), produces `soak-report.txt` with the four acceptance gates
(no_crashes / rss_growth / queue_depth_stable / service_restarts_ok).
Smart gate semantics: `rss_growth_under_limit` reports `n/a` on runs
< warmup+10 m so smoke runs don't spuriously fail. End-to-end smoke
verified at 60 s. The headline 4-hour acceptance run is the dogfood
gate.
**Step 2 — Grafana dashboard + observability stack:**
`packaging/observability/docker-compose.yml` brings up Prometheus 2.55
+ Grafana 11.3 with `network_mode: host` so they reach the host's
loopback-only acva (:9876) + llama (:8081) without bridge translation.
Auto-provisioned: Prometheus datasource, file-provider dashboard at
`packaging/grafana/acva.json` (7 panels: FSM state, speaches VRAM +
wedged, TTS first-audio P50/P95, service health, playback queue +
underruns, watchdog stuck + barge-in, pipeline state). Live-verified
end-to-end with a brief acva session — both targets `health=up`,
PromQL returns the right state series, the wedge classifier reads
the running speaches at 1212 MiB. The architecture is documented in
`docs/observability.dot` (5-cluster colored diagram, also rendered to
SVG + PDF).
**Speaches CUDA-OOM wedge detection (Known Issues item):**
`voice_speaches_vram_used_mib` + `voice_speaches_wedged` exposed by
the VramMonitor each tick. Pure classifier in
`src/observability/speaches_wedge.{hpp,cpp}` — parses `nvidia-smi
--query-compute-apps` CSV, resolves PIDs to cmdlines via an
injectable `CmdlineLookup`, returns the first match whose cmdline
contains "speaches". Live monitor wires popen + `/proc/<pid>/cmdline`.
`/status` adds a `speaches` block when the monitor has data, with a
`remediation` hint when wedged. 9 unit tests for the classifier.
**Step 3 — OTLP traces (opt-in):** `cfg.observability.otlp.{enabled,
endpoint, service_name}` + `find_package(opentelemetry-cpp)` →
`ACVA_HAVE_OTLP`. `src/observability/otlp.{hpp,cpp}` wraps an
`OtlpHttpExporter` + `BatchSpanProcessor` + `TracerProvider` behind
an otel-free header (every otel type erased via
`shared_ptr<void>` + a `SpanHolder` adapter); compiles to a no-op
stub when the dep is missing.
`src/observability/turn_span.{hpp,cpp}` brackets each turn with one
`voice.turn` span — start on `LlmStarted`, close on
`PlaybackFinished` (ok) / `UserInterrupted` (interrupted) /
`LlmFinished{cancelled}` (llm_cancelled). Child spans (vad / stt /
prompt / llm / tts / playback) are deferred — wiring is mechanical
but not load-bearing for v1.
**Step 4 — build-time reductions:** Honest results — clean-build
wall stayed at ~215 s (Glaze's `read_yaml<Config>` is one big
template instantiation that doesn't decompose across cores; the
plan's "< 60 s wall" target was unrealistic). What did improve:
**incremental builds**. Split `config.cpp` into a Glaze-free
validators TU (~5 s rebuild on validator edits, was 211 s) and the
Glaze-heavy `config_load.cpp`. Added `tests/test_pch.hpp` via
`target_precompile_headers`; single test-file edits now incremental
in **~2.8 s** (was ~5–8 s). pImpl audit found the listed HTTP-lib
consumers were already pImpl'd from M3/M5. Documented the
glaze-replacement option (rapidyaml + hand-rolled mapping, days of
work for ~100 s save) as a future optimisation when config.hpp
iteration becomes painful again.

**Operator stack:** `packaging/observability/` + `packaging/grafana/`
are the operator-installed observability surface. Optional, separate
from the inference compose (which stays minimal per pillar #1).
`docs/observability.dot/.svg/.pdf` documents the topology.

**M8C — distribution + wake-word — code-complete + closed 2026-05-08.** Tier 1 (registry catalog + aliases, observability counters, hot-reload, `dev-up.sh`/`dev-down.sh`, systemd units), Tier 2 (real openWakeWord 3-stage ONNX inference — `src/audio/wake_word.cpp` mel→embedding→classifier with 2.6 s warm-up; auto-discovers `melspectrogram.onnx`+`embedding_model.onnx` alongside each classifier; smoke-tested at `tests/test_wake_word_smoke.cpp`), and Tier 3 (`acva demo wake-word` live-mic harness; `tools/train-wake-word` Python driver — synthesizes positives via Speaches `/v1/audio/speech`, calls openwakeword's `compute_features_from_generator` + `train_model` opt-in pip dep, registers under `${XDG_DATA_HOME}/acva/models/wake_word/`; `--prepare-only` for split-host synthesis/training) all closed. **Wake-word ships off-by-default in `config/default.yaml`.** Debug round confirmed the engine works on real audio (offline replay peaks 0.37–0.47 on Russian-accented "hey jarvis"), but the architectural review concluded wake-word is the wrong primitive for acva's "multi-hour conversational" product line — the right gate is **address detection** (M10), and wake-word's pipeline gate seam (`audio/pipeline.cpp:174-208`) is the runway M10 will plug into. Framework, demos, trainer, registry, and observability stay built; `enabled: true` remains the one-line opt-in for sleep-and-wake mode. Full reasoning in `plans/open_questions.md` §L8 and `plans/milestones/m8c_distribution.md` Step 1 close-out. New debug aid: `acva demo wake-word-offline --wav <path>` + `scripts/wake-word-offline.sh` (records raw + AEC sources in parallel, runs each through STT + wake-word) — useful as a generic mic-and-routing diagnostic, will outlive M8C. M7B (barge-in validation) closed code-complete 2026-05-05: synthetic-fixture-driven acceptance suite at `scripts/validate-bargein.py` runs four manifest fixtures (clean-speakers, noise-speakers, headphones, false-positive-self) through `acva demo bargein-validation --fixture <wav>` and parses the structured done-line for cancellation outcome + cascade latency + persistence semantics. Latencies measured 54-68 ms (well inside M7 §19.3 P50 ≤ 200 / P95 ≤ 400 gates). The seam is `cfg.audio.test_input_wav`: when set, `audio::CaptureEngine` bypasses PortAudio and pumps the WAV's int16 mono samples through the SPSC ring at real-time pace, so Resample → APM → VAD → Endpointer → BargeInDetector run on the production code path. Fixture synthesis at `tools/build-bargein-fixtures` mixes Speaches TTS tracks per a YAML manifest (idempotent, sha256-keyed). The original C++-doctest probe approach was dropped in favour of subprocess + log parsing because main.cpp's stack construction is ~280 LOC and replicating it as a parallel test fixture was net-loss. The original `false-positive-tv` fixture was dropped because content-based suppression (TV-news vs the actual user) requires M8C wake-word or M10 address detection — outside M7's intentional scope. Hardware spot-check (M7B Step 7, real mic + speakers) deferred to dogfood. M7 (barge-in) closed code-complete 2026-05-04: BargeInDetector (`src/dialogue/barge_in.{hpp,cpp}`) subscribes to `SpeechStarted`, snapshots FSM state, and promotes events that arrive while `Speaking` (past the AEC + cooldown gates) into `UserInterrupted`. `voice_barge_in_latency_ms` histogram + `voice_barge_in_fires_total/suppressed_total{cause}` gauges land on `/metrics`. TurnWriter now persists only sentences whose `PlaybackFinished` was observed before the cancel — a synthesized-but-not-played sentence is dropped, matching §6 of project_design.md. `min_real_utterance_chars` (default 3) filters cough/throat-clear FinalTranscripts that would otherwise cost an LLM call. Carry-over bugs landed: pre-padding now replayed into the M5 streaming sink at SpeechStarted (Bug 1); LLM cap cancels at the next *sentence* boundary instead of the next token, so the user never hears a half-thought (Bug 2); RMS gate on `UtteranceReady` (default 200 ≈ -45 dBFS) suppresses the Whisper subtitle hallucinations on near-silent buffers (Bug 4). Bug 3 (Speaches `prefix_padding_ms` warn) left in place — Speaches' Pydantic schema requires the field even though Speaches itself rejects it; the warn line is harmless. The 50-trial recorded validation suite (Step 5) is deferred to a manual dogfood pass when wired up to real mic + speakers. M6B closed 2026-05-04 via Path B (PipeWire `module-echo-cancel` upstream of acva): gate 4 = 25-46 dB speech-band cancellation (`acva demo aec-record` + `scripts/aec_analyze.py`), gate 1 = 0.200/min false-starts vs 1.0/min threshold (`scripts/soak-vad-falsestarts.sh --quick`), gate 3 = 5/5 clean transcripts during continuous TTS (`scripts/barge-in-probe.py`, now self-contained). The `--stdin-lang ru` flag wires synthetic stdin sessions to the Russian system_prompt + voice and hard-fails on misconfigured langs. The system-AEC RAII helper (`src/orchestrator/system_aec.cpp`) parses `source_name=` / `sink_name=` from `pactl list short modules` when reusing an existing module, adopts ownership when names match the `acva-echo-*` convention so a prior crash gets cleaned up on next clean exit, and refuses to start when args are unparseable — silent fallback was the original gate-1 false-pass mode (33/min). See `docs/aec_report.md` for the full M6 + M6B analysis.

**M6 (AEC):** PlaybackEngine now taps the chunk it just emitted into
an `audio::LoopbackSink` ring (sized by `cfg.audio.loopback.ring_seconds`,
default 2 s). The capture pipeline inserts an APM stage between
resample and VAD: `SPSC ring → Resample → APM → VAD → Endpointer`.
APM (`audio::Apm`) wraps `webrtc::AudioProcessing` from the system
package `webrtc-audio-processing-1` 1.3 (BSD-3, Arch `extra`); the
build gates on `ACVA_HAVE_WEBRTC_APM` and falls back to a
pass-through stub when missing. `voice_aec_delay_estimate_ms`,
`voice_aec_erle_db`, and `voice_aec_frames_processed_total` join
`/metrics`; `/status` gains an `apm` block. The compose stack reaches all-three-healthy on the dev workstation; `acva --stdin` drives a real LLM end-to-end via `PromptBuilder` → `LlmClient` (libcurl SSE) → `DialogueManager` → `SentenceSplitter` → `LlmSentence` events → `TurnWriter` → SQLite, and (when `cfg.tts.voices` is non-empty) onward through `TtsBridge` → `PiperClient` → `Resampler` → `PlaybackQueue` → `PlaybackEngine`. M4 adds the capture path: when `cfg.audio.capture_enabled: true`, `CaptureEngine` (PortAudio input) → SPSC ring → `AudioPipeline` worker → `Resampler` (48 → 16 kHz) → `SileroVad` (optional, ONNX Runtime) → `Endpointer` → `UtteranceBuffer` → `SpeechStarted` / `SpeechEnded` / `UtteranceReady` events on the bus. The fake driver gains a `suppress_speech_events` flag so real VAD can own those events while synthetic FinalTranscript/LlmSentence keep flowing for end-to-end smoke tests. Two new demos ship: `acva demo loopback` (mic → speakers passthrough) and `acva demo capture` (mic + VAD endpointing report). Manager enforces `max_assistant_sentences` by cancelling the LLM stream once the cap is hit; `UserInterrupted` drains the bridge's pending queue and clears the playback queue. JSON-per-line logs on stderr; `voice_llm_*` / `voice_health_*` / `voice_pipeline_state` / `voice_llm_keepalive_total` / `voice_tts_first_audio_ms` / `voice_tts_audio_bytes_total` / `voice_playback_{queue_depth,underruns_total,chunks_played_total,drops_total}` emit on `/metrics`; `/status` includes `pipeline_state` + `services[]`. Supervisor probes each backend's `/health`, runs the per-service state machine, gates the dialogue path when a critical backend is unhealthy past the grace window, and runs LLM keep-alive while idle. Next: **M5 — STT** (streaming Whisper via `whisper-server` or Speaches; subscribes to `UtteranceReady` and publishes `PartialTranscript` / `FinalTranscript`).

## Repository Layout

```
src/                     — C++ source. Per-subsystem subdirs: audio/, cli/, config/,
                            dialogue/, event/, http/, llm/, log/, memory/, metrics/,
                            orchestrator/, pipeline/, playback/, stt/, supervisor/, tts/.
src/main.cpp             — linear orchestration (~525 lines as of M8A): parse args →
                            load config → demo dispatch → build per-subsystem stacks
                            via orchestrator/ helpers → main loop with reload + warm-
                            restart drain → orderly shutdown (+ checkpoint+execv on
                            warm-restart path). The body of each conceptual step is
                            in an orchestrator/ helper; main.cpp owns the locals and
                            their lifetimes.
src/orchestrator/        — host-side glue, organised by role into 5 subdirs:
                            stacks/        — tts_stack, capture_stack,
                                              stt_stack, dialogue_stack. Each is
                                              a non-copyable RAII bundle whose
                                              build_*() returns with a stop()
                                              that runs the right teardown
                                              order.
                            boot/          — runs once before/at startup:
                                              bootstrap (config + ALSA + VRAM
                                              monitor), system_aec (PipeWire
                                              module-echo-cancel RAII),
                                              model_controller_handoff (M8A
                                              sidecar pre-call), startup_runner
                                              (M8A force-load gates).
                            admin/         — M8A admin/state surface helpers:
                                              reload_setup, privacy_handlers,
                                              restart_driver, session_resume.
                            observability/ — bus subscribers + pollers:
                                              event_tracer, status_extra,
                                              barge_in_metrics, supervisor_setup.
                            io/            — input drivers: stdin_reader.
                            All still in `acva::orchestrator` namespace; the
                            split is purely navigational. main.cpp owns the
                            locals + their lifetimes; each helper isolates one
                            chunk of wiring.
tests/                   — doctest-based suites: acva_unit_tests (no deps) +
                            acva_integration_tests (real Silero model + future Speaches).
config/default.yaml      — default runtime config + `models:` registry
                            (LLM/STT/TTS/VAD aliases).
cmake/                   — Dependencies.cmake, Warnings.cmake.
third_party/cpp-httplib/ — vendored single-header HTTP server lib.
scripts/                 — one-shot dev shell scripts (soak-vad-falsestarts.sh,
                            barge-in-probe.py, aec_analyze.py, etc).
tools/acva-models        — Python 3 CLI that reads the same `models:` registry
                            and installs LLM/STT/TTS/VAD assets. Subcommands:
                            list, install, sync (install what cfg references),
                            verify. Replaces the old per-type bash downloaders.
                            Requires PyYAML.
packaging/
  systemd/               — alternative production deployment: per-user units (M2 stretch / M8).
  compose/               — dev default: docker-compose.yml. Two services: `llama` + `speaches`
                            (Speaches is the OpenAI-API-compat backend that consolidates STT + TTS).
plans/
  project_design.md      — source of truth for architecture, components, milestones, risks.
  open_questions.md      — resolved/unresolved decisions; section L holds implementation-driven revisions.
  milestones/            — one detailed plan per milestone (m0_skeleton.md, m1_llm_memory.md, ...).
  architecture_review.md, local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md — historical inputs.
CMakeLists.txt, CMakePresets.json
README.md, CLAUDE.md, LICENSE, .editorconfig, .gitignore
compile_commands.json    — symlink to _build/dev/compile_commands.json (for clangd).
build.sh                 — `./build.sh [dev|debug|release]`.
run_tests.sh             — `./run_tests.sh [dev|debug]`. Runs the **unit** suite
                            (`acva_unit_tests`): no external deps, fast feedback.
run_integration_tests.sh — `./run_integration_tests.sh [dev|debug]`. Runs the
                            **integration** suite (`acva_integration_tests`): real
                            on-disk assets (Silero model today, more later) and
                            real local services. Resolves dep paths via the same
                            XDG defaults main.cpp uses, so on the dev workstation
                            no env vars are required. Missing deps cause individual
                            cases to skip cleanly, never fail.
src/demos/               — `acva demo <name>` smoke checks per milestone (tone/tts/llm/health/fsm/chat/loopback/capture/stt).
docs/troubleshooting.md  — symptom-first guide; routes failures to the right `acva demo` and reads its output.
```

## Authoritative Documents

- **`plans/project_design.md`** is the source of truth for architecture, components, threading model, latency budget, milestones, and risks. Reference its section numbers when discussing trade-offs.
- **`plans/milestones/m{0..8}_*.md`** are the detailed per-milestone plans. The summary in `project_design.md` §17 links each. **`plans/open_questions.md` section L** holds implementation-driven revisions that supersede earlier sections — read L before assuming an earlier interview answer is still in force.
- **`plans/open_questions.md`** lists unresolved decisions. Before recommending a choice that touches one of these questions, check the default assumption there. If the user is making a real decision, update the question's status in that file.
- The two earlier docs (`architecture_review.md`, `local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md`) are historical inputs; prefer `project_design.md` when they conflict.

## Architectural Pillars (Do Not Violate Without Discussion)

1. **Process isolation for model backends.** llama.cpp, whisper.cpp, Piper run as separate processes the orchestrator does **not** fork. In dev (default since M1.B): Docker Compose containers using upstream images verbatim (`ghcr.io/ggml-org/llama.cpp:server-cuda` etc.). In production (M8 alternative): systemd units with optional sd-bus client (gated by `-DACVA_ENABLE_SDBUS=ON` + `cfg.supervisor.bus_kind`). The orchestrator never `popen`s or `fork()`s a backend. Don't propose embedding model runtimes until post-M8.
2. **Realtime audio path is sacred.** Audio callback never blocks, never allocates, never does I/O or HTTP. SPSC ring buffer is the only mechanism crossing the audio thread boundary.
3. **Cancellation is structural.** Every long-running operation carries a turn ID + cancellation token. TTS audio chunks carry sequence numbers; stale chunks are rejected at enqueue and dequeue. Speculative LLM runs use the same machinery.
4. **Backpressure everywhere.** Every async boundary uses bounded queues with explicit overflow policy.
5. **Observability from day one.** Per-turn trace IDs, structured JSON logs (M1 slice 3 — currently structured plain text), Prometheus metrics on `/metrics`, OTLP traces (opt-in) — not retrofitted later.
6. **Crash recovery is a state machine, not error handling.** SQLite turn lifecycle (`in_progress`, `committed`, `interrupted`, `discarded`) + startup recovery sweep.
7. **Speaker mode + AEC is primary, not optional.** AEC reference-signal alignment is non-negotiable; M6 is a hard prerequisite to M7.
8. **Supervisor observes, Compose acts.** The Supervisor in dev mode runs HTTP `/health` probes and gates the dialogue path; it does NOT issue restart commands — `restart: unless-stopped` in compose handles that. Same Supervisor logic in production with optional sd-bus extension.
9. **No CI.** Tests run locally. Discipline-based; no automated PR gates.

## Tech Stack (Locked for MVP)

- C++23 standard library + language. Lock is specifically against **Boost.Cobalt + C++23 modules**, not C++23 STL features. Glaze 7.x requires C++23 transitively. `std::expected`, `std::print`, deducing `this`, etc. are fair game. Do NOT enable C++23 modules; do NOT use Cobalt.
- Boost.Asio for async, **not** Boost.Cobalt.
- cpp-httplib (vendored at `third_party/cpp-httplib/`) for simple HTTP server + non-streaming client. **libcurl** for SSE streaming (LLM client in M1).
- PortAudio + soxr (M3/M4).
- WebRTC APM (vendored) for AEC/NS/AGC (M6).
- Silero VAD via ONNX Runtime (M4).
- SQLite (WAL mode) + dedicated memory thread (M1).
- glaze for JSON + YAML (config + IPC payloads).
- spdlog (structured-text in M0/M1.A; JSON sink in M1.s3), prometheus-cpp, opentelemetry-cpp (OTLP, opt-in M8).
- Docker Compose for dev backend deployment; **no custom Dockerfiles** in M1.B.
- systemd + libsystemd (sd-bus) — optional production path, gated by `-DACVA_ENABLE_SDBUS=ON`. Not a default M2 dependency.
- doctest for tests.
- CMake + presets.

If you find yourself recommending Boost.Cobalt, C++23 modules, an embedded inference engine for MVP, or a custom HTTP wrapper around a backend that already ships one — re-read `project_design.md` §16 and the open questions first.

## Milestone Order (Adjusted from Original)

`M0 skeleton → M1 LLM+memory (split: A complete, B Compose stack, C remaining) → M2 supervision → M3 TTS+playback → M4 audio+VAD → M4B Speaches consolidation → M5 STT → M6 AEC → M6B AEC hardware verification + system-AEC fallback → M7 barge-in → M7B barge-in validation → M8A admin/state ✅ → M8B observability/soak ✅ → M8C distribution + wake-word → M9 streaming partials + speculative LLM → M10 conversational UX (adaptive endpointer + address detection)`

Three reorderings / insertions vs. the original plan, all intentional:
- **Supervision (M2) before TTS (M3)** — llama.cpp will crash during long-context dev; retrofitting supervision is painful.
- **AEC (M6) before barge-in (M7)** — without AEC the assistant's own voice triggers VAD; you'll spend a week debugging phantom interruptions. M6B inserted because M6's in-process APM doesn't fully cancel on the dev laptop's codec; barge-in needs *working* AEC, not just wired AEC.
- **M4B Speaches consolidation between M4 and M5** — Speaches packages STT + TTS behind one OpenAI-compatible surface and matches CLAUDE.md pillar #5 ("don't write a custom HTTP wrapper around a backend that already ships one"). Doing this swap *before* M5 closes the M5 L1 decision (A/B/C) up front and lets M5 focus on streaming partials + speculation rather than mixing engine selection in. See `plans/milestones/m4b_speaches_consolidation.md`.

Don't propose moving these back without strong reasons.

**M1 is split** in `plans/milestones/m1_llm_memory.md`: Part A (config, memory, splitter — landed), Part B (Compose stack — next), Part C (LLM client, Dialogue Manager, turn writer, JSON logging — slice 2/3).

## When Working on Code

- **No unbounded queues.** If you need a queue, specify capacity and overflow policy.
- **No blocking calls in the audio callback path.** Even logging — use a lossy queue.
- **Every cancellable operation takes a `TurnContext`.** Don't add a "global cancel flag" or per-component bool.
- **Memory writes go through the memory thread.** Don't open SQLite from random threads — `MemoryThread::submit/read/post` is the only path.
- **HTTP client choice matters.** cpp-httplib for non-streaming, libcurl for SSE. Don't unify them on one library "for simplicity" without measuring.
- **Don't fork backends from the orchestrator.** They run as Compose containers (dev) or systemd units (prod alternative). The orchestrator only opens HTTP connections to them.
- **Don't write a custom HTTP wrapper around a backend that already ships one.** llama.cpp ships `llama-server`; Speaches ships an OpenAI-API-compatible STT + TTS surface that we use for both ends. Streaming-Whisper (`/v1/realtime`, WebRTC) lands in M5.
- **Language flows through the pipeline.** STT detects language → Dialogue Manager passes it to PromptBuilder + TTS. Voice selection is per-language.
- **Errors are silent.** Voice agent never speaks errors to the user. Logs and `/status` are the only error channels (configurable for debug).
- **Tests for the SentenceSplitter must include**: abbreviations (`Dr.`, `e.g.`), decimals (`3.14`), enumerations (`1.`, `2.`), code fences, ellipses, very-long-no-punctuation flush, and **multilingual** boundary cases.
- **Tests for the Dialogue FSM must be table-driven**, asserting all transitions including barge-in cancellation propagation and speculation-reconcile/restart paths.
- **Use `std::variant<T, DbError>` and similar `Result<T>` patterns** for fallible APIs, not exceptions across module boundaries. Internal helpers may throw; public-facing methods return Result.

## Latency Budget (Realistic)

P50 end-to-end (user-stop → first-audio): **~1.7 s.** P95: **~3.5 s.** The original "1–2 s" target was a P50 figure for short prompts. When discussing performance, use percentiles, not point estimates.

## Build & Run

Build:

```sh
./build.sh                # = dev preset; output under _build/dev/
./build.sh debug          # ASan/UBSan-friendly build; _build/debug/
./build.sh release        # -DNDEBUG, tests off; _build/release/
./run_tests.sh            # build + run unit suite for the dev preset
./run_tests.sh dev --test-case='paths*'   # filter pass-through to doctest
./run_integration_tests.sh                # build + run integration suite (Silero, etc.)
```

Equivalent raw cmake invocations (the scripts wrap these):

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run (dev path, with backends in Compose):

```sh
cd packaging/compose && docker compose up -d
cd ../..
./_build/dev/acva                                  # picks up config + db via XDG
```

Run (without backends, M0 fake-driver mode):

```sh
./_build/dev/acva                                  # fake_driver_enabled: true is the default
# in another terminal:
curl http://127.0.0.1:9876/status
curl http://127.0.0.1:9876/metrics
```

**Path resolution (M2.x).** When `--config` is omitted, `config::resolve_config_path` searches in order: `$XDG_CONFIG_HOME/acva/default.yaml` (default `~/.config/acva/default.yaml`), `./config/default.yaml` (in-tree dev fallback), `/etc/acva/default.yaml`. SQLite path: `cfg.memory.db_path` empty/relative resolves to `$XDG_DATA_HOME/acva/<value>` (default `~/.local/share/acva/acva.db`). Parent dirs are auto-created on first run. Tests in `tests/test_paths.cpp` cover the precedence + fallback rules.

The `acva` binary itself always runs on the host as a CLI process — it's intentionally never put inside Compose so the realtime audio path stays direct. Production-style packaging as `acva.service` is M8 work.

## Claude-Specific Working Notes

- Prefer editing `plans/project_design.md`, `plans/open_questions.md`, and the relevant `plans/milestones/m*_*.md` over creating new design docs.
- When user asks to "remember" a decision about an open question, update `plans/open_questions.md` (mark resolved, record the answer) — that's project state, not memory. New revisions go in section L.
- When the user asks for design changes, edit `plans/project_design.md` and the affected milestone plans directly; don't write a separate change-proposal file.
- Don't generate planning/decision/analysis docs unless explicitly requested.
- After each implementation slice, update the relevant milestone plan to mark steps as ✅ landed and capture lessons learned (see `m0_skeleton.md` and `m1_llm_memory.md` Part A as templates).
- When clangd flags errors that look like missing-include cascades, the most common cause is a stale `compile_commands.json`. The build itself is the truth — a clean `cmake --build --preset dev` is the verdict, not editor diagnostics.
