#!/usr/bin/env bash
# Local SRT e2e (Phase 2b): inter-camera sync over 4 real SRT streams.
#
# One flash producer is tee'd to 4 SRT listeners (coincident, byte-identical
# content) and recorded into a 4-view MKV. We measure the per-flash max-min PTS
# spread across the 4 views.
#   Gate A (teeth):  every view produced >= MIN_FLASHES flashes. A view whose
#     source failed to connect is blue-fill (0 flashes) -> FAIL. A dead view
#     cannot fabricate a flash, so this is the real discriminator.
#   Gate B (bound):  max spread <= MAX_SPREAD_MS. Deliberately generous: the
#     engine anchors each source to first-packet ARRIVAL (no genlock, audit REF-2),
#     so coincident SRT is phase-locked-within-bounds, not zero-skew.
#
# Teeth demo: OLR_SRT_SYNC_DROP_VIEW=<i> skips view i's bridge so that view records
#   no flash and Gate A FAILS — proving the content gate discriminates.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX (an SRT-enabled avformat).
# Usage: run_srt_sync.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23520}"
SECS=8
MIN_FLASHES="${OLR_SRT_SYNC_MIN_FLASHES:-4}"
MAX_SPREAD_MS="${OLR_SRT_SYNC_MAX_SPREAD_MS:-250}"
DROP_VIEW="${OLR_SRT_SYNC_DROP_VIEW:-}"

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-sync] base_port=$BASE drop_view=${DROP_VIEW:-none}"

# 1. One flash producer tee'd to 4 UDP ports; 4 SRT listener bridges; 4 caller URLs.
UDP_PORTS=(); SRT_PORTS=(); URLS=()
for i in 0 1 2 3; do
    srt_port=$((BASE + 2*i)); udp_port=$((BASE + 2*i + 1))
    SRT_PORTS+=("$srt_port"); UDP_PORTS+=("$udp_port")
    URLS+=("$(srt_caller_url "$srt_port")")
done
flash_marker_to_udps "${UDP_PORTS[@]}"
for i in 0 1 2 3; do
    if [ "$DROP_VIEW" = "$i" ]; then
        echo "[srt-sync] (teeth) skipping bridge for view $i"
        continue
    fi
    srt_bridge "${UDP_PORTS[$i]}" "${SRT_PORTS[$i]}"
done
sleep 1.5  # let the producer + listeners come up before the engine connects

# 2. Record the 4 views.
OUT="$("$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
       --outdir "$WORKDIR" --name srtsync --seconds "$SECS" --fps 30)"
RC=$?
MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-sync] harness rc=$RC out=$MKV"
if [ $RC -ne 0 ] || [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: no output (rc=$RC) — engine could not record SRT (built with -DOLR_FFMPEG_SRT_PREFIX?)"; exit 1
fi

VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "4" ]; then echo "FAIL: expected 4 video tracks, got ${VTRACKS:-0}"; exit 1; fi

# 3. Flash-onset series per view.
for i in 0 1 2 3; do flash_pts_series "$MKV" "$i" > "$WORKDIR/v$i.txt"; done

# Gate A: every view has >= MIN_FLASHES flashes.
fail=0; counts=""
for i in 0 1 2 3; do
    c=$(wc -l < "$WORKDIR/v$i.txt" | tr -d ' '); counts="$counts v$i=$c"
    if [ "${c:-0}" -lt "$MIN_FLASHES" ]; then
        echo "FAIL: view $i produced ${c:-0} flashes (< $MIN_FLASHES) — source likely never connected (blue-fill)"; fail=1
    fi
done
echo "[srt-sync] flash_counts:$counts (min_required=$MIN_FLASHES)"

# Gate B: per-flash max-min spread across the 4 views (index-paired via paste).
STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" "$WORKDIR/v2.txt" "$WORKDIR/v3.txt" | awk '
    NF==4 {
        mn=$1; mx=$1
        for (k=2;k<=4;k++){ if($k<mn)mn=$k; if($k>mx)mx=$k }
        d=(mx-mn)*1000; s+=d; if(d>peak)peak=d; n++
    }
    END { if(n>0) printf "%d %.1f %.1f", n, s/n, peak; else printf "0 nan nan" }')
read -r NP MEANSPREAD MAXSPREAD <<<"$STATS"
echo "[srt-sync] flashes_paired=$NP spread_ms: mean=$MEANSPREAD max=$MAXSPREAD (bound=$MAX_SPREAD_MS)"

if [ "${NP:-0}" -lt "$MIN_FLASHES" ]; then
    echo "FAIL: only $NP flashes paired across all 4 views (< $MIN_FLASHES)"; fail=1
fi
if [ "$MAXSPREAD" = "nan" ] || ! awk -v m="$MAXSPREAD" -v b="$MAX_SPREAD_MS" 'BEGIN{exit !(m+0 <= b+0)}'; then
    echo "FAIL: max inter-camera flash spread ${MAXSPREAD}ms exceeds bound ${MAX_SPREAD_MS}ms"; fail=1
fi

[ $fail -ne 0 ] && exit 1
echo "PASS: 4-source SRT inter-camera sync — all views live, max spread ${MAXSPREAD}ms <= ${MAX_SPREAD_MS}ms"
exit 0
