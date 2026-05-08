# M12 — Multi-User & Personality

**Estimate:** ~3 months (~12 weeks).  
**Status:** planned (after M11).  
**Depends on:** M11 KG + per-session memory; M9 address detection (gives a foundation for "is the user talking" filtering).  
**Blocks:** none directly; M11 tools become per-user-aware.

## Goal

Two related capabilities:

1. **Voice biometric ID** — recognize who's talking from short
   audio clips. Per-user enrollment + identification at utterance
   time.
2. **Per-user memory & sessions** — sessions / turns / facts
   scope by user; "remember main user by voice" UX; multi-user
   conversation handling (interleaved STT, switch detection).
3. **Personality cloning + drift policy** — clone a personality
   from session traces, drift-bound to keep clones recognizable.

## Phases

| ID    | Title                              | Duration | Folder |
|-------|------------------------------------|----------|--------|
| P12.1 | Voice biometric ID                 | 3-4 wk   | [`p12.1_voice_biometric_id/`](p12.1_voice_biometric_id/) |
| P12.2 | Per-user memory + attribution      | 2-3 wk   | [`p12.2_per_user_memory_attribution/`](p12.2_per_user_memory_attribution/) |
| P12.3 | Multi-user conversation            | 3-4 wk   | [`p12.3_multiuser_conversation/`](p12.3_multiuser_conversation/) |
| P12.4 | Personality cloning + drift        | 2-3 wk   | [`p12.4_personality_cloning_drift/`](p12.4_personality_cloning_drift/) |
| P12.5 | Multi-context / multi-model (Q12)  | 3-5 wk   | [`p12.5_multi_context_multi_model/`](p12.5_multi_context_multi_model/) |

P12.5 is the expansion of parked Q12 — investigation-heavy phase
that decides whether acva runs concurrent contexts / models, and
ships the wiring if the answer is yes.

## Out of scope

- Speaker diarization on a single mixed stream (multiple
  speakers in one waveform that needs to be separated). M12
  ships speaker switching ("who's the most likely speaker for
  this utterance"); per-channel diarization is harder.
- Cross-device user identity. Per-host only — the user's voice
  enrollment doesn't sync across machines.

## Acceptance

1. **Voice ID** — speaker identification accuracy ≥ 90% on
   enrolled users with ≥ 30 s of training audio per user.
2. **Per-user attribution** — sessions / turns / facts
   queryable by `user_id`. Memory queries scope by default
   to the active speaker.
3. **Multi-user conversation** — alternating speakers in a
   conversation get distinct `user_id` attribution; FSM
   re-enters cleanly on switches.
4. **Personality cloning** — operator can clone the
   `consultant` personality from their own session traces
   and the resulting clone is measurably similar (cosine
   similarity ≥ 0.8 on a held-out style metric) without
   collapsing into the original training data.

## See also

- [`open_questions.md`](open_questions.md) Q8 (drift
  policy).
