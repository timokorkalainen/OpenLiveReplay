#!/usr/bin/env bash
# Local native RTMP e2e: prove the real engine records one rtmp:// stream
# without using the FFmpeg ingest backend.
#
# The fixture is self-contained:
#   1. ffmpeg generates a short H.264/AAC FLV file.
#   2. rtmp_fixture_server.py serves that FLV over RTMP play.
#   3. record_harness records rtmp://127.0.0.1:<port>/live/stream.
#   4. ffprobe asserts real recorded media, and the harness log must show the
#      native RTMP backend marker.
#
# Usage: run_rtmp_smoke.sh <record_harness_exe> [rtmp_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
RTMP_PORT="${2:-23701}"
SECONDS_TO_RECORD=7
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

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

FLV="$WORKDIR/source.flv"
SERVER_LOG="$WORKDIR/rtmp_server.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

echo "[${RTMP_SCHEME:-rtmp}-e2e] port=$RTMP_PORT"
if [ "${RTMP_REQUIRE_PLAY_QUERY:-0}" = "1" ] && [ -z "${RTMP_URL_QUERY:-}" ]; then
    echo "FAIL: RTMP_REQUIRE_PLAY_QUERY=1 requires RTMP_URL_QUERY to be set"
    exit 1
fi

rtmp_generate_tone_flv "$FLV" 1000 "$SECONDS_TO_RECORD"
rtmp_server "$RTMP_PORT" "$FLV" "$SERVER_LOG" || exit 1

URL="$(rtmp_url "$RTMP_PORT")"
OLR_NATIVE_RTMP=1 "$HARNESS" --url "$URL" --name olr_rtmp_smoke --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30 \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"
RC=$?

OUT_MKV="$(tail -n 1 "$HARNESS_OUT")"
echo "[${RTMP_SCHEME:-rtmp}-e2e] harness rc=$RC out=$OUT_MKV"

fail=0
if [ "$RC" -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: RTMP ingest produced no output (rc=$RC)"
    fail=1
fi

if ! grep -q "Native RTMP connected" "$HARNESS_ERR"; then
    echo "FAIL: harness did not use the native RTMP backend (missing 'Native RTMP connected' log)"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
    V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
    A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
    echo "[${RTMP_SCHEME:-rtmp}-e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?}"

    MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
    if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
        echo "FAIL: too few/invalid video frames over RTMP (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"
        fail=1
    fi
    if [ "${A_CHANNELS:-}" != "2" ]; then
        echo "FAIL: expected stereo audio over RTMP, got '${A_CHANNELS:-none}'"
        fail=1
    fi

    RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
           -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
           | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
    echo "[${RTMP_SCHEME:-rtmp}-e2e] audio_rms_db=${RMS:-?}"
    if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
        echo "FAIL: recorded audio is silence/unmeasurable (rms=${RMS:-none} dB)"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "--- harness stderr ---"
    tail -n 80 "$HARNESS_ERR"
    echo "--- RTMP server log ---"
    cat "$SERVER_LOG"
    exit 1
fi

echo "PASS: native ${RTMP_SCHEME:-rtmp} ingest — ${V_PACKETS} frames, stereo audio recorded from $URL"
exit 0
