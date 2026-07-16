"""Protected Windows runtime cache model and canonical key encoding."""

from __future__ import annotations

import ctypes
import hashlib
import json
import math
import ntpath
import os
import re
import time
import uuid
from ctypes import wintypes
from dataclasses import dataclass, field
from pathlib import Path, PureWindowsPath
from typing import (
    BinaryIO,
    Callable,
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

_GENERIC_WRITE = 0x40000000
_GENERIC_READ = 0x80000000
_DELETE = 0x00010000
_FILE_ADD_FILE = 0x00000002
_FILE_ADD_SUBDIRECTORY = 0x00000004
_FILE_DELETE_CHILD = 0x00000040
_READ_CONTROL = 0x00020000
_WRITE_DAC = 0x00040000

_ERROR_ACCESS_DENIED = 5
_ERROR_FILE_EXISTS = 80
_ERROR_ALREADY_EXISTS = 183
_MOVEFILE_WRITE_THROUGH = 0x00000008
_FILE_BEGIN = 0

_DACL_SECURITY_INFORMATION = 0x00000004
_PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000
_SE_FILE_OBJECT = 1


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
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
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
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
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
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
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
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
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
    buffer = ctypes.create_string_buffer(1024 * 1024)
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
        validate_locked_tree()

        # The exact tree is now namespace-frozen. Convert root-first and verify
        # the same handle identities, sizes, and bytes after every final ACL.
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
        validate_locked_tree()

        root_owner = nodes[0].owner
        for node in nodes[1:]:
            node.owner.close()
        # Closed descendant handles prove the final RX DACL itself, rather than
        # a sharing violation, denies mutation.
        for node in nodes[1:]:
            _assert_mutation_denied(node.path, directory=node.entry.directory)
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
    """Require the immutable ACL to deny the exact mutation access mask."""
    if os.name != "nt":
        raise OSError("Windows mutation probes require Windows")
    kernel32 = _kernel32()
    desired_access = (
        _FILE_ADD_FILE | _FILE_ADD_SUBDIRECTORY | _FILE_DELETE_CHILD | _DELETE
        if directory
        else _GENERIC_WRITE | _DELETE
    )
    flags = _FILE_FLAG_BACKUP_SEMANTICS if directory else 0
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
        raise RuntimeError(f"cache mutation access unexpectedly permitted: {path}")
    error = ctypes.get_last_error()
    if error != _ERROR_ACCESS_DENIED:
        raise OSError(error, f"cache mutation probe failed unexpectedly: {path}")


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
    _validate_sealed_tree_at(sealed, source)

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
        _validate_sealed_tree_at(sealed, destination)
    finally:
        try:
            _set_handle_acl(sealed.handle, sealed.sid, "RX")
            _assert_handle_still_names(sealed.handle, publication_path)
            _validate_sealed_tree_at(sealed, publication_path)
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
    if any(0xD800 <= ord(character) <= 0xDFFF for character in normalized):
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


_INVALID_WIN32_COMPONENT_CHARACTERS = frozenset('<>:"/\\|?*')
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
    if any(
        ord(character) < 32 or character in _INVALID_WIN32_COMPONENT_CHARACTERS
        for character in component
    ):
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
