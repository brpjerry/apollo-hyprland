#!/usr/bin/env python3
"""Turn reactor (host) and probe (client) logs into latency numbers.

  analyze.py react RUN_DIR    key press on the client -> flip visible on the client, with a per-stage split
  analyze.py blink RUN_DIR    flip presented on the host -> flip visible on the client (video path only)

RUN_DIR holds reactor.log, probe.log and, for the per-stage split and blink mode, sync-before.json and
sync-after.json from clocksync.py run on the client (offset = host clock - client clock). The offset is
interpolated linearly between the two syncs to follow clock drift. Prints a JSON summary and a table.
"""
import json
import sys
from pathlib import Path

THRESHOLD = 128  # mean green value separating black (0) from white (255)


def load_log(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if line and not line.startswith("#"):
            rows.append(line.split())
    return rows


def load_sync(run):
    before = json.loads((run / "sync-before.json").read_text())
    after = json.loads((run / "sync-after.json").read_text())

    def offset_at(client_ns):
        t0, t1 = before["local_ns"], after["local_ns"]
        if t1 == t0:
            return before["offset_ns"]
        frac = (client_ns - t0) / (t1 - t0)
        return before["offset_ns"] + frac * (after["offset_ns"] - before["offset_ns"])

    return before, after, offset_at


def stats(values_ns):
    vals = sorted(v / 1e6 for v in values_ns)
    if not vals:
        return {"n": 0}

    def pct(p):
        return vals[min(len(vals) - 1, int(round(p / 100 * (len(vals) - 1))))]

    return {
        "n": len(vals),
        "min": round(vals[0], 2),
        "p50": round(pct(50), 2),
        "mean": round(sum(vals) / len(vals), 2),
        "p95": round(pct(95), 2),
        "p99": round(pct(99), 2),
        "max": round(vals[-1], 2),
    }


def client_transitions(probe_rows, use_recv=False):
    """(time_ns, new_state) for every change of the captured patch, client clock."""
    out = []
    state = None
    # With several captures in flight, frames can complete out of order or twice for one repaint.
    frames = sorted({(int(row[2] if use_recv else row[1]), int(row[3])) for row in probe_rows if row[0] == "F"})
    for t, mean in frames:
        s = 1 if mean >= THRESHOLD else 0
        if state is not None and s != state:
            out.append((t, s))
        state = s
    return out


def react(run):
    probe = load_log(run / "probe.log")
    reactor = load_log(run / "reactor.log")
    presses = [int(r[1]) for r in probe if r[0] == "K"]
    trans = client_transitions(probe)
    flips = [r for r in reactor if r[0] in ("P", "D") and r[3] == "key"]
    have_sync = (run / "sync-before.json").exists() and (run / "sync-after.json").exists()
    offset_at = load_sync(run)[2] if have_sync else None

    total, input_path, host_render, video_path = [], [], [], []
    missed = 0
    ti = 0
    fi = 0
    for i, press in enumerate(presses):
        nxt = presses[i + 1] if i + 1 < len(presses) else None
        while ti < len(trans) and trans[ti][0] < press:
            ti += 1
        if ti < len(trans) and (nxt is None or trans[ti][0] < nxt):
            seen = trans[ti][0]
            total.append(seen - press)
            ti += 1
        else:
            missed += 1
            seen = None
        if offset_at is None:
            continue
        off = offset_at(press)
        # The host flip caused by this press: the first key flip received after the press (in host time).
        while fi < len(flips) and int(flips[fi][5]) < press + off:
            fi += 1
        if fi >= len(flips):
            continue
        flip = flips[fi]
        recv = int(flip[5])
        if nxt is not None and recv >= nxt + off:
            continue
        fi += 1
        input_path.append(recv - (press + off))
        if flip[0] == "P":
            pres = int(flip[7])
            host_render.append(pres - recv)
            if seen is not None:
                video_path.append(seen - (pres - off))
    result = {
        "mode": "react",
        "presses": len(presses),
        "missed": missed,
        "total_ms": stats(total),
    }
    if offset_at is not None:
        result.update({
            "input_path_ms": stats(input_path),
            "host_render_ms": stats(host_render),
            "video_path_ms": stats(video_path),
        })
    return result


def blink(run):
    probe = load_log(run / "probe.log")
    reactor = load_log(run / "reactor.log")
    before, after, offset_at = load_sync(run)
    trans = client_transitions(probe)
    flips = [r for r in reactor if r[0] == "P" and r[3] == "timer"]
    video = []
    unmatched = 0
    ti = 0
    for flip in flips:
        color = int(flip[2])
        pres_host = int(flip[7])
        # Convert to the client clock with the offset at about that time.
        pres_client = pres_host - offset_at(pres_host - before["offset_ns"])
        # Allow the clock estimate to be a little early; flips are >= 200 ms apart.
        while ti < len(trans) and trans[ti][0] < pres_client - 20e6:
            ti += 1
        if ti < len(trans) and trans[ti][1] == color and trans[ti][0] < pres_client + 150e6:
            video.append(trans[ti][0] - pres_client)
            ti += 1
        else:
            unmatched += 1
    return {
        "mode": "blink",
        "flips": len(flips),
        "unmatched": unmatched,
        "video_path_ms": stats(video),
    }


def print_table(result):
    keys = [k for k in result if k.endswith("_ms")]
    print(f"{'stage':<16}{'n':>6}{'min':>8}{'p50':>8}{'mean':>8}{'p95':>8}{'p99':>8}{'max':>8}")
    for k in keys:
        s = result[k]
        if s.get("n"):
            print(f"{k[:-3]:<16}{s['n']:>6}{s['min']:>8}{s['p50']:>8}{s['mean']:>8}{s['p95']:>8}{s['p99']:>8}{s['max']:>8}")


def main(argv):
    if len(argv) != 3 or argv[1] not in ("react", "blink"):
        sys.exit(__doc__)
    run = Path(argv[2])
    result = react(run) if argv[1] == "react" else blink(run)
    (run / f"{argv[1]}-summary.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if not k.endswith("_ms")}))
    print_table(result)


if __name__ == "__main__":
    main(sys.argv)
