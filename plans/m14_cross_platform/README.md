# M14 — Cross-Platform

**Estimate:** ~3.5 months (~14 weeks).  
**Status:** planned (after M13).  
**Depends on:** M13.P13.3 CI matrix.  
**Blocks:** none.

## Goal

Port acva off Linux-only. macOS Metal first (Apple Silicon),
Windows second. Each is a substantial port — backend stack
swaps, audio backend changes, AEC equivalent.

This milestone is **conditional**. See
[`open_questions.md`](open_questions.md) Q5 — if the
answer is "Linux-only is fine", M14 collapses to the Windows
port at most.

## Phases

| ID    | Title                              | Duration | Folder |
|-------|------------------------------------|----------|--------|
| P14.1 | macOS Metal port                   | 4-5 wk   | [`p14.1_macos_metal_port/`](p14.1_macos_metal_port/) |
| P14.2 | Windows port                       | 4-5 wk   | [`p14.2_windows_port/`](p14.2_windows_port/) |
| P14.3 | Cross-platform CI matrix           | 2-3 wk   | [`p14.3_cross_platform_ci_matrix/`](p14.3_cross_platform_ci_matrix/) |
| P14.4 | Cross-platform packaging           | 2-3 wk   | [`p14.4_cross_platform_packaging/`](p14.4_cross_platform_packaging/) |
| P14.5 | Audio interleaving (Q11)           | 6-8 wk   | [`p14.5_audio_interleaving/`](p14.5_audio_interleaving/) |
| P14.6 | Desktop UX wrappers (Q13)          | 4-6 wk   | [`p14.6_desktop_ux_wrappers/`](p14.6_desktop_ux_wrappers/) |

P14.5 and P14.6 are expansions of parked Q11 / Q13.
P14.5 covers per-OS ducking primitives (PipeWire / CoreAudio /
WASAPI) plus a unified policy. P14.6 covers tray icon per OS,
Electron evaluation, Tauri alternative, and the menu UX. Both
are conditional on the platform their stages target having
shipped (M14.P14.1 / P14.2).

## Out of scope

- iOS / Android. Mobile is a different shape entirely;
  separate milestone if ever.
- Cross-compiling Linux→macOS or Linux→Windows. Each
  platform's CI builds natively; no cross-compilation
  voodoo.

## Acceptance

1. **macOS** — acva builds + runs on Apple Silicon. LLM via
   llama.cpp Metal backend; STT via faster-whisper or
   alternative; AEC via CoreAudio voice processing IO unit.
2. **Windows** — acva builds + runs on Windows 11. Audio via
   WASAPI; service registration via Windows Service.
3. **Cross-platform CI** — build + smoke runs on all three
   OSes per push.
4. **Each platform has at least one packaging artifact**
   (`.dmg`, `.msi`, `.deb`).
