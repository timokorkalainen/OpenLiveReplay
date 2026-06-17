#!/bin/bash
# Build FFmpeg (with libsrt) + a standalone SRT for the Windows/MinGW app build
# and local native-ingest smoke tests.
#
# Windows analogue of build_ffmpeg_ios_srt.sh: builds SRT from source (CMake) and
# then FFmpeg from source (--enable-libsrt), mirroring the iOS recipe but for the
# MinGW toolchain bundled with Qt for Windows. Run from Git Bash.
#
# DETERMINISM
#   * Pinned versions + integrity: FFmpeg tarball is SHA-256 verified; SRT is
#     cloned at a pinned tag and the checked-out commit is verified.
#   * Self-contained: downloads its own sources, emits its own pkg-config shim,
#     installs to windows_build/dist (delete that dir to force a clean rebuild).
#   * Idempotent: re-running is a no-op once the expected artifacts exist.
#
# WHY THIS DIFFERS FROM THE APPLE SCRIPTS
#   * SRT is built SHARED (libsrt.dll + import lib) with ENABLE_ENCRYPTION=OFF.
#     The native SRT ingest refuses encrypted (passphrase/pbkeylen) URLs and
#     falls back to FFmpeg's own libsrt for those, so no OpenSSL is needed.
#   * FFmpeg uses a CURATED codec set (--disable-everything + explicit enables).
#     A full build's libavcodec emits ~1000 objects and FFmpeg's
#     compat/windows/makedef gets its object list truncated by the Windows
#     command-line length limit under Git Bash, breaking the DLL .def step. The
#     curated set keeps the list small and covers everything the app does
#     (SRT/RTMP/MPEG-TS/file ingest, h264/hevc/aac decode, matroska recording).
#     On Windows the app decodes via Media Foundation, so FFmpeg needs no
#     platform codec integration.
#   * TLS uses --enable-schannel (Windows native) in place of SecureTransport.
#   * x86 asm is disabled (no nasm/yasm in the Qt toolchain).
#
# TOOLCHAIN OVERRIDES (env vars; sensible defaults auto-detected)
#   QT_MINGW_DIR  MinGW root (default: C:/Qt/Tools/mingw1310_64)
#   CMAKE_BIN     cmake     (default: PATH, then C:/Qt/Tools/CMake_64/bin/cmake.exe)
#   NINJA_BIN     ninja     (default: PATH, then C:/Qt/Tools/Ninja/ninja.exe)
#
# OUTPUT (point the app's CMake cache vars here)
#   windows_build/dist/ffmpeg  ->  -DOLR_FFMPEG_ROOT=.../windows_build/dist/ffmpeg
#       also contains ffmpeg.exe + ffprobe.exe for local smoke tests
#   windows_build/dist/srt     ->  -DOLR_SRT_ROOT=.../windows_build/dist/srt
#       also contains srt-live-transmit.exe for local SRT smoke tests
set -euo pipefail

# ==============================================================================
# PINNED VERSIONS + INTEGRITY
# ==============================================================================
FFMPEG_VERSION="8.1.1"
# sha256 of the canonical https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz
FFMPEG_SHA256="b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3"
SRT_VERSION="1.5.4"
# Commit that tag v1.5.4 points at (verified after clone for determinism).
SRT_COMMIT="a8c6b65520f814c5bd8f801be48c33ceece7c4a6"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/windows_build"
SRC_DIR="$WORK_DIR/src"
DIST_DIR="$WORK_DIR/dist"
SRT_DIST="$DIST_DIR/srt"
FFMPEG_DIST="$DIST_DIR/ffmpeg"
TMP_DIR="$WORK_DIR/tmp"
SHIM_DIR="$WORK_DIR/pkgconfig-shim"

# Convert an MSYS/Git-Bash path (/d/foo) to native (D:/foo) so native MinGW gcc
# and Git Bash coreutils agree on it.
winpath() { ( cd "$1" 2>/dev/null && pwd -W ) || echo "$1"; }

