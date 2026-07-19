"""Bounded subprocess execution for compiler-authoritative GPU audit views."""

from __future__ import annotations

import concurrent.futures
import contextlib
import hashlib
import json
import os
import queue
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from types import MappingProxyType

from gpu_capability_cache import (
    PreprocessCache,
    _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES,
    decode_configuration_audit_result_transport,
    encode_configuration_audit_result_transport,
)
from gpu_capability_command import (
    CompilerProcessHandleCarrier,
    RewrittenCommand,
    _environment_digest,
    _resume_suspended_windows_process,
    decode_compile_entry,
    make_configuration,
    launch_compiler_process,
    rewrite_preprocess_command,
    validate_compiler_executable_capability,
)
from gpu_capability_model import (
    AUDIT_RESULT_SCHEMA_BYTES,
    AuditResultFinding,
    AuditInfrastructureError,
    AuditLimits,
    CachePublicationPermit,
    CachePublicationRequested,
    CompactResultTransportCapability,
    CompactResultDraftBounds,
    CompilerFamily,
    CompilerLaunchEvent,
    CompilerLaunchPurpose,
    ConfigurationAuditOutcome,
    ConfigurationAuditResult,
    ConfigurationAuditTask,
    ConfigurationAuditTransportOutcome,
    CoverageReport,
    DependencyDigest,
    DependencyRootAuthority,
    FileIdentity,
    PerTaskCompactReservation,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    ProcessStartIdentity,
    WorkerStageTimings,
    _FilesystemGenerationObserver,
    _current_process_rss_bytes,
    _preprocessed_view_semantic_digest,
    enumerate_production_identities,
    requires_compile_entry,
    validate_dependency_root_authority,
)
from gpu_capability_provenance import (
    PreprocessedStreamBuilder,
    parse_gcc_dependencies,
    parse_msvc_dependencies,
    validate_dependency_identities,
)


_IO_CHUNK_BYTES = 256 * 1024
_POLL_SECONDS = 0.01
_REAP_SECONDS = 1.0
_DEPENDENCY_FILE_BYTES = 256 * 1024 * 1024
_DEPENDENCY_TOTAL_BYTES = 1024 * 1024 * 1024

_WORKER_INDEX: int | None = None
_WORKER_GENERATION: int | None = None
_WORKER_PRODUCTION: Mapping[PurePosixPath, FileIdentity] | None = None
_WORKER_LIMITS: AuditLimits | None = None
_WORKER_CANCEL_EVENT: object | None = None
_WORKER_ENGINE: str | None = None
_WORKER_CACHE: object | None = None


class _WorkerRssSampler:
    def sample(self) -> int:
        return _current_process_rss_bytes()


_WORKER_RSS: object = _WorkerRssSampler()


def _check_dependency_budget(
    deadline: float | None, cancel_event: object | None
) -> None:
    if cancel_event is not None:
        is_set = getattr(cancel_event, "is_set", None)
        if not callable(is_set):
            raise AuditInfrastructureError("dependency cancellation event is invalid")
        if is_set():
            raise AuditInfrastructureError("dependency hashing cancelled")
    if deadline is not None and time.monotonic() >= deadline:
        raise AuditInfrastructureError("dependency hashing deadline exceeded")


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
            return {
                "creationflags": (
                    subprocess.CREATE_NEW_PROCESS_GROUP
                    | getattr(subprocess, "CREATE_SUSPENDED", 0x00000004)
                )
            }
        return {"start_new_session": True}

    @property
    def requires_handshake(self) -> bool:
        return os.name == "nt"

    def prepare_command(self, command: tuple[str, ...]) -> list[str]:
        return list(command)

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        self._pid = process.pid
        if self._job is not None:
            self._job.attach(process)

    def release(self, process: subprocess.Popen[bytes]) -> None:
        if not self.requires_handshake:
            return
        _resume_suspended_windows_process(process)
        if process.stdin is not None:
            process.stdin.close()

    def terminate(self) -> None:
        if self._job is not None:
            self._job.terminate()
            return
        pid = self._pid
        self._pid = None
        if pid is not None:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def close(self) -> None:
        if self._job is not None:
            self._job.close()
        else:
            self._pid = None


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


