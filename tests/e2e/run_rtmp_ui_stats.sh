#!/usr/bin/env bash
# Local native RTMP e2e: the RTMP stats DATA PATH that feeds the connection-status
# UI. The native RTMP ingest samples its byte/packet/keyframe/decode counters and
# pushes an IngestStats(kind=Rtmp) via reportStats -> StreamWorker::statsUpdated ->
# ReplayManager::sourceStatsUpdated; sync_harness --report-stats prints whatever
# reaches that RM signal as a "stats src=0 kind=rtmp ..." line. We drive ONE source
# from the in-repo Python RTMP fixture (no live publisher) and assert the harness's
# reported per-source stats carry REAL telemetry: a kind=rtmp line exists at all,
# bytes>0 (real RTMP payload flowed end to end), and decodefail==0 (the fixture FLV
# is decodable). We also require the native backend marker so a successful FFmpeg
# fallback can't masquerade as a pass. This proves the exact numbers the UI dot/
# tooltip will read, without needing QML — the RTMP twin of e2e_native_srt_ui_stats.
#
# Native RTMP is the default ingest backend on Apple, so no env toggle is needed.
# Usage: run_rtmp_ui_stats.sh <sync_harness_exe> [rtmp_port]
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
RTMP_PORT="${2:-23800}"
SECS="${OLR_RTMP_UISTATS_SECS:-10}"
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

FLV="$WORKDIR/source.flv"
SERVER_LOG="$WORKDIR/rtmp_server.log"
HARNESS_OUT="$WORKDIR/out"
HARNESS_ERR="$WORKDIR/err"

echo "[${RTMP_SCHEME:-rtmp}-uistats] port=$RTMP_PORT secs=$SECS"

# Deterministic H.264/AAC FLV; pad past the record window so the fixture keeps
# feeding for the full --seconds (the smoke gate records the whole clip).
rtmp_generate_tone_flv "$FLV" 1000 "$((SECS + 4))"
rtmp_server "$RTMP_PORT" "$FLV" "$SERVER_LOG" || exit 1

URL="$(rtmp_url "$RTMP_PORT")"
"$HARNESS" --url "$URL" --outdir "$WORKDIR" --name uistats \
    --seconds "$SECS" --fps 30 --report-stats \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"
RC=$?
echo "[${RTMP_SCHEME:-rtmp}-uistats] harness rc=$RC"

# field from the harness "stats src=0 kind=rtmp bytes=.. lastpktage=.. keyframeage=.. decodefail=.." line
hstat() {  # $1=err file  $2=field
    grep '^stats src=0 kind=rtmp ' "$1" | tail -1 | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2; exit}'
}

BYTES="$(hstat "$HARNESS_ERR" bytes)"
DF="$(hstat "$HARNESS_ERR" decodefail)"
echo "[${RTMP_SCHEME:-rtmp}-uistats] bytes=${BYTES:-?} decodefail=${DF:-?}"

fail=0
# The data path delivered RTMP stats at all (harness saw a "kind=rtmp" line).
if [ -z "${BYTES:-}" ]; then
    echo "FAIL: no kind=rtmp stats line — engine -> ReplayManager::sourceStatsUpdated path carried no RTMP stats"
    fail=1
fi
# Real RTMP payload flowed through the RM signal to the harness.
if [ -n "${BYTES:-}" ] && ! awk -v b="${BYTES:-0}" 'BEGIN{exit !(b+0 > 0)}'; then
    echo "FAIL: kind=rtmp bytes=${BYTES:-?} not > 0 — no RTMP payload reached the UI signal"
    fail=1
fi
# The fixture FLV is decodable; no spurious decode failures on a clean stream.
if [ -z "${DF:-}" ] || ! awk -v d="${DF:-1}" 'BEGIN{exit !(d+0 == 0)}'; then
    echo "FAIL: kind=rtmp decodefail=${DF:-?} — expected 0 on a decodable fixture FLV"
    fail=1
fi
# Native backend produced these stats (not an FFmpeg fallback).
if ! grep -q "Native RTMP connected" "$HARNESS_ERR" "$HARNESS_OUT"; then
    echo "FAIL: harness did not use the native RTMP backend (missing 'Native RTMP connected' log)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- harness stderr ---"
    rtmp_redact_log "$HARNESS_ERR" | tail -n 80
    echo "--- RTMP server log ---"
    rtmp_redact_log "$SERVER_LOG"
    exit 1
fi

echo "PASS: RTMP stats data path delivered to the UI signal (bytes=$BYTES decodefail=$DF)"
exit 0
