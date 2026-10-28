#!/usr/bin/env python3
"""Unit tests for fail-closed Windows launch-source policy helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("check_windows_ctest_runtime.py")


def load_module():
    spec = importlib.util.spec_from_file_location("windows_runtime_policy", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Windows runtime policy")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WindowsRuntimePolicyTests(unittest.TestCase):
    def test_configured_runtime_uses_explicit_qml_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "compiler" / "bin" / "g++.exe"
            qt_runtime = root / "runtime-layout" / "qt" / "bin"
            plugins = root / "plugin-layout"
            qml = root / "separate-qml-layout"
            ffmpeg = root / "runtime-layout" / "ffmpeg"
            srt = root / "runtime-layout" / "srt"
            compiler.parent.mkdir(parents=True)
            compiler.write_bytes(b"compiler")
            for directory in (
                qt_runtime,
                plugins,
                qml,
                ffmpeg / "bin",
                ffmpeg / "lib",
                srt / "bin",
                srt / "lib",
            ):
                directory.mkdir(parents=True)

            runtime_dirs, selected_plugins, selected_qml = (
                self.policy.configured_runtime(
                    {
                        "CMAKE_CXX_COMPILER": str(compiler),
                        "OLR_FFMPEG_ROOT": str(ffmpeg),
                        "OLR_SRT_ROOT": str(srt),
                    },
                    qt_runtime,
                    plugins,
                    qml,
                )
            )

            self.assertEqual(runtime_dirs[1], qt_runtime.resolve())
            self.assertEqual(selected_plugins, plugins.resolve())
            self.assertEqual(selected_qml, qml.resolve())
            self.assertFalse((qt_runtime.parent / "qml").exists())

    def test_qmllint_rebinds_to_current_qt_prefix_on_every_configure(self) -> None:
        smoke_cmake = (
            MODULE_PATH.parents[1] / "smoke" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertRegex(
            smoke_cmake,
            r"unset\(OLR_QMLLINT CACHE\)\s*"
            r"find_program\(OLR_QMLLINT NAMES qmllint "
            r"HINTS \"\$\{_qt_prefix\}/bin\" NO_DEFAULT_PATH\)",
        )

    def test_rtmidi_no_tests_subbuild_emits_closed_empty_ctest_graph(self) -> None:
        root_cmake = (MODULE_PATH.parents[2] / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            root_cmake,
            r"set\(RTMIDI_BUILD_TESTING OFF CACHE BOOL \"\" FORCE\)"
            r"[\s\S]*FetchContent_MakeAvailable\(rtmidi\)"
            r"[\s\S]*file\(WRITE "
            r"\"\$\{rtmidi_BINARY_DIR\}/CTestTestfile\.cmake\"",
        )
        self.assertIn(
            "Intentionally empty: RTMIDI_BUILD_TESTING=OFF.", root_cmake
        )
        for relative in ("ui/theme", "ui/style"):
            self.assertIn(
                f'file(WRITE "${{CMAKE_BINARY_DIR}}/{relative}/CTestTestfile.cmake"',
                root_cmake,
            )
        self.assertIn("Intentionally empty: QML library has no tests.", root_cmake)

    def test_windows_runtime_declares_cmake_322_minimum(self) -> None:
        helper = MODULE_PATH.parents[2] / "cmake" / "OlrTestRuntime.cmake"
        text = helper.read_text(encoding="utf-8")
        self.assertRegex(text, r"CMAKE_VERSION\s+VERSION_LESS\s+3\.22")
        self.assertRegex(text, r"FATAL_ERROR[\s\S]*3\.22")

    def test_windows_runtime_prepends_qt_qml_without_replacing_test_imports(
        self,
    ) -> None:
        helper = MODULE_PATH.parents[2] / "cmake" / "OlrTestRuntime.cmake"
        text = helper.read_text(encoding="utf-8")

        self.assertIn(
            '"QML_IMPORT_PATH=path_list_prepend:${_olr_qml_dir}"', text
        )
        self.assertIn(
            '"QML2_IMPORT_PATH=path_list_prepend:${_olr_qml_dir}"', text
        )
        self.assertNotRegex(
            text,
            r'ENVIRONMENT\s+"QML2?_IMPORT_PATH=',
        )

    def test_qmlstyle_tests_register_project_qml_import_root(self) -> None:
        qmlstyle = (
            MODULE_PATH.parents[2] / "tests" / "qmlstyle" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        windows_block = re.search(
            r"if\(WIN32\)([\s\S]*?)endif\(\)", qmlstyle
        )
        self.assertIsNotNone(windows_block)
        block = windows_block.group(1)

        self.assertIn(
            '"QML_IMPORT_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/qml"',
            block,
        )
        self.assertIn(
            '"QML2_IMPORT_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/qml"',
            block,
        )
        for test_name in self.policy.QML_STYLE_TESTS:
            self.assertIn(test_name, block)

    def test_runtime_contract_declares_project_qml_import_root(self) -> None:
        smoke = (
            MODULE_PATH.parents[2] / "tests" / "smoke" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        self.assertIn(
            '--project-qml-dir "${CMAKE_BINARY_DIR}/qml"',
            smoke,
        )

    def test_registration_check_preserves_enclosing_ctest_logs(self) -> None:
        ctest_value = shutil.which("ctest")
        self.assertIsNotNone(ctest_value, "CTest is required for the integration test")
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / "build"
            temporary_logs = build / "Testing" / "Temporary"
            temporary_logs.mkdir(parents=True)
            sentinels = {
                temporary_logs / "LastTest.log": b"parent LastTest sentinel\r\n",
                temporary_logs / "LastTestsFailed.log": b"parent failures sentinel\r\n",
                temporary_logs / "CTestCostData.txt": b"parent cost sentinel\r\n",
            }
            for path, content in sentinels.items():
                path.write_bytes(content)

            runtime_dirs = tuple(build / "runtime" / str(index) for index in range(6))
            for path in runtime_dirs:
                path.mkdir(parents=True)
            plugins = build / "plugins"
            qml = build / "qt-qml"
            project_qml = build / "qml"
            plugins.mkdir()
            qml.mkdir()
            project_qml.mkdir()
            base_modifications = [
                f"PATH=path_list_prepend:{path.as_posix()}"
                for path in reversed(runtime_dirs)
            ]
            base_modifications.extend(
                (
                    f"QML_IMPORT_PATH=path_list_prepend:{qml.as_posix()}",
                    f"QML2_IMPORT_PATH=path_list_prepend:{qml.as_posix()}",
                )
            )
            tests = (
                "tst_muxer",
                "tst_qml_codecsettings",
                *self.policy.QML_STYLE_TESTS,
            )
            lines = [f'add_test({name} "missing-{name}.exe")' for name in tests]
            for test_name in tests:
                modifications = list(base_modifications)
                if test_name in self.policy.QML_STYLE_TESTS:
                    modifications.extend(
                        (
                            "QML_IMPORT_PATH=path_list_prepend:"
                            + project_qml.as_posix(),
                            "QML2_IMPORT_PATH=path_list_prepend:"
                            + project_qml.as_posix(),
                        )
                    )
                lines.append(
                    "set_tests_properties("
                    + test_name
                    + ' PROPERTIES ENVIRONMENT "QT_PLUGIN_PATH='
                    + plugins.as_posix()
                    + ';QT_QPA_PLATFORM=offscreen" ENVIRONMENT_MODIFICATION "'
                    + ";".join(modifications)
                    + '")'
                )
            (build / "CTestTestfile.cmake").write_text(
                "\n".join(lines) + "\n", encoding="utf-8"
            )

            self.policy.check_registration(
                Path(ctest_value),
                build,
                runtime_dirs,
                plugins,
                qml,
                project_qml,
            )

            self.assertEqual(
                {path: path.read_bytes() for path in sentinels}, sentinels
            )

    def test_cmake_graph_contract_rejects_disabled_qml_plugin_dependencies(self) -> None:
        contract = MODULE_PATH.parents[2] / "cmake" / "OlrTargetContracts.cmake"
        self.assertTrue(contract.is_file(), "missing generated target-graph contract")

        def configure(enabled: bool) -> subprocess.CompletedProcess[str]:
            with tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary) / "source"
                build = Path(temporary) / "build"
                source.mkdir()
                (source / "CMakeLists.txt").write_text(
                    "\n".join(
                        (
                            "cmake_minimum_required(VERSION 3.16)",
                            "project(TargetContract NONE)",
                            "add_custom_target(OpenLiveReplay)",
                            "add_custom_target(OlrStyleplugin)",
                            "add_custom_target(OlrThemeplugin)",
                            f"if({'TRUE' if enabled else 'FALSE'})",
                            "  add_dependencies(OpenLiveReplay OlrStyleplugin OlrThemeplugin)",
                            "endif()",
                            f'include("{contract.as_posix()}")',
                            "olr_require_target_dependencies(OpenLiveReplay",
                            "  OlrStyleplugin OlrThemeplugin)",
                        )
                    ),
                    encoding="utf-8",
                )
                return subprocess.run(
                    [
                        "cmake",
                        "-S",
                        str(source),
                        "-B",
                        str(build),
                        "-G",
                        "Ninja",
                    ],
                    text=True,
                    capture_output=True,
                    check=False,
                )

        enabled = configure(True)
        self.assertEqual(enabled.returncode, 0, enabled.stdout + enabled.stderr)
        disabled = configure(False)
        self.assertNotEqual(disabled.returncode, 0)
        self.assertRegex(
            disabled.stdout + disabled.stderr,
            r"(?s)OpenLiveReplay.*OlrStyleplugin.*OlrThemeplugin",
        )

    def test_top_level_readme_rejects_windows_launch_bypasses(self) -> None:
        checker = getattr(
            self.policy,
            "check_top_level_readme_windows_contract",
            None,
        )
        self.assertIsNotNone(checker, "top-level README policy is not implemented")
        invalid = """
