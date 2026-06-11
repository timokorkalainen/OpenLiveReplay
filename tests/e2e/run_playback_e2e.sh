#!/usr/bin/env bash
# End-to-end PLAYBACK test driver — the empirical gate for the windowed
# scheduler rework.
#
# 1. Streams a synthetic A/V source over UDP MPEG-TS with FFmpeg (a black frame
#    flashing white at integer seconds + a 1kHz beep gated to the first 100ms of
#    each second — a visually/audibly checkable timecode; a plain testsrc+sine
#    is fine too, the storm metric only needs valid A/V).
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
# Usage: run_playback_e2e.sh <play_harness_exe> <record_harness_exe> <scenario> <views>
set -uo pipefail

PLAY="${1:?play_harness executable path required}"
RECORD="${2:?record_harness executable path required}"
SCENARIO="${3:-play1x}"
VIEWS="${4:-2}"
PORT="${OLR_PB_PORT:-23470}"

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }

# Record long enough that every scenario's playback window stays inside the
# fixture (play1x plays ~12s from t=0; liveedge/seekplay reach ~mid/late).
SECONDS_TO_RECORD=25

WORKDIR="$(mktemp -d)"
FFPID=""
cleanup() {
    [ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null
    wait "$FFPID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "[pb-e2e] scenario=$SCENARIO views=$VIEWS port=$PORT record=${SECONDS_TO_RECORD}s"

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
        -f mpegts "udp://127.0.0.1:${PORT}?pkt_size=1316" &
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

# --- 2. Record a multi-track fixture -----------------------------------------
URL="udp://127.0.0.1:${PORT}?fifo_size=1000000&overrun_nonfatal=1"
REC_OUT="$(OLR_VIEWS="$VIEWS" "$RECORD" --url "$URL" --name "olr_pb_${SCENARIO}" \
    --outdir "$WORKDIR" --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
REC_RC=$?

# Stop the sender as soon as the fixture is recorded.
[ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null
wait "$FFPID" 2>/dev/null
FFPID=""

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
PLAY_OUT="$("$PLAY" "$FIXTURE" "$SCENARIO" "$VIEWS")"
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
skipForward="$(get skipForward)"
audioPushes="$(get audioPushes)"
framesDropped="$(get framesDropped)"
[ -n "$reposition" ] || reposition="?"
[ -n "$reuseSeek" ] || reuseSeek="?"
[ -n "$reverseChunkSeek" ] || reverseChunkSeek="?"
[ -n "$skipForward" ] || skipForward="?"
[ -n "$audioPushes" ] || audioPushes="?"
[ -n "$framesDropped" ] || framesDropped="?"

if [ $PLAY_RC -ne 0 ]; then
    echo "FAIL: play_harness exited $PLAY_RC"
    exit 1
fi

# --- 4. Assert scenario thresholds -------------------------------------------
fail=0
num() { case "${1:-}" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

case "$SCENARIO" in
    play1x)
        # The headline gate: steady 1x playback must NOT reposition at all, and
        # the audio path must have actually pushed samples.
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
        # One seek to mid, then steady 1x: at most the single reposition for the
        # seek itself (reuse may satisfy it → 0). Never a storm.
        if ! num "$reposition" || [ "$reposition" -gt 1 ]; then
            echo "FAIL: seekplay repositioned too much (reposition=$reposition, expected <=1)"
            fail=1
        fi
        ;;
    stepscrub)
        # 20 paused back-steps should mostly REUSE the window, not full-reposition
        # each time. Allow a small number of cold repositions.
        if ! num "$reposition" || [ "$reposition" -gt 6 ]; then
            echo "FAIL: stepscrub repositioned too much (reposition=$reposition, expected <=6)"
            fail=1
        fi
        ;;
    sliderscrub)
        # In-window scrubs should reuse; only the larger jumps repositon.
        if ! num "$reposition" || [ "$reposition" -gt 6 ]; then
            echo "FAIL: sliderscrub repositioned too much (reposition=$reposition, expected <=6)"
            fail=1
        fi
        ;;
    reverse)
        # Reverse must move via bounded chunk-seeks, not a per-frame storm.
        if ! num "$reposition" || [ "$reposition" -gt 3 ]; then
            echo "FAIL: reverse repositioned too much (reposition=$reposition, expected <=3)"
            fail=1
        fi
        ;;
    liveedge)
        # Playing into EOF must tail-hold, not storm.
        if ! num "$reposition" || [ "$reposition" -gt 1 ]; then
            echo "FAIL: liveedge repositioned (reposition=$reposition, expected <=1) — EOF spin"
            fail=1
        fi
        ;;
    *)
        echo "FAIL: unknown scenario '$SCENARIO'"
        fail=1
        ;;
esac

if [ $fail -ne 0 ]; then
    echo "FAIL: $SCENARIO ($VIEWS views) — reposition=$reposition reuseSeek=$reuseSeek reverseChunkSeek=$reverseChunkSeek skipForward=$skipForward audioPushes=$audioPushes framesDropped=$framesDropped"
    exit 1
fi

echo "PASS: $SCENARIO ($VIEWS views) — reposition=$reposition reuseSeek=$reuseSeek reverseChunkSeek=$reverseChunkSeek skipForward=$skipForward audioPushes=$audioPushes framesDropped=$framesDropped"
exit 0
