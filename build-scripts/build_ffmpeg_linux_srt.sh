#!/usr/bin/env bash
# Build the controlled, shared LGPL FFmpeg + SRT runtime for Linux packages.
set -euo pipefail

FFMPEG_VERSION="8.1.1"
FFMPEG_TARBALL_SHA256="b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3"
SRT_VERSION="1.5.4"
SRT_COMMIT="a8c6b65520f814c5bd8f801be48c33ceece7c4a6"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/linux_build"
SOURCE_DIR="$WORK_DIR/src"
DIST_DIR="$WORK_DIR/dist"
FFMPEG_PREFIX="$DIST_DIR/ffmpeg"
SRT_PREFIX="$DIST_DIR/srt"
FFMPEG_SOURCE_DIR="$SOURCE_DIR/ffmpeg-$FFMPEG_VERSION"
SRT_SOURCE_DIR="$SOURCE_DIR/srt-$SRT_VERSION"
FFMPEG_TARBALL="$SOURCE_DIR/ffmpeg-$FFMPEG_VERSION.tar.xz"
FFMPEG_TARBALL_PART="$FFMPEG_TARBALL.part"
BUILD_CONFIG_STAMP="$DIST_DIR/.ffmpeg-$FFMPEG_VERSION-libsrt-lgpl-shared.stamp"

CC="${CC:-cc}"
CXX="${CXX:-c++}"
compiler_identity="$($CC --version | head -n 1)"
cxx_compiler_identity="$($CXX --version | head -n 1)"
architecture="$(uname -m)"
NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)"
RPATH_LDFLAGS='-Wl,-rpath,\\\$\$\$\$ORIGIN'

SRT_CMAKE_FLAGS=(
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_INSTALL_PREFIX=$SRT_PREFIX"
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
    '-DCMAKE_INSTALL_RPATH=$ORIGIN'
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DENABLE_SHARED=ON
    -DENABLE_STATIC=OFF
    -DENABLE_APPS=OFF
    -DENABLE_ENCRYPTION=OFF
)

FFMPEG_CONFIGURE_FLAGS=(
    "--prefix=$FFMPEG_PREFIX"
    --enable-shared
    --disable-static
    --disable-gpl
    --disable-nonfree
    --disable-autodetect
    --disable-doc
    --disable-programs
    --disable-avdevice
    --disable-avfilter
    --enable-network
    --enable-libsrt
    --enable-protocol=libsrt
    --enable-protocol=file
    --enable-protocol=pipe
    --enable-protocol=tcp
    --enable-protocol=udp
    "--extra-ldflags=$RPATH_LDFLAGS"
)

for tool in "$CC" "$CXX" cmake ninja pkg-config curl git tar xz sha256sum make readelf; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: required tool '$tool' is not on PATH" >&2
        exit 1
    }
done

config_stamp_content() {
    printf '%s\n' \
        "ffmpeg_version=$FFMPEG_VERSION" \
        "ffmpeg_tarball_sha256=$FFMPEG_TARBALL_SHA256" \
        "srt_version=$SRT_VERSION" \
        "srt_commit=$SRT_COMMIT" \
        "cc=$CC" \
        "cxx=$CXX" \
        "compiler_identity=$compiler_identity" \
        "cxx_compiler_identity=$cxx_compiler_identity" \
        "architecture=$architecture"
    printf 'srt_cmake_flag=%s\n' "${SRT_CMAKE_FLAGS[@]}"
    printf 'ffmpeg_configure_flag=%s\n' "${FFMPEG_CONFIGURE_FLAGS[@]}"
}

expected_artifacts_exist() {
    local library
    for library in avcodec avformat avutil swscale swresample; do
        [ -e "$FFMPEG_PREFIX/lib/lib${library}.so" ] || return 1
    done
    [ -e "$SRT_PREFIX/lib/libsrt.so" ] \
        && [ -f "$FFMPEG_PREFIX/include/libavformat/avformat.h" ] \
        && [ -f "$FFMPEG_PREFIX/include/libavcodec/avcodec.h" ] \
        && [ -f "$FFMPEG_PREFIX/include/libavutil/avutil.h" ] \
        && [ -f "$FFMPEG_PREFIX/include/libswscale/swscale.h" ] \
        && [ -f "$FFMPEG_PREFIX/include/libswresample/swresample.h" ] \
        && [ -f "$SRT_PREFIX/include/srt/srt.h" ]
}

build_is_current() {
    [ -f "$BUILD_CONFIG_STAMP" ] || return 1
    local expected_stamp
    expected_stamp="$(mktemp "$DIST_DIR/.ffmpeg-stamp.XXXXXX")"
    config_stamp_content > "$expected_stamp"
    cmp -s "$expected_stamp" "$BUILD_CONFIG_STAMP" \
        && expected_artifacts_exist \
        && verify_origin_rpaths
    local status=$?
    rm -f "$expected_stamp"
    return "$status"
}

