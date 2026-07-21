"""Shared immutable model for the compiler-authoritative GPU capability audit."""

from __future__ import annotations

import enum
import dataclasses
import hashlib
import json
import math
import os
import stat
import struct
import sys
import threading
import time
from array import array
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Callable, overload


UINT32_MAX = (1 << 32) - 1
_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm"})
_TRANSLATION_UNIT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".mm"})
_PRODUCTION_ROOTS = ("playback", "recorder_engine")
_REPARSE_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class AuditInfrastructureError(RuntimeError):
    pass


class _HeldCompilerCapabilityMismatch(AuditInfrastructureError):
    pass


class _FilesystemGenerationObserver:
    """Own OS guards that make any watched file/path generation change fatal."""

    def __init__(self, paths: tuple[tuple[Path, bool], ...]) -> None:
        if not isinstance(paths, tuple) or not paths or len(paths) > 131072:
            raise AuditInfrastructureError("filesystem generation watch ceiling exceeded")
        if len({os.path.normcase(str(path)) for path, _is_directory in paths}) != len(paths):
            raise AuditInfrastructureError("filesystem generation watches are not unique")
        self._backend = (
            "windows" if os.name == "nt"
            else "macos" if sys.platform == "darwin"
            else "linux" if sys.platform.startswith("linux")
            else "unsupported"
        )
        self._owner: object | None = None
        self._handles: list[int] = []
        self._closed = False
        try:
            if self._backend == "windows":
                self._arm_windows(paths)
            elif self._backend == "linux":
                self._arm_linux(paths)
            elif self._backend == "macos":
                self._arm_macos(paths)
            elif self._backend == "unsupported":
                raise AuditInfrastructureError(
                    "filesystem generation observation is unsupported"
                )
            self.drain()
        except BaseException:
            self._close_no_raise()
            raise

    def _arm_windows(self, paths: tuple[tuple[Path, bool], ...]) -> None:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_file = kernel32.CreateFileW
        create_file.argtypes = (
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        )
        create_file.restype = wintypes.HANDLE
        close_handle = kernel32.CloseHandle
        close_handle.argtypes = (wintypes.HANDLE,)
        close_handle.restype = wintypes.BOOL
        self._owner = (kernel32, close_handle)
        invalid = ctypes.c_void_p(-1).value
        for path, is_directory in paths:
            metadata = path.lstat()
            is_alias = stat.S_ISLNK(metadata.st_mode) or bool(
                getattr(metadata, "st_file_attributes", 0) & _REPARSE_ATTRIBUTE
            )
            access = 0 if is_directory or is_alias else 0x80000000  # GENERIC_READ
            share = 0x1 | (0x2 if is_directory else 0)  # never FILE_SHARE_DELETE
            flags = (
                (0x02000000 if is_directory or is_alias else 0x00000080)
                | (0x00200000 if is_alias else 0)
            )
            handle = create_file(str(path), access, share, None, 3, flags, None)
            numeric = ctypes.cast(handle, ctypes.c_void_p).value
            if numeric in (None, invalid):
                errors = []
                for acquired in reversed(self._handles):
                    if not close_handle(acquired):
                        errors.append(acquired)
                self._handles.clear()
                self._owner = None
                if errors:
                    raise AuditInfrastructureError(
                        "Windows filesystem generation guard partial cleanup failed"
                    )
                raise AuditInfrastructureError(
                    "Windows filesystem generation guard setup failed"
                )
            self._handles.append(int(numeric))

    def _arm_linux(self, paths: tuple[tuple[Path, bool], ...]) -> None:
        import ctypes

        libc = ctypes.CDLL(None, use_errno=True)
        initialize = getattr(libc, "inotify_init1", None)
        add_watch = getattr(libc, "inotify_add_watch", None)
        remove_watch = getattr(libc, "inotify_rm_watch", None)
        if initialize is None or add_watch is None or remove_watch is None:
            raise AuditInfrastructureError("Linux inotify generation guards are unsupported")
        initialize.argtypes = (ctypes.c_int,)
        initialize.restype = ctypes.c_int
        add_watch.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32)
        add_watch.restype = ctypes.c_int
        remove_watch.argtypes = (ctypes.c_int, ctypes.c_int)
        remove_watch.restype = ctypes.c_int
        descriptor = int(initialize(os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)))
        if descriptor < 0:
            raise AuditInfrastructureError("Linux inotify generation guard setup failed")
        self._handles.append(descriptor)
        mask = (
            0x00000002 | 0x00000004 | 0x00000008
            | 0x00000040 | 0x00000080 | 0x00000100 | 0x00000200
            | 0x00000400 | 0x00000800 | 0x00002000 | 0x02000000
        )
        watches: list[int] = []
        directory_watches: set[int] = set()
        self._owner = (remove_watch, watches)
        for path, is_directory in paths:
            watch = int(add_watch(descriptor, os.fsencode(path), mask))
            if watch < 0:
                raise AuditInfrastructureError("Linux inotify generation guard setup failed")
            if watch in watches:
                if is_directory and watch in directory_watches:
                    continue
                raise AuditInfrastructureError("Linux inotify generation guard setup failed")
            watches.append(watch)
            if is_directory:
                directory_watches.add(watch)

    def _arm_macos(self, paths: tuple[tuple[Path, bool], ...]) -> None:
        import select

        if not hasattr(select, "kqueue") or not hasattr(select, "KQ_FILTER_VNODE"):
            raise AuditInfrastructureError("macOS vnode generation guards are unsupported")
        queue = select.kqueue()
        descriptors: list[int] = []
        flags = (
            select.KQ_NOTE_WRITE | select.KQ_NOTE_RENAME | select.KQ_NOTE_DELETE
            | select.KQ_NOTE_ATTRIB
        )
        try:
            for path, _is_directory in paths:
                descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
                descriptors.append(descriptor)
                event = select.kevent(
                    descriptor, filter=select.KQ_FILTER_VNODE,
                    flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR, fflags=flags,
                )
                if queue.control((event,), 0, 0):
                    raise AuditInfrastructureError("macOS vnode generation guard setup failed")
        except BaseException:
            for descriptor in reversed(descriptors):
                os.close(descriptor)
            queue.close()
            raise
        self._handles.extend(descriptors)
        self._owner = queue

    def drain(self) -> None:
        if self._closed:
            raise AuditInfrastructureError("filesystem generation observer is closed")
        if self._backend == "windows":
            return
        if self._backend == "macos":
            events = self._owner.control(None, 131072, 0)
            if events:
                raise AuditInfrastructureError("macOS vnode generation change observed")
            return
        descriptor = self._handles[0]
        while True:
            try:
                payload = os.read(descriptor, 1024 * 1024)
            except BlockingIOError:
                return
            except OSError as error:
                raise AuditInfrastructureError("Linux inotify generation guard failed") from error
            if not payload:
                raise AuditInfrastructureError("Linux inotify generation watch was lost")
            offset = 0
            while offset < len(payload):
                if len(payload) - offset < 16:
                    raise AuditInfrastructureError("Linux inotify event stream is invalid")
                _watch, mask, _cookie, name_length = struct.unpack_from("iIII", payload, offset)
                name_start = offset + 16
                name = payload[name_start:name_start + name_length].rstrip(b"\0")
                offset += 16 + name_length
                if offset > len(payload):
                    raise AuditInfrastructureError("Linux inotify event stream is invalid")
                if mask & 0x00004000:
                    raise AuditInfrastructureError("Linux inotify generation event overflow")
                if mask & (0x00002000 | 0x00008000):
                    raise AuditInfrastructureError("Linux inotify generation watch was lost")
                raise AuditInfrastructureError(
                    "Linux inotify generation change observed: "
                    f"mask=0x{mask:08x}, name={name!r}"
                )

    def _close_no_raise(self) -> list[BaseException]:
        if self._closed:
            return []
        self._closed = True
        errors: list[BaseException] = []
        if self._backend == "windows":
            close_handle = self._owner[1] if self._owner is not None else None
            if close_handle is not None:
                for handle in reversed(self._handles):
                    if not close_handle(handle):
                        errors.append(OSError("CloseHandle failed"))
        elif self._backend == "macos":
            queue = self._owner
            for descriptor in reversed(self._handles):
                try:
                    os.close(descriptor)
                except OSError as error:
                    errors.append(error)
            if queue is not None:
                try:
                    queue.close()
                except OSError as error:
                    errors.append(error)
        elif self._backend == "linux":
            import ctypes
            import errno

            descriptor = self._handles[0] if self._handles else None
            if descriptor is not None and self._owner is not None:
                remove_watch, watches = self._owner
                for watch in reversed(watches):
                    ctypes.set_errno(0)
                    if remove_watch(descriptor, watch) != 0:
                        error_number = ctypes.get_errno()
                        if error_number != errno.EINVAL:
                            errors.append(OSError(error_number, "inotify_rm_watch failed"))
            for descriptor in reversed(self._handles):
                try:
                    os.close(descriptor)
                except OSError as error:
                    errors.append(error)
        else:
            for descriptor in reversed(self._handles):
                try:
                    os.close(descriptor)
                except OSError as error:
                    errors.append(error)
        self._handles.clear()
        return errors

    def close(self) -> None:
        errors = self._close_no_raise()
        if errors:
            raise AuditInfrastructureError(
                "filesystem generation observer cleanup failed"
            ) from errors[0]

    def __del__(self) -> None:
        try:
            self._close_no_raise()
        except Exception:
            pass


class CompilerFamily(enum.Enum):
    GCC = "gcc"
    CLANG = "clang"
    MSVC = "msvc"
    CLANG_CL = "clang-cl"


def _default_worker_count() -> int:
    return min(8, os.cpu_count() or 1)


@dataclass(frozen=True)
class AuditLimits:
    response_depth: int = 8
    response_files: int = 32
    response_bytes: int = 4 * 1024 * 1024
    invocation_seconds: float = 60.0
    total_seconds: float = 180.0
    stdout_bytes: int = 128 * 1024 * 1024
    stderr_bytes: int = 1024 * 1024
    retained_token_bytes: int = 384 * 1024 * 1024
    rss_bytes: int = 512 * 1024 * 1024
    unique_dependency_handles: int = 32_768
    unique_generation_guard_directories: int = 65_536
    generation_guard_metadata_bytes: int = 64 * 1024 * 1024
    dependency_handle_metadata_bytes: int = 32 * 1024 * 1024
    compact_result_bytes: int = 16 * 1024 * 1024
    compact_batch_bytes: int = 128 * 1024 * 1024
    compact_aggregate_bytes: int = 128 * 1024 * 1024
    compact_result_dependencies: int = 16_384
    compact_result_reached: int = 4_096
    compact_result_findings: int = 65_536
    compact_result_path_bytes: int = 16 * 1024
    compact_result_expression_bytes: int = 64 * 1024
    compact_result_reason_bytes: int = 64 * 1024
    workers: int = field(default_factory=_default_worker_count)


class CompilerLaunchPurpose(enum.Enum):
    INSPECTION = "inspection"
    AUDIT_DISCOVERY = "audit-discovery"
    AUDIT_ACCEPTED = "audit-accepted"


@dataclass(frozen=True, slots=True)
class CommandArgumentSpan:
    role: str
    option_index: int
    operand_index: int
    attachment: str
    forwarded_depth: int

    def __post_init__(self) -> None:
        if (not isinstance(self.role, str) or not self.role
                or not isinstance(self.option_index, int)
                or isinstance(self.option_index, bool)
                or self.option_index < -1
                or not isinstance(self.operand_index, int)
                or isinstance(self.operand_index, bool)
                or self.operand_index < -1
                or self.attachment not in {
                    "compiler", "source", "scalar", "separate", "attached",
                    "forwarded", "forwarded-comma",
                }
                or not isinstance(self.forwarded_depth, int)
                or isinstance(self.forwarded_depth, bool)
                or self.forwarded_depth < 0):
            raise AuditInfrastructureError("command argument span is invalid")


@dataclass(frozen=True, slots=True)
class CommandRewriteClassification:
    response_expanded_launcher_stripped_arguments: tuple[str, ...]
    compiler: CommandArgumentSpan
    sources: tuple[CommandArgumentSpan, ...]
    nonsemantic_outputs: tuple[CommandArgumentSpan, ...]
    nonsemantic_dependencies: tuple[CommandArgumentSpan, ...]
    semantic_scalars: tuple[CommandArgumentSpan, ...]
    semantic_paths: tuple[CommandArgumentSpan, ...]

    def __post_init__(self) -> None:
        if (not isinstance(self.response_expanded_launcher_stripped_arguments, tuple)
                or any(not isinstance(value, str)
                       for value in self.response_expanded_launcher_stripped_arguments)
                or not isinstance(self.compiler, CommandArgumentSpan)):
            raise AuditInfrastructureError("command rewrite classification is invalid")
        for collection in (
            self.sources, self.nonsemantic_outputs,
            self.nonsemantic_dependencies, self.semantic_scalars,
            self.semantic_paths,
        ):
            if (not isinstance(collection, tuple)
                    or any(not isinstance(span, CommandArgumentSpan)
                           for span in collection)):
                raise AuditInfrastructureError("command rewrite classification is invalid")


@dataclass(frozen=True, slots=True)
class DecisionConfigurationRecord:
    configuration_digest: str
    compiler_family: CompilerFamily
    compiler_digest: str
    production_source: PurePosixPath
    normalized_decision_arguments: tuple[str, ...]
    decision_environment_digest: str
    working_directory_role: str
    compiler_capability_digest: str

    def __post_init__(self) -> None:
        _validate_digest(self.configuration_digest, "decision configuration")
        _validate_digest(self.compiler_digest, "decision compiler")
        _validate_digest(self.decision_environment_digest,
                         "decision environment")
        _validate_digest(self.compiler_capability_digest,
                         "decision compiler capability")
        if (not isinstance(self.compiler_family, CompilerFamily)
                or not isinstance(self.production_source, PurePosixPath)
                or not isinstance(self.normalized_decision_arguments, tuple)
                or any(not isinstance(value, str)
                       for value in self.normalized_decision_arguments)
                or not isinstance(self.working_directory_role, str)
                or not self.working_directory_role):
            raise AuditInfrastructureError("decision configuration record is invalid")


@dataclass(frozen=True, slots=True)
class ConfigurationCollection:
    configurations: tuple["PreprocessConfiguration", ...]
    decision_records: Mapping[str, DecisionConfigurationRecord]
    inspection_probe_invocations: int
    dependency_root_authority: "DependencyRootAuthority"
    capability_registry: object

    def __post_init__(self) -> None:
        if (not isinstance(self.configurations, tuple)
                or any(not isinstance(value, PreprocessConfiguration)
                       for value in self.configurations)
                or not isinstance(self.decision_records, Mapping)
                or set(self.decision_records)
                != {value.digest for value in self.configurations}
                or any(not isinstance(value, DecisionConfigurationRecord)
                       for value in self.decision_records.values())
                or not isinstance(self.inspection_probe_invocations, int)
                or isinstance(self.inspection_probe_invocations, bool)
                or self.inspection_probe_invocations < 0
                or not isinstance(self.dependency_root_authority,
                                  DependencyRootAuthority)):
            raise AuditInfrastructureError("configuration collection is invalid")


@dataclass(frozen=True, slots=True)
class ProcessStartIdentity:
    platform_kind: str
    pid: int
    native_start_token: str
    handle_cookie: str

    def __post_init__(self) -> None:
        if self.platform_kind not in {"windows", "linux", "macos"}:
            raise AuditInfrastructureError("compiler process platform identity is invalid")
        if not isinstance(self.pid, int) or isinstance(self.pid, bool) or self.pid <= 0:
            raise AuditInfrastructureError("compiler process PID identity is invalid")
        if not isinstance(self.native_start_token, str) or not self.native_start_token:
            raise AuditInfrastructureError("compiler process start identity is invalid")
        if not isinstance(self.handle_cookie, str) or not self.handle_cookie:
            raise AuditInfrastructureError("compiler process handle cookie is invalid")


@dataclass(frozen=True, slots=True)
class CompilerLaunchEvent:
    purpose: CompilerLaunchPurpose
    process_start: ProcessStartIdentity
    worker_index: int | None = None
    task_id: str | None = None
    generation: int | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.purpose, CompilerLaunchPurpose) or not isinstance(
            self.process_start, ProcessStartIdentity
        ):
            raise AuditInfrastructureError("compiler launch event is invalid")
        is_audit = self.purpose in {
            CompilerLaunchPurpose.AUDIT_DISCOVERY,
            CompilerLaunchPurpose.AUDIT_ACCEPTED,
        }
        if is_audit and (
            not isinstance(self.worker_index, int)
            or isinstance(self.worker_index, bool)
            or self.worker_index < 0
            or not isinstance(self.task_id, str)
            or not self.task_id
            or not isinstance(self.generation, int)
            or isinstance(self.generation, bool)
            or self.generation < 0
        ):
            raise AuditInfrastructureError("audit compiler launch identity is invalid")


@dataclass(frozen=True, slots=True)
class WindowsWorkerContainment:
    duplicated_job_handle: int
    expected_job_cookie: str

    def __post_init__(self) -> None:
        if (not isinstance(self.duplicated_job_handle, int)
                or isinstance(self.duplicated_job_handle, bool)
                or self.duplicated_job_handle <= 0
                or not isinstance(self.expected_job_cookie, str)
                or not self.expected_job_cookie):
            raise AuditInfrastructureError("Windows worker containment is invalid")


@dataclass(frozen=True, slots=True)
class LinuxWorkerContainment:
    cgroup_run_path: str
    cgroup_leaf_path: str
    generation_token: str

    def __post_init__(self) -> None:
        if (not isinstance(self.cgroup_run_path, str)
                or not self.cgroup_run_path.startswith("/")
                or not isinstance(self.cgroup_leaf_path, str)
                or not self.cgroup_leaf_path.startswith(
                    self.cgroup_run_path.rstrip("/") + "/")
                or re.fullmatch(r"[0-9a-f]{64}", self.generation_token) is None):
            raise AuditInfrastructureError("Linux worker containment is invalid")


@dataclass(frozen=True, slots=True)
class MacOSWorkerContainment:
    trusted_compiler_driver_fingerprints: tuple[str, ...]

    def __post_init__(self) -> None:
        if (not isinstance(self.trusted_compiler_driver_fingerprints, tuple)
                or not self.trusted_compiler_driver_fingerprints
                or any(re.fullmatch(r"[0-9a-f]{64}", value) is None
                       for value in self.trusted_compiler_driver_fingerprints)):
            raise AuditInfrastructureError("macOS worker containment is invalid")


WorkerContainment = (
    WindowsWorkerContainment | LinuxWorkerContainment | MacOSWorkerContainment
)


@dataclass(frozen=True, slots=True)
class WorkerContained:
    worker_index: int
    generation: int
    worker_pid: int
    containment_identity: str

    def __post_init__(self) -> None:
        if (any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in (self.worker_index, self.generation))
                or not isinstance(self.worker_pid, int)
                or isinstance(self.worker_pid, bool)
                or self.worker_pid <= 0
                or not isinstance(self.containment_identity, str)
                or not self.containment_identity):
            raise AuditInfrastructureError("worker containment acknowledgement is invalid")


@dataclass(frozen=True, slots=True)
class MacOSWorkerSessionReported:
    worker_index: int
    generation: int
    pid: int
    pgid: int
    bsd_start_identity: str

    def __post_init__(self) -> None:
        if (any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in (self.worker_index, self.generation, self.pid, self.pgid))
                or self.pid <= 0 or self.pgid != self.pid
                or not isinstance(self.bsd_start_identity, str)
                or not self.bsd_start_identity):
            raise AuditInfrastructureError("macOS worker session report is invalid")


@dataclass(frozen=True, slots=True)
class CompilerPgidReported:
    worker_index: int
    generation: int
    task_id: int
    purpose: CompilerLaunchPurpose
    pid: int
    pgid: int
    bsd_start_identity: str
    executable_identity: FileIdentity
    executable_sha256: str
    driver_fingerprint: str

    def __post_init__(self) -> None:
        if (any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in (self.worker_index, self.generation, self.task_id,
                              self.pid, self.pgid))
                or self.purpose not in {CompilerLaunchPurpose.AUDIT_DISCOVERY,
                                CompilerLaunchPurpose.AUDIT_ACCEPTED}
                or self.pid <= 0 or self.pgid != self.pid
                or not isinstance(self.bsd_start_identity, str)
                or not self.bsd_start_identity
                or not isinstance(self.executable_identity, FileIdentity)):
            raise AuditInfrastructureError("macOS compiler PGID report is invalid")
        _validate_digest(self.executable_sha256, "macOS compiler executable")
        _validate_digest(self.driver_fingerprint, "macOS compiler driver")


@dataclass(frozen=True, slots=True)
class CompilerExecPermit:
    worker_index: int
    generation: int
    task_id: int
    pgid: int

    def __post_init__(self) -> None:
        if (any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in (self.worker_index, self.generation, self.task_id,
                              self.pgid))
                or self.pgid <= 0):
            raise AuditInfrastructureError("macOS compiler exec permit is invalid")


@dataclass(frozen=True, slots=True)
class MacOSInspectionPgidReported:
    inspection_id: str
    pid: int
    pgid: int
    bsd_start_identity: str
    executable_identity: FileIdentity
    executable_sha256: str

    def __post_init__(self) -> None:
        if (any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in (self.pid, self.pgid))
                or not isinstance(self.inspection_id, str) or not self.inspection_id
                or self.pid <= 0 or self.pgid != self.pid
                or not isinstance(self.bsd_start_identity, str)
                or not self.bsd_start_identity
                or not isinstance(self.executable_identity, FileIdentity)):
            raise AuditInfrastructureError("macOS inspection PGID report is invalid")
        _validate_digest(self.executable_sha256, "macOS inspection executable")


@dataclass(frozen=True, slots=True)
class MacOSInspectionExecPermit:
    inspection_id: str
    pid: int
    pgid: int
    driver_fingerprint: str

    def __post_init__(self) -> None:
        if (any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                for value in (self.pid, self.pgid))
                or not isinstance(self.inspection_id, str) or not self.inspection_id
                or self.pid <= 0 or self.pgid != self.pid):
            raise AuditInfrastructureError("macOS inspection exec permit is invalid")
        _validate_digest(self.driver_fingerprint, "macOS inspection driver")


@dataclass(frozen=True, slots=True)
class CachePublicationRequested:
    worker_index: int
    generation: int
    task_id: str

    def __post_init__(self) -> None:
        if (
            not isinstance(self.worker_index, int)
            or isinstance(self.worker_index, bool)
            or self.worker_index < 0
            or not isinstance(self.generation, int)
            or isinstance(self.generation, bool)
            or self.generation < 0
            or not isinstance(self.task_id, str)
            or not self.task_id
        ):
            raise AuditInfrastructureError("cache publication request is invalid")


@dataclass(frozen=True, slots=True)
class WorkerStageTimings:
    discovery_seconds: float
    accepted_parse_seconds: float
    audit_seconds: float
    publish_seconds: float

    def __post_init__(self) -> None:
        if any(
            not isinstance(value, (int, float))
            or isinstance(value, bool)
            or value < 0
            or not math.isfinite(value)
            for value in (
                self.discovery_seconds,
                self.accepted_parse_seconds,
                self.audit_seconds,
                self.publish_seconds,
            )
        ):
            raise AuditInfrastructureError("worker stage timing is invalid")


