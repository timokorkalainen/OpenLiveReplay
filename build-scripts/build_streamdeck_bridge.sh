#!/bin/bash
set -e

# Builds StreamDeckBridge.xcframework from ios/streamdeck-bridge.
# Output: ios_build/xcframeworks/StreamDeckBridge.xcframework
# Requires: Xcode 15+, xcodegen (brew install xcodegen)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BRIDGE_DIR="$ROOT_DIR/ios/streamdeck-bridge"
OUT_DIR="$ROOT_DIR/ios_build/xcframeworks"
ARCHIVE_PATH="$ROOT_DIR/ios_build/streamdeck-bridge/StreamDeckBridge.xcarchive"

# Injected GIT_CONFIG_* env vars (e.g. from CI or agent harnesses) break
# xcodebuild's SPM git operations — drop them defensively.
unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0

# xcodegen (Xcode build environment may not have PATH)
if [ -z "${XCODEGEN_BIN}" ]; then
    if command -v xcodegen >/dev/null 2>&1; then
        XCODEGEN_BIN="$(command -v xcodegen)"
    elif [ -x "/opt/homebrew/bin/xcodegen" ]; then
        XCODEGEN_BIN="/opt/homebrew/bin/xcodegen"
    else
        echo "Error: xcodegen not found. Install with: brew install xcodegen" >&2
        exit 1
    fi
fi

echo "[StreamDeckBridge] Generating Xcode project..."
(cd "$BRIDGE_DIR" && "$XCODEGEN_BIN" generate)

echo "[StreamDeckBridge] Archiving for iOS device..."
rm -rf "$ARCHIVE_PATH"
xcodebuild archive \
    -project "$BRIDGE_DIR/StreamDeckBridge.xcodeproj" \
    -scheme StreamDeckBridge \
    -destination "generic/platform=iOS" \
    -archivePath "$ARCHIVE_PATH" \
    -derivedDataPath "$ROOT_DIR/ios_build/streamdeck-bridge/DerivedData" \
    SKIP_INSTALL=NO \
    CODE_SIGNING_ALLOWED=NO

# StreamDeckSimulator (statically linked into the framework) loads its assets
# via SPM's Bundle.module, which probes the enclosing framework root at runtime.
# The bundle is built during archive but not folded into the framework, so copy
# it in manually — otherwise showSimulator() crashes with a Bundle.module fatal.
echo "[StreamDeckBridge] Embedding StreamDeckSimulator resource bundle..."
FRAMEWORK_IN_ARCHIVE="$ARCHIVE_PATH/Products/Library/Frameworks/StreamDeckBridge.framework"
SIM_BUNDLE="$(find "$ROOT_DIR/ios_build/streamdeck-bridge/DerivedData" -type d -name "StreamDeckKit_StreamDeckSimulator.bundle" -path "*iphoneos*" | head -1)"
if [ -z "$SIM_BUNDLE" ]; then
    echo "Error: StreamDeckKit_StreamDeckSimulator.bundle not found in DerivedData." >&2
    echo "       StreamDeckSimulator.show() would crash via Bundle.module at runtime." >&2
    exit 1
fi
cp -R "$SIM_BUNDLE" "$FRAMEWORK_IN_ARCHIVE/"

echo "[StreamDeckBridge] Creating xcframework..."
mkdir -p "$OUT_DIR"
rm -rf "$OUT_DIR/StreamDeckBridge.xcframework"
# Consumed via the generated ObjC header only, so module stability (library
# evolution / swiftinterface) is unnecessary; -allow-internal-distribution
# skips the swiftinterface requirement.
xcodebuild -create-xcframework \
    -framework "$ARCHIVE_PATH/Products/Library/Frameworks/StreamDeckBridge.framework" \
    -allow-internal-distribution \
    -output "$OUT_DIR/StreamDeckBridge.xcframework"

HEADER="$OUT_DIR/StreamDeckBridge.xcframework/ios-arm64/StreamDeckBridge.framework/Headers/StreamDeckBridge-Swift.h"
if [ ! -f "$HEADER" ]; then
    echo "Error: generated Objective-C header missing at $HEADER" >&2
    exit 1
fi

SIM_BUNDLE_OUT="$OUT_DIR/StreamDeckBridge.xcframework/ios-arm64/StreamDeckBridge.framework/StreamDeckKit_StreamDeckSimulator.bundle"
if [ ! -d "$SIM_BUNDLE_OUT" ]; then
    echo "Error: simulator resource bundle missing at $SIM_BUNDLE_OUT" >&2
    exit 1
fi

echo "[StreamDeckBridge] Done: $OUT_DIR/StreamDeckBridge.xcframework"
