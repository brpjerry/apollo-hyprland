#!/usr/bin/env bash
# End-to-end latency run. Artemis (or moonlight-qt) on a client machine streams an app from this host into a
# virtual display; the reactor flips that display black/white, and the probe on the client watches a patch of its
# screen. Two phases:
#   blink: the reactor flips on a timer (video path: host present -> client screen)
#   react: the probe presses Left Shift into the client and waits for the flip (key press -> client screen)
#
# Usage: run.sh OUT_DIR [extra client stream args...]
# Environment (defaults in brackets):
#   CLIENT [x9.internal]  CLIENT_SSH_PORT [538]  HOST_IP [192.168.0.115]  APP [Virtual Display]
#   RES [2880x1800]  FPS [120]  DURATION [60]  CLIENT_OUTPUT [eDP-1]  PROBE_REGION [712,442,16,16] (logical px)
#   CLIENT_BIN [artemis]  PROBE [~/.cache/apollo-latency/bin/probe on the client]  REACTOR [host path, required]
#   PHASES ["blink react"]
#   STREAM_OUTPUT [] the stream's output, e.g. APOLLO-1a2b3c4d (fixed per client: the first 8 hex digits of its
#     UUID). Unset: the one Apollo output that appears, and the run stops if another one shows up meanwhile.
#   CLIENT_FOCUS [hl.dsp.focus({ window = "address:%s" })] hyprctl dispatch argument focusing the client window
#     whose address replaces %s (hyprlang: focuswindow address:%s)
#   REACTOR_EXTRA [] extra reactor arguments, e.g. --animate for a stream that never goes idle
#   PROBE_EXTRA [] extra probe arguments, e.g. --inflight 2 (see probe.c; more requests in flight sample more often
#     but load the client compositor)
# Prerequisites: the client is paired, `probe` is built on the client (build.sh probe), clocksync.py is next to
# it, and UDP port 48999 on this host is reachable from the client.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mkdir -p "$1" && cd "$1" && pwd)"
shift
CLIENT="${CLIENT:-x9.internal}"
CLIENT_SSH_PORT="${CLIENT_SSH_PORT:-538}"
HOST_IP="${HOST_IP:-192.168.0.115}"
APP="${APP:-Virtual Display}"
RES="${RES:-2880x1800}"
FPS="${FPS:-120}"
DURATION="${DURATION:-60}"
CLIENT_OUTPUT="${CLIENT_OUTPUT:-eDP-1}"
PROBE_REGION="${PROBE_REGION:-712,442,16,16}"
CLIENT_BIN="${CLIENT_BIN:-artemis}"
PROBE="${PROBE:-.cache/apollo-latency/bin/probe}"
CLOCKSYNC="${CLOCKSYNC:-.cache/apollo-latency/src/clocksync.py}"
REACTOR="${REACTOR:?set REACTOR to the built reactor binary}"
PHASES="${PHASES:-blink react}"

# Commands on the client run with its graphical session's environment.
client_env='export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1} DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus} HYPRLAND_INSTANCE_SIGNATURE=$(ls -t /run/user/$(id -u)/hypr | head -1);'
ssh_client() { ssh -p "$CLIENT_SSH_PORT" -o BatchMode=yes "$CLIENT" "$client_env $*"; }
log() { printf '[%(%T)T] %s\n' -1 "$*" | tee -a "$out/run.log"; }
hypr() { hyprctl -i "$HYPRLAND_INSTANCE_SIGNATURE" "$@"; }

cleanup() {
  ssh_client "WAYLAND_DISPLAY=wayland-1 $CLIENT_BIN quit $HOST_IP >/dev/null 2>&1; systemctl --user stop apollo-latency-client 2>/dev/null" || true
  systemctl --user stop apollo-latency-sync apollo-latency-reactor 2>/dev/null || true
}
# The reactor goes on the stream's output, found as the one Apollo output that appears: refuse to start when one is
# there already (another stream, a leftover), before the trap, so refusing does not quit someone else's session.
stream_outputs='[.[] | select(.name | test("^APOLLO-[0-9a-f]{8}$")) | .name]'
if [[ "$(hypr -j monitors | jq -r "$stream_outputs | length")" != 0 ]]; then
  log "an Apollo stream output already exists; end that stream first"
  exit 1