On Windows, use Build, Run, and Debug from the VS Code status bar.
```bash
ctest --test-dir build --output-on-failure
```
"""
        failures = checker(invalid)
        self.assertTrue(any("status-bar Run/Debug" in failure for failure in failures))
        self.assertTrue(any("bare CTest" in failure for failure in failures))

        mixed = """
On Windows, use Build, Run, and Debug from the VS Code status bar.
If that fails, start the app with `OpenLiveReplay: start controlled Windows app`.
Run tests with `python tools/run_ctest.py --test-dir build --output-on-failure`.
"""
        mixed_failures = checker(mixed)
        self.assertTrue(
            any("status-bar Run/Debug" in failure for failure in mixed_failures)
        )

        valid = """
On Windows, start the app with `OpenLiveReplay: start controlled Windows app`.
Run tests with `python tools/run_ctest.py --test-dir build --output-on-failure`.
"""
        self.assertEqual(
            checker(valid),
            [],
        )

        split_bypass = """
Windows users can click the status-bar Run button. Debug works there too.
Start the supported path with `OpenLiveReplay: start controlled Windows app`.
Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        self.assertTrue(
            any(
                "status-bar Run/Debug" in failure
                for failure in checker(split_bypass)
            )
        )

        safe_warning = """
On Windows, never use status-bar Run or Debug; instead use
`OpenLiveReplay: start controlled Windows app`.
Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        self.assertEqual(checker(safe_warning), [])

    def test_top_level_readme_keeps_markdown_list_items_separate(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        text = """