# Convert a native path (C:/foo or C:\foo) to MSYS form (/c/foo). Required before
# adding to PATH: Git Bash splits PATH on ':', so a "C:/..." entry is parsed as
# two bogus entries and tools there are never found.
to_msys() {
    local p="${1//\\//}"   # backslashes -> forward slashes (pure bash)
    case "$p" in
        [A-Za-z]:/*) p="/$(printf '%s' "${p%%:*}" | tr 'A-Z' 'a-z')${p#*:}" ;;
    esac
    printf '%s' "$p"
}

# ------------------------------------------------------------------ toolchain
# MinGW root: explicit QT_MINGW_DIR, else derive from a Qt kit (QT_ROOT_DIR set
# by install-qt-action in CI), else the default Qt-installer layout. No MinGW or
# Qt version is hard-coded — all discovered or overridden.
newest() { ls -d "$@" 2>/dev/null | sort -V | tail -1 || true; }
if [ -z "${QT_MINGW_DIR:-}" ]; then
    if [ -n "${QT_ROOT_DIR:-}" ]; then
        QT_MINGW_DIR="$(newest "$QT_ROOT_DIR"/../../Tools/mingw*_64)"
    fi
    [ -n "${QT_MINGW_DIR:-}" ] || QT_MINGW_DIR="$(newest C:/Qt/Tools/mingw*_64)"
fi
[ -n "${QT_MINGW_DIR:-}" ] && [ -x "$QT_MINGW_DIR/bin/gcc.exe" ] || {
    echo "ERROR: MinGW not found; set QT_MINGW_DIR or QT_ROOT_DIR" >&2; exit 1; }
export PATH="$(to_msys "$QT_MINGW_DIR")/bin:$PATH"

if [ -z "${CMAKE_BIN:-}" ]; then
    CMAKE_BIN="$(command -v cmake || true)"
    [ -n "$CMAKE_BIN" ] || CMAKE_BIN="$(newest C:/Qt/Tools/CMake*/bin/cmake.exe)"
fi
[ -n "${CMAKE_BIN:-}" ] || { echo "ERROR: cmake not found; set CMAKE_BIN or add to PATH" >&2; exit 1; }
if [ -z "${NINJA_BIN:-}" ]; then
    NINJA_BIN="$(command -v ninja || true)"
    [ -n "$NINJA_BIN" ] || NINJA_BIN="$(newest C:/Qt/Tools/Ninja/ninja.exe)"
fi
[ -n "${NINJA_BIN:-}" ] || { echo "ERROR: ninja not found; set NINJA_BIN or add to PATH" >&2; exit 1; }
GCC_BIN="$QT_MINGW_DIR/bin/gcc.exe"
GXX_BIN="$QT_MINGW_DIR/bin/g++.exe"
NPROC="$(nproc 2>/dev/null || echo 8)"

echo "[win-srt] toolchain:"
echo "  gcc   : $("$GCC_BIN" -dumpversion) ($GCC_BIN)"
echo "  cmake : $("$CMAKE_BIN" --version | head -1) ($CMAKE_BIN)"
echo "  ninja : $("$NINJA_BIN" --version) ($NINJA_BIN)"

mkdir -p "$SRC_DIR" "$DIST_DIR" "$TMP_DIR" "$SHIM_DIR"

# Idempotent: skip if app libraries and smoke-test tools already exist.
# Delete windows_build/ to force a fully clean rebuild.
if [ -f "$FFMPEG_DIST/bin/avformat-62.dll" ] \
    && [ -f "$FFMPEG_DIST/bin/ffmpeg.exe" ] \
    && [ -f "$FFMPEG_DIST/bin/ffprobe.exe" ] \
    && [ -f "$SRT_DIST/lib/libsrt.dll.a" ] \
    && [ -f "$SRT_DIST/bin/srt-live-transmit.exe" ]; then
    echo "[win-srt] already built at $DIST_DIR; skipping (delete windows_build/ to force)."
    echo "          OLR_FFMPEG_ROOT=$(winpath "$FFMPEG_DIST")"
    echo "          OLR_SRT_ROOT=$(winpath "$SRT_DIST")"
    exit 0
fi

verify_sha256() {
    local file="$1" want="$2" got
    got="$(sha256sum "$file" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        echo "ERROR: sha256 mismatch for $file" >&2
        echo "  expected $want" >&2
        echo "  got      $got" >&2
        exit 1
    fi
}

# ==============================================================================
# 1. FETCH SOURCES (verified)
# ==============================================================================
cd "$SRC_DIR"

