#!/usr/bin/env bash
# NDI marker fixture helpers for run_framesync_e2e.sh.
#
# Caller contract: declare PIDS=() and provide skip()/fail().

ndi_marker_sender_path() {
    local harness_dir
    harness_dir="$(cd "$(dirname "$HARNESS")" && pwd)"
    if [ -n "${OLR_NDI_MARKER_SENDER:-}" ]; then
        echo "$OLR_NDI_MARKER_SENDER"
    else
        echo "${harness_dir}/ndi_marker_sender"
    fi
}

ndi_require_tools_77() {
    command -v ffmpeg >/dev/null || skip "ffmpeg not found"
    command -v ffprobe >/dev/null || skip "ffprobe not found"
    command -v python3 >/dev/null || skip "python3 not found"
    local sender
    sender="$(ndi_marker_sender_path)"
    [ -x "$sender" ] || skip "ndi_marker_sender not built (configure with NDI SDK/runtime)"
}

ndi_url_for_source() {
    local source_name="$1"
    SOURCE_NAME="$source_name" python3 - <<'PY'
import os
import urllib.parse
print("ndi:" + urllib.parse.quote(os.environ["SOURCE_NAME"], safe=""))
PY
}

ndi_marker_source_name() {
    local prefix="$1" index="$2"
    echo "${prefix}-${index}"
}

ndi_start_marker_sender() {
    local prefix="$1" sources="$2" seconds="$3" skew_ppm="$4"
    local sender
    sender="$(ndi_marker_sender_path)"
    "$sender" \
        --name-prefix "$prefix" \
        --sources "$sources" \
        --seconds "$seconds" \
        --skew-ppm "$skew_ppm" \
        --timecode "${OLR_MARKER_TC:-10:00:00:00}" \
        >"${WORKDIR}/${prefix}.ndi.out" 2>"${WORKDIR}/${prefix}.ndi.err" &
    PIDS+=("$!")
    sleep "${OLR_NDI_DISCOVERY_SECS:-2}"
    if ! kill -0 "$!" 2>/dev/null; then
        sed -n '1,120p' "${WORKDIR}/${prefix}.ndi.err" >&2
        fail "NDI marker sender exited before discovery"
    fi
}
