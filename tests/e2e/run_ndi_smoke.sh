#!/usr/bin/env bash
# Opt-in native NDI ingest smoke. Requires a real NDI source and runtime.
#
# Usage: OLR_NDI_TEST_SOURCE='Studio (CAM1)' run_ndi_smoke.sh <record_harness>
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
SECONDS_TO_RECORD="${OLR_NDI_SMOKE_SECS:-6}"

skip() { echo "SKIP: $*"; exit 77; }
is_num() { case "${1:-}" in '' | *[!0-9.]*) return 1 ;; *) return 0 ;; esac; }

[ -x "$HARNESS" ] || { echo "FAIL: record_harness not executable: $HARNESS"; exit 1; }
command -v ffprobe >/dev/null || skip "ffprobe not found"
command -v python3 >/dev/null || skip "python3 not found"
[ -n "${OLR_NDI_TEST_SOURCE:-}" ] || skip "OLR_NDI_TEST_SOURCE not set"

ENCODED_SOURCE="$(python3 - <<'PY'
import os, urllib.parse
print(urllib.parse.quote(os.environ["OLR_NDI_TEST_SOURCE"], safe=""))
PY
)"
URL="${OLR_NDI_TEST_URL:-ndi:${ENCODED_SOURCE}}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "[ndi-e2e] source=${OLR_NDI_TEST_SOURCE} seconds=${SECONDS_TO_RECORD}"
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
echo "PASS: native NDI ingest — ${V_PACKETS} frames, stereo audio recorded from ${OLR_NDI_TEST_SOURCE}"
exit 0
