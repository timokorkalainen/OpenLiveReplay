#!/bin/bash
set -e

# ==============================================================================
# CONFIGURATION
# ==============================================================================
FFMPEG_VERSION="8.1.1"
SRT_VERSION="1.5.5-rc.0"

# Directories (anchor to repository root, not current working dir)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_CONFIG_STAMP="$ROOT_DIR/ios_build/xcframeworks/.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp"
BUILD_CONFIG_ID="ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter"
EXPECTED_ARCHIVES=(
    "$ROOT_DIR/ios_build/xcframeworks/libavcodec.xcframework/ios-arm64/libavcodec.a"
    "$ROOT_DIR/ios_build/xcframeworks/libavcodec.xcframework/ios-arm64-simulator/libavcodec.a"
    "$ROOT_DIR/ios_build/xcframeworks/libavformat.xcframework/ios-arm64/libavformat.a"
    "$ROOT_DIR/ios_build/xcframeworks/libavformat.xcframework/ios-arm64-simulator/libavformat.a"
    "$ROOT_DIR/ios_build/xcframeworks/libavutil.xcframework/ios-arm64/libavutil.a"
    "$ROOT_DIR/ios_build/xcframeworks/libavutil.xcframework/ios-arm64-simulator/libavutil.a"
    "$ROOT_DIR/ios_build/xcframeworks/libswscale.xcframework/ios-arm64/libswscale.a"
    "$ROOT_DIR/ios_build/xcframeworks/libswscale.xcframework/ios-arm64-simulator/libswscale.a"
    "$ROOT_DIR/ios_build/xcframeworks/libswresample.xcframework/ios-arm64/libswresample.a"
    "$ROOT_DIR/ios_build/xcframeworks/libswresample.xcframework/ios-arm64-simulator/libswresample.a"
    "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework/ios-arm64/libsrt.a"
    "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework/ios-arm64-simulator/libsrt.a"
)

remove_stale_crypto_xcframeworks() {
    for artifact in \
        "$ROOT_DIR/ios_build/xcframeworks/libmbedtls.xcframework" \
        "$ROOT_DIR/ios_build/xcframeworks/libmbedx509.xcframework" \
        "$ROOT_DIR/ios_build/xcframeworks/libmbedcrypto.xcframework" \
        "$ROOT_DIR/ios_build/xcframeworks/libssl.xcframework" \
        "$ROOT_DIR/ios_build/xcframeworks/libcrypto.xcframework" \
        "$ROOT_DIR/ios_build/dist/iphoneos-arm64/mbedtls" \
        "$ROOT_DIR/ios_build/dist/iphonesimulator-arm64/mbedtls" \
        "$ROOT_DIR/ios_build/dist/iphoneos-arm64/openssl" \
        "$ROOT_DIR/ios_build/dist/iphonesimulator-arm64/openssl"; do
        if [ -e "$artifact" ]; then
            echo "[FFmpeg] Removing stale crypto artifact: $artifact"
            rm -rf "$artifact"
        fi
    done
}

libsrt_has_forbidden_crypto_symbols() {
    local srt_lib="$1"

    if [ ! -f "$srt_lib" ] || ! command -v nm >/dev/null 2>&1; then
        return 1
    fi

    nm -u "$srt_lib" 2>/dev/null | grep -E '_mbedtls_|_SSL_|_OPENSSL_|_CRYPTO_|_EVP_|_RAND_' >/dev/null
}

all_expected_archives_exist() {
    local archive

    for archive in "${EXPECTED_ARCHIVES[@]}"; do
        if [ ! -f "$archive" ]; then
            return 1
        fi
    done
}

remove_stale_crypto_xcframeworks

