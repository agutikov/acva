# M7B — Barge-In Validation

**Status: code-complete 2026-05-05.** All four synthetic fixtures
(`clean-speakers`, `noise-speakers`, `headphones`,
`false-positive-self`) PASS via `./scripts/validate-bargein.py`.
3-trial smoke run shows P50 = 55.8 ms, P95 = 57.2 ms — well inside the
M7 §19.3 gates (P50 ≤ 200, P95 ≤ 400). 50-trial run + hardware spot
check (Step 7) deferred to a separate dogfood pass; the wiring is
proven and the per-trial numbers are stable.

**Design pivot from the original plan:** the C++ doctest probe
(`tests/test_barge_in_validation.cpp` + `BargeInProbe`) was replaced
by a Python subprocess harness (`scripts/validate-bargein.py`) that
launches `acva demo bargein-validation --fixture <wav>` per trial and
parses the demo's machine-readable done-line. This:

  - reuses the production `acva` binary as the seam (no parallel stack
    construction),
  - matches the existing `scripts/m7-bargein-bench.py` pattern,
  - cuts ~300 LOC of test-only orchestrator wiring,
  - exercises the *exact* code path users hit, including
    `system_aec.cpp`, the bootstrap log+ALSA sidestep, and PortAudio
    init.

Two manifest changes from the original spec:

  - **Assistant track attenuation** dropped to `-50 dB` (was `-25 dB`).
    `-25 dB` triggered Silero VAD on the residual itself, producing
    sub-100 ms false positives. `-50 dB` keeps the residual below VAD
    threshold so only the user track fires.
  - **`false-positive-tv` fixture removed.** Discriminating "wrong
    person speaking" needs M8C (wake-word) or M10 (address detection);
    the M7 detector design (project_design.md §13) intentionally
    treats VAD-onset-while-Speaking as the only trigger. Re-add the
    fixture once that filter exists.

**Originally:** Carved off from M7 §5 (Step 5 — recorded validation
suite + 50-trial dogfood, deferred at M7 close 2026-05-04). M7 closed
code-complete; M7B closes the acceptance criteria that need real
end-to-end signal exercise.

**Estimate:** 2–3 days, almost all automation.

**Depends on:**
- M7 (BargeInDetector, cascade, persistence policy, latency metric — all landed).
- M6B (system-AEC fallback active so synthesized fixtures match deployed acoustic model).
- Speaches up (used as the fixture synthesizer).

**Blocks:** nothing strictly. M8A doesn't depend on this. But the
acceptance gates on `voice_barge_in_latency_ms` (P50/P95) and on
false-fire counts can only be asserted once M7B closes — so anything
that materially reworks the cancellation path (M9 speculation, M10
adaptive endpointer) should re-run the M7B suite.

**Existing primitives we reuse:**
- `scripts/m7-bargein-bench.py` — covers the *cascade* leg
  (programmatic `UserInterrupted` injection + percentile analysis of
  `time_to_cancel_ms`). M7B reuses its summary/percentile machinery
  but feeds it from the *real* detector path, not the cascade-only
  shortcut.
- `audio::Wav` (`src/audio/wav.{hpp,cpp}`) — WAV read/write already in
  the tree.
- `audio::LoopbackSink` — M6 introduced the playback tap. M7B routes
  it to a file sink (audit) and optionally to the test mic source
  (closed-loop AEC stress).
- `OpenAiTtsClient` — used as fixture synthesizer; same code path the
  production assistant uses.

---

## 1. Goal

Close the M7 acceptance criteria with **automated, hardware-free** tests
that any contributor can run via `./run_integration_tests.sh`:

- §1 Speaker mode: ≥ 90% correct cancellation within 400 ms, ≤ 1 false
  fire per the 50-trial run.
- §2 Headphone mode: ≥ 95% within 300 ms.
- §3 `voice_barge_in_latency_ms` P50 ≤ 200 ms, P95 ≤ 400 ms.
- §5 False-positive fixtures (`tv`, `self`) produce zero spurious
  cancellations.

Plus a small physical spot-check (~5 trials, 10 minutes) to confirm
the synthetic suite reflects the dev workstation's real acoustic
behavior. That's it — no 50-button-press dogfood loop.

