#!/usr/bin/env python3
"""Native process-tree ownership and conservative accounting primitives.

This module deliberately exposes only primitives in Task 6.  The coordinator
that binds them to worker scheduling is introduced by Task 7.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Mapping

from gpu_capability_model import (
    AuditInfrastructureError,
    CompilerExecPermit,
    CompilerLaunchPurpose,
    CompilerPgidReported,
    FileIdentity,
    LinuxRunMemoryMeasurements,
    LinuxWorkerContainment,
    MacOSRunMemoryMeasurements,
    MacOSPhaseSnapshot,
    MacOSInspectionExecPermit,
    MacOSInspectionPgidReported,
    WindowsRunMemoryMeasurements,
)


LINUX_RENDEZVOUS_SCHEMA_BYTES = b"olr-gpu-cgroup-rendezvous-v1"
LINUX_RENDEZVOUS_MAX_BYTES = 16 * 1024
LINUX_MEMORY_MAX_BYTES = 512 << 20
LINUX_MEMORY_HIGH_BYTES = 448 << 20


def _require_contract(condition: bool, message: str) -> None:
    if not condition:
        raise AuditInfrastructureError(message)


def _nonnegative_integer(value: object, label: str) -> int:
    _require_contract(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{label} is invalid",
    )
    return value


@dataclass(frozen=True, slots=True)
class OwnedProcessIdentity:
    platform_kind: str
    pid: int
    native_start_identity: str

    def __post_init__(self) -> None:
        _require_contract(self.platform_kind in {"windows", "linux", "macos"},
                          "owned process platform is invalid")
        _require_contract(isinstance(self.pid, int) and not isinstance(self.pid, bool)
                          and self.pid > 0, "owned process PID is invalid")
        _require_contract(isinstance(self.native_start_identity, str)
                          and bool(self.native_start_identity),
                          "owned process start identity is invalid")


@dataclass(slots=True)
class _OwnedProcessRecord:
    identity: OwnedProcessIdentity
    parent: OwnedProcessIdentity | None
    purpose: str
    current_resident_bytes: int = 0
    peak_resident_bytes: int = 0
    exited: bool = False
    reaped: bool = False


class OwnedProcessTree:
    """Retain complete PID/start identities through exit and explicit reap."""

    def __init__(self, platform_kind: str, run_identity: str) -> None:
        _require_contract(platform_kind in {"windows", "linux", "macos"},
                          "owned process-tree platform is invalid")
        _require_contract(isinstance(run_identity, str) and bool(run_identity),
                          "owned process-tree run identity is invalid")
        self.platform_kind = platform_kind
        self.run_identity = run_identity
        self._records: dict[int, _OwnedProcessRecord] = {}
        self._accounting_complete = True

    def register(self, identity: OwnedProcessIdentity,
                 parent: OwnedProcessIdentity | None, purpose: str) -> None:
        _require_contract(identity.platform_kind == self.platform_kind,
                          "owned process platform disagrees with tree")
        _require_contract(isinstance(purpose, str) and bool(purpose),
                          "owned process purpose is invalid")
        existing = self._records.get(identity.pid)
        if existing is not None:
            if existing.identity != identity:
                self._accounting_complete = False
                raise AuditInfrastructureError("owned process start identity changed")
            raise AuditInfrastructureError("owned process identity was registered twice")
        if parent is not None:
            _require_contract(parent.pid in self._records
                              and self._records[parent.pid].identity == parent,
                              "owned process parent identity is not retained")
        self._records[identity.pid] = _OwnedProcessRecord(identity, parent, purpose)

    def observe_resident_bytes(self, identity: OwnedProcessIdentity, *,
                               current_bytes: int, peak_bytes: int) -> None:
        record = self._record(identity)
        current = _nonnegative_integer(current_bytes, "owned process current residency")
        peak = _nonnegative_integer(peak_bytes, "owned process peak residency")
        _require_contract(peak >= current, "owned process peak residency is invalid")
        record.current_resident_bytes = current
        record.peak_resident_bytes = max(record.peak_resident_bytes, peak)

    def mark_exited(self, identity: OwnedProcessIdentity) -> None:
        record = self._record(identity)
        _require_contract(not record.exited, "owned process exited twice")
        record.exited = True
        record.current_resident_bytes = 0

    def reap(self, identity: OwnedProcessIdentity) -> None:
        record = self._record(identity)
        _require_contract(record.exited and not record.reaped,
                          "owned process reap ordering is invalid")
        record.reaped = True

    def _record(self, identity: OwnedProcessIdentity) -> _OwnedProcessRecord:
        record = self._records.get(identity.pid)
        if record is None or record.identity != identity:
            self._accounting_complete = False
            raise AuditInfrastructureError("owned process start identity is not retained")
        return record

    @property
    def retained_identity_count(self) -> int:
        return len(self._records)

    @property
    def surviving_owned_process_count(self) -> int:
        return sum(not record.reaped for record in self._records.values())

    @property
    def accounting_complete(self) -> bool:
        return self._accounting_complete


def _query_job_member_identity(pid: int, native_start_identity: str) -> OwnedProcessIdentity:
    """Build the complete Job membership identity after native handle retention."""

    return OwnedProcessIdentity("windows", pid, native_start_identity)


def native_process_resident_bytes(pid: int) -> int:
    """Read a live process's native resident working set without global inference."""

    _require_contract(isinstance(pid, int) and not isinstance(pid, bool) and pid > 0,
                      "native process PID is invalid")
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
        if not handle:
            raise AuditInfrastructureError("native process residency is unavailable")
        try:
            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(
                    handle, ctypes.byref(counters), counters.cb):
                raise AuditInfrastructureError("native process residency is unavailable")
            return int(counters.WorkingSetSize)
        finally:
            kernel32.CloseHandle(handle)
    if sys.platform.startswith("linux"):
        try:
            resident_pages = int(Path(f"/proc/{pid}/statm").read_text(
                encoding="ascii").split()[1])
        except (OSError, ValueError, IndexError) as error:
            raise AuditInfrastructureError("native process residency is unavailable") from error
        return resident_pages * os.sysconf("SC_PAGE_SIZE")
    raise AuditInfrastructureError("native process residency query is unsupported")


class WindowsNativeJob:
    """Kill-on-close Job primitive used before collection on native Windows."""

    def __init__(self, name: str | None = None) -> None:
        if os.name != "nt":
            raise AuditInfrastructureError("Windows Job accounting requires Windows")
        import ctypes
        from ctypes import wintypes

        class BasicLimit(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in (
                "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class ExtendedLimit(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimit), ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        self._ctypes = ctypes
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p,
                                                    wintypes.LPCWSTR)
        self._kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        self._kernel32.CreateIoCompletionPort.argtypes = (
            wintypes.HANDLE, wintypes.HANDLE, ctypes.c_size_t, wintypes.DWORD)
        self._kernel32.CreateIoCompletionPort.restype = wintypes.HANDLE
        self._kernel32.OpenProcess.argtypes = (
            wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        self._kernel32.OpenProcess.restype = wintypes.HANDLE
        self._kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        self._kernel32.CloseHandle.restype = wintypes.BOOL
        self._kernel32.IsProcessInJob.argtypes = (
            wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)
        )
        self._kernel32.IsProcessInJob.restype = wintypes.BOOL
        self._handle = self._kernel32.CreateJobObjectW(None, name)
        if not self._handle:
            raise AuditInfrastructureError("Windows Job creation failed")
        limits = ExtendedLimit()
        limits.BasicLimitInformation.LimitFlags = 0x00002000
        if not self._kernel32.SetInformationJobObject(
                self._handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.close()
            raise AuditInfrastructureError("Windows Job kill-on-close setup failed")
        invalid = ctypes.c_void_p(-1).value
        self._completion_port = self._kernel32.CreateIoCompletionPort(
            invalid, None, 0, 1)
        if not self._completion_port:
            self.close()
            raise AuditInfrastructureError("Windows Job completion port creation failed")

        class CompletionPortAssociation(ctypes.Structure):
            _fields_ = [("CompletionKey", ctypes.c_void_p),
                        ("CompletionPort", wintypes.HANDLE)]

        association = CompletionPortAssociation(
            ctypes.c_void_p(id(self)), self._completion_port)
        if not self._kernel32.SetInformationJobObject(
                self._handle, 7, ctypes.byref(association),
                ctypes.sizeof(association)):
            self.close()
            raise AuditInfrastructureError("Windows Job completion port setup failed")

    def assign_process_handle(self, process_handle: int) -> None:
        if not self._handle or not isinstance(process_handle, int) or process_handle <= 0:
            raise AuditInfrastructureError("Windows Job process handle is invalid")
        if not self._kernel32.AssignProcessToJobObject(self._handle, process_handle):
            raise AuditInfrastructureError("Windows Job process assignment failed")

    def contains_process_handle(self, process_handle: int) -> bool:
        if not self._handle or not isinstance(process_handle, int) or process_handle <= 0:
            raise AuditInfrastructureError("Windows Job process handle is invalid")
        from ctypes import wintypes

        contained = wintypes.BOOL()
        if not self._kernel32.IsProcessInJob(
            process_handle, self._handle, self._ctypes.byref(contained)
        ):
            raise AuditInfrastructureError(
                "Windows Job exact membership query failed"
            )
        return bool(contained.value)

    def process_ids(self) -> tuple[int, ...]:
        ctypes = self._ctypes
        capacity = 16
        while capacity <= 65536:
            size = 8 + capacity * ctypes.sizeof(ctypes.c_size_t)
            buffer = ctypes.create_string_buffer(size)
            if not self._kernel32.QueryInformationJobObject(
                    self._handle, 3, buffer, size, None):
                raise AuditInfrastructureError("Windows Job membership query failed")
            assigned = int.from_bytes(buffer.raw[:4], "little")
            count = int.from_bytes(buffer.raw[4:8], "little")
            if count <= capacity:
                array_type = ctypes.c_size_t * count
                array = array_type.from_buffer_copy(buffer.raw[8:8 + count *
                                                          ctypes.sizeof(ctypes.c_size_t)])
                _require_contract(count <= assigned,
                                  "Windows Job process totals are invalid")
                return tuple(int(value) for value in array)
            capacity *= 2
        raise AuditInfrastructureError("Windows Job membership ceiling exceeded")

    def drain_notifications(self) -> tuple[tuple[int, int], ...]:
        """Drain every queued Job message; NEW_PROCESS payloads carry exact PIDs."""

        ctypes = self._ctypes
        from ctypes import wintypes
        messages: list[tuple[int, int]] = []
        while True:
            message = wintypes.DWORD()
            key = ctypes.c_size_t()
            payload = ctypes.c_void_p()
            ok = self._kernel32.GetQueuedCompletionStatus(
                self._completion_port, ctypes.byref(message), ctypes.byref(key),
                ctypes.byref(payload), 0)
            if not ok:
                error = ctypes.get_last_error()
                if error == 258:
                    break
                raise AuditInfrastructureError(
                    "Windows Job completion notification drain failed")
            messages.append((int(message.value), int(payload.value or 0)))
        return tuple(messages)

    def accounting_totals(self) -> tuple[int, int, int]:
        ctypes = self._ctypes
        from ctypes import wintypes

        class BasicAccounting(ctypes.Structure):
            _fields_ = [
                ("TotalUserTime", ctypes.c_int64),
                ("TotalKernelTime", ctypes.c_int64),
                ("ThisPeriodTotalUserTime", ctypes.c_int64),
                ("ThisPeriodTotalKernelTime", ctypes.c_int64),
                ("TotalPageFaultCount", wintypes.DWORD),
                ("TotalProcesses", wintypes.DWORD),
                ("ActiveProcesses", wintypes.DWORD),
                ("TotalTerminatedProcesses", wintypes.DWORD),
            ]

        value = BasicAccounting()
        if not self._kernel32.QueryInformationJobObject(
                self._handle, 1, ctypes.byref(value), ctypes.sizeof(value), None):
            raise AuditInfrastructureError("Windows Job accounting query failed")
        return (int(value.TotalProcesses), int(value.ActiveProcesses),
                int(value.TotalTerminatedProcesses))

    def peak_commit_charge_bytes(self) -> int:
        ctypes = self._ctypes
        from ctypes import wintypes

        class BasicLimit(ctypes.Structure):
            _fields_ = [("raw", ctypes.c_byte * 64)]

        class IoCounters(ctypes.Structure):
            _fields_ = [("raw", ctypes.c_byte * 48)]

        class ExtendedLimit(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimit), ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        value = ExtendedLimit()
        if not self._kernel32.QueryInformationJobObject(
                self._handle, 9, ctypes.byref(value), ctypes.sizeof(value), None):
            raise AuditInfrastructureError("Windows Job memory query failed")
        return int(value.PeakJobMemoryUsed)

    def open_member_handle(self, pid: int) -> int:
        handle = self._kernel32.OpenProcess(0x100000 | 0x0400 | 0x0010,
                                            False, pid)
        if not handle:
            raise AuditInfrastructureError(
                "Windows Job-reported process handle retention failed")
        return int(handle)

    def process_identity(self, process_handle: int, pid: int) -> OwnedProcessIdentity:
        ctypes = self._ctypes
        from ctypes import wintypes
        created = wintypes.FILETIME()
        exited = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not self._kernel32.GetProcessTimes(
                process_handle, ctypes.byref(created), ctypes.byref(exited),
                ctypes.byref(kernel), ctypes.byref(user)):
            raise AuditInfrastructureError("Windows process creation identity failed")
        token = (int(created.dwHighDateTime) << 32) | int(created.dwLowDateTime)
        return _query_job_member_identity(pid, str(token))

    def process_parent_pid(self, process_handle: int) -> int:
        ctypes = self._ctypes

        class ProcessBasicInformation(ctypes.Structure):
            _fields_ = [
                ("ExitStatus", ctypes.c_void_p),
                ("PebBaseAddress", ctypes.c_void_p),
                ("AffinityMask", ctypes.c_size_t),
                ("BasePriority", ctypes.c_long),
                ("UniqueProcessId", ctypes.c_size_t),
                ("InheritedFromUniqueProcessId", ctypes.c_size_t),
            ]

        ntdll = ctypes.WinDLL("ntdll")
        query = ntdll.NtQueryInformationProcess
        query.argtypes = (
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
            ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong))
        query.restype = ctypes.c_long
        information = ProcessBasicInformation()
        returned = ctypes.c_ulong()
        status = query(
            process_handle, 0, ctypes.byref(information),
            ctypes.sizeof(information), ctypes.byref(returned))
        if status < 0 or returned.value < ctypes.sizeof(information):
            raise AuditInfrastructureError(
                "Windows process parent identity query failed")
        parent_pid = int(information.InheritedFromUniqueProcessId)
        _require_contract(parent_pid > 0,
                          "Windows process parent identity is invalid")
        return parent_pid

    def process_memory(self, process_handle: int) -> tuple[int, int]:
        ctypes = self._ctypes
        from ctypes import wintypes

        class ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                *[(name, ctypes.c_size_t) for name in (
                    "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                    "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                    "PagefileUsage", "PeakPagefileUsage")],
            ]

        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        if not psapi.GetProcessMemoryInfo(
                process_handle, ctypes.byref(counters), counters.cb):
            raise AuditInfrastructureError("Windows retained process memory query failed")
        return int(counters.WorkingSetSize), int(counters.PeakWorkingSetSize)

    def close_process_handle(self, process_handle: int) -> None:
        if process_handle:
            self._kernel32.CloseHandle(process_handle)

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle:
            self._kernel32.CloseHandle(handle)
            self._handle = None
        completion_port = getattr(self, "_completion_port", None)
        if completion_port:
            self._kernel32.CloseHandle(completion_port)
            self._completion_port = None

    def __enter__(self) -> "WindowsNativeJob":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


