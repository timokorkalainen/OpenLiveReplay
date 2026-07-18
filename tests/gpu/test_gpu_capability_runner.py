from __future__ import annotations

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
from array import array
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_capability_command as capability_command  # noqa: E402
import gpu_capability_model as capability_model  # noqa: E402
import gpu_capability_runner as capability_runner  # noqa: E402
from gpu_capability_command import (  # noqa: E402
    RewrittenCommand,
    _clear_compiler_inspection_memo_for_tests,
    _environment_digest,
    open_compiler_executable_capability,
)
from gpu_capability_cache import PreprocessCache  # noqa: E402
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompactTokenSequence,
    CompilerFamily,
    DependencyDigest,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
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

    def stabilize_fixture(self, mode: str, *extra: str):
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
        if os.name == "nt":
            self.assertEqual(len(owner.streams), 15)
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
                    AuditLimits(), invocation_seconds=0.25, rss_bytes=2**63 - 1
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
                AuditLimits(), invocation_seconds=0.5, rss_bytes=2**63 - 1
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
        with mock.patch(
            "gpu_capability_runner.stabilize_and_parse_configuration",
            return_value=(
                expected,
                SimpleNamespace(dependency_identities=expected.dependencies),
                None,
            ),
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

        def preprocess(*_arguments):
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
        barrier = threading.Barrier(2)
        real_rename = os.rename
        successes: list[PreprocessedTranslationUnitView] = []
        failures: list[BaseException] = []

        def rename(source, destination) -> None:
            if Path(source).name.startswith(".tmp-") and Path(destination) == entry:
                barrier.wait(timeout=5.0)
            real_rename(source, destination)

        def preprocess(*_arguments):
            view = views[threading.current_thread().name]
            return view, SimpleNamespace(dependency_identities=view.dependencies), None

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
            threads = [
                threading.Thread(target=run, args=(cache,), name=name)
                for cache, name in zip(caches, ("first", "second"))
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10.0)
        self.assertFalse(any(thread.is_alive() for thread in threads))
        self.assertEqual(len(successes), 1)
        self.assertEqual(len(failures), 1)
        self.assertRegex(str(failures[0]), "concurrent cache winner differs")


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
            observer._owner = set()
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
                self.root, databases, self.environment, self.dependency_roots
            )

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
            )
        execute.assert_not_called()

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
            )

        diagnostic = str(raised.exception)
        self.assertLess(diagnostic.index("digest=a"), diagnostic.index("digest=z"))
        self.assertIn("failure-a", diagnostic)
        self.assertIn("failure-z", diagnostic)


if __name__ == "__main__":
    unittest.main()
