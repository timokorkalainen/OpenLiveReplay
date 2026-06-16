#!/usr/bin/env python3
# Lossy UDP relay for the SRT e2e (Phase 2c-b/2d). Sits on the SRT link between the
# engine (SRT caller) and srt-live-transmit (SRT listener). Forwards UDP datagrams
# both ways; drops a SEEDED % of DOWNSTREAM (bridge->engine) SRT DATA packets, and
# (when reorder_ms>0) delays each downstream DATA packet a random 0..reorder_ms to
# REORDER the link. SRT CONTROL packets (first byte high bit set, 0x80) always pass
# through immediately, so ARQ NAK/ACK timing is preserved.
#
# Usage:
#   lossy_udp_relay.py <listen_port> <bridge_host:bridge_port> <loss_pct> <stats_file> [seed] [reorder_ms]
import heapq
import os
import random
import select
import signal
import socket
import sys
import time


def main():
    listen_port = int(sys.argv[1])
    bridge_host, bridge_port = sys.argv[2].rsplit(":", 1)
    bridge_addr = (bridge_host, int(bridge_port))
    loss = float(sys.argv[3]) / 100.0
    stats_file = sys.argv[4]
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1234
    reorder_ms = int(sys.argv[6]) if len(sys.argv) > 6 else 0
    rng = random.Random(seed)

    engine_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    engine_sock.bind(("127.0.0.1", listen_port))
    bridge_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    engine_addr = None
    counters = {"dropped": 0, "forwarded": 0, "control_forwarded": 0, "reordered": 0}

    # Time-ordered release queue for delayed downstream DATA: (release_at, seq, data).
    # arrival_seq tags arrival order; a packet released after a later-arriving one
    # (seq < max_released_seq) counts as a real reordering.
    queue = []
    arrival_seq = 0
    max_released_seq = -1

    def write_stats_and_exit(*_):
        try:
            with open(stats_file, "w") as f:
                f.write("dropped=%d forwarded=%d control_forwarded=%d reordered=%d\n"
                        % (counters["dropped"], counters["forwarded"],
                           counters["control_forwarded"], counters["reordered"]))
        except OSError:
            pass
        os._exit(0)

    signal.signal(signal.SIGTERM, write_stats_and_exit)
    signal.signal(signal.SIGINT, write_stats_and_exit)

    def release_due(now):
        nonlocal max_released_seq
        while queue and queue[0][0] <= now and engine_addr is not None:
            _, seq, data = heapq.heappop(queue)
            if seq < max_released_seq:
                counters["reordered"] += 1
            if seq > max_released_seq:
                max_released_seq = seq
            engine_sock.sendto(data, engine_addr)
            counters["forwarded"] += 1

    while True:
        now = time.monotonic()
        release_due(now)
        # Wake when the next queued packet is due (else 1s, to avoid a busy loop).
        timeout = 1.0
        if queue:
            timeout = max(0.0, min(1.0, queue[0][0] - now))
        readable, _, _ = select.select([engine_sock, bridge_sock], [], [], timeout)
        for s in readable:
            data, addr = s.recvfrom(2048)
            if s is engine_sock:
                # Upstream (engine -> bridge): learn engine addr, forward unchanged.
                engine_addr = addr
                bridge_sock.sendto(data, bridge_addr)
                counters["forwarded"] += 1
            else:
                # Downstream (bridge -> engine).
                if engine_addr is None:
                    continue
                is_control = len(data) > 0 and (data[0] & 0x80)
                if is_control:
                    # Control (ACK/NAK/keepalive): never drop or delay.
                    engine_sock.sendto(data, engine_addr)
                    counters["control_forwarded"] += 1
                    continue
                if rng.random() < loss:
                    counters["dropped"] += 1
                    continue
                if reorder_ms > 0:
                    delay = rng.random() * (reorder_ms / 1000.0)
                    heapq.heappush(queue, (time.monotonic() + delay, arrival_seq, data))
                    arrival_seq += 1
                else:
                    engine_sock.sendto(data, engine_addr)
                    counters["forwarded"] += 1


if __name__ == "__main__":
    main()
