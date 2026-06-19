#!/usr/bin/env bash
# Rung-5 tier (a): send a marker stream through a real NdiOutputSink and verify the captured
# NDI output is continuous, A-V synced, and steady. Opt-in (CTest label "ndi-output").
# SKIPs (exit 77) when no NDI runtime/source is available.
#
# Usage: run_ndi_output_e2e.sh <ndi_output_sender_exe> <ndi_recv_probe_exe>
set -uo pipefail
SKIP=77

SENDER="${1:?ndi_output_sender executable required}"
PROBE="${2:?ndi_recv_probe executable required}"
SECONDS_RUN="${OLR_NDI_OUTPUT_SECONDS:-6}"
SRC="OLR NDI Output Probe $$"

SENDER_PID=""
cleanup() { [ -n "$SENDER_PID" ] && kill "$SENDER_PID" 2>/dev/null; wait "$SENDER_PID" 2>/dev/null; }
trap cleanup EXIT

# Start the sender in the background; give it a moment to register the source.
"$SENDER" "$SRC" "$((SECONDS_RUN + 3))" &
SENDER_PID=$!
sleep 1
# If the sender already exited 77 (no runtime), skip.
if ! kill -0 "$SENDER_PID" 2>/dev/null; then
    wait "$SENDER_PID"; rc=$?
    if [ "$rc" = "$SKIP" ]; then echo "SKIP: NDI runtime not available (sender)"; exit "$SKIP"; fi
    echo "FAIL: sender exited early ($rc)"; exit 1
fi

OUT="$("$PROBE" "$SRC" "$SECONDS_RUN")"
rc=$?
echo "$OUT"
if [ "$rc" = "$SKIP" ]; then echo "SKIP: NDI runtime/source not available (probe)"; exit "$SKIP"; fi
if [ "$rc" != "0" ]; then echo "FAIL: probe error ($rc)"; exit 1; fi

line="$(grep '^NDIRECV ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no NDIRECV report"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }

frames=$(field framesReceived); drops=$(field drops); dupes=$(field dupes)
reorders=$(field reorders); avsync=$(field avSyncMaxFrames); maxgap=$(field maxGapFrames)

fail=0
[ "${frames:-0}" -ge "$((SECONDS_RUN * 10))" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${drops:-1}" = "0" ]    || { echo "FAIL: drops=$drops"; fail=1; }
[ "${dupes:-1}" = "0" ]    || { echo "FAIL: dupes=$dupes"; fail=1; }
[ "${reorders:-1}" = "0" ] || { echo "FAIL: reorders=$reorders"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] || { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }
[ "${maxgap:-9}" -le 2 ]   || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }

if [ "$fail" = "0" ]; then echo "PASS: NDI output continuity/sync/cadence OK"; exit 0; fi
echo "NDI OUTPUT VALIDATION FAILED"; exit 1
