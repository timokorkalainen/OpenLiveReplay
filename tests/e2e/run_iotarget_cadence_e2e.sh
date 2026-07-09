#!/usr/bin/env bash
set -uo pipefail

HARNESS="${1:?iotarget_marker_sender executable required}"
KIND="${2:?target kind required (aja|omt|decklink-readback|decklink-gpu|decklink-st2110-readback|decklink-st2110-gpu)}"
FRAMES="${OLR_IOTARGET_FRAMES:-360}"

# Readback-path kinds (everything but the GPU-native *-gpu sinks) route frames
# through the async GPU readback ring, whose drain cadence is throughput-sensitive.
# CI's shared, virtualized GPU cannot sustain it reliably, so gate those variants to
# real hardware (the pre-push hook still runs them); GPU-native submit is unaffected.
case "$KIND" in
    *-gpu) : ;;
    *)
        if [ -n "${GITHUB_ACTIONS:-}" ]; then
            echo "SKIP: $KIND readback cadence is throughput-sensitive on CI's shared GPU"
            exit 77
        fi
        ;;
esac

OUT="$("$HARNESS" "$KIND" "$FRAMES")"
rc=$?
echo "$OUT"

[ "$rc" = "0" ] || { echo "FAIL: harness error ($rc)"; exit 1; }

line="$(grep '^IOTARGET ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no IOTARGET report"; exit 1; }

field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }

frames=$(field framesReceived)
maxgap=$(field maxGapFrames)
drops=$(field drops)
avsync=$(field avSyncMaxFrames)

fail=0
[ "${frames:-0}" -ge "$FRAMES" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${maxgap:-9}" -le 2 ] || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }
[ "${drops:-1}" = "0" ] || { echo "FAIL: drops=$drops"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] ||
    { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }

[ "$fail" = "0" ] && { echo "PASS: $KIND cadence/continuity OK"; exit 0; }
echo "IOTARGET VALIDATION FAILED"
exit 1