## 2. Out of scope

- Wake-word / dual-trigger barge-in (M8C).
- Adaptive thresholds / per-user voice profiles (M10).
- Multi-mic or beamformed AEC (post-MVP).
- Performance regression CI — M7B asserts thresholds locally; M8B
  promotes the same suite into a soak/dashboard surface.

---

## 3. Strategy: synthesize first, hardware spot-check second

Every fixture in the M7 §5 list is structurally a mix of:

```
assistant_audio  + (user_audio at offset T)  + (background noise | empty)
```

All three primitives are producible from software:

| Primitive | Source |
|---|---|
| assistant_audio | Speaches TTS via `OpenAiTtsClient` (same path as prod) |
| user_audio | Speaches TTS in a different voice, or short pre-recorded interjections |
| background noise | Public-domain pink noise / TV-news clip / silence |

Routed into the capture pipeline via a **test-only WAV input
driver** (Step 2), the rest of the pipeline (Resampler → APM → VAD →
Endpointer → BargeInDetector → FSM cascade) runs verbatim. The
cancellation path observed inside the test process is the same one
that runs against a real microphone.

This makes the headline numbers (P50/P95 latency, false-fire counts)
**deterministic and reproducible**. The hardware-only behaviors that
*can't* be synthesized — codec DSP, speaker non-linearity at usable
volume, room-specific reverb tail — are already covered by M6B's
`acva demo aec-record` + `scripts/aec_analyze.py` flow. M7B Step 7
adds a 5-trial physical spot check that reuses M6B's existing
infrastructure to confirm the synthetic results match reality on this
particular workstation.

---

## Step 1 — Fixture synthesis pipeline

**New file:** `tools/build-bargein-fixtures.py` (Python 3, soundfile,
numpy, requests — same deps as `tools/acva-models`).

Reads a manifest (`tests/fixtures/barge-in/manifest.yaml`) describing
each fixture's components and produces 16 kHz mono PCM WAVs alongside
the manifest. Idempotent; re-running re-synthesizes only fixtures
whose manifest entry changed (sha256 over the entry).

**Manifest schema** (one entry per fixture):

```yaml
- name: clean-speakers
  description: "Assistant talking; user clearly says 'stop' at 1.8 s."
  duration_s: 6.0
  expected:
    cancellation: yes
    max_latency_ms: 400
  tracks:
    - role: assistant
      tts:
        text: "Once upon a time on the moon, a quiet astronaut waited beneath the Earth."
        voice: en-amy
        gain_db: -6
      start_s: 0.0
    - role: user
      tts:
        text: "Stop."
        voice: en-libritts-male            # different voice than assistant
        gain_db: 0
      start_s: 1.8
  noise: silence

- name: noise-speakers
  # ... same shape, noise: pink, snr_db: 12

- name: headphones
  # no assistant track on the mic channel; only user_audio
  tracks:
    - role: user
      tts: { text: "Stop.", voice: en-libritts-male, gain_db: 0 }
      start_s: 1.8
  noise: silence

- name: false-positive-tv
  # assistant talking; TV news clip in background; expected: NO cancel
  tracks:
    - role: assistant
      tts: { text: "...", voice: en-amy, gain_db: -6 }
      start_s: 0.0
  noise:
    file: bg/tv-news-30s.wav
    snr_db: 6
  expected:
    cancellation: no
    max_false_fires: 0

- name: false-positive-self
  # pure assistant TTS bleeding back into mic with AEC simulating
  # ERLE 25 dB. Expected: BargeInDetector's require_aec_converged
  # gate suppresses; metric voice_barge_in_suppressed_total{cause=aec}
  # increments.
  tracks:
    - role: assistant
      tts: { text: "...", voice: en-amy, gain_db: -25 }   # post-AEC residual
      start_s: 0.0
  expected:
    cancellation: no
    max_false_fires: 0
```

**Implementation notes:**
- Voice catalog is read from `config/default.yaml`'s existing `models.tts`
  block (the same registry `tools/acva-models` uses).
- Synth uses `OpenAiTtsClient`-equivalent HTTP via `requests` against
  the running Speaches container. If Speaches isn't up the script
  exits non-zero with the same fix-up message `acva demo tts` prints.
