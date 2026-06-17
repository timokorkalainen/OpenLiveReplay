#!/usr/bin/env bash
# Shared plumbing for the local SRT feature-validation e2e (Phase 2b), sourced by
# run_srt_sync.sh / run_srt_trim.sh / run_srt_connect.sh.
#
# Provides: tool guards, an ffmpeg full-frame-flash marker producer tee'd to N UDP
# ports (byte-identical, coincident content), srt-live-transmit UDP->SRT listener
# bridges, SRT caller URLs, and the proven per-track flash-onset extractor (copied
# from run_sync_e2e.sh).
#
# Caller contract: declare `PIDS=()` and a cleanup trap that kills "${PIDS[@]}";
# the spawn helpers append PIDs to PIDS AND set $SRT_LAST_PID to the just-spawned
# PID (read it immediately, before the next spawn). Call srt_require_tools first.
# Targets localhost only.

SRT_LAST_PID=""

# SKIP (exit 0) unless ffmpeg/ffprobe/srt-live-transmit are all present.
srt_require_tools() {
    command -v ffmpeg            >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
    command -v ffprobe           >/dev/null || { echo "SKIP: ffprobe not found"; exit 0; }
    command -v srt-live-transmit >/dev/null || { echo "SKIP: srt-live-transmit not found (brew install srt)"; exit 0; }
}

# SRT caller URL for a local listener port. $1=srt_port
srt_caller_url() { echo "srt://127.0.0.1:${1}?transtype=live"; }

