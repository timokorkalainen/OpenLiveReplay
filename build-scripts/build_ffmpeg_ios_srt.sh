#!/bin/bash
set -euo pipefail

# ==============================================================================
# CONFIGURATION
# ==============================================================================
FFMPEG_VERSION="8.1.1"
FFMPEG_TARBALL_SHA256="b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3"
SRT_VERSION="1.5.5-rc.0"
SRT_COMMIT="52ceecdf5190885914f0f94d01be32441ccb1f4c"

# Directories (anchor to repository root, not current working dir)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/ios_build"
SRC_DIR="$WORK_DIR/src"
DIST_DIR="$WORK_DIR/dist"
FFMPEG_TARBALL="$SRC_DIR/ffmpeg-$FFMPEG_VERSION.tar.xz"
FFMPEG_SOURCE_DIR="$SRC_DIR/ffmpeg-$FFMPEG_VERSION"
SRT_SOURCE_DIR="$SRC_DIR/srt-$SRT_VERSION"
BUILD_CONFIG_STAMP="$ROOT_DIR/ios_build/xcframeworks/.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp"
BUILD_CONFIG_ID="ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter"
PROVENANCE_MANIFEST="$ROOT_DIR/ios_build/xcframeworks/ffmpeg-provenance.json"
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
EXPECTED_HEADERS=(
    "$ROOT_DIR/ios_build/xcframeworks/libavcodec.xcframework/ios-arm64/Headers/libavcodec/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libavcodec.xcframework/ios-arm64-simulator/Headers/libavcodec/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libavformat.xcframework/ios-arm64/Headers/libavformat/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libavformat.xcframework/ios-arm64-simulator/Headers/libavformat/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libavutil.xcframework/ios-arm64/Headers/libavutil/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libavutil.xcframework/ios-arm64-simulator/Headers/libavutil/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libswresample.xcframework/ios-arm64/Headers/libswresample/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libswresample.xcframework/ios-arm64-simulator/Headers/libswresample/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libswscale.xcframework/ios-arm64/Headers/libswscale/version_major.h"
    "$ROOT_DIR/ios_build/xcframeworks/libswscale.xcframework/ios-arm64-simulator/Headers/libswscale/version_major.h"
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

all_expected_headers_exist() {
    local header

    for header in "${EXPECTED_HEADERS[@]}"; do
        if [ ! -f "$header" ]; then
            return 1
        fi
    done
}

write_provenance_manifest() {
    local python_bin

    python_bin="${PYTHON_BIN:-$(command -v python3 || true)}"
    if [ -z "$python_bin" ]; then
        echo "Error: python3 not found; cannot write iOS FFmpeg provenance manifest." >&2
        exit 1
    fi

    FFMPEG_VERSION="$FFMPEG_VERSION" FFMPEG_TARBALL_SHA256="$FFMPEG_TARBALL_SHA256" \
        SRT_VERSION="$SRT_VERSION" SRT_COMMIT="$SRT_COMMIT" BUILD_CONFIG_ID="$BUILD_CONFIG_ID" \
        "$python_bin" - "$PROVENANCE_MANIFEST" "$BUILD_CONFIG_STAMP" "${EXPECTED_ARCHIVES[@]}" "${EXPECTED_HEADERS[@]}" <<'PY'
import hashlib
import json
import os
import re
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
stamp_path = Path(sys.argv[2])
paths = [Path(value) for value in sys.argv[3:]]
archives = [path for path in paths if "/Headers/" not in path.as_posix()]
headers = [path for path in paths if "/Headers/" in path.as_posix()]

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def header_abi(path: Path) -> int:
    component = path.parent.name
    macro = f"LIB{component.removeprefix('lib').upper()}_VERSION_MAJOR"
    match = re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\s+(\d+)\b", path.read_text(encoding="utf-8"), re.MULTILINE)
    if match is None:
        raise SystemExit(f"cannot determine FFmpeg ABI major from {path}")
    return int(match.group(1))

payload = {
    "archives": {str(path.resolve()): {"sha256": sha256(path)} for path in sorted(archives)},
    "build_config_stamp": os.environ["BUILD_CONFIG_ID"],
    "build_config_stamp_path": str(stamp_path.resolve()),
    "ffmpeg_version": os.environ["FFMPEG_VERSION"],
    "source_identities": {
        "ffmpeg": {
            "sha256": os.environ["FFMPEG_TARBALL_SHA256"],
            "version": os.environ["FFMPEG_VERSION"],
        },
        "srt": {
            "commit": os.environ["SRT_COMMIT"],
            "version": os.environ["SRT_VERSION"],
        },
    },
    "public_headers": {
        str(path.resolve()): {"abi": header_abi(path), "sha256": sha256(path)}
        for path in sorted(headers)
    },
}
manifest_path.parent.mkdir(parents=True, exist_ok=True)
manifest_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
}

validate_cached_provenance() {
    local python_bin

    python_bin="${PYTHON_BIN:-$(command -v python3 || true)}"
    if [ -z "$python_bin" ]; then
        echo "[FFmpeg] python3 is unavailable; cannot verify cached iOS XCFramework provenance." >&2
        return 1
    fi

    FFMPEG_VERSION="$FFMPEG_VERSION" FFMPEG_TARBALL_SHA256="$FFMPEG_TARBALL_SHA256" \
        SRT_VERSION="$SRT_VERSION" SRT_COMMIT="$SRT_COMMIT" BUILD_CONFIG_ID="$BUILD_CONFIG_ID" \
        "$python_bin" - "$PROVENANCE_MANIFEST" "$BUILD_CONFIG_STAMP" "${EXPECTED_ARCHIVES[@]}" "${EXPECTED_HEADERS[@]}" <<'PY'
import hashlib
import json
import os
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
stamp_path = Path(sys.argv[2])
paths = [Path(value) for value in sys.argv[3:]]
archives = [path for path in paths if "/Headers/" not in path.as_posix()]
headers = [path for path in paths if "/Headers/" in path.as_posix()]

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def fail(message: str) -> None:
    raise SystemExit(f"invalid cached iOS XCFramework provenance: {message}")

try:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as error:
    fail(f"cannot read manifest {manifest_path}: {error}")

if manifest.get("ffmpeg_version") != os.environ["FFMPEG_VERSION"]:
    fail("FFmpeg version does not match the locked source identity")
if manifest.get("build_config_stamp") != os.environ["BUILD_CONFIG_ID"]:
    fail("build-config stamp does not match the locked configuration")
if manifest.get("build_config_stamp_path") != str(stamp_path.resolve()):
    fail("build-config stamp path is not canonical")
try:
    if stamp_path.read_text(encoding="utf-8") != os.environ["BUILD_CONFIG_ID"] + "\n":
        fail("build-config stamp contents do not match the locked configuration")
except OSError as error:
    fail(f"cannot read build-config stamp {stamp_path}: {error}")

identities = manifest.get("source_identities")
if not isinstance(identities, dict):
    fail("source identities are missing")
if identities.get("ffmpeg") != {
    "sha256": os.environ["FFMPEG_TARBALL_SHA256"],
    "version": os.environ["FFMPEG_VERSION"],
}:
    fail("FFmpeg source identity does not match the locked source identity")
if identities.get("srt") != {
    "commit": os.environ["SRT_COMMIT"],
    "version": os.environ["SRT_VERSION"],
}:
    fail("SRT source identity does not match the locked source identity")

def verify_records(name: str, records: object, expected_paths: list[Path]) -> None:
    if not isinstance(records, dict):
        fail(f"{name} hashes are missing")
    expected = {str(path.resolve()) for path in expected_paths}
    if set(records) != expected:
        fail(f"{name} entries do not exactly match the expected XCFramework artifacts")
    for path in expected_paths:
        record = records[str(path.resolve())]
        if not isinstance(record, dict) or record.get("sha256") != sha256(path):
            fail(f"{name} SHA-256 does not match: {path}")

try:
    verify_records("archive", manifest.get("archives"), archives)
    verify_records("public header", manifest.get("public_headers"), headers)
except OSError as error:
    fail(f"cannot hash cached XCFramework artifact: {error}")
PY
}

remove_stale_crypto_xcframeworks

# Skip the ~20 minute rebuild when the artifacts already exist — matches the
# CMake custom command's only-when-missing semantics, and lets prebuilt
# xcframeworks be staged into ios_build/xcframeworks/ (e.g. from another
# checkout) without triggering a full rebuild.
if [ -f "$BUILD_CONFIG_STAMP" ] && grep -Fxq "$BUILD_CONFIG_ID" "$BUILD_CONFIG_STAMP" && all_expected_archives_exist && all_expected_headers_exist; then
    if libsrt_has_forbidden_crypto_symbols "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework/ios-arm64/libsrt.a" || \
       libsrt_has_forbidden_crypto_symbols "$ROOT_DIR/ios_build/xcframeworks/libsrt.xcframework/ios-arm64-simulator/libsrt.a"; then
        echo "[FFmpeg] Cached libsrt references OpenSSL/mbedTLS symbols; rebuilding."
        rm -f "$BUILD_CONFIG_STAMP"
    elif validate_cached_provenance; then
        echo "[FFmpeg] Verified prebuilt xcframework provenance; skipping rebuild."
        exit 0
    else
        echo "[FFmpeg] Cached iOS XCFramework provenance is missing or invalid; rebuilding."
        rm -f "$BUILD_CONFIG_STAMP"
    fi
fi

mkdir -p "$SRC_DIR" "$DIST_DIR"

# iOS SDK setup
IOS_MIN_VERSION="13.0"

# CMake (Xcode build environment may not have PATH)
if [ -z "${CMAKE_BIN}" ]; then
    if command -v cmake >/dev/null 2>&1; then
        CMAKE_BIN="$(command -v cmake)"
    elif [ -x "$HOME/Qt/Tools/CMake/CMake.app/Contents/bin/cmake" ]; then
        CMAKE_BIN="$HOME/Qt/Tools/CMake/CMake.app/Contents/bin/cmake"
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
verify_ffmpeg_tarball() {
    local actual_sha256

    actual_sha256="$(shasum -a 256 "$1" | awk '{print $1}')"
    if [ "$actual_sha256" != "$FFMPEG_TARBALL_SHA256" ]; then
        echo "Error: FFmpeg tarball SHA-256 does not match the locked source identity." >&2
        exit 1
    fi
}

verify_srt_source() {
    local actual_commit

    actual_commit="$(git -C "$SRT_SOURCE_DIR" rev-parse HEAD)"
    if [ "$actual_commit" != "$SRT_COMMIT" ]; then
        echo "Error: SRT source commit does not match the locked source identity." >&2
        exit 1
    fi
}

download_src() {
    echo "Downloading sources..."
    mkdir -p "$SRC_DIR"

    echo "Downloading FFmpeg..."
    curl -fL -o "$FFMPEG_TARBALL" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    verify_ffmpeg_tarball "$FFMPEG_TARBALL"
    rm -rf "$FFMPEG_SOURCE_DIR"
    tar -xJf "$FFMPEG_TARBALL" -C "$SRC_DIR"

    if [ -d "$SRT_SOURCE_DIR" ] && ! git -C "$SRT_SOURCE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        rm -rf "$SRT_SOURCE_DIR"
    fi
    if [ ! -d "$SRT_SOURCE_DIR" ]; then
        git clone --no-checkout "https://github.com/Haivision/srt.git" "$SRT_SOURCE_DIR"
    fi
    git -C "$SRT_SOURCE_DIR" fetch --depth 1 origin "$SRT_COMMIT"
    git -C "$SRT_SOURCE_DIR" checkout --detach "$SRT_COMMIT"
    verify_srt_source
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

    cd "$SRT_SOURCE_DIR"
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

    cd "$FFMPEG_SOURCE_DIR"

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
write_provenance_manifest

echo "DONE! XCFrameworks are in: $WORK_DIR/xcframeworks"