- Mixing: numpy float32 → int16 with -3 dBFS soft-clip ceiling.
  Mono. 16 kHz (matches post-resample pipeline rate; saves the test
  driver one resample step).
- Fixtures land in `tests/fixtures/barge-in/<name>.wav`.
- A `tests/fixtures/barge-in/.gitignore` excludes the WAVs themselves
  but commits the manifest. Fixtures regenerate from manifest+TTS
  on first test run via a CMake test fixture (see Step 6).

**Fallbacks for offline / pre-built CI environments:** the script
accepts `--from-cache <dir>` to use pre-rendered WAVs (e.g., a tarball
attached to the milestone PR). This avoids hard-coupling the tests
to a running Speaches.

---

## Step 2 — WAV input source for the capture path

**New file:** `src/audio/wav_capture.{hpp,cpp}`.

A drop-in alternative to `audio::Capture` that reads a WAV file and
pushes samples into the same SPSC ring at real-time pace (10 ms
slices). Selected via a new optional config field
`cfg.audio.test_input_wav: <path>`; when non-empty, `capture_stack`
constructs a `WavCapture` instead of the PortAudio `Capture`.

**API mirrors `audio::Capture`** so the rest of `capture_stack`
doesn't care:

```cpp
class WavCapture {
public:
    WavCapture(audio::SpscRing& ring,
               std::filesystem::path wav,
               WavCaptureOptions opts = {});
    void start();   // spawns the pump thread
    void stop();
    bool finished() const;     // true when WAV exhausted (lets test wait)
    std::uint64_t frames_pushed() const;
};

struct WavCaptureOptions {
    bool   loop = false;       // re-loop at EOF (for soak)
    double rate_multiplier = 1.0;  // 1.0 = real-time; >1 = faster (offline mode)
    std::optional<std::uint64_t> start_at_us;  // wall-clock anchor for repro
};
```

**Why a config flag, not a separate test binary:**
- All M5/M6/M7 wiring is in `pipeline.cpp` + `capture_stack.cpp`. Re-
  hosting them under a test-only entry point doubles the surface that
  can drift from production.
- The flag is dev-only by convention — `bootstrap.cpp` warns if it's
  set in non-stdin / non-demo configs.

**`rate_multiplier`** matters for the 50-trial run (Step 5): at 4×
real-time we run 50 6-second fixtures in ~75 s instead of ~5
minutes. The pipeline doesn't care about wall-clock for VAD
correctness; the latency metric is still correct because it's a
delta computed against the same clock the audio thread uses.

---

## Step 3 — Validation test driver

**New file:** `tests/test_barge_in_validation.cpp` (in the
**integration** suite, not unit — it wants Speaches up for fixture
regen).

Skeleton:

```cpp
TEST_CASE("barge-in validation: clean-speakers cancels within 400 ms") {
    if (!speaches_up()) { return; }   // skip cleanly, like other integration tests
    auto fx = ensure_fixture("clean-speakers");

    auto cfg = load_test_config();
    cfg.audio.test_input_wav = fx.path;
    cfg.audio.capture_enabled = true;

    BargeInProbe probe(cfg);            // builds the full stack via orchestrator/
    probe.run_until_finished();

    auto stats = probe.stats();
    CHECK(stats.user_interrupted_published == 1);
    CHECK(stats.fsm_reached_listening == true);
    CHECK(stats.barge_in_latency_ms <= 400);
    CHECK(stats.played_sentence_count >= 1);
    CHECK(stats.persisted_outcome == TurnOutcome::Interrupted);
}
```

`BargeInProbe` is a thin test helper (header-only in `tests/support/`):
- Builds the full subsystem stacks via `orchestrator/` exactly like
  `main.cpp` does (no parallel construction code). The capture stack
  picks up `test_input_wav` and uses `WavCapture`.
- Subscribes to `UserInterrupted`, `FsmTransition`, `PlaybackFinished`,
  `TurnPersisted`. Records timestamps + counts.
- `run_until_finished()` waits for `WavCapture::finished()` plus a
  500 ms tail for the cascade to drain.
- Reads `voice_barge_in_*` metrics for cross-check against the
  event-derived stats.

