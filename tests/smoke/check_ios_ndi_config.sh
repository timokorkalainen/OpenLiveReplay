#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CMAKE="$ROOT_DIR/CMakeLists.txt"
PLIST="$ROOT_DIR/ios/Info.plist"
NDI_RUNTIME_PATHS="$ROOT_DIR/playback/output/ndiruntimepaths.h"
NDI_STATICLINK="$ROOT_DIR/playback/output/ndistaticlink.h"
NDI_SINK="$ROOT_DIR/playback/output/ndisink.cpp"
NDI_INGEST="$ROOT_DIR/recorder_engine/ingest/nativendiingestsession.cpp"

require_in_file() {
    local file="$1"
    local needle="$2"
    if ! grep -Fq -- "$needle" "$file"; then
        echo "missing '$needle' in $file" >&2
        exit 1
    fi
}

reject_in_file() {
    local file="$1"
    local needle="$2"
    if grep -Fq -- "$needle" "$file"; then
        echo "forbidden '$needle' found in $file" >&2
        exit 1
    fi
}

# Local-network / Bonjour permissions needed for NDI mDNS discovery on iOS 14+.
require_in_file "$PLIST" "NSLocalNetworkUsageDescription"
require_in_file "$PLIST" "NSBonjourServices"
require_in_file "$PLIST" "_ndi._tcp."

# NDI's Apple SDK ships iOS support as a static archive rather than the
# runtime-loadable dylib/so used on desktop.
require_in_file "$CMAKE" "OLR_NDI_IOS_SDK_DIR"
require_in_file "$CMAKE" "libndi_ios.a"
require_in_file "$CMAKE" "NO_CMAKE_FIND_ROOT_PATH"
require_in_file "$CMAKE" "OLR_NDI_STATIC_LINK=1"
require_in_file "$CMAKE" '"-framework Accelerate"'
reject_in_file "$CMAKE" '"-ldns_sd"'

require_in_file "$NDI_RUNTIME_PATHS" "defined(Q_OS_IOS)"

# Send path (NDI output / PGM oracle) already resolves its symbols statically.
require_in_file "$NDI_SINK" "OLR_NDI_STATIC_LINK"
require_in_file "$NDI_SINK" "resolveStaticSymbols"
require_in_file "$NDI_SINK" "NDIlib_send_create"

# Receive path (NDI ingest) must resolve statically too, and share the ABI /
# static-link headers instead of redeclaring the NDI structs locally.
require_in_file "$NDI_INGEST" "playback/output/ndiabi.h"
require_in_file "$NDI_INGEST" "playback/output/ndistaticlink.h"
require_in_file "$NDI_INGEST" "OLR_NDI_STATIC_LINK"
require_in_file "$NDI_INGEST" "resolveStaticSymbols"
require_in_file "$NDI_INGEST" "NDIlib_recv_create_v3"
reject_in_file "$NDI_INGEST" "struct NDIlib_recv_create_v3_t"
reject_in_file "$NDI_INGEST" "struct NDIlib_video_frame_v2_t"
reject_in_file "$NDI_INGEST" "struct NDIlib_audio_frame_v3_t"

# The shared static-link header must declare the receive-side ABI alongside
# the already-merged send-side declarations.
require_in_file "$NDI_STATICLINK" "NDIlib_find_create_v2"
require_in_file "$NDI_STATICLINK" "NDIlib_find_get_current_sources"
require_in_file "$NDI_STATICLINK" "NDIlib_recv_create_v3"
require_in_file "$NDI_STATICLINK" "NDIlib_recv_capture_v3"
require_in_file "$NDI_STATICLINK" "NDIlib_send_create"

echo "ios_ndi_config: OK"
