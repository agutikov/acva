# acva — Configuration

The canonical reference for every field is `config/default.yaml`
itself — every option carries a comment block explaining what it
does, what its trade-offs are, and which milestone introduced it.
This doc covers the **structure**, **semantics**, and **operational
mechanics** that the inline comments don't: file location precedence,
how aliases resolve, how personalities override, what's hot-reloadable
vs. restart-required, and how paths are resolved.

## Where the config file lives

`acva` searches in this order when `--config` is omitted:

1. `${XDG_CONFIG_HOME:-~/.config}/acva/default.yaml`
2. `./config/default.yaml` (in-tree dev fallback)
3. `/etc/acva/default.yaml`

The first existing file wins. Override with `--config /path/to/file`.

The same precedence applies on warm restart, so editing
`~/.config/acva/default.yaml` and SIGHUP'ing or `POST /reload`-ing
picks up changes immediately for hot fields, or rejects the reload
with a 409 listing restart-required fields.

## File structure

Top-level sections, in the order they appear in `default.yaml`:

| Section            | Purpose |
|---|---|
| `models`           | Model registry. Aliases the rest of the config can reference (LLM, STT, TTS, VAD, wake-word). Source of truth for `tools/acva-models`. |
| `logging`          | Log level + sink (stderr / journal / file / dir-rotation). |
| `control`          | Where the orchestrator's HTTP control plane binds (`/status`, `/metrics`, `/reload`, `/wipe`, `/restart`). |
| `pipeline`         | Cross-cutting pipeline knobs (rare to touch). |
| `llm`              | `base_url`, `model` (alias), sampling (`temperature`, `top_p`, `repeat_penalty`, `max_tokens`), model-controller URL. |
| `stt`              | `base_url`, `model` (alias), `streaming`, `language` hint. |
| `tts`              | `base_url`, per-language `voices` (alias map), `tempo_wpm`, `fallback_lang`. |
| `audio`            | Input / output devices, sample rate, buffer size, capture enable, wake-word block, system-AEC toggles, ALSA full-probe skip, test WAV input. |
| `playback`         | `prefill_ms` and queue limits — how much TTS to buffer before playback starts. |
| `vad`              | Silero thresholds (`onset`, `offset`, `hangover_ms`, `min_speech_ms`, `pre/post_padding_ms`), model alias. |
| `utterance`        | Max duration / max in-flight clamps. |
| `apm`              | In-process WebRTC APM (off-by-default; system AEC is the default path). |
| `barge_in`         | `min_real_utterance_chars` RMS gate, AEC cooldown, half-duplex toggle. |
| `supervisor`       | `/health` probe interval + grace, watchdog stuck threshold, auto-restart, strict-startup gates. |
| `memory`           | SQLite path, summarizer settings. |
| `dialogue`         | `max_assistant_sentences`, `max_assistant_tokens`, `max_sentence_chars`, `max_tts_queue_sentences`. |
| `active_personality` | Which key from `personalities:` to apply as an overlay. |
| `personalities`    | Named overlays — system prompts (per language), voice selections, sampling, dialogue caps, optional wake-word threshold. |

## Path resolution

| Field                       | If empty / relative, resolves to |
|-----------------------------|---|
| `memory.db_path`            | `${XDG_DATA_HOME:-~/.local/share}/acva/<value>` (default `acva.db`) |
| `vad.model_path`            | Registry alias → `models/silero/<file>` under `${XDG_DATA_HOME}/acva/` |
| `audio.wake_word.model_paths[]` | Registry alias → `models/wake_word/<file>` under `${XDG_DATA_HOME}/acva/`. Bare filenames also resolve to that dir. Paths with subdirs are resolved relative to it. |
| `logging.file_path`         | Absolute path; relative resolves against CWD. |
| `logging.dir_path`          | `${XDG_DATA_HOME}/acva/log/` by default. |

Parent directories are auto-created on first run.

## The model registry

`config/default.yaml` carries a top-level `models:` block — catalogs
of short aliases per type:

```yaml
models:
  llm:
    qwen2.5-7b: { file: …, url: …, size: …, purpose: … }
    dialog:    { file: …, url: …, size: …, purpose: … }
  stt:
    large-v3-turbo: { id: deepdml/faster-whisper-large-v3-turbo-ct2, … }
  tts:
    en-amy:    { id: speaches-ai/piper-en_US-amy-medium, voice: amy, … }
    ru-denis:  { id: speaches-ai/piper-ru_RU-denis-medium, voice: denis, … }
  vad:
    silero-v5: { file: silero_vad.onnx, url: …, … }
  wake_word:
    _shared_melspectrogram: { file: …, url: …, … }
    _shared_embedding:      { file: …, url: …, … }
    hey-jarvis:             { file: …, url: …, … }
```

Subsystem fields then **reference aliases by name**:

```yaml
llm:    { model: dialog }
stt:    { model: large-v3-turbo }
tts:    { voices: { en: en-amy, ru: ru-denis } }
vad:    { model_path: silero-v5 }
audio.wake_word: { model_paths: [hey-jarvis] }
```

At config load (`config::resolve_aliases()`), aliases are rewritten to
backend-specific locators:

- **STT / TTS aliases** → HuggingFace ids (Speaches downloads them on
  demand via `POST /v1/models/<id>`).
- **LLM aliases** → kept as the alias label for now (passed to
  llama-server as the OpenAI-endpoint `model` name); M8A's
  model-controller sidecar will make the alias load-bearing by
  swapping the GGUF that llama-server has loaded.
- **VAD / wake-word aliases** → resolved filenames under
  `${XDG_DATA_HOME}/acva/models/<type>/`.

`tools/acva-models` reads the same registry block and installs the
matching assets:

```sh
tools/acva-models list                       # what's installed vs missing
tools/acva-models install large-v3-turbo     # one alias
tools/acva-models install --type tts --all   # whole catalog of one type
tools/acva-models sync                       # everything the active config references
tools/acva-models verify                     # file-size drift check
tools/acva-models select llm dialog          # switch active alias (writes both YAML + .env)
tools/acva-models status                     # end-to-end consistency check
```

The `_shared_*` entries in the wake-word catalog are infrastructure
(mel-spectrogram + embedding ONNX) that every classifier needs; they
get pulled automatically when you install any wake-word phrase.

## Personalities

A personality is a named overlay applied at config-load time on top of
the base config. The active personality is named in
`active_personality`; it must exist as a key under `personalities:`.

A personality can override:

- `system_prompts` — per-language; the dialogue manager picks the one
  matching the detected/hinted language.
- `voices` — per-language map of TTS aliases.
- `tempo_wpm` — TTS speaking rate.
- `llm.{temperature, top_p, repeat_penalty}` — sampling shape.
- `dialogue.{max_assistant_sentences, max_assistant_tokens, max_sentence_chars}` — turn shape.
- `wake_word.threshold` and/or `wake_word.model_paths` — wake gate
  tuning per persona.

Anything the personality doesn't override falls through to the
top-level value. `active_personality` is **restart-required** — see
the reload rules below.

## Hot-reloadable vs. restart-required

`POST /reload` and `SIGHUP` re-read the config file and apply changes:

- If only **hot-reloadable** fields changed, the orchestrator applies
  them in place and returns `200 OK`.
- If any **restart-required** field changed, the whole reload is
  rejected with `409 Conflict` and a list of offending paths. The
  orchestrator's running config is unchanged. Use `POST /restart`
  (M8A warm-restart) to apply, which preserves the active session and
  config-hash-gates the resume.
- If the YAML fails to parse, `400 Bad Request` with the parse error.

The catalog is hand-written in `src/config/reload.cpp`; below is its
current state.

### Hot-reloadable fields

These are picked up live without bouncing the process.

| Field                              | Why this is safe |
|---|---|
| `llm.temperature`                  | Per-request sampling param; takes effect next turn. |
| `llm.max_tokens`                   | Same. |
| `dialogue.max_assistant_sentences` | Read each turn by Manager. |
| `dialogue.max_tts_queue_sentences` | Read each turn by Manager. |
| `vad.onset_threshold`              | Endpointer reads it under a mutex; concurrent reloads don't tear. |
| `vad.offset_threshold`             | Same. |
| `vad.hangover_ms`                  | Same. |
| `tts.tempo_wpm`                    | Per-request param to Speaches. |
| `logging.level`                    | spdlog level switched in place. |
| `audio.wake_word.threshold`        | Atomic read on the audio worker thread. |

### Restart-required fields

The full list, grouped by reason:

- **Endpoints & model identities** — long-lived sockets / per-request
  state would need reconnecting clients to honor the new value:
  `llm.base_url`, `llm.model`, `stt.base_url`, `stt.model`,
  `stt.streaming`, `stt.language`, `tts.base_url`, `tts.voices`,
  `tts.fallback_lang`.
- **Filesystem / DB** — opening a different DB or model file
  mid-flight invalidates everything in memory: `memory.db_path`,
  `vad.model_path`.
- **Audio device & pipeline graph** — reopening the PortAudio
  stream or rebuilding the pipeline is a structural change:
  `audio.input_device`, `audio.output_device`, `audio.capture_enabled`,
  `audio.sample_rate_hz`, `audio.buffer_frames`.
- **Wake-word topology** — toggling the gate or swapping ONNX
  models mid-run is risky on the audio worker thread:
  `audio.wake_word.enabled`, `audio.wake_word.model_paths`,
  `audio.wake_word.followup_window_ms`. (`audio.wake_word.threshold`
  is hot.)
- **Control plane** — moving the listening socket requires reopening:
  `control.bind`, `control.port`.
- **Personality** — re-applying the overlay safely without an
  orchestrator-wide restart is future work (M8A Step 5 territory):
  `active_personality`.
- **Logging sink topology** — `logging.sink`, `logging.file_path`,
  `logging.dir_path`. (`logging.level` is hot.)

Anything not in either list is treated as restart-required by
default — adding a new hot field requires explicitly registering it
in `reload.cpp`.

## Examples

Lower wake-word threshold for accented speech without bouncing acva:

```sh
sed -i 's/threshold: 0.55/threshold: 0.45/' ~/.config/acva/default.yaml
curl -fsS -X POST http://127.0.0.1:9876/reload
# Or: kill -HUP $(pgrep -x acva)
```

Switch to a different LLM (restart-required):

```sh
tools/acva-models select llm socratic   # writes YAML + .env, recreates compose container
# acva picks up the change on next start; for a live process, hit /restart:
curl -fsS -X POST http://127.0.0.1:9876/restart
```

Try a parse-only check (`load_from_string` is what the orchestrator
runs at startup):

```sh
./_build/dev/acva --config /tmp/draft.yaml --check
# (exits 0 on parse + validation success; prints diagnostics on failure)
```

## See also

- `config/default.yaml` — every field with inline comments. The
  canonical reference.
- `src/config/config.hpp` — C++ structs, default values.
- `src/config/reload.cpp` — the hand-written hot/restart catalog.
- `docs/history/MVP/open_questions.md` §L8 — why wake-word ships `enabled: false`
  by default.
- `docs/history/MVP/open_questions.md` §L7 — why `apm.aec_enabled: false` and
  `apm.use_system_aec: true` are the defaults on this hardware class.
