#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CMAKE="$ROOT_DIR/CMakeLists.txt"
PRE_PUSH="$ROOT_DIR/.githooks/pre-push"

require_in_file() {
    local file="$1"
    local needle="$2"
    if ! grep -Fq -- "$needle" "$file"; then
        echo "missing '$needle' in $file" >&2
        exit 1
    fi
}

require_in_file "$CMAKE" 'target_compile_definitions(OpenLiveReplay PRIVATE OLR_GPU_PIPELINE_BUILD=1)'
require_in_file "$CMAKE" 'playback/gpu/iosgpupolicy.h playback/gpu/iosgpupolicy.cpp'
require_in_file "$CMAKE" 'playback/gpu/iosgpulifecyclesink.h playback/gpu/iosgpulifecyclesink.cpp'
require_in_file "$CMAKE" 'playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm'
require_in_file "$CMAKE" 'playback/gpu/vtkeepsurfaceimporter.h playback/gpu/vtkeepsurfaceimporter_apple.mm'
require_in_file "$CMAKE" 'ios/iosgpulifecycle.h ios/iosgpulifecycle.mm'
require_in_file "$CMAKE" '"-framework UIKit"'
require_in_file "$PRE_PUSH" '-DOLR_GPU_PIPELINE=ON'
require_in_file "$PRE_PUSH" 'env -u GIT_CONFIG -u GIT_CONFIG_GLOBAL -u GIT_CONFIG_SYSTEM -u GIT_CONFIG_COUNT'

echo "iOS GPU build-config invariants present."
