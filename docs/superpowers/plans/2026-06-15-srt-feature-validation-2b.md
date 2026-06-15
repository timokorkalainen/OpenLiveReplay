# SRT Feature Validation (Phase 2b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove — over real `srt://` transport — that inter-camera sync stays within a bounded window, per-source trim (#28) shifts a source by the set amount, and connection-status (#24) reports/discriminates connected sources.

**Architecture:** Three new local-only e2e gate scripts (`run_srt_sync.sh`, `run_srt_trim.sh`, `run_srt_connect.sh`) that stand up flash-marker SRT streams via `srt-live-transmit` bridges, record them with the real engine through `sync_harness`, and assert pass/fail. They share a new sourced library `srt_lib.sh` (bridge + tee'd flash producer + the proven `flash_pts_series` extractor). One additive `sync_harness.cpp` flag (`--report-connections`) exposes the connection count. All three register under the existing CI-excluded `srt` CTest label.

**Tech Stack:** Bash, ffmpeg/ffprobe (libsrt-enabled local build), `srt-live-transmit` (brew `srt`), Qt6 Core, CMake/CTest, the OpenLiveReplay recording engine (`ReplayManager`).

**Spec:** `docs/superpowers/specs/2026-06-15-srt-feature-validation-2b-design.md`

**Reference (proven prior art to mirror):**
- `tests/e2e/run_srt_4cam.sh` — bridge spawn, SKIP guards, cleanup trap, ffprobe track count.
- `tests/e2e/run_sync_e2e.sh` — `flash_pts_series`, the `geq` flash marker, `intercam_matched` (tee → N views), `intercam_trim` (tee + `--trim`, signed offset).
- `tests/e2e/sync_harness.cpp` — `argValue`/`allUrls` helpers, the `QTimer::singleShot` stop/flush/print block, `--trim` → `rm.updateSourceTrim(n-1, trimMs)`.
- `tests/e2e/CMakeLists.txt:106-117` — the `srt`-label local-only test block.
- `recorder_engine/replaymanager.h:73` — `void sourceConnectionChanged(int sourceIndex, bool connected);`

**Honest-ceiling note for the implementer:** the engine anchors each source to its first-packet arrival (no genlock), so coincident SRT input is bounded-but-not-zero skew. The sync gate's bound is deliberately generous; its real teeth is the per-view flash *count* (a disconnected view = 0 flashes). Thresholds (`MIN_FLASHES`, `MAX_SPREAD_MS`, trim `T`/`TOL_MS`) are starting values — validate against real local runs and widen (never delete) a gate if it proves flaky.

**macOS bash note:** CTest runs these via `/bin/bash` (3.2). Do **not** use negative array subscripts (`${arr[-1]}`) — capture `$!`/`$SRT_LAST_PID` immediately after each background spawn instead. Arrays, `+=`, `<<<` here-strings, and `paste` are all fine in 3.2.

---

## Task 0: One-time prerequisites — SRT ffmpeg build + configured build dir

This is environment setup (no test). It produces the libsrt-enabled ffmpeg and a build dir the later gate steps run against. Skip the build step if `macos_build/ffmpeg-srt/lib/libavformat.dylib` already exists.

**Files:** none (build artifacts only; `macos_build/` and `build/` are gitignored).

- [ ] **Step 1: Confirm tools + source tarball present**

Run:
```bash
command -v srt-live-transmit && ls ios_build/src/ffmpeg-8.0.tar.bz2
```
Expected: a path for `srt-live-transmit` and the tarball listed. (If `srt-live-transmit` is missing: `brew install srt openssl@3`.)

- [ ] **Step 2: Build the libsrt-enabled ffmpeg (~10 min, one-time)**

Run (skip if `macos_build/ffmpeg-srt/lib/libavformat.dylib` already exists):
```bash
bash build-scripts/build_ffmpeg_macos_srt.sh
ls macos_build/ffmpeg-srt/lib/libavformat*.dylib
otool -L macos_build/ffmpeg-srt/lib/libavformat.*.dylib | grep -i srt
```
Expected: `libavformat` dylib exists and links `libsrt`.

- [ ] **Step 3: Configure a build dir against the SRT ffmpeg and build sync_harness**

Run:
```bash
cmake -S . -B build/srt -G Ninja -DOLR_BUILD_TESTS=ON \
  -DCMAKE_MAKE_PROGRAM="$HOME/Qt/Tools/Ninja/ninja" \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos" \
  -DOLR_FFMPEG_SRT_PREFIX="$PWD/macos_build/ffmpeg-srt"
"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness record_harness
```
Expected: `build/srt/tests/e2e/sync_harness` and `record_harness` build cleanly.

- [ ] **Step 4: Sanity-check the existing SRT gate still passes (baseline)**

Run:
```bash
( cd build/srt && ctest -L srt --output-on-failure )
```
Expected: `e2e_srt_smoke` and `e2e_srt_4cam` PASS. (Confirms the SRT toolchain is healthy before adding new gates.)

---

## Task 1: Shared SRT e2e library `srt_lib.sh`

**Files:**
- Create: `tests/e2e/srt_lib.sh`

- [ ] **Step 1: Write the library**

Create `tests/e2e/srt_lib.sh` with exactly this content:

```bash
#!/usr/bin/env bash
# Shared plumbing for the local SRT feature-validation e2e (Phase 2b), sourced by
# run_srt_sync.sh / run_srt_trim.sh / run_srt_connect.sh.
#
# Provides: tool guards, an ffmpeg full-frame-flash marker producer tee'd to N UDP
# ports (byte-identical, coincident content), srt-live-transmit UDP->SRT listener
# bridges, SRT caller URLs, and the proven per-track flash-onset extractor (copied
# from run_sync_e2e.sh).
#
# Caller contract: declare `PIDS=()` and a cleanup trap that kills "${PIDS[@]}";
# the spawn helpers append PIDs to PIDS AND set $SRT_LAST_PID to the just-spawned
# PID (read it immediately, before the next spawn). Call srt_require_tools first.
# Targets localhost only.

SRT_LAST_PID=""

# SKIP (exit 0) unless ffmpeg/ffprobe/srt-live-transmit are all present.
srt_require_tools() {
    command -v ffmpeg            >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
    command -v ffprobe           >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }
    command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 0; }
}

# SRT caller URL for a local listener port. $1=srt_port
srt_caller_url() { echo "srt://127.0.0.1:${1}?transtype=live"; }

# Spawn one srt-live-transmit UDP->SRT listener bridge. $1=udp_port $2=srt_port
srt_bridge() {
    local udp_port="$1" srt_port="$2"
    srt-live-transmit "udp://127.0.0.1:${udp_port}?mode=listener" \
        "srt://127.0.0.1:${srt_port}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Spawn ONE ffmpeg full-frame-flash producer, tee'd to all given UDP ports so every
# consumer sees byte-identical, simultaneous content. Luma flashes to white (235)
# for the first ~60ms of every source-second, else black (16).
# Usage: flash_marker_to_udps <udp_port> [<udp_port> ...]
flash_marker_to_udps() {
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    local tee="" p
    for p in "$@"; do
        [ -n "$tee" ] && tee="${tee}|"
        tee="${tee}[f=mpegts]udp://127.0.0.1:${p}?pkt_size=1316"
    done
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -map 0:v \
        -f tee "$tee" &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Rising-edge flash-onset pts_time series for one video track. $1=mkv $2=v-index.
# Emits one pts_time per flash, ascending. Detects only the full-white flash
# (YAVG>180, above the h264 cold-start gray ~128 and black base ~16).
flash_pts_series() {
    ffmpeg -hide_banner -loglevel error -i "$1" -map "0:v:$2" \
        -vf "signalstats,metadata=print:file=-" -f null - 2>/dev/null \
    | awk -v THRESH="${FLASH_THRESH:-180}" '
        /pts_time:/ { for (i=1;i<=NF;i++) if ($i ~ /^pts_time:/) { split($i,a,":"); t=a[2]+0 } }
        /YAVG=/     { split($0,b,"="); y=b[2]+0; bright=(y>THRESH);
                      if (bright && !prev) printf "%.6f\n", t; prev=bright }'
}
```

- [ ] **Step 2: Verify it parses and exports every function (the lib's "test")**

Run:
```bash
bash -n tests/e2e/srt_lib.sh && echo "SYNTAX_OK"
bash -c '. tests/e2e/srt_lib.sh; for f in srt_require_tools srt_caller_url srt_bridge flash_marker_to_udps flash_pts_series; do declare -F "$f" >/dev/null || { echo "MISSING $f"; exit 1; }; done; echo "FUNCS_OK"'
bash -c '. tests/e2e/srt_lib.sh; test "$(srt_caller_url 23520)" = "srt://127.0.0.1:23520?transtype=live" && echo "URL_OK"'
```
Expected: `SYNTAX_OK`, `FUNCS_OK`, `URL_OK`.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/srt_lib.sh
git commit -m "test(srt): shared srt_lib.sh — bridge, tee'd flash marker, flash extraction"
```

---

## Task 2: Inter-camera sync gate `run_srt_sync.sh` + `e2e_srt_sync`

**Files:**
- Create: `tests/e2e/run_srt_sync.sh`
- Modify: `tests/e2e/CMakeLists.txt` (after line 117, the `e2e_srt_4cam` block)

- [ ] **Step 1: Write the gate script**

Create `tests/e2e/run_srt_sync.sh` (and make it executable in Step 2) with exactly:

```bash
#!/usr/bin/env bash
# Local SRT e2e (Phase 2b): inter-camera sync over 4 real SRT streams.
#
# One flash producer is tee'd to 4 SRT listeners (coincident, byte-identical
# content) and recorded into a 4-view MKV. We measure the per-flash max-min PTS
# spread across the 4 views.
#   Gate A (teeth):  every view produced >= MIN_FLASHES flashes. A view whose
#     source failed to connect is blue-fill (0 flashes) -> FAIL. A dead view
#     cannot fabricate a flash, so this is the real discriminator.
#   Gate B (bound):  max spread <= MAX_SPREAD_MS. Deliberately generous: the
#     engine anchors each source to first-packet ARRIVAL (no genlock, audit REF-2),
#     so coincident SRT is phase-locked-within-bounds, not zero-skew.
#
# Teeth demo: OLR_SRT_SYNC_DROP_VIEW=<i> skips view i's bridge so that view records
#   no flash and Gate A FAILS — proving the content gate discriminates.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX (an SRT-enabled avformat).
# Usage: run_srt_sync.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23520}"
SECS=8
MIN_FLASHES="${OLR_SRT_SYNC_MIN_FLASHES:-4}"
MAX_SPREAD_MS="${OLR_SRT_SYNC_MAX_SPREAD_MS:-250}"
DROP_VIEW="${OLR_SRT_SYNC_DROP_VIEW:-}"

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-sync] base_port=$BASE drop_view=${DROP_VIEW:-none}"

# 1. One flash producer tee'd to 4 UDP ports; 4 SRT listener bridges; 4 caller URLs.
UDP_PORTS=(); SRT_PORTS=(); URLS=()
for i in 0 1 2 3; do
    srt_port=$((BASE + 2*i)); udp_port=$((BASE + 2*i + 1))
    SRT_PORTS+=("$srt_port"); UDP_PORTS+=("$udp_port")
    URLS+=("$(srt_caller_url "$srt_port")")
done
flash_marker_to_udps "${UDP_PORTS[@]}"
for i in 0 1 2 3; do
    if [ "$DROP_VIEW" = "$i" ]; then
        echo "[srt-sync] (teeth) skipping bridge for view $i"
        continue
    fi
    srt_bridge "${UDP_PORTS[$i]}" "${SRT_PORTS[$i]}"
done
sleep 1.5  # let the producer + listeners come up before the engine connects

# 2. Record the 4 views.
OUT="$("$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
       --outdir "$WORKDIR" --name srtsync --seconds "$SECS" --fps 30)"
