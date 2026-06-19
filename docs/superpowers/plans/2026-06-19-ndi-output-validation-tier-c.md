# NDI Output Validation Tier (c) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate the whole broadcast pipe — marker NDI source → real native NDI ingest → record MKV → real PlaybackWorker playback → NDI output → receiver probe — asserting robust pipe invariants (ordering, no sustained loss, liveness, A-V sync, worker health).

**Architecture:** Reuse tier-(a) `ndi_output_sender` as the marker NDI source, the existing `record_harness --url ndi:<name>` ingest, the tier-(b) `play_harness` NDI-output hook, and the foundation `ndi_recv_probe`. New: a tiny `marker_yuv_probe` that checks marker continuity of the recorded MKV (Stage A), and a driver that runs the full pipe and asserts both stages. No production source changes.

**Tech Stack:** C++17, Qt 6 Core, the foundation `ndi_output_marker` + `ndi_recv_analysis`, ffmpeg/ffprobe CLI, the real recording + playback engines, CTest, bash.

## Global Constraints

- WORKTREE ONLY: all work in `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/ndi-tier-c`. Before any commit verify `git rev-parse --show-toplevel` ends with `.claude/worktrees/ndi-tier-c`. NEVER touch the main checkout or any other worktree. Format only changed files with `clang-format -i <files>`; `git add` only the task's files (never `-A`/`.`).
- No production source changes — only `tests/e2e/**`. Do NOT modify `playback/**`, `recorder_engine/**`, or any other production source. Do NOT modify the reused harnesses (`ndi_output_sender.cpp`, `record_harness.cpp`, `play_harness.cpp`, `ndi_recv_probe.cpp`, `ndi_output_marker.*`, `ndi_recv_analysis.*`).
- Build: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build with `~/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`. clang-format binary `/opt/homebrew/opt/llvm/bin/clang-format`.
- Opt-in CTest label is exactly `ndi-output` (already excluded from default/CI/pre-push). `SKIP_RETURN_CODE 77`, `RUN_SERIAL TRUE`.
- Marker config defaults (from `tests/e2e/ndi_output_marker.h`): 256×144 @ 30fps, 48000 Hz S16 stereo, flash every 15th frame. The recorder MUST record at 256×144 (matching the source) so the fixed-cell marker survives unscaled.
- The NDI runtime and ffmpeg/ffprobe ARE present on this machine, so the gate must actually RUN and PASS (not skip).

---

### Task 1: `marker_yuv_probe` — recorded-MKV marker continuity analyzer

**Files:**
- Create: `tests/e2e/marker_yuv_probe.cpp`
- Modify: `tests/e2e/CMakeLists.txt` (target only)

**Interfaces:**
- Consumes: `tests/e2e/ndi_output_marker.h` (`NdiOutputMarkerConfig`, `ndiMarkerDecodeIndex`), `tests/e2e/ndi_recv_analysis.h` (`NdiContinuity`, `ndiAnalyzeContinuity`).
- Produces: `marker_yuv_probe <width> <height>` reading raw single-plane luma frames from **stdin** (`width*height` bytes/frame), printing `MKVMARK framesDecoded=<n> drops=<n> dupes=<n> reorders=<n> maxGapFrames=<n> firstIndex=<n> lastIndex=<n>`.

- [ ] **Step 1: Write the analyzer**

Create `tests/e2e/marker_yuv_probe.cpp`:

