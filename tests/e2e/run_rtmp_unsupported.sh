#!/usr/bin/env bash
set -uo pipefail
# Negative native-RTMP unsupported-profile gate. This script verifies that native
# RTMP reports unsupported inputs visibly and does not keep retrying native; it
# does not use the legacy FFmpeg RTMP path.
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23760}"
IDLE_PORT=$((PORT + 1))

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

FLV="$WORKDIR/unsupported.flv"
SERVER_LOG="$WORKDIR/server.log"

if ! ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -t 5 -c:v flv -an -f flv "$FLV"; then
    echo "SKIP: ffmpeg cannot generate FLV1 fixture"
    exit 77
fi

fail_case() {
    local case_name="$1" reason="$2" harness_rc="$3" harness_out="$4" harness_err="$5" out_mkv="$6" server_log="$7"
    echo "FAIL: $case_name: $reason"
    echo "harness rc=$harness_rc"
    echo "harness output=${out_mkv:-<none>}"
    if [ -n "${out_mkv:-}" ] && [ -e "$out_mkv" ]; then
        ls -lh "$out_mkv"
    fi
    echo "--- harness stdout ---"
    cat "$harness_out"
    echo "--- harness stderr ---"
    cat "$harness_err"
    echo "--- RTMP server log ---"
    cat "$server_log"
    exit 1
}

source_like_recorded_output() {
    local out_mkv="$1"
    [ -n "${out_mkv:-}" ] && [ -s "$out_mkv" ] || return 1

    local rms
    rms="$(ffmpeg -hide_banner -nostats -i "$out_mkv" -map 0:a:0 \
           -af astats=metadata=1:measure_overall=RMS_level -f null - 2>&1 \
           | awk -F': ' '/Overall/{o=1} o && /RMS level dB/{print $2; exit}')"
    if [ -n "${rms:-}" ] && [ "$rms" != "-inf" ] && awk -v r="$rms" 'BEGIN{exit !(r+0 > -60)}'; then
        return 0
    fi

    local y_range
    y_range="$(ffmpeg -hide_banner -loglevel error -i "$out_mkv" -map 0:v:0 \
           -vf "signalstats,metadata=print:file=-" -frames:v 90 -f null - 2>/dev/null \
           | awk -F= '/YAVG=/{y=$2+0; if (!seen || y<min) min=y; if (!seen || y>max) max=y; seen=1}
                      END{if (seen) printf "%.3f\n", max-min; else print ""}')"
    [ -n "${y_range:-}" ] && awk -v r="$y_range" 'BEGIN{exit !(r > 10)}'
}

generic_failure_after_unsupported() {
    local harness_err="$1"
    awk '
        /Native RTMP unsupported profile/ { seen=1; next }
        seen && (/Native RTMP read failed/ ||
                 /Native RTMP disconnected/ ||
                 /Native RTMP connect failed/ ||
                 /Connect failed\. Retrying/ ||
                 /Attempting connection to:/) {
            found=1
        }
        END { exit found ? 0 : 1 }
    ' "$harness_err"
}

run_harness_case() {
    local case_name="$1" port="$2" seconds="$3" server_log="$4"
    local harness_out="$WORKDIR/${case_name}.harness.out"
    local harness_err="$WORKDIR/${case_name}.harness.err"
    local out_mkv=""
    local harness_rc=0

    "$HARNESS" --url "$(rtmp_url "$port")" --name "rtmp_${case_name}" \
        --outdir "$WORKDIR" --seconds "$seconds" --width 640 --height 480 --fps 30 \
        >"$harness_out" 2>"$harness_err"
    harness_rc=$?
    out_mkv="$(tail -n 1 "$harness_out" 2>/dev/null || true)"

    if ! grep -q "Native RTMP connected" "$harness_err"; then
        fail_case "$case_name" "native RTMP backend did not connect" "$harness_rc" \
            "$harness_out" "$harness_err" "$out_mkv" "$server_log"
    fi
    if ! grep -q "Native RTMP unsupported profile" "$harness_err"; then
        fail_case "$case_name" "missing explicit native RTMP unsupported-profile reason" \
            "$harness_rc" "$harness_out" "$harness_err" "$out_mkv" "$server_log"
    fi
    if [ "$harness_rc" -ne 0 ]; then
        fail_case "$case_name" "record_harness exited nonzero" "$harness_rc" \
            "$harness_out" "$harness_err" "$out_mkv" "$server_log"
    fi
    if generic_failure_after_unsupported "$harness_err"; then
        fail_case "$case_name" "generic native RTMP failure or retry after unsupported marker" \
            "$harness_rc" "$harness_out" "$harness_err" "$out_mkv" "$server_log"
    fi
    if source_like_recorded_output "$out_mkv"; then
        fail_case "$case_name" "unsupported stream produced source-like recorded output" \
            "$harness_rc" "$harness_out" "$harness_err" "$out_mkv" "$server_log"
    fi

    echo "PASS: $case_name rejected with explicit unsupported reason (harness rc=$harness_rc)"
}

rtmp_server "$PORT" "$FLV" "$SERVER_LOG" || exit 1
run_harness_case "unsupported_video" "$PORT" 6 "$SERVER_LOG"

IDLE_SERVER_LOG="$WORKDIR/idle_server.log"
rtmp_fixture_server_cmd --port "$IDLE_PORT" --idle-after-play --hold-open 8 \
    >"$IDLE_SERVER_LOG" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do
    if grep -q '^READY ' "$IDLE_SERVER_LOG"; then break; fi
    sleep 0.1
done
if ! grep -q '^READY ' "$IDLE_SERVER_LOG"; then
    echo "FAIL: idle RTMP fixture server did not become ready on port $IDLE_PORT"
    cat "$IDLE_SERVER_LOG"
    exit 1
fi
run_harness_case "idle_probe" "$IDLE_PORT" 7 "$IDLE_SERVER_LOG"

echo "PASS: unsupported native RTMP profile fails visibly without FFmpeg RTMP fallback"