def _aggregate_only_before_finalize(
    _worker_index: int, _generation: int
) -> None:
    return None


def _aggregate_only_finalize(
    _worker_index: int, _generation: int
) -> None:
    return None


def _windows_generation_terminal_transition(
    _kind: str,
    _stage: str,
    _position: str,
    _worker_index: int,
    _generation: int,
) -> None:
    return None


class WindowsNativeRunAccountant:
    """Retain every Job PID/start handle and seal only after stable equality."""

    def __init__(self, parent_pid: int) -> None:
        _require_contract(os.name == "nt" and isinstance(parent_pid, int)
                          and not isinstance(parent_pid, bool) and parent_pid > 0,
                          "Windows run accountant input is invalid")
        self.job = WindowsNativeJob()
        self._parent_pid = parent_pid
        self._handles: dict[int, int] = {}
        self._identities: dict[int, OwnedProcessIdentity] = {}
        self._purposes: dict[int, str] = {}
        self._parent_pids: dict[int, int] = {}
        self._peaks: dict[int, int] = {}
        self._exited: set[int] = set()
        self._finalized_peaks: set[int] = set()
        self._active_purpose: str | None = None
        self._parent_peak = 0
        self._simultaneous_peak = 0
        self._complete = True
        self._generation_jobs: dict[tuple[int, int], WindowsNativeJob] = {}
        self._archived_generation_jobs: dict[tuple[int, int], tuple[int, int]] = {}
        self._assigned_generation_jobs: set[tuple[int, int]] = set()
        self._discarded_generation_jobs: set[tuple[int, int]] = set()
        self._aggregate_only_generation_jobs: set[tuple[int, int]] = set()
        self._aggregate_only_job_close_progress: set[
            tuple[int, int]
        ] = set()
        self._generation_terminal_close_progress: set[
            tuple[str, int, int]
        ] = set()
        self._pending_generation_archives: dict[
            tuple[int, int], tuple[int, int]
        ] = {}
        self._aggregate_assignment_intents: dict[
            tuple[int, int], tuple[int, int, str, int]
        ] = {}
        self._generation_assignment_intents: dict[
            tuple[int, int], tuple[int, int, str, int]
        ] = {}
        self._aggregate_assignment_progress: set[tuple[int, int]] = set()
        self._generation_assignment_progress: set[tuple[int, int]] = set()
        self.inspection_probe_invocations = 0
        self._active_inspection_carriers: dict[object, object] = {}

    def register_compiler_process_launch(self, event, carrier) -> None:
        _require_contract(
            event.purpose is CompilerLaunchPurpose.INSPECTION
            and event.process_start.platform_kind == "windows"
            and event.process_start not in self._active_inspection_carriers,
            "Windows inspection launch registration is invalid",
        )
        process_handle = getattr(carrier, "windows_process_handle", None)
        _require_contract(
            isinstance(process_handle, int)
            and not isinstance(process_handle, bool)
            and process_handle != 0,
            "Windows inspection process handle is unavailable",
        )
        purpose = f"inspection:{self.inspection_probe_invocations}"
        self.assign_process_handle(
            process_handle, event.process_start.pid, purpose
        )
        self._active_inspection_carriers[event.process_start] = carrier
        self.inspection_probe_invocations += 1

    def complete_compiler_process_launch(self, event, carrier) -> None:
        _require_contract(
            self._active_inspection_carriers.pop(
                event.process_start, None
            ) is carrier,
            "Windows inspection process carrier differs",
        )

    def fail_compiler_process_launch(self, event, carrier) -> None:
        if self._active_inspection_carriers.get(event.process_start) is carrier:
            self._active_inspection_carriers.pop(event.process_start)

    def create_generation_job(self, worker_index: int,
                              generation: int) -> None:
        _require_contract(all(isinstance(value, int) and not isinstance(value, bool)
                              and value >= 0
                              for value in (worker_index, generation)),
                          "Windows generation Job identity is invalid")
        key = (worker_index, generation)
        _require_contract(key not in self._generation_jobs
                          and key not in self._archived_generation_jobs,
                          "Windows generation Job already exists")
        _require_contract(key not in self._discarded_generation_jobs,
                          "Windows generation Job was discarded")
        _require_contract(key not in self._aggregate_only_generation_jobs,
                          "Windows generation Job was partially finalized")
        job = WindowsNativeJob()
        if job.process_ids() or job.accounting_totals()[:2] != (0, 0):
            job.close()
            raise AuditInfrastructureError("Windows generation Job is not empty")
        self._generation_jobs[key] = job

    def assign_generation_process(self, worker_index: int, generation: int,
                                  process_handle: int, pid: int,
                                  purpose: str) -> None:
        key = (worker_index, generation)
        job = self._generation_jobs.get(key)
        _require_contract(job is not None, "Windows generation Job is unavailable")
        metadata = (process_handle, pid, purpose)

        def prepare_intent(intents, target_job):
            existing = intents.get(key)
            if existing is not None:
                _require_contract(
                    existing[:3] == metadata,
                    "Windows generation Job assignment intent differs",
                )
                return existing
            baseline_total = target_job.accounting_totals()[0]
            intent = (*metadata, baseline_total)
            intents[key] = intent
            return intent

        def effect_observed(target_job, intent) -> bool:
            del intent
            return target_job.contains_process_handle(process_handle)

        aggregate_intent = prepare_intent(
            self._aggregate_assignment_intents, self.job
        )
        if key not in self._aggregate_assignment_progress:
            if effect_observed(self.job, aggregate_intent):
                self._aggregate_assignment_progress.add(key)
            else:
                try:
                    self.job.assign_process_handle(process_handle)
                except BaseException:
                    if not effect_observed(self.job, aggregate_intent):
                        raise
                    self._aggregate_assignment_progress.add(key)
                self._aggregate_assignment_progress.add(key)
        self._retain(pid, purpose)

        generation_intent = prepare_intent(
            self._generation_assignment_intents, job
        )
        if key not in self._generation_assignment_progress:
            if effect_observed(job, generation_intent):
                self._generation_assignment_progress.add(key)
            else:
                try:
                    job.assign_process_handle(process_handle)
                except BaseException:
                    if not effect_observed(job, generation_intent):
                        raise
                    self._generation_assignment_progress.add(key)
                    self._assigned_generation_jobs.add(key)
                self._generation_assignment_progress.add(key)
            self._assigned_generation_jobs.add(key)

    def generation_job_assignment_completed(
        self, worker_index: int, generation: int
    ) -> bool:
        return self.generation_job_assignment_state(
            worker_index, generation
        ) == "generation-assigned"

    def generation_job_assignment_state(
        self, worker_index: int, generation: int
    ) -> str:
        key = (worker_index, generation)
        if key in self._archived_generation_jobs:
            return "generation-assigned"
        if key in self._aggregate_only_generation_jobs:
            return "aggregate-only"
        if key in self._discarded_generation_jobs:
            return "unassigned"
        _require_contract(
            key in self._generation_jobs,
            "Windows generation Job is unavailable",
        )
        aggregate = key in self._aggregate_assignment_progress
        generation_assigned = key in self._generation_assignment_progress
        _require_contract(
            not generation_assigned or aggregate,
            "Windows generation Job assignment progress differs",
        )
        if generation_assigned:
            return "generation-assigned"
        if aggregate:
            return "aggregate-only"
        return "unassigned"

    def finalize_aggregate_only_generation_job(
        self, worker_index: int, generation: int
    ) -> None:
        key = (worker_index, generation)
        if (
            key in self._aggregate_only_generation_jobs
            and key not in self._generation_jobs
        ):
            return
        job = self._generation_jobs.get(key)
        _require_contract(
            job is not None
            and (
                key in self._aggregate_only_generation_jobs
                or (
                    key in self._aggregate_assignment_progress
                    and key not in self._generation_assignment_progress
                )
            ),
            "Windows aggregate-only generation Job is invalid",
        )
        progress = self._terminal_close_progress()
        token = ("aggregate-only", *key)
        if token not in progress:
            _aggregate_only_before_finalize(worker_index, generation)
            job.drain_notifications()
            total, active, _terminated = job.accounting_totals()
            _require_contract(
                total == 0 and active == 0 and not job.process_ids(),
                "Windows aggregate-only generation Job is not empty",
            )
        self._complete_generation_terminal_transition(
            "aggregate-only",
            key,
            job.close,
            lambda: key in self._aggregate_only_generation_jobs,
            lambda: self._aggregate_only_generation_jobs.add(key),
            lambda: _aggregate_only_finalize(worker_index, generation),
        )
        self._aggregate_only_job_close_progress.add(key)

    def discard_generation_job(self, worker_index: int,
                               generation: int) -> None:
        key = (worker_index, generation)
        if (
            key in self._discarded_generation_jobs
            and key not in self._generation_jobs
        ):
            return
        job = self._generation_jobs.get(key)
        _require_contract(
            job is not None
            and key not in self._aggregate_assignment_progress
            and key not in self._generation_assignment_progress
            and key not in self._archived_generation_jobs,
            "Windows generation Job discard is invalid",
        )
        progress = self._terminal_close_progress()
        if ("discard", *key) not in progress:
            job.drain_notifications()
            total, active, _terminated = job.accounting_totals()
            _require_contract(
                total == 0 and active == 0 and not job.process_ids(),
                "Windows unassigned generation Job is not empty",
            )
        self._complete_generation_terminal_transition(
            "discard",
            key,
            job.close,
            lambda: key in self._discarded_generation_jobs,
            lambda: self._discarded_generation_jobs.add(key),
        )

    def archive_generation_job(self, worker_index: int, generation: int,
                               deadline: float) -> tuple[int, int]:
        key = (worker_index, generation)
        job = self._generation_jobs.get(key)
        archived = self._archived_generation_jobs.get(key)
        if archived is not None and key not in self._generation_jobs:
            self._finish_archived_generation_progress(key)
            return archived
        _require_contract(job is not None
                          and key in self._generation_assignment_progress
                          and isinstance(deadline, (int, float))
                          and not isinstance(deadline, bool),
                          "Windows generation Job archive is invalid")
        pending = self._pending_archives()
        archive = archived or pending.get(key)
        if archive is None:
            stable = False
            while time.monotonic() < deadline:
                job.drain_notifications()
                total, active, _terminated = job.accounting_totals()
                empty = not job.process_ids()
                if active == 0 and empty:
                    if stable:
                        archive = (total, job.peak_commit_charge_bytes())
                        pending[key] = archive
                        break
                    stable = True
                else:
                    stable = False
                time.sleep(0.005)
        if archive is None:
            raise AuditInfrastructureError(
                "Windows generation Job did not become empty"
            )
        self._complete_generation_terminal_transition(
            "archive",
            key,
            job.close,
            lambda: key in self._archived_generation_jobs,
            lambda: self._archived_generation_jobs.__setitem__(key, archive),
        )
        self._finish_archived_generation_progress(key)
        return archive

    def _terminal_close_progress(self) -> set[tuple[str, int, int]]:
        progress = getattr(self, "_generation_terminal_close_progress", None)
        if progress is None:
            progress = set()
            self._generation_terminal_close_progress = progress
        return progress

    def _pending_archives(self) -> dict[tuple[int, int], tuple[int, int]]:
        pending = getattr(self, "_pending_generation_archives", None)
        if pending is None:
            pending = {}
            self._pending_generation_archives = pending
        return pending

    def _complete_generation_terminal_transition(
        self,
        kind: str,
        key: tuple[int, int],
        close_action,
        is_published,
        publish_action,
        after_close=None,
    ) -> None:
        worker_index, generation = key
        progress = self._terminal_close_progress()
        token = (kind, worker_index, generation)
        if token not in progress:
            _windows_generation_terminal_transition(
                kind, "close", "before", worker_index, generation
            )
            close_action()
            progress.add(token)
            _windows_generation_terminal_transition(
                kind, "close", "after", worker_index, generation
            )
        if callable(after_close):
            after_close()
        if not is_published():
            _windows_generation_terminal_transition(
                kind, "terminal", "before", worker_index, generation
            )
            publish_action()
            _windows_generation_terminal_transition(
                kind, "terminal", "after", worker_index, generation
            )
        if key in self._generation_jobs:
            _windows_generation_terminal_transition(
                kind, "remove", "before", worker_index, generation
            )
            del self._generation_jobs[key]
            _windows_generation_terminal_transition(
                kind, "remove", "after", worker_index, generation
            )
        _require_contract(
            is_published() and key not in self._generation_jobs,
            "Windows generation Job terminal transition is incomplete",
        )

    def _finish_archived_generation_progress(
        self, key: tuple[int, int]
    ) -> None:
        self._assigned_generation_jobs.discard(key)
        self._aggregate_assignment_progress.discard(key)
        self._generation_assignment_progress.discard(key)
        self._pending_archives().pop(key, None)

    def begin_process_scope(self, purpose: str) -> None:
        _require_contract(isinstance(purpose, str) and bool(purpose)
                          and self._active_purpose is None,
                          "Windows Job process scope is invalid")
        self._active_purpose = purpose

    def end_process_scope(self) -> None:
        _require_contract(self._active_purpose is not None,
                          "Windows Job process scope is not active")
        self._active_purpose = None

    def assign_process_handle(self, process_handle: int, pid: int,
                              purpose: str) -> None:
        _require_contract(isinstance(purpose, str) and bool(purpose),
                          "Windows Job process purpose is invalid")
        self.job.assign_process_handle(process_handle)
        self._retain(pid, purpose)

    def _retain(self, pid: int, purpose: str | None = None) -> None:
        if pid in self._handles:
            current = self.job.process_identity(self._handles[pid], pid)
            if current != self._identities[pid]:
                self._complete = False
                raise AuditInfrastructureError("Windows Job PID creation identity changed")
            return
        handle = self.job.open_member_handle(pid)
        try:
            identity = self.job.process_identity(handle, pid)
            parent_pid = self.job.process_parent_pid(handle)
        except BaseException:
            self.job.close_process_handle(handle)
            self._complete = False
            raise
        selected_purpose = purpose
        if selected_purpose is None:
            ancestor = parent_pid
            visited = {pid}
            while ancestor not in self._purposes:
                if ancestor in visited or ancestor not in self._parent_pids:
                    self.job.close_process_handle(handle)
                    self._complete = False
                    raise AuditInfrastructureError(
                        "Windows Job descendant ancestry is unavailable")
                visited.add(ancestor)
                ancestor = self._parent_pids[ancestor]
            selected_purpose = self._purposes[ancestor]
        self._handles[pid] = handle
        self._identities[pid] = identity
        self._purposes[pid] = selected_purpose
        self._parent_pids[pid] = parent_pid
        self._peaks[pid] = 0

    def observe(self, parent_current_resident_bytes: int) -> tuple[int, int]:
        parent = _nonnegative_integer(parent_current_resident_bytes,
                                      "Windows parent current residency")
        notifications = self.job.drain_notifications()
        for message, pid in notifications:
            if message == 6:
                self._retain(pid)
            elif message in {7, 8}:
                self._exited.add(pid)
        for pid in self.job.process_ids():
            self._retain(pid)
        total, active, _terminated = self.job.accounting_totals()
        if total < len(self._handles):
            self._complete = False
            raise AuditInfrastructureError("incomplete Job process accounting")
        current_sum = parent
        for pid, handle in self._handles.items():
            if pid in self._finalized_peaks:
                continue
            try:
                current, peak = self.job.process_memory(handle)
            except AuditInfrastructureError:
                self._complete = False
                raise AuditInfrastructureError(
                    "Windows retained process peak query failed")
            self._peaks[pid] = max(self._peaks[pid], peak)
            current_sum += current
            if pid in self._exited:
                self._finalized_peaks.add(pid)
        self._parent_peak = max(self._parent_peak, parent)
        self._simultaneous_peak = max(self._simultaneous_peak, current_sum)
        return total, active

    def seal_phase(self, deadline: float,
                   parent_current_resident_bytes: int = 0) -> None:
        _require_contract(isinstance(deadline, (int, float))
                          and not isinstance(deadline, bool),
                          "Windows Job seal deadline is invalid")
        _require_contract(
            not self._active_inspection_carriers,
            "Windows inspection process carriers remain active",
        )
        stable: tuple[int, int] | None = None
        while time.monotonic() < deadline:
            totals = self.observe(parent_current_resident_bytes)
            pending = self.job.drain_notifications()
            if (totals[1] == 0 and totals[0] == len(self._handles)
                    and not pending and stable == totals):
                return
            stable = totals if totals[1] == 0 and not pending else None
            time.sleep(0.005)
        self._complete = False
        raise AuditInfrastructureError("incomplete Job process accounting at phase seal")

    def snapshot(self) -> "WindowsJobAccountingSnapshot":
        total, active, _terminated = self.job.accounting_totals()
        if total != len(self._handles):
            self._complete = False
            raise AuditInfrastructureError("incomplete Job process accounting")
        inspection: dict[str, int] = {}
        retained_inspection = 0
        worker_roots: dict[tuple[int, int], int] = {}
        compiler_invocations: dict[tuple[int, int, int], int] = {}
        generation_process_counts: dict[tuple[int, int], int] = {}
        for pid, purpose in self._purposes.items():
            if purpose.startswith("inspection:"):
                retained_inspection += 1
                inspection[purpose] = inspection.get(purpose, 0) + self._peaks[pid]
                continue
            worker_match = re.fullmatch(r"worker:(\d+):(\d+)", purpose)
            compiler_match = re.fullmatch(
                r"compiler:(\d+):(\d+):(\d+)", purpose)
            if worker_match is not None:
                key = tuple(map(int, worker_match.groups()))
                worker_roots[key] = worker_roots.get(key, 0) + self._peaks[pid]
                generation_process_counts[key] = (
                    generation_process_counts.get(key, 0) + 1)
                continue
            if compiler_match is not None:
                slot, generation, invocation = map(int, compiler_match.groups())
                key = (slot, generation)
                invocation_key = (slot, generation, invocation)
                compiler_invocations[invocation_key] = (
                    compiler_invocations.get(invocation_key, 0)
                    + self._peaks[pid])
                generation_process_counts[key] = (
                    generation_process_counts.get(key, 0) + 1)
                continue
            self._complete = False
            raise AuditInfrastructureError(
                "Windows retained process purpose is invalid")
        if self._generation_jobs:
            self._complete = False
            raise AuditInfrastructureError(
                "Windows generation Job is not archived")
        aggregate_only = getattr(
            self, "_aggregate_only_generation_jobs", set()
        )
        archived_generation_keys = (
            set(self._archived_generation_jobs) | set(aggregate_only)
        )
        if (set(worker_roots) != set(generation_process_counts)
                or archived_generation_keys != set(worker_roots)
                or set(self._archived_generation_jobs) & set(aggregate_only)):
            self._complete = False
            raise AuditInfrastructureError(
                "Windows generation accounting is incomplete")
        for key, process_count in generation_process_counts.items():
            if (
                key in self._archived_generation_jobs
                and self._archived_generation_jobs[key][0] != process_count
            ):
                self._complete = False
                raise AuditInfrastructureError(
                    "Windows generation process accounting differs")
        slots: list[WindowsSlotPeak] = []
        for slot in sorted({key[0] for key in worker_roots}):
            generations: list[WindowsGenerationPeak] = []
            for key in sorted((key for key in worker_roots if key[0] == slot),
                              key=lambda item: item[1]):
                invocations = tuple(WindowsInvocationPeak(value) for _invocation, value
                                    in sorted((invocation_key[2], value)
                                              for invocation_key, value
                                              in compiler_invocations.items()
                                              if invocation_key[:2] == key))
                generations.append(WindowsGenerationPeak(
                    worker_roots[key], invocations))
            slots.append(WindowsSlotPeak(tuple(generations)))
        return WindowsJobAccountingSnapshot(
            parent_peak_rss_bytes=self._parent_peak,
            maximum_simultaneous_working_set_bytes=self._simultaneous_peak,
            inspection_tree_peak_bytes=tuple(inspection.values()),
            slots=tuple(slots),
            job_peak_commit_charge_bytes=self.job.peak_commit_charge_bytes(),
            job_total_process_count=total,
            retained_process_identity_count=len(self._handles),
            retained_inspection_process_identity_count=retained_inspection,
            surviving_job_process_count=active,
            archived_generation_identities=tuple(
                sorted(archived_generation_keys)
            ),
            accounting_complete=self._complete,
        )

    def close(self) -> None:
        for job in self._generation_jobs.values():
            job.close()
        self._generation_jobs.clear()
        for handle in self._handles.values():
            self.job.close_process_handle(handle)
        self._handles.clear()
        self.job.close()

    def __enter__(self) -> "WindowsNativeRunAccountant":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


