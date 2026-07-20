#!/usr/bin/env bash
# Native RTMP e2e: per-source trim over real RTMP ingest.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23840}"
SECS=8
T="${OLR_RTMP_TRIM_MS:-300}"
TOL="${OLR_RTMP_TRIM_TOL_MS:-120}"

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[rtmp-trim] base_port=$BASE T=${T}ms tol=${TOL}ms"

FLASH="$WORKDIR/flash.flv"
rtmp_generate_flash_flv "$FLASH" "$SECS"

measure_offset() {
    local trim="$1" tag="$2" p0=$BASE p1=$((BASE + 1)) mkv
    rtmp_server "$p0" "$FLASH" "$WORKDIR/${tag}_server0.log" || { echo "nan"; return; }
    rtmp_server "$p1" "$FLASH" "$WORKDIR/${tag}_server1.log" || { echo "nan"; return; }
    mkv=$("$HARNESS" --url "$(rtmp_url "$p0")" --url "$(rtmp_url "$p1")" \
            --outdir "$WORKDIR" --name "rtmptrim_${tag}" --seconds "$SECS" --fps 30 --trim "$trim" | tail -n1)
    kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; PIDS=()
    if [ -z "$mkv" ] || [ ! -s "$mkv" ]; then echo "nan"; return; fi
    flash_pts_series "$mkv" 0 > "$WORKDIR/${tag}_0.txt"
    flash_pts_series "$mkv" 1 > "$WORKDIR/${tag}_1.txt"
    paste "$WORKDIR/${tag}_0.txt" "$WORKDIR/${tag}_1.txt" | awk '
        NF==2 { d=($1-$2)*1000; s+=d; n++ }
        END { if(n>0) printf "%.1f", s/n; else printf "nan" }'
}

UNTRIMMED=$(measure_offset 0 base)
TRIMMED=$(measure_offset "$T" trim)
echo "[rtmp-trim] untrimmed_ms=$UNTRIMMED trimmed_ms=$TRIMMED (applied=$T; expect trimmed-untrimmed ~= -$T)"

if [ "$UNTRIMMED" = "nan" ] || [ "$TRIMMED" = "nan" ]; then
    echo "FAIL: could not measure flash offset (no output / no flashes) -- RTMP connect or extraction failed"; exit 1
fi

SHIFT=$(awk -v u="$UNTRIMMED" -v t="$TRIMMED" 'BEGIN{printf "%.1f", t-u}')
PASS=$(awk -v sh="$SHIFT" -v tt="$T" -v tol="$TOL" 'BEGIN{ d=sh-(-tt); if(d<0)d=-d; printf (d<=tol)?"1":"0" }')
echo "[rtmp-trim] measured_shift_ms=$SHIFT expected=-$T tol=$TOL"
if [ "$PASS" != "1" ]; then
    echo "FAIL: trim shift ${SHIFT}ms not within ${TOL}ms of -${T}ms -- per-source trim not applied over RTMP"; exit 1
fi
echo "PASS: per-source trim over native RTMP -- view1 delayed by ~${T}ms (measured shift ${SHIFT}ms)"
exit 0
