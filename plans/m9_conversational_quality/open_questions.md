# M9 — Open Questions

Questions specific to the Conversational Quality milestone.
Cross-milestone and non-milestone questions live in
[`plans/open_questions.md`](../open_questions.md).

---

## Q15. XML / JSON output channel

**Status:** open — investigated in [`p9.4_llm_output_hygiene/s9.4.7_xml_json_output_channel.md`](p9.4_llm_output_hygiene/s9.4.7_xml_json_output_channel.md).

Three options for how the LLM emits structured *data* (distinct
from natural-language speech):

- **A)** Extend the GBNF action grammar with `data(format,
  payload)`.
- **B)** Use chat-completions' `tool_calls` array as a separate
  channel.
- **C)** Recognise markdown code-fence blocks at parse time.

**Decide by:** S9.4.7. Affects M11.P11.2 (tool-call output)
which inherits the chosen mechanism.

## Q16. Reasoning model support

**Status:** open — investigated in [`p9.4_llm_output_hygiene/s9.4.10_reasoning_models.md`](p9.4_llm_output_hygiene/s9.4.10_reasoning_models.md).

Reasoning models (DeepSeek-R1, QwQ, o1-style) emit
`<think>...</think>` blocks before the answer. Routing options:

- **A)** Pre-strip at LlmClient.
- **B)** Route to `event::LlmThought` (S9.4.8 reuse).
- **C)** Auto-fold into GBNF `think(...)` action.

**Decide by:** S9.4.10. Pairs with Q20.

## Q17. 2-stage generation (draft + refine)

**Status:** open — investigated in [`p9.4_llm_output_hygiene/s9.4.9_two_stage_generation.md`](p9.4_llm_output_hygiene/s9.4.9_two_stage_generation.md).

Should we add an opt-in refine pass after each generation? Three
options:

- **A)** Skip — single-pass only.
- **B)** Same-model two-stage as opt-in.
- **C)** Two-model — gated on M12.P12.5 multi-model.

**Decide by:** S9.4.9, with measured latency-vs-quality data.

## Q18. Voice silence command grammar

**Status:** open — investigated in [`p9.4_llm_output_hygiene/s9.4.12_voice_silence_command.md`](p9.4_llm_output_hygiene/s9.4.12_voice_silence_command.md).

How does the user voice-trigger mute? Three options:

- **A)** Lightweight keyword spotter for fixed phrases.
- **B)** STT + LLM intent classification.
- **C)** GBNF `silence` action — model recognises intent.

**Decide by:** S9.4.12. Unmute path documented as part of the
same stage.

## Q19. GBNF action superset

**Status:** open — opened by [`p9.4_llm_output_hygiene/s9.4.3_gbnf_actions.md`](p9.4_llm_output_hygiene/s9.4.3_gbnf_actions.md), extended by S9.5.6.

Current grammar: `think | wait | speak | ignore`. Candidates to
add:

- `tool_call(name, args)` — M11.P11.2.
- `data(format, payload)` — Q15 option A.
- `mood(name)` — S9.5.6 expression hint.
- `defer(ms)` — explicit "let me think for N ms" timer.
- `clarify(question)` — pose a back-question instead of
  answering.
- `state_update(key, value)` — S12.2.2 person-state update.
- `silence()` — Q18 option C.

**Decide by:** rolling — each new stage that wants an action
files a request here; the action grammar is bumped at M9 close.

## Q20. Thinking-channel routing (verbal thinking)

**Status:** open — investigated in [`p9.4_llm_output_hygiene/s9.4.8_thinking_vs_speaking.md`](p9.4_llm_output_hygiene/s9.4.8_thinking_vs_speaking.md).

Once `think(...)` action and `event::LlmThought` exist, how is
thinking voiced (when verbal thinking is enabled)?

- **A)** Separate parallel TTS request, lower volume / different
  voice. Plays before `speak(...)`.
- **B)** Inline: prepend "Hmm, " or similar, splice before the
  spoken reply. No second TTS run.
- **C)** Operator-only debug mode — never voiced, only logged.

**Decide by:** S9.4.8. Pairs with Q16.

## Conventions

Use the next free `Q<n>` ID across all files (currently
**Q29** is next free). Stages that resolve a new design
question land it here.
