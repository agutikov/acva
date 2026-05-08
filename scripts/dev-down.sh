#!/usr/bin/env bash
# acva dev-down — tear down the local inference stack.
#
# Usage:
#   ./scripts/dev-down.sh              # stop containers, keep volumes
#   ./scripts/dev-down.sh --wipe       # also remove anonymous volumes
#                                      #   (does NOT touch ACVA_MODELS_DIR
#                                      #   — that's a host bind-mount,
#                                      #   never managed by compose)
#   ./scripts/dev-down.sh -h | --help
#
# Symmetric counterpart to scripts/dev-up.sh. The observability
# stack (packaging/observability/) is independent — bring it down
# separately if needed.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

wipe=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wipe)     wipe=1; shift ;;
        -h|--help)  sed -n '2,16p' "$0"; exit 0 ;;
        *)          echo "dev-down: unknown argument: $1" >&2; exit 2 ;;
    esac
done

compose_dir="$repo_root/packaging/compose"
env_file="$repo_root/.env"

extra=()
[[ -f "$env_file" ]] && extra+=(--env-file "$env_file")
(( wipe == 1 )) && extra+=(-v) || true

# `compose down` without -v removes containers + the default network
# but preserves named/anonymous volumes (e.g. observability/'s
# prometheus-data, grafana-data — though those live in a separate
# project anyway). With --wipe → -v passed through.
docker compose -p acva --project-directory "$compose_dir" -f "$compose_dir/docker-compose.yml" \
    "${extra[@]}" down $( (( wipe == 1 )) && echo -v )

if (( wipe == 1 )); then
    echo "dev-down: stack down + anonymous volumes removed."
else
    echo "dev-down: stack down. Volumes preserved (use --wipe to clear)."
fi
