# model-controller

M8A Step 5 sidecar — owns the docker-socket privilege so `acva` itself
can stay free of it. Receives `POST /llm/load` from `acva` at startup
and recreates the `llama` compose service with the requested GGUF.

## Surface

```
GET  /llm/status   → {"loaded_file":"…","alias":"…","health":"…"}
POST /llm/load     body {"file":"<gguf-filename>"}   202|200|409|5xx
GET  /health       → "ok\n"
```

Bound to `127.0.0.1:9877` by default (`LISTEN` env to override).

## Build + run

Local debug build (host Go toolchain):

```sh
cd packaging/model-controller
go run . &
curl http://127.0.0.1:9877/llm/status
```

Container build:

```sh
docker build -t acva-model-controller packaging/model-controller
docker run --rm -p 127.0.0.1:9877:9877 \
    -v /var/run/docker.sock:/var/run/docker.sock \
    -v "$(pwd)/packaging/compose:/compose" \
    -e COMPOSE_DIR=/compose \
    -e LLAMA_HEALTH=http://host.docker.internal:8081/health \
    acva-model-controller
```

## Compose integration

The service is **not** added to `packaging/compose/docker-compose.yml`
by default — wiring it up touches the host docker socket and that's a
deliberate operator opt-in. Add the following stanza to the compose
file when you want it:

```yaml
  model-controller:
    build: ../model-controller
    container_name: acva-model-controller
    ports:
      - "127.0.0.1:9877:9877"
    volumes:
      - /var/run/docker.sock:/var/run/docker.sock
      - ./:/compose
    environment:
      COMPOSE_DIR: /compose
      LLAMA_HEALTH: http://llama:8081/health
    restart: unless-stopped
```

Then set in your `config/default.yaml`:

```yaml
llm:
  model_file: dialog-7B-q4.gguf      # registry filename or raw GGUF
  model_controller_url: http://127.0.0.1:9877
```

## Environment

| Var            | Default                          | Notes                                   |
|----------------|----------------------------------|-----------------------------------------|
| `LISTEN`       | `127.0.0.1:9877`                 | Bind address                            |
| `COMPOSE_DIR`  | `/compose`                       | Where the compose project root is mounted |
| `LLAMA_HEALTH` | `http://llama:8081/health`       | Probed after the recreate               |
| `LOAD_TIMEOUT` | `60` (seconds)                   | Cap on the recreate + health-poll wait  |

## Failure modes the controller surfaces

- `400` — missing or malformed body
- `409` — concurrent `/llm/load` already in flight
- `500` — `docker compose` failed (full stderr is logged)
- `504` — recreate succeeded but llama didn't report healthy in time

`acva` reports each of these as a `ClientError` from
`ModelControllerClient::load`; under `cfg.supervisor.strict_startup`
the orchestrator exits non-zero, otherwise it logs and continues with
whatever model llama already had loaded.
