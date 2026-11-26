#!/usr/bin/env python3
"""Estimate the offset between this machine's CLOCK_MONOTONIC and a peer's, over UDP.

  clocksync.py server [PORT]            echo each packet back with the server's monotonic time
  clocksync.py client HOST [PORT] [N]   print JSON with offset_ns = server_clock - client_clock

The client keeps the samples with the smallest round trip and takes the median of their offsets, so queueing
delay on the network (Wi-Fi in particular) mostly drops out. What remains is the up/down asymmetry of the kept
round trips: each kept sample's offset is off by at most half its own round trip, so (with the clock offset
constant while sampling) the median is off by at most rtt_kept_max_ns / 2.
"""
import json
import socket
import struct
import sys
import time

PORT = 48999


def mono():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def server(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    while True:
        data, addr = sock.recvfrom(64)
        now = mono()
        sock.sendto(data[:8] + struct.pack("<Q", now), addr)


def client(host, port, count):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.2)
    samples = []
    for seq in range(count):
        t0 = mono()
        sock.sendto(struct.pack("<Q", seq), (host, port))
        while True:
            try:
                data, _ = sock.recvfrom(64)
            except socket.timeout:
                data = None
                break
            if len(data) == 16 and struct.unpack("<Q", data[:8])[0] == seq:
                break
        t3 = mono()
        if data is not None:
            server_ns = struct.unpack("<Q", data[8:])[0]
            samples.append((t3 - t0, server_ns - (t0 + t3) // 2))
        time.sleep(0.004)
    if len(samples) < 10:
        sys.exit(f"only {len(samples)} replies from {host}:{port}")
    samples.sort()
    best = samples[: max(5, len(samples) // 10)]
    offsets = sorted(offset for _, offset in best)
    rtts = [rtt for rtt, _ in samples]
    print(json.dumps({
        "offset_ns": offsets[len(offsets) // 2],
        "offset_spread_ns": offsets[-1] - offsets[0],
        "rtt_min_ns": rtts[0],
        "rtt_kept_max_ns": best[-1][0],
        "rtt_median_ns": rtts[len(rtts) // 2],
        "rtt_p95_ns": rtts[int(len(rtts) * 0.95)],
        "replies": len(samples),
        "sent": count,
        "local_ns": mono(),
    }))


def main(argv):
    if len(argv) >= 2 and argv[1] == "server":
        server(int(argv[2]) if len(argv) > 2 else PORT)
    elif len(argv) >= 3 and argv[1] == "client":
        client(argv[2], int(argv[3]) if len(argv) > 3 else PORT, int(argv[4]) if len(argv) > 4 else 400)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
