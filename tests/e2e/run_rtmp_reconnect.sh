#!/usr/bin/env bash
# Native RTMP e2e: E-RTMP reconnect requests and short fixture disconnects
# restart the native backend instead of falling back to FFmpeg.
#
# Usage: run_rtmp_reconnect.sh <record_harness_exe> [rtmp_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23780}"
SECONDS_TO_RECORD=12
SOURCE_SECONDS=18
DISCONNECT_AFTER_TAGS=120
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

fail_case() {
    local reason="$1"
    echo "FAIL: $reason"
    echo "--- harness stdout ---"
    cat "$HARNESS_OUT" 2>/dev/null || true
    echo "--- harness stderr ---"
    rtmp_redact_log "$HARNESS_ERR" 2>/dev/null | tail -n 120 || true
    echo "--- RTMP server log ---"
    rtmp_redact_log "$SERVER_LOG" 2>/dev/null || true
    exit 1
}

FLV="$WORKDIR/source.flv"
SERVER_LOG="$WORKDIR/server.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

echo "[rtmp-reconnect-e2e] port=$PORT seconds=$SECONDS_TO_RECORD"
rtmp_generate_tone_flv "$FLV" 1000 "$SOURCE_SECONDS"

rtmp_fixture_server_cmd \
    --port "$PORT" \
    --flv "$FLV" \
    --max-clients 2 \
    --send-reconnect-request \
    --disconnect-after-tags "$DISCONNECT_AFTER_TAGS" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")

for _ in $(seq 1 50); do
    if grep -q '^READY ' "$SERVER_LOG"; then break; fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
    sleep 0.1
done
if ! grep -q '^READY ' "$SERVER_LOG"; then
    fail_case "RTMP fixture server did not become ready on port $PORT"
fi

URL="$(rtmp_url "$PORT")"
"$HARNESS" --url "$URL" --name olr_rtmp_reconnect --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30 \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"
RC=$?
OUT_MKV="$(tail -n 1 "$HARNESS_OUT" 2>/dev/null || true)"
echo "[rtmp-reconnect-e2e] harness rc=$RC out=${OUT_MKV:-<none>}"

if [ "$RC" -ne 0 ]; then
    fail_case "record_harness exited nonzero (rc=$RC)"
fi
if [ -z "${OUT_MKV:-}" ] || [ ! -s "$OUT_MKV" ]; then
    fail_case "RTMP ingest produced no output"
fi
if ! grep -q "Native RTMP connected" "$HARNESS_ERR"; then
    fail_case "harness did not use the native RTMP backend"
fi
NATIVE_CONNECTS="$(grep -c "Native RTMP connected" "$HARNESS_ERR" || true)"
if ! is_num "${NATIVE_CONNECTS:-}" || [ "${NATIVE_CONNECTS%.*}" -lt 2 ]; then
    fail_case "harness did not reconnect natively (native connect count='${NATIVE_CONNECTS:-none}')"
fi
if ! grep -Eq "Native RTMP (reconnect request|stalled)|Connect failed\\. Retrying" "$HARNESS_ERR"; then
    fail_case "harness log has no reconnect/stall/retry marker"
fi
if grep -Eq "Attempting FFmpeg fallback connection|retrying FFmpeg for this URL" "$HARNESS_ERR"; then
    fail_case "harness fell back to FFmpeg during reconnect scenario"
fi
if ! grep -q '^DISCONNECT index=1 ' "$SERVER_LOG"; then
    fail_case "fixture did not intentionally disconnect the first client"
fi
if ! grep -q '^CLIENT index=2 ' "$SERVER_LOG"; then
    fail_case "fixture did not accept a reconnecting second client"
fi

scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
echo "[rtmp-reconnect-e2e] video_packets=${V_PACKETS:-?}"
MIN_FRAMES=180
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    fail_case "too few/invalid video packets after reconnect (got '${V_PACKETS:-none}', need >= $MIN_FRAMES)"
fi

echo "PASS: native RTMP restarted after reconnect request/disconnect with ${V_PACKETS} video packets"
exit 0
