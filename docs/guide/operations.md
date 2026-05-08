# acva — Operations

Day-2 ops guide: how to deploy, watch, change, and clean up. For
**failure** triage (symptom → fix), see `docs/guide/troubleshooting.md` —
this doc covers the intentional operations.

## Lifecycle

### First start

```sh
# Build
./build.sh release             # _build/release/acva

# Bring up backends (Compose default)
./scripts/dev-up.sh            # llama + speaches; ~60 s to healthy from cold pull

# Install models referenced by the active config
tools/acva-models sync         # idempotent; ~6.5 GB on first run

# Run the orchestrator
./_build/release/acva
```

The first run materializes `~/.config/acva/default.yaml`,
`~/.local/share/acva/acva.db`, and `~/.local/share/acva/log/`. None
require manual creation.

### Daily start / stop

```sh
./scripts/dev-up.sh            # backends
./_build/release/acva &        # orchestrator (Ctrl+C or kill to stop)
./scripts/dev-down.sh          # stop backends, keep volumes
```

`dev-down.sh --wipe` also clears anonymous Docker volumes; the
`ACVA_MODELS_DIR` host bind-mount is **never** wiped automatically.

### Production-style with systemd

```sh
./scripts/install-systemd.sh
systemctl --user start acva.target
systemctl --user status acva.target

# Verify all three units active
systemctl --user status acva-llama.service acva-speaches.service acva.service

# Tail logs
journalctl --user -fu acva-llama -fu acva-speaches -fu acva
```

`packaging/systemd/README.md` has the full runbook (lingering for
headless workstations, system-wide install, sd-bus extension).

## Health & status

The orchestrator exposes one HTTP control plane on
`127.0.0.1:9876`:

| Endpoint | Purpose |
|---|---|
| `GET /status`   | JSON snapshot: FSM state, active turn, supervisor service states, queue depths, APM block, Speaches block (with remediation hint when wedged), pipeline state. |
| `GET /metrics`  | Prometheus exposition. |
| `GET /health`   | `ok\n` — for readiness probes. |
| `POST /reload`  | Re-read config; apply hot fields or 409 listing restart-required diff. |
| `POST /restart` | Warm restart — checkpoint runtime state + `execv` self. Preserves session id and prior-turn visibility. |
| `POST /mute`, `POST /unmute` | Privacy: short-circuit capture frames; force-endpoints any in-progress utterance. |
| `POST /new-session` | Open a fresh session id (returns the new id as JSON). |
| `POST /wipe?session=<id>` | Cascade-delete one session (turns, facts, summaries). |
| `POST /wipe?all=true` | DROP + re-exec the schema in one transaction. Bare `/wipe` returns 400. |

`SIGHUP` is equivalent to `POST /reload` — useful when running under
systemd:

```sh
systemctl --user kill -s HUP acva.service
```

