#!/usr/bin/env bash
# End-to-end recording test driver.
#
# 1. Streams a synthetic live A/V source over UDP MPEG-TS with FFmpeg, then
#    bridges it to an SRT listener (srt-live-transmit) so the engine ingests it
#    over its native srt:// transport (the only ingest path now — udp:// is no
#    longer accepted).
# 2. Runs the headless record_harness as an SRT CALLER for a fixed duration.
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
#   stereo    synthetic stereo sine -> baseline happy path
#   mono      synthetic mono   sine -> regression for the mono-audio SIGBUS crash
#   rational       MPEG-2, recorded advertising 29.97 (30000/1001) -> asserts the
#                  container carries the true rational rate, not the legacy {30,1}
#   rational-h264  same, but the hardware H.264 record path (SKIP 77 without HW)
#
# Usage: run_record_e2e.sh <harness_exe> <stereo|mono|rational|rational-h264> [srt_port]
set -uo pipefail

HARNESS="${1:?harness executable path required}"
MODE="${2:-stereo}"
SRT_PORT="${3:-23456}"
UDP_PORT=$((SRT_PORT + 1))
SECONDS_TO_RECORD=6

# shellcheck source=tests/e2e/srt_lib.sh
. "$(cd "$(dirname "$0")" && pwd)/srt_lib.sh"
srt_require_tools  # SKIP (exit 0) unless ffmpeg/ffprobe/srt-live-transmit present
olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder"; exit 0; }

if [ "$MODE" = "mono" ]; then CH=1; else CH=2; fi

# Rational-rate modes: record advertising 29.97 (30000/1001) and assert the output
# container carries that exact rate (not the legacy {30,1}). FPS_EXTRA / CODEC_EXTRA
# are plain strings expanded unquoted (empty for stereo/mono) to stay bash-3.2/set-u
# safe. `rational` is the MPEG-2 software path; `rational-h264` is the hardware H.264
# path (SKIPs 77 when no hardware H.264 encoder is available, e.g. headless runners).
FPS_EXTRA=""
EXPECT_RATE=""
CODEC_EXTRA=""
case "$MODE" in
    rational)
        FPS_EXTRA="--fps-num 30000 --fps-den 1001"; EXPECT_RATE="30000/1001" ;;
    rational-h264)
        FPS_EXTRA="--fps-num 30000 --fps-den 1001"; EXPECT_RATE="30000/1001"
        CODEC_EXTRA="--codec h264"
        # The recorder needs a hardware H.264 ENCODER for --codec h264. Probe via
        # the harness; a crashing probe is a FAIL, an unavailable codec is a SKIP.
        CAPS="$("$HARNESS" --probe-codec-caps 2>&1)"; PROBE_RC=$?
        [ "$PROBE_RC" -eq 0 ] || { echo "FAIL: --probe-codec-caps exited $PROBE_RC"; printf '%s\n' "$CAPS"; exit 1; }
        H264_AVAIL="$(printf '%s\n' "$CAPS" | awk -F= '/^h264=/{print $2}')"
        [ "${H264_AVAIL:-0}" = "1" ] || { echo "SKIP: hardware H.264 unavailable"; exit 77; }
        ;;
esac

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "[e2e] mode=$MODE channels=$CH srt_port=$SRT_PORT udp_port=$UDP_PORT"

is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

# --- 1. Producer: synthetic live stream (testsrc2 video + sine audio) --------
# h264 video + aac audio in MPEG-TS over UDP, paced in real time (-re), bridged
# to an SRT listener by srt-live-transmit so the engine ingests over srt://.
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -ac "$CH" \
    "${OLR_H264_VCODEC_ARGS[@]}" \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
PIDS+=($!)
srt_bridge "$UDP_PORT" "$SRT_PORT"  # UDP MPEG-TS -> SRT listener
sleep 1.0 # let the producer + SRT listener come up before the caller connects

# --- 2. Consumer: the real recording engine (native SRT caller) --------------
URL="$(srt_caller_url "$SRT_PORT")"
# shellcheck disable=SC2086 # FPS_EXTRA/CODEC_EXTRA are intentionally word-split (may be empty).
HARNESS_OUT="$("$HARNESS" --url "$URL" --name "olr_e2e_${MODE}" --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30 $FPS_EXTRA $CODEC_EXTRA)"
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

# Frame count: at least half the nominal fps*seconds (SRT warm-up + CI jitter).
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

# Rational mode: the container must advertise the true rate, not the legacy {30,1}.
if [ -n "$EXPECT_RATE" ]; then
    R_RATE="$(scalar -select_streams v:0 -show_entries stream=r_frame_rate)"
    echo "[e2e] r_frame_rate=${R_RATE:-?} (expect ${EXPECT_RATE})"
    if [ "${R_RATE:-}" != "$EXPECT_RATE" ]; then
        echo "FAIL: video r_frame_rate is '${R_RATE:-none}', expected ${EXPECT_RATE} — rational rate not advertised end to end"
        fail=1
    fi

    # TIMING GUARD: the advertised 29.97 rate must change METADATA ONLY — it must
    # NOT leak into the coded video PTS. The muxed video must stay on the integer-fps
    # ms cadence (~33.333ms = 1000/30), locked to the integer-fps audio, NOT the
    # 29.97 grid (~33.367ms = 1001000/30000). A regression that rescales the video
    # PTS through a rational encoder time_base would push the mean interval past the
    # 33.35ms bound and desync video from audio. Mean over ~180 frames averages out
    # ms-rounding noise, so the 33.333-vs-33.367 split is robustly distinguishable.
    VMEAN="$(probe -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 \
        | awk 'NF{n++; if(n==1)f=$1; l=$1} END{ if(n>1) printf "%.4f",(l-f)*1000/(n-1); else print "nan" }')"
    echo "[e2e] mean video frame interval=${VMEAN}ms (integer-30=33.333; 29.97-grid=33.367)"
    if ! awk -v m="${VMEAN:-99}" 'BEGIN{exit !(m+0 > 33.0 && m+0 < 33.35)}'; then
        echo "FAIL: video frame interval ${VMEAN}ms is off the integer-fps cadence (~33.333ms) — the rational rate leaked into the coded video PTS (A/V drift regression)"
        fail=1
    fi
fi

if [ $fail -ne 0 ]; then
    exit 1
fi

echo "PASS: e2e recording ($MODE) — stereo A/V, ${V_PACKETS} frames, A/V end-aligned${EXPECT_RATE:+, r_frame_rate=$EXPECT_RATE}"
exit 0
