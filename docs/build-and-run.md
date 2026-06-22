# Build and Run

This guide covers the common developer paths for OpenLiveReplay: VS Code,
terminal desktop builds, tests, and iOS device builds.

## Prerequisites

- Qt 6.x installed for the target kit. The presets auto-detect standard Qt
  installer paths such as `~/Qt/6.*/macos` and `~/Qt/6.*/gcc_64`.
- Ninja in `PATH`.
- macOS desktop: Homebrew FFmpeg and SRT, or `OLR_FFMPEG_ROOT` /
  `OLR_SRT_ROOT` for custom builds.
- Linux desktop: distro FFmpeg development packages discoverable by
  `pkg-config`.
- Windows MinGW: build the from-source FFmpeg/SRT dependencies first; see
  [windows-build.md](windows-build.md).

For non-standard Qt installs, either export `OLR_QT_ROOT` or create a local,
gitignored `CMakeUserPresets.json`:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "macos-debug-local",
      "inherits": "macos-debug",
      "cacheVariables": {
        "CMAKE_PREFIX_PATH": "/path/to/Qt/6.10.1/macos"
      }
    }
  ]
}
```

## VS Code

The repo includes committed VS Code configuration in [.vscode](../.vscode):

- [extensions.json](../.vscode/extensions.json) recommends CMake Tools,
  Microsoft C++ tools, and the official Qt C++/QML extensions.
- [settings.json](../.vscode/settings.json) forces CMake preset mode and copies
  `compile_commands.json` to the workspace root.
- [tasks.json](../.vscode/tasks.json) provides the default build task
  `CMake: build app`, a full build task, and a CTest task.
- [launch.json](../.vscode/launch.json) provides app-specific C++ and QML debug
  configurations using the active CMake preset.

Use it like this:

1. Open the repository folder in VS Code and install the recommended extensions.
2. Select the Debug configure preset for your platform, such as
   `macos-debug`, `linux-debug`, or `windows-mingw-debug`.
3. Build with the CMake Tools status bar or `Cmd+Shift+B` / `Ctrl+Shift+B`.
4. Run or debug with `OpenLiveReplay: Debug`.
5. Use `OpenLiveReplay: Debug C++ and QML` when QML debugger support is needed.

## Desktop Terminal Build

macOS:

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug --target OpenLiveReplay
open build/debug/OpenLiveReplay.app
```

Linux:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug --target OpenLiveReplay
./build/debug/OpenLiveReplay
```

Windows MinGW, from Git Bash after the Windows dependency setup:

```sh
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug --target OpenLiveReplay
./build/debug/OpenLiveReplay.exe
```

Release packaging scripts remain available for local package checks:

```sh
./build-scripts/build_macos_app.sh
./build-scripts/build_windows_app.sh
```

## Tests

Build the debug tree with tests enabled, then run CTest through the preset:

```sh
cmake --build --preset macos-debug
ctest --preset macos-debug
```

Use the matching `linux-debug` or `windows-mingw-debug` preset on those hosts.
For a focused unit-only run:

```sh
ctest --test-dir build/debug -L unit --output-on-failure
```

## iOS Device Build

iOS remains Xcode-driven. Use a separate build directory because it uses the
Xcode generator and the iOS Qt kit.

```sh
$HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B build/ios-debug -G Xcode \
  -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DOLR_ENABLE_STREAMDECK=ON
```

Find the paired device identifier:

```sh
xcrun devicectl list devices
```

Build, sign, install, and launch:

```sh
xcodebuild -project build/ios-debug/OpenLiveReplay.xcodeproj \
  -scheme OpenLiveReplay \
  -configuration Debug \
  -destination 'id=<device-udid>' \
  -allowProvisioningUpdates \
  -allowProvisioningDeviceRegistration \
  DEVELOPMENT_TEAM=<team-id> \
  build

xcrun devicectl device install app \
  --device <device-udid> \
  build/ios-debug/Debug-iphoneos/OpenLiveReplay.app

xcrun devicectl device process launch \
  --device <device-udid> \
  --terminate-existing \
  com.timokorkalainen.OpenLiveReplay
```

Use `-DOLR_ENABLE_STREAMDECK=ON` when validating StreamDeck integration.
