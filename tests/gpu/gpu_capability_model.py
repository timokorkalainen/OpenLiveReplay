"""Shared immutable model for the compiler-authoritative GPU capability audit."""

from __future__ import annotations

import enum
import hashlib
import json
import os
import stat
import struct
import sys
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
            access = 0 if is_directory else 0x80000000  # GENERIC_READ
            share = 0x1 | (0x2 if is_directory else 0)  # never FILE_SHARE_DELETE
            flags = 0x02000000 if is_directory else 0x00000080
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
                offset += 16 + name_length
                if offset > len(payload):
                    raise AuditInfrastructureError("Linux inotify event stream is invalid")
                if mask & 0x00004000:
                    raise AuditInfrastructureError("Linux inotify generation event overflow")
                if mask & (0x00002000 | 0x00008000):
                    raise AuditInfrastructureError("Linux inotify generation watch was lost")
                raise AuditInfrastructureError("Linux inotify generation change observed")

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
    workers: int = field(default_factory=_default_worker_count)


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

    def __post_init__(self) -> None:
        _validate_digest(self.configuration_digest, "configuration")
        _validate_digest(self.audit_engine_fingerprint, "audit engine fingerprint")
        if not isinstance(self.dependencies, tuple) or any(
            not isinstance(item, DependencyDigest) for item in self.dependencies
        ):
            raise AuditInfrastructureError("audit result dependencies are invalid")
        dependency_keys = tuple(_dependency_sort_key(item) for item in self.dependencies)
        if dependency_keys != tuple(sorted(dependency_keys)) or len(set(dependency_keys)) != len(dependency_keys):
            raise AuditInfrastructureError("audit result dependencies are not unique and sorted")
        if not isinstance(self.reached_production, tuple):
            raise AuditInfrastructureError("audit result reached-production paths are invalid")
        reached_keys: list[str] = []
        for path in self.reached_production:
            _stable_role, validated = _validate_role_relative_path("production", path)
            reached_keys.append(validated.as_posix())
        if tuple(reached_keys) != tuple(sorted(reached_keys)) or len(set(reached_keys)) != len(reached_keys):
            raise AuditInfrastructureError("audit result reached-production paths are not unique and sorted")
        if not isinstance(self.findings, tuple) or any(
            not isinstance(item, AuditResultFinding) for item in self.findings
        ):
            raise AuditInfrastructureError("audit result findings are invalid")
        finding_keys = tuple(_audit_finding_key(item) for item in self.findings)
        if finding_keys != tuple(sorted(finding_keys)) or len(set(finding_keys)) != len(finding_keys):
            raise AuditInfrastructureError("audit result findings are not unique and sorted")


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