def verify_windows_nested_job_support() -> None:
    """Prove aggregate→generation nesting and reject every breakaway mode."""

    if os.name != "nt":
        raise AuditInfrastructureError("Windows nested Job probe requires Windows")
    process = subprocess.Popen((
        sys.executable, "-c", "import time; time.sleep(30)"))
    aggregate = WindowsNativeJob()
    generation = WindowsNativeJob()
    try:
        aggregate.assign_process_handle(int(process._handle))
        generation.assign_process_handle(int(process._handle))
        _require_contract(process.pid in aggregate.process_ids()
                          and process.pid in generation.process_ids(),
                          "Windows nested Job membership probe failed")
    except BaseException as error:
        raise AuditInfrastructureError(
            "Windows nested Job support is unavailable or breakaway was required") from error
    finally:
        generation.close()
        aggregate.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


@dataclass(frozen=True, slots=True)
class WindowsInvocationPeak:
    compiler_descendant_peak_tree_bytes: int

    def __post_init__(self) -> None:
        _nonnegative_integer(self.compiler_descendant_peak_tree_bytes,
                             "compiler descendant peak")


@dataclass(frozen=True, slots=True)
class WindowsGenerationPeak:
    worker_root_peak_working_set_bytes: int
    invocations: tuple[WindowsInvocationPeak, ...]

    def __post_init__(self) -> None:
        _nonnegative_integer(self.worker_root_peak_working_set_bytes,
                             "worker root peak")
        _require_contract(isinstance(self.invocations, tuple)
                          and all(isinstance(item, WindowsInvocationPeak)
                                  for item in self.invocations),
                          "Windows invocation peaks are invalid")

    @property
    def compiler_upper_bound_bytes(self) -> int:
        return max((item.compiler_descendant_peak_tree_bytes
                    for item in self.invocations), default=0)

    @property
    def upper_bound_bytes(self) -> int:
        return self.worker_root_peak_working_set_bytes + self.compiler_upper_bound_bytes


@dataclass(frozen=True, slots=True)
class WindowsSlotPeak:
    generations: tuple[WindowsGenerationPeak, ...]

    def __post_init__(self) -> None:
        _require_contract(isinstance(self.generations, tuple)
                          and all(isinstance(item, WindowsGenerationPeak)
                                  for item in self.generations),
                          "Windows generation peaks are invalid")

    @property
    def maximum_generation_upper_bound_bytes(self) -> int:
        return max((item.upper_bound_bytes for item in self.generations), default=0)


