#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILDER="$ROOT_DIR/build-scripts/build_ffmpeg_linux_srt.sh"
CMAKE_FILE="$ROOT_DIR/CMakeLists.txt"
PRESETS="$ROOT_DIR/CMakePresets.json"
PACKAGER="$ROOT_DIR/build-scripts/build_linux_app.sh"
WORKFLOW="$ROOT_DIR/.github/workflows/build.yml"
SMOKE_CMAKE="$ROOT_DIR/tests/smoke/CMakeLists.txt"

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

[ -f "$BUILDER" ] || { echo "missing Linux FFmpeg/SRT builder: $BUILDER" >&2; exit 1; }

require_in_file "$BUILDER" 'FFMPEG_VERSION="8.1.1"'
require_in_file "$BUILDER" 'FFMPEG_TARBALL_SHA256="b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3"'
require_in_file "$BUILDER" 'SRT_VERSION="1.5.4"'
require_in_file "$BUILDER" 'SRT_COMMIT="a8c6b65520f814c5bd8f801be48c33ceece7c4a6"'
require_in_file "$BUILDER" "curl --proto '=https' --tlsv1.2 --fail --location"
require_in_file "$BUILDER" 'sha256sum -c -'
require_in_file "$BUILDER" 'git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" rev-parse HEAD'
require_in_file "$BUILDER" 'git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" reset --hard "$SRT_COMMIT"'
require_in_file "$BUILDER" 'git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" clean -ffdqx'
require_in_file "$BUILDER" 'git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" status --porcelain'
require_in_file "$BUILDER" '-DENABLE_SHARED=ON'
require_in_file "$BUILDER" '-DENABLE_STATIC=OFF'
require_in_file "$BUILDER" '-DENABLE_APPS=OFF'
require_in_file "$BUILDER" '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
require_in_file "$BUILDER" '--enable-shared'
require_in_file "$BUILDER" '--disable-static'
require_in_file "$BUILDER" '--disable-avfilter'
require_in_file "$BUILDER" '--enable-libsrt'
require_in_file "$BUILDER" '--enable-protocol=libsrt'
require_in_file "$BUILDER" '--disable-gpl'
require_in_file "$BUILDER" '--disable-nonfree'
require_in_file "$BUILDER" 'RPATH_LDFLAGS='
require_in_file "$BUILDER" '\\\$\$\$\$ORIGIN'
require_in_file "$BUILDER" 'config_stamp_content'
require_in_file "$BUILDER" 'compiler_identity'
require_in_file "$BUILDER" 'cxx_compiler_identity'
require_in_file "$BUILDER" "printf 'srt_cmake_flag=%s\\n'"
require_in_file "$BUILDER" 'architecture='
require_in_file "$BUILDER" 'FFMPEG_TARBALL_PART="$FFMPEG_TARBALL.part"'
require_in_file "$BUILDER" 'mv "$FFMPEG_TARBALL_PART" "$FFMPEG_TARBALL"'
reject_in_file "$BUILDER" '--enable-gpl'
reject_in_file "$BUILDER" '--enable-nonfree'
reject_in_file "$BUILDER" '--enable-libx264'
reject_in_file "$BUILDER" '--enable-libx265'
reject_in_file "$BUILDER" '--disable-shared'
reject_in_file "$BUILDER" '--enable-static'

build_is_current_body="$(sed -n '/^build_is_current()/,/^}/p' "$BUILDER")"
if ! grep -Fq 'verify_origin_rpaths' <<<"$build_is_current_body"; then
    echo "build_is_current must revalidate controlled ELF RPATHs before accepting the stamp" >&2
    exit 1
fi

FFMPEG_PREFIX="$ROOT_DIR/linux_build/dist/ffmpeg"
SRT_PREFIX="$ROOT_DIR/linux_build/dist/srt"
if [ -d "$FFMPEG_PREFIX/lib" ] && [ -d "$SRT_PREFIX/lib" ]; then
    command -v readelf >/dev/null 2>&1 || {
        echo "readelf is required to inspect existing controlled Linux prefixes" >&2
        exit 1
    }
    while IFS= read -r library; do
        if ! readelf -d "$library" | grep -Eq '\((RPATH|RUNPATH)\).*[[]\$ORIGIN[]]'; then
            echo "controlled library lacks an origin-relative RPATH: $library" >&2
            exit 1
        fi
    done < <(find "$FFMPEG_PREFIX/lib" "$SRT_PREFIX/lib" -type f -name '*.so*' | sort)
