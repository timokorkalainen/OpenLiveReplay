#!/bin/bash
# One-command macOS desktop build of OpenLiveReplay.
#
# Pipeline:
#   1. cmake --preset macos-release   (configure; FFmpeg + SRT from Homebrew)
#   2. cmake --build --preset macos-release
#   3. macdeployqt   (bundle Qt frameworks + QML + dependent dylibs into the .app)
#   4. zip the .app into dist/
#
# Prerequisites (the CI workflow installs these; install locally with Homebrew):
#   brew install ninja ffmpeg srt
#   a Qt 6.x macOS kit (clang_64)
#
# TOOLCHAIN OVERRIDES (env vars; auto-detected otherwise)
#   OLR_QT_ROOT   Qt macOS kit (default: QT_ROOT_DIR, else newest ~/Qt/6.*/macos)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$ROOT_DIR/macos_build/dist"

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

# ------------------------------------------------------------------ build
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