def _launch_compiler_process(
    prepared_arguments,
    *,
    configuration: PreprocessConfiguration,
    environment: Mapping[str, str],
    containment: _ProcessContainment,
    capability,
    launch_options: Mapping[str, object],
    launch_context: _AuditLaunchContext | None,
    launch_purpose: CompilerLaunchPurpose | None,
) -> tuple[subprocess.Popen[bytes], CompilerProcessHandleCarrier]:
    purpose = (
        launch_purpose
        if launch_purpose is not None
        else CompilerLaunchPurpose.INSPECTION
    )
    return launch_compiler_process(
        prepared_arguments,
        cwd=configuration.working_directory,
        environment=environment,
        containment=containment,
        platform_kind=capability.platform_kind,
        stdin=(
            subprocess.PIPE if containment.requires_handshake else subprocess.DEVNULL
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        launch_options=launch_options,
        purpose=purpose,
        launch_observer=launch_context,
        worker_index=(_WORKER_INDEX if launch_context is not None else None),
        task_id=(launch_context.task_id if launch_context is not None else None),
        generation=(
            launch_context.generation if launch_context is not None else None
        ),
    )


def run_bounded_preprocessor(
    command: RewrittenCommand,
    configuration: PreprocessConfiguration,
    limits: AuditLimits,
    deadline: float,
    consume_stdout: Callable[[bytes], None],
    cancel_event: object | None = None,
    *,
    launch_context: _AuditLaunchContext | None = None,
    launch_purpose: CompilerLaunchPurpose | None = None,
) -> ExecutionResult:
    """Execute one rewritten command without retaining its stdout stream."""

    _validate_execution_inputs(command, configuration, limits, deadline, consume_stdout)
    if (launch_context is None) != (launch_purpose is None):
        raise AuditInfrastructureError("compiler launch observation binding is invalid")
    started = time.monotonic()
    if cancel_event is not None and not callable(getattr(cancel_event, "is_set", None)):
        raise AuditInfrastructureError("preprocess cancellation event is invalid")
    if cancel_event is not None and cancel_event.is_set():
        raise _diagnostic(
            configuration,
            "coordinator cancelled before launch",
            exit_status="not-started",
            elapsed_seconds=0.0,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
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
    if cancel_event is not None and cancel_event.is_set():
        raise _diagnostic(
            configuration,
            "coordinator cancelled before launch",
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
    capability = configuration.compiler_capability
    if capability.capability_digest != configuration.compiler_capability_digest:
        raise _diagnostic(
            configuration,
            "compiler executable capability digest mismatch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    capability_owner = capability.native_owner
    validate_owner = getattr(capability_owner, "validate", None)
    if not callable(validate_owner):
        raise AuditInfrastructureError("compiler executable capability owner is invalid")
    validate_owner(
        content=False, deadline=effective_deadline,
        cancel_event=cancel_event,
    )
    if not command.arguments:
        raise AuditInfrastructureError("rewritten preprocess command is empty")
    try:
        requested_executable = Path(command.arguments[0]).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise AuditInfrastructureError("compiler executable launch path is unavailable") from error
    if os.path.normcase(str(requested_executable)) != os.path.normcase(
        str(capability.executable_identity.canonical)
    ):
        raise AuditInfrastructureError("compiler launch does not consume the held capability")
    containment = _ProcessContainment()
    process: subprocess.Popen[bytes] | None = None
    process_carrier: CompilerProcessHandleCarrier | None = None
    stdout_events: queue.Queue[bytes | BaseException | None] = queue.Queue(maxsize=2)
    stop_readers = threading.Event()
    stderr_tail = bytearray()
    stderr_error: list[BaseException] = []
    stderr_lock = threading.Lock()

    def read_stdout() -> None:
        if process is None or process.stdout is None:
            raise AuditInfrastructureError("preprocessor stdout reader is unavailable")
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
        if process is None or process.stderr is None:
            raise AuditInfrastructureError("preprocessor stderr reader is unavailable")
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
            launch_arguments = command.arguments
            launch_options: dict[str, object] = {}
            if capability.platform_kind == "linux":
                executable_fd = getattr(capability_owner, "executable_fd", None)
                if not isinstance(executable_fd, int):
                    raise AuditInfrastructureError("exact compiler executable fd is unavailable")
                launch_arguments = (
                    f"/proc/self/fd/{executable_fd}", *command.arguments[1:]
                )
                launch_options["pass_fds"] = (executable_fd,)
            prepared_arguments = containment.prepare_command(launch_arguments)
            now = time.monotonic()
            if cancel_event is not None and cancel_event.is_set():
                raise AuditInfrastructureError(
                    "coordinator cancelled before launch"
                )
            if now >= effective_deadline:
                raise AuditInfrastructureError(
                    f"{deadline_reason} before launch"
                )
            validate_owner(
                content=False, deadline=effective_deadline,
                cancel_event=cancel_event,
            )
            now = time.monotonic()
            if cancel_event is not None and cancel_event.is_set():
                raise AuditInfrastructureError(
                    "coordinator cancelled before launch"
                )
            if now >= effective_deadline:
                raise AuditInfrastructureError(
                    f"{deadline_reason} before launch"
                )
            process, process_carrier = _launch_compiler_process(
                prepared_arguments,
                configuration=configuration,
                environment=environment,
                containment=containment,
                capability=capability,
                launch_options=launch_options,
                launch_context=launch_context,
                launch_purpose=launch_purpose,
            )
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
            if cancel_event is not None and cancel_event.is_set():
                failure = (
                    "coordinator cancelled after another configuration failed",
                    "terminated",
                    now - started,
                )
                break
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
            if cancel_event is not None and cancel_event.is_set():
                failure = (
                    "coordinator cancelled after another configuration failed",
                    "terminated",
                    now - started,
                )
                break
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
        validate_owner(
            content=False, deadline=effective_deadline,
            cancel_event=cancel_event,
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
        finally:
            if process is not None and process_carrier is not None:
                try:
                    process_carrier.complete_after_exit()
                except AuditInfrastructureError as error:
                    if active_error is not None:
                        active_error.add_note(
                            f"process carrier completion also failed: {error}"
                        )
                    else:
                        raise


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


@dataclass(frozen=True, slots=True)
class StreamDigest:
    sha256: str
    byte_count: int


@dataclass(frozen=True, slots=True)
class PreprocessDiscovery:
    stream: StreamDigest
    dependencies: tuple[DependencyDigest, ...]
    dependency_identities: tuple[FileIdentity, ...]


@dataclass(frozen=True, slots=True)
class PreprocessStageTimings:
    discovery_seconds: float
    accepted_parse_seconds: float
    compiler_stdout_bytes: int = 0

    def __post_init__(self) -> None:
        if (
            any(
                not isinstance(value, (int, float))
                or isinstance(value, bool)
                or value < 0
                for value in (
                    self.discovery_seconds,
                    self.accepted_parse_seconds,
                )
            )
            or not isinstance(self.compiler_stdout_bytes, int)
            or isinstance(self.compiler_stdout_bytes, bool)
            or self.compiler_stdout_bytes < 0
        ):
            raise AuditInfrastructureError("preprocess stage telemetry is invalid")


class _StageTimer:
    __slots__ = ("_started", "elapsed_seconds")

    def __init__(self) -> None:
        self._started = 0.0
        self.elapsed_seconds = 0.0

    def __enter__(self) -> "_StageTimer":
        self._started = time.monotonic()
        return self

    def __exit__(self, _kind, _value, _traceback) -> None:
        self.elapsed_seconds = time.monotonic() - self._started


def _stage_timer() -> _StageTimer:
    return _StageTimer()


@dataclass(frozen=True, slots=True)
class _CompactAuditDraft:
    configuration_digest: str
    audit_engine_fingerprint: str
    dependencies: tuple[DependencyDigest, ...]
    reached_production: tuple[PurePosixPath, ...]
    findings: tuple[AuditResultFinding, ...]
    task_id: str
    generation: int

    def materialize_under(
        self,
        reservation: PerTaskCompactReservation,
        publication_permit: CachePublicationPermit,
    ) -> ConfigurationAuditResult:
        reservation.validate(self.task_id, self.generation)
        publication_permit.validate_for_task(
            self.task_id,
            self.generation,
            self.configuration_digest,
            self.audit_engine_fingerprint,
            self.dependencies,
        )
        ownership = reservation.begin_result_ownership(
            self.task_id, self.generation
        )
        try:
            return ConfigurationAuditResult(
                self.configuration_digest,
                self.audit_engine_fingerprint,
                self.dependencies,
                self.reached_production,
                self.findings,
                ownership,
            )
        except BaseException:
            ownership.release("worker-materialization-failure")
            raise


def _compact_findings(findings) -> tuple[AuditResultFinding, ...]:
    if not isinstance(findings, (tuple, list)):
        raise AuditInfrastructureError("audit findings collection is invalid")
    compact: list[AuditResultFinding] = []
    for finding in findings:
        path = getattr(finding, "path", None)
        line = getattr(finding, "line", None)
        expression = getattr(finding, "expression", None)
        reason = getattr(finding, "reason", None)
        if (
            not isinstance(path, PurePosixPath)
            or not isinstance(line, int)
            or isinstance(line, bool)
            or not isinstance(expression, str)
            or not isinstance(reason, str)
        ):
            raise AuditInfrastructureError("audit finding cannot be compacted")
        compact.append(AuditResultFinding(path, line, expression, reason))
    result = tuple(sorted(
        compact,
        key=lambda item: (
            item.path.as_posix(), item.line, item.expression, item.reason
        ),
    ))
    if len(set(
        (item.path.as_posix(), item.line, item.expression, item.reason)
        for item in result
    )) != len(result):
        raise AuditInfrastructureError("compact audit findings are duplicated")
    return result


def _json_ascii_string_bytes(value: str) -> int:
    if not isinstance(value, str):
        raise AuditInfrastructureError("canonical JSON string is invalid")
    total = 2
    for character in value:
        codepoint = ord(character)
        if character in {'"', "\\"} or character in "\b\f\n\r\t":
            total += 2
        elif codepoint < 0x20:
            total += 6
        elif codepoint <= 0x7f:
            total += 1
        elif codepoint <= 0xffff:
            total += 6
        else:
            total += 12
    return total


def _json_member_bytes(name: str, value_bytes: int) -> int:
    return _json_ascii_string_bytes(name) + 1 + value_bytes


def _canonical_audit_result_json_bytes(
    configuration_digest: str,
    audit_engine_fingerprint: str,
    dependencies: tuple[DependencyDigest, ...],
    raw_findings,
) -> int:
    dependency_array_bytes = 2
    for index, dependency in enumerate(dependencies):
        identity = dependency.identity
        values = (
            ("canonical", _json_ascii_string_bytes(str(identity.canonical))),
            ("device", len(str(identity.device))),
            ("inode", 4 if identity.inode is None else len(str(identity.inode))),
            ("line_count", len(str(identity.line_count))),
            ("production", 4 if identity.production else 5),
            (
                "relative",
                4 if identity.relative is None else _json_ascii_string_bytes(
                    identity.relative.as_posix()
                ),
            ),
            (
                "role_relative_path",
                _json_ascii_string_bytes(dependency.role_relative_path.as_posix()),
            ),
            ("sha256", _json_ascii_string_bytes(dependency.sha256)),
            ("stable_role", _json_ascii_string_bytes(dependency.stable_role)),
        )
        dependency_array_bytes += (1 if index else 0) + 2 + 8 + sum(
            _json_member_bytes(name, size) for name, size in values
        )

    reached_array_bytes = 2
    reached_index = 0
    for dependency in dependencies:
        identity = dependency.identity
        if identity.production and identity.relative is not None:
            reached_array_bytes += (
                (1 if reached_index else 0)
                + _json_ascii_string_bytes(identity.relative.as_posix())
            )
            reached_index += 1

    finding_array_bytes = 2
    for index, finding in enumerate(raw_findings):
        path = finding.path.as_posix()
        line = finding.line
        expression = finding.expression
        reason = finding.reason
        finding_array_bytes += (1 if index else 0) + 2 + 3 + sum((
            _json_member_bytes("expression", _json_ascii_string_bytes(expression)),
            _json_member_bytes("line", len(str(line))),
            _json_member_bytes("path", _json_ascii_string_bytes(path)),
            _json_member_bytes("reason", _json_ascii_string_bytes(reason)),
        ))

    top_values = (
        ("audit_engine_fingerprint", _json_ascii_string_bytes(audit_engine_fingerprint)),
        ("configuration_digest", _json_ascii_string_bytes(configuration_digest)),
        ("dependencies", dependency_array_bytes),
        ("findings", finding_array_bytes),
        ("reached_production", reached_array_bytes),
        ("schema", _json_ascii_string_bytes(AUDIT_RESULT_SCHEMA_BYTES.decode("ascii"))),
    )
    return 2 + 5 + sum(
        _json_member_bytes(name, size) for name, size in top_values
    )


def _bounded_compact_result_draft(
    configuration_digest: str,
    audit_engine_fingerprint: str,
    dependencies: tuple[DependencyDigest, ...],
    raw_findings,
    task_id: str,
    generation: int,
    reservation: PerTaskCompactReservation,
) -> _CompactAuditDraft:
    if (
        not isinstance(dependencies, tuple)
        or any(not isinstance(item, DependencyDigest) for item in dependencies)
        or not isinstance(raw_findings, (tuple, list))
    ):
        raise AuditInfrastructureError("compact result draft is invalid")
    reached_count = 0
    reached_path_bytes = 0
    for dependency in dependencies:
        identity = dependency.identity
        if identity.production and identity.relative is not None:
            reached_count += 1
            reached_path_bytes += len(
                identity.relative.as_posix().encode("utf-8")
            )
    finding_count = 0
    finding_path_bytes = 0
    expression_bytes = 0
    reason_bytes = 0
    for finding in raw_findings:
        path = getattr(finding, "path", None)
        line = getattr(finding, "line", None)
        expression = getattr(finding, "expression", None)
        reason = getattr(finding, "reason", None)
        if (
            not isinstance(path, PurePosixPath)
            or not isinstance(line, int)
            or isinstance(line, bool)
            or line <= 0
            or not isinstance(expression, str)
            or not isinstance(reason, str)
        ):
            raise AuditInfrastructureError("audit finding cannot be compacted")
        finding_count += 1
        finding_path_bytes += len(path.as_posix().encode("utf-8"))
        expression_bytes += len(expression.encode("utf-8"))
        reason_bytes += len(reason.encode("utf-8"))
    dependency_path_bytes = sum(
        len(item.role_relative_path.as_posix().encode("utf-8"))
        + len(str(item.identity.canonical).encode("utf-8"))
        + len(item.stable_role.encode("ascii"))
        for item in dependencies
    )
    bounds = CompactResultDraftBounds(
        len(dependencies),
        reached_count,
        finding_count,
        dependency_path_bytes + reached_path_bytes + finding_path_bytes,
        expression_bytes,
        reason_bytes,
    )
    encoded_bytes = _canonical_audit_result_json_bytes(
        configuration_digest,
        audit_engine_fingerprint,
        dependencies,
        raw_findings,
    )
    if encoded_bytes > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES:
        raise AuditInfrastructureError("encoded compact audit result limit exceeded")
    reservation.require_within_pre_dispatch_reservation(
        task_id, encoded_bytes, bounds, 4096
    )
    reservation.record_exact_canonical_json(task_id, encoded_bytes)
    reached_production = tuple(sorted(
        (
            dependency.identity.relative
            for dependency in dependencies
            if dependency.identity.production
            and dependency.identity.relative is not None
        ),
        key=PurePosixPath.as_posix,
    ))
    if len(set(reached_production)) != len(reached_production):
        raise AuditInfrastructureError("fresh production reachability is duplicated")
    findings = _compact_findings(raw_findings)
    return _CompactAuditDraft(
        configuration_digest,
        audit_engine_fingerprint,
        dependencies,
        reached_production,
        findings,
        task_id,
        generation,
    )


def validate_production_dependency_snapshots(
    dependencies: tuple[DependencyDigest, ...], production_snapshot
) -> None:
    if not isinstance(dependencies, tuple) or any(
        not isinstance(item, DependencyDigest) for item in dependencies
    ):
        raise AuditInfrastructureError("fresh production dependencies are invalid")
    if not hasattr(production_snapshot, "items"):
        raise AuditInfrastructureError("production snapshot generation is invalid")
    snapshot: dict[str, DependencyDigest] = {}
    for raw_path, dependency in production_snapshot.items():
        if not isinstance(dependency, DependencyDigest):
            raise AuditInfrastructureError("production snapshot generation is invalid")
        key = (
            raw_path.as_posix()
            if isinstance(raw_path, PurePosixPath)
            else str(raw_path).replace("\\", "/")
        )
        if key in snapshot:
            raise AuditInfrastructureError("production snapshot generation is ambiguous")
        snapshot[key] = dependency
    for dependency in dependencies:
        if dependency.stable_role != "production":
            continue
        expected = snapshot.get(dependency.role_relative_path.as_posix())
        if expected is None or expected != dependency:
            raise AuditInfrastructureError("production snapshot generation differs")


class _AuditLaunchContext:
    __slots__ = ("_control_endpoint", "_task", "_deadline")

    def __init__(self, control_endpoint, task: ConfigurationAuditTask, deadline: float) -> None:
        self._control_endpoint = control_endpoint
        self._task = task
        self._deadline = deadline

    @property
    def task_id(self) -> str:
        return self._task.task_id

    @property
    def generation(self) -> int:
        return self._task.generation

    def register_compiler_process_launch(
        self, event: CompilerLaunchEvent, carrier: CompilerProcessHandleCarrier
    ) -> None:
        if (
            event.task_id != self._task.task_id
            or event.generation != self._task.generation
        ):
            raise AuditInfrastructureError("compiler process carrier generation differs")
        register = getattr(
            self._control_endpoint, "register_compiler_process_launch", None
        )
        if not callable(register):
            raise AuditInfrastructureError(
                "parent compiler process launch observer is unavailable"
            )
        register(event, carrier)
        _send_control_frame_before(self._control_endpoint, event, self._deadline)

    def complete_compiler_process_launch(
        self, event: CompilerLaunchEvent, carrier: CompilerProcessHandleCarrier
    ) -> None:
        complete = getattr(
            self._control_endpoint, "complete_compiler_process_launch", None
        )
        if not callable(complete):
            raise AuditInfrastructureError(
                "parent compiler process completion observer is unavailable"
            )
        complete(event, carrier)

    def fail_compiler_process_launch(
        self, event: CompilerLaunchEvent, carrier: CompilerProcessHandleCarrier
    ) -> None:
        fail = getattr(
            self._control_endpoint, "fail_compiler_process_launch", None
        )
        if callable(fail):
            fail(event, carrier)

    def record_process_start(
        self, purpose: CompilerLaunchPurpose, process_start: ProcessStartIdentity
    ) -> None:
        _send_control_frame_before(
            self._control_endpoint,
            CompilerLaunchEvent(
                purpose,
                process_start,
                worker_index=_WORKER_INDEX,
                task_id=self._task.task_id,
                generation=self._task.generation,
            ),
            self._deadline,
        )

    def seal(self) -> None:
        seal = getattr(self._control_endpoint, "seal_audit_launch_protocol", None)
        if not callable(seal):
            raise AuditInfrastructureError("audit compiler launch protocol seal is unavailable")
        seal(self._task.task_id, self._task.generation, self._deadline)


def _send_control_frame_before(control_endpoint, frame, deadline: float) -> None:
    if not isinstance(deadline, (int, float)) or isinstance(deadline, bool):
        raise AuditInfrastructureError("worker control deadline is invalid")
    if time.monotonic() >= deadline:
        raise AuditInfrastructureError("worker control deadline exceeded")
    send = getattr(control_endpoint, "send", None)
    if not callable(send):
        raise AuditInfrastructureError("worker control endpoint is invalid")
    send(frame, deadline)


def _receive_bounded_command_frame_before(
    command_endpoint,
    task: ConfigurationAuditTask,
    cancel_event,
    deadline: float,
    *,
    maximum_quantum_seconds: float,
):
    if (
        not isinstance(maximum_quantum_seconds, (int, float))
        or isinstance(maximum_quantum_seconds, bool)
        or maximum_quantum_seconds <= 0
        or maximum_quantum_seconds > 0.05
        or time.monotonic() >= deadline
    ):
        raise AuditInfrastructureError("worker command receive boundary is invalid")
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError("worker cancelled before publication permit")
    receive = getattr(command_endpoint, "receive", None)
    if not callable(receive):
        raise AuditInfrastructureError("worker command endpoint is invalid")
    return receive(task, cancel_event, deadline, maximum_quantum_seconds)


def _require_valid_dispatch_reservation(
    reservation, task: ConfigurationAuditTask, generation: int
) -> PerTaskCompactReservation:
    if not isinstance(reservation, PerTaskCompactReservation):
        raise AuditInfrastructureError("worker dispatch reservation is unavailable")
    if task.generation != generation:
        raise AuditInfrastructureError("worker dispatch reservation generation differs")
    reservation.validate(task.task_id, generation)
    return reservation


def audit_configuration_worker(
    task,
    dependency_roots,
    production_snapshot,
    control_endpoint,
    command_endpoint,
    deadline,
):
    if not isinstance(task, ConfigurationAuditTask):
        raise AuditInfrastructureError("configuration audit task is invalid")
    reservation = (
        task.compact_reservation
        if isinstance(task.compact_reservation, PerTaskCompactReservation)
        else None
    )
    publication_permit: CachePublicationPermit | None = None
    try:
        if task.dependency_root_authority is not dependency_roots:
            raise AuditInfrastructureError(
                "worker dependency-root authority identity changed"
            )
        if not isinstance(_WORKER_GENERATION, int) or isinstance(
            _WORKER_GENERATION, bool
        ):
            raise AuditInfrastructureError("worker generation is unavailable")
        reservation = _require_valid_dispatch_reservation(
            task.compact_reservation, task, _WORKER_GENERATION
        )
        reservation.require_before_discovery(task.task_id, _WORKER_GENERATION)
        if _WORKER_CANCEL_EVENT is not None and _WORKER_CANCEL_EVENT.is_set():
            raise AuditInfrastructureError("worker cancelled before discovery")
        if (
            not isinstance(_WORKER_INDEX, int)
            or isinstance(_WORKER_INDEX, bool)
            or _WORKER_INDEX < 0
            or not isinstance(_WORKER_PRODUCTION, Mapping)
            or not isinstance(_WORKER_LIMITS, AuditLimits)
            or not isinstance(_WORKER_ENGINE, str)
            or _WORKER_CACHE is None
            or not callable(getattr(_WORKER_RSS, "sample", None))
        ):
            raise AuditInfrastructureError("worker runtime binding is invalid")
        from gpu_capability_source_audit import (
            _attest_loaded_audit_engine,
            audit_preprocessed_view,
        )

        engine = _attest_loaded_audit_engine(_WORKER_ENGINE)
        launch_context = _AuditLaunchContext(control_endpoint, task, deadline)
        view, discovery, preprocess_stages = stabilize_and_parse_configuration(
            task.configuration,
            dependency_roots,
            _WORKER_PRODUCTION,
            _WORKER_LIMITS,
            deadline,
            _WORKER_CANCEL_EVENT,
            launch_context=launch_context,
        )
        launch_context.seal()
        validate_production_dependency_snapshots(
            discovery.dependencies, production_snapshot
        )
        with _stage_timer() as audit_time:
            findings = audit_preprocessed_view(
                view, _WORKER_LIMITS, _WORKER_RSS.sample
            )
        draft = _bounded_compact_result_draft(
            task.configuration.digest,
            engine,
            discovery.dependencies,
            findings,
            task.task_id,
            task.generation,
            reservation,
        )
        del view
        _send_control_frame_before(
            control_endpoint,
            CachePublicationRequested(
                _WORKER_INDEX, _WORKER_GENERATION, task.task_id
            ),
            deadline,
        )
        received = _receive_bounded_command_frame_before(
            command_endpoint,
            task,
            _WORKER_CANCEL_EVENT,
            deadline,
            maximum_quantum_seconds=0.05,
        )
        if not isinstance(received, CachePublicationPermit):
            raise AuditInfrastructureError("root publication permit is invalid")
        publication_permit = received
        if (
            _WORKER_CANCEL_EVENT is not None
            and _WORKER_CANCEL_EVENT.is_set()
        ):
            raise AuditInfrastructureError(
                "worker cancelled before compact-result materialization"
            )
        result = draft.materialize_under(reservation, publication_permit)
        with _stage_timer() as publish_time:
            accepted = _WORKER_CACHE.publish(
                task.configuration,
                dependency_roots,
                result,
                publication_permit,
                deadline,
            )
        if not isinstance(accepted, ConfigurationAuditResult):
            raise AuditInfrastructureError("cache publication result is invalid")
        if accepted is not result:
            ownership = result._transport_ownership
            if ownership is None:
                raise AuditInfrastructureError(
                    "cache publication result ownership is unavailable"
                )
            rebound = ownership.rebind_materialized_result()
            accepted = ConfigurationAuditResult(
                accepted.configuration_digest,
                accepted.audit_engine_fingerprint,
                accepted.dependencies,
                accepted.reached_production,
                accepted.findings,
                rebound,
            )
        transport = encode_configuration_audit_result_transport(
            accepted, reservation.worker_transport_capability
        )
        del accepted
        del result
        del draft
        stages = WorkerStageTimings(
            preprocess_stages.discovery_seconds,
            preprocess_stages.accepted_parse_seconds,
            audit_time.elapsed_seconds,
            publish_time.elapsed_seconds,
        )
        return ConfigurationAuditTransportOutcome(
            transport=transport,
            stdout_bytes=preprocess_stages.compiler_stdout_bytes,
            stages=stages,
        )
    finally:
        active_error = sys.exc_info()[1]
        cleanup_error: BaseException | None = None
        if publication_permit is not None and not publication_permit.released:
            try:
                publication_permit.release_root_publication()
            except BaseException as error:
                if active_error is not None:
                    active_error.add_note(
                        f"root publication permit cleanup also failed: {error}"
                    )
                else:
                    cleanup_error = error
        keep_receiver_ownership = (
            active_error is None
            and cleanup_error is None
            and reservation is not None
            and reservation.owner_phase == "receiver-retained-result"
        )
        if (
            reservation is not None
            and not reservation.released
            and not keep_receiver_ownership
        ):
            try:
                reservation.release(
                    "worker-failure" if active_error is not None else "worker-return"
                )
            except BaseException as error:
                if active_error is not None:
                    active_error.add_note(
                        f"compact reservation cleanup also failed: {error}"
                    )
                elif cleanup_error is not None:
                    cleanup_error.add_note(
                        f"compact reservation cleanup also failed: {error}"
                    )
                else:
                    cleanup_error = error
        if active_error is None and cleanup_error is not None:
            raise cleanup_error


def receive_configuration_audit_outcome(
    reservation: PerTaskCompactReservation,
    capability: CompactResultTransportCapability,
    worker_outcome: ConfigurationAuditTransportOutcome,
) -> ConfigurationAuditOutcome:
    if (
        not isinstance(reservation, PerTaskCompactReservation)
        or not isinstance(capability, CompactResultTransportCapability)
        or not isinstance(worker_outcome, ConfigurationAuditTransportOutcome)
    ):
        raise AuditInfrastructureError("worker audit transport outcome is invalid")
    result = decode_configuration_audit_result_transport(
        worker_outcome.transport, reservation, capability
    )
    return ConfigurationAuditOutcome(
        result, worker_outcome.stdout_bytes, worker_outcome.stages
    )


class _StreamDigestConsumer:
    def __init__(self, downstream: Callable[[bytes], None] | None = None) -> None:
        self._digest = hashlib.sha256()
        self._count = 0
        self._downstream = downstream

    def __call__(self, chunk: bytes) -> None:
        self._digest.update(chunk)
        self._count += len(chunk)
        if self._downstream is not None:
            self._downstream(chunk)

    def finish(self) -> StreamDigest:
        return StreamDigest(self._digest.hexdigest(), self._count)


def _dependency_binding(
    path: Path, authority: DependencyRootAuthority
) -> tuple[str, PurePosixPath]:
    matches: list[tuple[str, PurePosixPath]] = []
    for binding in (authority.source_root, *authority.external_roots):
        try:
            relative = path.relative_to(binding.resolved_root)
        except ValueError:
            continue
        matches.append(
            (binding.stable_role, PurePosixPath(relative.as_posix()))
        )
    if len(matches) != 1:
        raise AuditInfrastructureError(
            "dependency is not mapped by exactly one dependency-root authority"
        )
    return matches[0]


def _hash_dependency_identity(
    identity: FileIdentity,
    authority: DependencyRootAuthority,
    deadline: float | None = None,
    cancel_event: object | None = None,
    expected_snapshot: tuple[int, int | None, int, int, int] | None = None,
) -> DependencyDigest:
    path = identity.canonical
    stable_role, relative = _dependency_binding(path, authority)
    digest = hashlib.sha256()
    try:
        _check_dependency_budget(deadline, cancel_event)
        before = path.stat()
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            while chunk := stream.read(1024 * 1024):
                _check_dependency_budget(deadline, cancel_event)
                digest.update(chunk)
            after_open = os.fstat(stream.fileno())
        after = path.stat()
    except OSError as error:
        raise AuditInfrastructureError("dependency changed while hashing") from error
    snapshot = lambda item: (
        int(item.st_dev),
        int(item.st_ino) if int(item.st_ino) != 0 else None,
        int(item.st_size),
        int(item.st_mtime_ns),
        int(getattr(item, "st_ctime_ns", 0)),
    )
    expected = snapshot(before)
    if expected_snapshot is not None and expected != expected_snapshot:
        raise AuditInfrastructureError("dependency changed before hashing")
    if (
        snapshot(opened)[:4] != expected[:4]
        or snapshot(after_open)[:4] != expected[:4]
        or snapshot(after) != expected
    ):
        raise AuditInfrastructureError("dependency changed while hashing")
    if (identity.device, identity.inode) != expected[:2]:
        raise AuditInfrastructureError("dependency identity changed while hashing")
    return DependencyDigest(stable_role, relative, identity, digest.hexdigest())


def _dependency_digests(
    identities: tuple[FileIdentity, ...],
    authority: DependencyRootAuthority,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[DependencyDigest, ...]:
    snapshots = []
    total_bytes = 0
    for identity in identities:
        _check_dependency_budget(deadline, cancel_event)
        try:
            metadata = identity.canonical.stat()
        except OSError as error:
            raise AuditInfrastructureError("dependency changed before hashing") from error
        snapshot = (
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            int(metadata.st_size), int(metadata.st_mtime_ns),
            int(getattr(metadata, "st_ctime_ns", 0)),
        )
        if snapshot[2] > _DEPENDENCY_FILE_BYTES:
            raise AuditInfrastructureError("dependency per-file byte ceiling exceeded")
        total_bytes += snapshot[2]
        if total_bytes > _DEPENDENCY_TOTAL_BYTES:
            raise AuditInfrastructureError("dependency total byte ceiling exceeded")
        snapshots.append(snapshot)
    return tuple(
        _hash_dependency_identity(
            identity, authority, deadline, cancel_event, snapshot
        )
        for identity, snapshot in zip(identities, snapshots)
    )


def _parse_dependency_output(
    rewritten: RewrittenCommand,
    configuration: PreprocessConfiguration,
    authority: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[FileIdentity, ...]:
    if rewritten.dependency_format == "gcc-depfile":
        paths = parse_gcc_dependencies(
            rewritten.dependency_output, deadline=deadline,
            cancel_event=cancel_event,
        )
    elif rewritten.dependency_format == "msvc-json":
        paths = parse_msvc_dependencies(
            rewritten.dependency_output, deadline=deadline,
            cancel_event=cancel_event,
        )
    else:
        raise AuditInfrastructureError(
            f"unsupported dependency format: {rewritten.dependency_format}"
        )
    return validate_dependency_identities(
        _absolute_dependencies(paths, configuration.working_directory),
        authority.source_root.resolved_root,
        production,
        deadline=deadline,
        cancel_event=cancel_event,
    )


_GENERATION_GUARD_IDENTITY_METADATA_BYTES = 96
_GENERATION_GUARD_OWNER_METADATA_BYTES = 128
_GENERATION_GUARD_INDEX_METADATA_BYTES = 72


@dataclass(frozen=True, slots=True)
class GenerationGuardPreflight:
    dependencies: tuple[DependencyDigest, ...]
    dependencies_by_path: Mapping[str, DependencyDigest]
    directory_paths: tuple[Path, ...]
    file_paths: tuple[Path, ...]
    metadata_bytes: int
    dependency_root_authority: DependencyRootAuthority


@dataclass(frozen=True, slots=True)
class DependencyGenerationGuard:
    dependency: DependencyDigest
    held_file_owner: object
    held_parent_owner: object
    armed_stat: FileIdentity
    armed_size: int
    armed_mtime_ns: int
    armed_ctime_ns: int
    platform_watch: object
    directory_chain_owners: tuple[object, ...]
    directory_chain_identities: tuple[FileIdentity, ...]


def _checked_guard_metadata_add(total: int, amount: int, ceiling: int) -> int:
    if amount < 0 or total > sys.maxsize - amount:
        raise AuditInfrastructureError("dependency generation metadata overflow")
    result = total + amount
    if result > ceiling:
        raise AuditInfrastructureError("dependency generation metadata byte ceiling exceeded")
    return result


def preflight_dependency_generation_guards(
    discovery: PreprocessDiscovery,
    dependency_roots: DependencyRootAuthority,
    limits: AuditLimits,
) -> GenerationGuardPreflight:
    authority = validate_dependency_root_authority(dependency_roots)
    if not isinstance(discovery, PreprocessDiscovery) or not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("dependency generation preflight is invalid")
    dependencies_by_path: dict[str, DependencyDigest] = {}
    directory_paths: dict[str, Path] = {}
    for dependency in discovery.dependencies:
        if not isinstance(dependency, DependencyDigest):
            raise AuditInfrastructureError("dependency generation preflight is invalid")
        path = dependency.identity.canonical
        key = os.path.normcase(str(path))
        if key in dependencies_by_path:
            raise AuditInfrastructureError("dependency generation path is duplicate or ambiguous")
        dependencies_by_path[key] = dependency
        role, _relative = _dependency_binding(path, authority)
        binding = next(
            item for item in (authority.source_root, *authority.external_roots)
            if item.stable_role == role
        )
        try:
            relative_parent = path.parent.relative_to(binding.resolved_root)
        except ValueError as error:
            raise AuditInfrastructureError("dependency generation path escaped authority") from error
        current = binding.resolved_root.parent
        directory_paths.setdefault(os.path.normcase(str(current)), current)
        current = binding.resolved_root
        directory_paths.setdefault(os.path.normcase(str(current)), current)
        for part in relative_parent.parts:
            current = current / part
            directory_paths.setdefault(os.path.normcase(str(current)), current)
    if not dependencies_by_path:
        raise AuditInfrastructureError("dependency generation file ceiling exceeded")
    if len(dependencies_by_path) > limits.unique_dependency_handles:
        raise AuditInfrastructureError("dependency generation file ceiling exceeded")
    if len(directory_paths) > limits.unique_generation_guard_directories:
        raise AuditInfrastructureError("dependency generation directory ceiling exceeded")
    metadata_bytes = 0
    fixed = (
        _GENERATION_GUARD_IDENTITY_METADATA_BYTES
        + _GENERATION_GUARD_OWNER_METADATA_BYTES
        + _GENERATION_GUARD_INDEX_METADATA_BYTES
    )
    for path in (*directory_paths.values(), *(item.identity.canonical for item in dependencies_by_path.values())):
        encoded_bytes = len(os.fsencode(str(path)))
        metadata_bytes = _checked_guard_metadata_add(
            metadata_bytes, fixed + encoded_bytes,
            limits.generation_guard_metadata_bytes,
        )
    return GenerationGuardPreflight(
        tuple(dependencies_by_path.values()),
        MappingProxyType(dependencies_by_path),
        tuple(directory_paths.values()),
        tuple(item.identity.canonical for item in dependencies_by_path.values()),
        metadata_bytes, authority,
    )


class _DependencyGenerationGuards:
    """OS-observed held-file/path-chain validation between the two runs."""

    def __init__(
        self,
        preflight: GenerationGuardPreflight,
        authority: DependencyRootAuthority,
        deadline: float | None = None,
    ) -> None:
        if not isinstance(preflight, GenerationGuardPreflight):
            raise AuditInfrastructureError("dependency generation preflight is invalid")
        if preflight.dependency_root_authority is not authority:
            raise AuditInfrastructureError("dependency generation authority was replaced")
        _check_dependency_budget(deadline, None)
        self._authority = authority
        self._discovery_dependencies = preflight.dependencies
        self._dependencies_by_path = preflight.dependencies_by_path
        self._streams: list[object] = []
        self._observer: _FilesystemGenerationObserver | None = None
        self.guards: tuple[DependencyGenerationGuard, ...] = ()
        try:
            self._directories = tuple(
                (path, self._directory_snapshot(path))
                for path in preflight.directory_paths
            )
            self._files = tuple(
                (dependency.identity.canonical,
                 self._file_snapshot(dependency.identity.canonical.stat()))
                for dependency in preflight.dependencies
            )
            for path, snapshot in self._files:
                _check_dependency_budget(deadline, None)
                stream = path.open("rb")
                opened = os.fstat(stream.fileno())
                if (
                    self._file_snapshot(opened)[:4] != snapshot[:4]
                    or self._file_snapshot(path.stat()) != snapshot
                ):
                    stream.close()
                    raise AuditInfrastructureError("dependency changed while guards were armed")
                self._streams.append(stream)
            self._observer = _FilesystemGenerationObserver(
                tuple((path, True) for path, _snapshot in self._directories)
                + tuple((path, False) for path, _snapshot in self._files)
            )
            self._observer.drain()
            directory_identities = tuple(
                FileIdentity(path, None, snapshot[0], snapshot[1], 0, False)
                for path, snapshot in self._directories
            )
            directory_paths = preflight.directory_paths
            self.guards = tuple(
                DependencyGenerationGuard(
                    dependency, stream, path.parent, dependency.identity,
                    snapshot[2], snapshot[3], snapshot[4], self,
                    directory_paths,
                    directory_identities,
                )
                for dependency, stream, (path, snapshot) in zip(
                    preflight.dependencies, self._streams, self._files
                )
            )
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _file_snapshot(metadata) -> tuple[int, int | None, int, int, int]:
        return (
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            int(metadata.st_size),
            int(metadata.st_mtime_ns),
            int(getattr(metadata, "st_ctime_ns", 0)),
        )

    @staticmethod
    def _directory_snapshot(path: Path) -> tuple[int, int | None, int]:
        metadata = path.lstat()
        if (
            not stat.S_ISDIR(metadata.st_mode)
            or stat.S_ISLNK(metadata.st_mode)
            or bool(getattr(metadata, "st_file_attributes", 0) & 0x400)
        ):
            raise AuditInfrastructureError("dependency directory guard is invalid")
        return (
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            int(metadata.st_mode),
        )

    def validate_and_hash(
        self,
        deadline: float | None = None,
        cancel_event: object | None = None,
    ) -> tuple[DependencyDigest, ...]:
        if self._observer is None:
            raise AuditInfrastructureError("dependency generation guards are not armed")
        self._observer.drain()
        for path, expected in self._directories:
            if self._directory_snapshot(path) != expected:
                raise AuditInfrastructureError("dependency directory generation changed")
        result: list[DependencyDigest] = []
        total_bytes = 0
        for _path, expected in self._files:
            _check_dependency_budget(deadline, cancel_event)
            if expected[2] > _DEPENDENCY_FILE_BYTES:
                raise AuditInfrastructureError("dependency per-file byte ceiling exceeded")
            total_bytes += expected[2]
            if total_bytes > _DEPENDENCY_TOTAL_BYTES:
                raise AuditInfrastructureError("dependency total byte ceiling exceeded")
        for stream, (path, expected) in zip(self._streams, self._files):
            opened = os.fstat(stream.fileno())
            if (
                self._file_snapshot(opened)[:4] != expected[:4]
                or self._file_snapshot(path.stat()) != expected
            ):
                raise AuditInfrastructureError("dependency generation changed")
            dependency = self._dependencies_by_path.get(
                os.path.normcase(str(path))
            )
            if dependency is None or dependency.identity.canonical != path:
                raise AuditInfrastructureError(
                    "dependency generation lookup changed"
                )
            digest = hashlib.sha256()
            stream.seek(0)
            while chunk := stream.read(1024 * 1024):
                _check_dependency_budget(deadline, cancel_event)
                digest.update(chunk)
            stream.seek(0)
            if self._file_snapshot(os.fstat(stream.fileno()))[:4] != expected[:4]:
                raise AuditInfrastructureError("dependency generation changed while hashing")
            result.append(DependencyDigest(
                dependency.stable_role,
                dependency.role_relative_path,
                dependency.identity,
                digest.hexdigest(),
            ))
        self._observer.drain()
        for path, expected in self._directories:
            if self._directory_snapshot(path) != expected:
                raise AuditInfrastructureError("dependency directory generation changed")
        return tuple(result)

    def bind(self, dependencies: tuple[DependencyDigest, ...]) -> "_DependencyGenerationGuards":
        if dependencies != self._discovery_dependencies:
            raise AuditInfrastructureError("dependency guard binding changed")
        self._discovery_dependencies = dependencies
        return self

    def close(self) -> None:
        errors: list[BaseException] = []
        observer = getattr(self, "_observer", None)
        if observer is not None:
            try:
                observer.close()
            except BaseException as error:
                errors.append(error)
            self._observer = None
        for stream in reversed(getattr(self, "_streams", ())):
            try:
                stream.close()
            except BaseException as error:
                errors.append(error)
        self._streams = []
        if errors:
            raise AuditInfrastructureError("dependency generation guard cleanup failed") from errors[0]


def arm_dependency_generation_guards(
    preflight: GenerationGuardPreflight,
    dependency_roots: DependencyRootAuthority,
    deadline: float,
) -> tuple[DependencyGenerationGuard, ...]:
    authority = validate_dependency_root_authority(dependency_roots)
    if preflight.dependency_root_authority is not authority:
        raise AuditInfrastructureError("dependency generation authority was replaced")
    owner = _DependencyGenerationGuards(preflight, authority, deadline)
    return owner.guards


def _guard_owner(
    guards: tuple[DependencyGenerationGuard, ...],
) -> _DependencyGenerationGuards:
    if not guards or any(
        not isinstance(guard, DependencyGenerationGuard) for guard in guards
    ):
        raise AuditInfrastructureError("dependency generation guards are invalid")
    owner = guards[0].platform_watch
    if not isinstance(owner, _DependencyGenerationGuards) or any(
        guard.platform_watch is not owner for guard in guards
    ) or owner.guards != guards:
        raise AuditInfrastructureError("dependency generation guards are invalid")
    return owner


def validate_and_hash_guarded_dependencies(
    guards: tuple[DependencyGenerationGuard, ...],
    deadline: float,
    cancel_event: object | None = None,
) -> tuple[DependencyDigest, ...]:
    owner = _guard_owner(guards)
    try:
        _check_dependency_budget(deadline, cancel_event)
        result = owner.validate_and_hash(deadline, cancel_event)
        _check_dependency_budget(deadline, cancel_event)
        return result
    finally:
        owner.close()


def discover_configuration(
    configuration: PreprocessConfiguration,
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
    cancel_event: object | None = None,
    *,
    launch_context: _AuditLaunchContext | None = None,
) -> PreprocessDiscovery:
    authority = validate_dependency_root_authority(
        dependency_roots,
        expected_digest=configuration.dependency_root_authority_digest,
    )
    validate_compiler_executable_capability(
        configuration.compiler_capability, authority,
        deadline=deadline, cancel_event=cancel_event,
    )
    consumer = _StreamDigestConsumer()
    with tempfile.TemporaryDirectory(
        prefix=".gpu-capability-discovery-",
    ) as temporary:
        suffix = ".json" if configuration.family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        } else ".d"
        rewritten = rewrite_preprocess_command(
            configuration, Path(temporary) / f"dependencies{suffix}"
        )
        run_bounded_preprocessor(
            rewritten,
            configuration,
            limits,
            deadline,
            consumer,
            cancel_event,
            launch_context=launch_context,
            launch_purpose=(
                CompilerLaunchPurpose.AUDIT_DISCOVERY
                if launch_context is not None else None
            ),
        )
        identities = _parse_dependency_output(
            rewritten, configuration, authority, production,
            deadline, cancel_event,
        )
        dependencies = _dependency_digests(
            identities, authority, deadline, cancel_event
        )
    return PreprocessDiscovery(consumer.finish(), dependencies, identities)


def stabilize_and_parse_configuration(
    configuration: PreprocessConfiguration,
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
    cancel_event: object | None = None,
    publication: Callable[[
        PreprocessedTranslationUnitView,
        PreprocessDiscovery,
        Callable[[], None],
    ], PreprocessedTranslationUnitView] | None = None,
    *,
    launch_context: _AuditLaunchContext | None = None,
) -> tuple[PreprocessedTranslationUnitView, PreprocessDiscovery, PreprocessStageTimings]:
    authority = validate_dependency_root_authority(
        dependency_roots,
        expected_digest=configuration.dependency_root_authority_digest,
    )
    validate_compiler_executable_capability(
        configuration.compiler_capability, authority,
        deadline=deadline, cancel_event=cancel_event,
    )
    discovery_started = time.monotonic()
    discovery = discover_configuration(
        configuration,
        dependency_roots,
        production,
        limits,
        deadline,
        cancel_event,
        launch_context=launch_context,
    )
    discovery_seconds = time.monotonic() - discovery_started
    preflight = preflight_dependency_generation_guards(
        discovery, authority, limits
    )
    guards = arm_dependency_generation_guards(
        preflight, authority, deadline
    )
    guard_owner = _guard_owner(guards)
    accepted_started = time.monotonic()

    def validate_accepted_generation() -> None:
        _check_dependency_budget(deadline, cancel_event)
        dependencies = guard_owner.validate_and_hash(deadline, cancel_event)
        _check_dependency_budget(deadline, cancel_event)
        if dependencies != discovery.dependencies:
            raise AuditInfrastructureError("dependency closure changed")

    try:
        if cancel_event is not None and cancel_event.is_set():
            raise AuditInfrastructureError("preprocessing cancelled before accepted launch")
        builder = PreprocessedStreamBuilder(
            configuration,
            production,
            limits,
            _current_process_rss_bytes,
            deadline=deadline,
            cancel_event=cancel_event,
        )
        consumer = _StreamDigestConsumer(builder.feed)
        with tempfile.TemporaryDirectory(
            prefix=".gpu-capability-accepted-",
        ) as temporary:
            suffix = ".json" if configuration.family in {
                CompilerFamily.MSVC, CompilerFamily.CLANG_CL
            } else ".d"
            rewritten = rewrite_preprocess_command(
                configuration, Path(temporary) / f"dependencies{suffix}"
            )
            guard_owner.validate_and_hash(deadline, cancel_event)
            run_bounded_preprocessor(
                rewritten,
                configuration,
                limits,
                deadline,
                consumer,
                cancel_event,
                launch_context=launch_context,
                launch_purpose=(
                    CompilerLaunchPurpose.AUDIT_ACCEPTED
                    if launch_context is not None else None
                ),
            )
            accepted_identities = _parse_dependency_output(
                rewritten, configuration, authority, production,
                deadline, cancel_event,
            )
            _check_dependency_budget(deadline, cancel_event)
            accepted_dependencies = guard_owner.validate_and_hash(
                deadline, cancel_event
            )
            _check_dependency_budget(deadline, cancel_event)
            if tuple(item.identity for item in accepted_dependencies) != accepted_identities:
                raise AuditInfrastructureError("dependency closure changed")
        accepted_stream = consumer.finish()
        if accepted_stream.byte_count != discovery.stream.byte_count:
            raise AuditInfrastructureError("raw preprocessed byte count changed")
        if accepted_stream.sha256 != discovery.stream.sha256:
            raise AuditInfrastructureError("raw preprocessed output changed")
        if accepted_dependencies != discovery.dependencies:
            raise AuditInfrastructureError("dependency closure changed")
        view = builder.finalize(accepted_identities)
        if publication is None:
            validate_accepted_generation()
        else:
            view = publication(view, discovery, validate_accepted_generation)
    finally:
        guard_owner.close()
    return (
        view,
        discovery,
        PreprocessStageTimings(
            discovery_seconds,
            time.monotonic() - accepted_started,
            discovery.stream.byte_count + accepted_stream.byte_count,
        ),
    )


def preprocess_configuration(
    configuration: PreprocessConfiguration,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
    cancel_event: threading.Event | None = None,
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
            deadline=deadline,
            cancel_event=cancel_event,
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
                cancel_event,
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
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    cache: PreprocessCache,
    limits: AuditLimits,
    deadline: float,
    cancel_event: object | None = None,
) -> PreprocessedTranslationUnitView:
    """Return a validated cache hit or publish one complete compiler result."""

    if not isinstance(cache, PreprocessCache):
        raise AuditInfrastructureError("preprocess cache is invalid")
    authority = validate_dependency_root_authority(
        dependency_roots,
        expected_digest=configuration.dependency_root_authority_digest,
    )
    validate_compiler_executable_capability(
        configuration.compiler_capability, authority,
        deadline=deadline, cancel_event=cancel_event,
    )
    cached = cache.load(configuration, deadline, cancel_event)
    if cached is not None:
        return cached
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError(
            f"configuration {configuration.digest} cancelled before preprocessing"
        )
    def publish_accepted(
        accepted: PreprocessedTranslationUnitView,
        discovery: PreprocessDiscovery,
        validate_generation: Callable[[], None],
    ) -> PreprocessedTranslationUnitView:
        try:
            snapshots = cache._snapshot_dependencies(
                discovery.dependency_identities, force=True
            )
        except OSError as error:
            raise AuditInfrastructureError(
                "dependency changed during preprocessing"
            ) from error
        if len(snapshots) != len(discovery.dependencies) or any(
            snapshot.identity != dependency.identity
            or snapshot.sha256 != dependency.sha256
            for snapshot, dependency in zip(snapshots, discovery.dependencies)
        ):
            raise AuditInfrastructureError(
                "dependency content changed during preprocessing"
            )
        if accepted.configuration != configuration:
            raise AuditInfrastructureError(
                "preprocessor returned a mismatched accepted configuration"
            )
        return cache._publish_stabilized(
            accepted,
            snapshots,
            final_validation=validate_generation,
            deadline=deadline,
            cancel_event=cancel_event,
        )

    accepted, _discovery, _stages = stabilize_and_parse_configuration(
        configuration,
        authority,
        production,
        limits,
        deadline,
        cancel_event,
        publication=publish_accepted,
    )
    return accepted


def _json_object_without_duplicates(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for name, value in pairs:
        if name in result:
            raise AuditInfrastructureError(
                f"compile database object has duplicate key: {name}"
            )
        result[name] = value
    return result


def _configuration_semantics(
    configuration: PreprocessConfiguration,
) -> tuple[object, ...]:
    return (
        configuration.family,
        configuration.compiler,
        configuration.working_directory,
        configuration.source,
        configuration.arguments,
        configuration.environment_digest,
        configuration.digest,
        configuration.dependency_root_authority_digest,
        configuration.compiler_capability_digest,
    )


def _database_path(root: Path, database: Path) -> Path:
    if not isinstance(database, Path):
        raise AuditInfrastructureError("compile database path is invalid")
    candidate = database if database.is_absolute() else root / database
    try:
        canonical = candidate.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile database is unavailable: {candidate}"
        ) from error
    if not canonical.is_file():
        raise AuditInfrastructureError(
            f"compile database must be one ordinary file: {canonical}"
        )
    return canonical


def _load_database_entries(database: Path) -> tuple[tuple[int, Mapping[str, object]], ...]:
    try:
        document = json.loads(
            database.read_text(encoding="utf-8-sig"),
            object_pairs_hook=_json_object_without_duplicates,
        )
    except AuditInfrastructureError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AuditInfrastructureError(
            f"cannot decode compile database {database}: {error}"
        ) from error
    if not isinstance(document, list):
        raise AuditInfrastructureError("compile database root must be an array")
    indexed: list[tuple[int, Mapping[str, object]]] = []
    for index, entry in enumerate(document):
        if not isinstance(entry, Mapping):
            raise AuditInfrastructureError(
                f"compile database entry must be an object: {database}:{index}"
            )
        indexed.append((index, entry))
    try:
        return tuple(
            sorted(
                indexed,
                key=lambda item: (
                    json.dumps(
                        item[1],
                        ensure_ascii=True,
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                    item[0],
                ),
            )
        )
    except (TypeError, ValueError) as error:
        raise AuditInfrastructureError(
            f"compile database contains a non-JSON entry: {database}"
        ) from error


def _entry_is_production(
    entry: Mapping[str, object],
    database: Path,
    source_root: Path,
    production: Mapping[PurePosixPath, FileIdentity],
) -> bool:
    directory_value = entry.get("directory")
    file_value = entry.get("file")
    if (
        not isinstance(directory_value, str)
        or not directory_value
        or "\0" in directory_value
    ):
        raise AuditInfrastructureError(
            "compile command directory must be a non-empty string"
        )
    if not isinstance(file_value, str) or not file_value or "\0" in file_value:
        raise AuditInfrastructureError(
            "compile command file must be a non-empty string"
        )
    lexical_directory = Path(directory_value)
    if not lexical_directory.is_absolute():
        lexical_directory = database.parent / lexical_directory
    lexical_directory = Path(os.path.abspath(lexical_directory))
    lexical_source = Path(file_value)
    if not lexical_source.is_absolute():
        lexical_source = lexical_directory / lexical_source
    lexical_source = Path(os.path.abspath(lexical_source))
    try:
        lexical_directory.relative_to(source_root)
        relative = lexical_source.relative_to(source_root)
    except ValueError as error:
        raise AuditInfrastructureError(
            "compile command directory or source is outside production root: "
            f"{lexical_source}"
        ) from error
    if not relative.parts or relative.parts[0].casefold() not in {
        "playback",
        "recorder_engine",
    }:
        return False

    working_directory, _arguments = decode_compile_entry(
        entry,
        database,
        windows=os.name == "nt",
    )
    source = Path(file_value)
    if not source.is_absolute():
        source = working_directory / source
    try:
        source = source.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile entry source is unavailable: {file_value}"
        ) from error

    for identity in production.values():
        if source == identity.canonical:
            return True
    try:
        metadata = source.stat()
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile entry source is unavailable: {source}"
        ) from error
    source_file_id = (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) else None,
    )
    for identity in production.values():
        identity_file_id = (identity.device, identity.inode)
        if identity_file_id[1] is not None and source_file_id == identity_file_id:
            raise AuditInfrastructureError(
                f"compile entry source aliases production file: {source}"
            )
    return False


def collect_configurations(
    root: Path,
    databases: tuple[Path, ...],
    environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    pipeline_deadline: float,
) -> tuple[PreprocessConfiguration, ...]:
    """Normalize every database entry and coalesce only semantic duplicates."""

    if not isinstance(root, Path):
        raise AuditInfrastructureError("production root is invalid")
    lexical_root = root.absolute()
    authority = validate_dependency_root_authority(dependency_roots)
    if (
        not isinstance(pipeline_deadline, (int, float))
        or isinstance(pipeline_deadline, bool)
        or time.monotonic() >= pipeline_deadline
    ):
        raise AuditInfrastructureError("configuration collection deadline exceeded")
    if not isinstance(databases, tuple) or not databases:
        raise AuditInfrastructureError("at least one compile database is required")
    # This validates environment keys and values before compiler probing begins.
    _environment_digest(environment)
    production = enumerate_production_identities(lexical_root)
    if time.monotonic() >= pipeline_deadline:
        raise AuditInfrastructureError("configuration collection deadline exceeded")
    # Enumeration has already rejected every aliasing root component, so this
    # resolution cannot hide a symlink/reparse traversal from identity checks.
    canonical_root = lexical_root.resolve(strict=True)
    if canonical_root != authority.source_root.resolved_root:
        raise AuditInfrastructureError("collection root differs from dependency authority")
    canonical_databases = sorted(
        {_database_path(lexical_root, database) for database in databases},
        key=lambda path: (str(path).casefold(), str(path)),
    )
    limits = AuditLimits()
    by_digest: dict[str, PreprocessConfiguration] = {}
    for database in canonical_databases:
        for entry_index, entry in _load_database_entries(database):
            if time.monotonic() >= pipeline_deadline:
                raise AuditInfrastructureError(
                    "configuration collection deadline exceeded"
                )
            if not _entry_is_production(
                entry,
                database,
                canonical_root,
                production,
            ):
                continue
            configuration = make_configuration(
                entry,
                database,
                entry_index,
                canonical_root,
                production,
                environment,
                limits,
                authority,
                None,
                "",
                pipeline_deadline,
            )
            previous = by_digest.get(configuration.digest)
            if previous is None:
                by_digest[configuration.digest] = configuration
            elif _configuration_semantics(previous) != _configuration_semantics(
                configuration
            ):
                raise AuditInfrastructureError(
                    f"configuration digest collision: {configuration.digest}"
                )
    return tuple(by_digest[digest] for digest in sorted(by_digest))


def _configuration_is_objcpp(configuration: PreprocessConfiguration) -> bool:
    if configuration.source.canonical.suffix.casefold() == ".mm":
        return True
    arguments = configuration.arguments
    for index, argument in enumerate(arguments):
        lowered = argument.casefold()
        if lowered == "-x" and index + 1 < len(arguments):
            if arguments[index + 1].casefold() in {"objective-c++", "objective-c++-cpp-output"}:
                return True
        if lowered.startswith("-x") and lowered[2:] in {
            "objective-c++",
            "objective-c++-cpp-output",
        }:
            return True
        if lowered.startswith("/clang:-x") and lowered[9:] in {
            "objective-c++",
            "objective-c++-cpp-output",
        }:
            return True
    return False


def _is_windows_backend_path(path: PurePosixPath) -> bool:
    parts = tuple(part.casefold() for part in path.parts)
    stem = path.stem.casefold()
    return "win" in parts[:-1] or stem.endswith(("_win", "_mediafoundation"))


def _validated_orchestration_inputs(
    configurations: tuple[PreprocessConfiguration, ...],
    production: Mapping[PurePosixPath, FileIdentity],
    cache: PreprocessCache,
    limits: AuditLimits,
) -> tuple[PreprocessConfiguration, ...]:
    if not isinstance(configurations, tuple) or not configurations:
        raise AuditInfrastructureError("no usable compile configurations")
    if not isinstance(cache, PreprocessCache):
        raise AuditInfrastructureError("preprocess cache is invalid")
    if not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("audit limits are invalid")
    if (
        not isinstance(limits.workers, int)
        or isinstance(limits.workers, bool)
        or limits.workers <= 0
        or not isinstance(limits.total_seconds, (int, float))
        or isinstance(limits.total_seconds, bool)
        or limits.total_seconds <= 0
    ):
        raise AuditInfrastructureError("orchestration limits are invalid")
    _source_root(production)
    by_digest: dict[str, PreprocessConfiguration] = {}
    for configuration in configurations:
        if not isinstance(configuration, PreprocessConfiguration):
            raise AuditInfrastructureError("preprocess configuration is invalid")
        relative = configuration.source.relative
        if relative is None or production.get(relative) != configuration.source:
            raise AuditInfrastructureError(
                "configuration source is not in the production identity table: "
                f"{configuration.digest}"
            )
        previous = by_digest.get(configuration.digest)
        if previous is None:
            by_digest[configuration.digest] = configuration
        elif _configuration_semantics(previous) != _configuration_semantics(
            configuration
        ):
            raise AuditInfrastructureError(
                f"configuration digest collision: {configuration.digest}"
            )
    return tuple(by_digest[digest] for digest in sorted(by_digest))


def _coverage_path(
    identity: FileIdentity | None,
    production: Mapping[PurePosixPath, FileIdentity],
) -> PurePosixPath | None:
    if identity is None:
        return None
    if not identity.production:
        return None
    relative = identity.relative
    if relative is None or production.get(relative) != identity:
        raise AuditInfrastructureError(
            f"preprocessed view contains an unknown production identity: {identity.canonical}"
        )
    return relative


def _view_production_provenance(
    view: PreprocessedTranslationUnitView,
    production: Mapping[PurePosixPath, FileIdentity],
) -> frozenset[PurePosixPath]:
    reached: set[PurePosixPath] = set()
    tokens = view.tokens
    for run in tokens.iter_runs():
        relative = _coverage_path(tokens.identity_for(run.identity_id), production)
        if relative is not None:
            reached.add(relative)
    return frozenset(reached)


def preprocess_all(
    configurations: tuple[PreprocessConfiguration, ...],
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    cache: PreprocessCache,
    limits: AuditLimits,
    pipeline_deadline: float,
) -> tuple[tuple[PreprocessedTranslationUnitView, ...], CoverageReport]:
    """Preprocess every semantic configuration under one bounded coordinator."""

    authority = validate_dependency_root_authority(dependency_roots)
    if (
        not isinstance(pipeline_deadline, (int, float))
        or isinstance(pipeline_deadline, bool)
        or time.monotonic() >= pipeline_deadline
    ):
        raise AuditInfrastructureError("preprocess pipeline deadline exceeded")
    ordered = _validated_orchestration_inputs(configurations, production, cache, limits)
    if any(
        item.dependency_root_authority_digest != authority.portable_authority_digest
        for item in ordered
    ):
        raise AuditInfrastructureError("configuration dependency authority differs")
    configured_families = frozenset(item.family for item in ordered)
    has_objcpp = any(_configuration_is_objcpp(item) for item in ordered)
    has_windows_backend = any(
        item.source.relative is not None
        and _is_windows_backend_path(item.source.relative)
        for item in ordered
    )
    active = frozenset(
        path
        for path in production
        if requires_compile_entry(
            path,
            configured_families,
            has_objcpp,
            has_windows_backend,
        )
    )
    configured_sources = frozenset(
        item.source.relative for item in ordered if item.source.relative is not None
    )
    missing_commands = sorted(active - configured_sources, key=lambda path: path.as_posix())
    if missing_commands:
        raise AuditInfrastructureError(
            "active source has no compile command: "
            + ", ".join(path.as_posix() for path in missing_commands)
        )

    cancellation = threading.Event()
    deadline = pipeline_deadline
    views: dict[str, PreprocessedTranslationUnitView] = {}
    failures: list[tuple[str, str]] = []
    iterator = iter(ordered)

    def execute(configuration: PreprocessConfiguration) -> PreprocessedTranslationUnitView:
        return load_or_preprocess(
            configuration,
            authority,
            production,
            cache,
            limits,
            deadline,
            cancellation,
        )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=min(limits.workers, len(ordered)),
        thread_name_prefix="gpu-capability-preprocess",
    ) as executor:
        pending: dict[
            concurrent.futures.Future[PreprocessedTranslationUnitView],
            PreprocessConfiguration,
        ] = {}
        for _ in range(min(limits.workers, len(ordered))):
            configuration = next(iterator, None)
            if configuration is not None:
                pending[executor.submit(execute, configuration)] = configuration

        while pending:
            completed, _ = concurrent.futures.wait(
                pending,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            for future in sorted(completed, key=lambda item: pending[item].digest):
                configuration = pending.pop(future)
                try:
                    view = future.result()
                    if view.configuration != configuration:
                        raise AuditInfrastructureError(
                            "preprocessor returned a mismatched orchestration configuration"
                        )
                    views[configuration.digest] = view
                except Exception as error:
                    failures.append((configuration.digest, str(error)))
            if failures:
                cancellation.set()
                continue
            while len(pending) < limits.workers:
                configuration = next(iterator, None)
                if configuration is None:
                    break
                pending[executor.submit(execute, configuration)] = configuration

    if failures:
        details = "; ".join(
            f"digest={digest}: {message}"
            for digest, message in sorted(failures, key=lambda item: (item[0], item[1]))
        )
        raise AuditInfrastructureError(
            f"GPU capability preprocessing configurations failed: {details}"
        )

    ordered_views = tuple(views[item.digest] for item in ordered)
    authoritative: set[PurePosixPath] = set()
    missing_view_sources: list[tuple[str, PurePosixPath]] = []
    for view in ordered_views:
        reached = _view_production_provenance(view, production)
        authoritative.update(reached)
        source = view.configuration.source
        if source.relative is None:
            raise AuditInfrastructureError("production source has no relative path")
        if source.relative in reached:
            continue
        # A physically empty main source has no token whose marker can carry
        # provenance. Its own validated configuration and manifest membership
        # are the only complete evidence available; headers never get this
        # exception because they are not configuration main sources.
        if source.line_count == 0 and source in view.dependencies:
            authoritative.add(source.relative)
            continue
        missing_view_sources.append((view.configuration.digest, source.relative))
    if missing_view_sources:
        details = ", ".join(
            f"digest={digest} source={path.as_posix()}"
            for digest, path in sorted(
                missing_view_sources,
                key=lambda item: (item[0], item[1].as_posix()),
            )
        )
        raise AuditInfrastructureError(
            f"configuration view lacks main-source provenance: {details}"
        )
    missing_authoritative = sorted(
        active - authoritative,
        key=lambda path: path.as_posix(),
    )
    if missing_authoritative:
        raise AuditInfrastructureError(
            "active source lacks authoritative compiler coverage: "
            + ", ".join(path.as_posix() for path in missing_authoritative)
        )
    source_only = frozenset(set(production) - authoritative)
    return ordered_views, CoverageReport(
        authoritative=frozenset(authoritative),
        source_only=source_only,
        configurations=tuple(item.digest for item in ordered),
    )
