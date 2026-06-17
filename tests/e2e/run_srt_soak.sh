#!/usr/bin/env bash
# Local SRT e2e (Phase 2c-c): long-run STABILITY SOAK on the native ingest.
#
# NOT a drift detector: on one machine the source and recording share the same wall
# clock, so the flash-PTS slope is ~1.0 by construction (the engine timestamps on
# arrival, not source emission) — real drift needs two machines (out of scope). This
# is a soak: a long native-SRT record must not crash/hang and must keep delivering
# content the whole time. The slope is reported as a diagnostic, not gated.
#
# Records over the native SRT ingest (the default, and only, SRT ingest); no
# SRT-enabled ffmpeg build is needed. Usage: run_srt_soak.sh <harness> [base]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23670}"
SECS="${OLR_SRT_SOAK_SECS:-30}"
S=$BASE; UDP=$((BASE+1))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.4; kill -9 "${PIDS[@]}" 2>/dev/null; }
    wait 2>/dev/null; rm -rf "$WORKDIR"
}
trap cleanup EXIT

flash_marker_to_udps "$UDP"; PIDS+=("$SRT_LAST_PID")
srt_bridge "$UDP" "$S"; PIDS+=("$SRT_LAST_PID")
sleep 1.5

echo "[srt-soak] recording one native SRT source for ${SECS}s..."
"$HARNESS" --url "$(srt_caller_url "$S")" --outdir "$WORKDIR" --name srtsoak \
    --seconds "$SECS" --fps 30 >"$WORKDIR/out.txt" 2>"$WORKDIR/err.txt" &
HP=$!; PIDS+=("$HP")
wait "$HP"

MKV="$(tail -n1 "$WORKDIR/out.txt")"
[ -n "$MKV" ] && [ -f "$MKV" ] || { echo "FAIL: no MKV produced (harness crash/hang under the ${SECS}s soak?)"; exit 1; }
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
[ "${VTRACKS:-0}" = "1" ] || { echo "FAIL: expected 1 video track, got ${VTRACKS:-0}"; exit 1; }

flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
# count, first-flash pts, max inter-flash gap, least-squares slope of (pts vs index).
read -r N FIRST GAP SLOPE <<<"$(awk '
    NR==1{first=$1}
    NR>1{d=$1-p; if(d>mx)mx=d}
    {x=NR-1; sx+=x; sy+=$1; sxx+=x*x; sxy+=x*$1; p=$1; n=NR}
    END{
        slope = (n>=2 && (n*sxx-sx*sx)!=0) ? (n*sxy-sx*sy)/(n*sxx-sx*sx) : 0
        printf "%d %.3f %.3f %.4f", n+0, first+0, (mx==""?0:mx), slope
    }' "$WORKDIR/v.txt")"
echo "[srt-soak] flashes=$N first=${FIRST}s maxgap=${GAP}s slope=${SLOPE} (slope = diagnostic only)"

fail=0
awk -v f="${FIRST:-9}" 'BEGIN{exit !(f+0 < 3.0)}' || { echo "FAIL: first flash at ${FIRST}s (>= 3s) — slow warm-up"; fail=1; }
awk -v n="${N:-0}" -v s="$SECS" 'BEGIN{exit !(n+0 >= s-3)}' || { echo "FAIL: only ${N} flashes over ${SECS}s (< SECS-3) — content not sustained"; fail=1; }
awk -v g="${GAP:-9}" 'BEGIN{exit !(g+0 <= 1.5)}' || { echo "FAIL: max gap ${GAP}s > 1.5s — mid-run stall"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: native SRT ingest stable over ${SECS}s soak — ${N} flashes, no stall (slope ${SLOPE})"
exit 0
