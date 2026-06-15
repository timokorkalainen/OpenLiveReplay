#!/bin/bash
# Build a NATIVE macOS ffmpeg with libsrt, for the LOCAL SRT e2e (not CI).
# Links the already-brew-installed libsrt + openssl@3 (no from-source of those,
# unlike the iOS build). Output: macos_build/ffmpeg-srt/{include,lib} with an
# SRT-enabled libavformat the test engine links via -DOLR_FFMPEG_SRT_PREFIX.
#
# --enable-nonfree (openssl) => this artifact is LOCAL-TEST-ONLY, not redistributable.
set -e

FFMPEG_VERSION="8.0"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/macos_build"
SRC_DIR="$WORK_DIR/src"
DIST="$WORK_DIR/ffmpeg-srt"
# Prefer an existing iOS-build tarball; otherwise download into macos_build/src.
TARBALL="$ROOT_DIR/ios_build/src/ffmpeg-${FFMPEG_VERSION}.tar.bz2"
if [ ! -f "$TARBALL" ]; then
    TARBALL="$SRC_DIR/ffmpeg-${FFMPEG_VERSION}.tar.bz2"
fi

# Idempotent: skip the ~10-min rebuild if already built.
if [ -f "$DIST/lib/libavformat.dylib" ]; then
    echo "[macos-srt] already built at $DIST; skipping (delete macos_build/ to force)."
    exit 0
fi

BREW="$(brew --prefix)"
SRT_PC="$BREW/opt/srt/lib/pkgconfig"
SSL_PC="$BREW/opt/openssl@3/lib/pkgconfig"
[ -f "$SRT_PC/srt.pc" ] || { echo "ERROR: brew libsrt missing — run 'brew install srt'"; exit 1; }
[ -f "$SSL_PC/openssl.pc" ] || { echo "ERROR: brew openssl@3 missing — run 'brew install openssl@3'"; exit 1; }
if [ ! -f "$TARBALL" ]; then
    echo "[macos-srt] downloading ffmpeg-${FFMPEG_VERSION} source..."
    mkdir -p "$SRC_DIR"
    curl -L -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.bz2"
fi

mkdir -p "$SRC_DIR"
if [ ! -d "$SRC_DIR/ffmpeg-${FFMPEG_VERSION}" ]; then
    echo "[macos-srt] extracting ffmpeg-${FFMPEG_VERSION} (fresh native tree)..."
    tar xf "$TARBALL" -C "$SRC_DIR"
fi
cd "$SRC_DIR/ffmpeg-${FFMPEG_VERSION}"
make distclean >/dev/null 2>&1 || true

echo "[macos-srt] configuring (native, --enable-libsrt)..."
PKG_CONFIG_PATH="$SRT_PC:$SSL_PC" ./configure \
    --prefix="$DIST" \
    --enable-gpl --enable-version3 --enable-nonfree \
    --disable-static --enable-shared \
    --disable-doc --disable-programs \
    --disable-avdevice --disable-indevs --disable-outdevs \
    --enable-libsrt --enable-openssl --enable-protocol=libsrt \
    --extra-cflags="-I$BREW/opt/srt/include -I$BREW/opt/openssl@3/include" \
    --extra-ldflags="-L$BREW/opt/srt/lib -L$BREW/opt/openssl@3/lib"

echo "[macos-srt] compiling..."
make -j"$(sysctl -n hw.ncpu)"
make install

# Rewrite ffmpeg's inter-library install names to @rpath so a consumer with an
# rpath to $DIST/lib loads them (libsrt/openssl keep their brew absolute paths).
LIBS=(libavcodec libavformat libavutil libswscale libswresample libavfilter)
for lib in "${LIBS[@]}"; do
    real="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$DIST/lib/$lib.dylib")"
    [ -f "$real" ] || continue
    install_name_tool -id "@rpath/$lib.dylib" "$real"
    otool -L "$real" | grep "$DIST/lib" | awk '{print $1}' | while read -r dep; do
        base="$(basename "$dep")"
        name="$(echo "$base" | sed -E 's/(\.[0-9]+)+\.dylib$/.dylib/')"
        install_name_tool -change "$dep" "@rpath/$name" "$real" || true
    done
done

# Self-check: libsrt MUST be linked into libavformat, else SRT support is absent.
if ! otool -L "$DIST/lib/libavformat.dylib" | grep -qi "libsrt"; then
    echo "ERROR: built libavformat does NOT link libsrt — SRT support missing"
    exit 1
fi
echo "[macos-srt] OK — ffmpeg-srt built at $DIST (libavformat links libsrt)."
