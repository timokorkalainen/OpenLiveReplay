# Build and Run

This guide covers the common developer paths for OpenLiveReplay: VS Code,
terminal desktop builds, tests, and iOS device builds.

## Prerequisites

- Qt 6.x installed for the target kit. The presets auto-detect standard Qt
  installer paths such as `~/Qt/6.*/macos` and `~/Qt/6.*/gcc_64`.
- Ninja in `PATH`.
- macOS desktop: Homebrew FFmpeg and SRT, or `OLR_FFMPEG_ROOT` /
  `OLR_SRT_ROOT` for custom builds.
- Linux debug desktop: distro FFmpeg development packages discoverable by
  `pkg-config`. Linux release packages build pinned, shared LGPL FFmpeg 8.1.1
  and SRT 1.5.4 with `build-scripts/build_ffmpeg_linux_srt.sh`; the packager
  exports its `linux_build/dist/{ffmpeg,srt}` prefixes automatically.
- Windows MinGW: build the from-source FFmpeg/SRT dependencies first; see
  [windows-build.md](windows-build.md).

OpenLiveReplay uses stock Qt kits as installed. Do not rebuild Qt, Qt Multimedia,
or perform a Qt source build, and do not create a Qt overlay. Vulkan headers are
not a prerequisite for a desktop or iOS build.

### Install stock Qt with aqt