RC=$?
MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-sync] harness rc=$RC out=$MKV"
if [ $RC -ne 0 ] || [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: no output (rc=$RC) — engine could not record SRT (built with -DOLR_FFMPEG_SRT_PREFIX?)"; exit 1
fi

VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "4" ]; then echo "FAIL: expected 4 video tracks, got ${VTRACKS:-0}"; exit 1; fi

# 3. Flash-onset series per view.
for i in 0 1 2 3; do flash_pts_series "$MKV" "$i" > "$WORKDIR/v$i.txt"; done

# Gate A: every view has >= MIN_FLASHES flashes.
fail=0; counts=""
for i in 0 1 2 3; do
    c=$(wc -l < "$WORKDIR/v$i.txt" | tr -d ' '); counts="$counts v$i=$c"
    if [ "${c:-0}" -lt "$MIN_FLASHES" ]; then
        echo "FAIL: view $i produced ${c:-0} flashes (< $MIN_FLASHES) — source likely never connected (blue-fill)"; fail=1
    fi
done
echo "[srt-sync] flash_counts:$counts (min_required=$MIN_FLASHES)"

# Gate B: per-flash max-min spread across the 4 views (index-paired via paste).
STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" "$WORKDIR/v2.txt" "$WORKDIR/v3.txt" | awk '
    NF==4 {
        mn=$1; mx=$1
        for (k=2;k<=4;k++){ if($k<mn)mn=$k; if($k>mx)mx=$k }
        d=(mx-mn)*1000; s+=d; if(d>peak)peak=d; n++
    }
    END { if(n>0) printf "%d %.1f %.1f", n, s/n, peak; else printf "0 nan nan" }')
