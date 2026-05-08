#!/usr/bin/env bash
# acva dev-up — bring up the local inference stack.
#
# Usage:
#   ./scripts/dev-up.sh              # default: bring up llama + speaches
#   ./scripts/dev-up.sh --no-status  # skip the post-up `acva-models status` check
#   ./scripts/dev-up.sh -h | --help
#
# What it does:
#   1. If `.env` is missing, copy `.env.example` to `.env` with the
#      `__SET_HOME__` sentinel substituted to your actual $HOME.
#   2. If `.env` still contains the sentinel (manual `cp` without
#      edits), bail with a clear pointer.
#   3. `docker compose up -d` from packaging/compose/.
#   4. Wait briefly for both containers to report healthy.
#   5. Run `tools/acva-models status` so a missing model in
#      cfg.tts.voices / cfg.stt.model surfaces immediately.
#
# Why this exists: the M7-era flow assumes the operator manually
# edits `.env` after `cp .env.example .env`. The placeholder caused
# silent breakage 2026-05-08 — `${ACVA_MODELS_DIR}` substituted to a
# non-existent path, llama crash-looped without a clear cause. This
# script eliminates the manual step + adds the post-up sanity check.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

run_status=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-status)  run_status=0; shift ;;
        -h|--help)    sed -n '2,18p' "$0"; exit 0 ;;
        *)            echo "dev-up: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# 1. .env bootstrap.
if [[ ! -f .env ]]; then
    if [[ ! -f .env.example ]]; then
        echo "dev-up: neither .env nor .env.example present in $repo_root" >&2
        exit 2
    fi
    sed "s|__SET_HOME__|$HOME|g" .env.example > .env
    echo "dev-up: created .env from .env.example (resolved \$HOME → $HOME)"
fi

# 2. Sentinel check — refuses to proceed if the operator copied
#    .env.example by hand and never substituted.
if grep -q '__SET_HOME__' .env; then
    cat <<EOF >&2
dev-up: .env still contains the __SET_HOME__ sentinel.
        Either run \`scripts/dev-up.sh\` against a fresh checkout
        (delete .env and rerun this script), or manually replace
        every \`__SET_HOME__\` occurrence with your home directory:

          sed -i "s|__SET_HOME__|\$HOME|g" .env
EOF
    exit 1
fi

# 3. Compose up.
compose_dir="$repo_root/packaging/compose"
echo "dev-up: bringing up compose stack from $compose_dir"
docker compose -p acva --project-directory "$compose_dir" -f "$compose_dir/docker-compose.yml" \
    --env-file "$repo_root/.env" up -d

# 4. Wait for healthy. Compose v2 doesn't have a wait-for-healthy
#    flag for `up -d`, so poll. 60 s ceiling — both containers' own
#    healthchecks should converge well inside that window.
echo "dev-up: waiting for healthchecks to converge…"
deadline=$(( $(date +%s) + 60 ))
while (( $(date +%s) < deadline )); do
    # Only check the services this compose owns — grafana /
    # prometheus from the observability stack may share the same
    # docker labels (compose-v2 quirk with `--project-directory`),
    # so we ask by container name to stay scoped.
    llama=$(docker inspect --format '{{.State.Health.Status}}' acva-llama 2>/dev/null || echo missing)
    speaches=$(docker inspect --format '{{.State.Health.Status}}' acva-speaches 2>/dev/null || echo missing)
    if [[ "$llama" == "healthy" && "$speaches" == "healthy" ]]; then
        break
    fi
    sleep 2
done

docker compose -p acva --project-directory "$compose_dir" \
    -f "$compose_dir/docker-compose.yml" --env-file "$repo_root/.env" ps

# 5. Optional sanity check via the registry.
if (( run_status == 1 )) && [[ -x tools/acva-models ]]; then
    echo
    tools/acva-models status || true
fi

echo "dev-up: stack is up. Bring it down with scripts/dev-down.sh."
