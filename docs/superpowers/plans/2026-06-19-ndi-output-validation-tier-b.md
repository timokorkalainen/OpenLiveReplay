# NDI Output Validation Tier (b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate the real PlaybackWorker decode→cache→output-bus→NdiOutputSink path: play a marker MKV through the worker with NDI output, captured by the foundation's `ndi_recv_probe`.

**Architecture:** A tiny tool writes the foundation marker as raw YUV420P+S16; ffmpeg muxes it into a worker-decodable MKV; `play_harness` (with an env-gated call to the existing public `setExternalOutputTargets`) plays it with NDI output; `ndi_recv_probe` captures and the driver asserts continuity/A-V/cadence + the worker's `reposition==0`. No production source changes.

**Tech Stack:** C++17, Qt 6 Core, the foundation `ndi_output_marker`, the real `PlaybackWorker` (via `olr_test_playback`), ffmpeg CLI, CTest, bash.

## Global Constraints

- WORKTREE ONLY: all work in `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/ndi-tier-b`. Before any commit verify `git rev-parse --show-toplevel` ends with `.claude/worktrees/ndi-tier-b`. NEVER touch the main checkout. Format only changed files with `clang-format -i <files>` (never repo-wide / `--commit origin/main`). `git add` only the task's files.
- No production source changes — only `tests/e2e/**` and the test `play_harness.cpp`. Do NOT modify `playback/**`.
- Build: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build with `~/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`.
- Opt-in CTest label is exactly `ndi-output` (already excluded from default/pre-push/CI). `SKIP_RETURN_CODE 77`.
- Marker config defaults (from `tests/e2e/ndi_output_marker.h`): 256×144 @ 30fps, 48000 Hz, S16 stereo. The MKV must be 256×144 @ 30fps so `ndi_recv_probe`'s fixed-cell decode matches.
- `setExternalOutputTargets` is PUBLIC (`playback/playbackworker.h:68`) — call it; do not add a method.

---

### Task 1: Marker MKV source tool

**Files:**
- Create: `tests/e2e/ndi_marker_mkv_source.cpp`
- Modify: `tests/e2e/CMakeLists.txt` (target only)

**Interfaces:**
- Consumes: `tests/e2e/ndi_output_marker.h` (`NdiOutputMarkerConfig`, `ndiMarkerLumaPlane`, `ndiMarkerAudioS16`, `ndiMarkerSamplesPerFrame`).
- Produces: `ndi_marker_mkv_source` executable writing `<out>.yuv` (raw YUV420P, neutral chroma) and `<out>.pcm` (raw S16LE stereo) for N seconds.

- [ ] **Step 1: Write the tool**

Create `tests/e2e/ndi_marker_mkv_source.cpp`:

```cpp
// Writes the foundation NDI marker as raw planar frames for muxing into a test MKV:
//   <out>.yuv : YUV420P (256x144), luma = marker (counter + flash), chroma = neutral 128.
//   <out>.pcm : S16LE interleaved stereo @ 48 kHz (marker beep on flash frames).
// Pure (no NDI). usage: ndi_marker_mkv_source <out-prefix> <seconds>
#include <QByteArray>
#include <QFile>

#include <cstdio>

#include "tests/e2e/ndi_output_marker.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ndi_marker_mkv_source <out-prefix> <seconds>\n");
        return 2;
    }
    const QString prefix = QString::fromUtf8(argv[1]);
    const double seconds = QString::fromUtf8(argv[2]).toDouble();

    NdiOutputMarkerConfig mk; // defaults must match the probe/sender
    const qint64 frames = qint64(seconds * mk.fpsNum / mk.fpsDen);
    const int chromaW = mk.width / 2;
    const int chromaH = mk.height / 2;
    const QByteArray neutralU(chromaW * chromaH, char(128));

    QFile yuv(prefix + ".yuv");
    QFile pcm(prefix + ".pcm");
    if (!yuv.open(QIODevice::WriteOnly) || !pcm.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "[ndi_marker_mkv_source] cannot open output files\n");
        return 1;
    }
    for (qint64 i = 0; i < frames; ++i) {
        const QByteArray y = ndiMarkerLumaPlane(mk, i); // width*height
        yuv.write(y);
        yuv.write(neutralU); // U
        yuv.write(neutralU); // V
        pcm.write(ndiMarkerAudioS16(mk, i));
    }
    yuv.close();
    pcm.close();
    fprintf(stderr, "[ndi_marker_mkv_source] wrote %lld frames (%dx%d @ %d/%d)\n",
            (long long) frames, mk.width, mk.height, mk.fpsNum, mk.fpsDen);
    return 0;
}
```

- [ ] **Step 2: Add the target to `tests/e2e/CMakeLists.txt`**

After the `ndi_output_sender` target block, add:

```cmake
# Tier (b): writes the marker as raw YUV420P+S16 for muxing into a playback MKV.
qt_add_executable(ndi_marker_mkv_source
    ndi_marker_mkv_source.cpp
    ndi_output_marker.cpp)
target_include_directories(ndi_marker_mkv_source PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ndi_marker_mkv_source PRIVATE Qt6::Core olr_warnings olr_sanitize)
```

- [ ] **Step 3: Build + sanity-run**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_marker_mkv_source
T=$(mktemp -d); ./build/claude-debug/tests/e2e/ndi_marker_mkv_source "$T/m" 1
# Expect: m.yuv size == 30 frames * (256*144 + 2*128*72) = 30 * 55296 = 1658880 bytes
#         m.pcm size == 30 frames * (48000/30 samples) * 2ch * 2bytes = 30*1600*4 = 192000 bytes
ls -l "$T"/m.yuv "$T"/m.pcm
echo "expect yuv=1658880 pcm=192000"; rm -rf "$T"
```
Expected: links clean (no warnings); the two files are exactly 1658880 and 192000 bytes.

- [ ] **Step 4: Format + commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/ndi_marker_mkv_source.cpp
git add tests/e2e/ndi_marker_mkv_source.cpp tests/e2e/CMakeLists.txt
git commit -m "test: add marker MKV source tool for tier (b) playback"
```

---

### Task 2: play_harness NDI-output hook

**Files:**
- Modify: `tests/e2e/play_harness.cpp`

**Interfaces:**
- Consumes: existing `PlaybackWorker::setExternalOutputTargets(QList<OutputTargetAssignment>)`; `OutputTargetAssignment` / `OutputTargetKind` / `OutputBusId` from `playback/output/outputtargetassignment.h`.
- Produces: when env `OLR_NDI_OUTPUT_SENDER` is set, the harness enables a feed(0) NDI output before `worker.start()`.

- [ ] **Step 1: Add the include**

Near the other `#include "playback/..."` lines in `tests/e2e/play_harness.cpp`, add:

```cpp
#include "playback/output/outputtargetassignment.h"
```

- [ ] **Step 2: Add the env-gated hook between openFile and start**

In `tests/e2e/play_harness.cpp`, the lines currently read:

```cpp
    worker.openFile(file);
    worker.setActiveAudioView(0); // route audio for view 0
    worker.start();
```

Change to:

```cpp
    worker.openFile(file);
    worker.setActiveAudioView(0); // route audio for view 0
    // Tier (b): enable a real NDI output on the feed(0) bus when requested, so the worker's
    // decode->cache->output-bus->NdiOutputSink path is exercised end to end.
    const QByteArray ndiSender = qgetenv("OLR_NDI_OUTPUT_SENDER");
    if (!ndiSender.isEmpty()) {
        OutputTargetAssignment ndi;
        ndi.id = QStringLiteral("ndi-tier-b");
        ndi.sourceBus = OutputBusId::feed(0);
        ndi.kind = OutputTargetKind::Ndi;
        ndi.enabled = true;
        ndi.settings.insert(QStringLiteral("senderName"),
                            QString::fromUtf8(ndiSender));
        worker.setExternalOutputTargets({ndi});
    }
    worker.start();
```

- [ ] **Step 3: Build + confirm existing playback behaviour is unaffected (env unset)**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug play_harness
```
Expected: links with no warnings. (The hook is inert unless `OLR_NDI_OUTPUT_SENDER` is set, so the existing `e2e_play_*` tests are unaffected — do not run the slow SRT-based playback gate here.)

- [ ] **Step 4: Format + commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/play_harness.cpp
git add tests/e2e/play_harness.cpp
git commit -m "test: add env-gated NDI output hook to play_harness (tier b)"
```

---

### Task 3: Driver script + opt-in gate (real loopback)

**Files:**
- Create: `tests/e2e/run_ndi_playback_e2e.sh`
- Modify: `tests/e2e/CMakeLists.txt` (add_test + properties)

**Interfaces:**
- Consumes: `ndi_marker_mkv_source`, `play_harness`, `ndi_recv_probe`, `ffmpeg`.
- Produces: CTest `e2e_ndi_playback` under label `ndi-output`, `SKIP_RETURN_CODE 77`.

- [ ] **Step 1: Write the driver**

Create `tests/e2e/run_ndi_playback_e2e.sh`:

```bash
#!/usr/bin/env bash
# Tier (b): play a marker MKV through the real PlaybackWorker with NDI output enabled and
# verify the captured NDI output is continuous, A-V synced, and steady. Opt-in (label
# "ndi-output"); SKIP (77) when ffmpeg or the NDI runtime is unavailable.
#
# Usage: run_ndi_playback_e2e.sh <ndi_marker_mkv_source> <play_harness> <ndi_recv_probe>
set -uo pipefail
SKIP=77

SRC="${1:?ndi_marker_mkv_source required}"
PLAY="${2:?play_harness required}"
PROBE="${3:?ndi_recv_probe required}"
SECONDS_RUN="${OLR_NDI_PLAYBACK_SECONDS:-6}"
SENDER="OLR NDI Playback Probe $$"

command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not found"; exit "$SKIP"; }

WORK="$(mktemp -d)"
PLAY_PID=""
cleanup() { [ -n "$PLAY_PID" ] && kill "$PLAY_PID" 2>/dev/null; wait "$PLAY_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

# 1. Generate the marker as raw planes (a couple seconds longer than the capture window).
"$SRC" "$WORK/m" "$((SECONDS_RUN + 4))" || { echo "FAIL: marker source"; exit 1; }

# 2. Mux to a worker-decodable MKV. ffv1 is lossless so the counter cells survive bit-exact.
if ! ffmpeg -loglevel error -y \
        -f rawvideo -pix_fmt yuv420p -s 256x144 -r 30 -i "$WORK/m.yuv" \
        -f s16le -ar 48000 -ac 2 -i "$WORK/m.pcm" \
        -c:v ffv1 -c:a pcm_s16le -video_track_timescale 1000 "$WORK/marker.mkv"; then
    echo "FAIL: ffmpeg mux"; exit 1
fi

# 3. Play it with NDI output enabled (background); give the source time to register.
OLR_NDI_OUTPUT_SENDER="$SENDER" "$PLAY" "$WORK/marker.mkv" play1x 1 > "$WORK/play.log" 2>&1 &
PLAY_PID=$!
sleep 2
if ! kill -0 "$PLAY_PID" 2>/dev/null; then
    wait "$PLAY_PID"; rc=$?
    if [ "$rc" = "$SKIP" ]; then echo "SKIP: player exited 77 (no NDI runtime)"; exit "$SKIP"; fi
    echo "FAIL: player exited early ($rc)"; cat "$WORK/play.log"; exit 1
fi

# 4. Capture + measure.
OUT="$("$PROBE" "$SENDER" "$SECONDS_RUN")"; rc=$?
echo "$OUT"
if [ "$rc" = "$SKIP" ]; then echo "SKIP: NDI runtime/source not available (probe)"; exit "$SKIP"; fi
if [ "$rc" != "0" ]; then echo "FAIL: probe error ($rc)"; cat "$WORK/play.log"; exit 1; fi

line="$(grep '^NDIRECV ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no NDIRECV report"; cat "$WORK/play.log"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }
frames=$(field framesReceived); drops=$(field drops); dupes=$(field dupes)
reorders=$(field reorders); avsync=$(field avSyncMaxFrames); maxgap=$(field maxGapFrames)

# Worker playback health from the COUNTERS line.
counters="$(grep '^COUNTERS ' "$WORK/play.log" || true)"
cfield() { sed -n "s/.*$1=\\([0-9-]*\\).*/\\1/p" <<<"$counters"; }
reposition=$(cfield reposition); audioPushes=$(cfield audioPushes)

fail=0
[ "${frames:-0}" -ge "$((SECONDS_RUN * 15))" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${drops:-1}" = "0" ]    || { echo "FAIL: drops=$drops"; fail=1; }
[ "${dupes:-1}" = "0" ]    || { echo "FAIL: dupes=$dupes"; fail=1; }
[ "${reorders:-1}" = "0" ] || { echo "FAIL: reorders=$reorders"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] || { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }
[ "${maxgap:-9}" -le 2 ]   || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }
[ "${reposition:-1}" = "0" ] || { echo "FAIL: worker reposition=$reposition"; fail=1; }
[ "${audioPushes:-0}" -gt 0 ] || { echo "WARN: audioPushes=$audioPushes (audio path idle)"; }

if [ "$fail" = "0" ]; then echo "PASS: NDI playback continuity/sync/cadence OK"; exit 0; fi
echo "NDI PLAYBACK VALIDATION FAILED"; cat "$WORK/play.log"; exit 1
```

- [ ] **Step 2: Register the test**

`chmod +x tests/e2e/run_ndi_playback_e2e.sh`

In `tests/e2e/CMakeLists.txt`, after the `e2e_ndi_output` test block, add:

```cmake
add_test(NAME e2e_ndi_playback
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_ndi_playback_e2e.sh"
        "$<TARGET_FILE:ndi_marker_mkv_source>" "$<TARGET_FILE:play_harness>"
        "$<TARGET_FILE:ndi_recv_probe>")
set_tests_properties(e2e_ndi_playback PROPERTIES
    LABELS "ndi-output"
    TIMEOUT 180
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)
```

