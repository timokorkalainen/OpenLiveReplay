from __future__ import annotations

import ast
import dataclasses
import base64
import contextlib
import gc
import hashlib
import inspect
import json
import multiprocessing
import os
import stat
import struct
import sys
import tempfile
import threading
import time
import traceback
import tracemalloc
import unittest
import weakref
from concurrent.futures import ThreadPoolExecutor
from array import array
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_capability_cache as capability_cache  # noqa: E402
import gpu_capability_runner as capability_runner  # noqa: E402
import gpu_capability_source_audit as capability_audit  # noqa: E402
from gpu_capability_cache import (  # noqa: E402
    CompilerInspectionCache,
    ConfigurationAuditCache,
    PreprocessCache,
    audit_cache_key,
    _directory_identity,
    _hash_cache,
    _hash_lock,
    _remove_held_flat_directory,
    _PublicationGuard,
    compiler_inspection_cache_key,
)
from gpu_capability_model import (  # noqa: E402
    AuditResultFinding,
    AuditInfrastructureError,
    CompactResultColdSlot,
    CompactResultDraftBounds,
    CompactResultMemoryBudget,
    CompactTokenSequence,
    CompilerExecutableCapability,
    CompilerFamily,
    CompilerInspection,
    DependencyRootBinding,
    DependencyDigest,
    FileIdentity,
    ConfigurationAuditPublicationPermit,
    ConfigurationAuditResult,
    ConfigurationAuditTransportOutcome,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    PerTaskCompactReservation,
    StreamingResultAggregator,
    WorkerStageTimings,
    encode_canonical_summary,
    build_dependency_root_authority,
    encode_compiler_inspection,
)


def _publish_equal_inspection_in_spawned_process(
    cache_root: str,
    source_root: str,
    toolchain_root: str,
    compiler_path: str,
    start_event,
    result_queue,
) -> None:
    try:
        source = Path(source_root)
        toolchain = Path(toolchain_root)
        compiler = Path(compiler_path)
        authority = build_dependency_root_authority(
            source, {"toolchain": toolchain}
        )
        metadata = compiler.stat()
        identity = FileIdentity(
            compiler.resolve(), None, int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            0, False,
        )
        inspection = CompilerInspection(
            CompilerFamily.GCC,
            identity,
            hashlib.sha256(compiler.read_bytes()).hexdigest(),
            "g++ (GCC) 14.1.0",
            "b" * 64,
            "c" * 64,
            "d" * 64,
        )
        if not start_event.wait(timeout=5.0):
            raise RuntimeError("spawned inspection publisher start timed out")
        published = CompilerInspectionCache(Path(cache_root)).publish(
            compiler.resolve(), CompilerFamily.GCC, {"PATH": str(toolchain)},
            authority, "e" * 64, "d" * 64, "f" * 64, inspection,
            time.monotonic() + 10.0,
        )
        result_queue.put(("ok", published.driver_fingerprint))
    except BaseException as error:
        result_queue.put(("error", type(error).__name__, str(error)))


def _send_owned_audit_transport_from_spawned_worker(
    connection,
    capability,
    result: ConfigurationAuditResult,
    mode: str,
) -> None:
    try:
        reservation = PerTaskCompactReservation.for_worker_transport(capability)
        reservation.require_before_discovery(
            capability.task_id, capability.generation
        )
        payload = capability_cache.encode_configuration_audit_result(result)
        path_bytes = sum(
            len(item.stable_role.encode("ascii"))
            + len(item.role_relative_path.as_posix().encode("utf-8"))
            + len(str(item.identity.canonical).encode("utf-8"))
            + (
                len(item.identity.relative.as_posix().encode("utf-8"))
                if item.identity.relative is not None
                else 0
            )
            for item in result.dependencies
        ) + sum(
            len(path.as_posix().encode("utf-8"))
            for path in result.reached_production
        ) + sum(
            len(item.path.as_posix().encode("utf-8"))
            for item in result.findings
        )
        bounds = CompactResultDraftBounds(
            len(result.dependencies),
            len(result.reached_production),
            len(result.findings),
            path_bytes,
            sum(len(item.expression.encode("utf-8")) for item in result.findings),
            sum(len(item.reason.encode("utf-8")) for item in result.findings),
        )
        reservation.require_within_pre_dispatch_reservation(
            capability.task_id, len(payload), bounds, 4096
        )
        reservation.record_exact_canonical_json(capability.task_id, len(payload))
        ownership = reservation.begin_result_ownership(
            capability.task_id, capability.generation
        )
        owned = dataclasses.replace(result, _transport_ownership=ownership)
        stages = WorkerStageTimings(1.0, 2.0, 3.0, 4.0)
        transport = capability_cache.encode_configuration_audit_result_transport(
            owned, capability, 17, stages
        )
        if mode.startswith("forge-"):
            changes = {
                "forge-task": {"task_id": capability.task_id + "-forged"},
                "forge-generation": {"generation": capability.generation + 1},
                "forge-nonce": {"nonce": "0" * 64},
                "forge-serial": {"serial": capability.serial + 1},
                "forge-hash": {"payload_sha256": "0" * 64},
                "forge-undercharge": {
                    "charged_bytes": transport.receipt.charged_bytes - 1
                },
                "forge-overcharge": {
                    "charged_bytes": transport.receipt.charged_bytes + 1
                },
            }
            forged_receipt = dataclasses.replace(
                transport.receipt, **changes[mode]
            )
            transport = dataclasses.replace(
                transport, receipt=forged_receipt
            )
        outcome = ConfigurationAuditTransportOutcome(transport, 17, stages)
        if mode == "worker-death":
            os._exit(19)
        try:
            capability_cache.send_configuration_audit_transport(
                connection, outcome
            )
        except AuditInfrastructureError:
            os._exit(17)
    finally:
        connection.close()


def _write_partial_audit_frame_from_spawned_worker(
    connection,
    frame: bytes,
    wire_bytes: int,
    mode: str,
    ready_event,
    prefix_frame: bytes | None = None,
) -> None:
    if prefix_frame is not None:
        connection.send_bytes(prefix_frame)
    wire = struct.pack("!i", len(frame)) + frame
    os.write(connection.fileno(), wire[:wire_bytes])
    ready_event.set()
    if mode == "exit":
        os._exit(23)
    try:
        time.sleep(30.0)
    finally:
        connection.close()


def _acquire_shared_lock_in_spawned_process(
    cache_root: str,
    carrier_name: str,
    result_queue,
    namespace_name: str | None = None,
) -> None:
    try:
        root = Path(cache_root)
        identity = _directory_identity(capability_cache._ordinary_directory(root))
        with capability_cache._SharedCacheFileLock(
            root / carrier_name,
            root,
            identity,
            time.monotonic() + 5.0,
            namespace_path=(
                root / namespace_name if namespace_name is not None else None
            ),
        ):
            result_queue.put(("ok",))
    except BaseException as error:
        result_queue.put(("error", type(error).__name__, str(error)))


def _hold_shared_lock_in_spawned_process(
    cache_root: str,
    carrier_name: str,
    ready_event,
    release_event,
    result_queue,
    namespace_name: str | None = None,
) -> None:
    try:
        root = Path(cache_root)
        identity = _directory_identity(capability_cache._ordinary_directory(root))
        with capability_cache._SharedCacheFileLock(
            root / carrier_name,
            root,
            identity,
            time.monotonic() + 5.0,
            namespace_path=(
                root / namespace_name if namespace_name is not None else None
            ),
        ):
            ready_event.set()
            if not release_event.wait(timeout=5.0):
                raise RuntimeError("shared cache lock release timed out")
        result_queue.put(("ok",))
    except BaseException as error:
        result_queue.put(("error", type(error).__name__, str(error)))


def _spawned_preprocess_fixture(
    fixture_root: str,
) -> tuple[PreprocessConfiguration, FileIdentity, FileIdentity]:
    root = Path(fixture_root)
    main_path = root / "playback" / "a.cpp"
    header_path = root / "playback" / "a.h"
    compiler = root / "toolchain" / "g++.exe"

    def identity(path: Path, relative: str | None = None) -> FileIdentity:
        metadata = path.stat()
        return FileIdentity(
            path.resolve(),
            PurePosixPath(relative) if relative is not None else None,
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            len(path.read_bytes().splitlines()),
            relative is not None,
        )

    main = identity(main_path, "playback/a.cpp")
    header = identity(header_path, "playback/a.h")
    authority = build_dependency_root_authority(root, {})
    executable = identity(compiler)
    binding = DependencyRootBinding(
        "toolchain",
        compiler.parent.resolve(),
        FileIdentity(
            compiler.parent.resolve(), None, executable.device,
            executable.inode, 0, False,
        ),
    )
    capability = CompilerExecutableCapability(
        "windows", dataclasses.replace(executable, line_count=0),
        "1" * 64, "2" * 64, object(), binding, (), (), "3" * 64, (),
    )
    configuration = PreprocessConfiguration(
        entry_id="compile_commands.json:0",
        family=CompilerFamily.GCC,
        compiler=compiler,
        working_directory=root,
        source=main,
        arguments=("-std=c++17", str(main_path)),
        environment_digest="environment-one",
        digest="compiler-fingerprint-one",
        dependency_root_authority_digest=authority.portable_authority_digest,
        compiler_capability_digest=capability.capability_digest,
        compiler_capability=capability,
    )
    return configuration, main, header


def _publish_preprocess_cache_in_spawned_process(
    cache_root: str,
    fixture_root: str,
    validation_fails: bool,
    validation_entered_path: str,
    release_validation_path: str,
    result_connection,
) -> None:
    deadline = time.monotonic() + 15.0
    configuration, main, header = _spawned_preprocess_fixture(fixture_root)
    tokens = CompactTokenSequence._from_packed(
        configuration,
        spellings=(b"lease", b".", b"nativeHandle"),
        identities=(main, header),
        spelling_ids=array("I", [0, 1, 2]),
        identity_ids=array("I", [0, 0, 1]),
        inclusion_ids=array("I", [1, 1, 2]),
        original_lines=array("I", [1, 1, 1]),
    )
    view = PreprocessedTranslationUnitView(
        configuration, tokens, (main, header)
    )
    cache = PreprocessCache(Path(cache_root))
    snapshots = cache._snapshot_dependencies(view.dependencies, force=True)

    def final_validation() -> None:
        Path(validation_entered_path).write_bytes(b"1")
        while (
            not Path(release_validation_path).exists()
            and time.monotonic() < deadline
        ):
            time.sleep(0.01)
        if not Path(release_validation_path).exists():
            raise AssertionError("final validation release timed out")
        if validation_fails:
            raise AuditInfrastructureError("forced final validation failure")

    try:
        result = cache._publish_stabilized(
            view,
            snapshots,
            final_validation=final_validation,
            deadline=deadline,
        )
        result_connection.send(("ok", result is not None))
    except BaseException as error:
        result_connection.send(("error", type(error).__name__, str(error)))
    finally:
        result_connection.close()


def _load_preprocess_cache_in_spawned_process(
    cache_root: str,
    fixture_root: str,
    load_started_path: str,
    result_connection,
) -> None:
    try:
        configuration, _main, _header = _spawned_preprocess_fixture(fixture_root)
        Path(load_started_path).write_bytes(b"1")
        result = PreprocessCache(Path(cache_root)).load(
            configuration, time.monotonic() + 15.0
        )
        result_connection.send(("ok", result is not None))
    except BaseException as error:
        result_connection.send(("error", type(error).__name__, str(error)))
    finally:
        result_connection.close()


