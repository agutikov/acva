# P14.5 — Audio interleaving with media players (Q11 expanded)

**Estimate:** 2-3 weeks per OS; the cross-OS bundle is 6-8 weeks.
**Status:** planned — replaces parked `plans/open_questions.md` Q11.
**Depends on:** M14.P14.1 macOS port and P14.2 Windows port for
the per-OS plumbing. Linux can ship independently.
**Blocks:** none.

## Goal

When acva speaks, duck system audio (music, browser video, …)
and restore on completion. Each OS has a different primitive;
this phase explores all three plus the policy layer.

## Stages

| ID     | Title                                  | Shape         | Folder |
|--------|----------------------------------------|---------------|--------|
| S14.5.1 | PipeWire ducking (Linux)               | investigation + impl | [`s14.5.1_pipewire_ducking_linux.md`](s14.5.1_pipewire_ducking_linux.md) |
| S14.5.2 | CoreAudio session (macOS)              | investigation + impl | [`s14.5.2_coreaudio_session_macos.md`](s14.5.2_coreaudio_session_macos.md) |
| S14.5.3 | WASAPI ducking (Windows)               | investigation + impl | [`s14.5.3_wasapi_ducking_windows.md`](s14.5.3_wasapi_ducking_windows.md) |
| S14.5.4 | Policy + UX (Q27)                      | decision + impl     | [`s14.5.4_policy_and_ux.md`](s14.5.4_policy_and_ux.md) |

## Out of scope

- Playing into specific output devices (multi-zone audio).
- Live-stream cooperation (Discord / OBS) — the assistant
  sharing the call. Separate concern; flag if requested.

## Acceptance

1. Operator-facing config field `cfg.audio.ducking.{enabled,
   amount_db, attack_ms, release_ms}` honored on each
   platform that has stage shipped.
2. Q27 (interleaving policy) resolved.
3. Per-OS stage that doesn't ship closes with a write-up of
   what blocked it (typically OS API gaps).

## See also

- `plans/open_questions.md` Q11 (parent).
- M14.P14.1 / P14.2 ports — pre-requisites for the macOS /
  Windows stages.