```cpp
// Reads raw single-plane luma frames (width*height bytes each) from stdin, decodes the
// per-frame marker counter, and reports continuity of the recorded marker sequence. Tier (c)
// uses it to validate the NDI ingest -> record segment, fed by ffmpeg decoding the recorded
// MKV's luma:  ffmpeg -i marker.mkv -map 0:v:0 -f rawvideo -pix_fmt gray - | marker_yuv_probe 256 144
// Pure (no NDI). The marker geometry is tied to its config, so only 256x144 is accepted.
#include <QByteArray>
#include <QString>

#include <cstdio>
#include <vector>

#include "tests/e2e/ndi_output_marker.h"
#include "tests/e2e/ndi_recv_analysis.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: marker_yuv_probe <width> <height>  (raw gray frames on stdin)\n");
        return 2;
    }
    const int width = QString::fromUtf8(argv[1]).toInt();
    const int height = QString::fromUtf8(argv[2]).toInt();

    NdiOutputMarkerConfig mk; // marker cell geometry is tied to the config (256x144)
    if (width != mk.width || height != mk.height) {
        fprintf(stderr, "[marker_yuv_probe] marker requires %dx%d, got %dx%d\n", mk.width,
                mk.height, width, height);
        return 1;
    }

    const size_t frameBytes = size_t(width) * size_t(height);
    QByteArray buf(int(frameBytes), '\0');
    auto* ptr = reinterpret_cast<uchar*>(buf.data());

    std::vector<qint64> indices;
    qint64 maxGap = 0;
    qint64 prev = -1;
    while (true) {
        const size_t got = std::fread(ptr, 1, frameBytes, stdin);
        if (got < frameBytes) break; // EOF or short final read
        const qint64 idx = ndiMarkerDecodeIndex(mk, ptr, width);
        if (idx < 0) continue; // undecodable frame - skip
        if (prev >= 0 && idx > prev) maxGap = qMax(maxGap, idx - prev);
        prev = idx;
        indices.push_back(idx);
    }

    if (indices.empty()) {
        fprintf(stderr, "[marker_yuv_probe] no marker frames decoded\n");
        return 1;
    }
    const NdiContinuity cont = ndiAnalyzeContinuity(indices);
    printf("MKVMARK framesDecoded=%lld drops=%lld dupes=%lld reorders=%lld maxGapFrames=%lld "
           "firstIndex=%lld lastIndex=%lld\n",
           (long long) cont.framesReceived, (long long) cont.drops, (long long) cont.dupes,
           (long long) cont.reorders, (long long) maxGap, (long long) indices.front(),
           (long long) indices.back());
    fflush(stdout);
    return 0;
}
```

- [ ] **Step 2: Add the target to `tests/e2e/CMakeLists.txt`**

After the `ndi_marker_mkv_source` target block (added in tier b), add:

```cmake
# Tier (c): decodes a recorded MKV's luma (piped from ffmpeg) and reports marker continuity,
# validating the NDI ingest -> record segment of the full pipe.
qt_add_executable(marker_yuv_probe
    marker_yuv_probe.cpp
    ndi_output_marker.cpp
    ndi_recv_analysis.cpp)
target_include_directories(marker_yuv_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(marker_yuv_probe PRIVATE Qt6::Core olr_warnings olr_sanitize)
```

- [ ] **Step 3: Build + sanity-run (no NDI needed — reuse the tier-b marker producer)**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug marker_yuv_probe ndi_marker_mkv_source
T=$(mktemp -d)
./build/claude-debug/tests/e2e/ndi_marker_mkv_source "$T/m" 2   # 60 frames, indices 0..59 (YUV420P)
ffmpeg -loglevel error -f rawvideo -pix_fmt yuv420p -s 256x144 -i "$T/m.yuv" \
       -f rawvideo -pix_fmt gray - | ./build/claude-debug/tests/e2e/marker_yuv_probe 256 144
rm -rf "$T"
```
Expected line (a perfectly contiguous synthetic sequence): `MKVMARK framesDecoded=60 drops=0 dupes=0 reorders=0 maxGapFrames=1 firstIndex=0 lastIndex=59`. Also confirm the dimension guard: `echo | ./build/claude-debug/tests/e2e/marker_yuv_probe 320 240` prints the `requires 256x144` error and exits 1.

- [ ] **Step 4: Format + commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/marker_yuv_probe.cpp
git add tests/e2e/marker_yuv_probe.cpp tests/e2e/CMakeLists.txt
git commit -m "test: add marker_yuv_probe for tier (c) recorded-MKV continuity"
```

---

### Task 2: Full-pipe driver + opt-in gate (real loopback)

