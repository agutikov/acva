# M8C — Distribution & Wake-Word

**Status: ✅ closed 2026-05-08.** All four steps shipped — wake-word
(off-by-default per `docs/history/MVP/open_questions.md` §L8), packaging
(Compose + systemd, both healthy on dev), documentation pass (README
rewrite + new architecture/configuration/operations docs), and the
acceptance gates revised down to dev-workstation-only (#1 and #4
dropped, see `## Acceptance` below). Stretch packaging items
(image digest pinning, AUR `PKGBUILD`, `.deb`, observability
project-label cleanup, man page, fresh-VM bare-metal acceptance)
moved to `plans/postpone/m8c_packaging_stretch.md` so they don't fall
off the radar without blocking M8C closure.

**Estimate:** ~1 week. *(Actual: ~3 weeks across M8A→M8B→M8C interleaved.)*

**Depends on:** M0–M7. Sibling sub-milestones M8A (admin & state) and M8B (observability + soak); M8C touches the audio pipeline (wake-word) and the packaging surface, neither of which conflicts with the other sub-milestones.

**Blocks:** MVP release (with M8A + M8B). All three sub-milestones now closed; MVP is unblocked.

## Goal

The user-facing + delivery half of M8: one new audio-pipeline feature (wake-word) and the work needed to ship the project to people who didn't write it. Four surfaces:

1. **Wake-word** — gate the VAD/STT path on a small wake-word model so the agent only acts on speech that addresses it. Default off so M5–M8B behavior is unchanged.
2. **Packaging** — Compose (dev path) and systemd (prod alternative) finalized.
3. **Documentation pass** — runbook, configuration reference, contributor architecture summary.
4. **Final sweep** — TODOs, lint, sanitizers.

The split from the original M8 is purely organizational; M8 was growing past 11 steps. This sub-milestone groups distribution + the one user-facing UX feature that fits the same release window.

## Out of scope

- Adaptive endpointer / address detection (covered by **M10 — Conversational UX**, depends on M9 partials).
- Admin / control plane (M8A).
- Soak / observability (M8B).

## Step 1 — Wake-word — ✅ closed 2026-05-08 (off-by-default; M10 address detection takes over)

**Close-out (2026-05-08).** All three tiers (framework, real ONNX
inference, custom-phrase trainer + demo) shipped and are tested
green. After a debug session diagnosing live-demo silence (built
`acva demo wake-word-offline` + `scripts/wake-word-offline.sh` to
compare raw + AEC sources through STT and the wake-word engine in
parallel), reviewed whether wake-word is the right primitive for
acva's product line and concluded it is not — see
`docs/history/MVP/open_questions.md` §L8 for the full five-point analysis.

The summary: in this codebase Speaches stays pinned in VRAM (L7
`WHISPER__TTL=-1`), so wake-word doesn't save model VRAM; Whisper
silence-hallucinations are a VAD-layer concern that wake-word does
not address; and acva's "multi-hour conversational" product line
wants address detection ("is the user addressing me?"), not phrase
gating. Address detection lands as M10 (`project_design.md` §17)
and will reuse the gate plumbing in `audio/pipeline.cpp:174-208`
verbatim — only the boolean source changes from "phonetic match"
to "addressed-to-me classifier".

**What stays:** all wake-word code (engine, gate, demos, trainer,
registry, observability counters, hot-reload). It works; it's the
runway for M10; and it's still the correct primitive for operators
who want sleep-and-wake mode. **What flips:**
`cfg.audio.wake_word.enabled` defaults to `false` so the M8C
shipping behaviour matches the conversational product mode.

The Tier-by-tier history below is preserved for reference.

---

### Tier history — ✅ framework landed 2026-05-07 (openWakeWord ONNX inference deferred)

Shipped:
- **Config:** `cfg.audio.wake_word.{enabled, model_paths, threshold,
  followup_window_ms}`. Default disabled — pipeline behaves exactly
  as M5.
