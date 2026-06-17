#!/usr/bin/env bash
# Local SRT e2e (AUD-4): A/V lip-sync over the NATIVE SRT ingest. A co-timed
# flash+beep MPEG-TS source goes UDP -> srt-live-transmit -> srt:// -> native
# ingest (PCR-anchored). Assert the recorded mean(audio_pts - video_pts) is within
# EBU R37 (audio lead <=40ms, lag <=60ms). Pre-AUD-4 the independent audio anchor
# baked in ~+60ms; the shared PCR anchor collapses it toward 0.
#
# Records over the native SRT ingest (the default, and only, SRT ingest).
# Usage: run_srt_lipsync.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23710}"
SECS="${OLR_SRT_LIPSYNC_SECS:-10}"
UDP=$BASE; SRT=$((BASE+1))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

flash_beep_marker_to_udp "$UDP"
srt_bridge "$UDP" "$SRT"
sleep 1.5
MKV=$("$HARNESS" --url "$(srt_caller_url "$SRT")" \
        --outdir "$WORKDIR" --name srt_lipsync --seconds "$SECS" --fps 30 | tail -n1)
[ -n "$MKV" ] && [ -s "$MKV" ] || { echo "FAIL: native SRT lipsync produced no MKV"; exit 1; }

flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
beep_pts_series  "$MKV" 0 > "$WORKDIR/a.txt"
# Nearest-match each flash to a beep within 200ms (co-timed per source-second);
# robust to a spurious silencedetect edge. Signed mean/max (audio - video) ms.
read -r NP MEAN MAX <<<"$(awk '
    FNR==NR { f[FNR]=$1; nf=FNR; next }
    { b[FNR]=$1; nb=FNR }
    END {
        for (i=1;i<=nf;i++) { best=1e9; bj=-1;
            for (j=1;j<=nb;j++) { dd=b[j]-f[i]; ad=(dd<0?-dd:dd); if(ad<best){best=ad;bj=j} }
            if (bj>0 && best<=0.2) { d=(b[bj]-f[i])*1000; s+=d; am=(d<0?-d:d); if(am>mx)mx=am; n++ } }
        if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan"
    }' "$WORKDIR/v.txt" "$WORKDIR/a.txt")"
echo "[srt-lipsync] base=$BASE pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37 -40..+60)"

[ "${NP:-0}" -ge 3 ] || { echo "FAIL: only ${NP:-0} flash/beep pairs — unreliable"; exit 1; }
if awk -v m="$MEAN" 'BEGIN{exit !(m >= -40 && m <= 60)}'; then
    echo "PASS: native SRT A/V offset ${MEAN}ms within EBU R37 (PCR-anchored)"; exit 0
fi
echo "FAIL: native SRT A/V offset ${MEAN}ms outside EBU R37 (-40..+60)"
exit 1
