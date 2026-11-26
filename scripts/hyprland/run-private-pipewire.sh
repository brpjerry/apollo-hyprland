#!/usr/bin/env bash
# Start or stop a private PipeWire + pipewire-pulse + WirePlumber instance for the audio tests.
#
# Usage:
#   eval "$(scripts/hyprland/run-private-pipewire.sh start)"
#   ... run tests or pactl against it ...
#   scripts/hyprland/run-private-pipewire.sh stop [DIR]
#
# start prints shell `export` lines for PULSE_SERVER, PULSE_RUNTIME_PATH, PIPEWIRE_RUNTIME_DIR, PIPEWIRE_REMOTE and
# APOLLO_PRIVATE_PIPEWIRE_DIR (the instance directory, which stop takes). Everything else goes to stderr.
# stop defaults to $APOLLO_PRIVATE_PIPEWIRE_DIR.
#
# Isolation from the desktop session:
# - The instance gets its own XDG_RUNTIME_DIR, HOME and XDG_* dirs (so neither the user's PipeWire/WirePlumber
#   configuration nor WirePlumber's stored stream/default state is read or written), and its own DBus session bus
#   (dbus-run-session), so it cannot claim the desktop's bus names.
# - WirePlumber runs without its hardware monitors (ALSA, Bluetooth, cameras), device reservation, logind and
#   portals, so it never opens or reserves a sound card. The instance has no sinks until a client loads some
#   (e.g. module-null-sink); pipewire-pulse adds a dummy sink when there is none.
#
# Exit status of start: 0 when the instance is up, 77 when PipeWire, pipewire-pulse, WirePlumber or dbus-run-session
# is not installed (the caller should skip), anything else on failure.
#
# Environment:
#   APOLLO_PIPEWIRE_TMPDIR         parent of the instance directory (default /tmp)
#   APOLLO_PIPEWIRE_START_TIMEOUT  seconds to wait for the instance (default 20)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
marker=".apollo-private-pipewire"

log() {
  echo "run-private-pipewire: $*" >&2
}

