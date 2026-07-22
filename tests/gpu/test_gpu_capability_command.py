from __future__ import annotations

import ast
import ctypes
import dataclasses
import inspect
import locale
import os
import re
import subprocess
import struct
import sys
import tempfile
import threading
import time
import tracemalloc
import unittest
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_capability_command as capability_command  # noqa: E402
from gpu_capability_command import (  # noqa: E402
    RewrittenCommand,
    _clear_compiler_inspection_memo_for_tests,
    _compiler_metadata_snapshot,
    decode_compile_entry,
    expand_response_files,
    identify_compiler,
    inspect_compiler,
    make_configuration,
    open_compiler_executable_capability,
    rewrite_preprocess_command,
    strip_launchers,
    classify_command_rewrite,
    compiler_digest,
    decision_environment_digest,
    normalize_decision_arguments,
)
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompilerExecutableCapability,
    CompilerFamily,
    DependencyRootBinding,
    FileIdentity,
    PreprocessConfiguration,
    build_dependency_root_authority,
)


class CompileEntryDecodeTests(unittest.TestCase):
    def test_command_module_has_no_optimization_sensitive_assertions(self):
        path = Path(__file__).resolve().with_name("gpu_capability_command.py")
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=path.name)
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(tree)))

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.database = self.root / "build" / "compile_commands.json"
        self.database.parent.mkdir()

    def tearDown(self) -> None:
        _clear_compiler_inspection_memo_for_tests()
        self.temporary.cleanup()

    def test_structured_arguments_are_preferred_and_directory_is_canonical(self):
        (self.root / "source").mkdir()
        entry = {
            "directory": "../source",
            "file": "file.cpp",
            "arguments": ["g++", "-DNAME=structured", "file.cpp"],
            "command": "g++ -DNAME=string file.cpp",
        }
        cwd, arguments = decode_compile_entry(entry, self.database, windows=False)
        self.assertEqual(cwd, (self.database.parent / "../source").resolve())
        self.assertEqual(arguments, ("g++", "-DNAME=structured", "file.cpp"))

    def test_posix_command_uses_shell_word_quoting_without_a_shell(self):
        entry = {
            "directory": str(self.root),
            "file": "source file.cpp",
            "command": "g++ '-DSAY=two words' -I\"SDK Path/include\" 'source file.cpp'",
        }
        _cwd, arguments = decode_compile_entry(entry, self.database, windows=False)
        self.assertEqual(
            arguments,
            ("g++", "-DSAY=two words", "-ISDK Path/include", "source file.cpp"),
        )

    def test_windows_command_uses_command_line_to_argv_w_quoting(self):
        entry = {
            "directory": str(self.root),
            "file": r"src\my file.cpp",
            "command": (
                r'"C:\Program Files\ccache\ccache.exe" '
                r'--config-path "ccache config.conf" '
                r'"C:\Program Files\LLVM\clang-cl.exe" /c '
                r'"src\my file.cpp" "quote\\\"value"'
            ),
        }
        _cwd, arguments = decode_compile_entry(entry, self.database, windows=True)
        self.assertEqual(
            arguments,
            (
                r"C:\Program Files\ccache\ccache.exe",
                "--config-path",
                "ccache config.conf",
                r"C:\Program Files\LLVM\clang-cl.exe",
                "/c",
                r"src\my file.cpp",
                'quote\\"value',
            ),
        )

    def test_rejects_shell_operators_redirection_and_command_substitution(self):
        commands = (
            "g++ file.cpp | tee output",
            "g++ file.cpp || true",
            "g++ file.cpp && echo done",
            "g++ file.cpp ; echo done",
            "g++ file.cpp > output.i",
            "g++ file.cpp 2> errors.txt",
            "g++ $(pick-compiler) file.cpp",
            "g++ `pick-compiler` file.cpp",
        )
        for command in commands:
            with self.subTest(command=command), self.assertRaisesRegex(
                AuditInfrastructureError, "shell syntax"
            ):
                decode_compile_entry(
                    {"directory": str(self.root), "file": "file.cpp", "command": command},
                    self.database,
                    windows=False,
                )

    def test_rejects_attached_posix_shell_operators_but_allows_quoted_literals(self):
        for command in (
            "g++ main.cpp ;post-step",
            "g++ main.cpp &&post-step",
            "g++ main.cpp ||post-step",
            "g++ main.cpp -DVALUE>/dev/null",
        ):
            with self.subTest(command=command), self.assertRaisesRegex(
                AuditInfrastructureError, "shell syntax"
            ):
                decode_compile_entry(
                    {"directory": str(self.root), "file": "main.cpp", "command": command},
                    self.database,
                    windows=False,
                )
        _cwd, arguments = decode_compile_entry(
            {
                "directory": str(self.root),
                "file": "main.cpp",
                "command": "g++ '-DVALUE=>;&&literal' main.cpp",
            },
            self.database,
            windows=False,
        )
        self.assertEqual(arguments[1], "-DVALUE=>;&&literal")

    def test_rejects_empty_or_malformed_entries(self):
        entries = (
            {"directory": str(self.root), "file": "file.cpp", "arguments": []},
            {"directory": str(self.root), "file": "file.cpp", "command": ""},
            {"directory": str(self.root), "file": "file.cpp"},
            {"directory": str(self.root), "file": "file.cpp", "arguments": "g++ file.cpp"},
            {"directory": str(self.root), "file": "file.cpp", "arguments": ["g++", 7]},
            {"directory": 7, "file": "file.cpp", "arguments": ["g++", "file.cpp"]},
        )
        for entry in entries:
            with self.subTest(entry=entry), self.assertRaises(AuditInfrastructureError):
                decode_compile_entry(entry, self.database, windows=False)


class LauncherTests(unittest.TestCase):
    def test_ccache_assignments_and_options_are_stripped(self):
        compiler, arguments, assignments = strip_launchers((
            "ccache", "compiler_check=content", "--config-path", "cache.conf",
            "C:/Qt/Tools/mingw1310_64/bin/g++.exe", "-c", "playback/gpu/file.cpp"))
        self.assertEqual(compiler.name, "g++.exe")
        self.assertEqual(arguments[-2:], ("-c", "playback/gpu/file.cpp"))
        self.assertEqual(assignments, {"compiler_check": "content"})

    def test_ccache_documented_value_and_flag_forms_are_stripped(self):
        options = (
            "--compiler-check", "content",
            "--config-path=config", "--dir", "cache", "--namespace", "gpu",
            "--set-config", "sloppiness=time_macros", "--trim-dir", "root",
            "-o", "stats.log", "--ccache-skip", "--",
        )
        compiler, arguments, assignments = strip_launchers(
            ("ccache", *options, "clang++", "-c", "file.cpp")
        )
        self.assertEqual(compiler, Path("clang++"))
        self.assertEqual(arguments, ("-c", "file.cpp"))
        self.assertEqual(assignments, {})

    def test_supported_launcher_chain_is_removed(self):
        compiler, arguments, assignments = strip_launchers((
            "sccache.exe", "distcc", "icecc", "ccache", "base_dir=C:/repo",
            "g++", "-DVALUE=1", "file.cpp"))
        self.assertEqual(compiler, Path("g++"))
        self.assertEqual(arguments, ("-DVALUE=1", "file.cpp"))
        self.assertEqual(assignments, {"base_dir": "C:/repo"})

    def test_unknown_launcher_option_fails(self):
        with self.assertRaisesRegex(AuditInfrastructureError, "unsupported ccache option.*--mystery"):
            strip_launchers(("ccache", "--mystery", "g++", "file.cpp"))

    def test_ccache_compiler_overrides_fail_closed(self):
        controls = (
            ("ccache", "compiler=clang++", "g++", "file.cpp"),
            ("ccache", "compiler_type=clang", "g++", "file.cpp"),
            ("ccache", "--compiler", "clang++", "g++", "file.cpp"),
            ("ccache", "--compiler-type=clang", "g++", "file.cpp"),
        )
        for arguments in controls:
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, "ccache compiler override"
            ):
                strip_launchers(arguments)

    def test_invalid_assignment_and_missing_values_fail(self):
        controls = (
            (("ccache", "9bad=value", "g++", "file.cpp"), "9bad=value"),
            (("ccache", "--config-path"), "requires a value"),
            (("ccache", "--"), "no compiler"),
            (("sccache",), "no compiler"),
            ((), "no executable"),
        )
        for arguments, message in controls:
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                strip_launchers(arguments)


class CompilerIdentificationTests(unittest.TestCase):
    @staticmethod
    def elf_runtime_image(
        *, needed: tuple[str, ...] = (), rpath: tuple[str, ...] = (),
        runpath: tuple[str, ...] = (),
    ) -> bytes:
        strings = bytearray(b"\0")
        offsets = {}
        for value in (*needed, *rpath, *runpath):
            if value not in offsets:
                offsets[value] = len(strings)
                strings.extend(value.encode("utf-8") + b"\0")
        entries = [(1, offsets[value]) for value in needed]
        entries.extend((15, offsets[value]) for value in rpath)
        entries.extend((29, offsets[value]) for value in runpath)
        entries.extend(((5, 0x400200), (0, 0)))
        dynamic = b"".join(struct.pack("<qQ", *entry) for entry in entries)
        image = bytearray(0x200 + len(strings))
        ident = b"\x7fELF" + bytes((2, 1, 1)) + b"\0" * 9
        struct.pack_into(
            "<16sHHIQQQIHHHHHH", image, 0, ident, 2, 0x3E, 1,
            0, 64, 0, 0, 64, 56, 2, 0, 0, 0,
        )
        struct.pack_into(
            "<IIQQQQQQ", image, 64, 1, 4, 0x200, 0x400200, 0,
            len(strings), len(strings), 1,
        )
        struct.pack_into(
            "<IIQQQQQQ", image, 120, 2, 4, 0x100, 0, 0,
            len(dynamic), len(dynamic), 8,
        )
        image[0x100:0x100 + len(dynamic)] = dynamic
        image[0x200:] = strings
        return bytes(image)

    @staticmethod
    def elf_interp_runtime_image(interpreter: str, *, terminated: bool = True) -> bytes:
        encoded = interpreter.encode("utf-8") + (b"\0" if terminated else b"")
        image = bytearray(0x100 + len(encoded))
        ident = b"\x7fELF" + bytes((2, 1, 1)) + b"\0" * 9
        struct.pack_into(
            "<16sHHIQQQIHHHHHH", image, 0, ident, 2, 0x3E, 1,
            0, 64, 0, 0, 64, 56, 1, 0, 0, 0,
        )
        struct.pack_into(
            "<IIQQQQQQ", image, 64, 3, 4, 0x100, 0, 0,
            len(encoded), len(encoded), 1,
        )
        image[0x100:] = encoded
        return bytes(image)

    @staticmethod
    def pe_delay_import_runtime_image(name: str, *, valid_name_rva: bool = True) -> bytes:
        encoded = name.encode("utf-8") + b"\0"
        image = bytearray(0x300)
        image[0:2] = b"MZ"
        struct.pack_into("<I", image, 0x3C, 0x80)
        image[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", image, 0x86, 1)
        struct.pack_into("<H", image, 0x94, 240)
        optional = 0x98
        struct.pack_into("<H", image, optional, 0x20B)
        struct.pack_into("<I", image, optional + 108, 16)
        directories = optional + 112
        struct.pack_into("<II", image, directories + 13 * 8, 0x1000, 64)
        section = optional + 240
        struct.pack_into("<IIII", image, section + 8, 0x100, 0x1000, 0x100, 0x200)
        name_rva = 0x1050 if valid_name_rva else 0x90000000
        struct.pack_into("<IIIIIIII", image, 0x200, 1, name_rva, 0, 0, 0, 0, 0, 0)
        image[0x250:0x250 + len(encoded)] = encoded
        return bytes(image)

    @staticmethod
    def macho_runtime_image(*, needed: str, rpath: str) -> bytes:
        def command(kind: int, header_bytes: int, value: str) -> bytes:
            encoded = value.encode("utf-8") + b"\0"
            size = (header_bytes + len(encoded) + 7) & ~7
            payload = bytearray(size)
            struct.pack_into("<II", payload, 0, kind, size)
            struct.pack_into("<I", payload, 8, header_bytes)
            payload[header_bytes:header_bytes + len(encoded)] = encoded
            return bytes(payload)

        dylib = command(0xC, 24, needed)
        load_path = command(0x8000001C, 12, rpath)
        image = bytearray(32 + len(dylib) + len(load_path))
        struct.pack_into(
            "<IIIIIIII", image, 0, 0xFEEDFACF, 0, 0, 2, 2,
            len(dylib) + len(load_path), 0, 0,
        )
        image[32:] = dylib + load_path
        return bytes(image)

    def test_elf_runtime_parser_retains_rpath_and_runpath(self):
        imports = capability_command._elf_runtime_imports(
            self.elf_runtime_image(
                needed=("libchild.so",), rpath=("$ORIGIN/legacy",),
                runpath=("$ORIGIN/private",),
            )
        )
        self.assertEqual(imports.names, ("libchild.so",))
        self.assertEqual(imports.rpath, ("$ORIGIN/legacy",))
        self.assertEqual(imports.runpath, ("$ORIGIN/private",))

    def test_elf_pt_interp_and_pe_delay_imports_are_parsed_and_closed(self):
        elf = self.elf_interp_runtime_image("/toolchain/ld-authoritative.so")
        pe = self.pe_delay_import_runtime_image("delay-runtime.dll")
        elf_imports = capability_command._elf_runtime_imports(elf)
        self.assertEqual(elf_imports.names, ())
        self.assertEqual(
            elf_imports.interpreters,
            ("/toolchain/ld-authoritative.so",),
        )
        self.assertEqual(
            capability_command._pe_runtime_import_names(pe),
            ("delay-runtime.dll",),
        )

    def test_elf_pt_interp_and_pe_delay_imports_reject_malformed_names(self):
        with self.assertRaisesRegex(AuditInfrastructureError, "ELF|unterminated"):
            capability_command._elf_runtime_imports(
                self.elf_interp_runtime_image("/toolchain/ld.so", terminated=False)
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "PE|RVA"):
            capability_command._pe_runtime_import_names(
                self.pe_delay_import_runtime_image(
                    "delay-runtime.dll", valid_name_rva=False
                )
            )

    def test_macho_runtime_parser_retains_lc_rpath(self):
        imports = capability_command._macho_runtime_imports(
            self.macho_runtime_image(
                needed="@rpath/libchild.dylib", rpath="@loader_path/../Frameworks"
            )
        )
        self.assertEqual(imports.names, ("@rpath/libchild.dylib",))
        self.assertEqual(imports.rpath, ("@loader_path/../Frameworks",))

    def test_linux_ldconfig_cache_parser_preserves_hwcap_preference(self):
        parser = getattr(capability_command, "_parse_linux_ldconfig_cache", None)
        self.assertTrue(callable(parser), "Linux loader cache parser is absent")
        cache = parser(
            b"3 libs found in cache `/etc/ld.so.cache'\n"
            b"\tlibx.so (libc6,x86-64, hwcap: x86-64-v3) => /lib/glibc-hwcaps/x86-64-v3/libx.so\n"
            b"\tlibx.so (libc6,x86-64) => /lib/libx.so\n"
        )
        self.assertEqual(
            cache["libx.so"],
            (
                Path("/lib/glibc-hwcaps/x86-64-v3/libx.so"),
                Path("/lib/libx.so"),
            ),
        )

    def test_macho_fat_image_selects_only_host_slice(self):
        x86 = self.macho_runtime_image(
            needed="@rpath/libx86.dylib", rpath="@loader_path/x86"
        )
        arm = self.macho_runtime_image(
            needed="@rpath/libarm.dylib", rpath="@loader_path/arm"
        )
        offset_x86 = 0x100
        offset_arm = offset_x86 + len(x86)
        image = bytearray(offset_arm + len(arm))
        struct.pack_into(">II", image, 0, 0xCAFEBABE, 2)
        struct.pack_into(">IIIII", image, 8, 0x01000007, 3, offset_x86, len(x86), 0)
        struct.pack_into(">IIIII", image, 28, 0x0100000C, 0, offset_arm, len(arm), 0)
        image[offset_x86:offset_x86 + len(x86)] = x86
        image[offset_arm:offset_arm + len(arm)] = arm
        with mock.patch("platform.machine", return_value="x86_64"):
            imports = capability_command._macho_runtime_imports(bytes(image))
        self.assertEqual(imports.names, ("@rpath/libx86.dylib",))
        self.assertEqual(imports.rpath, ("@loader_path/x86",))

    def test_macho_fat64_image_selects_host_slice(self):
        thin = self.macho_runtime_image(
            needed="@rpath/libhost.dylib", rpath="@loader_path/host"
        )
        offset = 0x100
        image = bytearray(offset + len(thin))
        struct.pack_into(">II", image, 0, 0xCAFEBABF, 1)
        struct.pack_into(">IIQQII", image, 8, 0x01000007, 3, offset, len(thin), 0, 0)
        image[offset:] = thin
        with mock.patch("platform.machine", return_value="x86_64"):
            imports = capability_command._macho_runtime_imports(bytes(image))
        self.assertEqual(imports.names, ("@rpath/libhost.dylib",))

    def test_binary_runtime_import_parser_reads_current_executable(self):
        resolver = getattr(capability_command, "_binary_runtime_import_names", None)
        self.assertTrue(callable(resolver), "binary runtime import resolver is absent")
        imports = resolver(Path(sys.executable).resolve())
        self.assertIsInstance(imports, tuple)
        self.assertTrue(imports)

    def test_identifies_all_supported_families_from_name_and_version(self):
        cases = (
            ("g++.exe", b"g++.exe (Rev2, Built by MSYS2 project) 13.1.0\n", CompilerFamily.GCC),
            ("x86_64-w64-mingw32-gcc-13.exe", b"x86_64-w64-mingw32-gcc (GCC) 13.1.0\n", CompilerFamily.GCC),
            ("clang++", b"Ubuntu clang version 18.1.3\n", CompilerFamily.CLANG),
            ("clang", b"Apple clang version 17.0.0\n", CompilerFamily.CLANG),
            ("cl.exe", b"Microsoft (R) C/C++ Optimizing Compiler Version 19.44\n", CompilerFamily.MSVC),
            ("clang-cl.exe", b"clang version 18.1.3\nTarget: x86_64-pc-windows-msvc\n", CompilerFamily.CLANG_CL),
        )
        for name, output, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(identify_compiler(Path(name), output), expected)

    def test_rejects_unknown_names_failed_and_contradictory_probes(self):
        controls = (
            ("wrapper", b"gcc (GCC) 13.1.0", "unsupported compiler"),
            ("g++", b"clang version 18.1.3", "contradictory"),
            ("clang++", b"gcc (GCC) 13.1.0", "contradictory"),
            ("cl.exe", b"clang version 18.1.3", "contradictory"),
            ("clang-cl.exe", b"Microsoft.*Compiler Version 19.44", "contradictory"),
            ("g++", b"", "version probe"),
            ("g++", b"\xff", "version probe"),
        )
        for name, output, message in controls:
            with self.subTest(name=name, output=output), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                identify_compiler(Path(name), output)

    def test_localized_msvc_version_uses_strict_host_native_encoding(self):
        output = (
            "Kääntäjä Microsoft (R) C/C++ Optimizing Compiler Version 19.44\n"
        ).encode(locale.getpreferredencoding(False), errors="strict")
        self.assertEqual(identify_compiler(Path("cl.exe"), output), CompilerFamily.MSVC)


class ResponseFileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, relative: str, content: str | bytes) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, str):
            path.write_text(content, encoding="utf-8")
        else:
            path.write_bytes(content)
        return path

    def test_nested_gcc_response_expands_in_order(self):
        self.write("inner.rsp", '-DNAME="two words" source.cpp')
        self.write("outer.rsp", '-I"SDK Path/include" @inner.rsp')
        expanded = expand_response_files(("@outer.rsp",), CompilerFamily.GCC,
                                         self.root, AuditLimits())
        self.assertEqual(expanded, ("-ISDK Path/include", "-DNAME=two words", "source.cpp"))

    def test_nested_response_paths_remain_relative_to_compile_working_directory(self):
        self.write("nested/inner.rsp", "-DINNER")
        self.write("outer.rsp", "@nested/inner.rsp -DOUTER")
        self.assertEqual(
            expand_response_files(("@outer.rsp",), CompilerFamily.CLANG, self.root, AuditLimits()),
            ("-DINNER", "-DOUTER"),
        )

    def test_msvc_quoting_and_supported_encodings(self):
        payload = '/I"SDK Path" /DNAME="two words" "source file.cpp"'
        self.write("utf8.rsp", b"\xef\xbb\xbf" + payload.encode("utf-8"))
        self.write("utf16.rsp", b"\xff\xfe" + payload.encode("utf-16-le"))
        expected = ("/ISDK Path", "/DNAME=two words", "source file.cpp")
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for response in ("utf8.rsp", "utf16.rsp"):
                with self.subTest(family=family, response=response):
                    self.assertEqual(
                        expand_response_files((f"@{response}",), family, self.root, AuditLimits()),
                        expected,
                    )

    def test_msvc_multiline_response_treats_crlf_and_lf_as_argument_boundaries(self):
        self.write(
            "multiline.rsp",
            b'/DOne=1\r\n/DTwo="two words"\n"source file.cpp"\r\n',
        )
        self.assertEqual(
            expand_response_files(
                ("@multiline.rsp",), CompilerFamily.MSVC, self.root, AuditLimits()
            ),
            ("/DOne=1", "/DTwo=two words", "source file.cpp"),
        )

    def test_response_cycle_fails(self):
        self.write("a.rsp", "@b.rsp")
        self.write("b.rsp", "@a.rsp")
        with self.assertRaisesRegex(AuditInfrastructureError, "response-file cycle"):
            expand_response_files(("@a.rsp",), CompilerFamily.GCC,
                                  self.root, AuditLimits())

    def test_canonical_symlink_cycle_fails(self):
        original = self.write("real.rsp", "@alias.rsp")
        alias = self.root / "alias.rsp"
        try:
            alias.symlink_to(original)
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        with self.assertRaisesRegex(AuditInfrastructureError, "response-file cycle"):
            expand_response_files(("@real.rsp",), CompilerFamily.GCC, self.root, AuditLimits())

    def test_hard_link_alias_is_rejected(self):
        original = self.write("real.rsp", "-DREAL")
        alias = self.root / "alias.rsp"
        try:
            os.link(original, alias)
        except OSError as error:
            self.skipTest(f"hard links unavailable: {error}")
        with self.assertRaisesRegex(AuditInfrastructureError, "response-file alias"):
            expand_response_files(
                ("@real.rsp", "@alias.rsp"), CompilerFamily.GCC, self.root, AuditLimits()
            )

    def test_depth_bound_accepts_eight_and_rejects_nine(self):
        for prefix, count in (("pass", 8), ("fail", 9)):
            for index in range(1, count + 1):
                content = f"@{prefix}-{index + 1}.rsp" if index < count else "-DEND"
                self.write(f"{prefix}-{index}.rsp", content)
        self.assertEqual(
            expand_response_files(
                ("@pass-1.rsp",), CompilerFamily.GCC, self.root, AuditLimits()
            ),
            ("-DEND",),
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "response-file depth"):
            expand_response_files(("@fail-1.rsp",), CompilerFamily.GCC, self.root, AuditLimits())

    def test_distinct_file_bound_accepts_thirty_two_and_rejects_thirty_three(self):
        for count in (31, 32):
            names = []
            for index in range(count):
                name = f"{count}-{index}.rsp"
                names.append(f"@{name}")
                self.write(name, f"-DVALUE{index}")
            self.write(f"all-{count}.rsp", " ".join(names))
        self.assertEqual(
            len(expand_response_files(
                ("@all-31.rsp",), CompilerFamily.GCC, self.root, AuditLimits()
            )),
            31,
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "response-file count"):
            expand_response_files(
                ("@all-32.rsp",), CompilerFamily.GCC, self.root, AuditLimits()
            )

    def test_aggregate_byte_bound_accepts_exact_and_rejects_one_over(self):
        self.write("exact.rsp", b"x" * 32)
        self.assertEqual(
            expand_response_files(
                ("@exact.rsp",), CompilerFamily.GCC, self.root,
                dataclasses.replace(AuditLimits(), response_bytes=32),
            ),
            ("x" * 32,),
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "response-file bytes"):
            expand_response_files(
                ("@exact.rsp",), CompilerFamily.GCC, self.root,
                dataclasses.replace(AuditLimits(), response_bytes=31),
            )

    def test_oversized_response_is_rejected_before_payload_read(self):
        response = self.write("oversized.rsp", b"x" * 64)
        original_open = Path.open

        def guarded_open(path, *args, **kwargs):
            if path == response.resolve():
                raise AssertionError("oversized response payload was opened")
            return original_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", guarded_open), self.assertRaisesRegex(
            AuditInfrastructureError, "response-file bytes"
        ):
            expand_response_files(
                ("@oversized.rsp",), CompilerFamily.GCC, self.root,
                dataclasses.replace(AuditLimits(), response_bytes=32),
            )

    def test_unreadable_invalid_encoding_and_quoting_fail_closed(self):
        self.write("invalid-utf8.rsp", b"-DOK \xff")
        self.write("utf16be.rsp", b"\xfe\xff\x00x")
        self.write("nul.rsp", b"-DOK\x00-DHIDDEN")
        self.write("quote.rsp", '"unterminated')
        controls = (
            ("missing.rsp", "unreadable"),
            ("invalid-utf8.rsp", "encoding"),
            ("utf16be.rsp", "encoding"),
            ("nul.rsp", "encoding"),
            ("quote.rsp", "quoting"),
        )
        for name, message in controls:
            with self.subTest(name=name), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                expand_response_files((f"@{name}",), CompilerFamily.MSVC, self.root, AuditLimits())


class ConfigurationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.real_driver_selected_helper_paths = (
            capability_command._driver_selected_helper_paths
        )
        self.driver_helpers = mock.patch(
            "gpu_capability_command._driver_selected_helper_paths", return_value=()
        )
        self.driver_helpers.start()
        self.addCleanup(self.driver_helpers.stop)
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source_root = self.root / "repo"
        self.build = self.source_root / "build"
        self.build.mkdir(parents=True)
        self.database = self.build / "compile_commands.json"
        self.source = self.source_root / "playback" / "gpu" / "file.cpp"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("int main() {}\n", encoding="utf-8")
        self.compiler = self.root / "toolchain" / "g++.exe"
        self.compiler.parent.mkdir()
        self.compiler.write_bytes(b"compiler-content-a")
        self.production = {
            PurePosixPath("playback/gpu/file.cpp"): FileIdentity(
                canonical=self.source.resolve(),
                relative=PurePosixPath("playback/gpu/file.cpp"),
                device=None,
                inode=None,
                line_count=1,
                production=True,
            )
        }
        self.environment = {"PATH": str(self.compiler.parent), "GPU_MODE": "on"}
        self.dependency_roots = build_dependency_root_authority(
            self.source_root, {"toolchain": self.compiler.parent}
        )

    def tearDown(self) -> None:
        _clear_compiler_inspection_memo_for_tests()
        self.temporary.cleanup()

    def entry(self, *, file: str = "../playback/gpu/file.cpp", arguments=None):
        if arguments is None:
            arguments = [str(self.compiler), "-c", file]
        return {"directory": str(self.build), "file": file, "arguments": arguments}

    def make(self, entry=None, *, environment=None, index=3):
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0\r\n",
        ):
            return make_configuration(
                self.entry() if entry is None else entry,
                self.database,
                index,
                self.source_root,
                self.production,
                self.environment if environment is None else environment,
                AuditLimits(),
                self.dependency_roots,
            )

    def test_implicit_configuration_deadline_uses_global_180_second_limit(self):
        with mock.patch(
            "gpu_capability_command.time.monotonic", return_value=1000.0
        ), mock.patch(
            "gpu_capability_command.inspect_compiler",
            side_effect=AuditInfrastructureError("captured implicit deadline"),
        ) as inspect, self.assertRaisesRegex(
            AuditInfrastructureError, "captured implicit deadline"
        ):
            make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits(), self.dependency_roots,
            )
        self.assertEqual(inspect.call_args.args[6], 1180.0)

    def test_make_configuration_normalizes_all_semantic_inputs(self):
        response = self.build / "flags.rsp"
        response.write_text("-DOLR_GPU=1 ../playback/gpu/file.cpp", encoding="utf-8")
        entry = self.entry(arguments=[
            "ccache", "compiler_check=content", str(self.compiler), "-c", "@flags.rsp"
        ])
        configuration = self.make(entry)
        self.assertEqual(configuration.entry_id, f"{self.database.resolve()}:3")
        self.assertEqual(configuration.family, CompilerFamily.GCC)
        self.assertEqual(configuration.compiler, self.compiler.resolve())
        self.assertEqual(configuration.working_directory, self.build.resolve())
        self.assertIs(configuration.source, next(iter(self.production.values())))
        self.assertEqual(
            configuration.arguments,
            ("-c", "-DOLR_GPU=1", "../playback/gpu/file.cpp"),
        )
        self.assertRegex(configuration.environment_digest, r"^[0-9a-f]{64}$")
        self.assertRegex(configuration.digest, r"^[0-9a-f]{64}$")

    def test_environment_digest_is_complete_deterministic_and_does_not_leak_values(self):
        first = self.make(environment={"GPU_MODE": "secret-value", "PATH": str(self.compiler.parent)})
        reordered = self.make(environment={"PATH": str(self.compiler.parent), "GPU_MODE": "secret-value"})
        changed = self.make(environment={"PATH": str(self.compiler.parent), "GPU_MODE": "changed"})
        self.assertEqual(first.environment_digest, reordered.environment_digest)
        self.assertEqual(first.digest, reordered.digest)
        self.assertNotEqual(first.environment_digest, changed.environment_digest)
        self.assertNotEqual(first.digest, changed.digest)
        self.assertNotIn("secret-value", repr(first))

    def test_compiler_capability_holds_helper_and_versioned_runtime_siblings(self):
        helper = self.compiler.parent / "cc1plus.exe"
        runtime = self.compiler.parent / "libcompiler-runtime.so.1"
        self.compiler.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(runtime.name,), runpath=("$ORIGIN",),
        ))
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        runtime.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ):
            configuration = self.make()
        self.assertEqual(
            tuple(
                item.role_relative_path.as_posix()
                for item in configuration.compiler_capability.resolved_runtime_closure
            ),
            ("cc1plus.exe", "g++.exe", "libcompiler-runtime.so.1"),
        )

    def test_unrelated_runtime_sibling_is_excluded_from_the_closure(self):
        runtime = self.compiler.parent / "unused-runtime.dll"
        runtime.write_bytes(b"unused runtime")
        empty = capability_command._RuntimeImports((), format_kind="pe")
        missing = capability_command._RuntimeImports(
            ("missing-transitive.dll",), format_kind="pe"
        )
        with mock.patch(
            "gpu_capability_command._binary_runtime_imports",
            side_effect=lambda path, **_kwargs: (
                missing if path.resolve() == runtime.resolve() else empty
            ),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
            )
        self.addCleanup(capability.native_owner.close)
        closure = {
            item.role_relative_path.as_posix()
            for item in capability.resolved_runtime_closure
        }
        self.assertNotIn("unused-runtime.dll", closure)

    def test_version_probe_launches_through_held_compiler_capability(self):
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._run_probe_command",
            return_value=b"g++.exe (GCC) 13.1.0\n",
        ) as run:
            inspect_compiler(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.dependency_roots, pipeline_deadline=time.monotonic() + 10.0,
                working_directory=self.build,
            )
        self.assertIsInstance(run.call_args.args[0], CompilerExecutableCapability)

    def test_shared_compiler_boundary_owns_inspection_and_exact_macos_abi(self):
        info_type = capability_command._macos_proc_bsdinfo_type()
        self.assertEqual(ctypes.sizeof(info_type), 136)
        fields = dict(info_type._fields_)
        self.assertEqual(fields["pbi_comm"]._length_, 16)
        self.assertEqual(fields["pbi_name"]._length_, 32)

        boundary_source = inspect.getsource(
            capability_command.launch_compiler_process
        )
        runner_source = Path(capability_command.__file__).with_name(
            "gpu_capability_runner.py"
        ).read_text(encoding="utf-8")
        self.assertEqual(boundary_source.count("subprocess." + "Popen("), 1)
        self.assertEqual(runner_source.count("subprocess." + "Popen("), 0)

    def test_inspection_uses_typed_shared_launch_boundary(self):
        capability = open_compiler_executable_capability(
            self.compiler.resolve(), self.dependency_roots,
            time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
        )
        self.addCleanup(capability.native_owner.close)
        observer = object()
        with mock.patch.object(
            capability_command,
            "launch_compiler_process",
            side_effect=AuditInfrastructureError("inspection boundary reached"),
        ) as launch, self.assertRaisesRegex(
            AuditInfrastructureError, "inspection boundary reached"
        ):
            inspect_compiler(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.dependency_roots, pipeline_deadline=time.monotonic() + 10.0,
                launch_accountant=observer, working_directory=self.build,
                compiler_capability=capability,
            )
        self.assertEqual(
            launch.call_args.kwargs["purpose"],
            capability_command.CompilerLaunchPurpose.INSPECTION,
        )
        self.assertIs(launch.call_args.kwargs["launch_observer"], observer)

    def test_parent_carrier_revalidates_process_identity_after_exit(self):
        class Process:
            pid = 42
            stdin = None

        class Containment:
            requires_handshake = False
            popen_arguments = {}

            def attach(self, process):
                self.process = process

            def release(self, process):
                self.released = process

            def terminate(self):
                pass

        class Observer:
            def __init__(self):
                self.registered = []
                self.completed = []

            def register_compiler_process_launch(self, event, carrier):
                self.registered.append((event, carrier))

            def complete_compiler_process_launch(self, event, carrier):
                self.completed.append((event, carrier))

        process = Process()
        observer = Observer()
        with mock.patch.object(
            capability_command.subprocess, "Popen", return_value=process
        ), mock.patch.object(
            capability_command, "_native_process_start_token",
            side_effect=("native-start", "reused-start"),
        ):
            returned, carrier = capability_command.launch_compiler_process(
                ("compiler", "--version"),
                cwd=self.build,
                environment=self.environment,
                containment=Containment(),
                platform_kind="windows",
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                launch_options={},
                purpose=capability_command.CompilerLaunchPurpose.INSPECTION,
                launch_observer=observer,
            )
            self.assertIs(returned, process)
            self.assertIs(observer.registered[0][1], carrier)
            with self.assertRaisesRegex(
                AuditInfrastructureError, "post-exit process identity differs"
            ):
                carrier.complete_after_exit()
            with self.assertRaisesRegex(
                AuditInfrastructureError, "already completed"
            ):
                carrier.complete_after_exit()
            self.assertIsNone(carrier.process)
        self.assertEqual(observer.completed, [])

    def test_macos_carrier_retains_native_exit_identity_before_reap(self):
        class Process:
            pid = 42
            stdin = None

        class Containment:
            popen_arguments = {}

            def attach(self, _process): pass
            def release(self, _process): pass
            def terminate(self): pass

        class Observer:
            def register_compiler_process_launch(self, _event, _carrier): pass
            def complete_compiler_process_launch(self, _event, _carrier): pass

        class MacIdentityHandle:
            def __init__(self):
                self.validations = 0
                self.closed = False

            def validate_exit(self):
                self.validations += 1

            def close(self):
                self.closed = True

        process = Process()
        retained = MacIdentityHandle()
        with mock.patch.object(
            capability_command.subprocess, "Popen", return_value=process
        ), mock.patch.object(
            capability_command, "_native_process_start_token",
            return_value="macos-proc:1:2",
        ) as token, mock.patch.object(
            capability_command, "_open_macos_process_identity_handle",
            return_value=retained,
        ):
            _process, carrier = capability_command.launch_compiler_process(
                ("compiler",), cwd=self.build, environment=self.environment,
                containment=Containment(), platform_kind="macos",
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, launch_options={},
                purpose=capability_command.CompilerLaunchPurpose.INSPECTION,
                launch_observer=Observer(),
            )
            carrier.complete_after_exit()
        self.assertEqual(token.call_count, 1)
        self.assertEqual(retained.validations, 1)
        self.assertTrue(retained.closed)

    def test_carrier_abort_closes_resources_when_observer_and_close_fail(self):
        class Process:
            pid = 42

        class Observer:
            def __init__(self):
                self.active = True

            def fail_compiler_process_launch(self, _event, _carrier):
                self.active = False
                raise RuntimeError("observer abort failed")

        event = capability_command.CompilerLaunchEvent(
            capability_command.CompilerLaunchPurpose.INSPECTION,
            capability_command.ProcessStartIdentity(
                "linux", 42, "linux-proc:boot:1", "a" * 64
            ),
        )
        observer = Observer()
        with mock.patch.object(
            capability_command.os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(
            capability_command.os, "close", side_effect=OSError("pidfd close failed")
        ) as close, self.assertRaisesRegex(
            RuntimeError, "observer abort failed"
        ) as raised:
            carrier = capability_command.CompilerProcessHandleCarrier(
                event, Process(), observer
            )
            carrier.abort_before_return()
        self.assertTrue(carrier.completed)
        self.assertIsNone(carrier.process)
        self.assertFalse(observer.active)
        close.assert_called_once_with(91)
        self.assertTrue(any(
            "carrier resource cleanup also failed: pidfd close failed" in note
            for note in (raised.exception.__notes__ or ())
        ))

    def test_carrier_completion_preserves_primary_when_native_close_fails(self):
        class Process:
            pid = 42

        class Observer:
            def complete_compiler_process_launch(self, _event, _carrier):
                raise RuntimeError("completion primary failed")

            def fail_compiler_process_launch(self, _event, _carrier):
                self.failed = True

        class MacIdentityHandle:
            def validate_exit(self): pass
            def close(self): raise OSError("kqueue close failed")

        cases = (
            ("linux", "linux-proc:boot:1", "pidfd close failed"),
            ("macos", "macos-proc:1:2", "kqueue close failed"),
        )
        for platform_kind, native_token, cleanup_message in cases:
            with self.subTest(platform_kind=platform_kind):
                event = capability_command.CompilerLaunchEvent(
                    capability_command.CompilerLaunchPurpose.INSPECTION,
                    capability_command.ProcessStartIdentity(
                        platform_kind, 42, native_token, "a" * 64
                    ),
                )
                observer = Observer()
                patches = [
                    mock.patch.object(
                        capability_command.os,
                        "pidfd_open",
                        return_value=91,
                        create=True,
                    ),
                    mock.patch.object(
                        capability_command.os,
                        "close",
                        side_effect=OSError("pidfd close failed"),
                    ),
                    mock.patch.object(
                        capability_command.os, "fstat", return_value=object()
                    ),
                    mock.patch.object(
                        capability_command,
                        "_open_macos_process_identity_handle",
                        return_value=MacIdentityHandle(),
                    ),
                ]
                with (
                    patches[0], patches[1], patches[2], patches[3],
                    self.assertRaisesRegex(
                    RuntimeError, "completion primary failed"
                    ) as raised,
                ):
                    carrier = capability_command.CompilerProcessHandleCarrier(
                        event, Process(), observer
                    )
                    carrier.complete_after_exit()
                self.assertTrue(observer.failed)
                self.assertTrue(carrier.completed)
                self.assertIsNone(carrier.process)
                self.assertTrue(any(
                    cleanup_message in note
                    for note in (raised.exception.__notes__ or ())
                ))

    def test_launch_preserves_primary_error_when_carrier_abort_observer_fails(self):
        class Process:
            pid = 42

            def kill(self): pass
            def wait(self, timeout): self.wait_timeout = timeout

        class Containment:
            popen_arguments = {}

            def attach(self, process): self.attached = process
            def release(self, _process): raise ValueError("launch release failed")
            def terminate(self): self.terminated = True

        class Observer:
            def __init__(self):
                self.active = None
                self.carrier = None

            def register_compiler_process_launch(self, _event, carrier):
                self.active = carrier
                self.carrier = carrier

            def fail_compiler_process_launch(self, _event, carrier):
                self.asserted_carrier = carrier
                self.active = None
                raise RuntimeError("observer abort failed")

        process = Process()
        containment = Containment()
        observer = Observer()
        with mock.patch.object(
            capability_command.subprocess, "Popen", return_value=process
        ), mock.patch.object(
            capability_command, "_native_process_start_token",
            return_value="linux-proc:boot:1",
        ), mock.patch.object(
            capability_command.os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(
            capability_command.os, "close", side_effect=OSError("pidfd close failed")
        ) as close, self.assertRaisesRegex(
            ValueError, "launch release failed"
        ) as raised:
            capability_command.launch_compiler_process(
                ("compiler",), cwd=self.build, environment=self.environment,
                containment=containment, platform_kind="linux",
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, launch_options={},
                purpose=capability_command.CompilerLaunchPurpose.INSPECTION,
                launch_observer=observer,
            )
        self.assertIsNone(observer.active)
        self.assertIs(observer.asserted_carrier, observer.carrier)
        self.assertTrue(observer.carrier.completed)
        self.assertIsNone(observer.carrier.process)
        self.assertTrue(containment.terminated)
        self.assertEqual(process.wait_timeout, 1.0)
        close.assert_called_once_with(91)
        self.assertTrue(any(
            "compiler process carrier abort also failed: observer abort failed" in note
            for note in (raised.exception.__notes__ or ())
        ))
        self.assertTrue(any(
            "compiler process carrier abort detail: "
            "carrier resource cleanup also failed: pidfd close failed" in note
            for note in (raised.exception.__notes__ or ())
        ))

    @unittest.skipUnless(sys.platform == "win32", "requires Windows process HANDLE")
    def test_windows_carrier_abort_closes_real_process_handle_once(self):
        class Observer:
            def __init__(self): self.failed = 0

            def fail_compiler_process_launch(self, _event, _carrier):
                self.failed += 1

        process = subprocess.Popen(
            (sys.executable, "-I", "-c", "raise SystemExit(0)"),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        raw_handle = int(process._handle)
        event = capability_command.CompilerLaunchEvent(
            capability_command.CompilerLaunchPurpose.INSPECTION,
            capability_command.ProcessStartIdentity(
                "windows", process.pid,
                capability_command._native_process_start_token(process, "windows"),
                "a" * 64,
            ),
        )
        observer = Observer()
        carrier = capability_command.CompilerProcessHandleCarrier(
            event, process, observer
        )
        self.assertEqual(process.wait(timeout=10.0), 0)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetProcessId.argtypes = (ctypes.c_void_p,)
        kernel32.GetProcessId.restype = ctypes.c_ulong
        self.assertEqual(kernel32.GetProcessId(raw_handle), process.pid)
        handle_type = type(process._handle)
        real_close = handle_type.Close
        close_calls = []

        def tracked_close(handle):
            close_calls.append(int(handle))
            return real_close(handle)

        with mock.patch.object(handle_type, "Close", autospec=True) as close:
            close.side_effect = tracked_close
            carrier.abort_before_return()
            with self.assertRaisesRegex(
                AuditInfrastructureError, "already completed"
            ):
                carrier.abort_before_return()
        self.assertEqual(observer.failed, 1)
        self.assertEqual(close_calls, [raw_handle])
        self.assertTrue(process._handle.closed)
        ctypes.set_last_error(0)
        self.assertEqual(kernel32.GetProcessId(raw_handle), 0)
        self.assertEqual(ctypes.get_last_error(), 6)

    @unittest.skipUnless(sys.platform == "win32", "requires Windows process HANDLE")
    def test_windows_carrier_completion_closes_real_process_handle_once(self):
        class Observer:
            def __init__(self):
                self.completed = 0
                self.failed = 0

            def complete_compiler_process_launch(self, _event, _carrier):
                self.completed += 1

            def fail_compiler_process_launch(self, _event, _carrier):
                self.failed += 1

        process = subprocess.Popen(
            (sys.executable, "-I", "-c", "raise SystemExit(0)"),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        raw_handle = int(process._handle)
        event = capability_command.CompilerLaunchEvent(
            capability_command.CompilerLaunchPurpose.INSPECTION,
            capability_command.ProcessStartIdentity(
                "windows",
                process.pid,
                capability_command._native_process_start_token(process, "windows"),
                "a" * 64,
            ),
        )
        observer = Observer()
        carrier = capability_command.CompilerProcessHandleCarrier(
            event, process, observer
        )
        self.assertEqual(process.wait(timeout=10.0), 0)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetProcessId.argtypes = (ctypes.c_void_p,)
        kernel32.GetProcessId.restype = ctypes.c_ulong
        self.assertEqual(kernel32.GetProcessId(raw_handle), process.pid)
        handle_type = type(process._handle)
        real_close = handle_type.Close
        close_calls = []

        def tracked_close(handle):
            close_calls.append(int(handle))
            return real_close(handle)

        with mock.patch.object(handle_type, "Close", autospec=True) as close:
            close.side_effect = tracked_close
            carrier.complete_after_exit()
            with self.assertRaisesRegex(
                AuditInfrastructureError, "already completed"
            ):
                carrier.complete_after_exit()
        self.assertEqual(observer.completed, 1)
        self.assertEqual(observer.failed, 0)
        self.assertEqual(close_calls, [raw_handle])
        self.assertTrue(process._handle.closed)
        ctypes.set_last_error(0)
        self.assertEqual(kernel32.GetProcessId(raw_handle), 0)
        self.assertEqual(ctypes.get_last_error(), 6)

    def test_launch_preserves_primary_when_containment_termination_fails(self):
        class Process:
            pid = 42

            def kill(self): self.killed = True
            def wait(self, timeout): self.wait_timeout = timeout

        class Containment:
            popen_arguments = {}

            def attach(self, process): self.attached = process
            def release(self, _process): raise ValueError("launch release failed")
            def terminate(self):
                self.terminated = True
                raise OSError("containment terminate failed")

        class Observer:
            def register_compiler_process_launch(self, _event, carrier):
                self.active = carrier
                self.carrier = carrier

            def fail_compiler_process_launch(self, _event, carrier):
                self.failed = carrier
                self.active = None

        process = Process()
        containment = Containment()
        observer = Observer()
        with mock.patch.object(
            capability_command.subprocess, "Popen", return_value=process
        ), mock.patch.object(
            capability_command, "_native_process_start_token",
            return_value="linux-proc:boot:1",
        ), mock.patch.object(
            capability_command.os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(
            capability_command.os, "close"
        ) as close, self.assertRaisesRegex(
            ValueError, "launch release failed"
        ) as raised:
            capability_command.launch_compiler_process(
                ("compiler",), cwd=self.build, environment=self.environment,
                containment=containment, platform_kind="linux",
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, launch_options={},
                purpose=capability_command.CompilerLaunchPurpose.INSPECTION,
                launch_observer=observer,
            )
        self.assertIsNone(observer.active)
        self.assertIs(observer.failed, observer.carrier)
        self.assertTrue(observer.failed.completed)
        self.assertIsNone(observer.failed.process)
        self.assertTrue(containment.terminated)
        self.assertTrue(process.killed)
        self.assertEqual(process.wait_timeout, 1.0)
        close.assert_called_once_with(91)
        self.assertTrue(any(
            "compiler process containment termination also failed: "
            "containment terminate failed" in note
            for note in (raised.exception.__notes__ or ())
        ))

    @unittest.skipUnless(sys.platform == "darwin", "requires macOS kqueue")
    def test_macos_carrier_validates_real_process_after_wait_reaps_it(self):
        class Containment:
            popen_arguments = {}

            def attach(self, _process): pass
            def release(self, _process): pass
            def terminate(self): pass

        class Observer:
            def __init__(self):
                self.completed = 0

            def register_compiler_process_launch(self, _event, _carrier): pass

            def complete_compiler_process_launch(self, _event, _carrier):
                self.completed += 1

        observer = Observer()
        process, carrier = capability_command.launch_compiler_process(
            (sys.executable, "-I", "-c", "raise SystemExit(0)"),
            cwd=self.build,
            environment=self.environment,
            containment=Containment(), platform_kind="macos",
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, launch_options={},
            purpose=capability_command.CompilerLaunchPurpose.INSPECTION,
            launch_observer=observer,
        )
        self.assertEqual(process.wait(timeout=5), 0)
        carrier.complete_after_exit()
        self.assertEqual(observer.completed, 1)

    def test_shared_boundary_emits_exact_inspection_discovery_accepted_events(self):
        class Handle:
            def Close(self):
                self.closed = True

        class Process:
            stdin = None

            def __init__(self, pid):
                self.pid = pid
                self._handle = Handle()

        class Containment:
            requires_handshake = False
            popen_arguments = {}

            def attach(self, _process): pass
            def release(self, _process): pass
            def terminate(self): pass

        class Observer:
            def __init__(self):
                self.events = []

            def register_compiler_process_launch(self, event, _carrier):
                self.events.append(event)

            def complete_compiler_process_launch(self, _event, _carrier):
                pass

        purposes = (
            capability_command.CompilerLaunchPurpose.INSPECTION,
            capability_command.CompilerLaunchPurpose.AUDIT_DISCOVERY,
            capability_command.CompilerLaunchPurpose.AUDIT_ACCEPTED,
        )
        processes = [Process(100 + index) for index in range(3)]
        tokens = tuple(
            token
            for index in range(3)
            for token in (f"start-{index}", f"start-{index}")
        )
        observer = Observer()
        with mock.patch.object(
            capability_command.subprocess, "Popen", side_effect=processes
        ), mock.patch.object(
            capability_command, "_native_process_start_token",
            side_effect=tokens,
        ):
            for index, purpose in enumerate(purposes):
                audit = purpose is not capability_command.CompilerLaunchPurpose.INSPECTION
                _process, carrier = capability_command.launch_compiler_process(
                    ("compiler",),
                    cwd=self.build,
                    environment=self.environment,
                    containment=Containment(),
                    platform_kind="windows",
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    launch_options={},
                    purpose=purpose,
                    launch_observer=observer,
                    worker_index=(3 if audit else None),
                    task_id=("task-a" if audit else None),
                    generation=(7 if audit else None),
                )
                carrier.complete_after_exit()
        self.assertEqual(tuple(event.purpose for event in observer.events), purposes)
        self.assertTrue(all(
            event.task_id == "task-a" and event.generation == 7
            for event in observer.events[1:]
        ))

    def test_parent_registry_rejects_pid_reuse_and_releases_failed_carrier(self):
        registry = capability_command._ParentCompilerLaunchObserver()
        first = capability_command.CompilerLaunchEvent(
            capability_command.CompilerLaunchPurpose.INSPECTION,
            capability_command.ProcessStartIdentity(
                "windows", 42, "start-a", "cookie-a"
            ),
        )
        reused = capability_command.CompilerLaunchEvent(
            capability_command.CompilerLaunchPurpose.INSPECTION,
            capability_command.ProcessStartIdentity(
                "windows", 42, "start-b", "cookie-b"
            ),
        )
        first_carrier = object()
        registry.register_compiler_process_launch(first, first_carrier)
        self.assertEqual(registry.active_count, 1)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "carrier is duplicated"
        ):
            registry.register_compiler_process_launch(reused, object())
        registry.fail_compiler_process_launch(first, first_carrier)
        self.assertEqual(registry.active_count, 0)

    def test_unreferenced_sibling_directory_is_not_scanned_into_closure(self):
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch.object(
            Path, "iterdir",
            side_effect=AssertionError("unreferenced sibling scan"),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
            )
        self.addCleanup(capability.native_owner.close)

    def test_compiler_closure_rejects_oversized_runtime_before_hashing(self):
        runtime = self.compiler.parent / "huge-runtime.dll"
        self.compiler.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(runtime.name,), runpath=("$ORIGIN",),
        ))
        with runtime.open("wb") as stream:
            stream.truncate(257 * 1024 * 1024)
        _clear_compiler_inspection_memo_for_tests()
        with self.assertRaisesRegex(AuditInfrastructureError, "per-file byte ceiling"):
            open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
            )

    def test_compiler_closure_honors_cancellation_before_enumeration(self):
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, cancel_event=cancelled,
            )

    def test_capability_memo_binds_driver_query_environment_and_working_directory(self):
        helpers = {}
        for name in ("a", "b", "c"):
            helper = self.compiler.parent / f"helper-{name}.exe"
            helper.write_bytes(name.encode("ascii"))
            helpers[name] = helper.resolve()
        other_cwd = self.build / "other"
        other_cwd.mkdir()

        def selected(_capability, _family, cwd, environment, *_rest):
            if environment["SELECT"] == "a":
                return (helpers["a"],)
            return (helpers["c"] if cwd == other_cwd else helpers["b"],)

        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            side_effect=selected,
        ) as query:
            first = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment={"SELECT": "a"}, working_directory=self.build,
            )
            second = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment={"SELECT": "b"}, working_directory=self.build,
            )
            third = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment={"SELECT": "b"}, working_directory=other_cwd,
            )
        closures = [
            {item.role_relative_path.name for item in capability.resolved_runtime_closure}
            for capability in (first, second, third)
        ]
        self.assertIn("helper-a.exe", closures[0])
        self.assertIn("helper-b.exe", closures[1])
        self.assertIn("helper-c.exe", closures[2])
        self.assertEqual(query.call_count, 3)

    def test_explicit_empty_environment_is_not_replaced_by_ambient_environment(self):
        observed = []

        def selected(_capability, _family, _cwd, environment, *_rest):
            observed.append(dict(environment))
            return ()

        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            side_effect=selected,
        ), mock.patch.dict(os.environ, {"AMBIENT_ONLY": "present"}, clear=True):
            open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment={}, working_directory=self.build,
            )
        self.assertEqual(observed, [{}])

    def test_existing_absolute_driver_helper_outside_authority_fails_closed(self):
        capability = open_compiler_executable_capability(
            self.compiler.resolve(), self.dependency_roots,
            time.monotonic() + 10.0,
        )
        outside = self.root / "untrusted" / "helper.exe"
        outside.parent.mkdir()
        outside.write_bytes(b"existing helper outside authority")
        with mock.patch(
            "gpu_capability_command._run_probe_command",
            return_value=os.fsencode(outside),
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "mapped|trusted toolchain root"
        ):
            self.real_driver_selected_helper_paths(
                capability, CompilerFamily.GCC, self.build, self.environment,
                self.compiler.parent, {}, time.monotonic() + 10.0, None,
                ("-c", str(self.source.resolve())),
            )

    def test_selected_preprocess_helper_missing_fails_closed(self):
        capability = open_compiler_executable_capability(
            self.compiler.resolve(), self.dependency_roots,
            time.monotonic() + 10.0,
        )
        with mock.patch(
            "gpu_capability_command._run_probe_command",
            return_value=b"definitely-absent-optional-helper",
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "preprocessing helper is unavailable"
        ):
            self.real_driver_selected_helper_paths(
                capability, CompilerFamily.GCC, self.build, self.environment,
                self.compiler.parent, {}, time.monotonic() + 10.0, None,
                ("-c", str(self.source.resolve())),
            )

    def test_capability_requires_identical_in_process_authority_object(self):
        capability = open_compiler_executable_capability(
            self.compiler.resolve(), self.dependency_roots,
            time.monotonic() + 10.0,
        )
        replacement = dataclasses.replace(self.dependency_roots)
        self.assertEqual(replacement, self.dependency_roots)
        self.assertIsNot(replacement, self.dependency_roots)
        with self.assertRaisesRegex(AuditInfrastructureError, "local dependency authority"):
            capability_command.validate_compiler_executable_capability(
                capability, replacement
            )

    def test_capability_validation_hashing_obeys_deadline_and_cancellation(self):
        capability = open_compiler_executable_capability(
            self.compiler.resolve(), self.dependency_roots,
            time.monotonic() + 10.0,
        )
        parameters = inspect.signature(
            capability_command.validate_compiler_executable_capability
        ).parameters
        self.assertIn("deadline", parameters)
        self.assertIn("cancel_event", parameters)
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            capability_command.validate_compiler_executable_capability(
                capability, self.dependency_roots,
                deadline=time.monotonic() - 1.0,
            )
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            capability_command.validate_compiler_executable_capability(
                capability, self.dependency_roots,
                deadline=time.monotonic() + 10.0, cancel_event=cancelled,
            )

    def test_nested_helper_runpath_runtime_and_every_ancestor_are_guarded(self):
        nested = self.compiler.parent / "libexec" / "nested"
        runtime_directory = self.compiler.parent / "runtime"
        nested.mkdir(parents=True)
        runtime_directory.mkdir()
        decoy_directory = self.compiler.parent / "decoy"
        decoy_directory.mkdir()
        helper = nested / "cc1.exe"
        runtime = runtime_directory / "libnested.so"
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(runtime.name,), runpath=("$ORIGIN/../../runtime",),
        ))
        runtime.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        (decoy_directory / runtime.name).write_bytes(
            CompilerIdentificationTests.elf_runtime_image()
        )
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment, working_directory=self.build,
            )
        closure_paths = {
            item.role_relative_path.as_posix()
            for item in capability.resolved_runtime_closure
        }
        self.assertIn("libexec/nested/cc1.exe", closure_paths)
        self.assertIn("runtime/libnested.so", closure_paths)
        guarded = set(capability.native_owner.directory_paths)
        self.assertIn(nested.resolve(), guarded)
        self.assertIn(runtime_directory.resolve(), guarded)

    def test_pt_interp_and_delay_import_runtime_files_enter_guarded_closure(self):
        helper_directory = self.compiler.parent / "libexec"
        helper_directory.mkdir()
        elf_helper = helper_directory / "cc1"
        interpreter = self.compiler.parent / "ld-authoritative.so"
        pe_helper = helper_directory / "cc1plus.exe"
        delayed = helper_directory / "delay-runtime.dll"
        interpreter.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        delayed.write_bytes(b"delay runtime payload")
        elf_helper.write_bytes(
            CompilerIdentificationTests.elf_interp_runtime_image(
                str(interpreter.resolve())
            )
        )
        pe_helper.write_bytes(
            CompilerIdentificationTests.pe_delay_import_runtime_image(delayed.name)
        )
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(elf_helper.resolve(), pe_helper.resolve()),
        ), mock.patch(
            "gpu_capability_command._windows_known_dlls", return_value=frozenset()
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment, working_directory=self.build,
            )
        self.addCleanup(capability.native_owner.close)
        closure = {
            item.role_relative_path.as_posix()
            for item in capability.resolved_runtime_closure
        }
        self.assertIn("ld-authoritative.so", closure)
        self.assertIn("libexec/delay-runtime.dll", closure)
        guarded = set(capability.native_owner.file_paths)
        self.assertIn(interpreter.resolve(), guarded)
        self.assertIn(delayed.resolve(), guarded)

    def test_pt_interp_outside_runtime_authority_fails_closed(self):
        helper = self.compiler.parent / "cc1"
        outside = self.root / "untrusted" / "ld-untrusted.so"
        outside.parent.mkdir()
        outside.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        helper.write_bytes(
            CompilerIdentificationTests.elf_interp_runtime_image(
                str(outside.resolve())
            )
        )
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ), self.assertRaisesRegex(AuditInfrastructureError, "authority"):
            open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment, working_directory=self.build,
            )

    def test_relative_pt_interp_is_resolved_as_working_directory_exec_path(self):
        working_directory = self.compiler.parent / "working"
        working_directory.mkdir()
        interpreter = working_directory / "ld-relative.so"
        interpreter.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        helper = self.compiler.parent / "cc1"
        helper.write_bytes(
            CompilerIdentificationTests.elf_interp_runtime_image(
                interpreter.name
            )
        )
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment,
                working_directory=working_directory,
            )
        self.addCleanup(capability.native_owner.close)
        self.assertIn(
            "working/ld-relative.so",
            {
                item.role_relative_path.as_posix()
                for item in capability.resolved_runtime_closure
            },
        )

    def test_preprocess_helper_closure_selects_only_actual_language_frontend(self):
        helper = self.compiler.parent / "cc1plus"
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        for unused in ("cc1", "collect2", "as", "ld"):
            (self.compiler.parent / unused).write_bytes(
                CompilerIdentificationTests.elf_runtime_image()
            )
        queries = []

        def query(_capability, arguments, *_rest):
            queries.append(arguments)
            return str(helper.resolve()).encode("utf-8")

        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._run_probe_command", side_effect=query
        ), mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            side_effect=self.real_driver_selected_helper_paths,
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
                compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment,
                working_directory=self.build,
                preprocess_arguments=("-c", str(self.source.resolve())),
            )
        self.addCleanup(capability.native_owner.close)
        self.assertEqual(queries, [("-print-prog-name=cc1plus",)])
        closure_names = {
            item.role_relative_path.name
            for item in capability.resolved_runtime_closure
        }
        self.assertIn("cc1plus", closure_names)
        self.assertTrue(closure_names.isdisjoint({"cc1", "collect2", "as", "ld"}))

    def test_attached_msvc_language_selectors_bind_exact_frontend_and_memo_source(self):
        from gpu_capability_command import _preprocess_helper_selection_key

        controls = (
            (("/Tcfile.cpp",), "c", "/Tcfile.cpp"),
            (("/Tpfile.c",), "c++", "/Tpfile.c"),
            (("/TP", "/Tcfile.cpp"), "c", "/Tcfile.cpp"),
            (("/Tcfile.cpp", "/TP"), "c", "/Tcfile.cpp"),
        )
        keys = [
            _preprocess_helper_selection_key(
                arguments, CompilerFamily.MSVC, self.build
            )
            for arguments, _expected_language, _expected_binding in controls
        ]
        for (arguments, expected_language, expected_binding), key in zip(controls, keys):
            with self.subTest(arguments=arguments):
                self.assertEqual(key[0], expected_language)
                self.assertIn(expected_binding, key[1])
        self.assertNotEqual(keys[0], keys[2])
        self.assertNotEqual(
            keys[0],
            _preprocess_helper_selection_key(
                ("/Tcother.cpp",), CompilerFamily.MSVC, self.build
            ),
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "conflicting.*language.*selector"
        ):
            _preprocess_helper_selection_key(
                ("/Tcfile.cpp", "/Tpfile.cpp"),
                CompilerFamily.MSVC,
                self.build,
            )

        c_frontend = self.compiler.parent / "c1.dll"
        cxx_frontend = self.compiler.parent / "c1xx.dll"
        c_frontend.write_bytes(b"c frontend")
        cxx_frontend.write_bytes(b"cxx frontend")
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            side_effect=self.real_driver_selected_helper_paths,
        ):
            c_capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
                compiler_family=CompilerFamily.MSVC,
                launcher_environment=self.environment,
                working_directory=self.build,
                preprocess_arguments=("/Tcfile.cpp",),
            )
            cxx_capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
                compiler_family=CompilerFamily.MSVC,
                launcher_environment=self.environment,
                working_directory=self.build,
                preprocess_arguments=("/Tpfile.c",),
            )
        self.assertIsNot(c_capability, cxx_capability)
        self.assertIn(
            "c1.dll",
            {item.role_relative_path.name for item in c_capability.resolved_runtime_closure},
        )
        self.assertIn(
            "c1xx.dll",
            {item.role_relative_path.name for item in cxx_capability.resolved_runtime_closure},
        )

    def test_posix_probe_containment_consumes_owned_process_group_once(self):
        class Process:
            pid = 5151

        with mock.patch(
            "gpu_capability_command.os.name", "posix"
        ), mock.patch(
            "gpu_capability_command.os.killpg", create=True
        ) as killpg, mock.patch(
            "gpu_capability_command.signal.SIGKILL", 9, create=True
        ):
            containment = capability_command._ProbeContainment()
            containment.attach(Process())
            containment.terminate()
            containment.close()
            containment.close()
        killpg.assert_called_once_with(5151, 9)

    def test_delay_import_directory_obeys_context_metadata_ceiling(self):
        image = CompilerIdentificationTests.pe_delay_import_runtime_image(
            "delay-runtime.dll"
        )
        with mock.patch(
            "gpu_capability_command._RUNTIME_CONTEXT_METADATA_BYTES", 63
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "PE delay import metadata ceiling"
        ):
            capability_command._pe_runtime_import_names(image)

    def test_shared_runtime_is_traversed_in_driver_and_helper_loader_contexts(self):
        common = self.compiler.parent / "common"
        driver_only = self.compiler.parent / "driver-only"
        helper_directory = self.compiler.parent / "libexec"
        helper_only = helper_directory / "helper-only"
        for directory in (common, driver_only, helper_directory, helper_only):
            directory.mkdir(exist_ok=True)
        helper = helper_directory / "cc1.exe"
        shared = common / "libshared.so"
        driver_transitive = driver_only / "libcontext.so"
        helper_transitive = helper_only / "libcontext.so"
        self.compiler.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(shared.name,),
            rpath=("$ORIGIN/common:$ORIGIN/driver-only",),
        ))
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(shared.name,),
            rpath=("$ORIGIN/../common:$ORIGIN/helper-only",),
        ))
        shared.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(driver_transitive.name,),
        ))
        driver_transitive.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        helper_transitive.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment, working_directory=self.build,
            )
        closure_paths = {
            item.role_relative_path.as_posix()
            for item in capability.resolved_runtime_closure
        }
        self.assertIn("driver-only/libcontext.so", closure_paths)
        self.assertIn("libexec/helper-only/libcontext.so", closure_paths)

    def test_macho_shared_runtime_uses_each_inherited_rpath_context(self):
        helper_directory = self.compiler.parent / "libexec"
        common = self.compiler.parent / "common"
        driver_only = self.compiler.parent / "driver-only"
        helper_only = helper_directory / "helper-only"
        for directory in (helper_directory, common, driver_only, helper_only):
            directory.mkdir(exist_ok=True)
        helper = helper_directory / "cc1"
        shared = common / "libshared.dylib"
        driver_transitive = driver_only / "libcontext.dylib"
        helper_transitive = helper_only / "libcontext.dylib"
        for path in (helper, shared, driver_transitive, helper_transitive):
            path.write_bytes(b"runtime")
        imports = {
            self.compiler.resolve(): capability_command._RuntimeImports(
                (str(shared.resolve()),), ("@loader_path/driver-only",), (), "macho"
            ),
            helper.resolve(): capability_command._RuntimeImports(
                (str(shared.resolve()),), ("@loader_path/helper-only",), (), "macho"
            ),
            shared.resolve(): capability_command._RuntimeImports(
                ("@rpath/libcontext.dylib",), (), (), "macho"
            ),
            driver_transitive.resolve(): capability_command._RuntimeImports(
                (), (), (), "macho"
            ),
            helper_transitive.resolve(): capability_command._RuntimeImports(
                (), (), (), "macho"
            ),
        }
        with mock.patch(
            "gpu_capability_command._binary_runtime_imports",
            side_effect=lambda path, **_kwargs: imports[path.resolve()],
        ):
            paths, _aliases = capability_command._recursive_runtime_paths(
                (self.compiler.resolve(), helper.resolve()),
                self.compiler.resolve(), self.dependency_roots, "macos", {},
                self.build, time.monotonic() + 10.0, None,
            )
        self.assertIn(driver_transitive.resolve(), paths)
        self.assertIn(helper_transitive.resolve(), paths)

    def test_windows_shared_runtime_uses_each_executable_loader_context(self):
        helper_directory = self.compiler.parent / "libexec"
        helper_directory.mkdir()
        helper = helper_directory / "cc1.exe"
        shared = self.compiler.parent / "shared.dll"
        driver_transitive = self.compiler.parent / "context.dll"
        helper_transitive = helper_directory / "context.dll"
        for path in (helper, shared, driver_transitive, helper_transitive):
            path.write_bytes(b"runtime")
        imports = {
            self.compiler.resolve(): capability_command._RuntimeImports(
                (str(shared.resolve()),), (), (), "pe"
            ),
            helper.resolve(): capability_command._RuntimeImports(
                (str(shared.resolve()),), (), (), "pe"
            ),
            shared.resolve(): capability_command._RuntimeImports(
                ("context.dll",), (), (), "pe"
            ),
            driver_transitive.resolve(): capability_command._RuntimeImports(
                (), (), (), "pe"
            ),
            helper_transitive.resolve(): capability_command._RuntimeImports(
                (), (), (), "pe"
            ),
        }
        with mock.patch(
            "gpu_capability_command._binary_runtime_imports",
            side_effect=lambda path, **_kwargs: imports[path.resolve()],
        ), mock.patch(
            "gpu_capability_command._windows_known_dlls", return_value=frozenset()
        ):
            paths, _aliases = capability_command._recursive_runtime_paths(
                (self.compiler.resolve(), helper.resolve()),
                self.compiler.resolve(), self.dependency_roots, "windows", {},
                self.build, time.monotonic() + 10.0, None,
            )
        self.assertIn(driver_transitive.resolve(), paths)
        self.assertIn(helper_transitive.resolve(), paths)

    def test_windows_api_set_delay_contracts_do_not_require_filesystem_paths(self):
        imports = capability_command._RuntimeImports(
            ("ext-ms-onecore-appmodel-test-l1-1-0.dll",),
            format_kind="pe",
        )
        with mock.patch(
            "gpu_capability_command._binary_runtime_imports", return_value=imports
        ), mock.patch(
            "gpu_capability_command._windows_known_dlls", return_value=frozenset()
        ):
            paths, _aliases = capability_command._recursive_runtime_paths(
                (self.compiler.resolve(),), self.compiler.resolve(),
                self.dependency_roots, "windows", {}, self.build,
                time.monotonic() + 10.0, None,
            )
        self.assertEqual(paths, (self.compiler.resolve(),))

    def test_absent_optional_delay_import_does_not_become_required_at_launch(self):
        name = "optional-delay-runtime-that-is-absent.dll"
        imports = capability_command._RuntimeImports(
            (name,), format_kind="pe", optional_names=(name,)
        )
        with mock.patch(
            "gpu_capability_command._binary_runtime_imports", return_value=imports
        ), mock.patch(
            "gpu_capability_command._windows_known_dlls", return_value=frozenset()
        ):
            paths, _aliases = capability_command._recursive_runtime_paths(
                (self.compiler.resolve(),), self.compiler.resolve(),
                self.dependency_roots, "windows", {}, self.build,
                time.monotonic() + 10.0, None,
            )
        self.assertEqual(paths, (self.compiler.resolve(),))

    def test_windows_loaded_module_cycle_is_traversed_once_per_process(self):
        runtime = self.compiler.parent / "cycle-runtime.dll"
        runtime.write_bytes(b"cycle runtime")
        imports = {
            self.compiler.resolve(): capability_command._RuntimeImports(
                (runtime.name,), format_kind="pe"
            ),
            runtime.resolve(): capability_command._RuntimeImports(
                (self.compiler.name,), format_kind="pe"
            ),
        }
        with mock.patch(
            "gpu_capability_command._binary_runtime_imports",
            side_effect=lambda path, **_kwargs: imports[path.resolve()],
        ) as parse, mock.patch(
            "gpu_capability_command._windows_known_dlls", return_value=frozenset()
        ):
            paths, _aliases = capability_command._recursive_runtime_paths(
                (self.compiler.resolve(),), self.compiler.resolve(),
                self.dependency_roots, "windows", {}, self.build,
                time.monotonic() + 10.0, None,
            )
        self.assertEqual(paths, (self.compiler.resolve(), runtime.resolve()))
        self.assertEqual(parse.call_count, 2)

    def test_runtime_loader_context_state_and_metadata_are_bounded(self):
        empty = capability_command._RuntimeImports((), (), (), "elf")
        for limit_name, message in (
            ("_RUNTIME_CONTEXT_STATES", "context state ceiling"),
            ("_RUNTIME_CONTEXT_METADATA_BYTES", "context metadata ceiling"),
        ):
            with self.subTest(limit=limit_name), mock.patch(
                f"gpu_capability_command.{limit_name}", 0
            ), mock.patch(
                "gpu_capability_command._binary_runtime_imports", return_value=empty
            ), mock.patch(
                "gpu_capability_command._read_linux_loader_cache", return_value={}
            ), self.assertRaisesRegex(AuditInfrastructureError, message):
                capability_command._recursive_runtime_paths(
                    (self.compiler.resolve(),), self.compiler.resolve(),
                    self.dependency_roots, "linux", {}, self.build,
                    time.monotonic() + 10.0, None,
                )

    def test_runtime_context_state_ceiling_is_per_launched_executable(self):
        helper = self.compiler.parent / "cc1.exe"
        helper.write_bytes(b"helper")
        empty = capability_command._RuntimeImports((), format_kind="pe")
        with mock.patch(
            "gpu_capability_command._RUNTIME_CONTEXT_STATES", 1
        ), mock.patch(
            "gpu_capability_command._binary_runtime_imports", return_value=empty
        ), mock.patch(
            "gpu_capability_command._windows_known_dlls", return_value=frozenset()
        ):
            paths, _aliases = capability_command._recursive_runtime_paths(
                (self.compiler.resolve(), helper.resolve()),
                self.compiler.resolve(), self.dependency_roots, "windows", {},
                self.build, time.monotonic() + 10.0, None,
            )
        self.assertEqual(paths, (self.compiler.resolve(), helper.resolve()))

    def test_unresolved_loader_import_fails_closed(self):
        nested = self.compiler.parent / "libexec"
        nested.mkdir()
        helper = nested / "cc1.exe"
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=("missing-runtime.so",), runpath=("$ORIGIN",),
        ))
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ), self.assertRaisesRegex(AuditInfrastructureError, "unresolved runtime import"):
            open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment, working_directory=self.build,
            )

    def test_loader_rejects_first_existing_candidate_outside_authority(self):
        outside = self.root / "outside-runtime"
        authorized = self.compiler.parent / "authorized-runtime"
        outside.mkdir()
        authorized.mkdir()
        name = "libfirst-effective.so"
        (outside / name).write_bytes(CompilerIdentificationTests.elf_runtime_image())
        (authorized / name).write_bytes(CompilerIdentificationTests.elf_runtime_image())
        imports = capability_command._RuntimeImports((name,), format_kind="elf")
        with mock.patch(
            "gpu_capability_command._loader_default_directories",
            return_value=(authorized,),
        ), self.assertRaisesRegex(AuditInfrastructureError, "authority"):
            capability_command._resolve_runtime_name(
                name,
                self.compiler.resolve(),
                self.compiler.resolve(),
                "linux",
                imports,
                (),
                self.dependency_roots,
                {"LD_LIBRARY_PATH": str(outside)},
                self.build,
            )

    def test_linux_loader_uses_cache_before_configured_default(self):
        cache_directory = self.compiler.parent / "cache-runtime"
        default_directory = self.compiler.parent / "default-runtime"
        cache_directory.mkdir()
        default_directory.mkdir()
        name = "libcached-first.so"
        cached = cache_directory / name
        default = default_directory / name
        cached.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        default.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        imports = capability_command._RuntimeImports((name,), format_kind="elf")
        with mock.patch(
            "gpu_capability_command._linux_loader_cache_candidates",
            return_value=(cached,), create=True,
        ), mock.patch(
            "gpu_capability_command._loader_default_directories",
            return_value=(default_directory,),
        ):
            resolved, _aliases, _inherited = capability_command._resolve_runtime_name(
                name, self.compiler.resolve(), self.compiler.resolve(), "linux",
                imports, (), self.dependency_roots, {}, self.build,
            )
        self.assertEqual(resolved, cached.resolve())

    def test_windows_loader_reuses_loaded_module_before_application_copy(self):
        loaded_directory = self.compiler.parent / "loaded-runtime"
        loaded_directory.mkdir()
        name = "already-loaded.dll"
        loaded = loaded_directory / name
        application_copy = self.compiler.parent / name
        loaded.write_bytes(b"loaded")
        application_copy.write_bytes(b"application")
        imports = capability_command._RuntimeImports((name,), format_kind="pe")
        parameters = inspect.signature(
            capability_command._resolve_runtime_name
        ).parameters
        self.assertIn("loaded_modules", parameters)
        resolved, _aliases, _inherited = capability_command._resolve_runtime_name(
            name, self.compiler.resolve(), self.compiler.resolve(), "windows",
            imports, (), self.dependency_roots, {}, self.build,
            loaded_modules={name.casefold(): loaded.resolve()}, known_dlls=frozenset(),
        )
        self.assertEqual(resolved, loaded.resolve())

    def test_windows_loader_uses_known_dll_before_application_copy(self):
        windows = self.root / "windows"
        system32 = windows / "System32"
        system = windows / "System"
        system32.mkdir(parents=True)
        system.mkdir()
        name = "known-runtime.dll"
        known = system32 / name
        application_copy = self.compiler.parent / name
        known.write_bytes(b"known")
        application_copy.write_bytes(b"application")
        authority = build_dependency_root_authority(
            self.source_root,
            {"toolchain": self.compiler.parent, "windows-system": windows},
        )
        imports = capability_command._RuntimeImports((name,), format_kind="pe")
        with mock.patch(
            "gpu_capability_command._loader_default_directories",
            return_value=(system32, system, windows),
        ):
            resolved, _aliases, _inherited = capability_command._resolve_runtime_name(
                name, self.compiler.resolve(), self.compiler.resolve(), "windows",
                imports, (), authority, {}, self.build,
                loaded_modules={}, known_dlls=frozenset({name.casefold()}),
            )
        self.assertEqual(resolved, known.resolve())

    def test_windows_loader_does_not_search_dependent_importer_directory(self):
        importer_directory = self.compiler.parent / "nested"
        path_directory = self.compiler.parent / "path-runtime"
        importer_directory.mkdir()
        path_directory.mkdir()
        importer = importer_directory / "dependent.dll"
        importer.write_bytes(b"dependent")
        name = "ordinary-runtime.dll"
        (importer_directory / name).write_bytes(b"importer-decoy")
        selected = path_directory / name
        selected.write_bytes(b"path-selected")
        imports = capability_command._RuntimeImports((name,), format_kind="pe")
        with mock.patch(
            "gpu_capability_command._loader_default_directories",
            return_value=(),
        ):
            resolved, _aliases, _inherited = capability_command._resolve_runtime_name(
                name, importer.resolve(), self.compiler.resolve(), "windows",
                imports, (), self.dependency_roots,
                {"PATH": str(path_directory)}, self.build,
                loaded_modules={}, known_dlls=frozenset(),
            )
        self.assertEqual(resolved, selected.resolve())

    def test_macho_loader_uses_explicit_fallback_path_for_bare_name(self):
        fallback = self.compiler.parent / "fallback-runtime"
        fallback.mkdir()
        name = "libfallback.dylib"
        runtime = fallback / name
        runtime.write_bytes(b"runtime")
        imports = capability_command._RuntimeImports((name,), format_kind="macho")
        resolved, _aliases, _inherited = capability_command._resolve_runtime_name(
            name, self.compiler.resolve(), self.compiler.resolve(), "macos",
            imports, (), self.dependency_roots,
            {"DYLD_FALLBACK_LIBRARY_PATH": str(fallback)}, self.build,
        )
        self.assertEqual(resolved, runtime.resolve())

    def test_runtime_closure_spans_distinct_authority_roots(self):
        helper_directory = self.compiler.parent / "libexec"
        helper_directory.mkdir()
        helper = helper_directory / "cc1.exe"
        runtime_root = self.root / "system-runtime"
        runtime_root.mkdir()
        runtime = runtime_root / "libsystem.so"
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(runtime.name,), runpath=("$ORIGIN/../../system-runtime",),
        ))
        runtime.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        authority = build_dependency_root_authority(
            self.source_root,
            {"toolchain": self.compiler.parent, "system-runtime": runtime_root},
        )
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), authority, time.monotonic() + 10.0,
                compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment,
                working_directory=self.build,
            )
        runtime_evidence = next(
            item for item in capability.resolved_runtime_closure
            if item.role_relative_path.name == runtime.name
        )
        self.assertEqual(runtime_evidence.stable_role, "system-runtime")
        self.assertIn(runtime_root.resolve(), capability.native_owner.directory_paths)

    def test_runtime_symlink_alias_and_real_target_are_both_guarded(self):
        nested = self.compiler.parent / "libexec"
        runtime_directory = self.compiler.parent / "runtime"
        real_directory = self.compiler.parent / "real"
        nested.mkdir()
        runtime_directory.mkdir()
        real_directory.mkdir()
        helper = nested / "cc1.exe"
        runtime = real_directory / "libnested.so.1"
        alias = runtime_directory / "libnested.so"
        helper.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=(alias.name,), runpath=("$ORIGIN/../runtime",),
        ))
        runtime.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        try:
            alias.symlink_to(runtime)
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._driver_selected_helper_paths",
            return_value=(helper.resolve(),),
        ):
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                launcher_environment=self.environment, working_directory=self.build,
            )
        self.assertIn(alias, capability.native_owner.alias_paths)
        self.assertIn(runtime.resolve(), capability.native_owner.file_paths)
        try:
            displaced = alias.with_name(f"{alias.name}.displaced")
            alias.rename(displaced)
            alias.symlink_to(runtime)
            alias.unlink()
            displaced.rename(alias)
            with self.assertRaisesRegex(AuditInfrastructureError, "generation change"):
                capability.native_owner.validate(
                    content=False, deadline=time.monotonic() + 10.0,
                    cancel_event=None,
                )
        finally:
            capability.native_owner.close()

    @unittest.skipIf(os.name == "nt", "directory symlink mutation proof runs under WSL")
    def test_nested_runtime_ancestor_symlink_swap_restore_is_guarded(self):
        real_directory = self.compiler.parent / "real-runtime-directory"
        alias_directory = self.compiler.parent / "runtime-directory-alias"
        real_directory.mkdir()
        runtime = real_directory / "libancestor.so"
        runtime.write_bytes(CompilerIdentificationTests.elf_runtime_image())
        try:
            alias_directory.symlink_to(real_directory, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks unavailable: {error}")

        resolved = capability_command._resolve_runtime_candidate(
            alias_directory / runtime.name, self.dependency_roots
        )
        self.assertIsNotNone(resolved)
        resolved_path, aliases = resolved
        self.assertIn(alias_directory, aliases)
        chains = capability_command._runtime_path_chains(
            self.dependency_roots, (resolved_path, *aliases)
        )
        observer = capability_command._FilesystemGenerationObserver(
            tuple((path, True) for path in chains)
            + tuple((path, False) for path in aliases)
            + ((resolved_path, False),)
        )
        displaced = alias_directory.with_name(f"{alias_directory.name}.displaced")
        try:
            alias_directory.rename(displaced)
            alias_directory.symlink_to(real_directory, target_is_directory=True)
            alias_directory.unlink()
            displaced.rename(alias_directory)
            with self.assertRaisesRegex(AuditInfrastructureError, "generation change"):
                observer.drain()
        finally:
            observer.close()

    def test_runtime_aggregate_is_reserved_before_any_binary_payload_read(self):
        runtimes = []
        for index in range(5):
            runtime = self.compiler.parent / f"runtime-{index}.dll"
            with runtime.open("wb") as stream:
                stream.truncate(220 * 1024 * 1024)
            runtimes.append(runtime)
        self.compiler.write_bytes(CompilerIdentificationTests.elf_runtime_image(
            needed=tuple(runtime.name for runtime in runtimes),
            runpath=("$ORIGIN",),
        ))
        _clear_compiler_inspection_memo_for_tests()
        original_imports = capability_command._binary_runtime_imports

        def reject_runtime_payload(path, **kwargs):
            if path.resolve() in {runtime.resolve() for runtime in runtimes}:
                raise AssertionError(
                    "runtime payload read before aggregate reservation"
                )
            return original_imports(path, **kwargs)

        with mock.patch(
            "gpu_capability_command._binary_runtime_imports",
            side_effect=reject_runtime_payload,
        ), self.assertRaisesRegex(AuditInfrastructureError, "total byte ceiling"):
            open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
            )

    def test_runtime_binary_parser_honors_cancellation_and_deadline(self):
        runtime = self.compiler.parent / "runtime.dll"
        runtime.write_bytes(b"MZ" + b"x" * (2 * 1024 * 1024))
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            capability_command._binary_runtime_import_names(
                runtime, deadline=time.monotonic() + 10.0,
                cancel_event=cancelled,
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            capability_command._binary_runtime_import_names(
                runtime, deadline=time.monotonic() - 1.0,
                cancel_event=None,
            )

    def test_helper_runtime_replacement_and_nested_ancestor_swap_fail_closed(self):
        nested = self.compiler.parent / "libexec" / "nested"
        nested.mkdir(parents=True)
        helper = nested / "cc1.exe"
        runtime = nested / "runtime.dll"
        helper.write_bytes(b"helper")
        runtime.write_bytes(b"runtime")
        for target, is_directory in ((helper, False), (runtime, False), (nested, True)):
            _clear_compiler_inspection_memo_for_tests()
            with mock.patch(
                "gpu_capability_command._driver_selected_helper_paths",
                return_value=(helper.resolve(), runtime.resolve()),
            ):
                capability = open_compiler_executable_capability(
                    self.compiler.resolve(), self.dependency_roots,
                    time.monotonic() + 10.0, compiler_family=CompilerFamily.GCC,
                    launcher_environment=self.environment,
                    working_directory=self.build,
                )
            prevented = False
            try:
                if is_directory:
                    moved = target.with_name(target.name + ".moved")
                    target.rename(moved)
                    moved.rename(target)
                else:
                    original = target.read_bytes()
                    target.write_bytes(original + b"changed")
                    target.write_bytes(original)
            except OSError:
                prevented = True
            if prevented:
                capability.native_owner.validate()
            else:
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "generation|path chain|content"
                ):
                    capability.native_owner.validate()

    def test_permit_to_exec_and_during_exec_changes_fail_closed(self):
        class Containment:
            requires_handshake = False
            popen_arguments = {}

            def prepare_command(self, command): return command
            def attach(self, _process): pass
            def release(self, _process): pass
            def close(self): pass
            def terminate(self): pass

        class Process:
            returncode = 0
            pid = 4242

            def __init__(self, stdout, mutate_on_poll=None):
                self.stdout = stdout
                self.mutate_on_poll = mutate_on_poll
                self.polled = False
                stdout.write(b"g++ (GCC) 14.1.0\n")
                stdout.flush()

            def poll(self):
                if not self.polled and self.mutate_on_poll is not None:
                    self.polled = True
                    self.mutate_on_poll()
                return self.returncode

            def wait(self, timeout=None): return self.returncode
            def kill(self): self.returncode = -9

        for phase in ("permit", "during"):
            _clear_compiler_inspection_memo_for_tests()
            capability = open_compiler_executable_capability(
                self.compiler.resolve(), self.dependency_roots,
                time.monotonic() + 10.0,
            )
            state = {"prevented": False}

            def mutate_restore():
                try:
                    original = self.compiler.read_bytes()
                    self.compiler.write_bytes(original + b"changed")
                    self.compiler.write_bytes(original)
                except OSError:
                    state["prevented"] = True

            def launch(*_args, **kwargs):
                if phase == "permit":
                    mutate_restore()
                return Process(
                    kwargs["stdout"],
                    mutate_restore if phase == "during" else None,
                )

            detected = False
            with mock.patch(
                "gpu_capability_command._ProbeContainment", Containment
            ), mock.patch(
                "gpu_capability_command.subprocess.Popen", side_effect=launch
            ), mock.patch(
                "gpu_capability_command._native_process_start_token",
                return_value="test-process-start",
            ):
                try:
                    capability_command._run_probe_command(
                        capability, ("--version",), self.build,
                        self.environment, time.monotonic() + 10.0,
                    )
                except AuditInfrastructureError as error:
                    detected = bool(re.search("generation|changed|content", str(error)))
            self.assertTrue(
                state["prevented"] or detected,
                f"{phase} change was neither prevented nor detected",
            )

    def test_compiler_content_metadata_and_version_all_affect_digest(self):
        baseline = self.make()
        _clear_compiler_inspection_memo_for_tests()
        self.compiler.write_bytes(b"compiler-content-b")
        content_changed = self.make()
        self.assertNotEqual(baseline.digest, content_changed.digest)

        _clear_compiler_inspection_memo_for_tests()
        before = self.compiler.stat().st_mtime_ns
        os.utime(self.compiler, ns=(before + 10_000_000, before + 10_000_000))
        metadata_changed = self.make()
        self.assertNotEqual(content_changed.digest, metadata_changed.digest)

        # A version change with otherwise identical process-local memo inputs
        # represents a subsequent audit process.
        _clear_compiler_inspection_memo_for_tests()
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 14.0.0\n",
        ):
            version_changed = make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits(), self.dependency_roots
            )
        self.assertNotEqual(metadata_changed.digest, version_changed.digest)

    def test_compiler_cannot_change_between_version_probe_and_fingerprint(self):
        def replace_during_probe(*_args, **_kwargs):
            try:
                self.compiler.write_bytes(b"replacement-compiler-content")
            except OSError as error:
                raise AuditInfrastructureError(
                    "compiler executable changed during compiler version probe"
                ) from error
            return b"g++.exe (GCC) 13.1.0\n"

        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            side_effect=replace_during_probe,
        ), self.assertRaisesRegex(
            AuditInfrastructureError,
            "changed during compiler version probe|generation change observed",
        ):
            make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits(), self.dependency_roots
            )

    def test_normalized_version_output_is_stable(self):
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0   \nTarget: mingw\r\n\r\n",
        ):
            first = make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits(), self.dependency_roots
            )
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0\r\nTarget: mingw\n",
        ):
            second = make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits(), self.dependency_roots
            )
        self.assertEqual(first.digest, second.digest)

    def test_entry_id_changes_but_semantic_digest_does_not(self):
        first = self.make(index=1)
        second = self.make(index=9)
        self.assertNotEqual(first.entry_id, second.entry_id)
        self.assertEqual(first.digest, second.digest)

    def test_concurrent_configuration_builds_share_one_stable_inspection(self):
        _clear_compiler_inspection_memo_for_tests()
        barrier = threading.Barrier(4)
        configurations = []
        failures = []

        def build(index: int) -> None:
            try:
                barrier.wait(timeout=2.0)
                configurations.append(
                    make_configuration(
                        self.entry(),
                        self.database,
                        index,
                        self.source_root,
                        self.production,
                        self.environment,
                        AuditLimits(),
                        self.dependency_roots,
                    )
                )
            except BaseException as error:
                failures.append(error)

        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0\n",
        ) as probe:
            threads = [threading.Thread(target=build, args=(index,)) for index in range(4)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=5.0)

        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertFalse(failures)
        self.assertEqual(len(configurations), 4)
        self.assertEqual(probe.call_count, 1)
        self.assertEqual(len({item.digest for item in configurations}), 1)

    def test_memo_hit_rejects_compiler_replacement_after_key_snapshot(self):
        baseline = self.make()
        replaced = False

        def replace_after_snapshot(compiler: Path):
            nonlocal replaced
            snapshot = _compiler_metadata_snapshot(compiler)
            if not replaced:
                replaced = True
                try:
                    compiler.write_bytes(b"compiler-replaced-after-cache-key")
                except OSError as error:
                    raise AuditInfrastructureError(
                        "compiler executable changed during compiler version probe"
                    ) from error
            return snapshot

        with mock.patch(
            "gpu_capability_command._compiler_metadata_snapshot",
            side_effect=replace_after_snapshot,
        ), mock.patch(
            "gpu_capability_command._probe_compiler_version"
        ) as probe, self.assertRaisesRegex(
            AuditInfrastructureError,
            "compiler executable changed during compiler version probe|"
            "generation change observed",
        ):
            make_configuration(
                self.entry(),
                self.database,
                4,
                self.source_root,
                self.production,
                self.environment,
                AuditLimits(),
                self.dependency_roots,
            )

        self.assertTrue(replaced)
        probe.assert_not_called()
        self.assertRegex(baseline.digest, r"^[0-9a-f]{64}$")

    def test_source_validation_rejects_stdin_multiple_and_mismatch(self):
        controls = (
            ([str(self.compiler), "-c", "-"], "stdin"),
            ([str(self.compiler), "-c", "../playback/gpu/file.cpp", "other.cpp"], "multiple source"),
            ([str(self.compiler), "-c", "other.cpp"], "does not match"),
        )
        for arguments, message in controls:
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                self.make(self.entry(arguments=arguments))

    def test_source_validation_rejects_extensionless_input_under_forced_language(self):
        entry = self.entry(arguments=[
            str(self.compiler), "-x", "c++", "../playback/gpu/file.cpp", "extra"
        ])
        with self.assertRaisesRegex(AuditInfrastructureError, "multiple source"):
            self.make(entry)

    def test_split_define_payload_that_looks_like_source_is_not_an_input(self):
        entry = self.entry(arguments=[
            str(self.compiler), "-D", "BUILD_FILE=file.cpp", "-c",
            "../playback/gpu/file.cpp",
        ])
        configuration = self.make(entry)
        self.assertEqual(configuration.source.relative, PurePosixPath("playback/gpu/file.cpp"))

    def test_msvc_dash_options_preserve_source_and_value_semantics(self):
        from gpu_capability_command import _source_inputs

        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family, option="-Tpother"):
                self.assertEqual(
                    len(_source_inputs(("main.cpp", "-Tpother"), self.build, family)),
                    2,
                )
            for option in ("-D", "-FI"):
                with self.subTest(family=family, option=option):
                    self.assertEqual(
                        _source_inputs(
                            (option, "BUILD_FILE=file.cpp", "main.cpp"),
                            self.build,
                            family,
                        ),
                        ((self.build / "main.cpp").resolve(),),
                    )

    def test_msvc_global_language_flags_do_not_consume_the_next_argument(self):
        from gpu_capability_command import _source_inputs

        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/TP", "/TC", "-TP", "-TC"):
                arguments = (option, "/D", "BUILD_FILE=file.cpp", "main.cpp")
                with self.subTest(family=family, option=option):
                    self.assertEqual(
                        _source_inputs(arguments, self.build, family),
                        ((self.build / "main.cpp").resolve(),),
                    )

    def test_msvc_per_file_language_flags_keep_case_sensitive_source_arity(self):
        from gpu_capability_command import _source_inputs

        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/Tp", "/Tc", "-Tp", "-Tc"):
                with self.subTest(family=family, option=option):
                    self.assertEqual(
                        _source_inputs((option, "main.cpp"), self.build, family),
                        ((self.build / "main.cpp").resolve(),),
                    )

    def test_msvc_exact_case_value_options_preserve_source_arity(self):
        from gpu_capability_command import _source_inputs

        options = (
            "/D", "/U", "/I", "/FI", "/Fo", "/Fe", "/Fd", "/Fi",
            "/Fp", "/Ft", "/Yu", "/Yc", "/sourceDependencies",
            "/scanDependencies", "/external:I", "/AI", "/FU",
            "/ifcOutput", "/reference", "/headerUnit",
            "/experimental:log",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in options:
                for spelling in (option, f"-{option[1:]}"):
                    with self.subTest(family=family, option=spelling):
                        self.assertEqual(
                            _source_inputs(
                                (spelling, "operand.file", "main.cpp"),
                                self.build,
                                family,
                            ),
                            ((self.build / "main.cpp").resolve(),),
                        )

    def test_msvc_case_distinct_options_do_not_acquire_other_option_arity(self):
        from gpu_capability_command import _source_inputs

        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/u", "/C", "/utf-8", "/fp:fast", "/favor:AMD64", "/interface"):
                with self.subTest(family=family, option=option):
                    self.assertEqual(
                        _source_inputs((option, "main.cpp"), self.build, family),
                        ((self.build / "main.cpp").resolve(),),
                    )

    def test_clang_cl_gnu_arity_does_not_hide_a_source_via_msvc_misspelling(self):
        from gpu_capability_command import _source_inputs

        with self.assertRaisesRegex(AuditInfrastructureError, "case-sensitive.*option"):
            _source_inputs(
                ("-d", "hidden.cpp", "main.cpp"),
                self.build,
                CompilerFamily.CLANG_CL,
            )

    def test_entry_source_outside_production_and_unknown_wrapper_fail(self):
        outside = self.root / "outside.cpp"
        outside.write_text("int x;\n", encoding="utf-8")
        controls = (
            (self.entry(file=str(outside), arguments=[str(self.compiler), str(outside)]), "production"),
            (self.entry(arguments=["mystery-wrapper", str(self.compiler), "../playback/gpu/file.cpp"]), "unsupported compiler"),
        )
        for entry, message in controls:
            with self.subTest(entry=entry), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                self.make(entry)

    def test_ccache_compiler_environment_override_fails_closed(self):
        entry = self.entry(arguments=[
            "ccache", str(self.compiler), "../playback/gpu/file.cpp"
        ])
        environment = dict(self.environment, CCACHE_COMPILER="clang++")
        with self.assertRaisesRegex(AuditInfrastructureError, "CCACHE_COMPILER"):
            self.make(entry, environment=environment)

    def test_ccache_selected_config_rejects_compiler_override_but_allows_benign_config(self):
        entry_arguments = lambda path: [
            "ccache", "--config-path", str(path), str(self.compiler),
            "../playback/gpu/file.cpp",
        ]
        bad = self.build / "bad-ccache.conf"
        bad.write_text("compiler = clang++\n", encoding="utf-8")
        with self.assertRaisesRegex(AuditInfrastructureError, "ccache config.*compiler"):
            self.make(self.entry(arguments=entry_arguments(bad)))
        benign = self.build / "benign-ccache.conf"
        benign.write_text("compiler_check = content\n", encoding="utf-8")
        self.make(self.entry(arguments=entry_arguments(benign)))
        with self.assertRaisesRegex(AuditInfrastructureError, "CCACHE_CC"):
            self.make(
                self.entry(arguments=["ccache", str(self.compiler), "../playback/gpu/file.cpp"]),
                environment=dict(self.environment, CCACHE_CC="clang++"),
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "ccache config.*prefix_command_cpp"):
            bad.write_text("prefix_command_cpp = distcc\n", encoding="utf-8")
            self.make(
                self.entry(arguments=["ccache", str(self.compiler), "../playback/gpu/file.cpp"]),
                environment=dict(self.environment, CCACHE_CONFIGPATH=str(bad)),
            )

    def test_msvc_cl_environment_arguments_fail_closed_before_double_application(self):
        compiler = self.compiler.with_name("cl.exe")
        compiler.write_bytes(self.compiler.read_bytes())
        response = self.build / "environment.rsp"
        response.write_text("/DTAIL=1", encoding="utf-8")
        entry = self.entry(arguments=[str(compiler), "../playback/gpu/file.cpp"])
        for name, value in (("CL", "/DHEAD=1"), ("_CL_", f"@{response}")):
            with self.subTest(name=name), mock.patch(
                "gpu_capability_command._probe_compiler_version",
                return_value=b"Microsoft (R) C/C++ Optimizing Compiler Version 19.44\n",
            ), self.assertRaisesRegex(AuditInfrastructureError, rf"{name}.*unsupported"):
                make_configuration(
                    entry, self.database, 3, self.source_root, self.production,
                    dict(self.environment, **{name: value}), AuditLimits(),
                    self.dependency_roots,
                )

    def test_clang_driver_and_response_dialect_overrides_fail_closed(self):
        clang = self.compiler.with_name("clang++.exe")
        clang.write_bytes(self.compiler.read_bytes())
        controls = (
            "--driver-mode=cl",
            "--driver-mode", "--rsp-quoting=windows", "--rsp-quoting",
        )
        for option in controls:
            arguments = [str(clang), option]
            if option in {"--driver-mode", "--rsp-quoting"}:
                arguments.append("cl" if option == "--driver-mode" else "windows")
            arguments.append("../playback/gpu/file.cpp")
            entry = self.entry(arguments=arguments)
            with self.subTest(option=option), mock.patch(
                "gpu_capability_command._probe_compiler_version",
                return_value=b"clang version 18.1.3\n",
            ), self.assertRaisesRegex(AuditInfrastructureError, "driver mode|response.*quoting"):
                make_configuration(
                    entry, self.database, 3, self.source_root, self.production,
                    self.environment, AuditLimits(), self.dependency_roots
                )

    def test_response_expansion_precedes_source_validation(self):
        (self.build / "source.rsp").write_text("other.cpp", encoding="utf-8")
        with self.assertRaisesRegex(AuditInfrastructureError, "does not match"):
            self.make(self.entry(arguments=[str(self.compiler), "@source.rsp"]))

    def test_compiler_probe_is_bounded_and_fail_closed(self):
        capability = open_compiler_executable_capability(
            self.compiler,
            self.dependency_roots,
            time.monotonic() + 60.0,
            compiler_family=CompilerFamily.GCC,
        )
        self.addCleanup(capability.native_owner.close)

        class FakeProcess:
            pid = 4242

            def __init__(self, *, stdout, payload=b"", running=False, **_kwargs):
                stdout.write(payload)
                stdout.flush()
                self.returncode = None if running else 0
                self.killed = False

            def poll(self):
                return self.returncode

            def kill(self):
                self.killed = True
                self.returncode = -9

            def wait(self, timeout=None):
                return self.returncode

        def overflow_process(*_args, **kwargs):
            return FakeProcess(stdout=kwargs["stdout"], payload=b"x" * (1024 * 1024 + 1))

        with mock.patch(
            "gpu_capability_command.subprocess.Popen", side_effect=overflow_process
        ) as popen, mock.patch(
            "gpu_capability_command._native_process_start_token",
            return_value="test-process-start",
        ), mock.patch.object(
            capability_command._ProbeContainment, "attach"
        ), mock.patch.object(
            capability_command._ProbeContainment, "release"
        ):
            with self.assertRaisesRegex(AuditInfrastructureError, "version output limit"):
                from gpu_capability_command import _probe_compiler_version
                _probe_compiler_version(capability, CompilerFamily.GCC, self.build, self.environment)
            self.assertFalse(popen.call_args.kwargs["shell"])

        with mock.patch(
            "gpu_capability_command.subprocess.Popen",
            side_effect=lambda *_args, **kwargs: FakeProcess(
                stdout=kwargs["stdout"], running=True
            ),
        ), mock.patch(
            "gpu_capability_command.time.monotonic", side_effect=(10.0, 16.0)
        ), mock.patch(
            "gpu_capability_command.time.sleep"
        ), mock.patch(
            "gpu_capability_command._native_process_start_token",
            return_value="test-process-start",
        ), mock.patch.object(
            capability_command._ProbeContainment, "attach"
        ), mock.patch.object(
            capability_command._ProbeContainment, "release"
        ):
            with self.assertRaisesRegex(AuditInfrastructureError, "version probe timeout"):
                from gpu_capability_command import _probe_compiler_version
                _probe_compiler_version(capability, CompilerFamily.GCC, self.build, self.environment)

    def test_probe_post_launch_preparation_failure_cleans_once_and_preserves_primary(self):
        capability = open_compiler_executable_capability(
            self.compiler,
            self.dependency_roots,
            time.monotonic() + 60.0,
            compiler_family=CompilerFamily.GCC,
        )
        self.addCleanup(capability.native_owner.close)

        class FatalPreparation(BaseException):
            pass

        for primary in (RuntimeError("finish failed"), FatalPreparation("fatal")):
            with self.subTest(primary=type(primary).__name__):
                events = []

                class Containment:
                    requires_handshake = False

                    def prepare_command(_self, command): return command
                    def terminate(_self): events.append("terminate")
                    def close(_self): events.append("close")

                class Process:
                    def poll(_self): return None
                    def kill(_self): events.append("kill")
                    def wait(_self, timeout=None):
                        events.append(("wait", timeout))
                        return -9

                class Carrier:
                    completed = False

                    def complete_after_exit(_self):
                        _self.completed = True
                        events.append("carrier-complete")

                class Observer:
                    def prepare_compiler_inspection_launch(_self, *_args):
                        return "preparation"

                    def finish_compiler_inspection_launch_preparation(
                        _self, preparation
                    ):
                        self.assertEqual(preparation, "preparation")
                        raise primary

                    def cancel_compiler_inspection_launch_preparation(
                        _self, preparation
                    ):
                        self.assertEqual(preparation, "preparation")
                        events.append("cancel-preparation")

                containment = Containment()
                process = Process()
                carrier = Carrier()
                with mock.patch.object(
                    capability_command,
                    "_ProbeContainment",
                    return_value=containment,
                ), mock.patch.object(
                    capability_command,
                    "launch_compiler_process",
                    return_value=(process, carrier),
                ), self.assertRaises(type(primary)) as raised:
                    capability_command._run_probe_command(
                        capability,
                        ("--version",),
                        self.build,
                        self.environment,
                        time.monotonic() + 10.0,
                        launch_observer=Observer(),
                    )
                self.assertIs(raised.exception, primary)
                self.assertEqual(
                    events,
                    [
                        "cancel-preparation",
                        "terminate",
                        "kill",
                        ("wait", 1.0),
                        "carrier-complete",
                        "close",
                    ],
                )

    def test_probe_uses_owned_generation_guards_without_rehashing_full_closure(self):
        capability = open_compiler_executable_capability(
            self.compiler, self.dependency_roots, time.monotonic() + 10.0
        )
        self.addCleanup(capability.native_owner.close)

        class Containment:
            requires_handshake = False
            popen_arguments = {}

            def prepare_command(self, command): return command
            def attach(self, _process): pass
            def release(self, _process): pass
            def close(self): pass
            def terminate(self): pass

        class Process:
            returncode = 0
            pid = 4242

            class Handle:
                def Close(self): self.closed = True

            def __init__(self, *_args, stdout, **_kwargs):
                self._handle = self.Handle()
                stdout.write(b"g++ (GCC) 14.1.0\n")
                stdout.flush()

            def poll(self): return self.returncode
            def wait(self, timeout=None): return self.returncode
            def kill(self): self.returncode = -9

        owner = capability.native_owner
        with mock.patch.object(
            owner, "validate", wraps=owner.validate
        ) as validate, mock.patch(
            "gpu_capability_command._ProbeContainment", Containment
        ), mock.patch(
            "gpu_capability_command.subprocess.Popen", Process
        ), mock.patch(
            "gpu_capability_command._native_process_start_token",
            return_value="test-process-start",
        ), mock.patch.object(
            capability_command._ProbeContainment, "attach"
        ), mock.patch.object(
            capability_command._ProbeContainment, "release"
        ):
            capability_command._run_probe_command(
                capability, ("--version",), self.build, self.environment,
                time.monotonic() + 10.0,
            )
        self.assertEqual(
            [call.kwargs.get("content") for call in validate.call_args_list],
            [False, False],
        )

    def test_msvc_probe_uses_a_temporary_source_and_leaves_no_artifact(self):
        capability = open_compiler_executable_capability(
            self.compiler,
            self.dependency_roots,
            time.monotonic() + 60.0,
            compiler_family=CompilerFamily.MSVC,
        )
        self.addCleanup(capability.native_owner.close)
        captured: dict[str, object] = {}

        class FakeProcess:
            returncode = 0
            pid = 4242

            class Handle:
                def Close(self): self.closed = True

            def __init__(self, command, *, stderr, **_kwargs):
                self._handle = self.Handle()
                captured["command"] = tuple(command)
                source = Path(command[-1])
                captured["source"] = source
                self.assert_source_exists = source.is_file()
                stderr.write(
                    b"Microsoft (R) C/C++ Optimizing Compiler Version 19.44\n"
                )
                stderr.flush()

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                return self.returncode

        with mock.patch(
            "gpu_capability_command.subprocess.Popen", FakeProcess
        ), mock.patch(
            "gpu_capability_command._native_process_start_token",
            return_value="test-process-start",
        ), mock.patch.object(
            capability_command._ProbeContainment, "attach"
        ), mock.patch.object(
            capability_command._ProbeContainment, "release"
        ):
            from gpu_capability_command import _probe_compiler_version
            output = _probe_compiler_version(
                capability, CompilerFamily.MSVC, self.build, self.environment
            )
        command = captured["command"]
        self.assertIn("/Bv", command)
        self.assertIn("/EP", command)
        self.assertIn("/TP", command)
        self.assertTrue(command[-1].endswith(".cpp"))
        self.assertIn(b"Microsoft", output)
        self.assertFalse(captured["source"].exists())

    def test_version_probe_cleans_descendants_after_parent_exits(self):
        fixture = self.root / "probe-tree"
        fixture.mkdir()
        pid_file = fixture / "child.pid"
        startup_pid_file = fixture / "startup-child.pid"
        heartbeat = fixture / "heartbeat.txt"
        (fixture / "sitecustomize.py").write_text(
            "import os,pathlib,subprocess,sys\n"
            "if os.environ.get('GPU_PROBE_SITE_GUARD') != '1':\n"
            " os.environ['GPU_PROBE_SITE_GUARD']='1'\n"
            " child=subprocess.Popen([os.environ['GPU_PROBE_PYTHON'], '-c', 'import time; time.sleep(30)'], env=os.environ.copy())\n"
            " pathlib.Path(os.environ['GPU_PROBE_STARTUP_PID']).write_text(str(child.pid), encoding='ascii')\n",
            encoding="utf-8",
        )
        child_code = (
            "import os,pathlib,time\n"
            "target=pathlib.Path(os.environ['GPU_PROBE_HEARTBEAT'])\n"
            "while True:\n"
            " target.write_text(str(time.monotonic()), encoding='ascii')\n"
            " time.sleep(0.02)\n"
        )
        parent = fixture / "parent.py"
        parent.write_text(
            "import os,pathlib,subprocess,sys\n"
            f"child_code={child_code!r}\n"
            "child=subprocess.Popen([os.environ['GPU_PROBE_PYTHON'], '-c', child_code], env=os.environ.copy())\n"
            "pathlib.Path(os.environ['GPU_PROBE_PID']).write_text(str(child.pid), encoding='ascii')\n"
            "print('gcc (GCC) 13.1.0', flush=True)\n",
            encoding="utf-8",
        )
        environment = dict(
            os.environ,
            GPU_PROBE_PID=str(pid_file),
            GPU_PROBE_STARTUP_PID=str(startup_pid_file),
            GPU_PROBE_HEARTBEAT=str(heartbeat),
            GPU_PROBE_PYTHON=str(Path(sys.executable).resolve()),
            PYTHONPATH=str(fixture),
        )
        from gpu_capability_command import _run_probe_command, _WindowsProbeJob
        python_roots = {"python": Path(sys.executable).resolve().parent}
        if os.name == "nt":
            python_roots["windows-system"] = Path(
                os.environ.get("SystemRoot", "C:/Windows")
            ).resolve()
        else:
            runtime_imports = capability_command._binary_runtime_imports(
                Path(sys.executable)
            )
            for index, interpreter in enumerate(runtime_imports.interpreters):
                python_roots[f"python-interpreter-{index}"] = Path(
                    interpreter
                ).resolve().parent
        python_authority = build_dependency_root_authority(fixture, python_roots)
        capability = open_compiler_executable_capability(
            Path(sys.executable), python_authority, time.monotonic() + 60.0
        )
        self.addCleanup(capability.native_owner.close)
        if os.name == "nt":
            original_attach = _WindowsProbeJob.attach

            def delayed_attach(job, process):
                deadline = time.monotonic() + 0.5
                while not startup_pid_file.is_file() and time.monotonic() < deadline:
                    time.sleep(0.005)
                return original_attach(job, process)

            with mock.patch.object(_WindowsProbeJob, "attach", delayed_attach):
                output = _run_probe_command(
                    capability, (str(parent),), fixture, environment
                )
        else:
            output = _run_probe_command(
                capability, (str(parent),), fixture, environment
            )
        self.assertIn(b"gcc (GCC)", output)
        child_pids = (
            int(pid_file.read_text(encoding="ascii")),
            int(startup_pid_file.read_text(encoding="ascii")),
        )
        for child_pid in child_pids:
            deadline = time.monotonic() + 2.0
            while self._pid_is_alive(child_pid) and time.monotonic() < deadline:
                time.sleep(0.02)
            survived = self._pid_is_alive(child_pid)
            if survived:
                subprocess.run(
                    ["taskkill", "/PID", str(child_pid), "/T", "/F"],
                    capture_output=True,
                    check=False,
                ) if os.name == "nt" else os.kill(child_pid, 9)
            self.assertFalse(survived, "probe descendant survived cleanup")

    @staticmethod
    def _pid_is_alive(pid: int) -> bool:
        if os.name != "nt":
            try:
                os.kill(pid, 0)
            except OSError:
                return False
            return True
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (
            wintypes.DWORD, wintypes.BOOL, wintypes.DWORD
        )
        kernel32.OpenProcess.restype = wintypes.HANDLE
        process = kernel32.OpenProcess(0x00100000, False, pid)
        if not process:
            return False
        kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        try:
            return kernel32.WaitForSingleObject(process, 0) == 258
        finally:
            kernel32.CloseHandle(process)


class MacOSCompilerExecGateTests(unittest.TestCase):
    def test_gate_report_wait_honors_cancel_before_exec(self):
        read_descriptor, write_descriptor = os.pipe()
        cancelled = threading.Event()
        cancelled.set()
        try:
            with self.assertRaisesRegex(
                AuditInfrastructureError, "cancelled"
            ):
                capability_command._read_length_prefixed_fd(
                    read_descriptor,
                    time.monotonic() + 10.0,
                    cancelled,
                )
        finally:
            os.close(read_descriptor)
            os.close(write_descriptor)

    def test_gate_helper_rejects_invalid_permit_without_exec(self):
        report_read, report_write = os.pipe()
        permit_read, permit_write = os.pipe()
        errors = []

        def run_gate():
            try:
                capability_command._run_macos_compiler_exec_gate(
                    report_write,
                    permit_read,
                    "a" * 64,
                    ("/trusted/compiler", "--version"),
                )
            except BaseException as error:
                errors.append(error)

        with mock.patch.object(
            capability_command,
            "_native_macos_self_start_identity",
            return_value=(71, 71, "1:2"),
        ), mock.patch.object(capability_command.os, "execvpe") as execute:
            thread = threading.Thread(target=run_gate)
            thread.start()
            header = os.read(report_read, 4)
            length = struct.unpack(">I", header)[0]
            self.assertLessEqual(length, 4096)
            os.read(report_read, length)
            os.write(permit_write, struct.pack(">I", 2) + b"{}")
            os.close(permit_write)
            thread.join(timeout=2.0)
        os.close(report_read)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(errors), 1)
        self.assertRegex(str(errors[0]), "macOS compiler exec permit")
        execute.assert_not_called()

    def test_shared_boundary_authorizes_macos_gate_before_launch_event(self):
        order = []

        class Process:
            pid = 71
            stdin = None

            def kill(self): order.append("kill")
            def wait(self, timeout): order.append(("wait", timeout))

        class Containment:
            popen_arguments = {"start_new_session": True}
            requires_handshake = False

            def attach(self, _process): order.append("attach")
            def release(self, _process): order.append("release")
            def terminate(self): order.append("terminate")

        class Observer:
            macos_launch_deadline = time.monotonic() + 10.0

            def authorize_macos_compiler_exec(self, process_start):
                order.append(("authorize", process_start.pid))
                return capability_command.CompilerExecPermit(3, 7, 5, 71)

            def register_compiler_process_launch(self, _event, _carrier):
                order.append("register")

        class Gate:
            command = ("gate",)
            environment = {"GATE": "1"}
            launch_options = {"pass_fds": (90, 91)}

            def __init__(self, *_args, **_kwargs): order.append("gate-create")

            def authorize(self, process, observer, **identity):
                order.append("gate-report")
                start = capability_command.ProcessStartIdentity(
                    "macos", process.pid, "1:2", "b" * 64
                )
                permit = observer.authorize_macos_compiler_exec(start)
                capability_command._validate_macos_compiler_exec_permit(
                    permit, start, **identity
                )
                order.append("gate-permit")
                return start

            def close(self): order.append("gate-close")

        with mock.patch.object(
            capability_command, "_MacOSCompilerExecGate", Gate
        ), mock.patch.object(
            capability_command.subprocess, "Popen", return_value=Process()
        ) as popen, mock.patch.object(
            capability_command, "_open_macos_process_identity_handle"
        ):
            process, _carrier = capability_command.launch_compiler_process(
                ("/trusted/compiler",),
                cwd=Path.cwd(),
                environment={},
                containment=Containment(),
                platform_kind="macos",
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                launch_options={},
                purpose=capability_command.CompilerLaunchPurpose.AUDIT_DISCOVERY,
                launch_observer=Observer(),
                worker_index=3,
                task_id="audit-5-0123456789abcdef",
                generation=7,
            )
        self.assertEqual(process.pid, 71)
        self.assertEqual(popen.call_args.args[0], ("gate",))
        self.assertEqual(
            order,
            [
                "gate-create", "attach", "gate-report", ("authorize", 71),
                "gate-permit", "register", "release", "gate-close",
            ],
        )

    def test_wrong_generation_permit_is_rejected(self):
        start = capability_command.ProcessStartIdentity(
            "macos", 71, "1:2", "c" * 64
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "macOS compiler exec permit"
        ):
            capability_command._validate_macos_compiler_exec_permit(
                capability_command.CompilerExecPermit(3, 8, 5, 71),
                start,
                worker_index=3,
                task_id="audit-5-0123456789abcdef",
                generation=7,
            )

    def test_inspection_permit_is_bound_to_ungenerated_launch_identity(self):
        start = capability_command.ProcessStartIdentity(
            "macos", 71, "1:2", "d" * 64)
        permit = capability_command.MacOSInspectionExecPermit(
            "inspection-1", 71, 71, "e" * 64)
        capability_command._validate_macos_compiler_exec_permit(
            permit, start, worker_index=None, task_id=None, generation=None)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "inspection exec permit"
        ):
            capability_command._validate_macos_compiler_exec_permit(
                dataclasses.replace(permit, pgid=72), start,
                worker_index=None, task_id=None, generation=None)


class CommandRewriteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "file.cpp"
        self.source.write_text("int value;\n", encoding="utf-8")
        self.dependency_output = self.root / "private dependencies" / "deps.out"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def configuration(
        self,
        family: CompilerFamily,
        arguments: tuple[str, ...],
    ) -> PreprocessConfiguration:
        compiler_name = {
            CompilerFamily.GCC: "g++.exe",
            CompilerFamily.CLANG: "clang++.exe",
            CompilerFamily.MSVC: "cl.exe",
            CompilerFamily.CLANG_CL: "clang-cl.exe",
        }[family]
        identity = FileIdentity(
            canonical=self.source.resolve(),
            relative=PurePosixPath("playback/gpu/file.cpp"),
            device=None,
            inode=None,
            line_count=1,
            production=True,
        )
        compiler = (self.root / compiler_name).resolve()
        executable = FileIdentity(compiler, None, 3, 81, 0, False)
        binding = DependencyRootBinding(
            "toolchain", self.root.resolve(),
            FileIdentity(self.root.resolve(), None, 3, 82, 0, False),
        )
        capability = CompilerExecutableCapability(
            "windows", executable, "1" * 64, "2" * 64, object(), binding,
            (), (), "3" * 64, (),
        )
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=family,
            compiler=compiler,
            working_directory=self.root.resolve(),
            source=identity,
            arguments=arguments,
            environment_digest="environment",
            digest=f"cfg-{family.value}",
            dependency_root_authority_digest="a" * 64,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )

    def rewrite(
        self,
        family: CompilerFamily,
        arguments: tuple[str, ...],
    ) -> RewrittenCommand:
        return rewrite_preprocess_command(
            self.configuration(family, arguments), self.dependency_output
        )

    def test_rewritten_command_is_exact_and_immutable(self):
        rewritten = self.rewrite(CompilerFamily.GCC, ("file.cpp",))
        self.assertEqual(
        tuple(field.name for field in dataclasses.fields(RewrittenCommand)),
        ("arguments", "dependency_output", "dependency_format", "classification"),
        )
        self.assertIsInstance(rewritten.arguments, tuple)
        self.assertEqual(rewritten.dependency_output, self.dependency_output)
        self.assertEqual(rewritten.dependency_format, "gcc-depfile")
        with self.assertRaises(dataclasses.FrozenInstanceError):
            rewritten.dependency_format = "msvc-json"

    def test_gcc_rewrite_preserves_semantics_and_replaces_outputs(self):
        arguments = (
            "-std=gnu++17", "-DOLR_GPU=1", "-Iinc", "-include", "forced.h",
            "--sysroot=C:/sdk", "-target", "x86_64-w64-windows-gnu",
            "-arch", "x86_64", "-FFrameworks", "-include-pch", "prefix.pch",
            "-fmodule-file=Core=Core.pcm", "-fmodule-map-file=module.modulemap",
            "-c", "file.cpp", "-o", "file.obj", "-MMD", "-MFdep.d",
            "-MT", "old target", "-MQquoted target", "-MJ", "record.json", "-P",
        )
        rewritten = self.rewrite(CompilerFamily.GCC, arguments)
        self.assertEqual(
            rewritten.arguments,
            (
                str(self.configuration(CompilerFamily.GCC, arguments).compiler),
                "-std=gnu++17", "-DOLR_GPU=1", "-Iinc", "-include", "forced.h",
                "--sysroot=C:/sdk", "-target", "x86_64-w64-windows-gnu",
                "-arch", "x86_64", "-FFrameworks", "-include-pch", "prefix.pch",
                "-fmodule-file=Core=Core.pcm", "-fmodule-map-file=module.modulemap",
                "file.cpp", "-E", "-MD", "-MF", str(self.dependency_output),
            ),
        )
        self.assertEqual(rewritten.arguments.count("file.cpp"), 1)
        self.assertNotIn("-P", rewritten.arguments)
        self.assertNotIn("file.obj", rewritten.arguments)

    def test_gnu_rewrite_matrix_strips_attached_and_standalone_outputs(self):
        arguments = (
            "-DKEEP=two words", "-xc++", "-c", "-S", "-E", "-P",
            "-oattached.obj", "-MD", "-MMD", "-M", "-MM", "-MG", "-MP",
            "-MFattached.d", "-MTattached", "-MQattached", "-MJattached.json",
            "file.cpp",
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(
                    rewritten.arguments,
                    (
                        str(self.configuration(family, arguments).compiler),
                        "-DKEEP=two words", "-xc++", "file.cpp",
                        "-E", "-MD", "-MF", str(self.dependency_output),
                    ),
                )
                self.assertEqual(rewritten.dependency_format, "gcc-depfile")

    def test_gnu_strips_documented_long_output_dependency_and_dump_modes(self):
        arguments = (
            "--output", "first.i", "--output=second.i",
            "--dependencies", "--user-dependencies",
            "--write-dependencies", "--write-user-dependencies",
            "--print-missing-file-dependencies", "--no-line-commands",
            "-dM", "--dump=M", "--dump", "M", "file.cpp",
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(
                    rewritten.arguments,
                    (
                        str(self.configuration(family, arguments).compiler),
                        "file.cpp", "-E", "-MD", "-MF",
                        str(self.dependency_output),
                    ),
                )

    def test_gnu_long_output_and_dump_options_require_values(self):
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            for option in ("--output", "--dump", "--output=", "--dump="):
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "requires a value"
                ):
                    self.rewrite(family, ("file.cpp", option))

    def test_gnu_strips_paired_diagnostic_outputs_without_treating_values_as_sources(self):
        arguments = (
            "-serialize-diagnostics", "diagnostics.dia",
            "--dependency-file", "driver-deps.d",
            "-fdiagnostics-file=diagnostics.txt", "file.cpp",
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(
                    rewritten.arguments,
                    (
                        str(self.configuration(family, arguments).compiler),
                        "file.cpp", "-E", "-MD", "-MF",
                        str(self.dependency_output),
                    ),
                )

    def test_gnu_preserves_position_sensitive_preprocessing_arguments_byte_for_byte(self):
        arguments = (
            "-x", "c++", "-isystem", "SDK Path", "-iquotequoted path",
            "-iframework", "Framework Path", "-imacrosmacros.h",
            "-include-pth", "prefix.pth", "-Xclang", "-fmodules",
            "-Xpreprocessor", "-DTHROUGH_FORWARDER=1", "file.cpp",
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

    def test_gnu_preserves_option_looking_operands_of_paired_semantic_options(self):
        arguments = (
            "-include", "-P", "-imacros", "-M", "-I", "-o",
            "-D", "-dM", "-isystem", "--dependencies", "file.cpp",
            "--include", "--no-line-commands", "--imacros", "--dependencies",
            "--define-macro", "--output", "--undefine-macro", "--dump=M",
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)
                self.assertEqual(rewritten.arguments.count("file.cpp"), 1)

    def test_gnu_rejects_output_source_and_marker_traps(self):
        controls = (
            (("file.cpp", "other.cpp"), "multiple source"),
            (("other.cpp",), "does not match"),
            (("-save-temps=obj", "file.cpp"), "output"),
            (("-fpreprocessed", "file.cpp"), "source-selection"),
            (("-x", "c++-cpp-output", "file.cpp"), "source-selection"),
            (("-Wp,-P", "file.cpp"), "hidden.*marker"),
            (("-Xpreprocessor", "-P", "file.cpp"), "hidden.*marker"),
            (("-Xclang", "-o", "file.cpp"), "hidden.*output"),
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            for arguments, message in controls:
                with self.subTest(family=family, arguments=arguments), self.assertRaisesRegex(
                    AuditInfrastructureError, message
                ):
                    self.rewrite(family, arguments)

    def test_gnu_and_clang_cl_reject_joined_xclang_hidden_controls(self):
        controls = (
            ("-Xclang=-P", "marker"),
            ("-Xclang=-o", "output"),
            ("-Xclang=-MFhidden.d", "dependency"),
            ("-Xclang=-x", "source-selection"),
            ("-Xclang=@hidden.rsp", "response"),
        )
        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            for option, category in controls:
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, f"hidden.*{category}"
                ):
                    self.rewrite(family, (option, "file.cpp"))

    def test_joined_forwarders_require_nonempty_payloads(self):
        controls = (
            (CompilerFamily.CLANG, ("-Xclang=", "file.cpp")),
            (CompilerFamily.CLANG_CL, ("-Xclang=", "file.cpp")),
            (CompilerFamily.CLANG_CL, ("/clang:", "file.cpp")),
            (CompilerFamily.CLANG_CL,
             ("/clang:-include", "/clang:", "file.cpp")),
        )
        for family, arguments in controls:
            with self.subTest(family=family, arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, "requires a value"
            ):
                self.rewrite(family, arguments)

    def test_forwarded_long_and_dump_controls_fail_closed(self):
        controls = (
            ("--output=hidden.i", "output"),
            ("--dependencies", "dependency"),
            ("--user-dependencies", "dependency"),
            ("--write-dependencies", "dependency"),
            ("--write-user-dependencies", "dependency"),
            ("--print-missing-file-dependencies", "dependency"),
            ("--no-line-commands", "marker"),
            ("-dM", "output"),
            ("--dump=M", "output"),
        )
        for family in (CompilerFamily.GCC, CompilerFamily.CLANG):
            for payload, category in controls:
                with self.subTest(family=family, payload=payload), self.assertRaisesRegex(
                    AuditInfrastructureError, f"hidden.*{category}"
                ):
                    self.rewrite(family, ("-Xpreprocessor", payload, "file.cpp"))

    def test_clang_frontend_dependency_and_action_controls_fail_closed(self):
        value_controls = (
            ("-dependency-dot", "hidden.dot", "dependency"),
            ("-dependency-file", "hidden.d", "dependency"),
            ("-serialize-diagnostic-file", "hidden.dia", "output"),
            ("-diagnostic-log-file", "hidden.log", "output"),
            ("-stats-file", "hidden.stats", "output"),
            ("-main-file-name", "other.cpp", "source-selection"),
            ("-plugin", "hidden-plugin", "action"),
            ("-add-plugin", "hidden-plugin", "action"),
            ("-load", "hidden-plugin.dll", "action"),
            ("-code-completion-at", "file.cpp:1:1", "action"),
            ("-module-file-info", "hidden.pcm", "action"),
        )
        flag_controls = (
            ("-ObjC", "source-selection"),
            ("-ObjC++", "source-selection"),
            ("-dump-tokens", "action"),
            ("-dump-raw-tokens", "action"),
            ("-Eonly", "action"),
            ("-emit-ast", "action"),
            ("-emit-codegen-only", "action"),
            ("-emit-llvm-only", "action"),
            ("-emit-obj", "action"),
            ("-emit-pch", "action"),
            ("-emit-module", "action"),
            ("-rewrite-objc", "action"),
            ("-rewrite-macros", "action"),
            ("-ast-dump", "action"),
            ("-ast-print", "action"),
            ("-analyze", "action"),
            ("-fixit", "action"),
            ("-fixit-recompile", "action"),
            ("-verify", "action"),
            ("-syntax-only", "action"),
        )
        attached_controls = (
            ("-dependency-dot=hidden.dot", "dependency"),
            ("-code-completion-at=file.cpp:1:1", "action"),
            ("-verify=expected", "action"),
            ("-ast-dump-filter=GpuSurface", "action"),
            ("-module-file-info=hidden.pcm", "action"),
            ("-fmodule-output=hidden.pcm", "output"),
            ("-gen-reproducer=always", "output"),
            ("-unknown-future-action=hidden", "action"),
            ("-unknown-future-cc1-control", "ambiguous"),
            ("-target-future-control=hidden", "ambiguous"),
            ("-fmodule-future-control=hidden", "ambiguous"),
            ("-fmodules-future-control=hidden", "ambiguous"),
            ("-fms-future-control", "ambiguous"),
            ("-fobjc-future-control", "ambiguous"),
        )

        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            for option, operand, category in value_controls:
                arguments = (
                    ("-Xclang", option, "-Xclang", operand, "file.cpp")
                    if family is CompilerFamily.CLANG
                    else (f"/clang:{option}", f"/clang:{operand}", "file.cpp")
                )
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, f"hidden.*{category}"
                ):
                    self.rewrite(family, arguments)
            for option, category in (*flag_controls, *attached_controls):
                arguments = (
                    ("-Xclang", option, "file.cpp")
                    if family is CompilerFamily.CLANG
                    else (f"/clang:{option}", "file.cpp")
                )
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, f"hidden.*{category}"
                ):
                    self.rewrite(family, arguments)

    def test_clang_frontend_semantic_preprocessor_target_and_module_controls_survive(self):
        safe_pairs = (
            ("-include", "forced.h"),
            ("-include-pch", "prefix.pch"),
            ("-isystem", "SDK Path"),
            ("-triple", "x86_64-pc-windows-msvc"),
            ("-aux-triple", "x86_64-pc-windows-msvc"),
            ("-target-cpu", "x86-64"),
            ("-target-feature", "+sse2"),
            ("-fmodules-user-build-path", "module-user-build"),
            ("-fmodule-implementation-of", "Core"),
            ("-fmodule-feature", "cplusplus"),
            ("-mrelocation-model", "pic"),
            ("-mthread-model", "posix"),
            ("-target-linker-version", "14.0"),
        )
        safe_flags = (
            "-DKEEP=1", "-UOLD", "-Iinclude", "-std=c++20",
            "-stdlib=libc++",
            "-fmodules", "-fimplicit-module-maps", "-fcxx-exceptions",
            "-Wno-unknown-warning-option", "-O2", "-gline-tables-only",
            "-mrelax-all", "-mnoexecstack", "-masm-verbose",
            "-mconstructor-aliases", "-mframe-pointer=all",
            "-target-sdk-version=15.0",
            "-fmodule-map-file=module.modulemap",
            "-fmodule-file=Core=Core.pcm",
            "-fmodule-name=Core", "-fmodule-format=raw",
            "-fmodules-cache-path=module-cache",
            "-fprebuilt-module-path=prebuilt-modules",
            "-fmodules-prune-interval=604800",
            "-fmodules-ignore-macro=IGNORED",
            "-fmodules-prune-after=2678400",
            "-fmodules-validate-once-per-build-session",
            "-fmodule-map-file-home-is-cwd",
            "-fms-extensions", "-fms-compatibility-version=19.0",
            "-fms-compatibility",
            "-fobjc-arc", "-fobjc-runtime=macosx-10.12",
            "-fobjc-arc-exceptions", "-fobjc-weak",
            "-disable-llvm-passes",
            "-debug-info-kind=constructor", "-dwarf-version=5",
            "-debugger-tuning=lldb", "-msoft-float", "-mstackrealign",
        )
        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            arguments: list[str] = []
            for option, operand in safe_pairs:
                if family is CompilerFamily.CLANG:
                    arguments.extend(("-Xclang", option, "-Xclang", operand))
                else:
                    arguments.extend((f"/clang:{option}", f"/clang:{operand}"))
            for option in safe_flags:
                if family is CompilerFamily.CLANG:
                    arguments.extend(("-Xclang", option))
                else:
                    arguments.append(f"/clang:{option}")
            arguments.append("file.cpp")
            with self.subTest(family=family):
                rewritten = self.rewrite(family, tuple(arguments))
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], tuple(arguments))

    def test_clang_frontend_semantic_value_controls_require_exact_forwarded_arity(self):
        safe_pairs = (
            "-include", "-include-pch", "-isystem",
            "-triple", "-aux-triple", "-target-cpu", "-target-feature",
            "-fmodules-user-build-path",
            "-fmodule-implementation-of", "-fmodule-feature",
            "-mrelocation-model", "-mthread-model", "-target-linker-version",
        )
        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            for option in safe_pairs:
                arguments = (
                    ("-Xclang", option, "file.cpp")
                    if family is CompilerFamily.CLANG
                    else (f"/clang:{option}", "file.cpp")
                )
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "requires a forwarded value|requires a value"
                ):
                    self.rewrite(family, arguments)

    def test_clang_frontend_joined_only_controls_cannot_hide_actions_as_operands(self):
        joined_only = (
            "-std", "-stdlib", "-target-sdk-version",
            "-fmodule-map-file", "-fmodule-file",
            "-fmodule-name", "-fmodule-format", "-fmodules-cache-path",
            "-fprebuilt-module-path", "-fmodules-prune-interval",
        )
        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            for option in joined_only:
                arguments = (
                    ("-Xclang", option, "-Xclang", "-emit-obj", "file.cpp")
                    if family is CompilerFamily.CLANG
                    else (f"/clang:{option}", "/clang:-emit-obj", "file.cpp")
                )
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "hidden ambiguous option"
                ):
                    self.rewrite(family, arguments)

    def test_clang_frontend_joined_only_controls_reject_malformed_spellings(self):
        malformed = ("-std", "-std=", "-stdlib", "-stdlib=")
        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            for option in malformed:
                arguments = (
                    ("-Xclang", option, "file.cpp")
                    if family is CompilerFamily.CLANG
                    else (f"/clang:{option}", "file.cpp")
                )
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "hidden ambiguous option"
                ):
                    self.rewrite(family, arguments)

    def test_clang_frontend_controls_absent_from_llvm_options_fail_closed(self):
        for family in (CompilerFamily.CLANG, CompilerFamily.CLANG_CL):
            controls = (
                (
                    ("-Xclang", "-include-pth", "-Xclang", "prefix.pth", "file.cpp")
                    if family is CompilerFamily.CLANG
                    else ("/clang:-include-pth", "/clang:prefix.pth", "file.cpp")
                ),
                (
                    ("-Xclang", "-mcode-model=small", "file.cpp")
                    if family is CompilerFamily.CLANG
                    else ("/clang:-mcode-model=small", "file.cpp")
                ),
            )
            for arguments in controls:
                with self.subTest(
                    family=family, arguments=arguments
                ), self.assertRaisesRegex(
                    AuditInfrastructureError, "hidden ambiguous option"
                ):
                    self.rewrite(family, arguments)

    def test_forwarded_semantic_options_preserve_option_looking_operands(self):
        controls = (
            (CompilerFamily.GCC,
             ("-Xpreprocessor", "-include", "-Xpreprocessor", "-P", "file.cpp")),
            (CompilerFamily.CLANG,
             ("-Xclang", "-include", "-Xclang", "-P", "file.cpp")),
            (CompilerFamily.CLANG,
             ("-Xclang=-include", "-Xclang=-P", "file.cpp")),
            (CompilerFamily.CLANG,
             ("-Xclang=-include", "-Xclang=-include", "file.cpp")),
            (CompilerFamily.CLANG_CL,
             ("-Xclang", "-include", "-Xclang", "-P", "file.cpp")),
            (CompilerFamily.CLANG_CL,
             ("/clang:-include", "/clang:-P", "file.cpp")),
            (CompilerFamily.GCC, ("-Wp,-include,-P", "file.cpp")),
            (CompilerFamily.CLANG, ("-Wp,-include,-P", "file.cpp")),
        )
        for family, arguments in controls:
            with self.subTest(family=family, arguments=arguments):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

    def test_msvc_rewrite_preserves_semantics_and_replaces_outputs(self):
        arguments = (
            "/std:c++17", "/DOLR_GPU=1", "/I", "SDK Path", "/external:IExternal",
            "/FIforced.h", "/Yuprefix.h", "/Fpprefix.pch", "/reference", "Core=Core.ifc",
            "/c", "file.cpp", "/Fo", "out.obj", "/Feprogram.exe", "/Fdstate.pdb",
            "/showIncludes", "/sourceDependencies", "old.json", "/P", "/EP",
            "/Fiold.i", "/ifcOutput", "old.ifc",
        )
        rewritten = self.rewrite(CompilerFamily.MSVC, arguments)
        self.assertEqual(
            rewritten.arguments,
            (
                str(self.configuration(CompilerFamily.MSVC, arguments).compiler),
                "/std:c++17", "/DOLR_GPU=1", "/I", "SDK Path", "/external:IExternal",
                "/FIforced.h", "/Yuprefix.h", "/Fpprefix.pch", "/reference", "Core=Core.ifc",
                "file.cpp", "/nologo", "/E", "/sourceDependencies",
                str(self.dependency_output),
            ),
        )
        self.assertEqual(rewritten.dependency_format, "msvc-json")

    def test_msvc_and_clang_cl_strip_paired_attached_and_dashed_output_forms(self):
        arguments = (
            "/DKEEP=1", "/c", "file.cpp", "/Foone.obj", "/Fe", "two.exe",
            "-Fdthree.pdb", "/Faassembly.asm", "/Fmmap.txt", "/FRbrowse.sbr",
            "/sourceDependencies:old.json", "/showIncludes:user", "/nologo", "/E",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(
                    rewritten.arguments,
                    (
                        str(self.configuration(family, arguments).compiler),
                        "/DKEEP=1", "file.cpp", "/nologo", "/E",
                        "/sourceDependencies", str(self.dependency_output),
                    ),
                )
                self.assertEqual(rewritten.dependency_format, "msvc-json")

    def test_msvc_strips_required_paired_and_optional_attached_outputs(self):
        arguments = (
            "/DKEEP=1", "/Fo", "one.obj", "/Fe", "two.exe",
            "/Fd", "three.pdb", "/Faassembly.asm", "/Fmmap.txt",
            "/FRbrowse.sbr", "/Fi", "preprocessed.i", "file.cpp",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(
                    rewritten.arguments,
                    (
                        str(self.configuration(family, arguments).compiler),
                        "/DKEEP=1", "file.cpp", "/nologo", "/E",
                        "/sourceDependencies", str(self.dependency_output),
                    ),
                )

    def test_msvc_default_path_outputs_do_not_consume_the_source(self):
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/FA", "/Fa", "/Fm", "/FR", "/Fr",
                           "-FA", "-Fa", "-Fm", "-FR", "-Fr"):
                arguments = ("/DKEEP=1", option, "file.cpp")
                with self.subTest(family=family, option=option):
                    rewritten = self.rewrite(family, arguments)
                    self.assertEqual(
                        rewritten.arguments,
                        (
                            str(self.configuration(family, arguments).compiler),
                            "/DKEEP=1", "file.cpp", "/nologo", "/E",
                            "/sourceDependencies", str(self.dependency_output),
                        ),
                    )

    def test_msvc_optional_output_paths_must_be_attached(self):
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/Fa", "/Fm", "/FR", "/Fr"):
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "multiple source"
                ):
                    self.rewrite(family, (option, "output.bin", "file.cpp"))

    def test_msvc_preserves_pch_module_external_include_and_forced_source_forms(self):
        arguments = (
            "/std:c++20", "/external:I", "External SDK", "/FI", "forced.h",
            "/Yu", "prefix.h", "/Fp", "prefix.pch", "/reference", "Core=Core.ifc",
            "/Tpfile.cpp",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

    def test_msvc_preserves_global_language_flags_without_consuming_semantics(self):
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/TP", "/TC", "-TP", "-TC"):
                arguments = (option, "/D", "KEEP=1", "file.cpp")
                with self.subTest(family=family, option=option):
                    rewritten = self.rewrite(family, arguments)
                    self.assertEqual(rewritten.arguments[1:5], arguments)

    def test_msvc_preserves_option_looking_operands_of_paired_semantic_options(self):
        arguments = (
            "/FI", "/P", "/I", "/Fo", "/D", "/showIncludes",
            "/Fp", "/sourceDependencies", "/reference", "/OUT:library.ifc",
            "file.cpp",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)
                self.assertEqual(rewritten.arguments.count("file.cpp"), 1)

    def test_clang_cl_rejects_hidden_gnu_output_marker_and_source_options(self):
        controls = (
            (("/clang:-o", "/clang:hidden.obj", "file.cpp"), "hidden.*output"),
            (("/clang:-MF", "/clang:hidden.d", "file.cpp"), "hidden.*dependency"),
            (("/clang:-dependency-file", "/clang:hidden.d", "file.cpp"),
             "hidden.*dependency"),
            (("/clang:-Wp,-MD,hidden.d", "file.cpp"), "hidden.*dependency"),
            (("/clang:-P", "file.cpp"), "hidden.*marker"),
            (("/clang:-c", "file.cpp"), "hidden.*source-selection"),
            (("/clang:-x", "/clang:c++-cpp-output", "file.cpp"), "hidden.*source-selection"),
            (("/clang:@hidden.rsp", "file.cpp"), "hidden.*response"),
        )
        for arguments, message in controls:
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                self.rewrite(CompilerFamily.CLANG_CL, arguments)

    def test_clang_cl_nested_forwarding_is_classified_as_one_stateful_stream(self):
        controls = (
            ("/clang:-Wp,-include,-P",),
            ("/clang:-Xpreprocessor=-include", "/clang:-Xpreprocessor=-P"),
            ("/clang:-Xclang=-include", "/clang:-Xclang=-P"),
            ("/clang:-Xpreprocessor", "/clang:-include",
             "/clang:-Xpreprocessor", "/clang:-P"),
        )
        for forwarded in controls:
            arguments = (*forwarded, "file.cpp")
            with self.subTest(arguments=arguments):
                rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

    def test_clang_cl_nested_forwarding_still_rejects_unconsumed_controls(self):
        controls = (
            (("/clang:-Xpreprocessor=-P", "file.cpp"), "marker"),
            (("/clang:-Xclang=-MFhidden.d", "file.cpp"), "dependency"),
            (("/clang:-Wp,-include,forced.h,-P", "file.cpp"), "marker"),
        )
        for arguments, category in controls:
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, f"hidden.*{category}"
            ):
                self.rewrite(CompilerFamily.CLANG_CL, arguments)

    def test_clang_cl_mixed_forwarding_channels_share_ordered_operand_state(self):
        controls = (
            ("-Xpreprocessor", "-include", "/clang:-P", "file.cpp"),
            ("-Xpreprocessor=-include", "/clang:-P", "file.cpp"),
            ("/clang:-Wp,-include", "/clang:-P", "file.cpp"),
            ("-Wp,-include", "/clang:-P", "file.cpp"),
        )
        for arguments in controls:
            with self.subTest(arguments=arguments):
                rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

    def test_clang_cl_forwarding_uses_effective_cc1_bucket_order(self):
        safe_controls = ("-P", "-o", "-MF")
        for control in safe_controls:
            arguments = (
                f"/clang:{control}",
                "-Xpreprocessor", "-include",
                "file.cpp",
            )
            with self.subTest(kind="safe", control=control):
                rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

        for control, category in (("-P", "marker"), ("-o", "output"), ("-MF", "dependency")):
            arguments = (
                "/clang:-include",
                "-Xpreprocessor", control,
                "file.cpp",
            )
            with self.subTest(kind="hidden", control=control), self.assertRaisesRegex(
                AuditInfrastructureError, f"hidden.*{category}"
            ):
                self.rewrite(CompilerFamily.CLANG_CL, arguments)

    def test_clang_cl_mixed_forwarding_rejects_controls_left_after_operand(self):
        controls = (
            (("/clang:-Xpreprocessor", "/clang:-include",
              "-Xpreprocessor", "-P", "/clang:-P", "file.cpp"), "marker"),
            (("/clang:-Wp,-include", "/clang:-P",
              "-Xpreprocessor", "-P", "file.cpp"), "marker"),
            (("/clang:-include", "-Wp,-P",
              "/clang:-o", "/clang:hidden.i", "file.cpp"), "marker"),
            (("/clang:-P", "/clang:-Wp,-include", "file.cpp"), "marker"),
        )
        for arguments, category in controls:
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                AuditInfrastructureError, f"hidden.*{category}"
            ):
                self.rewrite(CompilerFamily.CLANG_CL, arguments)

    def test_clang_cl_deep_joined_forwarding_is_iterative(self):
        payload = "-Xpreprocessor=" * 1_100 + "-DKEEP=1"
        arguments = (f"/clang:{payload}", "file.cpp")
        rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
        self.assertEqual(rewritten.arguments[1:3], arguments)

    def test_deep_joined_forwarding_scales_linearly(self):
        def elapsed(depth: int) -> float:
            payload = "-Xpreprocessor=" * depth + "-DKEEP=1"
            arguments = (f"/clang:{payload}", "file.cpp")
            started = time.perf_counter()
            rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
            self.assertEqual(rewritten.arguments[1:3], arguments)
            return time.perf_counter() - started

        elapsed(100)
        small = min(elapsed(1_000) for _ in range(3))
        large = min(elapsed(16_000) for _ in range(2))
        self.assertLess(
            large,
            small * 40 + 0.02,
            f"joined forwarding scaled superlinearly: 1k={small:.4f}s 16k={large:.4f}s",
        )

    def test_four_mib_joined_forwarding_has_bounded_time_and_memory(self):
        prefix = "-Xpreprocessor="
        depth = (4 * 1024 * 1024 - len("-P")) // len(prefix)
        payload = prefix * depth + "-P"
        arguments = (payload, "file.cpp")

        tracemalloc.start()
        started = time.perf_counter()
        try:
            with self.assertRaisesRegex(AuditInfrastructureError, "hidden marker"):
                self.rewrite(CompilerFamily.CLANG, arguments)
            elapsed = time.perf_counter() - started
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()

        self.assertLess(elapsed, 8.0, f"4 MiB forwarding took {elapsed:.3f}s")
        self.assertLess(peak, 16 * 1024 * 1024, f"4 MiB forwarding peaked at {peak} bytes")

    def test_four_mib_top_level_wp_fields_have_bounded_time_and_memory(self):
        field = "safe-forwarded-value-" + "x" * 44
        count = (4 * 1024 * 1024 - 4) // (len(field) + 1)
        payload = "-Wp," + f"{field}," * count
        arguments = (payload, "file.cpp")
        self.assertGreaterEqual(len(payload), 3 * 1024 * 1024)
        self.assertLessEqual(len(payload), 4 * 1024 * 1024)

        tracemalloc.start()
        started = time.perf_counter()
        try:
            rewritten = self.rewrite(CompilerFamily.CLANG, arguments)
            elapsed = time.perf_counter() - started
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()

        self.assertEqual(rewritten.arguments[1:3], arguments)
        self.assertLess(elapsed, 6.0, f"4 MiB -Wp fields took {elapsed:.3f}s")
        self.assertLess(peak, 10 * 1024 * 1024, f"4 MiB -Wp fields peaked at {peak} bytes")

    def test_msvc_strips_preprocessor_and_auxiliary_output_controls(self):
        shared = (
            "/PD", "/PH", "/Fx", "/doc", "/docattached.xdc",
            "/Ft", "import-headers",
            "/Ftattached-headers", "/experimental:log", "audit.sarif",
            "/experimental:logattached.sarif", "file.cpp",
        )
        expected = (
            str(self.configuration(CompilerFamily.MSVC, shared).compiler),
            "file.cpp", "/nologo", "/E", "/sourceDependencies",
            str(self.dependency_output),
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, shared)
                self.assertEqual(
                    rewritten.arguments,
                    (str(self.configuration(family, shared).compiler), *expected[1:]),
                )

    def test_clang_cl_strips_d1pp_without_weakening_fp_or_forced_include(self):
        arguments = (
            "/d1PP", "/Fp", "prefix.pch", "/Fpprefix-2.pch",
            "/FI", "forced.h", "/FIforced-2.h", "file.cpp",
        )
        rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
        self.assertEqual(
            rewritten.arguments[1:8],
            arguments[1:],
        )

    def test_msvc_output_controls_with_missing_values_fail_closed(self):
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in ("/Ft", "/experimental:log"):
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "requires a value"
                ):
                    self.rewrite(family, ("file.cpp", option))

    def test_msvc_output_controls_with_empty_attached_paths_fail_closed(self):
        options = (
            "/Fo:", "/Fe:", "/Fd:", "/Fi:", "/Ft:",
            "/sourceDependencies:", "/scanDependencies:", "/ifcOutput:",
            "/experimental:log:",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in options:
                for spelling in (option, f"-{option[1:]}"):
                    with self.subTest(family=family, option=spelling), self.assertRaisesRegex(
                        AuditInfrastructureError, "requires a value"
                    ):
                        self.rewrite(family, ("file.cpp", spelling))

    def test_msvc_output_controls_accept_nonempty_colon_attached_paths(self):
        options = (
            "/Fo:object.obj", "/Fe:program.exe", "/Fd:state.pdb",
            "/Fi:preprocessed.i", "/Ft:import-headers",
            "/sourceDependencies:old.json", "/scanDependencies:scan.json",
            "/ifcOutput:module.ifc", "/experimental:log:audit.sarif",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in options:
                for spelling in (option, f"-{option[1:]}"):
                    with self.subTest(family=family, option=spelling):
                        rewritten = self.rewrite(family, (spelling, "file.cpp"))
                        self.assertNotIn(spelling, rewritten.arguments)
                        self.assertEqual(rewritten.arguments.count("file.cpp"), 1)

    def test_msvc_ambiguous_output_misspellings_fail_closed_during_rewrite(self):
        wrong_case = (
            "/fOhidden.obj", "/fE:hidden.exe", "/fDstate.pdb", "/fIhidden.i",
            "/sourcedependencies:old.json", "/scandependencies:scan.json",
            "/ifcoutput:module.ifc", "/showincludes",
            "/e", "/p", "/ep", "/pd", "/ph", "/fX", "/fMmap.txt",
            "/frbrowse.sbr", "/out:hidden.exe", "/Link", "/yCprefix.h",
            "/ld", "/CLR:netcore",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in wrong_case:
                with self.subTest(family=family, option=option), self.assertRaisesRegex(
                    AuditInfrastructureError, "case-sensitive.*option"
                ):
                    self.rewrite(family, (option, "file.cpp"))

    def test_msvc_documented_case_distinct_semantics_are_preserved(self):
        options = (
            "/u", "/C", "/utf-8", "/fp:fast", "/favor:AMD64", "/interface",
            "/internalPartition", "/exportHeader", "/translateInclude",
            "/source-charset:utf-8", "/execution-charset:utf-8",
            "/validate-charset", "/Zc:preprocessor", "/arch:AVX2",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            arguments = (*options, "file.cpp")
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(rewritten.arguments[1:1 + len(arguments)], arguments)

    def test_clang_cl_gnu_dash_options_are_not_case_variant_false_positives(self):
        arguments = ("-fmodules", "file.cpp")
        rewritten = self.rewrite(CompilerFamily.CLANG_CL, arguments)
        self.assertEqual(rewritten.arguments[1:3], arguments)

    def test_msvc_documented_help_case_exception_is_not_misclassified(self):
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for option in (
                "/HELP", "/help", "-HeLp", "/Nologo", "/Reference",
                "/headerunit", "/fu",
            ):
                with self.subTest(family=family, option=option):
                    rewritten = self.rewrite(family, (option, "file.cpp"))
                    self.assertIn(option, rewritten.arguments)

    def test_msvc_and_clang_cl_strip_directives_mode_dependency_output(self):
        arguments = (
            "/DKEEP=1", "/sourceDependencies:directives", "old.json", "file.cpp",
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            with self.subTest(family=family):
                rewritten = self.rewrite(family, arguments)
                self.assertEqual(
                    rewritten.arguments,
                    (
                        str(self.configuration(family, arguments).compiler),
                        "/DKEEP=1", "file.cpp", "/nologo", "/E",
                        "/sourceDependencies", str(self.dependency_output),
                    ),
                )
                self.assertEqual(rewritten.arguments.count("file.cpp"), 1)
                self.assertNotIn("old.json", rewritten.arguments)

    def test_msvc_and_clang_cl_reject_multiple_sources_and_unknown_outputs(self):
        controls = (
            (("file.cpp", "other.cpp"), "multiple source"),
            (("other.cpp",), "does not match"),
            (("/OUT:hidden.exe", "file.cpp"), "unsupported output"),
            (("/link", "/OUT:hidden.exe", "file.cpp"), "source-selection"),
        )
        for family in (CompilerFamily.MSVC, CompilerFamily.CLANG_CL):
            for arguments, message in controls:
                with self.subTest(family=family, arguments=arguments), self.assertRaisesRegex(
                    AuditInfrastructureError, message
                ):
                    self.rewrite(family, arguments)


class DecisionProjectionContractTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.toolchain = self.root / "toolchain"
        self.cwd = self.build / "obj"
        for path in (self.source / "include", self.build, self.toolchain / "sysroot",
                     self.cwd):
            path.mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        self.temporary.cleanup()

    def test_typed_spans_remove_only_proven_outputs_and_source(self):
        arguments = (
            "-DOLR_GPU=1", "-I", str(self.source / "include"),
            f"--sysroot={self.toolchain / 'sysroot'}", "-std=c++20",
            "-o", str(self.build / "out.o"), "-MF", str(self.build / "out.d"),
            str(self.source / "unit.cpp"),
        )
        classification = classify_command_rewrite(arguments, self.cwd,
                                                   CompilerFamily.CLANG)
        rewritten = RewrittenCommand(
            (str(self.toolchain / "clang++"), *arguments, "-E", "-MD"),
            self.build / "generated.d", "gcc-depfile", classification)
        normalized = normalize_decision_arguments(
            rewritten, self.cwd, "build-object", self.source, self.build,
            self.toolchain)
        self.assertEqual(normalized, (
            "-DOLR_GPU=1", "-I", "<SOURCE_ROOT>/include",
            "--sysroot=<TOOLCHAIN_ROOT>/sysroot", "-std=c++20"))
        self.assertEqual(tuple(span.role for span in classification.semantic_paths),
                         ("include-path", "sysroot"))
        self.assertEqual(tuple(span.role for span in classification.nonsemantic_outputs),
                         ("output",))
        self.assertEqual(tuple(span.role for span in classification.nonsemantic_dependencies),
                         ("dependency-file",))

    def test_unknown_path_bearing_option_fails_closed(self):
        with self.assertRaisesRegex(AuditInfrastructureError,
                                    "unclassified path-bearing option"):
            classify_command_rewrite(
                (f"--mystery-path={self.source / 'include'}",), self.cwd,
                CompilerFamily.CLANG)
        with self.assertRaisesRegex(AuditInfrastructureError,
                                    "unclassified path-bearing option"):
            classify_command_rewrite(
                (f"--unknown-module-dir={self.source / 'modules'}",), self.cwd,
                CompilerFamily.CLANG)

    def test_forwarded_separate_path_and_absolute_posix_source_are_typed(self):
        source = self.source / "unit.cpp"
        arguments = ("-Xclang", "-I", "-Xclang",
                     str(self.source / "include"), str(source))
        classification = classify_command_rewrite(
            arguments, self.cwd, CompilerFamily.CLANG)
        self.assertEqual(tuple(span.operand_index for span in classification.sources),
                         (4,))
        self.assertEqual(tuple(span.operand_index
                               for span in classification.semantic_paths), (3,))
        rewritten = RewrittenCommand(("clang++", *arguments), self.build / "out.d",
                                     "gcc-depfile", classification)
        self.assertEqual(normalize_decision_arguments(
            rewritten, self.cwd, "object", self.source, self.build,
            self.toolchain),
            ("-Xclang", "-I", "-Xclang", "<SOURCE_ROOT>/include"))

    def test_iprefix_and_wp_semantic_paths_cannot_collide(self):
        source = str(self.source / "unit.cpp")

        def project(prefix: Path, wp: bool = False):
            option = (f"-Wp,-I,{prefix}" if wp
                      else ("-iprefix", str(prefix)))
            arguments = ((option, source) if isinstance(option, str)
                         else (*option, source))
            classification = classify_command_rewrite(
                arguments, self.cwd, CompilerFamily.CLANG)
            rewritten = RewrittenCommand(("clang++", *arguments),
                                         self.build / "out.d", "gcc-depfile",
                                         classification)
            return normalize_decision_arguments(
                rewritten, self.cwd, "object", self.source, self.build,
                self.toolchain)

        self.assertNotEqual(project(self.source / "include"),
                            project(self.source / "other"))
        self.assertEqual(project(self.source / "include", wp=True),
                         ("-Wp,-I,<SOURCE_ROOT>/include",))

    def test_normalization_is_relocatable_but_semantic(self):
        def project(root: Path, define: str = "OLR_GPU=1"):
            source = root / "source"
            build = root / "build"
            toolchain = root / "toolchain"
            cwd = build / "obj"
            arguments = (f"-D{define}", "-I../source/include", "-o", "../build/out.o",
                         "../source/unit.cpp")
            classification = classify_command_rewrite(arguments, cwd,
                                                       CompilerFamily.GCC)
            rewritten = RewrittenCommand(("g++", *arguments), build / "out.d",
                                         "gcc-depfile", classification)
            return normalize_decision_arguments(
                rewritten, cwd, "object", source, build, toolchain)
        relocated = self.root / "relocated"
        self.assertEqual(project(self.root), project(relocated))
        self.assertNotEqual(project(self.root), project(relocated, "OLR_GPU=0"))

    def test_decision_environment_allowlist_excludes_ambient_and_secrets(self):
        base = {
            "CPATH": str(self.source / "include"),
            "SDKROOT": str(self.toolchain / "sysroot"),
            "MACOSX_DEPLOYMENT_TARGET": "14.0",
            "GITHUB_RUN_ID": "1", "RUNNER_TEMP": str(self.build / "tmp"),
            "ACTIONS_RUNTIME_TOKEN": "secret",
        }
        first = decision_environment_digest(
            CompilerFamily.CLANG, base, {"CCACHE_NAMESPACE": "gpu"}, self.cwd,
            "object", self.source, self.build, self.toolchain)
        noisy = dict(base, GITHUB_RUN_ID="2", RUNNER_TEMP="elsewhere",
                     ACTIONS_RUNTIME_TOKEN="different")
        self.assertEqual(first, decision_environment_digest(
            CompilerFamily.CLANG, noisy, {"CCACHE_NAMESPACE": "gpu"}, self.cwd,
            "object", self.source, self.build, self.toolchain))
        changed = dict(base, MACOSX_DEPLOYMENT_TARGET="15.0")
        self.assertNotEqual(first, decision_environment_digest(
            CompilerFamily.CLANG, changed, {"CCACHE_NAMESPACE": "gpu"}, self.cwd,
            "object", self.source, self.build, self.toolchain))

    def test_explicit_launcher_path_assignments_are_relocation_stable(self):
        def project(root: Path) -> str:
            source = root / "source"
            build = root / "build"
            toolchain = root / "toolchain"
            return decision_environment_digest(
                CompilerFamily.CLANG, {},
                {"SDKROOT": str(toolchain / "sdk"),
                 "cache_dir": str(build / "ccache")}, build / "obj",
                "object", source, build, toolchain)

        self.assertEqual(project(self.root), project(self.root / "relocated"))

    def test_msvc_showincludes_flag_does_not_consume_semantic_macro(self):
        source = str(self.source / "unit.cpp")

        def project(arguments: tuple[str, ...]) -> tuple[str, ...]:
            classification = classify_command_rewrite(
                arguments, self.cwd, CompilerFamily.MSVC)
            rewritten = RewrittenCommand(
                ("cl.exe", *arguments), self.build / "out.json",
                "msvc-source-dependencies", classification)
            return normalize_decision_arguments(
                rewritten, self.cwd, "object", self.source, self.build,
                self.toolchain)

        with_macro = project(("/showIncludes", "/DFOO=1", source))
        without_macro = project(("/showIncludes", source))
        self.assertEqual(with_macro, ("/DFOO=1",))
        self.assertNotEqual(with_macro, without_macro)

    def test_compiler_digest_ignores_path_but_binds_content_and_version(self):
        first = self.toolchain / "clang-a"
        relocated = self.root / "other" / "clang-b"
        relocated.parent.mkdir()
        first.write_bytes(b"compiler")
        relocated.write_bytes(b"compiler")
        self.assertEqual(compiler_digest(CompilerFamily.CLANG, first, "clang 18.1.8"),
                         compiler_digest(CompilerFamily.CLANG, relocated, "clang 18.1.8"))
        relocated.write_bytes(b"changed")
        self.assertNotEqual(compiler_digest(CompilerFamily.CLANG, first, "clang 18.1.8"),
                            compiler_digest(CompilerFamily.CLANG, relocated,
                                            "clang 18.1.8"))


if __name__ == "__main__":
    unittest.main()
