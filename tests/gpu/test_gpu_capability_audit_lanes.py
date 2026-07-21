import ast
import dataclasses
import contextlib
import io
import inspect
import os
import shutil
import subprocess
import struct
import sys
import tempfile
import time
import tracemalloc
import types
import unittest
from array import array
from pathlib import Path, PurePosixPath
from types import FunctionType, MappingProxyType
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompactTokenSequence,
    CompilerExecutableCapability,
    CompilerFamily,
    DependencyRootBinding,
    CoverageReport,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    _current_process_rss_bytes,
)
import gpu_capability_source_audit as capability_audit  # noqa: E402
import gpu_capability_model as capability_model  # noqa: E402
from gpu_capability_source_audit import (  # noqa: E402
    OP_SCOPE_HEADER,
    REGISTRY_HEADER,
    AggregatedFinding,
    AuditBuffer,
    AuditAllocationShape,
    CompilerAuditAnalysis,
    Finding,
    TOKEN_PATTERN,
    aggregate_findings,
    audit_preprocessed_view,
    audit_allocation_plan,
    capability_candidate_spellings,
    conservative_allocation_schema,
    probe_cpython_allocation_layout,
    premeasure_streaming_policy_growth,
    reserve_before_allocation,
    view_has_capability_spelling,
    _audit_preprocessed_view_unfiltered,
)


def _reference_audit_pipeline_streaming(sources, compact_results):
    """Test-only retain-all oracle; production must use the bounded outer path."""

    by_digest = {result.configuration_digest: result for result in compact_results}
    authoritative = frozenset(
        path for result in by_digest.values()
        for path in result.reached_production
    )
    source_only = frozenset(sources) - authoritative
    configurations = tuple(sorted(by_digest))
    observations = [
        (finding, "raw-source")
        for finding in capability_audit.audit_raw_sources(sources)
    ]
    for digest in configurations:
        result = by_digest[digest]
        observations.extend(
            (
                Finding(
                    finding.path, finding.line,
                    finding.expression, finding.reason,
                ),
                digest,
            )
            for finding in result.findings
        )
    for path in sorted(source_only, key=PurePosixPath.as_posix):
        observations.extend(
            (finding, "source-only")
            for finding in capability_audit.audit_source_only(
                path, sources[path]
            )
        )
    return (
        aggregate_findings(observations),
        CoverageReport(authoritative, source_only, configurations),
    )


