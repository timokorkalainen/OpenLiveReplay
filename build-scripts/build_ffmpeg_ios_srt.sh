#!/bin/bash
set -e

# ==============================================================================
# CONFIGURATION
# ==============================================================================
FFMPEG_VERSION="8.0"
SRT_VERSION="1.5.5-rc.0"
OPENSSL_VERSION="3.2.0"

# Directories (anchor to repository root, not current working dir)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Local build-environment staging (worktree only, this file is untracked):
# the xcframeworks were copied prebuilt from the main checkout, so skip the
# ~20 minute rebuild when they are already present.
if [ -d "$ROOT_DIR/ios_build/xcframeworks/libavcodec.xcframework" ] && \
   [ -d "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework" ]; then
    echo "[FFmpeg] Prebuilt xcframeworks already present; skipping rebuild."
    exit 0
fi

WORK_DIR="$ROOT_DIR/ios_build"
SRC_DIR="$WORK_DIR/src"
DIST_DIR="$WORK_DIR/dist"
mkdir -p "$SRC_DIR" "$DIST_DIR"

# iOS SDK setup
IOS_MIN_VERSION="13.0"
XCODE_PATH=$(xcode-select -p)

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

echo "Starting Build: FFmpeg + SRT + OpenSSL for iOS"

# ==============================================================================
# 1. DOWNLOAD SOURCES
# ==============================================================================
download_src() {
    echo "Downloading sources..."
    cd "$SRC_DIR"
    
    if [ ! -d "openssl-$OPENSSL_VERSION" ]; then
        echo "Downloading OpenSSL..."
        curl -L -O "https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz"
        tar xf "openssl-$OPENSSL_VERSION.tar.gz"
    fi

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
# 2. BUILD OPENSSL
# ==============================================================================
build_openssl() {
    ARCH=$1
    PLATFORM=$2
    echo "----------------------------------------------------------------"
    echo "Building OpenSSL for $ARCH ($PLATFORM)..."
    echo "----------------------------------------------------------------"

    cd "$SRC_DIR/openssl-$OPENSSL_VERSION"

    # CRITICAL: Clean thoroughly between builds
    make distclean || true
    rm -rf Makefile config.status
    # OpenSSL 3.2.0 ships with include/openssl/opensslconf.h in the tarball.
    # Don't remove it (Configure won't regenerate it).
    if [ ! -f include/openssl/opensslconf.h ]; then
        echo "Re-extracting opensslconf.h from tarball..."
        tar --strip-components=1 -xf "$SRC_DIR/openssl-$OPENSSL_VERSION.tar.gz" \
            "openssl-$OPENSSL_VERSION/include/openssl/opensslconf.h"
    fi

    SDK_PATH=$(xcrun --sdk $PLATFORM --show-sdk-path)
    
    # Platform-specific configuration
    if [ "$PLATFORM" == "iphonesimulator" ]; then
        # For Simulator (especially arm64), we MUST force the target via CC
        # to prevent OpenSSL from confusing host-arm64 with simulator-arm64
        TARGET="iossimulator-xcrun"
        export CC="$(xcrun -find -sdk $PLATFORM clang) -target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
        export CFLAGS="-isysroot $SDK_PATH -Wno-unused-command-line-argument"
    else
        # For Device, standard configuration works best
        TARGET="ios64-xcrun"
        export CC="$(xcrun -find -sdk $PLATFORM clang) -target $ARCH-apple-ios$IOS_MIN_VERSION"
        export CFLAGS="-isysroot $SDK_PATH -Wno-unused-command-line-argument"
    fi

    export CROSS_TOP="$XCODE_PATH/Platforms/$PLATFORM.platform/Developer"
    export CROSS_SDK="$PLATFORM.sdk"

    echo "Configuring OpenSSL..."
    # no-async: Disable async to prevent build errors on iOS
    ./Configure $TARGET no-shared no-tests no-async \
        --prefix="$DIST_DIR/$PLATFORM-$ARCH/openssl" \
        --openssldir="$DIST_DIR/$PLATFORM-$ARCH/openssl" \
        "$CFLAGS"

    echo "Compiling OpenSSL..."
    make -j$(sysctl -n hw.ncpu)
    make install_sw
    
    unset CC CFLAGS CROSS_TOP CROSS_SDK
}

# ==============================================================================
# 3. BUILD SRT
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
    OPENSSL_ROOT="$DIST_DIR/$PLATFORM-$ARCH/openssl"

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
        -DENABLE_C_DEPS=ON \
        -DUSE_OPENSSL_PC=OFF \
        -DOPENSSL_USE_STATIC_LIBS=ON \
        -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT" \
        -DOPENSSL_INCLUDE_DIR="$OPENSSL_ROOT/include" \
        -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_ROOT/lib/libcrypto.a" \
        -DOPENSSL_SSL_LIBRARY="$OPENSSL_ROOT/lib/libssl.a" \
        -DCMAKE_INSTALL_PREFIX="$DIST_DIR/$PLATFORM-$ARCH/srt"

    echo "Compiling SRT..."
    make -j$(sysctl -n hw.ncpu)
    make install
}

