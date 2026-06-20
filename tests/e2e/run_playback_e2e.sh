#!/usr/bin/env bash
# End-to-end PLAYBACK test driver — the empirical gate for the windowed
# scheduler rework.
#
# 1. Streams a synthetic A/V source over UDP MPEG-TS with FFmpeg (a black frame
#    flashing white at integer seconds + a 1kHz beep gated to the first 100ms of
#    each second — a visually/audibly checkable timecode; a plain testsrc+sine
#    is fine too, the storm metric only needs valid A/V), then bridges it over
#    UDP->SRT (srt-live-transmit) so the engine records it via its native srt://
#    transport (the only ingest path now — udp:// is no longer accepted).
# 2. Records a multi-track fixture via the headless record_harness (OLR_VIEWS
#    controls the track count).
# 3. Stops the sender, then runs play_harness <fixture> <scenario> <views> and
#    captures the final COUNTERS line the worker prints.
# 4. Asserts the scenario's thresholds. The headline (play1x, 2 views):
#       reposition == 0   (the storm is gone)
#       audioPushes > 0    (the audio path actually ran)
#    Prints PASS/FAIL with the actual counter values; exits non-zero on failure.
#
# Hermetic: the fixture is recorded into a per-run temp dir (--outdir) and the
# whole dir is removed on exit.
#
# Usage: run_playback_e2e.sh <play_harness_exe> <record_harness_exe> <scenario> <views> [srt_port]
set -uo pipefail

PLAY="${1:?play_harness executable path required}"
RECORD="${2:?record_harness executable path required}"
SCENARIO="${3:-play1x}"
VIEWS="${4:-2}"
# Port: explicit 5th arg wins (CTest passes a unique SRT port per case so the
# RUN_SERIAL tests never share a socket), else OLR_PB_PORT, else default. The UDP
# producer port is derived as SRT_PORT+1.
SRT_PORT="${5:-${OLR_PB_PORT:-23470}}"
UDP_PORT=$((SRT_PORT + 1))

# shellcheck source=tests/e2e/srt_lib.sh
. "$(cd "$(dirname "$0")" && pwd)/srt_lib.sh"
srt_require_tools  # SKIP (exit 0) unless ffmpeg/ffprobe/srt-live-transmit present

# Record long enough that every scenario's playback window stays inside the
# fixture (play1x plays ~12s from t=0; liveedge/seekplay reach ~mid/late).
SECONDS_TO_RECORD=25

