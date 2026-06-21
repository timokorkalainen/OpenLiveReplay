#!/usr/bin/env bash
# Report-only frame-sync measurement driver.
#
# Generates synchronized synthetic sources, bridges each over UDP->SRT
# (srt-live-transmit) so the engine ingests them over its native srt:// transport
# (the only ingest path now — udp:// is no longer accepted), records them with
# sync_harness, and measures marker timing in the output MKV. Prints a scoreboard
# and ALWAYS exits 0 — this is a diagnostic, not a gate. See
# docs/superpowers/specs/2026-06-15-sync-measurement-harness-design.md.
#
# Usage: run_sync_e2e.sh <sync_harness> <scenario> <base_port> [--write-baseline]
#   scenarios: intercam_matched | intercam_skew | drift_2997 | lipsync
#   base_port is the base UDP producer port; each source's SRT listener is
#   bridged at UDP+100 (see srt_port()).
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
SCENARIO="${2:?scenario required}"
BASE_PORT="${3:?base udp port required}"
WRITE_BASELINE="${4:-}"

# shellcheck source=tests/e2e/srt_lib.sh
. "$(cd "$(dirname "$0")" && pwd)/srt_lib.sh"
srt_require_tools  # SKIP (exit 0) unless ffmpeg/ffprobe/srt-live-transmit present
olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# SRT listener port for a UDP producer port (UDP+100; see header). Keeps the SRT
# ports out of the packed UDP base band and clear of the other e2e gates' ports.
srt_port() { echo $(( $1 + 100 )); }

# Full-frame luma flash + (optional) co-timed beep, MPEG-TS over UDP, real-time,
# bridged to an SRT listener (srt-live-transmit) so the engine ingests over srt://.
# $1=udp_port $2=rate(e.g. 30 or 30000/1001) $3=with_audio(0/1)
produce() {
    local port="$1" rate="$2" with_audio="$3"
    local vsrc="color=c=black:s=320x240:r=${rate}"
    # geq luma: white (235) for the first ~60ms of every source-second, else 16.
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    if [ "$with_audio" = "1" ]; then
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "$vsrc" \
            -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
            -filter_complex "[0:v]${vflt}[v];[1:a]volume=volume='if(lt(mod(t,1),0.06),1,0)':eval=frame[a]" \
            -map "[v]" -map "[a]" \
            "${OLR_H264_VCODEC_ARGS[@]}" \
            -c:a aac -b:a 128k \
            -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    else
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "$vsrc" -vf "$vflt" \
            "${OLR_H264_VCODEC_ARGS[@]}" \
            -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    fi
    PIDS+=($!)
    srt_bridge "$port" "$(srt_port "$port")"  # UDP MPEG-TS -> SRT listener
}

# SRT caller URL for the SRT listener bridged from a given UDP producer port.
url() { srt_caller_url "$(srt_port "$1")"; }

# Rising-edge flash-onset pts_time series for one video track.
# $1=mkv $2=video-track-index. Emits one pts_time per flash, ascending.
# Detects only the full-white flash (YAVG~235); the default threshold (180)
# sits above the h264 cold-start gray (YAVG~128) and black base (YAVG~16).
flash_pts_series() {
    ffmpeg -hide_banner -loglevel error -i "$1" -map "0:v:$2" \
        -vf "signalstats,metadata=print:file=-" -f null - 2>/dev/null \
    | awk -v THRESH="${FLASH_THRESH:-180}" '
        /pts_time:/ { for (i=1;i<=NF;i++) if ($i ~ /^pts_time:/) { split($i,a,":"); t=a[2]+0 } }
        /YAVG=/     { split($0,b,"="); y=b[2]+0; bright=(y>THRESH);
                      if (bright && !prev) printf "%.6f\n", t; prev=bright }'
}

# Beep-onset pts_time series for one audio track (silence->sound rising edges).
# $1=mkv $2=audio-track-index.
beep_pts_series() {
    ffmpeg -hide_banner -loglevel info -i "$1" -map "0:a:$2" \
        -af "silencedetect=noise=-30dB:duration=0.03" -f null - 2>&1 \
    | awk '/silence_end:/ { for (i=1;i<=NF;i++) if ($i=="silence_end:") printf "%.6f\n", $(i+1) }'
}

