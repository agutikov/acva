# observability/ — Prometheus + Grafana

M8B Step 2. Operator-installed observability stack for the local
acva orchestrator. Separate from `packaging/compose/` so the
inference fast path stays minimal.

## What's here

```
observability/
  docker-compose.yml
  prometheus/prometheus.yml          # scrape acva (host:9876) + llama (8081)
  grafana/provisioning/
    datasources/prometheus.yml       # auto-wire the PromQL datasource
    dashboards/acva.yml              # auto-load JSON dashboards
  README.md
../grafana/acva.json                 # the dashboard itself (mounted into Grafana)
```

The dashboard JSON lives at `packaging/grafana/acva.json` so the
panels are version-controlled with the rest of the project, away
from Grafana's runtime DB.

## Bring it up

```sh
cd packaging/observability
docker compose up -d
docker compose ps     # both services should be healthy
```

Then open <http://127.0.0.1:3000>. Default login is `admin/admin`
(change via `GF_ADMIN_USER` / `GF_ADMIN_PASSWORD` env or the
Grafana UI on first login). Anonymous viewer access is enabled —
operators can read the dashboard without logging in.

## What gets scraped

| Job     | Target                          | Notes                                     |
|---------|---------------------------------|-------------------------------------------|
| acva    | `host.docker.internal:9876`     | the orchestrator's `/metrics`             |
| llama   | `host.docker.internal:8081`     | requires `--metrics` (set by compose)     |

Speaches doesn't expose `/metrics`. The wedge / VRAM signal
(`voice_speaches_*`) is exported by acva's VramMonitor and arrives
via the `acva` job — no separate Speaches scrape needed.

`host.docker.internal` resolves to the host's docker bridge gateway
via the `extra_hosts: ["host.docker.internal:host-gateway"]` entry
in `docker-compose.yml`. Standard on Linux + macOS.

## Editing the dashboard

Edit `packaging/grafana/acva.json` in-tree, then either:

- **In the UI:** "Save dashboard" overwrites the JSON via Grafana's
  default editor (anonymous viewers can't save; log in as admin
  first). Then commit the file from disk.
- **Via the file provider:** Grafana re-scans
  `/var/lib/grafana/dashboards` every 10 s; just re-save the JSON
  and the panels reload without a container restart.

## Volumes

Persistence uses named Docker volumes (`prometheus-data`,
`grafana-data`) rather than bind-mounts. Reasons: Prom runs as uid
65534 and Grafana as uid 472 inside their containers, so a host
bind-mount would need a chown step the operator might forget. Named
volumes let Docker manage permissions automatically.

Wipe with `docker compose down -v` (clears Prometheus TSDB and
Grafana saved state). Inspect via `docker volume inspect
acva-observability_prometheus-data` if you need the on-disk path.

## Default retention

Prometheus is configured for 30 days TSDB retention
(`--storage.tsdb.retention.time=30d` in `docker-compose.yml`).
Tighten / extend per host disk budget; soak runs at 5 s scrape
interval generate ~2 GB/day uncompressed across the
`voice_*`+host.docker.internal jobs.

## Bringing up acva alongside

acva runs on the host, not in this compose stack — that's
deliberate (CLAUDE.md pillar #2: realtime audio path stays direct,
no container hop). After this stack is up, start acva normally:

```sh
./_build/dev/acva                    # dev path
# or
./scripts/soak.sh --duration 4h      # full M8B Step 1 soak
```

Both populate the dashboards via the existing `/metrics` surface.

## Tearing down

```sh
docker compose down       # keep data
docker compose down -v    # also wipe Prometheus TSDB + Grafana state
```