read -r NP MEANSPREAD MAXSPREAD <<<"$STATS"
echo "[srt-sync] flashes_paired=$NP spread_ms: mean=$MEANSPREAD max=$MAXSPREAD (bound=$MAX_SPREAD_MS)"

if [ "${NP:-0}" -lt "$MIN_FLASHES" ]; then
    echo "FAIL: only $NP flashes paired across all 4 views (< $MIN_FLASHES)"; fail=1
fi
if [ "$MAXSPREAD" = "nan" ] || ! awk -v m="$MAXSPREAD" -v b="$MAX_SPREAD_MS" 'BEGIN{exit !(m+0 <= b+0)}'; then
    echo "FAIL: max inter-camera flash spread ${MAXSPREAD}ms exceeds bound ${MAX_SPREAD_MS}ms"; fail=1
fi

[ $fail -ne 0 ] && exit 1
echo "PASS: 4-source SRT inter-camera sync — all views live, max spread ${MAXSPREAD}ms <= ${MAX_SPREAD_MS}ms"
exit 0
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/run_srt_sync.sh`

- [ ] **Step 3: Run the TEETH config — expect FAIL (proves the gate discriminates)**

Run:
```bash
OLR_SRT_SYNC_DROP_VIEW=3 bash tests/e2e/run_srt_sync.sh build/srt/tests/e2e/sync_harness 23520; echo "exit=$?"
```
Expected: a `FAIL: view 3 produced 0 flashes ...` line and `exit=1`. (View 3 has no bridge → blue-fill → 0 flashes.)

- [ ] **Step 4: Run the NORMAL config — expect PASS with margin**

Run:
```bash
bash tests/e2e/run_srt_sync.sh build/srt/tests/e2e/sync_harness 23520; echo "exit=$?"
```
Expected: `PASS: ...` and `exit=0`. Note the printed `spread_ms: max=...` — it should sit comfortably under 250. If `max` is regularly within ~50ms of the bound, raise `MAX_SPREAD_MS` in the script (widen, don't delete) and re-run.

- [ ] **Step 5: Register the CTest gate**

In `tests/e2e/CMakeLists.txt`, immediately after the `e2e_srt_4cam` block (the `set_tests_properties(e2e_srt_4cam ...)` line near line 117), add:

```cmake
# Phase 2b: inter-camera sync over 4 real SRT streams (generous bound + per-view
# flash-count content gate). Base port 23520.
add_test(NAME e2e_srt_sync
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_sync.sh" "$<TARGET_FILE:sync_harness>" 23520)
set_tests_properties(e2e_srt_sync PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)
```

- [ ] **Step 6: Reconfigure and run the gate via CTest**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
( cd build/srt && ctest -R e2e_srt_sync --output-on-failure )
```
Expected: `e2e_srt_sync ... Passed`.

- [ ] **Step 7: Commit**

```bash
git add tests/e2e/run_srt_sync.sh tests/e2e/CMakeLists.txt
git commit -m "test(srt): e2e_srt_sync — inter-camera sync bound over 4 real SRT streams"
```

---

## Task 3: Per-source trim gate `run_srt_trim.sh` + `e2e_srt_trim`

**Files:**
- Create: `tests/e2e/run_srt_trim.sh`
- Modify: `tests/e2e/CMakeLists.txt` (after the `e2e_srt_sync` block)

- [ ] **Step 1: Write the gate script**

Create `tests/e2e/run_srt_trim.sh` with exactly:

```bash
#!/usr/bin/env bash
# Local SRT e2e (Phase 2b): per-source trim (#28) over real SRT.
#
# Port of the proven synthetic intercam_trim to SRT. One flash producer is tee'd
# to TWO coincident SRT views; we record once untrimmed and once with +T trim on
# the LAST source (view1), and measure the mean (view0 - view1) flash offset each
# time. The trim DELAYS view1, so its flash PTS increases and the signed
# (view0 - view1) offset DECREASES by ~T (matches intercam_trim: "delay =>
# trimmed ≈ untrimmed − TRIM").
#   Gate: (trimmed_offset - untrimmed_offset) ≈ -T within TOL_MS.
# A no-op/broken trim leaves the two offsets equal (diff ≈ 0) and FAILS the gate.
# Measuring the run-to-run DIFFERENCE cancels any systematic per-view connect-order
# bias, leaving only arrival jitter (which TOL_MS covers).
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX.
# Usage: run_srt_trim.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23530}"
SECS=8
T="${OLR_SRT_TRIM_MS:-300}"
TOL="${OLR_SRT_TRIM_TOL_MS:-120}"
SRT0=$BASE;       UDP0=$((BASE+1))
SRT1=$((BASE+2)); UDP1=$((BASE+3))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-trim] base_port=$BASE T=${T}ms tol=${TOL}ms"

# Record the tee'd 2-view setup with a given trim on the LAST source (view1) and
# echo the mean (view0 - view1) flash offset in ms, or "nan". Fresh producer +
# bridges per sub-run (so each run's arrival skew is independent and cancels in
# the trimmed-minus-untrimmed difference).
measure_offset() {  # $1=trim_ms $2=tag
    local trim="$1" tag="$2" pp pb0 pb1 mkv
    flash_marker_to_udps "$UDP0" "$UDP1"; pp=$SRT_LAST_PID
    srt_bridge "$UDP0" "$SRT0"; pb0=$SRT_LAST_PID
    srt_bridge "$UDP1" "$SRT1"; pb1=$SRT_LAST_PID
    sleep 1.5
    mkv=$("$HARNESS" --url "$(srt_caller_url "$SRT0")" --url "$(srt_caller_url "$SRT1")" \
            --outdir "$WORKDIR" --name "srttrim_${tag}" --seconds "$SECS" --fps 30 --trim "$trim" | tail -n1)
    kill "$pp" "$pb0" "$pb1" 2>/dev/null; wait "$pp" "$pb0" "$pb1" 2>/dev/null
    if [ -z "$mkv" ] || [ ! -s "$mkv" ]; then echo "nan"; return; fi
    flash_pts_series "$mkv" 0 > "$WORKDIR/${tag}_0.txt"
    flash_pts_series "$mkv" 1 > "$WORKDIR/${tag}_1.txt"
    paste "$WORKDIR/${tag}_0.txt" "$WORKDIR/${tag}_1.txt" | awk '
        NF==2 { d=($1-$2)*1000; s+=d; n++ }
        END { if(n>0) printf "%.1f", s/n; else printf "nan" }'
}

UNTRIMMED=$(measure_offset 0 base)
TRIMMED=$(measure_offset "$T" trim)
echo "[srt-trim] untrimmed_ms=$UNTRIMMED trimmed_ms=$TRIMMED (applied=$T; expect trimmed-untrimmed ≈ -$T)"

if [ "$UNTRIMMED" = "nan" ] || [ "$TRIMMED" = "nan" ]; then
    echo "FAIL: could not measure flash offset (no output / no flashes) — SRT connect or extraction failed"; exit 1
fi

SHIFT=$(awk -v u="$UNTRIMMED" -v t="$TRIMMED" 'BEGIN{printf "%.1f", t-u}')
PASS=$(awk -v sh="$SHIFT" -v tt="$T" -v tol="$TOL" 'BEGIN{ d=sh-(-tt); if(d<0)d=-d; printf (d<=tol)?"1":"0" }')
echo "[srt-trim] measured_shift_ms=$SHIFT expected=-$T tol=$TOL"
if [ "$PASS" != "1" ]; then
    echo "FAIL: trim shift ${SHIFT}ms not within ${TOL}ms of -${T}ms — per-source trim not applied as expected over SRT"; exit 1
fi
echo "PASS: per-source trim over SRT — view1 delayed by ~${T}ms (measured shift ${SHIFT}ms)"
exit 0
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/run_srt_trim.sh`

- [ ] **Step 3: Run it — expect PASS, and verify the shift tracks the trim (teeth)**

Run:
```bash
bash tests/e2e/run_srt_trim.sh build/srt/tests/e2e/sync_harness 23530; echo "exit=$?"
```
Expected: `PASS: ...` and `exit=0`, with `measured_shift_ms` near -300. **Verify the teeth:** confirm `untrimmed_ms` and `trimmed_ms` differ by ≈300 (the trim moved content); if they were equal the gate would have FAILED. As a control, optionally run `OLR_SRT_TRIM_MS=0` — the shift should be ≈0 (and that run passes its own -0 expectation), demonstrating no shift without trim. If the real shift lands outside ±120ms of -300 across a couple of runs, widen `OLR_SRT_TRIM_TOL_MS` default in the script (e.g. 150) and re-run.

- [ ] **Step 4: Register the CTest gate**

In `tests/e2e/CMakeLists.txt`, after the `e2e_srt_sync` block, add:

```cmake
# Phase 2b: per-source trim (#28) over real SRT — +T on view1 delays it by ~T.
# Base port 23530.
add_test(NAME e2e_srt_trim
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_trim.sh" "$<TARGET_FILE:sync_harness>" 23530)
set_tests_properties(e2e_srt_trim PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)
```

- [ ] **Step 5: Reconfigure and run the gate via CTest**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
( cd build/srt && ctest -R e2e_srt_trim --output-on-failure )
```
Expected: `e2e_srt_trim ... Passed`.

- [ ] **Step 6: Commit**

```bash
git add tests/e2e/run_srt_trim.sh tests/e2e/CMakeLists.txt
git commit -m "test(srt): e2e_srt_trim — per-source trim (#28) shift over real SRT"
```

---

## Task 4: `sync_harness` `--report-connections` flag

**Files:**
- Modify: `tests/e2e/sync_harness.cpp`

- [ ] **Step 1: Add the `<QSet>` include**

In `tests/e2e/sync_harness.cpp`, in the include block (after `#include <QtGlobal>`), add:

```cpp
#include <QSet>
```

- [ ] **Step 2: Parse the flag (presence-only)**

In `main`, after the existing `const int trimMs = argValue(...)` line, add:

```cpp
    const bool reportConnections = args.contains(QStringLiteral("--report-connections"));
```

- [ ] **Step 3: Track connected sources before recording starts**

In `tests/e2e/sync_harness.cpp`, immediately **before** the `rm.startRecording();` call, add:

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

- [ ] **Step 4: Print the count on stop (stderr, so stdout stays the MKV-path contract)**

In the inner `QTimer::singleShot(700, &app, [&]() { ... })` lambda, replace the existing body:

```cpp
            if (outPath.isEmpty()) {
                fprintf(stderr, "sync_harness: engine reported no output path\n");
                exitCode = 3;
            } else {
                printf("%s\n", qPrintable(outPath));
                fflush(stdout);
            }
            app.quit();
```

with:

```cpp
            if (outPath.isEmpty()) {
                fprintf(stderr, "sync_harness: engine reported no output path\n");
                exitCode = 3;
            } else {
                printf("%s\n", qPrintable(outPath));
                fflush(stdout);
            }
            if (reportConnections) {
                fprintf(stderr, "connected=%d\n", int(connectedSources.size()));
                fflush(stderr);
            }
            app.quit();
```

- [ ] **Step 5: Build**

Run: `"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness`
Expected: builds cleanly (no unused-variable / warning-as-error failures).

- [ ] **Step 6: Verify the flag over plain UDP (fast, transport-agnostic)**

The flag is independent of SRT, so verify it over a UDP source. Run:
```bash
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=320x240:rate=30" \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 \
  -f mpegts "udp://127.0.0.1:23599?pkt_size=1316" & FFPID=$!
sleep 1
build/srt/tests/e2e/sync_harness --url "udp://127.0.0.1:23599?fifo_size=1000000&overrun_nonfatal=1" \
  --report-connections --outdir /tmp --name connprobe --seconds 5 --fps 30 \
  >/tmp/conn.out 2>/tmp/conn.err
kill "$FFPID" 2>/dev/null
grep '^connected=' /tmp/conn.err; echo "---"; cat /tmp/conn.out
```
Expected: `connected=1` on stderr, and the MKV path on stdout (`/tmp/conn.out`). Confirms the signal fires and stdout stays the path-only contract.

- [ ] **Step 7: Verify the flag is off by default (no regression to other scenarios)**

Run:
```bash
build/srt/tests/e2e/sync_harness --url "udp://127.0.0.1:23599?fifo_size=1000000&overrun_nonfatal=1" \
  --outdir /tmp --name connprobe2 --seconds 2 --fps 30 2>/tmp/conn2.err >/dev/null
grep -c '^connected=' /tmp/conn2.err
```
Expected: `0` (no `connected=` line when the flag is absent).

- [ ] **Step 8: Commit**

```bash
git add tests/e2e/sync_harness.cpp
git commit -m "test(srt): sync_harness --report-connections — count connected sources"
```

---

## Task 5: Connection-status gate `run_srt_connect.sh` + `e2e_srt_connect`

**Files:**
- Create: `tests/e2e/run_srt_connect.sh`
- Modify: `tests/e2e/CMakeLists.txt` (after the `e2e_srt_trim` block)

- [ ] **Step 1: Write the gate script**

Create `tests/e2e/run_srt_connect.sh` with exactly:

```bash
#!/usr/bin/env bash
# Local SRT e2e (Phase 2b): per-source connection-status (#24) over real SRT.
#
# Run 1 (live):  4 SRT producers up -> sync_harness --report-connections must
#   report connected=4.
# Run 2 (teeth): only 3 bridges up; the 4th URL points at a DEAD SRT port (no
#   listener) -> the engine cannot connect it -> connected=3. Proves the count
#   reflects real connection state, not a constant.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX.
# Usage: run_srt_connect.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23540}"
SECS=8

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

# Spawn producers+bridges for views [0..live-1]; views >= live get a dead SRT port
# (no listener). Record all 4 URLs with --report-connections; echo the reported
# connected count (or -1 if the harness printed none).
run_connected() {  # $1=live_count(1..4) $2=tag
    local live="$1" tag="$2" i srt_port udp_port err
    local localpids=()
    URLS=()
    for i in 0 1 2 3; do
        srt_port=$((BASE + 2*i)); udp_port=$((BASE + 2*i + 1))
        URLS+=("$(srt_caller_url "$srt_port")")
        if [ "$i" -lt "$live" ]; then
            flash_marker_to_udps "$udp_port"; localpids+=("$SRT_LAST_PID")
            srt_bridge "$udp_port" "$srt_port"; localpids+=("$SRT_LAST_PID")
        fi
    done
    sleep 1.5
    err="$WORKDIR/${tag}.err"
    "$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
        --outdir "$WORKDIR" --name "srtconn_${tag}" --seconds "$SECS" --fps 30 \
        --report-connections >/dev/null 2>"$err"
    (( ${#localpids[@]} )) && kill "${localpids[@]}" 2>/dev/null
    wait "${localpids[@]}" 2>/dev/null
    awk -F= '/^connected=/{print $2; found=1} END{if(!found)print "-1"}' "$err"
}

echo "[srt-connect] base_port=$BASE"
LIVE4=$(run_connected 4 live)
echo "[srt-connect] live_run connected=$LIVE4 (expect 4)"
LIVE3=$(run_connected 3 dead)
echo "[srt-connect] dead_run connected=$LIVE3 (expect 3; 4th url has no listener)"

fail=0
[ "${LIVE4:-x}" = "4" ] || { echo "FAIL: live run reported connected=${LIVE4:-none}, expected 4 — not all SRT sources connected"; fail=1; }
[ "${LIVE3:-x}" = "3" ] || { echo "FAIL: teeth run reported connected=${LIVE3:-none}, expected 3 — connection-status does not discriminate a dead source"; fail=1; }
[ $fail -ne 0 ] && exit 1
echo "PASS: connection-status over SRT — 4 live => 4, 1 dead => 3"
exit 0
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/run_srt_connect.sh`

- [ ] **Step 3: Run it — expect PASS (covers both the live and teeth runs)**

Run:
```bash
bash tests/e2e/run_srt_connect.sh build/srt/tests/e2e/sync_harness 23540; echo "exit=$?"
```
Expected: `[srt-connect] live_run connected=4`, `[srt-connect] dead_run connected=3`, `PASS: ...`, `exit=0`.

**Dead-port risk to validate:** in the teeth run the 4th SRT caller has no listener. The engine opens it on a worker (capture) thread while the harness's fixed-duration `QTimer` fires `stopRecording()` on the main thread at 8s regardless, so the run should still finish and print `connected=3`. Confirm the run completes well within the 180s CTest timeout. If the engine instead blocks *indefinitely* on the unreachable SRT port (worker never returns, process won't exit), that's an engine-level connect-timeout gap — report it (DONE_WITH_CONCERNS) rather than working around it in the script.

