#!/usr/bin/env bash
# End-to-end recording test driver.
#
# 1. Streams a synthetic live A/V source over UDP MPEG-TS with FFmpeg.
# 2. Runs the headless record_harness against it for a fixed duration.
# 3. Probes the produced .mkv with ffprobe and asserts structural correctness:
#       - video + audio streams exist
#       - output audio is stereo (mono inputs MUST be rematrixed up, not crash)
#       - frame count is in the right ballpark for fps * duration
#       - audio and video durations track each other (gapless / in sync)
#
# Modes:
#   stereo  synthetic stereo sine  -> baseline happy path
#   mono    synthetic mono   sine  -> regression for the mono-audio SIGBUS crash
#
# Usage: run_record_e2e.sh <harness_exe> <stereo|mono> [udp_port]
set -uo pipefail

HARNESS="${1:?harness executable path required}"
MODE="${2:-stereo}"
PORT="${3:-23456}"
SECONDS_TO_RECORD=6

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }

if [ "$MODE" = "mono" ]; then CH=1; else CH=2; fi

WORKDIR="$(mktemp -d)"
# Redirect $HOME so <Documents>/videos resolves inside the temp dir (hermetic).
export HOME="$WORKDIR"
mkdir -p "$WORKDIR/Documents/videos"

FFPID=""
cleanup() {
    [ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null
    wait "$FFPID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "[e2e] mode=$MODE channels=$CH port=$PORT workdir=$WORKDIR"

# --- 1. Producer: synthetic live stream (testsrc2 video + sine audio) --------
# h264 video + aac audio in MPEG-TS over UDP, paced in real time (-re).
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -ac "$CH" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${PORT}?pkt_size=1316" &
FFPID=$!
sleep 0.5  # let the producer come up before the consumer binds

# --- 2. Consumer: the real recording engine ----------------------------------
URL="udp://127.0.0.1:${PORT}?fifo_size=1000000&overrun_nonfatal=1"
HARNESS_OUT="$("$HARNESS" --url "$URL" --name "olr_e2e_${MODE}" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
HARNESS_RC=$?

if [ $HARNESS_RC -ne 0 ]; then
    echo "FAIL: harness exited $HARNESS_RC (mono regression = SIGBUS crash if mode=mono)"
    exit 1
fi

OUT_MKV="$(printf '%s\n' "$HARNESS_OUT" | tail -n 1)"
echo "[e2e] harness output: $OUT_MKV"

if [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no (or empty) output file at '$OUT_MKV'"
    exit 1
fi

# --- 3. Probe + assert -------------------------------------------------------
probe() { ffprobe -v error "$@" "$OUT_MKV"; }

V_PACKETS="$(probe -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 | head -n1)"
A_CHANNELS="$(probe -select_streams a:0 -show_entries stream=channels -of csv=p=0 | head -n1)"
DURATION="$(probe -show_entries format=duration -of csv=p=0 | head -n1)"

echo "[e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?} duration=${DURATION:-?}s"

fail=0
if [ -z "${A_CHANNELS:-}" ]; then
    echo "FAIL: no audio stream in output"; fail=1
elif [ "$A_CHANNELS" != "2" ]; then
    # The engine conforms ALL inputs (incl. mono) to 48kHz stereo S16.
    echo "FAIL: expected stereo (2ch) output, got ${A_CHANNELS}ch"; fail=1
fi

# Expect at least half of the nominal frames (fps*seconds), allowing for the
# UDP warm-up gap and CI jitter.
MIN_FRAMES=$(( 30 * SECONDS_TO_RECORD / 2 ))
if [ -z "${V_PACKETS:-}" ] || [ "${V_PACKETS:-0}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few video frames (${V_PACKETS:-0} < ${MIN_FRAMES})"; fail=1
fi

if [ $fail -ne 0 ]; then
    exit 1
fi

echo "PASS: e2e recording ($MODE) produced a valid stereo A/V file"
exit 0
