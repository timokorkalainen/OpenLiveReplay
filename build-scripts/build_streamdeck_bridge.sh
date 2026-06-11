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
    SKIP_INSTALL=NO \
    CODE_SIGNING_ALLOWED=NO \
    BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
    OTHER_SWIFT_FLAGS="-no-verify-emitted-module-interface"

echo "[StreamDeckBridge] Creating xcframework..."
mkdir -p "$OUT_DIR"
rm -rf "$OUT_DIR/StreamDeckBridge.xcframework"
xcodebuild -create-xcframework \
    -framework "$ARCHIVE_PATH/Products/Library/Frameworks/StreamDeckBridge.framework" \
    -output "$OUT_DIR/StreamDeckBridge.xcframework"

HEADER="$OUT_DIR/StreamDeckBridge.xcframework/ios-arm64/StreamDeckBridge.framework/Headers/StreamDeckBridge-Swift.h"
if [ ! -f "$HEADER" ]; then
    echo "Error: generated Objective-C header missing at $HEADER" >&2
    exit 1
fi

echo "[StreamDeckBridge] Done: $OUT_DIR/StreamDeckBridge.xcframework"
