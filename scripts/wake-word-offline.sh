#!/usr/bin/env bash
# scripts/wake-word-offline.sh
#
# Records ${DURATION}s of audio simultaneously from:
#   - the raw default mic source (no AEC)
#   - the acva AEC source (PipeWire module-echo-cancel sink/source pair)
#
# Then runs each capture through `acva demo wake-word-offline`, which
# transcribes via Speaches and scores via the openWakeWord engine. Lets
# you tell apart "mic is silent", "AEC over-cancels", and "model genuinely
# missed the phrase" when the live `acva demo wake-word` shows max≈0.
#
# Usage:
#     scripts/wake-word-offline.sh [duration_s]
#
# Requires: parecord (pipewire-pulse), pactl. acva must be built.
# If no acva-echo-source is loaded, the script loads module-echo-cancel
# itself and unloads it on exit.

set -euo pipefail

DURATION="${1:-15}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACVA="${ACVA_BIN:-$ROOT/_build/dev/acva}"

if [[ ! -x "$ACVA" ]]; then
    echo "FATAL: acva binary not found at $ACVA — run ./build.sh first" >&2
    exit 1
fi

OUTDIR="$(mktemp -d -t acva-ww-offline-XXXXXX)"
LOADED_MODULE_ID=""

cleanup() {
    if [[ -n "$LOADED_MODULE_ID" ]]; then
        echo "Unloading module-echo-cancel (id=$LOADED_MODULE_ID)…"
        pactl unload-module "$LOADED_MODULE_ID" 2>/dev/null || true
    fi
    echo "Recordings preserved at: $OUTDIR"
}
trap cleanup EXIT

echo "=== sources ==="
pactl list short sources

# Resolve the raw default mic. If the user has the AEC source set as
# default (which acva does at runtime via system_aec), pick the first
# alsa_input.* instead.
RAW_SRC="$(pactl get-default-source)"
if [[ "$RAW_SRC" == acva-echo-source* || "$RAW_SRC" == *.echo-cancel* ]]; then
    RAW_SRC="$(pactl list short sources \
        | awk '$2 ~ /^alsa_input\./ { print $2; exit }')"
fi
if [[ -z "$RAW_SRC" ]]; then
    echo "FATAL: could not resolve a raw alsa_input.* source" >&2
    exit 1
fi
echo "raw source: $RAW_SRC"

# Find or create an AEC source. We mimic the names system_aec uses so
# a stale module from a crashed acva is reused rather than duplicated.
AEC_SRC="$(pactl list short sources \
    | awk '$2 ~ /^acva-echo-source/ { print $2; exit }')"
if [[ -z "$AEC_SRC" ]]; then
    echo "loading module-echo-cancel (no acva-echo-source present)…"
    LOADED_MODULE_ID="$(pactl load-module module-echo-cancel \
        source_name=acva-echo-source \
        sink_name=acva-echo-sink \
        aec_method=webrtc \
        source_master="$RAW_SRC" 2>&1 || true)"
    # pactl prints either a numeric id or an error message.
    if [[ "$LOADED_MODULE_ID" =~ ^[0-9]+$ ]]; then
        AEC_SRC="acva-echo-source"
        echo "loaded module id=$LOADED_MODULE_ID; AEC source=$AEC_SRC"
    else
        echo "WARN: could not load module-echo-cancel ($LOADED_MODULE_ID) — "\
             "skipping AEC capture" >&2
        LOADED_MODULE_ID=""
        AEC_SRC=""
    fi
else
    echo "AEC source already present: $AEC_SRC"
fi

RAW_WAV="$OUTDIR/raw.wav"
AEC_WAV="$OUTDIR/aec.wav"

echo
echo "=== recording ${DURATION}s — speak the wake word now ==="

# Record both in parallel so the same speech lands in both files.
parecord --device="$RAW_SRC" \
         --rate=16000 --channels=1 --format=s16le \
         --file-format=wav "$RAW_WAV" &
RAW_PID=$!
AEC_PID=""
if [[ -n "$AEC_SRC" ]]; then
    parecord --device="$AEC_SRC" \
             --rate=16000 --channels=1 --format=s16le \
             --file-format=wav "$AEC_WAV" &
    AEC_PID=$!
fi

sleep "$DURATION"

kill "$RAW_PID" 2>/dev/null || true
[[ -n "$AEC_PID" ]] && kill "$AEC_PID" 2>/dev/null || true
wait "$RAW_PID" 2>/dev/null || true
[[ -n "$AEC_PID" ]] && wait "$AEC_PID" 2>/dev/null || true

run_demo() {
    local label="$1" wav="$2"
    if [[ ! -s "$wav" ]]; then
        echo "skipping $label — '$wav' missing or empty"
        return
    fi
    echo
    echo "================================================================"
    echo "  $label  ($wav)"
    echo "================================================================"
    "$ACVA" demo wake-word-offline --wav "$wav"
}

run_demo "RAW MIC (no AEC)" "$RAW_WAV"
[[ -n "$AEC_SRC" ]] && run_demo "AEC SOURCE" "$AEC_WAV"

echo
echo "Done. WAVs preserved at $OUTDIR (re-run on them with:"
echo "  $ACVA demo wake-word-offline --wav $RAW_WAV)"
