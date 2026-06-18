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
    local sender sender_seconds
    sender="$(ndi_marker_sender_path)"
    sender_seconds=$((seconds + 10))
    "$sender" \
        --name-prefix "$prefix" \
        --sources "$sources" \
        --seconds "$sender_seconds" \
        --skew-ppm "$skew_ppm" \
        --timecode "${OLR_MARKER_TC:-10:00:00:00}" \
        >"${WORKDIR}/${prefix}.ndi.out" 2>"${WORKDIR}/${prefix}.ndi.err" &
    PIDS+=("$!")
    # The local SDK finder can retain just-destroyed sources briefly when matrix
    # cells spin marker senders up and down back-to-back. Give discovery a
    # conservative default window; callers can still tune it down locally.
    sleep "${OLR_NDI_DISCOVERY_SECS:-4}"
    if ! kill -0 "$!" 2>/dev/null; then
        local rc=0
        wait "$!" || rc=$?
        sed -n '1,120p' "${WORKDIR}/${prefix}.ndi.err" >&2
        if [ "$rc" -eq 77 ]; then
            skip "NDI marker sender runtime unavailable"
        fi
        fail "NDI marker sender exited before discovery (rc=${rc})"
    fi
}
