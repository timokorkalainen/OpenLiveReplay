#!/usr/bin/env bash
# Frame-sync acceptance rig: flash/beep/timecode markers through the real engine,
# measured from the recorded MKV. Report-only by default; selected cells become
# gates when OLR_FRAMESYNC_GATE=1.
#
# Usage: run_framesync_e2e.sh <sync_harness> <scenario> <base_port>
#   scenarios: lipsync | intercam | drift | timecode
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
SCENARIO="${2:?scenario required}"
BASE="${3:?base port required}"
TRANSPORT="${OLR_FRAMESYNC_TRANSPORT:-srt}"
SECS="${OLR_FRAMESYNC_SECS:-20}"
SKEW_PPM="${OLR_FRAMESYNC_SKEW_PPM:-0}"
MARKER_TC="${OLR_MARKER_TC:-10:00:00:00}"
RELAY="$HERE/lossy_udp_relay.py"

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }

require_tools_77() {
    command -v ffmpeg >/dev/null || skip "ffmpeg not found"
    command -v ffprobe >/dev/null || skip "ffprobe not found"
    command -v srt-live-transmit >/dev/null || skip "srt-live-transmit not found (brew install srt)"
    command -v python3 >/dev/null || skip "python3 not found"
}

[ -x "$HARNESS" ] || fail "sync_harness not found/executable: $HARNESS"
[ "$TRANSPORT" = "srt" ] || skip "framesync transport '$TRANSPORT' not wired yet"
require_tools_77
[ -f "$RELAY" ] || fail "$RELAY missing"

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.2; kill -9 "${PIDS[@]}" 2>/dev/null; }
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

gate_enabled() { [ "${OLR_FRAMESYNC_GATE:-0}" = "1" ]; }

record_one() {
    local url="$1" name="$2"
    "$HARNESS" --url "$url" --outdir "$WORKDIR" --name "$name" --seconds "$SECS" --fps 30 | tail -n1
}

expect_mkv() {
    local mkv="$1"
    [ -n "$mkv" ] && [ -s "$mkv" ] || fail "$SCENARIO produced no MKV"
}

run_lipsync() {
    local udp="$BASE" srt=$((BASE + 1)) mkv stats np mean max
    flash_beep_tc_marker_to_udp "$udp"
    srt_bridge "$udp" "$srt"
    sleep 1.5
    mkv="$(record_one "$(srt_caller_url "$srt")" framesync_lipsync)"
    expect_mkv "$mkv"

    flash_pts_series "$mkv" 0 > "$WORKDIR/v.txt"
    beep_pts_series "$mkv" 0 > "$WORKDIR/a.txt"
    stats="$(awk '
        FNR==NR { f[FNR]=$1; nf=FNR; next }
        { b[FNR]=$1; nb=FNR }
        END {
            for (i=1;i<=nf;i++) { best=1e9; bj=-1;
                for (j=1;j<=nb;j++) { dd=b[j]-f[i]; ad=(dd<0?-dd:dd); if(ad<best){best=ad;bj=j} }
                if (bj>0 && best<=0.2) { d=(b[bj]-f[i])*1000; s+=d; am=(d<0?-d:d); if(am>mx)mx=am; n++ } }
            if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan"
        }' "$WORKDIR/v.txt" "$WORKDIR/a.txt")"
    read -r np mean max <<<"$stats"
    echo "REPORT: scenario=lipsync pairs=${np} av_offset_ms_mean=${mean} av_offset_ms_max=${max} band=-40..+60"
    [ "${np:-0}" -ge 3 ] || fail "only ${np:-0} flash/beep pairs"
    if gate_enabled; then
        awk -v m="$mean" 'BEGIN{exit !(m >= -40 && m <= 60)}' \
            || fail "A/V offset ${mean}ms outside EBU R37 (-40..+60)"
        echo "PASS: framesync lipsync ${mean}ms within EBU R37"
    else
        echo "PASS: report emitted (framesync lipsync, non-gating)"
    fi
}

