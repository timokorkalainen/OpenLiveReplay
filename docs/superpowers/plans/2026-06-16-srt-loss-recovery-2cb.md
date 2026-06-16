# SRT Packet-Loss Recovery (Phase 2c-b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the native SRT ingest recovers from packet loss via SRT's ARQ retransmit (moderate loss recovered with no content loss, *provably via retransmission*) and degrades gracefully under heavy loss.

**Architecture:** A `lossy_udp_relay.py` sits on the SRT link (engine ↔ relay ↔ srt-live-transmit) and drops a seeded % of *downstream SRT DATA* packets only (control passes). A new local-only gate `run_srt_loss.sh` records one source through the relay at 0 % / 12 % / 60 % loss and asserts recovery (relay drop count + `srt_bstats` retransmit/loss stats + flash content) and heavy-loss degradation. A minimal `nativesrtingestsession` change logs SRT receiver stats on stop.

**Tech Stack:** python3 (stdlib), Bash (macOS 3.2-safe), ffmpeg/`srt-live-transmit`, libsrt, Qt6, CMake/CTest, the native SRT ingest.

**Spec:** `docs/superpowers/specs/2026-06-16-srt-loss-recovery-2cb-design.md`

**Reference (proven prior art):**
- `tests/e2e/srt_lib.sh` — `srt_require_tools`, `srt_bridge <udp> <srt>`, `flash_marker_to_udps <port...>` (call with ONE port for a dedicated producer), `flash_pts_series <mkv> <vidx>`, `SRT_LAST_PID`.
- `tests/e2e/run_srt_reconnect.sh` — background-harness + `>out 2>err` file capture + `wait` + cleanup-trap pattern; native gate registration (`native-apple-ingest`, `OLR_NATIVE_SRT=1`).
- `recorder_engine/ingest/nativesrtingestsession.cpp` — `run()` loop (line 169–205) calling `srt_recv(m_socket, …)`; `requestStop()` (207) closes the socket on the stop path; `log()` → qDebug → stderr; `#include <srt/srt.h>` (line 14).

**Key constraints:**
- The relay drops **downstream DATA only** — SRT control packets (first byte `& 0x80` set) must always pass, or the ACK/NAK flow dies and ARQ never runs.
- `srt_bstats` must be read **while the socket is alive** (in the capture loop), because `requestStop()` closes it from another thread before `run()`'s loop exits.
- macOS bash 3.2 (no `${arr[-1]}`); `command -v python3` SKIP guard.
- These features may reveal real behavior — if moderate loss does NOT recover (gaps / `pktRcvLossTotal>0` / no retransmits) on a correct build, that is a finding to report, not to loosen away.

---

## Task 0: One-time prerequisites — SRT ffmpeg + build dir + baseline

Skip the ffmpeg build if `macos_build/ffmpeg-srt/lib/libavformat.dylib` exists.

**Files:** none (gitignored artifacts).

- [ ] **Step 1: Tools + tarball**

Run:
```bash
command -v srt-live-transmit; command -v python3
mkdir -p ios_build/src
cp /Users/timo.korkalainen/Development/timo/OpenLiveReplay/ios_build/src/ffmpeg-8.0.tar.bz2 ios_build/src/ 2>/dev/null || true
```
Expected: paths for `srt-live-transmit` and `python3`.

- [ ] **Step 2: Build libsrt-enabled ffmpeg (~10 min, skip if present)**

Run: `bash build-scripts/build_ffmpeg_macos_srt.sh && otool -L macos_build/ffmpeg-srt/lib/libavformat.*.dylib | grep -i srt`
Expected: `libavformat` links `libsrt`.

- [ ] **Step 3: Configure + build harnesses**

Run:
```bash
cmake -S . -B build/srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_MAKE_PROGRAM="$HOME/Qt/Tools/Ninja/ninja" \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness record_harness
```
Expected: builds cleanly.

- [ ] **Step 4: Baseline — native suite passes**

Run: `( cd build/srt && ctest -L native-apple-ingest --output-on-failure )`
Expected: 6 native gates pass (smoke, 4cam, sync, trim, connect, reconnect).

---

## Task 1: `lossy_udp_relay.py` — data-only lossy SRT relay

**Files:**
- Create: `tests/e2e/lossy_udp_relay.py`

- [ ] **Step 1: Write the relay**

Create `tests/e2e/lossy_udp_relay.py` with exactly:

```python
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
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/lossy_udp_relay.py`

- [ ] **Step 3: Write a focused self-test (proves the BLOCKER fix: control always passes, data drops at loss%)**

Create a throwaway test and run it (do NOT commit the test file):
```bash
cat > /tmp/relay_selftest.py <<'PY'
import os, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath("tests/e2e/lossy_udp_relay.py"))
# "bridge" = a UDP echo target that just records what it receives (upstream),
# and a sender that injects downstream packets to the relay's engine-facing port.
relay_port = 24001
bridge = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); bridge.bind(("127.0.0.1", 24002))
stats = "/tmp/relay_selftest.stats"
p = subprocess.Popen([sys.executable, "tests/e2e/lossy_udp_relay.py",
                      str(relay_port), "127.0.0.1:24002", "50", stats, "7"])
time.sleep(0.5)
# Pretend to be the engine: send one upstream packet so the relay learns our addr.
eng = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); eng.bind(("127.0.0.1", 24003))
eng.sendto(b"\x00upstream", ("127.0.0.1", relay_port))
time.sleep(0.2)
# Now inject downstream packets FROM the bridge addr (24002) to the relay.
# 200 DATA (high bit clear) + 50 CONTROL (high bit set).
data_seen = ctrl_seen = 0
eng.settimeout(0.2)
for i in range(200): bridge.sendto(b"\x00" + bytes([i & 0xff]) * 10, ("127.0.0.1", relay_port))
for i in range(50):  bridge.sendto(b"\x80" + bytes([i & 0xff]) * 10, ("127.0.0.1", relay_port))
time.sleep(0.5)
while True:
    try:
        d, _ = eng.recvfrom(2048)
        if d[0] & 0x80: ctrl_seen += 1
        else: data_seen += 1
    except socket.timeout:
        break
p.terminate(); time.sleep(0.3)
dropped = open(stats).read()
print("data_seen=%d ctrl_seen=%d  %s" % (data_seen, ctrl_seen, dropped.strip()))
assert ctrl_seen == 50, "control packets must ALL pass (got %d/50)" % ctrl_seen
assert 60 < data_seen < 140, "data drop ~50%% expected, ~100 pass (got %d/200)" % data_seen
print("RELAY_SELFTEST_OK")
PY
python3 /tmp/relay_selftest.py
rm -f /tmp/relay_selftest.py /tmp/relay_selftest.stats
```
Expected: `RELAY_SELFTEST_OK` (all 50 control packets passed; ~100 of 200 data packets passed at 50 % loss). This proves the critical drop-only-data behavior.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/lossy_udp_relay.py
git commit -m "test(srt): lossy_udp_relay.py — drop downstream SRT DATA only (control passes)"
```

---

## Task 2: log SRT receiver stats on stop (native ingest)

**Files:**
- Modify: `recorder_engine/ingest/nativesrtingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`

- [ ] **Step 1: Confirm the SRT_TRACEBSTATS field names**

Run:
```bash
grep -nE "pktRcvRetrans|pktRcvLossTotal|pktRecvTotal" "$(brew --prefix srt)/include/srt/srt.h"
```
Expected: all three field names exist in `SRT_TRACEBSTATS`. (If a name differs — e.g. `pktRcvRetransTotal` — use the actual name from the header in Steps 3–4 and the test.)

- [ ] **Step 2: Add stats members to the header**

In `recorder_engine/ingest/nativesrtingestsession.h`, in the private members section (near `int m_socket = -1;`), add:

```cpp
    int64_t m_statRetrans = -1;
    int64_t m_statLossTotal = -1;
    int64_t m_statRecvTotal = -1;
    int64_t m_lastStatsAtMs = -1;
```

- [ ] **Step 3: Read stats periodically while the socket is alive**

In `recorder_engine/ingest/nativesrtingestsession.cpp`, in `run()`, replace the success branch:

```cpp
        if (received > 0) {
            m_lastPacketAtMs = m_monotonic.elapsed();
            processReceivedBytes(buffer.constData(), received);
            continue;
        }