fi

require_in_file "$CMAKE_FILE" 'option(OLR_CONTROLLED_DEPS_REQUIRED'
require_in_file "$CMAKE_FILE" 'OLR_CONTROLLED_DEPS_REQUIRED=ON requires OLR_FFMPEG_ROOT and OLR_SRT_ROOT'
require_in_file "$CMAKE_FILE" 'find_path(OLR_FFMPEG_AVFORMAT_INCLUDE'
require_in_file "$CMAKE_FILE" 'find_path(OLR_SRT_INCLUDE_DIR'
require_in_file "$CMAKE_FILE" 'find_library(OLR_FFMPEG_AVFORMAT_LIBRARY'
require_in_file "$CMAKE_FILE" 'find_library(OLR_FFMPEG_AVCODEC_LIBRARY'
require_in_file "$CMAKE_FILE" 'find_library(OLR_FFMPEG_AVUTIL_LIBRARY'
require_in_file "$CMAKE_FILE" 'find_library(OLR_FFMPEG_SWSCALE_LIBRARY'
require_in_file "$CMAKE_FILE" 'find_library(OLR_FFMPEG_SWRESAMPLE_LIBRARY'
require_in_file "$CMAKE_FILE" 'find_library(OLR_SRT_LIBRARY'
require_in_file "$CMAKE_FILE" 'NO_DEFAULT_PATH'
require_in_file "$CMAKE_FILE" 'BUILD_RPATH "$ORIGIN/../lib"'
require_in_file "$CMAKE_FILE" 'INSTALL_RPATH "$ORIGIN/../lib"'
require_in_file "$CMAKE_FILE" 'if(OLR_CONTROLLED_DEPS_REQUIRED)'
require_in_file "$CMAKE_FILE" 'else()
        find_package(PkgConfig REQUIRED)'

require_in_file "$PRESETS" '"OLR_CONTROLLED_DEPS_REQUIRED": "ON"'
require_in_file "$PRESETS" '"OLR_FFMPEG_ROOT": "$env{OLR_FFMPEG_ROOT}"'
require_in_file "$PRESETS" '"OLR_SRT_ROOT": "$env{OLR_SRT_ROOT}"'

require_in_file "$PACKAGER" 'build_ffmpeg_linux_srt.sh'
require_in_file "$PACKAGER" 'export OLR_QT_ROOT OLR_FFMPEG_ROOT OLR_SRT_ROOT'
require_in_file "$PACKAGER" 'cp "$BUILD_DIR/bin/OpenLiveReplay" "$APPDIR/usr/bin/OpenLiveReplay.bin"'
require_in_file "$PACKAGER" 'exec "$SCRIPT_DIR/OpenLiveReplay.bin" "$@"'
require_in_file "$PACKAGER" 'patchelf --force-rpath --set-rpath '\''$ORIGIN/../lib'\'''
require_in_file "$PACKAGER" 'patchelf --force-rpath --set-rpath "$library_rpath"'

require_in_file "$WORKFLOW" 'actions/cache@v4'
require_in_file "$WORKFLOW" 'build-scripts/build_ffmpeg_linux_srt.sh'
require_in_file "$WORKFLOW" 'nasm'
require_in_file "$WORKFLOW" 'binutils'
require_in_file "$WORKFLOW" 'OLR_FFMPEG_ROOT=$GITHUB_WORKSPACE/linux_build/dist/ffmpeg'
require_in_file "$WORKFLOW" 'OLR_SRT_ROOT=$GITHUB_WORKSPACE/linux_build/dist/srt'
reject_in_file "$WORKFLOW" 'libavcodec-dev'
reject_in_file "$WORKFLOW" 'libavformat-dev'
reject_in_file "$WORKFLOW" 'libavutil-dev'
reject_in_file "$WORKFLOW" 'libswscale-dev'
reject_in_file "$WORKFLOW" 'libswresample-dev'

require_in_file "$SMOKE_CMAKE" 'add_test(NAME linux_ffmpeg_build_config'
require_in_file "$SMOKE_CMAKE" 'add_test(NAME desktop_packaging_policy_unit'
