#!/bin/bash
# One-command, deterministic Windows build of OpenLiveReplay.
#
# Pipeline:
#   1. build_ffmpeg_windows_srt.sh   (FFmpeg + SRT from source; idempotent)
#   2. cmake --preset windows-mingw-release   (configure)
#   3. cmake --build --preset windows-mingw-release   (compile + link)
#   4. windeployqt + copy FFmpeg/SRT/rtmidi DLLs   (runnable build/ dir)
#
# Run from Git Bash. The only prerequisite is a Qt 6.x "MinGW" kit installed via
# the Qt online installer — it bundles the matching MinGW gcc, CMake and Ninja,
# which this script auto-detects. Everything else is built from pinned sources.
#
# TOOLCHAIN OVERRIDES (env vars; auto-detected otherwise)
#   OLR_QT_ROOT     Qt mingw kit (default: C:/Qt/6.10.2/mingw_64, else newest C:/Qt/6.*/mingw_64)
#   OLR_MINGW_ROOT  MinGW root   (default: C:/Qt/Tools/mingw1310_64)
#   CMAKE_BIN/NINJA_BIN  as in build_ffmpeg_windows_srt.sh
#
# NOTE: a freshly built unsigned .exe will not launch while Windows Smart App
# Control is enforced — see docs/windows-build.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/windows_build"
BUILD_DIR="$ROOT_DIR/build"

winpath() { ( cd "$1" 2>/dev/null && pwd -W ) || echo "$1"; }

# Native path (C:/foo) -> MSYS form (/c/foo). Needed before adding to PATH: Git
# Bash splits PATH on ':', so a "C:/..." entry parses as two bogus entries.
to_msys() {
    local p="${1//\\//}"   # backslashes -> forward slashes (pure bash)
    case "$p" in
        [A-Za-z]:/*) p="/$(printf '%s' "${p%%:*}" | tr 'A-Z' 'a-z')${p#*:}" ;;
    esac
    printf '%s' "$p"
}

# ------------------------------------------------------------------ toolchain
if [ -z "${OLR_QT_ROOT:-}" ]; then
    if [ -d "C:/Qt/6.10.2/mingw_64" ]; then
        OLR_QT_ROOT="C:/Qt/6.10.2/mingw_64"
    else
        # newest C:/Qt/6.*/mingw_64
        OLR_QT_ROOT="$(ls -d C:/Qt/6.*/mingw_64 2>/dev/null | sort -V | tail -1 || true)"
    fi
fi
[ -n "${OLR_QT_ROOT:-}" ] && [ -d "$OLR_QT_ROOT" ] || {
    echo "ERROR: Qt MinGW kit not found; set OLR_QT_ROOT (e.g. C:/Qt/6.10.2/mingw_64)" >&2; exit 1; }

OLR_MINGW_ROOT="${OLR_MINGW_ROOT:-C:/Qt/Tools/mingw1310_64}"
[ -x "$OLR_MINGW_ROOT/bin/gcc.exe" ] || {
    echo "ERROR: MinGW not found at $OLR_MINGW_ROOT (set OLR_MINGW_ROOT)" >&2; exit 1; }

if [ -z "${CMAKE_BIN:-}" ]; then
    if command -v cmake >/dev/null 2>&1; then CMAKE_BIN="$(command -v cmake)";
    elif [ -x "C:/Qt/Tools/CMake_64/bin/cmake.exe" ]; then CMAKE_BIN="C:/Qt/Tools/CMake_64/bin/cmake.exe";
    else echo "ERROR: cmake not found; set CMAKE_BIN" >&2; exit 1; fi
fi
if [ -z "${NINJA_BIN:-}" ]; then
    if command -v ninja >/dev/null 2>&1; then NINJA_BIN="$(command -v ninja)";
    elif [ -x "C:/Qt/Tools/Ninja/ninja.exe" ]; then NINJA_BIN="C:/Qt/Tools/Ninja/ninja.exe";
    else echo "ERROR: ninja not found; set NINJA_BIN" >&2; exit 1; fi
fi
export PATH="$(to_msys "$OLR_MINGW_ROOT")/bin:$(to_msys "$(dirname "$NINJA_BIN")"):$(to_msys "$(dirname "$CMAKE_BIN")"):$PATH"

echo "==> Qt    : $OLR_QT_ROOT"
echo "==> MinGW : $OLR_MINGW_ROOT"

# ------------------------------------------------------------------ 1. deps
QT_MINGW_DIR="$OLR_MINGW_ROOT" CMAKE_BIN="$CMAKE_BIN" NINJA_BIN="$NINJA_BIN" \
    "$SCRIPT_DIR/build_ffmpeg_windows_srt.sh"

# ------------------------------------------------------------------ 2/3. cmake (preset)
export OLR_QT_ROOT OLR_MINGW_ROOT
export OLR_NINJA="$NINJA_BIN"
export OLR_FFMPEG_ROOT="$(winpath "$WORK_DIR/dist/ffmpeg")"
export OLR_SRT_ROOT="$(winpath "$WORK_DIR/dist/srt")"

cd "$ROOT_DIR"   # cmake --preset reads CMakePresets.json from the cwd
echo "==> Configuring (preset windows-mingw-release)"
"$CMAKE_BIN" --preset windows-mingw-release
echo "==> Building"
"$CMAKE_BIN" --build --preset windows-mingw-release

# ------------------------------------------------------------------ 4. deploy
echo "==> Deploying Qt + native runtime DLLs into build/"
"$OLR_QT_ROOT/bin/windeployqt.exe" --qmldir "$(winpath "$ROOT_DIR")" \
    --compiler-runtime --no-translations "$BUILD_DIR/OpenLiveReplay.exe"

cp "$WORK_DIR/dist/ffmpeg/bin/"*.dll "$BUILD_DIR/"
cp "$WORK_DIR/dist/srt/bin/libsrt.dll" "$BUILD_DIR/"
# rtmidi is fetched + built by CMake (FetchContent).
RTMIDI_DLL="$(find "$BUILD_DIR/_deps" -iname 'librtmidi*.dll' 2>/dev/null | head -1)"
[ -n "$RTMIDI_DLL" ] && cp "$RTMIDI_DLL" "$BUILD_DIR/"

echo ""
echo "==> Done: $(winpath "$BUILD_DIR")/OpenLiveReplay.exe"