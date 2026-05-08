# acva

**Autonomous Conversational Voice Agent.** A local, production-grade C++ voice assistant designed for multi-hour conversations on a single workstation. All inference runs locally; no audio or transcripts leave the machine.

## Status

**Code-complete through M8C.** All eleven implementation milestones (M0 → M1 → M2 → M3 → M4 → M4B → M5 → M6 → M6B → M7 → M7B → M8A → M8B → M8C) have shipped. The full speech-to-speech loop runs end-to-end against the Compose stack: PortAudio → resample → AEC → VAD → streaming STT → dialogue FSM → LLM (SSE) → sentence splitter → TTS → playback queue → speakers. Barge-in cancels mid-speech; SQLite-backed turn lifecycle survives crashes; Prometheus + Grafana observability stack ships alongside; warm restart preserves session continuity. Test suite: 376 unit cases (no external deps) + 13 integration cases (real Silero + live Speaches), all green.

Stretch packaging (AUR `PKGBUILD`, `.deb`, image-digest pinning, observability project-label cleanup, man page, fresh-VM bare-metal acceptance) is parked at [`plans/postpone/m8c_packaging_stretch.md`](plans/postpone/m8c_packaging_stretch.md) — none MVP-blocking. Full milestone history in `docs/history/MVP/milestones/`; architectural revisions and resolved questions in `docs/history/MVP/open_questions.md` §L.

The product target is **always-listening conversational mode**, not press-button-and-ask. Wake-word (M8C) ships off-by-default; the conversational gate ("is the user addressing me?") moves to M10 address detection — see `docs/history/MVP/open_questions.md` §L8.

## What it does

A configurable voice assistant that:

- runs entirely on local hardware (RTX 4060 8 GB target)
- speaker mode primary, with system-AEC for reliable barge-in
- supports multilingual conversation with per-utterance language detection
- streams partial STT and starts the LLM speculatively for sub-2-second latency *(streaming session shipped in M5; speculation lifted to M9)*
- holds 30-minute to multi-hour conversations without crashes, leaks, or latency drift
- recovers cleanly from mid-turn crashes and survives warm restarts
- is observable from day one — structured JSON logs, per-turn traces, Prometheus metrics, opt-in OTLP, Grafana dashboard

## Design at a Glance

```
Mic → Resample → APM (AEC/NS/AGC) → VAD → Endpointer → Utterance Buffer → STT (streaming + final)
                                                            │
                                                            ▼
                                              Dialogue FSM ↔ Memory (SQLite WAL)
                                                            │
                                                            ▼
                                                           LLM (SSE)
                                                            │
                                                            ▼
                                              Sentence Splitter → TTS → Playback Queue → Resample → Speaker
                                                                                          ▲
                                                                                          └── Loopback (AEC reference, when in-process APM is on)
```

