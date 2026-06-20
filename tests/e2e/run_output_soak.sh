#!/usr/bin/env bash
# Output-bus continuity soak driver: runs soak_harness and asserts the produced output
# stream stayed frame/audio continuous and held cadence. Opt-in (CTest label "output-soak").
#
# Identity-skip aware: the dispatcher drops byte-identical consecutive submits (default-on),
# so continuity is "every tick is delivered or skipped-as-duplicate" -> frames + repeated ==
# ticks, with repeated>0 proving the pause segments froze the payload. The sink's raw
# indexGaps just equals the skip count and is NOT gated.
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

check_bus() {
    local bus="$1" line
    line="$(grep "SOAK bus=$bus " <<<"$OUT" || true)"
    if [ -z "$line" ]; then echo "FAIL: missing report for bus=$bus"; fail=1; return; fi
    local frames gaps seams placeholders repeated ticks
    frames=$(field "$line" frames)
    gaps=$(field "$line" indexGaps)
    seams=$(field "$line" audioSeams)
    placeholders=$(field "$line" placeholders)
    repeated=$(field "$line" repeated)
    ticks=$(field "$line" ticks)
    [ "${seams:-1}" = "0" ]        || { echo "FAIL[$bus]: audioSeams=$seams"; fail=1; }
    [ "${placeholders:-1}" = "0" ] || { echo "FAIL[$bus]: placeholders=$placeholders"; fail=1; }
    [ "${repeated:-0}" -gt 0 ]     || { echo "FAIL[$bus]: repeated=$repeated (no pause detected)"; fail=1; }
    [ "${frames:-0}" -gt 0 ]       || { echo "FAIL[$bus]: frames=$frames"; fail=1; }
    [ "${ticks:-0}" -gt 0 ]        || { echo "FAIL[$bus]: ticks=$ticks"; fail=1; }
    # Identity-skip-aware continuity: every tick is delivered OR skipped-as-duplicate.
    if [ "${frames:-0}" -gt 0 ] && [ "${ticks:-0}" -gt 0 ]; then
        [ "$(( frames + repeated ))" = "${ticks}" ] || {
            echo "FAIL[$bus]: frames($frames)+repeated($repeated) != ticks($ticks) — lost ticks (raw indexGaps=$gaps)"; fail=1; }
    fi
}

check_bus feed
check_bus multiview
check_bus pgm

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
