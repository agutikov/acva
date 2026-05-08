# M8C — Distribution & Wake-Word

**Estimate:** ~1 week.

**Depends on:** M0–M7. Sibling sub-milestones M8A (admin & state) and M8B (observability + soak); M8C touches the audio pipeline (wake-word) and the packaging surface, neither of which conflicts with the other sub-milestones.

**Blocks:** MVP release (with M8A + M8B).

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

## Step 1 — Wake-word — ✅ framework landed 2026-05-07 (openWakeWord ONNX inference deferred)

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
  `plans/milestones/m8c_distribution.md` Risks table.

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

**Tier 2 — it actually detects (~2 days)**

5. **Real openWakeWord ONNX inference** in `src/audio/wake_word.cpp`.
   The 3-stage pipeline:
   - **melspectrogram.onnx** — 16 kHz int16 PCM (1280-sample
     window, 320-sample hop = 80 ms / 20 ms) → 32 mel bins.
   - **embedding_model.onnx** — Mel features → 96-dim embedding.
   - **per-word classifier (\<wake-word\>.onnx)** — embedding window
     → confidence in [0..1]. Different models per phrase.
   The Mel + embedding ONNXs are shared across all wake words; the
   classifier is per-word. WakeWord loads them once at construction
   and reuses across `push_frame` calls. Replace the v1
   `return 0.0F` placeholder with the real inference. Acceptance
   target from the milestone Risks table: **< 2 ms/frame** on the
   audio worker thread.

**Tier 3 — custom phrases (~1.5 days)**

6. **`acva demo wake-word`** — live mic for `--duration <s>` (default
   5 s), feeds samples through the configured wake-word model, and
   prints per-frame confidence + a final summary (max score, count
   above threshold). Operator-side threshold tuning aid;
   complements `acva demo capture` for VAD.
7. **`tools/train-wake-word`** — Python helper that drives the
   openWakeWord training pipeline using acva's local Piper.
   Inputs: `<phrase>` + output filename. Synthesises ~50 min of
   positive audio across many Piper voices, downloads /
   caches the openWakeWord embedding model + negatives corpus,
   trains the classifier head, emits ONNX into
   `${XDG_DATA_HOME}/acva/models/wake_word/<name>.onnx` and
   appends an entry to a local `models-extra.yaml` overlay. Drives
   on Speaches' OpenAI-API surface (already running) so it doesn't
   pull a separate Piper install. ~30 min wall-clock per phrase
   on the dev box GPU.
   - Open question: vendor openWakeWord's training code as a Python
     dep, or install via `pip` and treat it as a build-time
     prerequisite. The project's existing `tools/acva-models`
     already requires PyYAML; another opt-in dep for `train-wake-word`
     is acceptable as long as it's not a hard dep for the runtime.
   - Adjacent decision: lives in the project repo
     (`tools/train-wake-word`) vs a sibling project. The training
     pipeline is loosely coupled to acva — only the output ONNX
     needs to land in the registry. Sibling project is cleaner
     architecturally; in-repo is more discoverable.

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

Deferred (the "Optional (stretch)" items + cleanup identified
mid-work):
- **Image digest pinning** in `packaging/compose/docker-compose.yml`
  — currently `:server-cuda` (rolling llama.cpp tag) and
  `:latest-cuda` (rolling Speaches tag). Pinning to `@sha256:…`
  digests is reproducible-build territory; deferred to MVP-cut
  time when the digests are otherwise stable.
- **AUR `PKGBUILD`** for Arch / Manjaro, **`.deb` build script**
  for Debian/Ubuntu — both stretch in the plan, deferred to a
  post-MVP packaging push.
- **Compose project-label clean-up** — the
  `packaging/observability/` containers ended up labelled
  `project=acva` despite the YAML's `name: acva-observability`
  (compose-v2 quirk with `--project-directory` overriding the
  YAML name). dev-up.sh works around it by polling specific
  container names instead of the project; a real fix is to rename
  the observability project to something less collision-prone
  (`acva-obs`?). Documented but not fixed.
- **`packaging/man/acva.1`** man page — listed in the original
  spec; not yet written. Trivial follow-up given `acva --help`
  is already complete; pairs naturally with M8C Step 3 (docs).

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

## Step 3 — Documentation pass

- `README.md` — installation steps refined based on real-user experience during M0–M7. (Already partially done; needs final pass.)
- `docs/operations.md` — runbook for "the LLM is unhappy", "the mic isn't picked up", common failure modes.
- `docs/configuration.md` — full reference of every config field, with default and notes.
- `docs/architecture.md` — distilled summary for new contributors.

## Step 4 — Final sweep

- Address every TODO in the codebase (or open an issue for it).
- Run clang-tidy with the project's `.clang-tidy` config; fix or suppress.
- Make sure every public function has at least a one-line comment when the *why* is non-obvious (per CLAUDE.md guidance).
- Run the full test suite under ASan, UBSan, TSan once each.

## Acceptance

1. **Wake-word works.** With `cfg.audio.wake_word.enabled: true`, the agent silently ignores background speech (no `SpeechStarted` events on the bus). Saying the wake phrase opens the gate; the next utterance routes through the M5 STT path normally. Latency cost vs M5 default ≤ 50 ms per turn.
2. **Both deployment paths work** end-to-end on a clean Manjaro and a clean Ubuntu 24.04 VM:
   - Docker Compose: `docker compose up -d && ./scripts/dev-up.sh` brings up backends; `./_build/release/acva` connects on the host.
   - systemd: `./scripts/install-systemd.sh && systemctl --user start acva.target` brings up the full stack as units; `systemctl --user status` shows all four `active (running)`.
3. **Documentation complete.** A new contributor can read README + architecture.md and understand the system.
4. **Final sweep clean.** Zero clang-tidy errors, zero ASan/UBSan/TSan reports on the test suite.

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
