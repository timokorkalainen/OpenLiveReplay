from __future__ import annotations

import ast
import contextlib
import dataclasses
import gc
import hashlib
import inspect
import json
import multiprocessing
import os
import shutil
import socket
import subprocess
import struct
import sys
import tempfile
import threading
import time
import unittest
import weakref
from array import array
from collections.abc import Mapping
from pathlib import Path, PurePosixPath
from types import MappingProxyType, SimpleNamespace
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_capability_command as capability_command  # noqa: E402
import gpu_capability_cache as capability_cache  # noqa: E402
import gpu_capability_calibration as capability_calibration  # noqa: E402
import gpu_capability_model as capability_model  # noqa: E402
import gpu_capability_process_tree as capability_process_tree  # noqa: E402
import gpu_capability_runner as capability_runner  # noqa: E402
import gpu_capability_source_audit as capability_audit  # noqa: E402
from gpu_capability_command import (  # noqa: E402
    RewrittenCommand,
    _clear_compiler_inspection_memo_for_tests,
    _environment_digest,
    open_compiler_executable_capability,
)
from gpu_capability_cache import (  # noqa: E402
    CompilerInspectionCache,
    ConfigurationAuditCache,
    ConfigurationAuditLoadBatch,
    PreprocessCache,
)
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CanonicalAuditContentSummary,
    CompilerAuditRun,
    CompactResultColdSlot,
    CompactResultMemoryBudget,
    CompactTokenSequence,
    CompilerFamily,
    DependencyDigest,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    StreamingResultAggregator,
    build_dependency_root_authority,
    _FilesystemGenerationObserver,
)
from gpu_capability_provenance import PreprocessedStreamBuilder  # noqa: E402
from gpu_capability_reference_fixture import (  # noqa: E402
    reference_preprocess_all as preprocess_all,
)
from gpu_capability_runner import (  # noqa: E402
    ExecutionResult,
    _WindowsJob,
    collect_configurations,
    collect_configurations_with_decision_records,
    compute_active_sources,
    decode_validated_production_sources,
    discover_configuration,
    load_or_preprocess,
    preprocess_configuration,
    run_bounded_preprocessor,
    snapshot_production_sources,
    stabilize_and_parse_configuration,
)


class BoundedPreprocessorTests(unittest.TestCase):
    def test_runner_module_has_no_optimization_sensitive_assertions(self):
        path = Path(__file__).resolve().with_name("gpu_capability_runner.py")
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=path.name)
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(tree)))

    def test_task8_production_descriptor_is_no_follow_and_bound_before_scheduling(self):
        opener = inspect.getsource(capability_runner._open_production_descriptor)
        self.assertIn("os.O_NOFOLLOW", opener)
        self.assertIn("0x00200000", opener)
        self.assertIn("path.lstat()", opener)
        snapshot = inspect.getsource(capability_runner.HeldProductionSnapshot.__init__)
        self.assertLess(
            snapshot.index("_open_production_descriptor(identity.canonical)"),
            snapshot.index("self._files.append("),
        )
        enumeration = inspect.getsource(
            capability_model.enumerate_production_identities
        )
        self.assertNotIn("read_bytes()", enumeration)
        self.assertNotIn('.decode("utf-8"', enumeration)
        self.assertIn("production_raw_per_file_bytes", enumeration)
        self.assertIn("production_raw_aggregate_bytes", enumeration)
        self.assertIn("pipeline_deadline", enumeration)

    def test_task8_enumeration_rejects_walk_to_open_swap_and_closes_stream(self):
        replacement = self.root / "playback" / "replacement.cpp"
        replacement.write_text("int replacement;\n", encoding="utf-8")
        walked = self.source.lstat()
        opened = []

        def swapped_open(_path):
            stream = replacement.open("rb")
            opened.append(stream)
            return stream, os.fstat(stream.fileno())

        with mock.patch.object(
            capability_model,
            "_walk_production_entries",
            return_value=iter(((self.source, walked),)),
        ), mock.patch.object(
            capability_model,
            "_open_enumerated_production_file",
            side_effect=swapped_open,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "generation differs"
        ):
            capability_model.enumerate_production_identities(
                self.root, AuditLimits(), time.monotonic() + 10.0
            )
        self.assertEqual(len(opened), 1)
        self.assertTrue(opened[0].closed)

    def test_task8_enumeration_streams_ignored_entries_under_a_hard_cap(self):
        directory = self.root / "playback"
        self.source.unlink()
        for index in range(20):
            (directory / f"ignored-{index:02d}.txt").write_text(
                "ignored", encoding="utf-8"
            )
        with os.scandir(directory) as iterator:
            entries = tuple(iterator)

        class BoundedScandir:
            def __init__(self):
                self.index = 0
                self.closed = False

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                self.close()

            def __iter__(self):
                return self

            def __next__(self):
                if self.index >= len(entries):
                    raise StopIteration
                value = entries[self.index]
                self.index += 1
                return value

            def close(self):
                self.closed = True

        carrier = BoundedScandir()
        limits = dataclasses.replace(AuditLimits(), unique_dependency_handles=4)
        with mock.patch.object(
            capability_model.os, "scandir", return_value=carrier
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "traversal entry count"
        ):
            capability_model.enumerate_production_identities(
                self.root, limits, time.monotonic() + 10.0
            )
        self.assertLessEqual(carrier.index, 5)
        self.assertTrue(carrier.closed)

    def test_task8_enumeration_raw_caps_are_exact_and_close_every_handle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            playback = root / "playback"
            playback.mkdir()
            exact = playback / "exact.cpp"
            with exact.open("wb") as stream:
                stream.truncate(8 << 20)
            deadline = time.monotonic() + 30.0
            with mock.patch.object(
                capability_model,
                "_scan_production_utf8_lines",
                return_value=1,
            ) as scan:
                table = capability_model.enumerate_production_identities(
                    root, AuditLimits(), deadline
                )
            self.assertIn(PurePosixPath("playback/exact.cpp"), table)
            scan.assert_called_once()
            table.close()

            with exact.open("ab") as stream:
                stream.write(b"x")
            with mock.patch.object(
                capability_model, "_scan_production_utf8_lines"
            ) as scan, self.assertRaisesRegex(
                AuditInfrastructureError, "per-file limit"
            ):
                capability_model.enumerate_production_identities(
                    root, AuditLimits(), time.monotonic() + 30.0
                )
            scan.assert_not_called()

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            playback = root / "playback"
            playback.mkdir()
            for index in range(8):
                with (playback / f"aggregate-{index}.cpp").open("wb") as stream:
                    stream.truncate(8 << 20)
            opened = []
            original_open = capability_model._open_enumerated_production_file

            def tracking_open(path):
                stream, metadata = original_open(path)
                opened.append(stream)
                return stream, metadata

            with mock.patch.object(
                capability_model,
                "_open_enumerated_production_file",
                side_effect=tracking_open,
            ), mock.patch.object(
                capability_model,
                "_scan_production_utf8_lines",
                return_value=1,
            ):
                table = capability_model.enumerate_production_identities(
                    root, AuditLimits(), time.monotonic() + 30.0
                )
            self.assertEqual(len(table), 8)
            self.assertTrue(all(stream.closed for stream in opened))
            table.close()

            (playback / "aggregate-plus-one.cpp").write_bytes(b"x")
            opened.clear()
            with mock.patch.object(
                capability_model,
                "_open_enumerated_production_file",
                side_effect=tracking_open,
            ), mock.patch.object(
                capability_model,
                "_scan_production_utf8_lines",
                return_value=1,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "aggregate limit"
            ):
                capability_model.enumerate_production_identities(
                    root, AuditLimits(), time.monotonic() + 30.0
                )
            self.assertTrue(opened)
            self.assertTrue(all(stream.closed for stream in opened))

    def test_task8_enumeration_open_is_no_follow_on_each_platform(self):
        if os.name == "nt":
            import ctypes

            create_file = mock.Mock(
                return_value=ctypes.c_void_p(-1)
            )
            kernel32 = SimpleNamespace(
                CreateFileW=create_file,
                CloseHandle=mock.Mock(),
            )
            with mock.patch.object(
                ctypes, "WinDLL", return_value=kernel32
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "descriptor open failed"
            ):
                capability_model._open_enumerated_production_file(self.source)
            flags = create_file.call_args.args[5]
            self.assertTrue(flags & 0x00200000)
        else:
            original_open = os.open
            observed = []

            def tracked_open(path, flags):
                observed.append(flags)
                return original_open(path, flags)

            with mock.patch.object(
                capability_model.os, "open", side_effect=tracked_open
            ):
                stream, _metadata = (
                    capability_model._open_enumerated_production_file(
                        self.source
                    )
                )
                stream.close()
            self.assertEqual(len(observed), 1)
            self.assertTrue(observed[0] & os.O_NOFOLLOW)

    def test_task8_enumeration_table_faults_clear_before_release(self):
        for target in (
            "insert-enumeration-table",
            "freeze-enumeration-table",
        ):
            with self.subTest(target=target):
                table = capability_model._EnumeratedProductionTable(
                    AuditLimits()
                )

                def fail_at(event):
                    if event == target:
                        raise RuntimeError(target)

                with mock.patch.object(
                    capability_model,
                    "_production_enumeration_allocation_event",
                    side_effect=fail_at,
                ), self.assertRaisesRegex(RuntimeError, target):
                    table.insert(self.identity.relative, self.identity)
                    table.freeze()
                table.close()
                self.assertEqual(table._backing, {})
                self.assertEqual(table._casefolded, {})
                self.assertEqual(table._filesystem_ids, {})
                self.assertEqual(table.budget.current_bytes, 0)
                self.assertTrue(table.structure_ownership.released)

    def test_decision_collection_uses_explicit_production_and_immutable_sidecars(self):
        database = self.root / "compile_commands.json"
        entry = {"directory": str(self.root), "file": str(self.source),
                 "arguments": [sys.executable, str(self.source)]}
        database.write_text(json.dumps((entry, entry)), encoding="utf-8")
        configuration = dataclasses.replace(
            self.configuration("success"), digest="a" * 64,
            arguments=(str(self.source),))

        class DuplicateOwner:
            def __init__(self):
                self.closed = False

            def close(self):
                self.closed = True

        duplicate_owner = DuplicateOwner()
        duplicate_capability = dataclasses.replace(
            configuration.compiler_capability, native_owner=duplicate_owner)
        duplicate_configuration = dataclasses.replace(
            configuration, compiler_capability=duplicate_capability)

        class Registry:
            def register(_self, capability):
                return capability.capability_digest

        accountant = SimpleNamespace(inspection_probe_invocations=0)
        inspection = SimpleNamespace(normalized_version="Python fixture compiler")
        with mock.patch(
                "gpu_capability_source_audit._attest_loaded_audit_engine",
                return_value="f" * 64), mock.patch(
                "gpu_capability_runner.make_configuration",
                side_effect=(configuration, duplicate_configuration)) as make, mock.patch(
                "gpu_capability_runner.inspect_compiler",
                return_value=inspection):
            collection = collect_configurations_with_decision_records(
                self.root, (database,), dict(os.environ), self.production,
                self.dependency_roots, Registry(), object(), "f" * 64,
                AuditLimits(), time.monotonic() + 10.0, accountant)
        self.assertEqual(collection.configurations, (configuration,))
        self.assertEqual(set(collection.decision_records), {"a" * 64})
        self.assertIsInstance(collection.decision_records, MappingProxyType)
        self.assertEqual(make.call_count, 2)
        self.assertTrue(duplicate_owner.closed)
        with self.assertRaises(TypeError):
            collection.decision_records["a" * 64] = collection.decision_records["a" * 64]

    def test_fake_preprocessor_holds_allocation_until_native_observation_ack(self):
        from gpu_capability_process_tree import (
            OwnedProcessIdentity, OwnedProcessTree, native_process_resident_bytes)

        ready = self.root / "allocation.ready"
        release = self.root / "allocation.release"
        process = subprocess.Popen(
            (sys.executable, str(self.fixture), "--fixture-mode", "allocation-hold",
             "--allocation-bytes", str(4 << 20),
             "--allocation-ready-file", str(ready),
             "--allocation-release-file", str(release),
             "--sleep-seconds", "10", str(self.source)),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=sys.platform == "darwin")
        try:
            deadline = time.monotonic() + 5
            observed_ready = None
            while time.monotonic() < deadline:
                try:
                    observed_ready = ready.read_text(encoding="ascii")
                except FileNotFoundError:
                    observed_ready = None
                if observed_ready == str(4 << 20):
                    break
                time.sleep(0.01)
            self.assertEqual(observed_ready, str(4 << 20))
            if sys.platform == "darwin":
                from gpu_capability_process_tree import (
                    MacOSLibprocProvider, MacOSRegisteredPgidAccountant)
                provider = MacOSLibprocProvider()
                native_identity, resident, _parent = (
                    provider._identity_and_residency(process.pid, process.pid))
                accountant = MacOSRegisteredPgidAccountant()
                accountant.register_group(process.pid, native_identity,
                                          "allocation-fixture")
                provider.observe(accountant, 0)
                observed = accountant.memory_measurements()
                self.assertGreaterEqual(
                    observed.maximum_observed_owned_group_resident_bytes,
                    4 << 20)
                release.write_text("observed", encoding="ascii")
                stdout, stderr = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, (stdout, stderr))
                accountant.reconcile_group(process.pid, provider)
                self.assertTrue(accountant.memory_measurements().accounting_complete)
                return
            identity = OwnedProcessIdentity(
                "windows" if os.name == "nt" else "linux", process.pid,
                f"fixture-{process.pid}")
            tree = OwnedProcessTree(identity.platform_kind, "allocation-fixture")
            tree.register(identity, None, "acknowledged-allocation")
            resident = native_process_resident_bytes(process.pid)
            tree.observe_resident_bytes(
                identity, current_bytes=resident, peak_bytes=resident)
            self.assertGreaterEqual(resident, 4 << 20)
            release.write_text("observed", encoding="ascii")
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    @unittest.skipUnless(os.name == "nt", "requires Windows suspended launch")
    def test_windows_launch_gate_suspends_the_actual_compiler_without_helper(self):
        containment = capability_runner._ProcessContainment()
        try:
            prepared = containment.prepare_command(("C:/toolchain/g++.exe", "--version"))
            self.assertEqual(prepared[0], "C:/toolchain/g++.exe")
            self.assertNotIn("subprocess.call", " ".join(prepared))
            self.assertTrue(
                containment.popen_arguments["creationflags"]
                & getattr(subprocess, "CREATE_SUSPENDED", 0x00000004)
            )
        finally:
            containment.close()

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "playback" / "a.cpp"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("lease.nativeHandle();\n", encoding="utf-8")
        self.external_temporary = tempfile.TemporaryDirectory()
        self.outside = Path(self.external_temporary.name).resolve() / "sdk" / "outside.h"
        self.outside.parent.mkdir()
        self.outside.write_text("outside\n", encoding="utf-8")
        metadata = self.source.stat()
        self.identity = FileIdentity(
            canonical=self.source,
            relative=PurePosixPath("playback/a.cpp"),
            device=int(metadata.st_dev),
            inode=int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            line_count=1,
            production=True,
        )
        self.production = {self.identity.relative: self.identity}
        authority_roots = {
            "sdk": self.outside.parent,
            "toolchain": Path(sys.executable).resolve().parent,
        }
        if os.name == "nt":
            authority_roots["windows-system"] = Path(
                os.environ.get("SystemRoot", "C:/Windows")
            ).resolve()
        self.dependency_roots = build_dependency_root_authority(
            self.root, authority_roots
        )
        self.compiler_capability = open_compiler_executable_capability(
            Path(sys.executable).resolve(), self.dependency_roots, time.monotonic() + 10.0
        )
        self.fixture = Path(__file__).parent / "fixtures" / "fake_preprocessor.py"

    def tearDown(self) -> None:
        self.compiler_capability.native_owner.close()
        _clear_compiler_inspection_memo_for_tests()
        self.temporary.cleanup()
        self.external_temporary.cleanup()

    def configuration(
        self,
        mode: str,
        *,
        family: CompilerFamily = CompilerFamily.GCC,
        extra: tuple[str, ...] = (),
    ) -> PreprocessConfiguration:
        family_name = "msvc" if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL} else "gcc"
        return PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=family,
            compiler=Path(sys.executable).resolve(),
            working_directory=self.root,
            source=self.identity,
            arguments=(str(self.source),),
            environment_digest=_environment_digest(dict(os.environ)),
            digest=f"cfg-{family.value}-{mode}",
            dependency_root_authority_digest=(
                self.dependency_roots.portable_authority_digest
            ),
            compiler_capability_digest=self.compiler_capability.capability_digest,
            compiler_capability=self.compiler_capability,
        )

    def preprocess_fixture(
        self,
        mode: str,
        *,
        family: CompilerFamily = CompilerFamily.GCC,
        extra: tuple[str, ...] = (),
        limits: AuditLimits | None = None,
        deadline: float | None = None,
    ):
        configuration = self.configuration(mode, family=family)

        def rewrite(_configuration, dependency_output):
            family_name = (
                "msvc"
                if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
                else "gcc"
            )
            dependency_arguments = (
                ("/sourceDependencies", str(dependency_output))
                if family_name == "msvc"
                else ("-MF", str(dependency_output))
            )
            return RewrittenCommand(
                arguments=(
                    sys.executable,
                    str(self.fixture),
                    "--fixture-mode",
                    mode,
                    "--family",
                    family_name,
                    *extra,
                    str(self.source),
                    *dependency_arguments,
                ),
                dependency_output=dependency_output,
                dependency_format=(
                    "msvc-json" if family_name == "msvc" else "gcc-depfile"
                ),
            )

        with mock.patch(
            "gpu_capability_runner.rewrite_preprocess_command", side_effect=rewrite
        ):
            return preprocess_configuration(
                configuration,
                self.production,
                limits or AuditLimits(rss_bytes=2**63 - 1),
                deadline if deadline is not None else time.monotonic() + 10.0,
            )

    def stabilize_fixture(
        self, mode: str, *extra: str,
        cancel_event: object | None = None,
    ):
        configuration = self.configuration(mode)

        def rewrite(_configuration, dependency_output):
            return RewrittenCommand(
                arguments=(
                    sys.executable,
                    str(self.fixture),
                    "--fixture-mode",
                    mode,
                    "--family",
                    "gcc",
                    *extra,
                    str(self.source),
                    "-MF",
                    str(dependency_output),
                ),
                dependency_output=dependency_output,
                dependency_format="gcc-depfile",
            )

        with mock.patch(
            "gpu_capability_runner.rewrite_preprocess_command", side_effect=rewrite
        ):
            return stabilize_and_parse_configuration(
                configuration,
                self.dependency_roots,
                self.production,
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
                cancel_event,
            )

    def test_cold_stabilization_invokes_twice_but_builds_once(self):
        with mock.patch(
            "gpu_capability_runner.PreprocessedStreamBuilder",
            wraps=PreprocessedStreamBuilder,
        ) as builder, mock.patch(
            "gpu_capability_runner.run_bounded_preprocessor",
            wraps=run_bounded_preprocessor,
        ) as execute:
            view, discovery, stages = self.stabilize_fixture("success")
        self.assertEqual(builder.call_count, 1)
        self.assertEqual(execute.call_count, 2)
        self.assertGreater(discovery.stream.byte_count, 0)
        self.assertGreater(stages.discovery_seconds, 0.0)
        self.assertGreater(stages.accepted_parse_seconds, 0.0)
        self.assertEqual(view.configuration.digest, "cfg-gcc-success")

    def test_cold_stabilization_does_not_rehash_held_compiler_closure(self):
        hashed_bytes = []
        original = capability_command._content_sha256

        def counted(stream, **kwargs):
            hashed_bytes.append(int(os.fstat(stream.fileno()).st_size))
            return original(stream, **kwargs)

        owner = self.compiler_capability.native_owner
        self.assertGreater(len(owner.streams), 0)
        closure_bytes = sum(
            int(os.fstat(stream.fileno()).st_size) for stream in owner.streams
        )
        self.assertGreater(closure_bytes, 0)
        with mock.patch(
            "gpu_capability_command._content_sha256", side_effect=counted
        ) as content_hash:
            self.stabilize_fixture("success")
        self.assertEqual(content_hash.call_count, 0)
        self.assertEqual(sum(hashed_bytes), 0)

    def test_invalidation_deadline_and_cancellation_prevent_launch(self):
        with mock.patch(
            "gpu_capability_runner.subprocess.Popen",
            side_effect=OSError("Popen reached"),
        ) as popen, mock.patch.object(
            self.compiler_capability.native_owner.observer,
            "drain",
            side_effect=(
                None,
                None,
                AuditInfrastructureError("generation invalidated"),
            ),
        ), self.assertRaisesRegex(AuditInfrastructureError, "generation invalidated"):
            self.run_direct("success")
        popen.assert_not_called()

        with mock.patch("gpu_capability_runner.subprocess.Popen") as popen, \
                self.assertRaisesRegex(AuditInfrastructureError, "global deadline"):
            self.run_direct("success", deadline=time.monotonic() - 0.001)
        popen.assert_not_called()

        cancelled = threading.Event()
        cancelled.set()
        with mock.patch("gpu_capability_runner.subprocess.Popen") as popen, \
                self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            self.run_direct("success", cancel_event=cancelled)
        popen.assert_not_called()

    def test_raw_output_and_byte_count_mismatches_publish_nothing(self):
        for mode, message in (
            ("different-second-output", "raw preprocessed output changed"),
            ("different-second-byte-count", "raw preprocessed byte count changed"),
        ):
            with self.subTest(mode=mode), self.assertRaisesRegex(
                AuditInfrastructureError, message
            ):
                self.stabilize_fixture(mode)

    def test_dependency_reorder_add_and_remove_are_rejected(self):
        header = self.root / "playback" / "guarded.h"
        header.write_text("header\n", encoding="utf-8")
        metadata = header.stat()
        header_identity = FileIdentity(
            header.resolve(), PurePosixPath("playback/guarded.h"),
            int(metadata.st_dev), int(metadata.st_ino) or None, 1, True,
        )
        self.production[header_identity.relative] = header_identity
        for schedule in ("reorder", "add", "remove"):
            with self.subTest(schedule=schedule), self.assertRaisesRegex(
                AuditInfrastructureError, "dependency closure changed"
            ):
                self.stabilize_fixture(
                    "success", "--extra-dependency", str(header),
                    "--dependency-schedule", schedule,
                )

    def test_accepted_production_and_external_mutation_restoration_fail_closed(self):
        for target in (self.source, self.outside):
            expected = (
                "guard prevented generation change"
                if os.name == "nt" else "generation change"
            )
            with self.subTest(target=target), self.assertRaisesRegex(
                AuditInfrastructureError, expected
            ):
                self.stabilize_fixture(
                    "mutate-restore", "--mutate-path", str(target),
                    "--extra-dependency", str(target),
                )

    def test_accepted_authority_root_swap_restoration_fails_closed(self):
        expected = (
            "guard prevented generation change"
            if os.name == "nt" else "generation change"
        )
        with self.assertRaisesRegex(AuditInfrastructureError, expected):
            self.stabilize_fixture(
                "swap-root-restore", "--mutate-path", str(self.outside.parent),
                "--extra-dependency", str(self.outside),
            )

    def test_discovery_mutation_restoration_cannot_match_accepted_output(self):
        with self.assertRaisesRegex(
            AuditInfrastructureError, "raw preprocessed (output|byte count) changed"
        ):
            self.stabilize_fixture(
                "discovery-mutate-restore", "--mutate-path", str(self.source),
            )

    def test_production_root_and_nested_ancestor_swap_restoration_fail_closed(self):
        nested_header = self.source.parent / "nested" / "guarded.h"
        nested_header.parent.mkdir()
        nested_header.write_text("guarded\n", encoding="utf-8")
        metadata = nested_header.stat()
        nested_identity = FileIdentity(
            nested_header.resolve(), PurePosixPath("playback/nested/guarded.h"),
            int(metadata.st_dev), int(metadata.st_ino) or None, 1, True,
        )
        self.production[nested_identity.relative] = nested_identity
        expected = "guard prevented generation change" if os.name == "nt" else "generation change"
        for target in (self.root, nested_header.parent):
            with self.subTest(target=target), self.assertRaisesRegex(
                AuditInfrastructureError, expected
            ):
                self.stabilize_fixture(
                    "swap-root-restore", "--mutate-path", str(target),
                    "--extra-dependency", str(nested_header),
                )

    def test_real_subprocess_accepts_thread_and_spawn_cancellation_events(self):
        events = (threading.Event(), multiprocessing.get_context("spawn").Event())
        for event in events:
            event.set()
            with self.subTest(event=type(event).__name__), self.assertRaisesRegex(
                AuditInfrastructureError, "cancelled"
            ):
                self.run_direct(
                    "wait-for-cancel",
                    cancel_event=event,
                    extra=("--sleep-seconds", "30"),
                )

    @staticmethod
    def altered_view(
        view: PreprocessedTranslationUnitView,
    ) -> PreprocessedTranslationUnitView:
        spellings = tuple(
            b"zease" if spelling == b"lease" else spelling
            for spelling in object.__getattribute__(view.tokens, "_spellings")
        )
        columns = tuple(array("I", column) for column in view.tokens._packed_columns())
        return PreprocessedTranslationUnitView(
            view.configuration,
            CompactTokenSequence._from_packed(
                view.configuration,
                spellings=spellings,
                identities=object.__getattribute__(view.tokens, "_identities"),
                spelling_ids=columns[0],
                identity_ids=columns[1],
                inclusion_ids=columns[2],
                original_lines=columns[3],
            ),
            view.dependencies,
        )

    def direct_command(self, mode: str, *extra: str) -> RewrittenCommand:
        return RewrittenCommand(
            arguments=(
                sys.executable,
                str(self.fixture),
                "--fixture-mode",
                mode,
                *extra,
                str(self.source),
            ),
            dependency_output=self.root / "unused.d",
            dependency_format="gcc-depfile",
        )

    def run_direct(
        self,
        mode: str,
        *,
        limits: AuditLimits | None = None,
        deadline: float | None = None,
        extra: tuple[str, ...] = (),
        cancel_event: threading.Event | None = None,
    ) -> tuple[ExecutionResult, bytes]:
        chunks: list[bytes] = []
        result = run_bounded_preprocessor(
            self.direct_command(mode, *extra),
            self.configuration(mode),
            limits or AuditLimits(rss_bytes=2**63 - 1),
            deadline if deadline is not None else time.monotonic() + 10.0,
            chunks.append,
            cancel_event,
        )
        return result, b"".join(chunks)

    def test_success_returns_compact_view_without_raw_stdout(self):
        view = self.preprocess_fixture("success")
        self.assertIsInstance(view.tokens, CompactTokenSequence)
        self.assertEqual(
            [token.spelling for token in view.tokens],
            [b"lease", b".", b"nativeHandle", b"(", b")", b";"],
        )
        self.assertEqual(view.dependencies, (self.identity,))

    def test_msvc_success_uses_json_dependencies_and_compact_view(self):
        view = self.preprocess_fixture("success", family=CompilerFamily.MSVC)
        self.assertIsInstance(view.tokens, CompactTokenSequence)
        self.assertEqual(view.configuration.family, CompilerFamily.MSVC)
        self.assertEqual(view.dependencies, (self.identity,))

    def test_nonzero_exit_never_returns_partial_view_and_bounds_stderr_tail(self):
        limits = dataclasses.replace(
            AuditLimits(), stderr_bytes=32, rss_bytes=2**63 - 1
        )
        with self.assertRaises(AuditInfrastructureError) as raised:
            self.run_direct(
                "fail",
                limits=limits,
                extra=("--stderr-bytes", "256"),
            )
        diagnostic = str(raised.exception)
        self.assertIn("exit=9", diagnostic)
        self.assertIn("TAIL-OF-STDERR", diagnostic)
        self.assertNotIn("BEGIN-OF-STDERR", diagnostic)

    def test_large_stderr_retains_the_final_tail_only(self):
        limits = dataclasses.replace(
            AuditLimits(), stderr_bytes=64, rss_bytes=2**63 - 1
        )
        with self.assertRaises(AuditInfrastructureError) as raised:
            self.run_direct(
                "fail",
                limits=limits,
                extra=("--stderr-bytes", str(2 * 1024 * 1024)),
            )
        diagnostic = str(raised.exception)
        self.assertIn("TAIL-OF-STDERR", diagnostic)
        self.assertNotIn("BEGIN-OF-STDERR", diagnostic)

    def test_timeout_terminates_process_group(self):
        child_pid_file = self.root / "child.pid"
        started = time.monotonic()
        with self.assertRaisesRegex(AuditInfrastructureError, "timeout"):
            self.run_direct(
                "child-sleep",
                limits=dataclasses.replace(
                    AuditLimits(), invocation_seconds=0.5, rss_bytes=2**63 - 1
                ),
                extra=(
                    "--child-pid-file",
                    str(child_pid_file),
                    "--sleep-seconds",
                    "30",
                ),
            )
        self.assertLess(time.monotonic() - started, 2.0)
        for _ in range(100):
            if child_pid_file.exists():
                break
            time.sleep(0.01)
        self.assertTrue(child_pid_file.exists())
        child_pid = int(child_pid_file.read_text(encoding="ascii"))
        self.assertFalse(self.process_is_alive(child_pid))

    def test_coordinator_cancellation_terminates_active_process_group(self):
        child_pid_file = self.root / "cancelled-child.pid"
        cancellation = threading.Event()

        def cancel_after_launch() -> None:
            deadline = time.monotonic() + 2.0
            while not child_pid_file.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            cancellation.set()

        trigger = threading.Thread(target=cancel_after_launch)
        trigger.start()
        started = time.monotonic()
        try:
            with self.assertRaisesRegex(AuditInfrastructureError, "coordinator cancelled"):
                self.run_direct(
                    "child-sleep",
                    limits=dataclasses.replace(
                        AuditLimits(), invocation_seconds=10.0, rss_bytes=2**63 - 1
                    ),
                    extra=(
                        "--child-pid-file",
                        str(child_pid_file),
                        "--sleep-seconds",
                        "30",
                    ),
                    cancel_event=cancellation,
                )
        finally:
            trigger.join(timeout=3.0)
        self.assertLess(time.monotonic() - started, 3.0)
        self.assertTrue(child_pid_file.exists())
        child_pid = int(child_pid_file.read_text(encoding="ascii"))
        self.assertFalse(self.process_is_alive(child_pid))

    def test_successful_parent_exit_cleans_descendant_holding_stdout(self):
        child_pid_file = self.root / "successful-child.pid"
        started = time.monotonic()
        result, output = self.run_direct(
            "child-exit",
            limits=dataclasses.replace(
                AuditLimits(), invocation_seconds=1.0, rss_bytes=2**63 - 1
            ),
            extra=("--child-pid-file", str(child_pid_file)),
        )
        self.assertLess(time.monotonic() - started, 2.0)
        self.assertGreater(result.observed_stdout_bytes, 0)
        self.assertIn(b"complete", output)
        self.assertTrue(child_pid_file.exists())
        child_pid = int(child_pid_file.read_text(encoding="ascii"))
        reap_deadline = time.monotonic() + 2.0
        while self.process_is_alive(child_pid) and time.monotonic() < reap_deadline:
            time.sleep(0.01)
        self.assertFalse(self.process_is_alive(child_pid))

    @staticmethod
    def process_is_alive(pid: int) -> bool:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            kernel32.OpenProcess.restype = wintypes.HANDLE
            handle = kernel32.OpenProcess(0x00100000, False, pid)
            if not handle:
                return False
            try:
                return kernel32.WaitForSingleObject(handle, 0) == 0x00000102
            finally:
                kernel32.CloseHandle(handle)
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        try:
            state = Path(f"/proc/{pid}/stat").read_text(encoding="ascii").split()[2]
        except (OSError, IndexError):
            return True
        return state != "Z"

    def test_stdout_limit_accepts_exact_boundary_and_rejects_one_byte_over(self):
        limit = 8192
        limits = dataclasses.replace(
            AuditLimits(), stdout_bytes=limit, rss_bytes=2**63 - 1
        )
        result, output = self.run_direct(
            "overflow",
            limits=limits,
            extra=("--stdout-bytes", str(limit)),
        )
        self.assertEqual(result.observed_stdout_bytes, limit)
        self.assertEqual(len(output), limit)
        with self.assertRaisesRegex(AuditInfrastructureError, "stdout.*limit"):
            self.run_direct(
                "overflow",
                limits=limits,
                extra=("--stdout-bytes", str(limit + 1)),
            )

    def test_execution_result_retains_no_stdout_or_raw_output_cache(self):
        result, output = self.run_direct(
            "overflow", extra=("--stdout-bytes", "17")
        )
        self.assertEqual(output, b"x" * 17)
        self.assertEqual(
            set(result.__dataclass_fields__),
            {"stderr_tail", "elapsed_seconds", "observed_stdout_bytes"},
        )
        self.assertFalse(
            any(
                "stdout" in name and name != "observed_stdout_bytes"
                for name in vars(result)
            )
        )

    def test_pipe_close_and_join_failures_still_finalize_process_carrier(self):
        real_launch = capability_runner._launch_compiler_process
        real_join = threading.Thread.join
        for failure_kind in ("join", "pipe-close"):
            with self.subTest(failure_kind=failure_kind):
                carriers = []
                processes = []
                original_streams = []
                stdout_joins = 0

                def capture_launch(*args, **kwargs):
                    process, carrier = real_launch(*args, **kwargs)
                    carriers.append(carrier)
                    processes.append(process)
                    original_streams.extend((process.stdout, process.stderr))
                    if failure_kind == "pipe-close":
                        original_stdout = process.stdout

                        class FailingCloseStream:
                            def read(self, size): return original_stdout.read(size)
                            def close(self): raise OSError("stdout close failed")

                        process.stdout = FailingCloseStream()
                    return process, carrier

                def injected_join(thread, timeout=None):
                    nonlocal stdout_joins
                    if thread.name == "gpu-audit-stdout":
                        stdout_joins += 1
                        if failure_kind == "join" and stdout_joins == 2:
                            raise RuntimeError("reader join failed")
                    return real_join(thread, timeout=timeout)

                try:
                    with mock.patch.object(
                        capability_runner,
                        "_launch_compiler_process",
                        side_effect=capture_launch,
                    ), mock.patch.object(
                        threading.Thread, "join", new=injected_join
                    ), self.assertRaisesRegex(
                        (AuditInfrastructureError, OSError, RuntimeError),
                        "(join|close)",
                    ):
                        self.run_direct("success")
                    self.assertEqual(len(carriers), 1)
                    self.assertTrue(carriers[0].completed)
                    self.assertIsNone(carriers[0].process)
                finally:
                    for carrier in carriers:
                        if not carrier.completed:
                            carrier.complete_after_exit()
                    for stream in original_streams:
                        if stream is not None and not stream.closed:
                            stream.close()

    def test_past_global_deadline_fails_without_starting_process(self):
        with mock.patch("gpu_capability_runner.subprocess.Popen") as popen:
            with self.assertRaisesRegex(AuditInfrastructureError, "global deadline"):
                self.run_direct("success", deadline=time.monotonic() - 0.001)
        popen.assert_not_called()

    def test_environment_digest_mismatch_fails_before_launch(self):
        configuration = dataclasses.replace(
            self.configuration("success"), environment_digest="0" * 64
        )
        with mock.patch("gpu_capability_runner.subprocess.Popen") as popen:
            with self.assertRaisesRegex(
                AuditInfrastructureError, "environment digest mismatch"
            ):
                run_bounded_preprocessor(
                    self.direct_command("success"),
                    configuration,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                    lambda _chunk: None,
                )
        popen.assert_not_called()

    def test_coordinator_cancellation_during_setup_fails_before_launch(self):
        cancellation = threading.Event()
        configuration = self.configuration("success")

        def snapshot(_environment):
            cancellation.set()
            return configuration.environment_digest

        with mock.patch(
            "gpu_capability_runner._environment_digest", side_effect=snapshot
        ), mock.patch("gpu_capability_runner.subprocess.Popen") as popen, self.assertRaisesRegex(
            AuditInfrastructureError, "coordinator cancelled before launch"
        ):
            run_bounded_preprocessor(
                self.direct_command("success"),
                configuration,
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
                lambda _chunk: None,
                cancellation,
            )
        popen.assert_not_called()

    def test_transferred_capability_launch_never_resolves_compiler_path(self):
        configuration = self.configuration("success")
        owner = configuration.compiler_capability.native_owner
        original_resolve = Path.resolve

        def reject_compiler_lookup(path, *args, **kwargs):
            if os.path.normcase(str(path)) == os.path.normcase(
                str(configuration.compiler_capability.executable_identity.canonical)
            ):
                raise AssertionError("worker resolved transferred compiler path")
            return original_resolve(path, *args, **kwargs)

        owner.validate_paths = False
        try:
            chunks = []
            with mock.patch.object(Path, "resolve", reject_compiler_lookup):
                result = run_bounded_preprocessor(
                    self.direct_command("success"),
                    configuration,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                    chunks.append,
                )
            self.assertIsInstance(result, ExecutionResult)
            self.assertTrue(chunks)
        finally:
            owner.validate_paths = True

    def test_cancellation_is_rechecked_immediately_before_popen(self):
        cancellation = threading.Event()

        def prepare(_containment, command):
            cancellation.set()
            return command

        with mock.patch.object(
            capability_runner._ProcessContainment,
            "prepare_command",
            prepare,
        ), mock.patch(
            "gpu_capability_runner.subprocess.Popen",
            side_effect=OSError("Popen was reached"),
        ) as popen, self.assertRaisesRegex(
            AuditInfrastructureError, "cancelled before launch"
        ):
            self.run_direct(
                "success", deadline=time.monotonic() + 10.0,
                cancel_event=cancellation,
            )
        popen.assert_not_called()

    def test_exact_environment_snapshot_is_passed_to_popen(self):
        snapshot = {"GPU_RUNNER_SENTINEL": "one"}
        configuration = dataclasses.replace(
            self.configuration("success"),
            environment_digest=_environment_digest(snapshot),
        )

        def reject_launch(*_args, **kwargs):
            self.assertEqual(kwargs["env"], snapshot)
            raise OSError("controlled launch stop")

        with mock.patch("gpu_capability_runner.os.environ", snapshot), mock.patch(
            "gpu_capability_runner.subprocess.Popen", side_effect=reject_launch
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "controlled launch stop"
            ):
                run_bounded_preprocessor(
                    self.direct_command("success"),
                    configuration,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                    lambda _chunk: None,
                )

    def test_stream_reader_uses_a_large_bounded_transport_chunk(self):
        self.assertEqual(capability_runner._IO_CHUNK_BYTES, 256 * 1024)

    def test_multi_megabyte_transport_meets_deadline_and_exact_bound(self):
        byte_count = 3_360_000
        limits = dataclasses.replace(
            AuditLimits(),
            stdout_bytes=byte_count,
            invocation_seconds=10.0,
            rss_bytes=2**63 - 1,
        )
        started = time.monotonic()
        result, output = self.run_direct(
            "overflow",
            limits=limits,
            extra=("--stdout-bytes", str(byte_count)),
        )
        self.assertLess(time.monotonic() - started, 5.0)
        self.assertEqual(result.observed_stdout_bytes, byte_count)
        self.assertEqual(len(output), byte_count)
        with self.assertRaisesRegex(AuditInfrastructureError, "stdout.*limit"):
            self.run_direct(
                "overflow",
                limits=dataclasses.replace(limits, stdout_bytes=byte_count - 1),
                extra=("--stdout-bytes", str(byte_count)),
            )

    def test_global_deadline_wins_when_shorter_than_invocation_limit(self):
        started = time.monotonic()
        with self.assertRaisesRegex(AuditInfrastructureError, "global deadline"):
            self.run_direct(
                "sleep",
                limits=dataclasses.replace(
                    AuditLimits(), invocation_seconds=60.0, rss_bytes=2**63 - 1
                ),
                deadline=time.monotonic() + 0.2,
                extra=("--sleep-seconds", "30"),
            )
        self.assertLess(time.monotonic() - started, 2.0)

    def test_injected_rss_overflow_terminates_process(self):
        with mock.patch(
            "gpu_capability_runner._current_process_rss_bytes", return_value=101
        ), mock.patch("gpu_capability_runner.subprocess.Popen") as popen:
            with self.assertRaisesRegex(AuditInfrastructureError, "RSS limit"):
                self.run_direct(
                    "sleep",
                    limits=dataclasses.replace(
                        AuditLimits(), rss_bytes=100, invocation_seconds=5.0
                    ),
                    extra=("--sleep-seconds", "30"),
                )
        popen.assert_not_called()

    def test_preprocess_setup_rss_failure_has_structured_diagnostic(self):
        configuration = self.configuration("success")
        with mock.patch(
            "gpu_capability_runner._current_process_rss_bytes", return_value=101
        ):
            with self.assertRaises(AuditInfrastructureError) as raised:
                preprocess_configuration(
                    configuration,
                    self.production,
                    dataclasses.replace(AuditLimits(), rss_bytes=100),
                    time.monotonic() + 10.0,
                )
        diagnostic = str(raised.exception)
        for fragment in (
            configuration.digest,
            "family=gcc",
            str(self.source),
            "exit=not-started",
            "elapsed=",
            "stdout_bytes=0",
            "stderr_tail=b''",
            "RSS limit",
        ):
            self.assertIn(fragment, diagnostic)

    def test_missing_working_directory_has_structured_diagnostic(self):
        configuration = dataclasses.replace(
            self.configuration("success"),
            working_directory=self.root / "missing-build-directory",
        )
        with self.assertRaises(AuditInfrastructureError) as raised:
            preprocess_configuration(
                configuration,
                self.production,
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
            )
        diagnostic = str(raised.exception)
        for fragment in (
            configuration.digest,
            "family=gcc",
            str(self.source),
            "exit=not-started",
            "elapsed=",
            "stdout_bytes=0",
            "stderr_tail=b''",
        ):
            self.assertIn(fragment, diagnostic)

    @unittest.skipUnless(os.name == "nt", "Windows Job Object behavior")
    def test_windows_job_terminate_failure_uses_close_fallback_and_reports(self):
        job = object.__new__(_WindowsJob)
        job._handle = 123
        job._kernel32 = mock.Mock()
        job._kernel32.TerminateJobObject.return_value = False
        job._kernel32.CloseHandle.return_value = True
        with self.assertRaisesRegex(AuditInfrastructureError, "terminate.*job"):
            job.terminate()
        job._kernel32.CloseHandle.assert_called_once_with(123)
        self.assertIsNone(job._handle)

    @unittest.skipUnless(os.name == "nt", "Windows Job Object behavior")
    def test_windows_job_close_failure_terminates_retries_and_reports(self):
        job = object.__new__(_WindowsJob)
        job._handle = 456
        job._kernel32 = mock.Mock()
        job._kernel32.CloseHandle.side_effect = (False, True)
        job._kernel32.TerminateJobObject.return_value = True
        with self.assertRaisesRegex(AuditInfrastructureError, "close.*job"):
            job.close()
        job._kernel32.TerminateJobObject.assert_called_once_with(456, 1)
        self.assertEqual(job._kernel32.CloseHandle.call_count, 2)
        self.assertIsNone(job._handle)

    def test_posix_containment_signals_each_owned_generation_at_most_once(self):
        class Process:
            pid = 4242

        sequences = (
            ("normal", ("terminate", "close")),
            ("timeout", ("terminate", "terminate", "close")),
            ("error", ("terminate", "close", "close")),
            ("close-twice", ("close", "close")),
        )
        for name, actions in sequences:
            with self.subTest(name=name), mock.patch(
                "gpu_capability_runner.os.name", "posix"
            ), mock.patch(
                "gpu_capability_runner.os.killpg", create=True
            ) as killpg, mock.patch(
                "gpu_capability_runner.signal.SIGKILL", 9, create=True
            ):
                containment = capability_runner._ProcessContainment()
                containment.attach(Process())
                for action in actions:
                    getattr(containment, action)()
                self.assertLessEqual(killpg.call_count, 1)
                if "terminate" in actions:
                    killpg.assert_called_once_with(4242, 9)

    def test_malformed_output_and_dependency_fail_closed(self):
        with self.assertRaisesRegex(
            AuditInfrastructureError,
            "marker path|invalid bytes|embedded NUL",
        ):
            self.preprocess_fixture("malformed")
        with self.assertRaisesRegex(AuditInfrastructureError, "dependency"):
            self.preprocess_fixture(
                "success", extra=("--dependency-mode", "missing")
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "MSVC dependency JSON"):
            self.preprocess_fixture(
                "success",
                family=CompilerFamily.MSVC,
                extra=("--dependency-mode", "malformed"),
            )

    def test_marker_not_present_in_dependency_manifest_is_rejected(self):
        with self.assertRaisesRegex(
            AuditInfrastructureError,
            "line marker does not name a dependency",
        ) as raised:
            self.preprocess_fixture(
                "success",
                extra=(
                    "--dependency-mode",
                    "outside",
                    "--outside-path",
                    str(self.outside),
                ),
            )
        diagnostic = str(raised.exception)
        self.assertIn("exit=0", diagnostic)
        self.assertNotIn("elapsed=0.000s", diagnostic)
        self.assertRegex(diagnostic, r"stdout_bytes=[1-9][0-9]*")

    def test_dense_output_crosses_packed_limit_before_stdout_limit(self):
        limits = dataclasses.replace(
            AuditLimits(),
            stdout_bytes=1024 * 1024,
            retained_token_bytes=4096,
            rss_bytes=2**63 - 1,
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "retained packed token limit"):
            self.preprocess_fixture(
                "dense",
                extra=("--token-count", "5000"),
                limits=limits,
            )

    def test_error_diagnostic_identifies_configuration_and_observations(self):
        with self.assertRaises(AuditInfrastructureError) as raised:
            self.preprocess_fixture(
                "fail", extra=("--stderr-bytes", "64")
            )
        diagnostic = str(raised.exception)
        for fragment in (
            "cfg-gcc-fail",
            "family=gcc",
            str(self.source),
            "exit=9",
            "elapsed=",
            "stdout_bytes=",
            "stderr_tail=",
        ):
            self.assertIn(fragment, diagnostic)

    def test_consumer_failure_preserves_actual_nonzero_exit(self):
        def reject_partial(_chunk: bytes) -> None:
            time.sleep(0.2)
            raise AuditInfrastructureError("consumer rejected partial stream")

        with self.assertRaises(AuditInfrastructureError) as raised:
            run_bounded_preprocessor(
                self.direct_command("fail"),
                self.configuration("fail"),
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
                reject_partial,
            )
        diagnostic = str(raised.exception)
        self.assertIn("consumer rejected partial stream", diagnostic)
        self.assertIn("exit=9", diagnostic)

    def test_load_or_preprocess_reuses_validated_hit(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        expected = self.preprocess_fixture("success")
        cache.publish(expected)
        with mock.patch(
            "gpu_capability_runner.preprocess_configuration",
            side_effect=AssertionError("compiler invoked on hit"),
        ):
            actual = load_or_preprocess(
                configuration,
                self.dependency_roots,
                self.production,
                cache,
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
            )
        self.assertEqual(actual.configuration, expected.configuration)
        self.assertEqual(actual.tokens.packed_bytes, expected.tokens.packed_bytes)

    def test_load_or_preprocess_publishes_only_success(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        expected = self.preprocess_fixture("success")
        discovery = SimpleNamespace(
            dependency_identities=expected.dependencies,
            dependencies=capability_runner._dependency_digests(
                expected.dependencies, self.dependency_roots,
                time.monotonic() + 10.0, None,
            ),
        )

        def stabilized(*_arguments, publication, **_kwargs):
            return publication(expected, discovery, lambda: None), discovery, None

        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration",
            side_effect=stabilized,
        ) as preprocess:
            self.assertIs(
                load_or_preprocess(
                    configuration,
                    self.dependency_roots,
                    self.production,
                    cache,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                ),
                expected,
            )
        self.assertEqual(preprocess.call_count, 1)
        self.assertIsNotNone(cache.load(configuration))

        failed_configuration = dataclasses.replace(configuration, digest="failed")
        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration",
            side_effect=AuditInfrastructureError("compiler failed"),
        ):
            with self.assertRaisesRegex(AuditInfrastructureError, "compiler failed"):
                load_or_preprocess(
                    failed_configuration,
                    self.dependency_roots,
                    self.production,
                    cache,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                )
        self.assertIsNone(cache.load(failed_configuration))

    def test_load_or_preprocess_never_caches_old_tokens_against_new_dependency(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        old_view = self.preprocess_fixture("success")

        calls = 0

        def preprocess(*_arguments, **_kwargs):
            nonlocal calls
            calls += 1
            self.source.write_text("changed after compiler read\n", encoding="utf-8")
            raise AuditInfrastructureError("dependency changed during preprocessing")

        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration", side_effect=preprocess
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "changed during preprocessing"
            ):
                load_or_preprocess(
                    configuration,
                    self.dependency_roots,
                    self.production,
                    cache,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                )
        self.assertEqual(calls, 1)
        self.assertIsNone(cache.load(configuration))

    def test_post_accepted_same_inode_mutation_never_publishes_old_tokens(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        before = self.source.stat()

        def mutate_after_accepted(*_arguments, publication, **_kwargs):
            view, discovery, stages = self.stabilize_fixture("success")
            self.source.write_text("other.nativeHandle();\n", encoding="utf-8")
            after = self.source.stat()
            self.assertEqual(int(after.st_ino), int(before.st_ino))
            self.assertEqual(
                self.source.read_text(encoding="utf-8").count("\n"),
                self.identity.line_count,
            )
            return publication(view, discovery, lambda: None), discovery, stages

        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration",
            side_effect=mutate_after_accepted,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "dependency content changed"
        ):
            load_or_preprocess(
                configuration,
                self.dependency_roots,
                self.production,
                cache,
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
            )
        self.assertIsNone(cache.load(configuration))

    def test_mutation_after_cache_stability_check_prevents_publish_and_return(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        before = self.source.stat()
        real_validate = cache._validate_stable_dependencies

        def rewrite(_configuration, dependency_output):
            return RewrittenCommand(
                arguments=(
                    sys.executable,
                    str(self.fixture),
                    "--fixture-mode",
                    "success",
                    "--family",
                    "gcc",
                    str(self.source),
                    "-MF",
                    str(dependency_output),
                ),
                dependency_output=dependency_output,
                dependency_format="gcc-depfile",
            )

        def mutate_after_validation(dependencies, snapshots):
            real_validate(dependencies, snapshots)
            self.source.write_text("other.nativeHandle();\n", encoding="utf-8")
            after = self.source.stat()
            self.assertEqual(int(after.st_ino), int(before.st_ino))
            self.assertEqual(
                self.source.read_text(encoding="utf-8").count("\n"),
                self.identity.line_count,
            )

        with mock.patch(
            "gpu_capability_runner.rewrite_preprocess_command",
            side_effect=rewrite,
        ), mock.patch.object(
            cache,
            "_validate_stable_dependencies",
            side_effect=mutate_after_validation,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "dependency.*changed|cannot publish"
        ):
            load_or_preprocess(
                configuration,
                self.dependency_roots,
                self.production,
                cache,
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
            )
        self.assertIsNone(cache.load(configuration))

    def test_failed_mutation_attempt_after_atomic_rename_removes_publication(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        entry = cache.root / cache._configuration_key(configuration)
        real_rename = os.rename

        def rewrite(_configuration, dependency_output):
            return RewrittenCommand(
                arguments=(
                    sys.executable, str(self.fixture), "--fixture-mode", "success",
                    "--family", "gcc", str(self.source), "-MF",
                    str(dependency_output),
                ),
                dependency_output=dependency_output,
                dependency_format="gcc-depfile",
            )

        def rename_then_mutate(source, destination):
            real_rename(source, destination)
            if Path(destination) == entry and Path(source).name.startswith(".tmp-"):
                self.source.write_text("other.nativeHandle();\n", encoding="utf-8")

        with mock.patch(
            "gpu_capability_runner.rewrite_preprocess_command", side_effect=rewrite
        ), mock.patch(
            "gpu_capability_cache.os.rename", side_effect=rename_then_mutate
        ), self.assertRaises(AuditInfrastructureError):
            load_or_preprocess(
                configuration, self.dependency_roots, self.production, cache,
                AuditLimits(rss_bytes=2**63 - 1), time.monotonic() + 10.0,
            )
        self.assertIsNone(cache.load(configuration))

    def test_cancellation_after_accepted_dependency_parse_prevents_success(self):
        cancellation = threading.Event()
        original = capability_runner._parse_dependency_output
        calls = 0

        def cancel_after_accepted(*arguments, **kwargs):
            nonlocal calls
            result = original(*arguments, **kwargs)
            calls += 1
            if calls == 2:
                cancellation.set()
            return result

        with mock.patch(
            "gpu_capability_runner._parse_dependency_output",
            side_effect=cancel_after_accepted,
        ), self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            self.stabilize_fixture(
                "success", cancel_event=cancellation
            )
        self.assertEqual(calls, 2)

    def test_load_or_preprocess_rejects_transient_output_even_when_content_is_restored(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration",
            side_effect=AuditInfrastructureError("raw preprocessed output changed"),
        ), mock.patch.object(
            CompactTokenSequence,
            "__iter__",
            side_effect=AssertionError("token iteration"),
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "raw preprocessed output changed"
            ):
                load_or_preprocess(
                    configuration,
                    self.dependency_roots,
                    self.production,
                    cache,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                )
        self.assertIsNone(cache.load(configuration))

    def test_concurrent_callers_cannot_return_different_views_for_one_key(self):
        configuration = self.configuration("success")
        cache_root = self.root / "cache"
        caches = (PreprocessCache(cache_root), PreprocessCache(cache_root))
        first_view = self.preprocess_fixture("success")
        second_view = self.altered_view(first_view)
        views = {"first": first_view, "second": second_view}
        entry = cache_root / caches[0]._configuration_key(configuration)
        rename_entered = threading.Event()
        release_rename = threading.Event()
        real_rename = os.rename
        successes: list[PreprocessedTranslationUnitView] = []
        failures: list[BaseException] = []

        def rename(source, destination) -> None:
            if Path(source).name.startswith(".tmp-") and Path(destination) == entry:
                rename_entered.set()
                if not release_rename.wait(timeout=5.0):
                    raise AssertionError("rename release timed out")
            real_rename(source, destination)

        def preprocess(*_arguments, publication, **_kwargs):
            view = views[threading.current_thread().name]
            discovery = SimpleNamespace(
                dependency_identities=view.dependencies,
                dependencies=capability_runner._dependency_digests(
                    view.dependencies, self.dependency_roots,
                    time.monotonic() + 10.0, None,
                ),
            )
            return publication(view, discovery, lambda: None), discovery, None

        def run(cache: PreprocessCache) -> None:
            try:
                successes.append(
                    load_or_preprocess(
                        configuration,
                        self.dependency_roots,
                        self.production,
                        cache,
                        AuditLimits(rss_bytes=2**63 - 1),
                        time.monotonic() + 10.0,
                    )
                )
            except BaseException as error:
                failures.append(error)

        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration", side_effect=preprocess
        ), mock.patch("gpu_capability_cache.os.rename", side_effect=rename):
            first_thread = threading.Thread(
                target=run, args=(caches[0],), name="first"
            )
            second_thread = threading.Thread(
                target=run, args=(caches[1],), name="second"
            )
            first_thread.start()
            self.assertTrue(rename_entered.wait(timeout=5.0))
            second_thread.start()
            try:
                time.sleep(0.2)
                self.assertEqual(successes, [])
                self.assertEqual(failures, [])
            finally:
                release_rename.set()
                first_thread.join(timeout=10.0)
                second_thread.join(timeout=10.0)
            threads = (first_thread, second_thread)
        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(len(successes), 2)
        self.assertEqual(failures, [])
        self.assertEqual(
            len(
                {
                    capability_model._preprocessed_view_semantic_digest(view)
                    for view in successes
                }
            ),
            1,
        )


class OrchestrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.driver_helpers = mock.patch(
            "gpu_capability_command._driver_selected_helper_paths", return_value=()
        )
        self.driver_helpers.start()
        self.addCleanup(self.driver_helpers.stop)
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        (self.root / "playback").mkdir()
        (self.root / "recorder_engine").mkdir()
        self.toolchain_temporary = tempfile.TemporaryDirectory()
        self.compiler = Path(self.toolchain_temporary.name).resolve() / "g++.exe"
        self.compiler.write_bytes(b"compiler-a")
        self.environment = {"PATH": str(self.compiler.parent), "GPU_MODE": "on"}
        self.cache = PreprocessCache((self.root / "cache").resolve())
        self.result_cache = ConfigurationAuditCache(
            (self.root / "audit-cache").resolve()
        )
        self.result_cache.prepare(time.monotonic() + 10.0)
        self.inspection_cache = CompilerInspectionCache(
            (self.root / "audit-cache").resolve()
        )
        self.dependency_roots = build_dependency_root_authority(
            self.root, {"toolchain": self.compiler.parent}
        )
        self.compiler_capability = open_compiler_executable_capability(
            self.compiler.resolve(),
            self.dependency_roots,
            time.monotonic() + 10.0,
            compiler_family=CompilerFamily.GCC,
        )

    def tearDown(self) -> None:
        self.compiler_capability.native_owner.close()
        _clear_compiler_inspection_memo_for_tests()
        self.temporary.cleanup()
        self.toolchain_temporary.cleanup()

    def write_source(self, relative: str, text: str = "int value;\n") -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path.resolve()

    @staticmethod
    def phase_accountant():
        memory = capability_model.WindowsRunMemoryMeasurements(
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, True
        )
        return SimpleNamespace(
            begin_phase=lambda _phase, _deadline: None,
            memory_measurements=lambda: memory,
        )

    def test_task9_smoke_public_entry_excludes_decision_and_evidence_authority(self):
        entry = getattr(
            capability_runner, "run_compiler_audit_smoke_pipeline", None)
        self.assertTrue(callable(entry), "production smoke entry is missing")
        parameters = inspect.signature(entry).parameters
        self.assertIn("runtime_contract", parameters)
        self.assertNotIn("decision_path", parameters)
        self.assertNotIn("evidence_suballocator", parameters)

    def test_task9_shared_core_preserves_authority_cache_and_preschedule_gates(self):
        core = inspect.getsource(
            capability_runner._run_compiler_audit_pipeline_core)
        self.assertLess(
            core.index("_validate_pipeline_cache_authorities"),
            core.index("enumerate_production_identities"))
        self.assertIn(
            "collection.dependency_root_authority is not dependency_roots", core)
        self.assertIn(
            "collection.capability_registry is not capability_registry", core)
        self.assertLess(
            core.index("len(ordered) != 1"),
            core.index("schedule_configuration_audits"))
        self.assertIn("if completed_reactor is None:", core)
        self.assertIn("scheduled task phase snapshot is unavailable", core)
        production_entry = inspect.getsource(
            capability_runner.run_compiler_audit_pipeline)
        self.assertIn("decision_path", production_entry)
        self.assertIn("evidence_suballocator", production_entry)
        self.assertIn("smoke_runtime_contract=None", production_entry)

    def test_task9_smoke_rejects_expired_runtime_before_engine_attestation(self):
        runtime = capability_model.WorkerRuntimeContract(
            1, 1, 1 << 20, time.monotonic() - 1.0)
        with mock.patch(
            "gpu_capability_source_audit._attest_loaded_audit_engine"
        ) as attest, self.assertRaisesRegex(
            AuditInfrastructureError, "smoke runtime contract"
        ):
            capability_runner.run_compiler_audit_smoke_pipeline(
                self.root, (self.root / "compile_commands.json",), {},
                self.dependency_roots, object(), object(), object(),
                AuditLimits(), runtime, time.monotonic() + 10.0,
                time.monotonic() + 10.0, object())
        attest.assert_not_called()

    def test_task9_linux_attempt_accountant_fails_closed_without_authority(self):
        with self.assertRaisesRegex(
            AuditInfrastructureError, "authority is incomplete"
        ):
            capability_runner.LinuxCompilerAuditAttemptAccountant(object())

    def test_task9_linux_attempt_accountant_owns_inspection_phase_cleanup(self):
        service = self.root / "service"
        run = service / "run"
        coordinator = run / "coordinator"
        coordinator.mkdir(parents=True)
        for owner in (service, run):
            (owner / "memory.events").write_text(
                "oom 0\noom_kill 0\nmax 0\n", encoding="ascii")
        for name, value in (
            ("memory.current", "1"), ("memory.peak", "2"),
            ("memory.max", str(512 << 20)),
            ("memory.high", str(448 << 20)),
        ):
            (run / name).write_text(value, encoding="ascii")
        (coordinator / "cgroup.procs").write_text(
            str(os.getpid()), encoding="ascii")

        class Backend:
            def coordinator_accounting_paths(_self):
                return coordinator, run, service
            def create_leaf(_self, *_args): return object()
            def acknowledge_leaf(_self, *_args): return None
            def release_leaf(_self, *_args): return None

        accountant = capability_runner.LinuxCompilerAuditAttemptAccountant(
            Backend())
        deadline = time.monotonic() + 10.0
        accountant.begin_phase("inspection", deadline)
        token = accountant.prepare_compiler_inspection_launch(object(), deadline)
        start = capability_model.ProcessStartIdentity(
            "linux", 123, "native", "a" * 64)
        event = capability_model.CompilerLaunchEvent(
            capability_model.CompilerLaunchPurpose.INSPECTION, start)
        carrier = object()
        accountant.register_compiler_process_launch(event, carrier)
        accountant.finish_compiler_inspection_launch_preparation(token)
        accountant.complete_compiler_process_launch(event, carrier)
        snapshot = accountant.seal_phase("inspection", deadline)
        self.assertEqual(accountant.inspection_probe_invocations, 1)
        self.assertTrue(snapshot.memory.accounting_complete)
        self.assertIs(
            accountant.seal_phase("inspection", deadline), snapshot
        )

    def test_task9_linux_attempt_accountant_rejects_any_foreign_coordinator_pid(self):
        service = self.root / "service-with-survivor"
        run = service / "run"
        coordinator = run / "coordinator"
        coordinator.mkdir(parents=True)
        for owner in (service, run):
            (owner / "memory.events").write_text(
                "oom 0\noom_kill 0\nmax 0\n", encoding="ascii"
            )
        for name, value in (
            ("memory.current", "1"), ("memory.peak", "2"),
            ("memory.max", str(512 << 20)),
            ("memory.high", str(448 << 20)),
        ):
            (run / name).write_text(value, encoding="ascii")
        (coordinator / "cgroup.procs").write_text(
            f"{os.getpid()}\n424242\n", encoding="ascii"
        )

        class Backend:
            def coordinator_accounting_paths(_self):
                return coordinator, run, service
            def create_leaf(_self, *_args): return object()
            def acknowledge_leaf(_self, *_args): return None
            def release_leaf(_self, *_args): return None

        accountant = capability_runner.LinuxCompilerAuditAttemptAccountant(
            Backend()
        )
        deadline = time.monotonic() + 10.0
        accountant.begin_phase("inspection", deadline)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "phase has survivors"
        ):
            accountant.seal_phase("inspection", deadline)

    def test_task9_linux_attempt_excludes_only_registered_spawn_tracker(self):
        run = self.root / "registered-helper" / "run"
        coordinator = run / "coordinator"
        coordinator.mkdir(parents=True)
        tracker_pid = 31337
        foreign_pid = 424242
        (coordinator / "cgroup.procs").write_text(
            f"{os.getpid()}\n{tracker_pid}\n{foreign_pid}\n",
            encoding="ascii",
        )
        accountant = object.__new__(
            capability_runner.LinuxCompilerAuditAttemptAccountant
        )
        accountant._phase = "tasks"
        accountant._sealed = {"inspection"}
        accountant._coordinator = coordinator
        accountant._run = run
        accountant._coordinator_process_carriers = {}
        carrier = mock.Mock(pid=tracker_pid)

        with mock.patch(
            "multiprocessing.resource_tracker._resource_tracker",
            SimpleNamespace(_pid=tracker_pid),
        ), mock.patch.object(
            capability_runner,
            "_LinuxCoordinatorProcessCarrier",
            return_value=carrier,
        ) as retain:
            accountant.register_spawn_resource_tracker()

        retain.assert_called_once_with(tracker_pid, coordinator)
        self.assertEqual(accountant._surviving_pids(), {foreign_pid})
        carrier.validate.assert_called_once_with(coordinator)
        accountant.close()
        carrier.close.assert_called_once_with()
        self.assertEqual(accountant._coordinator_process_carriers, {})

    def test_task9_linux_attempt_adopts_existing_tracker_before_inspection(self):
        coordinator = self.root / "existing-helper" / "run" / "coordinator"
        coordinator.mkdir(parents=True)
        tracker_pid = 31337
        accountant = object.__new__(
            capability_runner.LinuxCompilerAuditAttemptAccountant
        )
        accountant._phase = None
        accountant._sealed = set()
        accountant._sealed_snapshots = {}
        accountant._coordinator = coordinator
        accountant._coordinator_process_carriers = {}
        carrier = mock.Mock(pid=tracker_pid)
        deadline = time.monotonic() + 10.0

        with mock.patch.object(
            capability_runner.sys, "platform", "linux"
        ), mock.patch(
            "multiprocessing.resource_tracker._resource_tracker",
            SimpleNamespace(_pid=tracker_pid),
        ), mock.patch.object(
            capability_runner,
            "_LinuxCoordinatorProcessCarrier",
            return_value=carrier,
        ) as retain:
            accountant.adopt_existing_spawn_resource_tracker()
            accountant.begin_phase("inspection", deadline)
            accountant._sealed.add("inspection")
            accountant.begin_phase("tasks", deadline)
            accountant.register_spawn_resource_tracker()

        retain.assert_called_once_with(tracker_pid, coordinator)
        self.assertIs(
            accountant._coordinator_process_carriers[tracker_pid], carrier
        )
        carrier.validate.assert_called_once_with(coordinator)

    def test_task9_linux_spawn_tracker_carrier_revalidates_exact_identity(self):
        coordinator = (self.root / "exact-helper" / "run" / "coordinator")
        coordinator.mkdir(parents=True)
        pid = 31337
        snapshot = SimpleNamespace(
            pid=pid,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(
                os.fsencode(sys.executable),
                b"-c",
                b"from multiprocessing.resource_tracker import main;main(7)",
            ),
        )

        with mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            side_effect=(snapshot, snapshot, snapshot),
        ) as read_snapshot, mock.patch.object(
            os, "pidfd_open", return_value=91, create=True
        ) as pidfd_open, mock.patch.object(
            os, "fstat", return_value=SimpleNamespace()
        ) as fstat, mock.patch.object(os, "close") as close:
            carrier = capability_runner._LinuxCoordinatorProcessCarrier(
                pid, coordinator
            )
            carrier.validate(coordinator)
            carrier.close()

        self.assertEqual(read_snapshot.call_count, 3)
        pidfd_open.assert_called_once_with(pid)
        fstat.assert_called_once_with(91)
        close.assert_called_once_with(91)

    def test_task9_linux_spawn_tracker_carrier_rejects_wrong_command(self):
        coordinator = (self.root / "wrong-helper" / "run" / "coordinator")
        coordinator.mkdir(parents=True)
        snapshot = SimpleNamespace(
            pid=31337,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(os.fsencode(sys.executable), b"-c", b"print('foreign')"),
        )
        with mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            return_value=snapshot,
        ), mock.patch.object(
            os, "pidfd_open", create=True
        ) as pidfd_open, self.assertRaisesRegex(
            AuditInfrastructureError, "resource tracker identity"
        ):
            capability_runner._LinuxCoordinatorProcessCarrier(
                snapshot.pid, coordinator
            )
        pidfd_open.assert_not_called()

    def test_task9_linux_spawn_tracker_accepts_interpreter_flags(self):
        coordinator = (self.root / "flagged-helper" / "run" / "coordinator")
        coordinator.mkdir(parents=True)
        snapshot = SimpleNamespace(
            pid=31337,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(
                os.fsencode(sys.executable),
                b"-I",
                b"-W",
                b"default",
                b"-X",
                b"dev",
                b"-c",
                b"from multiprocessing.resource_tracker import main;main(7)",
            ),
        )

        with mock.patch(
            "multiprocessing.util._args_from_interpreter_flags",
            return_value=["-I", "-W", "default", "-X", "dev"],
        ), mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            side_effect=(snapshot, snapshot),
        ), mock.patch.object(
            os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(os, "close"):
            carrier = capability_runner._LinuxCoordinatorProcessCarrier(
                snapshot.pid, coordinator
            )
            carrier.close()

    def test_task9_linux_spawn_tracker_accepts_zero_pipe_fd(self):
        coordinator = (self.root / "fd-zero-helper" / "run" / "coordinator")
        coordinator.mkdir(parents=True)
        snapshot = SimpleNamespace(
            pid=31337,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(
                os.fsencode(sys.executable),
                b"-c",
                b"from multiprocessing.resource_tracker import main;main(0)",
            ),
        )

        with mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            side_effect=(snapshot, snapshot),
        ), mock.patch.object(
            os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(os, "close"):
            carrier = capability_runner._LinuxCoordinatorProcessCarrier(
                snapshot.pid, coordinator
            )
            carrier.close()

    def test_task9_linux_spawn_tracker_rejects_noncanonical_pipe_fd(self):
        coordinator = (
            self.root / "noncanonical-fd-helper" / "run" / "coordinator"
        )
        coordinator.mkdir(parents=True)
        snapshot = SimpleNamespace(
            pid=31337,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(
                os.fsencode(sys.executable),
                b"-c",
                b"from multiprocessing.resource_tracker import main;main(00)",
            ),
        )

        with mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            return_value=snapshot,
        ), mock.patch.object(
            os, "pidfd_open", create=True
        ) as pidfd_open, self.assertRaisesRegex(
            AuditInfrastructureError, "resource tracker identity"
        ):
            capability_runner._LinuxCoordinatorProcessCarrier(
                snapshot.pid, coordinator
            )

        pidfd_open.assert_not_called()

    def test_task9_linux_spawn_tracker_closes_pidfd_when_assignment_fails(self):
        coordinator = (
            self.root / "assignment-failure" / "run" / "coordinator"
        )
        coordinator.mkdir(parents=True)
        snapshot = SimpleNamespace(
            pid=31337,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(
                os.fsencode(sys.executable),
                b"-c",
                b"from multiprocessing.resource_tracker import main;main(7)",
            ),
        )

        class AssignmentFailingCarrier(
            capability_runner._LinuxCoordinatorProcessCarrier
        ):
            def __setattr__(self, name, value):
                if name == "pid":
                    raise MemoryError("assignment failed")
                super().__setattr__(name, value)

        with mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            side_effect=(snapshot, snapshot),
        ), mock.patch.object(
            os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(os, "close") as close, self.assertRaisesRegex(
            MemoryError, "assignment failed"
        ):
            AssignmentFailingCarrier(snapshot.pid, coordinator)

        close.assert_called_once_with(91)

    def test_task9_linux_spawn_tracker_preserves_identity_error_when_close_fails(self):
        coordinator = (self.root / "close-failure" / "run" / "coordinator")
        coordinator.mkdir(parents=True)
        valid = SimpleNamespace(
            pid=31337,
            parent_pid=os.getpid(),
            start_identity="linux-proc:boot:99",
            cgroup_path=coordinator.resolve(),
            executable_path=Path(sys.executable).resolve(),
            argv=(
                os.fsencode(sys.executable),
                b"-c",
                b"from multiprocessing.resource_tracker import main;main(7)",
            ),
        )
        changed = SimpleNamespace(**{**vars(valid), "parent_pid": os.getpid() + 1})

        with mock.patch.object(
            capability_runner,
            "_read_linux_coordinator_process_snapshot",
            side_effect=(valid, changed),
        ), mock.patch.object(
            os, "pidfd_open", return_value=91, create=True
        ), mock.patch.object(
            os, "close", side_effect=OSError("close failed")
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "resource tracker identity"
        ) as raised:
            capability_runner._LinuxCoordinatorProcessCarrier(
                valid.pid, coordinator
            )

        self.assertTrue(
            any("close failed" in note for note in raised.exception.__notes__)
        )

    def test_task9_linux_spawn_tracker_closes_carrier_when_registration_fails(self):
        coordinator = (self.root / "registration-failure" / "run" / "coordinator")
        coordinator.mkdir(parents=True)
        tracker_pid = 31337
        accountant = object.__new__(
            capability_runner.LinuxCompilerAuditAttemptAccountant
        )
        accountant._coordinator = coordinator
        carriers = mock.MagicMock()
        carriers.get.return_value = None
        carriers.__bool__.return_value = False
        carriers.__setitem__.side_effect = RuntimeError("registration failed")
        accountant._coordinator_process_carriers = carriers
        carrier = mock.Mock(pid=tracker_pid)

        with mock.patch(
            "multiprocessing.resource_tracker._resource_tracker",
            SimpleNamespace(_pid=tracker_pid),
        ), mock.patch.object(
            capability_runner,
            "_LinuxCoordinatorProcessCarrier",
            return_value=carrier,
        ), self.assertRaisesRegex(RuntimeError, "registration failed"):
            accountant._retain_spawn_resource_tracker(required=True)

        carrier.close.assert_called_once_with()

    def test_task9_linux_reactor_registers_spawn_tracker_after_event_creation(self):
        backend = SimpleNamespace(
            hello=mock.Mock(),
            create_leaf=mock.Mock(),
            acknowledge_leaf=mock.Mock(),
            release_leaf=mock.Mock(),
        )
        accountant = object.__new__(
            capability_runner.LinuxCompilerAuditAttemptAccountant
        )
        accountant._client = backend
        order = []
        accountant.register_spawn_resource_tracker = mock.Mock(
            side_effect=lambda: order.append("register")
        )
        context = SimpleNamespace(Event=mock.Mock(
            side_effect=lambda: (
                order.append("event"), threading.Event()
            )[1]
        ))
        runtime = capability_model.WorkerRuntimeContract(
            1, 1, 1 << 30, time.monotonic() + 10.0
        )

        with mock.patch.object(
            capability_runner.multiprocessing,
            "get_context",
            return_value=context,
        ), mock.patch.object(capability_runner.sys, "platform", "linux"):
            capability_runner.GenerationReactor(
                configurations=(),
                dependency_roots=object(),
                production_snapshot={},
                cache_root=self.root / "reactor-register-helper",
                limits=AuditLimits(),
                engine="e" * 64,
                runtime_contract=runtime,
                run_accountant=accountant,
                capability_registry=object(),
                result_budget=SimpleNamespace(observer=object()),
                worker_count=0,
                cleanup_deadline=runtime.pipeline_deadline,
            )

        context.Event.assert_called_once_with()
        accountant.register_spawn_resource_tracker.assert_called_once_with()
        self.assertEqual(order, ["event", "register"])

    def test_task8_public_run_and_production_limits_are_exact(self):
        self.assertEqual(
            tuple(field.name for field in dataclasses.fields(CompilerAuditRun)),
            ("summary", "measurements"),
        )
        summary = CanonicalAuditContentSummary(
            "a" * 64, ("b" * 64,), 1, 2, 3
        )
        for forbidden in ("results", "findings", "coverage"):
            self.assertNotIn(
                forbidden,
                tuple(field.name for field in dataclasses.fields(CompilerAuditRun)),
            )
        self.assertEqual(summary.finding_count, 1)
        limits = AuditLimits()
        self.assertEqual(limits.production_raw_per_file_bytes, 8 << 20)
        self.assertEqual(limits.production_raw_aggregate_bytes, 64 << 20)
        self.assertEqual(limits.production_decoded_transient_bytes, 256 << 20)
        self.assertEqual(limits.production_decoded_retained_bytes, 128 << 20)

    def test_task8_active_sources_and_source_only_use_complete_production(self):
        generic_path = self.write_source("playback/generic.cpp")
        inactive_path = self.write_source(
            "playback/gpu/applegpusurface_apple.mm"
        )
        generic = self.identity(generic_path, "playback/generic.cpp")
        inactive = self.identity(
            inactive_path, "playback/gpu/applegpusurface_apple.mm"
        )
        configuration = self.configuration(generic, "a" * 64)
        production = {
            generic.relative: generic,
            inactive.relative: inactive,
        }
        active, configured = compute_active_sources(
            (configuration,), production
        )
        self.assertEqual(active, frozenset((generic.relative,)))
        self.assertEqual(configured, frozenset((generic.relative,)))
        authoritative = frozenset((generic.relative,))
        self.assertEqual(
            frozenset(production) - authoritative,
            frozenset((inactive.relative,)),
        )

    def test_task8_snapshot_detects_mutate_restore_and_decode_is_strict(self):
        path = self.write_source("playback/snapshot.cpp", "A")
        identity = self.identity(path, "playback/snapshot.cpp")
        production = {identity.relative: identity}
        with snapshot_production_sources(
            production, AuditLimits(), time.monotonic() + 10.0
        ) as held:
            self.assertEqual(
                tuple(held.initial_digest_map), (identity.relative,)
            )
            if os.name == "nt":
                with self.assertRaises(OSError):
                    path.write_bytes(b"B")
                held.finalize_policy_boundary(time.monotonic() + 10.0)
            else:
                path.write_bytes(b"B")
                path.write_bytes(b"A")
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "production snapshot generation"
                ):
                    held.finalize_policy_boundary(time.monotonic() + 10.0)

        invalid_path = self.write_source("playback/invalid.cpp", "ok")
        invalid_identity = self.identity(invalid_path, "playback/invalid.cpp")
        invalid_path.write_bytes(b"\xff")
        invalid_identity = dataclasses.replace(
            invalid_identity, line_count=0
        )
        with snapshot_production_sources(
            {invalid_identity.relative: invalid_identity},
            AuditLimits(), time.monotonic() + 10.0,
        ) as held:
            sources = held.finalize_policy_boundary(time.monotonic() + 10.0)
            with self.assertRaisesRegex(AuditInfrastructureError, "UTF-8"):
                decode_validated_production_sources(
                    sources, AuditLimits(), time.monotonic() + 10.0
                )

    def test_task8_closed_decoded_mapping_drops_carrier_before_reservation(self):
        path = self.write_source("playback/closed-decoded.cpp", "stable")
        identity = self.identity(path, "playback/closed-decoded.cpp")
        with snapshot_production_sources(
            {identity.relative: identity}, AuditLimits(),
            time.monotonic() + 10.0,
        ) as held:
            raw = held.finalize_policy_boundary(time.monotonic() + 10.0)
            decoded = decode_validated_production_sources(
                raw, AuditLimits(), time.monotonic() + 10.0
            )
            budget = decoded.allocation_budget
            decoded.close()
            with self.assertRaisesRegex(
                AuditInfrastructureError, "production mapping is closed"
            ):
                decoded[identity.relative]
            self.assertNotIn(identity.relative, decoded._values)
            self.assertGreater(budget.current_bytes, 0)

    def test_task8_held_snapshot_invalidates_raw_mapping_before_raw_release(self):
        path = self.write_source("playback/borrowed-raw.cpp", "stable")
        identity = self.identity(path, "playback/borrowed-raw.cpp")
        with snapshot_production_sources(
            {identity.relative: identity}, AuditLimits(),
            time.monotonic() + 10.0,
        ) as held:
            raw = held.finalize_policy_boundary(time.monotonic() + 10.0)
            self.assertEqual(raw[identity.relative].raw_bytes, b"stable")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "production mapping is closed"
        ):
            raw[identity.relative]
        self.assertNotIn(identity.relative, raw._values)
        self.assertEqual(held._budget.current_bytes, 0)

    def test_task8_compact_result_must_reach_its_own_main(self):
        path = self.write_source("playback/own-main.cpp")
        identity = self.identity(path, "playback/own-main.cpp")
        configuration = self.configuration(identity, "a" * 64)
        result = capability_model.ConfigurationAuditResult(
            configuration.digest,
            "e" * 64,
            (),
            (PurePosixPath("playback/other.cpp"),),
            (),
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits(),
            require_main_provenance=True,
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "main-source provenance"
        ):
            aggregator.accept_validated_result(
                configuration, result, ownership
            )
        if not ownership.released:
            ownership.release()

    def test_task8_empty_main_proof_becomes_authoritative_outer_coverage(self):
        path = self.write_source("playback/empty-main.cpp", "")
        identity = self.identity(path, "playback/empty-main.cpp")
        self.assertEqual(identity.line_count, 0)
        configuration = self.configuration(identity, "a" * 64)
        dependency = DependencyDigest(
            "production", identity.relative, identity,
            hashlib.sha256(b"").hexdigest(),
        )
        result = capability_model.ConfigurationAuditResult(
            configuration.digest, "e" * 64, (dependency,), (), ()
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits(),
            require_main_provenance=True,
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        aggregator.accept_validated_result(configuration, result, ownership)
        summary = aggregator.finish()
        self.assertEqual(summary.reached_production, (identity.relative,))
        builder = capability_runner.BoundedCanonicalAuditContentSummaryBuilder(
            AuditLimits(), budget
        )
        growth = builder.premeasure_policy_merge_working_state(
            {identity.relative: ""}, summary, time.monotonic() + 10.0
        )
        budget.reserve_aggregate_growth(summary.budget_ownership, growth)
        budget.commit_aggregate_growth(summary.budget_ownership)
        builder.merge_findings_into_reserved_backing_state(
            {identity.relative: ""}, summary,
            {identity.relative: identity}, frozenset((identity.relative,)),
            summary.budget_ownership, time.monotonic() + 10.0,
        )
        canonical = builder.build_bounded_digest_and_count_summary(summary)
        self.assertEqual(canonical.authoritative_path_count, 1)
        builder.release_backing_state()
        summary.release()

    def test_task8_exact_conditional_translation_unit_table(self):
        expected = {
            PurePosixPath("playback/gpu/gpusurface_apple.mm"): "apple",
            PurePosixPath("playback/gpu/gpufence_apple.mm"): "apple",
            PurePosixPath("playback/gpu/applegpusurface_apple.mm"): "apple",
            PurePosixPath(
                "recorder_engine/codec/nativevideoencoder_videotoolbox.mm"
            ): "apple",
            PurePosixPath("playback/gpu/gpufence_win.cpp"): "windows",
            PurePosixPath("playback/output/win/wingpuimportedge.cpp"): "windows",
            PurePosixPath("playback/output/win/d3d11gpusurface.cpp"): "windows",
            PurePosixPath(
                "recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"
            ): "windows",
        }
        self.assertEqual(
            dict(capability_runner._CONDITIONALLY_SELECTED_TRANSLATION_UNITS),
            expected,
        )
        identities = {}
        for relative in (*expected, PurePosixPath("playback/ordinary.cpp")):
            path = self.write_source(relative.as_posix())
            identities[relative] = self.identity(path, relative.as_posix())
        ordinary = self.configuration(
            identities[PurePosixPath("playback/ordinary.cpp")], "1" * 64
        )
        active, _configured = compute_active_sources(
            (ordinary,), identities
        )
        self.assertEqual(
            active, frozenset((PurePosixPath("playback/ordinary.cpp"),))
        )
        apple = self.configuration(
            identities[PurePosixPath("playback/gpu/gpusurface_apple.mm")],
            "2" * 64,
        )
        active, _configured = compute_active_sources(
            (ordinary, apple), identities
        )
        self.assertEqual(
            {path for path in active if expected.get(path) == "apple"},
            {path for path, platform in expected.items() if platform == "apple"},
        )
        windows = self.configuration(
            identities[PurePosixPath("playback/gpu/gpufence_win.cpp")],
            "3" * 64,
        )
        active, _configured = compute_active_sources(
            (ordinary, windows), identities
        )
        self.assertEqual(
            {path for path in active if expected.get(path) == "windows"},
            {path for path, platform in expected.items() if platform == "windows"},
        )

    def test_task8_production_caps_are_exact_and_reserved_before_construction(self):
        limits = dataclasses.replace(
            AuditLimits(),
            production_raw_per_file_bytes=8,
            production_raw_aggregate_bytes=16,
            production_decoded_transient_bytes=65536,
            production_decoded_retained_bytes=65536,
        )
        first = self.write_source("playback/cap-a.cpp", "12345678")
        second = self.write_source("playback/cap-b.cpp", "abcdefgh")
        identities = {
            PurePosixPath("playback/cap-a.cpp"): self.identity(
                first, "playback/cap-a.cpp"
            ),
            PurePosixPath("playback/cap-b.cpp"): self.identity(
                second, "playback/cap-b.cpp"
            ),
        }
        events = []
        with mock.patch.object(
            capability_runner,
            "_production_allocation_event",
            side_effect=events.append,
        ):
            with snapshot_production_sources(
                identities, limits, time.monotonic() + 10.0
            ) as held:
                snapshots = held.finalize_policy_boundary(
                    time.monotonic() + 10.0
                )
                decoded = decode_validated_production_sources(
                    snapshots, limits, time.monotonic() + 10.0
                )
        self.assertEqual(decoded[PurePosixPath("playback/cap-a.cpp")], "12345678")
        budget = decoded.allocation_budget
        decoded.close()
        self.assertEqual(budget.current_bytes, 0)
        self.assertEqual(
            [event for event in events if event not in {"read-final", "open"}],
            [
                "reserve-snapshot-structure",
                "construct-frozen-table",
                "reserve-raw", "construct-initial-digest-map", "read", "read",
                "construct-final-snapshot-map",
                "reserve-decode", "reserve-retained-strings",
                "transfer-retained-before-construction", "decode",
                "construct-strings", "construct-decoded-map",
            ],
        )
        for event in (
            "construct-frozen-table",
            "construct-initial-digest-map",
            "construct-final-snapshot-map",
            "construct-decoded-map",
        ):
            self.assertEqual(events.count(event), 1)
        first.write_bytes(b"123456789")
        too_large = self.identity(first, "playback/cap-a.cpp")
        with self.assertRaisesRegex(AuditInfrastructureError, "per-file limit"):
            snapshot_production_sources(
                {too_large.relative: too_large}, limits,
                time.monotonic() + 10.0,
            )
        first.write_bytes(b"12345678")
        identities[PurePosixPath("playback/cap-a.cpp")] = self.identity(
            first, "playback/cap-a.cpp"
        )
        third = self.write_source("playback/cap-c.cpp", "x")
        third_identity = self.identity(third, "playback/cap-c.cpp")
        with self.assertRaisesRegex(AuditInfrastructureError, "aggregate limit"):
            snapshot_production_sources(
                {**identities, third_identity.relative: third_identity}, limits,
                time.monotonic() + 10.0,
            )

        invalid = self.write_source("playback/cap-invalid.cpp", "x")
        invalid.write_bytes(b"\xff")
        invalid_identity = self.identity(
            self.write_source("playback/cap-invalid-identity.cpp", "x"),
            "playback/cap-invalid.cpp",
        )
        invalid_identity = dataclasses.replace(
            invalid_identity,
            canonical=invalid.resolve(),
            device=int(invalid.stat().st_dev),
            inode=int(invalid.stat().st_ino) or None,
        )
        with snapshot_production_sources(
            {invalid_identity.relative: invalid_identity}, limits,
            time.monotonic() + 10.0,
        ) as held:
            snapshots = held.finalize_policy_boundary(time.monotonic() + 10.0)
            raw_live = snapshots.allocation_budget.current_bytes
            with self.assertRaisesRegex(AuditInfrastructureError, "UTF-8"):
                decode_validated_production_sources(
                    snapshots, limits, time.monotonic() + 10.0
                )
            self.assertEqual(snapshots.allocation_budget.current_bytes, raw_live)

    def test_task8_snapshot_structure_is_reserved_before_any_descriptor_open(self):
        identities = {}
        for index in range(64):
            relative = f"playback/empty-{index:03d}.cpp"
            path = self.write_source(relative, "")
            identity = self.identity(path, relative)
            identities[identity.relative] = identity
        events = []
        original_open = capability_runner._open_production_descriptor

        def tracked_open(path):
            events.append("open")
            return original_open(path)

        with mock.patch.object(
            capability_runner,
            "_production_allocation_event",
            side_effect=events.append,
        ), mock.patch.object(
            capability_runner,
            "_open_production_descriptor",
            side_effect=tracked_open,
        ):
            with snapshot_production_sources(
                identities, AuditLimits(), time.monotonic() + 10.0
            ) as held:
                self.assertGreater(held._budget.current_bytes, 0)
                held.finalize_policy_boundary(time.monotonic() + 10.0)
        self.assertEqual(events[0], "reserve-snapshot-structure")
        self.assertEqual(held._budget.current_bytes, 0)

    def test_task8_snapshot_descriptor_ceiling_fails_before_table_or_open(self):
        class OversizedProduction(Mapping):
            def __len__(self):
                return AuditLimits().unique_dependency_handles + 1

            def __iter__(self):
                raise AssertionError("oversized table was iterated")

            def __getitem__(self, _key):
                raise AssertionError("oversized table was indexed")

        events = []
        with mock.patch.object(
            capability_runner,
            "_production_allocation_event",
            side_effect=events.append,
        ), mock.patch.object(
            capability_runner, "_open_production_descriptor"
        ) as opened, self.assertRaisesRegex(
            AuditInfrastructureError, "descriptor ceiling"
        ):
            snapshot_production_sources(
                OversizedProduction(), AuditLimits(), time.monotonic() + 10.0
            )
        opened.assert_not_called()
        self.assertEqual(events, [])

    def test_task8_snapshot_bound_uses_allocation_free_path_lengths(self):
        path = self.write_source("playback/allocation-free.cpp", "")
        identity = self.identity(path, "playback/allocation-free.cpp")
        with mock.patch.object(
            PurePosixPath,
            "as_posix",
            side_effect=AssertionError("as_posix allocated before reservation"),
        ), mock.patch.object(
            PurePosixPath,
            "parts",
            new_callable=mock.PropertyMock,
            side_effect=AssertionError("parts allocated before reservation"),
        ), mock.patch.object(
            Path,
            "__str__",
            side_effect=AssertionError("str(Path) allocated before reservation"),
        ):
            bound = capability_runner._production_snapshot_structure_bound(
                {identity.relative: identity}
            )
        self.assertGreater(bound, 0)

    def test_task8_snapshot_bound_charges_three_simultaneous_dicts(self):
        self.assertEqual(
            capability_runner._production_snapshot_structure_dict_count(), 3
        )

    def test_task8_decode_reuses_snapshot_order_without_path_sort_allocations(self):
        path = self.write_source("playback/decode-order.cpp", "stable")
        identity = self.identity(path, "playback/decode-order.cpp")
        with snapshot_production_sources(
            {identity.relative: identity}, AuditLimits(),
            time.monotonic() + 10.0,
        ) as held:
            snapshots = held.finalize_policy_boundary(time.monotonic() + 10.0)
            with mock.patch.object(
                PurePosixPath,
                "as_posix",
                side_effect=AssertionError("decode allocated path sort text"),
            ), mock.patch.object(
                PurePosixPath,
                "parts",
                new_callable=mock.PropertyMock,
                side_effect=AssertionError("decode allocated path sort parts"),
            ):
                decoded = decode_validated_production_sources(
                    snapshots, AuditLimits(), time.monotonic() + 10.0
                )
        try:
            self.assertEqual(decoded[identity.relative], "stable")
        finally:
            decoded.close()

    def test_task8_snapshot_uses_one_open_and_two_reads_on_same_descriptor(self):
        path = self.write_source("playback/twice.cpp", "stable")
        identity = self.identity(path, "playback/twice.cpp")
        opened = []
        original_open = capability_runner._open_production_descriptor

        class TrackingStream:
            def __init__(self, stream):
                self.stream = stream
                self.read_count = 0

            def fileno(self):
                return self.stream.fileno()

            def read(self, *args, **kwargs):
                self.read_count += 1
                return self.stream.read(*args, **kwargs)

            def seek(self, *args, **kwargs):
                return self.stream.seek(*args, **kwargs)

            def close(self):
                return self.stream.close()

        def tracked_open(selected):
            stream = TrackingStream(original_open(selected))
            opened.append(stream)
            return stream

        with mock.patch.object(
            capability_runner, "_open_production_descriptor", tracked_open
        ):
            with snapshot_production_sources(
                {identity.relative: identity}, AuditLimits(),
                time.monotonic() + 10.0,
            ) as held:
                held.finalize_policy_boundary(time.monotonic() + 10.0)
        self.assertEqual(len(opened), 1)
        self.assertEqual(opened[0].read_count, 2)

    def test_task8_decoded_caps_accept_exact_and_reject_plus_one(self):
        path = self.write_source("playback/decode-cap.cpp", "ab")
        identity = self.identity(path, "playback/decode-cap.cpp")
        with snapshot_production_sources(
            {identity.relative: identity}, AuditLimits(),
            time.monotonic() + 10.0,
        ) as held:
            baseline = held.finalize_policy_boundary(time.monotonic() + 10.0)
            bounds = capability_runner._production_decode_allocation_bounds(
                baseline
            )
            structure_bytes = (
                capability_runner._production_snapshot_structure_bound(
                    {identity.relative: identity}
                )
            )
        exact = dataclasses.replace(
            AuditLimits(),
            production_raw_per_file_bytes=2,
            production_raw_aggregate_bytes=2,
            production_decoded_transient_bytes=(
                structure_bytes + bounds.transient_bytes
            ),
            production_decoded_retained_bytes=(
                structure_bytes + bounds.retained_bytes
            ),
        )
        with snapshot_production_sources(
            {identity.relative: identity}, exact, time.monotonic() + 10.0
        ) as held:
            snapshots = held.finalize_policy_boundary(time.monotonic() + 10.0)
            decoded = decode_validated_production_sources(
                snapshots, exact, time.monotonic() + 10.0
            )
            decoded.close()
        for field, value, message in (
            ("production_decoded_transient_bytes",
             structure_bytes + bounds.transient_bytes - 1,
             "transient limit"),
            ("production_decoded_retained_bytes",
             structure_bytes + bounds.retained_bytes - 1,
             "retained limit"),
        ):
            limits = dataclasses.replace(exact, **{field: value})
            with snapshot_production_sources(
                {identity.relative: identity}, limits,
                time.monotonic() + 10.0,
            ) as held:
                snapshots = held.finalize_policy_boundary(
                    time.monotonic() + 10.0
                )
                raw_live = snapshots.allocation_budget.current_bytes
                with self.assertRaisesRegex(AuditInfrastructureError, message):
                    decode_validated_production_sources(
                        snapshots, limits, time.monotonic() + 10.0
                    )
                self.assertEqual(
                    snapshots.allocation_budget.current_bytes, raw_live
                )

    def test_task8_decode_schema_precharges_scratch_objects_and_peak_overlap(self):
        path = self.write_source("playback/decode-schema.cpp", "abc")
        identity = self.identity(path, "playback/decode-schema.cpp")
        with snapshot_production_sources(
            {identity.relative: identity}, AuditLimits(),
            time.monotonic() + 10.0,
        ) as held:
            snapshots = held.finalize_policy_boundary(time.monotonic() + 10.0)
            bounds = capability_runner._production_decode_allocation_bounds(
                snapshots
            )
            self.assertGreater(bounds.transient_bytes, 4 * 3)
            self.assertGreater(bounds.retained_bytes, 4 * 3)
            self.assertGreater(bounds.decoder_scratch_bytes, 0)
            decoded = decode_validated_production_sources(
                snapshots, AuditLimits(), time.monotonic() + 10.0
            )
            self.assertLessEqual(
                decoded.allocation_budget.peak_bytes,
                (64 + 256) << 20,
            )
            self.assertLessEqual(
                decoded.allocation_budget.peak_bytes + (128 << 20),
                448 << 20,
            )
            decoded.close()
            self.assertEqual(
                snapshots.allocation_budget.current_bytes,
                (64 << 20) + capability_runner._production_snapshot_structure_bound(
                    {identity.relative: identity}
                ),
            )

    def test_task8_snapshot_reads_only_the_pre_reserved_fstat_extent(self):
        path = self.write_source("playback/read-extent.cpp", "stable")
        identity = self.identity(path, "playback/read-extent.cpp")
        requested = []
        original_open = capability_runner._open_production_descriptor

        class TrackingStream:
            def __init__(self, stream):
                self.stream = stream

            def fileno(self):
                return self.stream.fileno()

            def read(self, size):
                requested.append(size)
                return self.stream.read(size)

            def seek(self, *args):
                return self.stream.seek(*args)

            def close(self):
                return self.stream.close()

        with mock.patch.object(
            capability_runner,
            "_open_production_descriptor",
            side_effect=lambda selected: TrackingStream(original_open(selected)),
        ):
            with snapshot_production_sources(
                {identity.relative: identity}, AuditLimits(),
                time.monotonic() + 10.0,
            ) as held:
                held.finalize_policy_boundary(time.monotonic() + 10.0)
        self.assertEqual(requested, [len(b"stable"), len(b"stable")])

    def test_task8_same_content_path_replacement_cannot_pass_final_boundary(self):
        path = self.write_source("playback/replaced.cpp", "same")
        identity = self.identity(path, "playback/replaced.cpp")
        with snapshot_production_sources(
            {identity.relative: identity}, AuditLimits(),
            time.monotonic() + 10.0,
        ) as held:
            replacement = path.with_suffix(".replacement")
            replacement.write_bytes(b"same")
            if os.name == "nt":
                with self.assertRaises(OSError):
                    os.replace(replacement, path)
                held.finalize_policy_boundary(time.monotonic() + 10.0)
            else:
                os.replace(replacement, path)
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "generation differs"
                ):
                    held.finalize_policy_boundary(time.monotonic() + 10.0)

    def test_task8_aggregate_release_is_atomic_and_results_are_not_retained(self):
        path = self.write_source("playback/aggregate.cpp")
        identity = self.identity(path, "playback/aggregate.cpp")
        configuration = self.configuration(identity, "a" * 64)
        class TrackedResult(capability_model.ConfigurationAuditResult):
            __slots__ = ("__weakref__",)

        result = TrackedResult(
            configuration.digest, "e" * 64, (), (identity.relative,), ()
        )
        result_ref = weakref.ref(result)
        observer = capability_model.CompactAccountingObserver()
        budget = CompactResultMemoryBudget(observer=observer)
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        ownership.record_semantic(
            "retain-hit", delta_bytes=ownership.byte_count
        )
        aggregator.accept_validated_result(configuration, result, ownership)
        del result
        gc.collect()
        self.assertIsNone(result_ref())
        summary = aggregator.finish()
        before = budget.live_bytes
        with mock.patch.object(
            capability_model,
            "_before_compact_atomic_release",
            side_effect=RuntimeError("release fault"),
        ), self.assertRaisesRegex(RuntimeError, "release fault"):
            summary.release()
        self.assertEqual(budget.live_bytes, before)
        self.assertFalse(summary.budget_ownership.released)
        summary.release()
        self.assertEqual(budget.live_bytes, 0)

    def test_task8_pending_policy_growth_release_is_atomic_and_retryable(self):
        path = self.write_source("playback/pending-growth.cpp")
        identity = self.identity(path, "playback/pending-growth.cpp")
        configuration = self.configuration(identity, "a" * 64)
        budget = CompactResultMemoryBudget(
            observer=capability_model.CompactAccountingObserver()
        )
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        result = capability_model.ConfigurationAuditResult(
            configuration.digest, "e" * 64, (), (identity.relative,), ()
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        ownership.record_semantic(
            "retain-hit", delta_bytes=ownership.byte_count
        )
        aggregator.accept_validated_result(configuration, result, ownership)
        summary = aggregator.finish()
        budget.reserve_aggregate_growth(summary.budget_ownership, 4096)
        before = budget.live_bytes
        with mock.patch.object(
            capability_model,
            "_before_compact_atomic_release",
            side_effect=lambda index: (
                (_ for _ in ()).throw(RuntimeError("pending release fault"))
                if index == 1 else None
            ),
        ), self.assertRaisesRegex(RuntimeError, "pending release fault"):
            summary.release()
        self.assertEqual(budget.live_bytes, before)
        self.assertFalse(summary.budget_ownership.released)
        summary.release()
        self.assertEqual(budget.live_bytes, 0)

    def test_task8_session_refuses_consumption_with_nonempty_reorder_window(self):
        session = capability_runner._ScheduledAuditSession(
            aggregate=mock.sentinel.aggregate,
            cache_hits=1,
            cache_misses=0,
            inspection_probe_invocations=0,
            audit_compiler_invocations=2,
            runtime_contract=capability_model.WorkerRuntimeContract(
                1, 1, 0, time.monotonic() + 10.0
            ),
            cache_root=self.root,
            result_budget=CompactResultMemoryBudget(),
            compact_accounting_observer=capability_model.CompactAccountingObserver(),
            run_accountant=object(),
            capability_registry=object(),
            reorder_pending_count=1,
            cache_maximum_encoded_result_bytes=4096,
            cache_maximum_conservative_decoded_bytes=16384,
            cache_maximum_conservative_retained_bytes=2048,
        )
        self.assertEqual(session.worker_counts_started, ())
        with self.assertRaisesRegex(AuditInfrastructureError, "reorder window"):
            session.consume_aggregate()

    def test_task8_session_combines_warm_and_cold_authenticated_measurements(self):
        reactor = SimpleNamespace(
            stdout_bytes=17,
            maximum_encoded_result_bytes=3000,
            maximum_conservative_decoded_bytes=50000,
            maximum_conservative_retained_bytes=1500,
            worker_pids=[101],
            states=[None],
        )
        session = capability_runner._ScheduledAuditSession(
            aggregate=mock.sentinel.aggregate,
            cache_hits=1,
            cache_misses=1,
            inspection_probe_invocations=0,
            audit_compiler_invocations=2,
            runtime_contract=capability_model.WorkerRuntimeContract(
                4, 1, 0, time.monotonic() + 10.0
            ),
            cache_root=self.root,
            result_budget=CompactResultMemoryBudget(),
            compact_accounting_observer=capability_model.CompactAccountingObserver(),
            run_accountant=object(),
            capability_registry=object(),
            reactor=reactor,
            cache_maximum_encoded_result_bytes=4096,
            cache_maximum_conservative_decoded_bytes=40000,
            cache_maximum_conservative_retained_bytes=2048,
        )
        self.assertEqual(session.stdout_bytes, 17)
        self.assertEqual(session.maximum_encoded_result_bytes, 4096)
        self.assertEqual(session.maximum_conservative_decoded_bytes, 50000)
        self.assertEqual(session.maximum_conservative_retained_bytes, 2048)
        self.assertEqual(session.worker_counts_started, (1,))

    def test_task8_session_rejects_missing_cold_measurements(self):
        with self.assertRaisesRegex(
            AuditInfrastructureError, "cold-miss measurements"
        ):
            capability_runner._ScheduledAuditSession(
                aggregate=mock.sentinel.aggregate,
                cache_hits=0,
                cache_misses=1,
                inspection_probe_invocations=0,
                audit_compiler_invocations=2,
                runtime_contract=capability_model.WorkerRuntimeContract(
                    1, 1, 0, time.monotonic() + 10.0
                ),
                cache_root=self.root,
                result_budget=CompactResultMemoryBudget(),
                compact_accounting_observer=(
                    capability_model.CompactAccountingObserver()
                ),
                run_accountant=object(),
                capability_registry=object(),
                reactor=SimpleNamespace(worker_pids=[]),
            )

    def test_task8_session_accepts_authenticated_cold_zero_stdout(self):
        reactor = SimpleNamespace(
            stdout_bytes=0,
            maximum_encoded_result_bytes=1024,
            maximum_conservative_decoded_bytes=17408,
            maximum_conservative_retained_bytes=512,
            worker_pids=[202],
            states=[None],
        )
        session = capability_runner._ScheduledAuditSession(
            aggregate=mock.sentinel.aggregate,
            cache_hits=0,
            cache_misses=1,
            inspection_probe_invocations=0,
            audit_compiler_invocations=2,
            runtime_contract=capability_model.WorkerRuntimeContract(
                4, 1, 0, time.monotonic() + 10.0
            ),
            cache_root=self.root,
            result_budget=CompactResultMemoryBudget(),
            compact_accounting_observer=capability_model.CompactAccountingObserver(),
            run_accountant=object(),
            capability_registry=object(),
            reactor=reactor,
        )
        self.assertEqual(session.stdout_bytes, 0)
        self.assertEqual(session.maximum_encoded_result_bytes, 1024)
        self.assertEqual(session.worker_counts_started, (1,))

    def test_task8_251_disjoint_results_release_all_compact_ownership(self):
        path = self.write_source("playback/maximal.cpp")
        identity = self.identity(path, "playback/maximal.cpp")
        base = self.configuration(identity, "0" * 64)
        configurations = tuple(
            dataclasses.replace(base, digest=f"{index:064x}")
            for index in range(251)
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            configurations, budget, AuditLimits()
        )
        for index, configuration in enumerate(configurations):
            result = capability_model.ConfigurationAuditResult(
                configuration.digest,
                "e" * 64,
                (),
                (identity.relative,),
                (capability_model.AuditResultFinding(
                    identity.relative, index + 1, f"expr-{index}", "reason"
                ),),
            )
            ownership = budget.reserve(
                capability_model.compact_result_retained_bytes(result)
            ).commit()
            aggregator.accept_validated_result(
                configuration, result, ownership
            )
        summary = aggregator.finish()
        self.assertEqual(len(summary.configurations), 251)
        self.assertEqual(len(summary.findings), 251)
        builder = capability_runner.BoundedCanonicalAuditContentSummaryBuilder(
            AuditLimits(), budget
        )
        sources = {identity.relative: "int maximal;\n"}
        growth = builder.premeasure_policy_merge_working_state(
            sources, summary, time.monotonic() + 10.0
        )
        budget.reserve_aggregate_growth(summary.budget_ownership, growth)
        budget.commit_aggregate_growth(summary.budget_ownership)
        builder.merge_findings_into_reserved_backing_state(
            sources,
            summary,
            {identity.relative: identity},
            frozenset((identity.relative,)),
            summary.budget_ownership,
            time.monotonic() + 10.0,
        )
        canonical = builder.build_bounded_digest_and_count_summary(summary)
        self.assertEqual(canonical.finding_count, 251)
        builder.release_backing_state()
        summary.release()
        self.assertEqual(budget.live_bytes, 0)

    def test_task8_source_only_scratch_preflight_precedes_raw_lane(self):
        main_path = self.write_source("playback/main.cpp", "int main_value;\n")
        inactive_path = self.write_source("playback/inactive.cpp", "")
        main = self.identity(main_path, "playback/main.cpp")
        inactive = self.identity(inactive_path, "playback/inactive.cpp")
        configuration = self.configuration(main, "a" * 64)
        result = capability_model.ConfigurationAuditResult(
            configuration.digest, "e" * 64, (), (main.relative,), ()
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        aggregator.accept_validated_result(configuration, result, ownership)
        summary = aggregator.finish()
        builder = capability_runner.BoundedCanonicalAuditContentSummaryBuilder(
            AuditLimits(), budget
        )
        sources = {
            main.relative: "int main_value;\n",
            inactive.relative: "x" * (8 << 20),
        }
        with mock.patch.object(
            capability_audit,
            "audit_raw_sources",
            wraps=capability_audit.audit_raw_sources,
        ) as raw, self.assertRaisesRegex(
            AuditInfrastructureError, "policy premeasure"
        ):
            builder.premeasure_policy_merge_working_state(
                sources, summary, time.monotonic() + 10.0
            )
        raw.assert_not_called()
        builder.release_backing_state()
        summary.release()

    def test_task8_directive_fanout_premeasure_precedes_raw_lane(self):
        main_path = self.write_source("playback/main-directive.cpp", "int main_value;\n")
        inactive_path = self.write_source("playback/inactive-directive.cpp", "")
        main = self.identity(main_path, "playback/main-directive.cpp")
        inactive = self.identity(inactive_path, "playback/inactive-directive.cpp")
        configuration = self.configuration(main, "b" * 64)
        result = capability_model.ConfigurationAuditResult(
            configuration.digest, "e" * 64, (), (main.relative,), ()
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        aggregator.accept_validated_result(configuration, result, ownership)
        summary = aggregator.finish()
        cases = {
            inactive.relative: "#else\n" * 30000,
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
                builder = (
                    capability_runner.BoundedCanonicalAuditContentSummaryBuilder(
                        AuditLimits(), budget
                    )
                )
                sources = {main.relative: "int main_value;\n", path: source}
                with mock.patch.object(
                    capability_audit,
                    "audit_raw_sources",
                    wraps=capability_audit.audit_raw_sources,
                ) as raw, self.assertRaisesRegex(
                    AuditInfrastructureError, "policy premeasure"
                ):
                    builder.premeasure_policy_merge_working_state(
                        sources, summary, time.monotonic() + 10.0
                    )
                raw.assert_not_called()
                builder.release_backing_state()
        summary.release()

    def test_task8_structural_stale_decision_launches_nothing(self):
        deadline = time.monotonic() + 10.0
        with mock.patch(
            "gpu_capability_source_audit._attest_loaded_audit_engine",
            return_value="e" * 64,
        ), mock.patch(
            "gpu_capability_calibration.canonical_platform_kind",
            return_value="windows",
        ), mock.patch.object(
            capability_runner,
            "bounded_uninspected_configuration_digest",
            return_value="u" * 64,
        ), mock.patch(
            "gpu_capability_calibration.prevalidate_platform_worker_decision",
            side_effect=AuditInfrastructureError("structural stale"),
        ), mock.patch.object(
            capability_runner, "collect_configurations_with_decision_records"
        ) as collect, mock.patch.object(
            capability_runner, "schedule_configuration_audits"
        ) as schedule, self.assertRaisesRegex(
            AuditInfrastructureError, "structural stale"
        ):
            capability_runner.run_compiler_audit_pipeline(
                self.root,
                (self.root / "compile_commands.json",),
                {},
                self.dependency_roots,
                object(),
                self.result_cache,
                self.inspection_cache,
                AuditLimits(),
                "audit",
                self.root / "decision.json",
                deadline,
                self.phase_accountant(),
                object(),
            )
        collect.assert_not_called()
        schedule.assert_not_called()

    def test_task8_outer_rejects_unproved_accountant_before_enumeration(self):
        deadline = time.monotonic() + 10.0
        invalid_accountants = (
            object(),
            SimpleNamespace(begin_phase=lambda _phase, _deadline: None),
        )
        for accountant in invalid_accountants:
            with self.subTest(accountant=type(accountant).__name__), mock.patch(
                "gpu_capability_source_audit._attest_loaded_audit_engine",
                return_value="e" * 64,
            ), mock.patch(
                "gpu_capability_calibration.canonical_platform_kind",
                return_value="windows",
            ), mock.patch.object(
                capability_runner,
                "enumerate_production_identities",
                side_effect=AssertionError("enumeration ran"),
            ) as enumerate_sources, self.assertRaisesRegex(
                AuditInfrastructureError, "run accountant"
            ):
                capability_runner.run_compiler_audit_pipeline(
                    self.root,
                    (self.root / "compile_commands.json",),
                    {},
                    self.dependency_roots,
                    object(),
                    self.result_cache,
                    self.inspection_cache,
                    AuditLimits(),
                    "audit",
                    self.root / "decision.json",
                    deadline,
                    accountant,
                    object(),
                )
            enumerate_sources.assert_not_called()

    def test_task8_outer_rejects_total_seconds_above_hard_ceiling(self):
        deadline = time.monotonic() + 10.0
        for seconds in (180.000001, 181.0, 600.0):
            with self.subTest(seconds=seconds), mock.patch(
                "gpu_capability_source_audit._attest_loaded_audit_engine",
                return_value="e" * 64,
            ), mock.patch.object(
                capability_runner,
                "enumerate_production_identities",
                side_effect=AssertionError("enumeration ran"),
            ) as enumerate_sources, self.assertRaisesRegex(
                AuditInfrastructureError, "pipeline inputs"
            ):
                capability_runner.run_compiler_audit_pipeline(
                    self.root,
                    (self.root / "compile_commands.json",),
                    {},
                    self.dependency_roots,
                    object(),
                    self.result_cache,
                    self.inspection_cache,
                    dataclasses.replace(AuditLimits(), total_seconds=seconds),
                    "audit",
                    self.root / "decision.json",
                    deadline,
                    self.phase_accountant(),
                    object(),
                )
            enumerate_sources.assert_not_called()

    def test_task8_public_outer_rejects_unprepared_result_cache_before_collection(self):
        deadline = time.monotonic() + 10.0
        cache_root = (self.root / "prepared-authority").resolve()
        result_cache = ConfigurationAuditCache(cache_root)
        inspection_cache = CompilerInspectionCache(cache_root)
        with mock.patch(
            "gpu_capability_source_audit._attest_loaded_audit_engine",
            return_value="e" * 64,
        ), mock.patch(
            "gpu_capability_calibration.canonical_platform_kind",
            return_value="windows",
        ), mock.patch.object(
            capability_runner,
            "bounded_uninspected_configuration_digest",
            return_value="u" * 64,
        ), mock.patch(
            "gpu_capability_calibration.prevalidate_platform_worker_decision",
            return_value=object(),
        ), mock.patch.object(
            capability_runner, "collect_configurations_with_decision_records"
        ) as collect, self.assertRaisesRegex(
            AuditInfrastructureError, "result cache is not prepared"
        ):
            capability_runner.run_compiler_audit_pipeline(
                self.root,
                (self.root / "compile_commands.json",),
                {},
                self.dependency_roots,
                object(),
                result_cache,
                inspection_cache,
                AuditLimits(),
                "audit",
                self.root / "decision.json",
                deadline,
                self.phase_accountant(),
                object(),
            )
        collect.assert_not_called()

    def test_task8_final_key_stale_allows_inspection_but_no_audit_launch(self):
        path = self.write_source("playback/final-stale.cpp")
        identity = self.identity(path, "playback/final-stale.cpp")
        configuration = self.configuration(identity, "a" * 64)
        deadline = time.monotonic() + 10.0
        collection = SimpleNamespace(
            configurations=(configuration,),
            decision_records={configuration.digest: SimpleNamespace(
                compiler_digest="c" * 64
            )},
            inspection_probe_invocations=2,
            dependency_root_authority=self.dependency_roots,
            capability_registry=None,
        )
        registry = object()
        collection.capability_registry = registry
        with mock.patch(
            "gpu_capability_source_audit._attest_loaded_audit_engine",
            return_value="e" * 64,
        ), mock.patch(
            "gpu_capability_calibration.canonical_platform_kind",
            return_value="windows",
        ), mock.patch.object(
            capability_runner,
            "bounded_uninspected_configuration_digest",
            return_value="u" * 64,
        ), mock.patch(
            "gpu_capability_calibration.prevalidate_platform_worker_decision",
            return_value=object(),
        ), mock.patch.object(
            capability_runner,
            "collect_configurations_with_decision_records",
            return_value=collection,
        ) as collect, mock.patch.object(
            capability_runner, "_seal_accountant_phase", return_value=object()
        ), mock.patch.object(
            capability_runner,
            "_validated_orchestration_inputs",
            return_value=(configuration,),
        ), mock.patch.object(
            capability_runner,
            "compute_active_sources",
            return_value=(frozenset((identity.relative,)),
                          frozenset((identity.relative,))),
        ), mock.patch(
            "gpu_capability_calibration.configuration_set_digest",
            return_value="s" * 64,
        ), mock.patch(
            "gpu_capability_calibration.windows_calibration_envelope",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.effective_worker_capacity",
            return_value=1,
        ), mock.patch(
            "gpu_capability_calibration.platform_worker_decision_key",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.finalize_platform_worker_decision",
            side_effect=AuditInfrastructureError("final key stale"),
        ), mock.patch.object(
            capability_runner, "schedule_configuration_audits"
        ) as schedule, self.assertRaisesRegex(
            AuditInfrastructureError, "final key stale"
        ):
            capability_runner.run_compiler_audit_pipeline(
                self.root,
                (self.root / "compile_commands.json",),
                {},
                self.dependency_roots,
                registry,
                self.result_cache,
                self.inspection_cache,
                AuditLimits(),
                "audit",
                self.root / "decision.json",
                deadline,
                self.phase_accountant(),
                object(),
            )
        self.assertEqual(collect.call_count, 1)
        schedule.assert_not_called()

    def test_task8_outer_final_boundary_preserves_primary_cleanup(self):
        path = self.write_source("playback/mutate-final.cpp", "A")
        identity = self.identity(path, "playback/mutate-final.cpp")
        configuration = self.configuration(identity, "a" * 64)
        deadline = time.monotonic() + 10.0
        registry = object()
        collection = SimpleNamespace(
            configurations=(configuration,),
            decision_records={configuration.digest: SimpleNamespace(
                compiler_digest="c" * 64
            )},
            inspection_probe_invocations=2,
            dependency_root_authority=self.dependency_roots,
            capability_registry=registry,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        result = capability_model.ConfigurationAuditResult(
            configuration.digest, "e" * 64, (), (identity.relative,), ()
        )
        ownership = budget.reserve(
            capability_model.compact_result_retained_bytes(result)
        ).commit()
        aggregator.accept_validated_result(configuration, result, ownership)
        aggregate = aggregator.finish()
        compiler_bytes = []

        class Session:
            compact_result_budget = budget
            pending_result_reference_count = 0
            aborted = []

            def consume_aggregate(self):
                return aggregate

            def abort_and_reap(self, cleanup_deadline):
                self.aborted.append(cleanup_deadline)
                raise RuntimeError("cleanup failure")

        session = Session()

        def schedule(*_args, **_kwargs):
            path.write_bytes(b"B")
            compiler_bytes.extend((path.read_bytes(), path.read_bytes()))
            path.write_bytes(b"A")
            return session

        with mock.patch.object(
            capability_runner.time,
            "monotonic",
            return_value=deadline - 0.5,
        ), mock.patch(
            "gpu_capability_source_audit._attest_loaded_audit_engine",
            return_value="e" * 64,
        ), mock.patch(
            "gpu_capability_calibration.canonical_platform_kind",
            return_value="windows",
        ), mock.patch.object(
            capability_runner,
            "bounded_uninspected_configuration_digest",
            return_value="u" * 64,
        ), mock.patch(
            "gpu_capability_calibration.prevalidate_platform_worker_decision",
            return_value=object(),
        ), mock.patch.object(
            capability_runner,
            "collect_configurations_with_decision_records",
            return_value=collection,
        ), mock.patch.object(
            capability_runner, "_seal_accountant_phase", return_value=object()
        ), mock.patch.object(
            capability_runner,
            "_validated_orchestration_inputs",
            return_value=(configuration,),
        ), mock.patch.object(
            capability_runner,
            "compute_active_sources",
            return_value=(frozenset((identity.relative,)),
                          frozenset((identity.relative,))),
        ), mock.patch(
            "gpu_capability_calibration.configuration_set_digest",
            return_value="s" * 64,
        ), mock.patch(
            "gpu_capability_calibration.windows_calibration_envelope",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.effective_worker_capacity",
            return_value=1,
        ), mock.patch(
            "gpu_capability_calibration.platform_worker_decision_key",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.finalize_platform_worker_decision",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.runtime_contract_from_platform_decision",
            return_value=capability_model.WorkerRuntimeContract(
                1, 1, 0, deadline
            ),
        ), mock.patch.object(
            capability_runner,
            "schedule_configuration_audits",
            side_effect=schedule,
        ), mock.patch.object(
            capability_runner,
            "_open_production_descriptor",
            side_effect=lambda selected: selected.open("rb"),
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "production snapshot generation"
        ) as raised:
            capability_runner.run_compiler_audit_pipeline(
                self.root,
                (self.root / "compile_commands.json",),
                {},
                self.dependency_roots,
                registry,
                self.result_cache,
                self.inspection_cache,
                AuditLimits(),
                "audit",
                self.root / "decision.json",
                deadline,
                self.phase_accountant(),
                object(),
            )
        self.assertEqual(compiler_bytes, [b"B", b"B"])
        self.assertEqual(path.read_bytes(), b"A")
        self.assertEqual(len(session.aborted), 1)
        self.assertLessEqual(session.aborted[0], deadline)
        self.assertEqual(budget.live_bytes, 0)
        self.assertIn(
            "emergency abort/reap",
            " ".join(getattr(raised.exception, "__notes__", ())),
        )

    def test_task8_outer_pipeline_is_bounded_order_independent_and_one_deadline(self):
        identities = []
        configurations = []
        for index, relative in enumerate(
            ("playback/outer-a.cpp", "playback/outer-b.cpp")
        ):
            path = self.write_source(relative, f"int value_{index};\n")
            identity = self.identity(path, relative)
            identities.append(identity)
            configurations.append(
                self.configuration(identity, f"{index + 1:064x}")
            )
        configurations = tuple(configurations)
        production = {identity.relative: identity for identity in identities}
        registry = object()
        collection_tables = []
        snapshot_tables = []
        collection = SimpleNamespace(
            configurations=configurations,
            decision_records={
                configuration.digest: SimpleNamespace(compiler_digest="c" * 64)
                for configuration in configurations
            },
            inspection_probe_invocations=3,
            dependency_root_authority=self.dependency_roots,
            capability_registry=registry,
        )
        memory = capability_model.WindowsRunMemoryMeasurements(
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, True
        )
        summary_events = []
        allocation_events = []
        cancelled_summary_reservations = []
        decoded_source_mappings = []
        accounting_deadlines = []
        enumeration_deadlines = []

        class Accountant:
            def begin_phase(self, phase, observed_deadline):
                self_case.assertIn(phase, {"inspection", "tasks"})
                if phase == "inspection":
                    accounting_deadlines.append(observed_deadline)

            def memory_measurements(self):
                return memory

        self_case = self

        class Evidence:
            maximum_bytes = 524288

            def __init__(self):
                self.summaries = []

            def reserve_summary(self, count, deadline):
                summary_events.append("reserve-summary")
                self.reserved = (count, deadline)
                return object()

            def commit_summary(self, _reservation, summary):
                summary_events.append("commit-summary")
                self.summaries.append(summary)

            def cancel_summary(self, reservation):
                cancelled_summary_reservations.append(reservation)

            def release_summary(self, summary):
                self.summaries.remove(summary)

        def session_for(order, deadline):
            budget = CompactResultMemoryBudget()
            aggregator = StreamingResultAggregator(
                configurations, budget, AuditLimits()
            )
            for index in order:
                configuration = configurations[index]
                result = capability_model.ConfigurationAuditResult(
                    configuration.digest,
                    "e" * 64,
                    (),
                    (configuration.source.relative,),
                    (),
                )
                ownership = budget.reserve(
                    capability_model.compact_result_retained_bytes(result)
                ).commit()
                aggregator.accept_validated_result(
                    configuration, result, ownership
                )
            aggregate = aggregator.finish()

            class Session:
                compact_result_budget = budget
                result_budget = budget
                pending_result_reference_count = 0
                cache_hits = 2
                cache_misses = 0
                inspection_probe_invocations = 3
                audit_compiler_invocations = 0
                stdout_bytes = 0
                maximum_encoded_result_bytes = 4096
                maximum_conservative_decoded_bytes = 16384
                maximum_conservative_retained_bytes = 2048
                cache_root = self.root / "outer-cache"
                reactor = None
                worker_counts_started = ()

                def consume_aggregate(self):
                    return aggregate

                def shutdown_reap(self, observed_deadline):
                    self.shutdown_deadline = observed_deadline

                def abort_and_reap(self, _cleanup_deadline):
                    raise AssertionError("normal run aborted")

            Session.cache_root.mkdir(exist_ok=True)
            return Session()

        original_snapshot = capability_runner.snapshot_production_sources
        original_summary = capability_runner._canonical_streaming_summary
        original_decode = capability_runner.decode_validated_production_sources
        original_enumerate = capability_runner.enumerate_production_identities

        def enumerate_sources(root, limits, observed_deadline):
            if len(enumeration_deadlines) < 4:
                self.assertEqual(
                    len(accounting_deadlines), len(enumeration_deadlines) + 1
                )
            enumeration_deadlines.append(observed_deadline)
            return original_enumerate(root, limits, observed_deadline)

        def snapshot(table, limits, deadline, **kwargs):
            snapshot_tables.append(table)
            return original_snapshot(table, limits, deadline, **kwargs)

        def collect(*args):
            collection_tables.append(args[3])
            return collection

        def build_summary(*args):
            summary_events.append("build-summary")
            return original_summary(*args)

        def decode_sources(*args):
            decoded = original_decode(*args)
            decoded_source_mappings.append(decoded)
            return decoded

        deadline = time.monotonic() + 20.0
        sessions = (
            session_for((1, 0), deadline),
            session_for((0, 1), deadline),
            session_for((0, 1), deadline),
            session_for((0, 1), deadline),
            session_for((0, 1), deadline),
        )
        with mock.patch(
            "gpu_capability_source_audit._attest_loaded_audit_engine",
            return_value="e" * 64,
        ), mock.patch(
            "gpu_capability_calibration.canonical_platform_kind",
            return_value="windows",
        ), mock.patch.object(
            capability_runner,
            "enumerate_production_identities",
            side_effect=enumerate_sources,
        ), mock.patch.object(
            capability_runner,
            "bounded_uninspected_configuration_digest",
            return_value="u" * 64,
        ), mock.patch(
            "gpu_capability_calibration.prevalidate_platform_worker_decision",
            return_value=object(),
        ) as prevalidate, mock.patch.object(
            capability_runner,
            "collect_configurations_with_decision_records",
            side_effect=collect,
        ) as collect_mock, mock.patch.object(
            capability_runner,
            "compute_active_sources",
            return_value=(frozenset(production), frozenset(production)),
        ), mock.patch(
            "gpu_capability_calibration.configuration_set_digest",
            return_value="s" * 64,
        ), mock.patch(
            "gpu_capability_calibration.windows_calibration_envelope",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.effective_worker_capacity",
            return_value=1,
        ), mock.patch(
            "gpu_capability_calibration.platform_worker_decision_key",
            return_value=object(),
        ), mock.patch(
            "gpu_capability_calibration.finalize_platform_worker_decision",
            return_value=object(),
        ) as finalize, mock.patch(
            "gpu_capability_calibration.runtime_contract_from_platform_decision",
            side_effect=lambda _decision, observed: (
                capability_model.WorkerRuntimeContract(1, 1, 0, observed)
            ),
        ), mock.patch.object(
            capability_runner,
            "schedule_configuration_audits",
            side_effect=sessions,
        ) as schedule, mock.patch.object(
            capability_runner,
            "snapshot_production_sources",
            side_effect=snapshot,
        ), mock.patch.object(
            capability_runner,
            "_canonical_streaming_summary",
            side_effect=build_summary,
        ), mock.patch.object(
            capability_runner,
            "decode_validated_production_sources",
            side_effect=decode_sources,
        ), mock.patch.object(
            capability_runner,
            "_production_allocation_event",
            side_effect=allocation_events.append,
        ):
            runs = tuple(
                capability_runner.run_compiler_audit_pipeline(
                    self.root,
                    (self.root / "compile_commands.json",),
                    {},
                    self.dependency_roots,
                    registry,
                    self.result_cache,
                    self.inspection_cache,
                    AuditLimits(),
                    "audit",
                    self.root / "decision.json",
                    deadline,
                    Accountant(),
                    Evidence(),
                )
                for _ in range(2)
            )
            with mock.patch.object(
                capability_runner.BoundedCanonicalAuditContentSummaryBuilder,
                "premeasure_policy_merge_working_state",
                side_effect=AuditInfrastructureError("post-decode failure"),
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "post-decode failure"
            ):
                capability_runner.run_compiler_audit_pipeline(
                    self.root,
                    (self.root / "compile_commands.json",),
                    {},
                    self.dependency_roots,
                    registry,
                    self.result_cache,
                    self.inspection_cache,
                    AuditLimits(),
                    "audit",
                    self.root / "decision.json",
                    deadline,
                    Accountant(),
                    Evidence(),
                )
            failed_evidence = Evidence()
            with mock.patch.object(
                capability_runner,
                "_canonical_streaming_summary",
                side_effect=AuditInfrastructureError("summary build failure"),
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "summary build failure"
            ):
                capability_runner.run_compiler_audit_pipeline(
                    self.root,
                    (self.root / "compile_commands.json",),
                    {},
                    self.dependency_roots,
                    registry,
                    self.result_cache,
                    self.inspection_cache,
                    AuditLimits(),
                    "audit",
                    self.root / "decision.json",
                    deadline,
                    Accountant(),
                    failed_evidence,
                )
            if os.name == "nt":
                from gpu_capability_process_tree import (
                    WindowsNativeRunAccountant,
                )

                windows_accountant = WindowsNativeRunAccountant(os.getpid())
                try:
                    windows_run = capability_runner.run_compiler_audit_pipeline(
                        self.root,
                        (self.root / "compile_commands.json",),
                        {},
                        self.dependency_roots,
                        registry,
                        self.result_cache,
                        self.inspection_cache,
                        AuditLimits(),
                        "audit",
                        self.root / "decision.json",
                        deadline,
                        windows_accountant,
                        Evidence(),
                    )
                finally:
                    windows_accountant.close()
                self.assertIsInstance(
                    windows_run.measurements.inspection_phase_snapshot,
                    capability_model.WindowsPhaseSnapshot,
                )
                self.assertIsInstance(
                    windows_run.measurements.task_phase_snapshot,
                    capability_model.WindowsPhaseSnapshot,
                )
        self.assertEqual(runs[0].summary, runs[1].summary)
        self.assertTrue(
            all(run.measurements.worker_counts_started == () for run in runs)
        )
        self.assertEqual(
            tuple(field.name for field in dataclasses.fields(runs[0])),
            ("summary", "measurements"),
        )
        self.assertTrue(all(isinstance(table, MappingProxyType)
                            for table in collection_tables))
        self.assertEqual(
            [id(table) for table in collection_tables],
            [id(table) for table in snapshot_tables],
        )
        for start in (
            index for index, event in enumerate(allocation_events)
            if event == "reserve-snapshot-structure"
        ):
            segment = allocation_events[start:]
            self.assertLess(
                segment.index("reserve-snapshot-structure"),
                segment.index("construct-frozen-table"),
            )
            self.assertLess(segment.index("construct-frozen-table"), segment.index("collect"))
            self.assertLess(segment.index("collect"), segment.index("snapshot"))
            self.assertLess(segment.index("snapshot"), segment.index("open"))
        self.assertTrue(all(call.args[-1] == deadline
                            for call in prevalidate.call_args_list))
        expected_run_count = 5 if os.name == "nt" else 4
        self.assertEqual(enumeration_deadlines, [deadline] * expected_run_count)
        self.assertEqual(accounting_deadlines, [deadline] * 4)
        self.assertTrue(all(call.args[-1] == deadline
                            for call in finalize.call_args_list))
        self.assertTrue(all(call.args[-2] == deadline
                            for call in collect_mock.call_args_list))
        self.assertTrue(all(call.args[8].pipeline_deadline == deadline
                            for call in schedule.call_args_list))
        self.assertEqual(
            summary_events,
            [
                "reserve-summary", "build-summary", "commit-summary",
                "reserve-summary", "build-summary", "commit-summary",
                "reserve-summary",
            ] + (
                ["reserve-summary", "build-summary", "commit-summary"]
                if os.name == "nt" else []
            ),
        )
        self.assertEqual(
            [mapping.allocation_budget.current_bytes
             for mapping in decoded_source_mappings],
            [0] * expected_run_count,
        )
        self.assertEqual(len(cancelled_summary_reservations), 1)
        self.assertEqual(failed_evidence.summaries, [])

    def test_compact_result_batch_contract_keeps_cold_slot_inside_128_mib(self):
        budget = CompactResultMemoryBudget()
        slot = CompactResultColdSlot(AuditLimits().compact_result_bytes)
        configuration = PreprocessConfiguration(
            entry_id="compact-result-contract",
            family=CompilerFamily.GCC,
            compiler=self.compiler_capability.executable_identity.canonical,
            working_directory=self.root,
            source=FileIdentity(
                self.root / "playback" / "contract.cpp",
                PurePosixPath("playback/contract.cpp"),
                1,
                2,
                1,
                True,
            ),
            arguments=("contract.cpp",),
            environment_digest="contract",
            digest="1" * 64,
            dependency_root_authority_digest=(
                self.dependency_roots.portable_authority_digest
            ),
            compiler_capability_digest=self.compiler_capability.capability_digest,
            compiler_capability=self.compiler_capability,
        )
        aggregator = StreamingResultAggregator(
            (configuration,), budget, AuditLimits()
        )
        self.assertEqual(aggregator.reserve_cold_slot(slot), 16 << 20)
        batch = ConfigurationAuditLoadBatch(0, (configuration,), 16 << 20)
        self.assertEqual(batch.cold_slot_reserved_bytes, 16 << 20)
        self.assertLessEqual(budget.peak_live_bytes, 128 << 20)
        aggregator.finish().release()

    def test_dependency_guard_prevents_or_observes_write_then_restore(self):
        path = self.write_source("playback/guarded.h", "original\n")
        identity = self.identity(path, "playback/guarded.h")
        dependency = DependencyDigest(
            self.dependency_roots.source_root.stable_role,
            PurePosixPath("playback/guarded.h"),
            identity,
            hashlib.sha256(path.read_bytes()).hexdigest(),
        )
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            (dependency,), (identity,),
        )
        preflight = capability_runner.preflight_dependency_generation_guards(
            discovery, self.dependency_roots, AuditLimits()
        )
        guards = capability_runner.arm_dependency_generation_guards(
            preflight, self.dependency_roots, time.monotonic() + 10.0
        )
        owner = guards[0].platform_watch
        try:
            if os.name == "nt":
                with self.assertRaises(OSError):
                    path.write_text("changed\n", encoding="utf-8")
                self.assertEqual(
                    capability_runner.validate_and_hash_guarded_dependencies(
                        guards, time.monotonic() + 10.0
                    ),
                    (dependency,),
                )
            else:
                path.write_text("changed\n", encoding="utf-8")
                path.write_text("original\n", encoding="utf-8")
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "generation change"
                ):
                    capability_runner.validate_and_hash_guarded_dependencies(
                        guards, time.monotonic() + 10.0
                    )
        finally:
            owner.close()

    def test_generation_guard_preflight_charges_one_shared_directory_table(self):
        dependencies = []
        for index in range(2):
            relative = f"playback/shared-{index}/guarded.h"
            path = self.write_source(relative, f"guarded {index}\n")
            identity = self.identity(path, relative)
            dependencies.append(DependencyDigest(
                self.dependency_roots.source_root.stable_role,
                PurePosixPath(relative),
                identity,
                hashlib.sha256(path.read_bytes()).hexdigest(),
            ))
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            tuple(dependencies),
            tuple(item.identity for item in dependencies),
        )
        preflight = capability_runner.preflight_dependency_generation_guards(
            discovery, self.dependency_roots, AuditLimits()
        )
        self.assertIsInstance(preflight.dependencies_by_path, MappingProxyType)
        guards = capability_runner.arm_dependency_generation_guards(
            preflight, self.dependency_roots, time.monotonic() + 10.0
        )
        owner = guards[0].platform_watch
        try:
            self.assertIs(
                guards[0].directory_chain_owners,
                preflight.directory_paths,
            )
            self.assertIs(
                guards[0].directory_chain_owners,
                guards[1].directory_chain_owners,
            )
            self.assertIs(
                guards[0].directory_chain_identities,
                guards[1].directory_chain_identities,
            )
        finally:
            owner.close()

    def test_generation_guard_preflight_accepts_exact_32768_before_any_open(self):
        role = self.dependency_roots.source_root.stable_role
        dependencies = tuple(
            DependencyDigest(
                role,
                PurePosixPath(f"playback/ceiling-{index}.h"),
                FileIdentity(
                    self.root / "playback" / f"ceiling-{index}.h",
                    PurePosixPath(f"playback/ceiling-{index}.h"),
                    1,
                    index + 1,
                    0,
                    True,
                ),
                "a" * 64,
            )
            for index in range(32_768)
        )
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            dependencies,
            tuple(item.identity for item in dependencies),
        )
        with mock.patch.object(
            Path, "open", side_effect=AssertionError("file owner opened")
        ), mock.patch(
            "gpu_capability_runner._FilesystemGenerationObserver",
            side_effect=AssertionError("watch armed"),
        ):
            preflight = capability_runner.preflight_dependency_generation_guards(
                discovery, self.dependency_roots, AuditLimits()
            )
        self.assertEqual(len(preflight.dependencies), 32_768)
        self.assertEqual(len(preflight.dependencies_by_path), 32_768)

    def test_guarded_dependency_lookup_is_linear_and_rejects_duplicate_paths(self):
        dependencies = []
        for index in range(64):
            relative = f"playback/lookup-{index}.h"
            path = self.write_source(relative, f"lookup {index}\n")
            identity = self.identity(path, relative)
            dependencies.append(DependencyDigest(
                self.dependency_roots.source_root.stable_role,
                PurePosixPath(relative),
                identity,
                hashlib.sha256(path.read_bytes()).hexdigest(),
            ))
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            tuple(dependencies),
            tuple(item.identity for item in dependencies),
        )
        preflight = capability_runner.preflight_dependency_generation_guards(
            discovery, self.dependency_roots, AuditLimits()
        )
        guards = capability_runner.arm_dependency_generation_guards(
            preflight, self.dependency_roots, time.monotonic() + 10.0
        )
        owner = guards[0].platform_watch

        class CountingDependencies(tuple):
            visits = 0

            def __iter__(self):
                for item in super().__iter__():
                    self.visits += 1
                    yield item

        counted = CountingDependencies(owner._discovery_dependencies)
        owner._discovery_dependencies = counted
        self.assertEqual(
            capability_runner.validate_and_hash_guarded_dependencies(
                guards, time.monotonic() + 10.0
            ),
            tuple(dependencies),
        )
        self.assertLessEqual(counted.visits, len(dependencies))

        duplicate_discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            (dependencies[0], dependencies[0]),
            (dependencies[0].identity, dependencies[0].identity),
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "duplicate|ambiguous"
        ):
            capability_runner.preflight_dependency_generation_guards(
                duplicate_discovery, self.dependency_roots, AuditLimits()
            )

    def test_dependency_guard_cleanup_failure_is_fatal(self):
        path = self.write_source("playback/cleanup.h", "original\n")
        identity = self.identity(path, "playback/cleanup.h")
        dependency = DependencyDigest(
            self.dependency_roots.source_root.stable_role,
            PurePosixPath("playback/cleanup.h"),
            identity,
            hashlib.sha256(path.read_bytes()).hexdigest(),
        )
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            (dependency,), (identity,),
        )
        preflight = capability_runner.preflight_dependency_generation_guards(
            discovery, self.dependency_roots, AuditLimits()
        )
        guards = capability_runner.arm_dependency_generation_guards(
            preflight, self.dependency_roots, time.monotonic() + 10.0
        )
        owner = guards[0].platform_watch
        observer = owner._observer
        with mock.patch.object(
            observer, "close", side_effect=AuditInfrastructureError("cleanup")
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "guard cleanup failed"
        ):
            owner.close()
        observer._close_no_raise()

    def test_dependency_guard_rejects_unsupported_platform_backend(self):
        path = self.write_source("playback/unsupported.h", "original\n")
        with (
            mock.patch.object(capability_model.os, "name", "posix"),
            mock.patch.object(capability_model.sys, "platform", "aix"),
            self.assertRaisesRegex(
                AuditInfrastructureError, "observation is unsupported"
            ),
        ):
            _FilesystemGenerationObserver(((path, False),))

    def test_linux_generation_observer_fails_on_overflow_and_watch_loss(self):
        for mask, expected in ((0x00004000, "overflow"), (0x00008000, "watch was lost")):
            read_fd, write_fd = os.pipe()
            observer = object.__new__(_FilesystemGenerationObserver)
            observer._backend = "linux"
            observer._owner = None
            observer._handles = [read_fd]
            observer._closed = False
            os.set_blocking(read_fd, False)
            os.write(write_fd, struct.pack("iIII", 1, mask, 0, 0))
            os.close(write_fd)
            try:
                with self.subTest(mask=mask), self.assertRaisesRegex(
                    AuditInfrastructureError, expected
                ):
                    observer.drain()
            finally:
                observer.close()

    def test_local_authority_mismatch_rejects_before_preprocess_cache_or_compiler(self):
        source = self.write_source("playback/authority.cpp")
        identity = self.identity(source, "playback/authority.cpp")
        configuration = self.configuration(identity, "authority-mismatch")
        other = tempfile.TemporaryDirectory()
        self.addCleanup(other.cleanup)
        other_root = Path(other.name).resolve()
        other_source = other_root / "source"
        other_toolchain = other_root / "toolchain"
        other_source.mkdir()
        other_toolchain.mkdir()
        authority = build_dependency_root_authority(
            other_source, {"toolchain": other_toolchain}
        )
        self.assertEqual(
            authority.portable_authority_digest,
            self.dependency_roots.portable_authority_digest,
        )
        with (
            mock.patch.object(
                self.cache, "load", side_effect=AssertionError("cache reached")
            ),
            mock.patch(
                "gpu_capability_runner.stabilize_and_parse_configuration",
                side_effect=AssertionError("compiler reached"),
            ),
            self.assertRaisesRegex(
                AuditInfrastructureError, "local dependency authority"
            ),
        ):
            load_or_preprocess(
                configuration, authority, {identity.relative: identity}, self.cache,
                AuditLimits(rss_bytes=2**63 - 1), time.monotonic() + 10.0,
            )

    def test_discovery_rejects_local_authority_mismatch_before_compiler_launch(self):
        source = self.write_source("playback/direct-discovery.cpp")
        identity = self.identity(source, "playback/direct-discovery.cpp")
        configuration = self.configuration(identity, "direct-discovery-authority")
        other = tempfile.TemporaryDirectory()
        self.addCleanup(other.cleanup)
        other_root = Path(other.name).resolve()
        other_source = other_root / "source"
        other_toolchain = other_root / "toolchain"
        other_source.mkdir()
        other_toolchain.mkdir()
        authority = build_dependency_root_authority(
            other_source, {"toolchain": other_toolchain}
        )
        self.assertEqual(
            authority.portable_authority_digest,
            self.dependency_roots.portable_authority_digest,
        )
        with mock.patch(
            "gpu_capability_runner.run_bounded_preprocessor",
            side_effect=AssertionError("compiler reached"),
        ), self.assertRaisesRegex(AuditInfrastructureError, "local dependency authority"):
            discover_configuration(
                configuration,
                authority,
                {identity.relative: identity},
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
            )

    def test_stabilization_rejects_local_authority_mismatch_before_discovery(self):
        source = self.write_source("playback/direct-stabilization.cpp")
        identity = self.identity(source, "playback/direct-stabilization.cpp")
        configuration = self.configuration(identity, "direct-stabilization-authority")
        other = tempfile.TemporaryDirectory()
        self.addCleanup(other.cleanup)
        other_root = Path(other.name).resolve()
        other_source = other_root / "source"
        other_toolchain = other_root / "toolchain"
        other_source.mkdir()
        other_toolchain.mkdir()
        authority = build_dependency_root_authority(
            other_source, {"toolchain": other_toolchain}
        )
        with mock.patch(
            "gpu_capability_runner.discover_configuration",
            side_effect=AssertionError("discovery reached"),
        ), self.assertRaisesRegex(AuditInfrastructureError, "local dependency authority"):
            stabilize_and_parse_configuration(
                configuration,
                authority,
                {identity.relative: identity},
                AuditLimits(rss_bytes=2**63 - 1),
                time.monotonic() + 10.0,
            )

    def test_dependency_aggregate_is_reserved_before_any_payload_open(self):
        identities = []
        for index in range(5):
            path = self.write_source(f"playback/large-{index}.h", "")
            with path.open("wb") as stream:
                stream.truncate(220 * 1024 * 1024)
            identities.append(self.identity(path, f"playback/large-{index}.h"))
        original_open = Path.open

        def guarded_open(path, *args, **kwargs):
            if path in {identity.canonical for identity in identities}:
                raise AssertionError("dependency payload opened before aggregate reservation")
            return original_open(path, *args, **kwargs)

        with mock.patch.object(
            Path, "open", guarded_open
        ), self.assertRaisesRegex(AuditInfrastructureError, "total byte ceiling"):
            capability_runner._dependency_digests(
                tuple(identities), self.dependency_roots,
                time.monotonic() + 10.0, None,
            )

    def test_dependency_hashing_honors_cancellation_and_deadline(self):
        source = self.write_source("playback/hash-budget.h")
        identity = self.identity(source, "playback/hash-budget.h")
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            capability_runner._dependency_digests(
                (identity,), self.dependency_roots,
                time.monotonic() + 10.0, cancelled,
            )
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            capability_runner._dependency_digests(
                (identity,), self.dependency_roots,
                time.monotonic() - 1.0, None,
            )

    def test_dependency_generation_guards_use_locked_three_phase_interface(self):
        for name in (
            "preflight_dependency_generation_guards",
            "arm_dependency_generation_guards",
            "validate_and_hash_guarded_dependencies",
        ):
            self.assertTrue(callable(getattr(capability_runner, name, None)), name)
        source = self.write_source("playback/three-phase.h")
        identity = self.identity(source, "playback/three-phase.h")
        dependencies = capability_runner._dependency_digests(
            (identity,), self.dependency_roots,
            time.monotonic() + 10.0, None,
        )
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            dependencies, (identity,),
        )
        preflight = capability_runner.preflight_dependency_generation_guards(
            discovery, self.dependency_roots, AuditLimits()
        )
        guards = capability_runner.arm_dependency_generation_guards(
            preflight, self.dependency_roots, time.monotonic() + 10.0
        )
        self.assertEqual(
            capability_runner.validate_and_hash_guarded_dependencies(
                guards, time.monotonic() + 10.0
            ),
            dependencies,
        )

    def test_final_guarded_hash_polls_cancellation_before_during_and_after(self):
        sequence = 0

        def arm(payload: str):
            nonlocal sequence
            sequence += 1
            relative = f"playback/cancel-guard-{sequence}.h"
            path = self.write_source(relative, payload)
            identity = self.identity(path, relative)
            dependencies = capability_runner._dependency_digests(
                (identity,), self.dependency_roots,
                time.monotonic() + 10.0, None,
            )
            discovery = capability_runner.PreprocessDiscovery(
                capability_runner.StreamDigest("a" * 64, 1),
                dependencies, (identity,),
            )
            preflight = capability_runner.preflight_dependency_generation_guards(
                discovery, self.dependency_roots, AuditLimits()
            )
            return capability_runner.arm_dependency_generation_guards(
                preflight, self.dependency_roots, time.monotonic() + 10.0
            )

        before = threading.Event()
        before.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            capability_runner.validate_and_hash_guarded_dependencies(
                arm("before\n"), time.monotonic() + 10.0, before
            )

        during = threading.Event()
        during_guards = arm("x" * (2 * 1024 * 1024 + 1))
        during_owner = during_guards[0].platform_watch
        wrapped = during_owner._streams[0]

        class CancellingStream:
            def fileno(self):
                return wrapped.fileno()

            def seek(self, *arguments):
                return wrapped.seek(*arguments)

            def read(self, *arguments):
                chunk = wrapped.read(*arguments)
                if chunk:
                    during.set()
                return chunk

            def close(self):
                wrapped.close()

        during_owner._streams[0] = CancellingStream()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            capability_runner.validate_and_hash_guarded_dependencies(
                during_guards, time.monotonic() + 10.0, during
            )

        after = threading.Event()
        after_guards = arm("after\n")
        after_owner = after_guards[0].platform_watch
        real_validate = after_owner.validate_and_hash

        def cancel_after(deadline, cancel_event):
            result = real_validate(deadline, cancel_event)
            after.set()
            return result

        with mock.patch.object(
            after_owner, "validate_and_hash", side_effect=cancel_after
        ), self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            capability_runner.validate_and_hash_guarded_dependencies(
                after_guards, time.monotonic() + 10.0, after
            )

    def test_guard_metadata_admission_fails_before_first_open_or_watch(self):
        fields = AuditLimits.__dataclass_fields__
        self.assertIn("unique_dependency_handles", fields)
        self.assertIn("unique_generation_guard_directories", fields)
        self.assertIn("generation_guard_metadata_bytes", fields)
        source = self.write_source("playback/metadata-admission.h")
        identity = self.identity(source, "playback/metadata-admission.h")
        dependencies = capability_runner._dependency_digests(
            (identity,), self.dependency_roots,
            time.monotonic() + 10.0, None,
        )
        discovery = capability_runner.PreprocessDiscovery(
            capability_runner.StreamDigest("a" * 64, 1),
            dependencies, (identity,),
        )
        limits = dataclasses.replace(
            AuditLimits(), generation_guard_metadata_bytes=1
        )
        with mock.patch.object(
            Path, "open", side_effect=AssertionError("file owner opened")
        ), mock.patch(
            "gpu_capability_runner._FilesystemGenerationObserver",
            side_effect=AssertionError("watch armed"),
        ), self.assertRaisesRegex(AuditInfrastructureError, "metadata"):
            capability_runner.preflight_dependency_generation_guards(
                discovery, self.dependency_roots, limits
            )

    @staticmethod
    def identity(path: Path, relative: str) -> FileIdentity:
        metadata = path.stat()
        content = path.read_text(encoding="utf-8")
        line_count = 0 if not content else content.count("\n") + (
            0 if content.endswith("\n") else 1
        )
        return FileIdentity(
            canonical=path.resolve(),
            relative=PurePosixPath(relative),
            device=int(metadata.st_dev),
            inode=int(metadata.st_ino) if int(metadata.st_ino) else None,
            line_count=line_count,
            production=True,
        )

    def entry(
        self,
        relative: str,
        *,
        defines: tuple[str, ...] = (),
        compiler: Path | None = None,
        directory: str = ".",
    ) -> dict[str, object]:
        source_from_build = Path("..") / Path(relative)
        return {
            "directory": directory,
            "file": str(source_from_build),
            "arguments": [
                str(compiler or self.compiler),
                "-c",
                *(f"-D{value}" for value in defines),
                str(source_from_build),
            ],
        }

    def database(self, name: str, entries: object) -> Path:
        path = self.build / name
        path.write_text(json.dumps(entries), encoding="utf-8")
        return path

    def collect(self, databases: tuple[Path, ...]):
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0\n",
        ):
            return collect_configurations(
                self.root, databases, self.environment, self.dependency_roots,
                time.monotonic() + 180.0,
            )

    def test_collection_threads_caller_deadline_into_every_configuration(self):
        self.write_source("playback/deadline.cpp")
        database = self.database(
            "deadline.json", (self.entry("playback/deadline.cpp"),)
        )
        deadline = time.monotonic() + 37.0
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            return_value=b"g++.exe (GCC) 13.1.0\n",
        ), mock.patch(
            "gpu_capability_runner.make_configuration",
            wraps=capability_command.make_configuration,
        ) as make:
            configurations = collect_configurations(
                self.root, (database,), self.environment,
                self.dependency_roots, deadline,
            )
        self.assertEqual(len(configurations), 1)
        self.assertEqual(make.call_args.args[-1], deadline)

    def configuration(
        self,
        identity: FileIdentity,
        digest: str,
        *,
        family: CompilerFamily = CompilerFamily.GCC,
        arguments: tuple[str, ...] = (),
    ) -> PreprocessConfiguration:
        return PreprocessConfiguration(
            entry_id=f"db:{digest}",
            family=family,
            compiler=self.compiler.resolve(),
            working_directory=self.build.resolve(),
            source=identity,
            arguments=(*arguments, str(identity.canonical)),
            environment_digest=_environment_digest(dict(os.environ)),
            digest=digest,
            dependency_root_authority_digest=(
                self.dependency_roots.portable_authority_digest
            ),
            compiler_capability_digest=self.compiler_capability.capability_digest,
            compiler_capability=self.compiler_capability,
        )

    @staticmethod
    def view(
        configuration: PreprocessConfiguration,
        dependencies: tuple[FileIdentity, ...],
        token_identities: tuple[FileIdentity, ...] | None = None,
    ) -> PreprocessedTranslationUnitView:
        if token_identities is None:
            token_identities = (configuration.source,)
        identities = tuple(dict.fromkeys(token_identities))
        identity_ids = {identity: index for index, identity in enumerate(identities)}
        tokens = CompactTokenSequence._from_token_fields(
            configuration,
            spellings=(b"token",),
            identities=identities,
            fields=(
                (0, identity_ids[identity], index + 1, 1)
                for index, identity in enumerate(token_identities)
            ),
        ) if token_identities else CompactTokenSequence.empty(configuration)
        return PreprocessedTranslationUnitView(
            configuration,
            tokens,
            dependencies,
        )

    def test_distinct_defines_are_both_audited_and_exact_duplicates_coalesce(self):
        self.write_source("playback/file.cpp")
        database = self.database(
            "compile_commands.json",
            (
                self.entry("playback/file.cpp", defines=("MODE=1",)),
                self.entry("playback/file.cpp", defines=("MODE=2",)),
                self.entry("playback/file.cpp", defines=("MODE=1",)),
            ),
        )

        configurations = self.collect((database,))

        self.assertEqual(len(configurations), 2)
        self.assertNotEqual(configurations[0].digest, configurations[1].digest)
        self.assertEqual(
            {
                argument
                for item in configurations
                for argument in item.arguments
                if argument.startswith("-D")
            },
            {"-DMODE=1", "-DMODE=2"},
        )

    def test_large_database_inspects_one_stable_compiler_once(self):
        entries = []
        for index in range(251):
            relative = f"playback/generated/source{index:03d}.cpp"
            self.write_source(relative)
            entries.append(self.entry(relative))
        database = self.database("compile_commands.json", entries)

        def version_probe(*_args, **_kwargs):
            time.sleep(0.01)
            return b"g++.exe (GCC) 13.1.0\n"

        started = time.monotonic()
        with mock.patch(
            "gpu_capability_command._probe_compiler_version",
            side_effect=version_probe,
        ) as probe:
            configurations = collect_configurations(
                self.root,
                (database,),
                self.environment,
                self.dependency_roots,
                time.monotonic() + 180.0,
            )
        elapsed = time.monotonic() - started

        self.assertEqual(len(configurations), 251)
        self.assertEqual(probe.call_count, 1)
        self.assertLess(elapsed, 2.0)

    def test_collection_sorts_databases_and_entries_deterministically(self):
        self.write_source("playback/a.cpp")
        self.write_source("playback/b.cpp")
        first = self.database("z.json", (self.entry("playback/b.cpp"),))
        second = self.database("a.json", (self.entry("playback/a.cpp"),))

        forward = self.collect((first, second))
        reverse = self.collect((second, first))

        self.assertEqual(forward, reverse)
        self.assertEqual(
            tuple(item.digest for item in forward),
            tuple(sorted(item.digest for item in forward)),
        )

    def test_collection_accepts_relative_database_directory_and_file(self):
        source = self.write_source("playback/file.cpp")
        self.database("compile_commands.json", (self.entry("playback/file.cpp"),))

        configurations = self.collect((Path("build/compile_commands.json"),))

        self.assertEqual(configurations[0].working_directory, self.build.resolve())
        self.assertEqual(configurations[0].source.canonical, source)

    def test_collection_ignores_repository_nonproduction_entries(self):
        self.write_source("playback/file.cpp")
        self.write_source("tests/helper.cpp")
        database = self.database(
            "compile_commands.json",
            (
                self.entry("tests/helper.cpp"),
                self.entry("playback/file.cpp"),
            ),
        )

        configurations = self.collect((database,))

        self.assertEqual(len(configurations), 1)
        self.assertEqual(
            configurations[0].source.relative,
            PurePosixPath("playback/file.cpp"),
        )

    def test_collection_skips_missing_in_root_test_autogen_entry_before_resolve(self):
        self.write_source("playback/file.cpp")
        generated_directory = self.build / "gpu"
        generated_directory.mkdir()
        missing_generated = {
            "directory": str(generated_directory),
            "file": "gpu_tests_autogen/mocs_compilation.cpp",
            "arguments": [
                str(self.compiler),
                "-c",
                "gpu_tests_autogen/mocs_compilation.cpp",
            ],
        }
        database = self.database(
            "compile_commands.json",
            (missing_generated, self.entry("playback/file.cpp")),
        )

        configurations = self.collect((database,))

        self.assertEqual(len(configurations), 1)
        self.assertEqual(
            configurations[0].source.relative,
            PurePosixPath("playback/file.cpp"),
        )

    def test_collection_keeps_missing_production_entry_fail_closed(self):
        missing = {
            "directory": str(self.build),
            "file": "../playback/missing.cpp",
            "arguments": [
                str(self.compiler),
                "-c",
                "../playback/missing.cpp",
            ],
        }
        database = self.database("compile_commands.json", (missing,))

        with self.assertRaisesRegex(
            AuditInfrastructureError,
            "compile entry source is unavailable.*missing.cpp",
        ):
            self.collect((database,))

    def test_collection_rejects_non_array_non_object_and_outside_source(self):
        invalid_root = self.database("root.json", {"not": "an array"})
        invalid_entry = self.database("entry.json", ("not-an-object",))
        outside = self.root.parent / f"{self.root.name}-outside.cpp"
        outside.write_text("int outside;\n", encoding="utf-8")
        outside_entry = {
            "directory": str(self.build),
            "file": str(outside),
            "arguments": [str(self.compiler), "-c", str(outside)],
        }
        outside_db = self.database("outside.json", (outside_entry,))
        try:
            for database, message in (
                (invalid_root, "root must be an array"),
                (invalid_entry, "entry must be an object"),
                (outside_db, "outside production root"),
            ):
                with self.subTest(database=database), self.assertRaisesRegex(
                    AuditInfrastructureError, message
                ):
                    self.collect((database,))
        finally:
            outside.unlink(missing_ok=True)

    def test_missing_active_generic_source_fails_without_fallback(self):
        a_path = self.write_source("playback/a.cpp")
        missing_path = self.write_source("playback/missing.cpp")
        a = self.identity(a_path, "playback/a.cpp")
        missing = self.identity(missing_path, "playback/missing.cpp")
        configuration = self.configuration(a, "a")

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess"
        ) as execute, self.assertRaisesRegex(
            AuditInfrastructureError,
            "active source has no compile command.*playback/missing.cpp",
        ):
            preprocess_all(
                (configuration,),
                self.dependency_roots,
                {a.relative: a, missing.relative: missing},
                self.cache,
                AuditLimits(workers=1, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )
        execute.assert_not_called()

    def test_preprocess_threads_the_same_caller_deadline_to_cache_and_compiler(self):
        source = self.identity(
            self.write_source("playback/pipeline-deadline.cpp"),
            "playback/pipeline-deadline.cpp",
        )
        configuration = self.configuration(source, "pipeline-deadline")
        result = self.view(configuration, (source,))
        deadline = time.monotonic() + 37.0
        with mock.patch(
            "gpu_capability_runner.load_or_preprocess", return_value=result
        ) as execute:
            preprocess_all(
                (configuration,), self.dependency_roots,
                {source.relative: source}, self.cache,
                AuditLimits(workers=1, rss_bytes=2**63 - 1), deadline,
            )
        self.assertEqual(execute.call_args.args[5], deadline)

    def test_platform_classifier_activates_apple_and_windows_from_configurations(self):
        generic_path = self.write_source("playback/generic.cpp")
        apple_main_path = self.write_source("playback/gpu/backend.mm")
        apple_peer_path = self.write_source("playback/gpu/adapter_apple.cpp")
        windows_main_path = self.write_source("playback/output/win/backend.cpp")
        windows_peer_path = self.write_source("playback/output/adapter_win.cpp")
        generic = self.identity(generic_path, "playback/generic.cpp")
        apple_main = self.identity(apple_main_path, "playback/gpu/backend.mm")
        apple_peer = self.identity(apple_peer_path, "playback/gpu/adapter_apple.cpp")
        windows_main = self.identity(windows_main_path, "playback/output/win/backend.cpp")
        windows_peer = self.identity(windows_peer_path, "playback/output/adapter_win.cpp")
        production = {item.relative: item for item in (
            generic, apple_main, apple_peer, windows_main, windows_peer
        )}

        for configurations, missing in (
            (
                (self.configuration(generic, "generic"), self.configuration(apple_main, "apple")),
                "playback/gpu/adapter_apple.cpp",
            ),
            (
                (
                    self.configuration(generic, "generic"),
                    self.configuration(windows_main, "windows"),
                ),
                "playback/output/adapter_win.cpp",
            ),
        ):
            with self.subTest(missing=missing), self.assertRaisesRegex(
                AuditInfrastructureError, f"active source has no compile command.*{missing}"
            ):
                preprocess_all(
                    configurations,
                    self.dependency_roots,
                    production,
                    self.cache,
                    AuditLimits(workers=1, rss_bytes=2**63 - 1),
                    time.monotonic() + 180.0,
                )

    def test_coverage_separates_reached_headers_from_inactive_and_unreached_files(self):
        paths = {
            relative: self.write_source(relative)
            for relative in (
                "playback/a.cpp",
                "playback/gpu/adapter_apple.cpp",
                "playback/output/adapter_win.cpp",
                "playback/reached.h",
                "playback/unreached.hpp",
            )
        }
        identities = {relative: self.identity(path, relative) for relative, path in paths.items()}
        production = {item.relative: item for item in identities.values()}
        configuration = self.configuration(identities["playback/a.cpp"], "cfg")
        result = self.view(
            configuration,
            (identities["playback/a.cpp"], identities["playback/reached.h"]),
            (identities["playback/a.cpp"], identities["playback/reached.h"]),
        )

        with mock.patch("gpu_capability_runner.load_or_preprocess", return_value=result):
            views, coverage = preprocess_all(
                (configuration,),
                self.dependency_roots,
                production,
                self.cache,
                AuditLimits(workers=1, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

        self.assertEqual(views, (result,))
        self.assertEqual(
            coverage.authoritative,
            frozenset({PurePosixPath("playback/a.cpp"), PurePosixPath("playback/reached.h")}),
        )
        self.assertEqual(
            coverage.source_only,
            frozenset({
                PurePosixPath("playback/gpu/adapter_apple.cpp"),
                PurePosixPath("playback/output/adapter_win.cpp"),
                PurePosixPath("playback/unreached.hpp"),
            }),
        )
        self.assertEqual(coverage.configurations, ("cfg",))

    def test_different_compiler_configurations_are_all_returned_in_digest_order(self):
        source_path = self.write_source("playback/a.cpp")
        source = self.identity(source_path, "playback/a.cpp")
        first = self.configuration(source, "z", arguments=("-DMODE=1",))
        second = self.configuration(
            source,
            "a",
            family=CompilerFamily.CLANG,
            arguments=("-DMODE=2",),
        )
        results = {item.digest: self.view(item, (source,)) for item in (first, second)}

        for workers in (1, 4):
            with self.subTest(workers=workers), mock.patch(
                "gpu_capability_runner.load_or_preprocess",
                side_effect=lambda configuration, *_args, **_kwargs: results[configuration.digest],
            ):
                views, coverage = preprocess_all(
                    (first, second),
                    self.dependency_roots,
                    {source.relative: source},
                    self.cache,
                    AuditLimits(workers=workers, rss_bytes=2**63 - 1),
                    time.monotonic() + 180.0,
                )

            self.assertEqual(tuple(view.configuration.digest for view in views), ("a", "z"))
            self.assertEqual(coverage.configurations, ("a", "z"))

    def test_active_main_source_must_be_reached_by_its_validated_view(self):
        source_path = self.write_source("playback/a.cpp")
        source = self.identity(source_path, "playback/a.cpp")
        configuration = self.configuration(source, "cfg")
        incomplete = self.view(configuration, (), ())

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess", return_value=incomplete
        ), self.assertRaisesRegex(
            AuditInfrastructureError,
            "configuration view lacks main-source provenance.*playback/a.cpp",
        ):
            preprocess_all(
                (configuration,),
                self.dependency_roots,
                {source.relative: source},
                self.cache,
                AuditLimits(workers=1, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

    def test_crossed_main_provenance_does_not_satisfy_per_configuration_coverage(self):
        first = self.identity(
            self.write_source("playback/first.cpp"),
            "playback/first.cpp",
        )
        second = self.identity(
            self.write_source("playback/second.cpp"),
            "playback/second.cpp",
        )
        first_configuration = self.configuration(first, "first")
        second_configuration = self.configuration(second, "second")
        results = {
            "first": self.view(
                first_configuration,
                (first, second),
                (second,),
            ),
            "second": self.view(
                second_configuration,
                (first, second),
                (first,),
            ),
        }

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess",
            side_effect=lambda configuration, *_args: results[configuration.digest],
        ), self.assertRaisesRegex(
            AuditInfrastructureError,
            "configuration view lacks main-source provenance.*first.*second",
        ):
            preprocess_all(
                (first_configuration, second_configuration),
                self.dependency_roots,
                {first.relative: first, second.relative: second},
                self.cache,
                AuditLimits(workers=2, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

    def test_manifest_only_header_remains_source_only(self):
        source = self.identity(
            self.write_source("playback/source.cpp"),
            "playback/source.cpp",
        )
        header = self.identity(
            self.write_source("playback/manifest_only.h"),
            "playback/manifest_only.h",
        )
        configuration = self.configuration(source, "cfg")
        result = self.view(configuration, (source, header), (source,))

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess", return_value=result
        ):
            _views, coverage = preprocess_all(
                (configuration,),
                self.dependency_roots,
                {source.relative: source, header.relative: header},
                self.cache,
                AuditLimits(workers=1, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

        self.assertEqual(coverage.authoritative, frozenset({source.relative}))
        self.assertEqual(coverage.source_only, frozenset({header.relative}))

    def test_empty_active_main_is_explicitly_covered_without_token_provenance(self):
        source = self.identity(
            self.write_source("playback/empty.cpp", ""),
            "playback/empty.cpp",
        )
        header = self.identity(
            self.write_source("playback/empty.h", ""),
            "playback/empty.h",
        )
        configuration = self.configuration(source, "empty")
        result = self.view(configuration, (source, header), ())

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess", return_value=result
        ):
            _views, coverage = preprocess_all(
                (configuration,),
                self.dependency_roots,
                {source.relative: source, header.relative: header},
                self.cache,
                AuditLimits(workers=1, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

        self.assertEqual(coverage.authoritative, frozenset({source.relative}))
        self.assertEqual(coverage.source_only, frozenset({header.relative}))

    def test_worker_bound_and_stop_scheduling_after_infrastructure_failure(self):
        identities = []
        configurations = []
        for index in range(5):
            relative = f"playback/source{index}.cpp"
            identity = self.identity(self.write_source(relative), relative)
            identities.append(identity)
            configurations.append(self.configuration(identity, str(index)))
        production = {item.relative: item for item in identities}
        active = 0
        maximum_active = 0
        started: list[str] = []
        lock = threading.Lock()
        barrier = threading.Barrier(2)

        def execute(configuration, *_args, **_kwargs):
            nonlocal active, maximum_active
            with lock:
                active += 1
                maximum_active = max(maximum_active, active)
                started.append(configuration.digest)
            try:
                barrier.wait(timeout=2.0)
                if configuration.digest == "0":
                    raise AuditInfrastructureError("primary failure")
                time.sleep(0.05)
                return self.view(configuration, (configuration.source,))
            finally:
                with lock:
                    active -= 1

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess", side_effect=execute
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "primary failure"
        ):
            preprocess_all(
                tuple(configurations),
                self.dependency_roots,
                production,
                self.cache,
                AuditLimits(workers=2, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

        self.assertEqual(maximum_active, 2)
        self.assertEqual(set(started), {"0", "1"})

    def test_aggregated_failure_diagnostics_are_deterministic(self):
        identities = []
        configurations = []
        for digest in ("z", "a"):
            relative = f"playback/{digest}.cpp"
            identity = self.identity(self.write_source(relative), relative)
            identities.append(identity)
            configurations.append(self.configuration(identity, digest))
        production = {item.relative: item for item in identities}
        barrier = threading.Barrier(2)

        def execute(configuration, *_args, **_kwargs):
            barrier.wait(timeout=2.0)
            if configuration.digest == "a":
                time.sleep(0.02)
            raise AuditInfrastructureError(f"failure-{configuration.digest}")

        with mock.patch(
            "gpu_capability_runner.load_or_preprocess", side_effect=execute
        ), self.assertRaises(
            AuditInfrastructureError
        ) as raised:
            preprocess_all(
                tuple(configurations),
                self.dependency_roots,
                production,
                self.cache,
                AuditLimits(workers=2, rss_bytes=2**63 - 1),
                time.monotonic() + 180.0,
            )

        diagnostic = str(raised.exception)
        self.assertLess(diagnostic.index("digest=a"), diagnostic.index("digest=z"))
        self.assertIn("failure-a", diagnostic)
        self.assertIn("failure-z", diagnostic)


class WorkerAuditTests(unittest.TestCase):
    class _View:
        pass

    class _ControlEndpoint:
        def __init__(self, launch_purposes):
            self.launch_purposes = tuple(launch_purposes)
            self.frames = []
            self.sealed = False
            self.active_carriers = {}
            self.completed_carriers = []
            self.publication_contexts = []

        def set_publication_context(
            self, task_id, generation, configuration_digest, engine, dependencies
        ):
            self.publication_contexts.append((
                task_id, generation, configuration_digest, engine, dependencies
            ))

        def register_compiler_process_launch(self, event, carrier):
            self.active_carriers[event.process_start] = carrier

        def complete_compiler_process_launch(self, event, carrier):
            if self.active_carriers.pop(event.process_start, None) is not carrier:
                raise AuditInfrastructureError("compiler process carrier differs")
            self.completed_carriers.append((event, carrier))

        def fail_compiler_process_launch(self, event, carrier):
            if self.active_carriers.get(event.process_start) is carrier:
                del self.active_carriers[event.process_start]

        def send(self, frame, deadline):
            if time.monotonic() >= deadline:
                raise AuditInfrastructureError("control endpoint deadline exceeded")
            self.frames.append(frame)

        def seal_audit_launch_protocol(self, task_id, generation, deadline):
            events = tuple(
                frame for frame in self.frames
                if isinstance(frame, capability_model.CompilerLaunchEvent)
            )
            observed = tuple(event.purpose for event in events)
            expected = (
                capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY,
                capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED,
            )
            if observed != expected or any(
                event.task_id != task_id or event.generation != generation
                for event in events
            ):
                raise AuditInfrastructureError("audit compiler launch protocol differs")
            self.sealed = True

    class _CommandEndpoint:
        def __init__(self, permit=None, error=None):
            self.permit = permit
            self.error = error
            self.receives = 0

        def receive(self, task, cancel_event, deadline, maximum_quantum_seconds):
            self.receives += 1
            if self.error is not None:
                raise self.error
            return self.permit

    class _Cache:
        def __init__(self, error=None):
            self.error = error
            self.published = []
            self.loads = 0

        def load(self, *_args, **_kwargs):
            self.loads += 1
            raise AssertionError("worker cache hit path reached")

        def publish(self, configuration, dependency_roots, result, permit, deadline):
            self.published.append((configuration, dependency_roots, result, permit, deadline))
            if self.error is not None:
                raise self.error
            return result

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        self.source = self.root / "playback" / "gpu" / "worker.cpp"
        self.header = self.root / "playback" / "gpu" / "empty.h"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("#include \"empty.h\"\nint worker;\n", encoding="utf-8")
        self.header.write_bytes(b"")
        self.toolchain_temporary = tempfile.TemporaryDirectory()
        self.toolchain = Path(self.toolchain_temporary.name).resolve()
        self.compiler = self.toolchain / "g++.exe"
        self.compiler.write_bytes(b"compiler")
        self.authority = build_dependency_root_authority(
            self.root, {"toolchain": self.toolchain}
        )
        self.capability = open_compiler_executable_capability(
            self.compiler,
            self.authority,
            time.monotonic() + 10.0,
            compiler_family=CompilerFamily.GCC,
        )
        self.source_identity = self._identity(
            self.source, "playback/gpu/worker.cpp", line_count=2
        )
        self.header_identity = self._identity(
            self.header, "playback/gpu/empty.h", line_count=0
        )
        self.configuration = PreprocessConfiguration(
            entry_id="worker:0",
            family=CompilerFamily.GCC,
            compiler=self.compiler,
            working_directory=self.build,
            source=self.source_identity,
            arguments=(str(self.compiler), "-E", str(self.source)),
            environment_digest=_environment_digest(dict(os.environ)),
            digest="c" * 64,
            dependency_root_authority_digest=self.authority.portable_authority_digest,
            compiler_capability_digest=self.capability.capability_digest,
            compiler_capability=self.capability,
        )
        self.dependencies = tuple(sorted((
            self._dependency(self.source_identity),
            self._dependency(self.header_identity),
        ), key=lambda item: item.role_relative_path.as_posix()))
        self.production_snapshot = {
            item.role_relative_path: item for item in self.dependencies
        }
        self.engine = capability_audit.audit_engine_fingerprint()
        self.cancel_event = threading.Event()
        self.live_views = weakref.WeakSet()

    def tearDown(self):
        self.capability.native_owner.close()
        _clear_compiler_inspection_memo_for_tests()
        self.temporary.cleanup()
        self.toolchain_temporary.cleanup()

    @staticmethod
    def _identity(path, relative, *, line_count):
        metadata = path.stat()
        return FileIdentity(
            path,
            PurePosixPath(relative),
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            line_count,
            True,
        )

    def _dependency(self, identity):
        return DependencyDigest(
            "production",
            identity.relative,
            identity,
            hashlib.sha256(identity.canonical.read_bytes()).hexdigest(),
        )

    def _task(self, *, reservation=True):
        reservation_object = (
            capability_model.PerTaskCompactReservation("task-a", 7, 32 << 20)
            if reservation else None
        )
        task = capability_model.ConfigurationAuditTask(
            "task-a", 7, self.configuration, self.authority, reservation_object
        )
        return task, reservation_object

    def _permit(self, *, dependencies=None, release_callback=None):
        return capability_model.CachePublicationPermit(
            self.configuration.digest,
            self.engine,
            self.dependencies if dependencies is None else dependencies,
            task_id="task-a",
            generation=7,
            release_callback=release_callback,
        )

    def _run_worker(
        self,
        *,
        task=None,
        launch_purposes=None,
        command=None,
        cache=None,
        dependencies=None,
        findings=(),
        audit_error=None,
        attestation_error=None,
        stabilize_error=None,
        stabilize_override=None,
        worker_outcomes=None,
        return_owner=False,
    ):
        if task is None:
            task, _reservation = self._task()
        parent_reservation = task.compact_reservation
        capability = None
        if isinstance(
            parent_reservation, capability_model.PerTaskCompactReservation
        ):
            try:
                capability = parent_reservation.issue_worker_transport_capability(
                    task.task_id,
                    task.generation,
                    task.configuration.digest,
                    self.engine,
                    3,
                )
                task = dataclasses.replace(
                    task,
                    compact_reservation=(
                        capability_model.PerTaskCompactReservation.for_worker_transport(
                            capability
                        )
                    ),
                )
            except BaseException:
                if not parent_reservation.released:
                    parent_reservation.release("worker-dispatch-failure")
                raise
        if launch_purposes is None:
            launch_purposes = (
                capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY,
                capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED,
            )
        control = self._ControlEndpoint(launch_purposes)
        if command is None:
            command = self._CommandEndpoint(self._permit(
                dependencies=self.dependencies if dependencies is None else dependencies
            ))
        if cache is None:
            cache = self._Cache()
        discovery_dependencies = self.dependencies if dependencies is None else dependencies

        def stabilize(*_args, launch_context=None, **_kwargs):
            if stabilize_error is not None:
                raise stabilize_error
            if stabilize_override is not None:
                return stabilize_override(launch_context)
            view = self._View()
            self.live_views.add(view)
            for index, purpose in enumerate(launch_purposes):
                launch_context.record_process_start(
                    purpose,
                    capability_model.ProcessStartIdentity(
                        "windows", index + 100, f"start-{index}", f"cookie-{index}"
                    ),
                )
            return (
                view,
                capability_runner.PreprocessDiscovery(
                    capability_runner.StreamDigest("d" * 64, 17),
                    discovery_dependencies,
                    tuple(item.identity for item in discovery_dependencies),
                ),
                capability_runner.PreprocessStageTimings(1.0, 2.0, 34),
            )

        def audit(*_args, **_kwargs):
            if audit_error is not None:
                raise audit_error
            return list(findings)

        rss = SimpleNamespace(sample=lambda: 0)
        try:
            with (
                mock.patch.multiple(
                capability_runner,
                _WORKER_INDEX=3,
                _WORKER_GENERATION=7,
                _WORKER_PRODUCTION={
                    self.source_identity.relative: self.source_identity,
                    self.header_identity.relative: self.header_identity,
                },
                _WORKER_LIMITS=AuditLimits(),
                _WORKER_CANCEL_EVENT=self.cancel_event,
                _WORKER_ENGINE=self.engine,
                _WORKER_CACHE=cache,
                _WORKER_RSS=rss,
            ),
                mock.patch.object(
                capability_runner,
                "stabilize_and_parse_configuration",
                new=stabilize,
            ),
                mock.patch.object(
                capability_audit, "audit_preprocessed_view", new=audit
                ),
                mock.patch.object(
                capability_audit,
                "_attest_loaded_audit_engine",
                return_value=(self.engine if attestation_error is None else mock.DEFAULT),
                side_effect=attestation_error,
                ),
            ):
                worker_outcome = capability_runner.audit_configuration_worker(
                    task,
                    self.authority,
                    self.production_snapshot,
                    control,
                    command,
                    time.monotonic() + 30.0,
                )
            if worker_outcomes is not None:
                worker_outcomes.append(worker_outcome)
            owner = capability_runner.receive_configuration_audit_outcome(
                parent_reservation, capability, worker_outcome
            )
            outcome = owner if return_owner else owner.transfer()
        except BaseException:
            if (
                capability is not None
                and parent_reservation is not None
                and not parent_reservation.released
            ):
                parent_reservation.release_worker_transport_capability(
                    capability, "worker-failure"
                )
            raise
        return outcome, control, command, cache

    def test_received_outcome_owner_closes_on_scope_failure(self):
        task, reservation = self._task()
        owner, _control, _command, _cache = self._run_worker(
            task=task, return_owner=True
        )
        self.assertIsInstance(
            owner, capability_model.ConfigurationAuditOutcomeOwner
        )
        self.assertTrue(owner.active)
        with self.assertRaisesRegex(RuntimeError, "downstream failed"):
            with owner:
                self.assertEqual(owner.outcome.stdout_bytes, 34)
                raise RuntimeError("downstream failed")
        self.assertFalse(owner.active)
        self.assertTrue(reservation.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "transferred"):
            owner.close()

    def test_outcome_owner_cleanup_fault_preserves_scope_primary(self):
        task, reservation = self._task()
        owner, _control, _command, _cache = self._run_worker(
            task=task, return_owner=True
        )
        with mock.patch.object(
            capability_model.ConfigurationAuditResult,
            "release_transport_ownership",
            side_effect=RuntimeError("owner cleanup secondary"),
        ), self.assertRaisesRegex(ValueError, "scope primary") as raised:
            with owner:
                raise ValueError("scope primary")
        self.assertTrue(any(
            "owner cleanup secondary" in note
            for note in getattr(raised.exception, "__notes__", ())
        ))
        self.assertTrue(owner.active)
        self.assertFalse(reservation.released)
        owner.close()
        self.assertTrue(reservation.released)

    def test_received_outcome_owner_transfers_exactly_once(self):
        task, reservation = self._task()
        owner, _control, _command, _cache = self._run_worker(
            task=task, return_owner=True
        )
        outcome = owner.transfer()
        self.assertFalse(owner.active)
        self.assertFalse(reservation.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "transferred"):
            owner.transfer()
        outcome.result.release_transport_ownership()
        self.assertTrue(reservation.released)

    def test_owner_construction_failure_releases_received_result(self):
        task, reservation = self._task()
        with mock.patch.object(
            capability_runner,
            "ConfigurationAuditOutcomeOwner",
            side_effect=MemoryError("owner construction primary"),
        ), self.assertRaisesRegex(MemoryError, "owner construction primary"):
            self._run_worker(task=task)
        self.assertTrue(reservation.released)
        self.assertEqual(
            reservation.release_phase, "retained-result-transition"
        )

    def test_aggregation_acceptor_failure_keeps_owner_closeable(self):
        task, reservation = self._task()
        owner, _control, _command, _cache = self._run_worker(
            task=task, return_owner=True
        )

        def reject(_outcome):
            raise ValueError("aggregation rejected outcome")

        with self.assertRaisesRegex(ValueError, "aggregation rejected"):
            with owner:
                owner.transfer_to(reject)
        self.assertTrue(reservation.released)

        task, reservation = self._task()
        owner, _control, _command, _cache = self._run_worker(
            task=task, return_owner=True
        )
        retained = []
        accepted = owner.transfer_to(
            lambda outcome: retained.append(outcome) or "accepted"
        )
        self.assertEqual(accepted, "accepted")
        self.assertFalse(owner.active)
        self.assertFalse(reservation.released)
        retained.pop().result.release_transport_ownership()
        self.assertTrue(reservation.released)

    def test_worker_returns_only_compact_outcome_drops_view_and_never_loads(self):
        task, reservation = self._task()
        worker_outcomes = []
        outcome, control, _command, cache = self._run_worker(
            task=task, worker_outcomes=worker_outcomes
        )
        self.assertEqual(len(worker_outcomes), 1)
        self.assertIsInstance(
            worker_outcomes[0], capability_model.ConfigurationAuditTransportOutcome
        )
        self.assertFalse(hasattr(worker_outcomes[0], "result"))
        self.assertIsInstance(outcome, capability_model.ConfigurationAuditOutcome)
        self.assertFalse(hasattr(outcome, "view"))
        self.assertEqual(len(self.live_views), 0)
        self.assertEqual(cache.loads, 0)
        self.assertEqual(control.publication_contexts, [(
            "task-a", 7, self.configuration.digest, self.engine,
            self.dependencies,
        )])
        self.assertEqual(outcome.result.reached_production, (
            PurePosixPath("playback/gpu/empty.h"),
            PurePosixPath("playback/gpu/worker.cpp"),
        ))
        self.assertEqual(outcome.stdout_bytes, 34)
        self.assertTrue(control.sealed)
        self.assertFalse(reservation.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "ownership.*transferred"):
            cache.published[0][2].release_transport_ownership()
        outcome.result.release_transport_ownership()
        self.assertTrue(reservation.released)

    def test_inadequate_reservation_fails_before_launch_audit_or_draft(self):
        reservation = capability_model.PerTaskCompactReservation("task-a", 7, 1)
        task = capability_model.ConfigurationAuditTask(
            "task-a", 7, self.configuration, self.authority, reservation
        )
        with mock.patch.object(
            capability_runner, "stabilize_and_parse_configuration"
        ) as stabilize, mock.patch.object(
            capability_audit, "audit_preprocessed_view"
        ) as audit, mock.patch.object(
            capability_runner, "_bounded_compact_result_draft"
        ) as draft, self.assertRaisesRegex(
            AuditInfrastructureError, "pre-dispatch compact reservation"
        ):
            self._run_worker(task=task)
        stabilize.assert_not_called()
        audit.assert_not_called()
        draft.assert_not_called()
        self.assertTrue(reservation.released)

    def test_exact_canonical_payload_is_charged_and_owned_through_receiver(self):
        task, reservation = self._task()
        finding = capability_audit.Finding(
            PurePosixPath("playback/gpu/worker.cpp"),
            2,
            'lease.nativeHandle("雪")',
            "quoted \\\"reason\\\"",
        )
        original_compact = capability_runner._compact_findings

        def compact_only_after_exact_charge(findings):
            self.assertEqual(
                reservation.owner_phase, "worker-transport-dispatched"
            )
            return original_compact(findings)

        with mock.patch.object(
            capability_runner, "_compact_findings",
            side_effect=compact_only_after_exact_charge,
        ):
            outcome, _control, _command, _cache = self._run_worker(
                task=task, findings=(finding,)
            )
        encoded = capability_cache.encode_configuration_audit_result(outcome.result)
        self.assertEqual(reservation.canonical_json_bytes, len(encoded))
        self.assertEqual(reservation.owner_phase, "receiver-retained-result")
        self.assertFalse(reservation.released)
        outcome.result.release_transport_ownership()
        self.assertTrue(reservation.released)
        self.assertEqual(reservation.release_phase, "retained-result-transition")

    def test_root_release_failure_still_releases_reservation(self):
        def fail_root_release():
            raise AuditInfrastructureError("root release failed")

        task, reservation = self._task()
        command = self._CommandEndpoint(
            self._permit(release_callback=fail_root_release)
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "root release failed"
        ):
            self._run_worker(task=task, command=command)
        self.assertTrue(reservation.released)

        task, reservation = self._task()
        command = self._CommandEndpoint(
            self._permit(release_callback=fail_root_release)
        )
        cache = self._Cache(AuditInfrastructureError("primary publish failed"))
        with self.assertRaisesRegex(
            AuditInfrastructureError, "primary publish failed"
        ) as raised:
            self._run_worker(task=task, command=command, cache=cache)
        self.assertTrue(reservation.released)
        self.assertTrue(any(
            "root publication permit cleanup also failed: root release failed"
            in note
            for note in (raised.exception.__notes__ or ())
        ))

    def test_audit_count_comes_only_from_typed_parent_observed_launch_events(self):
        outcome, control, _command, _cache = self._run_worker()
        events = tuple(
            frame for frame in control.frames
            if isinstance(frame, capability_model.CompilerLaunchEvent)
        )
        expected = (
            capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY,
            capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED,
        )
        self.assertEqual(tuple(event.purpose for event in events), expected)
        self.assertEqual(len(events), len(expected))
        self.assertFalse(hasattr(outcome, "audit_compiler_invocations"))
        worker_source = Path(capability_runner.__file__).read_text(encoding="utf-8")
        self.assertNotIn("stream.byte_count" + " * 2", worker_source)
        corruptions = {
            "missing": expected[:1],
            "duplicate": (expected[0], expected[0], expected[1]),
            "retry": (*expected, expected[1]),
            "extra": (*expected, capability_model.CompilerLaunchPurpose.INSPECTION),
        }
        for name, purposes in corruptions.items():
            with self.subTest(corruption=name), self.assertRaisesRegex(
                AuditInfrastructureError, "audit compiler launch protocol"
            ):
                self._run_worker(launch_purposes=purposes)

    def test_generation_engine_and_permit_failures_never_publish(self):
        changed = dataclasses.replace(self.dependencies[0], sha256="e" * 64)
        for label, kwargs, expected in (
            ("production", {"dependencies": (changed, *self.dependencies[1:])},
             "production snapshot generation"),
            ("permit", {"command": self._CommandEndpoint(
                error=AuditInfrastructureError("publication permit unavailable"))},
             "publication permit unavailable"),
            ("audit", {"audit_error": AuditInfrastructureError("audit failed")},
             "audit failed"),
            ("raw", {"stabilize_error": AuditInfrastructureError(
                "raw preprocessed output changed")},
             "raw preprocessed output changed"),
        ):
            cache = self._Cache()
            with self.subTest(label=label), self.assertRaisesRegex(
                AuditInfrastructureError, expected
            ):
                self._run_worker(cache=cache, **kwargs)
            self.assertEqual(cache.published, [])

        cache = self._Cache()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "loaded audit engine attestation"
        ):
            self._run_worker(
                cache=cache,
                attestation_error=AuditInfrastructureError(
                    "loaded audit engine attestation mismatch"
                ),
            )
        self.assertEqual(cache.published, [])

    def test_restoring_initial_bytes_cannot_hide_mixed_production_generation(self):
        compiler_text = shutil.which("g++")
        if compiler_text is None:
            self.skipTest("requires a production g++ compiler")
        compiler = Path(compiler_text).resolve()
        generation_a = b"#define WORKER_VALUE 1\n"
        generation_b = b"#define WORKER_VALUE 2\n"
        self.source.write_text(
            '#include "empty.h"\nint worker = WORKER_VALUE;\n',
            encoding="utf-8",
        )
        self.header.write_bytes(generation_a)
        production = capability_model.enumerate_production_identities(
            self.root, AuditLimits(), time.monotonic() + 30.0
        )
        toolchain_root = compiler.parent.parent
        roots = {"toolchain": toolchain_root}
        runtime_candidates = []
        if os.name == "nt":
            runtime_candidates.append((
                "windows-system",
                Path(os.environ.get("SystemRoot", "C:/Windows")),
            ))
        elif sys.platform.startswith("linux"):
            runtime_candidates.extend((
                ("system-lib", Path("/lib")),
                ("system-lib64", Path("/lib64")),
            ))
        elif sys.platform == "darwin":
            runtime_candidates.append(("system-frameworks", Path("/System")))
        for role, candidate in runtime_candidates:
            try:
                resolved = candidate.resolve(strict=True)
            except OSError:
                continue
            if resolved.is_dir() and not any(
                resolved == existing
                or resolved.is_relative_to(existing)
                or existing.is_relative_to(resolved)
                for existing in roots.values()
            ):
                roots[role] = resolved
        authority = build_dependency_root_authority(self.root, roots)
        capability = open_compiler_executable_capability(
            compiler,
            authority,
            time.monotonic() + 30.0,
            compiler_family=CompilerFamily.GCC,
        )
        configuration = dataclasses.replace(
            self.configuration,
            entry_id="real-generation-race:0",
            family=CompilerFamily.GCC,
            compiler=compiler,
            working_directory=self.build,
            source=production[PurePosixPath("playback/gpu/worker.cpp")],
            arguments=(str(self.source), "-I", str(self.source.parent)),
            environment_digest=_environment_digest(dict(os.environ)),
            digest="b" * 64,
            dependency_root_authority_digest=authority.portable_authority_digest,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )
        task = capability_model.ConfigurationAuditTask(
            "real-generation-race", 11, configuration, authority,
            capability_model.PerTaskCompactReservation(
                "real-generation-race", 11, 32 << 20
            ),
        )
        production_snapshot = {
            relative: DependencyDigest(
                "production", relative, identity,
                hashlib.sha256(identity.canonical.read_bytes()).hexdigest(),
            )
            for relative, identity in production.items()
        }
        observed_generations = []
        events = []
        test_case = self

        class Endpoint:
            def register_compiler_process_launch(self, event, carrier):
                observed_generations.append(test_case.header.read_bytes())
                process = carrier.process
                test_case.assertEqual(process.pid, event.process_start.pid)
                test_case.assertNotIn("subprocess.call", " ".join(process.args))
                if os.name == "nt":
                    test_case.assertEqual(
                        Path(process.args[0]).resolve(), compiler
                    )

            def complete_compiler_process_launch(self, event, _carrier):
                pass

            def fail_compiler_process_launch(self, _event, _carrier): pass

            def send(self, frame, _deadline):
                if isinstance(frame, capability_model.CompilerLaunchEvent):
                    events.append(frame)

            def seal_audit_launch_protocol(self, task_id, generation, _deadline):
                test_case.assertEqual((task_id, generation), (task.task_id, 11))
                test_case.header.write_bytes(generation_a)

        self.header.write_bytes(generation_b)
        deadline = time.monotonic() + 30.0
        cache = self._Cache()
        try:
            with mock.patch.multiple(
                capability_runner,
                _WORKER_INDEX=2,
                _WORKER_GENERATION=11,
                _WORKER_PRODUCTION=production,
                _WORKER_LIMITS=AuditLimits(rss_bytes=2**63 - 1),
                _WORKER_CANCEL_EVENT=threading.Event(),
                _WORKER_ENGINE=self.engine,
                _WORKER_CACHE=cache,
                _WORKER_RSS=SimpleNamespace(sample=lambda: 0),
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "production snapshot generation"
            ):
                capability_runner.audit_configuration_worker(
                    task,
                    authority,
                    production_snapshot,
                    Endpoint(),
                    self._CommandEndpoint(),
                    deadline,
                )
        finally:
            capability.native_owner.close()
            self.header.write_bytes(generation_a)
        self.assertEqual(
            tuple(event.purpose for event in events),
            (
                capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY,
                capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED,
            ),
        )
        self.assertEqual(observed_generations, [generation_b, generation_b])
        self.assertEqual(self.header.read_bytes(), generation_a)
        self.assertEqual(cache.published, [])
        self.assertTrue(task.compact_reservation.released)

    def test_publication_permit_precedes_result_materialization_and_send(self):
        task, reservation = self._task()
        command = self._CommandEndpoint(
            error=AuditInfrastructureError("publication permit unavailable")
        )
        cache = self._Cache()
        with mock.patch.object(
            capability_runner,
            "ConfigurationAuditResult",
            side_effect=AssertionError("compact result materialized"),
        ) as materialized, self.assertRaisesRegex(
            AuditInfrastructureError, "publication permit unavailable"
        ):
            self._run_worker(task=task, command=command, cache=cache)
        materialized.assert_not_called()
        self.assertEqual(cache.published, [])
        self.assertTrue(reservation.released)

    def test_cancellation_after_root_grant_releases_both_owners_without_publish(self):
        releases = []
        permit = self._permit(release_callback=lambda: releases.append("root"))
        cancel_event = self.cancel_event

        class CancellingCommandEndpoint:
            def receive(self, task, observed_cancel, deadline, maximum_quantum_seconds):
                cancel_event.set()
                return permit

        task, reservation = self._task()
        cache = self._Cache()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "cancelled before compact-result materialization"
        ):
            self._run_worker(
                task=task,
                command=CancellingCommandEndpoint(),
                cache=cache,
            )
        self.assertEqual(cache.published, [])
        self.assertEqual(releases, ["root"])
        self.assertTrue(reservation.released)

    def test_process_handle_association_freezes_start_identity_and_has_one_launcher(self):
        runner_source = Path(capability_runner.__file__).read_text(encoding="utf-8")
        command_source = Path(capability_command.__file__).read_text(encoding="utf-8")
        self.assertEqual(runner_source.count("subprocess." + "Popen("), 0)
        self.assertEqual(
            inspect.getsource(capability_command.launch_compiler_process).count(
                "subprocess." + "Popen("
            ),
            1,
        )
        self.assertNotIn("_process_handle_associations", runner_source)
        self.assertIn("complete_after_exit", command_source)

    def test_two_workers_cannot_overlap_the_exact_root_publication_charge(self):
        releases = []
        receive_barrier = threading.Barrier(2)
        publication_gate = threading.Lock()
        test_case = self

        class SerializedCommandEndpoint:
            def receive(self, task, cancel_event, deadline, maximum_quantum_seconds):
                receive_barrier.wait(timeout=10.0)
                if not publication_gate.acquire(timeout=10.0):
                    raise AuditInfrastructureError("root publication gate timed out")
                return test_case._permit(
                    release_callback=lambda: (
                        releases.append(task.task_id), publication_gate.release()
                    )
                )

        class ObservedCache(self._Cache):
            def __init__(self):
                super().__init__()
                self.active = 0
                self.maximum_active = 0
                self.lock = threading.Lock()

            def publish(self, *args):
                with self.lock:
                    self.active += 1
                    self.maximum_active = max(self.maximum_active, self.active)
                try:
                    time.sleep(0.05)
                    return super().publish(*args)
                finally:
                    with self.lock:
                        self.active -= 1

        command = SerializedCommandEndpoint()
        cache = ObservedCache()
        tasks = tuple(self._task()[0] for _index in range(2))
        outcomes = []
        errors = []

        def execute(task):
            try:
                outcomes.append(self._run_worker(
                    task=task, command=command, cache=cache
                )[0])
            except BaseException as error:
                errors.append(error)

        threads = tuple(threading.Thread(target=execute, args=(task,)) for task in tasks)
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=20.0)
        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(errors, [])
        self.assertEqual(len(outcomes), 2)
        self.assertEqual(cache.maximum_active, 1)
        self.assertEqual(releases, ["task-a", "task-a"])
        self.assertTrue(all(
            not task.compact_reservation.released for task in tasks
        ))
        for outcome in outcomes:
            outcome.result.release_transport_ownership()
        self.assertTrue(all(task.compact_reservation.released for task in tasks))

    def test_missing_reservation_and_cancellation_launch_nothing(self):
        task, _reservation = self._task(reservation=False)
        with mock.patch.object(
            capability_runner, "stabilize_and_parse_configuration"
        ) as stabilize, self.assertRaisesRegex(
            AuditInfrastructureError, "dispatch reservation"
        ):
            self._run_worker(task=task)
        stabilize.assert_not_called()

        task, reservation = self._task()
        task = dataclasses.replace(
            task,
            dependency_root_authority=dataclasses.replace(self.authority),
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "authority identity changed"
        ):
            self._run_worker(task=task)
        self.assertTrue(reservation.released)

        task, reservation = self._task()
        self.cancel_event.set()
        with mock.patch.object(
            capability_runner, "stabilize_and_parse_configuration"
        ) as stabilize, self.assertRaisesRegex(
            AuditInfrastructureError, "cancelled before discovery"
        ):
            self._run_worker(task=task)
        stabilize.assert_not_called()
        self.assertTrue(reservation.released)

    def test_forbidden_finding_is_compacted_and_publish_failure_releases_owners(self):
        finding = capability_audit.Finding(
            PurePosixPath("playback/gpu/worker.cpp"),
            2,
            "lease.nativeHandle()",
            "raw native handle use",
        )
        outcome, _control, _command, _cache = self._run_worker(findings=(finding,))
        self.assertEqual(len(outcome.result.findings), 1)
        self.assertEqual(outcome.result.findings[0].reason, "raw native handle use")
        outcome.result.release_transport_ownership()

        releases = []
        task, reservation = self._task()
        command = self._CommandEndpoint(self._permit(release_callback=lambda: releases.append("root")))
        cache = self._Cache(AuditInfrastructureError("cache publish failed"))
        with self.assertRaisesRegex(AuditInfrastructureError, "cache publish failed"):
            self._run_worker(task=task, command=command, cache=cache)
        self.assertEqual(releases, ["root"])
        self.assertTrue(reservation.released)


class ProcessCoordinatorTests(unittest.TestCase):
    """Locked Task-7 parent/process boundary and admission invariants."""

    @classmethod
    def setUpClass(cls):
        cls.engine = capability_audit.audit_engine_fingerprint()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "playback" / "gpu" / "coordinator.cpp"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("int coordinator;\n", encoding="utf-8")
        self.toolchain_temporary = tempfile.TemporaryDirectory()
        self.toolchain = Path(self.toolchain_temporary.name).resolve()
        self.compiler = self.toolchain / "compiler.exe"
        self.compiler.write_bytes(b"coordinator-compiler")
        self.authority = build_dependency_root_authority(
            self.root, {"toolchain": self.toolchain}
        )
        self.capability = open_compiler_executable_capability(
            self.compiler,
            self.authority,
            time.monotonic() + 10.0,
            compiler_family=CompilerFamily.GCC,
        )
        metadata = self.source.stat()
        self.configuration = PreprocessConfiguration(
            entry_id="coordinator:0",
            family=CompilerFamily.GCC,
            compiler=self.compiler,
            working_directory=self.root,
            source=FileIdentity(
                self.source,
                PurePosixPath("playback/gpu/coordinator.cpp"),
                int(metadata.st_dev),
                int(metadata.st_ino) if int(metadata.st_ino) else None,
                1,
                True,
            ),
            arguments=(str(self.source),),
            environment_digest="coordinator-environment",
            digest="c" * 64,
            dependency_root_authority_digest=(
                self.authority.portable_authority_digest
            ),
            compiler_capability_digest=self.capability.capability_digest,
            compiler_capability=self.capability,
        )
        self.runtime = capability_model.WorkerRuntimeContract(
            2, 2, 1 << 30, time.monotonic() + 30.0
        )

    def tearDown(self):
        self.capability.native_owner.close()
        _clear_compiler_inspection_memo_for_tests()
        self.temporary.cleanup()
        self.toolchain_temporary.cleanup()

    def test_parent_load_many_validates_warm_batch_before_spawning(self):
        configuration = self.configuration
        engine = self.engine

        class WarmCache:
            root = self.root / "cache"

            def load_many(
                _self, configurations, dependency_roots, loaded_engine,
                production_snapshot, result_budget, aggregator,
                maximum_cold_slot, pipeline_deadline,
            ):
                self.assertEqual(configurations, (configuration,))
                self.assertIs(dependency_roots, self.authority)
                self.assertEqual(loaded_engine, engine)
                self.assertGreater(pipeline_deadline, time.monotonic())
                result = capability_model.ConfigurationAuditResult(
                    configuration.digest, engine, (),
                    (configuration.source.relative,), ()
                )
                ownership = result_budget.reserve(
                    capability_model.compact_result_retained_bytes(result),
                    label="warm fixture result",
                ).commit()
                ownership.record_semantic(
                    "retain-hit", delta_bytes=ownership.byte_count
                )
                aggregator.accept_validated_result(
                    configuration, result, ownership
                )
                return ConfigurationAuditLoadBatch(
                    1, (), 0, 4096, 16384, 2048
                )

        with mock.patch("multiprocessing.Process.start") as start:
            session = capability_runner.schedule_configuration_audits(
                self.root,
                (configuration,),
                self.authority,
                SimpleNamespace(),
                {},
                WarmCache(),
                AuditLimits(),
                engine,
                self.runtime,
                inspection_probe_invocations=7,
                run_accountant=SimpleNamespace(),
                compact_observer=capability_model.CompactAccountingObserver(),
            )
        start.assert_not_called()
        summary = session.consume_aggregate()
        self.assertEqual(summary.configurations, (configuration.digest,))
        self.assertEqual(session.cache_hits, 1)
        self.assertEqual(session.cache_misses, 0)
        self.assertEqual(session.inspection_probe_invocations, 7)
        self.assertEqual(session.audit_compiler_invocations, 0)
        self.assertEqual(session.stdout_bytes, 0)
        self.assertGreater(session.maximum_encoded_result_bytes, 0)
        self.assertGreater(session.maximum_conservative_decoded_bytes, 0)
        self.assertGreater(session.maximum_conservative_retained_bytes, 0)
        session.shutdown_reap(self.runtime.pipeline_deadline)
        summary.release()

    def test_scheduler_preserves_run_scoped_owners_and_observer(self):
        configuration = self.configuration
        observer = capability_model.CompactAccountingObserver()
        registry = SimpleNamespace(register=lambda capability: capability.capability_digest)
        accountant = SimpleNamespace()

        class WarmCache:
            root = self.root / "owner-cache"

            def load_many(
                _self, configurations, dependency_roots, loaded_engine,
                production_snapshot, result_budget, aggregator,
                maximum_cold_slot, pipeline_deadline,
            ):
                self.assertIs(result_budget.observer, observer)
                result = capability_model.ConfigurationAuditResult(
                    configuration.digest, self.engine, (),
                    (configuration.source.relative,), ()
                )
                ownership = result_budget.reserve(
                    capability_model.compact_result_retained_bytes(result),
                    label="owner fixture result",
                ).commit()
                ownership.record_semantic(
                    "retain-hit", delta_bytes=ownership.byte_count
                )
                aggregator.accept_validated_result(
                    configuration, result, ownership
                )
                return ConfigurationAuditLoadBatch(
                    1, (), 0, 4096, 16384, 2048
                )

        session = capability_runner.schedule_configuration_audits(
            self.root,
            (configuration,),
            self.authority,
            registry,
            {},
            WarmCache(),
            AuditLimits(),
            self.engine,
            self.runtime,
            inspection_probe_invocations=3,
            run_accountant=accountant,
            compact_observer=observer,
        )
        self.assertIs(session.compact_accounting_observer, observer)
        self.assertIs(session.run_accountant, accountant)
        self.assertIs(session.capability_registry, registry)
        self.assertIs(session.runtime_contract, self.runtime)
        self.assertEqual(session.inspection_probe_invocations, 3)
        summary = session.consume_aggregate()
        session.shutdown_reap(self.runtime.pipeline_deadline)
        summary.release()

    def test_decision_and_correctness_entries_construct_the_same_runtime_contract(self):
        decision = object()
        registry = object()
        cache = object()
        accountant = object()
        observer = capability_model.CompactAccountingObserver()
        common = dict(
            decision=decision,
            pipeline_deadline=self.runtime.pipeline_deadline,
            source_root=self.root,
            configurations=(self.configuration,),
            dependency_roots=self.authority,
            capability_registry=registry,
            initial_digest_map={},
            prepared_cache=cache,
            limits=AuditLimits(),
            expected_audit_engine_fingerprint=self.engine,
            inspection_probe_invocations=5,
            run_accountant=accountant,
            compact_observer=observer,
        )
        decision_session = object()
        correctness_session = object()
        with mock.patch(
            "gpu_capability_calibration.runtime_contract_from_platform_decision",
            return_value=self.runtime,
        ) as contract, mock.patch(
            "gpu_capability_runner.schedule_configuration_audits",
            side_effect=(decision_session, correctness_session),
        ) as schedule:
            self.assertIs(
                capability_calibration.schedule_decision_configuration_audits(
                    **common
                ),
                decision_session,
            )
            self.assertIs(
                capability_audit.schedule_correctness_configuration_audits(
                    **common
                ),
                correctness_session,
            )
        self.assertEqual(
            contract.call_args_list,
            [
                mock.call(decision, self.runtime.pipeline_deadline),
                mock.call(decision, self.runtime.pipeline_deadline),
            ],
        )
        self.assertEqual(schedule.call_count, 2)
        for call in schedule.call_args_list:
            self.assertEqual(call.args[:3], (
                self.root, (self.configuration,), self.authority
            ))
            self.assertIs(call.args[3], registry)
            self.assertIs(call.args[8], self.runtime)
            self.assertEqual(call.kwargs["inspection_probe_invocations"], 5)
            self.assertIs(call.kwargs["run_accountant"], accountant)
            self.assertIs(call.kwargs["compact_observer"], observer)

    def test_task6_fake_decision_seam_reaches_real_scheduler(self):
        observer = capability_model.CompactAccountingObserver()
        accountant = SimpleNamespace()
        registry = SimpleNamespace()

        class WarmCache:
            root = self.root / "decision-seam-cache"

            def load_many(
                _self, configurations, dependency_roots, loaded_engine,
                production_snapshot, result_budget, aggregator,
                maximum_cold_slot, pipeline_deadline,
            ):
                result = capability_model.ConfigurationAuditResult(
                    self.configuration.digest, self.engine, (),
                    (self.configuration.source.relative,), ()
                )
                ownership = result_budget.reserve(
                    capability_model.compact_result_retained_bytes(result),
                    label="task6 fake decision warm result",
                ).commit()
                ownership.record_semantic(
                    "retain-hit", delta_bytes=ownership.byte_count
                )
                aggregator.accept_validated_result(
                    self.configuration, result, ownership
                )
                return ConfigurationAuditLoadBatch(
                    1, (), 0, 4096, 16384, 2048
                )

        fake_task6_decision = object()
        with mock.patch(
            "gpu_capability_calibration.runtime_contract_from_platform_decision",
            return_value=self.runtime,
        ) as convert:
            session = capability_calibration.schedule_decision_configuration_audits(
                decision=fake_task6_decision,
                pipeline_deadline=self.runtime.pipeline_deadline,
                source_root=self.root,
                configurations=(self.configuration,),
                dependency_roots=self.authority,
                capability_registry=registry,
                initial_digest_map={},
                prepared_cache=WarmCache(),
                limits=AuditLimits(),
                expected_audit_engine_fingerprint=self.engine,
                inspection_probe_invocations=6,
                run_accountant=accountant,
                compact_observer=observer,
            )
        convert.assert_called_once_with(
            fake_task6_decision, self.runtime.pipeline_deadline
        )
        self.assertIs(session.runtime_contract, self.runtime)
        self.assertIs(session.run_accountant, accountant)
        self.assertIs(session.compact_accounting_observer, observer)
        self.assertEqual(session.cache_hits, 1)
        summary = session.consume_aggregate()
        session.shutdown_reap(self.runtime.pipeline_deadline)
        summary.release()

    def test_real_calibration_runner_decision_audit_path_invokes_scheduler(self):
        runner = capability_calibration.RealCalibrationRunner(
            "windows" if os.name == "nt" else "linux"
        )
        decision = SimpleNamespace(platform_kind=runner.platform_kind)
        expected_session = object()
        common = dict(
            decision=decision,
            pipeline_deadline=self.runtime.pipeline_deadline,
            source_root=self.root,
            configurations=(self.configuration,),
            dependency_roots=self.authority,
            capability_registry=object(),
            initial_digest_map={},
            prepared_cache=object(),
            limits=AuditLimits(),
            expected_audit_engine_fingerprint=self.engine,
            inspection_probe_invocations=3,
            run_accountant=object(),
            compact_observer=capability_model.CompactAccountingObserver(),
        )
        with mock.patch(
            "gpu_capability_calibration.schedule_decision_configuration_audits",
            return_value=expected_session,
        ) as schedule:
            self.assertIs(
                runner.run(kind="decision-audit", **common),
                expected_session,
            )
        schedule.assert_called_once_with(**common)

    def test_correctness_cli_and_prepared_execution_reach_scheduler(self):
        expected_session = object()
        common = dict(
            decision=object(),
            pipeline_deadline=self.runtime.pipeline_deadline,
            source_root=self.root,
            configurations=(self.configuration,),
            dependency_roots=self.authority,
            capability_registry=object(),
            initial_digest_map={},
            prepared_cache=object(),
            limits=AuditLimits(),
            expected_audit_engine_fingerprint=self.engine,
            inspection_probe_invocations=3,
            run_accountant=object(),
            compact_observer=capability_model.CompactAccountingObserver(),
        )
        with mock.patch(
            "gpu_capability_source_audit.schedule_correctness_configuration_audits",
            return_value=expected_session,
        ) as schedule:
            self.assertIs(
                capability_audit.execute_prepared_correctness_audits(**common),
                expected_session,
            )
        schedule.assert_called_once_with(**common)

        database = self.root / "compile_commands.json"
        decision_path = self.root / "worker-decision.json"
        with mock.patch(
            "gpu_capability_source_audit.run_correctness_only_cli",
            return_value=None,
        ) as run:
            self.assertEqual(
                capability_audit.main((
                    "--source-root", str(self.root),
                    "--compile-commands", str(database),
                    "--correctness-only",
                    "--worker-decision", str(decision_path),
                    "--dependency-root", f"toolchain={self.toolchain}",
                )),
                0,
            )
        run.assert_called_once()
        parsed = run.call_args.args[0]
        self.assertEqual(parsed.compile_commands, database)
        self.assertEqual(parsed.worker_decision, decision_path)
        self.assertEqual(parsed.dependency_root, [f"toolchain={self.toolchain}"])

        with mock.patch(
            "gpu_capability_source_audit.execute_prepared_correctness_audits"
        ) as schedule:
            self.assertEqual(
                capability_audit.main((
                    "--source-root", str(self.root),
                    "--compile-commands", str(database),
                    "--correctness-only",
                    "--worker-decision", str(decision_path),
                    "--dependency-root", f"toolchain={self.toolchain}",
                )),
                2,
            )
        schedule.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows native scheduler proof")
    def test_task8_a_to_b_to_a_real_scheduler_worker_never_publishes(self):
        compiler_text = shutil.which("g++")
        if compiler_text is None:
            self.skipTest("requires a production g++ compiler")
        compiler = Path(compiler_text).resolve()
        original = b"int coordinator_a;\n"
        mutated = b"int coordinator_b;\n"
        self.source.write_bytes(original)
        metadata = self.source.stat()
        source_identity = FileIdentity(
            self.source,
            PurePosixPath("playback/gpu/coordinator.cpp"),
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            1,
            True,
        )
        production = {source_identity.relative: source_identity}
        roots = {
            "toolchain": (
                compiler.parents[3]
                if len(compiler.parents) > 3 else compiler.parent.parent
            ),
            "windows-system": Path(
                os.environ.get("SystemRoot", "C:/Windows")
            ).resolve(),
        }
        authority = build_dependency_root_authority(self.root, roots)
        deadline = time.monotonic() + 180.0
        capability = open_compiler_executable_capability(
            compiler,
            authority,
            deadline,
            compiler_family=CompilerFamily.GCC,
            working_directory=self.root,
            preprocess_arguments=(str(self.source),),
        )
        configuration = PreprocessConfiguration(
            entry_id="task8-real-generation:0",
            family=CompilerFamily.GCC,
            compiler=compiler,
            working_directory=self.root,
            source=source_identity,
            arguments=(str(self.source),),
            environment_digest=_environment_digest(dict(os.environ)),
            digest="8" * 64,
            dependency_root_authority_digest=authority.portable_authority_digest,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )
        registry = capability_runner.CompilerCapabilityRegistry(authority)
        registry.register(capability)
        cache = ConfigurationAuditCache((self.root / "task8-real-cache").resolve())
        cache.prepare(deadline)
        from gpu_capability_process_tree import WindowsNativeRunAccountant

        accountant = WindowsNativeRunAccountant(os.getpid())
        launches = []
        publications = []
        observed_maps = []
        try:
            with mock.patch.object(
                capability_runner,
                "_open_production_descriptor",
                side_effect=lambda selected: selected.open("rb"),
            ):
                with snapshot_production_sources(
                    production, AuditLimits(), deadline
                ) as held:
                    initial_map = held.initial_digest_map
                    initial_items = tuple(initial_map.items())
                    self.source.write_bytes(mutated)
                    with mock.patch.object(
                        capability_runner,
                        "_scheduler_compiler_launch_event",
                        side_effect=launches.append,
                    ), mock.patch.object(
                        capability_runner,
                        "_scheduler_cache_publication_event",
                        side_effect=publications.append,
                    ), mock.patch.object(
                        capability_runner,
                        "_scheduler_initial_digest_map_event",
                        side_effect=observed_maps.append,
                    ), mock.patch(
                        "gpu_capability_source_audit._attest_loaded_audit_engine",
                        return_value=self.engine,
                    ), self.assertRaisesRegex(
                        AuditInfrastructureError,
                        "production snapshot generation",
                    ):
                        capability_runner.schedule_configuration_audits(
                            self.root,
                            (configuration,),
                            authority,
                            registry,
                            initial_map,
                            cache,
                            AuditLimits(rss_bytes=2**63 - 1),
                            self.engine,
                            capability_model.WorkerRuntimeContract(
                                1, 1, 1 << 30, deadline
                            ),
                            inspection_probe_invocations=0,
                            run_accountant=accountant,
                            compact_observer=(
                                capability_model.CompactAccountingObserver()
                            ),
                        )
                    self.source.write_bytes(original)
                    self.assertEqual(tuple(initial_map.items()), initial_items)
                    with self.assertRaisesRegex(
                        AuditInfrastructureError,
                        "production snapshot generation",
                    ):
                        held.finalize_policy_boundary(deadline)
        finally:
            self.source.write_bytes(original)
            capability.native_owner.close()
        self.assertEqual(observed_maps, [initial_map])
        self.assertIs(observed_maps[0], initial_map)
        self.assertEqual(
            tuple(event.purpose for event in launches),
            (
                capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY,
                capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED,
            ),
        )
        self.assertEqual(publications, [])
        self.assertEqual(list(cache._results_root.rglob("*")), [])

    def test_real_spawn_cold_miss_has_pid_separation_and_exact_launch_count(self):
        compiler_text = shutil.which("g++")
        if compiler_text is None:
            self.skipTest("requires a production g++ compiler")
        compiler = Path(compiler_text).resolve()
        toolchain_root = compiler.parent.parent
        if sys.platform.startswith("linux"):
            compiler = (self.toolchain / "linux-fake-compiler").resolve()
            launcher_source = self.toolchain / "linux-fake-compiler.cpp"
            fixture = (
                Path(__file__).parent / "fixtures" / "fake_preprocessor.py"
            ).resolve()
            launcher_source.write_text(
                "#include <unistd.h>\n"
                "#include <string>\n"
                "#include <vector>\n"
                "int main(int argc, char** argv) {\n"
                f"  std::vector<std::string> v = {{{json.dumps(str(Path(sys.executable).resolve()))}, {json.dumps(str(fixture))}}};\n"
                "  for (int i = 1; i < argc; ++i) {\n"
                "    if (std::string(argv[i]) == \"-MF\" && i + 1 < argc) {\n"
                "      v.emplace_back(std::string(\"-MF=\") + argv[++i]);\n"
                "    } else { v.emplace_back(argv[i]); }\n"
                "  }\n"
                "  std::vector<char*> p;\n"
                "  for (auto& s : v) p.push_back(s.data());\n"
                "  p.push_back(nullptr);\n"
                "  execv(p[0], p.data());\n"
                "  return 127;\n"
                "}\n",
                encoding="utf-8",
            )
            subprocess.run(
                (compiler_text, str(launcher_source), "-O2", "-o", str(compiler)),
                check=True,
                capture_output=True,
            )
            toolchain_root = self.toolchain
        if os.name == "nt" and len(compiler.parents) > 3:
            toolchain_root = compiler.parents[3]
        roots = {"toolchain": toolchain_root}
        if os.name == "nt":
            roots["windows-system"] = Path(
                os.environ.get("SystemRoot", "C:/Windows")
            ).resolve()
        elif sys.platform.startswith("linux"):
            roots["linux-system"] = Path("/usr").resolve()
        authority = build_dependency_root_authority(self.root, roots)
        deadline = time.monotonic() + 180.0
        helper_probe = (
            mock.patch(
                "gpu_capability_command._driver_selected_helper_paths",
                return_value=(),
            )
            if sys.platform.startswith("linux")
            else contextlib.nullcontext()
        )
        with helper_probe:
            capability = open_compiler_executable_capability(
                compiler,
                authority,
                deadline,
                compiler_family=CompilerFamily.GCC,
                working_directory=self.root,
                preprocess_arguments=(str(self.source),),
            )
        metadata = self.source.stat()
        configuration_arguments = (
            (
                "--fixture-mode=dense",
                "--token-count=16",
                str(self.source),
            )
            if sys.platform.startswith("linux")
            else (str(self.source),)
        )
        source_identity = FileIdentity(
            self.source,
            PurePosixPath("playback/gpu/coordinator.cpp"),
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            1,
            True,
        )
        configuration = PreprocessConfiguration(
            entry_id="coordinator-real:0",
            family=CompilerFamily.GCC,
            compiler=compiler,
            working_directory=self.root,
            source=source_identity,
            arguments=configuration_arguments,
            environment_digest=_environment_digest(dict(os.environ)),
            digest="d" * 64,
            dependency_root_authority_digest=authority.portable_authority_digest,
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )
        configurations = (
            configuration,
            dataclasses.replace(
                configuration,
                entry_id="coordinator-real:1",
                digest="e" * 64,
            ),
            dataclasses.replace(
                configuration,
                entry_id="coordinator-real:2",
                digest="f" * 64,
            ),
        )
        source_digest = DependencyDigest(
            "production",
            source_identity.relative,
            source_identity,
            hashlib.sha256(self.source.read_bytes()).hexdigest(),
        )

        class ColdCache:
            root = (self.root / "cold-cache").resolve()

            def load_many(
                _self, configurations, dependency_roots, loaded_engine,
                production_snapshot, result_budget, aggregator,
                maximum_cold_slot, pipeline_deadline,
            ):
                _self.root.mkdir(parents=True, exist_ok=True)
                aggregator.reserve_cold_slot(maximum_cold_slot)
                return ConfigurationAuditLoadBatch(
                    0, configurations, maximum_cold_slot.worst_case_live_bytes
                )

        class Registry(capability_runner.CompilerCapabilityRegistry):
            def __init__(_self):
                super().__init__(authority)
                _self.duplicates = []
                _self.transfers = []
                _self.releases = []

            def duplicate_for_generation(
                _self, digest, worker_index, generation
            ):
                duplicate = super().duplicate_for_generation(
                    digest, worker_index, generation
                )
                _self.duplicates.append(duplicate)
                return duplicate

            def transfer_duplicate_to_child(_self, duplicate, process):
                self.assertIsInstance(process.pid, int)
                super().transfer_duplicate_to_child(duplicate, process)
                _self.transfers.append(duplicate)

            def release_generation(_self, worker_index, generation):
                _self.releases.append((worker_index, generation))
                super().release_generation(worker_index, generation)

        registry = Registry()
        registry.register(capability)
        if os.name == "nt":
            from gpu_capability_process_tree import WindowsNativeRunAccountant

            run_accountant = WindowsNativeRunAccountant(os.getpid())
        elif sys.platform.startswith("linux"):
            if not {
                "OLR_CGROUP_RENDEZVOUS_PATH",
                "OLR_CGROUP_RENDEZVOUS_TOKEN",
            }.issubset(os.environ):
                self.skipTest("requires Task-6 delegated cgroup supervisor")
            from gpu_capability_process_tree import LinuxRendezvousClient

            run_accountant = LinuxRendezvousClient(os.environ)
        else:
            from gpu_capability_process_tree import MacOSRegisteredPgidAccountant

            run_accountant = MacOSRegisteredPgidAccountant()

        try:
            compact_observer = capability_model.CompactAccountingObserver()
            session = capability_runner.schedule_configuration_audits(
                self.root,
                configurations,
                authority,
                registry,
                {source_identity.relative: source_digest},
                ColdCache(),
                AuditLimits(rss_bytes=2**63 - 1),
                self.engine,
                capability_model.WorkerRuntimeContract(2, 1, 1 << 30, deadline),
                inspection_probe_invocations=0,
                run_accountant=run_accountant,
                compact_observer=compact_observer,
            )
            summary = session.consume_aggregate()
            self.assertEqual(
                summary.configurations,
                tuple(item.digest for item in configurations),
            )
            self.assertEqual(session.cache_misses, 3)
            self.assertEqual(session.audit_compiler_invocations, 6)
            self.assertEqual(session.expected_audit_compiler_invocations, 6)
            self.assertEqual(session.worker_counts_started, (2,))
            self.assertGreater(session.stdout_bytes, 0)
            self.assertGreater(session.maximum_encoded_result_bytes, 0)
            self.assertGreater(session.maximum_conservative_decoded_bytes, 0)
            self.assertGreater(session.maximum_conservative_retained_bytes, 0)
            self.assertGreaterEqual(len(session.worker_pids), 3)
            self.assertGreaterEqual(
                len(session.reactor.archived_generation_telemetry), 3
            )
            self.assertNotIn(os.getpid(), session.worker_pids)
            session.shutdown_reap(deadline)
            summary.release()
            semantic_names = tuple(
                event for event, _delta, _label, _owner
                in compact_observer.semantic_events
            )
            for event in (
                "reserve-dispatch",
                "activate-publication",
                "release-publication",
                "send-pipe",
                "decode",
                "retain-result",
                "release-result",
            ):
                self.assertEqual(semantic_names.count(event), 3)
            self.assertTrue(session.reactor.archived_scratch_roots)
            self.assertTrue(all(
                not path.exists()
                for path in session.reactor.archived_scratch_roots
            ))
            if os.name == "nt":
                self.assertIsInstance(
                    session.reactor.task_phase_snapshot,
                    capability_model.WindowsPhaseSnapshot,
                )
                self.assertEqual(
                    session.reactor.task_phase_snapshot.memory
                    .surviving_job_process_count,
                    0,
                )
            elif sys.platform.startswith("linux"):
                self.assertIsInstance(
                    session.reactor.task_phase_snapshot,
                    capability_model.LinuxPhaseSnapshot,
                )
                self.assertEqual(
                    session.reactor.task_phase_snapshot.memory
                    .surviving_cgroup_process_count,
                    0,
                )
            else:
                self.assertIsInstance(
                    session.reactor.task_phase_snapshot,
                    capability_model.MacOSPhaseSnapshot,
                )
                self.assertTrue(
                    session.reactor.task_phase_snapshot.memory.accounting_complete
                )
            self.assertEqual(registry.transfers, registry.duplicates)
            self.assertEqual(
                set(registry.releases),
                {
                    (duplicate.worker_index, duplicate.generation)
                    for duplicate in registry.duplicates
                },
            )
        finally:
            close_accountant = getattr(run_accountant, "close", None)
            if callable(close_accountant):
                close_accountant()
            capability.native_owner.close()

    def test_task9_native_smoke_uses_real_top_level_and_fails_closed_on_unix(self):
        source = inspect.getsource(
            type(self).test_task9_native_calibration_smoke_one_configuration)
        self.assertIn("run_compiler_audit_smoke_pipeline", source)
        for forbidden in (
            "class ColdCache", "CompilerAuditRun(",
            "CanonicalAuditContentSummary(", "StreamingAuditMeasurements(",
            "skipTest(\"requires the pinned native C++ compiler\")",
            "skipTest(\"requires Task-6 delegated cgroup supervisor\")",
        ):
            self.assertNotIn(forbidden, source)

    def test_task9_unix_attempt_shutdown_seals_tasks_without_replacing_snapshot(self):
        from gpu_capability_process_tree import MacOSCompilerAuditAttemptAccountant

        linux_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20, 0, 0, 0, 0, 0, 0, 0, True)
        mac_memory = capability_model.MacOSRunMemoryMeasurements(
            2, 1, 1, 0, 0, True)

        class LinuxLifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(_self): pass
            def seal_task_phase(_self, count):
                return capability_model.LinuxPhaseSnapshot(
                    "linux", "tasks", linux_memory, count)

        class MacLifecycle(capability_runner._MacOSGenerationLifecycle):
            def __init__(_self): pass
            def seal_task_phase(_self, count):
                return capability_model.MacOSPhaseSnapshot(
                    "macos", "tasks", mac_memory, count)

        class LinuxAttempt(capability_runner.LinuxCompilerAuditAttemptAccountant):
            def __init__(_self): _self.calls = []
            def seal_phase(_self, phase, deadline):
                _self.calls.append((phase, deadline))
                return capability_model.LinuxPhaseSnapshot(
                    "linux", phase, linux_memory, 0)
            def memory_measurements(_self):
                raise AssertionError("sealed memory must not be reread")

        class MacAttempt(MacOSCompilerAuditAttemptAccountant):
            def __init__(_self): _self.calls = []
            def seal_phase(_self, phase, deadline):
                _self.calls.append((phase, deadline))
                return capability_model.MacOSPhaseSnapshot(
                    "macos", phase, mac_memory, 0)
            def memory_measurements(_self):
                raise AssertionError("sealed memory must not be reread")

        for lifecycle, accountant, phase_type in (
            (LinuxLifecycle(), LinuxAttempt(), capability_model.LinuxPhaseSnapshot),
            (MacLifecycle(), MacAttempt(), capability_model.MacOSPhaseSnapshot),
        ):
            with self.subTest(platform=phase_type.__name__):
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor._closed = False
                reactor.states = []
                reactor.cancel_event = threading.Event()
                reactor.archived_generation_telemetry = [(0, 0, 1)]
                reactor.native_lifecycle = lifecycle
                reactor.run_accountant = accountant
                reactor.task_phase_snapshot = None
                reactor._stop_accounting_pump = mock.Mock()
                deadline = time.monotonic() + 10.0
                reactor.shutdown_reap(deadline)
                self.assertEqual(accountant.calls, [("tasks", deadline)])
                self.assertIsInstance(reactor.task_phase_snapshot, phase_type)
                self.assertEqual(
                    reactor.task_phase_snapshot.archived_generation_count, 1)

    def test_linux_task_seal_shares_sticky_memory_when_current_drifts(self):
        first = capability_model.LinuxRunMemoryMeasurements(
            100, 200, 512 << 20, 448 << 20,
            0, 0, 0, 0, 0, 0, 0, True,
        )
        drifted = dataclasses.replace(
            first, cgroup_current_accounted_memory_bytes=101
        )
        lifecycle = object.__new__(capability_runner._LinuxGenerationLifecycle)
        lifecycle.live = {}
        lifecycle.requested = set()
        lifecycle._sealed_task_phase_snapshot = None
        lifecycle.memory_measurements = mock.Mock(return_value=first)

        accountant = object.__new__(
            capability_runner.LinuxCompilerAuditAttemptAccountant
        )
        accountant._phase = "tasks"
        accountant._sealed = {"inspection"}
        accountant._sealed_snapshots = {}
        accountant._active_carriers = {}
        accountant._pending_inspection = None
        accountant.memory_measurements = mock.Mock(return_value=drifted)
        accountant._surviving_pids = mock.Mock(return_value=set())

        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = []
        reactor.cancel_event = threading.Event()
        reactor.archived_generation_telemetry = [(0, 0, 1)]
        reactor.native_lifecycle = lifecycle
        reactor.run_accountant = accountant
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()

        reactor.shutdown_reap(time.monotonic() + 10.0)

        lifecycle.memory_measurements.assert_called_once_with(require_empty=True)
        accountant.memory_measurements.assert_not_called()
        accountant._surviving_pids.assert_called_once_with()
        self.assertIs(reactor.task_phase_snapshot.memory, first)
        self.assertEqual(
            reactor.task_phase_snapshot.archived_generation_count, 1
        )

    def test_task9_unix_attempt_abort_seals_tasks_and_validates_survivors(self):
        from gpu_capability_process_tree import MacOSCompilerAuditAttemptAccountant

        linux_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20, 0, 0, 0, 0, 0, 0, 0, True)
        mac_memory = capability_model.MacOSRunMemoryMeasurements(
            2, 1, 1, 0, 0, True)

        class LinuxAttempt(capability_runner.LinuxCompilerAuditAttemptAccountant):
            def __init__(_self): _self.calls = []
            def seal_phase(_self, phase, deadline):
                _self.calls.append((phase, deadline))
                return capability_model.LinuxPhaseSnapshot(
                    "linux", phase, linux_memory, 0)
            def memory_measurements(_self):
                raise AssertionError("sealed memory must not be reread")

        class MacAttempt(MacOSCompilerAuditAttemptAccountant):
            def __init__(_self): _self.calls = []
            def seal_phase(_self, phase, deadline):
                _self.calls.append((phase, deadline))
                return capability_model.MacOSPhaseSnapshot(
                    "macos", phase, mac_memory, 0)
            def memory_measurements(_self):
                raise AssertionError("sealed memory must not be reread")

        class LinuxLifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(_self): pass
            def seal_task_phase(_self, count):
                return capability_model.LinuxPhaseSnapshot(
                    "linux", "tasks", linux_memory, count)

        class MacLifecycle(capability_runner._MacOSGenerationLifecycle):
            def __init__(_self): pass
            def seal_task_phase(_self, count):
                return capability_model.MacOSPhaseSnapshot(
                    "macos", "tasks", mac_memory, count)

        for accountant, lifecycle in (
            (LinuxAttempt(), LinuxLifecycle()),
            (MacAttempt(), MacLifecycle()),
        ):
            with self.subTest(accountant=type(accountant).__name__):
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor._closed = False
                reactor.states = []
                reactor.cancel_event = threading.Event()
                reactor.publication_requests = __import__("collections").deque()
                reactor.active_publication = None
                reactor.native_lifecycle = lifecycle
                reactor.run_accountant = accountant
                reactor.archived_generation_telemetry = []
                reactor.task_phase_snapshot = None
                reactor._stop_accounting_pump = mock.Mock()
                deadline = time.monotonic() + 10.0
                reactor.abort_and_reap(deadline)
                self.assertEqual(accountant.calls, [("tasks", deadline)])

        surviving_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20, 0, 0, 0, 0, 0, 0, 1, False)

        class SurvivingAttempt(
                capability_runner.LinuxCompilerAuditAttemptAccountant):
            def __init__(_self): pass
            def seal_phase(_self, phase, _deadline):
                return capability_model.LinuxPhaseSnapshot(
                    "linux", phase, surviving_memory, 0)
            def memory_measurements(_self):
                raise AssertionError("sealed memory must not be reread")

        class SurvivingLifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(_self): pass
            def seal_task_phase(_self, count):
                return capability_model.LinuxPhaseSnapshot(
                    "linux", "tasks", surviving_memory, count)

        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = []
        reactor.cancel_event = threading.Event()
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.native_lifecycle = SurvivingLifecycle()
        reactor.run_accountant = SurvivingAttempt()
        reactor.archived_generation_telemetry = []
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "generation cleanup is incomplete"
        ) as raised:
            reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertTrue(any(
            "survivors remain" in note
            for note in getattr(raised.exception, "__notes__", ())))

    @unittest.skipUnless(os.name == "nt", "Windows native accountant proof")
    def test_task9_windows_shutdown_and_abort_retain_phase_snapshots(self):
        from gpu_capability_process_tree import WindowsNativeRunAccountant

        for operation in ("shutdown_reap", "abort_and_reap"):
            with self.subTest(operation=operation):
                accountant = WindowsNativeRunAccountant(os.getpid())
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor._closed = False
                reactor.states = []
                reactor.cancel_event = threading.Event()
                reactor.publication_requests = __import__("collections").deque()
                reactor.active_publication = None
                reactor.native_lifecycle = None
                reactor.run_accountant = accountant
                reactor.archived_generation_telemetry = []
                reactor.task_phase_snapshot = None
                reactor._stop_accounting_pump = mock.Mock()
                try:
                    with mock.patch.object(
                        accountant, "seal_phase", wraps=accountant.seal_phase
                    ) as seal_phase, mock.patch.object(
                        accountant, "snapshot", wraps=accountant.snapshot
                    ) as snapshot:
                        getattr(reactor, operation)(time.monotonic() + 10.0)
                    seal_phase.assert_called_once()
                    snapshot.assert_called_once_with()
                    self.assertIsInstance(
                        reactor.task_phase_snapshot,
                        capability_model.WindowsPhaseSnapshot,
                    )
                    self.assertEqual(reactor.task_phase_snapshot.phase, "tasks")
                    self.assertEqual(
                        reactor.task_phase_snapshot.archived_generation_count, 0
                    )
                    self.assertTrue(
                        reactor.task_phase_snapshot.memory.accounting_complete
                    )
                    self.assertEqual(
                        reactor.task_phase_snapshot.memory
                        .surviving_job_process_count,
                        0,
                    )
                finally:
                    accountant.close()

    @unittest.skipUnless(os.name == "nt", "Windows native accountant proof")
    def test_task9_windows_abort_rejects_surviving_job_processes(self):
        from gpu_capability_process_tree import (
            WindowsJobAccountingSnapshot,
            WindowsNativeRunAccountant,
        )

        class SurvivingWindowsAccountant(WindowsNativeRunAccountant):
            def __init__(_self):
                pass

            def seal_phase(_self, _deadline, _parent_rss=0):
                pass

            def snapshot(_self):
                return WindowsJobAccountingSnapshot(
                    parent_peak_rss_bytes=0,
                    maximum_simultaneous_working_set_bytes=0,
                    inspection_tree_peak_bytes=(),
                    slots=(),
                    job_peak_commit_charge_bytes=0,
                    job_total_process_count=1,
                    retained_process_identity_count=1,
                    retained_inspection_process_identity_count=0,
                    surviving_job_process_count=1,
                )

        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = []
        reactor.cancel_event = threading.Event()
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.native_lifecycle = None
        reactor.run_accountant = SurvivingWindowsAccountant()
        reactor.archived_generation_telemetry = []
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "generation cleanup is incomplete"
        ) as raised:
            reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertTrue(any(
            "Windows tasks phase" in note and "survivors" in note
            for note in getattr(raised.exception, "__notes__", ())
        ))

    def test_task9_shutdown_caps_emergency_cleanup_to_outer_ceiling(self):
        class PrimaryError(RuntimeError):
            pass

        primary = PrimaryError("normal deadline expired")
        survivors = [8123]
        observed_cleanup_deadlines = []
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._shutdown_reap_normal = mock.Mock(side_effect=primary)
        reactor.cleanup_deadline = 160.0

        def abort(cleanup_deadline):
            observed_cleanup_deadlines.append(cleanup_deadline)
            if cleanup_deadline <= time.monotonic():
                raise AuditInfrastructureError("cleanup deadline was expired")
            survivors.clear()
            cleanup_error = AuditInfrastructureError("termination reported late")
            cleanup_error.add_note("worker termination failed: late report")
            raise cleanup_error

        reactor.abort_and_reap_until_closed = abort
        with mock.patch.object(
            capability_runner.time, "monotonic", return_value=159.5
        ), self.assertRaises(PrimaryError) as raised:
            reactor.shutdown_reap(130.0)
        self.assertIs(raised.exception, primary)
        self.assertEqual(survivors, [])
        self.assertEqual(observed_cleanup_deadlines, [160.0])
        notes = " ".join(getattr(primary, "__notes__", ()))
        self.assertIn("termination reported late", notes)
        self.assertIn("worker termination failed", notes)

    def test_task9_near_expiry_failure_cleanup_matrix_shares_outer_ceiling(self):
        class PrimaryError(RuntimeError):
            pass

        clock = [169.5]
        operation_deadline = 170.0
        cleanup_deadline = 200.0
        runtime = capability_model.WorkerRuntimeContract(
            1, 1, 1 << 30, operation_deadline
        )
        observed = {}

        def fail(label):
            clock[0] = 199.5
            raise PrimaryError(f"{label} failed")

        def record(label, reactor):
            def abort(deadline):
                observed.setdefault(label, []).append(deadline)
                reactor._closed = True
                reactor.states = [None for _state in reactor.states]
                return ()

            return abort

        base = dict(
            configurations=(self.configuration,),
            dependency_roots=self.authority,
            production_snapshot={},
            cache_root=self.root / "shared-deadline-startup",
            limits=AuditLimits(),
            engine=self.engine,
            runtime_contract=runtime,
            run_accountant=SimpleNamespace(),
            capability_registry=SimpleNamespace(),
            result_budget=CompactResultMemoryBudget(),
            worker_count=1,
            cleanup_deadline=cleanup_deadline,
        )

        with mock.patch.object(
            capability_runner.time, "monotonic", side_effect=lambda: clock[0]
        ):
            with self.subTest(path="startup"), mock.patch.object(
                capability_runner.GenerationReactor,
                "spawn_next_generation",
                side_effect=lambda _index: fail("startup"),
            ), mock.patch.object(
                capability_runner.GenerationReactor,
                "abort_and_reap_until_closed",
                autospec=True,
                side_effect=lambda reactor, deadline: record(
                    "startup", reactor
                )(deadline),
            ), self.assertRaisesRegex(PrimaryError, "startup failed"):
                capability_runner.GenerationReactor(**base)

            def bare_reactor(label):
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor.states = [None]
                reactor.next_generations = [0]
                reactor.runtime_contract = runtime
                reactor.cleanup_deadline = cleanup_deadline
                reactor.cancel_event = threading.Event()
                reactor.native_lifecycle = None
                reactor.run_accountant = SimpleNamespace()
                reactor.capability_registry = SimpleNamespace(
                    duplicate_for_generation=lambda *_args: None,
                    transfer_duplicate_to_child=lambda *_args: None,
                    acknowledge_generation=lambda *_args: None,
                    release_generation=lambda *_args: None,
                )
                reactor._accounting_lock = threading.Lock()
                reactor.registry_generation_duplicates = {}
                reactor.worker_pids = []
                reactor.archived_generation_telemetry = []
                reactor.archived_scratch_roots = []
                reactor.publication_requests = __import__("collections").deque()
                reactor.active_publication = None
                reactor.deferred_events = __import__("collections").deque()
                reactor.task_phase_snapshot = None
                reactor._stop_accounting_pump = mock.Mock()
                reactor._closed = False
                reactor.configurations = ()
                reactor.dependency_roots = self.authority
                reactor.production_snapshot = {}
                reactor.cache_root = self.root / f"shared-deadline-{label}"
                reactor.limits = AuditLimits()
                reactor.engine = self.engine
                reactor.abort_and_reap_until_closed = record(label, reactor)
                return reactor

            setup = bare_reactor("setup")
            clock[0] = 169.5
            with self.subTest(path="setup"), mock.patch.object(
                capability_runner.BoundedFrameChannel,
                "create",
                side_effect=lambda *_args: fail("setup"),
            ), self.assertRaisesRegex(PrimaryError, "setup failed"):
                setup.spawn_next_generation(0)

            spawn = bare_reactor("spawn")
            clock[0] = 169.5
            process = mock.Mock(pid=None, sentinel=None)
            spawn.context = SimpleNamespace(Process=lambda **_kwargs: process)
            frame_pairs = tuple((mock.Mock(), mock.Mock()) for _ in range(4))
            payload_pair = (mock.Mock(), mock.Mock())

            def transition(_state, label, action):
                if label == "process-start":
                    fail("spawn")
                return action()

            spawn._complete_generation_setup_transition = transition
            with self.subTest(path="spawn"), mock.patch.object(
                capability_runner.BoundedFrameChannel,
                "create",
                side_effect=frame_pairs,
            ), mock.patch.object(
                capability_runner.BoundedPayloadChannel,
                "create",
                return_value=payload_pair,
            ), mock.patch.object(
                capability_runner,
                "_worker_scratch_parent",
                return_value=self.root,
            ), self.assertRaisesRegex(PrimaryError, "spawn failed"):
                spawn.spawn_next_generation(0)

            class ColdCache:
                root = self.root / "shared-deadline-scheduler"

                def load_many(_self, *_args):
                    return ConfigurationAuditLoadBatch(
                        0, (self.configuration,), 16 << 20
                    )

            class SchedulingReactor:
                def __init__(_self, **kwargs):
                    self.assertEqual(kwargs["cleanup_deadline"], cleanup_deadline)
                    _self.states = [object()]
                    _self.worker_pids = []
                    _self.has_pending_tasks = False
                    _self.has_expected_retirement = False
                    _self.audit_launch_count = 0

                def idle_states(_self):
                    fail("scheduling")

                def abort_and_reap_until_closed(_self, deadline):
                    observed.setdefault("scheduling", []).append(deadline)
                    _self.closed = True
                    return ()

            clock[0] = 169.5
            with self.subTest(path="scheduling"), mock.patch(
                "gpu_capability_source_audit._attest_loaded_audit_engine",
                return_value=self.engine,
            ), mock.patch.object(
                capability_runner,
                "GenerationReactor",
                SchedulingReactor,
            ), self.assertRaisesRegex(PrimaryError, "scheduling failed"):
                capability_runner.schedule_configuration_audits(
                    self.root,
                    (self.configuration,),
                    self.authority,
                    SimpleNamespace(),
                    {},
                    ColdCache(),
                    AuditLimits(),
                    self.engine,
                    runtime,
                    inspection_probe_invocations=0,
                    run_accountant=SimpleNamespace(),
                    compact_observer=capability_model.CompactAccountingObserver(),
                    cleanup_deadline=cleanup_deadline,
                )

        for label in ("startup", "setup", "spawn", "scheduling"):
            with self.subTest(cleanup_path=label):
                self.assertEqual(len(observed[label]), 1)
                self.assertGreaterEqual(
                    observed[label][0], operation_deadline
                )
                self.assertLessEqual(observed[label][0], cleanup_deadline)

    def test_task9_shutdown_preserves_primary_and_attaches_abort_cleanup(self):
        class PrimaryError(RuntimeError): pass
        class CleanupError(RuntimeError): pass

        primary = PrimaryError("normal shutdown failed")
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._shutdown_reap_normal = mock.Mock(side_effect=primary)
        reactor.abort_and_reap_until_closed = mock.Mock(
            side_effect=CleanupError("abort cleanup failed"))
        deadline = time.monotonic() + 10.0
        with self.assertRaises(PrimaryError) as raised:
            reactor.shutdown_reap(deadline)
        self.assertIs(raised.exception, primary)
        self.assertTrue(any(
            "abort cleanup failed" in note
            for note in getattr(primary, "__notes__", ())))

    def test_task9_native_calibration_smoke_one_configuration(self):
        if os.name == "nt":
            self.skipTest("Task-9 native calibration smoke is Linux/macOS only")
        compiler_text = shutil.which(
            "g++" if sys.platform.startswith("linux") else "clang++")
        if compiler_text is None:
            self.fail("pinned native C++ compiler is unavailable")
        if sys.platform.startswith("linux") and not {
            "OLR_CGROUP_RENDEZVOUS_PATH",
            "OLR_CGROUP_RENDEZVOUS_TOKEN",
        }.issubset(os.environ):
            self.fail("Task-6 delegated cgroup rendezvous is unavailable")
        compiler = Path(compiler_text).resolve()
        toolchain_root = compiler.parent.parent
        roots = {"toolchain": toolchain_root}
        runtime_candidates = (
            (("system-lib", Path("/lib")), ("system-lib64", Path("/lib64")))
            if sys.platform.startswith("linux")
            else (("system-frameworks", Path("/System")),)
        )
        for role, candidate in runtime_candidates:
            try:
                resolved = candidate.resolve(strict=True)
            except OSError:
                continue
            if resolved.is_dir() and not any(
                resolved == existing
                or resolved.is_relative_to(existing)
                or existing.is_relative_to(resolved)
                for existing in roots.values()
            ):
                roots[role] = resolved
        authority = build_dependency_root_authority(self.root, roots)
        attempts = []
        database = self.root / "compile_commands.json"
        entry = [{
            "directory": str(self.root),
            "file": str(self.source),
            "arguments": [
                str(compiler), "-std=c++20", "-c", str(self.source),
            ],
        }]
        database.write_text(json.dumps(entry), encoding="utf-8")

        linux_rendezvous_client = None
        attempt_rendezvous_clients = []
        if sys.platform.startswith("linux"):
            from gpu_capability_process_tree import LinuxRendezvousClient

            linux_rendezvous_client = LinuxRendezvousClient(os.environ)

        def full_pipeline_attempt(*, kind, worker_count, runtime_parameters,
                                  cache_root, included_stages, preparation,
                                  operation_deadline, cleanup_deadline):
            self.assertEqual(worker_count, 1)
            self.assertEqual(preparation.configuration_count, 1)
            self.assertEqual(included_stages,
                             capability_calibration.CALIBRATION_INCLUDED_STAGES)
            _clear_compiler_inspection_memo_for_tests()
            attempt_database = cache_root.with_name(
                cache_root.name + ".compile_commands.json")
            attempt_database.parent.mkdir(parents=True, exist_ok=True)
            attempt_database.write_text(json.dumps(entry), encoding="utf-8")
            result_cache = ConfigurationAuditCache(cache_root)
            result_cache.prepare(operation_deadline)
            inspection_cache = CompilerInspectionCache(cache_root)
            registry = capability_runner.CompilerCapabilityRegistry(authority)
            if sys.platform.startswith("linux"):
                native = linux_rendezvous_client
                self.assertIsNotNone(native)
                attempt_rendezvous_clients.append(native)
                native.hello(operation_deadline)
                run_accountant = (
                    capability_runner.LinuxCompilerAuditAttemptAccountant(native))
                recycle_rss_bytes = (
                    runtime_parameters.recycle_rss_bytes
                    if runtime_parameters is not None else 1 << 30)
            else:
                from gpu_capability_process_tree import (
                    MacOSCompilerAuditAttemptAccountant,
                )

                run_accountant = MacOSCompilerAuditAttemptAccountant()
                recycle_rss_bytes = (
                    capability_calibration.MACOS_RSS_RECYCLE_DISABLED)
            runtime = capability_model.WorkerRuntimeContract(
                1, 1, recycle_rss_bytes, operation_deadline)
            primary_error = None
            try:
                audit_run = capability_runner.run_compiler_audit_smoke_pipeline(
                    self.root,
                    (attempt_database,),
                    dict(os.environ),
                    authority,
                    registry,
                    result_cache,
                    inspection_cache,
                    AuditLimits(workers=1, rss_bytes=2**63 - 1),
                    runtime,
                    operation_deadline,
                    cleanup_deadline,
                    run_accountant,
                    self.engine,
                )
                measurements = audit_run.measurements
                self.assertEqual(measurements.configuration_count, 1)
                self.assertEqual(measurements.cache_misses, 1)
                self.assertEqual(measurements.audit_compiler_invocations, 2)
                self.assertEqual(measurements.worker_counts_started, (1,))
                self.assertEqual(
                    measurements.included_stages,
                    capability_calibration.CALIBRATION_SMOKE_PIPELINE_STAGES,
                )
                memory = measurements.memory
                self.assertTrue(memory.accounting_complete)
                survivors = (
                    memory.surviving_cgroup_process_count
                    if sys.platform.startswith("linux")
                    else memory.surviving_registered_process_count)
                self.assertEqual(survivors, 0)
                if not sys.platform.startswith("linux"):
                    self.assertEqual(
                        memory.known_unreconciled_descendant_count, 0)
                phase_type = (capability_model.LinuxPhaseSnapshot
                              if sys.platform.startswith("linux")
                              else capability_model.MacOSPhaseSnapshot)
                self.assertIsInstance(
                    measurements.inspection_phase_snapshot, phase_type)
                self.assertIsInstance(
                    measurements.task_phase_snapshot, phase_type)
                attempts.append((kind, cache_root, memory))
                return audit_run
            except BaseException as error:
                primary_error = error
                raise
            finally:
                cleanup_errors = []
                for label, close in (
                    ("attempt accountant", getattr(run_accountant, "close", None)),
                    ("registry", registry.close),
                ):
                    if not callable(close):
                        continue
                    try:
                        close()
                    except BaseException as cleanup_error:
                        cleanup_errors.append((label, cleanup_error))
                if primary_error is not None:
                    for label, cleanup_error in cleanup_errors:
                        primary_error.add_note(
                            f"{label} cleanup also failed: {cleanup_error!r}"
                        )
                elif cleanup_errors:
                    label, cleanup_error = cleanup_errors[0]
                    for extra_label, extra_error in cleanup_errors[1:]:
                        cleanup_error.add_note(
                            f"{extra_label} cleanup also failed: {extra_error!r}"
                        )
                    raise cleanup_error

        runner = capability_calibration.RealCalibrationRunner(
            "linux" if sys.platform.startswith("linux") else "macos",
            full_pipeline_attempt=full_pipeline_attempt)
        try:
            cache_parent = (self.root / "task9-smoke-runs").resolve()
            readiness = capability_calibration.run_real_calibration_smoke(
                runner,
                self.root,
                (self.root / "compile_commands.json",),
                cache_parent,
                AuditLimits(workers=1, rss_bytes=2**63 - 1),
            )
            self.assertEqual(readiness.attempted_worker_count, 1)
            self.assertEqual(
                tuple(kind for kind, _cache, _memory in attempts),
                ("pilot", "measured"))
            self.assertEqual(len({cache for _kind, cache, _memory in attempts}),
                             2)
            self.assertTrue(all(memory.accounting_complete
                                for _kind, _cache, memory in attempts))
            if sys.platform.startswith("linux"):
                self.assertEqual(len(attempt_rendezvous_clients), 2)
                self.assertIs(attempt_rendezvous_clients[0],
                              attempt_rendezvous_clients[1])
        finally:
            _clear_compiler_inspection_memo_for_tests()

    def test_real_cpu_matrix_runs_one_two_and_four_workers_simultaneously(self):
        if os.name != "nt":
            self.skipTest("locked reference CPU matrix runs on Windows")
        gxx = shutil.which("g++")
        if gxx is None:
            self.skipTest("requires g++ to build the fake compiler launcher")
        compiler = (self.toolchain / "matrix-fake-compiler.exe").resolve()
        launcher_source = self.toolchain / "matrix-fake-compiler.cpp"
        launcher_source.write_text(
            "#include <process.h>\n"
            "#include <string>\n"
            "#include <vector>\n"
            "int main(int argc, char** argv) {\n"
            f"  std::vector<std::string> v = {{{json.dumps(str(Path(sys.executable).resolve()))}, \"-m\", \"tests.gpu.fixtures.fake_preprocessor\"}};\n"
            "  for (int i = 1; i < argc; ++i) v.emplace_back(argv[i]);\n"
            "  std::vector<const char*> p;\n"
            "  for (const auto& s : v) p.push_back(s.c_str());\n"
            "  p.push_back(nullptr);\n"
            "  return static_cast<int>(_spawnv(_P_WAIT, p[0], p.data()));\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.run(
            (gxx, str(launcher_source), "-O2", "-o", str(compiler)),
            check=True,
            capture_output=True,
        )
        roots = {
            "toolchain": self.toolchain,
            "mingw-runtime": Path(gxx).resolve().parent,
            "windows-system": Path(
                os.environ.get("SystemRoot", "C:/Windows")
            ).resolve(),
        }
        selected_runtime_root = next((
            Path(entry).resolve()
            for entry in os.environ.get("PATH", "").split(os.pathsep)
            if entry and (Path(entry) / "libgcc_s_seh-1.dll").is_file()
        ), None)
        if (
            selected_runtime_root is not None
            and selected_runtime_root != Path(gxx).resolve().parent
        ):
            roots["selected-mingw-runtime"] = selected_runtime_root
        authority = build_dependency_root_authority(self.root, roots)
        deadline = time.monotonic() + 240.0
        capability = open_compiler_executable_capability(
            compiler,
            authority,
            deadline,
            compiler_family=CompilerFamily.GCC,
        )
        metadata = self.source.stat()
        source_identity = FileIdentity(
            self.source,
            PurePosixPath("playback/gpu/coordinator.cpp"),
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            1,
            True,
        )
        source_digest = DependencyDigest(
            "production",
            source_identity.relative,
            source_identity,
            hashlib.sha256(self.source.read_bytes()).hexdigest(),
        )
        repository_root = Path(__file__).resolve().parents[2]

        try:
            for worker_count in (1, 2, 4):
                with self.subTest(worker_count=worker_count):
                    ready = self.root / f"matrix-{worker_count}-ready"
                    release = self.root / f"matrix-{worker_count}.release"
                    previous_ready = os.environ.get("OLR_CPU_READY_DIRECTORY")
                    previous_release = os.environ.get("OLR_CPU_RELEASE_FILE")
                    os.environ["OLR_CPU_READY_DIRECTORY"] = str(ready)
                    os.environ["OLR_CPU_RELEASE_FILE"] = str(release)
                    arguments = (
                        "--fixture-mode=dense",
                        "--cpu-burn-seconds=0.02",
                        "--token-count=16",
                        str(self.source),
                    )
                    configurations = tuple(
                        PreprocessConfiguration(
                            entry_id=f"matrix-{worker_count}:{index}",
                            family=CompilerFamily.GCC,
                            compiler=compiler,
                            working_directory=repository_root,
                            source=source_identity,
                            arguments=arguments,
                            environment_digest=_environment_digest(
                                dict(os.environ)
                            ),
                            digest=hashlib.sha256(
                                f"matrix:{worker_count}:{index}".encode("ascii")
                            ).hexdigest(),
                            dependency_root_authority_digest=(
                                authority.portable_authority_digest
                            ),
                            compiler_capability_digest=(
                                capability.capability_digest
                            ),
                            compiler_capability=capability,
                        )
                        for index in range(worker_count)
                    )

                    class ColdCache:
                        root = (
                            self.root / f"matrix-{worker_count}-cache"
                        ).resolve()

                        def load_many(
                            _self, configurations, dependency_roots,
                            loaded_engine, production_snapshot, result_budget,
                            aggregator, maximum_cold_slot, pipeline_deadline,
                        ):
                            _self.root.mkdir(parents=True, exist_ok=True)
                            aggregator.reserve_cold_slot(maximum_cold_slot)
                            return ConfigurationAuditLoadBatch(
                                0,
                                configurations,
                                maximum_cold_slot.worst_case_live_bytes,
                            )

                    from gpu_capability_process_tree import (
                        WindowsNativeRunAccountant,
                    )

                    accountant = WindowsNativeRunAccountant(os.getpid())
                    registry = capability_runner.CompilerCapabilityRegistry(
                        authority
                    )
                    registry.register(capability)
                    result = []
                    errors = []

                    def run_matrix():
                        try:
                            result.append(
                                capability_runner.schedule_configuration_audits(
                                    self.root,
                                    configurations,
                                    authority,
                                    registry,
                                    {source_identity.relative: source_digest},
                                    ColdCache(),
                                    AuditLimits(rss_bytes=2**63 - 1),
                                    self.engine,
                                    capability_model.WorkerRuntimeContract(
                                        worker_count,
                                        16,
                                        1 << 30,
                                        deadline,
                                    ),
                                    inspection_probe_invocations=0,
                                    run_accountant=accountant,
                                    compact_observer=(
                                        capability_model.CompactAccountingObserver()
                                    ),
                                )
                            )
                        except BaseException as error:
                            errors.append(error)

                    thread = threading.Thread(target=run_matrix)
                    thread.start()
                    observation_deadline = time.monotonic() + 60.0
                    while (
                        len(tuple(ready.glob("*.ready"))) < worker_count
                        and thread.is_alive()
                        and not errors
                        and time.monotonic() < observation_deadline
                    ):
                        time.sleep(0.01)
                    observed = tuple(ready.glob("*.ready"))
                    release.write_text("release", encoding="ascii")
                    thread.join(timeout=max(1.0, deadline - time.monotonic()))
                    try:
                        self.assertFalse(thread.is_alive())
                        if errors:
                            raise errors[0]
                        self.assertGreaterEqual(len(observed), worker_count)
                        session = result[0]
                        summary = session.consume_aggregate()
                        self.assertEqual(
                            summary.configurations,
                            tuple(item.digest for item in configurations),
                        )
                        self.assertEqual(
                            session.audit_compiler_invocations,
                            2 * worker_count,
                        )
                        session.shutdown_reap(deadline)
                        summary.release()
                    finally:
                        close_accountant = getattr(accountant, "close", None)
                        if callable(close_accountant):
                            close_accountant()
                        if previous_ready is None:
                            os.environ.pop("OLR_CPU_READY_DIRECTORY", None)
                        else:
                            os.environ["OLR_CPU_READY_DIRECTORY"] = previous_ready
                        if previous_release is None:
                            os.environ.pop("OLR_CPU_RELEASE_FILE", None)
                        else:
                            os.environ["OLR_CPU_RELEASE_FILE"] = previous_release
        finally:
            capability.native_owner.close()

    def test_startup_lifecycle_control_matrix_is_canonical_and_bounded(self):
        lifecycle = (
            capability_model.WorkerContained(3, 9, 123, "job:123:9"),
            capability_model.MacOSWorkerSessionReported(
                3, 9, 123, 123, "1:2"
            ),
            capability_model.CompilerPgidReported(
                3,
                9,
                5,
                capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY,
                124,
                124,
                "3:4",
                self.capability.executable_identity,
                self.capability.executable_sha256,
                self.capability.capability_digest,
            ),
            capability_model.CompilerExecPermit(3, 9, 5, 124),
            capability_model.WorkerCapabilitiesAccepted(
                3, 9, (self.capability.capability_digest,)
            ),
            capability_model.WorkerEngineReady(
                3, 9, 123, self.engine, (self.capability.capability_digest,)
            ),
            capability_model.WorkerRetire(3, 9, "task-limit"),
            capability_model.WorkerRetireAck(3, 9),
            capability_model.WorkerStop(3, 9),
            capability_model.WorkerStopped(3, 9),
            capability_model.WorkerFailure(3, 9, "task-0", "worker failed"),
        )
        for message in lifecycle:
            with self.subTest(message=type(message).__name__):
                encoded = capability_runner.encode_control_message(message)
                self.assertLessEqual(
                    len(encoded), capability_runner.CONTROL_MAX_BYTES
                )
                decoded = capability_runner.decode_control_message(encoded)
                self.assertEqual(decoded, message)
                self.assertEqual(
                    capability_runner.encode_control_message(decoded), encoded
                )
        corruptions = (
            b"\x80\x04N.",
            b'{"generation":true,"tag":"worker-stop","worker_index":3}',
            b'{"generation":9,"tag":"unknown","worker_index":3}',
            b"x" * (capability_runner.CONTROL_MAX_BYTES + 1),
        )
        for payload in corruptions:
            with self.subTest(payload=payload[:16]), self.assertRaises(
                AuditInfrastructureError
            ):
                capability_runner.decode_control_message(payload)

    def test_worker_payload_ready_codec_carries_allocation_declarations(self):
        message = capability_model.WorkerPayloadReady(
            worker_index=3,
            generation=9,
            task_id="task-0",
            configuration_digest="a" * 64,
            audit_engine_fingerprint="b" * 64,
            pipe_nonce="c" * 64,
            serial=11,
            nonce="e" * 64,
            encoded_bytes=4096,
            encoded_sha256="d" * 64,
            charged_bytes=8192,
            conservative_decoded_bytes=48 << 10,
            conservative_retained_bytes=16 << 10,
            counting_pass_peak_bytes=12 << 10,
            stdout_bytes=17,
            stages=capability_model.WorkerStageTimings(1.0, 2.0, 3.0, 4.0),
        )
        encoded = capability_runner.encode_control_message(message)
        self.assertEqual(capability_runner.decode_control_message(encoded), message)
        document = json.loads(encoded)
        self.assertEqual(
            {
                "conservative_decoded_bytes",
                "conservative_retained_bytes",
                "counting_pass_peak_bytes",
            },
            set(document)
            & {
                "conservative_decoded_bytes",
                "conservative_retained_bytes",
                "counting_pass_peak_bytes",
            },
        )
        document["conservative_decoded_bytes"] = True
        corrupted = json.dumps(
            document, ensure_ascii=True, sort_keys=True, separators=(",", ":")
        ).encode("ascii")
        with self.assertRaises(AuditInfrastructureError):
            capability_runner.decode_control_message(corrupted)

    def test_worker_capability_bootstrap_uses_transferred_streams_without_open(self):
        deadline = time.monotonic() + 30.0
        payload = capability_runner._encode_worker_bootstrap(
            (self.configuration,),
            self.authority,
            {},
            AuditLimits(),
            self.engine,
            deadline,
            transfer_cookie="a" * 64,
            transferred_handles={},
        )
        decoded = capability_runner._decode_worker_bootstrap(payload)
        document = decoded[1][0]
        streams = tuple(
            os.fdopen(os.dup(stream.fileno()), "rb", closefd=True)
            for stream in self.capability.native_owner.streams
        )
        transferred = None
        try:
            with mock.patch.object(
                Path,
                "open",
                side_effect=AssertionError("worker reopened capability path"),
            ):
                transferred = capability_runner._compiler_capability_from_bootstrap(
                    document,
                    self.authority,
                    deadline,
                    threading.Event(),
                    streams,
                )
            self.assertEqual(
                transferred.capability_digest,
                self.capability.capability_digest,
            )
        finally:
            if transferred is not None:
                transferred.native_owner.close()
            else:
                for stream in streams:
                    stream.close()

    def test_worker_capability_bootstrap_preserves_shared_cache_generation(self):
        expected_uuid = "11" * 16
        owner = self.capability.native_owner
        owner.macos_shared_cache_uuid = expected_uuid
        closure_digest = capability_command._runtime_closure_digest(
            self.capability.resolved_runtime_closure,
            "macos",
            expected_uuid,
        )
        capability_digest = capability_command._compiler_capability_digest(
            "macos",
            self.capability.trusted_toolchain_root,
            self.capability.executable_identity.canonical,
            self.capability.executable_sha256,
            closure_digest,
            self.authority.portable_authority_digest,
        )
        macos_capability = dataclasses.replace(
            self.capability,
            platform_kind="macos",
            resolved_runtime_closure_digest=closure_digest,
            capability_digest=capability_digest,
        )
        configuration = dataclasses.replace(
            self.configuration,
            compiler_capability=macos_capability,
            compiler_capability_digest=capability_digest,
        )
        streams = tuple(
            os.fdopen(os.dup(stream.fileno()), "rb", closefd=True)
            for stream in owner.streams
        )
        transferred = None
        try:
            payload = capability_runner._encode_worker_bootstrap(
                (configuration,), self.authority, {}, AuditLimits(),
                self.engine, time.monotonic() + 30.0,
                transfer_cookie="a" * 64, transferred_handles={},
            )
            decoded = capability_runner._decode_worker_bootstrap(payload)
            document = decoded[1][0]
            self.assertEqual(
                document["owner"]["macos_shared_cache_uuid"], expected_uuid
            )
            with mock.patch(
                "gpu_capability_command._macos_shared_cache_uuid",
                return_value=expected_uuid,
            ):
                transferred = capability_runner._compiler_capability_from_bootstrap(
                    document, self.authority, time.monotonic() + 30.0,
                    threading.Event(), streams,
                )
            self.assertEqual(
                transferred.native_owner.macos_shared_cache_uuid, expected_uuid
            )
        finally:
            owner.macos_shared_cache_uuid = ""
            if transferred is not None:
                transferred.native_owner.close()
            else:
                for stream in streams:
                    stream.close()

    def test_worker_capability_bootstrap_rejects_rebound_cache_generation_with_stale_digest(self):
        initial_uuid = "11" * 16
        rebound_uuid = "22" * 16
        owner = self.capability.native_owner
        owner.macos_shared_cache_uuid = initial_uuid
        initial_closure_digest = capability_command._runtime_closure_digest(
            self.capability.resolved_runtime_closure, "macos", initial_uuid
        )
        initial_capability_digest = capability_command._compiler_capability_digest(
            "macos",
            self.capability.trusted_toolchain_root,
            self.capability.executable_identity.canonical,
            self.capability.executable_sha256,
            initial_closure_digest,
            self.authority.portable_authority_digest,
        )
        macos_capability = dataclasses.replace(
            self.capability,
            platform_kind="macos",
            resolved_runtime_closure_digest=initial_closure_digest,
            capability_digest=initial_capability_digest,
        )
        configuration = dataclasses.replace(
            self.configuration,
            compiler_capability=macos_capability,
            compiler_capability_digest=initial_capability_digest,
        )
        streams = tuple(
            os.fdopen(os.dup(stream.fileno()), "rb", closefd=True)
            for stream in owner.streams
        )
        try:
            payload = capability_runner._encode_worker_bootstrap(
                (configuration,), self.authority, {}, AuditLimits(),
                self.engine, time.monotonic() + 30.0,
                transfer_cookie="a" * 64, transferred_handles={},
            )
            document = capability_runner._decode_worker_bootstrap(payload)[1][0]
            document["owner"]["macos_shared_cache_uuid"] = rebound_uuid
            document["resolved_runtime_closure_digest"] = (
                capability_command._runtime_closure_digest(
                    self.capability.resolved_runtime_closure,
                    "macos",
                    rebound_uuid,
                )
            )
            with mock.patch(
                "gpu_capability_command._macos_shared_cache_uuid",
                return_value=rebound_uuid,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "capability digest differs"
            ):
                capability_runner._compiler_capability_from_bootstrap(
                    document, self.authority, time.monotonic() + 30.0,
                    threading.Event(), streams,
                )
        finally:
            owner.macos_shared_cache_uuid = ""
            for stream in streams:
                if not stream.closed:
                    stream.close()

    def test_task_frame_binds_only_receiver_held_authority_and_capability(self):
        reservation = capability_model.PerTaskCompactReservation(
            "task-0", 4, 56 << 20
        )
        capability = reservation.issue_worker_transport_capability(
            "task-0", 4, self.configuration.digest, self.engine, 1
        )
        task = capability_model.ConfigurationAuditTask(
            "task-0", 4, self.configuration, self.authority,
            capability_model.PerTaskCompactReservation.for_worker_transport(
                capability
            ),
        )
        encoded = capability_runner.encode_task_frame(17, 1, task, capability)
        self.assertLessEqual(len(encoded), capability_runner.TASK_MAX_BYTES)
        document = json.loads(encoded)
        self.assertEqual(
            document["dependency_root_authority_digest"],
            self.authority.portable_authority_digest,
        )
        self.assertNotIn("dependency_root_authority", document)
        decoded_ordinal, decoded, decoded_capability = (
            capability_runner.decode_task_frame(
                encoded,
                self.authority,
                {self.capability.capability_digest: self.capability},
            )
        )
        self.assertEqual(decoded_ordinal, 17)
        self.assertIs(decoded.dependency_root_authority, self.authority)
        self.assertIs(decoded.configuration.compiler_capability, self.capability)
        self.assertEqual(decoded_capability, capability)
        decoded.compact_reservation.release("task-codec-test")
        reservation.release_worker_transport_capability(
            capability, "task-codec-test"
        )

        changed_root = self.root / "other-source-root"
        changed_root.mkdir()
        changed_metadata = changed_root.stat()
        changed_binding = dataclasses.replace(
            self.authority.source_root,
            resolved_root=changed_root,
            root_identity=dataclasses.replace(
                self.authority.source_root.root_identity,
                canonical=changed_root,
                device=int(changed_metadata.st_dev),
                inode=(
                    int(changed_metadata.st_ino)
                    if int(changed_metadata.st_ino) else None
                ),
            ),
        )
        changed_authority = dataclasses.replace(
            self.authority, source_root=changed_binding
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "authority"):
            capability_runner.decode_task_frame(
                encoded,
                changed_authority,
                {self.capability.capability_digest: self.capability},
            )

    def test_bounded_frame_channel_honors_deadline_cancel_and_peer_close(self):
        receiver, sender = capability_runner.BoundedFrameChannel.create(1024)
        try:
            sender.send_bytes_before(b"ready", time.monotonic() + 2.0)
            self.assertEqual(
                receiver.receive_bytes_before(time.monotonic() + 2.0), b"ready"
            )
            cancelled = threading.Event()
            cancelled.set()
            with self.assertRaisesRegex(AuditInfrastructureError, "cancel"):
                receiver.receive_bytes_before(
                    time.monotonic() + 2.0, cancel_event=cancelled
                )
            sender.close()
            with self.assertRaisesRegex(AuditInfrastructureError, "closed|truncated"):
                receiver.receive_bytes_before(time.monotonic() + 2.0)
        finally:
            receiver.close()
            sender.close()

        blocked_receiver, blocked_sender = (
            capability_runner.BoundedFrameChannel.create(
                capability_runner.TASK_MAX_BYTES
            )
        )
        failures = []

        def fill_without_reader():
            try:
                blocked_sender.send_bytes_before(
                    b"x" * capability_runner.TASK_MAX_BYTES,
                    time.monotonic() + 0.1,
                )
            except BaseException as error:
                failures.append(error)

        thread = threading.Thread(target=fill_without_reader)
        thread.start()
        thread.join(timeout=0.5)
        if thread.is_alive():
            blocked_receiver.close()
            thread.join(timeout=2.0)
        blocked_sender.close()
        blocked_receiver.close()
        self.assertFalse(thread.is_alive(), "bounded write ignored its deadline")
        self.assertEqual(len(failures), 1)
        self.assertRegex(str(failures[0]), "deadline")

    def test_bounded_payload_channel_authenticates_with_deadline_aware_send(self):
        reservation = capability_model.PerTaskCompactReservation(
            "payload-task", 2, 64 << 20
        )
        capability = reservation.issue_worker_transport_capability(
            "payload-task",
            2,
            self.configuration.digest,
            self.engine,
            1,
        )
        stages = capability_model.WorkerStageTimings(1.0, 2.0, 3.0, 4.0)
        payload = b'{"bounded":true}'
        receipt = capability_model.CompactResultTransportReceipt(
            capability.task_id,
            capability.generation,
            capability.configuration_digest,
            capability.audit_engine_fingerprint,
            capability.worker_slot,
            capability.pipe_nonce,
            capability.serial,
            capability.nonce,
            len(payload),
            4096,
            hashlib.sha256(payload).hexdigest(),
            7,
            stages,
        )
        outcome = capability_model.ConfigurationAuditTransportOutcome(
            capability_model.ConfigurationAuditResultTransport(payload, receipt),
            7,
            stages,
        )
        receiver, sender = capability_runner.BoundedPayloadChannel.create()
        try:
            sender.send_transport_before(outcome, time.monotonic() + 2.0)
            self.assertEqual(
                receiver.receive_transport_before(
                    capability, time.monotonic() + 2.0
                ),
                outcome,
            )
            cancelled = threading.Event()
            cancelled.set()
            with self.assertRaisesRegex(AuditInfrastructureError, "cancel"):
                sender.send_transport_before(
                    outcome,
                    time.monotonic() + 2.0,
                    cancel_event=cancelled,
                )
        finally:
            receiver.close()
            sender.close()
            reservation.release_worker_transport_capability(
                capability, "payload-channel-test"
            )

    def test_transport_allocation_layout_counts_each_live_copy_once(self):
        bounds = capability_model.CompactResultPreparseBounds(
            4 << 20, 16 << 20, 3 << 20
        )
        layout = capability_runner._transport_allocation_bound(
            bounds.encoded_bytes,
            bounds,
            capability_runner.conservative_allocation_schema(),
            64 << 10,
        )
        self.assertEqual(
            layout.permit_reservation_bytes,
            layout.canonical_encoder_scratch_bytes
            + layout.sender_payload_bytes
            + layout.pipe_frame_bytes
            + layout.pipe_kernel_capacity_bytes
            + layout.receiver_payload_bytes,
        )
        self.assertEqual(
            layout.decode_reservation_bytes,
            layout.receiver_payload_bytes
            + layout.json_decoded_transient_bytes
            + layout.retained_result_bytes,
        )
        self.assertEqual(
            layout.peak_pending_bytes,
            max(
                layout.counting_pass_peak_bytes,
                layout.permit_reservation_bytes,
                layout.decode_reservation_bytes,
            ),
        )

    def test_compact_payload_preparse_is_allocation_free_and_conservative(self):
        result = capability_model.ConfigurationAuditResult(
            self.configuration.digest,
            self.engine,
            (),
            (PurePosixPath("playback/gpu/coordinator.cpp"),),
            (),
        )
        payload = capability_cache.encode_configuration_audit_result(result)
        with mock.patch(
            "gpu_capability_runner.json.loads",
            side_effect=AssertionError("preparse allocated JSON"),
        ):
            bounds = capability_runner.preparse_compact_result(
                payload,
                expected_configuration_digest=self.configuration.digest,
                expected_audit_engine_fingerprint=self.engine,
                limits=AuditLimits(),
            )
        self.assertEqual(bounds.encoded_bytes, len(payload))
        self.assertGreaterEqual(
            bounds.conservative_retained_bytes,
            capability_model.compact_result_retained_bytes(result),
        )
        self.assertGreaterEqual(
            bounds.conservative_decoded_bytes,
            bounds.encoded_bytes,
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "payload"):
            capability_runner.preparse_compact_result(
                payload + b" ",
                expected_configuration_digest=self.configuration.digest,
                expected_audit_engine_fingerprint=self.engine,
                limits=AuditLimits(),
            )

    def test_mixed_budget_rejects_worker_payload_before_decode(self):
        result = capability_model.ConfigurationAuditResult(
            self.configuration.digest,
            self.engine,
            (),
            (),
            (
                capability_model.AuditResultFinding(
                    PurePosixPath("playback/gpu/coordinator.cpp"),
                    1,
                    "x" * (16 << 10),
                    "mixed budget pressure",
                ),
            ),
        )
        payload = capability_cache.encode_configuration_audit_result(result)
        bounds = capability_runner.preparse_compact_result(
            payload,
            expected_configuration_digest=self.configuration.digest,
            expected_audit_engine_fingerprint=self.engine,
            limits=AuditLimits(),
        )
        layout = capability_runner._transport_allocation_bound(
            len(payload),
            bounds,
            capability_runner.conservative_allocation_schema(),
            capability_runner._PIPE_KERNEL_CAPACITY_BYTES,
        )
        budget = CompactResultMemoryBudget(maximum_bytes=layout.decode_reservation_bytes)
        queued = budget.reserve(
            layout.permit_reservation_bytes,
            label="task:mixed-budget:queued",
        ).commit()
        blocker = budget.reserve(
            layout.decode_reservation_bytes - layout.permit_reservation_bytes,
            label="hit:mixed-budget:retained",
        ).commit()
        with mock.patch(
            "gpu_capability_runner.receive_configuration_audit_outcome"
        ) as decode, self.assertRaisesRegex(
            AuditInfrastructureError, "128 MiB|replacement"
        ):
            capability_runner._transition_worker_payload_to_decode(
                queued, bounds, layout
            )
        decode.assert_not_called()
        blocker.release()
        queued.release()

    def test_cold_slot_can_be_transferred_to_one_worker_decode_owner(self):
        budget = CompactResultMemoryBudget(maximum_bytes=128 << 20)
        aggregator = capability_model.StreamingResultAggregator(
            (self.configuration,), budget, AuditLimits()
        )
        slot = capability_model.CompactResultColdSlot(32 << 20)
        aggregator.reserve_cold_slot(slot)
        live_before = budget.live_bytes
        ownership = aggregator.take_cold_slot("task:decode")
        self.assertTrue(ownership.committed)
        self.assertEqual(ownership.byte_count, slot.worst_case_live_bytes)
        self.assertEqual(aggregator.cold_slot_reserved_bytes, 0)
        self.assertEqual(budget.live_bytes, live_before)
        with self.assertRaisesRegex(AuditInfrastructureError, "cold slot"):
            aggregator.take_cold_slot("task:second-decode")
        ownership.release()
        summary = aggregator.finish()
        summary.release()

    def test_ordinal_dispatch_window_does_not_advance_across_a_gap(self):
        window = capability_runner.OrdinalDispatchWindow(251, 4)
        self.assertEqual(window.dispatchable_ordinals(), (0, 1, 2, 3))
        window.accept(1)
        window.accept(2)
        window.accept(3)
        self.assertEqual(window.next_unaggregated, 0)
        self.assertEqual(window.dispatchable_ordinals(), ())
        window.accept(0)
        self.assertEqual(window.next_unaggregated, 4)
        self.assertEqual(window.dispatchable_ordinals(), (4, 5, 6, 7))

    def test_native_memory_contract_is_independent_from_compact_budget(self):
        compact = SimpleNamespace(peak_live_bytes=128 << 20)
        capability_runner._enforce_platform_memory_contract(
            SimpleNamespace(
                platform_kind=(
                    "windows" if os.name == "nt"
                    else ("macos" if sys.platform == "darwin" else "linux")
                ),
                maximum_observed_resident_bytes=(512 << 20) - 1,
                accounting_complete=True,
                surviving_processes=(),
            ),
            compact,
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "process-tree memory contract"
        ):
            capability_runner._enforce_platform_memory_contract(
                SimpleNamespace(
                    platform_kind=(
                        "windows" if os.name == "nt"
                        else (
                            "macos" if sys.platform == "darwin" else "linux"
                        )
                    ),
                    maximum_observed_resident_bytes=512 << 20,
                    accounting_complete=True,
                    surviving_processes=(),
                ),
                compact,
            )

    def test_recycled_generation_memory_uses_per_slot_archived_maximum(self):
        self.assertEqual(
            capability_runner._combine_owned_generation_memory(
                10,
                {0: 20, 1: 15},
                ((0, 0, 30), (0, 1, 25), (1, 0, 5)),
            ),
            55,
        )

    def test_run_scoped_compact_accounting_observer_never_double_charges(self):
        observer = capability_model.CompactAccountingObserver()
        budget = CompactResultMemoryBudget(observer=observer)
        ownership = budget.reserve(4096, label="observer-fixture").commit()
        self.assertEqual(observer.live_bytes, 4096)
        self.assertEqual(observer.peak_live_bytes, 4096)
        ownership.release()
        self.assertEqual(observer.live_bytes, 0)
        self.assertEqual(
            tuple(event[0] for event in observer.events),
            ("reserve", "commit", "release"),
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "already released"):
            ownership.release()

    def test_compact_observer_rejects_duplicate_semantic_owner_identity(self):
        observer = capability_model.CompactAccountingObserver()
        budget = CompactResultMemoryBudget(
            maximum_bytes=1 << 20, observer=observer
        )
        ownership = budget.reserve(4096, label="semantic-owner").commit()
        with self.assertRaisesRegex(AuditInfrastructureError, "owner|overlap"):
            observer.transition(
                "reserve",
                4096,
                "forged-alias",
                owner_id=ownership.owner_id,
            )
        replacement = ownership.replace_committed(
            2048, label="semantic-owner:retained"
        )
        self.assertNotEqual(replacement.owner_id, ownership.owner_id)
        with self.assertRaisesRegex(AuditInfrastructureError, "owner|underflow"):
            observer.transition(
                "release",
                -4096,
                "stale-owner",
                owner_id=ownership.owner_id,
            )
        replacement.release()
        self.assertEqual(
            tuple(event[0] for event in observer.events),
            ("reserve", "commit", "replace", "release"),
        )

        stale_observer = capability_model.CompactAccountingObserver()
        stale_budget = CompactResultMemoryBudget(
            maximum_bytes=1 << 20, observer=stale_observer
        )
        stale = stale_budget.reserve(
            1024, label="stale semantic owner"
        ).commit()
        stale_observer.transition(
            "release",
            -1024,
            stale.label,
            owner_id=stale.owner_id,
            budget_id=stale_budget.budget_id,
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "owner|underflow"
        ):
            stale.release(semantic_event="release-result")
        self.assertEqual(stale_budget.live_bytes, 1024)

    def test_run_scoped_observer_allocates_unique_cross_budget_semantic_owners(self):
        observer = capability_model.CompactAccountingObserver()
        hit_budget = CompactResultMemoryBudget(
            maximum_bytes=1 << 20, observer=observer
        )
        dispatch_budget = CompactResultMemoryBudget(
            maximum_bytes=1 << 20, observer=observer
        )
        hit = hit_budget.reserve(
            4096, label="cached result"
        ).commit()
        hit.record_semantic("retain-hit", delta_bytes=4096)
        dispatch = dispatch_budget.reserve(
            2048, label="queued transport", semantic_event="reserve-dispatch"
        ).commit()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "semantic owner"
        ):
            hit.record_semantic("retain-hit", delta_bytes=1)
        self.assertNotEqual(hit_budget.budget_id, dispatch_budget.budget_id)
        self.assertNotEqual(hit.owner_id, dispatch.owner_id)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "cross-budget|semantic owner"
        ):
            observer.transition(
                "commit",
                0,
                "forged cross-budget alias",
                budget_id=dispatch_budget.budget_id,
                owner_id=hit.owner_id,
            )
        hit.release(semantic_event="release-result")
        dispatch.record_semantic("activate-publication")
        dispatch.record_semantic("release-publication")
        dispatch.record_semantic("send-pipe")
        dispatch.release()
        self.assertEqual(
            tuple((event, delta) for event, delta, _label, _owner in observer.semantic_events),
            (
                ("retain-hit", 4096),
                ("reserve-dispatch", 2048),
                ("release-result", -4096),
                ("activate-publication", 0),
                ("release-publication", 0),
                ("send-pipe", 0),
            ),
        )

    def test_compact_observer_rejects_wrong_semantic_transition_and_order(self):
        observer = capability_model.CompactAccountingObserver()
        budget = CompactResultMemoryBudget(observer=observer)
        with self.assertRaisesRegex(AuditInfrastructureError, "semantic"):
            budget.reserve(
                8, label="wrong reserve", semantic_event="release-result"
            )
        ownership = budget.reserve(8, label="unclassified").commit()
        with self.assertRaisesRegex(AuditInfrastructureError, "semantic"):
            ownership.record_semantic("decode")
        with self.assertRaisesRegex(AuditInfrastructureError, "semantic"):
            ownership.release(semantic_event="retain-hit")
        self.assertEqual(budget.live_bytes, 8)
        ownership.release()

        dispatch = budget.reserve(
            16, label="dispatch", semantic_event="reserve-dispatch"
        ).commit()
        with self.assertRaisesRegex(AuditInfrastructureError, "semantic"):
            dispatch.record_semantic("release-publication")
        dispatch.record_semantic("activate-publication")
        dispatch.record_semantic("release-publication")
        dispatch.record_semantic("send-pipe")
        dispatch.release()

    def test_forged_payload_ready_cannot_release_active_or_advance_fifo(self):
        observer = capability_model.CompactAccountingObserver()
        budget = CompactResultMemoryBudget(observer=observer)
        reservation = capability_model.PerTaskCompactReservation(
            "audit-0-cccccccccccccccc", 0, 64 << 20
        )
        capability = reservation.issue_worker_transport_capability(
            "audit-0-cccccccccccccccc",
            0,
            self.configuration.digest,
            self.engine,
            0,
        )
        queued = budget.reserve(
            4096, label="queued transport", semantic_event="reserve-dispatch"
        ).commit()
        pending = capability_runner._PendingCoordinatorTask(
            0,
            self.configuration,
            "audit-0-cccccccccccccccc",
            reservation,
            capability,
            queued,
        )
        state = SimpleNamespace(
            worker_index=0,
            generation=0,
            pending=pending,
            launch_events=[
                SimpleNamespace(
                    purpose=capability_model.CompilerLaunchPurpose.AUDIT_DISCOVERY
                ),
                SimpleNamespace(
                    purpose=capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED
                ),
            ],
            payload_receiver=SimpleNamespace(
                receive_transport_before=mock.Mock(
                    side_effect=AssertionError("payload read before ready authentication")
                )
            ),
        )
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.states = [state, SimpleNamespace(generation=0, pending=None)]
        reactor.active_publication = (
            0, 0, "audit-0-cccccccccccccccc"
        )
        reactor.publication_requests = __import__("collections").deque(
            ((1, 0, "audit-1-dddddddddddddddd"),)
        )
        reactor.runtime_contract = self.runtime
        reactor.cancel_event = threading.Event()
        reactor._send_command = mock.Mock()
        reactor.grant_next_publication = mock.Mock()
        event = capability_model.WorkerPayloadReady(
            worker_index=0,
            generation=0,
            task_id=pending.task_id,
            configuration_digest=self.configuration.digest,
            audit_engine_fingerprint=self.engine,
            pipe_nonce=capability.pipe_nonce,
            serial=capability.serial,
            nonce="f" * 64,
            encoded_bytes=256,
            encoded_sha256="e" * 64,
            charged_bytes=4096,
            conservative_decoded_bytes=512,
            conservative_retained_bytes=128,
            counting_pass_peak_bytes=1,
            stdout_bytes=256,
            stages=capability_model.WorkerStageTimings(0.1, 0.1, 0.1, 0.1),
        )
        try:
            with self.assertRaisesRegex(
                AuditInfrastructureError, "receipt|declaration|capability"
            ):
                reactor.handle_task_event(
                    state,
                    event,
                    SimpleNamespace(),
                )
            self.assertEqual(
                reactor.active_publication,
                (0, 0, "audit-0-cccccccccccccccc"),
            )
            self.assertEqual(
                tuple(reactor.publication_requests),
                ((1, 0, "audit-1-dddddddddddddddd"),),
            )
            reactor._send_command.assert_not_called()
            reactor.grant_next_publication.assert_not_called()
            state.payload_receiver.receive_transport_before.assert_not_called()
        finally:
            if not queued.released:
                queued.release()
            if not reservation.released:
                reservation.release_worker_transport_capability(
                    capability, "test-cleanup"
                )

    def test_spawn_primary_error_survives_linux_release_cleanup_error(self):
        class PrimarySpawnError(RuntimeError):
            pass

        class CleanupError(RuntimeError):
            pass

        class FailingLinuxLifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(self):
                self.release_calls = 0

            def create_generation(self, worker_index, generation):
                return (worker_index, generation)

            def release_generation(
                self, worker_index, generation, carrier, deadline, *, force
            ):
                self.release_calls += 1
                if self.release_calls == 1:
                    raise CleanupError("release failed")

            def seal_task_phase(self, count):
                return capability_model.LinuxPhaseSnapshot(
                    "linux", "tasks",
                    capability_model.LinuxRunMemoryMeasurements(
                        1, 2, 512 << 20, 448 << 20,
                        0, 0, 0, 0, 0, 0, 0, True,
                    ), count,
                )

        class Process:
            pid = 123

            def __init__(self):
                self._liveness = iter((
                    CleanupError("liveness failed"),
                    CleanupError("post-join liveness failed"),
                    CleanupError("post-kill liveness failed"),
                    False,
                    False,
                ))
                self._joins = iter((
                    CleanupError("join failed"),
                    CleanupError("post-kill join failed"),
                    None,
                ))

            def start(self):
                raise PrimarySpawnError("spawn failed")

            def is_alive(self):
                value = next(self._liveness)
                if isinstance(value, BaseException):
                    raise value
                return value

            def terminate(self):
                raise CleanupError("terminate failed")

            def join(self, timeout):
                del timeout
                value = next(self._joins)
                if isinstance(value, BaseException):
                    raise value

            def kill(self):
                raise CleanupError("kill failed")

        class Context:
            @staticmethod
            def Process(**_kwargs):
                return Process()

        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.states = [None]
        reactor.next_generations = [0]
        reactor.context = Context()
        reactor.cancel_event = threading.Event()
        reactor.runtime_contract = self.runtime
        reactor.configurations = (self.configuration,)
        reactor.dependency_roots = self.authority
        reactor.production_snapshot = {}
        reactor.cache_root = self.root / "spawn-cleanup-cache"
        reactor.limits = AuditLimits()
        reactor.engine = self.engine
        reactor.run_accountant = SimpleNamespace()
        registry = capability_runner.CompilerCapabilityRegistry(self.authority)
        registry.register(self.capability)
        reactor.capability_registry = registry
        reactor.native_lifecycle = FailingLinuxLifecycle()
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {}
        reactor.worker_pids = []
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.deferred_events = __import__("collections").deque()
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        reactor._closed = False
        original_rmtree = capability_runner.shutil.rmtree
        rmtree_failed = False

        def fail_rmtree_once(path):
            nonlocal rmtree_failed
            if not rmtree_failed:
                rmtree_failed = True
                raise CleanupError("rmtree failed")
            return original_rmtree(path)

        try:
            with mock.patch.object(
                capability_runner, "_worker_scratch_parent", return_value=self.root
            ), mock.patch.object(
                capability_runner.shutil,
                "rmtree",
                side_effect=fail_rmtree_once,
            ):
                with self.assertRaises(PrimarySpawnError) as raised:
                    reactor.spawn_next_generation(0)
            notes = getattr(raised.exception, "__notes__", ())
            self.assertTrue(any("terminate failed" in note for note in notes))
            self.assertTrue(any("join failed" in note for note in notes))
            self.assertTrue(any("kill failed" in note for note in notes))
            self.assertTrue(any("rmtree failed" in note for note in notes))
            self.assertTrue(any("release failed" in note for note in notes))
        finally:
            registry.close()

    def test_spawn_setup_failure_closes_every_previously_created_channel(self):
        class SetupError(RuntimeError):
            pass

        endpoints = [mock.Mock() for _ in range(8)]
        pairs = tuple(
            (endpoints[index], endpoints[index + 1])
            for index in range(0, len(endpoints), 2)
        )
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.states = [None]
        reactor.next_generations = [0]
        reactor.runtime_contract = self.runtime
        reactor.cancel_event = threading.Event()
        reactor.native_lifecycle = None
        reactor.run_accountant = SimpleNamespace()
        reactor.capability_registry = SimpleNamespace()
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {}
        reactor.worker_pids = []
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.deferred_events = __import__("collections").deque()
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        reactor._closed = False
        with mock.patch.object(
            capability_runner.BoundedFrameChannel,
            "create",
            side_effect=pairs,
        ), mock.patch.object(
            capability_runner.BoundedPayloadChannel,
            "create",
            side_effect=SetupError("payload setup failed"),
        ), self.assertRaisesRegex(SetupError, "payload setup failed"):
            reactor.spawn_next_generation(0)
        for endpoint in endpoints:
            endpoint.close.assert_called_once_with()

    def test_task9_process_construction_failure_retries_provisional_ownership(self):
        class ProcessConstructionError(RuntimeError):
            pass

        class Lifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(_self):
                _self.release_calls = 0
                _self.saw_provisional = False

            def create_generation(_self, worker_index, generation):
                _self.saw_provisional = reactor.states[worker_index] is not None
                return capability_model.LinuxWorkerContainment(
                    "/run", f"/run/{worker_index}-{generation}", "a" * 64
                )

            def release_generation(
                _self, _worker, _generation, _carrier, _deadline, *, force
            ):
                self.assertTrue(force)
                _self.release_calls += 1
                if _self.release_calls == 1:
                    raise RuntimeError("leaf release once")

            def seal_task_phase(_self, count):
                self.assertEqual(count, 0)
                return capability_model.LinuxPhaseSnapshot(
                    "linux",
                    "tasks",
                    capability_model.LinuxRunMemoryMeasurements(
                        1, 2, 512 << 20, 448 << 20,
                        0, 0, 0, 0, 0, 0, 0, True,
                    ),
                    0,
                )

        class Context:
            @staticmethod
            def Process(**_kwargs):
                raise ProcessConstructionError("construct failed")

        frame_endpoints = [mock.Mock() for _ in range(8)]
        frame_pairs = tuple(
            (frame_endpoints[index], frame_endpoints[index + 1])
            for index in range(0, len(frame_endpoints), 2)
        )
        payload_receiver, payload_sender = mock.Mock(), mock.Mock()
        lifecycle = Lifecycle()
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.states = [None]
        reactor.next_generations = [0]
        reactor.context = Context()
        reactor.cancel_event = threading.Event()
        reactor.runtime_contract = self.runtime
        reactor.configurations = (self.configuration,)
        reactor.dependency_roots = self.authority
        reactor.production_snapshot = {}
        reactor.cache_root = self.root / "provisional-construct-cache"
        reactor.limits = AuditLimits()
        reactor.engine = self.engine
        reactor.run_accountant = SimpleNamespace()
        reactor.capability_registry = SimpleNamespace()
        reactor.native_lifecycle = lifecycle
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {}
        reactor.worker_pids = []
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.deferred_events = __import__("collections").deque()
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        reactor._closed = False

        with mock.patch.object(
            capability_runner.BoundedFrameChannel,
            "create",
            side_effect=frame_pairs,
        ), mock.patch.object(
            capability_runner.BoundedPayloadChannel,
            "create",
            return_value=(payload_receiver, payload_sender),
        ), mock.patch.object(
            capability_runner,
            "_worker_scratch_parent",
            return_value=self.root,
        ), self.assertRaisesRegex(
            ProcessConstructionError, "construct failed"
        ):
            reactor.spawn_next_generation(0)

        self.assertTrue(lifecycle.saw_provisional)
        self.assertEqual(lifecycle.release_calls, 2)
        self.assertEqual(reactor.states, [None])
        self.assertTrue(reactor._closed)
        for endpoint in (*frame_endpoints, payload_receiver, payload_sender):
            endpoint.close.assert_called_once_with()

    def test_task9_provisional_setup_fault_matrix_uses_shared_teardown(self):
        digest = self.configuration.compiler_capability_digest
        labels = (
            "native-leaf-create",
            "process-construct",
            f"registry-duplicate:{digest}",
            "accounting-job-create",
            "process-start",
            "accounting-job-assign",
            "state-promotion",
        )

        for target in labels:
            for position in ("before", "after"):
                with self.subTest(target=target, position=position):
                    class Lifecycle(capability_runner._LinuxGenerationLifecycle):
                        def __init__(_self):
                            _self.created = 0
                            _self.released = 0

                        def create_generation(_self, worker, generation):
                            _self.created += 1
                            return capability_model.LinuxWorkerContainment(
                                "/run", f"/run/{worker}-{generation}", "b" * 64
                            )

                        def release_generation(
                            _self, _worker, _generation, _carrier,
                            _deadline, *, force
                        ):
                            self.assertTrue(force)
                            _self.released += 1

                        def seal_task_phase(_self, count):
                            return capability_model.LinuxPhaseSnapshot(
                                "linux", "tasks",
                                capability_model.LinuxRunMemoryMeasurements(
                                    1, 2, 512 << 20, 448 << 20,
                                    0, 0, 0, 0, 0, 0, 0, True,
                                ), count,
                            )

                    class Process:
                        def __init__(_self):
                            _self.pid = None
                            _self.sentinel = None

                        def start(_self):
                            _self.pid = 4321
                            _self.sentinel = 99

                        def is_alive(_self):
                            return False

                        def join(_self, timeout):
                            del timeout

                        def terminate(_self):
                            raise AssertionError("dead process terminated")

                        def kill(_self):
                            raise AssertionError("dead process killed")

                    process = Process()
                    context = SimpleNamespace(
                        Process=mock.Mock(return_value=process)
                    )

                    class Accountant:
                        def __init__(_self):
                            _self.created = set()
                            _self.assigned = set()
                            _self.archived = []
                            _self.discarded = []

                        def create_generation_job(_self, worker, generation):
                            _self.created.add((worker, generation))

                        def assign_generation_process(
                            _self, worker, generation, *_args
                        ):
                            _self.assigned.add((worker, generation))

                        def archive_generation_job(
                            _self, worker, generation, _deadline
                        ):
                            key = (worker, generation)
                            self.assertIn(key, _self.assigned)
                            _self.archived.append(key)

                        def discard_generation_job(
                            _self, worker, generation
                        ):
                            key = (worker, generation)
                            self.assertNotIn(key, _self.assigned)
                            _self.discarded.append(key)

                    accountant = Accountant()
                    lifecycle = Lifecycle()
                    class Registry:
                        def __init__(_self):
                            _self.opened = {}
                            _self.released = set()

                        def duplicate_for_generation(
                            _self, actual_digest, worker, generation
                        ):
                            duplicate = capability_runner._GenerationCompilerCapabilityDuplicate(
                                _self, actual_digest, worker, generation, ()
                            )
                            _self.opened[(worker, generation)] = duplicate
                            return duplicate

                        def transfer_duplicate_to_child(_self, *_args):
                            pass

                        def acknowledge_generation(_self, *_args):
                            pass

                        def release_generation(_self, worker, generation):
                            key = (worker, generation)
                            duplicate = _self.opened.pop(key, None)
                            if duplicate is not None:
                                duplicate.close()
                            _self.released.add(key)

                    registry = Registry()
                    frame_endpoints = [mock.Mock() for _ in range(8)]
                    frame_pairs = tuple(
                        (frame_endpoints[index], frame_endpoints[index + 1])
                        for index in range(0, len(frame_endpoints), 2)
                    )
                    payload_receiver, payload_sender = mock.Mock(), mock.Mock()
                    reactor = object.__new__(capability_runner.GenerationReactor)
                    reactor.states = [None]
                    reactor.next_generations = [0]
                    reactor.context = context
                    reactor.cancel_event = threading.Event()
                    reactor.runtime_contract = self.runtime
                    reactor.configurations = (self.configuration,)
                    reactor.dependency_roots = self.authority
                    reactor.production_snapshot = {}
                    reactor.cache_root = self.root / (
                        f"setup-{position}-{target.replace(':', '-')}"
                    )
                    reactor.limits = AuditLimits()
                    reactor.engine = self.engine
                    reactor.run_accountant = accountant
                    reactor.capability_registry = registry
                    reactor.native_lifecycle = lifecycle
                    reactor._accounting_lock = threading.Lock()
                    reactor.registry_generation_duplicates = {}
                    reactor.worker_pids = []
                    reactor.archived_generation_telemetry = []
                    reactor.archived_scratch_roots = []
                    reactor.publication_requests = __import__("collections").deque()
                    reactor.active_publication = None
                    reactor.deferred_events = __import__("collections").deque()
                    reactor.task_phase_snapshot = None
                    reactor._stop_accounting_pump = mock.Mock()
                    reactor._closed = False
                    faulted = False

                    def fail(label, _state):
                        nonlocal faulted
                        if label == target and not faulted:
                            faulted = True
                            raise RuntimeError(f"{position} {label}")

                    before = fail if position == "before" else mock.Mock()
                    after = fail if position == "after" else mock.Mock()
                    with mock.patch.object(
                            capability_runner.BoundedFrameChannel,
                            "create",
                            side_effect=frame_pairs,
                        ), mock.patch.object(
                            capability_runner.BoundedPayloadChannel,
                            "create",
                            return_value=(payload_receiver, payload_sender),
                        ), mock.patch.object(
                            capability_runner,
                            "_worker_scratch_parent",
                            return_value=self.root,
                        ), mock.patch.object(
                            capability_runner,
                            "_generation_setup_before_transition",
                            side_effect=before,
                        ), mock.patch.object(
                            capability_runner,
                            "_generation_setup_transition",
                            side_effect=after,
                        ), self.assertRaisesRegex(
                            RuntimeError, f"{position}"
                        ):
                            reactor.spawn_next_generation(0)
                    self.assertTrue(faulted)
                    self.assertTrue(reactor._closed)
                    self.assertEqual(reactor.states, [None])
                    self.assertEqual(reactor.registry_generation_duplicates, {})
                    self.assertEqual(registry.opened, {})
                    created = bool(accountant.created)
                    assigned = bool(accountant.assigned)
                    self.assertEqual(
                        accountant.archived,
                        ([(0, 0)] if created and assigned else []),
                    )
                    self.assertEqual(
                        accountant.discarded,
                        ([(0, 0)] if created and not assigned else []),
                    )
                    self.assertEqual(
                        lifecycle.released,
                        1 if lifecycle.created else 0,
                    )
                    for endpoint in (
                        *frame_endpoints, payload_receiver, payload_sender
                    ):
                        endpoint.close.assert_called_once_with()

    def test_task9_setup_failure_retries_job_archive_faults_exactly_once(self):
        for position in ("before", "after"):
            with self.subTest(position=position):
                state = SimpleNamespace(
                    worker_index=0,
                    generation=0,
                    process=mock.Mock(pid=8123),
                    process_started=True,
                    generation_job_created=True,
                    generation_job_assigned=True,
                    registry_generation_acquired=False,
                    native_generation_acquired=False,
                    pending=None,
                    scratch_root=None,
                    startup_sender=None,
                    task_sender=None,
                    command_sender=None,
                    event_receiver=None,
                    payload_receiver=None,
                )
                state.process.is_alive.return_value = False
                accountant = SimpleNamespace(
                    archive_generation_job=mock.Mock()
                )
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor.states = [state]
                reactor.cancel_event = threading.Event()
                reactor.native_lifecycle = None
                reactor.run_accountant = accountant
                reactor.capability_registry = SimpleNamespace()
                reactor._accounting_lock = threading.Lock()
                reactor.registry_generation_duplicates = {}
                reactor.worker_pids = [8123]
                reactor.archived_generation_telemetry = []
                reactor.archived_scratch_roots = []
                reactor.publication_requests = __import__("collections").deque()
                reactor.active_publication = None
                reactor.deferred_events = __import__("collections").deque()
                reactor.task_phase_snapshot = None
                reactor._stop_accounting_pump = mock.Mock()
                reactor._closed = False
                faulted = False

                def fail(label, _state):
                    nonlocal faulted
                    if label == "accounting-archive" and not faulted:
                        faulted = True
                        raise RuntimeError(f"{position} archive")

                before = fail if position == "before" else mock.Mock()
                after = fail if position == "after" else mock.Mock()
                with mock.patch.object(
                    capability_runner,
                    "_generation_teardown_before_transition",
                    side_effect=before,
                ), mock.patch.object(
                    capability_runner,
                    "_generation_teardown_transition",
                    side_effect=after,
                ):
                    failures = reactor.abort_and_reap_until_closed(
                        time.monotonic() + 10.0
                    )
                self.assertTrue(failures)
                self.assertTrue(reactor._closed)
                accountant.archive_generation_job.assert_called_once_with(
                    0, 0, mock.ANY
                )

    def test_late_spawn_failure_fully_reaps_before_clearing_installed_state(self):
        class PrimaryError(RuntimeError):
            pass

        process = mock.Mock()
        process.pid = 4321
        process.sentinel = 99
        process.is_alive.side_effect = (
            RuntimeError("liveness failed"),
            RuntimeError("post-join liveness failed"),
            RuntimeError("post-kill liveness failed"),
            False,
            False,
        )
        process.terminate.side_effect = RuntimeError("terminate failed")
        process.join.side_effect = (
            RuntimeError("join failed"),
            RuntimeError("post-kill join failed"),
            None,
        )
        process.kill.side_effect = RuntimeError("kill failed")
        context = SimpleNamespace(Process=mock.Mock(return_value=process))
        frame_endpoints = [mock.Mock() for _ in range(8)]
        frame_pairs = tuple(
            (frame_endpoints[index], frame_endpoints[index + 1])
            for index in range(0, len(frame_endpoints), 2)
        )
        startup_sender = frame_pairs[0][1]
        startup_sender.send_bytes_before.side_effect = PrimaryError(
            "bootstrap failed"
        )
        frame_pairs[3][0].poll.return_value = False
        payload_receiver, payload_sender = mock.Mock(), mock.Mock()

        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.states = [None]
        reactor.next_generations = [0]
        reactor.context = context
        reactor.cancel_event = threading.Event()
        reactor.runtime_contract = self.runtime
        reactor.configurations = (self.configuration,)
        reactor.dependency_roots = self.authority
        reactor.production_snapshot = {}
        reactor.cache_root = self.root / "late-spawn-cache"
        reactor.limits = AuditLimits()
        reactor.engine = self.engine
        reactor.run_accountant = SimpleNamespace(
            create_generation_job=mock.Mock(),
            assign_generation_process=mock.Mock(),
            archive_generation_job=mock.Mock(),
            generation_job_assignment_state=mock.Mock(
                return_value="unassigned"
            ),
            discard_generation_job=mock.Mock(),
        )
        registry = capability_runner.CompilerCapabilityRegistry(self.authority)
        registry.register(self.capability)
        reactor.capability_registry = registry
        reactor.native_lifecycle = None
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {}
        reactor.worker_pids = []
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.deferred_events = __import__("collections").deque()
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        reactor._closed = False
        try:
            with mock.patch.object(
                capability_runner.BoundedFrameChannel,
                "create",
                side_effect=frame_pairs,
            ), mock.patch.object(
                capability_runner.BoundedPayloadChannel,
                "create",
                return_value=(payload_receiver, payload_sender),
            ), mock.patch.object(
                capability_runner,
                "_worker_scratch_parent",
                return_value=self.root,
            ), mock.patch.object(
                capability_runner,
                "_duplicate_windows_capability_streams",
                return_value=tuple(
                    range(100, 100 + len(self.capability.native_owner.streams))
                ),
            ), self.assertRaisesRegex(PrimaryError, "bootstrap failed"):
                reactor.spawn_next_generation(0)
            process.terminate.assert_called_once_with()
            self.assertGreaterEqual(process.join.call_count, 2)
            process.kill.assert_called_once_with()
            self.assertEqual(reactor.states, [None])
            self.assertEqual(reactor.registry_generation_duplicates, {})
            self.assertEqual(reactor.worker_pids, [])
        finally:
            registry.close()

    def test_abort_and_reap_retains_every_unconfirmed_generation_for_retry(self):
        class CleanupError(RuntimeError):
            pass

        def state(index):
            process = mock.Mock()
            process.is_alive.side_effect = CleanupError(
                f"liveness {index} failed"
            )
            process.terminate.side_effect = CleanupError(
                f"terminate {index} failed"
            )
            process.join.side_effect = CleanupError(f"join {index} failed")
            process.kill.side_effect = CleanupError(f"kill {index} failed")
            pending = SimpleNamespace(
                reservation=SimpleNamespace(
                    released=False,
                    release_worker_transport_capability=mock.Mock(
                        side_effect=CleanupError(
                            f"reservation {index} failed"
                        )
                    ),
                ),
                capability=object(),
                queued_ownership=SimpleNamespace(
                    released=False,
                    release=mock.Mock(
                        side_effect=CleanupError(f"queue {index} failed")
                    ),
                ),
            )
            return SimpleNamespace(
                worker_index=index,
                generation=0,
                process=process,
                native_carrier=object(),
                pending=pending,
                scratch_root=self.root / f"abort-{index}",
                startup_sender=mock.Mock(),
                task_sender=mock.Mock(),
                command_sender=mock.Mock(),
                event_receiver=mock.Mock(),
                payload_receiver=mock.Mock(),
            )

        states = [state(0), state(1)]
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.cancel_event = threading.Event()
        reactor.states = states.copy()
        reactor.native_lifecycle = SimpleNamespace(
            release_generation=mock.Mock(
                side_effect=CleanupError("native release failed")
            )
        )
        reactor.run_accountant = SimpleNamespace(
            archive_generation_job=mock.Mock(
                side_effect=CleanupError("archive failed")
            )
        )
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {(0, 0): (), (1, 0): ()}
        reactor.capability_registry = SimpleNamespace(
            release_generation=mock.Mock(
                side_effect=CleanupError("registry release failed")
            )
        )
        reactor.archived_scratch_roots = []
        reactor.publication_requests = []
        reactor.active_publication = None
        reactor._stop_accounting_pump = mock.Mock(
            side_effect=CleanupError("pump failed")
        )
        reactor._closed = False
        with mock.patch.object(
            capability_runner.shutil,
            "rmtree",
            side_effect=CleanupError("scratch failed"),
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "cleanup is incomplete"
        ):
            reactor.abort_and_reap(time.monotonic() + 1.0)
        self.assertEqual(reactor.states, states)
        for item in states:
            item.process.terminate.assert_called_once_with()
            item.process.kill.assert_called_once_with()
            item.payload_receiver.close.assert_not_called()
            item.pending.queued_ownership.release.assert_not_called()

    def test_task9_ambiguous_process_start_is_retained_until_liveness_is_false(self):
        process = mock.Mock(pid=None)
        process.is_alive.side_effect = (
            RuntimeError("start liveness unknown"),
            RuntimeError("post-join liveness unknown"),
            RuntimeError("post-kill liveness unknown"),
            False,
        )
        state = SimpleNamespace(
            worker_index=0,
            generation=0,
            process=process,
            process_started=False,
            process_start_attempted=True,
            native_generation_acquired=False,
            generation_job_created=False,
            registry_generation_acquired=False,
            pending=None,
            scratch_root=None,
            startup_sender=None,
            task_sender=None,
            command_sender=None,
            event_receiver=None,
            payload_receiver=None,
        )
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.states = [state]
        reactor.cancel_event = threading.Event()
        reactor.native_lifecycle = None
        reactor.run_accountant = SimpleNamespace()
        reactor.capability_registry = SimpleNamespace()
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {}
        reactor.worker_pids = []
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.deferred_events = __import__("collections").deque()
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        reactor._closed = False

        with self.assertRaisesRegex(
            AuditInfrastructureError, "cleanup is incomplete"
        ):
            reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertIs(reactor.states[0], state)
        reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertTrue(reactor._closed)
        self.assertEqual(reactor.states, [None])

    def test_task9_abort_retries_native_and_job_release_without_double_release(self):
        class ArchiveError(RuntimeError):
            pass

        process = mock.Mock()
        process.is_alive.return_value = False
        state = SimpleNamespace(
            worker_index=0,
            generation=3,
            process=process,
            native_carrier=object(),
            pending=None,
            scratch_root=self.root / "archive-retry-missing",
            startup_sender=mock.Mock(),
            task_sender=mock.Mock(),
            command_sender=mock.Mock(),
            event_receiver=mock.Mock(),
            payload_receiver=mock.Mock(),
        )
        accountant = SimpleNamespace(
            archive_generation_job=mock.Mock(
                side_effect=(ArchiveError("archive deadline expired"), None)
            )
        )
        native_lifecycle = SimpleNamespace(
            release_generation=mock.Mock(
                side_effect=(ArchiveError("native deadline expired"), None)
            )
        )
        registry = SimpleNamespace(release_generation=mock.Mock())
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor.cancel_event = threading.Event()
        reactor.states = [state]
        reactor.native_lifecycle = native_lifecycle
        reactor.run_accountant = accountant
        reactor._accounting_lock = threading.Lock()
        reactor.registry_generation_duplicates = {(0, 3): (object(),)}
        reactor.capability_registry = registry
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        reactor._closed = False

        with self.assertRaisesRegex(
            AuditInfrastructureError, "cleanup is incomplete"
        ):
            reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertIs(reactor.states[0], state)
        accountant.archive_generation_job.assert_not_called()
        registry.release_generation.assert_not_called()
        state.startup_sender.close.assert_not_called()

        with self.assertRaisesRegex(
            AuditInfrastructureError, "cleanup is incomplete"
        ):
            reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertIs(reactor.states[0], state)
        self.assertIn("native-release", state.teardown_completed)
        registry.release_generation.assert_not_called()

        reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertEqual(reactor.states, [None])
        self.assertTrue(reactor._closed)
        self.assertEqual(accountant.archive_generation_job.call_count, 2)
        self.assertEqual(native_lifecycle.release_generation.call_count, 2)
        registry.release_generation.assert_called_once_with(0, 3)
        state.startup_sender.close.assert_called_once_with()

    def test_task9_normal_to_abort_resumes_after_every_teardown_transition(self):
        transition_labels = (
            "native-release",
            "process-tree-release",
            "accounting-archive",
            "publication-detach",
            "reservation-release",
            "registry-release",
            "registry-duplicate-pop",
            "telemetry-append",
            "scratch-removal",
            "scratch-append",
            "queued-release",
            "startup-endpoint-close",
            "task-endpoint-close",
            "command-endpoint-close",
            "event-endpoint-close",
            "payload-endpoint-close",
            "deferred-events-remove",
            "state-clear",
        )

        for failed_transition in transition_labels:
            with self.subTest(failed_transition=failed_transition):
                scratch = self.root / f"resume-{failed_transition}"
                scratch.mkdir()
                process = mock.Mock(exitcode=0)
                process.is_alive.return_value = False
                tree = mock.Mock(surviving_owned_process_count=0)
                reservation = SimpleNamespace(released=False)
                reservation.release_worker_transport_capability = mock.Mock(
                    side_effect=lambda *_args: setattr(
                        reservation, "released", True
                    )
                )
                queued = SimpleNamespace(released=False)
                queued.release = mock.Mock(
                    side_effect=lambda: setattr(queued, "released", True)
                )
                pending = SimpleNamespace(
                    reservation=reservation,
                    capability=object(),
                    queued_ownership=queued,
                )
                has_pending = failed_transition in {
                    "reservation-release", "queued-release"
                }
                state = SimpleNamespace(
                    worker_index=0,
                    generation=7,
                    process=process,
                    tree=tree,
                    identity=object(),
                    active_descendants={},
                    expects_retire=False,
                    maximum_resident_bytes=123,
                    native_carrier=object(),
                    pending=(pending if has_pending else None),
                    scratch_root=scratch,
                    startup_sender=mock.Mock(),
                    task_sender=mock.Mock(),
                    command_sender=mock.Mock(),
                    event_receiver=mock.Mock(),
                    payload_receiver=mock.Mock(),
                )
                native = SimpleNamespace(release_generation=mock.Mock())
                accountant = SimpleNamespace(
                    archive_generation_job=mock.Mock()
                )
                registry = SimpleNamespace(release_generation=mock.Mock())
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor._closed = False
                reactor.states = [state]
                reactor.cancel_event = threading.Event()
                reactor.publication_requests = __import__("collections").deque(
                    ((0, 7, "queued"),)
                )
                reactor.active_publication = (0, 7, "active")
                reactor.deferred_events = __import__("collections").deque(
                    ((state, object()),)
                )
                reactor.native_lifecycle = native
                reactor.run_accountant = accountant
                reactor.capability_registry = registry
                reactor.registry_generation_duplicates = {(0, 7): (object(),)}
                reactor.archived_generation_telemetry = []
                reactor.archived_scratch_roots = []
                reactor.task_phase_snapshot = None
                reactor._accounting_lock = threading.Lock()
                reactor._stop_accounting_pump = mock.Mock()
                reactor.runtime_contract = capability_model.WorkerRuntimeContract(
                    1, 1, 1 << 20, time.monotonic() + 20.0
                )
                reactor.next_event = mock.Mock(
                    return_value=(
                        state,
                        capability_model.WorkerStopped(0, 7),
                    )
                )
                observed = []
                before_failed = False

                def inject_before(label, _state):
                    nonlocal before_failed
                    if label == failed_transition and not before_failed:
                        before_failed = True
                        raise RuntimeError(f"before {label}")

                def inject(label, _state):
                    observed.append(label)
                    if label == failed_transition and observed.count(label) == 1:
                        raise RuntimeError(f"after {label}")

                with mock.patch.object(
                    capability_runner,
                    "_generation_teardown_before_transition",
                    side_effect=inject_before,
                ), mock.patch.object(
                    capability_runner,
                    "_generation_teardown_transition",
                    side_effect=inject,
                    create=True,
                ):
                    if has_pending:
                        with self.assertRaisesRegex(
                            AuditInfrastructureError,
                            "normal shutdown has outstanding worker task",
                        ):
                            reactor.shutdown_reap(time.monotonic() + 10.0)
                    else:
                        with self.assertRaisesRegex(
                            RuntimeError, f"before {failed_transition}"
                        ):
                            reactor.shutdown_reap(time.monotonic() + 10.0)

                self.assertTrue(reactor._closed)
                self.assertEqual(reactor.states, [None])
                self.assertEqual(native.release_generation.call_count, 1)
                self.assertEqual(accountant.archive_generation_job.call_count, 1)
                self.assertEqual(registry.release_generation.call_count, 1)
                self.assertEqual(
                    reactor.archived_generation_telemetry, [(0, 7, 123)]
                )
                self.assertEqual(reactor.archived_scratch_roots, [scratch])
                self.assertEqual(
                    reservation.release_worker_transport_capability.call_count,
                    1 if has_pending else 0,
                )
                self.assertEqual(queued.release.call_count, 1 if has_pending else 0)
                for endpoint in (
                    state.startup_sender,
                    state.task_sender,
                    state.command_sender,
                    state.event_receiver,
                    state.payload_receiver,
                ):
                    self.assertEqual(endpoint.close.call_count, 1)

    def test_task9_accounting_cleanup_uses_accountant_assignment_progress(self):
        for actual, caller_flag in ((True, False), (False, True)):
            with self.subTest(actual=actual, caller_flag=caller_flag):
                accountant = SimpleNamespace(
                    generation_job_assignment_completed=mock.Mock(
                        return_value=actual
                    ),
                    archive_generation_job=mock.Mock(),
                    discard_generation_job=mock.Mock(),
                )
                state = SimpleNamespace(
                    worker_index=2,
                    generation=5,
                    generation_job_created=True,
                    generation_job_assigned=caller_flag,
                )
                capability_runner._release_generation_job_accounting(
                    accountant,
                    state,
                    time.monotonic() + 10.0,
                    threading.Lock(),
                )
                accountant.generation_job_assignment_completed.assert_called_once_with(
                    2, 5
                )
                if actual:
                    accountant.archive_generation_job.assert_called_once()
                    accountant.discard_generation_job.assert_not_called()
                else:
                    accountant.discard_generation_job.assert_called_once_with(
                        2, 5
                    )
                    accountant.archive_generation_job.assert_not_called()

        accountant = SimpleNamespace(
            generation_job_assignment_state=mock.Mock(
                return_value="aggregate-only"
            ),
            finalize_aggregate_only_generation_job=mock.Mock(),
            archive_generation_job=mock.Mock(),
            discard_generation_job=mock.Mock(),
        )
        state = SimpleNamespace(
            worker_index=2,
            generation=6,
            generation_job_created=True,
            generation_job_assigned=False,
        )
        capability_runner._release_generation_job_accounting(
            accountant,
            state,
            time.monotonic() + 10.0,
            threading.Lock(),
        )
        accountant.finalize_aggregate_only_generation_job.assert_called_once_with(
            2, 6
        )
        accountant.archive_generation_job.assert_not_called()
        accountant.discard_generation_job.assert_not_called()
        self.assertEqual(
            state.generation_job_assignment_state, "aggregate-only"
        )
        self.assertTrue(
            capability_runner._generation_has_accounted_processes(state)
        )

    def test_windows_accounting_cleanup_resumes_every_terminal_fault_boundary(self):
        from gpu_capability_process_tree import WindowsNativeRunAccountant

        class Job:
            def __init__(_self, total):
                _self.total = total
                _self.closed = False
                _self.close_calls = 0
                _self.query_after_close = 0

            def _query(_self):
                if _self.closed:
                    _self.query_after_close += 1
                    raise AssertionError("closed Windows Job was queried")

            def drain_notifications(_self):
                _self._query()

            def accounting_totals(_self):
                _self._query()
                return _self.total, 0, _self.total

            def process_ids(_self):
                _self._query()
                return ()

            def peak_commit_charge_bytes(_self):
                _self._query()
                return 17

            def close(_self):
                _self.close_calls += 1
                _self.closed = True

        for kind in ("aggregate-only", "discard", "archive"):
            for stage in ("close", "terminal", "remove"):
                for position in ("before", "after"):
                    with self.subTest(
                        kind=kind, stage=stage, position=position
                    ):
                        key = (2, 5)
                        job = Job(1 if kind == "archive" else 0)
                        accountant = object.__new__(WindowsNativeRunAccountant)
                        accountant._generation_jobs = {key: job}
                        accountant._archived_generation_jobs = {}
                        accountant._discarded_generation_jobs = set()
                        accountant._aggregate_only_generation_jobs = set()
                        accountant._aggregate_assignment_progress = (
                            {key} if kind != "discard" else set()
                        )
                        accountant._generation_assignment_progress = (
                            {key} if kind == "archive" else set()
                        )
                        accountant._assigned_generation_jobs = (
                            {key} if kind == "archive" else set()
                        )
                        accountant._aggregate_only_job_close_progress = set()
                        accountant._generation_terminal_close_progress = set()
                        accountant._pending_generation_archives = {}
                        state = SimpleNamespace(
                            worker_index=2,
                            generation=5,
                            generation_job_created=True,
                        )
                        faulted = False

                        def inject(actual_kind, actual_stage, actual_position,
                                   worker, generation):
                            nonlocal faulted
                            if (
                                (actual_kind, actual_stage, actual_position)
                                == (kind, stage, position)
                                and (worker, generation) == key
                                and not faulted
                            ):
                                faulted = True
                                raise RuntimeError(
                                    f"{kind} {stage} {position} fault"
                                )

                        with mock.patch(
                            "gpu_capability_process_tree._windows_generation_terminal_transition",
                            side_effect=inject,
                        ), self.assertRaisesRegex(
                            RuntimeError, f"{kind} {stage} {position} fault"
                        ):
                            capability_runner._release_generation_job_accounting(
                                accountant,
                                state,
                                time.monotonic() + 10.0,
                                threading.Lock(),
                            )
                        with mock.patch(
                            "gpu_capability_process_tree._windows_generation_terminal_transition",
                            side_effect=inject,
                        ):
                            capability_runner._release_generation_job_accounting(
                                accountant,
                                state,
                                time.monotonic() + 10.0,
                                threading.Lock(),
                            )
                        self.assertNotIn(key, accountant._generation_jobs)
                        self.assertEqual(job.close_calls, 1)
                        self.assertEqual(job.query_after_close, 0)

    def test_windows_discard_retry_never_queries_after_close_receipt(self):
        from gpu_capability_process_tree import WindowsNativeRunAccountant

        key = (3, 7)
        job = mock.Mock()
        accountant = object.__new__(WindowsNativeRunAccountant)
        accountant._generation_jobs = {key: job}
        accountant._archived_generation_jobs = {}
        accountant._discarded_generation_jobs = set()
        accountant._aggregate_assignment_progress = set()
        accountant._generation_assignment_progress = set()
        accountant._generation_terminal_close_progress = {
            ("discard", *key)
        }
        job.drain_notifications.side_effect = AssertionError(
            "closed Windows Job was queried"
        )
        job.accounting_totals.side_effect = AssertionError(
            "closed Windows Job was queried"
        )
        job.process_ids.side_effect = AssertionError(
            "closed Windows Job was queried"
        )

        accountant.discard_generation_job(*key)

        job.drain_notifications.assert_not_called()
        job.accounting_totals.assert_not_called()
        job.process_ids.assert_not_called()
        job.close.assert_not_called()
        self.assertNotIn(key, accountant._generation_jobs)
        self.assertIn(key, accountant._discarded_generation_jobs)

    @unittest.skipUnless(os.name == "nt", "Windows native accountant proof")
    def test_task9_abort_finalization_retries_pump_without_resealing_windows(self):
        from gpu_capability_process_tree import WindowsNativeRunAccountant

        accountant = WindowsNativeRunAccountant(os.getpid())
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = []
        reactor.cancel_event = threading.Event()
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.native_lifecycle = None
        reactor.run_accountant = accountant
        reactor.archived_generation_telemetry = []
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock(
            side_effect=(RuntimeError("pump once"), None)
        )
        phase_faulted = False

        def after_shutdown(label, _reactor):
            nonlocal phase_faulted
            if label == "task-phase-native-seal" and not phase_faulted:
                phase_faulted = True
                raise RuntimeError("after Windows task phase seal")

        try:
            with mock.patch.object(
                accountant, "seal_phase", wraps=accountant.seal_phase
            ) as seal_phase, mock.patch.object(
                capability_runner,
                "_shutdown_transition",
                side_effect=after_shutdown,
            ):
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "cleanup is incomplete"
                ):
                    reactor.abort_and_reap(time.monotonic() + 10.0)
                self.assertEqual(seal_phase.call_count, 0)
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "cleanup is incomplete"
                ):
                    reactor.abort_and_reap(time.monotonic() + 10.0)
                self.assertEqual(seal_phase.call_count, 1)
                reactor.abort_and_reap(time.monotonic() + 10.0)
                self.assertEqual(seal_phase.call_count, 1)
            self.assertTrue(reactor._closed)
        finally:
            accountant.close()

    def test_task9_abort_detaches_two_generation_publication_and_deferred_state(self):
        def state(index):
            scratch = self.root / f"two-generation-{index}"
            scratch.mkdir()
            process = mock.Mock()
            process.is_alive.return_value = False
            return SimpleNamespace(
                worker_index=index,
                generation=4,
                process=process,
                native_carrier=None,
                pending=None,
                maximum_resident_bytes=index + 1,
                scratch_root=scratch,
                startup_sender=mock.Mock(),
                task_sender=mock.Mock(),
                command_sender=mock.Mock(),
                event_receiver=mock.Mock(),
                payload_receiver=mock.Mock(),
            )

        states = [state(0), state(1)]
        registry = SimpleNamespace(release_generation=mock.Mock())
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = states.copy()
        reactor.cancel_event = threading.Event()
        reactor.publication_requests = __import__("collections").deque(
            ((1, 4, "queued"),)
        )
        reactor.active_publication = (0, 4, "active")
        reactor.deferred_events = __import__("collections").deque(
            ((states[0], object()), (states[1], object()))
        )
        reactor.native_lifecycle = None
        reactor.run_accountant = object()
        reactor.capability_registry = registry
        reactor.registry_generation_duplicates = {
            (0, 4): (object(),), (1, 4): (object(),)
        }
        reactor.archived_generation_telemetry = []
        reactor.archived_scratch_roots = []
        reactor.task_phase_snapshot = None
        reactor._accounting_lock = threading.Lock()
        reactor._stop_accounting_pump = mock.Mock()
        failed = False
        observed = []

        def inject(label, item):
            nonlocal failed
            observed.append((item.worker_index, label))
            if (
                item.worker_index == 0
                and label == "publication-detach"
                and not failed
            ):
                failed = True
                raise RuntimeError("after active publication detach")

        with mock.patch.object(
            capability_runner,
            "_generation_teardown_transition",
            side_effect=inject,
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "cleanup is incomplete"
            ):
                reactor.abort_and_reap(time.monotonic() + 10.0)
            self.assertIs(reactor.states[0], states[0])
            self.assertIsNone(reactor.states[1])
            reactor.abort_and_reap(time.monotonic() + 10.0)

        self.assertTrue(reactor._closed)
        self.assertEqual(reactor.states, [None, None])
        self.assertEqual(reactor.publication_requests, __import__("collections").deque())
        self.assertIsNone(reactor.active_publication)
        self.assertEqual(reactor.deferred_events, __import__("collections").deque())
        self.assertEqual(
            sorted(reactor.archived_generation_telemetry),
            [(0, 4, 1), (1, 4, 2)],
        )
        self.assertEqual(
            registry.release_generation.call_args_list,
            [mock.call(1, 4), mock.call(0, 4)],
        )
        for index in (0, 1):
            for label in {
                name for worker, name in observed if worker == index
            }:
                self.assertEqual(observed.count((index, label)), 1)

    def test_task9_abort_finalization_retries_pump_without_resealing_unix(self):
        linux_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20, 0, 0, 0, 0, 0, 0, 0, True
        )

        class LinuxAttempt(capability_runner.LinuxCompilerAuditAttemptAccountant):
            def __init__(_self):
                _self.seals = []

            def seal_phase(_self, phase, deadline):
                _self.seals.append((phase, deadline))
                return capability_model.LinuxPhaseSnapshot(
                    "linux", phase, linux_memory, 0
                )

            def memory_measurements(_self):
                raise AssertionError("sealed memory must not be reread")

        class LinuxLifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(_self):
                _self.calls = 0

            def seal_task_phase(_self, count):
                _self.calls += 1
                return capability_model.LinuxPhaseSnapshot(
                    "linux", "tasks", linux_memory, count
                )

        accountant = LinuxAttempt()
        lifecycle = LinuxLifecycle()
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = []
        reactor.cancel_event = threading.Event()
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.native_lifecycle = lifecycle
        reactor.run_accountant = accountant
        reactor.archived_generation_telemetry = []
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock(
            side_effect=(RuntimeError("pump once"), None)
        )
        phase_faulted = False

        def after_shutdown(label, _reactor):
            nonlocal phase_faulted
            if label == "task-phase-native-seal" and not phase_faulted:
                phase_faulted = True
                raise RuntimeError("after Unix task phase seal")

        with mock.patch.object(
            capability_runner,
            "_shutdown_transition",
            side_effect=after_shutdown,
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "cleanup is incomplete"
            ):
                reactor.abort_and_reap(time.monotonic() + 10.0)
            self.assertEqual(lifecycle.calls, 0)
            self.assertEqual(accountant.seals, [])
            with self.assertRaisesRegex(
                AuditInfrastructureError, "cleanup is incomplete"
            ):
                reactor.abort_and_reap(time.monotonic() + 10.0)
            self.assertEqual(lifecycle.calls, 1)
            self.assertEqual(len(accountant.seals), 1)
            reactor.abort_and_reap(time.monotonic() + 10.0)
        self.assertTrue(reactor._closed)
        self.assertEqual(lifecycle.calls, 1)
        self.assertEqual(len(accountant.seals), 1)

    @unittest.skipUnless(os.name == "nt", "Windows native accountant proof")
    def test_task9_windows_phase_substeps_retry_without_resealing(self):
        from gpu_capability_process_tree import (
            WindowsGenerationPeak,
            WindowsJobAccountingSnapshot,
            WindowsNativeRunAccountant,
            WindowsSlotPeak,
        )

        raw = WindowsJobAccountingSnapshot(
            parent_peak_rss_bytes=0,
            maximum_simultaneous_working_set_bytes=0,
            inspection_tree_peak_bytes=(),
            slots=(WindowsSlotPeak((WindowsGenerationPeak(0, ()),)),),
            job_peak_commit_charge_bytes=0,
            job_total_process_count=1,
            retained_process_identity_count=1,
            retained_inspection_process_identity_count=0,
            surviving_job_process_count=0,
            archived_generation_identities=((0, 0),),
        )
        original_memory = WindowsJobAccountingSnapshot.memory_measurements
        class RawSnapshotSubclass(WindowsJobAccountingSnapshot):
            pass

        subclass_raw = RawSnapshotSubclass(
            raw.parent_peak_rss_bytes,
            raw.maximum_simultaneous_working_set_bytes,
            raw.inspection_tree_peak_bytes,
            raw.slots,
            raw.job_peak_commit_charge_bytes,
            raw.job_total_process_count,
            raw.retained_process_identity_count,
            raw.retained_inspection_process_identity_count,
            raw.surviving_job_process_count,
            raw.archived_generation_identities,
            raw.accounting_complete,
        )

        for target in (
            "snapshot", "snapshot-type", "snapshot-subclass",
            "memory", "identities", "typed",
        ):
            with self.subTest(target=target):
                class Accountant(WindowsNativeRunAccountant):
                    def __init__(_self):
                        _self.seal_calls = 0
                        _self.snapshot_calls = 0

                    def seal_phase(_self, _deadline, _parent_rss=0):
                        _self.seal_calls += 1

                    def snapshot(_self):
                        _self.snapshot_calls += 1
                        if target == "snapshot" and _self.snapshot_calls == 1:
                            raise RuntimeError("snapshot once")
                        if target == "snapshot-type" and _self.snapshot_calls == 1:
                            return SimpleNamespace()
                        if (
                            target == "snapshot-subclass"
                            and _self.snapshot_calls == 1
                        ):
                            return subclass_raw
                        return raw

                accountant = Accountant()
                reactor = object.__new__(capability_runner.GenerationReactor)
                reactor._closed = False
                reactor.states = []
                reactor.cancel_event = threading.Event()
                reactor.publication_requests = __import__("collections").deque()
                reactor.active_publication = None
                reactor.deferred_events = __import__("collections").deque()
                reactor.native_lifecycle = None
                reactor.run_accountant = accountant
                reactor.archived_generation_telemetry = [(0, 0, 0)]
                reactor.task_phase_snapshot = None
                reactor._stop_accounting_pump = mock.Mock()
                memory_calls = 0
                identity_calls = 0
                typed_calls = 0

                def memory_once(item):
                    nonlocal memory_calls
                    memory_calls += 1
                    if target == "memory" and memory_calls == 1:
                        raise RuntimeError("memory once")
                    return original_memory(item)

                def identities_once(actual, expected, phase):
                    nonlocal identity_calls
                    identity_calls += 1
                    if target == "identities" and identity_calls == 1:
                        raise RuntimeError("identities once")
                    if phase != "tasks" or actual != expected:
                        raise AuditInfrastructureError(
                            "Windows tasks phase generation identities differ"
                        )

                def typed_once(platform, phase, phase_memory, generations):
                    nonlocal typed_calls
                    typed_calls += 1
                    if target == "typed" and typed_calls == 1:
                        raise RuntimeError("typed once")
                    self.assertEqual(platform, "windows")
                    return capability_model.WindowsPhaseSnapshot(
                        platform, phase, phase_memory, generations
                    )

                with mock.patch.object(
                    WindowsJobAccountingSnapshot,
                    "memory_measurements",
                    new=memory_once,
                ), mock.patch.object(
                    capability_runner,
                    "_validate_windows_phase_generation_identities",
                    side_effect=identities_once,
                    create=True,
                ), mock.patch.object(
                    capability_runner,
                    "_construct_task_phase_snapshot",
                    side_effect=typed_once,
                    create=True,
                ):
                    with self.assertRaisesRegex(
                        AuditInfrastructureError, "cleanup is incomplete"
                    ):
                        reactor.abort_and_reap(time.monotonic() + 10.0)
                    self.assertEqual(accountant.seal_calls, 1)
                    reactor.abort_and_reap(time.monotonic() + 10.0)
                self.assertTrue(reactor._closed)
                self.assertEqual(accountant.seal_calls, 1)

    def test_task9_unix_phase_substeps_retry_without_resealing(self):
        cases = (
            (
                "linux",
                capability_model.LinuxRunMemoryMeasurements(
                    1, 2, 512 << 20, 448 << 20,
                    0, 0, 0, 0, 0, 0, 0, True,
                ),
                capability_runner.LinuxCompilerAuditAttemptAccountant,
                capability_runner._LinuxGenerationLifecycle,
                capability_runner.LinuxPhaseSnapshot,
            ),
            (
                "macos",
                capability_model.MacOSRunMemoryMeasurements(
                    1, 1, 1, 0, 0, True
                ),
                __import__(
                    "gpu_capability_process_tree"
                ).MacOSCompilerAuditAttemptAccountant,
                capability_runner._MacOSGenerationLifecycle,
                capability_runner.MacOSPhaseSnapshot,
            ),
        )
        for platform, memory, attempt_base, lifecycle_base, typed_class in cases:
            for target in (
                "lifecycle-type", "native-type", "exception",
                "memory", "typed", "type-conversion",
            ):
                with self.subTest(platform=platform, target=target):
                    lifecycle_snapshot = typed_class(
                        platform, "tasks", memory, 0
                    )
                    attempt_memory = memory
                    if target == "memory":
                        field = dataclasses.fields(memory)[0].name
                        attempt_memory = dataclasses.replace(
                            memory,
                            **{field: getattr(memory, field) + 1},
                        )
                    attempt_snapshot = typed_class(
                        platform, "tasks", attempt_memory, 0
                    )
                    lifecycle_result = (
                        SimpleNamespace()
                        if target == "lifecycle-type"
                        else lifecycle_snapshot
                    )
                    attempt_result = (
                        SimpleNamespace()
                        if target == "native-type"
                        else attempt_snapshot
                    )

                    class Attempt(attempt_base):
                        def __init__(_self):
                            _self.seal_calls = 0

                        def seal_phase(_self, phase, _deadline):
                            self.assertEqual(phase, "tasks")
                            _self.seal_calls += 1
                            return attempt_result

                        def memory_measurements(_self):
                            raise AssertionError(
                                "sealed Unix memory must not be reread"
                            )

                    class Lifecycle(lifecycle_base):
                        def __init__(_self):
                            _self.calls = 0

                        def seal_task_phase(_self, count):
                            self.assertEqual(count, 0)
                            _self.calls += 1
                            return lifecycle_result

                    accountant = Attempt()
                    lifecycle = Lifecycle()
                    reactor = object.__new__(capability_runner.GenerationReactor)
                    reactor._closed = False
                    reactor.states = []
                    reactor.cancel_event = threading.Event()
                    reactor.publication_requests = __import__("collections").deque()
                    reactor.active_publication = None
                    reactor.deferred_events = __import__("collections").deque()
                    reactor.native_lifecycle = lifecycle
                    reactor.run_accountant = accountant
                    reactor.archived_generation_telemetry = []
                    reactor.task_phase_snapshot = None
                    reactor._stop_accounting_pump = mock.Mock()
                    typed_calls = 0
                    faulted = False
                    def typed_once(
                        actual_platform, phase, phase_memory, generations
                    ):
                        nonlocal typed_calls
                        typed_calls += 1
                        if target == "typed" and typed_calls == 1:
                            raise RuntimeError("typed once")
                        if target == "type-conversion":
                            return SimpleNamespace()
                        self.assertEqual(actual_platform, platform)
                        return typed_class(
                            platform, phase, phase_memory, generations
                        )

                    def after_transition(label, _reactor):
                        nonlocal faulted
                        if (
                            target == "exception"
                            and label == "lifecycle-task-snapshot"
                            and not faulted
                        ):
                            faulted = True
                            raise RuntimeError("after lifecycle seal")

                    with mock.patch.object(
                        capability_runner,
                        "_construct_task_phase_snapshot",
                        side_effect=typed_once,
                        create=True,
                    ), mock.patch.object(
                        capability_runner,
                        "_shutdown_transition",
                        side_effect=after_transition,
                    ):
                        with self.assertRaisesRegex(
                            AuditInfrastructureError, "cleanup is incomplete"
                        ):
                            reactor.abort_and_reap(time.monotonic() + 10.0)
                        if target in {
                            "lifecycle-type", "native-type", "memory",
                            "type-conversion",
                        }:
                            with self.assertRaisesRegex(
                                AuditInfrastructureError,
                                "cleanup is incomplete",
                            ):
                                reactor.abort_and_reap(
                                    time.monotonic() + 10.0
                                )
                        else:
                            reactor.abort_and_reap(time.monotonic() + 10.0)
                    self.assertEqual(lifecycle.calls, 1)
                    self.assertIs(
                        reactor._lifecycle_task_phase_snapshot,
                        lifecycle_result,
                    )
                    if target == "lifecycle-type":
                        self.assertEqual(accountant.seal_calls, 0)
                    else:
                        self.assertEqual(accountant.seal_calls, 1)
                        self.assertIs(
                            reactor._run_accountant_task_phase_snapshot,
                            attempt_result,
                        )
                    self.assertEqual(
                        reactor._closed,
                        target in {"exception", "typed"},
                    )

    def test_task9_unix_finalization_retains_exact_native_seal_objects(self):
        memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20,
            0, 0, 0, 0, 0, 0, 0, True,
        )
        lifecycle_snapshot = capability_model.LinuxPhaseSnapshot(
            "linux", "tasks", memory, 0
        )
        attempt_snapshot = capability_model.LinuxPhaseSnapshot(
            "linux", "tasks", memory, 0
        )

        class Attempt(capability_runner.LinuxCompilerAuditAttemptAccountant):
            def __init__(_self):
                _self.seal_calls = 0

            def seal_phase(_self, phase, _deadline):
                self.assertEqual(phase, "tasks")
                _self.seal_calls += 1
                return attempt_snapshot

            def memory_measurements(_self):
                raise AssertionError("sealed Unix memory must not be reread")

        class Lifecycle(capability_runner._LinuxGenerationLifecycle):
            def __init__(_self):
                _self.calls = 0

            def seal_task_phase(_self, count):
                self.assertEqual(count, 0)
                _self.calls += 1
                return lifecycle_snapshot

        accountant = Attempt()
        lifecycle = Lifecycle()
        reactor = object.__new__(capability_runner.GenerationReactor)
        reactor._closed = False
        reactor.states = []
        reactor.cancel_event = threading.Event()
        reactor.publication_requests = __import__("collections").deque()
        reactor.active_publication = None
        reactor.deferred_events = __import__("collections").deque()
        reactor.native_lifecycle = lifecycle
        reactor.run_accountant = accountant
        reactor.archived_generation_telemetry = []
        reactor.task_phase_snapshot = None
        reactor._stop_accounting_pump = mock.Mock()
        faulted = False

        def fault_after_native_seal(label, _reactor):
            nonlocal faulted
            if label == "task-phase-native-seal" and not faulted:
                faulted = True
                raise RuntimeError("after native seal")

        with mock.patch.object(
            capability_runner,
            "_shutdown_transition",
            side_effect=fault_after_native_seal,
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "cleanup is incomplete"
            ):
                reactor.abort_and_reap(time.monotonic() + 10.0)
            reactor.abort_and_reap(time.monotonic() + 10.0)

        self.assertTrue(reactor._closed)
        self.assertEqual(lifecycle.calls, 1)
        self.assertEqual(accountant.seal_calls, 1)
        self.assertIs(
            reactor._lifecycle_task_phase_snapshot, lifecycle_snapshot
        )
        self.assertIs(
            reactor._run_accountant_task_phase_snapshot, attempt_snapshot
        )
        self.assertIs(reactor._task_phase_raw_snapshot, attempt_snapshot)

    def test_task9_macos_phase_seal_retries_after_survivors_clear(self):
        from gpu_capability_process_tree import MacOSCompilerAuditAttemptAccountant

        survivor = capability_model.MacOSRunMemoryMeasurements(
            1, 1, 1, 1, 1, False
        )
        complete = capability_model.MacOSRunMemoryMeasurements(
            1, 1, 1, 0, 0, True
        )

        class Attempt(MacOSCompilerAuditAttemptAccountant):
            def __init__(_self):
                _self._phase = "tasks"
                _self._sealed = {"inspection"}
                _self._sealed_snapshots = {}
                _self._active_carriers = {}
                _self._pending_inspection = None
                _self._inspection_pgids = set()
                _self.measurements = iter((survivor, complete))

            def memory_measurements(_self):
                return next(_self.measurements)

        accountant = Attempt()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "survivors"
        ):
            accountant.seal_phase("tasks", time.monotonic() + 10.0)
        self.assertNotIn("tasks", accountant._sealed)
        snapshot = accountant.seal_phase("tasks", time.monotonic() + 10.0)
        self.assertIn("tasks", accountant._sealed)
        self.assertIs(
            accountant.seal_phase("tasks", time.monotonic() + 10.0),
            snapshot,
        )

    def test_task9_linux_phase_seal_returns_exact_sticky_snapshot(self):
        memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20,
            0, 0, 0, 0, 0, 0, 0, True,
        )
        accountant = object.__new__(
            capability_runner.LinuxCompilerAuditAttemptAccountant
        )
        accountant._phase = "tasks"
        accountant._sealed = {"inspection"}
        accountant._sealed_snapshots = {}
        accountant._active_carriers = {}
        accountant._pending_inspection = None
        accountant.memory_measurements = mock.Mock(return_value=memory)
        first = accountant.seal_phase("tasks", time.monotonic() + 10.0)
        second = accountant.seal_phase("tasks", time.monotonic() + 10.0)
        self.assertIs(second, first)
        self.assertEqual(accountant.memory_measurements.call_count, 1)

    def test_task9_active_unix_phase_seal_is_exact_and_never_rereads_memory(self):
        from gpu_capability_process_tree import MacOSCompilerAuditAttemptAccountant

        linux_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20,
            0, 0, 0, 0, 0, 0, 0, True,
        )
        macos_memory = capability_model.MacOSRunMemoryMeasurements(
            1, 1, 1, 0, 0, True
        )
        cases = (
            (
                "linux", capability_runner.LinuxCompilerAuditAttemptAccountant,
                capability_model.LinuxPhaseSnapshot, linux_memory,
                capability_model.MacOSPhaseSnapshot(
                    "macos", "inspection", macos_memory, 0
                ),
            ),
            (
                "macos", MacOSCompilerAuditAttemptAccountant,
                capability_model.MacOSPhaseSnapshot, macos_memory,
                capability_model.LinuxPhaseSnapshot(
                    "linux", "inspection", linux_memory, 0
                ),
            ),
        )
        for platform, base, snapshot_type, memory, cross_platform in cases:
            correct = snapshot_type(platform, "inspection", memory, 0)

            class SnapshotSubclass(snapshot_type):
                pass

            wrong_values = (
                object(),
                cross_platform,
                snapshot_type(platform, "tasks", memory, 0),
                snapshot_type(platform, "inspection", memory, 1),
                SnapshotSubclass(platform, "inspection", memory, 0),
            )
            for returned in (correct, *wrong_values):
                with self.subTest(
                    platform=platform, returned=type(returned).__name__
                ):
                    class Attempt(base):
                        def __init__(_self):
                            _self.seal_calls = 0
                            _self.memory_calls = 0

                        def seal_phase(_self, phase, _deadline):
                            self.assertEqual(phase, "inspection")
                            _self.seal_calls += 1
                            return returned

                        def memory_measurements(_self):
                            _self.memory_calls += 1
                            raise AssertionError(
                                "sealed Unix memory must not be reread"
                            )

                    accountant = Attempt()
                    if returned is correct:
                        actual = capability_runner._seal_accountant_phase(
                            accountant,
                            "inspection",
                            time.monotonic() + 10.0,
                            platform,
                        )
                        self.assertIs(actual, correct)
                    else:
                        with self.assertRaisesRegex(
                            AuditInfrastructureError, "snapshot differs"
                        ):
                            capability_runner._seal_accountant_phase(
                                accountant,
                                "inspection",
                                time.monotonic() + 10.0,
                                platform,
                            )
                    self.assertEqual(accountant.seal_calls, 1)
                    self.assertEqual(accountant.memory_calls, 0)

            task = snapshot_type(platform, "tasks", memory, 3)
            self.assertIs(
                capability_runner._final_memory_from_sealed_phases(
                    correct, task, platform
                ),
                memory,
            )

    def test_unix_attempt_seal_snapshot_is_the_atomic_commit_record(self):
        from gpu_capability_process_tree import (
            MacOSCompilerAuditAttemptAccountant,
        )

        linux_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20,
            0, 0, 0, 0, 0, 0, 0, True,
        )
        macos_memory = capability_model.MacOSRunMemoryMeasurements(
            1, 1, 1, 0, 0, True
        )
        for platform, accountant_type, memory, module in (
            (
                "linux",
                capability_runner.LinuxCompilerAuditAttemptAccountant,
                linux_memory,
                capability_runner,
            ),
            (
                "macos",
                MacOSCompilerAuditAttemptAccountant,
                macos_memory,
                __import__("gpu_capability_process_tree"),
            ),
        ):
            for stage in ("snapshot", "sealed"):
                for position in ("before", "after"):
                    with self.subTest(
                        platform=platform, stage=stage, position=position
                    ):
                        accountant = object.__new__(accountant_type)
                        accountant._phase = "tasks"
                        accountant._sealed = {"inspection"}
                        accountant._sealed_snapshots = {}
                        accountant._active_carriers = {}
                        accountant._pending_inspection = None
                        accountant.memory_measurements = mock.Mock(
                            return_value=memory
                        )
                        faulted = False

                        def inject(actual_platform, actual_stage,
                                   actual_position, phase):
                            nonlocal faulted
                            if (
                                (actual_platform, actual_stage, actual_position)
                                == (platform, stage, position)
                                and phase == "tasks"
                                and not faulted
                            ):
                                faulted = True
                                raise RuntimeError(
                                    f"{platform} {stage} {position}"
                                )

                        with mock.patch.object(
                            module,
                            "_attempt_phase_seal_transition",
                            side_effect=inject,
                            create=True,
                        ), self.assertRaisesRegex(
                            RuntimeError, f"{platform} {stage} {position}"
                        ):
                            accountant.seal_phase(
                                "tasks", time.monotonic() + 10.0
                            )
                        self.assertFalse(
                            "tasks" in accountant._sealed
                            and "tasks" not in accountant._sealed_snapshots
                        )
                        if "tasks" in accountant._sealed_snapshots:
                            with self.assertRaises(AuditInfrastructureError):
                                accountant.begin_phase(
                                    "tasks", time.monotonic() + 10.0
                                )
                        with mock.patch.object(
                            module,
                            "_attempt_phase_seal_transition",
                            side_effect=inject,
                            create=True,
                        ):
                            snapshot = accountant.seal_phase(
                                "tasks", time.monotonic() + 10.0
                            )
                        self.assertIs(
                            snapshot, accountant._sealed_snapshots["tasks"]
                        )
                        self.assertIn("tasks", accountant._sealed)

    def test_task9_dead_unix_reseal_helper_is_absent(self):
        self.assertFalse(
            hasattr(capability_runner, "_seal_unix_attempt_task_phase")
        )

    @unittest.skipUnless(os.name == "nt", "Windows native accountant proof")
    def test_task9_windows_phase_rejects_equal_count_generation_mismatch(self):
        from gpu_capability_process_tree import (
            WindowsGenerationPeak,
            WindowsJobAccountingSnapshot,
            WindowsNativeRunAccountant,
            WindowsSlotPeak,
        )

        self.assertIn(
            "archived_generation_identities",
            {field.name for field in dataclasses.fields(WindowsJobAccountingSnapshot)},
        )

        class Mismatched(WindowsNativeRunAccountant):
            def __init__(_self):
                pass

            def seal_phase(_self, _deadline, _parent_rss=0):
                pass

            def snapshot(_self):
                return WindowsJobAccountingSnapshot(
                    parent_peak_rss_bytes=0,
                    maximum_simultaneous_working_set_bytes=0,
                    inspection_tree_peak_bytes=(),
                    slots=(
                        WindowsSlotPeak((WindowsGenerationPeak(0, ()),)),
                    ),
                    job_peak_commit_charge_bytes=0,
                    job_total_process_count=1,
                    retained_process_identity_count=1,
                    retained_inspection_process_identity_count=0,
                    surviving_job_process_count=0,
                    archived_generation_identities=((0, 1),),
                )

        with self.assertRaisesRegex(
            AuditInfrastructureError, "generation identities differ"
        ):
            capability_runner._seal_windows_accountant_phase(
                Mismatched(),
                "tasks",
                time.monotonic() + 10.0,
                ((0, 2),),
            )

    def test_task9_generic_windows_phase_has_no_scalar_identity_branch(self):
        parameter = inspect.signature(
            capability_runner._phase_snapshot
        ).parameters
        self.assertIn("archived_generation_count", parameter)
        memory = capability_model.WindowsRunMemoryMeasurements(
            1, 1, 1, 1, 1, 0, 0, 0, 0, 0, True
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "backend differs"
        ):
            capability_runner._phase_snapshot(
                memory, "windows", "tasks", 1
            )
        snapshot = capability_runner._construct_task_phase_snapshot(
            "windows", "tasks", memory, ((2, 3),)
        )
        self.assertEqual(
            snapshot.archived_generation_identities, ((2, 3),)
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "canonical|identities"
        ):
            capability_runner._construct_task_phase_snapshot(
                "windows", "tasks", memory, 1
            )

    def test_registry_transfers_exact_generation_duplicates_and_ack_closes_them(self):
        registry = capability_runner.CompilerCapabilityRegistry(self.authority)
        digest = registry.register(self.capability)
        duplicate = registry.duplicate_for_generation(digest, 2, 7)
        self.assertEqual(
            tuple(os.fstat(stream.fileno())[:4] for stream in duplicate.streams),
            tuple(
                os.fstat(stream.fileno())[:4]
                for stream in self.capability.native_owner.streams
            ),
        )
        registry.transfer_duplicate_to_child(
            duplicate, SimpleNamespace(pid=os.getpid())
        )
        registry.acknowledge_generation(2, 7, (digest,))
        self.assertTrue(duplicate.closed)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "acknowledged|closed|generation"
        ):
            registry.acknowledge_generation(2, 7, (digest,))
        registry.release_generation(2, 7)
        registry.close()

    def test_registry_release_retries_each_duplicate_stream_close(self):
        class Stream:
            def __init__(_self):
                _self.calls = 0
                _self.closed = False

            def close(_self):
                _self.calls += 1
                _self.closed = True

        for target_index in range(3):
            for position in ("before", "after"):
                with self.subTest(
                    target_index=target_index, position=position
                ):
                    registry = capability_runner.CompilerCapabilityRegistry(
                        self.authority
                    )
                    digest = registry.register(self.capability)
                    duplicate = registry.duplicate_for_generation(
                        digest, 4, target_index
                    )
                    for stream in duplicate.streams:
                        stream.close()
                    streams = tuple(Stream() for _ in range(3))
                    duplicate.streams = streams
                    key = (4, target_index)
                    faulted = False

                    def fail(actual_duplicate, stream_index):
                        nonlocal faulted
                        if (
                            actual_duplicate is duplicate
                            and stream_index == target_index
                            and not faulted
                        ):
                            faulted = True
                            raise RuntimeError(
                                f"{position} stream {stream_index}"
                            )

                    before = fail if position == "before" else mock.Mock()
                    after = fail if position == "after" else mock.Mock()
                    with mock.patch.object(
                        capability_runner,
                        "_generation_duplicate_stream_before_close",
                        side_effect=before,
                        create=True,
                    ), mock.patch.object(
                        capability_runner,
                        "_generation_duplicate_stream_close",
                        side_effect=after,
                        create=True,
                    ):
                        with self.assertRaisesRegex(
                            AuditInfrastructureError, "release failed"
                        ):
                            registry.release_generation(*key)
                        self.assertIs(
                            registry._generation_duplicates[key][digest],
                            duplicate,
                        )
                        self.assertNotIn(key, registry._released_generations)
                        registry.release_generation(*key)

                    self.assertNotIn(key, registry._generation_duplicates)
                    self.assertIn(key, registry._released_generations)
                    self.assertTrue(duplicate.closed)
                    self.assertEqual(
                        duplicate._closed_stream_indices, {0, 1, 2}
                    )
                    self.assertTrue(all(stream.closed for stream in streams))
                    self.assertTrue(all(stream.calls == 1 for stream in streams))

    def test_registry_terminal_publication_precedes_live_state_removal(self):
        for operation in ("acknowledge", "release"):
            for stage in ("terminal", "remove"):
                for position in ("before", "after"):
                    with self.subTest(
                        operation=operation, stage=stage, position=position
                    ):
                        registry = capability_runner.CompilerCapabilityRegistry(
                            self.authority
                        )
                        capability = open_compiler_executable_capability(
                            self.compiler,
                            self.authority,
                            time.monotonic() + 10.0,
                            compiler_family=CompilerFamily.GCC,
                        )
                        digest = registry.register(capability)
                        duplicate = registry.duplicate_for_generation(
                            digest, 6, 7
                        )
                        key = (6, 7)
                        if operation == "acknowledge":
                            registry.transfer_duplicate_to_child(
                                duplicate, SimpleNamespace(pid=os.getpid())
                            )
                        faulted = False

                        def inject(kind, actual_stage, actual_position,
                                   worker, generation):
                            nonlocal faulted
                            if (
                                (kind, actual_stage, actual_position)
                                == (operation, stage, position)
                                and (worker, generation) == key
                                and not faulted
                            ):
                                faulted = True
                                raise RuntimeError(
                                    f"{operation} {stage} {position}"
                                )

                        def invoke():
                            if operation == "acknowledge":
                                return registry.acknowledge_generation(
                                    *key, (digest,)
                                )
                            return registry.release_generation(*key)

                        with mock.patch.object(
                            capability_runner,
                            "_generation_release_transition",
                            side_effect=inject,
                            create=True,
                        ), self.assertRaisesRegex(
                            RuntimeError, f"{operation} {stage} {position}"
                        ):
                            invoke()
                        terminal = (
                            key in registry._acknowledged_generations
                            if operation == "acknowledge"
                            else key in registry._released_generations
                        )
                        self.assertEqual(
                            terminal,
                            stage == "remove" or position == "after",
                        )
                        if not (stage == "remove" and position == "after"):
                            self.assertIn(key, registry._generation_duplicates)
                        with mock.patch.object(
                            capability_runner,
                            "_generation_release_transition",
                            side_effect=inject,
                            create=True,
                        ):
                            invoke()
                        self.assertNotIn(key, registry._generation_duplicates)
                        self.assertTrue(duplicate.closed)
                        if operation == "acknowledge":
                            registry.release_generation(*key)
                        registry.close()

    def test_registry_marker_add_failure_retains_live_state_for_retry(self):
        class FailingSet(set):
            def __init__(_self, position):
                super().__init__()
                _self.position = position
                _self.failed = False

            def add(_self, value):
                if not _self.failed and _self.position == "before":
                    _self.failed = True
                    raise RuntimeError("marker before")
                super().add(value)
                if not _self.failed:
                    _self.failed = True
                    raise RuntimeError("marker after")

        for operation in ("acknowledge", "release"):
            for position in ("before", "after"):
                with self.subTest(operation=operation, position=position):
                    capability = open_compiler_executable_capability(
                        self.compiler, self.authority, time.monotonic() + 10.0,
                        compiler_family=CompilerFamily.GCC,
                    )
                    registry = capability_runner.CompilerCapabilityRegistry(
                        self.authority
                    )
                    digest = registry.register(capability)
                    duplicate = registry.duplicate_for_generation(digest, 8, 9)
                    key = (8, 9)
                    if operation == "acknowledge":
                        registry.transfer_duplicate_to_child(
                            duplicate, SimpleNamespace(pid=os.getpid())
                        )
                        registry._acknowledged_generations = FailingSet(position)
                        invoke = lambda: registry.acknowledge_generation(
                            *key, (digest,)
                        )
                    else:
                        registry._released_generations = FailingSet(position)
                        invoke = lambda: registry.release_generation(*key)
                    with self.assertRaisesRegex(RuntimeError, "marker"):
                        invoke()
                    self.assertIn(key, registry._generation_duplicates)
                    invoke()
                    self.assertNotIn(key, registry._generation_duplicates)
                    if operation == "acknowledge":
                        registry.release_generation(*key)
                    registry.close()

    def test_registry_close_rejects_live_acknowledged_generation(self):
        registry = capability_runner.CompilerCapabilityRegistry(self.authority)
        digest = registry.register(self.capability)
        duplicate = registry.duplicate_for_generation(digest, 2, 8)
        registry.transfer_duplicate_to_child(
            duplicate, SimpleNamespace(pid=os.getpid())
        )
        registry.acknowledge_generation(2, 8, (digest,))
        with self.assertRaisesRegex(AuditInfrastructureError, "live generations"):
            registry.close()
        registry.release_generation(2, 8)
        registry.close()

    @unittest.skipUnless(os.name == "nt", "Windows locked-name capability")
    def test_windows_registry_lease_blocks_all_path_mutation_until_reap(self):
        registry = capability_runner.CompilerCapabilityRegistry(self.authority)
        digest = registry.register(self.capability)
        duplicate = registry.duplicate_for_generation(digest, 3, 1)
        registry.transfer_duplicate_to_child(
            duplicate, SimpleNamespace(pid=os.getpid())
        )
        registry.acknowledge_generation(3, 1, (digest,))
        replacement = self.compiler.with_name("replacement.exe")
        replacement.write_bytes(self.compiler.read_bytes())
        renamed = self.compiler.with_name("compiler-renamed.exe")
        try:
            with self.assertRaises(OSError):
                self.compiler.write_bytes(b"replacement")
            with self.assertRaises(OSError):
                self.compiler.rename(renamed)
            with self.assertRaises(OSError):
                self.compiler.unlink()
            with self.assertRaises(OSError):
                os.replace(replacement, self.compiler)
            registry.release_generation(3, 1)
            registry.close()
            self.compiler.rename(renamed)
            self.assertTrue(renamed.is_file())
        finally:
            if not registry._closed:
                registry.release_generation(3, 1)
                registry.close()
            replacement.unlink(missing_ok=True)

    def _retarget_bootstrap_document_platform(self, document, platform_kind):
        closure_digest = capability_command._runtime_closure_digest(
            self.capability.resolved_runtime_closure,
            platform_kind,
            "",
        )
        capability_digest = capability_command._compiler_capability_digest(
            platform_kind,
            self.capability.trusted_toolchain_root,
            self.capability.executable_identity.canonical,
            self.capability.executable_sha256,
            closure_digest,
            self.authority.portable_authority_digest,
        )
        document["platform_kind"] = platform_kind
        document["resolved_runtime_closure_digest"] = closure_digest
        document["digest"] = capability_digest

    def test_posix_capability_socket_type_is_supported_per_platform(self):
        with mock.patch.object(capability_runner.sys, "platform", "darwin"):
            self.assertEqual(
                capability_runner._posix_capability_socket_type(),
                socket.SOCK_DGRAM,
            )
        with mock.patch.object(capability_runner.sys, "platform", "linux"):
            self.assertEqual(
                capability_runner._posix_capability_socket_type(),
                socket.SOCK_SEQPACKET,
            )
        with mock.patch.object(
            capability_runner.sys,
            "platform",
            "freebsd",
        ), self.assertRaisesRegex(
            AuditInfrastructureError,
            "platform is unsupported",
        ):
            capability_runner._posix_capability_socket_type()

    def test_posix_capability_transfer_rejects_stream_socket(self):
        first, second = socket.socketpair()
        try:
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "not record-oriented",
            ):
                capability_runner._validate_posix_capability_socket(first)
        finally:
            first.close()
            second.close()

    def test_worker_failure_preempts_native_lifecycle_sampling(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        failure = capability_model.WorkerFailure(
            0,
            1,
            None,
            "worker bootstrap diagnostic",
        )
        receiver = mock.Mock()
        receiver.poll.return_value = True
        receiver.receive_bytes_before.return_value = (
            capability_runner.encode_control_message(failure)
        )
        process = mock.Mock(pid=1234)
        process.is_alive.return_value = False
        state = SimpleNamespace(
            worker_index=0,
            generation=1,
            event_receiver=receiver,
            process=process,
            pending=None,
            contained=True,
        )
        reactor.runtime_contract = SimpleNamespace(
            pipeline_deadline=time.monotonic() + 10.0
        )
        reactor.states = [state]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.cancel_event = threading.Event()
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(
            side_effect=missing
        )

        observed_state, observed_event = reactor._next_event_raw()

        self.assertIs(observed_state, state)
        self.assertEqual(observed_event, failure)
        reactor._sample_owned_forest.assert_not_called()

    def test_macos_session_then_failure_preempts_lifecycle_sampling(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        session = capability_model.MacOSWorkerSessionReported(
            0,
            1,
            1234,
            1234,
            "1:2",
        )
        failure = capability_model.WorkerFailure(
            0,
            1,
            None,
            "worker bootstrap diagnostic",
        )
        receiver = mock.Mock()
        receiver.poll.return_value = True
        receiver.receive_bytes_before.side_effect = (
            capability_runner.encode_control_message(session),
            capability_runner.encode_control_message(failure),
        )
        state = SimpleNamespace(
            worker_index=0,
            generation=1,
            event_receiver=receiver,
            process=mock.Mock(pid=1234),
            pending=None,
            contained=False,
        )
        reactor.runtime_contract = SimpleNamespace(
            pipeline_deadline=time.monotonic() + 10.0
        )
        reactor.states = [state]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {}
        reactor.cancel_event = threading.Event()
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(side_effect=missing)

        with mock.patch.object(
            capability_runner._MacOSGenerationLifecycle,
            "accept_worker_session",
        ) as accept_session:
            first_state, first_event = reactor._next_event_raw()
            second_state, second_event = reactor._next_event_raw()

        self.assertIs(first_state, state)
        self.assertEqual(first_event, session)
        self.assertIs(second_state, state)
        self.assertEqual(second_event, failure)
        accept_session.assert_called_once_with(session)
        self.assertTrue(state.contained)
        self.assertTrue(state.native_generation_acquired)
        reactor._sample_owned_forest.assert_called_once_with()

    def test_worker_stopped_preempts_native_lifecycle_sampling(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        stopped = capability_model.WorkerStopped(0, 1)
        receiver = mock.Mock()
        receiver.poll.return_value = True
        receiver.receive_bytes_before.return_value = (
            capability_runner.encode_control_message(stopped)
        )
        process = mock.Mock(pid=1234)
        process.is_alive.return_value = False
        state = SimpleNamespace(
            worker_index=0,
            generation=1,
            event_receiver=receiver,
            process=process,
            pending=None,
            contained=True,
        )
        reactor.runtime_contract = SimpleNamespace(
            pipeline_deadline=time.monotonic() + 10.0
        )
        reactor.states = [state]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {}
        reactor.cancel_event = threading.Event()
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(
            side_effect=missing
        )

        observed_state, observed_event = reactor._next_event_raw()

        self.assertIs(observed_state, state)
        self.assertEqual(observed_event, stopped)
        reactor._sample_owned_forest.assert_called_once_with()

    def test_nonterminal_event_then_failure_preempts_macos_sampling(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        accepted = capability_model.WorkerCapabilitiesAccepted(
            0,
            1,
            ("a" * 64,),
        )
        failure = capability_model.WorkerFailure(
            0,
            1,
            None,
            "worker post-event diagnostic",
        )
        receiver = mock.Mock()
        receiver.poll.return_value = True
        receiver.receive_bytes_before.side_effect = (
            capability_runner.encode_control_message(accepted),
            capability_runner.encode_control_message(failure),
        )
        process = mock.Mock(pid=1234)
        process.is_alive.return_value = False
        state = SimpleNamespace(
            worker_index=0,
            generation=1,
            event_receiver=receiver,
            process=process,
            pending=None,
            contained=True,
        )
        reactor.runtime_contract = SimpleNamespace(
            pipeline_deadline=time.monotonic() + 10.0
        )
        reactor.states = [state]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {}
        reactor.cancel_event = threading.Event()
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(side_effect=missing)

        first_state, first_event = reactor._next_event_raw()
        second_state, second_event = reactor._next_event_raw()

        self.assertIs(first_state, state)
        self.assertEqual(first_event, accepted)
        self.assertIs(second_state, state)
        self.assertEqual(second_event, failure)
        reactor._sample_owned_forest.assert_called_once_with()

    def test_macos_missing_leader_race_defers_for_queued_transition(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        receiver = mock.Mock()
        receiver.poll.return_value = True
        process = mock.Mock()
        process.is_alive.return_value = True
        state = SimpleNamespace(
            worker_index=0,
            generation=1,
            event_receiver=receiver,
            process=process,
        )
        reactor.states = [state]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {}
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(side_effect=missing)

        self.assertIsNone(
            reactor._sample_owned_forest_or_defer_macos_transition()
        )
        reactor._sample_owned_forest.assert_called_once_with()

    def test_macos_missing_leader_without_transition_fails_closed(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        receiver = mock.Mock()
        receiver.poll.return_value = False
        process = mock.Mock()
        process.is_alive.return_value = True
        state = SimpleNamespace(
            worker_index=0,
            generation=1,
            event_receiver=receiver,
            process=process,
        )
        reactor.states = [state]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {}
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(side_effect=missing)

        with self.assertRaises(
            capability_process_tree.MacOSRegisteredLeaderMissingError
        ):
            reactor._sample_owned_forest_or_defer_macos_transition()

    def test_macos_last_compiler_query_defers_until_next_protocol_event(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        receiver = mock.Mock()
        receiver.poll.return_value = False
        process = mock.Mock()
        process.is_alive.return_value = True
        reactor.states = [
            SimpleNamespace(
                worker_index=0,
                generation=1,
                event_receiver=receiver,
                process=process,
            )
        ]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {(0, 1, 7): [9000]}
        unavailable = (
            capability_process_tree.MacOSProcessQueryUnavailableError(
                9000,
                9000,
                "identity",
            )
        )
        reactor._sample_owned_forest = mock.Mock(side_effect=unavailable)

        self.assertIsNone(
            reactor._sample_owned_forest_or_defer_macos_transition()
        )
        receiver.poll.assert_not_called()

    def test_macos_nonlast_compiler_query_without_transition_fails_closed(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        receiver = mock.Mock()
        receiver.poll.return_value = False
        process = mock.Mock()
        process.is_alive.return_value = True
        reactor.states = [
            SimpleNamespace(
                worker_index=0,
                generation=1,
                event_receiver=receiver,
                process=process,
            )
        ]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {(0, 1): 1234}
        reactor.native_lifecycle.compiler_groups = {(0, 1, 7): [9000, 9001]}
        unavailable = (
            capability_process_tree.MacOSProcessQueryUnavailableError(
                9000,
                9000,
                "residency",
            )
        )
        reactor._sample_owned_forest = mock.Mock(side_effect=unavailable)

        with self.assertRaises(
            capability_process_tree.MacOSProcessQueryUnavailableError
        ):
            reactor._sample_owned_forest_or_defer_macos_transition()
        receiver.poll.assert_called_once_with(0)
        process.is_alive.assert_called_once_with()

    def test_macos_next_compiler_reconciles_prior_before_permit(self):
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.compiler_groups = {(0, 1, 7): [9000]}
        lifecycle._native_reconciled_groups = set()
        lifecycle._compiler_admission_receipts = {(0, 1, 9000)}
        event = SimpleNamespace(
            worker_index=0, generation=1, task_id=7, pgid=9001
        )

        lifecycle.prepare_compiler_admission(event)

        lifecycle.accountant.reconcile_group.assert_called_once_with(
            9000,
            lifecycle.provider,
        )
        self.assertEqual(lifecycle.compiler_groups, {(0, 1, 7): [9000]})
        self.assertIn((0, 1, 9000), lifecycle._native_reconciled_groups)

    def test_macos_duplicate_compiler_report_does_not_reconcile_current(self):
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.compiler_groups = {(0, 1, 7): [9000]}
        lifecycle._compiler_admission_receipts = {(0, 1, 9000)}
        event = SimpleNamespace(
            worker_index=0, generation=1, task_id=7, pgid=9000
        )

        lifecycle.prepare_compiler_admission(event)

        lifecycle.accountant.reconcile_group.assert_not_called()

    def test_macos_unsampled_prior_compiler_blocks_next_admission(self):
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        lifecycle.compiler_groups = {(0, 1, 7): [9000]}
        lifecycle._compiler_admission_receipts = set()
        event = SimpleNamespace(
            worker_index=0, generation=1, task_id=7, pgid=9001
        )

        with self.assertRaisesRegex(
            AuditInfrastructureError,
            "admission sample",
        ):
            lifecycle.prepare_compiler_admission(event)

    def test_macos_missing_current_gate_cannot_receive_admission(self):
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        lifecycle.accountant = SimpleNamespace(_leaders={})
        lifecycle.compiler_groups = {(0, 1, 7): [9000]}
        lifecycle._native_reconciled_groups = {(0, 1, 9000)}
        lifecycle._compiler_admission_receipts = set()
        event = SimpleNamespace(
            worker_index=0, generation=1, task_id=7, pgid=9000
        )

        with self.assertRaisesRegex(
            AuditInfrastructureError,
            "admission is unregistered",
        ):
            lifecycle.record_compiler_admission(event)
        self.assertEqual(lifecycle._compiler_admission_receipts, set())

    def test_macos_pending_admission_retries_after_other_worker_transition(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        state = SimpleNamespace()
        event = SimpleNamespace(pgid=9000)
        permit = SimpleNamespace(pgid=9000)
        reactor._pending_macos_admissions = capability_runner.collections.deque([
            (state, event, permit)
        ])
        reactor.deferred_events = capability_runner.collections.deque()
        sample = object()
        reactor.result_budget = object()
        reactor._sample_owned_forest_or_defer_macos_transition = mock.Mock(
            side_effect=(None, sample)
        )
        reactor.native_lifecycle = mock.Mock()
        reactor._send_command = mock.Mock()

        self.assertIsNone(reactor._complete_pending_macos_admissions())
        with mock.patch.object(
            capability_runner,
            "_enforce_platform_memory_contract",
        ) as enforce:
            completed = reactor._complete_pending_macos_admissions()

        self.assertEqual(completed, (state, event))
        enforce.assert_called_once_with(sample, reactor.result_budget)
        reactor.native_lifecycle.record_compiler_admission.assert_called_once_with(
            event
        )
        reactor._send_command.assert_called_once_with(state, permit)

    def test_macos_exited_compiler_reconciles_then_samples_next_compiler(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(0, 1): 1234}
        lifecycle.compiler_groups = {(0, 1, 7): [9000, 9001]}
        reactor.native_lifecycle = lifecycle
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(9000)
        second_compiler_sample = object()
        reactor._sample_owned_forest = mock.Mock(
            side_effect=(missing, second_compiler_sample),
        )

        self.assertIs(
            reactor._sample_owned_forest_or_defer_macos_transition(),
            second_compiler_sample,
        )
        lifecycle.accountant.reconcile_group.assert_called_once_with(
            9000,
            lifecycle.provider,
        )
        self.assertEqual(lifecycle.compiler_groups, {(0, 1, 7): [9000, 9001]})
        self.assertEqual(reactor._sample_owned_forest.call_count, 2)

    def test_macos_unrelated_worker_transition_does_not_hide_missing_leader(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        missing_receiver = mock.Mock()
        missing_receiver.poll.return_value = False
        missing_process = mock.Mock()
        missing_process.is_alive.return_value = True
        unrelated_receiver = mock.Mock()
        unrelated_receiver.poll.return_value = True
        unrelated_process = mock.Mock()
        unrelated_process.is_alive.return_value = False
        reactor.states = [
            SimpleNamespace(
                worker_index=0,
                generation=1,
                event_receiver=missing_receiver,
                process=missing_process,
            ),
            SimpleNamespace(
                worker_index=1,
                generation=1,
                event_receiver=unrelated_receiver,
                process=unrelated_process,
            ),
        ]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        reactor.native_lifecycle.worker_groups = {
            (0, 1): 1234,
            (1, 1): 5678,
        }
        reactor.native_lifecycle.compiler_groups = {}
        missing = capability_process_tree.MacOSRegisteredLeaderMissingError(1234)
        reactor._sample_owned_forest = mock.Mock(side_effect=missing)

        with self.assertRaises(
            capability_process_tree.MacOSRegisteredLeaderMissingError
        ):
            reactor._sample_owned_forest_or_defer_macos_transition()
        missing_receiver.poll.assert_called_once_with(0)
        missing_process.is_alive.assert_called_once_with()
        unrelated_receiver.poll.assert_not_called()
        unrelated_process.is_alive.assert_not_called()

    def test_macos_transition_in_another_worker_does_not_suppress_sampling(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        first_receiver = mock.Mock()
        first_receiver.poll.return_value = False
        first_process = mock.Mock()
        first_process.is_alive.return_value = True
        second_receiver = mock.Mock()
        second_receiver.poll.return_value = True
        second_process = mock.Mock()
        second_process.is_alive.return_value = False
        reactor.states = [
            SimpleNamespace(
                event_receiver=first_receiver,
                process=first_process,
            ),
            SimpleNamespace(
                event_receiver=second_receiver,
                process=second_process,
            ),
        ]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        sample = object()
        reactor._sample_owned_forest = mock.Mock(return_value=sample)

        self.assertIs(
            reactor._sample_owned_forest_or_defer_macos_transition(),
            sample,
        )
        reactor._sample_owned_forest.assert_called_once_with()

    def test_macos_quiescent_live_worker_samples_owned_forest(self):
        reactor = object.__new__(capability_runner.GenerationReactor)
        receiver = mock.Mock()
        receiver.poll.return_value = False
        process = mock.Mock()
        process.is_alive.return_value = True
        reactor.states = [
            SimpleNamespace(
                event_receiver=receiver,
                process=process,
            )
        ]
        reactor.native_lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle
        )
        sample = object()
        reactor._sample_owned_forest = mock.Mock(return_value=sample)

        self.assertIs(
            reactor._sample_owned_forest_or_defer_macos_transition(),
            sample,
        )
        reactor._sample_owned_forest.assert_called_once_with()

    @unittest.skipIf(os.name == "nt", "POSIX descriptor transfer")
    def test_posix_capability_socketpair_transfers_multiple_chunks(self):
        paths = []
        for index in range(65):
            path = self.root / f"capability-transfer-{index}"
            path.write_bytes(f"stream-{index}".encode("ascii"))
            paths.append(path)
        socket_types = tuple(dict.fromkeys((
            capability_runner._posix_capability_socket_type(),
            socket.SOCK_DGRAM,
        )))
        for socket_type in socket_types:
            with self.subTest(socket_type=socket_type):
                parent, child = socket.socketpair(socket.AF_UNIX, socket_type)
                sources = tuple(path.open("rb") for path in paths)
                expected_identities = tuple(os.fstat(stream.fileno()) for stream in sources)
                received = ()
                cookie = "a" * 64
                deadline = time.monotonic() + 10.0
                try:
                    capability_runner._send_posix_capability_streams(
                        parent,
                        sources,
                        cookie,
                        deadline,
                        threading.Event(),
                    )
                    received = capability_runner._receive_posix_capability_streams(
                        child,
                        len(sources),
                        cookie,
                        deadline,
                        threading.Event(),
                    )
                    self.assertEqual(len(received), len(sources))
                    for index, (stream, expected) in enumerate(
                        zip(received, expected_identities)
                    ):
                        observed = os.fstat(stream.fileno())
                        self.assertEqual(
                            (observed.st_dev, observed.st_ino),
                            (expected.st_dev, expected.st_ino),
                        )
                        self.assertFalse(os.get_inheritable(stream.fileno()))
                        self.assertEqual(
                            stream.read(),
                            f"stream-{index}".encode("ascii"),
                        )
                finally:
                    for stream in received:
                        stream.close()
                    for stream in sources:
                        stream.close()
                    parent.close()
                    child.close()

    @staticmethod
    def _open_posix_descriptor_numbers():
        root = Path("/dev/fd" if sys.platform == "darwin" else "/proc/self/fd")
        descriptors = set()
        for entry in root.iterdir():
            try:
                descriptor = int(entry.name)
                os.fstat(descriptor)
            except (OSError, ValueError):
                continue
            descriptors.add(descriptor)
        return descriptors

    @unittest.skipIf(os.name == "nt", "POSIX descriptor transfer")
    def test_posix_capability_receive_rejects_truncation_without_fd_leak(self):
        cookie = "b" * 64
        header = struct.pack(
            capability_runner._CAPABILITY_FD_HEADER_FORMAT,
            bytes.fromhex(cookie),
            0,
            1,
            1,
        )
        for case, descriptor_count, payload in (
            ("payload", 1, header + b"trailing-byte"),
            ("ancillary", 65, header),
        ):
            with self.subTest(case=case):
                parent, child = socket.socketpair(
                    socket.AF_UNIX,
                    capability_runner._posix_capability_socket_type(),
                )
                baseline = self._open_posix_descriptor_numbers()
                sources = tuple(self.compiler.open("rb") for _ in range(descriptor_count))
                try:
                    try:
                        sent = parent.sendmsg(
                            [payload],
                            [(
                                socket.SOL_SOCKET,
                                socket.SCM_RIGHTS,
                                array("i", (stream.fileno() for stream in sources)),
                            )],
                        )
                        self.assertEqual(sent, len(payload))
                        with self.assertRaisesRegex(
                            AuditInfrastructureError,
                            "truncated",
                        ):
                            capability_runner._receive_posix_capability_streams(
                                child,
                                1,
                                cookie,
                                time.monotonic() + 10.0,
                                threading.Event(),
                            )
                    finally:
                        for stream in sources:
                            stream.close()
                    self.assertEqual(self._open_posix_descriptor_numbers(), baseline)
                finally:
                    parent.close()
                    child.close()

    @unittest.skipIf(os.name == "nt", "POSIX descriptor transfer")
    def test_posix_capability_receive_rejects_wrong_generation_without_fd_leak(self):
        cookie = "c" * 64
        cases = (
            ("cookie", "d" * 64, 0, 1, 1),
            ("index", cookie, 1, 1, 1),
            ("chunks", cookie, 0, 2, 1),
            ("count", cookie, 0, 1, 2),
        )
        for case, sent_cookie, chunk_index, chunks, descriptor_count in cases:
            with self.subTest(case=case):
                parent, child = socket.socketpair(
                    socket.AF_UNIX,
                    capability_runner._posix_capability_socket_type(),
                )
                baseline = self._open_posix_descriptor_numbers()
                source = self.compiler.open("rb")
                try:
                    try:
                        header = struct.pack(
                            capability_runner._CAPABILITY_FD_HEADER_FORMAT,
                            bytes.fromhex(sent_cookie),
                            chunk_index,
                            chunks,
                            descriptor_count,
                        )
                        sent = parent.sendmsg(
                            [header],
                            [(
                                socket.SOL_SOCKET,
                                socket.SCM_RIGHTS,
                                array("i", (source.fileno(),)),
                            )],
                        )
                        self.assertEqual(sent, len(header))
                        with self.assertRaisesRegex(
                            AuditInfrastructureError,
                            "generation differs",
                        ):
                            capability_runner._receive_posix_capability_streams(
                                child,
                                1,
                                cookie,
                                time.monotonic() + 10.0,
                                threading.Event(),
                            )
                    finally:
                        source.close()
                    self.assertEqual(self._open_posix_descriptor_numbers(), baseline)
                finally:
                    parent.close()
                    child.close()

    @unittest.skipIf(os.name == "nt", "POSIX descriptor transfer")
    def test_posix_capability_receive_unwinds_partial_fdopen_without_leak(self):
        parent, child = socket.socketpair(
            socket.AF_UNIX,
            capability_runner._posix_capability_socket_type(),
        )
        baseline = self._open_posix_descriptor_numbers()
        sources = tuple(self.compiler.open("rb") for _ in range(2))
        cookie = "e" * 64
        header = struct.pack(
            capability_runner._CAPABILITY_FD_HEADER_FORMAT,
            bytes.fromhex(cookie),
            0,
            1,
            2,
        )
        real_fdopen = os.fdopen
        calls = 0

        def fail_second_fdopen(descriptor, *args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected fdopen failure")
            return real_fdopen(descriptor, *args, **kwargs)

        try:
            try:
                sent = parent.sendmsg(
                    [header],
                    [(
                        socket.SOL_SOCKET,
                        socket.SCM_RIGHTS,
                        array("i", (stream.fileno() for stream in sources)),
                    )],
                )
                self.assertEqual(sent, len(header))
                with mock.patch.object(
                    capability_runner.os,
                    "fdopen",
                    side_effect=fail_second_fdopen,
                ), self.assertRaisesRegex(
                    AuditInfrastructureError,
                    "receive failed",
                ):
                    capability_runner._receive_posix_capability_streams(
                        child,
                        2,
                        cookie,
                        time.monotonic() + 10.0,
                        threading.Event(),
                    )
            finally:
                for stream in sources:
                    stream.close()
            self.assertEqual(self._open_posix_descriptor_numbers(), baseline)
        finally:
            parent.close()
            child.close()

    def test_child_bootstrap_linux_uses_only_transferred_handles_without_path_or_watcher(self):
        streams = tuple(
            os.fdopen(os.dup(stream.fileno()), "rb", closefd=True)
            for stream in self.capability.native_owner.streams
        )
        payload = capability_runner._encode_worker_bootstrap(
            (self.configuration,),
            self.authority,
            {},
            AuditLimits(),
            self.engine,
            time.monotonic() + 10.0,
            transfer_cookie="a" * 64,
            transferred_handles={},
        )
        (
            authority,
            documents,
            _snapshot,
            _limits,
            _engine,
            deadline,
            _cookie,
            _count,
        ) = capability_runner._decode_worker_bootstrap(payload)
        self._retarget_bootstrap_document_platform(documents[0], "linux")
        child_capability = None
        try:
            with mock.patch.object(
                capability_runner,
                "_regular_file_snapshot",
                side_effect=AssertionError("child re-statted compiler path"),
            ), mock.patch.object(
                capability_runner,
                "_FilesystemGenerationObserver",
                side_effect=AssertionError("child created a filesystem watcher"),
            ), mock.patch.object(
                Path,
                "open",
                side_effect=AssertionError("child reopened compiler path"),
            ):
                child_capability = capability_runner._compiler_capability_from_bootstrap(
                    documents[0],
                    authority,
                    deadline,
                    threading.Event(),
                    streams,
                )
            self.assertEqual(
                child_capability.native_owner.executable_fd,
                streams[0].fileno(),
            )
        finally:
            if child_capability is not None:
                child_capability.native_owner.close()
            else:
                for stream in reversed(streams):
                    if not stream.closed:
                        stream.close()

    def test_child_bootstrap_macos_rearms_transferred_path_generation_guard(self):
        streams = tuple(
            os.fdopen(os.dup(stream.fileno()), "rb", closefd=True)
            for stream in self.capability.native_owner.streams
        )
        payload = capability_runner._encode_worker_bootstrap(
            (self.configuration,),
            self.authority,
            {},
            AuditLimits(),
            self.engine,
            time.monotonic() + 10.0,
            transfer_cookie="a" * 64,
            transferred_handles={},
        )
        (
            authority,
            documents,
            _snapshot,
            _limits,
            _engine,
            deadline,
            _cookie,
            _count,
        ) = capability_runner._decode_worker_bootstrap(payload)
        self._retarget_bootstrap_document_platform(documents[0], "macos")
        child_capability = None
        observer = mock.Mock()
        try:
            with mock.patch.object(
                capability_runner,
                "_FilesystemGenerationObserver",
                return_value=observer,
            ) as make_observer:
                child_capability = capability_runner._compiler_capability_from_bootstrap(
                    documents[0],
                    authority,
                    deadline,
                    threading.Event(),
                    streams,
                )
            make_observer.assert_called_once_with(
                tuple((Path(path), True) for path in documents[0]["owner"]["directory_paths"])
                + tuple((Path(path), False) for path in documents[0]["owner"]["alias_paths"])
                + tuple((Path(path), False) for path in documents[0]["owner"]["file_paths"])
            )
            self.assertIs(child_capability.native_owner.observer, observer)
            self.assertTrue(child_capability.native_owner.validate_paths)
        finally:
            if child_capability is not None:
                child_capability.native_owner.close()
            else:
                for stream in reversed(streams):
                    if not stream.closed:
                        stream.close()

    def _macos_transferred_script_capability(self):
        compiler = self.toolchain / "macos-compiler"
        replacement = self.toolchain / "macos-compiler-replacement"
        original_script = (
            b"#!/bin/sh\n"
            b"printf 'g++ (GCC) 14.1.0\\n'\n"
            b"if [ -n \"$OLR_TEST_READY\" ]; then\n"
            b"  : > \"$OLR_TEST_READY\"\n"
            b"  sleep 1\n"
            b"fi\n"
        )
        replacement_script = (
            b"#!/bin/sh\n"
            b"printf 'replacement g++ (GCC) 14.1.0\\n'\n"
            b": > \"$OLR_REPLACEMENT_MARKER\"\n"
            b"while [ ! -e \"$OLR_REPLACEMENT_RELEASE\" ]; do sleep 0.01; done\n"
        )
        compiler.write_bytes(original_script)
        replacement.write_bytes(replacement_script)
        compiler.chmod(0o755)
        replacement.chmod(0o755)
        parent = open_compiler_executable_capability(
            compiler,
            self.authority,
            time.monotonic() + 10.0,
            _query_driver=False,
        )
        configuration = dataclasses.replace(
            self.configuration,
            compiler=compiler,
            compiler_capability_digest=parent.capability_digest,
            compiler_capability=parent,
        )
        streams = tuple(
            os.fdopen(os.dup(stream.fileno()), "rb", closefd=True)
            for stream in parent.native_owner.streams
        )
        child = None
        try:
            payload = capability_runner._encode_worker_bootstrap(
                (configuration,),
                self.authority,
                {},
                AuditLimits(),
                self.engine,
                time.monotonic() + 10.0,
                transfer_cookie="a" * 64,
                transferred_handles={},
            )
            decoded = capability_runner._decode_worker_bootstrap(payload)
            child = capability_runner._compiler_capability_from_bootstrap(
                decoded[1][0],
                decoded[0],
                decoded[5],
                threading.Event(),
                streams,
            )
            return parent, child, compiler, replacement
        except BaseException:
            if child is not None:
                child.native_owner.close()
            else:
                for stream in reversed(streams):
                    if not stream.closed:
                        stream.close()
            parent.native_owner.close()
            raise

    def _macos_test_launch_observer(self, deadline, active):
        test = self

        class Observer:
            macos_launch_deadline = deadline

            def authorize_macos_compiler_exec(_self, process_start):
                return capability_model.MacOSInspectionExecPermit(
                    "transferred-owner-test",
                    process_start.pid,
                    process_start.pid,
                    "d" * 64,
                )

            def register_compiler_process_launch(_self, event, carrier):
                active[event.process_start] = carrier

            def complete_compiler_process_launch(_self, event, carrier):
                test.assertIs(active.pop(event.process_start), carrier)

            def fail_compiler_process_launch(_self, event, carrier):
                if active.get(event.process_start) is carrier:
                    active.pop(event.process_start)

        return Observer()

    @unittest.skipUnless(sys.platform == "darwin", "native macOS path-generation guard")
    def test_macos_transferred_owner_rejects_persistent_replacement_before_launch(self):
        parent, child, compiler, replacement = self._macos_transferred_script_capability()
        displaced = self.toolchain / "macos-compiler-displaced"
        try:
            compiler.rename(displaced)
            replacement.rename(compiler)
            with mock.patch.object(
                capability_command.subprocess,
                "Popen",
            ) as popen, self.assertRaisesRegex(
                AuditInfrastructureError,
                "generation|changed|path chain|identity",
            ):
                capability_command._run_probe_command(
                    child,
                    ("--version",),
                    self.root,
                    os.environ,
                    time.monotonic() + 10.0,
                )
            popen.assert_not_called()
        finally:
            child.native_owner.close()
            parent.native_owner.close()

    @unittest.skipUnless(sys.platform == "darwin", "native macOS permit-to-exec guard")
    def test_macos_transferred_owner_rejects_permit_to_exec_swap_restore(self):
        parent, child, compiler, replacement = self._macos_transferred_script_capability()
        displaced = self.toolchain / "macos-compiler-displaced"
        deadline = time.monotonic() + 10.0
        active = {}
        observer = self._macos_test_launch_observer(deadline, active)
        marker = self.root / "macos-replacement-marker"
        release = self.root / "macos-replacement-release"
        environment = dict(os.environ)
        environment["OLR_REPLACEMENT_MARKER"] = str(marker)
        environment["OLR_REPLACEMENT_RELEASE"] = str(release)

        real_write = capability_command._write_length_prefixed_fd
        swapped = False

        def install_replacement_release_permit_and_restore(descriptor, payload):
            nonlocal swapped
            compiler.rename(displaced)
            replacement.rename(compiler)
            real_write(descriptor, payload)
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.005)
            if not marker.exists():
                raise AssertionError("replacement compiler did not execute")
            try:
                compiler.rename(replacement)
                displaced.rename(compiler)
                swapped = True
            finally:
                release.write_bytes(b"release\n")

        try:
            with mock.patch.object(
                capability_command,
                "_write_length_prefixed_fd",
                side_effect=install_replacement_release_permit_and_restore,
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "generation|changed|path chain|identity",
            ):
                capability_command._run_probe_command(
                    child,
                    ("--version",),
                    self.root,
                    environment,
                    deadline,
                    launch_observer=observer,
                )
            self.assertTrue(swapped)
            self.assertTrue(marker.is_file())
            self.assertEqual(active, {})
        finally:
            if displaced.exists():
                if compiler.exists() and not replacement.exists():
                    compiler.rename(replacement)
                displaced.rename(compiler)
            release.write_bytes(b"release\n")
            child.native_owner.close()
            parent.native_owner.close()

    @unittest.skipUnless(sys.platform == "darwin", "native macOS during-exec guard")
    def test_macos_transferred_owner_rejects_during_exec_swap_restore(self):
        parent, child, compiler, replacement = self._macos_transferred_script_capability()
        displaced = self.toolchain / "macos-compiler-displaced"
        ready = self.root / "macos-compiler-ready"
        deadline = time.monotonic() + 10.0
        active = {}
        observer = self._macos_test_launch_observer(deadline, active)
        mutation_error = []
        stop_mutator = threading.Event()

        def swap_restore_during_exec():
            try:
                while (
                    not ready.exists()
                    and not stop_mutator.is_set()
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.005)
                if stop_mutator.is_set():
                    return
                if not ready.exists():
                    raise AssertionError("compiler did not reach its execution barrier")
                compiler.rename(displaced)
                replacement.rename(compiler)
                compiler.rename(replacement)
                displaced.rename(compiler)
            except BaseException as error:
                mutation_error.append(error)

        mutator = threading.Thread(target=swap_restore_during_exec)
        environment = dict(os.environ)
        environment["OLR_TEST_READY"] = str(ready)
        try:
            mutator.start()
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "generation|changed|path chain|identity",
            ):
                capability_command._run_probe_command(
                    child,
                    ("--version",),
                    self.root,
                    environment,
                    deadline,
                    launch_observer=observer,
                )
        finally:
            stop_mutator.set()
            mutator.join()
            child.native_owner.close()
            parent.native_owner.close()
        self.assertEqual(mutation_error, [])
        self.assertEqual(active, {})

    def test_registry_final_close_releases_path_generation_guard_for_rename(self):
        registry = capability_runner.CompilerCapabilityRegistry(self.authority)
        registry.register(self.capability)
        registry.close()
        renamed = self.compiler.with_name("compiler-renamed.exe")
        self.compiler.rename(renamed)
        self.assertTrue(renamed.is_file())

    def test_held_executable_launch_uses_native_descriptor_path_on_linux(self):
        arguments, options = capability_command._held_compiler_launch(
            dataclasses.replace(self.capability, platform_kind="linux"),
            (str(self.compiler), "--version"),
        )
        self.assertEqual(
            arguments[0],
            f"/proc/self/fd/{self.capability.native_owner.executable_fd}",
        )
        self.assertEqual(
            options["pass_fds"],
            (self.capability.native_owner.executable_fd,),
        )

    def test_held_executable_launch_uses_guarded_canonical_path_on_macos(self):
        arguments, options = capability_command._held_compiler_launch(
            dataclasses.replace(self.capability, platform_kind="macos"),
            (str(self.compiler), "--version"),
        )
        self.assertEqual(
            arguments[0],
            str(self.capability.executable_identity.canonical),
        )
        self.assertEqual(options, {})

    def test_fake_preprocessor_has_bounded_cpu_and_crash_coordinator_modes(self):
        fixture = Path(__file__).parent / "fixtures" / "fake_preprocessor.py"
        burned = subprocess.run(
            (
                sys.executable,
                str(fixture),
                "--fixture-mode",
                "cpu-burn",
                "--cpu-burn-seconds",
                "0.02",
                str(self.source),
            ),
            capture_output=True,
            timeout=5,
        )
        self.assertEqual(burned.returncode, 0, burned.stderr)
        self.assertIn(b"coordinator.cpp", burned.stdout)
        crashed = subprocess.run(
            (
                sys.executable,
                str(fixture),
                "--fixture-mode",
                "coordinator-crash",
                str(self.source),
            ),
            capture_output=True,
            timeout=5,
        )
        self.assertEqual(crashed.returncode, 86)


class NativeGenerationLifecycleAdapterTests(unittest.TestCase):
    def test_macos_reconciliation_success_is_retryable_before_lifecycle_progress(self):
        from gpu_capability_process_tree import (
            MacOSLibprocProvider,
            MacOSRegisteredPgidAccountant,
        )

        def build(include_worker=True):
            accountant = MacOSRegisteredPgidAccountant()
            if include_worker:
                accountant.register_group(
                    701,
                    capability_runner._owned_process_identity(
                        "macos", 701, "1:2"
                    ),
                    "worker:1:2",
                )
            accountant.register_group(
                811,
                capability_runner._owned_process_identity(
                    "macos", 811, "1:3"
                ),
                "compiler:1:2:3",
            )
            provider = object.__new__(MacOSLibprocProvider)
            provider.reconcile_survivors = mock.Mock(return_value=())
            lifecycle = object.__new__(
                capability_runner._MacOSGenerationLifecycle
            )
            lifecycle.accountant = accountant
            lifecycle.provider = provider
            lifecycle.worker_groups = {(1, 2): 701} if include_worker else {}
            lifecycle.compiler_groups = {(1, 2, 3): [811]}
            lifecycle.released_generations = set()
            lifecycle._release_reconciliation_progress = set()
            return lifecycle, provider

        lifecycle, provider = build(include_worker=False)
        faulted = False

        def fail_task(scope, worker, generation, pgid):
            nonlocal faulted
            if scope == "reconcile-task" and not faulted:
                faulted = True
                raise RuntimeError("task reconcile committed")

        with mock.patch.object(
            capability_runner,
            "_macos_reconciliation_completed",
            side_effect=fail_task,
            create=True,
        ), self.assertRaisesRegex(RuntimeError, "task reconcile committed"):
            lifecycle.reconcile_task(1, 2, 3)
        self.assertEqual(lifecycle.compiler_groups, {(1, 2, 3): [811]})
        with mock.patch.object(
            capability_runner,
            "_macos_reconciliation_completed",
            side_effect=fail_task,
            create=True,
        ):
            lifecycle.reconcile_task(1, 2, 3)
        self.assertEqual(provider.reconcile_survivors.call_count, 1)

        for target_scope in ("release-compiler", "release-worker"):
            with self.subTest(target_scope=target_scope):
                lifecycle, provider = build()
                faulted = False

                def fail_release(scope, worker, generation, pgid):
                    nonlocal faulted
                    if scope == target_scope and not faulted:
                        faulted = True
                        raise RuntimeError(f"{scope} committed")

                with mock.patch.object(
                    capability_runner,
                    "_macos_reconciliation_completed",
                    side_effect=fail_release,
                    create=True,
                ), self.assertRaisesRegex(RuntimeError, "committed"):
                    lifecycle.release_generation(
                        1, 2, None, time.monotonic() + 10.0, force=False
                    )
                self.assertIn((1, 2), lifecycle.worker_groups)
                self.assertIn((1, 2, 3), lifecycle.compiler_groups)
                with mock.patch.object(
                    capability_runner,
                    "_macos_reconciliation_completed",
                    side_effect=fail_release,
                    create=True,
                ):
                    lifecycle.release_generation(
                        1, 2, None, time.monotonic() + 10.0, force=False
                    )
                calls = [call.args[1] for call in provider.reconcile_survivors.call_args_list]
                self.assertEqual(calls.count(811), 1)
                self.assertEqual(calls.count(701), 1)

    def test_unix_terminal_marker_add_failure_retains_live_state_for_retry(self):
        class FailingSet(set):
            def __init__(_self, position):
                super().__init__()
                _self.position = position
                _self.failed = False

            def add(_self, value):
                if not _self.failed and _self.position == "before":
                    _self.failed = True
                    raise RuntimeError("marker before")
                super().add(value)
                if not _self.failed:
                    _self.failed = True
                    raise RuntimeError("marker after")

        for platform in ("linux", "macos"):
            for position in ("before", "after"):
                with self.subTest(platform=platform, position=position):
                    key = (1, 2)
                    if platform == "linux":
                        client = SimpleNamespace(
                            create_leaf=mock.Mock(),
                            acknowledge_leaf=mock.Mock(),
                            release_leaf=mock.Mock(),
                        )
                        lifecycle = capability_runner._LinuxGenerationLifecycle(
                            client, 100.0
                        )
                        lifecycle.live[key] = object()
                        lifecycle.requested.add(key)
                        lifecycle.released = FailingSet(position)
                        carrier = lifecycle.live[key]
                    else:
                        accountant = SimpleNamespace(
                            reconcile_group=mock.Mock(return_value=True)
                        )
                        lifecycle = object.__new__(
                            capability_runner._MacOSGenerationLifecycle
                        )
                        lifecycle.accountant = accountant
                        lifecycle.provider = object()
                        lifecycle.worker_groups = {key: 701}
                        lifecycle.compiler_groups = {}
                        lifecycle.released_generations = FailingSet(position)
                        lifecycle._release_reconciliation_progress = set()
                        carrier = None
                    release = lambda: lifecycle.release_generation(
                        *key, carrier, time.monotonic() + 10.0, force=False
                    )
                    with self.assertRaisesRegex(RuntimeError, "marker"):
                        release()
                    live = (
                        lifecycle.live
                        if platform == "linux"
                        else lifecycle.worker_groups
                    )
                    self.assertIn(key, live)
                    release()
                    self.assertNotIn(key, live)

    def test_unix_release_terminal_publication_is_retryable_at_each_boundary(self):
        key = (1, 2)
        for platform, stages in (
            ("linux", ("external", "terminal", "remove")),
            ("macos", ("terminal", "remove")),
        ):
            for stage in stages:
                for position in ("before", "after"):
                    with self.subTest(
                        platform=platform, stage=stage, position=position
                    ):
                        events = []
                        if platform == "linux":
                            class Client:
                                def create_leaf(_self, *_args): pass
                                def acknowledge_leaf(_self, *_args): pass
                                def release_leaf(_self, worker, generation,
                                                 _deadline):
                                    events.append(("external", worker, generation))

                            lifecycle = capability_runner._LinuxGenerationLifecycle(
                                Client(), 100.0
                            )
                            carrier = object()
                            lifecycle.live[key] = carrier
                            lifecycle.requested.add(key)
                        else:
                            class Accountant:
                                def reconcile_group(_self, pgid, _provider):
                                    events.append(("reconcile", pgid))
                                    return True

                            lifecycle = object.__new__(
                                capability_runner._MacOSGenerationLifecycle
                            )
                            lifecycle.accountant = Accountant()
                            lifecycle.provider = object()
                            lifecycle.worker_groups = {key: 701}
                            lifecycle.compiler_groups = {(1, 2, 3): [801]}
                            lifecycle.released_generations = set()
                            lifecycle._release_reconciliation_progress = set()
                            carrier = None
                        faulted = False

                        def inject(kind, actual_stage, actual_position,
                                   worker, generation):
                            nonlocal faulted
                            if (
                                (kind, actual_stage, actual_position)
                                == (platform, stage, position)
                                and (worker, generation) == key
                                and not faulted
                            ):
                                faulted = True
                                raise RuntimeError(
                                    f"{platform} {stage} {position}"
                                )

                        def release():
                            lifecycle.release_generation(
                                *key,
                                carrier,
                                time.monotonic() + 10.0,
                                force=False,
                            )

                        with mock.patch.object(
                            capability_runner,
                            "_generation_release_transition",
                            side_effect=inject,
                        ), self.assertRaisesRegex(
                            RuntimeError, f"{platform} {stage} {position}"
                        ):
                            release()
                        with mock.patch.object(
                            capability_runner,
                            "_generation_release_transition",
                            side_effect=inject,
                        ):
                            release()
                        released = (
                            lifecycle.released
                            if platform == "linux"
                            else lifecycle.released_generations
                        )
                        self.assertIn(key, released)
                        live = (
                            lifecycle.live
                            if platform == "linux"
                            else lifecycle.worker_groups
                        )
                        self.assertNotIn(key, live)
                        if platform == "linux":
                            self.assertEqual(events, [("external", 1, 2)])
                        else:
                            self.assertEqual(
                                events.count(("reconcile", 701)), 1
                            )
                            self.assertEqual(
                                events.count(("reconcile", 801)), 1
                            )

    def test_unix_task_phase_lifecycle_seals_are_exact_and_sticky(self):
        linux_memory = capability_model.LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20,
            0, 0, 0, 0, 0, 0, 0, True,
        )
        linux = object.__new__(capability_runner._LinuxGenerationLifecycle)
        linux.live = {}
        linux.requested = set()
        linux.memory_measurements = mock.Mock(return_value=linux_memory)
        linux_first = linux.seal_task_phase(3)
        linux_second = linux.seal_task_phase(3)
        self.assertIs(linux_second, linux_first)
        self.assertEqual(linux.memory_measurements.call_count, 1)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "generation count"
        ):
            linux.seal_task_phase(4)

        macos_memory = capability_model.MacOSRunMemoryMeasurements(
            1, 1, 1, 0, 0, True
        )
        macos = object.__new__(capability_runner._MacOSGenerationLifecycle)
        macos.worker_groups = {}
        macos.compiler_groups = {}
        macos.observe = mock.Mock()
        macos.accountant = SimpleNamespace(
            memory_measurements=mock.Mock(return_value=macos_memory)
        )
        macos_first = macos.seal_task_phase(3)
        macos_second = macos.seal_task_phase(3)
        self.assertIs(macos_second, macos_first)
        self.assertEqual(macos.observe.call_count, 1)
        self.assertEqual(macos.accountant.memory_measurements.call_count, 1)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "generation count"
        ):
            macos.seal_task_phase(4)

    def test_linux_abort_kills_generation_leaf_before_release(self):
        events = []
        carrier = capability_model.LinuxWorkerContainment(
            "/cg/run", "/cg/run/worker-1-g2", "b" * 64
        )

        class Client:
            def create_leaf(self, *_args):
                return carrier

            def acknowledge_leaf(self, *_args):
                pass

            def release_leaf(self, worker_index, generation, deadline):
                events.append(("release", worker_index, generation, deadline))

        lifecycle = capability_runner._LinuxGenerationLifecycle(Client(), 91.0)
        lifecycle.live[(1, 2)] = carrier
        lifecycle.requested.add((1, 2))
        with mock.patch.object(
            lifecycle,
            "_force_empty",
            side_effect=lambda actual, deadline: events.append(
                ("kill", actual, deadline)
            ),
        ):
            lifecycle.release_generation(1, 2, carrier, 92.0, force=True)
        self.assertEqual(
            events,
            [("kill", carrier, 92.0), ("release", 1, 2, 92.0)],
        )

    def test_linux_generation_leaf_precedes_spawn_ack_and_empty_release(self):
        events = []
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        service = Path(temporary.name).resolve()
        run = service / "run"
        leaf = run / "worker-2-g4"
        leaf.mkdir(parents=True)
        for root in (service, run):
            (root / "memory.events").write_text(
                "oom 0\noom_kill 0\nmax 0\n", encoding="ascii"
            )
        for name, value in (
            ("memory.current", "1"),
            ("memory.peak", "2"),
            ("memory.max", str(512 << 20)),
            ("memory.high", str(448 << 20)),
        ):
            (run / name).write_text(value, encoding="ascii")
        (leaf / "cgroup.procs").write_text("", encoding="ascii")
        carrier_run = str(run) if sys.platform.startswith("linux") else "/cg/run"
        carrier_leaf = (
            str(leaf)
            if sys.platform.startswith("linux")
            else "/cg/run/worker-2-g4"
        )
        carrier = capability_model.LinuxWorkerContainment(
            carrier_run, carrier_leaf, "a" * 64
        )

        class Client:
            def create_leaf(self, worker_index, generation, deadline):
                events.append(("create", worker_index, generation, deadline))
                return carrier

            def acknowledge_leaf(
                self, worker_index, generation, worker_pid, actual, deadline
            ):
                events.append((
                    "ack", worker_index, generation, worker_pid,
                    actual, deadline,
                ))

            def release_leaf(self, worker_index, generation, deadline):
                events.append(("release", worker_index, generation, deadline))

        lifecycle = capability_runner._LinuxGenerationLifecycle(Client(), 99.0)
        actual = lifecycle.create_generation(2, 4)
        lifecycle.accept_worker_contained(
            capability_model.WorkerContained(2, 4, 701, "linux:701:4"),
            actual,
        )
        lifecycle.release_generation(2, 4, actual, 99.0, force=False)
        self.assertEqual(
            events,
            [
                ("create", 2, 4, 99.0),
                ("ack", 2, 4, 701, carrier, 99.0),
                ("release", 2, 4, 99.0),
            ],
        )

    def test_linux_failed_create_request_is_releasable_without_carrier(self):
        class Client:
            def __init__(_self):
                _self.release_calls = []

            def create_leaf(_self, _worker, _generation, _deadline):
                raise AuditInfrastructureError("reply lost after create")

            def acknowledge_leaf(_self, *_args):
                pass

            def release_leaf(_self, worker, generation, deadline):
                _self.release_calls.append((worker, generation, deadline))

        client = Client()
        lifecycle = capability_runner._LinuxGenerationLifecycle(client, 99.0)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "reply lost"
        ):
            lifecycle.create_generation(2, 5)
        lifecycle.release_generation(2, 5, None, 100.0, force=True)
        lifecycle.release_generation(2, 5, None, 100.0, force=True)
        self.assertEqual(client.release_calls, [(2, 5, 100.0)])
        self.assertIn((2, 5), lifecycle.released)
        self.assertNotIn((2, 5), lifecycle.requested)

    def test_macos_registers_worker_and_compiler_then_reconciles_compiler_first(self):
        events = []
        deadline = time.monotonic() + 10.0

        class Accountant:
            def register_group(self, pgid, leader, purpose):
                events.append(("register", pgid, purpose))

            def reconcile_group(self, pgid, provider):
                events.append(("reconcile", pgid, provider.name))
                return True

            def memory_measurements(self):
                return capability_model.MacOSRunMemoryMeasurements(
                    30, 10, 20, 0, 0, True
                )

        class Provider:
            name = "provider"

            def _identity_and_residency(self, pid, pgid):
                return (
                    capability_runner._owned_process_identity(
                        "macos", pid, "1:2"
                    ),
                    1,
                    0,
                )

            def observe(self, _accountant, parent_resident):
                events.append(("observe", parent_resident))

        class Authority:
            def permit_compiler(self, report):
                events.append(("permit", report.pgid))
                return capability_model.CompilerExecPermit(
                    report.worker_index,
                    report.generation,
                    report.task_id,
                    report.pgid,
                )

        lifecycle = capability_runner._MacOSGenerationLifecycle(
            Accountant(),
            (),
            deadline,
            provider=Provider(),
            permit_authority=Authority(),
        )
        lifecycle.accept_worker_session(
            capability_model.MacOSWorkerSessionReported(2, 4, 701, 701, "1:2")
        )
        report = SimpleNamespace(
            worker_index=2, generation=4, task_id=5, pgid=811
        )
        permit = lifecycle.permit_compiler(report)
        self.assertEqual(permit.pgid, 811)
        lifecycle.reconcile_task(2, 4, 5)
        lifecycle.release_generation(2, 4, None, deadline, force=False)
        snapshot = lifecycle.seal_task_phase(1)
        self.assertEqual(snapshot.platform_kind, "macos")
        self.assertEqual(snapshot.phase, "tasks")
        self.assertTrue(snapshot.memory.accounting_complete)
        self.assertEqual(
            events[:5],
            [
                ("register", 701, "worker:2:4"),
                ("permit", 811),
                ("reconcile", 811, "provider"),
                ("reconcile", 701, "provider"),
                ("observe", mock.ANY),
            ],
        )

    def test_macos_early_compiler_reconcile_survives_forced_release_retry(self):
        deadline = time.monotonic() + 10.0
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(2, 4): 701}
        lifecycle.compiler_groups = {(2, 4, 5): [811]}
        lifecycle.released_generations = set()
        lifecycle._release_reconciliation_progress = set()

        self.assertTrue(
            lifecycle.reconcile_exited_compiler_for_observation(811)
        )
        hook_calls = []

        def reconciliation_hook(scope, worker, generation, pgid):
            hook_calls.append((scope, worker, generation, pgid))
            if hook_calls == [("release-compiler", 2, 4, 811)]:
                raise AuditInfrastructureError("injected completion-hook failure")

        with (
            mock.patch.object(lifecycle, "_kill_and_wait_empty") as kill,
            mock.patch.object(
                capability_runner,
                "_macos_reconciliation_completed",
                side_effect=reconciliation_hook,
            ),
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "completion-hook failure",
            ):
                lifecycle.release_generation(2, 4, None, deadline, force=True)
            lifecycle.release_generation(2, 4, None, deadline, force=True)

        self.assertEqual(kill.call_count, 1)
        self.assertEqual(kill.call_args.args[:2], (701, deadline))
        self.assertEqual(
            lifecycle.accountant.reconcile_group.call_args_list,
            [
                mock.call(811, lifecycle.provider),
                mock.call(701, lifecycle.provider),
            ],
        )
        self.assertEqual(lifecycle.worker_groups, {})
        self.assertEqual(lifecycle.compiler_groups, {})
        self.assertIn((2, 4), lifecycle.released_generations)

    def test_macos_forced_compiler_release_does_not_rekill_after_hook_failure(self):
        deadline = time.monotonic() + 10.0
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(2, 4): 701}
        lifecycle.compiler_groups = {(2, 4, 5): [811]}
        lifecycle.released_generations = set()
        lifecycle._release_reconciliation_progress = set()
        lifecycle._native_reconciled_groups = set()
        failed = False

        def reconciliation_hook(scope, _worker, _generation, _pgid):
            nonlocal failed
            if scope == "release-compiler" and not failed:
                failed = True
                raise AuditInfrastructureError("injected compiler-hook failure")

        with (
            mock.patch.object(lifecycle, "_kill_and_wait_empty") as kill,
            mock.patch.object(
                capability_runner,
                "_macos_reconciliation_completed",
                side_effect=reconciliation_hook,
            ),
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "compiler-hook failure",
            ):
                lifecycle.release_generation(2, 4, None, deadline, force=True)
            lifecycle.release_generation(2, 4, None, deadline, force=True)

        self.assertEqual(
            [call.args[:2] for call in kill.call_args_list],
            [(811, deadline), (701, deadline)],
        )
        self.assertEqual(
            lifecycle.accountant.reconcile_group.call_args_list,
            [
                mock.call(811, lifecycle.provider),
                mock.call(701, lifecycle.provider),
            ],
        )

    def test_macos_forced_worker_release_does_not_rekill_after_hook_failure(self):
        deadline = time.monotonic() + 10.0
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(2, 4): 701}
        lifecycle.compiler_groups = {}
        lifecycle.released_generations = set()
        lifecycle._release_reconciliation_progress = set()
        lifecycle._native_reconciled_groups = set()
        failed = False

        def reconciliation_hook(scope, _worker, _generation, _pgid):
            nonlocal failed
            if scope == "release-worker" and not failed:
                failed = True
                raise AuditInfrastructureError("injected worker-hook failure")

        with (
            mock.patch.object(lifecycle, "_kill_and_wait_empty") as kill,
            mock.patch.object(
                capability_runner,
                "_macos_reconciliation_completed",
                side_effect=reconciliation_hook,
            ),
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "worker-hook failure",
            ):
                lifecycle.release_generation(2, 4, None, deadline, force=True)
            lifecycle.release_generation(2, 4, None, deadline, force=True)

        self.assertEqual(kill.call_count, 1)
        self.assertEqual(kill.call_args.args[:2], (701, deadline))
        lifecycle.accountant.reconcile_group.assert_called_once_with(
            701,
            lifecycle.provider,
        )

    def test_macos_forced_compiler_does_not_resignal_after_wait_failure(self):
        deadline = time.monotonic() + 10.0
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(2, 4): 701}
        lifecycle.compiler_groups = {(2, 4, 5): [811]}
        lifecycle.released_generations = set()
        lifecycle._release_reconciliation_progress = set()
        lifecycle._native_reconciled_groups = set()
        kill_calls = []

        def kill_then_fail(pgid, _deadline, **kwargs):
            kill_calls.append(pgid)
            if pgid == 811:
                # The combined helper has signaled, then its wait fails.
                raise AuditInfrastructureError("post-signal compiler wait failure")

        with mock.patch.object(
            lifecycle,
            "_kill_and_wait_empty",
            side_effect=kill_then_fail,
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "compiler wait failure",
            ):
                lifecycle.release_generation(2, 4, None, deadline, force=True)
            lifecycle.release_generation(2, 4, None, deadline, force=True)

        self.assertEqual(kill_calls, [811, 701])
        self.assertEqual(lifecycle.worker_groups, {})
        self.assertEqual(lifecycle.compiler_groups, {})

    def test_macos_forced_worker_does_not_resignal_after_wait_failure(self):
        deadline = time.monotonic() + 10.0
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(2, 4): 701}
        lifecycle.compiler_groups = {}
        lifecycle.released_generations = set()
        lifecycle._release_reconciliation_progress = set()
        lifecycle._native_reconciled_groups = set()
        kill_calls = []

        def kill_then_fail(pgid, _deadline, **kwargs):
            kill_calls.append(pgid)
            # The combined helper has signaled, then its wait fails.
            raise AuditInfrastructureError("post-signal worker wait failure")

        with mock.patch.object(
            lifecycle,
            "_kill_and_wait_empty",
            side_effect=kill_then_fail,
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError,
                "worker wait failure",
            ):
                lifecycle.release_generation(2, 4, None, deadline, force=True)
            lifecycle.accountant.reconcile_group.side_effect = None
            lifecycle.release_generation(2, 4, None, deadline, force=True)

        self.assertEqual(kill_calls, [701])
        self.assertEqual(lifecycle.worker_groups, {})

    def _assert_macos_failed_signal_attempt_is_not_retried(
        self,
        *,
        compiler: bool,
    ) -> None:
        deadline = time.monotonic() + 10.0
        lifecycle = object.__new__(
            capability_runner._MacOSGenerationLifecycle,
        )
        lifecycle.accountant = mock.Mock()
        lifecycle.accountant.reconcile_group.return_value = True
        lifecycle.provider = mock.Mock()
        lifecycle.worker_groups = {(2, 4): 701}
        lifecycle.compiler_groups = (
            {(2, 4, 5): [811]} if compiler else {}
        )
        lifecycle.released_generations = set()
        lifecycle._release_reconciliation_progress = set()
        lifecycle._native_reconciled_groups = set()
        kill_calls = []

        def fail_first_signal_attempt(pgid, _deadline, **kwargs):
            kill_calls.append(pgid)
            if len(kill_calls) == 1:
                # Simulate killpg itself raising on the one allowed attempt.
                raise OSError("injected killpg failure")

        with mock.patch.object(
            lifecycle,
            "_kill_and_wait_empty",
            side_effect=fail_first_signal_attempt,
        ):
            with self.assertRaisesRegex(OSError, "killpg failure"):
                lifecycle.release_generation(2, 4, None, deadline, force=True)
            lifecycle.release_generation(2, 4, None, deadline, force=True)

        self.assertEqual(kill_calls, [811, 701] if compiler else [701])
        self.assertEqual(lifecycle.worker_groups, {})
        self.assertEqual(lifecycle.compiler_groups, {})

    def test_macos_failed_compiler_signal_attempt_is_not_retried(self):
        self._assert_macos_failed_signal_attempt_is_not_retried(compiler=True)

    def test_macos_failed_worker_signal_attempt_is_not_retried(self):
        self._assert_macos_failed_signal_attempt_is_not_retried(compiler=False)

    def test_macos_worker_registration_and_adoption_lost_replies_are_retryable(self):
        from gpu_capability_process_tree import (
            MacOSLibprocProvider,
            MacOSRegisteredPgidAccountant,
        )

        class LostReplyMembers(dict):
            faulted = False

            def __setitem__(_self, key, value):
                super().__setitem__(key, value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("registration reply lost")

        class LostReplyGroups(dict):
            faulted = False

            def __setitem__(_self, key, value):
                super().__setitem__(key, value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("adoption reply lost")

        report = capability_model.MacOSWorkerSessionReported(
            2, 4, 701, 701, "1:2"
        )
        for boundary in ("registration", "adoption"):
            with self.subTest(boundary=boundary):
                accountant = MacOSRegisteredPgidAccountant()
                if boundary == "registration":
                    accountant._members = LostReplyMembers()
                provider = object.__new__(MacOSLibprocProvider)
                provider._identity_and_residency = lambda pid, _pgid: (
                    capability_runner._owned_process_identity(
                        "macos", pid, "1:2"
                    ),
                    1,
                    0,
                )
                provider.reconcile_survivors = lambda _accountant, _pgid: ()
                lifecycle = capability_runner._MacOSGenerationLifecycle(
                    accountant,
                    (),
                    time.monotonic() + 10.0,
                    provider=provider,
                    permit_authority=object(),
                )
                if boundary == "adoption":
                    lifecycle.worker_groups = LostReplyGroups()
                with self.assertRaisesRegex(RuntimeError, "reply lost"):
                    lifecycle.accept_worker_session(report)
                lifecycle.accept_worker_session(report)
                self.assertEqual(lifecycle.worker_groups, {(2, 4): 701})
                self.assertTrue(accountant.reconcile_group(701, provider))
                self.assertTrue(accountant.memory_measurements().accounting_complete)

    def test_macos_task_permit_and_adoption_lost_replies_are_retryable(self):
        from gpu_capability_process_tree import (
            MacOSExecPermitAuthority,
            MacOSLibprocProvider,
            MacOSRegisteredPgidAccountant,
            OwnedProcessIdentity,
        )

        class LostReplyIssued(set):
            faulted = False

            def add(_self, value):
                super().add(value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("permit reply lost")

        class LostReplyGroups(list):
            faulted = False

            def append(_self, value):
                super().append(value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("adoption reply lost")

        identity = capability_model.FileIdentity(
            Path("/toolchain/clang"), None, 1, 2, 0, False
        )
        report = capability_model.CompilerPgidReported(
            2, 4, 5, capability_model.CompilerLaunchPurpose.AUDIT_ACCEPTED,
            811, 811, "3:4", identity, "a" * 64, "b" * 64,
        )
        for boundary in ("permit", "adoption"):
            with self.subTest(boundary=boundary):
                accountant = MacOSRegisteredPgidAccountant()
                provider = object.__new__(MacOSLibprocProvider)
                provider.reconcile_survivors = lambda _accountant, _pgid: ()
                authority = MacOSExecPermitAuthority(
                    accountant,
                    ("b" * 64,),
                    identity_verifier=lambda pid, _pgid: OwnedProcessIdentity(
                        "macos", pid, "3:4"
                    ),
                    executable_verifier=lambda _identity, _digest: "b" * 64,
                )
                if boundary == "permit":
                    authority._issued = LostReplyIssued()
                lifecycle = capability_runner._MacOSGenerationLifecycle(
                    accountant,
                    (),
                    time.monotonic() + 10.0,
                    provider=provider,
                    permit_authority=authority,
                )
                key = (2, 4, 5)
                if boundary == "adoption":
                    lifecycle.compiler_groups[key] = LostReplyGroups()
                with self.assertRaisesRegex(RuntimeError, "reply lost"):
                    lifecycle.permit_compiler(report)
                permit = lifecycle.permit_compiler(report)
                self.assertEqual(permit.pgid, 811)
                self.assertEqual(lifecycle.compiler_groups[key], [811])
                lifecycle.reconcile_task(*key)
                self.assertTrue(accountant.memory_measurements().accounting_complete)

    def test_macos_abort_kills_compiler_then_worker_and_reconciles_both(self):
        events = []
        deadline = time.monotonic() + 10.0

        class Accountant:
            def register_group(self, pgid, _leader, purpose):
                events.append(("register", pgid, purpose))

            def reconcile_group(self, pgid, _provider):
                events.append(("reconcile", pgid))
                return True

            def memory_measurements(self):
                return capability_model.MacOSRunMemoryMeasurements(
                    0, 0, 0, 0, 0, True
                )

        class Provider:
            def _identity_and_residency(self, pid, _pgid):
                return capability_runner._owned_process_identity(
                    "macos", pid, "1:2"
                ), 1, 0

            def reconcile_survivors(self, _accountant, pgid):
                events.append(("empty", pgid))
                return ()

            def observe(self, *_args):
                pass

        class Authority:
            def permit_compiler(self, report):
                return capability_model.CompilerExecPermit(
                    report.worker_index, report.generation,
                    report.task_id, report.pgid
                )

        lifecycle = capability_runner._MacOSGenerationLifecycle(
            Accountant(), (), deadline,
            provider=Provider(), permit_authority=Authority(),
        )
        lifecycle.accept_worker_session(
            capability_model.MacOSWorkerSessionReported(1, 2, 701, 701, "1:2")
        )
        lifecycle.permit_compiler(SimpleNamespace(
            worker_index=1, generation=2, task_id=3, pgid=811
        ))
        with mock.patch.object(
            capability_runner.os,
            "killpg",
            side_effect=lambda pgid, _signal: events.append(("kill", pgid)),
            create=True,
        ), mock.patch.object(
            capability_runner.signal, "SIGKILL", 9, create=True
        ):
            lifecycle.release_generation(
                1, 2, None, deadline, force=True
            )
        self.assertLess(events.index(("kill", 811)),
                        events.index(("reconcile", 811)))
        self.assertLess(events.index(("reconcile", 811)),
                        events.index(("kill", 701)))
        self.assertLess(events.index(("kill", 701)),
                        events.index(("reconcile", 701)))

    def test_macos_release_retains_maps_until_reconciliation_succeeds(self):
        deadline = time.monotonic() + 10.0
        failed = False

        class Accountant:
            def reconcile_group(_self, pgid, _provider):
                nonlocal failed
                if pgid == 811 and not failed:
                    failed = True
                    raise AuditInfrastructureError("survivor remains")
                return True

        lifecycle = object.__new__(capability_runner._MacOSGenerationLifecycle)
        lifecycle.accountant = Accountant()
        lifecycle.provider = object()
        lifecycle.worker_groups = {(1, 2): 701}
        lifecycle.compiler_groups = {(1, 2, 3): [811]}
        lifecycle.released_generations = set()

        with self.assertRaisesRegex(
            AuditInfrastructureError, "survivor remains"
        ):
            lifecycle.release_generation(1, 2, None, deadline, force=False)
        self.assertEqual(lifecycle.worker_groups, {(1, 2): 701})
        self.assertEqual(lifecycle.compiler_groups, {(1, 2, 3): [811]})

        lifecycle.release_generation(1, 2, None, deadline, force=False)
        self.assertEqual(lifecycle.worker_groups, {})
        self.assertEqual(lifecycle.compiler_groups, {})
        self.assertIn((1, 2), lifecycle.released_generations)
        lifecycle.release_generation(1, 2, None, deadline, force=False)


if __name__ == "__main__":
    unittest.main()
