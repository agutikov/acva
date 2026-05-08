# M10 — Open Questions

Questions specific to the Web UI milestone. Cross-milestone
and non-milestone questions live in
[`plans/open_questions.md`](../open_questions.md).

---

## Q3. Config in DB vs YAML

**Status:** open  
**Default:** YAML stays canonical; DB is a cache for editor
state.

For M10's Web UI config editor, options are:

- **A)** YAML stays canonical; the editor reads + writes YAML
  directly. Simplest; preserves operator-friendly editing.
- **B)** Config moves to SQLite; YAML becomes import/export only.
  Cleaner concurrent-edit story, transactional changes, but
  operators lose the "edit a file in $EDITOR" workflow.
- **C)** Hybrid — YAML on disk, in-memory canonical, the editor
  applies + persists; merge-conflict resolution if disk + UI
  diverge.

**Decide by:** M10.P10.4. Affects the entire reload story.
