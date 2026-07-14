# Windows Test Launch Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Windows CTest and VS Code launches independent of the caller's runtime `PATH` and prevent test failures from opening interactive Windows error dialogs.

**Architecture:** A Python entrypoint derives the controlled runtime from the selected build's CMake cache and invokes CTest non-interactively. A CMake helper gives each compiled unit test the same runtime directories, while launch integrations route supported workflows through the entrypoint.

**Tech Stack:** Python 3 standard library, CMake/CTest 3.16-compatible test properties, PowerShell/Bash/VS Code task integration, Qt 6, MinGW, controlled FFmpeg and SRT distributions.

## Global Constraints

- Never run a raw Windows test `.exe`; RED and GREEN integration runs must use CTest with Windows error-mode containment.
- Runtime directories must come from the active build's `CMakeCache.txt`, not an ambient FFmpeg/SRT install.
- Preserve the CMake 3.16 minimum: use `ENVIRONMENT_MODIFICATION` only when CMake is at least 3.22 and provide an `ENVIRONMENT` fallback.
- Do not copy uncontrolled DLLs from `PATH` or deploy a second FFmpeg runtime.
- Do not change production application code or hot-path behavior.
- Use targeted `git add <paths>` only.

---

### Task 1: Build-derived CTest launcher

**Files:**
- Create: `tools/run_ctest.py`
- Create: `tests/scripts/test_run_ctest.py`

**Interfaces:**
- Consumes: ordinary CTest command-line arguments and `<build>/CMakeCache.txt`.
- Produces: `find_build_directory(args, cwd) -> pathlib.Path`, `read_cmake_cache(path) -> dict[str, str]`, `prepare_windows_runtime(build_dir, inherited_env) -> tuple[dict[str, str], pathlib.Path]`, and `main(argv) -> int`.

- [ ] **Step 1: Write failing launcher unit tests**

  Cover split and `=` forms of `--test-dir`, cache parsing, exact controlled path precedence, Qt plugin/offscreen defaults, explicit QPA preservation, missing-key failure, automatic `--interactive-debug-mode 0`, and exact child exit propagation through a fake CTest executable.

  ```python
  class RunCtestTests(unittest.TestCase):
      def test_windows_runtime_precedes_inherited_path(self):
          env, ctest = run_ctest.prepare_windows_runtime(
              self.build_dir, {"PATH": str(self.ambient_bin)})
          self.assertEqual(env["PATH"].split(os.pathsep)[:6], [
              str(self.compiler_bin), str(self.qt_root / "bin"),
              str(self.ffmpeg_root / "bin"), str(self.ffmpeg_root / "lib"),
              str(self.srt_root / "bin"), str(self.srt_root / "lib"),
          ])
          self.assertEqual(ctest, self.cmake_bin / "ctest.exe")

      def test_main_propagates_child_exit_code(self):
          self.fake_ctest_exit_code = 23
          self.assertEqual(run_ctest.main(["--test-dir", str(self.build_dir)]), 23)
  ```

- [ ] **Step 2: Verify RED**

  Run: `python -m unittest tests.scripts.test_run_ctest -v`

  Expected: FAIL because `tools/run_ctest.py` does not exist.

- [ ] **Step 3: Implement the minimal launcher**

  Parse CMake cache values without executing them. On Windows derive the compiler directory, Qt prefix from `Qt6_DIR`, and controlled dependency roots; prepend their runtime directories to a copied child environment. Call `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)`, invoke CTest without a shell, restore the prior error mode, and return the child result.

  ```python
  def main(argv: Sequence[str] | None = None) -> int:
      args = list(sys.argv[1:] if argv is None else argv)
      build_dir = find_build_directory(args, Path.cwd())
      env, ctest = prepare_windows_runtime(build_dir, os.environ)
      command = [str(ctest), "--interactive-debug-mode", "0", *args]
      old_mode = _set_windows_error_mode()
      try:
          return subprocess.run(command, env=env, check=False).returncode
      finally:
          _restore_windows_error_mode(old_mode)
  ```

- [ ] **Step 4: Verify GREEN**

  Run: `python -m unittest tests.scripts.test_run_ctest -v`

  Expected: all launcher tests PASS.

- [ ] **Step 5: Commit**

  ```bash
  git add tools/run_ctest.py tests/scripts/test_run_ctest.py
  git commit -m "test: add controlled Windows CTest launcher"
  ```

### Task 2: Make compiled unit tests loader-safe under bare CTest

**Files:**
- Create: `cmake/OlrTestRuntime.cmake`
- Modify: `tests/unit/CMakeLists.txt`
- Create: `tests/scripts/check_windows_ctest_runtime.py`
- Modify: `tests/smoke/CMakeLists.txt`

**Interfaces:**
- Consumes: `CMAKE_CXX_COMPILER`, `Qt6::Core`, `OLR_FFMPEG_ROOT`, and `OLR_SRT_ROOT` from the active configure.
- Produces: `olr_configure_windows_test_runtime(TESTS <names...>)`, plus the `windows_ctest_runtime_contract` smoke test.

- [ ] **Step 1: Write the failing generated-contract check**

  The script reads verbose CTest registration for a representative FFmpeg-linked unit and requires compiler, Qt, FFmpeg `bin`/`lib`, SRT `bin`/`lib`, plugin path, and offscreen QPA entries. Register it as a Windows `smoke;ci` test.

  ```python
  result = subprocess.run(
      [ctest, "--test-dir", build_dir, "-N", "-V", "-R", "^tst_muxer$"],
      text=True, capture_output=True, check=True)
  for required in expected_runtime_entries(cache):
      if required.casefold() not in result.stdout.casefold():
          raise SystemExit(f"missing Windows test runtime entry: {required}")
  ```

