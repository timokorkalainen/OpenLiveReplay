import ast
import dataclasses
import gc
import hashlib
import json
import os
import statistics
import subprocess
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
import gpu_capability_runner as capability_runner  # noqa: E402

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

    def test_absolute_marker_resolution_is_normalized_for_native_and_windows_paths(self):
        builder = PreprocessedStreamBuilder(
            self.configuration(), self.production, AuditLimits(), lambda: 0
        )
        for marker in (str(Path(__file__).resolve()), "D:\\repo\\playback\\a.cpp"):
            with self.subTest(marker=marker):
                resolved, relative = builder._resolve_marker_path(marker)
                self.assertEqual(
                    resolved,
                    Path(provenance._display_normalized(marker)),
                )
                self.assertFalse(relative)

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

    def test_stream_builder_polls_deadline_inside_long_token_scans(self):
        cases = (
            ("identifier", b"identifier" * (4 * 1024 * 1024)),
            ("number", b"1" * (4 * 1024 * 1024)),
            ("literal", b'"' + b"x" * (4 * 1024 * 1024) + b'"'),
        )
        for name, token in cases:
            with self.subTest(name=name):
                builder = PreprocessedStreamBuilder(
                    self.configuration(),
                    self.production,
                    AuditLimits(retained_token_bytes=64 * 1024 * 1024),
                    lambda: 0,
                    deadline=time.monotonic() + 0.01,
                )
                builder.feed(b'# 1 "D:/repo/playback/a.cpp"\n')
                started = time.monotonic()
                with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
                    builder.feed(token + b"\n")
                self.assertLess(time.monotonic() - started, 1.0)

    def test_maximal_candidate_scan_polls_cancellation_without_deadline(self):
        cancelled = threading.Event()
        builder = PreprocessedStreamBuilder(
            self.configuration(),
            self.production,
            AuditLimits(retained_token_bytes=64 * 1024 * 1024),
            lambda: 0,
            cancel_event=cancelled,
        )
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            builder._poll_candidate_scan_budget(
                b"identifier" * (provenance._TOKEN_BUDGET_POLL_BYTES // 2)
            )

    def test_maximal_candidates_poll_with_a_distant_deadline(self):
        cases = (
            b"identifier" * (provenance._TOKEN_BUDGET_POLL_BYTES // 2),
            b"1" * (provenance._TOKEN_BUDGET_POLL_BYTES * 4),
            b'"' + b"x" * (provenance._TOKEN_BUDGET_POLL_BYTES * 4) + b'"',
        )
        builder = PreprocessedStreamBuilder(
            self.configuration(),
            self.production,
            AuditLimits(retained_token_bytes=64 * 1024 * 1024),
            lambda: 0,
            deadline=time.monotonic() + 60.0,
        )
        for token in cases:
            with self.subTest(prefix=token[:1]), mock.patch.object(
                PreprocessedStreamBuilder, "_check_budget", autospec=True
            ) as check:
                builder._poll_candidate_scan_budget(token)
                self.assertGreaterEqual(check.call_count, 4)

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


@dataclasses.dataclass(frozen=True)
class _ScannerOutcome:
    value: object | None = None
    error: Exception | None = None

    @property
    def succeeded(self):
        return self.error is None


def _frozen_byte_scan(builder, content: bytes) -> None:
    """Task-8 byte scanner, frozen solely as the Task-9 differential oracle."""

    def identifier_start(byte):
        return (
            byte == ord("_")
            or ord("A") <= byte <= ord("Z")
            or ord("a") <= byte <= ord("z")
            or byte >= 0x80
        )

    def identifier_continue(byte):
        return identifier_start(byte) or ord("0") <= byte <= ord("9")

    index = 0
    while index < len(content):
        byte = content[index]
        if builder._in_block_comment:
            close = content.find(b"*/", index)
            builder._check_budget()
            if close < 0:
                return
            builder._in_block_comment = False
            index = close + 2
            continue
        if byte in b" \t\r\f\v":
            index += 1
            continue
        if content.startswith(b"//", index):
            return
        if content.startswith(b"/*", index):
            builder._in_block_comment = True
            index += 2
            continue
        start = index
        if identifier_start(byte):
            index += 1
            while index < len(content) and identifier_continue(content[index]):
                index += 1
        elif ord("0") <= byte <= ord("9") or (
            byte == ord(".")
            and index + 1 < len(content)
            and content[index + 1 : index + 2].isdigit()
        ):
            index += 1
            while index < len(content):
                current = content[index]
                if identifier_continue(current) or current in b".'":
                    index += 1
                    continue
                if current in b"+-" and index > start and content[index - 1] in b"eEpP":
                    index += 1
                    continue
                break
        elif byte in (ord('"'), ord("'")):
            quote = byte
            index += 1
            escaped = False
            while index < len(content):
                current = content[index]
                index += 1
                if escaped:
                    escaped = False
                elif current == ord("\\"):
                    escaped = True
                elif current == quote:
                    break
            else:
                raise AuditInfrastructureError(
                    "compiler output contains an unterminated literal"
                )
        else:
            punctuator = next(
                (
                    value
                    for value in provenance._PUNCTUATORS
                    if content.startswith(value, index)
                ),
                None,
            )
            index += len(punctuator) if punctuator is not None else 1
        builder._append_token(content[start:index])


class _FrozenByteScannerBuilder(PreprocessedStreamBuilder):
    def _tokenize(self, content: bytes) -> None:
        _frozen_byte_scan(self, content)

    def _scan_line(self, content: bytes) -> None:
        _frozen_byte_scan(self, content)


class _CountingScannerBuilder(PreprocessedStreamBuilder):
    __slots__ = ("token_count", "token_bytes")

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.token_count = 0
        self.token_bytes = 0

    def _append_token(self, spelling: bytes) -> None:
        self.token_count += 1
        self.token_bytes += len(spelling)


class _CountingFrozenScannerBuilder(_CountingScannerBuilder):
    def _tokenize(self, content: bytes) -> None:
        _frozen_byte_scan(self, content)

    def _scan_line(self, content: bytes) -> None:
        _frozen_byte_scan(self, content)


class _SpellingScannerBuilder(_CountingScannerBuilder):
    __slots__ = ("spellings",)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.spellings = []

    def _append_token(self, spelling: bytes) -> None:
        super()._append_token(spelling)
        self.spellings.append(spelling)


class _SpellingFrozenScannerBuilder(_SpellingScannerBuilder):
    def _tokenize(self, content: bytes) -> None:
        _frozen_byte_scan(self, content)

    def _scan_line(self, content: bytes) -> None:
        _frozen_byte_scan(self, content)


class ProvenanceScannerParityTests(unittest.TestCase):
    """Exact Task-8 semantics around the Task-9 candidate scanner."""

    def setUp(self):
        ProvenanceTests.setUp(self)

    identity = ProvenanceTests.identity
    configuration = ProvenanceTests.configuration

    def _capture(self, builder_type, stream, splits, family, dependencies):
        try:
            builder = builder_type(
                self.configuration(family),
                self.production,
                AuditLimits(retained_token_bytes=128 * 1024 * 1024),
                lambda: 0,
            )
            offset = 0
            for width in splits:
                builder.feed(stream[offset : offset + width])
                offset += width
            builder.feed(stream[offset:])
            return _ScannerOutcome(value=builder.finalize(dependencies))
        except Exception as error:  # The oracle compares exact fail-closed surfaces.
            return _ScannerOutcome(error=error)

    @staticmethod
    def _compact_columns(view):
        return (
            view.configuration.digest,
            view.dependencies,
            tuple(
                (
                    token.spelling,
                    token.location.identity,
                    token.location.inclusion_instance,
                    token.location.line,
                    token.location.configuration_digest,
                )
                for token in view.tokens
            ),
        )

    @staticmethod
    def _feasible_differential_matrix(stream):
        length = len(stream)
        yielded = set()

        def admit(widths):
            widths = tuple(widths)
            if widths not in yielded:
                yielded.add(widths)
                return widths
            return None

        for widths in ((length,), *((offset, length - offset) for offset in range(1, length))):
            candidate = admit(widths)
            if candidate is not None:
                yield candidate
        candidate = admit((1,) * length)
        if candidate is not None:
            yield candidate
        for index in range(32):
            seed = hashlib.sha256(
                b"olr-task-9-three-way\0" + index.to_bytes(2, "big") + stream
            ).digest()
            first = int.from_bytes(seed[:8], "big") % (length + 1)
            second = int.from_bytes(seed[8:16], "big") % (length + 1)
            first, second = sorted((first, second))
            candidate = admit((first, second - first, length - second))
            if candidate is not None:
                yield candidate

    def assert_scanners_equal(
        self, stream, *, family=CompilerFamily.GCC, dependencies=None, succeeds=True
    ):
        dependencies = dependencies or (self.main_identity,)
        expected = self._capture(
            _FrozenByteScannerBuilder,
            stream,
            (len(stream),),
            family,
            dependencies,
        )
        self.assertEqual(expected.succeeded, succeeds)
        for splits in self._feasible_differential_matrix(stream):
            actual = self._capture(
                PreprocessedStreamBuilder, stream, splits, family, dependencies
            )
            if expected.succeeded:
                self.assertTrue(actual.succeeded, (splits, actual.error))
                self.assertEqual(
                    self._compact_columns(actual.value),
                    self._compact_columns(expected.value),
                    splits,
                )
            else:
                self.assertFalse(actual.succeeded, splits)
                self.assertIs(type(actual.error), type(expected.error), splits)
                self.assertEqual(str(actual.error), str(expected.error), splits)

    def test_small_corpus_matches_frozen_tokens_provenance_and_failures(self):
        # Keep the physical line non-directive-shaped at every feed boundary;
        # adjacent alternatives still exercise longest-first recognition.
        punctuators = b"x" + b"".join(provenance._PUNCTUATORS)
        lexical = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b"alpha /* split\nblock */ beta // line comment\n"
            b'"escaped\\\"string" \'escaped\\\'character\' '
            b"1e+2 0x1p-3 7e + 4p - 9 u8\"eight\" u\"short\" "
            b"U'wide' L\"long\" .5e-1 lease.\xc3\xb1ative()\n"
            + punctuators
            + b"\n"
        )
        gcc_markers = (
            b'# 0 "D:/repo/playback/a.cpp"\n'
            b'# 1 "D:/repo/build//"\n'
            b'# 0 "<built-in>"\n# 0 "<command-line>"\n'
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'#pragma GCC visibility push(default)\n'
            b'# 1 "D:/repo/playback/h.h" 1 3 4\nheader_token\n'
            b'# 5 "D:/repo/playback/a.cpp" 2\nmain_token\n'
        )
        recursive_gcc_markers = (
            b'# 1 "D:/repo/playback/a.cpp"\nmain_before\n'
            b'# 1 "D:/repo/playback/h.h" 1\nheader_outer\n'
            b'# 10 "D:/repo/playback/a.cpp" 1\nmain_recursive\n'
            b'# 2 "D:/repo/playback/h.h" 1\nheader_inner\n'
            b'# 11 "D:/repo/playback/a.cpp" 2\nmain_recursive_return\n'
            b'# 3 "D:/repo/playback/h.h" 2\nheader_outer_return\n'
            b'# 2 "D:/repo/playback/a.cpp" 2\nmain_after\n'
        )
        msvc_markers = (
            b'#line 1 "D:\\repo\\playback\\a.cpp"\nmain_token\n'
            b'#line 1 "D:\\repo\\playback\\h.h"\nheader_token\n'
            b'#line 4 "D:\\repo\\playback\\a.cpp"\nreturn_token\n'
        )
        macro_outputs = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b"lease.nativeHandle(); lease.safeX(); lease.safe(); "
            b'const char* value = "lease.CAT(native,Handle)()"; SELF\n'
        )
        cases = (
            ("lexical", lexical, CompilerFamily.GCC,
             (self.main_identity,), True),
            ("gcc-markers", gcc_markers, CompilerFamily.GCC,
             (self.main_identity, self.header_identity), True),
            ("recursive-gcc-markers", recursive_gcc_markers,
             CompilerFamily.GCC,
             (self.main_identity, self.header_identity), True),
            ("msvc-markers", msvc_markers, CompilerFamily.MSVC,
             (self.main_identity, self.header_identity), True),
            ("macro-outputs", macro_outputs, CompilerFamily.GCC,
             (self.main_identity,), True),
            (
                "multiline-raw-spelling",
                b'# 1 "D:/repo/playback/a.cpp"\nR"tag(first\nsecond)tag"\n',
                CompilerFamily.GCC,
                (self.main_identity,),
                False,
            ),
        )
        for label, stream, family, dependencies, succeeds in cases:
            with self.subTest(label=label):
                self.assert_scanners_equal(
                    stream,
                    family=family,
                    dependencies=dependencies,
                    succeeds=succeeds,
                )

    def test_named_lexical_boundaries_match_before_inside_and_after(self):
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'alpha /* block */ beta "escaped\\\"quote" '
            b"'escaped\\\'character' u8\"eight\" u\"short\" U'wide' "
            b'L"long" R"tag(raw)tag" 1e+2 0x1p-3 '
            b'lease.\xc3\xb1ative() '
            + b" ".join(provenance._PUNCTUATORS)
            + b' // line comment\n'
        )
        features = {
            "marker": (b'# 1 "',),
            "block-comment": (b"/* block */",),
            "line-comment": (b"// line comment",),
            "escape": (b'\\\"', b"\\\'"),
            "literal-prefix": (b'u8"', b'u"', b"U'", b'L"'),
            "literal-terminator": (b'quote"', b"character'"),
            "raw-delimiters": (b'R"tag(', b')tag"'),
            "pp-number-sign": (b"e+", b"p-"),
            "utf8": (b"\xc3\xb1",),
            "punctuator": provenance._PUNCTUATORS,
        }
        expected = self._capture(
            _FrozenByteScannerBuilder, stream, (len(stream),),
            CompilerFamily.GCC, (self.main_identity,))
        self.assertTrue(expected.succeeded, expected.error)
        exercised = set()
        for label, needles in features.items():
            for needle in needles:
                start = stream.find(needle)
                self.assertGreaterEqual(start, 0, (label, needle))
                for boundary in range(max(1, start - 1),
                                      min(len(stream), start + len(needle) + 2)):
                    exercised.add(label)
                    for builder_type in (
                            PreprocessedStreamBuilder,
                            _FrozenByteScannerBuilder):
                        observed = self._capture(
                            builder_type, stream,
                            (boundary, len(stream) - boundary),
                            CompilerFamily.GCC, (self.main_identity,))
                        self.assertTrue(
                            observed.succeeded,
                            (label, needle, boundary, observed.error))
                        self.assertEqual(
                            self._compact_columns(observed.value),
                            self._compact_columns(expected.value),
                            (label, needle, boundary, builder_type.__name__))
        self.assertEqual(exercised, set(features))

    def test_frozen_prefixed_literals_and_pp_numbers_keep_exact_boundaries(self):
        stream = (
            b'# 1 "D:/repo/playback/a.cpp"\n'
            b'u8"a" u"b" U"c" L"d" 1e+2 0x1p-3\n'
        )
        outcome = self._capture(
            PreprocessedStreamBuilder,
            stream,
            (stream.index(b"e+") + 1, 1, len(stream)),
            CompilerFamily.GCC,
            (self.main_identity,),
        )
        self.assertTrue(outcome.succeeded, outcome.error)
        self.assertEqual(
            tuple(token.spelling for token in outcome.value.tokens),
            (
                b"u8", b'"a"', b"u", b'"b"', b"U", b'"c"', b"L",
                b'"d"', b"1e+2", b"0x1p-3",
            ),
        )

    def test_compiled_candidate_pattern_and_punctuator_order_are_exact(self):
        self.assertEqual(
            provenance._PUNCTUATORS,
            tuple(sorted(provenance._PUNCTUATORS, key=len, reverse=True)),
        )
        self.assertEqual(
            provenance._PUNCTUATOR_PATTERN,
            b"|".join(provenance.re.escape(value)
                       for value in provenance._PUNCTUATORS),
        )
        self.assertEqual(
            provenance._TOKEN_CANDIDATE.pattern,
            (
                rb"(?P<whitespace>[ \t\f\v\r]+)"
                rb"|(?P<line_comment>//[^\r\n]*)"
                rb"|(?P<block_comment>/\*(?:[^*]|\*(?!/))*\*/)"
                rb"|(?P<identifier>[_A-Za-z\x80-\xff][_A-Za-z0-9\x80-\xff]*)"
                rb"|(?P<number>(?:[0-9]|\.[0-9])"
                rb"(?:[eEpP][+-]|[_A-Za-z0-9.\'\x80-\xff])*)"
                rb'|(?P<string>"(?:\\.|[^"\\])*")'
                rb"|(?P<character>\'(?:\\.|[^\'\\])*\')"
                rb"|(?P<punctuator>" + provenance._PUNCTUATOR_PATTERN + rb")"
                rb"|(?P<unknown>[^\r\n])"
            ),
        )

    def test_authoritative_maximal_match_observes_concurrent_cancellation(self):
        cancelled = threading.Event()
        authoritative_entered = threading.Event()
        builder = _CountingScannerBuilder(
            self.configuration(),
            self.production,
            AuditLimits(retained_token_bytes=64 * 1024 * 1024),
            lambda: 0,
            cancel_event=cancelled,
        )
        content = b"identifier" * (2 * provenance._TOKEN_BUDGET_POLL_BYTES)

        def cancel_after_authoritative_entry():
            if authoritative_entered.wait(2.0):
                cancelled.set()

        def authoritative_chunk_completed(*_args):
            authoritative_entered.set()
            self.assertTrue(cancelled.wait(1.0))

        thread = threading.Thread(
            target=cancel_after_authoritative_entry, daemon=True
        )
        thread.start()
        started = time.monotonic()
        with mock.patch.object(
            provenance,
            "_authoritative_candidate_chunk_completed",
            side_effect=authoritative_chunk_completed,
            create=True,
        ) as completed, self.assertRaisesRegex(
            AuditInfrastructureError, "cancelled"
        ):
            builder._scan_line(content)
        thread.join(timeout=2.0)

        self.assertTrue(authoritative_entered.is_set())
        completed.assert_called()
        self.assertLess(time.monotonic() - started, 1.0)

    def test_authoritative_chunks_preserve_boundary_spanning_maximal_tokens(self):
        boundary = provenance._TOKEN_BUDGET_POLL_BYTES
        cases = [
            ("identifier", b"a" * (boundary + 17), (0,)),
            ("number", b"1" * (boundary + 17), (0,)),
            (
                "number-exponent",
                b"1" + b"2" * (boundary - 2) + b"e+3",
                (0,),
            ),
            (
                "literal",
                b'"' + b"a" * (boundary + 9) + b'\\"z"',
                (0,),
            ),
        ]
        for punctuator in provenance._PUNCTUATORS:
            cases.append(
                (
                    f"punctuator-{punctuator!r}",
                    punctuator + b" tail",
                    range(1, len(punctuator)),
                )
            )
        cases.extend((
            ("line-comment", b"//comment tail", (1,)),
            ("block-comment", b"/*comment*/ tail", (1,)),
        ))
        for name, candidate, splits in cases:
            for split in splits:
                for offset in (-1, 0, 1):
                    prefix = b" " * max(0, boundary - split + offset)
                    content = prefix + candidate
                    actual = _SpellingScannerBuilder(
                        self.configuration(), self.production,
                        AuditLimits(retained_token_bytes=64 * 1024 * 1024),
                        lambda: 0,
                    )
                    frozen = _SpellingFrozenScannerBuilder(
                        self.configuration(), self.production,
                        AuditLimits(retained_token_bytes=64 * 1024 * 1024),
                        lambda: 0,
                    )
                    with self.subTest(
                        name=name, split=split, offset=offset
                    ):
                        actual._scan_line(content)
                        frozen._scan_line(content)
                        self.assertEqual(
                            tuple(actual.spellings), tuple(frozen.spellings)
                        )
                        self.assertEqual(
                            (actual.token_count, actual.token_bytes),
                            (frozen.token_count, frozen.token_bytes),
                        )

    def test_continued_block_comment_observes_concurrent_cancellation(self):
        cancelled = threading.Event()
        continuation_entered = threading.Event()
        builder = _SpellingScannerBuilder(
            self.configuration(), self.production,
            AuditLimits(retained_token_bytes=64 * 1024 * 1024),
            lambda: 0,
            cancel_event=cancelled,
        )
        builder._in_block_comment = True

        def cancel_during_continuation():
            if continuation_entered.wait(2.0):
                cancelled.set()

        def continuation_chunk_completed(*_args):
            continuation_entered.set()
            self.assertTrue(cancelled.wait(1.0))

        thread = threading.Thread(
            target=cancel_during_continuation, daemon=True
        )
        thread.start()
        try:
            with mock.patch.object(
                provenance,
                "_authoritative_candidate_chunk_completed",
                side_effect=continuation_chunk_completed,
            ) as completed, self.assertRaisesRegex(
                AuditInfrastructureError, "cancelled"
            ):
                builder._scan_line(
                    b"x" * (2 * provenance._TOKEN_BUDGET_POLL_BYTES + 17)
                )
        finally:
            thread.join(timeout=2.0)
        self.assertTrue(continuation_entered.is_set())
        completed.assert_called()

    def test_continued_block_comment_observes_deadline_between_chunks(self):
        builder = _SpellingScannerBuilder(
            self.configuration(), self.production,
            AuditLimits(retained_token_bytes=64 * 1024 * 1024),
            lambda: 0,
            deadline=time.monotonic() + 60.0,
        )
        builder._in_block_comment = True

        def expire_deadline(*_args):
            builder._deadline = time.monotonic() - 1.0

        with mock.patch.object(
            provenance,
            "_authoritative_candidate_chunk_completed",
            side_effect=expire_deadline,
        ) as completed, self.assertRaisesRegex(
            AuditInfrastructureError, "deadline"
        ):
            builder._scan_line(
                b"x" * (2 * provenance._TOKEN_BUDGET_POLL_BYTES + 17)
            )
        completed.assert_called()

    def test_continued_block_comment_boundaries_match_frozen_oracle(self):
        boundary = provenance._TOKEN_BUDGET_POLL_BYTES
        cases = [
            (f"close-{offset:+d}", b"x" * (boundary + offset) + b"*/tail")
            for offset in (-1, 0, 1)
        ]
        cases.extend(
            (f"no-close-{size}", b"x" * size)
            for size in (
                boundary - 1,
                boundary,
                boundary + 1,
                2 * boundary + 1,
            )
        )
        for name, content in cases:
            with self.subTest(name=name):
                actual = _SpellingScannerBuilder(
                    self.configuration(), self.production,
                    AuditLimits(retained_token_bytes=64 * 1024 * 1024),
                    lambda: 0,
                )
                frozen = _SpellingFrozenScannerBuilder(
                    self.configuration(), self.production,
                    AuditLimits(retained_token_bytes=64 * 1024 * 1024),
                    lambda: 0,
                )
                actual._in_block_comment = True
                frozen._in_block_comment = True
                actual._scan_line(content)
                frozen._scan_line(content)
                self.assertEqual(
                    tuple(actual.spellings), tuple(frozen.spellings)
                )
                self.assertEqual(
                    (actual.token_count, actual.token_bytes),
                    (frozen.token_count, frozen.token_bytes),
                )
                self.assertEqual(
                    actual._in_block_comment, frozen._in_block_comment
                )

    def _count_scan(self, builder_type, content):
        builder = builder_type(
            self.configuration(), self.production, AuditLimits(), lambda: 0
        )
        scanner = getattr(builder, "_scan_line", None)
        if scanner is None:
            scanner = builder._tokenize
        scanner(content)
        return builder.token_count, builder.token_bytes

    @staticmethod
    def _regenerate_required_mingw_capture(manifest, compiler, source_bytes):
        source_relative = PurePosixPath(manifest["source"]["relative_path"])
        output_relative = PurePosixPath(manifest["capture"]["relative_path"])
        if (
            source_relative.is_absolute()
            or output_relative.is_absolute()
            or ".." in source_relative.parts
            or ".." in output_relative.parts
        ):
            raise AssertionError("MinGW capture paths are not controlled")
        with tempfile.TemporaryDirectory(prefix="olr-task9-mingw-regenerate-") as name:
            root = Path(name).resolve()
            source = root.joinpath(*source_relative.parts)
            output = root.joinpath(*output_relative.parts)
            source.parent.mkdir(parents=True)
            output.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(source_bytes)
            command = (str(compiler), *manifest["command"][1:])
            environment = {
                key: os.environ[key]
                for key in ("SystemRoot", "WINDIR", "TEMP", "TMP")
                if key in os.environ
            }
            environment["PATH"] = str(compiler.parent)
            completed = subprocess.run(
                command,
                cwd=root,
                env=environment,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30.0,
            )
            if completed.returncode != 0:
                raise AssertionError(
                    "required MinGW capture regeneration failed: "
                    + completed.stderr[-4096:].decode("utf-8", errors="replace"))
            size = output.stat().st_size
            if size != manifest["capture"]["byte_count"]:
                raise AssertionError(
                    f"regenerated MinGW capture byte count differs: {size}")
            return output.read_bytes()

    def test_one_million_dense_tokens_match_reference_scanner(self):
        content = b"x " * 1_000_000
        self.assertEqual(
            self._count_scan(_CountingScannerBuilder, content),
            self._count_scan(_CountingFrozenScannerBuilder, content),
        )

    def test_one_million_dense_tokens_match_at_locked_feed_widths(self):
        token_count = 1_000_000
        stream = (b'# 1 "D:/repo/playback/a.cpp"\n'
                  + b"x " * token_count + b"\n")
        expected = self._count_scan(
            _CountingFrozenScannerBuilder, b"x " * token_count)
        self.assertEqual(expected, (token_count, token_count))
        for width in (1, 2, 3, 7, 4096):
            with self.subTest(width=width):
                builder = _CountingScannerBuilder(
                    self.configuration(), self.production, AuditLimits(),
                    lambda: 0)
                for offset in range(0, len(stream), width):
                    builder.feed(stream[offset:offset + width])
                self.assertEqual(
                    (builder.token_count, builder.token_bytes), expected)

    def test_compiled_scanner_is_at_least_twice_reference_throughput(self):
        fixture_root = Path(__file__).resolve().parent / "fixtures"
        manifest = json.loads(
            (fixture_root / "task9_mingw_preprocessed.json").read_text(
                encoding="utf-8"))
        source = fixture_root / "task9_mingw_capture.cpp"
        capture = fixture_root / "task9_mingw_preprocessed.ii"
        source_bytes = source.read_bytes()
        real_output = capture.read_bytes()
        self.assertEqual(manifest["schema"], "olr-task9-mingw-preprocessed-v1")
        self.assertEqual(
            manifest["toolchain"]["distribution"], "tools_mingw1310_64")
        self.assertEqual(
            manifest["command"],
            [
                "g++", "-std=c++20", "-E", "-ftrack-macro-expansion=0",
                "-fno-working-directory",
                "tests/gpu/fixtures/task9_mingw_capture.cpp", "-o",
                "tests/gpu/fixtures/task9_mingw_preprocessed.ii",
            ],
        )
        self.assertEqual(
            hashlib.sha256(source_bytes).hexdigest(), manifest["source"]["sha256"])
        self.assertEqual(
            source_bytes.count(b"\n"), manifest["source"]["line_count"])
        self.assertEqual(len(real_output), manifest["capture"]["byte_count"])
        self.assertGreaterEqual(len(real_output), 256 * 1024)
        self.assertLessEqual(len(real_output), 4 * 1024 * 1024)
        self.assertEqual(
            hashlib.sha256(real_output).hexdigest(), manifest["capture"]["sha256"])
        self.assertEqual(
            manifest["accepted_oracle"]["dependency_schema"],
            "one-production-source-v1")
        self.assertEqual(
            manifest["accepted_oracle"]["line_schema"], "gcc-line-marker-v1")

        required_compiler = os.environ.get("OLR_TASK9_REFERENCE_COMPILER")
        if required_compiler is not None:
            regenerate = getattr(self, "_regenerate_required_mingw_capture", None)
            self.assertTrue(callable(regenerate),
                            "required MinGW capture regeneration is unavailable")
            compiler = Path(required_compiler)
            self.assertTrue(compiler.is_absolute() and compiler.is_file())
            self.assertEqual(os.environ.get("OLR_TASK9_REQUIRED_FAMILY"), "GNU")
            self.assertEqual(
                hashlib.sha256(compiler.read_bytes()).hexdigest(),
                manifest["toolchain"]["compiler_executable_sha256"])
            version = subprocess.run(
                (str(compiler), "--version"), check=True, capture_output=True,
                text=True).stdout.splitlines()[0]
            target = subprocess.run(
                (str(compiler), "-dumpmachine"), check=True, capture_output=True,
                text=True).stdout.strip()
            self.assertIn(manifest["toolchain"]["compiler_version"], version)
            self.assertEqual(target, manifest["toolchain"]["target"])
            regenerated = regenerate(manifest, compiler, source_bytes)
            self.assertEqual(regenerated, real_output)
            self.assertEqual(
                hashlib.sha256(regenerated).hexdigest(),
                manifest["capture"]["sha256"])

        metadata = source.stat()
        identity = FileIdentity(
            source.resolve(),
            PurePosixPath(manifest["source"]["relative_path"]),
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            manifest["source"]["line_count"],
            True,
        )
        configuration = dataclasses.replace(
            self.configuration(),
            working_directory=Path(__file__).resolve().parents[2],
            source=identity,
            arguments=(manifest["source"]["relative_path"],),
        )
        production = {identity.relative: identity}
        dependencies = (identity,)
        chunk_bytes = manifest["replay"]["chunk_bytes"]
        self.assertEqual(chunk_bytes, capability_runner._IO_CHUNK_BYTES)
        self.assertEqual(manifest["replay"]["warmups_per_builder"], 1)
        self.assertEqual(
            manifest["replay"]["balanced_order"], ["AB", "BA", "AB", "BA"])

        def compact_oracle(view):
            digest = hashlib.sha256(b"olr-task9-compact-oracle-v1\0")
            digest.update(view.configuration.digest.encode("utf-8") + b"\0")
            digest.update(len(view.dependencies).to_bytes(4, "big"))
            for dependency in view.dependencies:
                digest.update(dependency.relative.as_posix().encode("utf-8") + b"\0")
            count = 0
            for token in view.tokens:
                count += 1
                for value in (
                    token.spelling,
                    token.location.identity.relative.as_posix().encode("utf-8"),
                    str(token.location.inclusion_instance).encode("ascii"),
                    str(token.location.line).encode("ascii"),
                    token.location.configuration_digest.encode("utf-8"),
                ):
                    digest.update(len(value).to_bytes(4, "big"))
                    digest.update(value)
            return count, digest.hexdigest()

        def replay(builder_type):
            builder = builder_type(
                configuration,
                production,
                AuditLimits(retained_token_bytes=128 * 1024 * 1024),
                lambda: 0,
            )
            gc.collect()
            gc.disable()
            try:
                started = time.perf_counter()
                for offset in range(0, len(real_output), chunk_bytes):
                    builder.feed(real_output[offset:offset + chunk_bytes])
                view = builder.finalize(dependencies)
                elapsed = time.perf_counter() - started
            finally:
                gc.enable()
            return elapsed, view

        expected_columns = None
        expected_oracle = None
        for builder_type in (_FrozenByteScannerBuilder, PreprocessedStreamBuilder):
            _elapsed, view = replay(builder_type)
            columns = self._compact_columns(view)
            oracle = compact_oracle(view)
            expected_columns = columns if expected_columns is None else expected_columns
            expected_oracle = oracle if expected_oracle is None else expected_oracle
            self.assertEqual(columns, expected_columns)
            self.assertEqual(oracle, expected_oracle)
            del view, columns
            gc.collect()
        self.assertEqual(expected_oracle[0], manifest["accepted_oracle"]["token_count"])
        self.assertEqual(
            expected_oracle[1], manifest["accepted_oracle"]["compact_sha256"])

        samples = {"A": [], "B": []}
        builders = {"A": PreprocessedStreamBuilder, "B": _FrozenByteScannerBuilder}
        for pair in manifest["replay"]["balanced_order"]:
            for label in pair:
                elapsed, view = replay(builders[label])
                self.assertEqual(self._compact_columns(view), expected_columns)
                self.assertEqual(compact_oracle(view), expected_oracle)
                samples[label].append(elapsed)
                del view
                gc.collect()

        native_seconds = statistics.median(samples["A"])
        reference_seconds = statistics.median(samples["B"])
        speedup = reference_seconds / native_seconds
        evidence = {
            "compiler_executable_sha256": manifest["toolchain"][
                "compiler_executable_sha256"],
            "compiler_version": manifest["toolchain"]["compiler_version"],
            "target": manifest["toolchain"]["target"],
            "raw_byte_count": len(real_output),
            "raw_sha256": manifest["capture"]["sha256"],
            "token_count": expected_oracle[0],
            "compact_oracle_sha256": expected_oracle[1],
            "chunk_bytes": chunk_bytes,
            "warmups_per_builder": manifest["replay"]["warmups_per_builder"],
            "balanced_order": manifest["replay"]["balanced_order"],
            "native_samples": samples["A"],
            "reference_samples": samples["B"],
            "native_median": native_seconds,
            "reference_median": reference_seconds,
            "speedup": speedup,
        }
        print(json.dumps(evidence, sort_keys=True))
        self.assertGreaterEqual(
            speedup,
            2.0,
            f"compiled scanner throughput is {speedup:.3f}x reference "
            f"({reference_seconds:.6f}s/{native_seconds:.6f}s); "
            f"evidence={json.dumps(evidence, sort_keys=True)}",
        )


if __name__ == "__main__":
    unittest.main()
