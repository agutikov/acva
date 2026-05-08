# acva — Architecture

A condensed primer for contributors. The full source-of-truth is
`plans/project_design.md`; this doc distils what you need to navigate
the codebase. Implementation-driven revisions live in
`plans/open_questions.md` §L — when this doc and §L disagree, §L wins.

## What it is

A C++23 voice agent that runs entirely on local hardware. The
**orchestrator** is a single CLI binary that owns audio I/O, dialogue
state, memory, cancellation, and observability. **Model backends**
(LLM, STT, TTS) run as separate processes the orchestrator reaches over
HTTP — never as forked subprocesses. Default deployment is Docker
Compose; systemd is the alternative production path.

## The big picture

```
                              ┌────────────────────────────────────────┐
                              │              acva (host CLI)           │
                              │                                        │
   PortAudio in ──► SPSC ring ─► Resample ─► APM ─► VAD ─► Endpointer ─┼─► Utterance Buffer ─► STT (streaming)
                                                                        │                          │
                                                                        │                          ▼
                                                                        │                FinalTranscript
                                                                        │                          │
                                                                        │                          ▼
                                                                        │                  Dialogue FSM ◄──┐
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │                   Memory thread  │
                                                                        │                   (SQLite WAL)   │
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │                   Prompt builder │
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │                  LlmClient (SSE) │
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │                  SentenceSplitter│
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │                     TtsBridge    │
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │             OpenAiTtsClient (HTTP stream)
                                                                        │                          │       │
                                                                        │                          ▼       │
                                                                        │                   Resample ► Playback Queue ─► PortAudio out
                                                                        │                                  │
                                                                        │                            (Loopback)        │
                                                                        │                                  │           │
                                                                        │                                  └─► AEC reference
                              └────────────────────────────────────────┘
                                       │            │            │
                                       │ HTTP       │ HTTP       │ HTTP
                                       ▼            ▼            ▼
                                ┌──────────┐ ┌────────────────────────────┐
                                │ llama.cpp│ │           Speaches         │
                                │  /server │ │  (faster-whisper + Piper)  │
                                └──────────┘ └────────────────────────────┘
```

The capture path is one worker thread, the playback path is one
worker thread, and the dialogue path runs on its own thread. The
audio callback (PortAudio) does **only** the SPSC ring write/read —
no allocation, no I/O, no blocking. See *Threading model* below.

## Process model

Three things run as separate processes:

| Process | Where | Purpose |
|---|---|---|
| `acva` | host CLI | the orchestrator. Always on the host so the realtime audio path stays direct. |
| `llama-server` | Compose container `acva-llama` | LLM, OpenAI-compatible REST + SSE, on `127.0.0.1:8081`. |
| Speaches | Compose container `acva-speaches` | STT (faster-whisper) + TTS (Piper) behind one OpenAI-compatible surface, on host port `:8090`. WebRTC realtime endpoint at `/v1/realtime` for streaming STT (M5). |

The orchestrator never forks a backend. It opens HTTP connections.
Compose's `restart: unless-stopped` is what restarts crashed backends
in dev. The Supervisor inside acva *observes* via `/health` probes
and *gates* the dialogue path when a backend is unhealthy past the
grace window — it does **not** issue restart commands. (Optional
sd-bus client is gated on `-DACVA_ENABLE_SDBUS=ON` for the systemd
production path.)

This separation is **architectural pillar #1**: in M0/M1 the team
considered embedding model runtimes; the decision was to keep them
as standalone processes for crash isolation, supervised restart,
and the ability to swap engines without recompiling acva.

## Threading model

```
   audio callback thread (PortAudio)  ──► SPSC ring buffer  ──►  capture worker (AudioPipeline)
                                                                        │
                                                                        ▼
                                                                 Resample → APM → VAD → Endpointer
                                                                        │
                                                                        ▼
                                                                  EventBus (lock-free)
                                                                        │
                                                                        ▼
                                                  ┌─────────────────────┴──────────────────────┐
                                                  │                                            │
                                          dialogue worker                                STT worker
                                          (Manager + FSM)                                (RealtimeSttClient)
                                                  │                                            │
                                                  ├──► memory thread (SQLite, single-threaded)
                                                  │
                                                  ├──► LLM I/O thread (libcurl SSE)
                                                  │
                                                  └──► TTS bridge → playback worker (PortAudio out)
```

Key rules:

- **Audio callback never blocks.** No allocation, no logging, no I/O.
  The only thing it does is push samples into an SPSC ring and pop
  from another. The capture worker drains the ring on its own
  schedule. This is **pillar #2** — non-negotiable.
- **Memory writes go through the memory thread.** `MemoryThread::post`
  (fire-and-forget) and `submit` (future-returning) are the only
  write paths. Direct SQLite handles from random threads are
  forbidden.
- **HTTP client choice matters.** cpp-httplib for non-streaming
  request/response (Speaches `/v1/audio/transcriptions`, `/health`).
  libcurl for SSE streaming (LLM). libdatachannel for WebRTC
  (Speaches `/v1/realtime`). Don't unify "for simplicity" without
  measuring.
- **EventBus is lock-free fan-out.** Subscribers get copies; no
  shared mutable state across thread boundaries.
- **Backpressure everywhere.** Every async boundary is a bounded
  queue with explicit overflow policy. **Pillar #4.**

## Cancellation & turn semantics

Every long-running operation carries a `TurnContext` with:

- a turn id (monotonic)
- a `CancellationToken` (atomic flag)

When barge-in fires (M7 `BargeInDetector` promotes a `SpeechStarted`
to `UserInterrupted` while in the `Speaking` FSM state), the manager:

1. Flips the cancellation token on the active turn
2. Drains the LLM SSE stream (libcurl notices the token and aborts)
3. Clears the playback queue (chunks carry seq numbers, stale ones
   are rejected at enqueue *and* at the audio thread's dequeue)
4. Drains the TTS bridge's pending queue
5. Marks the SQLite turn as `interrupted`

Speculative LLM runs (M9, planned) use the same machinery: the
"speculation gate" lets a tentative LLM start on a streaming partial,
and either commits when the final transcript matches the speculation
or cancels and restarts when it diverges.

This is **pillar #3** — cancellation is structural, not error
handling. There is no global cancel flag; only per-turn tokens.

## Key invariants (architectural pillars)

From CLAUDE.md, in priority order:

1. **Process isolation for model backends.** They run as Compose
   containers (dev) or systemd units (prod). Never `popen`, never
   `fork()`. Don't propose embedding model runtimes until post-M8.
2. **Realtime audio path is sacred.** No blocking, no allocation,
   no I/O on the audio thread. SPSC ring buffer is the only
   mechanism crossing the audio thread boundary.
3. **Cancellation is structural.** Per-turn tokens; stale work
   rejected at every queue boundary.
4. **Backpressure everywhere.** Every async boundary is a bounded
   queue with explicit overflow policy.
5. **Observability from day one.** Per-turn trace IDs, JSON logs,
   Prometheus metrics, opt-in OTLP. Not retrofitted.
6. **Crash recovery is a state machine, not error handling.**
   SQLite turn lifecycle (`in_progress`, `committed`, `interrupted`,
   `discarded`) + startup recovery sweep + warm-restart checkpoint.
7. **Speaker mode + AEC is primary, not optional.** AEC reference
   alignment is non-negotiable. Default AEC path is PipeWire's
   system-level `module-echo-cancel`; in-process WebRTC APM is
   compiled but disabled (see §L7).
8. **Supervisor observes, Compose acts.** The supervisor probes
   `/health` and gates the dialogue path; it doesn't restart
   anything. Compose's `restart: unless-stopped` does that.

## Where to look in the code

```
src/main.cpp              ~546 lines of linear orchestration. Reads
                          top-to-bottom; the body of each conceptual
                          step is in an orchestrator/ helper.
src/orchestrator/
  stacks/                 RAII bundles: tts_stack, capture_stack,
                          stt_stack, dialogue_stack. Each build_*
                          returns a stop() that runs teardown.
  boot/                   bootstrap (config + ALSA + VRAM monitor),
                          system_aec (PipeWire AEC RAII),
                          model_controller_handoff (M8A sidecar
                          pre-call), startup_runner (M8A force-load
                          gates).
  admin/                  reload_setup, privacy_handlers,
                          restart_driver, session_resume.
  observability/          event_tracer, status_extra,
                          barge_in_metrics, supervisor_setup.
  io/                     stdin_reader (text-driven sessions).
src/audio/
  capture.{hpp,cpp}       PortAudio in + SPSC ring producer
  pipeline.{hpp,cpp}      capture worker — Resample → APM → VAD
                          → Endpointer → UtteranceBuffer
  apm.{hpp,cpp}           WebRTC APM wrapper (in-process AEC,
                          opt-in)
  silero_vad.{hpp,cpp}    ONNX Silero VAD
  endpointer.{hpp,cpp}    speech onset/offset detection
  utterance_buffer.{hpp,cpp}  rolling pre-buffer + utterance slice
  loopback.{hpp,cpp}      AEC reference ring (when in-process APM
                          is on)
  wake_word.{hpp,cpp}     openWakeWord 3-stage ONNX (off-by-default;
                          see §L8)
src/dialogue/
  fsm.{hpp,cpp}           the dialogue state machine. States:
                          Idle, Listening, UserSpeaking, Transcribing,
                          Thinking, Speaking, Interrupted.
  manager.{hpp,cpp}       FSM driver — subscribes to STT events,
                          drives LLM + TTS, owns turn lifecycle.
  sentence_splitter.{hpp,cpp}  multilingual; handles abbreviations
                          (Dr., e.g.), decimals, enumerations,
                          ellipses, code fences.
  barge_in.{hpp,cpp}      promotes SpeechStarted → UserInterrupted
                          when in Speaking past AEC + cooldown.
  session.{hpp,cpp}       SessionManager — fans session-id changes
                          out to subscribers (Manager, TurnWriter,
                          Summarizer).
src/llm/
  client.{hpp,cpp}        libcurl SSE streaming client to llama-server.
  prompt_builder.{hpp,cpp}  assembles system + history + current
                          turn into a chat-completions prompt.
  model_controller_client.{hpp,cpp}  M8A sidecar client.
src/stt/
  openai_stt_client.{hpp,cpp}  request/response client (M4B fallback)
  realtime_stt_client.{hpp,cpp}  WebRTC streaming client (M5)
  realtime_envelope.{hpp,cpp}  oai-events parser
  realtime_event_dispatch.{hpp,cpp}  routes Speaches realtime events
src/tts/
  openai_tts_client.{hpp,cpp}  Speaches TTS (libcurl streaming PCM)
src/playback/
  queue.{hpp,cpp}         per-turn-aware bounded queue, drops stale
                          chunks
  engine.{hpp,cpp}        PortAudio out + prefill threshold
src/memory/
  memory_thread.{hpp,cpp}  the only legitimate path to SQLite
  repository.{hpp,cpp}     CRUD + recovery sweep
  turn_writer.{hpp,cpp}    persists turns through the lifecycle
  summarizer.{hpp,cpp}     long-context rollup
src/supervisor/
  supervisor.{hpp,cpp}    /health probing + gating
  service.{hpp,cpp}       per-service state machine
  watchdog.{hpp,cpp}      M8A — voice_stuck_total{state} on FSM
                          inactivity past threshold
  startup_check.{hpp,cpp}  M8A — force-load gates
src/observability/
  speaches_wedge.{hpp,cpp}  CUDA-OOM wedge classifier
  otlp.{hpp,cpp}           opentelemetry-cpp wrapper (opt-in)
  turn_span.{hpp,cpp}      brackets each turn with one OTLP span
  (VramMonitor lives in src/orchestrator/boot/bootstrap.hpp — it
   needs the LoggingConfig at construction so it sits next to the
   bootstrap helpers rather than under observability/.)
src/config/
  config.{hpp,cpp,_load.cpp}  glaze-based YAML loader + validators
                          (split for build-time reasons)
  reload.{hpp,cpp}         M8A diff catalog + apply path
src/cli/
  args.cpp                 command-line parsing
  memory_cli.{hpp,cpp}     `acva memory <subcommand>` — process-
                          isolated DB CRUD
src/demos/                 23 `acva demo <name>` smoke / debug tools
```

When clangd flags errors that look like missing-include cascades,
the most common cause is a stale `compile_commands.json` —
`./build.sh` (which regenerates it) is the verdict, not the editor.

## A turn, end to end

The minimal happy path for one user turn (no barge-in):

1. **Capture.** PortAudio callback writes 10 ms frames to the SPSC
   ring at 48 kHz.
2. **AudioPipeline worker** drains the ring. Resamples 48 → 16 kHz
   via soxr; runs APM (a no-op when system AEC is on); pushes
   through Silero VAD; fires `SpeechStarted` event when onset
   threshold is crossed.
3. **Endpointer** continues feeding samples to the M5 streaming STT
   sink while in-utterance; on offset (silence past hangover_ms),
   emits `SpeechEnded`.
4. **RealtimeSttClient** receives the `commit` and starts publishing
   `PartialTranscript` events as Speaches transcribes. On the
   final, emits `FinalTranscript`.
5. **Manager** consumes `FinalTranscript`, transitions FSM
   `Transcribing → Thinking`, asks the memory thread for context,
   builds a prompt via PromptBuilder, opens an SSE stream to
   `llama-server` via LlmClient.
6. **SentenceSplitter** consumes streaming tokens, emits
   `LlmSentence` events at sentence boundaries.
7. **TtsBridge** consumes sentences, calls Speaches `/v1/audio/speech`
   per sentence, gets streaming PCM back.
8. **Resample → PlaybackQueue → PlaybackEngine.** The queue
   pre-buffers `cfg.playback.prefill_ms` (default 100 ms) before
   handing chunks to the audio output callback. The audio out
   thread reads 10 ms chunks at a time.
9. **TurnWriter** persists the turn to SQLite as `committed`
   when `PlaybackFinished` arrives for the last sentence.

End-to-end P50 (user-stop → first-audio): **~1.7 s.** P95: **~3.5 s.**
The original "1–2 s" figure was P50 for short prompts.

## Persistence model

SQLite WAL, one DB at `~/.local/share/acva/acva.db`. Tables (M1 +
M8A):

- `sessions` — session ids + bounds
- `turns` — per-turn rows with lifecycle, user text, assistant text
- `summaries` — long-context rollups, range-keyed within a session
- `facts` — extracted facts for the memory layer
- `settings` — orchestrator-side small KV (e.g., active personality)
- `runtime_state` — M8A warm-restart checkpoint (snapshot + config hash + age)

Turn lifecycle states: `in_progress`, `committed`, `interrupted`,
`discarded`. Startup runs a recovery sweep — any `in_progress` turn
from a prior process is marked `discarded`. **Pillar #6.**

The CLI `acva memory <subcommand>` (sessions, session, turns, turn,
facts, summaries, delete-*, wipe, vacuum, restart) is process-
isolated from the orchestrator — opens its own `Database` directly.
Read paths coexist with a live acva via SQLite WAL; write paths
block on `busy_timeout=5000`.

## Observability

- **Logs:** spdlog with a JSON-per-line sink. Default to
  `${XDG_DATA_HOME}/acva/log/acva-<UTC-timestamp>.log` rotated per
  process (`logging.sink: dir`). Override with `logging.sink:
  stderr|journal|file`.
- **Metrics:** Prometheus on `127.0.0.1:9876/metrics`. Per-subsystem
  prefixes: `voice_pipeline_state`, `voice_llm_*`, `voice_tts_*`,
  `voice_playback_*`, `voice_health_*`, `voice_aec_*`,
  `voice_speaches_{vram_used_mib,wedged}`, `voice_barge_in_*`,
  `voice_wake_word_*`, `voice_stuck_total{state}`.
- **`/status`:** JSON with FSM state, supervisor service states,
  queue depths, APM block, Speaches block (when monitor has data,
  with remediation hint when wedged), `pipeline_state`, etc.
- **OTLP traces (opt-in):** one `voice.turn` span per user turn,
  bracketed at LlmStarted → PlaybackFinished | UserInterrupted |
  LlmFinished{cancelled}. Child spans (vad / stt / prompt / llm /
  tts / playback) are deferred — wiring is mechanical but not
  load-bearing.
- **Grafana dashboard:** `packaging/grafana/acva.json`, 7 panels.
  Bring up via `cd packaging/observability && docker compose -p
  acva-obs up -d`.
- **Topology diagram:** `docs/observability.{dot,svg,pdf}` — 5
  colored clusters showing the runtime + observability flows.

## Where to read more

- `plans/project_design.md` — the source of truth, sectioned for
  reference. Code comments cite it as `§N.M`.
- `plans/open_questions.md` §L — implementation-driven revisions.
  Read this before assuming any A–K answer is still in force.
- `plans/milestones/m{0..8}_*.md` — per-milestone implementation
  history.
- `docs/aec_report.md` — the M6 + M6B AEC analysis (why system AEC
  beats in-process APM on this hardware class).
- `docs/troubleshooting.md` — symptom-first debug guide.
- `CLAUDE.md` — guidance for Claude Code working in this repo;
  also a useful tour for humans.
