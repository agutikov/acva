# M8B — Observability & Soak

**Estimate:** ~1 week.

**Depends on:** M0–M7. Sibling sub-milestones M8A (admin & state) and M8C (distribution + wake-word). M8B can run in parallel with M8A since neither modifies the other's surface.

**Blocks:** MVP release (with M8A + M8C). The 4-hour soak passing is the headline acceptance gate that gives confidence to ship.

## Goal

The observability + correctness half of M8: verify the working pipeline holds up under sustained load, and document its internal behavior well enough that a maintainer can debug it from the outside. Three surfaces:

1. **Soak test infrastructure** — 4-hour scripted user/assistant exchange with leak/latency/restart criteria.
2. **Metrics dashboard** — Grafana JSON pinned to the Prometheus metrics already exposed since M2.
3. **OTLP wiring (opt-in)** — distributed traces for per-turn span trees.

The split from the original M8 is purely organizational: M8 was growing past 11 steps. This sub-milestone groups soak + observability work so it ships independently of admin features and packaging.

## Out of scope

- Admin / control-plane features (covered by M8A).
- Wake-word, packaging, docs, final sweep (covered by M8C).
- New features. M8 polishes existing ones.

## Step 1 — Soak test infrastructure — ✅ harness landed 2026-05-07 (4-hour run pending dogfood)

