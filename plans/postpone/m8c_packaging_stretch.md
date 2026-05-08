# M8C — Packaging Stretch

**Estimate:** 2–4 days, fragmented per item.

**Depends on:** M8C Step 2 (Compose + systemd packaging — landed
2026-05-08). All items here are independent of one another and of
the dev-workstation acceptance gates.

**Blocks:** none. The acva runtime ships and dogfoods on the dev
workstation without any of these. They become load-bearing only at
"distribute to operators who didn't write it" time — a post-MVP cut.

## Why this is postponed

M8C's goal was "code-complete + delivery surface for one operator".
That bar is met: Compose stack works end-to-end, systemd units pass
`systemd-analyze verify` and round-trip cleanly, `tools/acva-models`
drives model assets, observability stack auto-provisions, and the
docs (README, architecture, configuration, operations) are written.

These remaining items are about **distributing to other operators**
on **other distros** with **stricter reproducibility** — and each
one has a non-trivial "does this still build" maintenance cost that
isn't worth paying until there's a second user. They're parked here
so they don't fall off the radar, but they're explicitly **not**
in M8C's acceptance scope.

## Items

### 1. Image digest pinning in `packaging/compose/docker-compose.yml`

Currently:

```yaml
services:
  llama:
    image: ghcr.io/ggml-org/llama.cpp:server-cuda      # rolling tag
  speaches:
    image: ghcr.io/speaches-ai/speaches:latest-cuda    # rolling tag
```

Reproducible-build territory. Pin to `@sha256:...` digests so a
fresh `docker compose pull` on a teammate's box gets exactly the
same binaries. Trade-off: digest pins drift behind upstream security
patches; bumping requires a deliberate test pass.

**Done when:** both image lines carry an `@sha256:...` digest, and
`scripts/dev-up.sh` documents the bump procedure ("test on local;
update digest; commit").

### 2. AUR `PKGBUILD` for Arch / Manjaro

A `packaging/aur/acva/PKGBUILD` so an Arch operator can `yay -S
acva` instead of `git clone && ./build.sh && cp`. The build itself
is straightforward — depends on system packages already named in
`README.md` install instructions. The hard part is upstream'ing to
AUR and keeping it synced with releases.

**Done when:** `makepkg -si` from a clean checkout produces a working
`/usr/bin/acva` + units in `/usr/lib/systemd/user/` + config skeleton
in `/usr/share/acva/`. Optionally: submit to AUR under
`acva-git` (rolling) and `acva` (release-tag-tracking).

### 3. `.deb` build script for Debian / Ubuntu

`packaging/deb/build.sh` driving `debuild` / `dpkg-buildpackage`
against the project. Same shape as the AUR work but for the
APT world. Some dep names differ (`libwebrtc-audio-processing-dev`
vs Arch's `webrtc-audio-processing`), and a couple of opentelemetry
+ glaze deps need source-build instructions because they're not in
`apt` repos as of Ubuntu 24.04.

**Done when:** `./packaging/deb/build.sh` produces a `.deb` that
installs cleanly on a fresh 24.04 LTS, and the post-install
verification (`acva --help` + `acva demo health` against a stock
Compose stack) passes.

### 4. Compose project-label cleanup

`packaging/observability/docker-compose.yml` declares
`name: acva-observability`, but compose-v2's `--project-directory`
flag overrides that, so containers come up labelled `project=acva`
and collide with the inference stack's project. `dev-up.sh` works
around it by polling specific container names instead of the
project label, but that's friction.

**Done when:** observability stack uses an unambiguous project name
(`acva-obs` or similar) such that `docker compose -p acva-obs ps`
shows only Prometheus + Grafana, and `docker compose -p acva ps`
shows only llama + speaches. `dev-up.sh` / `dev-down.sh` updated to
match.

### 5. `packaging/man/acva.1` man page

Listed in the original M8C Step 2 spec; deferred because `acva
--help` is complete and `docs/guide/operations.md` covers the same ground.
A man page is the right interface for `man acva` and for distro
packaging (Lintian / Arch packaging guidelines flag binaries
without one).

**Done when:** `man acva` renders cleanly after install and matches
`acva --help` output. Probably auto-generate via `help2man` so it
stays in sync.

### 6. Bare-metal acceptance on fresh-VM Manjaro / Ubuntu 24.04

The original M8C acceptance gate "both deployment paths work
end-to-end on a clean Manjaro and a clean Ubuntu 24.04 VM" was
narrowed at M8C close to "dev workstation only" because (a)
operators on those distros use the Compose path, and (b) the
bare-metal install of `llama-server` + Speaches isn't validated
end-to-end on a fresh VM. That validation pass is real work — at
least a half-day per VM, and discovers OS-specific surprises.

**Done when:** a clean install of each distro can run `git clone &&
./build.sh && ./scripts/install-systemd.sh && systemctl --user
start acva.target`, and `acva demo health` reports both backends
healthy. Document any VM-specific gotchas in
`packaging/systemd/README.md`.

## When to revive

Revive when **any** of:

- A second operator (not the original author) tries to install
  acva on a non-dev workstation and hits friction. AUR or `.deb`
  graduates from "nice to have" to "required for adoption."
- The Compose images upstream introduce a regression. Digest
  pinning becomes "real work blocking release" rather than
  "reproducibility hygiene."
- A distro packaging policy review catches something. The man
  page becomes load-bearing for the package itself.

Until then, parked.