# ==============================================================================
# 4. BUILD FFMPEG
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
    OPENSSL_ROOT="$DIST_DIR/$PLATFORM-$ARCH/openssl"

    # 1. PKG_CONFIG setup
    export PKG_CONFIG="$PKG_CONFIG_BIN"
    export PKG_CONFIG_PATH="$SRT_ROOT/lib/pkgconfig:$OPENSSL_ROOT/lib/pkgconfig"
    export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
    export PATH="$(dirname "$PKG_CONFIG_BIN"):$PATH"

    # Verify pkg-config can see SRT and OpenSSL
    if ! "$PKG_CONFIG_BIN" --modversion srt >/dev/null 2>&1; then
        echo "Error: pkg-config cannot find srt.pc for $PLATFORM-$ARCH." >&2
        echo "PKG_CONFIG_PATH=$PKG_CONFIG_PATH" >&2
        "$PKG_CONFIG_BIN" --list-all | grep -i srt || true
        exit 1
    fi

    # 2. Patch srt.pc for static linking
    # We must append standard C++ libs because SRT is static
    PC_FILE="$SRT_ROOT/lib/pkgconfig/srt.pc"
    if [ -f "$PC_FILE" ]; then
        echo "Patching srt.pc for static linking..."
        sed -i.bak 's/Libs.private:/Libs.private: -lc++ -lssl -lcrypto/g' "$PC_FILE"
    fi

    # 3. Setup flags
    if [ "$PLATFORM" == "iphonesimulator" ]; then
        TARGET_FLAGS="-target $ARCH-apple-ios$IOS_MIN_VERSION-simulator"
    else
        TARGET_FLAGS="-target $ARCH-apple-ios$IOS_MIN_VERSION"
    fi

    CFLAGS="$TARGET_FLAGS -isysroot $SDK_PATH -I$SRT_ROOT/include -I$OPENSSL_ROOT/include"
    LDFLAGS="$TARGET_FLAGS -isysroot $SDK_PATH -L$SRT_ROOT/lib -L$OPENSSL_ROOT/lib"

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
        --enable-gpl \
        --enable-version3 \
        --enable-nonfree \
        --disable-static \
        --enable-shared \
        --disable-doc \
        --disable-programs \
        --disable-avdevice \
        --disable-indevs \
        --disable-outdevs \
        --disable-filter=scale_vt \
        --enable-videotoolbox \
        --enable-libsrt \
        --enable-openssl \
        --enable-protocol=libsrt \
        --pkg-config="$PKG_CONFIG_BIN" \
        --pkg-config-flags="--static"

    echo "Compiling FFmpeg..."
    make -j$(sysctl -n hw.ncpu)
    make install

    # 4. Fix install names for iOS runtime (avoid absolute build paths)
    LIBDIR="$DIST_DIR/$PLATFORM-$ARCH/ffmpeg/lib"
    LIBS=("libavcodec" "libavformat" "libavutil" "libswscale" "libswresample" "libavfilter")

    resolve_realpath() {
        python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$1"
    }

    echo "Fixing install names in $LIBDIR..."
    # Step 1: Fix the -id of each dylib
    for lib in "${LIBS[@]}"; do
        real_lib="$(resolve_realpath "$LIBDIR/$lib.dylib")"
        if [ -f "$real_lib" ]; then
            install_name_tool -id "@rpath/$lib.dylib" "$real_lib"
        fi
    done

    # Step 2: Fix inter-library dependencies
    # The dependencies use versioned names like libavutil.60.dylib with the full install prefix
    for lib in "${LIBS[@]}"; do
        real_lib="$(resolve_realpath "$LIBDIR/$lib.dylib")"
        if [ -f "$real_lib" ]; then
            # Find all dependencies pointing to our build dir and fix them
            otool -L "$real_lib" | grep "$LIBDIR" | awk '{print $1}' | while read -r old_dep; do
                # Extract base lib name: /path/to/libavutil.60.dylib -> libavutil
                dep_basename="$(basename "$old_dep")"
                dep_name="$(echo "$dep_basename" | sed 's/\.[0-9]*\.dylib/.dylib/')"
                install_name_tool -change "$old_dep" "@rpath/$dep_name" "$real_lib" 2>/dev/null || true
            done
        fi
    done
    
    unset PKG_CONFIG_PATH
}

