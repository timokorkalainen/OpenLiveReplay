#!/usr/bin/env bash
# End-to-end hardware H.264 recording smoke.
#
# Records a short session with the engine's codec set to H.264Hardware (VideoToolbox
# on macOS, MediaFoundation on Windows) and asserts the output MKV:
#   1. Contains an h264 video stream.
#   2. Every video packet is a keyframe (intra-only — the encoder must emit all-I).
#   3. The full track layout is present (video + audio; subtitle/metadata tracks
#      appear when OLR_VIEWS > 1 with telemetry, but the base layout is V+A).
#   4. Content is non-trivial: audio RMS is above -60 dB (not silence/blue-fill).
#
# SKIP gates (exit 0 on skip):
#   - ffmpeg/ffprobe/srt-live-transmit not found.
#   - Hardware H.264 encode unavailable (reported by --probe-codec-caps).
#
# Usage: run_record_h264_e2e.sh <record_harness_exe> [srt_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
SRT_PORT="${2:-23462}"
UDP_PORT=$((SRT_PORT + 1))
SECONDS_TO_RECORD=6
HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=tool_env.sh
. "$HERE/tool_env.sh"
olr_prepend_built_tool_paths

# --- Tool guards (SKIP cleanly when tools are absent) -------------------------
# Exit 77 = CTest SKIP_RETURN_CODE: CTest marks the test as "skipped" rather
# than failed, so CI passes cleanly on runners without a HW H.264 encoder or
# without the required producer tools.
command -v ffmpeg            >/dev/null || { echo "SKIP: ffmpeg not found";            exit 77; }
command -v ffprobe           >/dev/null || { echo "SKIP: ffprobe not found";           exit 77; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 77; }

# The ffmpeg producer uses an H.264 encoder; we need one that produces valid MPEG-TS.
olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder for producer"; exit 77; }

# --- Hardware H.264 encode capability gate (SKIP on CI without HW encoder) ----
CAPS="$("$HARNESS" --probe-codec-caps 2>/dev/null)"
H264_AVAIL="$(printf '%s\n' "$CAPS" | awk -F= '/^h264=/{print $2}')"
if [ "${H264_AVAIL:-0}" != "1" ]; then
    echo "SKIP: hardware H.264 encoder not available on this machine (queryNativeVideoEncodeCapabilities().h264=false)"
    exit 77
fi

echo "[h264-e2e] hardware H.264 available; starting smoke (srt_port=$SRT_PORT udp_port=$UDP_PORT)"

# --- Temp dir + cleanup -------------------------------------------------------
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

# --- 1. Producer: synthetic live stream (testsrc2 video + 1 kHz sine audio) --
# H.264 video + AAC audio in MPEG-TS over UDP, paced in real-time (-re),
# bridged to an SRT listener so the engine ingests over srt://.
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -ac 2 \
    "${OLR_H264_VCODEC_ARGS[@]}" \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
PIDS+=($!)

srt-live-transmit "udp://127.0.0.1:${UDP_PORT}?mode=listener" \
    "srt://127.0.0.1:${SRT_PORT}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
PIDS+=($!)

sleep 1.0  # let the producer + SRT listener come up before the caller connects

# --- 2. Consumer: the real engine recording in H.264Hardware codec mode -------
URL="srt://127.0.0.1:${SRT_PORT}?transtype=live"
HARNESS_OUT="$("$HARNESS" \
    --url "$URL" \
    --name "olr_h264_smoke" \
    --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" \
    --width 640 --height 480 --fps 30 \
    --codec h264)"
HARNESS_RC=$?

if [ $HARNESS_RC -ne 0 ]; then
    echo "FAIL: harness exited $HARNESS_RC"
    exit 1
fi

OUT_MKV="$(printf '%s\n' "$HARNESS_OUT" | tail -n 1)"
echo "[h264-e2e] harness output: $OUT_MKV"

if [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no (or empty) output file at '${OUT_MKV:-<none>}'"
    exit 1
fi

# --- 3. Assert output properties with ffprobe ---------------------------------
scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }

# 3a. Video codec must be h264.
V_CODEC="$(scalar -select_streams v:0 -show_entries stream=codec_name)"
echo "[h264-e2e] video_codec=${V_CODEC:-?}"

# 3b. Packet count (need at least half the expected frames).
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
echo "[h264-e2e] video_packets=${V_PACKETS:-?}"

# 3c. Audio channels (must be stereo — mono inputs are rematrixed up).
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
echo "[h264-e2e] audio_channels=${A_CHANNELS:-?}"

# 3d. All-intra check: count video packets whose flags do NOT include the keyframe
#     bit (key_frame=0). For a valid intra-only H.264 stream this count must be 0.
NON_KF="$(ffprobe -v error -select_streams v:0 \
    -show_entries packet=flags -of default=noprint_wrappers=1:nokey=1 \
    "$OUT_MKV" | grep -c '^[^K]' || true)"
echo "[h264-e2e] non_keyframe_packets=${NON_KF:-?}"

# 3e. Audio end / A-V sync.
last_pts() {
    ffprobe -v error -select_streams "$1" -show_entries packet=pts_time \
        -of csv=p=0 "$OUT_MKV" | awk 'NF{v=$1} END{print v}'
}
V_LAST="$(last_pts v:0)"
A_LAST="$(last_pts a:0)"
echo "[h264-e2e] v_last_pts=${V_LAST:-?} a_last_pts=${A_LAST:-?}"

# 3f. Content check: audio RMS must be above -60 dB (a 1 kHz tone is ~-20..-25 dB;
#     blue-fill/silence is -inf). Mirrors run_srt_smoke.sh.
RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
       -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
       | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
echo "[h264-e2e] audio_rms_db=${RMS:-?}"

# --- 4. Gate results ----------------------------------------------------------
fail=0

if [ "${V_CODEC:-}" != "h264" ]; then
    echo "FAIL: expected video codec h264, got '${V_CODEC:-none}'"
    fail=1
fi

MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"
    fail=1
fi

if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo (2ch) audio, got '${A_CHANNELS:-none}'"
    fail=1
fi

# All-intra: every packet must be a keyframe (flags start with 'K').
if ! is_num "${NON_KF:-}" || [ "${NON_KF}" -gt 0 ]; then
    echo "FAIL: ${NON_KF:-?} non-keyframe video packets found (encoder must be all-intra)"
    fail=1
fi

# A/V end-alignment: end timestamps must be within 0.75 s of each other.
if ! is_num "${V_LAST:-}" || ! is_num "${A_LAST:-}"; then
    echo "FAIL: could not read stream end timestamps (v='${V_LAST:-}' a='${A_LAST:-}')"
    fail=1
elif ! awk -v v="$V_LAST" -v a="$A_LAST" 'BEGIN{d=v-a; if(d<0)d=-d; exit !(d<0.75)}'; then
    echo "FAIL: audio/video end timestamps diverge >0.75s (v=$V_LAST a=$A_LAST)"
    fail=1
fi

# Content check: reject silence / blue-fill.
if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || \
   ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
    echo "FAIL: recorded audio is silence/unmeasurable (rms=${RMS:-none} dB) — content was NOT ingested"
    fail=1
fi

if [ $fail -ne 0 ]; then
    exit 1
fi

echo "PASS: e2e hardware H.264 recording — ${V_CODEC} codec, ${V_PACKETS} frames (all-intra, ${NON_KF} non-KF), stereo audio, rms=${RMS} dB"
exit 0
