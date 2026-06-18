#!/usr/bin/env bash
# Local SRT e2e: prove the real engine records an ENCRYPTED srt:// stream
# end-to-end over the native SRT path (SRTO_PASSPHRASE / SRTO_PBKEYLEN).
#
# Stands up an SRT LISTENER carrying a flash/beep MPEG-TS via the brew ffmpeg
# (UDP) + srt-live-transmit (UDP->SRT), with a matching passphrase+pbkeylen on
# the listener side. record_harness connects as an SRT CALLER carrying the SAME
# passphrase in the srt:// URL and we assert the MKV decoded real content. A
# passphrase MISMATCH (or no encryption support) yields no frames -> the test
# fails, so this positively proves encrypted ingest works.
#
# SKIP-clean (exit 77) when ffmpeg / ffprobe / srt-live-transmit are missing.
#
# Usage: run_srt_encrypted_smoke.sh <record_harness_exe> [srt_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
SRT_PORT="${2:-23720}"
UDP_PORT=$((SRT_PORT + 1))
SECONDS_TO_RECORD=6
PASSPHRASE="openlivereplaysecret"   # 20 chars, within SRT's 10..79 range
PBKEYLEN=16
HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=tool_env.sh
. "$HERE/tool_env.sh"
olr_prepend_built_tool_paths

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 77; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 77; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 77; }
olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder"; exit 77; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT
is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

echo "[srt-enc-e2e] srt_port=$SRT_PORT udp_port=$UDP_PORT pbkeylen=$PBKEYLEN"

# 1. Encrypted SRT listener carrying flash/beep MPEG-TS:
#    ffmpeg(UDP) -> srt-live-transmit(SRT listener, passphrase+pbkeylen).
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" -ac 2 \
    "${OLR_H264_VCODEC_ARGS[@]}" \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
PIDS+=($!)
srt-live-transmit "udp://127.0.0.1:${UDP_PORT}?mode=listener" \
    "srt://127.0.0.1:${SRT_PORT}?mode=listener&transtype=live&latency=200&passphrase=${PASSPHRASE}&pbkeylen=${PBKEYLEN}" \
    >/dev/null 2>&1 &
PIDS+=($!)
sleep 1.0  # let the producer + SRT listener come up before the caller connects

# 2. The real engine connects as an encrypted SRT caller and records.
URL="srt://127.0.0.1:${SRT_PORT}?transtype=live&passphrase=${PASSPHRASE}&pbkeylen=${PBKEYLEN}"
OUT="$("$HARNESS" --url "$URL" --name olr_srt_enc_smoke --outdir "$WORKDIR" \
       --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-enc-e2e] harness rc=$RC out=$OUT_MKV"

if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: encrypted SRT ingest produced no output (rc=$RC) — engine could not record encrypted srt://"
    exit 1
fi

# 3. Assert the MKV decoded real content (frames + the 1 kHz tone, not blue/silence).
scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
echo "[srt-enc-e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?}"

fail=0
MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames over encrypted SRT (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"; fail=1
fi
if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo audio over encrypted SRT, got '${A_CHANNELS:-none}'"; fail=1
fi

RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
       -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
       | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
echo "[srt-enc-e2e] audio_rms_db=${RMS:-?}"
if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
    echo "FAIL: recorded audio is silence/unmeasurable (rms=${RMS:-none} dB) — encrypted SRT content was NOT decrypted/ingested"; fail=1
fi

[ $fail -ne 0 ] && exit 1
echo "PASS: e2e encrypted SRT ingest — ${V_PACKETS} frames, stereo audio recorded from encrypted srt://"
exit 0
