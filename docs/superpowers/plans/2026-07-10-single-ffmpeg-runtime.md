# Stock Qt Single FFmpeg Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship OpenLiveReplay on Windows, macOS, Linux, and iOS with stock Qt and exactly one FFmpeg implementation: the project's controlled FFmpeg 8.

**Architecture:** Keep each stock Qt installation immutable. Select Qt's native media backend where available, prevent media-player APIs from entering production, exclude Qt's FFmpeg media plugin from dynamic packages and iOS static imports, then fail closed through package, link-map, and loaded-module audits.

**Tech Stack:** C++20, Qt 6 Multimedia/QML, CMake, Python 3, Bash, PE/Mach-O/ELF inspection tools, Xcode link maps, CTest, GitHub Actions.

## Global Constraints

- Do not rebuild Qt or Qt Multimedia and do not create a custom Qt overlay.
- OpenLiveReplay's controlled FFmpeg 8 is the only FFmpeg allowed in packages and running processes.
- Keep stock aqt/Qt installations immutable; filtering occurs only in assembled packages.
- Windows selects `QT_MEDIA_BACKEND=windows`; macOS and iOS select `QT_MEDIA_BACKEND=darwin`; Linux clears inherited selection.
- Release packages use controlled FFmpeg 8 and SRT; Linux release builds must not use distro FFmpeg.
- Dynamic packages contain no Qt FFmpeg media plugin; iOS imports no static Qt FFmpeg plugin or Qt FFmpeg archives.
- Package-local `qt.conf` and isolated launch environments prevent global Qt plugin discovery.
- Build, package, and provenance checks fail closed with actionable dependency diagnostics.
- Do not create or stage commits until the user explicitly grants permission.

---

### Task 1: Remove The Abandoned Qt Rebuild Path

**Files:**
- Delete: `build-scripts/qt-runtime-lock.json`
- Delete: `build-scripts/olr_qt_runtime.py`
- Delete: `build-scripts/build_qt_multimedia_no_ffmpeg.py`
- Delete: `tests/smoke/test_olr_qt_runtime.py`
- Delete: `tests/smoke/test_build_qt_multimedia_no_ffmpeg.py`
- Modify: `tests/smoke/CMakeLists.txt`
- Modify: `docs/build-and-run.md`
- Remove generated: `.qt-runtime-downloads/`, `.qt-runtime-work/`, `windows_build/qt-no-ffmpeg/`

**Interfaces:**
- Produces: a source tree and development guide with no custom Qt bootstrap, overlay, manifest, or Vulkan-header prerequisite.

- [ ] **Step 1: Record the current uncommitted paths and generated roots**

Run `git status --short`, `git diff -- tests/smoke/CMakeLists.txt docs/build-and-run.md`, and resolve each generated directory with PowerShell `Resolve-Path`. Require every resolved path to be inside this worktree before deletion.

- [ ] **Step 2: Remove only the abandoned uncommitted implementation**

Delete the five untracked source/test files. Restore only the custom-runtime additions in `tests/smoke/CMakeLists.txt` and `docs/build-and-run.md`; preserve all pre-existing repository content and the revised design/plan.

- [ ] **Step 3: Remove verified generated artifacts**

Use native PowerShell `Remove-Item -LiteralPath <verified-path> -Recurse -Force` for `.qt-runtime-downloads`, `.qt-runtime-work`, and `windows_build/qt-no-ffmpeg`. Do not delete any controlled FFmpeg/SRT output.

- [ ] **Step 4: Verify the old architecture is gone**

Run:

```powershell
rg -n "qt-no-ffmpeg|build_qt_multimedia_no_ffmpeg|olr_qt_runtime|qt-runtime-lock|Vulkan-Headers" build-scripts tests docs CMakeLists.txt CMakePresets.json
git diff --check
git diff --cached --name-only
```

Expected: no production or development references remain, whitespace checks pass, and the index is empty.

---

### Task 2: Deterministic Qt Media Backend Policy