**Files:**
- Create: `tests/e2e/run_ndi_e2e_pipe.sh`
- Modify: `tests/e2e/CMakeLists.txt` (add_test + properties)

**Interfaces:**
- Consumes: `ndi_output_sender` (marker NDI source), `record_harness` (NDI ingest→MKV), `marker_yuv_probe` (Stage A), `play_harness` (NDI out), `ndi_recv_probe` (Stage B), `ffmpeg`/`ffprobe`/`python3`.
- Produces: CTest `e2e_ndi_pipe` under label `ndi-output`, `SKIP_RETURN_CODE 77`.

- [ ] **Step 1: Write the driver**

Create `tests/e2e/run_ndi_e2e_pipe.sh`:

```bash
#!/usr/bin/env bash
# Tier (c): the WHOLE pipe — marker NDI source -> native NDI ingest -> record MKV -> real
# PlaybackWorker playback with NDI output -> receiver probe. Asserts robust pipe invariants
# (ordering strict, no sustained loss, liveness, A-V sync, worker health); phase-artifact
# dupes/drops are reported, not gated to zero (the pipe is rate-matched, not genlocked).
# Opt-in (label "ndi-output"); SKIP (77) when the NDI runtime / ffmpeg / ffprobe / source is
# unavailable.
#
# Usage: run_ndi_e2e_pipe.sh <ndi_output_sender> <record_harness> <marker_yuv_probe> \
#                            <play_harness> <ndi_recv_probe>
set -uo pipefail
SKIP=77

SENDER_BIN="${1:?ndi_output_sender required}"
RECORD_BIN="${2:?record_harness required}"
MKVPROBE_BIN="${3:?marker_yuv_probe required}"
PLAY_BIN="${4:?play_harness required}"
RECVPROBE_BIN="${5:?ndi_recv_probe required}"

REC_SECS="${OLR_NDI_PIPE_RECORD_SECS:-12}"
CAP_SECS="${OLR_NDI_PIPE_CAPTURE_SECS:-6}"
SRC_NAME="OLR NDI Pipe SRC $$"
OUT_NAME="OLR NDI Pipe OUT $$"

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit "$SKIP"; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit "$SKIP"; }
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit "$SKIP"; }

WORK="$(mktemp -d)"
SENDER_PID=""; PLAY_PID=""
cleanup() {
    [ -n "$SENDER_PID" ] && kill "$SENDER_PID" 2>/dev/null
    [ -n "$PLAY_PID" ] && kill "$PLAY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# 1. Start the marker NDI source; it must live through discovery + the whole record window.
"$SENDER_BIN" "$SRC_NAME" "$((REC_SECS + 10))" >"$WORK/sender.log" 2>&1 &
SENDER_PID=$!
sleep "${OLR_NDI_DISCOVERY_SECS:-4}"
if ! kill -0 "$SENDER_PID" 2>/dev/null; then
    wait "$SENDER_PID"; rc=$?
    [ "$rc" = "$SKIP" ] && { echo "SKIP: marker source exited 77 (no NDI runtime)"; exit "$SKIP"; }
    echo "FAIL: marker source exited early ($rc)"; cat "$WORK/sender.log"; exit 1
fi

# 2. Record the NDI source to an MKV at the marker's native 256x144 (no scaling -> cells survive).
#    OLR_VIEWS=1 -> a single marker view track.
ENC="$(SRC_NAME="$SRC_NAME" python3 -c 'import os,urllib.parse;print(urllib.parse.quote(os.environ["SRC_NAME"],safe=""))')"
REC_OUT="$(OLR_VIEWS=1 "$RECORD_BIN" --url "ndi:${ENC}" --name olr_ndi_pipe --outdir "$WORK" \
            --seconds "$REC_SECS" --width 256 --height 144 --fps 30)"; rc=$?
MKV="$(printf '%s\n' "$REC_OUT" | tail -n1)"
if [ "$rc" != "0" ] || [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: NDI ingest/record produced no MKV (rc=$rc)"; cat "$WORK/sender.log"; exit 1
fi
echo "[ndi-pipe] recorded $MKV"

# 2b. Pin resolution: a scaled record would corrupt the fixed-cell marker (self-check).
DIMS="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$MKV")"
if [ "$DIMS" != "256,144" ]; then
    echo "FAIL: recorded video is '$DIMS', expected '256,144' (scaling would corrupt the marker)"; exit 1
fi

# 3. Stage A — NDI ingest + record integrity: decode the MKV luma and check marker continuity.
MKVLINE="$(ffmpeg -loglevel error -i "$MKV" -map 0:v:0 -f rawvideo -pix_fmt gray - \
            | "$MKVPROBE_BIN" 256 144)"; mrc=$?
echo "$MKVLINE"
[ "$mrc" = "0" ] || { echo "FAIL: marker_yuv_probe/ffmpeg error ($mrc)"; exit 1; }
mfield() { sed -n "s/.*$1=\\([0-9-]*\\).*/\\1/p" <<<"$MKVLINE"; }
mFrames=$(mfield framesDecoded); mDrops=$(mfield drops)
mReorders=$(mfield reorders); mGap=$(mfield maxGapFrames)
A_FLOOR=$(( REC_SECS * 30 * 9 / 10 ))
A_DROP_CEIL=$(( ${mFrames:-0} / 20 )); [ "$A_DROP_CEIL" -lt 3 ] && A_DROP_CEIL=3
afail=0
[ "${mFrames:-0}" -ge "$A_FLOOR" ]        || { echo "FAIL[A]: framesDecoded=$mFrames < $A_FLOOR"; afail=1; }
[ "${mReorders:-1}" = "0" ]               || { echo "FAIL[A]: reorders=$mReorders"; afail=1; }
[ "${mGap:-99}" -le 2 ]                    || { echo "FAIL[A]: maxGapFrames=$mGap > 2"; afail=1; }
[ "${mDrops:-99}" -le "$A_DROP_CEIL" ]     || { echo "FAIL[A]: drops=$mDrops > $A_DROP_CEIL (ingest loss)"; afail=1; }
[ "$afail" = "0" ] || { echo "STAGE A (NDI in -> record) FAILED"; exit 1; }
echo "[ndi-pipe] Stage A OK (ingest+record integrity)"

# 4. Source no longer needed; stop it so its NDI name can't be confused with the output.
kill "$SENDER_PID" 2>/dev/null; wait "$SENDER_PID" 2>/dev/null; SENDER_PID=""

# 5. Stage B — record -> playback -> NDI out: play the MKV with NDI output and capture it.
OLR_NDI_OUTPUT_SENDER="$OUT_NAME" "$PLAY_BIN" "$MKV" play1x 1 >"$WORK/play.log" 2>&1 &
PLAY_PID=$!
sleep 2
if ! kill -0 "$PLAY_PID" 2>/dev/null; then
    wait "$PLAY_PID"; rc=$?
    [ "$rc" = "$SKIP" ] && { echo "SKIP: player exited 77"; exit "$SKIP"; }
    echo "FAIL: player exited early ($rc)"; cat "$WORK/play.log"; exit 1
fi
OUT="$("$RECVPROBE_BIN" "$OUT_NAME" "$CAP_SECS")"; rc=$?
echo "$OUT"
[ "$rc" = "$SKIP" ] && { echo "SKIP: output probe found no source"; exit "$SKIP"; }
[ "$rc" = "0" ] || { echo "FAIL: output probe error ($rc)"; cat "$WORK/play.log"; exit 1; }
wait "$PLAY_PID" 2>/dev/null; PLAY_PID=""   # let the player finish so COUNTERS is flushed

line="$(grep '^NDIRECV ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no NDIRECV report"; cat "$WORK/play.log"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }
frames=$(field framesReceived); reorders=$(field reorders)
avsync=$(field avSyncMaxFrames); maxgap=$(field maxGapFrames)
counters="$(grep '^COUNTERS ' "$WORK/play.log" || true)"
cfield() { sed -n "s/.*$1=\\([0-9-]*\\).*/\\1/p" <<<"$counters"; }
reposition=$(cfield reposition); audioPushes=$(cfield audioPushes)

B_FLOOR=$(( CAP_SECS * 30 / 2 ))
bfail=0
[ "${frames:-0}" -ge "$B_FLOOR" ]    || { echo "FAIL[B]: framesReceived=$frames < $B_FLOOR"; bfail=1; }
[ "${reorders:-1}" = "0" ]           || { echo "FAIL[B]: reorders=$reorders"; bfail=1; }
[ "${maxgap:-99}" -le 3 ]            || { echo "FAIL[B]: maxGapFrames=$maxgap > 3"; bfail=1; }
{ [ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 2 ]; } || { echo "FAIL[B]: avSyncMaxFrames=$avsync"; bfail=1; }
[ "${reposition:-1}" = "0" ]         || { echo "FAIL[B]: worker reposition=$reposition"; bfail=1; }
[ "${audioPushes:-0}" -gt 0 ]        || { echo "FAIL[B]: audioPushes=$audioPushes (audio path dead)"; bfail=1; }
[ "$bfail" = "0" ] || { echo "STAGE B (record -> NDI out) FAILED"; cat "$WORK/play.log"; exit 1; }

echo "PASS: full NDI pipe reliable — Stage A (ingest+record) and Stage B (playback+output) both green"
exit 0
```

