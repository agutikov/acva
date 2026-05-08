# M15 — Modularity / Plugin Platform

**Estimate:** ~2.5 months (~10 weeks).  
**Status:** planned.  
**Depends on:** none, technically. Could be picked up earlier per [`plans/open_questions.md`](../open_questions.md) Q1 (cross-milestone) — there's a real argument for moving M15 between M9 and M10 so subsequent milestones land into a stable plugin platform.  
**Blocks:** retroactively, M11 tools become first-class plugins; M14 cross-platform audio backends become plugins.

## Goal

Loadable runtime plugins for audio backends, memory stores,
TTS engines, and tool implementations. Extracts subsystems
behind a stable C ABI so operators can swap implementations
without recompiling acva.

This is an architectural milestone — significant code reshape
without much new user-visible behavior. Worth doing only if
the project intends to grow beyond a single configuration; if
acva stays a personal-use tool, M15 is over-engineering.

## Phases

| ID    | Title                              | Duration | Folder |
|-------|------------------------------------|----------|--------|
| P15.1 | Module ABI                         | 3-4 wk   | [`p15.1_module_abi/`](p15.1_module_abi/) |
| P15.2 | Extract reference modules          | 3-4 wk   | [`p15.2_extract_reference_modules/`](p15.2_extract_reference_modules/) |
| P15.3 | Plugin registry + ops              | 2-3 wk   | [`p15.3_plugin_registry_ops/`](p15.3_plugin_registry_ops/) |

## Out of scope

- Cross-language plugins. C ABI only; bindings for other
  languages (Python, Rust) come from third parties.
- Hot module reload. Plugins load at startup; reload requires
  restart. Hot-reload is a separate large project.

## Acceptance

1. **Module ABI documented and stable** — `include/acva/abi.h`
   carries the C interface; major version bumps require an
   open discussion.
2. **Reference modules ship as separate .so** — audio
   backend, memory store, at least one TTS engine. The
   shipped modules become the default set; operators can
   swap.
3. **Operators can install third-party modules** —
   `~/.local/share/acva/plugins/<name>.so` is auto-
   discovered; `acva plugins {list,enable,disable}` CLI works.
4. **Existing tests stay green** — extracting modules
   doesn't break functional tests.

## See also

- [`plans/open_questions.md`](../open_questions.md) Q1
  (ordering — cross-milestone).
- [`open_questions.md`](open_questions.md) Q9 (ABI stability —
  M15-local).
