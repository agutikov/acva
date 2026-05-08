# P12.5 — Multi-context / multi-model runtime (Q12 expanded)

**Estimate:** 3-5 weeks (investigation-heavy; ship-or-defer
decision is the deliverable).
**Status:** planned — replaces parked `plans/open_questions.md` Q12.
**Depends on:** M8A model-controller (load/unload primitive);
M12.P12.1 voice biometric ID and P12.2 per-user attribution
make per-user contexts coherent.
**Blocks:** anything that wants concurrent personalities
(post-M12 NPC scenarios — see `plans/scenarios/game_npc_character_design.md`).

## Goal

Decide whether acva's runtime can hold more than one
conversation context simultaneously, and whether a single
acva process can route to more than one LLM. Both are
runtime architectural changes that touch GPU budget,
model-controller scope, and pillar #1 (process isolation).

Q12 parked the question; this phase produces the answer.

## Stages

| ID     | Title                              | Shape             | Folder |
|--------|------------------------------------|-------------------|--------|
| S12.5.1 | Multi-context with one llama       | investigation     | [`s12.5.1_multi_context_single_model.md`](s12.5.1_multi_context_single_model.md) |
| S12.5.2 | Multi-model runtime                | investigation     | [`s12.5.2_multi_model_runtime.md`](s12.5.2_multi_model_runtime.md) |
| S12.5.3 | Persona-per-context wiring         | impl (gated)      | [`s12.5.3_persona_per_context.md`](s12.5.3_persona_per_context.md) |
| S12.5.4 | GPU budget under concurrency       | investigation     | [`s12.5.4_gpu_budget.md`](s12.5.4_gpu_budget.md) |

## Out of scope

- Cross-process orchestration of two acva instances. This phase
  is about one acva managing concurrent contexts; running two
  acvas side-by-side is an operator concern.
- Speculative-decoding multi-model setups (small draft + large
  verify). That's a latency optimization, separate from
  multi-personality.

## Acceptance

1. Q23 (multi-context vs multi-model strategy) resolved with a
   decision + the supporting numbers (VRAM, latency, ergonomic
   cost).
2. If "ship one context-multiplexing track" is the decision, S12.5.3
   lands the wiring; otherwise S12.5.3 closes with a write-up
   pointing at why and what would change the answer.

## See also

- `plans/open_questions.md` Q12 (parent — replaced by this phase).
- M8A model-controller code path — the load/unload primitive
  this phase builds on.
- `project_gpu_cdi_and_vram.md` memory note — the 8 GB budget
  this phase has to live under.