- [ ] **Step 4: Register the CTest gate**

In `tests/e2e/CMakeLists.txt`, after the `e2e_srt_trim` block, add:

```cmake
# Phase 2b: per-source connection-status (#24) over real SRT — 4 live => 4,
# 1 dead port => 3. Base port 23540.
add_test(NAME e2e_srt_connect
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_connect.sh" "$<TARGET_FILE:sync_harness>" 23540)
set_tests_properties(e2e_srt_connect PROPERTIES LABELS "srt" TIMEOUT 180 RUN_SERIAL TRUE)
```

- [ ] **Step 5: Reconfigure and run the gate via CTest**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
( cd build/srt && ctest -R e2e_srt_connect --output-on-failure )
```
Expected: `e2e_srt_connect ... Passed`.

- [ ] **Step 6: Commit**

```bash
git add tests/e2e/run_srt_connect.sh tests/e2e/CMakeLists.txt
git commit -m "test(srt): e2e_srt_connect — connection-status (#24) over real SRT"
```

---

## Task 6: Document Phase 2b in `SRT_README.md`

**Files:**
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Replace the "Next (Phase 2)" section with a Phase 2b section**

In `tests/e2e/SRT_README.md`, replace the final `## Next (Phase 2)` section (from `## Next (Phase 2)` to the end of the file) with:

