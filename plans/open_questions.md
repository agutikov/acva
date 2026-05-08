# Open Questions — global (cross-milestone + parked)

This file holds questions that span multiple milestones, are
parked indefinitely, or aren't bound to any specific milestone.
**Milestone-scoped questions live in their milestone's
`open_questions.md`** — see the index below.

Pre-MVP open questions (sections A–L) are archived at
[`docs/history/MVP/open_questions.md`](../docs/history/MVP/open_questions.md).

Question IDs are sticky — Q3, Q5–Q9 moved to milestone files but
keep their original numbers there. Don't reuse a closed ID;
allocate the next free.

## Index — where each question lives

| Q   | Title                                       | Lives in |
|-----|---------------------------------------------|----------|
| Q1  | M15 ordering — modularity first, or last?   | here (cross-milestone) |
| Q2  | All-in-one C++ binary                       | here (operationalised by M14.P14.4 S14.4.2) |
| Q3  | Config in DB vs YAML                        | [M10](m10_web_ui/open_questions.md) |
| Q4  | Niche scenarios                             | here (parked; expanded in [`scenarios/`](scenarios/)) |
| Q5  | macOS support seriousness                   | [M14](m14_cross_platform/open_questions.md) |
| Q6  | Tools framework — sandbox model             | [M11](m11_tools_agent_platform/open_questions.md) |
| Q7  | Knowledge graph storage                     | [M11](m11_tools_agent_platform/open_questions.md) |
| Q8  | Personality drift policy                    | [M12](m12_multiuser_personality/open_questions.md) |
| Q9  | Plugin ABI stability                        | [M15](m15_modularity_plugin_platform/open_questions.md) |
| Q10 | TTS expression — SSML, sound filters, sarcasm | here (expanded into phase [M9.P9.5](m9_conversational_quality/p9.5_tts_expression/)) |
| Q11 | Audio interleaving with media players       | here (expanded into phase [M14.P14.5](m14_cross_platform/p14.5_audio_interleaving/)) |
| Q12 | Multi-context / multi-model runtime         | here (expanded into phase [M12.P12.5](m12_multiuser_personality/p12.5_multi_context_multi_model/)) |
| Q13 | Desktop UX wrapper — tray icon / Electron   | here (expanded into phase [M14.P14.6](m14_cross_platform/p14.6_desktop_ux_wrappers/)) |
| Q14 | Convert plans + docs into single-file design | here (operationalised by M13.P13.3 S13.3.2) |
| Q15 | XML/JSON output channel                     | [M9](m9_conversational_quality/open_questions.md) |
| Q16 | Reasoning model support (DeepSeek-R1, …)    | [M9](m9_conversational_quality/open_questions.md) |
| Q17 | 2-stage generation (draft + refine)         | [M9](m9_conversational_quality/open_questions.md) |
| Q18 | Voice silence command grammar               | [M9](m9_conversational_quality/open_questions.md) |
| Q19 | GBNF action superset (beyond {think,wait,speak,ignore}) | [M9](m9_conversational_quality/open_questions.md) |
| Q20 | Thinking-channel routing (verbal thinking)  | [M9](m9_conversational_quality/open_questions.md) |
| Q21 | llama.cpp vs ollama                         | here (cross-milestone, decided in [M13.P13.4](m13_distribution_hardening/p13.4_llm_backend_strategy/)) |
| Q22 | Graph DB choice for KG                      | [M11](m11_tools_agent_platform/open_questions.md) |
| Q23 | Multi-context vs multi-model strategy       | [M12](m12_multiuser_personality/open_questions.md) |
| Q24 | Interleaved STT solution                    | [M12](m12_multiuser_personality/open_questions.md) |
| Q25 | Memory ↔ personality cardinality            | [M12](m12_multiuser_personality/open_questions.md) |
| Q26 | Modularity unit selection                   | [M15](m15_modularity_plugin_platform/open_questions.md) |
| Q27 | Audio interleaving policy                   | [M14](m14_cross_platform/open_questions.md) |
| Q28 | Desktop UX wrapper choice                   | [M14](m14_cross_platform/open_questions.md) |

---

## Q1. M15 ordering — modularity first, or last?

**Status:** open
**Default:** M15 last (current ordering — M9 → M10 → M11 → M12 → M13 → M14 → M15).

The argument for moving M15 earlier (between M9 and M10, say): M11
tools, M12 multi-user, and M14 cross-platform all add subsystems.
If module ABI lands first, those new subsystems land into a stable
plugin platform; if it lands last, they land into the monolith and
get re-extracted later. The argument against: ~2.5 months of pure
architectural work before the next user-visible win, and the
project may never grow large enough to need plugins (premature).

**Decide by:** start of M11 (tool framework needs to know whether
it's a module or a static subsystem).

## Q2. All-in-one C++ binary

**Status:** open — operationalised by [M14.P14.4 S14.4.2](m14_cross_platform/p14.4_cross_platform_packaging/s14.4.2_all_in_one_executable.md).
**Default:** rejected — violates pillar #1 (process isolation for
backends).