@dataclass(frozen=True, slots=True)
class WindowsJobAccountingSnapshot:
    parent_peak_rss_bytes: int
    maximum_simultaneous_working_set_bytes: int
    inspection_tree_peak_bytes: tuple[int, ...]
    slots: tuple[WindowsSlotPeak, ...]
    job_peak_commit_charge_bytes: int
    job_total_process_count: int
    retained_process_identity_count: int
    retained_inspection_process_identity_count: int
    surviving_job_process_count: int
    archived_generation_identities: tuple[tuple[int, int], ...] = ()
    accounting_complete: bool = True

    def __post_init__(self) -> None:
        for label in (
            "parent_peak_rss_bytes", "maximum_simultaneous_working_set_bytes",
            "job_peak_commit_charge_bytes", "job_total_process_count",
            "retained_process_identity_count",
            "retained_inspection_process_identity_count",
            "surviving_job_process_count",
        ):
            _nonnegative_integer(getattr(self, label), label)
        _require_contract(isinstance(self.inspection_tree_peak_bytes, tuple)
                          and all(isinstance(value, int) and not isinstance(value, bool)
                                  and value >= 0
                                  for value in self.inspection_tree_peak_bytes),
                          "inspection tree peaks are invalid")
        _require_contract(isinstance(self.slots, tuple)
                          and all(isinstance(slot, WindowsSlotPeak)
                                  for slot in self.slots),
                          "Windows slot peaks are invalid")
        _require_contract(
            isinstance(self.archived_generation_identities, tuple)
            and self.archived_generation_identities
            == tuple(sorted(set(self.archived_generation_identities)))
            and all(
                isinstance(identity, tuple)
                and len(identity) == 2
                and all(
                    isinstance(value, int)
                    and not isinstance(value, bool)
                    and value >= 0
                    for value in identity
                )
                for identity in self.archived_generation_identities
            )
            and len(self.archived_generation_identities)
            == sum(len(slot.generations) for slot in self.slots),
            "Windows archived generation identities are invalid",
        )
        _require_contract(
            isinstance(self.accounting_complete, bool),
            "Windows accounting completeness is invalid",
        )

    def memory_measurements(self) -> WindowsRunMemoryMeasurements:
        if self.job_total_process_count != self.retained_process_identity_count:
            raise AuditInfrastructureError("incomplete Job process accounting")
        inspection_peak = max(self.inspection_tree_peak_bytes, default=0)
        worker_peak = sum(slot.maximum_generation_upper_bound_bytes
                          for slot in self.slots)
        upper_bound = max(
            self.maximum_simultaneous_working_set_bytes,
            self.parent_peak_rss_bytes + inspection_peak,
            self.parent_peak_rss_bytes + worker_peak,
        )
        return WindowsRunMemoryMeasurements(
            parent_peak_rss_bytes=self.parent_peak_rss_bytes,
            worker_peak_rss_bytes=worker_peak,
            maximum_simultaneous_working_set_bytes=(
                self.maximum_simultaneous_working_set_bytes),
            aggregate_peak_rss_upper_bound_bytes=upper_bound,
            job_peak_commit_charge_bytes=self.job_peak_commit_charge_bytes,
            job_total_process_count=self.job_total_process_count,
            retained_process_identity_count=self.retained_process_identity_count,
            inspection_peak_tree_bytes=inspection_peak,
            retained_inspection_process_identity_count=(
                self.retained_inspection_process_identity_count),
            surviving_job_process_count=self.surviving_job_process_count,
            accounting_complete=self.accounting_complete,
        )


def linux_systemd_run_prefix(*, unit: str, uid: int, gid: int,
                             working_directory: Path) -> tuple[str, ...]:
    _require_contract(isinstance(unit, str) and bool(unit)
                      and all(character.isalnum() or character in "-_."
                              for character in unit),
                      "systemd unit identity is invalid")
    _nonnegative_integer(uid, "systemd uid")
    _nonnegative_integer(gid, "systemd gid")
    _require_contract(isinstance(working_directory, Path)
                      and (working_directory.is_absolute()
                           or working_directory.as_posix().startswith("/")),
                      "systemd working directory is invalid")
    return (
        "sudo", "systemd-run", f"--unit={unit}", "--wait", "--collect", "--pipe",
        "--property=Type=exec", "--property=Delegate=yes",
        "--property=MemoryAccounting=yes",
        f"--property=MemoryMax={LINUX_MEMORY_MAX_BYTES}",
        f"--property=MemoryHigh={LINUX_MEMORY_HIGH_BYTES}",
        f"--uid={uid}", f"--gid={gid}",
        f"--working-directory={working_directory.as_posix()}", "python3",
        "tests/gpu/gpu_capability_process_tree.py", "prepare-linux-cgroup",
    )


_LINUX_FRAME_KEYS = frozenset({
    "kind", "run_id", "worker_index", "generation", "worker_pid", "nonce",
    "sequence"
})
_LINUX_FRAME_KINDS = frozenset({
    "hello", "create-leaf", "ack-leaf", "release-leaf"})
_LINUX_ID_PATTERN = re.compile(r"[A-Za-z0-9._-]{1,128}")


def _validate_linux_rendezvous_fields(fields: object) -> Mapping[str, object]:
    _require_contract(isinstance(fields, Mapping)
                      and set(fields) == _LINUX_FRAME_KEYS,
                      "Linux rendezvous frame fields are invalid")
    _require_contract(fields["kind"] in _LINUX_FRAME_KINDS,
                      "Linux rendezvous frame fields are invalid")
    for name in ("run_id", "nonce"):
        value = fields[name]
        _require_contract(isinstance(value, str)
                          and _LINUX_ID_PATTERN.fullmatch(value) is not None,
                          "Linux rendezvous frame fields are invalid")
    for name, minimum in (("worker_index", 0), ("generation", 0),
                          ("worker_pid", 0), ("sequence", 1)):
        value = fields[name]
        _require_contract(isinstance(value, int) and not isinstance(value, bool)
                          and minimum <= value <= (1 << 31) - 1,
                          "Linux rendezvous frame fields are invalid")
    _require_contract((fields["kind"] == "ack-leaf" and fields["worker_pid"] > 0)
                      or (fields["kind"] != "ack-leaf"
                          and fields["worker_pid"] == 0),
                      "Linux rendezvous worker identity is invalid")
    return fields


def _canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def linux_rendezvous_run_id(token: bytes) -> str:
    _require_contract(isinstance(token, bytes) and len(token) == 32,
                      "Linux rendezvous token is invalid")
    return hmac.new(token, LINUX_RENDEZVOUS_SCHEMA_BYTES + b":run-id",
                    hashlib.sha256).hexdigest()


def encode_linux_rendezvous_frame(fields: Mapping[str, object], token: bytes) -> bytes:
    _validate_linux_rendezvous_fields(fields)
    _require_contract(isinstance(token, bytes) and len(token) == 32,
                      "Linux rendezvous token is invalid")
    body = _canonical_json(dict(fields))
    _require_contract(len(body) <= LINUX_RENDEZVOUS_MAX_BYTES,
                      "Linux rendezvous frame exceeds bound")
    authentication = hmac.new(token, LINUX_RENDEZVOUS_SCHEMA_BYTES + body,
                              hashlib.sha256).hexdigest()
    payload = _canonical_json({"body": body.decode("ascii"), "hmac": authentication,
                               "schema": LINUX_RENDEZVOUS_SCHEMA_BYTES.decode("ascii")})
    _require_contract(len(payload) <= LINUX_RENDEZVOUS_MAX_BYTES,
                      "Linux rendezvous frame exceeds bound")
    return payload


def decode_linux_rendezvous_frame(payload: bytes, token: bytes) -> dict[str, object]:
    try:
        _require_contract(isinstance(payload, bytes)
                          and 0 < len(payload) <= LINUX_RENDEZVOUS_MAX_BYTES,
                          "Linux rendezvous payload is invalid")
        _require_contract(isinstance(token, bytes) and len(token) == 32,
                          "Linux rendezvous token is invalid")
        envelope = json.loads(payload)
        _require_contract(isinstance(envelope, dict)
                          and set(envelope) == {"body", "hmac", "schema"}
                          and isinstance(envelope["body"], str)
                          and isinstance(envelope["hmac"], str)
                          and envelope["schema"] == LINUX_RENDEZVOUS_SCHEMA_BYTES.decode("ascii")
                          and _canonical_json(envelope) == payload,
                          "Linux rendezvous envelope is invalid")
        body = envelope["body"].encode("ascii")
        expected = hmac.new(token, LINUX_RENDEZVOUS_SCHEMA_BYTES + body,
                            hashlib.sha256).hexdigest()
        _require_contract(isinstance(envelope["hmac"], str)
                          and hmac.compare_digest(envelope["hmac"], expected),
                          "Linux rendezvous authentication failed")
        fields = json.loads(body)
        _require_contract(isinstance(fields, dict)
                          and set(fields) == _LINUX_FRAME_KEYS
                          and _canonical_json(fields) == body,
                          "Linux rendezvous body is invalid")
        _validate_linux_rendezvous_fields(fields)
        return fields
    except (AuditInfrastructureError, UnicodeError, ValueError, TypeError) as error:
        if isinstance(error, AuditInfrastructureError):
            raise
        raise AuditInfrastructureError("Linux rendezvous frame is invalid") from error


class LinuxRendezvousSession:
    """Stateful authority checked after native SO_PEERCRED extraction."""

    def __init__(self, *, token: bytes, run_id: str, coordinator_pid: int,
                 coordinator_uid: int, coordinator_leaf: str) -> None:
        _require_contract(isinstance(token, bytes) and len(token) == 32,
                          "Linux rendezvous token is invalid")
        _require_contract(isinstance(run_id, str)
                          and _LINUX_ID_PATTERN.fullmatch(run_id) is not None,
                          "Linux rendezvous run identity is invalid")
        _require_contract(isinstance(coordinator_pid, int)
                          and not isinstance(coordinator_pid, bool)
                          and coordinator_pid > 0,
                          "Linux rendezvous coordinator PID is invalid")
        _nonnegative_integer(coordinator_uid, "Linux rendezvous coordinator UID")
        _require_contract(isinstance(coordinator_leaf, str)
                          and coordinator_leaf.startswith("/")
                          and "\x00" not in coordinator_leaf,
                          "Linux rendezvous coordinator leaf is invalid")
        self._token = token
        self._run_id = run_id
        self._coordinator_pid = coordinator_pid
        self._coordinator_uid = coordinator_uid
        self._coordinator_leaf = coordinator_leaf
        self._last_sequence = 0
        self._nonces: set[str] = set()

    def accept(self, payload: bytes, *, peer_pid: int, peer_uid: int,
               peer_cgroup: str) -> dict[str, object]:
        if (not isinstance(peer_pid, int) or isinstance(peer_pid, bool)
                or peer_pid <= 0 or peer_uid != self._coordinator_uid):
            raise AuditInfrastructureError("Linux rendezvous peer credential mismatch")
        if peer_cgroup != self._coordinator_leaf:
            raise AuditInfrastructureError("Linux rendezvous coordinator membership mismatch")
        fields = decode_linux_rendezvous_frame(payload, self._token)
        if fields["run_id"] != self._run_id:
            raise AuditInfrastructureError("Linux rendezvous run identity mismatch")
        sequence = fields["sequence"]
        nonce = fields["nonce"]
        if sequence != self._last_sequence + 1 or nonce in self._nonces:
            raise AuditInfrastructureError("Linux rendezvous replay or sequence mismatch")
        self._last_sequence = sequence
        self._nonces.add(nonce)
        return fields


_LINUX_REPLY_KEYS = frozenset({
    "kind", "run_id", "worker_index", "generation", "worker_pid", "nonce",
    "sequence",
    "leaf_path", "leaf_token",
})
_LINUX_REPLY_KINDS = frozenset({
    "hello-accepted", "leaf-created", "leaf-acknowledged", "leaf-released"})


def encode_linux_rendezvous_reply(fields: Mapping[str, object], token: bytes) -> bytes:
    _require_contract(isinstance(fields, Mapping) and set(fields) == _LINUX_REPLY_KEYS,
                      "Linux rendezvous reply fields are invalid")
    _require_contract(fields["kind"] in _LINUX_REPLY_KINDS,
                      "Linux rendezvous reply fields are invalid")
    request_projection = {name: fields[name] for name in _LINUX_FRAME_KEYS}
    request_projection["kind"] = {
        "hello-accepted": "hello", "leaf-created": "create-leaf",
        "leaf-acknowledged": "ack-leaf", "leaf-released": "release-leaf",
    }[fields["kind"]]
    _validate_linux_rendezvous_fields(request_projection)
    for name in ("leaf_path", "leaf_token"):
        value = fields[name]
        _require_contract(isinstance(value, str) and len(value) <= 4096
                          and "\x00" not in value,
                          "Linux rendezvous reply fields are invalid")
    if fields["kind"] == "leaf-created":
        _require_contract(fields["leaf_path"].startswith("/")
                          and re.fullmatch(r"[0-9a-f]{64}",
                                           fields["leaf_token"]) is not None,
                          "Linux rendezvous reply fields are invalid")
    else:
        _require_contract(fields["leaf_path"] == ""
                          and fields["leaf_token"] == "",
                          "Linux rendezvous reply fields are invalid")
    body = _canonical_json(dict(fields))
    authentication = hmac.new(
        token, LINUX_RENDEZVOUS_SCHEMA_BYTES + b":reply:" + body,
        hashlib.sha256).hexdigest()
    payload = _canonical_json({
        "body": body.decode("ascii"), "hmac": authentication,
        "schema": LINUX_RENDEZVOUS_SCHEMA_BYTES.decode("ascii")})
    _require_contract(len(payload) <= LINUX_RENDEZVOUS_MAX_BYTES,
                      "Linux rendezvous reply exceeds bound")
    return payload


def decode_linux_rendezvous_reply(payload: bytes, token: bytes) -> dict[str, object]:
    try:
        _require_contract(isinstance(payload, bytes)
                          and 0 < len(payload) <= LINUX_RENDEZVOUS_MAX_BYTES
                          and isinstance(token, bytes) and len(token) == 32,
                          "Linux rendezvous reply is invalid")
        envelope = json.loads(payload)
        _require_contract(isinstance(envelope, dict)
                          and set(envelope) == {"body", "hmac", "schema"}
                          and isinstance(envelope["body"], str)
                          and isinstance(envelope["hmac"], str)
                          and envelope["schema"]
                          == LINUX_RENDEZVOUS_SCHEMA_BYTES.decode("ascii")
                          and _canonical_json(envelope) == payload,
                          "Linux rendezvous reply is invalid")
        body = envelope["body"].encode("ascii")
        expected = hmac.new(
            token, LINUX_RENDEZVOUS_SCHEMA_BYTES + b":reply:" + body,
            hashlib.sha256).hexdigest()
        _require_contract(isinstance(envelope["hmac"], str)
                          and hmac.compare_digest(envelope["hmac"], expected),
                          "Linux rendezvous reply authentication failed")
        fields = json.loads(body)
        _require_contract(isinstance(fields, dict)
                          and _canonical_json(fields) == body,
                          "Linux rendezvous reply is invalid")
        encode_linux_rendezvous_reply(fields, token)
        return fields
    except (AuditInfrastructureError, UnicodeError, ValueError, TypeError) as error:
        if isinstance(error, AuditInfrastructureError):
            raise
        raise AuditInfrastructureError("Linux rendezvous reply is invalid") from error


