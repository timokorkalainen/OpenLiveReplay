#!/bin/bash
# Build FFmpeg (with libsrt) + a standalone SRT for the macOS desktop app.
#
# macOS analogue of build_ffmpeg_windows_srt.sh / build_ffmpeg_ios_srt.sh: builds
# SRT and FFmpeg from pinned source so the app links a controlled, SRT-enabled,
# license-clean ffmpeg — NOT Homebrew's ffmpeg (which is --enable-gpl and ships
# libx264/libx265, and is an uncontrolled moving target).
#
# LICENSE: LGPL only. No --enable-gpl, no --enable-nonfree. TLS via Apple
# SecureTransport (no OpenSSL). SRT is built with ENABLE_ENCRYPTION=OFF (the
# native SRT ingest refuses encrypted URLs and falls back to ffmpeg's libsrt).
#
# NOTE: this is distinct from build_ffmpeg_macos_srt.sh, which builds a
# GPL+nonfree (OpenSSL) ffmpeg for the LOCAL SRT e2e test only — not for the app.
#
# Prereqs (CI installs these; locally: brew install ninja pkg-config): a C/C++
# toolchain (Xcode CLT), cmake, ninja, pkg-config.
#
# OUTPUT (consumed by build_macos_app.sh / the CMake macOS branch):
#   macos_build/dist/ffmpeg  ->  OLR_FFMPEG_ROOT
#   macos_build/dist/srt     ->  OLR_SRT_ROOT
set -euo pipefail

# ==============================================================================
# PINNED VERSIONS + INTEGRITY
# ==============================================================================
FFMPEG_VERSION="8.1.1"
FFMPEG_SHA256="b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3"
SRT_VERSION="1.5.4"
SRT_COMMIT="a8c6b65520f814c5bd8f801be48c33ceece7c4a6"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/macos_build"
SRC_DIR="$WORK_DIR/src"
DIST_DIR="$WORK_DIR/dist"
SRT_DIST="$DIST_DIR/srt"
FFMPEG_DIST="$DIST_DIR/ffmpeg"

for tool in cmake ninja pkg-config curl shasum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: '$tool' not found on PATH" >&2; exit 1; }
done
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

mkdir -p "$SRC_DIR" "$DIST_DIR"

# Idempotent: skip if both artifacts already exist.
if [ -f "$FFMPEG_DIST/lib/libavformat.dylib" ] && [ -f "$SRT_DIST/lib/libsrt.dylib" ]; then
    echo "[macos-srt] already built at $DIST_DIR; skipping (delete macos_build/ to force)."
    echo "  OLR_FFMPEG_ROOT=$FFMPEG_DIST"
    echo "  OLR_SRT_ROOT=$SRT_DIST"
    exit 0
fi

verify_sha256() {
    local got; got="$(shasum -a 256 "$1" | cut -d' ' -f1)"
    [ "$got" = "$2" ] || { echo "ERROR: sha256 mismatch for $1 (got $got want $2)" >&2; exit 1; }
}

# ==============================================================================
# 1. FETCH SOURCES (verified)
# ==============================================================================
cd "$SRC_DIR"
if [ ! -f "srt-$SRT_VERSION/CMakeLists.txt" ]; then
    echo "[macos-srt] cloning SRT v$SRT_VERSION..."
    rm -rf "srt-$SRT_VERSION"
    git clone --depth 1 --branch "v$SRT_VERSION" https://github.com/Haivision/srt.git "srt-$SRT_VERSION"
fi
got_commit="$(git -C "srt-$SRT_VERSION" rev-parse HEAD)"
[ "$got_commit" = "$SRT_COMMIT" ] || { echo "ERROR: SRT commit mismatch: $got_commit != $SRT_COMMIT" >&2; exit 1; }

if [ ! -f "ffmpeg-$FFMPEG_VERSION/configure" ]; then
    [ -f "ffmpeg-$FFMPEG_VERSION.tar.xz" ] || {
        echo "[macos-srt] downloading FFmpeg $FFMPEG_VERSION..."
        curl -fL -o "ffmpeg-$FFMPEG_VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"; }
    verify_sha256 "ffmpeg-$FFMPEG_VERSION.tar.xz" "$FFMPEG_SHA256"
    rm -rf "ffmpeg-$FFMPEG_VERSION"
    tar xf "ffmpeg-$FFMPEG_VERSION.tar.xz"
fi