```

with:

```cpp
        if (received > 0) {
            m_lastPacketAtMs = m_monotonic.elapsed();
            processReceivedBytes(buffer.constData(), received);
            // Snapshot SRT receiver stats ~1x/s WHILE the socket is alive —
            // requestStop() closes it from another thread before this loop exits,
            // so the last in-loop snapshot is the one we can log. clear=0 keeps
            // the counters cumulative since connect.
            if (m_lastStatsAtMs < 0 || m_monotonic.elapsed() - m_lastStatsAtMs > 1000) {
                SRT_TRACEBSTATS perf;
                if (srt_bstats(m_socket, &perf, 0) == 0) {
                    m_statRetrans = perf.pktRcvRetrans;
                    m_statLossTotal = perf.pktRcvLossTotal;
                    m_statRecvTotal = perf.pktRecvTotal;
                }
                m_lastStatsAtMs = m_monotonic.elapsed();
            }
            continue;
        }
```

- [ ] **Step 4: Log the snapshot when the loop exits**

In the same file, replace the loop-exit tail:

```cpp
    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(false);
    }
}
```

with:

```cpp
    // Loss/recovery telemetry (also seeds roadmap JIT-5). pktRcvRetrans>0 means
    // SRT's ARQ retransmitted; pktRcvLossTotal is the loss it could NOT recover.
    log(QStringLiteral("srt_stats pktRcvRetrans=%1 pktRcvLossTotal=%2 pktRecvTotal=%3")
            .arg(m_statRetrans)
            .arg(m_statLossTotal)
            .arg(m_statRecvTotal));

    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(false);
    }
}
```

- [ ] **Step 5: Build**

Run: `"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness`
Expected: clean (warnings are errors).

- [ ] **Step 6: Verify the stats line appears in a normal native SRT run**

Run:
```bash
. tests/e2e/srt_lib.sh
PIDS=(); B=23690
flash_marker_to_udps "$((B+1))"; srt_bridge "$((B+1))" "$B"
sleep 1.5
OLR_NATIVE_SRT=1 build/srt/tests/e2e/sync_harness --url "$(srt_caller_url $B)" \
  --outdir /tmp --name statprobe --seconds 6 --fps 30 >/tmp/stat.out 2>/tmp/stat.err
kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -f /tmp/statprobe*.mkv
grep "srt_stats" /tmp/stat.err
```
Expected: a line `... srt_stats pktRcvRetrans=<n> pktRcvLossTotal=<n> pktRecvTotal=<n>` with `pktRecvTotal` > 0 (and on a clean local link, retrans/loss ≈ 0).

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/nativesrtingestsession.h recorder_engine/ingest/nativesrtingestsession.cpp
git commit -m "feat(srt): log native SRT receiver stats (pktRcvRetrans/LossTotal) on stop"
```

---

## Task 3: `run_srt_loss.sh` gate + `e2e_native_srt_loss`

**Files:**
- Create: `tests/e2e/run_srt_loss.sh`
- Modify: `tests/e2e/CMakeLists.txt` (inside the `if(APPLE)` block, after `e2e_native_srt_reconnect`)

- [ ] **Step 1: Write the gate script**

Create `tests/e2e/run_srt_loss.sh` with exactly:

```bash
#!/usr/bin/env bash
# Local SRT e2e (Phase 2c-b): packet-loss recovery on the NATIVE SRT ingest.
#
# A lossy_udp_relay.py sits on the SRT link (engine <-> relay <-> srt-live-transmit)
# and drops a seeded % of DOWNSTREAM SRT DATA packets (control passes). We record one
# source through the relay at 0% / 12% / 60% loss and prove SRT's ARQ recovers
# moderate loss (no content loss, retransmits happened) and degrades under heavy loss.
#
#   baseline 0%  -> reference flash count B (and proves the relay drops 0).
#   moderate 12% -> relay dropped data; native srt_stats pktRcvRetrans>0 &
#                   pktRcvLossTotal~0 (ARQ recovered); flashes >= 0.85*B, no gap >1.5s.
#   heavy 60%    -> run exits cleanly AND content degrades (flashes <= 0.5*B OR a gap
#                   >2s) — proving the injected loss is real and the gate discriminates.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX; native ingest via
# OLR_NATIVE_SRT=1 (set by the CTest registration). Usage: run_srt_loss.sh <harness> [base]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23660}"
SECS="${OLR_SRT_LOSS_SECS:-10}"
SEED="${OLR_SRT_LOSS_SEED:-1234}"
LOSS_MOD="${OLR_SRT_LOSS_MOD:-12}"
LOSS_HEAVY="${OLR_SRT_LOSS_HEAVY:-60}"
RELAY="$HERE/lossy_udp_relay.py"

