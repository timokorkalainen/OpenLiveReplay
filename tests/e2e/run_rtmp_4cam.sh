#!/usr/bin/env bash
# Native RTMP e2e: 4 RTMP sources route to the matching 4 recorded views.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rtmp_lib.sh
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23810}"
SECS=8
SHIFT="${OLR_RTMP4_EXPECT_SHIFT:-0}"

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "[rtmp-4cam] base_port=$BASE shift=$SHIFT"

URLS=()
for i in 0 1 2 3; do
    port=$((BASE + i)); freq=$(((i + 1) * 1000))
    flv="$WORKDIR/cam${i}.flv"
    rtmp_generate_tone_flv "$flv" "$freq" "$SECS"
    rtmp_server "$port" "$flv" "$WORKDIR/server${i}.log" || exit 1
    URLS+=("$(rtmp_url "$port")")
done

OUT="$("$HARNESS" --url "${URLS[0]}" --url "${URLS[1]}" --url "${URLS[2]}" --url "${URLS[3]}" \
       --outdir "$WORKDIR" --name rtmp4cam --seconds "$SECS" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[rtmp-4cam] harness rc=$RC out=$OUT_MKV"
if [ $RC -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: no output (rc=$RC) -- engine could not record the RTMP sources"; exit 1
fi

VTRACKS="$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$OUT_MKV" | wc -l | tr -d ' ')"
if [ "${VTRACKS:-0}" != "4" ]; then echo "FAIL: expected 4 video tracks, got ${VTRACKS:-0}"; exit 1; fi

detect_band() {
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

fail=0; line="[rtmp-4cam]"
for i in 0 1 2 3; do
    expected=$(((((i + SHIFT) % 4) + 1) * 1000))
    detected="$(detect_band "$OUT_MKV" "$i")"
    line="$line view$i=${detected}Hz(exp${expected})"
    if [ "$detected" != "$expected" ]; then
        echo "FAIL: view $i carries ${detected}Hz, expected camera tone ${expected}Hz"
        fail=1
    fi
done
echo "$line"
[ $fail -ne 0 ] && exit 1
echo "PASS: 4-source native RTMP routing -- each view carries its own camera's tone"
exit 0