- [ ] **Step 2: Register the test**

`chmod +x tests/e2e/run_ndi_e2e_pipe.sh`

In `tests/e2e/CMakeLists.txt`, after the `e2e_ndi_playback` test block (added in tier b), add:

```cmake
add_test(NAME e2e_ndi_pipe
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_ndi_e2e_pipe.sh"
        "$<TARGET_FILE:ndi_output_sender>" "$<TARGET_FILE:record_harness>"
        "$<TARGET_FILE:marker_yuv_probe>" "$<TARGET_FILE:play_harness>"
        "$<TARGET_FILE:ndi_recv_probe>")
set_tests_properties(e2e_ndi_pipe PROPERTIES
    LABELS "ndi-output"
    TIMEOUT 300
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)
```

- [ ] **Step 3: Reconfigure, build, run the REAL full pipe, and iterate to green (tune thresholds)**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_output_sender record_harness marker_yuv_probe play_harness ndi_recv_probe
ctest --test-dir build/claude-debug -L ndi-output -R e2e_ndi_pipe --output-on-failure
```
Expected on this machine (NDI runtime present): `e2e_ndi_pipe` PASSES, printing an `MKVMARK …` line (Stage A) with `reorders=0`, a small `maxGapFrames`, a small `drops`, and `framesDecoded ≈ REC_SECS*30`; then an `NDIRECV …` line + `COUNTERS … reposition=0` (Stage B) with `reorders=0`, `maxGapFrames ≤ 3`, `avSyncMaxFrames ≤ 2` (Stage B dupes/drops may be high — that is the documented, expected phase artifact and is NOT gated).

**This is a real integration test — debug to green WITHOUT weakening the gated invariants (reorders, ordering, liveness floors, A-V sync, reposition):**
- Record the OBSERVED `MKVMARK` and `NDIRECV`/`COUNTERS` numbers. The thresholds in the script (`A_FLOOR=0.9`, `maxGapFrames≤2/≤3`, `A_DROP_CEIL=5%`, `avSync≤2`) are STARTING values — if a metric is close to its bound on a clean run, set that bound to pass with clear margin while still catching catastrophic loss, and write the chosen value + the observed number in the report. Never relax `reorders==0` or `reposition==0`.
- Stage A `drops` higher than ~5% on a clean loopback → the sender/recorder clocks are mismatched more than expected; raise `OLR_NDI_PIPE_RECORD_SECS` margin or note it — but a *clean* loopback should rate-match within a few percent; investigate before loosening `A_DROP_CEIL`.
- ffprobe reports non-`256,144` → the recorder scaled the source; this is a real finding (the record path didn't honor `--width/--height` for NDI). Report DONE_WITH_CONCERNS with the ffprobe output; do not mask it.
- Stage B probe SKIPs "no source" → the output sink didn't register; check `play.log`, increase the pre-probe sleep, confirm `OLR_NDI_OUTPUT_SENDER` reached the harness.
- Stage A `ffmpeg … -pix_fmt gray` decodes 0 frames → confirm `-map 0:v:0` selects the marker video track (OLR_VIEWS=1 should give a single video stream).
- Run the gate 3× to confirm it is not flaky.

- [ ] **Step 4: Confirm gating (excluded by default; selectable by label)**

```bash
ctest --test-dir build/claude-debug -N -L ndi-output | grep -c e2e_ndi_pipe   # expect 1
ctest --test-dir build/claude-debug -N -LE 'sync-report|srt|native-apple-ingest|ndi-output' | grep -c e2e_ndi_pipe   # expect 0
```

- [ ] **Step 5: Format + commit**

```bash
git add tests/e2e/run_ndi_e2e_pipe.sh tests/e2e/CMakeLists.txt
git commit -m "test: register opt-in tier (c) full NDI pipe gate (label: ndi-output)"
```

---

### Task 3: Full verification

**Files:** none.

- [ ] **Step 1: Build all + run the foundation units + all NDI output gates (tier a + b + c)**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug marker_yuv_probe ndi_output_sender record_harness play_harness ndi_recv_probe ndi_marker_mkv_source tst_ndioutputmarker tst_ndirecvanalysis
ctest --test-dir build/claude-debug -R 'tst_ndioutputmarker|tst_ndirecvanalysis' --output-on-failure   # foundation units still green
ctest --test-dir build/claude-debug -L ndi-output --output-on-failure                                  # tier a + b + c pass (runtime present) or skip
```
Expected: unit tests 2/2; the `ndi-output` gates (`e2e_ndi_output`, `e2e_ndi_playback`, `e2e_ndi_pipe`) pass (or skip), none fail.