# Assert N video tracks exist; report-only (never fails the run).
expect_video_tracks() {
    local mkv="$1" want="$2"
    local got
    got=$(ffprobe -v error -select_streams v -show_entries stream=index -of csv=p=0 "$mkv" | wc -l | tr -d ' ')
    if [ "${got:-0}" != "$want" ]; then
        echo "ERROR: expected $want video tracks, got ${got:-0}"
        return 1
    fi
    return 0
}

# Append a scoreboard line to SYNC_BASELINE.md when --write-baseline is given.
emit() {
    local line="$1"
    echo "$line"
    if [ "$WRITE_BASELINE" = "--write-baseline" ]; then
        local base_md
        base_md="$(cd "$(dirname "$0")" && pwd)/SYNC_BASELINE.md"
        printf '%s\n' "$line" >> "$base_md"
    fi
}

echo "=== sync scoreboard: ${SCENARIO} ==="

case "$SCENARIO" in
  intercam_matched)
    olr_ffmpeg_has_muxer tee || { echo "SKIP: ffmpeg tee muxer not available"; exit 0; }
    # One producer split to two ports via the tee muxer: byte-identical, same
    # PTS, simultaneous. Any measured offset is engine anchoring/mux, not source.
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        "${OLR_H264_VCODEC_ARGS[@]}" \
        -map 0:v \
        -f tee "[f=mpegts]udp://127.0.0.1:${P0}?pkt_size=1316|[f=mpegts]udp://127.0.0.1:${P1}?pkt_size=1316" &
    PIDS+=($!)
    srt_bridge "$P0" "$(srt_port "$P0")"  # UDP MPEG-TS -> SRT listener (view 0)
    srt_bridge "$P1" "$(srt_port "$P1")"  # UDP MPEG-TS -> SRT listener (view 1)
    sleep 1.0
    MKV=$("$HARNESS" --url "$(url "$P0")" --url "$(url "$P1")" \
            --outdir "$WORKDIR" --name intercam_matched --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=intercam_matched ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 2 || { emit "[sync] scenario=intercam_matched ERROR=wrong_track_count"; echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
    # Pair flashes by index; report mean/max |Δ| in ms.
    STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" | awk '
        NF==2 { d=($1-$2)*1000; ad=(d<0?-d:d); s+=ad; if(ad>mx)mx=ad; n++ }
        END { if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan" }')
    read -r NP MEAN MAX <<<"$STATS"
    emit "[sync] scenario=intercam_matched flashes_paired=${NP} intercam_offset_ms: mean=${MEAN} max=${MAX}"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  intercam_skew)
    # Two independent producers; source B started D ms after A models a
    # camera whose timeline is offset by D. The engine anchors each to its own
    # first-packet arrival, so it bakes the offset in (no shared reference).
    D_MS=${SKEW_MS:-250}
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    produce "$P0" 30 0
    sleep "$(awk -v d="$D_MS" 'BEGIN{printf "%.3f", d/1000}')"
    produce "$P1" 30 0
    sleep 1.0
    MKV=$("$HARNESS" --url "$(url "$P0")" --url "$(url "$P1")" \
            --outdir "$WORKDIR" --name intercam_skew --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=intercam_skew ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 2 || { emit "[sync] scenario=intercam_skew ERROR=wrong_track_count"; echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    flash_pts_series "$MKV" 1 > "$WORKDIR/v1.txt"
    # Index-pair flashes; signed mean Δ (view0 - view1) and stdev, in ms.
    STATS=$(paste "$WORKDIR/v0.txt" "$WORKDIR/v1.txt" | awk '
        NF==2 { d=($1-$2)*1000; s+=d; ss+=d*d; n++ }
        END { if(n>0){ m=s/n; v=ss/n-m*m; if(v<0)v=0; printf "%d %.1f %.1f", n, m, sqrt(v) } else printf "0 nan nan" }')
    read -r NP MEAN STDEV <<<"$STATS"
    emit "[sync] scenario=intercam_skew flashes_paired=${NP} intercam_offset_ms: mean=${MEAN} stdev=${STDEV} (D_injected=${D_MS})"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  intercam_trim)
    olr_ffmpeg_has_muxer tee || { echo "SKIP: ffmpeg tee muxer not available"; exit 0; }
    # Proof that the per-source trim shifts a source by the set amount, in the
    # robust DELAY direction. ONE tee'd source feeds both views (identical content,
    # simultaneous), so the untrimmed inter-view offset is ≈0; a +TRIM on source 1
    # delays it, so the measured offset (view0-view1) shifts to ≈ -TRIM.
    TRIM_MS=${TRIM_MS:-250}
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"

    # Records the tee'd 2-view setup with a given trim on source 1; echoes the
    # mean (view0-view1) ms, or "nan". Restarts a fresh producer + SRT bridges
    # each sub-run (so the SRT listeners are re-armed for the fresh caller).
    measure_trim() {
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
            "${OLR_H264_VCODEC_ARGS[@]}" \
            -map 0:v \
            -f tee "[f=mpegts]udp://127.0.0.1:${P0}?pkt_size=1316|[f=mpegts]udp://127.0.0.1:${P1}?pkt_size=1316" &
        local pid=$!
        PIDS+=("$pid")
        srt_bridge "$P0" "$(srt_port "$P0")"; local b0=$SRT_LAST_PID
        srt_bridge "$P1" "$(srt_port "$P1")"; local b1=$SRT_LAST_PID
        sleep 1.0
        local mkv
        mkv=$("$HARNESS" --url "$(url "$P0")" --url "$(url "$P1")" \
                --outdir "$WORKDIR" --name "trim_$1" --seconds 8 --fps 30 --trim "$1" | tail -n1)
        kill "$pid" "$b0" "$b1" 2>/dev/null; wait "$pid" "$b0" "$b1" 2>/dev/null
        if [ -z "$mkv" ] || [ ! -s "$mkv" ]; then echo "nan"; return; fi
        flash_pts_series "$mkv" 0 > "$WORKDIR/t0.txt"
        flash_pts_series "$mkv" 1 > "$WORKDIR/t1.txt"
        paste "$WORKDIR/t0.txt" "$WORKDIR/t1.txt" | awk '
            NF==2 { d=($1-$2)*1000; s+=d; n++ }
            END { if(n>0) printf "%.1f", s/n; else printf "nan" }'
    }

    BASE=$(measure_trim 0)
    TRIMMED=$(measure_trim "$TRIM_MS")
    emit "[sync] scenario=intercam_trim untrimmed_ms=${BASE} trimmed_ms=${TRIMMED} (trim_applied=${TRIM_MS}; delay => trimmed ≈ untrimmed − ${TRIM_MS})"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  drift_2997)
    # One source paced at 29.97 (30000/1001) while the session runs int fps=30.
    # Flash every source-second; measure how the flash PTS slope deviates from
    # 1.0 (drift) over a long run. Quantifies FRAC-1.
    P0=$BASE_PORT
    DUR=${DRIFT_SECONDS:-60}
    produce "$P0" "30000/1001" 0
    sleep 1.0
    MKV=$("$HARNESS" --url "$(url "$P0")" \
            --outdir "$WORKDIR" --name drift_2997 --seconds "$DUR" --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=drift_2997 ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 1 || { emit "[sync] scenario=drift_2997 ERROR=wrong_track_count"; echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v0.txt"
    # Least-squares slope of (flash_pts vs flash_index); index k=0,1,2,...
    # Each flash is one source-second apart, so an ideal timeline has slope 1.0.
    STATS=$(awk '
        { x=NR-1; y=$1; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; n++ }
        END {
            if (n>=2) {
                slope=(n*sxy - sx*sy)/(n*sxx - sx*sx);
                ppm=(slope-1.0)*1e6;
                slip=(slope-1.0)*30*(y);          # frames of slip over the run
                printf "%d %.6f %.0f %.2f", n, slope, ppm, slip
            } else printf "0 nan nan nan"
        }' "$WORKDIR/v0.txt")
    read -r NF SLOPE PPM SLIP <<<"$STATS"
    emit "[sync] scenario=drift_2997 flashes=${NF} slope=${SLOPE} drift_ppm=${PPM} drift_frames_slip=${SLIP}"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  lipsync)
    olr_ffmpeg_has_filter volume || { echo "SKIP: ffmpeg volume filter not available"; exit 0; }
    olr_ffmpeg_has_filter silencedetect || { echo "SKIP: ffmpeg silencedetect filter not available"; exit 0; }
    # One source with a co-timed flash + beep every second. Measure audio-onset
    # minus video-flash PTS (signed ms). EBU R37 band is +40/-60 ms (context).
    P0=$BASE_PORT
    produce "$P0" 30 1
    sleep 1.0
    MKV=$("$HARNESS" --url "$(url "$P0")" \
            --outdir "$WORKDIR" --name lipsync --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=lipsync ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 1 || { emit "[sync] scenario=lipsync ERROR=wrong_track_count"; echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
    beep_pts_series  "$MKV" 0 > "$WORKDIR/a.txt"
    # Pair each flash with its NEAREST beep within 200ms (flash+beep are co-timed per
    # source-second). Nearest-match (not index pairing) is robust to a spurious extra
    # silencedetect edge, which would otherwise slip every pair by ~1s. Signed mean/max
    # (audio - video) ms over matched pairs.
    STATS=$(awk '
        FNR==NR { f[FNR]=$1; nf=FNR; next }
        { b[FNR]=$1; nb=FNR }
        END {
            for (i=1;i<=nf;i++) { best=1e9; bj=-1;
                for (j=1;j<=nb;j++) { dd=b[j]-f[i]; ad=(dd<0?-dd:dd); if(ad<best){best=ad;bj=j} }
                if (bj>0 && best<=0.2) { d=(b[bj]-f[i])*1000; s+=d; am=(d<0?-d:d); if(am>mx)mx=am; n++ } }
            if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan"
        }' "$WORKDIR/v.txt" "$WORKDIR/a.txt")
    read -r NP MEAN MAX <<<"$STATS"
    emit "[sync] scenario=lipsync pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37_band=+40/-60)"
    if [ "${OLR_AV_SYNC_GATE:-0}" = "1" ]; then
        # Two gates over the same matched flash/beep pairs:
        #   1) MEAN gate (EBU R37): mean(audio_pts - video_pts) within -40..+60 ms.
        #      A positive offset means audio lags the picture. This catches a
        #      steady-state anchoring regression.
        #   2) MAX (worst-case) gate: the largest single-pair |audio - video|
        #      must stay <= OLR_AV_SYNC_MAX_MS (default 100). The mean can pass
        #      while one frame is badly out of sync (a dropped/duplicated audio
        #      span, a single mis-anchored picture); the mean averages that
        #      excursion away, so a per-pair worst-case bound is required to make
        #      a single out-of-bounds frame FAIL. 100 ms is ~3 frames @30 — wider
        #      than the mean band (so it is not redundant) yet tight enough to bite
        #      a genuine excursion, with headroom over the ~1-frame onset-detection
        #      quantization on each of the audio and video edges (~66 ms combined
        #      worst-case measurement noise).
        MAX_BOUND="${OLR_AV_SYNC_MAX_MS:-100}"
        if [ "${NP:-0}" -lt 3 ]; then
            echo "FAIL: only ${NP:-0} flash/beep pairs — measurement unreliable"; exit 1
        fi
        if ! awk -v m="$MEAN" 'BEGIN{exit !(m >= -40 && m <= 60)}'; then
            echo "FAIL: A/V mean offset ${MEAN}ms outside EBU R37 (-40..+60) — shared anchor regressed"
            exit 1
        fi
        if ! awk -v x="$MAX" -v b="$MAX_BOUND" 'BEGIN{exit !(x <= b)}'; then
            echo "FAIL: A/V worst-case offset ${MAX}ms exceeds MAX bound ${MAX_BOUND}ms — single-frame A/V excursion"
            exit 1
        fi
        echo "PASS: A/V mean ${MEAN}ms within EBU R37 (-40..+60) and max ${MAX}ms within ${MAX_BOUND}ms"
        exit 0
    fi
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  *)
    echo "ERROR: unknown scenario '$SCENARIO'"
    echo "PASS: report emitted (diagnostic)"
    exit 0
    ;;
esac
