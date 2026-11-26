#!/usr/bin/env bash
# Configure and build Apollo (with unit tests) on Arch Linux + Hyprland.
# Usage: [JOBS=n] [TARGETS="t1 t2"] [CMAKE_EXTRA_ARGS="-DFOO=BAR ..."] scripts/hyprland/build.sh [build-dir]
# CMAKE_EXTRA_ARGS is split on whitespace and appended to the first configure, so it can override the defaults
# below (e.g. CMAKE_EXTRA_ARGS="-DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF" without CUDA). It is ignored
# once the build directory has been configured; delete build.ninja to reconfigure. TARGETS limits the ninja targets.
#
# Memory safety (a set of parallel builds once exhausted RAM and swap and took the desktop down):
# - Only one build runs at a time on this machine, across all checkouts and worktrees: the build waits for
#   $APOLLO_BUILD_LOCK (default $XDG_RUNTIME_DIR/apollo-hyprland-build.lock).
# - The build runs in a systemd user scope with a hard memory cap and no swap (BUILD_MEMORY_MAX, default 16G;
#   BUILD_MEMORY_HIGH, default 12G, throttles before the cap). If it runs out, the kernel kills the build, not the
#   desktop. Set BUILD_NO_SCOPE=1 only where systemd is not available (CI).
# - Jobs default to min(nproc, 8, BUILD_MEMORY_MAX / 2 GiB), at nice 10. Don't call ninja or cmake --build directly.
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/build}"
read -ra extra_args <<< "${CMAKE_EXTRA_ARGS:-}"
read -ra targets <<< "${TARGETS:-}"

memory_max="${BUILD_MEMORY_MAX:-16G}"
memory_high="${BUILD_MEMORY_HIGH:-12G}"
to_gib() {
  local v="${1^^}"
  case "$v" in
    *G) echo "${v%G}" ;;
    *M) echo $(( ${v%M} / 1024 )) ;;
    *) echo $(( v / 1024 / 1024 / 1024 )) ;;
  esac
}
default_jobs=$(nproc)
(( default_jobs > 8 )) && default_jobs=8
mem_jobs=$(( $(to_gib "$memory_max") / 2 ))
(( mem_jobs < 1 )) && mem_jobs=1
(( default_jobs > mem_jobs )) && default_jobs=$mem_jobs
jobs="${JOBS:-$default_jobs}"

use_scope=1
if [[ -n "${BUILD_NO_SCOPE:-}" ]]; then
  use_scope=0
elif ! command -v systemd-run >/dev/null || ! systemctl --user show-environment >/dev/null 2>&1; then
  echo "build.sh: WARNING: no systemd user manager; building WITHOUT a memory cap" >&2
  use_scope=0
fi
limited() {
  if (( use_scope )); then
    systemd-run --user --scope --quiet --collect -p MemoryMax="$memory_max" -p MemoryHigh="$memory_high" -p MemorySwapMax=0 \
      -- nice -n 10 "$@"
  else
    nice -n 10 "$@"
  fi
}

lock="${APOLLO_BUILD_LOCK:-${XDG_RUNTIME_DIR:-/tmp}/apollo-hyprland-build.lock}"
exec 9>"$lock"
if ! flock -n 9; then
  echo "build.sh: another Apollo build is running; waiting for it to finish ($lock)" >&2
  flock 9
fi

# The only submodule the build uses; CMake fetches the other dependencies with CPM (package-lock.cmake).
if [[ ! -f "$src/third-party/lizardbyte-common/CMakeLists.txt" ]]; then
  git -C "$src" submodule update --init --recursive --depth 1 third-party/lizardbyte-common
fi

if [[ ! -f "$build/build.ninja" ]]; then
  limited cmake -S "$src" -B "$build" -G Ninja -Wno-dev \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=ON \
    -DSUNSHINE_SYSTEM_WAYLAND_PROTOCOLS=ON \
    -DSUNSHINE_ENABLE_CUDA=ON \
    -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
    -DSUNSHINE_ASSETS_DIR=share/apollo \
    "${extra_args[@]}"
fi
echo "build.sh: building $build with $jobs jobs (memory cap $memory_max)" >&2
limited ninja -C "$build" -j "$jobs" "${targets[@]}"
