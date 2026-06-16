#!/usr/bin/env bash
# Local SRT e2e (Phase 2d): multi-source loss ISOLATION on the native ingest.
#
# Two INDEPENDENT native SRT sources, each with its own producer + bridge + relay.
# src0's relay is clean (0%); src1's relay drops LOSS%. Assert src1 recovers via ARQ
# (pktRcvDropTotal==0, full content) AND src0 is FULLY ISOLATED (relay dropped 0,
# pktRcvDropTotal==0, full content, no gap) — loss on src1 must not touch src0. This
# extends the 2c-a isolation finding to the loss dimension: each source owns its own
# libsrt socket, and the only SRT mutex guards startup/cleanup (not srt_recv), so
# isolation is structural.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX; native ingest via
# OLR_NATIVE_SRT=1 (set by the CTest registration). Usage: run_srt_loss_multi.sh <harness> [base]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23680}"
SECS="${OLR_SRT_LOSSMULTI_SECS:-12}"
LOSS="${OLR_SRT_LOSSMULTI_LOSS:-12}"
SEED="${OLR_SRT_LOSSMULTI_SEED:-1234}"
RELAY="$HERE/lossy_udp_relay.py"
# src0 at BASE (S/UDP/R = BASE/+1/+2); src1 at BASE+5 (+5/+6/+7) — 5-wide stride.
S0=$BASE;       U0=$((BASE+1));  R0=$((BASE+2))
S1=$((BASE+5)); U1=$((BASE+6));  R1=$((BASE+7))

srt_require_tools
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
[ -f "$RELAY" ] || { echo "FAIL: $RELAY missing"; exit 1; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.4; kill -9 "${PIDS[@]}" 2>/dev/null; }
    wait 2>/dev/null; rm -rf "$WORKDIR"
}
trap cleanup EXIT

# src0: clean relay (0% loss).
flash_marker_to_udps "$U0"; PIDS+=("$SRT_LAST_PID")
srt_bridge "$U0" "$S0"; PIDS+=("$SRT_LAST_PID")
python3 "$RELAY" "$R0" "127.0.0.1:$S0" 0 "$WORKDIR/r0.stats" "$SEED" & RP0=$!; PIDS+=("$RP0")
# src1: lossy relay (LOSS%).
flash_marker_to_udps "$U1"; PIDS+=("$SRT_LAST_PID")
srt_bridge "$U1" "$S1"; PIDS+=("$SRT_LAST_PID")
python3 "$RELAY" "$R1" "127.0.0.1:$S1" "$LOSS" "$WORKDIR/r1.stats" "$SEED" & RP1=$!; PIDS+=("$RP1")
sleep 1.5

echo "[srt-lossmulti] recording src0(clean) + src1(${LOSS}% loss) for ${SECS}s..."
"$HARNESS" --url "srt://127.0.0.1:$R0?transtype=live" --url "srt://127.0.0.1:$R1?transtype=live" \
    --outdir "$WORKDIR" --name srtlossmulti --seconds "$SECS" --fps 30 \
    >"$WORKDIR/out.txt" 2>"$WORKDIR/err.txt" &
HP=$!; PIDS+=("$HP")
wait "$HP"
kill -TERM "$RP0" "$RP1" 2>/dev/null; sleep 0.5; kill -9 "$RP0" "$RP1" 2>/dev/null   # flush relay stats

MKV="$(tail -n1 "$WORKDIR/out.txt")"
[ -n "$MKV" ] && [ -f "$MKV" ] || { echo "FAIL: no MKV produced (harness crash/hang?)"; exit 1; }
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
[ "${VTRACKS:-0}" = "2" ] || { echo "FAIL: expected 2 video tracks, got ${VTRACKS:-0} (a source never connected?)"; exit 1; }

relay_dropped() { awk -F'[= ]' '/dropped=/{print $2;exit}' "$1" 2>/dev/null; }
srt_stat_src() {  # $1=src_index $2=err_file $3=field — picks THIS source's srt_stats line
    grep "Source $1 " "$2" | grep 'srt_stats' | tail -1 | tr ' ' '\n' | awk -F= -v k="$3" '$1==k{print $2;exit}'
}
flash_count_gap() { flash_pts_series "$1" "$2" | awk 'NR>2{d=$1-p;if(d>mx)mx=d}{p=$1}END{print NR+0,(mx==""?0:mx)}'; }

DROP0="$(relay_dropped "$WORKDIR/r0.stats")"
DROP1="$(relay_dropped "$WORKDIR/r1.stats")"
SD0="$(srt_stat_src 0 "$WORKDIR/err.txt" pktRcvDropTotal)"
SD1="$(srt_stat_src 1 "$WORKDIR/err.txt" pktRcvDropTotal)"
read -r N0 G0 <<<"$(flash_count_gap "$MKV" 0)"
read -r N1 G1 <<<"$(flash_count_gap "$MKV" 1)"
echo "[srt-lossmulti] src0(clean): flashes=$N0 maxgap=${G0}s relay_dropped=${DROP0:-?} pktRcvDropTotal=${SD0:-?}"
echo "[srt-lossmulti] src1(${LOSS}%): flashes=$N1 maxgap=${G1}s relay_dropped=${DROP1:-?} pktRcvDropTotal=${SD1:-?}"

fail=0
# src1 recovered the injected loss via ARQ.
awk -v d="${DROP1:-0}" 'BEGIN{exit !(d+0 >= 20)}' || { echo "FAIL: src1 relay dropped ${DROP1:-0} (<20) — loss not injected"; fail=1; }
awk -v p="${SD1:-9}"   'BEGIN{exit !(p+0 == 0)}'  || { echo "FAIL: src1 pktRcvDropTotal=${SD1:-?} — loss not recovered"; fail=1; }
awk -v n="${N1:-0}" -v s="$SECS" 'BEGIN{exit !(n+0 >= s-3)}' || { echo "FAIL: src1 only ${N1} flashes — content not recovered"; fail=1; }
# src0 fully isolated — src1's loss did not touch it.
[ "${DROP0:-1}" = "0" ] || { echo "FAIL: src0 relay dropped ${DROP0} at 0% (should be 0)"; fail=1; }
awk -v p="${SD0:-9}"   'BEGIN{exit !(p+0 == 0)}'  || { echo "FAIL: src0 pktRcvDropTotal=${SD0:-?} — control source perturbed by src1's loss"; fail=1; }
awk -v n="${N0:-0}" -v s="$SECS" 'BEGIN{exit !(n+0 >= s-3)}' || { echo "FAIL: src0 only ${N0} flashes — control source lost content"; fail=1; }
awk -v g="${G0:-9}"    'BEGIN{exit !(g+0 <= 1.5)}' || { echo "FAIL: src0 max gap ${G0}s > 1.5s — control source disrupted by src1's loss"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: native SRT loss isolation — src1 recovered ${LOSS}% (dropTotal=0); src0 fully intact (${N0} flashes, no gap)"
exit 0
