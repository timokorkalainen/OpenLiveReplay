#!/usr/bin/env bash
# macOS real-app visual reliability gate.
#
# Launches the actual OpenLiveReplay.app visibly, drives it over WebSocket,
# records a deterministic numbered-frame marker stream over local SRT, then
# verifies paused seek/jog preview pixels via provider captures, OS screenshots,
# and PGM NDI marker latency when an NDI receiver probe is available.
set -uo pipefail

APP="${1:?OpenLiveReplay executable required}"
MARKER_SRC="${2:?ndi_marker_mkv_source required}"
MARKER_PROBE="${3:?marker_yuv_probe required}"
NDI_RECV_PROBE="${4:-}"

SKIP=77
HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"
export SRT_SKIP_CODE="$SKIP"
export OLR_E2E_LATENCY_TRACE="${OLR_E2E_LATENCY_TRACE:-1}"

[ "$(uname -s)" = "Darwin" ] || { echo "SKIP: macOS app visual e2e requires Darwin"; exit "$SKIP"; }
[ -x "$APP" ] || { echo "FAIL: app executable not found at $APP"; exit 1; }
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit "$SKIP"; }
command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not found"; exit "$SKIP"; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit "$SKIP"; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found"; exit "$SKIP"; }
pgrep -x WindowServer >/dev/null || { echo "SKIP: no macOS WindowServer"; exit "$SKIP"; }

if ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])libx264[[:space:]]'; then
    VIDEO_ARGS=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p
        -g 1 -keyint_min 1 -sc_threshold 0 -b:v 4M)
elif ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])h264_videotoolbox[[:space:]]'; then
    VIDEO_ARGS=(-c:v h264_videotoolbox -allow_sw 1 -realtime 1 -pix_fmt yuv420p
        -g 1 -b:v 4M)
else
    echo "SKIP: ffmpeg has no usable H.264 encoder"
    exit "$SKIP"
fi

BASE_PORT="${OLR_APP_E2E_BASE_PORT:-31870}"
CONTROL_PORT="${OLR_APP_E2E_CONTROL_PORT:-$((BASE_PORT + RANDOM % 1000))}"
SRT_PORT="${OLR_APP_E2E_SRT_PORT:-$((CONTROL_PORT + 1))}"
SOURCE_COUNT=4
MARKER_OFFSET_FRAMES=3000
SRT_PORTS=()
UDP_PORTS=()
for ((index = 0; index < SOURCE_COUNT; index++)); do
    SRT_PORTS+=("$((SRT_PORT + index * 2))")
    UDP_PORTS+=("$((SRT_PORT + index * 2 + 1))")
done

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    if [ "${#PIDS[@]}" -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null
        wait "${PIDS[@]}" 2>/dev/null
    fi
    if [ "${OLR_APP_E2E_KEEP_WORKDIR:-0}" != "1" ]; then
        rm -rf "$WORKDIR"
    else
        echo "[app-e2e] kept workdir: $WORKDIR"
    fi
}
trap cleanup EXIT

echo "[app-e2e] workdir=$WORKDIR control_port=$CONTROL_PORT srt_ports=${SRT_PORTS[*]} udp_ports=${UDP_PORTS[*]}"

for ((index = 0; index < SOURCE_COUNT; index++)); do
    "$MARKER_SRC" "$WORKDIR/marker-$index" 45 "$((index * MARKER_OFFSET_FRAMES))" || {
        echo "FAIL: marker source failed for index $index"
        exit 1
    }

    if ! ffmpeg -hide_banner -loglevel error -y \
            -f rawvideo -pix_fmt yuv420p -s 256x144 -r 30 -i "$WORKDIR/marker-$index.yuv" \
            -f s16le -ar 48000 -ac 2 -i "$WORKDIR/marker-$index.pcm" \
            -filter_complex "[0:v]settb=1/1000,setpts=trunc(N*1000/30)[v]" \
            -map "[v]" -map 1:a \
            "${VIDEO_ARGS[@]}" -enc_time_base 1:1000 -c:a pcm_s16le \
            "$WORKDIR/marker-$index.mkv"; then
        echo "FAIL: marker MKV mux failed for index $index"
        exit 1
    fi
done

for ((index = 0; index < SOURCE_COUNT; index++)); do
    srt_bridge "${UDP_PORTS[$index]}" "${SRT_PORTS[$index]}"
done
sleep 0.5

FFPIDS=()
for ((index = 0; index < SOURCE_COUNT; index++)); do
    ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$WORKDIR/marker-$index.mkv" \
        -map 0:v:0 -map 0:a:0 -c:v copy -c:a aac -b:a 96k \
        -f mpegts "udp://127.0.0.1:${UDP_PORTS[$index]}?pkt_size=1316" &
    FFPIDS+=("$!")
    PIDS+=("$!")
done
sleep 0.8
for pid in "${FFPIDS[@]}"; do
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: marker stream exited early"
        exit 1
    fi
done

DRIVER_ARGS=(
    "$HERE/macos_app_driver.py"
    --app "$APP"
    --port "$CONTROL_PORT"
    --documents-root "$WORKDIR/Documents"
    --marker-probe "$MARKER_PROBE"
    --workdir "$WORKDIR"
    --latency-threshold-ms "${OLR_APP_E2E_LATENCY_THRESHOLD_MS:-25}"
)
for ((index = 0; index < SOURCE_COUNT; index++)); do
    DRIVER_ARGS+=(--srt-url "$(srt_caller_url "${SRT_PORTS[$index]}")")
done
if [ -n "$NDI_RECV_PROBE" ] && [ -x "$NDI_RECV_PROBE" ]; then
    DRIVER_ARGS+=(--ndi-recv-probe "$NDI_RECV_PROBE" --require-ndi-latency)
fi

python3 "${DRIVER_ARGS[@]}"