## Quick start

1. Configure the project.
2. From the CMake Tools status bar, pick the Debug configure preset.
3. On macOS/Linux, use Build, Run, and Debug from the status bar. On Windows,
   start the app with **OpenLiveReplay: start controlled Windows app**.

Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        self.assertEqual(checker(text), [])

    def test_top_level_readme_rejects_status_bar_advice_split_across_list_items(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        text = """
## Windows quick start

Use the VS Code status bar for application actions:

- Click Run there.
- Debug works there too.
- The supported task is **OpenLiveReplay: start controlled Windows app**.

Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        failures = checker(text)
        self.assertTrue(
            any("status-bar Run/Debug" in failure for failure in failures),
            failures,
        )

    def test_top_level_readme_requires_positive_safe_guidance_in_windows_section(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        text = """
## Windows quick start

Start and test from the IDE.

## Reference

The task is **OpenLiveReplay: start controlled Windows app**.
Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        failures = checker(text)
        self.assertTrue(
            any("Windows section" in failure for failure in failures),
            failures,
        )

    def test_top_level_readme_scopes_platform_and_negation_by_clause(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        mixed_platforms = """
## Quick start

On macOS/Linux, use Run from the status bar; on Windows, click Debug there.
On Windows, start with **OpenLiveReplay: start controlled Windows app**.
Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        mixed_negation = """
## Windows

Status-bar Run is unavailable; click Debug there instead.
Start with **OpenLiveReplay: start controlled Windows app**.
Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        for text in (mixed_platforms, mixed_negation):
            with self.subTest(text=text):
                failures = checker(text)
                self.assertTrue(
                    any("status-bar Run/Debug" in failure for failure in failures),
                    failures,
                )

    def test_top_level_readme_allows_disabled_advice_with_affirmative_guidance(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        text = """
## Windows

Status-bar Run and Debug are unavailable on Windows.
Start with **OpenLiveReplay: start controlled Windows app**.
Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        self.assertEqual(checker(text), [])

    def test_top_level_readme_rejects_negated_or_unavailable_launcher_mentions(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        text = """
## Windows

Do not use **OpenLiveReplay: start controlled Windows app** because it is unavailable.
The command `python tools/run_ctest.py --test-dir build` is disabled.
"""
        failures = checker(text)
        self.assertTrue(any("positive controlled app" in failure for failure in failures))
        self.assertTrue(any("positive controlled CTest" in failure for failure in failures))

    def test_top_level_readme_preserves_nested_windows_heading_inheritance(self) -> None:
        checker = self.policy.check_top_level_readme_windows_contract
        text = """
## Windows

### Application

Start with **OpenLiveReplay: start controlled Windows app**.

### Tests

Run tests with `python tools/run_ctest.py --test-dir build`.
"""
        self.assertEqual(checker(text), [])

    @classmethod
    def setUpClass(cls) -> None:
        cls.policy = load_module()

    def test_windows_job_classification_handles_literal_matrix_and_dynamic_runners(self) -> None:
        self.assertTrue(self.policy.job_can_run_on_windows("runs-on: windows-latest\n"))
        self.assertTrue(
            self.policy.job_can_run_on_windows(
                "runs-on: ${{ matrix.os }}\nstrategy:\n  matrix:\n    os: [ubuntu-latest, windows-latest]\n"
            )
        )
        self.assertFalse(
            self.policy.job_can_run_on_windows(
                "runs-on: ${{ matrix.os }}\nstrategy:\n  matrix:\n    os: [ubuntu-latest, macos-14]\n"
            )
        )
        self.assertTrue(self.policy.job_can_run_on_windows("runs-on: ${{ vars.RUNNER }}\n"))
        self.assertTrue(
            self.policy.job_can_run_on_windows("runs-on:\n  - self-hosted\n  - windows\n")
        )
        self.assertFalse(
            self.policy.job_can_run_on_windows("runs-on:\n  - self-hosted\n  - linux\n")
        )

    def test_raw_windows_launch_detection_rejects_shell_variants_only(self) -> None:
        bad = """
        run: |
          ctest --test-dir build
          Start-Process .\\tst_muxer.exe
          & "$env:BUILD\\rhi_d3d_probe.exe"
          "$probe"
          FOO=bar ctest --test-dir build
          powershell -Command .\\tst_muxer.exe
          env FOO=bar ctest --test-dir build
          powershell -Command ctest --test-dir build
          cmd /c ctest --test-dir build
        """
        self.assertEqual(len(self.policy.find_raw_windows_launches(bad)), 9)
        allowed = """
        run: |
          python tools/run_ctest.py --test-dir build -R '^tst_muxer$'
          "$HOST_PYTHON" tools/run_ctest.py --test-dir build -R '^tst_muxer$'
          python tests/e2e/run_qt_core_media_smoke.py --harness build/qt_core_media_smoke.exe
          "$HOST_PYTHON" -m unittest tests.scripts.test_run_ctest
          "$NINJA_BIN" -C build
        """
        self.assertEqual(self.policy.find_raw_windows_launches(allowed), [])

    def test_policy_selector_requires_an_executed_launcher_command(self) -> None:
        self.assertFalse(
            self.policy.has_executed_policy_selector(
                "name: windows_source_launch_policy\nrun: echo windows_source_launch_policy\n"
            )
        )
        self.assertFalse(
            self.policy.has_executed_policy_selector(
                "# python tools/run_ctest.py -R '^windows_source_launch_policy$'\n"
            )
        )
        self.assertFalse(
            self.policy.has_executed_policy_selector(
                "run: python tools/run_ctest.py -R '^not_windows_source_launch_policy$'\n"
            )
        )
        self.assertFalse(
            self.policy.has_executed_policy_selector(
                "run: python tools/run_ctest.py -R windows_source_launch_policy_typo\n"
            )
        )
        self.assertTrue(
            self.policy.has_executed_policy_selector(
                "run: python tools/run_ctest.py --test-dir build "
                "-R '^windows_source_launch_policy$'\n"
            )
        )
        self.assertTrue(
            self.policy.has_executed_policy_selector(
                'run: "$HOST_PYTHON" tools/run_ctest.py --test-dir build '
                "-R '^windows_source_launch_policy$'\n"
            )
        )
        self.assertTrue(
            self.policy.has_executed_policy_selector(
                'run: python tools/run_ctest.py -R="^windows_source_launch_policy$"\n'
            )
        )
        self.assertTrue(
            self.policy.has_executed_policy_selector(
                "run: python tools/run_ctest.py -R=^windows_source_launch_policy$\n"
            )
        )

    def test_vscode_policy_rejects_hardcoded_or_ambient_windows_launch(self) -> None:
        launch = {
            "configurations": [
                {
                    "name": "bad",
                    "type": "cppdbg",
                    "request": "launch",
                    "windows": {
                        "environment": [
                            {
                                "name": "PATH",
                                "value": "${workspaceFolder}/windows_build/dist/ffmpeg/bin;${env:PATH}",
                            }
                        ]
                    },
                }
            ]
        }
        failures = self.policy.validate_vscode_windows_contract({"tasks": []}, launch)
        self.assertTrue(any("attach-only" in failure for failure in failures))
        self.assertTrue(any("hard-coded or ambient PATH" in failure for failure in failures))

        inherited_launch = {
            "configurations": [
                {"name": "unguarded", "type": "cppdbg", "request": "launch"},
                {
                    "name": "OpenLiveReplay: Attach QML",
                    "type": "qml",
                    "request": "attach",
                    "host": "localhost",
                    "port": "${command:qt-qml.debugPort}",
                },
            ]
        }
        inherited_failures = self.policy.validate_vscode_windows_contract(
            {"tasks": []}, inherited_launch
        )
        self.assertTrue(
            any("not fail-closed on Windows" in failure for failure in inherited_failures)
        )

    def test_vscode_policy_accepts_cache_derived_tasks_and_real_qml_attach(self) -> None:
        tasks = {
            "tasks": [
                {
                    "label": "OpenLiveReplay: start controlled Windows app",
                    "type": "process",
                    "command": "python",
                    "args": [
                        "${workspaceFolder}/tools/run_windows_app.py",
                        "--build-dir",
                        "${command:cmake.buildDirectory}",
                        "--executable",
                        "${command:cmake.launchTargetPath}",
                    ],
                },
                {
                    "label": "OpenLiveReplay: start controlled Windows QML-debug app",
                    "type": "process",
                    "command": "python",
                    "args": [
                        "${workspaceFolder}/tools/run_windows_app.py",
                        "--build-dir",
                        "${command:cmake.buildDirectory}",
                        "--executable",
                        "${command:cmake.launchTargetPath}",
                        "--",
                        "-qmljsdebugger=port:${command:qt-qml.debugPort},block",
                    ],
                },
            ]
        }
        launch = {
            "configurations": [
                {
                    "name": "OpenLiveReplay: Debug",
                    "type": "cppdbg",
                    "request": "launch",
                    "windows": {"request": "attach", "processId": "${command:pickProcess}"},
                },
                {
                    "name": "OpenLiveReplay: Attach QML",
                    "type": "qml",
                    "request": "attach",
                    "host": "localhost",
                    "port": "${command:qt-qml.debugPort}",
                },
            ]
        }
        self.assertEqual(self.policy.validate_vscode_windows_contract(tasks, launch), [])

    def test_quick_start_policy_rejects_unqualified_bare_ctest(self) -> None:
        failures = self.policy.check_windows_quick_start_docs(
            "## Quick start\n```bash\nctest --test-dir build -L ci\n```\n"
        )
        self.assertTrue(any("bare CTest" in failure for failure in failures))
        inline_failures = self.policy.check_windows_quick_start_docs(
            "## Quick start\nRun a subset: `ctest --test-dir build -L unit`.\n"
        )
        self.assertTrue(any("bare CTest" in failure for failure in inline_failures))
        self.assertEqual(
            self.policy.check_windows_quick_start_docs(
                "## Quick start\n```bash\n"
                "# Windows\npython tools/run_ctest.py --test-dir build -L ci\n"
                "# macOS/Linux\nctest --test-dir build -L ci\n```\n"
            ),
            [],
        )
        later_failures = self.policy.check_windows_quick_start_docs(
            "## Quick start\n# Windows\npython tools/run_ctest.py --test-dir build\n"
            "The gate **hard-fails** on missing tools.\n"
            "## Later\n```bash\nctest --test-dir build -L unit\n```\n"
        )
        self.assertTrue(any("bare CTest" in failure for failure in later_failures))
        self.assertEqual(
            self.policy.check_windows_quick_start_docs(
                "## Later\n```bash\n# macOS/Linux\nctest --test-dir build -L unit\n```\n"
            ),
            [],
        )

    def test_windows_executables_receive_central_embedded_manifest_resource(self) -> None:
        root = MODULE_PATH.parents[2]
        module = root / "cmake" / "OlrWindowsManifest.cmake"
        resource = root / "cmake" / "olr_windows_application_manifest.rc"
        manifest = root / "cmake" / "olr_windows_application.manifest"
        self.assertTrue(module.is_file())
        self.assertTrue(resource.is_file())
        self.assertTrue(manifest.is_file())
        module_text = module.read_text(encoding="utf-8")
        self.assertIn("BUILDSYSTEM_TARGETS", module_text)
        self.assertIn("SUBDIRECTORIES", module_text)
        self.assertIn("target_sources", module_text)
        self.assertRegex(resource.read_text(encoding="utf-8"), r"1\s+RT_MANIFEST")
        manifest_text = manifest.read_text(encoding="utf-8")
        self.assertIn('manifestVersion="1.0"', manifest_text)
        self.assertIn('requestedExecutionLevel level="asInvoker"', manifest_text)
        root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("olr_embed_all_windows_executables", root_cmake)


if __name__ == "__main__":
    unittest.main()