- [ ] **Step 3: Reconfigure, build, run the REAL loopback, and iterate to green**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_marker_mkv_source play_harness ndi_recv_probe
ctest --test-dir build/claude-debug -L ndi-output -R e2e_ndi_playback --output-on-failure
```
Expected on this machine (NDI runtime present): `e2e_ndi_playback` PASSES with a `NDIRECV ... drops=0 dupes=0 reorders=0 avSyncMaxFrames=0 maxGapFrames<=2` line and a `COUNTERS ... reposition=0` line.

**This is a real integration test — if it does not pass first try, debug (do NOT weaken the assertions):**
- If the probe reports large `drops`/`reorders` and `framesReceived` is reasonable: the feed(0) output is likely not 256×144 (scaled), so the fixed-cell decode misreads. Confirm the played frame size — print `v.xres/v.yres` in a scratch run — and if the worker scales the feed, that is a real finding to report (it would mean the output path doesn't preserve source resolution); report DONE_WITH_CONCERNS with the evidence rather than masking it.
- If `framesReceived` is ~0 or the probe SKIPs "no source": the player's NDI sink did not start or register in time — check `play.log` for the sink state, increase the `sleep` before the probe, or confirm `OLR_NDI_OUTPUT_SENDER` reached the harness.
- If the worker rejects the ffv1 MKV at `openFile` (check `play.log`): switch the ffmpeg `-c:v ffv1` to intra mpeg2: `-c:v mpeg2video -q:v 2 -intra -pix_fmt yuv420p`. Re-run; the high-contrast cells still survive.
- Run the gate 3× to confirm it is not flaky.

- [ ] **Step 4: Confirm gating (excluded by default; selectable by label)**

```bash
ctest --test-dir build/claude-debug -N -L ndi-output | grep -c e2e_ndi_playback   # expect 1
ctest --test-dir build/claude-debug -N -LE 'sync-report|srt|native-apple-ingest|ndi-output' | grep -c e2e_ndi_playback   # expect 0
```

- [ ] **Step 5: Format + commit**

```bash
git add tests/e2e/run_ndi_playback_e2e.sh tests/e2e/CMakeLists.txt
git commit -m "test: register opt-in tier (b) NDI playback gate (label: ndi-output)"
```

---

### Task 4: Full verification

**Files:** none.

- [ ] **Step 1: Build all + run the NDI output gates (tier a + tier b)**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_marker_mkv_source play_harness ndi_recv_probe ndi_output_sender tst_ndioutputmarker tst_ndirecvanalysis
ctest --test-dir build/claude-debug -R 'tst_ndioutputmarker|tst_ndirecvanalysis' --output-on-failure   # foundation units still green
ctest --test-dir build/claude-debug -L ndi-output --output-on-failure                                  # tier a + tier b pass (runtime present) or skip
```
Expected: unit tests 2/2; the `ndi-output` gates pass (or skip), none fail.

- [ ] **Step 2: Format + clean-tree + main untouched**

```bash
for f in tests/e2e/ndi_marker_mkv_source.cpp tests/e2e/play_harness.cpp; do
  /opt/homebrew/opt/llvm/bin/clang-format --output-replacements-xml "$f" | grep -q "<replacement " && echo "NEEDS-FORMAT: $f" || true
done
git diff --check
git status --short
git -C /Users/timo.korkalainen/Development/timo/OpenLiveReplay status --porcelain | grep -E "playback/|tests/" || echo "main: no tracked tier-b changes"
```
Expected: no `NEEDS-FORMAT`; `git diff --check` empty; worktree clean; main checkout has none of this branch's files.

---

## Self-Review Checklist

- **Spec coverage:** marker MKV producer (Task 1), env-gated NDI hook using the existing public `setExternalOutputTargets` (Task 2), driver + ffv1 mux + real loopback + worker-counter assertions + opt-in gate (Task 3), verification (Task 4). Covered.
- **No production change:** only `tests/e2e/**` + `play_harness.cpp`; `playback/**` untouched.
- **Placeholder scan:** complete code/commands in every step; the ffmpeg fallback and the resolution-risk debug path are concrete, not vague.
- **Type consistency:** `NdiOutputMarkerConfig`/`ndiMarkerLumaPlane`/`ndiMarkerAudioS16`/`ndiMarkerSamplesPerFrame` (Task 1) match the foundation header; `OutputTargetAssignment`/`OutputTargetKind::Ndi`/`OutputBusId::feed(0)`/`setExternalOutputTargets` (Task 2) match the verified production signatures; the driver's grepped fields match `ndi_recv_probe`'s `NDIRECV` line and `play_harness`'s `COUNTERS` line.
- **Honest scope:** tier (c) (NDI ingest) deferred; no production change; metrics reuse the foundation probe + the worker's existing counters.
```
