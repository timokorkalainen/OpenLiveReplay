#!/bin/bash
# One-command macOS desktop build of OpenLiveReplay.
#
# Pipeline:
#   1. build_ffmpeg_macos_app_srt.sh   (LGPL FFmpeg + SRT from source; idempotent)
#   2. cmake --preset macos-release   (configure)
#   3. cmake --build --preset macos-release
#   4. macdeployqt   (bundle Qt frameworks + QML + ffmpeg/srt dylibs into the .app)
#   5. zip the .app into dist/
#
# FFmpeg + SRT are built FROM SOURCE (not Homebrew) so the bundled ffmpeg is a
# controlled, SRT-enabled, LGPL build — Homebrew's ffmpeg is GPL and ships
# libx264/libx265.
#
# Prerequisites (CI installs these; locally: brew install ninja pkg-config):
#   a C/C++ toolchain (Xcode CLT), cmake, ninja, pkg-config, and a Qt 6.x macOS kit.
#
# TOOLCHAIN OVERRIDES (env vars; auto-detected otherwise)
#   OLR_QT_ROOT   Qt macOS kit (default: QT_ROOT_DIR, else newest ~/Qt/6.*/macos)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
WORK_DIR="$ROOT_DIR/macos_build"
DIST_DIR="$WORK_DIR/dist"

newest() { ls -d "$@" 2>/dev/null | sort -V | tail -1 || true; }

# ------------------------------------------------------------------ toolchain
# Qt kit: explicit OLR_QT_ROOT, then QT_ROOT_DIR (install-qt-action in CI), then
# the default Qt-installer layout. No Qt version is hard-coded.
: "${OLR_QT_ROOT:=${QT_ROOT_DIR:-$(newest "$HOME"/Qt/6.*/macos)}}"
[ -n "${OLR_QT_ROOT:-}" ] && [ -d "$OLR_QT_ROOT" ] || {
    echo "ERROR: Qt macOS kit not found; set OLR_QT_ROOT or QT_ROOT_DIR" >&2; exit 1; }
export OLR_QT_ROOT

command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake not on PATH" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "ERROR: ninja not on PATH (brew install ninja)" >&2; exit 1; }

echo "==> Qt    : $OLR_QT_ROOT"

# ------------------------------------------------------------------ 1. deps (from source)
"$SCRIPT_DIR/build_ffmpeg_macos_app_srt.sh"
export OLR_FFMPEG_ROOT="$DIST_DIR/ffmpeg"
export OLR_SRT_ROOT="$DIST_DIR/srt"

# ------------------------------------------------------------------ 2/3. build
cd "$ROOT_DIR"   # cmake --preset reads CMakePresets.json from the cwd
echo "==> Configuring (preset macos-release)"
cmake --preset macos-release
echo "==> Building"
cmake --build --preset macos-release

APP="$BUILD_DIR/OpenLiveReplay.app"
[ -d "$APP" ] || { echo "ERROR: $APP not produced" >&2; exit 1; }

# ------------------------------------------------------------------ deploy + package
echo "==> macdeployqt (bundling Qt frameworks, QML, dependent dylibs)"
"$OLR_QT_ROOT/bin/macdeployqt" "$APP" -qmldir="$ROOT_DIR"

mkdir -p "$DIST_DIR"
ZIP="$DIST_DIR/OpenLiveReplay-macos.zip"
rm -f "$ZIP"
# ditto preserves macOS bundle attributes / symlinks.
( cd "$BUILD_DIR" && ditto -c -k --keepParent "OpenLiveReplay.app" "$ZIP" )

echo ""
echo "==> Done: $APP"
echo "==> Artifact: $ZIP"