fi
trap cleanup EXIT

stream_args=(stream "$HOST_IP" "$APP" --resolution "$RES" --fps "$FPS" --display-mode fullscreen --quit-after --no-performance-overlay "$@")
printf '%s\n' "client: $CLIENT_BIN ${stream_args[*]}" > "$out/config.txt"
env | grep -E '^(RES|FPS|DURATION|APP|PROBE_REGION|PHASES|REACTOR_EXTRA|PROBE_EXTRA)=' >> "$out/config.txt" || true

log "starting stream on $CLIENT"
ssh_client "systemctl --user reset-failed apollo-latency-client 2>/dev/null; systemd-run --user --quiet --collect --unit=apollo-latency-client $CLIENT_BIN $(printf '%q ' "${stream_args[@]}")"

vout=""
for _ in $(seq 1 150); do
  if [[ -n "${STREAM_OUTPUT:-}" ]]; then
    vout="$(hypr -j monitors | jq -r --arg n "$STREAM_OUTPUT" '.[] | select(.name == $n) | .name')"
  else
    vout="$(hypr -j monitors | jq -r "$stream_outputs | if length == 1 then .[0] else empty end")"
  fi
  [[ -n "$vout" ]] && break
  timeout 0.2 tail -f /dev/null || true
done
[[ -n "$vout" ]] || { log "no APOLLO output appeared"; exit 1; }
# Without STREAM_OUTPUT, another session's output appearing after the preflight check could have been taken for this
# stream's; by the time the client window has focus this stream's output is there, so two of them are ambiguous.
check_one_stream_output() {
  [[ -n "${STREAM_OUTPUT:-}" ]] && return 0
  local outputs
  outputs="$(hypr -j monitors | jq -r "$stream_outputs | join(\" \")")"
  [[ "$outputs" == "$vout" ]] || { log "Apollo outputs changed to [$outputs] after picking $vout; set STREAM_OUTPUT"; exit 1; }
}
hypr -j monitors | jq -r ".[] | select(.name == \"$vout\") | \"virtual display \(.name) \(.width)x\(.height)@\(.refreshRate)\"" | tee -a "$out/run.log"

# Wait for the client's stream window to be focused. Hyprland may tile it despite --display-mode fullscreen, so
# make it fullscreen explicitly (CLIENT_FULLSCREEN is a hyprctl dispatch argument; Lua-config syntax by default).
CLIENT_STREAM_CLASS="${CLIENT_STREAM_CLASS:-com.moonlight_stream.Moonlight}"
CLIENT_FULLSCREEN="${CLIENT_FULLSCREEN:-hl.dsp.window.fullscreen({ mode = \"fullscreen\" \})}"
CLIENT_FOCUS="${CLIENT_FOCUS:-hl.dsp.focus({ window = \"address:%s\" \})}"
window=""
for _ in $(seq 1 75); do
  window="$(ssh_client 'hyprctl -j activewindow' | jq -r --arg c "$CLIENT_STREAM_CLASS" 'select(.class == $c) | .address' 2>/dev/null)" || true
  [[ -n "$window" ]] && break
  timeout 0.2 tail -f /dev/null || true
done
# Never fullscreen whatever else has focus on the client.
[[ -n "$window" ]] || { log "client stream window ($CLIENT_STREAM_CLASS) did not take focus"; exit 1; }
# From here on the stream window is named by its address: focus can move to another window at any time.
stream_window() { ssh_client 'hyprctl -j clients' | jq -e --arg a "$window" '.[] | select(.address == $a)'; }
if ! stream_window | jq -e '.fullscreen > 0' >/dev/null 2>&1; then
  # One batch: Hyprland runs both before handling anything else, so the fullscreen goes to the window just focused.
  # shellcheck disable=SC2059 # CLIENT_FOCUS is the format
  ssh_client "hyprctl --batch $(printf '%q' "dispatch $(printf "$CLIENT_FOCUS" "$window") ; dispatch $CLIENT_FULLSCREEN")" >/dev/null
  timeout 1 tail -f /dev/null || true