# ==============================================================================
# 2. BUILD SRT (CMake, shared dylib, no encryption)
# ==============================================================================
echo "[macos-srt] building SRT..."
SRT_SRC="$SRC_DIR/srt-$SRT_VERSION"
rm -rf "$SRT_SRC/build-macos"
cmake -S "$SRT_SRC" -B "$SRT_SRC/build-macos" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DENABLE_SHARED=ON -DENABLE_STATIC=OFF \
    -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=OFF \
    -DCMAKE_INSTALL_NAME_DIR=@rpath \
    -DCMAKE_INSTALL_PREFIX="$SRT_DIST"
cmake --build "$SRT_SRC/build-macos"
cmake --install "$SRT_SRC/build-macos"

# ==============================================================================
# 3. BUILD FFMPEG (LGPL, shared, SecureTransport + libsrt, curated codec set)
# ==============================================================================
echo "[macos-srt] building FFmpeg (LGPL)..."
cd "$SRC_DIR/ffmpeg-$FFMPEG_VERSION"
make distclean >/dev/null 2>&1 || true
export PKG_CONFIG_PATH="$SRT_DIST/lib/pkgconfig"

./configure \
    --prefix="$FFMPEG_DIST" \
    --disable-static --enable-shared \
    --disable-gpl --disable-nonfree \
    --disable-doc --disable-programs \
    --disable-avdevice --disable-avfilter \
    --disable-autodetect \
    --enable-network \
    --enable-securetransport \
    --enable-videotoolbox --enable-audiotoolbox \
    --enable-libsrt \
    --enable-protocol=file --enable-protocol=pipe --enable-protocol=libsrt \
    --enable-protocol=tcp --enable-protocol=udp --enable-protocol=crypto \
    --enable-protocol=rtmp --enable-protocol=rtmpt --enable-protocol=rtmps \
    --enable-protocol=tls --enable-protocol=http --enable-protocol=https \
    --enable-demuxer=mpegts --enable-demuxer=matroska --enable-demuxer=mov \
    --enable-demuxer=flv --enable-demuxer=live_flv --enable-demuxer=h264 \
    --enable-demuxer=hevc --enable-demuxer=aac --enable-demuxer=mp3 \
    --enable-demuxer=wav --enable-demuxer=mpegvideo --enable-demuxer=mpegps \
    --enable-muxer=matroska --enable-muxer=mov --enable-muxer=mp4 \
    --enable-muxer=mpegts \
    --enable-parser=h264 --enable-parser=hevc --enable-parser=aac \
    --enable-parser=aac_latm --enable-parser=av1 --enable-parser=vp9 \
    --enable-parser=mpegvideo --enable-parser=mpegaudio --enable-parser=mpeg4video \
    --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=aac \
    --enable-decoder=aac_latm --enable-decoder=aac_at --enable-decoder=av1 \
    --enable-decoder=vp9 --enable-decoder=mpeg2video --enable-decoder=mpeg4 \
    --enable-decoder=mp3 --enable-decoder=ac3 --enable-decoder=pcm_s16le \
    --enable-decoder=pcm_s24le --enable-decoder=rawvideo \
    --enable-encoder=mpeg2video --enable-encoder=pcm_s16le \
    --enable-encoder=mjpeg --enable-encoder=png --enable-encoder=rawvideo \
    --enable-bsf=extract_extradata --enable-bsf=h264_mp4toannexb \
    --enable-bsf=hevc_mp4toannexb --enable-bsf=aac_adtstoasc \
    --enable-bsf=null --enable-bsf=vp9_superframe --enable-bsf=dump_extradata \
    --pkg-config=pkg-config --pkg-config-flags=--static

make -j"$NPROC"
make install

# Rewrite ffmpeg's dylib IDs + inter-library deps to @rpath so the app (with an
# rpath to dist/ffmpeg/lib) loads them and macdeployqt can bundle them.
LIBS=(libavcodec libavformat libavutil libswscale libswresample)
for lib in "${LIBS[@]}"; do
    real="$(cd "$FFMPEG_DIST/lib" && ls "$lib".*.dylib 2>/dev/null | head -1 || true)"
    [ -n "$real" ] || continue
    install_name_tool -id "@rpath/$real" "$FFMPEG_DIST/lib/$real"
    otool -L "$FFMPEG_DIST/lib/$real" | awk '/\/dist\/ffmpeg\/lib\//{print $1}' | while read -r dep; do
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$FFMPEG_DIST/lib/$real" || true
    done
done

# Self-check: libavformat MUST link libsrt.
if ! otool -L "$FFMPEG_DIST/lib/libavformat.dylib" | grep -qi "srt"; then
    echo "ERROR: built libavformat does NOT link libsrt — SRT support missing" >&2
    exit 1
fi

echo ""
echo "[macos-srt] OK. App build will use:"
echo "  OLR_FFMPEG_ROOT=$FFMPEG_DIST"
echo "  OLR_SRT_ROOT=$SRT_DIST"
