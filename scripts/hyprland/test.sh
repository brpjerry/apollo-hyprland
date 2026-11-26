#!/usr/bin/env bash
# Run the unit tests, skipping tests that already failed at the base commit (adc5c5a0).
# These are stale upstream Sunshine tests; see tickets.json for the baseline list.
# The *LiveTest suites are left out too: they create and reconfigure outputs, read the clipboard and type into the
# compositor they find, so they only run against a private headless Hyprland through test-live.sh. A
# --gtest_filter among the extra arguments replaces this filter.
# Usage: scripts/hyprland/test.sh [build-dir] [extra gtest args...]
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/build}"; shift || true
baseline_failures='ConfigConsistencyTest.AllConfigOptionsExistInAllFiles:ConfigConsistencyTest.ConfigOptionsInSameOrderWithinSections:MdnsInstanceNameTests/*:DisplayDeviceConfigTest/ParseRefreshRateOption.*:DisplayDeviceConfigTest/DisplayModeRemapping.*'
cd "$build/tests"
# The same patterns test-live.sh selects.
live_suites='*LiveTest.*:*LiveTest/*'

# Keep the tests away from the desktop session. Hyprland discovery scans $XDG_RUNTIME_DIR/hypr even without
# HYPRLAND_INSTANCE_SIGNATURE, and process tests launch apps, which would then create virtual displays on the user's
# Hyprland. An empty private runtime directory hides every compositor, PipeWire and D-Bus socket.
unset HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY DBUS_SESSION_BUS_ADDRESS
runtime="$(mktemp -d "${TMPDIR:-/tmp}/apollo-unit-runtime.XXXXXX")"
trap 'rm -rf -- "$runtime"' EXIT
export XDG_RUNTIME_DIR="$runtime"

./test_sunshine --gtest_brief=1 --gtest_filter="-${baseline_failures}:${live_suites}" "$@"