Shipped:
- `scripts/soak-driver.py` (~330 LOC, stdlib-only Python). Spawns
  `acva --stdin`, feeds random prompts at 5–30 s intervals, polls
  `/metrics` + `/status` every 5 s into a CSV, monitors
  `voice_speaches_wedged`, runs `docker compose -f
  packaging/compose/docker-compose.yml restart speaches` on the
  rising edge with a 60 s cooldown between restarts. Tracks restart
  count toward the `≤ 2` acceptance gate. Reads RSS from
  `/proc/<acva-pid>/status` directly (Prometheus doesn't expose it).
- `scripts/soak.sh` — bash wrapper. `--duration 4h | 30m | 240s |
  14400`, `--output DIR` (default `tests/soak/reports/<isodate>/`),
  `--config PATH`, `--seed N`. exec's into the Python driver.
- `tests/soak/prompts.txt` — 30-line corpus (factual / conversational
  / creative / code mix). Comments + blank lines stripped at load.
- Soak report (`<output>/soak-report.txt`): start/end timestamps,
  prompts sent, acva exit code, RSS baseline + final + growth (MiB),
  queue depth max + final, P95 first-audio baseline + final + drift,
  speaches restarts, wedge events, playback underruns. Acceptance
  gate: `no_crashes`, `rss_growth_under_limit`,
  `queue_depth_stable`, `service_restarts_ok`. Per-tick CSV
  (`soak-metrics.csv`) holds the raw time series; acva's stderr
  goes to `acva.log` next to it.
- Smart gate semantics: `rss_growth_under_limit` is reported `n/a`
  on runs shorter than `WARMUP_SEC + 10 min` (1 h 10 m) so smoke
  runs don't spuriously fail on legitimate startup load.

Smoke verified end-to-end:
- 30 s run: harness spawns, polls, classifier identifies the
  speaches process at 1190 MiB (matches the documented turbo+int8
  baseline), CSV + report produced, gate logic correct (FAIL
  intentional on too-short smoke).
- 60 s run: 3 prompts sent, queue_depth_max=28, no crashes, no
  restarts → overall PASS with RSS gate skipped.

Known v1 limitations:
- **P95 first-audio drift is a placeholder (0%).** Reading histogram
  buckets from /metrics is a follow-up; the dashboard (Step 2) will
  read them directly via Grafana/PromQL anyway, and the soak gate
  becomes meaningful once that's wired.
- **Underrun count is high under FakeDriver-only / no real
  speakers** because PortAudio is open with no real output. Not a
  real signal in the v1 harness; the production 4-hour soak runs
  with the dev workstation's actual speakers + AEC, where underruns
  reflect real playback starvation.
- **Queue-depth gate is a peak threshold (500), not the
  monotonic-growth slope check the plan calls for.** Tightens when
  time-series analysis lands.
- **No barge-in injection in v1** (the plan's 15% mid-turn barge-in
  rate). Requires audio routing — bundled with the virtual-mic
  follow-up below.

Deferred follow-ups (next slice candidates):
- **Real virtual-mic mode** — set `cfg.audio.test_input_wav` to a
  pre-recorded fixture loop (the M7B fixture-WAV mode already pumps
  WAV through the production capture path). The driver picks
  prompts → looks up the matching audio fixture → updates
  `cfg.audio.test_input_wav` → triggers reload. Or simpler: a
  PulseAudio `module-null-sink` + `module-loopback` route
  pre-recorded audio at the OS level.
- **`tools/acva-models recover` Python subcommand** — manual escape
  hatch mirroring the soak driver's auto-restart logic. Trivial
  follow-up; the soak driver already has the docker-compose-restart
  flow.
- **Histogram-bucket scrape** for P95 first-audio drift, fed into
  the same gate.
- **Time-series queue-depth slope check.**

The dev-box 4-hour acceptance run is the headline gate — deferred
to dogfood once the operator has dedicated workstation time. The
harness produces the canonical `tests/soak/reports/<date>/`
artifact for archival on a passing run.

## Step 1 — Soak test infrastructure (original spec)

**Files:**
- `scripts/soak.sh` — runs the orchestrator + scripted user input for `$DURATION` (default 4 h).
- `scripts/soak-driver.py` — synthesizes user audio at random intervals, types prompts, logs assistant timing.
- `tests/soak/fixtures/` — recorded user audio clips for replay.

The soak driver:
- Picks a random prompt from a fixed corpus (~50 prompts).
- Plays the user audio through a virtual mic (PulseAudio loopback module).
- Waits for assistant playback to complete.
- Logs turn metrics (latency, outcome) to a CSV.
- Occasionally (15% of turns) injects mid-turn barge-in.
- Repeats for `$DURATION`.

After the run: a small post-processor reads the CSV + Prometheus metrics dump and produces a soak report:
- P50/P95 per stage (vad → final, final → first_token, first_token → first_audio).
- Heap RSS over time.
- Queue depths (max, mean).
- Service restart count.
- Turn outcome counts.

**Acceptance criteria** (re-stated from §14):
```
duration:        4 h
crashes:         0
heap growth:     < 50 MB after 1 h warmup
queue depth:     stable, no monotonic growth
latency P95:     within +20% of post-warmup baseline
service restarts: ≤ 2 (incidental), pipeline never enters failed state
```

## Step 2 — Metrics dashboard

Two options:
- **A.** Grafana dashboard JSON, points at a local Prometheus instance.
- **B.** Static HTML + Chart.js page served at `/dashboard`.

Recommend A for proper operability — standard tooling, can plug into other workstation Prometheus setups. Ship the dashboard JSON in `packaging/grafana/acva.json`. Document the Prometheus config snippet to scrape `acva.service` in the README.

Dashboard sections:
- **Latency** — VAD → final, final → first_token, first_token → first_audio, end-to-end.
- **Throughput** — turns/min, sentences/min.
- **Health** — service states, restart counts.
- **Audio** — playback underruns, AEC delay, queue depths.
- **Memory** — turns persisted, summary count, DB size on disk.

## Step 3 — OTLP wiring (opt-in) — ✅ landed 2026-05-07

Shipped:
- **Config:** `cfg.observability.otlp.{enabled, endpoint, service_name}`,
  default disabled (endpoint defaults to
  `http://127.0.0.1:4318/v1/traces`).
- **Build gate:** `find_package(opentelemetry-cpp CONFIG QUIET)` in
  `cmake/Dependencies.cmake`; sets `ACVA_HAVE_OTLP` when present.
  Linked targets: `opentelemetry-cpp::trace` +
  `opentelemetry-cpp::otlp_http_exporter`. The define is exported
  PUBLIC on `acva_core` so test TUs that gate on `#ifdef
  ACVA_HAVE_OTLP` are in lockstep with the production build.
- **`src/observability/otlp.{hpp,cpp}`** — `Tracer` class with
  `init(cfg) / shutdown()` + RAII `Span` handles. The header is
  otel-free (every otel type is hidden behind `std::shared_ptr<void>`
  + a `SpanHolder` adapter struct in the .cpp), so no downstream TU
  pulls in `opentelemetry/...` headers. Stub implementation when
  `ACVA_HAVE_OTLP` is undefined: `init()` logs a warn if the
  operator enabled OTLP without the dependency, all methods are
  no-ops, `Span::active()` returns false. Real implementation
  uses `OtlpHttpExporter` + `BatchSpanProcessor` +
  `TracerProvider`; `shutdown()` does a 2 s force-flush before
  swapping in a `NoopTracerProvider`.
- **`src/observability/turn_span.{hpp,cpp}`** — bus subscriber
  that brackets each turn with one `voice.turn` span. Anchors:
  - **start** on `LlmStarted` (the first event whose `turn` carries
    the FSM-minted id).
  - **close ok** on `PlaybackFinished`.
  - **close error** on `UserInterrupted` (status `interrupted`) or
    `LlmFinished{cancelled=true}` (status `llm_cancelled`).
  Per-turn spans are stashed in a mutex-guarded `unordered_map`
  keyed by `event::TurnId`; the subscriber owns the only reference,
  so closing happens on terminal-event erase.
- **main.cpp wiring** — `Tracer` constructed alongside the
  metrics::Registry; `tracer.init(cfg.observability)` runs before
  the dialogue path so the first `LlmStarted` lands in the
  subscriber. `tracer.shutdown()` runs after the supervisor +
  control plane teardown but before `bus.shutdown()`, so any
  in-flight Span gets a clean End() before the bus drains.
- **Tests:** 4 cases in `tests/test_otlp.cpp` covering disabled
  init, idempotent shutdown, enabled init against an unreachable
  endpoint (verifies the exporter doesn't synchronously connect on
  init — flush at shutdown is best-effort), and the subscriber's
  three close paths (success / interrupted / cancelled). Full
  suite: **365 cases** (was 361), all green.

Known v1 limitations:
- **Only the root `voice.turn` span is emitted.** The plan's child
  spans (`vad`, `stt.partial`, `stt.final`, `prompt.assemble`,
  `llm.first_token`, `llm.stream`, `tts`, `playback`) are deferred —
  each requires picking up a SpanContext per turn and threading it
  through the bus subscribers. Wiring them is mechanical (the same
  bus events already exist) but not load-bearing for the v1 trace
  view, which surfaces "turn started → turn done" as the headline
  span. Operators see one trace per turn, with the right duration
  and outcome attribute.
- **No SpanLink between sibling turns** — each is a fresh trace.
  Cross-turn correlation lives in the session_id (already on every
  log line), not the trace tree.
- **Batch processor uses default options** (max queue 2048, max
  export batch 512, schedule delay 5 s). Tunable later via
  `BatchSpanProcessorOptions` if real soak loads expose backpressure.
- **End-to-end smoke against a real otelcol-contrib** is deferred
  to dogfood — the unit tests verify the exporter's lifecycle
  against an unreachable endpoint (where the connection refusal is
  exactly the expected behaviour).

## Step 3 — OTLP wiring (original spec)

Per H2: OTLP traces via `opentelemetry-cpp`. Disabled by default; enabled via `cfg.observability.otlp.endpoint`.

**Files:**
- `src/observability/otlp.hpp`, `src/observability/otlp.cpp`.

Each user turn is a span tree:
- Root span: `voice.turn` with `turn_id` attribute.
- Child spans: `vad`, `stt.partial`, `stt.final`, `prompt.assemble`, `llm.first_token`, `llm.stream`, `tts`, `playback`.

Span starts / ends are wired via callbacks the components already have (the observability hooks were left empty in M0–M7 for exactly this purpose).

OTLP export uses HTTP (not gRPC) to keep the dependency surface lean. Endpoint defaults to `http://127.0.0.1:4318/v1/traces` if a local otelcol-contrib is configured.

## Demo commands (planned)

### `acva demo soak-mini` — ✅ landed 2026-05-07

Shipped a 60-second FakeDriver-only end-to-end mini-soak at
`src/demos/soak_mini.cpp` (~225 LOC). Reports:

- Per-tick (`t={10,30,60}s` by default; quarter-spaced if `--duration`
  overrides the 60s default): RSS from `/proc/self/status`, turn count
  from FSM outcome observer, p95 of LlmStarted→first-TtsAudioChunk
  latencies. underruns/queue_max are placeholders (`-`) because the
  FakeDriver-only path doesn't run real playback.
- Final summary: `rss_growth` (final − warmup-baseline at t=10s),
  `latency_p95_drift` (% change vs warmup), supervisor_restarts (0 with
  no real backends).
- PASS gate: `rss_growth ≤ 50 MiB AND p95_drift ≤ +20% AND turns > 0`.
- `--duration <s>` and `--warmup <s>` overrides; default 60/10.

Smoke verified: default 60s run produces 7 turns, RSS stable at 18 MiB,
p95 steady at 602ms (FakeDriver llm_first_token_delay + tts_first_audio
within timing tolerance), PASS. Full 352-case unit suite still green
post-add. The demo is a self-contained pre-flight; the full 4-hour
harness with virtual mic + real backends lands in Step 1 proper below.

### `acva demo soak` — 60-second mini-soak (original spec)

Runs the FakeDriver + (optional) real LLM/TTS for 60 seconds and
reports the same metrics the 4-hour harness does, but at a tractable
size. Useful as a pre-flight before kicking off the real soak.

Expected output:

```
demo[soak] duration=60s driver=fake llm=on tts=on
  t=10s  rss=215MiB  turns=4  underruns=0  queue_max=3   p95_first_audio=412ms
  t=30s  rss=218MiB  turns=12 underruns=0  queue_max=3   p95_first_audio=438ms
  t=60s  rss=220MiB  turns=24 underruns=1  queue_max=4   p95_first_audio=441ms
demo[soak] done: rss_growth=5MiB latency_p95_drift=+29ms supervisor_restarts=0
demo[soak] PASS (acceptance: rss_growth<50MiB, p95_drift<+20%, no supervisor restarts)
```

Failure modes:
- `rss_growth > 50 MiB / minute` → leak. The full 4-hour harness will catch it; this is a quick screen.
- `latency_p95_drift > 20%` → backpressure or thermal throttling.
- `underruns > 5 / minute` → playback queue starving; producer (LLM or Piper) is slow.

## Acceptance

1. **4-hour soak passes.** All criteria met. Report committed to `tests/soak/reports/` with date and git revision.
2. **Dashboard renders.** With Prometheus scraping acva and Grafana loading `packaging/grafana/acva.json`, every panel shows non-empty data after a 60 s mini-soak.
3. **OTLP traces visible** in a local otelcol when enabled; no impact when disabled.

## Risks specific to M8B

| Risk | Mitigation |
|---|---|
| Soak finds a leak that wasn't in earlier dev | Buffer 0.5 weeks for fixes; profile with heaptrack |
| OTLP export contention with critical path | Async + non-blocking; if exporter blocks, drop spans |
| Dashboard panels regress as metrics get renamed | Pin panel queries to a stable subset; add a CI step that diffs `/metrics` output against a golden list before merge |

## Known issues — Speaches wedge detection ✅ landed 2026-05-07 (item 1 + item 3 partial)

Shipped (items 1 + 3a from the plan below):
- `cfg.supervisor.speaches_wedge_threshold_mib` (default 2000 — covers
  turbo+int8 baseline + the ~800 MiB CUDA-context budget).
- `voice_speaches_vram_used_mib` (gauge, MiB) and
  `voice_speaches_wedged` (0/1 gauge) published on `/metrics` by the
  VramMonitor each tick.
- Pure classifier in `src/observability/speaches_wedge.{hpp,cpp}` —
  parses `nvidia-smi --query-compute-apps=pid,used_memory` CSV output,
  resolves PIDs to cmdlines via an injectable `CmdlineLookup`, returns
  the first match whose cmdline contains "speaches" along with
  `wedged = (used_mib >= threshold_mib)`. Live monitor wires
  `popen("nvidia-smi …")` + `read_proc_cmdline(pid)` reading
  `/proc/<pid>/cmdline` (NUL → space).
- VramMonitor now takes (LoggingConfig, SupervisorConfig, Registry);
  pushes the metrics each tick AND maintains a `SpeachesWedgeState`
  snapshot (PID, used_mib, wedged, threshold_mib) the `/status`
  closure reads. Edge-triggered `speaches_wedged` /
  `speaches_recovered` log events on transitions. Convenience ctor
  `VramMonitor(LoggingConfig)` retained for tests/demos that don't
  need wedge detection.
- `/status` adds a `"speaches"` block when the monitor has data:
  `vram_used_mib`, `wedged`, `threshold_mib`, `pid`. When wedged, a
  `remediation` string surfaces the manual escape hatch
  (`docker compose -f packaging/compose/docker-compose.yml restart speaches`).
- Tests: 9 cases / 26 assertions in
  `tests/test_speaches_wedge_classifier.cpp` covering CSV parse
  (whitespace + blanks + malformed lines), classify match / no-match /
  threshold-inclusive / first-match ordering / non-speaches PID
  ignored. Full unit suite at **361 cases** (was 352), all green.

Deferred items from the original "Three things M8B owes" list:
- **Item 2 (Recovery action — soak driver issues `docker compose restart speaches`).**
  Lives inside the 4-hour soak driver, which is M8B Step 1's bigger
  follow-up. The `voice_speaches_wedged` metric is now the trigger;
  the driver acts on it.
- **Item 3b (`tools/acva-models recover` Python subcommand).** Manual
  escape hatch for dev sessions that mirrors the soak driver's auto-
  restart. Bundled with the soak driver work above so the manual +
  automated paths share logic.

The above two are pure additions on top of the detection metric — no
further C++ surface changes needed.

## Known issues — full original notes

- **Speaches CUDA-OOM wedge leaves VRAM unrecoverable.** Surfaced 2026-05-04.
  After running `tests/test_speaches_wedge.cpp` (or any sustained
  long-Russian-TTS load), faster-whisper's encoder workspace can fail to
  allocate; Speaches enters a state where `/v1/audio/transcriptions`
  returns HTTP 500 instantly while VRAM stays held in the CUDA context.
  Steady-state turbo+int8 normally sits at ~1190 MiB; post-wedge it
  pegs at ~2600 MiB and free VRAM drops to single-digit MiB until the
  speaches container is restarted. Background already documented in
  memory note `project_speaches_stuck_models.md` and in the wedge test's
  preamble. **A 4-hour soak will hit this.** Three things M8B owes:
  1. **Detection metric** — sample `nvidia-smi --query-compute-apps`
     for the speaches process; emit `voice_speaches_vram_used_mib` and
     `voice_speaches_wedged{}` (1 when used > expected_model_size +
     800 MiB CUDA-context budget, 0 otherwise). Driven from the same
     poller that already reads `nvidia-smi` for `vram_low_threshold_mib`
     (`src/log/vram_monitor.cpp` post-M6).
  2. **Recovery action** — soak driver detects the wedged metric and
     issues `docker compose restart speaches` automatically. Counts
     toward the `service restarts ≤ 2 (incidental)` acceptance gate;
     more than two in a 4-hour window means the upstream bug
     regressed and the soak should fail.
  3. **`tools/acva-models recover` subcommand + `status` enhancement** —
     manual escape hatch for dev sessions, plus `status` flagging
     wedged-likely state with a remediation hint. Mirrors the
     soak-driver logic so the manual path matches the automated one.

  Out of M8B scope: actually fixing the upstream Speaches/faster-whisper
  bug. Workaround-via-restart is the documented stance; if the
  upstream bug is fixed before M8B closes, the metric and action can
  stay as a future-proofing safety net.

## Step 4 — Build-time observability + reductions — ✅ landed 2026-05-07 (results below the original aspiration)

Shipped:
- **Split `src/config/config.cpp`** into two TUs (`config.cpp` for
  validators + alias resolution, `config_load.cpp` for the Glaze
  YAML reader). The plan's hypothesis — that splitting would
  parallelize the Glaze instantiation across cores — turned out to
  be incorrect: Glaze's `read_yaml<Config>` instantiates the entire
  reflection tree at the call site, in one TU, no matter how you
  split the surrounding code. After-split clean-build wall was the
  same 211 s, all paid by `config_load.cpp`. **The split still
  pays off on incremental rebuilds** — edits to validators or alias
  resolution (the daily edit surface during M8A/M8B) now compile in
  ~5 s instead of 211 s. Operators touching `cfg.llm.foo` field
  defaults no longer block the loop on the Glaze rebuild.
- **`tests/test_pch.hpp` + `target_precompile_headers`** wiring
  for both `acva_unit_tests` and `acva_integration_tests`. Includes
  the most-used stdlib (chrono / vector / thread / string / atomic
  / filesystem / variant / memory / mutex / optional / cstdint /
  cstdlib / functional / string_view) plus the most-shared acva
  public headers (config, dialogue/turn, event/bus, event/event,
  memory/{db,memory_thread,repository}). doctest is intentionally
  NOT in the PCH — `test_main.cpp` defines
  `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` before its `#include`, and
  pre-including the header anywhere defeats that.
- **pImpl audit**: `http/server.hpp`, `llm/client.hpp`,
  `stt/{openai,realtime}_stt_client.hpp`, `tts/openai_tts_client.hpp`
  all already keep third-party includes (`<httplib.h>`,
  `<curl/...>`, `<rtc/...>`) out of their public surface — the
  pImpl pattern was already in place from M3/M5. The `to do` item
  in the plan was a phantom; it was done before M8B started.

Measured impact:
- **Clean-build wall: 214 s → ~215 s.** Unchanged. `config_load.cpp`
  remains the floor at ~211 s. The Glaze reflection over the
  Config struct is fundamentally one big template instantiation;
  the only meaningful path to reducing it further is replacing
  Glaze's YAML reader for this struct (e.g. with rapidyaml + a
  hand-rolled mapping), which is days of work for a 100s save —
  deferred as a future optimisation when iteration on `config.hpp`
  becomes a frequent pain point again.
