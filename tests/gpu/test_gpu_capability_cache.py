from __future__ import annotations

import dataclasses
import base64
import hashlib
import inspect
import json
import multiprocessing
import os
import stat
import sys
import tempfile
import threading
import time
import unittest
from array import array
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_cache import (  # noqa: E402
    CompilerInspectionCache,
    PreprocessCache,
    _hash_cache,
    _hash_lock,
    _PublicationGuard,
    compiler_inspection_cache_key,
)
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    CompactTokenSequence,
    CompilerExecutableCapability,
    CompilerFamily,
    CompilerInspection,
    DependencyRootBinding,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
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

    def test_missing_replaced_and_hardlinked_dependency_are_misses(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        self.header_path.unlink()
        self.assertIsNone(cache.load(self.configuration))
        self.header_path.write_text("#pragma once\n", encoding="utf-8")
        self.assertIsNone(cache.load(self.configuration))

        cache.publish(self.view(dependencies=(self.main, self.identity(self.header_path, "playback/a.h"))))
        alias = self.root / "alias.h"
        try:
            os.link(self.header_path, alias)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        self.assertIsNone(cache.load(self.configuration))

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

    def test_payload_replacement_between_hash_and_deserialize_is_a_miss(self):
        cache = PreprocessCache(self.cache_root)
        cache.publish(self.view())
        payload = self.entry(cache) / "payload.bin"

        def replace_after_hash(_path: Path) -> None:
            replacement = payload.with_name("replacement.bin")
            replacement.write_bytes(payload.read_bytes().replace(b"lease", b"zease", 1))
            os.replace(replacement, payload)

        with mock.patch(
            "gpu_capability_cache._before_payload_parse",
            side_effect=replace_after_hash,
        ):
            self.assertIsNone(cache.load(self.configuration))

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

    def test_two_cache_instances_remove_atomic_rename_loser_temporary(self):
        first = PreprocessCache(self.cache_root)
        second = PreprocessCache(self.cache_root)
        key = first._configuration_key(self.configuration)
        entry = self.cache_root / key
        barrier = threading.Barrier(2)
        real_rename = os.rename
        failures: list[BaseException] = []

        def rename(source, destination) -> None:
            if Path(source).name.startswith(f".tmp-{key}-") and Path(destination) == entry:
                barrier.wait(timeout=5.0)
            real_rename(source, destination)

        def publish(cache: PreprocessCache) -> None:
            try:
                cache.publish(self.view())
            except BaseException as error:
                failures.append(error)

        with mock.patch("gpu_capability_cache.os.rename", side_effect=rename):
            threads = [
                threading.Thread(target=publish, args=(cache,))
                for cache in (first, second)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10.0)
        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(failures, [])
        self.assertIsNotNone(first.load(self.configuration))
        self.assertEqual(list(self.cache_root.glob(f".tmp-{key}-*")), [])

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

    def test_publish_failure_never_creates_a_complete_entry(self):
        cache = PreprocessCache(self.cache_root)
        with mock.patch("gpu_capability_cache._write_payload", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(Exception, "disk full"):
                cache.publish(self.view())
        self.assertIsNone(cache.load(self.configuration))

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
        with _PublicationGuard(active / "active.lock"):
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
        self.assertIsNone(cache.load(self.configuration))


if __name__ == "__main__":
    unittest.main()
