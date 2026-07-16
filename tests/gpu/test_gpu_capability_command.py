from __future__ import annotations

import dataclasses
import locale
import os
import subprocess
import sys
import tempfile
import time
import tracemalloc
import unittest
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_command import (  # noqa: E402
    RewrittenCommand,
    decode_compile_entry,
    expand_response_files,
    identify_compiler,
    make_configuration,
    rewrite_preprocess_command,
    strip_launchers,
)
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompilerFamily,
    FileIdentity,
    PreprocessConfiguration,
)


class CompileEntryDecodeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.database = self.root / "build" / "compile_commands.json"
        self.database.parent.mkdir()

    def tearDown(self) -> None:
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

    def tearDown(self) -> None:
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
            )

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

    def test_compiler_content_metadata_and_version_all_affect_digest(self):
        baseline = self.make()
        self.compiler.write_bytes(b"compiler-content-b")
        content_changed = self.make()
        self.assertNotEqual(baseline.digest, content_changed.digest)

        before = self.compiler.stat().st_mtime_ns
        os.utime(self.compiler, ns=(before + 10_000_000, before + 10_000_000))
        metadata_changed = self.make()
        self.assertNotEqual(content_changed.digest, metadata_changed.digest)

        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 14.0.0\n",
        ):
            version_changed = make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits()
            )
        self.assertNotEqual(metadata_changed.digest, version_changed.digest)

    def test_compiler_cannot_change_between_version_probe_and_fingerprint(self):
        def replace_during_probe(*_args, **_kwargs):
            self.compiler.write_bytes(b"replacement-compiler-content")
            return b"g++.exe (GCC) 13.1.0\n"

        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            side_effect=replace_during_probe,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "changed during compiler version probe"
        ):
            make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits()
            )

    def test_normalized_version_output_is_stable(self):
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0   \nTarget: mingw\r\n\r\n",
        ):
            first = make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits()
            )
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0\r\nTarget: mingw\n",
        ):
            second = make_configuration(
                self.entry(), self.database, 3, self.source_root, self.production,
                self.environment, AuditLimits()
            )
        self.assertEqual(first.digest, second.digest)

    def test_entry_id_changes_but_semantic_digest_does_not(self):
        first = self.make(index=1)
        second = self.make(index=9)
        self.assertNotEqual(first.entry_id, second.entry_id)
        self.assertEqual(first.digest, second.digest)

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
                    dict(self.environment, **{name: value}), AuditLimits()
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
                    self.environment, AuditLimits()
                )

    def test_response_expansion_precedes_source_validation(self):
        (self.build / "source.rsp").write_text("other.cpp", encoding="utf-8")
        with self.assertRaisesRegex(AuditInfrastructureError, "does not match"):
            self.make(self.entry(arguments=[str(self.compiler), "@source.rsp"]))

    def test_compiler_probe_is_bounded_and_fail_closed(self):
        class FakeProcess:
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

        with mock.patch("gpu_capability_command.subprocess.Popen", side_effect=overflow_process) as popen:
            with self.assertRaisesRegex(AuditInfrastructureError, "version output limit"):
                from gpu_capability_command import _probe_compiler_version
                _probe_compiler_version(self.compiler, CompilerFamily.GCC, self.build, self.environment)
            self.assertFalse(popen.call_args.kwargs["shell"])

        with mock.patch(
            "gpu_capability_command.subprocess.Popen",
            side_effect=lambda *_args, **kwargs: FakeProcess(
                stdout=kwargs["stdout"], running=True
            ),
        ), mock.patch(
            "gpu_capability_command.time.monotonic", side_effect=(10.0, 16.0)
        ), mock.patch("gpu_capability_command.time.sleep"):
            with self.assertRaisesRegex(AuditInfrastructureError, "version probe timeout"):
                from gpu_capability_command import _probe_compiler_version
                _probe_compiler_version(self.compiler, CompilerFamily.GCC, self.build, self.environment)

    def test_msvc_probe_uses_a_temporary_source_and_leaves_no_artifact(self):
        captured: dict[str, object] = {}

        class FakeProcess:
            returncode = 0

            def __init__(self, command, *, stderr, **_kwargs):
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

        with mock.patch("gpu_capability_command.subprocess.Popen", FakeProcess):
            from gpu_capability_command import _probe_compiler_version
            output = _probe_compiler_version(
                self.compiler, CompilerFamily.MSVC, self.build, self.environment
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
            " child=subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'], env=os.environ.copy())\n"
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
            "child=subprocess.Popen([sys.executable, '-c', child_code], env=os.environ.copy())\n"
            "pathlib.Path(os.environ['GPU_PROBE_PID']).write_text(str(child.pid), encoding='ascii')\n"
            "print('gcc (GCC) 13.1.0', flush=True)\n",
            encoding="utf-8",
        )
        environment = dict(
            os.environ,
            GPU_PROBE_PID=str(pid_file),
            GPU_PROBE_STARTUP_PID=str(startup_pid_file),
            GPU_PROBE_HEARTBEAT=str(heartbeat),
            PYTHONPATH=str(fixture),
        )
        from gpu_capability_command import _run_probe_command, _WindowsProbeJob
        if os.name == "nt":
            original_attach = _WindowsProbeJob.attach

            def delayed_attach(job, process):
                deadline = time.monotonic() + 0.5
                while not startup_pid_file.is_file() and time.monotonic() < deadline:
                    time.sleep(0.005)
                return original_attach(job, process)

            with mock.patch.object(_WindowsProbeJob, "attach", delayed_attach):
                output = _run_probe_command(
                    [sys.executable, str(parent)], Path(sys.executable), fixture, environment
                )
        else:
            output = _run_probe_command(
                [sys.executable, str(parent)], Path(sys.executable), fixture, environment
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
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=family,
            compiler=(self.root / compiler_name).resolve(),
            working_directory=self.root.resolve(),
            source=identity,
            arguments=arguments,
            environment_digest="environment",
            digest=f"cfg-{family.value}",
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
            ("arguments", "dependency_output", "dependency_format"),
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
            ("-include-pth", "prefix.pth"),
            ("-isystem", "SDK Path"),
            ("-triple", "x86_64-pc-windows-msvc"),
            ("-aux-triple", "x86_64-pc-windows-msvc"),
            ("-target-cpu", "x86-64"),
            ("-target-feature", "+sse2"),
            ("-target-sdk-version", "15.0"),
            ("-fmodule-map-file", "module.modulemap"),
            ("-fmodule-file", "Core=Core.pcm"),
            ("-fmodules-cache-path", "module-cache"),
            ("-fmodules-user-build-path", "module-user-build"),
            ("-fmodule-implementation-of", "Core"),
            ("-fmodule-feature", "cplusplus"),
            ("-mrelocation-model", "pic"),
            ("-mthread-model", "posix"),
            ("-target-linker-version", "14.0"),
            ("-fmodules-prune-interval", "604800"),
        )
        safe_flags = (
            "-DKEEP=1", "-UOLD", "-Iinclude", "-std=c++20",
            "-fmodules", "-fimplicit-module-maps", "-fcxx-exceptions",
            "-Wno-unknown-warning-option", "-O2", "-gline-tables-only",
            "-mrelax-all", "-mnoexecstack", "-masm-verbose",
            "-mconstructor-aliases", "-mframe-pointer=all",
            "-fmodule-file=Core=Core.pcm",
            "-fmodules-cache-path=module-cache",
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
            "-mcode-model=small",
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
            "-include", "-include-pch", "-include-pth", "-isystem",
            "-triple", "-aux-triple", "-target-cpu", "-target-feature",
            "-target-sdk-version", "-fmodule-map-file", "-fmodule-file",
            "-fmodules-cache-path", "-fmodules-user-build-path",
            "-fmodule-implementation-of", "-fmodule-feature",
            "-mrelocation-model", "-mthread-model", "-target-linker-version",
            "-fmodules-prune-interval",
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


if __name__ == "__main__":
    unittest.main()
