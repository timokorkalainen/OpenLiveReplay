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

# Spawn ONE ffmpeg full-frame-flash producer, tee'd to all given UDP ports so every
# consumer sees byte-identical, simultaneous content. Luma flashes to white (235)
# for the first ~60ms of every source-second, else black (16).
# Usage: flash_marker_to_udps <udp_port> [<udp_port> ...]
flash_marker_to_udps() {
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    local tee="" p
    for p in "$@"; do
        [ -n "$tee" ] && tee="${tee}|"
        tee="${tee}[f=mpegts]udp://127.0.0.1:${p}?pkt_size=1316"
    done
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
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
