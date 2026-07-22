"""Fail-closed compile-command normalization for the GPU capability audit."""

from __future__ import annotations

import hashlib
import json
import locale
import os
import platform
import re
import secrets
import select
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import sysconfig
import tempfile
import threading
import time
from collections import deque
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from types import MappingProxyType

from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CompilerExecutableCapability,
    CompilerExecPermit,
    CompilerFamily,
    CompilerLaunchEvent,
    CompilerLaunchPurpose,
    CompilerInspection,
    CommandArgumentSpan,
    CommandRewriteClassification,
    _FilesystemGenerationObserver,
    DependencyDigest,
    DependencyRootAuthority,
    DependencyRootBinding,
    FileIdentity,
    MacOSInspectionExecPermit,
    PreprocessConfiguration,
    ProcessStartIdentity,
    portable_compiler_inspection_key,
    validate_dependency_root_authority,
)


_LAUNCHERS = frozenset({"ccache", "sccache", "distcc", "icecc"})
_CCACHE_VALUE_OPTIONS = frozenset({
    "--compiler",
    "--compiler-check",
    "--compiler-type",
    "--config-path",
    "--dir",
    "--namespace",
    "--set-config",
    "--trim-dir",
    "-o",
})
_CCACHE_FLAG_OPTIONS = frozenset({"--ccache-skip"})
_CCACHE_COMPILER_OPTIONS = frozenset({"--compiler", "--compiler-type"})
_CCACHE_COMPILER_ASSIGNMENTS = frozenset({
    "compiler", "compiler_type", "prefix_command", "prefix_command_cpp"
})
_CCACHE_COMPILER_ENVIRONMENT = frozenset({
    "ccache_compiler", "ccache_compilertype", "ccache_prefix", "ccache_prefix_cpp",
    "ccache_cc",
})
_ASSIGNMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_.-]*=(.*)\Z", re.DOTALL)
_GNU_VALUE_OPTIONS = frozenset({
    "-o", "-D", "-U", "-A", "-I", "-isystem", "-iquote", "-idirafter",
    "-iprefix", "-iwithprefix", "-iwithprefixbefore", "-include", "-imacros",
    "--output", "--dump", "-d", "--define-macro", "--undefine-macro",
    "--include", "--imacros",
    "-isysroot", "--sysroot", "-x", "-target", "--target", "-arch", "-MF",
    "-MT", "-MQ", "-MJ", "-F", "-iframework", "-include-pch", "-include-pth",
    "-Xclang", "-Xpreprocessor", "-Xassembler", "-Xlinker", "-B", "-specs",
    "-wrapper", "-fmodule-file", "-fmodule-map-file",
    "-serialize-diagnostics", "--serialize-diagnostics", "-dependency-file",
    "--dependency-file",
})
_MSVC_VALUE_OPTIONS = frozenset({
    "/D", "/U", "/I", "/FI", "/Fo", "/Fe", "/Fd", "/Fi",
    "/Fp", "/Ft", "/Yu", "/Yc", "/experimental:log",
    "/sourceDependencies", "/scanDependencies", "/external:I", "/AI", "/FU",
    "/ifcOutput", "/reference", "/headerUnit",
})
# cl and clang-cl switches are case-sensitive. This is the exact option grammar
# the audit itself interprets; unknown switches remain compiler-authoritative.
# Exact spellings are accepted with either the '/' or '-' introducer.
_MSVC_CASE_SENSITIVE_EXACT_OPTIONS = _MSVC_VALUE_OPTIONS | frozenset({
    "/c", "/nologo", "/E", "/P", "/EP", "/showIncludes",
    "/PD", "/PH", "/Fx", "/doc", "/TP", "/TC", "/Tp", "/Tc",
    "/FA", "/Fa", "/Fm", "/FR", "/Fr", "/d1PP",
})
_MSVC_CASE_SENSITIVE_PREFIX_OPTIONS = (
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
    "/experimental:log", "/external:I", "/showIncludes:",
    "/headerUnit", "/reference", "/clang:",
    "/Fo", "/Fe", "/Fd", "/Fi", "/Ft", "/Fp", "/FI",
    "/FA", "/Fa", "/Fm", "/FR", "/Fr",
    "/Yu", "/Yc", "/AI", "/FU", "/D", "/U", "/I",
    "/OUT", "/link", "/LD", "/clr:netcore", "/Tp", "/Tc",
    "/doc",
)
# These documented switches deliberately differ only by case from a shorter
# switch above. Recognize them before checking misspellings so, for example,
# /u never acquires /U's operand and /interface never looks like attached /I.
_MSVC_CASE_DISTINCT_EXACT_OPTIONS = frozenset({
    "/C", "/u", "/utf-8", "/interface", "/internalPartition",
    "/exportHeader", "/translateInclude", "/validate-charset",
})
_MSVC_CASE_DISTINCT_PREFIX_OPTIONS = (
    "/arch:", "/diagnostics:", "/execution-charset:", "/favor:",
    "/fp:", "/source-charset:", "/Zc:",
)
# Only spellings that could be mistaken for a source-selection, marker, output,
# or dependency control are rejected case-insensitively. Broad prefix folding
# is incorrect for MSVC: /u versus /U, /C versus /c, /fp versus /Fp, and
# /interface versus /I all have distinct documented meanings.
_MSVC_AMBIGUOUS_EXACT_OPTIONS = frozenset({
    "/c", "/E", "/P", "/EP", "/showIncludes", "/PD", "/PH", "/Fx",
    "/doc", "/TP", "/TC", "/Tp", "/Tc",
})
_MSVC_AMBIGUOUS_PREFIX_OPTIONS = (
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
    "/experimental:log", "/showIncludes:", "/Fo", "/Fe", "/Fd", "/Fi",
    "/Ft", "/FA", "/Fa", "/Fm", "/FR", "/Fr", "/OUT", "/link",
    "/LD", "/clr:netcore", "/Yc", "/Tp", "/Tc", "/doc",
)
_VERSION_SECONDS = 5.0
_VERSION_BYTES = 1024 * 1024
_RUNTIME_ENUMERATION_ENTRIES = 4096
_RUNTIME_CLOSURE_FILES = 256
_RUNTIME_PATH_COMPONENTS = 256
_RUNTIME_PATH_METADATA_BYTES = 256 * 1024
_RUNTIME_CONTEXT_STATES = 1024
_RUNTIME_CONTEXT_METADATA_BYTES = 8 * 1024 * 1024
_RUNTIME_FILE_BYTES = 256 * 1024 * 1024
_RUNTIME_TOTAL_BYTES = 1024 * 1024 * 1024
_RUNTIME_TREE_ENTRIES = 65536
_RUNTIME_TREE_DIRECTORIES = 4096
_REPARSE_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
_compiler_inspection_lock = threading.Lock()
_compiler_capability_lock = threading.Lock()
_compiler_capability_memo: dict[tuple[object, ...], CompilerExecutableCapability] = {}
_compiler_inspection_memo: dict[
    tuple[object, ...],
    CompilerInspection,
] = {}


def _macos_proc_bsdinfo_type():
    import ctypes

    class ProcBsdInfo(ctypes.Structure):
        _fields_ = (
            ("pbi_flags", ctypes.c_uint32),
            ("pbi_status", ctypes.c_uint32),
            ("pbi_xstatus", ctypes.c_uint32),
            ("pbi_pid", ctypes.c_uint32),
            ("pbi_ppid", ctypes.c_uint32),
            ("pbi_uid", ctypes.c_uint32),
            ("pbi_gid", ctypes.c_uint32),
            ("pbi_ruid", ctypes.c_uint32),
            ("pbi_rgid", ctypes.c_uint32),
            ("pbi_svuid", ctypes.c_uint32),
            ("pbi_svgid", ctypes.c_uint32),
            ("rfu_1", ctypes.c_uint32),
            ("pbi_comm", ctypes.c_char * 16),
            ("pbi_name", ctypes.c_char * 32),
            ("pbi_nfiles", ctypes.c_uint32),
            ("pbi_pgid", ctypes.c_uint32),
            ("pbi_pjobc", ctypes.c_uint32),
            ("e_tdev", ctypes.c_uint32),
            ("e_tpgid", ctypes.c_uint32),
            ("pbi_nice", ctypes.c_int32),
            ("pbi_start_tvsec", ctypes.c_uint64),
            ("pbi_start_tvusec", ctypes.c_uint64),
        )

    if ctypes.sizeof(ProcBsdInfo) != 136:
        raise AuditInfrastructureError("macOS proc_bsdinfo ABI size differs")
    return ProcBsdInfo


def _native_process_start_token(process: subprocess.Popen[bytes], platform_kind: str) -> str:
    if platform_kind == "windows":
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetProcessTimes.argtypes = (
            wintypes.HANDLE,
            ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
            ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
        )
        kernel32.GetProcessTimes.restype = wintypes.BOOL
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel_time = wintypes.FILETIME()
        user_time = wintypes.FILETIME()
        process_handle = getattr(process, "_handle", None)
        if process_handle is None or not kernel32.GetProcessTimes(
            process_handle, ctypes.byref(creation), ctypes.byref(exit_time),
            ctypes.byref(kernel_time), ctypes.byref(user_time),
        ):
            raise AuditInfrastructureError("cannot authenticate Windows process start")
        value = (int(creation.dwHighDateTime) << 32) | int(creation.dwLowDateTime)
        return f"windows-filetime:{value}"
    if platform_kind == "linux":
        try:
            stat_text = Path(f"/proc/{process.pid}/stat").read_text(encoding="ascii")
            close = stat_text.rfind(")")
            fields = stat_text[close + 2:].split()
            start_ticks = fields[19]
            boot_id = Path("/proc/sys/kernel/random/boot_id").read_text(
                encoding="ascii"
            ).strip()
        except (OSError, IndexError, ValueError) as error:
            raise AuditInfrastructureError("cannot authenticate Linux process start") from error
        if not boot_id or not start_ticks.isdigit():
            raise AuditInfrastructureError("Linux process start identity is invalid")
        return f"linux-proc:{boot_id}:{start_ticks}"
    if platform_kind == "macos":
        import ctypes

        info_type = _macos_proc_bsdinfo_type()
        libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        query = libproc.proc_pidinfo
        query.argtypes = (
            ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
            ctypes.c_void_p, ctypes.c_int,
        )
        query.restype = ctypes.c_int
        info = info_type()
        received = query(process.pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info))
        if received != ctypes.sizeof(info) or info.pbi_pid != process.pid:
            raise AuditInfrastructureError("cannot authenticate macOS process start")
        return f"macos-proc:{info.pbi_start_tvsec}:{info.pbi_start_tvusec}"
    raise AuditInfrastructureError("compiler process platform identity is invalid")


class _MacProcessIdentityHandle:
    """Retained kqueue process registration that survives child reaping."""

    __slots__ = ("_queue", "_pid", "_note_exit", "_closed")

    def __init__(self, queue, pid: int, note_exit: int) -> None:
        self._queue = queue
        self._pid = pid
        self._note_exit = note_exit
        self._closed = False

    def validate_exit(self) -> None:
        if self._closed:
            raise AuditInfrastructureError("macOS process identity handle is closed")
        try:
            events = self._queue.control([], 1, 0)
        except OSError as error:
            raise AuditInfrastructureError(
                "cannot validate retained macOS compiler process identity"
            ) from error
        if (
            len(events) != 1
            or int(events[0].ident) != self._pid
            or not (int(events[0].fflags) & self._note_exit)
        ):
            raise AuditInfrastructureError("post-exit process identity differs")

    def close(self) -> None:
        if not self._closed:
            self._queue.close()
            self._closed = True


def _open_macos_process_identity_handle(pid: int) -> _MacProcessIdentityHandle:
    import select

    try:
        queue = select.kqueue()
        registration = select.kevent(
            pid,
            filter=select.KQ_FILTER_PROC,
            flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
            fflags=select.KQ_NOTE_EXIT,
        )
        queue.control([registration], 0, 0)
    except (AttributeError, OSError) as error:
        try:
            queue.close()
        except (AttributeError, UnboundLocalError):
            pass
        raise AuditInfrastructureError(
            "cannot retain macOS compiler process identity"
        ) from error
    return _MacProcessIdentityHandle(queue, pid, select.KQ_NOTE_EXIT)


class CompilerProcessHandleCarrier:
    """Generation-bound parent-observable owner of one compiler process handle."""

    __slots__ = (
        "event", "_process", "_observer", "_completed", "_linux_pidfd",
        "_macos_identity_handle", "_windows_process_handle",
    )

    def __init__(self, event: CompilerLaunchEvent, process, observer) -> None:
        self.event = event
        self._process = process
        self._observer = observer
        self._completed = False
        self._linux_pidfd = None
        self._macos_identity_handle = None
        self._windows_process_handle = (
            getattr(process, "_handle", None)
            if event.process_start.platform_kind == "windows"
            else None
        )
        if event.process_start.platform_kind == "linux" and hasattr(os, "pidfd_open"):
            try:
                self._linux_pidfd = os.pidfd_open(process.pid)
            except OSError as error:
                raise AuditInfrastructureError(
                    "cannot retain Linux compiler process identity"
                ) from error
        if event.process_start.platform_kind == "macos":
            self._macos_identity_handle = _open_macos_process_identity_handle(
                process.pid
            )

    @property
    def process(self):
        return self._process

    @property
    def completed(self) -> bool:
        return self._completed

    @property
    def windows_process_handle(self) -> int:
        if (
            self.event.process_start.platform_kind != "windows"
            or self._completed
            or self._windows_process_handle is None
        ):
            raise AuditInfrastructureError(
                "Windows compiler process handle is unavailable"
            )
        return int(self._windows_process_handle)

    def complete_after_exit(self) -> None:
        if self._completed:
            raise AuditInfrastructureError("compiler process carrier was already completed")
        primary_error: BaseException | None = None
        try:
            identity = self.event.process_start
            if self._linux_pidfd is not None:
                try:
                    os.fstat(self._linux_pidfd)
                except OSError as error:
                    raise AuditInfrastructureError(
                        "post-exit process identity differs"
                    ) from error
            elif self._macos_identity_handle is not None:
                self._macos_identity_handle.validate_exit()
            else:
                observed = _native_process_start_token(
                    self._process, identity.platform_kind
                )
                if observed != identity.native_start_token:
                    raise AuditInfrastructureError(
                        "post-exit process identity differs"
                    )
            complete = getattr(
                self._observer, "complete_compiler_process_launch", None
            )
            if not callable(complete):
                raise AuditInfrastructureError(
                    "parent compiler process completion observer is invalid"
                )
            complete(self.event, self)
        except BaseException as identity_error:
            primary_error = identity_error
            fail = getattr(
                self._observer, "fail_compiler_process_launch", None
            )
            if callable(fail):
                try:
                    fail(self.event, self)
                except BaseException as cleanup_error:
                    identity_error.add_note(
                        f"parent process carrier failure cleanup also failed: {cleanup_error}"
                    )
        self._completed = True
        cleanup_errors = []
        if self._linux_pidfd is not None:
            descriptor = self._linux_pidfd
            self._linux_pidfd = None
            try:
                os.close(descriptor)
            except BaseException as error:
                cleanup_errors.append(error)
        if self._macos_identity_handle is not None:
            identity_handle = self._macos_identity_handle
            self._macos_identity_handle = None
            try:
                identity_handle.close()
            except BaseException as error:
                cleanup_errors.append(error)
        if self.event.process_start.platform_kind == "windows":
            process_handle = self._windows_process_handle
            self._windows_process_handle = None
            close_process_handle = getattr(process_handle, "Close", None)
            try:
                if not callable(close_process_handle):
                    raise AuditInfrastructureError(
                        "Windows compiler process handle is unavailable"
                    )
                close_process_handle()
            except BaseException as error:
                cleanup_errors.append(error)
        self._process = None
        if primary_error is not None:
            for cleanup_error in cleanup_errors:
                primary_error.add_note(
                    f"carrier resource cleanup also failed: {cleanup_error}"
                )
            raise primary_error
        if cleanup_errors:
            cleanup_error = AuditInfrastructureError(
                "compiler process carrier resource cleanup failed"
            )
            for error in cleanup_errors[1:]:
                cleanup_error.add_note(
                    f"additional carrier resource cleanup failed: {error}"
                )
            raise cleanup_error from cleanup_errors[0]

    def abort_before_return(self) -> None:
        if self._completed:
            raise AuditInfrastructureError("compiler process carrier was already completed")
        primary_error: BaseException | None = None
        fail = getattr(self._observer, "fail_compiler_process_launch", None)
        try:
            if callable(fail):
                fail(self.event, self)
        except BaseException as error:
            primary_error = error
        self._completed = True
        cleanup_errors = []
        if self.event.process_start.platform_kind == "windows":
            process_handle = self._windows_process_handle
            self._windows_process_handle = None
            close_process_handle = getattr(process_handle, "Close", None)
            try:
                if not callable(close_process_handle):
                    raise AuditInfrastructureError(
                        "Windows compiler process handle is unavailable"
                    )
                close_process_handle()
            except BaseException as error:
                cleanup_errors.append(error)
        if self._linux_pidfd is not None:
            descriptor = self._linux_pidfd
            self._linux_pidfd = None
            try:
                os.close(descriptor)
            except BaseException as error:
                cleanup_errors.append(error)
        if self._macos_identity_handle is not None:
            identity_handle = self._macos_identity_handle
            self._macos_identity_handle = None
            try:
                identity_handle.close()
            except BaseException as error:
                cleanup_errors.append(error)
        self._process = None
        if primary_error is not None:
            for cleanup_error in cleanup_errors:
                primary_error.add_note(
                    f"carrier resource cleanup also failed: {cleanup_error}"
                )
            raise primary_error
        if cleanup_errors:
            cleanup_error = AuditInfrastructureError(
                "compiler process carrier resource cleanup failed"
            )
            for error in cleanup_errors[1:]:
                cleanup_error.add_note(
                    f"additional carrier resource cleanup failed: {error}"
                )
            raise cleanup_error from cleanup_errors[0]


class _ParentCompilerLaunchObserver:
    __slots__ = ("_active", "_lock")

    def __init__(self) -> None:
        self._active = {}
        self._lock = threading.Lock()

    @property
    def active_count(self) -> int:
        with self._lock:
            return len(self._active)

    def register_compiler_process_launch(self, event, carrier) -> None:
        with self._lock:
            if event.process_start in self._active or any(
                identity.pid == event.process_start.pid
                for identity in self._active
            ):
                raise AuditInfrastructureError("compiler process carrier is duplicated")
            self._active[event.process_start] = carrier

    def complete_compiler_process_launch(self, event, carrier) -> None:
        with self._lock:
            if self._active.get(event.process_start) is not carrier:
                raise AuditInfrastructureError("compiler process carrier differs")
            del self._active[event.process_start]

    def fail_compiler_process_launch(self, event, carrier) -> None:
        with self._lock:
            if self._active.get(event.process_start) is carrier:
                del self._active[event.process_start]


_parent_compiler_launch_observer = _ParentCompilerLaunchObserver()


_MACOS_EXEC_GATE_MAX_BYTES = 4096
_MACOS_EXEC_GATE_SCHEMA = "olr-gpu-macos-compiler-exec-gate-v1"


