# SRT Disconnect/Reconnect Survival (Phase 2c-a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Implementation outcome (superseding Task 2/3 names below):** the gate uncovered a
> cross-source coupling on the **ffmpeg** ingest (a dead source's avformat reconnect
> churn starves the other sources via libsrt's global receive thread). The **native
> Apple SRT ingest** (`OLR_NATIVE_SRT=1`) does not, so the shipped gate is
> **`e2e_native_srt_reconnect`** in the **`native-apple-ingest`** label (not
> `e2e_srt_reconnect`/`srt`), and the control-isolation check is **strict** (no
> mid-record disconnect, no content gap). The shipped `run_srt_reconnect.sh` isolation
> check is gap-based, not the in-window-flash sample below. See `tests/e2e/SRT_README.md`.

**Goal:** Prove over real `srt://` that the engine survives a mid-recording source drop — observes the disconnect, reconnects when the source returns, and resumes recording real frames (not frozen/blue-fill).

**Architecture:** A new local-only e2e gate (`run_srt_reconnect.sh` → CTest `e2e_srt_reconnect`) orchestrates kill/restart of a victim SRT bridge mid-record while a control source stays live, asserting a per-source connection-transition sequence + flash-PTS content resumption. One additive `sync_harness` flag (`--report-connection-events`) exposes the timestamped transitions. Builds on Phase 2b's `srt_lib.sh`.

**Tech Stack:** Bash (macOS 3.2-safe), ffmpeg/ffprobe (libsrt-enabled local build), `srt-live-transmit`, Qt6 Core, CMake/CTest, the OpenLiveReplay engine.

**Spec:** `docs/superpowers/specs/2026-06-16-srt-reconnect-2c-design.md`

**Reference (proven prior art):**
- `tests/e2e/srt_lib.sh` — `srt_require_tools`, `srt_caller_url`, `srt_bridge <udp> <srt>`, `flash_marker_to_udps <port...>` (one ffmpeg tee'd to the given ports; call it with ONE port to get a dedicated producer), `flash_pts_series <mkv> <vidx>`, `SRT_LAST_PID`.
- `tests/e2e/sync_harness.cpp` — `argValue`/`allUrls`, the existing `--report-connections` block (lines 51, 84–91, 118–121), the `QTimer::singleShot` stop/print block.
- `tests/e2e/run_srt_connect.sh` — the background/foreground harness + per-source orchestration pattern.
- `tests/e2e/CMakeLists.txt:133–135` — the `e2e_srt_connect` block to mirror (add after it).

**Key constraints (from the spec + the design review):**
- **The outage MUST exceed the engine's 8 s stall window.** Disconnect is detected via the stall timeout (`m_stallTimeoutMs`=8000) or the 5 s socket `rw_timeout`; the SRT recv buffer keeps draining after the bridge dies. A short outage is masked → false-pass. Defaults: kill@10 s, restart@20 s (10 s outage), record 38 s.
- macOS default bash is 3.2 (CI runs scripts via `/bin/bash`): NO `${arr[-1]}`; capture `$!`/`$SRT_LAST_PID` immediately after each spawn.
- These validate an EXISTING engine feature (reconnect). The gate should PASS on a correct build; the `OLR_SRT_RECONN_NO_RESTART=1` teeth (→ FAIL) is the "watch it fail" analog. If the NORMAL run fails, that is a real engine finding — report it, do not loosen the gate.

---

## Task 0: One-time prerequisites — SRT ffmpeg build + configured build dir

Environment setup (no test). Skip the build if `macos_build/ffmpeg-srt/lib/libavformat.dylib` already exists.

**Files:** none (gitignored build artifacts).

- [ ] **Step 1: Confirm tools + seed the ffmpeg tarball to skip the download**

Run:
```bash
command -v srt-live-transmit
mkdir -p ios_build/src
cp /Users/timo.korkalainen/Development/timo/OpenLiveReplay/ios_build/src/ffmpeg-8.0.tar.bz2 ios_build/src/ 2>/dev/null || true
```
Expected: a path for `srt-live-transmit`. (If missing: `brew install srt openssl@3`.)

- [ ] **Step 2: Build the libsrt-enabled ffmpeg (~10 min, one-time)**

Run (skip if `macos_build/ffmpeg-srt/lib/libavformat.dylib` exists):
```bash
bash build-scripts/build_ffmpeg_macos_srt.sh
otool -L macos_build/ffmpeg-srt/lib/libavformat.*.dylib | grep -i srt
```
Expected: `libavformat` links `libsrt`.

- [ ] **Step 3: Configure build/srt and build the harnesses**

Run:
```bash
cmake -S . -B build/srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_MAKE_PROGRAM="$HOME/Qt/Tools/Ninja/ninja" \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness record_harness
```
Expected: `build/srt/tests/e2e/sync_harness` builds cleanly.

- [ ] **Step 4: Baseline — the existing SRT gates pass**

Run: `( cd build/srt && ctest -L srt --output-on-failure )`
Expected: `e2e_srt_smoke`, `e2e_srt_4cam`, `e2e_srt_sync`, `e2e_srt_trim`, `e2e_srt_connect` all pass (confirms the SRT toolchain is healthy before adding the new gate).

---

## Task 1: `sync_harness` `--report-connection-events` flag

**Files:**
- Modify: `tests/e2e/sync_harness.cpp`

- [ ] **Step 1: Add includes**

In `tests/e2e/sync_harness.cpp`, after `#include <QSet>` (line 16), add:

```cpp
#include <QHash>
#include <QVector>
#include <QPair>
#include <QList>
#include <QElapsedTimer>
#include <algorithm>
```

- [ ] **Step 2: Parse the flag**

After the existing `const bool reportConnections = ...` line (line 51), add:

```cpp
    const bool reportConnectionEvents = args.contains(QStringLiteral("--report-connection-events"));
```

- [ ] **Step 3: Record per-source timestamped transitions**

Replace the existing connection-tracking block (lines 84–91):

```cpp
    // Count distinct sources that reach the connected state. Queued to the app
    // (main) thread — the signal is emitted from a worker thread. Connected
    // before startRecording() so no early connect is missed.
    QSet<int> connectedSources;
    QObject::connect(&rm, &ReplayManager::sourceConnectionChanged, &app,
                     [&connectedSources](int sourceIndex, bool connected) {
                         if (connected) connectedSources.insert(sourceIndex);
                     });
```

with:

```cpp
    // Per-source connection observability. connectionChanged fires on EVERY real
    // transition (StreamWorker::setConnected emits only on change), queued to this
    // (main) thread, so the captures below are mutated single-threaded. Wired before
    // startRecording() so no early connect is missed.
    //  - connectedSources: distinct sources ever connected (for --report-connections).
    //  - connEvents: per source, a chronological (elapsedMs, connected) timeline
    //    (for --report-connection-events), used by the reconnect e2e to verify a
    //    real up->down->up sequence.
    QSet<int> connectedSources;
    QHash<int, QVector<QPair<qint64, bool>>> connEvents;
    QElapsedTimer connTimer;
    connTimer.start();
    QObject::connect(&rm, &ReplayManager::sourceConnectionChanged, &app,
                     [&connectedSources, &connEvents, &connTimer](int sourceIndex, bool connected) {
                         if (connected) connectedSources.insert(sourceIndex);
                         connEvents[sourceIndex].append(qMakePair(connTimer.elapsed(), connected));
                     });
```

- [ ] **Step 4: Print the per-source timeline on stop**

In the inner `QTimer::singleShot(700, ...)` lambda, after the existing `if (reportConnections) { ... }` block (lines 118–121) and before `app.quit();`, add:

```cpp
            if (reportConnectionEvents) {
                QList<int> srcs = connEvents.keys();
                std::sort(srcs.begin(), srcs.end());
                for (int src : srcs) {
                    QString line = QStringLiteral("conn_events src=%1").arg(src);
                    const QVector<QPair<qint64, bool>>& evs = connEvents.value(src);
                    for (const QPair<qint64, bool>& ev : evs)
                        line += QStringLiteral(" %1:%2").arg(ev.first)
                                    .arg(ev.second ? QStringLiteral("up") : QStringLiteral("down"));
                    fprintf(stderr, "%s\n", qPrintable(line));
                }
                fflush(stderr);
            }
```

- [ ] **Step 5: Build**

Run: `"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness`
Expected: clean (warnings are errors — watch for unused/signedness).

- [ ] **Step 6: Verify over plain UDP (transport-agnostic, fast)**

The flag is transport-independent, so a single UDP source must produce one `up` event. Run:
```bash
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=320x240:rate=30" \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 \
  -f mpegts "udp://127.0.0.1:23599?pkt_size=1316" & FFPID=$!
sleep 1
build/srt/tests/e2e/sync_harness --url "udp://127.0.0.1:23599?fifo_size=1000000&overrun_nonfatal=1" \
  --report-connection-events --outdir /tmp --name evprobe --seconds 5 --fps 30 \
  >/tmp/ev.out 2>/tmp/ev.err
kill "$FFPID" 2>/dev/null
echo "--- events ---"; grep '^conn_events' /tmp/ev.err; echo "--- stdout path ---"; cat /tmp/ev.out
```
Expected: a line like `conn_events src=0 <ms>:up` on stderr (the `<ms>` is the connect time), and the MKV path on stdout. Confirms the timeline records, the `up` token is correct, and stdout stays the path-only contract.

- [ ] **Step 7: Verify off-by-default (no regression)**

Run:
```bash
build/srt/tests/e2e/sync_harness --url "udp://127.0.0.1:23599?fifo_size=1000000&overrun_nonfatal=1" \
  --outdir /tmp --name evprobe2 --seconds 2 --fps 30 2>/tmp/ev2.err >/dev/null
grep -c '^conn_events' /tmp/ev2.err
```
Expected: `0` (no `conn_events` line without the flag).

- [ ] **Step 8: Commit**

```bash
git add tests/e2e/sync_harness.cpp
git commit -m "test(srt): sync_harness --report-connection-events — per-source transition timeline"
```

---

## Task 2: `run_srt_reconnect.sh` gate + `e2e_srt_reconnect`

**Files:**
- Create: `tests/e2e/run_srt_reconnect.sh`
- Modify: `tests/e2e/CMakeLists.txt` (add after the `e2e_srt_connect` block, ~line 135)

- [ ] **Step 1: Write the gate script**

Create `tests/e2e/run_srt_reconnect.sh` with exactly this content:

```bash
#!/usr/bin/env bash
# Local SRT e2e (Phase 2c-a): mid-recording disconnect/reconnect survival.
#
# Two INDEPENDENT flash sources (separate producers + bridges): src0 control
# (never touched), src1 victim. We record both, KILL src1's bridge mid-record,
# then RESTART it on the same port, and prove the engine: (a) observed the drop
# and reconnected (per-source conn_events sequence up->down->up), (b) resumed
# recording REAL frames for src1 (flashes in a late post-reconnect window), while
# (c) src0 kept recording throughout (the outage was isolated).
#
# The outage MUST exceed the engine's ~8s stall window: disconnect is detected via
# the stall timeout / 5s rw_timeout, and the SRT recv buffer keeps draining buffered
# packets after the bridge dies, so a short outage is masked (false-pass). Hence
# kill@10s / restart@20s (10s outage) / record 38s by default.
#
# Teeth: OLR_SRT_RECONN_NO_RESTART=1 skips the restart -> src1 never reconnects ->
# no second 'up' and no late flashes -> FAIL, proving the gate discriminates.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX.
# Usage: run_srt_reconnect.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23550}"
KILL_TIME="${OLR_SRT_RECONN_KILL:-10}"
RESTART_TIME="${OLR_SRT_RECONN_RESTART:-20}"
DURATION="${OLR_SRT_RECONN_DURATION:-38}"
LATE_MIN="${OLR_SRT_RECONN_LATE_MIN:-$((RESTART_TIME + 6))}"
NO_RESTART="${OLR_SRT_RECONN_NO_RESTART:-0}"

SRT0=$BASE;       UDP0=$((BASE+1))
SRT1=$((BASE+2)); UDP1=$((BASE+3))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-reconn] base=$BASE kill@${KILL_TIME}s restart@${RESTART_TIME}s dur=${DURATION}s late_min=${LATE_MIN}s no_restart=${NO_RESTART}"

# 1. Two INDEPENDENT flash sources (a dedicated producer per source) + bridges.
flash_marker_to_udps "$UDP0"                       # src0 control producer
srt_bridge "$UDP0" "$SRT0"
flash_marker_to_udps "$UDP1"                       # src1 victim producer
srt_bridge "$UDP1" "$SRT1"; SRC1_BRIDGE_PID=$SRT_LAST_PID
sleep 1.5  # warm-up: both connect before the kill

# 2. Record both in the BACKGROUND so the script can kill/restart mid-record.
"$HARNESS" --url "$(srt_caller_url "$SRT0")" --url "$(srt_caller_url "$SRT1")" \
    --outdir "$WORKDIR" --name srtreconn --seconds "$DURATION" --fps 30 \
    --report-connection-events >"$WORKDIR/out.txt" 2>"$WORKDIR/err.txt" &
HARNESS_PID=$!
PIDS+=("$HARNESS_PID")

# 3. Mid-record: kill src1's bridge (network drop; src1's producer keeps running).
sleep "$KILL_TIME"
echo "[srt-reconn] killing src1 bridge (pid $SRC1_BRIDGE_PID) at ~${KILL_TIME}s"
kill "$SRC1_BRIDGE_PID" 2>/dev/null

# 4. Restart src1's bridge on the SAME port (unless teeth mode).
if [ "$NO_RESTART" = "1" ]; then
    echo "[srt-reconn] (teeth) NOT restarting src1 bridge"
else
    sleep "$(( RESTART_TIME - KILL_TIME ))"
    echo "[srt-reconn] restarting src1 bridge at ~${RESTART_TIME}s"
    # Retry the rebind in case the OS briefly holds the port after the kill.
    for attempt in 1 2 3 4 5; do
        srt_bridge "$UDP1" "$SRT1"; SRC1_BRIDGE_PID=$SRT_LAST_PID
        sleep 0.3
        if kill -0 "$SRC1_BRIDGE_PID" 2>/dev/null; then break; fi
    done
fi

# 5. Wait for the recording to finish; collect outputs.
wait "$HARNESS_PID"
MKV="$(tail -n 1 "$WORKDIR/out.txt")"
echo "[srt-reconn] out=$MKV"
echo "[srt-reconn] conn_events:"; grep '^conn_events' "$WORKDIR/err.txt" || echo "  (none)"

if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: no output MKV (engine could not record — built with -DOLR_FFMPEG_SRT_PREFIX?)"; exit 1
fi
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "2" ]; then echo "FAIL: expected 2 video tracks, got ${VTRACKS:-0}"; exit 1; fi

fail=0

# --- Assertion A: src1 sequence up(<kill) -> down(>kill) -> up(after the down). ---
EV1="$(grep '^conn_events src=1' "$WORKDIR/err.txt" | head -1)"
SEQ_OK="$(echo "$EV1" | awk -v kill="$((KILL_TIME*1000))" '
    {
        init_up=0; down_t=-1; reconn=0
        for (i=3;i<=NF;i++){
            split($i,a,":"); t=a[1]+0; st=a[2]
            if (st=="up" && down_t<0 && t<kill) init_up=1
            else if (st=="down" && init_up && t>kill && down_t<0) down_t=t
            else if (st=="up" && down_t>=0 && t>down_t) reconn=1
        }
        print (init_up && down_t>=0 && reconn) ? "1" : "0"
    }')"
if [ "$SEQ_OK" != "1" ]; then
    echo "FAIL: src1 did not show up(before kill)->down(after kill)->reconnect. events: ${EV1:-none}"; fail=1
fi

# --- Assertion B: control src0 never disconnected + has flashes during the outage. ---
EV0="$(grep '^conn_events src=0' "$WORKDIR/err.txt" | head -1)"
if echo "$EV0" | grep -q ":down"; then
    echo "FAIL: control src0 saw a disconnect — outage not isolated. events: $EV0"; fail=1
fi
flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
SPAN0="$(awk -v a="$KILL_TIME" -v b="$RESTART_TIME" '$1>a && $1<b{n++} END{print n+0}' "$WORKDIR/v0.txt")"
if [ "${SPAN0:-0}" -lt 1 ]; then
    echo "FAIL: control src0 has no flashes during the src1 outage window — not truly isolated"; fail=1
fi

# --- Assertion C (the real teeth): src1 has pre-kill AND post-reconnect content. ---
flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
PRE1="$(awk -v k="$KILL_TIME" '$1<k{n++} END{print n+0}' "$WORKDIR/v1.txt")"
LATE1="$(awk -v m="$LATE_MIN" '$1>m{n++} END{print n+0}' "$WORKDIR/v1.txt")"
echo "[srt-reconn] src1 flashes: pre_kill=$PRE1 post_reconnect(>${LATE_MIN}s)=$LATE1"
if [ "${PRE1:-0}" -lt 1 ]; then echo "FAIL: src1 had no pre-kill flashes (never recorded before the drop)"; fail=1; fi
if [ "${LATE1:-0}" -lt 1 ]; then echo "FAIL: src1 had no post-reconnect flashes — content did NOT resume after reconnect"; fail=1; fi

# --- Diagnostic (non-gating): re-anchor offset src1 vs src0 in the late window. ---
OFF="$(awk -v m="$LATE_MIN" '
    FNR==NR { if($1>m) a[++na]=$1; next }
    { if($1>m) b[++nb]=$1 }
    END {
        if (na<1||nb<1){print "nan"; exit}
        s=0; cnt=0
        for (j=1;j<=nb;j++){ best=1e9; bv=0; for(i=1;i<=na;i++){d=b[j]-a[i]; ad=(d<0?-d:d); if(ad<best){best=ad; bv=d}} s+=bv; cnt++ }
        printf "%.1f", (cnt? s/cnt*1000 : 0)
    }' "$WORKDIR/v0.txt" "$WORKDIR/v1.txt")"
echo "[srt-reconn] reanchor_offset_ms=$OFF (diagnostic; re-anchored to fresh arrival, expected nonzero)"

[ $fail -ne 0 ] && exit 1
echo "PASS: SRT disconnect/reconnect survival — src1 dropped, reconnected, resumed real content; src0 isolated"
exit 0
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/run_srt_reconnect.sh`

- [ ] **Step 3: Run the TEETH config — expect FAIL (proves the gate discriminates)**

Run:
```bash
OLR_SRT_RECONN_NO_RESTART=1 bash tests/e2e/run_srt_reconnect.sh build/srt/tests/e2e/sync_harness 23550; echo "exit=$?"
```
Expected: the `conn_events src=1` line shows an `up` then a `down` but **no second up**; a `FAIL: src1 did not show ... reconnect` and/or `FAIL: src1 had no post-reconnect flashes`; `exit=1`. (This takes ~40 s — it records the full duration.) Confirm src1 actually went `down` (proving the kill was detected); if src1 shows NO `down` at all, the outage was too short to be detected — increase `OLR_SRT_RECONN_KILL`/`OLR_SRT_RECONN_RESTART` spacing.

- [ ] **Step 4: Run the NORMAL config — expect PASS**

Run:
```bash
bash tests/e2e/run_srt_reconnect.sh build/srt/tests/e2e/sync_harness 23550; echo "exit=$?"
```
Expected: `conn_events src=1` shows `up ... down ... up`; `src1 flashes: pre_kill=N post_reconnect=M` with both N≥1 and M≥1; `PASS`; `exit=0`. Record the printed `reanchor_offset_ms`. **If the normal run FAILS** (no reconnect, or no post-reconnect flashes) on a correct build, that is a real engine reconnect bug — capture the full output and report it (DONE_WITH_CONCERNS); do NOT loosen the gate to make it pass. If it's a timing miss (src1 `down` fires too close to the restart, so reconnect is racy), widen the outage (`OLR_SRT_RECONN_RESTART=24`, `OLR_SRT_RECONN_DURATION=44`, `OLR_SRT_RECONN_LATE_MIN=30`) and re-run — widen, never delete.

- [ ] **Step 5: Register the CTest gate**

In `tests/e2e/CMakeLists.txt`, immediately after the `e2e_srt_connect` block (the `set_tests_properties(e2e_srt_connect ...)` line near line 135), add:

```cmake
# Phase 2c-a: mid-recording disconnect/reconnect survival over real SRT — kill a
# victim bridge, restart it, prove the engine reconnects and resumes real content
# while a control source is isolated. Base port 23550. Longer record (~38s) +
# reconnect backoff => TIMEOUT 240.
add_test(NAME e2e_srt_reconnect
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_reconnect.sh" "$<TARGET_FILE:sync_harness>" 23550)
set_tests_properties(e2e_srt_reconnect PROPERTIES LABELS "srt" TIMEOUT 240 RUN_SERIAL TRUE)
```

- [ ] **Step 6: Reconfigure and run the gate via CTest**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
( cd build/srt && ctest -R e2e_srt_reconnect --output-on-failure )
```
Expected: `e2e_srt_reconnect ... Passed`.

- [ ] **Step 7: Commit**

```bash
git add tests/e2e/run_srt_reconnect.sh tests/e2e/CMakeLists.txt
git commit -m "test(srt): e2e_srt_reconnect — mid-recording disconnect/reconnect survival"
```

---

## Task 3: Document Phase 2c-a in `SRT_README.md`

**Files:**
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Replace the "Next (Phase 2c)" section**

In `tests/e2e/SRT_README.md`, replace the final `## Next (Phase 2c)` section (from that heading to end of file) with:

```markdown
## Phase 2c-a: disconnect/reconnect survival

`e2e_srt_reconnect` proves the engine survives a mid-recording source drop. Two
independent flash sources record (src0 control, src1 victim); the script kills
src1's `srt-live-transmit` bridge mid-record and restarts it on the same port, then
asserts:

- **src1 reconnected:** its per-source `conn_events` timeline (from
  `sync_harness --report-connection-events`) shows `up`(before kill) → `down`(after
  kill) → `up`(reconnect) — a true reconnect, not a flaky warm-up.
- **content resumed:** src1's view has flashes both before the kill and in a late
  post-reconnect window (`flash_pts_series`) — real frames resumed, not a frozen
  frame or blue-fill.
- **outage isolated:** src0 never disconnected and kept flashing throughout.

It also reports a non-gating `reanchor_offset_ms` (src1 re-anchors to fresh arrival
on reconnect — audit REF-4/5 — so a nonzero offset is expected).

**The outage must exceed the engine's ~8 s stall window.** Disconnect is detected via
the stall timeout / 5 s socket `rw_timeout`, and the SRT receive buffer keeps draining
buffered packets after the bridge dies — so a short outage is *masked* and the test
would false-pass. Defaults: kill@10 s, restart@20 s (10 s outage), record 38 s
(`TIMEOUT 240`). All timings are env-overridable (`OLR_SRT_RECONN_KILL`,
`OLR_SRT_RECONN_RESTART`, `OLR_SRT_RECONN_DURATION`, `OLR_SRT_RECONN_LATE_MIN`).
Teeth: `OLR_SRT_RECONN_NO_RESTART=1` skips the restart → src1 never reconnects → FAIL.

## Next (Phase 2c-b / 2c-c)

Packet-loss / jitter injection (needs privileged macOS network emulation) and
long-run drift over SRT — each its own spec under `docs/superpowers/specs/`.
```

- [ ] **Step 2: Commit**

```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): document Phase 2c-a disconnect/reconnect gate"
```

---

## Final: full-suite check + branch finish

- [ ] **Step 1: Whole local SRT label, end to end**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness record_harness
( cd build/srt && ctest -L srt --output-on-failure )
```
Expected: all 6 `srt` gates pass — `e2e_srt_smoke`, `e2e_srt_4cam`, `e2e_srt_sync`, `e2e_srt_trim`, `e2e_srt_connect`, `e2e_srt_reconnect`.

- [ ] **Step 2: Confirm CI selection is unaffected**

Run: `( cd build/srt && ctest -N -LE 'sync-report|srt' | grep -oE "Test #[0-9]+: [a-z0-9_]+" | grep -iE "srt|sync" || echo CLEAN )`
Expected: `CLEAN` (no `srt`/`sync` test in the CI selection).

- [ ] **Step 3: Final whole-branch review + finish**

Dispatch a final whole-branch code reviewer, then use `superpowers:finishing-a-development-branch` to open the PR **green but UNMERGED** (the user merges). When pushing, the iOS pre-push hook will fail on the harness's `GIT_CONFIG` injection (environmental, not the code) — use `SKIP_IOS_BUILD=1 git push` and clang-format-check changed lines with the Homebrew LLVM `git-clang-format` first.

---

## Self-review notes (for the implementer)

- **Spec coverage:** harness flag (spec §Component 1) = Task 1; the gate + all four assertions + teeth + diagnostic (§Component 2) = Task 2; CMake + docs (§Component 3) = Task 2 Step 5 + Task 3. The >8 s-outage constraint is baked into the default timings.
- **This validates an existing feature** (reconnect). The teeth (`NO_RESTART=1` → FAIL) is the discriminator; the normal run should PASS, and a normal-run failure is a real engine finding to escalate.
- **bash 3.2:** no `${arr[-1]}`; `$SRT_LAST_PID`/`$!` captured right after each spawn; the victim bridge PID is tracked explicitly in `SRC1_BRIDGE_PID`.
- **Thresholds widen, never delete.** If the gate flakes, increase the outage/record/late-window via the env vars and bake the new defaults in.
