#!/usr/bin/env bash
# Native NDI ingest e2e: mid-recording source disconnect/reconnect survival.
#
# Two INDEPENDENT local NDI senders: src0 control (never touched) and src1 victim.
# We record both through sync_harness, KILL the victim sender mid-record, then
# RESTART it with the SAME source name, and prove the engine: (a) observed the drop
# and reconnected (per-source conn_events up->down->up), (b) the control source src0
# is FULLY ISOLATED (no mid-record disconnect, no content gap), and (c) src1 recorded
# REAL content both BEFORE the kill and AFTER the reconnect.
#
# The outage MUST exceed the NDI session's ~8s stall window: a frozen/absent source
# surfaces as Capture::None, which the session-internal stall timer (and the
# StreamWorker watchdog) break after ~8s to trigger reconnect. A shorter outage can
# be masked, so kill@10s / restart@24s (14s outage) / record 44s by default.
#
# Content is asserted via per-track VIDEO PACKET PTS windows (the synthetic NDI
# sender emits a ramp-luma frame + 1kHz tone, with no per-second flash marker, so
# the SRT flash-onset detector does not apply here).
#
# Teeth: OLR_NDI_RECONN_NO_RESTART=1 skips the restart -> src1 never reconnects ->
# no second 'up' and no post-reconnect packets -> FAIL, proving the gate discriminates.
#
# Usage: run_ndi_reconnect.sh <sync_harness> [ndi_runtime_sender]
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
SENDER="${2:-${OLR_NDI_RUNTIME_SENDER:-}}"

KILL_TIME="${OLR_NDI_RECONN_KILL:-10}"
RESTART_TIME="${OLR_NDI_RECONN_RESTART:-24}"
DURATION="${OLR_NDI_RECONN_DURATION:-44}"
LATE_MIN="${OLR_NDI_RECONN_LATE_MIN:-$((RESTART_TIME + 8))}"
DISCOVERY="${OLR_NDI_DISCOVERY_SECS:-4}"
NO_RESTART="${OLR_NDI_RECONN_NO_RESTART:-0}"

PIDS=()
skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }
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
if [ -z "$SENDER" ]; then
    HARNESS_DIR="$(cd "$(dirname "$HARNESS")" && pwd)"
    SENDER="${HARNESS_DIR}/ndi_runtime_sender"
fi
[ -x "$SENDER" ] || skip "ndi_runtime_sender not executable: $SENDER"
WORKDIR="$(mktemp -d)" || fail "could not create temp directory"

STAMP="$$_$(date +%s)"
NAME0="OLR NDI Reconn ctrl ${STAMP}"
NAME1="OLR NDI Reconn victim ${STAMP}"

encode() { SRC="$1" python3 -c 'import os,urllib.parse;print(urllib.parse.quote(os.environ["SRC"],safe=""))'; }
URL0="ndi:$(encode "$NAME0")"
URL1="ndi:$(encode "$NAME1")"

# Launch a sender in the background; echo its PID. $1=name $2=seconds $3=tag
start_sender() {
    "$SENDER" --name "$1" --seconds "$2" --width 640 --height 480 --fps 30 \
        >"${WORKDIR}/sender_${3}.out" 2>"${WORKDIR}/sender_${3}.err" &
    echo "$!"
}

echo "[ndi-reconn] kill@${KILL_TIME}s restart@${RESTART_TIME}s dur=${DURATION}s late_min=${LATE_MIN}s no_restart=${NO_RESTART}"

# 1. Start both senders. Control outlives the whole record; victim is killed/restarted.
CTRL_PID="$(start_sender "$NAME0" "$((DURATION + 10))" ctrl)"; PIDS+=("$CTRL_PID")
VICTIM_PID="$(start_sender "$NAME1" "$((KILL_TIME + 10))" victim)"; PIDS+=("$VICTIM_PID")
sleep "$DISCOVERY"
for pid in "$CTRL_PID" "$VICTIM_PID"; do
    if ! kill -0 "$pid" 2>/dev/null; then
        rc=0; wait "$pid" || rc=$?
        sed -n '1,40p' "${WORKDIR}"/sender_*.err >&2
        [ "$rc" -eq 77 ] && skip "NDI runtime unavailable for local sender"
        fail "an NDI sender exited before discovery (rc=$rc)"
    fi
done

# 2. Record both in the BACKGROUND so the script can kill/restart mid-record.
"$HARNESS" --url "$URL0" --url "$URL1" --outdir "$WORKDIR" --name ndireconn \
    --seconds "$DURATION" --width 640 --height 480 --fps 30 \
    --report-connection-events >"$WORKDIR/out.txt" 2>"$WORKDIR/err.txt" &
HARNESS_PID=$!
PIDS+=("$HARNESS_PID")

# 3. Mid-record: kill the victim sender (its NDI source disappears).
sleep "$KILL_TIME"
echo "[ndi-reconn] killing victim sender (pid $VICTIM_PID) at ~${KILL_TIME}s"
kill "$VICTIM_PID" 2>/dev/null
wait "$VICTIM_PID" 2>/dev/null || true

# 4. Restart the victim with the SAME source name (unless teeth mode).
if [ "$NO_RESTART" = "1" ]; then
    echo "[ndi-reconn] (teeth) NOT restarting the victim sender"