class AuditEngineFingerprintTests(unittest.TestCase):
    def test_task7_stage_and_module_roots_are_exact(self):
        self.assertEqual(
            capability_audit.AUDIT_ENGINE_GRAPH_SCHEMA_BYTES,
            b"olr-gpu-capability-live-graph-v7",
        )
        self.assertEqual(
            capability_audit.AUDIT_ENGINE_STAGE_BYTES,
            b"task-8-stream-findings-coverage",
        )
        self.assertEqual(
            tuple(module.__name__ for module in capability_audit._AUDIT_ENGINE_TARGET_MODULES),
            (
                "gpu_capability_model",
                "gpu_capability_source_audit",
                "gpu_capability_command",
                "gpu_capability_cache",
                "gpu_capability_provenance",
                "gpu_capability_runner",
            ),
        )

    def test_task5_semantic_graph_owns_worker_and_compact_publication_contract(self):
        names = {
            name for name, _value in capability_audit._enumerate_live_semantic_graph()
        }
        for expected in (
            "gpu_capability_source_audit.AuditBuffer",
            "gpu_capability_source_audit.CompilerAuditAnalysis",
            "gpu_capability_source_audit.CapabilityRule",
            "gpu_capability_source_audit.capability_candidate_spellings",
            "gpu_capability_source_audit.conservative_allocation_schema",
            "gpu_capability_source_audit.cpp_tokens",
            "gpu_capability_provenance.reject_source_line_spoofs",
            "gpu_capability_model.CachePublicationPermit",
            "gpu_capability_model.ConfigurationAuditOutcome",
            "gpu_capability_model.PerTaskCompactReservation",
            "gpu_capability_model.CompilerLaunchEvent",
            "gpu_capability_runner.audit_configuration_worker",
            "gpu_capability_runner.validate_production_dependency_snapshots",
                "gpu_capability_runner._bounded_compact_result_draft",
            "gpu_capability_runner.GenerationReactor",
            "gpu_capability_runner.BoundedFrameChannel",
            "gpu_capability_runner.encode_control_message",
            "gpu_capability_runner.decode_control_message",
            "gpu_capability_runner.encode_task_frame",
            "gpu_capability_runner.decode_task_frame",
            "gpu_capability_runner.schedule_configuration_audits",
            "gpu_capability_runner._transport_allocation_bound",
            "gpu_capability_model.WorkerPayloadReady",
            "gpu_capability_model.WorkerStop",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, names)

    def test_task5_worker_helper_and_default_rebinding_changes_fingerprint(self):
        baseline = capability_audit.audit_engine_fingerprint()
        runner = capability_audit._gpu_capability_runner
        with mock.patch.object(
            runner,
            "validate_production_dependency_snapshots",
            lambda *_args, **_kwargs: (),
        ):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

        worker = runner.audit_configuration_worker
        defaults = worker.__defaults__
        with mock.patch.object(worker, "__defaults__", (None,)):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)
        self.assertEqual(worker.__defaults__, defaults)

        command = capability_audit._gpu_capability_command
        with mock.patch.object(
            command,
            "launch_compiler_process",
            lambda *_args, **_kwargs: (_args, _kwargs),
        ):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

    def test_production_modules_have_no_bare_asserts(self):
        source_directory = Path(__file__).resolve().parent
        names = (
            "gpu_capability_model.py",
            "gpu_capability_command.py",
            "gpu_capability_runner.py",
            "gpu_capability_provenance.py",
            "gpu_capability_cache.py",
            "gpu_capability_source_audit.py",
        )
        for name in names:
            tree = ast.parse(
                (source_directory / name).read_text(encoding="utf-8"), filename=name
            )
            assertions = [node for node in ast.walk(tree) if isinstance(node, ast.Assert)]
            with self.subTest(module=name):
                self.assertEqual(assertions, [])

    def test_preprocess_configuration_constructor_inventory_is_exact(self):
        self.assertEqual(
            dict(capability_audit._PREPROCESS_CONFIGURATION_CONSTRUCTOR_INVENTORY),
            {
                "gpu_capability_command.py": 1,
                "gpu_capability_runner.py": 1,
                "test_gpu_capability_audit_lanes.py": 1,
                "test_gpu_capability_cache.py": 2,
                "test_gpu_capability_command.py": 1,
                "test_gpu_capability_model.py": 1,
                "test_gpu_capability_provenance.py": 1,
                "test_gpu_capability_runner.py": 8,
            },
        )
        source_directory = Path(__file__).resolve().parent
        observed = {}
        for path in source_directory.glob("*.py"):
            count = path.read_text(encoding="utf-8").count(
                "Preprocess" + "Configuration("
            )
            if count:
                observed[path.name] = count
        self.assertEqual(observed, dict(
            capability_audit._PREPROCESS_CONFIGURATION_CONSTRUCTOR_INVENTORY
        ))

    def test_slot_descriptor_owner_rebinding_changes_behavior_and_digest(self):
        class ForeignPackedColumn:
            __slots__ = ("_view",)

        packed_column = capability_model._ReadOnlyPackedColumn
        foreign_view = ForeignPackedColumn.__dict__["_view"]
        self.assertIsInstance(foreign_view, types.MemberDescriptorType)
        baseline = capability_audit.audit_engine_fingerprint()
        with mock.patch.object(packed_column, "_view", foreign_view):
            with self.assertRaises(TypeError):
                packed_column(array("I", (1,)))
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

    def test_getset_descriptor_owner_is_structurally_attested(self):
        type_name = type.__dict__["__name__"]
        function_name = FunctionType.__dict__["__name__"]
        self.assertIsInstance(type_name, types.GetSetDescriptorType)
        self.assertIsInstance(function_name, types.GetSetDescriptorType)
        packed_column = capability_model._ReadOnlyPackedColumn
        with mock.patch.object(
            packed_column, "descriptor_probe", type_name, create=True
        ):
            type_owner_fingerprint = capability_audit.audit_engine_fingerprint()
        with mock.patch.object(
            packed_column, "descriptor_probe", function_name, create=True
        ):
            function_owner_fingerprint = capability_audit.audit_engine_fingerprint()
        self.assertNotEqual(type_owner_fingerprint, function_owner_fingerprint)

    def test_enum_lookup_and_iteration_state_are_attested(self):
        baseline = capability_audit.audit_engine_fingerprint()
        compiler_family = capability_model.CompilerFamily

        lookup = dict(compiler_family._value2member_map_)
        lookup["gcc"] = compiler_family.CLANG
        with mock.patch.object(compiler_family, "_value2member_map_", lookup):
            self.assertIs(compiler_family("gcc"), compiler_family.CLANG)
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

        reordered_lookup = dict(reversed(tuple(compiler_family._value2member_map_.items())))
        with mock.patch.object(
            compiler_family, "_value2member_map_", reordered_lookup
        ):
            self.assertEqual(capability_audit.audit_engine_fingerprint(), baseline)

        reversed_names = list(reversed(compiler_family._member_names_))
        with mock.patch.object(compiler_family, "_member_names_", reversed_names):
            self.assertEqual(
                tuple(compiler_family),
                tuple(reversed(tuple(compiler_family.__members__.values()))),
            )
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

    def test_enum_live_state_rejects_unsupported_values(self):
        compiler_family = capability_model.CompilerFamily
        unsupported_lookup = dict(compiler_family._value2member_map_)
        unsupported_lookup["gcc"] = []
        with mock.patch.object(
            compiler_family, "_value2member_map_", unsupported_lookup
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "Enum member mapping is invalid"
            ):
                capability_audit.audit_engine_fingerprint()

    def test_live_semantic_encoder_supports_python_314_slice_constants(self):
        encoder = capability_audit._LiveSemanticEncoder(
            frozenset(module.__name__ for module in (
                capability_model, capability_audit
            ))
        )
        self.assertNotEqual(
            encoder.encode(slice(None, 4, None)),
            encoder.encode(slice(1, 4, 2)),
        )

    def test_nested_code_globals_are_followed_recursively(self):
        def fixture_outer():
            def inner():
                return nested_runtime_policy

            return inner

        outer = FunctionType(
            fixture_outer.__code__,
            capability_audit.__dict__,
            "nested_policy_outer",
        )
        outer.__module__ = capability_audit.__name__
        outer.__qualname__ = "nested_policy_outer"
        with (
            mock.patch.object(
                capability_audit, "nested_runtime_policy", "first", create=True
            ),
            mock.patch.object(
                capability_audit, "nested_policy_outer", outer, create=True
            ),
        ):
            baseline = capability_audit.audit_engine_fingerprint()
            with mock.patch.object(
                capability_audit, "nested_runtime_policy", "second"
            ):
                self.assertEqual(outer()(), "second")
                self.assertNotEqual(
                    capability_audit.audit_engine_fingerprint(), baseline
                )

    def test_worker_default_mutation_changes_digest_without_hashing_host_count(self):
        baseline = capability_audit.audit_engine_fingerprint()
        defaults = capability_model.AuditLimits.__init__.__defaults__
        self.assertIsNotNone(defaults)
        with mock.patch.object(
            capability_model.AuditLimits.__init__,
            "__defaults__",
            (*defaults[:-1], 999),
        ):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

        source_directory = Path(__file__).resolve().parent
        script = (
            "import os,sys; os.cpu_count=lambda: int(sys.argv[2]); "
            "sys.path.insert(0,sys.argv[1]); "
            "import gpu_capability_source_audit as audit; "
            "print(audit.audit_engine_fingerprint())"
        )

        def fingerprint_for_cpu_count(cpu_count: int) -> str:
            completed = subprocess.run(
                (sys.executable, "-c", script, str(source_directory), str(cpu_count)),
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
            )
            return completed.stdout.strip()

        self.assertEqual(fingerprint_for_cpu_count(1), fingerprint_for_cpu_count(999))

    def test_worker_default_computation_is_a_loaded_semantic_component(self):
        baseline = capability_audit.audit_engine_fingerprint()

        def mutated_worker_default():
            return 999

        with mock.patch.object(
            capability_model, "_default_worker_count", mutated_worker_default
        ):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

    def test_owned_class_data_members_are_exhaustive_and_fail_closed(self):
        baseline = capability_audit.audit_engine_fingerprint()
        packed_column = capability_model._ReadOnlyPackedColumn
        for name, replacement in (
            ("itemsize", 999),
            ("format", "mutated"),
            ("readonly", False),
        ):
            with self.subTest(name=name), mock.patch.object(
                packed_column, name, replacement
            ):
                self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

        with mock.patch.object(
            packed_column, "arbitrary_owned_semantic", 7, create=True
        ):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)
        with mock.patch.object(
            packed_column, "unsupported_owned_semantic", [], create=True
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "unsupported loaded semantic object"
            ):
                capability_audit.audit_engine_fingerprint()

    def test_dataclass_generated_init_and_repr_are_attested(self):
        baseline = capability_audit.audit_engine_fingerprint()

        def replacement_init(self, *_args, **_kwargs):
            object.__setattr__(self, "stable_role", "mutated")

        def replacement_repr(_self):
            return "mutated-dependency"

        for method_name, replacement in (
            ("__init__", replacement_init),
            ("__repr__", replacement_repr),
        ):
            with self.subTest(method=method_name), mock.patch.object(
                capability_model.DependencyDigest, method_name, replacement
            ):
                self.assertNotEqual(
                    capability_audit.audit_engine_fingerprint(), baseline
                )

    def test_unrelated_relative_semantic_string_is_not_an_origin_role(self):
        relative = "tests/gpu/gpu_capability_model.py"
        role = "python-module:gpu_capability_model"
        relative_payload = capability_audit._marshal_live_semantic_object(relative)
        role_payload = capability_audit._marshal_live_semantic_object(role)
        self.assertIn(relative.encode("utf-8"), relative_payload)
        self.assertNotEqual(relative_payload, role_payload)
        relative_fingerprint = capability_audit._audit_engine_fingerprint_from_marshaled_graph(
            (("semantic.fixture", relative_payload),)
        )
        role_fingerprint = capability_audit._audit_engine_fingerprint_from_marshaled_graph(
            (("semantic.fixture", role_payload),)
        )
        self.assertNotEqual(relative_fingerprint, role_fingerprint)

    def test_mapping_proxy_encoding_is_canonical_with_shared_values(self):
        shared = frozenset({"shared-value"})
        first = MappingProxyType({
            PurePosixPath("playback/a.h"): shared,
            PurePosixPath("playback/b.h"): shared,
        })
        reversed_insertion = MappingProxyType({
            PurePosixPath("playback/b.h"): shared,
            PurePosixPath("playback/a.h"): shared,
        })
        self.assertEqual(
            capability_audit._marshal_live_semantic_object(first),
            capability_audit._marshal_live_semantic_object(reversed_insertion),
        )
        with mock.patch.object(
            capability_audit, "REVIEWED_NATIVE_HANDLE_SINKS", first
        ):
            first_fingerprint = capability_audit.audit_engine_fingerprint()
        with mock.patch.object(
            capability_audit,
            "REVIEWED_NATIVE_HANDLE_SINKS",
            reversed_insertion,
        ):
            reversed_fingerprint = capability_audit.audit_engine_fingerprint()
        self.assertEqual(first_fingerprint, reversed_fingerprint)

        first_key = type("DuplicateSemanticKey", (), {})
        second_key = type("DuplicateSemanticKey", (), {})
        for key in (first_key, second_key):
            key.__module__ = "external_semantic_fixture"
            key.__qualname__ = "DuplicateSemanticKey"
        duplicate_semantic_keys = MappingProxyType({
            first_key: frozenset({"first"}),
            second_key: frozenset({"second"}),
        })
        with self.assertRaisesRegex(
            AuditInfrastructureError, "duplicate semantic keys"
        ):
            capability_audit._marshal_live_semantic_object(duplicate_semantic_keys)

    def test_frozenset_encoding_is_canonical_with_shared_nested_values(self):
        source_directory = Path(__file__).resolve().parent
        script = (
            "import hashlib,sys; sys.path.insert(0,sys.argv[1]); "
            "import gpu_capability_source_audit as a; "
            "shared=frozenset({'shared'}); "
            "value=frozenset(((shared,'a'),(shared,'b'))); "
            "print(hashlib.sha256(a._marshal_live_semantic_object(value)).hexdigest())"
        )
        fingerprints = []
        for seed in ("1", "2"):
            completed = subprocess.run(
                (sys.executable, "-c", script, str(source_directory)),
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
                env={**os.environ, "PYTHONHASHSEED": seed},
            )
            fingerprints.append(completed.stdout.strip())
        self.assertEqual(fingerprints[0], fingerprints[1])

    def test_mutable_target_module_dataclass_is_rejected_fail_closed(self):
        @dataclasses.dataclass
        class MutableSemanticFixture:
            value: int = 1

        MutableSemanticFixture.__module__ = capability_audit.__name__
        with mock.patch.object(
            capability_audit,
            "MUTABLE_SEMANTIC_FIXTURE",
            MutableSemanticFixture,
            create=True,
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "dataclass class is mutable"
            ):
                capability_audit.audit_engine_fingerprint()

    def test_live_semantic_graph_is_exhaustive_sorted_and_recomputed(self):
        graph = capability_audit._enumerate_live_semantic_graph()
        names = tuple(name for name, _loaded_object in graph)
        self.assertEqual(names, tuple(sorted(names)))
        self.assertEqual(len(names), len(set(names)))
        for expected in (
            "gpu_capability_cache.ConfigurationAuditCache",
            "gpu_capability_cache.audit_cache_key",
            "gpu_capability_command.CompilerProcessHandleCarrier",
            "gpu_capability_command.launch_compiler_process",
            "gpu_capability_model.CompactResultMemoryBudget",
            "gpu_capability_model.ConfigurationAuditResult",
            "gpu_capability_model.DependencyDigest",
            "gpu_capability_model.PerTaskCompactReservation",
            "gpu_capability_model.StreamingResultAggregator",
            "gpu_capability_source_audit.cpp_tokens",
            "gpu_capability_source_audit._header_operand_after_leading_comments",
        ):
            self.assertIn(expected, names)

    def test_task8_semantic_graph_owns_streaming_boundary_and_policy_growth(self):
        import gpu_capability_runner as capability_runner

        names = {
            name for name, _value in capability_audit._enumerate_live_semantic_graph()
        }
        for expected in (
            "gpu_capability_model.CanonicalAuditContentSummary",
            "gpu_capability_model.CompilerAuditRun",
            "gpu_capability_model.CompactBudgetOwnership",
            "gpu_capability_runner._CONDITIONALLY_SELECTED_TRANSLATION_UNITS",
            "gpu_capability_runner.HeldProductionSnapshot",
            "gpu_capability_runner.compute_active_sources",
            "gpu_capability_runner.decode_validated_production_sources",
            "gpu_capability_runner.run_compiler_audit_pipeline",
            "gpu_capability_runner.snapshot_production_sources",
            "gpu_capability_source_audit.premeasure_streaming_policy_growth",
        ):
            self.assertIn(expected, names)
        self.assertNotIn(
            "gpu_capability_source_audit.audit_pipeline_streaming", names
        )
        self.assertFalse(
            hasattr(capability_audit, "audit_pipeline_streaming")
        )
        self.assertNotIn("gpu_capability_runner.preprocess_all", names)
        self.assertFalse(hasattr(capability_runner, "preprocess_all"))
        self.assertEqual(
            capability_audit.AUDIT_ENGINE_STAGE_BYTES,
            b"task-8-stream-findings-coverage",
        )

    def test_task8_pipeline_measures_cache_before_memory_under_effective_deadline(self):
        import gpu_capability_runner as capability_runner

        runner_path = Path(__file__).resolve().with_name("gpu_capability_runner.py")
        source = runner_path.read_text(encoding="utf-8")
        function = source[source.index("def run_compiler_audit_pipeline("):]
        self.assertLess(
            function.index("cache.measure(effective_deadline)"),
            function.index("_accountant_memory(run_accountant)"),
        )
        self.assertIn(
            "effective_deadline = min(\n"
            "        operation_deadline, pipeline_started_at + total_seconds\n"
            "    )",
            function,
        )
        after_effective_deadline = function[
            function.index("if time.monotonic() >= effective_deadline:"):
        ]
        self.assertNotIn("operation_deadline", after_effective_deadline)
        parameters = inspect.signature(
            capability_runner.run_compiler_audit_pipeline
        ).parameters
        self.assertNotIn("production", parameters)
        self.assertNotIn("pipeline_started_at", parameters)
        self.assertLess(
            function.index("pipeline_started_at = time.monotonic()"),
            function.index("production = enumerate_production_identities("),
        )

    def test_task8_long_policy_scans_accept_and_poll_effective_deadline(self):
        runner_source = inspect.getsource(
            __import__("gpu_capability_runner")._strict_utf8_code_point_count
        )
        premeasure_source = inspect.getsource(
            capability_audit.premeasure_streaming_policy_growth
        )
        raw_source = inspect.getsource(capability_audit.audit_raw_sources)
        self.assertIn("pipeline_deadline", runner_source)
        self.assertIn("_check_deadline", runner_source)
        self.assertIn("pipeline_deadline", premeasure_source)
        self.assertIn("_check_policy_deadline", premeasure_source)
        self.assertIn("pipeline_deadline", raw_source)
        self.assertIn("_check_policy_deadline", raw_source)

    def test_task8_multibyte_utf8_scan_polls_deadline_by_byte_threshold(self):
        import gpu_capability_runner as capability_runner

        class TrackingBytes(bytes):
            last_index = -1

            def __getitem__(self, index):
                if isinstance(index, int):
                    self.last_index = index
                return super().__getitem__(index)

        raw = TrackingBytes("\u20ac".encode("utf-8") * (128 * 1024))
        calls = []

        def stop_on_second(deadline):
            calls.append((deadline, raw.last_index))
            if len(calls) == 2:
                raise AuditInfrastructureError("deadline cadence")

        with mock.patch.object(
            capability_runner.HeldProductionSnapshot,
            "_check_deadline",
            side_effect=stop_on_second,
        ), self.assertRaisesRegex(AuditInfrastructureError, "deadline cadence"):
            capability_runner._strict_utf8_code_point_count(raw, 321.0)
        self.assertEqual([deadline for deadline, _index in calls], [321.0, 321.0])
        self.assertGreaterEqual(calls[1][1], (64 * 1024) - 4)

    def test_task8_source_only_propagates_deadline_and_releases_scratch(self):
        path = PurePosixPath("playback/gpu/deadline-source-only.cpp")
        source = "int value;\n"
        observed = []

        class Workspace:
            current = 0

            def reserve_policy_scratch(self, _characters, *, source_only):
                self_case.assertTrue(source_only)
                self.current += 1
                return 1

            def release_policy_scratch(self, charge):
                self.current -= charge

        self_case = self
        workspace = Workspace()
        deadline = time.monotonic() + 1000.0

        def raw_sources(*_args, **kwargs):
            observed.append(kwargs.get("pipeline_deadline"))
            raise AuditInfrastructureError("between source-only phases")

        with mock.patch.object(
            capability_audit, "audit_raw_sources", side_effect=raw_sources
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "between source-only phases"
        ):
            capability_audit.audit_source_only(
                path,
                source,
                workspace=workspace,
                sink=lambda *_args: None,
                pipeline_deadline=deadline,
            )
        self.assertEqual(observed, [deadline])
        self.assertEqual(workspace.current, 0)

    def test_decision_graph_is_exhaustive_and_rebinding_changes_fingerprint(self):
        baseline = capability_audit.decision_engine_fingerprint(
            capability_audit.audit_engine_fingerprint())
        graph = capability_audit._enumerate_live_decision_semantic_graph(
            capability_audit.audit_engine_fingerprint())
        names = tuple(name for name, _value in graph)
        self.assertEqual(names, tuple(sorted(names)))
        for expected in (
            "gpu_capability_process_tree._query_job_member_identity",
            "gpu_capability_calibration._sample_is_eligible",
            "gpu_capability_calibration.WORKER_SELECTION_SEMANTICS_BYTES",
            "gpu_capability_calibration._WORKER_SELECTION_ORDER",
        ):
            self.assertIn(expected, names)
        original = capability_audit._gpu_capability_calibration._sample_is_eligible
        with mock.patch.object(
                capability_audit._gpu_capability_calibration,
                "_sample_is_eligible",
                lambda _sample, _platform: not original(_sample, _platform)):
            self.assertNotEqual(
                capability_audit.decision_engine_fingerprint(
                    capability_audit.audit_engine_fingerprint()), baseline)
        with mock.patch.object(
                capability_audit._gpu_capability_process_tree,
                "_query_job_member_identity",
                lambda pid, native_start_identity: (pid, native_start_identity)):
            self.assertNotEqual(
                capability_audit.decision_engine_fingerprint(
                    capability_audit.audit_engine_fingerprint()), baseline)
        with mock.patch.object(
                capability_audit._gpu_capability_calibration,
                "_WORKER_SELECTION_ORDER", (1, 2)):
            self.assertNotEqual(
                capability_audit.decision_engine_fingerprint(
                    capability_audit.audit_engine_fingerprint()), baseline)

        marshaled = tuple(
            (name, capability_audit._marshal_live_semantic_object(loaded_object))
            for name, loaded_object in graph
        )
        framed_baseline = capability_audit._audit_engine_fingerprint_from_marshaled_graph(
            marshaled
        )
        for index, (name, payload) in enumerate(marshaled):
            mutated = marshaled[:index] + ((name, payload + b"semantic-mutation"),) + marshaled[index + 1:]
            with self.subTest(framed_component=name):
                self.assertNotEqual(
                    capability_audit._audit_engine_fingerprint_from_marshaled_graph(mutated),
                    framed_baseline,
                )

        baseline = capability_audit.audit_engine_fingerprint()

        def mutated_cpp_tokens(*_args, **_kwargs):
            return []

        with mock.patch.object(capability_audit, "cpp_tokens", mutated_cpp_tokens):
            self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)
        self.assertEqual(capability_audit.audit_engine_fingerprint(), baseline)

    def test_transitive_helper_global_and_policy_table_rebinding_changes_digest(self):
        baseline = capability_audit.audit_engine_fingerprint()

        def mutated_helper(_tail):
            return "mutated"

        mutations = (
            (capability_audit, "_header_operand_after_leading_comments", mutated_helper),
            (
                capability_audit,
                "_RAW_DIRECTIVES",
                frozenset((*capability_audit._RAW_DIRECTIVES, "mutated")),
            ),
            (capability_model, "AUDIT_RESULT_SCHEMA_BYTES", b"different-schema"),
        )
        for owner, name, replacement in mutations:
            with self.subTest(name=name), mock.patch.object(owner, name, replacement):
                self.assertNotEqual(capability_audit.audit_engine_fingerprint(), baseline)

    def test_task1_policy_globals_are_deeply_immutable(self):
        self.assertIsInstance(capability_audit.SOURCE_SUFFIXES, frozenset)
        tables = (
            capability_audit.REVIEWED_NATIVE_HANDLE_SINKS,
            capability_audit.REVIEWED_NATIVE_HANDLE_METHODS,
            capability_audit.REVIEWED_NATIVE_HANDLE_MEMBER_SINKS,
            capability_audit.REVIEWED_NATIVE_HANDLE_TYPES,
        )
        for table in tables:
            self.assertIsInstance(table, MappingProxyType)
            with self.assertRaises(TypeError):
                table[PurePosixPath("replacement.cpp")] = frozenset({"sink"})
            self.assertTrue(all(isinstance(value, frozenset) for value in table.values()))

        with mock.patch.object(capability_audit, "SOURCE_SUFFIXES", {".cpp"}):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "unsupported loaded semantic object"
            ):
                capability_audit.audit_engine_fingerprint()

    def test_runtime_state_exclusions_are_exact_existing_and_nonsemantic(self):
        self.assertEqual(
            tuple(
                module.__name__
                for module in capability_audit._AUDIT_ENGINE_TARGET_MODULES
            ),
            (
                "gpu_capability_model",
                "gpu_capability_source_audit",
                "gpu_capability_command",
                "gpu_capability_cache",
                "gpu_capability_provenance",
                "gpu_capability_runner",
            ),
        )
        target_modules = {
            module.__name__: module
            for module in capability_audit._AUDIT_ENGINE_TARGET_MODULES
        }
        exclusions = capability_audit._AUDIT_RUNTIME_STATE_EXCLUSIONS
        self.assertEqual(set(exclusions), set(target_modules))
        for module_name, excluded_names in exclusions.items():
            module = target_modules[module_name]
            for name in excluded_names:
                with self.subTest(module=module_name, name=name):
                    self.assertTrue(hasattr(module, name))
                    value = getattr(module, name)
                    self.assertFalse(
                        isinstance(value, (type, type(lambda: None)))
                        and getattr(value, "__module__", None) == module_name
                    )
                    self.assertFalse(capability_audit._is_semantic_constant_name(name))

        invalid_exclusions = MappingProxyType({
            **exclusions,
            "gpu_capability_source_audit": frozenset({"_RUNTIME_OBSERVER_LOCK"}),
        })
        with self.assertRaisesRegex(AuditInfrastructureError, "runtime-state exclusion"):
            capability_audit._walk_live_semantic_graph_cycle_safe(
                target_modules=capability_audit._AUDIT_ENGINE_TARGET_MODULES,
                runtime_state_exclusions=invalid_exclusions,
            )

        semantic_exclusions = MappingProxyType({
            **exclusions,
            "gpu_capability_source_audit": frozenset({"cpp_tokens"}),
        })
        with self.assertRaisesRegex(AuditInfrastructureError, "semantic symbol"):
            capability_audit._walk_live_semantic_graph_cycle_safe(
                target_modules=capability_audit._AUDIT_ENGINE_TARGET_MODULES,
                runtime_state_exclusions=semantic_exclusions,
            )

    def test_recomputation_does_not_read_the_filesystem(self):
        source_directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as temporary:
            imported_root = Path(temporary) / "gpu"
            imported_root.mkdir()
            module_files = (
                "gpu_capability_model.py",
                "gpu_capability_source_audit.py",
                "gpu_capability_command.py",
                "gpu_capability_cache.py",
                "gpu_capability_provenance.py",
                "gpu_capability_runner.py",
                "gpu_capability_process_tree.py",
                "gpu_capability_calibration.py",
            )
            for name in module_files:
                shutil.copyfile(source_directory / name, imported_root / name)

            script = r'''
import builtins
import io
import os
import sys
from pathlib import Path
from unittest import mock

root = Path(sys.argv[1])
sys.path.insert(0, str(root))
import gpu_capability_source_audit as audit

baseline = audit.audit_engine_fingerprint()
captured = audit._IMPORTED_MODULE_IDENTITIES
for name in {module_files!r}:
    original = root / name
    os.replace(original, original.with_suffix(".loaded"))
    original.write_bytes(b"replacement module bytes")

denied = AssertionError("fingerprint recomputation read the filesystem")
with (
    mock.patch.object(builtins, "open", side_effect=denied),
    mock.patch.object(io, "open", side_effect=denied),
    mock.patch.object(os, "open", side_effect=denied),
    mock.patch.object(os, "stat", side_effect=denied),
    mock.patch.object(os, "lstat", side_effect=denied),
    mock.patch.object(os, "scandir", side_effect=denied),
    mock.patch.object(Path, "open", side_effect=denied),
    mock.patch.object(Path, "read_bytes", side_effect=denied),
    mock.patch.object(Path, "read_text", side_effect=denied),
    mock.patch.object(Path, "resolve", side_effect=denied),
    mock.patch.object(Path, "stat", side_effect=denied),
    mock.patch.object(Path, "lstat", side_effect=denied),
):
    recomputed = audit.audit_engine_fingerprint()

if recomputed != baseline:
    raise AssertionError("path replacement changed loaded semantics")
if audit._IMPORTED_MODULE_IDENTITIES != captured:
    raise AssertionError("captured loaded identities changed")
print(recomputed)
'''.format(module_files=module_files)
            completed = subprocess.run(
                (sys.executable, "-c", script, str(imported_root)),
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertRegex(completed.stdout.strip(), r"\A[0-9a-f]{64}\Z")

    def test_audit_fingerprint_is_portable_across_absolute_import_roots(self):
        source_directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first" / "gpu"
            second = root / "second" / "gpu"
            first.mkdir(parents=True)
            second.mkdir(parents=True)
            module_files = (
                "gpu_capability_model.py",
                "gpu_capability_source_audit.py",
                "gpu_capability_command.py",
                "gpu_capability_cache.py",
                "gpu_capability_provenance.py",
                "gpu_capability_runner.py",
            )
            for name in module_files:
                shutil.copyfile(source_directory / name, first / name)
                shutil.copyfile(source_directory / name, second / name)

            script = (
                "import sys; sys.path[:0] = [sys.argv[1], sys.argv[2]]; "
                "import gpu_capability_source_audit as a; print(a.audit_engine_fingerprint())"
            )

            def fingerprint(path: Path) -> str:
                completed = subprocess.run(
                    (sys.executable, "-c", script, str(path), str(source_directory)),
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                return completed.stdout.strip()

            self.assertEqual(fingerprint(first), fingerprint(second))
            mutated = (second / "gpu_capability_source_audit.py").read_text(encoding="utf-8")
            self.assertIn(
                'AUDIT_ENGINE_STAGE_BYTES = b"task-8-stream-findings-coverage"',
                mutated,
            )
            (second / "gpu_capability_source_audit.py").write_text(
                mutated.replace(
                    'AUDIT_ENGINE_STAGE_BYTES = b"task-8-stream-findings-coverage"',
                    'AUDIT_ENGINE_STAGE_BYTES = b"task-8-stream-findings-coverage-mutated"',
                    1,
                ),
                encoding="utf-8",
            )
            self.assertNotEqual(fingerprint(first), fingerprint(second))

    def test_decision_fingerprint_is_portable_across_absolute_import_roots(self):
        source_directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first" / "gpu"
            second = root / "second" / "gpu"
            first.mkdir(parents=True)
            second.mkdir(parents=True)
            module_files = (
                "gpu_capability_model.py",
                "gpu_capability_source_audit.py",
                "gpu_capability_command.py",
                "gpu_capability_cache.py",
                "gpu_capability_provenance.py",
                "gpu_capability_runner.py",
                "gpu_capability_process_tree.py",
                "gpu_capability_calibration.py",
            )
            for name in module_files:
                shutil.copyfile(source_directory / name, first / name)
                shutil.copyfile(source_directory / name, second / name)

            script = (
                "import sys; sys.path[:0] = [sys.argv[1], sys.argv[2]]; "
                "import gpu_capability_source_audit as a; "
                "print(a.decision_engine_fingerprint(a.audit_engine_fingerprint()))"
            )

            def fingerprint(path: Path) -> str:
                completed = subprocess.run(
                    (sys.executable, "-c", script, str(path), str(source_directory)),
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                return completed.stdout.strip()

            self.assertEqual(fingerprint(first), fingerprint(second))
            mutated = (second / "gpu_capability_calibration.py").read_text(
                encoding="utf-8")
            self.assertIn("_WORKER_SELECTION_ORDER = (2, 1)", mutated)
            (second / "gpu_capability_calibration.py").write_text(
                mutated.replace(
                    "_WORKER_SELECTION_ORDER = (2, 1)",
                    "_WORKER_SELECTION_ORDER = (1, 2)",
                    1,
                ),
                encoding="utf-8",
            )
            self.assertNotEqual(fingerprint(first), fingerprint(second))

    def test_task1_fingerprint_no_longer_attests_after_staged_evolution(self):
        current = capability_audit.audit_engine_fingerprint()
        with (
            mock.patch.object(
                capability_audit,
                "AUDIT_ENGINE_GRAPH_SCHEMA_BYTES",
                b"olr-gpu-capability-live-graph-v1",
            ),
            mock.patch.object(
                capability_audit,
                "AUDIT_ENGINE_STAGE_BYTES",
                b"task-1-model-and-source-audit",
            ),
        ):
            task1 = capability_audit.audit_engine_fingerprint()
        self.assertNotEqual(task1, current)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "loaded audit engine attestation"
        ):
            capability_audit._attest_loaded_audit_engine(task1)

    def test_windows_relative_forward_slash_import_attests_loaded_engine(self):
        source_directory = Path(__file__).resolve().parent
        repository_root = source_directory.parents[1]
        script = (
            "import sys; sys.path.insert(0,'tests/gpu'); "
            "import gpu_capability_source_audit as a; "
            "print(a.audit_engine_fingerprint())"
        )
        completed = subprocess.run(
            (sys.executable, "-c", script),
            cwd=repository_root,
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertRegex(completed.stdout.strip(), r"\A[0-9a-f]{64}\Z")

    def test_parent_recomputes_attestation_and_rejects_caller_mismatch(self):
        actual = capability_audit.audit_engine_fingerprint()
        self.assertEqual(capability_audit._attest_loaded_audit_engine(actual), actual)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "loaded audit engine attestation"
        ):
            capability_audit._attest_loaded_audit_engine("0" * 64)

        def mutated_cpp_tokens(*_args, **_kwargs):
            return []

        with mock.patch.object(capability_audit, "cpp_tokens", mutated_cpp_tokens):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "loaded audit engine attestation"
            ):
                capability_audit._attest_loaded_audit_engine(actual)


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
        executable = FileIdentity(
            Path("C:/toolchain/g++.exe"), None, 3, 9001, 0, False
        )
        root = DependencyRootBinding(
            "toolchain", Path("C:/toolchain"),
            FileIdentity(Path("C:/toolchain"), None, 3, 9002, 0, False),
        )
        capability = CompilerExecutableCapability(
            "windows", executable, "1" * 64, "2" * 64, object(), root,
            (), (), "3" * 64, (),
        )
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=CompilerFamily.GCC,
            compiler=Path("C:/toolchain/g++.exe"),
            working_directory=Path("D:/repo/build"),
            source=source,
            arguments=(str(source.canonical),),
            environment_digest="environment-a",
            digest=digest,
            dependency_root_authority_digest="a" * 64,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
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

    def test_candidate_prefilter_does_not_build_audit_buffer(self):
        view = self.view((
            self.production("playback/gpu/example.cpp", 1, b"int safe ;"),
        ))
        self.assertFalse(view_has_capability_spelling(view))
        with mock.patch.object(AuditBuffer, "from_preprocessed") as build:
            self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])
        build.assert_not_called()

    def test_profile_uses_authenticated_stabilized_runner(self):
        relative = PurePosixPath("playback/gpu/profile.cpp")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            database = root / "compile_commands.json"
            database.write_text("[]", encoding="utf-8")
            source_path = root / Path(*relative.parts)
            source_path.parent.mkdir(parents=True)
            source_path.write_text("int profile;\n", encoding="utf-8")
            identity = self.identity(relative.as_posix(), canonical=str(source_path))
            configuration = self.configuration(identity)
            view = self.view(((identity, 1, 1, b"int profile ;"),))
            authority = object()
            with (
                mock.patch.object(
                    capability_audit,
                    "build_dependency_root_authority",
                    return_value=authority,
                ),
                mock.patch.object(
                    capability_audit._gpu_capability_runner,
                    "collect_configurations",
                    return_value=(configuration,),
                ),
                mock.patch.object(
                    capability_audit,
                    "enumerate_production_identities",
                    autospec=True,
                    return_value={relative: identity},
                ) as enumerate_sources,
                mock.patch.object(
                    capability_audit._gpu_capability_runner,
                    "stabilize_and_parse_configuration",
                    return_value=(view, object(), object()),
                ) as stabilize,
                mock.patch.object(
                    capability_audit._gpu_capability_runner.subprocess,
                    "run",
                    side_effect=AssertionError("direct subprocess reached"),
                ),
                mock.patch.object(
                    capability_audit,
                    "audit_preprocessed_view",
                    return_value=[],
                ),
            ):
                result = capability_audit.profile_compiler_view(
                    root, database, relative
                )
        self.assertIn("configurations=1", result)
        stabilize.assert_called_once()
        self.assertEqual(stabilize.call_args.args[3].invocation_seconds, 300.0)
        self.assertEqual(stabilize.call_args.args[3].total_seconds, 600.0)
        enumerate_sources.assert_called_once_with(
            root, stabilize.call_args.args[3], stabilize.call_args.args[4]
        )

    def test_candidate_paths_exclude_production_paths_without_rule_trigger(self):
        view = self.view((
            self.production("playback/gpu/example.cpp", 1, b"int safe ;"),
            self.production(
                "playback/gpu/gpufence.h", 20, b"surface . nativeHandle ( ) ;"
            ),
        ))
        buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        self.assertEqual(
            buffer.candidate_paths(),
            (PurePosixPath("playback/gpu/gpufence.h"),),
        )

    def test_capability_candidates_are_mechanically_derived_from_rules(self):
        expected = frozenset(
            spelling
            for rule in capability_audit._CAPABILITY_RULES
            for spelling in rule.trigger_spellings
        )
        self.assertEqual(capability_candidate_spellings(), expected)
        self.assertNotIn("_CAPABILITY_" + "CANDIDATE_SPELLINGS", capability_audit.__dict__)

    def test_batched_buffer_is_byte_and_location_identical(self):
        view = self.view((
            self.external(b"namespace sdk {", line=2, inclusion=3),
            self.production(
                "playback/gpu/gpufence.h",
                71,
                b"lease.nativeHandle();",
                inclusion=9,
            ),
            self.external(b"}", line=4, inclusion=3),
        ))
        buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        self.assertEqual(
            buffer.text,
            "namespace sdk {\nlease . nativeHandle ( ) ;\n}\n",
        )
        location = buffer.location_for_line(2)
        self.assertEqual(location.identity.relative, PurePosixPath("playback/gpu/gpufence.h"))
        self.assertEqual(location.inclusion_instance, 9)
        self.assertEqual(location.line, 71)

    def test_unused_high_byte_spelling_does_not_change_measured_storage_kind(self):
        identity = self.identity("playback/gpu/example.cpp")
        configuration = self.configuration(identity)
        tokens = CompactTokenSequence._from_packed(
            configuration,
            spellings=(b"x", b"\xff"),
            identities=(None, identity),
            spelling_ids=array("I", (0,)),
            identity_ids=array("I", (1,)),
            inclusion_ids=array("I", (1,)),
            original_lines=array("I", (1,)),
        )
        view = PreprocessedTranslationUnitView(configuration, tokens, (identity,))
        buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        self.assertEqual(buffer.text, "x\n")
        self.assertEqual(buffer.text_max_code_point, ord("x"))

    def test_analysis_plan_covers_scope_shaped_peak_allocations(self):
        identity = self.identity("playback/gpu/example.cpp")
        statement = (
            b"GpuSyncReadScope scopeIdentifierWithLongPadding { } "
        )
        source = statement * 12_501
        view = self.view(((identity, 1, 1, source),))
        buffer = AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        shape = capability_audit._measure_audit_buffer_layout(view.tokens).shape
        plan = audit_allocation_plan(shape)
        translated = capability_audit.TranslationText(
            buffer.text,
            buffer.text,
            buffer.text,
            (),
            (),
            buffer._line_starts,
        )
        tracemalloc.start()
        try:
            analysis = CompilerAuditAnalysis.from_translation(translated)
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(len(analysis.tokens), 50_004)
        self.assertGreaterEqual(plan.analysis_objects, peak)

    def test_analysis_plan_covers_native_handle_member_match_allocations(self):
        identity = self.identity("playback/gpu/example.cpp")
        source = b"surface . nativeHandle ( ) ; " * 50_000
        view = self.view(((identity, 1, 1, source),))
        limits = dataclasses.replace(self.limits, rss_bytes=1024 * 1024 * 1024)
        buffer = AuditBuffer.from_preprocessed(view, limits, lambda: 0)
        shape = capability_audit._measure_audit_buffer_layout(view.tokens).shape
        plan = audit_allocation_plan(shape)
        translated = capability_audit.TranslationText(
            buffer.text,
            buffer.text,
            buffer.text,
            (),
            (),
            buffer._line_starts,
        )
        tracemalloc.start()
        try:
            analysis = CompilerAuditAnalysis.from_translation(translated)
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(shape.member_call_count, 50_000)
        self.assertFalse(shape.scope_analysis_required)
        self.assertEqual(len(analysis.member_calls), 50_000)
        self.assertGreaterEqual(plan.analysis_objects, peak)

    def test_filtered_and_forced_unfiltered_results_match_owned_corpus(self):
        fixtures = (
            ("empty", self.view((self.production("playback/gpu/empty.cpp", 1, b""),))),
            ("candidate-free", self.view((self.production(
                "playback/gpu/example.cpp", 1, b"int value = 7 ;"
            ),))),
            ("native-handle", self.view((self.production(
                "playback/gpu/gpufence.h", 20, b"surface.nativeHandle();"
            ),))),
            ("nonlocal-jump", self.view((self.production(
                "playback/gpu/gpufence.h", 30,
                b"GpuSyncReadScope scope; scope.withRead(s, [](auto lease) { longjmp(e, 1); });"
            ),))),
            ("public-registry", self.view((self.production(
                str(REGISTRY_HEADER), 40,
                b"class GpuRetireRegistry { public: void registerRetire(); };"
            ),))),
            ("public-op-scope", self.view((self.production(
                str(OP_SCOPE_HEADER), 50,
                b"class GpuOpScope { public: void track(); };"
            ),))),
            ("literal-only", self.view((self.production(
                "playback/gpu/example.cpp", 60, b"const char * text = safe ;"
            ),))),
        )
        derived = capability_candidate_spellings()
        for name, view in fixtures:
            filtered = audit_preprocessed_view(view, self.limits, lambda: 0)
            unfiltered = _audit_preprocessed_view_unfiltered(
                view, self.limits, lambda: 0
            )
            with self.subTest(name=name):
                self.assertEqual(filtered, unfiltered)
                if unfiltered:
                    spellings = set(object.__getattribute__(view.tokens, "_spellings"))
                    self.assertFalse(derived.isdisjoint(spellings))

    def test_prefilter_matches_forced_oracle_over_mutation_and_live_corpora(self):
        captured: list[tuple[str, str, PurePosixPath, str]] = []
        original_capability = capability_audit.audit_capability_uses
        original_source_only = capability_audit.audit_source_only
        original_raw = capability_audit.audit_raw_sources

        def record_capability(path, source, *args, **kwargs):
            if not kwargs.get("compiler_view", False):
                captured.append((
                    "mutation-authoritative",
                    f"authoritative-{sum(item[0] == 'mutation-authoritative' for item in captured):03d}",
                    path,
                    source,
                ))
            return original_capability(path, source, *args, **kwargs)

        def record_source_only(path, source, *args, **kwargs):
            captured.append((
                "mutation-source-only",
                f"source-only-{sum(item[0] == 'mutation-source-only' for item in captured):03d}",
                path,
                source,
            ))
            return original_source_only(path, source, *args, **kwargs)

        def record_raw(sources, *args, **kwargs):
            for path, source in sources.items():
                captured.append((
                    "mutation-raw",
                    f"raw-{sum(item[0] == 'mutation-raw' for item in captured):03d}",
                    path,
                    source,
                ))
            return original_raw(sources, *args, **kwargs)

        with (
            mock.patch.object(
                capability_audit,
                "audit_capability_uses",
                side_effect=record_capability,
            ),
            mock.patch.object(
                capability_audit,
                "audit_source_only",
                side_effect=record_source_only,
            ),
            mock.patch.object(
                capability_audit,
                "audit_raw_sources",
                side_effect=record_raw,
            ),
        ):
            capability_audit.mutation_self_tests()

        from test_gpu_capability_live_compilers import (
            FIXTURE_PATH,
            FORBIDDEN_OUTPUT_ORACLES,
            SAFE_FIXTURES,
        )

        for name, output in FORBIDDEN_OUTPUT_ORACLES.items():
            captured.append((
                "live-forbidden", name, FIXTURE_PATH,
                output.decode("ascii") + ";",
            ))
        for name, (_source, output) in SAFE_FIXTURES.items():
            captured.append((
                "live-safe", name, FIXTURE_PATH,
                output.decode("ascii") + ";",
            ))

        expected_counts = {
            "mutation-authoritative": 181,
            "mutation-source-only": 31,
            "mutation-raw": 33,
            "live-forbidden": 6,
            "live-safe": 7,
        }

        def require_exact_inventory(entries):
            observed_counts = {
                category: sum(item[0] == category for item in entries)
                for category in expected_counts
            }
            self.assertEqual(observed_counts, expected_counts)
            self.assertEqual(len(entries), 258)
            self.assertEqual(len({(item[2], item[3]) for item in entries}), 186)
            self.assertEqual(
                {item[1] for item in entries if item[0] == "live-forbidden"},
                set(FORBIDDEN_OUTPUT_ORACLES),
            )
            self.assertEqual(
                {item[1] for item in entries if item[0] == "live-safe"},
                set(SAFE_FIXTURES),
            )

        require_exact_inventory(captured)
        with self.assertRaises(AssertionError):
            require_exact_inventory(captured[:-1])

        derived = capability_candidate_spellings()
        for index, (category, name, path, source) in enumerate(captured):
            try:
                encoded = source.encode("ascii")
            except UnicodeEncodeError as error:
                self.fail(f"owned corpus source is not ASCII: {path}: {error}")
            view = self.view(((self.identity(path.as_posix()), 1, index + 1, encoded),))
            filtered = audit_preprocessed_view(view, self.limits, lambda: 0)
            forced = _audit_preprocessed_view_unfiltered(
                view, self.limits, lambda: 0
            )
            with self.subTest(
                category=category, name=name, path=path, corpus_index=index
            ):
                self.assertEqual(filtered, forced)
                if forced:
                    spellings = set(
                        object.__getattribute__(view.tokens, "_spellings")
                    )
                    self.assertFalse(derived.isdisjoint(spellings))

    def test_forced_unfiltered_reference_bypasses_candidate_spellings(self):
        view = self.view((self.production(
            "playback/gpu/gpufence.h", 20, b"surface.nativeHandle();"
        ),))
        with mock.patch.object(
            capability_audit,
            "capability_candidate_spellings",
            return_value=frozenset(),
        ):
            self.assertEqual(
                audit_preprocessed_view(view, self.limits, lambda: 0), []
            )
            forced = _audit_preprocessed_view_unfiltered(
                view, self.limits, lambda: 0
            )
        self.assertTrue(forced)
        self.assertIn("nativeHandle()", forced[0].expression)

    def test_capability_rule_evaluator_is_the_dispatched_owner(self):
        view = self.view((self.production(
            "playback/gpu/gpufence.h", 20, b"surface.nativeHandle();"
        ),))
        owner = mock.Mock(wraps=capability_audit.audit_capability_uses)
        rules = (
            dataclasses.replace(capability_audit._CAPABILITY_RULES[0], evaluator=owner),
            *capability_audit._CAPABILITY_RULES[1:],
        )
        with mock.patch.object(capability_audit, "_CAPABILITY_RULES", rules):
            _audit_preprocessed_view_unfiltered(view, self.limits, lambda: 0)
        self.assertGreater(owner.call_count, 0)

    def test_one_compiler_analysis_is_reused_across_policy_groups(self):
        view = self.view((
            self.production(
                "playback/gpu/gpufence.h", 70,
                b"void bad() { GpuSyncReadScope scope; surface.nativeHandle(); }",
            ),
            self.production(
                "playback/output/win/wingpuimportedge.cpp", 80,
                b"void bad2() { GpuSyncReadScope other; surface.nativeHandle(); }",
                inclusion=2,
            ),
        ))
        original_tokens = capability_audit.cpp_tokens
        original_shadow = capability_audit.build_shadow_index
        with (
            mock.patch.object(
                capability_audit, "cpp_tokens", wraps=original_tokens
            ) as token_calls,
            mock.patch.object(
                capability_audit, "build_shadow_index", wraps=original_shadow
            ) as shadow_calls,
        ):
            audit_preprocessed_view(view, self.limits, lambda: 0)
        self.assertEqual(token_calls.call_count, 1)
        self.assertEqual(shadow_calls.call_count, 1)

    def test_native_handle_only_analysis_skips_scope_and_shadow_indices(self):
        view = self.view((self.production(
            "playback/gpu/gpusurface.h",
            70,
            b"class GpuSurface { void * nativeHandle ( ) const ; } ;",
        ),))
        with (
            mock.patch.object(
                capability_audit,
                "scope_bindings_linear",
                side_effect=AssertionError("scope bindings built"),
            ),
            mock.patch.object(
                capability_audit,
                "build_shadow_index",
                side_effect=AssertionError("shadow index built"),
            ),
        ):
            self.assertEqual(
                audit_preprocessed_view(view, self.limits, lambda: 0), []
            )

    def test_portable_allocation_schema_has_explicit_checked_formulas(self):
        schema = conservative_allocation_schema()
        self.assertEqual(schema.schema_version, 1)
        self.assertEqual(schema.string_bound(7), schema.round_up(
            schema.str_header_bytes + 8 * 4
        ))
        self.assertEqual(schema.bytes_bound(7), schema.round_up(
            schema.bytes_header_bytes + 8
        ))
        self.assertEqual(schema.bytearray_bound(7), schema.round_up(
            schema.bytearray_header_bytes + 8
        ))
        digits = max(1, ((4096).bit_length() + schema.pylong_digit_bits - 1)
                     // schema.pylong_digit_bits)
        self.assertEqual(schema.pylong_bound(4096), schema.round_up(
            schema.pylong_header_bytes
            + digits * schema.pylong_digit_bytes_upper_bound
        ))
        self.assertGreater(schema.dict_bound(1366), schema.dict_bound(1365))
        with self.assertRaisesRegex(AuditInfrastructureError, "allocation arithmetic"):
            schema.checked_add(schema.maximum_allocation_bytes, 1)
        with self.assertRaisesRegex(AuditInfrastructureError, "allocation arithmetic"):
            schema.checked_multiply(schema.maximum_allocation_bytes, 2)

    def test_allocation_plan_charges_every_declared_phase_overlap(self):
        schema = conservative_allocation_schema()
        shape = AuditAllocationShape(
            text_character_count=131_073,
            maximum_code_point=255,
            run_count=19,
            origin_count=5,
            chunk_count=3,
            token_count=40,
            json_input_chunk_bytes=4097,
            json_input_chunk_count=2,
            json_token_count=13,
            json_string_characters=257,
            json_integer_values=(1 << 200,),
            json_list_slots=11,
            json_tuple_slots=7,
            json_dict_entries=17,
            duplicate_key_count=17,
            production_raw_bytes=8193,
        )
        plan = audit_allocation_plan(shape, schema)
        self.assertEqual(plan.pre_reserved_bytes, sum(plan.phase_charges))
        for field in (
            "input_chunks_and_containers",
            "joined_input_and_join_transient",
            "final_text",
            "mapping_arrays",
            "duplicate_key_structures",
            "analysis_objects",
            "json_scanner_decoder",
            "production_decode_mapping",
        ):
            with self.subTest(field=field):
                self.assertGreater(getattr(plan, field), 0)
        reservation = reserve_before_allocation(plan.pre_reserved_bytes)
        reservation.require_before_allocation(plan.pre_reserved_bytes)
        with self.assertRaisesRegex(AuditInfrastructureError, "pre-reserved allocation"):
            reservation.require_before_allocation(plan.pre_reserved_bytes + 1)

    def test_dense_view_reserves_chunks_mapping_join_and_final_text_first(self):
        view = self.one_million_token_view()
        observations = capability_audit.AuditAllocationObservations()
        buffer = AuditBuffer.from_preprocessed(
            view, self.limits, lambda: 0, _allocation_observer=observations
        )
        self.assertIs(type(buffer.text), str)
        self.assertEqual(buffer.text_character_count, len(buffer.text))
        self.assertEqual(
            buffer.text_max_code_point, max(map(ord, buffer.text), default=0)
        )
        self.assertEqual(
            observations.events[:2],
            ["measure-layout", "reserve-all-allocations"],
        )
        for slot in (
            observations.final_str,
            observations.chunk_list,
            observations.join_transient,
            observations.mapping_arrays,
        ):
            self.assertTrue(slot.reserved_before_allocation)
        self.assertLessEqual(observations.maximum_chunk_characters, 64 * 1024)
        self.assertEqual(observations.final_str.constructions, 1)
        self.assertLessEqual(
            observations.peak_charged_bytes, observations.pre_reserved_bytes
        )

    def test_analysis_charge_is_committed_before_final_text_allocation(self):
        view = self.view((self.production(
            "playback/gpu/gpufence.h",
            1,
            b"GpuSyncReadScope scope ; scope . complete ( ) ;",
        ),))
        shape = capability_audit._measure_audit_buffer_layout(view.tokens).shape
        plan = audit_allocation_plan(shape)
        self.assertGreater(plan.analysis_objects, 0)
        observations = capability_audit.AuditAllocationObservations()
        AuditBuffer.from_preprocessed(
            view, self.limits, lambda: 0, _allocation_observer=observations
        )
        self.assertEqual(observations.pre_reserved_bytes, plan.pre_reserved_bytes)
        self.assertEqual(
            observations.events[:2],
            ["measure-layout", "reserve-all-allocations"],
        )
        self.assertTrue(observations.final_str.reserved_before_allocation)

    def test_measurement_scratch_is_reserved_before_layout_measurement(self):
        view = self.view((self.production(
            "playback/gpu/example.cpp", 1, b"surface . nativeHandle ( ) ;"
        ),))
        reservations = []
        original_reserve = capability_audit.reserve_before_allocation
        original_measure = capability_audit._measure_audit_buffer_layout

        def record_reserve(amount):
            reservations.append(amount)
            return original_reserve(amount)

        def guarded_measure(tokens):
            self.assertTrue(reservations, "layout measurement preceded reservation")
            return original_measure(tokens)

        with (
            mock.patch.object(
                capability_audit,
                "reserve_before_allocation",
                side_effect=record_reserve,
            ),
            mock.patch.object(
                capability_audit,
                "_measure_audit_buffer_layout",
                side_effect=guarded_measure,
            ),
        ):
            AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)
        self.assertEqual(
            reservations[0], capability_audit.audit_measurement_scratch_bound()
        )
        tracemalloc.start()
        try:
            measured = original_measure(view.tokens)
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertGreater(measured.shape.token_count, 0)
        self.assertLessEqual(
            peak, capability_audit.audit_measurement_scratch_bound()
        )

    def test_production_buffer_never_uses_private_cpython_probe(self):
        view = self.view((self.production(
            "playback/gpu/example.cpp", 1, b"GpuSyncReadScope scope ;"
        ),))
        with mock.patch.object(
            capability_audit,
            "probe_cpython_allocation_layout",
            side_effect=AssertionError("diagnostic probe selected production bounds"),
        ):
            AuditBuffer.from_preprocessed(view, self.limits, lambda: 0)

    @unittest.skipUnless(
        sys.version_info[:3] == (3, 11, 9),
        "private-layout calibration is pinned to CPython 3.11.9",
    )
    def test_runtime_allocation_probe_matches_supported_cpython_3119_objects(self):
        layout = probe_cpython_allocation_layout()
        self.assertEqual(layout.python_version, (3, 11, 9))
        for value, reserved in layout.representative_objects_and_bounds:
            with self.subTest(type=type(value), value=repr(value)[:40]):
                self.assertLessEqual(sys.getsizeof(value), reserved)

    def test_runtime_allocation_probe_rejects_other_versions(self):
        with mock.patch.object(capability_audit.sys, "version_info", (3, 11, 8)):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "CPython 3.11.9 allocation layout"
            ):
                probe_cpython_allocation_layout()

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
        plan = audit_allocation_plan(
            capability_audit._measure_audit_buffer_layout(view.tokens).shape
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "coordinator RSS limit"):
            AuditBuffer.from_preprocessed(
                view,
                dataclasses.replace(
                    self.limits, rss_bytes=50_000 + plan.pre_reserved_bytes
                ),
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

    def test_task8_streaming_pipeline_is_order_independent_and_equivalent(self):
        first = capability_model.ConfigurationAuditResult(
            "b" * 64,
            capability_audit.audit_engine_fingerprint(),
            (),
            (PurePosixPath("playback/gpu/b.cpp"),),
            (capability_model.AuditResultFinding(
                self.finding(line=30).path,
                30,
                self.finding().expression,
                self.finding().reason,
            ),),
        )
        second = dataclasses.replace(
            first,
            configuration_digest="a" * 64,
            reached_production=(PurePosixPath("playback/gpu/a.cpp"),),
            findings=(capability_model.AuditResultFinding(
                self.finding(line=2).path,
                2,
                self.finding().expression,
                self.finding().reason,
            ),),
        )
        sources = {
            PurePosixPath("playback/gpu/a.cpp"): "int a;\n",
            PurePosixPath("playback/gpu/b.cpp"): "int b;\n",
            self.finding().path: "int h;\n",
        }
        expected_coverage = CoverageReport(
            authoritative=frozenset((
                PurePosixPath("playback/gpu/a.cpp"),
                PurePosixPath("playback/gpu/b.cpp"),
            )),
            source_only=frozenset((self.finding().path,)),
            configurations=("a" * 64, "b" * 64),
        )
        ordered = _reference_audit_pipeline_streaming(sources, (first, second))
        reversed_order = _reference_audit_pipeline_streaming(
            sources, (second, first)
        )
        self.assertEqual(ordered, reversed_order)
        self.assertEqual(ordered[1], expected_coverage)
        self.assertEqual(
            [item.finding.line for item in ordered[0]], [2, 30]
        )

    def test_task8_policy_premeasure_charges_full_provenance_fanout(self):
        sources = {
            PurePosixPath("playback/gpu/a.cpp"): "int value;\n"
        }
        base = premeasure_streaming_policy_growth(
            sources, 1, configuration_provenance_count=1
        )
        fanned_out = premeasure_streaming_policy_growth(
            sources, 1, configuration_provenance_count=251
        )
        self.assertGreater(fanned_out - base, 250 * 64)

    def test_task8_policy_premeasure_counts_every_finding_lane(self):
        path = PurePosixPath("playback/gpu/every-lane.cpp")
        cases = {
            "directive": "#else\n" * 100,
            "phase-two": ("native\\\nHandle();\n" * 100),
            "capability": "surface.nativeHandle();\n" * 100,
            "macro-ambiguity": "GPU_SURFACE_ALIAS\n" * 100,
            "retire-registry": "public: void registerRetire();\n" * 100,
            "op-scope": "public: void track();\n" * 100,
        }
        for label, source in cases.items():
            with self.subTest(label=label):
                quiet = "x" * len(source)
                self.assertGreater(
                    premeasure_streaming_policy_growth({path: source}, 0),
                    premeasure_streaming_policy_growth({path: quiet}, 0),
                )

    def test_task8_directive_fanout_premeasure_exceeds_compact_boundary(self):
        path = PurePosixPath("playback/gpu/directive-boundary.cpp")
        required = premeasure_streaming_policy_growth(
            {path: "#else\n" * 30000}, 0
        )
        self.assertGreater(required, 128 << 20)
        smaller = premeasure_streaming_policy_growth(
            {path: "#else\n" * 100}, 0
        )
        exact = capability_model.CompactResultMemoryBudget(
            maximum_bytes=smaller
        )
        ownership = exact.reserve(smaller, label="final policy aggregate growth")
        ownership.release()
        one_less = capability_model.CompactResultMemoryBudget(
            maximum_bytes=smaller - 1
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "aggregate 128 MiB"):
            one_less.reserve(smaller, label="final policy aggregate growth")

    def test_task8_special_public_member_fanout_exceeds_compact_boundary(self):
        cases = {
            PurePosixPath("playback/gpu/gpuretireregistry.h"): (
                "class GpuRetireRegistry { public:\n"
                + "void registerRetire();\n" * 7750
                + "};\n"
            ),
            PurePosixPath("playback/gpu/gpuopscope.h"): (
                "class GpuOpScope { public:\n"
                + "void track();\n" * 10000
                + "};\n"
            ),
        }
        for path, source in cases.items():
            with self.subTest(path=path):
                self.assertGreater(
                    premeasure_streaming_policy_growth({path: source}, 0),
                    128 << 20,
                )

    def test_task8_policy_premeasure_does_not_allocate_path_or_authority_copies(self):
        path = PurePosixPath("playback/gpu/allocation-free-premeasure.cpp")
        with mock.patch.object(
            PurePosixPath,
            "as_posix",
            side_effect=AssertionError("premeasure rendered a path"),
        ), mock.patch.object(
            PurePosixPath,
            "parts",
            new_callable=mock.PropertyMock,
            side_effect=AssertionError("premeasure allocated path parts"),
        ), mock.patch.object(
            capability_audit,
            "frozenset",
            create=True,
            side_effect=AssertionError("premeasure copied authority"),
        ):
            required = premeasure_streaming_policy_growth(
                {path: "int value;\n"},
                0,
                authoritative_paths=(path,),
            )
        self.assertGreater(required, 0)

    def test_task8_policy_premeasure_owns_source_only_grammar_workspace(self):
        path = PurePosixPath("playback/gpu/source-only.cpp")
        sources = {path: "int value;\n" * 5000}
        reserved = premeasure_streaming_policy_growth(sources, 0)
        tracemalloc.start()
        try:
            capability_audit.audit_source_only(path, sources[path])
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertLessEqual(peak, reserved)

    def test_task8_exact_eight_mib_source_only_fails_compact_budget_preflight(self):
        path = PurePosixPath("playback/gpu/eight-mib-source-only.cpp")
        source = "x" * (8 << 20)
        reserved = premeasure_streaming_policy_growth({path: source}, 0)
        budget = capability_model.CompactResultMemoryBudget(
            maximum_bytes=128 << 20
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "aggregate 128 MiB"
        ):
            budget.reserve(reserved, label="final policy aggregate growth")

    def test_task8_bounded_policy_workspace_rejects_before_unowned_parse(self):
        import gpu_capability_runner as capability_runner

        path = PurePosixPath("playback/gpu/eight-mib-workspace.cpp")
        source = "x" * (8 << 20)
        workspace = capability_runner._BoundedPolicyWorkspace(128 << 20)
        with mock.patch.object(
            capability_audit, "translate_source", wraps=capability_audit.translate_source
        ) as translate, self.assertRaisesRegex(
            AuditInfrastructureError, "policy workspace"
        ):
            capability_audit.audit_source_only(
                path,
                source,
                workspace=workspace,
                sink=workspace.record,
            )
        translate.assert_not_called()
        self.assertEqual(workspace.current, 0)

    def test_task8_bounded_policy_workspace_exact_zero_finding_boundary(self):
        import gpu_capability_runner as capability_runner

        path = PurePosixPath("playback/gpu/zero-finding.cpp")
        source = "int value;\n" * 100
        schema = conservative_allocation_schema()
        exact = (
            2 * 65536
            + len(source) * schema.object_bound(8)
            + len(source) * schema.object_bound(0)
        )
        workspace = capability_runner._BoundedPolicyWorkspace(exact)
        capability_audit.audit_source_only(
            path, source, workspace=workspace, sink=workspace.record
        )
        self.assertEqual(workspace.finding_provenance, {})
        self.assertLessEqual(workspace.peak, exact)
        with self.assertRaisesRegex(AuditInfrastructureError, "policy workspace"):
            capability_audit.audit_source_only(
                path,
                source,
                workspace=capability_runner._BoundedPolicyWorkspace(exact - 1),
                sink=lambda *_args: None,
            )

    def test_task8_raw_sink_emits_each_path_before_releasing_its_scratch(self):
        class Workspace:
            def __init__(self):
                self.live = 0
                self.emissions = 0

            def reserve_policy_scratch(self, _characters, *, source_only):
                self.assert_false = source_only
                self.live += 1
                return 1

            def release_policy_scratch(self, charge):
                self.assertEqual(charge, 1)
                self.assertGreater(self.emissions, 0)
                self.live -= charge

            def assertEqual(self, left, right):
                self_case.assertEqual(left, right)

            def assertGreater(self, left, right):
                self_case.assertGreater(left, right)

        self_case = self
        workspace = Workspace()

        def sink(_finding, _provenance):
            self.assertGreater(workspace.live, 0)
            workspace.emissions += 1

        result = capability_audit.audit_raw_sources(
            {
                PurePosixPath("playback/gpu/first.cpp"): "surface.nativeHandle();\n",
                PurePosixPath("playback/gpu/second.cpp"): "surface.nativeHandle();\n",
            },
            workspace=workspace,
            sink=sink,
        )
        self.assertEqual(result, [])
        self.assertEqual(workspace.live, 0)
        self.assertGreaterEqual(workspace.emissions, 2)

    def test_task8_hash_encode_charge_fails_before_bytes_construction(self):
        import gpu_capability_runner as capability_runner

        schema = conservative_allocation_schema()
        value = "x" * 4096
        required = schema.bytes_bound(4 * len(value))
        workspace = capability_runner._BoundedPolicyWorkspace(required - 1)
        with self.assertRaisesRegex(AuditInfrastructureError, "policy workspace"):
            workspace.encode_for_hash(value)
        self.assertEqual(workspace.current, 0)

    def test_task8_policy_key_and_sort_path_lengths_do_not_allocate_first(self):
        import hashlib
        import gpu_capability_runner as capability_runner

        workspace = capability_runner._BoundedPolicyWorkspace(1 << 20)
        finding = self.finding()
        original_as_posix = PurePosixPath.as_posix
        rendered = []

        def charged_as_posix(path):
            self.assertGreater(workspace.current, 0)
            rendered.append(path)
            return original_as_posix(path)

        with mock.patch.object(
            PurePosixPath,
            "as_posix",
            new=charged_as_posix,
        ), mock.patch.object(
            PurePosixPath,
            "parts",
            new_callable=mock.PropertyMock,
            side_effect=AssertionError("path parts allocated outside the ledger"),
        ):
            workspace.record(finding, "source-only")
            ordered, charge = workspace.sorted_findings()
            canonical = hashlib.sha256()
            workspace.update_hash_path(canonical, finding.path)
        self.assertEqual(ordered, [finding])
        self.assertEqual(rendered, [finding.path])
        ordered.clear()
        workspace.release_transient(charge)

    def test_task8_policy_hash_frame_preflight_fails_before_struct_pack(self):
        import gpu_capability_runner as capability_runner

        workspace = capability_runner._BoundedPolicyWorkspace(1)
        with mock.patch.object(struct, "pack") as packed, self.assertRaisesRegex(
            AuditInfrastructureError, "policy workspace"
        ):
            workspace.pack_hash_frame(7)
        packed.assert_not_called()

    def test_task8_candidate_free_directive_fanout_is_sink_bounded(self):
        import gpu_capability_runner as capability_runner

        workspace = capability_runner._BoundedPolicyWorkspace(128 << 20)
        capability_audit.audit_source_only(
            PurePosixPath("playback/gpu/directive-fanout.cpp"),
            "#else\n" * 5000,
            workspace=workspace,
            sink=workspace.record,
        )
        self.assertEqual(len(workspace.finding_provenance), 5000)
        self.assertLessEqual(workspace.peak, workspace.capacity)


class RawLaneTests(unittest.TestCase):
    def setUp(self):
        self.path = PurePosixPath("playback/gpu/example.cpp")

    def findings(self, source: str):
        return capability_audit.audit_raw_sources({self.path: source})

    def test_compiler_covered_macro_definition_is_not_rejected_raw(self):
        source = (
            "#define X nativeHandle\n"
            "#define CAT(a,b) a##b\n"
            "lease.CAT(safe,X)();\n"
        )
        self.assertEqual(self.findings(source), [])

    def test_direct_source_access_remains_rejected(self):
        findings = self.findings("surface.nativeHandle();\n")
        self.assertEqual(findings[0].line, 1)

    def test_inactive_direct_spelling_is_still_raw_source(self):
        findings = self.findings("#if 0\nsurface.nativeHandle();\n#endif\n")
        self.assertTrue(any(item.line == 2 for item in findings))

    def test_physical_splice_and_line_directives_fail_raw(self):
        controls = (
            "surface.native\\\nHandle();\n",
            '#line 7 "playback/gpu/gpusurface.h"\n',
            '#li\\\nne 7 "playback/gpu/gpusurface.h"\n',
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])

    def test_malformed_directive_fails_raw(self):
        findings = self.findings("#define\n")
        self.assertTrue(any("directive" in item.reason for item in findings))

    def test_objective_c_import_is_a_valid_raw_directive(self):
        self.assertEqual(self.findings("#import <Foundation/Foundation.h>\n"), [])

    def test_quoted_and_angle_header_operands_survive_literal_masking(self):
        for source in (
            '#include "playback/gpu/gpusurface.h"\n',
            "#include <memory>\n",
            '#include /* configuration */ "playback/gpu/gpusurface.h"\n',
            "#include/**/<memory>\n",
            '#import "Foundation/Foundation.h"\n',
        ):
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_directives_requiring_operands_fail_when_empty(self):
        for source in (
            "#if\n", "#include\n", "#import\n", "#include // comment only\n"
        ):
            with self.subTest(source=source):
                findings = self.findings(source)
                self.assertTrue(any("directive" in item.reason for item in findings))

    def test_header_delimiters_and_trailing_tokens_are_validated(self):
        for source in (
            '#include "unterminated\n',
            "#include <unterminated\n",
            '#include "valid.h" trailing\n',
            "#import <Foundation/Foundation.h> trailing\n",
        ):
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])

    def test_conditional_directive_structure_is_balanced(self):
        for source in (
            "#if FLAG\n",
            "#elif FLAG\n",
            "#else\n",
            "#endif\n",
            "#if FLAG\n#else\n#else\n#endif\n",
            "#if FLAG\n#else\n#elif OTHER\n#endif\n",
        ):
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])

    def test_directive_trailing_tokens_and_macro_grammar_are_validated(self):
        malformed = (
            "#else trailing\n",
            "#endif trailing\n",
            "#define F(\n",
            "#define F(a,,b) a\n",
            "#define F(a, 1) a\n",
            "#if (\n#endif\n",
            "#if FLAG\n#elif )\n#endif\n",
        )
        for source in malformed:
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])

        valid = (
            "#if defined(FLAG) && (VALUE + 1)\n"
            "#else /* alternate */\n#endif // FLAG\n",
            "#define F(a, ...) a\n",
        )
        for source in valid:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_preprocessing_expression_grammar_rejects_invalid_sequences(self):
        malformed = (
            "#if 1 2\n#endif\n",
            "#if FLAG\n#elif 1 2\n#endif\n",
            "#if +\n#endif\n",
            "#if FLAG ? VALUE\n#endif\n",
        )
        for source in malformed:
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])
        valid = (
            "#if defined(FLAG)\n#endif\n",
            "#if FLAG ? VALUE : OTHER\n#endif\n",
            "#if !FLAG || (VALUE + 1 >= 2)\n#endif\n",
        )
        for source in valid:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_preprocessing_integer_character_and_alternative_operator_atoms(self):
        valid = (
            "#if 0xCAFEuL && 0b1010'0101ULL\n#endif\n",
            "#if 0777L bitand 01\n#endif\n",
            "#if '\\n' or L'x'\n#endif\n",
            "#if __cplusplus >= 202002L and not defined(OLD)\n#endif\n",
            "#if CHECK(1.0, token)\n#endif\n",
            '#if __has_include("foo.h")\n#endif\n',
            '#if CHECK("x", 1)\n#endif\n',
        )
        for source in valid:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])
        for source in (
            "#if 1(2)\n#endif\n",
            "#if 1.0\n#endif\n",
            "#if 0x\n#endif\n",
            "#if ''\n#endif\n",
            "#if 1uu\n#endif\n",
            "#if 1lul\n#endif\n",
            "#if '\\x'\n#endif\n",
        ):
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])

        for source in (
            "#if '\\x1'\n#endif\n",
            "#if '\\\\'\n#endif\n",
            "#if '\\101'\n#endif\n",
        ):
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_public_retirement_surfaces_remain_rejected(self):
        controls = (
            (
                REGISTRY_HEADER,
                "class GpuRetireRegistry final { public: void registerRetire(); };\n",
            ),
            (
                OP_SCOPE_HEADER,
                "class GpuOpScope final { public: void track(); };\n",
            ),
        )
        for path, source in controls:
            with self.subTest(path=path):
                findings = capability_audit.audit_raw_sources({path: source})
                self.assertNotEqual(findings, [])


