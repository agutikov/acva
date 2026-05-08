# M13 — Distribution Hardening

**Estimate:** ~2.5 months (~10 weeks).  
**Status:** planned.  
**Depends on:** none directly. Could be picked up at any time after M9.P9.0 closes the bugs; deferred so user-visible feature work (M9–M12) lands first.  
**Blocks:** M14 (cross-platform build needs the CI matrix from P13.3).

## Goal

Take acva from "git clone + build.sh on the dev box" to
"package install on a fresh Linux machine". Absorbs the M8C
packaging stretch + everything else needed to make
distribution real.

## Phases

| ID    | Title                              | Duration | Folder |
|-------|------------------------------------|----------|--------|
| P13.0 | M8C packaging stretch              | 1-2 wk   | [`p13.0_m8c_packaging_stretch/`](p13.0_m8c_packaging_stretch/) |
| P13.1 | Linux native packages              | 2-3 wk   | [`p13.1_linux_native_packages/`](p13.1_linux_native_packages/) |
| P13.2 | Self-contained installers          | 3-4 wk   | [`p13.2_self_contained_installers/`](p13.2_self_contained_installers/) |
| P13.3 | CI/CD + release engineering        | 2-3 wk   | [`p13.3_cicd_release_engineering/`](p13.3_cicd_release_engineering/) |
| P13.4 | LLM backend strategy (llama.cpp vs ollama) | 2-3 wk | [`p13.4_llm_backend_strategy/`](p13.4_llm_backend_strategy/) |

P13.4 is an investigation phase opening Q21 — does ollama replace
the M8A model-controller, or is llama.cpp the right backend for
the long term? The answer reshapes M8A maintenance + packaging
priorities.

P13.0 absorbs what was parked at MVP-close time as
`plans/postpone/m8c_packaging_stretch.md` (now removed; the
content lives in [`p13.0_m8c_packaging_stretch/`](p13.0_m8c_packaging_stretch/)).

## Acceptance

1. **AUR + .deb + .rpm** all build and install cleanly on a
   fresh VM of the matching distro; `acva --help` runs.
2. **Self-contained installer** (flatpak OR appimage) ships
   acva + the model-controller + the systemd units in one
   artifact.
3. **CI/CD** runs build + tests on Linux on every push;
   tagged releases produce signed artifacts.
4. **Image digests pinned** in the Compose stack so
   reproducible builds work across operators.

## Out of scope

- Backend models packaged with the installer. Models stay
  external (operators run `tools/acva-models sync` after
  install). Different update cadence.
- macOS / Windows packaging — that's M14.