- **Incremental wall (single test-file edit): ~2.8 s** (with PCH).
  Pre-PCH equivalent edits ran ~5–8 s. The PCH wins are
  per-test-TU, not on the wall floor (which is dominated by
  config_load.cpp on clean builds).
- **Sequential build time** (sum of all TU times): 743 s → 736 s.
  About 7 s of net wins from PCH cache hits across 50+ test TUs.
- **Full unit suite still green** (361 cases, all pass after the
  split + PCH).

Acceptance against the plan's gate:
- Plan: "clean `./build.sh` reports < 60 s wall on the 8-core dev
  workstation; no single TU > 60 s."
- Actual: 215 s wall, one TU at 211 s. **Not met** as written —
  the plan's clean-build target was unrealistic given Glaze's
  monolithic instantiation. **The achievable iteration target —
  fast incremental builds — is met**: a single test file edits in
  2.8 s, and validator edits no longer trigger the 211 s
  config_load rebuild.

Documented constraint: until / unless we replace Glaze for
`config_load.cpp`, the clean-build floor is ~210 s. We accept this
as the price of glaze-driven YAML parsing for a 30+ struct schema;
the alternative (hand-rolled YAML parser) is not warranted at
current iteration cadence.

## Step 4 — Build-time observability + reductions (original spec)

