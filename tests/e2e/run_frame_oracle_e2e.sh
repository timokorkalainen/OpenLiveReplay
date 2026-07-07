#!/usr/bin/env bash
# Frame-perfect scrub/jog oracle:
#   1. Generate a deterministic marker video whose pixels encode frame number N.
#   2. Mux it to a multi-track MKV with exact PTS=floor(N*1000/30).
#   3. Drive play_harness frameoracle, which scrubs/jogs while paused and checks that
#      every output feed delivers the exact requested frame in both metadata and pixels.
#
# Usage: run_frame_oracle_e2e.sh <ndi_marker_mkv_source> <play_harness> [views]
set -uo pipefail

SKIP=77
SRC="${1:?ndi_marker_mkv_source required}"
PLAY="${2:?play_harness required}"
VIEWS="${3:-2}"
SECONDS_FIXTURE="${OLR_FRAME_ORACLE_SECONDS:-22}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=tool_env.sh
. "$HERE/tool_env.sh"
olr_prepend_built_tool_paths

case "$VIEWS" in
    ''|*[!0-9]*)
        echo "FAIL: frame oracle requires numeric view count (got '$VIEWS')"
        exit 1
        ;;
esac
[ "$VIEWS" -ge 1 ] || { echo "FAIL: frame oracle requires at least one view"; exit 1; }

command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not found"; exit "$SKIP"; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit "$SKIP"; }
olr_ffmpeg_has_demuxer rawvideo || { echo "SKIP: ffmpeg rawvideo demuxer not available"; exit "$SKIP"; }
olr_ffmpeg_has_demuxer s16le || { echo "SKIP: ffmpeg s16le demuxer not available"; exit "$SKIP"; }

GPU_RUNTIME_ENABLED=0
case "${OLR_GPU_PIPELINE:-}" in
    1|true|TRUE|on|ON) GPU_RUNTIME_ENABLED=1 ;;
esac

# Use an all-intra H.264 marker fixture so every oracle seek has a real random
# access target. The GPU lane consumes the same codec through native decode.
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])libx264[[:space:]]'; then
    VIDEO_ARGS=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p
        -g 1 -keyint_min 1 -sc_threshold 0 -b:v 4M)
elif ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])h264_mf[[:space:]]'; then
    VIDEO_ARGS=(-c:v h264_mf -pix_fmt yuv420p -g 1 -b:v 4M)
else
    echo "SKIP: ffmpeg has no usable H.264 encoder for frame oracle"
    exit "$SKIP"
fi
if [ "$GPU_RUNTIME_ENABLED" -eq 1 ]; then
    "$PLAY" --probe-gpu-backend >/dev/null 2>&1
    rc=$?
    if [ "$rc" = "$SKIP" ]; then echo "SKIP: GPU backend unavailable"; exit "$SKIP"; fi
    [ "$rc" = "0" ] || { echo "FAIL: GPU backend probe failed ($rc)"; exit 1; }

    DECODE_CAPS="$("$PLAY" --probe-native-decode-caps 2>&1)"
    rc=$?
    if [ "$rc" != "0" ]; then
        echo "FAIL: native decode caps probe failed ($rc)"
        printf '%s\n' "$DECODE_CAPS"
        exit 1
    fi
    H264_DECODE_AVAIL="$(printf '%s\n' "$DECODE_CAPS" | awk -F= '/^h264=/{print $2}')"
    [ "$H264_DECODE_AVAIL" = "1" ] || { echo "SKIP: native H.264 decode unavailable"; exit "$SKIP"; }
fi

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo "[frame-oracle] views=$VIEWS seconds=$SECONDS_FIXTURE gpu=$GPU_RUNTIME_ENABLED"
"$SRC" "$WORK/marker" "$SECONDS_FIXTURE" || { echo "FAIL: marker source"; exit 1; }

split_labels=""
map_args=()
i=0
while [ "$i" -lt "$VIEWS" ]; do
    split_labels="${split_labels}[v${i}]"
    map_args+=(-map "[v${i}]")
    i=$((i + 1))
done
map_args+=(-map 1:a)
filter="[0:v]settb=1/1000,setpts=trunc(N*1000/30),split=${VIEWS}${split_labels}"

if ! ffmpeg -loglevel error -y \
        -f rawvideo -pix_fmt yuv420p -s 256x144 -r 30 -i "$WORK/marker.yuv" \
        -f s16le -ar 48000 -ac 2 -i "$WORK/marker.pcm" \
        -filter_complex "$filter" \
        "${map_args[@]}" \
        "${VIDEO_ARGS[@]}" -enc_time_base 1:1000 -c:a pcm_s16le "$WORK/frame_oracle.mkv"; then
    echo "FAIL: ffmpeg mux"
    exit 1
fi

VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 \
    "$WORK/frame_oracle.mkv" | grep -c .)"
echo "[frame-oracle] fixture video tracks: ${VTRACKS:-?} (expected $VIEWS)"
if [ "${VTRACKS:-0}" != "$VIEWS" ]; then
    echo "FAIL: fixture has ${VTRACKS:-0} video tracks, expected $VIEWS"
    exit 1
fi

PLAY_OUT="$(OLR_FRAME_ORACLE_VISUAL="${OLR_FRAME_ORACLE_VISUAL:-1}" \
    "$PLAY" "$WORK/frame_oracle.mkv" frameoracle "$VIEWS" 2>&1)"
PLAY_RC=$?
printf '%s\n' "$PLAY_OUT"

if [ "$PLAY_RC" = "$SKIP" ]; then
    echo "SKIP: play_harness exited 77"
    exit "$SKIP"
fi
if [ "$PLAY_RC" != "0" ]; then
    echo "FAIL: frame oracle play_harness exited $PLAY_RC"
    exit 1
fi
if ! printf '%s\n' "$PLAY_OUT" | grep -q '^FRAME_ORACLE_PASS '; then
    echo "FAIL: frame oracle did not report FRAME_ORACLE_PASS"
    exit 1
fi

echo "PASS: frame-perfect scrub/jog oracle OK"
