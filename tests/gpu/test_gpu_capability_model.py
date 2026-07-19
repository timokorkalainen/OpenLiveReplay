import dataclasses
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from array import array
from collections.abc import Sequence
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_model import (  # noqa: E402
    AUDIT_RESULT_SCHEMA_BYTES,
    UINT32_MAX,
    AuditResultFinding,
    AuditInfrastructureError,
    AuditLimits,
    CompactResultColdSlot,
    CompactResultMemoryBudget,
    CompactResultDraftBounds,
    CompactTokenSequence,
    CompilerLaunchEvent,
    CompilerLaunchPurpose,
    CompilerExecutableCapability,
    CompilerInspection,
    CompilerFamily,
    CoverageReport,
    ConfigurationAuditResult,
    ConfigurationAuditOutcome,
    ConfigurationAuditPublicationPermit,
    CachePublicationPermit,
    DependencyDigest,
    DependencyRootAuthority,
    DependencyRootBinding,
    FileIdentity,
    PackedTokenRun,
    PerTaskCompactReservation,
    ProcessStartIdentity,
    PreprocessedToken,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    SourceLocation,
    StreamingResultAggregator,
    WorkerStageTimings,
    compact_result_retained_bytes,
    encode_canonical_summary,
    _check_casefold_collision,
    _FilesystemGenerationObserver,
    _validate_native_canonical_text,
    _walk_production_entries,
    build_dependency_root_authority,
    decode_compiler_inspection,
    decode_local_dependency_digest,
    encode_compiler_inspection,
    encode_local_dependency_digest,
    enumerate_production_identities,
    portable_dependency_key,
    requires_compile_entry,
    validate_dependency_root_authority,
)