# Spawn one srt-live-transmit UDP->SRT listener bridge. $1=udp_port $2=srt_port
srt_bridge() {
    local udp_port="$1" srt_port="$2"
    srt-live-transmit "udp://127.0.0.1:${udp_port}?mode=listener" \
        "srt://127.0.0.1:${srt_port}?mode=listener&transtype=live&latency=200" >/dev/null 2>&1 &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Populate SRT_HEVC_VCODEC_ARGS with an ffmpeg HEVC encoder + args (clean 30-frame
# GOP, no scenecut so the per-second flash always lands on a decodable picture).
# Prefers libx265 (deterministic), then a hardware HEVC encoder. Returns 1 (no args)
# when the local ffmpeg has no usable HEVC encoder, so callers can SKIP.
SRT_HEVC_VCODEC_ARGS=()
srt_hevc_vcodec_args() {
    local encoders enc
    encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
    if printf '%s\n' "$encoders" | grep -q ' libx265 '; then
        SRT_HEVC_VCODEC_ARGS=(-c:v libx265 -preset ultrafast
            -x265-params "log-level=error:keyint=30:min-keyint=30:scenecut=0"
            -pix_fmt yuv420p -b:v 4M)
        return 0
    fi
    for enc in hevc_videotoolbox hevc_nvenc hevc_qsv hevc_amf hevc_mf; do
        if printf '%s\n' "$encoders" | grep -q " ${enc} "; then
            if [ "$enc" = "hevc_videotoolbox" ]; then
                SRT_HEVC_VCODEC_ARGS=(-c:v "$enc" -allow_sw 1 -realtime 1 -g 30
                    -pix_fmt yuv420p -b:v 4M)
            else
                SRT_HEVC_VCODEC_ARGS=(-c:v "$enc" -g 30 -pix_fmt yuv420p -b:v 4M)
            fi
            return 0
        fi
    done
    return 1
}

# Spawn ONE ffmpeg full-frame-flash producer, tee'd to all given UDP ports so every
# consumer sees byte-identical, simultaneous content. Luma flashes to white (235)
# for the first ~60ms of every source-second, else black (16).
# Video codec follows OLR_FLASH_CODEC (default "avc" = H.264; "hevc" = H.265 via
# srt_hevc_vcodec_args, exits 77/SKIP if no HEVC encoder is available).
# Usage: flash_marker_to_udps <udp_port> [<udp_port> ...]
flash_marker_to_udps() {
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    local tee="" p
    for p in "$@"; do
        [ -n "$tee" ] && tee="${tee}|"
        tee="${tee}[f=mpegts]udp://127.0.0.1:${p}?pkt_size=1316"
    done
    local vargs
    if [ "${OLR_FLASH_CODEC:-avc}" = "hevc" ]; then
        srt_hevc_vcodec_args || { echo "SKIP: local ffmpeg has no HEVC encoder for the flash marker"; exit 77; }
        vargs=("${SRT_HEVC_VCODEC_ARGS[@]}")
    else
        vargs=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M)
    fi
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        "${vargs[@]}" \
        -map 0:v \
        -f tee "$tee" &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Rising-edge flash-onset pts_time series for one video track. $1=mkv $2=v-index.
# Emits one pts_time per flash, ascending. Detects only the full-white flash
# (YAVG>180, above the h264 cold-start gray ~128 and black base ~16).
flash_pts_series() {
    ffmpeg -hide_banner -loglevel error -i "$1" -map "0:v:$2" \
        -vf "signalstats,metadata=print:file=-" -f null - 2>/dev/null \
    | awk -v THRESH="${FLASH_THRESH:-180}" '
        /pts_time:/ { for (i=1;i<=NF;i++) if ($i ~ /^pts_time:/) { split($i,a,":"); t=a[2]+0 } }
        /YAVG=/     { split($0,b,"="); y=b[2]+0; bright=(y>THRESH);
                      if (bright && !prev) printf "%.6f\n", t; prev=bright }'
}

# Spawn ONE ffmpeg producer with a co-timed full-frame flash + 1kHz beep (first
# ~60ms of every source-second), MPEG-TS to a single UDP port. $1=udp_port
flash_beep_marker_to_udp() {
    local port="$1"
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -filter_complex "[0:v]${vflt}[v];[1:a]volume=volume='if(lt(mod(t,1),0.06),1,0)':eval=frame[a]" \
        -map "[v]" -map "[a]" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -c:a aac -b:a 128k \
        -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Spawn ONE ffmpeg producer with co-timed full-frame flash + 1kHz beep plus a
# burnt-in SMPTE timecode, MPEG-TS to a single UDP port. $1=udp_port
flash_beep_tc_marker_to_udp() {
    local port="$1"
    local tc="${OLR_MARKER_TC:-10\\:00\\:00\\:00}"
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128,drawtext=fontfile=:text='':timecode='${tc}':r=30:fontsize=20:fontcolor=white:x=10:y=10"
    local vargs
    if [ "${OLR_FLASH_CODEC:-avc}" = "hevc" ]; then
        srt_hevc_vcodec_args || { echo "SKIP: no HEVC encoder for TC marker"; exit 77; }
        vargs=("${SRT_HEVC_VCODEC_ARGS[@]}")
    else
        vargs=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M)
    fi
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -filter_complex "[0:v]${vflt}[v];[1:a]volume=volume='if(lt(mod(t,1),0.06),1,0)':eval=frame[a]" \
        -map "[v]" -map "[a]" "${vargs[@]}" \
        -c:a aac -b:a 128k -timecode "${OLR_MARKER_TC:-10:00:00:00}" \
        -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Echoes the MKV's start timecode (HH:MM:SS:FF) or empty.
mkv_start_timecode() {
    {
        ffprobe -v error -show_entries format_tags=timecode -of default=nk=1:nw=1 "$1" 2>/dev/null
        ffprobe -v error -show_entries stream_tags=timecode -of default=nk=1:nw=1 "$1" 2>/dev/null
    } | awk 'NF { print; exit }'
}

# Beep-onset pts_time series for one audio track (silence->sound rising edges).
# $1=mkv $2=audio-track-index.
beep_pts_series() {
    ffmpeg -hide_banner -loglevel info -i "$1" -map "0:a:$2" \
        -af "silencedetect=noise=-30dB:duration=0.03" -f null - 2>&1 \
    | awk '/silence_end:/ { for (i=1;i<=NF;i++) if ($i=="silence_end:") printf "%.6f\n", $(i+1) }'
}
