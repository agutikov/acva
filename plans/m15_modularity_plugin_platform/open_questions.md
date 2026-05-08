# M15 — Open Questions

Questions specific to the Modularity / Plugin Platform milestone.
Cross-milestone and non-milestone questions live in
[`plans/open_questions.md`](../open_questions.md).

The biggest **cross-milestone** question that touches M15 is
[Q1 (M15 ordering — modularity first or last?)](../open_questions.md)
in the global file — it's not split out here because it's
fundamentally about where M15 sits in the roadmap, not about
internal M15 design.

---

## Q9. Plugin ABI stability

**Status:** open  
**Default:** TBD at M15.P15.1.

If we ship a stable C ABI, breaking changes require a major
version bump. The MVP-era pace would have made that painful.
Options: pre-1.0 unstable (current), strict semver, or
"major.minor.patch where minor breaks ABI" (FFmpeg-style).

## Q26. Modularity unit selection

**Status:** open — investigated in [`p15.1_module_abi/s15.1.2_module_unit_selection.md`](p15.1_module_abi/s15.1.2_module_unit_selection.md).

`ideas.md` lines 51-62 ask: which subsystems become modules?
The default M15.P15.2 extraction list (audio backend, memory
store, TTS engine) is a guess. Candidates to consider:

- audio backend — yes (default)
- memory store — yes (default)
- TTS engine — yes (default)
- STT engine — likely yes
- LLM client — likely yes
- AEC (APM) — maybe
- VAD / endpointer — maybe
- PromptBuilder — maybe
- FSM — likely no (too coupled to Manager)
- DialogueManager — no (orchestration kernel)
- Address detector — yes (M9.P9.3 deliverable, fits a
  classifier-as-module shape cleanly)

**Decide by:** S15.1.2 close. Reshapes M15.P15.2 extraction
list.

Discipline (`ideas.md` line 62): "do not mix up modularity
and configurability". A module exists when operators want a
*different binary* for that role; everything else (system
prompts, voices, personalities) stays config.
