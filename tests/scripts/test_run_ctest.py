"""Unit tests for the build-derived Windows CTest launcher."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import run_ctest, windows_runtime_guard
from tools.windows_runtime_guard import (
    RuntimeRoots,
    pe_imports,
    registered_test_executables,
    reject_runtime_dll_shadowing,
)


def _write_pe_fixture(
    path: Path,
    *,
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
    name = b"fixture-runtime.dll\0"
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
            self.qt_root / "qml",
            self.ffmpeg_root / "bin",
            self.ffmpeg_root / "lib",
            self.srt_root / "bin",
            self.srt_root / "lib",
            self.cmake_bin,
        ):
            runtime_dir.mkdir(parents=True)
        _write_manifest_pe_fixture(self.ctest)
        self.cache_values = {
            "CMAKE_CXX_COMPILER": str(self.compiler_bin / "g++.exe"),
            "CMAKE_COMMAND": str(self.cmake_bin / "cmake.exe"),
            "CMAKE_CTEST_COMMAND": str(self.ctest),
            "Qt6_DIR": str(self.qt_root / "lib" / "cmake" / "Qt6"),
            "OLR_FFMPEG_ROOT": str(self.ffmpeg_root),
            "OLR_SRT_ROOT": str(self.srt_root),
        }
        self.write_cache()

    def write_cache(self) -> None:
        lines = [f"{key}:FILEPATH={value}" for key, value in self.cache_values.items()]
        (self.build_dir / "CMakeCache.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8"
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

        self.assertEqual(
            registered_test_executables(self.build_dir),
            (bracket_executable, helper_executable, test_executable),
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

    def test_registered_test_executables_fail_closed_on_build_lexical_escape(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "tst_escaped.exe").write_bytes(b"pe-fixture")
        junction = self.build_dir / "junction"
        (self.build_dir / "CTestTestfile.cmake").write_text(
            'add_test([=[escaped]=] "junction/tst_escaped.exe")\n',
            encoding="utf-8",
        )

        escaped = junction / "tst_escaped.exe"
        real_resolve = Path.resolve

        def resolve(path: Path, *args, **kwargs) -> Path:
            if Path(os.path.abspath(path)) == escaped:
                return outside / "tst_escaped.exe"
            return real_resolve(path, *args, **kwargs)

        with (
            mock.patch.object(Path, "resolve", autospec=True, side_effect=resolve),
            self.assertRaisesRegex(RuntimeError, "resolves outside"),
        ):
            registered_test_executables(self.build_dir)

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

    def test_windows_runtime_precedes_inherited_path(self):
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
        self.assertEqual(env["PATH"].split(os.pathsep)[6:], [str(self.ambient_bin)])
        self.assertEqual(ctest, self.ctest)

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
                    path.rmdir()
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

    def test_windows_runtime_sets_controlled_qt_defaults(self):
        inherited = {"PATH": str(self.ambient_bin)}

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt_root / "plugins"))
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
        self.assertNotIn("QT_PLUGIN_PATH", inherited)
        self.assertNotIn("QT_QPA_PLATFORM", inherited)

    def test_windows_runtime_prepends_qt_qml_and_preserves_inherited_paths(self):
        inherited_qml = self.root / "inherited-qml"
        inherited_qml2 = self.root / "inherited-qml2"
        inherited = {
            "QML_IMPORT_PATH": str(inherited_qml),
            "QML2_IMPORT_PATH": str(inherited_qml2),
        }

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        self.assertEqual(
            env["QML_IMPORT_PATH"].split(os.pathsep),
            [str(self.qt_root / "qml"), str(inherited_qml)],
        )
        self.assertEqual(
            env["QML2_IMPORT_PATH"].split(os.pathsep),
            [str(self.qt_root / "qml"), str(inherited_qml2)],
        )
        self.assertEqual(inherited["QML_IMPORT_PATH"], str(inherited_qml))
        self.assertEqual(inherited["QML2_IMPORT_PATH"], str(inherited_qml2))

    def test_windows_runtime_preserves_explicit_qpa_platform(self):
        env, _ = run_ctest.prepare_windows_runtime(
            self.build_dir,
            {"PATH": str(self.ambient_bin), "QT_QPA_PLATFORM": "windows"},
        )

        self.assertEqual(env["QT_QPA_PLATFORM"], "windows")

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
        ):
            lease.hold(self.build_dir / "first.exe")
            lease.hold(self.build_dir / "second.exe")

        with self.assertRaisesRegex(KeyboardInterrupt, "close interrupted"):
            lease.close()

        first.close.assert_called_once_with()
        second.close.assert_called_once_with()

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

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=17)
    @mock.patch("tools.run_ctest.run_process_tree")
    def test_main_preserves_hostile_arguments_and_appends_final_zero(
        self,
        run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
    ):
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
        self.assertEqual(
            command,
            [
                str(self.ctest),
                *caller_args,
                "--interactive-debug-mode",
                "0",
            ],
        )
        self.assertTrue(run.call_args.kwargs["enabled"])
        self.assertEqual(
            run.call_args.kwargs["env"]["QT_PLUGIN_PATH"],
            str(self.qt_root / "plugins"),
        )
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(17)

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=13)
    @mock.patch("tools.run_ctest.run_process_tree")
    def test_main_preserves_interactive_spelling_as_test_dir_value(
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
        caller_args = ["--test-dir", "--interactive-debug-mode=1"]

        with mock.patch.object(run_ctest.Path, "cwd", return_value=self.root):
            result = run_ctest.main(caller_args)

        self.assertEqual(result, 0)
        self.assertEqual(
            run.call_args.args[0],
            [str(self.ctest), *caller_args, "--interactive-debug-mode", "0"],
        )
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
        result = run_ctest.main(["--test-dir", str(self.build_dir)])

        self.assertEqual(result, 23)
        run.assert_called_once()
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(41)

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=29)
    @mock.patch("tools.run_ctest.run_process_tree", side_effect=OSError("launch failed"))
    def test_main_restores_error_mode_when_child_launch_raises(
        self,
        _run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
    ):
        with self.assertRaisesRegex(OSError, "launch failed"):
            run_ctest.main(["--test-dir", str(self.build_dir)])

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
