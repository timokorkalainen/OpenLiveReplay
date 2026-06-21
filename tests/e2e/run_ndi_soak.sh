#!/usr/bin/env bash
# Native NDI ingest e2e: long-run STABILITY SOAK.
#
# NOT a drift detector: the local sender and the recording share one wall clock, so
# arrival-timestamped PTS cannot reveal real source drift (that needs two machines,
# out of scope). This is a soak: a long native-NDI record must not crash/hang and
# must keep delivering content the whole time (no mid-run stall / silent freeze).
#
# Content is measured via VIDEO PACKET PTS (the synthetic NDI sender emits a ramp-luma
# frame + 1kHz tone, with no per-second flash marker): assert a near-full packet count,
# a prompt first packet, and no >1.5s inter-packet gap.
#
#   OLR_NDI_SOAK_SECS   soak duration in seconds (default 30)
# Usage: run_ndi_soak.sh <sync_harness> [ndi_runtime_sender]
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
SENDER="${2:-${OLR_NDI_RUNTIME_SENDER:-}}"
SECS="${OLR_NDI_SOAK_SECS:-30}"
DISCOVERY="${OLR_NDI_DISCOVERY_SECS:-4}"

PIDS=()
skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }
is_uint() { case "${1:-}" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }
# shellcheck disable=SC2329 # Invoked by the EXIT trap.
cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    [ -n "${WORKDIR:-}" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

[ -x "$HARNESS" ] || fail "sync_harness not executable: $HARNESS"
command -v ffprobe >/dev/null || skip "ffprobe not found"
command -v python3 >/dev/null || skip "python3 not found"
is_uint "$SECS" && [ "$SECS" -gt 0 ] || fail "OLR_NDI_SOAK_SECS must be a positive integer, got '${SECS}'"
if [ -z "$SENDER" ]; then
    HARNESS_DIR="$(cd "$(dirname "$HARNESS")" && pwd)"
    SENDER="${HARNESS_DIR}/ndi_runtime_sender"
fi
[ -x "$SENDER" ] || skip "ndi_runtime_sender not executable: $SENDER"
WORKDIR="$(mktemp -d)" || fail "could not create temp directory"

NAME="OLR NDI Soak $$ $(date +%s)"
ENC="$(SRC="$NAME" python3 -c 'import os,urllib.parse;print(urllib.parse.quote(os.environ["SRC"],safe=""))')"
URL="ndi:${ENC}"

# Sender outlives the record by a margin so the source never disappears mid-soak.
"$SENDER" --name "$NAME" --seconds "$((SECS + 12))" --width 640 --height 480 --fps 30 \
    >"${WORKDIR}/sender.out" 2>"${WORKDIR}/sender.err" &
PIDS+=("$!")
sleep "$DISCOVERY"
if ! kill -0 "${PIDS[0]}" 2>/dev/null; then
    rc=0; wait "${PIDS[0]}" || rc=$?
    sed -n '1,40p' "${WORKDIR}/sender.err" >&2
    [ "$rc" -eq 77 ] && skip "NDI runtime unavailable for local sender"
    fail "local NDI sender exited before discovery (rc=$rc)"
fi

echo "[ndi-soak] recording one native NDI source for ${SECS}s..."
OUT="$("$HARNESS" --url "$URL" --name ndisoak --outdir "$WORKDIR" \
       --seconds "$SECS" --width 640 --height 480 --fps 30)"
RC=$?
MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[ndi-soak] harness rc=$RC out=$MKV"
[ "$RC" -eq 0 ] && [ -n "$MKV" ] && [ -s "$MKV" ] || fail "no MKV produced (harness crash/hang under the ${SECS}s soak?)"

VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | grep -c .)"
[ "${VTRACKS:-0}" = "1" ] || fail "expected 1 video track, got ${VTRACKS:-0}"

ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$MKV" | awk 'NF' | sort -n >"$WORKDIR/p.txt"
read -r N FIRST GAP <<<"$(awk 'NR==1{first=$1} NR>1{d=$1-p; if(d>mx)mx=d} {p=$1} END{printf "%d %.3f %.3f", NR+0, first+0, (mx==""?0:mx)}' "$WORKDIR/p.txt")"
echo "[ndi-soak] video_packets=$N first=${FIRST}s maxgap=${GAP}s"

fail=0
MIN_PKTS=$(( 30 * SECS / 2 ))  # ~30fps; tolerate half for warmup/jitter
awk -v f="${FIRST:-9}" 'BEGIN{exit !(f+0 < 3.0)}' || { echo "FAIL: first packet at ${FIRST}s (>= 3s) — slow warm-up"; fail=1; }
awk -v n="${N:-0}" -v m="$MIN_PKTS" 'BEGIN{exit !(n+0 >= m)}' || { echo "FAIL: only ${N} video packets over ${SECS}s (< ${MIN_PKTS}) — content not sustained"; fail=1; }
awk -v g="${GAP:-9}" 'BEGIN{exit !(g+0 <= 1.5)}' || { echo "FAIL: max inter-packet gap ${GAP}s > 1.5s — mid-run stall"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: native NDI ingest stable over ${SECS}s soak — ${N} packets, no stall"
exit 0
