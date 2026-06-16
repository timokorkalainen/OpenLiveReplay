# Building OpenLiveReplay on Windows

The Windows app builds with the **Qt MinGW kit** plus FFmpeg and SRT compiled
from source. Unlike macOS (Homebrew) and iOS (Xcode frameworks), Windows has no
system package manager for these, so the build scripts fetch and build them from
pinned, integrity-verified sources — analogous to `build_ffmpeg_ios_srt.sh`.

## Prerequisites

Install **Qt 6.x for Windows with the MinGW kit** via the
[Qt online installer](https://www.qt.io/download-qt-installer). Select:

- `Qt 6.10.x > MinGW 13.1.0 64-bit`
- `Developer and Designer Tools > MinGW 13.1.0 64-bit`, `CMake`, `Ninja`

That kit bundles the matching `gcc`, `cmake`, and `ninja`; the scripts
auto-detect them under `C:/Qt`. No MSVC, vcpkg, or MSYS2 is required. You only
need **Git for Windows** (for Git Bash, used to run the scripts and build FFmpeg).

## One command

From **Git Bash** at the repo root:

```bash
./build-scripts/build_windows_app.sh
```

This will, deterministically and idempotently:

1. Build **SRT** (`v1.5.4`) and **FFmpeg** (`8.1.1`, with `libsrt`) from source
   into `windows_build/dist/` — see `build-scripts/build_ffmpeg_windows_srt.sh`.
2. Configure with the `windows-mingw-release` CMake preset.
3. Compile and link `build/OpenLiveReplay.exe`.
4. Run `windeployqt` and copy the FFmpeg/SRT/rtmidi DLLs into `build/`, leaving a
   runnable directory.

Re-running is cheap: the dependency build skips when its artifacts already exist
(delete `windows_build/` to force a clean rebuild).

### Toolchain overrides

If your Qt lives elsewhere, set any of these before running:

| Variable | Default |
| --- | --- |
| `OLR_QT_ROOT` | `C:/Qt/6.10.2/mingw_64` (else newest `C:/Qt/6.*/mingw_64`) |
| `OLR_MINGW_ROOT` | `C:/Qt/Tools/mingw1310_64` |
| `CMAKE_BIN` / `NINJA_BIN` | from `PATH`, else `C:/Qt/Tools/{CMake_64/bin,Ninja}` |

## Manual / IDE build

The dependency step and the preset are usable on their own:

```bash
./build-scripts/build_ffmpeg_windows_srt.sh          # build deps once
export OLR_QT_ROOT=C:/Qt/6.10.2/mingw_64
export OLR_MINGW_ROOT=C:/Qt/Tools/mingw1310_64
export OLR_NINJA=C:/Qt/Tools/Ninja/ninja.exe
export OLR_FFMPEG_ROOT="$PWD/windows_build/dist/ffmpeg"
export OLR_SRT_ROOT="$PWD/windows_build/dist/srt"
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
```

Qt Creator / VS Code can use the `windows-mingw-release` preset directly once the
same `OLR_*` environment variables are set.

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
  ingest/record/playback codecs. Native decode on Windows uses Media Foundation,
  so FFmpeg needs no platform codec integration.
- **SRT without encryption.** SRT is built with `ENABLE_ENCRYPTION=OFF`; the
  native SRT ingest refuses encrypted URLs and falls back to FFmpeg's own
  `libsrt` for those, so no OpenSSL dependency is pulled in.
- CI does not build Windows; the macOS leg remains the primary gate.
