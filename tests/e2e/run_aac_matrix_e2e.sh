#!/usr/bin/env bash
# Focused native-SRT AAC-LC confidence matrix.
#
# Each cell generates a live H.264/AAC MPEG-TS source, bridges UDP->SRT with
# srt-live-transmit, records through the real engine, and probes the MKV for the
# engine audio contract: PCM S16, 48 kHz, stereo, non-silent, and A/V-aligned.
# Repeating the matrix also exercises the Windows Media Foundation AAC teardown
# and the native SRT thread shutdown path that previously exposed heap corruption.
#
# Usage: run_aac_matrix_e2e.sh <record_harness_exe> [base_srt_port]
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
BASE="${2:-23820}"
SECONDS_TO_RECORD="${OLR_AAC_MATRIX_SECONDS:-5}"
REPEATS="${OLR_AAC_MATRIX_REPEATS:-2}"

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"
HARNESS="$(olr_msys_path "$HARNESS")"

command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not found"; exit 77; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 77; }
command -v srt-live-transmit >/dev/null || {
    echo "SKIP: srt-live-transmit not found"
    exit 77
}
olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder"; exit 77; }

is_num() { case "${1:-}" in '' | *[!0-9.-]*) return 1 ;; *) return 0 ;; esac; }

PIDS=()
WORKDIR=""

cleanup_current() {
    if [ "${#PIDS[@]}" -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null
    fi
    wait 2>/dev/null
    if [ -n "${WORKDIR:-}" ]; then
        rm -rf "$WORKDIR"
    fi
    PIDS=()
    WORKDIR=""
}
trap cleanup_current EXIT

scalar() {
    local mkv="$1"
    shift
    ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$mkv" | head -n1
}

last_pts() {
    local mkv="$1" stream="$2"
    ffprobe -v error -select_streams "$stream" -show_entries packet=pts_time -of csv=p=0 "$mkv" \
        | awk 'NF{v=$1} END{print v}'
}

run_case() {
    local name="$1" sample_rate="$2" channels="$3" bitrate="$4" freq="$5" port_offset="$6"
    local srt_port=$((BASE + port_offset))
    local udp_port=$((srt_port + 1))
    local min_frames=$((30 * SECONDS_TO_RECORD / 2))

    WORKDIR="$(mktemp -d)"
    PIDS=()

    echo "[aac-matrix] case=$name sr=$sample_rate ch=$channels br=$bitrate srt=$srt_port"

    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=320x240:rate=30" \
        -f lavfi -i "sine=frequency=${freq}:sample_rate=${sample_rate}" \
        -ac "$channels" -ar "$sample_rate" \
        "${OLR_H264_VCODEC_ARGS[@]}" \
        -c:a aac -b:a "$bitrate" \
        -f mpegts "udp://127.0.0.1:${udp_port}?pkt_size=1316" &
    PIDS+=("$!")

    srt_bridge "$udp_port" "$srt_port"
    sleep 1.0

    "$HARNESS" --url "$(srt_caller_url "$srt_port")" \
        --name "olr_aac_${name}" --outdir "$WORKDIR" \
        --seconds "$SECONDS_TO_RECORD" --width 320 --height 240 --fps 30 \
        >"$WORKDIR/harness.out" 2>"$WORKDIR/harness.err"
    local rc=$?
    local out_mkv
    out_mkv="$(tail -n 1 "$WORKDIR/harness.out" 2>/dev/null || true)"

    local fail=0
    if [ "$rc" -ne 0 ] || [ -z "${out_mkv:-}" ] || [ ! -s "$out_mkv" ]; then
        echo "FAIL: $name produced no output (rc=$rc out=${out_mkv:-none})"
        fail=1
    fi

    if [ "$fail" -eq 0 ]; then
        local v_packets a_codec a_rate a_channels v_last a_last rms
        v_packets="$(scalar "$out_mkv" -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
        a_codec="$(scalar "$out_mkv" -select_streams a:0 -show_entries stream=codec_name)"
        a_rate="$(scalar "$out_mkv" -select_streams a:0 -show_entries stream=sample_rate)"
        a_channels="$(scalar "$out_mkv" -select_streams a:0 -show_entries stream=channels)"
        v_last="$(last_pts "$out_mkv" v:0)"
        a_last="$(last_pts "$out_mkv" a:0)"
        rms="$(ffmpeg -hide_banner -nostats -i "$out_mkv" -map 0:a:0 \
            -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
            | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"

        echo "[aac-matrix] $name packets=${v_packets:-?} audio=${a_codec:-?}/${a_rate:-?}Hz/${a_channels:-?}ch rms=${rms:-?} v_last=${v_last:-?} a_last=${a_last:-?}"

        if ! is_num "${v_packets:-}" || [ "${v_packets%.*}" -lt "$min_frames" ]; then
            echo "FAIL: $name too few/invalid video frames (${v_packets:-none}, need >= $min_frames)"
            fail=1
        fi
        if [ "${a_codec:-}" != "pcm_s16le" ]; then
            echo "FAIL: $name expected pcm_s16le audio, got '${a_codec:-none}'"
            fail=1
        fi
        if [ "${a_rate:-}" != "48000" ]; then
            echo "FAIL: $name expected 48 kHz output, got '${a_rate:-none}'"
            fail=1
        fi
        if [ "${a_channels:-}" != "2" ]; then
            echo "FAIL: $name expected stereo output, got '${a_channels:-none}'"
            fail=1
        fi
        if [ -z "${rms:-}" ] || [ "${rms:-}" = "-inf" ] || ! awk -v r="${rms:-}" 'BEGIN{exit !(r+0 > -60)}'; then
            echo "FAIL: $name recorded audio is silence/unmeasurable (rms=${rms:-none} dB)"
            fail=1
        fi
        if ! is_num "${v_last:-}" || ! is_num "${a_last:-}"; then
            echo "FAIL: $name could not read stream end timestamps (v=${v_last:-none} a=${a_last:-none})"
            fail=1
        elif ! awk -v v="$v_last" -v a="$a_last" 'BEGIN{d=v-a; if(d<0)d=-d; exit !(d<0.75)}'; then
            echo "FAIL: $name audio/video end timestamps diverge >0.75s (v=$v_last a=$a_last)"
            fail=1
        fi
    fi

    if [ "$fail" -ne 0 ]; then
        echo "--- harness stderr ($name) ---"
        tail -n 100 "$WORKDIR/harness.err" 2>/dev/null || true
        return 1
    fi

    cleanup_current
    return 0
}

CASES=(
    "stereo48 48000 2 128k 1000"
    "mono48 48000 1 96k 700"
    "stereo441 44100 2 96k 1200"
    "stereo32 32000 2 64k 1500"
)

case_count="${#CASES[@]}"
for repeat in $(seq 1 "$REPEATS"); do
    echo "[aac-matrix] repeat=$repeat/$REPEATS"
    idx=0
    for row in "${CASES[@]}"; do
        # shellcheck disable=SC2086
        set -- $row
        run_case "$1" "$2" "$3" "$4" "$5" $((repeat * 100 + idx * 2)) || exit 1
        idx=$((idx + 1))
    done
done

echo "PASS: native SRT AAC matrix (${case_count} cases x ${REPEATS} repeats) kept 48 kHz stereo PCM, non-silent audio, and A/V alignment"
exit 0
