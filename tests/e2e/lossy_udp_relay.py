#!/usr/bin/env python3
# Lossy UDP relay for the SRT packet-loss e2e (Phase 2c-b). Sits on the SRT link
# between the engine (SRT caller) and srt-live-transmit (SRT listener). Forwards
# UDP datagrams both ways; drops a SEEDED % of DOWNSTREAM (bridge->engine) SRT DATA
# packets only — SRT CONTROL packets (first byte high bit set, 0x80) always pass,
# so the engine's ARQ NAKs/ACKs keep flowing and retransmits can be requested.
#
# Usage:
#   lossy_udp_relay.py <listen_port> <bridge_host:bridge_port> <loss_pct> <stats_file> [seed]
import os
import random
import select
import signal
import socket
import sys


def main():
    listen_port = int(sys.argv[1])
    bridge_host, bridge_port = sys.argv[2].rsplit(":", 1)
    bridge_addr = (bridge_host, int(bridge_port))
    loss = float(sys.argv[3]) / 100.0
    stats_file = sys.argv[4]
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1234
    rng = random.Random(seed)

    engine_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    engine_sock.bind(("127.0.0.1", listen_port))
    bridge_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    engine_addr = None
    counters = {"dropped": 0, "forwarded": 0, "control_forwarded": 0}

    def write_stats_and_exit(*_):
        try:
            with open(stats_file, "w") as f:
                f.write("dropped=%d forwarded=%d control_forwarded=%d\n"
                        % (counters["dropped"], counters["forwarded"],
                           counters["control_forwarded"]))
        except OSError:
            pass
        os._exit(0)

    signal.signal(signal.SIGTERM, write_stats_and_exit)
    signal.signal(signal.SIGINT, write_stats_and_exit)

    while True:
        readable, _, _ = select.select([engine_sock, bridge_sock], [], [], 1.0)
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
                if (not is_control) and rng.random() < loss:
                    counters["dropped"] += 1
                    continue
                if is_control:
                    counters["control_forwarded"] += 1
                else:
                    counters["forwarded"] += 1
                engine_sock.sendto(data, engine_addr)


if __name__ == "__main__":
    main()