srt_require_tools
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
[ -f "$RELAY" ] || { echo "FAIL: $RELAY missing"; exit 1; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.4; kill -9 "${PIDS[@]}" 2>/dev/null; }
    wait 2>/dev/null; rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Record one source through the relay at $1=loss% $2=tag $3=port_base. Each run uses
# its own port_base so a lingering socket can't collide with the next.
measure() {
    local loss="$1" tag="$2" pb="$3"
    local S=$pb UDP=$((pb+1)) R=$((pb+2)) pp brp rp hp
    flash_marker_to_udps "$UDP"; pp=$SRT_LAST_PID
    srt_bridge "$UDP" "$S"; brp=$SRT_LAST_PID
    python3 "$RELAY" "$R" "127.0.0.1:$S" "$loss" "$WORKDIR/$tag.stats" "$SEED" &
    rp=$!; PIDS+=("$rp")
    sleep 1.5
    "$HARNESS" --url "srt://127.0.0.1:$R?transtype=live" \
        --outdir "$WORKDIR" --name "loss_$tag" --seconds "$SECS" --fps 30 \
        >"$WORKDIR/$tag.out" 2>"$WORKDIR/$tag.err" &
    hp=$!; PIDS+=("$hp")
    wait "$hp"
    kill -TERM "$rp" 2>/dev/null; sleep 0.5; kill -9 "$rp" 2>/dev/null   # flush relay stats
    kill "$pp" "$brp" 2>/dev/null; wait "$pp" "$brp" 2>/dev/null
}

# count + max-gap of the recorded view's flashes -> "count maxgap"
flash_stats() {
    flash_pts_series "$1" 0 | awk 'NR>1{d=$1-p; if(d>mx)mx=d} {p=$1} END{print NR+0, (mx==""?0:mx)}'
}
relay_dropped() { awk -F'[= ]' '/dropped=/{print $2; exit}' "$1" 2>/dev/null; }
srt_stat() {  # $1=err file $2=field -> value
    grep 'srt_stats' "$1" | tail -1 | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2; exit}'
}

echo "[srt-loss] base=$BASE secs=$SECS seed=$SEED mod=${LOSS_MOD}% heavy=${LOSS_HEAVY}%"
measure 0          base  "$BASE"
measure "$LOSS_MOD"  mod  "$((BASE+4))"
measure "$LOSS_HEAVY" heavy "$((BASE+8))"

MKV0="$(tail -n1 "$WORKDIR/base.out")"; read -r B0 G0 <<<"$(flash_stats "$MKV0")"
MKV1="$(tail -n1 "$WORKDIR/mod.out")";  read -r B1 G1 <<<"$(flash_stats "$MKV1")"
MKV2="$(tail -n1 "$WORKDIR/heavy.out")";read -r B2 G2 <<<"$(flash_stats "$MKV2")"
DROP0="$(relay_dropped "$WORKDIR/base.stats")"
DROP1="$(relay_dropped "$WORKDIR/mod.stats")"
RETR1="$(srt_stat "$WORKDIR/mod.err" pktRcvRetrans)"
RLOSS1="$(srt_stat "$WORKDIR/mod.err" pktRcvLossTotal)"
echo "[srt-loss] baseline: flashes=$B0 maxgap=${G0}s relay_dropped=${DROP0:-?}"
echo "[srt-loss] moderate(${LOSS_MOD}%): flashes=$B1 maxgap=${G1}s relay_dropped=${DROP1:-?} pktRcvRetrans=${RETR1:-?} pktRcvLossTotal=${RLOSS1:-?}"
echo "[srt-loss] heavy(${LOSS_HEAVY}%): flashes=$B2 maxgap=${G2}s"

