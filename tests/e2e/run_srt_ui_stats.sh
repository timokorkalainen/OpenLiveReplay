#!/usr/bin/env bash
# Local SRT e2e (JIT-5): the SRT loss-telemetry DATA PATH that feeds the
# connection-status UI. The native ingest samples srt_bstats and pushes it via
# reportStats -> StreamWorker::statsUpdated -> ReplayManager::sourceStatsUpdated;
# sync_harness --report-stats prints whatever reaches that RM signal. We drive one
# source through a lossy_udp_relay.py and assert the harness's reported per-source
# stats carry REAL telemetry: retrans>0 under loss, drop==0 (recovered), recv>0;
# and a clean control run shows recv>0 with ~no retransmits. This proves the exact
# numbers the UI dot/tooltip will read, without needing QML.
#
# Records over the native SRT ingest (the default, and only, SRT ingest); no
# SRT-enabled ffmpeg build is needed.
# Usage: run_srt_ui_stats.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23700}"
SECS="${OLR_SRT_UISTATS_SECS:-10}"
SEED="${OLR_SRT_UISTATS_SEED:-1234}"
LOSS_MOD="${OLR_SRT_UISTATS_LOSS:-12}"
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

# Record one source through the relay at $1=loss% $2=tag $3=port_base, with
# --report-stats. The harness writes its "stats src=0 ..." line to $tag.err, which
# the caller reads via hstat() after this returns.
measure() {
    local loss="$1" tag="$2" pb="$3"
    local S=$pb UDP=$((pb+1)) R=$((pb+2)) pp brp rp hp
    flash_marker_to_udps "$UDP"; pp=$SRT_LAST_PID
    srt_bridge "$UDP" "$S"; brp=$SRT_LAST_PID
    python3 "$RELAY" "$R" "127.0.0.1:$S" "$loss" "$WORKDIR/$tag.stats" "$SEED" &
    rp=$!; PIDS+=("$rp")
    sleep 1.5
    "$HARNESS" --url "srt://127.0.0.1:$R?transtype=live" \
        --outdir "$WORKDIR" --name "uistats_$tag" --seconds "$SECS" --fps 30 \
        --report-stats >"$WORKDIR/$tag.out" 2>"$WORKDIR/$tag.err" &
    hp=$!; PIDS+=("$hp")
    wait "$hp"
    kill -TERM "$rp" 2>/dev/null; sleep 0.5; kill -9 "$rp" 2>/dev/null
    kill "$pp" "$brp" 2>/dev/null; wait "$pp" "$brp" 2>/dev/null
}

# field from the harness "stats src=0 recv=.. retrans=.. loss=.. drop=.." line
hstat() {  # $1=err file  $2=field
    grep '^stats src=0 ' "$1" | tail -1 | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2; exit}'
}
relay_dropped() { awk -F'[= ]' '/dropped=/{print $2; exit}' "$1" 2>/dev/null; }

echo "[srt-uistats] base=$BASE secs=$SECS seed=$SEED loss=${LOSS_MOD}%"
measure 0           clean "$BASE"
measure "$LOSS_MOD" lossy "$((BASE+4))"

RECV0="$(hstat "$WORKDIR/clean.err" recv)"; RETR0="$(hstat "$WORKDIR/clean.err" retrans)"
RECV1="$(hstat "$WORKDIR/lossy.err" recv)"; RETR1="$(hstat "$WORKDIR/lossy.err" retrans)"
DROP1="$(hstat "$WORKDIR/lossy.err" drop)"; RDROP1="$(relay_dropped "$WORKDIR/lossy.stats")"
echo "[srt-uistats] clean: recv=${RECV0:-?} retrans=${RETR0:-?}"
echo "[srt-uistats] lossy(${LOSS_MOD}%): recv=${RECV1:-?} retrans=${RETR1:-?} drop=${DROP1:-?} relay_dropped=${RDROP1:-?}"

fail=0
# The data path delivered stats at all (harness saw a "stats src=0" line w/ recv).
[ -n "${RECV0:-}" ] || { echo "FAIL: clean run reported no stats line — data path dead"; fail=1; }
[ -n "${RECV1:-}" ] || { echo "FAIL: lossy run reported no stats line — data path dead"; fail=1; }
awk -v r="${RECV1:-0}" 'BEGIN{exit !(r+0 > 0)}'  || { echo "FAIL: lossy recv=${RECV1:-?} not > 0"; fail=1; }
# Relay actually injected loss, and the UI data path carried the retransmit signal.
awk -v d="${RDROP1:-0}" 'BEGIN{exit !(d+0 >= 20)}' || { echo "FAIL: relay dropped only ${RDROP1:-0} at ${LOSS_MOD}% — loss not injected"; fail=1; }
awk -v r="${RETR1:-0}"  'BEGIN{exit !(r+0 > 0)}'   || { echo "FAIL: lossy retrans=${RETR1:-?} — UI data path did not carry ARQ retransmits"; fail=1; }
# Recovered: nothing finally unrecovered (this is what keeps the dot out of red).
awk -v p="${DROP1:-999}" 'BEGIN{exit !(p+0 == 0)}' || { echo "FAIL: lossy drop=${DROP1:-?} — expected full recovery (0) at ${LOSS_MOD}%"; fail=1; }
# Clean control: the same path reports ~no retransmits (discriminates healthy vs
# stress). Tolerant of a stray loopback hiccup; the lossy retrans>0 is the real proof.
awk -v r="${RETR0:-0}" 'BEGIN{exit !(r+0 <= 2)}'   || { echo "FAIL: clean retrans=${RETR0:-?} — expected ~0 on a clean loopback link"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: SRT stats data path delivers real telemetry to the UI signal (lossy retrans=${RETR1}, drop=${DROP1}; clean retrans=${RETR0})"
exit 0