- [ ] **Step 2: Verify RED**

  Configure the Windows build and run:

  `python tools/run_ctest.py --test-dir build/windows-launch -R '^windows_ctest_runtime_contract$' --output-on-failure`

  Expected: FAIL because unit registration contains only `QT_QPA_PLATFORM=offscreen`.

- [ ] **Step 3: Implement the CMake helper**

  For CMake 3.22+, append `PATH=path_list_prepend:<dir>` entries through `ENVIRONMENT_MODIFICATION`. For CMake 3.16–3.21, prepend the same directories to an escaped inherited `PATH` through `ENVIRONMENT`. Set the configured Qt plugin path and offscreen QPA for all compiled unit tests. Apply the helper inside `olr_add_unit_test()` and to `tst_ndi_runtime_smoke`.

  ```cmake
  function(olr_configure_windows_test_runtime)
      cmake_parse_arguments(ARG "" "" "TESTS" ${ARGN})
      if(NOT WIN32)
          return()
      endif()
      foreach(test_name IN LISTS ARG_TESTS)
          set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT
              "QT_QPA_PLATFORM=offscreen"
              "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
          if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
              set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT_MODIFICATION
                  "PATH=path_list_prepend:${OLR_FFMPEG_ROOT}/bin"
                  "PATH=path_list_prepend:${OLR_SRT_ROOT}/bin")
          endif()
      endforeach()
  endfunction()
  ```

- [ ] **Step 4: Verify GREEN and the real loader boundary**

  Build an FFmpeg-linked unit, scrub Qt/FFmpeg/SRT/MinGW entries from the parent `PATH`, then run it through `tools/run_ctest.py`. Require PASS and confirm `ctest -N -V` contains every configured runtime root.

- [ ] **Step 5: Commit**

  ```bash
  git add cmake/OlrTestRuntime.cmake tests/unit/CMakeLists.txt tests/scripts/check_windows_ctest_runtime.py tests/smoke/CMakeLists.txt
  git commit -m "test: register controlled Windows runtime paths"
  ```

### Task 3: Route supported launch paths through the contract

**Files:**
- Modify: `.vscode/tasks.json`
- Modify: `.vscode/launch.json`
- Modify: `.githooks/pre-push`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/build-and-run.md`
- Modify: `docs/windows-build.md`
- Modify: `docs/superpowers/plans/2026-07-14-transport-epoch-completion.md`

**Interfaces:**
- Consumes: `python tools/run_ctest.py <ordinary ctest args>`.
- Produces: one supported cross-shell CTest invocation with exact exit propagation.

- [ ] **Step 1: Add a failing source-policy check**

  Extend `tests/scripts/check_windows_ctest_runtime.py` to reject raw `tst_*.exe` commands in current plans, plain Windows VS Code CTest launches, raw Windows CI test/probe launches, and direct pre-push CTest calls that bypass the helper function.

  ```python
  checks = {
      "raw test executable": (root / "docs/superpowers/plans", r"tst_[A-Za-z0-9_]+\.exe"),
      "pre-push bypass": (root / ".githooks/pre-push", r"(?m)^\s*ctest\s"),
  }
  failures = scan_policy_violations(checks)
  if failures:
      raise SystemExit("\n".join(failures))
  ```

- [ ] **Step 2: Verify RED**

  Run: `python tests/scripts/check_windows_ctest_runtime.py --source-root .`

  Expected: FAIL on the transport plan's raw `tst_playbackworker.exe`, plain VS Code CTest task, direct pre-push calls, and raw CI probes/tests.

- [ ] **Step 3: Integrate the launcher**

  Add a platform-aware pre-push function that calls the Python launcher on Windows and ordinary CTest elsewhere. Give the VS Code task a Windows override. Replace Windows CI raw test launches with CTest selection through the launcher and make the GPU diagnostic a registered CTest probe or give it a build-derived controlled environment. Add repository-controlled FFmpeg/SRT `bin` paths to Windows app debugging. Replace the raw transport test command and document the supported Windows syntax.

  ```bash
  olr_ctest() {
      if [[ "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ]]; then
          "$PYTHON_BIN" tools/run_ctest.py "$@"
      else
          command ctest "$@"
      fi
  }
  ```

- [ ] **Step 4: Verify GREEN and full gates**

  Run the source-policy check, Python unit tests, focused contract smoke, a clean-shell FFmpeg-linked unit, full Windows `-L unit`, `python tools/roadmap/audit.py`, `python -m unittest discover -s tools/roadmap/tests -p 'test_*.py'`, JSON parsing for VS Code files, workflow YAML parsing, shell syntax check for pre-push, and `git diff --check`.

- [ ] **Step 5: Independent review and fix loop**

  Give a fresh reviewer the design, plan, full diff, and verification evidence. Fix every Critical or Important correctness/portability finding and repeat review until none remain.

- [ ] **Step 6: Commit**

  ```bash
  git add .vscode/tasks.json .vscode/launch.json .githooks/pre-push .github/workflows/ci.yml docs/build-and-run.md docs/windows-build.md docs/superpowers/plans/2026-07-14-transport-epoch-completion.md tests/scripts/check_windows_ctest_runtime.py
  git commit -m "build: standardize noninteractive Windows test launches"
  ```