# ==============================================================================
# 5. EXECUTION & PACKAGING
# ==============================================================================

download_src

# --- Build for Device (arm64) ---
build_openssl "arm64" "iphoneos"
build_srt     "arm64" "iphoneos"
build_ffmpeg  "arm64" "iphoneos"

# --- Build for Simulator (arm64) ---
build_openssl "arm64" "iphonesimulator"
build_srt     "arm64" "iphonesimulator"
build_ffmpeg  "arm64" "iphonesimulator"

# --- Create XCFrameworks ---
echo "Packaging XCFrameworks..."
mkdir -p "$WORK_DIR/xcframeworks"

create_xcframework() {
    LIB_NAME=$1
    echo "Creating $LIB_NAME.xcframework..."
    rm -rf "$WORK_DIR/xcframeworks/$LIB_NAME.xcframework"

    LIB_DEV="$DIST_DIR/iphoneos-arm64/ffmpeg/lib/$LIB_NAME.dylib"
    LIB_SIM="$DIST_DIR/iphonesimulator-arm64/ffmpeg/lib/$LIB_NAME.dylib"
    LIB_DEV_REAL="$(python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$LIB_DEV")"
    LIB_SIM_REAL="$(python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$LIB_SIM")"

    xcodebuild -create-xcframework \
        -library "$LIB_DEV_REAL" \
        -headers "$DIST_DIR/iphoneos-arm64/ffmpeg/include" \
        -library "$LIB_SIM_REAL" \
        -headers "$DIST_DIR/iphonesimulator-arm64/ffmpeg/include" \
        -output "$WORK_DIR/xcframeworks/$LIB_NAME.xcframework"

    # Ensure a stable lib name exists for linkers expecting lib<name>.dylib
    DEV_SLICE="$WORK_DIR/xcframeworks/$LIB_NAME.xcframework/ios-arm64"
    SIM_SLICE="$WORK_DIR/xcframeworks/$LIB_NAME.xcframework/ios-arm64-simulator"
    DEV_BASENAME="$(basename "$LIB_DEV_REAL")"
    SIM_BASENAME="$(basename "$LIB_SIM_REAL")"
    if [ -d "$DEV_SLICE" ]; then
        ln -sf "$DEV_BASENAME" "$DEV_SLICE/$LIB_NAME.dylib"
    fi
    if [ -d "$SIM_SLICE" ]; then
        ln -sf "$SIM_BASENAME" "$SIM_SLICE/$LIB_NAME.dylib"
    fi
}

# Create FFmpeg frameworks
LIBS=("libavcodec" "libavfilter" "libavformat" "libavutil" "libswresample" "libswscale")
for LIB in "${LIBS[@]}"; do
    create_xcframework $LIB
done

# Create SRT framework (Static)
echo "Creating libsrt.xcframework..."
rm -rf "$WORK_DIR/xcframeworks/libsrt.xcframework"
xcodebuild -create-xcframework \
    -library "$DIST_DIR/iphoneos-arm64/srt/lib/libsrt.a" \
    -library "$DIST_DIR/iphonesimulator-arm64/srt/lib/libsrt.a" \
    -output "$WORK_DIR/xcframeworks/libsrt.xcframework"

echo "DONE! XCFrameworks are in: $WORK_DIR/xcframeworks"