#!/usr/bin/env bash
# Configure and build Apollo (with unit tests) on Arch Linux + Hyprland.
# Usage: [JOBS=n] [CMAKE_EXTRA_ARGS="-DFOO=BAR ..."] scripts/hyprland/build.sh [build-dir]
# CMAKE_EXTRA_ARGS is split on whitespace and appended to the first configure, so it can override the defaults
# below (e.g. CMAKE_EXTRA_ARGS="-DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF" without CUDA). It is ignored
# once the build directory has been configured; delete build.ninja to reconfigure.
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/build}"
read -ra extra_args <<< "${CMAKE_EXTRA_ARGS:-}"
if [[ ! -f "$build/build.ninja" ]]; then
  cmake -S "$src" -B "$build" -G Ninja -Wno-dev \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=ON \
    -DBOOST_ALLOW_NEWER_SYSTEM=ON \
    -DSUNSHINE_ENABLE_CUDA=ON \
    -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
    -DSUNSHINE_ASSETS_DIR=share/apollo \
    "${extra_args[@]}"
fi
ninja -C "$build" -j "${JOBS:-$(nproc)}"