else
    sleep "$(( RESTART_TIME - KILL_TIME ))"
    echo "[ndi-reconn] restarting victim sender at ~${RESTART_TIME}s"
    VICTIM_PID="$(start_sender "$NAME1" "$((DURATION - RESTART_TIME + 10))" victim2)"
    PIDS+=("$VICTIM_PID")
fi

# 5. Wait for the recording to finish; collect outputs.
wait "$HARNESS_PID"
MKV="$(tail -n 1 "$WORKDIR/out.txt")"
echo "[ndi-reconn] out=$MKV"
echo "[ndi-reconn] conn_events:"; grep '^conn_events' "$WORKDIR/err.txt" || echo "  (none)"

[ -n "$MKV" ] && [ -s "$MKV" ] || fail "no output MKV (engine could not record over NDI)"
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | grep -c .)"
[ "${VTRACKS:-0}" = "2" ] || fail "expected 2 video tracks, got ${VTRACKS:-0}"

# Per-track video packet PTS (seconds), sorted. Track index == --url order == src index.
pkt_pts() {
    ffprobe -v error -select_streams "v:$2" -show_entries packet=pts_time -of csv=p=0 "$1" \
        | awk 'NF' | sort -n
}
pkt_pts "$MKV" 0 >"$WORKDIR/p0.txt"
pkt_pts "$MKV" 1 >"$WORKDIR/p1.txt"

fail=0

# --- Assertion A: victim (src=1) up(<kill) -> down(near kill) -> up(after). ---
# 2s tolerance around the kill: the harness connTimer starts at launch (before
# startRecording), so the down may land slightly before the nominal KILL_TIME.
EV1="$(grep '^conn_events src=1' "$WORKDIR/err.txt" | head -1)"
SEQ_OK="$(echo "$EV1" | awk -v kill="$((KILL_TIME*1000))" -v tol=2000 '
    {
        init_up=0; down_t=-1; reconn=0
        for (i=3;i<=NF;i++){
            split($i,a,":"); t=a[1]+0; st=a[2]
            if (st=="up" && down_t<0 && t<(kill-tol)) init_up=1
            else if (st=="down" && init_up && t>(kill-tol) && down_t<0) down_t=t
            else if (st=="up" && down_t>=0 && t>down_t) reconn=1
        }
        print (init_up && down_t>=0 && reconn) ? "1" : "0"
    }')"
if [ "$SEQ_OK" != "1" ]; then
    echo "FAIL: victim src=1 did not show up(before kill)->down(near kill)->reconnect. events: ${EV1:-none}"; fail=1
fi

# --- Assertion B: control (src=0) is isolated — no mid-record down AND continuous. ---
EV0="$(grep '^conn_events src=0' "$WORKDIR/err.txt" | head -1)"
MIDDOWN0="$(echo "$EV0" | awk -v end="$(( (DURATION-3)*1000 ))" '
    { for(i=3;i<=NF;i++){ split($i,a,":"); if(a[2]=="down" && a[1]+0 < end) n++ } print n+0 }')"
if [ "${MIDDOWN0:-0}" -gt 0 ]; then
    echo "FAIL: control src=0 disconnected mid-record (${MIDDOWN0}x) — outage NOT isolated. events: $EV0"; fail=1
fi
# Continuity: packet count near full and no >1.5s gap across the whole record.
read -r N0 GAP0 <<<"$(awk 'NR>1{d=$1-p; if(d>mx)mx=d} {p=$1} END{printf "%d %.3f", NR+0, (mx==""?0:mx)}' "$WORKDIR/p0.txt")"
MIN0=$(( 30 * DURATION / 2 ))  # ~30fps; tolerate half for warmup/jitter
echo "[ndi-reconn] src0 control: packets=$N0 maxgap=${GAP0}s (want >=$MIN0 packets, gap<=1.5s)"
if [ "${N0:-0}" -lt "$MIN0" ]; then
    echo "FAIL: control src0 has only ${N0} video packets (< ${MIN0}) — not recording continuous content"; fail=1
fi
if awk -v g="${GAP0:-9}" 'BEGIN{exit !(g+0 > 1.5)}'; then
    echo "FAIL: control src0 lost content (gap ${GAP0}s > 1.5s) during the victim's outage — cross-source coupling"; fail=1
fi

# --- Assertion C (the real teeth): victim has pre-kill AND post-reconnect content. ---
PRE1="$(awk -v k="$KILL_TIME" '$1<k{n++} END{print n+0}' "$WORKDIR/p1.txt")"
LATE1="$(awk -v m="$LATE_MIN" '$1>m{n++} END{print n+0}' "$WORKDIR/p1.txt")"
echo "[ndi-reconn] victim src1 packets: pre_kill=$PRE1 post_reconnect(>${LATE_MIN}s)=$LATE1"
if [ "${PRE1:-0}" -lt 1 ]; then echo "FAIL: victim src1 had no pre-kill packets (never recorded before the drop)"; fail=1; fi
if [ "${LATE1:-0}" -lt 1 ]; then echo "FAIL: victim src1 had no post-reconnect packets — content did NOT resume after reconnect"; fail=1; fi

[ $fail -ne 0 ] && exit 1
echo "PASS: NDI disconnect/reconnect survival — victim dropped, reconnected, resumed real content; control isolated"
exit 0
