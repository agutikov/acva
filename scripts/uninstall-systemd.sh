#!/usr/bin/env bash
# acva uninstall-systemd — disable + remove the per-user systemd
# units installed by scripts/install-systemd.sh.
#
# Usage:
#   ./scripts/uninstall-systemd.sh         # disable + remove unit files
#   ./scripts/uninstall-systemd.sh -h
#
# Idempotent: missing units are silently skipped.

set -euo pipefail

dst="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *)         echo "uninstall-systemd: unknown argument: $1" >&2; exit 2 ;;
    esac
done

units=(acva-llama.service acva-speaches.service acva.service acva.target)

# Stop + disable first so we don't leave a half-active target.
for unit in "${units[@]}"; do
    if [[ -f "$dst/$unit" ]]; then
        systemctl --user disable --now "$unit" 2>/dev/null || true
    fi
done

for unit in "${units[@]}"; do
    if [[ -f "$dst/$unit" ]]; then
        rm -v "$dst/$unit"
    fi
done

systemctl --user daemon-reload
echo "uninstall-systemd: per-user units removed from $dst"
