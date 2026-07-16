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
_FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
_FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
_FILE_READ_ATTRIBUTES = 0x00000080
_FILE_SHARE_READ = 0x00000001
_FILE_SHARE_WRITE = 0x00000002
_FILE_SHARE_DELETE = 0x00000004
_OPEN_EXISTING = 3
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

_GENERIC_WRITE = 0x40000000
_DELETE = 0x00010000
_FILE_ADD_FILE = 0x00000002
_FILE_ADD_SUBDIRECTORY = 0x00000004
_FILE_DELETE_CHILD = 0x00000040

_ERROR_ACCESS_DENIED = 5
_ERROR_FILE_EXISTS = 80
_ERROR_ALREADY_EXISTS = 183
_MOVEFILE_WRITE_THROUGH = 0x00000008

_DACL_SECURITY_INFORMATION = 0x00000004
_PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000


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
        self.root = _ensure_private_directory_tree(
            local_app_data_cache_root() if root is None else root,
            sid,
        )
        self.objects = _ensure_private_directory_tree(self.root / "objects", sid)
        self.staging = _ensure_private_directory_tree(self.root / "staging", sid)
        self.quarantine = _ensure_private_directory_tree(
            self.root / "quarantine", sid
        )
        self.access = _ensure_private_directory_tree(self.root / "access", sid)
        self.locks = _ensure_private_directory_tree(self.root / "locks", sid)


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
    kernel32.MoveFileExW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.LPCWSTR,
        wintypes.DWORD,
    )
    kernel32.MoveFileExW.restype = wintypes.BOOL
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


def _assert_canonical_non_reparse_tree(
    path: Path, parent: Optional[Path]
) -> Path:
    """Hold one path without following aliases and require lexical identity."""
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
        _FILE_READ_ATTRIBUTES,
        _FILE_SHARE_READ | _FILE_SHARE_WRITE,
        None,
        _OPEN_EXISTING,
        _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT,
        None,
    )
    if handle == _INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
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
        final = _final_path_from_handle(kernel32, int(handle))
    finally:
        kernel32.CloseHandle(handle)

    if _path_identity(lexical) != _path_identity(final):
        raise RuntimeError(
            f"cache path lexical/final mismatch: {lexical} != {final}"
        )
    return final


def _ensure_private_directory_tree(path: Path, sid: str) -> Path:
    lexical = _absolute_lexical(path)
    if not lexical.anchor:
        raise RuntimeError(f"cache root must be absolute: {path}")
    current = Path(lexical.anchor)
    canonical_parent = _assert_canonical_non_reparse_tree(current, None)
    for component in lexical.parts[1:]:
        current /= component
        if not os.path.lexists(current):
            try:
                os.mkdir(current)
            except FileExistsError:
                pass
            else:
                _set_path_acl(current, sid, "PRIVATE")
        canonical_parent = _assert_canonical_non_reparse_tree(
            current, canonical_parent
        )
        if not current.is_dir():
            raise RuntimeError(f"cache directory is not a directory: {current}")
    _set_path_acl(lexical, sid, "PRIVATE")
    return lexical


def _security_descriptor(sid: str, mode: _AclMode) -> tuple[object, object]:
    acl_sddl = {
        "PRIVATE": f"D:P(A;OICI;FA;;;{sid})",
        "BUILD": f"D:P(A;OICI;FA;;;{sid})",
        "RX": f"D:P(A;OICI;GRGX;;;{sid})",
        "DELETE": f"D:P(A;OICI;FA;;;{sid})",
    }
    try:
        sddl = acl_sddl[mode]
    except KeyError as error:
        raise ValueError(f"unsupported cache ACL mode: {mode}") from error

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


def _set_path_acl(path: Path, sid: str, mode: _AclMode) -> None:
    if os.name != "nt":
        raise OSError("Windows ACL conversion requires Windows")
    descriptor, kernel32 = _security_descriptor(sid, mode)
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    advapi32.SetFileSecurityW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        ctypes.c_void_p,
    )
    advapi32.SetFileSecurityW.restype = wintypes.BOOL
    try:
        if not advapi32.SetFileSecurityW(
            str(path),
            _DACL_SECURITY_INFORMATION | _PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor,
        ):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
        kernel32.LocalFree.restype = ctypes.c_void_p
        kernel32.LocalFree(descriptor)


def _enumerate_non_reparse_tree(path: Path) -> tuple[list[Path], list[Path]]:
    root = _absolute_lexical(path)
    _assert_canonical_non_reparse_tree(root, root.parent)
    directories: list[Path] = []
    files: list[Path] = []

    def visit(directory: Path) -> None:
        with os.scandir(directory) as entries:
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
                _assert_canonical_non_reparse_tree(target, directory)
                if entry.is_dir(follow_symlinks=False):
                    directories.append(target)
                    visit(target)
                else:
                    files.append(target)

    visit(root)
    return directories, files


def _set_tree_acl(path: Path, sid: str, mode: _AclMode) -> None:
    """Apply a protected DACL, recursively for immutable/delete conversions."""
    root = _absolute_lexical(path)
    if mode not in ("RX", "DELETE"):
        _assert_canonical_non_reparse_tree(root, root.parent)
        _set_path_acl(root, sid, mode)
        return

    directories, files = _enumerate_non_reparse_tree(root)
    if mode == "RX":
        targets = [
            *files,
            *sorted(directories, key=lambda value: len(value.parts), reverse=True),
            root,
        ]
    else:
        targets = [
            root,
            *sorted(directories, key=lambda value: len(value.parts)),
            *files,
        ]
    for target in targets:
        _assert_canonical_non_reparse_tree(target, target.parent)
        _set_path_acl(target, sid, mode)


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


def _atomic_move_no_replace(source: Path, destination: Path) -> None:
    """Atomically publish a tree without replacing an existing creator winner."""
    if os.name != "nt":
        raise OSError("Windows atomic publication requires Windows")
    kernel32 = _kernel32()
    if kernel32.MoveFileExW(
        str(source), str(destination), _MOVEFILE_WRITE_THROUGH
    ):
        return
    error = ctypes.get_last_error()
    if error in (_ERROR_FILE_EXISTS, _ERROR_ALREADY_EXISTS):
        raise FileExistsError(
            error,
            f"cache publication destination already exists: {destination}",
            str(destination),
        )
    raise ctypes.WinError(error)


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
