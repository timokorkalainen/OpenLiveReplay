# Windows Test Launch Runtime Design

## Problem

Windows build-tree test executables are not deployed applications. CMake copies
Qt target DLLs beside the unit binaries, but FFmpeg and SRT are linked through
import libraries and their runtime DLL directories are not represented by
`TARGET_RUNTIME_DLLS`. MinGW runtime DLLs are likewise supplied by the toolchain
or Qt directories. Today each test receives only `QT_QPA_PLATFORM=offscreen`, so
launch success depends on the shell that happened to start CTest.

This has two user-visible failure modes:

- a test started from a clean shell can fail in the Windows loader before Qt
  Test starts; and
- CTest's default interactive Windows mode can display loader, assertion, or
  crash dialogs instead of returning a normal test failure.

One implementation plan also invokes a raw test `.exe`, bypassing CTest
entirely. VS Code, pre-push, CI, and local documentation currently use different
runtime environments.

## Considered approaches

### Selected: build-derived CTest launcher plus per-test runtime properties

A repository launcher reads the selected build's `CMakeCache.txt`, derives the
configured compiler, Qt, FFmpeg, and SRT runtime directories, and starts CTest
with a controlled child environment. On Windows it sets the process error mode
that children inherit and always passes `--interactive-debug-mode 0`.

Unit-test registration also prepends the same configured directories through
CTest test properties. This makes direct CTest loader-safe while the supported
launcher additionally prevents interactive crash UI. CMake 3.22 and newer use
`ENVIRONMENT_MODIFICATION`; the declared CMake 3.16 floor receives an escaped
`PATH` through the older `ENVIRONMENT` property.

### Rejected: deploy every dependency beside every test executable

This would make raw binaries start, but it duplicates large controlled runtime
trees across build directories, adds rebuild work, and still does not suppress
dialogs from genuine abnormal exits.

### Rejected: document a shell-specific `PATH`

This is the current failure mode. It is easy to omit, differs between Bash,
PowerShell, VS Code, and CI, and cannot guarantee which configured dependency
tree is used.

## Components

### `tools/run_ctest.py`

The launcher accepts ordinary CTest arguments. It finds the build directory
from `--test-dir` (or the current directory), parses `CMakeCache.txt`, locates
CTest beside the configured CMake executable when possible, and prepends:

1. the configured C++ compiler directory;
2. the configured Qt `bin` directory;
3. `${OLR_FFMPEG_ROOT}/bin` and `/lib`; and
4. `${OLR_SRT_ROOT}/bin` and `/lib`.

It sets `QT_PLUGIN_PATH` from the configured Qt prefix and defaults
`QT_QPA_PLATFORM` to `offscreen` without overriding an explicit caller choice.
Missing cache keys or directories fail before any child test starts, with an
actionable console message. The child exit code is returned unchanged.

### CMake test runtime helper

`cmake/OlrTestRuntime.cmake` owns the Windows test-environment contract.
`olr_configure_windows_test_runtime(TESTS ...)` is applied by the unit-test
registration helper and to manually registered unit executables. Non-Windows
configurations are unchanged.

### Launch integration

- VS Code's test task uses the repository launcher on Windows.
- The pre-push hook routes every CTest invocation through a small platform-aware
  shell function.
- Windows CI uses the launcher for CTest and stops raw-launching test binaries.
- Windows app debugging includes the repository's controlled FFmpeg and SRT
  `bin` directories in addition to Qt.
- Active plans and build documentation use CTest rather than raw test `.exe`
  commands and show the supported launcher on Windows.

## Verification

1. Unit-test the cache parser, build-directory discovery, environment assembly,
   CTest command construction, and exact child exit propagation.
2. Configure and build an FFmpeg-linked unit test on Windows.
3. Remove Qt, FFmpeg, SRT, and MinGW directories from the parent `PATH`; verify
   the supported launcher starts and passes the test.
4. Inspect `ctest -N -V` to prove a representative unit test contains all
   configured runtime roots even without the launcher.
5. Verify no `WerFault`, test, app, or CTest processes remain after the run and
   no new Windows Error Reporting record was created.
6. Run the full Windows unit label through the supported launcher.
7. Run repository formatting, roadmap audit, and the relevant script tests.

## Performance and scope

The launcher affects development and CI test process startup only. Environment
assembly is performed once per CTest invocation and adds no production code,
application hot-path work, or packaged runtime dependency. The app debug PATH
change affects only VS Code-launched development builds.