WORKDIR="$(mktemp -d)"
FFPID=""
BRIDGE_PID=""
cleanup() {
    [ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null
    [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null
    wait "$FFPID" "$BRIDGE_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "[pb-e2e] scenario=$SCENARIO views=$VIEWS srt_port=$SRT_PORT udp_port=$UDP_PORT record=${SECONDS_TO_RECORD}s"

# --- 1. Producer: synthetic flash/beep timecode source -----------------------
# Video: black, flashed white during the first 60ms of each integer second.
# Audio: 1kHz sine gated to the first 100ms of each second (a beep per second).
# If the lavfi expression chain is unsupported, fall back to testsrc2 + sine.
VIDEO_FILTER="color=c=black:s=640x480:r=30,geq=lum='if(lt(mod(T,1),0.06),255,16)':cb=128:cr=128"
AUDIO_FILTER="sine=frequency=1000:sample_rate=48000,volume='if(lt(mod(t,1),0.1),1,0)':eval=frame"

start_producer() {
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "$1" \
        -f lavfi -i "$2" \
        -ac 2 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -c:a aac -b:a 128k \
        -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
    FFPID=$!
}

start_producer "$VIDEO_FILTER" "$AUDIO_FILTER"
sleep 0.5
# If the fancy filter chain died immediately, fall back to a plain source.
if ! kill -0 "$FFPID" 2>/dev/null; then
    echo "[pb-e2e] flash/beep filter unsupported; falling back to testsrc2+sine"
    start_producer "testsrc2=size=640x480:rate=30" \
                   "sine=frequency=1000:sample_rate=48000"
    sleep 0.5
fi

# Bridge the UDP MPEG-TS to an SRT listener so the engine ingests over srt://.
PIDS=()
srt_bridge "$UDP_PORT" "$SRT_PORT"
BRIDGE_PID=$SRT_LAST_PID
sleep 1.0  # let the SRT listener come up before the caller connects

# --- 2. Record a multi-track fixture -----------------------------------------
URL="$(srt_caller_url "$SRT_PORT")"
REC_OUT="$(OLR_VIEWS="$VIEWS" "$RECORD" --url "$URL" --name "olr_pb_${SCENARIO}" \
    --outdir "$WORKDIR" --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
REC_RC=$?

# Stop the sender + SRT bridge as soon as the fixture is recorded.
[ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null
[ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null
wait "$FFPID" "$BRIDGE_PID" 2>/dev/null
FFPID=""
BRIDGE_PID=""

if [ $REC_RC -ne 0 ]; then
    echo "FAIL: record_harness exited $REC_RC"
    exit 1
fi

FIXTURE="$(printf '%s\n' "$REC_OUT" | tail -n 1)"
echo "[pb-e2e] fixture: $FIXTURE"
if [ -z "$FIXTURE" ] || [ ! -s "$FIXTURE" ]; then
    echo "FAIL: no (or empty) fixture at '$FIXTURE'"
    exit 1
fi

# Sanity: the fixture should carry <views> video tracks.
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$FIXTURE" | grep -c .)"
echo "[pb-e2e] fixture video tracks: ${VTRACKS:-?} (expected $VIEWS)"

# --- 3. Drive the real PlaybackWorker ----------------------------------------
PH_SCENARIO="$SCENARIO"
if [ "$SCENARIO" = "latency" ]; then
    export OLR_AUDIO_LATENCY_MS="${OLR_AUDIO_LATENCY_MS:-300}"
    PH_SCENARIO="play1x"
fi
PLAY_OUT="$("$PLAY" "$FIXTURE" "$PH_SCENARIO" "$VIEWS")"
PLAY_RC=$?
echo "[pb-e2e] play_harness rc=$PLAY_RC"

COUNTERS="$(printf '%s\n' "$PLAY_OUT" | grep '^COUNTERS ' | tail -n 1)"
if [ -z "$COUNTERS" ]; then
    echo "FAIL: play_harness produced no COUNTERS line"
    echo "----- play_harness stdout -----"
    printf '%s\n' "$PLAY_OUT"
    exit 1
fi
echo "[pb-e2e] $COUNTERS"

# Parse "COUNTERS key=val key=val ..." into individual shell vars. Avoid bash-4
# associative arrays: macOS ships bash 3.2, where `declare -A` does not exist.
get() { printf '%s\n' "$COUNTERS" | sed -n "s/.*[[:space:]]$1=\([0-9]*\).*/\1/p"; }
reposition="$(get reposition)"
reuseSeek="$(get reuseSeek)"
reverseChunkSeek="$(get reverseChunkSeek)"
eofTailSeek="$(get eofTailSeek)"
skipForward="$(get skipForward)"
audioPushes="$(get audioPushes)"
framesDropped="$(get framesDropped)"
resyncCount="$(get resyncCount)"
placeholderFramesDelta="$(get placeholderFramesDelta)"
skippedDuplicateFrames="$(get skippedDuplicateFrames)"
cacheGeneration="$(get cacheGeneration)"
heldFramesDelta="$(get heldFramesDelta)"
maxClockDivergenceMs="$(get maxClockDivergenceMs)"
cutsFired="$(get cutsFired)"
cutFollowReposition="$(get cutFollowReposition)"
[ -n "$reposition" ] || reposition="?"
[ -n "$reuseSeek" ] || reuseSeek="?"
[ -n "$reverseChunkSeek" ] || reverseChunkSeek="?"
[ -n "$eofTailSeek" ] || eofTailSeek="?"
[ -n "$skipForward" ] || skipForward="?"
[ -n "$audioPushes" ] || audioPushes="?"
[ -n "$framesDropped" ] || framesDropped="?"
[ -n "$resyncCount" ] || resyncCount="?"
[ -n "$placeholderFramesDelta" ] || placeholderFramesDelta="?"
[ -n "$skippedDuplicateFrames" ] || skippedDuplicateFrames="?"
[ -n "$cacheGeneration" ] || cacheGeneration="?"
[ -n "$heldFramesDelta" ] || heldFramesDelta="?"
[ -n "$maxClockDivergenceMs" ] || maxClockDivergenceMs="?"
[ -n "$cutsFired" ] || cutsFired="?"
[ -n "$cutFollowReposition" ] || cutFollowReposition="?"

if [ $PLAY_RC -ne 0 ]; then
    echo "FAIL: play_harness exited $PLAY_RC"
    exit 1
fi

# --- 4. Assert scenario thresholds -------------------------------------------
fail=0
num() { case "${1:-}" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

# Each branch's thresholds carry headroom over the measured 2-view numbers
# (recorded in run_playback_e2e.sh's header / the Task 8 prompt) but still trip
# on a return of the seek storm. Do NOT weaken these to make a run pass.
case "$SCENARIO" in
    play1x)
        # The headline gate (spec §11.6): steady 1x playback must NOT reposition
        # at all, and the audio path must have actually pushed samples.
        # Measured: reposition=0 audioPushes=343. Exact 0 — the storm is a hard
        # regression. Same gate for the 4-view multi-track decode path.
        if ! num "$reposition" || [ "$reposition" -ne 0 ]; then
            echo "FAIL: play1x repositioned (reposition=$reposition, expected 0) — scheduler is still stormy"
            fail=1
        fi
        if ! num "$audioPushes" || [ "$audioPushes" -le 0 ]; then
            echo "FAIL: audio path did not run (audioPushes=$audioPushes, expected >0)"
            fail=1
        fi
        ;;
    seekplay)
        # One seek to mid, then steady 1x. Measured: reposition=1 audioPushes=312.
        # At most the single reposition for the seek (reuse may satisfy it → 0);
        # must NOT re-storm, and the audio path must run after the seek.
        if ! num "$reposition" || [ "$reposition" -gt 2 ]; then
            echo "FAIL: seekplay repositioned too much (reposition=$reposition, expected <=2) — post-seek storm"
            fail=1
        fi
        if ! num "$audioPushes" || [ "$audioPushes" -le 0 ]; then
            echo "FAIL: seekplay audio path did not run (audioPushes=$audioPushes, expected >0)"
            fail=1
        fi
        ;;
    stepscrub)
        # 20 paused back-steps. Measured: reposition=6 reuseSeek=17. Most steps
        # must REUSE the window (old code = 20 hard seeks); allow a few cold
        # repositions, but the bulk must be reuse-seeks.
        if ! num "$reposition" || [ "$reposition" -gt 10 ]; then
            echo "FAIL: stepscrub repositioned too much (reposition=$reposition, expected <=10) — seek-per-step"
            fail=1
        fi
        if ! num "$reuseSeek" || [ "$reuseSeek" -lt 1 ]; then
            echo "FAIL: stepscrub did not reuse the window (reuseSeek=$reuseSeek, expected >=1)"
            fail=1
        fi
        ;;
    sliderscrub)
        # A bounded series of in-/out-of-window scrubs. Measured: reposition=12.
        # In-window scrubs reuse; only the larger jumps reposition — so the count
        # stays bounded (no per-event storm). No crash (PLAY_RC checked above).
        if ! num "$reposition" || [ "$reposition" -gt 20 ]; then
            echo "FAIL: sliderscrub repositioned too much (reposition=$reposition, expected <=20) — per-scrub storm"
            fail=1
        fi
        ;;
    reverse)
        # Reverse moves via bounded chunk-seeks, not a per-frame storm.
        #
        # The primary anti-thrash gate here is reposition: reverse must not fall
        # back to repeated full repositions. It is normally 1, but the
        # direction-flip transient on a loaded/slow CI runner can add a few more
        # (observed reposition=4 on busy hosts while reverseChunkSeek held at the
        # intended ~17). Bound it at 4 — a genuine reverse-thrash regression
        # produces many more — so the gate stays meaningful without flaking.
        #
        # reverseChunkSeek is a SECONDARY, load-variant bound. By design each
        # chunk-seek fetches kChunkMs(=500ms) of reverse travel, so the count
        # scales with the reverse distance AND the worker loop's wake cadence:
        # measured 32–86 across runs on this 2-view fixture (idle host ~32–35,
        # busy host ~75–86; the §11.2 1.5×|speed|×1000/kChunkMs steady-state
        # figure understates the direction-flip transient + the loop's top-up
        # rate under load). A bound of 60 (the naive steady-state ceiling) flakes
        # ~half the time, so it is NOT a usable gate. We bound it at 150 instead:
        # comfortably above the observed busy-host band (~86) yet far below a
        # per-frame reverse storm (~30fps×5×4s ≈ 600+), so a genuine thrash
        # regression still trips it without false failures. See report/findings.
        if ! num "$reverseChunkSeek" || [ "$reverseChunkSeek" -gt 150 ]; then
            echo "FAIL: reverse chunk-seek storm (reverseChunkSeek=$reverseChunkSeek, expected <=150) — per-frame reverse thrash"
            fail=1
        fi
        if ! num "$reposition" || [ "$reposition" -gt 4 ]; then
            echo "FAIL: reverse repositioned too much (reposition=$reposition, expected <=4) — reverse thrash"
            fail=1
        fi
        ;;
    liveedge)
        # Play into EOF must tail-hold, not spin. Measured: eofTailSeek=1
        # reposition=1. A bounded eofTailSeek (counted separately from
        # repositions) proves no EOF loop.
        if ! num "$eofTailSeek" || [ "$eofTailSeek" -gt 3 ]; then
            echo "FAIL: liveedge EOF spin (eofTailSeek=$eofTailSeek, expected <=3)"
            fail=1
        fi
        if ! num "$reposition" || [ "$reposition" -gt 3 ]; then
            echo "FAIL: liveedge repositioned too much (reposition=$reposition, expected <=3)"
            fail=1
        fi
        ;;
    latency)
        # 1x playback with a 300 ms output-latency offset must NOT storm re-aligns:
        # the resync threshold scales with the offset (kResyncHeadroomMs + offset),
        # so the steady offset divergence is tolerated. resyncCount must stay 0.
        # (reposition not re-asserted here — PH_SCENARIO=play1x, and e2e_play_storm owns that gate)
        if ! num "$audioPushes" || [ "$audioPushes" -le 0 ]; then
            echo "FAIL: latency produced no audio (audioPushes=$audioPushes) — audio path dead"
            fail=1
        fi
        if ! num "$resyncCount" || [ "$resyncCount" -ne 0 ]; then
            echo "FAIL: latency re-align storm (resyncCount=$resyncCount, expected 0) — resync threshold not scaled with offset"
            fail=1
        fi
        ;;
    seekflash)
        # Prove a seek introduces NO NEW gray placeholder. After a warmup play
        # fills the output cache, a FORWARD test seek must not paint the gray
        # placeholder: Task 1 keeps the output cache across the reposition so
        # videoFrameAt returns the largest pts<=target (a stale-but-real frame)
        # until the new frames decode in. The metric is the POST-SEEK delta
        # (a cold start legitimately placeholders during warmup, so the
        # whole-run total is never 0); a non-zero delta is the seek flash.
        # Task 4 (deliver-target-first) lowers time-to-first-paint; the
        # placeholderFramesDelta==0 gate is the regression guard.
        if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
            echo "FAIL: seekflash painted gray after seek (placeholderFramesDelta=$placeholderFramesDelta, expected 0) — seek flash"
            fail=1
        fi
        if ! num "$reposition" || [ "$reposition" -gt 2 ]; then
            echo "FAIL: seekflash repositioned too much (reposition=$reposition, expected <=2)"
            fail=1
        fi
        # FRAME-ACCURACY gate (the same epoch-divergence guard armedcut asserts):
        # placeholderFramesDelta==0 proves no gray was painted, but NOT that the
        # frame shown is the seek target — a reposition that re-bases the playhead
        # without resetPlayEpoch() leaves sampledPlayheadMs (drives the cache
        # lookup) diverged by the seek distance and renders a stale-but-real frame
        # with no gray/reposition flag. That regression measured ~24000ms on the
        # far-back path before PR #93 reset the epoch at both repositionTo commit
        # sites; healthy runs sit at ~15ms (one frame). Bound at 1500 (clear of
        # sub-frame jitter + the seek-settle transient, far below the regression).
        if ! num "$maxClockDivergenceMs" || [ "$maxClockDivergenceMs" -gt 1500 ]; then
            echo "FAIL: seekflash clock diverged (maxClockDivergenceMs=$maxClockDivergenceMs, expected <=1500) — output rendered the wrong frame (play epoch not re-anchored after the seek)"
            fail=1
        fi
        ;;
    farback)
        # Far-backward seek (near EOF -> 0): the worst case for the seek flash.
        # The committed-playhead gate (Task 1) + worker double-buffer (Task 2)
        # must keep the flash gate clean — NO new placeholder across the jump
        # (placeholderFramesDelta==0) — with a bounded reposition count and at
        # least one committed cache generation (cacheGeneration>=1 proves the
        # reposition decoded and committed a frame at its target).
        #
        # reposition is a SECONDARY bound here. Unlike seekplay's single mid-seek,
        # farback structurally repositions a handful of times: the warmup seek to
        # near-EOF, the EOF tail-hold, the far-backward seek to 0, and a settling
        # reposition as playback resumes from 0 (measured 3-4 across runs). Bound
        # it at 6 — clear headroom over the observed band yet far below a seek
        # storm (the old loop produced dozens) — so a genuine regression still
        # trips without flaking. The flash gate (placeholderFramesDelta) is the
        # primary assertion.
        if ! num "$reposition" || [ "$reposition" -gt 6 ]; then
            echo "FAIL: farback repositioned too much (reposition=$reposition, expected <=6) — seek storm"
            fail=1
        fi
        if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
            echo "FAIL: farback flashed a placeholder (placeholderFramesDelta=$placeholderFramesDelta, expected 0) — seek flash regressed"
            fail=1
        fi
        if ! num "$cacheGeneration" || [ "$cacheGeneration" -lt 1 ]; then
            echo "FAIL: farback never committed a cache generation (cacheGeneration=$cacheGeneration, expected >=1)"
            fail=1
        fi
        # SECONDARY anti-masking bound (Task 2): the worker double-buffer keeps the
        # OLD frames published throughout the far-back fill, so the dispatcher
        # hold-last only fires for a brief transient at the playhead flip (the live
        # playhead momentarily sits just below the first decoded frame) — measured
        # ~6 held ticks. A masking regression (publishing the half-built staging
        # cache for the whole multi-reposition fill) drives this into the dozens.
        # Bound at 20: clear of the transient + host variance, far below a masking
        # regression. The primary proof that no gray was shown is placeholderFrames-
        # Delta==0 above; baseHeld (stderr) being ~0 shows the warmup reposition did
        # not mask either.
        if ! num "$heldFramesDelta" || [ "$heldFramesDelta" -gt 20 ]; then
            echo "FAIL: farback held frames across the jump (heldFramesDelta=$heldFramesDelta, expected <=20) — double-buffer not keeping old frames published (Tier-1 masking)"
            fail=1
        fi
        # FRAME-ACCURACY gate — far-back is the worst case for epoch divergence:
        # this exact path measured ~24000ms of clock divergence before PR #93
        # added resetPlayEpoch() after the CommitGate-held reposition, rendering a
        # frame ~24s stale with placeholderFramesDelta==0 (no gray) and a bounded
        # reposition count, so every gate above passed despite the wrong frame.
        # Healthy runs sit at ~14ms. Bound at 1500 (same as armedcut/seekflash).
        if ! num "$maxClockDivergenceMs" || [ "$maxClockDivergenceMs" -gt 1500 ]; then
            echo "FAIL: farback clock diverged (maxClockDivergenceMs=$maxClockDivergenceMs, expected <=1500) — output rendered the wrong frame (play epoch not re-anchored after the far-back seek)"
            fail=1
        fi
        ;;
    armedcut)
        # Tier3 ARMED CUT (Tasks 9/10/12): a recalled cut must snap to the target
        # frame with NO gray flash and NO reposition fallback. The worker pre-rolls
        # the target window into a private staging cache on its SECOND
        # AVFormatContext and atomically promotes it at a scheduled output frame —
        # the playhead re-bases WITHOUT invoking repositionTo. Two hard gates:
        #   placeholderFramesDelta==0  -> no gray painted across the cut
        #   reposition==0              -> the cut fired (did NOT fall back to the
        #                                 coarse repositionTo path)
        # A non-zero reposition means the cut did not promote in time and the
        # normal seek path serviced the jump — debug the schedule, do NOT loosen.
        if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
            echo "FAIL: armedcut painted gray across the cut (placeholderFramesDelta=$placeholderFramesDelta, expected 0) — cut flash"
            fail=1
        fi
        if ! num "$reposition" || [ "$reposition" -ne 0 ]; then
            echo "FAIL: armedcut fell back to repositionTo (reposition=$reposition, expected 0) — cut did not fire in time"
            fail=1
        fi
        # FRAME-ACCURACY gate (the one the placeholder/reposition gates miss): the
        # output clock must re-anchor to the target at the cut. If the play epoch
        # is not reset, sampledPlayheadMs diverges from the target by the cut
        # distance and the output renders the WRONG frame with no gray/reposition
        # reported. maxClockDivergenceMs was ~11000 with the bug; <500 after the
        # fix. Bound at 1500 (well clear of normal sub-frame jitter + the seek-
        # settle transient, far below a divergence regression).
        if ! num "$maxClockDivergenceMs" || [ "$maxClockDivergenceMs" -gt 1500 ]; then
            echo "FAIL: armedcut clock diverged (maxClockDivergenceMs=$maxClockDivergenceMs, expected <=1500) — output rendered the wrong frame (play epoch not re-anchored at the cut)"
            fail=1
        fi
        # SAFE RE-ARM QUEUE gate: the scenario fires a first cut (to durMs/2) and,
        # while it is in flight, issues a SECOND Recall to a different (forward)
        # target (durMs*3/4). The safe re-arm queue must NOT drop or unsafely apply
        # that re-arm — it queues the latest target and the worker fires it once the
        # first cut clears, so exactly TWO cuts fire. cutsFired<2 means the queued
        # re-arm was dropped (the old drop-while-pending behavior); cutsFired>2
        # would mean spurious extra cuts. Both fired cuts must also clear the
        # placeholder/reposition/divergence gates above (the queued second cut is
        # held to the same frame-accuracy bar as the first).
        if ! num "$cutsFired" || [ "$cutsFired" -ne 2 ]; then
            echo "FAIL: armedcut re-arm queue fired $cutsFired cuts (expected 2) — a Recall during an in-flight cut was dropped or duplicated (safe re-arm queue regressed)"
            fail=1
        fi
        # FORWARD cuts must NOT trigger the armed-cut decoder-follow: the primary
        # bank is BEHIND the new playhead, so the forward-lag skip-forward path
        # resyncs it with no reposition. A non-zero cutFollowReposition here means
        # the follow fired for a forward cut (wrong direction guard).
        if ! num "$cutFollowReposition" || [ "$cutFollowReposition" -ne 0 ]; then
            echo "FAIL: armedcut (forward) fired the decoder-follow (cutFollowReposition=$cutFollowReposition, expected 0) — follow should only arm for backward cuts"
            fail=1
        fi
        ;;
    armedcut-back)
        # Tier3 ARMED CUT with a BACKWARD target (the dominant EVS replay case),
        # played LONG past the staging span. The cut swaps only the output cache +
        # re-bases the playhead, leaving the primary decoder bank forward, so the
        # worker resyncs it with a reactive (non-clearing) backward repositionTo —
        # a benign decoder resync, NOT a coarse-seek fallback. Gates:
        #   placeholderFramesDelta==0  -> no gray painted across the cut
        #   heldFramesDelta==0         -> NO frozen-frame stall. THIS is the gate
        #     the forward armedcut/placeholder gates miss: if the resync ever lags
        #     and the playhead outruns the ~800ms promoted coverage, the cache runs
        #     dry and the dispatcher's hold-last paints the last-good frame +
        #     bumps heldFrames (a frozen picture), which placeholderFramesDelta
        #     never sees. Verified clean across 38 runs incl. 6-core load + a 6.4s
        #     long-play; this gate locks that in.
        #   framesDropped==0           -> the resync never drops a frame
        #   maxClockDivergenceMs<=1500 -> frame accuracy (epoch re-anchored at cut)
        # NOT reposition==0: a backward cut legitimately repositions to resync the
        # primary bank (measured 3 per cut). Bound it loosely (<=8) purely as an
        # anti-runaway guard so a genuine seek storm still trips.
        if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
            echo "FAIL: armedcut-back painted gray across the backward cut (placeholderFramesDelta=$placeholderFramesDelta, expected 0) — cut flash"
            fail=1
        fi
        # heldFramesDelta is the frozen-frame detector: if the cache runs dry the
        # dispatcher paints the last-good frame (heldFrames++) instead of gray, so
        # the placeholder gate misses it. A REAL dry-cache stall (the playhead
        # outrunning the ~800ms promoted coverage before the resync extends it)
        # would be HUNDREDS-to-thousands of held ticks (~1ms/tick, seconds of
        # freeze). The small bound tolerates only the brief cut-flip transient (the
        # re-based playhead momentarily a tick or two below the first promoted
        # frame): observed 0-2, bounded at 20 (same as farback) for host-load
        # variance — orders of magnitude below a real stall, so it never flakes yet
        # still trips on a genuine freeze.
        if ! num "$heldFramesDelta" || [ "$heldFramesDelta" -gt 20 ]; then
            echo "FAIL: armedcut-back froze (heldFramesDelta=$heldFramesDelta, expected <=20) — output cache ran dry before the decoder resync extended coverage (frozen frame, invisible to the placeholder gate)"
            fail=1
        fi
        if ! num "$framesDropped" || [ "$framesDropped" -ne 0 ]; then
            echo "FAIL: armedcut-back dropped frames (framesDropped=$framesDropped, expected 0) across the backward cut/resync"
            fail=1
        fi
        if ! num "$maxClockDivergenceMs" || [ "$maxClockDivergenceMs" -gt 1500 ]; then
            echo "FAIL: armedcut-back clock diverged (maxClockDivergenceMs=$maxClockDivergenceMs, expected <=1500) — output rendered the wrong frame (play epoch not re-anchored at the backward cut)"
            fail=1
        fi
        # DECODER-FOLLOW gate: a backward cut must resync the primary bank via the
        # deterministic decoder-follow (cutFollowReposition==1), NOT the reactive
        # backward-jump path. With the codec flush draining stale post-seek frames,
        # the resync is a SINGLE reposition, so the only entry on the reposition
        # counter is the scenario's own warmup seekTo (1). Before this work a
        # backward cut tripped the reactive path 2-3 times (reposition ~4) AND the
        # follow did not exist; now reposition==1 (warmup) + cutFollowReposition==1.
        if ! num "$cutFollowReposition" || [ "$cutFollowReposition" -ne 1 ]; then
            echo "FAIL: armedcut-back decoder-follow did not fire exactly once (cutFollowReposition=$cutFollowReposition, expected 1) — backward cut should resync the primary bank via the proactive follow"
            fail=1
        fi
        if ! num "$reposition" || [ "$reposition" -gt 2 ]; then
            echo "FAIL: armedcut-back repositioned too much (reposition=$reposition, expected <=2 = warmup only) — reactive backward-jump storm (codec not flushed) or coarse-seek fallback"
            fail=1
        fi
        ;;
    armedcut-seekrace)
        # Manual-seek-vs-in-flight-cut policy: a cut is armed, then an operator
        # seekTo to a different target is issued before the cut fires. The seek must
        # WIN — the seek bumps m_seekGeneration, so maybeFireScheduledCut aborts the
        # cut (it never snaps to a target the operator moved away from).
        #   cutsFired==0   -> the cut was cancelled by the manual seek
        #   reposition>=1  -> the manual seek serviced the jump via repositionTo
        #   placeholderFramesDelta==0 -> the seek (Tier1/2) painted no gray
        # A regression (cut fires despite the seek) shows cutsFired>=1 — the output
        # would have jumped to the cut target instead of the operator's seek target.
        if ! num "$cutsFired" || [ "$cutsFired" -ne 0 ]; then
            echo "FAIL: armedcut-seekrace cut fired despite a manual seek (cutsFired=$cutsFired, expected 0) — manual seek must cancel an in-flight cut"
            fail=1
        fi
        if ! num "$reposition" || [ "$reposition" -lt 1 ]; then
            echo "FAIL: armedcut-seekrace manual seek did not reposition (reposition=$reposition, expected >=1) — the seek should service the jump"
            fail=1
        fi
        if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
            echo "FAIL: armedcut-seekrace painted gray (placeholderFramesDelta=$placeholderFramesDelta, expected 0) — seek flash"
            fail=1
        fi
        # FRAME-ACCURACY: prove the SEEK won — the output clock must re-anchor to the
        # seek target. cutsFired==0 alone only proves the cut was suppressed, not that
        # the output landed at the seek target (vs the abandoned cut target). The
        # divergence gate (same bound the other armed-cut gates use) closes that
        # false-pass: a cut that silently won, or a seek that mis-positioned, diverges.
        if ! num "$maxClockDivergenceMs" || [ "$maxClockDivergenceMs" -gt 1500 ]; then
            echo "FAIL: armedcut-seekrace clock diverged (maxClockDivergenceMs=$maxClockDivergenceMs, expected <=1500) — the seek did not cleanly service the jump (or the cut fired)"
            fail=1
        fi
        ;;
    armedcut-rearm-seek)
        # COMBINED recall+seek interleaving (the path the race review flagged as
        # uncovered): arm a cut, queue a re-arm (double Recall) while it is in flight,
        # THEN issue a manual seek. The manual seek is the newest explicit action, so
        # BOTH the in-flight cut and the queued re-arm must be cancelled — no cut fires.
        #   cutsFired==0   -> in-flight cut aborted (seek-gen mismatch) AND queued
        #                     re-arm dropped (queue-time gen superseded by the seek)
        #   placeholderFramesDelta==0 + maxClockDivergenceMs<=1500 -> the seek won,
        #                     frame-accurately, with no flash
        if ! num "$cutsFired" || [ "$cutsFired" -ne 0 ]; then
            echo "FAIL: armedcut-rearm-seek fired a cut (cutsFired=$cutsFired, expected 0) — a queued re-arm or in-flight cut survived a later manual seek"
            fail=1
        fi
        if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
            echo "FAIL: armedcut-rearm-seek painted gray (placeholderFramesDelta=$placeholderFramesDelta, expected 0)"
            fail=1
        fi
        if ! num "$maxClockDivergenceMs" || [ "$maxClockDivergenceMs" -gt 1500 ]; then
            echo "FAIL: armedcut-rearm-seek clock diverged (maxClockDivergenceMs=$maxClockDivergenceMs, expected <=1500) — the seek did not cleanly win over the recall+re-arm"
            fail=1
        fi
        ;;
    *)
        echo "FAIL: unknown scenario '$SCENARIO'"
        fail=1
        ;;
esac

SUMMARY="reposition=$reposition reuseSeek=$reuseSeek reverseChunkSeek=$reverseChunkSeek eofTailSeek=$eofTailSeek skipForward=$skipForward audioPushes=$audioPushes framesDropped=$framesDropped resyncCount=$resyncCount placeholderFramesDelta=$placeholderFramesDelta skippedDuplicateFrames=$skippedDuplicateFrames cacheGeneration=$cacheGeneration heldFramesDelta=$heldFramesDelta maxClockDivergenceMs=$maxClockDivergenceMs cutsFired=$cutsFired cutFollowReposition=$cutFollowReposition"

if [ $fail -ne 0 ]; then
    echo "FAIL: $SCENARIO ($VIEWS views) — $SUMMARY"
    exit 1
fi

echo "PASS: $SCENARIO ($VIEWS views) — $SUMMARY"
exit 0
