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
HERE="$(cd "$(dirname "$0")" && pwd)"
# Output bus under test: feed (default) | pgm | multiview. play_harness routes the worker's
# OutputBusEngine render of this bus to the NdiOutputSink. For a single-feed marker at the
# source's own size, pgm (selected feed) and the 1-cell multiview composite are identity
# copies, so the full-frame marker stays decodable and the same gate applies to every bus.
BUS="${OLR_NDI_OUTPUT_BUS:-feed}"
case "$BUS" in feed|pgm|multiview) ;; *) echo "FAIL: bad OLR_NDI_OUTPUT_BUS='$BUS'"; exit 1 ;; esac
SENDER="OLR NDI Playback ${BUS} $$"
echo "[ndi-playback] bus=${BUS} seconds=${SECONDS_RUN}"

# shellcheck source=tool_env.sh
. "$HERE/tool_env.sh"
olr_prepend_built_tool_paths

command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not found"; exit "$SKIP"; }
olr_ffmpeg_has_demuxer rawvideo || { echo "SKIP: ffmpeg rawvideo demuxer not available"; exit "$SKIP"; }
olr_ffmpeg_has_demuxer s16le || { echo "SKIP: ffmpeg s16le demuxer not available"; exit "$SKIP"; }

GPU_RUNTIME_ENABLED=0
case "${OLR_GPU_PIPELINE:-}" in
    1|true|TRUE|on|ON) GPU_RUNTIME_ENABLED=1 ;;
esac
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    "$PLAY" --probe-gpu-backend >/dev/null || {
        rc=$?
        [ "$rc" = "$SKIP" ] && { echo "SKIP: GPU backend unavailable"; exit "$SKIP"; }
        echo "FAIL: GPU backend probe failed ($rc)"; exit 1
    }
    olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder for GPU readback playback"; exit "$SKIP"; }
    DECODE_CAPS="$("$PLAY" --probe-native-decode-caps 2>&1)"; rc=$?
    if [ "$rc" != "0" ]; then
        echo "FAIL: native decode caps probe failed ($rc)"
        printf '%s\n' "$DECODE_CAPS"
        exit 1
    fi
    H264_DECODE_AVAIL="$(printf '%s\n' "$DECODE_CAPS" | awk -F= '/^h264=/{print $2}')"
    [ "$H264_DECODE_AVAIL" = "1" ] || { echo "SKIP: native H.264 decode unavailable for GPU readback playback"; exit "$SKIP"; }
fi

WORK="$(mktemp -d)"
PLAY_PID=""
cleanup() { [ -n "$PLAY_PID" ] && kill "$PLAY_PID" 2>/dev/null; wait "$PLAY_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

# 1. Generate the marker as raw planes (a couple seconds longer than the capture window).
"$SRC" "$WORK/m" "$((SECONDS_RUN + 4))" || { echo "FAIL: marker source"; exit 1; }

# 2. Mux to a worker-decodable MKV. The default CPU lane uses ffv1 so the counter cells survive
#    bit-exact. The GPU lane intentionally uses H.264 so PlaybackWorker's native decoder produces
#    GPU-backed frames and NDI traverses AsyncGpuReadbackSink before reaching the receiver probe.
#    The setpts+enc_time_base flags force PTS = floor(N*1000/30) so the output clock (which
#    also uses floor truncation) samples a unique source frame each tick with no dupe/drop.
VIDEO_ARGS=(-c:v ffv1)
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    VIDEO_ARGS=("${OLR_H264_VCODEC_ARGS[@]}")
fi
if ! ffmpeg -loglevel error -y \
         -f rawvideo -pix_fmt yuv420p -s 256x144 -r 30 -i "$WORK/m.yuv" \
         -f s16le -ar 48000 -ac 2 -i "$WORK/m.pcm" \
         -vf "settb=1/1000,setpts=trunc(N*1000/30)" \
         "${VIDEO_ARGS[@]}" -enc_time_base 1:1000 -c:a pcm_s16le "$WORK/marker.mkv"; then
    echo "FAIL: ffmpeg mux"; exit 1
fi

# 3. Play it with NDI output enabled on the selected bus (background); give it time to register.
OLR_NDI_OUTPUT_SENDER="$SENDER" OLR_NDI_OUTPUT_BUS="$BUS" \
    "$PLAY" "$WORK/marker.mkv" play1x 1 > "$WORK/play.log" 2>&1 &
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

# 5. Wait for the play_harness to finish so its COUNTERS line is in the log.
#    The play1x scenario runs for 12 s after a 1.5 s warmup (total ~13.5 s from launch);
#    the probe window ends at ~8 s from launch, so the player is still running here.
wait "$PLAY_PID" 2>/dev/null; PLAY_PID=""

line="$(grep '^NDIRECV ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no NDIRECV report"; cat "$WORK/play.log"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }
frames=$(field framesReceived); drops=$(field drops); dupes=$(field dupes)
reorders=$(field reorders); avsync=$(field avSyncMaxFrames); maxgap=$(field maxGapFrames)
vTcChecked=$(field vTcChecked)
vTcMatches=$(field vTcMatches)
vTcSynth=$(field vTcSynth)
aTcSeen=$(field aTcSeen)
aTcSynth=$(field aTcSynth)

