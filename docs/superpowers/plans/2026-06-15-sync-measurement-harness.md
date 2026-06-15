# Sync-Measurement Scoreboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a headless, report-only diagnostic (`sync_harness` + `run_sync_e2e.sh`) that quantifies inter-camera offset, fractional-rate drift, and A/V lip-sync against the real recording engine.

**Architecture:** A C++ harness records N synthetic sources into an N-view MKV via the real `ReplayManager`; a bash driver generates the synchronized FFmpeg producers, runs the harness, measures marker timing with `signalstats`/`silencedetect`, and prints a scoreboard. Nothing gates CI — every scenario `exit 0`.

**Tech Stack:** Qt6 Core, the `olr_test_engine` static lib (real muxer/streamworker/replaymanager), FFmpeg CLI (lavfi producers + `signalstats`/`silencedetect` analysis), CMake/CTest, bash/awk.

**Spec:** `docs/superpowers/specs/2026-06-15-sync-measurement-harness-design.md`

**Note on a deliberate simplification from the spec:** §4.2's "3-flash start clap" is implemented as **index-pairing** of the per-track flash series (pair flash #k on view 0 with flash #k on view 1). For the controlled synthetic sources here this is equivalent and simpler; the clap is recorded as a future robustness option if a producer ever drops its first flash.

---

## File Structure

| File | Responsibility | Status |
|---|---|---|
| `tests/e2e/sync_harness.cpp` | record N URLs → N-view MKV (no measurement) | create |
| `tests/e2e/run_sync_e2e.sh` | produce + record + analyze + scoreboard, per scenario | create |
| `tests/e2e/CMakeLists.txt` | build `sync_harness`, register 4 `sync-report` tests | modify |
| `tests/e2e/SYNC_BASELINE.md` | committed snapshot of current-`main` numbers | create |

**Build/run prerequisites (this machine):** configure with tests on, FFmpeg from `/opt/homebrew`:
```
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
```
Build a single target: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug sync_harness`.

---

## Task 1: `sync_harness` — N-source → N-view recording

**Files:**
- Create: `tests/e2e/sync_harness.cpp`
- Modify: `tests/e2e/CMakeLists.txt` (add executable only; tests come in Task 6)

- [ ] **Step 1: Write `tests/e2e/sync_harness.cpp`**

This generalizes `record_harness.cpp` from one `--url` to a repeatable `--url`, mapping view *i* ← source *i* (1:1). It drives the same `ReplayManager` entry points and prints the `.mkv` path as the last stdout line.

```cpp
// Headless multi-source sync-measurement harness.
//
// Records N source URLs into an N-view MKV (view i <- source i, 1:1) using the
// real ReplayManager, for a fixed wall-clock duration, then prints the absolute
// path of the produced .mkv on stdout (last line). The driver (run_sync_e2e.sh)
// feeds it synchronized synthetic FFmpeg streams and measures marker timing in
// the output. No measurement or assertions live here.
//
// Usage:
//   sync_harness --url <U1> [--url <U2> ...] --outdir <dir> --seconds <n>
//                [--name <base>] [--width W] [--height H] [--fps F]
#include <QCoreApplication>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <cstdio>

#include "recorder_engine/replaymanager.h"