write_config_stamp() {
    local temporary_stamp
    temporary_stamp="$(mktemp "$DIST_DIR/.ffmpeg-stamp.XXXXXX")"
    config_stamp_content > "$temporary_stamp"
    mv "$temporary_stamp" "$BUILD_CONFIG_STAMP"
}

verify_ffmpeg_tarball() {
    local tarball="$1"
    printf '%s  %s\n' "$FFMPEG_TARBALL_SHA256" "$tarball" | sha256sum -c -
}

fetch_sources() {
    mkdir -p "$SOURCE_DIR" "$DIST_DIR"

    if [ -f "$FFMPEG_TARBALL" ] && ! verify_ffmpeg_tarball "$FFMPEG_TARBALL"; then
        echo "==> Removing invalid FFmpeg tarball"
        rm -f "$FFMPEG_TARBALL"
    fi
    if [ ! -f "$FFMPEG_TARBALL" ]; then
        echo "==> Downloading FFmpeg $FFMPEG_VERSION"
        rm -f "$FFMPEG_TARBALL_PART"
        curl --proto '=https' --tlsv1.2 --fail --location --retry 3 \
            -o "$FFMPEG_TARBALL_PART" \
            "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
        verify_ffmpeg_tarball "$FFMPEG_TARBALL_PART"
        mv "$FFMPEG_TARBALL_PART" "$FFMPEG_TARBALL"
    fi
    verify_ffmpeg_tarball "$FFMPEG_TARBALL"

    rm -rf "$SRT_SOURCE_DIR"
    echo "==> Cloning SRT $SRT_VERSION"
    git -c http.sslVerify=true clone --depth 1 --branch "v$SRT_VERSION" \
        https://github.com/Haivision/srt.git "$SRT_SOURCE_DIR"
    git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" reset --hard "$SRT_COMMIT"
    git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" clean -ffdqx
    local srt_head
    srt_head="$(git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" rev-parse HEAD)"
    [ "$srt_head" = "$SRT_COMMIT" ] || {
        echo "ERROR: SRT commit mismatch: $srt_head != $SRT_COMMIT" >&2
        exit 1
    }
    [ -z "$(git -c http.sslVerify=true -C "$SRT_SOURCE_DIR" status --porcelain)" ] || {
        echo "ERROR: SRT source tree is not clean after pinning $SRT_COMMIT" >&2
        exit 1
    }

    rm -rf "$FFMPEG_SOURCE_DIR"
    tar -xJf "$FFMPEG_TARBALL" -C "$SOURCE_DIR"
}

build_srt() {
    local build_dir="$WORK_DIR/build/srt-$SRT_VERSION"
    rm -rf "$build_dir" "$SRT_PREFIX"
    cmake -S "$SRT_SOURCE_DIR" -B "$build_dir" -G Ninja "${SRT_CMAKE_FLAGS[@]}"
    cmake --build "$build_dir" --parallel "$NPROC"
    cmake --install "$build_dir"
}

verify_origin_rpaths() {
    local library
    while IFS= read -r library; do
        if ! readelf -d "$library" | grep -Eq '\((RPATH|RUNPATH)\).*[[]\$ORIGIN[]]'; then
            echo "ERROR: controlled Linux library lacks an origin-relative RPATH: $library" >&2
            return 1
        fi
    done < <(find "$FFMPEG_PREFIX/lib" "$SRT_PREFIX/lib" -type f -name '*.so*' | sort)
}

build_ffmpeg() {
    rm -rf "$FFMPEG_PREFIX"
    export PKG_CONFIG_PATH="$SRT_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export CC CXX
    (
        cd "$FFMPEG_SOURCE_DIR"
        make distclean >/dev/null 2>&1 || true
        ./configure "${FFMPEG_CONFIGURE_FLAGS[@]}"
        make -j"$NPROC"
        make install
    )
}

mkdir -p "$DIST_DIR"
if build_is_current; then
    echo "==> Controlled Linux FFmpeg/SRT is current"
    echo "OLR_FFMPEG_ROOT=$FFMPEG_PREFIX"
    echo "OLR_SRT_ROOT=$SRT_PREFIX"
    exit 0
fi

fetch_sources
echo "==> Building shared SRT $SRT_VERSION"
build_srt
echo "==> Building shared LGPL FFmpeg $FFMPEG_VERSION with libsrt"
build_ffmpeg
expected_artifacts_exist || {
    echo "ERROR: controlled FFmpeg/SRT build did not produce all expected headers and shared libraries" >&2
    exit 1
}
verify_origin_rpaths
write_config_stamp

echo "==> Controlled Linux FFmpeg/SRT built"
echo "OLR_FFMPEG_ROOT=$FFMPEG_PREFIX"
echo "OLR_SRT_ROOT=$SRT_PREFIX"
