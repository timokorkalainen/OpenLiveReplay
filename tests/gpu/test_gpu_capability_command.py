from __future__ import annotations

import dataclasses
import os
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_command import (  # noqa: E402
    decode_compile_entry,
    expand_response_files,
    identify_compiler,
    make_configuration,
    strip_launchers,
)
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompilerFamily,
    FileIdentity,
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
            "--compiler=g++", "--compiler-check", "content", "--compiler-type", "gcc",
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


if __name__ == "__main__":
    unittest.main()
