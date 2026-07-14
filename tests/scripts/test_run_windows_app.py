#!/usr/bin/env python3
"""Tests for the cache-derived controlled Windows app launcher."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "run_windows_app.py"


def load_module():
    if not MODULE_PATH.is_file():
        raise AssertionError(f"missing controlled Windows app launcher: {MODULE_PATH}")
    spec = importlib.util.spec_from_file_location("run_windows_app", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load controlled Windows app launcher")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RunWindowsAppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.launcher = load_module()

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.workspace = root / "workspace"
        self.build = self.workspace / "build" / "active"
        self.build.mkdir(parents=True)
        self.app = self.build / "OpenLiveReplay.exe"
        self.app.write_bytes(b"app")

        configured = root / "configured-elsewhere"
        self.compiler = configured / "toolchain" / "bin"
        self.qt = configured / "Qt"
        self.ffmpeg = configured / "ffmpeg"
        self.srt = configured / "srt"
        for directory in (
            self.compiler,
            self.qt / "bin",
            self.qt / "plugins",
            self.qt / "qml",
            self.ffmpeg / "bin",
            self.ffmpeg / "lib",
            self.srt / "bin",
            self.srt / "lib",
        ):
            directory.mkdir(parents=True)
        compiler = self.compiler / "g++.exe"
        compiler.write_bytes(b"compiler")
        cache = "\n".join(
            (
                f"CMAKE_CXX_COMPILER:FILEPATH={compiler}",
                f"Qt6_DIR:PATH={self.qt / 'lib/cmake/Qt6'}",
                f"OLR_FFMPEG_ROOT:PATH={self.ffmpeg}",
                f"OLR_SRT_ROOT:PATH={self.srt}",
            )
        )
        (self.build / "CMakeCache.txt").write_text(cache + "\n", encoding="utf-8")

    def test_runtime_uses_active_cache_roots_not_workspace_or_ambient_path(self) -> None:
        ambient = self.workspace / "windows_build" / "dist" / "ffmpeg" / "bin"
        system_root = Path(self.temporary.name) / "Windows"
        env = self.launcher.prepare_windows_app_runtime(
            self.build,
            self.app,
            {"PATH": str(ambient), "SystemRoot": str(system_root)},
        )
        self.assertEqual(
            env["PATH"].split(os.pathsep),
            [
                str(self.compiler),
                str(self.qt / "bin"),
                str(self.ffmpeg / "bin"),
                str(self.ffmpeg / "lib"),
                str(self.srt / "bin"),
                str(self.srt / "lib"),
                str(system_root / "System32"),
                str(system_root),
            ],
        )
        self.assertNotIn(str(ambient), env["PATH"])
        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt / "plugins"))
        self.assertEqual(env["QML_IMPORT_PATH"], str(self.qt / "qml"))

    def test_rejects_executable_outside_selected_build(self) -> None:
        outside = Path(self.temporary.name) / "outside" / "OpenLiveReplay.exe"
        outside.parent.mkdir()
        outside.write_bytes(b"outside")
        with self.assertRaisesRegex(ValueError, "selected build directory"):
            self.launcher.prepare_windows_app_runtime(self.build, outside, {})

    def test_main_preserves_arguments_and_exact_child_exit(self) -> None:
        completed = mock.Mock(returncode=37)
        guard = mock.MagicMock()
        guard.__enter__.return_value = None
        with (
            mock.patch.object(self.launcher, "prepare_windows_app_runtime", return_value={"PATH": "controlled"}),
            mock.patch.object(self.launcher, "suppress_windows_error_dialogs", return_value=guard),
            mock.patch.object(self.launcher.subprocess, "run", return_value=completed) as run,
        ):
            result = self.launcher.main(
                [
                    "--build-dir",
                    str(self.build),
                    "--executable",
                    str(self.app),
                    "--",
                    "--value=with spaces",
                    "-qmljsdebugger=test",
                ]
            )
        self.assertEqual(result, 37)
        self.assertEqual(
            run.call_args.args[0],
            [str(self.app.resolve()), "--value=with spaces", "-qmljsdebugger=test"],
        )
        self.assertEqual(run.call_args.kwargs["env"], {"PATH": "controlled"})
        self.assertFalse(run.call_args.kwargs["check"])
        guard.__enter__.assert_called_once()
        guard.__exit__.assert_called_once()


if __name__ == "__main__":
    unittest.main()