usage() {
  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

# Prints `export NAME='value'`.
export_line() {
  local q="'\\''"
  printf "export %s='%s'\n" "$1" "${2//\'/$q}"
}

# Runs a command against the instance in $1.
in_instance() {
  local dir="$1"
  shift
  env -u DBUS_SESSION_BUS_ADDRESS PULSE_SERVER="unix:$dir/run/pulse/native" PULSE_RUNTIME_PATH="$dir/run/pulse" \
    PIPEWIRE_RUNTIME_DIR="$dir/run" PIPEWIRE_REMOTE=pipewire-0 timeout 5 "$@"
}

stop_instance() {
  local dir="$1" pid i
  if [[ ! -f $dir/$marker ]]; then
    log "$dir is not a private PipeWire directory (no $marker); not touching it"
    return 1
  fi
  if [[ -f $dir/session.pid ]]; then
    pid="$(<"$dir/session.pid")"
    # The session runs in its own process group (setsid). Only signal it if the leader is still ours.
    if [[ -r /proc/$pid/environ ]] && tr '\0' '\n' < "/proc/$pid/environ" | grep -qxF "APOLLO_PRIVATE_PIPEWIRE_DIR=$dir"; then
      kill -TERM -- "-$pid" 2>/dev/null || true
      for ((i = 0; i < 50; i++)); do
        kill -0 -- "-$pid" 2>/dev/null || break
        sleep 0.1
      done
      if kill -0 -- "-$pid" 2>/dev/null; then
        log "the instance ($pid) did not exit after SIGTERM; killing it"
        kill -KILL -- "-$pid" 2>/dev/null || true
      fi
    fi
  fi
  rm -rf -- "$dir"
}

# Runs inside dbus-run-session: starts the three daemons and waits until one exits.
run_session() {
  local dir="$APOLLO_PRIVATE_PIPEWIRE_DIR" i
  pipewire > "$dir/pipewire.log" 2>&1 &
  for ((i = 0; i < 100; i++)); do
    [[ -S $XDG_RUNTIME_DIR/pipewire-0 ]] && break
    sleep 0.1
  done
  pipewire-pulse > "$dir/pipewire-pulse.log" 2>&1 &
  wireplumber > "$dir/wireplumber.log" 2>&1 &
  # Any exit, failed or not, ends the instance; under set -e a failed one must not skip the group kill below.
  wait -n || true
  log "a daemon of the private instance exited"
  kill -TERM 0 2>/dev/null || true
}

start_instance() {
  (($# == 0)) || usage
  local bin
  for bin in pipewire pipewire-pulse wireplumber dbus-run-session pactl pw-cli setsid; do
    if ! command -v "$bin" > /dev/null; then
      log "$bin not found; skipping"
      return 77
    fi
  done

  local dir
  dir="$(mktemp -d "${APOLLO_PIPEWIRE_TMPDIR:-/tmp}/apw.XXXXXX")"
  touch "$dir/$marker"
  mkdir -p "$dir/run" "$dir/home" "$dir/config/wireplumber/wireplumber.conf.d" "$dir/config/pipewire" "$dir/state" \
    "$dir/data" "$dir/cache"
  chmod 700 "$dir/run"
  cat > "$dir/config/wireplumber/wireplumber.conf.d/90-apollo-private.conf" << 'EOF'
# No hardware and nothing that talks to the system bus or the portals: this instance must never touch a real device.
wireplumber.profiles = {
  main = {
    hardware.audio = disabled
    hardware.bluetooth = disabled
    hardware.video-capture = disabled
    support.reserve-device = disabled
    monitor.alsa.reserve-device = disabled
    support.logind = disabled
    support.portal-permissionstore = disabled
    support.modem-manager = disabled
    support.mpris = disabled
  }
}
EOF

  # setsid: the session gets its own process group, which stop signals as a whole.
  env -i PATH="$PATH" HOME="$dir/home" XDG_RUNTIME_DIR="$dir/run" XDG_CONFIG_HOME="$dir/config" \
    XDG_STATE_HOME="$dir/state" XDG_DATA_HOME="$dir/data" XDG_CACHE_HOME="$dir/cache" \
    PULSE_RUNTIME_PATH="$dir/run/pulse" APOLLO_PRIVATE_PIPEWIRE_DIR="$dir" \
    setsid dbus-run-session -- "$here/run-private-pipewire.sh" _session > "$dir/session.out" 2>&1 < /dev/null &
  echo $! > "$dir/session.pid"

  # Up means: pipewire-pulse answers and WirePlumber has created the "default" metadata (it owns default sinks).
  local deadline=$((SECONDS + ${APOLLO_PIPEWIRE_START_TIMEOUT:-20}))
  until in_instance "$dir" pactl info > /dev/null 2>&1 &&
    in_instance "$dir" pw-cli ls Metadata 2> /dev/null | grep -q 'metadata.name = "default"'; do
    if ((SECONDS >= deadline)) || ! kill -0 "$(<"$dir/session.pid")" 2>/dev/null; then
      log "the private instance did not come up"
      local f
      for f in "$dir"/session.out "$dir"/*.log; do
        [[ -s $f ]] && { echo "----- $f -----" >&2; tail -n 20 "$f" >&2; }
      done
      stop_instance "$dir"
      return 1
    fi
    sleep 0.1
  done

  log "private PipeWire is up in $dir"
  export_line APOLLO_PRIVATE_PIPEWIRE_DIR "$dir"
  export_line PULSE_SERVER "unix:$dir/run/pulse/native"
  export_line PULSE_RUNTIME_PATH "$dir/run/pulse"
  export_line PIPEWIRE_RUNTIME_DIR "$dir/run"
  export_line PIPEWIRE_REMOTE pipewire-0
}

case "${1:-}" in
  start)
    shift
    start_instance "$@"
    ;;
  stop)
    shift
    dir="${1:-${APOLLO_PRIVATE_PIPEWIRE_DIR:-}}"
    [[ -n $dir ]] || usage
    stop_instance "$dir"
    ;;
  _session)
    run_session
    ;;
  *) usage ;;
esac