# Skip the ~20 minute rebuild when the artifacts already exist — matches the
# CMake custom command's only-when-missing semantics, and lets prebuilt
# xcframeworks be staged into ios_build/xcframeworks/ (e.g. from another
# checkout) without triggering a full rebuild.
if [ -f "$BUILD_CONFIG_STAMP" ] && grep -Fxq "$BUILD_CONFIG_ID" "$BUILD_CONFIG_STAMP" && all_expected_archives_exist; then
    if libsrt_has_forbidden_crypto_symbols "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework/ios-arm64/libsrt.a" || \
       libsrt_has_forbidden_crypto_symbols "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework/ios-arm64-simulator/libsrt.a"; then
        echo "[FFmpeg] Cached libsrt references OpenSSL/mbedTLS symbols; rebuilding."
        rm -f "$BUILD_CONFIG_STAMP"
    else
        echo "[FFmpeg] Prebuilt xcframeworks already present; skipping rebuild."
        touch "$BUILD_CONFIG_STAMP"
        exit 0
    fi
fi

WORK_DIR="$ROOT_DIR/ios_build"
SRC_DIR="$WORK_DIR/src"
DIST_DIR="$WORK_DIR/dist"
mkdir -p "$SRC_DIR" "$DIST_DIR"

# iOS SDK setup
IOS_MIN_VERSION="13.0"

# CMake (Xcode build environment may not have PATH)
if [ -z "${CMAKE_BIN}" ]; then
    if command -v cmake >/dev/null 2>&1; then
        CMAKE_BIN="$(command -v cmake)"
    elif [ -x "/Users/timo.korkalainen/Qt/Tools/CMake/CMake.app/Contents/bin/cmake" ]; then
        CMAKE_BIN="/Users/timo.korkalainen/Qt/Tools/CMake/CMake.app/Contents/bin/cmake"
    elif [ -x "/opt/homebrew/bin/cmake" ]; then
        CMAKE_BIN="/opt/homebrew/bin/cmake"
    elif [ -x "/usr/local/bin/cmake" ]; then
        CMAKE_BIN="/usr/local/bin/cmake"
    else
        echo "Error: cmake not found. Set CMAKE_BIN or add cmake to PATH." >&2
        exit 1
    fi
fi

# pkg-config (Xcode build environment may not have PATH)
if [ -z "${PKG_CONFIG_BIN}" ]; then
    if command -v pkg-config >/dev/null 2>&1; then
        PKG_CONFIG_BIN="$(command -v pkg-config)"
    elif [ -x "/opt/homebrew/bin/pkg-config" ]; then
        PKG_CONFIG_BIN="/opt/homebrew/bin/pkg-config"
    elif [ -x "/usr/local/bin/pkg-config" ]; then
        PKG_CONFIG_BIN="/usr/local/bin/pkg-config"
    else
        echo "Error: pkg-config not found. Install it or set PKG_CONFIG_BIN." >&2
        exit 1
    fi
fi

echo "Starting Build: FFmpeg + SRT for iOS"

# ==============================================================================
# 1. DOWNLOAD SOURCES
# ==============================================================================
download_src() {
    echo "Downloading sources..."
    cd "$SRC_DIR"

    if [ ! -d "srt-$SRT_VERSION" ]; then
        echo "Downloading SRT..."
        curl -L -O "https://github.com/Haivision/srt/archive/v$SRT_VERSION.tar.gz"
        tar xf "v$SRT_VERSION.tar.gz"
    fi

    if [ ! -d "ffmpeg-$FFMPEG_VERSION" ]; then
        echo "Downloading FFmpeg..."
        curl -L -O "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.bz2"
        tar xf "ffmpeg-$FFMPEG_VERSION.tar.bz2"
    fi
}

# ==============================================================================
# 2. BUILD SRT
# ==============================================================================
build_srt() {
    ARCH=$1
    PLATFORM=$2
    echo "----------------------------------------------------------------"
    echo "Building SRT for $ARCH ($PLATFORM)..."
    echo "----------------------------------------------------------------"

    cd "$SRC_DIR/srt-$SRT_VERSION"
    rm -rf "build-$PLATFORM-$ARCH"
    mkdir -p "build-$PLATFORM-$ARCH"
    cd "build-$PLATFORM-$ARCH"

    SDK_PATH=$(xcrun --sdk $PLATFORM --show-sdk-path)

    # Set target flags explicitly for CMake
    if [ "$PLATFORM" == "iphonesimulator" ]; then
        TARGET_FLAGS="-target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    else
        TARGET_FLAGS="-target $ARCH-apple-ios$IOS_MIN_VERSION"
    fi

    echo "Configuring SRT..."
    "$CMAKE_BIN" ../ \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT=$SDK_PATH \
        -DCMAKE_OSX_ARCHITECTURES=$ARCH \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN_VERSION \
        -DCMAKE_C_FLAGS="$TARGET_FLAGS" \
        -DCMAKE_CXX_FLAGS="$TARGET_FLAGS" \
        -DENABLE_SHARED=OFF \
        -DENABLE_STATIC=ON \
        -DENABLE_APPS=OFF \
        -DENABLE_ENCRYPTION=OFF \
        -DENABLE_C_DEPS=ON \
        -DCMAKE_INSTALL_PREFIX="$DIST_DIR/$PLATFORM-$ARCH/srt"

    echo "Compiling SRT..."
    make -j$(sysctl -n hw.ncpu)
    make install
}

