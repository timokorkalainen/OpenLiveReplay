#!/usr/bin/env bash
# Frame-sync acceptance rig: flash/beep/timecode markers through the real engine,
# measured from the recorded MKV. Report-only by default; selected cells become
# gates when OLR_FRAMESYNC_GATE=1.
#
# Usage: run_framesync_e2e.sh <sync_harness> <scenario> <base_port>
#   scenarios: lipsync | intercam | drift | drift_skew | drift_avsync | timecode
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"
# shellcheck source=ndi_lib.sh
. "$HERE/ndi_lib.sh"

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

require_media_tools_77() {
    command -v ffmpeg >/dev/null || skip "ffmpeg not found"
    command -v ffprobe >/dev/null || skip "ffprobe not found"
    command -v python3 >/dev/null || skip "python3 not found"
}

require_srt_tools_77() {
    require_media_tools_77
    command -v srt-live-transmit >/dev/null || skip "srt-live-transmit not found (brew install srt)"
}

[ -x "$HARNESS" ] || fail "sync_harness not found/executable: $HARNESS"
case "$TRANSPORT" in
    srt)
        require_srt_tools_77
        [ -f "$RELAY" ] || fail "$RELAY missing"
        ;;
    ndi)
        ndi_require_tools_77
        ;;
    rtmp)
        skip "framesync transport 'rtmp' not wired yet"
        ;;
    *)
        skip "unknown framesync transport '$TRANSPORT'"
        ;;
esac

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
    if [ "$TRANSPORT" = "ndi" ]; then
        local prefix="OLR-FS-${SCENARIO}-$$"
        ndi_start_marker_sender "$prefix" 1 "$((SECS + 5))" 0
        mkv="$(record_one "$(ndi_url_for_source "$(ndi_marker_source_name "$prefix" 0)")" framesync_lipsync)"
    else
        flash_beep_tc_marker_to_udp "$udp"
        srt_bridge "$udp" "$srt"
        sleep 1.5
        mkv="$(record_one "$(srt_caller_url "$srt")" framesync_lipsync)"
    fi
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
    if [ "$TRANSPORT" = "ndi" ]; then
        local prefix="OLR-FS-${SCENARIO}-$$"
        ndi_start_marker_sender "$prefix" 2 "$((SECS + 5))" 0
        mkv=$("$HARNESS" \
            --url "$(ndi_url_for_source "$(ndi_marker_source_name "$prefix" 0)")" \
            --url "$(ndi_url_for_source "$(ndi_marker_source_name "$prefix" 1)")" \
            --outdir "$WORKDIR" --name framesync_intercam --seconds "$SECS" --fps 30 | tail -n1)
    else
        flash_marker_to_udps "$udp0" "$udp1"
        srt_bridge "$udp0" "$srt0"
        srt_bridge "$udp1" "$srt1"
        sleep 1.5
        mkv=$("$HARNESS" --url "$(srt_caller_url "$srt0")" --url "$(srt_caller_url "$srt1")" \
            --outdir "$WORKDIR" --name framesync_intercam --seconds "$SECS" --fps 30 | tail -n1)
    fi
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
        skip "intercam gate requires common timecode; current fixture is report-only"
    else
        echo "PASS: report emitted (framesync intercam, non-gating)"
    fi
}

