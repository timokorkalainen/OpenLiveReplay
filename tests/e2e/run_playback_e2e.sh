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
[ -n "$reposition" ] || reposition="?"
[ -n "$reuseSeek" ] || reuseSeek="?"
[ -n "$reverseChunkSeek" ] || reverseChunkSeek="?"
[ -n "$eofTailSeek" ] || eofTailSeek="?"
[ -n "$skipForward" ] || skipForward="?"
[ -n "$audioPushes" ] || audioPushes="?"
[ -n "$framesDropped" ] || framesDropped="?"
[ -n "$resyncCount" ] || resyncCount="?"
[ -n "$placeholderFramesDelta" ] || placeholderFramesDelta="?"

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
        ;;
    *)
        echo "FAIL: unknown scenario '$SCENARIO'"
        fail=1
        ;;
esac

SUMMARY="reposition=$reposition reuseSeek=$reuseSeek reverseChunkSeek=$reverseChunkSeek eofTailSeek=$eofTailSeek skipForward=$skipForward audioPushes=$audioPushes framesDropped=$framesDropped resyncCount=$resyncCount placeholderFramesDelta=$placeholderFramesDelta"

if [ $fail -ne 0 ]; then
    echo "FAIL: $SCENARIO ($VIEWS views) — $SUMMARY"
    exit 1
fi

echo "PASS: $SCENARIO ($VIEWS views) — $SUMMARY"
exit 0
