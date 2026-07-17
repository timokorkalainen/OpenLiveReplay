"""Bounded subprocess execution for compiler-authoritative GPU audit views."""

from __future__ import annotations

import os
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from gpu_capability_cache import PreprocessCache
from gpu_capability_command import (
    RewrittenCommand,
    _environment_digest,
    rewrite_preprocess_command,
)
from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CompilerFamily,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    _current_process_rss_bytes,
    _preprocessed_view_semantic_digest,
)
from gpu_capability_provenance import (
    PreprocessedStreamBuilder,
    parse_gcc_dependencies,
    parse_msvc_dependencies,
    validate_dependency_identities,
)


_IO_CHUNK_BYTES = 4096
_POLL_SECONDS = 0.01
_REAP_SECONDS = 1.0


@dataclass(frozen=True)
class ExecutionResult:
    stderr_tail: bytes
    elapsed_seconds: float
    observed_stdout_bytes: int


def _validate_execution_inputs(
    command: RewrittenCommand,
    configuration: PreprocessConfiguration,
    limits: AuditLimits,
    deadline: float,
    consume_stdout: Callable[[bytes], None],
) -> None:
    if not isinstance(command, RewrittenCommand) or not command.arguments:
        raise AuditInfrastructureError("rewritten preprocess command is invalid")
    if not isinstance(configuration, PreprocessConfiguration):
        raise AuditInfrastructureError("preprocess configuration is invalid")
    if not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("audit limits are invalid")
    if not isinstance(deadline, (int, float)) or isinstance(deadline, bool):
        raise AuditInfrastructureError("global deadline is invalid")
    if not callable(consume_stdout):
        raise AuditInfrastructureError("stdout consumer is invalid")
    numeric_limits = {
        "invocation timeout": limits.invocation_seconds,
        "stdout limit": limits.stdout_bytes,
        "stderr limit": limits.stderr_bytes,
        "RSS limit": limits.rss_bytes,
    }
    for label, value in numeric_limits.items():
        if (
            not isinstance(value, (int, float))
            or isinstance(value, bool)
            or value < 0
        ):
            raise AuditInfrastructureError(f"{label} is invalid")
    if limits.invocation_seconds <= 0:
        raise AuditInfrastructureError("invocation timeout is invalid")


def _stderr_tail_text(stderr_tail: bytes) -> str:
    return repr(stderr_tail)


def _diagnostic(
    configuration: PreprocessConfiguration,
    reason: str,
    *,
    exit_status: int | str,
    elapsed_seconds: float,
    observed_stdout_bytes: int,
    stderr_tail: bytes,
) -> AuditInfrastructureError:
    return AuditInfrastructureError(
        f"GPU capability preprocessing failed: digest={configuration.digest} "
        f"family={configuration.family.value} source={configuration.source.canonical} "
        f"exit={exit_status} elapsed={elapsed_seconds:.3f}s "
        f"stdout_bytes={observed_stdout_bytes} "
        f"stderr_tail={_stderr_tail_text(stderr_tail)} reason={reason}"
    )


