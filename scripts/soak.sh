#!/usr/bin/env bash
# acva soak — M8B Step 1.
#
# Runs the 4-hour soak harness defined in
# `plans/milestones/m8b_observability.md`. Spawns `acva --stdin` via
# scripts/soak-driver.py, which feeds prompts, polls /metrics, and
# auto-restarts the speaches container on the wedge metric.
#
# Usage:
#   scripts/soak.sh [--duration 4h|240s|0] [--output DIR] [--config PATH]
#                   [--seed N] [--acva PATH]
#
# Defaults match the milestone plan: 4 h, output under
# `tests/soak/reports/<isodate>/`, config resolved by acva's normal
# XDG search.

set -euo pipefail

# --- defaults -----------------------------------------------------
duration_arg="4h"
output=""
config=""
acva_bin="$(dirname "$0")/../_build/dev/acva"
seed=0

# --- arg parsing --------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) duration_arg="$2"; shift 2 ;;
        --output)   output="$2";       shift 2 ;;
        --config)   config="$2";       shift 2 ;;
        --acva)     acva_bin="$2";     shift 2 ;;
        --seed)     seed="$2";         shift 2 ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *)
            echo "soak: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# --- duration parser ---------------------------------------------
# Accepts: 4h | 30m | 240s | 14400 (bare seconds).
parse_duration() {
    local raw="$1"
    case "$raw" in
        *h) echo $(( ${raw%h} * 3600 )) ;;
        *m) echo $(( ${raw%m} * 60 )) ;;
        *s) echo "${raw%s}" ;;
        *)  echo "$raw" ;;
    esac
}
duration_sec="$(parse_duration "$duration_arg")"
if ! [[ "$duration_sec" =~ ^[0-9]+$ ]] || (( duration_sec < 1 )); then
    echo "soak: invalid --duration '$duration_arg' (use 4h, 30m, 240s, or seconds)" >&2
    exit 2
fi

# --- output dir --------------------------------------------------
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "$output" ]]; then
    output="$repo_root/tests/soak/reports/$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$output"

# --- preflight ---------------------------------------------------
if [[ ! -x "$acva_bin" ]]; then
    echo "soak: acva binary not found / not executable at $acva_bin" >&2
    echo "      build with `./build.sh dev` first." >&2
    exit 2
fi

driver="$(dirname "$0")/soak-driver.py"
if [[ ! -f "$driver" ]]; then
    echo "soak: driver not found at $driver" >&2
    exit 2
fi

# --- run ---------------------------------------------------------
echo "soak: duration   = ${duration_sec}s ($duration_arg)"
echo "soak: output dir = $output"
echo "soak: acva       = $acva_bin"
[[ -n "$config" ]] && echo "soak: config     = $config"

driver_args=(
    --duration "$duration_sec"
    --output "$output"
    --acva "$acva_bin"
    --seed "$seed"
)
[[ -n "$config" ]] && driver_args+=(--config "$config")

# Forward SIGINT/SIGTERM to the driver so a Ctrl-C produces a
# partial report instead of a half-killed process tree.
exec python3 "$driver" "${driver_args[@]}"
