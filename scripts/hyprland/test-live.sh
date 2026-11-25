#!/usr/bin/env bash
# Run the tests that need a compositor against a private headless Hyprland (run-headless-hyprland.sh), never against
# the desktop session, then stop it. Exits with the test status, or 77 when Hyprland cannot run here (no DRM device).
# Usage: scripts/hyprland/test-live.sh [build-dir] [extra gtest args...]
#
# The instance gets two headless outputs (APOLLO-TEST-1 at 1920x1080@60, APOLLO-TEST-2 at 1280x720@30), which
# WaylandLiveTest is told to expect. Hyprland's logs are copied to <build-dir>/tests/hyprland-live/ afterwards.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$here/../.." && pwd)"
build="${1:-$src/build}"
shift || true

# gtest suites that talk to a compositor. Name new suites *LiveTest and they are picked up; list any other suite
# (or Suite.Test pattern) here.
live_suites=(
  '*LiveTest'
)

filter=""
for suite in "${live_suites[@]}"; do
  if [[ $suite == *.* ]]; then
    filter+="${suite}:"
  else
    filter+="${suite}.*:${suite}/*:"
  fi
done
filter="${filter%:}"

# Never let the tests reach the desktop session: drop everything that could point at it. Synthetic input is not
# routed to a headless instance (it has no libinput), so the uinput tests stay off.
unset HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS \
  APOLLO_SKIP_LIVE_HYPRLAND APOLLO_TEST_INJECT_INPUT APOLLO_TEST_EXPECT_OUTPUTS

status=0
env_lines="$("$here/run-headless-hyprland.sh" start -o 1920x1080@60 -o 1280x720@30)" || status=$?
if ((status == 77)); then
  echo "test-live: no headless Hyprland on this machine; live tests skipped" >&2
  exit 77
elif ((status != 0)); then
  echo "test-live: could not start a headless Hyprland" >&2
  exit "$status"
fi
eval "$env_lines"

logs="$build/tests/hyprland-live"
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
  mkdir -p "$logs"
  cp -f "$APOLLO_HEADLESS_HYPRLAND_DIR/hyprland.out" "$APOLLO_HEADLESS_HYPRLAND_DIR"/hypr/*/hyprland.log "$logs/" 2>/dev/null || true
  "$here/run-headless-hyprland.sh" stop "$APOLLO_HEADLESS_HYPRLAND_DIR" || true
}
trap cleanup EXIT

export APOLLO_TEST_EXPECT_OUTPUTS="$APOLLO_TEST_HEADLESS_OUTPUTS"
echo "test-live: running '$filter' against $HYPRLAND_INSTANCE_SIGNATURE ($WAYLAND_DISPLAY)" >&2
cd "$build/tests"
mkdir -p "$logs"
set +e
./test_sunshine --gtest_filter="$filter" "$@" 2>&1 | tee "$logs/test_sunshine.out"
status=${PIPESTATUS[0]}
set -e

# A live test skips itself when it finds no compositor, so a broken harness would look green. Fail if nothing ran.
passed="$(sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' "$logs/test_sunshine.out" | tail -n 1)"
if ((status == 0)) && [[ ${passed:-0} == 0 ]]; then
  echo "test-live: every live test was skipped; the headless Hyprland is not reachable from the tests" >&2
  status=1
fi
exit "$status"