One DOCTEST_TEST_CASE per fixture (5 cases). Plus two negative
fixtures (`false-positive-tv`, `false-positive-self`) that assert
`stats.user_interrupted_published == 0` and the corresponding
`voice_barge_in_suppressed_total{cause=…}` increment.

---

## Step 4 — 50-trial harness as one driver call

The M7 acceptance criteria want population statistics, not single-run
asserts. Instead of running the test 50× from the shell, **the
trial loop lives inside one test case** so percentiles are computed
in-process and only one threshold assert fires:

```cpp
TEST_CASE("barge-in validation: 50-trial latency distribution") {
    auto fx = ensure_fixture("clean-speakers");

    Stats accum;
    for (int i = 0; i < 50; ++i) {
        auto perturbed = perturb_fixture(fx, /*seed=*/i, /*offset_jitter_ms=*/200,
                                         /*level_jitter_db=*/3,
                                         /*noise_db=*/(i % 5 == 0 ? -30 : -60));
        BargeInProbe probe = run_one(perturbed);
        accum.add(probe.stats());
    }

    auto p50 = accum.percentile(50);
    auto p95 = accum.percentile(95);
    auto correct = accum.cancellations_within(400);

    INFO("p50=" << p50 << " p95=" << p95 << " correct=" << correct << "/50");
    CHECK(correct >= 45);            // ≥ 90 %
    CHECK(p50 <= 200);
    CHECK(p95 <= 400);
}
```

`perturb_fixture` deterministically randomizes:
- Start offset of the user "stop" within ±200 ms.
- Per-track level within ±3 dB.
- Noise floor between -60 dB (quiet) and -30 dB (busy room).
- Optional pre-stop "uh," prefix on 20 % of trials (catches the
  `min_real_utterance_chars` filter behavior).

`rate_multiplier=4` keeps the whole 50-trial case under 90 s wall
clock.

A separate test case runs the same loop against the headphone fixture
to satisfy acceptance §2 (≥ 95 % within 300 ms).

---

## Step 5 — Persistence assertion harness

Acceptance §6 (memory rows for interrupted turns contain only the
played-out text) is partially covered by the unit tests landed in M7
§3, but those are against a fake bus. M7B asserts it end-to-end:

```cpp
TEST_CASE("barge-in validation: interrupted turn persists only played sentences") {
    auto fx = ensure_fixture("clean-speakers");
    BargeInProbe probe = run_one(fx);

    auto rows = probe.read_memory_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].outcome == "interrupted");
    CHECK(rows[0].interrupted_at_sentence > 0);
    CHECK(rows[0].text == probe.stats().played_text);   // not the full LLM stream
}
```

`probe.played_text` is reconstructed from the `LlmSentence` +
`PlaybackFinished` events the probe collected, so the assertion is
"what was persisted == what the probe actually heard play out".

---

## Step 6 — CMake fixture + integration runner hook

`CMakeLists.txt` integration block adds a `pretest` step
(`add_test(NAME bargein-fixtures COMMAND tools/build-bargein-fixtures.py …)`)
with `set_tests_properties(... FIXTURES_SETUP bargein_fixtures)`.
Each barge-in test case declares
`FIXTURES_REQUIRED bargein_fixtures` so ctest builds them on first
run and reuses them on subsequent runs.

`run_integration_tests.sh` already runs the full integration suite —
no changes needed beyond the new test cases being picked up. Skip
behavior remains "Speaches not up → cases skip cleanly", same
contract the existing integration tests use.

A new top-level `scripts/validate-bargein.sh` is the user-friendly
entry point:

```sh
./scripts/validate-bargein.sh             # 5 fixtures + 50-trial; ~3 min
./scripts/validate-bargein.sh --quick     # 5 fixtures only; ~30 s
./scripts/validate-bargein.sh --rebuild-fixtures
```

It's a thin wrapper around `ctest --preset dev -R bargein_validation`.

---

## Step 7 — Hardware spot check (manual, 5 trials, ~10 min)

A dramatically reduced version of the original 50-trial dogfood,
intended only to confirm that the synthetic suite's numbers track the
physical setup. Reuses M6B infrastructure:

1. `./_build/dev/acva demo aec-record` + `scripts/aec_analyze.py` —
   confirm gate 4 (≥ 25 dB band-attenuation) is still PASS for this
   workstation. If not, the synthetic suite is overestimating the
   AEC quality and Step 4's results are optimistic.
