#!/usr/bin/env bash
# Start or stop a private, headless Hyprland instance for the live tests.
#
# Usage:
#   eval "$(scripts/hyprland/run-headless-hyprland.sh start [-o [NAME=]WxH@HZ]...)"
#   ... run tests ...
#   scripts/hyprland/run-headless-hyprland.sh stop [DIR]
#
# start prints shell `export` lines for XDG_RUNTIME_DIR, WAYLAND_DISPLAY, HYPRLAND_INSTANCE_SIGNATURE,
# APOLLO_HEADLESS_HYPRLAND_DIR (the instance directory, which stop takes) and APOLLO_TEST_HEADLESS_OUTPUTS (the
# outputs created with -o). Everything else goes to stderr. Each -o creates one headless output, named NAME or
# APOLLO-TEST-<n>, and waits until it runs at the given mode. stop defaults to $APOLLO_HEADLESS_HYPRLAND_DIR.
#
# Isolation from the desktop session:
# - The instance gets its own XDG_RUNTIME_DIR, HOME and XDG_* dirs, and no DBus or WAYLAND_DISPLAY, so it cannot
#   nest into, or be found next to, the running session, and does not touch systemd's activation environment.
# - LIBSEAT_BACKEND names no real backend, so aquamarine cannot open a seat: no DRM/KMS outputs, no libinput.
# - Hyprland 0.57 cannot render without a DRM device, though. headless-drm-shim.cpp hands its headless backend a
#   render node (default: the first one), or a vkms card node on machines without a GPU. A card node of a real GPU
#   is refused. See the shim for details.
#
# Exit status of start: 0 when the instance is up, 77 when there is no usable DRM device (the caller should skip),
# anything else on failure.
#
# Environment:
#   APOLLO_HYPRLAND_DRM_NODE       device for the headless backend (default: first render node, else a vkms card)
#   APOLLO_HYPRLAND_TMPDIR         parent of the instance directory (default /tmp; the Hyprland socket path must
#                                  fit in 108 bytes, so this must be short)
#   APOLLO_HYPRLAND_START_TIMEOUT  seconds to wait for the IPC socket (default 30)
#   HYPRLAND_BIN, HYPRCTL_BIN      binaries to use (default: Hyprland and hyprctl from PATH)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
hyprland_bin="${HYPRLAND_BIN:-Hyprland}"
hyprctl_bin="${HYPRCTL_BIN:-hyprctl}"
marker=".apollo-headless-hyprland"

log() {
  echo "run-headless-hyprland: $*" >&2
}