**Files:**
- Modify: `appenv.h`
- Modify: `appenv.cpp`
- Modify: `main.cpp`
- Modify: `tests/unit/tst_appenv.cpp`

**Interfaces:**
- Produces: `void appenv::configureQtMediaBackend()` called before `QGuiApplication` construction.

- [ ] **Step 1: Add the failing platform-policy test**

Add a test that starts with `QT_MEDIA_BACKEND=ffmpeg`, calls `configureQtMediaBackend()`, and expects `windows` on `Q_OS_WIN`, `darwin` on `Q_OS_MACOS`/`Q_OS_IOS`, and an unset variable elsewhere. Clear the variable in `cleanup()`.

- [ ] **Step 2: Build the focused test and confirm RED**

Run `cmake --build build/debug --target tst_appenv`.

Expected: compilation fails because `configureQtMediaBackend` is not declared.

- [ ] **Step 3: Implement and invoke the policy**

Implement exactly:

```cpp
void configureQtMediaBackend() {
#if defined(Q_OS_WIN)
    qputenv("QT_MEDIA_BACKEND", "windows");
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    qputenv("QT_MEDIA_BACKEND", "darwin");
#else
    qunsetenv("QT_MEDIA_BACKEND");
#endif
}
```

Call it in `main()` immediately before `QGuiApplication app(argc, argv);`.

- [ ] **Step 4: Verify GREEN**

Run `ctest --test-dir build/debug -R '^tst_appenv$' --output-on-failure` and `git diff --check`.

Expected: `tst_appenv` passes and no whitespace errors are reported.

---

### Task 3: Qt Media API Boundary Gate

**Files:**
- Create: `tests/smoke/check_qt_media_api_boundary.py`
- Create: `tests/smoke/test_qt_media_api_boundary.py`
- Modify: `tests/smoke/CMakeLists.txt`

**Interfaces:**
- Produces: scanner CLI `check_qt_media_api_boundary.py --root <path>`; exit zero means production uses only raw audio and application-fed video surfaces.

- [ ] **Step 1: Write RED scanner tests**

Use temporary `.cpp` and `.qml` fixtures. Accept `QAudioSink`, `QMediaDevices`, `QVideoFrame`, `QVideoSink`, and `VideoOutput`; reject the word-boundary tokens `QMediaPlayer`, `MediaPlayer`, `QMediaRecorder`, `MediaRecorder`, `QAudioDecoder`, `QCamera`, `Camera`, `QMediaCaptureSession`, and `CaptureSession`. Prove comments do not trigger findings and diagnostics include relative path, line, and token.

- [ ] **Step 2: Run RED**

Run `python tests/smoke/test_qt_media_api_boundary.py`.

Expected: import or file-not-found failure because the scanner does not exist.

- [ ] **Step 3: Implement token-aware source scanning**

Scan production `*.cpp`, `*.h`, `*.mm`, and `*.qml`. Exclude `.git`, `.claude`, build/output directories, `docs`, and `tests`. Strip C/C++ line/block comments and QML line/block comments while preserving line counts, then report prohibited tokens with word-boundary matching.

- [ ] **Step 4: Register and verify both gates**

Register the Python unit test and repository scan as CTest tests labeled `smoke;ci`. Run:

```powershell
python tests/smoke/test_qt_media_api_boundary.py
python tests/smoke/check_qt_media_api_boundary.py --root .
```

Expected: tests pass and the current production tree has no prohibited API.

---

### Task 4: Cross-Platform Package And Link-Map Audit

**Files:**
- Create: `build-scripts/single_ffmpeg_policy.json`
- Create: `build-scripts/audit_single_ffmpeg.py`
- Create: `tests/smoke/test_audit_single_ffmpeg.py`
- Modify: `tests/smoke/CMakeLists.txt`

**Interfaces:**
- Consumes: `--package`, `--platform windows|macos|linux|ios`, optional `--link-map`, and policy JSON.
- Produces: human diagnostics plus JSON evidence/SBOM; exit zero only for one controlled FFmpeg 8 ABI and no Qt FFmpeg plugin.

