#!/usr/bin/env python3
"""Own a Windows child process tree with kill-on-parent-close semantics."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from contextlib import nullcontext
import os
import subprocess
from collections.abc import Sequence
import time
from typing import Any
import uuid


_JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
_PROCESS_TERMINATE = 0x0001
_PROCESS_SET_QUOTA = 0x0100
_CREATE_SUSPENDED = 0x00000004
_TH32CS_SNAPTHREAD = 0x00000004
_THREAD_SUSPEND_RESUME = 0x0002
_CREATE_UNICODE_ENVIRONMENT = 0x00000400
_WAIT_TIMEOUT = 0x00000102
_INFINITE = 0xFFFFFFFF


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


class _BasicAccountingInformation(ctypes.Structure):
    _fields_ = [
        ("total_user_time", ctypes.c_int64),
        ("total_kernel_time", ctypes.c_int64),
        ("period_user_time", ctypes.c_int64),
        ("period_kernel_time", ctypes.c_int64),
        ("total_page_fault_count", wintypes.DWORD),
        ("total_processes", wintypes.DWORD),
        ("active_processes", wintypes.DWORD),
        ("terminated_processes", wintypes.DWORD),
    ]


class _StartupInfoW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("reserved", wintypes.LPWSTR),
        ("desktop", wintypes.LPWSTR),
        ("title", wintypes.LPWSTR),
        ("x", wintypes.DWORD),
        ("y", wintypes.DWORD),
        ("x_size", wintypes.DWORD),
        ("y_size", wintypes.DWORD),
        ("x_chars", wintypes.DWORD),
        ("y_chars", wintypes.DWORD),
        ("fill_attribute", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("show_window", wintypes.WORD),
        ("reserved2_size", wintypes.WORD),
        ("reserved2", ctypes.POINTER(ctypes.c_ubyte)),
        ("stdin", wintypes.HANDLE),
        ("stdout", wintypes.HANDLE),
        ("stderr", wintypes.HANDLE),
    ]


class _ProcessInformation(ctypes.Structure):
    _fields_ = [
        ("process", wintypes.HANDLE),
        ("thread", wintypes.HANDLE),
        ("process_id", wintypes.DWORD),
        ("thread_id", wintypes.DWORD),
    ]


class PrivateDesktop:
    """A private WinSta0 desktop retained for the complete process-tree lifetime."""

    def __init__(self) -> None:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.CreateDesktopW.argtypes = (
            wintypes.LPCWSTR,
            wintypes.LPCWSTR,
            ctypes.c_void_p,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_void_p,
        )
        user32.CreateDesktopW.restype = wintypes.HANDLE
        user32.CloseDesktop.argtypes = (wintypes.HANDLE,)
        user32.CloseDesktop.restype = wintypes.BOOL
        self._user32 = user32
        self._name = f"OLR_{os.getpid()}_{uuid.uuid4().hex}"
        self.path = f"winsta0\\{self._name}"
        self._handle = user32.CreateDesktopW(
            self._name,
            None,
            None,
            0,
            0x01FF,  # DESKTOP_ALL_ACCESS
            None,
        )
        if not self._handle:
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self) -> None:
        if self._handle:
            if not self._user32.CloseDesktop(self._handle):
                raise ctypes.WinError(ctypes.get_last_error())
            self._handle = None

    def __enter__(self) -> "PrivateDesktop":
        return self

    def __exit__(self, *_exc: object) -> None:
        if _exc and _exc[0] is not None:
            try:
                self.close()
            except BaseException:
                pass
        else:
            self.close()


class _NativeProcess:
    def __init__(self, handle: int, pid: int) -> None:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.GetExitCodeProcess.argtypes = (
            wintypes.HANDLE,
            ctypes.POINTER(wintypes.DWORD),
        )
        kernel32.GetExitCodeProcess.restype = wintypes.BOOL
        kernel32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
        kernel32.TerminateProcess.restype = wintypes.BOOL
        self._kernel32 = kernel32
        self._handle = handle
        self.pid = pid
        self.returncode: int | None = None

    def _close(self) -> None:
        if self._handle:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None

    def wait(self, timeout: float | None = None) -> int:
        milliseconds = _INFINITE if timeout is None else max(0, int(timeout * 1000))
        result = int(self._kernel32.WaitForSingleObject(self._handle, milliseconds))
        if result == _WAIT_TIMEOUT:
            raise subprocess.TimeoutExpired(["private-desktop-child"], timeout)
        if result != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        code = wintypes.DWORD()
        if not self._kernel32.GetExitCodeProcess(
            self._handle, ctypes.byref(code)
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        self.returncode = int(code.value)
        self._close()
        return self.returncode

    def kill(self) -> None:
        if self._handle and not self._kernel32.TerminateProcess(self._handle, 1):
            raise ctypes.WinError(ctypes.get_last_error())

    def __del__(self) -> None:
        try:
            self._close()
        except BaseException:
            pass


def _environment_block(environment: dict[str, str] | None) -> ctypes.Array | None:
    if environment is None:
        return None
    entries = sorted(
        (f"{key}={value}" for key, value in environment.items()),
        key=str.casefold,
    )
    return ctypes.create_unicode_buffer("\0".join(entries) + "\0\0")


def _popen_private_desktop(
    command: Sequence[str],
    *,
    desktop_path: str,
    creationflags: int,
    env: dict[str, str] | None = None,
    cwd: str | os.PathLike[str] | None = None,
    **kwargs: Any,
) -> _NativeProcess:
    """Create a suspended process with STARTUPINFO.lpDesktop set explicitly."""
    if kwargs:
        raise TypeError(
            "unsupported private-desktop process options: "
            + ", ".join(sorted(kwargs))
        )
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateProcessW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.LPWSTR,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.BOOL,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.LPCWSTR,
        ctypes.POINTER(_StartupInfoW),
        ctypes.POINTER(_ProcessInformation),
    )
    kernel32.CreateProcessW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL

    startup = _StartupInfoW()
    startup.cb = ctypes.sizeof(startup)
    startup.desktop = desktop_path
    information = _ProcessInformation()
    command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
    environment = _environment_block(env)
    flags = creationflags | (_CREATE_UNICODE_ENVIRONMENT if environment else 0)
    if not kernel32.CreateProcessW(
        str(command[0]),
        command_line,
        None,
        None,
        False,
        flags,
        environment,
        str(cwd) if cwd is not None else None,
        ctypes.byref(startup),
        ctypes.byref(information),
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    kernel32.CloseHandle(information.thread)
    return _NativeProcess(int(information.process), int(information.process_id))


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
        kernel32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
        kernel32.TerminateJobObject.restype = wintypes.BOOL
        kernel32.QueryInformationJobObject.argtypes = (
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.c_void_p,
        )
        kernel32.QueryInformationJobObject.restype = wintypes.BOOL
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
            handle = self._handle
            self._handle = None
            first_error: BaseException | None = None
            if not self._kernel32.TerminateJobObject(handle, 1):
                first_error = ctypes.WinError(ctypes.get_last_error())
            deadline = time.monotonic() + 5.0
            while first_error is None:
                information = _BasicAccountingInformation()
                if not self._kernel32.QueryInformationJobObject(
                    handle,
                    1,  # JobObjectBasicAccountingInformation
                    ctypes.byref(information),
                    ctypes.sizeof(information),
                    None,
                ):
                    first_error = ctypes.WinError(ctypes.get_last_error())
                    break
                if information.active_processes == 0:
                    break
                if time.monotonic() >= deadline:
                    first_error = TimeoutError(
                        "Windows Job Object did not become empty during cleanup"
                    )
                    break
                time.sleep(0.01)
            if not self._kernel32.CloseHandle(handle) and first_error is None:
                first_error = ctypes.WinError(ctypes.get_last_error())
            if first_error is not None:
                raise first_error

    def __enter__(self) -> "KillOnCloseJob":
        return self

    def __exit__(self, *_exc: object) -> None:
        if _exc and _exc[0] is not None:
            try:
                self.close()
            except BaseException:
                pass
        else:
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
    command: Sequence[str],
    *,
    enabled: bool,
    private_desktop: bool = False,
    **popen_kwargs: Any,
) -> int:
    """Run a child and ensure Windows descendants die when this process exits."""
    if not enabled:
        return subprocess.run(command, check=False, **popen_kwargs).returncode

    desktop_context = PrivateDesktop() if private_desktop else nullcontext(None)
    with desktop_context as desktop:
        with KillOnCloseJob() as job:
            creationflags = int(popen_kwargs.pop("creationflags", 0)) | _CREATE_SUSPENDED
            if desktop is None:
                process = subprocess.Popen(
                    command,
                    creationflags=creationflags,
                    **popen_kwargs,
                )
            else:
                process = _popen_private_desktop(
                    command,
                    desktop_path=desktop.path,
                    creationflags=creationflags,
                    **popen_kwargs,
                )
            try:
                job.assign(process.pid)
                _resume_process_threads(process.pid)
            except BaseException:
                try:
                    process.kill()
                except BaseException:
                    pass
                try:
                    process.wait()
                except BaseException:
                    pass
                raise
            try:
                return process.wait()
            except BaseException:
                # Closing first kills descendants; bounded wait releases the
                # direct process handle while preserving the primary exception.
                try:
                    job.close()
                except BaseException:
                    pass
                try:
                    process.wait(timeout=5)
                except BaseException:
                    pass
                raise
