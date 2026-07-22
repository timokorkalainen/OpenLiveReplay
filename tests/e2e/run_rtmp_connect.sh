#!/usr/bin/env bash
# Native RTMP e2e: per-source connection status over real RTMP ingest.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23850}"
SECS=8

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

FLASH="$WORKDIR/flash.flv"
rtmp_generate_flash_flv "$FLASH" "$SECS"

run_connected() {
    local live="$1" tag="$2" i port err
    URLS=()
    for i in 0 1 2 3; do
        port=$((BASE + i))
        URLS+=("$(rtmp_url "$port")")
        if [ "$i" -lt "$live" ]; then
            rtmp_server "$port" "$FLASH" "$WORKDIR/${tag}_server${i}.log" || true
        fi
    done
    err="$WORKDIR/${tag}.err"
    "$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
        --outdir "$WORKDIR" --name "rtmpconn_${tag}" --seconds "$SECS" --fps 30 \
        --report-connections >/dev/null 2>"$err"
    kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; PIDS=()
    awk -F= '/^connected=/{print $2; found=1} END{if(!found)print "-1"}' "$err"
}

echo "[rtmp-connect] base_port=$BASE"
LIVE4=$(run_connected 4 live)
echo "[rtmp-connect] live_run connected=$LIVE4 (expect 4)"
LIVE3=$(run_connected 3 dead)
echo "[rtmp-connect] dead_run connected=$LIVE3 (expect 3; 4th url has no listener)"

fail=0
[ "${LIVE4:-x}" = "4" ] || { echo "FAIL: live run reported connected=${LIVE4:-none}, expected 4"; fail=1; }
[ "${LIVE3:-x}" = "3" ] || { echo "FAIL: teeth run reported connected=${LIVE3:-none}, expected 3"; fail=1; }
[ $fail -ne 0 ] && exit 1
echo "PASS: connection-status over native RTMP -- 4 live => 4, 1 dead => 3"
exit 0