# ==============================================================================
# 3. BUILD FFMPEG
# ==============================================================================
build_ffmpeg() {
    ARCH=$1
    PLATFORM=$2
    echo "----------------------------------------------------------------"
    echo "Building FFmpeg for $ARCH ($PLATFORM)..."
    echo "----------------------------------------------------------------"

    cd "$SRC_DIR/ffmpeg-$FFMPEG_VERSION"

    # Clean build environment
    make distclean || true

    SDK_PATH=$(xcrun --sdk $PLATFORM --show-sdk-path)
    SRT_ROOT="$DIST_DIR/$PLATFORM-$ARCH/srt"

    # 1. PKG_CONFIG setup
    export PKG_CONFIG="$PKG_CONFIG_BIN"
    export PKG_CONFIG_PATH="$SRT_ROOT/lib/pkgconfig"
    export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
    export PATH="$(dirname "$PKG_CONFIG_BIN"):$PATH"

    # Verify pkg-config can see SRT
    if ! "$PKG_CONFIG_BIN" --modversion srt >/dev/null 2>&1; then
        echo "Error: pkg-config cannot find srt.pc for $PLATFORM-$ARCH." >&2
        echo "PKG_CONFIG_PATH=$PKG_CONFIG_PATH" >&2
        "$PKG_CONFIG_BIN" --list-all | grep -i srt || true
        exit 1
    fi

    # 2. Patch srt.pc for static linking. SRT is static C++ code.
    PC_FILE="$SRT_ROOT/lib/pkgconfig/srt.pc"
    if [ -f "$PC_FILE" ]; then
        echo "Patching srt.pc for static linking..."
        sed -i.bak 's/Libs.private:/Libs.private: -lc++/g' "$PC_FILE"
    fi

    # 3. Setup flags
    if [ "$PLATFORM" == "iphonesimulator" ]; then
        TARGET_FLAGS="-target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    else
        TARGET_FLAGS="-target $ARCH-apple-ios$IOS_MIN_VERSION"
    fi

    CFLAGS="$TARGET_FLAGS -isysroot $SDK_PATH -I$SRT_ROOT/include"
    LDFLAGS="$TARGET_FLAGS -isysroot $SDK_PATH -L$SRT_ROOT/lib"

    echo "Configuring FFmpeg..."
    ./configure \
        --prefix="$DIST_DIR/$PLATFORM-$ARCH/ffmpeg" \
        --enable-cross-compile \
        --target-os=darwin \
        --arch=$ARCH \
        --cc="$(xcrun -find -sdk $PLATFORM clang)" \
        --sysroot=$SDK_PATH \
        --extra-cflags="$CFLAGS" \
        --extra-ldflags="$LDFLAGS" \
        --extra-libs="-lc++" \
        --enable-static \
        --disable-shared \
        --disable-gpl \
        --disable-nonfree \
        --disable-autodetect \
        --disable-everything \
        --disable-doc \
        --disable-programs \
        --disable-avdevice \
        --disable-avfilter \
        --enable-avcodec \
        --enable-avformat \
        --enable-avutil \
        --enable-swscale \
        --enable-swresample \
        --enable-network \
        --enable-videotoolbox \
        --enable-audiotoolbox \
        --enable-securetransport \
        --enable-libsrt \
        --enable-protocol=file \
        --enable-protocol=libsrt \
        --enable-protocol=tcp \
        --enable-protocol=rtmp \
        --enable-protocol=tls \
        --enable-protocol=rtmps \
        --enable-demuxer=mpegts \
        --enable-demuxer=matroska \
        --enable-demuxer=flv \
        --enable-demuxer=live_flv \
        --enable-muxer=matroska \
        --enable-parser=h264 \
        --enable-parser=av1 \
        --enable-parser=vp9 \
        --enable-parser=prores \
        --enable-parser=mpegvideo \
        --enable-parser=aac \
        --enable-decoder=h264 \
        --enable-decoder=av1 \
        --enable-decoder=vp9 \
        --enable-decoder=prores \
        --enable-decoder=mpeg2video \
        --enable-decoder=aac \
        --enable-decoder=aac_at \
        --enable-decoder=pcm_s16le \
        --enable-encoder=mpeg2video \
        --enable-encoder=pcm_s16le \
        --enable-encoder=h264_videotoolbox \
        --enable-encoder=prores_videotoolbox \
        --enable-hwaccel=h264_videotoolbox \
        --enable-hwaccel=av1_videotoolbox \
        --enable-hwaccel=vp9_videotoolbox \
        --enable-hwaccel=prores_videotoolbox \
        --enable-hwaccel=mpeg2_videotoolbox \
        --pkg-config="$PKG_CONFIG_BIN" \
        --pkg-config-flags="--static"

    echo "Compiling FFmpeg..."
    make -j$(sysctl -n hw.ncpu)
    make install

    unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG
}

