import ast
import dataclasses
import gc
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import weakref
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_capability_provenance as provenance  # noqa: E402

from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompilerExecutableCapability,
    CompilerFamily,
    DependencyRootBinding,
    FileIdentity,
    PreprocessConfiguration,
    _current_process_rss_bytes,
)
from gpu_capability_provenance import (  # noqa: E402
    PreprocessedStreamBuilder,
    parse_gcc_dependencies,
    parse_msvc_dependencies,
    reject_source_line_spoofs,
    validate_dependency_identities,
)


class ProvenanceTests(unittest.TestCase):
    def test_provenance_module_has_no_optimization_sensitive_assertions(self):
        path = Path(__file__).resolve().with_name("gpu_capability_provenance.py")
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=path.name)
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(tree)))

    def setUp(self):
        self.main_identity = self.identity("playback/a.cpp", line_count=50)
        self.header_identity = self.identity("playback/h.h", line_count=50)
        self.adapter_identity = self.identity(
            "playback/gpu/adapter.h", line_count=50
        )
        self.production = {
            identity.relative: identity
            for identity in (
                self.main_identity,
                self.header_identity,
                self.adapter_identity,
            )
        }

    def test_dependency_documents_reject_sparse_payload_before_open(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        for name, parser in (("deps.d", parse_gcc_dependencies), ("deps.json", parse_msvc_dependencies)):
            path = root / name
            with path.open("wb") as stream:
                stream.truncate(4 * 1024 * 1024 + 1)
            original_open = Path.open

            def guarded_open(candidate, *args, **kwargs):
                if candidate == path:
                    raise AssertionError("oversized dependency document was opened")
                return original_open(candidate, *args, **kwargs)

            with self.subTest(name=name), mock.patch.object(
                Path, "open", guarded_open
            ), self.assertRaisesRegex(AuditInfrastructureError, "byte ceiling"):
                parser(
                    path, deadline=time.monotonic() + 10.0,
                    cancel_event=None,
                )

    def test_dependency_document_parsers_honor_cancellation_and_deadline(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        depfile = Path(temporary.name).resolve() / "deps.d"
        depfile.write_text("out: source.cpp\n", encoding="utf-8")
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            parse_gcc_dependencies(
                depfile, deadline=time.monotonic() + 10.0,
                cancel_event=cancelled,
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            parse_gcc_dependencies(
                depfile, deadline=time.monotonic() - 1.0,
                cancel_event=None,
            )

    def test_gcc_dependency_entry_count_is_bounded_before_path_materialization(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        depfile = Path(temporary.name).resolve() / "many.d"
        depfile.write_bytes(b"out: " + b"a " * 70_000 + b"\n")
        with mock.patch(
            "gpu_capability_provenance.Path",
            side_effect=AssertionError("dependency Path materialized"),
        ), self.assertRaisesRegex(AuditInfrastructureError, "entry ceiling"):
            parse_gcc_dependencies(
                depfile, deadline=time.monotonic() + 10.0,
                cancel_event=None,
            )

    def test_gcc_dependency_parser_preserves_mingw_native_path_separators(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        depfile = Path(temporary.name).resolve() / "mingw.d"
        depfile.write_bytes(
            b"object.o: D:\\Development\\OpenLiveReplay\\playback\\gpu\\source.cpp\n"
        )
        self.assertEqual(
            parse_gcc_dependencies(depfile),
            (Path(r"D:\Development\OpenLiveReplay\playback\gpu\source.cpp"),),
        )

    def test_msvc_dependency_entry_count_is_bounded_before_json_materialization(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        document = Path(temporary.name).resolve() / "many.json"
        document.write_text(
            json.dumps({
                "Data": {"Source": "source.cpp", "Includes": ["a"] * 70_000}
            }, separators=(",", ":")),
            encoding="utf-8",
        )
        with mock.patch(
            "gpu_capability_provenance.json.loads",
            side_effect=AssertionError("JSON objects materialized"),
        ), self.assertRaisesRegex(AuditInfrastructureError, "entry ceiling"):
            parse_msvc_dependencies(
                document, deadline=time.monotonic() + 10.0,
                cancel_event=None,
            )

    def identity(
        self,
        relative: str | None,
        *,
        canonical: str | None = None,
        line_count: int = 20,
        production: bool = True,
    ) -> FileIdentity:
        if canonical is None:
            canonical = "D:/repo/" + (relative or "sdk/wrapper.h")
        return FileIdentity(
            canonical=Path(canonical),
            relative=PurePosixPath(relative) if relative else None,
            device=3,
            inode=abs(hash(canonical)) & 0xFFFF,
            line_count=line_count,
            production=production,
        )

    def configuration(self, family=CompilerFamily.GCC):
        executable = self.identity(
            None, canonical="C:/toolchain/compiler.exe", line_count=0,
            production=False,
        )
        binding = DependencyRootBinding(
            "toolchain", Path("C:/toolchain"),
            self.identity(None, canonical="C:/toolchain", line_count=0,
                          production=False),
        )
        capability = CompilerExecutableCapability(
            "windows", executable, "1" * 64, "2" * 64, object(), binding,
            (), (), "3" * 64, (),
        )
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=family,
            compiler=Path("C:/toolchain/compiler.exe"),
            working_directory=Path("D:/repo/build"),
            source=self.main_identity,
            arguments=("../playback/a.cpp",),
            environment_digest="env",
            digest="cfg",
            dependency_root_authority_digest="a" * 64,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )

    def builder(self, *, family=CompilerFamily.GCC, limits=None, rss_reader=None):
        return PreprocessedStreamBuilder(
            self.configuration(family),
            self.production,
            limits or AuditLimits(),
            rss_reader or (lambda: 0),
        )

    def parse(self, stream, *, family=CompilerFamily.GCC, deps=(), chunks=None):
        builder = self.builder(family=family)
        if chunks is None:
            builder.feed(stream)
        else:
            offset = 0
            for width in chunks:
                builder.feed(stream[offset : offset + width])
                offset += width
            builder.feed(stream[offset:])
        return builder.finalize(tuple(deps))

    def test_gcc_view_keeps_nonproduction_context_and_production_locations(self):
        sdk = self.identity(
            None,
            canonical="C:/sdk/wrapper.h",
            line_count=20,
            production=False,
        )
        stream = (
            b'# 1 "C:/sdk/wrapper.h" 1\nnamespace sdk {\n'
            b'# 7 "D:/repo/playback/gpu/adapter.h" 1\n'
            b'lease.nativeHandle();\n'
            b'# 3 "C:/sdk/wrapper.h" 2\n}\n'
        )
        view = self.parse(
            stream, deps=(sdk, self.adapter_identity), chunks=(1, 3, 17, 2, 31)
        )
        self.assertEqual(
            b"".join(token.spelling for token in view.tokens),
            b"namespacesdk{lease.nativeHandle();}",
        )
        native = next(
            token for token in view.tokens if token.spelling == b"nativeHandle"
        )
        self.assertEqual(
            native.location.identity.relative,
            PurePosixPath("playback/gpu/adapter.h"),
        )
        self.assertEqual(native.location.line, 7)

    def test_absolute_marker_lookup_does_not_rescan_all_production_paths(self):
        production = {
            PurePosixPath(f"playback/generated-{index}.h"): self.identity(
                f"playback/generated-{index}.h"
            )
            for index in range(2_000)
        }
        production[self.main_identity.relative] = self.main_identity
        builder = PreprocessedStreamBuilder(
            self.configuration(), production, AuditLimits(), lambda: 0
        )
        original = provenance._path_key
        calls = 0

        def counted(path):
            nonlocal calls
            calls += 1
            return original(path)

        stream = b''.join(
            b'# 1 "D:/repo/playback/a.cpp"\nint value;\n'
            for _index in range(100)
        )
        with mock.patch.object(provenance, "_path_key", side_effect=counted):
            builder.feed(stream)
        self.assertLess(calls, 1_000)

    def test_stream_builder_enforces_deadline_during_tokenization(self):
        builder = PreprocessedStreamBuilder(
            self.configuration(),
            self.production,
            AuditLimits(retained_token_bytes=64 * 1024 * 1024),
            lambda: 0,
            deadline=time.monotonic() + 0.01,
        )
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            + b"identifier " * 1_000_000
            + b"\n"
        )
        started = time.monotonic()
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            builder.feed(stream)
        self.assertLess(time.monotonic() - started, 1.0)

    def test_retained_accounting_uses_incremental_intern_totals(self):
        slots = set(PreprocessedStreamBuilder.__slots__)
        self.assertIn("_retained_spelling_bytes", slots)
        self.assertIn("_retained_marker_alias_bytes", slots)

    def test_gcc_balanced_nested_and_repeated_includes_get_distinct_instances(self):
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'# 1 "D:/repo/playback/h.h" 1 3 4\nint one;\n'
            b'# 2 "D:/repo/playback/a.cpp" 2\n'
            b'# 1 "D:/repo/playback/h.h" 1\nint two;\n'
            b'# 3 "D:/repo/playback/a.cpp" 2\nint main_token;\n'
        )
        view = self.parse(stream, deps=(self.main_identity, self.header_identity))
        instances = {
            token.location.inclusion_instance
            for token in view.tokens
            if token.location.identity == self.header_identity
        }
        self.assertEqual(len(instances), 2)
        self.assertNotIn(
            next(
                token.location.inclusion_instance
                for token in view.tokens
                if token.spelling == b"main_token"
            ),
            instances,
        )

    def test_gcc_bootstrap_pseudo_files_are_allowed_only_before_real_code(self):
        stream = (
            b'# 0 "<built-in>"\n# 0 "<command-line>"\n'
            b'# 1 "<command line>"\n'
            b'# 1 "D:/repo/playback/a.cpp"\nint ok;\n'
        )
        self.assertEqual(
            b"".join(t.spelling for t in self.parse(stream, deps=(self.main_identity,)).tokens),
            b"intok;",
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "pseudo-file"):
            self.parse(
                b'# 1 "D:/repo/playback/a.cpp"\nint ok;\n# 1 "<built-in>"\n',
                deps=(self.main_identity,),
            )

    def test_gcc_real_file_line_zero_is_allowed_only_in_bootstrap(self):
        stream = (
            b'# 0 "D:/repo/playback/a.cpp"\n'
            b'# 0 "<built-in>"\n# 0 "<command-line>"\n'
            b'# 1 "D:/repo/playback/a.cpp"\nint ok;\n'
        )
        self.assertEqual(len(self.parse(stream, deps=(self.main_identity,)).tokens), 3)
        with self.assertRaisesRegex(AuditInfrastructureError, "line zero"):
            self.parse(
                b'# 1 "D:/repo/playback/a.cpp"\nint ok;\n'
                b'# 0 "D:/repo/playback/a.cpp"\n',
                deps=(self.main_identity,),
            )

    def test_gcc_mingw_working_directory_bootstrap_marker_is_ignored(self):
        stream = (
            b'# 0 "D:/repo/playback/a.cpp"\n'
            b'# 1 "D:/repo/build//"\n'
            b'# 0 "<built-in>"\n# 0 "<command-line>"\n'
            b'# 1 "D:/repo/playback/a.cpp"\nint ok;\n'
        )
        self.assertEqual(len(self.parse(stream, deps=(self.main_identity,)).tokens), 3)

    def test_gcc_preserved_pragma_is_not_misclassified_as_line_marker(self):
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'#pragma GCC visibility push(default)\nint ok;\n'
        )
        spellings = b"".join(
            token.spelling for token in self.parse(stream, deps=(self.main_identity,)).tokens
        )
        self.assertIn(b"pragmaGCCvisibilitypush", spellings)
        self.assertTrue(spellings.endswith(b"intok;"))

    def test_gcc_recursive_include_returns_to_nearest_matching_ancestor(self):
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'# 1 "D:/repo/playback/h.h" 1\n'
            b'# 10 "D:/repo/playback/a.cpp" 1\n'
            b'# 2 "D:/repo/playback/h.h" 1\n'
            b'# 11 "D:/repo/playback/a.cpp" 2\nint ok;\n'
            b'# 2 "D:/repo/playback/h.h" 2\n'
            b'# 2 "D:/repo/playback/a.cpp" 2\n'
        )
        view = self.parse(stream, deps=(self.main_identity, self.header_identity))
        self.assertEqual(b"".join(token.spelling for token in view.tokens), b"intok;")
        self.assertEqual(next(iter(view.tokens)).location.line, 11)

    def test_absolute_relative_marker_collision_fails_closed(self):
        with self.assertRaisesRegex(AuditInfrastructureError, "absolute.*relative|relative.*absolute"):
            self.parse(
                b'# 1 "D:/repo/playback/a.cpp"\nint one;\n'
                b'# 2 "../playback/a.cpp"\nint two;\n',
                deps=(self.main_identity,),
            )

    def test_gcc_rejects_stack_and_marker_inconsistencies(self):
        cases = (
            (b'# 1 "D:/repo/playback/a.cpp"\n# 1 "D:/repo/playback/h.h" 2\n', "return"),
            (b'# 1 "D:/repo/playback/a.cpp"\n# 1 "D:/repo/playback/h.h"\n', "unflagged"),
            (b'# 1 "D:/repo/playback/a.cpp"\n# 0 "D:/repo/playback/a.cpp"\n', "line zero"),
            (b'# 1 "<stdin>"\n', "pseudo-file"),
            (b'# 1 "<mystery>"\n', "pseudo-file"),
            (b'# 1 "D:/repo/playback/a.cpp" 1', "truncated"),
            (b'# nope "D:/repo/playback/a.cpp"\n', "marker"),
        )
        for stream, message in cases:
            with self.subTest(stream=stream), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                self.parse(stream, deps=(self.main_identity, self.header_identity))

    def test_gcc_rejects_non_dependency_marker_and_out_of_range_line(self):
        other = self.identity(None, canonical="C:/sdk/other.h", production=False)
        with self.assertRaisesRegex(AuditInfrastructureError, "dependency"):
            self.parse(
                b'# 1 "C:/sdk/not-listed.h"\nint x;\n',
                deps=(self.main_identity,),
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "line range"):
            self.parse(
                b'# 51 "D:/repo/playback/a.cpp"\nint x;\n',
                deps=(self.main_identity, other),
            )

    def test_non_ascii_identifiers_remain_opaque_bytes_and_chunks_are_invariant(self):
        stream = b'# 1 "D:/repo/playback/a.cpp"\nlease.\xc3\xb1ative();\n'
        whole = self.parse(stream, deps=(self.main_identity,))
        split = self.parse(
            stream,
            deps=(self.main_identity,),
            chunks=tuple(1 for _ in range(len(stream))),
        )
        self.assertEqual(
            [(t.spelling, t.location) for t in whole.tokens],
            [(t.spelling, t.location) for t in split.tokens],
        )
        self.assertIn(b"\xc3\xb1ative", [token.spelling for token in whole.tokens])

    def test_builder_has_no_raw_output_or_retained_token_view(self):
        builder = self.builder()
        names = set(dir(builder))
        self.assertNotIn("raw_output", names)
        self.assertNotIn("tokens", names)
        self.assertFalse(any("token_view" in name for name in names))

    def test_one_million_dense_tokens_stay_packed(self):
        baseline = _current_process_rss_bytes()
        limits = dataclasses.replace(
            AuditLimits(),
            retained_token_bytes=20 * 1024 * 1024,
            rss_bytes=baseline + 96 * 1024 * 1024,
        )
        builder = self.builder(limits=limits, rss_reader=_current_process_rss_bytes)
        builder.feed(b'# 1 "D:/repo/playback/a.cpp"\n')
        for _ in range(2000):
            builder.feed(b"x " * 500)
        view = builder.finalize((self.main_identity,))
        self.assertEqual(len(view.tokens), 1_000_000)
        self.assertLessEqual(view.tokens.packed_bytes, 16_100_000)
        self.assertLess(builder.peak_rss_bytes, limits.rss_bytes)
        self.assertNotIsInstance(view.tokens, tuple)
        packed_column = object.__getattribute__(view.tokens, "_spelling_ids")
        reference = weakref.ref(packed_column)
        del packed_column
        del view
        gc.collect()
        self.assertIsNone(reference())

    def test_builder_fails_before_packed_limit_is_crossed(self):
        limits = dataclasses.replace(AuditLimits(), retained_token_bytes=1024 * 1024)
        builder = self.builder(limits=limits, rss_reader=lambda: 0)
        builder.feed(b'# 1 "D:/repo/playback/a.cpp"\n')
        with self.assertRaisesRegex(
            AuditInfrastructureError, "retained packed token limit"
        ):
            for _ in range(1000):
                builder.feed(b"x " * 500)
        self.assertLessEqual(builder.retained_bytes, limits.retained_token_bytes)

    def test_rss_reserves_transient_finalize_copy_and_intern_bytes(self):
        samples = iter((0, 0, 0, 5000))
        limits = dataclasses.replace(AuditLimits(), rss_bytes=4096)
        builder = self.builder(limits=limits, rss_reader=lambda: next(samples, 5000))
        with self.assertRaisesRegex(AuditInfrastructureError, "RSS"):
            builder.feed(b'# 1 "D:/repo/playback/a.cpp"\nx;\n')

    def test_msvc_repeated_header_gets_distinct_instances(self):
        view = self.parse(
            b'#line 1 "D:\\repo\\playback\\a.cpp"\n'
            b'#line 1 "D:\\repo\\playback\\h.h"\nint one;\n'
            b'#line 2 "D:\\repo\\playback\\a.cpp"\n'
            b'#line 1 "d:/REPO/playback/h.h"\nint two;\n',
            family=CompilerFamily.MSVC,
            deps=(self.main_identity, self.header_identity),
        )
        instances = {
            token.location.inclusion_instance
            for token in view.tokens
            if token.location.identity == self.header_identity
        }
        self.assertEqual(len(instances), 2)

    def test_msvc_rejects_ambiguous_recursive_or_return_transition(self):
        stream = (
            b'#line 1 "D:/repo/playback/a.cpp"\n'
            b'#line 1 "D:/repo/playback/h.h"\n'
            b'#line 1 "D:/repo/playback/a.cpp"\n'
            b'#line 1 "D:/repo/playback/h.h"\n'
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "ambiguous"):
            self.parse(
                stream,
                family=CompilerFamily.MSVC,
                deps=(self.main_identity, self.header_identity),
            )

    def test_msvc_rejects_malformed_paths_and_encoding(self):
        cases = (
            b'#line 1 "D:/repo/playback/a.cpp\n',
            b'#line 1 "D:/repo/playback/\x00a.cpp"\n',
            b'#line 1 "D:/repo/playback/\x81a.cpp"\n',
        )
        for stream in cases:
            with self.subTest(stream=stream), self.assertRaises(AuditInfrastructureError):
                self.parse(stream, family=CompilerFamily.MSVC, deps=(self.main_identity,))

    def test_dependency_parsers_and_identity_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "playback" / "a.cpp"
            header = root / "sdk space" / "h.h"
            source.parent.mkdir()
            header.parent.mkdir()
            source.write_text("int x;\n", encoding="utf-8")
            header.write_text("#pragma once\n", encoding="utf-8")
            depfile = root / "deps.d"
            escaped_header = str(header).replace("\\", "/").replace(" ", "\\ ")
            depfile.write_text(
                f"object.o: {source.as_posix()} {escaped_header}\\\n {source.as_posix()}\n"
                f"{escaped_header}:\n",
                encoding="utf-8",
            )
            self.assertEqual(
                parse_gcc_dependencies(depfile), (source, header)
            )
            json_path = root / "deps.json"
            json_path.write_text(
                json.dumps(
                    {
                        "Version": "1.2",
                        "Data": {
                            "Source": str(source),
                            "Includes": [str(header), str(source)],
                        },
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(
                parse_msvc_dependencies(json_path), (source, header)
            )
            production_identity = FileIdentity(
                source.resolve(),
                PurePosixPath("playback/a.cpp"),
                None,
                None,
                1,
                True,
            )
            validated = validate_dependency_identities(
                (source, header, source),
                root,
                {production_identity.relative: production_identity},
            )
            self.assertEqual(validated[0], production_identity)
            self.assertEqual(len(validated), 2)
            self.assertFalse(validated[1].production)

            stale = dataclasses.replace(production_identity, line_count=2)
            with self.assertRaisesRegex(AuditInfrastructureError, "changed"):
                validate_dependency_identities(
                    (source,), root, {stale.relative: stale}
                )

    def test_dependency_validation_rejects_relative_alias_and_case_collisions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "playback" / "a.cpp"
            path.parent.mkdir()
            path.write_text("x\n", encoding="utf-8")
            identity = FileIdentity(
                path.resolve(), PurePosixPath("playback/a.cpp"), None, None, 1, True
            )
            with self.assertRaisesRegex(AuditInfrastructureError, "absolute"):
                validate_dependency_identities(
                    (Path("playback/a.cpp"),), root, {identity.relative: identity}
                )
            if os.name == "nt":
                with self.assertRaisesRegex(AuditInfrastructureError, "alias|collision"):
                    validate_dependency_identities(
                        (path, Path(str(path).upper())),
                        root,
                        {identity.relative: identity},
                    )

    def test_dependency_validation_rejects_external_hardlink_to_production(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "playback" / "a.cpp"
            alias = root / "generated" / "a.cpp"
            source.parent.mkdir()
            alias.parent.mkdir()
            source.write_text("int x;\n", encoding="utf-8")
            try:
                os.link(source, alias)
            except OSError as error:
                self.skipTest(f"hardlinks unavailable: {error}")

            metadata = source.stat()
            identity = FileIdentity(
                source.resolve(),
                PurePosixPath("playback/a.cpp"),
                int(metadata.st_dev),
                int(metadata.st_ino),
                1,
                True,
            )
            with self.assertRaisesRegex(AuditInfrastructureError, "aliases production"):
                validate_dependency_identities(
                    (alias,), root, {identity.relative: identity}
                )

    def test_dependency_validation_rejects_duplicate_production_filesystem_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "playback" / "a.cpp"
            second = root / "playback" / "b.cpp"
            first.parent.mkdir()
            first.write_text("int x;\n", encoding="utf-8")
            try:
                os.link(first, second)
            except OSError as error:
                self.skipTest(f"hardlinks unavailable: {error}")

            metadata = first.stat()
            device = int(metadata.st_dev)
            inode = int(metadata.st_ino)
            first_identity = FileIdentity(
                first.resolve(), PurePosixPath("playback/a.cpp"), device, inode, 1, True
            )
            second_identity = FileIdentity(
                second.resolve(), PurePosixPath("playback/b.cpp"), device, inode, 1, True
            )
            production = {
                first_identity.relative: first_identity,
                second_identity.relative: second_identity,
            }
            with self.assertRaisesRegex(
                AuditInfrastructureError, "filesystem identity aliases production path"
            ):
                validate_dependency_identities((), root, production)

    def test_reject_source_line_spoofs_direct_spliced_and_aliases(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "playback" / "gpu" / "target.h"
            source = root / "generated" / "input.h"
            target.parent.mkdir(parents=True)
            source.parent.mkdir()
            target.write_text("x\n", encoding="utf-8")
            identity = FileIdentity(
                target.resolve(),
                PurePosixPath("playback/gpu/target.h"),
                None,
                None,
                1,
                True,
            )
            production = {identity.relative: identity}
            spellings = (
                str(target),
                os.path.relpath(target, source.parent),
                str(target).replace(os.sep, "/").upper(),
            )
            for spelling in spellings:
                with self.subTest(spelling=spelling):
                    source.write_text(f'#line 1 "{spelling}"\n', encoding="utf-8")
                    with self.assertRaisesRegex(AuditInfrastructureError, "spoof"):
                        reject_source_line_spoofs(source, production)
            source.write_bytes(
                (f'#li\\\nne 1 "{target}"\n').encode("utf-8")
            )
            with self.assertRaisesRegex(AuditInfrastructureError, "spoof"):
                reject_source_line_spoofs(source, production)
            source.write_text('#line 1 "unrelated-generated.h"\n', encoding="utf-8")
            reject_source_line_spoofs(source, production)

            target.write_text('# 1 "unrelated-generated.h"\n', encoding="utf-8")
            with self.assertRaisesRegex(AuditInfrastructureError, "spoof"):
                reject_source_line_spoofs(target, production)

    def test_literal_split_across_chunks_does_not_change_tokens(self):
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'const char * value = "hello world"; int number = 1+2;\n'
        )
        whole = self.parse(stream, deps=(self.main_identity,))
        split = self.parse(
            stream,
            deps=(self.main_identity,),
            chunks=tuple(1 for _ in range(len(stream))),
        )
        self.assertEqual(
            [token.spelling for token in whole.tokens],
            [token.spelling for token in split.tokens],
        )
        self.assertIn(b'"hello world"', [token.spelling for token in split.tokens])
        self.assertIn(b"+", [token.spelling for token in split.tokens])


if __name__ == "__main__":
    unittest.main()