@dataclass(frozen=True, slots=True)
class WindowsRunMemoryMeasurements:
    parent_peak_rss_bytes: int
    worker_peak_rss_bytes: int
    maximum_simultaneous_working_set_bytes: int
    aggregate_peak_rss_upper_bound_bytes: int
    job_peak_commit_charge_bytes: int
    job_total_process_count: int
    retained_process_identity_count: int
    inspection_peak_tree_bytes: int
    retained_inspection_process_identity_count: int
    surviving_job_process_count: int
    accounting_complete: bool

    def __post_init__(self) -> None:
        _validate_nonnegative_int_fields(self, "Windows memory measurement")
        if not isinstance(self.accounting_complete, bool):
            raise AuditInfrastructureError("Windows memory accounting flag is invalid")


@dataclass(frozen=True, slots=True)
class LinuxRunMemoryMeasurements:
    cgroup_current_accounted_memory_bytes: int
    cgroup_peak_accounted_memory_bytes: int
    cgroup_memory_max_bytes: int
    cgroup_memory_high_bytes: int
    cgroup_oom_count_delta: int
    cgroup_oom_kill_count_delta: int
    cgroup_max_event_count_delta: int
    service_root_oom_count_delta: int
    service_root_oom_kill_count_delta: int
    service_root_max_event_count_delta: int
    surviving_cgroup_process_count: int
    accounting_complete: bool

    def __post_init__(self) -> None:
        _validate_nonnegative_int_fields(self, "Linux memory measurement")
        if not isinstance(self.accounting_complete, bool):
            raise AuditInfrastructureError("Linux memory accounting flag is invalid")


@dataclass(frozen=True, slots=True)
class MacOSRunMemoryMeasurements:
    maximum_observed_aggregate_resident_bytes: int
    maximum_observed_parent_resident_bytes: int
    maximum_observed_owned_group_resident_bytes: int
    surviving_registered_process_count: int
    known_unreconciled_descendant_count: int
    accounting_complete: bool

    def __post_init__(self) -> None:
        _validate_nonnegative_int_fields(self, "macOS memory measurement")
        if not isinstance(self.accounting_complete, bool):
            raise AuditInfrastructureError("macOS memory accounting flag is invalid")


PlatformRunMemoryMeasurements = (
    WindowsRunMemoryMeasurements | LinuxRunMemoryMeasurements
    | MacOSRunMemoryMeasurements
)


@dataclass(frozen=True, slots=True)
class WorkerTelemetry:
    worker_index: int
    generation: int
    worker_pid: int
    sequence: int
    owned_process_count: int
    worker_current_resident_bytes: int
    worker_peak_resident_bytes: int
    accounting_complete: bool

    def __post_init__(self) -> None:
        _validate_nonnegative_int_fields(self, "worker telemetry")
        if self.worker_pid <= 0 or not isinstance(self.accounting_complete, bool):
            raise AuditInfrastructureError("worker telemetry is invalid")


@dataclass(frozen=True, slots=True)
class WindowsPostReturnSample:
    platform_kind: str
    worker_slot: int
    worker_generation: int
    task_ordinal_in_generation: int
    worker_current_rss_bytes: int
    aggregate_peak_rss_upper_bound_bytes: int
    accounting_complete: bool

    def __post_init__(self) -> None:
        if self.platform_kind != "windows":
            raise AuditInfrastructureError("Windows post-return sample is invalid")
        _validate_post_return_sample(self)


@dataclass(frozen=True, slots=True)
class LinuxPostReturnSample:
    platform_kind: str
    worker_slot: int
    worker_generation: int
    task_ordinal_in_generation: int
    worker_current_rss_bytes: int
    cgroup_current_accounted_memory_bytes: int
    cgroup_peak_accounted_memory_bytes: int
    cgroup_memory_high_bytes: int
    cgroup_memory_max_bytes: int
    service_and_run_event_deltas_zero: bool
    accounting_complete: bool

    def __post_init__(self) -> None:
        if self.platform_kind != "linux":
            raise AuditInfrastructureError("Linux post-return sample is invalid")
        _validate_post_return_sample(self)
        if not isinstance(self.service_and_run_event_deltas_zero, bool):
            raise AuditInfrastructureError("Linux post-return events are invalid")


@dataclass(frozen=True, slots=True)
class MacOSPostReturnSample:
    platform_kind: str
    worker_slot: int
    worker_generation: int
    task_ordinal_in_generation: int
    worker_current_rss_bytes: int
    maximum_observed_owned_group_resident_bytes: int
    registered_survivors: int
    known_unreconciled_descendants: int
    accounting_complete: bool

    def __post_init__(self) -> None:
        if self.platform_kind != "macos":
            raise AuditInfrastructureError("macOS post-return sample is invalid")
        _validate_post_return_sample(self)


PlatformPostReturnSample = (
    WindowsPostReturnSample | LinuxPostReturnSample | MacOSPostReturnSample
)


@dataclass(frozen=True, slots=True)
class WindowsPhaseSnapshot:
    platform_kind: str
    phase: str
    memory: WindowsRunMemoryMeasurements
    archived_generation_count: int

    def __post_init__(self) -> None:
        _validate_phase_snapshot(self, "windows", WindowsRunMemoryMeasurements)


@dataclass(frozen=True, slots=True)
class LinuxPhaseSnapshot:
    platform_kind: str
    phase: str
    memory: LinuxRunMemoryMeasurements
    archived_generation_count: int

    def __post_init__(self) -> None:
        _validate_phase_snapshot(self, "linux", LinuxRunMemoryMeasurements)


@dataclass(frozen=True, slots=True)
class MacOSPhaseSnapshot:
    platform_kind: str
    phase: str
    memory: MacOSRunMemoryMeasurements
    archived_generation_count: int

    def __post_init__(self) -> None:
        _validate_phase_snapshot(self, "macos", MacOSRunMemoryMeasurements)


NativePhaseSnapshot = WindowsPhaseSnapshot | LinuxPhaseSnapshot | MacOSPhaseSnapshot


@dataclass(frozen=True, slots=True)
class ReferenceCalibrationEnvelope:
    runner_image: str
    python_version: str
    qt_package: str
    mingw_package: str
    worker_capacity: int
    affinity_cpu_indices: tuple[int, ...]

    def __post_init__(self) -> None:
        _validate_envelope_strings(self, ("runner_image", "python_version",
                                          "qt_package", "mingw_package"))
        if (not isinstance(self.worker_capacity, int)
                or isinstance(self.worker_capacity, bool)
                or self.worker_capacity <= 0
                or not isinstance(self.affinity_cpu_indices, tuple)
                or self.affinity_cpu_indices != tuple(range(self.worker_capacity))):
            raise AuditInfrastructureError("reference calibration envelope is invalid")


@dataclass(frozen=True, slots=True)
class LinuxCalibrationEnvelope:
    runner_image: str
    kernel_release: str
    python_version: str
    clang_version: str
    systemd_version: str
    cgroup_v2: bool
    effective_worker_capacity: int

    def __post_init__(self) -> None:
        _validate_envelope_strings(self, ("runner_image", "kernel_release",
                                          "python_version", "clang_version",
                                          "systemd_version"))
        if (not isinstance(self.cgroup_v2, bool)
                or not isinstance(self.effective_worker_capacity, int)
                or isinstance(self.effective_worker_capacity, bool)
                or self.effective_worker_capacity <= 0):
            raise AuditInfrastructureError("Linux calibration envelope is invalid")


@dataclass(frozen=True, slots=True)
class MacOSCalibrationEnvelope:
    runner_image: str
    macos_version: str
    python_version: str
    appleclang_version: str
    effective_worker_capacity: int

    def __post_init__(self) -> None:
        _validate_envelope_strings(self, ("runner_image", "macos_version",
                                          "python_version", "appleclang_version"))
        if (not isinstance(self.effective_worker_capacity, int)
                or isinstance(self.effective_worker_capacity, bool)
                or self.effective_worker_capacity <= 0):
            raise AuditInfrastructureError("macOS calibration envelope is invalid")


@dataclass(frozen=True, slots=True)
class WorkerDecisionKey:
    platform_tag: str
    compiler_digest: str
    configuration_set_digest: str
    uninspected_configuration_digest: str
    decision_engine_fingerprint: str
    effective_worker_capacity: int
    reference_envelope_digest: str
    key_digest: str

    def __post_init__(self) -> None:
        if not isinstance(self.platform_tag, str) or not self.platform_tag:
            raise AuditInfrastructureError("worker decision platform tag is invalid")
        for label, value in (
            ("compiler", self.compiler_digest),
            ("configuration set", self.configuration_set_digest),
            ("uninspected configuration", self.uninspected_configuration_digest),
            ("decision engine", self.decision_engine_fingerprint),
            ("reference envelope", self.reference_envelope_digest),
            ("key", self.key_digest),
        ):
            _validate_digest(value, f"worker decision {label}")
        if (not isinstance(self.effective_worker_capacity, int)
                or isinstance(self.effective_worker_capacity, bool)
                or self.effective_worker_capacity <= 0):
            raise AuditInfrastructureError("worker decision capacity is invalid")


@dataclass(frozen=True, slots=True)
class CalibrationRejection:
    platform_kind: str
    worker_count: int
    failure_stage: str
    failure_code: str
    cleanup_complete: bool
    surviving_owned_process_count: int


@dataclass(frozen=True, slots=True)
class WorkerCalibrationSample:
    platform_kind: str
    worker_count: int
    configuration_count: int
    elapsed_seconds: float
    memory: WindowsRunMemoryMeasurements
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    cache_bytes: int
    cache_entries: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    pilot_eligible: bool
    surviving_owned_process_count: int
    included_stages: tuple[str, ...]
    post_return_samples: tuple[WindowsPostReturnSample, ...]
    stage_p50_seconds: WorkerStageTimings
    stage_p95_seconds: WorkerStageTimings
    inspection_phase_snapshot: WindowsPhaseSnapshot
    task_phase_snapshot: WindowsPhaseSnapshot

    def __post_init__(self) -> None:
        _validate_calibration_sample(self, "windows", WindowsRunMemoryMeasurements,
                                     WindowsPostReturnSample, WindowsPhaseSnapshot)


@dataclass(frozen=True, slots=True)
class LinuxWorkerCalibrationSample:
    platform_kind: str
    worker_count: int
    configuration_count: int
    elapsed_seconds: float
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    memory: LinuxRunMemoryMeasurements
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    cache_bytes: int
    cache_entries: int
    pilot_eligible: bool
    surviving_owned_process_count: int
    included_stages: tuple[str, ...]
    post_return_samples: tuple[LinuxPostReturnSample, ...]
    stage_p50_seconds: WorkerStageTimings
    stage_p95_seconds: WorkerStageTimings
    inspection_phase_snapshot: LinuxPhaseSnapshot
    task_phase_snapshot: LinuxPhaseSnapshot

    def __post_init__(self) -> None:
        _validate_calibration_sample(self, "linux", LinuxRunMemoryMeasurements,
                                     LinuxPostReturnSample, LinuxPhaseSnapshot)


@dataclass(frozen=True, slots=True)
class MacOSWorkerCalibrationSample:
    platform_kind: str
    worker_count: int
    configuration_count: int
    elapsed_seconds: float
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    memory: MacOSRunMemoryMeasurements
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    cache_bytes: int
    cache_entries: int
    pilot_eligible: bool
    surviving_owned_process_count: int
    included_stages: tuple[str, ...]
    post_return_samples: tuple[MacOSPostReturnSample, ...]
    stage_p50_seconds: WorkerStageTimings
    stage_p95_seconds: WorkerStageTimings
    inspection_phase_snapshot: MacOSPhaseSnapshot
    task_phase_snapshot: MacOSPhaseSnapshot

    def __post_init__(self) -> None:
        _validate_calibration_sample(self, "macos", MacOSRunMemoryMeasurements,
                                     MacOSPostReturnSample, MacOSPhaseSnapshot)


@dataclass(frozen=True, slots=True)
class WorkerCountDecision:
    platform_kind: str
    artifact_schema: int
    key: WorkerDecisionKey
    selected_workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    attempted_worker_counts: tuple[int, ...]
    rejections: tuple[CalibrationRejection, ...]
    samples: tuple[WorkerCalibrationSample, ...]
    evidence_record_sha256: str


@dataclass(frozen=True, slots=True)
class LinuxWorkerCountDecision:
    platform_kind: str
    artifact_schema: int
    key: WorkerDecisionKey
    envelope: LinuxCalibrationEnvelope
    selected_workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    attempted_worker_counts: tuple[int, ...]
    rejections: tuple[CalibrationRejection, ...]
    samples: tuple[LinuxWorkerCalibrationSample, ...]
    evidence_record_sha256: str


@dataclass(frozen=True, slots=True)
class MacOSWorkerCountDecision:
    platform_kind: str
    artifact_schema: int
    key: WorkerDecisionKey
    envelope: MacOSCalibrationEnvelope
    selected_workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    attempted_worker_counts: tuple[int, ...]
    rejections: tuple[CalibrationRejection, ...]
    samples: tuple[MacOSWorkerCalibrationSample, ...]
    evidence_record_sha256: str


PlatformWorkerDecision = (
    WorkerCountDecision | LinuxWorkerCountDecision | MacOSWorkerCountDecision
)


@dataclass(frozen=True, slots=True)
class PrevalidatedWorkerDecision:
    path: Path
    platform_kind: str
    audit_engine_fingerprint: str
    uninspected_configuration_digest: str
    bounded_payload_sha256: str

    def __post_init__(self) -> None:
        if (not isinstance(self.path, Path)
                or self.platform_kind not in {"windows", "linux", "macos"}):
            raise AuditInfrastructureError("prevalidated worker decision is invalid")
        _validate_digest(self.audit_engine_fingerprint,
                         "prevalidated decision audit engine")
        _validate_digest(self.uninspected_configuration_digest,
                         "prevalidated decision configuration")
        _validate_digest(self.bounded_payload_sha256,
                         "prevalidated decision payload")


@dataclass(frozen=True, slots=True)
class WorkerRuntimeContract:
    workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    pipeline_deadline: float

    def __post_init__(self) -> None:
        if (not isinstance(self.workers, int) or isinstance(self.workers, bool)
                or self.workers <= 0
                or not isinstance(self.maximum_tasks_per_worker, int)
                or isinstance(self.maximum_tasks_per_worker, bool)
                or self.maximum_tasks_per_worker <= 0
                or not isinstance(self.recycle_rss_bytes, int)
                or isinstance(self.recycle_rss_bytes, bool)
                or self.recycle_rss_bytes < 0
                or not isinstance(self.pipeline_deadline, (int, float))
                or isinstance(self.pipeline_deadline, bool)
                or not math.isfinite(self.pipeline_deadline)):
            raise AuditInfrastructureError("worker runtime contract is invalid")


def _validate_worker_generation_identity(
    worker_index: object, generation: object, label: str
) -> None:
    if (
        not isinstance(worker_index, int)
        or isinstance(worker_index, bool)
        or worker_index < 0
        or worker_index > (1 << 32) - 1
        or not isinstance(generation, int)
        or isinstance(generation, bool)
        or generation < 0
        or generation > (1 << 64) - 1
    ):
        raise AuditInfrastructureError(f"{label} generation is invalid")


@dataclass(frozen=True, slots=True)
class WorkerCapabilitiesAccepted:
    worker_index: int
    generation: int
    capability_digests: tuple[str, ...]

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index,
            self.generation,
            "worker capability acceptance",
        )
        if (
            not isinstance(self.capability_digests, tuple)
            or not self.capability_digests
            or tuple(sorted(set(self.capability_digests)))
            != self.capability_digests
        ):
            raise AuditInfrastructureError(
                "worker capability acceptance is invalid"
            )
        for digest in self.capability_digests:
            _validate_digest(digest, "worker accepted compiler capability")


@dataclass(frozen=True, slots=True)
class WorkerEngineReady:
    worker_index: int
    generation: int
    worker_pid: int
    audit_engine_fingerprint: str
    capability_digests: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker engine readiness"
        )
        if (
            not isinstance(self.worker_pid, int)
            or isinstance(self.worker_pid, bool)
            or self.worker_pid <= 0
            or not isinstance(self.capability_digests, tuple)
            or tuple(sorted(set(self.capability_digests)))
            != self.capability_digests
        ):
            raise AuditInfrastructureError("worker engine readiness is invalid")
        _validate_digest(
            self.audit_engine_fingerprint, "worker audit engine fingerprint"
        )
        for digest in self.capability_digests:
            _validate_digest(digest, "worker compiler capability")


@dataclass(frozen=True, slots=True)
class WorkerStop:
    worker_index: int
    generation: int

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker stop"
        )


@dataclass(frozen=True, slots=True)
class WorkerStopped:
    worker_index: int
    generation: int

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker stopped"
        )


@dataclass(frozen=True, slots=True)
class WorkerRetire:
    worker_index: int
    generation: int
    reason: str

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker retirement"
        )
        if (
            not isinstance(self.reason, str)
            or not self.reason
            or len(self.reason.encode("utf-8")) > 1024
        ):
            raise AuditInfrastructureError("worker retirement reason is invalid")


@dataclass(frozen=True, slots=True)
class WorkerRetireAck:
    worker_index: int
    generation: int

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker retirement acknowledgement"
        )


@dataclass(frozen=True, slots=True)
class WorkerFailure:
    worker_index: int
    generation: int
    task_id: str | None
    diagnostic: str

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker failure"
        )
        if (
            self.task_id is not None
            and (not isinstance(self.task_id, str) or not self.task_id)
        ) or (
            not isinstance(self.diagnostic, str)
            or not self.diagnostic
            or len(self.diagnostic.encode("utf-8")) > 4096
        ):
            raise AuditInfrastructureError("worker failure diagnostic is invalid")


@dataclass(frozen=True, slots=True)
class WorkerPayloadReady:
    worker_index: int
    generation: int
    task_id: str
    configuration_digest: str
    audit_engine_fingerprint: str
    pipe_nonce: str
    serial: int
    nonce: str
    encoded_bytes: int
    encoded_sha256: str
    charged_bytes: int
    conservative_decoded_bytes: int
    conservative_retained_bytes: int
    counting_pass_peak_bytes: int
    stdout_bytes: int
    stages: WorkerStageTimings

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker payload readiness"
        )
        if (
            not isinstance(self.task_id, str)
            or not self.task_id
            or not isinstance(self.serial, int)
            or isinstance(self.serial, bool)
            or self.serial <= 0
            or self.serial > (1 << 64) - 1
            or not isinstance(self.encoded_bytes, int)
            or isinstance(self.encoded_bytes, bool)
            or self.encoded_bytes <= 0
            or self.encoded_bytes > (4 << 20)
            or not isinstance(self.charged_bytes, int)
            or isinstance(self.charged_bytes, bool)
            or self.charged_bytes <= 0
            or not isinstance(self.conservative_decoded_bytes, int)
            or isinstance(self.conservative_decoded_bytes, bool)
            or self.conservative_decoded_bytes < self.encoded_bytes
            or not isinstance(self.conservative_retained_bytes, int)
            or isinstance(self.conservative_retained_bytes, bool)
            or self.conservative_retained_bytes < 0
            or not isinstance(self.counting_pass_peak_bytes, int)
            or isinstance(self.counting_pass_peak_bytes, bool)
            or self.counting_pass_peak_bytes <= 0
            or not isinstance(self.stdout_bytes, int)
            or isinstance(self.stdout_bytes, bool)
            or self.stdout_bytes < 0
            or not isinstance(self.stages, WorkerStageTimings)
        ):
            raise AuditInfrastructureError("worker result payload is invalid")
        _validate_digest(
            self.configuration_digest, "worker result payload configuration"
        )
        _validate_digest(
            self.audit_engine_fingerprint, "worker result payload engine"
        )
        _validate_digest(self.pipe_nonce, "worker result payload pipe")
        _validate_digest(self.nonce, "worker result payload nonce")
        _validate_digest(self.encoded_sha256, "worker result payload")


@dataclass(frozen=True, slots=True)
class WorkerPayloadPermit:
    worker_index: int
    generation: int
    task_id: str

    def __post_init__(self) -> None:
        _validate_worker_generation_identity(
            self.worker_index, self.generation, "worker payload permit"
        )
        if not isinstance(self.task_id, str) or not self.task_id:
            raise AuditInfrastructureError("worker result payload permit is invalid")


@dataclass(frozen=True, slots=True)
class CompactResultPreparseBounds:
    encoded_bytes: int
    conservative_decoded_bytes: int
    conservative_retained_bytes: int

    def __post_init__(self) -> None:
        if any(
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
            for value in (
                self.encoded_bytes,
                self.conservative_decoded_bytes,
                self.conservative_retained_bytes,
            )
        ) or self.encoded_bytes > (4 << 20):
            raise AuditInfrastructureError(
                "compact result preparse bounds are invalid"
            )


@dataclass(frozen=True, slots=True)
class CompactResultTransportAllocation:
    counting_pass_peak_bytes: int
    canonical_encoder_scratch_bytes: int
    sender_payload_bytes: int
    pipe_frame_bytes: int
    pipe_kernel_capacity_bytes: int
    receiver_payload_bytes: int
    json_decoded_transient_bytes: int
    retained_result_bytes: int
    permit_reservation_bytes: int
    decode_reservation_bytes: int
    peak_pending_bytes: int

    def __post_init__(self) -> None:
        if any(
            not isinstance(getattr(self, field_info.name), int)
            or isinstance(getattr(self, field_info.name), bool)
            or getattr(self, field_info.name) < 0
            for field_info in dataclasses.fields(self)
        ):
            raise AuditInfrastructureError(
                "compact result transport allocation is invalid"
            )
        if self.permit_reservation_bytes != (
            self.canonical_encoder_scratch_bytes
            + self.sender_payload_bytes
            + self.pipe_frame_bytes
            + self.pipe_kernel_capacity_bytes
            + self.receiver_payload_bytes
        ) or self.decode_reservation_bytes != (
            self.receiver_payload_bytes
            + self.json_decoded_transient_bytes
            + self.retained_result_bytes
        ) or self.peak_pending_bytes != max(
            self.counting_pass_peak_bytes,
            self.permit_reservation_bytes,
            self.decode_reservation_bytes,
        ):
            raise AuditInfrastructureError(
                "compact result transport allocation differs"
            )


@dataclass(frozen=True, slots=True)
class CompactResultSlot:
    worst_case_live_bytes: int
    included_components: tuple[str, ...]

    def __post_init__(self) -> None:
        if (
            not isinstance(self.worst_case_live_bytes, int)
            or isinstance(self.worst_case_live_bytes, bool)
            or self.worst_case_live_bytes < 0
            or self.worst_case_live_bytes > _COMPACT_RESULT_MAXIMUM_LIVE_BYTES
            or not isinstance(self.included_components, tuple)
            or any(not isinstance(value, str) or not value
                   for value in self.included_components)
        ):
            raise AuditInfrastructureError("compact result slot is invalid")


