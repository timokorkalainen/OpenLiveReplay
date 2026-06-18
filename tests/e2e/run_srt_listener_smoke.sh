#!/usr/bin/env bash
# Local SRT e2e: prove the real engine records as an SRT LISTENER end-to-end over
# the native SRT path (srt_bind + srt_listen + srt_accept).
#
# This INVERTS the usual caller-vs-listener roles of the other SRT e2e: the engine
# binds + listens (mode=listener) and srt-live-transmit connects to it as the SRT
# CALLER, carrying a flash/beep MPEG-TS produced by ffmpeg over UDP. We assert the
# MKV decoded real content, which only happens if the engine actually accepted the
# inbound caller and ingested it.
#
# SKIP-clean (exit 77) when ffmpeg / ffprobe / srt-live-transmit are missing.
#
# Usage: run_srt_listener_smoke.sh <record_harness_exe> [srt_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
SRT_PORT="${2:-23730}"
UDP_PORT=$((SRT_PORT + 1))
SECONDS_TO_RECORD=8
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

echo "[srt-listener-e2e] srt_port=$SRT_PORT udp_port=$UDP_PORT (engine=listener, slt=caller)"

# 1. The real engine records as an SRT LISTENER. Start it in the background FIRST so
#    it is bound + listening before the caller connects (the engine also retries on
#    accept timeout with backoff, so exact ordering is not critical).
URL="srt://127.0.0.1:${SRT_PORT}?transtype=live&mode=listener"
OUT_FILE="$WORKDIR/harness.out"
( "$HARNESS" --url "$URL" --name olr_srt_listener_smoke --outdir "$WORKDIR" \
    --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30 >"$OUT_FILE" 2>&1 ) &
HARNESS_PID=$!
PIDS+=("$HARNESS_PID")
sleep 1.0  # give the engine time to bind + listen

# 2. Producer: ffmpeg(UDP) -> srt-live-transmit as the SRT CALLER into the engine.
ffmpeg -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" -ac 2 \
    "${OLR_H264_VCODEC_ARGS[@]}" \
    -c:a aac -b:a 128k \
    -f mpegts "udp://127.0.0.1:${UDP_PORT}?pkt_size=1316" &
PIDS+=($!)
srt-live-transmit "udp://127.0.0.1:${UDP_PORT}?mode=listener" \
    "srt://127.0.0.1:${SRT_PORT}?mode=caller&transtype=live&latency=200" >/dev/null 2>&1 &
PIDS+=($!)

# 3. Wait for the engine to finish recording.
wait "$HARNESS_PID"
RC=$?
OUT_MKV="$(tail -n 1 "$OUT_FILE")"
echo "[srt-listener-e2e] harness rc=$RC out=$OUT_MKV"

if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: SRT listener ingest produced no output (rc=$RC) — engine could not accept + record an inbound caller"
    sed 's/^/[harness] /' "$OUT_FILE" | tail -n 20
    exit 1
fi

# 4. Assert the MKV decoded real content (frames + the 1 kHz tone, not blue/silence).
scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
echo "[srt-listener-e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?}"

fail=0
MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 3))   # generous: caller connects after listen
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames over SRT listener (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"; fail=1
fi
if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo audio over SRT listener, got '${A_CHANNELS:-none}'"; fail=1
fi

RMS="$(ffmpeg -hide_banner -nostats -i "$OUT_MKV" -map 0:a:0 \
       -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
       | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
echo "[srt-listener-e2e] audio_rms_db=${RMS:-?}"
if [ -z "${RMS:-}" ] || [ "${RMS:-}" = "-inf" ] || ! awk -v r="${RMS:-}" 'BEGIN{exit !(r+0 > -60)}'; then
    echo "FAIL: recorded audio is silence/unmeasurable (rms=${RMS:-none} dB) — listener did not ingest the inbound caller"; fail=1
fi

[ $fail -ne 0 ] && exit 1
echo "PASS: e2e SRT listener ingest — ${V_PACKETS} frames, stereo audio recorded from an inbound caller"
exit 0