- [ ] **Step 2: Format + clean-tree + main untouched**

```bash
/opt/homebrew/opt/llvm/bin/clang-format --output-replacements-xml tests/e2e/marker_yuv_probe.cpp | grep -q "<replacement " && echo "NEEDS-FORMAT" || echo "format OK"
git diff --check
git status --short
git -C /Users/timo.korkalainen/Development/timo/OpenLiveReplay status --porcelain | grep -E "playback/|recorder_engine/|tests/" || echo "main: no tracked tier-c changes"
```
Expected: `format OK`; `git diff --check` empty; worktree clean; main checkout has none of this branch's files.

---

## Self-Review Checklist

- **Spec coverage:** Stage-A analyzer (Task 1), full-pipe driver with both stages + opt-in gate + real loopback tuning (Task 2), verification (Task 3). The spec's two-stage metric design (Stage A gated drops-ceiling + ordering + liveness; Stage B ordering + bounded gap + liveness + A-V + worker health, dupes/drops reported) is implemented in the driver. Covered.
- **No production change:** only `tests/e2e/**`; reused harnesses untouched.
- **Placeholder scan:** complete code/commands in every step; the threshold-tuning instruction is concrete (record observed numbers, keep margins, never relax `reorders`/`reposition`).
- **Type consistency:** `NdiOutputMarkerConfig`/`ndiMarkerDecodeIndex` (Task 1) and `NdiContinuity{framesReceived,drops,dupes,reorders}`/`ndiAnalyzeContinuity` match the foundation headers; the driver's grepped fields match `marker_yuv_probe`'s `MKVMARK` line, `ndi_recv_probe`'s `NDIRECV` line, and `play_harness`'s `COUNTERS` line; `record_harness` args (`--url ndi:<enc> --name --outdir --seconds --width --height --fps`, `OLR_VIEWS`) match its actual parsing.
- **Honest scope:** the pipe is rate-matched not genlocked, so Stage B dupes/drops are reported not gated; ordering/liveness/sync/no-sustained-loss is the verified reliability contract; no production change; reuses the foundation + tiers a/b.
```
