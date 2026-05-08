# P9.5 — TTS expression (Q10 expanded)

**Estimate:** 4-6 weeks (depth varies by which stages ship; min preset
+ SSML feasibility is the floor, RVC + neural vocoders is the
ceiling).
**Status:** planned — replaces parked `plans/open_questions.md` Q10.
**Depends on:** Speaches TTS surface (today's baseline). RVC stage
(S9.5.3) and neural-vocoders stage (S9.5.4) need GPU headroom — see
the existing 8 GB VRAM budget memory note.
**Blocks:** none directly. Min/max preset harness (S9.5.8) is the
gate that turns the rest into operator-visible features.

## Goal

Make acva's voice expressive enough to sustain multi-hour
conversations without sounding like a 2010-era TTS demo, while
keeping the "default config = plain computer voice" path so
operators on a budget hardware path can opt out.

The pre-MVP `plans/open_questions.md` Q10 collected the entire
TTS expression family as one parked question. This phase
unfolds it. Each stage is one investigation track plus the
follow-on implementation if the investigation says "ship it".

## Stages

| ID    | Title                                       | Shape          | Folder |
|-------|---------------------------------------------|----------------|--------|
| S9.5.1 | Sound filter chain (cheap DSP)             | investigation + impl | [`s9.5.1_sound_filter_chain.md`](s9.5.1_sound_filter_chain.md) |
| S9.5.2 | SSML feasibility                           | investigation       | [`s9.5.2_ssml_feasibility.md`](s9.5.2_ssml_feasibility.md) |
| S9.5.3 | RVC (Retrieval-based Voice Conversion)     | investigation + impl | [`s9.5.3_rvc_voice_conversion.md`](s9.5.3_rvc_voice_conversion.md) |
| S9.5.4 | Neural vocoders (HiFi-GAN, BigVGAN, etc.)  | investigation       | [`s9.5.4_neural_vocoders.md`](s9.5.4_neural_vocoders.md) |
| S9.5.5 | Prosody control (rate / pitch / emphasis)  | investigation + impl | [`s9.5.5_prosody_control.md`](s9.5.5_prosody_control.md) |
| S9.5.6 | Artistic / sarcastic / expressive modes    | investigation + impl | [`s9.5.6_artistic_sarcastic_modes.md`](s9.5.6_artistic_sarcastic_modes.md) |
| S9.5.7 | Style post-processing pipeline             | investigation + impl | [`s9.5.7_style_post_processing.md`](s9.5.7_style_post_processing.md) |
| S9.5.8 | Min vs max preset comparison harness       | impl + harness      | [`s9.5.8_min_vs_max_preset_harness.md`](s9.5.8_min_vs_max_preset_harness.md) |

S9.5.1 + S9.5.2 + S9.5.5 are the floor (cheap, low-risk). S9.5.3
and S9.5.4 are the ceiling — keep them gated on dogfood demand.
S9.5.8 closes the phase.

## Out of scope

- Re-implementing TTS engines. acva keeps using Speaches /
  Piper / Kokoro upstream; expression is layered on top.
- Voice cloning from operator audio for impersonation —
  M12.P12.4 owns personality cloning; this phase ships voice
  styling for whatever voice is selected.

## Acceptance

1. Operator can A/B "min" vs "max" preset on the same prompt
   and rate the result (S9.5.8 harness produces side-by-side
   audio + a structured rating CSV).
2. SSML feasibility documented (S9.5.2) — concrete yes/no per
   backend, with workaround if no.
3. At least one cheap-DSP filter (S9.5.1) and one prosody knob
   (S9.5.5) ship as hot-reload-tunable config fields.
4. RVC and neural-vocoder stages produce a write-up either way
   (ship / defer with rationale).

## See also

- `plans/open_questions.md` Q10 (parent — this phase replaces it).
- M12.P12.4 personality cloning — distinct concern (style
  fingerprint, not voice timbre).
