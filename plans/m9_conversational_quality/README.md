# M9 — Conversational Quality

**Estimate:** ~3 months (~12 weeks).  
**Status:** next.  
**Depends on:** MVP closed (M0–M8C ✅). For P9.1, an STT backend that emits partial transcripts — Speaches as of 2026-05-02 does not (see [`p9.1_streaming_partials_source/`](p9.1_streaming_partials_source/)).  
**Blocks:** M10 (Web UI shows live FSM + dialogue trace; cleaner output makes the trace more useful), M11 (GBNF tool-call grammar reuses M9.P9.4 grammar plumbing).

## Goal

Make acva feel natural in real conversations. The MVP works
end-to-end — speech in, speech out, barge-in cancels, memory
persists — but four sharp-edge issues remain after dogfood:

1. **Latency is good but not great.** P50 ~1.7 s, P95 ~3.5 s.
   Speculative LLM start was supposed to land in M5 but couldn't
   without partial transcripts. Brings P50 to ~1.4 s on
   stable-prefix utterances.
2. **Endpointing is a fixed timeout.** Mid-thought pauses get
   cut; crisp finishes wait an extra 600 ms for nothing. Adaptive
   endpointing reads the partial transcript itself.
3. **Address detection is missing.** Wake-word ships off
   ([open_questions §L8 in MVP archive](../../docs/history/MVP/open_questions.md)) because
   it's the wrong primitive for always-listening mode. The right
   primitive — "is this addressed to me?" — is the M9 deliverable.
4. **LLM output is too verbose / formal / stuffed with filler.**
   "Of course! How can I help you?" before every answer; chat
   templates picked up from training data; generation-stop
   tokens not consistently honored. GBNF for explicit
   {think/wait/speak/ignore} actions makes the FSM transitions
   explicit at the LLM level.

Plus three bugs that surfaced during M8C dogfood
([`plans/bugs/`](../bugs/)) close in P9.0 as a one-week warm-up.

## Phases

| ID    | Title                                       | Duration | Folder |
|-------|---------------------------------------------|----------|--------|
| P9.0  | MVP bug sweep                               | 1 wk     | [`p9.0_mvp_bug_sweep/`](p9.0_mvp_bug_sweep/) |
| P9.1  | Streaming partials source                   | 3-4 wk   | [`p9.1_streaming_partials_source/`](p9.1_streaming_partials_source/) |
| P9.2  | Speculative LLM start                       | 2-3 wk   | [`p9.2_speculative_llm_start/`](p9.2_speculative_llm_start/) |
| P9.3  | Adaptive endpointer + address detection     | 2-3 wk   | [`p9.3_adaptive_endpointer_address_detection/`](p9.3_adaptive_endpointer_address_detection/) |
| P9.4  | LLM output hygiene                          | 4-6 wk   | [`p9.4_llm_output_hygiene/`](p9.4_llm_output_hygiene/) |
| P9.5  | TTS expression (Q10 expanded)               | 4-6 wk   | [`p9.5_tts_expression/`](p9.5_tts_expression/) |

Phase order is not strict beyond P9.0 first and P9.2 after P9.1
(speculation needs partials). P9.3, P9.4, and P9.5 are
independent and can interleave.

P9.4 grew during planning (initial scope of three stages → twelve)
to absorb every dialog-quality idea from `plans/ideas.md`. P9.5
is the expansion of parked Q10 (TTS expression / SSML / RVC /
HiFi-GAN / mood / sound filters / preset comparison).

## Out of scope

- Full-duplex perfection (the user and assistant both speaking
  at once productively) — separate problem, post-M9.
- Speaker diarization / "who is speaking" — partly addressed in
  M12 (voice biometric ID).
- Multi-turn context modelling beyond the existing summarizer
  rollup — dialogue policy concern, not a perception one.
- The wake-word baseline itself — stays as M8C ships it. M9
  layers semantic signals on top, doesn't replace.

## Acceptance

