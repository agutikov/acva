# MVP bugs

Open bugs surfaced during the M8C dogfood pass that aren't blocking
MVP close but shouldn't be lost. One file per bug; remove the file
when fixed (the git history preserves it).

| File | Severity | Stack |
|---|---|---|
| [`no_metrics_in_grafana.md`](no_metrics_in_grafana.md) | UX | M8B observability |
| [`wake_word_latency_and_threshold.md`](wake_word_latency_and_threshold.md) | medium | M8C wake-word |
| [`model_controller_unverified.md`](model_controller_unverified.md) | medium | M8A model-controller |

Each file follows the same shape: symptom, root cause if known,
"what fixed looks like" (often multiple paths, with a recommendation),
how to reproduce, and links to related code + plans. The format is
deliberately lighter than a milestone plan — bugs are punch-list
items, not multi-week deliverables.

Conventions:

- **Severity** — `low` (nice-to-fix), `medium` (real annoyance, not
  blocking), `high` (blocks a documented workflow). `critical` for
  data-loss / crashes.
- **Stack** — which milestone / subsystem owns the surface. Helps
  someone fixing one bug avoid stepping on another.
- **Status** — `open`, `triaged`, `in-progress`, or `wontfix`. Add
  closure notes inline; don't delete the file until git-mv'ing it
  to an archive.
