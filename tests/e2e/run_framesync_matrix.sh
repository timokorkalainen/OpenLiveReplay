#!/usr/bin/env bash
# Broadcast frame-sync matrix: {lipsync, intercam, drift, drift_skew, timecode} x
# {srt, rtmp, ndi}. SRT is live now; RTMP/NDI cells are explicit SKIPs until
# their transport fixtures are wired into run_framesync_e2e.sh.
#
# Usage: run_framesync_matrix.sh <sync_harness> [base_port]
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-24100}"
SECS="${OLR_FRAMESYNC_SECS:-20}"

[ -x "$HARNESS" ] || { echo "FATAL: sync_harness not found/executable: $HARNESS"; exit 2; }

OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT

RESULTS=()

run_cell() {
    local transport="$1" scenario="$2" port="$3" log="$4"
    local label="${transport}/${scenario}"
    printf '\n======== %-18s start %s  (%ss/cell) ========\n' "$label" "$(date '+%H:%M:%S')" "$SECS"
    local started ended elapsed rc statusline reportline result
    started=$(date +%s)
    if [ "$scenario" = "drift_skew" ]; then
        env OLR_FRAMESYNC_TRANSPORT="$transport" OLR_FRAMESYNC_SECS="$SECS" \
            OLR_FRAMESYNC_SKEW_PPM="${OLR_FRAMESYNC_SKEW_PPM:-200}" \
            bash "$HERE/run_framesync_e2e.sh" "$HARNESS" "$scenario" "$port" >"$log" 2>&1
    else
        env OLR_FRAMESYNC_TRANSPORT="$transport" OLR_FRAMESYNC_SECS="$SECS" \
            bash "$HERE/run_framesync_e2e.sh" "$HARNESS" "$scenario" "$port" >"$log" 2>&1
    fi
    rc=$?
    ended=$(date +%s)
    elapsed=$((ended - started))
    statusline="$(grep -E '^(PASS|FAIL|SKIP)' "$log" | tail -n1)"
    reportline="$(grep -E '^REPORT' "$log" | tail -n1)"
    [ -n "$statusline" ] || statusline="$(tail -n1 "$log" 2>/dev/null)"
    [ -n "$reportline" ] || reportline="no report"
    case "$rc" in
    0) result="PASS" ;;
    77) result="SKIP" ;;
    *) result="FAIL" ;;
    esac
    printf '======== %-18s %s in %ss (rc=%s) ========\n  %s\n' \
        "$label" "$result" "$elapsed" "$rc" "$statusline"
    RESULTS+=("${label}|${result}|${elapsed}|${reportline}|${statusline}")
}

SCENARIOS=(lipsync intercam drift drift_skew timecode)
echo "Frame-sync matrix: $((3 * ${#SCENARIOS[@]})) cells x ${SECS}s. base port ${BASE}, start $(date '+%Y-%m-%d %H:%M:%S')."
echo "sync_harness=$HARNESS"

idx=0
for transport in srt rtmp ndi; do
    for scenario in "${SCENARIOS[@]}"; do
        port=$((BASE + idx * 10))
        run_cell "$transport" "$scenario" "$port" "$OUTDIR/${transport}_${scenario}.log"
        idx=$((idx + 1))
    done
done

echo
echo "================ FRAMESYNC MATRIX SUMMARY (${SECS}s/cell) ================"
fails=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r label result elapsed reportline statusline <<<"$r"
    printf '  %-18s %-4s  %5ss | %s | %s\n' "$label" "$result" "$elapsed" "$reportline" "$statusline"
    [ "$result" = "FAIL" ] && fails=$((fails + 1))
done
echo "======================================================================="
echo "finished $(date '+%Y-%m-%d %H:%M:%S')"
if [ "$fails" -gt 0 ]; then
    echo "RESULT: ${fails} cell(s) FAILED"
    exit 1
fi
echo "RESULT: all cells PASS or SKIP (no failures)"
exit 0
