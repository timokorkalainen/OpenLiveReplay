#!/usr/bin/env bash
# End-to-end recording test driver.
#
# 1. Streams a synthetic live A/V source over UDP MPEG-TS with FFmpeg.
# 2. Runs the headless record_harness against it for a fixed duration.
# 3. Probes the produced .mkv with ffprobe and asserts structural correctness:
#       - video + audio streams exist
#       - output audio is stereo (mono inputs MUST be rematrixed up, not crash)
#       - frame count is in the right ballpark for fps * duration
#       - audio and video end timestamps track each other (A/V in sync, gapless)
#
# Output location: the harness is pointed at a per-run temp dir via --outdir
# (the engine honors it through Muxer::setOutputDirectory), so this test is
# hermetic — the whole temp dir is removed on exit.
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
FFPID=""
cleanup() {
    [ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null
    wait "$FFPID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "[e2e] mode=$MODE channels=$CH port=$PORT"

is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

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
sleep 0.5 # let the producer come up before the consumer binds

# --- 2. Consumer: the real recording engine ----------------------------------
URL="udp://127.0.0.1:${PORT}?fifo_size=1000000&overrun_nonfatal=1"
HARNESS_OUT="$("$HARNESS" --url "$URL" --name "olr_e2e_${MODE}" --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
HARNESS_RC=$?

if [ $HARNESS_RC -ne 0 ]; then
    echo "FAIL: harness exited $HARNESS_RC (mono regression = SIGBUS crash if mode=mono)"
    exit 1
fi

OUT_MKV="$(printf '%s\n' "$HARNESS_OUT" | tail -n 1)"
echo "[e2e] harness output: $OUT_MKV"

if [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no (or empty) output file at '$OUT_MKV'"
    exit 1
fi

# --- 3. Probe + assert -------------------------------------------------------
probe() { ffprobe -v error "$@" "$OUT_MKV"; }
# Last presentation timestamp on a stream (for A/V end-alignment).
last_pts() {
    probe -select_streams "$1" -show_entries packet=pts_time -of csv=p=0 \
        | awk 'NF{v=$1} END{print v}'
}

# default=nokey gives a clean scalar; csv=p=0 appends a stray trailing comma.
scalar() { probe "$@" -of default=noprint_wrappers=1:nokey=1 | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
DURATION="$(scalar -show_entries format=duration)"
V_LAST="$(last_pts v:0)"
A_LAST="$(last_pts a:0)"

echo "[e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?} duration=${DURATION:-?}s v_last=${V_LAST:-?} a_last=${A_LAST:-?}"

fail=0

# Audio must be present and conformed to stereo (mono inputs rematrixed up).
if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo (2ch) output, got '${A_CHANNELS:-none}'"
    fail=1
fi

# Frame count: at least half the nominal fps*seconds (UDP warm-up + CI jitter).
# A non-numeric value (e.g. ffprobe 'N/A' from a truncated file) is a failure.
MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"
    fail=1
fi

# A/V sync / gaplessness: audio and video must end within 0.75s of each other.
if ! is_num "${V_LAST:-}" || ! is_num "${A_LAST:-}"; then
    echo "FAIL: could not read stream end timestamps (v='${V_LAST:-}' a='${A_LAST:-}')"
    fail=1
elif ! awk -v v="$V_LAST" -v a="$A_LAST" 'BEGIN{d=v-a; if(d<0)d=-d; exit !(d<0.75)}'; then
    echo "FAIL: audio/video end timestamps diverge >0.75s (v=$V_LAST a=$A_LAST)"
    fail=1
fi

if [ $fail -ne 0 ]; then
    exit 1
fi

echo "PASS: e2e recording ($MODE) — stereo A/V, ${V_PACKETS} frames, A/V end-aligned"
exit 0