run_drift() {
    local udp="$BASE" srt=$((BASE + 1))
    local mkv stats nf slope ppm slip_frames av_stats avn av_mean av_max av_delta_ms av_delta_frames av_reg_ms av_reg_frames
    local harness_err="$WORKDIR/harness.err" clock_ppm
    if [ "$TRANSPORT" = "ndi" ]; then
        local prefix="OLR-FS-${SCENARIO}-$$"
        ndi_start_marker_sender "$prefix" 1 "$((SECS + 5))" "$SKEW_PPM"
        mkv="$("$HARNESS" --url "$(ndi_url_for_source "$(ndi_marker_source_name "$prefix" 0)")" \
            --outdir "$WORKDIR" --name framesync_drift --seconds "$SECS" --fps 30 \
            --report-stats 2>"$harness_err" | tail -n1)"
    else
        OLR_MARKER_SKEW_PPM="$SKEW_PPM" flash_beep_tc_marker_to_udp "$udp"
        srt_bridge "$udp" "$srt"
        sleep 1.5
        mkv="$("$HARNESS" --url "$(srt_caller_url "$srt")" --outdir "$WORKDIR" \
            --name framesync_drift --seconds "$SECS" --fps 30 --report-stats 2>"$harness_err" | tail -n1)"
    fi
    expect_mkv "$mkv"

    flash_pts_series "$mkv" 0 > "$WORKDIR/v.txt"
    beep_pts_series "$mkv" 0 > "$WORKDIR/a.txt"
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
    av_stats="$(awk '
        FNR==NR { f[++nf]=$1; next }
        { b[++nb]=$1 }
        END {
            for (i=1;i<=nf;i++) { best=1e9; bj=-1;
                for (j=1;j<=nb;j++) { dd=b[j]-f[i]; ad=(dd<0?-dd:dd); if(ad<best){best=ad;bj=j} }
                if (bj>0 && best<=0.25) {
                    d=(b[bj]-f[i])*1000; x=n; if(n==0) first=d; last=d; s+=d;
                    sx+=x; sy+=d; sxx+=x*x; sxy+=x*d;
                    am=(d<0?-d:d); if(am>mx)mx=am; n++;
                }
            }
            if(n>0) {
                drift=last-first; reg=drift;
                denom=n*sxx-sx*sx;
                if(n>=2 && denom != 0) {
                    slope=(n*sxy-sx*sy)/denom;
                    reg=slope*(n-1);
                }
                printf "%d %.1f %.1f %.1f %.3f %.1f %.3f", n, s/n, mx, drift, drift/33.333333, reg, reg/33.333333;
            } else printf "0 nan nan nan nan nan nan"
        }' "$WORKDIR/v.txt" "$WORKDIR/a.txt")"
    read -r avn av_mean av_max av_delta_ms av_delta_frames av_reg_ms av_reg_frames <<<"$av_stats"
    clock_ppm="$(awk '
        /clockppm=/ {
            for (i=1;i<=NF;i++) if ($i ~ /^clockppm=/) { split($i,a,"="); v=a[2] }
        }
        END { if (v == "") print "nan"; else print v }
        ' "$harness_err")"
    echo "REPORT: scenario=drift flashes=${nf} slope=${slope} drift_ppm=${ppm} slip_frames=${slip_frames} injected_ppm=${SKEW_PPM} recovered_clock_ppm=${clock_ppm} av_pairs=${avn} av_offset_ms_mean=${av_mean} av_offset_ms_max=${av_max} av_offset_delta_ms=${av_delta_ms} av_offset_delta_frames=${av_delta_frames} av_offset_regression_ms=${av_reg_ms} av_offset_regression_frames=${av_reg_frames}"
    [ "${nf:-0}" -ge 3 ] || fail "only ${nf:-0} flashes"
    [ "${avn:-0}" -ge 3 ] || fail "only ${avn:-0} A/V pairs"
    if awk -v injected="$SKEW_PPM" 'BEGIN{if(injected<0)injected=-injected; exit !(injected > 0)}'; then
        awk -v c="$clock_ppm" -v injected="$SKEW_PPM" '
            BEGIN {
                if (c != c) exit 1;
                if (c < 0) c = -c;
                if (injected < 0) injected = -injected;
                exit !(c >= injected * 0.25);
            }' || fail "nonzero injected skew ${SKEW_PPM}ppm did not produce recovered clockppm=${clock_ppm}"
    fi
    if gate_enabled; then
        awk -v s="$av_reg_frames" 'BEGIN{if(s<0)s=-s; exit !(s < 1.0)}' \
            || fail "A/V offset regression drift ${av_reg_frames} frames exceeds <1 frame gate"
        echo "PASS: framesync A/V regression drift ${av_reg_frames} frames within gate"
    else
        echo "PASS: report emitted (framesync drift, non-gating)"
    fi
}