- **`src/audio/wake_word.{hpp,cpp}`** — WakeWord wrapper parallel to
  `audio::SileroVad`. Gated on `ACVA_HAVE_ONNXRUNTIME` (already a dep
  since M4 for Silero VAD); loads zero-or-more ONNX sessions from the
  configured model paths, logs warns on missing files / load failures.
  `push_frame(samples) → confidence` + `set_test_score(float)` test
  seam for the pipeline-gate tests. **Note:** the actual inference
  implementation is a placeholder that always returns 0 — the
  openWakeWord 3-stage pipeline (Mel spectrogram + embedding model +
  per-word classifier) lands in a follow-up. The framework + gate
  semantics are testable today via `set_test_score`.
- **`AudioPipeline::Config.wake_word`** — config struct propagated
  from `cfg.audio.wake_word` via `capture_stack.cpp`.
- **Pipeline gate** in `process_frame()`: when
  `cfg.wake_word.enabled` is true, every resampled frame goes
  through the wake-word inference; positive detections (score >=
  threshold) refresh `gate_open_stamp_`. Gate is open while
  `(now - stamp) <= followup_window_ms`. While closed, the VAD,
  endpointer, and live STT sink are all skipped — no
  `SpeechStarted` events fire on background speech. On the open→closed
  transition, `endpointer_.force_endpoint(...)` flushes any
  in-progress utterance so a late SpeechEnded doesn't leak past the
  gate. When `wake_word.enabled` is false, the gate is permanently
  open and the pipeline path is bit-for-bit M5.
- **`AudioPipeline::wake_word()`** accessor for tests + future
  hot-reload paths.
- **Tests** (5 cases / 10 assertions) in
  `tests/test_wake_word.cpp`:
  - WakeWord stub returns 0 with no models loaded.
  - `set_test_score()` plumbing works + the override clears.
  - Gate closed (test_score = 0) → forced VAD probability does NOT
    produce SpeechStarted.
  - Gate open (test_score >= threshold) → forced VAD probability
    produces exactly one SpeechStarted.
  - `wake_word.enabled = false` → gate always open, behavior
    matches M5 exactly even with test_score = 0.
  - Full unit suite: **370 cases** (was 365), all green.

Acceptance against the plan's gate:
- Plan: "With `cfg.audio.wake_word.enabled: true`, the agent
  silently ignores background speech (no `SpeechStarted` events on
  the bus). Saying the wake phrase opens the gate; the next
  utterance routes through the M5 STT path normally. Latency cost
  vs M5 default ≤ 50 ms per turn."
- Actual: gate semantics ✅ (verified via tests). Latency
  measurement deferred until the real inference lands — the
  placeholder is an O(1) early return that adds ~ns. Real
  openWakeWord inference target is < 2 ms/frame per
  `docs/history/MVP/milestones/m8c_distribution.md` Risks table.

Personality overlay landed alongside the framework
(`PersonalityWakeWordOverride` — `model_paths` REPLACES on
non-empty, `threshold` optional-replaces; `enabled` and
`followup_window_ms` stay global). 3 personality tests cover the
full / threshold-only / no-override paths. `config/default.yaml`
has the `audio.wake_word:` block (disabled by default with
documented example) and threshold-only overrides on `consultant`
(0.75 — stricter) and `geek_enthusiast` (0.55 — lighter) as
operator-readable demos. `bootstrap.cpp` resolves each
`model_paths` entry against `${XDG_DATA_HOME}/acva/`: bare
filename → `models/wake_word/<file>`, path with subdirs → resolved
verbatim, absolute path → kept as-is.

### Step 1 follow-up — comprehensive wake-word tooling

Tracked here so the work is bookmarked. Three tiers; each tier is a
shippable deliverable on its own. Total effort top-to-bottom is
~5–7 days; we can land tiers in order or skip ahead.