def _read_cgroup_events(path: Path) -> dict[str, int]:
    try:
        lines = path.read_text(encoding="ascii").splitlines()
        values = {name: int(value) for name, value in
                  (line.split() for line in lines)}
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError("Linux cgroup events are unavailable") from error
    for required in ("oom", "oom_kill", "max"):
        _require_contract(required in values and values[required] >= 0,
                          "Linux cgroup events are incomplete")
    return values


def _read_cgroup_integer(path: Path) -> int:
    try:
        value = path.read_text(encoding="ascii").strip()
        return _nonnegative_integer(int(value), f"Linux cgroup {path.name}")
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError(
            f"Linux cgroup {path.name} is unavailable") from error


def _cgroup_pids(path: Path) -> tuple[int, ...]:
    try:
        values = tuple(int(line) for line in path.read_text(
            encoding="ascii").splitlines() if line.strip())
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError("Linux cgroup membership is unavailable") from error
    _require_contract(all(value > 0 for value in values),
                      "Linux cgroup membership is invalid")
    return values


class LinuxDelegatedCgroupSupervisor:
    """Own a delegated cgroup-v2 service while remaining outside run/."""

    def __init__(self, service_root: Path) -> None:
        _require_contract(isinstance(service_root, Path) and service_root.is_absolute(),
                          "Linux delegated service root is invalid")
        self.service_root = service_root
        self.supervisor = service_root / "supervisor"
        self.run = service_root / "run"
        self.coordinator = self.run / "coordinator"
        self._baseline_service: dict[str, int] | None = None
        self._baseline_run: dict[str, int] | None = None
        self._worker_leaves: dict[tuple[int, int], tuple[Path, str]] = {}
        self._prepared = False

    @staticmethod
    def _write(path: Path, value: str) -> None:
        try:
            path.write_text(value, encoding="ascii")
        except OSError as error:
            raise AuditInfrastructureError(
                f"Linux cgroup write failed: {path.name}") from error

    def prepare(self) -> None:
        _require_contract(not self._prepared, "Linux cgroup supervisor prepared twice")
        try:
            controllers = (self.service_root / "cgroup.controllers").read_text(
                encoding="ascii").split()
        except OSError as error:
            raise AuditInfrastructureError("delegated cgroup v2 is unavailable") from error
        _require_contract("memory" in controllers,
                          "delegated cgroup memory controller is unavailable")
        _require_contract(not self.supervisor.exists() and not self.run.exists(),
                          "delegated cgroup contains stale owned state")
        try:
            self.supervisor.mkdir()
            self.run.mkdir()
        except OSError as error:
            raise AuditInfrastructureError("delegated cgroup creation failed") from error
        self._write(self.supervisor / "cgroup.procs", str(os.getpid()))
        _require_contract(os.getpid() not in _cgroup_pids(
            self.service_root / "cgroup.procs"),
            "cgroup supervisor remained in delegated service root")
        self._write(self.service_root / "cgroup.subtree_control", "+memory")
        try:
            enabled = (self.service_root / "cgroup.subtree_control").read_text(
                encoding="ascii").replace("+", "").split()
        except OSError as error:
            raise AuditInfrastructureError("cgroup memory controller enable failed") from error
        _require_contract("memory" in enabled,
                          "cgroup memory controller enable failed")
        self._write(self.run / "memory.max", str(LINUX_MEMORY_MAX_BYTES))
        self._write(self.run / "memory.high", str(LINUX_MEMORY_HIGH_BYTES))
        self._write(self.run / "cgroup.subtree_control", "+memory")
        _require_contract(_read_cgroup_integer(self.run / "memory.max")
                          == LINUX_MEMORY_MAX_BYTES
                          and _read_cgroup_integer(self.run / "memory.high")
                          == LINUX_MEMORY_HIGH_BYTES,
                          "Linux cgroup effective memory limits differ")
        self._baseline_service = _read_cgroup_events(
            self.service_root / "memory.events")
        self._baseline_run = _read_cgroup_events(self.run / "memory.events")
        self.coordinator.mkdir()
        self._prepared = True

    def move_coordinator(self, pid: int) -> None:
        _require_contract(self._prepared and isinstance(pid, int)
                          and not isinstance(pid, bool) and pid > 0,
                          "Linux coordinator membership is invalid")
        self._write(self.coordinator / "cgroup.procs", str(pid))

    def create_worker_leaf(self, worker_index: int, generation: int,
                           nonce: str) -> tuple[Path, str]:
        _require_contract(self._prepared
                          and isinstance(worker_index, int)
                          and not isinstance(worker_index, bool)
                          and worker_index >= 0
                          and isinstance(generation, int)
                          and not isinstance(generation, bool)
                          and generation >= 0
                          and isinstance(nonce, str)
                          and _LINUX_ID_PATTERN.fullmatch(nonce) is not None,
                          "Linux worker leaf request is invalid")
        key = (worker_index, generation)
        _require_contract(key not in self._worker_leaves,
                          "Linux worker leaf was created twice")
        leaf = self.run / f"worker-{worker_index}-g{generation}"
        try:
            leaf.mkdir()
        except OSError as error:
            raise AuditInfrastructureError("Linux worker leaf creation failed") from error
        token = secrets.token_hex(32)
        self._worker_leaves[key] = (leaf, token)
        return leaf, token

    def acknowledge_worker_membership(self, worker_index: int, generation: int,
                                      token: str, pid: int) -> None:
        current = self._worker_leaves.get((worker_index, generation))
        _require_contract(current is not None and hmac.compare_digest(current[1], token),
                          "Linux worker containment token mismatch")
        self._write(current[0] / "cgroup.procs", str(pid))
        _require_contract(pid in _cgroup_pids(current[0] / "cgroup.procs"),
                          "Linux worker containment acknowledgement failed")

    def verify_worker_membership(self, worker_index: int, generation: int,
                                 token: str, worker_pid: int) -> None:
        current = self._worker_leaves.get((worker_index, generation))
        _require_contract(current is not None
                          and isinstance(token, str)
                          and hmac.compare_digest(current[1], token),
                          "Linux worker containment token mismatch")
        _require_contract(isinstance(worker_pid, int)
                          and not isinstance(worker_pid, bool)
                          and worker_pid > 0
                          and _cgroup_pids(current[0] / "cgroup.procs")
                          == (worker_pid,),
                          "Linux worker containment acknowledgement failed")

    def release_worker_leaf(self, worker_index: int, generation: int) -> None:
        current = self._worker_leaves.get((worker_index, generation))
        if current is None:
            return
        leaf, _token = current
        _require_contract(not _cgroup_pids(leaf / "cgroup.procs"),
                          "Linux worker leaf is occupied")
        try:
            leaf.rmdir()
        except OSError as error:
            raise AuditInfrastructureError("Linux worker leaf cleanup failed") from error
        del self._worker_leaves[(worker_index, generation)]

    def memory_measurements(self) -> LinuxRunMemoryMeasurements:
        _require_contract(self._prepared and self._baseline_service is not None
                          and self._baseline_run is not None,
                          "Linux cgroup accounting is not prepared")
        service = _read_cgroup_events(self.service_root / "memory.events")
        run = _read_cgroup_events(self.run / "memory.events")
        delta = lambda current, baseline, name: max(0, current[name] - baseline[name])
        survivors = len(_cgroup_pids(self.coordinator / "cgroup.procs"))
        survivors += sum(len(_cgroup_pids(leaf / "cgroup.procs"))
                         for leaf, _token in self._worker_leaves.values())
        values = LinuxRunMemoryMeasurements(
            cgroup_current_accounted_memory_bytes=_read_cgroup_integer(
                self.run / "memory.current"),
            cgroup_peak_accounted_memory_bytes=_read_cgroup_integer(
                self.run / "memory.peak"),
            cgroup_memory_max_bytes=_read_cgroup_integer(self.run / "memory.max"),
            cgroup_memory_high_bytes=_read_cgroup_integer(self.run / "memory.high"),
            cgroup_oom_count_delta=delta(run, self._baseline_run, "oom"),
            cgroup_oom_kill_count_delta=delta(run, self._baseline_run, "oom_kill"),
            cgroup_max_event_count_delta=delta(run, self._baseline_run, "max"),
            service_root_oom_count_delta=delta(
                service, self._baseline_service, "oom"),
            service_root_oom_kill_count_delta=delta(
                service, self._baseline_service, "oom_kill"),
            service_root_max_event_count_delta=delta(
                service, self._baseline_service, "max"),
            surviving_cgroup_process_count=survivors,
            accounting_complete=(survivors == 0))
        if any((values.cgroup_oom_count_delta,
                values.cgroup_oom_kill_count_delta,
                values.cgroup_max_event_count_delta,
                values.service_root_oom_count_delta,
                values.service_root_oom_kill_count_delta,
                values.service_root_max_event_count_delta)):
            raise AuditInfrastructureError("Linux cgroup memory event increased")
        return values

    def cleanup(self) -> None:
        _require_contract(self._prepared and not self._worker_leaves,
                          "Linux cgroup cleanup ordering is invalid")
        _require_contract(not _cgroup_pids(self.coordinator / "cgroup.procs"),
                          "Linux coordinator cgroup is occupied")
        try:
            self.coordinator.rmdir()
            self.run.rmdir()
        except OSError as error:
            raise AuditInfrastructureError("Linux cgroup cleanup failed") from error
        self._prepared = False

    def force_cleanup(self, deadline: float) -> None:
        _require_contract(self._prepared and isinstance(deadline, (int, float))
                          and not isinstance(deadline, bool),
                          "Linux cgroup forced cleanup is invalid")
        self._write(self.run / "cgroup.kill", "1")
        while time.monotonic() < deadline:
            occupied = bool(_cgroup_pids(self.coordinator / "cgroup.procs"))
            occupied = occupied or any(
                _cgroup_pids(leaf / "cgroup.procs")
                for leaf, _token in self._worker_leaves.values())
            if not occupied:
                break
            time.sleep(0.01)
        else:
            raise AuditInfrastructureError("Linux cgroup forced cleanup timed out")
        for key, (leaf, _token) in tuple(self._worker_leaves.items()):
            try:
                leaf.rmdir()
            except OSError as error:
                raise AuditInfrastructureError(
                    "Linux worker leaf forced cleanup failed") from error
            del self._worker_leaves[key]
        self.cleanup()


def _linux_pid_cgroup_path(pid: int) -> Path:
    try:
        for line in Path(f"/proc/{pid}/cgroup").read_text(
                encoding="ascii").splitlines():
            hierarchy, controllers, relative = line.split(":", 2)
            if hierarchy == "0" and controllers == "":
                return (Path("/sys/fs/cgroup") / relative.lstrip("/")).resolve()
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "Linux rendezvous peer membership is unavailable") from error
    raise AuditInfrastructureError("Linux rendezvous peer is not in cgroup v2")


def _linux_pid_descends_from(
    pid: int, ancestor_pid: int, proc_root: Path = Path("/proc"),
) -> bool:
    _require_contract(all(isinstance(value, int) and not isinstance(value, bool)
                          and value > 0 for value in (pid, ancestor_pid))
                      and isinstance(proc_root, Path),
                      "Linux rendezvous process ancestry is invalid")
    current = pid
    visited: set[int] = set()
    while current not in visited:
        if current == ancestor_pid:
            return True
        if current <= 1:
            return False
        visited.add(current)
        try:
            stat = (proc_root / str(current) / "stat").read_text(
                encoding="ascii")
            suffix = stat[stat.rindex(")") + 1:].split()
            current = int(suffix[1])
        except (OSError, ValueError, IndexError) as error:
            raise AuditInfrastructureError(
                "Linux rendezvous process ancestry is unavailable") from error
    raise AuditInfrastructureError("Linux rendezvous process ancestry is cyclic")


