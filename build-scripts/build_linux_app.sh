#!/bin/bash
# Build a self-contained Linux AppDir from the controlled desktop dependencies.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$ROOT_DIR/linux_build"
DIST_DIR="$WORK_DIR/dist"
BUILD_DIR="$ROOT_DIR/build"
APPDIR="$DIST_DIR/OpenLiveReplay"

newest() { ls -d "$@" 2>/dev/null | sort -V | tail -1 || true; }
: "${OLR_QT_ROOT:=${QT_ROOT_DIR:-$(newest "$HOME"/Qt/6.*/gcc_64)}}"
[ -d "$OLR_QT_ROOT" ] || { echo "ERROR: Qt Linux kit not found; set OLR_QT_ROOT or QT_ROOT_DIR" >&2; exit 1; }
: "${OLR_FFMPEG_ROOT:=$DIST_DIR/ffmpeg}"
: "${OLR_SRT_ROOT:=$DIST_DIR/srt}"
if [ ! -d "$OLR_FFMPEG_ROOT" ] || [ ! -d "$OLR_SRT_ROOT" ]; then
    echo "==> Building absent controlled Linux FFmpeg/SRT prefixes"
    "$SCRIPT_DIR/build_ffmpeg_linux_srt.sh"
fi
[ -d "$OLR_FFMPEG_ROOT" ] || {
    echo "ERROR: controlled FFmpeg prefix is absent after the builder ran: $OLR_FFMPEG_ROOT" >&2; exit 1; }
[ -d "$OLR_SRT_ROOT" ] || {
    echo "ERROR: controlled SRT prefix is absent after the builder ran: $OLR_SRT_ROOT" >&2; exit 1; }
for tool in cmake ninja patchelf python; do
    command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool is required" >&2; exit 1; }
done

QT_PATHS="$OLR_QT_ROOT/bin/qtpaths"
[ -x "$QT_PATHS" ] || QT_PATHS="$(command -v qtpaths6 || true)"
[ -n "$QT_PATHS" ] || { echo "ERROR: qtpaths is required from the selected Qt kit" >&2; exit 1; }
QT_PLUGIN_DIR="$("$QT_PATHS" --query QT_INSTALL_PLUGINS)"
QT_QML_DIR="$("$QT_PATHS" --query QT_INSTALL_QML)"
QT_LIB_DIR="$("$QT_PATHS" --query QT_INSTALL_LIBS)"

export OLR_QT_ROOT OLR_FFMPEG_ROOT OLR_SRT_ROOT
cd "$ROOT_DIR"
echo "==> Configuring (preset linux-release)"
cmake --preset linux-release
echo "==> Building"
cmake --build --preset linux-release

echo "==> Assembling AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/plugins" "$APPDIR/usr/qml"
cp "$BUILD_DIR/bin/OpenLiveReplay" "$APPDIR/usr/bin/OpenLiveReplay.bin"
cat > "$APPDIR/usr/bin/OpenLiveReplay" <<'EOF'
#!/bin/sh
unset LD_LIBRARY_PATH
unset QT_PLUGIN_PATH
unset QT_QPA_PLATFORM_PLUGIN_PATH
unset QML2_IMPORT_PATH
unset QML_IMPORT_PATH
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/OpenLiveReplay.bin" "$@"
EOF
chmod 0755 "$APPDIR/usr/bin/OpenLiveReplay"
cp -a "$QT_LIB_DIR/"libQt6*.so* "$APPDIR/usr/lib/"
for plugin in \
    platforms platforminputcontexts imageformats iconengines tls audio \
    xcbglintegrations egldeviceintegrations scenegraph \
    wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
    [ -d "$QT_PLUGIN_DIR/$plugin" ] || continue
    cp -a "$QT_PLUGIN_DIR/$plugin" "$APPDIR/usr/plugins/"
done
for module in Qt QtQml QtQuick QtMultimedia; do
    [ -d "$QT_QML_DIR/$module" ] || continue
    cp -a "$QT_QML_DIR/$module" "$APPDIR/usr/qml/"
done
for module in OlrTheme OlrStyle; do
    [ -d "$BUILD_DIR/qml/$module" ] || { echo "ERROR: $module QML module is missing" >&2; exit 1; }
    cp -a "$BUILD_DIR/qml/$module" "$APPDIR/usr/qml/"
done
cp -a "$OLR_FFMPEG_ROOT/lib/"libav*.so* "$APPDIR/usr/lib/"
cp -a "$OLR_FFMPEG_ROOT/lib/"libsw*.so* "$APPDIR/usr/lib/"
cp -a "$OLR_SRT_ROOT/lib/"libsrt.so* "$APPDIR/usr/lib/"

echo "==> Installing package-local Qt configuration"
cp "$ROOT_DIR/qt.conf" "$APPDIR/usr/bin/qt.conf"
sed -i 's/^Prefix = \.$/Prefix = ../' "$APPDIR/usr/bin/qt.conf"
sed -i 's/^Plugins = \.$/Plugins = plugins/' "$APPDIR/usr/bin/qt.conf"

echo "==> Setting package-local runtime search paths"
patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/OpenLiveReplay.bin"
while IFS= read -r library; do
    relative_lib="$(python -c 'import os, sys; print(os.path.relpath(sys.argv[2], sys.argv[1]))' \
        "$(dirname "$library")" "$APPDIR/usr/lib")"
    if [ "$relative_lib" = "." ]; then
        library_rpath='$ORIGIN'
    else
        library_rpath="\$ORIGIN/$relative_lib"
    fi
    patchelf --force-rpath --set-rpath "$library_rpath" "$library"
done < <(find "$APPDIR/usr" -type f -name '*.so*' | sort)

echo "==> Removing Qt FFmpeg plugin and plugin-only FFmpeg runtime"
python "$SCRIPT_DIR/filter_qt_ffmpeg_plugin.py" \
    --package "$APPDIR" \
    --platform linux \
    --allow-absent

echo "==> Auditing controlled FFmpeg and SRT runtime"
EVIDENCE="$DIST_DIR/OpenLiveReplay-linux-evidence.json"
SPDX="$DIST_DIR/OpenLiveReplay-linux.spdx.json"
python "$SCRIPT_DIR/audit_single_ffmpeg.py" \
    --package "$APPDIR" \
    --platform linux \
    --controlled-prefix "ffmpeg=$OLR_FFMPEG_ROOT" \
    --controlled-prefix "srt=$OLR_SRT_ROOT" \
    --evidence "$EVIDENCE" \
    --spdx "$SPDX"

echo "==> Packaging tarball"
ARCHIVE="$DIST_DIR/OpenLiveReplay-linux.tar.gz"
rm -f "$ARCHIVE"
tar -czf "$ARCHIVE" -C "$DIST_DIR" OpenLiveReplay
echo "==> Done: $ARCHIVE"