The control plane is loopback-only by default. To watch from another
machine, use SSH port forwarding rather than rebinding (any rebind is
restart-required and there's no auth in front of the endpoints).

## Observability stack

```sh
cd packaging/observability && docker compose -p acva-obs up -d
# Grafana   http://127.0.0.1:3000  (anonymous viewer; admin/admin first login)
# Prometheus http://127.0.0.1:9090
```

The dashboard auto-loads from `packaging/grafana/acva.json` — 7
panels: FSM state, Speaches VRAM + wedged, TTS first-audio P50/P95,
service health, playback queue + underruns, watchdog stuck +
barge-in, pipeline state.

The observability stack uses `network_mode: host` so it reaches the
loopback control plane on `:9876` and llama's metrics on `:8081`
without bridge translation.

## Logs

Default sink is `dir` — one file per process under
`${XDG_DATA_HOME:-~/.local/share}/acva/log/acva-<UTC-timestamp>.log`,
JSON-per-line.

```sh
# Tail the live log
tail -F ~/.local/share/acva/log/acva-$(date -u +%Y%m%d-)*.log

# Filter to one component
jq -c 'select(.component == "dialogue")' ~/.local/share/acva/log/acva-*.log

# Switch to stderr / journal at startup (or via reload — `logging.level`
# is hot, but `logging.sink` is restart-required)
sed -i 's/^  sink: dir$/  sink: stderr/' ~/.config/acva/default.yaml
```

When running under systemd, journald captures stdout regardless of
the sink setting; use `journalctl --user -u acva -f` to tail.

## Switching models & personalities

### LLM swap

```sh
tools/acva-models select llm dialog
```

This:
1. rewrites `~/.config/acva/default.yaml` (`llm.model: dialog`),
2. rewrites `packaging/compose/.env` (`ACVA_LLM_MODEL=...`),
3. recreates the `acva-llama` container so llama-server picks up the
   new GGUF.

`--no-restart` skips step 3 — useful when you want to change config
now and bounce later. After a select, the orchestrator's
`/reload` rejects (model id is restart-required); use `/restart`
or just kill + relaunch.

### STT or TTS voice swap

```sh
tools/acva-models select stt large-v3-turbo
tools/acva-models select tts en-amy --lang en
```

Both swaps recreate `acva-speaches` (Speaches' VRAM-leak workaround
— see §L7). Skip with `--no-restart` if you want to defer.

### Personality

Personalities live under `personalities:` in the same YAML file.
Switch by editing `active_personality:` and bouncing — the field is
restart-required.

```sh
sed -i 's/^active_personality: .*/active_personality: consultant/' \
    ~/.config/acva/default.yaml
curl -fsS -X POST http://127.0.0.1:9876/restart
```

## Privacy commands

Mid-session privacy controls:

```sh
# Stop listening (capture frames are dropped; in-progress utterance forced to end)
curl -fsS -X POST http://127.0.0.1:9876/mute

# Resume listening
curl -fsS -X POST http://127.0.0.1:9876/unmute

# Open a fresh session (existing session ends; subsequent turns belong to the new one)
curl -fsS -X POST http://127.0.0.1:9876/new-session

# Delete one session and all its turns / facts / summaries
curl -fsS -X POST 'http://127.0.0.1:9876/wipe?session=42'

# Nuke the whole DB (DROP + re-create schema in one transaction)
curl -fsS -X POST 'http://127.0.0.1:9876/wipe?all=true'
```

Bare `/wipe` returns 400 — the qualifier is mandatory to avoid
accidental scope.

## Memory CLI

`acva memory <subcommand>` is a process-isolated client to the same
SQLite DB the orchestrator owns. Read paths coexist with a live acva
via WAL; write paths block on `busy_timeout=5000`.

| Subcommand           | Purpose |
|---|---|
| `sessions`           | List sessions; `--json` for jq pipelines. |
| `session <id>`       | One session's metadata + turn count. |
| `turns [--session N]` | List turns; `--limit`, `--since`. |
| `turn <id>`          | One turn's full text + lifecycle state. |
| `facts [--session N]` | Extracted facts. |
| `summaries [--session N]` | Long-context rollups. |
| `delete-turn <id>`   | Drop one turn. `--dry-run` previews. |
| `delete-session <id>` | Drop a session and cascades. |
| `delete-fact <id>`   | Drop one fact. |
| `wipe [--all] [--yes]` | Wipe the DB. `--yes` skips confirmation. |
| `vacuum`             | `VACUUM` to reclaim space. |
| `restart`            | `POST /restart` against a running orchestrator. |

The orchestrator does **not** need to be stopped for read paths;
write paths are fine on a live DB but block briefly on busy_timeout.

```sh
# What sessions do I have?
acva memory sessions

# What was the user's most recent turn?
acva memory turns --session 42 --limit 1 --json | jq '.turns[].user_text'

# Compact the DB after a wipe
acva memory vacuum
```

## Restart strategies

| What you want | How |
|---|---|
| Apply a hot config change | `POST /reload` or `kill -HUP` — no downtime. |
| Apply a restart-required change, preserve session | `POST /restart` — warm restart via checkpoint + `execv`. Same session id; prior turns stay visible to the LLM. |
| Cold restart (lose session) | Kill + relaunch. New session opens on next user turn. |
| Backend died | Compose's `restart: unless-stopped` brings it back; the supervisor gates the dialogue path until `/health` recovers. |
| Speaches stuck at full VRAM (wedge) | `docker restart acva-speaches`; the wedge classifier flags it via `voice_speaches_wedged` and `/status.speaches.remediation`. |

Warm restart is gated on (a) the runtime-state row's age (must be
recent — orphan checkpoints are ignored) and (b) the FNV-1a hash of
the resolved config matching the prior process's. If you edited
config between checkpoint and resume, the resume gate fails and you
get a fresh session (intentional — config change shouldn't pretend to
continue a prior context).

## Backups & DB hygiene

The SQLite DB is a single file: `~/.local/share/acva/acva.db`. WAL
mode means a hot copy needs a checkpoint:

```sh
# Hot snapshot (safe with acva running)
sqlite3 ~/.local/share/acva/acva.db ".backup /tmp/acva-backup.db"

# Or stop acva and just copy
cp ~/.local/share/acva/acva.db /tmp/acva-backup.db
```

Restore:

```sh
# Stop acva first
mv /tmp/acva-backup.db ~/.local/share/acva/acva.db
```

Periodic `VACUUM` reclaims space after large `wipe?session=` deletes:

```sh
acva memory vacuum   # while acva is stopped, ideally
```

## Soak / load testing

```sh
# 60 s smoke (FakeDriver only — no backends needed)
./_build/dev/acva demo soak-mini

# 4-hour acceptance run with CSV trace + report
./scripts/soak.sh
```

The full soak emits a CSV at 5 s cadence and runs four acceptance
gates (no_crashes / rss_growth / queue_depth_stable /
service_restarts_ok). Auto-restarts `acva-speaches` on rising-edge
`voice_speaches_wedged` (60 s cooldown). `soak-report.txt` summarizes
at the end.

## Updating

### acva itself

```sh
git pull
./build.sh release
# Hot reload picks up config changes; warm restart for code:
curl -fsS -X POST http://127.0.0.1:9876/restart
# Or cold:
systemctl --user restart acva.service     # systemd
```

### Backend images (Compose)

The compose file pins to rolling tags (`:server-cuda`,
`:latest-cuda`) — image-digest pinning is M8C stretch work.

```sh
cd packaging/compose
docker compose pull              # fetch newer images
docker compose up -d             # recreate with new images
```

Version drift hygiene: `tools/acva-models verify` checks file sizes
against the registry; `tools/acva-models status` checks .env / cfg /
file / container / loaded-model consistency end-to-end and exits
non-zero on drift.

## When something is wrong

`docs/guide/troubleshooting.md` is the symptom-first index. Quick triage:

```sh
./_build/dev/acva demo health     # backends
./_build/dev/acva demo tone       # audio output
./_build/dev/acva demo tts        # Speaches TTS + playback
./_build/dev/acva demo llm        # LLM streaming
./_build/dev/acva demo capture    # mic + VAD
./_build/dev/acva demo transcribe # mic + streaming STT
./_build/dev/acva demo fsm        # no-backends FSM smoke
```

The first failing demo points at the broken hop. Each demo prints
either `demo[<name>] done: …` on success or `demo[<name>] FAIL: …`
with the immediate cause.
