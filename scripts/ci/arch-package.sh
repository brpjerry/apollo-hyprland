#!/usr/bin/env bash
# Builds the Arch package (packaging/linux/Arch/PKGBUILD) from the commit checked out in the current directory, with
# the unit tests, inside a throwaway Arch Linux container. Run as root, e.g.:
#   docker run --rm -v "$PWD:/w" -w /w archlinux:latest scripts/ci/arch-package.sh
# Writes to dist/: the package, SHA256SUMS, and the unit test binary (for the live Hyprland tests).
# Not for a desktop: makepkg runs ninja with one job per CPU and no memory cap (use scripts/hyprland/build.sh there).
set -euo pipefail
ws="$PWD"

# Source the PKGBUILD as makepkg will see it below, so checkdepends (only set when the unit tests run) is included.
_run_unit_tests=true
checkdepends=()
# shellcheck source=/dev/null
source packaging/linux/Arch/PKGBUILD
# shellcheck disable=SC2154 # depends and makedepends come from the PKGBUILD
pacman -Syu --noconfirm --needed base-devel git "${depends[@]}" "${makedepends[@]}" "${checkdepends[@]}"
commit="$(git -c safe.directory="$ws" rev-parse HEAD)"

# makepkg refuses to run as root.
useradd --create-home builder
build=/home/builder/pkg
install -d -o builder "$build"
install -o builder -m 644 packaging/linux/Arch/PKGBUILD packaging/linux/Arch/sunshine.install "$build"
# The checkout belongs to the runner's user; let the builder clone it.
su builder -c "git config --global --add safe.directory '*'"

su builder -c "cd '$build' && _source='git+file://$ws' _ref='commit=$commit' _run_unit_tests=true \
  makepkg --noconfirm"

rm -rf dist
mkdir -p dist/tests
cp "$build"/*.pkg.tar.zst dist/
cp "$build/src/build/tests/test_sunshine" dist/tests/
(cd dist && sha256sum -- *.pkg.tar.zst > SHA256SUMS)
cat dist/SHA256SUMS
