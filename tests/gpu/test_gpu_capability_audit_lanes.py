import dataclasses
import sys
import tracemalloc
import unittest
from array import array
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompactTokenSequence,
    CompilerFamily,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    _current_process_rss_bytes,
)
from gpu_capability_source_audit import (  # noqa: E402
    OP_SCOPE_HEADER,
    REGISTRY_HEADER,
    AggregatedFinding,
    AuditBuffer,
    Finding,
    TOKEN_PATTERN,
    aggregate_findings,
    audit_preprocessed_view,
)


class CompilerAuditLaneTests(unittest.TestCase):
    def setUp(self):
        self.limits = dataclasses.replace(
            AuditLimits(), retained_token_bytes=8 * 1024 * 1024
        )
        self.external_identity = self.identity(
            None, canonical="C:/sdk/wrapper.h", production=False
        )

    def identity(
        self,
        relative: str | None,
        *,
        canonical: str | None = None,
        production: bool = True,
        inode: int | None = None,
    ) -> FileIdentity:
        canonical = canonical or "D:/repo/" + (relative or "external.h")
        return FileIdentity(
            canonical=Path(canonical),
            relative=PurePosixPath(relative) if relative is not None else None,
            device=3,
            inode=inode if inode is not None else abs(hash(canonical)) & 0xFFFF,
            line_count=10_000,
            production=production,
        )

    def configuration(self, source: FileIdentity, digest: str = "cfg-a"):
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=CompilerFamily.GCC,
            compiler=Path("C:/toolchain/g++.exe"),
            working_directory=Path("D:/repo/build"),
            source=source,
            arguments=(str(source.canonical),),
            environment_digest="environment-a",
            digest=digest,
        )

    @staticmethod
    def spellings(source: bytes) -> tuple[bytes, ...]:
        text = source.decode("ascii")
        return tuple(match.group(0).encode("ascii") for match in TOKEN_PATTERN.finditer(text))

    def view(self, segments, *, digest: str = "cfg-a"):
        identities = [None]
        identity_ids = {}
        spelling_table = []
        spelling_ids = {}
        fields = []
        dependencies = []
        for identity, line, inclusion, source in segments:
            key = id(identity)
            if key not in identity_ids:
                identity_ids[key] = len(identities)
                identities.append(identity)
                dependencies.append(identity)
            for spelling in self.spellings(source):
                spelling_id = spelling_ids.get(spelling)
                if spelling_id is None:
                    spelling_id = len(spelling_table)
                    spelling_ids[spelling] = spelling_id
                    spelling_table.append(spelling)
                fields.append((spelling_id, identity_ids[key], inclusion, line))
        source_identity = next(
            identity for identity, _line, _inclusion, _source in segments
            if identity.production
        )
        configuration = self.configuration(source_identity, digest)
        tokens = CompactTokenSequence._from_token_fields(
            configuration,
            spellings=tuple(spelling_table),
            identities=tuple(identities),
            fields=fields,
        )
        return PreprocessedTranslationUnitView(
            configuration, tokens, tuple(dict.fromkeys(dependencies))
        )

    def production(self, path, line, source, inclusion=1):
        return self.identity(path), line, inclusion, source

    def external(self, source, *, line=1, inclusion=1):
        return self.external_identity, line, inclusion, source

    def one_million_token_view(self):
        count = 1_000_000
        identity = self.identity("playback/gpu/example.cpp")
        configuration = self.configuration(identity)
        tokens = CompactTokenSequence._from_packed(
            configuration,
            spellings=(b"x",),
            identities=(None, identity),
            spelling_ids=array("I", (0,)) * count,
            identity_ids=array("I", (1,)) * count,
            inclusion_ids=array("I", (4,)) * count,
            original_lines=array("I", (12,)) * count,
        )
        return PreprocessedTranslationUnitView(configuration, tokens, (identity,))

    def test_nonproduction_scope_context_is_kept_but_not_reported(self):
        view = self.view((
            self.external(b"void sdk(nativeHandle); namespace outer {"),
            self.production(
                "playback/gpu/gpufence.h", 41, b"lease.nativeHandle();"
            ),
            self.external(b"}"),
        ))
        findings = audit_preprocessed_view(
            view, self.limits, _current_process_rss_bytes
        )
        self.assertEqual(
            [(item.path, item.line) for item in findings],
            [(PurePosixPath("playback/gpu/gpufence.h"), 41)],
        )

    def test_external_guarded_spelling_cannot_report(self):
        view = self.view((
            self.production("playback/gpu/empty.cpp", 1, b"void anchor();"),
            self.external(b"surface.nativeHandle();"),
        ))
        self.assertEqual(
            audit_preprocessed_view(view, self.limits, _current_process_rss_bytes),
            [],
        )

    def test_audit_buffer_consumes_packed_runs_without_token_views(self):
        view = self.one_million_token_view()
        with mock.patch.object(
            CompactTokenSequence,
            "__iter__",
            side_effect=AssertionError("token iteration"),
        ), mock.patch.object(
            CompactTokenSequence,
            "__getitem__",
            side_effect=AssertionError("token materialization"),
        ):
            buffer = AuditBuffer.from_preprocessed(
                view, self.limits, _current_process_rss_bytes
            )
        self.assertLess(buffer.peak_rss_bytes, self.limits.rss_bytes)
        self.assertEqual(len(buffer.text), 2_000_000)

    def test_repeated_header_instance_reports_the_selected_instance(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (header, 12, 4, b"void first();"),
            self.external(b"void between();", inclusion=2),
            (header, 27, 5, b"lease.nativeHandle();"),
        ))
        findings = audit_preprocessed_view(view, self.limits, lambda: 0)
        self.assertEqual([(item.path, item.line) for item in findings], [
            (PurePosixPath("playback/gpu/gpufence.h"), 27)
        ])

    def test_location_at_maps_normalized_offset_to_exact_origin(self):
        view = self.view((
            self.external(b"namespace sdk {"),
            self.production(
                "playback/gpu/gpufence.h",
                52,
                b"lease.nativeHandle();",
                inclusion=11,
            ),
            self.external(b"}"),
        ))
        buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        location = buffer.location_at(buffer.text.index("nativeHandle"))
        self.assertEqual(location.identity.relative, PurePosixPath("playback/gpu/gpufence.h"))
        self.assertEqual(location.inclusion_instance, 11)
        self.assertEqual(location.line, 52)
        self.assertEqual(location.configuration_digest, "cfg-a")

    def test_scope_can_span_external_include_context(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                10,
                8,
                b"void safe(const std::shared_ptr<GpuSurface>& s) { "
                b"GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
                b"void* handle = lease.nativeHandle();",
            ),
            self.external(b"using sdk_type = int;", inclusion=9),
            (header, 11, 8, b"bool valid = handle != nullptr; (void) valid; scope.complete(); }"),
        ))
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])

    def test_external_complete_cannot_satisfy_production_read_scope(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                20,
                3,
                b"void bad(const std::shared_ptr<GpuSurface>& s) { "
                b"GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
                b"void* handle = lease.nativeHandle(); bool valid = handle != nullptr;",
            ),
            self.external(b"scope.complete();", inclusion=4),
            (header, 21, 3, b"(void) valid; }"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_other_inclusion_complete_cannot_satisfy_read_scope(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                30,
                6,
                b"void bad(const std::shared_ptr<GpuSurface>& s) { "
                b"GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
                b"void* handle = lease.nativeHandle(); bool valid = handle != nullptr;",
            ),
            (header, 31, 7, b"scope.complete();"),
            (header, 32, 6, b"(void) valid; }"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_withread_callback_tokens_cannot_cross_provenance(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                40,
                9,
                b"void bad(const std::shared_ptr<GpuSurface>& s) { "
                b"GpuSyncReadScope scope; scope.withRead(s,",
            ),
            self.external(
                b"[](const GpuReadLease&) { longjmp(env, 1); }", inclusion=10
            ),
            (header, 41, 9, b"); }"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_external_lease_declaration_prefix_cannot_authorize_production_use(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (header, 110, 12, b"void bad(const Surface& s) { GpuSyncReadScope scope;"),
            self.external(b"const GpuReadLease lease =", inclusion=13),
            (
                header,
                111,
                12,
                b"scope.read(s); void* handle = lease.nativeHandle(); "
                b"bool valid = handle != nullptr; (void)valid; scope.complete(); }",
            ),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_external_alias_prefix_cannot_consume_production_native_handle(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                120,
                14,
                b"void bad(const Surface& s) { GpuSyncReadScope scope; "
                b"const GpuReadLease lease = scope.read(s);",
            ),
            self.external(b"void* handle =", inclusion=15),
            (
                header,
                121,
                14,
                b"lease.nativeHandle(); bool valid = handle != nullptr; "
                b"(void)valid; scope.complete(); }",
            ),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_external_receiver_token_cannot_borrow_production_native_member(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                130,
                16,
                b"void bad(const Surface& s) { GpuSyncReadScope scope; "
                b"const GpuReadLease lease = scope.read(s); void* handle =",
            ),
            self.external(b"lease", inclusion=17),
            (
                header,
                131,
                16,
                b".nativeHandle(); bool valid = handle != nullptr; "
                b"(void)valid; scope.complete(); }",
            ),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_external_allowlisted_declaration_prefix_fails_provenance(self):
        header = self.identity("playback/gpu/gpusurface.h")
        view = self.view((
            self.external(b"virtual void*", inclusion=18),
            (header, 88, 19, b"nativeHandle() const = 0;"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_external_native_handle_inside_production_declaration_fails_provenance(self):
        header = self.identity("playback/gpu/gpusurface.h")
        view = self.view((
            (
                header,
                88,
                24,
                b"class GpuSurface { protected: virtual void*",
            ),
            self.external(b"nativeHandle", inclusion=25),
            (header, 88, 24, b"() const = 0; };"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_wholly_external_native_handle_declaration_is_ignored(self):
        production = self.identity("playback/gpu/example.cpp")
        view = self.view((
            self.external(
                b"class ExternalSurface { protected: virtual void* "
                b"nativeHandle() const = 0; };",
                inclusion=26,
            ),
            (production, 1, 27, b"void ok() {}"),
        ))
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])

    def test_external_completion_cannot_complete_production_lifecycle(self):
        header = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (
                header,
                140,
                20,
                b"void bad(const Surface& s) { GpuSyncReadScope scope; "
                b"const GpuReadLease lease = scope.read(s); void* handle = "
                b"lease.nativeHandle(); bool valid = handle != nullptr; (void)valid;",
            ),
            self.external(b"scope.complete();", inclusion=21),
            (header, 141, 20, b"}"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_unrelated_stream_members_may_have_external_arguments(self):
        production = self.identity("playback/gpu/example.cpp")
        for member in (b"read", b"complete", b"withRead"):
            with self.subTest(member=member):
                view = self.view((
                    (production, 150, 22, b"void ok() { Stream stream; stream." + member + b"("),
                    self.external(b"external_argument", inclusion=23),
                    (production, 150, 22, b"); }"),
                ))
                self.assertEqual(
                    audit_preprocessed_view(view, self.limits, lambda: 0), []
                )

    def test_withread_callback_and_typed_method_keep_path_policy(self):
        path = "playback/output/win/wingpuimportedge.cpp"
        source = (
            b"void safe(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
            b"scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
            b"static_cast<ID3D11Texture2D*>(lease.nativeHandle()); if (texture) "
            b"texture->GetDesc(&desc); }); }"
        )
        view = self.view((self.production(path, 224, source),))
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])

    def test_typed_method_is_not_approved_for_an_unreviewed_path(self):
        source = (
            b"void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
            b"scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
            b"static_cast<ID3D11Texture2D*>(lease.nativeHandle()); if (texture) "
            b"texture->GetDesc(&desc); }); }"
        )
        findings = audit_preprocessed_view(
            self.view((self.production("playback/gpu/gpufence.h", 95, source),)),
            self.limits,
            lambda: 0,
        )
        self.assertTrue(any(item.line == 95 for item in findings))

    def test_internal_native_handle_declaration_allowlist_is_preserved(self):
        source = (
            b"class GpuSurface { protected: virtual void* nativeHandle() const = 0; };"
        )
        view = self.view((
            self.production("playback/gpu/gpusurface.h", 88, source),
        ))
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])

    def test_withread_nonlocal_jump_reports_callback_location(self):
        path = "playback/gpu/gpufence.h"
        source = (
            b"void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
            b"scope.withRead(s, [](const GpuReadLease&) { longjmp(env, 1); }); }"
        )
        findings = audit_preprocessed_view(
            self.view((self.production(path, 63, source),)), self.limits, lambda: 0
        )
        self.assertTrue(any(item.line == 63 and "non-local jump" in item.reason
                            for item in findings))

    def test_explicit_complete_after_withread_is_rejected(self):
        path = "playback/gpu/gpufence.h"
        source = (
            b"void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
            b"scope.withRead(s, [](const GpuReadLease&) {}); scope.complete(); }"
        )
        findings = audit_preprocessed_view(
            self.view((self.production(path, 74, source),)), self.limits, lambda: 0
        )
        self.assertTrue(any("withRead() owns completion" in item.reason
                            for item in findings))

    def test_mixed_provenance_capability_expression_fails_closed(self):
        production = self.identity("playback/gpu/gpufence.h")
        view = self.view((
            (production, 90, 3, b"lease."),
            self.external(b"nativeHandle", inclusion=4),
            (production, 90, 3, b"();"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "mixed provenance"):
            audit_preprocessed_view(view, self.limits, lambda: 0)

    def test_public_registry_and_op_scope_members_report_from_production_only(self):
        for path, class_name, member in (
            (REGISTRY_HEADER, b"GpuRetireRegistry", b"registerRetire"),
            (OP_SCOPE_HEADER, b"GpuOpScope", b"track"),
        ):
            with self.subTest(path=path):
                source = b"class " + class_name + b" final { public: void " + member + b"(); };"
                findings = audit_preprocessed_view(
                    self.view((self.production(str(path), 14, source),)),
                    self.limits,
                    lambda: 0,
                )
                self.assertTrue(any(item.path == path and item.line == 14
                                    for item in findings))

    def test_external_public_class_does_not_shadow_protected_production_class(self):
        production = self.identity(str(REGISTRY_HEADER))
        view = self.view((
            self.external(
                b"class GpuRetireRegistry { public: void registerRetire(); };",
                inclusion=3,
            ),
            (
                production,
                18,
                4,
                b"class GpuRetireRegistry { protected: void registerRetire(); };",
            ),
        ))
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])

    def test_literal_token_cannot_manufacture_a_guarded_candidate(self):
        identity = self.identity("playback/gpu/example.cpp")
        configuration = self.configuration(identity)
        spellings = (
            b"const", b"char", b"*", b"text", b"=",
            b'"surface.nativeHandle()"', b";",
        )
        tokens = CompactTokenSequence._from_token_fields(
            configuration,
            spellings=spellings,
            identities=(None, identity),
            fields=((index, 1, 2, 7) for index in range(len(spellings))),
        )
        view = PreprocessedTranslationUnitView(configuration, tokens, (identity,))
        buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        self.assertNotIn("nativeHandle", buffer.text)
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])

    def test_normalized_buffer_limit_is_fail_closed(self):
        view = self.view((
            self.production("playback/gpu/example.cpp", 1, b"identifier identifier"),
        ))
        limits = dataclasses.replace(self.limits, retained_token_bytes=4)
        with self.assertRaisesRegex(AuditInfrastructureError, "normalized audit byte limit"):
            AuditBuffer.from_preprocessed(view, limits, lambda: 0)

    def test_rss_limit_is_checked_before_normalized_allocation(self):
        view = self.view((
            self.production("playback/gpu/example.cpp", 1, b"identifier"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "coordinator RSS limit"):
            AuditBuffer.from_preprocessed(
                view,
                dataclasses.replace(self.limits, rss_bytes=100),
                lambda: 99,
            )

    def test_run_mapping_reserve_is_checked_before_array_growth(self):
        view = self.view((
            self.production("playback/gpu/example.cpp", 1, b"x"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "coordinator RSS limit"):
            AuditBuffer.from_preprocessed(
                view,
                dataclasses.replace(self.limits, rss_bytes=200_000),
                lambda: 50_000,
            )

    def test_normalized_limit_counts_compact_run_mappings(self):
        view = self.view((
            self.production("playback/gpu/example.cpp", 1, b"x"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "normalized audit byte limit"):
            AuditBuffer.from_preprocessed(
                view,
                dataclasses.replace(self.limits, retained_token_bytes=45),
                lambda: 0,
            )

    def test_retained_limit_counts_origin_flags_at_exact_boundary(self):
        identity = self.identity("playback/gpu/example.cpp")
        view = self.view((
            (identity, 1, 1, b"x"),
            (identity, 2, 1, b"x"),
        ))
        with self.assertRaisesRegex(AuditInfrastructureError, "normalized audit byte limit"):
            AuditBuffer.from_preprocessed(
                view,
                dataclasses.replace(self.limits, retained_token_bytes=88),
                lambda: 0,
            )
        buffer = AuditBuffer.from_preprocessed(
            view,
            dataclasses.replace(self.limits, retained_token_bytes=89),
            lambda: 0,
        )
        self.assertEqual(buffer.text, "x\nx\n")

    def test_origin_flags_copy_is_reserved_before_immutable_conversion(self):
        identities = tuple(
            self.identity(
                None,
                canonical=f"C:/sdk/origin-{index}.h",
                production=False,
                inode=index + 1,
            )
            for index in range(64)
        )
        production = self.identity("playback/gpu/example.cpp")
        view = self.view(tuple(
            (identity, 1, index + 1, b"x")
            for index, identity in enumerate(identities)
        ) + ((production, 1, 65, b"x"),))
        reserves = []
        original = AuditBuffer._sample_rss

        def record(reader, limit, peak, *, reserve=0):
            reserves.append(reserve)
            return original(reader, limit, peak, reserve=reserve)

        with mock.patch.object(AuditBuffer, "_sample_rss", side_effect=record):
            buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        normalized_bytes = len(buffer.text.encode("latin-1"))
        self.assertIn(normalized_bytes + 65, reserves)

    def test_many_unterminated_scope_tokens_have_linear_provenance_work(self):
        count = 256
        identity = self.identity("playback/gpu/example.cpp")
        configuration = self.configuration(identity)
        tokens = CompactTokenSequence._from_packed(
            configuration,
            spellings=(b"GpuSyncReadScope",),
            identities=(None, identity),
            spelling_ids=array("I", (0,)) * count,
            identity_ids=array("I", (1,)) * count,
            inclusion_ids=array("I", (2,)) * count,
            original_lines=array("I", (9,)) * count,
        )
        view = PreprocessedTranslationUnitView(configuration, tokens, (identity,))
        original = AuditBuffer.token_spelling
        calls = 0

        def counted(buffer, index):
            nonlocal calls
            calls += 1
            return original(buffer, index)

        with mock.patch.object(AuditBuffer, "token_spelling", autospec=True,
                               side_effect=counted):
            audit_preprocessed_view(view, self.limits, lambda: 0)
        self.assertLess(calls, count * 8)

    def test_repeated_inclusions_use_a_constant_number_of_grammar_passes(self):
        original = __import__(
            "gpu_capability_source_audit"
        ).audit_capability_uses
        pass_counts = []
        for count in (50, 100, 200, 400):
            identity = self.identity("playback/gpu/gpufence.h")
            segments = tuple(
                (
                    identity,
                    index + 1,
                    index + 1,
                    (
                        f"void f{index}(const Surface& s) {{ GpuSyncReadScope scope{index}; "
                        f"scope{index}.withRead(s, [](const GpuReadLease&) {{}}); }}"
                    ).encode("ascii"),
                )
                for index in range(count)
            )
            view = self.view(segments)
            calls = 0

            def counted(*args, **kwargs):
                nonlocal calls
                calls += 1
                return original(*args, **kwargs)

            with mock.patch(
                "gpu_capability_source_audit.audit_capability_uses",
                side_effect=counted,
            ):
                audit_preprocessed_view(view, self.limits, lambda: 0)
            pass_counts.append(calls)
        self.assertLessEqual(max(pass_counts), 8)
        self.assertEqual(len(set(pass_counts)), 1)

    def test_many_provenance_runs_use_compact_bounded_metadata(self):
        count = 20_000
        identity = self.identity("playback/gpu/example.cpp")
        configuration = self.configuration(identity)
        tokens = CompactTokenSequence._from_packed(
            configuration,
            spellings=(b"x",),
            identities=(None, identity),
            spelling_ids=array("I", (0,)) * count,
            identity_ids=array("I", (1,)) * count,
            inclusion_ids=array("I", (2,)) * count,
            original_lines=array("I", (1, 2)) * (count // 2),
        )
        view = PreprocessedTranslationUnitView(configuration, tokens, (identity,))
        tracemalloc.start()
        try:
            AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 2 * 1024 * 1024)


class FindingAggregationTests(unittest.TestCase):
    def finding(self, *, line=17):
        return Finding(
            PurePosixPath("playback/gpu/gpufence.h"),
            line,
            "GpuSurface::nativeHandle()",
            "native handle access must use a lease",
        )

    def test_union_reports_all_forbidden_configurations_once(self):
        aggregated = aggregate_findings((
            (self.finding(), "cfg-b"),
            (self.finding(), "cfg-a"),
            (self.finding(), "cfg-a"),
        ))
        self.assertEqual(len(aggregated), 1)
        self.assertIsInstance(aggregated[0], AggregatedFinding)
        self.assertEqual(aggregated[0].configurations, ("cfg-a", "cfg-b"))

    def test_aggregation_is_sorted_by_diagnostic_identity(self):
        late = self.finding(line=30)
        early = self.finding(line=2)
        aggregated = aggregate_findings(((late, "cfg"), (early, "cfg")))
        self.assertEqual([item.finding.line for item in aggregated], [2, 30])


if __name__ == "__main__":
    unittest.main()
