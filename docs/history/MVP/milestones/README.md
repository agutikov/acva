# Milestone plans

> **Archive (MVP).** This directory captures the per-milestone
> plans as they stood at MVP close (M8C, 2026-05-08). The live
> source-of-truth for architecture is `docs/project_design.md`;
> post-MVP planning lives in `plans/postpone/`. Cross-links below
> point at those live locations.

Detailed per-milestone plans. The high-level table lives in
[`docs/project_design.md`](../../../project_design.md) §17; this
directory has one file per milestone with concrete steps, file
lists, APIs, tests, and acceptance criteria. Plans for milestones
not yet started — and for stretch work parked post-MVP — live in
[`plans/postpone/`](../../../../plans/postpone/).

## Status

All M0–M8C milestones closed. **MVP is unblocked.** M9 + M10 are
planned post-MVP; M8C packaging stretch is parked.

| #   | File | Status |
|-----|------|--------|
| M0  | `m0_skeleton.md`               | ✅ landed |
| M1  | `m1_llm_memory.md`             | ✅ landed |
| M2  | `m2_supervision.md`            | ✅ landed |
| M3  | `m3_tts_playback.md`           | ✅ landed |
| M4  | `m4_audio_vad.md`              | ✅ landed |
| M4B | `m4b_speaches_consolidation.md`| ✅ landed (Speaches replaces whisper.cpp/server + piper.http_server) |
| M5  | `m5_streaming_stt.md`          | ✅ landed 2026-05-03 (streaming session; speculation lifted to M9) |
| M6  | `m6_aec.md`                    | ✅ code-complete 2026-05-03; superseded by M6B for the dev hardware class |
| M6B | `m6b_aec_hardware.md`          | ✅ closed 2026-05-04 — Path B (PipeWire `module-echo-cancel`) is the default; gates 1/3/4 PASS |
| M7  | `m7_barge_in.md`               | ✅ code-complete 2026-05-04 |
| M7B | `m7b_barge_in_validation.md`   | ✅ closed 2026-05-05 (4/4 fixtures PASS via `scripts/validate-bargein.py`) |
| M8A | `m8a_admin_state.md`           | ✅ closed 2026-05-07 (hot-reload, privacy, memory CLI, watchdog+restart, model orchestration) |
| M8B | `m8b_observability.md`         | ✅ closed 2026-05-07 (soak harness, Grafana dashboard, OTLP, build-time pass) |
| M8C | `m8c_distribution.md`          | ✅ closed 2026-05-08 (wake-word off-by-default, Compose+systemd packaging, docs pass) |
| M9  | [`../../../../plans/postpone/m9_speculation.md`](../../../../plans/postpone/m9_speculation.md) | planned (originally M5 Steps 4–5; depends on a partial-transcript source — Speaches doesn't emit them yet) |
| M10 | [`../../../../plans/postpone/m10_conversational_ux.md`](../../../../plans/postpone/m10_conversational_ux.md) | planned (adaptive endpointer + address detection; depends on M9; wake-word's gate seam is the runway) |
| —   | [`../../../../plans/postpone/m8c_packaging_stretch.md`](../../../../plans/postpone/m8c_packaging_stretch.md) | parked (image digest pinning, AUR `PKGBUILD`, `.deb`, observability project-label cleanup, man page, fresh-VM acceptance) |

## Demos — `acva demo <name>`

Every milestone exposes one or more `acva demo <name>` subcommands
that exercise its headline deliverable end-to-end with no user input.
Run `acva demo` for the live catalog; the table below is the M8C
snapshot.

| Demo                  | Milestone   | What it verifies |
|-----------------------|-------------|------------------|
| `fsm`                 | M0          | synthetic FSM driver runs through 3 turns; no backends needed |
| `llm`                 | M1          | `LlmClient` reaches `llama-server`, streams a fixed prompt's reply |
| `health`              | M2          | probes each backend's `/health`; prints `ok / status / latency` |
| `tone`                | M3          | 1.5 s 440 Hz sine through `cfg.audio.output_device` (no TTS) |
| `tts`                 | M3+M4B      | a fixed sentence flows through `TtsBridge` → Speaches → `PlaybackEngine` |
| `chat`                | M1+M3+M4B   | text-in → speech-out: prompt → LLM → SentenceSplitter → TTS → speakers |
| `loopback`            | M4          | mic → speakers passthrough through SPSC + 48↔16 kHz resampler |
| `capture`             | M4          | mic capture + Silero VAD endpointing report |
| `stt`                 | M4B         | TTS-fixture audio → Speaches `/v1/audio/transcriptions` round-trip |
| `transcribe`          | M5          | 30 s mic + Silero VAD + Speaches realtime STT (partials + finals) |
| `bargein`             | M7          | auto-injected interrupt mid-turn; verifies the cancellation cascade |
| `bargein-validation`  | M7B         | fixture-driven barge-in: WAV mic → BargeInDetector → cascade |
| `aec`                 | M6          | synthetic AEC validation: stimulus → loopback → APM → ERLE convergence |
| `aec-hw`              | M6          | real-hardware AEC validation: speaker → mic → APM (asserts ERLE ≥ 25 dB) |
| `aec-record`          | M6B         | record original/raw/cleaned WAVs of speaker→air→mic→APM for offline analysis |
| `soak`                | M6          | 5-min Speaches throughput + VRAM monitor (silent; for leak detection) |
| `soak-mini`           | M8B         | 60 s FakeDriver-only end-to-end mini-soak — RSS + p95 first-audio drift |
| `wedge`               | M6          | reproduce the Speaches CUDA-OOM wedge (long Russian TTS) and diagnose |
| `reload`              | M8A         | `POST /reload` roundtrip — hot accepted, restart-required rejected |
| `wipe`                | M8A         | `POST /wipe?session` + `?all=true` roundtrip; verifies cascade + rollover |
| `wake-word`           | M8C         | live mic + WakeWord — per-tick confidence + max/above-threshold summary |
| `wake-word-offline`   | M8C         | offline replay: WAV → Speaches STT + WakeWord per-second scores (debug aid) |

Each demo exits `0` on success and non-zero with a clear failure line
on stderr. They use the same config-resolution path as `acva` itself —
pass `--config PATH` to override.

When something doesn't work, [`docs/guide/troubleshooting.md`](../../../guide/troubleshooting.md)
is the symptom-first guide. Operational tasks (start/stop, model
swap, privacy commands, backup) are in
[`docs/guide/operations.md`](../../../guide/operations.md).

## What you can try after each milestone

A short tour of the user-touchable surface each milestone delivered.
All commands assume the project root as cwd, a successful
`./build.sh`, and (for M1+) a healthy Compose stack
(`./scripts/dev-up.sh`).

Most surfaces are now exercised by the `acva demo …` commands above
or documented in `docs/guide/operations.md`; this section keeps the
historical "headline feature per milestone" view for orientation.

### M0 ✅ — skeleton runs end-to-end on synthetic events

`acva demo fsm` runs the synthetic driver and exits. The fake driver
(off by default) can be enabled in config for a longer demo session.

### M1 ✅ — talk to a real LLM, memory persists across restarts

`acva --stdin` for a text-in interactive loop;
`acva memory sessions` / `acva memory turns --session <id>` for
introspection. Mid-turn `kill -9` + restart exercises the recovery
sweep (in_progress → interrupted).

### M2 ✅ — supervision + dialogue gating + keep-alive

Stop one backend (`docker compose stop llama`) and watch
`pipeline_state` flip to `failed` after the grace window; restart and
watch it recover. `acva demo health` is the one-shot probe.

### M3 ✅ — TTS + playback queue

`acva demo tone` (audio device only, no TTS) and `acva demo tts`
(full Piper synthesis through Speaches → playback). Run `tone` first
when "I hear nothing" — isolates the audio device from the TTS
config.

### M4 ✅ — mic capture + VAD endpointing

`acva demo loopback` (mic → speakers passthrough; SPSC + resampler);
`acva demo capture` (mic + VAD endpointing report; useful for
threshold tuning).

### M4B ✅ — voice-backend consolidation onto Speaches

One Speaches container replaces the separate whisper.cpp/server + piper
services. `acva demo stt` is the self-contained STT smoke; `acva demo
chat` exercises the full text-in → speech-out loop now that real STT is
wired.

### M5 ✅ — streaming STT (no speculation yet)

`acva demo transcribe` runs 30 s of mic + Silero VAD + Speaches
realtime STT, printing partials and finals. Speculation (originally
M5 Steps 4–5) lifted to M9 because Speaches doesn't emit
`transcription.delta` events; see
[`../open_questions.md`](../open_questions.md) §L6.

### M6 ✅ + M6B ✅ — AEC

System AEC (PipeWire `module-echo-cancel`) is the default path; the
in-process WebRTC APM stays compiled but disabled per
[`../open_questions.md`](../open_questions.md) §L7.
`acva demo aec-record` captures the speaker → mic chain; analyze with
`scripts/aec_analyze.py`.

### M7 ✅ + M7B ✅ — barge-in

`acva demo bargein` injects a synthetic interrupt mid-speaking;
`acva demo bargein-validation --fixture <wav>` runs one of the four
M7B manifest fixtures. Cascade latencies measured 54–68 ms
(well inside the §19.3 P50 ≤ 200 / P95 ≤ 400 gates).

### M8A ✅ — admin / state

Hot-reload (`POST /reload` or `SIGHUP`), privacy commands (`/mute`,
`/unmute`, `/new-session`, `/wipe`), `acva memory <subcommand>`
(process-isolated DB CRUD), watchdog + warm restart (`POST /restart`
checkpoints + `execv`s self), boot-time model orchestration via the
`packaging/model-controller/` Go sidecar. `acva demo reload` and
`acva demo wipe` are the smoke checks.

### M8B ✅ — observability + soak

`acva demo soak-mini` (60 s FakeDriver-only smoke);
`scripts/soak.sh` (4-hour acceptance run with CSV trace + report).
Bring up Grafana via `cd packaging/observability && docker compose
-p acva-obs up -d`; dashboard at `packaging/grafana/acva.json`.
OTLP traces are opt-in via `cfg.observability.otlp.enabled`.

### M8C ✅ — distribution + wake-word

`acva demo wake-word` (live mic) + `acva demo wake-word-offline --wav
<path>` (offline WAV replay; pair with `scripts/wake-word-offline.sh`
for the raw + AEC compare). Wake-word ships off-by-default; see
[`../open_questions.md`](../open_questions.md) §L8 for why and
where the conversational gate moves next. `tools/acva-models` drives
all model assets via the `models:` registry block. systemd path:
`./scripts/install-systemd.sh && systemctl --user start acva.target`.

### M9 (planned) — streaming partials + speculation

Source `PartialTranscript` from a streaming STT backend; speculative
LLM start in the Dialogue Manager with stability-window reconcile.
Plan: [`../../../../plans/postpone/m9_speculation.md`](../../../../plans/postpone/m9_speculation.md).

### M10 (planned) — conversational UX

Adaptive endpointer (depends on M9 partials) + address-detection
classifier above the wake-word gate seam. Plan:
[`../../../../plans/postpone/m10_conversational_ux.md`](../../../../plans/postpone/m10_conversational_ux.md).

## Conventions used in these documents

- **Step**: an ordered chunk of work suitable for a single PR (or, in solo dev, a single coherent push).
- **Files**: paths under `src/` and `tests/` that the milestone creates or substantially modifies.
- **Acceptance**: the observable behavior that must hold before the milestone closes. Anything not listed is not in scope for this milestone.
- **Risks**: things that have a real chance of derailing the milestone, with the mitigation we've already chosen.

When a milestone surfaces a new design question, log it in
[`../open_questions.md`](../open_questions.md) (section L for
implementation-driven revisions) rather than mutating the milestone
plan in place.
