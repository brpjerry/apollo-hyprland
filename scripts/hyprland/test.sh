#!/usr/bin/env bash
# Run the unit tests, skipping tests that already failed at the base commit (adc5c5a0).
# These are stale upstream Sunshine tests; see tickets.json for the baseline list.
# Usage: scripts/hyprland/test.sh [build-dir] [extra gtest args...]
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/build}"; shift || true
baseline_failures='ConfigConsistencyTest.AllConfigOptionsExistInAllFiles:ConfigConsistencyTest.ConfigOptionsInSameOrderWithinSections:MdnsInstanceNameTests/*:DisplayDeviceConfigTest/ParseRefreshRateOption.*:DisplayDeviceConfigTest/DisplayModeRemapping.*'
cd "$build/tests"
exec ./test_sunshine --gtest_brief=1 --gtest_filter="-${baseline_failures}" "$@"
