#!/usr/bin/env bash
# Run the build-tree Apollo binary with its assets (web UI, GL shaders) visible at the compiled-in
# /usr/local/share/apollo, without installing anything: build/assets is bind-mounted there inside a
# private user+mount namespace. Without this, NVENC/VAAPI fall back to software because the shaders
# are not found.
# Usage: scripts/hyprland/run-from-build.sh [build-dir] -- <sunshine args, e.g. path/to/sunshine.conf>
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="$src/build"
if [[ $# -gt 0 && "$1" != "--" ]]; then build="$(cd "$1" && pwd)"; shift; fi
[[ "${1:-}" == "--" ]] && shift
share="$(mktemp -d -t apollo-share.XXXXXX)"
trap 'rm -f "$share/apollo"; rmdir "$share"' EXIT
ln -s "$build/assets" "$share/apollo"
unshare --user --map-current-user --keep-caps --mount \
  sh -c 'mount --bind "$1" /usr/local/share && cd "$2" && shift 2 && exec ./sunshine "$@"' sh "$share" "$build" "$@"
