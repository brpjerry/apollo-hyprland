#!/usr/bin/env bash
# Build the latency tools.
# Usage: build.sh OUT_DIR reactor|probe [WLR_PROTOCOLS_DIR]
# WLR_PROTOCOLS_DIR defaults to the wlr-protocols checkout CPM made in the repo's build/ tree (configure the project
# first, or pass it when building a copy of this directory on another machine).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$1"
tool="$2"
wlr="${3:-$here/../../../build/_deps/wlr_protocols-src/unstable}"
wp="$(pkg-config --variable=pkgdatadir wayland-protocols)"
mkdir -p "$out"

gen() {
  wayland-scanner client-header "$1" "$out/$2-client-protocol.h"
  wayland-scanner private-code "$1" "$out/$2-protocol.c"
}

case "$tool" in
  reactor)
    gen "$wp/stable/presentation-time/presentation-time.xml" presentation-time
    gen "$wp/stable/xdg-shell/xdg-shell.xml" xdg-shell
    gen "$wlr/wlr-layer-shell-unstable-v1.xml" wlr-layer-shell-unstable-v1
    cc -O2 -Wall -Wextra -I"$out" -o "$out/reactor" "$here/reactor.c" \
      "$out/presentation-time-protocol.c" "$out/xdg-shell-protocol.c" "$out/wlr-layer-shell-unstable-v1-protocol.c" \
      -lwayland-client
    ;;
  probe)
    gen "$wlr/wlr-screencopy-unstable-v1.xml" wlr-screencopy-unstable-v1
    cc -O2 -Wall -Wextra -I"$out" -o "$out/probe" "$here/probe.c" "$out/wlr-screencopy-unstable-v1-protocol.c" -lwayland-client
    ;;
  *)
    echo "usage: $0 OUT_DIR reactor|probe [WLR_PROTOCOLS_DIR]" >&2
    exit 2
    ;;
esac
echo "built $out/$tool"