1. **All three M8C-era bugs closed** ([`plans/bugs/*.md`](../bugs/))
   or explicitly downgraded with a write-up in
   [`open_questions.md`](../open_questions.md).
2. **Speculation saves measurable latency.** P50 first-token-ready
   on stable-prefix utterances drops by ≥ 200 ms vs current M5
   baseline (logged in
   `voice_speculation_first_token_ready_ms` histogram).
3. **Adaptive endpointer reduces both error modes.** Mid-thought
   cuts (utterances of < 4 words ending in a hesitation marker)
   drop ≥ 70 %; crisp finishes (utterances ending in terminal
   punctuation per Whisper) end ≤ 200 ms after the partial vs
   600 ms today.
4. **Address detection blocks side-talk.** With acva listening
   in a multi-occupant room, ≥ 90 % of non-addressed utterances
   are gated; addressed-to-me false-negative rate ≤ 5 % across
   the validation fixture set.
5. **LLM output hygiene measured.** Filler-rate (per the GBNF
   action grammar — utterances that should `wait` but `speak`)
   drops ≥ 50 % vs M8C baseline on the soak-driver prompt set.
6. **Soak run still green.** 4-hour soak with M9 features on
   passes the same four gates as M8B (no_crashes, rss_growth,
   queue_depth_stable, service_restarts_ok).

## Risks

| Risk | Mitigation |
|---|---|
| P9.1 strategy drag — picking the right partials source is the long pole | Time-box the decision to 1 week (S9.1.1); fall back to "side-car streaming Whisper" if upstream Speaches PR doesn't land. |
| Address detection is M10-conversational-UX work conceptually but uses a wake-word-style classifier; quality varies wildly with training data | Ship a dumb-but-working baseline first (heuristic — utterance starts with assistant name OR is < 4 words AND ends with question mark); learn from production traces; upgrade to a classifier in P9.3 second iteration if signal warrants. |
| GBNF over-constrains the LLM and makes it sound robotic | Make the grammar a soft preference (sample with grammar applied at higher temperature for "natural" speech, fall back to free generation on `speak` action); A/B with operators. |
| Speculation reconcile bugs (cancelled-and-restarted runs) leak state between turns | Reuse the M7 cancellation-cascade tests as the regression seam — every speculative cancel is a barge-in-style cancel. |

## What lands in the codebase

New code:
- `event::PartialTranscript` event type
- `dialogue::SpeculationGate` policy class
- `dialogue::FSM::SpeculativeThinking` concurrent sub-state
- `audio::AdaptiveEndpointer` (replaces or augments
  `audio::Endpointer`)
- `dialogue::AddressDetector` with at least a heuristic
  implementation
- `llm::Grammar` (GBNF) with the {think/wait/speak/ignore} action
  schema
- `voice_speculation_*`, `voice_address_detection_*`,
  `voice_endpointer_adaptive_*` metrics

Modified:
- `stt::RealtimeSttClient` — emits partials when the backend
  supports them
- `dialogue::Manager` — speculation reconcile, address-detector
  gate, GBNF action handling
- `src/dialogue/fsm.{hpp,cpp}` — new sub-state + transitions

Tests: ~30 new unit cases (speculation gate policy table,
address-detector heuristic edges, adaptive endpointer
transitions, GBNF parser); 2 new integration cases (live
Speaches partials probe, soak driver with M9 features).

## See also

- [`plans/open_questions.md`](../open_questions.md) Q1
  (modularity ordering — cross-milestone) and
  [`plans/m10_web_ui/open_questions.md`](../m10_web_ui/open_questions.md)
  Q3 (config-in-DB) — these don't block M9 but get decided
  around its completion.
- [`docs/history/MVP/milestones/m5_streaming_stt.md`](../../docs/history/MVP/milestones/m5_streaming_stt.md) —
  why partials were lifted out of M5.
- [`docs/history/MVP/open_questions.md`](../../docs/history/MVP/open_questions.md) §L6
  (M5 partials → M9) and §L8 (wake-word vs address detection).