fi
stream_window | jq -r '"client window: \(.class) fullscreen=\(.fullscreen) at \(.at) size \(.size)"' | tee -a "$out/run.log"
ssh_client 'hyprctl -j activewindow' | jq -e --arg a "$window" '.address == $a and .fullscreen > 0' >/dev/null ||
  { log "client stream window is not fullscreen and focused"; exit 1; }
ssh_client 'echo "client vrr=$(hyprctl getoption misc:vrr | head -1) direct_scanout=$(hyprctl getoption render:direct_scanout | head -1)"' | tee -a "$out/run.log"
# Let the stream settle (encoder warm-up, first IDR).
timeout 3 tail -f /dev/null || true
check_one_stream_output

systemctl --user reset-failed apollo-latency-sync 2>/dev/null || true
systemd-run --user --quiet --collect --unit=apollo-latency-sync python3 "$here/clocksync.py" server

for phase in $PHASES; do
  dir="$out/$phase"
  mkdir -p "$dir"
  if [[ "$phase" == blink ]]; then
    reactor_args=(--blink 250)
    probe_args=()
  else
    reactor_args=()
    probe_args=(--press 250,500)
    notice="Latency test: a full-screen reactor takes the keyboard on $vout for ~$((DURATION + 10))s; the client types Left Shift into $CLIENT_BIN"
    # automation-notice (a local desktop notification helper) is optional; without it the notice only goes to the log.
    if command -v automation-notice > /dev/null; then
      automation-notice "$notice"
    else
      log "$notice"
    fi
    ssh_client "notify-send -t 5000 'Apollo latency test' 'Pressing Left Shift into Artemis for ${DURATION}s'" || true
  fi
  log "phase $phase: reactor on $vout"
  systemctl --user reset-failed apollo-latency-reactor 2>/dev/null || true
  systemd-run --user --quiet --collect --unit=apollo-latency-reactor -p StandardOutput="file:$dir/reactor.log" -p StandardError="file:$dir/reactor.err" \
    "$REACTOR" --output "$vout" --duration $((DURATION + 8)) "${reactor_args[@]}" ${REACTOR_EXTRA:-}
  timeout 1.5 tail -f /dev/null || true
  log "phase $phase: clock sync + probe ($DURATION s)"
  ssh_client "python3 $CLOCKSYNC client $HOST_IP" > "$dir/sync-before.json"
  ssh_client "$PROBE --output $CLIENT_OUTPUT --region $PROBE_REGION --duration $DURATION ${probe_args[*]} ${PROBE_EXTRA:-}" > "$dir/probe.log" 2> "$dir/probe.err"
  ssh_client "python3 $CLOCKSYNC client $HOST_IP" > "$dir/sync-after.json"
  systemctl --user stop apollo-latency-reactor 2>/dev/null || true
  for _ in $(seq 1 50); do systemctl --user is-active --quiet apollo-latency-reactor || break; timeout 0.2 tail -f /dev/null || true; done
  log "phase $phase: $(grep -c '^F' "$dir/probe.log") frames, $(grep -c '^K' "$dir/probe.log" || true) presses, $(grep -c '^P' "$dir/reactor.log") host flips"
  python3 "$here/analyze.py" "$phase" "$dir" | tee -a "$out/run.log"
done

log "stopping stream"
ssh_client "WAYLAND_DISPLAY=wayland-1 $CLIENT_BIN quit $HOST_IP >/dev/null 2>&1" || true
for _ in $(seq 1 50); do ssh_client 'systemctl --user is-active --quiet apollo-latency-client' || break; timeout 0.2 tail -f /dev/null || true; done
ssh_client 'journalctl --user -u apollo-latency-client --no-pager -o cat --since "-15min"' > "$out/client.log" 2>&1 || true
log "done: $out"
