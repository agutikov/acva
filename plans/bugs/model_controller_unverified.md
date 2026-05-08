# Bug — Does the model-controller sidecar work? Operator can't see.

**Status:** open / unverified (M8A Step 5 close-out explicitly deferred this to a manual dogfood pass)  
**Severity:** medium (the LLM model-swap UX is broken without it; static configs still work)  
**Filed:** 2026-05-08  
**Stack:** M8A — `src/llm/model_controller_client.{hpp,cpp}`, `packaging/model-controller/`

## Symptom

`acva` reads `cfg.llm.model_controller_url` (default
`http://127.0.0.1:9877`) at startup and, when
`cfg.llm.model_file` differs from what `llama-server` has loaded,
calls `POST /llm/load` and waits for the sidecar to swap the
model. **There is no obvious indicator from the operator side
whether the sidecar is reachable, healthy, currently loading
something, or not running at all.** The handoff is silent.

Today's state:

```
$ curl -fsS http://127.0.0.1:9877/health
curl: (7) Failed to connect to 127.0.0.1 port 9877 after 0 ms: …

$ docker ps --format '{{.Names}}' | grep -i controller
(empty)

$ grep model-controller packaging/compose/docker-compose.yml
(empty)
```

The sidecar source (`packaging/model-controller/main.go` +
`Dockerfile`) exists in-tree but is **deliberately not
auto-started** by `docker compose up` — per CLAUDE.md M8A Step 5
("the bind-mount is a deliberate operator opt-in; the README
has the paste-in stanza"). End-to-end smoke ("set
`cfg.llm.model_file` → sidecar recreates llama → acva picks up
the new model") was deferred to a manual dogfood pass that has
not happened yet.

So the actual question is two things:

1. **Does the sidecar still work** against current llama / Docker
   versions, post-M8A and post-M8C? Nobody has run it end-to-end.
2. **How would an operator see** that the sidecar is running, that
   acva is talking to it, and that a model swap is in progress?

## What "fixed" looks like

### Verification (1)

Run the dogfood pass:

```sh
# Bring up the controller manually (single Docker invocation per
# packaging/model-controller/README.md)
cd packaging/model-controller && docker build -t acva-model-controller .
docker run -d --name acva-model-controller \
    -v /var/run/docker.sock:/var/run/docker.sock \
    --network host \
    acva-model-controller

# Verify health
curl -fsS http://127.0.0.1:9877/health   # expected: 200 + JSON

# Edit ~/.config/acva/default.yaml: change llm.model_file to a
# different installed GGUF (use `tools/acva-models list`).
# Start acva. Expected:
#   - acva logs "model_controller: loaded=<old> requested=<new>"
#   - acva logs "model_controller: POST /llm/load"
#   - sidecar logs "recreating acva-llama with <new>.gguf"
#   - llama container restarts
#   - acva proceeds past the handoff into normal startup
#   - first turn uses the new model
```

If any step fails, file the specific failure as its own bug and
link back here.

### Operator visibility (2)

Pick at least one:

- **`/status` extension** — add a `model_controller` block to
  `GET /status`:
  ```json
  {
    "model_controller": {
      "url": "http://127.0.0.1:9877",
      "reachable": true,
      "loaded_model": "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
      "requested_model": "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
      "in_progress": false,
      "last_swap_at": "2026-05-08T14:31:22Z",
      "last_swap_duration_ms": 4231
    }
  }
  ```
  When unreachable, surface that explicitly with a remediation
  hint (similar to the Speaches wedge classifier's pattern).
- **Metrics** — `voice_model_controller_reachable{}`,
  `voice_model_controller_swap_total{outcome="ok|failed|skipped"}`,
  `voice_model_controller_swap_duration_seconds` histogram.
  Surface in the Grafana dashboard as a small "model-controller"
  panel.
- **Docs** — `docs/guide/operations.md` § "Switching LLMs"
  already covers `tools/acva-models select llm <alias>`. Add a
  paragraph: *"If you've installed the optional model-controller
  sidecar, this triggers a hot model swap without restarting
  acva. To check whether the sidecar is wired in: …"*.

Recommended: **#1 first** — extending `/status` is small (M8A
already wires the sidecar client into bootstrap), surfaces
state to the existing JSON consumer, and the Grafana panel from
#2 falls out naturally.

## How to verify the symptom

```sh
# Symptom (sidecar not running):
curl -fsS http://127.0.0.1:9876/status | jq '.model_controller // "no field"'
# → "no field"  (the bug — should at least say {reachable:false})

# Sidecar running but stale URL:
curl -fsS http://127.0.0.1:9877/health || echo "sidecar unreachable"
```

## Related

- CLAUDE.md M8A Step 5 ("End-to-end smoke of the model-controller
  is deferred to a manual dogfood pass once the sidecar is wired
  into the user's compose stack")
- `docs/history/MVP/milestones/m8a_admin_state.md` Step 5
- `src/llm/model_controller_client.cpp` — the handoff caller
- `src/orchestrator/boot/model_controller_handoff.cpp` — startup
  pre-call
- `packaging/model-controller/README.md` — how to wire it in