usage() {
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

# True if the DRM node belongs to vkms (a platform device before Linux 6.15, a faux device since).
is_vkms() {
  local dev
  dev="/sys/class/drm/$(basename "$1")/device"
  [[ -e $dev ]] || return 1
  [[ $(basename "$(readlink -f "$dev")") == vkms || $(basename "$(readlink -f "$dev/driver")") == vkms ]]
}

# Prints `export NAME='value'`.
export_line() {
  local q="'\\''"
  printf "export %s='%s'\n" "$1" "${2//\'/$q}"
}

# Prints the DRM node to hand to the headless backend. Returns 1 when there is none, 2 when
# APOLLO_HYPRLAND_DRM_NODE is unusable.
pick_drm_node() {
  local node
  if [[ -n ${APOLLO_HYPRLAND_DRM_NODE:-} ]]; then
    node="$APOLLO_HYPRLAND_DRM_NODE"
    if [[ $(basename "$node") == card* ]] && ! is_vkms "$node"; then
      log "refusing $node: it is the KMS node of a real GPU; use its render node or a vkms card"
      return 2
    fi
    if [[ ! -r $node || ! -w $node ]]; then
      log "cannot open $node for reading and writing"
      return 2
    fi
    echo "$node"
    return 0
  fi
  for node in /dev/dri/renderD*; do
    if [[ -c $node && -r $node && -w $node ]]; then
      echo "$node"
      return 0
    fi
  done
  for node in /dev/dri/card*; do
    if [[ -c $node && -r $node && -w $node ]] && is_vkms "$node"; then
      echo "$node"
      return 0
    fi
  done
  return 1
}

hyprctl_in() {
  local dir="$1" sig="$2"
  shift 2
  XDG_RUNTIME_DIR="$dir" timeout 5 "$hyprctl_bin" -i "$sig" "$@"
}

# Prints "WxH@HZ" of an active output, e.g. 1280x720@60.00000.
current_mode() {
  local dir="$1" sig="$2" name="$3"
  hyprctl_in "$dir" "$sig" monitors 2>/dev/null |
    awk -v name="$name" '$1 == "Monitor" && $2 == name { getline; print $1; exit }'
}

mode_matches() {
  local current="$1" wanted="$2"
  [[ ${current%@*} == "${wanted%@*}" ]] || return 1
  awk -v a="${current#*@}" -v b="${wanted#*@}" 'BEGIN { d = a - b; exit !(d < 0.5 && d > -0.5) }'
}

print_tail() {
  local dir="$1" f
  for f in "$dir/hyprland.out" "$dir"/hypr/*/hyprland.log; do
    if [[ -s $f ]]; then
      echo "----- last lines of $f -----" >&2
      tail -n 40 "$f" >&2
    fi
  done
}

stop_instance() {
  local dir="$1" pid i
  if [[ ! -f $dir/$marker ]]; then
    log "$dir is not a headless Hyprland directory (no $marker); not touching it"
    return 1
  fi
  if [[ -f $dir/hyprland.pid ]]; then
    pid="$(<"$dir/hyprland.pid")"
    # Only signal the pid if it is still our Hyprland, not a recycled pid.
    if [[ -r /proc/$pid/environ ]] && tr '\0' '\n' < "/proc/$pid/environ" | grep -qxF "XDG_RUNTIME_DIR=$dir"; then
      kill -TERM "$pid" 2>/dev/null || true
      for ((i = 0; i < 100; i++)); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
      done
      if kill -0 "$pid" 2>/dev/null; then
        log "Hyprland ($pid) did not exit after SIGTERM; killing it"
        kill -KILL "$pid" 2>/dev/null || true
      fi
    fi
  fi
  rm -rf -- "$dir"
}

start_instance() {
  local outputs=() arg
  while (($#)); do
    case "$1" in
      -o | --output)
        [[ $# -ge 2 ]] || usage
        outputs+=("$2")
        shift 2
        ;;
      -h | --help) usage ;;
      *)
        log "unknown argument: $1"
        usage
        ;;
    esac
  done
  for arg in "${outputs[@]}"; do
    if [[ ! ${arg#*=} =~ ^[0-9]+x[0-9]+@[0-9]+(\.[0-9]+)?$ ]]; then
      log "bad output mode '$arg' (want [NAME=]WxH@HZ)"
      return 2
    fi
  done

  for arg in "$hyprland_bin" "$hyprctl_bin" c++ pkg-config; do
    if ! command -v "$arg" > /dev/null; then
      log "$arg not found"
      return 1
    fi
  done

  local node status=0
  node="$(pick_drm_node)" || status=$?
  if ((status == 2)); then
    return 1
  elif ((status != 0)); then
    log "no usable DRM device. Hyprland 0.57 needs a render node (or a vkms card node: 'modprobe vkms') even" \
      "with only headless outputs; skipping"
    return 77
  fi

  local dir
  dir="$(mktemp -d "${APOLLO_HYPRLAND_TMPDIR:-/tmp}/ahl.XXXXXX")"
  touch "$dir/$marker"
  # Hyprland's socket is $dir/hypr/<62-character signature>/.socket2.sock, and must fit in sun_path (108 bytes).
  if ((${#dir} > 25)); then
    log "$dir is too long for Hyprland's socket paths; set APOLLO_HYPRLAND_TMPDIR to a shorter directory"
    stop_instance "$dir"
    return 1
  fi
  mkdir -p "$dir/home"

  # shellcheck disable=SC2046 # pkg-config prints several flags
  if ! c++ -std=c++23 -O2 -shared -fPIC -o "$dir/headless-drm-shim.so" "$here/headless-drm-shim.cpp" \
    $(pkg-config --cflags --libs aquamarine hyprutils pixman-1 libdrm gbm) 2> "$dir/shim-build.log"; then
    cat "$dir/shim-build.log" >&2
    log "could not build headless-drm-shim (needs the aquamarine, hyprutils, pixman, libdrm and gbm headers)"
    stop_instance "$dir"
    return 1
  fi

  cat > "$dir/hyprland.lua" << 'EOF'
-- Generated by scripts/hyprland/run-headless-hyprland.sh: a bare compositor for tests.
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 1 })
hl.config({
    misc = {
        disable_hyprland_logo = true,
        disable_splash_rendering = true,
        force_default_wallpaper = 0,
        disable_watchdog_warning = true,
    },
    ecosystem = { no_update_news = true, no_donation_nag = true },
    animations = { enabled = false },
    xwayland = { enabled = false },
})
EOF

  local env_vars=(
    "PATH=$PATH"
    "HOME=$dir/home"
    "XDG_RUNTIME_DIR=$dir"
    "XDG_CONFIG_HOME=$dir/home/.config"
    "XDG_CACHE_HOME=$dir/home/.cache"
    "XDG_DATA_HOME=$dir/home/.local/share"
    "XDG_STATE_HOME=$dir/home/.local/state"
    # Not a libseat backend: aquamarine's DRM backend (KMS outputs, libinput) fails to open a seat and is skipped.
    "LIBSEAT_BACKEND=apollo-test-no-seat"
    "GSETTINGS_BACKEND=memory"
    "HYPRLAND_NO_SD_VARS=1"
    "HYPRLAND_NO_SD_NOTIFY=1"
    "HYPRLAND_NO_SD_TARGET=1"
    "APOLLO_HYPRLAND_DRM_NODE=$node"
    "LD_PRELOAD=$dir/headless-drm-shim.so"
  )
  # Pass through renderer and debugging knobs (e.g. LIBGL_ALWAYS_SOFTWARE, AQ_TRACE, HYPRLAND_TRACE).
  local name
  while IFS= read -r name; do
    env_vars+=("$name=${!name}")
  done < <(compgen -e | grep -E '^(AQ_|HYPRLAND_TRACE$|LIBGL_|MESA_|GALLIUM_|__EGL_|__GLX_|GBM_|EGL_|LANG$|LC_)' || true)

  local hyprland_args=(--config "$dir/hyprland.lua")
  if ((EUID == 0)); then
    # CI containers run as root; Hyprland refuses that without this flag.
    hyprland_args+=(--i-am-really-stupid)
  fi

  log "starting Hyprland in $dir on $node"
  (cd "$dir" && exec env -i "${env_vars[@]}" "$hyprland_bin" "${hyprland_args[@]}" > "$dir/hyprland.out" 2>&1 < /dev/null) &
  local pid=$!
  echo "$pid" > "$dir/hyprland.pid"

  local sig="" i deadline=$((SECONDS + ${APOLLO_HYPRLAND_START_TIMEOUT:-30}))
  while ((SECONDS < deadline)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      log "Hyprland exited during startup"
      print_tail "$dir"
      stop_instance "$dir"
      return 1
    fi
    for i in "$dir"/hypr/*/; do
      [[ -S $i/.socket.sock ]] || continue
      if hyprctl_in "$dir" "$(basename "$i")" version > /dev/null 2>&1; then
        sig="$(basename "$i")"
      fi
    done
    [[ -n $sig ]] && break
    sleep 0.1
  done
  if [[ -z $sig ]]; then
    log "Hyprland's IPC socket did not answer within ${APOLLO_HYPRLAND_START_TIMEOUT:-30}s"
    print_tail "$dir"
    stop_instance "$dir"
    return 1
  fi

  local wayland_display=""
  for i in "$dir"/wayland-*; do
    if [[ -S $i ]]; then
      wayland_display="$(basename "$i")"
      break
    fi
  done
  if [[ -z $wayland_display ]]; then
    log "Hyprland is up but has no Wayland socket in $dir"
    print_tail "$dir"
    stop_instance "$dir"
    return 1
  fi

  local names=() n=0 mode current ok
  for arg in "${outputs[@]}"; do
    n=$((n + 1))
    if [[ $arg == *=* ]]; then
      name="${arg%%=*}"
      mode="${arg#*=}"
    else
      name="APOLLO-TEST-$n"
      mode="$arg"
    fi
    if ! hyprctl_in "$dir" "$sig" output create headless "$name" > /dev/null ||
      ! hyprctl_in "$dir" "$sig" eval "hl.monitor({ output = \"$name\", mode = \"$mode\", position = \"auto\", scale = 1 })" > /dev/null; then
      log "could not create output $name"
      stop_instance "$dir"
      return 1
    fi
    ok=""
    for ((i = 0; i < 100; i++)); do
      current="$(current_mode "$dir" "$sig" "$name")"
      if [[ -n $current ]] && mode_matches "$current" "$mode"; then
        ok=1
        break
      fi
      sleep 0.1
    done
    if [[ -z $ok ]]; then
      log "output $name did not reach $mode (now: ${current:-missing})"
      stop_instance "$dir"
      return 1
    fi
    names+=("$name")
  done

  local joined
  joined="$(IFS=,; echo "${names[*]}")"
  {
    export_line XDG_RUNTIME_DIR "$dir"
    export_line WAYLAND_DISPLAY "$wayland_display"
    export_line HYPRLAND_INSTANCE_SIGNATURE "$sig"
    export_line APOLLO_HEADLESS_HYPRLAND_DIR "$dir"
    export_line APOLLO_TEST_HEADLESS_OUTPUTS "$joined"
  } | tee "$dir/env"
  log "up: pid $pid, signature $sig, $wayland_display, outputs: ${joined:-none}"
}

case "${1:-}" in
  start)
    shift
    start_instance "$@"
    ;;
  stop)
    shift
    stop_dir="${1:-${APOLLO_HEADLESS_HYPRLAND_DIR:-}}"
    [[ -n $stop_dir ]] || usage
    stop_instance "$stop_dir"
    ;;
  *) usage ;;
esac
