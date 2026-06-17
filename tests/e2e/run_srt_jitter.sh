#!/usr/bin/env bash
# Local SRT e2e (Phase 2d): packet-REORDERING recovery on the native ingest.
#
# lossy_udp_relay.py's reorder mode holds each downstream SRT DATA packet a random
# 0..JITTER_MS before forwarding (control packets pass immediately), reordering the
# link. SRT's TSBPD (500ms SRTO_LATENCY) must re-order packets back into sequence;
# we assert the recording is complete + continuous despite reordering, the engine
# finally dropped NOTHING (pktRcvDropTotal==0 = TSBPD recovered), and the relay
# ACTUALLY reordered packets (reordered>0 — else a no-op relay would false-pass).
#
# Records over the native SRT ingest (the default, and only, SRT ingest); no
# SRT-enabled ffmpeg build is needed. Usage: run_srt_jitter.sh <harness> [base]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23690}"
SECS="${OLR_SRT_JITTER_SECS:-12}"
JITTER_MS="${OLR_SRT_JITTER_MS:-120}"   # < 500ms SRTO_LATENCY so TSBPD recovers
SEED="${OLR_SRT_JITTER_SEED:-1234}"
RELAY="$HERE/lossy_udp_relay.py"
S=$BASE; UDP=$((BASE+1)); R=$((BASE+2))

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

flash_marker_to_udps "$UDP"; PIDS+=("$SRT_LAST_PID")
srt_bridge "$UDP" "$S"; PIDS+=("$SRT_LAST_PID")
# Relay: 0% loss, reorder by up to JITTER_MS.
python3 "$RELAY" "$R" "127.0.0.1:$S" 0 "$WORKDIR/relay.stats" "$SEED" "$JITTER_MS" &
RP=$!; PIDS+=("$RP")
sleep 1.5

echo "[srt-jitter] recording through reorder=${JITTER_MS}ms relay for ${SECS}s..."
"$HARNESS" --url "srt://127.0.0.1:$R?transtype=live" --outdir "$WORKDIR" --name srtjitter \
    --seconds "$SECS" --fps 30 >"$WORKDIR/out.txt" 2>"$WORKDIR/err.txt" &
HP=$!; PIDS+=("$HP")
wait "$HP"
kill -TERM "$RP" 2>/dev/null; sleep 0.5; kill -9 "$RP" 2>/dev/null   # flush relay stats

MKV="$(tail -n1 "$WORKDIR/out.txt")"
[ -n "$MKV" ] && [ -f "$MKV" ] || { echo "FAIL: no MKV produced (harness crash/hang?)"; exit 1; }
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
[ "${VTRACKS:-0}" = "1" ] || { echo "FAIL: expected 1 video track, got ${VTRACKS:-0}"; exit 1; }

REORD="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^reordered=/){split($i,a,"=");print a[2];exit}}' "$WORKDIR/relay.stats" 2>/dev/null)"
flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
read -r N GAP <<<"$(awk 'NR>2{d=$1-p; if(d>mx)mx=d} {p=$1} END{print NR+0, (mx==""?0:mx)}' "$WORKDIR/v.txt")"
DROPT="$(grep 'srt_stats' "$WORKDIR/err.txt" | tail -1 | tr ' ' '\n' | awk -F= '$1=="pktRcvDropTotal"{print $2;exit}')"
echo "[srt-jitter] reordered=${REORD:-?} flashes=$N maxgap=${GAP}s pktRcvDropTotal=${DROPT:-?}"

fail=0
awk -v r="${REORD:-0}"  'BEGIN{exit !(r+0 > 0)}'        || { echo "FAIL: relay reordered=${REORD:-0} — reordering NOT injected (no-op relay?)"; fail=1; }
awk -v n="${N:-0}" -v s="$SECS" 'BEGIN{exit !(n+0 >= s-3)}' || { echo "FAIL: only ${N} flashes over ${SECS}s — content lost despite reorder"; fail=1; }
awk -v g="${GAP:-9}"    'BEGIN{exit !(g+0 <= 1.5)}'     || { echo "FAIL: max gap ${GAP}s > 1.5s — reordering not recovered (out-of-order/gap)"; fail=1; }
awk -v p="${DROPT:-999}" 'BEGIN{exit !(p+0 == 0)}'      || { echo "FAIL: pktRcvDropTotal=${DROPT:-?} — TSBPD could not recover the reordering"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: native SRT ingest recovers ${JITTER_MS}ms reordering via TSBPD (reordered=${REORD}, content in-order, 0 drops)"
exit 0
