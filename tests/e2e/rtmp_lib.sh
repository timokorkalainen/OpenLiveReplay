#!/usr/bin/env bash
# Shared plumbing for local native RTMP e2e gates.
#
# Provides: tool guards, deterministic FLV fixture generation, one-client RTMP
# fixture server startup, RTMP URLs, and flash-onset extraction. The RTMP fixture
# server paces tags by their FLV timestamps so the harness sees live-ish playback.

# shellcheck source=tool_env.sh
. "$HERE/tool_env.sh"
olr_prepend_built_tool_paths

RTMP_LAST_PID=""
RTMP_PYTHON_CMD=()

rtmp_find_python3() {
    local candidate
    for candidate in python3 python; do
        if command -v "$candidate" >/dev/null 2>&1 \
            && "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
            RTMP_PYTHON_CMD=("$candidate")
            return 0
        fi
    done
    if command -v py >/dev/null 2>&1 \
        && py -3 -c 'import sys; raise SystemExit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
        RTMP_PYTHON_CMD=(py -3)
        return 0
    fi
    return 1
}

rtmp_require_tools() {
    command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 77; }
    command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not found"; exit 77; }
    rtmp_find_python3 || { echo "SKIP: usable Python 3 not found"; exit 77; }
}

rtmp_redact_log() {
    sed -E \
        -e "s|([[:alpha:]][[:alnum:]+.-]*://)[^/@[:space:]'\"]+@|\\1<redacted>@|g" \
        -e "s|([?&][^=[:space:]'\"]+=)[^&[:space:]'\"#]+|\\1<redacted>|g" \
        -e "s|#[^[:space:]'\"]*|#<redacted>|g" "$@"
}

rtmp_redact_value() {
    printf '%s\n' "$1" | rtmp_redact_log
}

rtmp_url() {
    local url="${RTMP_SCHEME:-rtmp}://127.0.0.1:${1}/live/stream"
    if [ -n "${RTMP_URL_QUERY:-}" ]; then
        echo "${url}?${RTMP_URL_QUERY}"
    else
        echo "$url"
    fi
}

rtmp_fixture_server_cmd() {
    if [ -n "${RTMP_FIXTURE_OUT_CHUNK_SIZE:-}" ]; then
        set -- "$@" --out-chunk-size "$RTMP_FIXTURE_OUT_CHUNK_SIZE"
    fi
    if [ -n "${RTMP_FIXTURE_WRITE_FRAGMENT:-}" ]; then
        set -- "$@" --write-fragment "$RTMP_FIXTURE_WRITE_FRAGMENT"
    fi
    if [ -n "${RTMP_FIXTURE_WRITE_FRAGMENT_DELAY:-}" ]; then
        set -- "$@" --write-fragment-delay "$RTMP_FIXTURE_WRITE_FRAGMENT_DELAY"
    fi
    if [ "${#RTMP_PYTHON_CMD[@]}" -eq 0 ]; then
        rtmp_find_python3 || { echo "SKIP: usable Python 3 not found"; exit 77; }
    fi
    "${RTMP_PYTHON_CMD[@]}" "$HERE/rtmp_fixture_server.py" "$@"
}

rtmp_generate_tone_flv() { # $1=path $2=freq_hz $3=seconds
    local out="$1" freq="$2" secs="$3"
    olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder"; exit 77; }
    ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -f lavfi -i "sine=frequency=${freq}:sample_rate=48000" -ac 2 \
        -t "$secs" \
        "${OLR_H264_VCODEC_ARGS[@]}" \
        -c:a aac -b:a 128k \
        -f flv "$out"
}

rtmp_generate_flash_flv() { # $1=path $2=seconds
    local out="$1" secs="$2"
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    olr_h264_vcodec_args || { echo "SKIP: ffmpeg has no usable H.264 encoder"; exit 77; }
    ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "color=c=black:s=320x240:r=30" -vf "$vflt" \
        -t "$secs" \
        "${OLR_H264_VCODEC_ARGS[@]}" \
        -map 0:v \
        -f flv "$out"
}

rtmp_server() { # $1=port $2=flv $3=log
    local port="$1" flv="$2" log="$3" i
    local expect_play_path="${RTMP_EXPECT_PLAY_PATH:-}"
    if [ -z "$expect_play_path" ] && [ "${RTMP_REQUIRE_PLAY_QUERY:-0}" = "1" ] && [ -n "${RTMP_URL_QUERY:-}" ]; then
        expect_play_path="stream?${RTMP_URL_QUERY}"
    fi
    set -- --port "$port" --flv "$flv"
    if [ -n "${RTMP_SERVER_TLS_CERT:-}" ] || [ -n "${RTMP_SERVER_TLS_KEY:-}" ]; then
        set -- "$@" --tls-cert "${RTMP_SERVER_TLS_CERT:-}" --tls-key "${RTMP_SERVER_TLS_KEY:-}"
    fi
    if [ "${RTMP_REQUIRE_PLAY_QUERY:-0}" = "1" ]; then
        set -- "$@" --require-play-query
    fi
    if [ -n "$expect_play_path" ]; then
        set -- "$@" --expect-play-path "$expect_play_path"
    fi
    rtmp_fixture_server_cmd "$@" >"$log" 2>&1 &
    RTMP_LAST_PID=$!
    PIDS+=("$RTMP_LAST_PID")

    for i in $(seq 1 50); do
        if grep -q '^READY ' "$log"; then return 0; fi
        if ! kill -0 "$RTMP_LAST_PID" 2>/dev/null; then break; fi
        sleep 0.1
    done
    echo "FAIL: RTMP fixture server did not become ready on port $port"
    cat "$log"
    return 1
}

flash_pts_series() {
    ffmpeg -hide_banner -loglevel error -i "$1" -map "0:v:$2" \
        -vf "signalstats,metadata=print:file=-" -f null - 2>/dev/null \
    | awk -v THRESH="${FLASH_THRESH:-180}" '
        /pts_time:/ { for (i=1;i<=NF;i++) if ($i ~ /^pts_time:/) { split($i,a,":"); t=a[2]+0 } }
        /YAVG=/     { split($0,b,"="); y=b[2]+0; bright=(y>THRESH);
                      if (bright && !prev) printf "%.6f\n", t; prev=bright }'
}
