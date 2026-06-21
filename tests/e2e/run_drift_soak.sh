#!/usr/bin/env bash
# Long-run clock-skew stability soak (T3.4).
#
# A real two-machine drift rig is out of scope: on a single host the source and the recorder share
# one wall clock, so genuine cross-machine drift is ~0 by construction and absolute clock accuracy
# cannot be measured here (see run_srt_soak.sh). What this DOES is run the real engine (sync_harness)
# for a long duration against a source whose MEDIA clock is skewed by a fixed ppm (OLR_MARKER_SKEW_PPM
# -> ffmpeg setpts=PTS*(1+ppm/1e6); delivery stays real-time), and gate the behavior that IS
# observable on one machine over a long run:
#   1. CONTINUITY   — the skewed stream records without stalls for the whole duration (a flash a
#                     second lands; no multi-second inter-flash gap).
#   2. A/V LIP-SYNC — the flash-vs-beep offset stays locked: its first-third vs last-third mean does
#                     not drift apart. Audio and video are stamped through the SAME
#                     SourceClock::toSessionMs anchor+slope, so a shared-clock skew is common-mode and
#                     cancels in the A/V offset; this gate therefore catches a PATH-SPLIT regression
#                     (audio and video diverging onto different clocks/anchors — the lip-sync bug
#                     class) holding up under sustained skew, NOT absolute drift-correction accuracy.
#
# Reported but NOT gated: recovered_clock_ppm and source_slip_frames. Over loopback the ingest
# timestamps on arrival, so the recovered ppm is noisy/sign-unstable and the recorded slip is a
# -re-pacing artifact, not a clean measure of the injected skew or of the engine's correction —
# gating either would be vacuous or flaky. They are emitted for diagnosis only.
#
# Opt-in: SKIPs (exit 77) unless OLR_DRIFT_RUN_SOAK=1, since a meaningful run is minutes. Best run on
# a quiesced host (the timing gates can false-fail under heavy concurrent CPU load).
#
# Usage: run_drift_soak.sh <sync_harness> <base_port>
# Env:
#   OLR_DRIFT_RUN_SOAK=1     enable (else SKIP 77)
#   OLR_DRIFT_SOAK_SECS=300  recording duration (s)
#   OLR_DRIFT_SOAK_PPM=300   injected source-clock skew (ppm, may be negative)
#   OLR_DRIFT_AV_FRAMES=2.0  max |A/V offset regression| (first vs last third), in frames @30
#                            (quiesced runs measure <1 frame; the default carries headroom for the
#                             beep-onset jitter + CPU contention a long soak is exposed to)
#   OLR_DRIFT_MAX_GAP=3.0    max inter-flash gap (s) before the stream is deemed stalled. A missed
#                            1 Hz flash (routine: detection misses ~one white frame per run) is ~2 s,
#                            so the floor tolerates a dropped marker or two; a real stall is many s.
set -uo pipefail
# Pin the numeric locale: awk's printf "%.Nf" (the skew factor fed into ffmpeg's setpts, and the
# slope/gap/regression values round-tripped through the shell into the gate comparisons) must use a
# dot decimal. A comma-decimal LC_NUMERIC otherwise breaks the setpts filter and corrupts the gates.
export LC_ALL=C

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:?base port required}"
SECS="${OLR_DRIFT_SOAK_SECS:-300}"
SKEW_PPM="${OLR_DRIFT_SOAK_PPM:-300}"
AV_FRAMES="${OLR_DRIFT_AV_FRAMES:-2.0}"
MAX_GAP="${OLR_DRIFT_MAX_GAP:-3.0}"

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }

[ "${OLR_DRIFT_RUN_SOAK:-0}" = "1" ] || skip "drift soak is opt-in (set OLR_DRIFT_RUN_SOAK=1)"
[ -x "$HARNESS" ] || fail "sync_harness not found/executable: $HARNESS"
command -v ffmpeg >/dev/null || skip "ffmpeg not found"
command -v ffprobe >/dev/null || skip "ffprobe not found"
command -v srt-live-transmit >/dev/null || skip "srt-live-transmit not found (brew install srt)"
olr_ffmpeg_has_filter volume || skip "ffmpeg volume filter not available"
olr_ffmpeg_has_filter silencedetect || skip "ffmpeg silencedetect filter not available"
awk -v p="$SKEW_PPM" 'BEGIN { if (p < 0) p = -p; exit !(p > 0) }' \
    || fail "OLR_DRIFT_SOAK_PPM must be nonzero (got '$SKEW_PPM')"

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.2; kill -9 "${PIDS[@]}" 2>/dev/null; }
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

UDP="$BASE"; SRT=$((BASE + 1))
HARNESS_ERR="$WORKDIR/harness.err"
echo "[drift-soak] secs=$SECS injected_ppm=$SKEW_PPM av_frames=$AV_FRAMES max_gap=$MAX_GAP port=$BASE"

# Skewed flash+beep source -> SRT bridge -> real engine recording for the full duration.
OLR_MARKER_SKEW_PPM="$SKEW_PPM" flash_beep_marker_to_udp "$UDP"
srt_bridge "$UDP" "$SRT"
sleep 1.5
MKV="$("$HARNESS" --url "$(srt_caller_url "$SRT")" --outdir "$WORKDIR" \
    --name drift_soak --seconds "$SECS" --fps 30 --report-stats 2>"$HARNESS_ERR" | tail -n1)"
[ -n "$MKV" ] && [ -s "$MKV" ] || fail "no output mkv (harness tail: $(tail -n3 "$HARNESS_ERR" | tr '\n' ' '))"

flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
beep_pts_series  "$MKV" 0 > "$WORKDIR/a.txt"

