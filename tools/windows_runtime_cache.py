"""Protected Windows runtime cache model and canonical key encoding."""

from __future__ import annotations

import ctypes
import hashlib
import json
import math
import ntpath
import os
import re
import shutil
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from ctypes import wintypes
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path, PureWindowsPath
from types import MappingProxyType
from typing import (
    BinaryIO,
    Callable,
    Iterator,
    List,
    Literal,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    Union,
)


JsonScalar = Union[type(None), bool, int, float, str]
JsonValue = Union[
    JsonScalar,
    Tuple["JsonValue", ...],
    List["JsonValue"],
    Mapping[str, "JsonValue"],
]

SCHEMA_VERSION = "olr-windows-runtime-cache-v2"
CLOSURE_POLICY_VERSION = "closed-launch-layout-v2"
_HASH_BUFFER_BYTES = 1024 * 1024
_hash_buffer_local = threading.local()


@lru_cache(maxsize=1)
def _parallel_executor() -> ThreadPoolExecutor:
    """Share one bounded pool across consecutive immutable-object checks."""
    return ThreadPoolExecutor(
        max_workers=min(16, max(1, os.cpu_count() or 1)),
        thread_name_prefix="olr-runtime-cache",
    )


class _CacheObjectValidationError(RuntimeError):
    pass


@dataclass(frozen=True)
class HeldSource:
    source: Path
    final_path: Path
    device: int
    inode: int
    size: int
    sha256: str
    stream: BinaryIO = field(compare=False, repr=False)


@dataclass(frozen=True)
class _ResolvedHeldSource(HeldSource):
    resolver_token: object = field(compare=False, repr=False)
    key_input: Mapping[str, object] = field(compare=False, repr=False)


@dataclass(frozen=True)
class GeneratedLayoutEntry:
    policy: str
    inputs: Mapping[str, JsonValue]
    render: Callable[[Path], bytes] = field(compare=False, repr=False)


LayoutEntry = Union[HeldSource, bytes, GeneratedLayoutEntry]
LayoutEntries = Sequence[Tuple[Path, LayoutEntry]]


@dataclass(frozen=True)
class QtConfPaths:
    plugins: Path
    qml_imports: Path


def render_qt_conf(paths: QtConfPaths) -> bytes:
    return (
        "[Paths]\n"
        "Prefix = .\n"
        f"Plugins = {paths.plugins.as_posix()}\n"
        f"QmlImports = {paths.qml_imports.as_posix()}\n"
    ).encode("utf-8")


def generated_qt_conf(qt_paths: QtConfPaths) -> GeneratedLayoutEntry:
    if not isinstance(qt_paths, QtConfPaths):
        raise TypeError("qt_paths must be a QtConfPaths value")
    if not isinstance(qt_paths.plugins, Path) or not isinstance(
        qt_paths.qml_imports, Path
    ):
        raise TypeError("Qt configuration paths must be Path values")
    paths = QtConfPaths(Path(qt_paths.plugins), Path(qt_paths.qml_imports))
    return GeneratedLayoutEntry(
        "qt-conf-v2",
        {
            "plugins": paths.plugins.as_posix(),
            "qml_imports": paths.qml_imports.as_posix(),
        },
        lambda _layout: render_qt_conf(paths),
    )


@dataclass(frozen=True)
class CacheLimits:
    max_bytes: int = 8 << 30
    max_objects: int = 256
    max_unused_seconds: int = 14 * 86400
    staging_seconds: int = 3600
    stamp_seconds: int = 3600
    cleanup_seconds: int = 86400


@dataclass
class CacheStats:
    logical_files: int = 0
    logical_bytes: int = 0
    hits: int = 0
    misses: int = 0
    payload_files: int = 0
    payload_bytes: int = 0


@dataclass
class CachedLayoutLease:
    key: str
    object_root: Path
    layout: Path
    logical_files: int
    logical_bytes: int
    cache_hit: bool
    payload_files: int
    payload_bytes: int
    _owners: tuple[object, ...]

    def close(self) -> None:
        owners, self._owners = self._owners, ()
        for owner in reversed(owners):
            close = getattr(owner, "close", None)
            if callable(close):
                close()

    def __enter__(self) -> "CachedLayoutLease":
        return self

    def __exit__(self, *_unused: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()


_AclMode = Literal["PRIVATE", "BUILD", "RX", "DELETE"]

_FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
_FILE_ATTRIBUTE_DIRECTORY = 0x00000010
_FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
_FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
_FILE_READ_ATTRIBUTES = 0x00000080
_FILE_SHARE_READ = 0x00000001
_FILE_SHARE_WRITE = 0x00000002
_FILE_SHARE_DELETE = 0x00000004
_OPEN_EXISTING = 3
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

_GENERIC_READ = 0x80000000
_DELETE = 0x00010000
_FILE_ADD_FILE = 0x00000002
_FILE_ADD_SUBDIRECTORY = 0x00000004
_FILE_DELETE_CHILD = 0x00000040
_FILE_WRITE_DATA = 0x00000002
_FILE_APPEND_DATA = 0x00000004
_FILE_WRITE_EA = 0x00000010
_FILE_WRITE_ATTRIBUTES = 0x00000100
_READ_CONTROL = 0x00020000
_WRITE_DAC = 0x00040000

_ERROR_ACCESS_DENIED = 5
_ERROR_SHARING_VIOLATION = 32
_ERROR_FILE_EXISTS = 80
_ERROR_ALREADY_EXISTS = 183
_MOVEFILE_WRITE_THROUGH = 0x00000008
_FILE_BEGIN = 0

_DACL_SECURITY_INFORMATION = 0x00000004
_PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000
_SE_FILE_OBJECT = 1

_REPAIR_WAIT_SECONDS = 10.0
_REPAIR_POLL_SECONDS = 0.05
_REPAIR_STALE_SECONDS = 600


class _SecurityAttributes(ctypes.Structure):
    _fields_ = (
        ("length", wintypes.DWORD),
        ("security_descriptor", ctypes.c_void_p),
        ("inherit_handle", wintypes.BOOL),
    )


class _ByHandleFileInformation(ctypes.Structure):
    _fields_ = (
        ("file_attributes", wintypes.DWORD),
        ("creation_time", wintypes.FILETIME),
        ("last_access_time", wintypes.FILETIME),
        ("last_write_time", wintypes.FILETIME),
        ("volume_serial_number", wintypes.DWORD),
        ("file_size_high", wintypes.DWORD),
        ("file_size_low", wintypes.DWORD),
        ("number_of_links", wintypes.DWORD),
        ("file_index_high", wintypes.DWORD),
        ("file_index_low", wintypes.DWORD),
    )


class _OwnedHandle:
    def __init__(self, api: object, value: int) -> None:
        self._api = api
        self.value = value

    def close(self) -> None:
        if self.value:
            self._api.CloseHandle(self.value)
            self.value = 0

    def __enter__(self) -> "_OwnedHandle":
        return self

    def __exit__(self, *_unused: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()


@dataclass(frozen=True)
class _SealedEntry:
    relative: Path
    directory: bool
    identity: tuple[int, int, int]
    size: int
    digest: Optional[str]


class _SealedTree:
    """Retain the exact immutable root identity through publication."""

    def __init__(
        self,
        root: Path,
        owner: _OwnedHandle,
        entries: tuple[_SealedEntry, ...],
        sid: str,
    ) -> None:
        self.root = root
        self._owner = owner
        self.entries = entries
        self.sid = sid

    @property
    def handle(self) -> int:
        if not self._owner.value:
            raise RuntimeError("sealed cache tree handle is closed")
        return self._owner.value

    def close(self) -> None:
        self._owner.close()

    def __enter__(self) -> "_SealedTree":
        return self

    def __exit__(self, *_unused: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()


class _Guid(ctypes.Structure):
    _fields_ = (
        ("data1", wintypes.DWORD),
        ("data2", wintypes.WORD),
        ("data3", wintypes.WORD),
        ("data4", ctypes.c_ubyte * 8),
    )

    @classmethod
    def parse(cls, value: str) -> "_Guid":
        parsed = uuid.UUID(value)
        return cls(
            parsed.time_low,
            parsed.time_mid,
            parsed.time_hi_version,
            (ctypes.c_ubyte * 8)(*parsed.bytes[8:]),
        )


_FOLDERID_LOCAL_APP_DATA = _Guid.parse(
    "F1B32785-6FBA-4FCF-9D55-7B8E7F157091"
)


class WindowsRuntimeCache:
    """Own the canonical filesystem boundary for one per-user cache root."""

    def __init__(
        self,
        root: Optional[Path],
        sid: str,
        source_resolver: Callable[[Path], HeldSource],
        directory_holder: Callable[[Path], tuple[object, Path]],
        *,
        limits: CacheLimits = CacheLimits(),
        clock: Callable[[], float] = time.time,
    ) -> None:
        if os.name != "nt":
            raise OSError("Windows runtime cache filesystem is only available on Windows")
        self.sid = sid
        self.source_resolver = source_resolver
        self.directory_holder = directory_holder
        self.limits = limits
        self.clock = clock
        self._resolver_token = object()
        self._management_handles: list[_OwnedHandle] = []
        production_root = root is None
        self.root = _ensure_private_directory_tree(
            local_app_data_cache_root() if root is None else root,
            sid,
            managed_components=3 if production_root else 1,
            retained_handles=self._management_handles,
        )
        self.objects = _ensure_private_directory_tree(
            self.root / "objects",
            sid,
            retained_handles=self._management_handles,
            leaf_sddl=_managed_namespace_sddl(sid),
        )
        self.staging = _ensure_private_directory_tree(
            self.root / "staging",
            sid,
            retained_handles=self._management_handles,
            leaf_sddl=_managed_namespace_sddl(sid),
        )
        self.quarantine = _ensure_private_directory_tree(
            self.root / "quarantine",
            sid,
            retained_handles=self._management_handles,
        )
        self.access = _ensure_private_directory_tree(
            self.root / "access",
            sid,
            retained_handles=self._management_handles,
        )
        self.locks = _ensure_private_directory_tree(
            self.root / "locks",
            sid,
            retained_handles=self._management_handles,
        )

    def describe_source(self, path: Path) -> HeldSource:
        return self._accept_resolved_source(self.source_resolver(path))

    def _accept_resolved_source(self, descriptor: HeldSource) -> HeldSource:
        if not isinstance(descriptor, HeldSource):
            raise TypeError("runtime cache source resolver must return HeldSource")
        key_input = _entry_key_input(descriptor)
        return _ResolvedHeldSource(
            source=descriptor.source,
            final_path=descriptor.final_path,
            device=descriptor.device,
            inode=descriptor.inode,
            size=descriptor.size,
            sha256=descriptor.sha256,
            stream=descriptor.stream,
            resolver_token=self._resolver_token,
            key_input=MappingProxyType(key_input),
        )

    def materialize(
        self,
        name: str,
        entries: LayoutEntries,
        *,
        context: Mapping[str, JsonValue],
        dependencies: Sequence[CachedLayoutLease] = (),
        qt_paths: QtConfPaths | None = None,
        entry_limit: int = 8192,
        byte_limit: int = 1 << 30,
    ) -> CachedLayoutLease:
        return _materialize_layout(
            self,
            name,
            entries,
            context=context,
            dependencies=dependencies,
            qt_paths=qt_paths,
            entry_limit=entry_limit,
            byte_limit=byte_limit,
        )

    def cleanup(self, *, force: bool = False) -> None:
        if type(force) is not bool:
            raise TypeError("cache cleanup force must be a bool")
        _cleanup_cache(self, force=force)

    def _repair(
        self,
        key: str,
        build: Callable[[], CachedLayoutLease],
    ) -> CachedLayoutLease:
        return _repair_cache_object(self, key, build)

    def _touch_access(self, key: str) -> None:
        _touch_cache_access(self, key)

    def close(self) -> None:
        """Release cache-management handles without removing published objects."""
        owners, self._management_handles = self._management_handles, []
        first_error: Optional[BaseException] = None
        for owner in reversed(owners):
            try:
                owner.close()
            except BaseException as error:
                if first_error is None:
                    first_error = error
        if first_error is not None:
            raise first_error


def local_app_data_cache_root() -> Path:
    """Return the known-folder cache root without consulting inherited variables."""
    if os.name != "nt":
        raise OSError("Windows LocalAppData known folder is only available on Windows")

    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    ole32 = ctypes.WinDLL("ole32", use_last_error=True)
    shell32.SHGetKnownFolderPath.argtypes = (
        ctypes.POINTER(_Guid),
        wintypes.DWORD,
        wintypes.HANDLE,
        ctypes.POINTER(ctypes.c_wchar_p),
    )
    shell32.SHGetKnownFolderPath.restype = ctypes.c_long
    ole32.CoTaskMemFree.argtypes = (ctypes.c_void_p,)
    ole32.CoTaskMemFree.restype = None

    value = ctypes.c_wchar_p()
    result = int(
        shell32.SHGetKnownFolderPath(
            ctypes.byref(_FOLDERID_LOCAL_APP_DATA),
            0,
            None,
            ctypes.byref(value),
        )
    )
    if result != 0:
        raise OSError(result, "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed")
    try:
        if not value.value:
            raise RuntimeError("LocalAppData known folder returned an empty path")
        return Path(value.value) / "OpenLiveReplay" / "launch-cache" / "v2"
    finally:
        ole32.CoTaskMemFree(ctypes.cast(value, ctypes.c_void_p))


@lru_cache(maxsize=1)
def _kernel32() -> object:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    )
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.GetFileInformationByHandleEx.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
    )
    kernel32.GetFileInformationByHandleEx.restype = wintypes.BOOL
    kernel32.GetFinalPathNameByHandleW.argtypes = (
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
    )
    kernel32.GetFinalPathNameByHandleW.restype = wintypes.DWORD
    kernel32.GetFileInformationByHandle.argtypes = (
        wintypes.HANDLE,
        ctypes.c_void_p,
    )
    kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
    kernel32.SetFilePointerEx.argtypes = (
        wintypes.HANDLE,
        ctypes.c_longlong,
        ctypes.POINTER(ctypes.c_longlong),
        wintypes.DWORD,
    )
    kernel32.SetFilePointerEx.restype = wintypes.BOOL
    kernel32.ReadFile.argtypes = (
        wintypes.HANDLE,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    )
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.MoveFileExW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.LPCWSTR,
        wintypes.DWORD,
    )
    kernel32.MoveFileExW.restype = wintypes.BOOL
    kernel32.CreateDirectoryW.argtypes = (
        wintypes.LPCWSTR,
        ctypes.POINTER(_SecurityAttributes),
    )
    kernel32.CreateDirectoryW.restype = wintypes.BOOL
    return kernel32


