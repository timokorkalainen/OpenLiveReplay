from __future__ import annotations

import dataclasses
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from array import array
from pathlib import Path, PurePosixPath
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_command import RewrittenCommand, _environment_digest  # noqa: E402
from gpu_capability_cache import PreprocessCache  # noqa: E402
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompactTokenSequence,
    CompilerFamily,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
)
from gpu_capability_runner import (  # noqa: E402
    ExecutionResult,
    _WindowsJob,
    load_or_preprocess,
    preprocess_configuration,
    run_bounded_preprocessor,
)


class BoundedPreprocessorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "playback" / "a.cpp"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("lease.nativeHandle();\n", encoding="utf-8")
        self.outside = self.root / "sdk" / "outside.h"
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
        self.fixture = Path(__file__).parent / "fixtures" / "fake_preprocessor.py"

    def tearDown(self) -> None:
        self.temporary.cleanup()

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
    ) -> tuple[ExecutionResult, bytes]:
        chunks: list[bytes] = []
        result = run_bounded_preprocessor(
            self.direct_command(mode, *extra),
            self.configuration(mode),
            limits or AuditLimits(rss_bytes=2**63 - 1),
            deadline if deadline is not None else time.monotonic() + 10.0,
            chunks.append,
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
            "gpu_capability_runner.preprocess_configuration", return_value=expected
        ) as preprocess:
            self.assertIs(
                load_or_preprocess(
                    configuration,
                    self.production,
                    cache,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                ),
                expected,
            )
        self.assertEqual(preprocess.call_count, 2)
        self.assertIsNotNone(cache.load(configuration))

        failed_configuration = dataclasses.replace(configuration, digest="failed")
        with mock.patch(
            "gpu_capability_runner.preprocess_configuration",
            side_effect=AuditInfrastructureError("compiler failed"),
        ):
            with self.assertRaisesRegex(AuditInfrastructureError, "compiler failed"):
                load_or_preprocess(
                    failed_configuration,
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
            if calls == 2:
                self.source.write_text("changed after compiler read\n", encoding="utf-8")
            return old_view

        with mock.patch(
            "gpu_capability_runner.preprocess_configuration", side_effect=preprocess
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "changed during preprocessing"
            ):
                load_or_preprocess(
                    configuration,
                    self.production,
                    cache,
                    AuditLimits(rss_bytes=2**63 - 1),
                    time.monotonic() + 10.0,
                )
        self.assertEqual(calls, 2)
        self.assertIsNone(cache.load(configuration))

    def test_load_or_preprocess_rejects_transient_output_even_when_content_is_restored(self):
        configuration = self.configuration("success")
        cache = PreprocessCache(self.root / "cache")
        discovery = self.preprocess_fixture("success")
        accepted = self.altered_view(discovery)
        original = self.source.read_bytes()
        calls = 0

        def preprocess(*_arguments):
            nonlocal calls
            if calls == 0:
                calls += 1
                return discovery
            self.source.write_text("transient B\n", encoding="utf-8")
            self.source.write_bytes(original)
            return accepted

        with mock.patch(
            "gpu_capability_runner.preprocess_configuration", side_effect=preprocess
        ), mock.patch.object(
            CompactTokenSequence,
            "__iter__",
            side_effect=AssertionError("token iteration"),
        ):
            with self.assertRaisesRegex(
                AuditInfrastructureError, "nondeterministic preprocessed output"
            ):
                load_or_preprocess(
                    configuration,
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
            return views[threading.current_thread().name]

        def run(cache: PreprocessCache) -> None:
            try:
                successes.append(
                    load_or_preprocess(
                        configuration,
                        self.production,
                        cache,
                        AuditLimits(rss_bytes=2**63 - 1),
                        time.monotonic() + 10.0,
                    )
                )
            except BaseException as error:
                failures.append(error)

        with mock.patch(
            "gpu_capability_runner.preprocess_configuration", side_effect=preprocess
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


if __name__ == "__main__":
    unittest.main()
