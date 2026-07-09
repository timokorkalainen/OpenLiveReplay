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
HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=tool_env.sh
. "$HERE/tool_env.sh"
olr_prepend_built_tool_paths

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit "$SKIP"; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit "$SKIP"; }
olr_python3_usable || { echo "SKIP: usable python3 not found"; exit "$SKIP"; }
olr_ffmpeg_has_muxer rawvideo || { echo "SKIP: ffmpeg rawvideo muxer not available"; exit "$SKIP"; }

GPU_RUNTIME_ENABLED=0
case "${OLR_GPU_PIPELINE:-}" in
    1|true|TRUE|on|ON) GPU_RUNTIME_ENABLED=1 ;;
esac
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    "$PLAY_BIN" --probe-gpu-backend >/dev/null || {
        rc=$?
        [ "$rc" = "$SKIP" ] && { echo "SKIP: GPU backend unavailable"; exit "$SKIP"; }
        echo "FAIL: GPU backend probe failed ($rc)"; exit 1
    }
    DECODE_CAPS="$("$PLAY_BIN" --probe-native-decode-caps 2>&1)"; rc=$?
    if [ "$rc" != "0" ]; then
        echo "FAIL: native decode caps probe failed ($rc)"
        printf '%s\n' "$DECODE_CAPS"
        exit 1
    fi
    H264_DECODE_AVAIL="$(printf '%s\n' "$DECODE_CAPS" | awk -F= '/^h264=/{print $2}')"
    [ "$H264_DECODE_AVAIL" = "1" ] || { echo "SKIP: native H.264 decode unavailable for GPU NDI pipe"; exit "$SKIP"; }
    CAPS="$("$RECORD_BIN" --probe-codec-caps 2>&1)"; rc=$?
    if [ "$rc" != "0" ]; then
        [ "$rc" = "$SKIP" ] && { echo "SKIP: recorder codec probe unavailable"; exit "$SKIP"; }
        echo "FAIL: recorder codec probe failed ($rc)"
        printf '%s\n' "$CAPS"
        exit 1
    fi
    H264_AVAIL="$(printf '%s\n' "$CAPS" | awk -F= '/^h264=/{print $2}')"
    [ "$H264_AVAIL" = "1" ] || { echo "SKIP: hardware H.264 recorder unavailable for GPU NDI pipe"; exit "$SKIP"; }
fi

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
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    REC_OUT="$(OLR_VIEWS=1 "$RECORD_BIN" --url "ndi:${ENC}" --name olr_ndi_pipe --outdir "$WORK" \
                --seconds "$REC_SECS" --width 256 --height 144 --fps 30 --codec h264)"; rc=$?
else
    REC_OUT="$(OLR_VIEWS=1 "$RECORD_BIN" --url "ndi:${ENC}" --name olr_ndi_pipe --outdir "$WORK" \
                --seconds "$REC_SECS" --width 256 --height 144 --fps 30)"; rc=$?
fi
MKV="$(printf '%s\n' "$REC_OUT" | tail -n1)"
if [ "$rc" != "0" ] || [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: NDI ingest/record produced no MKV (rc=$rc)"; cat "$WORK/sender.log"; exit 1
fi
echo "[ndi-pipe] recorded $MKV"
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    VCODEC="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$MKV" | head -n1)"
    [ "$VCODEC" = "h264" ] || { echo "FAIL: GPU NDI pipe recorded codec '$VCODEC', expected h264"; exit 1; }
fi

# 2b. Pin resolution: a scaled record would corrupt the fixed-cell marker (self-check).
# ffprobe -of csv=p=0 appends a trailing comma after each field (e.g. "256,144,"); strip it.
DIMS_RAW="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$MKV")"
DIMS="${DIMS_RAW%,}"
if [ "$DIMS" != "256,144" ]; then
    echo "FAIL: recorded video is '$DIMS', expected '256,144' (scaling would corrupt the marker)"; exit 1
fi

# 3. Stage A — NDI ingest + record integrity: decode the MKV luma and check marker continuity.
# Skip the first second (-ss 1 after -i = output-seek, frame-accurate) to bypass the ~8 NDI
# connection-establishment frames that arrive before the marker sender's first real frame.
# These init frames have luma ~130 (just above the 128 decode threshold), so all counter bits
# decode as 1 (index=16777215), and the subsequent jump to the real first index looks like a
# reorder. Skipping 1s clears this artifact while the ~11s of actual marker content easily
# passes the liveness floor.
MKVLINE="$(ffmpeg -loglevel error -i "$MKV" -map 0:v:0 -ss 1 -f rawvideo -pix_fmt gray - \
            | "$MKVPROBE_BIN" 256 144)"; mrc=$?
