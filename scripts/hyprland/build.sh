#!/usr/bin/env bash
# Configure and build Apollo (with unit tests) on Arch Linux + Hyprland.
# Usage: scripts/hyprland/build.sh [build-dir]
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/build}"
if [[ ! -f "$build/build.ninja" ]]; then
  cmake -S "$src" -B "$build" -G Ninja -Wno-dev \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=ON \
    -DBOOST_ALLOW_NEWER_SYSTEM=ON \
    -DSUNSHINE_ENABLE_CUDA=ON \
    -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
    -DSUNSHINE_ASSETS_DIR=share/apollo
fi
ninja -C "$build" -j "${JOBS:-$(nproc)}"
