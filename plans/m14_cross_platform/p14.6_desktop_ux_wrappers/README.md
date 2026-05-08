# P14.6 — Desktop UX wrappers (Q13 expanded)

**Estimate:** 4-6 weeks (depends on which wrappers ship).
**Status:** planned — replaces parked `plans/open_questions.md` Q13.
**Depends on:** M10 Web UI (the surface the wrapper hosts).
**Blocks:** none.

## Goal

Provide a discoverable, OS-native chrome around acva so a
non-technical operator doesn't have to keep a terminal open.
Two candidate shapes: a tray icon driving the existing local
Web UI, or a packaged desktop app (Electron / Tauri / native).

Q13 parked the choice. This phase produces a decision + at
least one shipped wrapper.

## Stages

| ID     | Title                              | Shape            | Folder |
|--------|------------------------------------|------------------|--------|
| S14.6.1 | Tray icon per OS                   | investigation + impl | [`s14.6.1_tray_icon_per_os.md`](s14.6.1_tray_icon_per_os.md) |
| S14.6.2 | Electron evaluation                | investigation        | [`s14.6.2_electron_evaluation.md`](s14.6.2_electron_evaluation.md) |
| S14.6.3 | Tauri alternative                  | investigation        | [`s14.6.3_tauri_alternative.md`](s14.6.3_tauri_alternative.md) |
| S14.6.4 | Quick-menu UX                      | impl                  | [`s14.6.4_quick_menu_ux.md`](s14.6.4_quick_menu_ux.md) |

## Out of scope

- Mobile companion apps. Phone is post-M14 entirely.
- Multi-window OS shell. Tray + a single Web-UI window is the
  ceiling for v1.

## Acceptance

1. Q28 (wrapper choice) resolved.
2. At least one tray icon ships on Linux. macOS / Windows
   tray follow if M14.P14.1 / P14.2 close first.
3. Either Electron or Tauri ships an installer, OR both
   close with a written decision-not-to-ship.

## See also

- `plans/open_questions.md` Q13 (parent).
- M10 Web UI — the wrapper hosts this surface.
- M13.P13.1 / P13.2 packaging — the packaging stages this
  phase rides on.