The idea (from `plans/ideas.md`): replace separate llama.cpp + Speaches
processes with libllama / libwhisper / libpiper linked into the
acva binary. Pros: single-process install, no Compose, easier
distribution. Cons: loses crash isolation, makes hot model swap
much harder, ties acva's release cadence to upstream library
ABIs.

**Decide by:** M14 cross-platform (S14.4.2 is the resolution
stage). The decision touches M11 (tools could share the same
in-process bus), M14 (packaging shape), and M15 (modules
become the way you'd opt back into separation).

## Q4. Niche scenarios

**Status:** parked indefinitely — each scenario expanded under [`plans/scenarios/`](scenarios/).
**Default:** not in any milestone.

Three ideas that show up in `plans/ideas.md` but don't have an obvious
home: game-NPC character design, low-latency online voice
translation, Telegram on-call assistant. Each has its own stub
file under [`scenarios/`](scenarios/) describing trigger + shape if
picked up.

Each could be a small new milestone (M16+) or a third-party
integration. Revisit only if a real use case lands.

## Q10. TTS expression — SSML, sound filters, sarcasm, RVC, vocoders, …

**Status:** open — **expanded into phase [M9.P9.5](m9_conversational_quality/p9.5_tts_expression/)**.

Originally one parked question covering the entire TTS expression
family. Now broken out into one stage per topic:

- S9.5.1 sound filter chain (cheap DSP)
- S9.5.2 SSML feasibility
- S9.5.3 RVC (Retrieval-based Voice Conversion)
- S9.5.4 neural vocoders (HiFi-GAN, BigVGAN, …)
- S9.5.5 prosody control (rate / pitch / emphasis)
- S9.5.6 artistic / sarcastic / expressive modes
- S9.5.7 style post-processing
- S9.5.8 min vs max preset comparison harness

Decisions per stage land in the stage closure notes. P9.5 closes
when S9.5.8 has data.

## Q11. Audio interleaving with media players

**Status:** open — **expanded into phase [M14.P14.5](m14_cross_platform/p14.5_audio_interleaving/)**.

Per-OS mechanism work (PipeWire, CoreAudio, WASAPI) plus a unified
policy decision (Q27 child question).

## Q12. Multi-context / multi-model runtime

**Status:** open — **expanded into phase [M12.P12.5](m12_multiuser_personality/p12.5_multi_context_multi_model/)**.

Investigation phase: multi-context with one llama, multi-model
runtime, persona-per-context wiring, GPU budget under
concurrency. Resolves Q23.

## Q13. Desktop UX wrapper — tray icon / Electron / Tauri

**Status:** open — **expanded into phase [M14.P14.6](m14_cross_platform/p14.6_desktop_ux_wrappers/)**.

Investigation phase: tray icon per OS, Electron evaluation,
Tauri alternative, quick-menu UX. Resolves Q28.

## Q14. Convert plans + docs into single-file design with memory loading

**Status:** open — operationalised by [M13.P13.3 S13.3.2](m13_distribution_hardening/p13.3_cicd_release_engineering/s13.3.2_design_history_export.md).

From `plans/ideas.md`: collapse `plans/` + `docs/` into a single
"history of acva creation" file, process it, load into
memory + add memory-read instructions to context, so future
agents can answer questions about the project's history.

Niche tooling; useful only if the agent loop benefits from
it. Decide based on observed agent friction.

## Q21. llama.cpp vs ollama

**Status:** open — investigated in [M13.P13.4](m13_distribution_hardening/p13.4_llm_backend_strategy/).
**Default:** keep llama.cpp + M8A model-controller.

Should acva replace llama.cpp + M8A model-controller with ollama
(or add ollama as an alternative backend)? Affects pillar #1
shape, packaging (M13/M14), and M9.P9.4 GBNF support.

Three options:
- **A)** Keep llama.cpp only. Status quo. M8A maintained.
- **B)** Add ollama as alt-backend; both supported.
- **C)** Adopt ollama; delete M8A model-controller.

**Decide by:** M13.P13.4 close. Trigger to revisit if not
shipped: GBNF support gap in ollama narrows, OR M8A maintenance
cost grows.

---

## Conventions

- Add a milestone-scoped question to the relevant
  `<milestone>/open_questions.md`. If unsure or it spans
  multiple milestones, add it here and re-home later.
- Use the next free `Q<n>` ID across all files (sticky IDs
  prevent stale-ref breakage). Next free: **Q29**.
- Resolve in place — keep the choice + the *why* + the
  consequence.
- When in doubt, keep the default behavior in code and log
  the question rather than picking blindly.
- A "parked" question that turns out to be a multi-topic
  investigation should be **expanded into a phase** (see
  Q10/Q11/Q12/Q13 examples). The question stays here as a
  pointer; the stages live under the phase folder.
