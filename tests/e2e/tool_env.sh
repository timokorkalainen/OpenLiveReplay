#!/usr/bin/env bash

olr_msys_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -u "$1"
        return
    fi

    local p="${1//\\//}"
    case "$p" in
        [A-Za-z]:/*) p="/$(printf '%s' "${p%%:*}" | tr 'A-Z' 'a-z')${p#*:}" ;;
    esac
    printf '%s\n' "$p"
}

olr_prepend_path_dir() {
    local dir="$1"
    [ -n "$dir" ] || return 0
    local msys_dir
    msys_dir="$(olr_msys_path "$dir")"
    [ -d "$msys_dir" ] || return 0
    PATH="$msys_dir:$PATH"
}

olr_prepend_built_tool_paths() {
    if [ -n "${OLR_FFMPEG_ROOT:-}" ]; then
        olr_prepend_path_dir "$OLR_FFMPEG_ROOT/bin"
    fi
    if [ -n "${OLR_SRT_ROOT:-}" ]; then
        olr_prepend_path_dir "$OLR_SRT_ROOT/bin"
    fi
    if [ -n "${OLR_MINGW_BIN:-}" ]; then
        olr_prepend_path_dir "$OLR_MINGW_BIN"
    fi
    if [ -n "${OLR_QT_BIN:-}" ]; then
        olr_prepend_path_dir "$OLR_QT_BIN"
    fi
}

olr_h264_vcodec_args() {
    local encoders
    encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
    if printf '%s\n' "$encoders" | grep -Eq '(^|[[:space:]])libx264[[:space:]]'; then
        OLR_H264_VCODEC_ARGS=(-c:v libx264 -preset ultrafast -tune zerolatency
            -pix_fmt yuv420p -g 30 -b:v 4M)
        return 0
    fi
    if printf '%s\n' "$encoders" | grep -Eq '(^|[[:space:]])h264_mf[[:space:]]'; then
        OLR_H264_VCODEC_ARGS=(-c:v h264_mf -pix_fmt yuv420p -g 30 -b:v 4M)
        return 0
    fi
    return 1
}
