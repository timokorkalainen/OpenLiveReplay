#!/usr/bin/env bash
# Local SRT e2e (Phase 2b): per-source connection-status (#24) over real SRT.
#
# Run 1 (live):  4 SRT producers up -> sync_harness --report-connections must
#   report connected=4.
# Run 2 (teeth): only 3 bridges up; the 4th URL points at a DEAD SRT port (no
#   listener) -> the engine cannot connect it -> connected=3. Proves the count
#   reflects real connection state, not a constant.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX.
# Usage: run_srt_connect.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23540}"
SECS=8

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

# Spawn producers+bridges for views [0..live-1]; views >= live get a dead SRT port
# (no listener). Record all 4 URLs with --report-connections; echo the reported
# connected count (or -1 if the harness printed none).
run_connected() {  # $1=live_count(1..4) $2=tag
    local live="$1" tag="$2" i srt_port udp_port err
    local localpids=()
    URLS=()
    for i in 0 1 2 3; do
        srt_port=$((BASE + 2*i)); udp_port=$((BASE + 2*i + 1))
        URLS+=("$(srt_caller_url "$srt_port")")
        if [ "$i" -lt "$live" ]; then
            flash_marker_to_udps "$udp_port"; localpids+=("$SRT_LAST_PID")
            srt_bridge "$udp_port" "$srt_port"; localpids+=("$SRT_LAST_PID")
        fi
    done
    sleep 1.5
    err="$WORKDIR/${tag}.err"
    "$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
        --outdir "$WORKDIR" --name "srtconn_${tag}" --seconds "$SECS" --fps 30 \
        --report-connections >/dev/null 2>"$err"
    (( ${#localpids[@]} )) && kill "${localpids[@]}" 2>/dev/null
    wait "${localpids[@]}" 2>/dev/null
    awk -F= '/^connected=/{print $2; found=1} END{if(!found)print "-1"}' "$err"
}

echo "[srt-connect] base_port=$BASE"
LIVE4=$(run_connected 4 live)
echo "[srt-connect] live_run connected=$LIVE4 (expect 4)"
LIVE3=$(run_connected 3 dead)
echo "[srt-connect] dead_run connected=$LIVE3 (expect 3; 4th url has no listener)"

fail=0
[ "${LIVE4:-x}" = "4" ] || { echo "FAIL: live run reported connected=${LIVE4:-none}, expected 4 — not all SRT sources connected"; fail=1; }
[ "${LIVE3:-x}" = "3" ] || { echo "FAIL: teeth run reported connected=${LIVE3:-none}, expected 3 — connection-status does not discriminate a dead source"; fail=1; }
[ $fail -ne 0 ] && exit 1
echo "PASS: connection-status over SRT — 4 live => 4, 1 dead => 3"
exit 0
