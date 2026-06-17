#!/usr/bin/env bash
# Native-ingest soak matrix: {native SRT, native RTMP} x {H.264, H.265}.
#
# Runs four long-run stability soaks SEQUENTIALLY (no port/CPU contention) and
# prints a PASS/FAIL/SKIP summary. Each cell records a single native source for
# OLR_SOAK_SECONDS and asserts the engine never crashes/hangs and keeps delivering
# content the whole time:
#   - SRT  cells: 1 video track, ~1 flash/sec sustained, no >1.5s stall (run_srt_soak.sh).
#   - RTMP cells: "Native RTMP connected", hvc1 for H.265, packets >= 80% of fps*secs,
#                 plus stereo audio + 1kHz-tone RMS for the H.264 cell (run_rtmp_soak.sh).
#
#   OLR_SOAK_SECONDS   per-cell duration in seconds (default 1800 = 30 min)
# Usage: run_soak_matrix.sh <record_harness> <sync_harness> [base_port]
#
# A cell that can't run locally (e.g. no HEVC encoder, missing srt-live-transmit)
# SKIPs (exit 77) and does NOT fail the matrix; only a real soak failure does.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

RECORD_HARNESS="${1:?record_harness executable path required}"
SYNC_HARNESS="${2:?sync_harness executable path required}"
BASE="${3:-23900}"
SECS="${OLR_SOAK_SECONDS:-1800}"

for h in "$RECORD_HARNESS" "$SYNC_HARNESS"; do
    [ -x "$h" ] || { echo "FATAL: harness not found/executable: $h"; exit 2; }
done

OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT

RESULTS=()

# run_cell <label> <logfile> -- <command...>
# Honors any OLR_* env the caller prefixes; maps exit 0->PASS, 77->SKIP, else FAIL.
run_cell() {
    local label="$1" log="$2"; shift 2
    printf '\n======== %-9s start %s  (%ss/cell) ========\n' "$label" "$(date '+%H:%M:%S')" "$SECS"
    local started ended elapsed rc lastline result
    started=$(date +%s)
    "$@" >"$log" 2>&1
    rc=$?
    ended=$(date +%s)
    elapsed=$((ended - started))
    lastline="$(grep -E '^(PASS|FAIL|SKIP)' "$log" | tail -n1)"
    [ -n "$lastline" ] || lastline="$(tail -n1 "$log" 2>/dev/null)"
    case "$rc" in
    0) result="PASS" ;;
    77) result="SKIP" ;;
    *) result="FAIL" ;;
    esac
    printf '======== %-9s %s in %ss (rc=%s) ========\n  %s\n' \
        "$label" "$result" "$elapsed" "$rc" "$lastline"
    RESULTS+=("${label}|${result}|${elapsed}|${lastline}")
}

echo "Soak matrix: 4 cells x ${SECS}s (~$((SECS * 4 / 60)) min total). base port ${BASE}, start $(date '+%Y-%m-%d %H:%M:%S')."
echo "record_harness=$RECORD_HARNESS"
echo "sync_harness  =$SYNC_HARNESS"

OLR_SRT_SOAK_SECS="$SECS" OLR_SRT_SOAK_CODEC=avc \
    run_cell "SRT/H264" "$OUTDIR/srt_avc.log" \
    bash "$HERE/run_srt_soak.sh" "$SYNC_HARNESS" "$BASE"

OLR_SRT_SOAK_SECS="$SECS" OLR_SRT_SOAK_CODEC=hevc \
    run_cell "SRT/H265" "$OUTDIR/srt_hevc.log" \
    bash "$HERE/run_srt_soak.sh" "$SYNC_HARNESS" "$((BASE + 2))"

OLR_RTMP_RUN_SOAK=1 OLR_RTMP_SOAK_SECONDS="$SECS" OLR_RTMP_SOAK_CODEC=avc \
    run_cell "RTMP/H264" "$OUTDIR/rtmp_avc.log" \
    bash "$HERE/run_rtmp_soak.sh" "$RECORD_HARNESS" "$((BASE + 4))"

OLR_RTMP_RUN_SOAK=1 OLR_RTMP_SOAK_SECONDS="$SECS" OLR_RTMP_SOAK_CODEC=hevc \
    run_cell "RTMP/H265" "$OUTDIR/rtmp_hevc.log" \
    bash "$HERE/run_rtmp_soak.sh" "$RECORD_HARNESS" "$((BASE + 5))"

echo
echo "================ SOAK MATRIX SUMMARY (${SECS}s/cell) ================"
fails=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r label result elapsed line <<<"$r"
    printf '  %-10s %-4s  %5ss | %s\n' "$label" "$result" "$elapsed" "$line"
    [ "$result" = "FAIL" ] && fails=$((fails + 1))
done
echo "===================================================================="
echo "finished $(date '+%Y-%m-%d %H:%M:%S')"
if [ "$fails" -gt 0 ]; then
    echo "RESULT: ${fails} cell(s) FAILED"
    exit 1
fi
echo "RESULT: all cells PASS or SKIP (no failures)"
exit 0