- [ ] **Step 1: Define the locked policy and RED fixtures**

Lock FFmpeg `8.1.1`, the existing platform-specific controlled SRT versions and hashes, allowed ABI majors (`avcodec=62`, `avformat=62`, `avutil=60`, `swresample=6`, `swscale=9`), and forbidden plugin names/metadata (`ffmpegmediaplugin`, `QFFmpegMediaPlugin`). Desktop policy records SRT `1.5.4`; iOS records the existing pinned XCFramework build. Test valid and mixed PE, Mach-O, ELF dependency samples; unresolved/out-of-package FFmpeg; symlink escape; plugin rejection; and iOS link-map origins outside approved XCFramework roots.

- [ ] **Step 2: Run RED**

Run `python tests/smoke/test_audit_single_ffmpeg.py`.

Expected: import failure because `audit_single_ffmpeg.py` is absent.

- [ ] **Step 3: Implement pure parsers and command adapters**

Use dataclasses for binary records and dependencies. Parse `objdump -p`/`dumpbin`, `otool -L`, `readelf -d`, and Xcode link maps; keep subprocess execution separate from pure parsing. Recursively inspect binaries, frameworks, plugins, QML modules, symlinks, and nested bundles. Reject dependency paths outside the package and unexpected ABI majors.

- [ ] **Step 4: Emit provenance evidence**

Write deterministic audit JSON with every FFmpeg/SRT binary, parent, resolved path, hash, ABI, and source prefix. Emit an SPDX 2.3 fragment for locked FFmpeg/SRT and their verified package hashes.

- [ ] **Step 5: Register and verify parser/policy tests**

Run `python tests/smoke/test_audit_single_ffmpeg.py` and the registered CTest smoke test.

Expected: all fixtures pass; deliberate mixed-ABI/plugin/link-map cases fail with the exact parent and path.

---

### Task 5: Dynamic Desktop Package Isolation

**Files:**
- Create: `build-scripts/filter_qt_ffmpeg_plugin.py`
- Create: `build-scripts/build_linux_app.sh`
- Create: `qt.conf`
- Create: `tests/smoke/test_desktop_packaging_policy.py`
- Modify: `build-scripts/build_windows_app.sh`
- Modify: `build-scripts/build_macos_app.sh`
- Modify: `CMakePresets.json`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Consumes: a package assembled by stock `windeployqt`, `macdeployqt`, or Linux deployment logic.
- Produces: isolated Windows, macOS, and Linux packages with Qt's FFmpeg backend and its solely-owned FFmpeg dependency graph removed, followed by Task 4 audit.

- [ ] **Step 1: Write RED packaging-policy tests**

Assert all desktop packagers install package-local `qt.conf`, invoke the filter after stock Qt deployment, invoke the audit before archiving, and never modify `$OLR_QT_ROOT`. Verify the filter rejects ambiguous shared ownership instead of deleting an uncertain library.

- [ ] **Step 2: Run RED**

Run `python tests/smoke/test_desktop_packaging_policy.py`.

Expected: failures identify missing filter/audit calls and the absent Linux packager.

- [ ] **Step 3: Implement deterministic package-local filtering**

Locate the Qt FFmpeg plugin by platform filename and Qt plugin metadata, resolve its dependency graph using Task 4 adapters, remove the plugin, and remove only FFmpeg libraries whose remaining package reference count is zero. Fail when metadata or ownership is ambiguous. Never write under the Qt installation.

- [ ] **Step 4: Integrate Windows and macOS**

After `windeployqt`/`macdeployqt`, install `qt.conf`, filter the package, copy/preserve controlled FFmpeg 8 and SRT, audit, then archive. Keep native Windows/Darwin media plugins and platform/audio/rendering/image plugins.

- [ ] **Step 5: Add the Linux release packager**

Assemble an AppDir from stock Qt plus controlled dependencies, deploy no Qt media-player plugin, install `qt.conf`, set `$ORIGIN`-relative runtime paths, audit the AppDir, then create `linux_build/dist/OpenLiveReplay-linux.tar.gz`.