class SourceOnlyLaneTests(unittest.TestCase):
    def setUp(self):
        self.path = PurePosixPath("playback/gpu/example.cpp")

    def findings(self, source: str):
        return capability_audit.audit_source_only(self.path, source)

    @staticmethod
    def depth_source(count: int) -> str:
        definitions = "".join(
            f"#define M{index} M{index + 1}\n" for index in range(count - 1)
        )
        return definitions + f"#define M{count - 1} safe\nlease.M0();\n"

    @staticmethod
    def token_source(count: int) -> str:
        return "#define M " + " ".join(("safe",) * count) + "\nlease.M();\n"

    @staticmethod
    def paste_source(count: int) -> str:
        return "#define M " + "##".join(("a",) * (count + 1)) + "\nlease.M();\n"

    def test_source_local_guarded_reconstruction_is_rejected(self):
        findings = self.findings(
            "#define CAT(a,b) a##b\nlease.CAT(native,Handle)();\n"
        )
        self.assertTrue(any("guarded identifier" in item.expression
                            for item in findings))

    def test_conditional_macro_definitions_are_fail_closed_in_both_orders(self):
        controls = (
            "#if FLAG\n#define M nativeHandle\n#else\n#define M safe\n#endif\nlease.M();\n",
            "#if FLAG\n#define M safe\n#else\n#define M nativeHandle\n#endif\nlease.M();\n",
        )
        for source in controls:
            with self.subTest(source=source):
                findings = self.findings(source)
                self.assertTrue(any(item.expression ==
                                    "source-only conditional macro ambiguity"
                                    for item in findings))

    def test_unrelated_conditional_and_va_opt_macros_are_clean(self):
        conditional = (
            "#if FLAG\n#define VALUE 1\n#else\n#define VALUE 2\n#endif\n"
            "int value = VALUE;\n"
        )
        va_opt = (
            "#define PICK(...) 7 __VA_OPT__(+ 1)\n"
            "int value = PICK(item);\n"
        )
        self.assertEqual(self.findings(conditional), [])
        self.assertEqual(self.findings(va_opt), [])

    def test_unconditional_redefinition_clears_conditional_state(self):
        source = (
            "#if FLAG\n#define M nativeHandle\n#else\n#define M safe\n#endif\n"
            "#undef M\n#define M safe\nlease.M();\n"
        )
        self.assertEqual(self.findings(source), [])

    def test_conditional_state_preserves_undefined_baseline(self):
        findings = self.findings(
            "#if FLAG\n#define M safe\n#endif\nlease.M();\n"
        )
        self.assertTrue(any(
            item.expression == "source-only conditional macro ambiguity"
            for item in findings
        ))

    def test_conditional_state_overflow_fails_with_named_complexity(self):
        def source(count: int) -> str:
            branches = ["#if V0\n#define M safe0\n"]
            branches.extend(
                f"#elif V{index}\n#define M safe{index}\n"
                for index in range(1, count - 1)
            )
            branches.append(f"#else\n#define M safe{count - 1}\n")
            return "".join(branches) + "#endif\nint value = M;\n"

        self.assertEqual(self.findings(source(1024)), [])
        findings = self.findings(source(1025))
        self.assertTrue(any(
            item.expression == "source-only conditional state complexity"
            for item in findings
        ))

    def test_conditional_environments_preserve_cross_macro_correlation(self):
        prefix = (
            "#define CAT(a,b) CAT_I(a,b)\n#define CAT_I(a,b) a##b\n"
        )
        controls = (
            prefix
            + "#if FLAG\n#define A native\n#else\n#define A safe\n#endif\n"
            "lease.CAT(A,Handle)();\n",
            prefix
            + "#if FLAG\n#define A native\n#define B Handle\n"
            "#else\n#define A safe\n#define B Value\n#endif\n"
            "lease.CAT(A,B)();\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only conditional macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_conditional_environments_honor_each_macro_kind(self):
        controls = (
            "#if FLAG\n#define M nativeHandle\n#else\n#define M() safe\n"
            "#endif\n&GpuSurface::M;\n",
            "#if FLAG\n#define M() safe\n#else\n#define M nativeHandle\n"
            "#endif\n&GpuSurface::M;\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only conditional macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_same_name_macro_argument_prescan_precedes_outer_disable(self):
        source = (
            "#define F(x) x\n"
            "#define A CAT\n"
            "#define CAT(a,b) a##b\n"
            "F(F(A))(native,Handle);\n"
        )
        self.assertTrue(any(
            item.expression == "guarded identifier macro composition"
            for item in self.findings(source)
        ))

    def test_parameter_prescan_depends_on_replacement_usage(self):
        chain = "".join(
            f"#define M{index} M{index + 1}\n" for index in range(96)
        ) + "#define M96 safe\n"
        safe = (
            "#define S(x) #x\nS(M0);\n",
            "#define IGNORE(x) safe\nIGNORE(M0);\n",
            "#define CAT(a,b) a##b\nCAT(M0,x);\n",
        )
        for suffix in safe:
            with self.subTest(suffix=suffix):
                self.assertEqual(self.findings(chain + suffix), [])
        findings = self.findings(chain + "#define ID(x) x\nlease.ID(M0)();\n")
        self.assertTrue(any(
            item.expression == "source-only macro expansion depth"
            for item in findings
        ))

    def test_consumed_macro_arguments_are_not_independently_audited(self):
        safe = (
            "#define X nativeHandle\n#define S(x) #x\nS(X);\n",
            "#define X nativeHandle\n#define CAT(a,b) a##b\nCAT(safe,X);\n",
        )
        for source in safe:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])
        dangerous = (
            "#define X nativeHandle\n#define ID(x) x\nID(X);\n",
            "#define CAT(a,b) a##b\nlease.CAT(native,Handle)();\n",
        )
        for source in dangerous:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "guarded identifier macro composition"
                    for item in self.findings(source)
                ))

    def test_chained_invocations_own_arguments_per_environment(self):
        safe = (
            "#define X nativeHandle\n#define S(x) #x\n"
            "#define F() S\nF()(X);\n",
            "#define X nativeHandle\n#define S(x) #x\n"
            "#define F S\nF(X);\n",
            "#define X nativeHandle\n#define S(x) #x\n"
            "#if FLAG\n#define F(x) #x\n#else\n#define F S\n#endif\nF(X);\n",
        )
        for source in safe:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])
        dangerous = (
            "#define X nativeHandle\n#define ID(x) x\n"
            "#define F() ID\nF()(X);\n",
            "#define X nativeHandle\n#define ID(x) x\n"
            "#if FLAG\n#define F(x) x\n#else\n#define F ID\n#endif\n"
            "lease.F(X)();\n",
        )
        for source in dangerous:
            with self.subTest(source=source):
                self.assertNotEqual(self.findings(source), [])

    def test_whole_expansion_is_audited_for_capabilities(self):
        findings = self.findings(
            "#define CALL surface.nativeHandle\nCALL();\n"
        )
        self.assertTrue(any("guarded identifier" in item.expression
                            for item in findings))

    def test_macro_generated_nonlocal_control_call_is_rejected(self):
        findings = self.findings(
            "#define CAT(a,b) a##b\nCAT(long,jmp)(env, 1);\n"
        )
        self.assertTrue(any(
            item.expression == "guarded identifier macro composition"
            for item in findings
        ))

    def test_va_opt_paste_expansion_fails_closed(self):
        findings = self.findings(
            "#define CAT(a,...) a __VA_OPT__(## __VA_ARGS__)\n"
            "lease.CAT(native,Handle)();\n"
        )
        self.assertTrue(any("guarded identifier" in item.expression
                            for item in findings))

    def test_standalone_va_opt_expansion_runs_capability_policy(self):
        dangerous = (
            "#define CALL(...) surface __VA_OPT__(.nativeHandle())\n"
            "CALL(enabled);\n"
        )
        self.assertTrue(any(
            "guarded identifier" in item.expression
            or item.expression == "source-only unsupported macro construct"
            for item in self.findings(dangerous)
        ))
        self.assertEqual(self.findings(
            "#define PICK(...) 7 __VA_OPT__(+ 1)\nint value = PICK(item);\n"
        ), [])

    def test_va_opt_emptiness_uses_prescanned_variadic_arguments(self):
        safe = (
            "#define EMPTY\n"
            "#define CALL(...) surface __VA_OPT__(.nativeHandle())\n"
            "CALL(EMPTY);\n"
        )
        dangerous = safe.replace("CALL(EMPTY)", "CALL(enabled)")
        self.assertEqual(self.findings(safe), [])
        self.assertTrue(any(
            "guarded identifier" in item.expression
            for item in self.findings(dangerous)
        ))

    def test_macro_generated_public_registry_member_is_rejected(self):
        source = (
            "#define RETIRE public: void registerRetire();\n"
            "class GpuRetireRegistry { RETIRE };\n"
        )
        findings = capability_audit.audit_source_only(REGISTRY_HEADER, source)
        self.assertTrue(any("registerRetire" in item.expression
                            for item in findings))

    def test_expansion_uses_capability_grammar_not_token_membership(self):
        source = (
            "#define MODE read\n#define STATE complete\n"
            "int mode = MODE; int state = STATE;\n"
        )
        self.assertEqual(self.findings(source), [])

    def test_macro_generated_private_registry_member_is_allowed(self):
        source = (
            "#define RETIRE private: void registerRetire();\n"
            "class GpuRetireRegistry { RETIRE };\n"
        )
        self.assertEqual(
            capability_audit.audit_source_only(REGISTRY_HEADER, source), []
        )

    def test_malformed_sensitive_expansion_fails_closed(self):
        findings = self.findings("#define CALL(x) x\nlease.CALL(nativeHandle(;\n")
        self.assertTrue(any("syntax" in item.expression for item in findings))

    def test_expansion_depth_accepts_96_and_rejects_97(self):
        self.assertEqual(self.findings(self.depth_source(96)), [])
        findings = self.findings(self.depth_source(97))
        self.assertTrue(any(item.expression == "source-only macro expansion depth"
                            for item in findings))

    def test_generated_tokens_accept_2048_and_reject_2049(self):
        self.assertEqual(self.findings(self.token_source(2048)), [])
        findings = self.findings(self.token_source(2049))
        self.assertTrue(any(item.expression == "source-only macro token complexity"
                            for item in findings))

    def test_token_pastes_accept_1024_and_reject_1025(self):
        self.assertEqual(self.findings(self.paste_source(1024)), [])
        findings = self.findings(self.paste_source(1025))
        self.assertTrue(any(item.expression == "source-only macro paste complexity"
                            for item in findings))

    def test_unknown_macros_in_sensitive_contexts_fail_closed(self):
        controls = (
            "UNKNOWN(scope).withRead(surface, callback);\n",
            "lease.UNKNOWN();\n",
            "GpuSyncReadScope scope; scope.withRead(surface, UNKNOWN(callback));\n",
            "GpuSyncReadScope scope; scope.UNKNOWN();\n",
            "GpuSyncReadScope scope; scope.withRead(surface, [](auto& lease) "
            "{ UNKNOWN(return); });\n",
        )
        for source in controls:
            with self.subTest(source=source):
                findings = self.findings(source)
                self.assertTrue(any("source-only macro ambiguity" == item.expression
                                    for item in findings))

    def test_lowercase_ambiguity_is_receiver_aware(self):
        for source in (
            "unknown(scope).withRead(surface, callback);\n",
            "lease.unknown();\n",
        ):
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))
        self.assertEqual(self.findings("Logger::UNKNOWN();\n"), [])

    def test_receiver_proof_uses_types_and_lease_bindings_not_names(self):
        unrelated = (
            "struct Logger { void withRead(); void unknown(); };\n"
            "Logger scope; Logger lease; scope.withRead(); lease.unknown();\n"
        )
        self.assertEqual(self.findings(unrelated), [])

        controls = (
            "ALIAS.withRead(surface, callback);\n",
            "GpuSyncReadScope unusual; auto token = unusual.read(surface); "
            "token.unknown(); unusual.complete();\n",
            "GpuSyncReadScope unusual; unusual.withRead(surface, "
            "[](const GpuReadLease& token) { token.unknown(); });\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_pointer_reference_receivers_and_lowercase_aliases(self):
        unrelated = (
            "struct Logger { void withRead(); void unknown(); };\n"
            "Logger* scope; Logger& lease = logger;\n"
            "scope->withRead(); lease.unknown();\n"
        )
        self.assertEqual(self.findings(unrelated), [])
        self.assertTrue(any(
            item.expression == "source-only macro ambiguity"
            for item in self.findings("alias.withRead(surface, callback);\n")
        ))

    def test_arbitrary_typed_and_auto_receivers_are_proven_ordinary(self):
        controls = (
            "logger scope; scope.withRead();\n",
            "ns::Logger<Item>* scope; scope->withRead();\n",
            "auto lease = makeLogger(); lease.unknown();\n",
            "void inspect(ns::Logger<Item>& lease) { lease.unknown(); }\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])
        self.assertTrue(any(
            item.expression == "source-only macro ambiguity"
            for item in self.findings("alias.withRead(surface, callback);\n")
        ))

    def test_receiver_bindings_are_positioned_lexical_and_shadowable(self):
        rejected = (
            "alias.withRead(surface, callback); logger alias;\n",
            "{ logger alias; } alias.withRead(surface, callback);\n",
            "void f() { logger value; { GpuSyncReadScope value; value.unknown(); } }\n",
        )
        for source in rejected:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))
        clean = (
            "void f() { GpuSyncReadScope value; { logger value; value.unknown(); } }\n",
            "void f() { logger value; { GpuSyncReadScope value; } value.unknown(); }\n",
        )
        for source in clean:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_balanced_declarators_prove_ordinary_receivers(self):
        controls = (
            "logger scope(make()); scope.withRead();\n",
            "logger scope{}; scope.withRead();\n",
            "logger first, scope; scope.withRead();\n",
            "ns::Logger<Item> first{}, *scope(makePtr()); scope->withRead();\n",
            "[[maybe_unused]] logger scope; scope.withRead();\n",
            "decltype(make()) scope; scope.withRead();\n",
            "auto [scope, other] = makePair(); scope.withRead();\n",
            "const auto& [scope, other] = makePair(); scope.withRead();\n",
            "static auto [scope, other] = makePair(); scope.withRead();\n",
            "void f() { for (Logger& scope : scopes) { scope.withRead(); } }\n",
            "void f() { for (auto& scope : scopes) { scope.withRead(); } }\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_conditional_environments_cover_receivers_and_callbacks(self):
        controls = (
            "#if FLAG\n#define ALIAS scope\n#else\n#undef ALIAS\n#endif\n"
            "ALIAS.withRead(surface, callback);\n",
            "GpuSyncReadScope scope;\n#if FLAG\n#define CB callback\n"
            "#else\n#undef CB\n#endif\nscope.withRead(surface, CB(callback));\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only conditional macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_conditional_member_macro_on_ordinary_receiver_is_clean(self):
        source = (
            "struct Logger { void safe(); void other(); }; Logger log;\n"
            "#if FLAG\n#define M safe\n#else\n#define M other\n#endif\n"
            "log.M();\n"
        )
        self.assertEqual(self.findings(source), [])

    def test_conditional_state_overflow_rejects_external_sensitive_names(self):
        conditionals = "".join(
            f"#if F{index}\n#define M{index} 1\n#endif\n"
            for index in range(12)
        )
        for suffix in (
            "alias.withRead(surface, callback);\n",
            "GpuSyncReadScope scope; scope.withRead(surface, CB(callback));\n",
            "void f() { GpuSyncReadScope scope; scope.MEMBER(); }\n",
            "alias.MEMBER();\n",
            "void f() { GpuSyncReadScope scope; scope.DONE(); }\n",
        ):
            with self.subTest(suffix=suffix):
                self.assertTrue(any(
                    item.expression == "source-only conditional state complexity"
                    for item in self.findings(conditionals + suffix)
                ))

    def test_ordinary_withread_callback_is_not_capability_sensitive(self):
        source = (
            "struct Logger { template<class F> void withRead(int, F); };\n"
            "void f() { Logger scope; scope.withRead(surface, SAFE(callback)); }\n"
        )
        self.assertEqual(self.findings(source), [])

    def test_binding_scope_index_does_not_rescan_brace_pairs(self):
        source = (
            "void f() {\n"
            + "{ Logger local; local.unknown();\n" * 64
            + "}\n" * 64
            + "}\n"
        )
        translated = capability_audit.translate_source(source)
        _events, directive_ranges = capability_audit.source_macro_events(
            translated.masked
        )
        with mock.patch.object(
            capability_audit,
            "enclosing_block",
            wraps=capability_audit.enclosing_block,
        ) as enclosing_probe:
            capability_audit._source_only_declared_bindings(
                translated.masked, directive_ranges
            )
        self.assertEqual(enclosing_probe.call_count, 0)

    def test_type_alias_and_outer_declared_type_control_receiver_category(self):
        capability = (
            "void f() { using Scope = GpuSyncReadScope; Scope scope; "
            "scope.unknown(); }\n",
            "void f() { typedef GpuSyncReadScope Scope; Scope scope; "
            "scope.unknown(); }\n",
        )
        for source in capability:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))
        ordinary = (
            "void f() { vector<GpuSyncReadScope> scopes; scopes.withRead(); }\n",
            "void f() { decltype(GpuSyncReadScope{}.desc()) desc; "
            "desc.unknown(); }\n",
        )
        for source in ordinary:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_type_declarations_and_template_parameters_shadow_aliases(self):
        controls = (
            "using Scope = GpuSyncReadScope; void f() { struct Scope {}; "
            "Scope scope; scope.unknown(); } Scope outer; outer.unknown();\n",
            "using Scope = GpuSyncReadScope; template<class Scope> "
            "void f() { Scope scope; scope.unknown(); }\n",
            "using Scope = GpuSyncReadScope; template<class Scope> "
            "void f(Scope scope) { scope.unknown(); } "
            "Scope outer; outer.unknown();\n",
        )
        for source in controls:
            with self.subTest(source=source):
                findings = self.findings(source)
                if "outer" in source:
                    self.assertTrue(any(
                        item.expression == "source-only macro ambiguity"
                        and item.line == 1 for item in findings
                    ))
                    self.assertEqual(sum(
                        item.reason.startswith("unresolved macro-like unknown")
                        for item in findings
                    ), 1)
                else:
                    self.assertEqual(findings, [])

    def test_complex_receiver_requires_ordinary_or_documented_proof(self):
        rejected = (
            "s[0].withRead(surface, callback);\n",
            "(s + 1)->withRead(surface, callback);\n",
            "Logger value; move(value).withRead(surface, callback);\n",
            "Logger value; evil::as_const(value).withRead(surface, callback);\n",
            "Logger value; value.get().withRead(surface, callback);\n",
        )
        for source in rejected:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))
        clean = (
            "Logger* s; s[0].withRead(); (s + 1)->withRead();\n",
            "Logger value; std::move(value).withRead();\n",
            "Logger value; std::as_const(value).withRead();\n",
            "Logger value; std::ref(value).get().withRead();\n",
            "std::reference_wrapper<Logger> value; value.get().withRead();\n",
        )
        for source in clean:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_alias_targets_preserve_strict_wrapper_provenance(self):
        clean = (
            "std::reference_wrapper<Logger> w; w.get().unknown();\n",
            "using W = std::reference_wrapper<Logger>; W w; "
            "w.get().unknown();\n",
            "using W0 = std::reference_wrapper<Logger>; using W = W0; "
            "W w; w.get().unknown();\n",
        )
        for source in clean:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

        rejected = (
            "using W = std::reference_wrapper<GpuSyncReadScope>; W w; "
            "w.get().unknown();\n",
            "using W0 = std::reference_wrapper<GpuReadLease>; using W = W0; "
            "W w; w.get().unknown();\n",
            "using W = evil::reference_wrapper<Logger>; W w; "
            "w.get().unknown();\n",
            "using A = B; using B = A; A value; value.unknown();\n",
        )
        for source in rejected:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_std_wrapper_alias_requires_positioned_namespace_provenance(self):
        clean = (
            "using L = Logger; using W = ::std::reference_wrapper<L>; "
            "W w; w.get().unknown();\n",
            "void f() { struct std {}; "
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown(); }\n",
            "void f() { struct std {}; } "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "#define std evil\n#undef std\n"
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n#define std evil\n",
        )
        for source in clean:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

        rejected = (
            "using S = GpuSyncReadScope; "
            "using W = ::std::reference_wrapper<S>; "
            "W w; w.get().unknown();\n",
            "struct std {}; using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "struct std {}; using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "void f() { struct std {}; "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown(); }\n",
            "namespace std = evil; "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "#define std evil\n"
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "#define std evil\n"
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "#if FLAG\n#define std evil\n#endif\n"
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
        )
        for source in rejected:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_std_namespace_definitions_are_positioned_provenance(self):
        fake_wrapper = (
            "template<class T> struct reference_wrapper { "
            "GpuSyncReadScope& get(); }; "
        )
        clean = (
            "namespace project { namespace std { " + fake_wrapper + "} } "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "namespace project::std { " + fake_wrapper + "} "
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "void f() { struct std {}; "
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown(); }\n",
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown(); "
            "namespace std { " + fake_wrapper + "}\n",
        )
        for source in clean:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

        rejected = (
            "namespace std { " + fake_wrapper + "} "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "namespace std { " + fake_wrapper + "} "
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "inline namespace std { " + fake_wrapper + "} "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
            "namespace project { namespace std { " + fake_wrapper + "} "
            "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown(); }\n",
            "namespace project::std { " + fake_wrapper
            + "using W = std::reference_wrapper<Logger>; "
            "W w; w.get().unknown(); }\n",
            "namespace std::detail {} "
            "using W = ::std::reference_wrapper<Logger>; "
            "W w; w.get().unknown();\n",
        )
        for source in rejected:
            with self.subTest(source=source):
                self.assertTrue(any(
                    item.expression == "source-only macro ambiguity"
                    for item in self.findings(source)
                ))

    def test_isolated_same_name_use_before_declaration_stays_unresolved(self):
        source = "".join(
            f"void f{index}() {{ scope.withRead(); Logger scope; }}\n"
            for index in range(128)
        )
        with mock.patch.object(
            capability_audit,
            "receiver_binding_references",
            wraps=capability_audit.receiver_binding_references,
        ) as receiver_probe:
            findings = self.findings(source)
        self.assertLessEqual(receiver_probe.call_count, 128)
        self.assertGreaterEqual(sum(
            item.expression == "source-only macro ambiguity" for item in findings
        ), 128)

    def test_many_read_result_bindings_are_indexed_once(self):
        source = "".join(
            f"void f{index}() {{ GpuSyncReadScope scope; "
            "auto lease = scope.read(surface); lease.unknown(); "
            "scope.complete(); }\n"
            for index in range(128)
        )
        translated = capability_audit.translate_source(source)
        _events, ranges = capability_audit.source_macro_events(translated.masked)
        with mock.patch.object(
            capability_audit,
            "_source_binding_index",
            wraps=capability_audit._source_binding_index,
        ) as index_probe:
            bindings = capability_audit._source_only_declared_bindings(
                translated.masked, ranges
            )
        self.assertLessEqual(index_probe.call_count, 1)
        self.assertEqual(sum(
            binding.category == "lease" for binding in bindings
        ), 128)

    def test_va_opt_presence_retains_prescanned_variadic_commas(self):
        prefix = (
            "#define E\n"
            "#define CALL(...) surface __VA_OPT__(.nativeHandle())\n"
        )
        self.assertEqual(self.findings(prefix + "CALL(E);\n"), [])
        self.assertTrue(any(
            item.expression == "guarded identifier macro composition"
            for item in self.findings(prefix + "CALL(E,E);\n")
        ))

    def test_unused_macro_replacement_calls_do_not_enter_receiver_audit(self):
        controls = (
            "#define UNUSED surface.nativeHandle()\n",
            "#define UNUSED scope.withRead(surface, callback)\n",
            "#define UNUSED lease.complete()\n",
        )
        for source in controls:
            with self.subTest(source=source):
                self.assertEqual(self.findings(source), [])

    def test_nested_withread_ambiguity_uses_precomputed_delimiters(self):
        depth = 64
        source = (
            "GpuSyncReadScope scope;\n"
            + "scope.withRead(surface, [&] {\n" * depth
            + "safe();\n"
            + "});\n" * depth
        )
        with mock.patch.object(
            capability_audit,
            "matching_delimiter",
            wraps=capability_audit.matching_delimiter,
        ) as delimiter_probe, mock.patch.object(
            capability_audit,
            "_source_only_declared_bindings",
            wraps=capability_audit._source_only_declared_bindings,
        ) as binding_probe, mock.patch.object(
            capability_audit,
            "_source_binding_index",
            wraps=capability_audit._source_binding_index,
        ) as index_probe, mock.patch.object(
            capability_audit,
            "_source_scope_at_fallback",
            wraps=capability_audit._source_scope_at_fallback,
        ) as fallback_probe:
            self.findings(source)
        self.assertLessEqual(delimiter_probe.call_count, 1)
        self.assertLessEqual(binding_probe.call_count, 2)
        self.assertLessEqual(index_probe.call_count, 4)
        self.assertEqual(fallback_probe.call_count, 0)

    def test_adjacent_sensitive_calls_reuse_statement_tokenization(self):
        source = (
            "void audit() { GpuSyncReadScope scope;\n"
            + "scope.withRead(surface, callback);\n" * 100
            + "}\n"
        )
        with mock.patch.object(
            capability_audit,
            "canonical_scope_declaration",
            wraps=capability_audit.canonical_scope_declaration,
        ) as declaration_probe:
            self.findings(source)
        self.assertLessEqual(declaration_probe.call_count, 1)

    def test_unrelated_unknown_macro_use_is_ignored(self):
        self.assertEqual(self.findings("int value = UNKNOWN(1);\n"), [])


