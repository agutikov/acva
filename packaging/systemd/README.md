# systemd unit files

Per-user systemd units for the acva runtime stack — the
**production-style alternative** to `packaging/compose/`. The dev
default is Docker Compose (`scripts/dev-up.sh`); this directory
matters when:

- You don't want a Docker dependency on the host.
- You're packaging acva for a distro (the units copy verbatim).
- You're running on a headless / shared host that already has
  systemd as the supervisor.

The orchestrator binary itself is the same in both paths — only the
backend supervision strategy differs.

## Files

| File | Role | Notes |
|---|---|---|
| `acva-llama.service`    | LLM backend (llama.cpp)        | listens on `127.0.0.1:8081`. CUDA build expected. |
| `acva-speaches.service` | STT + TTS backend (Speaches, M4B) | listens on `127.0.0.1:8090`. Replaces the pre-M4B `acva-whisper.service` + `acva-piper.service` pair. |
| `acva.service`          | Orchestrator                   | listens on `127.0.0.1:9876` (control plane). Depends on the two backends. |
| `acva.target`           | Convenience target             | brings up all three units in dependency order. |
| `nvidia-cdi-refresh.service` | NVIDIA CDI spec regenerator | **System-level**, not per-user. Regenerates `/etc/cdi/nvidia.yaml` on every boot before `docker.service` so Docker's CDI runtime picks up the kernel's current `nvidia-uvm` device majors. Without this, `cuInit` returns 999 inside containers after some reboots and llama / Speaches silently fall back to CPU. See `docs/guide/troubleshooting.md` § "All NNs run on CPU after reboot". Useful in the systemd path too if you also run the observability compose stack against the GPU. |

The pre-M4B `acva-whisper.service` + `acva-piper.service` pair was
removed in M8C Step 2 — Speaches consolidates STT + TTS behind one
OpenAI-API-compatible surface, so two units became one.

## Path conventions used inside the units

The units use `%h` (systemd specifier expanding to the user's home)
for portability. They assume the operator has manually installed:

```
%h/.local/opt/llama.cpp/build/bin/llama-server
%h/.local/opt/speaches/venv/bin/uvicorn       # `pip install speaches` into the venv
%h/.local/bin/acva                            # built from this repo
%h/.local/share/acva/models/llama.cpp/<file>.gguf
%h/.local/share/acva/models/speaches/         # HuggingFace cache (HF_HOME)
%h/.config/acva/config.yaml                   # cfg.* (typically a copy of config/default.yaml)
%h/.config/acva/llama.env                     # ACVA_LLM_MODEL, ACVA_LLM_ALIAS (read by acva-llama.service)
%h/.config/acva/speaches.env                  # WHISPER__INFERENCE_DEVICE, COMPUTE_TYPE, TTL (optional)
```

Edit any `ExecStart` path that doesn't match your layout.

## Install (per-user)

The fastest way is the helper script:

```sh
scripts/install-systemd.sh           # copies units + daemon-reload
systemctl --user enable --now acva.target
```

Or manually:

```sh
mkdir -p ~/.config/systemd/user
cp packaging/systemd/acva.{service,target} packaging/systemd/acva-llama.service \
   packaging/systemd/acva-speaches.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now acva.target
```

To survive logout on a headless server: `sudo loginctl enable-linger "$USER"`.

To uninstall:

```sh
scripts/uninstall-systemd.sh
```

## Install (system-level NVIDIA CDI refresh)

`nvidia-cdi-refresh.service` is the one unit in this directory that
must be installed system-wide — it writes `/etc/cdi/nvidia.yaml` and
must run before `docker.service` on each boot:

```sh
sudo cp packaging/systemd/nvidia-cdi-refresh.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nvidia-cdi-refresh.service
```

Verify after the next reboot: `journalctl -u nvidia-cdi-refresh.service`
should show a single successful `nvidia-ctk cdi generate` invocation,
and llama logs should show `ggml_cuda_init: found 1 CUDA devices`
rather than `failed to initialize CUDA: unknown error`. Requires
`nvidia-utils` (provides `nvidia-modprobe`) and
`libnvidia-container-tools` (provides `nvidia-ctk`).

## Install (system-wide, for headless / shared hosts)

1. Move binaries to `/opt/acva/` (or similar root-owned path) and
   edit each `ExecStart` line accordingly.
2. Create a dedicated `acva` system user and add `User=acva` and
   `Group=acva` under each `[Service]` section.
3. Replace every `%h` with the absolute home path (e.g.,
   `/var/lib/acva`).
4. Copy units to `/etc/systemd/system/` instead of
   `~/.config/systemd/user/`.
5. `sudo systemctl daemon-reload && sudo systemctl enable --now acva.target`.
6. In the orchestrator's YAML config, set `supervisor.bus_kind: system`
   (post-M8A; the sd-bus client picks up the right bus).

## Quick reference

```sh
# bring everything up
systemctl --user start acva.target

# health checks
curl -sS http://127.0.0.1:8081/health   # llama
curl -sS http://127.0.0.1:8090/health   # speaches (STT + TTS)
curl -sS http://127.0.0.1:9876/status   # orchestrator

# logs
journalctl --user -fu acva.service
journalctl --user -fu acva-llama.service
journalctl --user -fu acva-speaches.service

# stop the stack
systemctl --user stop acva.target

# disable autostart
systemctl --user disable acva.target acva.service \
  acva-llama.service acva-speaches.service
```

## Validation status

The units in this directory are **structural placeholders** —
syntactically valid systemd, with paths that match the canonical
dev-box layout. End-to-end validation on a clean Manjaro / Ubuntu
24.04 box is the M8C acceptance gate (Step 2 in
`docs/history/MVP/milestones/m8c_distribution.md`). Production deployments
should test on a fresh VM before relying on these.
