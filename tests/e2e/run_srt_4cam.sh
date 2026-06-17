#!/usr/bin/env bash
# Local SRT e2e: prove 4 real SRT streams record into a 4-view MKV and ROUTE
# correctly — view i carries camera i's content. Each camera emits a distinct
# audio tone (camera i = (i+1)*1000 Hz); we detect each recorded view's dominant
# tone and assert it matches that camera. A view from a source that failed to
# connect is blue-fill SILENCE (no dominant tone) and FAILS. The engine ingests
# srt:// over its native SRT path — no SRT-enabled ffmpeg build is needed.
#
# Teeth-check: set OLR_SRT4_EXPECT_SHIFT=1 to rotate the EXPECTED mapping (view i
# expects camera i+1); a correctly-routed recording then FAILS, proving the
# routing assertion really discriminates.
#
# Usage: run_srt_4cam.sh <sync_harness_exe> [base_port]
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23510}"
SECONDS_TO_RECORD=8
SHIFT="${OLR_SRT4_EXPECT_SHIFT:-0}"

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }
command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[srt-4cam] base_port=$BASE shift=$SHIFT"

# --- 1. Four distinct-tone SRT cameras: camera i = sine (i+1)*1000 Hz over srt. ---
URLS=()
for i in 0 1 2 3; do
    srt_port=$((BASE + 2*i)); udp_port=$((BASE + 2*i + 1)); freq=$(((i+1)*1000))
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -f lavfi -i "sine=frequency=${freq}:sample_rate=48000" -ac 2 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -c:a aac -b:a 128k \
        -f mpegts "udp://127.0.0.1:${udp_port}?pkt_size=1316" &
    PIDS+=($!)
    srt-live-transmit "udp://127.0.0.1:${udp_port}?mode=listener" \
        "srt://127.0.0.1:${srt_port}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
    PIDS+=($!)
    URLS+=("srt://127.0.0.1:${srt_port}?transtype=live")
done
sleep 1.5  # let 4 producers + 4 SRT listeners come up before the engine connects

# --- 2. Record all four as a 4-view MKV (source i -> view i). ---
OUT="$("$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
       --outdir "$WORKDIR" --name srt4cam --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[srt-4cam] harness rc=$RC out=$OUT_MKV"
if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no output (rc=$RC) — engine could not record the SRT sources over the native SRT path"
    exit 1
fi

# --- 3. Assert 4 video tracks. ---
VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$OUT_MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "4" ]; then
    echo "FAIL: expected 4 video tracks, got ${VTRACKS:-0}"; exit 1
fi

# Dominant audio band (Hz) of one recorded view, or "none" if below the -60 dB floor.
detect_band() {  # $1=mkv $2=audio-stream-index
    local mkv="$1" idx="$2" best_f="none" best_rms="-1000" f rms
    for f in 1000 2000 3000 4000; do
        rms="$(ffmpeg -hide_banner -nostats -i "$mkv" -map "0:a:$idx" \
               -af "bandpass=f=$f:width_type=h:w=200,astats=metadata=1:measure_overall=RMS_level" \
               -f null - 2>&1 | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
        if [ -z "$rms" ] || [ "$rms" = "-inf" ]; then rms="-1000"; fi
        if awk -v a="$rms" -v b="$best_rms" 'BEGIN{exit !(a+0 > b+0)}'; then best_rms="$rms"; best_f="$f"; fi
    done
    awk -v r="$best_rms" 'BEGIN{exit !(r+0 > -60)}' && echo "$best_f" || echo "none"
}

# --- 4. Per-view routing assertion: view i must carry camera (i+SHIFT)'s tone. ---
fail=0; line="[srt-4cam]"
for i in 0 1 2 3; do
    expected=$(( ((( i + SHIFT ) % 4) + 1) * 1000 ))
    detected="$(detect_band "$OUT_MKV" "$i")"
    line="$line view$i=${detected}Hz(exp${expected})"
    if [ "$detected" != "$expected" ]; then
        echo "FAIL: view $i carries ${detected}Hz, expected camera tone ${expected}Hz — wrong routing or source not connected"
        fail=1
    fi
done
echo "$line"
[ $fail -ne 0 ] && exit 1
echo "PASS: 4-source SRT routing — each view carries its own camera's tone"
exit 0