def _macos_exec_gate_payload(kind: str, token: str, pid: int, pgid: int,
                             start_identity: str) -> bytes:
    payload = json.dumps(
        {
            "kind": kind,
            "pgid": pgid,
            "pid": pid,
            "schema": _MACOS_EXEC_GATE_SCHEMA,
            "start_identity": start_identity,
            "token": token,
        },
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")
    if not (0 < len(payload) <= _MACOS_EXEC_GATE_MAX_BYTES):
        raise AuditInfrastructureError("macOS compiler exec gate payload is invalid")
    return payload


def _decode_macos_exec_gate_payload(payload: bytes, *, expected_kind: str,
                                    token: str) -> tuple[int, int, str]:
    try:
        value = json.loads(payload.decode("ascii"))
        if (
            not isinstance(value, dict)
            or set(value) != {
                "kind", "pgid", "pid", "schema", "start_identity", "token"
            }
            or value["kind"] != expected_kind
            or value["schema"] != _MACOS_EXEC_GATE_SCHEMA
            or value["token"] != token
            or not isinstance(value["pid"], int)
            or isinstance(value["pid"], bool)
            or value["pid"] <= 0
            or not isinstance(value["pgid"], int)
            or isinstance(value["pgid"], bool)
            or value["pgid"] != value["pid"]
            or not isinstance(value["start_identity"], str)
            or not value["start_identity"]
            or json.dumps(
                value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
            ).encode("ascii") != payload
        ):
            raise ValueError("invalid")
        return value["pid"], value["pgid"], value["start_identity"]
    except (KeyError, TypeError, ValueError, UnicodeError,
            json.JSONDecodeError) as error:
        raise AuditInfrastructureError(
            "macOS compiler exec permit is invalid"
        ) from error


def _write_length_prefixed_fd(descriptor: int, payload: bytes) -> None:
    frame = struct.pack(">I", len(payload)) + payload
    offset = 0
    while offset < len(frame):
        written = os.write(descriptor, frame[offset:])
        if written <= 0:
            raise AuditInfrastructureError("macOS compiler exec gate write failed")
        offset += written


def _read_length_prefixed_fd(descriptor: int, deadline: float | None = None,
                             cancel_event=None) -> bytes:
    def read_exact(count: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < count:
            if cancel_event is not None and cancel_event.is_set():
                raise AuditInfrastructureError(
                    "macOS compiler exec permit was cancelled"
                )
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AuditInfrastructureError(
                        "macOS compiler exec permit deadline exceeded"
                    )
                readable, _writable, _errors = select.select(
                    (descriptor,), (), (), min(remaining, 0.05)
                )
                if not readable:
                    continue
            chunk = os.read(descriptor, count - len(chunks))
            if not chunk:
                raise AuditInfrastructureError(
                    "macOS compiler exec permit channel closed"
                )
            chunks.extend(chunk)
        return bytes(chunks)

    length = struct.unpack(">I", read_exact(4))[0]
    if not (0 < length <= _MACOS_EXEC_GATE_MAX_BYTES):
        raise AuditInfrastructureError("macOS compiler exec permit is invalid")
    return read_exact(length)


def _native_macos_self_start_identity() -> tuple[int, int, str]:
    pid = os.getpid()
    pgid = os.getpgrp()

    class _SelfProcess:
        pass

    process = _SelfProcess()
    process.pid = pid
    token = _native_process_start_token(process, "macos")
    return pid, pgid, token.removeprefix("macos-proc:")


def _run_macos_compiler_exec_gate(report_descriptor: int,
                                   permit_descriptor: int, token: str,
                                   command: tuple[str, ...]) -> None:
    if (
        not isinstance(report_descriptor, int)
        or not isinstance(permit_descriptor, int)
        or not isinstance(token, str)
        or re.fullmatch(r"[0-9a-f]{64}", token) is None
        or not isinstance(command, tuple)
        or not command
        or any(not isinstance(item, str) or not item for item in command)
    ):
        raise AuditInfrastructureError("macOS compiler exec gate input is invalid")
    pid, pgid, start_identity = _native_macos_self_start_identity()
    report = _macos_exec_gate_payload(
        "report", token, pid, pgid, start_identity
    )
    try:
        _write_length_prefixed_fd(report_descriptor, report)
    finally:
        os.close(report_descriptor)
    permit = _read_length_prefixed_fd(permit_descriptor)
    os.close(permit_descriptor)
    actual = _decode_macos_exec_gate_payload(
        permit, expected_kind="permit", token=token
    )
    if actual != (pid, pgid, start_identity):
        raise AuditInfrastructureError("macOS compiler exec permit is invalid")
    os.execvpe(command[0], command, dict(os.environ))
    raise AuditInfrastructureError("macOS compiler exec returned unexpectedly")


def _audit_task_ordinal(task_id: str | None) -> int:
    match = re.fullmatch(r"audit-([0-9]+)-[0-9a-f]{16}", task_id or "")
    if match is None:
        raise AuditInfrastructureError("macOS compiler exec permit task is invalid")
    return int(match.group(1))


def _validate_macos_compiler_exec_permit(
    permit: object,
    process_start: ProcessStartIdentity,
    *,
    worker_index: int | None,
    task_id: str | None,
    generation: int | None,
) -> None:
    if worker_index is None and task_id is None and generation is None:
        if (
            not isinstance(permit, MacOSInspectionExecPermit)
            or permit.pid != process_start.pid
            or permit.pgid != process_start.pid
        ):
            raise AuditInfrastructureError("macOS inspection exec permit differs")
        return
    if (
        not isinstance(permit, CompilerExecPermit)
        or permit.worker_index != worker_index
        or permit.generation != generation
        or permit.task_id != _audit_task_ordinal(task_id)
        or permit.pgid != process_start.pid
    ):
        raise AuditInfrastructureError("macOS compiler exec permit differs")


class _MacOSCompilerExecGate:
    """Start a session leader, register it, and permit compiler exec once."""

    def __init__(self, command, environment, launch_options) -> None:
        if sys.platform != "darwin":
            raise AuditInfrastructureError("macOS compiler exec gate requires macOS")
        self._token = secrets.token_hex(32)
        report_read, report_write = os.pipe()
        permit_read, permit_write = os.pipe()
        self._report_read = report_read
        self._report_write = report_write
        self._permit_read = permit_read
        self._permit_write = permit_write
        self.command = (
            sys.executable,
            str(Path(__file__).resolve()),
            "--macos-compiler-exec-gate",
            str(report_write),
            str(permit_read),
            self._token,
            *tuple(command),
        )
        self.environment = dict(environment)
        options = dict(launch_options)
        inherited = tuple(options.get("pass_fds", ()))
        options["pass_fds"] = tuple(sorted(set(
            (*inherited, report_write, permit_read)
        )))
        self.launch_options = options

    def authorize(self, process, observer, *, worker_index, task_id,
                  generation) -> ProcessStartIdentity:
        os.close(self._report_write)
        self._report_write = -1
        os.close(self._permit_read)
        self._permit_read = -1
        deadline = getattr(observer, "macos_launch_deadline", None)
        if (
            not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or time.monotonic() >= deadline
        ):
            raise AuditInfrastructureError(
                "macOS compiler exec permit deadline is invalid"
            )
        payload = _read_length_prefixed_fd(
            self._report_read,
            float(deadline),
            getattr(observer, "macos_launch_cancel_event", None),
        )
        pid, pgid, start_identity = _decode_macos_exec_gate_payload(
            payload, expected_kind="report", token=self._token
        )
        if pid != process.pid or pgid != process.pid:
            raise AuditInfrastructureError("macOS compiler exec report differs")
        process_start = ProcessStartIdentity(
            "macos", pid, start_identity, secrets.token_hex(32)
        )
        authorize = getattr(observer, "authorize_macos_compiler_exec", None)
        if not callable(authorize):
            raise AuditInfrastructureError(
                "macOS compiler exec permit authority is unavailable"
            )
        permit = authorize(process_start)
        _validate_macos_compiler_exec_permit(
            permit,
            process_start,
            worker_index=worker_index,
            task_id=task_id,
            generation=generation,
        )
        _write_length_prefixed_fd(
            self._permit_write,
            _macos_exec_gate_payload(
                "permit", self._token, pid, pgid, start_identity
            ),
        )
        os.close(self._permit_write)
        self._permit_write = -1
        return process_start

    def close(self) -> None:
        for name in (
            "_report_read", "_report_write", "_permit_read", "_permit_write"
        ):
            descriptor = getattr(self, name, -1)
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
                setattr(self, name, -1)


def launch_compiler_process(
    prepared_arguments,
    *,
    cwd: Path,
    environment: Mapping[str, str],
    containment,
    platform_kind: str,
    stdin,
    stdout,
    stderr,
    launch_options: Mapping[str, object],
    purpose: CompilerLaunchPurpose,
    launch_observer=None,
    worker_index: int | None = None,
    task_id: str | None = None,
    generation: int | None = None,
):
    if not isinstance(purpose, CompilerLaunchPurpose):
        raise AuditInfrastructureError("compiler launch purpose is invalid")
    observer = (
        _parent_compiler_launch_observer
        if launch_observer is None else launch_observer
    )
    gate = None
    effective_arguments = prepared_arguments
    effective_environment = dict(environment)
    effective_launch_options = dict(launch_options)
    if platform_kind == "macos" and callable(
        getattr(observer, "authorize_macos_compiler_exec", None)
    ):
        gate = _MacOSCompilerExecGate(
            prepared_arguments, effective_environment, effective_launch_options
        )
        effective_arguments = gate.command
        effective_environment = gate.environment
        effective_launch_options = gate.launch_options
    try:
        process = subprocess.Popen(
            effective_arguments,
            cwd=str(cwd), env=effective_environment, shell=False,
            stdin=stdin, stdout=stdout, stderr=stderr,
            **effective_launch_options, **containment.popen_arguments,
        )
    except BaseException:
        if gate is not None:
            gate.close()
        raise
    carrier = None
    try:
        containment.attach(process)
        identity = (
            gate.authorize(
                process,
                observer,
                worker_index=worker_index,
                task_id=task_id,
                generation=generation,
            )
            if gate is not None
            else ProcessStartIdentity(
                platform_kind, process.pid,
                _native_process_start_token(process, platform_kind),
                secrets.token_hex(32),
            )
        )
        event = CompilerLaunchEvent(
            purpose, identity, worker_index=worker_index,
            task_id=task_id, generation=generation,
        )
        carrier = CompilerProcessHandleCarrier(event, process, observer)
        register = getattr(observer, "register_compiler_process_launch", None)
        if not callable(register):
            raise AuditInfrastructureError(
                "parent compiler process launch observer is invalid"
            )
        register(event, carrier)
        containment.release(process)
        if gate is not None:
            gate.close()
        return process, carrier
    except BaseException as launch_error:
        try:
            containment.terminate()
        except BaseException as cleanup_error:
            launch_error.add_note(
                "compiler process containment termination also failed: "
                f"{cleanup_error}"
            )
        try:
            process.kill()
        except (AttributeError, OSError):
            pass
        try:
            process.wait(timeout=1.0)
        except (AttributeError, OSError, subprocess.TimeoutExpired):
            pass
        if carrier is not None and not carrier.completed:
            try:
                carrier.abort_before_return()
            except BaseException as cleanup_error:
                launch_error.add_note(
                    f"compiler process carrier abort also failed: {cleanup_error}"
                )
                for cleanup_note in getattr(cleanup_error, "__notes__", ()):
                    launch_error.add_note(
                        f"compiler process carrier abort detail: {cleanup_note}"
                    )
        if gate is not None:
            try:
                gate.close()
            except BaseException as cleanup_error:
                launch_error.add_note(
                    f"macOS compiler exec gate cleanup also failed: {cleanup_error}"
                )
        raise


def _clear_compiler_inspection_memo_for_tests() -> None:
    with _compiler_inspection_lock:
        _compiler_inspection_memo.clear()
    with _compiler_capability_lock:
        for capability in _compiler_capability_memo.values():
            try:
                capability.native_owner.close()
            except AuditInfrastructureError:
                pass
        _compiler_capability_memo.clear()


@dataclass(frozen=True)
class RewrittenCommand:
    arguments: tuple[str, ...]
    dependency_output: Path
    dependency_format: str
    classification: CommandRewriteClassification | None = None


DECISION_ENVIRONMENT_SCHEMA_BYTES = b"olr-gpu-decision-environment-v1"
DECISION_ARGUMENT_SCHEMA_BYTES = b"olr-gpu-decision-arguments-v1"
COMPILER_DIGEST_SCHEMA_BYTES = b"olr-gpu-compiler-digest-v1"
_DECISION_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".mm"})

_DECISION_GNU_SEMANTIC_PATHS = MappingProxyType({
    "-I": "include-path", "-isystem": "system-include-path",
    "-iquote": "quote-include-path", "-idirafter": "after-include-path",
    "-iprefix": "include-prefix", "-iwithprefix": "include-path",
    "-iwithprefixbefore": "include-path",
    "-include": "forced-include", "-imacros": "forced-macros",
    "--include": "forced-include", "--imacros": "forced-macros",
    "-isysroot": "sysroot", "--sysroot": "sysroot", "-F": "framework-path",
    "-iframework": "framework-path", "-include-pch": "pch-path",
    "-include-pth": "pch-path", "-B": "tool-search-path", "-specs": "specs-path",
    "-fmodule-file": "module-file", "-fmodule-map-file": "module-map-file",
    "-fmodules-cache-path": "module-cache-path",
    "-fprebuilt-module-path": "module-search-path",
    "-fmodules-user-build-path": "module-build-path",
    "-ivfsoverlay": "vfs-overlay", "-resource-dir": "resource-dir",
    "-fprofile-use": "profile-path", "-fprofile-instr-use": "profile-path",
    "-fprofile-sample-use": "profile-path", "-fcoverage-compilation-dir": "coverage-path",
})
_DECISION_GNU_NONSEMANTIC_OUTPUTS = MappingProxyType({
    "-o": "output", "--output": "output", "-MJ": "output",
    "--dump": "output", "-d": "output",
    "-serialize-diagnostics": "output", "--serialize-diagnostics": "output",
    "-fdiagnostics-file": "output",
    "-fdiagnostics-serialization-file": "output",
    "-fmodule-output": "output",
})
_DECISION_GNU_NONSEMANTIC_DEPENDENCIES = MappingProxyType({
    "-MF": "dependency-file", "-MT": "dependency-target",
    "-MQ": "dependency-target", "-dependency-file": "dependency-file",
    "--dependency-file": "dependency-file",
})
_DECISION_GNU_SCALAR_VALUES = frozenset({
    "-D", "-U", "-A", "-x", "-target", "--target", "-arch",
})
_DECISION_MSVC_SEMANTIC_PATHS = MappingProxyType({
    "/I": "include-path", "/FI": "forced-include", "/Fp": "pch-path",
    "/Yu": "pch-path", "/external:I": "system-include-path", "/AI": "assembly-path",
    "/FU": "forced-assembly", "/reference": "module-reference",
    "/headerUnit": "module-reference",
})
_DECISION_MSVC_OUTPUTS = MappingProxyType({
    "/Fo": "output", "/Fe": "output", "/Fd": "output", "/Fi": "output",
    "/Ft": "output", "/ifcOutput": "output", "/experimental:log": "output",
    "/FA": "output", "/Fa": "output", "/Fm": "output",
    "/FR": "output", "/Fr": "output", "/doc": "output",
})
_DECISION_MSVC_DEPENDENCIES = MappingProxyType({
    "/sourceDependencies": "dependency-file", "/scanDependencies": "dependency-file",
    "/sourceDependencies:directives": "dependency-file",
    "/showIncludes": "dependency-output",
})
_DECISION_MSVC_SCALAR_VALUES = frozenset({"/D", "/U"})
_DECISION_LAUNCHER_PATH_ASSIGNMENTS = frozenset({
    "cache_dir", "ccache_basedir", "ccache_configpath", "ccache_dir",
    "ccache_tempdir",
})


def _decision_span(role: str, option_index: int, operand_index: int,
                   attachment: str, forwarded_depth: int = 0) -> CommandArgumentSpan:
    return CommandArgumentSpan(role, option_index, operand_index, attachment,
                               forwarded_depth)


def _decision_attached_option(value: str, options: Mapping[str, str],
                              *, msvc: bool) -> tuple[str, str] | None:
    candidates = sorted(options, key=len, reverse=True)
    for option in candidates:
        if value.startswith(f"{option}=") and len(value) > len(option) + 1:
            return option, options[option]
        if len(value) > len(option) and value.startswith(option):
            if msvc or option in {
                "-I", "-D", "-U", "-A", "-F", "-B", "-o", "-MF",
                "-MT", "-MQ", "-MJ",
            }:
                return option, options[option]
    return None


def _looks_path_bearing(value: str) -> bool:
    if not value:
        return False
    if value.startswith(("-", "/")):
        head = value[:256].casefold()
        separator = "=" if "=" in head else ":" if ":" in head else ""
        option = head.split(separator, 1)[0] if separator else head
        if separator:
            separator_index = value.find(separator)
            payload_index = separator_index + 1
            if (option.endswith((
                    "path", "file", "dir", "root", "include", "framework",
                    "module", "pch", "sdkroot"))
                    or value.find("/", payload_index) >= 0
                    or value.find("\\", payload_index) >= 0
                    or (payload_index < len(value)
                        and value[payload_index] in ".~")
                    or (payload_index + 1 < len(value)
                        and value[payload_index].isalpha()
                        and value[payload_index + 1] == ":")):
                return True
        return (not value.startswith("/")
                and (value.find("/", 1) >= 0 or value.find("\\", 1) >= 0))
    candidate = value
    return (
        "/" in candidate or "\\" in candidate
        or candidate.startswith((".", "~"))
        or bool(re.fullmatch(r"[A-Za-z]:.*", candidate))
        or Path(candidate).suffix.casefold() in {
            ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm",
            ".pch", ".pth", ".pcm", ".modulemap", ".o", ".obj", ".d",
        }
    )


def _validate_classifier_forwarded_controls(
    arguments: tuple[str, ...], compiler_family: CompilerFamily,
) -> None:
    if compiler_family is CompilerFamily.CLANG_CL:
        category = _gnu_forwarded_sequence_control(
            _clang_cl_forwarded_arguments(arguments))
        if category:
            raise AuditInfrastructureError(
                f"hidden {category} option is unsupported: /clang:")
        return
    if compiler_family not in {CompilerFamily.GCC, CompilerFamily.CLANG}:
        return
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value.startswith("-Wp,"):
            category = _gnu_forwarded_sequence_control((value,))
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}")
        forwarder = next((candidate for candidate in _GNU_FORWARDERS
                          if value == candidate
                          or value.startswith(f"{candidate}=")), None)
        if forwarder is None:
            index += 1
            continue
        end, category = _gnu_forwarded_span(arguments, index, forwarder)
        if category:
            raise AuditInfrastructureError(
                f"hidden {category} option is unsupported: {value}")
        index = end


def _forwarded_wp_path(value: str) -> tuple[str, str, str] | None:
    if not value.startswith("-Wp,"):
        return None
    separator = value.find(",", 4)
    if separator < 0 or value.find(",", separator + 1) >= 0:
        return None
    option = value[4:separator]
    role = _DECISION_GNU_SEMANTIC_PATHS.get(option)
    if role is None:
        return None
    operand = value[separator + 1:]
    prefix = value[:separator + 1]
    return role, prefix, operand


def classify_command_rewrite(
    raw_arguments: tuple[str, ...], cwd: Path, compiler_family: CompilerFamily
) -> CommandRewriteClassification:
    """Freeze one typed parse shared by rewrite and decision normalization."""

    if (not isinstance(raw_arguments, tuple)
            or any(not isinstance(value, str) for value in raw_arguments)
            or not isinstance(cwd, Path) or not cwd.is_absolute()
            or not isinstance(compiler_family, CompilerFamily)):
        raise AuditInfrastructureError("command rewrite classification input is invalid")
    arguments = raw_arguments
    _validate_classifier_forwarded_controls(arguments, compiler_family)
    sources: list[CommandArgumentSpan] = []
    outputs: list[CommandArgumentSpan] = []
    dependencies: list[CommandArgumentSpan] = []
    scalars: list[CommandArgumentSpan] = []
    paths: list[CommandArgumentSpan] = []
    msvc = compiler_family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
    path_options = _DECISION_MSVC_SEMANTIC_PATHS if msvc else _DECISION_GNU_SEMANTIC_PATHS
    output_options = _DECISION_MSVC_OUTPUTS if msvc else _DECISION_GNU_NONSEMANTIC_OUTPUTS
    dependency_options = (
        _DECISION_MSVC_DEPENDENCIES if msvc
        else _DECISION_GNU_NONSEMANTIC_DEPENDENCIES
    )
    scalar_values = _DECISION_MSVC_SCALAR_VALUES if msvc else _DECISION_GNU_SCALAR_VALUES
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if not msvc and not value.startswith("-"):
            sources.append(_decision_span("source", index, index, "source"))
            index += 1
            continue
        if (not msvc and value in _GNU_FORWARDERS and index + 1 < len(arguments)):
            forwarded_option = arguments[index + 1]
            if (index + 3 < len(arguments)
                    and arguments[index + 2] in _GNU_FORWARDERS
                    and forwarded_option in _DECISION_GNU_SEMANTIC_PATHS):
                scalars.extend((
                    _decision_span("forwarder", index, index, "scalar", 1),
                    _decision_span("forwarded-option", index + 1, index + 1,
                                   "scalar", 1),
                    _decision_span("forwarder", index + 2, index + 2,
                                   "scalar", 1),
                ))
                paths.append(_decision_span(
                    _DECISION_GNU_SEMANTIC_PATHS[forwarded_option],
                    index + 3, index + 3, "forwarded", 1))
                index += 4
                continue
        if not msvc:
            wp_path = _forwarded_wp_path(value)
            if wp_path is not None:
                paths.append(_decision_span(
                    wp_path[0], index, index, "forwarded-comma", 1))
                index += 1
                continue
        if (msvc and value.startswith("/clang:") and index + 1 < len(arguments)):
            forwarded_option = value[len("/clang:"):]
            if (forwarded_option in _DECISION_GNU_SEMANTIC_PATHS
                    and arguments[index + 1].startswith("/clang:")):
                scalars.append(_decision_span(
                    "forwarded-option", index, index, "scalar", 1))
                paths.append(_decision_span(
                    _DECISION_GNU_SEMANTIC_PATHS[forwarded_option],
                    index + 1, index + 1, "forwarded", 1))
                index += 2
                continue
        option = _msvc_option(value) if msvc and value.startswith(("/", "-")) else value
        if msvc and option in {"/TC", "/TP"}:
            scalars.append(_decision_span("language-mode", index, index, "scalar"))
            index += 1
            continue
        if msvc and option in {"/Tc", "/Tp"}:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(
                    f"compiler option requires a source: {value}")
            sources.append(_decision_span("source", index, index + 1, "separate"))
            index += 2
            continue
        if msvc and (option.startswith(("/Tc", "/Tp"))
                     and len(option) > 3):
            sources.append(_decision_span("source", index, index, "attached"))
            index += 1
            continue
        if msvc and option in {"/FA", "/Fa", "/Fm", "/FR", "/Fr", "/doc"}:
            outputs.append(_decision_span("output", index, index, "attached"))
            index += 1
            continue
        if msvc and option == "/showIncludes":
            dependencies.append(_decision_span(
                "dependency-output", index, index, "scalar"))
            index += 1
            continue
        matched = False
        for table, destination in (
            (path_options, paths), (output_options, outputs),
            (dependency_options, dependencies),
        ):
            if option in table:
                if index + 1 >= len(arguments):
                    raise AuditInfrastructureError(
                        f"compiler option requires a value: {value}")
                destination.append(_decision_span(table[option], index, index + 1,
                                                  "separate"))
                index += 2
                matched = True
                break
            attached = _decision_attached_option(option, table, msvc=msvc)
            if attached is not None:
                destination.append(_decision_span(attached[1], index, index, "attached"))
                index += 1
                matched = True
                break
        if matched:
            continue
        if option in scalar_values:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            scalars.append(_decision_span("semantic-scalar", index, index + 1,
                                         "separate"))
            index += 2
            continue
        scalar_attached = _decision_attached_option(
            option, {name: "semantic-scalar" for name in scalar_values}, msvc=msvc)
        if scalar_attached is not None:
            scalars.append(_decision_span("semantic-scalar", index, index, "attached"))
            index += 1
            continue
        forwarded_role = _forwarded_decision_path_role(arguments, index, msvc)
        if forwarded_role is not None:
            paths.append(_decision_span(forwarded_role, index, index, "forwarded", 1))
            index += 1
            continue
        if ((not msvc and not value.startswith("-"))
                or (msvc and not value.startswith(("-", "/")))):
            sources.append(_decision_span("source", index, index, "source"))
            index += 1
            continue
        if _looks_path_bearing(value):
            raise AuditInfrastructureError(
                f"unclassified path-bearing option: {value}")
        scalars.append(_decision_span("semantic-scalar", index, index, "scalar"))
        index += 1
    return CommandRewriteClassification(
        response_expanded_launcher_stripped_arguments=arguments,
        compiler=_decision_span("compiler", -1, -1, "compiler"),
        sources=tuple(sources), nonsemantic_outputs=tuple(outputs),
        nonsemantic_dependencies=tuple(dependencies),
        semantic_scalars=tuple(scalars), semantic_paths=tuple(paths))


def _forwarded_decision_path_role(arguments: tuple[str, ...], index: int,
                                  msvc: bool) -> str | None:
    if msvc and index > 0 and arguments[index].startswith("/clang:"):
        payload = arguments[index][len("/clang:"):]
        attached = _decision_attached_option(
            payload, _DECISION_GNU_SEMANTIC_PATHS, msvc=False)
        if attached is not None:
            return attached[1]
        previous = arguments[index - 1]
        if previous.startswith("/clang:"):
            option = previous[len("/clang:"):]
            return _DECISION_GNU_SEMANTIC_PATHS.get(option)
    if index >= 3 and arguments[index - 1] in _GNU_FORWARDERS:
        option = arguments[index - 2]
        if arguments[index - 3] in _GNU_FORWARDERS:
            return _DECISION_GNU_SEMANTIC_PATHS.get(option)
    return None


def _attached_operand(value: str, role: str, compiler_family: CompilerFamily) -> tuple[str, str]:
    options = (_DECISION_MSVC_SEMANTIC_PATHS
               if compiler_family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
               else _DECISION_GNU_SEMANTIC_PATHS)
    for option, candidate_role in sorted(options.items(), key=lambda item: len(item[0]),
                                         reverse=True):
        if candidate_role != role:
            continue
        if value.startswith(f"{option}="):
            return f"{option}=", value[len(option) + 1:]
        if value.startswith(option) and len(value) > len(option):
            return option, value[len(option):]
    raise AuditInfrastructureError("unclassified path-bearing option")


def _normalize_typed_path(value: str, cwd: Path, working_directory_role: str,
                          source_root: Path, build_root: Path,
                          toolchain_root: Path) -> str:
    path = Path(value)
    if not path.is_absolute():
        path = cwd / path
    normalized = Path(os.path.normpath(path))
    roots = (
        (source_root, "<SOURCE_ROOT>"), (build_root, "<BUILD_ROOT>"),
        (toolchain_root, "<TOOLCHAIN_ROOT>"),
        (cwd, f"<WORKING_DIRECTORY:{working_directory_role}>"),
    )
    for root, marker in roots:
        try:
            suffix = normalized.relative_to(Path(os.path.normpath(root)))
        except ValueError:
            continue
        return marker if not suffix.parts else f"{marker}/{suffix.as_posix()}"
    raise AuditInfrastructureError("decision semantic path is outside typed roots")