class _ProcessContainment:
    """Own one compiler process tree until completion or forced teardown."""

    def __init__(self) -> None:
        self._pid: int | None = None
        self._job = _WindowsJob() if os.name == "nt" else None

    @property
    def popen_arguments(self) -> dict[str, object]:
        if os.name == "nt":
            return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        return {"start_new_session": True}

    @property
    def requires_handshake(self) -> bool:
        return os.name == "nt"

    def prepare_command(self, command: tuple[str, ...]) -> list[str]:
        if os.name != "nt":
            return list(command)
        helper = (
            "import subprocess,sys;"
            "sys.stdin.buffer.read(1);"
            "raise SystemExit(subprocess.call(sys.argv[1:]))"
        )
        return [sys.executable, "-I", "-S", "-c", helper, *command]

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        self._pid = process.pid
        if self._job is not None:
            self._job.attach(process)

    def release(self, process: subprocess.Popen[bytes]) -> None:
        if not self.requires_handshake:
            return
        if process.stdin is None:
            raise AuditInfrastructureError("compiler process handshake is unavailable")
        process.stdin.write(b"1")
        process.stdin.close()

    def terminate(self) -> None:
        if self._job is not None:
            self._job.terminate()
            return
        if self._pid is not None:
            try:
                os.killpg(self._pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def close(self) -> None:
        if self._job is not None:
            self._job.close()
        else:
            self.terminate()


class _WindowsJob:
    """Kill-on-close Windows Job Object with race-free child release."""

    def __init__(self) -> None:
        if os.name != "nt":
            self._handle = None
            return
        import ctypes
        from ctypes import wintypes

        class _IoCounters(ctypes.Structure):
            _fields_ = tuple(
                (name, ctypes.c_ulonglong)
                for name in (
                    "read_operations",
                    "write_operations",
                    "other_operations",
                    "read_bytes",
                    "write_bytes",
                    "other_bytes",
                )
            )

        class _BasicLimits(ctypes.Structure):
            _fields_ = (
                ("per_process_user_time", ctypes.c_longlong),
                ("per_job_user_time", ctypes.c_longlong),
                ("limit_flags", wintypes.DWORD),
                ("minimum_working_set", ctypes.c_size_t),
                ("maximum_working_set", ctypes.c_size_t),
                ("active_process_limit", wintypes.DWORD),
                ("affinity", ctypes.c_size_t),
                ("priority_class", wintypes.DWORD),
                ("scheduling_class", wintypes.DWORD),
            )

        class _ExtendedLimits(ctypes.Structure):
            _fields_ = (
                ("basic", _BasicLimits),
                ("io", _IoCounters),
                ("process_memory", ctypes.c_size_t),
                ("job_memory", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t),
                ("peak_job_memory", ctypes.c_size_t),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = (
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        )
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
        kernel32.TerminateJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.CreateJobObjectW(None, None)
        if not handle:
            raise AuditInfrastructureError("cannot create compiler process job object")
        limits = _ExtendedLimits()
        limits.basic.limit_flags = 0x00002000
        if not kernel32.SetInformationJobObject(
            handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            kernel32.CloseHandle(handle)
            raise AuditInfrastructureError("cannot configure compiler process job object")
        self._handle = handle
        self._kernel32 = kernel32

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        if self._handle is None:
            return
        raw_handle = getattr(process, "_handle", None)
        if raw_handle is None or not self._kernel32.AssignProcessToJobObject(
            self._handle, raw_handle
        ):
            raise AuditInfrastructureError("cannot contain compiler process tree")

    def terminate(self) -> None:
        if self._handle is not None:
            handle = self._handle
            if self._kernel32.TerminateJobObject(handle, 1):
                return
            closed = bool(self._kernel32.CloseHandle(handle))
            if closed:
                self._handle = None
            fallback = "succeeded" if closed else "failed"
            raise AuditInfrastructureError(
                "cannot terminate compiler process job object; "
                f"kill-on-close fallback {fallback}"
            )

    def close(self) -> None:
        if self._handle is not None:
            handle = self._handle
            if self._kernel32.CloseHandle(handle):
                self._handle = None
                return
            terminated = bool(self._kernel32.TerminateJobObject(handle, 1))
            closed = bool(self._kernel32.CloseHandle(handle))
            if closed:
                self._handle = None
            raise AuditInfrastructureError(
                "cannot close compiler process job object; "
                f"termination fallback {'succeeded' if terminated else 'failed'}, "
                f"close retry {'succeeded' if closed else 'failed'}"
            )


def _terminate_and_reap(
    process: subprocess.Popen[bytes], containment: _ProcessContainment
) -> None:
    containment_error: AuditInfrastructureError | None = None
    try:
        containment.terminate()
    except AuditInfrastructureError as error:
        containment_error = error
    try:
        process.wait(timeout=_REAP_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=_REAP_SECONDS)
    if containment_error is not None:
        raise containment_error


def run_bounded_preprocessor(
    command: RewrittenCommand,
    configuration: PreprocessConfiguration,
    limits: AuditLimits,
    deadline: float,
    consume_stdout: Callable[[bytes], None],
) -> ExecutionResult:
    """Execute one rewritten command without retaining its stdout stream."""

    _validate_execution_inputs(command, configuration, limits, deadline, consume_stdout)
    started = time.monotonic()
    if deadline <= started:
        raise _diagnostic(
            configuration,
            "global deadline exceeded before launch",
            exit_status="not-started",
            elapsed_seconds=0.0,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    try:
        environment = dict(os.environ)
        actual_environment_digest = _environment_digest(environment)
    except (AuditInfrastructureError, OSError, RuntimeError, ValueError) as error:
        raise _diagnostic(
            configuration,
            f"cannot snapshot compiler environment: {error}",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        ) from error
    if actual_environment_digest != configuration.environment_digest:
        raise _diagnostic(
            configuration,
            "compiler environment digest mismatch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    invocation_deadline = started + limits.invocation_seconds
    effective_deadline = min(invocation_deadline, deadline)
    deadline_reason = (
        "global deadline exceeded" if deadline <= invocation_deadline else "timeout"
    )
    if _current_process_rss_bytes() > limits.rss_bytes:
        raise _diagnostic(
            configuration,
            "coordinator RSS limit exceeded before launch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    containment = _ProcessContainment()
    process: subprocess.Popen[bytes] | None = None
    stdout_events: queue.Queue[bytes | BaseException | None] = queue.Queue(maxsize=2)
    stop_readers = threading.Event()
    stderr_tail = bytearray()
    stderr_error: list[BaseException] = []
    stderr_lock = threading.Lock()

    def read_stdout() -> None:
        assert process is not None and process.stdout is not None
        try:
            while not stop_readers.is_set():
                chunk = process.stdout.read(_IO_CHUNK_BYTES)
                if not chunk:
                    break
                while not stop_readers.is_set():
                    try:
                        stdout_events.put(chunk, timeout=_POLL_SECONDS)
                        break
                    except queue.Full:
                        continue
        except BaseException as error:
            while not stop_readers.is_set():
                try:
                    stdout_events.put(error, timeout=_POLL_SECONDS)
                    break
                except queue.Full:
                    continue
        finally:
            while not stop_readers.is_set():
                try:
                    stdout_events.put(None, timeout=_POLL_SECONDS)
                    break
                except queue.Full:
                    continue

    def read_stderr() -> None:
        assert process is not None and process.stderr is not None
        try:
            while True:
                chunk = process.stderr.read(_IO_CHUNK_BYTES)
                if not chunk:
                    break
                with stderr_lock:
                    if limits.stderr_bytes:
                        stderr_tail.extend(chunk)
                        excess = len(stderr_tail) - limits.stderr_bytes
                        if excess > 0:
                            del stderr_tail[:excess]
        except BaseException as error:
            stderr_error.append(error)

    stdout_thread: threading.Thread | None = None
    stderr_thread: threading.Thread | None = None
    observed_stdout_bytes = 0
    failure: tuple[str, int | str, float] | None = None
    try:
        try:
            process = subprocess.Popen(
                containment.prepare_command(command.arguments),
                cwd=str(configuration.working_directory),
                env=environment,
                shell=False,
                stdin=(subprocess.PIPE if containment.requires_handshake else subprocess.DEVNULL),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                **containment.popen_arguments,
            )
            containment.attach(process)
            containment.release(process)
        except (OSError, AuditInfrastructureError) as error:
            if process is not None:
                _terminate_and_reap(process, containment)
            raise _diagnostic(
                configuration,
                str(error),
                exit_status="launch-failed",
                elapsed_seconds=time.monotonic() - started,
                observed_stdout_bytes=0,
                stderr_tail=b"",
            ) from error

        stdout_thread = threading.Thread(target=read_stdout, name="gpu-audit-stdout", daemon=True)
        stderr_thread = threading.Thread(target=read_stderr, name="gpu-audit-stderr", daemon=True)
        stdout_thread.start()
        stderr_thread.start()
        stdout_complete = False
        tree_terminated = False
        while not stdout_complete:
            now = time.monotonic()
            if now >= effective_deadline:
                failure = (
                    deadline_reason,
                    "terminated",
                    now - started,
                )
                break
            rss = _current_process_rss_bytes()
            if rss > limits.rss_bytes:
                failure = (
                    "coordinator RSS limit exceeded",
                    "terminated",
                    now - started,
                )
                break
            if process.poll() is not None and not tree_terminated:
                try:
                    containment.terminate()
                except AuditInfrastructureError as error:
                    failure = (
                        str(error),
                        process.returncode,
                        time.monotonic() - started,
                    )
                    tree_terminated = True
                    break
                tree_terminated = True
            try:
                event = stdout_events.get(timeout=min(_POLL_SECONDS, effective_deadline - now))
            except queue.Empty:
                continue
            if event is None:
                stdout_complete = True
                continue
            if isinstance(event, BaseException):
                failure = (
                    f"cannot read compiler stdout: {event}",
                    "terminated",
                    time.monotonic() - started,
                )
                break
            observed_stdout_bytes += len(event)
            if observed_stdout_bytes > limits.stdout_bytes:
                failure = (
                    "stdout output limit exceeded",
                    "terminated",
                    time.monotonic() - started,
                )
                break
            try:
                consume_stdout(event)
            except Exception as error:
                failure = (
                    str(error),
                    "terminated",
                    time.monotonic() - started,
                )
                break

        while failure is None and process.poll() is None:
            now = time.monotonic()
            if now >= effective_deadline:
                failure = (
                    deadline_reason,
                    "terminated",
                    now - started,
                )
                break
            if _current_process_rss_bytes() > limits.rss_bytes:
                failure = (
                    "coordinator RSS limit exceeded",
                    "terminated",
                    now - started,
                )
                break
            time.sleep(min(_POLL_SECONDS, effective_deadline - now))

        if failure is not None:
            reason, exit_status, failure_elapsed = failure
            actual_returncode = process.poll()
            if actual_returncode is not None:
                exit_status = actual_returncode
            try:
                if not tree_terminated:
                    _terminate_and_reap(process, containment)
                    tree_terminated = True
            except AuditInfrastructureError as cleanup_error:
                reason = f"{reason}; containment cleanup failed: {cleanup_error}"
            failure = (reason, exit_status, failure_elapsed)
        elif not tree_terminated:
            try:
                # A compiler must not leave a descendant holding either output pipe.
                containment.terminate()
            except AuditInfrastructureError as error:
                failure = (
                    str(error),
                    process.returncode,
                    time.monotonic() - started,
                )
            tree_terminated = True
        stop_readers.set()
        for thread in (stdout_thread, stderr_thread):
            if thread is not None:
                thread.join(timeout=_REAP_SECONDS)
        if (
            failure is None
            and any(
                thread is not None and thread.is_alive()
                for thread in (stdout_thread, stderr_thread)
            )
        ):
            failure = (
                "compiler output reader did not terminate",
                "terminated",
                time.monotonic() - started,
            )
        if stderr_error and failure is None:
            failure = (
                f"cannot read compiler stderr: {stderr_error[0]}",
                process.returncode if process.returncode is not None else "terminated",
                time.monotonic() - started,
            )
        if failure is not None:
            reason, exit_status, elapsed_seconds = failure
            with stderr_lock:
                complete_stderr_tail = bytes(stderr_tail)
            raise _diagnostic(
                configuration,
                reason,
                exit_status=exit_status,
                elapsed_seconds=elapsed_seconds,
                observed_stdout_bytes=observed_stdout_bytes,
                stderr_tail=complete_stderr_tail,
            )
        if process.returncode != 0:
            with stderr_lock:
                complete_stderr_tail = bytes(stderr_tail)
            raise _diagnostic(
                configuration,
                "compiler returned a nonzero status",
                exit_status=process.returncode,
                elapsed_seconds=time.monotonic() - started,
                observed_stdout_bytes=observed_stdout_bytes,
                stderr_tail=complete_stderr_tail,
            )
        with stderr_lock:
            complete_stderr_tail = bytes(stderr_tail)
        return ExecutionResult(
            stderr_tail=complete_stderr_tail,
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=observed_stdout_bytes,
        )
    finally:
        active_error = sys.exc_info()[1]
        stop_readers.set()
        if process is not None and process.poll() is None:
            try:
                _terminate_and_reap(process, containment)
            except AuditInfrastructureError as error:
                if active_error is not None:
                    active_error.add_note(f"containment cleanup also failed: {error}")
        if stdout_thread is not None:
            stdout_thread.join(timeout=_REAP_SECONDS)
        if stderr_thread is not None:
            stderr_thread.join(timeout=_REAP_SECONDS)
        if process is not None:
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
        try:
            containment.close()
        except AuditInfrastructureError as error:
            if active_error is not None:
                active_error.add_note(f"containment close also failed: {error}")
            else:
                with stderr_lock:
                    cleanup_stderr_tail = bytes(stderr_tail)
                raise _diagnostic(
                    configuration,
                    str(error),
                    exit_status=(
                        process.returncode
                        if process is not None and process.returncode is not None
                        else "terminated"
                    ),
                    elapsed_seconds=time.monotonic() - started,
                    observed_stdout_bytes=observed_stdout_bytes,
                    stderr_tail=cleanup_stderr_tail,
                ) from error


def _source_root(
    production: Mapping[PurePosixPath, FileIdentity],
) -> Path:
    roots: set[Path] = set()
    for relative, identity in production.items():
        if relative != identity.relative or not identity.production:
            raise AuditInfrastructureError("production identity table is inconsistent")
        root = identity.canonical
        for _part in relative.parts:
            root = root.parent
        roots.add(root)
    if len(roots) != 1:
        raise AuditInfrastructureError("production identities do not share one source root")
    return next(iter(roots))


def _absolute_dependencies(
    paths: tuple[Path, ...], working_directory: Path
) -> tuple[Path, ...]:
    return tuple(
        path if path.is_absolute() else (working_directory / path).resolve(strict=False)
        for path in paths
    )


def preprocess_configuration(
    configuration: PreprocessConfiguration,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
) -> PreprocessedTranslationUnitView:
    """Preprocess, validate dependencies, then finalize one complete packed view."""

    started = time.monotonic()
    execution: ExecutionResult | None = None
    try:
        source_root = _source_root(production)
        builder = PreprocessedStreamBuilder(
            configuration,
            production,
            limits,
            _current_process_rss_bytes,
        )
        with tempfile.TemporaryDirectory(
            prefix=".gpu-capability-dependencies-",
            dir=str(configuration.working_directory),
        ) as temporary:
            suffix = ".json" if configuration.family in {
                CompilerFamily.MSVC,
                CompilerFamily.CLANG_CL,
            } else ".d"
            dependency_output = Path(temporary) / f"dependencies{suffix}"
            rewritten = rewrite_preprocess_command(configuration, dependency_output)
            execution = run_bounded_preprocessor(
                rewritten,
                configuration,
                limits,
                deadline,
                builder.feed,
            )
            if rewritten.dependency_format == "gcc-depfile":
                dependency_paths = parse_gcc_dependencies(dependency_output)
            elif rewritten.dependency_format == "msvc-json":
                dependency_paths = parse_msvc_dependencies(dependency_output)
            else:
                raise AuditInfrastructureError(
                    f"unsupported dependency format: {rewritten.dependency_format}"
                )
            dependencies = validate_dependency_identities(
                _absolute_dependencies(
                    dependency_paths, configuration.working_directory
                ),
                source_root,
                production,
            )
            return builder.finalize(dependencies)
    except (AuditInfrastructureError, OSError) as error:
        if str(error).startswith("GPU capability preprocessing failed:"):
            raise
        raise _diagnostic(
            configuration,
            str(error),
            exit_status=(0 if execution is not None else "not-started"),
            elapsed_seconds=(
                execution.elapsed_seconds
                if execution is not None
                else time.monotonic() - started
            ),
            observed_stdout_bytes=(
                execution.observed_stdout_bytes if execution is not None else 0
            ),
            stderr_tail=(execution.stderr_tail if execution is not None else b""),
        ) from error


def load_or_preprocess(
    configuration: PreprocessConfiguration,
    production: Mapping[PurePosixPath, FileIdentity],
    cache: PreprocessCache,
    limits: AuditLimits,
    deadline: float,
) -> PreprocessedTranslationUnitView:
    """Return a validated cache hit or publish one complete compiler result."""

    if not isinstance(cache, PreprocessCache):
        raise AuditInfrastructureError("preprocess cache is invalid")
    cached = cache.load(configuration)
    if cached is not None:
        return cached
    discovery = preprocess_configuration(
        configuration,
        production,
        limits,
        deadline,
    )
    if discovery.configuration != configuration:
        raise AuditInfrastructureError(
            "preprocessor returned a mismatched discovery configuration"
        )
    discovery_semantic_digest = _preprocessed_view_semantic_digest(discovery)
    try:
        snapshots = cache._snapshot_dependencies(discovery.dependencies, force=True)
    except OSError as error:
        raise AuditInfrastructureError(
            "dependency changed during preprocessing"
        ) from error
    del discovery
    accepted = preprocess_configuration(
        configuration,
        production,
        limits,
        deadline,
    )
    if accepted.configuration != configuration:
        raise AuditInfrastructureError(
            "preprocessor returned a mismatched accepted configuration"
        )
    if _preprocessed_view_semantic_digest(accepted) != discovery_semantic_digest:
        # Volatile macros are deliberately fail-closed: reproducible compiler
        # output is required before it can be bound to a content snapshot.
        raise AuditInfrastructureError("nondeterministic preprocessed output")
    return cache._publish_stabilized(accepted, snapshots)
