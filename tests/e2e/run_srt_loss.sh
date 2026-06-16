#!/usr/bin/env bash
# Local SRT e2e (Phase 2c-b): packet-loss recovery on the NATIVE SRT ingest.
#
# A lossy_udp_relay.py sits on the SRT link (engine <-> relay <-> srt-live-transmit)
# and drops a seeded % of DOWNSTREAM SRT DATA packets (control passes). We record one
# source through the relay at 0% / 12% / 88% loss and prove SRT's ARQ recovers
# moderate loss (no content loss, retransmits happened) and degrades under heavy loss.
#
#   baseline 0%  -> reference flash count B (and proves the relay drops 0).
#   moderate 12% -> relay dropped data; native srt_stats pktRcvRetrans>0 (ARQ ran) &
#                   pktRcvDropTotal==0 (NOTHING finally unrecovered); content full
#                   (flashes >= 0.85*B, no gap >1.5s). pktRcvLossTotal (DETECTED loss)
#                   is expectedly non-zero — those packets were retransmitted; only
#                   pktRcvDropTotal counts loss SRT gave up on.
#   heavy 88%    -> run exits cleanly AND content degrades (flashes <= 0.5*B OR a gap
#                   >=2s) — proving the injected loss is real and the gate discriminates.
#                   88% is required because SRT's 500ms ARQ window on loopback fully
#                   recovers loss well past 60%; ~88% reliably exhausts the retransmit
#                   budget so packets are finally dropped (gap >=2s).
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX; native ingest via
# OLR_NATIVE_SRT=1 (set by the CTest registration). Usage: run_srt_loss.sh <harness> [base]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23660}"
SECS="${OLR_SRT_LOSS_SECS:-10}"
SEED="${OLR_SRT_LOSS_SEED:-1234}"
LOSS_MOD="${OLR_SRT_LOSS_MOD:-12}"
LOSS_HEAVY="${OLR_SRT_LOSS_HEAVY:-88}"
RELAY="$HERE/lossy_udp_relay.py"

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

# Record one source through the relay at $1=loss% $2=tag $3=port_base. Each run uses
# its own port_base so a lingering socket can't collide with the next.
measure() {
    local loss="$1" tag="$2" pb="$3"
    local S=$pb UDP=$((pb+1)) R=$((pb+2)) pp brp rp hp
    flash_marker_to_udps "$UDP"; pp=$SRT_LAST_PID
    srt_bridge "$UDP" "$S"; brp=$SRT_LAST_PID
    python3 "$RELAY" "$R" "127.0.0.1:$S" "$loss" "$WORKDIR/$tag.stats" "$SEED" &
    rp=$!; PIDS+=("$rp")
    sleep 1.5
    "$HARNESS" --url "srt://127.0.0.1:$R?transtype=live" \
        --outdir "$WORKDIR" --name "loss_$tag" --seconds "$SECS" --fps 30 \
        >"$WORKDIR/$tag.out" 2>"$WORKDIR/$tag.err" &
    hp=$!; PIDS+=("$hp")
    wait "$hp"
    kill -TERM "$rp" 2>/dev/null; sleep 0.5; kill -9 "$rp" 2>/dev/null   # flush relay stats
    kill "$pp" "$brp" 2>/dev/null; wait "$pp" "$brp" 2>/dev/null
}

# count + max-gap of the recorded view's flashes -> "count maxgap"
flash_stats() {
    flash_pts_series "$1" 0 | awk 'NR>1{d=$1-p; if(d>mx)mx=d} {p=$1} END{print NR+0, (mx==""?0:mx)}'
}
relay_dropped() { awk -F'[= ]' '/dropped=/{print $2; exit}' "$1" 2>/dev/null; }
srt_stat() {  # $1=err file $2=field -> value
    grep 'srt_stats' "$1" | tail -1 | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2; exit}'
}

echo "[srt-loss] base=$BASE secs=$SECS seed=$SEED mod=${LOSS_MOD}% heavy=${LOSS_HEAVY}%"
measure 0          base  "$BASE"
measure "$LOSS_MOD"  mod  "$((BASE+4))"
measure "$LOSS_HEAVY" heavy "$((BASE+8))"

