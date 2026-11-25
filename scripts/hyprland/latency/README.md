# End-to-end latency tools

Measures latency from a key press on a client machine to the change becoming visible on that client's screen, and
splits it into input path, host app, and video path. No camera needed: both ends use compositor timestamps, and a
UDP exchange estimates the offset between the two machines' clocks.

| Tool | Runs on | What it does |
|---|---|---|
| `reactor.c` | host | Layer-shell surface on the APOLLO output that flips black/white on a key press (`react`) or a jittered timer (`--blink`); logs presentation times. `--animate` also redraws a corner square every frame, so the stream never goes idle (game-like). |
| `probe.c` | client | Captures a small region of the client's screen with wlr-screencopy and logs its brightness with the compositor's timestamp; optionally presses Left Shift through its own uinput keyboard. |
| `clocksync.py` | both | UDP offset estimate between the two CLOCK_MONOTONICs (accurate to about half the fastest round trip). |
| `analyze.py` | host | Matches presses, host flips and client frames, and prints p50/p95/p99 per stage. |
| `run.sh` | host | Starts the stream over SSH, runs the blink and react phases, collects logs, analyzes. |

Build: `./build.sh OUT reactor` on the host, `./build.sh OUT probe [wlr-protocols dir]` on the client.
Run: `REACTOR=OUT/reactor ./run.sh RESULTS_DIR [extra client stream args]` (see the header of `run.sh` for the
environment variables; `HYPRLAND_INSTANCE_SIGNATURE` must point at the live host instance).

The react phase gives the reactor exclusive keyboard focus on the host for about a minute.

## Caveats

- The client timestamp is when the client's compositor repainted with the change, not when the panel lit up; add
  scanout and panel response (a few ms) for photon latency.
- The probe only sees repaints it has a capture request pending for. On Hyprland a request is serviced roughly every
  other repaint, so detection can be up to one capture interval late (check the gaps between `F` timestamps). More
  requests in flight (`--inflight`) sample more often but load the client's compositor, which matters on weak GPUs.
- Clock offset error is at most half the fastest round trip (about 0.5 ms on a good Wi-Fi link).
