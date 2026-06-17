#!/usr/bin/env bash
# Local SRT e2e (Phase 2b): per-source trim (#28) over real SRT.
#
# Port of the proven synthetic intercam_trim to SRT. One flash producer is tee'd
# to TWO coincident SRT views; we record once untrimmed and once with +T trim on
# the LAST source (view1), and measure the mean (view0 - view1) flash offset each
# time. The trim DELAYS view1, so its flash PTS increases and the signed
# (view0 - view1) offset DECREASES by ~T (matches intercam_trim: "delay =>
# trimmed ≈ untrimmed − TRIM").
#   Gate: (trimmed_offset - untrimmed_offset) ≈ -T within TOL_MS.
# A no-op/broken trim leaves the two offsets equal (diff ≈ 0) and FAILS the gate.
# Measuring the run-to-run DIFFERENCE cancels any systematic per-view connect-order
# bias, leaving only arrival jitter (which TOL_MS covers).
#
# The engine ingests srt:// over its native SRT path — no SRT-enabled ffmpeg build
# is needed.
# Usage: run_srt_trim.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23530}"
SECS=8
T="${OLR_SRT_TRIM_MS:-300}"
TOL="${OLR_SRT_TRIM_TOL_MS:-120}"
SRT0=$BASE;       UDP0=$((BASE+1))
SRT1=$((BASE+2)); UDP1=$((BASE+3))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-trim] base_port=$BASE T=${T}ms tol=${TOL}ms"

# Record the tee'd 2-view setup with a given trim on the LAST source (view1) and
# echo the mean (view0 - view1) flash offset in ms, or "nan". Fresh producer +
# bridges per sub-run (so each run's arrival skew is independent and cancels in
# the trimmed-minus-untrimmed difference).
measure_offset() {  # $1=trim_ms $2=tag
    local trim="$1" tag="$2" pp pb0 pb1 mkv
    flash_marker_to_udps "$UDP0" "$UDP1"; pp=$SRT_LAST_PID
    srt_bridge "$UDP0" "$SRT0"; pb0=$SRT_LAST_PID
    srt_bridge "$UDP1" "$SRT1"; pb1=$SRT_LAST_PID
    sleep 1.5
    mkv=$("$HARNESS" --url "$(srt_caller_url "$SRT0")" --url "$(srt_caller_url "$SRT1")" \
            --outdir "$WORKDIR" --name "srttrim_${tag}" --seconds "$SECS" --fps 30 --trim "$trim" | tail -n1)
    kill "$pp" "$pb0" "$pb1" 2>/dev/null; wait "$pp" "$pb0" "$pb1" 2>/dev/null
    if [ -z "$mkv" ] || [ ! -s "$mkv" ]; then echo "nan"; return; fi
    flash_pts_series "$mkv" 0 > "$WORKDIR/${tag}_0.txt"
    flash_pts_series "$mkv" 1 > "$WORKDIR/${tag}_1.txt"
    paste "$WORKDIR/${tag}_0.txt" "$WORKDIR/${tag}_1.txt" | awk '
        NF==2 { d=($1-$2)*1000; s+=d; n++ }
        END { if(n>0) printf "%.1f", s/n; else printf "nan" }'
}

UNTRIMMED=$(measure_offset 0 base)
TRIMMED=$(measure_offset "$T" trim)
echo "[srt-trim] untrimmed_ms=$UNTRIMMED trimmed_ms=$TRIMMED (applied=$T; expect trimmed-untrimmed ≈ -$T)"

if [ "$UNTRIMMED" = "nan" ] || [ "$TRIMMED" = "nan" ]; then
    echo "FAIL: could not measure flash offset (no output / no flashes) — SRT connect or extraction failed"; exit 1
fi

SHIFT=$(awk -v u="$UNTRIMMED" -v t="$TRIMMED" 'BEGIN{printf "%.1f", t-u}')
PASS=$(awk -v sh="$SHIFT" -v tt="$T" -v tol="$TOL" 'BEGIN{ d=sh-(-tt); if(d<0)d=-d; printf (d<=tol)?"1":"0" }')
echo "[srt-trim] measured_shift_ms=$SHIFT expected=-$T tol=$TOL"
if [ "$PASS" != "1" ]; then
    echo "FAIL: trim shift ${SHIFT}ms not within ${TOL}ms of -${T}ms — per-source trim not applied as expected over SRT"; exit 1
fi
echo "PASS: per-source trim over SRT — view1 delayed by ~${T}ms (measured shift ${SHIFT}ms)"
exit 0