- [ ] **Step 6: Verify on the available Windows host**

Run the static tests, then `bash build-scripts/build_windows_app.sh`. Audit the resulting package and launch it with package-local plugin variables.

Expected: no `ffmpegmediaplugin.dll`, only ABI-62/60/6/9 FFmpeg DLLs, QAudioSink initializes through the Windows path, and the app remains responsive.

---

### Task 6: iOS Static Plugin Exclusion

**Files:**
- Modify: `CMakeLists.txt`
- Create: `tests/smoke/check_ios_qt_media_plugins.py`
- Modify: `tests/smoke/CMakeLists.txt`
- Modify: `.githooks/pre-push`

**Interfaces:**
- Produces: iOS CMake plugin selection that includes the Darwin media plugin, excludes the FFmpeg media plugin, never calls `qt_add_ios_ffmpeg_libraries`, and audits the final Xcode link map.

- [ ] **Step 1: Inspect the installed iOS Qt CMake exports**

Find the exact exported target names for the Darwin and FFmpeg media plugins. Require equivalent targets to `Qt6::QDarwinMediaPlugin` and `Qt6::QFFmpegMediaPlugin`; record exact names in one CMake variable block. Fail configure with the installed Qt path/version if deterministic exclusion is unavailable.

- [ ] **Step 2: Write RED static selection tests**

Require `qt_import_plugins(OpenLiveReplay INCLUDE <Darwin-target> EXCLUDE <FFmpeg-target>)`, prohibit `qt_add_ios_ffmpeg_libraries`, require Xcode link-map generation, and require invocation of Task 4's iOS audit.

- [ ] **Step 3: Run RED**

Run `python tests/smoke/check_ios_qt_media_plugins.py`.

Expected: failure because explicit static plugin selection is absent.

- [ ] **Step 4: Implement fail-closed static imports and link-map audit**

Add exact plugin selection under `Q_OS_IOS`/iOS CMake conditions, retain OpenLiveReplay's FFmpeg 8/SRT XCFrameworks, enable `LD_GENERATE_MAP_FILE=YES`, and audit the generated map after the Xcode build. Accept `libav*` origins only beneath approved project XCFramework roots.

- [ ] **Step 5: Verify simulator and device configurations**

Configure/build both targets where available and run the static/link-map checks.

Expected: Darwin plugin imported, no Qt FFmpeg plugin/archive objects, and all FFmpeg symbols originate from OpenLiveReplay's FFmpeg 8 XCFrameworks.

---

### Task 7: Controlled Linux FFmpeg 8 And SRT

**Files:**
- Create: `build-scripts/build_ffmpeg_linux_srt.sh`
- Create: `tests/smoke/check_linux_ffmpeg_build_config.sh`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `tests/smoke/CMakeLists.txt`
- Modify: `docs/build-and-run.md`

**Interfaces:**
- Produces: `linux_build/dist/ffmpeg`, `linux_build/dist/srt`, and release-mode `OLR_FFMPEG_ROOT`/`OLR_SRT_ROOT` with no default-path fallback.

- [ ] **Step 1: Write the RED static configuration gate**

Require FFmpeg `8.1.1` and SRT `1.5.4` with the same pinned hashes/options as other controlled builders, shared libraries, libsrt enabled, LGPL-only flags, and `$ORIGIN` runtime paths. Reject GPL/nonfree options and x264/x265.

- [ ] **Step 2: Run RED**

Run `bash tests/smoke/check_linux_ffmpeg_build_config.sh`.

Expected: failure because the Linux builder is absent.

- [ ] **Step 3: Implement the pinned builder**

Download with TLS verification, verify hashes before extraction, build shared SRT and FFmpeg into project-local prefixes, and write a stamp containing source versions, hashes, configure flags, compiler identity, and architecture.

- [ ] **Step 4: Gate release dependency discovery**