@lru_cache(maxsize=1)
def _advapi32() -> object:
    return ctypes.WinDLL("advapi32", use_last_error=True)


class _FileAttributeTagInfo(ctypes.Structure):
    _fields_ = (
        ("file_attributes", wintypes.DWORD),
        ("reparse_tag", wintypes.DWORD),
    )


def _strip_resolver_final_path_prefix(value: str) -> str:
    if value.startswith("\\\\?\\UNC\\"):
        return "\\\\" + value[8:]
    if value.startswith("\\\\?\\"):
        return value[4:]
    return value


def _absolute_lexical(path: Path) -> Path:
    return Path(os.path.abspath(str(path)))


def _path_identity(path: Path) -> str:
    return ntpath.normcase(ntpath.normpath(str(path)))


def _final_path_from_handle(kernel32: object, handle: int) -> Path:
    required = int(kernel32.GetFinalPathNameByHandleW(handle, None, 0, 0))
    if not required:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_unicode_buffer(required + 1)
    written = int(
        kernel32.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
    )
    if not written or written >= len(buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    return Path(_strip_resolver_final_path_prefix(buffer.value))


def _assert_handle_still_names(handle: int, expected: Path) -> None:
    final = _final_path_from_handle(_kernel32(), handle)
    if _path_identity(final) != _path_identity(_absolute_lexical(expected)):
        raise RuntimeError(
            f"cache path moved during protected update: {expected} -> {final}"
        )


def _open_canonical_non_reparse(
    path: Path,
    parent: Optional[Path],
    *,
    desired_access: int = _FILE_READ_ATTRIBUTES,
    share_mode: int = _FILE_SHARE_READ | _FILE_SHARE_WRITE,
    directory: Optional[bool] = None,
) -> tuple[_OwnedHandle, Path]:
    """Open one canonical path without delete sharing or alias following."""
    if os.name != "nt":
        raise OSError("canonical Windows path validation requires Windows")
    lexical = _absolute_lexical(path)
    if parent is not None:
        lexical_parent = _absolute_lexical(parent)
        try:
            common = ntpath.commonpath((str(lexical_parent), str(lexical)))
        except ValueError as error:
            raise RuntimeError(f"cache path escapes canonical parent: {path}") from error
        if _path_identity(Path(common)) != _path_identity(lexical_parent):
            raise RuntimeError(f"cache path escapes canonical parent: {path}")

    kernel32 = _kernel32()
    handle = kernel32.CreateFileW(
        str(lexical),
        desired_access,
        share_mode,
        None,
        _OPEN_EXISTING,
        _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT,
        None,
    )
    if handle == _INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    owner = _OwnedHandle(kernel32, int(handle))
    try:
        attributes = _FileAttributeTagInfo()
        if not kernel32.GetFileInformationByHandleEx(
            handle,
            9,  # FileAttributeTagInfo
            ctypes.byref(attributes),
            ctypes.sizeof(attributes),
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if attributes.file_attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
            raise RuntimeError(f"cache path contains a reparse point: {lexical}")
        is_directory = bool(
            attributes.file_attributes & _FILE_ATTRIBUTE_DIRECTORY
        )
        if directory is not None and directory != is_directory:
            expected = "directory" if directory else "file"
            raise RuntimeError(f"cache path is not a {expected}: {lexical}")
        final = _final_path_from_handle(kernel32, owner.value)

        if _path_identity(lexical) != _path_identity(final):
            raise RuntimeError(
                f"cache path lexical/final mismatch: {lexical} != {final}"
            )
        return owner, final
    except BaseException:
        owner.close()
        raise


def _open_non_reparse_under_held_tree(
    path: Path,
    *,
    desired_access: int,
    directory: bool,
) -> _OwnedHandle:
    """Open a leaf whose canonical ancestor handles already prevent renames."""
    kernel32 = _kernel32()
    handle = kernel32.CreateFileW(
        str(_absolute_lexical(path)),
        desired_access,
        _FILE_SHARE_READ,
        None,
        _OPEN_EXISTING,
        _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT,
        None,
    )
    if handle == _INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    owner = _OwnedHandle(kernel32, int(handle))
    try:
        attributes = _FileAttributeTagInfo()
        if not kernel32.GetFileInformationByHandleEx(
            handle,
            9,  # FileAttributeTagInfo
            ctypes.byref(attributes),
            ctypes.sizeof(attributes),
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if attributes.file_attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
            raise RuntimeError(f"cache path contains a reparse point: {path}")
        actual_directory = bool(
            attributes.file_attributes & _FILE_ATTRIBUTE_DIRECTORY
        )
        if actual_directory != directory:
            expected = "directory" if directory else "file"
            raise RuntimeError(f"cache path is not a {expected}: {path}")
        return owner
    except BaseException:
        owner.close()
        raise


def _assert_canonical_non_reparse_tree(
    path: Path, parent: Optional[Path]
) -> Path:
    """Validate one path through a held, non-delete-shared handle."""
    owner, final = _open_canonical_non_reparse(path, parent)
    owner.close()
    return final


def _ensure_private_directory_tree(
    path: Path,
    sid: str,
    *,
    managed_components: int = 1,
    retained_handles: Optional[list[_OwnedHandle]] = None,
    leaf_sddl: Optional[str] = None,
) -> Path:
    lexical = _absolute_lexical(path)
    if not lexical.anchor:
        raise RuntimeError(f"cache root must be absolute: {path}")
    components = lexical.parts[1:]
    if managed_components < 1 or managed_components > len(components):
        raise RuntimeError(f"invalid managed cache path depth: {path}")
    managed_start = len(components) - managed_components
    current = Path(lexical.anchor)
    canonical_parent = _assert_canonical_non_reparse_tree(current, None)
    for index, component in enumerate(components):
        current /= component
        managed = index >= managed_start
        sddl = (
            leaf_sddl
            if current == lexical and leaf_sddl is not None
            else _acl_sddl(sid, "PRIVATE")
        )
        if not os.path.lexists(current):
            if not managed:
                raise RuntimeError(
                    f"unmanaged cache parent does not exist: {current}"
                )
            _create_directory_with_acl(current, sid, sddl)
        owner, canonical = _open_canonical_non_reparse(
            current,
            canonical_parent,
            desired_access=(
                _FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC
                if managed
                else _FILE_READ_ATTRIBUTES
            ),
            directory=True,
        )
        canonical_parent = canonical
        if managed:
            if current == lexical and leaf_sddl is not None:
                _set_handle_acl_sddl(owner.value, sddl)
            else:
                _set_handle_acl(owner.value, sid, "PRIVATE")
            _assert_handle_still_names(owner.value, current)
            if retained_handles is not None:
                retained_handles.append(owner)
                continue
        owner.close()
    return lexical


def _acl_sddl(sid: str, mode: _AclMode) -> str:
    acl_sddl = {
        "PRIVATE": f"D:P(A;OICI;FA;;;{sid})",
        "BUILD": f"D:P(A;OICI;FA;;;{sid})",
        "RX": f"D:P(A;OICI;GRGX;;;{sid})",
        "DELETE": f"D:P(A;OICI;FA;;;{sid})",
    }
    try:
        return acl_sddl[mode]
    except KeyError as error:
        raise ValueError(f"unsupported cache ACL mode: {mode}") from error


def _managed_namespace_sddl(sid: str) -> str:
    # Full current-user management access except FILE_DELETE_CHILD. Published
    # children deny DELETE themselves; maintenance explicitly converts a child
    # to DELETE before removing or quarantining it.
    return f"D:P(A;OICI;0x1f01bf;;;{sid})"


def _frozen_build_namespace_sddl(sid: str) -> str:
    # Transitional BUILD access without child insertion or parent-authorized
    # child deletion. Existing payload handles remain readable/writable only
    # until each exact descendant has been opened and retained below.
    return f"D:P(A;OICI;0x1f01b9;;;{sid})"


def _frozen_parent_namespace_sddl(sid: str) -> str:
    # Parent-only freeze: block add/subdirectory/delete-child without changing
    # inheritance on concurrent protected staging children.
    return f"D:P(A;;0x1f01b9;;;{sid})"


def _publication_root_sddl(sid: str) -> str:
    # Exact RX plus DELETE for the brief retained-handle MoveFileExW boundary.
    # Child insertion/deletion and content writes remain denied.
    return f"D:P(A;OICI;GRGXSD;;;{sid})"


def _security_descriptor_from_sddl(sddl: str) -> tuple[ctypes.c_void_p, object]:
    advapi32 = _advapi32()
    kernel32 = _kernel32()
    advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(wintypes.ULONG),
    )
    advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = wintypes.BOOL
    descriptor = ctypes.c_void_p()
    descriptor_size = wintypes.ULONG()
    if not advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl,
        1,
        ctypes.byref(descriptor),
        ctypes.byref(descriptor_size),
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    return descriptor, kernel32


def _descriptor_dacl(descriptor: ctypes.c_void_p) -> ctypes.c_void_p:
    advapi32 = _advapi32()
    advapi32.GetSecurityDescriptorDacl.argtypes = (
        ctypes.c_void_p,
        ctypes.POINTER(wintypes.BOOL),
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(wintypes.BOOL),
    )
    advapi32.GetSecurityDescriptorDacl.restype = wintypes.BOOL
    present = wintypes.BOOL()
    defaulted = wintypes.BOOL()
    dacl = ctypes.c_void_p()
    if not advapi32.GetSecurityDescriptorDacl(
        descriptor,
        ctypes.byref(present),
        ctypes.byref(dacl),
        ctypes.byref(defaulted),
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    if not present.value:
        raise RuntimeError("cache security descriptor has no DACL")
    return dacl


def _free_security_descriptor(
    descriptor: ctypes.c_void_p, kernel32: object
) -> None:
    kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
    kernel32.LocalFree.restype = ctypes.c_void_p
    kernel32.LocalFree(descriptor)


def _set_handle_acl_sddl(handle: int, sddl: str) -> None:
    descriptor, kernel32 = _security_descriptor_from_sddl(sddl)
    advapi32 = _advapi32()
    advapi32.SetSecurityInfo.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.DWORD,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    )
    advapi32.SetSecurityInfo.restype = wintypes.DWORD
    try:
        result = int(
            advapi32.SetSecurityInfo(
                handle,
                _SE_FILE_OBJECT,
                _DACL_SECURITY_INFORMATION
                | _PROTECTED_DACL_SECURITY_INFORMATION,
                None,
                None,
                _descriptor_dacl(descriptor),
                None,
            )
        )
        if result:
            raise OSError(result, "SetSecurityInfo(cache handle) failed")
    finally:
        _free_security_descriptor(descriptor, kernel32)


def _set_handle_acl(handle: int, sid: str, mode: _AclMode) -> None:
    _set_handle_acl_sddl(handle, _acl_sddl(sid, mode))


def _get_handle_acl_sddl(handle: int) -> str:
    advapi32 = _advapi32()
    kernel32 = _kernel32()
    kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
    kernel32.LocalFree.restype = ctypes.c_void_p
    advapi32.GetSecurityInfo.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.DWORD,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    )
    advapi32.GetSecurityInfo.restype = wintypes.DWORD
    advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = (
        ctypes.c_void_p,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.POINTER(ctypes.c_wchar_p),
        ctypes.POINTER(wintypes.ULONG),
    )
    advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW.restype = wintypes.BOOL

    descriptor = ctypes.c_void_p()
    result = int(
        advapi32.GetSecurityInfo(
            handle,
            _SE_FILE_OBJECT,
            _DACL_SECURITY_INFORMATION,
            None,
            None,
            None,
            None,
            ctypes.byref(descriptor),
        )
    )
    if result:
        raise OSError(result, "GetSecurityInfo(cache handle) failed")
    value = ctypes.c_wchar_p()
    try:
        if not advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor,
            1,
            _DACL_SECURITY_INFORMATION,
            ctypes.byref(value),
            None,
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if not value.value:
            raise RuntimeError("cache handle returned an empty DACL")
        return value.value
    finally:
        if value:
            kernel32.LocalFree(ctypes.cast(value, ctypes.c_void_p))
        if descriptor:
            kernel32.LocalFree(descriptor)


def _create_directory_with_acl(path: Path, sid: str, sddl: str) -> bool:
    del sid  # The SID is already bound into the supplied protected descriptor.
    descriptor, kernel32 = _security_descriptor_from_sddl(sddl)
    attributes = _SecurityAttributes(
        ctypes.sizeof(_SecurityAttributes), descriptor, False
    )
    try:
        if kernel32.CreateDirectoryW(str(path), ctypes.byref(attributes)):
            return True
        error = ctypes.get_last_error()
        if error in (_ERROR_FILE_EXISTS, _ERROR_ALREADY_EXISTS):
            return False
        raise ctypes.WinError(error)
    finally:
        _free_security_descriptor(descriptor, kernel32)


def _set_path_acl(path: Path, sid: str, mode: _AclMode) -> None:
    if os.name != "nt":
        raise OSError("Windows ACL conversion requires Windows")
    owner, _canonical = _open_canonical_non_reparse(
        path,
        _absolute_lexical(path).parent,
        desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC,
    )
    try:
        _set_handle_acl(owner.value, sid, mode)
        _assert_handle_still_names(owner.value, path)
    finally:
        owner.close()


@dataclass
class _HeldTreeNode:
    path: Path
    owner: _OwnedHandle
    entry: _SealedEntry


def _sha256_file_handle(handle: int) -> str:
    kernel32 = _kernel32()
    if not kernel32.SetFilePointerEx(handle, 0, None, _FILE_BEGIN):
        raise ctypes.WinError(ctypes.get_last_error())
    digest = hashlib.sha256()
    buffer = getattr(_hash_buffer_local, "buffer", None)
    if buffer is None:
        buffer = ctypes.create_string_buffer(_HASH_BUFFER_BYTES)
        _hash_buffer_local.buffer = buffer
    try:
        while True:
            read = wintypes.DWORD()
            if not kernel32.ReadFile(
                handle,
                buffer,
                len(buffer),
                ctypes.byref(read),
                None,
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            if not read.value:
                break
            digest.update(buffer.raw[: read.value])
        return digest.hexdigest()
    finally:
        if not kernel32.SetFilePointerEx(handle, 0, None, _FILE_BEGIN):
            raise ctypes.WinError(ctypes.get_last_error())


def _snapshot_handle(
    handle: int,
    relative: Path,
    *,
    expected_directory: bool,
) -> _SealedEntry:
    kernel32 = _kernel32()
    information = _ByHandleFileInformation()
    if not kernel32.GetFileInformationByHandle(handle, ctypes.byref(information)):
        raise ctypes.WinError(ctypes.get_last_error())
    directory = bool(information.file_attributes & _FILE_ATTRIBUTE_DIRECTORY)
    if directory != expected_directory:
        raise RuntimeError(f"cache tree entry type changed: {relative}")
    identity = (
        int(information.volume_serial_number),
        int(information.file_index_high),
        int(information.file_index_low),
    )
    size = (int(information.file_size_high) << 32) | int(information.file_size_low)
    digest = None if directory else _sha256_file_handle(handle)
    return _SealedEntry(relative, directory, identity, size, digest)


def _tree_entry_set(root: Path) -> set[tuple[str, bool]]:
    result: set[tuple[str, bool]] = set()

    def visit(directory: Path) -> None:
        with os.scandir(directory) as scanned:
            entries = sorted(scanned, key=lambda entry: entry.name.casefold())
        for entry in entries:
            target = Path(entry.path)
            stat_result = entry.stat(follow_symlinks=False)
            if (
                getattr(stat_result, "st_file_attributes", 0)
                & _FILE_ATTRIBUTE_REPARSE_POINT
            ):
                raise RuntimeError(
                    f"cache ACL tree contains a reparse point: {target}"
                )
            directory_entry = entry.is_dir(follow_symlinks=False)
            result.add((target.relative_to(root).as_posix(), directory_entry))
            if directory_entry:
                visit(target)

    visit(root)
    return result


def _seal_tree_acl(path: Path, sid: str, mode: _AclMode = "RX") -> _SealedTree:
    """Freeze, verify, and retain one exact immutable tree identity."""
    if mode != "RX":
        raise ValueError("only RX trees can be sealed for publication")
    root = _absolute_lexical(path)
    nodes: list[_HeldTreeNode] = []

    def hold_node(target: Path, directory: bool) -> None:
        desired_access = _FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC
        if target == root:
            desired_access |= _DELETE
        if not directory:
            desired_access |= _GENERIC_READ
        owner, _canonical = _open_canonical_non_reparse(
            target,
            target.parent,
            desired_access=desired_access,
            share_mode=(
                _FILE_SHARE_READ | _FILE_SHARE_DELETE
                if target == root
                else _FILE_SHARE_READ
            ),
            directory=directory,
        )
        relative = Path(".") if target == root else target.relative_to(root)
        entry = _snapshot_handle(
            owner.value,
            relative,
            expected_directory=directory,
        )
        node = _HeldTreeNode(target, owner, entry)
        nodes.append(node)
        if directory:
            with os.scandir(target) as scanned:
                entries = sorted(scanned, key=lambda entry: entry.name.casefold())
            for entry in entries:
                stat_result = entry.stat(follow_symlinks=False)
                child = Path(entry.path)
                if (
                    getattr(stat_result, "st_file_attributes", 0)
                    & _FILE_ATTRIBUTE_REPARSE_POINT
                ):
                    raise RuntimeError(
                        f"cache ACL tree contains a reparse point: {child}"
                    )
                hold_node(child, entry.is_dir(follow_symlinks=False))

    def validate_locked_tree() -> None:
        expected_entries = {
            (node.entry.relative.as_posix(), node.entry.directory)
            for node in nodes
            if node.path != root
        }
        if _tree_entry_set(root) != expected_entries:
            raise RuntimeError("cache tree changed during immutable conversion")
        for node in nodes:
            _assert_handle_still_names(node.owner.value, node.path)
            current = _snapshot_handle(
                node.owner.value,
                node.entry.relative,
                expected_directory=node.entry.directory,
            )
            if current != node.entry:
                raise RuntimeError(
                    f"cache entry changed during immutable conversion: {node.path}"
                )

    try:
        hold_node(root, True)
        validate_locked_tree()

        # No ACL changes occur until every expected node is open without
        # write/delete sharing. Freeze child namespaces before their parents so
        # protected descendants cannot escape through an inherited gap.
        directories = sorted(
            (node for node in nodes if node.entry.directory),
            key=lambda node: len(node.entry.relative.parts),
            reverse=True,
        )
        for node in directories:
            _set_handle_acl_sddl(
                node.owner.value,
                _frozen_build_namespace_sddl(sid),
            )
            _assert_handle_still_names(node.owner.value, node.path)

        # The exact tree is now namespace-frozen. Convert root-first and verify
        # each retained handle still names its path after every final ACL. The
        # initial exact-tree revalidation already proved identities and bytes;
        # the held no-write/no-delete-sharing handles prevent either changing.
        root_first = sorted(
            nodes,
            key=lambda node: (
                len(node.entry.relative.parts),
                not node.entry.directory,
            ),
        )
        for node in root_first:
            _set_handle_acl(node.owner.value, sid, "RX")
            _assert_handle_still_names(node.owner.value, node.path)
            _assert_handle_rx_acl(
                node.owner.value,
                sid,
                node.path,
                directory=node.entry.directory,
            )

        root_owner = nodes[0].owner
        for node in nodes[1:]:
            node.owner.close()
        return _SealedTree(
            root,
            root_owner,
            tuple(node.entry for node in nodes),
            sid,
        )
    except BaseException:
        for node in nodes:
            node.owner.close()
        raise


def _set_tree_acl(path: Path, sid: str, mode: _AclMode) -> None:
    """Apply a protected DACL, recursively for immutable/delete conversions."""
    root = _absolute_lexical(path)
    if mode == "RX":
        with _seal_tree_acl(root, sid):
            return
    if mode != "DELETE":
        _set_path_acl(root, sid, mode)
        return

    def convert_delete(target: Path, directory: bool) -> None:
        owner, _canonical = _open_canonical_non_reparse(
            target,
            target.parent,
            desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC,
            share_mode=_FILE_SHARE_READ,
            directory=directory,
        )
        try:
            if directory:
                with os.scandir(target) as scanned:
                    entries = sorted(scanned, key=lambda entry: entry.name.casefold())
                for entry in entries:
                    child = Path(entry.path)
                    stat_result = entry.stat(follow_symlinks=False)
                    if (
                        getattr(stat_result, "st_file_attributes", 0)
                        & _FILE_ATTRIBUTE_REPARSE_POINT
                    ):
                        raise RuntimeError(
                            f"cache ACL tree contains a reparse point: {child}"
                        )
                    convert_delete(child, entry.is_dir(follow_symlinks=False))
            _set_handle_acl(owner.value, sid, "DELETE")
            _assert_handle_still_names(owner.value, target)
        finally:
            owner.close()

    convert_delete(root, True)


def _assert_mutation_denied(path: Path, *, directory: bool) -> None:
    """Require the immutable ACL to deny every individual mutation right."""
    if os.name != "nt":
        raise OSError("Windows mutation probes require Windows")
    kernel32 = _kernel32()
    desired_accesses = (
        (_FILE_ADD_FILE, _FILE_ADD_SUBDIRECTORY, _FILE_DELETE_CHILD, _DELETE)
        if directory
        else (
            _FILE_WRITE_DATA,
            _FILE_APPEND_DATA,
            _FILE_WRITE_EA,
            _FILE_WRITE_ATTRIBUTES,
            _DELETE,
        )
    )
    flags = _FILE_FLAG_BACKUP_SEMANTICS if directory else 0
    for desired_access in desired_accesses:
        handle = kernel32.CreateFileW(
            str(path),
            desired_access,
            _FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
            None,
            _OPEN_EXISTING,
            flags,
            None,
        )
        if handle != _INVALID_HANDLE_VALUE:
            kernel32.CloseHandle(handle)
            raise RuntimeError(
                f"cache mutation access unexpectedly permitted: {path} "
                f"(0x{desired_access:x})"
            )
        error = ctypes.get_last_error()
        if error != _ERROR_ACCESS_DENIED:
            raise OSError(error, f"cache mutation probe failed unexpectedly: {path}")


def _expected_rx_acl_sddls(sid: str, *, directory: bool) -> set[str]:
    direct = f"D:PAI(A;;0x1200a9;;;{sid})"
    if not directory:
        return {direct}
    return {
        direct,
        direct + f"(A;OICIIO;GXGR;;;{sid})",
        direct + f"(A;OICIIO;GRGX;;;{sid})",
    }


def _assert_handle_rx_acl(handle: int, sid: str, path: Path, *, directory: bool) -> None:
    actual = _get_handle_acl_sddl(handle)
    if actual not in _expected_rx_acl_sddls(sid, directory=directory):
        raise RuntimeError(f"cache object ACL mismatch: {path}: {actual!r}")


def _assert_path_rx_acl(path: Path, sid: str, *, directory: bool) -> None:
    owner, final = _open_canonical_non_reparse(
        path,
        path.parent,
        desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL,
        share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
        directory=directory,
    )
    try:
        if _path_identity(final) != _path_identity(path):
            raise RuntimeError(f"cache ACL path mismatch: {path}")
        _assert_handle_still_names(owner.value, path)
        _assert_handle_rx_acl(owner.value, sid, path, directory=directory)
    finally:
        owner.close()


@contextmanager
def _hold_rx_directories(
    paths: Sequence[Path], sid: str
) -> Iterator[None]:
    owners: list[_OwnedHandle] = []
    try:
        for path in paths:
            owner, _final = _open_canonical_non_reparse(
                path,
                path.parent,
                desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL,
                share_mode=_FILE_SHARE_READ,
                directory=True,
            )
            try:
                _assert_handle_rx_acl(owner.value, sid, path, directory=True)
            except BaseException:
                owner.close()
                raise
            owners.append(owner)
        yield
    finally:
        for owner in reversed(owners):
            owner.close()


def _validate_sealed_tree_at(sealed: _SealedTree, root: Path) -> None:
    root = _absolute_lexical(root)
    expected_entries = {
        (entry.relative.as_posix(), entry.directory)
        for entry in sealed.entries
        if entry.relative != Path(".")
    }
    if _tree_entry_set(root) != expected_entries:
        raise RuntimeError(f"sealed cache tree exact set changed: {root}")

    root_entry = sealed.entries[0]
    _assert_handle_still_names(sealed.handle, root)
    if _snapshot_handle(
        sealed.handle,
        root_entry.relative,
        expected_directory=True,
    ) != root_entry:
        raise RuntimeError(f"sealed cache root identity changed: {root}")

    for expected in sealed.entries[1:]:
        target = root / expected.relative
        desired_access = _FILE_READ_ATTRIBUTES
        if not expected.directory:
            desired_access |= _GENERIC_READ
        owner, _canonical = _open_canonical_non_reparse(
            target,
            target.parent,
            desired_access=desired_access,
            share_mode=_FILE_SHARE_READ,
            directory=expected.directory,
        )
        try:
            current = _snapshot_handle(
                owner.value,
                expected.relative,
                expected_directory=expected.directory,
            )
            if current != expected:
                raise RuntimeError(f"sealed cache entry identity changed: {target}")
            _assert_handle_still_names(owner.value, target)
        finally:
            owner.close()


def _validate_sealed_root_at(sealed: _SealedTree, root: Path) -> None:
    root = _absolute_lexical(root)
    expected = sealed.entries[0]
    _assert_handle_still_names(sealed.handle, root)
    current = _snapshot_handle(
        sealed.handle,
        expected.relative,
        expected_directory=True,
    )
    if current != expected:
        raise RuntimeError(f"sealed cache root identity changed: {root}")


def _move_file_no_replace_write_through(source: Path, destination: Path) -> None:
    kernel32 = _kernel32()
    if kernel32.MoveFileExW(
        str(source),
        str(destination),
        _MOVEFILE_WRITE_THROUGH,
    ):
        return
    error = ctypes.get_last_error()
    if error in (_ERROR_FILE_EXISTS, _ERROR_ALREADY_EXISTS) or (
        error == _ERROR_ACCESS_DENIED and os.path.lexists(destination)
    ):
        raise FileExistsError(
            error,
            f"cache publication destination already exists: {destination}",
            str(destination),
        )
    raise ctypes.WinError(error)


def _atomic_move_no_replace(
    source: Path,
    destination: Path,
    *,
    sealed: Optional[_SealedTree] = None,
) -> None:
    """Atomically publish the retained immutable identity without replacement."""
    if os.name != "nt":
        raise OSError("Windows atomic publication requires Windows")
    if sealed is None:
        raise RuntimeError("cache publication requires a retained sealed tree")
    source = _absolute_lexical(source)
    destination = _absolute_lexical(destination)
    if _path_identity(source) != _path_identity(sealed.root):
        raise RuntimeError("sealed cache tree does not match publication source")
    # Sealing already retained and validated the complete immutable tree. The
    # atomic directory move cannot split descendants from that root, so the
    # publication boundary needs to bind only the retained root identity/path.
    _validate_sealed_root_at(sealed, source)

    source_parent = source.parent
    source_parent_owner, _canonical = _open_canonical_non_reparse(
        source_parent,
        source_parent.parent,
        desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC,
        share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE,
        directory=True,
    )
    destination_parent = destination.parent
    try:
        destination_parent_owner, _canonical = _open_canonical_non_reparse(
            destination_parent,
            destination_parent.parent,
            desired_access=_FILE_READ_ATTRIBUTES,
            share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE,
            directory=True,
        )
    except BaseException:
        source_parent_owner.close()
        raise
    publication_path = source
    original_source_parent_sddl: Optional[str] = None
    try:
        _assert_handle_still_names(source_parent_owner.value, source_parent)
        _assert_handle_still_names(
            destination_parent_owner.value,
            destination_parent,
        )
        original_source_parent_sddl = _get_handle_acl_sddl(
            source_parent_owner.value
        )
        _set_handle_acl_sddl(
            source_parent_owner.value,
            _frozen_parent_namespace_sddl(sealed.sid),
        )
        _assert_handle_still_names(source_parent_owner.value, source_parent)
        # MoveFileExW cannot consume an existing handle. Grant DELETE only on
        # the retained immutable root while its parent forbids adding a path
        # replacement. Source identity and both parents stay bound across the
        # write-through path API; post-move validation requires the same file ID.
        _set_handle_acl_sddl(
            sealed.handle,
            _publication_root_sddl(sealed.sid),
        )
        _assert_handle_still_names(sealed.handle, source)
        _move_file_no_replace_write_through(source, destination)
        publication_path = destination
        _assert_handle_still_names(
            destination_parent_owner.value,
            destination_parent,
        )
        _validate_sealed_root_at(sealed, destination)
    finally:
        try:
            _set_handle_acl(sealed.handle, sealed.sid, "RX")
            _assert_handle_still_names(sealed.handle, publication_path)
            _validate_sealed_root_at(sealed, publication_path)
        finally:
            try:
                if original_source_parent_sddl is not None:
                    _set_handle_acl_sddl(
                        source_parent_owner.value,
                        original_source_parent_sddl,
                    )
                    _assert_handle_still_names(
                        source_parent_owner.value,
                        source_parent,
                    )
            finally:
                source_parent_owner.close()
                destination_parent_owner.close()


def _stream_sha256(stream: BinaryIO) -> str:
    stream.seek(0)
    digest = hashlib.sha256()
    try:
        while True:
            payload = stream.read(1024 * 1024)
            if not payload:
                return digest.hexdigest()
            digest.update(payload)
    finally:
        stream.seek(0)


def _revalidate_held_source(
    descriptor: HeldSource, resolver_token: object | None = None
) -> HeldSource:
    _entry_key_input(descriptor)
    if (
        isinstance(descriptor, _ResolvedHeldSource)
        and descriptor.resolver_token is resolver_token
    ):
        if getattr(descriptor.stream, "closed", False):
            raise RuntimeError(
                f"held source stream is not an open file: {descriptor.source}"
            )
        return descriptor
    try:
        import msvcrt

        descriptor_fd = descriptor.stream.fileno()
        os_handle = msvcrt.get_osfhandle(descriptor_fd)
    except (AttributeError, OSError, ValueError) as error:
        raise RuntimeError(
            f"held source stream is not an open file: {descriptor.source}"
        ) from error
    handle_final_path = _final_path_from_handle(_kernel32(), os_handle)
    expected_final = _normalized_held_final_path(descriptor.final_path)
    if _normalized_held_final_path(handle_final_path) != expected_final:
        raise RuntimeError(f"held source final path changed: {descriptor.source}")
    stat_result = os.fstat(descriptor_fd)
    actual_identity = (
        int(stat_result.st_dev),
        int(stat_result.st_ino),
        int(stat_result.st_size),
    )
    expected_identity = (descriptor.device, descriptor.inode, descriptor.size)
    if actual_identity != expected_identity:
        raise RuntimeError(
            f"held source filesystem identity changed: {descriptor.source}"
        )
    if _stream_sha256(descriptor.stream) != descriptor.sha256:
        raise RuntimeError(f"held source content changed: {descriptor.source}")
    return descriptor


def _entry_kind(entry: LayoutEntry) -> str:
    if isinstance(entry, HeldSource):
        return "held-source"
    if isinstance(entry, bytes):
        return "literal-bytes"
    if isinstance(entry, GeneratedLayoutEntry):
        return "generated"
    raise TypeError(f"unsupported layout entry: {type(entry).__name__}")


def _enumerate_tree(root: Path) -> tuple[set[str], set[str]]:
    directories: set[str] = set()
    files: set[str] = set()

    def visit(directory: Path, relative_parent: str) -> None:
        with os.scandir(directory) as scanned:
            entries = sorted(scanned, key=lambda item: ntpath.normcase(item.name))
        for scanned_entry in entries:
            target = Path(scanned_entry.path)
            stat_result = scanned_entry.stat(follow_symlinks=False)
            if (
                getattr(stat_result, "st_file_attributes", 0)
                & _FILE_ATTRIBUTE_REPARSE_POINT
            ):
                raise RuntimeError(
                    f"cache object contains a reparse point: {target}"
                )
            relative = (
                f"{relative_parent}/{scanned_entry.name}"
                if relative_parent
                else scanned_entry.name
            )
            if scanned_entry.is_dir(follow_symlinks=False):
                directories.add(relative)
                visit(target, relative)
            elif scanned_entry.is_file(follow_symlinks=False):
                link_count = int(
                    getattr(os.stat(target, follow_symlinks=False), "st_nlink", 1)
                )
                if link_count != 1:
                    raise RuntimeError(f"cache object contains a file alias: {target}")
                files.add(relative)
            else:
                raise RuntimeError(f"cache object contains an invalid entry: {target}")

    visit(root, "")
    return directories, files


def _read_manifest(object_root: Path) -> dict[str, object]:
    manifest_path = object_root / "manifest.json"
    try:
        encoded = manifest_path.read_bytes()
        decoded = json.loads(encoded.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid cache manifest: {manifest_path}") from error
    if not isinstance(decoded, dict):
        raise RuntimeError(f"invalid cache manifest: {manifest_path}")
    try:
        canonical = canonical_json(decoded)
    except RuntimeError as error:
        raise RuntimeError(f"invalid cache manifest: {manifest_path}") from error
    if encoded != canonical:
        raise RuntimeError(f"cache manifest is not canonical: {manifest_path}")
    return decoded


def _expected_tree_sets(
    layout_name: str, file_records: Sequence[Mapping[str, object]]
) -> tuple[set[str], set[str]]:
    directories = {layout_name}
    files = {"manifest.json"}
    for record in file_records:
        destination = record["destination"]
        assert isinstance(destination, str)
        destination_posix = destination.replace("\\", "/")
        relative = f"{layout_name}/{destination_posix}"
        files.add(relative)
        parent = relative.rpartition("/")[0]
        while parent:
            directories.add(parent)
            parent = parent.rpartition("/")[0]
    return directories, files


def _validate_object_contents(
    object_root: Path,
    final_object_root: Path,
    key: str,
    key_input: Mapping[str, object],
    requested_entries: Sequence[Tuple[Path, LayoutEntry]],
    *,
    require_acl: bool,
    rendered_entries: Mapping[str, bytes],
    sid: str | None = None,
    manifest: Mapping[str, object] | None = None,
) -> tuple[Path, int, tuple[Path, ...], tuple[Path, ...]]:
    if object_root.name != key and object_root == final_object_root:
        raise RuntimeError(f"cache object path does not match key: {object_root}")
    if manifest is None:
        manifest = _read_manifest(object_root)
    if set(manifest) != {
        "schema",
        "closure_policy",
        "key_input",
        "object_key",
        "layout_name",
        "created",
        "files",
    }:
        raise RuntimeError(f"invalid cache manifest fields: {object_root}")
    if (
        manifest.get("schema") != SCHEMA_VERSION
        or manifest.get("closure_policy") != CLOSURE_POLICY_VERSION
        or manifest.get("object_key") != key
        or manifest.get("key_input") != key_input
    ):
        raise RuntimeError(f"cache manifest key or policy mismatch: {object_root}")
    created = manifest.get("created")
    if not isinstance(created, (int, float)) or isinstance(created, bool) or not math.isfinite(created):
        raise RuntimeError(f"invalid cache manifest creation time: {object_root}")

    stored_layout_name = manifest.get("layout_name")
    if not isinstance(stored_layout_name, str):
        raise RuntimeError(f"invalid cache manifest layout spelling: {object_root}")
    if _validate_win32_component(stored_layout_name, label="layout name") != key_input.get("name"):
        raise RuntimeError(f"cache manifest layout spelling mismatch: {object_root}")
    layout = final_object_root / stored_layout_name

    records_value = manifest.get("files")
    if not isinstance(records_value, list):
        raise RuntimeError(f"invalid cache manifest file records: {object_root}")
    requested_by_normalized = {
        _normalized_destination(destination): entry
        for destination, entry in requested_entries
    }
    key_entries_value = key_input.get("entries")
    if not isinstance(key_entries_value, list):
        raise RuntimeError(f"invalid cache manifest key entries: {object_root}")
    key_entries = {
        item["normalized_destination"]: item["entry"]
        for item in key_entries_value
        if isinstance(item, dict)
        and isinstance(item.get("normalized_destination"), str)
        and isinstance(item.get("entry"), dict)
    }
    if len(key_entries) != len(key_entries_value):
        raise RuntimeError(f"invalid cache manifest key entries: {object_root}")

    records: list[Mapping[str, object]] = []
    seen: set[str] = set()
    acl_validated_files: set[str] = set()
    payload_checks: list[
        tuple[Path, str, str, int, LayoutEntry, str]
    ] = []
    logical_bytes = 0
    for value in records_value:
        if not isinstance(value, dict):
            raise RuntimeError(f"invalid cache manifest file record: {object_root}")
        destination = value.get("destination")
        if not isinstance(destination, str):
            raise RuntimeError(f"invalid cache manifest destination: {object_root}")
        normalized = _normalized_destination(Path(destination))
        if normalized in seen:
            raise RuntimeError(f"duplicate cache manifest destination: {destination}")
        seen.add(normalized)
        if normalized not in requested_by_normalized or normalized not in key_entries:
            raise RuntimeError(f"cache manifest destination identity mismatch: {destination}")
        entry = requested_by_normalized[normalized]
        key_entry = key_entries[normalized]
        if key_entry != _entry_key_input(entry):
            raise RuntimeError(f"cache key entry identity mismatch: {destination}")
        if (
            value.get("kind") != _entry_kind(entry)
            or value.get("kind") != key_entry.get("kind")
        ):
            raise RuntimeError(f"cache manifest entry kind mismatch: {destination}")
        expected_record_fields = {"destination", "kind", "size", "sha256"}
        if isinstance(entry, HeldSource):
            expected_record_fields.add("source_final_path")
        if set(value) != expected_record_fields:
            raise RuntimeError(f"invalid cache manifest file record: {destination}")
        size = value.get("size")
        digest = value.get("sha256")
        if type(size) is not int or size < 0 or not isinstance(digest, str) or not _LOWER_SHA256.fullmatch(digest):
            raise RuntimeError(f"invalid cache manifest file metadata: {destination}")
        if "size" in key_entry and size != key_entry.get("size"):
            raise RuntimeError(f"cache manifest key entry metadata mismatch: {destination}")
        if "sha256" in key_entry and digest != key_entry.get("sha256"):
            raise RuntimeError(f"cache manifest key entry metadata mismatch: {destination}")
        target = object_root / stored_layout_name / Path(destination)
        payload_checks.append(
            (target, destination, digest, size, entry, normalized)
        )
        if isinstance(entry, HeldSource):
            source_final = value.get("source_final_path")
            if (
                not isinstance(source_final, str)
                or not isinstance(key_entry, dict)
                or source_final != key_entry.get("final_path")
            ):
                raise RuntimeError(f"cache manifest source identity mismatch: {target}")
        logical_bytes += size
        records.append(value)

    if seen != set(requested_by_normalized) or seen != set(key_entries):
        raise RuntimeError(f"cache manifest exact file set mismatch: {object_root}")

    expected_directories, expected_files = _expected_tree_sets(
        stored_layout_name, records
    )
    actual_directories, actual_files = _enumerate_tree(object_root)
    if actual_directories != expected_directories or actual_files != expected_files:
        raise RuntimeError(f"cache exact file set mismatch: {object_root}")

    directory_paths = tuple(
        object_root / Path(relative)
        for relative in sorted(
            actual_directories,
            key=lambda item: (item.count("/"), ntpath.normcase(item)),
        )
    )
    file_paths = tuple(
        object_root / Path(relative)
        for relative in sorted(actual_files, key=ntpath.normcase)
    )
    if require_acl:
        if sid is None:
            raise RuntimeError("cache ACL validation requires a user SID")
        _assert_mutation_denied(object_root, directory=True)

    def validate_payload(
        check: tuple[Path, str, str, int, LayoutEntry, str]
    ) -> str | None:
        target, destination, digest, size, entry, normalized = check
        if require_acl:
            if sid is None:
                raise RuntimeError("cache ACL validation requires a user SID")
            try:
                owner = _open_non_reparse_under_held_tree(
                    target,
                    desired_access=(
                        _FILE_READ_ATTRIBUTES | _READ_CONTROL | _GENERIC_READ
                    ),
                    directory=False,
                )
            except OSError as error:
                raise RuntimeError(
                    f"cache exact file set mismatch: {object_root}"
                ) from error
            try:
                snapshot = _snapshot_handle(
                    owner.value,
                    Path(destination),
                    expected_directory=False,
                )
                _assert_handle_rx_acl(owner.value, sid, target, directory=False)
                actual_size = snapshot.size
                actual_digest = snapshot.digest
            finally:
                owner.close()
        else:
            try:
                payload = target.read_bytes()
            except OSError as error:
                raise RuntimeError(
                    f"cache exact file set mismatch: {object_root}"
                ) from error
            actual_size = len(payload)
            actual_digest = hashlib.sha256(payload).hexdigest()
        if actual_size != size or actual_digest != digest:
            raise RuntimeError(f"cache file digest mismatch: {target}")
        if isinstance(entry, GeneratedLayoutEntry):
            expected_payload = rendered_entries.get(normalized)
            if (
                expected_payload is None
                or len(expected_payload) != size
                or hashlib.sha256(expected_payload).hexdigest() != digest
            ):
                raise RuntimeError(f"cache generated entry mismatch: {target}")
        return _path_identity(target) if require_acl else None

    def run_payload_checks() -> tuple[str | None, ...]:
        if len(payload_checks) >= 32:
            return tuple(_parallel_executor().map(validate_payload, payload_checks))
        return tuple(map(validate_payload, payload_checks))

    if require_acl:
        assert sid is not None
        with _hold_rx_directories(directory_paths, sid):
            validated_payloads = run_payload_checks()
            acl_validated_files.update(
                path for path in validated_payloads if path is not None
            )
            for file_path in file_paths:
                if _path_identity(file_path) not in acl_validated_files:
                    _assert_path_rx_acl(file_path, sid, directory=False)
    else:
        run_payload_checks()
    return final_object_root / stored_layout_name, logical_bytes, directory_paths, file_paths


def _hold_validated_object(
    cache: WindowsRuntimeCache,
    object_root: Path,
    key: str,
    key_input: Mapping[str, object],
    entries: Sequence[Tuple[Path, LayoutEntry]],
    *,
    cache_hit: bool,
    payload_files: int,
    payload_bytes: int,
    rendered_entries: Mapping[str, bytes],
    manifest: Mapping[str, object] | None = None,
) -> CachedLayoutLease:
    validation_owner, root_final = _open_canonical_non_reparse(
        object_root,
        object_root.parent,
        directory=True,
    )
    protected_owner: Optional[_OwnedHandle] = None
    try:
        if _path_identity(root_final) != _path_identity(object_root):
            raise RuntimeError(f"cache object final path mismatch: {object_root}")
        validation_identity = _snapshot_handle(
            validation_owner.value,
            Path("."),
            expected_directory=True,
        ).identity
        layout, logical_bytes, _directories, _files = _validate_object_contents(
            object_root,
            object_root,
            key,
            key_input,
            entries,
            require_acl=True,
            sid=cache.sid,
            rendered_entries=rendered_entries,
            manifest=manifest,
        )
        protected_owner, protected_final = _open_canonical_non_reparse(
            object_root,
            object_root.parent,
            directory=True,
        )
        protected_identity = _snapshot_handle(
            protected_owner.value,
            Path("."),
            expected_directory=True,
        ).identity
        if (
            _path_identity(protected_final) != _path_identity(object_root)
            or protected_identity != validation_identity
        ):
            raise RuntimeError(
                f"cache object identity changed during validation: {object_root}"
            )
        _assert_handle_still_names(validation_owner.value, object_root)
    except BaseException:
        if protected_owner is not None:
            protected_owner.close()
        raise
    finally:
        validation_owner.close()

    # The complete tree has just passed exact-set, hash, generated-byte, final
    # path, reparse, and mutation-denial validation. Retain the root identity
    # plus the manifest without delete sharing. The manifest guard makes the
    # lifecycle remover's all-or-nothing descendant preflight fail before any
    # deletion, while the protected DACL keeps every payload immutable. Opening
    # every descendant would only repeat the same full-tree validation.
    manifest_path = object_root / "manifest.json"
    try:
        manifest_owner, manifest_final = _open_canonical_non_reparse(
            manifest_path,
            object_root,
            desired_access=_FILE_READ_ATTRIBUTES | _GENERIC_READ,
            share_mode=_FILE_SHARE_READ,
            directory=False,
        )
        if _path_identity(manifest_final) != _path_identity(manifest_path):
            manifest_owner.close()
            raise RuntimeError(
                f"cache manifest final path mismatch: {manifest_path}"
            )
    except BaseException:
        protected_owner.close()
        raise
    owners: list[object] = [protected_owner, manifest_owner]
    try:
        return CachedLayoutLease(
            key=key,
            object_root=object_root,
            layout=layout,
            logical_files=len(entries),
            logical_bytes=logical_bytes,
            cache_hit=cache_hit,
            payload_files=payload_files,
            payload_bytes=payload_bytes,
            _owners=tuple(owners),
        )
    except BaseException:
        for owner in reversed(owners):
            close = getattr(owner, "close", None)
            if callable(close):
                close()
        raise


def _remove_staging_tree(path: Path, sid: str) -> None:
    if not os.path.lexists(path):
        return
    _set_tree_acl(path, sid, "DELETE")
    shutil.rmtree(path)


def _bounded_entry_snapshot(
    entries: LayoutEntries,
    *,
    entry_limit: int,
    byte_limit: int,
) -> tuple[tuple[Path, LayoutEntry], ...]:
    """Reject cheap structural/size failures before sorting or hashing keys."""
    if type(entry_limit) is not int or entry_limit < 0:
        raise RuntimeError(
            "runtime cache entry limit must be a nonnegative integer"
        )
    if type(byte_limit) is not int or byte_limit < 0:
        raise RuntimeError(
            "runtime cache byte limit must be a nonnegative integer"
        )

    snapshot: list[tuple[Path, LayoutEntry]] = []
    known_bytes = 0
    for item in entries:
        if len(snapshot) >= entry_limit:
            raise RuntimeError(
                "runtime cache entry limit exceeded: "
                f"more than {entry_limit} entries"
            )
        try:
            destination, entry = item
        except (TypeError, ValueError) as error:
            raise TypeError(
                "runtime cache entry must be a destination/entry pair"
            ) from error
        if not isinstance(destination, Path):
            raise TypeError("runtime cache destination must be a Path")
        _normalized_destination(destination)

        if isinstance(entry, _ResolvedHeldSource):
            _entry_key_input(entry)
            known_bytes += entry.size
        elif isinstance(entry, HeldSource):
            _normalized_held_final_path(entry.final_path)
            for field_name in ("device", "inode", "size"):
                value = getattr(entry, field_name)
                if type(value) is not int or value < 0:
                    raise RuntimeError(
                        f"invalid held source {field_name}: "
                        "expected nonnegative integer"
                    )
            if not isinstance(entry.sha256, str) or not _LOWER_SHA256.fullmatch(
                entry.sha256
            ):
                raise RuntimeError(
                    "invalid held source sha256: expected lowercase "
                    "64-character hex"
                )
            known_bytes += entry.size
        elif isinstance(entry, bytes):
            known_bytes += len(entry)
        elif isinstance(entry, GeneratedLayoutEntry):
            if not isinstance(entry.policy, str) or not entry.policy:
                raise RuntimeError(
                    "invalid generated policy: expected nonempty string"
                )
            if not callable(entry.render):
                raise RuntimeError("invalid generated render: expected callable")
        else:
            raise TypeError(f"unsupported layout entry: {type(entry).__name__}")

        if known_bytes > byte_limit:
            raise RuntimeError(
                "runtime cache byte limit exceeded: "
                f"{known_bytes} > {byte_limit}"
            )
        snapshot.append((destination, entry))
    return tuple(snapshot)


def _hold_final_object_root(object_root: Path) -> _OwnedHandle:
    owner, final = _open_canonical_non_reparse(
        object_root,
        object_root.parent,
        directory=True,
    )
    if _path_identity(final) != _path_identity(object_root):
        owner.close()
        raise RuntimeError(f"cache object final path mismatch: {object_root}")
    return owner


def _stamp_time(path: Path) -> float:
    try:
        return path.stat(follow_symlinks=False).st_mtime
    except OSError as error:
        raise RuntimeError(f"invalid cache lifecycle stamp: {path}") from error


def _stamp_is_fresh(path: Path, now: float, interval: int) -> bool:
    if not os.path.lexists(path):
        return False
    owner, _canonical = _open_canonical_non_reparse(
        path,
        path.parent,
        desired_access=_FILE_READ_ATTRIBUTES,
        share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
        directory=False,
    )
    try:
        _assert_handle_still_names(owner.value, path)
        age = now - _stamp_time(path)
        return 0 <= age < interval
    finally:
        owner.close()


def _write_stamp_no_replace(parent: Path, stamp: Path, now: float) -> None:
    temporary = parent / f".{stamp.name}.{uuid.uuid4()}.tmp"
    try:
        with temporary.open("xb") as output:
            output.write(f"{now:.9f}\n".encode("ascii"))
            output.flush()
            os.fsync(output.fileno())
        os.utime(temporary, (now, now))
        _move_file_no_replace_write_through(temporary, stamp)
    finally:
        if os.path.lexists(temporary):
            temporary.unlink()


def _remove_stamp(path: Path) -> None:
    if not os.path.lexists(path):
        return
    owner, _canonical = _open_canonical_non_reparse(
        path,
        path.parent,
        desired_access=_FILE_READ_ATTRIBUTES | _DELETE,
        share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
        directory=False,
    )
    try:
        _assert_handle_still_names(owner.value, path)
        path.unlink()
    finally:
        owner.close()


def _try_create_lock(cache: WindowsRuntimeCache, path: Path) -> Optional[_OwnedHandle]:
    if not _create_directory_with_acl(
        path,
        cache.sid,
        _acl_sddl(cache.sid, "PRIVATE"),
    ):
        return None
    try:
        now = cache.clock()
        os.utime(path, (now, now))
        owner, _canonical = _open_canonical_non_reparse(
            path,
            cache.locks,
            desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC | _DELETE,
            share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE,
            directory=True,
        )
        _assert_handle_still_names(owner.value, path)
        return owner
    except BaseException:
        if os.path.lexists(path):
            _set_tree_acl(path, cache.sid, "DELETE")
            path.rmdir()
        raise


def _release_lock(cache: WindowsRuntimeCache, path: Path, owner: _OwnedHandle) -> None:
    try:
        _assert_handle_still_names(owner.value, path)
        _set_handle_acl(owner.value, cache.sid, "DELETE")
    finally:
        owner.close()
    try:
        path.rmdir()
    except FileNotFoundError:
        pass


def _touch_cache_access(cache: WindowsRuntimeCache, key: str) -> None:
    if not _LOWER_SHA256.fullmatch(key):
        raise RuntimeError(f"invalid cache access key: {key}")
    now = cache.clock()
    stamp = cache.access / f"{key}.stamp"
    if _stamp_is_fresh(stamp, now, cache.limits.stamp_seconds):
        return

    lock = cache.locks / f"access-{key}"
    owner = _try_create_lock(cache, lock)
    if owner is None and _recover_stale_lock(
        cache,
        lock,
        cache.limits.stamp_seconds,
    ):
        owner = _try_create_lock(cache, lock)
    if owner is None:
        return
    try:
        if _stamp_is_fresh(stamp, now, cache.limits.stamp_seconds):
            return
        if os.path.lexists(stamp):
            _remove_stamp(stamp)
        try:
            _write_stamp_no_replace(cache.access, stamp, now)
        except FileExistsError:
            pass
    finally:
        _release_lock(cache, lock, owner)


@dataclass
class _RemovalGuard:
    path: Path
    directory: bool
    control: _OwnedHandle
    delete: _OwnedHandle
    original_sddl: str


def _acquire_removal_guards(
    cache: WindowsRuntimeCache,
    path: Path,
    parent: Path,
) -> list[_RemovalGuard]:
    guards: list[_RemovalGuard] = []

    def guard(target: Path, target_parent: Path, directory: bool) -> None:
        control, _canonical = _open_canonical_non_reparse(
            target,
            target_parent,
            desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC,
            share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
            directory=directory,
        )
        delete: Optional[_OwnedHandle] = None
        original_sddl = ""
        try:
            _assert_handle_still_names(control.value, target)
            original_sddl = _get_handle_acl_sddl(control.value)
            _set_handle_acl_sddl(
                control.value,
                _publication_root_sddl(cache.sid),
            )
            delete, _canonical = _open_canonical_non_reparse(
                target,
                target_parent,
                desired_access=_FILE_READ_ATTRIBUTES | _DELETE,
                share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
                directory=directory,
            )
            _assert_handle_still_names(delete.value, target)
            guards.append(
                _RemovalGuard(target, directory, control, delete, original_sddl)
            )
            if directory:
                with os.scandir(target) as scanned:
                    entries = sorted(
                        scanned,
                        key=lambda entry: ntpath.normcase(entry.name),
                    )
                for entry in entries:
                    child = Path(entry.path)
                    stat_result = entry.stat(follow_symlinks=False)
                    if (
                        getattr(stat_result, "st_file_attributes", 0)
                        & _FILE_ATTRIBUTE_REPARSE_POINT
                    ):
                        raise RuntimeError(
                            "cache lifecycle tree contains a reparse point: "
                            f"{child}"
                        )
                    guard(
                        child,
                        target,
                        entry.is_dir(follow_symlinks=False),
                    )
        except BaseException:
            if delete is not None and not any(
                item.delete is delete for item in guards
            ):
                delete.close()
            if not any(item.control is control for item in guards):
                if original_sddl:
                    _set_handle_acl_sddl(control.value, original_sddl)
                control.close()
            raise

    try:
        guard(path, parent, True)
        return guards
    except BaseException:
        for item in reversed(guards):
            try:
                _set_handle_acl_sddl(item.control.value, item.original_sddl)
                _assert_handle_still_names(item.control.value, item.path)
            finally:
                item.delete.close()
                item.control.close()
        raise


def _remove_cache_tree(
    cache: WindowsRuntimeCache,
    path: Path,
    parent: Path,
    *,
    quarantine_to: Optional[Path] = None,
) -> bool:
    if not os.path.lexists(path):
        return True
    try:
        guards = _acquire_removal_guards(cache, path, parent)
    except OSError as error:
        if getattr(error, "winerror", None) in (
            _ERROR_ACCESS_DENIED,
            _ERROR_SHARING_VIOLATION,
        ):
            return False
        raise
    original_acls = tuple(
        (item.path, item.directory, item.original_sddl) for item in guards
    )
    try:
        for item in reversed(guards):
            _set_handle_acl(item.control.value, cache.sid, "DELETE")
            _assert_handle_still_names(item.control.value, item.path)
        # The root DELETE handle prevents a new protected lease from entering.
        # Close descendants after the all-or-nothing sharing preflight because
        # Windows cannot rename a directory while descendants remain open,
        # even when those handles opted into delete sharing.
        for item in reversed(guards[1:]):
            item.delete.close()
            item.control.close()
        guards = guards[:1]
        try:
            if quarantine_to is None:
                shutil.rmtree(path)
            else:
                _move_file_no_replace_write_through(path, quarantine_to)
        except OSError as error:
            if getattr(error, "winerror", None) not in (
                _ERROR_ACCESS_DENIED,
                _ERROR_SHARING_VIOLATION,
            ):
                raise
            for target, directory, sddl in reversed(original_acls):
                if not os.path.lexists(target):
                    raise RuntimeError(
                        f"cache lifecycle removal was partial: {path}"
                    ) from error
                restore, _canonical = _open_canonical_non_reparse(
                    target,
                    target.parent,
                    desired_access=(
                        _FILE_READ_ATTRIBUTES | _READ_CONTROL | _WRITE_DAC
                    ),
                    share_mode=(
                        _FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE
                    ),
                    directory=directory,
                )
                try:
                    _set_handle_acl_sddl(restore.value, sddl)
                    _assert_handle_still_names(restore.value, target)
                finally:
                    restore.close()
            return False
        return True
    finally:
        for item in reversed(guards):
            item.delete.close()
            item.control.close()


def _object_logical_size(
    cache: WindowsRuntimeCache,
    object_root: Path,
    key: str,
) -> tuple[int, float]:
    if not _LOWER_SHA256.fullmatch(key):
        raise RuntimeError(f"invalid cache object name: {object_root}")
    owner, _canonical = _open_canonical_non_reparse(
        object_root,
        object_root.parent,
        desired_access=_FILE_READ_ATTRIBUTES | _READ_CONTROL,
        share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
        directory=True,
    )
    try:
        _assert_handle_still_names(owner.value, object_root)
        actual_sddl = _get_handle_acl_sddl(owner.value)
        expected_sddls = {
            _acl_sddl(cache.sid, "RX"),
            (
                "D:PAI"
                f"(A;;0x1200a9;;;{cache.sid})"
                f"(A;OICIIO;GXGR;;;{cache.sid})"
            ),
            (
                "D:PAI"
                f"(A;;0x1200a9;;;{cache.sid})"
                f"(A;OICIIO;GRGX;;;{cache.sid})"
            ),
        }
        if actual_sddl not in expected_sddls:
            raise RuntimeError(
                f"cache object ACL mismatch: {object_root}: "
                f"{actual_sddl!r}"
            )
        manifest = _read_manifest(object_root)
    finally:
        owner.close()
    if manifest.get("object_key") != key:
        raise RuntimeError(f"cache manifest object key mismatch: {object_root}")
    records = manifest.get("files")
    if not isinstance(records, list):
        raise RuntimeError(f"invalid cache manifest file records: {object_root}")
    logical_bytes = 0
    for record in records:
        if not isinstance(record, dict) or type(record.get("size")) is not int:
            raise RuntimeError(f"invalid cache manifest file record: {object_root}")
        size = record["size"]
        if size < 0:
            raise RuntimeError(f"invalid cache manifest file size: {object_root}")
        logical_bytes += size
    created = manifest.get("created")
    if (
        not isinstance(created, (int, float))
        or isinstance(created, bool)
        or not math.isfinite(created)
    ):
        raise RuntimeError(f"invalid cache manifest creation time: {object_root}")
    return logical_bytes, float(created)


def _remove_old_namespace_entries(
    cache: WindowsRuntimeCache,
    parent: Path,
    now: float,
) -> None:
    with os.scandir(parent) as scanned:
        entries = sorted(scanned, key=lambda entry: ntpath.normcase(entry.name))
    for entry in entries:
        path = Path(entry.path)
        if not entry.is_dir(follow_symlinks=False):
            raise RuntimeError(f"invalid cache lifecycle entry: {path}")
        if now - entry.stat(follow_symlinks=False).st_mtime < cache.limits.staging_seconds:
            continue
        _remove_cache_tree(cache, path, parent)


def _cleanup_cache_contents(
    cache: WindowsRuntimeCache,
    *,
    protected_key: Optional[str] = None,
) -> None:
    now = cache.clock()
    _remove_old_namespace_entries(cache, cache.staging, now)
    _remove_old_namespace_entries(cache, cache.quarantine, now)

    candidates: list[tuple[float, str, Path, Path, int]] = []
    total_bytes = 0
    with os.scandir(cache.objects) as scanned:
        object_entries = sorted(scanned, key=lambda entry: ntpath.normcase(entry.name))
    for entry in object_entries:
        object_root = Path(entry.path)
        if not entry.is_dir(follow_symlinks=False):
            raise RuntimeError(f"invalid cache object entry: {object_root}")
        key = entry.name
        logical_bytes, created = _object_logical_size(cache, object_root, key)
        stamp = cache.access / f"{key}.stamp"
        accessed = _stamp_time(stamp) if os.path.lexists(stamp) else created
        candidates.append((accessed, key, object_root, stamp, logical_bytes))
        total_bytes += logical_bytes

    candidates.sort(key=lambda item: (item[0], item[1]))
    remaining_objects = len(candidates)
    for accessed, key, object_root, stamp, logical_bytes in candidates:
        unused = now - accessed >= cache.limits.max_unused_seconds
        over_capacity = (
            total_bytes > cache.limits.max_bytes
            or remaining_objects > cache.limits.max_objects
        )
        if not unused and not over_capacity:
            continue
        if key == protected_key:
            continue
        if not _remove_cache_tree(cache, object_root, cache.objects):
            continue
        _remove_stamp(stamp)
        total_bytes -= logical_bytes
        remaining_objects -= 1

    if (
        total_bytes > cache.limits.max_bytes
        or remaining_objects > cache.limits.max_objects
    ):
        raise RuntimeError(
            "runtime cache capacity cannot be reached while objects are active"
        )


def _cleanup_cache(
    cache: WindowsRuntimeCache,
    *,
    force: bool,
    protected_key: Optional[str] = None,
    require_lock: bool = False,
) -> None:
    now = cache.clock()
    marker = cache.access / "cleanup.stamp"
    if not force and _stamp_is_fresh(marker, now, cache.limits.cleanup_seconds):
        return
    lock = cache.locks / "cleanup"
    owner = _try_create_lock(cache, lock)
    if owner is None and _recover_stale_lock(
        cache,
        lock,
        cache.limits.cleanup_seconds,
    ):
        owner = _try_create_lock(cache, lock)
    if owner is None:
        if require_lock:
            raise RuntimeError("runtime cache capacity cleanup is contended")
        return
    try:
        if not force and _stamp_is_fresh(marker, now, cache.limits.cleanup_seconds):
            return
        _cleanup_cache_contents(cache, protected_key=protected_key)
        if os.path.lexists(marker):
            _remove_stamp(marker)
        _write_stamp_no_replace(cache.access, marker, now)
    finally:
        _release_lock(cache, lock, owner)


def _rollback_published_object(
    cache: WindowsRuntimeCache,
    key: str,
    object_root: Path,
) -> None:
    if os.path.lexists(object_root) and not _remove_cache_tree(
        cache, object_root, cache.objects
    ):
        raise RuntimeError(
            f"failed to roll back published cache object: {object_root}"
        )
    _remove_stamp(cache.access / f"{key}.stamp")


def _publication_marker_path(cache: WindowsRuntimeCache, key: str) -> Path:
    if not _LOWER_SHA256.fullmatch(key):
        raise RuntimeError(f"invalid cache publication key: {key}")
    return cache.locks / f"publish-{key}"


def _publication_marker_exists(cache: WindowsRuntimeCache, key: str) -> bool:
    marker = _publication_marker_path(cache, key)
    if not os.path.lexists(marker):
        return False
    try:
        owner, _canonical = _open_canonical_non_reparse(
            marker,
            cache.locks,
            desired_access=_FILE_READ_ATTRIBUTES,
            share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
            directory=True,
        )
    except FileNotFoundError:
        return False
    try:
        _assert_handle_still_names(owner.value, marker)
        return True
    finally:
        owner.close()


def _await_publication_finalization(
    cache: WindowsRuntimeCache,
    key: str,
    *,
    deadline: Optional[float] = None,
) -> None:
    if deadline is None:
        deadline = time.monotonic() + _REPAIR_WAIT_SECONDS
    while _publication_marker_exists(cache, key):
        if time.monotonic() >= deadline:
            _recover_stale_publication(cache, key)
            return
        time.sleep(_REPAIR_POLL_SECONDS)


def _acquire_publication_marker(
    cache: WindowsRuntimeCache,
    key: str,
) -> tuple[Path, _OwnedHandle]:
    marker = _publication_marker_path(cache, key)
    deadline = time.monotonic() + _REPAIR_WAIT_SECONDS
    while True:
        owner = _try_create_lock(cache, marker)
        if owner is not None:
            return marker, owner
        _await_publication_finalization(cache, key, deadline=deadline)


def _hold_finalized_object_root(
    cache: WindowsRuntimeCache,
    object_root: Path,
    key: str,
) -> Optional[_OwnedHandle]:
    deadline: Optional[float] = None
    while True:
        if _publication_marker_exists(cache, key):
            if deadline is None:
                deadline = time.monotonic() + _REPAIR_WAIT_SECONDS
            _await_publication_finalization(cache, key, deadline=deadline)
            continue
        if not os.path.lexists(object_root):
            return None
        if _publication_marker_exists(cache, key):
            continue
        try:
            owner = _hold_final_object_root(object_root)
        except FileNotFoundError:
            continue
        if not _publication_marker_exists(cache, key):
            return owner
        owner.close()


def _recover_stale_lock(
    cache: WindowsRuntimeCache,
    lock: Path,
    stale_seconds: int,
) -> bool:
    if not os.path.lexists(lock):
        return False
    owner, _canonical = _open_canonical_non_reparse(
        lock,
        cache.locks,
        desired_access=_FILE_READ_ATTRIBUTES,
        share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
        directory=True,
    )
    try:
        _assert_handle_still_names(owner.value, lock)
        age = cache.clock() - _stamp_time(lock)
    finally:
        owner.close()
    if age < stale_seconds:
        return False
    return _remove_cache_tree(cache, lock, cache.locks)


def _acquire_repair_lock(
    cache: WindowsRuntimeCache,
    key: str,
) -> tuple[Path, _OwnedHandle]:
    lock = cache.locks / f"repair-{key}"
    deadline = time.monotonic() + _REPAIR_WAIT_SECONDS
    while True:
        owner = _try_create_lock(cache, lock)
        if owner is not None:
            return lock, owner
        if os.path.lexists(lock):
            lock_owner, _canonical = _open_canonical_non_reparse(
                lock,
                cache.locks,
                desired_access=_FILE_READ_ATTRIBUTES,
                share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
                directory=True,
            )
            try:
                _assert_handle_still_names(lock_owner.value, lock)
                stale = cache.clock() - _stamp_time(lock) >= _REPAIR_STALE_SECONDS
            finally:
                lock_owner.close()
            if stale and _remove_cache_tree(cache, lock, cache.locks):
                continue
        if time.monotonic() >= deadline:
            raise RuntimeError(f"runtime cache repair lock timeout: {key}")
        time.sleep(_REPAIR_POLL_SECONDS)


def _recover_stale_publication(
    cache: WindowsRuntimeCache,
    key: str,
) -> None:
    marker = _publication_marker_path(cache, key)
    repair_lock, repair_owner = _acquire_repair_lock(cache, key)
    try:
        if not _publication_marker_exists(cache, key):
            return
        marker_owner, _canonical = _open_canonical_non_reparse(
            marker,
            cache.locks,
            desired_access=_FILE_READ_ATTRIBUTES,
            share_mode=_FILE_SHARE_READ | _FILE_SHARE_WRITE | _FILE_SHARE_DELETE,
            directory=True,
        )
        try:
            _assert_handle_still_names(marker_owner.value, marker)
            age = cache.clock() - _stamp_time(marker)
        finally:
            marker_owner.close()
        if age < _REPAIR_STALE_SECONDS:
            raise RuntimeError(f"runtime cache publication timeout: {key}")

        object_root = cache.objects / key
        capacity_error: Optional[BaseException] = None
        if os.path.lexists(object_root):
            try:
                _cleanup_cache(
                    cache,
                    force=True,
                    protected_key=key,
                    require_lock=True,
                )
                cache._touch_access(key)
            except BaseException as error:
                capacity_error = error
                _rollback_published_object(cache, key, object_root)

        if not _remove_cache_tree(cache, marker, cache.locks):
            raise RuntimeError(
                f"runtime cache publication is still active: {key}"
            )
        if capacity_error is not None:
            raise capacity_error
    finally:
        _release_lock(cache, repair_lock, repair_owner)


def _repair_cache_object(
    cache: WindowsRuntimeCache,
    key: str,
    build: Callable[[], CachedLayoutLease],
) -> CachedLayoutLease:
    if not _LOWER_SHA256.fullmatch(key):
        raise RuntimeError(f"invalid cache repair key: {key}")
    lock, owner = _acquire_repair_lock(cache, key)
    try:
        try:
            return build()
        except _CacheObjectValidationError as validation_error:
            object_root = cache.objects / key
            if not os.path.lexists(object_root):
                raise
            quarantine = cache.quarantine / f"{key}-{uuid.uuid4()}"
            if not _remove_cache_tree(
                cache,
                object_root,
                cache.objects,
                quarantine_to=quarantine,
            ):
                raise RuntimeError(
                    f"cache object is active and cannot be repaired: {object_root}"
                ) from validation_error
            quarantined_at = cache.clock()
            os.utime(quarantine, (quarantined_at, quarantined_at))
            _remove_stamp(cache.access / f"{key}.stamp")
        return build()
    finally:
        _release_lock(cache, lock, owner)


def _entries_with_generated_qt_conf(
    entries: LayoutEntries,
    qt_paths: QtConfPaths | None,
    dependencies: Sequence[CachedLayoutLease],
    *,
    entry_limit: int,
    byte_limit: int,
) -> tuple[tuple[Path, LayoutEntry], ...]:
    validated = _bounded_entry_snapshot(
        entries, entry_limit=entry_limit, byte_limit=byte_limit
    )
    if qt_paths is None:
        return validated

    generated = generated_qt_conf(qt_paths)
    dependency_layouts: set[str] = set()
    for dependency in dependencies:
        if not isinstance(dependency, CachedLayoutLease):
            raise TypeError("runtime cache dependency must be a CachedLayoutLease")
        if not dependency._owners:
            raise RuntimeError(
                f"runtime cache dependency lease is closed: {dependency.layout}"
            )
        dependency_layouts.add(_path_identity(dependency.layout))
    for label, path in (
        ("plugins", qt_paths.plugins),
        ("QML imports", qt_paths.qml_imports),
    ):
        if _path_identity(path) not in dependency_layouts:
            raise RuntimeError(
                f"Qt configuration path is not an exact {label} dependency layout: "
                f"{path}"
            )

    executable_parents: dict[str, Path] = {}
    for destination, _entry in validated:
        if ntpath.normcase(destination.suffix) != ".exe":
            continue
        parent = destination.parent
        executable_parents.setdefault(
            ntpath.normcase(ntpath.normpath(str(parent))), parent
        )

    filtered: list[tuple[Path, LayoutEntry]] = []
    for destination, entry in validated:
        parent_key = ntpath.normcase(ntpath.normpath(str(destination.parent)))
        if (
            parent_key in executable_parents
            and ntpath.normcase(destination.name) == "qt.conf"
        ):
            continue
        filtered.append((destination, entry))
    for parent_key in sorted(executable_parents):
        parent = executable_parents[parent_key]
        filtered.append((parent / "qt.conf", generated))

    # Revalidate the complete replacement sequence before key construction or
    # any cache mutation. This also accounts for every generated configuration
    # in the caller's entry bound.
    return _bounded_entry_snapshot(
        tuple(filtered), entry_limit=entry_limit, byte_limit=byte_limit
    )


def _materialize_layout(
    cache: WindowsRuntimeCache,
    name: str,
    entries: LayoutEntries,
    *,
    context: Mapping[str, JsonValue],
    dependencies: Sequence[CachedLayoutLease],
    qt_paths: QtConfPaths | None,
    entry_limit: int,
    byte_limit: int,
    _repairing: bool = False,
    _expected_key: Optional[str] = None,
) -> CachedLayoutLease:
    entry_snapshot = _entries_with_generated_qt_conf(
        entries,
        qt_paths,
        dependencies,
        entry_limit=entry_limit,
        byte_limit=byte_limit,
    )
    dependency_keys = tuple(dependency.key for dependency in dependencies)
    # This first key pass performs every spelling, descriptor, dependency, and
    # JSON snapshot validation before resolving sources or mutating cache state.
    key, key_input = layout_key(name, entry_snapshot, context, dependency_keys)
    if _expected_key is not None and key != _expected_key:
        raise RuntimeError("runtime cache key input changed during repair")

    def retry_under_repair() -> CachedLayoutLease:
        return _materialize_layout(
            cache,
            name,
            entry_snapshot,
            context=key_input["context"],
            dependencies=dependencies,
            qt_paths=qt_paths,
            entry_limit=entry_limit,
            byte_limit=byte_limit,
            _repairing=True,
            _expected_key=key,
        )

    resolved_sources: dict[int, HeldSource] = {}
    rendered_entries: dict[str, bytes] = {}
    try:
        final_object_root = cache.objects / key
        known_bytes = 0
        for index, (destination, entry) in enumerate(entry_snapshot):
            if isinstance(entry, HeldSource):
                resolved = _revalidate_held_source(entry, cache._resolver_token)
                resolved_sources[index] = resolved
                known_bytes += resolved.size
            elif isinstance(entry, bytes):
                known_bytes += len(entry)
            if known_bytes > byte_limit:
                raise RuntimeError(
                    f"runtime cache byte limit exceeded: {known_bytes} > {byte_limit}"
                )

        stored_layout_name = name
        existing_owner = _hold_finalized_object_root(
            cache,
            final_object_root,
            key,
        )
        existing_manifest: Mapping[str, object] | None = None
        try:
            if existing_owner is not None:
                try:
                    manifest = _read_manifest(final_object_root)
                    existing_manifest = manifest
                    candidate = manifest.get("layout_name")
                    if (
                        not isinstance(candidate, str)
                        or _validate_win32_component(
                            candidate,
                            label="layout name",
                        )
                        != key_input.get("name")
                    ):
                        raise RuntimeError(
                            "cache manifest layout spelling mismatch: "
                            f"{final_object_root}"
                        )
                except RuntimeError as error:
                    if _repairing:
                        raise _CacheObjectValidationError(str(error)) from error
                    _assert_handle_still_names(
                        existing_owner.value,
                        final_object_root,
                    )
                    existing_owner.close()
                    existing_owner = None
                    return cache._repair(key, retry_under_repair)
                stored_layout_name = candidate
            final_layout = final_object_root / stored_layout_name
            non_generated_bytes = known_bytes
            for destination, entry in entry_snapshot:
                if isinstance(entry, GeneratedLayoutEntry):
                    payload = entry.render(final_layout)
                    if not isinstance(payload, bytes):
                        raise RuntimeError(
                            f"generated cache entry did not render bytes: {destination}"
                        )
                    rendered_entries[_normalized_destination(destination)] = payload
                    known_bytes += len(payload)
                if known_bytes > byte_limit:
                    raise RuntimeError(
                        f"runtime cache byte limit exceeded: {known_bytes} > {byte_limit}"
                    )

            if existing_owner is not None:
                try:
                    lease = _hold_validated_object(
                        cache,
                        final_object_root,
                        key,
                        key_input,
                        entry_snapshot,
                        cache_hit=True,
                        payload_files=0,
                        payload_bytes=0,
                        rendered_entries=rendered_entries,
                        manifest=existing_manifest,
                    )
                except RuntimeError as error:
                    if _repairing:
                        raise _CacheObjectValidationError(str(error)) from error
                    _assert_handle_still_names(
                        existing_owner.value,
                        final_object_root,
                    )
                    existing_owner.close()
                    existing_owner = None
                    return cache._repair(key, retry_under_repair)
                try:
                    cache._touch_access(key)
                    cache.cleanup()
                    return lease
                except BaseException:
                    lease.close()
                    raise
        finally:
            if existing_owner is not None:
                existing_owner.close()

        staging_root = cache.staging / str(uuid.uuid4())
        while not _create_directory_with_acl(
            staging_root, cache.sid, _acl_sddl(cache.sid, "BUILD")
        ):
            staging_root = cache.staging / str(uuid.uuid4())
        sealed: Optional[_SealedTree] = None
        publication_marker: Optional[Path] = None
        publication_owner: Optional[_OwnedHandle] = None
        published = False
        try:
            staging_layout = staging_root / name
            staging_layout.mkdir()
            file_records: list[dict[str, object]] = []
            payload_bytes = 0
            for index, (destination, entry) in enumerate(entry_snapshot):
                target = staging_layout / destination
                target.parent.mkdir(parents=True, exist_ok=True)
                if isinstance(entry, HeldSource):
                    resolved = resolved_sources[index]
                    resolved.stream.seek(0)
                    with target.open("wb") as output:
                        shutil.copyfileobj(resolved.stream, output, 1024 * 1024)
                    resolved.stream.seek(0)
                    payload = target.read_bytes()
                    source_final_path: Optional[str] = _normalized_held_final_path(
                        resolved.final_path
                    )
                elif isinstance(entry, bytes):
                    payload = entry
                    target.write_bytes(payload)
                    source_final_path = None
                else:
                    payload = rendered_entries[_normalized_destination(destination)]
                    target.write_bytes(payload)
                    source_final_path = None
                payload_bytes += len(payload)
                if payload_bytes > byte_limit:
                    raise RuntimeError(
                        f"runtime cache byte limit exceeded: {payload_bytes} > {byte_limit}"
                    )
                record: dict[str, object] = {
                    "destination": destination.as_posix(),
                    "kind": _entry_kind(entry),
                    "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
                if source_final_path is not None:
                    record["source_final_path"] = source_final_path
                file_records.append(record)
            file_records.sort(
                key=lambda record: _normalized_destination(
                    Path(str(record["destination"]))
                )
            )
            manifest: dict[str, object] = {
                "schema": SCHEMA_VERSION,
                "closure_policy": CLOSURE_POLICY_VERSION,
                "key_input": key_input,
                "object_key": key,
                "layout_name": name,
                "created": cache.clock(),
                "files": file_records,
            }
            # Completion marker: this is deliberately the final staging write.
            (staging_root / "manifest.json").write_bytes(canonical_json(manifest))
            _validate_object_contents(
                staging_root,
                final_object_root,
                key,
                key_input,
                entry_snapshot,
                require_acl=False,
                rendered_entries=rendered_entries,
            )
            sealed = _seal_tree_acl(staging_root, cache.sid, "RX")
            try:
                publication_marker, publication_owner = (
                    _acquire_publication_marker(cache, key)
                )
                lost_publication = os.path.lexists(final_object_root)
                if not lost_publication:
                    try:
                        _atomic_move_no_replace(
                            staging_root, final_object_root, sealed=sealed
                        )
                        published = True
                    except FileExistsError:
                        lost_publication = True
                if not lost_publication:
                    sealed.close()
                    sealed = None
                else:
                    _release_lock(
                        cache,
                        publication_marker,
                        publication_owner,
                    )
                    publication_marker = None
                    publication_owner = None
            except BaseException:
                sealed.close()
                sealed = None
                raise
            if not published:
                _remove_staging_tree(staging_root, cache.sid)
                winner_owner = _hold_finalized_object_root(
                    cache,
                    final_object_root,
                    key,
                )
                if winner_owner is None:
                    raise RuntimeError(
                        f"runtime cache publication was rolled back: {key}"
                    )
                try:
                    winner_manifest = _read_manifest(final_object_root)
                    winner_layout_name = winner_manifest.get("layout_name")
                    if (
                        not isinstance(winner_layout_name, str)
                        or _validate_win32_component(
                            winner_layout_name, label="layout name"
                        )
                        != key_input.get("name")
                    ):
                        raise RuntimeError(
                            "cache manifest layout spelling mismatch: "
                            f"{final_object_root}"
                        )
                    winner_layout = final_object_root / winner_layout_name
                    if winner_layout_name != stored_layout_name:
                        winner_rendered_entries: dict[str, bytes] = {}
                        winner_bytes = non_generated_bytes
                        for destination, entry in entry_snapshot:
                            if not isinstance(entry, GeneratedLayoutEntry):
                                continue
                            payload = entry.render(winner_layout)
                            if not isinstance(payload, bytes):
                                raise RuntimeError(
                                    "generated cache entry did not render bytes: "
                                    f"{destination}"
                                )
                            winner_rendered_entries[
                                _normalized_destination(destination)
                            ] = payload
                            winner_bytes += len(payload)
                            if winner_bytes > byte_limit:
                                raise RuntimeError(
                                    "runtime cache byte limit exceeded: "
                                    f"{winner_bytes} > {byte_limit}"
                                )
                        rendered_entries = winner_rendered_entries
                    lease = _hold_validated_object(
                        cache,
                        final_object_root,
                        key,
                        key_input,
                        entry_snapshot,
                        cache_hit=True,
                        payload_files=len(entry_snapshot),
                        payload_bytes=payload_bytes,
                        rendered_entries=rendered_entries,
                    )
                    try:
                        cache._touch_access(key)
                        cache.cleanup()
                        return lease
                    except BaseException:
                        lease.close()
                        raise
                finally:
                    winner_owner.close()
            lease: Optional[CachedLayoutLease] = None
            try:
                _cleanup_cache(
                    cache,
                    force=True,
                    protected_key=key,
                    require_lock=True,
                )
                cache._touch_access(key)
                lease = _hold_validated_object(
                    cache,
                    final_object_root,
                    key,
                    key_input,
                    entry_snapshot,
                    cache_hit=False,
                    payload_files=len(entry_snapshot),
                    payload_bytes=payload_bytes,
                    rendered_entries=rendered_entries,
                )
                _release_lock(
                    cache,
                    publication_marker,
                    publication_owner,
                )
                publication_marker = None
                publication_owner = None
                return lease
            except BaseException:
                if lease is not None:
                    lease.close()
                _rollback_published_object(cache, key, final_object_root)
                raise
        except BaseException:
            if sealed is not None:
                sealed.close()
            if publication_owner is not None and publication_marker is not None:
                _release_lock(cache, publication_marker, publication_owner)
            if not published and os.path.lexists(staging_root):
                _remove_staging_tree(staging_root, cache.sid)
            raise
    finally:
        rendered_entries.clear()


def canonical_json(value: JsonValue) -> bytes:
    normalized = _json_snapshot(value)
    return json.dumps(
        normalized,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def _json_snapshot(
    value: object,
    *,
    location: str = "$",
    active_containers: set[int] | None = None,
) -> object:
    if value is None:
        return value
    if isinstance(value, bool):
        return bool(value)
    if isinstance(value, int):
        return int(value)
    if isinstance(value, str):
        return _json_string(value, location=location)
    if isinstance(value, float):
        if math.isfinite(value):
            return float(value)
        raise RuntimeError(f"invalid JSON value at {location}: non-finite float")

    if active_containers is None:
        active_containers = set()
    if isinstance(value, Mapping):
        marker = id(value)
        if marker in active_containers:
            raise RuntimeError(f"invalid JSON value at {location}: cyclic mapping")
        active_containers.add(marker)
        try:
            result: dict[str, object] = {}
            for key, nested in value.items():
                if not isinstance(key, str):
                    raise RuntimeError(
                        f"invalid JSON value at {location}: mapping key is not a string"
                    )
                normalized_key = _json_string(key, location=f"{location}.<key>")
                result[normalized_key] = _json_snapshot(
                    nested,
                    location=f"{location}.{normalized_key}",
                    active_containers=active_containers,
                )
            return result
        finally:
            active_containers.remove(marker)
    if isinstance(value, (list, tuple)):
        marker = id(value)
        if marker in active_containers:
            raise RuntimeError(f"invalid JSON value at {location}: cyclic sequence")
        active_containers.add(marker)
        try:
            return [
                _json_snapshot(
                    nested,
                    location=f"{location}[{index}]",
                    active_containers=active_containers,
                )
                for index, nested in enumerate(value)
            ]
        finally:
            active_containers.remove(marker)
    raise RuntimeError(
        f"invalid JSON value at {location}: unsupported {type(value).__name__}"
    )


def _json_string(value: str, *, location: str) -> str:
    normalized = str(value)
    if _UNICODE_SURROGATE.search(normalized):
        raise RuntimeError(
            f"invalid JSON value at {location}: Unicode surrogate code point"
        )
    return normalized


def _json_mapping_snapshot(value: object, *, location: str) -> dict[str, object]:
    if not isinstance(value, Mapping):
        raise RuntimeError(f"invalid JSON value at {location}: expected mapping")
    snapshot = _json_snapshot(value, location=location)
    assert isinstance(snapshot, dict)
    return snapshot


_UNICODE_SURROGATE = re.compile("[\ud800-\udfff]")
_INVALID_WIN32_COMPONENT = re.compile(r'[\x00-\x1f<>:"/\\|?*]')
_RESERVED_DOS_BASENAME = re.compile(
    r"^(?:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|CLOCK\$|"
    r"COM[1-9¹²³]|LPT[1-9¹²³])$"
)
_LOWER_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_FORBIDDEN_FINAL_PATH_PREFIXES = (
    "\\\\?\\",
    "\\\\.\\",
    "\\??\\",
    "\\\\??\\",
    "\\device\\",
    "\\\\device\\",
)


def _validate_win32_component(component: object, *, label: str) -> str:
    if (
        not isinstance(component, str)
        or not component
        or component in (".", "..")
    ):
        raise RuntimeError(f"unsafe {label}: {component}")
    if component.endswith((".", " ")):
        raise RuntimeError(f"unsafe {label}: {component}")
    if _INVALID_WIN32_COMPONENT.search(component):
        raise RuntimeError(f"unsafe {label}: {component}")

    basename = component.split(".", 1)[0].rstrip(" .").upper()
    if _RESERVED_DOS_BASENAME.fullmatch(basename):
        raise RuntimeError(f"unsafe {label}: {component}")
    return ntpath.normcase(component)


def _normalized_destination(destination: Path) -> str:
    if destination.anchor or destination.is_absolute():
        raise RuntimeError(f"unsafe destination: {destination}")

    parts = destination.parts
    if not parts:
        raise RuntimeError(f"unsafe destination: {destination}")
    return "/".join(
        _validate_win32_component(part, label="destination") for part in parts
    )


def _normalized_held_final_path(value: object) -> str:
    if not isinstance(value, Path):
        raise RuntimeError("invalid held source final_path: expected Path")

    raw_path = str(value).replace("/", "\\")
    normalized_raw_path = ntpath.normcase(raw_path)
    if normalized_raw_path.startswith(_FORBIDDEN_FINAL_PATH_PREFIXES):
        raise RuntimeError("invalid held source final_path: namespace prefix")

    final_path = PureWindowsPath(raw_path)
    if not final_path.is_absolute() or final_path.root != "\\":
        raise RuntimeError(
            "invalid held source final_path: expected drive-rooted or UNC path"
        )

    def validated_component(component: str) -> str:
        try:
            return _validate_win32_component(
                component, label="held source final_path component"
            )
        except RuntimeError as error:
            raise RuntimeError(
                f"invalid held source final_path component: {error}"
            ) from None

    drive = final_path.drive
    if re.fullmatch(r"[A-Za-z]:", drive):
        normalized_anchor = ntpath.normcase(drive)
    elif drive.startswith("\\\\"):
        unc_parts = drive[2:].split("\\")
        if len(unc_parts) != 2:
            raise RuntimeError("invalid held source final_path: malformed UNC anchor")
        server, share = (
            validated_component(component) for component in unc_parts
        )
        normalized_anchor = f"//{server}/{share}"
    else:
        raise RuntimeError("invalid held source final_path: unsupported anchor")

    components = final_path.parts[1:]
    if not components:
        raise RuntimeError("invalid held source final_path: missing file component")
    normalized_components = [
        validated_component(component) for component in components
    ]
    return f"{normalized_anchor}/{'/'.join(normalized_components)}"


def _entry_key_input(entry: LayoutEntry) -> dict[str, object]:
    if isinstance(entry, _ResolvedHeldSource):
        return dict(entry.key_input)
    if isinstance(entry, HeldSource):
        final_path = _normalized_held_final_path(entry.final_path)
        for field_name in ("device", "inode", "size"):
            value = getattr(entry, field_name)
            if type(value) is not int or value < 0:
                raise RuntimeError(
                    f"invalid held source {field_name}: expected nonnegative integer"
                )
        if not isinstance(entry.sha256, str) or not _LOWER_SHA256.fullmatch(
            entry.sha256
        ):
            raise RuntimeError(
                "invalid held source sha256: expected lowercase 64-character hex"
            )
        return {
            "kind": "held-source",
            "final_path": final_path,
            "device": entry.device,
            "inode": entry.inode,
            "size": entry.size,
            "sha256": entry.sha256,
        }
    if isinstance(entry, bytes):
        return {
            "kind": "literal-bytes",
            "size": len(entry),
            "sha256": hashlib.sha256(entry).hexdigest(),
        }
    if isinstance(entry, GeneratedLayoutEntry):
        if not isinstance(entry.policy, str) or not entry.policy:
            raise RuntimeError("invalid generated policy: expected nonempty string")
        return {
            "kind": "generated",
            "policy": str(entry.policy),
            "inputs": _json_mapping_snapshot(
                entry.inputs, location="generated entry inputs"
            ),
        }
    raise TypeError(f"unsupported layout entry: {type(entry).__name__}")


def layout_key(
    name: str,
    entries: LayoutEntries,
    context: Mapping[str, JsonValue],
    dependency_keys: Sequence[str],
) -> tuple[str, dict[str, object]]:
    normalized_name = _validate_win32_component(name, label="layout name")
    context_snapshot = _json_mapping_snapshot(context, location="context")
    normalized_dependency_keys: list[str] = []
    seen_dependency_keys: set[str] = set()
    for dependency_key in dependency_keys:
        if not isinstance(dependency_key, str) or not _LOWER_SHA256.fullmatch(
            dependency_key
        ):
            raise RuntimeError(
                "invalid dependency key: expected lowercase 64-character hex"
            )
        normalized_dependency_key = str(dependency_key)
        if normalized_dependency_key in seen_dependency_keys:
            raise RuntimeError(f"duplicate dependency key: {normalized_dependency_key}")
        seen_dependency_keys.add(normalized_dependency_key)
        normalized_dependency_keys.append(normalized_dependency_key)
    normalized_dependency_keys.sort()
    normalized_entries: list[tuple[str, Path, LayoutEntry]] = []
    seen_destinations: set[str] = set()
    for destination, entry in entries:
        normalized = _normalized_destination(destination)
        if normalized in seen_destinations:
            raise RuntimeError(f"duplicate destination: {destination}")
        seen_destinations.add(normalized)
        normalized_entries.append((normalized, destination, entry))

    encoded_entries = []
    for normalized, _destination, entry in sorted(
        normalized_entries, key=lambda item: item[0]
    ):
        encoded_entries.append(
            {
                "normalized_destination": normalized,
                "entry": _entry_key_input(entry),
            }
        )

    key_input: dict[str, object] = {
        "schema": SCHEMA_VERSION,
        "closure_policy": CLOSURE_POLICY_VERSION,
        "name": normalized_name,
        "context": context_snapshot,
        "dependency_keys": normalized_dependency_keys,
        "entries": encoded_entries,
    }
    digest = hashlib.sha256(canonical_json(key_input)).hexdigest()
    return digest, key_input