def normalize_decision_arguments(
    rewritten: RewrittenCommand, cwd: Path, working_directory_role: str,
    source_root: Path, build_root: Path, toolchain_root: Path,
) -> tuple[str, ...]:
    if (not isinstance(rewritten, RewrittenCommand)
            or not isinstance(rewritten.classification, CommandRewriteClassification)
            or not isinstance(working_directory_role, str)
            or not working_directory_role):
        raise AuditInfrastructureError("decision argument classification is invalid")
    classification = rewritten.classification
    arguments = classification.response_expanded_launcher_stripped_arguments
    excluded = {span.option_index for span in (
        classification.sources + classification.nonsemantic_outputs
        + classification.nonsemantic_dependencies)}
    excluded.update(span.operand_index for span in (
        classification.sources + classification.nonsemantic_outputs
        + classification.nonsemantic_dependencies)
        if span.attachment == "separate")
    path_by_option = {span.option_index: span for span in classification.semantic_paths}
    result: list[str] = []
    index = 0
    while index < len(arguments):
        if index in excluded:
            index += 1
            continue
        span = path_by_option.get(index)
        if span is None:
            result.append(arguments[index])
            index += 1
            continue
        if span.attachment == "separate":
            result.extend((arguments[index], _normalize_typed_path(
                arguments[span.operand_index], cwd, working_directory_role,
                source_root, build_root, toolchain_root)))
            index = span.operand_index + 1
        elif span.attachment == "forwarded":
            token = arguments[index]
            prefix = "/clang:" if token.startswith("/clang:") else ""
            operand = token[len(prefix):]
            try:
                option_prefix, attached_operand = _attached_operand(
                    operand, span.role, CompilerFamily.CLANG)
            except AuditInfrastructureError:
                option_prefix, attached_operand = "", operand
            result.append(prefix + option_prefix + _normalize_typed_path(
                attached_operand, cwd, working_directory_role, source_root,
                build_root, toolchain_root))
            index += 1
        elif span.attachment == "forwarded-comma":
            wp_path = _forwarded_wp_path(arguments[index])
            if wp_path is None or wp_path[0] != span.role:
                raise AuditInfrastructureError("unclassified path-bearing option")
            result.append(wp_path[1] + _normalize_typed_path(
                wp_path[2], cwd, working_directory_role, source_root,
                build_root, toolchain_root))
            index += 1
        else:
            prefix, operand = _attached_operand(
                arguments[index], span.role,
                CompilerFamily.MSVC if arguments[index].startswith("/")
                else CompilerFamily.CLANG)
            result.append(prefix + _normalize_typed_path(
                operand, cwd, working_directory_role, source_root, build_root,
                toolchain_root))
            index += 1
    return tuple(result)


def decision_environment_digest(
    compiler_family: CompilerFamily, effective_environment: Mapping[str, str],
    explicit_launcher_assignments: Mapping[str, str], cwd: Path,
    working_directory_role: str, source_root: Path, build_root: Path,
    toolchain_root: Path,
) -> str:
    if not isinstance(compiler_family, CompilerFamily):
        raise AuditInfrastructureError("decision environment family is invalid")
    _environment_digest(effective_environment)
    common_paths = {
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "COMPILER_PATH", "GCC_EXEC_PREFIX",
    }
    path_names = set(common_paths) if compiler_family in {
        CompilerFamily.GCC, CompilerFamily.CLANG
    } else {"INCLUDE", "WindowsSdkDir"}
    scalar_names = {"MACOSX_DEPLOYMENT_TARGET"} if compiler_family is CompilerFamily.CLANG else set()
    if compiler_family is CompilerFamily.CLANG:
        path_names.add("SDKROOT")
    if compiler_family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        _reject_msvc_environment_arguments(effective_environment)
    normalized: list[tuple[str, str]] = []
    for name in sorted(path_names | scalar_names, key=str.casefold):
        actual = next((key for key in effective_environment
                       if key.casefold() == name.casefold()), None)
        if actual is None or not effective_environment[actual]:
            continue
        value = effective_environment[actual]
        if name in path_names:
            parts = value.split(os.pathsep)
            value = os.pathsep.join(_normalize_typed_path(
                part, cwd, working_directory_role, source_root, build_root,
                toolchain_root) for part in parts)
        normalized.append((name, value))
    for name, value in sorted(explicit_launcher_assignments.items(),
                              key=lambda item: (item[0].casefold(), item[0])):
        if (not isinstance(name, str) or not name or not isinstance(value, str)
                or any(secret in name.casefold() for secret in (
                    "token", "secret", "credential", "password"))):
            raise AuditInfrastructureError("decision launcher environment is invalid")
        canonical_path_name = next(
            (candidate for candidate in path_names
             if candidate.casefold() == name.casefold()), None)
        launcher_path = name.casefold() in _DECISION_LAUNCHER_PATH_ASSIGNMENTS
        if canonical_path_name is not None or launcher_path:
            value = os.pathsep.join(_normalize_typed_path(
                part, cwd, working_directory_role, source_root, build_root,
                toolchain_root) for part in value.split(os.pathsep))
        elif _looks_path_bearing(value):
            raise AuditInfrastructureError(
                "unclassified path-bearing launcher assignment")
        normalized.append((f"launcher:{name}", value))
    digest = hashlib.sha256()
    _hash_field(digest, DECISION_ENVIRONMENT_SCHEMA_BYTES)
    _hash_field(digest, compiler_family.value.encode("ascii"))
    for name, value in normalized:
        _hash_field(digest, name.encode("utf-8"))
        _hash_field(digest, value.encode("utf-8"))
    return digest.hexdigest()


def compiler_digest(compiler_family: CompilerFamily, executable: Path,
                    normalized_version: str) -> str:
    if (not isinstance(compiler_family, CompilerFamily)
            or not isinstance(executable, Path)
            or not isinstance(normalized_version, str)
            or not normalized_version.strip()):
        raise AuditInfrastructureError("compiler decision identity is invalid")
    content = hashlib.sha256()
    try:
        with executable.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                content.update(chunk)
    except OSError as error:
        raise AuditInfrastructureError("compiler decision identity is unavailable") from error
    return _compiler_decision_digest(
        compiler_family, normalized_version, content.hexdigest())


def _compiler_decision_digest(compiler_family: CompilerFamily,
                              normalized_version: str,
                              executable_sha256: str) -> str:
    if (not isinstance(compiler_family, CompilerFamily)
            or not isinstance(normalized_version, str)
            or not normalized_version.strip()
            or re.fullmatch(r"[0-9a-f]{64}", executable_sha256) is None):
        raise AuditInfrastructureError("compiler decision identity is invalid")
    digest = hashlib.sha256()
    _hash_field(digest, COMPILER_DIGEST_SCHEMA_BYTES)
    _hash_field(digest, compiler_family.value.encode("ascii"))
    _hash_field(digest, normalized_version.encode("utf-8"))
    _hash_field(digest, bytes.fromhex(executable_sha256))
    return digest.hexdigest()


def compiler_digest_from_capability(
    compiler_family: CompilerFamily,
    capability: CompilerExecutableCapability,
    normalized_version: str,
    *,
    deadline: float | None = None,
) -> str:
    """Bind the decision to the already-held executable, never its pathname."""

    if not isinstance(capability, CompilerExecutableCapability):
        raise AuditInfrastructureError("compiler decision capability is invalid")
    owner = capability.native_owner
    if not isinstance(owner, _CompilerCapabilityOwner):
        raise AuditInfrastructureError("compiler decision capability owner is invalid")
    owner.validate(content=True, deadline=deadline, cancel_event=None)
    actual = _compiler_decision_digest(
        compiler_family, normalized_version, capability.executable_sha256)
    owner.validate(content=True, deadline=deadline, cancel_event=None)
    return actual


def _launcher_name(value: str) -> str:
    return Path(value.replace("\\", "/")).name.casefold().removesuffix(".exe")


def _windows_command_line_split(command: str) -> tuple[str, ...]:
    """Use CommandLineToArgvW on Windows and its documented rules elsewhere."""

    quoted = False
    slashes = 0
    for character in command:
        if character == "\\":
            slashes += 1
            continue
        if character == '"' and slashes % 2 == 0:
            quoted = not quoted
        slashes = 0
    if quoted:
        raise AuditInfrastructureError("unsupported Windows command quoting")

    if os.name == "nt":
        import ctypes

        argc = ctypes.c_int()
        shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        shell32.CommandLineToArgvW.argtypes = (
            ctypes.c_wchar_p,
            ctypes.POINTER(ctypes.c_int),
        )
        shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
        kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
        kernel32.LocalFree.restype = ctypes.c_void_p
        pointer = shell32.CommandLineToArgvW(command, ctypes.byref(argc))
        if not pointer:
            raise AuditInfrastructureError("CommandLineToArgvW failed")
        try:
            return tuple(pointer[index] for index in range(argc.value))
        finally:
            kernel32.LocalFree(pointer)

    arguments: list[str] = []
    index = 0
    length = len(command)
    while index < length:
        while index < length and command[index] in " \t":
            index += 1
        if index == length:
            break
        value: list[str] = []
        quoted = False
        while index < length and (quoted or command[index] not in " \t"):
            if command[index] == "\\":
                start = index
                while index < length and command[index] == "\\":
                    index += 1
                slashes = index - start
                if index < length and command[index] == '"':
                    value.extend("\\" * (slashes // 2))
                    if slashes % 2:
                        value.append('"')
                        index += 1
                    else:
                        if quoted and index + 1 < length and command[index + 1] == '"':
                            value.append('"')
                            index += 2
                        else:
                            quoted = not quoted
                            index += 1
                else:
                    value.extend("\\" * slashes)
                continue
            if command[index] == '"':
                if quoted and index + 1 < length and command[index + 1] == '"':
                    value.append('"')
                    index += 2
                else:
                    quoted = not quoted
                    index += 1
                continue
            value.append(command[index])
            index += 1
        if quoted:
            raise AuditInfrastructureError("unsupported Windows command quoting")
        arguments.append("".join(value))
    return tuple(arguments)


def _token_has_shell_syntax(token: str) -> bool:
    if "$(" in token or "`" in token:
        return True
    if token in {"|", "||", "&&", ";", "<", "<<", ">", ">>", "<>"}:
        return True
    return re.fullmatch(r"(?:\d+|&)?(?:>>?|<<?|<>|>&|<&).*", token) is not None


def _validate_arguments(arguments: tuple[str, ...]) -> None:
    if not arguments or not arguments[0]:
        raise AuditInfrastructureError("compile command is empty")
    for argument in arguments:
        if not isinstance(argument, str):
            raise AuditInfrastructureError("compile command arguments must be strings")
        if not argument or "\0" in argument:
            raise AuditInfrastructureError("compile command contains an empty or NUL argument")
        if _token_has_shell_syntax(argument):
            raise AuditInfrastructureError(f"unsupported shell syntax: {argument}")


def _posix_command_split(command: str) -> tuple[str, ...]:
    lexer = shlex.shlex(command, posix=True, punctuation_chars="|&;<>")
    lexer.whitespace_split = True
    lexer.commenters = ""
    return tuple(lexer)


def decode_compile_entry(
    entry: Mapping[str, object],
    database: Path,
    windows: bool,
) -> tuple[Path, tuple[str, ...]]:
    """Decode one compile database entry without invoking a shell."""

    if not isinstance(entry, Mapping):
        raise AuditInfrastructureError("compile database entry must be an object")
    directory_value = entry.get("directory")
    file_value = entry.get("file")
    if not isinstance(directory_value, str) or not directory_value or "\0" in directory_value:
        raise AuditInfrastructureError("compile command directory must be a non-empty string")
    if not isinstance(file_value, str) or not file_value or "\0" in file_value:
        raise AuditInfrastructureError("compile command file must be a non-empty string")
    directory = Path(directory_value)
    if not directory.is_absolute():
        directory = database.parent / directory
    try:
        directory = directory.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile command directory is unavailable: {directory_value}"
        ) from error
    if not directory.is_dir():
        raise AuditInfrastructureError(f"compile command directory is not a directory: {directory}")

    structured = entry.get("arguments")
    if structured is not None:
        if not isinstance(structured, list):
            raise AuditInfrastructureError("compile command arguments must be an array")
        if any(not isinstance(value, str) for value in structured):
            raise AuditInfrastructureError("compile command arguments must be strings")
        arguments = tuple(structured)
    else:
        command = entry.get("command")
        if not isinstance(command, str) or not command.strip():
            raise AuditInfrastructureError("compile command has neither arguments nor command")
        try:
            arguments = (
                _windows_command_line_split(command)
                if windows
                else _posix_command_split(command)
            )
        except (ValueError, OSError) as error:
            raise AuditInfrastructureError("unsupported compile command quoting") from error
    _validate_arguments(arguments)
    return directory, arguments


def strip_launchers(
    arguments: tuple[str, ...],
) -> tuple[Path, tuple[str, ...], Mapping[str, str]]:
    """Remove only the explicitly supported launcher grammar."""

    if not arguments:
        raise AuditInfrastructureError("compile command has no executable")
    index = 0
    assignments: dict[str, str] = {}
    while index < len(arguments) and _launcher_name(arguments[index]) in _LAUNCHERS:
        launcher = _launcher_name(arguments[index])
        index += 1
        if launcher != "ccache":
            if index < len(arguments) and arguments[index].startswith("-"):
                raise AuditInfrastructureError(
                    f"unsupported {launcher} option: {arguments[index]}"
                )
            continue
        while index < len(arguments):
            token = arguments[index]
            if token == "--":
                index += 1
                break
            option_name = token.partition("=")[0]
            if option_name in _CCACHE_VALUE_OPTIONS:
                if option_name in _CCACHE_COMPILER_OPTIONS:
                    raise AuditInfrastructureError(
                        f"ccache compiler override is unsupported: {token}"
                    )
                if "=" in token:
                    if not token.partition("=")[2]:
                        raise AuditInfrastructureError(
                            f"ccache option requires a value: {option_name}"
                        )
                    index += 1
                else:
                    if index + 1 >= len(arguments):
                        raise AuditInfrastructureError(
                            f"ccache option requires a value: {token}"
                        )
                    index += 2
                continue
            if token in _CCACHE_FLAG_OPTIONS:
                index += 1
                continue
            assignment = _ASSIGNMENT.fullmatch(token)
            if assignment:
                name, _, value = token.partition("=")
                if name.casefold().replace("-", "_") in _CCACHE_COMPILER_ASSIGNMENTS:
                    raise AuditInfrastructureError(
                        f"ccache compiler override is unsupported: {token}"
                    )
                if not value:
                    raise AuditInfrastructureError(f"invalid ccache assignment: {token}")
                if name in assignments:
                    raise AuditInfrastructureError(f"duplicate ccache assignment: {name}")
                assignments[name] = value
                index += 1
                continue
            if token.startswith("-"):
                raise AuditInfrastructureError(f"unsupported ccache option: {token}")
            if "=" in token:
                raise AuditInfrastructureError(f"invalid ccache assignment: {token}")
            break
    if index >= len(arguments):
        raise AuditInfrastructureError("compile command has launchers but no compiler")
    return Path(arguments[index]), tuple(arguments[index + 1 :]), assignments


def _validate_ccache_config_sources(
    arguments: tuple[str, ...], cwd: Path, environment: Mapping[str, str]
) -> None:
    paths: list[str] = []
    index = 0
    while index < len(arguments) and _launcher_name(arguments[index]) in _LAUNCHERS:
        launcher = _launcher_name(arguments[index])
        index += 1
        if launcher != "ccache":
            continue
        while index < len(arguments):
            token = arguments[index]
            if token == "--":
                index += 1
                break
            option_name, separator, attached = token.partition("=")
            if option_name in _CCACHE_VALUE_OPTIONS:
                if separator:
                    value = attached
                    index += 1
                else:
                    if index + 1 >= len(arguments):
                        return  # strip_launchers() reports the precise syntax error.
                    value = arguments[index + 1]
                    index += 2
                if option_name == "--config-path":
                    paths.append(value)
                if option_name == "--set-config":
                    _reject_ccache_config_assignment(value, "--set-config")
                continue
            if token in _CCACHE_FLAG_OPTIONS or _ASSIGNMENT.fullmatch(token):
                index += 1
                continue
            break

    environment_config: str | None = None
    for name, value in environment.items():
        if name.casefold() == "ccache_configpath":
            environment_config = value
            break
    if environment_config:
        paths.append(environment_config)
    canonical_paths: list[Path] = []
    for value in paths:
        path = Path(value)
        if not path.is_absolute():
            path = cwd / path
        try:
            canonical = path.resolve(strict=True)
            metadata = canonical.stat()
        except OSError as error:
            raise AuditInfrastructureError(f"ccache config is unreadable: {path}") from error
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > 1024 * 1024:
            raise AuditInfrastructureError(f"ccache config is invalid or too large: {canonical}")
        if canonical not in canonical_paths:
            canonical_paths.append(canonical)
    if len(canonical_paths) > 1:
        raise AuditInfrastructureError("ambiguous ccache config paths")
    for path in canonical_paths:
        try:
            text = path.read_text(encoding="utf-8-sig", errors="strict")
        except (OSError, UnicodeError) as error:
            raise AuditInfrastructureError(f"ccache config is unreadable: {path}") from error
        for line in text.splitlines():
            content = line.split("#", 1)[0].strip()
            if not content or "=" not in content:
                continue
            _reject_ccache_config_assignment(content, str(path))


def _reject_ccache_config_assignment(value: str, origin: str) -> None:
    name = value.partition("=")[0].strip().casefold().replace("-", "_")
    if name in _CCACHE_COMPILER_ASSIGNMENTS:
        raise AuditInfrastructureError(
            f"ccache config {origin} contains compiler override {name}"
        )


def _family_from_name(path: Path) -> CompilerFamily:
    name = _launcher_name(str(path))
    if name == "clang-cl":
        return CompilerFamily.CLANG_CL
    if name == "cl":
        return CompilerFamily.MSVC
    if re.fullmatch(r"(?:.*-)?clang(?:\+\+)?(?:-\d+(?:\.\d+)*)?", name):
        return CompilerFamily.CLANG
    if re.fullmatch(r"(?:.*-)?(?:gcc|g\+\+)(?:-\d+(?:\.\d+)*)?", name):
        return CompilerFamily.GCC
    raise AuditInfrastructureError(f"unsupported compiler or wrapper: {path}")


def _normalize_version_output(version_output: bytes) -> bytes:
    if not isinstance(version_output, bytes) or not version_output:
        raise AuditInfrastructureError("compiler version probe produced no output")
    if len(version_output) > _VERSION_BYTES:
        raise AuditInfrastructureError("compiler version output limit exceeded")
    if b"\0" in version_output:
        raise AuditInfrastructureError("compiler version probe output is invalid")
    try:
        text = version_output.decode(
            locale.getpreferredencoding(False), errors="strict"
        )
    except UnicodeDecodeError as error:
        raise AuditInfrastructureError("compiler version probe output is undecodable") from error
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    normalized = "\n".join(lines).encode("utf-8")
    if not normalized:
        raise AuditInfrastructureError("compiler version probe produced no output")
    return normalized


def identify_compiler(path: Path, version_output: bytes) -> CompilerFamily:
    """Require executable name and bounded version output to agree."""

    family = _family_from_name(path)
    text = _normalize_version_output(version_output).decode("utf-8").casefold()
    clang = "clang version" in text
    msvc = "microsoft" in text and "compiler version" in text
    gcc = (
        re.search(r"(?:^|[^a-z0-9_])gcc(?:[^a-z0-9_]|$)", text) is not None
        or re.search(r"(?:^|[^a-z0-9_])g\+\+(?:[^a-z0-9_]|$)", text) is not None
        or "free software foundation" in text
    )
    expected = {
        CompilerFamily.GCC: gcc and not clang and not msvc,
        CompilerFamily.CLANG: clang and not msvc,
        CompilerFamily.MSVC: msvc and not clang,
        CompilerFamily.CLANG_CL: clang and not msvc,
    }[family]
    if not expected:
        raise AuditInfrastructureError(
            f"contradictory compiler version probe for {path.name}"
        )
    return family


def _response_text(data: bytes, family: CompilerFamily, path: Path) -> str:
    try:
        if data.startswith(b"\xff\xfe"):
            if family not in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
                raise AuditInfrastructureError(
                    f"unsupported response-file encoding: {path}"
                )
            text = data[2:].decode("utf-16-le", errors="strict")
        elif data.startswith(b"\xfe\xff"):
            raise AuditInfrastructureError(f"unsupported response-file encoding: {path}")
        else:
            payload = data[3:] if data.startswith(b"\xef\xbb\xbf") else data
            text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise AuditInfrastructureError(f"invalid response-file encoding: {path}") from error
    if "\0" in text or text.startswith("\ufeff"):
        raise AuditInfrastructureError(f"ambiguous response-file encoding: {path}")
    return text


def _response_arguments(text: str, family: CompilerFamily, path: Path) -> tuple[str, ...]:
    try:
        if family in {CompilerFamily.GCC, CompilerFamily.CLANG}:
            return tuple(shlex.split(text, posix=True))
        # CommandLineToArgvW gives argv[0] special quoting semantics and does
        # not treat CR/LF as whitespace. Driver command files parse each line.
        result: list[str] = []
        for line in text.splitlines():
            if line.strip():
                result.extend(
                    _windows_command_line_split(f'gpu-capability-response {line}')[1:]
                )
        return tuple(result)
    except (ValueError, OSError, AuditInfrastructureError) as error:
        raise AuditInfrastructureError(f"unsupported response-file quoting: {path}") from error


def expand_response_files(
    arguments: tuple[str, ...],
    family: CompilerFamily,
    cwd: Path,
    limits: AuditLimits,
) -> tuple[str, ...]:
    """Recursively expand driver response files under fixed fail-closed bounds."""

    if family not in set(CompilerFamily):
        raise AuditInfrastructureError(f"unsupported compiler family: {family}")
    seen_paths: set[str] = set()
    seen_identities: dict[tuple[int, int], Path] = {}
    active: set[str] = set()
    aggregate_bytes = 0

    def expand(values: tuple[str, ...], depth: int) -> list[str]:
        nonlocal aggregate_bytes
        result: list[str] = []
        for value in values:
            if not value.startswith("@"):
                result.append(value)
                continue
            if len(value) == 1:
                raise AuditInfrastructureError("response-file path is empty")
            next_depth = depth + 1
            if next_depth > limits.response_depth:
                raise AuditInfrastructureError("response-file depth limit exceeded")
            requested = Path(value[1:])
            if not requested.is_absolute():
                requested = cwd / requested
            try:
                canonical = requested.resolve(strict=True)
                metadata = canonical.stat()
            except (OSError, RuntimeError) as error:
                raise AuditInfrastructureError(f"response file is unreadable: {requested}") from error
            if not stat.S_ISREG(metadata.st_mode):
                raise AuditInfrastructureError(f"response file is not regular: {canonical}")
            key = os.path.normcase(str(canonical))
            if key in active:
                raise AuditInfrastructureError(f"response-file cycle: {canonical}")
            identity = (int(metadata.st_dev), int(metadata.st_ino))
            if identity != (0, 0):
                prior = seen_identities.get(identity)
                if prior is not None and os.path.normcase(str(prior)) != key:
                    raise AuditInfrastructureError(
                        f"response-file alias: {canonical} aliases {prior}"
                    )
                seen_identities[identity] = canonical
            if key not in seen_paths:
                seen_paths.add(key)
                if len(seen_paths) > limits.response_files:
                    raise AuditInfrastructureError("response-file count limit exceeded")
            remaining = limits.response_bytes - aggregate_bytes
            if metadata.st_size > remaining:
                raise AuditInfrastructureError("response-file bytes limit exceeded")
            try:
                with canonical.open("rb") as stream:
                    data = stream.read(remaining + 1)
            except OSError as error:
                raise AuditInfrastructureError(f"response file is unreadable: {canonical}") from error
            if len(data) > remaining:
                raise AuditInfrastructureError("response-file bytes limit exceeded")
            if len(data) != metadata.st_size:
                raise AuditInfrastructureError(
                    f"response file changed while expanding: {canonical}"
                )
            aggregate_bytes += len(data)
            nested = _response_arguments(_response_text(data, family, canonical), family, canonical)
            active.add(key)
            try:
                result.extend(expand(nested, next_depth))
            finally:
                active.remove(key)
        return result

    return tuple(expand(arguments, 0))


def _probe_compiler_version(
    capability: CompilerExecutableCapability,
    family: CompilerFamily,
    cwd: Path,
    environment: Mapping[str, str],
    pipeline_deadline: float | None = None,
    launch_observer=None,
) -> bytes:
    compiler = capability.executable_identity.canonical
    probe_directory: tempfile.TemporaryDirectory[str] | None = None
    if family is CompilerFamily.MSVC:
        probe_directory = tempfile.TemporaryDirectory(prefix="gpu-capability-cl-probe-")
        probe_source = Path(probe_directory.name) / "probe.cpp"
        probe_source.write_bytes(b"\n")
        arguments = ("/nologo", "/Bv", "/EP", "/TP", str(probe_source))
    else:
        arguments = ("--version",)
    try:
        return _run_probe_command(
            capability, arguments, cwd, environment, pipeline_deadline,
            launch_observer=launch_observer,
        )
    finally:
        if probe_directory is not None:
            probe_directory.cleanup()


def _held_compiler_launch(
    capability: CompilerExecutableCapability,
    prepared_arguments: tuple[str, ...],
) -> tuple[tuple[str, ...], dict[str, object]]:
    """Select the executable through the already-held native capability.

    POSIX executes the transferred descriptor path.  Windows CreateProcess
    necessarily accepts a name; the parent registry retains non-write/delete
    shared handles through generation reap, so this is the attested locked
    name and the child performs no path lookup or identity refresh.
    """

    if (
        not isinstance(capability, CompilerExecutableCapability)
        or not isinstance(prepared_arguments, tuple)
        or not prepared_arguments
        or any(not isinstance(value, str) for value in prepared_arguments)
    ):
        raise AuditInfrastructureError("held compiler launch is invalid")
    owner = capability.native_owner
    if not isinstance(owner, _CompilerCapabilityOwner):
        raise AuditInfrastructureError("held compiler launch owner is invalid")
    if capability.platform_kind == "windows":
        return (
            (str(capability.executable_identity.canonical), *prepared_arguments[1:]),
            {},
        )
    executable_fd = owner.executable_fd
    if capability.platform_kind == "linux":
        executable = f"/proc/self/fd/{executable_fd}"
    elif capability.platform_kind == "macos":
        executable = f"/dev/fd/{executable_fd}"
    else:
        raise AuditInfrastructureError("held compiler launch platform is invalid")
    return (
        (executable, *prepared_arguments[1:]),
        {"pass_fds": (executable_fd,)},
    )


def _run_probe_command(
    capability: CompilerExecutableCapability,
    arguments: tuple[str, ...],
    cwd: Path,
    environment: Mapping[str, str],
    pipeline_deadline: float | None = None,
    *,
    launch_observer=None,
) -> bytes:
    if not isinstance(capability, CompilerExecutableCapability):
        raise AuditInfrastructureError("compiler probe capability is invalid")
    compiler = capability.executable_identity.canonical
    owner = capability.native_owner
    validate = getattr(owner, "validate", None)
    if not callable(validate):
        raise AuditInfrastructureError("compiler probe capability owner is invalid")
    validate(content=False, deadline=pipeline_deadline)
    command, launch_options = _held_compiler_launch(
        capability, (str(compiler), *arguments)
    )
    containment = _ProbeContainment()
    with tempfile.TemporaryFile(mode="w+b") as stdout_stream, tempfile.TemporaryFile(
        mode="w+b"
    ) as stderr_stream:
        process: subprocess.Popen[bytes] | None = None
        carrier: CompilerProcessHandleCarrier | None = None
        preparation = None
        try:
            try:
                prepare = getattr(
                    launch_observer, "prepare_compiler_inspection_launch", None
                )
                if callable(prepare):
                    preparation = prepare(capability, pipeline_deadline)
                launch_command = containment.prepare_command(command)
                process, carrier = launch_compiler_process(
                    launch_command,
                    cwd=cwd,
                    environment=environment,
                    containment=containment,
                    platform_kind=capability.platform_kind,
                    stdin=(
                        subprocess.PIPE
                        if containment.requires_handshake
                        else subprocess.DEVNULL
                    ),
                    stdout=stdout_stream,
                    stderr=stderr_stream,
                    launch_options=launch_options,
                    purpose=CompilerLaunchPurpose.INSPECTION,
                    launch_observer=launch_observer,
                )
                finish_preparation = getattr(
                    launch_observer,
                    "finish_compiler_inspection_launch_preparation",
                    None,
                )
                if callable(finish_preparation):
                    finish_preparation(preparation)
                preparation = None
            except OSError as error:
                raise AuditInfrastructureError(
                    f"compiler version probe failed: {compiler}"
                ) from error
            deadline = min(
                time.monotonic() + _VERSION_SECONDS,
                pipeline_deadline if pipeline_deadline is not None else float("inf"),
            )
            failure: str | None = None
            while True:
                observed = os.fstat(stdout_stream.fileno()).st_size + os.fstat(
                    stderr_stream.fileno()
                ).st_size
                if observed > _VERSION_BYTES:
                    failure = "compiler version output limit exceeded"
                    break
                if process.poll() is not None:
                    break
                if time.monotonic() >= deadline:
                    failure = "compiler version probe timeout"
                    break
                time.sleep(0.01)
            if failure is not None:
                raise AuditInfrastructureError(failure)
            returncode = process.wait(timeout=1.0)
            if carrier is None:
                raise AuditInfrastructureError(
                    "compiler probe process carrier is unavailable"
                )
            carrier.complete_after_exit()
        finally:
            active_error = sys.exc_info()[1]
            cleanup_errors: list[tuple[str, BaseException]] = []

            def cleanup(label: str, action) -> None:
                try:
                    action()
                except BaseException as error:
                    cleanup_errors.append((label, error))

            if preparation is not None:
                cancel_preparation = getattr(
                    launch_observer,
                    "cancel_compiler_inspection_launch_preparation",
                    None,
                )
                if callable(cancel_preparation):
                    cleanup(
                        "inspection launch preparation cancellation",
                        lambda: cancel_preparation(preparation),
                    )
            # Closing the Windows Job or killing the POSIX process group removes
            # descendants even when preparation fails immediately after launch.
            cleanup("probe containment termination", containment.terminate)
            if process is not None and (
                carrier is None or not carrier.completed
            ):
                running = True
                try:
                    running = process.poll() is None
                except BaseException as error:
                    cleanup_errors.append(("probe process poll", error))
                if running:
                    cleanup("probe process kill", process.kill)
                    cleanup(
                        "probe process wait",
                        lambda: process.wait(timeout=1.0),
                    )
                if carrier is not None and not carrier.completed:
                    cleanup(
                        "process carrier completion",
                        carrier.complete_after_exit,
                    )
            cleanup("probe containment close", containment.close)
            if active_error is not None:
                for label, error in cleanup_errors:
                    active_error.add_note(f"{label} also failed: {error}")
            elif cleanup_errors:
                primary_label, primary_cleanup = cleanup_errors[0]
                for label, error in cleanup_errors[1:]:
                    primary_cleanup.add_note(f"{label} also failed: {error}")
                primary_cleanup.add_note(f"cleanup operation: {primary_label}")
                raise primary_cleanup
        observed = os.fstat(stdout_stream.fileno()).st_size + os.fstat(
            stderr_stream.fileno()
        ).st_size
        if observed > _VERSION_BYTES:
            raise AuditInfrastructureError("compiler version output limit exceeded")
        if returncode != 0:
            raise AuditInfrastructureError(
                f"compiler version probe failed with exit={returncode}: {compiler}"
            )
        stdout_stream.seek(0)
        stderr_stream.seek(0)
        stdout = stdout_stream.read(_VERSION_BYTES + 1)
        stderr = stderr_stream.read(_VERSION_BYTES + 1)
    if stdout and stderr:
        combined = stdout.rstrip(b"\r\n") + b"\n" + stderr
    else:
        combined = stdout or stderr
    if len(combined) > _VERSION_BYTES:
        raise AuditInfrastructureError("compiler version output limit exceeded")
    validate(content=False)
    return combined


def _resume_suspended_windows_process(process: subprocess.Popen[bytes]) -> None:
    """Release an already-contained Windows process without a helper child."""

    if os.name != "nt":
        raise AuditInfrastructureError("Windows process launch gate is unavailable")
    import ctypes
    from ctypes import wintypes

    handle = getattr(process, "_handle", None)
    if handle is None:
        raise AuditInfrastructureError("Windows process launch handle is unavailable")
    ntdll = ctypes.WinDLL("ntdll")
    resume = ntdll.NtResumeProcess
    resume.argtypes = (wintypes.HANDLE,)
    resume.restype = ctypes.c_long
    status = int(resume(handle))
    if status != 0:
        raise AuditInfrastructureError(
            f"Windows process launch gate release failed: status={status:#x}"
        )


class _ProbeContainment:
    """Own the complete version-probe process tree on every supported host."""

    def __init__(self) -> None:
        self._pid: int | None = None
        self._job = _WindowsProbeJob() if os.name == "nt" else None

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

    def prepare_command(self, command: list[str]) -> list[str]:
        return command

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        pid = getattr(process, "pid", None)
        if not isinstance(pid, int):
            return  # Deterministic unit-test process double.
        self._pid = pid
        if self._job is not None:
            self._job.attach(process)

    def release(self, process: subprocess.Popen[bytes]) -> None:
        if not self.requires_handshake or self._pid is None:
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
            return
        self._pid = None


class _WindowsProbeJob:
    """Kill-on-close Job Object used only by the bounded Windows probe."""

    def __init__(self) -> None:
        if os.name != "nt":
            self._handle = None
            return
        import ctypes
        from ctypes import wintypes

        class IoCounters(ctypes.Structure):
            _fields_ = tuple(
                (name, ctypes.c_ulonglong)
                for name in (
                    "read_operations", "write_operations", "other_operations",
                    "read_bytes", "write_bytes", "other_bytes",
                )
            )

        class BasicLimits(ctypes.Structure):
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

        class ExtendedLimits(ctypes.Structure):
            _fields_ = (
                ("basic", BasicLimits),
                ("io", IoCounters),
                ("process_memory", ctypes.c_size_t),
                ("job_memory", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t),
                ("peak_job_memory", ctypes.c_size_t),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = (
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD
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
            raise AuditInfrastructureError("cannot create compiler probe job object")
        limits = ExtendedLimits()
        limits.basic.limit_flags = 0x00002000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(
            handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            kernel32.CloseHandle(handle)
            raise AuditInfrastructureError("cannot configure compiler probe job object")
        self._handle = handle
        self._kernel32 = kernel32

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        if self._handle is None:
            return
        raw_handle = getattr(process, "_handle", None)
        if raw_handle is None or not self._kernel32.AssignProcessToJobObject(
            self._handle, raw_handle
        ):
            raise AuditInfrastructureError("cannot contain compiler probe process tree")

    def terminate(self) -> None:
        if self._handle is not None:
            self._kernel32.TerminateJobObject(self._handle, 1)

    def close(self) -> None:
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


def _resolve_compiler(
    compiler: Path,
    cwd: Path,
    environment: Mapping[str, str],
) -> Path:
    _family_from_name(compiler)
    value = str(compiler)
    has_separator = "/" in value or "\\" in value
    if compiler.is_absolute() or has_separator:
        candidate = compiler if compiler.is_absolute() else cwd / compiler
        try:
            canonical = candidate.resolve(strict=True)
        except OSError as error:
            raise AuditInfrastructureError(f"compiler executable is unavailable: {compiler}") from error
    else:
        located = shutil.which(value, path=environment.get("PATH"))
        if located is None:
            raise AuditInfrastructureError(f"compiler executable is unavailable: {compiler}")
        canonical = Path(located).resolve(strict=True)
    try:
        metadata = canonical.stat()
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {canonical}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise AuditInfrastructureError(f"compiler executable is not regular: {canonical}")
    return canonical


def _hash_field(hasher: "hashlib._Hash", value: bytes) -> None:
    hasher.update(len(value).to_bytes(8, "big"))
    hasher.update(value)


def _environment_digest(environment: Mapping[str, str]) -> str:
    if not isinstance(environment, Mapping):
        raise AuditInfrastructureError("compiler environment must be a mapping")
    entries: list[tuple[str, str]] = []
    normalized_names: set[str] = set()
    for name, value in environment.items():
        if not isinstance(name, str) or not isinstance(value, str) or "\0" in name or "\0" in value:
            raise AuditInfrastructureError("compiler environment must contain valid strings")
        normalized = name.casefold() if os.name == "nt" else name
        if normalized in normalized_names:
            raise AuditInfrastructureError(f"ambiguous compiler environment key: {name}")
        normalized_names.add(normalized)
        entries.append((normalized, value))
    hasher = hashlib.sha256()
    for name, value in sorted(entries):
        _hash_field(hasher, name.encode("utf-8", errors="surrogatepass"))
        _hash_field(hasher, value.encode("utf-8", errors="surrogatepass"))
    return hasher.hexdigest()


def _compiler_metadata_snapshot(compiler: Path) -> tuple[int, int, int, int, int]:
    try:
        metadata = compiler.stat()
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {compiler}") from error
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_size),
        int(metadata.st_mtime_ns),
        int(getattr(metadata, "st_ctime_ns", 0)),
    )


def _regular_file_snapshot(path: Path) -> tuple[int, int | None, int, int, int, int]:
    metadata = path.stat()
    if not stat.S_ISREG(metadata.st_mode):
        raise AuditInfrastructureError(f"compiler capability path is not regular: {path}")
    return (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
        int(metadata.st_size),
        int(metadata.st_mtime_ns),
        int(getattr(metadata, "st_ctime_ns", 0)),
        int(metadata.st_mode),
    )


def _symlink_snapshot(
    path: Path,
) -> tuple[int, int | None, int, int, int, str]:
    try:
        metadata = path.lstat()
        target = os.readlink(path)
    except OSError as error:
        raise AuditInfrastructureError("compiler runtime symlink changed") from error
    if not (
        stat.S_ISLNK(metadata.st_mode)
        or bool(getattr(metadata, "st_file_attributes", 0) & _REPARSE_ATTRIBUTE)
    ):
        raise AuditInfrastructureError("compiler runtime symlink changed")
    return (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) else None,
        int(metadata.st_mtime_ns), int(getattr(metadata, "st_ctime_ns", 0)),
        int(metadata.st_mode), target,
    )


def _directory_snapshot(path: Path) -> tuple[int, int | None, int]:
    metadata = path.stat()
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise AuditInfrastructureError(f"compiler capability directory is invalid: {path}")
    return (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
        int(metadata.st_mode),
    )


def _check_capability_budget(
    deadline: float | None, cancel_event: object | None
) -> None:
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError("compiler capability construction cancelled")
    if deadline is not None and time.monotonic() >= deadline:
        raise AuditInfrastructureError("compiler capability deadline exceeded")


@dataclass(frozen=True, slots=True)
class _RuntimeImports:
    names: tuple[str, ...]
    rpath: tuple[str, ...] = ()
    runpath: tuple[str, ...] = ()
    format_kind: str = "unknown"
    optional_names: tuple[str, ...] = ()
    interpreters: tuple[str, ...] = ()


_LINUX_LDCONFIG_BYTES = 4 * 1024 * 1024
_LINUX_LDCONFIG_ENTRIES = 65_536


def _parse_linux_ldconfig_cache(payload: bytes) -> dict[str, tuple[Path, ...]]:
    if not isinstance(payload, bytes) or len(payload) > _LINUX_LDCONFIG_BYTES:
        raise AuditInfrastructureError("Linux loader cache output exceeds bounds")
    result: dict[str, list[Path]] = {}
    entries = 0
    for raw_line in payload.splitlines():
        line = raw_line.strip()
        if b"=>" not in line:
            continue
        left, raw_path = line.rsplit(b"=>", 1)
        name = left.split(None, 1)[0]
        try:
            decoded_name = name.decode("ascii")
            decoded_path = raw_path.strip().decode("utf-8")
        except UnicodeError as error:
            raise AuditInfrastructureError("Linux loader cache output is invalid") from error
        path = Path(decoded_path)
        if (
            not decoded_name or len(decoded_name) > 4096
            or not decoded_path.startswith("/")
            or len(decoded_path.encode("utf-8")) > 32768
        ):
            raise AuditInfrastructureError("Linux loader cache entry is invalid")
        entries += 1
        if entries > _LINUX_LDCONFIG_ENTRIES:
            raise AuditInfrastructureError("Linux loader cache entry ceiling exceeded")
        paths = result.setdefault(decoded_name, [])
        if path not in paths:
            paths.append(path)
    return {name: tuple(paths) for name, paths in result.items()}


def _read_linux_loader_cache(
    deadline: float | None, cancel_event: object | None
) -> dict[str, tuple[Path, ...]]:
    _check_capability_budget(deadline, cancel_event)
    executable = next(
        (candidate for candidate in (Path("/sbin/ldconfig"), Path("/usr/sbin/ldconfig"))
         if candidate.is_file()),
        None,
    )
    if executable is None:
        located = shutil.which("ldconfig")
        executable = Path(located) if located else None
    if executable is None:
        raise AuditInfrastructureError("Linux loader cache query is unavailable")
    with tempfile.TemporaryFile(mode="w+b") as output:
        try:
            process = subprocess.Popen(
                (str(executable), "-p"), stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.DEVNULL,
                env={"LC_ALL": "C"}, shell=False,
            )
        except OSError as error:
            raise AuditInfrastructureError("Linux loader cache query failed") from error
        try:
            while process.poll() is None:
                _check_capability_budget(deadline, cancel_event)
                if os.fstat(output.fileno()).st_size > _LINUX_LDCONFIG_BYTES:
                    raise AuditInfrastructureError("Linux loader cache output exceeds bounds")
                time.sleep(0.01)
            if process.returncode != 0:
                raise AuditInfrastructureError("Linux loader cache query failed")
            if os.fstat(output.fileno()).st_size > _LINUX_LDCONFIG_BYTES:
                raise AuditInfrastructureError("Linux loader cache output exceeds bounds")
            output.seek(0)
            return _parse_linux_ldconfig_cache(output.read(_LINUX_LDCONFIG_BYTES + 1))
        except BaseException:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=1.0)
            raise