class LiveCompilerCliTests(unittest.TestCase):
    def test_repeated_live_compilers_are_typed_and_duplicates_fail(self):
        parser = getattr(capability_audit, "parse_live_compiler_options", None)
        self.assertTrue(callable(parser))
        if not callable(parser):
            return
        parsed = parser((
            f"gcc={sys.executable}",
            f"clang={Path(sys.executable).with_name('clang.exe')}",
        ))
        self.assertEqual(set(parsed), {CompilerFamily.GCC, CompilerFamily.CLANG})
        self.assertEqual(parsed[CompilerFamily.GCC], Path(sys.executable).resolve())
        with self.assertRaisesRegex(
            AuditInfrastructureError, "duplicate live compiler family"
        ):
            parser((
                f"gcc={sys.executable}", f"gcc={sys.executable}",
            ))
        with self.assertRaisesRegex(
            AuditInfrastructureError, "unsupported live compiler family"
        ):
            parser(("cuda=nvcc",))

    def test_required_missing_fails_and_optional_absent_is_explicit(self):
        live_runner = getattr(capability_audit, "run_live_only", None)
        self.assertTrue(callable(live_runner))
        if not callable(live_runner):
            return
        messages: list[str] = []
        with self.assertRaisesRegex(
            AuditInfrastructureError, "required live compiler family is missing: gcc"
        ):
            live_runner(
                {},
                frozenset({CompilerFamily.GCC}),
                suite_runner=lambda *_args: self.fail("runner must not be called"),
                printer=messages.append,
            )

        calls: list[tuple[Path, CompilerFamily]] = []
        live_runner(
            {CompilerFamily.GCC: Path(sys.executable).resolve()},
            frozenset(),
            suite_runner=lambda compiler, family, _budget: calls.append((compiler, family)),
            printer=messages.append,
        )
        self.assertEqual(calls, [
            (Path(sys.executable).resolve(), CompilerFamily.GCC)
        ])
        self.assertEqual(sum("SKIP: optional live compiler" in line
                             for line in messages), 3)

    def test_one_total_live_budget_is_shared_across_all_families(self):
        live_runner = getattr(capability_audit, "run_live_only", None)
        self.assertTrue(callable(live_runner))
        if not callable(live_runner):
            return
        now = [100.0]
        budgets = []

        def suite(_compiler, _family, budget):
            budgets.append(budget)
            budget.remaining_seconds()
            now[0] += 10.0

        live_runner(
            {
                CompilerFamily.GCC: Path(sys.executable).resolve(),
                CompilerFamily.CLANG: Path(sys.executable).resolve(),
            },
            frozenset(),
            suite_runner=suite,
            printer=lambda _message: None,
            clock=lambda: now[0],
            total_seconds=240.0,
        )
        self.assertEqual(len(budgets), 2)
        self.assertIs(budgets[0], budgets[1])
        self.assertEqual(budgets[0].deadline, 340.0)

    def test_live_budget_expiry_after_suite_is_infrastructure_failure(self):
        live_runner = getattr(capability_audit, "run_live_only", None)
        self.assertTrue(callable(live_runner))
        if not callable(live_runner):
            return
        now = [25.0]

        def overrun(_compiler, _family, budget):
            budget.remaining_seconds()
            now[0] = 266.0

        with self.assertRaisesRegex(
            AuditInfrastructureError, "total execution deadline"
        ):
            live_runner(
                {CompilerFamily.GCC: Path(sys.executable).resolve()},
                frozenset(),
                suite_runner=overrun,
                printer=lambda _message: None,
                clock=lambda: now[0],
                total_seconds=240.0,
            )

    def test_live_cli_normalizes_suite_oserror_to_status_two(self):
        import test_gpu_capability_live_compilers as live_module

        output = io.StringIO()
        arguments = (
            "gpu_capability_source_audit.py",
            "--source-root", ".",
            "--live-only",
            "--live-compiler", f"gcc={sys.executable}",
            "--require-live-family", "gcc",
        )
        with (
            mock.patch.object(sys, "argv", arguments),
            mock.patch.object(
                live_module,
                "run_live_compiler_suite",
                side_effect=OSError("fixture disk unavailable"),
            ),
            contextlib.redirect_stdout(output),
        ):
            status = capability_audit.main()
        self.assertEqual(status, 2)
        self.assertIn("live compiler execution failed", output.getvalue())

    def test_live_cli_returns_infrastructure_status_for_missing_requirement(self):
        self.assertTrue(hasattr(capability_audit, "run_live_only"))
        if not hasattr(capability_audit, "run_live_only"):
            return
        output = io.StringIO()
        arguments = (
            "gpu_capability_source_audit.py",
            "--source-root", ".",
            "--live-only",
            "--require-live-family", "gcc",
        )
        with mock.patch.object(sys, "argv", arguments), contextlib.redirect_stdout(output):
            status = capability_audit.main()
        self.assertEqual(status, 2)
        self.assertIn("required live compiler family is missing: gcc", output.getvalue())