# Worker playback health from the COUNTERS line.
counters="$(grep '^COUNTERS ' "$WORK/play.log" || true)"
cfield() { sed -n "s/.*$1=\\([0-9-]*\\).*/\\1/p" <<<"$counters"; }
reposition=$(cfield reposition); audioPushes=$(cfield audioPushes)
gpuReadToCpuCount=$(cfield gpuReadToCpuCount)
gpuReadbacks=$(cfield gpuReadbacks)
uniqueGpuReadbackSurfaces=$(cfield uniqueGpuReadbackSurfaces)
redundantGpuReadbacks=$(cfield redundantGpuReadbacks)
readbackQueueDepth=$(cfield readbackQueueDepth)
readbackDrops=$(cfield readbackDrops)
fenceWaitStalls=$(cfield fenceWaitStalls)
gpuOomDegrades=$(cfield gpuOomDegrades)
gpuVramBytes=$(cfield gpuVramBytes)

num() { case "${1:-}" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }
assert_gpu_readback_path() {
    if ! num "$gpuReadToCpuCount" || [ "$gpuReadToCpuCount" -le 0 ]; then
        echo "FAIL: GPU NDI playback produced no CPU materialization (gpuReadToCpuCount=$gpuReadToCpuCount)"
        fail=1
    fi
    if [ "$BUS" = "multiview" ]; then
        # The multiview bus may be composited through the CPU bridge in headless CI.
        # PGM/feed NDI remain strict GPU-readback gates; multiview is a continuity
        # oracle for the preview composite and accepts CPU materialization telemetry.
        if ! num "$gpuReadbacks" || ! num "$uniqueGpuReadbackSurfaces"; then
            echo "FAIL: GPU NDI multiview emitted non-numeric readback telemetry (gpuReadbacks=$gpuReadbacks uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
            fail=1
        elif [ "$gpuReadbacks" -gt 0 ]; then
            if [ "$uniqueGpuReadbackSurfaces" -le 0 ]; then
                echo "FAIL: GPU NDI multiview produced readbacks without unique surface telemetry (uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
                fail=1
            fi
            if [ "$gpuReadbacks" -ne "$uniqueGpuReadbackSurfaces" ]; then
                echo "FAIL: GPU NDI multiview read back a surface more than once (gpuReadbacks=$gpuReadbacks uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
                fail=1
            fi
        fi
    else
        if ! num "$gpuReadbacks" || [ "$gpuReadbacks" -le 0 ]; then
            echo "FAIL: GPU NDI playback produced no readback telemetry (gpuReadbacks=$gpuReadbacks)"
            fail=1
        fi
        if ! num "$uniqueGpuReadbackSurfaces" || [ "$uniqueGpuReadbackSurfaces" -le 0 ]; then
            echo "FAIL: GPU NDI playback produced no unique readback surfaces (uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
            fail=1
        fi
        if num "$gpuReadbacks" && num "$uniqueGpuReadbackSurfaces" && [ "$gpuReadbacks" -ne "$uniqueGpuReadbackSurfaces" ]; then
            echo "FAIL: GPU NDI playback read back a surface more than once (gpuReadbacks=$gpuReadbacks uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
            fail=1
        fi
    fi
    if ! num "$redundantGpuReadbacks" || [ "$redundantGpuReadbacks" -ne 0 ]; then
        echo "FAIL: GPU NDI playback redundant readbacks=$redundantGpuReadbacks"
        fail=1
    fi
    if ! num "$readbackDrops" || [ "$readbackDrops" -ne 0 ]; then
        echo "FAIL: GPU NDI playback readbackDrops=$readbackDrops"
        fail=1
    fi
    if ! num "$readbackQueueDepth" || [ "$readbackQueueDepth" -gt 3 ]; then
        echo "FAIL: GPU NDI playback readbackQueueDepth=$readbackQueueDepth > 3"
        fail=1
    fi
    if ! num "$vTcChecked" || [ "$vTcChecked" -le 0 ]; then
        echo "FAIL: GPU NDI playback checked no video content/timecode pairs (vTcChecked=$vTcChecked)"
        fail=1
    fi
    if ! num "$vTcSynth" || [ "$vTcSynth" -ne 0 ]; then
        echo "FAIL: GPU NDI playback video used synthesized timecode (vTcSynth=$vTcSynth)"
        fail=1
    fi
    echo "[ndi-playback] vTcMatches=$vTcMatches/$vTcChecked (report-only; playback timecode is capture-relative)"
    if ! num "$aTcSeen" || [ "$aTcSeen" -le 0 ]; then
        echo "FAIL: GPU NDI playback observed no audio timecodes (aTcSeen=$aTcSeen)"
        fail=1
    fi
    if ! num "$aTcSynth" || [ "$aTcSynth" -ne 0 ]; then
        echo "FAIL: GPU NDI playback audio used synthesized timecode (aTcSynth=$aTcSynth)"
        fail=1
    fi
}
assert_no_gpu_readback_path() {
    for gpucnt in gpuReadToCpuCount gpuReadbacks uniqueGpuReadbackSurfaces \
                  redundantGpuReadbacks readbackQueueDepth readbackDrops fenceWaitStalls \
                  gpuOomDegrades gpuVramBytes; do
        eval "gpuval=\$$gpucnt"
        if ! num "$gpuval" || [ "$gpuval" -ne 0 ]; then
            echo "FAIL: GPU telemetry counter $gpucnt=$gpuval, expected 0 on the CPU NDI playback path"
            fail=1
        fi
    done
}

fail=0
[ "${frames:-0}" -ge "$((SECONDS_RUN * 15))" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${drops:-1}" = "0" ]    || { echo "FAIL: drops=$drops"; fail=1; }
[ "${dupes:-1}" = "0" ]    || { echo "FAIL: dupes=$dupes"; fail=1; }
[ "${reorders:-1}" = "0" ] || { echo "FAIL: reorders=$reorders"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] || { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }
[ "${maxgap:-9}" -le 2 ]   || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }
[ "${reposition:-1}" = "0" ] || { echo "FAIL: worker reposition=$reposition"; fail=1; }
[ "${audioPushes:-0}" -gt 0 ] || { echo "WARN: audioPushes=$audioPushes (audio path idle)"; }
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    assert_gpu_readback_path
else
    assert_no_gpu_readback_path
fi

if [ "$fail" = "0" ]; then echo "PASS: NDI playback continuity/sync/cadence OK"; exit 0; fi
echo "NDI PLAYBACK VALIDATION FAILED"; cat "$WORK/play.log"; exit 1
