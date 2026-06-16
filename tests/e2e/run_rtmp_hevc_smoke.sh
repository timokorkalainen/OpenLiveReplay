#!/usr/bin/env bash
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
RTMP_PORT="${2:-23770}"
SECONDS_TO_RECORD=4
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

HEVC="$WORKDIR/source.h265"
SERVER_LOG="$WORKDIR/rtmp_server.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

generate_hevc() {
    local codec="$1"
    shift
    ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -t "$SECONDS_TO_RECORD" -an -pix_fmt yuv420p \
        -c:v "$codec" "$@" \
        -g 1 -bf 0 \
        -f hevc "$HEVC"
}

echo "[rtmp-hevc-e2e] port=$RTMP_PORT"
if ! generate_hevc hevc_videotoolbox -allow_sw 1 >/dev/null 2>&1 || [ ! -s "$HEVC" ]; then
    rm -f "$HEVC"
    if ! generate_hevc libx265 -preset ultrafast \
        -x265-params "log-level=error:keyint=1:min-keyint=1:scenecut=0" >/dev/null 2>&1 \
        || [ ! -s "$HEVC" ]; then
        echo "SKIP: ffmpeg cannot generate raw Annex B HEVC with hevc_videotoolbox or libx265"
        exit 77
    fi
fi

python3 "$HERE/rtmp_fixture_server.py" --port "$RTMP_PORT" \
    --enhanced-hevc --hevc-annexb-source "$HEVC" \
    >"$SERVER_LOG" 2>&1 &
PIDS+=("$!")

for _ in $(seq 1 50); do
    if grep -q '^READY ' "$SERVER_LOG"; then break; fi
    if ! kill -0 "${PIDS[0]}" 2>/dev/null; then break; fi
    sleep 0.1
done
if ! grep -q '^READY ' "$SERVER_LOG"; then
    echo "FAIL: enhanced HEVC RTMP fixture server did not become ready on port $RTMP_PORT"
    echo "--- RTMP server log ---"
    rtmp_redact_log "$SERVER_LOG"
    exit 1
fi

URL="$(rtmp_url "$RTMP_PORT")"
OLR_NATIVE_RTMP=1 "$HARNESS" --url "$URL" --name olr_rtmp_hevc_smoke --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30 \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"
RC=$?

OUT_MKV="$(tail -n 1 "$HARNESS_OUT" 2>/dev/null || true)"
echo "[rtmp-hevc-e2e] harness rc=$RC out=${OUT_MKV:-<none>}"

fail=0
if [ "$RC" -ne 0 ] || [ -z "${OUT_MKV:-}" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: HEVC RTMP ingest produced no output (rc=$RC)"
    fail=1
fi

if ! grep -q "hvc1" "$HARNESS_ERR"; then
    echo "FAIL: harness stderr did not identify hvc1"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- harness stdout ---"
    rtmp_redact_log "$HARNESS_OUT"
    echo "--- harness stderr ---"
    rtmp_redact_log "$HARNESS_ERR" | tail -n 120
    echo "--- RTMP server log ---"
    rtmp_redact_log "$SERVER_LOG"
    exit 1
fi

echo "PASS: native RTMP HEVC hvc1 ingest produced $OUT_MKV"
exit 0
