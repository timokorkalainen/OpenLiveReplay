from __future__ import annotations

import ast
import dataclasses
import hashlib
import json
import multiprocessing
import os
import subprocess
import struct
import sys
import tempfile
import threading
import time
import unittest
import weakref
from array import array
from pathlib import Path, PurePosixPath
from types import MappingProxyType, SimpleNamespace
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_capability_command as capability_command  # noqa: E402
import gpu_capability_model as capability_model  # noqa: E402
import gpu_capability_runner as capability_runner  # noqa: E402
import gpu_capability_source_audit as capability_audit  # noqa: E402
from gpu_capability_command import (  # noqa: E402
    RewrittenCommand,
    _clear_compiler_inspection_memo_for_tests,
    _environment_digest,
    open_compiler_executable_capability,
)
from gpu_capability_cache import ConfigurationAuditLoadBatch, PreprocessCache  # noqa: E402
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
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
from gpu_capability_runner import (  # noqa: E402
    ExecutionResult,
    _WindowsJob,
    collect_configurations,
    discover_configuration,
    load_or_preprocess,
    preprocess_all,
    preprocess_configuration,
    run_bounded_preprocessor,
    stabilize_and_parse_configuration,
)


class BoundedPreprocessorTests(unittest.TestCase):
    def test_runner_module_has_no_optimization_sensitive_assertions(self):
        path = Path(__file__).resolve().with_name("gpu_capability_runner.py")
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=path.name)
        self.assertFalse(any(isinstance(node, ast.Assert) for node in ast.walk(tree)))

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
    ):
        if task is None:
            task, _reservation = self._task()
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
            outcome = capability_runner.audit_configuration_worker(
                task,
                self.authority,
                self.production_snapshot,
                control,
                command,
                time.monotonic() + 30.0,
            )
        return outcome, control, command, cache

    def test_worker_returns_only_compact_outcome_drops_view_and_never_loads(self):
        task, reservation = self._task()
        outcome, control, _command, cache = self._run_worker(task=task)
        self.assertIsInstance(outcome, capability_model.ConfigurationAuditOutcome)
        self.assertFalse(hasattr(outcome, "view"))
        self.assertEqual(len(self.live_views), 0)
        self.assertEqual(cache.loads, 0)
        self.assertEqual(outcome.result.reached_production, (
            PurePosixPath("playback/gpu/empty.h"),
            PurePosixPath("playback/gpu/worker.cpp"),
        ))
        self.assertEqual(outcome.stdout_bytes, 34)
        self.assertTrue(control.sealed)
        self.assertTrue(reservation.released)

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
        initial_bytes = self.source.read_bytes()
        self.source.write_bytes(b"int generation_b;\n")
        generation_b = self._dependency(self._identity(
            self.source, "playback/gpu/worker.cpp", line_count=1
        ))
        self.source.write_bytes(initial_bytes)
        mixed = tuple(sorted(
            (generation_b, self.dependencies[0]),
            key=lambda item: item.role_relative_path.as_posix(),
        ))
        cache = self._Cache()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "production snapshot generation"
        ):
            self._run_worker(cache=cache, dependencies=mixed)
        self.assertEqual(cache.published, [])
        self.assertEqual(self.source.read_bytes(), initial_bytes)

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
        source = Path(capability_runner.__file__).read_text(encoding="utf-8")
        self.assertEqual(source.count("subprocess." + "Popen("), 1)
        table = capability_runner._ProcessHandleAssociationTable()
        identity = capability_model.ProcessStartIdentity(
            "windows", 42, "creation-time", "authenticated-cookie"
        )
        process = object()
        with self.assertRaisesRegex(
            AuditInfrastructureError, "preceded handle registration"
        ):
            table.require(identity)
        table.register(identity, process)
        self.assertIs(table.require(identity), process)
        self.assertEqual(table.active_count, 1)
        with self.assertRaisesRegex(AuditInfrastructureError, "cookie differs"):
            table.release(identity, object())
        table.release(identity, process)
        self.assertEqual(table.active_count, 0)
        with self.assertRaisesRegex(AuditInfrastructureError, "already closed"):
            table.release(identity, process)

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

        releases = []
        task, reservation = self._task()
        command = self._CommandEndpoint(self._permit(release_callback=lambda: releases.append("root")))
        cache = self._Cache(AuditInfrastructureError("cache publish failed"))
        with self.assertRaisesRegex(AuditInfrastructureError, "cache publish failed"):
            self._run_worker(task=task, command=command, cache=cache)
        self.assertEqual(releases, ["root"])
        self.assertTrue(reservation.released)


if __name__ == "__main__":
    unittest.main()