MKV0="$(tail -n1 "$WORKDIR/base.out")"; read -r B0 G0 <<<"$(flash_stats "$MKV0")"
MKV1="$(tail -n1 "$WORKDIR/mod.out")";  read -r B1 G1 <<<"$(flash_stats "$MKV1")"
MKV2="$(tail -n1 "$WORKDIR/heavy.out")";read -r B2 G2 <<<"$(flash_stats "$MKV2")"
DROP0="$(relay_dropped "$WORKDIR/base.stats")"
DROP1="$(relay_dropped "$WORKDIR/mod.stats")"
RETR1="$(srt_stat "$WORKDIR/mod.err" pktRcvRetrans)"
DROPT1="$(srt_stat "$WORKDIR/mod.err" pktRcvDropTotal)"
echo "[srt-loss] baseline: flashes=$B0 maxgap=${G0}s relay_dropped=${DROP0:-?}"
echo "[srt-loss] moderate(${LOSS_MOD}%): flashes=$B1 maxgap=${G1}s relay_dropped=${DROP1:-?} pktRcvRetrans=${RETR1:-?} pktRcvDropTotal=${DROPT1:-?}"
echo "[srt-loss] heavy(${LOSS_HEAVY}%): flashes=$B2 maxgap=${G2}s"

fail=0
# Baseline sane + relay injected nothing at 0%.
[ "${B0:-0}" -ge "$((SECS-2))" ] || { echo "FAIL: baseline only ${B0:-0} flashes (< $((SECS-2)))"; fail=1; }
[ "${DROP0:-1}" = "0" ] || { echo "FAIL: relay dropped ${DROP0} at 0% loss (should be 0)"; fail=1; }
# Moderate: loss injected, ARQ engaged + recovered, content near-full.
awk -v d="${DROP1:-0}"  'BEGIN{exit !(d+0 >= 20)}'        || { echo "FAIL: relay dropped only ${DROP1:-0} data pkts at ${LOSS_MOD}% — loss not injected"; fail=1; }
awk -v r="${RETR1:-0}"  'BEGIN{exit !(r+0 > 0)}'          || { echo "FAIL: pktRcvRetrans=${RETR1:-?} — SRT ARQ did not retransmit"; fail=1; }
# Airtight recovery proof: pktRcvDropTotal is SRT's too-late-to-play (finally
# UNRECOVERED) loss. ARQ recovered everything iff it is 0. (pktRcvLossTotal being
# non-zero is fine — that is DETECTED loss which was then retransmitted; only
# pktRcvDropTotal counts loss SRT gave up on.)
awk -v p="${DROPT1:-999}" 'BEGIN{exit !(p+0 == 0)}' \
    || { echo "FAIL: pktRcvDropTotal=${DROPT1:-?} — SRT could NOT recover all the injected loss"; fail=1; }
awk -v a="${B1:-0}" -v b="${B0:-1}" 'BEGIN{exit !(a+0 >= 0.85*b)}' || { echo "FAIL: moderate flashes ${B1} < 0.85*baseline ${B0} — content not recovered"; fail=1; }
awk -v g="${G1:-9}"     'BEGIN{exit !(g+0 <= 1.5)}'       || { echo "FAIL: moderate max gap ${G1}s > 1.5s — content not continuous"; fail=1; }
# Heavy: clean exit + measurable degradation (teeth).
[ -s "$MKV2" ] || { echo "FAIL: heavy run produced no MKV (engine hang/crash under loss)"; fail=1; }
awk -v a="${B2:-99}" -v b="${B0:-1}" -v g="${G2:-0}" 'BEGIN{exit !(a+0 <= 0.5*b || g+0 >= 2.0)}' \
    || { echo "FAIL: heavy loss did not degrade (flashes ${B2} vs base ${B0}, maxgap ${G2}s) — teeth did not bite; raise OLR_SRT_LOSS_HEAVY (current: ${LOSS_HEAVY}%)"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: native SRT ingest recovers ${LOSS_MOD}% loss via ARQ (retrans=${RETR1}, pktRcvDropTotal=${DROPT1:-?}, content intact); degrades under ${LOSS_HEAVY}%"
exit 0
