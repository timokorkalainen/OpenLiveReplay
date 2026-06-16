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

# Any path -> native (C:/foo). CMake needs native paths for compiler/prefix.
to_native() {
    if [ -d "$1" ]; then ( cd "$1" && pwd -W );
    elif [ -e "$1" ]; then printf '%s/%s' "$( cd "$(dirname "$1")" && pwd -W )" "$(basename "$1")";
    else printf '%s' "$1"; fi
}
newest() { ls -d "$@" 2>/dev/null | sort -V | tail -1 || true; }

# ------------------------------------------------------------------ toolchain
# Resolve the Qt MinGW kit. Order: explicit OLR_QT_ROOT, then QT_ROOT_DIR (set by
# jurplel/install-qt-action in CI), then the default Qt-installer layout. No Qt
# version or MinGW version is hard-coded — everything is discovered or overridden.
: "${OLR_QT_ROOT:=${QT_ROOT_DIR:-$(newest C:/Qt/6.*/mingw_64)}}"
[ -n "${OLR_QT_ROOT:-}" ] && [ -d "$OLR_QT_ROOT" ] || {
    echo "ERROR: Qt MinGW kit not found; set OLR_QT_ROOT or QT_ROOT_DIR" >&2; exit 1; }
OLR_QT_ROOT="$(to_native "$OLR_QT_ROOT")"

# Qt "Tools" dir is a sibling of the version dir: <base>/Tools
QT_TOOLS_DIR="$(to_native "$OLR_QT_ROOT/../..")/Tools"

# MinGW root: explicit, else newest mingw*_64 under the Qt Tools dir, else default layout.
: "${OLR_MINGW_ROOT:=$(newest "$QT_TOOLS_DIR"/mingw*_64)}"
[ -n "${OLR_MINGW_ROOT:-}" ] || OLR_MINGW_ROOT="$(newest C:/Qt/Tools/mingw*_64)"
[ -n "${OLR_MINGW_ROOT:-}" ] && [ -x "$OLR_MINGW_ROOT/bin/gcc.exe" ] || {
    echo "ERROR: MinGW not found under $QT_TOOLS_DIR; set OLR_MINGW_ROOT" >&2; exit 1; }
OLR_MINGW_ROOT="$(to_native "$OLR_MINGW_ROOT")"

# cmake / ninja: PATH first (CI provides them), then the Qt Tools dir.
if [ -z "${CMAKE_BIN:-}" ]; then
    CMAKE_BIN="$(command -v cmake || true)"
    [ -n "$CMAKE_BIN" ] || CMAKE_BIN="$(newest "$QT_TOOLS_DIR"/CMake*/bin/cmake.exe)"
fi
[ -n "${CMAKE_BIN:-}" ] || { echo "ERROR: cmake not found; set CMAKE_BIN or add to PATH" >&2; exit 1; }
if [ -z "${NINJA_BIN:-}" ]; then
    NINJA_BIN="$(command -v ninja || true)"
    [ -n "$NINJA_BIN" ] || NINJA_BIN="$(newest "$QT_TOOLS_DIR"/Ninja/ninja.exe)"
fi
[ -n "${NINJA_BIN:-}" ] || { echo "ERROR: ninja not found; set NINJA_BIN or add to PATH" >&2; exit 1; }

export PATH="$(to_msys "$OLR_MINGW_ROOT")/bin:$(to_msys "$(dirname "$NINJA_BIN")"):$(to_msys "$(dirname "$CMAKE_BIN")"):$PATH"

echo "==> Qt    : $OLR_QT_ROOT"
echo "==> MinGW : $OLR_MINGW_ROOT"
echo "==> cmake : $CMAKE_BIN"
echo "==> ninja : $NINJA_BIN"

# ------------------------------------------------------------------ 1. deps
QT_MINGW_DIR="$OLR_MINGW_ROOT" CMAKE_BIN="$CMAKE_BIN" NINJA_BIN="$NINJA_BIN" \
    "$SCRIPT_DIR/build_ffmpeg_windows_srt.sh"

# ------------------------------------------------------------------ 2/3. cmake (preset)
# The preset reads these as native paths (CMake rejects MSYS /c/... compiler paths).
export OLR_QT_ROOT OLR_MINGW_ROOT
export OLR_NINJA="$(to_native "$NINJA_BIN")"
export OLR_FFMPEG_ROOT="$(winpath "$WORK_DIR/dist/ffmpeg")"
export OLR_SRT_ROOT="$(winpath "$WORK_DIR/dist/srt")"

cd "$ROOT_DIR"   # cmake --preset reads CMakePresets.json from the cwd
echo "==> Configuring (preset windows-mingw-release)"
"$CMAKE_BIN" --preset windows-mingw-release
echo "==> Building"
"$CMAKE_BIN" --build --preset windows-mingw-release

# ------------------------------------------------------------------ 4. deploy
# Assemble a clean, self-contained app dir (kept separate from the raw build
# tree) and zip it as the distributable artifact.
echo "==> Assembling self-contained app dir"
APPDIR="$WORK_DIR/dist/OpenLiveReplay"
rm -rf "$APPDIR"; mkdir -p "$APPDIR"
cp "$BUILD_DIR/OpenLiveReplay.exe" "$APPDIR/"
cp "$WORK_DIR/dist/ffmpeg/bin/"*.dll "$APPDIR/"
cp "$WORK_DIR/dist/srt/bin/libsrt.dll" "$APPDIR/"
# rtmidi is fetched + built by CMake (FetchContent).
RTMIDI_DLL="$(find "$BUILD_DIR/_deps" -iname 'librtmidi*.dll' 2>/dev/null | head -1)"
[ -n "$RTMIDI_DLL" ] && cp "$RTMIDI_DLL" "$APPDIR/"

echo "==> windeployqt (Qt DLLs, plugins, QML, compiler runtime)"
"$OLR_QT_ROOT/bin/windeployqt.exe" --qmldir "$(winpath "$ROOT_DIR")" \
    --compiler-runtime --no-translations "$APPDIR/OpenLiveReplay.exe"

echo "==> Packaging zip"
ZIP="$WORK_DIR/dist/OpenLiveReplay-windows.zip"
rm -f "$ZIP"
( cd "$WORK_DIR/dist" && "$CMAKE_BIN" -E tar cf "$ZIP" --format=zip OpenLiveReplay )

echo ""
echo "==> Done: $APPDIR/OpenLiveReplay.exe"
echo "==> Artifact: $ZIP"