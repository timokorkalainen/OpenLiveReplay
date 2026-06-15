#!/usr/bin/env bash
# Report-only frame-sync measurement driver.
#
# Generates synchronized synthetic sources, records them with sync_harness, and
# measures marker timing in the output MKV. Prints a scoreboard and ALWAYS exits
# 0 — this is a diagnostic, not a gate. See
# docs/superpowers/specs/2026-06-15-sync-measurement-harness-design.md.
#
# Usage: run_sync_e2e.sh <sync_harness> <scenario> <base_port> [--write-baseline]
#   scenarios: intercam_matched | intercam_skew | drift_2997 | lipsync
set -uo pipefail

HARNESS="${1:?sync_harness executable path required}"
SCENARIO="${2:?scenario required}"
BASE_PORT="${3:?base udp port required}"
WRITE_BASELINE="${4:-}"

command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Full-frame luma flash + (optional) co-timed beep, MPEG-TS over UDP, real-time.
# $1=port $2=rate(e.g. 30 or 30000/1001) $3=with_audio(0/1)
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
            -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
            -c:a aac -b:a 128k \
            -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    else
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "$vsrc" -vf "$vflt" \
            -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
            -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    fi
    PIDS+=($!)
}

url() { echo "udp://127.0.0.1:${1}?fifo_size=1000000&overrun_nonfatal=1"; }

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
    # One producer split to two ports via the tee muxer: byte-identical, same
    # PTS, simultaneous. Any measured offset is engine anchoring/mux, not source.
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -map 0:v \
        -f tee "[f=mpegts]udp://127.0.0.1:${P0}?pkt_size=1316|[f=mpegts]udp://127.0.0.1:${P1}?pkt_size=1316" &
    PIDS+=($!)
    sleep 0.5
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
    sleep 0.5
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
    # Proof that the per-source trim shifts a source by the set amount, in the
    # robust DELAY direction. ONE tee'd source feeds both views (identical content,
    # simultaneous), so the untrimmed inter-view offset is ≈0; a +TRIM on source 1
    # delays it, so the measured offset (view0-view1) shifts to ≈ -TRIM.
    TRIM_MS=${TRIM_MS:-250}
    P0=$BASE_PORT; P1=$((BASE_PORT+1))
    vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"

    # Records the tee'd 2-view setup with a given trim on source 1; echoes the
    # mean (view0-view1) ms, or "nan". Restarts a fresh producer each sub-run.
    measure_trim() {
        ffmpeg -hide_banner -loglevel error -re \
            -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
            -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
            -map 0:v \
            -f tee "[f=mpegts]udp://127.0.0.1:${P0}?pkt_size=1316|[f=mpegts]udp://127.0.0.1:${P1}?pkt_size=1316" &
        local pid=$!
        sleep 0.5
        local mkv
        mkv=$("$HARNESS" --url "$(url "$P0")" --url "$(url "$P1")" \
                --outdir "$WORKDIR" --name "trim_$1" --seconds 8 --fps 30 --trim "$1" | tail -n1)
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
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
    sleep 0.5
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
    # One source with a co-timed flash + beep every second. Measure audio-onset
    # minus video-flash PTS (signed ms). EBU R37 band is +40/-60 ms (context).
    P0=$BASE_PORT
    produce "$P0" 30 1
    sleep 0.5
    MKV=$("$HARNESS" --url "$(url "$P0")" \
            --outdir "$WORKDIR" --name lipsync --seconds 8 --fps 30 | tail -n1)
    if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then emit "[sync] scenario=lipsync ERROR=no_output"; echo "PASS: report emitted (diagnostic)"; exit 0; fi
    expect_video_tracks "$MKV" 1 || { emit "[sync] scenario=lipsync ERROR=wrong_track_count"; echo "PASS: report emitted (diagnostic)"; exit 0; }

    flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
    beep_pts_series  "$MKV" 0 > "$WORKDIR/a.txt"
    # Pair the k-th flash with the k-th beep; signed mean/max (audio - video) ms.
    STATS=$(paste "$WORKDIR/v.txt" "$WORKDIR/a.txt" | awk '
        NF==2 { d=($2-$1)*1000; s+=d; ad=(d<0?-d:d); if(ad>mx)mx=ad; n++ }
        END { if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan" }')
    read -r NP MEAN MAX <<<"$STATS"
    emit "[sync] scenario=lipsync pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37_band=+40/-60)"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;

  *)
    echo "ERROR: unknown scenario '$SCENARIO'"
    echo "PASS: report emitted (diagnostic)"
    exit 0
    ;;
esac