fail=0
# Baseline sane + relay injected nothing at 0%.
[ "${B0:-0}" -ge "$((SECS-2))" ] || { echo "FAIL: baseline only ${B0:-0} flashes (< $((SECS-2)))"; fail=1; }
[ "${DROP0:-1}" = "0" ] || { echo "FAIL: relay dropped ${DROP0} at 0% loss (should be 0)"; fail=1; }
# Moderate: loss injected, ARQ engaged + recovered, content near-full.
awk -v d="${DROP1:-0}"  'BEGIN{exit !(d+0 >= 20)}'        || { echo "FAIL: relay dropped only ${DROP1:-0} data pkts at ${LOSS_MOD}% — loss not injected"; fail=1; }
awk -v r="${RETR1:-0}"  'BEGIN{exit !(r+0 > 0)}'          || { echo "FAIL: pktRcvRetrans=${RETR1:-?} — SRT ARQ did not retransmit"; fail=1; }
awk -v l="${RLOSS1:-9}" 'BEGIN{exit !(l+0 <= 5)}'         || { echo "FAIL: pktRcvLossTotal=${RLOSS1:-?} — loss NOT recovered"; fail=1; }
awk -v a="${B1:-0}" -v b="${B0:-1}" 'BEGIN{exit !(a+0 >= 0.85*b)}' || { echo "FAIL: moderate flashes ${B1} < 0.85*baseline ${B0} — content not recovered"; fail=1; }
awk -v g="${G1:-9}"     'BEGIN{exit !(g+0 <= 1.5)}'       || { echo "FAIL: moderate max gap ${G1}s > 1.5s — content not continuous"; fail=1; }
# Heavy: clean exit + measurable degradation (teeth).
[ -s "$MKV2" ] || { echo "FAIL: heavy run produced no MKV (engine hang/crash under loss)"; fail=1; }
awk -v a="${B2:-99}" -v b="${B0:-1}" -v g="${G2:-0}" 'BEGIN{exit !(a+0 <= 0.5*b || g+0 > 2.0)}' \
    || { echo "FAIL: heavy loss did not degrade (flashes ${B2} vs base ${B0}, maxgap ${G2}s) — teeth did not bite; raise OLR_SRT_LOSS_HEAVY"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: native SRT ingest recovers ${LOSS_MOD}% loss via ARQ (retrans=${RETR1}, no gaps); degrades under ${LOSS_HEAVY}%"
exit 0
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/run_srt_loss.sh`

- [ ] **Step 3: Run it and empirically tune (the key exploratory step)**

Run:
```bash
bash tests/e2e/run_srt_loss.sh build/srt/tests/e2e/sync_harness 23660; echo "exit=$?"
```
Expected: `PASS` with `exit=0`. Study the printed line for each run BEFORE trusting the result:
- **baseline** should record ~`SECS` flashes, 0 relay drops.
- **moderate** should show `relay_dropped` in the tens–hundreds, `pktRcvRetrans>0`, `pktRcvLossTotal` ≈ 0, flashes ≈ baseline, gap ≤ 1.5 s.
- **heavy** should show clearly fewer flashes and/or a multi-second gap.

Tune to the observed values (the defaults are starting points):
- If **moderate does NOT recover** (gaps, `pktRcvLossTotal>0`, or no retransmits) on a correct build — that is a real finding about the native ingest's loss handling under this latency/bitrate. Capture the full output and report it (DONE_WITH_CONCERNS); do NOT just loosen the thresholds.
- If **heavy (60 %) still fully recovers** (teeth doesn't bite), raise `OLR_SRT_LOSS_HEAVY` (e.g. 75/85) until degradation is reliable, and bake the new default into the script.
- If `MIN_DROPS` (20), `pktRcvLossTotal` tolerance (5), or the `0.85`/`0.5` ratios are marginal across 2 runs, widen them with margin and note the change.

- [ ] **Step 4: Register the CTest gate**

In `tests/e2e/CMakeLists.txt`, inside the `if(APPLE)` block, immediately after the `e2e_native_srt_reconnect` `set_tests_properties(...)` line, add:

```cmake
    # Phase 2c-b: SRT packet-loss recovery on the native ingest. A lossy_udp_relay.py
    # drops downstream SRT DATA on the link; assert ARQ recovers moderate loss (no
    # content loss, retransmits happened) and degrades under heavy loss. Base 23660.
    add_test(NAME e2e_native_srt_loss
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_loss.sh" "$<TARGET_FILE:sync_harness>" 23660)
    set_tests_properties(e2e_native_srt_loss PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 180
        RUN_SERIAL TRUE
        ENVIRONMENT "OLR_NATIVE_SRT=1")
```

- [ ] **Step 5: Reconfigure + run via CTest**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
( cd build/srt && ctest -R e2e_native_srt_loss --output-on-failure )
```
Expected: `e2e_native_srt_loss ... Passed`.

- [ ] **Step 6: Commit**

```bash
git add tests/e2e/run_srt_loss.sh tests/e2e/CMakeLists.txt
git commit -m "test(srt): e2e_native_srt_loss — SRT ARQ packet-loss recovery on native ingest"
```

---

## Task 4: Document Phase 2c-b in `SRT_README.md`

**Files:**
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Replace the "Next (Phase 2c-b / 2c-c)" section**

In `tests/e2e/SRT_README.md`, replace the final `## Next (Phase 2c-b / 2c-c)` section with:

```markdown
## Phase 2c-b: packet-loss recovery

`e2e_native_srt_loss` (label `native-apple-ingest`, `OLR_NATIVE_SRT=1`) proves the
native SRT ingest recovers from packet loss via SRT's ARQ retransmit. A
`lossy_udp_relay.py` sits on the SRT link between the engine (SRT caller) and
`srt-live-transmit` (SRT listener) and drops a **seeded % of downstream SRT DATA
packets only** — SRT control (ACK/NAK/keepalive; first byte high bit set) always
passes, so loss must be injected on the *data* path while ARQ signaling stays intact.

One source is recorded through the relay at three loss levels:

- **0 % baseline** → reference flash count `B`, relay drops nothing.
- **12 % moderate** → the relay dropped data; the native session's `srt_stats`
  (`pktRcvRetrans > 0`, `pktRcvLossTotal ≈ 0`) proves ARQ retransmitted and
  recovered the loss; the recorded view keeps ≥ 0.85·`B` flashes with no gap > 1.5 s.
- **60 % heavy (teeth)** → the run exits cleanly AND content measurably degrades
  (≤ 0.5·`B` flashes or a gap > 2 s) — proving the injected loss is real and the gate
  would catch a recovery failure.

The `srt_stats` line (`pktRcvRetrans`/`pktRcvLossTotal`/`pktRecvTotal`, logged by the
native ingest on stop) is the airtight proof that recovery happened *via
retransmission*, not luck. Loss %, seed, and thresholds are env-overridable
(`OLR_SRT_LOSS_*`); the fixed seed makes drops deterministic.

## Next (Phase 2c-c)

Long-run drift over SRT, plus (later) multi-source simultaneous loss and
jitter/reordering injection — each its own spec under `docs/superpowers/specs/`.
```

- [ ] **Step 2: Commit**

```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): document Phase 2c-b packet-loss recovery gate"
```

---

## Final: full suites + branch finish

- [ ] **Step 1: Native + ffmpeg suites green**

Run:
```bash
"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness record_harness
( cd build/srt && ctest -L native-apple-ingest --output-on-failure )
( cd build/srt && ctest -L srt --output-on-failure )
```
Expected: native-apple-ingest 7/7 (incl. `e2e_native_srt_loss`); ffmpeg srt 5/5.

- [ ] **Step 2: CI selection unaffected**

Run: `( cd build/srt && ctest -N -LE 'sync-report|srt|native-apple-ingest' | grep -ciE "loss|native" || echo CLEAN )`
Expected: `CLEAN` (the loss gate is local-only).

- [ ] **Step 3: Final review + finish**

Dispatch a final whole-branch reviewer (focus: the relay's data-only drop correctness, the `srt_bstats` lifecycle/thread-safety, and the gate's threshold soundness + teeth), then use `superpowers:finishing-a-development-branch` to open the PR **green but UNMERGED**. clang-format the changed C++ lines (Homebrew LLVM `git-clang-format` vs origin/main); push with `SKIP_IOS_BUILD=1` (the iOS app build is unaffected and the hook fails environmentally) — note the `nativesrtingestsession` change DOES compile into the iOS app, so if feasible do a direct iOS build like Phase 2b (seed rtmidi via `-DFETCHCONTENT_SOURCE_DIR_RTMIDI`, `safe.bareRepository=all`, `-DOLR_ENABLE_STREAMDECK=OFF`) to validate it.

---

## Self-review notes (for the implementer)

- **Spec coverage:** relay data-only drop (spec §Component 1) = Task 1; `srt_bstats` telemetry (§Component 2) = Task 2; the 3-run gate + assertions (§Component 3) = Task 3; CMake + docs (§Component 4) = Task 3 Step 4 + Task 4.
- **The recovery proof is the `srt_stats` retransmit line**, not just content count — don't drop the Task 2 telemetry.
- **This is exploratory.** A moderate-loss recovery FAILURE on a correct build is a real finding (report it); a heavy-loss non-degradation means raise the loss. Widen thresholds, never delete a gate.
- **bash 3.2 / python3:** no `${arr[-1]}`; `$SRT_LAST_PID`/`$!` right after each spawn; `command -v python3` SKIP guard; relay stats via a FILE (SIGTERM-flushed), not stdout.