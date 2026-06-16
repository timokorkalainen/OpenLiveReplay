#!/usr/bin/env bash
# Native RTMP/RTMPS interop gate against a real server, not the Python fixture.
#
# Required:
#   OLR_RTMP_INTEROP_PLAY_URL=rtmp://host/app/stream
#
# Optional:
#   OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://host/app/stream
#   OLR_RTMP_INTEROP_CODEC=avc|hevc
#   OLR_RTMP_INTEROP_SERVER_CMD='mediamtx /path/to/config.yml'
#   OLR_NATIVE_RTMP_ALLOW_INSECURE_TLS=1   # only for intentional test servers
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
SECONDS_TO_RECORD="${OLR_RTMP_INTEROP_SECONDS:-8}"
CODEC="${OLR_RTMP_INTEROP_CODEC:-avc}"
PLAY_URL="${OLR_RTMP_INTEROP_PLAY_URL:-}"
PUBLISH_URL="${OLR_RTMP_INTEROP_PUBLISH_URL:-$PLAY_URL}"

if [ -z "$PLAY_URL" ]; then
    echo "SKIP: set OLR_RTMP_INTEROP_PLAY_URL to run real-server RTMP interop"
    exit 77
fi

rtmp_require_tools

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

if [ -n "${OLR_RTMP_INTEROP_SERVER_CMD:-}" ]; then
    bash -c "$OLR_RTMP_INTEROP_SERVER_CMD" >"$WORKDIR/server.out" 2>"$WORKDIR/server.err" &
    PIDS+=("$!")
    sleep "${OLR_RTMP_INTEROP_SERVER_STARTUP_SECS:-2}"
fi

SOURCE="$WORKDIR/interop.flv"
PUBLISH_LOG="$WORKDIR/publish.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

publish_cmd=()
if [ "$CODEC" = "avc" ]; then
    rtmp_generate_tone_flv "$SOURCE" 1000 "$SECONDS_TO_RECORD"
    publish_cmd=(ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$SOURCE" -c copy -f flv "$PUBLISH_URL")
elif [ "$CODEC" = "hevc" ]; then
    SOURCE="$WORKDIR/interop.hevc"
    if ! ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -t "$SECONDS_TO_RECORD" -an -pix_fmt yuv420p \
        -c:v hevc_videotoolbox -allow_sw 1 -g 30 -bf 0 \
        -f hevc "$SOURCE" >/dev/null 2>&1 || [ ! -s "$SOURCE" ]; then
        rm -f "$SOURCE"
        if ! ffmpeg -hide_banner -loglevel error \
            -f lavfi -i "testsrc2=size=640x480:rate=30" \
            -t "$SECONDS_TO_RECORD" -an -pix_fmt yuv420p \
            -c:v libx265 -preset ultrafast \
            -x265-params "log-level=error:keyint=30:min-keyint=30:scenecut=0" \
            -f hevc "$SOURCE" >/dev/null 2>&1 || [ ! -s "$SOURCE" ]; then
            echo "SKIP: local ffmpeg cannot generate HEVC fixture"
            exit 77
        fi
    fi
    publish_cmd=(ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -f hevc -i "$SOURCE" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" -ac 2 \
        -c:v copy -c:a aac -b:a 128k -f flv "$PUBLISH_URL")
else
    echo "FAIL: unsupported OLR_RTMP_INTEROP_CODEC=$CODEC"
    exit 1
fi

"${publish_cmd[@]}" >"$PUBLISH_LOG" 2>&1 &
PUB_PID="$!"
PIDS+=("$PUB_PID")
sleep "${OLR_RTMP_INTEROP_PUBLISH_WARMUP_SECS:-2}"
if ! kill -0 "$PUB_PID" 2>/dev/null; then
    echo "FAIL: interop publisher exited before recording started"
    rtmp_redact_log "$PUBLISH_LOG"
    exit 1
fi

DISPLAY_PLAY_URL="$(rtmp_redact_value "$PLAY_URL")"
DISPLAY_PUBLISH_URL="$(rtmp_redact_value "$PUBLISH_URL")"
echo "[rtmp-interop] codec=$CODEC play=$DISPLAY_PLAY_URL publish=$DISPLAY_PUBLISH_URL seconds=$SECONDS_TO_RECORD"
OLR_NATIVE_RTMP=1 "$HARNESS" --url "$PLAY_URL" --name olr_rtmp_interop --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30 \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"
RC=$?
OUT_MKV="$(tail -n 1 "$HARNESS_OUT")"
echo "[rtmp-interop] harness rc=$RC out=$OUT_MKV"

fail=0
if [ "$RC" -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: native RTMP interop produced no output (rc=$RC)"
    fail=1
fi

if ! grep -q "Native RTMP connected" "$HARNESS_ERR"; then
    echo "FAIL: interop run did not use the native RTMP backend"
    fail=1
fi
if [ "$CODEC" = "hevc" ] && ! grep -q "hvc1" "$HARNESS_ERR"; then
    echo "FAIL: HEVC interop run did not identify hvc1"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
    V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
    A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
    RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
           -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
           | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
    echo "[rtmp-interop] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?} audio_rms_db=${RMS:-?}"

    MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
    case "${V_PACKETS:-}" in '' | *[!0-9.-]*) fail=1; echo "FAIL: invalid video packet count '${V_PACKETS:-none}'" ;; esac
    if [ "$fail" -eq 0 ] && [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
        echo "FAIL: too few video frames over real-server interop (got $V_PACKETS, need >= $MIN_FRAMES)"
        fail=1
    fi
    if [ "${A_CHANNELS:-}" != "2" ]; then
        echo "FAIL: expected stereo audio over real-server interop, got '${A_CHANNELS:-none}'"
        fail=1
    fi
    if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
        echo "FAIL: real-server interop audio is silence/unmeasurable (rms=${RMS:-none} dB)"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "--- publisher log ---"
    rtmp_redact_log "$PUBLISH_LOG"
    echo "--- harness stderr ---"
    rtmp_redact_log "$HARNESS_ERR" | tail -n 80
    [ -f "$WORKDIR/server.err" ] && {
        echo "--- server stderr ---"
        rtmp_redact_log "$WORKDIR/server.err" | tail -n 80
    }
    exit 1
fi

echo "PASS: native RTMP real-server interop -- ${V_PACKETS} frames, stereo audio recorded"
exit 0
