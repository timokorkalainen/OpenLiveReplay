#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23760}"

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

FLV="$WORKDIR/unsupported.flv"
SERVER_LOG="$WORKDIR/server.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

if ! ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -t 5 -c:v flv -an -f flv "$FLV"; then
    echo "SKIP: ffmpeg cannot generate FLV1 fixture"
    exit 77
fi

rtmp_server "$PORT" "$FLV" "$SERVER_LOG" || exit 1

OLR_NATIVE_RTMP=1 "$HARNESS" --url "$(rtmp_url "$PORT")" --name rtmp_unsupported \
    --outdir "$WORKDIR" --seconds 6 --width 640 --height 480 --fps 30 \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"

if grep -q "Native RTMP connected" "$HARNESS_ERR" &&
   ! grep -q "unsupported" "$HARNESS_ERR"; then
    echo "FAIL: unsupported stream connected without explicit unsupported reason"
    cat "$HARNESS_ERR"
    exit 1
fi

echo "PASS: unsupported native RTMP profile fails visibly"
