#!/usr/bin/env python3
"""Own a Windows child process tree with kill-on-parent-close semantics."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import subprocess
from collections.abc import Sequence
from typing import Any


_JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
_PROCESS_TERMINATE = 0x0001
_PROCESS_SET_QUOTA = 0x0100
_CREATE_SUSPENDED = 0x00000004
_TH32CS_SNAPTHREAD = 0x00000004
_THREAD_SUSPEND_RESUME = 0x0002


class _ThreadEntry32(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD),
        ("usage_count", wintypes.DWORD),
        ("thread_id", wintypes.DWORD),
        ("owner_process_id", wintypes.DWORD),
        ("base_priority", wintypes.LONG),
        ("priority_delta", wintypes.LONG),
        ("flags", wintypes.DWORD),
    ]


class _IoCounters(ctypes.Structure):
    _fields_ = [
        ("read_operation_count", ctypes.c_uint64),
        ("write_operation_count", ctypes.c_uint64),
        ("other_operation_count", ctypes.c_uint64),
        ("read_transfer_count", ctypes.c_uint64),
        ("write_transfer_count", ctypes.c_uint64),
        ("other_transfer_count", ctypes.c_uint64),
    ]


class _BasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("per_process_user_time_limit", ctypes.c_int64),
        ("per_job_user_time_limit", ctypes.c_int64),
        ("limit_flags", wintypes.DWORD),
        ("minimum_working_set_size", ctypes.c_size_t),
        ("maximum_working_set_size", ctypes.c_size_t),
        ("active_process_limit", wintypes.DWORD),
        ("affinity", ctypes.c_size_t),
        ("priority_class", wintypes.DWORD),
        ("scheduling_class", wintypes.DWORD),
    ]


class _ExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("basic_limit_information", _BasicLimitInformation),
        ("io_info", _IoCounters),
        ("process_memory_limit", ctypes.c_size_t),
        ("job_memory_limit", ctypes.c_size_t),
        ("peak_process_memory_used", ctypes.c_size_t),
        ("peak_job_memory_used", ctypes.c_size_t),
    ]


class KillOnCloseJob:
    """A non-inheritable Job Object that terminates all members when closed."""

    def __init__(self) -> None:
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
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        self._kernel32 = kernel32
        self._handle = kernel32.CreateJobObjectW(None, None)
        if not self._handle:
            raise ctypes.WinError(ctypes.get_last_error())
        information = _ExtendedLimitInformation()
        information.basic_limit_information.limit_flags = (
            _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        )
        if not kernel32.SetInformationJobObject(
            self._handle,
            _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
            ctypes.byref(information),
            ctypes.sizeof(information),
        ):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def assign(self, pid: int) -> None:
        process = self._kernel32.OpenProcess(
            _PROCESS_TERMINATE | _PROCESS_SET_QUOTA,
            False,
            pid,
        )
        if not process:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if not self._kernel32.AssignProcessToJobObject(self._handle, process):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            self._kernel32.CloseHandle(process)

    def close(self) -> None:
        if self._handle:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "KillOnCloseJob":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def _resume_process_threads(pid: int) -> None:
    """Resume a just-created process after it has joined the owning Job Object."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Thread32First.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(_ThreadEntry32),
    )
    kernel32.Thread32First.restype = wintypes.BOOL
    kernel32.Thread32Next.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(_ThreadEntry32),
    )
    kernel32.Thread32Next.restype = wintypes.BOOL
    kernel32.OpenThread.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenThread.restype = wintypes.HANDLE
    kernel32.ResumeThread.argtypes = (wintypes.HANDLE,)
    kernel32.ResumeThread.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL

    snapshot = kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPTHREAD, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    thread_ids: list[int] = []
    try:
        entry = _ThreadEntry32()
        entry.size = ctypes.sizeof(entry)
        more = kernel32.Thread32First(snapshot, ctypes.byref(entry))
        while more:
            if entry.owner_process_id == pid:
                thread_ids.append(int(entry.thread_id))
            more = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    if not thread_ids:
        raise RuntimeError(f"created process has no resumable thread: pid {pid}")

    for thread_id in thread_ids:
        thread = kernel32.OpenThread(_THREAD_SUSPEND_RESUME, False, thread_id)
        if not thread:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if kernel32.ResumeThread(thread) == 0xFFFFFFFF:
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            kernel32.CloseHandle(thread)


def run_process_tree(
    command: Sequence[str], *, enabled: bool, **popen_kwargs: Any
) -> int:
    """Run a child and ensure Windows descendants die when this process exits."""
    if not enabled:
        return subprocess.run(command, check=False, **popen_kwargs).returncode

    with KillOnCloseJob() as job:
        creationflags = int(popen_kwargs.pop("creationflags", 0)) | _CREATE_SUSPENDED
        process = subprocess.Popen(
            command,
            creationflags=creationflags,
            **popen_kwargs,
        )
        try:
            job.assign(process.pid)
            _resume_process_threads(process.pid)
        except BaseException:
            process.kill()
            process.wait()
            raise
        try:
            return process.wait()
        except BaseException:
            # Closing first is what kills descendants. Waiting is bounded by the
            # direct child's termination and avoids leaving a zombie handle.
            job.close()
            try:
                process.wait(timeout=5)
            except BaseException:
                pass
            raise