@dataclass(frozen=True, slots=True)
class PilotRuntimeParameters:
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    pilot_eligible: bool

    @property
    def diagnostic_parameters(self) -> tuple[int, int]:
        if not self.pilot_eligible:
            return (1, 1)
        return (self.maximum_tasks_per_worker, self.recycle_rss_bytes)


def _validate_nonnegative_int_fields(value: object, label: str) -> None:
    for field_info in dataclasses.fields(value):
        candidate = getattr(value, field_info.name)
        if field_info.name in {
            "platform_kind", "accounting_complete",
            "service_and_run_event_deltas_zero",
        }:
            continue
        if (not isinstance(candidate, int) or isinstance(candidate, bool)
                or candidate < 0):
            raise AuditInfrastructureError(f"{label} is invalid")


def _validate_post_return_sample(value: object) -> None:
    _validate_nonnegative_int_fields(value, "post-return sample")
    if (getattr(value, "task_ordinal_in_generation") <= 0
            or not isinstance(getattr(value, "accounting_complete"), bool)):
        raise AuditInfrastructureError("post-return sample is invalid")


def _validate_phase_snapshot(value: object, platform_kind: str,
                             memory_type: type) -> None:
    if (getattr(value, "platform_kind") != platform_kind
            or getattr(value, "phase") not in {"inspection", "tasks"}
            or not isinstance(getattr(value, "memory"), memory_type)
            or not isinstance(getattr(value, "archived_generation_count"), int)
            or isinstance(getattr(value, "archived_generation_count"), bool)
            or getattr(value, "archived_generation_count") < 0):
        raise AuditInfrastructureError("native phase snapshot is invalid")


def _validate_envelope_strings(value: object, names: tuple[str, ...]) -> None:
    if any(not isinstance(getattr(value, name), str) or not getattr(value, name)
           for name in names):
        raise AuditInfrastructureError("calibration envelope is invalid")


def _validate_calibration_sample(value: object, platform_kind: str,
                                 memory_type: type, post_type: type,
                                 phase_type: type) -> None:
    integer_names = (
        "worker_count", "configuration_count", "inspection_probe_invocations",
        "audit_compiler_invocations", "stdout_bytes", "cache_bytes", "cache_entries",
        "maximum_tasks_per_worker", "recycle_rss_bytes",
        "surviving_owned_process_count",
    )
    if (getattr(value, "platform_kind") != platform_kind
            or any(not isinstance(getattr(value, name), int)
                   or isinstance(getattr(value, name), bool)
                   or getattr(value, name) < (1 if name in {
                       "worker_count", "configuration_count", "maximum_tasks_per_worker"
                   } else 0) for name in integer_names)
            or not isinstance(getattr(value, "elapsed_seconds"), (int, float))
            or isinstance(getattr(value, "elapsed_seconds"), bool)
            or not math.isfinite(getattr(value, "elapsed_seconds"))
            or getattr(value, "elapsed_seconds") < 0
            or not isinstance(getattr(value, "memory"), memory_type)
            or not isinstance(getattr(value, "pilot_eligible"), bool)
            or not isinstance(getattr(value, "included_stages"), tuple)
            or any(not isinstance(item, str) or not item
                   for item in getattr(value, "included_stages"))
            or not isinstance(getattr(value, "post_return_samples"), tuple)
            or any(not isinstance(item, post_type)
                   for item in getattr(value, "post_return_samples"))
            or not isinstance(getattr(value, "stage_p50_seconds"), WorkerStageTimings)
            or not isinstance(getattr(value, "stage_p95_seconds"), WorkerStageTimings)
            or not isinstance(getattr(value, "inspection_phase_snapshot"), phase_type)
            or not isinstance(getattr(value, "task_phase_snapshot"), phase_type)):
        raise AuditInfrastructureError("worker calibration sample is invalid")


@dataclass(frozen=True, slots=True)
class CompactResultDraftBounds:
    dependency_count: int
    reached_count: int
    finding_count: int
    path_utf8_bytes: int
    expression_utf8_bytes: int
    reason_utf8_bytes: int

    def __post_init__(self) -> None:
        values = (
            self.dependency_count,
            self.reached_count,
            self.finding_count,
            self.path_utf8_bytes,
            self.expression_utf8_bytes,
            self.reason_utf8_bytes,
        )
        if any(
            not isinstance(value, int) or isinstance(value, bool) or value < 0
            for value in values
        ):
            raise AuditInfrastructureError("compact result draft bounds are invalid")
        limits = AuditLimits()
        if (
            self.dependency_count > limits.compact_result_dependencies
            or self.reached_count > limits.compact_result_reached
            or self.finding_count > limits.compact_result_findings
        ):
            raise AuditInfrastructureError("compact result draft count limit exceeded")


@dataclass(frozen=True, slots=True)
class CompactResultTransportCapability:
    task_id: str
    generation: int
    configuration_digest: str
    audit_engine_fingerprint: str
    worker_slot: int
    pipe_nonce: str
    maximum_bytes: int
    serial: int
    nonce: str

    def __post_init__(self) -> None:
        if (
            not isinstance(self.task_id, str)
            or not self.task_id
            or not isinstance(self.generation, int)
            or isinstance(self.generation, bool)
            or self.generation < 0
            or self.generation > (1 << 64) - 1
            or not isinstance(self.worker_slot, int)
            or isinstance(self.worker_slot, bool)
            or self.worker_slot < 0
            or self.worker_slot > (1 << 32) - 1
            or not isinstance(self.maximum_bytes, int)
            or isinstance(self.maximum_bytes, bool)
            or self.maximum_bytes < (32 << 20)
            or not isinstance(self.serial, int)
            or isinstance(self.serial, bool)
            or self.serial <= 0
            or self.serial > (1 << 64) - 1
        ):
            raise AuditInfrastructureError("worker transport capability is invalid")
        _validate_digest(
            self.configuration_digest, "worker transport configuration"
        )
        _validate_digest(
            self.audit_engine_fingerprint, "worker transport audit engine"
        )
        _validate_digest(self.pipe_nonce, "worker transport pipe association")
        _validate_digest(self.nonce, "worker transport capability nonce")


@dataclass(frozen=True, slots=True)
class CompactResultTransportReceipt:
    task_id: str
    generation: int
    configuration_digest: str
    audit_engine_fingerprint: str
    worker_slot: int
    pipe_nonce: str
    serial: int
    nonce: str
    encoded_bytes: int
    charged_bytes: int
    payload_sha256: str
    stdout_bytes: int
    stages: WorkerStageTimings

    def __post_init__(self) -> None:
        if (
            not isinstance(self.task_id, str)
            or not self.task_id
            or not isinstance(self.generation, int)
            or isinstance(self.generation, bool)
            or self.generation < 0
            or self.generation > (1 << 64) - 1
            or not isinstance(self.worker_slot, int)
            or isinstance(self.worker_slot, bool)
            or self.worker_slot < 0
            or self.worker_slot > (1 << 32) - 1
            or not isinstance(self.serial, int)
            or isinstance(self.serial, bool)
            or self.serial <= 0
            or self.serial > (1 << 64) - 1
            or not isinstance(self.encoded_bytes, int)
            or isinstance(self.encoded_bytes, bool)
            or self.encoded_bytes <= 0
            or self.encoded_bytes > (1 << 64) - 1
            or not isinstance(self.charged_bytes, int)
            or isinstance(self.charged_bytes, bool)
            or self.charged_bytes <= 0
            or self.charged_bytes > (1 << 64) - 1
            or not isinstance(self.stdout_bytes, int)
            or isinstance(self.stdout_bytes, bool)
            or self.stdout_bytes < 0
            or self.stdout_bytes > (1 << 64) - 1
            or not isinstance(self.stages, WorkerStageTimings)
        ):
            raise AuditInfrastructureError("worker transport receipt is invalid")
        _validate_digest(self.configuration_digest, "worker transport configuration")
        _validate_digest(
            self.audit_engine_fingerprint, "worker transport audit engine"
        )
        _validate_digest(self.pipe_nonce, "worker transport pipe association")
        _validate_digest(self.nonce, "worker transport receipt nonce")
        _validate_digest(self.payload_sha256, "worker transport receipt payload")


@dataclass(frozen=True, slots=True)
class ConfigurationAuditResultTransport:
    payload: bytes
    receipt: CompactResultTransportReceipt

    def __post_init__(self) -> None:
        if not isinstance(self.payload, bytes) or not isinstance(
            self.receipt, CompactResultTransportReceipt
        ):
            raise AuditInfrastructureError("compact audit result transport is invalid")


class PerTaskCompactReservation:
    """Linear parent-issued reservation that gates one cold worker miss."""

    _ENCODED_ENVELOPE_BYTES = 4 << 20
    _RETAINED_ENVELOPE_BYTES = 16 << 20
    _COUNTING_WORKSPACE_BYTES = 4096
    _TRANSPORT_OVERHEAD_BYTES = 8192
    _RECEIVER_DECODE_ENCODED_MULTIPLIER = 3

    __slots__ = (
        "task_id", "generation", "maximum_bytes", "_charged_bytes",
        "_peak_bytes", "_reserved_encoded_bytes", "_canonical_json_bytes", "_owner_phase",
        "_owner_serial", "_worker_transport_nonce", "_worker_transport_serial",
        "_worker_transport_configuration_digest", "_worker_transport_engine",
        "_worker_transport_worker_slot", "_worker_transport_pipe_nonce",
        "_worker_transport_mirror", "_released", "_release_phase", "_lock",
    )

    def __init__(self, task_id: str, generation: int, maximum_bytes: int = 32 << 20) -> None:
        if (
            not isinstance(task_id, str)
            or not task_id
            or not isinstance(generation, int)
            or isinstance(generation, bool)
            or generation < 0
            or not isinstance(maximum_bytes, int)
            or isinstance(maximum_bytes, bool)
            or maximum_bytes <= 0
            or maximum_bytes > (128 << 20)
        ):
            raise AuditInfrastructureError("pre-dispatch compact reservation is invalid")
        self.task_id = task_id
        self.generation = generation
        self.maximum_bytes = maximum_bytes
        self._charged_bytes = 0
        self._peak_bytes = 0
        self._reserved_encoded_bytes = 0
        self._canonical_json_bytes = 0
        self._owner_phase = "parent-pre-dispatch"
        self._owner_serial = 0
        self._worker_transport_nonce: str | None = None
        self._worker_transport_serial = 0
        self._worker_transport_configuration_digest: str | None = None
        self._worker_transport_engine: str | None = None
        self._worker_transport_worker_slot: int | None = None
        self._worker_transport_pipe_nonce: str | None = None
        self._worker_transport_mirror = False
        self._released = False
        self._release_phase: str | None = None
        self._lock = threading.Lock()

    @property
    def released(self) -> bool:
        with self._lock:
            return self._released

    @property
    def charged_bytes(self) -> int:
        with self._lock:
            return self._charged_bytes

    @property
    def peak_bytes(self) -> int:
        with self._lock:
            return self._peak_bytes

    @property
    def release_phase(self) -> str | None:
        with self._lock:
            return self._release_phase

    @property
    def canonical_json_bytes(self) -> int:
        with self._lock:
            return self._canonical_json_bytes

    @property
    def owner_phase(self) -> str:
        with self._lock:
            return self._owner_phase

    @property
    def worker_transport_capability(self) -> CompactResultTransportCapability:
        with self._lock:
            if (
                not self._worker_transport_mirror
                or self._worker_transport_nonce is None
                or self._worker_transport_serial <= 0
                or self._worker_transport_configuration_digest is None
                or self._worker_transport_engine is None
                or self._worker_transport_worker_slot is None
                or self._worker_transport_pipe_nonce is None
            ):
                raise AuditInfrastructureError(
                    "worker transport capability is unavailable"
                )
            return CompactResultTransportCapability(
                self.task_id,
                self.generation,
                self._worker_transport_configuration_digest,
                self._worker_transport_engine,
                self._worker_transport_worker_slot,
                self._worker_transport_pipe_nonce,
                self.maximum_bytes,
                self._worker_transport_serial,
                self._worker_transport_nonce,
            )

    def issue_worker_transport_capability(
        self,
        task_id: str,
        generation: int,
        configuration_digest: str,
        audit_engine_fingerprint: str,
        worker_slot: int,
    ) -> CompactResultTransportCapability:
        _validate_digest(configuration_digest, "worker transport configuration")
        _validate_digest(
            audit_engine_fingerprint, "worker transport audit engine"
        )
        if (
            not isinstance(worker_slot, int)
            or isinstance(worker_slot, bool)
            or worker_slot < 0
            or worker_slot > (1 << 32) - 1
        ):
            raise AuditInfrastructureError("worker transport slot is invalid")
        valid_envelope_bytes = (
            self._TRANSPORT_OVERHEAD_BYTES
            + self._ENCODED_ENVELOPE_BYTES
            + self._COUNTING_WORKSPACE_BYTES
            + self._RETAINED_ENVELOPE_BYTES
        )
        with self._lock:
            if (
                self._released
                or self._worker_transport_mirror
                or task_id != self.task_id
                or generation != self.generation
                or self._owner_phase != "parent-pre-dispatch"
                or self.maximum_bytes < (32 << 20)
                or valid_envelope_bytes > self.maximum_bytes
            ):
                raise AuditInfrastructureError(
                    "pre-dispatch compact reservation cannot issue worker transport capability"
                )
            self._worker_transport_serial += 1
            self._worker_transport_nonce = os.urandom(32).hex()
            self._worker_transport_configuration_digest = configuration_digest
            self._worker_transport_engine = audit_engine_fingerprint
            self._worker_transport_worker_slot = worker_slot
            self._worker_transport_pipe_nonce = os.urandom(32).hex()
            self._owner_phase = "worker-transport-dispatched"
            serial = self._worker_transport_serial
            nonce = self._worker_transport_nonce
        return CompactResultTransportCapability(
            task_id,
            generation,
            configuration_digest,
            audit_engine_fingerprint,
            worker_slot,
            self._worker_transport_pipe_nonce,
            self.maximum_bytes,
            serial,
            nonce,
        )

    @classmethod
    def for_worker_transport(
        cls, capability: CompactResultTransportCapability
    ) -> "PerTaskCompactReservation":
        if not isinstance(capability, CompactResultTransportCapability):
            raise AuditInfrastructureError("worker transport capability is invalid")
        reservation = cls(
            capability.task_id, capability.generation, capability.maximum_bytes
        )
        reservation._worker_transport_nonce = capability.nonce
        reservation._worker_transport_serial = capability.serial
        reservation._worker_transport_configuration_digest = (
            capability.configuration_digest
        )
        reservation._worker_transport_engine = capability.audit_engine_fingerprint
        reservation._worker_transport_worker_slot = capability.worker_slot
        reservation._worker_transport_pipe_nonce = capability.pipe_nonce
        reservation._worker_transport_mirror = True
        return reservation

    def complete_worker_transport(
        self,
        capability: CompactResultTransportCapability,
        payload: bytes,
        stdout_bytes: int,
        stages: WorkerStageTimings,
    ) -> CompactResultTransportReceipt:
        if (
            not isinstance(capability, CompactResultTransportCapability)
            or not isinstance(payload, bytes)
            or not isinstance(stdout_bytes, int)
            or isinstance(stdout_bytes, bool)
            or stdout_bytes < 0
            or not isinstance(stages, WorkerStageTimings)
        ):
            raise AuditInfrastructureError("worker transport completion is invalid")
        with self._lock:
            if (
                self._released
                or not self._worker_transport_mirror
                or capability.task_id != self.task_id
                or capability.generation != self.generation
                or capability.maximum_bytes != self.maximum_bytes
                or capability.configuration_digest
                != self._worker_transport_configuration_digest
                or capability.audit_engine_fingerprint
                != self._worker_transport_engine
                or capability.worker_slot != self._worker_transport_worker_slot
                or capability.pipe_nonce != self._worker_transport_pipe_nonce
                or capability.serial != self._worker_transport_serial
                or capability.nonce != self._worker_transport_nonce
                or self._owner_phase != "serialized-pipe"
                or len(payload) != self._canonical_json_bytes
                or self._charged_bytes <= 0
            ):
                raise AuditInfrastructureError("worker transport completion differs")
            receipt = CompactResultTransportReceipt(
                self.task_id,
                self.generation,
                capability.configuration_digest,
                capability.audit_engine_fingerprint,
                capability.worker_slot,
                capability.pipe_nonce,
                capability.serial,
                capability.nonce,
                len(payload),
                self._charged_bytes,
                hashlib.sha256(payload).hexdigest(),
                stdout_bytes,
                stages,
            )
            self._released = True
            self._owner_phase = "released"
            self._release_phase = "worker-handoff-sent"
        return receipt

    def begin_receiver_transport_decode(
        self,
        capability: CompactResultTransportCapability,
        receipt: CompactResultTransportReceipt,
        payload: bytes,
        authenticated_charge_bytes: int,
    ) -> "CompactResultReservationOwnership":
        if (
            not isinstance(capability, CompactResultTransportCapability)
            or not isinstance(receipt, CompactResultTransportReceipt)
            or not isinstance(payload, bytes)
            or not isinstance(authenticated_charge_bytes, int)
            or isinstance(authenticated_charge_bytes, bool)
            or authenticated_charge_bytes <= 0
        ):
            raise AuditInfrastructureError("worker transport authentication failed")
        with self._lock:
            if self._released:
                raise AuditInfrastructureError("compact reservation was already released")
            if self._owner_phase != "worker-transport-dispatched":
                raise AuditInfrastructureError("compact audit result transport was already consumed")
            authentic = self._worker_transport_envelope_matches(
                capability, receipt, payload
            )
            if not authentic:
                self._released = True
                self._owner_phase = "released"
                self._release_phase = "receiver-transport-rejected"
                raise AuditInfrastructureError(
                    "compact audit result transport authentication failed"
                )
            if (
                receipt.charged_bytes != authenticated_charge_bytes
                or authenticated_charge_bytes > self.maximum_bytes
            ):
                self._released = True
                self._owner_phase = "released"
                self._release_phase = "receiver-transport-rejected"
                raise AuditInfrastructureError(
                    "compact audit result transport charge authentication failed"
                )
            self._charged_bytes = receipt.charged_bytes
            self._peak_bytes = max(self._peak_bytes, receipt.charged_bytes)
            self._reserved_encoded_bytes = receipt.encoded_bytes
            self._canonical_json_bytes = receipt.encoded_bytes
            self._owner_serial += 1
            self._owner_phase = "receiver-decode"
            serial = self._owner_serial
        return CompactResultReservationOwnership(self, serial, "receiver-decode")

    def authenticate_worker_transport_envelope(
        self,
        capability: CompactResultTransportCapability,
        receipt: CompactResultTransportReceipt,
        payload: bytes,
    ) -> None:
        if (
            not isinstance(capability, CompactResultTransportCapability)
            or not isinstance(receipt, CompactResultTransportReceipt)
            or not isinstance(payload, bytes)
        ):
            raise AuditInfrastructureError("worker transport authentication failed")
        with self._lock:
            if self._released:
                raise AuditInfrastructureError("compact reservation was already released")
            if self._owner_phase != "worker-transport-dispatched":
                raise AuditInfrastructureError(
                    "compact audit result transport was already consumed"
                )
            if not self._worker_transport_envelope_matches(
                capability, receipt, payload
            ):
                self._released = True
                self._owner_phase = "released"
                self._release_phase = "receiver-transport-rejected"
                raise AuditInfrastructureError(
                    "compact audit result transport authentication failed"
                )

    def _worker_transport_envelope_matches(
        self,
        capability: CompactResultTransportCapability,
        receipt: CompactResultTransportReceipt,
        payload: bytes,
    ) -> bool:
        return (
            not self._worker_transport_mirror
            and capability.task_id == self.task_id
            and capability.generation == self.generation
            and capability.maximum_bytes == self.maximum_bytes
            and capability.configuration_digest
            == self._worker_transport_configuration_digest
            and capability.audit_engine_fingerprint
            == self._worker_transport_engine
            and capability.worker_slot == self._worker_transport_worker_slot
            and capability.pipe_nonce == self._worker_transport_pipe_nonce
            and capability.serial == self._worker_transport_serial
            and capability.nonce == self._worker_transport_nonce
            and receipt.task_id == capability.task_id
            and receipt.generation == capability.generation
            and receipt.configuration_digest == capability.configuration_digest
            and receipt.audit_engine_fingerprint
            == capability.audit_engine_fingerprint
            and receipt.worker_slot == capability.worker_slot
            and receipt.pipe_nonce == capability.pipe_nonce
            and receipt.serial == capability.serial
            and receipt.nonce == capability.nonce
            and receipt.encoded_bytes == len(payload)
            and receipt.encoded_bytes <= self._ENCODED_ENVELOPE_BYTES
            and receipt.payload_sha256 == hashlib.sha256(payload).hexdigest()
        )

    def release_worker_transport_capability(
        self, capability: CompactResultTransportCapability, phase: str
    ) -> None:
        if not isinstance(capability, CompactResultTransportCapability):
            raise AuditInfrastructureError("worker transport capability is invalid")
        if not isinstance(phase, str) or not phase:
            raise AuditInfrastructureError("compact reservation release phase is invalid")
        with self._lock:
            if self._released:
                raise AuditInfrastructureError("compact reservation was already released")
            if (
                self._worker_transport_mirror
                or self._owner_phase != "worker-transport-dispatched"
                or capability.task_id != self.task_id
                or capability.generation != self.generation
                or capability.maximum_bytes != self.maximum_bytes
                or capability.configuration_digest
                != self._worker_transport_configuration_digest
                or capability.audit_engine_fingerprint
                != self._worker_transport_engine
                or capability.worker_slot != self._worker_transport_worker_slot
                or capability.pipe_nonce != self._worker_transport_pipe_nonce
                or capability.serial != self._worker_transport_serial
                or capability.nonce != self._worker_transport_nonce
            ):
                raise AuditInfrastructureError("worker transport capability differs")
            self._released = True
            self._charged_bytes = 0
            self._owner_phase = "released"
            self._release_phase = phase

    def require_before_discovery(self, task_id: str, generation: int) -> None:
        valid_envelope_bytes = (
            self._TRANSPORT_OVERHEAD_BYTES
            + self._ENCODED_ENVELOPE_BYTES
            + self._COUNTING_WORKSPACE_BYTES
            + self._RETAINED_ENVELOPE_BYTES
        )
        with self._lock:
            if (
                self._released
                or task_id != self.task_id
                or generation != self.generation
                or self.maximum_bytes < (32 << 20)
                or valid_envelope_bytes > self.maximum_bytes
            ):
                raise AuditInfrastructureError(
                    "pre-dispatch compact reservation is inadequate"
                )
            self._owner_phase = "worker-audit"

    def validate(self, task_id: str, generation: int) -> None:
        with self._lock:
            if (
                self._released
                or task_id != self.task_id
                or generation != self.generation
            ):
                raise AuditInfrastructureError("worker dispatch reservation differs")

    def require_within_pre_dispatch_reservation(
        self,
        task_id: str,
        encoded_bytes: int,
        bounds: CompactResultDraftBounds,
        counting_pass_peak_bytes: int,
    ) -> int:
        if (
            task_id != self.task_id
            or not isinstance(encoded_bytes, int)
            or isinstance(encoded_bytes, bool)
            or encoded_bytes < 0
            or not isinstance(bounds, CompactResultDraftBounds)
            or not isinstance(counting_pass_peak_bytes, int)
            or isinstance(counting_pass_peak_bytes, bool)
            or counting_pass_peak_bytes < 0
        ):
            raise AuditInfrastructureError("pre-dispatch compact reservation differs")
        charged = self.exact_transport_charge(
            encoded_bytes, bounds, counting_pass_peak_bytes
        )
        with self._lock:
            if (
                self._released
                or charged > self.maximum_bytes
            ):
                raise AuditInfrastructureError(
                    "pre-dispatch compact reservation limit exceeded"
                )
            self._charged_bytes = max(self._charged_bytes, charged)
            self._peak_bytes = max(self._peak_bytes, self._charged_bytes)
            self._reserved_encoded_bytes = encoded_bytes
        return charged

    @classmethod
    def exact_transport_charge(
        cls,
        encoded_bytes: int,
        bounds: CompactResultDraftBounds,
        counting_pass_peak_bytes: int,
    ) -> int:
        if (
            not isinstance(encoded_bytes, int)
            or isinstance(encoded_bytes, bool)
            or encoded_bytes < 0
            or not isinstance(bounds, CompactResultDraftBounds)
            or not isinstance(counting_pass_peak_bytes, int)
            or isinstance(counting_pass_peak_bytes, bool)
            or counting_pass_peak_bytes < 0
        ):
            raise AuditInfrastructureError(
                "compact result transport charge input is invalid"
            )
        retained_bytes = sum((
            1024,
            bounds.dependency_count * 384,
            bounds.reached_count * 128,
            bounds.finding_count * 320,
            bounds.path_utf8_bytes,
            bounds.expression_utf8_bytes,
            bounds.reason_utf8_bytes,
        ))
        charged = sum((
            cls._TRANSPORT_OVERHEAD_BYTES,
            encoded_bytes,
            counting_pass_peak_bytes,
            retained_bytes,
            cls._RECEIVER_DECODE_ENCODED_MULTIPLIER * encoded_bytes,
        ))
        if charged > (1 << 63) - 1:
            raise AuditInfrastructureError(
                "pre-dispatch compact reservation arithmetic overflow"
            )
        if (
            encoded_bytes > cls._ENCODED_ENVELOPE_BYTES
            or counting_pass_peak_bytes > cls._COUNTING_WORKSPACE_BYTES
            or retained_bytes > cls._RETAINED_ENVELOPE_BYTES
            or (
                bounds.path_utf8_bytes
                + bounds.expression_utf8_bytes
                + bounds.reason_utf8_bytes
            ) > encoded_bytes
        ):
            raise AuditInfrastructureError(
                "pre-dispatch compact reservation limit exceeded"
            )
        return charged

    def record_exact_canonical_json(self, task_id: str, encoded_bytes: int) -> None:
        if (
            task_id != self.task_id
            or not isinstance(encoded_bytes, int)
            or isinstance(encoded_bytes, bool)
            or encoded_bytes <= 0
        ):
            raise AuditInfrastructureError("canonical compact result charge is invalid")
        with self._lock:
            if (
                self._released
                or self._charged_bytes <= 0
                or encoded_bytes != self._reserved_encoded_bytes
            ):
                raise AuditInfrastructureError(
                    "canonical compact result was not reserved"
                )
            self._canonical_json_bytes = encoded_bytes
            self._owner_phase = "worker-bounded-draft"

    def begin_result_ownership(
        self, task_id: str, generation: int
    ) -> "CompactResultReservationOwnership":
        with self._lock:
            if (
                self._released
                or task_id != self.task_id
                or generation != self.generation
                or self._canonical_json_bytes <= 0
                or self._owner_phase != "worker-bounded-draft"
            ):
                raise AuditInfrastructureError(
                    "compact reservation result ownership differs"
                )
            self._owner_serial += 1
            self._owner_phase = "worker-materialized-result"
            serial = self._owner_serial
        return CompactResultReservationOwnership(
            self, serial, "worker-materialized-result"
        )

    def _transfer_result_ownership(
        self, serial: int, source_phase: str, target_phase: str
    ) -> "CompactResultReservationOwnership":
        allowed = {
            "worker-materialized-result": "serialized-pipe",
            "serialized-pipe": "receiver-decode",
            "receiver-decode": "receiver-retained-result",
        }
        with self._lock:
            if (
                self._released
                or serial != self._owner_serial
                or source_phase != self._owner_phase
                or allowed.get(source_phase) != target_phase
            ):
                raise AuditInfrastructureError(
                    "compact result ownership transfer differs"
                )
            self._owner_serial += 1
            self._owner_phase = target_phase
            next_serial = self._owner_serial
        return CompactResultReservationOwnership(
            self, next_serial, target_phase
        )

    def _rebind_result_ownership(
        self, serial: int, source_phase: str
    ) -> "CompactResultReservationOwnership":
        with self._lock:
            if (
                self._released
                or serial != self._owner_serial
                or source_phase != self._owner_phase
                or source_phase != "worker-materialized-result"
            ):
                raise AuditInfrastructureError(
                    "compact result ownership rebind differs"
                )
            self._owner_serial += 1
            next_serial = self._owner_serial
        return CompactResultReservationOwnership(
            self, next_serial, source_phase
        )

    def _release_result_ownership(
        self, serial: int, source_phase: str, release_phase: str
    ) -> None:
        if not isinstance(release_phase, str) or not release_phase:
            raise AuditInfrastructureError("compact reservation release phase is invalid")
        with self._lock:
            if (
                self._released
                or serial != self._owner_serial
                or source_phase != self._owner_phase
            ):
                raise AuditInfrastructureError(
                    "compact result ownership was transferred"
                )
            self._released = True
            self._owner_phase = "released"
            self._release_phase = release_phase

    def release(self, phase: str) -> None:
        if not isinstance(phase, str) or not phase:
            raise AuditInfrastructureError("compact reservation release phase is invalid")
        with self._lock:
            if self._released:
                raise AuditInfrastructureError("compact reservation was already released")
            self._released = True
            self._charged_bytes = 0
            self._owner_phase = "released"
            self._release_phase = phase


