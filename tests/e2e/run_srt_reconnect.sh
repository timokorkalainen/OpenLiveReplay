#!/usr/bin/env bash
# Local SRT e2e (Phase 2c-a): mid-recording disconnect/reconnect survival.
#
# Two INDEPENDENT flash sources (separate producers + bridges): src0 control
# (never touched), src1 victim. We record both, KILL src1's bridge mid-record,
# then RESTART it on the same port, and prove the engine: (a) observed the drop
# and reconnected (per-source conn_events sequence up->down->up), (b) resumed
# recording REAL frames for src1 (flashes in a late post-reconnect window), and
# (c) the control source src0 is FULLY ISOLATED — no mid-record disconnect and no
# content loss.
#
# (c) holds on the NATIVE SRT ingest (OLR_NATIVE_SRT=1), where each source owns its
# libsrt socket. The legacy ffmpeg ingest instead COUPLES sources: a dead source's
# avformat reconnect churn monopolizes libsrt's global receive thread and starves
# the others (~2s content loss each), so this gate runs on the native path.
#
# The outage MUST exceed the engine's ~8s stall window: disconnect is detected via
# the stall timeout / 5s rw_timeout, and the SRT recv buffer keeps draining buffered
# packets after the bridge dies, so a short outage is masked (false-pass). Hence
# kill@10s / restart@20s (10s outage) / record 38s by default.
#
# Teeth: OLR_SRT_RECONN_NO_RESTART=1 skips the restart -> src1 never reconnects ->
# no second 'up' and no late flashes -> FAIL, proving the gate discriminates.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX; selects the native
# ingest via OLR_NATIVE_SRT=1 (set by the CTest registration).
# Usage: run_srt_reconnect.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23550}"
KILL_TIME="${OLR_SRT_RECONN_KILL:-10}"
RESTART_TIME="${OLR_SRT_RECONN_RESTART:-20}"
DURATION="${OLR_SRT_RECONN_DURATION:-38}"
LATE_MIN="${OLR_SRT_RECONN_LATE_MIN:-$((RESTART_TIME + 6))}"
NO_RESTART="${OLR_SRT_RECONN_NO_RESTART:-0}"

SRT0=$BASE;       UDP0=$((BASE+1))
SRT1=$((BASE+2)); UDP1=$((BASE+3))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-reconn] base=$BASE kill@${KILL_TIME}s restart@${RESTART_TIME}s dur=${DURATION}s late_min=${LATE_MIN}s no_restart=${NO_RESTART}"

# 1. Two INDEPENDENT flash sources (a dedicated producer per source) + bridges.
flash_marker_to_udps "$UDP0"                       # src0 control producer
srt_bridge "$UDP0" "$SRT0"
flash_marker_to_udps "$UDP1"                       # src1 victim producer
srt_bridge "$UDP1" "$SRT1"; SRC1_BRIDGE_PID=$SRT_LAST_PID
sleep 1.5  # warm-up: both connect before the kill

# 2. Record both in the BACKGROUND so the script can kill/restart mid-record.
"$HARNESS" --url "$(srt_caller_url "$SRT0")" --url "$(srt_caller_url "$SRT1")" \
    --outdir "$WORKDIR" --name srtreconn --seconds "$DURATION" --fps 30 \
    --report-connection-events >"$WORKDIR/out.txt" 2>"$WORKDIR/err.txt" &
HARNESS_PID=$!
PIDS+=("$HARNESS_PID")

# 3. Mid-record: kill src1's bridge (network drop; src1's producer keeps running).
sleep "$KILL_TIME"
echo "[srt-reconn] killing src1 bridge (pid $SRC1_BRIDGE_PID) at ~${KILL_TIME}s"
kill "$SRC1_BRIDGE_PID" 2>/dev/null

# 4. Restart src1's bridge on the SAME port (unless teeth mode).
if [ "$NO_RESTART" = "1" ]; then
    echo "[srt-reconn] (teeth) NOT restarting src1 bridge"
else
    sleep "$(( RESTART_TIME - KILL_TIME ))"
    echo "[srt-reconn] restarting src1 bridge at ~${RESTART_TIME}s"
    # Retry the rebind in case the OS briefly holds the port after the kill.
    for _ in 1 2 3 4 5; do
        srt_bridge "$UDP1" "$SRT1"; SRC1_BRIDGE_PID=$SRT_LAST_PID
        sleep 0.3
        if kill -0 "$SRC1_BRIDGE_PID" 2>/dev/null; then break; fi
    done
fi

# 5. Wait for the recording to finish; collect outputs.
wait "$HARNESS_PID"
MKV="$(tail -n 1 "$WORKDIR/out.txt")"
echo "[srt-reconn] out=$MKV"
echo "[srt-reconn] conn_events:"; grep '^conn_events' "$WORKDIR/err.txt" || echo "  (none)"