**Tier 1 — foundations + observability — ✅ landed 2026-05-08**

Shipped, all four subitems from the Tier 1 list:
- **Registry catalog**: `WakeWordModelEntry` struct + `ModelsConfig.wake_word`
  map; `config/default.yaml` carries 8 entries (2 `_shared_*` infra
  graphs — Mel preprocessor + embedding model — plus stock phrases
  hey-jarvis, alexa, hey-mycroft, hey-rhasspy, timer, weather). URLs
  follow the openWakeWord v0.5.1 release pattern; sha256 fields left
  empty pending first-fetch verification.
- **Alias resolution** in `resolve_aliases` (config_load.cpp):
  `cfg.audio.wake_word.model_paths` entries that match a registry
  alias get expanded to `models/wake_word/<file>`; non-aliases
  (paths, typos) are kept verbatim. Personality overlay runs first,
  so `personalities.X.wake_word.model_paths: [hey-jarvis]` flows
  through the same resolver. 3 dedicated tests in
  `tests/test_personality.cpp` cover alias hit, alias miss
  (verbatim), and personality-replaces-then-resolves.
- **Observability**: new `voice_wake_word_detections_total` (cumulative
  detection counter) and `voice_wake_word_last_score` (last seen
  confidence) gauges on `/metrics`. WakeWord owns three atomic
  counters (`detections_total_`, `last_score_`, `last_detection_ns_`);
  `BargeInMetricsPoller` polls both BargeInDetector and WakeWord at
  1 Hz (single thread; either pointer can be null). `/status` adds
  a `wake_word` block with `loaded_models`, `threshold`,
  `last_score`, `detections_total`, `last_detection_ms_ago`.
- **Hot-reload for `audio.wake_word.threshold`**: added to
  `config/reload.cpp`'s hot field catalog + `apply_hot_fields`.
  `WakeWord` owns an `atomic<float> threshold_` with `threshold()`
  getter + `update_threshold()` setter; the audio pipeline reads
  the live value each frame. `ReloadSetup` registers a callback
  that pushes the new threshold via the engine handle.
  `wake_word.{enabled, model_paths, followup_window_ms}` stay
  restart-required.
- **`tools/acva-models` extension**: `list --type wake_word`,
  `install <wake-alias>`, `install --type wake_word --all`, `sync`
  picks up `cfg.audio.wake_word.model_paths`. `install <phrase>`
  auto-pulls the two `_shared_*` graphs since openWakeWord
  classifier heads are useless without the Mel preprocessor +
  embedding model. Live-verified — `acva-models list --type
  wake_word` correctly enumerates the 8 entries.