echo "$MKVLINE"
[ "$mrc" = "0" ] || { echo "FAIL: marker_yuv_probe/ffmpeg error ($mrc)"; exit 1; }
mfield() { sed -n "s/.*$1=\\([0-9-]*\\).*/\\1/p" <<<"$MKVLINE"; }
mFrames=$(mfield framesDecoded); mDrops=$(mfield drops)
mReorders=$(mfield reorders); mGap=$(mfield maxGapFrames)
mDupes=$(mfield dupes); mFirst=$(mfield firstIndex); mLast=$(mfield lastIndex)
# A_FLOOR: 90% of (REC_SECS-1)*30 since we skip the first second.
A_FLOOR=$(( (REC_SECS - 1) * 30 * 9 / 10 ))
afail=0
[ "${mFrames:-0}" -ge "$A_FLOOR" ]        || { echo "FAIL[A]: framesDecoded=$mFrames < $A_FLOOR"; afail=1; }
[ "${mReorders:-1}" = "0" ]               || { echo "FAIL[A]: reorders=$mReorders"; afail=1; }
[ "${mGap:-99}" -le 2 ]                    || { echo "FAIL[A]: maxGapFrames=$mGap > 2"; afail=1; }
# Catastrophic-loss gate: COVERAGE = distinct indices captured / source span. With reorders==0,
# distinct = framesDecoded - dupes. A clean rate-matched loopback covers ~75-90%; a real uniform
# ingest loss (e.g. 50%) covers ~50%. This catches uniform loss that maxGapFrames (=2 for every
# other-frame loss) and a raw-drops ratio cannot. Phase-artifact dupes/drops do NOT lower coverage.
A_SPAN=$(( ${mLast:-0} - ${mFirst:-0} + 1 ))
A_COV_MIN=65   # percent; clean runs observed >=~75%, 50% loss ~50% -> fails
[ "${A_SPAN}" -gt 0 ] || { echo "FAIL[A]: bad index span (first=$mFirst last=$mLast)"; afail=1; }
A_COV_PCT=$(( ( ${mFrames:-0} - ${mDupes:-0} ) * 100 / ( A_SPAN > 0 ? A_SPAN : 1 ) ))
[ $(( ( ${mFrames:-0} - ${mDupes:-0} ) * 100 )) -ge $(( A_SPAN * A_COV_MIN )) ] \
    || { echo "FAIL[A]: coverage ${A_COV_PCT}% < ${A_COV_MIN}% (catastrophic ingest loss)"; afail=1; }
echo "[ndi-pipe] Stage A coverage ${A_COV_PCT}% (distinct=$(( ${mFrames:-0} - ${mDupes:-0} )) span=${A_SPAN} floor=${A_COV_MIN}%)"
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
tcavsync=$(field tcAvMaxFrames)
vTcChecked=$(field vTcChecked)
vTcMatches=$(field vTcMatches)
vTcSynth=$(field vTcSynth)
aTcSeen=$(field aTcSeen)
aTcSynth=$(field aTcSynth)
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
        echo "FAIL[B]: GPU NDI pipe produced no CPU materialization (gpuReadToCpuCount=$gpuReadToCpuCount)"
        bfail=1
    fi
    if ! num "$gpuReadbacks" || [ "$gpuReadbacks" -le 0 ]; then
        echo "FAIL[B]: GPU NDI pipe produced no readback telemetry (gpuReadbacks=$gpuReadbacks)"
        bfail=1
    fi
    if ! num "$uniqueGpuReadbackSurfaces" || [ "$uniqueGpuReadbackSurfaces" -le 0 ]; then
        echo "FAIL[B]: GPU NDI pipe produced no unique readback surfaces (uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
        bfail=1
    fi
    if num "$gpuReadbacks" && num "$uniqueGpuReadbackSurfaces" && [ "$gpuReadbacks" -ne "$uniqueGpuReadbackSurfaces" ]; then
        echo "FAIL[B]: GPU NDI pipe read back a surface more than once (gpuReadbacks=$gpuReadbacks uniqueGpuReadbackSurfaces=$uniqueGpuReadbackSurfaces)"
        bfail=1
    fi
    if ! num "$redundantGpuReadbacks" || [ "$redundantGpuReadbacks" -ne 0 ]; then
        echo "FAIL[B]: GPU NDI pipe redundant readbacks=$redundantGpuReadbacks"
        bfail=1
    fi
    if ! num "$readbackDrops" || [ "$readbackDrops" -ne 0 ]; then
        echo "FAIL[B]: GPU NDI pipe readbackDrops=$readbackDrops"
        bfail=1
    fi
    if ! num "$readbackQueueDepth" || [ "$readbackQueueDepth" -gt 3 ]; then
        echo "FAIL[B]: GPU NDI pipe readbackQueueDepth=$readbackQueueDepth > 3"
        bfail=1
    fi
    if ! num "$vTcChecked" || [ "$vTcChecked" -le 0 ]; then
        echo "FAIL[B]: GPU NDI pipe checked no video content/timecode pairs (vTcChecked=$vTcChecked)"
        bfail=1
    fi
    if ! num "$vTcSynth" || [ "$vTcSynth" -ne 0 ]; then
        echo "FAIL[B]: GPU NDI pipe video used synthesized timecode (vTcSynth=$vTcSynth)"
        bfail=1
    fi
    if ! num "$aTcSeen" || [ "$aTcSeen" -le 0 ]; then
        echo "FAIL[B]: GPU NDI pipe observed no audio timecodes (aTcSeen=$aTcSeen)"
        bfail=1
    fi
    if num "$aTcSeen" && num "$frames"; then
        if [ "$aTcSeen" -lt "$B_FLOOR" ] || [ $(( aTcSeen * 100 )) -lt $(( frames * 80 )) ]; then
            echo "FAIL[B]: GPU NDI pipe audio timecodes are too sparse (aTcSeen=$aTcSeen frames=$frames floor=$B_FLOOR)"
            bfail=1
        fi
    else
        echo "FAIL[B]: GPU NDI pipe audio/video timecode counts are not numeric (aTcSeen=$aTcSeen frames=$frames)"
        bfail=1
    fi
    if ! num "$aTcSynth" || [ "$aTcSynth" -ne 0 ]; then
        echo "FAIL[B]: GPU NDI pipe audio used synthesized timecode (aTcSynth=$aTcSynth)"
        bfail=1
    fi
}
assert_no_gpu_readback_path() {
    for gpucnt in gpuReadToCpuCount gpuReadbacks uniqueGpuReadbackSurfaces \
                  redundantGpuReadbacks readbackQueueDepth readbackDrops fenceWaitStalls \
                  gpuOomDegrades gpuVramBytes; do
        eval "gpuval=\$$gpucnt"
        if ! num "$gpuval" || [ "$gpuval" -ne 0 ]; then
            echo "FAIL[B]: GPU telemetry counter $gpucnt=$gpuval, expected 0 on the CPU NDI pipe path"
            bfail=1
        fi
    done
}