def _linux_loader_cache_candidates(
    name: str,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[Path, ...]:
    if not sys.platform.startswith("linux"):
        return ()
    return _read_linux_loader_cache(deadline, cancel_event).get(name, ())


class _RuntimeBinaryReader:
    """Bounded random-access reader that never materializes a runtime image."""

    def __init__(
        self, path: Path, deadline: float | None, cancel_event: object | None
    ) -> None:
        self.path = path
        self.deadline = deadline
        self.cancel_event = cancel_event
        _check_capability_budget(deadline, cancel_event)
        try:
            before = path.stat()
        except OSError as error:
            raise AuditInfrastructureError("compiler runtime binary is unreadable") from error
        if not stat.S_ISREG(before.st_mode) or before.st_size > _RUNTIME_FILE_BYTES:
            raise AuditInfrastructureError("compiler runtime per-file byte ceiling exceeded")
        try:
            self.stream = path.open("rb")
            opened = os.fstat(self.stream.fileno())
        except OSError as error:
            raise AuditInfrastructureError("compiler runtime binary is unreadable") from error
        self.size = int(before.st_size)
        self.snapshot = _regular_file_snapshot(path)
        if (
            int(opened.st_dev), int(opened.st_ino) if int(opened.st_ino) else None,
            int(opened.st_size), int(opened.st_mtime_ns),
        ) != self.snapshot[:4]:
            self.stream.close()
            raise AuditInfrastructureError("compiler runtime binary changed while opening")

    def read(self, offset: int, size: int) -> bytes:
        if (
            not isinstance(offset, int) or not isinstance(size, int)
            or offset < 0 or size < 0 or offset + size > self.size
        ):
            raise AuditInfrastructureError("compiler runtime binary range is invalid")
        _check_capability_budget(self.deadline, self.cancel_event)
        payload = bytearray()
        self.stream.seek(offset)
        remaining = size
        while remaining:
            _check_capability_budget(self.deadline, self.cancel_event)
            chunk = self.stream.read(min(64 * 1024, remaining))
            if not chunk:
                raise AuditInfrastructureError("compiler runtime binary is truncated")
            payload.extend(chunk)
            remaining -= len(chunk)
        return bytes(payload)

    def close(self) -> None:
        try:
            opened = os.fstat(self.stream.fileno())
            current = _regular_file_snapshot(self.path)
        except OSError as error:
            self.stream.close()
            raise AuditInfrastructureError("compiler runtime binary changed while parsing") from error
        self.stream.close()
        if (
            int(opened.st_dev), int(opened.st_ino) if int(opened.st_ino) else None,
            int(opened.st_size), int(opened.st_mtime_ns),
        ) != self.snapshot[:4] or current != self.snapshot:
            raise AuditInfrastructureError("compiler runtime binary changed while parsing")

    def __enter__(self) -> "_RuntimeBinaryReader":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()


class _RuntimeBinaryView:
    def __init__(self, source, offset: int, size: int) -> None:
        self.source = source
        self.offset = offset
        self.size = size

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise AuditInfrastructureError("compiler runtime binary range is invalid")
        return _binary_read(self.source, self.offset + offset, size)


def _binary_length(data) -> int:
    return data.size if isinstance(data, (_RuntimeBinaryReader, _RuntimeBinaryView)) else len(data)


def _binary_read(data, offset: int, size: int) -> bytes:
    if isinstance(data, (_RuntimeBinaryReader, _RuntimeBinaryView)):
        return data.read(offset, size)
    if offset < 0 or size < 0 or offset + size > len(data):
        raise AuditInfrastructureError("compiler runtime binary range is invalid")
    return bytes(data[offset:offset + size])


def _binary_unpack(data, format_text: str, offset: int):
    size = struct.calcsize(format_text)
    return struct.unpack(format_text, _binary_read(data, offset, size))


def _binary_view(data, offset: int, size: int):
    if isinstance(data, bytes):
        return _binary_read(data, offset, size)
    return _RuntimeBinaryView(data, offset, size)


def _bounded_c_string(data, offset: int) -> str:
    if not isinstance(offset, int) or offset < 0 or offset >= _binary_length(data):
        raise AuditInfrastructureError("compiler runtime binary string is invalid")
    payload = bytearray()
    cursor = offset
    while cursor < min(_binary_length(data), offset + 32768):
        chunk = _binary_read(
            data, cursor, min(4096, _binary_length(data) - cursor, offset + 32768 - cursor)
        )
        nul = chunk.find(b"\0")
        if nul >= 0:
            payload.extend(chunk[:nul])
            break
        payload.extend(chunk)
        cursor += len(chunk)
    else:
        raise AuditInfrastructureError("compiler runtime binary string is unterminated")
    try:
        value = bytes(payload).decode("utf-8")
    except UnicodeError as error:
        raise AuditInfrastructureError("compiler runtime binary string is invalid") from error
    if not value:
        raise AuditInfrastructureError("compiler runtime binary import is empty")
    return value


def _pe_runtime_import_details(
    data,
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    length = _binary_length(data)
    if length < 64 or _binary_read(data, 0, 2) != b"MZ":
        raise AuditInfrastructureError("compiler runtime PE image is invalid")
    pe_offset = _binary_unpack(data, "<I", 0x3C)[0]
    if pe_offset + 24 > length or _binary_read(data, pe_offset, 4) != b"PE\0\0":
        raise AuditInfrastructureError("compiler runtime PE image is invalid")
    section_count = _binary_unpack(data, "<H", pe_offset + 6)[0]
    optional_size = _binary_unpack(data, "<H", pe_offset + 20)[0]
    optional = pe_offset + 24
    if optional + optional_size > length or section_count > 4096:
        raise AuditInfrastructureError("compiler runtime PE headers exceed bounds")
    magic = _binary_unpack(data, "<H", optional)[0]
    if magic == 0x10B:
        image_base = _binary_unpack(data, "<I", optional + 28)[0]
        directory_count_offset = optional + 92
        directory_offset = optional + 96
    elif magic == 0x20B:
        image_base = _binary_unpack(data, "<Q", optional + 24)[0]
        directory_count_offset = optional + 108
        directory_offset = optional + 112
    else:
        raise AuditInfrastructureError("compiler runtime PE optional header is invalid")
    if directory_offset > optional + optional_size:
        raise AuditInfrastructureError("compiler runtime PE optional header is invalid")
    directory_count = _binary_unpack(data, "<I", directory_count_offset)[0]
    available_directories = (optional + optional_size - directory_offset) // 8
    if directory_count > 4096:
        raise AuditInfrastructureError("compiler runtime PE directory ceiling exceeded")
    if directory_count > available_directories:
        raise AuditInfrastructureError("compiler runtime PE directory table is truncated")

    def directory(index: int) -> tuple[int, int]:
        if index >= directory_count:
            return 0, 0
        return _binary_unpack(data, "<II", directory_offset + index * 8)

    import_rva, import_size = directory(1)
    delay_rva, delay_size = directory(13)
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        if offset + 40 > length:
            raise AuditInfrastructureError("compiler runtime PE section table is invalid")
        virtual_size, virtual_address, raw_size, raw_offset = _binary_unpack(
            data, "<IIII", offset + 8
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_offset(rva: int) -> int:
        for address, span, raw_offset, raw_size in sections:
            if address <= rva < address + span:
                relative = rva - address
                if relative >= raw_size or raw_offset + relative >= length:
                    break
                return raw_offset + relative
        if rva < length:
            return rva
        raise AuditInfrastructureError("compiler runtime PE import RVA is invalid")

    metadata_bytes = 0
    names: list[str] = []
    optional_names: list[str] = []

    def parse_descriptors(
        rva: int,
        size: int,
        descriptor_format: str,
        name_index: int,
        label: str,
        *,
        delay: bool = False,
    ) -> None:
        nonlocal metadata_bytes
        if rva == 0 and size == 0:
            return
        if rva == 0 or size == 0:
            raise AuditInfrastructureError(
                f"compiler runtime PE {label} directory is invalid"
            )
        if size > _RUNTIME_CONTEXT_METADATA_BYTES - metadata_bytes:
            raise AuditInfrastructureError(
                f"compiler runtime PE {label} metadata ceiling exceeded"
            )
        metadata_bytes += size
        descriptor_size = struct.calcsize(descriptor_format)
        cursor = rva_offset(rva)
        directory_end = cursor + size
        if size < descriptor_size or directory_end > length:
            raise AuditInfrastructureError(
                f"compiler runtime PE {label}s are truncated"
            )
        descriptor_count = size // descriptor_size
        if descriptor_count > 4096:
            raise AuditInfrastructureError(
                f"compiler runtime PE {label} ceiling exceeded"
            )
        for _index in range(descriptor_count):
            descriptor = _binary_unpack(data, descriptor_format, cursor)
            if not any(descriptor):
                return
            name_rva = descriptor[name_index]
            if delay:
                attributes = descriptor[0]
                if attributes not in (0, 1):
                    raise AuditInfrastructureError(
                        "compiler runtime PE delay import attributes are invalid"
                    )
                if attributes == 0:
                    if name_rva < image_base:
                        raise AuditInfrastructureError(
                            "compiler runtime PE delay import name VA is invalid"
                        )
                    name_rva -= image_base
            name = _bounded_c_string(data, rva_offset(name_rva))
            encoded_size = len(name.encode("utf-8")) + 1
            if encoded_size > _RUNTIME_CONTEXT_METADATA_BYTES - metadata_bytes:
                raise AuditInfrastructureError(
                    f"compiler runtime PE {label} metadata ceiling exceeded"
                )
            metadata_bytes += encoded_size
            names.append(name)
            if delay:
                optional_names.append(name)
            cursor += descriptor_size
        raise AuditInfrastructureError(
            f"compiler runtime PE {label} directory is unterminated"
        )

    parse_descriptors(import_rva, import_size, "<IIIII", 3, "import")
    parse_descriptors(
        delay_rva, delay_size, "<IIIIIIII", 1, "delay import", delay=True
    )
    return (
        tuple(dict.fromkeys(names)),
        tuple(dict.fromkeys(optional_names)),
    )


def _pe_runtime_import_names(data) -> tuple[str, ...]:
    return _pe_runtime_import_details(data)[0]


def _pe_runtime_imports(data) -> _RuntimeImports:
    names, optional_names = _pe_runtime_import_details(data)
    return _RuntimeImports(
        names, format_kind="pe", optional_names=optional_names
    )


def _elf_runtime_imports(data) -> _RuntimeImports:
    length = _binary_length(data)
    if length < 64 or _binary_read(data, 0, 4) != b"\x7fELF":
        raise AuditInfrastructureError("compiler runtime ELF image is invalid")
    elf_class, encoding = _binary_read(data, 4, 2)
    if elf_class not in (1, 2) or encoding not in (1, 2):
        raise AuditInfrastructureError("compiler runtime ELF format is unsupported")
    order = "<" if encoding == 1 else ">"
    if elf_class == 2:
        phoff = _binary_unpack(data, order + "Q", 32)[0]
        phentsize, phnum = _binary_unpack(data, order + "HH", 54)
        ph_format = order + "IIQQQQQQ"
        dynamic_format = order + "qQ"
    else:
        phoff = _binary_unpack(data, order + "I", 28)[0]
        phentsize, phnum = _binary_unpack(data, order + "HH", 42)
        ph_format = order + "IIIIIIII"
        dynamic_format = order + "iI"
    if phnum > 4096 or phentsize < struct.calcsize(ph_format):
        raise AuditInfrastructureError("compiler runtime ELF program headers exceed bounds")
    loads = []
    dynamic = None
    interpreter_names: list[str] = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + struct.calcsize(ph_format) > length:
            raise AuditInfrastructureError("compiler runtime ELF program headers are truncated")
        values = _binary_unpack(data, ph_format, offset)
        if elf_class == 2:
            kind, file_offset, virtual, file_size = values[0], values[2], values[3], values[5]
        else:
            kind, file_offset, virtual, file_size = values[0], values[1], values[2], values[4]
        if kind == 1:
            loads.append((virtual, file_size, file_offset))
        elif kind == 2:
            dynamic = (file_offset, file_size)
        elif kind == 3:
            if interpreter_names:
                raise AuditInfrastructureError(
                    "compiler runtime ELF has multiple PT_INTERP records"
                )
            if (
                file_size < 2
                or file_size > 32768
                or file_size > _RUNTIME_CONTEXT_METADATA_BYTES
                or file_offset + file_size > length
            ):
                raise AuditInfrastructureError(
                    "compiler runtime ELF PT_INTERP metadata exceeds bounds"
                )
            payload = _binary_read(data, file_offset, file_size)
            terminator = payload.find(b"\0")
            if terminator < 1:
                raise AuditInfrastructureError(
                    "compiler runtime ELF PT_INTERP is unterminated or empty"
                )
            if any(payload[terminator + 1:]):
                raise AuditInfrastructureError(
                    "compiler runtime ELF PT_INTERP contains trailing data"
                )
            try:
                interpreter = payload[:terminator].decode("utf-8")
            except UnicodeError as error:
                raise AuditInfrastructureError(
                    "compiler runtime ELF PT_INTERP is invalid"
                ) from error
            interpreter_names.append(interpreter)
    if dynamic is None:
        return _RuntimeImports(
            (), format_kind="elf", interpreters=tuple(interpreter_names)
        )
    entry_size = struct.calcsize(dynamic_format)
    needed = []
    rpath = []
    runpath = []
    string_virtual = None
    for cursor in range(dynamic[0], dynamic[0] + dynamic[1], entry_size):
        if cursor + entry_size > length:
            raise AuditInfrastructureError("compiler runtime ELF dynamic table is truncated")
        tag, value = _binary_unpack(data, dynamic_format, cursor)
        if tag == 0:
            break
        if tag == 1:
            needed.append(value)
        elif tag == 15:
            rpath.append(value)
        elif tag == 29:
            runpath.append(value)
        elif tag == 5:
            string_virtual = value
        if len(needed) > 4096:
            raise AuditInfrastructureError("compiler runtime ELF import ceiling exceeded")
    if string_virtual is None:
        if needed or rpath or runpath:
            raise AuditInfrastructureError(
                "compiler runtime ELF dynamic string table is absent"
            )
        return _RuntimeImports(
            (), format_kind="elf", interpreters=tuple(interpreter_names)
        )
    string_offset = None
    for virtual, file_size, file_offset in loads:
        if virtual <= string_virtual < virtual + file_size:
            string_offset = file_offset + string_virtual - virtual
            break
    if string_offset is None:
        raise AuditInfrastructureError("compiler runtime ELF string table is invalid")
    decode = lambda offsets: tuple(dict.fromkeys(
        _bounded_c_string(data, string_offset + item) for item in offsets
    ))
    return _RuntimeImports(
        decode(needed),
        decode(rpath),
        decode(runpath),
        "elf",
        interpreters=tuple(interpreter_names),
    )


def _elf_runtime_import_names(data) -> tuple[str, ...]:
    return _elf_runtime_imports(data).names


def _macho_runtime_imports(data) -> _RuntimeImports:
    length = _binary_length(data)
    if length < 28:
        raise AuditInfrastructureError("compiler runtime Mach-O image is invalid")
    magic = _binary_read(data, 0, 4)
    formats = {
        b"\xfe\xed\xfa\xce": (">", False), b"\xce\xfa\xed\xfe": ("<", False),
        b"\xfe\xed\xfa\xcf": (">", True), b"\xcf\xfa\xed\xfe": ("<", True),
    }
    fat_formats = {
        b"\xca\xfe\xba\xbe": (">", False), b"\xbe\xba\xfe\xca": ("<", False),
        b"\xca\xfe\xba\xbf": (">", True), b"\xbf\xba\xfe\xca": ("<", True),
    }
    if magic in fat_formats:
        order, fat_64 = fat_formats[magic]
        count = _binary_unpack(data, order + "I", 4)[0]
        if count > 64:
            raise AuditInfrastructureError("compiler runtime Mach-O fat image exceeds bounds")
        machine = platform.machine().casefold()
        host_cpu = {
            "x86_64": 0x01000007, "amd64": 0x01000007,
            "arm64": 0x0100000C, "aarch64": 0x0100000C,
            "i386": 7, "i686": 7,
        }.get(machine)
        if host_cpu is None:
            raise AuditInfrastructureError("compiler runtime Mach-O host architecture is unsupported")
        selected = None
        entry_size = 32 if fat_64 else 20
        for index in range(count):
            entry = 8 + index * entry_size
            if entry + entry_size > length:
                raise AuditInfrastructureError("compiler runtime Mach-O fat image is truncated")
            cpu = _binary_unpack(data, order + "I", entry)[0]
            offset, size = _binary_unpack(
                data, order + ("QQ" if fat_64 else "II"), entry + 8
            )
            if offset + size > length:
                raise AuditInfrastructureError("compiler runtime Mach-O slice is invalid")
            if cpu == host_cpu and selected is None:
                selected = (offset, size)
        if selected is None:
            raise AuditInfrastructureError("compiler runtime Mach-O host slice is absent")
        return _macho_runtime_imports(_binary_view(data, *selected))
    if magic not in formats:
        raise AuditInfrastructureError("compiler runtime binary format is unsupported")
    order, is_64 = formats[magic]
    command_count, command_bytes = _binary_unpack(data, order + "II", 16)
    cursor = 32 if is_64 else 28
    if command_count > 4096 or cursor + command_bytes > length:
        raise AuditInfrastructureError("compiler runtime Mach-O commands exceed bounds")
    dylib_commands = {0xC, 0x18, 0x1F, 0x20, 0x23}
    result = []
    rpaths = []
    for _index in range(command_count):
        if cursor + 8 > length:
            raise AuditInfrastructureError("compiler runtime Mach-O command is truncated")
        command, size = _binary_unpack(data, order + "II", cursor)
        command &= 0x7FFFFFFF
        if size < 8 or cursor + size > length:
            raise AuditInfrastructureError("compiler runtime Mach-O command is invalid")
        if command in dylib_commands:
            if size < 24:
                raise AuditInfrastructureError("compiler runtime Mach-O dylib command is invalid")
            name_offset = _binary_unpack(data, order + "I", cursor + 8)[0]
            result.append(_bounded_c_string(data, cursor + name_offset))
        elif command == 0x1C:
            if size < 12:
                raise AuditInfrastructureError("compiler runtime Mach-O rpath command is invalid")
            path_offset = _binary_unpack(data, order + "I", cursor + 8)[0]
            rpaths.append(_bounded_c_string(data, cursor + path_offset))
        cursor += size
    return _RuntimeImports(
        tuple(dict.fromkeys(result)), tuple(dict.fromkeys(rpaths)),
        format_kind="macho",
    )


def _macho_runtime_import_names(data) -> tuple[str, ...]:
    return _macho_runtime_imports(data).names


def _binary_runtime_imports(
    path: Path,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> _RuntimeImports:
    with _RuntimeBinaryReader(path, deadline, cancel_event) as data:
        magic = data.read(0, min(4, data.size))
        if magic.startswith(b"MZ"):
            return _pe_runtime_imports(data)
        if magic == b"\x7fELF":
            return _elf_runtime_imports(data)
        if magic in {
            b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
            b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
            b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
            b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
        }:
            return _macho_runtime_imports(data)
        return _RuntimeImports(())


def _binary_runtime_import_names(
    path: Path,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[str, ...]:
    return _binary_runtime_imports(
        path, deadline=deadline, cancel_event=cancel_event
    ).names


def _toolchain_file_index(
    root: Path, deadline: float, cancel_event: object | None
) -> dict[str, tuple[Path, ...]]:
    pending = [root]
    directories = 0
    entries = 0
    result: dict[str, list[Path]] = {}
    while pending:
        _check_capability_budget(deadline, cancel_event)
        directory = pending.pop()
        directories += 1
        if directories > _RUNTIME_TREE_DIRECTORIES:
            raise AuditInfrastructureError("compiler runtime directory enumeration ceiling exceeded")
        try:
            with os.scandir(directory) as iterator:
                for entry in iterator:
                    _check_capability_budget(deadline, cancel_event)
                    entries += 1
                    if entries > _RUNTIME_TREE_ENTRIES:
                        raise AuditInfrastructureError(
                            "compiler runtime tree enumeration ceiling exceeded"
                        )
                    try:
                        candidate = Path(entry.path).absolute()
                        if entry.is_symlink():
                            result.setdefault(entry.name.casefold(), []).append(candidate)
                        elif entry.is_dir(follow_symlinks=False):
                            pending.append(Path(entry.path))
                        elif entry.is_file(follow_symlinks=False):
                            result.setdefault(entry.name.casefold(), []).append(
                                candidate
                            )
                    except OSError as error:
                        raise AuditInfrastructureError(
                            "compiler runtime tree changed during enumeration"
                        ) from error
        except OSError as error:
            raise AuditInfrastructureError("compiler runtime tree is unreadable") from error
    return {
        name: tuple(sorted(paths, key=lambda item: os.path.normcase(str(item))))
        for name, paths in result.items()
    }


def _runtime_binding_for_path(
    path: Path, authority: DependencyRootAuthority
) -> DependencyRootBinding:
    matches = []
    for binding in authority.external_roots:
        try:
            path.relative_to(binding.resolved_root)
        except ValueError:
            continue
        matches.append(binding)
    if len(matches) != 1:
        raise AuditInfrastructureError(
            f"compiler runtime path is not mapped by exactly one authority root: {path}"
        )
    return matches[0]


def _resolve_runtime_candidate(
    candidate: Path, authority: DependencyRootAuthority
) -> tuple[Path, tuple[Path, ...]] | None:
    aliases: list[Path] = []
    current = Path(os.path.abspath(candidate))
    component_count = 0
    metadata_bytes = 0
    for _depth in range(32):
        sentinel = Path(current.anchor)
        relative = current.relative_to(sentinel)
        cursor = sentinel
        redirected = False
        for index, part in enumerate(relative.parts):
            component_count += 1
            cursor = cursor / part
            metadata_bytes += len(os.fsencode(cursor)) + 64
            if (
                component_count > _RUNTIME_PATH_COMPONENTS
                or metadata_bytes > _RUNTIME_PATH_METADATA_BYTES
            ):
                raise AuditInfrastructureError(
                    "compiler runtime path component metadata ceiling exceeded"
                )
            try:
                metadata = cursor.lstat()
            except OSError:
                return None
            is_alias = stat.S_ISLNK(metadata.st_mode) or bool(
                getattr(metadata, "st_file_attributes", 0) & _REPARSE_ATTRIBUTE
            )
            if is_alias:
                aliases.append(cursor)
                try:
                    target = Path(os.readlink(cursor))
                except OSError as error:
                    raise AuditInfrastructureError(
                        "compiler runtime symlink is unreadable"
                    ) from error
                redirected_root = (
                    target if target.is_absolute() else cursor.parent / target
                )
                current = Path(os.path.abspath(
                    redirected_root.joinpath(*relative.parts[index + 1:])
                ))
                redirected = True
                break
            if index + 1 < len(relative.parts):
                if not stat.S_ISDIR(metadata.st_mode):
                    return None
            elif not stat.S_ISREG(metadata.st_mode):
                return None
        if redirected:
            continue
        _runtime_binding_for_path(current, authority)
        return current, tuple(dict.fromkeys(aliases))
    raise AuditInfrastructureError("compiler runtime symlink depth exceeded")


def _expand_loader_path(value: str, importer: Path, executable: Path) -> Path:
    expanded = (
        value.replace("$ORIGIN", str(importer.parent))
        .replace("${ORIGIN}", str(importer.parent))
        .replace("@loader_path", str(importer.parent))
        .replace("@executable_path", str(executable.parent))
    )
    return Path(expanded)


def _loader_default_directories(platform_kind: str) -> tuple[Path, ...]:
    if platform_kind == "windows":
        windows = Path(os.environ.get("SystemRoot", "C:/Windows"))
        return (windows / "System32", windows / "System", windows)
    if platform_kind == "macos":
        return (Path("/usr/lib"), Path("/System/Library/Frameworks"))
    multiarch = sysconfig.get_config_var("MULTIARCH")
    result = [Path("/lib"), Path("/usr/lib"), Path("/lib64"), Path("/usr/lib64")]
    if isinstance(multiarch, str) and multiarch:
        result.extend((Path("/lib") / multiarch, Path("/usr/lib") / multiarch))
    return tuple(result)


def _windows_known_dlls() -> frozenset[str]:
    if os.name != "nt":
        return frozenset()
    try:
        import winreg

        result = set()
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs",
        ) as key:
            for index in range(4096):
                try:
                    _label, value, _kind = winreg.EnumValue(key, index)
                except OSError:
                    break
                if isinstance(value, str) and value and "\0" not in value:
                    result.add(Path(value).name.casefold())
        return frozenset(result)
    except (ImportError, OSError) as error:
        raise AuditInfrastructureError("Windows KnownDLL policy is unavailable") from error


def _resolve_runtime_name(
    name: str,
    importer: Path,
    executable: Path,
    platform_kind: str,
    imports: _RuntimeImports,
    inherited_rpath: tuple[Path, ...],
    authority: DependencyRootAuthority,
    environment: Mapping[str, str],
    working_directory: Path,
    *,
    loaded_modules: Mapping[str, Path] | None = None,
    known_dlls: frozenset[str] | None = None,
    deadline: float | None = None,
    cancel_event: object | None = None,
    linux_loader_cache: Mapping[str, tuple[Path, ...]] | None = None,
) -> tuple[Path, tuple[Path, ...], tuple[Path, ...]]:
    candidates = []
    child_inherited = inherited_rpath
    raw = Path(name)
    loader_kind = {
        "elf": "linux", "macho": "macos", "pe": "windows"
    }.get(imports.format_kind, platform_kind)
    if loader_kind == "macos":
        rpaths = tuple(
            _expand_loader_path(value, importer, executable) for value in imports.rpath
        )
        library_paths = tuple(
            Path(value) for value in environment.get("DYLD_LIBRARY_PATH", "").split(os.pathsep)
            if value
        )
        fallback_value = environment.get("DYLD_FALLBACK_LIBRARY_PATH")
        fallback_paths = tuple(
            Path(value) for value in fallback_value.split(os.pathsep) if value
        ) if fallback_value is not None else (
            Path.home() / "lib", Path("/usr/local/lib"), Path("/usr/lib")
        )
        if name.startswith("@rpath/"):
            suffix = name[len("@rpath/"):]
            candidates.extend(path / suffix for path in (*rpaths, *inherited_rpath))
        elif name.startswith(("@loader_path/", "@executable_path/")) or raw.is_absolute():
            candidates.append(_expand_loader_path(name, importer, executable))
        else:
            leaf = raw.name
            candidates.extend(path / leaf for path in library_paths)
            candidates.extend(path / leaf for path in fallback_paths)
        child_inherited = tuple(dict.fromkeys((*rpaths, *inherited_rpath)))
    elif loader_kind == "linux":
        local_rpath = tuple(
            _expand_loader_path(value, importer, executable)
            for entry in imports.rpath
            for value in entry.split(":") if value
        )
        local_runpath = tuple(
            _expand_loader_path(value, importer, executable)
            for entry in imports.runpath
            for value in entry.split(":") if value
        )
        environment_paths = tuple(
            Path(value) for value in environment.get("LD_LIBRARY_PATH", "").split(os.pathsep)
            if value
        )
        if raw.is_absolute() or "/" in name:
            candidates.append(_expand_loader_path(name, importer, executable))
        else:
            search = (
                (*local_rpath, *inherited_rpath, *environment_paths)
                if not local_runpath else
                (*inherited_rpath, *environment_paths, *local_runpath)
            )
            candidates.extend(path / name for path in search)
            candidates.extend(
                _linux_loader_cache_candidates(name, deadline, cancel_event)
                if linux_loader_cache is None else linux_loader_cache.get(name, ())
            )
            candidates.extend(path / name for path in _loader_default_directories("linux"))
        if not local_runpath:
            child_inherited = tuple(dict.fromkeys((*inherited_rpath, *local_rpath)))
    else:
        if raw.is_absolute() or "\\" in name or "/" in name:
            candidates.append(raw)
        else:
            folded = name.casefold()
            loaded = (loaded_modules or {}).get(folded)
            if loaded is not None:
                candidates.append(loaded)
            elif folded in (known_dlls if known_dlls is not None else _windows_known_dlls()):
                candidates.append(_loader_default_directories("windows")[0] / name)
            else:
                candidates.append(executable.parent / name)
                candidates.extend(path / name for path in _loader_default_directories("windows"))
                candidates.append(working_directory / name)
                candidates.extend(
                    Path(value) / name
                    for value in environment.get("PATH", "").split(os.pathsep)
                    if value
                )
    for candidate in candidates:
        resolved = _resolve_runtime_candidate(candidate, authority)
        if resolved is not None:
            resolved_path, aliases = resolved
            return resolved_path, aliases, child_inherited
    raise AuditInfrastructureError(f"unresolved runtime import: {name}")


def _resolve_elf_interpreter(
    name: str,
    working_directory: Path,
    authority: DependencyRootAuthority,
) -> tuple[Path, tuple[Path, ...]]:
    if (
        not isinstance(name, str)
        or not name
        or "\0" in name
    ):
        raise AuditInfrastructureError("compiler runtime ELF PT_INTERP path is invalid")
    raw = Path(name)
    if raw.is_absolute():
        candidate = raw
    else:
        if any(part in ("", ".", "..") for part in raw.parts):
            raise AuditInfrastructureError(
                "compiler runtime ELF PT_INTERP path escapes working directory"
            )
        candidate = working_directory / raw
    resolved = _resolve_runtime_candidate(candidate, authority)
    if resolved is None:
        raise AuditInfrastructureError(
            f"unresolved ELF PT_INTERP executable: {name}"
        )
    return resolved


def _recursive_runtime_paths(
    seeds: tuple[Path, ...],
    executable: Path,
    authority: DependencyRootAuthority,
    platform_kind: str,
    environment: Mapping[str, str],
    working_directory: Path,
    deadline: float,
    cancel_event: object | None,
) -> tuple[tuple[Path, ...], tuple[Path, ...]]:
    pending = [
        (
            seed,
            seed,
            (),
            {seed.name.casefold(): seed.resolve(strict=True)},
        )
        for seed in seeds
    ]
    result = []
    aliases = []
    analyzed_states = set()
    process_state_counts: dict[tuple[str, int, int | None], int] = {}
    emitted = set()
    reserved = set()
    total_bytes = 0
    context_metadata_bytes = 0
    known_dlls = _windows_known_dlls() if platform_kind == "windows" else frozenset()
    linux_loader_cache = (
        _read_linux_loader_cache(deadline, cancel_event)
        if platform_kind == "linux" else None
    )

    def reserve(path: Path) -> None:
        nonlocal total_bytes
        resolved = path.resolve(strict=True)
        key = os.path.normcase(str(resolved))
        if key in reserved:
            return
        metadata = resolved.stat()
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > _RUNTIME_FILE_BYTES:
            raise AuditInfrastructureError("compiler runtime per-file byte ceiling exceeded")
        total_bytes += metadata.st_size
        if total_bytes > _RUNTIME_TOTAL_BYTES:
            raise AuditInfrastructureError("compiler runtime total byte ceiling exceeded")
        reserved.add(key)

    for seed in seeds:
        reserve(seed)
    while pending:
        _check_capability_budget(deadline, cancel_event)
        current, process_executable, inherited_rpath, loaded_modules = pending.pop(0)
        current = current.resolve(strict=True)
        process_executable = process_executable.resolve(strict=True)
        current_metadata = current.stat()
        executable_metadata = process_executable.stat()
        current_key = os.path.normcase(str(current))
        executable_key = os.path.normcase(str(process_executable))
        inherited_key = tuple(
            os.path.normcase(str(path.resolve(strict=True)))
            for path in inherited_rpath
        )
        loaded_key = (
            ()
            if platform_kind == "windows"
            else tuple(sorted(
                (
                    name,
                    os.path.normcase(str(path)),
                )
                for name, path in loaded_modules.items()
            ))
        )
        state = (
            current_key,
            int(current_metadata.st_dev),
            int(current_metadata.st_ino) or None,
            executable_key,
            int(executable_metadata.st_dev),
            int(executable_metadata.st_ino) or None,
            inherited_key,
            loaded_key,
        )
        if state in analyzed_states:
            continue
        state_bytes = 256 + sum(
            len(os.fsencode(value))
            for value in (
                current_key,
                executable_key,
                *inherited_key,
                *(part for item in loaded_key for part in item),
            )
        )
        if state_bytes > _RUNTIME_CONTEXT_METADATA_BYTES - context_metadata_bytes:
            raise AuditInfrastructureError(
                "compiler runtime context metadata ceiling exceeded"
            )
        context_metadata_bytes += state_bytes
        analyzed_states.add(state)
        process_key = (
            executable_key,
            int(executable_metadata.st_dev),
            int(executable_metadata.st_ino) or None,
        )
        process_state_counts[process_key] = process_state_counts.get(
            process_key, 0
        ) + 1
        if process_state_counts[process_key] > _RUNTIME_CONTEXT_STATES:
            raise AuditInfrastructureError(
                "compiler runtime context state ceiling exceeded"
            )
        if current_key not in emitted:
            emitted.add(current_key)
            result.append(current)
            if len(result) > _RUNTIME_CLOSURE_FILES:
                raise AuditInfrastructureError(
                    "compiler runtime closure file ceiling exceeded"
                )
        imports = _binary_runtime_imports(
            current, deadline=deadline, cancel_event=cancel_event
        )
        for interpreter in imports.interpreters:
            resolved, resolved_aliases = _resolve_elf_interpreter(
                interpreter, working_directory, authority
            )
            reserve(resolved)
            for alias in resolved_aliases:
                if alias not in aliases:
                    aliases.append(alias)
            pending.append((
                resolved,
                process_executable,
                inherited_rpath,
                loaded_modules,
            ))
        for name in imports.names:
            if imports.format_kind == "pe" and name.casefold().startswith(
                ("api-ms-", "ext-ms-")
            ):
                # API-set contract names are resolved by the Windows loader's
                # ApiSet map and do not name replaceable filesystem objects.
                continue
            module_name = Path(name).name.casefold()
            already_loaded = (
                imports.format_kind == "pe" and module_name in loaded_modules
            )
            if already_loaded:
                continue
            try:
                resolved, resolved_aliases, child_inherited = _resolve_runtime_name(
                    name, current, process_executable, platform_kind, imports,
                    inherited_rpath, authority, environment, working_directory,
                    loaded_modules=loaded_modules, known_dlls=known_dlls,
                    deadline=deadline, cancel_event=cancel_event,
                    linux_loader_cache=linux_loader_cache,
                )
            except AuditInfrastructureError as error:
                if (
                    name in imports.optional_names
                    and str(error) == f"unresolved runtime import: {name}"
                ):
                    continue
                raise
            reserve(resolved)
            loaded_modules.setdefault(module_name, resolved)
            for alias in resolved_aliases:
                if alias not in aliases:
                    aliases.append(alias)
            pending.append((
                resolved,
                process_executable,
                child_inherited,
                loaded_modules,
            ))
    return tuple(result), tuple(aliases)


def _content_sha256(
    stream,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
    byte_ceiling: int = _RUNTIME_FILE_BYTES,
) -> str:
    digest = hashlib.sha256()
    observed = 0
    stream.seek(0)
    while chunk := stream.read(1024 * 1024):
        _check_capability_budget(deadline, cancel_event)
        observed += len(chunk)
        if observed > byte_ceiling:
            raise AuditInfrastructureError("compiler runtime per-file byte ceiling exceeded")
        digest.update(chunk)
    stream.seek(0)
    return digest.hexdigest()


class _CompilerCapabilityOwner:
    """Held executable/closure files plus exact path-chain snapshots."""

    def __init__(
        self,
        streams: tuple[object, ...],
        file_paths: tuple[Path, ...],
        file_snapshots: tuple[tuple[int, int | None, int, int, int, int], ...],
        file_hashes: tuple[str, ...],
        alias_paths: tuple[Path, ...],
        alias_snapshots: tuple[tuple[int, int | None, int, int, int, str], ...],
        directory_paths: tuple[Path, ...],
        directory_snapshots: tuple[tuple[int, int | None, int], ...],
        observer: _FilesystemGenerationObserver | None,
        dependency_root_authority: DependencyRootAuthority,
        *,
        validate_paths: bool = True,
    ) -> None:
        self.streams = streams
        self.file_paths = file_paths
        self.file_snapshots = file_snapshots
        self.file_hashes = file_hashes
        self.alias_paths = alias_paths
        self.alias_snapshots = alias_snapshots
        self.directory_paths = directory_paths
        self.directory_snapshots = directory_snapshots
        self.observer = observer
        self.dependency_root_authority = dependency_root_authority
        self.validate_paths = validate_paths
        self._closed = False
        self._lock = threading.Lock()

    @property
    def executable_fd(self) -> int:
        if self._closed or not self.streams:
            raise AuditInfrastructureError("compiler executable capability is closed")
        return self.streams[0].fileno()

    def validate(
        self, *, content: bool = True, deadline: float | None = None,
        cancel_event: object | None = None,
    ) -> None:
        with self._lock:
            self._validate_locked(
                content=content, deadline=deadline, cancel_event=cancel_event
            )

    def _validate_locked(
        self, *, content: bool, deadline: float | None,
        cancel_event: object | None,
    ) -> None:
        _check_capability_budget(deadline, cancel_event)
        if self._closed:
            raise AuditInfrastructureError("compiler executable capability is closed")
        if self.observer is not None:
            self.observer.drain()
        for stream, path, expected in zip(
            self.streams, self.file_paths, self.file_snapshots
        ):
            opened = os.fstat(stream.fileno())
            opened_snapshot = (
                int(opened.st_dev),
                int(opened.st_ino) if int(opened.st_ino) != 0 else None,
                int(opened.st_size),
                int(opened.st_mtime_ns),
                int(getattr(opened, "st_ctime_ns", 0)),
                int(opened.st_mode),
            )
            if (
                opened_snapshot[:4] != expected[:4]
                or (
                    self.validate_paths
                    and _regular_file_snapshot(path) != expected
                )
            ):
                raise AuditInfrastructureError(
                    "compiler executable changed during compiler version probe: "
                    "capability identity changed"
                )
            if content and _content_sha256(
                stream, deadline=deadline, cancel_event=cancel_event
            ) != self.file_hashes[
                self.streams.index(stream)
            ]:
                raise AuditInfrastructureError(
                    "compiler executable capability content changed"
                )
        if self.validate_paths:
            for path, expected in zip(
                self.directory_paths, self.directory_snapshots
            ):
                _check_capability_budget(deadline, cancel_event)
                if _directory_snapshot(path) != expected:
                    raise AuditInfrastructureError(
                        "compiler executable path chain changed"
                    )
            for path, expected in zip(
                self.alias_paths, self.alias_snapshots
            ):
                _check_capability_budget(deadline, cancel_event)
                if _symlink_snapshot(path) != expected:
                    raise AuditInfrastructureError(
                        "compiler runtime symlink changed"
                    )
        if self.observer is not None:
            self.observer.drain()

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            errors: list[BaseException] = []
            if self.observer is not None:
                try:
                    self.observer.close()
                except BaseException as error:
                    errors.append(error)
            for stream in reversed(self.streams):
                try:
                    stream.close()
                except BaseException as error:
                    errors.append(error)
            if errors:
                raise AuditInfrastructureError("compiler capability owner cleanup failed") from errors[0]

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _toolchain_binding_for_path(
    path: Path, dependency_roots: DependencyRootAuthority
) -> DependencyRootBinding:
    matches: list[DependencyRootBinding] = []
    for binding in dependency_roots.external_roots:
        try:
            path.relative_to(binding.resolved_root)
        except ValueError:
            continue
        matches.append(binding)
    if len(matches) != 1:
        raise AuditInfrastructureError("compiler is not mapped by exactly one trusted toolchain root")
    return matches[0]


def _path_chain(sentinel: Path, target: Path) -> tuple[Path, ...]:
    try:
        relative = target.relative_to(sentinel)
    except ValueError as error:
        raise AuditInfrastructureError("compiler path escapes trusted toolchain root") from error
    result = [sentinel]
    current = sentinel
    for part in relative.parts:
        current = current / part
        result.append(current)
    return tuple(result)


def _runtime_path_chains(
    authority: DependencyRootAuthority, paths: Iterable[Path]
) -> tuple[Path, ...]:
    result = []
    seen = set()
    for path in paths:
        matches = []
        for binding in authority.external_roots:
            try:
                path.relative_to(binding.resolved_root)
            except ValueError:
                continue
            matches.append(binding)
        if len(matches) > 1:
            raise AuditInfrastructureError(
                f"compiler runtime path is mapped by ambiguous authority roots: {path}"
            )
        sentinel = (
            matches[0].resolved_root.parent if matches else Path(path.anchor)
        )
        for directory in _path_chain(sentinel, path.parent):
            key = os.path.normcase(str(directory))
            if key not in seen:
                seen.add(key)
                result.append(directory)
    return tuple(result)


def _driver_selected_helper_paths(
    capability: CompilerExecutableCapability,
    family: CompilerFamily,
    working_directory: Path,
    environment: Mapping[str, str],
    root: Path,
    index: Mapping[str, tuple[Path, ...]],
    deadline: float,
    cancel_event: object | None,
    preprocess_arguments: tuple[str, ...] | None = None,
    preprocess_language: str | None = None,
) -> tuple[Path, ...]:
    owner = capability.native_owner
    if preprocess_arguments is None:
        queries = ()
        required = False
    else:
        language = preprocess_language or _preprocess_language(
            preprocess_arguments, family, working_directory
        )
        required = True
        if family is CompilerFamily.GCC:
            queries = ({
                "c": "cc1",
                "c++": "cc1plus",
                "objective-c": "cc1obj",
                "objective-c++": "cc1objplus",
            }[language],)
        else:
            queries = ()
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        if preprocess_arguments is None or family is CompilerFamily.CLANG_CL:
            return ()
        names = ("c1.dll",) if language in {"c", "objective-c"} else ("c1xx.dll",)
        result = []
        for name in names:
            sibling = _resolve_runtime_candidate(
                capability.executable_identity.canonical.parent / name,
                owner.dependency_root_authority,
            )
            candidates = (
                (sibling[0],) if sibling is not None
                else index.get(name, ())
            )
            if len(candidates) == 1:
                result.append(candidates[0])
            elif required:
                raise AuditInfrastructureError(
                    f"compiler preprocessing helper is unavailable: {name}"
                )
        return tuple(result)
    result = []
    for program in queries:
        _check_capability_budget(deadline, cancel_event)
        output = _run_probe_command(
            capability,
            (f"-print-prog-name={program}",),
            working_directory,
            environment,
            deadline,
        )
        try:
            value = output.decode("utf-8").strip()
        except UnicodeError as error:
            raise AuditInfrastructureError("compiler helper query output is invalid") from error
        if not value or "\n" in value or "\0" in value or len(value) > 32768:
            raise AuditInfrastructureError("compiler helper query output is invalid")
        supplied = Path(value)
        candidates = []
        if supplied.is_absolute():
            candidates.append(supplied)
        elif "/" in value or "\\" in value:
            candidates.extend((working_directory / supplied, root / supplied))
        else:
            candidates.extend(index.get(supplied.name.casefold(), ()))
            candidates.extend((
                capability.executable_identity.canonical.parent / supplied,
                working_directory / supplied,
            ))
        matches = []
        for candidate in candidates:
            resolved = _resolve_runtime_candidate(
                candidate, owner.dependency_root_authority
            )
            if resolved is not None and resolved[0] not in matches:
                matches.append(resolved[0])
        if len(matches) > 1:
            raise AuditInfrastructureError(
                f"compiler helper query is ambiguous: {program}"
            )
        if matches and matches[0] not in result:
            result.append(matches[0])
        elif required and not matches:
            raise AuditInfrastructureError(
                f"compiler preprocessing helper is unavailable: {program}"
            )
    return tuple(result)


def _msvc_language_selectors(
    arguments: tuple[str, ...], working_directory: Path
) -> tuple[str | None, dict[str, str], tuple[str, ...]]:
    global_language: str | None = None
    per_source: dict[str, str] = {}
    bindings: list[str] = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        spelling = _msvc_slash_spelling(value)
        if spelling in {"/TC", "/TP"}:
            global_language = "c" if spelling == "/TC" else "c++"
            bindings.append(value)
            index += 1
            continue
        language: str | None = None
        candidate: str | None = None
        if spelling in {"/Tc", "/Tp"}:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(
                    f"compiler language selector requires a source: {value}"
                )
            language = "c" if spelling == "/Tc" else "c++"
            candidate = arguments[index + 1]
            bindings.append(value)
            index += 2
        elif spelling.startswith(("/Tc", "/Tp")) and len(value) > 3:
            language = "c" if spelling.startswith("/Tc") else "c++"
            candidate = value[3:]
            bindings.append(value)
            index += 1
        else:
            index += 1
            continue
        if candidate == "-":
            raise AuditInfrastructureError("stdin cannot be a compile source")
        canonical = _canonical_argument_path(candidate, working_directory)
        source_key = os.path.normcase(str(canonical))
        previous = per_source.get(source_key)
        if previous is not None and previous != language:
            raise AuditInfrastructureError(
                "conflicting per-source compiler language selectors"
            )
        per_source[source_key] = language
        bindings.append(source_key)
    return global_language, per_source, tuple(bindings)


def _preprocess_language(
    arguments: tuple[str, ...],
    family: CompilerFamily,
    working_directory: Path,
    source_path: Path | None = None,
) -> str:
    explicit: str | None = None
    index = 0
    while index < len(arguments):
        value = arguments[index]
        lowered = value.casefold()
        if lowered == "-x":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError("compiler language option requires a value")
            explicit = arguments[index + 1].casefold()
            index += 2
            continue
        if lowered.startswith("-x") and len(value) > 2:
            explicit = value[2:].casefold()
        index += 1
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        global_language, per_source, _bindings = _msvc_language_selectors(
            arguments, working_directory
        )
        if source_path is None and len(per_source) == 1:
            source_path = Path(next(iter(per_source)))
        selected_language = global_language
        if source_path is not None:
            source_key = os.path.normcase(str(_canonical_argument_path(
                str(source_path), working_directory
            )))
            selected_language = per_source.get(source_key, global_language)
        if selected_language is not None:
            if explicit is not None:
                aliases = {
                    "c": "c", "cpp-output": "c", "c-header": "c",
                    "c++": "c++", "c++-cpp-output": "c++", "c++-header": "c++",
                    "objective-c": "objective-c",
                    "objective-c-cpp-output": "objective-c",
                    "objective-c++": "objective-c++",
                    "objective-c++-cpp-output": "objective-c++",
                }
                if aliases.get(explicit) != selected_language:
                    raise AuditInfrastructureError(
                        "conflicting compiler language selectors"
                    )
            explicit = selected_language
    aliases = {
        "c": "c", "cpp-output": "c", "c-header": "c",
        "c++": "c++", "c++-cpp-output": "c++", "c++-header": "c++",
        "objective-c": "objective-c", "objective-c-cpp-output": "objective-c",
        "objective-c++": "objective-c++",
        "objective-c++-cpp-output": "objective-c++",
    }
    if explicit is not None:
        try:
            return aliases[explicit]
        except KeyError as error:
            raise AuditInfrastructureError(
                f"unsupported compiler preprocessing language: {explicit}"
            ) from error
    if source_path is None:
        sources = _source_inputs(arguments, working_directory, family)
        if len(sources) != 1:
            raise AuditInfrastructureError(
                "compiler preprocessing helper requires exactly one source language"
            )
        source_path = sources[0]
    suffix = source_path.suffix.casefold()
    if suffix in {".c", ".i"}:
        return "c"
    if suffix in {".m", ".mi"}:
        return "objective-c"
    if suffix in {".mm", ".mii"}:
        return "objective-c++"
    return "c++"


def _preprocess_helper_selection_key(
    arguments: tuple[str, ...],
    family: CompilerFamily,
    working_directory: Path,
    preprocess_language: str | None = None,
) -> tuple[str, tuple[str, ...]]:
    language = preprocess_language or _preprocess_language(
        arguments, family, working_directory
    )
    selectors: list[str] = []
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        _global, _per_source, language_bindings = _msvc_language_selectors(
            arguments, working_directory
        )
        selectors.extend(language_bindings)
    index = 0
    value_options = {"-B", "--gcc-toolchain", "--target", "-target"}
    while index < len(arguments):
        value = arguments[index]
        if value in value_options:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(
                    f"compiler helper selector requires a value: {value}"
                )
            selectors.extend((value, arguments[index + 1]))
            index += 2
            continue
        if (
            value.startswith("-B") and value != "-B"
            or value.startswith("--gcc-toolchain=")
            or value.startswith("--target=")
            or value.startswith("-target=")
            or value.startswith("-specs=")
        ):
            selectors.append(value)
        index += 1
    return language, tuple(selectors)


def open_compiler_executable_capability(
    compiler: Path,
    dependency_roots: DependencyRootAuthority,
    pipeline_deadline: float,
    *,
    cancel_event: object | None = None,
    compiler_family: CompilerFamily | None = None,
    launcher_environment: Mapping[str, str] | None = None,
    working_directory: Path | None = None,
    preprocess_arguments: tuple[str, ...] | None = None,
    preprocess_language: str | None = None,
    _extra_candidates: tuple[Path, ...] = (),
    _query_driver: bool = True,
) -> CompilerExecutableCapability:
    authority = validate_dependency_root_authority(dependency_roots)
    if not isinstance(pipeline_deadline, (int, float)) or isinstance(pipeline_deadline, bool):
        raise AuditInfrastructureError("compiler capability deadline is invalid")
    _check_capability_budget(pipeline_deadline, cancel_event)
    try:
        canonical = compiler.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise AuditInfrastructureError("compiler executable is unavailable") from error
    binding = _toolchain_binding_for_path(canonical, authority)
    if compiler_family is not None and not isinstance(compiler_family, CompilerFamily):
        raise AuditInfrastructureError("compiler capability family is invalid")
    if launcher_environment is not None and not isinstance(launcher_environment, Mapping):
        raise AuditInfrastructureError("compiler capability environment is invalid")
    if preprocess_arguments is not None and (
        not isinstance(preprocess_arguments, tuple)
        or not all(isinstance(value, str) for value in preprocess_arguments)
    ):
        raise AuditInfrastructureError("compiler preprocessing arguments are invalid")
    if preprocess_language is not None and preprocess_language not in {
        "c", "c++", "objective-c", "objective-c++"
    }:
        raise AuditInfrastructureError("compiler preprocessing language is invalid")
    query_environment = (
        os.environ if launcher_environment is None else launcher_environment
    )
    query_working_directory = working_directory or canonical.parent
    try:
        query_working_directory = query_working_directory.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise AuditInfrastructureError(
            "compiler capability working directory is unavailable"
        ) from error
    helper_selection_key = (
        None
        if preprocess_arguments is None or compiler_family is None
        else _preprocess_helper_selection_key(
            preprocess_arguments,
            compiler_family,
            query_working_directory,
            preprocess_language,
        )
    )
    memo_key = (
        os.path.normcase(str(canonical)),
        authority.source_root,
        authority.external_roots,
        compiler_family,
        _environment_digest(query_environment),
        os.path.normcase(str(query_working_directory)),
        helper_selection_key,
    )
    with _compiler_capability_lock:
        memoized = _compiler_capability_memo.get(memo_key)
        if memoized is not None:
            try:
                memoized.native_owner.validate(
                    content=False, deadline=pipeline_deadline,
                    cancel_event=cancel_event,
                )
            except AuditInfrastructureError:
                try:
                    memoized.native_owner.close()
                finally:
                    _compiler_capability_memo.pop(memo_key, None)
            else:
                return memoized

    candidates = [canonical]
    candidates.extend(_extra_candidates)
    index = (
        _toolchain_file_index(
            binding.resolved_root, pipeline_deadline, cancel_event
        )
        if compiler_family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
        else {}
    )
    platform_kind = (
        "windows" if os.name == "nt"
        else ("macos" if sys.platform == "darwin" else "linux")
    )
    candidate_paths, alias_paths = _recursive_runtime_paths(
        tuple(candidates), canonical, authority, platform_kind,
        query_environment, query_working_directory,
        pipeline_deadline, cancel_event,
    )
    alias_paths = tuple(dict.fromkeys(
        Path(os.path.normpath(str(path))) for path in alias_paths
    ))
    candidates = list(candidate_paths)

    streams: list[object] = []
    observer: _FilesystemGenerationObserver | None = None
    paths: list[Path] = []
    snapshots: list[tuple[int, int | None, int, int, int, int]] = []
    hashes: list[str] = []
    total_bytes = 0
    try:
        seen: set[tuple[int, int | None]] = set()
        for candidate in candidates:
            _check_capability_budget(pipeline_deadline, cancel_event)
            try:
                resolved = candidate.resolve(strict=True)
                expected = _regular_file_snapshot(resolved)
                if expected[2] > _RUNTIME_FILE_BYTES:
                    raise AuditInfrastructureError(
                        "compiler runtime per-file byte ceiling exceeded"
                    )
                total_bytes += expected[2]
                if total_bytes > _RUNTIME_TOTAL_BYTES:
                    raise AuditInfrastructureError(
                        "compiler runtime total byte ceiling exceeded"
                    )
                stream = resolved.open("rb")
            except OSError as error:
                raise AuditInfrastructureError("compiler runtime closure changed while opening") from error
            opened = os.fstat(stream.fileno())
            opened_key = (
                int(opened.st_dev),
                int(opened.st_ino) if int(opened.st_ino) != 0 else None,
            )
            if opened_key != expected[:2] or opened_key in seen:
                stream.close()
                if opened_key in seen:
                    continue
                raise AuditInfrastructureError("compiler runtime closure identity changed")
            seen.add(opened_key)
            paths.append(resolved)
            snapshots.append(expected)
            streams.append(stream)
            hashes.append(_content_sha256(
                stream, deadline=pipeline_deadline, cancel_event=cancel_event
            ))

        chain_paths = _runtime_path_chains(
            authority, (*paths, *alias_paths)
        )
        chain_snapshots = tuple(_directory_snapshot(path) for path in chain_paths)
        alias_snapshots = tuple(_symlink_snapshot(path) for path in alias_paths)
        closure: list[DependencyDigest] = []
        for path, snapshot, content in zip(paths, snapshots, hashes):
            runtime_binding = _runtime_binding_for_path(path, authority)
            relative = PurePosixPath(
                path.relative_to(runtime_binding.resolved_root).as_posix()
            )
            closure.append(
                DependencyDigest(
                    runtime_binding.stable_role,
                    relative,
                    FileIdentity(path, None, snapshot[0], snapshot[1], 0, False),
                    content,
                )
            )
        closure.sort(key=lambda item: (item.stable_role, item.role_relative_path.as_posix(), item.sha256))
        closure_digest = hashlib.sha256(
            b"".join(
                len(portable_compiler_inspection_key_value).to_bytes(8, "little")
                + portable_compiler_inspection_key_value
                for item in closure
                for portable_compiler_inspection_key_value in (
                    item.stable_role.encode("ascii")
                    + b"\0"
                    + item.role_relative_path.as_posix().encode("utf-8")
                    + b"\0"
                    + item.sha256.encode("ascii"),
                )
            )
        ).hexdigest()
        capability_digest_hasher = hashlib.sha256()
        for value in (
            platform_kind,
            binding.stable_role,
            canonical.relative_to(binding.resolved_root).as_posix(),
            hashes[0],
            closure_digest,
            authority.portable_authority_digest,
        ):
            _hash_field(capability_digest_hasher, value.encode("utf-8"))
        owner = _CompilerCapabilityOwner(
            tuple(streams), tuple(paths), tuple(snapshots), tuple(hashes),
            tuple(alias_paths), alias_snapshots,
            chain_paths, chain_snapshots,
            _FilesystemGenerationObserver(
                tuple((path, True) for path in chain_paths)
                + tuple((path, False) for path in alias_paths)
                + tuple((path, False) for path in paths)
            ),
            authority,
        )
        observer = owner.observer
        capability = CompilerExecutableCapability(
            platform_kind,
            FileIdentity(canonical, None, snapshots[0][0], snapshots[0][1], 0, False),
            hashes[0],
            capability_digest_hasher.hexdigest(),
            owner,
            binding,
            tuple(chain_paths),
            tuple(
                FileIdentity(path, None, snapshot[0], snapshot[1], 0, False)
                for path, snapshot in zip(chain_paths, chain_snapshots)
            ),
            closure_digest,
            tuple(closure),
        )
        owner.validate(
            content=False, deadline=pipeline_deadline,
            cancel_event=cancel_event,
        )
        if compiler_family is not None and _query_driver:
            helpers = _driver_selected_helper_paths(
                capability,
                compiler_family,
                query_working_directory,
                query_environment,
                binding.resolved_root,
                index,
                pipeline_deadline,
                cancel_event,
                preprocess_arguments,
                preprocess_language,
            )
            if any(path not in paths for path in helpers):
                owner.close()
                return open_compiler_executable_capability(
                    canonical,
                    authority,
                    pipeline_deadline,
                    cancel_event=cancel_event,
                    compiler_family=compiler_family,
                    launcher_environment=launcher_environment,
                    working_directory=query_working_directory,
                    preprocess_arguments=preprocess_arguments,
                    preprocess_language=preprocess_language,
                    _extra_candidates=helpers,
                    _query_driver=False,
                )
        with _compiler_capability_lock:
            existing = _compiler_capability_memo.get(memo_key)
            if existing is not None:
                owner.close()
                existing.native_owner.validate(
                    content=False, deadline=pipeline_deadline,
                    cancel_event=cancel_event,
                )
                return existing
            _compiler_capability_memo[memo_key] = capability
        return capability
    except BaseException:
        if observer is not None:
            try:
                observer.close()
            except Exception:
                pass
        for stream in reversed(streams):
            try:
                stream.close()
            except Exception:
                pass
        raise


def validate_compiler_executable_capability(
    capability: CompilerExecutableCapability,
    dependency_roots: DependencyRootAuthority,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> None:
    authority = validate_dependency_root_authority(
        dependency_roots,
    )
    if not isinstance(capability, CompilerExecutableCapability):
        raise AuditInfrastructureError("compiler executable capability is invalid")
    owner = capability.native_owner
    if not isinstance(owner, _CompilerCapabilityOwner):
        raise AuditInfrastructureError("compiler executable capability owner is invalid")
    if owner.dependency_root_authority is not authority:
        raise AuditInfrastructureError("compiler capability local dependency authority differs")
    expected_binding = _toolchain_binding_for_path(
        capability.executable_identity.canonical, authority
    )
    if expected_binding != capability.trusted_toolchain_root:
        raise AuditInfrastructureError("compiler executable capability root differs")
    owner.validate(
        content=False, deadline=deadline, cancel_event=cancel_event
    )


def _compiler_fingerprint(
    compiler: Path,
    normalized_version: bytes,
    expected_snapshot: tuple[int, int, int, int, int],
) -> str:
    before_snapshot = _compiler_metadata_snapshot(compiler)
    if before_snapshot != expected_snapshot:
        raise AuditInfrastructureError(
            f"compiler executable changed during compiler version probe: {compiler}"
        )
    hasher = hashlib.sha256()
    _hash_field(hasher, str(compiler).encode("utf-8", errors="surrogatepass"))
    _hash_field(hasher, str(expected_snapshot[2]).encode("ascii"))
    _hash_field(hasher, str(expected_snapshot[3]).encode("ascii"))
    _hash_field(hasher, str(expected_snapshot[4]).encode("ascii"))
    content = hashlib.sha256()
    try:
        with compiler.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                content.update(chunk)
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {compiler}") from error
    if _compiler_metadata_snapshot(compiler) != expected_snapshot:
        raise AuditInfrastructureError(f"compiler executable changed while fingerprinting: {compiler}")
    _hash_field(hasher, content.digest())
    _hash_field(hasher, normalized_version)
    return hasher.hexdigest()


def inspect_compiler(
    compiler: Path,
    compiler_family: CompilerFamily,
    launcher_environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    inspection_cache=None,
    expected_audit_engine_fingerprint: str = "",
    pipeline_deadline: float | None = None,
    limits: AuditLimits | None = None,
    launch_accountant=None,
    *,
    working_directory: Path | None = None,
    preprocess_arguments: tuple[str, ...] | None = None,
    preprocess_language: str | None = None,
    compiler_capability: CompilerExecutableCapability | None = None,
) -> CompilerInspection:
    """Inspect a held compiler capability and return portable exact evidence."""

    del limits
    if not isinstance(compiler_family, CompilerFamily):
        raise AuditInfrastructureError("compiler inspection family is invalid")
    if not isinstance(launcher_environment, Mapping):
        raise AuditInfrastructureError("compiler inspection environment is invalid")
    authority = (
        dependency_roots
        if compiler_capability is not None
        and isinstance(dependency_roots, DependencyRootAuthority)
        else validate_dependency_root_authority(dependency_roots)
    )
    deadline = (
        time.monotonic() + _VERSION_SECONDS
        if pipeline_deadline is None
        else pipeline_deadline
    )
    if not isinstance(deadline, (int, float)) or isinstance(deadline, bool):
        raise AuditInfrastructureError("compiler inspection deadline is invalid")
    if time.monotonic() >= deadline:
        raise AuditInfrastructureError("compiler inspection deadline exceeded")
    environment_digest = _environment_digest(launcher_environment)
    cwd = working_directory or compiler.parent

    compiler_snapshot = _compiler_metadata_snapshot(compiler)
    capability = compiler_capability
    if capability is None:
        capability = open_compiler_executable_capability(
            compiler,
            authority,
            deadline,
            compiler_family=compiler_family,
            launcher_environment=launcher_environment,
            working_directory=cwd,
            preprocess_arguments=preprocess_arguments,
            preprocess_language=preprocess_language,
        )
    elif (
        not isinstance(capability, CompilerExecutableCapability)
        or capability.executable_identity.canonical != compiler
    ):
        raise AuditInfrastructureError("compiler inspection capability differs")
    owner = capability.native_owner
    try:
        validate_compiler_executable_capability(
            capability, authority, deadline=deadline
        )
        if _compiler_metadata_snapshot(compiler) != compiler_snapshot:
            raise AuditInfrastructureError(
                "compiler executable changed during compiler version probe: "
                f"{compiler}"
            )
        arguments = (
            "/nologo", "/Bv", "/EP", "/TP"
        ) if compiler_family is CompilerFamily.MSVC else ("--version",)
        arguments_digest = hashlib.sha256()
        for value in (
            compiler_family.value,
            *arguments,
            environment_digest,
            authority.portable_authority_digest,
        ):
            _hash_field(arguments_digest, value.encode("utf-8"))
        inspection_arguments_digest = arguments_digest.hexdigest()
        memo_key = (
            capability.executable_sha256,
            capability.capability_digest,
            environment_digest,
            compiler_family,
            authority.portable_authority_digest,
            capability.executable_identity.canonical,
            capability.executable_identity.device,
            capability.executable_identity.inode,
        )

        with _compiler_inspection_lock:
            memoized = _compiler_inspection_memo.get(memo_key)
            if memoized is not None:
                return memoized

            cached = None
            if inspection_cache is not None:
                load = getattr(inspection_cache, "load", None)
                if not callable(load):
                    raise AuditInfrastructureError("compiler inspection cache is invalid")
                cached = load(
                    compiler,
                    compiler_family,
                    launcher_environment,
                    authority,
                    expected_audit_engine_fingerprint,
                    capability.capability_digest,
                    capability.resolved_runtime_closure_digest,
                    deadline,
                    held_executable_identity=capability.executable_identity,
                    held_executable_sha256=capability.executable_sha256,
                )
            if cached is not None:
                if (
                    not isinstance(cached, CompilerInspection)
                    or cached.compiler_family is not compiler_family
                    or cached.executable_identity != capability.executable_identity
                    or cached.executable_sha256 != capability.executable_sha256
                    or cached.inspection_arguments_digest != inspection_arguments_digest
                    or cached.executable_capability_digest != capability.capability_digest
                ):
                    raise AuditInfrastructureError("compiler inspection cache authority differs")
                validate_compiler_executable_capability(
                    capability, authority, deadline=deadline
                )
                _compiler_inspection_memo[memo_key] = cached
                return cached

            version_output = _probe_compiler_version(
                capability,
                compiler_family,
                cwd,
                launcher_environment,
                deadline,
                launch_observer=launch_accountant,
            )
            validate_compiler_executable_capability(
                capability, authority, deadline=deadline
            )
            family = identify_compiler(compiler, version_output)
            if family is not compiler_family:
                raise AuditInfrastructureError("compiler inspection family changed")
            normalized_version_bytes = _normalize_version_output(version_output)
            normalized_version = normalized_version_bytes.decode("utf-8")
            driver = hashlib.sha256()
            for value in (
                family.value.encode("ascii"),
                capability.executable_sha256.encode("ascii"),
                normalized_version_bytes,
                capability.capability_digest.encode("ascii"),
                authority.portable_authority_digest.encode("ascii"),
            ):
                _hash_field(driver, value)
            inspection = CompilerInspection(
                family,
                capability.executable_identity,
                capability.executable_sha256,
                normalized_version,
                driver.hexdigest(),
                inspection_arguments_digest,
                capability.capability_digest,
                validation_deadline=deadline,
            )
            # Force construction of the relocation-stable key before cache publication.
            portable_compiler_inspection_key(inspection)
            if inspection_cache is not None:
                publish = getattr(inspection_cache, "publish", None)
                if not callable(publish):
                    raise AuditInfrastructureError("compiler inspection cache is invalid")
                inspection = publish(
                    compiler,
                    compiler_family,
                    launcher_environment,
                    authority,
                    expected_audit_engine_fingerprint,
                    capability.capability_digest,
                    capability.resolved_runtime_closure_digest,
                    inspection,
                    deadline,
                )
            validate_compiler_executable_capability(
                capability, authority, deadline=deadline
            )
            _compiler_inspection_memo[memo_key] = inspection
            return inspection
    finally:
        # The process-local capability memo owns the held closure. Configuration
        # objects borrow the same validated capability without reopening it.
        pass


def _inspect_compiler(
    compiler: Path,
    family_hint: CompilerFamily,
    working_directory: Path,
    environment: Mapping[str, str],
    environment_digest: str,
    dependency_roots: DependencyRootAuthority,
) -> CompilerInspection:
    if environment_digest != _environment_digest(environment):
        raise AuditInfrastructureError("compiler environment digest mismatch")
    return inspect_compiler(
        compiler,
        family_hint,
        environment,
        dependency_roots,
        pipeline_deadline=time.monotonic() + _VERSION_SECONDS,
        working_directory=working_directory,
    )


def _canonical_argument_path(value: str, cwd: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = cwd / path
    return path.resolve(strict=False)


def _msvc_slash_spelling(value: str) -> str:
    if value.startswith(("-", "/")):
        return f"/{value[1:]}"
    return value


def _reject_msvc_case_variant(value: str) -> None:
    slash_spelling = _msvc_slash_spelling(value)
    _reject_msvc_exact_case_variant(value)
    if (
        slash_spelling in _MSVC_CASE_DISTINCT_EXACT_OPTIONS
        or slash_spelling.startswith(_MSVC_CASE_DISTINCT_PREFIX_OPTIONS)
        or slash_spelling in _MSVC_CASE_SENSITIVE_EXACT_OPTIONS
        or any(
            slash_spelling.startswith(prefix)
            for prefix in _MSVC_CASE_SENSITIVE_PREFIX_OPTIONS
        )
    ):
        return
    # A dash may introduce an unrelated GNU-compatible clang-cl option such as
    # -fmodules. Exact cl options above still enforce case, but only slash
    # spellings are unambiguously members of the attached cl-option grammar.
    if value.startswith("-"):
        return
    folded = slash_spelling.casefold()
    if any(
        folded == option.casefold()
        for option in _MSVC_AMBIGUOUS_EXACT_OPTIONS
    ) or any(
        folded.startswith(prefix.casefold())
        for prefix in _MSVC_AMBIGUOUS_PREFIX_OPTIONS
    ):
        raise AuditInfrastructureError(
            f"case-sensitive compiler option has unsupported spelling: {value}"
        )


def _reject_msvc_exact_case_variant(value: str) -> None:
    slash_spelling = _msvc_slash_spelling(value)
    if (
        slash_spelling in _MSVC_CASE_DISTINCT_EXACT_OPTIONS
        or slash_spelling.startswith(_MSVC_CASE_DISTINCT_PREFIX_OPTIONS)
    ):
        return
    if slash_spelling in _MSVC_CASE_SENSITIVE_EXACT_OPTIONS:
        return
    folded = slash_spelling.casefold()
    ambiguous = any(
        folded == option.casefold()
        for option in _MSVC_AMBIGUOUS_EXACT_OPTIONS
    )
    # Resolve a dash option before clang-cl's GNU compatibility grammar. In
    # particular, -d takes a GNU operand but is not the documented MSVC /D.
    gnu_arity_collision = value.startswith("-") and value in _GNU_VALUE_OPTIONS and any(
        folded == option.casefold() for option in _MSVC_VALUE_OPTIONS
    )
    if ambiguous or gnu_arity_collision:
        raise AuditInfrastructureError(
            f"case-sensitive compiler option has unsupported spelling: {value}"
        )


def _source_inputs(
    arguments: tuple[str, ...], cwd: Path, family: CompilerFamily
) -> tuple[Path, ...]:
    sources: list[Path] = []
    index = 0
    positional_only = False
    while index < len(arguments):
        value = arguments[index]
        if value == "-":
            raise AuditInfrastructureError("stdin cannot be a compile source")
        if value == "--" and family in {
            CompilerFamily.GCC, CompilerFamily.CLANG, CompilerFamily.CLANG_CL
        }:
            positional_only = True
            index += 1
            continue
        if (
            not positional_only
            and family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
            and value.startswith(("-", "/"))
        ):
            # Resolve exact cl spellings before clang-cl's GNU compatibility
            # options: for example, -d must not silently acquire GNU arity and
            # hide a source when /D is the case-sensitive cl spelling.
            _reject_msvc_exact_case_variant(value)
        if not positional_only and value in _GNU_VALUE_OPTIONS and family in {
            CompilerFamily.GCC, CompilerFamily.CLANG, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        msvc_spelling = _msvc_slash_spelling(value)
        if (
            not positional_only
            and family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
            and value.startswith(("-", "/"))
        ):
            _reject_msvc_case_variant(value)
        if (
            not positional_only
            and msvc_spelling == "/sourceDependencies:directives"
            and family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
        ):
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        slash_spelling = msvc_spelling
        if not positional_only and slash_spelling in {"/TC", "/TP"} and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            index += 1
            continue
        if not positional_only and slash_spelling in {"/Tc", "/Tp"} and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a source: {value}")
            candidate = arguments[index + 1]
            if candidate == "-":
                raise AuditInfrastructureError("stdin cannot be a compile source")
            sources.append(_canonical_argument_path(candidate, cwd))
            index += 2
            continue
        if (
            not positional_only
            and slash_spelling.startswith(("/Tc", "/Tp"))
            and len(value) > 3
        ):
            candidate = value[3:]
            if candidate == "-":
                raise AuditInfrastructureError("stdin cannot be a compile source")
            sources.append(_canonical_argument_path(candidate, cwd))
            index += 1
            continue
        if not positional_only and msvc_spelling in _MSVC_VALUE_OPTIONS and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if not positional_only and value.startswith("-"):
            index += 1
            continue
        if not positional_only and value.startswith("/") and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            index += 1
            continue
        sources.append(_canonical_argument_path(value, cwd))
        index += 1
    return tuple(sources)


def _same_path(first: Path, second: Path) -> bool:
    return os.path.normcase(str(first)) == os.path.normcase(str(second))


_GNU_REWRITE_REMOVE_FLAGS = frozenset({
    "-c", "-S", "-E", "-P", "-M", "-MM", "-MD", "-MMD", "-MG", "-MP",
    "--dependencies", "--user-dependencies", "--write-dependencies",
    "--write-user-dependencies", "--print-missing-file-dependencies",
    "--no-line-commands", "-dM", "-dD", "-dN", "-dI", "-dU",
})
_GNU_REWRITE_REMOVE_VALUES = frozenset({
    "-o", "-MF", "-MT", "-MQ", "-MJ", "-serialize-diagnostics",
    "--serialize-diagnostics", "-dependency-file", "--dependency-file",
    "--output", "--dump", "-d",
})
_GNU_REWRITE_ATTACHED_VALUES = (
    "-MF", "-MT", "-MQ", "-MJ", "-o",
)
_GNU_REWRITE_EQUALS_VALUES = (
    "-fdiagnostics-file=", "-fdiagnostics-serialization-file=",
    "-fmodule-output=", "--output=", "--dump=",
)
_GNU_REWRITE_REJECT_PREFIXES = (
    "-save-temps", "--save-temps", "-dumpbase", "-dumpdir",
    "-fmodule-output", "-fpreprocessed", "-fdirectives-only",
    "-frewrite-includes",
)
_GNU_FORWARDERS = frozenset({"-Xclang", "-Xpreprocessor"})
_GNU_FORWARDED_SEPARATE_VALUE_OPTIONS = frozenset({
    "-D", "-U", "-A", "-I", "-isystem", "-iquote", "-idirafter",
    "-iprefix", "-iwithprefix", "-iwithprefixbefore", "-include", "-imacros",
    "-include-pch",
    "-isysroot", "--sysroot", "--define-macro", "--undefine-macro",
    "--include", "--imacros",
    "-F", "-iframework", "-ivfsoverlay", "-resource-dir",
    "-triple", "-aux-triple", "-target-cpu", "-target-feature", "-target-abi",
    "-fmodules-user-build-path", "-fmodule-implementation-of", "-fmodule-feature",
    "-mrelocation-model", "-mthread-model", "-target-linker-version",
})

_CLANG_FRONTEND_DEPENDENCY_OPTIONS = frozenset({
    "-MD", "-MMD", "-M", "-MM", "-MG", "-MP", "-MF", "-MT", "-MQ", "-MJ",
    "-dependency-dot", "-dependency-file", "--dependency-file",
    "--dependencies", "--user-dependencies", "--write-dependencies",
    "--write-user-dependencies", "--print-missing-file-dependencies",
    "-sys-header-deps", "-module-file-deps", "-show-includes",
})
_CLANG_FRONTEND_OUTPUT_OPTIONS = frozenset({
    "-o", "--output", "--dump", "-d", "-dM", "-dD", "-dN", "-dI", "-dU",
    "-serialize-diagnostics", "--serialize-diagnostics",
    "-serialize-diagnostic-file", "-diagnostic-log-file", "-stats-file",
})
_CLANG_FRONTEND_ACTION_PREFIXES = (
    "-emit-", "-dump-", "-ast-", "-analy", "-plugin", "-add-plugin",
    "-load", "-fplugin", "-code-completion", "-fixit", "-verify",
    "-rewrite-", "-print-", "-action", "-execute", "-arcmt-", "-objcmt-",
    "-migrate", "-index-", "-extract-api",
)
_CLANG_FRONTEND_ACTION_OPTIONS = frozenset({
    "-E", "-Eonly", "-syntax-only", "-fsyntax-only", "-module-file-info",
})
_CLANG_FRONTEND_SAFE_FLAGS = frozenset({
    "-undef", "-nostdinc", "-nostdinc++", "-nobuiltininc", "-pthread",
    "-fmodules", "-fimplicit-module-maps", "-fmodules-decluse",
    "-fmodules-strict-decluse", "-fmodules-local-submodule-visibility",
    "-fcxx-exceptions", "-fexceptions", "-fno-exceptions", "-frtti", "-fno-rtti",
    "-fdelayed-template-parsing", "-fblocks", "-fcoroutines", "-fchar8_t",
    "-fno-char8_t", "-pedantic", "-pedantic-errors",
    "-O", "-O0", "-O1", "-O2", "-O3", "-O4", "-Og", "-Os", "-Oz",
    "-Ofast", "-g", "-g0", "-g1", "-g2", "-g3", "-gline-tables-only",
    "-gline-directives-only", "-mrelax-all", "-mnoexecstack",
    "-masm-verbose", "-mconstructor-aliases", "-msoft-float", "-mstackrealign",
    "-fmodules-validate-once-per-build-session",
    "-fmodule-map-file-home-is-cwd", "-disable-llvm-passes",
    "-fms-extensions", "-fms-compatibility", "-fobjc-arc",
    "-fobjc-arc-exceptions", "-fobjc-weak",
})
_CLANG_FRONTEND_SAFE_PREFIXES = (
    "-D", "-U", "-I", "-F", "-W", "-R",
)
_CLANG_FRONTEND_SAFE_EQUALS_OPTIONS = frozenset({
    "-std", "-stdlib", "-triple", "-target-cpu", "-target-feature",
    "-target-abi", "-target-linker-version", "-target-sdk-version",
    "-fmodule-map-file",
    "-fmodule-file", "-fmodule-name", "-fmodule-format",
    "-fmodules-cache-path", "-fmodules-prune-interval",
    "-fmodules-ignore-macro", "-fmodules-prune-after",
    "-fprebuilt-module-path", "-mframe-pointer", "-mrelocation-model",
    "-mthread-model", "-fms-compatibility-version",
    "-fobjc-runtime", "-debug-info-kind", "-dwarf-version", "-debugger-tuning",
})


def _validate_rewrite_source(
    configuration: PreprocessConfiguration,
    classification: CommandRewriteClassification,
) -> None:
    if (classification.response_expanded_launcher_stripped_arguments
            != configuration.arguments):
        raise AuditInfrastructureError("command rewrite classification differs")
    sources = _source_inputs(
        configuration.arguments,
        configuration.working_directory,
        configuration.family,
    )
    if len(sources) > 1:
        raise AuditInfrastructureError("compile command has multiple source inputs")
    if not sources or not _same_path(sources[0], configuration.source.canonical):
        raise AuditInfrastructureError("compile command source does not match configuration")


def _gnu_forwarded_control(payload: str) -> str | None:
    candidate = payload
    if not candidate.startswith(("-", "@")):
        return None
    lowered = candidate.casefold()
    option_name = candidate.partition("=")[0]
    if candidate in {"-P", "--no-line-commands"} or lowered.startswith(
        "-frewrite-includes"
    ):
        return "marker"
    if candidate in {"-c", "-S", "-x", "-ObjC", "-ObjC++"} or lowered.startswith(
        ("-fpreprocessed", "-fdirectives-only", "-main-file-name")
    ):
        return "source-selection"
    if "cpp-output" in lowered:
        return "source-selection"
    if candidate in _CLANG_FRONTEND_DEPENDENCY_OPTIONS or any(
        candidate.startswith(prefix) and len(candidate) > len(prefix)
        for prefix in ("-MF", "-MT", "-MQ", "-MJ")
    ) or "dependenc" in option_name.casefold():
        return "dependency"
    if candidate == "-o" or candidate.startswith("-o") and len(candidate) > 2:
        return "output"
    if candidate in _CLANG_FRONTEND_OUTPUT_OPTIONS:
        return "output"
    if lowered.startswith(("--output=", "--dump=")) or candidate == "--dump":
        return "output"
    if lowered.startswith((
        "-save-temps", "--save-temps", "-dumpbase", "-dumpdir",
        "-fmodule-output", "-serialize-diagnostics", "--serialize-diagnostics",
        "-serialize-diagnostic-file", "-diagnostic-log-file", "-stats-file",
        "-fdiagnostics-file=", "-fdiagnostics-serialization-file=",
        "-gen-reproducer",
    )):
        return "output"
    if option_name in _CLANG_FRONTEND_ACTION_OPTIONS or option_name.startswith(
        _CLANG_FRONTEND_ACTION_PREFIXES
    ):
        return "action"
    # cc1 is explicitly unstable. Do not allow a future action selector merely
    # because its spelling was absent when this audit was authored.
    if any(
        fragment in option_name.casefold()
        for fragment in (
            "action", "completion", "plugin", "analy", "fixit", "verify",
            "rewrite", "emit", "dump", "output",
        )
    ):
        return "action"
    if candidate.startswith("@"):
        return "response"
    if (
        candidate in _CLANG_FRONTEND_SAFE_FLAGS
        or candidate.startswith(_CLANG_FRONTEND_SAFE_PREFIXES)
        or (
            option_name in _CLANG_FRONTEND_SAFE_EQUALS_OPTIONS
            and "=" in candidate
            and bool(candidate.partition("=")[2])
        )
    ):
        return None
    return "ambiguous"


@dataclass(frozen=True, slots=True)
class _ForwardedSpan:
    text: str
    start: int
    stop: int


class _CommaSpanCursor:
    __slots__ = ("text", "position", "stop")

    def __init__(self, text: str, position: int, stop: int) -> None:
        self.text = text
        self.position = position
        self.stop = stop

    def next_span(self) -> _ForwardedSpan | None:
        if self.position > self.stop:
            return None
        comma = self.text.find(",", self.position, self.stop)
        if comma < 0:
            result = _ForwardedSpan(self.text, self.position, self.stop)
            self.position = self.stop + 1
            return result
        result = _ForwardedSpan(self.text, self.position, comma)
        self.position = comma + 1
        return result


def _span_equals(span: _ForwardedSpan, value: str) -> bool:
    return span.stop - span.start == len(value) and span.text.startswith(
        value, span.start, span.stop
    )


def _span_startswith(span: _ForwardedSpan, value: str) -> bool:
    return span.text.startswith(value, span.start, span.stop)


def _gnu_flattened_forwarded_arguments(
    payloads: Iterable[str],
) -> Iterator[_ForwardedSpan]:
    """Flatten forwarding grammar in linear time without copying suffixes."""

    source = iter(payloads)
    pending: deque[_ForwardedSpan | _CommaSpanCursor] = deque()

    def next_span() -> _ForwardedSpan:
        while pending:
            current = pending[0]
            if isinstance(current, _ForwardedSpan):
                pending.popleft()
                return current
            result = current.next_span()
            if result is not None:
                return result
            pending.popleft()
        value = next(source)
        return _ForwardedSpan(value, 0, len(value))

    while True:
        try:
            span = next_span()
        except StopIteration:
            break
        while True:
            nested_forwarder = next(
                (
                    candidate for candidate in _GNU_FORWARDERS
                    if _span_equals(span, candidate)
                    or _span_startswith(span, f"{candidate}=")
                ),
                None,
            )
            if nested_forwarder is not None:
                if _span_equals(span, nested_forwarder):
                    try:
                        span = next_span()
                    except StopIteration as error:
                        raise AuditInfrastructureError(
                            f"forwarded compiler option requires a value: {nested_forwarder}"
                        ) from error
                else:
                    start = span.start + len(nested_forwarder) + 1
                    if start == span.stop:
                        value = span.text[span.start:span.stop]
                        raise AuditInfrastructureError(
                            f"forwarded compiler option requires a value: {value}"
                        )
                    span = _ForwardedSpan(span.text, start, span.stop)
                continue
            if (
                span.stop - span.start >= 4
                and span.text[span.start:span.start + 4].casefold() == "-wp,"
            ):
                pending.appendleft(
                    _CommaSpanCursor(span.text, span.start + 4, span.stop)
                )
                break
            yield span
            break


def _gnu_forwarded_sequence_control(payloads: Iterable[str]) -> str | None:
    pending_value: str | None = None
    for span in _gnu_flattened_forwarded_arguments(payloads):
        if pending_value is not None:
            pending_value = None
            continue
        value_option = next(
            (
                candidate for candidate in _GNU_FORWARDED_SEPARATE_VALUE_OPTIONS
                if _span_equals(span, candidate)
            ),
            None,
        )
        if value_option is not None:
            pending_value = value_option
            continue
        if span.start == span.stop or span.text[span.start] not in "-@":
            continue
        category = _gnu_forwarded_control(span.text[span.start:span.stop])
        if category:
            return category
    if pending_value is not None:
        raise AuditInfrastructureError(
            f"forwarded compiler option requires a value: {pending_value}"
        )
    return None


def _clang_cl_forwarded_arguments(arguments: tuple[str, ...]) -> Iterator[str]:
    """Yield clang-cl forwarding in the order the driver constructs cc1 args.

    Clang's driver appends -Wp/-Xpreprocessor values while assembling
    preprocessing options, then appends -Xclang values near the end of the cc1
    command. /clang: is clang-cl's spelling of the latter. Preserve argv order
    within each bucket, but never let an operand cross the real bucket order.
    """

    for xclang_bucket in (False, True):
        index = 0
        while index < len(arguments):
            value = arguments[index]
            option = _msvc_option(value)
            if option.startswith("/clang:"):
                payload = value[len(value.partition(":")[0]) + 1 :]
                if not payload:
                    raise AuditInfrastructureError(
                        f"compiler option requires a value: {value}"
                    )
                if xclang_bucket:
                    yield payload
                index += 1
                continue
            if value.startswith("-Wp,"):
                if not xclang_bucket:
                    yield value
                index += 1
                continue
            forwarder = next(
                (
                    candidate for candidate in _GNU_FORWARDERS
                    if value == candidate or value.startswith(f"{candidate}=")
                ),
                None,
            )
            if forwarder is not None:
                current = _gnu_forwarded_argument(arguments, index, forwarder)
                if current is None:
                    raise AuditInfrastructureError(
                        "forwarded compiler argument disappeared during classification"
                    )
                _, end = current
                if (forwarder == "-Xclang") == xclang_bucket:
                    yield from arguments[index:end]
                index = end
                continue
            index += 1


def _gnu_forwarded_argument(
    arguments: tuple[str, ...], index: int, forwarder: str
) -> tuple[str, int] | None:
    value = arguments[index]
    if value == forwarder:
        if index + 1 >= len(arguments):
            raise AuditInfrastructureError(f"compiler option requires a value: {value}")
        return arguments[index + 1], index + 2
    elif value.startswith(f"{forwarder}="):
        payload = value.partition("=")[2]
        if not payload:
            raise AuditInfrastructureError(f"compiler option requires a value: {value}")
        return payload, index + 1
    return None


def _gnu_forwarded_span(
    arguments: tuple[str, ...], index: int, forwarder: str
) -> tuple[int, str | None] | None:
    current = _gnu_forwarded_argument(arguments, index, forwarder)
    if current is None:
        return None
    payload, end = current
    if payload not in _GNU_FORWARDED_SEPARATE_VALUE_OPTIONS:
        return end, _gnu_forwarded_sequence_control((payload,))
    if end >= len(arguments):
        raise AuditInfrastructureError(
            f"forwarded compiler option requires a value: {payload}"
        )
    operand = _gnu_forwarded_argument(arguments, end, forwarder)
    if operand is None:
        raise AuditInfrastructureError(
            f"forwarded compiler option requires a forwarded value: {payload}"
        )
    _, operand_end = operand
    return operand_end, None


def _rewrite_gnu(
    configuration: PreprocessConfiguration, dependency_output: Path,
    classification: CommandRewriteClassification,
) -> RewrittenCommand:
    rewritten: list[str] = [str(configuration.compiler)]
    arguments = classification.response_expanded_launcher_stripped_arguments
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == "--":
            raise AuditInfrastructureError("unsupported source-selection option: --")
        if value in _GNU_REWRITE_REMOVE_FLAGS:
            index += 1
            continue
        if value in _GNU_REWRITE_REMOVE_VALUES:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if value in {"--output=", "--dump="}:
            raise AuditInfrastructureError(f"compiler option requires a value: {value}")
        if any(
            value.startswith(prefix) and len(value) > len(prefix)
            for prefix in _GNU_REWRITE_ATTACHED_VALUES
        ) or value.startswith(_GNU_REWRITE_EQUALS_VALUES):
            index += 1
            continue
        if value.startswith(_GNU_REWRITE_REJECT_PREFIXES):
            category = "source-selection" if value.startswith(
                ("-fpreprocessed", "-fdirectives-only", "-frewrite-includes")
            ) else "output"
            raise AuditInfrastructureError(f"unsupported {category} option: {value}")
        if value.startswith("-Wp,"):
            category = _gnu_forwarded_sequence_control((value,))
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}"
                )
        forwarder = next(
            (candidate for candidate in _GNU_FORWARDERS
             if value == candidate or value.startswith(f"{candidate}=")),
            None,
        )
        if forwarder is not None:
            end, category = _gnu_forwarded_span(arguments, index, forwarder)
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}"
                )
            rewritten.extend(arguments[index:end])
            index = end
            continue
        if value == "-x":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError("compiler option requires a value: -x")
            language = arguments[index + 1]
            if "cpp-output" in language.casefold() or "preprocessed" in language.casefold():
                raise AuditInfrastructureError(
                    f"unsupported source-selection language: {language}"
                )
            rewritten.extend((value, language))
            index += 2
            continue
        if value.startswith("-x") and len(value) > 2:
            language = value[2:]
            if "cpp-output" in language.casefold() or "preprocessed" in language.casefold():
                raise AuditInfrastructureError(
                    f"unsupported source-selection language: {language}"
                )
        if value in _GNU_VALUE_OPTIONS:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            rewritten.extend((value, arguments[index + 1]))
            index += 2
            continue
        rewritten.append(value)
        index += 1
    rewritten.extend(("-E", "-MD", "-MF", str(dependency_output)))
    return RewrittenCommand(
        arguments=tuple(rewritten),
        dependency_output=dependency_output,
        dependency_format="gcc-depfile",
    )


_MSVC_REWRITE_REMOVE_FLAGS = frozenset({
    "/c", "/nologo", "/E", "/P", "/EP", "/showIncludes",
    "/PD", "/PH", "/Fx", "/doc",
})
_MSVC_REWRITE_REMOVE_VALUES = frozenset({
    "/Fo", "/Fe", "/Fd", "/Fi",
    "/Ft", "/experimental:log",
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
})
_MSVC_REWRITE_ATTACHED_VALUES = (
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
    "/experimental:log", "/doc",
    "/Fo", "/Fe", "/Fd", "/Fi", "/Ft",
)
_MSVC_REWRITE_OPTIONAL_OUTPUT_PREFIXES = ("/FA", "/Fa", "/Fm", "/FR", "/Fr")
_MSVC_REWRITE_REJECT_PREFIXES = (
    "/OUT", "/link", "/Yc", "/LD", "/clr:netcore",
)
_MSVC_REWRITE_PRESERVE_VALUES = frozenset({
    "/D", "/U", "/I", "/Fp", "/Yu", "/external:I", "/AI", "/FU",
    "/reference", "/headerUnit",
})


def _msvc_option(value: str) -> str:
    return _msvc_slash_spelling(value)


def _msvc_attached_required_output(option: str) -> bool:
    for prefix in _MSVC_REWRITE_ATTACHED_VALUES:
        if not option.startswith(prefix) or len(option) == len(prefix):
            continue
        payload = option[len(prefix):]
        if payload == ":":
            raise AuditInfrastructureError(
                f"compiler option requires a value: {option}"
            )
        return True
    return False


def _rewrite_msvc(
    configuration: PreprocessConfiguration, dependency_output: Path,
    classification: CommandRewriteClassification,
) -> RewrittenCommand:
    rewritten: list[str] = [str(configuration.compiler)]
    arguments = classification.response_expanded_launcher_stripped_arguments
    if configuration.family is CompilerFamily.CLANG_CL:
        category = _gnu_forwarded_sequence_control(
            _clang_cl_forwarded_arguments(arguments)
        )
        if category:
            raise AuditInfrastructureError(
                f"hidden {category} option is unsupported: /clang:"
            )
    index = 0
    while index < len(arguments):
        value = arguments[index]
        option = _msvc_option(value)
        if value.startswith(("-", "/")):
            _reject_msvc_case_variant(value)
        slash_spelling = option
        if slash_spelling == "/FI":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            rewritten.extend((value, arguments[index + 1]))
            index += 2
            continue
        if slash_spelling.startswith("/FI"):
            rewritten.append(value)
            index += 1
            continue
        if configuration.family is CompilerFamily.CLANG_CL and slash_spelling == "/d1PP":
            index += 1
            continue
        if slash_spelling.startswith(_MSVC_REWRITE_OPTIONAL_OUTPUT_PREFIXES):
            index += 1
            continue
        forwarder = next(
            (candidate for candidate in _GNU_FORWARDERS
             if value == candidate or value.startswith(f"{candidate}=")),
            None,
        )
        if forwarder is not None:
            if configuration.family is CompilerFamily.CLANG_CL:
                current = _gnu_forwarded_argument(arguments, index, forwarder)
                if current is None:
                    raise AuditInfrastructureError(
                        "forwarded clang-cl argument disappeared during rewrite"
                    )
                _, end = current
                rewritten.extend(arguments[index:end])
                index = end
                continue
            end, category = _gnu_forwarded_span(arguments, index, forwarder)
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}"
                )
            rewritten.extend(arguments[index:end])
            index = end
            continue
        if option == "/sourceDependencies:directives":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if option.startswith("/clang:"):
            rewritten.append(value)
            index += 1
            continue
        if option in _MSVC_REWRITE_REMOVE_FLAGS or option.startswith("/showIncludes:"):
            index += 1
            continue
        if option in _MSVC_REWRITE_REMOVE_VALUES:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if _msvc_attached_required_output(option):
            index += 1
            continue
        if option in _MSVC_REWRITE_PRESERVE_VALUES:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            rewritten.extend((value, arguments[index + 1]))
            index += 2
            continue
        if option.startswith(_MSVC_REWRITE_REJECT_PREFIXES):
            category = "source-selection" if option.startswith(("/link", "/yc")) else "output"
            raise AuditInfrastructureError(f"unsupported {category} option: {value}")
        rewritten.append(value)
        index += 1
    rewritten.extend(("/nologo", "/E", "/sourceDependencies", str(dependency_output)))
    return RewrittenCommand(
        arguments=tuple(rewritten),
        dependency_output=dependency_output,
        dependency_format="msvc-json",
    )


def rewrite_preprocess_command(
    configuration: PreprocessConfiguration, dependency_output: Path
) -> RewrittenCommand:
    """Rewrite one normalized compile command into a fail-closed preprocess command."""

    if not isinstance(configuration, PreprocessConfiguration):
        raise AuditInfrastructureError("preprocess configuration is invalid")
    if not isinstance(dependency_output, Path) or not dependency_output.is_absolute():
        raise AuditInfrastructureError("dependency output must be an absolute path")
    _validate_arguments((str(configuration.compiler), *configuration.arguments))
    classification = classify_command_rewrite(
        configuration.arguments, configuration.working_directory,
        configuration.family)
    _validate_rewrite_source(configuration, classification)
    if configuration.family in {CompilerFamily.GCC, CompilerFamily.CLANG}:
        rewritten = _rewrite_gnu(configuration, dependency_output, classification)
    elif configuration.family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        rewritten = _rewrite_msvc(configuration, dependency_output, classification)
    else:
        raise AuditInfrastructureError(
            f"unsupported compiler family: {configuration.family}"
        )
    return RewrittenCommand(
        arguments=rewritten.arguments,
        dependency_output=rewritten.dependency_output,
        dependency_format=rewritten.dependency_format,
        classification=classification,
    )


def _reject_driver_dialect_overrides(arguments: tuple[str, ...]) -> None:
    for value in arguments:
        lowered = value.casefold()
        if lowered == "--driver-mode" or lowered.startswith("--driver-mode="):
            raise AuditInfrastructureError(f"explicit compiler driver mode is unsupported: {value}")
        if lowered == "--rsp-quoting" or lowered.startswith("--rsp-quoting="):
            raise AuditInfrastructureError(f"explicit response-file quoting mode is unsupported: {value}")
        if lowered.startswith("/clang:") and (
            "--driver-mode" in lowered or "--rsp-quoting" in lowered
        ):
            raise AuditInfrastructureError(f"hidden compiler driver/response mode is unsupported: {value}")


def _reject_msvc_environment_arguments(environment: Mapping[str, str]) -> None:
    for name, value in environment.items():
        normalized = name.casefold()
        if normalized in {"cl", "_cl_"} and value.strip():
            raise AuditInfrastructureError(
                f"MSVC environment command line {name} is unsupported"
            )


def make_configuration(
    entry: Mapping[str, object],
    database: Path,
    entry_index: int,
    source_root: Path,
    production: Mapping[PurePosixPath, FileIdentity],
    environment: Mapping[str, str],
    limits: AuditLimits,
    dependency_roots: DependencyRootAuthority,
    inspection_cache=None,
    expected_audit_engine_fingerprint: str = "",
    pipeline_deadline: float | None = None,
    launch_accountant=None,
) -> PreprocessConfiguration:
    """Build one stable semantic configuration from a compile database entry."""

    if not isinstance(dependency_roots, DependencyRootAuthority):
        raise AuditInfrastructureError("compile dependency authority is invalid")
    authority = dependency_roots
    if authority.source_root.resolved_root != source_root:
        raise AuditInfrastructureError("compile source root differs from dependency authority")
    if not isinstance(entry_index, int) or isinstance(entry_index, bool) or entry_index < 0:
        raise AuditInfrastructureError("compile database entry index is invalid")
    cwd, decoded = decode_compile_entry(entry, database, windows=os.name == "nt")
    launcher_names: list[str] = []
    for value in decoded:
        name = _launcher_name(value)
        if name not in _LAUNCHERS:
            break
        launcher_names.append(name)
    if "ccache" in launcher_names:
        _validate_ccache_config_sources(decoded, cwd, environment)
        for name in environment:
            if name.casefold() in _CCACHE_COMPILER_ENVIRONMENT:
                raise AuditInfrastructureError(
                    f"ccache compiler environment override is unsupported: {name}"
                )
    compiler_argument, compiler_arguments, _assignments = strip_launchers(decoded)
    _reject_driver_dialect_overrides(compiler_arguments)
    family_hint = _family_from_name(compiler_argument)
    compiler = _resolve_compiler(compiler_argument, cwd, environment)
    environment_digest = _environment_digest(environment)
    expanded_arguments = expand_response_files(
        compiler_arguments, family_hint, cwd, limits
    )
    _reject_driver_dialect_overrides(expanded_arguments)
    _validate_arguments((str(compiler), *expanded_arguments))
    file_value = entry.get("file")
    if not isinstance(file_value, str):
        raise AuditInfrastructureError("compile command file must be a string")
    source_hint = Path(file_value)
    if not source_hint.is_absolute():
        source_hint = cwd / source_hint
    preprocess_language = _preprocess_language(
        expanded_arguments, family_hint, cwd, source_hint
    )
    deadline = pipeline_deadline or (time.monotonic() + limits.total_seconds)
    compiler_capability = open_compiler_executable_capability(
        compiler, authority, deadline, compiler_family=family_hint,
        launcher_environment=environment, working_directory=cwd,
        preprocess_arguments=expanded_arguments,
        preprocess_language=preprocess_language,
    )
    inspection = inspect_compiler(
        compiler,
        family_hint,
        environment,
        authority,
        inspection_cache,
        expected_audit_engine_fingerprint,
        deadline,
        limits,
        launch_accountant,
        working_directory=cwd,
        preprocess_arguments=expanded_arguments,
        preprocess_language=preprocess_language,
        compiler_capability=compiler_capability,
    )
    family = inspection.compiler_family
    compiler_fingerprint = inspection.driver_fingerprint
    if compiler_capability.capability_digest != inspection.executable_capability_digest:
        compiler_capability.native_owner.close()
        raise AuditInfrastructureError("compiler capability changed after inspection")
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        _reject_msvc_environment_arguments(environment)
    source_path = Path(file_value)
    if not source_path.is_absolute():
        source_path = cwd / source_path
    try:
        source_path = source_path.resolve(strict=True)
        canonical_root = authority.source_root.resolved_root
    except OSError as error:
        raise AuditInfrastructureError(f"compile entry source is unavailable: {file_value}") from error
    try:
        source_path.relative_to(canonical_root)
    except ValueError as error:
        raise AuditInfrastructureError(
            f"compile entry source is outside production root: {source_path}"
        ) from error
    identities = [
        identity for identity in production.values()
        if _same_path(identity.canonical, source_path)
    ]
    if len(identities) != 1:
        raise AuditInfrastructureError(
            f"compile entry source is not an enumerated production file: {source_path}"
        )
    source = identities[0]

    source_inputs = _source_inputs(expanded_arguments, cwd, family)
    if len(source_inputs) > 1:
        raise AuditInfrastructureError("compile command has multiple source inputs")
    if not source_inputs:
        raise AuditInfrastructureError("compile command source does not match database entry")
    if not _same_path(source_inputs[0], source_path):
        raise AuditInfrastructureError("compile command source does not match database entry")

    semantic = {
        "schema": 1,
        "family": family.value,
        "compiler": str(compiler),
        "compiler_fingerprint": compiler_fingerprint,
        "compiler_metadata": _compiler_metadata_snapshot(compiler),
        "working_directory": str(cwd),
        "source": str(source.canonical),
        "arguments": expanded_arguments,
        "environment_digest": environment_digest,
    }
    semantic_bytes = json.dumps(
        semantic,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    digest_builder = hashlib.sha256()
    _hash_field(digest_builder, semantic_bytes)
    _hash_field(
        digest_builder, authority.portable_authority_digest.encode("ascii")
    )
    _hash_field(digest_builder, compiler_capability.capability_digest.encode("ascii"))
    digest = digest_builder.hexdigest()
    return PreprocessConfiguration(
        entry_id=f"{database if database.is_absolute() else database.resolve()}:{entry_index}",
        family=family,
        compiler=compiler,
        working_directory=cwd,
        source=source,
        arguments=expanded_arguments,
        environment_digest=environment_digest,
        digest=digest,
        dependency_root_authority_digest=authority.portable_authority_digest,
        compiler_capability_digest=compiler_capability.capability_digest,
        compiler_capability=compiler_capability,
    )


def _command_main(arguments: tuple[str, ...]) -> int:
    if len(arguments) >= 5 and arguments[0] == "--macos-compiler-exec-gate":
        try:
            report_descriptor = int(arguments[1])
            permit_descriptor = int(arguments[2])
        except ValueError as error:
            raise AuditInfrastructureError(
                "macOS compiler exec gate descriptors are invalid"
            ) from error
        _run_macos_compiler_exec_gate(
            report_descriptor,
            permit_descriptor,
            arguments[3],
            tuple(arguments[4:]),
        )
        return 0
    raise AuditInfrastructureError("gpu capability command operation is invalid")


if __name__ == "__main__":
    raise SystemExit(_command_main(tuple(sys.argv[1:])))
