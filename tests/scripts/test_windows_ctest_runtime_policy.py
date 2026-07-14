#!/usr/bin/env python3
"""Unit tests for fail-closed Windows launch-source policy helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
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
        """
        self.assertEqual(len(self.policy.find_raw_windows_launches(bad)), 4)
        allowed = """
        run: |
          python tools/run_ctest.py --test-dir build -R '^tst_muxer$'
          python tests/e2e/run_qt_core_media_smoke.py --harness build/qt_core_media_smoke.exe
        """
        self.assertEqual(self.policy.find_raw_windows_launches(allowed), [])

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


if __name__ == "__main__":
    unittest.main()
