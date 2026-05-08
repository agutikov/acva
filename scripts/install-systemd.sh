#!/usr/bin/env bash
# acva install-systemd — copy per-user systemd units to
# ~/.config/systemd/user/ and daemon-reload.
#
# Usage:
#   ./scripts/install-systemd.sh           # install (per-user)
#   ./scripts/install-systemd.sh -h        # help
#
# What this script does NOT do:
#   - Install the orchestrator binary. Build with `./build.sh release`
#     and copy `_build/release/acva` to `~/.local/bin/acva` yourself.
#   - Install backend binaries (llama.cpp, Speaches). See
#     `packaging/systemd/README.md` for the canonical layout.
#   - Touch `nvidia-cdi-refresh.service` (that one's system-wide;
#     install separately per the README).
#   - Enable autostart. Run `systemctl --user enable --now acva.target`
#     after install, when you're ready.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
src="$repo_root/packaging/systemd"
dst="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *)         echo "install-systemd: unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$dst"

# Per-user units only. The system-wide nvidia-cdi-refresh unit is
# documented in packaging/systemd/README.md and intentionally not
# touched here.
for unit in acva-llama.service acva-speaches.service acva.service acva.target; do
    cp -v "$src/$unit" "$dst/$unit"
done

systemctl --user daemon-reload
echo
echo "install-systemd: units copied to $dst"
echo "                  daemon-reload completed."
echo
echo "Next steps:"
echo "  1. Make sure backend binaries exist (see packaging/systemd/README.md)."
echo "  2. systemctl --user enable --now acva.target"
echo "  3. journalctl --user -fu acva.target"