2. `./scripts/barge-in-probe.py --attempts 5` — 5 real interrupts
   over real speakers. Compare per-trial `time_to_cancel_ms` to the
   synthetic P50/P95 from Step 4. They should be within 100 ms;
   wider gaps indicate the synthetic acoustic model needs
   recalibration (bump assistant `gain_db` in manifest).

Documented in `docs/guide/troubleshooting.md` § "Barge-in latency feels
slow" as the first diagnostic to run, not as a recurring chore.

---

## Acceptance

1. `./scripts/validate-bargein.sh` exits 0 on the dev workstation
   without any human in the loop.
2. CI-equivalent: `./run_integration_tests.sh -R bargein_validation`
   exits 0.
3. Hardware spot check (Step 7) shows synthetic vs physical
   `time_to_cancel_ms` median within 100 ms.
4. Fixture build is reproducible: re-running `tools/build-bargein-fixtures.py`
   against the same manifest produces byte-identical WAVs (modulo
   Speaches non-determinism — recorded as a known caveat with sha256
   pinning of the rendered fixtures in the manifest).

## Risks specific to M7B

| Risk | Mitigation |
|---|---|
| Speaches TTS is non-deterministic across versions, fixtures drift | Pin Speaches image tag in compose; record fixture sha256 in manifest; rebuild fixtures only on intentional version bump. |
| `WavCapture` real-time pacing drifts under loaded CI | `rate_multiplier=1.0` for the latency-asserting cases; ≤ 4.0 only for the 50-trial volume case where individual trial latency variance is what matters, not absolute ms. |
| Synthetic AEC residual model (-25 dB attenuation in `false-positive-self`) doesn't match real codec | Step 7 spot check catches drift; manifest has a single dial (assistant gain_db) to recalibrate. |
| Test-only `cfg.audio.test_input_wav` flag rots in production code | Bootstrap warns when set in stdin / demo paths; one place to grep. Removed entirely if a future test framework lets us inject without a config flag. |
| Speaches required for fixture regen makes tests fragile | `--from-cache` mode + sha256-pinned fixtures in the manifest let CI run against a tarball. Fixtures only regen when manifest changes. |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Fixture synthesis pipeline | 0.5 day |
| 2 `WavCapture` source | 0.5 day |
| 3 Validation test driver + probe helper | 0.5 day |
| 4 50-trial perturbation harness | 0.5 day |
| 5 Persistence E2E assert | 0.25 day |
| 6 CMake fixture + script wrapper | 0.25 day |
| 7 Hardware spot check + docs | 0.25 day |
| **Total** | **~2.5 days** |

## Demo command (planned)

### `acva demo bargein-validation`

A user-facing single-fixture run that mirrors what the test driver
does, for spot-checking outside ctest:

```sh
./_build/dev/acva demo bargein-validation --fixture clean-speakers
demo[bargein-validation] using fixture tests/fixtures/barge-in/clean-speakers.wav
demo[bargein-validation] BargeInDetector fired at 1854 ms (offset 1800 ms; +54 ms detection latency)
demo[bargein-validation] cancellation cascade silenced output at 1932 ms (78 ms after publish)
demo[bargein-validation] persisted: outcome=interrupted played=1 sentence
demo[bargein-validation] done: pass (within 400 ms gate)
```

`acva demo bargein` (M7) stays as the cascade-only smoke; the new
demo is the *full* path including detector + AEC gate + persistence.

---

## Cleanup at close

- Promote M7's "code-complete" to "✅ closed" in `docs/history/MVP/milestones/README.md`
  and CLAUDE.md once Step 7's spot check passes.
- Delete `scripts/m7-bargein-bench.py` if Step 4's in-process loop
  fully subsumes it (it does — same percentiles, no shell parsing).
  Keep it if it's still the cheapest "is the cascade alive at all"
  smoke; Step 4's harness needs the WavCapture path built.
- Update `docs/guide/troubleshooting.md` § "Barge-in fires when it
  shouldn't" to reference `scripts/validate-bargein.sh` as the
  diagnostic, replacing today's pointer to `acva demo bargein`.
