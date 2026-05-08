# M14 — Open Questions

Questions specific to the Cross-Platform milestone.
Cross-milestone and non-milestone questions live in
[`plans/open_questions.md`](../open_questions.md).

---

## Q5. macOS support seriousness

**Status:** open  
**Default:** Linux-only through M13; M14 is the decision point.

Real questions:

- Apple Silicon Metal vs CUDA — separate inference paths or pick
  one quantization that runs on both?
- VRAM budget on consumer Macs is different (M2/M3 unified
  memory).
- AEC: macOS doesn't have PipeWire; CoreAudio's voice processing
  IO unit is the equivalent but has a different API and
  different per-hardware behavior.

**Decide by:** start of M14. If Linux-only is the answer, M14
collapses to "Windows port" which may not be worth a milestone of
its own.

## Q27. Audio interleaving policy

**Status:** open — investigated in [`p14.5_audio_interleaving/s14.5.4_policy_and_ux.md`](p14.5_audio_interleaving/s14.5.4_policy_and_ux.md).

When does acva duck system audio (music, browser, …)?

- **A)** Always while assistant speaks. Simple; can be annoying
  when listening to music *and* asking acva background questions.
- **B)** Only when user is engaged (recently spoke). Smart;
  needs heuristic.
- **C)** Operator-configured per personality.

**Default:** A. **Decide by:** S14.5.4 close.

## Q28. Desktop UX wrapper choice

**Status:** open — investigated in [`p14.6_desktop_ux_wrappers/`](p14.6_desktop_ux_wrappers/).

What desktop chrome does acva ship?

- **A)** Tray icon only — minimal; relies on browser for the
  Web UI.
- **B)** Tray icon + Electron desktop app — heavyweight; hosts
  Web UI in a Chromium window.
- **C)** Tray icon + Tauri desktop app — lighter alternative
  to Electron.
- **D)** Native per-OS apps — most polished, most work.

**Decide by:** P14.6 close (S14.6.2 + S14.6.3 produce the
data; S14.6.1 ships the tray regardless).