class PipelineLaneTests(unittest.TestCase):
    def setUp(self):
        self.helper = CompilerAuditLaneTests()
        self.helper.setUp()
        self.authoritative = PurePosixPath("playback/gpu/gpufence.h")
        self.source_only = PurePosixPath("playback/gpu/inactive.cpp")

    def test_pipeline_unions_raw_authoritative_and_source_only_findings(self):
        view = self.helper.view((
            self.helper.production(
                str(self.authoritative), 31, b"surface.nativeHandle();"
            ),
        ))
        sources = {
            self.authoritative: "#define HANDLE nativeHandle\nsurface.HANDLE();\n",
            self.source_only: (
                "#define CAT(a,b) a##b\nlease.CAT(native,Handle)();\n"
            ),
        }
        coverage = CoverageReport(
            authoritative=frozenset((self.authoritative,)),
            source_only=frozenset((self.source_only,)),
            configurations=("cfg-a",),
        )
        findings, returned = capability_audit.audit_pipeline(sources, (view,), coverage)
        self.assertEqual(returned, coverage)
        configurations = {
            configuration
            for item in findings
            for configuration in item.configurations
        }
        self.assertIn("cfg-a", configurations)
        self.assertIn("source-only", configurations)

    def test_authoritative_path_does_not_use_source_only_emulation(self):
        view = self.helper.view((
            self.helper.production(str(self.authoritative), 1, b"void safe();"),
        ))
        sources = {
            self.authoritative: (
                "#define CAT(a,b) a##b\nlease.CAT(native,Handle)();\n"
            ),
        }
        coverage = CoverageReport(
            authoritative=frozenset((self.authoritative,)),
            source_only=frozenset(),
            configurations=("cfg-a",),
        )
        findings, _returned = capability_audit.audit_pipeline(sources, (view,), coverage)
        self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