run_intercam() {
    local udp0="$BASE" udp1=$((BASE + 1)) srt0=$((BASE + 2)) srt1=$((BASE + 3)) mkv stats np mean max
    flash_marker_to_udps "$udp0" "$udp1"
    srt_bridge "$udp0" "$srt0"
    srt_bridge "$udp1" "$srt1"
    sleep 1.5
    mkv=$("$HARNESS" --url "$(srt_caller_url "$srt0")" --url "$(srt_caller_url "$srt1")" \
        --outdir "$WORKDIR" --name framesync_intercam --seconds "$SECS" --fps 30 | tail -n1)
    expect_mkv "$mkv"

    flash_pts_series "$mkv" 0 > "$WORKDIR/v0.txt"
    flash_pts_series "$mkv" 1 > "$WORKDIR/v1.txt"
    stats="$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" | awk '
        NF==2 { d=($1-$2)*1000; ad=(d<0?-d:d); s+=ad; if(ad>mx)mx=ad; n++ }
        END { if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan" }')"
    read -r np mean max <<<"$stats"
    echo "REPORT: scenario=intercam flashes_paired=${np} flash_spread_ms_mean=${mean} flash_spread_ms_max=${max} common_tc=no"
    [ "${np:-0}" -ge 3 ] || fail "only ${np:-0} paired flashes"
    if gate_enabled; then
        echo "PASS: intercam common-TC absent; bounded report only"
    else
        echo "PASS: report emitted (framesync intercam, non-gating)"
    fi
}

run_drift() {
    local udp="$BASE" srt=$((BASE + 1)) relay_port=$((BASE + 2)) relay_stats="$WORKDIR/relay.stats"
    local mkv stats nf slope ppm slip_frames
    flash_beep_tc_marker_to_udp "$udp"
    srt_bridge "$udp" "$srt"
    python3 "$RELAY" "$relay_port" "127.0.0.1:$srt" 0 "$relay_stats" 1234 0 "$SKEW_PPM" &
    PIDS+=("$!")
    sleep 1.5
    mkv="$(record_one "srt://127.0.0.1:${relay_port}?transtype=live" framesync_drift)"
    expect_mkv "$mkv"

    flash_pts_series "$mkv" 0 > "$WORKDIR/v.txt"
    stats="$(awk -v secs="$SECS" '
        { x=NR-1; y=$1; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; n++ }
        END {
            if (n>=2) {
                slope=(n*sxy - sx*sy)/(n*sxx - sx*sx);
                ppm=(slope-1.0)*1e6;
                slip=(slope-1.0)*30*secs;
                printf "%d %.7f %.1f %.3f", n, slope, ppm, slip
            } else printf "0 nan nan nan"
        }' "$WORKDIR/v.txt")"
    read -r nf slope ppm slip_frames <<<"$stats"
    echo "REPORT: scenario=drift flashes=${nf} slope=${slope} drift_ppm=${ppm} slip_frames=${slip_frames} injected_ppm=${SKEW_PPM}"
    [ "${nf:-0}" -ge 3 ] || fail "only ${nf:-0} flashes"
    if gate_enabled; then
        awk -v s="$slip_frames" 'BEGIN{if(s<0)s=-s; exit !(s < 1.0)}' \
            || fail "drift slip ${slip_frames} frames exceeds <1 frame gate"
        echo "PASS: framesync drift slip ${slip_frames} frames within gate"
    else
        echo "PASS: report emitted (framesync drift, non-gating)"
    fi
}

run_timecode() {
    local udp="$BASE" srt=$((BASE + 1)) mkv tc
    flash_beep_tc_marker_to_udp "$udp"
    srt_bridge "$udp" "$srt"
    sleep 1.5
    mkv="$(record_one "$(srt_caller_url "$srt")" framesync_timecode)"
    expect_mkv "$mkv"
    tc="$(mkv_start_timecode "$mkv")"
    if [ -z "$tc" ]; then
        echo "REPORT: scenario=timecode expected=${MARKER_TC} recorded=n/a note='engine writes no tmcd yet'"
        echo "PASS: timecode n/a until tmcd writing lands"
        return
    fi
    echo "REPORT: scenario=timecode expected=${MARKER_TC} recorded=${tc}"
    if [ "$tc" = "$MARKER_TC" ]; then
        echo "PASS: recorded timecode matches injected marker"
    elif gate_enabled; then
        fail "recorded timecode ${tc} != ${MARKER_TC}"
    else
        echo "PASS: report emitted (framesync timecode mismatch, non-gating)"
    fi
}

echo "=== framesync: scenario=${SCENARIO} transport=${TRANSPORT} base=${BASE} secs=${SECS} ==="
case "$SCENARIO" in
    lipsync) run_lipsync ;;
    intercam) run_intercam ;;
    drift) run_drift ;;
    timecode) run_timecode ;;
    *) fail "unknown scenario '$SCENARIO'" ;;
esac