namespace {
QString argValue(const QStringList& args, const QString& flag, const QString& fallback) {
    const int i = args.indexOf(flag);
    if (i >= 0 && i + 1 < args.size()) return args.at(i + 1);
    return fallback;
}
// All values following each occurrence of --url, in order.
QStringList allUrls(const QStringList& args) {
    QStringList urls;
    for (int i = 0; i + 1 < args.size(); ++i)
        if (args.at(i) == QStringLiteral("--url")) urls << args.at(i + 1);
    return urls;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    const QStringList urls = allUrls(args);
    const QString name = argValue(args, QStringLiteral("--name"), QStringLiteral("olr_sync"));
    const int seconds = argValue(args, QStringLiteral("--seconds"), QStringLiteral("8")).toInt();
    const int width = argValue(args, QStringLiteral("--width"), QStringLiteral("320")).toInt();
    const int height = argValue(args, QStringLiteral("--height"), QStringLiteral("240")).toInt();
    const int fps = argValue(args, QStringLiteral("--fps"), QStringLiteral("30")).toInt();
    const QString outdir = argValue(args, QStringLiteral("--outdir"), QString());

    if (urls.isEmpty()) {
        fprintf(stderr, "sync_harness: at least one --url is required\n");
        return 2;
    }

    const int n = urls.size();

    // One view per source, mapped 1:1. View names: SRC0, SRC1, ...
    QStringList sourceNames, viewNames;
    QList<int> viewSlotMap;
    for (int i = 0; i < n; ++i) {
        sourceNames << QStringLiteral("SRC%1").arg(i);
        viewNames << QStringLiteral("SRC%1").arg(i);
        viewSlotMap << i;
    }

    ReplayManager rm;
    rm.setSourceUrls(urls);
    rm.setSourceNames(sourceNames);
    rm.setViewCount(n);
    rm.setViewNames(viewNames);
    if (!outdir.isEmpty()) rm.setOutputDirectory(outdir);
    rm.setBaseFileName(name);
    rm.setVideoWidth(width);
    rm.setVideoHeight(height);
    rm.setFps(fps);

    rm.startRecording();
    if (!rm.isRecording()) {
        fprintf(stderr, "sync_harness: startRecording() failed (engine not recording)\n");
        return 4;
    }
    rm.updateViewMapping(viewSlotMap);

    const QString outPath = rm.getVideoPath();

    int exitCode = 0;
    QTimer::singleShot(seconds * 1000, &app, [&]() {
        rm.stopRecording();
        QTimer::singleShot(700, &app, [&]() {
            if (outPath.isEmpty()) {
                fprintf(stderr, "sync_harness: engine reported no output path\n");
                exitCode = 3;
            } else {
                printf("%s\n", qPrintable(outPath));
                fflush(stdout);
            }
            app.quit();
        });
    });

    app.exec();
    return exitCode;
}
```

- [ ] **Step 2: Add the executable to `tests/e2e/CMakeLists.txt`**

Insert immediately after the `play_harness` target block (before the `set(_driver ...)` line):

```cmake
# Headless multi-source sync-measurement harness: records N synthetic sources
# into an N-view MKV for the report-only frame-sync scoreboard (run_sync_e2e.sh).
qt_add_executable(sync_harness sync_harness.cpp)
target_link_libraries(sync_harness PRIVATE
    Qt6::Core olr_test_engine olr_warnings olr_sanitize)
```

- [ ] **Step 3: Build it**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug sync_harness`
Expected: links cleanly, produces `build/claude-debug/tests/e2e/sync_harness`.

- [ ] **Step 4: Verify it records 2 views (manual smoke)**

Run this one-liner (two throwaway testsrc producers → 2-view MKV → ffprobe count):
```bash
H=build/claude-debug/tests/e2e/sync_harness; D=$(mktemp -d)
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=s=320x240:r=30" \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 \
  -f mpegts "udp://127.0.0.1:23490?pkt_size=1316" & P1=$!
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=s=320x240:r=30" \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 \
  -f mpegts "udp://127.0.0.1:23491?pkt_size=1316" & P2=$!
sleep 0.5
MKV=$("$H" --url "udp://127.0.0.1:23490?fifo_size=1000000&overrun_nonfatal=1" \
            --url "udp://127.0.0.1:23491?fifo_size=1000000&overrun_nonfatal=1" \
            --outdir "$D" --name smoke --seconds 4 --fps 30 | tail -n1)
kill $P1 $P2 2>/dev/null
echo "video tracks: $(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
rm -rf "$D"
```
Expected: `video tracks: 2`.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/sync_harness.cpp tests/e2e/CMakeLists.txt
git commit -m "test(sync): N-source -> N-view recording harness"
```

---

## Task 2: Driver skeleton + shared analysis helpers + `intercam_matched`

**Files:**
- Create: `tests/e2e/run_sync_e2e.sh`

- [ ] **Step 1: Write `tests/e2e/run_sync_e2e.sh` (skeleton + helpers + matched scenario)**

```bash
#!/usr/bin/env bash
# Report-only frame-sync measurement driver.
#
# Generates synchronized synthetic sources, records them with sync_harness, and
# measures marker timing in the output MKV. Prints a scoreboard and ALWAYS exits
# 0 — this is a diagnostic, not a gate. See
# docs/superpowers/specs/2026-06-15-sync-measurement-harness-design.md.
#
# Usage: run_sync_e2e.sh <sync_harness> <scenario> <base_port> [--write-baseline]
#   scenarios: intercam_matched | intercam_skew | drift_2997 | lipsync
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
SCENARIO="${2:?scenario required}"
BASE_PORT="${3:?base udp port required}"
WRITE_BASELINE="${4:-}"

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

is_num() { case "${1:-}" in '' | *[!0-9.eE+-]*) return 1 ;; *) return 0 ;; esac; }

