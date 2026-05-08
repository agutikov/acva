# M11 — Open Questions

Questions specific to the Tools & Agent Platform milestone.
Cross-milestone and non-milestone questions live in
[`plans/open_questions.md`](../open_questions.md).

---

## Q6. Tools framework — sandbox model

**Status:** open
**Default:** stub at M11.P11.1 (subprocess with stdio JSON);
revisit at M11.P11.5.

Options for tool isolation:

- **A)** Subprocess per call (simple, slow startup).
- **B)** Long-lived tool processes (faster, more state).
- **C)** WASM (wasmtime / wasmer) — best isolation, real
  resource limits, cross-platform; new build dep.
- **D)** Native plugins via the M15 module ABI — fastest, no
  isolation. Operator vouches for the plugin code.

**Decide by:** M11.P11.1 design. Affects manifest schema +
permissions model.

## Q7. Knowledge graph storage

**Status:** open
**Default:** SQLite with sqlite-vec extension (lowest dep
addition).

Alternatives floated in `plans/ideas.md`: dedicated vector DB
(qdrant / chroma) running as a sibling Compose service; graph DB
(neo4j / kuzu). Trade: a separate service is more capable and
takes more memory; sqlite-vec inherits acva's existing memory
thread + path resolution.

**Decide by:** M11.P11.3.

## Q22. Graph DB choice for KG

**Status:** open — investigated in [`p11.3_knowledge_graph_vector_store/s11.3.2_graph_database_investigation.md`](p11.3_knowledge_graph_vector_store/s11.3.2_graph_database_investigation.md).

Distinct from Q7 (which only chose sqlite-vec for v1). This is
the v2 question: do we add a graph store (neo4j / kuzu /
oxigraph) for richer queries?

- **A)** Stay sqlite-vec only.
- **B)** Optional sidecar — operator runs a graph DB
  alongside if their workload demands it; acva auto-detects.
- **C)** First-class swap — KG store is a config-selected
  backend.

**Decide by:** S11.3.2 close, after a workload survey
demonstrates whether the queries actually demand graph
semantics.