class ModelTests(unittest.TestCase):
    def identity(
        self,
        relative: str = "playback/gpu/example.cpp",
        *,
        line_count: int = 7,
    ) -> FileIdentity:
        return FileIdentity(
            canonical=Path("D:/repo") / relative,
            relative=PurePosixPath(relative),
            device=3,
            inode=9,
            line_count=line_count,
            production=True,
        )

    def configuration(self, *, digest: str = "cfg-a") -> PreprocessConfiguration:
        identity = self.identity()
        executable_identity = FileIdentity(
            Path("C:/toolchain/g++.exe"), None, 3, 10, 0, False
        )
        toolchain = DependencyRootBinding(
            "toolchain",
            Path("C:/toolchain"),
            FileIdentity(Path("C:/toolchain"), None, 3, 11, 0, False),
        )
        capability = CompilerExecutableCapability(
            "windows", executable_identity, "1" * 64, "2" * 64, object(),
            toolchain, (), (), "3" * 64, (),
        )
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=CompilerFamily.GCC,
            compiler=Path("C:/toolchain/g++.exe"),
            working_directory=Path("D:/repo/build"),
            source=identity,
            arguments=("-std=c++17", "playback/gpu/example.cpp"),
            environment_digest="environment-a",
            digest=digest,
            dependency_root_authority_digest="a" * 64,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )

    def sequence(self) -> CompactTokenSequence:
        identity = self.identity()
        return CompactTokenSequence._from_packed(
            self.configuration(),
            spellings=(b"lease", b".", b"nativeHandle", b"(", b")", b";"),
            identities=(None, identity),
            spelling_ids=array("I", (0, 1, 2, 3, 4, 5, 0)),
            identity_ids=array("I", (1, 1, 1, 1, 1, 1, 1)),
            inclusion_ids=array("I", (4, 4, 4, 4, 4, 4, 5)),
            original_lines=array("I", (12, 12, 12, 12, 12, 12, 20)),
        )

    def dependency(
        self,
        *,
        identity: FileIdentity | None = None,
        sha256: str = "b" * 64,
    ) -> DependencyDigest:
        return DependencyDigest(
            stable_role="production",
            role_relative_path=PurePosixPath("playback/gpu/example.cpp"),
            identity=identity or self.identity(),
            sha256=sha256,
        )

    def test_dependency_root_authority_binds_local_roots_and_portable_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source"
            sdk = root / "sdk"
            source.mkdir()
            sdk.mkdir()
            authority = build_dependency_root_authority(source, {"sdk:test": sdk})
        self.assertIsInstance(authority, DependencyRootAuthority)
        self.assertIsInstance(authority.source_root, DependencyRootBinding)
        self.assertEqual(authority.source_root.stable_role, "production")
        self.assertEqual(authority.external_roots[0].stable_role, "sdk:test")
        self.assertRegex(authority.portable_authority_digest, r"\A[0-9a-f]{64}\Z")

    def test_repeated_authority_validation_rejects_reparse_root_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source"
            sdk = root / "sdk"
            source.mkdir()
            sdk.mkdir()
            authority = build_dependency_root_authority(source, {"sdk:test": sdk})
            original_lstat = Path.lstat

            def reparse_lstat(path: Path):
                metadata = original_lstat(path)
                if path == sdk.resolve():
                    return SimpleNamespace(
                        st_dev=metadata.st_dev,
                        st_ino=metadata.st_ino,
                        st_mode=metadata.st_mode,
                        st_size=metadata.st_size,
                        st_mtime_ns=metadata.st_mtime_ns,
                        st_ctime_ns=metadata.st_ctime_ns,
                        st_file_attributes=0x400,
                    )
                return metadata

            with mock.patch.object(Path, "lstat", reparse_lstat), self.assertRaisesRegex(
                AuditInfrastructureError, "root identity changed|ordinary directory"
            ):
                validate_dependency_root_authority(authority)

    def test_compiler_inspection_is_exact_frozen_and_strict(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        compiler = Path(temporary.name).resolve() / "g++.exe"
        compiler.write_bytes(b"compiler")
        metadata = compiler.stat()
        identity = FileIdentity(
            canonical=compiler, relative=None, device=int(metadata.st_dev),
            inode=int(metadata.st_ino) or None, line_count=0, production=False,
        )
        inspection = CompilerInspection(
            CompilerFamily.GCC, identity,
            hashlib.sha256(compiler.read_bytes()).hexdigest(),
            "g++ (GCC) 14.1.0", "b" * 64, "c" * 64, "d" * 64,
        )
        self.assertEqual(
            tuple(inspection.__dataclass_fields__),
            (
                "compiler_family",
                "executable_identity",
                "executable_sha256",
                "normalized_version",
                "driver_fingerprint",
                "inspection_arguments_digest",
                "executable_capability_digest",
            ),
        )
        self.assertFalse(hasattr(inspection, "__dict__"))
        with self.assertRaises(dataclasses.FrozenInstanceError):
            inspection.normalized_version = "changed"
        replacements = {
            "compiler_family": True,
            "executable_identity": object(),
            "executable_sha256": "A" * 64,
            "normalized_version": "",
            "driver_fingerprint": "b" * 63,
            "inspection_arguments_digest": "not-a-digest",
            "executable_capability_digest": "D" * 64,
        }
        for field, value in replacements.items():
            with self.subTest(field=field), self.assertRaises(AuditInfrastructureError):
                dataclasses.replace(inspection, **{field: value})
        directory_identity = dataclasses.replace(identity, canonical=compiler.parent)
        with self.assertRaisesRegex(AuditInfrastructureError, "regular"):
            dataclasses.replace(inspection, executable_identity=directory_identity)
        compiler.write_bytes(b"changed")
        with self.assertRaisesRegex(AuditInfrastructureError, "changed"):
            dataclasses.replace(inspection)

    def test_compiler_inspection_codec_is_exact_and_rejects_unknown_keys(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        compiler = Path(temporary.name).resolve() / "g++.exe"
        compiler.write_bytes(b"compiler")
        metadata = compiler.stat()
        identity = FileIdentity(
            canonical=compiler, relative=None, device=int(metadata.st_dev),
            inode=int(metadata.st_ino) or None, line_count=0, production=False,
        )
        inspection = CompilerInspection(
            CompilerFamily.GCC, identity,
            hashlib.sha256(compiler.read_bytes()).hexdigest(), "g++ (GCC) 14.1.0",
            "b" * 64, "c" * 64, "d" * 64,
        )
        payload = encode_compiler_inspection(inspection)
        self.assertEqual(decode_compiler_inspection(payload), inspection)
        document = json.loads(payload)
        document["unknown"] = 1
        with self.assertRaisesRegex(AuditInfrastructureError, "schema"):
            decode_compiler_inspection(
                json.dumps(document, separators=(",", ":")).encode("ascii")
            )

    def test_compiler_inspection_hash_validation_obeys_caller_deadline(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        compiler = Path(temporary.name).resolve() / "g++.exe"
        compiler.write_bytes(b"compiler")
        metadata = compiler.stat()
        identity = FileIdentity(
            compiler, None, int(metadata.st_dev), int(metadata.st_ino) or None,
            0, False,
        )
        arguments = (
            CompilerFamily.GCC, identity,
            hashlib.sha256(compiler.read_bytes()).hexdigest(),
            "g++ (GCC) 14.1.0", "b" * 64, "c" * 64, "d" * 64,
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            CompilerInspection(
                *arguments, validation_deadline=time.monotonic() - 1.0
            )
        inspection = CompilerInspection(*arguments)
        payload = encode_compiler_inspection(inspection)
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            decode_compiler_inspection(
                payload, validation_deadline=time.monotonic() - 1.0
            )

    def test_windows_generation_observer_closes_partial_handles_on_setup_failure(self):
        class Callable:
            def __init__(self, values):
                self.values = iter(values)
                self.argtypes = None
                self.restype = None

            def __call__(self, *_args):
                return next(self.values)

        create = Callable((101, -1))
        closed = []
        close = Callable((True,))

        def close_record(handle):
            closed.append(handle)
            return close(handle)

        close_record.argtypes = None
        close_record.restype = None
        kernel32 = SimpleNamespace(CreateFileW=create, CloseHandle=close_record)
        observer = object.__new__(_FilesystemGenerationObserver)
        observer._handles = []
        observer._owner = None
        regular_metadata = SimpleNamespace(
            st_mode=0o100644, st_file_attributes=0
        )
        with mock.patch(
            "ctypes.WinDLL", return_value=kernel32
        ), mock.patch.object(
            Path, "lstat", return_value=regular_metadata
        ), self.assertRaisesRegex(AuditInfrastructureError, "setup failed"):
            observer._arm_windows(
                ((Path("C:/first"), False), (Path("C:/second"), False))
            )
        self.assertEqual(closed, [101])

    def test_linux_generation_observer_no_follows_and_removes_exact_watches(self):
        class Callable:
            def __init__(self, function):
                self.function = function
                self.argtypes = None
                self.restype = None

            def __call__(self, *arguments):
                return self.function(*arguments)

        added = []
        removed = []
        watch_ids = iter((41, 42))
        initialize = Callable(lambda _flags: 17)

        def add_watch(descriptor, path, mask):
            added.append((descriptor, path, mask))
            return next(watch_ids)

        add = Callable(add_watch)
        remove = Callable(
            lambda descriptor, watch: removed.append((descriptor, watch)) or 0
        )
        libc = SimpleNamespace(
            inotify_init1=initialize,
            inotify_add_watch=add,
            inotify_rm_watch=remove,
        )
        observer = object.__new__(_FilesystemGenerationObserver)
        observer._backend = "linux"
        observer._owner = None
        observer._handles = []
        observer._closed = False
        required_mask = (
            0x00000002 | 0x00000004 | 0x00000008
            | 0x00000040 | 0x00000080 | 0x00000100 | 0x00000200
            | 0x00000400 | 0x00000800 | 0x00002000 | 0x02000000
        )
        with mock.patch("ctypes.CDLL", return_value=libc), mock.patch(
            "gpu_capability_model.os.O_NONBLOCK", 0x800, create=True
        ), mock.patch(
            "gpu_capability_model.os.O_CLOEXEC", 0x80000, create=True
        ), mock.patch("gpu_capability_model.os.close") as close:
            observer._arm_linux(
                ((Path("/toolchain/lib.so"), False),
                 (Path("/toolchain/lib.so.1"), False))
            )
            self.assertEqual(
                [(descriptor, mask) for descriptor, _path, mask in added],
                [(17, required_mask), (17, required_mask)],
            )
            observer.close()
        self.assertEqual(removed, [(17, 42), (17, 41)])
        close.assert_called_once_with(17)

    def test_configuration_audit_result_is_compact_frozen_and_sorted(self):
        dependency = self.dependency()
        finding = AuditResultFinding(
            path=PurePosixPath("playback/gpu/example.cpp"),
            line=7,
            expression="nativeHandle()",
            reason="outside lease",
        )
        result = ConfigurationAuditResult(
            configuration_digest="c" * 64,
            audit_engine_fingerprint="a" * 64,
            dependencies=(dependency,),
            reached_production=(PurePosixPath("playback/gpu/example.cpp"),),
            findings=(finding,),
        )
        self.assertEqual(AUDIT_RESULT_SCHEMA_BYTES, b"olr-gpu-capability-audit-result-v1")
        self.assertFalse(hasattr(result, "tokens"))
        self.assertFalse(hasattr(result, "__dict__"))
        with self.assertRaises(dataclasses.FrozenInstanceError):
            result.configuration_digest = "other"

        valid_fields = {
            field.name: getattr(result, field.name)
            for field in dataclasses.fields(result)
        }
        invalid_cases = (
            {**valid_fields, "configuration_digest": "C" * 64},
            {**valid_fields, "dependencies": (dependency, dependency)},
            {
                **valid_fields,
                "reached_production": (
                    PurePosixPath("playback/z.cpp"),
                    PurePosixPath("playback/a.cpp"),
                ),
            },
            {**valid_fields, "findings": ({
                "path": finding.path,
                "line": True,
                "expression": finding.expression,
                "reason": finding.reason,
            },)},
        )
        for index, invalid in enumerate(invalid_cases):
            with self.subTest(index=index), self.assertRaises(AuditInfrastructureError):
                if index == 3:
                    bad_finding = AuditResultFinding(**invalid["findings"][0])
                    invalid = {**invalid, "findings": (bad_finding,)}
                ConfigurationAuditResult(**invalid)

    def test_streaming_aggregate_is_ordered_and_releases_result_ownership(self):
        configurations = (
            self.configuration(digest="1" * 64),
            self.configuration(digest="2" * 64),
        )
        finding = AuditResultFinding(
            PurePosixPath("playback/gpu/example.cpp"),
            7,
            "nativeHandle()",
            "outside lease",
        )
        results = tuple(
            ConfigurationAuditResult(
                configuration.digest,
                "a" * 64,
                (self.dependency(),),
                (PurePosixPath("playback/gpu/example.cpp"),),
                (finding,),
            )
            for configuration in configurations
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(configurations, budget, AuditLimits())
        for index in (1, 0):
            ownership = budget.reserve(
                compact_result_retained_bytes(results[index])
            ).commit()
            aggregator.accept_validated_result(
                configurations[index], results[index], ownership
            )
            self.assertTrue(ownership.released)
        summary = aggregator.finish()
        self.assertEqual(summary.configurations, ("1" * 64, "2" * 64))
        self.assertEqual(
            summary.findings[0].configurations, ("1" * 64, "2" * 64)
        )
        canonical = encode_canonical_summary(summary)
        self.assertEqual(canonical, encode_canonical_summary(summary))
        self.assertGreater(budget.committed_bytes, 0)
        summary.release()
        self.assertEqual(budget.live_bytes, 0)

    def test_streaming_checkpoint_restores_only_new_cold_ownership(self):
        configuration = self.configuration(digest="1" * 64)
        cold_slot = CompactResultColdSlot(1 << 20)

        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        baseline = budget.live_bytes
        checkpoint = aggregator._checkpoint()
        aggregator.reserve_cold_slot(cold_slot)
        aggregator._rollback_to(checkpoint)
        self.assertEqual(aggregator.cold_slot_reserved_bytes, 0)
        self.assertEqual(budget.live_bytes, baseline)

        aggregator.reserve_cold_slot(cold_slot)
        preexisting = aggregator._cold_ownership
        checkpoint = aggregator._checkpoint()
        aggregator.reserve_cold_slot(cold_slot)
        aggregator._rollback_to(checkpoint)
        self.assertIs(aggregator._cold_ownership, preexisting)
        self.assertEqual(
            aggregator.cold_slot_reserved_bytes, cold_slot.worst_case_live_bytes
        )

    def test_caller_can_retain_result_charge_until_aliases_are_cleared(self):
        configuration = self.configuration(digest="1" * 64)
        result = ConfigurationAuditResult(
            configuration.digest,
            "a" * 64,
            (self.dependency(),),
            (),
            (),
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        ownership = budget.reserve(
            compact_result_retained_bytes(result)
        ).commit()
        aggregator.accept_validated_result(
            configuration,
            result,
            ownership,
            caller_retains_ownership=True,
        )
        self.assertTrue(ownership.committed)
        ownership.release()
        self.assertTrue(ownership.released)

    def test_finish_reserves_immutable_copy_before_marking_finished(self):
        configuration = self.configuration(digest="1" * 64)
        result = ConfigurationAuditResult(
            configuration.digest,
            "a" * 64,
            (self.dependency(),),
            (PurePosixPath("playback/gpu/example.cpp"),),
            (
                AuditResultFinding(
                    PurePosixPath("playback/gpu/example.cpp"),
                    7,
                    "nativeHandle()",
                    "outside lease",
                ),
            ),
        )
        calibration_budget = CompactResultMemoryBudget()
        calibration = StreamingResultAggregator(
            (configuration,), calibration_budget, AuditLimits()
        )
        calibration.accept_validated_result(
            configuration,
            result,
            calibration_budget.reserve(
                compact_result_retained_bytes(result)
            ).commit(),
        )
        accepted_peak = calibration_budget.peak_live_bytes

        budget = CompactResultMemoryBudget(accepted_peak)
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        aggregator.accept_validated_result(
            configuration,
            result,
            budget.reserve(compact_result_retained_bytes(result)).commit(),
        )
        before = (
            aggregator.accepted_count,
            len(aggregator._reached),
            len(aggregator._findings),
            len(aggregator._digests),
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "final"):
            aggregator.finish()
        self.assertFalse(aggregator._finished)
        self.assertEqual(
            (
                aggregator.accepted_count,
                len(aggregator._reached),
                len(aggregator._findings),
                len(aggregator._digests),
            ),
            before,
        )

    def test_accept_validated_result_rolls_back_every_mutation_boundary(self):
        configuration = self.configuration(digest="1" * 64)
        result = ConfigurationAuditResult(
            configuration.digest,
            "a" * 64,
            (self.dependency(),),
            (PurePosixPath("playback/gpu/example.cpp"),),
            (
                AuditResultFinding(
                    PurePosixPath("playback/gpu/example.cpp"),
                    7,
                    "nativeHandle()",
                    "outside lease",
                ),
            ),
        )
        stages = ("accepted", "reached", "finding", "digest", "ownership")
        for stage in stages:
            with self.subTest(stage=stage):
                budget = CompactResultMemoryBudget()
                aggregator = StreamingResultAggregator(
                    (configuration,), budget, AuditLimits()
                )
                ownership = budget.reserve(
                    compact_result_retained_bytes(result)
                ).commit()
                before = (
                    aggregator.accepted_count,
                    tuple(aggregator._reached.items()),
                    tuple(aggregator._findings.items()),
                    tuple(aggregator._digests.items()),
                    len(aggregator._growth_ownerships),
                    budget.live_bytes,
                )

                def fail_at_boundary(observed):
                    if observed == stage:
                        raise RuntimeError(f"forced {stage} failure")

                with mock.patch(
                    "gpu_capability_model._after_streaming_aggregate_mutation",
                    side_effect=fail_at_boundary,
                    create=True,
                ), self.assertRaisesRegex(RuntimeError, f"forced {stage}"):
                    aggregator.accept_validated_result(
                        configuration, result, ownership
                    )
                self.assertEqual(
                    (
                        aggregator.accepted_count,
                        tuple(aggregator._reached.items()),
                        tuple(aggregator._findings.items()),
                        tuple(aggregator._digests.items()),
                        len(aggregator._growth_ownerships),
                        budget.live_bytes,
                    ),
                    before,
                )
                self.assertTrue(ownership.committed)
                aggregator.accept_validated_result(
                    configuration, result, ownership
                )
                self.assertTrue(ownership.released)

    def test_aggregate_growth_failure_occurs_before_insert(self):
        configuration = self.configuration(digest="1" * 64)
        result = ConfigurationAuditResult(
            configuration.digest,
            "a" * 64,
            (self.dependency(),),
            (),
            (),
        )
        retained = compact_result_retained_bytes(result)
        calibration_budget = CompactResultMemoryBudget()
        calibration = StreamingResultAggregator(
            (configuration,), calibration_budget, AuditLimits()
        )
        base_bytes = calibration_budget.committed_bytes
        del calibration
        budget = CompactResultMemoryBudget(base_bytes + retained + 4096)
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        ownership = budget.reserve(retained).commit()
        with self.assertRaisesRegex(AuditInfrastructureError, "before insert"):
            aggregator.accept_validated_result(configuration, result, ownership)
        self.assertEqual(aggregator.accepted_count, 0)
        ownership.release()

    def test_251_disjoint_growth_fails_before_final_insert(self):
        configurations = tuple(
            self.configuration(digest=f"{index:064x}")
            for index in range(251)
        )
        results = []
        for index, configuration in enumerate(configurations):
            relative = PurePosixPath(f"playback/disjoint/{index:04d}.h")
            identity = FileIdentity(
                Path("D:/repo") / Path(relative.as_posix()),
                relative,
                3,
                index + 100,
                1,
                True,
            )
            dependency = DependencyDigest(
                "production", relative, identity, f"{index + 1:064x}"
            )
            finding = AuditResultFinding(
                relative,
                1,
                f"surface{index}.nativeHandle()",
                f"disjoint lease {index}",
            )
            results.append(
                ConfigurationAuditResult(
                    configuration.digest,
                    "a" * 64,
                    (dependency,),
                    (relative,),
                    (finding,),
                )
            )
        results = tuple(results)

        calibration_budget = CompactResultMemoryBudget()
        calibration = StreamingResultAggregator(
            configurations, calibration_budget, AuditLimits()
        )
        for configuration, result in zip(
            configurations[:250], results[:250], strict=True
        ):
            calibration.accept_validated_result(
                configuration,
                result,
                calibration_budget.reserve(
                    compact_result_retained_bytes(result)
                ).commit(),
            )
        exact_limit = (
            calibration_budget.committed_bytes
            + compact_result_retained_bytes(results[250])
            + 4096
        )
        budget = CompactResultMemoryBudget(exact_limit)
        aggregator = StreamingResultAggregator(configurations, budget, AuditLimits())
        for configuration, result in zip(
            configurations[:250], results[:250], strict=True
        ):
            aggregator.accept_validated_result(
                configuration,
                result,
                budget.reserve(compact_result_retained_bytes(result)).commit(),
            )
        before = (
            aggregator.accepted_count,
            len(aggregator._reached),
            len(aggregator._findings),
            len(aggregator._digests),
        )
        ownership = budget.reserve(
            compact_result_retained_bytes(results[250])
        ).commit()
        with self.assertRaisesRegex(AuditInfrastructureError, "before insert"):
            aggregator.accept_validated_result(
                configurations[250], results[250], ownership
            )
        self.assertEqual(
            (
                aggregator.accepted_count,
                len(aggregator._reached),
                len(aggregator._findings),
                len(aggregator._digests),
            ),
            before,
        )
        ownership.release()
        self.assertLessEqual(budget.peak_live_bytes, 128 << 20)

    def test_251st_production_scale_result_fails_at_actual_128_mib_frontier(self):
        configurations = tuple(
            self.configuration(digest=f"{index:064x}")
            for index in range(251)
        )
        shared_expression = "x" * 58_800

        def result_for(index: int) -> ConfigurationAuditResult:
            findings = tuple(
                AuditResultFinding(
                    PurePosixPath(
                        f"playback/frontier/{index:04d}-{finding_index:02d}.cpp"
                    ),
                    1,
                    shared_expression,
                    f"frontier-{index:04d}-{finding_index:02d}",
                )
                for finding_index in range(9)
            )
            return ConfigurationAuditResult(
                configurations[index].digest,
                "a" * 64,
                (self.dependency(),),
                (),
                findings,
            )

        budget = CompactResultMemoryBudget(128 << 20)
        aggregator = StreamingResultAggregator(
            configurations, budget, AuditLimits()
        )
        for index in range(250):
            result = result_for(index)
            aggregator.accept_validated_result(
                configurations[index],
                result,
                budget.reserve(compact_result_retained_bytes(result)).commit(),
            )
        before = (
            aggregator.accepted_count,
            len(aggregator._findings),
            budget.committed_bytes,
        )
        final_result = result_for(250)
        final_ownership = budget.reserve(
            compact_result_retained_bytes(final_result)
        ).commit()
        with self.assertRaisesRegex(AuditInfrastructureError, "before insert"):
            aggregator.accept_validated_result(
                configurations[250], final_result, final_ownership
            )
        self.assertEqual(
            (
                aggregator.accepted_count,
                len(aggregator._findings),
                before[2],
            ),
            before,
        )
        final_ownership.release()
        self.assertGreater(budget.peak_live_bytes, 127 << 20)
        self.assertLessEqual(budget.peak_live_bytes, 128 << 20)

    def test_publication_permit_binds_complete_result_generation(self):
        dependency = self.dependency()
        permit = ConfigurationAuditPublicationPermit(
            "c" * 64, "a" * 64, (dependency,)
        )
        self.assertEqual(permit.dependencies, (dependency,))
        with self.assertRaises(AuditInfrastructureError):
            dataclasses.replace(permit, dependencies=(dependency, dependency))

    def test_worker_compact_reservation_and_root_permit_are_linear_and_bounded(self):
        inadequate = PerTaskCompactReservation("task-small", 7, (32 << 20) - 1)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "pre-dispatch compact reservation"
        ):
            inadequate.require_before_discovery("task-small", 7)
        reservation = PerTaskCompactReservation("task-a", 7, 32 << 20)
        reservation.require_before_discovery("task-a", 7)
        bounds = CompactResultDraftBounds(1, 1, 1, 64, 32, 48)
        charged = reservation.require_within_pre_dispatch_reservation(
            "task-a", 1024, bounds, 512
        )
        self.assertGreaterEqual(charged, 1536)
        self.assertLessEqual(charged, reservation.maximum_bytes)
        reservation.record_exact_canonical_json("task-a", 1024)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "pre-dispatch compact reservation"
        ):
            reservation.require_within_pre_dispatch_reservation(
                "different-task", 1024, bounds, 512
            )

        releases = []
        dependency = self.dependency()
        permit = CachePublicationPermit(
            "c" * 64,
            "a" * 64,
            (dependency,),
            task_id="task-a",
            generation=7,
            release_callback=lambda: releases.append("root"),
        )
        self.assertEqual(permit.root_publication_bytes, 8 << 20)
        permit.release_root_publication()
        self.assertEqual(releases, ["root"])
        self.assertTrue(permit.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "already released"):
            permit.release_root_publication()

        reservation.transfer_to_receiver_result("task-a", 7)
        owned_result = ConfigurationAuditResult(
            "c" * 64, "a" * 64, (dependency,), (), (), reservation
        )
        self.assertFalse(reservation.released)
        owned_result.release_transport_ownership()
        self.assertTrue(reservation.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "already released"):
            reservation.release("duplicate")

    def test_worker_outcome_and_launch_event_contracts_are_typed(self):
        result = ConfigurationAuditResult(
            "c" * 64, "a" * 64, (self.dependency(),), (), ()
        )
        stages = WorkerStageTimings(1.0, 2.0, 3.0, 4.0)
        outcome = ConfigurationAuditOutcome(result, 123, stages)
        self.assertEqual(
            tuple(field.name for field in dataclasses.fields(outcome)),
            ("result", "stdout_bytes", "stages"),
        )
        self.assertFalse(hasattr(outcome, "view"))

        identity = ProcessStartIdentity("windows", 42, "start-token", "cookie")
        event = CompilerLaunchEvent(
            CompilerLaunchPurpose.AUDIT_DISCOVERY,
            identity,
            worker_index=3,
            task_id="task-a",
            generation=7,
        )
        self.assertEqual(event.process_start, identity)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "audit compiler launch identity"
        ):
            dataclasses.replace(event, task_id=None)

    def test_dependency_digest_portable_key_and_exact_local_codec_are_separate(self):
        digest = self.dependency()
        relocated = dataclasses.replace(
            digest,
            identity=dataclasses.replace(
                digest.identity,
                canonical=Path("E:/relocated/playback/gpu/example.cpp"),
                device=99,
                inode=101,
            ),
        )
        self.assertEqual(portable_dependency_key(digest), portable_dependency_key(relocated))
        self.assertNotEqual(
            encode_local_dependency_digest(digest),
            encode_local_dependency_digest(relocated),
        )

        encoded = encode_local_dependency_digest(digest)
        pairs = json.loads(encoded.decode("ascii"), object_pairs_hook=list)
        self.assertEqual(tuple(key for key, _value in pairs), (
            "stable_role",
            "role_relative_path",
            "canonical",
            "relative",
            "device",
            "inode",
            "line_count",
            "production",
            "sha256",
        ))
        self.assertEqual(decode_local_dependency_digest(encoded), digest)

        mutations = {
            "missing-field": pairs[:-1],
            "unknown-field": pairs + [("unknown", 1)],
            "reordered-field": (pairs[1], pairs[0], *pairs[2:]),
            "wrong-type": tuple(
                (key, True if key == "line_count" else value)
                for key, value in pairs
            ),
            "replacement-identity": tuple(
                (key, 999 if key == "inode" else value)
                for key, value in pairs
            ),
        }
        for name, mutated_pairs in mutations.items():
            mutated = json.dumps(
                dict(mutated_pairs), ensure_ascii=True, separators=(",", ":")
            ).encode("ascii")
            with self.subTest(name=name):
                if name == "replacement-identity":
                    decoded = decode_local_dependency_digest(mutated)
                    self.assertNotEqual(decoded, digest)
                    with self.assertRaisesRegex(
                        AuditInfrastructureError, "identity was replaced"
                    ):
                        decode_local_dependency_digest(mutated, expected=digest)
                else:
                    with self.assertRaises(AuditInfrastructureError):
                        decode_local_dependency_digest(mutated)

        for invalid_path in ("playback/./a.h", "playback/../a.h", "playback//a.h"):
            mutated = dict(pairs)
            mutated["role_relative_path"] = invalid_path
            with self.subTest(invalid_path=invalid_path), self.assertRaises(
                AuditInfrastructureError
            ):
                decode_local_dependency_digest(
                    json.dumps(mutated, separators=(",", ":")).encode("ascii")
                )

        for invalid_canonical in (
            "",
            ".",
            "relative.cpp",
            "D:/repo/../repo/playback/gpu/example.cpp",
        ):
            mutated = dict(pairs)
            mutated["canonical"] = invalid_canonical
            with self.subTest(invalid_canonical=invalid_canonical), self.assertRaises(
                AuditInfrastructureError
            ):
                decode_local_dependency_digest(
                    json.dumps(mutated, separators=(",", ":")).encode("ascii")
                )

        for invalid_canonical in (
            Path("."),
            Path("relative.cpp"),
            Path("D:/repo/../repo/playback/gpu/example.cpp"),
        ):
            with self.subTest(direct_canonical=invalid_canonical), self.assertRaises(
                AuditInfrastructureError
            ):
                dataclasses.replace(
                    digest,
                    identity=dataclasses.replace(
                        digest.identity, canonical=invalid_canonical
                    ),
                )

    def test_native_canonical_path_spellings_are_exact_and_round_trip(self):
        digest = self.dependency()
        encoded = encode_local_dependency_digest(digest)
        decoded = decode_local_dependency_digest(encoded, expected=digest)
        self.assertEqual(encode_local_dependency_digest(decoded), encoded)

        pairs = json.loads(encoded.decode("ascii"), object_pairs_hook=list)
        malformed = (
            "C://repo/file.cpp",
            "C:\\\\repo\\file.cpp",
            "C:/repo/file.cpp/",
            "/repo/file.cpp/",
            "//a",
            "///repo/file.cpp",
            "C:repo/file.cpp",
            "C:/repo\\file.cpp",
            "C:/repo//file.cpp",
            "\\\\server\\share",
            "\\\\server\\\\share\\file.cpp",
            "\\server\\share\\file.cpp",
        )
        for spelling in malformed:
            mutated_pairs = tuple(
                (key, spelling if key == "canonical" else value)
                for key, value in pairs
            )
            payload = json.dumps(
                dict(mutated_pairs), ensure_ascii=True, separators=(",", ":")
            ).encode("ascii")
            with self.subTest(decode=spelling), self.assertRaises(
                AuditInfrastructureError
            ):
                decode_local_dependency_digest(payload, expected=digest)
            with self.subTest(raw_decode=spelling), self.assertRaises(
                AuditInfrastructureError
            ):
                decode_local_dependency_digest(payload)

            malformed_identity = dataclasses.replace(
                digest.identity, canonical=spelling
            )
            with self.subTest(direct=spelling), self.assertRaises(
                AuditInfrastructureError
            ):
                dataclasses.replace(digest, identity=malformed_identity)

        for spelling in (
            "/repo/playback/gpu/example.cpp",
            "C:/repo/playback/gpu/example.cpp",
            "//server/share/playback/gpu/example.cpp",
        ):
            with self.subTest(valid_raw=spelling):
                self.assertEqual(_validate_native_canonical_text(spelling), spelling)

        valid_spellings = [
            Path("C:/repo/playback/gpu/example.cpp"),
            Path("//server/share/playback/gpu/example.cpp"),
        ]
        if os.name != "nt":
            valid_spellings.append(Path("/repo/playback/gpu/example.cpp"))
        for canonical in valid_spellings:
            with self.subTest(valid=canonical):
                candidate = dataclasses.replace(
                    digest,
                    identity=dataclasses.replace(
                        digest.identity, canonical=canonical
                    ),
                )
                candidate_bytes = encode_local_dependency_digest(candidate)
                self.assertEqual(
                    encode_local_dependency_digest(
                        decode_local_dependency_digest(
                            candidate_bytes, expected=candidate
                        )
                    ),
                    candidate_bytes,
                )

        noncanonical_json = json.dumps(
            dict(pairs), ensure_ascii=True, indent=1
        ).encode("ascii")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "canonical local dependency payload"
        ):
            decode_local_dependency_digest(noncanonical_json, expected=digest)

    def test_compact_result_model_rejects_invalid_paths_roles_and_ordering(self):
        identity = self.identity()
        invalid_dependencies = (
            dict(stable_role="", role_relative_path=PurePosixPath("playback/a.h")),
            dict(stable_role="production", role_relative_path=PurePosixPath("../a.h")),
            dict(stable_role="production", role_relative_path=PurePosixPath("sdk/a.h")),
            dict(stable_role="production", role_relative_path=PurePosixPath("playback/a.txt")),
        )
        for fields in invalid_dependencies:
            with self.subTest(fields=fields), self.assertRaises(AuditInfrastructureError):
                DependencyDigest(identity=identity, sha256="a" * 64, **fields)

        first = self.dependency(sha256="a" * 64)
        second = dataclasses.replace(
            first,
            role_relative_path=PurePosixPath("playback/gpu/another.h"),
            identity=dataclasses.replace(
                first.identity,
                canonical=Path("D:/repo/playback/gpu/another.h"),
                relative=PurePosixPath("playback/gpu/another.h"),
            ),
        )
        self.assertGreater(portable_dependency_key(first), portable_dependency_key(second))
        with self.assertRaises(AuditInfrastructureError):
            ConfigurationAuditResult(
                configuration_digest="c" * 64,
                audit_engine_fingerprint="a" * 64,
                dependencies=(first, second),
                reached_production=(),
                findings=(),
            )

    def test_dependency_order_uses_role_path_and_digest_tuple_not_framed_bytes(self):
        z_dependency = DependencyDigest(
            stable_role="production",
            role_relative_path=PurePosixPath("playback/z.h"),
            identity=self.identity("playback/z.h"),
            sha256="a" * 64,
        )
        aa_dependency = DependencyDigest(
            stable_role="production",
            role_relative_path=PurePosixPath("playback/aa.h"),
            identity=self.identity("playback/aa.h"),
            sha256="a" * 64,
        )
        self.assertLess(portable_dependency_key(z_dependency), portable_dependency_key(aa_dependency))
        with self.assertRaisesRegex(AuditInfrastructureError, "unique and sorted"):
            ConfigurationAuditResult(
                configuration_digest="c" * 64,
                audit_engine_fingerprint="a" * 64,
                dependencies=(z_dependency, aa_dependency),
                reached_production=(),
                findings=(),
            )

    def test_local_dependency_codec_wraps_every_malformed_pair_shape(self):
        malformed_payloads = (
            b"[1]",
            b"[[]]",
            b'[["stable_role"]]',
            b'[["stable_role","production","extra"]]',
            b'{"stable_role":"production"}',
        )
        for payload in malformed_payloads:
            with self.subTest(payload=payload), self.assertRaises(
                AuditInfrastructureError
            ):
                decode_local_dependency_digest(payload)

    def test_local_dependency_codec_wraps_deep_json_recursion(self):
        payload = b"[" * 5000 + b"0" + b"]" * 5000
        with self.assertRaises(AuditInfrastructureError):
            decode_local_dependency_digest(payload)

    def test_private_owned_packed_construction_avoids_copy_reserve_and_stays_read_only(self):
        configuration = self.configuration()
        limits = dataclasses.replace(AuditLimits(), rss_bytes=100_000)
        columns = tuple(array("I", (0,)) for _ in range(4))
        with self.assertRaisesRegex(AuditInfrastructureError, "RSS limit"):
            CompactTokenSequence._from_packed(
                configuration,
                spellings=(b"lease",),
                identities=(configuration.source,),
                spelling_ids=columns[0],
                identity_ids=columns[1],
                inclusion_ids=columns[2],
                original_lines=columns[3],
                limits=limits,
                rss_reader=lambda: 96_100,
            )
        owned = CompactTokenSequence._from_owned_packed(
            configuration,
            spellings=(b"lease",),
            identities=(configuration.source,),
            spelling_ids=array("I", (0,)),
            identity_ids=array("I", (0,)),
            inclusion_ids=array("I", (0,)),
            original_lines=array("I", (1,)),
            limits=limits,
            rss_reader=lambda: 96_100,
        )
        self.assertEqual(owned[0].spelling, b"lease")
        with self.assertRaises(TypeError):
            owned._packed_columns()[0][0] = 1

    def test_limits_are_exact(self):
        limits = AuditLimits()
        self.assertEqual((limits.response_depth, limits.response_files), (8, 32))
        self.assertEqual(limits.response_bytes, 4 * 1024 * 1024)
        self.assertEqual((limits.invocation_seconds, limits.total_seconds), (60.0, 180.0))
        self.assertEqual(limits.stdout_bytes, 128 * 1024 * 1024)
        self.assertEqual(limits.stderr_bytes, 1024 * 1024)
        self.assertEqual(limits.retained_token_bytes, 384 * 1024 * 1024)
        self.assertEqual(limits.rss_bytes, 512 * 1024 * 1024)
        self.assertEqual(limits.workers, min(8, os.cpu_count() or 1))

    def test_compiler_family_values_are_stable(self):
        self.assertEqual(
            tuple(family.value for family in CompilerFamily),
            ("gcc", "clang", "msvc", "clang-cl"),
        )

    def test_shared_records_are_frozen_and_token_views_are_slotted(self):
        configuration = self.configuration()
        location = SourceLocation(configuration.source, 2, 11, configuration.digest)
        token = PreprocessedToken(b"lease", location)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            token.spelling = b"surface"
        with self.assertRaises(AttributeError):
            token.retained_state = object()
        self.assertFalse(hasattr(token, "__dict__"))
        self.assertFalse(hasattr(location, "__dict__"))

        view = PreprocessedTranslationUnitView(
            configuration,
            CompactTokenSequence.empty(configuration),
            (configuration.source,),
        )
        coverage = CoverageReport(
            frozenset({configuration.source.relative}),
            frozenset(),
            (configuration.digest,),
        )
        with self.assertRaises(dataclasses.FrozenInstanceError):
            view.dependencies = ()
        with self.assertRaises(dataclasses.FrozenInstanceError):
            coverage.configurations = ()

    def test_compact_sequence_does_not_retain_token_views(self):
        sequence = CompactTokenSequence.empty(self.configuration(digest="cfg-a"))
        self.assertIsInstance(sequence, Sequence)
        self.assertNotIsInstance(sequence, tuple)
        self.assertEqual(len(sequence), 0)
        self.assertEqual(sequence.packed_bytes, 0)
        self.assertEqual(list(sequence), [])
        self.assertEqual(list(sequence.iter_runs()), [])

    def test_compact_sequence_uses_exactly_four_four_byte_columns(self):
        sequence = self.sequence()
        columns = sequence._packed_columns()
        self.assertEqual(len(columns), 4)
        storage = tuple(
            object.__getattribute__(sequence, name)
            for name in (
                "_spelling_ids",
                "_identity_ids",
                "_inclusion_ids",
                "_original_lines",
            )
        )
        self.assertTrue(all(isinstance(column, array) for column in storage))
        self.assertTrue(all(column.typecode == "I" for column in storage))
        self.assertTrue(
            all(not isinstance(column, (array, memoryview)) for column in columns)
        )
        self.assertTrue(all(column.readonly for column in columns))
        self.assertTrue(all(column.format == "I" for column in columns))
        self.assertTrue(all(column.itemsize == 4 for column in columns))
        self.assertEqual(sequence.packed_bytes, len(sequence) * 4 * 4)

    def test_compact_sequence_owns_read_only_packed_storage(self):
        configuration = self.configuration()
        spelling_ids = array("I", (0,))
        identity_ids = array("I", (0,))
        inclusion_ids = array("I", (7,))
        original_lines = array("I", (19,))
        sequence = CompactTokenSequence._from_packed(
            configuration,
            spellings=(b"lease", b"nativeHandle"),
            identities=(configuration.source, None),
            spelling_ids=spelling_ids,
            identity_ids=identity_ids,
            inclusion_ids=inclusion_ids,
            original_lines=original_lines,
        )

        spelling_ids[0] = 1
        identity_ids[0] = 1
        inclusion_ids[0] = 99
        original_lines[0] = 99

        token = sequence[0]
        self.assertEqual(token.spelling, b"lease")
        self.assertEqual(token.location.identity, configuration.source)
        self.assertEqual(token.location.inclusion_instance, 7)
        self.assertEqual(token.location.line, 19)
        for column in sequence._packed_columns():
            with self.assertRaises(TypeError):
                column[0] = 0
            column.release()
        self.assertEqual(sequence[0], token)

        direct = sequence._spelling_ids
        self.assertNotIsInstance(direct, (array, memoryview))
        with self.assertRaises(TypeError):
            direct[0] = 1
        with self.assertRaises(TypeError):
            del direct[0]
        with self.assertRaises(TypeError):
            memoryview(direct)

        packed = sequence._packed_columns()
        self.assertTrue(
            all(not isinstance(column, (array, memoryview)) for column in packed)
        )
        self.assertTrue(all(column.readonly for column in packed))
        packed[0].release()
        self.assertEqual(sequence[0], token)
        self.assertEqual(sequence._packed_columns()[0][0], 0)

        with self.assertRaises(AttributeError):
            sequence._spelling_ids = array("I", (0,))
        with self.assertRaises(AttributeError):
            del sequence._spelling_ids
        with self.assertRaises(AttributeError):
            sequence._frozen = False
        with self.assertRaises(AttributeError):
            del sequence._frozen
        self.assertEqual(sequence[0], token)

    def test_compact_sequence_counts_intern_tables_at_exact_retained_boundary(self):
        configuration = self.configuration()
        spellings = (b"lease",)
        identities = (configuration.source,)
        table_bytes = (
            sum(len(spelling) for spelling in spellings)
            + (len(spellings) + len(identities))
            * (sys.getsizeof((None,)) - sys.getsizeof(()))
        )
        fields = dict(
            spellings=spellings,
            identities=identities,
            spelling_ids=array("I"),
            identity_ids=array("I"),
            inclusion_ids=array("I"),
            original_lines=array("I"),
        )
        exact = dataclasses.replace(
            AuditLimits(), retained_token_bytes=table_bytes, rss_bytes=1_000_000
        )

        sequence = CompactTokenSequence._from_packed(
            configuration, **fields, limits=exact, rss_reader=lambda: 0
        )
        self.assertEqual(len(sequence), 0)

        with self.assertRaisesRegex(
            AuditInfrastructureError, "retained packed token limit"
        ):
            CompactTokenSequence._from_packed(
                configuration,
                **fields,
                limits=dataclasses.replace(exact, retained_token_bytes=table_bytes - 1),
                rss_reader=lambda: 0,
            )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "retained packed token limit"
        ):
            CompactTokenSequence._from_packed(
                configuration,
                **fields,
                limits=dataclasses.replace(exact, retained_token_bytes=0),
                rss_reader=lambda: 0,
            )

    def test_compact_sequence_checks_rss_before_uniqueness_allocation(self):
        configuration = self.configuration()
        spellings = tuple(f"token-{index}".encode() for index in range(1000))
        with mock.patch("builtins.set", side_effect=AssertionError("set allocated")) as make_set:
            with self.assertRaisesRegex(
                AuditInfrastructureError, "coordinator RSS limit"
            ):
                CompactTokenSequence._from_packed(
                    configuration,
                    spellings=spellings,
                    identities=(),
                    spelling_ids=array("I"),
                    identity_ids=array("I"),
                    inclusion_ids=array("I"),
                    original_lines=array("I"),
                    limits=dataclasses.replace(AuditLimits(), rss_bytes=1),
                    rss_reader=lambda: 0,
                )
            make_set.assert_not_called()

    def test_compact_sequence_samples_rss_during_construction(self):
        configuration = self.configuration()
        samples = iter((0, 0, 1_000_001))
        with self.assertRaisesRegex(AuditInfrastructureError, "coordinator RSS limit"):
            CompactTokenSequence._from_packed(
                configuration,
                spellings=(b"lease",),
                identities=(),
                spelling_ids=array("I"),
                identity_ids=array("I"),
                inclusion_ids=array("I"),
                original_lines=array("I"),
                limits=dataclasses.replace(AuditLimits(), rss_bytes=1_000_000),
                rss_reader=lambda: next(samples),
            )

    def test_compact_sequence_rejects_copy_before_crossing_memory_bounds(self):
        configuration = self.configuration()
        fields = dict(
            spellings=(b"lease",),
            identities=(configuration.source,),
            spelling_ids=array("I", (0, 0)),
            identity_ids=array("I", (0, 0)),
            inclusion_ids=array("I", (7, 7)),
            original_lines=array("I", (19, 19)),
        )
        packed_bytes = 2 * 4 * 4
        table_bytes = (
            sum(len(spelling) for spelling in fields["spellings"])
            + (len(fields["spellings"]) + len(fields["identities"]))
            * (sys.getsizeof((None,)) - sys.getsizeof(()))
        )
        retained_bytes = packed_bytes + table_bytes
        exact_limits = dataclasses.replace(
            AuditLimits(),
            retained_token_bytes=retained_bytes,
            rss_bytes=1_000_000,
        )
        sequence = CompactTokenSequence._from_packed(
            configuration,
            **fields,
            limits=exact_limits,
            rss_reader=lambda: 0,
        )
        self.assertEqual(sequence.packed_bytes, packed_bytes)

        with mock.patch(
            "gpu_capability_model._copy_packed_column", create=True
        ) as copy_column:
            with self.assertRaisesRegex(
                AuditInfrastructureError, "coordinator RSS limit"
            ):
                CompactTokenSequence._from_packed(
                    configuration,
                    **fields,
                    limits=dataclasses.replace(exact_limits, rss_bytes=1),
                    rss_reader=lambda: 0,
                )
            copy_column.assert_not_called()

        retained_too_small = dataclasses.replace(
            exact_limits, retained_token_bytes=retained_bytes - 1
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "retained packed token limit"
        ):
            CompactTokenSequence._from_packed(
                configuration,
                **fields,
                limits=retained_too_small,
                rss_reader=lambda: 0,
            )

    def test_compact_sequence_materializes_views_only_on_demand(self):
        sequence = self.sequence()
        first = sequence[0]
        self.assertIsInstance(first, PreprocessedToken)
        self.assertEqual(first.spelling, b"lease")
        self.assertEqual(first.location.identity.relative, PurePosixPath("playback/gpu/example.cpp"))
        self.assertEqual(first.location.inclusion_instance, 4)
        self.assertEqual(first.location.line, 12)
        self.assertEqual(first.location.configuration_digest, "cfg-a")
        self.assertIsNot(first, sequence[0])
        self.assertIsNot(first.location, sequence[0].location)
        self.assertEqual(sequence[-1].location.line, 20)
        self.assertEqual([token.spelling for token in sequence[1:3]], [b".", b"nativeHandle"])

        for slot in sequence.__slots__:
            retained = getattr(sequence, slot)
            self.assertNotIsInstance(retained, (PreprocessedToken, SourceLocation))
            if isinstance(retained, tuple):
                self.assertFalse(any(isinstance(value, (PreprocessedToken, SourceLocation)) for value in retained))

    def test_compact_sequence_exposes_packed_lookups_and_runs(self):
        sequence = self.sequence()
        self.assertEqual(sequence.spelling_id_at(2), 2)
        self.assertEqual(sequence.spelling_id_at(-1), 0)
        self.assertEqual(sequence.spelling_for(2), b"nativeHandle")
        self.assertIsNone(sequence.identity_for(0))
        self.assertEqual(sequence.identity_for(1).relative, PurePosixPath("playback/gpu/example.cpp"))
        self.assertEqual(
            tuple(sequence.iter_runs()),
            (
                PackedTokenRun(0, 6, 1, 4, 12),
                PackedTokenRun(6, 7, 1, 5, 20),
            ),
        )

    def test_compact_sequence_rejects_invalid_columns_and_ids(self):
        configuration = self.configuration()
        valid = dict(
            spellings=(b"x",),
            identities=(None,),
            spelling_ids=array("I", (0,)),
            identity_ids=array("I", (0,)),
            inclusion_ids=array("I", (0,)),
            original_lines=array("I", (1,)),
        )
        for name in ("identity_ids", "inclusion_ids", "original_lines"):
            broken = dict(valid)
            broken[name] = array("I")
            with self.subTest(name=name), self.assertRaisesRegex(
                AuditInfrastructureError, "packed token columns have different lengths"
            ):
                CompactTokenSequence._from_packed(configuration, **broken)

        broken = dict(valid)
        broken["spelling_ids"] = array("I", (1,))
        with self.assertRaisesRegex(AuditInfrastructureError, "spelling ID"):
            CompactTokenSequence._from_packed(configuration, **broken)
        broken = dict(valid)
        broken["identity_ids"] = array("I", (1,))
        with self.assertRaisesRegex(AuditInfrastructureError, "identity ID"):
            CompactTokenSequence._from_packed(configuration, **broken)

    def test_compact_sequence_rejects_uint32_overflow_before_array_append(self):
        configuration = self.configuration()
        for row, label in (
            ((UINT32_MAX + 1, 0, 0, 1), "spelling ID"),
            ((0, UINT32_MAX + 1, 0, 1), "identity ID"),
            ((0, 0, UINT32_MAX + 1, 1), "inclusion instance"),
            ((0, 0, 0, UINT32_MAX + 1), "original line"),
            ((-1, 0, 0, 1), "spelling ID"),
        ):
            with self.subTest(label=label), self.assertRaisesRegex(
                AuditInfrastructureError, f"{label}.*32-bit"
            ):
                CompactTokenSequence._from_token_fields(
                    configuration,
                    spellings=(b"x",),
                    identities=(None,),
                    fields=(row,),
                )

    def test_platform_classifier_is_path_based(self):
        families = frozenset({CompilerFamily.GCC})
        generic = (
            "playback/a.c",
            "playback/a.cc",
            "playback/a.cpp",
            "recorder_engine/a.cxx",
        )
        for path in generic:
            with self.subTest(path=path):
                self.assertTrue(requires_compile_entry(PurePosixPath(path), families, False, False))

        self.assertFalse(
            requires_compile_entry(
                PurePosixPath("playback/gpu/gpurhicontext_apple.mm"), families, False, False
            )
        )
        self.assertTrue(
            requires_compile_entry(
                PurePosixPath("playback/gpu/gpurhicontext_apple.mm"), families, True, False
            )
        )
        self.assertFalse(
            requires_compile_entry(
                PurePosixPath("playback/gpu/adapter_apple.cpp"), families, False, False
            )
        )
        self.assertTrue(
            requires_compile_entry(
                PurePosixPath("playback/gpu/adapter_apple.cpp"), families, True, False
            )
        )
        for path in (
            "playback/output/win/wingpuimportedge.cpp",
            "playback/output/adapter_win.cpp",
            "recorder_engine/codec/nativevideoencoder_mediafoundation.cpp",
        ):
            with self.subTest(path=path):
                posix = PurePosixPath(path)
                self.assertFalse(requires_compile_entry(posix, families, False, False))
                self.assertTrue(requires_compile_entry(posix, families, False, True))

        for path in ("playback/a.h", "playback/a.hpp", "playback/a.txt"):
            with self.subTest(path=path):
                self.assertFalse(
                    requires_compile_entry(PurePosixPath(path), families, True, True)
                )
        self.assertFalse(
            requires_compile_entry(PurePosixPath("playback/a.cpp"), frozenset(), False, False)
        )


class ProductionIdentityTests(unittest.TestCase):
    def write(self, root: Path, relative: str, content: bytes) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_enumerates_canonical_regular_utf8_production_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = self.write(root, "playback/a.cpp", b"one\ntwo\n")
            self.write(root, "recorder_engine/include/b.hpp", "alpha\nbeta".encode("utf-8"))
            self.write(root, "playback/ignored.txt", b"not production source")

            identities = enumerate_production_identities(root)

            self.assertEqual(
                tuple(identities),
                (PurePosixPath("playback/a.cpp"), PurePosixPath("recorder_engine/include/b.hpp")),
            )
            identity = identities[PurePosixPath("playback/a.cpp")]
            self.assertEqual(identity.canonical, first.resolve())
            self.assertEqual(identity.relative, PurePosixPath("playback/a.cpp"))
            self.assertTrue(identity.production)
            self.assertEqual(identity.line_count, 2)
            self.assertIsInstance(identity.device, (int, type(None)))
            self.assertIsInstance(identity.inode, (int, type(None)))
            self.assertEqual(identities[PurePosixPath("recorder_engine/include/b.hpp")].line_count, 2)

    def test_enumeration_does_not_follow_exists_when_checking_production_roots(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(root, "playback/a.cpp", b"int x;\n")
            with mock.patch.object(Path, "exists", return_value=False):
                identities = enumerate_production_identities(root)
            self.assertIn(PurePosixPath("playback/a.cpp"), identities)

    def test_rejects_source_root_symlink_before_following_it(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            real_root = parent / "real"
            self.write(real_root, "playback/a.cpp", b"int x;\n")
            alias = parent / "alias"
            try:
                alias.symlink_to(real_root, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks unavailable: {error}")

            with self.assertRaisesRegex(AuditInfrastructureError, "source root.*symlink"):
                enumerate_production_identities(alias)

    @unittest.skipUnless(os.name == "nt", "requires Windows directory junctions")
    def test_rejects_source_root_junction_before_following_it(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            real_root = parent / "real"
            self.write(real_root, "playback/a.cpp", b"int x;\n")
            alias = parent / "alias"
            subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(alias), str(real_root)],
                check=True,
                capture_output=True,
            )
            try:
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "source root.*reparse"
                ):
                    enumerate_production_identities(alias)
            finally:
                if alias.exists():
                    os.rmdir(alias)

    @unittest.skipUnless(os.name == "nt", "requires Windows directory junctions")
    def test_rejects_junction_ancestor_of_source_root_before_following_it(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            real_parent = parent / "real"
            real_root = real_parent / "repo"
            self.write(real_root, "playback/a.cpp", b"int x;\n")
            alias = parent / "alias"
            subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(alias), str(real_parent)],
                check=True,
                capture_output=True,
            )
            try:
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "source root.*reparse"
                ):
                    enumerate_production_identities(alias / "repo")
            finally:
                if alias.exists():
                    os.rmdir(alias)

    def test_rejects_unreadable_utf8_with_relative_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(root, "playback/bad.cpp", b"valid\n\xff\n")
            with self.assertRaisesRegex(
                AuditInfrastructureError, r"playback/bad\.cpp.*UTF-8"
            ):
                enumerate_production_identities(root)

    def test_rejects_duplicate_filesystem_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = self.write(root, "playback/original.cpp", b"int x;\n")
            alias = root / "recorder_engine/alias.cpp"
            alias.parent.mkdir(parents=True)
            try:
                os.link(original, alias)
            except OSError as error:
                self.skipTest(f"hard links unavailable: {error}")
            with self.assertRaisesRegex(
                AuditInfrastructureError, r"recorder_engine/alias\.cpp.*aliases.*playback/original\.cpp"
            ):
                enumerate_production_identities(root)

    def test_walk_rejects_symlink_without_following_it(self):
        class FakeEntry:
            name = "alias.cpp"

            def __init__(self, path: Path):
                self.path = str(path)

            @staticmethod
            def is_symlink() -> bool:
                return True

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            production = root / "playback"
            production.mkdir()
            alias = production / "alias.cpp"
            metadata = SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
            with mock.patch(
                "gpu_capability_model.os.scandir", return_value=[FakeEntry(alias)]
            ), mock.patch("gpu_capability_model.os.stat", return_value=metadata):
                with self.assertRaisesRegex(
                    AuditInfrastructureError, r"playback/alias\.cpp.*symlink"
                ):
                    list(_walk_production_entries(production, root))

    def test_rejects_non_regular_production_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fifo = self.write(root, "playback/pipe.cpp", b"")
            fake_metadata = SimpleNamespace(st_mode=stat.S_IFIFO, st_dev=1, st_ino=2)
            with self.assertRaisesRegex(
                AuditInfrastructureError, r"playback/pipe\.cpp.*regular file"
            ):
                with mock.patch(
                    "gpu_capability_model._walk_production_entries",
                    return_value=iter(((fifo, fake_metadata),)),
                ):
                    enumerate_production_identities(root)

    def test_walk_rejects_reparse_points_without_following_them(self):
        class FakeEntry:
            name = "alias.cpp"

            def __init__(self, path: Path):
                self.path = str(path)

            @staticmethod
            def is_symlink() -> bool:
                return False

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            production = root / "playback"
            production.mkdir()
            alias = production / "alias.cpp"
            metadata = SimpleNamespace(st_mode=stat.S_IFREG, st_file_attributes=0x400)
            with mock.patch(
                "gpu_capability_model.os.scandir", return_value=[FakeEntry(alias)]
            ), mock.patch("gpu_capability_model.os.stat", return_value=metadata):
                with self.assertRaisesRegex(
                    AuditInfrastructureError, r"playback/alias\.cpp.*reparse"
                ):
                    list(_walk_production_entries(production, root))

    def test_casefold_collision_logic_names_both_paths(self):
        seen: dict[str, PurePosixPath] = {}
        _check_casefold_collision(PurePosixPath("playback/Gpu/File.cpp"), seen)
        with self.assertRaisesRegex(
            AuditInfrastructureError,
            r"playback/gpu/file\.cpp.*case.*playback/Gpu/File\.cpp",
        ):
            _check_casefold_collision(PurePosixPath("playback/gpu/file.cpp"), seen)


if __name__ == "__main__":
    unittest.main()