# Full-frame luma flash + (optional) co-timed beep, MPEG-TS over UDP, real-time.
# $1=port $2=rate(e.g. 30 or 30000/1001) $3=with_audio(0/1)
produce() {
    local port="$1" rate="$2" with_audio="$3"
    local vsrc="color=c=black:s=320x240:r=${rate}"
    # geq luma: white (235) for the first ~60ms of every source-second, else 16.
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    if [ "$with_audio" = "1" ]; then
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "$vsrc" \
            -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
            -filter_complex "[0:v]${vflt}[v];[1:a]volume=volume='if(lt(mod(t,1),0.06),1,0)':eval=frame[a]" \
            -map "[v]" -map "[a]" \
            -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
            -c:a aac -b:a 128k \
            -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    else
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "$vsrc" -vf "$vflt" \
            -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
            -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    fi
    PIDS+=($!)
}

url() { echo "udp://127.0.0.1:${1}?fifo_size=1000000&overrun_nonfatal=1"; }

# Rising-edge flash-onset pts_time series for one video track.
# $1=mkv $2=video-track-index. Emits one pts_time per flash, ascending.
flash_pts_series() {
    ffmpeg -hide_banner -loglevel error -i "$1" -map "0:v:$2" \
        -vf "signalstats,metadata=print:file=-" -f null - 2>/dev/null \
    | awk -v THRESH="${FLASH_THRESH:-120}" '
        /pts_time:/ { for (i=1;i<=NF;i++) if ($i ~ /^pts_time:/) { split($i,a,":"); t=a[2]+0 } }
        /YAVG=/     { split($0,b,"="); y=b[2]+0; bright=(y>THRESH);
                      if (bright && !prev) printf "%.6f\n", t; prev=bright }'
}

# Beep-onset pts_time series for one audio track (silence->sound rising edges).
# $1=mkv $2=audio-track-index.
beep_pts_series() {
    ffmpeg -hide_banner -loglevel error -i "$1" -map "0:a:$2" \
        -af "silencedetect=noise=-30dB:duration=0.03" -f null - 2>&1 \
    | awk '/silence_end:/ { for (i=1;i<=NF;i++) if ($i=="silence_end:") printf "%.6f\n", $(i+1) }'
}

# Assert N video tracks exist; report-only (never fails the run).
expect_video_tracks() {
    local mkv="$1" want="$2"
    local got
    got=$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$mkv" | wc -l | tr -d ' ')
    if [ "${got:-0}" != "$want" ]; then
        echo "ERROR: expected $want video tracks, got ${got:-0}"
        return 1
    fi
    return 0
}

# Append a scoreboard line to SYNC_BASELINE.md when --write-baseline is given.
emit() {
    local line="$1"
    echo "$line"
    if [ "$WRITE_BASELINE" = "--write-baseline" ]; then
        local base_md
        base_md="$(cd "$(dirname "$0")" && pwd)/SYNC_BASELINE.md"
        printf '%s\n' "$line" >> "$base_md"
    fi
}