if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: no output MKV (engine could not record — built with -DOLR_FFMPEG_SRT_PREFIX?)"; exit 1
fi
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "2" ]; then echo "FAIL: expected 2 video tracks, got ${VTRACKS:-0}"; exit 1; fi

fail=0

# --- Assertion A: src1 sequence up(<kill) -> down(near kill) -> up(after the down). ---
# Allow a 2s tolerance window around the kill: the connTimer in the harness starts
# at launch (before startRecording), and sleep "$KILL_TIME" in this script starts
# right after the harness forks — so the down may arrive a few ms before the
# nominal KILL_TIME boundary. A 2s window catches any realistic timing skew without
# masking a false positive (the outage is 10s, so any accidental early down would
# appear well before kill-2s and not be attributed to the kill).
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
    echo "FAIL: src1 did not show up(before kill)->down(near kill)->reconnect. events: ${EV1:-none}"; fail=1
fi

# --- Assertion B (strict isolation): the control source is UNAFFECTED by src1's
# disconnect/reconnect — no mid-record disconnect AND no content gap. On the native
# SRT ingest each source owns its libsrt socket, so a dead source's reconnect does
# not perturb the others. (The legacy ffmpeg ingest DOES couple sources — a dead
# source's avformat reconnect churn monopolizes libsrt's global receive thread and
# starves the rest; that is why this gate runs on the native path. See SRT_README.)
EV0="$(grep '^conn_events src=0' "$WORKDIR/err.txt" | head -1)"
# A 'down' before the final teardown (~DURATION) means src0 dropped mid-record.
MIDDOWN0="$(echo "$EV0" | awk -v end="$(( (DURATION-3)*1000 ))" '
    { for(i=3;i<=NF;i++){ split($i,a,":"); if(a[2]=="down" && a[1]+0 < end) n++ } print n+0 }')"
if [ "${MIDDOWN0:-0}" -gt 0 ]; then
    echo "FAIL: control src0 disconnected mid-record (${MIDDOWN0}x) — outage NOT isolated. events: $EV0"; fail=1
fi
flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
GAP0="$(awk 'NR>1{d=$1-p; if(d>1.5)g++} {p=$1} END{print g+0}' "$WORKDIR/v0.txt")"
N0="$(wc -l < "$WORKDIR/v0.txt" | tr -d ' ')"
MIN0=$(( DURATION / 2 ))  # ~1 flash/s; a continuous control track has ~DURATION of them
echo "[srt-reconn] src0 control: flashes=$N0 content_gaps>1.5s=$GAP0 (want >=$MIN0 flashes, 0 gaps)"
# Require a near-full flash count so the gap check can't pass vacuously on a
# near-empty track (a 0/1-flash series has no measurable gap).
if [ "${N0:-0}" -lt "$MIN0" ]; then
    echo "FAIL: control src0 has only ${N0} flashes (< ${MIN0}) — not recording continuous content"; fail=1
fi
if [ "${GAP0:-1}" -gt 0 ]; then
    echo "FAIL: control src0 lost content (${GAP0} gap(s) >1.5s) during src1's outage — cross-source coupling"; fail=1
fi

# --- Assertion C (the real teeth): src1 has pre-kill AND post-reconnect content. ---
flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
PRE1="$(awk -v k="$KILL_TIME" '$1<k{n++} END{print n+0}' "$WORKDIR/v1.txt")"
LATE1="$(awk -v m="$LATE_MIN" '$1>m{n++} END{print n+0}' "$WORKDIR/v1.txt")"
echo "[srt-reconn] src1 flashes: pre_kill=$PRE1 post_reconnect(>${LATE_MIN}s)=$LATE1"
if [ "${PRE1:-0}" -lt 1 ]; then echo "FAIL: src1 had no pre-kill flashes (never recorded before the drop)"; fail=1; fi
if [ "${LATE1:-0}" -lt 1 ]; then echo "FAIL: src1 had no post-reconnect flashes — content did NOT resume after reconnect"; fail=1; fi

# --- Diagnostic (non-gating): re-anchor offset src1 vs src0 in the late window. ---
OFF="$(awk -v m="$LATE_MIN" '
    FNR==NR { if($1>m) a[++na]=$1; next }
    { if($1>m) b[++nb]=$1 }
    END {
        if (na<1||nb<1){print "nan"; exit}
        s=0; cnt=0
        for (j=1;j<=nb;j++){ best=1e9; bv=0; for(i=1;i<=na;i++){d=b[j]-a[i]; ad=(d<0?-d:d); if(ad<best){best=ad; bv=d}} s+=bv; cnt++ }
        printf "%.1f", (cnt? s/cnt*1000 : 0)
    }' "$WORKDIR/v0.txt" "$WORKDIR/v1.txt")"
echo "[srt-reconn] reanchor_offset_ms=$OFF (diagnostic; re-anchored to fresh arrival, expected nonzero)"

[ $fail -ne 0 ] && exit 1
echo "PASS: SRT disconnect/reconnect survival — src1 dropped, reconnected, resumed real content; src0 isolated"
exit 0