- **C++ orchestrator** runs as a host CLI binary and owns audio I/O, dialogue state, memory, cancellation, and observability.
- **Model backends run as separate processes**, never forked from the orchestrator: `llama.cpp/llama-server` for LLM, [Speaches](https://github.com/speaches-ai/speaches) for STT + TTS (one OpenAI-API-compatible surface that consolidates faster-whisper + Piper). Default deployment is **Docker Compose**; systemd units are an alternative production path.
- **Realtime audio path is isolated**: lock-free SPSC ring between the audio callback and the processing thread; no blocking, no allocation, no I/O on the audio thread.
- **Cancellation is structural**: every long-running operation carries a turn ID; barge-in invalidates the turn ID and stale work is rejected at every queue boundary.
- **AEC is system-level by default**: PipeWire `module-echo-cancel` upstream of acva delivers 25–46 dB of speech-band cancellation on the dev workstation. The in-process WebRTC APM stays compiled but disabled; see `docs/reports/aec_report.md` for the full M6 + M6B analysis.

See `docs/project_design.md` for the complete architecture and `docs/diagrams/observability.svg` for the runtime topology.

## Tech Stack

| Concern            | Choice                                                          |
|--------------------|-----------------------------------------------------------------|
| Language           | C++23 (no modules, no Cobalt — see `docs/history/MVP/open_questions.md` §L2) |
| Async / threads    | `std::thread` + dedicated workers; SPSC ring across audio boundary |
| Audio I/O          | PortAudio + soxr (resample)                                      |
| AEC / NS / AGC     | PipeWire `module-echo-cancel` (default) + WebRTC APM (in-process, opt-in) |
| VAD                | Silero VAD v5 via ONNX Runtime                                   |
| Wake-word (off-by-default) | openWakeWord 3-stage ONNX (mel + embedding + classifier) |
| LLM                | llama.cpp / `llama-server` (CUDA build) + Qwen 7-8B family       |
| STT                | Speaches (faster-whisper-large-v3-turbo, OpenAI-API + WebRTC realtime) |
| TTS                | Speaches → Piper voices                                          |
| Storage            | SQLite (WAL mode) on a dedicated memory thread                   |
| HTTP — non-streaming | cpp-httplib (vendored single-header)                           |
| HTTP — SSE / WebRTC | libcurl (LLM SSE), libdatachannel (Speaches realtime)          |
| Config / JSON      | glaze (JSON + YAML)                                              |
| Logs / metrics     | spdlog (JSON sink) + prometheus-cpp                              |
| Dashboard          | Grafana 11.3 + Prometheus 2.55 (auto-provisioned)                |
| Tracing (opt-in)   | opentelemetry-cpp (OTLP/HTTP)                                    |
| Tests              | doctest                                                          |
| Build              | CMake + presets, Ninja                                           |

## Repository Layout

```
src/                      C++ source. Per-subsystem subdirs: audio/, cli/, config/,
                          demos/, dialogue/, event/, http/, llm/, log/, memory/,
                          metrics/, observability/, orchestrator/, pipeline/,
                          playback/, stt/, supervisor/, tts/.
src/main.cpp              ~546 lines of linear orchestration: parse args → load
                          config → demo dispatch → build per-subsystem stacks via
                          orchestrator/ helpers → main loop with reload + warm-
                          restart drain → orderly shutdown.
src/orchestrator/         host-side glue, organised into 5 subdirs:
                            stacks/        tts/capture/stt/dialogue stacks (RAII)
                            boot/          bootstrap, system_aec, model-controller
                                           handoff, startup_runner
                            admin/         hot-reload, privacy commands, restart,
                                           session resume
                            observability/ event_tracer, status_extra,
                                           barge_in_metrics, supervisor_setup
                            io/            stdin_reader (text-driven sessions)
src/demos/                23 `acva demo <name>` smoke / debug commands.
tests/                    doctest suites: acva_unit_tests (376 cases, no deps)
                          + acva_integration_tests (13 cases, real Silero + Speaches).
config/default.yaml       runtime config + `models:` registry (LLM/STT/TTS/VAD/
                          wake-word aliases) + per-personality overrides.
cmake/                    Dependencies.cmake, Warnings.cmake.
third_party/cpp-httplib/  vendored single-header HTTP server.
scripts/                  one-shot dev shell scripts (dev-up, dev-down, soak,
                          aec_analyze, barge-in-probe, validate-bargein,
                          wake-word-offline, ...).
tools/acva-models         Python CLI driving the model registry: list / install /
                          sync / select / verify / status.
tools/train-wake-word     Python driver for custom openwakeword classifiers.
packaging/
  compose/                docker-compose.yml — `llama` + `speaches`.
  systemd/                user-systemd units — `acva-llama`, `acva-speaches`,
                          `acva.service`, `acva.target`.
  observability/          docker-compose.yml for Prometheus + Grafana.
  grafana/                auto-provisioned dashboard (acva.json).
  model-controller/       Go sidecar — owns the docker socket, swaps llama models
                          via `POST /llm/load`. Operator opt-in.
plans/
  project_design.md       source of truth for architecture, milestones, risks.
  open_questions.md       resolved / open decisions; §L holds implementation-
                          driven revisions that supersede earlier sections.
  milestones/m{0..8}_*.md per-milestone implementation plans.
docs/
  troubleshooting.md      symptom-first guide; routes failures to the right demo.
  aec_report.md           M6 + M6B AEC analysis.
  observability.{dot,svg,pdf}  runtime topology diagram.
build.sh                  ./build.sh [dev|debug|release].
run_tests.sh              unit suite (no external deps).
run_integration_tests.sh  integration suite (Silero + live Speaches).
CMakeLists.txt, CMakePresets.json
README.md, CLAUDE.md, LICENSE, .editorconfig, .gitignore
```

## Hardware Target

- GPU: RTX 4060 8 GB (CUDA 12+). Tested on 595 driver / CUDA 13.2.
- CPU: modern x86_64 (≥ 8 cores).
- OS: Linux. Manjaro/Arch is primary, Ubuntu 24.04 LTS secondary.
- Audio: USB or built-in mic + speakers. PipeWire ≥ 1.0 recommended for the system-AEC path.

VRAM budget on the 4060: llama-7B Q4_K_M (~5 GB) + faster-whisper-large-v3-turbo (~1.6 GB) + Piper voices (~250 MB) ≈ 6.8 GB resident, leaving ~1.2 GB headroom. The Speaches `WHISPER__TTL=-1` pin is non-negotiable on this card — the default 5-min auto-evict combined with faster-whisper's [#992](https://github.com/SYSTRAN/faster-whisper/issues/992) leaks ~300 MB per unload cycle and OOMs after a few reloads. See `docs/history/MVP/open_questions.md` §L7 for the rationale.

## Quickstart — Docker Compose (default dev path)

The orchestrator runs on the host as a CLI binary; backends run in containers.

### 1. System dependencies

**Manjaro / Arch:**

```sh
# Build toolchain + system libs
sudo pacman -S --needed \
    base-devel cmake ninja pkgconf git \
    sqlite curl portaudio soxr \
    onnxruntime spdlog fmt \
    webrtc-audio-processing \
    libdatachannel \
    nvidia-container-toolkit
# AUR (yay / paru)
yay -S --needed prometheus-cpp opentelemetry-cpp glaze cpp-httplib doctest
```

**Debian / Ubuntu 24.04+:**

```sh
sudo apt install --no-install-recommends \
    build-essential gcc-13 g++-13 cmake ninja-build pkg-config git \
    libsqlite3-dev libcurl4-openssl-dev portaudio19-dev libsoxr-dev \
    libonnxruntime-dev libspdlog-dev libfmt-dev \
    libwebrtc-audio-processing-dev \
    libdatachannel-dev
# Build from source under ~/src or vendor as submodules:
#   prometheus-cpp, opentelemetry-cpp, glaze (header-only), cpp-httplib (header-only)
```

NVIDIA Container Toolkit needs a CDI spec for GPU passthrough; regenerate after every driver upgrade:

```sh
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
```

If `cuInit` returns "unknown error" inside the container even though `nvidia-smi` works, the `/dev/nvidia-uvm` major drifted out of sync with the CDI spec — bounce dockerd after regenerating. Same playbook covers the post-upgrade case where the kernel module reloaded with a new dynamic major.

### 2. Build acva

```sh
./build.sh                # = dev preset; output under _build/dev/
./build.sh release        # -DNDEBUG, tests off
./run_tests.sh            # unit suite, ~5 s
./run_integration_tests.sh   # integration suite, needs Speaches up
```

The `build.sh` wrapper caps parallelism so it doesn't saturate the box. Raw `cmake --preset dev && cmake --build --preset dev` works too.

### 3. Bring up backends

```sh
./scripts/dev-up.sh
```

This wraps `cd packaging/compose && docker compose -p acva up -d`, polls until `acva-llama` and `acva-speaches` are both healthy, and runs `tools/acva-models status` to surface any missing model assets. On first run it materializes `.env` from `.env.example` (replacing the `__SET_HOME__` sentinel).

### 4. Install model assets

```sh
tools/acva-models sync     # installs every alias the active config references
```

Default footprint ≈ 6.5 GB:

| File | Size | Source |
|---|---|---|
| `llama.cpp/<active LLM>.gguf` | ~4.7 GB | HuggingFace via direct download |
| `speaches/...` faster-whisper-large-v3-turbo | ~1.6 GB | HuggingFace via Speaches `POST /v1/models/<id>` |
| `speaches/...` Piper voices (en + ru × 4) | ~250 MB | HuggingFace via Speaches |
| `silero/silero_vad.onnx` | ~2 MB | snakers4/silero-vad |

The downloader is idempotent / resumable. To swap an LLM:

```sh
tools/acva-models select llm dialog              # alias from config/default.yaml
```

### 5. Run the orchestrator

```sh
./_build/dev/acva
# or
./_build/release/acva
```

acva resolves config + DB paths via XDG (`~/.config/acva/default.yaml`, `~/.local/share/acva/acva.db`) — see CLAUDE.md for full path resolution rules. Control plane:

```sh
curl -sS http://127.0.0.1:9876/status | jq
curl -sS http://127.0.0.1:9876/metrics | head
```

Demos covering individual paths:

```sh
./_build/dev/acva demo                # list every demo
./_build/dev/acva demo health         # probe each backend's /health
./_build/dev/acva demo chat           # text-in → speech-out smoke
./_build/dev/acva demo capture        # mic + VAD endpointing report
./_build/dev/acva demo transcribe     # mic + streaming STT
./_build/dev/acva demo bargein        # auto-injected interrupt cascade
```

### 6. Stop / clean

```sh
./scripts/dev-down.sh           # stop, keep volumes
./scripts/dev-down.sh --wipe    # also clear anonymous volumes
```

The `ACVA_MODELS_DIR` host bind-mount is never wiped automatically; remove it manually if you really want a fresh model store.

### 7. Observability stack (optional)

```sh
cd packaging/observability && docker compose -p acva-obs up -d
# Grafana on http://127.0.0.1:3000  (anonymous viewer auth; admin/admin first login)
# Prometheus on http://127.0.0.1:9090
```

The dashboard auto-loads from `packaging/grafana/acva.json` — 7 panels covering FSM state, Speaches VRAM + wedged classifier, TTS first-audio P50/P95, service health, playback queue + underruns, watchdog, and pipeline state.

## Production alternative — systemd

For unattended deployments without Docker, four user-systemd units ship in `packaging/systemd/`:

| File | Role |
|---|---|
| `acva-llama.service`    | LLM backend (llama.cpp `llama-server`) on `127.0.0.1:8081` |
| `acva-speaches.service` | STT + TTS backend (Speaches) on `127.0.0.1:8090` (Compose) / `:8000` (bare-metal) |
| `acva.service`          | Orchestrator (control plane on `127.0.0.1:9876`) |
| `acva.target`           | Convenience target — brings up all three in dependency order |

Install:

```sh
./scripts/install-systemd.sh             # copies units to ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user start acva.target
```

`packaging/systemd/README.md` has the full runbook including bare-metal binary layout, system-wide install variant (root-owned), and the `loginctl enable-linger` step for headless workstations. The optional sd-bus extension to the supervisor is gated by `-DACVA_ENABLE_SDBUS=ON` at build time and `cfg.supervisor.bus_kind: user` at runtime.

`nvidia-cdi-refresh.service` is provided as a one-shot helper that regenerates `/etc/cdi/nvidia.yaml` on boot — useful on rolling-release distros where the spec drifts after kernel-module dynamic-major changes.

## Tests

Two suites, two binaries, two scripts:

```sh
./run_tests.sh                            # unit — no external deps, ~5 s
./run_tests.sh dev --test-case='paths*'   # filter pass-through to doctest
./run_integration_tests.sh                # integration — Silero + live Speaches
```

The integration suite resolves model paths via the same XDG defaults `acva` itself uses, so on the dev workstation no env vars are needed. Missing assets cause a clean per-case skip, never a fail.

For longer runs:

```sh
./_build/dev/acva demo soak-mini          # 60 s FakeDriver smoke
./scripts/soak.sh                         # 4-hour acceptance run with CSV trace + report
```

## Troubleshooting

`docs/guide/troubleshooting.md` is the symptom-first guide — routes failures to the right `acva demo <name>` and reads its output. Common entry points:

| Symptom | Start with |
|---|---|
| Can't connect to a backend | `acva demo health` |
| Mic captures silence | `scripts/wake-word-offline.sh` (records raw + AEC, runs both through STT + wake-word) |
| TTS audio cuts in/out | playback queue depth + underruns metrics; `acva demo tts` |
| Barge-in doesn't fire | `scripts/barge-in-probe.py`, `acva demo bargein-validation` |
| AEC isn't cancelling | `acva demo aec-record` + `scripts/aec_analyze.py` |
| Speaches stuck at full VRAM | `acva demo wedge` reproduces the CUDA-OOM wedge case |
| `cuInit` "unknown error" inside container | regenerate CDI spec + bounce dockerd; see Quickstart §1 |

## Plans & docs

- **`docs/project_design.md`** — architecture source of truth (sections referenced throughout the codebase comments).
- **`docs/history/MVP/open_questions.md`** — resolved + open decisions; §L holds implementation-driven revisions.
- **`docs/history/MVP/milestones/m{0..8}_*.md`** — per-milestone implementation history.
- **`docs/reports/aec_report.md`** — M6 + M6B AEC analysis.
- **`docs/guide/troubleshooting.md`** — symptom-first guide.
- **`docs/diagrams/observability.{svg,pdf}`** — runtime topology diagram.
- **`CLAUDE.md`** — guidance for Claude Code working in this repo (also useful as a deeper reading-list for humans).

## License

See `LICENSE`.