echo "=== sync scoreboard: ${SCENARIO} ==="

case "$SCENARIO" in
  intercam_matched)
    # One producer split to two ports via the tee muxer: byte-identical, same
    # PTS, simultaneous. Any measured offset is engine anchoring/mux, not source.
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -f tee "[f=mpegts]udp://127.0.0.1:${P0}?pkt_size=1316|[f=mpegts]udp://127.0.0.1:${P1}?pkt_size=1316" &
    PIDS+=($!)
    sleep 0.5
    MKV=$("$HARNESS" --url "$(url $P0)" --url "$(url $P1)" \
            --outdir "$WORKDIR" --name intercam_matched --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=intercam_matched ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 2 || { echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
    # Pair flashes by index; report mean/max |Δ| in ms.
    STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" | awk '
        NF==2 { d=($1-$2)*1000; ad=(d<0?-d:d); s+=ad; if(ad>mx)mx=ad; n++ }
        END { if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan" }')
    read -r NP MEAN MAX <<<"$STATS"
    emit "[sync] scenario=intercam_matched flashes_paired=${NP} intercam_offset_ms: mean=${MEAN} max=${MAX}"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  *)
    echo "ERROR: unknown scenario '$SCENARIO'"
    echo "PASS: report emitted (diagnostic)"
    exit 0
    ;;
esac
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/e2e/run_sync_e2e.sh`

- [ ] **Step 3: Run the matched scenario**

Run: `bash tests/e2e/run_sync_e2e.sh build/claude-debug/tests/e2e/sync_harness intercam_matched 23480`
Expected: a scoreboard line like
`[sync] scenario=intercam_matched flashes_paired=7 intercam_offset_ms: mean=<small> max=<small>` and `PASS: report emitted`. `flashes_paired` should be ≥ 5 for an 8 s run. If `flashes_paired=0`, the flash threshold needs tuning — verify with `ffmpeg -i <mkv> -map 0:v:0 -vf signalstats,metadata=print:file=- -f null - 2>/dev/null | grep YAVG | sort -u | tail` and set `FLASH_THRESH` between the flash and base YAVG.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/run_sync_e2e.sh
git commit -m "test(sync): driver skeleton + intercam_matched scenario"
```

---

## Task 3: `intercam_skew` scenario

**Files:**
- Modify: `tests/e2e/run_sync_e2e.sh` (add one `case` branch)

- [ ] **Step 1: Add the `intercam_skew` branch**

Insert this branch into the `case "$SCENARIO"` block, immediately after the `intercam_matched) ... ;;` branch:

```bash
  intercam_skew)
    # Two independent producers; source B started D ms after A models a
    # camera whose timeline is offset by D. The engine anchors each to its own
    # first-packet arrival, so it bakes the offset in (no shared reference).
    D_MS=${SKEW_MS:-250}
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    produce "$P0" 30 0
    sleep "$(awk -v d="$D_MS" 'BEGIN{printf "%.3f", d/1000}')"
    produce "$P1" 30 0
    sleep 0.5
    MKV=$("$HARNESS" --url "$(url $P0)" --url "$(url $P1)" \
            --outdir "$WORKDIR" --name intercam_skew --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=intercam_skew ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 2 || { echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
    # Index-pair flashes; signed mean Δ (view0 - view1) and stdev, in ms.
    STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" | awk '
        NF==2 { d=($1-$2)*1000; s+=d; ss+=d*d; n++ }
        END { if(n>0){ m=s/n; v=ss/n-m*m; if(v<0)v=0; printf "%d %.1f %.1f", n, m, sqrt(v) } else printf "0 nan nan" }')
    read -r NP MEAN STDEV <<<"$STATS"
    emit "[sync] scenario=intercam_skew flashes_paired=${NP} intercam_offset_ms: mean=${MEAN} stdev=${STDEV} (D_injected=${D_MS})"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;
```

- [ ] **Step 2: Run it**

Run: `bash tests/e2e/run_sync_e2e.sh build/claude-debug/tests/e2e/sync_harness intercam_skew 23482`
Expected: `[sync] scenario=intercam_skew flashes_paired=<n> intercam_offset_ms: mean=<≈±250 today> stdev=<small> (D_injected=250)`. The mean magnitude near 250 ms confirms the engine bakes in the offset (REF-2). The number is recorded, not asserted.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/run_sync_e2e.sh
git commit -m "test(sync): intercam_skew scenario (baked-in latency offset)"
```

---

## Task 4: `drift_2997` scenario

**Files:**
- Modify: `tests/e2e/run_sync_e2e.sh` (add one `case` branch)

- [ ] **Step 1: Add the `drift_2997` branch**

Insert after the `intercam_skew) ... ;;` branch:

```bash
  drift_2997)
    # One source paced at 29.97 (30000/1001) while the session runs int fps=30.
    # Flash every source-second; measure how the flash PTS slope deviates from
    # 1.0 (drift) over a long run. Quantifies FRAC-1.
    P0=$BASE_PORT
    DUR=${DRIFT_SECONDS:-60}
    produce "$P0" "30000/1001" 0
    sleep 0.5
    MKV=$("$HARNESS" --url "$(url $P0)" \
            --outdir "$WORKDIR" --name drift_2997 --seconds "$DUR" --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=drift_2997 ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    # Least-squares slope of (flash_pts vs flash_index); index k=0,1,2,...
    # Each flash is one source-second apart, so an ideal timeline has slope 1.0.
    STATS=$(awk '
        { x=NR-1; y=$1; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; n++ }
        END {
            if (n>=2) {
                slope=(n*sxy - sx*sy)/(n*sxx - sx*sx);
                ppm=(slope-1.0)*1e6;
                slip=(slope-1.0)*30*(y);          # frames of slip over the run
                printf "%d %.6f %.0f %.2f", n, slope, ppm, slip
            } else printf "0 nan nan nan"
        }' "$WORKDIR/v0.txt")
    read -r NF SLOPE PPM SLIP <<<"$STATS"
    emit "[sync] scenario=drift_2997 flashes=${NF} slope=${SLOPE} drift_ppm=${PPM} drift_frames_slip=${SLIP}"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;
```

- [ ] **Step 2: Run it**

Run: `DRIFT_SECONDS=30 bash tests/e2e/run_sync_e2e.sh build/claude-debug/tests/e2e/sync_harness drift_2997 23484`
(Use 30 s for a quick check; CTest uses the 60 s default.)
Expected: `[sync] scenario=drift_2997 flashes=<~30> slope=<≈1.001 or as measured> drift_ppm=<measured> drift_frames_slip=<measured>`. The exact numbers are the FRAC-1 baseline; they should trend toward 0 once rational-rate support lands.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/run_sync_e2e.sh
git commit -m "test(sync): drift_2997 scenario (fractional-rate timeline drift)"
```

---

## Task 5: `lipsync` scenario

**Files:**
- Modify: `tests/e2e/run_sync_e2e.sh` (add one `case` branch)

- [ ] **Step 1: Add the `lipsync` branch**

Insert after the `drift_2997) ... ;;` branch:

```bash
  lipsync)
    # One source with a co-timed flash + beep every second. Measure audio-onset
    # minus video-flash PTS (signed ms). EBU R37 band is +40/-60 ms (context).
    P0=$BASE_PORT
    produce "$P0" 30 1
    sleep 0.5
    MKV=$("$HARNESS" --url "$(url $P0)" \
            --outdir "$WORKDIR" --name lipsync --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=lipsync ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi

    flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
    beep_pts_series  "$MKV" 0 > "$WORKDIR/a.txt"
    # Pair the k-th flash with the k-th beep; signed mean/max (audio - video) ms.
    STATS=$(paste "$WORKDIR/v.txt" "$WORKDIR/a.txt" | awk '
        NF==2 { d=($2-$1)*1000; s+=d; ad=(d<0?-d:d); if(ad>mx)mx=ad; n++ }
        END { if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan" }')
    read -r NP MEAN MAX <<<"$STATS"
    emit "[sync] scenario=lipsync pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37_band=+40/-60)"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;
```

- [ ] **Step 2: Run it**

Run: `bash tests/e2e/run_sync_e2e.sh build/claude-debug/tests/e2e/sync_harness lipsync 23486`
Expected: `[sync] scenario=lipsync pairs=<n> av_offset_ms: mean=<measured> max=<measured> (EBU_R37_band=+40/-60)`. If `pairs=0`, check the beep detection: `ffmpeg -i <mkv> -map 0:a:0 -af silencedetect=noise=-30dB:duration=0.03 -f null - 2>&1 | grep silence_end` and adjust the noise floor if the synthetic beep sits below −30 dB.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/run_sync_e2e.sh
git commit -m "test(sync): lipsync scenario (A/V offset magnitude)"
```

---

## Task 6: CTest registration + committed baseline

**Files:**
- Modify: `tests/e2e/CMakeLists.txt`
- Create: `tests/e2e/SYNC_BASELINE.md`

- [ ] **Step 1: Register the four scenarios as `sync-report` tests**

Append to the end of `tests/e2e/CMakeLists.txt`:

```cmake
# --- Report-only frame-sync scoreboard (NON-GATING) -------------------------
# These drive sync_harness through synthetic synchronized sources and print
# measured inter-camera offset / drift / lip-sync numbers. They ALWAYS exit 0
# (diagnostic, not a gate) and carry the distinct "sync-report" label so the
# gating e2e selection and the sanitizer build-target list are unaffected.
# Run on demand with:  ctest -L sync-report
set(_sync_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_sync_e2e.sh")
add_test(NAME sync_intercam_matched
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" intercam_matched 23480)
add_test(NAME sync_intercam_skew
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" intercam_skew 23482)
add_test(NAME sync_drift_2997
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" drift_2997 23484)
add_test(NAME sync_lipsync
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" lipsync 23486)

set_tests_properties(
    sync_intercam_matched sync_intercam_skew sync_drift_2997 sync_lipsync
    PROPERTIES LABELS "sync-report" TIMEOUT 180 RUN_SERIAL TRUE)
```

- [ ] **Step 2: Reconfigure so CTest picks up the new tests**

Run: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos`
Then: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug sync_harness`

- [ ] **Step 3: Verify the gating suite is unaffected and the sync tests are separate**

Run: `cd build/claude-debug && ctest -N -L e2e | tail -3 && echo "--- sync ---" && ctest -N -L sync-report`
Expected: the `sync_*` tests appear ONLY under `-L sync-report`, and the existing `e2e_*` list under `-L e2e` is unchanged (still 9 e2e tests).

- [ ] **Step 4: Generate the committed baseline snapshot**

Create `tests/e2e/SYNC_BASELINE.md` with this header:

```markdown
# Sync Scoreboard — Baseline (pre-fix)

Numbers produced by `run_sync_e2e.sh` on `main` before any frame-sync fixes.
Regenerate locally with, e.g.:
`bash tests/e2e/run_sync_e2e.sh <sync_harness> intercam_skew 23482 --write-baseline`
Report-only; these are diagnostics, not pass/fail thresholds.

## 2026-06-15 (branch feat/sync-measurement-harness)
```

Then append real numbers by running each scenario with `--write-baseline`:
```bash
H=build/claude-debug/tests/e2e/sync_harness
bash tests/e2e/run_sync_e2e.sh "$H" intercam_matched 23480 --write-baseline
bash tests/e2e/run_sync_e2e.sh "$H" intercam_skew    23482 --write-baseline
bash tests/e2e/run_sync_e2e.sh "$H" drift_2997       23484 --write-baseline
bash tests/e2e/run_sync_e2e.sh "$H" lipsync          23486 --write-baseline
```
Expected: four `[sync] …` lines appended to `tests/e2e/SYNC_BASELINE.md`.

- [ ] **Step 5: Run the full sync-report suite via CTest once**

Run: `cd build/claude-debug && ctest -L sync-report --output-on-failure`
Expected: all 4 pass (they always exit 0); their stdout shows the scoreboard lines.

- [ ] **Step 6: Confirm the existing suite still passes (no regression)**

Run: `cd build/claude-debug && ctest -L e2e --output-on-failure`
Expected: 9/9 e2e tests pass (unchanged).

- [ ] **Step 7: Commit**

```bash
git add tests/e2e/CMakeLists.txt tests/e2e/SYNC_BASELINE.md
git commit -m "test(sync): register sync-report CTests + committed baseline snapshot"
```

---

## Task 7: clang-format / docs / PR

**Files:** none (process task)

- [ ] **Step 1: Pre-empt the CI lint gate on the C++ file**

Run (only `sync_harness.cpp` is C++; the `.sh`/`.md` are not format-checked):
```bash
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format CF=/opt/homebrew/opt/llvm/bin/clang-format
"$GCF" --binary "$CF" --commit "$(git merge-base origin/main HEAD)" --extensions cpp,h,hpp,mm,c --force
"$GCF" --binary "$CF" --commit "$(git merge-base origin/main HEAD)" --diff --extensions cpp,h,hpp,mm,c
```
Expected: second command prints `clang-format did not modify any files`. If the first reformatted anything, `git commit -am "clang-format: sync_harness"`.

- [ ] **Step 2: Push and open the PR**

```bash
SKIP_IOS_BUILD=1 git push -u origin feat/sync-measurement-harness
gh pr create --base main --title "Sync-measurement scoreboard (report-only frame-sync diagnostic)" --body "<summary of the 4 scenarios + that it is non-gating + links the spec>"
```

- [ ] **Step 3: Watch CI**

Run: `gh pr checks <n>` until Build+Test, Lint, and both sanitizers are green. The `sync-report` tests are NOT in the sanitizer build-target list or the gating ctest selection, so they don't run in CI by design — confirm the sanitizer job still builds (it doesn't reference `sync_harness`).

---

## Self-Review

**Spec coverage:**
- §3.1 sync_harness (N→N views) → Task 1. ✓
- §3.2 driver (produce/record/analyze/report, `--write-baseline`) → Tasks 2–5 + Task 6 Step 4. ✓
- §3.3 CTest `sync-report` label, kept off the sanitizer/gating lists → Task 6 Steps 1,3. ✓
- §4.1–4.4 four scenarios + methodology → Tasks 2,3,4,5. ✓
- §5 scoreboard keys (`intercam_offset_ms`, `drift_ppm`/`drift_frames_slip`, `av_offset_ms`) + baseline → emitted in each task, snapshot in Task 6. ✓
- §7 error handling (missing ffmpeg → SKIP; no MKV → ERROR exit 0; too few markers → nan, exit 0) → `produce`/`emit`/guards in Task 2, reused. ✓
- §8 multi-view guard (`expect_video_tracks`) → Task 2 helper, used in matched/skew. ✓

**Placeholder scan:** PR body in Task 7 Step 2 is intentionally a fill-at-time summary (not code); all code/script steps are complete. No TBD/TODO in scripts.

**Type/name consistency:** helper names (`produce`, `url`, `flash_pts_series`, `beep_pts_series`, `expect_video_tracks`, `emit`), metric keys, ports (matched 23480/81, skew 23482/83, drift 23484, lipsync 23486), and CTest names are consistent across Tasks 2–6. `FLASH_THRESH`/`SKEW_MS`/`DRIFT_SECONDS` env overrides are referenced where used.

**Known fragile points (flagged for the implementer):** flash YAVG threshold and beep noise floor are empirical — Tasks 2/5 include the exact command to inspect real values and the env var to tune. These are the only steps expected to need iteration against live ffmpeg output.
