#!/usr/bin/env bash
# Native RTMP e2e: inter-camera sync over 4 coincident RTMP streams.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23830}"
SECS=8
MIN_FLASHES="${OLR_RTMP_SYNC_MIN_FLASHES:-4}"
MAX_SPREAD_MS="${OLR_RTMP_SYNC_MAX_SPREAD_MS:-250}"
DROP_VIEW="${OLR_RTMP_SYNC_DROP_VIEW:-}"

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[rtmp-sync] base_port=$BASE drop_view=${DROP_VIEW:-none}"

FLASH="$WORKDIR/flash.flv"
rtmp_generate_flash_flv "$FLASH" "$SECS"

URLS=()
for i in 0 1 2 3; do
    port=$((BASE + i))
    URLS+=("$(rtmp_url "$port")")
    if [ "$DROP_VIEW" = "$i" ]; then
        echo "[rtmp-sync] (teeth) skipping server for view $i"
        continue
    fi
    rtmp_server "$port" "$FLASH" "$WORKDIR/server${i}.log" || exit 1
done

OUT="$("$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
       --outdir "$WORKDIR" --name rtmpsync --seconds "$SECS" --fps 30)"
RC=$?
MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[rtmp-sync] harness rc=$RC out=$MKV"
if [ $RC -ne 0 ] || [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: no output (rc=$RC) -- engine could not record RTMP"; exit 1
fi

VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "4" ]; then echo "FAIL: expected 4 video tracks, got ${VTRACKS:-0}"; exit 1; fi

for i in 0 1 2 3; do flash_pts_series "$MKV" "$i" > "$WORKDIR/v$i.txt"; done

fail=0; counts=""
for i in 0 1 2 3; do
    c=$(wc -l < "$WORKDIR/v$i.txt" | tr -d ' '); counts="$counts v$i=$c"
    if [ "${c:-0}" -lt "$MIN_FLASHES" ]; then
        echo "FAIL: view $i produced ${c:-0} flashes (< $MIN_FLASHES) -- source likely never connected"
        fail=1
    fi
done
echo "[rtmp-sync] flash_counts:$counts (min_required=$MIN_FLASHES)"

STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" "$WORKDIR/v2.txt" "$WORKDIR/v3.txt" | awk '
    NF==4 {
        mn=$1; mx=$1
        for (k=2;k<=4;k++){ if($k<mn)mn=$k; if($k>mx)mx=$k }
        d=(mx-mn)*1000; s+=d; if(d>peak)peak=d; n++
    }
    END { if(n>0) printf "%d %.1f %.1f", n, s/n, peak; else printf "0 nan nan" }')
read -r NP MEANSPREAD MAXSPREAD <<<"$STATS"
echo "[rtmp-sync] flashes_paired=$NP spread_ms: mean=$MEANSPREAD max=$MAXSPREAD (bound=$MAX_SPREAD_MS)"

if [ "${NP:-0}" -lt "$MIN_FLASHES" ]; then echo "FAIL: only $NP flashes paired across all 4 views (< $MIN_FLASHES)"; fail=1; fi
if [ "$MAXSPREAD" = "nan" ] || ! awk -v m="$MAXSPREAD" -v b="$MAX_SPREAD_MS" 'BEGIN{exit !(m+0 <= b+0)}'; then
    echo "FAIL: max inter-camera flash spread ${MAXSPREAD}ms exceeds bound ${MAX_SPREAD_MS}ms"; fail=1
fi

[ $fail -ne 0 ] && exit 1
echo "PASS: 4-source native RTMP inter-camera sync -- all views live, max spread ${MAXSPREAD}ms <= ${MAX_SPREAD_MS}ms"
exit 0
