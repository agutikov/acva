# Bug — Wake-word latency + threshold characterization

**Status:** open / characterization (wake-word is off-by-default in M8C; this bug doesn't block MVP)  
**Severity:** medium (operators who opt in to wake-word with `enabled: true` will hit it)  
**Filed:** 2026-05-08  
**Stack:** M8C — `src/audio/wake_word.cpp`, `src/audio/pipeline.cpp:174-208`

## Symptom

Two related issues observed during the M8C dogfood pass on
2026-05-08 (offline-replay debug session — see
`scripts/wake-word-offline.sh`):

### 1. Threshold is too high for accented speech

The default `cfg.audio.wake_word.threshold` is **0.55** (top-level)
and **0.6** in the registry comment / **0.75** for the
`consultant` personality / **0.55** for `geek_enthusiast`. On
Russian-accented "hey jarvis", the openWakeWord stock
`hey-jarvis` classifier peaks at **0.37 / 0.47** (raw mic / AEC
source) — well above the noise floor (≈ 0.001) but **below
threshold**, so detection never fires.

```
RAW   t=2-3s max=0.372    AEC   t=2-3s max=0.467
RAW   t=9-10s max=0.322   AEC   t=3-4s max=0.394
```

Whisper transcribes the same audio cleanly (`"Хей Джарвис, Хей
Джарвис, …"`), so the audio is real. The model is functioning
correctly — it's just less confident on accented English than the
0.55 default assumes.

### 2. Warm-up + per-frame latency

`acva demo wake-word` reports a **~2.6 s warm-up** before any
inference fires (the mel-spectrogram + embedding window need to
fill before the classifier can score). This is documented in
`src/audio/wake_word.hpp` and is correct openWakeWord behavior,
but it surprises operators who say "hey jarvis" in the first 3
seconds and see `score=0.000` per tick.

Per-frame inference latency hasn't been characterized. The
in-budget claim is "< 2 ms/frame on the dev workstation"
(`docs/history/MVP/milestones/m8c_distribution.md` Tier 2 Step 5)
but no benchmark numbers ship with the binary. **Deferred to
a Tier 3 / dogfood pass** per the M8C plan — that pass has not
yet happened.

## What "fixed" looks like

### For threshold (1)

Multiple paths, none mutually exclusive:

- **Lower the default** to `0.45` so accented "hey jarvis" fires
  at the cost of slightly more false positives. Operators who
  want strict can raise it via personality overlay (`consultant`
  already does this with `0.75`).
- **Train a personalized classifier** via `tools/train-wake-word`
  against the operator's voice (synthesizes positives via
  Speaches TTS across multiple voices) — this is the M8C Tier 3
  shipped path.
- **Document** the calibration step explicitly in
  `docs/guide/operations.md` § "Wake-word setup": run
  `acva demo wake-word --duration 30` against the operator's
  actual mic, observe peak scores, set threshold to peak × 0.85
  or so.

Recommended: **doc + lowered default**. Personalized training is
the right answer for a sleep-and-wake deployment but is heavier
than most operators want.

### For warm-up + latency (2)

- **Surface the warm-up state** in `acva demo wake-word` output:
  the first ~26 ticks (at 100 ms poll) should print
  `t=…s warm-up` instead of `score=0.000`, so the operator
  doesn't think the model is broken.
- **Benchmark per-frame inference latency** (the "deferred to
  Tier 3" line item). Run the offline scorer against a 10-min
  WAV and report mean / p95 / max per-step latency. If > 2 ms
  on the dev workstation, find the slow stage (mel / embedding /
  classifier) and lower batch.

## How to characterize

```sh
# Build + record raw + AEC for 15 s, score offline
./build.sh
./scripts/wake-word-offline.sh 15

# Look at the per-second max output. If peaks are 0.3-0.5 and
# threshold is 0.55, that's bug #1.

# Live demo to see warm-up
./_build/dev/acva demo wake-word --duration 30
# First ~26 ticks read 0.000 by design — that's bug #2's UX.
```

## Related

- `plans/open_questions.md` §L8 — why wake-word is off-by-default
  (this bug is part of the reasoning behind that decision; the
  conversational gate moves to M10 address detection)
- `docs/history/MVP/milestones/m8c_distribution.md` Tier 2 Step 5
  ("Performance is not separately benchmarked yet — deferred to a
  Tier 3 / dogfood pass")
- `src/audio/wake_word.cpp` `WakeWord::Impl::run_inference_step`
- `config/default.yaml` `audio.wake_word.threshold`
- `tools/train-wake-word/` — the personalized-classifier path
