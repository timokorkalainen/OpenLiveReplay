#!/usr/bin/env python3
"""Reject build-tree DLLs that shadow the configured Windows runtime."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from collections.abc import Iterable, Mapping, Sequence
from contextlib import nullcontext
from functools import lru_cache
import hashlib
import os
from pathlib import Path
import re
import struct
import time
import uuid
from typing import BinaryIO, Callable, NamedTuple
import xml.etree.ElementTree as ET

from tools.windows_runtime_cache import (
    CacheLimits,
    CachedLayoutLease,
    GeneratedLayoutEntry,
    HeldSource,
    JsonValue,
    QtConfPaths,
    WindowsRuntimeCache,
    _draining_map,
    _parallel_executor,
)


@lru_cache(maxsize=1)
def _runtime_kernel32() -> object:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = (
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    )
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.GetFinalPathNameByHandleW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
    )
    kernel32.GetFinalPathNameByHandleW.restype = ctypes.c_uint32
    return kernel32


class RuntimeRoots(NamedTuple):
    compiler_bin: Path
    qt_bin: Path
    ffmpeg_bin: Path
    ffmpeg_lib: Path
    srt_bin: Path
    srt_lib: Path


class CacheEntry(NamedTuple):
    value: str
    cache_type: str


def _current_user_sid() -> str:
    if os.name != "nt":
        raise OSError("Windows user SID is only available on Windows")
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    token = wintypes.HANDLE()
    advapi32.OpenProcessToken.argtypes = (
        wintypes.HANDLE,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.HANDLE),
    )
    advapi32.OpenProcessToken.restype = wintypes.BOOL
    advapi32.GetTokenInformation.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    )
    advapi32.GetTokenInformation.restype = wintypes.BOOL
    advapi32.ConvertSidToStringSidW.argtypes = (
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_wchar_p),
    )
    advapi32.ConvertSidToStringSidW.restype = wintypes.BOOL
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    if not advapi32.OpenProcessToken(kernel32.GetCurrentProcess(), 0x0008, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        required = wintypes.DWORD()
        advapi32.GetTokenInformation(token, 1, None, 0, ctypes.byref(required))
        if not required.value:
            raise ctypes.WinError(ctypes.get_last_error())
        buffer = ctypes.create_string_buffer(required.value)
        if not advapi32.GetTokenInformation(
            token, 1, buffer, len(buffer), ctypes.byref(required)
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        sid_pointer = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_void_p))[0]
        text = ctypes.c_wchar_p()
        if not advapi32.ConvertSidToStringSidW(sid_pointer, ctypes.byref(text)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return str(text.value)
        finally:
            kernel32.LocalFree(text)
    finally:
        kernel32.CloseHandle(token)


def replace_environment_case_insensitively(
    inherited: Mapping[str, str], replacements: Mapping[str, str]
) -> dict[str, str]:
    """Replace controlled Windows variables without case-variant duplicates."""
    controlled = {key.casefold() for key in replacements}
    environment = {
        key: value
        for key, value in inherited.items()
        if key.casefold() not in controlled
    }
    environment.update(replacements)
    return environment


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def parse_cmake_cache(path: Path) -> dict[str, CacheEntry]:
    """Parse CMake cache declarations and reject duplicate typed keys."""
    entries: dict[str, CacheEntry] = {}
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line or line.startswith(("#", "//")):
            continue
        declaration, separator, value = line.partition("=")
        if not separator:
            continue
        key, type_separator, cache_type = declaration.partition(":")
        if not type_separator or not key:
            continue
        if key in entries:
            raise ValueError(
                f"duplicate CMake cache entry {key} at line {line_number}"
            )
        entries[key] = CacheEntry(value, cache_type)
    return entries


def require_cache_entries(
    path: Path,
    schema: dict[str, tuple[str, ...]],
    *,
    absolute_paths: tuple[str, ...] = (),
) -> dict[str, str]:
    entries = parse_cmake_cache(path)
    values: dict[str, str] = {}
    for key, allowed_types in schema.items():
        entry = entries.get(key)
        if entry is None or not entry.value:
            raise ValueError(f"CMake cache is missing required entry: {key}")
        if entry.cache_type not in allowed_types:
            expected = "/".join(allowed_types)
            raise ValueError(
                f"CMake cache entry {key} has type {entry.cache_type}; "
                f"expected {expected}"
            )
        if key in absolute_paths and not Path(entry.value).is_absolute():
            raise ValueError(f"CMake cache entry {key} must be an absolute path")
        values[key] = entry.value
    return values


def windows_system_directories() -> tuple[Path, Path]:
    """Return System32 and Windows directories from the operating system APIs."""
    if os.name != "nt":
        raise OSError("Windows directories are only available on Windows")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetSystemDirectoryW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32)
    kernel32.GetSystemDirectoryW.restype = ctypes.c_uint32
    kernel32.GetWindowsDirectoryW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32)
    kernel32.GetWindowsDirectoryW.restype = ctypes.c_uint32

    def query(function: object, label: str) -> Path:
        size = 32768
        buffer = ctypes.create_unicode_buffer(size)
        written = int(function(buffer, size))
        if not written or written >= size:
            raise ctypes.WinError(
                ctypes.get_last_error(), f"cannot query {label} directory"
            )
        result = Path(buffer.value)
        if not result.is_absolute() or not result.is_dir():
            raise RuntimeError(f"Windows API returned invalid {label} directory: {result}")
        return result.resolve()

    return (
        query(kernel32.GetSystemDirectoryW, "system"),
        query(kernel32.GetWindowsDirectoryW, "Windows"),
    )


def set_windows_dll_directory(directory: Path) -> str | None:
    """Set the inherited loader directory while preserving the prior value."""
    kernel32 = ctypes.windll.kernel32
    kernel32.GetDllDirectoryW.argtypes = (
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_wchar),
    )
    kernel32.GetDllDirectoryW.restype = ctypes.c_uint32
    kernel32.SetDllDirectoryW.argtypes = (ctypes.c_wchar_p,)
    kernel32.SetDllDirectoryW.restype = ctypes.c_int
    required = int(kernel32.GetDllDirectoryW(0, None))
    previous: str | None = None
    if required:
        buffer = ctypes.create_unicode_buffer(required + 1)
        written = int(kernel32.GetDllDirectoryW(len(buffer), buffer))
        if (not written and required > 1) or written >= len(buffer):
            raise ctypes.WinError(ctypes.get_last_error())
        previous = buffer.value
    if not kernel32.SetDllDirectoryW(str(directory)):
        raise ctypes.WinError(ctypes.get_last_error())
    return previous


def restore_windows_dll_directory(previous: str | None) -> None:
    if not ctypes.windll.kernel32.SetDllDirectoryW(previous):
        raise ctypes.WinError(ctypes.get_last_error())


class RuntimeAuditLease:
    """Hold audited binaries read-only until the controlled child exits."""

    def __init__(
        self,
        cache_root: Path | None = None,
        *,
        cache_limits: CacheLimits = CacheLimits(),
        cache_clock: Callable[[], float] = time.time,
    ) -> None:
        self._streams: dict[Path, BinaryIO] = {}
        self._canonical_files: dict[Path, Path] = {}
        self._directory_handles: dict[Path, tuple[object, Path]] = {}
        self._cache_root = cache_root
        self._cache_limits = cache_limits
        self._cache_clock = cache_clock
        self._legacy_render_nonce = uuid.uuid4().hex
        self._cache: WindowsRuntimeCache | None = None
        self._cached_layout_leases: list[CachedLayoutLease] = []
        self._cached_layout_by_path: dict[str, CachedLayoutLease] = {}
        self.launch_build_directory: Path | None = None
        self.launch_source_directory: Path | None = None
        self.launch_runtime_directory: Path | None = None
        self.launch_runtime_directories: tuple[Path, ...] = ()
        self.launch_executable: Path | None = None
        self.staged_file_count = 0
        self.staged_byte_count = 0
        self.cache_hit_count = 0
        self.cache_miss_count = 0
        self.payload_written_file_count = 0
        self.payload_written_byte_count = 0

    @property
    def cached_layout_directories(self) -> tuple[Path, ...]:
        return tuple(lease.layout for lease in self._cached_layout_leases)

    def __enter__(self) -> RuntimeAuditLease:
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        if _type is not None:
            try:
                self.close()
            except BaseException:
                pass
        else:
            self.close()

    def hold(self, path: Path) -> BinaryIO:
        key = path.absolute()
        existing = self._streams.get(key)
        if existing is not None:
            return existing
        stream = self._open_read_locked(key)
        self._streams[key] = stream
        self._canonical_files[key] = self._final_path_from_stream(stream, key)
        return stream

    def canonical_file(self, path: Path) -> Path:
        """Hold a file and return the final handle-backed path used for launch."""
        key = path.absolute()
        self.hold(key)
        return self._canonical_files[key]

    def canonical_directory(self, path: Path) -> Path:
        """Hold a directory identity and return its final handle-backed path."""
        key = path.absolute()
        existing = self._directory_handles.get(key)
        if existing is not None:
            return existing[1]
        if not key.is_dir():
            raise FileNotFoundError(f"runtime directory does not exist: {key}")
        if os.name != "nt":
            canonical = key.resolve()
            self._directory_handles[key] = (nullcontext(), canonical)
            return canonical

        owner, canonical = self._open_directory_locked(key)
        self._directory_handles[key] = (owner, canonical)
        return canonical

    def hold_dll_tree(self, directory: Path) -> None:
        canonical = self.canonical_directory(directory)
        for child in sorted(
            canonical.rglob("*"), key=lambda value: str(value).casefold()
        ):
            is_junction = getattr(child, "is_junction", lambda: False)()
            if child.is_symlink() or is_junction:
                raise RuntimeError(
                    f"runtime DLL subtree contains a reparse alias: {child}"
                )
            if child.is_file() and child.suffix.casefold() == ".dll":
                final = self.canonical_file(child)
                if not _is_relative_to(final, canonical):
                    raise RuntimeError(
                        "runtime DLL subtree resolves outside its selected root: "
                        f"{child} -> {final}"
                    )
                if os.path.normcase(str(child)) != os.path.normcase(str(final)):
                    raise RuntimeError(
                        f"runtime DLL subtree contains a reparse alias: {child} -> {final}"
                    )

    def _runtime_cache(self) -> WindowsRuntimeCache:
        if os.name != "nt":
            raise OSError("protected launch layouts are only available on Windows")
        if self._cache is None:
            self._cache = WindowsRuntimeCache(
                self._cache_root,
                _current_user_sid(),
                self._describe_held_source,
                self._open_directory_locked,
                limits=self._cache_limits,
                clock=self._cache_clock,
            )
        return self._cache

    def _describe_held_source(self, path: Path) -> HeldSource:
        source = path.absolute()
        stream = self.hold(source)
        stat_result = os.fstat(stream.fileno())
        stream.seek(0)
        digest = hashlib.sha256()
        try:
            while True:
                payload = stream.read(1024 * 1024)
                if not payload:
                    break
                digest.update(payload)
        finally:
            stream.seek(0)
        return HeldSource(
            source=source,
            final_path=self._canonical_files[source],
            device=int(stat_result.st_dev),
            inode=int(stat_result.st_ino),
            size=int(stat_result.st_size),
            sha256=digest.hexdigest(),
            stream=stream,
        )

    def _describe_held_sources(self, paths: Sequence[Path]) -> dict[Path, HeldSource]:
        unique = tuple(dict.fromkeys(path.absolute() for path in paths))
        for path in unique:
            self.hold(path)
        if len(unique) >= 32:
            resolved = _draining_map(
                _parallel_executor(), self._describe_held_source, unique
            )
        else:
            resolved = tuple(self._describe_held_source(path) for path in unique)
        cache = self._runtime_cache()
        return {
            path: cache._accept_resolved_source(descriptor)
            for path, descriptor in zip(unique, resolved, strict=True)
        }

    @staticmethod
    def _layout_path_key(path: Path) -> str:
        return os.path.normcase(os.path.abspath(path))

    def _record_cached_layout(self, lease: CachedLayoutLease) -> Path:
        self._cached_layout_leases.append(lease)
        self._cached_layout_by_path[self._layout_path_key(lease.layout)] = lease
        self.staged_file_count += lease.logical_files
        self.staged_byte_count += lease.logical_bytes
        self.cache_hit_count += int(lease.cache_hit)
        self.cache_miss_count += int(not lease.cache_hit)
        self.payload_written_file_count += lease.payload_files
        self.payload_written_byte_count += lease.payload_bytes
        return lease.layout

    def stage_readonly_tree(
        self,
        directory: Path,
        *,
        entry_limit: int = 4096,
        byte_limit: int = 256 << 20,
        excluded: tuple[Path, ...] = (),
        included_suffixes: tuple[str, ...] | None = None,
        included_names: tuple[str, ...] = (),
    ) -> Path:
        """Snapshot a runtime tree and deny mutation for the child lifetime."""
        source = self.canonical_directory(directory)
        if os.name != "nt":
            self.hold_dll_tree(source)
            return source
        if type(entry_limit) is not int or entry_limit < 0:
            raise RuntimeError(
                "runtime cache entry limit must be a nonnegative integer"
            )
        if type(byte_limit) is not int or byte_limit < 0:
            raise RuntimeError(
                "runtime cache byte limit must be a nonnegative integer"
            )

        excluded_canonical = tuple(Path(os.path.abspath(path)) for path in excluded)

        def is_excluded(path: Path) -> bool:
            absolute = Path(os.path.abspath(path))
            return any(
                absolute == item or _is_relative_to(absolute, item)
                for item in excluded_canonical
            )

        suffixes = (
            {suffix.casefold() for suffix in included_suffixes}
            if included_suffixes is not None
            else None
        )
        names = {name.casefold() for name in included_names}
        entries: list[tuple[Path, HeldSource]] = []
        total_size = 0
        for current_text, directory_names, file_names in os.walk(source):
            current = Path(current_text)
            retained_directories: list[str] = []
            for name in directory_names:
                child = current / name
                if is_excluded(child):
                    continue
                is_junction = getattr(child, "is_junction", lambda: False)()
                if child.is_symlink() or is_junction:
                    raise RuntimeError(
                        f"protected tree contains a reparse alias: {child}"
                    )
                retained_directories.append(name)
            directory_names[:] = retained_directories
            for name in file_names:
                entry = current / name
                if is_excluded(entry):
                    continue
                if suffixes is not None and (
                    entry.suffix.casefold() not in suffixes
                    and entry.name.casefold() not in names
                ):
                    continue
                if entry.is_symlink():
                    raise RuntimeError(
                        f"protected tree contains a reparse alias: {entry}"
                    )
                if not entry.is_file():
                    raise RuntimeError(
                        f"protected tree contains unsupported entry: {entry}"
                    )
                if len(entries) >= entry_limit:
                    raise RuntimeError(
                        "runtime cache entry limit exceeded: "
                        f"more than {entry_limit} entries"
                    )
                stream = self.hold(entry)
                total_size += int(os.fstat(stream.fileno()).st_size)
                if total_size > byte_limit:
                    raise RuntimeError(
                        "runtime cache byte limit exceeded: "
                        f"{total_size} > {byte_limit}"
                    )
                descriptor = self._runtime_cache().describe_source(entry)
                final = descriptor.final_path
                if not _is_relative_to(final, source) or os.path.normcase(
                    str(entry)
                ) != os.path.normcase(str(final)):
                    raise RuntimeError(
                        "runtime QML tree contains a reparse alias: "
                        f"{entry} -> {final}"
                    )
                entries.append((entry.relative_to(source), descriptor))
        entries.sort(key=lambda pair: str(pair[0]).casefold())

        context: dict[str, JsonValue] = {
            "policy": "readonly-tree-cache-v1",
            "source_final_path": source.as_posix(),
            "excluded": tuple(
                sorted(path.as_posix() for path in excluded_canonical)
            ),
            "included_suffixes": (
                None if suffixes is None else tuple(sorted(suffixes))
            ),
            "included_names": tuple(sorted(names)),
            "entry_limit": entry_limit,
            "byte_limit": byte_limit,
        }
        lease = self._runtime_cache().materialize(
            source.name,
            tuple(entries),
            context=context,
            entry_limit=entry_limit,
            byte_limit=byte_limit,
        )
        return self._record_cached_layout(lease)

    def stage_readonly_layout(
        self,
        name: str,
        entries: (
            Mapping[Path, Path | bytes | Callable[[Path], bytes] | GeneratedLayoutEntry]
            | Sequence[
                tuple[Path, Path | bytes | Callable[[Path], bytes] | GeneratedLayoutEntry]
            ]
        ),
        *,
        context: Mapping[str, JsonValue] | None = None,
        dependencies: Sequence[Path] = (),
        qt_paths: QtConfPaths | None = None,
        entry_limit: int = 8192,
        byte_limit: int = 1 << 30,
    ) -> Path:
        """Materialize one immutable cached tree from held source descriptors."""
        if os.name != "nt":
            raise OSError("protected launch layouts are only available on Windows")
        if type(entry_limit) is not int or entry_limit < 0:
            raise RuntimeError(
                "runtime cache entry limit must be a nonnegative integer"
            )
        if type(byte_limit) is not int or byte_limit < 0:
            raise RuntimeError(
                "runtime cache byte limit must be a nonnegative integer"
            )
        if len(entries) > entry_limit:
            raise RuntimeError(
                "runtime cache entry limit exceeded: "
                f"more than {entry_limit} entries"
            )

        pairs = tuple(entries.items()) if isinstance(entries, Mapping) else tuple(entries)
        described: list[tuple[Path, HeldSource | bytes | GeneratedLayoutEntry]] = []
        known_bytes = 0
        path_sources: list[Path] = []
        for _relative, source in pairs:
            if isinstance(source, Path):
                stream = self.hold(source)
                known_bytes += int(os.fstat(stream.fileno()).st_size)
                path_sources.append(source)
            elif isinstance(source, bytes):
                known_bytes += len(source)
            elif not isinstance(source, GeneratedLayoutEntry) and not callable(source):
                raise RuntimeError(
                    "protected layout contains unsupported source: "
                    f"{type(source).__name__}"
                )
            if known_bytes > byte_limit:
                raise RuntimeError(
                    "runtime cache byte limit exceeded: "
                    f"{known_bytes} > {byte_limit}"
                )
        held_sources = self._describe_held_sources(path_sources)
        for relative, source in pairs:
            if isinstance(source, Path):
                entry: HeldSource | bytes | GeneratedLayoutEntry = (
                    held_sources[source.absolute()]
                )
            elif isinstance(source, bytes):
                entry = source
            elif isinstance(source, GeneratedLayoutEntry):
                entry = source
            elif callable(source):
                entry = GeneratedLayoutEntry(
                    "legacy-layout-render-v1",
                    {
                        "destination": Path(relative).as_posix(),
                        "lease_nonce": self._legacy_render_nonce,
                    },
                    source,
                )
            else:
                raise AssertionError("unsupported layout source escaped validation")
            described.append((Path(relative), entry))

        dependency_leases: list[CachedLayoutLease] = []
        for dependency in dependencies:
            dependency_lease = self._cached_layout_by_path.get(
                self._layout_path_key(dependency)
            )
            if dependency_lease is None:
                raise RuntimeError(
                    "protected layout dependency is not a registered cached layout: "
                    f"{dependency}"
                )
            dependency_leases.append(dependency_lease)

        lease = self._runtime_cache().materialize(
            name,
            tuple(described),
            context={} if context is None else context,
            dependencies=tuple(dependency_leases),
            qt_paths=qt_paths,
            entry_limit=entry_limit,
            byte_limit=byte_limit,
        )
        return self._record_cached_layout(lease)

    def close(self) -> None:
        streams = tuple(reversed(tuple(self._streams.values())))
        directory_handles = tuple(
            reversed(tuple(owner for owner, _path in self._directory_handles.values()))
        )
        cached_layouts = tuple(reversed(tuple(self._cached_layout_leases)))
        cache = self._cache
        self._streams.clear()
        self._canonical_files.clear()
        self._directory_handles.clear()
        self._cached_layout_leases.clear()
        self._cached_layout_by_path.clear()
        self._cache = None
        self.launch_build_directory = None
        self.launch_source_directory = None
        self.launch_runtime_directory = None
        self.launch_runtime_directories = ()
        self.launch_executable = None
        first_error: BaseException | None = None
        for cached_layout in cached_layouts:
            try:
                cached_layout.close()
            except BaseException as error:
                if first_error is None:
                    first_error = error
        for stream in streams:
            try:
                stream.close()
            except BaseException as error:
                if first_error is None:
                    first_error = error
        for owner in directory_handles:
            try:
                owner.close()
            except BaseException as error:
                if first_error is None:
                    first_error = error
        if cache is not None:
            try:
                cache.close()
            except BaseException as error:
                if first_error is None:
                    first_error = error
        if first_error is not None:
            raise first_error

    @staticmethod
    def _open_read_locked(path: Path) -> BinaryIO:
        if os.name != "nt":
            return path.open("rb")

        import msvcrt

        kernel32 = _runtime_kernel32()
        handle = kernel32.CreateFileW(
            str(path),
            0x80000000,  # GENERIC_READ
            0x00000001,  # FILE_SHARE_READ: deny replacement/write/delete
            None,
            3,  # OPEN_EXISTING
            0x08000000,  # FILE_FLAG_SEQUENTIAL_SCAN
            None,
        )
        invalid_handle = ctypes.c_void_p(-1).value
        if handle == invalid_handle:
            error = ctypes.get_last_error()
            raise OSError(error, f"cannot hold audited runtime file {path}")
        try:
            descriptor = msvcrt.open_osfhandle(
                int(handle), os.O_RDONLY | getattr(os, "O_BINARY", 0)
            )
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
        return os.fdopen(descriptor, "rb", closefd=True)

    @staticmethod
    def _strip_extended_prefix(value: str) -> Path:
        if value.startswith("\\\\?\\UNC\\"):
            value = "\\\\" + value[8:]
        elif value.startswith("\\\\?\\"):
            value = value[4:]
        return Path(value)

    @classmethod
    def _final_path_from_stream(cls, stream: BinaryIO, fallback: Path) -> Path:
        if os.name != "nt":
            return fallback.resolve()
        import msvcrt

        handle = msvcrt.get_osfhandle(stream.fileno())
        return cls._final_path_from_handle(handle)

    @classmethod
    def _final_path_from_handle(cls, handle: int) -> Path:
        kernel32 = _runtime_kernel32()
        size = int(kernel32.GetFinalPathNameByHandleW(handle, None, 0, 0))
        if not size:
            raise ctypes.WinError(ctypes.get_last_error())
        buffer = ctypes.create_unicode_buffer(size + 1)
        written = int(
            kernel32.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
        )
        if not written or written >= len(buffer):
            raise ctypes.WinError(ctypes.get_last_error())
        return cls._strip_extended_prefix(buffer.value)

    @classmethod
    def _open_directory_locked(cls, path: Path) -> tuple[object, Path]:
        kernel32 = _runtime_kernel32()
        handle = kernel32.CreateFileW(
            str(path),
            0x00000080,  # FILE_READ_ATTRIBUTES
            0x00000001 | 0x00000002,  # share read/write, deny delete/retarget
            None,
            3,
            0x02000000,  # FILE_FLAG_BACKUP_SEMANTICS
            None,
        )
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())

        class _Owner:
            def __init__(self, api: object, value: int) -> None:
                self._api = api
                self._value = value

            def close(self) -> None:
                if self._value:
                    self._api.CloseHandle(self._value)
                    self._value = 0

        try:
            canonical = cls._final_path_from_handle(int(handle))
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
        return _Owner(kernel32, int(handle)), canonical


def has_embedded_application_manifest(path: Path) -> bool:
    """Return whether PE resource ID 1 is a valid embedded RT_MANIFEST."""
    if os.name != "nt":
        raise OSError("embedded Windows manifests can only be inspected on Windows")

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryExW.argtypes = (
        ctypes.c_wchar_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
    )
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    kernel32.FindResourceW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    )
    kernel32.FindResourceW.restype = ctypes.c_void_p
    kernel32.SizeofResource.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    kernel32.SizeofResource.restype = ctypes.c_uint32
    kernel32.LoadResource.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    kernel32.LoadResource.restype = ctypes.c_void_p
    kernel32.LockResource.argtypes = (ctypes.c_void_p,)
    kernel32.LockResource.restype = ctypes.c_void_p
    kernel32.FreeLibrary.argtypes = (ctypes.c_void_p,)
    kernel32.FreeLibrary.restype = ctypes.c_int

    module = kernel32.LoadLibraryExW(
        str(path),
        None,
        0x00000040 | 0x00000020,  # AS_DATAFILE_EXCLUSIVE | AS_IMAGE_RESOURCE
    )
    if not module:
        error = ctypes.get_last_error()
        raise ValueError(
            f"cannot inspect embedded PE resources for {path}: Windows error {error}"
        )
    try:
        resource = kernel32.FindResourceW(
            module,
            ctypes.c_void_p(1),
            ctypes.c_void_p(24),  # RT_MANIFEST
        )
        if not resource:
            error = ctypes.get_last_error()
            if error in (0, 1812, 1813, 1814, 1815):
                return False
            raise ValueError(
                f"cannot inspect embedded PE resource for {path}: Windows error {error}"
            )
        size = int(kernel32.SizeofResource(module, resource))
        loaded = kernel32.LoadResource(module, resource)
        locked = kernel32.LockResource(loaded) if loaded else None
        if not size or not locked:
            raise ValueError(f"malformed embedded PE resource for {path}")
        contents = ctypes.string_at(locked, size)
        try:
            root = ET.fromstring(contents)
        except ET.ParseError as error:
            raise ValueError(
                f"invalid embedded application manifest XML for {path}: {error}"
            ) from error
        if root.tag.rsplit("}", 1)[-1] != "assembly":
            raise ValueError(
                f"invalid embedded application manifest XML root for {path}"
            )
        if root.attrib.get("manifestVersion") != "1.0":
            raise ValueError(
                f"invalid embedded application manifest XML version for {path}"
            )
        return True
    finally:
        kernel32.FreeLibrary(module)


def reject_dotlocal_dev_override() -> None:
    """Fail if machine policy can override manifest-based DotLocal isolation."""
    if os.name != "nt":
        return
    import winreg

    subkey = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
    views = (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY)
    for view in views:
        try:
            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                subkey,
                0,
                access=winreg.KEY_READ | view,
            ) as key:
                try:
                    value, value_type = winreg.QueryValueEx(
                        key, "DevOverrideEnable"
                    )
                except FileNotFoundError:
                    continue
        except FileNotFoundError:
            continue
        except OSError as error:
            raise RuntimeError(
                "cannot read HKLM IFEO DevOverrideEnable DotLocal policy: "
                f"{error}"
            ) from error
        if value_type != winreg.REG_DWORD:
            raise RuntimeError(
                "HKLM IFEO DevOverrideEnable has unsafe registry type; "
                "remove the value or set a REG_DWORD value of 0"
            )
        if value != 0:
            raise RuntimeError(
                "HKLM IFEO DevOverrideEnable is enabled; embedded manifests no "
                "longer disable DotLocal redirection. Set both registry views to 0."
            )


_FFMPEG_FAMILY = re.compile(
    r"^(avcodec|avdevice|avfilter|avformat|avutil|postproc|swresample|swscale)-\d+\.dll$",
    re.IGNORECASE,
)
_QT_FAMILY = re.compile(r"^qt\d+([a-z0-9_]+)\.dll$", re.IGNORECASE)
_SRT_FAMILY = re.compile(r"^(?:lib)?srt(?:-[a-z0-9_.-]+)?\.dll$", re.IGNORECASE)
_TOOLCHAIN_FAMILY = re.compile(
    r"^(lib(?:atomic|gcc_[a-z0-9_]+|gfortran|gomp|quadmath|stdc\+\+|winpthread))(?:-\d+)?\.dll$",
    re.IGNORECASE,
)


def _controlled_family(name: str) -> tuple[str, str] | None:
    lowered = name.casefold()
    match = _FFMPEG_FAMILY.fullmatch(lowered)
    if match:
        return "OLR_FFMPEG_ROOT", match.group(1).casefold()
    match = _QT_FAMILY.fullmatch(lowered)
    if match:
        return "Qt6_DIR", match.group(1).casefold()
    if _SRT_FAMILY.fullmatch(lowered):
        return "OLR_SRT_ROOT", "srt"
    match = _TOOLCHAIN_FAMILY.fullmatch(lowered)
    if match:
        return "CMAKE_CXX_COMPILER", match.group(1).casefold()
    return None


class _VsFixedFileInfo(ctypes.Structure):
    _fields_ = [
        ("signature", ctypes.c_uint32),
        ("structure_version", ctypes.c_uint32),
        ("file_version_ms", ctypes.c_uint32),
        ("file_version_ls", ctypes.c_uint32),
        ("product_version_ms", ctypes.c_uint32),
        ("product_version_ls", ctypes.c_uint32),
        ("file_flags_mask", ctypes.c_uint32),
        ("file_flags", ctypes.c_uint32),
        ("file_os", ctypes.c_uint32),
        ("file_type", ctypes.c_uint32),
        ("file_subtype", ctypes.c_uint32),
        ("file_date_ms", ctypes.c_uint32),
        ("file_date_ls", ctypes.c_uint32),
    ]


def _file_version(path: Path) -> str:
    if os.name != "nt":
        return "unknown"
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = (ctypes.c_wchar_p, ctypes.c_void_p)
    version.GetFileVersionInfoSizeW.restype = ctypes.c_uint32
    version.GetFileVersionInfoW.argtypes = (
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    )
    version.GetFileVersionInfoW.restype = ctypes.c_int
    version.VerQueryValueW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint32),
    )
    version.VerQueryValueW.restype = ctypes.c_int

    size = version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return "unknown"
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, buffer):
        return "unknown"
    value = ctypes.c_void_p()
    length = ctypes.c_uint32()
    if not version.VerQueryValueW(buffer, "\\", ctypes.byref(value), ctypes.byref(length)):
        return "unknown"
    if length.value < ctypes.sizeof(_VsFixedFileInfo):
        return "unknown"
    info = ctypes.cast(value, ctypes.POINTER(_VsFixedFileInfo)).contents
    if info.signature != 0xFEEF04BD:
        return "unknown"
    return ".".join(
        str(component)
        for component in (
            info.file_version_ms >> 16,
            info.file_version_ms & 0xFFFF,
            info.file_version_ls >> 16,
            info.file_version_ls & 0xFFFF,
        )
    )


def _sha256(path: Path, *, stream: BinaryIO | None = None) -> str:
    digest = hashlib.sha256()
    previous = stream.tell() if stream is not None else None
    with (path.open("rb") if stream is None else nullcontext(stream)) as source:
        source.seek(0)
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    if stream is not None and previous is not None:
        stream.seek(previous)
    return digest.hexdigest()


def _describe(path: Path, *, stream: BinaryIO | None = None) -> str:
    return (
        f"{path} (version {_file_version(path)}, "
        f"sha256 {_sha256(path, stream=stream)})"
    )


def pe_imports(path: Path, *, stream: BinaryIO | None = None) -> set[str]:
    """Return normal and delay-load PE import basenames without loading the file."""

    try:
        previous = stream.tell() if stream is not None else None
        with (path.open("rb") if stream is None else nullcontext(stream)) as source:
            stream = source
            stream.seek(0)
            file_size = os.fstat(stream.fileno()).st_size

            def read_at(offset: int, size: int) -> bytes:
                if offset < 0 or size < 0 or offset + size > file_size:
                    raise ValueError("truncated PE field")
                stream.seek(offset)
                value = stream.read(size)
                if len(value) != size:
                    raise ValueError("truncated PE field")
                return value

            def u16(offset: int) -> int:
                return struct.unpack("<H", read_at(offset, 2))[0]

            def u32(offset: int) -> int:
                return struct.unpack("<I", read_at(offset, 4))[0]

            if file_size < 64 or read_at(0, 2) != b"MZ":
                return set()
            pe_offset = u32(0x3C)
            if pe_offset + 24 > file_size or read_at(pe_offset, 4) != b"PE\0\0":
                return set()
            section_count = u16(pe_offset + 6)
            optional_size = u16(pe_offset + 20)
            optional = pe_offset + 24
            optional_end = optional + optional_size
            if optional_end > file_size:
                raise ValueError("truncated PE optional header")
            if optional_size < 2:
                raise ValueError("truncated PE optional header magic")
            magic = u16(optional)
            if magic == 0x20B:
                directory_count_offset = optional + 108
                directories_offset = optional + 112
                image_base_offset = optional + 24
                image_base_size = 8
            elif magic == 0x10B:
                directory_count_offset = optional + 92
                directories_offset = optional + 96
                image_base_offset = optional + 28
                image_base_size = 4
            else:
                return set()
            if directory_count_offset + 4 > optional_end:
                raise ValueError("PE data directories exceed optional header")
            if image_base_offset + image_base_size > optional_end:
                raise ValueError("PE image base exceeds optional header")
            directory_count = u32(directory_count_offset)
            available_directories = max(0, (optional_end - directories_offset) // 8)
            if directory_count > available_directories:
                raise ValueError("PE data directories exceed optional header")
            image_base = (
                struct.unpack("<Q", read_at(image_base_offset, 8))[0]
                if image_base_size == 8
                else u32(image_base_offset)
            )

            def data_directory(index: int) -> tuple[int, int]:
                if index >= directory_count:
                    return 0, 0
                offset = directories_offset + index * 8
                if offset + 8 > optional_end:
                    raise ValueError("PE data directories exceed optional header")
                return struct.unpack("<II", read_at(offset, 8))

            sections_offset = optional_end
            sections: list[tuple[int, int, int]] = []
            for index in range(section_count):
                section = sections_offset + index * 40
                if section + 40 > file_size:
                    raise ValueError("truncated PE section table")
                virtual_address = u32(section + 12)
                raw_size = u32(section + 16)
                raw_offset = u32(section + 20)
                if raw_size and raw_offset + raw_size > file_size:
                    raise ValueError("truncated PE raw section data")
                sections.append((virtual_address, raw_size, raw_offset))

            def rva_location(rva: int, size: int = 1) -> tuple[int, int]:
                if rva < 0 or size < 0 or rva + size > 0x100000000:
                    raise ValueError(f"PE RVA is outside raw section data: 0x{rva:x}")
                for virtual_address, raw_size, raw_offset in sections:
                    delta = rva - virtual_address
                    if 0 <= delta < raw_size and size <= raw_size - delta:
                        return raw_offset + delta, raw_size - delta
                raise ValueError(f"PE RVA is outside raw section data: 0x{rva:x}")

            def c_string(rva: int) -> str:
                offset, available = rva_location(rva)
                value = read_at(offset, min(1025, available))
                end = value.find(b"\0")
                if end < 0:
                    raise ValueError("unterminated PE import name")
                return value[:end].decode("ascii", errors="strict")

            imports: set[str] = set()
            import_rva, import_size = data_directory(1)
            if import_rva or import_size:
                if not import_rva or import_size < 20:
                    raise ValueError("malformed PE import descriptor directory")
                if import_size // 20 > 65536:
                    raise ValueError("excessive PE import descriptor count")
                terminated = False
                for offset in range(0, import_size - 19, 20):
                    descriptor, _available = rva_location(import_rva + offset, 20)
                    fields = struct.unpack("<IIIII", read_at(descriptor, 20))
                    if not any(fields):
                        terminated = True
                        break
                    imports.add(c_string(fields[3]).casefold())
                if not terminated:
                    raise ValueError(
                        "PE import descriptor has no terminator inside declared size"
                    )

            delay_rva, delay_size = data_directory(13)
            if delay_rva or delay_size:
                if not delay_rva or delay_size < 32:
                    raise ValueError("malformed PE delay-import descriptor directory")
                if delay_size // 32 > 65536:
                    raise ValueError("excessive PE delay-import descriptor count")
                terminated = False
                for offset in range(0, delay_size - 31, 32):
                    descriptor, _available = rva_location(delay_rva + offset, 32)
                    fields = struct.unpack("<IIIIIIII", read_at(descriptor, 32))
                    if not any(fields):
                        terminated = True
                        break
                    attributes, name_value = fields[0], fields[1]
                    if attributes == 1:
                        name_rva = name_value
                    elif attributes == 0:
                        if name_value < image_base:
                            raise ValueError(
                                "PE delay-import VA is below image base"
                            )
                        name_rva = name_value - image_base
                        if name_rva > 0xFFFFFFFF:
                            raise ValueError("PE delay-import VA exceeds RVA range")
                    else:
                        raise ValueError(
                            "unsupported PE delay-import descriptor attributes: "
                            f"0x{attributes:x}"
                        )
                    imports.add(c_string(name_rva).casefold())
                if not terminated:
                    raise ValueError(
                        "PE delay-import descriptor has no terminator inside declared size"
                    )
            return imports
    except OSError as error:
        raise OSError(f"cannot inspect PE imports for {path}: {error}") from error
    finally:
        if stream is not None and previous is not None:
            stream.seek(previous)


def transitive_runtime_closure(
    seeds: Iterable[Path], runtime_directories: tuple[Path, ...]
) -> tuple[Path, ...]:
    """Resolve the configured PE import closure in launcher PATH order."""
    runtime_index: dict[str, Path] = {}
    for directory in runtime_directories:
        for child in sorted(directory.iterdir(), key=lambda path: path.name.casefold()):
            if child.is_file() and child.suffix.casefold() == ".dll":
                runtime_index.setdefault(child.name.casefold(), child.resolve())
    selected: set[Path] = set()
    visited: set[Path] = set()
    pending = [path.resolve() for path in seeds if path.is_file()]
    while pending:
        binary = pending.pop()
        if binary in visited:
            continue
        visited.add(binary)
        try:
            imports = pe_imports(binary)
        except ValueError as error:
            raise RuntimeError(f"cannot resolve PE runtime closure for {binary}: {error}") from error
        for name in imports:
            dependency = runtime_index.get(name.casefold())
            if dependency is None or dependency in selected:
                continue
            selected.add(dependency)
            pending.append(dependency)
    return tuple(sorted(selected, key=lambda path: str(path).casefold()))


def qt_plugin_closure(
    qt_plugins: Path,
    families: tuple[str, ...] = (
        "iconengines",
        "imageformats",
        "multimedia",
        "networkinformation",
        "styles",
        "tls",
    ),
    *,
    platforms: tuple[str, ...] = ("qoffscreen.dll",),
    lease: RuntimeAuditLease | None = None,
) -> tuple[Path, ...]:
    """Select the explicit plugin families used by the headless/app launchers."""
    if os.name == "nt" and lease is None:
        raise RuntimeError("Qt plugin closure requires a runtime audit lease")
    lexical_plugins = Path(os.path.abspath(qt_plugins))
    qt_plugins = (
        lease.canonical_directory(lexical_plugins)
        if lease is not None
        else lexical_plugins.resolve()
    )

    def held_directory(path: Path) -> Path:
        lexical = Path(os.path.abspath(path))
        canonical = (
            lease.canonical_directory(lexical)
            if lease is not None
            else lexical.resolve()
        )
        if (
            not canonical.is_relative_to(qt_plugins)
            or os.path.normcase(str(lexical))
            != os.path.normcase(str(canonical))
        ):
            raise RuntimeError(
                f"Qt plugin closure contains a reparse alias: {lexical} -> {canonical}"
            )
        return canonical

    def held_file(path: Path) -> Path:
        lexical = Path(os.path.abspath(path))
        canonical = (
            lease.canonical_file(lexical)
            if lease is not None
            else lexical.resolve()
        )
        if (
            not canonical.is_relative_to(qt_plugins)
            or os.path.normcase(str(lexical))
            != os.path.normcase(str(canonical))
        ):
            raise RuntimeError(
                f"Qt plugin closure contains a reparse alias: {lexical} -> {canonical}"
            )
        return canonical

    selected: set[Path] = set()
    for platform_name in platforms:
        platform = qt_plugins / "platforms" / platform_name
        if platform.is_file():
            selected.add(held_file(platform))
    for family in families:
        directory = qt_plugins / family
        if not directory.is_dir():
            continue
        directory = held_directory(directory)
        with os.scandir(directory) as scanned:
            entries = tuple(scanned)
        for entry in entries:
            stat_result = entry.stat(follow_symlinks=False)
            if (
                getattr(stat_result, "st_file_attributes", 0)
                & _FILE_ATTRIBUTE_REPARSE_POINT
            ):
                raise RuntimeError(
                    f"Qt plugin closure contains a reparse alias: {entry.path}"
                )
            if entry.is_file(follow_symlinks=False) and Path(
                entry.name
            ).suffix.casefold() == ".dll":
                selected.add(held_file(Path(entry.path)))
    return tuple(sorted(selected, key=lambda path: str(path).casefold()))


_QML_RUNTIME_SUFFIXES = frozenset(
    {".dll", ".qml", ".js", ".mjs", ".qmltypes"}
)
_FILE_ATTRIBUTE_REPARSE_POINT = 0x400
_QML_IMPORT = re.compile(
    r"(?m)^\s*import\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)"
)
_QMLDIR_DEPENDENCY = re.compile(
    r"(?m)^\s*(?:depends|import)\s+"
    r"([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)"
)


def qt_qml_closure(
    qt_qml: Path,
    source_roots: Iterable[Path],
    *,
    lease: RuntimeAuditLease | None = None,
) -> tuple[Path, ...]:
    """Resolve Qt module directories imported by project/QuickTest QML sources."""
    if os.name == "nt" and lease is None:
        raise RuntimeError("Qt QML closure requires a runtime audit lease")
    lexical_qt_qml = Path(os.path.abspath(qt_qml))
    qt_qml = (
        lease.canonical_directory(lexical_qt_qml)
        if lease is not None
        else lexical_qt_qml.resolve()
    )
    pending: set[str] = set()
    excluded_names = {
        ".claude", ".git", ".venv", "__pycache__", "build", "build-tests",
        "linux_build", "macos_build", "venv", "windows_build",
    }

    def imports_from(path: Path) -> set[str]:
        if path.suffix.casefold() not in (".qml", ".js", ".mjs") and (
            path.name.casefold() != "qmldir"
        ):
            return set()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return set()
        found = set(_QML_IMPORT.findall(text))
        if path.name.casefold() == "qmldir":
            found.update(_QMLDIR_DEPENDENCY.findall(text))
        return found

    for root in source_roots:
        if not root.exists():
            continue
        candidates: Iterable[Path]
        if root.is_file():
            candidates = (root,)
        else:
            files: list[Path] = []
            for current_text, directory_names, file_names in os.walk(root):
                directory_names[:] = [
                    name for name in directory_names
                    if name.casefold() not in excluded_names
                ]
                current = Path(current_text)
                files.extend(
                    current / name
                    for name in file_names
                    if Path(name).suffix.casefold() in (".qml", ".js", ".mjs")
                    or name.casefold() == "qmldir"
                )
            candidates = files
        for candidate in candidates:
            pending.update(imports_from(candidate))

    def scandir_entries(directory: Path) -> tuple[os.DirEntry[str], ...]:
        with os.scandir(directory) as scanned:
            return tuple(scanned)

    def reject_reparse(entry: os.DirEntry[str]) -> None:
        stat_result = entry.stat(follow_symlinks=False)
        if (
            getattr(stat_result, "st_file_attributes", 0)
            & _FILE_ATTRIBUTE_REPARSE_POINT
        ):
            raise RuntimeError(
                f"Qt QML closure contains a reparse alias: {entry.path}"
            )

    def held_directory(path: Path) -> Path:
        lexical = Path(os.path.abspath(path))
        canonical = (
            lease.canonical_directory(lexical)
            if lease is not None
            else lexical.resolve()
        )
        if (
            not canonical.is_relative_to(qt_qml)
            or os.path.normcase(str(lexical))
            != os.path.normcase(str(canonical))
        ):
            raise RuntimeError(
                f"Qt QML closure contains a reparse alias: {lexical} -> {canonical}"
            )
        return canonical

    def held_file(path: Path) -> Path:
        lexical = Path(os.path.abspath(path))
        canonical = (
            lease.canonical_file(lexical)
            if lease is not None
            else lexical.resolve()
        )
        if (
            not canonical.is_relative_to(qt_qml)
            or os.path.normcase(str(lexical))
            != os.path.normcase(str(canonical))
        ):
            raise RuntimeError(
                f"Qt QML closure contains a reparse alias: {lexical} -> {canonical}"
            )
        return canonical

    selected: set[Path] = set()
    for root_entry in scandir_entries(qt_qml):
        reject_reparse(root_entry)
        if root_entry.is_file(follow_symlinks=False):
            root_file = Path(root_entry.path)
            if (
                root_file.suffix.casefold() in _QML_RUNTIME_SUFFIXES
                or root_file.name.casefold() == "qmldir"
            ):
                selected.add(held_file(root_file))

    visited: set[str] = set()
    while pending:
        uri = pending.pop()
        if uri in visited:
            continue
        visited.add(uri)
        module = qt_qml.joinpath(*uri.split("."))
        if not module.is_dir():
            continue
        directories = [held_directory(module)]
        while directories:
            directory = directories.pop()
            for child_entry in scandir_entries(directory):
                reject_reparse(child_entry)
                child = Path(child_entry.path)
                if child_entry.is_dir(follow_symlinks=False):
                    directories.append(held_directory(child))
                elif child_entry.is_file(follow_symlinks=False):
                    canonical_child = held_file(child)
                    selected.add(canonical_child)
                    pending.update(imports_from(canonical_child))
                else:
                    raise RuntimeError(
                        f"Qt QML closure contains an invalid entry: {child}"
                    )
    return tuple(sorted(selected, key=lambda path: str(path).casefold()))


class CMakeArgumentOccurrence(NamedTuple):
    value: str
    start: int
    end: int
    style: str


def _cmake_command_argument_occurrences(
    text: str, command_name: str
) -> tuple[tuple[int, tuple[CMakeArgumentOccurrence, ...]], ...]:
    """Parse literal arguments from generated CMake commands."""
    text = _strip_cmake_comments(text)
    commands: list[tuple[int, tuple[CMakeArgumentOccurrence, ...]]] = []
    starts: list[int] = []
    scan = 0
    while scan < len(text):
        if text[scan] == '"':
            scan += 1
            while scan < len(text):
                if text[scan] == "\\" and scan + 1 < len(text):
                    scan += 2
                    continue
                if text[scan] == '"':
                    scan += 1
                    break
                scan += 1
            continue
        bracket = re.match(r"\[(=*)\[", text[scan:])
        if bracket:
            closing = "]" + bracket.group(1) + "]"
            end = text.find(closing, scan + len(bracket.group(0)))
            scan = len(text) if end < 0 else end + len(closing)
            continue
        identifier = re.match(r"[A-Za-z_][A-Za-z0-9_]*", text[scan:])
        if identifier:
            name = identifier.group(0)
            after = scan + len(name)
            while after < len(text) and text[after].isspace():
                after += 1
            if name.casefold() == command_name.casefold() and (
                after < len(text) and text[after] == "("
            ):
                starts.append(after + 1)
            scan += len(name)
            continue
        scan += 1

    for command_start in starts:
        index = command_start
        arguments: list[CMakeArgumentOccurrence] = []
        while index < len(text):
            while index < len(text) and text[index].isspace():
                index += 1
            if index >= len(text) or text[index] == ")":
                break
            if text[index] == '"':
                token_start = index
                index += 1
                value: list[str] = []
                while index < len(text):
                    character = text[index]
                    if character == '"':
                        index += 1
                        break
                    if character == "\\" and index + 1 < len(text):
                        index += 1
                        value.append(text[index])
                    else:
                        value.append(character)
                    index += 1
                else:
                    raise ValueError(
                        f"unterminated quoted {command_name} argument"
                    )
                arguments.append(CMakeArgumentOccurrence(
                    "".join(value), token_start, index, "quoted"
                ))
                continue
            bracket = re.match(r"\[(=*)\[", text[index:])
            if bracket:
                opening = bracket.group(0)
                closing = "]" + bracket.group(1) + "]"
                value_start = index + len(opening)
                value_end = text.find(closing, value_start)
                if value_end < 0:
                    raise ValueError(
                        f"unterminated bracket {command_name} argument"
                    )
                index = value_end + len(closing)
                arguments.append(CMakeArgumentOccurrence(
                    text[value_start:value_end],
                    value_start - len(opening),
                    index,
                    "bracket",
                ))
                continue
            value_start = index
            while (
                index < len(text)
                and not text[index].isspace()
                and text[index] != ")"
            ):
                index += 1
            arguments.append(CMakeArgumentOccurrence(
                text[value_start:index], value_start, index, "bare"
            ))
        else:
            raise ValueError(f"unterminated {command_name} command")
        if index >= len(text) or text[index] != ")":
            raise ValueError(f"unterminated {command_name} command")
        commands.append((command_start, tuple(arguments)))
    return tuple(commands)


def _cmake_command_arguments_with_positions(
    text: str, command_name: str
) -> tuple[tuple[int, tuple[str, ...]], ...]:
    return tuple(
        (position, tuple(argument.value for argument in arguments))
        for position, arguments in _cmake_command_argument_occurrences(
            text, command_name
        )
    )


def _cmake_commands_in_order(
    text: str, command_names: tuple[str, ...]
) -> tuple[
    tuple[str, tuple[str, ...], tuple[CMakeArgumentOccurrence, ...]], ...
]:
    commands = [
        (
            position,
            command_name,
            tuple(argument.value for argument in occurrences),
            occurrences,
        )
        for command_name in command_names
        for position, occurrences in _cmake_command_argument_occurrences(
            text, command_name
        )
    ]
    commands.sort(key=lambda command: command[0])
    return tuple(
        (name, arguments, occurrences)
        for _position, name, arguments, occurrences in commands
    )


def _strip_cmake_comments(text: str) -> str:
    """Mask comments while preserving argument source offsets."""
    output: list[str] = []

    def masked(value: str) -> str:
        return "".join(character if character in "\r\n" else " " for character in value)

    index = 0
    while index < len(text):
        if text[index] == '"':
            start = index
            index += 1
            while index < len(text):
                if text[index] == "\\" and index + 1 < len(text):
                    index += 2
                    continue
                if text[index] == '"':
                    index += 1
                    break
                index += 1
            output.append(text[start:index])
            continue
        bracket = re.match(r"\[(=*)\[", text[index:])
        if bracket:
            closing = "]" + bracket.group(1) + "]"
            end = text.find(closing, index + len(bracket.group(0)))
            if end < 0:
                output.append(text[index:])
                break
            end += len(closing)
            output.append(text[index:end])
            index = end
            continue
        if text[index] == "#":
            bracket_comment = re.match(r"#\[(=*)\[", text[index:])
            if bracket_comment:
                closing = "]" + bracket_comment.group(1) + "]"
                end = text.find(
                    closing, index + len(bracket_comment.group(0))
                )
                comment_end = len(text) if end < 0 else end + len(closing)
                output.append(masked(text[index:comment_end]))
                index = comment_end
                continue
            newline = text.find("\n", index)
            if newline < 0:
                output.append(masked(text[index:]))
                break
            output.append(masked(text[index:newline + 1]))
            index = newline + 1
            continue
        output.append(text[index])
        index += 1
    return "".join(output)


def _test_name(arguments: tuple[str, ...]) -> str | None:
    if not arguments:
        return None
    if arguments[0].casefold() != "name":
        return arguments[0]
    return arguments[1] if len(arguments) > 1 else None


def _executable_argument(argument: str) -> str | None:
    if argument.startswith("-") and "=" in argument:
        value = argument.partition("=")[2]
        if value.casefold().endswith(".exe"):
            return value
    if argument.casefold().endswith(".exe"):
        return argument
    return None


class CTestRecord(NamedTuple):
    name: str
    arguments: tuple[str, ...]
    argument_occurrences: tuple[CMakeArgumentOccurrence, ...]
    definition_file: Path
    scope_file: Path
    executable_arguments: tuple[
        tuple[str, Path, CMakeArgumentOccurrence], ...
    ]
    working_directory: Path
    labels: tuple[str, ...]
    fixtures_setup: tuple[str, ...]
    fixtures_cleanup: tuple[str, ...]
    fixtures_required: tuple[str, ...]
    environment_values: tuple[tuple[str, CMakeArgumentOccurrence], ...]
    environment_modification_values: tuple[
        tuple[str, CMakeArgumentOccurrence], ...
    ]
    environment: tuple[str, ...]
    environment_modification: tuple[str, ...]


class CTestPathArgument(NamedTuple):
    definition_file: Path
    argument: str
    path: Path
    occurrence: CMakeArgumentOccurrence


class CTestGraph(NamedTuple):
    inputs: tuple[Path, ...]
    tests: tuple[CTestRecord, ...]
    executables: tuple[Path, ...]
    working_directories: tuple[Path, ...]
    path_arguments: tuple[CTestPathArgument, ...]


def _property_assignments_for_command(
    command_name: str, arguments: tuple[str, ...]
) -> tuple[tuple[tuple[str, ...], str, str, bool, tuple[int, ...]], ...]:
    assignments: list[
        tuple[tuple[str, ...], str, str, bool, tuple[int, ...]]
    ] = []
    if command_name == "set_tests_properties":
        try:
            index = next(
                value
                for value, argument in enumerate(arguments)
                if argument.casefold() == "properties"
            )
        except StopIteration:
            return ()
        tests = arguments[:index]
        properties = arguments[index + 1 :]
        for offset in range(0, len(properties) - 1, 2):
            assignments.append(
                (
                    tests,
                    properties[offset].casefold(),
                    properties[offset + 1],
                    False,
                    (index + 2 + offset,),
                )
            )
    elif command_name == "set_property":
        if not arguments or arguments[0].casefold() != "test":
            return ()
        try:
            index = next(
                value
                for value, argument in enumerate(arguments)
                if argument.casefold() == "property"
            )
        except StopIteration:
            return ()
        if index + 2 >= len(arguments):
            return ()
        append = any(
            value.casefold() in ("append", "append_string")
            for value in arguments[1:index]
        )
        append_string = any(
            value.casefold() == "append_string" for value in arguments[1:index]
        )
        tests = tuple(
            value
            for value in arguments[1:index]
            if value.casefold() not in ("append", "append_string")
        )
        assignments.append(
            (
                tests,
                arguments[index + 1].casefold(),
                ("" if append_string else ";").join(arguments[index + 2 :]),
                append,
                tuple(range(index + 2, len(arguments))),
            )
        )
    return tuple(assignments)


def _split_cmake_list(value: str) -> tuple[str, ...]:
    return tuple(item for item in value.split(";") if item)


def discover_ctest_graph(
    build_dir: Path,
    *,
    lease: RuntimeAuditLease | None = None,
) -> CTestGraph:
    """Traverse literal CTest graph edges and retain their effective runtime state."""
    owned = lease is None
    active = RuntimeAuditLease() if lease is None else lease
    try:
        lexical_build = Path(os.path.abspath(build_dir))
        canonical_build = active.canonical_directory(lexical_build)
        root = lexical_build / "CTestTestfile.cmake"
        roots = (
            [(root, False, root.parent)]
            if root.is_file()
            else [
                (path, False, path.parent)
                for path in sorted(lexical_build.rglob("CTestTestfile.cmake"))
            ]
        )
        if not roots:
            return CTestGraph((), (), (), (), ())
        seen_inputs: set[Path] = set()
        active_inputs: set[Path] = set()
        inputs: list[Path] = []
        additions: list[
            tuple[
                tuple[str, ...],
                tuple[CMakeArgumentOccurrence, ...],
                Path,
                Path,
                Path,
                tuple[str, str],
            ]
        ] = []
        property_values: dict[tuple[str, str], dict[str, list[str]]] = {}
        property_tokens: dict[
            tuple[str, str],
            dict[str, list[tuple[str, CMakeArgumentOccurrence]]],
        ] = {}
        path_arguments: list[CTestPathArgument] = []

        def evaluate(
            pending_path: Path,
            allow_missing: bool,
            execution_directory: Path,
            scope_file: Path,
        ) -> None:
            lexical = Path(os.path.abspath(pending_path))
            execution_directory = Path(os.path.abspath(execution_directory))
            try:
                lexical.relative_to(lexical_build)
            except ValueError as error:
                raise RuntimeError(
                    f"CTest graph edge points outside selected build: {lexical}"
                ) from error
            if not lexical.is_file() and allow_missing:
                raise RuntimeError(
                    "CTest legacy subdirs edge does not exist during the "
                    f"closed-graph audit: {lexical}"
                )
            if not lexical.is_file():
                raise RuntimeError(f"CTest graph input does not exist: {lexical}")
            canonical = active.canonical_file(lexical)
            try:
                canonical.relative_to(canonical_build)
            except ValueError as error:
                raise RuntimeError(
                    "CTest graph input is lexically inside the build but resolves "
                    f"outside it: {lexical} -> {canonical}"
                ) from error
            if os.path.normcase(str(lexical)) != os.path.normcase(str(canonical)):
                raise RuntimeError(
                    "CTest graph reparse alias is unsupported because CTest reopens "
                    f"the lexical path: {lexical} -> {canonical}"
                )
            if canonical in active_inputs:
                raise RuntimeError(f"recursive CTest graph include: {canonical}")
            if canonical not in seen_inputs:
                seen_inputs.add(canonical)
                inputs.append(canonical)
            active_inputs.add(canonical)
            try:
                stream = active.hold(lexical)
                stream.seek(0)
                text = _strip_cmake_comments(
                    stream.read().decode("utf-8", errors="replace")
                )
                commands = _cmake_commands_in_order(
                    text,
                    (
                        "add_test",
                        "add_subdirectory",
                        "discover_tests",
                        "include",
                        "set_property",
                        "set_tests_properties",
                        "subdirs",
                    ),
                )
                scope = os.path.normcase(str(execution_directory))
                for command_name, arguments, occurrences in commands:
                    if command_name == "discover_tests":
                        raise RuntimeError(
                            f"unsupported dynamic CTest discover_tests command in {canonical}"
                        )
                    if command_name in ("subdirs", "add_subdirectory", "include"):
                        if len(arguments) != 1 or any(
                            marker in arguments[0] for marker in ("${", "$<", ";")
                        ):
                            raise RuntimeError(
                                f"unsupported dynamic CTest {command_name} edge in {canonical}"
                            )
                        edge = Path(arguments[0])
                        if command_name in ("subdirs", "add_subdirectory"):
                            subdirectory = (
                                edge
                                if edge.is_absolute()
                                else execution_directory / edge
                            )
                            target = subdirectory / "CTestTestfile.cmake"
                            next_execution_directory = subdirectory
                        else:
                            target = (
                                edge if edge.is_absolute() else lexical.parent / edge
                            )
                            next_execution_directory = execution_directory
                        evaluate(
                            target,
                            command_name == "subdirs",
                            next_execution_directory,
                            target if command_name != "include" else scope_file,
                        )
                        path_arguments.append(
                            CTestPathArgument(
                                canonical,
                                arguments[0],
                                Path(os.path.abspath(
                                    subdirectory
                                    if command_name in ("subdirs", "add_subdirectory")
                                    else target
                                )).resolve(),
                                occurrences[0],
                            )
                        )
                        continue
                    if command_name == "add_test":
                        name = _test_name(arguments)
                        if name is not None:
                            additions.append(
                                (
                                    arguments,
                                    occurrences,
                                    execution_directory,
                                    canonical,
                                    scope_file,
                                    (scope, name),
                                )
                            )
                        continue
                    for tests, property_name, value, append, token_indices in (
                        _property_assignments_for_command(command_name, arguments)
                    ):
                        for test in tests:
                            properties = property_values.setdefault(
                                (scope, test), {}
                            )
                            if append:
                                properties.setdefault(property_name, []).append(value)
                            else:
                                properties[property_name] = [value]
                            token_values = property_tokens.setdefault(
                                (scope, test), {}
                            )
                            tokens = [
                                (arguments[index], occurrences[index])
                                for index in token_indices
                            ]
                            if append:
                                token_values.setdefault(property_name, []).extend(tokens)
                            else:
                                token_values[property_name] = tokens
            finally:
                active_inputs.remove(canonical)

        for graph_root, allow_missing, execution_directory in roots:
            evaluate(
                graph_root,
                allow_missing,
                execution_directory,
                graph_root,
            )

        records: list[CTestRecord] = []
        executables: set[Path] = set()
        working_directories: set[Path] = set()
        for (
            arguments,
            argument_occurrences,
            default_working,
            definition_file,
            scope_file,
            property_key,
        ) in additions:
            name = _test_name(arguments)
            if name is None:
                continue
            properties = property_values.get(property_key, {})
            tokens = property_tokens.get(property_key, {})
            configured_working = properties.get("working_directory", [])
            working_value = configured_working[-1] if configured_working else ""
            working_lexical = (
                Path(working_value)
                if working_value and Path(working_value).is_absolute()
                else default_working / working_value
            )
            working_lexical = Path(os.path.abspath(working_lexical))
            try:
                working_lexical.relative_to(lexical_build)
            except ValueError as error:
                raise RuntimeError(
                    f"CTest WORKING_DIRECTORY points outside selected build: {working_lexical}"
                ) from error
            if not working_lexical.is_dir():
                raise RuntimeError(
                    f"CTest WORKING_DIRECTORY does not exist: {working_lexical}"
                )
            working = active.canonical_directory(working_lexical)
            try:
                working.relative_to(canonical_build)
            except ValueError as error:
                raise RuntimeError(
                    "CTest WORKING_DIRECTORY is lexically inside the build but "
                    f"resolves outside it: {working_lexical} -> {working}"
                ) from error
            if os.path.normcase(str(working_lexical)) != os.path.normcase(str(working)):
                raise RuntimeError(
                    "CTest WORKING_DIRECTORY reparse alias is unsupported: "
                    f"{working_lexical} -> {working}"
                )
            working_directories.add(working)

            executable_arguments: list[
                tuple[str, Path, CMakeArgumentOccurrence]
            ] = []
            for argument, occurrence in zip(
                arguments, argument_occurrences, strict=True
            ):
                candidate = _executable_argument(argument)
                if candidate is None:
                    continue
                command = Path(candidate)
                lexical = command if command.is_absolute() else working_lexical / command
                lexical = Path(os.path.abspath(lexical))
                inside_build = _is_relative_to(lexical, lexical_build)
                if not lexical.is_file():
                    continue
                executable = active.canonical_file(lexical)
                if inside_build:
                    try:
                        executable.relative_to(canonical_build)
                    except ValueError as error:
                        raise RuntimeError(
                            "CTest executable is lexically inside the build tree but "
                            f"resolves outside it: {lexical} -> {executable}"
                        ) from error
                if os.path.normcase(str(lexical)) != os.path.normcase(
                    str(executable)
                ):
                    raise RuntimeError(
                        "CTest executable reparse alias is unsupported: "
                        f"{lexical} -> {executable}"
                    )
                executables.add(executable)
                executable_arguments.append((argument, executable, occurrence))

            environment = tuple(
                item
                for value in properties.get("environment", [])
                for item in _split_cmake_list(value)
            )
            modifications = tuple(
                item
                for value in properties.get("environment_modification", [])
                for item in _split_cmake_list(value)
            )
            labels = tuple(
                item
                for value in properties.get("labels", [])
                for item in _split_cmake_list(value)
            )
            fixtures_setup = tuple(
                item
                for value in properties.get("fixtures_setup", [])
                for item in _split_cmake_list(value)
            )
            fixtures_cleanup = tuple(
                item
                for value in properties.get("fixtures_cleanup", [])
                for item in _split_cmake_list(value)
            )
            fixtures_required = tuple(
                item
                for value in properties.get("fixtures_required", [])
                for item in _split_cmake_list(value)
            )
            records.append(
                CTestRecord(
                    name,
                    arguments,
                    argument_occurrences,
                    definition_file,
                    scope_file,
                    tuple(executable_arguments),
                    working,
                    labels,
                    fixtures_setup,
                    fixtures_cleanup,
                    fixtures_required,
                    tuple(tokens.get("environment", [])),
                    tuple(tokens.get("environment_modification", [])),
                    environment,
                    modifications,
                )
            )
        return CTestGraph(
            tuple(inputs),
            tuple(records),
            tuple(sorted(executables, key=lambda path: str(path).casefold())),
            tuple(sorted(working_directories, key=lambda path: str(path).casefold())),
            tuple(path_arguments),
        )
    finally:
        if owned:
            active.close()


def registered_test_executables(build_dir: Path) -> tuple[Path, ...]:
    """Return executables reachable through the actual generated CTest graph."""
    return discover_ctest_graph(build_dir).executables


def validate_ctest_runtime_properties(
    graph: CTestGraph,
    *,
    runtime_dirs: tuple[Path, ...],
    qt_plugins: Path,
    qt_platform_plugins: Path,
    ffmpeg_root: Path,
    srt_root: Path,
    qt_qml: Path | None = None,
    source_root: Path | None = None,
    build_root: Path | None = None,
) -> None:
    """Reject per-test loader changes not derived from the selected cache."""
    runtime_values = {str(path).casefold() for path in runtime_dirs}
    exact_values = {
        "qt_plugin_path": str(qt_plugins),
        "qt_qpa_platform_plugin_path": str(qt_platform_plugins),
        "qt_qpa_platform": "offscreen",
        "olr_ffmpeg_root": str(ffmpeg_root),
        "olr_srt_root": str(srt_root),
        "olr_mingw_bin": str(runtime_dirs[0]),
        "olr_qt_bin": str(runtime_dirs[1]),
    }

    def same_identity(value: str, expected: Path) -> bool:
        lexical = Path(os.path.abspath(value))
        return (
            os.path.normcase(str(lexical)) == os.path.normcase(str(expected))
            and lexical.resolve() == expected
        )

    def approved_qml_path(value: str) -> bool:
        if not Path(value).is_absolute():
            return False
        lexical = Path(os.path.abspath(value))
        if not lexical.exists():
            return False
        canonical = lexical.resolve()
        approved = tuple(
            root for root in (qt_qml, source_root, build_root) if root is not None
        )
        return any(
            (canonical == root or _is_relative_to(canonical, root))
            and os.path.normcase(str(lexical)) == os.path.normcase(str(canonical))
            for root in approved
        )

    for test in graph.tests:
        for assignment in test.environment:
            name, separator, value = assignment.partition("=")
            if not separator:
                raise RuntimeError(
                    f"CTest ENVIRONMENT for {test.name} is malformed: {assignment}"
                )
            folded = name.casefold()
            if folded == "path":
                raise RuntimeError(
                    f"CTest ENVIRONMENT for {test.name} may not replace PATH"
                )
            expected = exact_values.get(folded)
            if expected is not None:
                if Path(value).is_absolute() and folded != "qt_qpa_platform":
                    matches = same_identity(value, Path(expected))
                else:
                    matches = value.casefold() == expected.casefold()
                if not matches:
                    raise RuntimeError(
                        f"CTest ENVIRONMENT for {test.name} changes {name}: {value}"
                    )
                continue
            if folded in ("qml_import_path", "qml2_import_path"):
                if approved_qml_path(value):
                    continue
                raise RuntimeError(
                    f"CTest ENVIRONMENT for {test.name} changes {name}: {value}"
                )
            if folded == "qtest_function_timeout" and value.isdigit():
                continue
            if folded.startswith("olr_") and "\0" not in value:
                continue
            raise RuntimeError(
                f"CTest ENVIRONMENT for {test.name} has unapproved variable {name}"
            )

        path_modifications: list[Path] = []
        for modification in test.environment_modification:
            name, separator, operation_value = modification.partition("=")
            operation, operation_separator, value = operation_value.partition(":")
            if not separator or not operation_separator:
                raise RuntimeError(
                    "CTest ENVIRONMENT_MODIFICATION for "
                    f"{test.name} is malformed: {modification}"
                )
            folded = name.casefold()
            if folded == "path":
                if operation.casefold() != "path_list_prepend" or (
                    str(Path(os.path.abspath(value))).casefold() not in runtime_values
                    or not same_identity(value, Path(os.path.abspath(value)))
                ):
                    raise RuntimeError(
                        "CTest ENVIRONMENT_MODIFICATION for "
                        f"{test.name} changes PATH: {modification}"
                    )
                path_modifications.append(Path(os.path.abspath(value)).resolve())
                continue
            if folded in ("qml_import_path", "qml2_import_path"):
                if (
                    operation.casefold() == "path_list_prepend"
                    and approved_qml_path(value)
                ):
                    continue
                raise RuntimeError(
                    "CTest ENVIRONMENT_MODIFICATION for "
                    f"{test.name} changes {name}: {modification}"
                )
            raise RuntimeError(
                "CTest ENVIRONMENT_MODIFICATION for "
                f"{test.name} has unapproved variable {name}"
            )
        if path_modifications and tuple(reversed(path_modifications)) != runtime_dirs:
            raise RuntimeError(
                "CTest ENVIRONMENT_MODIFICATION PATH sequence for "
                f"{test.name} does not preserve controlled runtime order"
            )


def _configured_dlls(roots: RuntimeRoots) -> dict[str, Path]:
    """Index configured DLLs in the same precedence order as launcher PATH."""
    configured: dict[str, Path] = {}
    for directory in roots:
        children = sorted(directory.iterdir(), key=lambda path: path.name.casefold())
        for child in children:
            if child.is_file() and child.suffix.casefold() == ".dll":
                configured.setdefault(child.name.casefold(), child)
    return configured


def _authoritative_families(
    roots: RuntimeRoots,
) -> dict[tuple[str, str], dict[str, Path]]:
    authoritative: dict[tuple[str, str], dict[str, Path]] = {}
    owners = (
        ("CMAKE_CXX_COMPILER", (roots.compiler_bin,)),
        ("Qt6_DIR", (roots.qt_bin,)),
        ("OLR_FFMPEG_ROOT", (roots.ffmpeg_bin, roots.ffmpeg_lib)),
        ("OLR_SRT_ROOT", (roots.srt_bin, roots.srt_lib)),
    )
    for owner, directories in owners:
        for directory in directories:
            for child in sorted(directory.iterdir(), key=lambda path: path.name.casefold()):
                if not child.is_file() or child.suffix.casefold() != ".dll":
                    continue
                family = _controlled_family(child.name)
                if family is None or family[0] != owner:
                    continue
                authoritative.setdefault(family, {}).setdefault(
                    child.name.casefold(), child
                )
    return authoritative


def _expected_family_text(candidates: dict[str, Path]) -> str:
    if not candidates:
        return "no matching DLL"
    return ", ".join(str(path) for path in candidates.values())


def _reject_runtime_dll_shadowing(
    directory: Path,
    roots: RuntimeRoots,
    *,
    executables: tuple[Path, ...] = (),
    lease: RuntimeAuditLease,
    require_embedded_manifests: bool = False,
) -> None:
    """Fail when an adjacent controlled-runtime DLL differs from the cache source."""
    if not directory.is_dir():
        return
    configured_dlls = _configured_dlls(roots)
    authoritative = _authoritative_families(roots)
    adjacent_cache: dict[Path, dict[str, Path]] = {}
    digest_cache: dict[tuple[int, int, int, int], str] = {}

    def digest(path: Path) -> str:
        stream = lease.hold(path)
        stat = os.fstat(stream.fileno())
        key = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
        cached = digest_cache.get(key)
        if cached is None:
            cached = _sha256(path, stream=stream)
            digest_cache[key] = cached
        return cached

    def describe(path: Path) -> str:
        return _describe(path, stream=lease.hold(path))

    def adjacent_dlls(parent: Path) -> dict[str, Path]:
        parent = parent.resolve()
        cached = adjacent_cache.get(parent)
        if cached is not None:
            return cached
        adjacent_cache[parent] = found = {}
        if not parent.is_dir():
            return found
        for adjacent in sorted(parent.iterdir(), key=lambda path: path.name.casefold()):
            if not adjacent.is_file() or adjacent.suffix.casefold() != ".dll":
                continue
            lease.hold(adjacent)
            name = adjacent.name.casefold()
            found[name] = adjacent
            family = _controlled_family(name)
            if family is not None:
                candidates = authoritative.get(family, {})
                configured = candidates.get(name)
                if configured is None and not (
                    family[0] == "Qt6_DIR" and not candidates
                ):
                    raise RuntimeError(
                        "stale controlled-family DLL is not provided by active "
                        f"{family[0]}: {describe(adjacent)}; active family provides "
                        f"{_expected_family_text(candidates)}. Reconfigure and rebuild "
                        "the selected build tree."
                    )
            else:
                configured = configured_dlls.get(name)
            if configured is None:
                continue
            if digest(adjacent) != digest(configured):
                raise RuntimeError(
                    "incompatible runtime DLL shadows the configured Windows runtime: "
                    f"{describe(adjacent)}; configured peer: {describe(configured)}. "
                    "Remove the stale build tree or reconfigure and rebuild it."
                )
        return found

    adjacent_dlls(directory)

    resolved_executables = tuple(executable.resolve() for executable in executables)
    for executable in resolved_executables:
        redirection = Path(f"{executable}.local")
        if redirection.exists():
            raise RuntimeError(
                f"Windows {redirection} redirection is forbidden for controlled launches"
            )
        if executable.is_file():
            lease.hold(executable)
            if require_embedded_manifests:
                try:
                    embedded = has_embedded_application_manifest(executable)
                except (OSError, ValueError) as error:
                    raise RuntimeError(
                        f"cannot validate embedded RT_MANIFEST for {executable}: "
                        f"{error}. Rebuild the repository target or install a CTest "
                        "toolchain executable with a valid embedded application manifest."
                    ) from error
                if not embedded:
                    raise RuntimeError(
                        f"selected Windows executable lacks embedded RT_MANIFEST ID 1: "
                        f"{executable}. External .manifest files do not disable the "
                        "DotLocal race; rebuild the target or install a manifest-bearing "
                        "CTest toolchain."
                    )
    pending = [(executable, executable.parent) for executable in resolved_executables]
    inspected: set[tuple[Path, Path]] = set()
    while pending:
        binary, executable_dir = pending.pop()
        inspection = (binary, executable_dir)
        if inspection in inspected or not binary.is_file():
            continue
        if len(inspected) >= 4096:
            raise RuntimeError("Windows runtime import closure exceeds 4096 binaries")
        inspected.add(inspection)
        binary_adjacent = adjacent_dlls(executable_dir)
        for imported_name in pe_imports(binary, stream=lease.hold(binary)):
            imported_name = imported_name.casefold()
            resolved = binary_adjacent.get(imported_name)
            if resolved is None:
                resolved = configured_dlls.get(imported_name)
            family = _controlled_family(imported_name)
            if family is not None:
                candidates = authoritative.get(family, {})
                configured = candidates.get(imported_name)
                if configured is None and not (
                    family[0] == "Qt6_DIR" and not candidates
                ):
                    raise RuntimeError(
                        f"{binary} imports stale controlled runtime {imported_name}; "
                        f"active {family[0]} family provides "
                        f"{_expected_family_text(candidates)}. Reconfigure and rebuild "
                        "the selected executable."
                    )
                if configured is None:
                    if resolved is not None:
                        pending.append((resolved, executable_dir))
                    continue
                if resolved is None:
                    raise RuntimeError(
                        f"{binary} imports controlled runtime {imported_name}, but "
                        "Windows search order resolves no configured peer"
                    )
                if (
                    resolved != configured
                    and digest(resolved) != digest(configured)
                ):
                    raise RuntimeError(
                        f"{binary} imports {imported_name}, but an earlier Windows "
                        f"search path resolves incompatible peer {describe(resolved)}; "
                        f"authoritative {family[0]} peer: {describe(configured)}. "
                        "Reconfigure the dependency roots so authoritative runtime "
                        "families resolve first."
                    )
            if resolved is not None:
                pending.append((resolved, executable_dir))


def reject_runtime_dll_shadowing(
    directory: Path,
    roots: RuntimeRoots,
    *,
    executables: tuple[Path, ...] = (),
    lease: RuntimeAuditLease | None = None,
    require_embedded_manifests: bool = False,
) -> None:
    """Audit runtime provenance, optionally holding files for child lifetime."""
    owned_lease = lease is None
    active_lease = RuntimeAuditLease() if lease is None else lease
    try:
        if require_embedded_manifests:
            reject_dotlocal_dev_override()
        _reject_runtime_dll_shadowing(
            directory,
            roots,
            executables=executables,
            lease=active_lease,
            require_embedded_manifests=require_embedded_manifests,
        )
    finally:
        if owned_lease:
            active_lease.close()