A clean build (post-orchestrator-refactor, 107 TUs / 8 cores) takes
**~10 s wall** but **~556 s sequential**. One TU still dominates:
`src/config/config.cpp` at **~139 s** by itself — Glaze's YAML
reflection instantiates the full template tree for every nested
struct in the schema. The next tier (`supervisor/probe`,
`llm/client`, `stt/realtime_stt_client`, `http/server`, the
`test_manager` / `test_speaches_realtime_smoke` /
`test_tts_bridge` / `test_summarizer` units) all sit at 14–20 s
each because of heavy transitive includes (libdatachannel, libcurl,
cpp-httplib, doctest, glaze-driven config types).

Already landed (2026-05-03):
- `cmake -DACVA_TIME_TRACE=ON` adds `-ftime-trace`; per-TU JSONs
  drop next to each `.o` for chrome://tracing flamegraphs.
- `scripts/build-times.sh [preset] [top-N]` parses
  `_build/<preset>/.ninja_log` and prints a sorted wall-time table.
- `./build.sh` calls the script automatically; suppress with
  `ACVA_BUILD_TIMES=0`, change cap with `ACVA_BUILD_TIMES_TOP=N`.

To do under M8B:
1. **Split `src/config/config.cpp`** so each subsystem's parsing +
   validation lives in its own TU. Each per-section TU then
   instantiates only its own slice of the reflection tree, and the
   work parallelises across cores. Target: drop config.cpp's ~139 s
   to ~7 × ~20 s on 8 cores ≈ ~20 s wall.
2. **PCH for the test suites.** A 200-line `tests/test_pch.hpp`
   covering doctest + the most-shared `acva_core` public headers,
   wired via `target_precompile_headers`. Cuts the ~20 s tier in
   half — biggest impact on the 8 worst test TUs.
3. **pImpl for libcurl / libdatachannel / cpp-httplib consumers.**
   `llm/client.cpp`, `http/server.cpp`, `stt/realtime_stt_client.cpp`,
   `tts/openai_tts_client.cpp`, `stt/openai_stt_client.cpp` — wrap
   the third-party type in an `Impl` struct in the .cpp,
   forward-declare in the .hpp. Stops every downstream TU from
   re-parsing the library headers.

Acceptance for Step 4: clean `./build.sh` reports < 60 s wall on the
8-core dev workstation; no single TU > 60 s.

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Soak infra + first run | 3 days |
| 2 Dashboard | 1 day |
| 3 OTLP | 1.5 days |
| 4 Build-time reductions (config split + PCH + pImpl) | 2 days |
| **Total** | **~7.5 days = ~1.5 weeks** |
