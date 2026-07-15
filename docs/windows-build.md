# Building OpenLiveReplay on Windows

The Windows app builds with the **Qt MinGW kit** plus FFmpeg and SRT compiled
from pinned, integrity-verified sources — analogous to `build_ffmpeg_ios_srt.sh`
(iOS) and `build_ffmpeg_macos_app_srt.sh` (macOS). All three produce a
controlled, **LGPL**, SRT-enabled ffmpeg rather than a distro package (e.g.
Homebrew's ffmpeg is GPL and ships libx264/libx265).

## Prerequisites

Install **Qt 6.x for Windows with the MinGW kit** via the
[Qt online installer](https://www.qt.io/download-qt-installer). Select:

- `Qt 6.11.x > MinGW 13.1.0 64-bit`
- `Developer and Designer Tools > MinGW 13.1.0 64-bit`, `CMake`, `Ninja`

That kit bundles the matching `gcc`, `cmake`, and `ninja`; the scripts
auto-detect them under `C:/Qt`. No MSVC, vcpkg, or MSYS2 is required. You only
need **Git for Windows** (for Git Bash, used to run the scripts and build FFmpeg).
Use the installed stock Qt kit as-is: OpenLiveReplay does not rebuild Qt,
Qt Multimedia, or perform a Qt source build, and Vulkan headers are not required.

## One command

From **Git Bash** at the repo root:

```bash
./build-scripts/build_windows_app.sh
```

This will, deterministically and idempotently:

1. Build **SRT** (`v1.5.4`) and **FFmpeg** (`8.1.1`, with `libsrt`) from source
   into `windows_build/dist/` — see `build-scripts/build_ffmpeg_windows_srt.sh`.
   The dependency step also installs `ffmpeg.exe`, `ffprobe.exe`, and
   `srt-live-transmit.exe` so local native ingest smoke tests can run from the
   same pinned toolchain.
2. Configure with the `windows-mingw-release` CMake preset.
3. Compile and link `build/OpenLiveReplay.exe`.
4. Run `windeployqt` and assemble a self-contained bundle at
   `windows_build/dist/OpenLiveReplay/`, zipped to
   `windows_build/dist/OpenLiveReplay-windows.zip`.

Re-running is cheap: the dependency build skips when its artifacts already exist
(delete `windows_build/` to force a clean rebuild).

### Toolchain overrides

The toolchain is auto-discovered: no Qt or MinGW version is hard-coded. Override
any of these if your install differs:

| Variable | How it's resolved |
| --- | --- |
| `OLR_QT_ROOT` | env → `QT_ROOT_DIR` (set by `install-qt-action` in CI) → newest `C:/Qt/6.*/mingw_64` |
| `OLR_MINGW_ROOT` | env → newest `mingw*_64` under the Qt `Tools` dir → newest `C:/Qt/Tools/mingw*_64` |
| `CMAKE_BIN` / `NINJA_BIN` | from `PATH`, else under the Qt `Tools` dir |

## Continuous integration

`.github/workflows/build.yml` is a manually-triggered (`workflow_dispatch`)
workflow that runs this same script on a clean Windows runner (and builds the
macOS app via `build-scripts/build_macos_app.sh`), uploading each as an artifact.
Running on a fresh runner is the proof that the build has no dependency on any
one developer's machine.

## Manual / IDE build

The dependency step and the preset are usable on their own:

```bash
./build-scripts/build_ffmpeg_windows_srt.sh          # build deps once
export OLR_QT_ROOT=C:/Qt/6.11.1/mingw_64
export OLR_MINGW_ROOT=C:/Qt/Tools/mingw1310_64
export OLR_NINJA=C:/Qt/Tools/Ninja/ninja.exe
export OLR_FFMPEG_ROOT="$PWD/windows_build/dist/ffmpeg"
export OLR_SRT_ROOT="$PWD/windows_build/dist/srt"
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
```

To build the test harnesses and run local smoke tests from the same dependency
tools:

```bash
cmake --preset windows-mingw-release -DOLR_BUILD_TESTS=ON
cmake --build --preset windows-mingw-release --target record_harness
python tools/run_ctest.py --test-dir build \
  -R "e2e_native_(srt|rtmp)_smoke" --output-on-failure
```

All Windows tests and diagnostic probes must be selected through CTest via
`tools/run_ctest.py`; do not launch their executables directly. The launcher
uses the compiler, Qt, FFmpeg, and SRT paths recorded in `CMakeCache.txt`, sets
the noninteractive Qt runtime, prepends Qt's QML module directory without
discarding test-specific imports, suppresses Windows crash dialogs, and
propagates CTest's exit code. It accepts ordinary CTest arguments, for example:

```bash
python tools/run_ctest.py --test-dir build -L unit --output-on-failure
python tools/run_ctest.py --test-dir build -R '^rhi_d3d_probe$' --output-on-failure
```

The supported launcher snapshots the literal CTest graph, test executables, a
minimal loader closure (runtime DLLs and selected tools), runtime-bearing Qt
plugin/module files, and QuickTest source closure into read/execute-only
temporary layouts for the child lifetime. It does not copy an entire Qt or
CMake installation for a selected tool. Parsed path tokens are rewritten by
Windows path identity and each test's configured working directory is restored.
Other external tools declare install-relative resource closure in an adjacent
`<tool>.exe.olr-runtime-files` file instead of implicitly copying their prefix.
Absent `subdirs()` files and unsupported dynamic or external QML inputs are
errors rather than names that may appear after the audit. Descendants inherit a
protected `SetDllDirectory` runtime and a system-only tail on `PATH`, so their
application, current-directory, and configured-runtime DLL namespaces cannot
gain a late unaudited DLL. Snapshots are removed after the complete CTest
process tree exits.
Fixture-bearing graphs and fixture selector flags disable narrow snapshot
selection so CTest's implicit setup/cleanup closure remains protected.
The VS Code controlled app task uses the corresponding protected app/runtime
layout as well; attach-only debugging therefore observes the same loader policy
as command-line tests.

To verify the self-contained release package uses only the controlled FFmpeg 8
runtime, run the Qt core-media smoke after packaging. It audits an isolated copy
of the package, starts raw audio through the Windows Qt backend, and submits two
application-fed frames to `VideoOutput`:

```bash
cmake --preset windows-mingw-release -DOLR_BUILD_TESTS=ON
cmake --build --preset windows-mingw-release --target qt_core_media_smoke
python tests/e2e/run_qt_core_media_smoke.py \
  --package windows_build/dist/OpenLiveReplay \
  --harness build/qt_core_media_smoke.exe \
  --platform windows \
  --controlled-prefix "ffmpeg=$PWD/windows_build/dist/ffmpeg" \
  --controlled-prefix "srt=$PWD/windows_build/dist/srt" \
  --evidence-dir windows_build/dist/runtime-evidence
```

On an intentional headless run, append `--allow-no-audio-device`; developer
workstations should require `audio=started`. The package scripts remove Qt's
FFmpeg media plugin and retain the native Windows backend plus the controlled
FFmpeg/SRT runtime. On failure, start with `runtime-error.txt`, then inspect
`harness-loaded-modules.json` for package-local `windowsmediaplugin.dll` and the
`harness-*` / `packaged-app-*` stdout and stderr logs in the selected evidence
directory. Audit ABI, path, and plugin details remain in the audit JSON.

Qt Creator / VS Code can use the `windows-mingw-release` preset directly once the
same `OLR_*` environment variables are set. The committed VS Code test task uses
the Windows CTest launcher. Start the app with the committed controlled Windows
process task, which derives compiler, Qt, FFmpeg, and SRT runtime directories from
the active build's `CMakeCache.txt`, then use the attach-only C++ or combined
C++/QML debug configuration.

## Notes

- **Smart App Control / WDAC.** When enforced, Windows blocks a freshly built,
  unsigned `.exe` from launching (`"Application Control policy has blocked this
  file"`). Turn Smart App Control off (Settings → Privacy & security → Windows
  Security → App & browser control → Smart App Control) to run local builds, or
  sign the binary.
- **Why a curated FFmpeg codec set.** A full FFmpeg build's `libavcodec` emits
  ~1000 objects, and FFmpeg's `compat/windows/makedef` overruns the Windows
  command-line length limit under Git Bash, breaking DLL generation. The curated
  set (see the deps script) stays under the limit and still covers the app's
  ingest/record/playback codecs. The local smoke-test FFmpeg uses Windows Media
  Foundation's H.264 encoder (`h264_mf`) instead of GPL `libx264`; native decode
  on Windows also uses Media Foundation, so FFmpeg needs no platform decode
  integration.
- **SRT without encryption.** SRT is built with `ENABLE_ENCRYPTION=OFF`; the
  native SRT ingest refuses encrypted URLs and falls back to FFmpeg's own
  `libsrt` for those, so no OpenSSL dependency is pulled in.
- CI builds and smoke-tests the Windows package alongside the macOS and Linux
  release packages.
