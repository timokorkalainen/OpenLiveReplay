"""Unit tests for the build-derived Windows CTest launcher."""

from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import run_ctest, windows_runtime_cache, windows_runtime_guard
from tools.windows_runtime_guard import (
    RuntimeAuditLease,
    RuntimeRoots,
    pe_imports,
    registered_test_executables,
    reject_runtime_dll_shadowing,
)


def _write_pe_fixture(
    path: Path,
    *,
    imported_name: str = "fixture-runtime.dll",
    magic: int = 0x20B,
    directory_index: int = 13,
    attributes: int = 1,
    directory_size: int = 64,
    optional_size: int | None = None,
    directory_rva: int = 0x1100,
    name_rva: int = 0x1180,
    raw_size: int = 0x400,
    overlay_size: int = 0,
) -> None:
    pe_offset = 0x80
    if optional_size is None:
        optional_size = 0xF0 if magic == 0x20B else 0xE0
    # VA-form delay descriptors store addresses in DWORD fields, so use a valid
    # low PE32+ image base as well as the conventional PE32 base.
    image_base = 0x400000
    directory_count_offset = 108 if magic == 0x20B else 92
    directories_offset = 112 if magic == 0x20B else 96
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, magic)
    if magic == 0x20B and optional_size >= 32:
        struct.pack_into("<Q", optional, 24, image_base)
    elif magic == 0x10B and optional_size >= 32:
        struct.pack_into("<I", optional, 28, image_base)
    if optional_size >= directory_count_offset + 4:
        struct.pack_into("<I", optional, directory_count_offset, 16)
    if optional_size >= directories_offset + (directory_index + 1) * 8:
        struct.pack_into(
            "<II",
            optional,
            directories_offset + directory_index * 8,
            directory_rva,
            directory_size,
        )

    headers = bytearray(0x200)
    headers[:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, pe_offset)
    headers[pe_offset : pe_offset + 4] = b"PE\0\0"
    struct.pack_into(
        "<HHIIIHH",
        headers,
        pe_offset + 4,
        0x8664 if magic == 0x20B else 0x14C,
        1,
        0,
        0,
        0,
        optional_size,
        0,
    )
    optional_offset = pe_offset + 24
    headers[optional_offset : optional_offset + optional_size] = optional
    section = optional_offset + optional_size
    headers[section : section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", headers, section + 8, 0x1000, 0x1000, raw_size, 0x200)

    contents = headers + bytearray(max(raw_size, 0) + overlay_size)
    descriptor_offset = 0x200 + directory_rva - 0x1000
    if descriptor_offset >= 0 and descriptor_offset + 32 <= len(contents):
        if directory_index == 13:
            name_value = name_rva if attributes & 1 else image_base + name_rva
            struct.pack_into(
                "<IIIIIIII",
                contents,
                descriptor_offset,
                attributes,
                name_value,
                0,
                0,
                0,
                0,
                0,
                0,
            )
        else:
            struct.pack_into(
                "<IIIII",
                contents,
                descriptor_offset,
                0,
                0,
                0,
                name_rva,
                0,
            )
    name_offset = 0x200 + name_rva - 0x1000
    name = imported_name.encode("ascii") + b"\0"
    if name_offset >= 0 and name_offset + len(name) <= len(contents):
        contents[name_offset : name_offset + len(name)] = name
    path.write_bytes(contents)


def _write_manifest_pe_fixture(
    path: Path,
    *,
    resource_type: int = 24,
    resource_id: int = 1,
    malformed_offset: bool = False,
    manifest: bytes | None = None,
) -> None:
    if manifest is None:
        manifest = (
            b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
            b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1" '
            b'manifestVersion="1.0"><assemblyIdentity name="Olr.Test" '
            b'processorArchitecture="*" type="win32" version="1.0.0.0"/>'
            b'</assembly>'
        )
    pe_offset = 0x80
    optional_size = 0xF0
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<Q", optional, 24, 0x400000)
    struct.pack_into("<II", optional, 32, 0x1000, 0x200)
    struct.pack_into("<II", optional, 56, 0x2000, 0x200)
    struct.pack_into("<H", optional, 68, 3)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into("<II", optional, 112 + 2 * 8, 0x1100, 0x300)

    contents = bytearray(0x800)
    contents[:2] = b"MZ"
    struct.pack_into("<I", contents, 0x3C, pe_offset)
    contents[pe_offset : pe_offset + 4] = b"PE\0\0"
    struct.pack_into(
        "<HHIIIHH",
        contents,
        pe_offset + 4,
        0x8664,
        1,
        0,
        0,
        0,
        optional_size,
        0x22,
    )
    optional_offset = pe_offset + 24
    contents[optional_offset : optional_offset + optional_size] = optional
    section = optional_offset + optional_size
    contents[section : section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", contents, section + 8, 0x1000, 0x1000, 0x600, 0x200)
    struct.pack_into("<I", contents, section + 36, 0x40000040)

    resource = 0x300
    struct.pack_into("<IIHHHH", contents, resource, 0, 0, 0, 0, 0, 1)
    type_offset = 0x7FFFFFF0 if malformed_offset else 0x20
    struct.pack_into(
        "<II", contents, resource + 16, resource_type, 0x80000000 | type_offset
    )
    struct.pack_into("<IIHHHH", contents, resource + 0x20, 0, 0, 0, 0, 0, 1)
    struct.pack_into(
        "<II", contents, resource + 0x30, resource_id, 0x80000000 | 0x40
    )
    struct.pack_into("<IIHHHH", contents, resource + 0x40, 0, 0, 0, 0, 0, 1)
    struct.pack_into("<II", contents, resource + 0x50, 0x409, 0x60)
    struct.pack_into("<IIII", contents, resource + 0x60, 0x1200, len(manifest), 0, 0)
    contents[0x400 : 0x400 + len(manifest)] = manifest
    path.write_bytes(contents)


def find_real_windows_build(project_source: Path) -> tuple[Path, Path]:
    """Return the newest complete Windows build owned by this exact worktree."""
    project_source = project_source.resolve()
    candidates: dict[tuple[str, str], tuple[int, Path, Path]] = {}
    for app in project_source.glob("build*/**/OpenLiveReplay.exe"):
        if not app.is_file():
            continue
        for build in app.parents:
            if build == project_source:
                break
            cache = build / "CMakeCache.txt"
            graph = build / "CTestTestfile.cmake"
            if not cache.is_file() or not graph.is_file():
                continue
            try:
                entries = windows_runtime_guard.parse_cmake_cache(cache)
                home_entry = entries.get("CMAKE_HOME_DIRECTORY")
                if home_entry is None:
                    continue
                cache_home = Path(home_entry.value)
                if not cache_home.is_absolute() or cache_home.resolve() != project_source:
                    continue
            except (OSError, ValueError):
                continue
            resolved_build = build.resolve()
            resolved_app = app.resolve()
            if (
                not resolved_build.is_relative_to(project_source)
                or not resolved_app.is_relative_to(resolved_build)
            ):
                continue
            identity = (
                os.path.normcase(str(resolved_build)),
                os.path.normcase(str(resolved_app)),
            )
            candidates[identity] = (
                max(
                    cache.stat().st_mtime_ns,
                    graph.stat().st_mtime_ns,
                    app.stat().st_mtime_ns,
                ),
                build,
                app,
            )
            break
    if not candidates:
        raise AssertionError(
            "actual built OpenLiveReplay.exe with CMakeCache.txt, "
            "CTestTestfile.cmake, and CMAKE_HOME_DIRECTORY bound to the "
            "current project source is required"
        )
    _modified, build, app = max(candidates.values(), key=lambda item: item[0])
    return build, app


class RunCtestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name)
        self.build_dir = self.root / "build"
        self.compiler_bin = self.root / "mingw" / "bin"
        self.qt_root = self.root / "Qt" / "6.8.3" / "mingw_64"
        self.ffmpeg_root = self.root / "ffmpeg"
        self.srt_root = self.root / "srt"
        self.cmake_bin = self.root / "cmake" / "bin"
        self.ambient_bin = self.root / "ambient" / "bin"
        self.ctest = self.cmake_bin / "ctest.exe"

        self.build_dir.mkdir()
        for runtime_dir in (
            self.compiler_bin,
            self.qt_root / "bin",
            self.qt_root / "plugins",
            self.qt_root / "plugins" / "platforms",
            self.qt_root / "qml",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
            self.cmake_bin,
            self.qt_root / "lib" / "cmake" / "Qt6",
        ):
            runtime_dir.mkdir(parents=True)
        _write_manifest_pe_fixture(self.ctest)
        (self.compiler_bin / "g++.exe").write_bytes(b"compiler")
        (self.cmake_bin / "cmake.exe").write_bytes(b"cmake")
        self.cache_values = {
            "CMAKE_CXX_COMPILER": str(self.compiler_bin / "g++.exe"),
            "CMAKE_COMMAND": str(self.cmake_bin / "cmake.exe"),
            "CMAKE_CTEST_COMMAND": str(self.ctest),
            "Qt6_DIR": str(self.qt_root / "lib" / "cmake" / "Qt6"),
            "OLR_FFMPEG_ROOT": str(self.ffmpeg_root),
            "OLR_SRT_ROOT": str(self.srt_root),
            "CMAKE_HOME_DIRECTORY": str(self.root),
        }
        self.write_cache()

    def test_direct_script_bootstrap_imports_from_outside_repository(self) -> None:
        launcher = Path(__file__).resolve().parents[2] / "tools" / "run_ctest.py"
        completed = subprocess.run(
            [
                sys.executable,
                "-I",
                "-c",
                "import runpy; runpy.run_path(" + repr(str(launcher)) + ")",
            ],
            cwd=self.root,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(
            completed.returncode,
            0,
            msg=completed.stdout + completed.stderr,
        )

    def test_package_import_preserves_sys_path_and_single_tool_identities(self) -> None:
        project_root = Path(__file__).resolve().parents[2]
        probe = (
            "import sys\n"
            "before = tuple(sys.path)\n"
            "import tools.run_ctest\n"
            "assert tuple(sys.path) == before, (before, tuple(sys.path))\n"
            "assert 'windows_job' not in sys.modules\n"
            "assert 'windows_runtime_guard' not in sys.modules\n"
            "assert 'windows_runtime_cache' not in sys.modules\n"
            "assert 'tools.windows_job' in sys.modules\n"
            "assert 'tools.windows_runtime_guard' in sys.modules\n"
            "assert 'tools.windows_runtime_cache' in sys.modules\n"
        )
        completed = subprocess.run(
            [sys.executable, "-c", probe],
            cwd=project_root,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(
            completed.returncode,
            0,
            msg=completed.stdout + completed.stderr,
        )

    def write_cache(self) -> None:
        cache_types = {
            "CMAKE_CXX_COMPILER": "FILEPATH",
            "CMAKE_COMMAND": "INTERNAL",
            "CMAKE_CTEST_COMMAND": "INTERNAL",
            "Qt6_DIR": "PATH",
            "OLR_FFMPEG_ROOT": "PATH",
            "OLR_SRT_ROOT": "PATH",
            "CMAKE_HOME_DIRECTORY": "INTERNAL",
        }
        lines = [
            f"{key}:{cache_types.get(key, 'STRING')}={value}"
            for key, value in self.cache_values.items()
        ]
        (self.build_dir / "CMakeCache.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8"
        )

    def write_cacheable_ctest_graph(self, *names: str) -> tuple[Path, ...]:
        executables: list[Path] = []
        commands: list[str] = []
        for name in names:
            executable = self.build_dir / f"tst_{name}.exe"
            _write_manifest_pe_fixture(executable)
            executables.append(executable)
            commands.append(f'add_test([=[{name}]=] "{executable.as_posix()}")')
        (self.build_dir / "CTestTestfile.cmake").write_text(
            "\n".join(commands) + "\n", encoding="utf-8"
        )
        return tuple(executables)

    def test_real_build_finder_binds_cache_home_to_current_worktree(self) -> None:
        project = self.root / "finder-project"
        project.mkdir()

        def make_build(name: str, home: Path, timestamp: int) -> tuple[Path, Path]:
            build = project / name
            build.mkdir()
            app = build / "OpenLiveReplay.exe"
            app.write_bytes(name.encode())
            cache = build / "CMakeCache.txt"
            cache.write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={home.resolve()}\n",
                encoding="utf-8",
            )
            graph = build / "CTestTestfile.cmake"
            graph.write_text("", encoding="utf-8")
            for path in (app, cache, graph):
                os.utime(path, ns=(timestamp, timestamp))
            return build, app

        expected = make_build("build-current", project, 1_000_000_000)
        make_build("build-newer-wrong-home", self.root / "other-source", 2_000_000_000)

        self.assertEqual(find_real_windows_build(project), expected)

    def test_real_build_finder_rejects_only_wrong_worktree_candidates(self) -> None:
        project = self.root / "finder-wrong-only"
        build = project / "build-tests"
        build.mkdir(parents=True)
        (build / "OpenLiveReplay.exe").write_bytes(b"wrong")
        (build / "CTestTestfile.cmake").write_text("", encoding="utf-8")
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={(self.root / 'other').resolve()}\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(AssertionError, "current project source"):
            find_real_windows_build(project)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_ctest_warm_cache_and_every_executable_has_protected_qt_conf(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "ctest-warm-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        arguments = ("-R", "^unit$")

        with RuntimeAuditLease(cache_root) as cold:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=cold, ctest_args=arguments
            )
            self.assertGreater(cold.cache_miss_count, 0)

        with RuntimeAuditLease(cache_root) as warm:
            env, warm_ctest = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=warm, ctest_args=arguments
            )
            self.assertEqual(
                (
                    warm.cache_miss_count,
                    warm.payload_written_file_count,
                    warm.payload_written_byte_count,
                ),
                (0, 0, 0),
            )
            executable_paths = {
                executable
                for layout in warm.cached_layout_directories
                for executable in layout.rglob("*.exe")
            }
            executable_paths.add(warm_ctest)
            self.assertGreater(len(executable_paths), 1)
            for executable in executable_paths:
                with self.subTest(executable=executable):
                    rendered = (executable.parent / "qt.conf").read_text(
                        encoding="utf-8"
                    )
                    self.assertIn(Path(env["QT_PLUGIN_PATH"]).as_posix(), rendered)
                    self.assertIn(Path(env["QML_IMPORT_PATH"]).as_posix(), rendered)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_ctest_graph_uses_declared_provenance_and_source_change_misses(self) -> None:
        self.write_cacheable_ctest_graph("unit", "other")
        cache_root = self.root / "ctest-graph-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        arguments = ("-R", "^unit$")
        captured: list[windows_runtime_cache.GeneratedLayoutEntry] = []
        original = RuntimeAuditLease.stage_readonly_layout

        def record_entries(lease, name, entries, **kwargs):
            pairs = tuple(entries.items()) if isinstance(entries, dict) else tuple(entries)
            if name == "ctest-build":
                captured.extend(
                    entry
                    for _destination, entry in pairs
                    if isinstance(entry, windows_runtime_cache.GeneratedLayoutEntry)
                )
            return original(lease, name, pairs, **kwargs)

        with mock.patch.object(
            RuntimeAuditLease, "stage_readonly_layout", new=record_entries
        ):
            with RuntimeAuditLease(cache_root) as cold:
                run_ctest.prepare_windows_runtime(
                    self.build_dir, {}, audit_lease=cold, ctest_args=arguments
                )
            with RuntimeAuditLease(cache_root) as warm:
                run_ctest.prepare_windows_runtime(
                    self.build_dir, {}, audit_lease=warm, ctest_args=arguments
                )
                self.assertEqual(warm.cache_miss_count, 0)

        self.assertGreater(len(captured), 0)
        for generated in captured:
            self.assertEqual(generated.policy, "ctest-graph-rewrite-v2")
            self.assertEqual(generated.inputs["selected_test_names"], ("unit",))
            self.assertEqual(
                set(generated.inputs),
                {
                    "source_final_path",
                    "source_sha256",
                    "selected_test_names",
                    "replacement_paths",
                    "occurrence_replacements",
                    "working_overrides",
                },
            )

        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(graph.read_text(encoding="utf-8") + "# changed\n", encoding="utf-8")
        with RuntimeAuditLease(cache_root) as changed:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=changed, ctest_args=arguments
            )
            self.assertGreater(changed.cache_miss_count, 0)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_graph_rewrite_prepares_high_cardinality_path_table_once(self) -> None:
        (executable,) = self.write_cacheable_ctest_graph("unit")
        environment = ";".join(
            f"OLR_QT_BIN={(self.qt_root / 'bin').as_posix()}"
            for _index in range(128)
        )
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(
            f'add_test([=[unit]=] "{executable.as_posix()}")\n'
            f'set_tests_properties([=[unit]=] PROPERTIES ENVIRONMENT '
            f'{run_ctest._cmake_bracket(environment)})\n',
            encoding="utf-8",
        )
        cache_root = self.root / "prepared-replacements-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        prepared_sizes: list[int] = []
        original_prepare = run_ctest._prepare_path_replacements

        def record_prepare(replacements):
            materialized = tuple(replacements)
            prepared_sizes.append(len(materialized))
            return original_prepare(materialized)

        with (
            mock.patch.object(
                run_ctest,
                "_prepare_path_replacements",
                side_effect=record_prepare,
            ),
            RuntimeAuditLease(cache_root) as lease,
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )
            rewritten = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")

        self.assertEqual(sum(size > 8 for size in prepared_sizes), 1)
        self.assertNotIn((self.qt_root / "bin").as_posix(), rewritten)
        self.assertEqual(rewritten.count("OLR_QT_BIN="), 128)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_ctest_selection_and_source_snapshot_keys_are_stable_and_exact(self) -> None:
        executable, other = self.write_cacheable_ctest_graph("unit", "other")
        qml_input = self.root / "tests" / "cacheable-qml"
        qml_input.mkdir(parents=True)
        (qml_input / "tst_cacheable.qml").write_text(
            "import QtQuick\nItem {}\n", encoding="utf-8"
        )
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(
            f'add_test([=[unit]=] "{executable.as_posix()}" "-input" '
            f'"{qml_input.as_posix()}")\n'
            f'add_test([=[other]=] "{other.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_parent = Path(tempfile.mkdtemp(prefix="olr-ctest-source-cache-"))
        self.addCleanup(shutil.rmtree, cache_parent, True)
        cache_root = cache_parent / "cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as cold:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=cold,
                ctest_args=("-R", "^unit$"),
            )
            cold_build = cold.launch_build_directory
            cold_source = cold.launch_source_directory
        with RuntimeAuditLease(cache_root) as warm:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=warm,
                ctest_args=("-R", "^unit$"),
            )
            self.assertEqual(warm.cache_miss_count, 0)
            self.assertEqual(warm.payload_written_file_count, 0)
            self.assertEqual(warm.launch_build_directory, cold_build)
            self.assertEqual(warm.launch_source_directory, cold_source)
        with RuntimeAuditLease(cache_root) as changed_selection:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=changed_selection,
                ctest_args=("-R", "^other$"),
            )
            self.assertGreater(changed_selection.cache_miss_count, 0)
            self.assertNotEqual(changed_selection.launch_build_directory, cold_build)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_external_python_generated_closure_is_warm_cacheable_and_configured(self) -> None:
        base = self.root / "python-base"
        (base / "Lib").mkdir(parents=True)
        (base / "Lib" / "os.py").write_text("# fixture\n", encoding="utf-8")
        (base / "python313.dll").write_bytes(b"python-runtime")
        venv = self.root / "venv"
        scripts = venv / "Scripts"
        scripts.mkdir(parents=True)
        python = scripts / "python.exe"
        _write_manifest_pe_fixture(python)
        (venv / "pyvenv.cfg").write_text(
            f"home = {base.as_posix()}\n", encoding="utf-8"
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python]=] "{python.as_posix()}" "-c" "pass")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "external-python-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as cold:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=cold
            )
            replacement = self.root / "replacement-pyvenv.cfg"
            replacement.write_text("home = C:/poison\n", encoding="utf-8")
            with self.assertRaises(OSError):
                os.replace(replacement, venv / "pyvenv.cfg")
        with RuntimeAuditLease(cache_root) as warm:
            env, _ctest = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=warm
            )
            self.assertEqual(
                (
                    warm.cache_miss_count,
                    warm.payload_written_file_count,
                    warm.payload_written_byte_count,
                ),
                (0, 0, 0),
            )
            rewritten = (
                warm.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")
            staged_python = next(
                Path(value)
                for value in re.findall(r'"([^\"]+python\.exe)"', rewritten)
                if Path(value) != python
            )
            rendered = (staged_python.parent / "qt.conf").read_text(encoding="utf-8")
            self.assertIn(Path(env["QT_PLUGIN_PATH"]).as_posix(), rendered)
            self.assertIn(Path(env["QML_IMPORT_PATH"]).as_posix(), rendered)
            self.assertIn(
                (staged_python.parent.parent / "base").as_posix(),
                (staged_python.parent.parent / "pyvenv.cfg").read_text(
                    encoding="utf-8"
                ),
            )

    def make_junction(self, link: Path, target: Path) -> None:
        completed = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            self.skipTest(f"cannot create test junction: {completed.stderr}")

    def remove_test_runtime_cache(self, cache_root: Path) -> None:
        if not cache_root.exists():
            return
        windows_runtime_cache._set_tree_acl(
            cache_root, windows_runtime_guard._current_user_sid(), "DELETE"
        )
        shutil.rmtree(cache_root)

    def assert_directory_rejects_late_insertion(self, directory: Path) -> None:
        if directory.is_dir():
            with self.assertRaises(OSError, msg=str(directory)):
                (directory / f"late-task8-{time.time_ns()}.dll").write_bytes(
                    b"late"
                )
        else:
            with self.assertRaises(OSError, msg=str(directory)):
                directory.mkdir(parents=True)

    def real_build(self) -> tuple[Path, Path]:
        return find_real_windows_build(Path(__file__).resolve().parents[2])

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_real_source_aliases_are_rejected_before_process_creation(self) -> None:
        executable = self.build_dir / "tst_alias.exe"
        _write_manifest_pe_fixture(executable)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[control]=] "{executable.as_posix()}")\n',
            encoding="utf-8",
        )
        control_cache = self.root / "source-alias-control-cache"
        self.addCleanup(self.remove_test_runtime_cache, control_cache)
        with RuntimeAuditLease(control_cache) as lease:
            with mock.patch("tools.run_ctest.run_process_tree") as launch:
                run_ctest.prepare_windows_runtime(
                    self.build_dir, {}, audit_lease=lease
                )
            launch.assert_not_called()

        graph = self.build_dir / "CTestTestfile.cmake"
        graph_alias = self.root / "CTestTestfile-hardlink.cmake"
        os.link(graph, graph_alias)
        graph_cache = self.root / "graph-alias-rejection-cache"
        self.addCleanup(self.remove_test_runtime_cache, graph_cache)
        with (
            RuntimeAuditLease(graph_cache) as lease,
            self.assertRaisesRegex(RuntimeError, "alias|hard.?link"),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
        graph_alias.unlink()

        file_alias = self.build_dir / "tst_alias-hardlink.exe"
        os.link(executable, file_alias)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[hardlink-source]=] "{file_alias.as_posix()}")\n',
            encoding="utf-8",
        )
        alias_cache = self.root / "source-alias-rejection-cache"
        self.addCleanup(self.remove_test_runtime_cache, alias_cache)
        with (
            self.assertRaisesRegex(RuntimeError, "alias|hard.?link"),
            RuntimeAuditLease(alias_cache) as lease,
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )

        real_plugins = self.qt_root / "real-plugins"
        real_plugins.mkdir()
        plugin_alias = self.qt_root / "plugins" / "aliased"
        self.make_junction(plugin_alias, real_plugins)
        with (
            mock.patch("tools.run_ctest.run_process_tree") as launch,
            self.assertRaisesRegex(RuntimeError, "reparse alias"),
        ):
            run_ctest.prepare_windows_runtime(self.build_dir, {})
        launch.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "requires a built Windows app")
    def test_real_focused_prepare_has_absolute_cold_warm_budgets(self) -> None:
        build, _app = self.real_build()
        cache_parent = Path(tempfile.mkdtemp(prefix="olr-real-focused-cache-"))
        self.addCleanup(shutil.rmtree, cache_parent, True)
        cache_root = cache_parent / "cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        arguments = ("-R", "^tst_timecode$")

        with RuntimeAuditLease(cache_root) as cold:
            started = time.perf_counter()
            run_ctest.prepare_windows_runtime(
                build, {}, audit_lease=cold, ctest_args=arguments
            )
            cold_elapsed = time.perf_counter() - started
            self.assertLessEqual(cold_elapsed, 30.0)
            self.assertLessEqual(cold.staged_byte_count, 256 << 20)
            self.assertLessEqual(cold.staged_file_count, 4096)
            self.assertGreater(cold.cache_miss_count, 0)

        with RuntimeAuditLease(cache_root) as warm:
            started = time.perf_counter()
            run_ctest.prepare_windows_runtime(
                build, {}, audit_lease=warm, ctest_args=arguments
            )
            warm_elapsed = time.perf_counter() - started
            self.assertLessEqual(warm_elapsed, 5.0)
            self.assertGreater(warm.cache_hit_count, 0)
            self.assertEqual(
                (
                    warm.cache_miss_count,
                    warm.payload_written_file_count,
                    warm.payload_written_byte_count,
                ),
                (0, 0, 0),
            )

    @unittest.skipUnless(os.name == "nt", "requires a built Windows app")
    def test_real_full_prepare_has_absolute_cold_warm_budgets(self) -> None:
        build, _app = self.real_build()
        cache_parent = Path(tempfile.mkdtemp(prefix="olr-real-full-cache-"))
        self.addCleanup(shutil.rmtree, cache_parent, True)
        cache_root = cache_parent / "cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as cold:
            started = time.perf_counter()
            env, _ctest = run_ctest.prepare_windows_runtime(
                build, {}, audit_lease=cold, ctest_args=()
            )
            cold_elapsed = time.perf_counter() - started
            self.assertLessEqual(cold_elapsed, 120.0)
            self.assertLessEqual(cold.staged_byte_count, 2 << 30)
            self.assertLessEqual(cold.staged_file_count, 16384)
            self.assertGreater(cold.cache_miss_count, 0)

            graph_files = tuple(
                cold.launch_build_directory.rglob("CTestTestfile.cmake")
            )
            self.assertGreater(len(graph_files), 1)
            rewritten = "\n".join(
                graph.read_text(encoding="utf-8") for graph in graph_files
            )
            external_python = {
                Path(value).parent
                for value in re.findall(r'"([^\"]+python(?:w)?\.exe)"', rewritten, re.I)
            }
            external_qt_tools = {
                Path(value).parent
                for value in re.findall(r'"([^\"]+qmllint\.exe)"', rewritten, re.I)
            }
            self.assertTrue(external_python)
            self.assertTrue(external_qt_tools)

            plugins = Path(env["QT_PLUGIN_PATH"])
            qml = Path(env["QML_IMPORT_PATH"])
            qml_asset_directories = tuple(
                path for path in qml.rglob("*") if path.is_dir()
            )
            self.assertTrue(qml_asset_directories)
            mutation_namespaces = {
                plugins / "imageformats",
                plugins / "multimedia",
                qml_asset_directories[0],
                cold.launch_build_directory,
                cold.launch_runtime_directory,
                *cold.launch_runtime_directories,
                *(graph.parent for graph in graph_files),
                *external_python,
                *external_qt_tools,
            }
            for directory in mutation_namespaces:
                with self.subTest(namespace=directory):
                    self.assert_directory_rejects_late_insertion(directory)

        with RuntimeAuditLease(cache_root) as warm:
            started = time.perf_counter()
            run_ctest.prepare_windows_runtime(
                build, {}, audit_lease=warm, ctest_args=()
            )
            warm_elapsed = time.perf_counter() - started
            self.assertLessEqual(warm_elapsed, 8.0)
            self.assertGreater(warm.cache_hit_count, 0)
            self.assertEqual(
                (
                    warm.cache_miss_count,
                    warm.payload_written_file_count,
                    warm.payload_written_byte_count,
                ),
                (0, 0, 0),
            )

    def test_find_build_directory_accepts_split_test_dir(self):
        cwd = self.root / "working"
        actual = run_ctest.find_build_directory(
            ["-L", "unit", "--test-dir", "../build"], cwd
        )
        self.assertEqual(actual, (cwd / "../build").resolve())

    def test_find_build_directory_accepts_equals_test_dir(self):
        actual = run_ctest.find_build_directory(
            [f"--test-dir={self.build_dir}", "--output-on-failure"], self.root
        )
        self.assertEqual(actual, self.build_dir.resolve())

    def test_find_build_directory_uses_last_split_or_equals_occurrence(self):
        actual = run_ctest.find_build_directory(
            [
                "--test-dir",
                "first",
                "--test-dir=second",
                "--test-dir",
                "third",
            ],
            self.root,
        )

        self.assertEqual(actual, (self.root / "third").resolve())

    def test_find_build_directory_respects_official_option_value_arity(self):
        arguments = [
            "-R",
            "--test-dir",
            "-L",
            "unit",
            "--test-output-truncation",
            "--test-dir=not-an-option.txt",
        ]

        self.assertEqual(
            run_ctest.find_build_directory(arguments, self.root),
            self.root.resolve(),
        )

    def test_find_build_directory_does_not_reinterpret_repeat_value_as_test_dir(self):
        arguments = [
            "--repeat-until-fail",
            "--test-dir",
            "-L",
            "unit",
        ]

        self.assertEqual(
            run_ctest.find_build_directory(arguments, self.root),
            self.root.resolve(),
        )

    def test_find_build_directory_rejects_missing_values(self):
        for args in (["--test-dir"], ["--test-dir="]):
            with self.subTest(args=args):
                with self.assertRaisesRegex(ValueError, "requires a build directory"):
                    run_ctest.find_build_directory(args, self.root)

    def test_find_build_directory_defaults_to_working_directory(self):
        self.assertEqual(run_ctest.find_build_directory(["-L", "unit"], self.root), self.root)

    def test_read_cmake_cache_parses_typed_values_without_executing_them(self):
        cache_path = self.build_dir / "parser-cache.txt"
        cache_path.write_text(
            "# generated cache\n"
            "// documentation\n"
            "CMAKE_COMMAND:INTERNAL=C:/Program Files/CMake/bin/cmake.exe\n"
            "OLR_FFMPEG_ROOT:PATH=C:/deps/ffmpeg=controlled\n"
            "MALFORMED_LINE\n",
            encoding="utf-8",
        )

        self.assertEqual(
            run_ctest.read_cmake_cache(cache_path),
            {
                "CMAKE_COMMAND": "C:/Program Files/CMake/bin/cmake.exe",
                "OLR_FFMPEG_ROOT": "C:/deps/ffmpeg=controlled",
            },
        )

    @unittest.skipUnless(os.name == "nt", "requires a real PE executable")
    def test_pe_import_parser_reads_real_windows_executable(self):
        self.assertIn("kernel32.dll", pe_imports(Path(sys.executable)))

    def test_pe_import_parser_reads_delay_import_va_form_for_pe32_and_pe32_plus(self):
        for magic in (0x10B, 0x20B):
            with self.subTest(magic=hex(magic)):
                executable = self.build_dir / f"delay-{magic:x}.exe"
                _write_pe_fixture(executable, magic=magic, attributes=0)
                self.assertEqual(pe_imports(executable), {"fixture-runtime.dll"})

    def test_pe_import_parser_rejects_delay_va_below_image_base(self):
        executable = self.build_dir / "delay-invalid-va.exe"
        _write_pe_fixture(executable, attributes=0)
        contents = bytearray(executable.read_bytes())
        struct.pack_into("<I", contents, 0x300 + 4, 1)
        executable.write_bytes(contents)
        with self.assertRaisesRegex(ValueError, "delay-import.*image base"):
            pe_imports(executable)

    def test_pe_import_parser_rejects_virtual_tail_overlay_rva(self):
        executable = self.build_dir / "overlay-rva.exe"
        _write_pe_fixture(
            executable,
            directory_index=1,
            directory_rva=0x1300,
            name_rva=0x1350,
            raw_size=0x200,
            overlay_size=0x200,
        )
        with self.assertRaisesRegex(ValueError, "raw section data"):
            pe_imports(executable)

    def test_pe_import_parser_bounds_data_directories_to_optional_header(self):
        executable = self.build_dir / "short-optional.exe"
        _write_pe_fixture(
            executable,
            directory_index=1,
            optional_size=112,
        )
        with self.assertRaisesRegex(ValueError, "data directories.*optional header"):
            pe_imports(executable)

    def test_pe_import_parser_requires_terminator_inside_declared_directory(self):
        executable = self.build_dir / "unterminated-imports.exe"
        _write_pe_fixture(
            executable,
            directory_index=1,
            directory_size=20,
        )
        with self.assertRaisesRegex(ValueError, "import descriptor.*declared size"):
            pe_imports(executable)

    def test_pe_import_parser_bounds_delay_descriptors_to_declared_size(self):
        executable = self.build_dir / "unterminated-delay-imports.exe"
        _write_pe_fixture(executable, directory_size=32)
        with self.assertRaisesRegex(
            ValueError,
            "delay-import descriptor.*declared size",
        ):
            pe_imports(executable)

    def test_pe_import_parser_rejects_unknown_delay_import_attributes(self):
        for attributes in (2, 3):
            with self.subTest(attributes=attributes):
                executable = self.build_dir / f"delay-attributes-{attributes}.exe"
                _write_pe_fixture(executable, attributes=attributes)
                with self.assertRaisesRegex(
                    ValueError, "delay-import descriptor attributes"
                ):
                    pe_imports(executable)

    @unittest.skipUnless(os.name == "nt", "requires Windows PE resource APIs")
    def test_embedded_application_manifest_accepts_only_rt_manifest_id_one(self):
        valid = self.build_dir / "manifest-valid.exe"
        wrong_type = self.build_dir / "manifest-wrong-type.exe"
        wrong_id = self.build_dir / "manifest-wrong-id.exe"
        external_only = self.build_dir / "manifest-external-only.exe"
        _write_manifest_pe_fixture(valid)
        _write_manifest_pe_fixture(wrong_type, resource_type=23)
        _write_manifest_pe_fixture(wrong_id, resource_id=2)
        _write_pe_fixture(external_only)
        Path(f"{external_only}.manifest").write_text(
            "<assembly manifestVersion='1.0'/>", encoding="utf-8"
        )

        self.assertTrue(
            windows_runtime_guard.has_embedded_application_manifest(valid)
        )
        self.assertFalse(
            windows_runtime_guard.has_embedded_application_manifest(wrong_type)
        )
        self.assertFalse(
            windows_runtime_guard.has_embedded_application_manifest(wrong_id)
        )
        self.assertFalse(
            windows_runtime_guard.has_embedded_application_manifest(external_only)
        )

    @unittest.skipUnless(os.name == "nt", "requires Windows PE resource APIs")
    def test_embedded_application_manifest_rejects_malformed_resource_or_xml(self):
        malformed_resource = self.build_dir / "manifest-bad-resource.exe"
        malformed_xml = self.build_dir / "manifest-bad-xml.exe"
        _write_manifest_pe_fixture(malformed_resource, malformed_offset=True)
        _write_manifest_pe_fixture(malformed_xml, manifest=b"not xml")

        with self.assertRaisesRegex(ValueError, "PE resource"):
            windows_runtime_guard.has_embedded_application_manifest(
                malformed_resource
            )
        with self.assertRaisesRegex(ValueError, "application manifest XML"):
            windows_runtime_guard.has_embedded_application_manifest(malformed_xml)

    @unittest.skipUnless(os.name == "nt", "requires Windows registry views")
    def test_dotlocal_override_registry_check_is_fail_closed(self):
        import winreg

        key = mock.MagicMock()
        key.__enter__.return_value = key
        cases = (
            ([(1, winreg.REG_DWORD)], "enabled"),
            ([(0, winreg.REG_SZ)], "type"),
            ([PermissionError("denied")], "cannot read"),
        )
        for query_results, expected in cases:
            with self.subTest(expected=expected), mock.patch.object(
                winreg, "OpenKey", return_value=key
            ), mock.patch.object(
                winreg, "QueryValueEx", side_effect=query_results
            ), self.assertRaisesRegex(RuntimeError, expected):
                windows_runtime_guard.reject_dotlocal_dev_override()

    @unittest.skipUnless(os.name == "nt", "requires Windows registry views")
    def test_dotlocal_override_registry_check_accepts_absent_or_zero_views(self):
        import winreg

        key = mock.MagicMock()
        key.__enter__.return_value = key
        with mock.patch.object(
            winreg, "OpenKey", return_value=key
        ) as open_key, mock.patch.object(
            winreg,
            "QueryValueEx",
            side_effect=(FileNotFoundError(), (0, winreg.REG_DWORD)),
        ):
            windows_runtime_guard.reject_dotlocal_dev_override()

        accesses = [call.kwargs["access"] for call in open_key.call_args_list]
        self.assertEqual(accesses[0] & winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_64KEY)
        self.assertEqual(accesses[1] & winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_32KEY)

    def test_registered_test_executables_resolve_relative_to_testfile(self):
        nested = self.build_dir / "nested"
        nested.mkdir()
        executable = nested / "tst_relative.exe"
        executable.write_bytes(b"pe-fixture")
        (nested / "CTestTestfile.cmake").write_text(
            'add_test([=[relative]=] "tst_relative.exe")\n',
            encoding="utf-8",
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    def test_registered_test_executables_include_wrapped_executable_arguments(self):
        nested = self.build_dir / "nested"
        nested.mkdir()
        test_executable = nested / "tst_wrapped.exe"
        helper_executable = nested / "OlrProbe.exe"
        bracket_executable = nested / "OlrBracket.exe"
        test_executable.write_bytes(b"pe-fixture")
        helper_executable.write_bytes(b"pe-fixture")
        bracket_executable.write_bytes(b"pe-fixture")
        (nested / "CTestTestfile.cmake").write_text(
            'add_test([=[bash-wrapped]=] "C:/Program Files/Git/bin/bash.exe" '
            '"runner.sh" "tst_wrapped.exe" "OlrProbe.exe")\n'
            'add_test([=[python-wrapped]=] "C:/Python/python.exe" '
            '"runner.py" "tst_wrapped.exe")\n'
            'add_test(NAME bracket-wrapped COMMAND [=[C:/Python/python.exe]=]\n'
            '  runner.py [=[OlrBracket.exe]=])\n',
            encoding="utf-8",
        )

        expected = {bracket_executable, helper_executable, test_executable}
        for external in (
            Path("C:/Program Files/Git/bin/bash.exe"),
            Path("C:/Python/python.exe"),
        ):
            if external.is_file():
                expected.add(external.resolve())
        self.assertEqual(
            registered_test_executables(self.build_dir),
            tuple(sorted(expected, key=lambda path: str(path).casefold())),
        )

    def test_registered_test_executables_preserve_literal_equals_in_paths(self):
        nested = self.build_dir / "nested=configuration"
        nested.mkdir()
        executable = nested / "tst_value=expected.exe"
        executable.write_bytes(b"pe-fixture")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[equals]=] "nested=configuration/tst_value=expected.exe")\n',
            encoding="utf-8",
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    def test_registered_test_executables_extract_option_assignment_paths(self):
        nested = self.build_dir / "nested"
        nested.mkdir()
        executable = nested / "tst_option.exe"
        executable.write_bytes(b"pe-fixture")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[option]=] "runner" '
            '"--binary=nested/tst_option.exe")\n',
            encoding="utf-8",
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    def test_registered_test_executables_honor_working_directory(self):
        working = self.build_dir / "generated" / "working"
        working.mkdir(parents=True)
        executable = working / "tst_working.exe"
        executable.write_bytes(b"pe-fixture")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[working]=] "tst_working.exe")\n'
            'set_tests_properties([=[working]=] PROPERTIES '
            'WORKING_DIRECTORY [=[generated/working]=])\n',
            encoding="utf-8",
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    def test_registered_test_executables_honor_set_property_working_directory(self):
        working = self.build_dir / "property" / "working"
        working.mkdir(parents=True)
        executable = working / "tst_property_working.exe"
        executable.write_bytes(b"pe-fixture")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test(NAME property-working COMMAND "tst_property_working.exe")\n'
            'set_property(TEST [=[property-working]=] PROPERTY '
            'WORKING_DIRECTORY [=[property/working]=])\n',
            encoding="utf-8",
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_registered_test_executables_fail_closed_on_build_lexical_escape(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "tst_escaped.exe").write_bytes(b"pe-fixture")
        junction = self.build_dir / "junction"
        self.make_junction(junction, outside)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[escaped]=] "junction/tst_escaped.exe")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "resolves outside"):
            registered_test_executables(self.build_dir)

    def test_ctest_graph_rejects_subdirectory_outside_build(self) -> None:
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "CTestTestfile.cmake").write_text("", encoding="utf-8")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'subdirs("../outside")\n', encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "CTest.*outside"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_graph_follows_literal_include(self) -> None:
        executable = self.build_dir / "tst_included.exe"
        executable.write_bytes(b"pe-fixture")
        included = self.build_dir / "generated-tests.cmake"
        included.write_text(
            'add_test([=[included]=] "tst_included.exe")\n', encoding="utf-8"
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'include("generated-tests.cmake")\n', encoding="utf-8"
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    def test_ctest_graph_follows_add_subdirectory(self) -> None:
        child = self.build_dir / "child"
        child.mkdir()
        (child / "CTestTestfile.cmake").write_text(
            'add_test([=[child]=] "runner")\n'
            'set_tests_properties([=[child]=] PROPERTIES '
            'ENVIRONMENT "PATH=C:/poison")\n',
            encoding="utf-8",
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_subdirectory("child")\n', encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT.*PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_graph_rejects_missing_legacy_subdirs_edge(self) -> None:
        missing = self.build_dir / "generated-later"
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'subdirs("generated-later")\n', encoding="utf-8"
        )

        with RuntimeAuditLease() as lease:
            with self.assertRaisesRegex(RuntimeError, "subdirs.*does not exist"):
                run_ctest.prepare_windows_runtime(
                    self.build_dir, {}, audit_lease=lease
                )
            missing.mkdir()
            (missing / "CTestTestfile.cmake").write_text(
                'add_test([=[late]=] "runner")\n', encoding="utf-8"
            )
            self.assertIsNone(lease.launch_build_directory)

    def test_ctest_graph_rejects_discover_tests_command(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'discover_tests("dynamic")\n', encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "discover_tests"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_include_properties_follow_execution_order(self) -> None:
        included = self.build_dir / "included-properties.cmake"
        included.write_text(
            'set_tests_properties([=[ordered]=] PROPERTIES '
            'ENVIRONMENT "PATH=C:/poison")\n',
            encoding="utf-8",
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[ordered]=] "runner")\n'
            'include("included-properties.cmake")\n'
            'set_tests_properties([=[ordered]=] PROPERTIES '
            'ENVIRONMENT "OLR_SAFE=1")\n',
            encoding="utf-8",
        )

        run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_include_cannot_hide_later_unsafe_property(self) -> None:
        included = self.build_dir / "included-safe-properties.cmake"
        included.write_text(
            'set_tests_properties([=[ordered]=] PROPERTIES '
            'ENVIRONMENT "OLR_SAFE=1")\n',
            encoding="utf-8",
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[ordered]=] "runner")\n'
            'include("included-safe-properties.cmake")\n'
            'set_tests_properties([=[ordered]=] PROPERTIES '
            'ENVIRONMENT "PATH=C:/poison")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT.*PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_include_preserves_callers_directory_context(self) -> None:
        executable = self.build_dir / "tst_included_context.exe"
        executable.write_bytes(b"pe-fixture")
        generated = self.build_dir / "config"
        generated.mkdir()
        (generated / "generated-tests.cmake").write_text(
            'add_test([=[included-context]=] "tst_included_context.exe")\n',
            encoding="utf-8",
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'include("config/generated-tests.cmake")\n', encoding="utf-8"
        )

        self.assertEqual(registered_test_executables(self.build_dir), (executable,))

    def test_ctest_graph_rejects_dynamic_edges(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'subdirs("${DYNAMIC_TEST_DIR}")\n', encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "dynamic.*CTest|CTest.*dynamic"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_graph_ignores_commented_edges(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            '# subdirs("../outside")\n# include("missing.cmake")\n',
            encoding="utf-8",
        )

        run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_graph_ignores_edge_syntax_inside_arguments(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[literal]=] "runner" "subdirs(../outside)")\n',
            encoding="utf-8",
        )

        run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_cmake_command_set_is_masked_and_lexically_scanned_once(self) -> None:
        command_names = ("add_test", "set_tests_properties", "subdirs")
        lines = [
            "# add_test(commented)",
            '"add_test(quoted)"',
            "[=[subdirs(bracketed)]=]",
            "myadd_test(not-an-identifier-match)",
        ]
        expected_names: list[str] = []
        for index in range(128):
            if index % 2:
                lines.append(
                    f"SeT_TeStS_PrOpErTiEs(test-{index:03d} PROPERTIES "
                    "WORKING_DIRECTORY work)"
                )
                expected_names.append("set_tests_properties")
            else:
                lines.append(f"AdD_TeSt(test-{index:03d} tst.exe)")
                expected_names.append("add_test")
        text = "\n".join(lines) + "\n"
        mask_calls = 0
        scan_calls = 0
        original_mask = windows_runtime_guard._strip_cmake_comments
        original_scan = windows_runtime_guard._scan_cmake_command_starts

        def count_mask(value: str) -> str:
            nonlocal mask_calls
            mask_calls += 1
            return original_mask(value)

        def count_scan(value: str, names: tuple[str, ...]):
            nonlocal scan_calls
            scan_calls += 1
            return original_scan(value, names)

        with (
            mock.patch.object(
                windows_runtime_guard,
                "_strip_cmake_comments",
                side_effect=count_mask,
            ),
            mock.patch.object(
                windows_runtime_guard,
                "_scan_cmake_command_starts",
                side_effect=count_scan,
            ),
        ):
            commands = windows_runtime_guard._cmake_commands_in_order(
                text, command_names
            )

        self.assertEqual(mask_calls, 1)
        self.assertEqual(scan_calls, 1)
        self.assertEqual(
            [name for name, _arguments, _occurrences in commands], expected_names
        )
        self.assertEqual(
            [arguments[0] for _name, arguments, _occurrences in commands],
            [f"test-{index:03d}" for index in range(128)],
        )

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_ctest_graph_rejects_reparse_alias_edges(self) -> None:
        real = self.build_dir / "real-tests"
        real.mkdir()
        (real / "CTestTestfile.cmake").write_text("", encoding="utf-8")
        alias = self.build_dir / "test-alias"
        self.make_junction(alias, real)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'subdirs("test-alias")\n', encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "CTest graph.*reparse|alias"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_graph_files_are_leased_until_child_returns(self) -> None:
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text("", encoding="utf-8")
        replacement = self.build_dir / "replacement-CTestTestfile.cmake"
        replacement.write_text("# changed\n", encoding="utf-8")

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            with self.assertRaises(OSError):
                os.replace(replacement, graph)

        os.replace(replacement, graph)

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_included_ctest_graph_file_is_leased(self) -> None:
        included = self.build_dir / "included.cmake"
        included.write_text("", encoding="utf-8")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'include("included.cmake")\n', encoding="utf-8"
        )
        replacement = self.build_dir / "replacement-included.cmake"
        replacement.write_text("# changed\n", encoding="utf-8")

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(self.build_dir, {}, audit_lease=lease)
            with self.assertRaises(OSError):
                os.replace(replacement, included)

    def test_ctest_working_directory_is_part_of_dll_audit(self) -> None:
        executable = self.build_dir / "tst_workdir.exe"
        _write_manifest_pe_fixture(executable)
        workdir = self.build_dir / "runtime-workdir"
        workdir.mkdir()
        (self.qt_root / "bin" / "Qt6Core.dll").write_bytes(b"configured")
        (workdir / "Qt6Core.dll").write_bytes(b"stale")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[workdir]=] "tst_workdir.exe")\n'
            'set_tests_properties([=[workdir]=] PROPERTIES '
            'WORKING_DIRECTORY [=[runtime-workdir]=])\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "runtime-workdir.*Qt6Core"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_rejects_working_directory_outside_build(self) -> None:
        outside = self.root / "outside-workdir"
        outside.mkdir()
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[outside-workdir]=] "runner")\n'
            f'set_tests_properties([=[outside-workdir]=] PROPERTIES '
            f'WORKING_DIRECTORY "{outside.as_posix()}")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "WORKING_DIRECTORY.*outside"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_working_directory_runtime_dll_is_leased(self) -> None:
        executable = self.build_dir / "tst_workdir.exe"
        _write_manifest_pe_fixture(executable)
        workdir = self.build_dir / "runtime-workdir"
        workdir.mkdir()
        configured = self.qt_root / "bin" / "Qt6Core.dll"
        configured.write_bytes(b"configured")
        adjacent = workdir / "Qt6Core.dll"
        adjacent.write_bytes(configured.read_bytes())
        replacement = workdir / "replacement.dll"
        replacement.write_bytes(b"replacement")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[workdir]=] "{executable.as_posix()}")\n'
            'set_tests_properties([=[workdir]=] PROPERTIES '
            'WORKING_DIRECTORY [=[runtime-workdir]=])\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(self.build_dir, {}, audit_lease=lease)
            with self.assertRaises(OSError):
                os.replace(replacement, adjacent)

    def test_ctest_rejects_path_reset_or_replace_modifications(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[bad-env]=] "runner")\n'
            'set_tests_properties([=[bad-env]=] PROPERTIES '
            'ENVIRONMENT_MODIFICATION "PATH=reset:;PATH=set:C:/poison")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT_MODIFICATION.*PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_rejects_incomplete_path_prepend_sequence(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[reorder]=] "runner")\n'
            'set_tests_properties([=[reorder]=] PROPERTIES '
            f'ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:{(self.qt_root / "bin").as_posix()}")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "PATH.*order|sequence"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_properties_are_scoped_to_their_test_directory(self) -> None:
        safe = self.build_dir / "safe"
        unsafe = self.build_dir / "unsafe"
        safe.mkdir()
        unsafe.mkdir()
        (safe / "CTestTestfile.cmake").write_text(
            'add_test([=[duplicate]=] "runner")\n'
            'set_tests_properties([=[duplicate]=] PROPERTIES '
            'ENVIRONMENT "OLR_SAFE=1")\n',
            encoding="utf-8",
        )
        (unsafe / "CTestTestfile.cmake").write_text(
            'add_test([=[duplicate]=] "runner")\n'
            'set_tests_properties([=[duplicate]=] PROPERTIES '
            'ENVIRONMENT "PATH=C:/poison")\n',
            encoding="utf-8",
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'subdirs("safe")\nsubdirs("unsafe")\n', encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT.*PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_test_names_remain_case_sensitive(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[CaseSensitive]=] "runner")\n'
            'set_tests_properties([=[CaseSensitive]=] PROPERTIES '
            'ENVIRONMENT "PATH=C:/poison")\n'
            'add_test([=[casesensitive]=] "runner")\n'
            'set_tests_properties([=[casesensitive]=] PROPERTIES '
            'ENVIRONMENT "OLR_SAFE=1")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT.*PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_rejects_uncontrolled_qt_environment_paths(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[bad-qt]=] "runner")\n'
            'set_tests_properties([=[bad-qt]=] PROPERTIES '
            'ENVIRONMENT "QT_PLUGIN_PATH=C:/poison;QT_QPA_PLATFORM=xcb")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "QT_PLUGIN_PATH|QT_QPA_PLATFORM"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_rejects_nonexistent_qml_import_addition(self) -> None:
        missing = (self.build_dir / "missing-qml").as_posix()
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[bad-qml]=] "runner")\n'
            'set_tests_properties([=[bad-qml]=] PROPERTIES '
            f'ENVIRONMENT "QML_IMPORT_PATH={missing}")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "QML_IMPORT_PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_rejects_additional_qml_import_root(self) -> None:
        qml = Path(tempfile.mkdtemp(prefix="olr-uncontrolled-qml-"))
        self.addCleanup(shutil.rmtree, qml, True)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[qml]=] "runner")\n'
            'set_tests_properties([=[qml]=] PROPERTIES '
            f'ENVIRONMENT "QML_IMPORT_PATH={qml.as_posix()}")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "QML_IMPORT_PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_ctest_models_every_set_property_environment_value(self) -> None:
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[multi-env]=] "runner")\n'
            'set_property(TEST [=[multi-env]=] PROPERTY ENVIRONMENT '
            '"OLR_ALLOWED=1" "PATH=C:/poison")\n',
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT.*PATH"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_cache_rejects_duplicate_required_entry(self) -> None:
        with (self.build_dir / "CMakeCache.txt").open("a", encoding="utf-8") as cache:
            cache.write(f"Qt6_DIR:PATH={self.root / 'other-qt'}\n")

        with self.assertRaises(Exception) as raised:
            run_ctest.prepare_windows_runtime(self.build_dir, {})
        self.assertIsInstance(raised.exception, ValueError)
        self.assertRegex(str(raised.exception), "duplicate.*Qt6_DIR")

    def test_cache_rejects_wrong_type_and_relative_root(self) -> None:
        cache = self.build_dir / "CMakeCache.txt"
        contents = cache.read_text(encoding="utf-8")
        cache.write_text(
            contents.replace("Qt6_DIR:PATH=", "Qt6_DIR:STRING=").replace(
                f"OLR_FFMPEG_ROOT:PATH={self.ffmpeg_root}",
                "OLR_FFMPEG_ROOT:PATH=relative/ffmpeg",
            ),
            encoding="utf-8",
        )

        with self.assertRaises(Exception) as raised:
            run_ctest.prepare_windows_runtime(self.build_dir, {})
        self.assertIsInstance(raised.exception, ValueError)
        self.assertRegex(str(raised.exception), "Qt6_DIR.*type|relative")

    def test_runtime_uses_canonical_cache_paths(self) -> None:
        with mock.patch.object(Path, "resolve", autospec=True, wraps=Path.resolve) as resolve:
            env, ctest = run_ctest.prepare_windows_runtime(self.build_dir, {})

        self.assertEqual(ctest, self.ctest.resolve())
        self.assertEqual(env["QT_PLUGIN_PATH"], str((self.qt_root / "plugins").resolve()))
        self.assertGreater(resolve.call_count, 0)

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_ctest_junction_retarget_cannot_change_selected_binary(self) -> None:
        link = self.root / "cmake-link"
        self.make_junction(link, self.cmake_bin)
        self.cache_values["CMAKE_COMMAND"] = str(link / "cmake.exe")
        self.cache_values["CMAKE_CTEST_COMMAND"] = str(link / "ctest.exe")
        self.write_cache()

        _env, selected = run_ctest.prepare_windows_runtime(self.build_dir, {})
        os.rmdir(link)
        poison = self.root / "poison-cmake"
        poison.mkdir()
        _write_manifest_pe_fixture(poison / "ctest.exe")
        self.make_junction(link, poison)

        self.assertEqual(selected, self.ctest.resolve())

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_runtime_junction_retarget_cannot_change_path_identity(self) -> None:
        link = self.root / "qt-link"
        self.make_junction(link, self.qt_root)
        self.cache_values["Qt6_DIR"] = str(link / "lib" / "cmake" / "Qt6")
        self.write_cache()

        env, _selected = run_ctest.prepare_windows_runtime(self.build_dir, {})
        os.rmdir(link)
        poison = self.root / "poison-qt"
        for child in ("bin", "plugins", "qml"):
            (poison / child).mkdir(parents=True, exist_ok=True)
        self.make_junction(link, poison)

        self.assertIn(str((self.qt_root / "bin").resolve()), env["PATH"])
        self.assertNotIn(str(link), env["PATH"])

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_plugin_junction_retarget_cannot_change_plugin_identity(self) -> None:
        link = self.root / "qt-plugin-link"
        self.make_junction(link, self.qt_root)
        self.cache_values["Qt6_DIR"] = str(link / "lib" / "cmake" / "Qt6")
        self.write_cache()

        env, _selected = run_ctest.prepare_windows_runtime(self.build_dir, {})
        os.rmdir(link)
        poison = self.root / "poison-plugins"
        for child in ("bin", "plugins", "qml"):
            (poison / child).mkdir(parents=True, exist_ok=True)
        self.make_junction(link, poison)

        self.assertEqual(
            env["QT_PLUGIN_PATH"], str((self.qt_root / "plugins").resolve())
        )

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_plugin_subtree_rejects_junction_to_external_dlls(self) -> None:
        outside = self.root / "outside-plugin"
        outside.mkdir()
        (outside / "qexternal.dll").write_bytes(b"external")
        self.make_junction(self.qt_root / "plugins" / "external", outside)

        with self.assertRaisesRegex(RuntimeError, "DLL subtree.*outside|reparse"):
            run_ctest.prepare_windows_runtime(self.build_dir, {})

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_qt_plugin_dll_subtree_is_leased(self) -> None:
        plugin = self.qt_root / "plugins" / "imageformats" / "qjpeg.dll"
        plugin.parent.mkdir()
        plugin.write_bytes(b"plugin")
        replacement = plugin.with_name("replacement.dll")
        replacement.write_bytes(b"replacement")
        qml_dll = self.qt_root / "qml" / "QtQuick" / "quickplugin.dll"
        qml_dll.parent.mkdir()
        qml_dll.write_bytes(b"qml-plugin")
        qml_replacement = qml_dll.with_name("replacement.dll")
        qml_replacement.write_bytes(b"replacement")

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(self.build_dir, {}, audit_lease=lease)
            with self.assertRaises(OSError):
                os.replace(replacement, plugin)
            with self.assertRaises(OSError):
                os.replace(qml_replacement, qml_dll)

    def test_transitive_runtime_closure_selects_only_imported_dlls(self) -> None:
        runtime = self.qt_root / "bin"
        seed = self.build_dir / "seed.exe"
        seed.write_bytes(b"seed")
        core = runtime / "Qt6Core.dll"
        gui = runtime / "Qt6Gui.dll"
        core.write_bytes(b"core")
        gui.write_bytes(b"gui")
        for index in range(79):
            with (runtime / f"Qt6Unused{index:02d}.dll").open("wb") as stream:
                stream.truncate(2 << 20)
        imports = {
            seed.resolve(): {core.name},
            core.resolve(): {gui.name},
            gui.resolve(): set(),
        }
        with mock.patch.object(
            windows_runtime_guard,
            "pe_imports",
            side_effect=lambda path: imports.get(path.resolve(), set()),
        ):
            selected = windows_runtime_guard.transitive_runtime_closure(
                (seed,), (runtime,)
            )
        self.assertEqual(set(selected), {core.resolve(), gui.resolve()})
        self.assertLess(sum(path.stat().st_size for path in selected), 1 << 20)

    def test_qt_plugin_and_qml_closures_are_explicit_and_transitive(self) -> None:
        plugins = self.qt_root / "plugins"
        offscreen = plugins / "platforms" / "qoffscreen.dll"
        jpeg = plugins / "imageformats" / "qjpeg.dll"
        designer = plugins / "designer" / "qdesigner.dll"
        for path in (offscreen, jpeg, designer):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(path.name.encode())
        with RuntimeAuditLease() as lease:
            plugin_files = windows_runtime_guard.qt_plugin_closure(
                plugins, lease=lease
            )
        self.assertIn(offscreen.resolve(), plugin_files)
        self.assertIn(jpeg.resolve(), plugin_files)
        self.assertNotIn(designer.resolve(), plugin_files)

        source = self.root / "qml-source"
        source.mkdir()
        (source / "Main.qml").write_text(
            "import QtQuick.Controls\nItem {}\n", encoding="utf-8"
        )
        controls = self.qt_root / "qml" / "QtQuick" / "Controls"
        controls.mkdir(parents=True)
        (controls / "qmldir").write_text("depends QtQml\n", encoding="utf-8")
        (controls / "Control.qml").write_text("import QtQml\n", encoding="utf-8")
        qtqml = self.qt_root / "qml" / "QtQml"
        qtqml.mkdir()
        (qtqml / "qmldir").write_text("module QtQml\n", encoding="utf-8")
        unused = self.qt_root / "qml" / "QtUnused"
        unused.mkdir()
        for index in range(676):
            (unused / f"Unused{index:03d}.qml").write_bytes(b"unused")

        with RuntimeAuditLease() as lease:
            qml_files = windows_runtime_guard.qt_qml_closure(
                self.qt_root / "qml", (source,), lease=lease
            )
        self.assertIn((controls / "qmldir").resolve(), qml_files)
        self.assertIn((qtqml / "qmldir").resolve(), qml_files)
        self.assertFalse(any(path.is_relative_to(unused) for path in qml_files))
        self.assertLess(len(qml_files), 8)

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_plugin_closure_holds_family_against_junction_retarget(self) -> None:
        plugins = self.root / "plugin-race"
        family = plugins / "imageformats"
        family.mkdir(parents=True)
        original_family = plugins / "imageformats-original"
        external = self.root / "plugin-race-external"
        external.mkdir()
        (external / "qpoison.dll").write_bytes(b"poison")

        with RuntimeAuditLease() as lease:
            lease.hold_dll_tree(plugins)
            family.rename(original_family)
            self.make_junction(family, external)
            with self.assertRaisesRegex(RuntimeError, "reparse alias"):
                windows_runtime_guard.qt_plugin_closure(
                    plugins,
                    ("imageformats",),
                    platforms=(),
                    lease=lease,
                )

    def test_qml_closure_keeps_module_assets_but_not_optional_imports(self) -> None:
        source = self.root / "qml-assets-source"
        source.mkdir()
        (source / "Main.qml").write_text(
            "import Qt.AssetRoot\nItem {}\n", encoding="utf-8"
        )
        qml_root = self.qt_root / "qml"
        module = qml_root / "Qt" / "AssetRoot"
        assets = module / "assets"
        assets.mkdir(parents=True)
        (module / "qmldir").write_text(
            "module Qt.AssetRoot\n"
            "depends Qt.Required 1.0\n"
            "optional import Qt.Optional 1.0\n",
            encoding="utf-8",
        )
        (module / "Root.qml").write_text("Item {}\n", encoding="utf-8")
        image = assets / "control.png"
        font = assets / "control.ttf"
        shader = assets / "control.qsb"
        image.write_bytes(b"png")
        font.write_bytes(b"font")
        shader.write_bytes(b"shader")
        required = qml_root / "Qt" / "Required"
        required.mkdir(parents=True)
        (required / "Required.qml").write_text("Item {}\n", encoding="utf-8")
        optional = qml_root / "Qt" / "Optional"
        optional.mkdir(parents=True)
        optional_file = optional / "Optional.qml"
        optional_file.write_text("Item {}\n", encoding="utf-8")

        with RuntimeAuditLease() as lease:
            selected = windows_runtime_guard.qt_qml_closure(
                qml_root, (source,), lease=lease
            )

        for asset in (image, font, shader):
            self.assertIn(asset.resolve(), selected)
        self.assertIn((required / "Required.qml").resolve(), selected)
        self.assertNotIn(optional_file.resolve(), selected)

    def test_qml_closure_parses_qt_modules_from_restored_held_streams(self) -> None:
        source = self.root / "qml-held-source"
        source.mkdir()
        (source / "Main.qml").write_text(
            "import Qt.HeldRoot\nItem {}\n", encoding="utf-8"
        )
        qml_root = self.qt_root / "qml"
        module = qml_root / "Qt" / "HeldRoot"
        module.mkdir(parents=True)
        qmldir = module / "qmldir"
        qmldir.write_text("depends Qt.HeldRequired\n", encoding="utf-8")
        required = qml_root / "Qt" / "HeldRequired"
        required.mkdir(parents=True)
        required_file = required / "Required.qml"
        required_file.write_text("Item {}\n", encoding="utf-8")

        original_read_text = Path.read_text

        def reject_qt_module_reopen(path: Path, *args, **kwargs) -> str:
            if path.is_relative_to(qml_root):
                raise AssertionError(f"Qt module path was reopened: {path}")
            return original_read_text(path, *args, **kwargs)

        cache_root = self.root / "qml-held-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        with RuntimeAuditLease(cache_root) as lease:
            held_qmldir = lease.hold(qmldir)
            held_qmldir.seek(3)
            with mock.patch.object(Path, "read_text", reject_qt_module_reopen):
                selected = windows_runtime_guard.qt_qml_closure(
                    qml_root, (source,), lease=lease
                )
            self.assertEqual(held_qmldir.tell(), 3)
            with mock.patch.object(
                windows_runtime_guard,
                "hashlib",
                SimpleNamespace(
                    sha256=mock.Mock(
                        side_effect=AssertionError("held QML text was read twice")
                    )
                ),
            ):
                lease.stage_readonly_layout(
                    "held-qml",
                    {
                        path.relative_to(qml_root): path
                        for path in selected
                    },
                    context={"policy": "held-qml-test-v1"},
                )

        self.assertIn(required_file.resolve(), selected)

    def test_qml_descriptor_memo_rejects_stale_metadata_and_stream(self) -> None:
        source = self.root / "qml-memo-source.qml"
        source.write_text("import Memo\n", encoding="utf-8")
        qml_root = self.qt_root / "qml"
        module = qml_root / "Memo"
        module.mkdir(parents=True)
        module_file = module / "Memo.qml"
        module_file.write_text("Item {}\n", encoding="utf-8")
        other_file = self.root / "memo-other.qml"
        other_file.write_text("different\n", encoding="utf-8")

        with RuntimeAuditLease() as lease:
            windows_runtime_guard.qt_qml_closure(
                qml_root, (source,), lease=lease
            )
            key = module_file.absolute()
            descriptor = lease._held_source_descriptors[key]
            stale = windows_runtime_cache.HeldSource(
                source=descriptor.source,
                final_path=descriptor.final_path,
                device=descriptor.device,
                inode=descriptor.inode,
                size=descriptor.size + 1,
                sha256=descriptor.sha256,
                stream=descriptor.stream,
            )
            lease._held_source_descriptors[key] = stale
            with self.assertRaisesRegex(RuntimeError, "metadata mismatch"):
                lease._describe_held_source(module_file)

            different_stream = lease.hold(other_file)
            lease._held_source_descriptors[key] = windows_runtime_cache.HeldSource(
                source=descriptor.source,
                final_path=descriptor.final_path,
                device=descriptor.device,
                inode=descriptor.inode,
                size=descriptor.size,
                sha256=descriptor.sha256,
                stream=different_stream,
            )
            with self.assertRaisesRegex(RuntimeError, "stream mismatch"):
                lease._describe_held_source(module_file)

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_qml_closure_holds_final_paths_against_junction_swap(self) -> None:
        qml_root = self.root / "qml-race"
        module = qml_root / "Foo"
        module.mkdir(parents=True)
        (module / "asset.qml").write_text("internal", encoding="utf-8")
        original_module = qml_root / "Foo-original"
        external = self.root / "qml-race-external"
        external.mkdir()
        (external / "asset.qml").write_text("external", encoding="utf-8")
        source = self.root / "qml-race-source.qml"
        source.write_text("import Foo\n", encoding="utf-8")
        original_scandir = os.scandir
        swapped = False
        test_case = self

        class RacingScandir:
            def __init__(self, directory: Path) -> None:
                self.directory = Path(directory)
                self.inner = original_scandir(directory)

            def __enter__(self):
                return self.inner.__enter__()

            def __exit__(self, exc_type, exc_value, traceback):
                nonlocal swapped
                result = self.inner.__exit__(exc_type, exc_value, traceback)
                if (
                    not swapped
                    and os.path.normcase(str(self.directory))
                    == os.path.normcase(str(module))
                ):
                    module.rename(original_module)
                    test_case.make_junction(module, external)
                    swapped = True
                return result

        with RuntimeAuditLease() as lease:
            lease.canonical_directory(qml_root)
            with (
                mock.patch.object(
                    windows_runtime_guard.os,
                    "scandir",
                    side_effect=lambda path: RacingScandir(Path(path)),
                ),
                self.assertRaisesRegex(RuntimeError, "reparse alias"),
            ):
                windows_runtime_guard.qt_qml_closure(
                    qml_root,
                    (source,),
                    lease=lease,
                )
        self.assertTrue(swapped)

    def test_discovery_and_audit_cost_is_bounded_for_focused_launches(self):
        commands: list[str] = []
        for index in range(256):
            executable = self.build_dir / f"tst_bounded_{index:03d}.exe"
            executable.write_bytes(b"pe-fixture")
            commands.append(
                f'add_test([=[bounded-{index}]=] "{executable.name}")'
            )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            "\n".join(commands) + "\n", encoding="utf-8"
        )
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        started = time.perf_counter()
        executables = registered_test_executables(self.build_dir)
        reject_runtime_dll_shadowing(
            self.build_dir,
            roots,
            executables=executables,
        )
        elapsed = time.perf_counter() - started

        self.assertEqual(len(executables), 256)
        self.assertLess(elapsed, 5.0)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_protected_launch_snapshot_cost_is_bounded(self):
        commands: list[str] = []
        for index in range(64):
            executable = self.build_dir / f"tst_snapshot_{index:03d}.exe"
            _write_manifest_pe_fixture(executable)
            commands.append(
                f'add_test([=[snapshot-{index}]=] "{executable.as_posix()}")'
            )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            "\n".join(commands) + "\n", encoding="utf-8"
        )
        noise_roots = (
            self.compiler_bin,
            self.qt_root / "bin",
            self.qt_root / "plugins" / "documentation",
            self.qt_root / "qml" / "examples",
            self.cmake_bin.parent,
        )
        for root in noise_roots:
            root.mkdir(parents=True, exist_ok=True)
            with (root / "unused-debug-payload.pdb").open("wb") as stream:
                stream.truncate(64 << 20)
        for index in range(1024):
            root = noise_roots[index % len(noise_roots)]
            (root / f"unused-{index:04d}.txt").write_bytes(b"not runtime")

        measurements: list[tuple[str, float, int, int]] = []
        for label, arguments in (
            ("full-cold", ()),
            ("full-warm", ()),
            ("focused-cold", ("-R", "^snapshot-7$")),
            ("focused-warm", ("-R", "^snapshot-7$")),
        ):
            started = time.perf_counter()
            with RuntimeAuditLease() as lease:
                run_ctest.prepare_windows_runtime(
                    self.build_dir,
                    {},
                    audit_lease=lease,
                    ctest_args=arguments,
                )
                staged = lease.launch_build_directory
                expected = 1 if arguments else 64
                self.assertEqual(
                    len(tuple(staged.glob("tst_snapshot_*.exe"))), expected
                )
                measurements.append(
                    (
                        label,
                        time.perf_counter() - started,
                        lease.staged_file_count,
                        lease.staged_byte_count,
                    )
                )

        for label, elapsed, file_count, byte_count in measurements:
            with self.subTest(label=label):
                self.assertLess(elapsed, 12.0)
                self.assertLess(file_count, 256)
                self.assertLess(byte_count, 64 << 20)
        focused = [item for item in measurements if item[0].startswith("focused")]
        self.assertTrue(all(item[2] < 64 for item in focused))

    @unittest.skipUnless(os.name == "nt", "requires installed Windows Qt kit")
    def test_actual_qt_focused_full_and_python_closures_have_absolute_budgets(self):
        candidates = sorted(
            path
            for path in Path("C:/Qt").glob("*/mingw_64")
            if (path / "bin" / "Qt6Core.dll").is_file()
        )
        if not candidates:
            self.skipTest("no installed C:/Qt/*/mingw_64 kit")
        qt = candidates[-1]
        project_source = Path(__file__).resolve().parents[2]
        started = time.perf_counter()
        with RuntimeAuditLease() as lease:
            plugins = windows_runtime_guard.qt_plugin_closure(
                qt / "plugins", lease=lease
            )
        with RuntimeAuditLease() as lease:
            qml = windows_runtime_guard.qt_qml_closure(
                qt / "qml", (project_source,), lease=lease
            )
        self.assertTrue(
            any(
                path.suffix.casefold()
                not in {".dll", ".qml", ".js", ".mjs", ".qmltypes"}
                and path.name.casefold() != "qmldir"
                for path in qml
            ),
            "actual selected QML modules must retain runtime assets",
        )
        runtime = windows_runtime_guard.transitive_runtime_closure(
            (*plugins, *(path for path in qml if path.suffix.casefold() == ".dll")),
            (qt / "bin",),
        )
        with RuntimeAuditLease() as lease:
            focused_plugins = windows_runtime_guard.qt_plugin_closure(
                qt / "plugins", (), lease=lease
            )
        focused_runtime = windows_runtime_guard.transitive_runtime_closure(
            focused_plugins, (qt / "bin",)
        )
        resolved_elapsed = time.perf_counter() - started

        full = (*runtime, *plugins, *qml)
        full_bytes = sum(path.stat().st_size for path in full)
        focused_bytes = sum(
            path.stat().st_size for path in (*focused_runtime, *focused_plugins)
        )
        self.assertLess(resolved_elapsed, 20.0)
        self.assertLess(full_bytes, 160 << 20)
        self.assertLess(len(full), 1800)
        self.assertLess(focused_bytes, 32 << 20)
        self.assertLess(len(focused_runtime) + len(focused_plugins), 16)

        cache_root = self.root / "actual-qt-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        staged_started = time.perf_counter()
        with RuntimeAuditLease(cache_root) as lease:
            lease.stage_readonly_layout(
                "actual-qt-runtime",
                {Path(str(index)) / path.name: path for index, path in enumerate(runtime)},
                byte_limit=1 << 30,
            )
            lease.stage_readonly_layout(
                "actual-qt-plugins",
                {path.relative_to(qt / "plugins"): path for path in plugins},
                byte_limit=1 << 30,
            )
            lease.stage_readonly_layout(
                "actual-qt-qml",
                {path.relative_to(qt / "qml"): path for path in qml},
                entry_limit=32768,
                byte_limit=1 << 30,
            )
            self.assertEqual(lease.staged_byte_count, full_bytes)
            self.assertGreater(lease.cache_miss_count, 0)
        self.assertLess(time.perf_counter() - staged_started, 60.0)

        warm_started = time.perf_counter()
        with RuntimeAuditLease(cache_root) as warm:
            warm.stage_readonly_layout(
                "actual-qt-runtime",
                {Path(str(index)) / path.name: path for index, path in enumerate(runtime)},
                byte_limit=1 << 30,
            )
            warm.stage_readonly_layout(
                "actual-qt-plugins",
                {path.relative_to(qt / "plugins"): path for path in plugins},
                byte_limit=1 << 30,
            )
            warm.stage_readonly_layout(
                "actual-qt-qml",
                {path.relative_to(qt / "qml"): path for path in qml},
                entry_limit=32768,
                byte_limit=1 << 30,
            )
            self.assertEqual(warm.cache_miss_count, 0)
            self.assertEqual(warm.cache_hit_count, 3)
            self.assertEqual(
                (warm.payload_written_file_count, warm.payload_written_byte_count),
                (0, 0),
            )
        self.assertLess(time.perf_counter() - warm_started, 20.0)

        focused_started = time.perf_counter()
        with RuntimeAuditLease() as lease:
            lease.stage_readonly_layout(
                "actual-focused-runtime",
                {
                    Path(str(index)) / path.name: path
                    for index, path in enumerate(focused_runtime)
                },
                byte_limit=64 << 20,
            )
            lease.stage_readonly_layout(
                "actual-focused-plugins",
                {
                    path.relative_to(qt / "plugins"): path
                    for path in focused_plugins
                },
                byte_limit=16 << 20,
            )
            self.assertEqual(lease.staged_byte_count, focused_bytes)
        self.assertLess(time.perf_counter() - focused_started, 10.0)

        python_closure = run_ctest._external_tool_closure(Path(sys.executable))
        python_bytes = sum(
            value.stat().st_size
            for value in python_closure.entries.values()
            if isinstance(value, Path)
        )
        self.assertLess(len(python_closure.entries), 2000)
        self.assertLess(python_bytes, 64 << 20)
        python_started = time.perf_counter()
        with RuntimeAuditLease() as lease:
            lease.stage_readonly_layout(
                "actual-python",
                python_closure.entries,
                entry_limit=32768,
                byte_limit=256 << 20,
            )
            self.assertEqual(lease.staged_byte_count, python_bytes)
        self.assertLess(time.perf_counter() - python_started, 45.0)

    def test_windows_runtime_replaces_inherited_path(self):
        env, ctest = run_ctest.prepare_windows_runtime(
            self.build_dir, {"PATH": str(self.ambient_bin)}
        )

        self.assertEqual(
            env["PATH"].split(os.pathsep)[:6],
            [
                str(self.compiler_bin),
                str(self.qt_root / "bin"),
                str(self.ffmpeg_root / "bin"),
                str(self.ffmpeg_root / "lib"),
                str(self.srt_root / "bin"),
                str(self.srt_root / "lib"),
            ],
        )
        self.assertNotIn(str(self.ambient_bin), env["PATH"].split(os.pathsep))
        system_dir, windows_dir = windows_runtime_guard.windows_system_directories()
        self.assertEqual(
            env["PATH"].split(os.pathsep)[-2:],
            [str(system_dir), str(windows_dir)],
        )
        self.assertEqual(ctest, self.ctest)

    def test_windows_runtime_replaces_controlled_environment_case_insensitively(self):
        inherited = {
            "Path": str(self.ambient_bin),
            "SYSTEMROOT": str(self.root / "hostile-windows"),
            "qt_plugin_path": str(self.root / "hostile-plugins"),
            "Qt_Qpa_Platform": "windows",
            "unrelated": "preserved",
        }

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        for controlled in (
            "PATH",
            "SystemRoot",
            "QT_PLUGIN_PATH",
            "QT_QPA_PLATFORM",
        ):
            self.assertEqual(
                sum(key.casefold() == controlled.casefold() for key in env),
                1,
                controlled,
            )
        self.assertEqual(env["unrelated"], "preserved")
        self.assertNotIn("hostile", "\n".join(env.values()))

    def test_windows_runtime_scrubs_interpreter_and_qt_injection_environment(self):
        removed = (
            "CMAKE_CONFIG_DIR",
            "CTEST_PARALLEL_LEVEL",
            "CTEST_USE_INSTRUMENTATION",
            "CTEST_USE_VERBOSE_INSTRUMENTATION",
            "PYTHONPATH",
            "PYTHONHOME",
            "PYTHONBREAKPOINT",
            "PYTHONWARNINGS",
            "BASH_ENV",
            "BASH_LOADABLES_PATH",
            "BASH_FUNC_ATTACK%%",
            "ENV",
            "QT_OPENGL_DLL",
            "QT_OPENGL_BUGLIST",
            "QT_QUICK_CONTROLS_CONF",
            "QT_QUICK_CONTROLS_IMAGINE_PATH",
            "QT_STYLE_OVERRIDE",
            "QT_QPA_PLATFORMTHEME",
            "QT_QPA_GENERIC_PLUGINS",
            "QT_QPA_FONTDIR",
            "QT_QUICK_CONTROLS_STYLE",
            "QT_QUICK_CONTROLS_FALLBACK_STYLE",
            "QT_VIRTUALKEYBOARD_LAYOUT_PATH",
            "QTWEBENGINEPROCESS_PATH",
            "QTWEBENGINE_DICTIONARIES_PATH",
            "QTWEBENGINE_LOCALES_PATH",
        )
        inherited = {
            name.swapcase(): str(self.root / f"hostile-{index}")
            for index, name in enumerate(removed)
        }
        inherited["BASH_FUNC_injected%%"] = "() { C:/attacker.exe; }"
        inherited["unrelated"] = "preserved"

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        folded = {name.casefold() for name in env}
        self.assertTrue({name.casefold() for name in removed}.isdisjoint(folded))
        self.assertNotIn("bash_func_injected%%", folded)
        self.assertEqual(env["unrelated"], "preserved")

    def test_ctest_unknown_olr_environment_preserves_scalars_but_rejects_paths(self):
        executable = self.build_dir / "tst_environment.exe"
        _write_manifest_pe_fixture(executable)
        scalar_values = ("1", "ON", "multiview", "12.5", "ndi")
        scalar_environment = ";".join(
            f"OLR_SCALAR_{index}={value}"
            for index, value in enumerate(scalar_values)
        )
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(
            f'add_test([=[environment]=] "{executable.as_posix()}")\n'
            'set_tests_properties([=[environment]=] PROPERTIES '
            f'ENVIRONMENT "{scalar_environment}")\n',
            encoding="utf-8",
        )
        run_ctest.prepare_windows_runtime(self.build_dir, {})

        for value in (
            "C:/mutable/input.dat",
            "../mutable-input",
            "relative/input.dat",
            "mutable-input.cfg",
            ".env",
            "%TEMP%",
            "$HOME",
            "${HOME}",
            "~",
        ):
            with self.subTest(value=value):
                graph.write_text(
                    f'add_test([=[environment]=] "{executable.as_posix()}")\n'
                    'set_tests_properties([=[environment]=] PROPERTIES '
                    f'ENVIRONMENT "OLR_UNKNOWN={value}")\n',
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(
                    RuntimeError, r"OLR_UNKNOWN.*path|path.*OLR_UNKNOWN"
                ):
                    run_ctest.prepare_windows_runtime(self.build_dir, {})

    def test_windows_runtime_requires_embedded_manifests_for_selected_processes(self):
        with mock.patch(
            "tools.run_ctest.reject_runtime_dll_shadowing"
        ) as audit:
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

        self.assertTrue(audit.call_args.kwargs["require_embedded_manifests"])

    def test_windows_runtime_prefers_cached_ctest_command(self):
        selected_ctest = self.cmake_bin / "selected-ctest.exe"
        _write_manifest_pe_fixture(selected_ctest)
        self.cache_values["CMAKE_CTEST_COMMAND"] = str(selected_ctest)
        self.write_cache()

        _, ctest = run_ctest.prepare_windows_runtime(self.build_dir, {})

        self.assertEqual(ctest, selected_ctest)

    def test_windows_runtime_falls_back_to_ctest_beside_cmake(self):
        del self.cache_values["CMAKE_CTEST_COMMAND"]
        self.write_cache()

        _, ctest = run_ctest.prepare_windows_runtime(self.build_dir, {})

        self.assertEqual(ctest, self.ctest)

    def test_windows_runtime_rejects_each_missing_runtime_path(self):
        required_paths = {
            "compiler runtime": self.compiler_bin,
            "Qt runtime": self.qt_root / "bin",
            "Qt plugins": self.qt_root / "plugins",
            "Qt platform plugins": self.qt_root / "plugins" / "platforms",
            "Qt QML imports": self.qt_root / "qml",
            "FFmpeg runtime": self.ffmpeg_root / "bin",
            "FFmpeg libraries": self.ffmpeg_root / "lib",
            "SRT runtime": self.srt_root / "bin",
            "SRT libraries": self.srt_root / "lib",
            "CTest executable": self.ctest,
        }
        for label, path in required_paths.items():
            with self.subTest(label=label):
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink()
                try:
                    with self.assertRaisesRegex(
                        FileNotFoundError, f"{label}.*{path.name}"
                    ):
                        run_ctest.prepare_windows_runtime(self.build_dir, {})
                finally:
                    if label == "CTest executable":
                        path.touch()
                    else:
                        path.mkdir(parents=True)
                        if label == "compiler runtime":
                            (path / "g++.exe").write_bytes(b"compiler")
                        if label == "Qt plugins":
                            (path / "platforms").mkdir()

    def test_windows_runtime_sets_controlled_qt_defaults(self):
        inherited = {"PATH": str(self.ambient_bin)}

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt_root / "plugins"))
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
        self.assertNotIn("QT_PLUGIN_PATH", inherited)
        self.assertNotIn("QT_QPA_PLATFORM", inherited)

    def test_windows_runtime_replaces_inherited_qml_paths(self):
        inherited_qml = self.root / "inherited-qml"
        inherited_qml2 = self.root / "inherited-qml2"
        inherited = {
            "QML_IMPORT_PATH": str(inherited_qml),
            "QML2_IMPORT_PATH": str(inherited_qml2),
        }

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        self.assertEqual(env["QML_IMPORT_PATH"], str(self.qt_root / "qml"))
        self.assertEqual(env["QML2_IMPORT_PATH"], str(self.qt_root / "qml"))
        self.assertEqual(inherited["QML_IMPORT_PATH"], str(inherited_qml))
        self.assertEqual(inherited["QML2_IMPORT_PATH"], str(inherited_qml2))

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_windows_runtime_stages_qml_for_external_launch_lease(self):
        source_qml = self.qt_root / "qml"
        (source_qml / "Module.qml").write_text("source", encoding="utf-8")
        lease = RuntimeAuditLease()
        try:
            env, _ = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            staged_qml = Path(env["QML_IMPORT_PATH"])
            self.assertNotEqual(staged_qml, source_qml)
            self.assertEqual(env["QML2_IMPORT_PATH"], str(staged_qml))
            self.assertEqual(
                (staged_qml / "Module.qml").read_text(encoding="utf-8"), "source"
            )
        finally:
            lease.close()
        self.assertTrue(staged_qml.is_dir())
        with self.assertRaises(OSError):
            (staged_qml / "Injected.qml").write_text("poison", encoding="utf-8")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_external_launch_snapshots_quicktest_input_and_relative_imports(self):
        qml_input = self.root / "tests" / "qml"
        relative_module = self.root / "ui" / "components"
        qml_input.mkdir(parents=True)
        relative_module.mkdir(parents=True)
        test_qml = qml_input / "tst_snapshot.qml"
        component = relative_module / "SnapshotComponent.qml"
        test_qml.write_text(
            'import QtQuick\nimport "../../ui/components"\nItem {}\n',
            encoding="utf-8",
        )
        component.write_text("import QtQuick\nItem {}\n", encoding="utf-8")
        executable = self.build_dir / "tst_snapshot.exe"
        _write_manifest_pe_fixture(executable)
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(
            f'add_test([=[snapshot]=] "{executable.as_posix()}" '
            f'"-input" "../{qml_input.relative_to(self.root).as_posix()}" '
            '"--label" "source-fixture")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            staged_build = lease.launch_build_directory
            staged_source = lease.launch_source_directory
            self.assertNotEqual(staged_build, self.build_dir)
            self.assertNotEqual(staged_source, self.root)
            staged_graph = staged_build / "CTestTestfile.cmake"
            staged_text = staged_graph.read_text(encoding="utf-8")
            staged_input = staged_source / "tests" / "qml"
            staged_component = (
                staged_source / "ui" / "components" / component.name
            )
            self.assertIn(staged_input.as_posix(), staged_text)
            self.assertIn("source-fixture", staged_text)
            self.assertEqual(
                staged_component.read_text(encoding="utf-8"),
                "import QtQuick\nItem {}\n",
            )

            with self.assertRaises(OSError):
                test_qml.write_text("poison", encoding="utf-8")
            with self.assertRaises(OSError):
                component.write_text("poison", encoding="utf-8")
            self.assertIn(
                "../../ui/components", test_qml.read_text(encoding="utf-8")
            )
            self.assertIn("Item", staged_component.read_text(encoding="utf-8"))
            replacement = self.root / "replacement.qml"
            replacement.write_text("poison", encoding="utf-8")
            with self.assertRaises(OSError):
                (staged_input / test_qml.name).write_text(
                    "poison", encoding="utf-8"
                )
            with self.assertRaises(OSError):
                os.replace(replacement, staged_input / test_qml.name)
            with self.assertRaises(OSError):
                (staged_input / "Injected.qml").write_text(
                    "poison", encoding="utf-8"
                )
            with self.assertRaises(OSError):
                (staged_component.parent / "NestedInjected.qml").write_text(
                    "poison", encoding="utf-8"
                )
            with self.assertRaises(OSError):
                os.replace(
                    staged_source,
                    staged_source.with_name("retargeted-source"),
                )
            with self.assertRaises(OSError):
                os.replace(
                    staged_component.parent,
                    staged_component.parent.with_name("retargeted-components"),
                )

        self.assertTrue(staged_build.is_dir())
        self.assertTrue(staged_source.is_dir())
        with self.assertRaises(OSError):
            (staged_build / "late.exe").write_bytes(b"poison")
        with self.assertRaises(OSError):
            (staged_source / "late.qml").write_text("poison", encoding="utf-8")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_external_launch_uses_closed_executable_and_path_namespaces(self):
        executable = self.build_dir / "tst_closed_namespace.exe"
        _write_manifest_pe_fixture(executable)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[closed]=] "{executable.as_posix()}")\n'
            'set_tests_properties([=[closed]=] PROPERTIES ENVIRONMENT '
            f'"OLR_FFMPEG_ROOT={self.ffmpeg_root.as_posix()};'
            f'OLR_SRT_ROOT={self.srt_root.as_posix()}")\n',
            encoding="utf-8",
        )
        configured_qt = self.qt_root / "bin" / "Qt6Core.dll"
        configured_qt.write_bytes(b"qt")
        (self.build_dir / configured_qt.name).write_bytes(configured_qt.read_bytes())
        (self.ffmpeg_root / "bin" / "ffprobe.exe").write_bytes(b"ffprobe")
        (self.compiler_bin / "runtime-tool.dll").write_bytes(b"compiler-first")
        (self.qt_root / "bin" / "runtime-tool.dll").write_bytes(b"qt-second")
        support = self.build_dir / "OpenLiveReplaySupport.dll"
        support.write_bytes(b"support")

        with (
            RuntimeAuditLease() as lease,
            mock.patch.object(
                run_ctest,
                "transitive_runtime_closure",
                return_value=(
                    self.compiler_bin / "runtime-tool.dll",
                    configured_qt,
                ),
            ) as closure,
        ):
            env, selected_ctest = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            runtime = lease.launch_runtime_directory
            runtime_dirs = lease.launch_runtime_directories
            staged_build = lease.launch_build_directory
            staged_executable = staged_build / executable.relative_to(self.build_dir)
            self.assertEqual(
                Path(env["PATH"].split(os.pathsep)[0]), runtime_dirs[0]
            )
            self.assertNotIn(str(self.qt_root / "bin"), env["PATH"])
            self.assertNotEqual(selected_ctest, self.ctest)
            self.assertEqual(selected_ctest.name, "ctest.exe")
            self.assertEqual(
                (runtime_dirs[0] / "runtime-tool.dll").read_bytes(),
                b"compiler-first",
            )
            self.assertEqual(
                (runtime_dirs[1] / configured_qt.name).read_bytes(), b"qt"
            )
            self.assertFalse((staged_build / configured_qt.name).exists())
            self.assertTrue(staged_executable.is_file())
            self.assertIn(
                staged_executable.as_posix(),
                (staged_build / "CTestTestfile.cmake").read_text(encoding="utf-8"),
            )
            staged_graph = (staged_build / "CTestTestfile.cmake").read_text(
                encoding="utf-8"
            )
            self.assertIn((runtime.parent / "ffmpeg").as_posix(), staged_graph)
            self.assertIn((runtime.parent / "srt").as_posix(), staged_graph)
            for directory in (
                runtime,
                *runtime_dirs,
                selected_ctest.parent,
                staged_executable.parent,
            ):
                with self.subTest(directory=directory), self.assertRaises(OSError):
                    (directory / "late-injection.dll").write_bytes(b"poison")
            self.assertIn(support, closure.call_args.args[0])

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_external_launch_rewrites_relative_executable_and_stages_workdir(self):
        working = self.build_dir / "relative-workdir"
        working.mkdir()
        executable = working / "tst_relative_staged.exe"
        _write_manifest_pe_fixture(executable)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[relative-staged]=] "tst_relative_staged.exe" "future")\n'
            'set_tests_properties([=[relative-staged]=] PROPERTIES '
            'WORKING_DIRECTORY [=[relative-workdir]=])\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            staged_build = lease.launch_build_directory
            staged_executable = staged_build / executable.relative_to(self.build_dir)
            graph = (staged_build / "CTestTestfile.cmake").read_text(
                encoding="utf-8"
            )
            self.assertIn(staged_executable.as_posix(), graph)
            staged_working = staged_build / working.relative_to(self.build_dir)
            self.assertIn("# Controlled launch uses immutable", graph)
            self.assertIn(staged_working.as_posix(), graph)
            self.assertNotIn(working.as_posix(), graph)
            (working / "future").write_bytes(b"late mutable input")
            self.assertFalse((staged_working / "future").exists())
            with self.assertRaises(OSError):
                (staged_working / "future").write_bytes(b"late staged input")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_external_wrapper_keeps_protected_install_relative_resources(self):
        external_root = self.root / "external-tool"
        external_root.mkdir()
        wrapper = external_root / "wrapper.exe"
        _write_manifest_pe_fixture(wrapper)
        resources = external_root / "resources"
        resources.mkdir()
        (resources / "runtime.dat").write_bytes(b"resource")
        wrapper.with_name(wrapper.name + ".olr-runtime-files").write_text(
            "resources\n", encoding="utf-8"
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[external]=] "{wrapper.as_posix()}")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            graph = (lease.launch_build_directory / "CTestTestfile.cmake").read_text(
                encoding="utf-8"
            )
            command = Path(re.search(r'add_test\([^\n]+"([^"]+wrapper\.exe)"', graph).group(1))
            self.assertNotEqual(command, wrapper)
            self.assertEqual(
                (command.parent / "resources" / "runtime.dat").read_bytes(),
                b"resource",
            )
            with self.assertRaises(OSError):
                (command.parent / "late.dll").write_bytes(b"poison")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_git_bash_wrapper_stages_real_usr_bin_family(self):
        git_root = self.root / "Git"
        wrapper = git_root / "bin" / "bash.exe"
        real_bash = git_root / "usr" / "bin" / "bash.exe"
        msys_runtime = git_root / "usr" / "bin" / "msys-2.0.dll"
        shell_tool = git_root / "usr" / "bin" / "sed.exe"
        unrelated = git_root / "mingw64" / "bin" / "unused.exe"
        for executable in (wrapper, real_bash, shell_tool, unrelated):
            executable.parent.mkdir(parents=True, exist_ok=True)
            _write_manifest_pe_fixture(executable)
        msys_runtime.write_bytes(b"msys runtime")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[shell]=] "{wrapper.as_posix()}")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            graph = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")
            staged_wrapper = Path(
                re.search(r'add_test\([^\n]+"([^"]+bash\.exe)"', graph).group(1)
            )
            staged_root = staged_wrapper.parent.parent

            self.assertEqual(staged_wrapper.relative_to(staged_root), Path("bin/bash.exe"))
            self.assertEqual(
                (staged_root / "usr" / "bin" / "bash.exe").read_bytes(),
                real_bash.read_bytes(),
            )
            self.assertEqual(
                (staged_root / "usr" / "bin" / "msys-2.0.dll").read_bytes(),
                msys_runtime.read_bytes(),
            )
            self.assertEqual(
                (staged_root / "usr" / "bin" / "sed.exe").read_bytes(),
                shell_tool.read_bytes(),
            )
            self.assertFalse(
                (staged_root / "mingw64" / "bin" / unrelated.name).exists()
            )
            self.assertEqual(
                (
                    staged_root
                    / "mingw64"
                    / "bin"
                    / ".olr-runtime-root"
                ).read_bytes(),
                b"",
            )
            self.assert_directory_rejects_late_insertion(
                staged_root / "usr" / "bin"
            )
            self.assert_directory_rejects_late_insertion(
                staged_root / "mingw64" / "bin"
            )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_tree_closure_enforces_budgets_during_held_traversal(self):
        tree = self.root / "bounded-tree"
        tree.mkdir()
        for name in ("one.dat", "two.dat", "three.dat"):
            (tree / name).write_bytes(b"12")

        with RuntimeAuditLease() as lease:
            with self.assertRaisesRegex(
                RuntimeError, "external tool closure entry limit exceeded"
            ):
                run_ctest._tree_closure_files(
                    tree,
                    tree,
                    lease=lease,
                    entry_limit=2,
                    byte_limit=1024,
                )

        with RuntimeAuditLease() as lease:
            with self.assertRaisesRegex(
                RuntimeError, "external tool closure byte limit exceeded"
            ):
                run_ctest._tree_closure_files(
                    tree,
                    tree,
                    lease=lease,
                    entry_limit=10,
                    byte_limit=3,
                )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_literal_ctest_selectors_bound_snapshot_to_safe_superset(self):
        unit = self.build_dir / "tst_selected.exe"
        e2e = self.build_dir / "tst_unselected.exe"
        _write_manifest_pe_fixture(unit)
        _write_manifest_pe_fixture(e2e)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[selected]=] "{unit.as_posix()}")\n'
            'set_tests_properties([=[selected]=] PROPERTIES LABELS "unit;ci")\n'
            f'add_test([=[unselected]=] "{e2e.as_posix()}")\n'
            'set_tests_properties([=[unselected]=] PROPERTIES LABELS "e2e;ci")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-L", "unit", "-R", "^selected$"),
            )
            staged = lease.launch_build_directory
            self.assertTrue((staged / unit.name).is_file())
            self.assertFalse((staged / e2e.name).exists())
            graph = (staged / "CTestTestfile.cmake").read_text(encoding="utf-8")
            self.assertIn((staged / unit.name).as_posix(), graph)
            self.assertIn(e2e.as_posix(), graph)

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "selected|unselected"),
            )
            staged = lease.launch_build_directory
            self.assertTrue((staged / unit.name).is_file())
            self.assertTrue((staged / e2e.name).is_file())

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_selected_missing_command_executable_fails_before_late_creation(self):
        future = self.root / "outside" / "future.exe"
        future.parent.mkdir()
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[late]=] "{future.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "missing-command-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with (
            RuntimeAuditLease(cache_root) as lease,
            self.assertRaisesRegex(
                RuntimeError, r"late.*existing.*executable|executable.*late"
            ),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^late$"),
            )

        future.write_bytes(b"late mutable executable")
        self.assertTrue(future.is_file())

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_input_files_are_staged_and_arguments_rewritten(self):
        self.write_cacheable_ctest_graph("unit")
        inputs = {
            "tests-from": b"unit\n",
            "exclude-from": b"excluded\n",
            "resource": b'{"version":{"major":1,"minor":0},"local":[]}',
            "information": b"1,1,1\n",
        }
        paths: dict[str, Path] = {}
        for name, payload in inputs.items():
            path = self.root / "ctest-inputs" / f"{name}.txt"
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(payload)
            paths[name] = path
        caller_args = (
            "--tests-from-file",
            str(paths["tests-from"]),
            f'--tests-from-file={paths["tests-from"]}',
            "--exclude-from-file",
            str(paths["exclude-from"]),
            f'--exclude-from-file={paths["exclude-from"]}',
            "--resource-spec-file",
            str(paths["resource"]),
            f'--resource-spec-file={paths["resource"]}',
            "-I",
            str(paths["information"]),
            f'--tests-information={paths["information"]}',
            "--test-dir",
            str(self.build_dir),
            "--output-on-failure",
        )
        cache_root = self.root / "outer-ctest-input-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=caller_args,
            )
            rewritten = lease.launch_ctest_arguments
            self.assertNotIn("--test-dir", rewritten)
            self.assertNotIn(str(self.build_dir), rewritten)
            self.assertIn("--output-on-failure", rewritten)
            rendered = "\n".join(rewritten)
            for name, source in paths.items():
                self.assertNotIn(str(source), rendered, name)
                staged_paths = tuple(
                    Path(value.partition("=")[2] if "=" in value else value)
                    for value in rewritten
                    if value.endswith(source.name)
                )
                self.assertGreaterEqual(len(staged_paths), 2, name)
                for staged in staged_paths:
                    self.assertEqual(staged.read_bytes(), inputs[name])
                    with self.assertRaises(OSError):
                        staged.write_bytes(b"late mutation")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_parser_respects_official_option_value_arity(self):
        arguments = (
            "-R",
            "--tests-from-file",
            "-L",
            "--test-dir",
            "--test-output-size-passed",
            "--resource-spec-file",
            "--test-output-size-failed",
            "--exclude-from-file",
        )

        with RuntimeAuditLease() as lease:
            self.assertEqual(
                run_ctest._stage_controlled_ctest_arguments(arguments, lease),
                arguments,
            )

    def test_outer_ctest_parser_consumes_new_official_option_values(self):
        arguments = (
            "--presets-file",
            "--test-dir",
            "--repeat-until-fail",
            "--test-dir",
            "--submit-index",
            "--test-dir",
            "--collect-instrumentation",
            "--test-dir",
            "-L",
            "unit",
        )

        parsed = run_ctest._parse_ctest_arguments(arguments)

        self.assertEqual(
            tuple((argument.option, argument.values) for argument in parsed),
            (
                ("--presets-file", ("--test-dir",)),
                ("--repeat-until-fail", ("--test-dir",)),
                ("--submit-index", ("--test-dir",)),
                ("--collect-instrumentation", ("--test-dir",)),
                ("-L", ("unit",)),
            ),
        )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_repeat_until_fail_remains_test_mode(self):
        arguments = ("--repeat-until-fail", "7", "-L", "unit")

        with RuntimeAuditLease() as lease:
            self.assertEqual(
                run_ctest._stage_controlled_ctest_arguments(arguments, lease),
                arguments,
            )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_repeat_policy_is_bounded_and_until_fail_only(self):
        cap = run_ctest._CTEST_REPEAT_CAP
        accepted = (
            ("--repeat", "until-fail:1"),
            (f"--repeat=until-fail:{cap}",),
            ("--repeat-until-fail", "7"),
            (f"--repeat-until-fail={cap}",),
        )
        rejected = (
            ("--repeat", "until-pass:1"),
            ("--repeat", "after-timeout:1"),
            ("--repeat", "until-fail:0"),
            ("--repeat", "until-fail:not-a-number"),
            ("--repeat", f"until-fail:{cap + 1}"),
            ("--repeat-until-fail", "0"),
            ("--repeat-until-fail", "not-a-number"),
            ("--repeat-until-fail", str(cap + 1)),
        )

        for arguments in accepted:
            with self.subTest(accepted=arguments), RuntimeAuditLease() as lease:
                self.assertEqual(
                    run_ctest._stage_controlled_ctest_arguments(arguments, lease),
                    arguments,
                )
        for arguments in rejected:
            with self.subTest(rejected=arguments), RuntimeAuditLease() as lease:
                with self.assertRaisesRegex(RuntimeError, "repeat.*bounded|until-fail"):
                    run_ctest._stage_controlled_ctest_arguments(arguments, lease)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_parallel_policy_requires_bounded_positive_value(self):
        cap = run_ctest._CTEST_PARALLEL_CAP
        accepted = (
            ("-j", "1"),
            (f"-j{cap}",),
            ("-j=1",),
            ("--parallel", str(cap)),
            ("--parallel=1",),
        )
        rejected = (
            ("-j",),
            ("--parallel",),
            ("-j0",),
            ("-j=0",),
            ("--parallel", "0"),
            ("--parallel=not-a-number",),
            (f"-j{cap + 1}",),
            ("--parallel", str(cap + 1)),
        )

        for arguments in accepted:
            with self.subTest(accepted=arguments), RuntimeAuditLease() as lease:
                self.assertEqual(
                    run_ctest._stage_controlled_ctest_arguments(arguments, lease),
                    arguments,
                )
        for arguments in rejected:
            with self.subTest(rejected=arguments), RuntimeAuditLease() as lease:
                with self.assertRaisesRegex(RuntimeError, "parallel.*bounded|positive"):
                    run_ctest._stage_controlled_ctest_arguments(arguments, lease)

    def test_outer_ctest_output_files_are_rejected_before_child_launch(self):
        cases = (
            ("--output-log", str(self.root / "ctest.log")),
            (f"--output-log={self.root / 'ctest.log'}",),
            ("--output-junit", str(self.root / "ctest.xml")),
            (f"--output-junit={self.root / 'ctest.xml'}",),
            ("-O", str(self.root / "short.log")),
            (f"-O{self.root / 'attached.log'}",),
            (f"-O={self.root / 'equal.log'}",),
        )

        for arguments in cases:
            with self.subTest(arguments=arguments), mock.patch.object(
                run_ctest, "run_process_tree"
            ) as run:
                with self.assertRaisesRegex(RuntimeError, "unsupported.*CTest.*option"):
                    run_ctest.main(
                        ["--test-dir", str(self.build_dir), *arguments]
                    )
                run.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_equal_short_information_file_preserves_spelling(self):
        information = self.root / "indices.txt"
        information.write_text("1,1,1\n", encoding="utf-8")

        with RuntimeAuditLease() as lease:
            rewritten = run_ctest._stage_controlled_ctest_arguments(
                (f"-I={information}",), lease
            )
            self.assertEqual(len(rewritten), 1)
            self.assertTrue(rewritten[0].startswith("-I="))
            staged = Path(rewritten[0][3:])
            self.assertEqual(staged.read_text(encoding="utf-8"), "1,1,1\n")
            with self.assertRaises(OSError):
                staged.write_text("late", encoding="utf-8")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_missing_input_files_fail_before_child_launch(self):
        self.write_cacheable_ctest_graph("unit")
        missing = self.root / "future-ctest-input.txt"
        cases = (
            ("--tests-from-file", str(missing)),
            (f"--exclude-from-file={missing}",),
            ("--resource-spec-file", str(missing)),
            ("-I", str(missing)),
        )

        for arguments in cases:
            with self.subTest(arguments=arguments), mock.patch.object(
                run_ctest, "run_process_tree"
            ) as run:
                with self.assertRaisesRegex(
                    RuntimeError, r"input file does not exist"
                ):
                    run_ctest.main(["--test-dir", str(self.build_dir), *arguments])
                run.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_outer_ctest_unsupported_modes_fail_before_child_launch(self):
        self.write_cacheable_ctest_graph("unit")
        script = self.root / "dashboard.cmake"
        script.write_text("# dashboard\n", encoding="utf-8")
        cases = (
            ("rerun", ("--rerun-failed",)),
            ("dashboard", ("-D", "Experimental")),
            ("script", ("-S", str(script))),
            ("preset", ("--preset", "ci")),
            ("build-and-test", ("--build-and-test", "source", "build")),
            ("overwrite", ("--overwrite", "Site:STRING=hostile")),
            ("passthrough", ("--", "--test-dir", str(self.build_dir))),
            ("presets-file", ("--presets-file", "external.json")),
            ("submit-index", ("--submit-index", "1")),
            (
                "collect-instrumentation",
                ("--collect-instrumentation", "instrumentation"),
            ),
        )

        for label, arguments in cases:
            with self.subTest(label=label), mock.patch.object(
                run_ctest, "run_process_tree"
            ) as run:
                with self.assertRaisesRegex(
                    RuntimeError, r"unsupported.*CTest.*option|immutable"
                ):
                    run_ctest.main(["--test-dir", str(self.build_dir), *arguments])
                run.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_fixture_properties_disable_narrow_snapshot_optimization(self):
        setup = self.build_dir / "fixture_setup.exe"
        victim = self.build_dir / "fixture_victim.exe"
        cleanup = self.build_dir / "fixture_cleanup.exe"
        for executable in (setup, victim, cleanup):
            _write_manifest_pe_fixture(executable)
        setup_qml = self.root / "tests" / "fixture-qml"
        setup_qml.mkdir(parents=True)
        (setup_qml / "tst_setup.qml").write_text(
            "import QtQuick\nItem {}\n", encoding="utf-8"
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[setup]=] "{setup.as_posix()}" "-input" '
            f'"../{setup_qml.relative_to(self.root).as_posix()}")\n'
            'set_tests_properties([=[setup]=] PROPERTIES FIXTURES_SETUP "device")\n'
            f'add_test([=[victim]=] "{victim.as_posix()}")\n'
            'set_tests_properties([=[victim]=] PROPERTIES FIXTURES_REQUIRED "device")\n'
            f'add_test([=[cleanup]=] "{cleanup.as_posix()}")\n'
            'set_tests_properties([=[cleanup]=] PROPERTIES FIXTURES_CLEANUP "device")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^victim$"),
            )
            staged = lease.launch_build_directory
            for executable in (setup, victim, cleanup):
                self.assertTrue((staged / executable.name).is_file())
            staged_source = lease.launch_source_directory
            self.assertNotEqual(staged_source, self.root)
            graph = (staged / "CTestTestfile.cmake").read_text(encoding="utf-8")
            self.assertIn(
                (staged_source / setup_qml.relative_to(self.root)).as_posix(),
                graph,
            )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^victim$", "-FA", "ignored"),
            )
            self.assertTrue((lease.launch_build_directory / setup.name).is_file())

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_graph_rewrite_uses_path_identity_without_prefix_corruption(self):
        executable = self.build_dir / "tst_identity.exe"
        _write_manifest_pe_fixture(executable)
        runtime_dirs = (
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        mixed = tuple(
            str(path).swapcase().replace("\\", "/") for path in runtime_dirs
        )
        modifications = ";".join(
            f"PATH=path_list_prepend:{path}" for path in reversed(mixed)
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[identity]=] "{executable.as_posix()}" "opaque-output")\n'
            'set_tests_properties([=[identity]=] PROPERTIES '
            f'ENVIRONMENT_MODIFICATION "{modifications}")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            graph = (lease.launch_build_directory / "CTestTestfile.cmake").read_text(
                encoding="utf-8"
            )
            self.assertIn("opaque-output", graph)
            for staged_runtime in lease.launch_runtime_directories:
                self.assertIn(staged_runtime.as_posix(), graph)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_graph_rewrite_is_scoped_to_role_bearing_argument_occurrences(self):
        executable = self.build_dir / "tst_occurrence.exe"
        _write_manifest_pe_fixture(executable)
        forms = (
            lambda value: f'"{value}"',
            lambda value: f'[=[{value}]=]',
            lambda value: value,
        )
        commands: list[str] = []
        identities: list[Path] = []
        for index, render in enumerate(forms):
            child = self.build_dir / f"occurrence-{index}"
            child.mkdir()
            (child / "CTestTestfile.cmake").write_text("", encoding="utf-8")
            identity = child.resolve()
            identities.append(identity)
            literal = identity.as_posix()
            commands.append(
                f'add_test([=[ordinary-{index}]=] "{executable.as_posix()}" '
                f'{render(f"opaque-token-{index}")})'
            )
            commands.append(f'add_subdirectory({render(literal)})')
        (self.build_dir / "CTestTestfile.cmake").write_text(
            "\n".join(commands) + "\n", encoding="utf-8"
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            staged = lease.launch_build_directory
            graph = (staged / "CTestTestfile.cmake").read_text(encoding="utf-8")
            for identity in identities:
                staged_identity = staged / identity.relative_to(self.build_dir)
                self.assertNotIn(identity.as_posix(), graph)
                self.assertEqual(graph.count(staged_identity.as_posix()), 1)

    def test_fixture_selector_flags_disable_narrow_selection_without_fixture_properties(self):
        unit = self.build_dir / "selector-unit.exe"
        other = self.build_dir / "selector-other.exe"
        unit.write_bytes(b"unit")
        other.write_bytes(b"other")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[unit]=] "{unit.as_posix()}")\n'
            f'add_test([=[other]=] "{other.as_posix()}")\n',
            encoding="utf-8",
        )
        graph = windows_runtime_guard.discover_ctest_graph(self.build_dir)
        controls = (
            ("-FA", "skip"),
            ("-FS", "skip"),
            ("-FC", "skip"),
            ("--fixture-exclude-any", "skip"),
            ("--fixture-exclude-setup", "skip"),
            ("--fixture-exclude-cleanup", "skip"),
            ("--fixture-exclude-any=skip",),
            ("--fixture-exclude-setup=skip",),
            ("--fixture-exclude-cleanup=skip",),
        )
        for control in controls:
            with self.subTest(control=control):
                selected = run_ctest._selected_test_records(
                    graph,
                    run_ctest._parse_ctest_arguments(
                        ("-R", "^unit$", *control)
                    ),
                )
                self.assertEqual({test.name for test in selected}, {"unit", "other"})

    def test_selected_tests_share_parser_for_option_looking_output_value(self):
        unit = self.build_dir / "selector-unit.exe"
        other = self.build_dir / "selector-other.exe"
        unit.write_bytes(b"unit")
        other.write_bytes(b"other")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[unit]=] "{unit.as_posix()}")\n'
            f'add_test([=[other]=] "{other.as_posix()}")\n',
            encoding="utf-8",
        )
        graph = windows_runtime_guard.discover_ctest_graph(self.build_dir)
        parsed = run_ctest._parse_ctest_arguments(
            ("--output-log", "-R", "--tests-regex", "^unit$")
        )

        selected = run_ctest._selected_test_records(graph, parsed)

        self.assertEqual({test.name for test in selected}, {"unit"})

    def test_unknown_ctest_option_disables_exact_selection_optimization(self):
        unit = self.build_dir / "unknown-selector-unit.exe"
        other = self.build_dir / "unknown-selector-other.exe"
        unit.write_bytes(b"unit")
        other.write_bytes(b"other")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[unit]=] "{unit.as_posix()}")\n'
            f'add_test([=[other]=] "{other.as_posix()}")\n',
            encoding="utf-8",
        )
        graph = windows_runtime_guard.discover_ctest_graph(self.build_dir)
        parsed = run_ctest._parse_ctest_arguments(
            ("--future-positive-selector", "selection.txt", "-R", "^unit$")
        )

        selected = run_ctest._selected_test_records(graph, parsed)

        self.assertEqual({test.name for test in selected}, {"unit", "other"})

    def test_rendered_command_invariant_rejects_mutable_or_unstaged_executable(self):
        selected_name = "unit"
        original = self.build_dir / "unit.exe"
        staged = self.root / "protected-cache" / "unit.exe"
        outside = self.root / "unstaged" / "helper.exe"

        def graph_with(executable: Path) -> SimpleNamespace:
            return SimpleNamespace(
                tests=(
                    SimpleNamespace(
                        name=selected_name,
                        executable_arguments=((str(executable), executable, None),),
                    ),
                )
            )

        run_ctest._validate_rendered_test_commands(
            graph_with(staged),
            frozenset({selected_name}),
            self.build_dir,
            frozenset({staged}),
        )
        for executable in (original, outside):
            with self.subTest(executable=executable), self.assertRaisesRegex(
                RuntimeError, "original build|unstaged external"
            ):
                run_ctest._validate_rendered_test_commands(
                    graph_with(executable),
                    frozenset({selected_name}),
                    self.build_dir,
                    frozenset({staged}),
                )

    def test_quicktest_inputs_are_keyed_by_add_test_occurrence(self) -> None:
        first_scope = self.build_dir / "quick-first"
        second_scope = self.build_dir / "quick-second"
        first_scope.mkdir()
        second_scope.mkdir()
        first_input = self.root / "tests" / "quick-first"
        second_input = self.root / "tests" / "quick-second"
        first_input.mkdir(parents=True)
        second_input.mkdir(parents=True)
        (first_input / "tst_first.qml").write_text("Item {}\n", encoding="utf-8")
        (second_input / "tst_second.qml").write_text("Item {}\n", encoding="utf-8")
        executable = self.build_dir / "quicktest.exe"
        executable.write_bytes(b"quicktest")
        (first_scope / "CTestTestfile.cmake").write_text(
            f'add_test([=[duplicate]=] "{executable.as_posix()}" '
            f'"-input" "{first_input.as_posix()}")\n',
            encoding="utf-8",
        )
        (second_scope / "CTestTestfile.cmake").write_text(
            "# different source offset\n\n"
            f'add_test([=[duplicate]=] "{executable.as_posix()}" '
            f'"-input={second_input.as_posix()}")\n',
            encoding="utf-8",
        )
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'subdirs("quick-first")\nsubdirs("quick-second")\n',
            encoding="utf-8",
        )

        graph = windows_runtime_guard.discover_ctest_graph(self.build_dir)
        mapping, paths = run_ctest._quicktest_inputs(
            graph.tests, self.root, self.build_dir
        )

        self.assertEqual(len(mapping), 2)
        self.assertEqual(set(paths), {first_input.resolve(), second_input.resolve()})
        self.assertNotEqual(
            graph.tests[0].argument_occurrences[0].start,
            graph.tests[1].argument_occurrences[0].start,
        )
        for record in graph.tests:
            selected = mapping[run_ctest._quicktest_record_key(record)]
            self.assertEqual(len(selected), 1)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_qt_bin_tool_does_not_snapshot_the_entire_install_prefix(self):
        tool = self.qt_root / "bin" / "qmllint.exe"
        _write_manifest_pe_fixture(tool)
        (self.qt_root / "bin" / "avcodec-61.dll").write_bytes(b"qt-tool-ffmpeg")
        unused_qt = self.qt_root / "bin" / "Qt6UnusedForQmllint.dll"
        with unused_qt.open("wb") as stream:
            stream.truncate(32 << 20)
        (self.ffmpeg_root / "bin" / "avcodec-62.dll").write_bytes(
            b"application-ffmpeg"
        )
        noise = self.qt_root / "Docs" / "Qt-oversized"
        noise.mkdir(parents=True)
        for index in range(512):
            (noise / f"unused-{index:04d}.dat").write_bytes(b"unused")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[qml-smoke]=] "{tool.as_posix()}")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            env, _ctest = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            graph = (lease.launch_build_directory / "CTestTestfile.cmake").read_text(
                encoding="utf-8"
            )
            staged_tool = next(
                Path(value)
                for value in re.findall(r'"([^"]+qmllint\.exe)"', graph)
                if Path(value) != tool
            )
            self.assertTrue(staged_tool.is_file())
            self.assertEqual(
                (staged_tool.parent / "avcodec-61.dll").read_bytes(),
                b"qt-tool-ffmpeg",
            )
            self.assertFalse((staged_tool.parent / unused_qt.name).exists())
            rendered = (staged_tool.parent / "qt.conf").read_text(encoding="utf-8")
            self.assertIn(Path(env["QT_PLUGIN_PATH"]).as_posix(), rendered)
            self.assertIn(Path(env["QML_IMPORT_PATH"]).as_posix(), rendered)
            self.assertFalse(
                (staged_tool.parent.parent / "Docs" / "Qt-oversized").exists()
            )

    def test_qmllint_arguments_use_only_staged_build_qt_and_source_paths(self):
        tool = self.qt_root / "bin" / "qmllint.exe"
        _write_manifest_pe_fixture(tool)
        lint_plugin = (
            self.qt_root / "plugins" / "qmllint" / "quicklintplugin.dll"
        )
        lint_plugin.parent.mkdir(parents=True)
        lint_plugin.write_bytes(b"quick lint plugin")
        build_qml = self.build_dir / "qml"
        build_qml.mkdir()
        (build_qml / "BuildModule.qmltypes").write_text(
            "build module\n", encoding="utf-8"
        )
        source_qml = self.root / "ui" / "Main.qml"
        source_qml.parent.mkdir()
        source_qml.write_text("import QtQuick\nItem {}\n", encoding="utf-8")
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(
            f'add_test([=[qml-smoke]=] "{tool.as_posix()}" '
            f'"-I" "{build_qml.as_posix()}" "-I" '
            f'"{(self.qt_root / "qml").as_posix()}" '
            f'"--Quick.layout-positioning" "error" '
            f'"{source_qml.as_posix()}" "--literal-mode")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "qml-argument-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as lease:
            env, _ctest = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            rewritten = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")
            arguments = tuple(Path(value) for value in re.findall(r'"([^"]+)"', rewritten))

            self.assertNotIn(build_qml.as_posix(), rewritten)
            self.assertNotIn((self.qt_root / "qml").as_posix(), rewritten)
            self.assertNotIn(source_qml.as_posix(), rewritten)
            staged_build_qml = next(path for path in arguments if path.name == "qml")
            staged_source = next(path for path in arguments if path.name == "Main.qml")
            self.assertIn(Path(env["QML_IMPORT_PATH"]), arguments)
            self.assertEqual(
                (staged_build_qml / "BuildModule.qmltypes").read_text(encoding="utf-8"),
                "build module\n",
            )
            self.assertEqual(staged_source.read_bytes(), source_qml.read_bytes())
            self.assertEqual(
                (
                    Path(env["QT_PLUGIN_PATH"])
                    / "qmllint"
                    / lint_plugin.name
                ).read_bytes(),
                lint_plugin.read_bytes(),
            )
            self.assertNotEqual(lease.launch_source_directory, self.root)
            self.assertIn('"--literal-mode"', rewritten)
            self.assertIn('"--Quick.layout-positioning" "error"', rewritten)
            self.assert_directory_rejects_late_insertion(staged_build_qml)
            self.assert_directory_rejects_late_insertion(staged_source.parent)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_nested_build_qml_import_prefers_its_immutable_build_snapshot(self):
        executable = self.build_dir / "tst_style.exe"
        _write_manifest_pe_fixture(executable)
        quicktest_input = self.root / "tests" / "tst_style.qml"
        quicktest_input.parent.mkdir()
        quicktest_input.write_text("import QtQuick\nItem {}\n", encoding="utf-8")
        build_qml = self.build_dir / "qml"
        module = build_qml / "OlrTheme"
        module.mkdir(parents=True)
        (module / "qmldir").write_text("module OlrTheme\n", encoding="utf-8")
        qt_qml = self.qt_root / "qml"
        qmllint = self.qt_root / "bin" / "qmllint.exe"
        _write_manifest_pe_fixture(qmllint)
        lint_plugin = (
            self.qt_root / "plugins" / "qmllint" / "quicklintplugin.dll"
        )
        lint_plugin.parent.mkdir(parents=True)
        lint_plugin.write_bytes(b"quick lint plugin")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[style]=] "{executable.as_posix()}" '
            f'"-input" "{quicktest_input.as_posix()}")\n'
            'set_tests_properties([=[style]=] PROPERTIES '
            'ENVIRONMENT_MODIFICATION "'
            f'QML_IMPORT_PATH=path_list_prepend:{qt_qml.as_posix()};'
            f'QML2_IMPORT_PATH=path_list_prepend:{qt_qml.as_posix()};'
            f'QML_IMPORT_PATH=path_list_prepend:{build_qml.as_posix()};'
            f'QML2_IMPORT_PATH=path_list_prepend:{build_qml.as_posix()}")\n'
            f'add_test([=[qml-smoke]=] "{qmllint.as_posix()}" '
            f'"-I" "{build_qml.as_posix()}" "{quicktest_input.as_posix()}")\n',
            encoding="utf-8",
        )

        with RuntimeAuditLease() as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^(style|qml-smoke)$"),
            )
            rewritten = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")
            staged_imports = tuple(
                Path(value)
                for value in re.findall(
                    r"QML_IMPORT_PATH=path_list_prepend:([^;\"]+)",
                    rewritten,
                )
            )
            staged_build_qml = next(
                path
                for path in staged_imports
                if (path / "OlrTheme" / "qmldir").is_file()
            )

            self.assertNotIn(build_qml.as_posix(), rewritten)
            self.assertGreaterEqual(
                rewritten.count(staged_build_qml.as_posix()),
                2,
            )
            self.assertFalse(
                staged_build_qml.is_relative_to(lease.launch_source_directory)
            )
            self.assertEqual(
                (staged_build_qml / "OlrTheme" / "qmldir").read_text(
                    encoding="utf-8"
                ),
                "module OlrTheme\n",
            )
            self.assert_directory_rejects_late_insertion(staged_build_qml)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_qmllint_plugins_are_scoped_to_the_selected_tests_and_cache_key(self):
        tool = self.qt_root / "bin" / "qmllint.exe"
        _write_manifest_pe_fixture(tool)
        lint_plugin = (
            self.qt_root / "plugins" / "qmllint" / "quicklintplugin.dll"
        )
        lint_plugin.parent.mkdir(parents=True)
        lint_plugin.write_bytes(b"selected quick lint plugin")
        unit = self.build_dir / "unit.exe"
        _write_manifest_pe_fixture(unit)
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[qml-smoke]=] "{tool.as_posix()}")\n'
            f'add_test([=[unit]=] "{unit.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "qmllint-selection-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as normal_lease:
            normal_env, _ctest = run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=normal_lease,
                ctest_args=("-R", "^unit$"),
            )
            self.assertFalse(
                (Path(normal_env["QT_PLUGIN_PATH"]) / "qmllint").exists()
            )

        with RuntimeAuditLease(cache_root) as qmllint_lease:
            qmllint_env, _ctest = run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=qmllint_lease,
                ctest_args=("-R", "^qml-smoke$"),
            )
            self.assertEqual(
                (
                    Path(qmllint_env["QT_PLUGIN_PATH"])
                    / "qmllint"
                    / lint_plugin.name
                ).read_bytes(),
                lint_plugin.read_bytes(),
            )

    def test_python_script_and_option_paths_share_one_immutable_source_snapshot(self):
        python = self.root / "python-tool" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "driver.py"
        data = self.root / "tests" / "fixtures" / "input.json"
        positional = self.root / "playback" / "input.cpp"
        unrelated_qml = self.root / "ui" / "Unrelated.qml"
        qt_module = self.qt_root / "qml" / "ReviewerOnly"
        plugin = self.qt_root / "plugins" / "imageformats" / "qreviewer.dll"
        data.parent.mkdir(parents=True)
        positional.parent.mkdir(parents=True)
        unrelated_qml.parent.mkdir(parents=True)
        qt_module.mkdir()
        plugin.parent.mkdir()
        script.write_text("print('driver')\n", encoding="utf-8")
        data.write_text('{"input": true}\n', encoding="utf-8")
        positional.write_text("// positional Python input\n", encoding="utf-8")
        unrelated_qml.write_text("import ReviewerOnly\nItem {}\n", encoding="utf-8")
        (qt_module / "qmldir").write_text(
            "module ReviewerOnly\n", encoding="utf-8"
        )
        (qt_module / "ReviewerOnly.qml").write_text(
            "import QtQuick\nItem {}\n", encoding="utf-8"
        )
        plugin.write_bytes(b"python launch must not stage this plugin")
        graph = self.build_dir / "CTestTestfile.cmake"
        graph.write_text(
            f'add_test([=[python-data]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" "{positional.as_posix()}" '
            f'"--data={data.as_posix()}" '
            '"--raw-token")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "python-argument-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as lease:
            env, _ctest = run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            rewritten = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")

            self.assertNotIn(script.as_posix(), rewritten)
            self.assertNotIn(data.as_posix(), rewritten)
            self.assertNotIn(positional.as_posix(), rewritten)
            staged_script = next(
                Path(value)
                for value in re.findall(r'"([^"]+driver\.py)"', rewritten)
            )
            staged_data = next(
                Path(value.partition("=")[2])
                for value in re.findall(r'"(--data=[^"]+)"', rewritten)
            )
            staged_positional = next(
                Path(value)
                for value in re.findall(r'"([^"]+input\.cpp)"', rewritten)
            )
            self.assertEqual(staged_script.read_bytes(), script.read_bytes())
            self.assertEqual(staged_data.read_bytes(), data.read_bytes())
            self.assertEqual(staged_positional.read_bytes(), positional.read_bytes())
            self.assertEqual(staged_script.parents[1], staged_data.parents[2])
            self.assertEqual(staged_script.parents[1], staged_positional.parents[1])
            self.assertIn('"--raw-token"', rewritten)
            self.assertFalse(
                (Path(env["QML_IMPORT_PATH"]) / "ReviewerOnly").exists()
            )
            self.assertFalse(
                (Path(env["QT_PLUGIN_PATH"]) / "imageformats" / plugin.name).exists()
            )
            self.assert_directory_rejects_late_insertion(staged_script.parent)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows staging")
    def test_windows_ctest_runtime_contract_gets_exact_immutable_build_registration_closure(
        self,
    ):
        python = self.root / "python-contract" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        unselected = self.build_dir / "tst_unselected.exe"
        _write_manifest_pe_fixture(unselected)
        unselected_style = self.build_dir / "tst_unselected_style.exe"
        _write_manifest_pe_fixture(unselected_style)
        script = self.root / "tests" / "scripts" / "check_windows_ctest_runtime.py"
        script.parent.mkdir(parents=True)
        script.write_text("print('contract')\n", encoding="utf-8")
        generated = self.build_dir / "generated" / "CTestTestfile.cmake"
        generated.parent.mkdir()
        generated.write_text("# generated registration\n", encoding="utf-8")
        unrelated = self.build_dir / "generated" / "Unrelated.cmake"
        unrelated.write_text("# not in the declared CTest graph\n", encoding="utf-8")
        noise = self.build_dir / "generated" / "mutable-noise.dat"
        noise.write_bytes(b"not declared by the contract")
        project_qml = self.build_dir / "qml"
        project_module = project_qml / "OlrTheme"
        project_module.mkdir(parents=True)
        (project_module / "qmldir").write_text(
            "module OlrTheme\n", encoding="utf-8"
        )
        cache = self.build_dir / "CMakeCache.txt"
        graph = self.build_dir / "CTestTestfile.cmake"
        runtime_dirs = (
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        runtime_modifications = ";".join(
            f"PATH=path_list_prepend:{path.as_posix()}"
            for path in reversed(runtime_dirs)
        )
        graph.write_text(
            f'add_test([=[windows_ctest_runtime_contract]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" "--build-dir" "{self.build_dir.as_posix()}" '
            f'"--qt-runtime-dir" "{(self.qt_root / "bin").as_posix()}" '
            f'"--qt-plugin-dir" "{(self.qt_root / "plugins").as_posix()}" '
            f'"--qt-qml-dir" "{(self.qt_root / "qml").as_posix()}" '
            f'"--project-qml-dir" "{project_qml.as_posix()}")\n'
            f'add_test([=[unselected]=] "{unselected.as_posix()}")\n'
            'set_tests_properties([=[unselected]=] PROPERTIES '
            f'ENVIRONMENT_MODIFICATION "{runtime_modifications}")\n'
            f'add_test([=[unselected-style]=] "{unselected_style.as_posix()}")\n'
            'set_tests_properties([=[unselected-style]=] PROPERTIES '
            'ENVIRONMENT_MODIFICATION "'
            f'QML_IMPORT_PATH=path_list_prepend:{(self.qt_root / "qml").as_posix()};'
            f'QML2_IMPORT_PATH=path_list_prepend:{(self.qt_root / "qml").as_posix()};'
            f'QML_IMPORT_PATH=path_list_prepend:{project_qml.as_posix()};'
            f'QML2_IMPORT_PATH=path_list_prepend:{project_qml.as_posix()}")\n'
            'add_subdirectory("generated")\n',
            encoding="utf-8",
        )
        cache_root = Path(tempfile.mkdtemp(prefix="olr-contract-build-cache-"))
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        cache_bytes = cache.read_bytes()
        cache_was_locked_during_parse = False
        original_require_cache_entries = run_ctest.require_cache_entries

        def observe_cache_lock(*args, **kwargs):
            nonlocal cache_was_locked_during_parse
            result = original_require_cache_entries(*args, **kwargs)
            try:
                cache.write_bytes(cache_bytes)
            except OSError:
                cache_was_locked_during_parse = True
            return result

        with RuntimeAuditLease(cache_root) as lease, mock.patch.object(
            run_ctest,
            "require_cache_entries",
            side_effect=observe_cache_lock,
        ):
            env, staged_ctest = run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^windows_ctest_runtime_contract$"),
            )
            staged_build = lease.launch_build_directory
            rewritten = (staged_build / "CTestTestfile.cmake").read_text(
                encoding="utf-8"
            )
            staged_build_argument = next(
                Path(value)
                for value in re.findall(r'"([^\"]+)"', rewritten)
                if Path(value) == staged_build
            )

            self.assertEqual(staged_build_argument, staged_build)
            staged_project_qml = Path(
                re.search(
                    r'"--project-qml-dir" "([^"]+)"', rewritten
                ).group(1)
            )
            self.assertNotEqual(staged_project_qml, project_qml)
            self.assertFalse(staged_project_qml.is_relative_to(staged_build))
            self.assertNotIn(project_qml.as_posix(), rewritten)
            self.assertGreaterEqual(
                rewritten.count(staged_project_qml.as_posix()),
                3,
            )
            self.assertEqual(
                (staged_project_qml / "OlrTheme" / "qmldir").read_text(
                    encoding="utf-8"
                ),
                "module OlrTheme\n",
            )
            self.assert_directory_rejects_late_insertion(staged_project_qml)
            self.assertTrue(cache_was_locked_during_parse)
            staged_cache = staged_build / "CMakeCache.txt"
            staged_entries = windows_runtime_guard.parse_cmake_cache(staged_cache)
            expected_cache_paths = {
                "CMAKE_CXX_COMPILER": lease.launch_runtime_directories[0]
                / (self.compiler_bin / "g++.exe").name,
                "OLR_FFMPEG_ROOT": lease.launch_runtime_directories[2].parent,
                "OLR_SRT_ROOT": lease.launch_runtime_directories[4].parent,
                "CMAKE_CTEST_COMMAND": staged_ctest,
            }
            for key, expected in expected_cache_paths.items():
                self.assertEqual(Path(staged_entries[key].value), expected)
                self.assertTrue(expected.exists())
                self.assertNotIn(str(self.cache_values[key]), staged_entries[key].value)
            self.assertEqual(cache.read_bytes(), cache_bytes)
            self.assertNotEqual(staged_cache.read_bytes(), cache_bytes)
            self.assertIn(Path(env["QML_IMPORT_PATH"]).as_posix(), rewritten)
            self.assertNotIn((self.qt_root / "qml").as_posix(), rewritten)
            for original, staged in zip(
                runtime_dirs, lease.launch_runtime_directories, strict=True
            ):
                self.assertNotIn(original.as_posix(), rewritten)
                self.assertIn(staged.as_posix(), rewritten)
            self.assertFalse((staged_build / unselected.name).exists())
            self.assertFalse((staged_build / unselected_style.name).exists())
            self.assertEqual(
                (staged_build / generated.relative_to(self.build_dir)).read_bytes(),
                generated.read_bytes(),
            )
            self.assertFalse(
                (staged_build / unrelated.relative_to(self.build_dir)).exists()
            )
            self.assertFalse(
                (staged_build / noise.relative_to(self.build_dir)).exists()
            )
            self.assertEqual(
                {
                    path.relative_to(staged_build)
                    for path in staged_build.rglob("*.cmake")
                },
                {
                    Path("CTestTestfile.cmake"),
                    generated.relative_to(self.build_dir),
                },
            )
            with self.assertRaises(OSError):
                cache.write_text("poison", encoding="utf-8")
            with self.assertRaises(OSError):
                generated.write_text("poison", encoding="utf-8")
            with self.assertRaises(OSError):
                staged_cache.write_text("poison", encoding="utf-8")

        with cache.open("ab") as stream:
            stream.write(b"# cache identity changed\n")
        with RuntimeAuditLease(cache_root) as changed:
            _env, changed_ctest = run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=changed,
                ctest_args=("-R", "^windows_ctest_runtime_contract$"),
            )
            self.assertNotEqual(changed.launch_build_directory, staged_build)
            changed_cache = changed.launch_build_directory / "CMakeCache.txt"
            self.assertIn(
                b"# cache identity changed\n",
                changed_cache.read_bytes(),
            )
            self.assertEqual(
                Path(
                    windows_runtime_guard.parse_cmake_cache(changed_cache)[
                        "CMAKE_CTEST_COMMAND"
                    ].value
                ),
                changed_ctest,
            )

    def test_unknown_existing_python_option_path_fails_closed(self):
        python = self.root / "python-unknown" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "unknown_driver.py"
        undeclared = self.root / "tests" / "undeclared.dat"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        undeclared.write_bytes(b"undeclared")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python-unknown]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" "--mystery={undeclared.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "unknown-path-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with (
            RuntimeAuditLease(cache_root) as lease,
            self.assertRaisesRegex(
                RuntimeError, "unsupported CTest argument path role"
            ),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )

    def test_unknown_split_python_option_path_fails_closed(self):
        python = self.root / "python-unknown-split" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "unknown_split_driver.py"
        undeclared = self.root / "tests" / "undeclared-split.dat"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        undeclared.write_bytes(b"undeclared")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python-unknown-split]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" "--mystery" "{undeclared.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "unknown-split-path-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with (
            RuntimeAuditLease(cache_root) as lease,
            self.assertRaisesRegex(
                RuntimeError, "unsupported CTest argument path role"
            ),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )

    def test_standard_python_interpreter_options_preserve_their_arguments(self):
        python = self.root / "python-standard-options" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "scripts" / "option_driver.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        stdin_argument = self.root / "tests" / "fixtures" / "stdin-data.txt"
        stdin_argument.parent.mkdir(parents=True)
        stdin_argument.write_text("stdin argument\n", encoding="utf-8")
        command = "open('fixtures/input.txt')"
        module = "tests.scripts.test_run_ctest"
        discovery_start = script.parent
        discovery_top = self.root
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python-module]=] "{python.as_posix()}" '
            f'"-I" "-X" "utf8" "-m" "{module}")\n'
            f'add_test([=[python-unittest-selector]=] "{python.as_posix()}" '
            f'"-m" "unittest" "-v" "-k" "Case.test_method" '
            f'"package.tests.Case.test_method")\n'
            f'add_test([=[python-unittest-discovery]=] "{python.as_posix()}" '
            f'"-m" "unittest" "discover" "-s" "{discovery_start.as_posix()}" '
            f'"-p" "test*.py" "-t" "{discovery_top.as_posix()}")\n'
            f'add_test([=[python-command]=] "{python.as_posix()}" '
            f'"-B" "-Werror" "-c" "{command}")\n'
            f'add_test([=[python-combined-flags]=] "{python.as_posix()}" '
            f'"-IE" "-R" "-VVV" "{script.as_posix()}")\n'
            f'add_test([=[python-separator]=] "{python.as_posix()}" '
            f'"--" "{script.as_posix()}")\n'
            f'add_test([=[python-stdin]=] "{python.as_posix()}" '
            f'"--" "-" "{stdin_argument.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "standard-python-options-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            rewritten = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")

        self.assertIn(f'"-I" "-X" "utf8" "-m" "{module}"', rewritten)
        self.assertIn(
            '"-m" "unittest" "-v" "-k" "Case.test_method" '
            '"package.tests.Case.test_method"',
            rewritten,
        )
        self.assertNotIn(f'"{discovery_start.as_posix()}"', rewritten)
        self.assertNotIn(f'"{discovery_top.as_posix()}"', rewritten)
        self.assertIn('"-p" "test*.py"', rewritten)
        self.assertIn(f'"-B" "-Werror" "-c" "{command}"', rewritten)
        self.assertIn('"-IE" "-R" "-VVV"', rewritten)
        self.assertNotIn(script.as_posix(), rewritten)
        self.assertNotIn(stdin_argument.as_posix(), rewritten)
        self.assertIn('"--"', rewritten)

    def test_unknown_colon_attached_python_option_path_fails_closed(self):
        python = self.root / "python-unknown-colon" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "unknown_colon_driver.py"
        undeclared = self.root / "tests" / "undeclared-colon.dat"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        undeclared.write_bytes(b"undeclared")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python-unknown-colon]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" '
            f'"--mystery:{undeclared.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "unknown-colon-path-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with (
            RuntimeAuditLease(cache_root) as lease,
            self.assertRaisesRegex(
                RuntimeError, "unsupported CTest argument path role"
            ),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )

    def test_python_hash_mode_rejects_empty_attached_value(self):
        python = self.root / "python-empty-hash-mode" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "hash_mode_driver.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python-empty-hash-mode]=] "{python.as_posix()}" '
            f'"--check-hash-based-pycs=" "{script.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "empty-hash-mode-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with (
            RuntimeAuditLease(cache_root) as lease,
            self.assertRaisesRegex(RuntimeError, "hash-based-pycs|value"),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )

    def test_python_input_tokens_are_not_misclassified_as_quicktest_paths(self):
        python = self.root / "python-input-token" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "input_token_driver.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[python-command-input-token]=] "{python.as_posix()}" '
            f'"-c" "-input" "opaque-enum")\n'
            f'add_test([=[python-app-input-token]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" "-input" "opaque-enum")\n'
            f'add_test([=[python-app-attached-input-token]=] "{python.as_posix()}" '
            f'"{script.as_posix()}" "-input=opaque-enum")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "python-input-token-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with RuntimeAuditLease(cache_root) as lease:
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )
            rewritten = (
                lease.launch_build_directory / "CTestTestfile.cmake"
            ).read_text(encoding="utf-8")

        self.assertIn('"-c" "-input" "opaque-enum"', rewritten)
        self.assertIn('"-input" "opaque-enum"', rewritten)
        self.assertIn('"-input=opaque-enum"', rewritten)

    def test_unknown_absent_python_paths_fail_before_late_creation(self):
        python = self.root / "python-absent" / "python.exe"
        python.parent.mkdir()
        _write_manifest_pe_fixture(python)
        script = self.root / "tests" / "absent_driver.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("print('driver')\n", encoding="utf-8")
        candidates = (
            self.root / "tests" / "future.dat",
            self.root / "escaping.dat",
        )
        arguments = (
            f"--mystery={candidates[0].as_posix()}",
            "../escaping.dat",
        )

        for index, (argument, candidate) in enumerate(zip(arguments, candidates)):
            with self.subTest(argument=argument):
                (self.build_dir / "CTestTestfile.cmake").write_text(
                    f'add_test([=[python-absent-{index}]=] "{python.as_posix()}" '
                    f'"{script.as_posix()}" "{argument}")\n',
                    encoding="utf-8",
                )
                cache_root = Path(
                    tempfile.mkdtemp(prefix=f"olr-absent-path-cache-{index}-")
                )
                self.addCleanup(self.remove_test_runtime_cache, cache_root)
                with (
                    RuntimeAuditLease(cache_root) as lease,
                    self.assertRaisesRegex(
                        RuntimeError, "unsupported CTest argument path role"
                    ),
                ):
                    run_ctest.prepare_windows_runtime(
                        self.build_dir, {}, audit_lease=lease
                    )
                candidate.write_bytes(b"late mutable input")
                candidate.unlink()

    def test_unsupported_tool_existing_path_argument_fails_closed(self):
        tool = self.build_dir / "custom_tool.exe"
        _write_manifest_pe_fixture(tool)
        input_path = self.root / "custom-input.dat"
        input_path.write_bytes(b"input")
        (self.build_dir / "CTestTestfile.cmake").write_text(
            f'add_test([=[custom]=] "{tool.as_posix()}" '
            f'"{input_path.as_posix()}")\n',
            encoding="utf-8",
        )
        cache_root = self.root / "custom-path-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)

        with (
            RuntimeAuditLease(cache_root) as lease,
            self.assertRaisesRegex(
                RuntimeError, "unsupported CTest argument path role"
            ),
        ):
            run_ctest.prepare_windows_runtime(
                self.build_dir, {}, audit_lease=lease
            )

    def test_non_python_tool_closure_keeps_normal_delay_and_dynamic_family(self):
        root = self.root / "qt-tool-closure"
        root.mkdir()
        tool = root / "qmllint.exe"
        normal = root / "Qt6Normal.dll"
        delayed = root / "Qt6Delayed.dll"
        carrier = root / "avcodec-61.dll"
        unused = root / "Qt6Unused.dll"
        _write_pe_fixture(
            tool,
            imported_name=normal.name,
            directory_index=1,
        )
        _write_pe_fixture(normal, imported_name=delayed.name)
        _write_pe_fixture(delayed, imported_name="kernel32.dll", directory_index=1)
        carrier.write_bytes(b"tool-local-dynamic-family")
        with unused.open("wb") as stream:
            stream.truncate(16 << 20)

        closure = run_ctest._external_tool_closure(tool)

        self.assertIn(Path(normal.name), closure.entries)
        self.assertIn(Path(delayed.name), closure.entries)
        self.assertIn(Path(carrier.name), closure.entries)
        self.assertNotIn(Path(unused.name), closure.entries)

    def test_python_external_tool_closure_bounds_normal_install(self):
        root = self.root / "python-normal"
        executable = root / "python.exe"
        executable.parent.mkdir()
        _write_pe_fixture(executable, imported_name="python313.dll")
        _write_pe_fixture(root / "python313.dll", imported_name="kernel32.dll")
        (root / "DLLs").mkdir()
        _write_pe_fixture(
            root / "DLLs" / "_socket.pyd", imported_name="kernel32.dll"
        )
        (root / "Lib" / "json").mkdir(parents=True)
        (root / "Lib" / "json" / "__init__.py").write_text(
            "", encoding="utf-8"
        )
        adjacent = root / "python-adjacent-runtime.dll"
        adjacent.write_bytes(b"python-adjacent")
        site = root / "Lib" / "site-packages" / "oversized"
        site.mkdir(parents=True)
        for index in range(1024):
            (site / f"unused-{index:04d}.py").write_bytes(b"unused")

        closure = run_ctest._external_tool_closure(executable)

        self.assertEqual(closure.root, root)
        self.assertEqual(closure.executable_relative, Path("python.exe"))
        self.assertIn(Path("Lib/json/__init__.py"), closure.entries)
        self.assertIn(Path("DLLs/_socket.pyd"), closure.entries)
        self.assertIn(Path(adjacent.name), closure.entries)
        self.assertNotIn(
            Path("Lib/site-packages/oversized/unused-0000.py"), closure.entries
        )
        self.assertLess(len(closure.entries), 64)

    def test_python_external_tool_closure_preserves_venv_base_layout(self):
        base = self.root / "python-base"
        (base / "Lib" / "encodings").mkdir(parents=True)
        (base / "Lib" / "encodings" / "__init__.py").write_text(
            "", encoding="utf-8"
        )
        (base / "DLLs").mkdir()
        _write_pe_fixture(
            base / "DLLs" / "_socket.pyd", imported_name="kernel32.dll"
        )
        _write_pe_fixture(
            base / "python313.dll", imported_name="kernel32.dll"
        )
        venv = self.root / "venv"
        scripts = venv / "Scripts"
        scripts.mkdir(parents=True)
        executable = scripts / "python.exe"
        _write_pe_fixture(executable, imported_name="python313.dll")
        config = venv / "pyvenv.cfg"
        config.write_text(
            f"home = {base}\ninclude-system-site-packages = false\n",
            encoding="utf-8",
        )
        (venv / "Lib" / "site-packages").mkdir(parents=True)
        (venv / "Lib" / "site-packages" / "poison.py").write_text(
            "", encoding="utf-8"
        )

        closure = run_ctest._external_tool_closure(executable)

        self.assertEqual(closure.root, venv)
        self.assertEqual(closure.executable_relative, Path("Scripts/python.exe"))
        self.assertIn(Path("base/Lib/encodings/__init__.py"), closure.entries)
        self.assertNotIn(Path("Lib/site-packages/poison.py"), closure.entries)
        generated = closure.entries[Path("pyvenv.cfg")]
        self.assertIsInstance(generated, windows_runtime_cache.GeneratedLayoutEntry)
        rendered = generated.render(Path("C:/protected-python"))
        self.assertIn(b"home = C:/protected-python/base", rendered)

    def test_python_venv_closure_follows_base_root_pe_imports(self):
        base = self.root / "python-import-base"
        base.mkdir()
        runtime = base / "python313.dll"
        compiler_runtime = base / "vcruntime140.dll"
        _write_pe_fixture(runtime, imported_name=compiler_runtime.name)
        _write_pe_fixture(compiler_runtime, imported_name="kernel32.dll")
        (base / "Lib").mkdir()

        venv = self.root / "python-import-venv"
        scripts = venv / "Scripts"
        scripts.mkdir(parents=True)
        executable = scripts / "python.exe"
        _write_pe_fixture(executable, imported_name=runtime.name)
        (venv / "pyvenv.cfg").write_text(
            f"home = {base}\ninclude-system-site-packages = false\n",
            encoding="utf-8",
        )

        closure = run_ctest._external_tool_closure(executable)

        self.assertIn(Path("base/python313.dll"), closure.entries)
        self.assertIn(Path("base/vcruntime140.dll"), closure.entries)

    def test_external_tool_manifest_rejects_rooted_relative_path(self):
        root = self.root / "external-rooted"
        root.mkdir()
        executable = root / "wrapper.exe"
        executable.write_bytes(b"wrapper")
        executable.with_name(executable.name + ".olr-runtime-files").write_text(
            "\\rooted\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(RuntimeError, "unsafe path"):
            run_ctest._external_tool_closure(executable)

    def test_external_tool_closure_validates_descriptor_root_and_adjacent_files(self):
        root = self.root / "external-validation"
        root.mkdir()
        executable = root / "wrapper.exe"
        executable.write_bytes(b"wrapper")
        adjacent = root / "support.dll"
        adjacent.write_bytes(b"support")
        resources = root / "resources"
        resources.mkdir()
        (resources / "data.bin").write_bytes(b"data")
        descriptor = executable.with_name(executable.name + ".olr-runtime-files")
        descriptor.write_text("resources\n", encoding="utf-8")

        real_validate = run_ctest._validated_closure_path
        validated: list[Path] = []

        def record(path: Path, selected_root: Path | None = None, **kwargs) -> Path:
            validated.append(path)
            return real_validate(path, selected_root, **kwargs)

        with mock.patch.object(
            run_ctest, "_validated_closure_path", side_effect=record
        ):
            run_ctest._external_tool_closure(executable)

        self.assertIn(descriptor, validated)
        self.assertIn(resources, validated)
        self.assertIn(adjacent, validated)

    def test_external_tool_closure_rejects_descriptor_root_and_adjacent_aliases(self):
        cases = ("descriptor", "root", "adjacent")
        for kind in cases:
            with self.subTest(kind=kind):
                root = self.root / f"external-{kind}-alias"
                root.mkdir()
                executable = root / "wrapper.exe"
                executable.write_bytes(b"wrapper")
                descriptor = executable.with_name(
                    executable.name + ".olr-runtime-files"
                )
                if kind == "descriptor":
                    target = root / "descriptor-real.txt"
                    target.write_text("", encoding="utf-8")
                    link = descriptor
                    target_is_directory = False
                elif kind == "root":
                    target = root / "resources-real"
                    target.mkdir()
                    (target / "data.bin").write_bytes(b"data")
                    link = root / "resources"
                    target_is_directory = True
                    descriptor.write_text("resources\n", encoding="utf-8")
                else:
                    target = root / "support-real.dll"
                    target.write_bytes(b"support")
                    link = root / "support.dll"
                    target_is_directory = False
                try:
                    link.symlink_to(target, target_is_directory=target_is_directory)
                except OSError as error:
                    self.skipTest(f"cannot create symlink control: {error}")

                with self.assertRaisesRegex(RuntimeError, "reparse alias"):
                    run_ctest._external_tool_closure(executable)

    def test_external_tool_closure_rejects_file_alias_final_path_mismatch(self):
        real_resolve = Path.resolve
        for kind in ("descriptor", "adjacent"):
            with self.subTest(kind=kind):
                root = self.root / f"external-{kind}-final-path"
                root.mkdir()
                executable = root / "wrapper.exe"
                executable.write_bytes(b"wrapper")
                target = root / f"{kind}-real"
                target.write_bytes(b"real")
                if kind == "descriptor":
                    alias = executable.with_name(
                        executable.name + ".olr-runtime-files"
                    )
                    alias.write_text("", encoding="utf-8")
                else:
                    alias = root / "support.dll"
                    alias.write_bytes(b"support")

                def resolve(path: Path, *args, **kwargs) -> Path:
                    if os.path.normcase(str(path)) == os.path.normcase(str(alias)):
                        return target
                    return real_resolve(path, *args, **kwargs)

                with (
                    mock.patch.object(Path, "resolve", resolve),
                    self.assertRaisesRegex(RuntimeError, "reparse alias"),
                ):
                    run_ctest._external_tool_closure(executable)

    @unittest.skipUnless(os.name == "nt", "requires Windows directory handles")
    def test_external_tool_closure_holds_every_traversed_directory(self) -> None:
        root = self.root / "external-held-traversal"
        resources = root / "resources"
        nested = resources / "nested"
        nested.mkdir(parents=True)
        executable = root / "wrapper.exe"
        executable.write_bytes(b"wrapper")
        (nested / "data.bin").write_bytes(b"data")
        executable.with_name(executable.name + ".olr-runtime-files").write_text(
            "resources\n", encoding="utf-8"
        )

        with RuntimeAuditLease() as lease:
            closure = run_ctest._external_tool_closure(executable, lease=lease)
            self.assertIn(Path("resources/nested/data.bin"), closure.entries)
            with self.assertRaises(OSError):
                nested.rename(nested.with_name("retargeted-nested"))
            with self.assertRaises(OSError):
                resources.rename(resources.with_name("retargeted-resources"))

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    def test_external_tool_manifest_rejects_junction_resource_root(self) -> None:
        root = self.root / "external-junction-root"
        root.mkdir()
        executable = root / "wrapper.exe"
        executable.write_bytes(b"wrapper")
        outside = self.root / "external-junction-target"
        outside.mkdir()
        (outside / "data.bin").write_bytes(b"outside")
        self.make_junction(root / "resources", outside)
        executable.with_name(executable.name + ".olr-runtime-files").write_text(
            "resources\n", encoding="utf-8"
        )

        with self.assertRaisesRegex(RuntimeError, "reparse alias"):
            run_ctest._external_tool_closure(executable)

    def test_windows_runtime_replaces_explicit_qpa_platform(self):
        env, _ = run_ctest.prepare_windows_runtime(
            self.build_dir,
            {"PATH": str(self.ambient_bin), "QT_QPA_PLATFORM": "windows"},
        )

        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")

    def test_windows_runtime_reports_missing_required_cache_key(self):
        del self.cache_values["OLR_SRT_ROOT"]
        self.write_cache()

        with self.assertRaisesRegex(ValueError, "OLR_SRT_ROOT"):
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

    def test_windows_runtime_rejects_stale_qt_dll_beside_tests(self):
        configured = self.qt_root / "bin" / "Qt6Core.dll"
        configured_contents = b"configured-qt-6.10.3"
        configured.write_bytes(configured_contents)
        adjacent = self.build_dir / "Qt6Core.dll"
        adjacent_contents = b"stale-qt-6.10.2"
        adjacent.write_bytes(adjacent_contents)

        with self.assertRaises(RuntimeError) as raised:
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})
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

    def test_windows_runtime_accepts_identical_adjacent_qt_dll(self):
        configured = self.qt_root / "bin" / "Qt6Core.dll"
        configured.write_bytes(b"configured-qt-6.10.3")
        adjacent = self.build_dir / "Qt6Core.dll"
        adjacent.write_bytes(configured.read_bytes())

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt_root / "plugins"))
        self.assertEqual(adjacent.read_bytes(), configured.read_bytes())

    def test_windows_runtime_rejects_any_configured_toolchain_dll_shadow(self):
        configured = self.compiler_bin / "libatomic-1.dll"
        configured.write_bytes(b"configured-toolchain-runtime")
        adjacent = self.build_dir / "libatomic-1.dll"
        adjacent.write_bytes(b"stale-toolchain-runtime")

        with self.assertRaisesRegex(
            RuntimeError,
            rf"{re.escape(str(adjacent))}.*{re.escape(str(configured))}",
        ):
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

    def test_windows_runtime_rejects_configured_lib_directory_dll_shadow(self):
        configured = self.ffmpeg_root / "lib" / "runtime-only-in-lib.dll"
        configured.write_bytes(b"configured-lib-runtime")
        adjacent = self.build_dir / configured.name
        adjacent.write_bytes(b"stale-lib-runtime")

        with self.assertRaisesRegex(
            RuntimeError,
            rf"{re.escape(str(adjacent))}.*{re.escape(str(configured))}",
        ):
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

    def test_windows_runtime_uses_first_path_peer_for_duplicate_dll_name(self):
        configured_bin = self.ffmpeg_root / "bin" / "duplicate-runtime.dll"
        configured_bin.write_bytes(b"first-path-runtime")
        configured_lib = self.ffmpeg_root / "lib" / configured_bin.name
        configured_lib.write_bytes(b"later-path-runtime")
        adjacent = self.build_dir / configured_bin.name
        adjacent.write_bytes(configured_bin.read_bytes())

        run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

        self.assertNotEqual(adjacent.read_bytes(), configured_lib.read_bytes())

    def test_imported_stale_ffmpeg_abi_is_rejected_by_authoritative_root(self):
        active = self.ffmpeg_root / "bin" / "avcodec-62.dll"
        active.write_bytes(b"active-ffmpeg-abi")
        qt_private = self.qt_root / "bin" / "avcodec-61.dll"
        qt_private.write_bytes(b"qt-private-ffmpeg-abi")
        executable = self.build_dir / "tst_stale_codec.exe"
        executable.write_bytes(b"pe-fixture")

        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        with (
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                return_value={"avcodec-61.dll"},
            ),
            self.assertRaisesRegex(
                RuntimeError,
                r"tst_stale_codec\.exe.*avcodec-61\.dll.*OLR_FFMPEG_ROOT.*avcodec-62\.dll",
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_unknown_qt_named_app_dll_is_not_an_absent_authoritative_family(self):
        (self.qt_root / "bin" / "Qt6Core.dll").write_bytes(b"active-qt-core")
        executable = self.build_dir / "tst_qt_named_extension.exe"
        executable.write_bytes(b"pe-fixture")
        extension = self.build_dir / "Qt6OpenLiveReplayExtension.dll"
        extension.write_bytes(b"app-owned-extension")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with mock.patch(
            "tools.windows_runtime_guard.pe_imports",
            side_effect=lambda path, **_kwargs: (
                {extension.name} if path == executable else set()
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_selected_executable_rejects_dot_local_redirection(self):
        executable = self.build_dir / "tst_redirected.exe"
        executable.write_bytes(b"pe-fixture")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        redirection = Path(f"{executable}.local")
        for as_directory in (False, True):
            with self.subTest(as_directory=as_directory):
                if as_directory:
                    redirection.mkdir()
                else:
                    redirection.write_text("redirect", encoding="utf-8")
                try:
                    with self.assertRaisesRegex(RuntimeError, r"\.local.*redirection"):
                        reject_runtime_dll_shadowing(
                            self.build_dir,
                            roots,
                            executables=(executable,),
                        )
                finally:
                    if redirection.is_dir():
                        redirection.rmdir()
                    else:
                        redirection.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "requires Windows PE resource APIs")
    def test_manifest_requirement_rejects_missing_or_external_only_manifest(self):
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        for external_manifest in (False, True):
            with self.subTest(external_manifest=external_manifest):
                executable = self.build_dir / f"no-embedded-{external_manifest}.exe"
                _write_pe_fixture(executable)
                if external_manifest:
                    Path(f"{executable}.manifest").write_text(
                        "<assembly manifestVersion='1.0'/>", encoding="utf-8"
                    )
                with self.assertRaisesRegex(RuntimeError, "embedded RT_MANIFEST"):
                    reject_runtime_dll_shadowing(
                        self.build_dir,
                        roots,
                        executables=(executable,),
                        require_embedded_manifests=True,
                    )

    @unittest.skipUnless(os.name == "nt", "requires Windows PE resource APIs")
    def test_embedded_manifest_keeps_post_audit_dotlocal_creation_disabled(self):
        executable = self.build_dir / "manifest-protected.exe"
        _write_manifest_pe_fixture(executable)
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with windows_runtime_guard.RuntimeAuditLease() as lease:
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
                lease=lease,
                require_embedded_manifests=True,
            )
            Path(f"{executable}.local").write_text("late", encoding="utf-8")
            self.assertTrue(
                windows_runtime_guard.has_embedded_application_manifest(executable)
            )

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_runtime_audit_lease_blocks_replacement_until_closed(self):
        executable = self.build_dir / "tst_held.exe"
        executable.write_bytes(b"pe-fixture")
        replacement = self.build_dir / "replacement.exe"
        replacement.write_bytes(b"replacement")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with windows_runtime_guard.RuntimeAuditLease() as lease:
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
                lease=lease,
            )
            with self.assertRaises(OSError):
                os.replace(replacement, executable)

        os.replace(replacement, executable)
        self.assertEqual(executable.read_bytes(), b"replacement")

    def test_runtime_audit_lease_attempts_every_close_after_one_fails(self):
        first = mock.MagicMock()
        second = mock.MagicMock()
        second.close.side_effect = KeyboardInterrupt("close interrupted")
        lease = windows_runtime_guard.RuntimeAuditLease()
        with mock.patch.object(
            lease,
            "_open_read_locked",
            side_effect=(first, second),
        ), mock.patch.object(
            lease,
            "_final_path_from_stream",
            side_effect=(self.build_dir / "first.exe", self.build_dir / "second.exe"),
        ):
            lease.hold(self.build_dir / "first.exe")
            lease.hold(self.build_dir / "second.exe")

        with self.assertRaisesRegex(KeyboardInterrupt, "close interrupted"):
            lease.close()

        first.close.assert_called_once_with()
        second.close.assert_called_once_with()

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_runtime_audit_lease_reuses_published_layout_and_preserves_logical_counts(
        self,
    ) -> None:
        cache_root = self.root / "layout-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        tool = self.build_dir / "tool.exe"
        tool.write_bytes(b"tool")
        entries = ((Path("tool.exe"), tool),)

        with RuntimeAuditLease(cache_root) as first:
            first_path = first.stage_readonly_layout(
                "tool", entries, context={"purpose": "unit"}
            )
            self.assertEqual((first.cache_hit_count, first.cache_miss_count), (0, 1))
            self.assertEqual((first.staged_file_count, first.staged_byte_count), (1, 4))
            self.assertEqual(
                (
                    first.payload_written_file_count,
                    first.payload_written_byte_count,
                ),
                (1, 4),
            )

        with RuntimeAuditLease(cache_root) as second:
            second_path = second.stage_readonly_layout(
                "tool", entries, context={"purpose": "unit"}
            )
            self.assertEqual(first_path, second_path)
            self.assertEqual((second.cache_hit_count, second.cache_miss_count), (1, 0))
            self.assertEqual((second.staged_file_count, second.staged_byte_count), (1, 4))
            self.assertEqual(
                (
                    second.payload_written_file_count,
                    second.payload_written_byte_count,
                ),
                (0, 0),
            )
            self.assertEqual(second.cached_layout_directories, (second_path,))

        self.assertTrue(second_path.is_dir())
        with self.assertRaises(OSError):
            (second_path / "late.dll").write_bytes(b"late")

        tool.write_bytes(b"tools")
        with RuntimeAuditLease(cache_root) as changed:
            changed_path = changed.stage_readonly_layout(
                "tool", entries, context={"purpose": "unit"}
            )
            self.assertNotEqual(changed_path, second_path)
            self.assertEqual((changed.cache_hit_count, changed.cache_miss_count), (0, 1))
            self.assertEqual((changed.staged_file_count, changed.staged_byte_count), (1, 5))
        self.assertEqual((second_path / "tool.exe").read_bytes(), b"tool")

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_runtime_audit_lease_caches_filtered_tree_with_stable_context(self) -> None:
        cache_root = self.root / "tree-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        source = self.root / "tree-source"
        source.mkdir()
        (source / "Module.qml").write_bytes(b"qml")
        (source / "qmldir").write_bytes(b"dir")
        (source / "ignored.txt").write_bytes(b"ignored")
        excluded = source / "excluded"
        excluded.mkdir()
        (excluded / "Hidden.qml").write_bytes(b"hidden")

        options = {
            "excluded": (excluded,),
            "included_suffixes": (".qml",),
            "included_names": ("qmldir",),
        }
        with RuntimeAuditLease(cache_root) as cold:
            cold_path = cold.stage_readonly_tree(source, **options)
            self.assertEqual((cold.cache_hit_count, cold.cache_miss_count), (0, 1))
            self.assertEqual((cold.staged_file_count, cold.staged_byte_count), (2, 6))
        with RuntimeAuditLease(cache_root) as warm:
            warm_path = warm.stage_readonly_tree(source, **options)
            self.assertEqual(warm_path, cold_path)
            self.assertEqual((warm.cache_hit_count, warm.cache_miss_count), (1, 0))
            self.assertEqual(
                (warm.payload_written_file_count, warm.payload_written_byte_count),
                (0, 0),
            )
        self.assertEqual(
            sorted(path.name for path in warm_path.iterdir()),
            ["Module.qml", "qmldir"],
        )
        with RuntimeAuditLease(cache_root) as changed_filter:
            changed_path = changed_filter.stage_readonly_tree(
                source,
                excluded=(excluded,),
            )
            self.assertNotEqual(changed_path, warm_path)
            self.assertEqual(
                (changed_filter.cache_hit_count, changed_filter.cache_miss_count),
                (0, 1),
            )
            self.assertTrue((changed_path / "ignored.txt").is_file())

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_runtime_audit_lease_batches_tree_source_descriptors(self) -> None:
        cache_root = self.root / "batched-tree-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        source = self.root / "batched-tree-source"
        source.mkdir()
        for index in range(40):
            (source / f"Module{index}.qml").write_bytes(
                f"qml-{index}".encode("ascii")
            )

        with RuntimeAuditLease(cache_root) as lease, mock.patch.object(
            lease,
            "_describe_held_sources",
            wraps=lease._describe_held_sources,
        ) as describe_many:
            staged = lease.stage_readonly_tree(source)

        describe_many.assert_called_once()
        self.assertEqual(len(tuple(staged.glob("*.qml"))), 40)

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_runtime_audit_lease_binds_dependencies_to_exact_cached_layouts(self) -> None:
        cache_root = self.root / "dependency-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        dependency_source = self.build_dir / "dependency.dll"
        dependency_source.write_bytes(b"dependency")

        with RuntimeAuditLease(cache_root) as lease:
            dependency = lease.stage_readonly_layout(
                "dependency",
                ((Path("dependency.dll"), dependency_source),),
                context={"kind": "dependency"},
            )
            child = lease.stage_readonly_layout(
                "child",
                ((Path("child.txt"), b"child"),),
                context={"kind": "child"},
                dependencies=(dependency,),
            )
            self.assertTrue(child.is_dir())
            self.assertEqual(
                lease.cached_layout_directories,
                (dependency, child),
            )
            with self.assertRaisesRegex(RuntimeError, "registered cached layout"):
                lease.stage_readonly_layout(
                    "invalid-child",
                    ((Path("child.txt"), b"child"),),
                    context={},
                    dependencies=(self.root / "not-cached",),
                )

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_runtime_audit_lease_failed_layout_does_not_increment_counters(self) -> None:
        cache_root = self.root / "failure-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        source = self.build_dir / "oversized.bin"
        source.write_bytes(b"too-large")

        with RuntimeAuditLease(cache_root) as lease, mock.patch.object(
            lease,
            "_describe_held_source",
            wraps=lease._describe_held_source,
        ) as describe:
            with self.assertRaisesRegex(RuntimeError, "byte limit"):
                lease.stage_readonly_layout(
                    "oversized",
                    ((Path("oversized.bin"), source),),
                    context={},
                    byte_limit=1,
                )
            self.assertEqual(lease.cached_layout_directories, ())
            self.assertEqual(
                (
                    lease.cache_hit_count,
                    lease.cache_miss_count,
                    lease.staged_file_count,
                    lease.staged_byte_count,
                    lease.payload_written_file_count,
                    lease.payload_written_byte_count,
                ),
                (0, 0, 0, 0, 0, 0),
            )
            describe.assert_not_called()
            self.assertFalse(cache_root.exists())

    @unittest.skipUnless(os.name == "nt", "requires protected Windows caching")
    def test_runtime_audit_lease_tree_limit_stops_before_hashing_extra_files(
        self,
    ) -> None:
        cache_root = self.root / "bounded-tree-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        source = self.root / "bounded-tree"
        source.mkdir()
        (source / "first.qml").write_bytes(b"first")
        (source / "second.qml").write_bytes(b"second")

        with RuntimeAuditLease(cache_root) as lease, mock.patch.object(
            lease,
            "_describe_held_source",
            wraps=lease._describe_held_source,
        ) as describe:
            with self.assertRaisesRegex(RuntimeError, "entry limit"):
                lease.stage_readonly_tree(source, entry_limit=1)
            self.assertEqual(describe.call_count, 0)
            self.assertEqual(lease.cached_layout_directories, ())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_mid_audit_runtime_peer_replacements_are_blocked_not_cached(self):
        configured = self.qt_root / "bin" / "Qt6Core.dll"
        configured.write_bytes(b"approved-qt-runtime")
        adjacent = self.build_dir / "Qt6Core.dll"
        adjacent.write_bytes(configured.read_bytes())
        executable = self.build_dir / "tst_recursive_qt.exe"
        executable.write_bytes(b"pe-fixture")
        adjacent_replacement = self.build_dir / "adjacent-replacement.dll"
        adjacent_replacement.write_bytes(b"changed-adjacent-runtime")
        configured_replacement = self.qt_root / "bin" / "configured-replacement.dll"
        configured_replacement.write_bytes(b"changed-configured-runtime")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        real_sha256 = windows_runtime_guard._sha256
        blocked: list[Path] = []
        hashed: list[Path] = []

        def hash_and_try_replace(path: Path, **kwargs) -> str:
            hashed.append(path)
            replacement = {
                adjacent: adjacent_replacement,
                configured: configured_replacement,
            }.get(path)
            if replacement is not None:
                with self.assertRaises(OSError):
                    os.replace(replacement, path)
                blocked.append(path)
            return real_sha256(path, **kwargs)

        with (
            windows_runtime_guard.RuntimeAuditLease() as lease,
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                side_effect=lambda path, **_kwargs: (
                    {"Qt6Core.dll"} if path == executable else set()
                ),
            ),
            mock.patch(
                "tools.windows_runtime_guard._sha256",
                side_effect=hash_and_try_replace,
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
                lease=lease,
            )

        self.assertEqual(blocked, [adjacent, configured])
        self.assertEqual(hashed.count(adjacent), 1)
        self.assertEqual(hashed.count(configured), 1)
        self.assertEqual(adjacent.read_bytes(), b"approved-qt-runtime")
        self.assertEqual(configured.read_bytes(), b"approved-qt-runtime")

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_runtime_audit_context_closes_handles_after_audit_failure(self):
        configured = self.qt_root / "bin" / "Qt6Core.dll"
        configured.write_bytes(b"configured")
        adjacent = self.build_dir / "Qt6Core.dll"
        adjacent.write_bytes(b"stale")
        replacement = self.build_dir / "replacement.dll"
        replacement.write_bytes(b"replacement")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with self.assertRaises(RuntimeError):
            with windows_runtime_guard.RuntimeAuditLease() as lease:
                reject_runtime_dll_shadowing(
                    self.build_dir,
                    roots,
                    lease=lease,
                )

        os.replace(replacement, adjacent)
        self.assertEqual(adjacent.read_bytes(), b"replacement")

    @unittest.skipUnless(os.name == "nt", "requires Windows ACLs")
    def test_staged_qml_tree_blocks_replacement_injection_and_retarget(self) -> None:
        source = self.qt_root / "qml"
        module = source / "Module.qml"
        module.write_text("import QtQuick\nItem {}\n", encoding="utf-8")
        nested = source / "Nested"
        nested.mkdir()
        (nested / "Nested.qml").write_text(
            "import QtQuick\nItem {}\n", encoding="utf-8"
        )
        replacement = self.root / "replacement.qml"
        replacement.write_text("import QtQuick\nQtObject {}\n", encoding="utf-8")

        with RuntimeAuditLease() as lease:
            staged = lease.stage_readonly_tree(source)
            staged_module = staged / module.relative_to(source)
            with self.assertRaises(OSError):
                staged_module.write_text("poison", encoding="utf-8")
            with self.assertRaises(OSError):
                os.replace(replacement, staged_module)
            with self.assertRaises(OSError):
                (staged / "Injected.qml").write_text("poison", encoding="utf-8")
            with self.assertRaises(OSError):
                (staged / "Nested" / "Injected.qml").write_text(
                    "poison", encoding="utf-8"
                )
            with self.assertRaises(OSError):
                os.replace(staged, staged.with_name("retargeted-qml"))

        self.assertTrue(staged.is_dir())
        with self.assertRaises(OSError):
            (staged / "late.qml").write_text("poison", encoding="utf-8")

    def test_earlier_non_owner_path_peer_cannot_shadow_authoritative_family(self):
        qt_peer = self.qt_root / "bin" / "avcodec-62.dll"
        qt_peer.write_bytes(b"qt-private-ffmpeg")
        authoritative = self.ffmpeg_root / "bin" / "avcodec-62.dll"
        authoritative.write_bytes(b"configured-ffmpeg")
        executable = self.build_dir / "tst_codec.exe"
        executable.write_bytes(b"pe-fixture")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with (
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                side_effect=lambda path, **_kwargs: (
                    {"avcodec-62.dll"} if path == executable else set()
                ),
            ),
            self.assertRaisesRegex(
                RuntimeError,
                rf"avcodec-62\.dll.*{re.escape(str(qt_peer))}.*"
                rf"{re.escape(str(authoritative))}",
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_repeated_provenance_checks_hash_each_runtime_peer_once(self):
        qt_peer = self.qt_root / "bin" / "avcodec-62.dll"
        qt_peer.write_bytes(b"identical-runtime")
        authoritative = self.ffmpeg_root / "bin" / "avcodec-62.dll"
        authoritative.write_bytes(qt_peer.read_bytes())
        executables = (
            self.build_dir / "tst_codec_a.exe",
            self.build_dir / "tst_codec_b.exe",
        )
        for executable in executables:
            executable.write_bytes(b"pe-fixture")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )
        real_sha256 = windows_runtime_guard._sha256

        with (
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                side_effect=lambda path, **_kwargs: (
                    {"avcodec-62.dll"} if path in executables else set()
                ),
            ),
            mock.patch(
                "tools.windows_runtime_guard._sha256",
                wraps=real_sha256,
            ) as sha256,
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=executables,
            )

        hashed = [call.args[0] for call in sha256.call_args_list]
        self.assertEqual(hashed.count(qt_peer), 1)
        self.assertEqual(hashed.count(authoritative), 1)

    def test_configured_runtime_dll_import_closure_is_audited(self):
        codec = self.ffmpeg_root / "bin" / "avcodec-62.dll"
        codec.write_bytes(b"pe-fixture")
        active_util = self.ffmpeg_root / "bin" / "avutil-60.dll"
        active_util.write_bytes(b"pe-fixture")
        executable = self.build_dir / "tst_codec.exe"
        executable.write_bytes(b"pe-fixture")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        def imports(path: Path, **_kwargs) -> set[str]:
            if path == executable:
                return {"avcodec-62.dll"}
            if path == codec:
                return {"avutil-59.dll"}
            return set()

        with (
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                side_effect=imports,
            ),
            self.assertRaisesRegex(
                RuntimeError,
                r"avcodec-62\.dll.*avutil-59\.dll.*OLR_FFMPEG_ROOT.*avutil-60\.dll",
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_unrelated_imported_app_dll_remains_allowed(self):
        executable = self.build_dir / "tst_app_owned.exe"
        executable.write_bytes(b"pe-fixture")
        app_owned = self.build_dir / "OpenLiveReplaySupport.dll"
        app_owned.write_bytes(b"app-owned")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with mock.patch(
            "tools.windows_runtime_guard.pe_imports",
            side_effect=lambda path, **_kwargs: (
                {"OpenLiveReplaySupport.dll"} if path == executable else set()
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_nested_test_directory_rejects_stale_adjacent_qt_dll(self):
        configured = self.qt_root / "bin" / "Qt6Core.dll"
        configured.write_bytes(b"active-qt")
        test_dir = self.build_dir / "tests" / "nested"
        test_dir.mkdir(parents=True)
        executable = test_dir / "tst_nested.exe"
        executable.write_bytes(b"pe-fixture")
        adjacent = test_dir / "Qt6Core.dll"
        adjacent.write_bytes(b"stale-qt")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        with (
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                return_value={"qt6core.dll"},
            ),
            self.assertRaisesRegex(
                RuntimeError,
                rf"{re.escape(str(adjacent))}.*{re.escape(str(configured))}",
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_nested_app_owned_import_closure_is_audited(self):
        active = self.ffmpeg_root / "bin" / "avcodec-62.dll"
        active.write_bytes(b"active-ffmpeg")
        test_dir = self.build_dir / "tests" / "nested"
        test_dir.mkdir(parents=True)
        executable = test_dir / "tst_nested.exe"
        executable.write_bytes(b"pe-fixture")
        support = test_dir / "OpenLiveReplaySupport.dll"
        support.write_bytes(b"pe-fixture")
        roots = RuntimeRoots(
            self.compiler_bin,
            self.qt_root / "bin",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
        )

        def imports(path: Path, **_kwargs) -> set[str]:
            if path == executable:
                return {support.name}
            if path == support:
                return {"avcodec-61.dll"}
            return set()

        with (
            mock.patch(
                "tools.windows_runtime_guard.pe_imports",
                side_effect=imports,
            ),
            self.assertRaisesRegex(
                RuntimeError,
                r"OpenLiveReplaySupport\.dll.*avcodec-61\.dll.*OLR_FFMPEG_ROOT",
            ),
        ):
            reject_runtime_dll_shadowing(
                self.build_dir,
                roots,
                executables=(executable,),
            )

    def test_windows_runtime_stale_cache_error_requires_reconfigure_guidance(self):
        shutil.rmtree(self.qt_root / "bin")
        with self.assertRaisesRegex(
            FileNotFoundError,
            r"Qt runtime.*Qt6_DIR.*reconfigure or recreate",
        ):
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

    @mock.patch("tools.run_ctest._restore_windows_dll_directory")
    @mock.patch("tools.run_ctest._set_windows_dll_directory", return_value="C:/prior")
    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=17)
    @mock.patch("tools.run_ctest.run_process_tree")
    def test_main_uses_ephemeral_writable_controller_and_immutable_test_cwd(
        self,
        run: mock.Mock,
        _set_error_mode: mock.Mock,
        _restore_error_mode: mock.Mock,
        _set_dll_directory: mock.Mock,
        _restore_dll_directory: mock.Mock,
    ) -> None:
        self.write_cacheable_ctest_graph("unit")
        controllers: list[Path] = []
        ambient_config = self.root / "ambient-cmake-config"
        ambient_query = ambient_config / "instrumentation" / "v1" / "query"
        ambient_query.mkdir(parents=True)
        (ambient_query / "callback.json").write_text(
            '{"version":1,"callbacks":["C:/attacker.exe"],'
            '"hooks":["postCTest"]}\n',
            encoding="utf-8",
        )

        def inspect_controller(command, **kwargs) -> int:
            controller = Path(command[-3])
            controllers.append(controller)
            graph = controller / "CTestTestfile.cmake"
            self.assertTrue(graph.is_file())
            self.assertEqual(tuple(controller.rglob("*.exe")), ())
            self.assertFalse((controller / "CMakeCache.txt").exists())
            temporary = controller / "Testing" / "Temporary"
            self.assertEqual(Path(kwargs["env"]["TMPDIR"]), temporary)
            config = Path(kwargs["env"]["CMAKE_CONFIG_DIR"])
            self.assertEqual(config, controller / "cmake-config")
            self.assertTrue(config.is_dir())
            self.assertEqual(tuple(config.rglob("*")), ())
            self.assertNotEqual(config, ambient_config)
            self.assertNotIn("attacker", "\n".join(kwargs["env"].values()).casefold())
            for name in (
                "CTEST_USE_INSTRUMENTATION",
                "CTEST_USE_VERBOSE_INSTRUMENTATION",
                "CTEST_PARALLEL_LEVEL",
            ):
                self.assertNotIn(name.casefold(), {
                    key.casefold() for key in kwargs["env"]
                })
            with self.assertRaises(OSError):
                (config / "instrumentation" / "v1" / "query").mkdir(
                    parents=True
                )
            shell_temporary = Path(tempfile.mkdtemp(dir=kwargs["env"]["TMPDIR"]))
            self.assertEqual(shell_temporary.parent, temporary)
            shell_temporary.rmdir()
            (temporary / "LastTest.log").write_text("ctest log\n", encoding="utf-8")
            with self.assertRaises(OSError):
                (controller / "CTestCustom.cmake").write_text(
                    "poison\n", encoding="utf-8"
                )
            for graph_directory in {
                path.parent for path in controller.rglob("CTestTestfile.cmake")
            }:
                with self.assertRaises(OSError, msg=str(graph_directory)):
                    (graph_directory / "injected.ctest").write_text(
                        "poison\n", encoding="utf-8"
                    )

            replacement = temporary / "replacement.cmake"
            replacement.write_text("poison\n", encoding="utf-8")
            with self.assertRaises(OSError):
                os.replace(replacement, graph)

            rendered = graph.read_text(encoding="utf-8")
            working_match = re.search(
                r"WORKING_DIRECTORY \[\[(.*?)\]\]", rendered, re.DOTALL
            )
            self.assertIsNotNone(working_match)
            self.assert_directory_rejects_late_insertion(
                Path(working_match.group(1))
            )
            return 0

        run.side_effect = inspect_controller
        with mock.patch.dict(
            os.environ,
            {
                "CmAkE_CoNfIg_DiR": str(ambient_config),
                "cTeSt_UsE_InStRuMeNtAtIoN": "1",
                "CtEsT_UsE_VeRbOsE_InStRuMeNtAtIoN": "1",
                "cTeSt_PaRaLlEl_LeVeL": "0",
            },
        ):
            self.assertEqual(
                run_ctest.main(
                    ["--test-dir", str(self.build_dir), "-R", "^unit$"]
                ),
                0,
            )

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_injection_before_final_seal(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-injection-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def inject(controller: Path) -> None:
                controllers.append(controller)
                (controller / "CTestCustom.cmake").write_text(
                    "poison\n", encoding="utf-8"
                )

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_controller_seal",
                    side_effect=inject,
                    create=True,
                ),
                self.assertRaisesRegex(RuntimeError, "unexpected CTest controller"),
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_expected_graph_substitution(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-graph-substitution-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def substitute(_source: Path, destination: Path) -> None:
                controllers.append(destination.parent)
                replacement = destination.with_name("replacement-graph.cmake")
                replacement.write_text("poison\n", encoding="utf-8")
                os.replace(replacement, destination)

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_graph_hold",
                    side_effect=substitute,
                    create=True,
                ),
                self.assertRaises(OSError),
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_expected_graph_mutation(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-graph-mutation-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def mutate(_source: Path, destination: Path) -> None:
                controllers.append(destination.parent)
                destination.write_text("poison\n", encoding="utf-8")

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_graph_hold",
                    side_effect=mutate,
                    create=True,
                ),
                self.assertRaises(OSError),
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_same_byte_graph_replacement(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-same-byte-graph-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def substitute(source: Path, destination: Path) -> None:
                controllers.append(destination.parent)
                replacement = destination.with_name("same-byte-graph.cmake")
                replacement.write_bytes(source.read_bytes())
                os.replace(replacement, destination)

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_graph_hold",
                    side_effect=substitute,
                ),
                self.assertRaises(OSError),
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_transient_graph_mutation(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-transient-graph-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def mutate_then_restore(source: Path, destination: Path) -> None:
                controllers.append(destination.parent)
                original = source.read_bytes()
                with destination.open("r+b") as stream:
                    stream.write(b"poison\n")
                    stream.truncate()
                    stream.flush()
                    os.fsync(stream.fileno())
                    stream.seek(0)
                    stream.write(original)
                    stream.truncate()
                    stream.flush()
                    os.fsync(stream.fileno())

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_graph_hold",
                    side_effect=mutate_then_restore,
                ),
                self.assertRaises(OSError),
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows handle semantics")
    def test_ctest_controller_graph_handle_is_not_inheritable(self) -> None:
        import msvcrt

        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-handle-inheritance-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        inherited: list[bool] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def inspect_handle(_source: Path, destination: Path) -> None:
                controllers.append(destination.parent)
                stream = lease._streams[destination.absolute()]
                handle = msvcrt.get_osfhandle(stream.fileno())
                inherited.append(os.get_handle_inheritable(handle))

            with mock.patch.object(
                lease,
                "_before_ctest_graph_hold",
                side_effect=inspect_handle,
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(inherited, [False])
        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_expected_graph_hardlink(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-graph-hardlink-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def hardlink(source: Path, destination: Path) -> None:
                del source
                controllers.append(destination.parent)
                alias = destination.with_name("graph-alias.cmake")
                os.link(destination, alias)

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_graph_hold",
                    side_effect=hardlink,
                    create=True,
                ),
                self.assertRaisesRegex(RuntimeError, "file alias"),
            ):
                lease.create_ctest_controller()
        finally:
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    def test_ctest_controller_rejects_preopened_directory_writer(self) -> None:
        self.write_cacheable_ctest_graph("unit")
        cache_root = self.root / "controller-writer-cache"
        self.addCleanup(self.remove_test_runtime_cache, cache_root)
        controllers: list[Path] = []
        writer_handles: list[int] = []
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        lease = RuntimeAuditLease(cache_root)
        try:
            run_ctest.prepare_windows_runtime(
                self.build_dir,
                {},
                audit_lease=lease,
                ctest_args=("-R", "^unit$"),
            )

            def open_writer(controller: Path) -> None:
                controllers.append(controller)
                handle = kernel32.CreateFileW(
                    str(controller),
                    0x00000002,  # FILE_ADD_FILE on a directory
                    0x00000001 | 0x00000002 | 0x00000004,
                    None,
                    3,
                    0x02000000,
                    None,
                )
                self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
                writer_handles.append(int(handle))

            with (
                mock.patch.object(
                    lease,
                    "_before_ctest_controller_seal",
                    side_effect=open_writer,
                    create=True,
                ),
                self.assertRaises(OSError),
            ):
                lease.create_ctest_controller()
        finally:
            for handle in writer_handles:
                kernel32.CloseHandle(handle)
            lease.close()

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())

    @mock.patch("tools.run_ctest._restore_windows_dll_directory")
    @mock.patch("tools.run_ctest._set_windows_dll_directory", return_value="C:/prior")
    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=17)
    @mock.patch("tools.run_ctest.run_process_tree")
    def test_main_normalizes_test_dir_and_appends_final_zero(
        self,
        run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
        set_dll_directory: mock.Mock,
        restore_dll_directory: mock.Mock,
    ):
        self.write_cacheable_ctest_graph("unit")
        run.return_value = 0
        caller_args = [
            "--interactive-debug-mode",
            "1",
            "--test-dir",
            str(self.build_dir),
            "-R",
            "--interactive-debug-mode=1",
            "--interactive-debug-mode=1",
            "--interactive-debug-mode",
            "1",
            "-L",
            "unit",
        ]

        result = run_ctest.main(caller_args)

        self.assertEqual(result, 0)
        command = run.call_args.args[0]
        forwarded_args = caller_args[:2] + caller_args[4:]
        self.assertEqual(command[1 : 1 + len(forwarded_args)], forwarded_args)
        self.assertNotIn(str(self.build_dir), command)
        self.assertEqual(
            command[-4::2], ["--test-dir", "--interactive-debug-mode"]
        )
        self.assertEqual(command[-1], "0")
        self.assertNotEqual(Path(command[0]), self.ctest)
        self.assertTrue(run.call_args.kwargs["enabled"])
        self.assertTrue(run.call_args.kwargs.get("private_desktop"))
        runtime = set_dll_directory.call_args.args[0]
        self.assertNotEqual(Path(command[0]).parent, self.ctest.parent)
        self.assertEqual(
            Path(run.call_args.kwargs["env"]["PATH"].split(os.pathsep)[0]).parents[1],
            runtime.parent,
        )
        self.assertNotEqual(
            Path(run.call_args.kwargs["env"]["QT_PLUGIN_PATH"]),
            self.qt_root / "plugins",
        )
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(17)
        restore_dll_directory.assert_called_once_with("C:/prior")

    @unittest.skipUnless(os.name == "nt", "requires Windows junctions")
    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=9)
    @mock.patch("tools.run_ctest.run_process_tree", return_value=0)
    def test_main_appends_canonical_test_dir_after_caller_alias(
        self,
        run: mock.Mock,
        _set_error_mode: mock.Mock,
        _restore_error_mode: mock.Mock,
    ) -> None:
        self.write_cacheable_ctest_graph("unit")
        alias = self.root / "build-alias"
        self.make_junction(alias, self.build_dir)

        self.assertEqual(run_ctest.main(["--test-dir", str(alias)]), 0)

        command = run.call_args.args[0]
        self.assertEqual(command[-4], "--test-dir")
        staged_build = Path(command[-3])
        self.assertNotEqual(staged_build, self.build_dir.resolve())
        self.assertNotEqual(staged_build, alias)
        self.assertFalse(staged_build.exists())
        self.assertEqual(command[-2:], ["--interactive-debug-mode", "0"])

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=13)
    @mock.patch("tools.run_ctest.run_process_tree")
    def test_main_consumes_interactive_spelling_as_test_dir_value(
        self,
        run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
    ):
        run.return_value = 0
        named_build_dir = self.root / "--interactive-debug-mode=1"
        named_build_dir.mkdir()
        self.build_dir = named_build_dir
        self.write_cache()
        (self.build_dir / "CTestTestfile.cmake").write_text(
            "", encoding="utf-8"
        )
        caller_args = ["--test-dir", "--interactive-debug-mode=1"]

        with mock.patch.object(run_ctest.Path, "cwd", return_value=self.root):
            result = run_ctest.main(caller_args)

        self.assertEqual(result, 0)
        command = run.call_args.args[0]
        self.assertNotIn("--interactive-debug-mode=1", command)
        self.assertEqual(command[-4], "--test-dir")
        self.assertNotEqual(Path(command[-3]), self.build_dir.resolve())
        self.assertEqual(command[-2:], ["--interactive-debug-mode", "0"])
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(13)

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=41)
    @mock.patch("tools.run_ctest.run_process_tree", return_value=23)
    def test_main_propagates_process_tree_exit_code(
        self,
        run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
    ):
        self.write_cacheable_ctest_graph("unit")
        result = run_ctest.main(["--test-dir", str(self.build_dir)])

        self.assertEqual(result, 23)
        run.assert_called_once()
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(41)

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=29)
    @mock.patch("tools.run_ctest.run_process_tree")
    def test_main_restores_error_mode_when_child_launch_raises(
        self,
        run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
    ):
        self.write_cacheable_ctest_graph("unit")
        controllers: list[Path] = []

        def fail_launch(command, **_kwargs):
            controllers.append(Path(command[-3]))
            raise OSError("launch failed")

        run.side_effect = fail_launch
        with self.assertRaisesRegex(OSError, "launch failed"):
            run_ctest.main(["--test-dir", str(self.build_dir)])

        self.assertEqual(len(controllers), 1)
        self.assertFalse(controllers[0].exists())
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(29)

    @unittest.skipUnless(os.name == "nt", "requires Windows sharing semantics")
    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=31)
    def test_main_holds_audited_ctest_until_process_tree_returns(
        self,
        _set_error_mode: mock.Mock,
        _restore_error_mode: mock.Mock,
    ) -> None:
        self.write_cacheable_ctest_graph("unit")
        replacement = self.cmake_bin / "replacement-ctest.exe"
        replacement.write_bytes(b"replacement")

        def inspect_held_file(*_args, **_kwargs) -> int:
            with self.assertRaises(OSError):
                os.replace(replacement, self.ctest)
            return 0

        with mock.patch(
            "tools.run_ctest.run_process_tree", side_effect=inspect_held_file
        ):
            self.assertEqual(
                run_ctest.main(["--test-dir", str(self.build_dir)]),
                0,
            )

        os.replace(replacement, self.ctest)
        self.assertEqual(self.ctest.read_bytes(), b"replacement")

    def test_windows_error_mode_uses_exact_mask_and_restore_value(self):
        set_error_mode = mock.Mock(return_value=73)
        windll = SimpleNamespace(
            kernel32=SimpleNamespace(SetErrorMode=set_error_mode)
        )

        with mock.patch.object(run_ctest.ctypes, "windll", windll, create=True):
            old_mode = run_ctest._set_windows_error_mode()
            run_ctest._restore_windows_error_mode(old_mode)

        self.assertEqual(old_mode, 73)
        self.assertEqual(
            set_error_mode.call_args_list,
            [mock.call(0x8003), mock.call(73)],
        )


if __name__ == "__main__":
    unittest.main()