class LinuxSeqpacketRendezvousServer:
    """Durable private authenticated supervisor rendezvous (never inherited)."""

    def __init__(self, supervisor: LinuxDelegatedCgroupSupervisor,
                 coordinator_pid: int | None,
                 coordinator_uid: int) -> None:
        _require_contract(sys.platform.startswith("linux")
                          and isinstance(supervisor,
                                         LinuxDelegatedCgroupSupervisor)
                          and supervisor._prepared,
                          "Linux rendezvous supervisor is invalid")
        self._supervisor = supervisor
        self._token = secrets.token_bytes(32)
        self._directory = Path(tempfile.mkdtemp(prefix="olr-gpu-cgroup-"))
        os.chmod(self._directory, 0o700)
        self.path = self._directory / "rendezvous.sock"
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            self._socket.bind(str(self.path))
            os.chmod(self.path, 0o600)
            self._socket.listen(1)
        except OSError as error:
            self.close()
            raise AuditInfrastructureError("Linux rendezvous socket setup failed") from error
        self._run_id = linux_rendezvous_run_id(self._token)
        self._coordinator_uid = coordinator_uid
        self._session: LinuxRendezvousSession | None = None
        if coordinator_pid is not None:
            self.bind_coordinator(coordinator_pid)

    def bind_coordinator(self, coordinator_pid: int) -> None:
        _require_contract(self._session is None,
                          "Linux rendezvous coordinator was bound twice")
        self._session = LinuxRendezvousSession(
            token=self._token, run_id=self._run_id,
            coordinator_pid=coordinator_pid,
            coordinator_uid=self._coordinator_uid,
            coordinator_leaf=str(self._supervisor.coordinator.resolve()))

    @property
    def environment(self) -> Mapping[str, str]:
        return {
            "OLR_CGROUP_RENDEZVOUS_PATH": str(self.path),
            "OLR_CGROUP_RENDEZVOUS_TOKEN": self._token.hex(),
        }

    def receive_once(self, deadline: float) -> dict[str, object]:
        _require_contract(isinstance(deadline, (int, float))
                          and not isinstance(deadline, bool)
                          and time.monotonic() < deadline,
                          "Linux rendezvous deadline is invalid")
        _require_contract(self._session is not None,
                          "Linux rendezvous coordinator is unbound")
        self._socket.settimeout(max(0.0, deadline - time.monotonic()))
        try:
            connection, _address = self._socket.accept()
        except (OSError, TimeoutError) as error:
            raise AuditInfrastructureError("Linux rendezvous accept timed out") from error
        with connection:
            connection.settimeout(max(0.0, deadline - time.monotonic()))
            try:
                credentials = connection.getsockopt(
                    socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
                peer_pid, peer_uid, _peer_gid = struct.unpack("3i", credentials)
                payload = connection.recv(LINUX_RENDEZVOUS_MAX_BYTES + 1)
            except OSError as error:
                raise AuditInfrastructureError("Linux rendezvous receive failed") from error
            _require_contract(0 < len(payload) <= LINUX_RENDEZVOUS_MAX_BYTES,
                              "Linux rendezvous payload is invalid")
            _require_contract(_linux_pid_descends_from(
                peer_pid, self._session._coordinator_pid),
                "Linux rendezvous peer is outside coordinator ancestry")
            fields = self._session.accept(
                payload, peer_pid=peer_pid, peer_uid=peer_uid,
                peer_cgroup=str(_linux_pid_cgroup_path(peer_pid)))
            kind = fields["kind"]
            leaf_path = ""
            leaf_token = ""
            if kind == "create-leaf":
                leaf, leaf_token = self._supervisor.create_worker_leaf(
                    fields["worker_index"], fields["generation"], fields["nonce"])
                leaf_path = str(leaf.resolve())
                reply_kind = "leaf-created"
            elif kind == "ack-leaf":
                self._supervisor.verify_worker_membership(
                    fields["worker_index"], fields["generation"], fields["nonce"],
                    fields["worker_pid"])
                reply_kind = "leaf-acknowledged"
            elif kind == "release-leaf":
                self._supervisor.release_worker_leaf(
                    fields["worker_index"], fields["generation"])
                reply_kind = "leaf-released"
            else:
                reply_kind = "hello-accepted"
            reply = dict(fields)
            reply.update(kind=reply_kind, leaf_path=leaf_path,
                         leaf_token=leaf_token)
            response = encode_linux_rendezvous_reply(reply, self._token)
            try:
                sent = connection.send(response)
            except OSError as error:
                raise AuditInfrastructureError("Linux rendezvous reply failed") from error
            _require_contract(sent == len(response),
                              "Linux rendezvous reply was truncated")
            return fields

    def close(self) -> None:
        current = getattr(self, "_socket", None)
        if current is not None:
            current.close()
            self._socket = None
        path = getattr(self, "path", None)
        if isinstance(path, Path):
            try:
                path.unlink(missing_ok=True)
            except OSError as error:
                raise AuditInfrastructureError("Linux rendezvous socket cleanup failed") from error
        directory = getattr(self, "_directory", None)
        if isinstance(directory, Path) and directory.exists():
            try:
                directory.rmdir()
            except OSError as error:
                raise AuditInfrastructureError(
                    "Linux rendezvous directory cleanup failed") from error

    def __enter__(self) -> "LinuxSeqpacketRendezvousServer":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


class LinuxRendezvousClient:
    """Coordinator client reconstructed only from the two exported variables."""

    def __init__(self, environment: Mapping[str, str]) -> None:
        _require_contract(isinstance(environment, Mapping)
                          and set(environment).issuperset({
                              "OLR_CGROUP_RENDEZVOUS_PATH",
                              "OLR_CGROUP_RENDEZVOUS_TOKEN"}),
                          "Linux rendezvous environment is invalid")
        path = environment["OLR_CGROUP_RENDEZVOUS_PATH"]
        encoded_token = environment["OLR_CGROUP_RENDEZVOUS_TOKEN"]
        _require_contract(isinstance(path, str) and path.startswith("/")
                          and isinstance(encoded_token, str)
                          and re.fullmatch(r"[0-9a-f]{64}", encoded_token) is not None,
                          "Linux rendezvous environment is invalid")
        self._path = path
        self._token = bytes.fromhex(encoded_token)
        self._run_id = linux_rendezvous_run_id(self._token)
        self._sequence = 0

    def _request(self, kind: str, worker_index: int, generation: int,
                 nonce: str, deadline: float, worker_pid: int = 0
                 ) -> dict[str, object]:
        self._sequence += 1
        fields = {
            "kind": kind, "run_id": self._run_id,
            "worker_index": worker_index, "generation": generation,
            "worker_pid": worker_pid, "nonce": nonce,
            "sequence": self._sequence,
        }
        payload = encode_linux_rendezvous_frame(fields, self._token)
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        connection.settimeout(max(0.0, deadline - time.monotonic()))
        try:
            connection.connect(self._path)
            sent = connection.send(payload)
            _require_contract(sent == len(payload),
                              "Linux rendezvous request was truncated")
            response = connection.recv(LINUX_RENDEZVOUS_MAX_BYTES + 1)
        except OSError as error:
            raise AuditInfrastructureError("Linux rendezvous request failed") from error
        finally:
            connection.close()
        reply = decode_linux_rendezvous_reply(response, self._token)
        for name in _LINUX_FRAME_KEYS - {"kind"}:
            _require_contract(reply[name] == fields[name],
                              "Linux rendezvous reply binding differs")
        return reply

    def hello(self, deadline: float) -> None:
        reply = self._request("hello", 0, 0, secrets.token_hex(16), deadline)
        _require_contract(reply["kind"] == "hello-accepted",
                          "Linux rendezvous hello was rejected")

    def coordinator_accounting_paths(self) -> tuple[Path, Path, Path]:
        """Return the delegated coordinator, run, and service cgroups."""

        coordinator = _current_linux_cgroup_path()
        run = coordinator.parent
        service_root = run.parent
        _require_contract(
            coordinator.name == "coordinator"
            and run.name == "run"
            and coordinator.is_dir()
            and run.is_dir()
            and service_root.is_dir(),
            "Linux rendezvous coordinator cgroup differs",
        )
        return coordinator, run, service_root

    def create_leaf(self, worker_index: int, generation: int,
                    deadline: float) -> LinuxWorkerContainment:
        reply = self._request(
            "create-leaf", worker_index, generation, secrets.token_hex(16), deadline)
        _require_contract(reply["kind"] == "leaf-created",
                          "Linux rendezvous leaf creation was rejected")
        return LinuxWorkerContainment(
            cgroup_run_path=str(Path(reply["leaf_path"]).parent),
            cgroup_leaf_path=reply["leaf_path"],
            generation_token=reply["leaf_token"])

    def acknowledge_leaf(self, worker_index: int, generation: int,
                         worker_pid: int, carrier: LinuxWorkerContainment,
                         deadline: float) -> None:
        reply = self._request(
            "ack-leaf", worker_index, generation,
            carrier.generation_token, deadline, worker_pid)
        _require_contract(reply["kind"] == "leaf-acknowledged",
                          "Linux rendezvous leaf acknowledgement was rejected")

    def release_leaf(self, worker_index: int, generation: int,
                     deadline: float) -> None:
        reply = self._request(
            "release-leaf", worker_index, generation,
            secrets.token_hex(16), deadline)
        _require_contract(reply["kind"] == "leaf-released",
                          "Linux rendezvous leaf release was rejected")


def acknowledge_linux_worker_containment(
    carrier: LinuxWorkerContainment, pid: int,
) -> None:
    _require_contract(isinstance(carrier, LinuxWorkerContainment)
                      and isinstance(pid, int) and not isinstance(pid, bool)
                      and pid > 0,
                      "Linux worker containment carrier is invalid")
    leaf = Path(carrier.cgroup_leaf_path)
    run = Path(carrier.cgroup_run_path)
    _require_contract(leaf.parent == run,
                      "Linux worker containment leaf differs from run")
    try:
        (leaf / "cgroup.procs").write_text(str(pid), encoding="ascii")
    except OSError as error:
        raise AuditInfrastructureError("Linux worker containment move failed") from error
    _require_contract(pid in _cgroup_pids(leaf / "cgroup.procs"),
                      "Linux worker containment acknowledgement failed")


MACOS_PGID_MESSAGE_SCHEMA = "olr-gpu-macos-pgid-v1"
MACOS_PGID_MESSAGE_MAX_BYTES = 64 * 1024


def _macos_file_identity_object(identity: FileIdentity) -> dict[str, object]:
    return {
        "canonical": str(identity.canonical),
        "relative": (None if identity.relative is None
                     else identity.relative.as_posix()),
        "device": identity.device, "inode": identity.inode,
        "line_count": identity.line_count, "production": identity.production,
    }


def _decode_macos_file_identity(value: object) -> FileIdentity:
    _require_contract(isinstance(value, dict) and set(value) == {
        "canonical", "relative", "device", "inode", "line_count", "production"},
        "macOS PGID executable identity is invalid")
    _require_contract(isinstance(value["canonical"], str)
                      and (Path(value["canonical"]).is_absolute()
                           or value["canonical"].startswith(("/", "\\")))
                      and (value["relative"] is None
                           or isinstance(value["relative"], str))
                      and isinstance(value["production"], bool),
                      "macOS PGID executable identity is invalid")
    for name in ("device", "inode", "line_count"):
        _nonnegative_integer(value[name], "macOS PGID executable identity")
    return FileIdentity(
        Path(value["canonical"]),
        None if value["relative"] is None else PurePosixPath(value["relative"]),
        value["device"], value["inode"], value["line_count"], value["production"])


def encode_macos_pgid_message(message: object) -> bytes:
    if isinstance(message, CompilerPgidReported):
        kind = "audit-report"
        body = dataclasses.asdict(message)
        body["purpose"] = message.purpose.value
        body["executable_identity"] = _macos_file_identity_object(
            message.executable_identity)
    elif isinstance(message, CompilerExecPermit):
        kind = "audit-permit"
        body = dataclasses.asdict(message)
    elif isinstance(message, MacOSInspectionPgidReported):
        kind = "inspection-report"
        body = dataclasses.asdict(message)
        body["executable_identity"] = _macos_file_identity_object(
            message.executable_identity)
    elif isinstance(message, MacOSInspectionExecPermit):
        kind = "inspection-permit"
        body = dataclasses.asdict(message)
    else:
        raise AuditInfrastructureError("macOS PGID message type is invalid")
    payload = _canonical_json({"schema": MACOS_PGID_MESSAGE_SCHEMA,
                               "kind": kind, "body": body})
    _require_contract(len(payload) <= MACOS_PGID_MESSAGE_MAX_BYTES,
                      "macOS PGID message exceeds bound")
    return payload


def decode_macos_pgid_message(payload: bytes) -> object:
    try:
        _require_contract(isinstance(payload, bytes)
                          and 0 < len(payload) <= MACOS_PGID_MESSAGE_MAX_BYTES,
                          "macOS PGID message is invalid")
        value = json.loads(payload)
        _require_contract(isinstance(value, dict)
                          and set(value) == {"schema", "kind", "body"}
                          and value["schema"] == MACOS_PGID_MESSAGE_SCHEMA
                          and _canonical_json(value) == payload
                          and isinstance(value["body"], dict),
                          "macOS PGID message is invalid")
        kind = value["kind"]
        body = value["body"]
        types = {
            "audit-report": CompilerPgidReported,
            "audit-permit": CompilerExecPermit,
            "inspection-report": MacOSInspectionPgidReported,
            "inspection-permit": MacOSInspectionExecPermit,
        }
        message_type = types.get(kind)
        _require_contract(message_type is not None
                          and set(body) == {field.name for field in
                                           dataclasses.fields(message_type)},
                          "macOS PGID message fields are invalid")
        if kind in {"audit-report", "inspection-report"}:
            body["executable_identity"] = _decode_macos_file_identity(
                body["executable_identity"])
        if kind == "audit-report":
            body["purpose"] = CompilerLaunchPurpose(body["purpose"])
        return message_type(**body)
    except (AuditInfrastructureError, UnicodeError, ValueError, TypeError) as error:
        if isinstance(error, AuditInfrastructureError):
            raise
        raise AuditInfrastructureError("macOS PGID message is invalid") from error


class MacOSRegisteredPgidAccountant:
    """Account only explicitly registered PGIDs; no global-descendant claim."""

    def __init__(self) -> None:
        self._leaders: dict[int, OwnedProcessIdentity] = {}
        self._members: dict[int, tuple[OwnedProcessIdentity, ...]] = {}
        self._maximum_owned = 0
        self._maximum_parent = 0
        self._maximum_aggregate = 0
        self._unreconciled: dict[
            int, tuple[OwnedProcessIdentity, ...]
        ] = {}
        self._reconciled_pgids: set[int] = set()
        self._registration_intents: dict[
            int, tuple[OwnedProcessIdentity, str]
        ] = {}
        self._registration_receipts: set[int] = set()
        self._complete = True

    def register_group(self, pgid: int, leader: OwnedProcessIdentity,
                       purpose: str) -> None:
        _require_contract(isinstance(pgid, int) and not isinstance(pgid, bool)
                          and pgid > 0 and leader.platform_kind == "macos"
                          and leader.pid == pgid,
                          "macOS registered PGID is invalid")
        _require_contract(isinstance(purpose, str) and bool(purpose),
                          "macOS registered PGID purpose is invalid")
        intent = (leader, purpose)
        existing_intent = self._registration_intents.get(pgid)
        if existing_intent is None:
            _require_contract(
                pgid not in self._leaders
                and pgid not in self._members
                and pgid not in self._reconciled_pgids,
                "macOS PGID was registered twice",
            )
            self._registration_intents[pgid] = intent
        else:
            _require_contract(
                existing_intent == intent,
                "macOS registered PGID intent differs",
            )
        if pgid in self._registration_receipts:
            return
        existing_leader = self._leaders.get(pgid)
        existing_members = self._members.get(pgid)
        _require_contract(
            existing_leader in (None, leader)
            and existing_members in (None, (leader,)),
            "macOS registered PGID effect differs",
        )
        if existing_leader is None:
            self._leaders[pgid] = leader
        if existing_members is None:
            self._members[pgid] = (leader,)
        self._registration_receipts.add(pgid)

    def observe_group(self, pgid: int,
                      resident_by_identity: Mapping[OwnedProcessIdentity, int]) -> None:
        """Validate one group; callers needing metrics use observe_snapshot()."""
        leader = self._leaders.get(pgid)
        _require_contract(leader is not None, "macOS PGID is not registered")
        _require_contract(isinstance(resident_by_identity, Mapping),
                          "macOS PGID observation is invalid")
        members: list[OwnedProcessIdentity] = []
        for identity, resident in resident_by_identity.items():
            if identity.pid == leader.pid and identity != leader:
                self._complete = False
                raise AuditInfrastructureError("macOS process start identity changed")
            _require_contract(identity.platform_kind == "macos",
                              "foreign process in macOS registered group")
            members.append(identity)
            _nonnegative_integer(resident, "macOS observed residency")
        _require_contract(leader in resident_by_identity,
                          "macOS registered leader is missing")
        self._members[pgid] = tuple(members)

    def observe_snapshot(
        self,
        parent_resident_bytes: int,
        resident_by_pgid: Mapping[int, Mapping[OwnedProcessIdentity, int]],
    ) -> None:
        parent = _nonnegative_integer(parent_resident_bytes,
                                      "macOS parent observed residency")
        _require_contract(isinstance(resident_by_pgid, Mapping)
                          and set(resident_by_pgid) == set(self._leaders),
                          "macOS registered-group snapshot is incomplete")
        owned = 0
        for pgid, residents in resident_by_pgid.items():
            self.observe_group(pgid, residents)
            owned += sum(_nonnegative_integer(value, "macOS observed residency")
                         for value in residents.values())
        self._maximum_parent = max(self._maximum_parent, parent)
        self._maximum_owned = max(self._maximum_owned, owned)
        self._maximum_aggregate = max(self._maximum_aggregate, parent + owned)

    def reconcile_group(self, pgid: int,
                        provider: "MacOSLibprocProvider") -> bool:
        reconciled = getattr(self, "_reconciled_pgids", None)
        if reconciled is None:
            reconciled = set()
            self._reconciled_pgids = reconciled
        _require_contract(isinstance(provider, MacOSLibprocProvider),
                          "macOS reconciliation native provider is invalid")
        if pgid in reconciled:
            self._leaders.pop(pgid, None)
            self._members.pop(pgid, None)
            self._unreconciled.pop(pgid, None)
            return True
        _require_contract(pgid in self._leaders,
                          "macOS reconciliation native provider is invalid")
        surviving_identities = provider.reconcile_survivors(self, pgid)
        _require_contract(isinstance(surviving_identities, tuple)
                          and all(isinstance(identity, OwnedProcessIdentity)
                                  and identity.platform_kind == "macos"
                                  for identity in surviving_identities),
                          "macOS native reconciliation result is invalid")
        if surviving_identities:
            self._unreconciled[pgid] = surviving_identities
            self._members[pgid] = surviving_identities
            raise AuditInfrastructureError(
                "macOS registered PGID survivors remain"
            )
        self._unreconciled.pop(pgid, None)
        reconciled.add(pgid)
        self._leaders.pop(pgid, None)
        self._members.pop(pgid, None)
        return True

    def memory_measurements(self) -> MacOSRunMemoryMeasurements:
        survivors = sum(max(1, len(members))
                        for members in self._members.values())
        return MacOSRunMemoryMeasurements(
            maximum_observed_aggregate_resident_bytes=self._maximum_aggregate,
            maximum_observed_parent_resident_bytes=self._maximum_parent,
            maximum_observed_owned_group_resident_bytes=self._maximum_owned,
            surviving_registered_process_count=survivors,
            known_unreconciled_descendant_count=sum(
                len(identities)
                for identities in self._unreconciled.values()
            ),
            accounting_complete=(self._complete and not self._leaders
                                 and survivors == 0
                                 and not self._unreconciled),
        )


class MacOSExecPermitAuthority:
    """Register verified PGIDs before issuing one-shot compiler exec permits."""

    def __init__(self, accountant: MacOSRegisteredPgidAccountant,
                 trusted_driver_fingerprints: tuple[str, ...],
                 identity_verifier=None, executable_verifier=None) -> None:
        _require_contract(isinstance(accountant, MacOSRegisteredPgidAccountant)
                          and isinstance(trusted_driver_fingerprints, tuple)
                          and trusted_driver_fingerprints
                          and all(re.fullmatch(r"[0-9a-f]{64}", value) is not None
                                  for value in trusted_driver_fingerprints),
                          "macOS exec permit authority is invalid")
        _require_contract(callable(executable_verifier),
                          "macOS held executable verifier is unavailable")
        self._accountant = accountant
        self._trusted = frozenset(trusted_driver_fingerprints)
        self._verifier = identity_verifier
        self._executable_verifier = executable_verifier
        self._issued: set[tuple[object, ...]] = set()
        self._permit_intents: dict[tuple[object, ...], object] = {}

    def _verify(self, pid: int, pgid: int, start_identity: str) -> None:
        _require_contract(pid == pgid and isinstance(start_identity, str)
                          and bool(start_identity),
                          "macOS reported process identity is invalid")
        if self._verifier is not None:
            actual = self._verifier(pid, pgid)
            _require_contract(actual == OwnedProcessIdentity(
                "macos", pid, start_identity),
                "macOS reported process identity differs")
            return
        provider = MacOSLibprocProvider()
        identity, _resident, _parent = provider._identity_and_residency(pid, pgid)
        _require_contract(identity == OwnedProcessIdentity(
            "macos", pid, start_identity),
            "macOS reported process identity differs")

    def permit_compiler(self, report: CompilerPgidReported) -> CompilerExecPermit:
        _require_contract(isinstance(report, CompilerPgidReported),
                          "unregistered or untrusted macOS compiler exec")
        derived_fingerprint = self._executable_verifier(
            report.executable_identity, report.executable_sha256)
        _require_contract(derived_fingerprint == report.driver_fingerprint
                          and derived_fingerprint in self._trusted,
                          "unregistered or untrusted macOS compiler exec")
        key = ("compiler", report.worker_index, report.generation,
               report.task_id, report.pgid)
        existing_intent = self._permit_intents.get(key)
        if existing_intent is None:
            self._permit_intents[key] = report
        else:
            _require_contract(
                existing_intent == report,
                "macOS compiler exec permit intent differs",
            )
        permit = CompilerExecPermit(
            report.worker_index, report.generation, report.task_id, report.pgid)
        if key in self._issued:
            return permit
        self._verify(report.pid, report.pgid, report.bsd_start_identity)
        leader = OwnedProcessIdentity(
            "macos", report.pid, report.bsd_start_identity)
        self._accountant.register_group(report.pgid, leader, "compiler")
        self._issued.add(key)
        return permit

    def permit_inspection(
        self, report: MacOSInspectionPgidReported,
    ) -> MacOSInspectionExecPermit:
        _require_contract(isinstance(report, MacOSInspectionPgidReported),
                          "unregistered or untrusted macOS inspection exec")
        driver_fingerprint = self._executable_verifier(
            report.executable_identity, report.executable_sha256)
        _require_contract(driver_fingerprint in self._trusted,
                          "unregistered or untrusted macOS inspection exec")
        key = ("inspection", report.inspection_id, report.pgid)
        existing_intent = self._permit_intents.get(key)
        if existing_intent is None:
            self._permit_intents[key] = report
        else:
            _require_contract(
                existing_intent == report,
                "macOS inspection exec permit intent differs",
            )
        permit = MacOSInspectionExecPermit(
            report.inspection_id, report.pid, report.pgid, driver_fingerprint)
        if key in self._issued:
            return permit
        self._verify(report.pid, report.pgid, report.bsd_start_identity)
        leader = OwnedProcessIdentity(
            "macos", report.pid, report.bsd_start_identity)
        self._accountant.register_group(report.pgid, leader, "inspection")
        self._issued.add(key)
        return permit


class MacOSLibprocProvider:
    """Bounded native PGID membership/start/RSS observer for Darwin."""

    def __init__(self, maximum_members: int = 4096) -> None:
        _require_contract(sys.platform == "darwin",
                          "macOS libproc accounting requires macOS")
        _require_contract(isinstance(maximum_members, int)
                          and not isinstance(maximum_members, bool)
                          and 0 < maximum_members <= 65536,
                          "macOS libproc member ceiling is invalid")
        import ctypes

        self._ctypes = ctypes
        self._libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        self._maximum_members = maximum_members

    def _list_pids(self, pgid: int) -> tuple[int, ...]:
        ctypes = self._ctypes
        capacity = 32
        while capacity <= self._maximum_members:
            values = (ctypes.c_int * capacity)()
            written = self._libproc.proc_listpids(
                2, pgid, ctypes.byref(values), ctypes.sizeof(values))
            if written < 0:
                raise AuditInfrastructureError("macOS PGID enumeration failed")
            count = written // ctypes.sizeof(ctypes.c_int)
            if count < capacity:
                return tuple(int(values[index]) for index in range(count)
                             if values[index] > 0)
            capacity *= 2
        raise AuditInfrastructureError("macOS PGID enumeration was truncated")

    def _stable_list_pids(self, pgid: int) -> tuple[int, ...]:
        previous: tuple[int, ...] | None = None
        stable_observations = 0
        for _attempt in range(8):
            current = tuple(sorted(set(self._list_pids(pgid))))
            if current == previous:
                stable_observations += 1
            else:
                previous = current
                stable_observations = 1
            if stable_observations >= 3:
                return current
            time.sleep(0.001)
        raise AuditInfrastructureError(
            "macOS PGID enumeration did not become stable")

    def _identity_and_residency(self, pid: int, expected_pgid: int
                                ) -> tuple[OwnedProcessIdentity, int, int]:
        ctypes = self._ctypes

        class BsdInfo(ctypes.Structure):
            _fields_ = [
                ("flags", ctypes.c_uint32), ("status", ctypes.c_uint32),
                ("xstatus", ctypes.c_uint32), ("pid", ctypes.c_uint32),
                ("ppid", ctypes.c_uint32), ("uid", ctypes.c_uint32),
                ("gid", ctypes.c_uint32), ("ruid", ctypes.c_uint32),
                ("rgid", ctypes.c_uint32), ("svuid", ctypes.c_uint32),
                ("svgid", ctypes.c_uint32), ("rfu_1", ctypes.c_uint32),
                ("comm", ctypes.c_char * 16), ("name", ctypes.c_char * 32),
                ("nfiles", ctypes.c_uint32), ("pgid", ctypes.c_uint32),
                ("pjobc", ctypes.c_uint32), ("e_tdev", ctypes.c_uint32),
                ("e_tpgid", ctypes.c_uint32), ("nice", ctypes.c_int32),
                ("start_tvsec", ctypes.c_uint64),
                ("start_tvusec", ctypes.c_uint64),
            ]

        class TaskInfo(ctypes.Structure):
            _fields_ = [
                ("virtual_size", ctypes.c_uint64),
                ("resident_size", ctypes.c_uint64),
                ("total_user", ctypes.c_uint64), ("total_system", ctypes.c_uint64),
                ("threads_user", ctypes.c_uint64),
                ("threads_system", ctypes.c_uint64),
                *[(name, ctypes.c_int32) for name in (
                    "policy", "faults", "pageins", "cow_faults")],
                *[(name, ctypes.c_uint32) for name in (
                    "messages_sent", "messages_received", "syscalls_mach",
                    "syscalls_unix", "csw", "threadnum", "numrunning")],
                ("priority", ctypes.c_int32),
            ]

        bsd = BsdInfo()
        if self._libproc.proc_pidinfo(
                pid, 3, 0, ctypes.byref(bsd), ctypes.sizeof(bsd)) != ctypes.sizeof(bsd):
            raise AuditInfrastructureError("macOS process identity query failed")
        _require_contract(int(bsd.pid) == pid and int(bsd.pgid) == expected_pgid,
                          "foreign process in macOS registered group")
        task = TaskInfo()
        if self._libproc.proc_pidinfo(
                pid, 4, 0, ctypes.byref(task), ctypes.sizeof(task)) != ctypes.sizeof(task):
            raise AuditInfrastructureError("macOS process residency query failed")
        identity = OwnedProcessIdentity(
            "macos", pid, f"{int(bsd.start_tvsec)}:{int(bsd.start_tvusec)}")
        return identity, int(task.resident_size), int(bsd.ppid)

    def observe(self, accountant: MacOSRegisteredPgidAccountant,
                parent_resident_bytes: int) -> None:
        _require_contract(isinstance(accountant, MacOSRegisteredPgidAccountant),
                          "macOS registered accountant is invalid")
        snapshot: dict[int, dict[OwnedProcessIdentity, int]] = {}
        for pgid in tuple(accountant._leaders):
            live_prior_pids: set[int] = set()
            for prior in accountant._members[pgid]:
                try:
                    actual, _resident, _parent = self._identity_and_residency(
                        prior.pid, pgid)
                except AuditInfrastructureError:
                    try:
                        os.kill(prior.pid, 0)
                    except ProcessLookupError:
                        continue
                    except PermissionError:
                        pass
                    raise
                if actual != prior:
                    raise AuditInfrastructureError(
                        "macOS process start identity changed")
                live_prior_pids.add(prior.pid)
            stable_pids = self._stable_list_pids(pgid)
            for pid in live_prior_pids - set(stable_pids):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    continue
                except PermissionError:
                    pass
                raise AuditInfrastructureError(
                    "macOS stable PGID enumeration omitted a live member")
            observed = tuple(self._identity_and_residency(pid, pgid)
                             for pid in stable_pids)
            entries = {identity: resident for identity, resident, _ppid in observed}
            parents = {identity.pid: ppid for identity, _resident, ppid in observed}
            leader = accountant._leaders[pgid]
            if leader not in entries:
                raise AuditInfrastructureError("macOS registered leader is missing")
            member_pids = set(parents)
            for pid in member_pids - {pgid}:
                current = pid
                visited: set[int] = set()
                while current != pgid:
                    if current in visited or current not in parents:
                        raise AuditInfrastructureError(
                            "foreign process in macOS registered group")
                    visited.add(current)
                    current = parents[current]
            snapshot[pgid] = entries
        accountant.observe_snapshot(parent_resident_bytes, snapshot)

    def reconcile_survivors(
        self, accountant: MacOSRegisteredPgidAccountant, pgid: int,
    ) -> tuple[OwnedProcessIdentity, ...]:
        _require_contract(isinstance(accountant, MacOSRegisteredPgidAccountant)
                          and pgid in accountant._leaders,
                          "macOS native reconciliation input is invalid")
        live_prior_pids: set[int] = set()
        for prior in accountant._members[pgid]:
            try:
                actual, _resident, _parent = self._identity_and_residency(
                    prior.pid, pgid)
            except AuditInfrastructureError:
                try:
                    os.kill(prior.pid, 0)
                except ProcessLookupError:
                    continue
                except PermissionError:
                    pass
                raise
            if actual != prior:
                raise AuditInfrastructureError(
                    "macOS process start identity changed")
            live_prior_pids.add(prior.pid)
        stable_pids = self._stable_list_pids(pgid)
        missing_live = live_prior_pids - set(stable_pids)
        for pid in missing_live:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                continue
            except PermissionError:
                pass
            raise AuditInfrastructureError(
                "macOS stable PGID enumeration omitted a live member")
        observed = tuple(self._identity_and_residency(pid, pgid)[0]
                         for pid in stable_pids)
        return tuple(sorted(observed, key=lambda identity: (
            identity.pid, identity.native_start_identity)))


def _attempt_phase_seal_transition(
    _platform: str, _stage: str, _position: str, _phase: str
) -> None:
    return None


class MacOSCompilerAuditAttemptAccountant(MacOSRegisteredPgidAccountant):
    """Phase-aware native authority for one production calibration attempt."""

    def __init__(self) -> None:
        super().__init__()
        _require_contract(sys.platform == "darwin",
                          "macOS attempt accountant requires macOS")
        self.inspection_probe_invocations = 0
        self._provider = MacOSLibprocProvider()
        self._phase: str | None = None
        self._sealed: set[str] = set()
        self._sealed_snapshots: dict[str, MacOSPhaseSnapshot] = {}
        self._active_carriers: dict[object, object] = {}
        self._inspection_pgids: set[int] = set()
        self._pending_inspection: tuple[
            str, object, MacOSExecPermitAuthority, float
        ] | None = None

    def begin_phase(self, phase: str, deadline: float) -> None:
        expected = "inspection" if self._phase is None else "tasks"
        _require_contract(
            phase == expected
            and phase not in self._sealed
            and phase not in self._sealed_snapshots
            and isinstance(deadline, (int, float))
            and not isinstance(deadline, bool)
            and time.monotonic() < deadline,
            "macOS attempt accountant phase is invalid",
        )
        if phase == "tasks":
            _require_contract("inspection" in self._sealed,
                              "macOS inspection phase is not sealed")
        self._phase = phase

    def prepare_compiler_inspection_launch(self, capability, deadline: float) -> str:
        _require_contract(
            self._phase == "inspection"
            and self._pending_inspection is None
            and isinstance(deadline, (int, float))
            and not isinstance(deadline, bool)
            and time.monotonic() < deadline,
            "macOS inspection launch preparation is invalid",
        )
        inspection_id = secrets.token_hex(16)

        def verify_executable(identity, digest):
            _require_contract(
                identity == capability.executable_identity
                and digest == capability.executable_sha256,
                "macOS inspection executable authority differs",
            )
            return capability.capability_digest

        permit_authority = MacOSExecPermitAuthority(
            self,
            (capability.capability_digest,),
            executable_verifier=verify_executable,
        )
        self._pending_inspection = (
            inspection_id, capability, permit_authority, float(deadline))
        return inspection_id

    @property
    def macos_launch_deadline(self) -> float:
        _require_contract(self._pending_inspection is not None,
                          "macOS inspection launch deadline is unavailable")
        return self._pending_inspection[3]

    @property
    def macos_launch_cancel_event(self):
        return None

    def authorize_macos_compiler_exec(self, process_start):
        _require_contract(
            self._pending_inspection is not None
            and process_start.platform_kind == "macos",
            "macOS inspection exec authority is unavailable",
        )
        inspection_id, capability, authority, _deadline = self._pending_inspection
        report = MacOSInspectionPgidReported(
            inspection_id,
            process_start.pid,
            process_start.pid,
            process_start.native_start_token,
            capability.executable_identity,
            capability.executable_sha256,
        )
        permit = authority.permit_inspection(report)
        self._inspection_pgids.add(process_start.pid)
        self._provider.observe(self, native_process_resident_bytes(os.getpid()))
        return permit

    def register_compiler_process_launch(self, event, carrier) -> None:
        _require_contract(
            self._phase == "inspection"
            and event.purpose is CompilerLaunchPurpose.INSPECTION
            and event.process_start not in self._active_carriers
            and self._pending_inspection is not None,
            "macOS inspection launch registration is invalid",
        )
        self._active_carriers[event.process_start] = carrier
        self.inspection_probe_invocations += 1

    def complete_compiler_process_launch(self, event, carrier) -> None:
        _require_contract(
            self._active_carriers.pop(event.process_start, None) is carrier,
            "macOS inspection process carrier differs",
        )

    def fail_compiler_process_launch(self, event, carrier) -> None:
        if self._active_carriers.get(event.process_start) is carrier:
            self._active_carriers.pop(event.process_start)

    def finish_compiler_inspection_launch_preparation(self, token: str) -> None:
        _require_contract(
            self._pending_inspection is not None
            and self._pending_inspection[0] == token,
            "macOS inspection launch preparation differs",
        )
        self._pending_inspection = None

    def cancel_compiler_inspection_launch_preparation(self, token: str) -> None:
        if self._pending_inspection is not None and self._pending_inspection[0] == token:
            self._pending_inspection = None

    def seal_phase(self, phase: str, deadline: float):
        existing = self._sealed_snapshots.get(phase)
        if existing is None:
            _require_contract(
                phase == self._phase
                and phase not in self._sealed
                and not self._active_carriers
                and self._pending_inspection is None
                and time.monotonic() < deadline,
                "macOS attempt accountant phase seal is invalid",
            )
            if phase == "inspection":
                for pgid in tuple(self._inspection_pgids):
                    _require_contract(
                        self.reconcile_group(pgid, self._provider) is True,
                        "macOS inspection reconciliation did not complete",
                    )
                    self._inspection_pgids.remove(pgid)
            memory = self.memory_measurements()
            _require_contract(
                memory.accounting_complete
                and memory.surviving_registered_process_count == 0
                and memory.known_unreconciled_descendant_count == 0,
                "macOS attempt accountant phase survivors remain",
            )
            existing = MacOSPhaseSnapshot("macos", phase, memory, 0)
            _attempt_phase_seal_transition(
                "macos", "snapshot", "before", phase
            )
            self._sealed_snapshots[phase] = existing
            _attempt_phase_seal_transition(
                "macos", "snapshot", "after", phase
            )
        if phase not in self._sealed:
            _attempt_phase_seal_transition(
                "macos", "sealed", "before", phase
            )
            self._sealed.add(phase)
            _attempt_phase_seal_transition(
                "macos", "sealed", "after", phase
            )
        return existing


def _current_linux_cgroup_path() -> Path:
    try:
        for line in Path("/proc/self/cgroup").read_text(
                encoding="ascii").splitlines():
            hierarchy, controllers, relative = line.split(":", 2)
            if hierarchy == "0" and controllers == "":
                return Path("/sys/fs/cgroup") / relative.lstrip("/")
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError("delegated cgroup membership is unavailable") from error
    raise AuditInfrastructureError("delegated cgroup-v2 membership is unavailable")


def _prepare_linux_cgroup(child_argv: tuple[str, ...]) -> int:
    if not sys.platform.startswith("linux"):
        raise AuditInfrastructureError("Linux cgroup supervisor requires Linux")
    _require_contract(isinstance(child_argv, tuple) and bool(child_argv)
                      and all(isinstance(value, str) and value and "\x00" not in value
                              for value in child_argv),
                      "Linux coordinator argv is invalid")
    supervisor = LinuxDelegatedCgroupSupervisor(_current_linux_cgroup_path())
    supervisor.prepare()
    rendezvous = LinuxSeqpacketRendezvousServer(
        supervisor, None, os.getuid())
    child = os.fork()
    if child == 0:
        try:
            supervisor.move_coordinator(os.getpid())
            environment = dict(os.environ)
            environment.update(rendezvous.environment)
            os.execvpe(child_argv[0], child_argv, environment)
        except BaseException:
            os._exit(127)
    try:
        rendezvous.bind_coordinator(child)
        hello_seen = False
        status = None
        while status is None:
            _pid, candidate = os.waitpid(child, os.WNOHANG)
            if _pid == child:
                status = candidate
                break
            try:
                fields = rendezvous.receive_once(time.monotonic() + 0.1)
                hello_seen = hello_seen or fields["kind"] == "hello"
            except AuditInfrastructureError as error:
                if "accept timed out" not in str(error):
                    raise
        _require_contract(hello_seen,
                          "Linux coordinator did not authenticate rendezvous")
        if os.WIFEXITED(status):
            returncode = os.WEXITSTATUS(status)
        elif os.WIFSIGNALED(status):
            returncode = 128 + os.WTERMSIG(status)
        else:
            raise AuditInfrastructureError("Linux coordinator status is invalid")
        measurements = supervisor.memory_measurements()
        _require_contract(measurements.accounting_complete,
                          "Linux cgroup accounting is incomplete")
        supervisor.cleanup()
        rendezvous.close()
        return returncode
    except BaseException as original_error:
        try:
            os.kill(child, 9)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(child, 0)
        except ChildProcessError:
            pass
        cleanup_error: AuditInfrastructureError | None = None
        try:
            if supervisor._prepared:
                supervisor.force_cleanup(time.monotonic() + 5.0)
        except AuditInfrastructureError as error:
            cleanup_error = error
        try:
            rendezvous.close()
        except AuditInfrastructureError as error:
            cleanup_error = cleanup_error or error
        if cleanup_error is not None:
            raise AuditInfrastructureError(
                f"Linux supervisor cleanup failed: {cleanup_error}") from original_error
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("prepare-linux-cgroup",))
    parser.add_argument("child_argv", nargs=argparse.REMAINDER)
    parsed = parser.parse_args(argv)
    if parsed.operation == "prepare-linux-cgroup":
        child = tuple(parsed.child_argv)
        if child and child[0] == "--":
            child = child[1:]
        return _prepare_linux_cgroup(child)
    raise AuditInfrastructureError("unsupported process-tree operation")


if __name__ == "__main__":
    raise SystemExit(main())
