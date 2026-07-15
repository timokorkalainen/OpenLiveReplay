#!/usr/bin/env python3
"""Tests for the cache-derived controlled Windows app launcher."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import tempfile
import unittest
from unittest import mock

from tests.scripts.test_run_ctest import _write_manifest_pe_fixture


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
        _write_manifest_pe_fixture(self.app)

        configured = root / "configured-elsewhere"
        self.compiler = configured / "toolchain" / "bin"
        self.qt = configured / "Qt"
        self.ffmpeg = configured / "ffmpeg"
        self.srt = configured / "srt"
        for directory in (
            self.compiler,
            self.qt / "bin",
            self.qt / "plugins",
            self.qt / "plugins" / "platforms",
            self.qt / "qml",
            self.qt / "lib" / "cmake" / "Qt6",
            self.ffmpeg / "bin",
            self.ffmpeg / "lib",
            self.srt / "bin",
            self.srt / "lib",
        ):
            directory.mkdir(parents=True)
        compiler = self.compiler / "g++.exe"
        compiler.write_bytes(b"compiler")
        cmake = configured / "cmake" / "bin" / "cmake.exe"
        cmake.parent.mkdir(parents=True)
        cmake.write_bytes(b"cmake")
        cache = "\n".join(
            (
                f"CMAKE_CXX_COMPILER:FILEPATH={compiler}",
                f"CMAKE_COMMAND:INTERNAL={cmake}",
                f"Qt6_DIR:PATH={self.qt / 'lib/cmake/Qt6'}",
                f"OLR_FFMPEG_ROOT:PATH={self.ffmpeg}",
                f"OLR_SRT_ROOT:PATH={self.srt}",
                f"CMAKE_HOME_DIRECTORY:INTERNAL={self.workspace}",
            )
        )
        (self.build / "CMakeCache.txt").write_text(cache + "\n", encoding="utf-8")

    def test_runtime_uses_active_cache_roots_not_workspace_or_ambient_path(self) -> None:
        ambient = self.workspace / "windows_build" / "dist" / "ffmpeg" / "bin"
        windows_dir = self.workspace / "api-windows"
        system_dir = windows_dir / "System32"
        with mock.patch.object(
            self.launcher.runtime_guard,
            "windows_system_directories",
            return_value=(system_dir, windows_dir),
            create=True,
        ):
            env = self.launcher.prepare_windows_app_runtime(
                self.build,
                self.app,
                {
                    "PATH": str(ambient),
                    "SystemRoot": str(self.workspace / "hostile-windows"),
                    "QT_PLUGIN_PATH": str(self.workspace / "poisoned-plugins"),
                    "QT_QPA_PLATFORM_PLUGIN_PATH": str(self.workspace / "poisoned-platforms"),
                    "QML_IMPORT_PATH": str(self.workspace / "poisoned-qml"),
                    "QML2_IMPORT_PATH": str(self.workspace / "poisoned-qml2"),
                },
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
                str(system_dir),
                str(windows_dir),
            ],
        )
        self.assertNotIn(str(ambient), env["PATH"])
        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt / "plugins"))
        self.assertEqual(
            env["QT_QPA_PLATFORM_PLUGIN_PATH"], str(self.qt / "plugins" / "platforms")
        )
        self.assertEqual(env["QML_IMPORT_PATH"], str(self.qt / "qml"))
        self.assertNotIn("poisoned", "\n".join(env.values()))

    def test_cache_rejects_duplicate_wrong_type_and_relative_roots(self) -> None:
        cache = self.build / "CMakeCache.txt"
        original = cache.read_text(encoding="utf-8")
        cases = (
            (original + f"Qt6_DIR:PATH={self.workspace / 'other'}\n", "duplicate"),
            (original.replace("Qt6_DIR:PATH=", "Qt6_DIR:STRING="), "type"),
            (
                original.replace(
                    f"OLR_SRT_ROOT:PATH={self.srt}",
                    "OLR_SRT_ROOT:PATH=relative/srt",
                ),
                "absolute",
            ),
        )
        for contents, pattern in cases:
            with self.subTest(pattern=pattern):
                cache.write_text(contents, encoding="utf-8")
                with self.assertRaises(Exception) as raised:
                    self.launcher.prepare_windows_app_runtime(self.build, self.app, {})
                self.assertIsInstance(raised.exception, ValueError)
                self.assertRegex(str(raised.exception), pattern)
        cache.write_text(original, encoding="utf-8")

    def test_runtime_requires_embedded_manifest_for_selected_app(self) -> None:
        with mock.patch.object(
            self.launcher.runtime_guard, "reject_runtime_dll_shadowing"
        ) as audit:
            self.launcher.prepare_windows_app_runtime(
                self.build,
                self.app,
                {"SystemRoot": str(Path(self.temporary.name) / "Windows")},
            )

        self.assertTrue(audit.call_args.kwargs["require_embedded_manifests"])

    def test_rejects_executable_outside_selected_build(self) -> None:
        outside = Path(self.temporary.name) / "outside" / "OpenLiveReplay.exe"
        outside.parent.mkdir()
        outside.write_bytes(b"outside")
        with self.assertRaisesRegex(ValueError, "selected build directory"):
            self.launcher.prepare_windows_app_runtime(self.build, outside, {})

    def test_rejects_stale_qt_dll_beside_app_with_exact_provenance(self) -> None:
        configured = self.qt / "bin" / "Qt6Core.dll"
        configured_contents = b"configured-qt-6.10.3"
        configured.write_bytes(configured_contents)
        adjacent = self.build / "Qt6Core.dll"
        adjacent_contents = b"stale-qt-6.10.2"
        adjacent.write_bytes(adjacent_contents)

        with self.assertRaises(RuntimeError) as raised:
            self.launcher.prepare_windows_app_runtime(
                self.build,
                self.app,
                {"SystemRoot": str(Path(self.temporary.name) / "Windows")},
            )
        message = str(raised.exception)
        self.assertRegex(
            message,
            rf"{re.escape(str(adjacent))}.*version.*sha256.*"
            rf"{re.escape(str(configured))}.*version.*sha256",
        )
        self.assertIn(hashlib.sha256(adjacent_contents).hexdigest(), message)
        self.assertIn(hashlib.sha256(configured_contents).hexdigest(), message)
        self.assertEqual(adjacent.read_bytes(), adjacent_contents)
        self.assertEqual(configured.read_bytes(), configured_contents)

    def test_accepts_matching_runtime_dll_in_nested_build_package(self) -> None:
        package = self.build / "package" / "bin"
        package.mkdir(parents=True)
        packaged_app = package / "OpenLiveReplay.exe"
        _write_manifest_pe_fixture(packaged_app)
        configured = self.qt / "bin" / "Qt6Core.dll"
        configured.write_bytes(b"configured-qt-6.10.3")
        deployed = package / "Qt6Core.dll"
        deployed.write_bytes(configured.read_bytes())
        app_owned = package / "OpenLiveReplaySupport.dll"
        app_owned.write_bytes(b"application-owned-runtime")

        env = self.launcher.prepare_windows_app_runtime(
            self.build,
            packaged_app,
            {"SystemRoot": str(Path(self.temporary.name) / "Windows")},
        )

        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt / "plugins"))
        self.assertEqual(deployed.read_bytes(), configured.read_bytes())
        self.assertEqual(app_owned.read_bytes(), b"application-owned-runtime")

    def test_selected_app_imports_are_audited(self) -> None:
        active = self.ffmpeg / "bin" / "avcodec-62.dll"
        active.write_bytes(b"active-ffmpeg")
        with (
            mock.patch.object(
                self.launcher.runtime_guard,
                "pe_imports",
                return_value={"avcodec-61.dll"},
            ),
            self.assertRaisesRegex(
                RuntimeError,
                r"OpenLiveReplay\.exe.*avcodec-61\.dll.*OLR_FFMPEG_ROOT",
            ),
        ):
            self.launcher.prepare_windows_app_runtime(
                self.build,
                self.app,
                {"SystemRoot": str(Path(self.temporary.name) / "Windows")},
            )

    def test_stale_qt_cache_error_requires_reconfigure_guidance(self) -> None:
        shutil.rmtree(self.qt / "bin")
        with self.assertRaisesRegex(
            FileNotFoundError,
            r"Qt runtime.*Qt6_DIR.*reconfigure or recreate",
        ):
            self.launcher.prepare_windows_app_runtime(
                self.build,
                self.app,
                {"SystemRoot": str(Path(self.temporary.name) / "Windows")},
            )

    def test_main_preserves_arguments_and_exact_child_exit(self) -> None:
        guard = mock.MagicMock()
        guard.__enter__.return_value = None
        with (
            mock.patch.object(self.launcher, "prepare_windows_app_runtime", return_value={"PATH": "controlled"}),
            mock.patch.object(self.launcher, "suppress_windows_error_dialogs", return_value=guard),
            mock.patch.object(self.launcher, "run_process_tree", return_value=37) as run,
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
        self.assertEqual(run.call_args.kwargs["cwd"], self.app.resolve().parent)
        self.assertTrue(run.call_args.kwargs["enabled"])
        self.assertTrue(run.call_args.kwargs.get("private_desktop"))
        guard.__enter__.assert_called_once()
        guard.__exit__.assert_called_once()

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_main_holds_audited_app_until_process_tree_returns(self) -> None:
        replacement = self.build / "replacement.exe"
        replacement.write_bytes(b"replacement")

        def inspect_held_file(*_args, **_kwargs) -> int:
            with self.assertRaises(OSError):
                os.replace(replacement, self.app)
            return 0

        with mock.patch.object(
            self.launcher, "run_process_tree", side_effect=inspect_held_file
        ):
            self.assertEqual(
                self.launcher.main(
                    [
                        "--build-dir",
                        str(self.build),
                        "--executable",
                        str(self.app),
                    ]
                ),
                0,
            )

        os.replace(replacement, self.app)
        self.assertEqual(self.app.read_bytes(), b"replacement")


if __name__ == "__main__":
    unittest.main()