# Flash cadence: count, source-clock slope + slip (reported diagnostics) and the gated max gap.
# On too-few flashes emit a large numeric gap sentinel so the gap gate fails closed (never "nan").
FL="$(awk -v secs="$SECS" '
    { x=NR-1; y=$1; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; n++;
      if (NR>1) { d=y-py; if (d>mxgap) mxgap=d } py=y }
    END {
        if (n>=2) { slope=(n*sxy-sx*sy)/(n*sxx-sx*sx);
                    printf "%d %.7f %.1f %.3f %.3f", n, slope, (slope-1)*1e6, (slope-1)*30*secs, mxgap }
        else printf "%d 1.0 0.0 0.0 99.000", n+0
    }' "$WORKDIR/v.txt")"
read -r NF_FLASH SLOPE SLOPE_PPM SLIP_FRAMES MAXGAP <<<"$FL"

# Recovered clock ppm (final telemetry snapshot; reported, not gated).
CLOCK_PPM="$(awk '
    /clockppm=/ { for (i=1;i<=NF;i++) if ($i ~ /^clockppm=/) { split($i,a,"="); v=a[2] } }
    END { if (v == "") print "nan"; else print v }' "$HARNESS_ERR")"

# A/V offset regression: pair each flash to the nearest beep (<=0.25 s), bucket pairs into thirds by
# flash time, compare the mean offset of the last third vs the first third. On a degenerate set
# (too few pairs / an empty third) emit a large numeric regression sentinel so the A/V gate fails
# closed rather than passing on an uncomputable value (never "nan").
AV="$(awk '
    FNR==NR { f[++nf]=$1; next }
    { b[++nb]=$1 }
    END {
        np=0; tmin=1e18; tmax=-1e18;
        for (i=1;i<=nf;i++) {
            best=1e9; bj=-1;
            for (j=1;j<=nb;j++) { dd=b[j]-f[i]; ad=(dd<0?-dd:dd); if (ad<best){best=ad;bj=j} }
            if (bj>0 && best<=0.25) { np++; pt[np]=f[i]; pd[np]=(b[bj]-f[i])*1000;
                                      if (f[i]<tmin) tmin=f[i]; if (f[i]>tmax) tmax=f[i] }
        }
        if (np<6 || tmax<=tmin) { printf "%d 0.0 0.0 9999.0 99.000 0.000", np; exit }
        lo=tmin+(tmax-tmin)/3.0; hi=tmin+2.0*(tmax-tmin)/3.0;
        fs=0; fn=0; ls=0; ln=0;
        for (k=1;k<=np;k++) { if (pt[k]<=lo){fs+=pd[k];fn++} else if (pt[k]>=hi){ls+=pd[k];ln++} }
        if (fn==0 || ln==0) { printf "%d 0.0 0.0 9999.0 99.000 %.3f", np, tmax-tmin; exit }
        fm=fs/fn; lm=ls/ln; reg=lm-fm;
        printf "%d %.1f %.1f %.1f %.3f %.3f", np, fm, lm, reg, reg/33.333333, tmax-tmin
    }' "$WORKDIR/v.txt" "$WORKDIR/a.txt")"
read -r AV_PAIRS AV_FIRST_MS AV_LAST_MS AV_REG_MS AV_REG_FRAMES AV_SPAN_S <<<"$AV"

echo "REPORT: drift_soak secs=${SECS} injected_ppm=${SKEW_PPM} flashes=${NF_FLASH} max_flash_gap_s=${MAXGAP} slope=${SLOPE} source_slope_ppm=${SLOPE_PPM} source_slip_frames=${SLIP_FRAMES} recovered_clock_ppm=${CLOCK_PPM} av_pairs=${AV_PAIRS} av_span_s=${AV_SPAN_S} av_offset_first_third_ms=${AV_FIRST_MS} av_offset_last_third_ms=${AV_LAST_MS} av_offset_regression_ms=${AV_REG_MS} av_offset_regression_frames=${AV_REG_FRAMES}"

# --- Gates ---------------------------------------------------------------------------
# 1. Continuity: near-complete marker delivery (a flash a second), and no multi-second stall.
MIN_FLASH=$(awk -v s="$SECS" 'BEGIN{ printf "%d", (s * 0.8) }')
[ "${NF_FLASH:-0}" -ge "$MIN_FLASH" ] || fail "only ${NF_FLASH:-0} flashes over ${SECS}s (need >= ${MIN_FLASH} = 0.8*secs)"
[ "${AV_PAIRS:-0}" -ge 6 ] || fail "only ${AV_PAIRS:-0} A/V pairs"
awk -v g="${MAXGAP:-99}" -v lim="$MAX_GAP" 'BEGIN{ exit !((g+0) <= (lim+0)) }' \
    || fail "max inter-flash gap ${MAXGAP}s exceeds ${MAX_GAP}s (stream stalled under skew)"

# 2. A/V lip-sync lock: the flash-vs-beep offset must not drift apart over the run (a path-split
#    onto separate audio/video clocks would make the last third diverge from the first).
awk -v f="${AV_REG_FRAMES:-99}" -v lim="$AV_FRAMES" 'BEGIN{ f=f+0; if (f<0) f=-f; exit !(f <= (lim+0)) }' \
    || fail "A/V offset regression ${AV_REG_FRAMES} frames (${AV_REG_MS}ms) exceeds ${AV_FRAMES} frame over ${SECS}s at ${SKEW_PPM}ppm"

echo "PASS: drift soak ${SECS}s @ ${SKEW_PPM}ppm — ${NF_FLASH} flashes (gap<=${MAXGAP}s), A/V lip-sync regression ${AV_REG_FRAMES} fr within ${AV_FRAMES} (source slip ${SLIP_FRAMES} fr, recovered ppm ${CLOCK_PPM} reported only)"
