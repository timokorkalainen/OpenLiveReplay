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

# Bash 3.2 and macOS's BSD userland lack version sorting and associative arrays,
# so compare the numeric Qt installer version components directly.
olr_newest_qt_kit() {
    local suffix="$1"
    local kit version major minor patch key
    local best="" best_key=""
    for kit in "$HOME"/Qt/6.*/"$suffix"; do
        [ -d "$kit" ] || continue
        version="${kit%/$suffix}"
        version="${version##*/}"
        IFS=. read -r major minor patch <<< "$version"
        case "$major" in ''|*[!0-9]*) continue ;; esac
        case "$minor" in ''|*[!0-9]*) continue ;; esac
        case "$patch" in ''|*[!0-9]*) continue ;; esac
        key="$(printf '%09d%09d%09d' "$major" "$minor" "$patch")"
        if [ -z "$best_key" ] || [ "$key" \> "$best_key" ]; then
            best="$kit"
            best_key="$key"
        fi
    done
    printf '%s\n' "$best"
}

# ------------------------------------------------------------------ toolchain
# Qt kit: explicit OLR_QT_ROOT, then QT_ROOT_DIR (install-qt-action in CI), then
# the default Qt-installer layout. No Qt version is hard-coded.
: "${OLR_QT_ROOT:=${QT_ROOT_DIR:-$(olr_newest_qt_kit macos)}}"
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
echo "==> Preserving controlled FFmpeg and SRT dylibs"
cp -R "$OLR_FFMPEG_ROOT/lib/"libavcodec*.dylib "$APP/Contents/Frameworks/"
cp -R "$OLR_FFMPEG_ROOT/lib/"libavformat*.dylib "$APP/Contents/Frameworks/"
cp -R "$OLR_FFMPEG_ROOT/lib/"libavutil*.dylib "$APP/Contents/Frameworks/"
cp -R "$OLR_FFMPEG_ROOT/lib/"libswresample*.dylib "$APP/Contents/Frameworks/"
cp -R "$OLR_FFMPEG_ROOT/lib/"libswscale*.dylib "$APP/Contents/Frameworks/"
cp -R "$OLR_SRT_ROOT/lib/"libsrt*.dylib "$APP/Contents/Frameworks/"

echo "==> Installing package-local Qt configuration"
cp "$ROOT_DIR/qt.conf" "$APP/Contents/Resources/qt.conf"
sed -i '' 's/^Prefix = \.$/Prefix = ../' "$APP/Contents/Resources/qt.conf"
sed -i '' 's/^Plugins = \.$/Plugins = PlugIns/' "$APP/Contents/Resources/qt.conf"
sed -i '' 's|^QmlImports = qml$|QmlImports = Resources/qml|' "$APP/Contents/Resources/qt.conf"

echo "==> Removing Qt FFmpeg plugin and plugin-only FFmpeg runtime"
python "$SCRIPT_DIR/filter_qt_ffmpeg_plugin.py" \
    --package "$APP" \
    --platform macos

echo "==> Auditing controlled FFmpeg and SRT runtime"
EVIDENCE="$DIST_DIR/OpenLiveReplay-macos-evidence.json"
SPDX="$DIST_DIR/OpenLiveReplay-macos.spdx.json"
python "$SCRIPT_DIR/audit_single_ffmpeg.py" \
    --package "$APP" \
    --platform macos \
    --controlled-prefix "ffmpeg=$OLR_FFMPEG_ROOT" \
    --controlled-prefix "srt=$OLR_SRT_ROOT" \
    --evidence "$EVIDENCE" \
    --spdx "$SPDX"

ZIP="$DIST_DIR/OpenLiveReplay-macos.zip"
rm -f "$ZIP"
# ditto preserves macOS bundle attributes / symlinks.
( cd "$BUILD_DIR" && ditto -c -k --keepParent "OpenLiveReplay.app" "$ZIP" )

echo ""
echo "==> Done: $APP"
echo "==> Artifact: $ZIP"