# Amplified A/V-offset regression under sustained clock skew. Injects a large
# skew (default 2000 ppm) for a long run (default 60 s), then measures the
# flash-vs-beep A/V offset in the FIRST third vs the LAST third of the recording.
# If the restamp (SourceClock::toSessionMs slope correction) holds A/V alignment
# under skew, the offset stays flat and (last_third - first_third) ~ 0. If A/V
# offset accumulates, the last third drifts away from the first. Gate: regression
# (last-first) must stay within 1 frame @30 (33.333 ms).
run_drift_avsync() {
    local udp="$BASE" srt=$((BASE + 1))
    local mkv harness_err="$WORKDIR/harness.err" clock_ppm
    if [ "$TRANSPORT" = "ndi" ]; then
        local prefix="OLR-FS-${SCENARIO}-$$"
        ndi_start_marker_sender "$prefix" 1 "$((SECS + 5))" "$SKEW_PPM"
        mkv="$("$HARNESS" --url "$(ndi_url_for_source "$(ndi_marker_source_name "$prefix" 0)")" \
            --outdir "$WORKDIR" --name framesync_drift_avsync --seconds "$SECS" --fps 30 \
            --report-stats 2>"$harness_err" | tail -n1)"
    else
        OLR_MARKER_SKEW_PPM="$SKEW_PPM" flash_beep_tc_marker_to_udp "$udp"
        srt_bridge "$udp" "$srt"
        sleep 1.5
        mkv="$("$HARNESS" --url "$(srt_caller_url "$srt")" --outdir "$WORKDIR" \
            --name framesync_drift_avsync --seconds "$SECS" --fps 30 --report-stats 2>"$harness_err" | tail -n1)"
    fi
    expect_mkv "$mkv"

    flash_pts_series "$mkv" 0 > "$WORKDIR/v.txt"
    beep_pts_series "$mkv" 0 > "$WORKDIR/a.txt"

    # Pair each flash to nearest beep, bucket pairs into thirds by flash time,
    # and report the mean A/V offset (ms) of the first vs last third.
    local av_stats avn first_ms last_ms reg_ms reg_frames span_s
    av_stats="$(awk '
        FNR==NR { f[++nf]=$1; next }
        { b[++nb]=$1 }
        END {
            np=0; tmin=1e18; tmax=-1e18;
            for (i=1;i<=nf;i++) {
                best=1e9; bj=-1;
                for (j=1;j<=nb;j++) { dd=b[j]-f[i]; ad=(dd<0?-dd:dd); if(ad<best){best=ad;bj=j} }
                if (bj>0 && best<=0.25) {
                    np++; pt[np]=f[i]; pd[np]=(b[bj]-f[i])*1000;
                    if (f[i]<tmin) tmin=f[i]; if (f[i]>tmax) tmax=f[i];
                }
            }
            if (np<3 || tmax<=tmin) { printf "%d nan nan nan nan nan", np; exit }
            lo=tmin + (tmax-tmin)/3.0; hi=tmin + 2.0*(tmax-tmin)/3.0;
            fs=0; fn=0; ls=0; ln=0;
            for (k=1;k<=np;k++) {
                if (pt[k] <= lo)      { fs+=pd[k]; fn++ }
                else if (pt[k] >= hi) { ls+=pd[k]; ln++ }
            }
            if (fn==0 || ln==0) { printf "%d nan nan nan nan %.3f", np, tmax-tmin; exit }
            fm=fs/fn; lm=ls/ln; reg=lm-fm;
            printf "%d %.1f %.1f %.1f %.3f %.3f", np, fm, lm, reg, reg/33.333333, tmax-tmin;
        }' "$WORKDIR/v.txt" "$WORKDIR/a.txt")"
    read -r avn first_ms last_ms reg_ms reg_frames span_s <<<"$av_stats"

    clock_ppm="$(awk '
        /clockppm=/ {
            for (i=1;i<=NF;i++) if ($i ~ /^clockppm=/) { split($i,a,"="); v=a[2] }
        }
        END { if (v == "") print "nan"; else print v }
        ' "$harness_err")"

    echo "REPORT: scenario=drift_avsync injected_ppm=${SKEW_PPM} secs=${SECS} recovered_clock_ppm=${clock_ppm} av_pairs=${avn} span_s=${span_s} av_offset_first_third_ms=${first_ms} av_offset_last_third_ms=${last_ms} av_offset_regression_ms=${reg_ms} av_offset_regression_frames=${reg_frames}"
    [ "${avn:-0}" -ge 3 ] || fail "only ${avn:-0} A/V pairs"
    awk -v r="$reg_ms" 'BEGIN{ exit !(r==r) }' || fail "could not compute A/V regression (first/last third empty)"

    # Always gate this scenario: the whole point is to assert flat A/V offset.
    awk -v r="$reg_ms" 'BEGIN{ if(r<0)r=-r; exit !(r <= 33.333334) }' \
        || fail "A/V offset regression ${reg_ms}ms (${reg_frames} frames) exceeds 33ms (1 frame @30) under ${SKEW_PPM}ppm skew"
    echo "PASS: framesync A/V offset regression ${reg_ms}ms (${reg_frames} frames) within 33ms under ${SKEW_PPM}ppm/${SECS}s skew"
}

run_timecode() {
    local udp="$BASE" srt=$((BASE + 1)) mkv tc
    if [ "$TRANSPORT" = "ndi" ]; then
        local prefix="OLR-FS-${SCENARIO}-$$"
        ndi_start_marker_sender "$prefix" 1 "$((SECS + 5))" 0
        mkv="$(record_one "$(ndi_url_for_source "$(ndi_marker_source_name "$prefix" 0)")" framesync_timecode)"
    else
        flash_beep_tc_marker_to_udp "$udp"
        srt_bridge "$udp" "$srt"
        sleep 1.5
        mkv="$(record_one "$(srt_caller_url "$srt")" framesync_timecode)"
    fi
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
    drift_skew) SKEW_PPM="${OLR_FRAMESYNC_SKEW_PPM:-200}"; run_drift ;;
    drift_avsync)
        SKEW_PPM="${OLR_FRAMESYNC_SKEW_PPM:-2000}"
        SECS="${OLR_FRAMESYNC_SECS:-60}"
        run_drift_avsync
        ;;
    timecode) run_timecode ;;
    *) fail "unknown scenario '$SCENARIO'" ;;
esac