class CompactResultReservationOwnership:
    """Linear phase token carrying one pre-dispatch reservation."""

    __slots__ = ("_reservation", "_serial", "phase", "_active")

    def __init__(
        self, reservation: PerTaskCompactReservation, serial: int, phase: str
    ) -> None:
        self._reservation = reservation
        self._serial = serial
        self.phase = phase
        self._active = True

    @property
    def active(self) -> bool:
        return self._active

    @property
    def reservation(self) -> PerTaskCompactReservation:
        return self._reservation

    def transfer(self, target_phase: str) -> "CompactResultReservationOwnership":
        if not self._active:
            raise AuditInfrastructureError("compact result ownership was transferred")
        replacement = self._reservation._transfer_result_ownership(
            self._serial, self.phase, target_phase
        )
        self._active = False
        return replacement

    def rebind_materialized_result(self) -> "CompactResultReservationOwnership":
        if not self._active:
            raise AuditInfrastructureError("compact result ownership was transferred")
        replacement = self._reservation._rebind_result_ownership(
            self._serial, self.phase
        )
        self._active = False
        return replacement

    def release(self, release_phase: str) -> None:
        if not self._active:
            raise AuditInfrastructureError("compact result ownership was transferred")
        self._reservation._release_result_ownership(
            self._serial, self.phase, release_phase
        )
        self._active = False


_COMPACT_RESULT_MAXIMUM_LIVE_BYTES = 128 * 1024 * 1024


class CompactAccountingObserver:
    """Run-scoped logical compact-byte transition ledger."""

    __slots__ = (
        "_live_bytes", "_peak_live_bytes", "_events", "_semantic_events",
        "_owners", "_owner_budgets", "_budgets", "_next_budget_id",
        "_next_owner_id", "_semantic_transaction_id",
        "_semantic_transaction_events", "_lock"
    )

    _SEMANTIC_EVENTS = frozenset({
        "retain-hit",
        "reserve-dispatch",
        "activate-publication",
        "release-publication",
        "send-pipe",
        "decode",
        "retain-result",
        "release-result",
    })

    def __init__(self) -> None:
        self._live_bytes = 0
        self._peak_live_bytes = 0
        self._events: list[tuple[str, int, str]] = []
        self._semantic_events: list[tuple[str, int, str, int]] = []
        self._owners: dict[int, tuple[int, str, int, str, str]] = {}
        self._owner_budgets: dict[int, int] = {}
        self._budgets: set[int] = set()
        self._next_budget_id = 0
        self._next_owner_id = 0
        self._semantic_transaction_id = 0
        self._semantic_transaction_events: list[
            tuple[str, int, str, int]
        ] | None = None
        self._lock = threading.Lock()

    @property
    def live_bytes(self) -> int:
        with self._lock:
            return self._live_bytes

    @property
    def peak_live_bytes(self) -> int:
        with self._lock:
            return self._peak_live_bytes

    @property
    def events(self) -> tuple[tuple[str, int, str], ...]:
        with self._lock:
            return tuple(self._events)

    @property
    def semantic_events(self) -> tuple[tuple[str, int, str, int], ...]:
        with self._lock:
            return tuple(self._semantic_events)

    def register_budget(self) -> int:
        with self._lock:
            self._next_budget_id += 1
            budget_id = self._next_budget_id
            self._budgets.add(budget_id)
            return budget_id

    def begin_semantic_transaction(self) -> int:
        with self._lock:
            if self._semantic_transaction_events is not None:
                raise AuditInfrastructureError(
                    "compact accounting semantic transaction overlaps"
                )
            self._semantic_transaction_id += 1
            self._semantic_transaction_events = []
            return self._semantic_transaction_id

    def finish_semantic_transaction(self, transaction_id: int, *, commit: bool) -> None:
        if (
            not isinstance(transaction_id, int)
            or isinstance(transaction_id, bool)
            or transaction_id <= 0
            or not isinstance(commit, bool)
        ):
            raise AuditInfrastructureError(
                "compact accounting semantic transaction is invalid"
            )
        with self._lock:
            pending = self._semantic_transaction_events
            if (
                pending is None
                or transaction_id != self._semantic_transaction_id
            ):
                raise AuditInfrastructureError(
                    "compact accounting semantic transaction is unavailable"
                )
            if commit:
                self._semantic_events.extend(pending)
            self._semantic_transaction_events = None

    def _append_semantic_locked(
        self, event: tuple[str, int, str, int]
    ) -> None:
        pending = self._semantic_transaction_events
        if pending is None:
            self._semantic_events.append(event)
        else:
            pending.append(event)

    def allocate_owner(self, budget_id: int) -> int:
        if (
            not isinstance(budget_id, int)
            or isinstance(budget_id, bool)
            or budget_id <= 0
        ):
            raise AuditInfrastructureError(
                "compact accounting budget identity is invalid"
            )
        with self._lock:
            if budget_id not in self._budgets:
                raise AuditInfrastructureError(
                    "compact accounting budget identity is unavailable"
                )
            self._next_owner_id += 1
            owner_id = self._next_owner_id
            self._owner_budgets[owner_id] = budget_id
            return owner_id

    def discard_unobserved_owner(self, budget_id: int, owner_id: int) -> None:
        with self._lock:
            if owner_id in self._owners:
                raise AuditInfrastructureError(
                    "compact accounting semantic owner is already active"
                )
            if self._owner_budgets.get(owner_id) != budget_id:
                raise AuditInfrastructureError(
                    "compact accounting semantic owner budget differs"
                )
            del self._owner_budgets[owner_id]

    def transition(
        self,
        event: str,
        delta_bytes: int,
        label: str,
        *,
        owner_id: int,
        previous_owner_id: int | None = None,
        budget_id: int | None = None,
        semantic_event: str | None = None,
    ) -> None:
        if (
            event not in {"reserve", "commit", "release", "replace"}
            or not isinstance(delta_bytes, int)
            or isinstance(delta_bytes, bool)
            or not isinstance(label, str)
            or not label
            or not isinstance(owner_id, int)
            or isinstance(owner_id, bool)
            or owner_id <= 0
            or (
                budget_id is not None
                and (
                    not isinstance(budget_id, int)
                    or isinstance(budget_id, bool)
                    or budget_id <= 0
                )
            )
            or (
                semantic_event is not None
                and semantic_event not in self._SEMANTIC_EVENTS
            )
            or (
                previous_owner_id is not None
                and (
                    not isinstance(previous_owner_id, int)
                    or isinstance(previous_owner_id, bool)
                    or previous_owner_id <= 0
                )
            )
        ):
            raise AuditInfrastructureError(
                "compact accounting transition is invalid"
            )
        with self._lock:
            owner_budget = self._owner_budgets.get(owner_id)
            if budget_id is None:
                budget_id = owner_budget
            if (
                budget_id not in self._budgets
                or owner_budget != budget_id
                or (
                    previous_owner_id is not None
                    and self._owner_budgets.get(previous_owner_id) != budget_id
                )
            ):
                raise AuditInfrastructureError(
                    "compact accounting cross-budget semantic owner differs"
                )
            if event == "reserve":
                if owner_id in self._owners or previous_owner_id is not None:
                    raise AuditInfrastructureError(
                        "compact accounting semantic owner overlaps"
                    )
                if semantic_event not in {None, "reserve-dispatch"}:
                    raise AuditInfrastructureError(
                        "compact accounting semantic transition differs"
                    )
                owner_bytes = delta_bytes
                owner_state = "reserved"
                owner_phase = (
                    "dispatch-reserved"
                    if semantic_event == "reserve-dispatch"
                    else "plain"
                )
            elif event == "commit":
                current = self._owners.get(owner_id)
                if (
                    current is None
                    or current[3] != "reserved"
                    or delta_bytes != 0
                    or previous_owner_id is not None
                    or semantic_event is not None
                ):
                    raise AuditInfrastructureError(
                        "compact accounting semantic owner is unavailable"
                    )
                owner_bytes = current[0]
                owner_state = "committed"
                owner_phase = current[4]
            elif event == "release":
                current = self._owners.get(owner_id)
                if (
                    current is None
                    or delta_bytes != -current[0]
                    or previous_owner_id is not None
                    or (
                        semantic_event is not None
                        and (
                            semantic_event != "release-result"
                            or current[3] != "committed"
                            or current[4] not in {
                                "hit-retained", "result-retained"
                            }
                        )
                    )
                ):
                    raise AuditInfrastructureError(
                        "compact accounting semantic owner underflows or transition differs"
                    )
                owner_bytes = 0
            else:
                previous = self._owners.get(previous_owner_id)
                if (
                    previous is None
                    or previous[3] != "committed"
                    or owner_id in self._owners
                    or owner_id == previous_owner_id
                ):
                    raise AuditInfrastructureError(
                        "compact accounting semantic owner replacement overlaps"
                    )
                owner_bytes = previous[0] + delta_bytes
                if owner_bytes < 0:
                    raise AuditInfrastructureError(
                        "compact accounting semantic owner replacement underflows"
                    )
                if semantic_event is None:
                    owner_phase = previous[4]
                elif semantic_event == "decode" and previous[4] == "plain":
                    owner_phase = "decode"
                elif (
                    semantic_event == "retain-result"
                    and previous[4] == "decode"
                ):
                    owner_phase = "result-retained"
                else:
                    raise AuditInfrastructureError(
                        "compact accounting semantic replacement transition differs"
                    )
                owner_state = "committed"
            next_live = self._live_bytes + delta_bytes
            if next_live < 0 or next_live > _COMPACT_RESULT_MAXIMUM_LIVE_BYTES:
                raise AuditInfrastructureError(
                    "compact accounting ownership overlaps or underflows"
                )
            if event == "release":
                del self._owners[owner_id]
                del self._owner_budgets[owner_id]
            elif event == "replace":
                del self._owners[previous_owner_id]
                del self._owner_budgets[previous_owner_id]
                self._owners[owner_id] = (
                    owner_bytes, label, budget_id, owner_state, owner_phase
                )
            elif event == "reserve":
                self._owners[owner_id] = (
                    owner_bytes, label, budget_id, owner_state, owner_phase
                )
            elif event == "commit":
                self._owners[owner_id] = (
                    owner_bytes, label, budget_id, owner_state, owner_phase
                )
            self._live_bytes = next_live
            self._peak_live_bytes = max(self._peak_live_bytes, next_live)
            self._events.append((event, delta_bytes, label))
            if semantic_event is not None:
                self._append_semantic_locked(
                    (semantic_event, delta_bytes, label, owner_id)
                )

    def record_semantic(
        self,
        semantic_event: str,
        delta_bytes: int,
        label: str,
        *,
        owner_id: int,
        budget_id: int,
    ) -> None:
        if (
            semantic_event not in self._SEMANTIC_EVENTS
            or not isinstance(delta_bytes, int)
            or isinstance(delta_bytes, bool)
            or not isinstance(label, str)
            or not label
            or not isinstance(owner_id, int)
            or isinstance(owner_id, bool)
            or owner_id <= 0
            or not isinstance(budget_id, int)
            or isinstance(budget_id, bool)
            or budget_id <= 0
        ):
            raise AuditInfrastructureError(
                "compact accounting semantic transition is invalid"
            )
        with self._lock:
            current = self._owners.get(owner_id)
            allowed = {
                ("plain", "retain-hit"): "hit-retained",
                (
                    "dispatch-reserved", "activate-publication"
                ): "publication-active",
                (
                    "publication-active", "release-publication"
                ): "publication-released",
                ("publication-released", "send-pipe"): "pipe-sent",
            }
            next_phase = (
                None
                if current is None
                else allowed.get((current[4], semantic_event))
            )
            expected_delta = (
                current[0]
                if current is not None and semantic_event == "retain-hit"
                else 0
            )
            if (
                current is None
                or current[3] != "committed"
                or next_phase is None
                or current[2] != budget_id
                or self._owner_budgets.get(owner_id) != budget_id
                or delta_bytes != expected_delta
                or any(
                    event == semantic_event and event_owner == owner_id
                    for event, _delta, _label, event_owner
                    in self._semantic_events
                )
            ):
                raise AuditInfrastructureError(
                    "compact accounting semantic owner is unavailable"
                )
            self._owners[owner_id] = (
                current[0], current[1], current[2], current[3], next_phase
            )
            self._append_semantic_locked(
                (semantic_event, delta_bytes, label, owner_id)
            )


class CompactResultMemoryBudget:
    """Exact single-owner accounting for compact audit objects and indices."""

    def __init__(
        self,
        maximum_bytes: int = _COMPACT_RESULT_MAXIMUM_LIVE_BYTES,
        *,
        observer: CompactAccountingObserver | None = None,
    ) -> None:
        if (
            not isinstance(maximum_bytes, int)
            or isinstance(maximum_bytes, bool)
            or maximum_bytes < 0
            or maximum_bytes > _COMPACT_RESULT_MAXIMUM_LIVE_BYTES
        ):
            raise AuditInfrastructureError("compact result memory limit is invalid")
        if observer is not None and not isinstance(
            observer, CompactAccountingObserver
        ):
            raise AuditInfrastructureError("compact accounting observer is invalid")
        self.maximum_bytes = maximum_bytes
        self.observer = observer
        self._reserved_bytes = 0
        self._committed_bytes = 0
        self._peak_live_bytes = 0
        self._lock = threading.Lock()
        self._next_owner_id = 0
        self._budget_id = (
            0 if observer is None else observer.register_budget()
        )

    @property
    def budget_id(self) -> int:
        return self._budget_id

    @property
    def reserved_bytes(self) -> int:
        with self._lock:
            return self._reserved_bytes

    @property
    def committed_bytes(self) -> int:
        with self._lock:
            return self._committed_bytes

    @property
    def live_bytes(self) -> int:
        with self._lock:
            return self._reserved_bytes + self._committed_bytes

    @property
    def peak_live_bytes(self) -> int:
        with self._lock:
            return self._peak_live_bytes

    def reserve(
        self,
        byte_count: int,
        *,
        label: str = "compact result",
        semantic_event: str | None = None,
    ) -> "CompactResultOwnership":
        if (
            not isinstance(byte_count, int)
            or isinstance(byte_count, bool)
            or byte_count < 0
            or not isinstance(label, str)
            or not label
        ):
            raise AuditInfrastructureError("compact result reservation is invalid")
        with self._lock:
            live = self._reserved_bytes + self._committed_bytes
            if byte_count > self.maximum_bytes - live:
                raise AuditInfrastructureError(
                    f"{label} exceeds aggregate 128 MiB compact result limit"
                )
            self._reserved_bytes += byte_count
            if self.observer is None:
                self._next_owner_id += 1
                owner_id = self._next_owner_id
            else:
                owner_id = self.observer.allocate_owner(self._budget_id)
            self._peak_live_bytes = max(
                self._peak_live_bytes,
                self._reserved_bytes + self._committed_bytes,
            )
        if self.observer is not None:
            try:
                self.observer.transition(
                    "reserve", byte_count, label, owner_id=owner_id,
                    budget_id=self._budget_id,
                    semantic_event=semantic_event,
                )
            except BaseException:
                with self._lock:
                    self._reserved_bytes -= byte_count
                self.observer.discard_unobserved_owner(
                    self._budget_id, owner_id
                )
                raise
        return CompactResultOwnership(
            self, byte_count, "reserved", label, owner_id
        )

    def _commit(self, byte_count: int, label: str, owner_id: int) -> None:
        with self._lock:
            if byte_count > self._reserved_bytes:
                raise AuditInfrastructureError("compact result reservation accounting underflow")
            self._reserved_bytes -= byte_count
            self._committed_bytes += byte_count
        if self.observer is not None:
            try:
                self.observer.transition(
                    "commit", 0, label, owner_id=owner_id,
                    budget_id=self._budget_id,
                )
            except BaseException:
                with self._lock:
                    self._reserved_bytes += byte_count
                    self._committed_bytes -= byte_count
                raise

    def _record_semantic(
        self,
        label: str,
        owner_id: int,
        semantic_event: str,
        delta_bytes: int,
    ) -> None:
        if self.observer is not None:
            self.observer.record_semantic(
                semantic_event,
                delta_bytes,
                label,
                owner_id=owner_id,
                budget_id=self._budget_id,
            )

    def _release(
        self, byte_count: int, state: str, label: str, owner_id: int,
        semantic_event: str | None = None,
    ) -> None:
        with self._lock:
            if state == "reserved":
                if byte_count > self._reserved_bytes:
                    raise AuditInfrastructureError(
                        "compact result reservation accounting underflow"
                    )
                self._reserved_bytes -= byte_count
            elif state == "committed":
                if byte_count > self._committed_bytes:
                    raise AuditInfrastructureError(
                        "compact result ownership accounting underflow"
                    )
                self._committed_bytes -= byte_count
            else:
                raise AuditInfrastructureError("compact result ownership state is invalid")
        if self.observer is not None:
            try:
                self.observer.transition(
                    "release", -byte_count, label, owner_id=owner_id,
                    budget_id=self._budget_id,
                    semantic_event=semantic_event,
                )
            except BaseException:
                with self._lock:
                    if state == "reserved":
                        self._reserved_bytes += byte_count
                    else:
                        self._committed_bytes += byte_count
                raise

    def _replace_committed(
        self,
        old_bytes: int,
        new_bytes: int,
        label: str,
        old_label: str,
        old_owner_id: int,
        semantic_event: str | None = None,
    ) -> int:
        with self._lock:
            if old_bytes > self._committed_bytes:
                raise AuditInfrastructureError(
                    "compact result ownership accounting underflow"
                )
            live_without_old = (
                self._reserved_bytes + self._committed_bytes - old_bytes
            )
            if new_bytes > self.maximum_bytes - live_without_old:
                raise AuditInfrastructureError(
                    "replacement exceeds aggregate 128 MiB compact result limit"
                )
            self._committed_bytes += new_bytes - old_bytes
            if self.observer is None:
                self._next_owner_id += 1
                new_owner_id = self._next_owner_id
            else:
                new_owner_id = self.observer.allocate_owner(self._budget_id)
            self._peak_live_bytes = max(
                self._peak_live_bytes,
                self._reserved_bytes + self._committed_bytes,
            )
        if self.observer is not None:
            try:
                self.observer.transition(
                    "replace",
                    new_bytes - old_bytes,
                    label,
                    owner_id=new_owner_id,
                    previous_owner_id=old_owner_id,
                    budget_id=self._budget_id,
                    semantic_event=semantic_event,
                )
            except BaseException:
                with self._lock:
                    self._committed_bytes -= new_bytes - old_bytes
                self.observer.discard_unobserved_owner(
                    self._budget_id, new_owner_id
                )
                raise
        return new_owner_id


