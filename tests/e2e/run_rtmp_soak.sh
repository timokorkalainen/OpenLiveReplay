#!/usr/bin/env bash
# Opt-in long-run native RTMP soak for AVC/AAC and Enhanced RTMP HEVC.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23790}"
CODEC="${OLR_RTMP_SOAK_CODEC:-avc}"
SECS="${OLR_RTMP_SOAK_SECONDS:-1800}"

if [ "${OLR_RTMP_RUN_SOAK:-0}" != "1" ]; then
    echo "SKIP: set OLR_RTMP_RUN_SOAK=1 to run native RTMP soak"
    exit 77
fi

rtmp_require_tools

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    if [ "${#PIDS[@]}" -gt 0 ]; then kill "${PIDS[@]}" 2>/dev/null; fi
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

is_num() { case "${1:-}" in '' | *[!0-9.-]*) return 1 ;; *) return 0 ;; esac; }

SOURCE="$WORKDIR/source.flv"
SERVER_LOG="$WORKDIR/server.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

if [ "$CODEC" = "avc" ]; then
    rtmp_generate_tone_flv "$SOURCE" 1000 "$SECS"
    rtmp_server "$PORT" "$SOURCE" "$SERVER_LOG" || exit 1
elif [ "$CODEC" = "hevc" ]; then
    SOURCE="$WORKDIR/hevc_source.hevc"
    HEVC_SOURCE_SECS="$SECS"
    if [ "$HEVC_SOURCE_SECS" -gt 60 ]; then
        HEVC_SOURCE_SECS=60
    fi
    if ! ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -t "$HEVC_SOURCE_SECS" -an -pix_fmt yuv420p \
        -c:v hevc_videotoolbox -allow_sw 1 -g 30 -bf 0 \
        -f hevc "$SOURCE" >/dev/null 2>&1 || [ ! -s "$SOURCE" ]; then
        rm -f "$SOURCE"
        if ! ffmpeg -hide_banner -loglevel error \
            -f lavfi -i "testsrc2=size=640x480:rate=30" \
            -t "$HEVC_SOURCE_SECS" -an -pix_fmt yuv420p \
            -c:v libx265 -preset ultrafast \
            -x265-params "log-level=error:keyint=30:min-keyint=30:scenecut=0" \
            -f hevc "$SOURCE" >/dev/null 2>&1 || [ ! -s "$SOURCE" ]; then
            echo "SKIP: local ffmpeg cannot generate HEVC fixture"
            exit 77
        fi
    fi
    rtmp_fixture_server_cmd --port "$PORT" --hevc-annexb-source "$SOURCE" --enhanced-hevc \
        --loop-duration "$SECS" \
        >"$SERVER_LOG" 2>&1 &
    PIDS+=("$!")
    for _ in $(seq 1 50); do
        if grep -q '^READY ' "$SERVER_LOG"; then break; fi
        if ! kill -0 "${PIDS[0]}" 2>/dev/null; then break; fi
        sleep 0.1
    done
    if ! grep -q '^READY ' "$SERVER_LOG"; then
        echo "FAIL: RTMP HEVC soak fixture server did not become ready on port $PORT"
        rtmp_redact_log "$SERVER_LOG"
        exit 1
    fi
else
    echo "FAIL: unsupported OLR_RTMP_SOAK_CODEC=$CODEC"
    exit 1
fi

URL="$(rtmp_url "$PORT")"
"$HARNESS" --url "$URL" \
    --name "rtmp_soak_${CODEC}" --outdir "$WORKDIR" --seconds "$SECS" \
    --width 640 --height 480 --fps 30 >"$HARNESS_OUT" 2>"$HARNESS_ERR"
RC=$?
OUT_MKV="$(tail -n 1 "$HARNESS_OUT" 2>/dev/null || true)"

if [ "$RC" -ne 0 ] || [ -z "${OUT_MKV:-}" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no native RTMP soak output (rc=$RC)"
    rtmp_redact_log "$HARNESS_ERR" | tail -n 120
    exit 1
fi
if ! grep -q "Native RTMP connected" "$HARNESS_ERR"; then
    echo "FAIL: soak did not use the native RTMP backend"
    rtmp_redact_log "$HARNESS_ERR" | tail -n 120
    exit 1
fi
if [ "$CODEC" = "hevc" ] && ! grep -q "hvc1" "$HARNESS_ERR"; then
    echo "FAIL: HEVC soak did not identify hvc1"
    rtmp_redact_log "$HARNESS_ERR" | tail -n 120
    exit 1
fi

PACKETS="$(ffprobe -v error -select_streams v:0 -count_packets \
    -show_entries stream=nb_read_packets -of default=noprint_wrappers=1:nokey=1 \
    "$OUT_MKV" | head -n1)"
MIN=$((30 * SECS * 8 / 10))
if ! is_num "${PACKETS:-}" || [ "${PACKETS%.*}" -lt "$MIN" ]; then
    echo "FAIL: too few soak packets ${PACKETS:-none} < $MIN"
    exit 1
fi

if [ "$CODEC" = "avc" ]; then
    scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
    A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
    if [ "${A_CHANNELS:-}" != "2" ]; then
        echo "FAIL: expected stereo audio in AVC soak, got '${A_CHANNELS:-none}'"
        exit 1
    fi
    RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
           -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
           | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
    if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
        echo "FAIL: AVC soak audio is silence/unmeasurable (rms=${RMS:-none} dB)"
        exit 1
    fi
fi

echo "PASS: native RTMP ${CODEC} soak packets=$PACKETS seconds=$SECS"
exit 0