Add `OLR_CONTROLLED_DEPS_REQUIRED`. When enabled, use `find_path`/`find_library` with `NO_DEFAULT_PATH` under `OLR_FFMPEG_ROOT` and `OLR_SRT_ROOT`; keep pkg-config only for explicit developer configurations. Enable the gate in `linux-release`.

- [ ] **Step 5: Verify static and real dependency behavior**

Run the static test, real builder, `readelf -d` on all FFmpeg libraries, and a controlled release configure.

Expected: `libavformat.so.62` resolves package-local SRT, no system FFmpeg is selected, and Task 4 accepts the prefix/package.

---

### Task 8: Runtime Media Smoke, CI, And Documentation

**Files:**
- Create: `tests/e2e/qt_core_media_smoke.cpp`
- Create: `tests/e2e/run_qt_core_media_smoke.py`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `.githooks/pre-push`
- Modify: `docs/build-and-run.md`
- Modify: `docs/windows-build.md`

**Interfaces:**
- Consumes: an isolated desktop package or iOS build and Task 4 audit.
- Produces: runtime evidence for raw audio, two application-fed video frames, backend selection, and loaded FFmpeg modules.

- [ ] **Step 1: Write the C++ media-surface smoke**

Create a `QGuiApplication`, start 48 kHz stereo PCM through `QAudioSink` from a finite buffer, instantiate QML `VideoOutput`, obtain its `QVideoSink`, submit two distinct `QVideoFrame`s, and emit JSON containing audio state, observed frame count, selected backend, and process ID. Permit `no-device` only through an explicit headless-test flag.

- [ ] **Step 2: Write the isolated package driver**

Set package-local Qt plugin/QML paths, clear global Qt paths, bound startup time, collect logs, inspect loaded modules on Windows/macOS/Linux, call Task 4's audit, and reject any FFmpeg ABI outside 62/60/6/9 or Qt FFmpeg initialization/fallback logging.

- [ ] **Step 3: Register and run focused smoke tests**

Build the harness and run it against the available Windows package. Require `audio=started` on a physical workstation and `videoFramesObserved>=2`.

- [ ] **Step 4: Add CI and pre-push enforcement**

Run source/API/parser/static-package gates on every host; build and audit native packages in Windows/macOS/Linux release jobs; retain iOS static/link-map checks in the full pre-push path where hosted device builds are unavailable. Cache controlled dependency prefixes by script/source hash.

- [ ] **Step 5: Document stock Qt development and release setup**

Document aqt/Qt installation, native backend behavior, controlled FFmpeg/SRT builders, package-local filtering, audit commands, and failure diagnostics. Explicitly state that Vulkan headers and Qt source builds are not prerequisites.

- [ ] **Step 6: Run final verification without committing**

Run:

```powershell
ctest --test-dir build/debug -L unit --output-on-failure
ctest --test-dir build/debug -L smoke --output-on-failure
python tests/smoke/check_qt_media_api_boundary.py --root .
python tests/smoke/test_audit_single_ffmpeg.py
git diff --check
git diff --cached --name-only
git rev-parse HEAD
```

Then build, audit, and runtime-smoke the package on the current host. Run existing playback/SRT E2E coverage affected by dependency or packaging changes. Expected: all available checks pass, the index is empty, and `HEAD` remains `e658b610`.

---

## Final Review Checklist

- [ ] Stock Qt is used unchanged and no Qt component is rebuilt.
- [ ] Dynamic packages contain no Qt FFmpeg plugin or private Qt FFmpeg libraries.
- [ ] iOS imports Darwin media support and no Qt FFmpeg plugin/archive.
- [ ] Every release package contains only controlled FFmpeg 8 and SRT.
- [ ] Linux release mode cannot resolve distro FFmpeg.
- [ ] Package-local Qt lookup cannot discover a developer/global FFmpeg plugin.
- [ ] Raw audio and application-fed video previews pass runtime smoke tests.
- [ ] Package, link-map, loaded-module, and source-policy gates fail closed.
- [ ] Existing recording, playback, seeking, and SRT behavior remains green.
- [ ] No commit or staging operation occurred without user permission.
