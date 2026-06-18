#!/usr/bin/env bash
# Native NDI ingest smoke. By default it starts a local NDI sender through the
# app's runtime-loaded output sink, so the gate only needs the NDI runtime.
#
# Usage:
#   run_ndi_smoke.sh <record_harness> [ndi_runtime_sender]
#   OLR_NDI_TEST_SOURCE='Studio (CAM1)' run_ndi_smoke.sh <record_harness>
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
LOCAL_SENDER="${2:-${OLR_NDI_RUNTIME_SENDER:-}}"
SECONDS_TO_RECORD="${OLR_NDI_SMOKE_SECS:-6}"
SOURCE_NAME="${OLR_NDI_TEST_SOURCE:-}"
PIDS=()

skip() { echo "SKIP: $*"; exit 77; }
is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }
cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    [ -n "${WORKDIR:-}" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

[ -x "$HARNESS" ] || { echo "FAIL: record_harness not executable: $HARNESS"; exit 1; }
command -v ffprobe >/dev/null || skip "ffprobe not found"
command -v python3 >/dev/null || skip "python3 not found"
WORKDIR="$(mktemp -d)"

if [ -z "$SOURCE_NAME" ]; then
    if [ -z "$LOCAL_SENDER" ]; then
        HARNESS_DIR="$(cd "$(dirname "$HARNESS")" && pwd)"
        LOCAL_SENDER="${HARNESS_DIR}/ndi_runtime_sender"
    fi
    [ -x "$LOCAL_SENDER" ] || skip "no OLR_NDI_TEST_SOURCE and ndi_runtime_sender not executable"

    SOURCE_NAME="OLR NDI Smoke $$ $(date +%s)"
    SENDER_SECONDS=$((SECONDS_TO_RECORD + ${OLR_NDI_SENDER_EXTRA_SECS:-12}))
    "$LOCAL_SENDER" --name "$SOURCE_NAME" --seconds "$SENDER_SECONDS" \
        --width 640 --height 480 --fps 30 \
        >"${WORKDIR}/ndi_runtime_sender.out" 2>"${WORKDIR}/ndi_runtime_sender.err" &
    PIDS+=("$!")
    sleep "${OLR_NDI_DISCOVERY_SECS:-4}"
    if ! kill -0 "${PIDS[0]}" 2>/dev/null; then
        rc=0
        wait "${PIDS[0]}" || rc=$?
        sed -n '1,120p' "${WORKDIR}/ndi_runtime_sender.err" >&2
        [ "$rc" -eq 77 ] && skip "NDI runtime unavailable for local sender"
        echo "FAIL: local NDI sender exited before discovery (rc=$rc)"
        exit 1
    fi
fi

ENCODED_SOURCE="$(SOURCE_NAME="$SOURCE_NAME" python3 - <<'PY'
import os, urllib.parse
print(urllib.parse.quote(os.environ["SOURCE_NAME"], safe=""))
PY
)"
URL="${OLR_NDI_TEST_URL:-ndi:${ENCODED_SOURCE}}"

echo "[ndi-e2e] source=${SOURCE_NAME} seconds=${SECONDS_TO_RECORD}"
OUT="$("$HARNESS" --url "$URL" --name olr_ndi_smoke --outdir "$WORKDIR" \
       --seconds "$SECONDS_TO_RECORD" --width 640 --height 480 --fps 30)"
RC=$?
OUT_MKV="$(printf '%s\n' "$OUT" | tail -n 1)"
echo "[ndi-e2e] harness rc=$RC out=$OUT_MKV"

if [ "$RC" -ne 0 ] || [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: NDI ingest produced no output (rc=$RC)"
    exit 1
fi

scalar() { ffprobe -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$OUT_MKV" | head -n1; }
V_PACKETS="$(scalar -select_streams v:0 -count_packets -show_entries stream=nb_read_packets)"
A_CHANNELS="$(scalar -select_streams a:0 -show_entries stream=channels)"
echo "[ndi-e2e] video_packets=${V_PACKETS:-?} audio_channels=${A_CHANNELS:-?}"

fail=0
MIN_FRAMES=$((30 * SECONDS_TO_RECORD / 2))
if ! is_num "${V_PACKETS:-}" || [ "${V_PACKETS%.*}" -lt "$MIN_FRAMES" ]; then
    echo "FAIL: too few/invalid video frames over NDI (got '${V_PACKETS:-none}', need >= ${MIN_FRAMES})"
    fail=1
fi
if [ "${A_CHANNELS:-}" != "2" ]; then
    echo "FAIL: expected stereo audio over NDI, got '${A_CHANNELS:-none}'"
    fail=1
fi

[ "$fail" -ne 0 ] && exit 1
echo "PASS: native NDI ingest — ${V_PACKETS} frames, stereo audio recorded from ${SOURCE_NAME}"
exit 0
