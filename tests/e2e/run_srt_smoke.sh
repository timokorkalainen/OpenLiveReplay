#!/usr/bin/env bash
# Local SRT e2e: prove the real engine records one srt:// stream end-to-end.
#
# Stands up an SRT LISTENER carrying a flash/beep MPEG-TS via the brew ffmpeg
# (UDP) + the already-installed srt-live-transmit (UDP->SRT). record_harness
# connects as an SRT CALLER (the engine's default) and we assert the MKV is
# valid. Requires the harness to be built with -DOLR_FFMPEG_SRT_PREFIX (i.e. an
# SRT-enabled avformat); otherwise the engine cannot open srt:// and this FAILS.
#
# Usage: run_srt_smoke.sh <record_harness_exe> [srt_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
SRT_PORT="${2:-23501}"
UDP_PORT=$((SRT_PORT + 1))
SECONDS_TO_RECORD=6

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT
is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

echo "[srt-e2e] srt_port=$SRT_PORT udp_port=$UDP_PORT"

# 1. SRT listener carrying flash/beep MPEG-TS: ffmpeg(UDP) -> srt-live-transmit(SRT listener).
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
PIDS+=($!)
srt-live-transmit "udp://127.0.0.1:${UDP_PORT}?mode=listener" \
    "srt://127.0.0.1:${SRT_PORT}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
PIDS+=($!)
sleep 1.0  # let the producer + SRT listener come up before the caller connects

# 2. The real engine connects as SRT caller and records.
URL="srt://127.0.0.1:${SRT_PORT}?transtype=live"
OUT="$("$HARNESS" --url "$URL" --name olr_srt_smoke --outdir "$WORKDIR" \
       --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-e2e] harness rc=$RC out=$OUT_MKV"

if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: SRT ingest produced no output (rc=$RC) — engine could not record srt:// (is the harness built with -DOLR_FFMPEG_SRT_PREFIX?)"
    exit 1
fi

# 3. Assert the MKV is valid (same checks as run_record_e2e.sh).
scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
echo "[srt-e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?}"

fail=0
MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames over SRT (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"; fail=1
fi
if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo audio over SRT, got '${A_CHANNELS:-none}'"; fail=1
fi

# POSITIVE content proof: the recorded audio must carry the 1 kHz tone the SRT
# stream sent — NOT silence. record_harness writes blue-fill video + SILENCE when
# no source connects, so this is what actually proves SRT content was ingested
# (and is what makes the SRT-less brew build fail this test).
RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
       -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
       | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
echo "[srt-e2e] audio_rms_db=${RMS:-?}"
# Silence reports "-inf" or a very low dB; the 1 kHz tone is around -20..-25 dB.
if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
    echo "FAIL: recorded audio is silence/unmeasurable (rms=${RMS:-none} dB) — SRT stream content was NOT ingested (blue-fill/no-source path)"; fail=1
fi

[ $fail -ne 0 ] && exit 1
echo "PASS: e2e SRT ingest — ${V_PACKETS} frames, stereo audio recorded from srt://"
exit 0