# ==============================================================================
# 4. EXECUTION & PACKAGING
# ==============================================================================

download_src

# --- Build for Device (arm64) ---
build_srt     "arm64" "iphoneos"
build_ffmpeg  "arm64" "iphoneos"

# --- Build for Simulator (arm64) ---
build_srt     "arm64" "iphonesimulator"
build_ffmpeg  "arm64" "iphonesimulator"

# --- Create XCFrameworks ---
echo "Packaging XCFrameworks..."
mkdir -p "$WORK_DIR/xcframeworks"

create_xcframework() {
    LIB_NAME=$1
    echo "Creating $LIB_NAME.xcframework..."
    rm -rf "$WORK_DIR/xcframeworks/$LIB_NAME.xcframework"

    xcodebuild -create-xcframework \
        -library "$DIST_DIR/iphoneos-arm64/ffmpeg/lib/$LIB_NAME.a" \
        -headers "$DIST_DIR/iphoneos-arm64/ffmpeg/include" \
        -library "$DIST_DIR/iphonesimulator-arm64/ffmpeg/lib/$LIB_NAME.a" \
        -headers "$DIST_DIR/iphonesimulator-arm64/ffmpeg/include" \
        -output "$WORK_DIR/xcframeworks/$LIB_NAME.xcframework"
}

create_dependency_xcframework() {
    LIB_NAME=$1
    DEP_NAME=$2
    echo "Creating $LIB_NAME.xcframework..."
    rm -rf "$WORK_DIR/xcframeworks/$LIB_NAME.xcframework"

    xcodebuild -create-xcframework \
        -library "$DIST_DIR/iphoneos-arm64/$DEP_NAME/lib/$LIB_NAME.a" \
        -headers "$DIST_DIR/iphoneos-arm64/$DEP_NAME/include" \
        -library "$DIST_DIR/iphonesimulator-arm64/$DEP_NAME/lib/$LIB_NAME.a" \
        -headers "$DIST_DIR/iphonesimulator-arm64/$DEP_NAME/include" \
        -output "$WORK_DIR/xcframeworks/$LIB_NAME.xcframework"
}

# Create FFmpeg frameworks
LIBS=("libavcodec" "libavformat" "libavutil" "libswresample" "libswscale")
for LIB in "${LIBS[@]}"; do
    create_xcframework $LIB
done

# Create dependency frameworks
create_dependency_xcframework "libsrt" "srt"

printf '%s\n' "$BUILD_CONFIG_ID" > "$BUILD_CONFIG_STAMP"

echo "DONE! XCFrameworks are in: $WORK_DIR/xcframeworks"