B_FLOOR=$(( CAP_SECS * 30 / 2 ))
bfail=0
[ "${frames:-0}" -ge "$B_FLOOR" ]    || { echo "FAIL[B]: framesReceived=$frames < $B_FLOOR"; bfail=1; }
[ "${reorders:-1}" = "0" ]           || { echo "FAIL[B]: reorders=$reorders"; bfail=1; }
MAX_GAP_B=3
[ "$GPU_RUNTIME_ENABLED" -eq 0 ] || MAX_GAP_B=2
[ "${maxgap:-99}" -le "$MAX_GAP_B" ] || { echo "FAIL[B]: maxGapFrames=$maxgap > $MAX_GAP_B"; bfail=1; }
# CPU NDI pipe beep A/V sync is report-only: ndi_recv_probe pairs the k-th video flash with
# the k-th audio beep by ordinal, which is unreliable for arrival-anchored NDI-recorded MKVs.
# The GPU lane hard-gates programme timecode pairing instead; this survives the record/playback
# pipe even when the marker beep is not reliably recoverable by the receiver probe.
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    echo "[ndi-pipe] GPU readback tcAvMaxFrames=$tcavsync (gate <=2), avSyncMaxFrames=$avsync (report-only), vTcMatches=$vTcMatches/$vTcChecked (epoch-rebased report-only)"
    # The pipe is rate-matched, not genlocked (see top of file): the receiver pairs the
    # k-th video and k-th audio timecode by ordinal, so 1-2 frames of A/V-timecode
    # divergence is inherent jitter, not desync. Gate at <=2 (consistent with maxGap<=2
    # above and within lip-sync tolerance) so the check is deterministic on slower hosts
    # and loaded CI instead of flaking on the 1<->2 boundary.
    [ "${tcavsync:-99}" -ge 0 ] && [ "${tcavsync:-99}" -le 2 ] || {
        echo "FAIL[B]: GPU readback tcAvMaxFrames=$tcavsync > 2"; bfail=1;
    }
else
    echo "[ndi-pipe] (report-only) avSyncMaxFrames=$avsync"
    [ "${avsync:--1}" -ge 0 ] || {
        echo "FAIL[B]: avSyncMaxFrames=$avsync (no beeps; audio path dead)"; bfail=1;
    }
fi
[ "${reposition:-1}" = "0" ]         || { echo "FAIL[B]: worker reposition=$reposition"; bfail=1; }
[ "${audioPushes:-0}" -gt 0 ]        || { echo "FAIL[B]: audioPushes=$audioPushes (audio path dead)"; bfail=1; }
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    assert_gpu_readback_path
else
    assert_no_gpu_readback_path
fi
[ "$bfail" = "0" ] || { echo "STAGE B (record -> NDI out) FAILED"; cat "$WORK/play.log"; exit 1; }

echo "PASS: full NDI pipe reliable — Stage A (ingest+record) and Stage B (playback+output) both green"
exit 0