class _PreprocessCacheFixture:
    def setUp(self) -> None:
        with _hash_lock:
            _hash_cache.clear()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.cache_root = self.root / "cache"
        self.main_path = self.root / "playback" / "a.cpp"
        self.header_path = self.root / "playback" / "a.h"
        self.compiler = self.root / "toolchain" / "g++.exe"
        self.main_path.parent.mkdir(parents=True)
        self.compiler.parent.mkdir()
        self.main_path.write_text("lease.nativeHandle();\n", encoding="utf-8")
        self.header_path.write_text("#pragma once\n", encoding="utf-8")
        self.compiler.write_bytes(b"compiler-fingerprint-one")
        self.main = self.identity(self.main_path, "playback/a.cpp")
        self.header = self.identity(self.header_path, "playback/a.h")
        self.dependency_roots = build_dependency_root_authority(self.root, {})
        executable = self.identity(self.compiler)
        binding = DependencyRootBinding(
            "toolchain", self.compiler.parent.resolve(),
            FileIdentity(self.compiler.parent.resolve(), None, executable.device,
                         executable.inode, 0, False),
        )
        capability = CompilerExecutableCapability(
            "windows", dataclasses.replace(executable, line_count=0),
            "1" * 64, "2" * 64, object(), binding, (), (), "3" * 64, (),
        )
        self.configuration = PreprocessConfiguration(
            entry_id="compile_commands.json:0",
            family=CompilerFamily.GCC,
            compiler=self.compiler,
            working_directory=self.root,
            source=self.main,
            arguments=("-std=c++17", str(self.main_path)),
            environment_digest="environment-one",
            digest="compiler-fingerprint-one",
            dependency_root_authority_digest=(
                self.dependency_roots.portable_authority_digest
            ),
            compiler_capability_digest=capability.capability_digest,
            compiler_capability=capability,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def identity(path: Path, relative: str | None = None) -> FileIdentity:
        metadata = path.stat()
        return FileIdentity(
            canonical=path.resolve(),
            relative=PurePosixPath(relative) if relative is not None else None,
            device=int(metadata.st_dev),
            inode=int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            line_count=len(path.read_bytes().splitlines()),
            production=relative is not None,
        )

    def view(
        self,
        *,
        configuration: PreprocessConfiguration | None = None,
        dependencies: tuple[FileIdentity, ...] | None = None,
        repeats: int = 1,
    ) -> PreprocessedTranslationUnitView:
        configuration = configuration or self.configuration
        tokens = CompactTokenSequence._from_packed(
            configuration,
            spellings=(b"lease", b".", b"nativeHandle"),
            identities=(self.main, self.header),
            spelling_ids=array("I", [0, 1, 2] * repeats),
            identity_ids=array("I", [0, 0, 1] * repeats),
            inclusion_ids=array("I", [1, 1, 2] * repeats),
            original_lines=array("I", [1, 1, 1] * repeats),
        )
        return PreprocessedTranslationUnitView(
            configuration,
            tokens,
            dependencies or (self.main, self.header),
        )

    def entry(self, cache: PreprocessCache | None = None) -> Path:
        cache = cache or PreprocessCache(self.cache_root)
        return self.cache_root / cache._configuration_key(self.configuration)

    def _assert_hit_requires_every_dependency_content_hash(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        self.assertIsNotNone(cache.load(self.configuration))
        self.header_path.write_text("changed\n", encoding="utf-8")
        self.assertIsNone(cache.load(self.configuration))


class CompilerInspectionCacheTests(unittest.TestCase, _PreprocessCacheFixture):
    def test_cache_module_has_no_optimization_sensitive_assertions(self):
        path = Path(__file__).resolve().with_name("gpu_capability_cache.py")
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=path.name)
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(tree)))

    def setUp(self) -> None:
        _PreprocessCacheFixture.setUp(self)
        self.source = self.root / "inspection-source"
        self.toolchain = self.root / "inspection-toolchain"
        inspection_cache_root = self.root / "inspection-cache"
        self.source.mkdir()
        self.toolchain.mkdir()
        self.compiler = self.toolchain / "g++.exe"
        self.compiler.write_bytes(b"compiler-one")
        self.authority = build_dependency_root_authority(
            self.source, {"toolchain": self.toolchain}
        )
        self.cache = CompilerInspectionCache(inspection_cache_root)
        metadata = self.compiler.stat()
        self.compiler_identity = FileIdentity(
            self.compiler.resolve(), None, int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            0, False,
        )
        self.environment = {"PATH": str(self.toolchain)}
        self.capability_digest = "d" * 64
        self.closure_digest = "f" * 64

    def tearDown(self) -> None:
        _PreprocessCacheFixture.tearDown(self)

    def inspection(self) -> CompilerInspection:
        return CompilerInspection(
            CompilerFamily.GCC,
            self.compiler_identity,
            hashlib.sha256(self.compiler.read_bytes()).hexdigest(),
            "g++ (GCC) 14.1.0",
            "b" * 64,
            "c" * 64,
            "d" * 64,
        )

    def test_exact_inspection_manifest_round_trips_under_local_authority(self):
        inspection = self.inspection()
        published = self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        self.assertEqual(published, inspection)
        self.assertEqual(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            ),
            inspection,
        )

    def test_inspection_cache_hit_uses_held_capability_without_rehashing(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        with mock.patch(
            "gpu_capability_model._current_executable_sha256",
            side_effect=AssertionError("cache hit reopened compiler for hashing"),
        ) as current_sha256:
            loaded = self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
                held_executable_identity=self.compiler_identity,
                held_executable_sha256=inspection.executable_sha256,
            )
        self.assertEqual(loaded, inspection)
        current_sha256.assert_not_called()

    def test_inspection_cache_rejects_mismatched_held_capability_binding(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "held capability"):
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
                held_executable_identity=self.compiler_identity,
                held_executable_sha256="0" * 64,
            )

    def test_portable_inspection_key_excludes_path_and_native_identity(self):
        other_source = self.root / "other-source"
        other_toolchain = self.root / "other-toolchain"
        other_source.mkdir()
        other_toolchain.mkdir()
        other_compiler = other_toolchain / "g++.exe"
        other_compiler.write_bytes(self.compiler.read_bytes())
        other_authority = build_dependency_root_authority(
            other_source, {"toolchain": other_toolchain}
        )
        first = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, {}, self.authority,
            "e" * 64, executable_capability_digest="d" * 64,
            resolved_runtime_closure_digest="f" * 64,
        )
        second = compiler_inspection_cache_key(
            other_compiler.resolve(), CompilerFamily.GCC, {}, other_authority,
            "e" * 64, executable_capability_digest="d" * 64,
            resolved_runtime_closure_digest="f" * 64,
        )
        self.assertEqual(first, second)

    def test_inspection_cache_key_binds_capability_and_runtime_closure_digests(self):
        parameters = inspect.signature(compiler_inspection_cache_key).parameters
        self.assertIn("executable_capability_digest", parameters)
        self.assertIn("resolved_runtime_closure_digest", parameters)
        common = (
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
        )
        first = compiler_inspection_cache_key(
            *common, executable_capability_digest="a" * 64,
            resolved_runtime_closure_digest="b" * 64,
        )
        second = compiler_inspection_cache_key(
            *common, executable_capability_digest="c" * 64,
            resolved_runtime_closure_digest="d" * 64,
        )
        self.assertNotEqual(first, second)

    def test_inspection_publish_rejects_contradictory_embedded_capability_digest(self):
        contradictory = dataclasses.replace(
            self.inspection(), executable_capability_digest="a" * 64
        )
        with self.assertRaisesRegex(
            AuditInfrastructureError, "capability digest"
        ):
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, contradictory, time.monotonic() + 10.0,
            )

    def test_inspection_load_rejects_contradictory_embedded_capability_digest(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        path = self.cache._path(key)
        document = json.loads(path.read_text(encoding="ascii"))
        contradictory = dataclasses.replace(
            inspection, executable_capability_digest="a" * 64
        )
        document["inspection"] = base64.b64encode(
            encode_compiler_inspection(contradictory)
        ).decode("ascii")
        path.write_text(
            json.dumps(document, ensure_ascii=True, separators=(",", ":")),
            encoding="ascii",
        )
        self.assertIsNone(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )
        )

    def test_equal_portable_digest_with_different_local_roots_fails_before_access(self):
        other_source = self.root / "second-source"
        other_toolchain = self.root / "second-toolchain"
        other_source.mkdir()
        other_toolchain.mkdir()
        other_authority = build_dependency_root_authority(
            other_source, {"toolchain": other_toolchain}
        )
        self.assertEqual(
            other_authority.portable_authority_digest,
            self.authority.portable_authority_digest,
        )
        with mock.patch.object(
            self.cache, "_path", side_effect=AssertionError("cache accessed")
        ), self.assertRaisesRegex(AuditInfrastructureError, "local dependency authority"):
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                other_authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )

    def test_changed_compiler_content_is_a_miss_without_old_manifest_use(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        self.compiler.write_bytes(b"compiler-two")
        self.assertIsNone(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )
        )

    def test_cache_key_load_and_publish_do_not_rehash_held_compiler(self):
        inspection = self.inspection()
        with mock.patch(
            "gpu_capability_cache._held_compiler_snapshot",
            wraps=sys.modules["gpu_capability_cache"]._held_compiler_snapshot,
        ) as snapshot:
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, inspection, time.monotonic() + 10.0,
            )
            self.assertEqual(
                self.cache.load(
                    self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                    self.authority, "e" * 64, self.capability_digest,
                    self.closure_digest, time.monotonic() + 10.0,
                ),
                inspection,
            )
        self.assertEqual(snapshot.call_count, 0)

    def test_corrupt_authentic_inspection_payload_is_a_cache_miss(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        path = self.cache._path(key)
        document = json.loads(path.read_text(encoding="ascii"))
        embedded = json.loads(base64.b64decode(document["inspection"]).decode("ascii"))
        embedded["compiler_family"] = "not-a-family"
        document["inspection"] = base64.b64encode(
            json.dumps(embedded, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        ).decode("ascii")
        path.write_text(
            json.dumps(document, ensure_ascii=True, separators=(",", ":")),
            encoding="ascii",
        )
        self.assertIsNone(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )
        )

    def test_valid_shaped_inspection_payload_corruption_is_a_cache_miss(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        path = self.cache._path(key)
        document = json.loads(path.read_text(encoding="ascii"))
        embedded = json.loads(base64.b64decode(document["inspection"]).decode("ascii"))
        embedded["driver_fingerprint"] = "a" * 64
        document["inspection"] = base64.b64encode(
            json.dumps(
                embedded, ensure_ascii=True, separators=(",", ":")
            ).encode("ascii")
        ).decode("ascii")
        path.write_text(
            json.dumps(document, ensure_ascii=True, separators=(",", ":")),
            encoding="ascii",
        )
        self.assertIsNone(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )
        )

    def test_hardlinked_inspection_manifest_is_fatal(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        manifest = self.cache._path(key)
        alias = self.root / "inspection-manifest-alias"
        try:
            os.link(manifest, alias)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "alias|namespace|ordinary"
        ):
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )

    def test_inspection_publish_rejects_preexisting_hardlinked_destination(self):
        inspection = self.inspection()
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        destination = self.cache._path(key)
        seed = self.root / "inspection-hardlink-seed"
        seed.write_bytes(b"unsafe destination")
        try:
            os.link(seed, destination)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "alias|namespace|ordinary"
        ):
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, inspection, time.monotonic() + 10.0,
            )
        self.assertEqual(os.path.samefile(seed, destination), True)

    def test_existing_equal_inspection_winner_is_preserved(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        destination = self.cache._path(key)
        before = destination.stat()
        self.assertEqual(
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, inspection, time.monotonic() + 10.0,
            ),
            inspection,
        )
        after = destination.stat()
        self.assertEqual(
            (int(after.st_dev), int(after.st_ino)),
            (int(before.st_dev), int(before.st_ino)),
        )

    def test_distinct_instances_accept_one_equal_concurrent_inspection_winner(self):
        inspection = self.inspection()
        caches = (self.cache, CompilerInspectionCache(self.cache.root))
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        destination = self.cache._path(key)
        collision = threading.Barrier(2)
        winner_ready = threading.Event()
        replace_lock = threading.Lock()
        replace_calls: list[Path] = []
        real_replace = os.replace
        results: list[CompilerInspection] = []
        failures: list[BaseException] = []

        def collide_replace(source, target):
            if (
                Path(source).name.startswith(".tmp-inspection-")
                and Path(target) == destination
            ):
                with replace_lock:
                    index = len(replace_calls)
                    replace_calls.append(Path(source))
                try:
                    collision.wait(timeout=1.0)
                except threading.BrokenBarrierError:
                    pass
                if index == 0:
                    real_replace(source, target)
                    winner_ready.set()
                    return
                winner_ready.wait(timeout=1.0)
                raise FileExistsError("deterministic concurrent winner")
            real_replace(source, target)

        def publish(cache):
            try:
                results.append(cache.publish(
                    self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                    self.authority, "e" * 64, self.capability_digest,
                    self.closure_digest, inspection, time.monotonic() + 10.0,
                ))
            except BaseException as error:
                failures.append(error)

        with mock.patch("gpu_capability_cache.os.replace", side_effect=collide_replace):
            threads = [threading.Thread(target=publish, args=(cache,)) for cache in caches]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10.0)
        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(failures, [])
        self.assertEqual(results, [inspection, inspection])
        self.assertEqual(len(replace_calls), 1)
        self.assertEqual(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            ),
            inspection,
        )

    def test_spawned_processes_accept_one_equal_inspection_winner(self):
        context = multiprocessing.get_context("spawn")
        start = context.Event()
        results = context.Queue()
        arguments = (
            str(self.cache.root),
            str(self.source),
            str(self.toolchain),
            str(self.compiler),
            start,
            results,
        )
        processes = [
            context.Process(
                target=_publish_equal_inspection_in_spawned_process,
                args=arguments,
            )
            for _index in range(2)
        ]
        for process in processes:
            process.start()
        start.set()
        for process in processes:
            process.join(timeout=15.0)
        self.assertFalse(any(process.is_alive() for process in processes))
        self.assertEqual([process.exitcode for process in processes], [0, 0])
        observed = [results.get(timeout=2.0) for _process in processes]
        self.assertEqual(observed, [("ok", "b" * 64), ("ok", "b" * 64)])
        self.assertEqual(
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            ),
            self.inspection(),
        )
        results.close()
        results.join_thread()

    def test_inspection_publication_lock_honors_deadline_and_cancellation(self):
        inspection = self.inspection()
        lock_type = sys.modules["gpu_capability_cache"]._SharedCacheFileLock
        root_lock = lock_type(
            self.cache.root / ".compiler-inspection-root.lock",
            self.cache.root,
            self.cache._root_identity,
            time.monotonic() + 10.0,
        )
        with root_lock, self.assertRaisesRegex(
            AuditInfrastructureError, "lock deadline"
        ):
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, inspection, time.monotonic() + 0.05,
            )
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, inspection, time.monotonic() + 10.0,
                cancelled,
            )

    def test_distinct_lock_lanes_enter_concurrently(self):
        locks_by_path = {}
        for index in range(64):
            candidate = self.cache._key_lock(
                f"distinct-{index}", time.monotonic() + 10.0
            )
            locks_by_path.setdefault(candidate.path, candidate)
            if len(locks_by_path) == 2:
                break
        self.assertEqual(len(locks_by_path), 2)
        first, second = locks_by_path.values()
        entered = threading.Event()
        failures: list[BaseException] = []

        def acquire_distinct_lane() -> None:
            try:
                with second:
                    entered.set()
            except BaseException as error:
                failures.append(error)

        first.__enter__()
        waiter = threading.Thread(target=acquire_distinct_lane)
        waiter.start()
        try:
            entered_while_first_held = entered.wait(timeout=0.5)
        finally:
            first.__exit__(None, None, None)
            waiter.join(timeout=5.0)
        self.assertFalse(waiter.is_alive())
        self.assertEqual(failures, [])
        self.assertTrue(
            entered_while_first_held,
            "distinct lock lane was serialized behind the active lane",
        )

    def test_same_lock_lane_remains_serialized(self):
        first = self.cache._key_lock("same-lane", time.monotonic() + 10.0)
        second = self.cache._key_lock("same-lane", time.monotonic() + 5.0)
        self.assertEqual(first.path, second.path)
        attempted = threading.Event()
        entered = threading.Event()
        failures: list[BaseException] = []

        def acquire_same_lane() -> None:
            attempted.set()
            try:
                with second:
                    entered.set()
            except BaseException as error:
                failures.append(error)

        first.__enter__()
        waiter = threading.Thread(target=acquire_same_lane)
        waiter.start()
        self.assertTrue(attempted.wait(timeout=2.0))
        try:
            self.assertFalse(entered.wait(timeout=0.15))
        finally:
            first.__exit__(None, None, None)
            waiter.join(timeout=5.0)
        self.assertFalse(waiter.is_alive())
        self.assertEqual(failures, [])
        self.assertTrue(entered.is_set())

    def test_legacy_namespace_marker_upgrades_to_bounded_spawn_reusable_ledger(self):
        first = self.cache._key_lock(
            "legacy-namespace-upgrade", time.monotonic() + 10.0
        )
        namespace = first.namespace_path
        self.assertIsNotNone(namespace)
        namespace_anchor = capability_cache._lock_carrier_anchor_path(namespace)
        namespace.write_bytes(b"1")
        try:
            os.link(namespace, namespace_anchor)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        before = namespace.stat()
        with first:
            pass
        after = namespace.stat()
        self.assertEqual(
            (int(after.st_dev), int(after.st_ino)),
            (int(before.st_dev), int(before.st_ino)),
        )
        self.assertEqual(after.st_size, capability_cache._LOCK_NAMESPACE_BYTES)
        with namespace.open("rb") as stream:
            capability_cache._validate_lock_namespace_ledger(stream)

        context = multiprocessing.get_context("spawn")
        results = context.Queue()
        process = context.Process(
            target=_acquire_shared_lock_in_spawned_process,
            args=(
                str(self.cache.root),
                first.path.name,
                results,
                namespace.name,
            ),
        )
        try:
            process.start()
            process.join(timeout=10.0)
            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode, 0)
            self.assertEqual(results.get(timeout=2.0), ("ok",))
            self.assertEqual(
                namespace.stat().st_size,
                capability_cache._LOCK_NAMESPACE_BYTES,
            )
        finally:
            if process.is_alive():
                process.terminate()
                process.join(timeout=5.0)
            if process.pid is not None:
                process.close()
            results.close()
            results.join_thread()

    def test_incomplete_lane_generation_commit_fails_closed(self):
        first = self.cache._key_lock(
            "incomplete-generation-commit", time.monotonic() + 10.0
        )
        namespace = first.namespace_path
        self.assertIsNotNone(namespace)
        with first:
            pass
        lane = capability_cache._lock_namespace_lane_index(first.path)
        second_record = (
            1
            + capability_cache._LOCK_NAMESPACE_HEADER_BYTES
            + lane * capability_cache._LOCK_NAMESPACE_SLOT_BYTES
            + capability_cache._LOCK_NAMESPACE_RECORD_BYTES
        )
        with namespace.open("r+b") as stream:
            stream.seek(second_record)
            stream.write(bytes(capability_cache._LOCK_NAMESPACE_RECORD_BYTES))
            stream.flush()
            os.fsync(stream.fileno())
        with self.assertRaisesRegex(
            AuditInfrastructureError, "lock namespace"
        ):
            with self.cache._key_lock(
                "incomplete-generation-commit", time.monotonic() + 5.0
            ):
                self.fail("incomplete generation commit entered the lane")

    def test_post_replacement_opener_rejects_split_lock_namespace(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".compiler-inspection-root.lock"
        with lock_type(
            carrier,
            self.cache.root,
            self.cache._root_identity,
            time.monotonic() + 10.0,
        ):
            pass
        displaced = carrier.with_name(f"{carrier.name}.displaced")
        carrier.rename(displaced)
        carrier.write_bytes(b"1")
        try:
            with self.assertRaisesRegex(
                AuditInfrastructureError, "lock namespace"
            ):
                with lock_type(
                    carrier,
                    self.cache.root,
                    self.cache._root_identity,
                    time.monotonic() + 1.0,
                ):
                    self.fail("replacement carrier entered a split namespace")
        finally:
            carrier.unlink(missing_ok=True)
            displaced.rename(carrier)

    def test_spawned_post_replacement_opener_is_fatal(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".spawned-replacement-root.lock"
        with lock_type(
            carrier,
            self.cache.root,
            self.cache._root_identity,
            time.monotonic() + 10.0,
        ):
            pass
        displaced = carrier.with_name(f"{carrier.name}.displaced")
        carrier.rename(displaced)
        carrier.write_bytes(b"1")
        context = multiprocessing.get_context("spawn")
        results = context.Queue()
        process = context.Process(
            target=_acquire_shared_lock_in_spawned_process,
            args=(str(self.cache.root), carrier.name, results),
        )
        try:
            process.start()
            process.join(timeout=10.0)
            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode, 0)
            observed = results.get(timeout=2.0)
            self.assertEqual(observed[0:2], ("error", "AuditInfrastructureError"))
            self.assertRegex(observed[2], "lock namespace")
        finally:
            if process.is_alive():
                process.terminate()
                process.join(timeout=5.0)
            results.close()
            results.join_thread()
            process.close()
            carrier.unlink(missing_ok=True)
            displaced.rename(carrier)

    @unittest.skipIf(os.name == "nt", "Windows denies active carrier replacement")
    def test_spawned_whole_lane_pair_replacement_under_stable_authority_is_fatal(self):
        first = self.cache._key_lock(
            "spawned-whole-pair-replacement", time.monotonic() + 10.0
        )
        carrier = first.path
        anchor = capability_cache._lock_carrier_anchor_path(carrier)
        namespace = first.namespace_path
        self.assertIsNotNone(namespace)
        displaced = carrier.with_name(f"{carrier.name}.displaced")
        displaced_anchor = anchor.with_name(f"{anchor.name}.displaced")
        context = multiprocessing.get_context("spawn")
        results = context.Queue()
        process = context.Process(
            target=_acquire_shared_lock_in_spawned_process,
            args=(
                str(self.cache.root),
                carrier.name,
                results,
                namespace.name,
            ),
        )
        first.__enter__()
        try:
            carrier.rename(displaced)
            anchor.rename(displaced_anchor)
            anchor.write_bytes(b"1")
            os.link(anchor, carrier)
            process.start()
            process.join(timeout=10.0)
            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode, 0)
            observed = results.get(timeout=2.0)
            self.assertEqual(observed[0:2], ("error", "AuditInfrastructureError"))
            self.assertRegex(observed[2], "lock namespace")
        finally:
            if process.is_alive():
                process.terminate()
                process.join(timeout=5.0)
            if process.pid is not None:
                process.close()
            results.close()
            results.join_thread()
            first.__exit__(None, None, None)
            carrier.unlink(missing_ok=True)
            anchor.unlink(missing_ok=True)
            displaced.rename(carrier)
            displaced_anchor.rename(anchor)

    @unittest.skipIf(os.name == "nt", "POSIX permits replacement of an open carrier")
    def test_namespace_replacement_during_lane_acquisition_is_fatal(self):
        lock_type = capability_cache._SharedCacheFileLock
        namespace = self.cache.root / ".during-acquisition-root.lock"
        lane = self.cache.root / ".during-acquisition-lane.lock"
        displaced = namespace.with_name(f"{namespace.name}.displaced")
        real_open_pair = capability_cache._open_lock_carrier_pair

        def replace_namespace_while_lane_opens(path):
            pair = real_open_pair(path)
            if Path(path) == lane:
                namespace.rename(displaced)
                namespace.write_bytes(b"1")
            return pair

        try:
            with mock.patch(
                "gpu_capability_cache._open_lock_carrier_pair",
                side_effect=replace_namespace_while_lane_opens,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "lock namespace"
            ):
                with lock_type(
                    lane,
                    self.cache.root,
                    self.cache._root_identity,
                    time.monotonic() + 5.0,
                    namespace_path=namespace,
                ):
                    self.fail("replaced namespace carrier entered the lane")
        finally:
            namespace.unlink(missing_ok=True)
            if displaced.exists():
                displaced.rename(namespace)

    @unittest.skipUnless(os.name == "nt", "Windows first-use publication race")
    def test_concurrent_first_use_never_exposes_uninitialized_carrier(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".first-use-root.lock"
        first_create_opened = threading.Event()
        release_first_create = threading.Event()
        delay_lock = threading.Lock()
        delayed = False
        real_open = Path.open
        outcomes: list[str] = []
        failures: list[BaseException] = []

        def delay_first_create(path, *args, **kwargs):
            nonlocal delayed
            stream = real_open(path, *args, **kwargs)
            mode = args[0] if args else kwargs.get("mode", "r")
            with delay_lock:
                should_delay = mode == "x+b" and not delayed
                if should_delay:
                    delayed = True
            if should_delay:
                first_create_opened.set()
                if not release_first_create.wait(timeout=5.0):
                    stream.close()
                    raise AssertionError("first carrier creation release timed out")
            return stream

        def acquire() -> None:
            try:
                with lock_type(
                    carrier,
                    self.cache.root,
                    self.cache._root_identity,
                    time.monotonic() + 5.0,
                ):
                    outcomes.append("acquired")
            except BaseException as error:
                failures.append(error)

        with mock.patch.object(Path, "open", autospec=True, side_effect=delay_first_create):
            first = threading.Thread(target=acquire)
            second = threading.Thread(target=acquire)
            first.start()
            self.assertTrue(first_create_opened.wait(timeout=2.0))
            public_size = carrier.stat().st_size if carrier.exists() else None
            second.start()
            time.sleep(0.1)
            release_first_create.set()
            first.join(timeout=5.0)
            second.join(timeout=5.0)
        self.assertFalse(first.is_alive())
        self.assertFalse(second.is_alive())
        self.assertNotEqual(public_size, 0)
        self.assertEqual(failures, [])
        self.assertEqual(outcomes, ["acquired", "acquired"])

    def test_failed_lock_carrier_initialization_cleans_owned_temporary(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".failed-initialization.lock"
        anchor = capability_cache._lock_carrier_anchor_path(carrier)
        temporary = self.cache.root / f".tmp-lock-{'f' * 32}"
        operation = "rename" if os.name == "nt" else "link"
        real_operation = getattr(os, operation)

        def reject_anchor_publication(source, destination, *args, **kwargs):
            if Path(source) == temporary and Path(destination) == anchor:
                raise OSError("deterministic carrier publication failure")
            return real_operation(source, destination, *args, **kwargs)

        with mock.patch(
            "gpu_capability_cache.uuid.uuid4",
            return_value=mock.Mock(hex="f" * 32),
        ), mock.patch(
            f"gpu_capability_cache.os.{operation}",
            side_effect=reject_anchor_publication,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "lock namespace"
        ):
            with lock_type(
                carrier,
                self.cache.root,
                self.cache._root_identity,
                time.monotonic() + 5.0,
            ):
                self.fail("failed carrier initialization entered the lock")
        self.assertFalse(temporary.exists())
        self.assertFalse(anchor.exists())
        self.assertFalse(carrier.exists())

    def _assert_failed_initialization_io_cleans_temporary(
        self, operation: str
    ) -> None:
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / f".failed-{operation}.lock"
        anchor = capability_cache._lock_carrier_anchor_path(carrier)
        temporary = self.cache.root / f".tmp-lock-{'a' * 32}"
        real_open = Path.open
        real_fsync = os.fsync

        class FailingStream:
            def __init__(self, stream) -> None:
                self.stream = stream

            def __enter__(self):
                self.stream.__enter__()
                return self

            def __exit__(self, *args):
                return self.stream.__exit__(*args)

            def fileno(self):
                return self.stream.fileno()

            def write(self, value):
                if operation == "write":
                    raise OSError("deterministic carrier write failure")
                return self.stream.write(value)

            def flush(self):
                if operation == "flush":
                    raise OSError("deterministic carrier flush failure")
                return self.stream.flush()

        def open_with_failure(path, *args, **kwargs):
            stream = real_open(path, *args, **kwargs)
            mode = args[0] if args else kwargs.get("mode", "r")
            if Path(path) == temporary and mode == "x+b":
                return FailingStream(stream)
            return stream

        def fsync_with_failure(fd):
            if operation == "fsync":
                raise OSError("deterministic carrier fsync failure")
            return real_fsync(fd)

        with mock.patch(
            "gpu_capability_cache.uuid.uuid4",
            return_value=mock.Mock(hex="a" * 32),
        ), mock.patch.object(
            Path, "open", autospec=True, side_effect=open_with_failure
        ), mock.patch(
            "gpu_capability_cache.os.fsync", side_effect=fsync_with_failure
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "lock namespace"
        ):
            with lock_type(
                carrier,
                self.cache.root,
                self.cache._root_identity,
                time.monotonic() + 5.0,
            ):
                self.fail("failed carrier initialization entered the lock")
        self.assertFalse(temporary.exists())
        self.assertFalse(anchor.exists())
        self.assertFalse(carrier.exists())

    def test_lock_carrier_write_failure_cleans_owned_temporary(self):
        self._assert_failed_initialization_io_cleans_temporary("write")

    def test_lock_carrier_flush_failure_cleans_owned_temporary(self):
        self._assert_failed_initialization_io_cleans_temporary("flush")

    def test_lock_carrier_fsync_failure_cleans_owned_temporary(self):
        self._assert_failed_initialization_io_cleans_temporary("fsync")

    def test_lock_carrier_first_lstat_failure_cleans_owned_temporary(self):
        anchor = self.cache.root / ".failed-first-lstat.lock.anchor"
        temporary = self.cache.root / f".tmp-lock-{'b' * 32}"
        quarantine = self.cache.root / f".quarantine-lock-{'c' * 32}"
        real_lstat = Path.lstat
        failed = False

        def fail_first_temporary_lstat(path, *args, **kwargs):
            nonlocal failed
            if Path(path) == temporary and not failed:
                failed = True
                raise OSError("deterministic first carrier lstat failure")
            return real_lstat(path, *args, **kwargs)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                side_effect=(
                    mock.Mock(hex="b" * 32),
                    mock.Mock(hex="c" * 32),
                ),
            ), mock.patch.object(
                Path,
                "lstat",
                autospec=True,
                side_effect=fail_first_temporary_lstat,
            ), self.assertRaisesRegex(
                OSError, "deterministic first carrier lstat failure"
            ):
                capability_cache._initialize_lock_carrier_anchor(anchor)
            self.assertTrue(failed)
            self.assertFalse(temporary.exists())
            self.assertFalse(quarantine.exists())
            self.assertFalse(anchor.exists())
        finally:
            temporary.unlink(missing_ok=True)
            quarantine.unlink(missing_ok=True)
            anchor.unlink(missing_ok=True)

    def test_failed_lock_carrier_cleanup_preserves_replacement(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".replaced-initialization.lock"
        anchor = capability_cache._lock_carrier_anchor_path(carrier)
        temporary = self.cache.root / f".tmp-lock-{'e' * 32}"
        displaced = self.cache.root / ".owned-lock-carrier-temporary"
        quarantine = self.cache.root / f".quarantine-lock-{'e' * 32}"
        replacement = b"replacement lock carrier temporary"
        operation = "rename" if os.name == "nt" else "link"
        real_operation = getattr(os, operation)
        real_rename = os.rename

        def reject_anchor_publication(source, destination, *args, **kwargs):
            if Path(source) == temporary and Path(destination) == anchor:
                raise OSError("deterministic carrier publication failure")
            return real_operation(source, destination, *args, **kwargs)

        def replace_before_cleanup(path):
            self.assertEqual(Path(path), temporary)
            real_rename(temporary, displaced)
            temporary.write_bytes(replacement)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="e" * 32),
            ), mock.patch(
                f"gpu_capability_cache.os.{operation}",
                side_effect=reject_anchor_publication,
            ), mock.patch(
                "gpu_capability_cache._before_lock_carrier_temporary_quarantine",
                side_effect=replace_before_cleanup,
            ) as cleanup, self.assertRaisesRegex(
                AuditInfrastructureError,
                "lock carrier temporary was replaced; replacement preserved",
            ):
                with lock_type(
                    carrier,
                    self.cache.root,
                    self.cache._root_identity,
                    time.monotonic() + 5.0,
                ):
                    self.fail("replaced carrier initialization entered the lock")
            cleanup.assert_called_once_with(temporary)
            self.assertFalse(temporary.exists())
            self.assertEqual(quarantine.read_bytes(), replacement)
            self.assertFalse(anchor.exists())
            self.assertFalse(carrier.exists())
        finally:
            temporary.unlink(missing_ok=True)
            displaced.unlink(missing_ok=True)
            quarantine.unlink(missing_ok=True)

    def test_lock_carrier_cleanup_quarantines_before_identity_unlink(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".quarantined-initialization.lock"
        anchor = capability_cache._lock_carrier_anchor_path(carrier)
        temporary = self.cache.root / f".tmp-lock-{'9' * 32}"
        replacement = b"replacement after owned temporary quarantine"
        operation = "rename" if os.name == "nt" else "link"
        real_operation = getattr(os, operation)

        def reject_anchor_publication(source, destination, *args, **kwargs):
            if Path(source) == temporary and Path(destination) == anchor:
                raise OSError("deterministic carrier publication failure")
            return real_operation(source, destination, *args, **kwargs)

        def replace_after_quarantine(source, quarantine):
            self.assertEqual(Path(source), temporary)
            self.assertTrue(Path(quarantine).exists())
            temporary.write_bytes(replacement)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                side_effect=(
                    mock.Mock(hex="9" * 32),
                    mock.Mock(hex="8" * 32),
                ),
            ), mock.patch(
                f"gpu_capability_cache.os.{operation}",
                side_effect=reject_anchor_publication,
            ), mock.patch(
                "gpu_capability_cache._after_lock_carrier_temporary_quarantine",
                side_effect=replace_after_quarantine,
                create=True,
            ) as quarantined, self.assertRaisesRegex(
                AuditInfrastructureError, "lock namespace"
            ):
                with lock_type(
                    carrier,
                    self.cache.root,
                    self.cache._root_identity,
                    time.monotonic() + 5.0,
                ):
                    self.fail("failed carrier initialization entered the lock")
            quarantined.assert_called_once()
            self.assertEqual(temporary.read_bytes(), replacement)
            self.assertEqual(list(self.cache.root.glob(".quarantine-lock-*")), [])
            self.assertFalse(anchor.exists())
            self.assertFalse(carrier.exists())
        finally:
            temporary.unlink(missing_ok=True)
            for path in self.cache.root.glob(".quarantine-lock-*"):
                path.unlink(missing_ok=True)

    def test_lock_carrier_cleanup_preserves_quarantine_name_collision(self):
        temporary = self.cache.root / ".tmp-lock-collision-source"
        quarantine = self.cache.root / f".quarantine-lock-{'7' * 32}"
        temporary_bytes = b"owned lock carrier temporary"
        collision_bytes = b"preexisting quarantine collision"
        temporary.write_bytes(temporary_bytes)
        expected_identity = capability_cache._file_ownership_identity(
            temporary.stat()
        )
        quarantine.write_bytes(collision_bytes)
        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="7" * 32),
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "cannot quarantine"
            ):
                capability_cache._cleanup_owned_lock_carrier_temporary(
                    temporary,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertEqual(temporary.read_bytes(), temporary_bytes)
            self.assertEqual(quarantine.read_bytes(), collision_bytes)
        finally:
            temporary.unlink(missing_ok=True)
            quarantine.unlink(missing_ok=True)

    def test_lock_carrier_cleanup_reports_quarantine_replacement(self):
        temporary = self.cache.root / ".tmp-lock-quarantine-replacement"
        quarantine = self.cache.root / f".quarantine-lock-{'6' * 32}"
        displaced = self.cache.root / ".displaced-owned-quarantine"
        replacement = b"quarantine replacement"
        temporary.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            temporary.stat()
        )

        def replace_quarantine(_source, path):
            Path(path).rename(displaced)
            Path(path).write_bytes(replacement)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="6" * 32),
            ), mock.patch(
                "gpu_capability_cache._after_lock_carrier_temporary_quarantine",
                side_effect=replace_quarantine,
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "lock carrier temporary was replaced; replacement preserved",
            ):
                capability_cache._cleanup_owned_lock_carrier_temporary(
                    temporary,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertFalse(temporary.exists())
            self.assertEqual(quarantine.read_bytes(), replacement)
            self.assertEqual(displaced.read_bytes(), b"owned lock carrier temporary")
        finally:
            temporary.unlink(missing_ok=True)
            quarantine.unlink(missing_ok=True)
            displaced.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_does_not_leak_adopted_crt_descriptor(self):
        import msvcrt

        quarantine = self.cache.root / ".quarantine-lock-crt-descriptor-leak"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        real_open_osfhandle = msvcrt.open_osfhandle
        real_close = os.close
        adopted_descriptors: list[int] = []
        cleanup_error: BaseException | None = None

        def record_adopted_descriptor(handle, flags):
            descriptor = real_open_osfhandle(handle, flags)
            adopted_descriptors.append(descriptor)
            return descriptor

        try:
            with mock.patch(
                "msvcrt.open_osfhandle",
                side_effect=record_adopted_descriptor,
            ), mock.patch(
                "gpu_capability_cache.os.close",
                side_effect=OSError("deterministic CRT descriptor close failure"),
            ):
                try:
                    capability_cache._delete_verified_windows_lock_carrier_temporary(
                        quarantine,
                        expected_identity,
                        OSError("deterministic initialization failure"),
                    )
                except BaseException as error:
                    cleanup_error = error

            leaked_descriptors = []
            for descriptor in adopted_descriptors:
                try:
                    os.fstat(descriptor)
                except OSError:
                    continue
                leaked_descriptors.append(descriptor)
            self.assertEqual(
                leaked_descriptors,
                [],
                "cleanup leaked an adopted CRT descriptor and its native handle",
            )
            self.assertEqual(adopted_descriptors, [])
            self.assertIsNone(cleanup_error)
            self.assertFalse(quarantine.exists())
        finally:
            for descriptor in adopted_descriptors:
                try:
                    real_close(descriptor)
                except OSError:
                    pass
            quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_reports_windows_create_failure(self):
        import ctypes

        quarantine = self.cache.root / ".quarantine-lock-create-failure"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        invalid_handle = ctypes.c_void_p(-1).value

        def fail_create(*_args):
            ctypes.set_last_error(5)
            return invalid_handle

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32, "CreateFileW", side_effect=fail_create
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cache lock carrier quarantine changed",
            ) as raised:
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertIsInstance(raised.exception.__cause__, OSError)
            self.assertIn(
                "cannot open cache lock carrier quarantine",
                str(raised.exception.__cause__),
            )
            self.assertTrue(quarantine.exists())
        finally:
            quarantine.unlink(missing_ok=True)

    @staticmethod
    def _assert_windows_native_handle_is_invalid(handle) -> None:
        import ctypes
        from ctypes import wintypes

        class FileInformation(ctypes.Structure):
            _fields_ = (
                ("attributes", wintypes.DWORD),
                ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME),
                ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD),
                ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD),
                ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD),
                ("index_low", wintypes.DWORD),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetFileInformationByHandle.argtypes = (
            wintypes.HANDLE,
            ctypes.POINTER(FileInformation),
        )
        kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
        ctypes.set_last_error(0)
        information = FileInformation()
        result = kernel32.GetFileInformationByHandle(
            handle, ctypes.byref(information)
        )
        error_number = ctypes.get_last_error()
        if result:
            kernel32.CloseHandle(handle)
        if result or error_number != 6:
            raise AssertionError(
                "closed native handle remained queryable "
                f"(result={result!r}, error={error_number})"
            )

    def _exercise_windows_file_id_mapping(
        self,
        *,
        name: str,
        version: tuple[int, int, int],
        file_id_query_succeeds: bool,
        file_id_value: int,
        expected_device: int,
        expected_inode: int,
    ) -> None:
        import ctypes
        from ctypes import wintypes

        class FileInformation(ctypes.Structure):
            _fields_ = (
                ("attributes", wintypes.DWORD),
                ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME),
                ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD),
                ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD),
                ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD),
                ("index_low", wintypes.DWORD),
            )

        class FileId128(ctypes.Structure):
            _fields_ = (("identifier", ctypes.c_ubyte * 16),)

        class FileIdInformation(ctypes.Structure):
            _fields_ = (
                ("volume", ctypes.c_ulonglong),
                ("file_id", FileId128),
            )

        class Metadata:
            st_mode = stat.S_IFREG | stat.S_IREAD | stat.S_IWRITE
            st_nlink = 1
            st_dev = expected_device
            st_ino = expected_inode
            st_file_attributes = 0

        legacy_device = 0x12345678
        legacy_inode = 0x123456789ABCDEF0
        file_id_device = 0x8877665544332211
        quarantine = self.cache.root / f".quarantine-lock-file-id-{name}"
        quarantine.write_bytes(b"owned lock carrier temporary")
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        file_id_queries = 0

        def base_query(_handle, pointer):
            information = ctypes.cast(
                pointer, ctypes.POINTER(FileInformation)
            ).contents
            information.attributes = 0
            information.volume = legacy_device
            information.links = 1
            information.index_high = legacy_inode >> 32
            information.index_low = legacy_inode & 0xFFFFFFFF
            return True

        def file_id_query(_handle, information_class, pointer, _size):
            nonlocal file_id_queries
            file_id_queries += 1
            self.assertEqual(information_class, 18)
            if not file_id_query_succeeds:
                ctypes.set_last_error(50)
                return False
            information = ctypes.cast(
                pointer, ctypes.POINTER(FileIdInformation)
            ).contents
            information.volume = file_id_device
            encoded = file_id_value.to_bytes(16, "little")
            for index, value in enumerate(encoded):
                information.file_id.identifier[index] = value
            return True

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32,
                "GetFileInformationByHandle",
                side_effect=base_query,
            ), mock.patch.object(
                kernel32,
                "GetFileInformationByHandleEx",
                side_effect=file_id_query,
            ), mock.patch.object(
                Path, "lstat", autospec=True, return_value=Metadata()
            ), mock.patch(
                "gpu_capability_cache.sys.version_info", version
            ):
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    (expected_device, expected_inode),
                    OSError("deterministic initialization failure"),
                )
            self.assertEqual(file_id_queries, 0 if version < (3, 12) else 1)
            self.assertFalse(quarantine.exists())
        finally:
            quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows FileIdInfo identity mapping")
    def test_lock_carrier_cleanup_matches_cpython_file_id_mapping(self):
        legacy_device = 0x12345678
        legacy_inode = 0x123456789ABCDEF0
        file_id_device = 0x8877665544332211
        file_id_value = 0x102030405060708090A0B0C0D0E0F001
        cases = (
            (
                "legacy-311",
                (3, 11, 9),
                False,
                0,
                legacy_device,
                legacy_inode,
            ),
            (
                "zero-312",
                (3, 12, 0),
                True,
                0,
                file_id_device,
                legacy_inode,
            ),
            (
                "little-endian-312",
                (3, 12, 0),
                True,
                file_id_value,
                file_id_device,
                file_id_value,
            ),
            (
                "fallback-3121",
                (3, 12, 1),
                False,
                0,
                legacy_device,
                legacy_inode,
            ),
            (
                "fallback-313",
                (3, 13, 0),
                False,
                0,
                legacy_device,
                legacy_inode,
            ),
        )
        for case in cases:
            with self.subTest(case=case[0]):
                self._exercise_windows_file_id_mapping(
                    name=case[0],
                    version=case[1],
                    file_id_query_succeeds=case[2],
                    file_id_value=case[3],
                    expected_device=case[4],
                    expected_inode=case[5],
                )

    @unittest.skipUnless(os.name == "nt", "Windows FileIdInfo identity mapping")
    def test_lock_carrier_cleanup_surfaces_cpython_3120_file_id_query_failure(self):
        import ctypes

        quarantine = self.cache.root / ".quarantine-lock-file-id-query-3120"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

        def fail_query(*_args):
            ctypes.set_last_error(50)
            return False

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32,
                "GetFileInformationByHandleEx",
                side_effect=fail_query,
            ), mock.patch(
                "gpu_capability_cache.sys.version_info", (3, 12, 0)
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cannot inspect cache lock carrier quarantine file ID",
            ) as raised:
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertIsInstance(raised.exception.__cause__, OSError)
            self.assertTrue(quarantine.exists())
        finally:
            quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_closes_each_post_create_failure_exactly_once(self):
        import ctypes
        from ctypes import wintypes

        class FileInformation(ctypes.Structure):
            _fields_ = (
                ("attributes", wintypes.DWORD),
                ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME),
                ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD),
                ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD),
                ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD),
                ("index_low", wintypes.DWORD),
            )

        cases = (
            ("query", "cannot inspect cache lock carrier quarantine handle"),
            ("file-id-query", "cannot inspect cache lock carrier quarantine file ID"),
            ("lstat", "cache lock carrier quarantine changed"),
            ("validation", "lock carrier temporary was replaced"),
            ("seam", "deterministic delete seam failure"),
            ("disposition", "cannot remove cache lock carrier temporary"),
        )
        for point, expected_message in cases:
            with self.subTest(point=point):
                quarantine = self.cache.root / f".quarantine-lock-close-{point}"
                quarantine.write_bytes(b"owned lock carrier temporary")
                expected_identity = capability_cache._file_ownership_identity(
                    quarantine.stat()
                )
                kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
                real_close = kernel32.CloseHandle
                real_lstat = Path.lstat
                real_query = kernel32.GetFileInformationByHandle
                closed_handles = []

                def close_real_handle(handle):
                    closed_handles.append(handle)
                    return real_close(handle)

                def fail_native_call(*_args):
                    ctypes.set_last_error(6)
                    return False

                def fail_quarantine_lstat(path, *args, **kwargs):
                    if Path(path) == quarantine:
                        raise OSError("deterministic quarantine lstat failure")
                    return real_lstat(path, *args, **kwargs)

                def invalidate_link_count(handle, pointer):
                    result = real_query(handle, pointer)
                    information = ctypes.cast(
                        pointer, ctypes.POINTER(FileInformation)
                    ).contents
                    information.links = 2
                    return result

                try:
                    with contextlib.ExitStack() as stack:
                        stack.enter_context(
                            mock.patch("ctypes.WinDLL", return_value=kernel32)
                        )
                        close_handle = stack.enter_context(
                            mock.patch.object(
                                kernel32,
                                "CloseHandle",
                                side_effect=close_real_handle,
                            )
                        )
                        if point == "query":
                            stack.enter_context(
                                mock.patch.object(
                                    kernel32,
                                    "GetFileInformationByHandle",
                                    side_effect=fail_native_call,
                                )
                            )
                        elif point == "file-id-query":
                            stack.enter_context(
                                mock.patch.object(
                                    kernel32,
                                    "GetFileInformationByHandleEx",
                                    side_effect=fail_native_call,
                                )
                            )
                            stack.enter_context(
                                mock.patch(
                                    "gpu_capability_cache.sys.version_info",
                                    (3, 12, 0),
                                )
                            )
                        elif point == "lstat":
                            stack.enter_context(
                                mock.patch.object(
                                    Path,
                                    "lstat",
                                    autospec=True,
                                    side_effect=fail_quarantine_lstat,
                                )
                            )
                        elif point == "validation":
                            stack.enter_context(
                                mock.patch.object(
                                    kernel32,
                                    "GetFileInformationByHandle",
                                    side_effect=invalidate_link_count,
                                )
                            )
                        elif point == "seam":
                            stack.enter_context(
                                mock.patch(
                                    "gpu_capability_cache._before_windows_lock_carrier_temporary_delete",
                                    side_effect=RuntimeError(
                                        "deterministic delete seam failure"
                                    ),
                                )
                            )
                        else:
                            stack.enter_context(
                                mock.patch.object(
                                    kernel32,
                                    "SetFileInformationByHandle",
                                    side_effect=fail_native_call,
                                )
                            )
                        with self.assertRaisesRegex(
                            BaseException, expected_message
                        ):
                            capability_cache._delete_verified_windows_lock_carrier_temporary(
                                quarantine,
                                expected_identity,
                                OSError("deterministic initialization failure"),
                            )
                    close_handle.assert_called_once()
                    self.assertEqual(len(closed_handles), 1)
                    self._assert_windows_native_handle_is_invalid(
                        closed_handles[0]
                    )
                finally:
                    quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_preserves_primary_when_close_also_fails(self):
        import ctypes

        cases = (
            ("query", "cannot inspect cache lock carrier quarantine handle"),
            ("disposition", "cannot remove cache lock carrier temporary"),
        )
        for point, primary_message in cases:
            with self.subTest(point=point):
                quarantine = self.cache.root / f".quarantine-lock-combined-{point}"
                quarantine.write_bytes(b"owned lock carrier temporary")
                expected_identity = capability_cache._file_ownership_identity(
                    quarantine.stat()
                )
                kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
                real_close = kernel32.CloseHandle
                closed_handles = []

                def fail_primary(*_args):
                    ctypes.set_last_error(5)
                    return False

                def close_then_report_failure(handle):
                    closed_handles.append(handle)
                    self.assertTrue(real_close(handle))
                    ctypes.set_last_error(6)
                    return False

                target = (
                    "GetFileInformationByHandle"
                    if point == "query"
                    else "SetFileInformationByHandle"
                )
                try:
                    with mock.patch(
                        "ctypes.WinDLL", return_value=kernel32
                    ), mock.patch.object(
                        kernel32, target, side_effect=fail_primary
                    ), mock.patch.object(
                        kernel32,
                        "CloseHandle",
                        side_effect=close_then_report_failure,
                    ) as close_handle, self.assertRaisesRegex(
                        AuditInfrastructureError, primary_message
                    ) as raised:
                        capability_cache._delete_verified_windows_lock_carrier_temporary(
                            quarantine,
                            expected_identity,
                            OSError("deterministic initialization failure"),
                        )
                    close_handle.assert_called_once()
                    self.assertEqual(len(closed_handles), 1)
                    self._assert_windows_native_handle_is_invalid(
                        closed_handles[0]
                    )
                    self.assertIs(
                        type(raised.exception), AuditInfrastructureError
                    )
                    secondary = raised.exception.secondary_close_error
                    self.assertIsInstance(secondary, AuditInfrastructureError)
                    self.assertIsInstance(secondary.__cause__, OSError)
                    self.assertIn(
                        "cannot close cache lock carrier quarantine handle",
                        str(secondary),
                    )
                    rendered = "".join(
                        traceback.format_exception(raised.exception)
                    )
                    self.assertIn("Secondary CloseHandle failure", rendered)
                    self.assertIn(primary_message, rendered)
                finally:
                    quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_never_dispatches_against_custom_primaries(self):
        import ctypes

        def mutate_primary(primary):
            BaseException.__dict__["args"].__set__(
                primary, ("corrupted primary message",)
            )
            BaseException.__dict__["__cause__"].__set__(
                primary, RuntimeError("corrupted cause")
            )
            BaseException.__dict__["__context__"].__set__(
                primary, RuntimeError("corrupted context")
            )
            vars(primary)["__notes__"] = ["corrupted note"]
            BaseException.__dict__["__traceback__"].__set__(primary, None)

        def hostile_primary_type(base_type):
            class HostilePrimary(base_type):
                attachment_dispatches = 0

                @property
                def secondary_close_error(self):
                    type(self).attachment_dispatches += 1
                    mutate_primary(self)
                    raise RuntimeError("secondary diagnostic read dispatched")

                @secondary_close_error.setter
                def secondary_close_error(self, _value):
                    type(self).attachment_dispatches += 1
                    mutate_primary(self)
                    raise RuntimeError("secondary diagnostic write dispatched")

                @property
                def __notes__(self):
                    type(self).attachment_dispatches += 1
                    mutate_primary(self)
                    raise RuntimeError("notes diagnostic read dispatched")

                @__notes__.setter
                def __notes__(self, _value):
                    type(self).attachment_dispatches += 1
                    mutate_primary(self)
                    raise RuntimeError("notes diagnostic write dispatched")

            return HostilePrimary

        for base_type in (RuntimeError, AuditInfrastructureError):
            with self.subTest(base_type=base_type.__name__):
                quarantine = self.cache.root / (
                    ".quarantine-lock-hostile-" + base_type.__name__
                )
                quarantine.write_bytes(b"owned lock carrier temporary")
                expected_identity = capability_cache._file_ownership_identity(
                    quarantine.stat()
                )
                kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
                real_close = kernel32.CloseHandle
                primary_type = hostile_primary_type(base_type)
                primary = primary_type("authoritative primary message", 17)
                original_args = primary.args
                original_message = str(primary)
                original_cause = ValueError("authoritative cause")
                original_context = KeyError("authoritative context")
                BaseException.__dict__["__cause__"].__set__(
                    primary, original_cause
                )
                BaseException.__dict__["__context__"].__set__(
                    primary, original_context
                )
                original_notes = ["authoritative note"]
                vars(primary)["__notes__"] = original_notes
                captured_traceback = None

                def raise_primary(_path):
                    nonlocal captured_traceback
                    try:
                        raise primary
                    except BaseException:
                        captured_traceback = sys.exc_info()[2]
                        raise

                def close_then_report_failure(handle):
                    self.assertTrue(real_close(handle))
                    ctypes.set_last_error(6)
                    return False

                try:
                    with mock.patch(
                        "ctypes.WinDLL", return_value=kernel32
                    ), mock.patch(
                        "gpu_capability_cache._before_windows_lock_carrier_temporary_delete",
                        side_effect=raise_primary,
                    ), mock.patch.object(
                        kernel32,
                        "CloseHandle",
                        side_effect=close_then_report_failure,
                    ) as close_handle:
                        try:
                            capability_cache._delete_verified_windows_lock_carrier_temporary(
                                quarantine,
                                expected_identity,
                                primary,
                            )
                        except primary_type as raised:
                            self.assertIs(raised, primary)
                            self.assertIs(raised.args, original_args)
                            self.assertEqual(str(raised), original_message)
                            self.assertIs(raised.__cause__, original_cause)
                            self.assertIs(raised.__context__, original_context)
                            self.assertIs(
                                vars(raised)["__notes__"], original_notes
                            )
                            self.assertEqual(
                                primary_type.attachment_dispatches, 0
                            )
                            self.assertNotIn(
                                "secondary_close_error", vars(raised)
                            )
                            traceback_nodes = []
                            current = raised.__traceback__
                            while current is not None:
                                traceback_nodes.append(current)
                                current = current.tb_next
                            self.assertIsNotNone(captured_traceback)
                            self.assertIs(
                                traceback_nodes[-1], captured_traceback
                            )
                            helper_frames = [
                                node.tb_frame
                                for node in traceback_nodes
                                if node.tb_frame.f_code
                                is capability_cache._delete_verified_windows_lock_carrier_temporary.__code__
                            ]
                            self.assertEqual(len(helper_frames), 1)
                            protected_state = (
                                primary,
                                original_args,
                                original_cause,
                                original_context,
                                original_notes,
                            )
                            for name, value in helper_frames[0].f_locals.items():
                                self.assertFalse(
                                    any(value is state for state in protected_state),
                                    name,
                                )
                                self.assertFalse(
                                    any(value is node for node in traceback_nodes),
                                    name,
                                )
                        else:
                            self.fail("custom primary did not propagate")
                    close_handle.assert_called_once()
                finally:
                    quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_reports_windows_information_query_failure(self):
        import ctypes

        quarantine = self.cache.root / ".quarantine-lock-query-failure"
        renamed = self.cache.root / ".renamed-query-failure"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

        def fail_query(*_args):
            ctypes.set_last_error(6)
            return False

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32,
                "GetFileInformationByHandle",
                side_effect=fail_query,
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cannot inspect cache lock carrier quarantine handle",
            ) as raised:
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertIsInstance(raised.exception.__cause__, OSError)
            quarantine.rename(renamed)
            renamed.rename(quarantine)
        finally:
            quarantine.unlink(missing_ok=True)
            renamed.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_reports_windows_path_validation_failure(self):
        quarantine = self.cache.root / ".quarantine-lock-path-query-failure"
        renamed = self.cache.root / ".renamed-path-query-failure"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        real_lstat = Path.lstat

        def fail_quarantine_lstat(path, *args, **kwargs):
            if Path(path) == quarantine:
                raise OSError("deterministic quarantine path validation failure")
            return real_lstat(path, *args, **kwargs)

        try:
            with mock.patch.object(
                Path,
                "lstat",
                autospec=True,
                side_effect=fail_quarantine_lstat,
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cache lock carrier quarantine changed",
            ) as raised:
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertIsInstance(raised.exception.__cause__, OSError)
            self.assertIn(
                "deterministic quarantine path validation failure",
                str(raised.exception.__cause__),
            )
            quarantine.rename(renamed)
            renamed.rename(quarantine)
        finally:
            quarantine.unlink(missing_ok=True)
            renamed.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_rejects_mutated_windows_handle_metadata(self):
        import ctypes
        from ctypes import wintypes

        class FileInformation(ctypes.Structure):
            _fields_ = (
                ("attributes", wintypes.DWORD),
                ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME),
                ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD),
                ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD),
                ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD),
                ("index_low", wintypes.DWORD),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        real_query = kernel32.GetFileInformationByHandle

        for field in ("links", "attributes"):
            with self.subTest(field=field):
                quarantine = self.cache.root / f".quarantine-lock-mutated-{field}"
                quarantine.write_bytes(b"owned lock carrier temporary")
                expected_identity = capability_cache._file_ownership_identity(
                    quarantine.stat()
                )

                def mutate_query(handle, pointer):
                    result = real_query(handle, pointer)
                    information = ctypes.cast(
                        pointer, ctypes.POINTER(FileInformation)
                    ).contents
                    if field == "links":
                        information.links = 2
                    else:
                        information.attributes |= 0x00000400
                    return result

                try:
                    with mock.patch(
                        "ctypes.WinDLL", return_value=kernel32
                    ), mock.patch.object(
                        kernel32,
                        "GetFileInformationByHandle",
                        side_effect=mutate_query,
                    ), self.assertRaisesRegex(
                        AuditInfrastructureError,
                        "lock carrier temporary was replaced; replacement preserved",
                    ):
                        capability_cache._delete_verified_windows_lock_carrier_temporary(
                            quarantine,
                            expected_identity,
                            OSError("deterministic initialization failure"),
                        )
                    self.assertTrue(quarantine.exists())
                finally:
                    quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(
        os.name == "nt" and sys.version_info >= (3, 12),
        "Windows FileIdInfo identity mapping",
    )
    def test_lock_carrier_cleanup_rejects_mutated_windows_file_id(self):
        import ctypes

        class FileId128(ctypes.Structure):
            _fields_ = (("identifier", ctypes.c_ubyte * 16),)

        class FileIdInformation(ctypes.Structure):
            _fields_ = (
                ("volume", ctypes.c_ulonglong),
                ("file_id", FileId128),
            )

        quarantine = self.cache.root / ".quarantine-lock-mutated-file-id"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        real_query = kernel32.GetFileInformationByHandleEx

        def mutate_file_id(handle, information_class, pointer, size):
            result = real_query(handle, information_class, pointer, size)
            if result and information_class == 18:
                identity = ctypes.cast(
                    pointer, ctypes.POINTER(FileIdInformation)
                ).contents
                identity.file_id.identifier[0] ^= 1
            return result

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32,
                "GetFileInformationByHandleEx",
                side_effect=mutate_file_id,
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "lock carrier temporary was replaced; replacement preserved",
            ):
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertTrue(quarantine.exists())
        finally:
            quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_closes_handle_when_test_seam_fails(self):
        quarantine = self.cache.root / ".quarantine-lock-seam-failure"
        renamed = self.cache.root / ".renamed-seam-failure"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        try:
            with mock.patch(
                "gpu_capability_cache._before_windows_lock_carrier_temporary_delete",
                side_effect=RuntimeError("deterministic delete seam failure"),
            ), self.assertRaisesRegex(
                RuntimeError, "deterministic delete seam failure"
            ):
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            quarantine.rename(renamed)
            renamed.rename(quarantine)
        finally:
            quarantine.unlink(missing_ok=True)
            renamed.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_reports_windows_disposition_failure(self):
        import ctypes

        quarantine = self.cache.root / ".quarantine-lock-disposition-failure"
        renamed = self.cache.root / ".renamed-disposition-failure"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

        def fail_disposition(*_args):
            ctypes.set_last_error(5)
            return False

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32,
                "SetFileInformationByHandle",
                side_effect=fail_disposition,
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cannot remove cache lock carrier temporary",
            ) as raised:
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertIsInstance(raised.exception.__cause__, OSError)
            self.assertIn(
                "cannot mark cache lock carrier quarantine for deletion",
                str(raised.exception.__cause__),
            )
            quarantine.rename(renamed)
            renamed.rename(quarantine)
        finally:
            quarantine.unlink(missing_ok=True)
            renamed.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_reports_one_windows_close_failure(self):
        import ctypes

        quarantine = self.cache.root / ".quarantine-lock-close-failure"
        quarantine.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            quarantine.stat()
        )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        real_close = kernel32.CloseHandle
        closed_handles = []

        def fail_close(handle):
            closed_handles.append(handle)
            self.assertTrue(real_close(handle))
            ctypes.set_last_error(6)
            return False

        try:
            with mock.patch(
                "ctypes.WinDLL", return_value=kernel32
            ), mock.patch.object(
                kernel32, "CloseHandle", side_effect=fail_close
            ) as close_handle, self.assertRaisesRegex(
                AuditInfrastructureError,
                "cannot close cache lock carrier quarantine handle",
            ) as raised:
                capability_cache._delete_verified_windows_lock_carrier_temporary(
                    quarantine,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            close_handle.assert_called_once()
            self.assertEqual(len(closed_handles), 1)
            self._assert_windows_native_handle_is_invalid(closed_handles[0])
            self.assertIsInstance(raised.exception.__cause__, OSError)
        finally:
            quarantine.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows verified-handle deletion")
    def test_lock_carrier_cleanup_deletes_verified_windows_handle_not_replacement(self):
        temporary = self.cache.root / ".tmp-lock-windows-handle-delete"
        quarantine = self.cache.root / f".quarantine-lock-{'5' * 32}"
        displaced = self.cache.root / ".displaced-owned-windows-quarantine"
        replacement = b"replacement after verified Windows quarantine open"
        temporary.write_bytes(b"owned lock carrier temporary")
        expected_identity = capability_cache._file_ownership_identity(
            temporary.stat()
        )
        real_unlink = Path.unlink
        raced = False

        def replace_verified_quarantine(path):
            nonlocal raced
            self.assertEqual(Path(path), quarantine)
            Path(path).rename(displaced)
            Path(path).write_bytes(replacement)
            raced = True

        def unlink_with_pre_delete_race(path, *args, **kwargs):
            if Path(path) == quarantine and not raced:
                replace_verified_quarantine(path)
            return real_unlink(path, *args, **kwargs)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="5" * 32),
            ), mock.patch(
                "gpu_capability_cache._before_windows_lock_carrier_temporary_delete",
                side_effect=replace_verified_quarantine,
                create=True,
            ) as before_delete, mock.patch.object(
                Path,
                "unlink",
                autospec=True,
                side_effect=unlink_with_pre_delete_race,
            ):
                capability_cache._cleanup_owned_lock_carrier_temporary(
                    temporary,
                    expected_identity,
                    OSError("deterministic initialization failure"),
                )
            self.assertTrue(raced)
            before_delete.assert_called_once_with(quarantine)
            self.assertEqual(quarantine.read_bytes(), replacement)
            self.assertFalse(displaced.exists())
            self.assertFalse(temporary.exists())
        finally:
            temporary.unlink(missing_ok=True)
            quarantine.unlink(missing_ok=True)
            displaced.unlink(missing_ok=True)

    def test_unsafe_lock_anchor_is_rejected_without_public_link_creation(self):
        lock_type = capability_cache._SharedCacheFileLock
        carrier = self.cache.root / ".unsafe-anchor.lock"
        anchor = capability_cache._lock_carrier_anchor_path(carrier)
        seed = self.root / "unsafe-anchor-seed"
        seed.write_bytes(b"1")
        try:
            os.link(seed, anchor)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        before_links = int(seed.stat().st_nlink)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "lock namespace"
        ):
            with lock_type(
                carrier,
                self.cache.root,
                self.cache._root_identity,
                time.monotonic() + 5.0,
            ):
                self.fail("unsafe carrier anchor entered the lock")
        self.assertFalse(carrier.exists())
        self.assertEqual(int(seed.stat().st_nlink), before_links)

    def test_inspection_publication_rejects_unsafe_shared_lock_carrier(self):
        inspection = self.inspection()
        seed = self.root / "inspection-lock-seed"
        seed.write_bytes(b"1")
        carrier = self.cache.root / ".compiler-inspection-root.lock"
        try:
            os.link(seed, carrier)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "lock namespace"
        ):
            self.cache.publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, inspection, time.monotonic() + 10.0,
            )
        self.assertTrue(os.path.samefile(seed, carrier))

    def test_different_concurrent_inspection_winner_remains_fatal(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        different = dataclasses.replace(inspection, driver_fingerprint="a" * 64)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "winner differs"
        ):
            CompilerInspectionCache(self.cache.root).publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, different, time.monotonic() + 10.0,
            )
        self.assertEqual(list(self.cache.root.glob(".tmp-inspection-*")), [])

    def test_inspection_owned_temporary_cleanup_failure_preserves_original_context(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        key = compiler_inspection_cache_key(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64,
            executable_capability_digest=self.capability_digest,
            resolved_runtime_closure_digest=self.closure_digest,
        )
        winner = self.cache._path(key)
        winner_identity = winner.stat().st_ino
        winner_bytes = winner.read_bytes()
        different = dataclasses.replace(inspection, driver_fingerprint="a" * 64)
        real_unlink = Path.unlink

        def reject_owned_temporary(path, *args, **kwargs):
            if path.name.startswith(".tmp-inspection-"):
                raise PermissionError("deterministic owned temporary cleanup failure")
            return real_unlink(path, *args, **kwargs)

        with mock.patch.object(
            Path, "unlink", autospec=True, side_effect=reject_owned_temporary
        ), self.assertRaisesRegex(
            AuditInfrastructureError,
            "cannot remove compiler inspection publication temporary.*winner differs",
        ):
            CompilerInspectionCache(self.cache.root).publish(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, different, time.monotonic() + 10.0,
            )

        temporaries = list(self.cache.root.glob(".tmp-inspection-*"))
        self.assertEqual(len(temporaries), 1)
        self.assertEqual(winner.stat().st_ino, winner_identity)
        self.assertEqual(winner.read_bytes(), winner_bytes)
        temporaries[0].unlink()

    def test_live_compiler_mutation_during_cache_decode_remains_fatal(self):
        inspection = self.inspection()
        self.cache.publish(
            self.compiler.resolve(), CompilerFamily.GCC, self.environment,
            self.authority, "e" * 64, self.capability_digest,
            self.closure_digest, inspection, time.monotonic() + 10.0,
        )
        original_decode = sys.modules["gpu_capability_cache"].decode_compiler_inspection

        def mutate_then_decode(payload, **kwargs):
            self.compiler.write_bytes(b"compiler-mutated-during-load")
            return original_decode(payload, **kwargs)

        with mock.patch(
            "gpu_capability_cache.decode_compiler_inspection",
            side_effect=mutate_then_decode,
        ), self.assertRaisesRegex(AuditInfrastructureError, "content changed"):
            self.cache.load(
                self.compiler.resolve(), CompilerFamily.GCC, self.environment,
                self.authority, "e" * 64, self.capability_digest,
                self.closure_digest, time.monotonic() + 10.0,
            )

class PreprocessCacheTests(unittest.TestCase, _PreprocessCacheFixture):
    def setUp(self) -> None:
        _PreprocessCacheFixture.setUp(self)

    def tearDown(self) -> None:
        _PreprocessCacheFixture.tearDown(self)

    def test_hit_requires_every_dependency_content_hash(self):
        self._assert_hit_requires_every_dependency_content_hash()

    def test_missing_and_regularly_replaced_dependencies_are_misses(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        self.header_path.unlink()
        self.assertIsNone(cache.load(self.configuration))
        self.header_path.write_text("#pragma once\n", encoding="utf-8")
        self.assertIsNone(cache.load(self.configuration))

        cache.publish(self.view(dependencies=(self.main, self.identity(self.header_path, "playback/a.h"))))

    def test_hardlinked_dependency_is_a_fatal_namespace_violation(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        alias = self.root / "alias.h"
        try:
            os.link(self.header_path, alias)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "namespace|alias|ordinary"
        ):
            cache.load(self.configuration)

    def test_unreadable_dependency_is_a_miss(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        with mock.patch("gpu_capability_cache._open_dependency", side_effect=PermissionError("denied")):
            self.assertIsNone(cache.load(self.configuration))

    def test_partial_corrupt_schema_and_payload_entries_are_misses(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        manifest = entry / "manifest.json"
        original = manifest.read_bytes()

        manifest.unlink()
        self.assertIsNone(cache.load(self.configuration))
        manifest.write_bytes(original)
        manifest.write_text("{", encoding="utf-8")
        self.assertIsNone(cache.load(self.configuration))
        manifest.write_bytes(original)
        document = json.loads(original)
        document["schema"] += 1
        manifest.write_text(json.dumps(document), encoding="utf-8")
        self.assertIsNone(cache.load(self.configuration))
        manifest.write_bytes(original)
        (entry / "payload.bin").write_bytes(b"corrupt")
        self.assertIsNone(cache.load(self.configuration))

    def test_payload_namespace_violation_during_decode_is_fatal(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        with mock.patch(
            "gpu_capability_cache._HeldCacheFile.verify",
            side_effect=capability_cache._UnsafeCacheNamespaceError(
                "payload was replaced"
            ),
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "namespace|replaced|changed"
        ):
            cache.load(self.configuration)

    def test_in_place_payload_mutation_with_restored_mtime_is_a_miss(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        payload = self.entry(cache) / "payload.bin"
        original_mtime = payload.stat().st_mtime_ns

        def mutate_after_hash(_path: Path) -> None:
            with payload.open("r+b") as stream:
                contents = stream.read()
                offset = contents.index(b"lease")
                stream.seek(offset)
                stream.write(b"zease")
                stream.flush()
                os.fsync(stream.fileno())
            os.utime(payload, ns=(original_mtime, original_mtime))

        with mock.patch(
            "gpu_capability_cache._before_payload_parse",
            side_effect=mutate_after_hash,
        ):
            self.assertIsNone(cache.load(self.configuration))

    def test_cache_serializes_packed_columns_without_token_views(self):
        cache = PreprocessCache(self.cache_root)
        dense_view = self.view(repeats=1000)
        with mock.patch.object(
            CompactTokenSequence,
            "__iter__",
            side_effect=AssertionError("token iteration"),
        ), mock.patch.object(
            CompactTokenSequence,
            "__getitem__",
            side_effect=AssertionError("token materialization"),
        ):
            cache.publish(dense_view)
            restored = cache.load(self.configuration)
        self.assertIsNotNone(restored)
        assert restored is not None
        self.assertEqual(restored.tokens.packed_bytes, dense_view.tokens.packed_bytes)
        self.assertEqual(restored.tokens._packed_columns()[0][2999], 2)

    def test_deserialization_checks_coordinator_rss_before_allocating(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view(repeats=1000))
        with mock.patch(
            "gpu_capability_cache._current_process_rss_bytes",
            return_value=2**63 - 1,
        ):
            self.assertIsNone(cache.load(self.configuration))

    def test_deserialization_transfers_packed_column_ownership_without_copying(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view(repeats=1000))
        with mock.patch("gpu_capability_model._copy_packed_column", wraps=array) as copier:
            restored = cache.load(self.configuration)
        self.assertIsNotNone(restored)
        self.assertEqual(copier.call_count, 0)

    def test_key_changes_for_every_configuration_semantic(self):
        cache = PreprocessCache(self.cache_root)
        base = cache._configuration_key(self.configuration)
        variants = (
            dataclasses.replace(self.configuration, family=CompilerFamily.CLANG),
            dataclasses.replace(self.configuration, compiler=self.compiler.with_name("clang.exe")),
            dataclasses.replace(self.configuration, working_directory=self.root / "other"),
            dataclasses.replace(self.configuration, source=self.header),
            dataclasses.replace(self.configuration, arguments=("-O2",)),
            dataclasses.replace(self.configuration, environment_digest="environment-two"),
            dataclasses.replace(self.configuration, digest="compiler-fingerprint-two"),
        )
        self.assertEqual(len({base, *(cache._configuration_key(value) for value in variants)}), 8)

    def test_environment_value_is_not_persisted(self):
        secret = "cache-test-secret-value"
        configuration = dataclasses.replace(self.configuration, environment_digest=secret)
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view(configuration=configuration))
        persisted = b"".join(
            path.read_bytes() for path in self.cache_root.rglob("*") if path.is_file()
        )
        self.assertNotIn(secret.encode(), persisted)

    def test_load_only_replaces_access_sidecar(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        payload = entry / "payload.bin"
        manifest = entry / "manifest.json"
        before = {
            path: (path.stat().st_mtime_ns, path.read_bytes()) for path in (payload, manifest)
        }
        time.sleep(0.01)
        self.assertIsNotNone(cache.load(self.configuration))
        after = {
            path: (path.stat().st_mtime_ns, path.read_bytes()) for path in (payload, manifest)
        }
        self.assertEqual(after, before)
        self.assertTrue(cache._access_path(self.entry(cache).name).is_file())

    def test_access_sidecar_is_root_direct_and_replaces_alias_without_touching_target(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        key = self.entry(cache).name
        access = cache._access_path(key)
        self.assertEqual(access.parent, self.cache_root)
        outside = self.root / "outside-access"
        outside.write_text("keep", encoding="utf-8")
        access.unlink()
        try:
            os.link(outside, access)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        self.assertIsNotNone(cache.load(self.configuration))
        self.assertEqual(outside.read_text(encoding="utf-8"), "keep")
        self.assertEqual(access.stat().st_nlink, 1)

    def test_publication_fsyncs_payload_and_manifest_before_atomic_rename(self):
        cache = PreprocessCache(self.cache_root)
        events: list[str] = []
        real_fsync = os.fsync
        real_rename = os.rename

        def fsync(fd: int) -> None:
            events.append("fsync")
            real_fsync(fd)

        def rename(source, destination) -> None:
            events.append("rename")
            real_rename(source, destination)

        with mock.patch("gpu_capability_cache.os.fsync", side_effect=fsync), mock.patch(
            "gpu_capability_cache.os.rename", side_effect=rename
        ):
            cache.publish(self.view())
        self.assertGreaterEqual(events.count("fsync"), 2)
        rename_index = events.index("rename")
        self.assertGreaterEqual(events[:rename_index].count("fsync"), 2)

    def test_concurrent_publication_has_one_complete_winner(self):
        cache = PreprocessCache(self.cache_root)
        barrier = threading.Barrier(4)
        failures: list[BaseException] = []

        def publish() -> None:
            try:
                barrier.wait()
                cache.publish(self.view())
            except BaseException as error:
                failures.append(error)

        threads = [threading.Thread(target=publish) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(failures, [])
        self.assertIsNotNone(cache.load(self.configuration))
        self.assertEqual(len([path for path in self.cache_root.iterdir() if path.name[0].isalnum()]), 1)

    def test_reader_cannot_escape_while_final_validation_pauses_then_fails(self):
        publisher = PreprocessCache(self.cache_root)
        reader = PreprocessCache(self.cache_root)
        view = self.view()
        snapshots = publisher._snapshot_dependencies(view.dependencies, force=True)
        validation_entered = threading.Event()
        release_validation = threading.Event()
        reader_done = threading.Event()
        publication_failures: list[BaseException] = []
        reader_results: list[PreprocessedTranslationUnitView | None] = []

        def final_validation() -> None:
            validation_entered.set()
            if not release_validation.wait(timeout=5.0):
                raise AssertionError("final validation release timed out")
            raise AuditInfrastructureError("forced final validation failure")

        def publish() -> None:
            try:
                publisher._publish_stabilized(
                    view, snapshots, final_validation=final_validation
                )
            except BaseException as error:
                publication_failures.append(error)

        def load() -> None:
            try:
                reader_results.append(reader.load(self.configuration))
            finally:
                reader_done.set()

        publisher_thread = threading.Thread(target=publish)
        publisher_thread.start()
        self.assertTrue(validation_entered.wait(timeout=5.0))
        reader_thread = threading.Thread(target=load)
        reader_thread.start()
        try:
            self.assertFalse(
                reader_done.wait(timeout=0.2),
                "reader returned while final validation was unresolved",
            )
        finally:
            release_validation.set()
            publisher_thread.join(timeout=10.0)
            reader_thread.join(timeout=10.0)
        self.assertFalse(publisher_thread.is_alive())
        self.assertFalse(reader_thread.is_alive())
        self.assertEqual(len(publication_failures), 1)
        self.assertRegex(str(publication_failures[0]), "forced final validation failure")
        self.assertEqual(reader_results, [None])

    def test_spawned_reader_waits_for_failed_and_successful_final_validation(self):
        context = multiprocessing.get_context("spawn")
        for validation_fails in (True, False):
            with self.subTest(validation_fails=validation_fails):
                publisher = PreprocessCache(self.cache_root)
                view = self.view()
                snapshots = publisher._snapshot_dependencies(
                    view.dependencies, force=True
                )
                validation_entered_path = self.root / (
                    f"spawned-validation-entered-{int(validation_fails)}"
                )
                release_validation_path = self.root / (
                    f"spawned-validation-release-{int(validation_fails)}"
                )
                result_connection, child_connection = context.Pipe(duplex=False)
                publisher_process = context.Process(
                    target=_publish_preprocess_cache_in_spawned_process,
                    args=(
                        str(self.cache_root),
                        str(self.root),
                        validation_fails,
                        str(validation_entered_path),
                        str(release_validation_path),
                        child_connection,
                    ),
                )
                publisher_process.start()
                child_connection.close()
                reader_started_path = self.root / (
                    f"spawned-reader-started-{int(validation_fails)}"
                )
                reader_connection, reader_child_connection = context.Pipe(
                    duplex=False
                )
                reader_process = context.Process(
                    target=_load_preprocess_cache_in_spawned_process,
                    args=(
                        str(self.cache_root),
                        str(self.root),
                        str(reader_started_path),
                        reader_child_connection,
                    ),
                )
                try:
                    started_deadline = time.monotonic() + 10.0
                    while (
                        not validation_entered_path.exists()
                        and time.monotonic() < started_deadline
                    ):
                        time.sleep(0.01)
                    self.assertTrue(validation_entered_path.exists())
                    reader_process.start()
                    reader_child_connection.close()
                    reader_started_deadline = time.monotonic() + 10.0
                    while (
                        not reader_started_path.exists()
                        and time.monotonic() < reader_started_deadline
                    ):
                        time.sleep(0.01)
                    self.assertTrue(reader_started_path.exists())
                    self.assertFalse(
                        reader_connection.poll(0.2),
                        "spawned reader returned while final validation was unresolved",
                    )
                finally:
                    release_validation_path.write_bytes(b"1")
                    publisher_process.join(timeout=10.0)
                    if reader_process.pid is not None:
                        reader_process.join(timeout=10.0)
                    if publisher_process.is_alive():
                        publisher_process.terminate()
                        publisher_process.join(timeout=5.0)
                    if reader_process.is_alive():
                        reader_process.terminate()
                        reader_process.join(timeout=5.0)
                self.assertEqual(publisher_process.exitcode, 0)
                self.assertEqual(reader_process.exitcode, 0)
                try:
                    self.assertTrue(result_connection.poll(2.0))
                    publisher_result = result_connection.recv()
                    self.assertTrue(reader_connection.poll(2.0))
                    reader_result = reader_connection.recv()
                finally:
                    result_connection.close()
                    reader_connection.close()
                    validation_entered_path.unlink(missing_ok=True)
                    release_validation_path.unlink(missing_ok=True)
                    reader_started_path.unlink(missing_ok=True)
                    publisher_process.close()
                    reader_process.close()
                if validation_fails:
                    self.assertEqual(publisher_result[0:2], ("error", "AuditInfrastructureError"))
                else:
                    self.assertEqual(publisher_result, ("ok", True))
                self.assertEqual(reader_result, ("ok", not validation_fails))

    def test_two_cache_instances_serialize_public_entry_observation(self):
        first = PreprocessCache(self.cache_root)
        second = PreprocessCache(self.cache_root)
        key = first._configuration_key(self.configuration)
        entry = self.cache_root / key
        rename_entered = threading.Event()
        release_rename = threading.Event()
        rename_count = 0
        rename_count_lock = threading.Lock()
        real_rename = os.rename
        failures: list[BaseException] = []

        def rename(source, destination) -> None:
            nonlocal rename_count
            if Path(source).name.startswith(f".tmp-{key}-") and Path(destination) == entry:
                with rename_count_lock:
                    rename_count += 1
                    current_count = rename_count
                if current_count == 1:
                    rename_entered.set()
                    if not release_rename.wait(timeout=5.0):
                        raise AssertionError("rename release timed out")
            real_rename(source, destination)

        def publish(cache: PreprocessCache) -> None:
            try:
                cache.publish(self.view())
            except BaseException as error:
                failures.append(error)

        with mock.patch("gpu_capability_cache.os.rename", side_effect=rename):
            first_thread = threading.Thread(target=publish, args=(first,))
            second_thread = threading.Thread(target=publish, args=(second,))
            first_thread.start()
            self.assertTrue(rename_entered.wait(timeout=5.0))
            second_thread.start()
            try:
                time.sleep(0.2)
                self.assertEqual(rename_count, 1)
            finally:
                release_rename.set()
                first_thread.join(timeout=10.0)
                second_thread.join(timeout=10.0)
            threads = (first_thread, second_thread)
        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(failures, [])
        self.assertEqual(rename_count, 1)
        self.assertIsNotNone(first.load(self.configuration))
        self.assertEqual(list(self.cache_root.glob(f".tmp-{key}-*")), [])

    def test_blocked_preprocess_reader_honors_deadline_and_cancellation(self):
        publisher = PreprocessCache(self.cache_root)
        reader = PreprocessCache(self.cache_root)
        view = self.view()
        snapshots = publisher._snapshot_dependencies(view.dependencies, force=True)
        validation_entered = threading.Event()
        release_validation = threading.Event()
        publication_failures: list[BaseException] = []

        def final_validation() -> None:
            validation_entered.set()
            if not release_validation.wait(timeout=10.0):
                raise AssertionError("final validation release timed out")

        def publish() -> None:
            try:
                publisher._publish_stabilized(
                    view,
                    snapshots,
                    final_validation=final_validation,
                    deadline=time.monotonic() + 15.0,
                )
            except BaseException as error:
                publication_failures.append(error)

        publisher_thread = threading.Thread(target=publish)
        publisher_thread.start()
        self.assertTrue(validation_entered.wait(timeout=5.0))
        try:
            with self.assertRaisesRegex(AuditInfrastructureError, "lock deadline"):
                reader.load(self.configuration, time.monotonic() + 0.05)
            cancelled = threading.Event()
            cancelled.set()
            with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
                reader.load(
                    self.configuration,
                    time.monotonic() + 5.0,
                    cancelled,
                )
        finally:
            release_validation.set()
            publisher_thread.join(timeout=10.0)
        self.assertFalse(publisher_thread.is_alive())
        self.assertEqual(publication_failures, [])

    def test_different_key_publication_lock_honors_deadline_and_cancellation(self):
        for mode in ("deadline", "cancel"):
            with self.subTest(mode=mode):
                cache = PreprocessCache(self.root / f"publication-lock-{mode}")
                blocker_view = self.view()
                contender_configuration = dataclasses.replace(
                    self.configuration, digest=f"contender-{mode}"
                )
                contender_view = self.view(configuration=contender_configuration)
                blocker_snapshots = cache._snapshot_dependencies(
                    blocker_view.dependencies, force=True
                )
                contender_snapshots = cache._snapshot_dependencies(
                    contender_view.dependencies, force=True
                )
                validation_entered = threading.Event()
                release_validation = threading.Event()
                contender_done = threading.Event()
                blocker_failures: list[BaseException] = []
                contender_failures: list[BaseException] = []
                cancelled = threading.Event()

                def final_validation() -> None:
                    validation_entered.set()
                    if not release_validation.wait(timeout=5.0):
                        raise AssertionError("publication lock release timed out")

                def block() -> None:
                    try:
                        cache._publish_stabilized(
                            blocker_view,
                            blocker_snapshots,
                            final_validation=final_validation,
                            deadline=time.monotonic() + 5.0,
                        )
                    except BaseException as error:
                        blocker_failures.append(error)

                def contend() -> None:
                    try:
                        cache._publish_stabilized(
                            contender_view,
                            contender_snapshots,
                            deadline=(
                                time.monotonic() + 0.05
                                if mode == "deadline"
                                else time.monotonic() + 5.0
                            ),
                            cancel_event=cancelled,
                        )
                    except BaseException as error:
                        contender_failures.append(error)
                    finally:
                        contender_done.set()

                blocker_thread = threading.Thread(target=block)
                contender_thread = threading.Thread(target=contend)
                blocker_thread.start()
                self.assertTrue(validation_entered.wait(timeout=5.0))
                contender_thread.start()
                if mode == "cancel":
                    time.sleep(0.05)
                    cancelled.set()
                try:
                    self.assertTrue(
                        contender_done.wait(timeout=0.3),
                        "different-key publication ignored deadline/cancellation",
                    )
                finally:
                    release_validation.set()
                    blocker_thread.join(timeout=5.0)
                    contender_thread.join(timeout=5.0)
                self.assertEqual(blocker_failures, [])
                self.assertEqual(len(contender_failures), 1)
                self.assertIsInstance(
                    contender_failures[0], AuditInfrastructureError
                )
                self.assertIsNone(cache.load(contender_configuration))

    def test_active_publication_guard_honors_deadline_and_cancellation(self):
        for mode in ("deadline", "cancel"):
            with self.subTest(mode=mode):
                directory = self.root / f"active-guard-{mode}"
                directory.mkdir()
                cancelled = threading.Event()
                if mode == "cancel":
                    cancelled.set()
                with mock.patch.object(
                    _PublicationGuard, "_lock", return_value=False,
                ) as acquire, self.assertRaisesRegex(
                    AuditInfrastructureError,
                    "cancelled" if mode == "cancel" else "deadline",
                ):
                    with _PublicationGuard(
                        directory / "active.lock",
                        time.monotonic() + (5.0 if mode == "cancel" else 0.03),
                        cancelled,
                    ):
                        self.fail("unavailable publication guard was entered")
                self.assertTrue(acquire.call_count > 0)
                self.assertTrue(all(
                    call.kwargs == {"blocking": False}
                    for call in acquire.call_args_list
                ))

    def test_publication_guard_replacement_survives_failed_cleanup(self):
        directory = self.root / "active-guard-replacement"
        directory.mkdir()
        guard_path = directory / "active.lock"
        displaced = directory / "owned-active.lock"
        replacement = b"replacement publication guard"

        def replace_before_cleanup(path):
            self.assertEqual(Path(path), guard_path)
            guard_path.rename(displaced)
            guard_path.write_bytes(replacement)

        cancelled = threading.Event()
        cancelled.set()
        try:
            with mock.patch(
                "gpu_capability_cache._before_file_temporary_cleanup",
                side_effect=replace_before_cleanup,
                create=True,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "publication guard.*replaced"
            ):
                with _PublicationGuard(
                    guard_path, time.monotonic() + 10.0, cancelled
                ):
                    self.fail("cancelled publication guard was entered")
            self.assertEqual(guard_path.read_bytes(), replacement)
        finally:
            guard_path.unlink(missing_ok=True)
            displaced.unlink(missing_ok=True)

    def test_record_access_replacement_survives_failed_cleanup(self):
        cache = PreprocessCache(self.cache_root)
        key = "a" * 64
        temporary = self.cache_root / f".tmp-access-{key}-{'f' * 32}"
        displaced = self.cache_root / "owned-access-temporary"
        replacement = b"replacement access marker"

        def replace_before_cleanup(path):
            self.assertEqual(Path(path), temporary)
            temporary.rename(displaced)
            temporary.write_bytes(replacement)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="f" * 32),
            ), mock.patch(
                "gpu_capability_cache.os.utime",
                side_effect=OSError("access timestamp failed"),
            ), mock.patch(
                "gpu_capability_cache._before_file_temporary_cleanup",
                side_effect=replace_before_cleanup,
                create=True,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "access temporary.*replaced"
            ):
                cache._record_access(key)
            self.assertEqual(temporary.read_bytes(), replacement)
        finally:
            temporary.unlink(missing_ok=True)
            displaced.unlink(missing_ok=True)

    def test_publication_guard_owned_cleanup_failure_is_fatal(self):
        directory = self.root / "active-guard-cleanup-failure"
        directory.mkdir()
        guard_path = directory / "active.lock"
        cancelled = threading.Event()
        cancelled.set()
        real_unlink = Path.unlink

        def reject_guard_unlink(path, *args, **kwargs):
            if Path(path) == guard_path:
                raise PermissionError("deterministic guard cleanup failure")
            return real_unlink(path, *args, **kwargs)

        try:
            with mock.patch.object(
                Path, "unlink", autospec=True, side_effect=reject_guard_unlink
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cannot remove cache publication guard temporary.*cancelled",
            ):
                with _PublicationGuard(
                    guard_path, time.monotonic() + 10.0, cancelled
                ):
                    self.fail("cancelled publication guard was entered")
            self.assertTrue(guard_path.exists())
        finally:
            guard_path.unlink(missing_ok=True)

    def test_record_access_owned_cleanup_failure_is_fatal(self):
        cache = PreprocessCache(self.cache_root)
        key = "b" * 64
        temporary = self.cache_root / f".tmp-access-{key}-{'e' * 32}"
        real_unlink = Path.unlink

        def reject_access_unlink(path, *args, **kwargs):
            if Path(path) == temporary:
                raise PermissionError("deterministic access cleanup failure")
            return real_unlink(path, *args, **kwargs)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="e" * 32),
            ), mock.patch(
                "gpu_capability_cache.os.utime",
                side_effect=OSError("access timestamp failed"),
            ), mock.patch.object(
                Path, "unlink", autospec=True, side_effect=reject_access_unlink
            ), self.assertRaisesRegex(
                AuditInfrastructureError,
                "cannot remove cache access temporary.*timestamp failed",
            ):
                cache._record_access(key)
            self.assertTrue(temporary.exists())
        finally:
            temporary.unlink(missing_ok=True)

    def test_explicit_cache_deadline_is_capped_from_operation_start(self):
        with mock.patch(
            "gpu_capability_cache.time.monotonic", return_value=1000.0
        ):
            self.assertEqual(
                PreprocessCache._operation_deadline(5000.0),
                1240.0,
            )
            self.assertEqual(
                PreprocessCache._operation_deadline(1100.0),
                1100.0,
            )

    def test_valid_winner_fails_if_losing_temporary_cannot_be_removed(self):
        winner = PreprocessCache(self.cache_root)
        winner.publish(self.view())
        loser = PreprocessCache(self.cache_root)
        with mock.patch(
            "gpu_capability_cache._remove_held_flat_directory", return_value=False
        ):
            with self.assertRaisesRegex(
                Exception, "losing cache publication temporary"
            ):
                loser.publish(self.view())

    def test_losing_temporary_replacement_is_never_deleted(self):
        winner = PreprocessCache(self.cache_root)
        winner.publish(self.view())
        loser = PreprocessCache(self.cache_root)
        real_remove = _remove_held_flat_directory
        replacement = None
        displaced = None
        received_identities = []

        def replace_before_remove(path, *, expected_identity=None, **kwargs):
            nonlocal replacement, displaced
            if path.name.startswith(".tmp-") and replacement is None:
                received_identities.append(expected_identity)
                displaced = path.with_name(f"{path.name}.displaced")
                path.rename(displaced)
                path.mkdir()
                (path / "manifest.json").write_bytes(b"replacement manifest")
                (path / "payload.bin").write_bytes(b"replacement payload")
                replacement = path
            return real_remove(
                path, expected_identity=expected_identity, **kwargs
            )

        try:
            with mock.patch(
                "gpu_capability_cache._remove_held_flat_directory",
                side_effect=replace_before_remove,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "temporary.*replaced|cannot remove.*temporary"
            ):
                loser.publish(self.view())
            self.assertIsNotNone(replacement)
            self.assertTrue(replacement.exists())
            self.assertEqual(len(received_identities), 1)
            self.assertIsNotNone(received_identities[0])
            self.assertIsNotNone(winner.load(self.configuration))
        finally:
            if replacement is not None and replacement.exists():
                real_remove(replacement)
            if displaced is not None and displaced.exists():
                real_remove(displaced)

    def test_publish_failure_never_creates_a_complete_entry(self):
        cache = PreprocessCache(self.cache_root)
        with mock.patch("gpu_capability_cache._write_payload", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(Exception, "disk full"):
                cache.publish(self.view())
        self.assertIsNone(cache.load(self.configuration))

    def test_publish_failure_surfaces_exact_owned_temporary_cleanup_failure(self):
        cache = PreprocessCache(self.cache_root)
        with mock.patch(
            "gpu_capability_cache._write_payload",
            side_effect=OSError("disk full"),
        ), mock.patch(
            "gpu_capability_cache._remove_held_flat_directory",
            return_value=False,
        ), self.assertRaisesRegex(
            AuditInfrastructureError,
            "cannot remove cache publication temporary after OSError: disk full",
        ):
            cache.publish(self.view())

    def test_preexisting_uuid_temporary_survives_failed_identity_capture(self):
        cache = PreprocessCache(self.cache_root)
        temporary = self.cache_root / (
            f".tmp-{cache._configuration_key(self.configuration)}-{'f' * 32}"
        )
        temporary.mkdir()
        manifest = temporary / "manifest.json"
        payload = temporary / "payload.bin"
        manifest.write_bytes(b"preexisting manifest")
        payload.write_bytes(b"preexisting payload")

        with mock.patch(
            "gpu_capability_cache.uuid.uuid4",
            return_value=mock.Mock(hex="f" * 32),
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "cannot publish GPU capability cache"
        ):
            cache.publish(self.view())

        self.assertTrue(temporary.is_dir())
        self.assertEqual(manifest.read_bytes(), b"preexisting manifest")
        self.assertEqual(payload.read_bytes(), b"preexisting payload")

    def test_replacement_during_temporary_identity_capture_survives(self):
        cache = PreprocessCache(self.cache_root)
        temporary = self.cache_root / (
            f".tmp-{cache._configuration_key(self.configuration)}-{'e' * 32}"
        )
        displaced = temporary.with_name(f"{temporary.name}.displaced")
        real_ordinary_directory = capability_cache._ordinary_directory

        def replace_before_identity_capture(path):
            if Path(path) == temporary and not displaced.exists():
                temporary.rename(displaced)
                temporary.mkdir()
                (temporary / "manifest.json").write_bytes(b"replacement manifest")
                (temporary / "payload.bin").write_bytes(b"replacement payload")
                raise OSError("temporary identity capture failed")
            return real_ordinary_directory(path)

        try:
            with mock.patch(
                "gpu_capability_cache.uuid.uuid4",
                return_value=mock.Mock(hex="e" * 32),
            ), mock.patch(
                "gpu_capability_cache._ordinary_directory",
                side_effect=replace_before_identity_capture,
            ), self.assertRaisesRegex(
                AuditInfrastructureError, "cannot publish GPU capability cache"
            ):
                cache.publish(self.view())

            self.assertTrue(temporary.is_dir())
            self.assertEqual(
                (temporary / "manifest.json").read_bytes(),
                b"replacement manifest",
            )
            self.assertEqual(
                (temporary / "payload.bin").read_bytes(),
                b"replacement payload",
            )
        finally:
            if temporary.exists():
                _remove_held_flat_directory(temporary)
            if displaced.exists():
                _remove_held_flat_directory(displaced)

    def test_invalid_entry_replacement_cleanup_failure_is_fatal_and_bounded(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        original_identity = _directory_identity(entry.stat())
        (entry / "manifest.json").write_text("{", encoding="utf-8")

        real_remove = _remove_held_flat_directory

        stale_identities = []

        def fail_stale(path, *, expected_identity=None):
            if path.name.startswith(".stale-"):
                stale_identities.append(expected_identity)
                return False
            return real_remove(path, expected_identity=expected_identity)

        with mock.patch(
            "gpu_capability_cache._remove_held_flat_directory",
            side_effect=fail_stale,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "cannot remove stale cache entry"
        ):
            cache.publish(self.view())

        self.assertEqual(stale_identities, [original_identity])
        stale = list(self.cache_root.glob(".stale-*"))
        self.assertEqual(len(stale), 1)
        self.assertFalse(entry.exists())
        cache.publish(self.view())
        self.assertEqual(len(list(self.cache_root.glob(".stale-*"))), 1)
        self.assertIsNotNone(cache.load(self.configuration))

    def test_cleanup_removes_only_expired_incomplete_entries(self):
        cache = PreprocessCache(self.cache_root)
        old = self.cache_root / ".tmp-old"
        active = self.cache_root / ".tmp-active"
        old.mkdir()
        active.mkdir()
        now = time.time()
        os.utime(old, (now - 3601, now - 3601))
        os.utime(active, (now - 3599, now - 3599))
        cache.cleanup(now)
        self.assertFalse(old.exists())
        self.assertTrue(active.exists())

    def test_cleanup_preserves_an_active_old_publication(self):
        cache = PreprocessCache(self.cache_root)
        active = self.cache_root / ".tmp-active-publication"
        active.mkdir()
        now = time.time()
        with _PublicationGuard(active / "active.lock", time.monotonic() + 10.0):
            os.utime(active, (now - 7200, now - 7200))
            cache.cleanup(now)
            self.assertTrue(active.exists())
        cache.cleanup(now)
        self.assertFalse(active.exists())

    def test_dependency_digests_are_shared_across_configurations(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        self.assertEqual(len(_hash_cache), 2)
        cache.publish(
            self.view(
                configuration=dataclasses.replace(self.configuration, digest="second")
            )
        )
        self.assertEqual(len(_hash_cache), 2)

    def test_cleanup_removes_complete_entries_unused_for_fourteen_days(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        access = cache._access_path(self.entry(cache).name)
        now = time.time()
        os.utime(access, (now - 14 * 86400 - 1, now - 14 * 86400 - 1))
        cache.cleanup(now)
        self.assertFalse(self.entry(cache).exists())

    def test_cleanup_evicts_lru_to_entry_and_byte_limits(self):
        cache = PreprocessCache(self.cache_root, maximum_bytes=10**9, maximum_entries=2)
        configurations = [dataclasses.replace(self.configuration, digest=f"cfg-{index}") for index in range(3)]
        for index, configuration in enumerate(configurations):
            cache.publish(self.view(configuration=configuration))
            access = cache._access_path(cache._configuration_key(configuration))
            os.utime(access, (100 + index, 100 + index))
        cache.cleanup(1000.0)
        self.assertIsNone(cache.load(configurations[0]))
        self.assertIsNotNone(cache.load(configurations[1]))
        self.assertIsNotNone(cache.load(configurations[2]))

        remaining = [path for path in self.cache_root.iterdir() if path.name[0].isalnum()]
        total = sum(file.stat().st_size for entry in remaining for file in entry.iterdir())
        byte_cache = PreprocessCache(self.cache_root, maximum_bytes=max(1, total - 1), maximum_entries=10)
        byte_cache.cleanup(time.time())
        self.assertLessEqual(
            sum(file.stat().st_size for entry in self.cache_root.iterdir() if entry.name[0].isalnum() for file in entry.iterdir()),
            max(1, total - 1),
        )

    def test_cleanup_refuses_links_and_is_idempotent(self):
        cache = PreprocessCache(self.cache_root)
        outside = self.root / "outside"
        outside.mkdir()
        protected = outside / "keep.txt"
        protected.write_text("keep", encoding="utf-8")
        link = self.cache_root / ".tmp-link"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks unavailable: {error}")
        cache.cleanup(time.time() + 7200)
        cache.cleanup(time.time() + 7200)
        self.assertEqual(protected.read_text(encoding="utf-8"), "keep")
        self.assertTrue(link.is_symlink())

    def test_cleanup_quarantines_before_deleting_a_swapped_target(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        access = cache._access_path(entry.name)
        now = time.time()
        os.utime(access, (now - 14 * 86400 - 1, now - 14 * 86400 - 1))
        replacement = entry / "keep.txt"

        def replace_after_quarantine(_quarantined: Path) -> None:
            entry.mkdir()
            replacement.write_text("keep", encoding="utf-8")

        with mock.patch(
            "gpu_capability_cache._after_cleanup_quarantine",
            create=True,
            side_effect=replace_after_quarantine,
        ) as hook:
            cache.cleanup(now)
        hook.assert_called_once()
        self.assertEqual(replacement.read_text(encoding="utf-8"), "keep")

    def test_cleanup_holds_quarantined_directory_during_swap_attempt(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        access = cache._access_path(entry.name)
        outside = self.root / "outside-held"
        outside.mkdir()
        protected = outside / "keep.txt"
        protected.write_text("keep", encoding="utf-8")
        moved = self.cache_root / ".moved-held-entry"
        outcome: list[str] = []
        now = time.time()
        os.utime(access, (now - 14 * 86400 - 1, now - 14 * 86400 - 1))

        def attempt_swap(quarantine: Path) -> None:
            try:
                os.rename(quarantine, moved)
            except OSError:
                outcome.append("blocked")
                return
            if os.name == "nt":
                outcome.append("unblocked")
                os.rename(moved, quarantine)
                return
            quarantine.symlink_to(outside, target_is_directory=True)
            outcome.append("swapped")

        with mock.patch(
            "gpu_capability_cache._after_cleanup_quarantine",
            side_effect=attempt_swap,
        ):
            cache.cleanup(now)
        self.assertEqual(protected.read_text(encoding="utf-8"), "keep")
        if os.name == "nt":
            self.assertEqual(outcome, ["blocked"])
        else:
            self.assertEqual(outcome, ["swapped"])
            self.assertEqual(list(moved.iterdir()), [])

    def test_cleanup_refuses_to_delete_when_held_directory_open_fails(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        access = cache._access_path(entry.name)
        now = time.time()
        os.utime(access, (now - 14 * 86400 - 1, now - 14 * 86400 - 1))
        with mock.patch(
            "gpu_capability_cache._HeldDirectory",
            create=True,
            side_effect=OSError("cannot hold directory"),
        ):
            cache.cleanup(now)
        quarantined_payloads = list(
            self.cache_root.glob(f".quarantine-{entry.name}-*/payload.bin")
        )
        self.assertEqual(len(quarantined_payloads), 1)

    def test_cache_root_replacement_is_rejected_before_publication(self):
        cache = PreprocessCache(self.cache_root)
        moved = self.root / "moved-cache"
        self.cache_root.rename(moved)
        self.cache_root.mkdir()
        with self.assertRaisesRegex(Exception, "identity|replaced"):
            cache.publish(self.view())
        self.assertEqual(list(self.cache_root.iterdir()), [])

    def test_invalid_entry_cleanup_removes_orphan_sidecar_and_counts_sidecar_bytes(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        entry = self.entry(cache)
        access = cache._access_path(entry.name)
        (entry / "manifest.json").write_text("{", encoding="utf-8")
        now = time.time()
        os.utime(entry, (now - 3601, now - 3601))
        cache.cleanup(now)
        self.assertFalse(entry.exists())
        self.assertFalse(access.exists())

        cache.publish(self.view())
        entry = self.entry(cache)
        access = cache._access_path(entry.name)
        entry_bytes = sum(path.stat().st_size for path in entry.iterdir())
        bounded = PreprocessCache(
            self.cache_root,
            maximum_bytes=entry_bytes,
            maximum_entries=10,
        )
        bounded.cleanup(time.time())
        self.assertFalse(entry.exists())
        self.assertFalse(access.exists())

    def test_hardlinked_manifest_is_refused(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        manifest = self.entry(cache) / "manifest.json"
        alias = self.root / "manifest-alias"
        try:
            os.link(manifest, alias)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "namespace|alias|ordinary"
        ):
            cache.load(self.configuration)


class ConfigurationAuditCacheTests(unittest.TestCase, _PreprocessCacheFixture):
    def setUp(self) -> None:
        _PreprocessCacheFixture.setUp(self)
        dependency = DependencyDigest(
            "production",
            PurePosixPath("playback/a.cpp"),
            self.main,
            hashlib.sha256(self.main_path.read_bytes()).hexdigest(),
        )
        self.engine = "e" * 64
        self.configuration = dataclasses.replace(
            self.configuration, digest="c" * 64
        )
        self.result = ConfigurationAuditResult(
            self.configuration.digest,
            self.engine,
            (dependency,),
            (PurePosixPath("playback/a.cpp"),),
            (
                AuditResultFinding(
                    PurePosixPath("playback/a.cpp"),
                    1,
                    "lease.nativeHandle()",
                    "outside lease",
                ),
            ),
        )
        self.production_snapshot = {dependency.role_relative_path: dependency}
        self.publication_permit = ConfigurationAuditPublicationPermit(
            self.result.configuration_digest,
            self.result.audit_engine_fingerprint,
            self.result.dependencies,
        )
        self.pipeline_deadline = time.monotonic() + 30.0

    def tearDown(self) -> None:
        _PreprocessCacheFixture.tearDown(self)

    def _transport_outcome(
        self,
        task_id: str,
        generation: int,
        *,
        worker_slot: int = 2,
        result: ConfigurationAuditResult | None = None,
        stdout_bytes: int = 17,
        stages: WorkerStageTimings | None = None,
        return_owned: bool = False,
    ):
        result = self.result if result is None else result
        stages = stages or WorkerStageTimings(1.0, 2.0, 3.0, 4.0)
        parent = PerTaskCompactReservation(task_id, generation, 32 << 20)
        capability = parent.issue_worker_transport_capability(
            task_id,
            generation,
            result.configuration_digest,
            result.audit_engine_fingerprint,
            worker_slot,
        )
        worker = PerTaskCompactReservation.for_worker_transport(capability)
        worker.require_before_discovery(task_id, generation)
        payload = capability_cache.encode_configuration_audit_result(result)
        bounds = CompactResultDraftBounds(
            len(result.dependencies),
            len(result.reached_production),
            len(result.findings),
            sum(
                len(item.stable_role.encode("ascii"))
                + len(item.role_relative_path.as_posix().encode("utf-8"))
                + len(str(item.identity.canonical).encode("utf-8"))
                + (
                    len(item.identity.relative.as_posix().encode("utf-8"))
                    if item.identity.relative is not None
                    else 0
                )
                for item in result.dependencies
            )
            + sum(
                len(path.as_posix().encode("utf-8"))
                for path in result.reached_production
            )
            + sum(
                len(item.path.as_posix().encode("utf-8"))
                for item in result.findings
            ),
            sum(len(item.expression.encode("utf-8")) for item in result.findings),
            sum(len(item.reason.encode("utf-8")) for item in result.findings),
        )
        worker.require_within_pre_dispatch_reservation(
            task_id, len(payload), bounds, 4096
        )
        worker.record_exact_canonical_json(task_id, len(payload))
        ownership = worker.begin_result_ownership(task_id, generation)
        owned = dataclasses.replace(result, _transport_ownership=ownership)
        if return_owned:
            return parent, capability, owned
        transport = capability_cache.encode_configuration_audit_result_transport(
            owned, capability, stdout_bytes, stages
        )
        return (
            parent,
            capability,
            ConfigurationAuditTransportOutcome(
                transport, stdout_bytes, stages
            ),
        )

    def test_result_cache_round_trips_without_token_payload(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        loaded = cache.load(
            self.configuration,
            self.dependency_roots,
            self.engine,
            self.production_snapshot,
            self.pipeline_deadline,
        )
        self.assertEqual(loaded, self.result)
        self.assertFalse(
            any(path.suffix == ".bin" for path in self.cache_root.rglob("*"))
        )

    def test_combined_key_is_length_framed(self):
        first = audit_cache_key("a" * 64, "bc" + "0" * 62)
        second = audit_cache_key("ab" + "0" * 62, "c" + "0" * 63)
        self.assertNotEqual(first, second)

    def test_compact_result_codec_is_canonical_bounded_and_rejects_duplicates(self):
        payload = capability_cache.encode_configuration_audit_result(self.result)
        self.assertEqual(
            capability_cache.decode_configuration_audit_result(payload),
            self.result,
        )
        duplicate = b'{"schema":"duplicate",' + payload[1:]
        with self.assertRaisesRegex(AuditInfrastructureError, "payload"):
            capability_cache.decode_configuration_audit_result(duplicate)
        with mock.patch("gpu_capability_cache.json.loads") as loads, self.assertRaises(
            AuditInfrastructureError
        ):
            capability_cache.decode_configuration_audit_result(
                b"{" + b"x" * ((4 << 20) + 1)
            )
        loads.assert_not_called()

    def test_raw_transport_rejects_empty_truncated_oversize_and_malformed_headers(self):
        cases = (
            ("empty", None),
            ("truncated", b"x"),
            (
                "oversize",
                b"x" * (capability_cache._AUDIT_TRANSPORT_HEADER_BYTES + 1),
            ),
            (
                "malformed",
                b"\0" * capability_cache._AUDIT_TRANSPORT_HEADER_BYTES,
            ),
        )
        for name, frame in cases:
            with self.subTest(name=name):
                reservation, capability, _outcome = self._transport_outcome(
                    f"raw-{name}", 11
                )
                receiving, sending = multiprocessing.Pipe(duplex=False)
                if frame is not None:
                    sending.send_bytes(frame)
                sending.close()
                with self.assertRaisesRegex(
                    AuditInfrastructureError,
                    "transport.*(truncated|limit|malformed|authentication)",
                ):
                    capability_runner.receive_configuration_audit_outcome_from_pipe(
                        reservation,
                        capability,
                        receiving,
                        time.monotonic() + 2.0,
                    )
                receiving.close()
                self.assertTrue(reservation.released)
                self.assertEqual(
                    reservation.release_phase, "receiver-transport-rejected"
                )

    def test_raw_transport_observes_deadline_exit_and_each_cancel_boundary(self):
        reservation, capability, _outcome = self._transport_outcome(
            "raw-stall", 12
        )
        receiving, sending = multiprocessing.Pipe(duplex=False)
        started = time.monotonic()
        with self.assertRaisesRegex(AuditInfrastructureError, "deadline"):
            capability_runner.receive_configuration_audit_outcome_from_pipe(
                reservation,
                capability,
                receiving,
                started + 0.08,
                worker_alive=lambda: True,
            )
        self.assertLess(time.monotonic() - started, 0.5)
        sending.close()
        receiving.close()
        self.assertTrue(reservation.released)

        reservation, capability, outcome = self._transport_outcome(
            "raw-exit", 13
        )
        receiving, sending = multiprocessing.Pipe(duplex=False)
        sending.send_bytes(
            capability_cache._encode_configuration_audit_transport_header(outcome)
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "worker exited"):
            capability_runner.receive_configuration_audit_outcome_from_pipe(
                reservation,
                capability,
                receiving,
                time.monotonic() + 2.0,
                worker_alive=lambda: False,
            )
        sending.close()
        receiving.close()
        self.assertTrue(reservation.released)

        class SequencedCancellation:
            def __init__(self, trigger: int) -> None:
                self.calls = 0
                self.trigger = trigger

            def is_set(self) -> bool:
                self.calls += 1
                return self.calls >= self.trigger

        for label, trigger in (("before", 1), ("during", 2), ("after", 3)):
            with self.subTest(cancel=label):
                reservation, capability, outcome = self._transport_outcome(
                    f"raw-cancel-{label}", 14
                )
                receiving, sending = multiprocessing.Pipe(duplex=False)
                sending.send_bytes(
                    capability_cache._encode_configuration_audit_transport_header(
                        outcome
                    )
                )
                sending.send_bytes(outcome.transport.payload)
                cancellation = SequencedCancellation(trigger)
                with self.assertRaisesRegex(AuditInfrastructureError, "cancelled"):
                    capability_runner.receive_configuration_audit_outcome_from_pipe(
                        reservation,
                        capability,
                        receiving,
                        time.monotonic() + 2.0,
                        cancel_event=cancellation,
                    )
                sending.close()
                receiving.close()
                self.assertTrue(reservation.released)

    @unittest.skipIf(
        os.name == "nt",
        "Windows message-mode pipe writes expose only complete messages",
    )
    def test_raw_transport_mid_frame_stall_and_exit_are_deadline_safe(self):
        context = multiprocessing.get_context("spawn")
        for boundary, mode in (
            ("header", "stall"),
            ("header", "exit"),
            ("payload", "stall"),
            ("payload", "exit"),
        ):
            with self.subTest(boundary=boundary, mode=mode):
                reservation, capability, outcome = self._transport_outcome(
                    f"partial-{boundary}-{mode}", 21
                )
                header = (
                    capability_cache._encode_configuration_audit_transport_header(
                        outcome
                    )
                )
                frame = (
                    header if boundary == "header" else outcome.transport.payload
                )
                prefix_frame = header if boundary == "payload" else None
                wire_bytes = 2 if boundary == "header" else 4 + len(frame) // 2
                receiving, sending = context.Pipe(duplex=False)
                ready_event = context.Event()
                process = context.Process(
                    target=_write_partial_audit_frame_from_spawned_worker,
                    args=(
                        sending,
                        frame,
                        wire_bytes,
                        mode,
                        ready_event,
                        prefix_frame,
                    ),
                )
                process.start()
                sending.close()
                self.assertTrue(ready_event.wait(timeout=10.0))
                started = time.monotonic()
                expected = "deadline" if mode == "stall" else "truncated|worker exited"
                with self.assertRaisesRegex(
                    AuditInfrastructureError, expected
                ):
                    capability_runner.receive_configuration_audit_outcome_from_pipe(
                        reservation,
                        capability,
                        receiving,
                        started + 0.25,
                        worker_alive=process.is_alive,
                    )
                self.assertLess(time.monotonic() - started, 1.0)
                receiving.close()
                if mode == "stall":
                    process.terminate()
                process.join(timeout=5.0)
                self.assertFalse(process.is_alive())
                self.assertEqual(process.exitcode, -15 if mode == "stall" else 23)
                process.close()
                self.assertTrue(reservation.released)
                self.assertEqual(
                    reservation.release_phase, "receiver-transport-rejected"
                )
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "already released"
                ):
                    reservation.release_worker_transport_capability(
                        capability, "duplicate-partial-frame-release"
                    )

    def test_raw_transport_rejects_declared_length_and_digest_before_decode(self):
        for label, changes in (
            ("length", {"encoded_bytes": 1}),
            ("digest", {"payload_sha256": "0" * 64}),
        ):
            with self.subTest(label=label):
                reservation, capability, outcome = self._transport_outcome(
                    f"raw-{label}", 15
                )
                receipt = dataclasses.replace(
                    outcome.transport.receipt, **changes
                )
                forged_transport = dataclasses.replace(
                    outcome.transport, receipt=receipt
                )
                forged = ConfigurationAuditTransportOutcome(
                    forged_transport, outcome.stdout_bytes, outcome.stages
                )
                receiving, sending = multiprocessing.Pipe(duplex=False)
                capability_cache.send_configuration_audit_transport(
                    sending, forged
                )
                sending.close()
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "payload authentication"
                ):
                    capability_runner.receive_configuration_audit_outcome_from_pipe(
                        reservation,
                        capability,
                        receiving,
                        time.monotonic() + 2.0,
                    )
                receiving.close()
                self.assertTrue(reservation.released)

    def test_raw_transport_accepts_exact_four_mib_frame_with_bounded_peak(self):
        reservation, capability, outcome = self._transport_outcome(
            "raw-near-limit", 16
        )
        payload = b"x" * (4 << 20)
        receipt = dataclasses.replace(
            outcome.transport.receipt,
            encoded_bytes=len(payload),
            payload_sha256=hashlib.sha256(payload).hexdigest(),
        )
        transport = dataclasses.replace(
            outcome.transport, payload=payload, receipt=receipt
        )
        outbound = ConfigurationAuditTransportOutcome(
            transport, outcome.stdout_bytes, outcome.stages
        )
        receiving, sending = multiprocessing.Pipe(duplex=False)
        sender = threading.Thread(
            target=capability_cache.send_configuration_audit_transport,
            args=(sending, outbound),
        )
        tracemalloc.start()
        started = time.monotonic()
        try:
            sender.start()
            inbound = capability_cache.receive_configuration_audit_transport(
                receiving, capability, time.monotonic() + 10.0
            )
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
            sender.join(timeout=10.0)
            sending.close()
            receiving.close()
        self.assertFalse(sender.is_alive())
        self.assertEqual(inbound.transport.payload, payload)
        self.assertLess(peak, 24 << 20)
        self.assertLess(time.monotonic() - started, 5.0)
        reservation.release_worker_transport_capability(
            capability, "raw-receive-test-complete"
        )

    def test_crossed_task_configuration_engine_slot_and_pipe_frames_reject(self):
        alternate = dataclasses.replace(
            self.result,
            configuration_digest="d" * 64,
            audit_engine_fingerprint="f" * 64,
        )
        first = self._transport_outcome(
            "cross-a", 17, worker_slot=1, stdout_bytes=101
        )
        second = self._transport_outcome(
            "cross-b",
            18,
            worker_slot=7,
            result=alternate,
            stdout_bytes=202,
            stages=WorkerStageTimings(4.0, 3.0, 2.0, 1.0),
        )
        first_receiving, first_sending = multiprocessing.Pipe(duplex=False)
        second_receiving, second_sending = multiprocessing.Pipe(duplex=False)
        capability_cache.send_configuration_audit_transport(
            first_sending, second[2]
        )
        capability_cache.send_configuration_audit_transport(
            second_sending, first[2]
        )
        first_sending.close()
        second_sending.close()

        def reject(item, connection):
            reservation, capability, _outcome = item
            with self.assertRaisesRegex(
                AuditInfrastructureError, "header authentication"
            ):
                capability_runner.receive_configuration_audit_outcome_from_pipe(
                    reservation,
                    capability,
                    connection,
                    time.monotonic() + 2.0,
                )
            return reservation.released

        with ThreadPoolExecutor(max_workers=2) as pool:
            rejected = tuple(pool.map(
                lambda pair: reject(*pair),
                ((first, first_receiving), (second, second_receiving)),
            ))
        first_receiving.close()
        second_receiving.close()
        self.assertEqual(rejected, (True, True))
        self.assertNotEqual(first[1].pipe_nonce, second[1].pipe_nonce)

        with self.assertRaisesRegex(
            AuditInfrastructureError, "transport outcome"
        ):
            ConfigurationAuditTransportOutcome(
                first[2].transport,
                second[2].stdout_bytes,
                second[2].stages,
            )

        third = self._transport_outcome("cross-c", 19, worker_slot=3)
        fourth = self._transport_outcome("cross-d", 20, worker_slot=4)
        receiving, sending = multiprocessing.Pipe(duplex=False)
        capability_cache.send_configuration_audit_transport(sending, third[2])
        sending.close()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "header authentication"
        ) as rejected_capability:
            capability_runner.receive_configuration_audit_outcome_from_pipe(
                third[0],
                fourth[1],
                receiving,
                time.monotonic() + 2.0,
            )
        receiving.close()
        self.assertTrue(third[0].released)
        self.assertTrue(any(
            "capability cleanup also failed" in note
            for note in getattr(rejected_capability.exception, "__notes__", ())
        ))
        fourth[0].release_worker_transport_capability(
            fourth[1], "crossed-capability-test-complete"
        )

    def test_transport_codec_moves_one_reservation_into_decoded_result(self):
        reservation = PerTaskCompactReservation("task-a", 7, 32 << 20)
        capability = reservation.issue_worker_transport_capability(
            "task-a", 7, self.result.configuration_digest, self.engine, 2
        )
        worker_reservation = PerTaskCompactReservation.for_worker_transport(
            capability
        )
        worker_reservation.require_before_discovery("task-a", 7)
        payload = capability_cache.encode_configuration_audit_result(self.result)
        path_bytes = sum(
            len(item.stable_role.encode("ascii"))
            + len(item.role_relative_path.as_posix().encode("utf-8"))
            + len(str(item.identity.canonical).encode("utf-8"))
            + (
                len(item.identity.relative.as_posix().encode("utf-8"))
                if item.identity.relative is not None
                else 0
            )
            for item in self.result.dependencies
        ) + sum(
            len(path.as_posix().encode("utf-8"))
            for path in self.result.reached_production
        ) + sum(
            len(item.path.as_posix().encode("utf-8"))
            for item in self.result.findings
        )
        bounds = CompactResultDraftBounds(
            len(self.result.dependencies),
            len(self.result.reached_production),
            len(self.result.findings),
            path_bytes,
            sum(
                len(item.expression.encode("utf-8"))
                for item in self.result.findings
            ),
            sum(
                len(item.reason.encode("utf-8"))
                for item in self.result.findings
            ),
        )
        worker_reservation.require_within_pre_dispatch_reservation(
            "task-a", len(payload), bounds, 4096
        )
        worker_reservation.record_exact_canonical_json("task-a", len(payload))
        ownership = worker_reservation.begin_result_ownership("task-a", 7)
        owned = dataclasses.replace(self.result, _transport_ownership=ownership)

        transport = capability_cache.encode_configuration_audit_result_transport(
            owned, capability, 17, WorkerStageTimings(1.0, 2.0, 3.0, 4.0)
        )
        with self.assertRaisesRegex(AuditInfrastructureError, "ownership.*transferred"):
            owned.release_transport_ownership()
        decoded = capability_cache.decode_configuration_audit_result_transport(
            transport, reservation, capability
        )
        self.assertEqual(decoded, self.result)
        self.assertIsNotNone(decoded._transport_ownership)
        self.assertEqual(reservation.owner_phase, "receiver-retained-result")
        with self.assertRaisesRegex(AuditInfrastructureError, "already consumed"):
            capability_cache.decode_configuration_audit_result_transport(
                transport, reservation, capability
            )
        decoded.release_transport_ownership()
        self.assertTrue(reservation.released)

    def test_transport_encode_cleanup_preserves_primary_error(self):
        parent, capability, owned = self._transport_outcome(
            "encode-cleanup", 22, return_owned=True
        )
        worker = owned._transport_ownership.reservation
        with mock.patch.object(
            PerTaskCompactReservation,
            "complete_worker_transport",
            side_effect=ValueError("encode primary"),
        ), mock.patch.object(
            PerTaskCompactReservation,
            "_release_result_ownership",
            side_effect=RuntimeError("encode cleanup secondary"),
        ), self.assertRaisesRegex(ValueError, "encode primary") as raised:
            capability_cache.encode_configuration_audit_result_transport(
                owned,
                capability,
                17,
                WorkerStageTimings(1.0, 2.0, 3.0, 4.0),
            )
        self.assertTrue(any(
            "encode cleanup secondary" in note
            for note in getattr(raised.exception, "__notes__", ())
        ))
        worker.release("encode-cleanup-test-complete")
        parent.release_worker_transport_capability(
            capability, "encode-cleanup-test-complete"
        )

    def test_transport_decode_triple_fault_preserves_primary_error(self):
        reservation, capability, outcome = self._transport_outcome(
            "decode-cleanup", 23
        )
        with mock.patch.object(
            capability_cache,
            "_decode_audit_result_payload",
            side_effect=AuditInfrastructureError("decode primary"),
        ), mock.patch.object(
            PerTaskCompactReservation,
            "release_worker_transport_capability",
            side_effect=RuntimeError("decode capability cleanup secondary"),
        ) as capability_release, mock.patch.object(
            PerTaskCompactReservation,
            "release",
            side_effect=OSError("decode fallback cleanup tertiary"),
        ) as fallback_release, self.assertRaisesRegex(
            AuditInfrastructureError, "decode primary"
        ) as raised:
            capability_cache.decode_configuration_audit_result_transport(
                outcome.transport, reservation, capability
            )
        capability_release.assert_called_once()
        fallback_release.assert_called_once()
        notes = getattr(raised.exception, "__notes__", ())
        self.assertTrue(any(
            "decode capability cleanup secondary" in note for note in notes
        ))
        self.assertTrue(any(
            "decode fallback cleanup tertiary" in note for note in notes
        ))
        self.assertFalse(reservation.released)
        reservation.release("decode-cleanup-test-complete")

    def test_spawn_pipe_moves_receipt_into_parent_owned_decoded_result(self):
        identity_semantics_before_spawn = (
            capability_audit._marshal_live_semantic_object(FileIdentity)
        )
        reservation = PerTaskCompactReservation("spawn-task", 9, 32 << 20)
        capability = reservation.issue_worker_transport_capability(
            "spawn-task", 9, self.result.configuration_digest, self.engine, 2
        )
        context = multiprocessing.get_context("spawn")
        receiving, sending = context.Pipe(duplex=False)
        process = context.Process(
            target=_send_owned_audit_transport_from_spawned_worker,
            args=(sending, capability, self.result, "send"),
        )
        process.start()
        sending.close()
        process.join(timeout=20.0)
        self.assertFalse(process.is_alive())
        self.assertEqual(process.exitcode, 0)
        transport = capability_cache.receive_configuration_audit_transport(
            receiving,
            capability,
            time.monotonic() + 20.0,
            worker_alive=lambda: False,
        ).transport
        receiving.close()
        process.close()
        self.assertEqual(
            capability_audit._marshal_live_semantic_object(FileIdentity),
            identity_semantics_before_spawn,
        )
        self.assertEqual(reservation.owner_phase, "worker-transport-dispatched")

        decoded = capability_cache.decode_configuration_audit_result_transport(
            transport, reservation, capability
        )
        self.assertEqual(decoded, self.result)
        self.assertEqual(reservation.owner_phase, "receiver-retained-result")
        decoded.release_transport_ownership()
        self.assertTrue(reservation.released)

    def test_parent_releases_once_for_decode_send_cancel_and_worker_death(self):
        def issued(task_id):
            reservation = PerTaskCompactReservation(task_id, 4, 32 << 20)
            capability = reservation.issue_worker_transport_capability(
                task_id, 4, self.result.configuration_digest, self.engine, 2
            )
            return reservation, capability

        cancelled, cancelled_capability = issued("cancelled")
        cancelled.release_worker_transport_capability(
            cancelled_capability, "parent-cancelled"
        )
        self.assertTrue(cancelled.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "already released"):
            cancelled.release_worker_transport_capability(
                cancelled_capability, "duplicate-cancel"
            )

        context = multiprocessing.get_context("spawn")
        rejected, rejected_capability = issued("decode-rejected")
        receiving, sending = context.Pipe(duplex=False)
        process = context.Process(
            target=_send_owned_audit_transport_from_spawned_worker,
            args=(sending, rejected_capability, self.result, "send"),
        )
        process.start()
        sending.close()
        transport = capability_cache.receive_configuration_audit_transport(
            receiving,
            rejected_capability,
            time.monotonic() + 20.0,
            worker_alive=process.is_alive,
        ).transport
        receiving.close()
        process.join(timeout=20.0)
        self.assertEqual(process.exitcode, 0)
        corrupted = dataclasses.replace(transport, payload=b"{}")
        with self.assertRaisesRegex(
            AuditInfrastructureError, "transport.*authentication"
        ):
            capability_cache.decode_configuration_audit_result_transport(
                corrupted, rejected, rejected_capability
            )
        self.assertTrue(rejected.released)
        with self.assertRaisesRegex(AuditInfrastructureError, "already released"):
            rejected.release_worker_transport_capability(
                rejected_capability, "duplicate-decode-failure"
            )

        for mode, expected_exit, release_phase in (
            ("send-failure", 17, "worker-send-failure"),
            ("worker-death", 19, "worker-death"),
        ):
            reservation, capability = issued(mode)
            receiving, sending = context.Pipe(duplex=False)
            if mode == "send-failure":
                receiving.close()
            process = context.Process(
                target=_send_owned_audit_transport_from_spawned_worker,
                args=(sending, capability, self.result, mode),
            )
            process.start()
            sending.close()
            if mode == "worker-death":
                receiving.close()
            process.join(timeout=20.0)
            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode, expected_exit)
            reservation.release_worker_transport_capability(
                capability, release_phase
            )
            self.assertTrue(reservation.released)
            with self.assertRaisesRegex(AuditInfrastructureError, "already released"):
                reservation.release_worker_transport_capability(
                    capability, "duplicate-failure"
                )

    def test_spawn_pipe_rejects_forged_exact_charge_before_decode_ownership(self):
        context = multiprocessing.get_context("spawn")
        for mode in ("forge-undercharge", "forge-overcharge"):
            with self.subTest(mode=mode):
                reservation = PerTaskCompactReservation(mode, 5, 32 << 20)
                capability = reservation.issue_worker_transport_capability(
                    mode, 5, self.result.configuration_digest, self.engine, 2
                )
                receiving, sending = context.Pipe(duplex=False)
                process = context.Process(
                    target=_send_owned_audit_transport_from_spawned_worker,
                    args=(sending, capability, self.result, mode),
                )
                process.start()
                sending.close()
                transport = capability_cache.receive_configuration_audit_transport(
                    receiving,
                    capability,
                    time.monotonic() + 20.0,
                    worker_alive=process.is_alive,
                ).transport
                receiving.close()
                process.join(timeout=20.0)
                self.assertFalse(process.is_alive())
                self.assertEqual(process.exitcode, 0)
                self.assertEqual(
                    reservation.owner_phase, "worker-transport-dispatched"
                )
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "transport.*charge"
                ):
                    capability_cache.decode_configuration_audit_result_transport(
                        transport, reservation, capability
                    )
                self.assertTrue(reservation.released)
                self.assertEqual(
                    reservation.release_phase, "receiver-transport-rejected"
                )
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "already released"
                ):
                    reservation.release_worker_transport_capability(
                        capability, "duplicate-forged-charge"
                    )

    def test_spawn_pipe_rejects_forged_receipt_envelope_bindings(self):
        context = multiprocessing.get_context("spawn")
        for mode in (
            "forge-task",
            "forge-generation",
            "forge-nonce",
            "forge-serial",
            "forge-hash",
        ):
            with self.subTest(mode=mode):
                reservation = PerTaskCompactReservation(mode, 6, 32 << 20)
                capability = reservation.issue_worker_transport_capability(
                    mode, 6, self.result.configuration_digest, self.engine, 2
                )
                receiving, sending = context.Pipe(duplex=False)
                process = context.Process(
                    target=_send_owned_audit_transport_from_spawned_worker,
                    args=(sending, capability, self.result, mode),
                )
                process.start()
                sending.close()
                with self.assertRaisesRegex(
                    AuditInfrastructureError,
                    "transport.*(authentication|malformed|limit)",
                ):
                    capability_cache.receive_configuration_audit_transport(
                        receiving,
                        capability,
                        time.monotonic() + 20.0,
                        worker_alive=process.is_alive,
                    )
                receiving.close()
                process.join(timeout=20.0)
                self.assertFalse(process.is_alive())
                self.assertEqual(process.exitcode, 0)
                reservation.release_worker_transport_capability(
                    capability, "receiver-transport-rejected"
                )
                self.assertTrue(reservation.released)
                self.assertEqual(
                    reservation.release_phase, "receiver-transport-rejected"
                )
                with self.assertRaisesRegex(
                    AuditInfrastructureError, "already released"
                ):
                    reservation.release_worker_transport_capability(
                        capability, "duplicate-forged-envelope"
                    )

    def test_spawn_pipe_rejects_canonical_cross_configuration_and_engine(self):
        context = multiprocessing.get_context("spawn")
        for mode in ("substitute-configuration", "substitute-engine"):
            with self.subTest(mode=mode):
                reservation = PerTaskCompactReservation(mode, 8, 32 << 20)
                capability = reservation.issue_worker_transport_capability(
                    mode, 8, self.result.configuration_digest, self.engine, 2
                )
                substituted = dataclasses.replace(
                    self.result,
                    configuration_digest=(
                        "d" * 64
                        if mode == "substitute-configuration"
                        else self.result.configuration_digest
                    ),
                    audit_engine_fingerprint=(
                        "f" * 64
                        if mode == "substitute-engine"
                        else self.result.audit_engine_fingerprint
                    ),
                )
                receiving, sending = context.Pipe(duplex=False)
                process = context.Process(
                    target=_send_owned_audit_transport_from_spawned_worker,
                    args=(sending, capability, substituted, "send"),
                )
                process.start()
                sending.close()
                transport = capability_cache.receive_configuration_audit_transport(
                    receiving,
                    capability,
                    time.monotonic() + 20.0,
                    worker_alive=process.is_alive,
                ).transport
                receiving.close()
                process.join(timeout=20.0)
                self.assertFalse(process.is_alive())
                self.assertEqual(process.exitcode, 0)
                with self.assertRaisesRegex(
                    AuditInfrastructureError,
                    "(configuration|engine|transport|payload)",
                ):
                    capability_cache.decode_configuration_audit_result_transport(
                        transport, reservation, capability
                    )
                self.assertTrue(reservation.released)
                self.assertEqual(
                    reservation.release_phase, "receiver-transport-rejected"
                )

    def test_large_valid_transport_decode_peak_is_covered_by_reservation(self):
        findings = tuple(
            AuditResultFinding(
                PurePosixPath(f"playback/g/{index:05d}.h"),
                1,
                "x",
                "r",
            )
            for index in range(45_000)
        )
        large_result = dataclasses.replace(self.result, findings=findings)
        reservation = PerTaskCompactReservation("large-decode", 10, 32 << 20)
        capability = reservation.issue_worker_transport_capability(
            "large-decode", 10, self.result.configuration_digest, self.engine, 2
        )
        worker_reservation = PerTaskCompactReservation.for_worker_transport(
            capability
        )
        worker_reservation.require_before_discovery("large-decode", 10)
        payload = capability_cache.encode_configuration_audit_result(large_result)
        path_bytes = sum(
            len(item.stable_role.encode("ascii"))
            + len(item.role_relative_path.as_posix().encode("utf-8"))
            + len(str(item.identity.canonical).encode("utf-8"))
            + (
                len(item.identity.relative.as_posix().encode("utf-8"))
                if item.identity.relative is not None
                else 0
            )
            for item in large_result.dependencies
        ) + sum(
            len(path.as_posix().encode("utf-8"))
            for path in large_result.reached_production
        ) + sum(
            len(item.path.as_posix().encode("utf-8"))
            for item in large_result.findings
        )
        bounds = CompactResultDraftBounds(
            len(large_result.dependencies),
            len(large_result.reached_production),
            len(large_result.findings),
            path_bytes,
            sum(
                len(item.expression.encode("utf-8"))
                for item in large_result.findings
            ),
            sum(
                len(item.reason.encode("utf-8"))
                for item in large_result.findings
            ),
        )
        worker_reservation.require_within_pre_dispatch_reservation(
            "large-decode", len(payload), bounds, 4096
        )
        worker_reservation.record_exact_canonical_json(
            "large-decode", len(payload)
        )
        ownership = worker_reservation.begin_result_ownership(
            "large-decode", 10
        )
        owned = dataclasses.replace(
            large_result, _transport_ownership=ownership
        )
        transport = capability_cache.encode_configuration_audit_result_transport(
            owned, capability, 17, WorkerStageTimings(1.0, 2.0, 3.0, 4.0)
        )
        del owned, ownership, large_result, findings
        gc.collect()
        tracemalloc.start()
        try:
            decoded = capability_cache.decode_configuration_audit_result_transport(
                transport, reservation, capability
            )
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(len(decoded.findings), 45_000)
        self.assertLessEqual(peak, transport.receipt.charged_bytes)
        self.assertLessEqual(peak, reservation.maximum_bytes)
        decoded.release_transport_ownership()

    def test_combined_key_is_used_for_path_lane_and_manifest(self):
        cache = ConfigurationAuditCache(self.cache_root)
        expected = audit_cache_key(self.configuration.digest, self.engine)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        self.assertEqual(cache.observed_entry_key, expected)
        self.assertEqual(
            cache.observed_lane,
            hashlib.sha256(f"preprocess:{expected}".encode()).digest()[0] % 8,
        )
        self.assertEqual(cache.observed_manifest_key, expected)

    def test_same_configuration_different_engines_coexist(self):
        cache = ConfigurationAuditCache(self.cache_root)
        for engine in ("d" * 64, "e" * 64):
            result = dataclasses.replace(
                self.result, audit_engine_fingerprint=engine
            )
            permit = dataclasses.replace(
                self.publication_permit, audit_engine_fingerprint=engine
            )
            cache.publish(
                self.configuration,
                self.dependency_roots,
                result,
                permit,
                self.pipeline_deadline,
            )
            self.assertEqual(
                cache.load(
                    self.configuration,
                    self.dependency_roots,
                    engine,
                    self.production_snapshot,
                    self.pipeline_deadline,
                ),
                result,
            )

    def test_concurrent_equal_publishers_accept_one_canonical_winner(self):
        start = threading.Barrier(2)

        def publish_equal():
            start.wait(timeout=5.0)
            return ConfigurationAuditCache(self.cache_root).publish(
                self.configuration,
                self.dependency_roots,
                self.result,
                self.publication_permit,
                time.monotonic() + 15.0,
            )

        with ThreadPoolExecutor(max_workers=2) as executor:
            published = tuple(
                future.result(timeout=20.0)
                for future in (
                    executor.submit(publish_equal),
                    executor.submit(publish_equal),
                )
            )
        self.assertEqual(published, (self.result, self.result))

    def test_concurrent_different_publishers_reject_the_losing_winner(self):
        different = dataclasses.replace(
            self.result,
            findings=(
                dataclasses.replace(
                    self.result.findings[0], reason="different canonical winner"
                ),
            ),
        )
        start = threading.Barrier(2)

        def publish(result):
            start.wait(timeout=5.0)
            try:
                return (
                    "ok",
                    ConfigurationAuditCache(self.cache_root).publish(
                        self.configuration,
                        self.dependency_roots,
                        result,
                        self.publication_permit,
                        time.monotonic() + 15.0,
                    ),
                )
            except BaseException as error:
                return ("error", type(error), str(error))

        with ThreadPoolExecutor(max_workers=2) as executor:
            outcomes = tuple(
                future.result(timeout=20.0)
                for future in (
                    executor.submit(publish, self.result),
                    executor.submit(publish, different),
                )
            )
        self.assertEqual(tuple(sorted(item[0] for item in outcomes)), ("error", "ok"))
        failure = next(item for item in outcomes if item[0] == "error")
        self.assertIs(failure[1], AuditInfrastructureError)
        self.assertRegex(failure[2], "winner differs")

    def test_cached_production_identity_must_match_initial_snapshot(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        dependency = self.result.dependencies[0]
        mismatched = dataclasses.replace(
            dependency,
            identity=dataclasses.replace(dependency.identity, inode=999999),
        )
        self.assertIsNone(
            cache.load(
                self.configuration,
                self.dependency_roots,
                self.engine,
                {dependency.role_relative_path: mismatched},
                self.pipeline_deadline,
            )
        )

    def test_authentic_corruption_is_miss_but_linked_entry_is_fatal(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        key = audit_cache_key(self.configuration.digest, self.engine)
        entry = cache._entry_path(key)
        (entry / "payload.json").write_bytes(b"{truncated")
        self.assertIsNone(
            cache.load(
                self.configuration,
                self.dependency_roots,
                self.engine,
                self.production_snapshot,
                self.pipeline_deadline,
            )
        )
        self.assertTrue(capability_cache._remove_held_flat_directory(entry))
        target = self.root / "linked-result"
        target.mkdir()
        try:
            entry.symlink_to(target, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks unavailable: {error}")
        with self.assertRaisesRegex(AuditInfrastructureError, "namespace|linked"):
            cache.load(
                self.configuration,
                self.dependency_roots,
                self.engine,
                self.production_snapshot,
                self.pipeline_deadline,
            )

    def test_prepare_rejects_legacy_full_view_partition(self):
        self.cache_root.mkdir()
        (self.cache_root / ("a" * 64)).mkdir()
        cache = ConfigurationAuditCache(self.cache_root)
        with self.assertRaisesRegex(AuditInfrastructureError, "full-view partition"):
            cache.prepare(self.pipeline_deadline)

    def test_cleanup_uses_access_time_and_enforces_entry_capacity(self):
        cache = ConfigurationAuditCache(self.cache_root, maximum_entries=2)
        configurations = tuple(
            dataclasses.replace(self.configuration, digest=f"{index + 1:064x}")
            for index in range(3)
        )
        now = time.time()
        for index, configuration in enumerate(configurations):
            result = dataclasses.replace(
                self.result, configuration_digest=configuration.digest
            )
            cache.publish(
                configuration,
                self.dependency_roots,
                result,
                dataclasses.replace(
                    self.publication_permit,
                    configuration_digest=configuration.digest,
                ),
                self.pipeline_deadline,
            )
            key = audit_cache_key(configuration.digest, self.engine)
            os.utime(cache._access_path(key), (now + index, now + index))
        cache.cleanup(now + 3, self.pipeline_deadline)
        present = tuple(
            cache._entry_path(audit_cache_key(item.digest, self.engine)).exists()
            for item in configurations
        )
        self.assertEqual(present, (False, True, True))

    @unittest.skipUnless(os.name == "nt", "Windows read-sharing contract")
    def test_windows_held_dependency_denies_write_and_delete(self):
        dependency = self.result.dependencies[0]
        with capability_cache._open_dependency_handle(dependency):
            with self.assertRaises(OSError):
                self.main_path.write_bytes(b"replacement")
            with self.assertRaises(OSError):
                self.main_path.unlink()
        self.assertTrue(self.main_path.exists())

    def test_dependency_hashing_stops_at_caller_deadline(self):
        with capability_cache._open_dependency_handle(
            self.result.dependencies[0]
        ) as held:
            held.deadline = time.monotonic() - 1.0
            with self.assertRaisesRegex(
                AuditInfrastructureError, "hashing deadline"
            ):
                capability_cache._hash_held_dependency(held)

    def test_cache_rejects_nonfinite_and_expired_deadlines_before_locking(self):
        cache = ConfigurationAuditCache(self.cache_root)
        for deadline in (
            float("nan"),
            float("inf"),
            float("-inf"),
            time.monotonic() - 1.0,
        ):
            with self.subTest(deadline=deadline), mock.patch.object(
                cache, "_root_lock"
            ) as root_lock, self.assertRaisesRegex(
                AuditInfrastructureError, "deadline"
            ):
                cache.prepare(deadline)
            root_lock.assert_not_called()

    def test_contended_root_and_key_locks_honor_the_caller_deadline(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        key = audit_cache_key(self.configuration.digest, self.engine)
        lane = hashlib.sha256(f"preprocess:{key}".encode()).digest()[0] % 8
        context = multiprocessing.get_context("spawn")
        for carrier, namespace, operation in (
            (
                ".preprocess-root.lock",
                None,
                lambda deadline: cache.prepare(deadline),
            ),
            (
                f".preprocess-key-{lane}.lock",
                ".preprocess-root.lock",
                lambda deadline: cache.load(
                    self.configuration,
                    self.dependency_roots,
                    self.engine,
                    self.production_snapshot,
                    deadline,
                ),
            ),
        ):
            with self.subTest(carrier=carrier):
                ready = context.Event()
                release = context.Event()
                queue = context.Queue()
                process = context.Process(
                    target=_hold_shared_lock_in_spawned_process,
                    args=(
                        str(self.cache_root), carrier, ready, release, queue,
                        namespace,
                    ),
                )
                process.start()
                self.assertTrue(ready.wait(timeout=5.0))
                started = time.monotonic()
                try:
                    with self.assertRaisesRegex(
                        AuditInfrastructureError, "lock deadline"
                    ):
                        operation(started + 0.15)
                    self.assertLess(time.monotonic() - started, 1.0)
                finally:
                    release.set()
                    process.join(timeout=5.0)
                    if process.is_alive():
                        process.kill()
                        process.join(timeout=5.0)
                self.assertEqual(queue.get(timeout=2.0), ("ok",))

    def test_noncanonical_authentic_manifest_is_a_cache_miss(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        entry = cache._entry_path(
            audit_cache_key(self.configuration.digest, self.engine)
        )
        manifest = json.loads((entry / "manifest.json").read_text("ascii"))
        (entry / "manifest.json").write_text(
            json.dumps(manifest, indent=2), encoding="ascii"
        )
        self.assertIsNone(
            cache.load(
                self.configuration,
                self.dependency_roots,
                self.engine,
                self.production_snapshot,
                self.pipeline_deadline,
            )
        )

    def test_load_many_rejects_mismatched_budget_before_cache_access(self):
        cache = ConfigurationAuditCache(self.cache_root)
        caller_budget = CompactResultMemoryBudget()
        aggregator_budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,),
            aggregator_budget,
            capability_cache.AuditLimits(),
        )
        with mock.patch.object(cache, "_prepare_for_operation") as prepare, \
             self.assertRaisesRegex(AuditInfrastructureError, "ownership"):
            cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, caller_budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        prepare.assert_not_called()

    @unittest.skipIf(os.name == "nt", "POSIX held-handle mutation semantics")
    def test_final_held_rehash_detects_changed_bytes_without_reopen(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        original_hash = capability_cache._hash_held_dependency
        calls = 0

        def mutate_after_initial(handle):
            nonlocal calls
            digest = original_hash(handle)
            calls += 1
            if calls == 1:
                self.main_path.write_bytes(b"changed dependency bytes")
            return digest

        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        with mock.patch(
            "gpu_capability_cache._hash_held_dependency",
            side_effect=mutate_after_initial,
        ), mock.patch(
            "gpu_capability_cache._open_dependency_handle",
            wraps=capability_cache._open_dependency_handle,
        ) as opened:
            batch = cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        self.assertEqual((batch.hit_count, batch.misses), (0, (self.configuration,)))
        self.assertEqual(opened.call_count, 1)

    @unittest.skipIf(os.name == "nt", "POSIX held-handle restoration semantics")
    def test_exact_restoration_before_final_held_rehash_is_equivalent(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        original = self.main_path.read_bytes()
        original_hash = capability_cache._hash_held_dependency
        calls = 0

        def mutate_and_restore(handle):
            nonlocal calls
            digest = original_hash(handle)
            calls += 1
            if calls == 1:
                self.main_path.write_bytes(b"x" * len(original))
                self.main_path.write_bytes(original)
            return digest

        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        with mock.patch(
            "gpu_capability_cache._hash_held_dependency",
            side_effect=mutate_and_restore,
        ):
            batch = cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        self.assertEqual((batch.hit_count, len(batch.misses)), (1, 0))

    def test_memory_budget_ownership_commits_and_releases_exactly(self):
        budget = CompactResultMemoryBudget(128 << 20)
        ownership = budget.reserve(4096)
        self.assertEqual(budget.reserved_bytes, 4096)
        ownership.commit()
        self.assertEqual((budget.reserved_bytes, budget.committed_bytes), (0, 4096))
        ownership.release()
        self.assertEqual(budget.live_bytes, 0)

    def test_all_251_results_survive_unchanged_warm_load(self):
        cache = ConfigurationAuditCache(self.cache_root)
        configurations = tuple(
            dataclasses.replace(self.configuration, digest=f"{index:064x}")
            for index in range(251)
        )
        for configuration in configurations:
            result = dataclasses.replace(
                self.result, configuration_digest=configuration.digest
            )
            permit = dataclasses.replace(
                self.publication_permit,
                configuration_digest=configuration.digest,
            )
            cache.publish(
                configuration,
                self.dependency_roots,
                result,
                permit,
                self.pipeline_deadline,
            )
        budget = CompactResultMemoryBudget(128 << 20)
        aggregator = StreamingResultAggregator(
            configurations, budget, capability_cache.AuditLimits()
        )
        with mock.patch(
            "gpu_capability_cache._open_dependency_handle",
            wraps=capability_cache._open_dependency_handle,
        ) as opened, mock.patch(
            "gpu_capability_cache._hash_held_dependency",
            wraps=capability_cache._hash_held_dependency,
        ) as reads:
            batch = cache.load_many(
                configurations,
                self.dependency_roots,
                self.engine,
                self.production_snapshot,
                budget,
                aggregator,
                CompactResultColdSlot(1024 * 1024),
                self.pipeline_deadline,
            )
        self.assertEqual((batch.hit_count, len(batch.misses)), (251, 0))
        self.assertEqual((opened.call_count, reads.call_count), (1, 2))
        summary = aggregator.finish()
        self.assertEqual(summary.configurations, tuple(item.digest for item in configurations))
        cold_budget = CompactResultMemoryBudget(128 << 20)
        cold_aggregator = StreamingResultAggregator(
            configurations, cold_budget, capability_cache.AuditLimits()
        )
        for configuration in configurations:
            cold_result = dataclasses.replace(
                self.result, configuration_digest=configuration.digest
            )
            cold_aggregator.accept_validated_result(
                configuration,
                cold_result,
                cold_budget.reserve(
                    capability_cache.compact_result_retained_bytes(cold_result)
                ).commit(),
            )
        cold_summary = cold_aggregator.finish()
        self.assertEqual(
            encode_canonical_summary(summary),
            encode_canonical_summary(cold_summary),
        )

    def test_load_many_decodes_each_authenticated_hit_only_once(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        with mock.patch(
            "gpu_capability_cache._decode_audit_result_payload",
            wraps=capability_cache._decode_audit_result_payload,
        ) as decoded:
            batch = cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        self.assertEqual((batch.hit_count, batch.misses), (1, ()))
        self.assertEqual(decoded.call_count, 1)

    def test_decoded_result_charge_outlives_every_cache_reference(self):
        cache = ConfigurationAuditCache(self.cache_root)
        configurations = tuple(
            dataclasses.replace(self.configuration, digest=f"{index + 1:064x}")
            for index in range(2)
        )
        for configuration in configurations:
            result = dataclasses.replace(
                self.result, configuration_digest=configuration.digest
            )
            cache.publish(
                configuration,
                self.dependency_roots,
                result,
                dataclasses.replace(
                    self.publication_permit,
                    configuration_digest=configuration.digest,
                ),
                self.pipeline_deadline,
            )

        calibration_budget = CompactResultMemoryBudget()
        calibration_aggregator = StreamingResultAggregator(
            configurations, calibration_budget, capability_cache.AuditLimits()
        )
        calibration = cache.load_many(
            configurations,
            self.dependency_roots,
            self.engine,
            self.production_snapshot,
            calibration_budget,
            calibration_aggregator,
            CompactResultColdSlot(1 << 20),
            self.pipeline_deadline,
        )
        self.assertEqual((calibration.hit_count, calibration.misses), (2, ()))

        class TrackedResult(ConfigurationAuditResult):
            __slots__ = ("__weakref__",)

        budget = CompactResultMemoryBudget(calibration_budget.peak_live_bytes)
        aggregator = StreamingResultAggregator(
            configurations, budget, capability_cache.AuditLimits()
        )
        original_read = cache._read_authenticated_entry
        original_reserve = budget.reserve
        original_hash = capability_cache._hash_held_dependency
        result_references = []
        admitted_results = 0
        hash_calls = 0

        def track_result_lifetime(*arguments, **keywords):
            candidate, result = original_read(*arguments, **keywords)
            tracked = TrackedResult(
                result.configuration_digest,
                result.audit_engine_fingerprint,
                result.dependencies,
                result.reached_production,
                result.findings,
            )
            result_references.append(weakref.ref(tracked))
            return candidate, tracked

        def enforce_admission_lifetime(byte_count, *, label="compact result"):
            nonlocal admitted_results
            if label == "batch retained result limit":
                admitted_results += 1
                if admitted_results == 2 and result_references:
                    gc.collect()
                    self.assertIsNone(
                        result_references[-1](),
                        "the previous decoded result reached the next admission",
                    )
            return original_reserve(byte_count, label=label)

        def enforce_final_hash_lifetime(handle):
            nonlocal hash_calls
            hash_calls += 1
            if hash_calls == 2:
                gc.collect()
                self.assertTrue(
                    all(reference() is None for reference in result_references),
                    "decoded results reached final dependency classification",
                )
            return original_hash(handle)

        with mock.patch.object(
            cache,
            "_read_authenticated_entry",
            side_effect=track_result_lifetime,
        ), mock.patch.object(
            budget,
            "reserve",
            side_effect=enforce_admission_lifetime,
        ), mock.patch(
            "gpu_capability_cache._hash_held_dependency",
            side_effect=enforce_final_hash_lifetime,
        ):
            batch = cache.load_many(
                configurations,
                self.dependency_roots,
                self.engine,
                self.production_snapshot,
                budget,
                aggregator,
                CompactResultColdSlot(1 << 20),
                self.pipeline_deadline,
            )
        gc.collect()
        self.assertEqual((batch.hit_count, batch.misses), (2, ()))
        self.assertEqual(budget.peak_live_bytes, calibration_budget.peak_live_bytes)
        self.assertEqual((admitted_results, hash_calls), (2, 2))
        self.assertTrue(all(reference() is None for reference in result_references))

    def test_failed_load_many_restores_only_its_own_cold_slot(self):
        cache = ConfigurationAuditCache(self.cache_root)
        configurations = (
            dataclasses.replace(self.configuration, digest="1" * 64),
            dataclasses.replace(self.configuration, digest="2" * 64),
        )
        hit_result = dataclasses.replace(
            self.result, configuration_digest=configurations[1].digest
        )
        cache.publish(
            configurations[1],
            self.dependency_roots,
            hit_result,
            dataclasses.replace(
                self.publication_permit,
                configuration_digest=configurations[1].digest,
            ),
            self.pipeline_deadline,
        )
        cold_slot = CompactResultColdSlot(1 << 20)

        def phase_patch(cache_under_test, phase):
            if phase == "decode":
                return mock.patch.object(
                    cache_under_test,
                    "_read_authenticated_entry",
                    side_effect=AuditInfrastructureError("forced decode failure"),
                )
            if phase == "aggregate":
                return mock.patch(
                    "gpu_capability_model._after_streaming_aggregate_mutation",
                    side_effect=AuditInfrastructureError("forced aggregate failure"),
                )
            if phase == "final_hash":
                original_hash = capability_cache._hash_held_dependency
                calls = 0

                def fail_final_hash(handle):
                    nonlocal calls
                    calls += 1
                    if calls == 2:
                        raise AuditInfrastructureError("forced final hash failure")
                    return original_hash(handle)

                return mock.patch(
                    "gpu_capability_cache._hash_held_dependency",
                    side_effect=fail_final_hash,
                )
            return mock.patch.object(
                cache_under_test,
                "_record_access",
                side_effect=AuditInfrastructureError("forced access failure"),
            )

        for phase in ("decode", "aggregate", "final_hash", "access"):
            for preexisting in (False, True):
                with self.subTest(phase=phase, preexisting=preexisting):
                    budget = CompactResultMemoryBudget()
                    aggregator = StreamingResultAggregator(
                        configurations, budget, capability_cache.AuditLimits()
                    )
                    if preexisting:
                        aggregator.reserve_cold_slot(cold_slot)
                    baseline_live = budget.live_bytes
                    baseline_cold = aggregator.cold_slot_reserved_bytes
                    with phase_patch(cache, phase), self.assertRaisesRegex(
                        AuditInfrastructureError, f"forced {phase.replace('_', ' ')}"
                    ):
                        cache.load_many(
                            configurations,
                            self.dependency_roots,
                            self.engine,
                            self.production_snapshot,
                            budget,
                            aggregator,
                            cold_slot,
                            self.pipeline_deadline,
                        )
                    self.assertEqual(aggregator.accepted_count, 0)
                    self.assertEqual(budget.live_bytes, baseline_live)
                    self.assertEqual(
                        aggregator.cold_slot_reserved_bytes, baseline_cold
                    )

    def test_exception_traceback_retains_exact_decoded_result_charge(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        baseline = budget.live_bytes
        retained_result_bytes = capability_cache.compact_result_retained_bytes(
            self.result
        )

        class TrackedResult(ConfigurationAuditResult):
            __slots__ = ("__weakref__",)

        original_read = cache._read_authenticated_entry
        original_reserve = budget.reserve
        result_reference = None

        def track_result(*arguments, **keywords):
            nonlocal result_reference
            candidate, result = original_read(*arguments, **keywords)
            tracked = TrackedResult(
                result.configuration_digest,
                result.audit_engine_fingerprint,
                result.dependencies,
                result.reached_production,
                result.findings,
            )
            result_reference = weakref.ref(tracked)
            return candidate, tracked

        def fail_growth_reservation(byte_count, *, label="compact result"):
            if label == "aggregate growth 128 MiB limit":
                raise AuditInfrastructureError("forced growth reservation failure")
            return original_reserve(byte_count, label=label)

        retained_error = None
        try:
            with mock.patch.object(
                cache, "_read_authenticated_entry", side_effect=track_result
            ), mock.patch.object(
                budget, "reserve", side_effect=fail_growth_reservation
            ):
                cache.load_many(
                    (self.configuration,),
                    self.dependency_roots,
                    self.engine,
                    self.production_snapshot,
                    budget,
                    aggregator,
                    CompactResultColdSlot(1 << 20),
                    self.pipeline_deadline,
                )
        except AuditInfrastructureError as error:
            retained_error = error
        else:
            self.fail("forced aggregate growth failure did not propagate")

        gc.collect()
        self.assertIsNotNone(result_reference)
        self.assertIsNotNone(result_reference())
        retained_charge_bytes = None
        retained_labels = None
        retained_decoded_charge = None
        current = retained_error.__traceback__
        while current is not None:
            if current.tb_frame.f_code.co_name == "_load_many_prepared":
                frame_locals = current.tb_frame.f_locals
                cleanup = frame_locals["ownership_cleanup"]
                ownerships = (
                    *cleanup._ownerships,
                    frame_locals["result_ownership"],
                )
                unique = {id(item): item for item in ownerships}
                retained_charge_bytes = sum(
                    item.byte_count for item in unique.values()
                )
                retained_labels = tuple(
                    sorted(item.label for item in unique.values())
                )
                retained_decoded_charge = frame_locals[
                    "result_ownership"
                ].byte_count
                self.assertTrue(all(item.committed for item in unique.values()))
                break
            current = current.tb_next
        del current, frame_locals, cleanup, ownerships, unique
        self.assertIn("batch retained result limit", retained_labels)
        self.assertIn("batch retained candidate metadata", retained_labels)
        self.assertIn("dependency validation metadata", retained_labels)
        self.assertEqual(retained_decoded_charge, retained_result_bytes)
        self.assertEqual(budget.live_bytes, baseline + retained_charge_bytes)
        self.assertEqual(aggregator.accepted_count, 0)

        traceback.clear_frames(retained_error.__traceback__)
        retained_error = retained_error.with_traceback(None)
        del retained_error
        gc.collect()
        self.assertIsNone(result_reference())
        self.assertEqual(budget.live_bytes, baseline)

    def test_final_hash_traceback_retains_candidate_and_validation_charges(self):
        cache = ConfigurationAuditCache(self.cache_root)
        configurations = tuple(
            dataclasses.replace(self.configuration, digest=f"{index + 1:064x}")
            for index in range(2)
        )
        for configuration in configurations:
            cache.publish(
                configuration,
                self.dependency_roots,
                dataclasses.replace(
                    self.result, configuration_digest=configuration.digest
                ),
                dataclasses.replace(
                    self.publication_permit,
                    configuration_digest=configuration.digest,
                ),
                self.pipeline_deadline,
            )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            configurations, budget, capability_cache.AuditLimits()
        )
        baseline = budget.live_bytes

        class TrackedCandidate(capability_cache._AuditCacheCandidate):
            __slots__ = ("__weakref__",)

        original_metadata = cache._read_authenticated_candidate_metadata
        original_hash = capability_cache._hash_held_dependency
        candidate_references = []
        retained_handles = []
        hashes = 0

        def track_candidate(*arguments, **keywords):
            candidate = original_metadata(*arguments, **keywords)
            if candidate is None:
                return None
            tracked = TrackedCandidate(
                candidate.configuration,
                candidate.key,
                candidate.entry_identity,
                candidate.payload_identity,
                candidate.dependencies,
                candidate.manifest_identity,
                candidate.payload_sha256,
                candidate.decoded_result_bytes,
                candidate.metadata_ownership,
            )
            candidate_references.append(weakref.ref(tracked))
            return tracked

        def fail_final_hash(handle):
            nonlocal hashes
            hashes += 1
            if hashes == 2:
                retained_handles.append(handle)
                raise AuditInfrastructureError("forced retained final hash failure")
            return original_hash(handle)

        retained_error = None
        try:
            with mock.patch.object(
                cache,
                "_read_authenticated_candidate_metadata",
                side_effect=track_candidate,
            ), mock.patch(
                "gpu_capability_cache._hash_held_dependency",
                side_effect=fail_final_hash,
            ):
                cache.load_many(
                    configurations,
                    self.dependency_roots,
                    self.engine,
                    self.production_snapshot,
                    budget,
                    aggregator,
                    CompactResultColdSlot(1 << 20),
                    self.pipeline_deadline,
                )
        except AuditInfrastructureError as error:
            retained_error = error
        else:
            self.fail("forced final hash failure did not propagate")

        charged_bytes = None
        labels = None
        retained_graph = None
        current = retained_error.__traceback__
        while current is not None:
            if current.tb_frame.f_code.co_name == "_load_many_prepared":
                frame_locals = current.tb_frame.f_locals
                cleanup = frame_locals["ownership_cleanup"]
                ownerships = tuple(cleanup._ownerships)
                charged_bytes = sum(item.byte_count for item in ownerships)
                labels = tuple(sorted(item.label for item in ownerships))
                retained_graph = (
                    len(frame_locals["candidates"]),
                    len(frame_locals["dependency_users"]),
                    len(frame_locals["dependency_representatives"]),
                    len(frame_locals["expected_dependencies"]),
                    all(item.committed for item in ownerships),
                )
                break
            current = current.tb_next
        del current, frame_locals, cleanup, ownerships
        gc.collect()
        self.assertEqual(hashes, 2)
        self.assertEqual(len(retained_handles), 1)
        self.assertIsNone(retained_handles[0].stream)
        self.assertTrue(all(reference() is not None for reference in candidate_references))
        self.assertEqual(
            retained_graph,
            (2, 1, 1, 2, True),
        )
        self.assertEqual(
            labels,
            (
                "batch retained candidate metadata",
                "batch retained candidate metadata",
                "dependency validation metadata",
            ),
        )
        self.assertEqual(budget.live_bytes, baseline + charged_bytes)

        retained_handles.clear()
        traceback.clear_frames(retained_error.__traceback__)
        retained_error = retained_error.with_traceback(None)
        del retained_error
        gc.collect()
        self.assertTrue(all(reference() is None for reference in candidate_references))
        self.assertEqual(budget.live_bytes, baseline)

    def test_decode_traceback_retains_every_workspace_and_object_charge(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        baseline = budget.live_bytes
        original_candidate_type = capability_cache._AuditCacheCandidate
        original_decode = capability_cache._decode_audit_result_payload
        candidate_constructions = 0
        result_reference = None

        class TrackedCandidate(original_candidate_type):
            __slots__ = ("__weakref__",)

        class TrackedResult(ConfigurationAuditResult):
            __slots__ = ("__weakref__",)

        def track_decoded_result(*arguments, **keywords):
            nonlocal result_reference
            result = original_decode(*arguments, **keywords)
            tracked = TrackedResult(
                result.configuration_digest,
                result.audit_engine_fingerprint,
                result.dependencies,
                result.reached_production,
                result.findings,
            )
            result_reference = weakref.ref(tracked)
            return tracked

        def fail_final_candidate_construction(*arguments, **keywords):
            nonlocal candidate_constructions
            candidate_constructions += 1
            candidate = TrackedCandidate(*arguments, **keywords)
            if candidate_constructions == 2:
                raise AuditInfrastructureError("forced retained decode failure")
            return candidate

        retained_error = None
        try:
            with mock.patch(
                "gpu_capability_cache._decode_audit_result_payload",
                side_effect=track_decoded_result,
            ), mock.patch(
                "gpu_capability_cache._AuditCacheCandidate",
                side_effect=fail_final_candidate_construction,
            ):
                cache.load_many(
                    (self.configuration,),
                    self.dependency_roots,
                    self.engine,
                    self.production_snapshot,
                    budget,
                    aggregator,
                    CompactResultColdSlot(1 << 20),
                    self.pipeline_deadline,
                )
        except AuditInfrastructureError as error:
            retained_error = error
        else:
            self.fail("forced decode failure did not propagate")

        live_ownerships = None
        retained_payload_bytes = None
        current = retained_error.__traceback__
        while current is not None:
            frame_name = current.tb_frame.f_code.co_name
            if frame_name == "_load_many_prepared":
                frame_locals = current.tb_frame.f_locals
                cleanup = frame_locals["ownership_cleanup"]
                candidates = tuple(cleanup._ownerships)
                transient = (
                    frame_locals["result_ownership"],
                    frame_locals["decode_workspace"],
                )
                unique = {id(item): item for item in (*candidates, *transient)}
                live_ownerships = tuple(
                    sorted(
                        (item.label, item.byte_count, item.committed)
                        for item in unique.values()
                    )
                )
            elif frame_name == "_read_authenticated_entry":
                payload = current.tb_frame.f_locals["payload"]
                retained_payload_bytes = len(payload)
            current = current.tb_next
        del current, frame_locals, cleanup, candidates, transient, unique, payload
        gc.collect()
        self.assertEqual(candidate_constructions, 2)
        self.assertIsNotNone(result_reference())
        self.assertGreater(retained_payload_bytes, 0)
        self.assertTrue(all(committed for _label, _bytes, committed in live_ownerships))
        self.assertEqual(
            tuple(label for label, _bytes, _committed in live_ownerships),
            (
                "batch retained candidate metadata",
                "batch retained result limit",
                "compact result decode workspace",
                "dependency validation metadata",
            ),
        )
        self.assertEqual(
            budget.live_bytes,
            baseline + sum(byte_count for _label, byte_count, _state in live_ownerships),
        )

        traceback.clear_frames(retained_error.__traceback__)
        retained_error = retained_error.with_traceback(None)
        del retained_error
        gc.collect()
        self.assertIsNone(result_reference())
        self.assertEqual(budget.live_bytes, baseline)

    def test_metadata_preparse_traceback_retains_payload_workspace_charges(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        baseline = budget.live_bytes
        retained_error = None

        try:
            with mock.patch(
                "gpu_capability_cache._preparse_audit_result_payload",
                side_effect=AuditInfrastructureError(
                    "forced retained metadata preparse failure"
                ),
            ):
                cache.load_many(
                    (self.configuration,),
                    self.dependency_roots,
                    self.engine,
                    self.production_snapshot,
                    budget,
                    aggregator,
                    CompactResultColdSlot(1 << 20),
                    self.pipeline_deadline,
                )
        except AuditInfrastructureError as error:
            retained_error = error
        else:
            self.fail("forced metadata preparse failure did not propagate")

        workspace_state = None
        retained_sizes = None
        current = retained_error.__traceback__
        while current is not None:
            if (
                current.tb_frame.f_code.co_name
                == "_read_authenticated_candidate_metadata"
            ):
                frame_locals = current.tb_frame.f_locals
                ownerships = (
                    frame_locals["manifest_scratch"],
                    frame_locals["scratch"],
                )
                workspace_state = tuple(
                    sorted(
                        (item.label, item.byte_count, item.committed)
                        for item in ownerships
                    )
                )
                retained_sizes = (
                    len(frame_locals["manifest_payload"]),
                    len(frame_locals["payload"]),
                )
                break
            current = current.tb_next
        del current, frame_locals, ownerships
        gc.collect()
        self.assertTrue(all(size > 0 for size in retained_sizes))
        self.assertEqual(
            tuple(label for label, _bytes, _state in workspace_state),
            (
                "compact result manifest workspace",
                "compact result metadata preparse workspace",
            ),
        )
        self.assertTrue(
            all(committed for _label, _bytes, committed in workspace_state)
        )
        self.assertEqual(
            budget.live_bytes,
            baseline
            + sum(byte_count for _label, byte_count, _state in workspace_state),
        )

        traceback.clear_frames(retained_error.__traceback__)
        retained_error = retained_error.with_traceback(None)
        del retained_error
        gc.collect()
        self.assertEqual(budget.live_bytes, baseline)

    def test_in_place_payload_and_manifest_replacement_cannot_become_a_hit(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        key = audit_cache_key(self.configuration.digest, self.engine)
        entry = cache._entry_path(key)
        altered = dataclasses.replace(
            self.result,
            findings=(
                dataclasses.replace(
                    self.result.findings[0], reason="altered after validation"
                ),
            ),
        )
        altered_payload = capability_cache.encode_configuration_audit_result(altered)
        altered_manifest = cache._manifest_bytes(key, altered_payload)
        original_read = cache._read_authenticated_entry

        def replace_before_final_decode(*arguments, **keywords):
            for path, payload in (
                (entry / "payload.json", altered_payload),
                (entry / "manifest.json", altered_manifest),
            ):
                with path.open("r+b") as stream:
                    stream.seek(0)
                    stream.write(payload)
                    stream.truncate()
                    stream.flush()
                    os.fsync(stream.fileno())
            return original_read(*arguments, **keywords)

        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        with mock.patch.object(
            cache,
            "_read_authenticated_entry",
            side_effect=replace_before_final_decode,
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "replaced|generation"
        ):
            cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )

    def test_dependency_handles_cover_decode_and_final_batch_classification(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        original_read = cache._read_authenticated_entry
        original_dependency = self.main_path.read_bytes()
        rewrite_denied = False

        def decode_then_rewrite(*arguments, **keywords):
            nonlocal rewrite_denied
            decoded = original_read(*arguments, **keywords)
            try:
                self.main_path.write_bytes(b"x" * len(self.main_path.read_bytes()))
            except OSError:
                rewrite_denied = True
            return decoded

        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        with mock.patch.object(
            cache,
            "_read_authenticated_entry",
            side_effect=decode_then_rewrite,
        ), mock.patch.object(
            cache, "_record_access", wraps=cache._record_access
        ) as recorded_access:
            batch = cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        if os.name == "nt":
            self.assertTrue(rewrite_denied)
            self.assertEqual((batch.hit_count, batch.misses), (1, ()))
            recorded_access.assert_called_once()
        else:
            self.assertFalse(rewrite_denied)
            self.assertEqual(
                (batch.hit_count, batch.misses),
                (0, (self.configuration,)),
            )
            self.assertEqual(aggregator.accepted_count, 0)
            recorded_access.assert_not_called()
            self.main_path.write_bytes(original_dependency)
            retry = cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
            self.assertEqual((retry.hit_count, retry.misses), (1, ()))
            self.assertEqual(aggregator.accepted_count, 1)

    @unittest.skipIf(os.name == "nt", "POSIX provisional batch rollback semantics")
    def test_final_drift_rolls_back_only_current_batch_and_records_no_access(self):
        cache = ConfigurationAuditCache(self.cache_root)
        configurations = tuple(
            dataclasses.replace(self.configuration, digest=f"{index + 1:064x}")
            for index in range(3)
        )
        results = tuple(
            dataclasses.replace(self.result, configuration_digest=item.digest)
            for item in configurations
        )
        for configuration, result in zip(
            configurations[1:], results[1:], strict=True
        ):
            cache.publish(
                configuration,
                self.dependency_roots,
                result,
                dataclasses.replace(
                    self.publication_permit,
                    configuration_digest=configuration.digest,
                ),
                self.pipeline_deadline,
            )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            configurations, budget, capability_cache.AuditLimits()
        )
        aggregator.accept_validated_result(
            configurations[0],
            results[0],
            budget.reserve(
                capability_cache.compact_result_retained_bytes(results[0])
            ).commit(),
        )
        original_dependency = self.main_path.read_bytes()
        original_read = cache._read_authenticated_entry
        mutated = False

        def decode_then_mutate(*arguments, **keywords):
            nonlocal mutated
            decoded = original_read(*arguments, **keywords)
            if not mutated:
                mutated = True
                self.main_path.write_bytes(b"x" * len(original_dependency))
            return decoded

        with mock.patch.object(
            cache,
            "_read_authenticated_entry",
            side_effect=decode_then_mutate,
        ), mock.patch.object(cache, "_record_access") as recorded_access:
            batch = cache.load_many(
                configurations[1:], self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        self.assertEqual((batch.hit_count, batch.misses), (0, configurations[1:]))
        self.assertEqual(aggregator.accepted_count, 1)
        recorded_access.assert_not_called()
        self.main_path.write_bytes(original_dependency)
        retry = cache.load_many(
            configurations[1:], self.dependency_roots, self.engine,
            self.production_snapshot, budget, aggregator,
            CompactResultColdSlot(1 << 20), self.pipeline_deadline,
        )
        self.assertEqual((retry.hit_count, retry.misses), (2, ()))
        self.assertEqual(aggregator.accepted_count, 3)

    def test_final_dependency_hash_honors_deadline_and_rolls_back_hits(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        baseline = budget.live_bytes
        original_hash = capability_cache._hash_held_dependency
        hashes = 0

        def expire_at_final_boundary(handle):
            nonlocal hashes
            hashes += 1
            if hashes == 2:
                handle.deadline = time.monotonic() - 1.0
            return original_hash(handle)

        with mock.patch(
            "gpu_capability_cache._hash_held_dependency",
            side_effect=expire_at_final_boundary,
        ), mock.patch.object(cache, "_record_access") as recorded_access, \
             self.assertRaisesRegex(AuditInfrastructureError, "hashing deadline"):
            cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        self.assertEqual(hashes, 2)
        self.assertEqual(aggregator.accepted_count, 0)
        self.assertEqual(budget.live_bytes, baseline)
        recorded_access.assert_not_called()

    def test_load_many_keeps_captured_candidate_exactly_charged_on_exception(self):
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            self.configuration,
            self.dependency_roots,
            self.result,
            self.publication_permit,
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            (self.configuration,), budget, capability_cache.AuditLimits()
        )
        baseline = budget.live_bytes
        captured = []
        original_metadata = cache._read_authenticated_candidate_metadata

        def capture_metadata(*arguments, **keywords):
            candidate = original_metadata(*arguments, **keywords)
            captured.append(candidate)
            return candidate

        with mock.patch.object(
            cache,
            "_read_authenticated_candidate_metadata",
            side_effect=capture_metadata,
        ), mock.patch(
            "gpu_capability_cache._open_dependency_handle",
            side_effect=AuditInfrastructureError("forced dependency open failure"),
        ), self.assertRaisesRegex(
            AuditInfrastructureError, "forced dependency open failure"
        ):
            cache.load_many(
                (self.configuration,), self.dependency_roots, self.engine,
                self.production_snapshot, budget, aggregator,
                CompactResultColdSlot(1 << 20), self.pipeline_deadline,
            )
        self.assertEqual(len(captured), 1)
        candidate_charge = captured[0].metadata_ownership.byte_count
        self.assertTrue(captured[0].metadata_ownership.committed)
        self.assertEqual(budget.live_bytes, baseline + candidate_charge)
        captured.clear()
        gc.collect()
        self.assertEqual(budget.live_bytes, baseline)

    def test_same_path_candidates_with_conflicting_identities_do_not_share_a_hit(self):
        authority = build_dependency_root_authority(
            self.main_path.parent, {"toolchain": self.compiler.parent}
        )
        old_identity = self.identity(self.compiler)
        old_dependency = DependencyDigest(
            "toolchain",
            PurePosixPath(self.compiler.name),
            old_identity,
            hashlib.sha256(self.compiler.read_bytes()).hexdigest(),
        )
        configurations = (
            dataclasses.replace(self.configuration, digest="1" * 64),
            dataclasses.replace(self.configuration, digest="2" * 64),
        )
        old_result = ConfigurationAuditResult(
            configurations[1].digest, self.engine, (old_dependency,), (), ()
        )
        cache = ConfigurationAuditCache(self.cache_root)
        cache.publish(
            configurations[1], authority, old_result,
            ConfigurationAuditPublicationPermit(
                configurations[1].digest, self.engine, (old_dependency,)
            ),
            self.pipeline_deadline,
        )
        original_bytes = self.compiler.read_bytes()
        self.compiler.unlink()
        self.compiler.write_bytes(original_bytes)
        new_identity = self.identity(self.compiler)
        if old_identity.inode == new_identity.inode:
            self.skipTest("filesystem reused the dependency identity")
        new_dependency = dataclasses.replace(old_dependency, identity=new_identity)
        new_result = ConfigurationAuditResult(
            configurations[0].digest, self.engine, (new_dependency,), (), ()
        )
        cache.publish(
            configurations[0], authority, new_result,
            ConfigurationAuditPublicationPermit(
                configurations[0].digest, self.engine, (new_dependency,)
            ),
            self.pipeline_deadline,
        )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            configurations, budget, capability_cache.AuditLimits()
        )
        batch = cache.load_many(
            configurations, authority, self.engine, {}, budget, aggregator,
            CompactResultColdSlot(1 << 20), self.pipeline_deadline,
        )
        self.assertEqual((batch.hit_count, batch.misses), (1, (configurations[1],)))

    def test_250_hits_reserve_one_cold_slot_and_finish_with_one_rebuild(self):
        cache = ConfigurationAuditCache(self.cache_root)
        configurations = tuple(
            dataclasses.replace(self.configuration, digest=f"{index:064x}")
            for index in range(251)
        )
        shared_expression = "x" * 48_000

        def result_for(index: int) -> ConfigurationAuditResult:
            findings = tuple(
                AuditResultFinding(
                    PurePosixPath(
                        f"playback/cold-frontier/{index:04d}-{finding_index:02d}.cpp"
                    ),
                    1,
                    shared_expression,
                    f"cold-frontier-{index:04d}-{finding_index:02d}",
                )
                for finding_index in range(9)
            )
            return ConfigurationAuditResult(
                configurations[index].digest,
                self.engine,
                self.result.dependencies,
                (),
                findings,
            )

        for index, configuration in enumerate(configurations[:250]):
            result = result_for(index)
            cache.publish(
                configuration,
                self.dependency_roots,
                result,
                dataclasses.replace(
                    self.publication_permit,
                    configuration_digest=configuration.digest,
                ),
                self.pipeline_deadline,
            )
        budget = CompactResultMemoryBudget()
        aggregator = StreamingResultAggregator(
            configurations, budget, capability_cache.AuditLimits()
        )
        cold_slot = CompactResultColdSlot(16 << 20)
        batch = cache.load_many(
            configurations,
            self.dependency_roots,
            self.engine,
            self.production_snapshot,
            budget,
            aggregator,
            cold_slot,
            self.pipeline_deadline,
        )
        self.assertEqual((batch.hit_count, len(batch.misses)), (250, 1))
        self.assertEqual(batch.cold_slot_reserved_bytes, 16 << 20)
        rebuilt = result_for(250)
        aggregator.accept_validated_result(
            configurations[250],
            rebuilt,
            aggregator.ownership_for_cold_result(rebuilt),
        )
        summary = aggregator.finish()
        self.assertEqual(
            summary.configurations,
            tuple(configuration.digest for configuration in configurations),
        )
        self.assertGreater(budget.peak_live_bytes, 120 << 20)
        self.assertLessEqual(budget.peak_live_bytes, 128 << 20)


if __name__ == "__main__":
    unittest.main()