# SRT: clone the pinned tag, then verify the checked-out commit. Test for a
# source file (not just the dir) so a partial/interrupted checkout self-heals.
if [ ! -f "srt-$SRT_VERSION/CMakeLists.txt" ]; then
    echo "[win-srt] cloning SRT v$SRT_VERSION..."
    rm -rf "srt-$SRT_VERSION"
    git clone --depth 1 --branch "v$SRT_VERSION" https://github.com/Haivision/srt.git "srt-$SRT_VERSION"
fi
got_commit="$(git -C "srt-$SRT_VERSION" rev-parse HEAD)"
if [ "$got_commit" != "$SRT_COMMIT" ]; then
    echo "ERROR: SRT commit mismatch: expected $SRT_COMMIT got $got_commit" >&2
    exit 1
fi

# FFmpeg: download the pinned tarball, verify sha256, extract. Test for the
# configure script so a partial extraction self-heals.
if [ ! -f "ffmpeg-$FFMPEG_VERSION/configure" ]; then
    if [ ! -f "ffmpeg-$FFMPEG_VERSION.tar.xz" ]; then
        echo "[win-srt] downloading FFmpeg $FFMPEG_VERSION..."
        curl -fL -o "ffmpeg-$FFMPEG_VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    fi
    verify_sha256 "ffmpeg-$FFMPEG_VERSION.tar.xz" "$FFMPEG_SHA256"
    rm -rf "ffmpeg-$FFMPEG_VERSION"
    # Windows bsdtar handles .xz; GNU tar in Git Bash may not.
    "$SYSTEMROOT/System32/tar.exe" -xf "ffmpeg-$FFMPEG_VERSION.tar.xz" 2>/dev/null || tar xf "ffmpeg-$FFMPEG_VERSION.tar.xz"
fi

# ==============================================================================
# 2. BUILD SRT (CMake + MinGW, shared, no encryption, C++11 std threads)
# ==============================================================================
echo "[win-srt] building SRT..."
SRT_SRC="$SRC_DIR/srt-$SRT_VERSION"
SRT_BUILD="$SRT_SRC/build-win"
rm -rf "$SRT_BUILD"
"$CMAKE_BIN" -S "$SRT_SRC" -B "$SRT_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_COMPILER="$GCC_BIN" \
    -DCMAKE_CXX_COMPILER="$GXX_BIN" \
    -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
    -DENABLE_SHARED=ON -DENABLE_STATIC=ON \
    -DENABLE_APPS=ON -DENABLE_ENCRYPTION=OFF \
    -DENABLE_STDCXX_SYNC=ON \
    -DCMAKE_INSTALL_PREFIX="$(winpath "$WORK_DIR")/dist/srt"
"$CMAKE_BIN" --build "$SRT_BUILD"
"$CMAKE_BIN" --install "$SRT_BUILD"

# ==============================================================================
# 3. pkg-config SHIM (the Qt Windows toolchain has none). Serves only "srt", so
#    FFmpeg's configure can locate the SRT we just built; all else reads as
#    "not found" so FFmpeg falls back to its own check_lib probes.
# ==============================================================================
SRT_DIST_WIN="$(winpath "$SRT_DIST")"
cat > "$SHIM_DIR/pkg-config" <<SHIM
#!/bin/bash
SRT_ROOT="$SRT_DIST_WIN"
args="\$*"
case "\$args" in *--version*) echo "0.29.2"; exit 0 ;; esac
case "\$args" in *srt*) ;; *) case "\$args" in *--exists*|*--cflags*|*--libs*|*--modversion*) exit 1 ;; *) exit 0 ;; esac ;; esac
case "\$args" in
  *--exists*) exit 0 ;;
  *--modversion*) echo "$SRT_VERSION"; exit 0 ;;
esac
out=""
case "\$args" in *--cflags*) out="\$out -I\$SRT_ROOT/include" ;; esac
case "\$args" in *--libs*) out="\$out -L\$SRT_ROOT/lib -lsrt"; case "\$args" in *--static*) out="\$out -lws2_32" ;; esac ;; esac
echo "\${out# }"
SHIM
chmod +x "$SHIM_DIR/pkg-config"

