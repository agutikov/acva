# Plans

Forward-looking work for acva. The MVP (M0–M8C) is closed; its
plans + open questions are archived under
[`docs/history/MVP/`](../docs/history/MVP/). This directory carries
**post-MVP** work only.

## Layout

```
plans/
├── open_questions.md            cross-milestone + parked questions only
├── README.md                    this file
├── ideas.md                     ongoing scratchpad
├── bugs/                        punch-list (one md per bug)
├── scenarios/                   niche use-case parking lot (Q4 expansion)
│   ├── README.md
│   ├── game_npc_character_design.md
│   ├── voice_translation.md
│   └── tg_on_call_assistant.md
├── m9_conversational_quality/
│   ├── README.md                milestone overview (goal, phases, acceptance)
│   ├── open_questions.md        questions scoped to this milestone
│   ├── p9.0_<phase>/
│   │   └── s9.0.1_<topic>.md    stages — md files with task sections
│   └── p9.1_<phase>/...
├── m10_web_ui/
│   ├── README.md
│   ├── open_questions.md
│   └── ...
├── m11_tools_agent_platform/...
├── m12_multiuser_personality/...
├── m13_distribution_hardening/...
├── m14_cross_platform/...
└── m15_modularity_plugin_platform/...
```

## Schema

Four-level hierarchy with explicit duration brackets.

| Level     | Duration       | ID format          | On-disk |
|-----------|----------------|--------------------|---------|
| Milestone | > 2 months     | `M<n>`             | folder + `README.md` |
| Phase     | 2–4 weeks      | `P<n>.<m>`         | folder (no README) |
| Stage     | days–weeks     | `S<n>.<m>.<k>`     | one `.md` file |
| Task      | up to 2 days   | `T<n>.<m>.<k>.<j>` | a `## Task T<n>.<m>.<k>.<j> — <title>` section inside the stage file |

IDs inherit the parent verbatim (a task's ID always reveals its
full path through the hierarchy).

## Roadmap

| #   | Milestone                       | Duration  | Status |
|-----|---------------------------------|-----------|--------|
| M9  | Conversational Quality          | ~3 mo     | next   |
| M10 | Built-in Web UI                 | ~3 mo     | planned |
| M11 | Tools & Agent Platform          | ~3.5 mo   | planned |
| M12 | Multi-User & Personality        | ~3 mo     | planned |
| M13 | Distribution Hardening          | ~2.5 mo   | planned |
| M14 | Cross-Platform                  | ~3.5 mo   | planned |
| M15 | Modularity / Plugin Platform    | ~2.5 mo   | planned |

Total: ~21 months for one developer.

Ordering is **not** locked — see [`open_questions.md`](open_questions.md)
Q1 (modularity first vs last, cross-milestone) and
[`m14_cross_platform/open_questions.md`](m14_cross_platform/open_questions.md)
Q5 (macOS seriousness) for the live debates.

## Conventions

- One milestone = one folder. Milestone README has goal, phase
  list, depends-on, blocks, acceptance, estimate.
- One phase = one folder. No phase README; the milestone README
  covers it. Stages live inside as md files.
- One stage = one md file. Task list inside as `## Task T<id> — <title>`
  sections. Tasks are sized to ≤ 2 days each; if a task takes more,
  it's a stage and gets promoted.
- Stages can be added or removed without renumbering siblings — IDs
  are sticky (don't reuse a closed ID; allocate the next free).
- Closed phases / stages get an `## Status: ✅ closed YYYY-MM-DD`
  note at the top with a one-line outcome. Don't delete closed
  files — git history is fine but operators reading the milestone
  benefit from seeing what closed.
- Bugs that surface mid-milestone go in `plans/bugs/` if they're
  unrelated; in the active stage's task list if they're directly
  caused by the work.
- New design questions go in the relevant milestone's local
  `open_questions.md`; cross-milestone or non-milestone
  questions go in [`plans/open_questions.md`](open_questions.md).
  Plan files reference questions by ID, never inline the
  decision text.
- Question IDs are global and sticky: the next new question
  is `Q<n+1>` regardless of which file it lands in. The
  global file's index table tracks where each Q lives.