[`aqtinstall`](https://aqtinstall.readthedocs.io/) installs the same stock Qt
archives used by `install-qt-action` in CI. Install the `aqt` command first:

```sh
python3 -m pip install --user aqtinstall
```

Install the desktop kit for the host with the Multimedia, Shader Tools, and
WebSockets modules required by this repository:

```sh
# macOS
aqt install-qt -O "$HOME/Qt" mac desktop 6.11.1 clang_64 \
  -m qtmultimedia qtshadertools qtwebsockets

# Linux
aqt install-qt -O "$HOME/Qt" linux desktop 6.11.1 linux_gcc_64 \
  -m qtmultimedia qtshadertools qtwebsockets
```

On Windows, run these commands in PowerShell. The repository uses the Qt MinGW
kit, not an MSVC kit:

```powershell
py -m pip install --user aqtinstall
aqt install-qt -O C:/Qt windows desktop 6.10.2 win64_mingw `
  -m qtmultimedia qtshadertools qtwebsockets
aqt install-tool -O C:/Qt windows desktop tools_mingw1310
aqt install-tool -O C:/Qt windows desktop tools_ninja
```

Windows CI currently pins the newest MinGW Qt that aqt can resolve, 6.10.2,
because the Qt 6.11 Windows repository layout is not yet resolvable by the aqt
version used in CI. macOS and Linux CI use 6.11.1. When aqt gains Windows 6.11
support, update the Windows pin and keep `win64_mingw` with the matching MinGW
tool package. Set `OLR_QT_ROOT` to the resulting kit directory when it is not in
the standard layout, for example `C:/Qt/6.10.2/mingw_64`.

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
        "CMAKE_PREFIX_PATH": "/path/to/Qt/<version>/macos"
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
./build-scripts/build_linux_app.sh
```

## Single-FFmpeg Runtime Smoke

Desktop packages retain the native Qt media backend (`windows` on Windows,
`darwin` on macOS) while the package scripts remove Qt's FFmpeg media plugin.
They preserve OpenLiveReplay's controlled FFmpeg 8 and SRT runtime, install a
package-local `qt.conf`, and write package audits before archiving. Linux clears
any inherited `QT_MEDIA_BACKEND`, uses Qt's default selection, and fails the
runtime smoke if a Qt FFmpeg backend/plugin is loaded.

Build a release package, then build the smoke harness and run it against that
package. The command audits an isolated package copy, starts raw Qt audio, feeds
two application-owned frames to `VideoOutput`, and records runtime audit evidence:

```sh
# macOS
./build-scripts/build_macos_app.sh
cmake --preset macos-release -DOLR_BUILD_TESTS=ON
cmake --build --preset macos-release --target qt_core_media_smoke
python3 tests/e2e/run_qt_core_media_smoke.py \
  --package build/OpenLiveReplay.app \
  --harness build/qt_core_media_smoke \
  --platform macos \
  --controlled-prefix "ffmpeg=$PWD/macos_build/dist/ffmpeg" \
  --controlled-prefix "srt=$PWD/macos_build/dist/srt" \
  --evidence-dir macos_build/dist/runtime-evidence
```

Use the matching Windows or Linux package path, harness suffix, platform, and
controlled dependency prefixes. Hosted CI passes `--allow-no-audio-device` because
its runners have no physical audio device; local workstation smoke runs require
`audio=started`. The full local pre-push runtime gate is enabled by
`OLR_PREPUSH_FULL=1`; use `SKIP_QT_MEDIA_SMOKE=1` only when that package gate
cannot run, or `OLR_QT_MEDIA_SMOKE_ALLOW_NO_AUDIO=1` for a deliberate headless run.

The evidence directory retains `harness-stdout.log`, `harness-stderr.log`,
`harness-loaded-modules.json`, the equivalent three `packaged-app-*` files,
`harness-media.json`, `runtime-result.json`, the audit JSON, and the SPDX JSON.
Failures also write `runtime-error.txt`, including audit and process-start errors.
Use the files as follows:

- ABI, outside-package path, or forbidden-plugin audit failure: inspect the audit
  JSON and `runtime-error.txt`; rebuild the controlled prefixes and package.
- Backend mismatch or missing native media plugin: inspect
  `harness-loaded-modules.json` and `harness-stderr.log`; the Windows package must
  load package-local `windowsmediaplugin.dll`, macOS a package-local Darwin media
  plugin dylib, and Linux must load no Qt FFmpeg media plugin.
- Missing plugin or QML directory: verify package-local `qt.conf`, the package's
  plugin directory, and QML directory named by the driver's "incomplete" error.
- Startup timeout: inspect both process stdout/stderr logs and
  `runtime-error.txt`; check package-local Qt dependencies before increasing
  `--startup-timeout`.

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

For frame-accurate playback, jog, and scrub-preview validation, including the
macOS visual oracle, >5 second cold-seek checks, PGM NDI output-latency samples,
and iOS local SRT marker oracle, see
[Frame-Accurate Scrub Testing](frame-accurate-scrub-testing.md).

## iOS Device Build

iOS remains Xcode-driven. Use a separate build directory because it uses the
Xcode generator and the iOS Qt kit.

`QT_IOS_PREFIX` and `QT_HOST_PREFIX` point at the installed Qt kits and default
to the standard installer layout (e.g. `~/Qt/6.*/ios` and `~/Qt/6.*/macos`); set
them if your Qt lives elsewhere:

```sh
newest_qt_kit() {
  suffix="$1"
  best=""; best_key=""
  for kit in "$HOME"/Qt/6.*/"$suffix"; do
    [ -d "$kit" ] || continue
    version="${kit%/$suffix}"; version="${version##*/}"
    IFS=. read -r major minor patch <<< "$version"
    case "$major" in ''|*[!0-9]*) continue ;; esac
    case "$minor" in ''|*[!0-9]*) continue ;; esac
    case "$patch" in ''|*[!0-9]*) continue ;; esac
    key="$(printf '%09d%09d%09d' "$major" "$minor" "$patch")"
    if [ -z "$best_key" ] || [ "$key" \> "$best_key" ]; then
      best="$kit"; best_key="$key"
    fi
  done
  printf '%s\n' "$best"
}
: "${QT_IOS_PREFIX:=$(newest_qt_kit ios)}"
: "${QT_HOST_PREFIX:=$(newest_qt_kit macos)}"

"$QT_IOS_PREFIX/bin/qt-cmake" -S . -B build/ios-debug -G Xcode \
  -DQT_HOST_PATH="$QT_HOST_PREFIX" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DOLR_ENABLE_STREAMDECK=ON \
  -DOLR_GPU_PIPELINE=ON \
  -DOLR_GPU_PIPELINE_FORCE_ON=ON
```

For device validation that uses PGM NDI as the external output oracle, install
the NDI SDK for Apple and make NDI mandatory at configure time:

```sh
"$QT_IOS_PREFIX/bin/qt-cmake" -S . -B build/ios-debug -G Xcode \
  -DQT_HOST_PATH="$QT_HOST_PREFIX" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DOLR_ENABLE_STREAMDECK=ON \
  -DOLR_GPU_PIPELINE=ON \
  -DOLR_GPU_PIPELINE_FORCE_ON=ON \
  -DOLR_NDI_IOS_REQUIRED=ON \
  -DOLR_NDI_IOS_SDK_DIR="/Library/NDI SDK for Apple"
```

The iOS build links `lib/iOS/libndi_ios.a` statically and adds the required
Apple frameworks. Do not add `-ldns_sd` on iPhoneOS; Bonjour/DNSService symbols
resolve from the platform libraries there. The app plist must include local
network usage text and `_ndi._tcp.` in `NSBonjourServices` so iOS allows the PGM
NDI sender to advertise on the LAN.

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
Use the GPU flags above for replay/scrub validation so the app exercises the
same PGM-first GPU path that the device oracle tests.