# ==============================================================================
# 4. BUILD FFMPEG (curated set, shared, --enable-libsrt, schannel TLS)
# ==============================================================================
echo "[win-srt] building FFmpeg..."
cd "$SRC_DIR/ffmpeg-$FFMPEG_VERSION"
make distclean >/dev/null 2>&1 || true
# Clean forward-slash TMPDIR: Git Bash mangles the default backslash %TEMP%,
# tripping FFmpeg configure's sanity test.
export TMPDIR="$(winpath "$TMP_DIR")"
export PATH="$SHIM_DIR:$PATH"
SHIM_WIN="$(winpath "$SHIM_DIR")/pkg-config"

./configure \
    --prefix="$(winpath "$WORK_DIR")/dist/ffmpeg" \
    --cc=gcc --arch=x86_64 --target-os=mingw32 \
    --pkg-config="$SHIM_WIN" --pkg-config-flags=--static \
    --disable-static --enable-shared \
    --disable-doc --disable-ffplay --enable-ffmpeg --enable-ffprobe \
    --enable-avdevice --enable-avfilter \
    --disable-x86asm \
    --disable-everything \
    --enable-network --enable-schannel --enable-mediafoundation \
    --enable-libsrt \
    --enable-indev=lavfi \
    --enable-protocol=file --enable-protocol=pipe --enable-protocol=libsrt \
    --enable-protocol=tcp --enable-protocol=udp --enable-protocol=crypto \
    --enable-protocol=rtmp --enable-protocol=rtmpt --enable-protocol=rtmps \
    --enable-protocol=tls --enable-protocol=http --enable-protocol=https \
    --enable-demuxer=mpegts --enable-demuxer=matroska --enable-demuxer=mov \
    --enable-demuxer=flv --enable-demuxer=live_flv --enable-demuxer=h264 \
    --enable-demuxer=hevc --enable-demuxer=aac --enable-demuxer=mp3 \
    --enable-demuxer=wav --enable-demuxer=mpegvideo --enable-demuxer=mpegps \
    --enable-muxer=matroska --enable-muxer=mov --enable-muxer=mp4 \
    --enable-muxer=mpegts --enable-muxer=flv --enable-muxer=null \
    --enable-parser=h264 --enable-parser=hevc --enable-parser=aac \
    --enable-parser=aac_latm --enable-parser=av1 --enable-parser=vp9 \
    --enable-parser=mpegvideo --enable-parser=mpegaudio --enable-parser=mpeg4video \
    --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=aac \
    --enable-decoder=aac_latm --enable-decoder=av1 --enable-decoder=vp9 \
    --enable-decoder=mpeg2video --enable-decoder=mpeg4 --enable-decoder=mp3 \
    --enable-decoder=ac3 --enable-decoder=pcm_s16le --enable-decoder=pcm_s24le \
    --enable-decoder=rawvideo --enable-decoder=wrapped_avframe \
    --enable-encoder=h264_mf --enable-encoder=aac \
    --enable-encoder=mpeg2video --enable-encoder=pcm_s16le \
    --enable-encoder=mjpeg --enable-encoder=png --enable-encoder=rawvideo \
    --enable-filter=testsrc2 --enable-filter=sine --enable-filter=color \
    --enable-filter=geq --enable-filter=signalstats \
    --enable-filter=astats --enable-filter=aresample --enable-filter=aformat \
    --enable-filter=format --enable-filter=scale --enable-filter=null \
    --enable-filter=anull \
    --enable-bsf=extract_extradata --enable-bsf=h264_mp4toannexb \
    --enable-bsf=hevc_mp4toannexb --enable-bsf=aac_adtstoasc \
    --enable-bsf=null --enable-bsf=vp9_superframe --enable-bsf=dump_extradata

mingw32-make -j"$NPROC"
mingw32-make install

# Self-check: libavformat MUST link libsrt, else SRT support is absent.
if command -v objdump >/dev/null 2>&1; then
    if ! objdump -p "$FFMPEG_DIST/bin/avformat-62.dll" | grep -qi "libsrt.dll"; then
        echo "ERROR: built avformat does NOT link libsrt — SRT support missing" >&2
        exit 1
    fi
fi

echo ""
echo "[win-srt] OK. Configure the app with:"
echo "    -DOLR_FFMPEG_ROOT=$(winpath "$FFMPEG_DIST")"
echo "    -DOLR_SRT_ROOT=$(winpath "$SRT_DIST")"
echo "  Smoke tools: $FFMPEG_DIST/bin/ffmpeg.exe, $FFMPEG_DIST/bin/ffprobe.exe, $SRT_DIST/bin/srt-live-transmit.exe"
