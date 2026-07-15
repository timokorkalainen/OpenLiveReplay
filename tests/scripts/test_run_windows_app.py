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
import time
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

    def test_runtime_replaces_controlled_environment_case_insensitively(self) -> None:
        windows_dir = self.workspace / "api-windows"
        system_dir = windows_dir / "System32"
        inherited = {
            "Path": str(self.workspace / "hostile-bin"),
            "SYSTEMROOT": str(self.workspace / "hostile-windows"),
            "qt_plugin_path": str(self.workspace / "hostile-plugins"),
            "qml_import_path": str(self.workspace / "hostile-qml"),
            "unrelated": "preserved",
        }
        with mock.patch.object(
            self.launcher.runtime_guard,
            "windows_system_directories",
            return_value=(system_dir, windows_dir),
            create=True,
        ):
            env = self.launcher.prepare_windows_app_runtime(
                self.build, self.app, inherited
            )

        for controlled in (
            "PATH",
            "SystemRoot",
            "QT_PLUGIN_PATH",
            "QML_IMPORT_PATH",
        ):
            self.assertEqual(
                sum(key.casefold() == controlled.casefold() for key in env),
                1,
                controlled,
            )
        self.assertEqual(env["unrelated"], "preserved")
        self.assertNotIn("hostile", "\n".join(env.values()))

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_runtime_stages_qml_for_external_launch_lease(self) -> None:
        source_qml = self.qt / "qml"
        (source_qml / "Module.qml").write_text("source", encoding="utf-8")
        lease = self.launcher.runtime_guard.RuntimeAuditLease()
        try:
            env = self.launcher.prepare_windows_app_runtime(
                self.build, self.app, {}, audit_lease=lease
            )
            staged_qml = Path(env["QML_IMPORT_PATH"])
            self.assertNotEqual(staged_qml, source_qml)
            self.assertEqual(env["QML2_IMPORT_PATH"], str(staged_qml))
            self.assertEqual(
                (staged_qml / "Module.qml").read_text(encoding="utf-8"), "source"
            )
        finally:
            lease.close()
        self.assertFalse(staged_qml.exists())

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_runtime_stages_windows_and_offscreen_platform_plugins(self) -> None:
        platforms = self.qt / "plugins" / "platforms"
        (platforms / "qwindows.dll").write_bytes(b"windows")
        (platforms / "qoffscreen.dll").write_bytes(b"offscreen")

        with self.launcher.runtime_guard.RuntimeAuditLease() as lease:
            env = self.launcher.prepare_windows_app_runtime(
                self.build, self.app, {}, audit_lease=lease
            )
            staged = Path(env["QT_QPA_PLATFORM_PLUGIN_PATH"])
            self.assertEqual((staged / "qwindows.dll").read_bytes(), b"windows")
            self.assertEqual(
                (staged / "qoffscreen.dll").read_bytes(), b"offscreen"
            )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_runtime_replaces_source_qt_conf_with_closed_staged_paths(self) -> None:
        source_conf = self.build / "qt.conf"
        source_conf.write_text(
            "[Paths]\nPlugins=C:/mutable/plugins\nQmlImports=C:/mutable/qml\n",
            encoding="utf-8",
        )

        with self.launcher.runtime_guard.RuntimeAuditLease() as lease:
            env = self.launcher.prepare_windows_app_runtime(
                self.build, self.app, {}, audit_lease=lease
            )
            staged_conf = lease.launch_executable.parent / "qt.conf"
            rendered = staged_conf.read_text(encoding="utf-8")
            self.assertNotIn("mutable", rendered)
            self.assertIn(Path(env["QT_PLUGIN_PATH"]).as_posix(), rendered)
            self.assertIn(Path(env["QML_IMPORT_PATH"]).as_posix(), rendered)
            self.assertEqual(
                source_conf.read_text(encoding="utf-8"),
                "[Paths]\nPlugins=C:/mutable/plugins\nQmlImports=C:/mutable/qml\n",
            )
            with self.assertRaises(OSError):
                staged_conf.write_text("Plugins=C:/late", encoding="utf-8")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_external_app_lease_closes_app_and_runtime_namespaces(self) -> None:
        (self.compiler / "runtime-tool.dll").write_bytes(b"compiler")
        (self.qt / "bin" / "runtime-tool.dll").write_bytes(b"qt")
        (self.build / "OpenLiveReplaySupport.dll").write_bytes(b"support")

        with (
            self.launcher.runtime_guard.RuntimeAuditLease() as lease,
            mock.patch.object(
                self.launcher.runtime_guard,
                "transitive_runtime_closure",
                return_value=(self.compiler / "runtime-tool.dll",),
            ) as closure,
        ):
            env = self.launcher.prepare_windows_app_runtime(
                self.build, self.app, {}, audit_lease=lease
            )
            staged_app = lease.launch_executable
            loader = lease.launch_runtime_directory
            runtime_dirs = lease.launch_runtime_directories
            self.assertNotEqual(staged_app, self.app)
            self.assertEqual(staged_app.read_bytes(), self.app.read_bytes())
            self.assertEqual(
                (staged_app.parent / "OpenLiveReplaySupport.dll").read_bytes(),
                b"support",
            )
            self.assertEqual(
                Path(env["PATH"].split(os.pathsep)[0]), runtime_dirs[0]
            )
            self.assertEqual(
                (runtime_dirs[0] / "runtime-tool.dll").read_bytes(), b"compiler"
            )
            for directory in (loader, *runtime_dirs, staged_app.parent):
                with self.subTest(directory=directory), self.assertRaises(OSError):
                    (directory / "late-injection.dll").write_bytes(b"poison")
            self.assertIn(
                self.build / "OpenLiveReplaySupport.dll",
                closure.call_args.args[0],
            )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_representative_app_snapshot_ignores_nonruntime_payload(self) -> None:
        noise_roots = (
            self.compiler,
            self.qt / "bin",
            self.qt / "plugins" / "documentation",
            self.qt / "qml" / "examples",
        )
        for root in noise_roots:
            root.mkdir(parents=True, exist_ok=True)
            with (root / "unused-debug-payload.pdb").open("wb") as stream:
                stream.truncate(64 << 20)
        for index in range(1024):
            root = noise_roots[index % len(noise_roots)]
            (root / f"unused-{index:04d}.txt").write_bytes(b"not runtime")

        measurements: list[tuple[float, int, int]] = []
        for _temperature in ("cold", "warm"):
            started = time.perf_counter()
            with self.launcher.runtime_guard.RuntimeAuditLease() as lease:
                self.launcher.prepare_windows_app_runtime(
                    self.build, self.app, {}, audit_lease=lease
                )
                measurements.append(
                    (
                        time.perf_counter() - started,
                        lease.staged_file_count,
                        lease.staged_byte_count,
                    )
                )
        for elapsed, file_count, byte_count in measurements:
            self.assertLess(elapsed, 5.0)
            self.assertLess(file_count, 128)
            self.assertLess(byte_count, 64 << 20)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_actual_qt_app_closure_has_practical_size_time_budgets(self) -> None:
        candidates = sorted(
            path
            for path in Path("C:/Qt").glob("*/mingw_64")
            if (path / "bin" / "Qt6Core.dll").is_file()
        )
        if not candidates:
            self.skipTest("no installed C:/Qt/*/mingw_64 kit")
        qt = candidates[-1]
        project_source = Path(__file__).resolve().parents[2]
        cache_path = self.build / "CMakeCache.txt"
        cache = cache_path.read_text(encoding="utf-8")
        cache = cache.replace(
            f"Qt6_DIR:PATH={self.qt / 'lib/cmake/Qt6'}",
            f"Qt6_DIR:PATH={qt / 'lib/cmake/Qt6'}",
        ).replace(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.workspace}",
            f"CMAKE_HOME_DIRECTORY:INTERNAL={project_source}",
        )
        cache_path.write_text(cache, encoding="utf-8")

        started = time.perf_counter()
        with self.launcher.runtime_guard.RuntimeAuditLease() as lease:
            env = self.launcher.prepare_windows_app_runtime(
                self.build,
                self.app,
                {"SystemRoot": str(Path(self.temporary.name) / "Windows")},
                audit_lease=lease,
            )
            elapsed = time.perf_counter() - started
            self.assertNotEqual(Path(env["QT_PLUGIN_PATH"]), qt / "plugins")
            self.assertNotEqual(Path(env["QML_IMPORT_PATH"]), qt / "qml")
            self.assertLess(lease.staged_byte_count, 160 << 20)
            self.assertLess(lease.staged_file_count, 1800)
        self.assertLess(elapsed, 25.0)

    @unittest.skipUnless(os.name == "nt", "requires a built Windows app")
    def test_real_built_app_snapshot_has_absolute_size_time_budgets(self) -> None:
        project_source = Path(__file__).resolve().parents[2]
        candidates: list[tuple[float, Path, Path]] = []
        for app in project_source.glob("build*/**/OpenLiveReplay.exe"):
            for build in (app.parent, *app.parents):
                cache = build / "CMakeCache.txt"
                if cache.is_file():
                    candidates.append((app.stat().st_mtime, build, app))
                    break
                if build == project_source:
                    break
        if not candidates:
            self.fail("actual built OpenLiveReplay.exe with CMakeCache.txt is required")
        _modified, build, app = max(candidates)

        started = time.perf_counter()
        with self.launcher.runtime_guard.RuntimeAuditLease() as lease:
            self.launcher.prepare_windows_app_runtime(
                build,
                app,
                {"SystemRoot": os.environ.get("SystemRoot", "C:/Windows")},
                audit_lease=lease,
            )
            elapsed = time.perf_counter() - started
            self.assertEqual(lease.launch_executable.read_bytes(), app.read_bytes())
            # Debug builds contain an ~86 MiB executable; this absolute ceiling
            # still rejects restaging the verified adjacent Qt runtime peers.
            self.assertLess(lease.staged_byte_count, 240 << 20)
            self.assertLess(lease.staged_file_count, 1900)
        self.assertLess(elapsed, 30.0)

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