class CompactResultOwnership:
    """Linear ownership token; commit and release are each permitted once."""

    __slots__ = (
        "_budget", "byte_count", "_state", "label", "_owner_id"
    )

    def __init__(
        self,
        budget: CompactResultMemoryBudget,
        byte_count: int,
        state: str,
        label: str,
        owner_id: int,
    ) -> None:
        self._budget = budget
        self.byte_count = byte_count
        self._state = state
        self.label = label
        self._owner_id = owner_id

    @property
    def budget(self) -> CompactResultMemoryBudget:
        return self._budget

    @property
    def owner_id(self) -> int:
        return self._owner_id

    @property
    def committed(self) -> bool:
        return self._state == "committed"

    @property
    def released(self) -> bool:
        return self._state == "released"

    def commit(self) -> "CompactResultOwnership":
        if self._state != "reserved":
            raise AuditInfrastructureError("compact result ownership cannot be committed")
        self._budget._commit(self.byte_count, self.label, self._owner_id)
        self._state = "committed"
        return self

    def record_semantic(
        self, semantic_event: str, *, delta_bytes: int = 0
    ) -> None:
        if self._state != "committed":
            raise AuditInfrastructureError(
                "compact result semantic ownership is unavailable"
            )
        self._budget._record_semantic(
            self.label, self._owner_id, semantic_event, delta_bytes
        )

    def release(self, *, semantic_event: str | None = None) -> None:
        if self._state == "released":
            raise AuditInfrastructureError("compact result ownership was already released")
        self._budget._release(
            self.byte_count, self._state, self.label, self._owner_id,
            semantic_event,
        )
        self._state = "released"

    def replace_committed(
        self, byte_count: int, *, label: str,
        semantic_event: str | None = None,
    ) -> "CompactResultOwnership":
        if (
            self._state != "committed"
            or not isinstance(byte_count, int)
            or isinstance(byte_count, bool)
            or byte_count < 0
            or not isinstance(label, str)
            or not label
        ):
            raise AuditInfrastructureError(
                "compact result ownership replacement is invalid"
            )
        new_owner_id = self._budget._replace_committed(
            self.byte_count,
            byte_count,
            label,
            self.label,
            self._owner_id,
            semantic_event,
        )
        self._state = "released"
        return CompactResultOwnership(
            self._budget, byte_count, "committed", label, new_owner_id
        )

    def __del__(self) -> None:
        try:
            if self._state != "released":
                self._budget._release(
                    self.byte_count,
                    self._state,
                    self.label,
                    self._owner_id,
                )
                self._state = "released"
        except Exception:
            pass


@dataclass(frozen=True, slots=True)
class CompactResultColdSlot:
    worst_case_live_bytes: int

    def __post_init__(self) -> None:
        if (
            not isinstance(self.worst_case_live_bytes, int)
            or isinstance(self.worst_case_live_bytes, bool)
            or self.worst_case_live_bytes < 0
            or self.worst_case_live_bytes > _COMPACT_RESULT_MAXIMUM_LIVE_BYTES
        ):
            raise AuditInfrastructureError("compact result cold slot is invalid")


@dataclass(frozen=True, slots=True)
class CompactResultGrowth:
    configuration_index_bytes: int
    coverage_index_bytes: int
    finding_index_bytes: int
    digest_state_bytes: int

    def __post_init__(self) -> None:
        if any(
            not isinstance(value, int) or isinstance(value, bool) or value < 0
            for value in (
                self.configuration_index_bytes,
                self.coverage_index_bytes,
                self.finding_index_bytes,
                self.digest_state_bytes,
            )
        ):
            raise AuditInfrastructureError("compact result aggregate growth is invalid")

    @property
    def total_bytes(self) -> int:
        return (
            self.configuration_index_bytes
            + self.coverage_index_bytes
            + self.finding_index_bytes
            + self.digest_state_bytes
        )


@dataclass(frozen=True)
class FileIdentity:
    canonical: Path
    relative: PurePosixPath | None
    device: int | None
    inode: int | None
    line_count: int
    production: bool


@dataclass(frozen=True, slots=True)
class DependencyRootBinding:
    stable_role: str
    resolved_root: Path
    root_identity: FileIdentity

    def __post_init__(self) -> None:
        if not isinstance(self.stable_role, str):
            raise AuditInfrastructureError("dependency root role is invalid")
        if not isinstance(self.resolved_root, Path):
            raise AuditInfrastructureError("dependency root path is invalid")
        if not isinstance(self.root_identity, FileIdentity):
            raise AuditInfrastructureError("dependency root identity is invalid")


@dataclass(frozen=True, slots=True)
class DependencyRootAuthority:
    source_root: DependencyRootBinding
    external_roots: tuple[DependencyRootBinding, ...]
    portable_authority_digest: str

    def __post_init__(self) -> None:
        _validate_authority_structure(self)


@dataclass(frozen=True, slots=True, init=False)
class CompilerInspection:
    compiler_family: CompilerFamily
    executable_identity: FileIdentity
    executable_sha256: str
    normalized_version: str
    driver_fingerprint: str
    inspection_arguments_digest: str
    executable_capability_digest: str

    def __init__(
        self,
        compiler_family: CompilerFamily,
        executable_identity: FileIdentity,
        executable_sha256: str,
        normalized_version: str,
        driver_fingerprint: str,
        inspection_arguments_digest: str,
        executable_capability_digest: str,
        *,
        validation_deadline: float | None = None,
        cancel_event: object | None = None,
        held_executable_identity: FileIdentity | None = None,
        held_executable_sha256: str | None = None,
    ) -> None:
        for name, value in (
            ("compiler_family", compiler_family),
            ("executable_identity", executable_identity),
            ("executable_sha256", executable_sha256),
            ("normalized_version", normalized_version),
            ("driver_fingerprint", driver_fingerprint),
            ("inspection_arguments_digest", inspection_arguments_digest),
            ("executable_capability_digest", executable_capability_digest),
        ):
            object.__setattr__(self, name, value)
        self._validate(
            validation_deadline,
            cancel_event,
            held_executable_identity,
            held_executable_sha256,
        )

    def _validate(
        self,
        validation_deadline: float | None,
        cancel_event: object | None,
        held_executable_identity: FileIdentity | None,
        held_executable_sha256: str | None,
    ) -> None:
        if not isinstance(self.compiler_family, CompilerFamily):
            raise AuditInfrastructureError("compiler inspection family is invalid")
        if (held_executable_identity is None) != (held_executable_sha256 is None):
            raise AuditInfrastructureError("compiler inspection held capability is invalid")
        if held_executable_identity is None:
            _validate_current_executable_identity(self.executable_identity)
        else:
            _validate_executable_identity(held_executable_identity)
            _validate_digest(
                held_executable_sha256, "compiler inspection held capability content"
            )
            if self.executable_identity != held_executable_identity:
                raise _HeldCompilerCapabilityMismatch(
                    "compiler inspection held capability identity differs"
                )
        _validate_digest(self.executable_sha256, "compiler executable content")
        if held_executable_sha256 is None:
            if _current_executable_sha256(
                self.executable_identity, validation_deadline, cancel_event
            ) != self.executable_sha256:
                raise AuditInfrastructureError("compiler executable content changed")
        elif self.executable_sha256 != held_executable_sha256:
            raise _HeldCompilerCapabilityMismatch(
                "compiler inspection held capability content differs"
            )
        if (
            not isinstance(self.normalized_version, str)
            or not self.normalized_version
            or "\0" in self.normalized_version
            or "\r" in self.normalized_version
            or self.normalized_version != self.normalized_version.strip()
            or any(line != line.rstrip() for line in self.normalized_version.split("\n"))
        ):
            raise AuditInfrastructureError("compiler normalized version is invalid")
        _validate_digest(self.driver_fingerprint, "compiler driver fingerprint")
        _validate_digest(
            self.inspection_arguments_digest, "compiler inspection arguments"
        )
        _validate_digest(
            self.executable_capability_digest, "compiler executable capability"
        )


@dataclass(frozen=True, slots=True)
class CompilerExecutableCapability:
    platform_kind: str
    executable_identity: FileIdentity
    executable_sha256: str
    capability_digest: str
    native_owner: object = field(compare=False, repr=False)
    trusted_toolchain_root: DependencyRootBinding
    directory_chain_owners: tuple[object, ...] = field(compare=False, repr=False)
    directory_chain_identities: tuple[FileIdentity, ...]
    resolved_runtime_closure_digest: str
    resolved_runtime_closure: tuple["DependencyDigest", ...]

    def __post_init__(self) -> None:
        if self.platform_kind not in {"windows", "linux", "macos"}:
            raise AuditInfrastructureError("compiler capability platform is invalid")
        _validate_executable_identity(self.executable_identity)
        _validate_digest(self.executable_sha256, "compiler executable content")
        _validate_digest(self.capability_digest, "compiler executable capability")
        if not isinstance(self.trusted_toolchain_root, DependencyRootBinding):
            raise AuditInfrastructureError("compiler trusted toolchain root is invalid")
        if not isinstance(self.directory_chain_owners, tuple) or not isinstance(
            self.directory_chain_identities, tuple
        ):
            raise AuditInfrastructureError("compiler directory chain is invalid")
        _validate_digest(
            self.resolved_runtime_closure_digest, "compiler runtime closure"
        )
        if not isinstance(self.resolved_runtime_closure, tuple) or any(
            not isinstance(item, DependencyDigest)
            for item in self.resolved_runtime_closure
        ):
            raise AuditInfrastructureError("compiler runtime closure is invalid")


@dataclass(frozen=True, slots=True)
class PreprocessConfiguration:
    entry_id: str
    family: CompilerFamily
    compiler: Path
    working_directory: Path
    source: FileIdentity
    arguments: tuple[str, ...]
    environment_digest: str
    digest: str
    dependency_root_authority_digest: str
    compiler_capability_digest: str
    compiler_capability: CompilerExecutableCapability = field(compare=False, repr=False)

    def __post_init__(self) -> None:
        _validate_digest(
            self.dependency_root_authority_digest, "dependency root authority"
        )
        _validate_digest(self.compiler_capability_digest, "compiler capability")
        if not isinstance(self.compiler_capability, CompilerExecutableCapability):
            raise AuditInfrastructureError("compiler executable capability is invalid")
        if self.compiler_capability.capability_digest != self.compiler_capability_digest:
            raise AuditInfrastructureError("compiler capability digest disagrees")


@dataclass(frozen=True, slots=True)
class ConfigurationAuditTask:
    task_id: str
    generation: int
    configuration: PreprocessConfiguration
    dependency_root_authority: DependencyRootAuthority
    compact_reservation: PerTaskCompactReservation | None

    def __post_init__(self) -> None:
        if (
            not isinstance(self.task_id, str)
            or not self.task_id
            or not isinstance(self.generation, int)
            or isinstance(self.generation, bool)
            or self.generation < 0
            or not isinstance(self.configuration, PreprocessConfiguration)
            or not isinstance(self.dependency_root_authority, DependencyRootAuthority)
        ):
            raise AuditInfrastructureError("configuration audit task is invalid")
        if (
            self.compact_reservation is not None
            and not isinstance(self.compact_reservation, PerTaskCompactReservation)
        ):
            raise AuditInfrastructureError("worker dispatch reservation is invalid")


@dataclass(frozen=True, slots=True)
class SourceLocation:
    identity: FileIdentity | None
    inclusion_instance: int
    line: int
    configuration_digest: str


@dataclass(frozen=True, slots=True)
class PreprocessedToken:
    spelling: bytes
    location: SourceLocation


@dataclass(frozen=True, slots=True)
class PackedTokenRun:
    start: int
    stop: int
    identity_id: int
    inclusion_instance: int
    original_line: int


@dataclass(frozen=True)
class PreprocessedTranslationUnitView:
    configuration: PreprocessConfiguration
    tokens: "CompactTokenSequence"
    dependencies: tuple[FileIdentity, ...]


@dataclass(frozen=True)
class CoverageReport:
    authoritative: frozenset[PurePosixPath]
    source_only: frozenset[PurePosixPath]
    configurations: tuple[str, ...]


