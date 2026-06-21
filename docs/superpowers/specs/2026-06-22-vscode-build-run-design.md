# VS Code build & run — portable, zero-ask design

**Status:** approved (design)
**Date:** 2026-06-22

## Goal

A fresh checkout on any machine can be built and run from VS Code's CMake Tools
**Build / Run / Debug** buttons (and `Ctrl+Shift+B`) without hand-holding — no
"ask Claude to build", no per-machine path edits in the common case. Auto-detect
where possible; provide one explicit per-machine escape hatch where not.

## Background / current state

- [`CMakePresets.json`](../../../CMakePresets.json) exists with **Release-only**
  presets: `macos-release`, `windows-mingw-release`. Both read Qt/FFmpeg/SRT
  locations from `OLR_*` environment variables. No Linux preset, no Debug preset,
  no test preset.
- [`.vscode/launch.json`](../../../.vscode/launch.json) exists and is wired for
  the **CMake Tools + official Qt** extensions
  (`${command:cmake.launchTargetPath}`, `${command:qt-cpp.natvis}`,
  `${command:qt-qml.debugPort}`) — but it is **gitignored**, so it does not travel
  to other machines.
- No `tasks.json`, `settings.json`, or `extensions.json`.
- Dependency discovery in `CMakeLists.txt`:
  - **macOS desktop:** uses `OLR_FFMPEG_ROOT`/`OLR_SRT_ROOT` when set, otherwise
    **falls back to Homebrew** — so local dev needs no FFmpeg/SRT env vars.
  - **Linux desktop:** FFmpeg via `pkg-config` (distro `libav*-dev`); no env vars.
  - **Windows:** requires FFmpeg + SRT **built from source** first
    (`OLR_FFMPEG_ROOT`/`OLR_SRT_ROOT` + MinGW + Ninja via `OLR_*` env).
  - **Qt:** found via `CMAKE_PREFIX_PATH`; there is no auto-detection today.

The single thing that genuinely differs per machine and cannot be globbed from a
preset is the **Qt install path** (`~/Qt/<version>/<kit>`).

## Key constraint that shapes the design

When CMake Tools runs in **presets mode** — which `launch.json` already commits us
to via `${command:cmake.launchTargetPath}` — it **ignores** kits and
`cmake.configureSettings`. Therefore the Qt extension's `CMAKE_PREFIX_PATH`
injection does **not** apply, and Qt discovery cannot live in VS Code settings.
It must live in CMake. This makes a small, guarded auto-detect block in
`CMakeLists.txt` the correct (and editor-agnostic) home for "find Qt anywhere".

## Decisions (from brainstorming)

- **Qt discovery:** auto-detect + per-machine `CMakeUserPresets.json` fallback.
- **Build config:** add Debug presets, keep the existing Release presets.
- **Scope:** all platforms (macOS / Linux / Windows) + the test suite.

## Design

### 1. Qt auto-detection in `CMakeLists.txt`

Insert a guarded block immediately **before** `find_package(Qt6 ...)` (currently
line 106). It only acts as a *fallback*:

- Honors, in priority order, anything already provided and **skips entirely** if
  so: `-DCMAKE_PREFIX_PATH`, `CMAKE_PREFIX_PATH` env, `OLR_QT_ROOT`,
  `QT_ROOT_DIR` (CI's `install-qt-action`).
- Only when none of those is set, probe standard locations and append the newest
  (natural version sort, `list(SORT ... COMPARE NATURAL ORDER DESCENDING)`):
  - macOS: `~/Qt/6.*/macos`, then `/opt/homebrew/opt/qt`, `/usr/local/opt/qt`.
  - Linux: `~/Qt/6.*/gcc_64` (else leave empty → system Qt via find_package).
  - Windows: `%USERPROFILE%/Qt/6.*/mingw_*`, `%USERPROFILE%/Qt/6.*/msvc*`.
- `message(STATUS ...)` the path it selected.

Guarantees:
- **CI and `build-scripts/*` are unaffected** — they always export
  `QT_ROOT_DIR` / `OLR_QT_ROOT`, so the fallback never fires.
- A bare `cmake -S . -B build` on a dev box now also "just works".
- Because the existing `macos-release` preset sets `CMAKE_PREFIX_PATH` to
  `$env{OLR_QT_ROOT}` (an empty string when unset), the `if(NOT CMAKE_PREFIX_PATH)`
  guard still fires for it — so even the Release preset auto-detects Qt when no
  env is exported, with no change to that preset.

### 2. `CMakePresets.json` — extend, do not disturb Release

- **Keep `macos-release` and `windows-mingw-release` verbatim.** They stay pinned
  to `binaryDir = ${sourceDir}/build`, which `build_macos_app.sh` and CI depend on.
- **Add Debug presets** `macos-debug`, `linux-debug`, `windows-mingw-debug`:
  - `CMAKE_BUILD_TYPE=Debug`, `OLR_BUILD_TESTS=ON`.
  - `binaryDir = ${sourceDir}/build/debug` — a **separate** tree so Debug and
    Release never clobber each other or force a full reconfigure on switch.
  - Generator Ninja; `condition` on `${hostSystemName}` so only the host's presets
    appear in the picker.
  - **No hardcoded Qt path** (Section 1 + `CMakeUserPresets.json` handle it).
  - macOS Debug sets no FFmpeg/SRT env (Homebrew fallback). Windows Debug still
    reads `OLR_*` for from-source FFmpeg/SRT + the MinGW compilers, mirroring the
    Release preset.
- **Add `linux-release`** for parity.
- **Add `testPresets`** (`macos-debug`, `linux-debug`, `windows-mingw-debug`)
  referencing the Debug configure presets with `output.outputOnFailure = true`.
  This lights up CMake Tools' **Run Tests** / CTest integration.
- Use `inherits` from a hidden base preset where it reduces duplication.

### 3. Committed `.vscode/` files

- **`extensions.json`** — recommend `ms-vscode.cmake-tools`, `ms-vscode.cpptools`,
  `theqtcompany.qt-cpp`, `theqtcompany.qt-qml` (the extensions `launch.json`
  already depends on).
- **`settings.json`** —
  - `"cmake.useCMakePresets": "always"` (force presets mode; matches `launch.json`).
  - `"C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"` (IntelliSense
    from the active preset).
  - `"cmake.copyCompileCommands": "${workspaceFolder}/compile_commands.json"` so
    clangd/IntelliSense find the DB regardless of which preset's `binaryDir` is
    active (`compile_commands.json` is already gitignored).
- **`tasks.json`** — a default `cmake`-type **build** task (so `Ctrl+Shift+B` is
  deterministic) plus a **Run Tests** (ctest) task.
- **`launch.json`** — **un-gitignore and commit** the existing file unchanged
  (it is generic — pure `${command:...}`, no hardcoded paths), so Run/Debug
  travels to any machine.

### 4. `.gitignore`

- Remove the blanket `launch.json` ignore so `.vscode/launch.json` is tracked.
- Add `CMakeUserPresets.json` (per-machine escape hatch; never committed).
- `compile_commands.json` is already ignored — no change.

### 5. Documentation

- A short **"Building in VS Code"** section (in `README.md`, cross-referenced from
  `docs/` as appropriate): install the recommended extensions → pick the
  `*-debug` preset → Build/Run/Debug. Document the `CMakeUserPresets.json`
  override for a non-standard Qt location, and the Windows from-source-deps
  prerequisite (`build-scripts/build_ffmpeg_windows_srt.sh` + `OLR_*`).

## Per-machine cost

- **macOS / Linux, Qt in the standard location:** truly zero-config.
- **Non-standard Qt location:** one `CMakeUserPresets.json` (gitignored) with the
  Qt path, written once.
- **Windows:** run the from-source FFmpeg/SRT deps script once and export `OLR_*`
  (unavoidable — the app needs an LGPL, SRT-enabled FFmpeg built from source).

## Out of scope (YAGNI)

- iOS (Xcode-driven; not a VS Code build target).
- Reworking the existing Release presets or the build scripts.
- GammaRay / soak / e2e harness launch configurations.
- A bespoke non-CMake-Tools task pipeline (the repo is already committed to CMake
  Tools via `launch.json`).

## Verification

- macOS: fresh `build/debug` configure+build from the VS Code Build button with
  **no** `OLR_*`/`CMAKE_PREFIX_PATH` exported; app launches via Run/Debug; Run
  Tests executes the suite.
- Confirm CI is unaffected: `QT_ROOT_DIR` present → Qt auto-detect skipped;
  `build_macos_app.sh` still finds `build/OpenLiveReplay.app`.
- Confirm a stale/empty `CMAKE_PREFIX_PATH` still resolves Qt via the fallback.
</content>
