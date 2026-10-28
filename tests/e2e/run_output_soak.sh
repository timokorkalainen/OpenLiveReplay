#!/usr/bin/env bash
# Output-bus continuity soak driver: runs soak_harness and asserts the produced output
# stream followed the paused-output contract. Opt-in (CTest label "output-soak").
#
# Qt previews are intentionally suppressed during PausedPgmCadence. The external PGM target
# must continue to receive every tick, with repeated payloads proving the pause was exercised.
#
# Usage: run_output_soak.sh <soak_harness_exe>
# Env: OLR_SOAK_SECONDS (default 5 here for a fast opt-in run; raise for a real soak).
set -uo pipefail

HARNESS="${1:?soak_harness executable path required}"
export OLR_SOAK_SECONDS="${OLR_SOAK_SECONDS:-5}"

OUT="$("$HARNESS")"
status=$?
echo "$OUT"
if [ $status -ne 0 ]; then
    echo "FAIL: soak_harness exited $status"
    exit 1
fi

fail=0
field() { sed -n "s/.*$2=\\([0-9-]*\\).*/\\1/p" <<<"$1"; }

load_bus() {
    bus="$1"
    local line
    frames= gaps= seams= placeholders= repeated= ticks= playing_ticks= paused_ticks=
    line="$(grep "SOAK bus=$bus " <<<"$OUT" || true)"
    if [ -z "$line" ]; then echo "FAIL: missing report for bus=$bus"; fail=1; return 1; fi
    frames=$(field "$line" frames)
    gaps=$(field "$line" indexGaps)
    seams=$(field "$line" audioSeams)
    placeholders=$(field "$line" placeholders)
    repeated=$(field "$line" repeated)
    ticks=$(field "$line" ticks)
    playing_ticks=$(field "$line" playingTicks)
    paused_ticks=$(field "$line" pausedTicks)
}

check_common() {
    local bus="$1"
    [ "${seams:-1}" = "0" ]        || { echo "FAIL[$bus]: audioSeams=$seams"; fail=1; }
    [ "${placeholders:-1}" = "0" ] || { echo "FAIL[$bus]: placeholders=$placeholders"; fail=1; }
    [ "${frames:-0}" -gt 0 ]       || { echo "FAIL[$bus]: frames=$frames"; fail=1; }
    [ "${ticks:-0}" -gt 0 ]        || { echo "FAIL[$bus]: ticks=$ticks"; fail=1; }
    [ "${playing_ticks:-0}" -gt 0 ] || { echo "FAIL[$bus]: playingTicks=$playing_ticks"; fail=1; }
    [ "${paused_ticks:-0}" -gt 0 ]  || { echo "FAIL[$bus]: pausedTicks=$paused_ticks"; fail=1; }
    if [ "${ticks:-0}" -gt 0 ] && [ "${playing_ticks:-0}" -gt 0 ] && [ "${paused_ticks:-0}" -gt 0 ]; then
        [ "$(( playing_ticks + paused_ticks ))" = "${ticks}" ] || { echo "FAIL[$bus]: playingTicks=$playing_ticks pausedTicks=$paused_ticks ticks=$ticks"; fail=1; }
    fi
}

check_preview() {
    local requested_bus="$1"
    load_bus "$requested_bus" || return
    check_common "$requested_bus"
    [ "${repeated:-1}" = "0" ] || { echo "FAIL[$requested_bus]: repeated=$repeated (preview pause should be suppressed)"; fail=1; }
    [ "${gaps:-0}" -gt 0 ]      || { echo "FAIL[$requested_bus]: indexGaps=$gaps (no suppressed pause interval)"; fail=1; }
    if [ "${frames:-0}" -gt 0 ] && [ "${playing_ticks:-0}" -gt 0 ]; then
        [ "${frames}" = "${playing_ticks}" ] || { echo "FAIL[$requested_bus]: frames=$frames playingTicks=$playing_ticks (playing tick dropped or paused tick delivered)"; fail=1; }
    fi
}

check_pgm() {
    load_bus pgm || return
    check_common pgm
    [ "${repeated:-0}" -gt 0 ] || { echo "FAIL[pgm]: repeated=$repeated (no paused held-frame cadence)"; fail=1; }
    [ "${gaps:-1}" = "0" ]     || { echo "FAIL[pgm]: indexGaps=$gaps"; fail=1; }
    if [ "${frames:-0}" -gt 0 ] && [ "${ticks:-0}" -gt 0 ]; then
        [ "${frames}" = "${ticks}" ] || { echo "FAIL[pgm]: frames=$frames ticks=$ticks (lost cadence ticks)"; fail=1; }
    fi
}

check_preview feed
check_preview multiview
check_pgm

runtime_line="$(grep "^RUNTIME " <<<"$OUT" || true)"
if [ -z "$runtime_line" ]; then
    echo "FAIL: missing RUNTIME report line"; fail=1
else
    misses=$(field "$runtime_line" deadlineMisses)
    [ "${misses:-1}" = "0" ] || { echo "FAIL: deadlineMisses=$misses"; fail=1; }
fi

if [ "$fail" = "0" ]; then echo "PASS: output soak continuity OK"; exit 0; fi
echo "SOAK FAILED"
exit 1