```markdown
## Phase 2b: feature validation over real SRT

Three gates validate shipped features over real `srt://` ingest (same SRT build,
`ctest -L srt`). They build on `srt_lib.sh` (bridge + tee'd full-frame-flash
producer + the `flash_pts_series` extractor reused from `run_sync_e2e.sh`):

- `e2e_srt_sync` — one flash source tee'd to **4** coincident SRT views; asserts
  every view carries the live flash (≥4 flashes — a disconnected view is blue-fill
  with 0 and FAILS) and the per-flash inter-view spread stays within a **generous**
  bound (250ms). The bound is generous by design: the engine anchors each source to
  first-packet **arrival** (no genlock, audit REF-2), so coincident SRT is
  phase-locked-within-bounds, not zero-skew. Teeth: `OLR_SRT_SYNC_DROP_VIEW=<i>`.
- `e2e_srt_trim` — flash source tee'd to **2** coincident SRT views; trims view1 by
  `T` (default 300ms) and asserts the measured (view0−view1) flash offset shifts by
  ≈ **−T** (the trim delays view1). Proves per-source trim (#28) works over SRT.
- `e2e_srt_connect` — records 4 SRT URLs with `sync_harness --report-connections`
  and asserts `connected=4`; a second run with the 4th URL pointed at a dead port
  asserts `connected=3`. Proves connection-status (#24) detects and discriminates.

The gate thresholds are validated against real local runs; if one proves flaky,
widen the bound (never delete the gate) — the content gates (per-view flash count,
connection count) carry the discrimination.

## Next (Phase 2c)

Disconnect/reconnect mid-recording, packet-loss / jitter injection, reconnect
re-anchoring, and long-run drift over SRT — each its own spec under
`docs/superpowers/specs/`.
```

- [ ] **Step 2: Commit**

```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): document Phase 2b feature-validation gates"
```

---

## Final: full-suite check + branch finish

- [ ] **Step 1: Run the whole local SRT label end-to-end**

Run:
```bash
cmake -S . -B build/srt -G Ninja >/dev/null
"$HOME/Qt/Tools/Ninja/ninja" -C build/srt sync_harness record_harness
( cd build/srt && ctest -L srt --output-on-failure )
```
Expected: all 5 `srt` gates pass — `e2e_srt_smoke`, `e2e_srt_4cam`, `e2e_srt_sync`, `e2e_srt_trim`, `e2e_srt_connect`.

- [ ] **Step 2: Confirm CI selection is unaffected (the `srt` label is excluded)**

Run:
```bash
( cd build/srt && ctest -N -LE 'sync-report|srt' | grep -c srt )
```
Expected: `0` (no `srt`-labelled test is in the CI selection).

- [ ] **Step 3: Final whole-branch code review + finish**

Dispatch the final code reviewer over the whole branch, then use `superpowers:finishing-a-development-branch` to open the PR. Per the standing constraint, open the PR **green but UNMERGED** (the user merges).

---

## Self-review notes (for the implementer)

- **Spec coverage:** srt_lib.sh (spec §"Shared plumbing") = Task 1; sync gate (§Scenario 1) = Task 2; trim gate (§Scenario 2) = Task 3; harness flag (§"Harness change") = Task 4; connect gate (§Scenario 3) = Task 5; CTest registration (§"CTest registration") = Tasks 2/3/5; docs (§"Docs") = Task 6. All spec sections covered.
- **The features already exist** (#24/#28 shipped); these are *validation* gates, so the TDD "watch it fail" is realized via teeth configs (drop-view, dead-port, no-op-trim) that must FAIL, then the correct config that must PASS.
- **Thresholds are tunable** — widen, never delete, a flaky gate.
- **bash 3.2** — no `${arr[-1]}`; use `$SRT_LAST_PID` captured right after each spawn.