def _validate_uint32(value: int, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0 or value > UINT32_MAX:
        raise AuditInfrastructureError(f"{label} must be an unsigned 32-bit integer")
    return value


def _current_process_rss_bytes() -> int:
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class _ProcessMemoryCounters(ctypes.Structure):
            _fields_ = (
                ("cb", wintypes.DWORD),
                ("page_fault_count", wintypes.DWORD),
                ("peak_working_set_size", ctypes.c_size_t),
                ("working_set_size", ctypes.c_size_t),
                ("quota_peak_paged_pool_usage", ctypes.c_size_t),
                ("quota_paged_pool_usage", ctypes.c_size_t),
                ("quota_peak_non_paged_pool_usage", ctypes.c_size_t),
                ("quota_non_paged_pool_usage", ctypes.c_size_t),
                ("pagefile_usage", ctypes.c_size_t),
                ("peak_pagefile_usage", ctypes.c_size_t),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.GetCurrentProcess.argtypes = ()
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        psapi.GetProcessMemoryInfo.argtypes = (
            wintypes.HANDLE,
            ctypes.POINTER(_ProcessMemoryCounters),
            wintypes.DWORD,
        )
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        counters = _ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(
            kernel32.GetCurrentProcess(),
            ctypes.byref(counters),
            counters.cb,
        ):
            raise AuditInfrastructureError("cannot sample coordinator RSS")
        return int(counters.working_set_size)

    proc_statm = Path("/proc/self/statm")
    if proc_statm.is_file():
        try:
            resident_pages = int(proc_statm.read_text(encoding="ascii").split()[1])
            return resident_pages * int(os.sysconf("SC_PAGE_SIZE"))
        except (OSError, ValueError, IndexError) as error:
            raise AuditInfrastructureError("cannot sample coordinator RSS") from error

    try:
        import resource

        maximum_rss = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    except (ImportError, OSError, ValueError) as error:
        raise AuditInfrastructureError("cannot sample coordinator RSS") from error
    return maximum_rss if sys.platform == "darwin" else maximum_rss * 1024


def _copy_packed_column(column: array) -> array:
    return array("I", column)


_PACKED_COLUMN_SLOTS = (
    "_spelling_ids",
    "_identity_ids",
    "_inclusion_ids",
    "_original_lines",
)
_SET_ENTRY_RSS_RESERVE = 128
_EMPTY_SET_RSS_BYTES = sys.getsizeof(set())
_EMPTY_ARRAY_RSS_BYTES = sys.getsizeof(array("I"))
_COLUMN_LIST_RSS_BYTES = sys.getsizeof([None, None, None, None])
_ARRAY_ALLOCATION_RSS_SLACK = 4096
_TUPLE_SLOT_BYTES = sys.getsizeof((None,)) - sys.getsizeof(())


def _checked_rss(
    rss_reader: Callable[[], int],
    limit: int,
    *,
    reserve: int = 0,
) -> int:
    rss = rss_reader()
    if not isinstance(rss, int) or isinstance(rss, bool) or rss < 0:
        raise AuditInfrastructureError("coordinator RSS sample is invalid")
    if rss > limit or reserve > limit - rss:
        raise AuditInfrastructureError("coordinator RSS limit exceeded")
    return rss


def _validate_interned_table(
    values: tuple[object, ...],
    label: str,
    *,
    rss_reader: Callable[[], int],
    rss_limit: int,
) -> None:
    if not values:
        return
    _checked_rss(
        rss_reader,
        rss_limit,
        reserve=_EMPTY_SET_RSS_BYTES
        + len(values) * _SET_ENTRY_RSS_RESERVE,
    )
    unique = set(values)
    _checked_rss(rss_reader, rss_limit)
    if len(unique) != len(values):
        raise AuditInfrastructureError(f"token {label} table is not interned")


def _retained_table_bytes(
    spellings: tuple[bytes, ...],
    identities: tuple[FileIdentity | None, ...],
) -> int:
    return (
        sum(len(spelling) for spelling in spellings)
        + (len(spellings) + len(identities)) * _TUPLE_SLOT_BYTES
    )


class _ReadOnlyPackedColumn(Sequence[int]):
    """Non-aliasing proxy over one fresh read-only memoryview."""

    __slots__ = ("_view",)
    itemsize = 4
    format = "I"
    readonly = True

    def __init__(self, column: array) -> None:
        object.__setattr__(self, "_view", memoryview(column).toreadonly())

    def __getattribute__(self, name: str) -> object:
        if name == "_view":
            raise AttributeError("packed view internals are private")
        return object.__getattribute__(self, name)

    def __len__(self) -> int:
        view = object.__getattribute__(self, "_view")
        return len(view)

    @overload
    def __getitem__(self, index: int) -> int:
        ...

    @overload
    def __getitem__(self, index: slice) -> tuple[int, ...]:
        ...

    def __getitem__(self, index: int | slice) -> int | tuple[int, ...]:
        view = object.__getattribute__(self, "_view")
        if isinstance(index, slice):
            return tuple(view[index])
        return int(view[index])

    def release(self) -> None:
        view = object.__getattribute__(self, "_view")
        view.release()


class CompactTokenSequence(Sequence[PreprocessedToken]):
    """Packed token columns with immutable token views materialized on demand."""

    __slots__ = (
        "_configuration",
        "_spellings",
        "_identities",
        "_spelling_ids",
        "_identity_ids",
        "_inclusion_ids",
        "_original_lines",
    )

    def __setattr__(self, name: str, value: object) -> None:
        raise AttributeError("CompactTokenSequence is immutable")

    def __delattr__(self, name: str) -> None:
        raise AttributeError("CompactTokenSequence is immutable")

    def __getattribute__(self, name: str) -> object:
        if name in _PACKED_COLUMN_SLOTS:
            column = object.__getattribute__(self, name)
            return _ReadOnlyPackedColumn(column)
        return object.__getattribute__(self, name)

    def __init__(
        self,
        configuration: PreprocessConfiguration,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        spelling_ids: array,
        identity_ids: array,
        inclusion_ids: array,
        original_lines: array,
        *,
        limits: AuditLimits = AuditLimits(),
        rss_reader: Callable[[], int] = _current_process_rss_bytes,
        _adopt_columns: bool = False,
    ) -> None:
        _checked_rss(rss_reader, limits.rss_bytes)
        supplied_columns = (
            spelling_ids,
            identity_ids,
            inclusion_ids,
            original_lines,
        )
        if any(
            not isinstance(column, array) or column.typecode != "I"
            for column in supplied_columns
        ):
            raise AuditInfrastructureError("packed token columns must use array('I')")
        if array("I").itemsize != 4 or any(
            column.itemsize != 4 for column in supplied_columns
        ):
            raise AuditInfrastructureError(
                "packed token columns require four-byte array('I') items"
            )
        lengths = {len(column) for column in supplied_columns}
        if len(lengths) != 1:
            raise AuditInfrastructureError("packed token columns have different lengths")
        if any(not isinstance(spelling, bytes) for spelling in spellings):
            raise AuditInfrastructureError("token spellings must be bytes")
        if len(spellings) > UINT32_MAX + 1 or len(identities) > UINT32_MAX + 1:
            raise AuditInfrastructureError("token intern table exceeds unsigned 32-bit IDs")
        if any(spelling_id >= len(spellings) for spelling_id in supplied_columns[0]):
            raise AuditInfrastructureError(
                "packed spelling ID does not exist in the spelling table"
            )
        if any(identity_id >= len(identities) for identity_id in supplied_columns[1]):
            raise AuditInfrastructureError(
                "packed identity ID does not exist in the identity table"
            )

        column_bytes = sum(
            len(column) * column.itemsize for column in supplied_columns
        )
        retained_bytes = column_bytes + _retained_table_bytes(
            spellings, identities
        )
        if retained_bytes > limits.retained_token_bytes:
            raise AuditInfrastructureError("retained packed token limit exceeded")
        _validate_interned_table(
            spellings,
            "spelling",
            rss_reader=rss_reader,
            rss_limit=limits.rss_bytes,
        )
        _validate_interned_table(
            identities,
            "identity",
            rss_reader=rss_reader,
            rss_limit=limits.rss_bytes,
        )

        _checked_rss(
            rss_reader, limits.rss_bytes, reserve=_COLUMN_LIST_RSS_BYTES
        )
        columns: list[array] = []
        for column in supplied_columns:
            if _adopt_columns:
                columns.append(column)
            else:
                _checked_rss(
                    rss_reader,
                    limits.rss_bytes,
                    reserve=_EMPTY_ARRAY_RSS_BYTES
                    + len(column) * column.itemsize
                    + _ARRAY_ALLOCATION_RSS_SLACK,
                )
                columns.append(_copy_packed_column(column))
            _checked_rss(rss_reader, limits.rss_bytes)
        object.__setattr__(self, "_configuration", configuration)
        object.__setattr__(self, "_spellings", spellings)
        object.__setattr__(self, "_identities", identities)
        object.__setattr__(self, "_spelling_ids", columns[0])
        object.__setattr__(self, "_identity_ids", columns[1])
        object.__setattr__(self, "_inclusion_ids", columns[2])
        object.__setattr__(self, "_original_lines", columns[3])
        _checked_rss(rss_reader, limits.rss_bytes)

    @classmethod
    def empty(cls, configuration: PreprocessConfiguration) -> "CompactTokenSequence":
        return cls(
            configuration,
            (),
            (),
            array("I"),
            array("I"),
            array("I"),
            array("I"),
        )

    @classmethod
    def _from_packed(
        cls,
        configuration: PreprocessConfiguration,
        *,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        spelling_ids: array,
        identity_ids: array,
        inclusion_ids: array,
        original_lines: array,
        limits: AuditLimits = AuditLimits(),
        rss_reader: Callable[[], int] = _current_process_rss_bytes,
    ) -> "CompactTokenSequence":
        return cls(
            configuration,
            spellings,
            identities,
            spelling_ids,
            identity_ids,
            inclusion_ids,
            original_lines,
            limits=limits,
            rss_reader=rss_reader,
        )

    @classmethod
    def _from_owned_packed(
        cls,
        configuration: PreprocessConfiguration,
        *,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        spelling_ids: array,
        identity_ids: array,
        inclusion_ids: array,
        original_lines: array,
        limits: AuditLimits = AuditLimits(),
        rss_reader: Callable[[], int] = _current_process_rss_bytes,
    ) -> "CompactTokenSequence":
        """Consume uniquely-owned packed columns without a second full copy."""

        return cls(
            configuration,
            spellings,
            identities,
            spelling_ids,
            identity_ids,
            inclusion_ids,
            original_lines,
            limits=limits,
            rss_reader=rss_reader,
            _adopt_columns=True,
        )

    @classmethod
    def _from_token_fields(
        cls,
        configuration: PreprocessConfiguration,
        *,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        fields: Iterable[tuple[int, int, int, int]],
    ) -> "CompactTokenSequence":
        spelling_ids = array("I")
        identity_ids = array("I")
        inclusion_ids = array("I")
        original_lines = array("I")
        for spelling_id, identity_id, inclusion_id, original_line in fields:
            values = (
                _validate_uint32(spelling_id, "spelling ID"),
                _validate_uint32(identity_id, "identity ID"),
                _validate_uint32(inclusion_id, "inclusion instance"),
                _validate_uint32(original_line, "original line"),
            )
            if values[0] >= len(spellings):
                raise AuditInfrastructureError("packed spelling ID does not exist in the spelling table")
            if values[1] >= len(identities):
                raise AuditInfrastructureError("packed identity ID does not exist in the identity table")
            spelling_ids.append(values[0])
            identity_ids.append(values[1])
            inclusion_ids.append(values[2])
            original_lines.append(values[3])
        return cls._from_packed(
            configuration,
            spellings=spellings,
            identities=identities,
            spelling_ids=spelling_ids,
            identity_ids=identity_ids,
            inclusion_ids=inclusion_ids,
            original_lines=original_lines,
        )

    def __len__(self) -> int:
        return len(object.__getattribute__(self, "_spelling_ids"))

    def _normalize_index(self, index: int) -> int:
        if index < 0:
            index += len(self)
        if index < 0 or index >= len(self):
            raise IndexError("compact token index out of range")
        return index

    def _token_at(self, index: int) -> PreprocessedToken:
        index = self._normalize_index(index)
        spelling_ids = object.__getattribute__(self, "_spelling_ids")
        identity_ids = object.__getattribute__(self, "_identity_ids")
        inclusion_ids = object.__getattribute__(self, "_inclusion_ids")
        original_lines = object.__getattribute__(self, "_original_lines")
        location = SourceLocation(
            identity=self._identities[identity_ids[index]],
            inclusion_instance=inclusion_ids[index],
            line=original_lines[index],
            configuration_digest=self._configuration.digest,
        )
        return PreprocessedToken(self._spellings[spelling_ids[index]], location)

    @overload
    def __getitem__(self, index: int) -> PreprocessedToken:
        ...

    @overload
    def __getitem__(self, index: slice) -> list[PreprocessedToken]:
        ...

    def __getitem__(self, index: int | slice) -> PreprocessedToken | list[PreprocessedToken]:
        if isinstance(index, slice):
            return [self._token_at(position) for position in range(*index.indices(len(self)))]
        if not isinstance(index, int) or isinstance(index, bool):
            raise TypeError("compact token indices must be integers or slices")
        return self._token_at(index)

    def __iter__(self) -> Iterator[PreprocessedToken]:
        for index in range(len(self)):
            yield self._token_at(index)

    @property
    def packed_bytes(self) -> int:
        return sum(
            len(object.__getattribute__(self, slot))
            * object.__getattribute__(self, slot).itemsize
            for slot in (
                "_spelling_ids",
                "_identity_ids",
                "_inclusion_ids",
                "_original_lines",
            )
        )

    def spelling_id_at(self, index: int) -> int:
        column = object.__getattribute__(self, "_spelling_ids")
        return int(column[self._normalize_index(index)])

    def spelling_for(self, spelling_id: int) -> bytes:
        if spelling_id < 0 or spelling_id >= len(self._spellings):
            raise IndexError("spelling ID out of range")
        return self._spellings[spelling_id]

    def identity_for(self, identity_id: int) -> FileIdentity | None:
        if identity_id < 0 or identity_id >= len(self._identities):
            raise IndexError("identity ID out of range")
        return self._identities[identity_id]

    def iter_runs(self) -> Iterator[PackedTokenRun]:
        size = len(self)
        identity_ids = object.__getattribute__(self, "_identity_ids")
        inclusion_ids = object.__getattribute__(self, "_inclusion_ids")
        original_lines = object.__getattribute__(self, "_original_lines")
        start = 0
        while start < size:
            identity_id = identity_ids[start]
            inclusion_instance = inclusion_ids[start]
            original_line = original_lines[start]
            stop = start + 1
            while (
                stop < size
                and identity_ids[stop] == identity_id
                and inclusion_ids[stop] == inclusion_instance
                and original_lines[stop] == original_line
            ):
                stop += 1
            yield PackedTokenRun(
                start,
                stop,
                identity_id,
                inclusion_instance,
                original_line,
            )
            start = stop

    def _packed_columns(
        self,
    ) -> tuple[
        _ReadOnlyPackedColumn,
        _ReadOnlyPackedColumn,
        _ReadOnlyPackedColumn,
        _ReadOnlyPackedColumn,
    ]:
        return tuple(
            _ReadOnlyPackedColumn(object.__getattribute__(self, slot))
            for slot in _PACKED_COLUMN_SLOTS
        )


def _update_semantic_bytes(digest, value: bytes) -> None:
    digest.update(struct.pack("<Q", len(value)))
    digest.update(value)


def _identity_semantic_bytes(identity: FileIdentity | None) -> bytes:
    if identity is None:
        return b"null"
    return json.dumps(
        {
            "canonical": str(identity.canonical),
            "relative": str(identity.relative) if identity.relative is not None else None,
            "device": identity.device,
            "inode": identity.inode,
            "line_count": identity.line_count,
            "production": identity.production,
        },
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")


def _preprocessed_view_semantic_digest(
    view: PreprocessedTranslationUnitView,
) -> str:
    """Bind one full packed view without materializing token objects."""

    digest = hashlib.sha256()
    configuration = view.configuration
    _update_semantic_bytes(
        digest,
        json.dumps(
            {
                "digest": configuration.digest,
                "family": configuration.family.value,
                "compiler": str(configuration.compiler),
                "working_directory": str(configuration.working_directory),
                "source": _identity_semantic_bytes(configuration.source).decode("ascii"),
                "arguments": configuration.arguments,
                "environment_digest": configuration.environment_digest,
            },
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("ascii"),
    )
    tokens = view.tokens
    spellings = object.__getattribute__(tokens, "_spellings")
    identities = object.__getattribute__(tokens, "_identities")
    digest.update(struct.pack("<Q", len(spellings)))
    for spelling in spellings:
        _update_semantic_bytes(digest, spelling)
    digest.update(struct.pack("<Q", len(identities)))
    for identity in identities:
        _update_semantic_bytes(digest, _identity_semantic_bytes(identity))
    for slot in _PACKED_COLUMN_SLOTS:
        column = object.__getattribute__(tokens, slot)
        byte_view = memoryview(column).cast("B")
        digest.update(struct.pack("<Q", len(byte_view)))
        for offset in range(0, len(byte_view), 64 * 1024):
            digest.update(byte_view[offset : offset + 64 * 1024])
        byte_view.release()
    digest.update(struct.pack("<Q", len(view.dependencies)))
    for dependency in view.dependencies:
        _update_semantic_bytes(digest, _identity_semantic_bytes(dependency))
    return digest.hexdigest()


def requires_compile_entry(
    path: PurePosixPath,
    configured_families: frozenset[CompilerFamily],
    has_objcpp: bool,
    has_windows_backend: bool,
) -> bool:
    """Return whether this production translation unit is active in the supplied build."""

    suffix = path.suffix.casefold()
    if suffix not in _TRANSLATION_UNIT_SUFFIXES or not configured_families:
        return False

    parts = tuple(part.casefold() for part in path.parts)
    stem = path.stem.casefold()
    apple_specific = suffix == ".mm" or stem.endswith("_apple")
    windows_specific = "win" in parts[:-1] or stem.endswith(("_win", "_mediafoundation"))
    if apple_specific and not has_objcpp:
        return False
    if windows_specific and not has_windows_backend:
        return False
    return True


def _display_relative(path: Path, root: Path) -> PurePosixPath:
    try:
        return PurePosixPath(path.relative_to(root).as_posix())
    except ValueError:
        return PurePosixPath(path.as_posix())


def _is_reparse(stat_result: os.stat_result) -> bool:
    attributes = getattr(stat_result, "st_file_attributes", 0)
    return bool(attributes & _REPARSE_ATTRIBUTE)


def _check_casefold_collision(
    relative: PurePosixPath,
    seen: dict[str, PurePosixPath],
) -> None:
    folded = relative.as_posix().casefold()
    previous = seen.get(folded)
    if previous is not None and previous != relative:
        raise AuditInfrastructureError(
            f"{relative}: case-fold collision with production path {previous}"
        )
    seen[folded] = relative


def _walk_production_entries(directory: Path, root: Path) -> Iterator[tuple[Path, os.stat_result]]:
    try:
        entries = sorted(os.scandir(directory), key=lambda entry: entry.name.casefold())
    except OSError as error:
        relative = _display_relative(directory, root)
        raise AuditInfrastructureError(f"{relative}: cannot enumerate production path: {error}") from error

    for entry in entries:
        path = Path(entry.path)
        relative = _display_relative(path, root)
        try:
            # DirEntry.stat() on Windows may omit the volume/file index fields;
            # os.stat() supplies the stable identity needed to detect hard links.
            metadata = os.stat(path, follow_symlinks=False)
        except OSError as error:
            raise AuditInfrastructureError(f"{relative}: cannot inspect production path: {error}") from error
        if entry.is_symlink():
            raise AuditInfrastructureError(f"{relative}: production path crosses a symlink")
        if _is_reparse(metadata):
            raise AuditInfrastructureError(f"{relative}: production path crosses a reparse point")
        if stat.S_ISDIR(metadata.st_mode):
            yield from _walk_production_entries(path, root)
        else:
            yield path, metadata


def _physical_line_count(source: str) -> int:
    if not source:
        return 0
    normalized = source.replace("\r\n", "\n").replace("\r", "\n")
    return normalized.count("\n") + (0 if normalized.endswith("\n") else 1)


def enumerate_production_identities(root: Path) -> dict[PurePosixPath, FileIdentity]:
    """Enumerate unambiguous canonical identities under the two production roots."""

    lexical_root = Path(root).absolute()
    current = Path(lexical_root.anchor)
    for component in lexical_root.parts[1:]:
        current /= component
        try:
            component_metadata = current.lstat()
        except OSError as error:
            raise AuditInfrastructureError(
                f"{lexical_root}: source root cannot be inspected at {current}: {error}"
            ) from error
        if stat.S_ISLNK(component_metadata.st_mode):
            raise AuditInfrastructureError(
                f"{lexical_root}: source root crosses a symlink at {current}"
            )
        if _is_reparse(component_metadata):
            raise AuditInfrastructureError(
                f"{lexical_root}: source root crosses a reparse point at {current}"
            )
        if not stat.S_ISDIR(component_metadata.st_mode):
            raise AuditInfrastructureError(
                f"{lexical_root}: source root component is not a directory: {current}"
            )
    try:
        canonical_root = lexical_root.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(f"{lexical_root}: source root cannot be resolved: {error}") from error
    identities: dict[PurePosixPath, FileIdentity] = {}
    casefolded: dict[str, PurePosixPath] = {}
    filesystem_ids: dict[tuple[int, int], PurePosixPath] = {}

    for production_root_name in _PRODUCTION_ROOTS:
        production_root = lexical_root / production_root_name
        try:
            root_metadata = production_root.lstat()
        except FileNotFoundError:
            continue
        except OSError as error:
            raise AuditInfrastructureError(
                f"{production_root_name}: cannot inspect production root: {error}"
            ) from error
        if production_root.is_symlink():
            raise AuditInfrastructureError(
                f"{production_root_name}: production path crosses a symlink"
            )
        if _is_reparse(root_metadata):
            raise AuditInfrastructureError(
                f"{production_root_name}: production path crosses a reparse point"
            )
        if not stat.S_ISDIR(root_metadata.st_mode):
            raise AuditInfrastructureError(f"{production_root_name}: production root is not a directory")

        for candidate, metadata in _walk_production_entries(production_root, lexical_root):
            relative = _display_relative(candidate, lexical_root)
            if candidate.suffix.casefold() not in _SOURCE_SUFFIXES:
                continue
            if not stat.S_ISREG(metadata.st_mode):
                raise AuditInfrastructureError(f"{relative}: production path is not a regular file")
            try:
                canonical = candidate.resolve(strict=True)
            except OSError as error:
                raise AuditInfrastructureError(f"{relative}: production path cannot be resolved: {error}") from error
            try:
                canonical.relative_to(canonical_root)
            except ValueError as error:
                raise AuditInfrastructureError(
                    f"{relative}: resolved production path is outside the source root"
                ) from error

            _check_casefold_collision(relative, casefolded)
            raw_device = getattr(metadata, "st_dev", None)
            raw_inode = getattr(metadata, "st_ino", None)
            device = int(raw_device) if isinstance(raw_device, int) else None
            inode = int(raw_inode) if isinstance(raw_inode, int) and raw_inode != 0 else None
            if device is not None and inode is not None:
                filesystem_id = (device, inode)
                previous = filesystem_ids.get(filesystem_id)
                if previous is not None:
                    raise AuditInfrastructureError(
                        f"{relative}: filesystem identity aliases production path {previous}"
                    )
                filesystem_ids[filesystem_id] = relative

            try:
                source = candidate.read_bytes().decode("utf-8", errors="strict")
            except UnicodeDecodeError as error:
                raise AuditInfrastructureError(f"{relative}: production file is not valid UTF-8") from error
            except OSError as error:
                raise AuditInfrastructureError(f"{relative}: production file is unreadable: {error}") from error

            identities[relative] = FileIdentity(
                canonical=canonical,
                relative=relative,
                device=device,
                inode=inode,
                line_count=_physical_line_count(source),
                production=True,
            )

    return dict(sorted(identities.items(), key=lambda item: item[0].as_posix()))
import re
AUDIT_RESULT_SCHEMA_BYTES = b"olr-gpu-capability-audit-result-v1"
_LOWER_HEX_256 = re.compile(r"[0-9a-f]{64}\Z")
_STABLE_DEPENDENCY_ROLE = re.compile(r"[a-z0-9][a-z0-9._-]*(?::[a-z0-9][a-z0-9._-]*)?\Z")
_LOCAL_DEPENDENCY_FIELDS = (
    "stable_role",
    "role_relative_path",
    "canonical",
    "relative",
    "device",
    "inode",
    "line_count",
    "production",
    "sha256",
)
_LOCAL_DEPENDENCY_MAX_BYTES = 64 * 1024


def _validate_digest(value: object, label: str) -> str:
    if not isinstance(value, str) or _LOWER_HEX_256.fullmatch(value) is None:
        raise AuditInfrastructureError(f"{label} must be a lowercase SHA-256 digest")
    return value


_DEPENDENCY_ROOT_AUTHORITY_SCHEMA = "olr-gpu-dependency-root-authority-v1"
_DEPENDENCY_ROOT_AUTHORITY_FIELDS = (
    "schema",
    "source_root",
    "external_roots",
    "portable_authority_digest",
)
_COMPILER_INSPECTION_FIELDS = (
    "compiler_family",
    "executable_identity",
    "executable_sha256",
    "normalized_version",
    "driver_fingerprint",
    "inspection_arguments_digest",
    "executable_capability_digest",
)
_LOCAL_AUTHORITY_MAX_BYTES = 256 * 1024
_LOCAL_INSPECTION_MAX_BYTES = 256 * 1024


def _identity_from_stat(path: Path, metadata: os.stat_result, *, production: bool) -> FileIdentity:
    inode = int(metadata.st_ino) if int(metadata.st_ino) != 0 else None
    return FileIdentity(
        canonical=path,
        relative=None,
        device=int(metadata.st_dev),
        inode=inode,
        line_count=0,
        production=production,
    )


def _identity_native_key(identity: FileIdentity) -> tuple[int | None, int | None]:
    return identity.device, identity.inode


def _validate_root_binding(binding: object, *, source: bool) -> DependencyRootBinding:
    if not isinstance(binding, DependencyRootBinding):
        raise AuditInfrastructureError("dependency root binding is invalid")
    if source:
        if binding.stable_role != "production":
            raise AuditInfrastructureError("source root role must be production")
    elif (
        binding.stable_role == "production"
        or _STABLE_DEPENDENCY_ROLE.fullmatch(binding.stable_role) is None
    ):
        raise AuditInfrastructureError("external dependency root role is invalid")
    _validate_native_canonical_path(binding.resolved_root)
    identity = binding.root_identity
    if (
        not isinstance(identity, FileIdentity)
        or identity.canonical != binding.resolved_root
        or identity.relative is not None
        or identity.production is not source
        or identity.line_count != 0
        or any(
            value is not None
            and (not isinstance(value, int) or isinstance(value, bool) or value < 0)
            for value in (identity.device, identity.inode)
        )
    ):
        raise AuditInfrastructureError("dependency root identity is invalid")
    return binding


def _portable_authority_digest(roles: tuple[str, ...]) -> str:
    digest = hashlib.sha256()
    for value in (_DEPENDENCY_ROOT_AUTHORITY_SCHEMA, *roles):
        encoded = value.encode("ascii")
        digest.update(struct.pack("<Q", len(encoded)))
        digest.update(encoded)
    return digest.hexdigest()


def _validate_authority_structure(authority: object) -> DependencyRootAuthority:
    if not isinstance(authority, DependencyRootAuthority):
        raise AuditInfrastructureError("dependency root authority is invalid")
    source = _validate_root_binding(authority.source_root, source=True)
    if not isinstance(authority.external_roots, tuple):
        raise AuditInfrastructureError("external dependency roots are invalid")
    external = tuple(
        _validate_root_binding(binding, source=False)
        for binding in authority.external_roots
    )
    roles = tuple(binding.stable_role for binding in external)
    if roles != tuple(sorted(roles)) or len(set(roles)) != len(roles):
        raise AuditInfrastructureError("external dependency root roles are not unique and sorted")
    paths = (source.resolved_root, *(binding.resolved_root for binding in external))
    keys = tuple(os.path.normcase(str(path)) for path in paths)
    if len(set(keys)) != len(keys):
        raise AuditInfrastructureError("dependency roots overlap")
    for index, first in enumerate(paths):
        for second in paths[index + 1 :]:
            try:
                first.relative_to(second)
            except ValueError:
                pass
            else:
                raise AuditInfrastructureError("dependency roots overlap")
            try:
                second.relative_to(first)
            except ValueError:
                pass
            else:
                raise AuditInfrastructureError("dependency roots overlap")
    expected = _portable_authority_digest(("production", *roles))
    if authority.portable_authority_digest != expected:
        raise AuditInfrastructureError("dependency root authority digest disagrees")
    return authority


def build_dependency_root_authority(
    source_root: Path,
    external_roots: Mapping[str, Path],
) -> DependencyRootAuthority:
    if not isinstance(source_root, Path) or not isinstance(external_roots, Mapping):
        raise AuditInfrastructureError("dependency root inputs are invalid")

    def binding(role: str, value: Path, *, source: bool) -> DependencyRootBinding:
        if not isinstance(value, Path):
            raise AuditInfrastructureError("dependency root path is invalid")
        try:
            requested = value.absolute()
            before = requested.lstat()
            resolved = requested.resolve(strict=True)
            after = resolved.stat()
        except (OSError, RuntimeError) as error:
            raise AuditInfrastructureError(f"dependency root is unavailable: {value}") from error
        if (
            not stat.S_ISDIR(before.st_mode)
            or stat.S_ISLNK(before.st_mode)
            or bool(getattr(before, "st_file_attributes", 0) & _REPARSE_ATTRIBUTE)
            or not stat.S_ISDIR(after.st_mode)
        ):
            raise AuditInfrastructureError(f"dependency root is not an ordinary directory: {value}")
        before_key = (int(before.st_dev), int(before.st_ino) or None)
        after_key = (int(after.st_dev), int(after.st_ino) or None)
        if before_key != after_key:
            raise AuditInfrastructureError(f"dependency root changed while binding: {value}")
        return DependencyRootBinding(
            role,
            resolved,
            _identity_from_stat(resolved, after, production=source),
        )

    source_binding = binding("production", source_root, source=True)
    externals: list[DependencyRootBinding] = []
    for role, root in external_roots.items():
        if not isinstance(role, str):
            raise AuditInfrastructureError("external dependency root role is invalid")
        externals.append(binding(role, root, source=False))
    externals.sort(key=lambda item: item.stable_role)
    authority = DependencyRootAuthority(
        source_binding,
        tuple(externals),
        _portable_authority_digest(
            ("production", *(item.stable_role for item in externals))
        ),
    )
    return _validate_authority_structure(authority)


def validate_dependency_root_authority(
    authority: object,
    *,
    expected_digest: str | None = None,
) -> DependencyRootAuthority:
    validated = _validate_authority_structure(authority)
    if expected_digest is not None and (
        _validate_digest(expected_digest, "dependency root authority")
        != validated.portable_authority_digest
    ):
        raise AuditInfrastructureError("dependency root authority digest mismatch")
    for binding in (validated.source_root, *validated.external_roots):
        try:
            metadata = binding.resolved_root.lstat()
        except OSError as error:
            raise AuditInfrastructureError("dependency root identity changed") from error
        if (
            not stat.S_ISDIR(metadata.st_mode)
            or stat.S_ISLNK(metadata.st_mode)
            or bool(getattr(metadata, "st_file_attributes", 0) & _REPARSE_ATTRIBUTE)
        ):
            raise AuditInfrastructureError(
                "dependency root is not an ordinary directory"
            )
        current = _identity_from_stat(
            binding.resolved_root,
            metadata,
            production=binding.stable_role == "production",
        )
        if current != binding.root_identity:
            raise AuditInfrastructureError("dependency root identity changed")
    return validated


def _identity_document(identity: FileIdentity) -> dict[str, object]:
    return {
        "canonical": str(identity.canonical),
        "relative": identity.relative.as_posix() if identity.relative is not None else None,
        "device": identity.device,
        "inode": identity.inode,
        "line_count": identity.line_count,
        "production": identity.production,
    }


def _identity_from_exact_document(value: object) -> FileIdentity:
    fields = ("canonical", "relative", "device", "inode", "line_count", "production")
    if not isinstance(value, dict) or tuple(value) != fields:
        raise AuditInfrastructureError("local identity schema is invalid")
    canonical = value["canonical"]
    relative = value["relative"]
    if not isinstance(canonical, str):
        raise AuditInfrastructureError("local identity is invalid")
    return FileIdentity(
        Path(_validate_native_canonical_text(canonical)),
        PurePosixPath(relative) if isinstance(relative, str) else None,
        value["device"],
        value["inode"],
        value["line_count"],
        value["production"],
    )


def encode_dependency_root_authority(authority: DependencyRootAuthority) -> bytes:
    validated = _validate_authority_structure(authority)

    def record(binding: DependencyRootBinding) -> dict[str, object]:
        return {
            "stable_role": binding.stable_role,
            "resolved_root": str(binding.resolved_root),
            "root_identity": _identity_document(binding.root_identity),
        }

    document = {
        "schema": _DEPENDENCY_ROOT_AUTHORITY_SCHEMA,
        "source_root": record(validated.source_root),
        "external_roots": [record(item) for item in validated.external_roots],
        "portable_authority_digest": validated.portable_authority_digest,
    }
    return json.dumps(document, ensure_ascii=True, separators=(",", ":")).encode("ascii")


def decode_dependency_root_authority(
    payload: bytes,
    *,
    expected: DependencyRootAuthority | None = None,
) -> DependencyRootAuthority:
    if not isinstance(payload, bytes) or len(payload) > _LOCAL_AUTHORITY_MAX_BYTES:
        raise AuditInfrastructureError("local dependency root authority payload is invalid")
    try:
        document = json.loads(payload.decode("ascii"))
    except (UnicodeError, ValueError, TypeError, RecursionError, json.JSONDecodeError) as error:
        raise AuditInfrastructureError("local dependency root authority payload is invalid") from error
    if not isinstance(document, dict) or tuple(document) != _DEPENDENCY_ROOT_AUTHORITY_FIELDS:
        raise AuditInfrastructureError("local dependency root authority schema is invalid")
    if document["schema"] != _DEPENDENCY_ROOT_AUTHORITY_SCHEMA:
        raise AuditInfrastructureError("local dependency root authority schema is invalid")

    def binding(value: object) -> DependencyRootBinding:
        if not isinstance(value, dict) or tuple(value) != (
            "stable_role", "resolved_root", "root_identity"
        ):
            raise AuditInfrastructureError("local dependency root binding schema is invalid")
        role = value["stable_role"]
        root = value["resolved_root"]
        if not isinstance(role, str) or not isinstance(root, str):
            raise AuditInfrastructureError("local dependency root binding is invalid")
        return DependencyRootBinding(
            role,
            Path(_validate_native_canonical_text(root)),
            _identity_from_exact_document(value["root_identity"]),
        )

    external_values = document["external_roots"]
    if not isinstance(external_values, list):
        raise AuditInfrastructureError("local external dependency roots are invalid")
    decoded = _validate_authority_structure(
        DependencyRootAuthority(
            binding(document["source_root"]),
            tuple(binding(value) for value in external_values),
            document["portable_authority_digest"],
        )
    )
    if expected is not None and decoded != expected:
        raise AuditInfrastructureError("local dependency root authority differs")
    return decoded


def _validate_executable_identity(identity: object) -> FileIdentity:
    if not isinstance(identity, FileIdentity):
        raise AuditInfrastructureError("compiler executable identity is invalid")
    _validate_native_canonical_path(identity.canonical)
    if (
        identity.relative is not None
        or identity.production is not False
        or identity.line_count != 0
        or any(
            value is not None
            and (not isinstance(value, int) or isinstance(value, bool) or value < 0)
            for value in (identity.device, identity.inode)
        )
    ):
        raise AuditInfrastructureError("compiler executable identity is invalid")
    return identity


def _validate_current_executable_identity(identity: object) -> FileIdentity:
    identity = _validate_executable_identity(identity)
    try:
        before = identity.canonical.lstat()
        canonical = identity.canonical.resolve(strict=True)
        current = canonical.stat()
    except (OSError, RuntimeError) as error:
        raise AuditInfrastructureError("compiler executable identity changed") from error
    if (
        canonical != identity.canonical
        or not stat.S_ISREG(before.st_mode)
        or stat.S_ISLNK(before.st_mode)
        or not stat.S_ISREG(current.st_mode)
        or (int(current.st_dev), int(current.st_ino) or None)
        != (identity.device, identity.inode)
    ):
        raise AuditInfrastructureError("compiler executable identity is not a current regular file")
    return identity


def _current_executable_sha256(
    identity: FileIdentity,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> str:
    def check_budget() -> None:
        if cancel_event is not None and cancel_event.is_set():
            raise AuditInfrastructureError("compiler inspection hashing cancelled")
        if deadline is not None and time.monotonic() >= deadline:
            raise AuditInfrastructureError("compiler inspection hashing deadline exceeded")

    check_budget()
    _validate_current_executable_identity(identity)
    digest = hashlib.sha256()
    try:
        with identity.canonical.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (int(opened.st_dev), int(opened.st_ino) or None) != (
                identity.device, identity.inode
            ):
                raise OSError("identity changed")
            while chunk := stream.read(1024 * 1024):
                check_budget()
                digest.update(chunk)
            after = os.fstat(stream.fileno())
        current = identity.canonical.stat()
    except OSError as error:
        raise AuditInfrastructureError("compiler executable content changed") from error
    expected = (identity.device, identity.inode)
    if (
        (int(after.st_dev), int(after.st_ino) or None) != expected
        or (int(current.st_dev), int(current.st_ino) or None) != expected
    ):
        raise AuditInfrastructureError("compiler executable content changed")
    return digest.hexdigest()


def encode_compiler_inspection(inspection: CompilerInspection) -> bytes:
    if not isinstance(inspection, CompilerInspection):
        raise AuditInfrastructureError("compiler inspection is invalid")
    document = {
        "compiler_family": inspection.compiler_family.value,
        "executable_identity": _identity_document(inspection.executable_identity),
        "executable_sha256": inspection.executable_sha256,
        "normalized_version": inspection.normalized_version,
        "driver_fingerprint": inspection.driver_fingerprint,
        "inspection_arguments_digest": inspection.inspection_arguments_digest,
        "executable_capability_digest": inspection.executable_capability_digest,
    }
    return json.dumps(document, ensure_ascii=True, separators=(",", ":")).encode("ascii")


def decode_compiler_inspection(
    payload: bytes,
    *,
    validation_deadline: float | None = None,
    cancel_event: object | None = None,
    held_executable_identity: FileIdentity | None = None,
    held_executable_sha256: str | None = None,
) -> CompilerInspection:
    if not isinstance(payload, bytes) or len(payload) > _LOCAL_INSPECTION_MAX_BYTES:
        raise AuditInfrastructureError("compiler inspection payload is invalid")
    try:
        document = json.loads(payload.decode("ascii"))
    except (UnicodeError, ValueError, TypeError, RecursionError, json.JSONDecodeError) as error:
        raise AuditInfrastructureError("compiler inspection payload is invalid") from error
    if not isinstance(document, dict) or tuple(document) != _COMPILER_INSPECTION_FIELDS:
        raise AuditInfrastructureError("compiler inspection schema is invalid")
    try:
        family = CompilerFamily(document["compiler_family"])
    except (ValueError, TypeError) as error:
        raise AuditInfrastructureError("compiler inspection family is invalid") from error
    return CompilerInspection(
        family,
        _identity_from_exact_document(document["executable_identity"]),
        document["executable_sha256"],
        document["normalized_version"],
        document["driver_fingerprint"],
        document["inspection_arguments_digest"],
        document["executable_capability_digest"],
        validation_deadline=validation_deadline,
        cancel_event=cancel_event,
        held_executable_identity=held_executable_identity,
        held_executable_sha256=held_executable_sha256,
    )


def portable_compiler_inspection_key(inspection: CompilerInspection) -> bytes:
    if not isinstance(inspection, CompilerInspection):
        raise AuditInfrastructureError("compiler inspection is invalid")
    payload = bytearray()
    for value in (
        inspection.compiler_family.value,
        inspection.executable_sha256,
        inspection.normalized_version,
        inspection.driver_fingerprint,
        inspection.inspection_arguments_digest,
        inspection.executable_capability_digest,
    ):
        encoded = value.encode("utf-8")
        payload.extend(struct.pack("<Q", len(encoded)))
        payload.extend(encoded)
    return bytes(payload)


def _validate_role_relative_path(
    stable_role: object,
    role_relative_path: object,
) -> tuple[str, PurePosixPath]:
    if (
        not isinstance(stable_role, str)
        or _STABLE_DEPENDENCY_ROLE.fullmatch(stable_role) is None
    ):
        raise AuditInfrastructureError("dependency stable role is invalid")
    if not isinstance(role_relative_path, PurePosixPath):
        raise AuditInfrastructureError("dependency role-relative path is invalid")
    path_text = role_relative_path.as_posix()
    if (
        not path_text
        or path_text == "."
        or role_relative_path.is_absolute()
        or "\\" in path_text
        or any(part in ("", ".", "..") for part in role_relative_path.parts)
        or (
            stable_role == "production"
            and role_relative_path.suffix.casefold() not in _SOURCE_SUFFIXES
        )
    ):
        raise AuditInfrastructureError("dependency role-relative path is invalid")
    if stable_role == "production" and (
        not role_relative_path.parts
        or role_relative_path.parts[0] not in _PRODUCTION_ROOTS
    ):
        raise AuditInfrastructureError(
            "production dependency path is outside the production roots"
        )
    return stable_role, role_relative_path


def _validate_file_identity(identity: object, *, stable_role: str, path: PurePosixPath) -> FileIdentity:
    if not isinstance(identity, FileIdentity):
        raise AuditInfrastructureError("dependency file identity is invalid")
    _validate_native_canonical_path(identity.canonical)
    if identity.relative is not None and not isinstance(identity.relative, PurePosixPath):
        raise AuditInfrastructureError("dependency relative identity is invalid")
    for label, value in (("device", identity.device), ("inode", identity.inode)):
        if value is not None and (
            not isinstance(value, int) or isinstance(value, bool) or value < 0
        ):
            raise AuditInfrastructureError(f"dependency {label} identity is invalid")
    if (
        not isinstance(identity.line_count, int)
        or isinstance(identity.line_count, bool)
        or identity.line_count < 0
    ):
        raise AuditInfrastructureError("dependency line count is invalid")
    if not isinstance(identity.production, bool):
        raise AuditInfrastructureError("dependency production identity is invalid")
    is_production = stable_role == "production"
    if identity.production != is_production:
        raise AuditInfrastructureError("dependency role and production identity disagree")
    if is_production and identity.relative != path:
        raise AuditInfrastructureError("production dependency identity path disagrees")
    if not is_production and identity.relative is not None:
        raise AuditInfrastructureError("external dependency identity must be role-relative")
    return identity


def _validate_native_canonical_path(value: object) -> Path:
    if not isinstance(value, Path):
        raise AuditInfrastructureError("dependency canonical identity is invalid")
    _validate_native_canonical_text(str(value))
    return value


def _validate_native_canonical_text(text: object) -> str:
    if not isinstance(text, str) or not text or text == "." or "\0" in text:
        raise AuditInfrastructureError("dependency canonical identity is invalid")
    if text.endswith(("/", "\\")):
        raise AuditInfrastructureError("dependency canonical identity is not canonical")

    drive_match = re.match(r"\A[A-Za-z]:([\\/])", text)
    if drive_match is not None:
        separator = drive_match.group(1)
        tail = text[3:]
        if not tail or (("\\" if separator == "/" else "/") in tail):
            raise AuditInfrastructureError(
                "dependency canonical identity is not canonical"
            )
        components = tail.split(separator)
    elif text.startswith(("//", "\\\\")):
        separator = text[0]
        if text.startswith(separator * 3):
            raise AuditInfrastructureError(
                "dependency canonical identity is not canonical"
            )
        tail = text[2:]
        if ("\\" if separator == "/" else "/") in tail:
            raise AuditInfrastructureError(
                "dependency canonical identity is not canonical"
            )
        components = tail.split(separator)
        if len(components) < 3:
            raise AuditInfrastructureError(
                "dependency UNC identity must name a file below a share"
            )
    elif text.startswith("/"):
        if "\\" in text:
            raise AuditInfrastructureError(
                "dependency canonical identity is not canonical"
            )
        components = text[1:].split("/")
    else:
        raise AuditInfrastructureError("dependency canonical identity must be absolute")
    if any(part in ("", ".", "..") for part in components):
        raise AuditInfrastructureError("dependency canonical identity is not canonical")
    return text


@dataclass(frozen=True, slots=True)
class DependencyDigest:
    stable_role: str
    role_relative_path: PurePosixPath
    identity: FileIdentity
    sha256: str

    def __post_init__(self) -> None:
        stable_role, path = _validate_role_relative_path(
            self.stable_role, self.role_relative_path
        )
        _validate_file_identity(self.identity, stable_role=stable_role, path=path)
        _validate_digest(self.sha256, "dependency content")


@dataclass(frozen=True, slots=True)
class AuditResultFinding:
    path: PurePosixPath
    line: int
    expression: str
    reason: str

    def __post_init__(self) -> None:
        _stable_role, path = _validate_role_relative_path("production", self.path)
        if (
            not isinstance(self.line, int)
            or isinstance(self.line, bool)
            or self.line < 1
        ):
            raise AuditInfrastructureError("audit result finding line is invalid")
        if not isinstance(self.expression, str) or not self.expression:
            raise AuditInfrastructureError("audit result finding expression is invalid")
        if not isinstance(self.reason, str) or not self.reason:
            raise AuditInfrastructureError("audit result finding reason is invalid")
        if path != self.path:
            raise AuditInfrastructureError("audit result finding path is invalid")


def portable_dependency_key(dependency: DependencyDigest) -> bytes:
    if not isinstance(dependency, DependencyDigest):
        raise AuditInfrastructureError("portable dependency is invalid")
    payload = bytearray()
    for value in (
        dependency.stable_role.encode("ascii"),
        dependency.role_relative_path.as_posix().encode("utf-8"),
        dependency.sha256.encode("ascii"),
    ):
        payload.extend(struct.pack("<Q", len(value)))
        payload.extend(value)
    return bytes(payload)


def _dependency_sort_key(dependency: DependencyDigest) -> tuple[str, str, str]:
    return (
        dependency.stable_role,
        dependency.role_relative_path.as_posix(),
        dependency.sha256,
    )


def _audit_finding_key(finding: AuditResultFinding) -> tuple[str, int, str, str]:
    return (
        finding.path.as_posix(),
        finding.line,
        finding.expression,
        finding.reason,
    )


@dataclass(frozen=True, slots=True)
class ConfigurationAuditResult:
    configuration_digest: str
    audit_engine_fingerprint: str
    dependencies: tuple[DependencyDigest, ...]
    reached_production: tuple[PurePosixPath, ...]
    findings: tuple[AuditResultFinding, ...]
    _transport_ownership: CompactResultReservationOwnership | None = field(
        default=None, compare=False, repr=False
    )

    def __post_init__(self) -> None:
        limits = AuditLimits()
        _validate_digest(self.configuration_digest, "configuration")
        _validate_digest(self.audit_engine_fingerprint, "audit engine fingerprint")
        if not isinstance(self.dependencies, tuple) or any(
            not isinstance(item, DependencyDigest) for item in self.dependencies
        ):
            raise AuditInfrastructureError("audit result dependencies are invalid")
        if len(self.dependencies) > limits.compact_result_dependencies:
            raise AuditInfrastructureError("audit result limit exceeded for dependencies")
        previous_dependency_key: tuple[str, str, str] | None = None
        for dependency in self.dependencies:
            dependency_key = _dependency_sort_key(dependency)
            if (
                previous_dependency_key is not None
                and dependency_key <= previous_dependency_key
            ):
                raise AuditInfrastructureError(
                    "audit result dependencies are not unique and sorted"
                )
            previous_dependency_key = dependency_key
            if (
                len(dependency.role_relative_path.as_posix().encode("utf-8"))
                > limits.compact_result_path_bytes
                or len(str(dependency.identity.canonical).encode("utf-8"))
                > limits.compact_result_path_bytes
            ):
                raise AuditInfrastructureError("audit result limit exceeded for dependency path")
        if not isinstance(self.reached_production, tuple):
            raise AuditInfrastructureError("audit result reached-production paths are invalid")
        if len(self.reached_production) > limits.compact_result_reached:
            raise AuditInfrastructureError("audit result limit exceeded for reached paths")
        previous_reached_key: str | None = None
        for path in self.reached_production:
            _stable_role, validated = _validate_role_relative_path("production", path)
            if len(validated.as_posix().encode("utf-8")) > limits.compact_result_path_bytes:
                raise AuditInfrastructureError("audit result limit exceeded for path")
            reached_key = validated.as_posix()
            if previous_reached_key is not None and reached_key <= previous_reached_key:
                raise AuditInfrastructureError(
                    "audit result reached-production paths are not unique and sorted"
                )
            previous_reached_key = reached_key
        if not isinstance(self.findings, tuple) or any(
            not isinstance(item, AuditResultFinding) for item in self.findings
        ):
            raise AuditInfrastructureError("audit result findings are invalid")
        if len(self.findings) > limits.compact_result_findings:
            raise AuditInfrastructureError("audit result limit exceeded for findings")
        previous_finding_key: tuple[str, int, str, str] | None = None
        for finding in self.findings:
            finding_key = _audit_finding_key(finding)
            if previous_finding_key is not None and finding_key <= previous_finding_key:
                raise AuditInfrastructureError(
                    "audit result findings are not unique and sorted"
                )
            previous_finding_key = finding_key
            if (
                len(finding.path.as_posix().encode("utf-8"))
                > limits.compact_result_path_bytes
                or len(finding.expression.encode("utf-8"))
                > limits.compact_result_expression_bytes
                or len(finding.reason.encode("utf-8"))
                > limits.compact_result_reason_bytes
            ):
                raise AuditInfrastructureError("audit result limit exceeded for finding text")
        if (
            self._transport_ownership is not None
            and not isinstance(
                self._transport_ownership, CompactResultReservationOwnership
            )
        ):
            raise AuditInfrastructureError("audit result transport ownership is invalid")

    def release_transport_ownership(self) -> None:
        ownership = self._transport_ownership
        if ownership is None:
            raise AuditInfrastructureError(
                "audit result transport ownership is unavailable"
            )
        if ownership.phase != "receiver-retained-result":
            raise AuditInfrastructureError(
                "audit result transport ownership was transferred"
            )
        ownership.release("retained-result-transition")


@dataclass(frozen=True, slots=True)
class ConfigurationAuditPublicationPermit:
    configuration_digest: str
    audit_engine_fingerprint: str
    dependencies: tuple[DependencyDigest, ...]

    def __post_init__(self) -> None:
        _validate_digest(self.configuration_digest, "configuration publication permit")
        _validate_digest(self.audit_engine_fingerprint, "audit publication permit engine")
        if not isinstance(self.dependencies, tuple) or any(
            not isinstance(item, DependencyDigest) for item in self.dependencies
        ):
            raise AuditInfrastructureError("audit publication permit dependencies are invalid")
        keys = tuple(_dependency_sort_key(item) for item in self.dependencies)
        if keys != tuple(sorted(keys)) or len(set(keys)) != len(keys):
            raise AuditInfrastructureError(
                "audit publication permit dependencies are not unique and sorted"
            )


class _RootPublicationLease:
    __slots__ = ("_callback", "_released", "_lock")

    def __init__(self, callback: Callable[[], None] | None) -> None:
        if callback is not None and not callable(callback):
            raise AuditInfrastructureError("root publication release callback is invalid")
        self._callback = callback
        self._released = False
        self._lock = threading.Lock()

    @property
    def released(self) -> bool:
        with self._lock:
            return self._released

    def release(self) -> None:
        with self._lock:
            if self._released:
                raise AuditInfrastructureError("root publication permit was already released")
            self._released = True
            callback = self._callback
            self._callback = None
        if callback is not None:
            callback()


@dataclass(frozen=True, slots=True)
class CachePublicationPermit(ConfigurationAuditPublicationPermit):
    task_id: str
    generation: int
    release_callback: Callable[[], None] | None = field(
        default=None, compare=False, repr=False
    )
    root_publication_bytes: int = field(default=8 << 20, init=False)
    _lease: _RootPublicationLease = field(init=False, compare=False, repr=False)

    def __post_init__(self) -> None:
        ConfigurationAuditPublicationPermit.__post_init__(self)
        if (
            not isinstance(self.task_id, str)
            or not self.task_id
            or not isinstance(self.generation, int)
            or isinstance(self.generation, bool)
            or self.generation < 0
            or self.root_publication_bytes != 8 << 20
        ):
            raise AuditInfrastructureError("root publication permit generation is invalid")
        object.__setattr__(self, "_lease", _RootPublicationLease(self.release_callback))

    @property
    def released(self) -> bool:
        return self._lease.released

    def validate_for_task(
        self,
        task_id: str,
        generation: int,
        configuration_digest: str,
        audit_engine_fingerprint: str,
        dependencies: tuple[DependencyDigest, ...],
    ) -> None:
        if (
            self.released
            or task_id != self.task_id
            or generation != self.generation
            or configuration_digest != self.configuration_digest
            or audit_engine_fingerprint != self.audit_engine_fingerprint
            or dependencies != self.dependencies
        ):
            raise AuditInfrastructureError("root publication permit generation differs")

    def release_root_publication(self) -> None:
        self._lease.release()


@dataclass(frozen=True, slots=True)
class ConfigurationAuditOutcome:
    result: ConfigurationAuditResult
    stdout_bytes: int
    stages: WorkerStageTimings

    def __post_init__(self) -> None:
        if (
            not isinstance(self.result, ConfigurationAuditResult)
            or not isinstance(self.stdout_bytes, int)
            or isinstance(self.stdout_bytes, bool)
            or self.stdout_bytes < 0
            or not isinstance(self.stages, WorkerStageTimings)
        ):
            raise AuditInfrastructureError("configuration audit outcome is invalid")


class ConfigurationAuditOutcomeOwner:
    """Closeable parent owner for one receiver-retained audit outcome."""

    __slots__ = ("_outcome", "_active", "_lock")

    def __init__(self, outcome: ConfigurationAuditOutcome) -> None:
        if (
            not isinstance(outcome, ConfigurationAuditOutcome)
            or outcome.result._transport_ownership is None
            or outcome.result._transport_ownership.phase
            != "receiver-retained-result"
        ):
            raise AuditInfrastructureError("configuration audit outcome owner is invalid")
        self._outcome = outcome
        self._active = True
        self._lock = threading.Lock()

    @property
    def outcome(self) -> ConfigurationAuditOutcome:
        with self._lock:
            if not self._active:
                raise AuditInfrastructureError(
                    "configuration audit outcome owner was transferred"
                )
            return self._outcome

    @property
    def active(self) -> bool:
        with self._lock:
            return self._active

    def close(self) -> None:
        with self._lock:
            if not self._active:
                raise AuditInfrastructureError(
                    "configuration audit outcome owner was transferred"
                )
            outcome = self._outcome
            outcome.result.release_transport_ownership()
            self._active = False

    def transfer(self) -> ConfigurationAuditOutcome:
        with self._lock:
            if not self._active:
                raise AuditInfrastructureError(
                    "configuration audit outcome owner was transferred"
                )
            self._active = False
            return self._outcome

    def transfer_to(self, acceptor) -> object:
        if not callable(acceptor):
            raise AuditInfrastructureError("audit outcome acceptor is invalid")
        outcome = self.outcome
        accepted = acceptor(outcome)
        with self._lock:
            if not self._active:
                raise AuditInfrastructureError(
                    "configuration audit outcome owner was transferred"
                )
            self._active = False
        return accepted

    def __enter__(self) -> "ConfigurationAuditOutcomeOwner":
        if not self.active:
            raise AuditInfrastructureError(
                "configuration audit outcome owner was transferred"
            )
        return self

    def __exit__(self, _type, value, _traceback) -> None:
        if not self.active:
            return
        try:
            self.close()
        except BaseException as cleanup_error:
            if value is None:
                raise
            value.add_note(
                "configuration audit outcome owner cleanup also failed: "
                f"{cleanup_error}"
            )

    def __del__(self) -> None:
        try:
            if self.active:
                self.close()
        except BaseException:
            pass


@dataclass(frozen=True, slots=True)
class ConfigurationAuditTransportOutcome:
    transport: ConfigurationAuditResultTransport
    stdout_bytes: int
    stages: WorkerStageTimings

    def __post_init__(self) -> None:
        if (
            not isinstance(self.transport, ConfigurationAuditResultTransport)
            or not isinstance(self.stdout_bytes, int)
            or isinstance(self.stdout_bytes, bool)
            or self.stdout_bytes < 0
            or not isinstance(self.stages, WorkerStageTimings)
            or self.transport.receipt.stdout_bytes != self.stdout_bytes
            or self.transport.receipt.stages != self.stages
        ):
            raise AuditInfrastructureError(
                "configuration audit transport outcome is invalid"
            )


def compact_result_retained_bytes(result: ConfigurationAuditResult) -> int:
    """Conservative retained allocation charge for one decoded compact result."""
    if not isinstance(result, ConfigurationAuditResult):
        raise AuditInfrastructureError("compact audit result is invalid")
    total = 1024
    for dependency in result.dependencies:
        total += (
            384
            + len(dependency.stable_role.encode("ascii"))
            + len(dependency.role_relative_path.as_posix().encode("utf-8"))
            + len(str(dependency.identity.canonical).encode("utf-8"))
            + (
                len(dependency.identity.relative.as_posix().encode("utf-8"))
                if dependency.identity.relative is not None
                else 0
            )
        )
    total += sum(
        128 + len(path.as_posix().encode("utf-8"))
        for path in result.reached_production
    )
    total += sum(
        320
        + len(finding.path.as_posix().encode("utf-8"))
        + len(finding.expression.encode("utf-8"))
        + len(finding.reason.encode("utf-8"))
        for finding in result.findings
    )
    if total > AuditLimits().compact_result_bytes:
        raise AuditInfrastructureError("per-entry decoded result limit exceeded")
    return total


@dataclass(frozen=True, slots=True)
class AggregatedAuditResultFinding:
    finding: AuditResultFinding
    configurations: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class StreamingAuditSummary:
    configurations: tuple[str, ...]
    reached_production: tuple[PurePosixPath, ...]
    findings: tuple[AggregatedAuditResultFinding, ...]
    dependency_digests: tuple[tuple[str, str, str], ...]
    _ownerships: tuple[CompactResultOwnership, ...] = field(
        compare=False, repr=False
    )

    @property
    def authoritative(self) -> frozenset[PurePosixPath]:
        return frozenset(self.reached_production)

    def release(self) -> None:
        for ownership in self._ownerships:
            if not ownership.released:
                ownership.release()


def _after_streaming_aggregate_mutation(_stage: str) -> None:
    """Fault-injection boundary for transactional aggregate mutation tests."""


@dataclass(frozen=True, slots=True)
class _StreamingAggregateMutation:
    configuration_digest: str
    reached_keys: tuple[str, ...]
    finding_changes: tuple[tuple[tuple[str, int, str, str], bool], ...]
    digest_keys: tuple[tuple[str, str, str], ...]
    growth_ownership: CompactResultOwnership


@dataclass(frozen=True, slots=True)
class _StreamingAggregateCheckpoint:
    mutation_count: int
    cold_ownership: CompactResultOwnership | None


def encode_canonical_summary(summary: StreamingAuditSummary) -> bytes:
    if not isinstance(summary, StreamingAuditSummary):
        raise AuditInfrastructureError("streaming audit summary is invalid")
    document = {
        "configurations": list(summary.configurations),
        "reached_production": [path.as_posix() for path in summary.reached_production],
        "findings": [
            {
                "path": item.finding.path.as_posix(),
                "line": item.finding.line,
                "expression": item.finding.expression,
                "reason": item.finding.reason,
                "configurations": list(item.configurations),
            }
            for item in summary.findings
        ],
        "dependency_digests": [list(item) for item in summary.dependency_digests],
    }
    return json.dumps(
        document, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    ).encode("ascii")


class StreamingResultAggregator:
    """Consumes compact results in configuration order with charged index growth."""

    def __init__(
        self,
        configurations: tuple[PreprocessConfiguration, ...],
        budget: CompactResultMemoryBudget,
        limits: AuditLimits,
    ) -> None:
        if not isinstance(configurations, tuple) or any(
            not isinstance(item, PreprocessConfiguration) for item in configurations
        ):
            raise AuditInfrastructureError("streaming result configurations are invalid")
        if not isinstance(budget, CompactResultMemoryBudget) or not isinstance(
            limits, AuditLimits
        ):
            raise AuditInfrastructureError("streaming result aggregate inputs are invalid")
        digests = tuple(item.digest for item in configurations)
        if len(set(digests)) != len(digests):
            raise AuditInfrastructureError("streaming result configurations are not unique")
        base_bytes = 256 + sum(
            128 + len(digest.encode("ascii")) for digest in digests
        )
        base_ownership = budget.reserve(
            base_bytes, label="streaming aggregate base index"
        ).commit()
        self._expected = configurations
        try:
            self._expected_index = {
                configuration.digest: index
                for index, configuration in enumerate(configurations)
            }
        except BaseException:
            base_ownership.release()
            raise
        self._budget = budget
        self._limits = limits
        self._base_ownership: CompactResultOwnership | None = base_ownership
        self._accepted: set[str] = set()
        self._reached: dict[str, PurePosixPath] = {}
        self._findings: dict[
            tuple[str, int, str, str], tuple[AuditResultFinding, list[str]]
        ] = {}
        self._digests: dict[tuple[str, str, str], None] = {}
        self._growth_ownerships: list[CompactResultOwnership] = []
        self._mutation_journals: list[_StreamingAggregateMutation] = []
        self._cold_ownership: CompactResultOwnership | None = None
        self._finished = False

    @property
    def accepted_count(self) -> int:
        return len(self._accepted)

    @property
    def next_unaggregated_ordinal(self) -> int:
        """Lowest configuration ordinal not yet accepted into the aggregate."""
        for ordinal, configuration in enumerate(self._expected):
            if configuration.digest not in self._accepted:
                return ordinal
        return len(self._expected)

    @property
    def budget(self) -> CompactResultMemoryBudget:
        return self._budget

    @property
    def cold_slot_reserved_bytes(self) -> int:
        ownership = self._cold_ownership
        return 0 if ownership is None or ownership.released else ownership.byte_count

    def reserve_cold_slot(self, slot: CompactResultColdSlot) -> int:
        if not isinstance(slot, CompactResultColdSlot):
            raise AuditInfrastructureError("compact result cold slot is invalid")
        if self._cold_ownership is not None and not self._cold_ownership.released:
            if self._cold_ownership.byte_count != slot.worst_case_live_bytes:
                raise AuditInfrastructureError("compact result cold slot differs")
            return self._cold_ownership.byte_count
        self._cold_ownership = self._budget.reserve(
            slot.worst_case_live_bytes,
            label="batch retained result limit cold slot",
        ).commit()
        return slot.worst_case_live_bytes

    def take_cold_slot(self, label: str) -> CompactResultOwnership:
        """Move the single cold capacity owner to one authenticated decode."""
        if not isinstance(label, str) or not label:
            raise AuditInfrastructureError(
                "compact result cold slot transfer is invalid"
            )
        ownership = self._cold_ownership
        if ownership is None or ownership.released:
            raise AuditInfrastructureError(
                "compact result cold slot is unavailable"
            )
        transferred = ownership.replace_committed(
            ownership.byte_count, label=label
        )
        self._cold_ownership = None
        return transferred

    def ownership_for_cold_result(
        self, result: ConfigurationAuditResult
    ) -> CompactResultOwnership:
        byte_count = compact_result_retained_bytes(result)
        if self._cold_ownership is None or self._cold_ownership.released:
            return self._budget.reserve(
                byte_count, label="batch retained result limit"
            ).commit()
        if byte_count > self._cold_ownership.byte_count:
            raise AuditInfrastructureError("cold result exceeds reserved cold slot")
        self._cold_ownership.release()
        return self._budget.reserve(
            byte_count, label="batch retained result limit"
        ).commit()

    def _growth_for(self, result: ConfigurationAuditResult) -> CompactResultGrowth:
        coverage_bytes = 0
        for path in result.reached_production:
            if path.as_posix() not in self._reached:
                coverage_bytes += 176 + len(path.as_posix().encode("utf-8"))
        finding_bytes = 0
        for finding in result.findings:
            key = _audit_finding_key(finding)
            finding_bytes += 120
            if key not in self._findings:
                finding_bytes += (
                    320
                    + len(finding.path.as_posix().encode("utf-8"))
                    + len(finding.expression.encode("utf-8"))
                    + len(finding.reason.encode("utf-8"))
                )
        digest_bytes = 0
        for dependency in result.dependencies:
            digest_key = (
                dependency.stable_role,
                dependency.role_relative_path.as_posix(),
                dependency.sha256,
            )
            if digest_key not in self._digests:
                digest_bytes += (
                    272
                    + len(digest_key[0].encode("ascii"))
                    + len(digest_key[1].encode("utf-8"))
                    + len(digest_key[2])
                )
        return CompactResultGrowth(
            configuration_index_bytes=192,
            coverage_index_bytes=coverage_bytes,
            finding_index_bytes=finding_bytes,
            digest_state_bytes=digest_bytes,
        )

    def accept_validated_result(
        self,
        configuration: PreprocessConfiguration,
        result: ConfigurationAuditResult,
        ownership: CompactResultOwnership,
        *,
        caller_retains_ownership: bool = False,
    ) -> None:
        if not isinstance(caller_retains_ownership, bool):
            raise AuditInfrastructureError("streaming result ownership is invalid")
        try:
            self._accept_validated_result_impl(
                configuration, result, ownership
            )
        finally:
            result = None
        if not caller_retains_ownership:
            ownership.release(semantic_event="release-result")

    def _accept_validated_result_impl(
        self,
        configuration: PreprocessConfiguration,
        result: ConfigurationAuditResult,
        ownership: CompactResultOwnership,
    ) -> None:
        if self._finished:
            raise AuditInfrastructureError("streaming result aggregate is already finished")
        index = self._expected_index.get(configuration.digest)
        if (
            index is None
            or configuration != self._expected[index]
            or configuration.digest in self._accepted
        ):
            raise AuditInfrastructureError("streaming result configuration is unexpected")
        if (
            not isinstance(result, ConfigurationAuditResult)
            or result.configuration_digest != configuration.digest
        ):
            raise AuditInfrastructureError("streaming result configuration digest differs")
        if (
            not isinstance(ownership, CompactResultOwnership)
            or ownership.budget is not self._budget
            or not ownership.committed
            or ownership.byte_count < compact_result_retained_bytes(result)
        ):
            raise AuditInfrastructureError("streaming result ownership is invalid")
        workspace = self._budget.reserve(
            4096, label="aggregate growth workspace"
        ).commit()
        try:
            growth = self._growth_for(result)
            if growth.total_bytes > self._limits.compact_aggregate_bytes:
                raise AuditInfrastructureError(
                    "aggregate growth exceeds 128 MiB limit"
                )
            try:
                growth_ownership = self._budget.reserve(
                    growth.total_bytes, label="aggregate growth 128 MiB limit"
                ).commit()
            except AuditInfrastructureError as error:
                raise AuditInfrastructureError(
                    "aggregate growth exceeds 128 MiB limit before insert"
                ) from error
        finally:
            workspace.release()

        reached_keys: list[str] = []
        finding_changes: list[tuple[tuple[str, int, str, str], bool]] = []
        digest_keys: list[tuple[str, str, str]] = []
        accepted_inserted = False
        growth_appended = False
        journal_appended = False
        try:
            self._accepted.add(configuration.digest)
            accepted_inserted = True
            _after_streaming_aggregate_mutation("accepted")
            for path in result.reached_production:
                path_key = path.as_posix()
                if path_key not in self._reached:
                    self._reached[path_key] = path
                    reached_keys.append(path_key)
            _after_streaming_aggregate_mutation("reached")
            for finding in result.findings:
                key = _audit_finding_key(finding)
                existing = self._findings.get(key)
                if existing is None:
                    self._findings[key] = (finding, [configuration.digest])
                    finding_changes.append((key, True))
                else:
                    existing[1].append(configuration.digest)
                    finding_changes.append((key, False))
            _after_streaming_aggregate_mutation("finding")
            for dependency in result.dependencies:
                key = (
                    dependency.stable_role,
                    dependency.role_relative_path.as_posix(),
                    dependency.sha256,
                )
                if key not in self._digests:
                    self._digests[key] = None
                    digest_keys.append(key)
            _after_streaming_aggregate_mutation("digest")
            journal = _StreamingAggregateMutation(
                configuration.digest,
                tuple(reached_keys),
                tuple(finding_changes),
                tuple(digest_keys),
                growth_ownership,
            )
            self._growth_ownerships.append(growth_ownership)
            growth_appended = True
            self._mutation_journals.append(journal)
            journal_appended = True
            _after_streaming_aggregate_mutation("ownership")
        except BaseException:
            if journal_appended:
                popped_journal = self._mutation_journals.pop()
                if popped_journal.growth_ownership is not growth_ownership:
                    raise AuditInfrastructureError(
                        "streaming aggregate journal order differs"
                    )
            if growth_appended:
                popped_growth = self._growth_ownerships.pop()
                if popped_growth is not growth_ownership:
                    raise AuditInfrastructureError(
                        "streaming aggregate ownership order differs"
                    )
            for key in reversed(digest_keys):
                self._digests.pop(key)
            for key, inserted in reversed(finding_changes):
                if inserted:
                    self._findings.pop(key)
                else:
                    existing = self._findings[key]
                    if not existing[1] or existing[1][-1] != configuration.digest:
                        raise AuditInfrastructureError(
                            "streaming aggregate finding rollback differs"
                        )
                    existing[1].pop()
            for key in reversed(reached_keys):
                self._reached.pop(key)
            if accepted_inserted:
                self._accepted.discard(configuration.digest)
            if not growth_ownership.released:
                growth_ownership.release()
            raise

    def _checkpoint(self) -> _StreamingAggregateCheckpoint:
        if self._finished:
            raise AuditInfrastructureError(
                "streaming result aggregate is already finished"
            )
        cold_ownership = self._cold_ownership
        if cold_ownership is not None and cold_ownership.released:
            cold_ownership = None
        return _StreamingAggregateCheckpoint(
            len(self._mutation_journals), cold_ownership
        )

    def _rollback_to(self, checkpoint: _StreamingAggregateCheckpoint) -> None:
        if (
            self._finished
            or not isinstance(checkpoint, _StreamingAggregateCheckpoint)
            or checkpoint.mutation_count < 0
            or checkpoint.mutation_count > len(self._mutation_journals)
            or (
                checkpoint.cold_ownership is not None
                and checkpoint.cold_ownership.released
            )
        ):
            raise AuditInfrastructureError(
                "streaming aggregate rollback checkpoint is invalid"
            )
        while len(self._mutation_journals) > checkpoint.mutation_count:
            journal = self._mutation_journals.pop()
            growth = self._growth_ownerships.pop()
            if growth is not journal.growth_ownership:
                raise AuditInfrastructureError(
                    "streaming aggregate rollback ownership differs"
                )
            for key in reversed(journal.digest_keys):
                self._digests.pop(key)
            for key, inserted in reversed(journal.finding_changes):
                if inserted:
                    self._findings.pop(key)
                else:
                    existing = self._findings[key]
                    if (
                        not existing[1]
                        or existing[1][-1] != journal.configuration_digest
                    ):
                        raise AuditInfrastructureError(
                            "streaming aggregate rollback finding differs"
                        )
                    existing[1].pop()
            for key in reversed(journal.reached_keys):
                self._reached.pop(key)
            self._accepted.remove(journal.configuration_digest)
            growth.release()
        current_cold = self._cold_ownership
        if current_cold is not checkpoint.cold_ownership:
            if current_cold is not None and not current_cold.released:
                current_cold.release()
            self._cold_ownership = checkpoint.cold_ownership

    def finish(self) -> StreamingAuditSummary:
        if self._finished:
            raise AuditInfrastructureError("streaming result aggregate is already finished")
        accepted_count = len(self._accepted)
        finding_configuration_count = sum(
            len(configurations)
            for _finding, configurations in self._findings.values()
        )
        finalization_bytes = (
            8192
            + 24 * accepted_count
            + 24 * len(self._reached)
            + 32 * len(self._digests)
            + 192 * len(self._findings)
            + 24 * finding_configuration_count
            + 16 * len(self._growth_ownerships)
        )
        replaced_cold_bytes: int | None = None
        try:
            if (
                self._cold_ownership is not None
                and not self._cold_ownership.released
            ):
                replaced_cold_bytes = self._cold_ownership.byte_count
                finalization_ownership = self._cold_ownership.replace_committed(
                    finalization_bytes,
                    label="streaming aggregate final immutable copy",
                )
                self._cold_ownership = None
            else:
                finalization_ownership = self._budget.reserve(
                    finalization_bytes,
                    label="streaming aggregate final immutable copy",
                ).commit()
        except AuditInfrastructureError as error:
            raise AuditInfrastructureError(
                "streaming aggregate finalization exceeds 128 MiB before copy"
            ) from error
        try:
            configurations = tuple(
                configuration.digest for configuration in self._expected
                if configuration.digest in self._accepted
            )
            reached = tuple(self._reached[key] for key in sorted(self._reached))
            findings = tuple(
                AggregatedAuditResultFinding(
                    finding,
                    tuple(
                        sorted(
                            finding_configurations,
                            key=self._expected_index.__getitem__,
                        )
                    ),
                )
                for _key, (finding, finding_configurations) in sorted(
                    self._findings.items()
                )
            )
            digests = tuple(sorted(self._digests))
            if self._base_ownership is None:
                raise AuditInfrastructureError(
                    "streaming result base ownership is unavailable"
                )
            ownerships = (
                self._base_ownership,
                *self._growth_ownerships,
                finalization_ownership,
            )
            summary = StreamingAuditSummary(
                configurations,
                reached,
                findings,
                digests,
                ownerships,
            )
        except BaseException:
            if replaced_cold_bytes is not None:
                self._cold_ownership = finalization_ownership.replace_committed(
                    replaced_cold_bytes,
                    label="batch retained result limit cold slot",
                )
            else:
                finalization_ownership.release()
            raise
        self._finished = True
        self._base_ownership = None
        self._growth_ownerships.clear()
        self._mutation_journals.clear()
        self._accepted.clear()
        self._reached.clear()
        self._findings.clear()
        self._digests.clear()
        self._expected_index.clear()
        return summary


def encode_local_dependency_digest(dependency: DependencyDigest) -> bytes:
    if not isinstance(dependency, DependencyDigest):
        raise AuditInfrastructureError("local dependency is invalid")
    identity = dependency.identity
    record = {
        "stable_role": dependency.stable_role,
        "role_relative_path": dependency.role_relative_path.as_posix(),
        "canonical": str(identity.canonical),
        "relative": identity.relative.as_posix() if identity.relative is not None else None,
        "device": identity.device,
        "inode": identity.inode,
        "line_count": identity.line_count,
        "production": identity.production,
        "sha256": dependency.sha256,
    }
    return json.dumps(record, ensure_ascii=True, separators=(",", ":")).encode("ascii")


def _decode_role_relative_path(value: object) -> PurePosixPath:
    if not isinstance(value, str) or not value:
        raise AuditInfrastructureError("local dependency role-relative path is invalid")
    if (
        "\\" in value
        or value.startswith("/")
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise AuditInfrastructureError("local dependency role-relative path is invalid")
    return PurePosixPath(value)


def decode_local_dependency_digest(
    payload: bytes,
    *,
    expected: DependencyDigest | None = None,
) -> DependencyDigest:
    if not isinstance(payload, bytes):
        raise AuditInfrastructureError("local dependency payload is invalid")
    if len(payload) > _LOCAL_DEPENDENCY_MAX_BYTES:
        raise AuditInfrastructureError("local dependency payload is too large")
    try:
        pairs = json.loads(
            payload.decode("ascii"),
            object_pairs_hook=lambda items: items,
            parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
        )
    except (
        UnicodeError,
        ValueError,
        TypeError,
        RecursionError,
        json.JSONDecodeError,
    ) as error:
        raise AuditInfrastructureError("local dependency payload is invalid") from error
    if not isinstance(pairs, list) or any(
        not isinstance(pair, tuple)
        or len(pair) != 2
        or not isinstance(pair[0], str)
        for pair in pairs
    ):
        raise AuditInfrastructureError("local dependency schema is invalid")
    if tuple(pair[0] for pair in pairs) != _LOCAL_DEPENDENCY_FIELDS:
        raise AuditInfrastructureError("local dependency schema is invalid")
    values = dict(pairs)
    stable_role = values["stable_role"]
    role_path_text = values["role_relative_path"]
    canonical = values["canonical"]
    relative_text = values["relative"]
    if not isinstance(canonical, str):
        raise AuditInfrastructureError("local dependency path fields are invalid")
    _validate_native_canonical_text(canonical)
    if relative_text is not None and not isinstance(relative_text, str):
        raise AuditInfrastructureError("local dependency relative identity is invalid")
    identity = FileIdentity(
        canonical=Path(canonical),
        relative=(
            _decode_role_relative_path(relative_text)
            if relative_text is not None
            else None
        ),
        device=values["device"],
        inode=values["inode"],
        line_count=values["line_count"],
        production=values["production"],
    )
    try:
        decoded = DependencyDigest(
            stable_role=stable_role,
            role_relative_path=_decode_role_relative_path(role_path_text),
            identity=identity,
            sha256=values["sha256"],
        )
    except (TypeError, ValueError) as error:
        raise AuditInfrastructureError("local dependency payload is invalid") from error
    if encode_local_dependency_digest(decoded) != payload:
        raise AuditInfrastructureError(
            "canonical local dependency payload does not round trip exactly"
        )
    if expected is not None:
        if not isinstance(expected, DependencyDigest) or decoded != expected:
            raise AuditInfrastructureError("local dependency identity was replaced")
    return decoded
