# M11 — Tools & Agent Platform

**Estimate:** ~3.5 months (~14 weeks).  
**Status:** planned (after M10).  
**Depends on:** M9.P9.4 GBNF infrastructure (grammar + action parser); M10 Web UI for tool authoring + permissions surfaces.  
**Blocks:** none directly. M12 multi-user can layer per-user tool permissions on top.

## Goal

Turn acva from "smart speaker" into "agent platform". Land a
tool framework with structural cancellation, GBNF-constrained
tool-call output, a knowledge graph + vector store, and the
first useful tools (config, web search, diagrams, quiz).

The MVP project line says "modular experimental platform" — this
is the first milestone where modularity actually shows up to
the operator: tools are the smallest unit of "you can extend
acva without recompiling."

## Phases

| ID    | Title                              | Duration | Folder |
|-------|------------------------------------|----------|--------|
| P11.1 | Tool framework                     | 3-4 wk   | [`p11.1_tool_framework/`](p11.1_tool_framework/) |
| P11.2 | GBNF tool-call output              | 2-3 wk   | [`p11.2_gbnf_tool_call_output/`](p11.2_gbnf_tool_call_output/) |
| P11.3 | Knowledge graph + vector store     | 3-4 wk   | [`p11.3_knowledge_graph_vector_store/`](p11.3_knowledge_graph_vector_store/) |
| P11.4 | Reference tools                    | 2-3 wk   | [`p11.4_reference_tools/`](p11.4_reference_tools/) |
| P11.5 | Tool authoring + docs              | 2-3 wk   | [`p11.5_tool_authoring_docs/`](p11.5_tool_authoring_docs/) |

P11.1 + P11.2 are the foundation; P11.3 / P11.4 / P11.5 follow.

## Out of scope

- Native plugins (loadable .so) — that's M15.P15.1 territory.
  M11 ships subprocess / WASM tools.
- Marketplace / registry — operators install tools by hand for
  now.
- Tool authoring in non-Python languages — pick one for the
  reference tools, add language adapters later.

## Acceptance

1. **Tool framework** — manifest schema, lifecycle, sandbox
   model decided (Q6), structural cancellation through the
   call boundary.
2. **GBNF tool-call output** — model emits `tool_call(name,
   args)` actions; parser routes to the framework.
3. **Knowledge graph + vector store** — embedding pipeline,
   schema in SQLite (or sqlite-vec), retrieval into
   `PromptBuilder`.
4. **At least 3 reference tools** working end-to-end: config
   modification, web search, knowledge-graph query.
5. **Tool author can write and ship a tool** following
   `docs/guide/tool-authoring.md` without modifying acva
   source.

## Risks

| Risk | Mitigation |
|---|---|
| Sandbox model debate (Q6) drags out | Time-box to 1 week at P11.1 start; start with subprocess as the dumb-but-working baseline; revisit at P11.5. |
| Knowledge graph balloons into a full graph DB | Stay in SQLite + sqlite-vec for v1; postpone neo4j / kuzu to a separate milestone if needed. |
| GBNF output shape diverges from M9.P9.4 actions | Reuse the same parser infrastructure; add `tool_call` as a peer action, don't fork. |

## See also

- M9.P9.4 GBNF action grammar (the parser this builds on).
- [`open_questions.md`](open_questions.md) Q6 (sandbox
  model) and Q7 (knowledge graph storage) — both decided here.
