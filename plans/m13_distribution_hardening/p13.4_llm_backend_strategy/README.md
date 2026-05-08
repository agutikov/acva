# P13.4 — LLM backend strategy (llama.cpp vs ollama)

**Estimate:** 2-3 weeks (investigation-heavy).
**Status:** planned — opens new question Q21.
**Depends on:** M8A model-controller (current llama.cpp-only
implementation is the baseline this phase compares against).
**Blocks:** M11.P11.1 tool framework (sandbox model partly
depends on whether we're in-process with the LLM); M14.P14.4
all-in-one packaging.

## Goal

Decide whether acva should keep llama.cpp as the only LLM
backend or add ollama (or a different orchestration layer)
as either an alternative or a replacement.

`ideas.md` line 118 raises this casually but the consequences
fan out across model-controller scope, packaging, hot-swap
ergonomics, and pillar #1.

Output: a written decision in Q21 + either an implementation
stage or an explicit "not now, here's the trigger" close-out.

## Stages

| ID     | Title                                | Shape           | Folder |
|--------|--------------------------------------|-----------------|--------|
| S13.4.1 | Comparison matrix                    | investigation   | [`s13.4.1_llama_cpp_vs_ollama_comparison.md`](s13.4.1_llama_cpp_vs_ollama_comparison.md) |
| S13.4.2 | Model-controller scope under ollama  | investigation   | [`s13.4.2_model_controller_scope.md`](s13.4.2_model_controller_scope.md) |

## Out of scope

- Replacing llama.cpp with vLLM / TGI / TabbyAPI / etc. The
  ollama comparison is the narrow scope; if a broader survey
  is warranted, it gets its own phase.
- Replacing Speaches. STT/TTS backend choice is not part of
  this phase.

## Acceptance

1. Q21 resolved with a decision + reasoning + revisit
   condition.
2. If decision is "add ollama as alt-backend", a stage is
   spawned under M11 or M13 to do the integration.
3. If decision is "keep llama.cpp only", model-controller
   scope is documented and S13.4.2 closes with a paragraph.

## See also

- `plans/open_questions.md` Q21 (added with this phase).
- Pillar #1 in `CLAUDE.md` — process-isolation constraint
  any decision has to respect.
- `docs/history/MVP/milestones/m1_llm_memory.md` — original
  rationale for picking llama.cpp.
