# Bug — No metrics visible in Grafana / Prometheus

**Status:** open  
**Severity:** UX (observability stack runs but shows blank panels)  
**Filed:** 2026-05-08  
**Stack:** M8B observability — `packaging/observability/` + `packaging/grafana/`

## Symptom

After bringing up the observability stack:

```sh
cd packaging/observability && docker compose -p acva-obs up -d
```

Grafana opens (http://127.0.0.1:3000) and Prometheus opens
(http://127.0.0.1:9090) but the `acva` dashboard panels are mostly
empty / "No data".

## Root cause (found 2026-05-08 dogfood pass)

The observability stack scrapes acva's control-plane on
`127.0.0.1:9876/metrics`. **If `acva` isn't running, Prometheus marks
the target `down` and every dashboard panel that pulls from
`voice_*` metrics renders blank.** The `llama` target (`:8081`) is
independent and stays green via its own metrics endpoint.

Diagnostic snapshot from the failing state:

```
http://127.0.0.1:9876/metrics → down
    Get "http://127.0.0.1:9876/metrics": dial tcp 127.0.0.1:9876: connect: connection refused
http://127.0.0.1:8081/metrics → up
```

This is a discovery / UX issue, not a config bug — the stack is
wired correctly. But operators don't realize the dashboard needs
acva running to populate; the workflow `dev-up.sh` → "open Grafana"
omits the acva run step.

## What "fixed" looks like

Pick one (or all):

1. **Doc fix** — make the prerequisite explicit. `README.md`
   §Observability + `docs/guide/operations.md` § Observability
   stack should both say in bold:
   *"Grafana panels populate from `acva`'s `/metrics` endpoint —
   start acva (`./_build/release/acva`) before opening the
   dashboard, or panels will read 'No data'."*
2. **Empty-state handling** — Grafana panels could carry a
   `noValue: "acva not running"` hint, or each panel's text panel
   neighbor explains the prereq when target is `down`.
3. **dev-up.sh extension** — optional `--with-acva` flag that
   starts the orchestrator in the background (current behavior is
   strict: only Compose containers).
4. **Status-aware dashboard** — top-of-dashboard "service health"
   panel that includes a target-up indicator for acva, so the
   blank-everywhere state has an obvious explanation.

Recommended path: **#1 first** (zero-cost, immediate), **#4
second** (one new panel that points at the `up{instance=…}`
prometheus metric).

## How to verify the symptom

```sh
# 1. Start observability without acva running
cd packaging/observability && docker compose -p acva-obs up -d
sleep 5

# 2. Check Prometheus targets
curl -fsS http://127.0.0.1:9090/api/v1/targets | jq '.data.activeTargets[] | {url:.scrapeUrl, health}'

# Expected (failing):
#   {"url":"http://127.0.0.1:9876/metrics", "health":"down"}
#   {"url":"http://127.0.0.1:8081/metrics", "health":"up"}

# 3. Grafana dashboard panels reading voice_* are blank.

# 4. Start acva — the same panels populate within ~15 s.
./_build/release/acva &
sleep 15
curl -fsS http://127.0.0.1:9090/api/v1/targets | jq '.data.activeTargets[] | {url:.scrapeUrl, health}'
# Now both health=up.
```

## Related

- `plans/milestones/m8b_observability.md` Step 2 — observability
  stack landing
- `packaging/observability/prometheus/prometheus.yml` — scrape config
- `packaging/grafana/acva.json` — dashboard
- CLAUDE.md M8B Step 2 paragraph
