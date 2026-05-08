# M12 — Open Questions

Questions specific to the Multi-User & Personality milestone.
Cross-milestone and non-milestone questions live in
[`plans/open_questions.md`](../open_questions.md).

---

## Q8. Personality drift policy

**Status:** open
**Default:** TBD at M12.P12.4 design.

Cloning a personality from session traces will shift over time.
Some drift is desirable (the clone reflects how the user actually
talks); too much makes it unrecognizable. Need a measurable
similarity metric and an explicit clamp.

## Q23. Multi-context vs multi-model strategy

**Status:** open — investigated in [`p12.5_multi_context_multi_model/`](p12.5_multi_context_multi_model/).

How does acva support concurrent personalities / users / NPCs?

- **A)** Single context — current. No concurrency, swap via
  warm-restart.
- **B)** Multi-context with one llama (slot pool). Cheap; bound
  by ctx-size / N.
- **C)** Multi-model runtime. Two `llama-server` processes
  managed by an extended model-controller. Most flexible;
  tightest VRAM.
- **D)** Both — operator-selected per use case.

**Decide by:** P12.5 close (combined output of S12.5.1 +
S12.5.2 + S12.5.4).

## Q24. Interleaved STT solution

**Status:** open — investigated in [`p12.3_multiuser_conversation/s12.3.2_interleaved_stt_solution.md`](p12.3_multiuser_conversation/s12.3.2_interleaved_stt_solution.md).

When two users alternate inside one utterance window, how does
acva attribute correctly?

- **A)** Single stream + speaker-switch detection on partials.
- **B)** Per-speaker streaming sessions (fork on switch).
- **C)** Diarization + post-segment.

**Decide by:** S12.3.2 close.

## Q25. Memory ↔ personality cardinality

**Status:** open — investigated in [`p12.4_personality_cloning_drift/s12.4.2_memory_personality_n_n.md`](p12.4_personality_cloning_drift/s12.4.2_memory_personality_n_n.md).

How are facts scoped relative to personalities?

- **A)** No link — every personality reads every fact.
- **B)** Strict per-personality scope — facts tagged with one
  personality, no cross-read.
- **C)** N-N optional — facts have an explicit `personality_set`
  (NULL = universal).

**Decide by:** S12.4.2 close.