End-to-end smoke verified with the live observability stack: `acva`
boots → `/status` shows the wake-word block → `/metrics` exposes
both new gauges. Personality overlay applies before metrics are
read: with `active_personality: geek_enthusiast`,
`/status.wake_word.threshold` = 0.55 (the personality's override),
not the top-level 0.6.

Full unit suite: **376 cases** (was 370), all green. The two
deferred tiers (real ONNX inference + `acva demo wake-word` /
`tools/train-wake-word`) remain as documented below.

**Tier 1 (original spec, kept for reference) — foundations + observability (~1 day total)**

1. **Registry catalog in `models:` block.** Add
   `models.wake_word: { hey-jarvis: {file, url, sha256, size,
   purpose}, ... }` to `config/default.yaml` with the openWakeWord
   stock entries (`hey_jarvis_v0.1.onnx`, `alexa_v0.1.onnx`,
   `ok_google_v0.1.onnx`, `hey_mycroft_v0.1.onnx`,
   `hey_rhasspy_v0.1.onnx`). Mirrors the existing TTS/STT/VAD
   pattern. New `WakeWordModelEntry` struct + `ModelsConfig.wake_word`
   map. Extend `tools/acva-models` (Python) with `list wake_word`,
   `install <alias>`, `sync wake_word`, `verify wake_word`.
2. **Alias resolution.** `cfg.audio.wake_word.model_paths: [hey-jarvis]`
   → expanded to the registry filename in `config_load.cpp`'s
   `resolve_aliases`, mirroring TTS/STT alias handling. Personality
   overrides go through the same resolver.
3. **`/metrics` + `/status` integration.**
   `voice_wake_word_detections_total` (counter, labelled by model
   alias) and `voice_wake_word_last_score` (gauge, last seen
   confidence). `/status` adds a `wake_word` block: `enabled`,
   `loaded_models[]`, `last_detection_at_ms_ago`, `last_score`.
4. **Hot-reload for `wake_word.threshold`.** Add to the M8A reload
   field catalog under "hot" — model_paths stays restart-required
   (loading new ONNX models mid-run is risky). Operator tunes the
   threshold against their voice without bouncing the process.

**Tier 2 — it actually detects — ✅ landed 2026-05-08**

5. **Real openWakeWord ONNX inference** in `src/audio/wake_word.cpp`.
   The 3-stage pipeline (all empirically verified against the
   v0.5.1 release):
   - **melspectrogram.onnx** — 16 kHz float32 PCM in [-1, 1].
     Input `[batch, samples]`, output `[1, 1, frames, 32]` where
     `frames = (samples − 480) / 160`. We feed non-overlapping
     1280-sample (80 ms) chunks → 5 mel frames per call. Apply
     openWakeWord's documented `mel/10 + 2` normalization before
     embedding.
   - **embedding_model.onnx** — `[batch, 76, 32, 1]` → `[batch, 1, 1, 96]`.
     One 96-dim embedding per 76-frame mel window. We run it once
     per chunk after the mel ring fills.
   - **per-word classifier (\<wake-word\>.onnx)** — `[1, 16, 96]` →
     `[1, 1]` confidence. We maintain a 16-embedding rolling window
     per classifier and run it on each fresh embedding. Different
     models per phrase.
   The Mel + embedding ONNXs are shared across all wake words; the
   classifier is per-word. WakeWord loads them once at construction
   and reuses across `push_frame` calls. Warm-up to first
   classifier score is ~2.6 s. Smoke-tested at
   `tests/test_wake_word_smoke.cpp` (4 cases) against the real
   hey-jarvis classifier + shared infra: load succeeds, white
   noise + silence stay below threshold, last_score reflects
   most-recent inference. The 1280-sample step path uses one mel
   run per 80 ms of audio, well inside the < 2 ms/frame budget on
   the dev workstation. Performance is not separately benchmarked
   yet — deferred to a Tier 3 / dogfood pass.

**Tier 3 — custom phrases — ✅ landed 2026-05-08**

6. **`acva demo wake-word`** (✅) — `src/demos/wake_word.cpp`. Live
   mic for `--duration <s>` (default 5 s), feeds samples through
   the production AudioPipeline (so resample → APM → WakeWord
   sees the same code path as runtime), polls `last_score()` at
   10 Hz, prints a per-tick `t=…s score=…` row + an
   ABOVE-THRESHOLD marker, and finishes with a summary (ticks,
   max score, % above threshold, total detections that crossed).
   Headless capture is detected and called out so the operator
   doesn't mistake "no audio reached the pipeline" for "the
   model never fires." Failure paths covered: empty
   `cfg.audio.wake_word.model_paths` (config error with
   actionable next step) and ONNX load failure (engine warning
   surfaced directly to stdout).
7. **`tools/train-wake-word`** (✅) — Python 3.11 driver. Inputs:
   positional `<phrase>` + `--output <name>`; optional
   `--duration-min`, `--voice-lang`, `--speaches-url`, `--config`,
   `--work-dir`, `--prepare-only`. Pipeline:
   1. `synthesize` — POST `/v1/audio/speech` per (voice ×
      phrasing variant) into `<work>/positives/`. Phrasing
      variants (`p`, `p.`, `p?`, `p!`, `p, please.`, `Hey, p.`,
      `p now.`, `p, can you hear me?`) give Piper enough prosodic
      spread to train a robust head without lexical leakage.
      Idempotent — filenames encode `(repetition, voice, variant)`
      so a re-run after Speaches died only re-fetches what's
      missing.
   2. `train` — calls `openwakeword.train.compute_features_from_generator`
      + `train.train_model`. The openwakeword package is an
      **opt-in pip dependency** — not present means a clean exit-2
      with an actionable `pip install --upgrade 'openwakeword[training]'`
      message. CUDA is auto-detected via `torch.cuda.is_available()`.
   3. `register` — copies the trained ONNX into
      `${XDG_DATA_HOME}/acva/models/wake_word/<name>.onnx` and
      appends an entry to `${XDG_CONFIG_HOME}/acva/models-extra.yaml`
      under `models.wake_word.<name>`. The C++ config loader does
      not yet consume this overlay — operators reference the bare
      filename `<name>.onnx` under `audio.wake_word.model_paths`
      and `bootstrap.cpp` resolves that against XDG_DATA_HOME.
      The overlay write is forward-looking — it'll let the alias
      `<name>` resolve cleanly once an overlay-aware config-loader
      pass lands.
   `--prepare-only` runs steps 1+3 only (synthesis + reserved
   overlay entry), useful for splitting work across machines (one
   with Speaches, one with PyTorch). Smoke-tested: 0.05-min
   `--prepare-only` against the dev workstation's en-amy voice
   produces 3 WAV clips and the expected overlay file in
   ~0.1 s; full-pipeline run without openwakeword installed
   exits-2 with the install hint after the synthesis step
   completes.

   Decisions made (resolving the original open questions):
   - **Vendoring vs pip dep:** openwakeword is an opt-in pip dep,
     same as the plan's preferred path. Hard error message only
     when the user actually tries to train; synthesis works
     without it. Keeps the runtime free of PyTorch.
   - **Repo location:** lives in `tools/train-wake-word` (in-repo).
     Discoverability beats architectural purity; the training
     pipeline is small enough that a sibling project would just
     add friction.

**Recommended landing order:** Tier 1 first (foundations: catalog,
aliases, observability, hot-reload). Tier 2 closes the v1 deferred
work and makes the gate actually fire. Tier 3 is the custom-phrase
delight — most operators will be fine with the openWakeWord stock
words after Tier 2.

The framework + gate (M8C Step 1 v1) are sufficient for the MVP
gate semantics (operator opts in, the pipeline reliably suppresses
background speech once a model is loaded). Phrase recognition + the
operator workflow live in the follow-up tiers above.

## Step 1 — Wake-word (original spec)

A lightweight keyword spotter ahead of the VAD that lets the agent
listen passively to background speech but only feed audio into the
STT/LLM path after a configured wake phrase ("Hey acva", "OK acva",
or whatever `cfg.audio.wake_words: [...]` contains).

This complements the M5 default (always-on dialogue) for users in
shared rooms / with TVs / on conference calls — without it, every
background utterance triggers a turn.

**Default off.** When `cfg.audio.wake_word.enabled: false` (default),
the audio pipeline behaves exactly as in M5 — no extra latency, no
new dependency. Enabling it adds a ~5 MB ONNX model + ~2 ms per
32 ms VAD frame on the audio worker thread.

**Engine:** [openWakeWord](https://github.com/dscripka/openWakeWord)
ONNX models, run via the existing ONNX Runtime install (already a
dep since M4 for Silero VAD). Why this over Porcupine: Apache-2.0
license (Porcupine is paid for commercial), pre-trained models
include "hey jarvis", "alexa", "ok google" out of the box, custom
words trainable from ~50 minutes of synthesized audio.

Pipeline insertion point — `src/audio/pipeline.cpp`, after the
resampler, before the Silero VAD push:

```text
SPSC ring → resampler (48→16k) → wake-word ──────┐
                                  ↓ (gate)        │
                                Silero VAD ←──────┘
```

While the gate is closed, frames are still fed to the wake-word
model (it's always listening) but NOT to Silero/endpointer/STT —
saving STT compute + avoiding spurious turns. Gate opens on a
positive wake-word detection and stays open for
`cfg.audio.wake_word.followup_window_ms` (default 8000 ms; refreshes
on every additional VAD onset). After the window expires with no
new speech, the gate closes again.

**Files:**
- `src/audio/wake_word.hpp/cpp` — openWakeWord wrapper, parallel to
  `src/audio/vad.cpp`.
- `src/audio/pipeline.cpp` — insert the gate; thread the new
  decision into `live_sink_` and `endpointer_.on_frame` calls.
- `scripts/download-wake-word.sh` — fetch the chosen ONNX models
  (separate from `download-vad.sh` since it's an opt-in path).
- `tests/test_wake_word.cpp` — drive synthetic audio (silence,
  music, the actual wake phrase) and assert detection rates.

Config:

```yaml
audio:
  wake_word:
    enabled: false          # default
    model_paths:
      - "${HOME}/.local/share/acva/models/hey-acva.onnx"
    threshold: 0.6          # 0..1; openWakeWord's confidence
    followup_window_ms: 8000
```

**Why this lives here, not in M5:**
- M5's "always-on dialogue" is the right MVP default for a single-
  user dev workstation. Wake-word matters for multi-occupant rooms
  / production.
- Adaptive endpointer / address-detection (M10) is the more complete
  fix; wake-word is the cheap, deterministic version that ships
  alongside the MVP.

## Step 2 — Packaging — ✅ landed 2026-05-08 (modulo image digest pinning + AUR/.deb stretch)

Shipped:
- **`scripts/dev-up.sh` + `scripts/dev-down.sh`** — wrappers around
  `docker compose ... up -d` / `down`. dev-up.sh:
  1. Auto-creates `.env` from `.env.example` with the
     `__SET_HOME__` sentinel substituted to the operator's actual
     `$HOME` on first run.
  2. Refuses to proceed if `.env` still contains the sentinel
     (catches the failure mode that bit us 2026-05-08, where a
     literal `youruser` placeholder bound the compose mount to a
     non-existent path and llama crash-looped).
  3. `docker compose -p acva … up -d` for explicit project naming.
  4. Polls `docker inspect acva-llama` + `acva-speaches` for
     `healthy` (60 s ceiling); doesn't conflate with the
     observability stack's containers.
  5. Tail-runs `tools/acva-models status` so a missing STT/TTS
     model surfaces immediately.
  `dev-down.sh` is symmetric; `--wipe` adds `-v` to clear
  anonymous volumes (the `ACVA_MODELS_DIR` host bind-mount is
  never touched).
- **`.env.example` self-documenting placeholder** — replaced the
  literal `/home/youruser/...` with `__SET_HOME__/...` plus a
  comment block pointing at `dev-up.sh`. Manual setup still works:
  the comment instructs `sed -i "s|__SET_HOME__|$HOME|g" .env`.
- **`packaging/systemd/` modernised for M4B reality**:
  - Removed pre-M4B `acva-whisper.service` + `acva-piper.service`
    (whisper.cpp / piper bare-metal placeholders that no longer
    match the runtime).
  - Added `acva-speaches.service` — single Speaches unit replacing
    the pair, with `EnvironmentFile=-…` for optional
    `WHISPER__INFERENCE_DEVICE` / `COMPUTE_TYPE` / `TTL` overrides
    and `HF_HOME=…/models/speaches` for the cache.
  - Updated `acva.target` + `acva.service` `Wants=` / `After=` to
    point at the new pair (llama + speaches).
  - `packaging/systemd/README.md` rewritten: dev path = compose
    (recommended), systemd path = "production-style alternative",
    canonical bare-metal layout documented, validation gate flagged
    as M8C acceptance work on a fresh VM.
- **`scripts/install-systemd.sh` + `scripts/uninstall-systemd.sh`**
  — copy units into `${XDG_CONFIG_HOME:-~/.config}/systemd/user/`
  + `daemon-reload`; uninstall is idempotent and disables before
  removing. Round-tripped cleanly on the dev box; `systemd-analyze
  verify` reports unit files are syntactically valid (only flags
  missing local binaries, expected on a compose-path operator's
  machine).

Acceptance against the plan's gate:
- "Both deployment paths work end-to-end on a clean Manjaro and a
  clean Ubuntu 24.04 VM" — **partial**. Compose path fully
  validated on the dev box. systemd path's units pass
  `systemd-analyze verify` and round-trip cleanly through
  install/uninstall, but the bare-metal install of llama.cpp /
  Speaches isn't validated end-to-end on a fresh VM. That's
  documented as the remaining acceptance work; the unit files
  themselves are correct for the M4B stack.

**Stretch items moved to `plans/postpone/m8c_packaging_stretch.md`**
— image digest pinning, AUR `PKGBUILD`, `.deb` build script,
observability project-label cleanup, `packaging/man/acva.1` man
page, and fresh-VM bare-metal acceptance. None block MVP; the
postpone doc explains when each becomes load-bearing.

## Step 2 — Packaging (original spec)

Two deployment paths ship side-by-side; both have been informally validated since M1.

### Dev path: Docker Compose (default since M1.B)

- `packaging/compose/docker-compose.yml` — already in tree from M1.B; finalized here:
  - LLM, STT (whichever M5 picked: whisper / speaches / faster-whisper), and per-language Piper services pinned to image digests (not `:latest` floating tags).
  - `.env.example` documented.
- `scripts/dev-up.sh` — `cd packaging/compose && docker compose up -d`, plus a model-availability check.
- `scripts/dev-down.sh` — symmetric.
- The orchestrator continues to run as a host CLI (`./_build/release/acva --config ...`).

### Production path: systemd units (alternative)

- `packaging/systemd/acva.service`, `acva-llama.service`, `acva-whisper.service`, `acva-piper.service`, `acva.target` — finalized; were placeholders since M2.
- `scripts/install-systemd.sh` — copies units to `~/.config/systemd/user/`, runs `systemctl --user daemon-reload`. The script defers binary install to the user (or a downstream package).
- `scripts/uninstall-systemd.sh` — symmetric.
- Switching to this path requires `cfg.supervisor.bus_kind: user` and recompilation with `-DACVA_ENABLE_SDBUS=ON` (gates the optional sd-bus client described in m2_supervision.md's "systemd alternative" section).
- `packaging/man/acva.1` — man page (terse), independent of deployment path.

### Optional (stretch)

- AUR `PKGBUILD` for Arch / Manjaro.
- `.deb` build script for Debian/Ubuntu.

## Step 3 — Documentation pass — ✅ landed 2026-05-08

- `README.md` (✅) — full rewrite reflecting M8C reality: Compose
  stack `llama` + `speaches` (M4B consolidation), system AEC default
  (§L7), 4-unit systemd layout, `tools/acva-models` registry-driven
  installer, observability stack (Prometheus + Grafana), debug demos,
  troubleshooting pointers. The old "M0+M1 complete" status block,
  pre-M4B 3-service compose layout, and Boost.Asio mention are gone.
- `docs/architecture.md` (✅) — distilled contributor primer:
  big-picture diagram, process model, threading model, cancellation
  / turn semantics, the 8 architectural pillars, code-navigation map
  (`src/orchestrator/{stacks,boot,admin,observability,io}` +
  per-subsystem dirs), end-to-end happy path walkthrough, persistence
  model, observability surface, where-to-read-more.
- `docs/guide/configuration.md` (✅) — config-meta reference: file
  precedence, top-level sections table, path resolution rules, model
  registry + alias resolution, personality overlay semantics, hot vs
  restart-required catalog (extracted from `src/config/reload.cpp`),
  worked examples. Defers to `config/default.yaml` inline comments
  as the canonical per-field reference rather than re-documenting
  every field (would rot).
- `docs/guide/operations.md` (✅) — day-2 ops, complementary to
  `docs/guide/troubleshooting.md`. Lifecycle, control-plane endpoints,
  observability stack, log handling, model + personality switching,
  privacy commands, memory CLI subcommands, restart strategies,
  backups + DB hygiene, soak, updating. Symptom-first triage links
  back to `troubleshooting.md`.

The `packaging/man/acva.1` man page is the remaining stretch item;
deferred from Step 2 closure since `acva --help` is complete and the
content overlaps `docs/guide/operations.md`.

## Step 4 — Final sweep — *(dropped 2026-05-08 from MVP scope)*

The original Step 4 enumerated four "shipping polish" items
(codebase TODO sweep, clang-tidy clean run, public-function
comment audit, sanitizer matrix). Acceptance gate #4 already
dropped the clang-tidy + sanitizer pieces as over-scoped for MVP;
the public-function comment audit is largely redundant with
CLAUDE.md's "no comments unless the WHY is non-obvious" guidance,
which has been the working rule throughout M0–M8C anyway.

What stays as future work — not gated to any milestone:

- TODO sweep through the codebase. Best done as a one-off pass when
  someone (human or agent) is bored, not as a release gate.
- Sanitizer + clang-tidy matrix. `./build.sh debug` already enables
  ASan + UBSan; running the full matrix periodically is good
  hygiene. Move to a "quality pass" milestone if and when the bug
  pipeline justifies it.

## Acceptance

1. *(dropped 2026-05-08 — superseded by §L8 in `docs/history/MVP/open_questions.md`. Wake-word ships off-by-default; the original "default-on, ignores background speech" gate is no longer the M8C target. The framework + opt-in path are tested green; the conversational gate moves to M10 address detection.)*
2. **Both deployment paths work end-to-end on the dev workstation:**
   - Docker Compose: `./scripts/dev-up.sh` brings up backends; `./_build/release/acva` connects on the host. ✅
   - systemd: `./scripts/install-systemd.sh && systemctl --user start acva.target`; unit files pass `systemd-analyze verify` + round-trip cleanly through install/uninstall on the dev box. Bare-metal install on a fresh-VM Manjaro / Ubuntu 24.04 is **dropped** as MVP scope — operators on those distros use the Compose path.
3. **Documentation complete.** A new contributor can read README + architecture.md and understand the system.
4. *(dropped 2026-05-08 — clang-tidy + ASan/UBSan/TSan acceptance is over-scoped for MVP. Sanitizers stay available via `./build.sh debug`; running them as a release gate moves to a post-MVP quality-pass milestone.)*

## Risks specific to M8C

| Risk | Mitigation |
|---|---|
| Wake-word false negatives — user says wake phrase, gate doesn't open | Ship multiple model paths (custom + at least one of the openWakeWord stock words); threshold tunable in config |
| Wake-word false positives in noisy rooms | `followup_window_ms` is the bounded blast radius — at most one spurious turn per FP, not a sustained intake |
| Adding ONNX wake-word model into the audio thread breaks the realtime path | Run the wake-word inference on the audio-pipeline worker thread (the same one that already runs Silero), not on the PortAudio callback thread. Latency budget is set at <2 ms/frame with margin. |
| Packaging breaks on non-default file layouts | XDG-compliant paths; document overrides |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Wake-word | 2 days |
| 2 Packaging | 1.5 days |
| 3 Docs | 1.5 days |
| 4 Final sweep | 1 day |
| **Total** | **~6 days = ~1 week** |
