# M10 — Built-in Web UI

**Estimate:** ~3 months (~12 weeks).  
**Status:** planned (after M9).  
**Depends on:** MVP closed (M0–M8C ✅). M9.P9.1 partials make the dialogue trace much more useful.  
**Blocks:** M11 (tool framework benefits from a UI surface for tool authoring + permissions).

## Goal

A first-party Web UI served by `acva` itself — same process, same
binary in production. Local-first; embedded into the binary so
operators don't run a separate web server. Replaces the "watch
JSON streams in a terminal" experience with something usable.

The UI surfaces:

- **Chat + dialogue inspection** — chat surface for text-driven
  sessions; live FSM trace + per-turn span viewer; sentence-by-
  sentence playback timeline.
- **Configuration editing** — schema-aware form with hot-vs-restart
  awareness; merges with the existing reload semantics.
- **Memory + session management** — sessions / turns / facts /
  summaries CRUD; privacy commands (mute / unmute / wipe / new
  session) surfaced.
- **Audio + AEC + diagnostics** — VAD/RMS scope, AEC ERLE
  history, playback queue depth, log + metric viewer,
  debug-bundle export.
- **Model + voice management** — `tools/acva-models` flows through
  the UI; install / select / verify without hitting the CLI.

Source design: the pre-M10 `plans/web_ui_architecture.md`
sketch is folded into this milestone (removed 2026-05-08;
git history preserves it). M10 turns the sketch into an
executed plan.

## Phases

| ID    | Title                              | Duration | Folder |
|-------|------------------------------------|----------|--------|
| P10.1 | API boundary                        | 2-3 wk   | [`p10.1_api_boundary/`](p10.1_api_boundary/) |
| P10.2 | Static assets + production embed    | 2-3 wk   | [`p10.2_static_assets_embed/`](p10.2_static_assets_embed/) |
| P10.3 | Chat & dialogue inspection          | 2-3 wk   | [`p10.3_chat_dialogue_inspection/`](p10.3_chat_dialogue_inspection/) |
| P10.4 | Config editor + memory ops          | 2-3 wk   | [`p10.4_config_editor_memory_ops/`](p10.4_config_editor_memory_ops/) |
| P10.5 | Audio + diagnostics panels          | 2-3 wk   | [`p10.5_audio_diagnostics_panels/`](p10.5_audio_diagnostics_panels/) |

P10.1 + P10.2 land first as foundation. P10.3 / P10.4 / P10.5
are independent and can be picked up in parallel once the
foundation is in place.

## Out of scope

- Remote access. Default bind stays `127.0.0.1`. Remote access
  is a deliberate post-MVP discussion: requires auth, TLS, a
  threat model.
- Multi-tenant / multi-user (per-user views) — that's M12
  territory.
- Custom dashboards. Operators get the shipped panels; deeper
  custom dashboards stay in Grafana.

## Acceptance

1. **API stable** — `/api/v1/*` documented in OpenAPI; the
   existing script-friendly routes (`/status`, `/metrics`,
   `/health`, `/reload`, `/wipe`, `/restart`) keep their shape.
2. **One binary** — release build embeds the static assets;
   no separate web server required at runtime.
3. **Hot reload aware** — config editor distinguishes hot from
   restart-required, validates before applying, blocks
   destructive changes behind explicit confirmation.
4. **No DTO leakage** — handlers return DTOs, not internal
   classes or raw rows. Frontend has zero knowledge of C++
   types.
5. **All M2 control endpoints survive** — `/mute`, `/unmute`,
   `/new-session`, `/wipe`, `/restart` work both via UI and
   curl, with identical semantics.

## Risks

| Risk | Mitigation |
|---|---|
| Feature creep — Web UI is naturally a "build it forever" sink | Strict phase scope; P10.3+ each has a defined surface and ships independently. Anything wider (custom dashboards, plugin UIs) gets postponed. |
| Frontend stack churn | Pick conservative stack at P10.1 — Vite + Preact / Solid (smaller than React). Lock the bundler config; treat upgrades as their own stages. |
| Static-asset embedding bloats the binary | Compress aggressively; only ship release-mode assets. Dev mode reads from disk. |
| Embedded HTTP server scope creep — cpp-httplib was sized for control-plane-only use | When cpp-httplib hits a wall (long-running ops, WebSocket), introduce a second server lib rather than rewriting the control plane. |

## See also

- The pre-M10 `plans/web_ui_architecture.md` architecture
  sketch — folded into this milestone, file removed
  2026-05-08. Visible via `git log -- plans/web_ui_architecture.md`.
- [`open_questions.md`](open_questions.md) Q3 (config in DB
  vs YAML) — decided in P10.4.
