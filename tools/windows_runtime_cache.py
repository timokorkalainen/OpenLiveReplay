"""Protected Windows runtime cache model and canonical key encoding."""

from __future__ import annotations

import hashlib
import json
import math
import ntpath
import re
from dataclasses import dataclass, field
from pathlib import Path, PureWindowsPath
from typing import BinaryIO, Callable, List, Mapping, Sequence, Tuple, Union


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
