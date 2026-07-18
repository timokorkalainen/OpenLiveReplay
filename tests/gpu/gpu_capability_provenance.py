"""Streaming, fail-closed provenance for compiler-expanded GPU audit tokens."""

from __future__ import annotations

import json
import ntpath
import os
import re
import stat
import sys
import time
from array import array
from collections.abc import Callable, Mapping
from pathlib import Path, PurePosixPath, PureWindowsPath

from gpu_capability_model import (
    UINT32_MAX,
    AuditInfrastructureError,
    AuditLimits,
    CompactTokenSequence,
    CompilerFamily,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
)


_GCC_MARKER = re.compile(
    rb'^\s*#\s+([0-9]+)\s+"((?:[^"\\]|\\.)*)"(?:\s+([0-9]+(?:\s+[0-9]+)*))?\s*$'
)
_MSVC_MARKER = re.compile(
    rb'^\s*#\s*line\s+([0-9]+)\s+"((?:[^"\\]|\\.)*)"\s*$'
)
_MARKER_PREFIX = re.compile(rb"^\s*#")
_SPOOF_MARKER = re.compile(
    rb'(?m)^\s*#\s*(?:line\s+)?[0-9]+\s+"((?:[^"\\]|\\.)*)"'
)
_BOOTSTRAP_PSEUDO_FILES = frozenset(
    {"<built-in>", "<command-line>", "<command line>"}
)
_DEPENDENCY_DOCUMENT_BYTES = 4 * 1024 * 1024
_DEPENDENCY_FILE_BYTES = 256 * 1024 * 1024


def _check_dependency_io_budget(
    deadline: float | None, cancel_event: object | None
) -> None:
    if cancel_event is not None:
        is_set = getattr(cancel_event, "is_set", None)
        if not callable(is_set):
            raise _fail("dependency cancellation event is invalid")
        if is_set():
            raise _fail("dependency I/O cancelled")
    if deadline is not None and time.monotonic() >= deadline:
        raise _fail("dependency I/O deadline exceeded")


def _read_bounded_dependency_file(
    path: Path,
    *,
    byte_ceiling: int,
    deadline: float | None,
    cancel_event: object | None,
) -> bytes:
    _check_dependency_io_budget(deadline, cancel_event)
    try:
        before = path.stat()
    except OSError as error:
        raise _fail(f"dependency file is unavailable: {path}") from error
    if not stat.S_ISREG(before.st_mode):
        raise _fail("dependency file is not regular")
    if before.st_size > byte_ceiling:
        raise _fail("dependency file byte ceiling exceeded")
    payload = bytearray()
    try:
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            while chunk := stream.read(64 * 1024):
                _check_dependency_io_budget(deadline, cancel_event)
                if len(payload) + len(chunk) > byte_ceiling:
                    raise _fail("dependency file byte ceiling exceeded")
                payload.extend(chunk)
            after_open = os.fstat(stream.fileno())
        after = path.stat()
    except OSError as error:
        raise _fail(f"dependency file changed while reading: {path}") from error
    snapshot = lambda item: (
        int(item.st_dev), int(item.st_ino) if int(item.st_ino) else None,
        int(item.st_size), int(item.st_mtime_ns),
        int(getattr(item, "st_ctime_ns", 0)),
    )
    expected = snapshot(before)
    if (
        snapshot(opened)[:4] != expected[:4]
        or snapshot(after_open)[:4] != expected[:4]
        or snapshot(after) != expected
        or len(payload) != before.st_size
    ):
        raise _fail("dependency file changed while reading")
    return bytes(payload)
_PUNCTUATORS = tuple(
    sorted(
        (
            b"%:%:",
            b">>=",
            b"<<=",
            b"->*",
            b"...",
            b"<=>",
            b"##",
            b"::",
            b".*",
            b"->",
            b"++",
            b"--",
            b"<<",
            b">>",
            b"<=",
            b">=",
            b"==",
            b"!=",
            b"&&",
            b"||",
            b"*=",
            b"/=",
            b"%=",
            b"+=",
            b"-=",
            b"&=",
            b"^=",
            b"|=",
            b"<:",
            b":>",
            b"<%",
            b"%>",
            b"%:",
        ),
        key=len,
        reverse=True,
    )
)
_BLOCK_SIZE = 4096
_ARRAY_SLACK = 4096
_TUPLE_SLOT_BYTES = sys.getsizeof((None,)) - sys.getsizeof(())


def _fail(message: str) -> AuditInfrastructureError:
    return AuditInfrastructureError(message)


def _is_windows_path(value: str) -> bool:
    path = PureWindowsPath(value)
    return path.is_absolute() or bool(path.drive)


def _display_normalized(path: Path | str) -> str:
    value = str(path)
    if _is_windows_path(value):
        return ntpath.normpath(value.replace("/", "\\"))
    return os.path.normpath(value)


def _path_key(path: Path | str) -> str:
    value = _display_normalized(path)
    if _is_windows_path(value):
        return value.casefold()
    return os.path.normcase(value)


def _path_alias(path: Path | str) -> str:
    return _display_normalized(path).replace("\\", "/")


def _decode_path_bytes(value: bytes) -> str:
    output = bytearray()
    index = 0
    while index < len(value):
        byte = value[index]
        if byte != 0x5C:
            output.append(byte)
            index += 1
            continue
        index += 1
        if index >= len(value):
            raise _fail("marker path has a truncated escape")
        escaped = value[index]
        simple = {
            ord("\\"): ord("\\"),
            ord('"'): ord('"'),
        }
        if escaped in simple:
            output.append(simple[escaped])
            index += 1
            continue
        if ord("0") <= escaped <= ord("7"):
            stop = index + 1
            while stop < min(index + 3, len(value)) and ord("0") <= value[stop] <= ord("7"):
                stop += 1
            output.append(int(value[index:stop], 8))
            index = stop
            continue
        # cl emits ordinary Windows separators in marker strings. Preserve an
        # unknown escape as a separator plus its following path character.
        output.extend((ord("\\"), escaped))
        index += 1
    if 0 in output:
        raise _fail("marker path contains an embedded NUL")
    encoding = "mbcs" if os.name == "nt" else sys.getfilesystemencoding()
    try:
        decoded = bytes(output).decode(encoding, errors="strict")
    except (LookupError, UnicodeDecodeError) as error:
        raise _fail("marker path cannot be decoded in the active code page") from error
    if any(0xD800 <= ord(character) <= 0xDFFF for character in decoded):
        raise _fail("marker path contains invalid bytes")
    return decoded


def _physical_line_count(
    path: Path,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> int:
    data = _read_bounded_dependency_file(
        path, byte_ceiling=_DEPENDENCY_FILE_BYTES,
        deadline=deadline, cancel_event=cancel_event,
    )
    return data.count(b"\n") + (1 if data and not data.endswith(b"\n") else 0)


def _deduplicate_paths(paths: list[Path]) -> tuple[Path, ...]:
    result: list[Path] = []
    aliases: dict[str, str] = {}
    for path in paths:
        if "\x00" in str(path):
            raise _fail("dependency path contains an embedded NUL")
        key = _path_key(path)
        alias = _path_alias(path)
        previous = aliases.get(key)
        if previous is None:
            aliases[key] = alias
            result.append(path)
        elif previous != alias:
            raise _fail("dependency path has an ambiguous canonical alias")
    return tuple(result)


def parse_gcc_dependencies(
    path: Path,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[Path, ...]:
    """Parse one GCC/Clang Make depfile without resolving relative paths."""

    data = _read_bounded_dependency_file(
        path, byte_ceiling=_DEPENDENCY_DOCUMENT_BYTES,
        deadline=deadline, cancel_event=cancel_event,
    )
    if b"\x00" in data:
        raise _fail("GCC dependency file contains an embedded NUL")
    data = data.replace(b"\\\r\n", b"").replace(b"\\\n", b"")
    colon = None
    escaped = False
    for index, byte in enumerate(data):
        if escaped:
            escaped = False
            continue
        if byte == 0x5C:
            escaped = True
            continue
        if byte == ord(":") and not (
            index == 1 and data[:1].isalpha()
        ):
            colon = index
            break
    if colon is None or not data[:colon].strip():
        raise _fail("GCC dependency file has no target rule")
    rule_end = data.find(b"\n", colon + 1)
    if rule_end >= 0:
        data = data[:rule_end]
    fields: list[bytes] = []
    current = bytearray()
    escaped = False
    for byte in data[colon + 1 :]:
        if escaped:
            current.append(byte)
            escaped = False
        elif byte == 0x5C:
            escaped = True
        elif byte in b" \t\r\n":
            if current:
                fields.append(bytes(current))
                current.clear()
        else:
            current.append(byte)
    if escaped:
        raise _fail("GCC dependency file has a truncated escape")
    if current:
        fields.append(bytes(current))
    if not fields:
        raise _fail("GCC dependency file contains no dependencies")
    decoded: list[Path] = []
    for field in fields:
        try:
            value = field.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            raise _fail("GCC dependency path is not valid UTF-8") from error
        decoded.append(Path(value.replace("/", os.sep)))
    return _deduplicate_paths(decoded)


def _json_no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise _fail(f"MSVC dependency JSON repeats key: {key}")
        result[key] = value
    return result


def parse_msvc_dependencies(
    path: Path,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[Path, ...]:
    """Parse an MSVC `/sourceDependencies` JSON document."""

    try:
        payload = _read_bounded_dependency_file(
            path, byte_ceiling=_DEPENDENCY_DOCUMENT_BYTES,
            deadline=deadline, cancel_event=cancel_event,
        )
        text = payload.decode("utf-8-sig", errors="strict")
        document = json.loads(text, object_pairs_hook=_json_no_duplicates)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise _fail(f"cannot parse MSVC dependency JSON: {path}") from error
    if not isinstance(document, dict) or not isinstance(document.get("Data"), dict):
        raise _fail("MSVC dependency JSON has no Data object")
    data = document["Data"]
    assert isinstance(data, dict)
    source = data.get("Source")
    includes = data.get("Includes")
    if not isinstance(source, str) or not isinstance(includes, list):
        raise _fail("MSVC dependency JSON has invalid Source or Includes")
    values = [source, *includes]
    if any(not isinstance(value, str) or not value for value in values):
        raise _fail("MSVC dependency JSON contains an invalid path")
    return _deduplicate_paths([Path(value) for value in values])


def validate_dependency_identities(
    paths: tuple[Path, ...],
    source_root: Path,
    production: Mapping[PurePosixPath, FileIdentity],
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[FileIdentity, ...]:
    """Canonicalize dependencies and bind production paths to locked identities."""

    if not source_root.is_absolute():
        raise _fail("source root must be absolute")
    production_by_key: dict[str, FileIdentity] = {}
    production_by_filesystem_id: dict[tuple[int, int], FileIdentity] = {}
    for relative, identity in production.items():
        if relative != identity.relative or not identity.production:
            raise _fail("production identity table is inconsistent")
        key = _path_key(identity.canonical)
        if key in production_by_key:
            raise _fail("production identity table has a canonical collision")
        production_by_key[key] = identity
        if identity.device is not None and identity.inode not in (None, 0):
            filesystem_id = (identity.device, identity.inode)
            previous = production_by_filesystem_id.get(filesystem_id)
            if previous is not None:
                raise _fail(
                    f"{relative}: filesystem identity aliases production path "
                    f"{previous.relative}"
                )
            production_by_filesystem_id[filesystem_id] = identity

    aliases: dict[str, str] = {}
    result: list[FileIdentity] = []
    seen: set[str] = set()
    for supplied in paths:
        _check_dependency_io_budget(deadline, cancel_event)
        if not supplied.is_absolute():
            raise _fail("dependency path must be absolute before identity validation")
        alias = _path_alias(supplied)
        key = _path_key(supplied)
        previous = aliases.get(key)
        if previous is not None and previous != alias:
            raise _fail("dependency path alias or case collision")
        aliases[key] = alias
        if key in seen:
            continue
        seen.add(key)
        try:
            canonical = supplied.resolve(strict=True)
            metadata = canonical.stat()
        except OSError as error:
            raise _fail(f"dependency is unavailable: {supplied}") from error
        if not stat.S_ISREG(metadata.st_mode):
            raise _fail(f"dependency is not a regular file: {supplied}")
        canonical_key = _path_key(canonical)
        if canonical_key != key:
            raise _fail("dependency resolves through a canonical alias")
        locked = production_by_key.get(canonical_key)
        if locked is not None:
            if _path_key(locked.canonical) != canonical_key:
                raise _fail("production dependency identity changed")
            locked_device = locked.device
            locked_inode = locked.inode
            if (
                locked_device is not None
                and int(metadata.st_dev) != locked_device
            ) or (
                locked_inode is not None
                and int(metadata.st_ino) != locked_inode
            ) or _physical_line_count(
                canonical, deadline=deadline, cancel_event=cancel_event
            ) != locked.line_count:
                raise _fail("production dependency changed after enumeration")
            result.append(locked)
            continue
        raw_device = getattr(metadata, "st_dev", None)
        raw_inode = getattr(metadata, "st_ino", None)
        device = int(raw_device) if isinstance(raw_device, int) else None
        inode = int(raw_inode) if isinstance(raw_inode, int) and raw_inode != 0 else None
        if (
            device is not None
            and inode is not None
            and (device, inode) in production_by_filesystem_id
        ):
            raise _fail("dependency filesystem identity aliases production path")
        result.append(
            FileIdentity(
                canonical=canonical,
                relative=None,
                device=device,
                inode=inode,
                line_count=_physical_line_count(
                    canonical, deadline=deadline, cancel_event=cancel_event
                ),
                production=False,
            )
        )
    return tuple(result)


class _Frame:
    __slots__ = ("path_key", "instance", "line")

    def __init__(self, path_key: str, instance: int, line: int) -> None:
        self.path_key = path_key
        self.instance = instance
        self.line = line


class PreprocessedStreamBuilder:
    """Consume compiler stdout incrementally and produce one packed TU view."""

    __slots__ = (
        "_configuration",
        "_production",
        "_limits",
        "_rss_reader",
        "_line_buffer",
        "_spellings",
        "_spelling_ids_by_value",
        "_marker_paths",
        "_marker_aliases",
        "_marker_relative",
        "_marker_ids_by_key",
        "_spelling_ids",
        "_identity_ids",
        "_inclusion_ids",
        "_original_lines",
        "_stack",
        "_current_line",
        "_next_instance",
        "_seen_real_code",
        "_seen_real_marker",
        "_in_block_comment",
        "_finalized",
        "_peak_rss_bytes",
    )

    def __init__(
        self,
        configuration: PreprocessConfiguration,
        production: Mapping[PurePosixPath, FileIdentity],
        limits: AuditLimits,
        rss_reader: Callable[[], int],
    ) -> None:
        if not isinstance(configuration, PreprocessConfiguration):
            raise _fail("preprocess configuration is invalid")
        if not isinstance(limits, AuditLimits) or not callable(rss_reader):
            raise _fail("provenance limits or RSS reader are invalid")
        self._configuration = configuration
        self._production = dict(production)
        self._limits = limits
        self._rss_reader = rss_reader
        self._line_buffer = bytearray()
        self._spellings: list[bytes] = []
        self._spelling_ids_by_value: dict[bytes, int] = {}
        self._marker_paths: list[Path] = []
        self._marker_aliases: list[str] = []
        self._marker_relative: list[bool] = []
        self._marker_ids_by_key: dict[str, int] = {}
        self._spelling_ids = array("I")
        self._identity_ids = array("I")
        self._inclusion_ids = array("I")
        self._original_lines = array("I")
        self._stack: list[_Frame] = []
        self._current_line = 0
        self._next_instance = 1
        self._seen_real_code = False
        self._seen_real_marker = False
        self._in_block_comment = False
        self._finalized = False
        self._peak_rss_bytes = 0
        self._sample_rss()

    @property
    def retained_bytes(self) -> int:
        return (
            len(self._spelling_ids) * 16
            + sum(len(value) for value in self._spellings)
            + len(self._spellings) * _TUPLE_SLOT_BYTES
            + len(self._line_buffer)
            + sum(len(value) for value in self._marker_aliases)
        )

    @property
    def peak_rss_bytes(self) -> int:
        return self._peak_rss_bytes

    def _sample_rss(self, reserve: int = 0) -> int:
        value = self._rss_reader()
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise _fail("coordinator RSS sample is invalid")
        self._peak_rss_bytes = max(self._peak_rss_bytes, value)
        if value > self._limits.rss_bytes or reserve > self._limits.rss_bytes - value:
            raise _fail("coordinator RSS limit exceeded")
        return value

    def _check_retained(self, addition: int = 0) -> None:
        if addition < 0 or self.retained_bytes > self._limits.retained_token_bytes - addition:
            raise _fail("retained packed token limit exceeded")

    def _resolve_marker_path(self, decoded: str) -> tuple[Path, bool]:
        if decoded.startswith("<") and decoded.endswith(">"):
            return Path(decoded), False
        if _is_windows_path(decoded) or Path(decoded).is_absolute():
            return Path(_display_normalized(decoded)), False
        bases: list[Path] = [self._configuration.working_directory]
        if self._stack:
            current = self._marker_paths[
                self._marker_ids_by_key[self._stack[-1].path_key]
            ]
            if not str(current).startswith("<"):
                bases.append(current.parent)
        candidates = [Path(_display_normalized(base / decoded)) for base in bases]
        matching = {
            _path_key(identity.canonical): identity
            for identity in self._production.values()
            if _path_key(identity.canonical) in {_path_key(candidate) for candidate in candidates}
        }
        if len(matching) > 1:
            raise _fail("relative marker path has an ambiguous production identity")
        if matching:
            return Path(next(iter(matching.values())).canonical), True
        existing = []
        for candidate in candidates:
            try:
                if candidate.resolve(strict=True).is_file():
                    existing.append(candidate)
            except OSError:
                pass
        if len({_path_key(candidate) for candidate in existing}) > 1:
            raise _fail("relative marker path has ambiguous working/current resolution")
        return (existing[0] if existing else candidates[0]), True

    def _marker_id(self, path: Path, relative: bool) -> int:
        key = _path_key(path)
        existing = self._marker_ids_by_key.get(key)
        alias = _path_alias(path)
        if existing is not None:
            if self._marker_relative[existing] != relative:
                raise _fail("absolute and relative marker paths collide")
            if self._marker_aliases[existing] != alias:
                # Separator and case aliases are tolerated only for Windows compiler output.
                if not _is_windows_path(alias):
                    raise _fail("marker path has an ambiguous canonical alias")
            return existing
        if len(self._marker_paths) > UINT32_MAX:
            raise _fail("marker identity table exceeds unsigned 32-bit IDs")
        marker_id = len(self._marker_paths)
        self._marker_ids_by_key[key] = marker_id
        self._marker_paths.append(path)
        self._marker_aliases.append(alias)
        self._marker_relative.append(relative)
        self._check_retained()
        return marker_id

    def _new_frame(self, key: str, line: int) -> _Frame:
        if self._next_instance > UINT32_MAX:
            raise _fail("inclusion instance exceeds unsigned 32-bit range")
        frame = _Frame(key, self._next_instance, line)
        self._next_instance += 1
        return frame

    def _apply_gcc_marker(
        self, line: int, path: Path, relative: bool, flags: tuple[int, ...]
    ) -> None:
        if line == 0:
            if self._seen_real_marker or self._seen_real_code:
                raise _fail("line zero is allowed only for bootstrap pseudo-files")
            if str(path).startswith("<"):
                if str(path) not in _BOOTSTRAP_PSEUDO_FILES:
                    raise _fail("unknown pseudo-file marker")
            else:
                self._marker_id(path, relative)
            return
        if str(path).startswith("<"):
            if str(path) not in _BOOTSTRAP_PSEUDO_FILES or self._seen_real_marker or self._seen_real_code:
                raise _fail("unknown or post-code pseudo-file marker")
            return
        if any(flag not in {1, 2, 3, 4} for flag in flags) or len(set(flags)) != len(flags):
            raise _fail("GCC marker has invalid flags")
        if 1 in flags and 2 in flags:
            raise _fail("GCC marker cannot enter and return simultaneously")
        key = _path_key(path)
        self._marker_id(path, relative)
        if not self._stack:
            if 2 in flags:
                raise _fail("GCC marker return has no ancestor")
            self._stack.append(self._new_frame(key, line))
        elif 1 in flags:
            self._stack.append(self._new_frame(key, line))
        elif 2 in flags:
            matches = [index for index, frame in enumerate(self._stack[:-1]) if frame.path_key == key]
            if len(matches) != 1:
                raise _fail("GCC marker return does not name exactly one ancestor")
            self._stack = self._stack[: matches[0] + 1]
            self._stack[-1].line = line
        elif self._stack[-1].path_key != key:
            raise _fail("GCC unflagged cross-file transition")
        else:
            self._stack[-1].line = line
        self._current_line = line
        self._seen_real_marker = True

    def _apply_msvc_marker(self, line: int, path: Path, relative: bool) -> None:
        if line == 0:
            raise _fail("MSVC marker line zero is invalid")
        if str(path).startswith("<"):
            raise _fail("MSVC pseudo-file marker is invalid")
        key = _path_key(path)
        self._marker_id(path, relative)
        if not self._stack:
            self._stack.append(self._new_frame(key, line))
        elif self._stack[-1].path_key == key:
            self._stack[-1].line = line
        else:
            ancestors = [
                index for index, frame in enumerate(self._stack[:-1]) if frame.path_key == key
            ]
            if len(ancestors) > 1:
                raise _fail("MSVC marker has ambiguous recursive ancestors")
            if ancestors:
                ancestor = self._stack[ancestors[0]]
                if line <= ancestor.line:
                    raise _fail("MSVC marker is ambiguous between return and recursive include")
                self._stack = self._stack[: ancestors[0] + 1]
                self._stack[-1].line = line
            else:
                self._stack.append(self._new_frame(key, line))
        self._current_line = line
        self._seen_real_marker = True

    def _parse_marker(self, content: bytes) -> bool:
        family = self._configuration.family
        expression = _GCC_MARKER if family in {CompilerFamily.GCC, CompilerFamily.CLANG} else _MSVC_MARKER
        match = expression.fullmatch(content)
        if match is None:
            if _MARKER_PREFIX.match(content):
                raise _fail("malformed or unsupported line marker")
            return False
        try:
            line = int(match.group(1))
        except (TypeError, ValueError) as error:
            raise _fail("line marker number is invalid") from error
        if line > UINT32_MAX:
            raise _fail("line marker exceeds unsigned 32-bit range")
        decoded = _decode_path_bytes(match.group(2))
        path, relative = self._resolve_marker_path(decoded)
        if family in {CompilerFamily.GCC, CompilerFamily.CLANG}:
            raw_flags = match.group(3)
            flags = tuple(int(value) for value in raw_flags.split()) if raw_flags else ()
            self._apply_gcc_marker(line, path, relative, flags)
        else:
            self._apply_msvc_marker(line, path, relative)
        return True

    @staticmethod
    def _identifier_start(byte: int) -> bool:
        return byte == ord("_") or ord("A") <= byte <= ord("Z") or ord("a") <= byte <= ord("z") or byte >= 0x80

    @staticmethod
    def _identifier_continue(byte: int) -> bool:
        return PreprocessedStreamBuilder._identifier_start(byte) or ord("0") <= byte <= ord("9")

    def _append_token(self, spelling: bytes) -> None:
        if not self._stack:
            raise _fail("compiler output contains code without a real-file marker")
        spelling_id = self._spelling_ids_by_value.get(spelling)
        intern_addition = 0
        if spelling_id is None:
            spelling_id = len(self._spellings)
            if spelling_id > UINT32_MAX:
                raise _fail("token spelling table exceeds unsigned 32-bit IDs")
            intern_addition = len(spelling) + _TUPLE_SLOT_BYTES
        addition = 16 + intern_addition
        self._check_retained(addition)
        if len(self._spelling_ids) % _BLOCK_SIZE == 0:
            self._sample_rss(reserve=_BLOCK_SIZE * 16 + _ARRAY_SLACK)
        if intern_addition:
            self._spelling_ids_by_value[spelling] = spelling_id
            self._spellings.append(spelling)
        marker_id = self._marker_ids_by_key[self._stack[-1].path_key]
        self._spelling_ids.append(spelling_id)
        self._identity_ids.append(marker_id)
        self._inclusion_ids.append(self._stack[-1].instance)
        self._original_lines.append(self._current_line)
        self._seen_real_code = True

    def _tokenize(self, content: bytes) -> None:
        index = 0
        while index < len(content):
            byte = content[index]
            if self._in_block_comment:
                close = content.find(b"*/", index)
                if close < 0:
                    return
                self._in_block_comment = False
                index = close + 2
                continue
            if byte in b" \t\r\f\v":
                index += 1
                continue
            if content.startswith(b"//", index):
                return
            if content.startswith(b"/*", index):
                self._in_block_comment = True
                index += 2
                continue
            start = index
            if self._identifier_start(byte):
                index += 1
                while index < len(content) and self._identifier_continue(content[index]):
                    index += 1
            elif ord("0") <= byte <= ord("9") or (
                byte == ord(".") and index + 1 < len(content) and content[index + 1 : index + 2].isdigit()
            ):
                index += 1
                while index < len(content):
                    current = content[index]
                    if self._identifier_continue(current) or current in b".'":
                        index += 1
                        continue
                    if current in b"+-" and index > start and content[index - 1] in b"eEpP":
                        index += 1
                        continue
                    break
            elif byte in (ord('"'), ord("'")):
                quote = byte
                index += 1
                escaped = False
                while index < len(content):
                    current = content[index]
                    index += 1
                    if escaped:
                        escaped = False
                    elif current == ord("\\"):
                        escaped = True
                    elif current == quote:
                        break
                else:
                    raise _fail("compiler output contains an unterminated literal")
            else:
                punctuator = next(
                    (value for value in _PUNCTUATORS if content.startswith(value, index)),
                    None,
                )
                index += len(punctuator) if punctuator is not None else 1
            self._append_token(content[start:index])

    def _process_line(self, content: bytes) -> None:
        if not self._in_block_comment and self._parse_marker(content):
            return
        self._tokenize(content)
        if self._stack:
            if self._current_line == UINT32_MAX:
                raise _fail("source line exceeds unsigned 32-bit range")
            self._current_line += 1
            self._stack[-1].line = self._current_line

    def feed(self, chunk: bytes) -> None:
        if self._finalized:
            raise _fail("preprocessed stream is already finalized")
        if not isinstance(chunk, bytes):
            raise _fail("preprocessed stream chunks must be bytes")
        if b"\x00" in chunk:
            raise _fail("preprocessed stream contains an embedded NUL")
        offset = 0
        while True:
            newline = chunk.find(b"\n", offset)
            if newline < 0:
                self._check_retained(len(chunk) - offset)
                self._line_buffer.extend(chunk[offset:])
                # Dense compiler output may contain a very long physical line. Flush
                # complete whitespace-delimited tokens without retaining that line.
                if (
                    not self._in_block_comment
                    and not _MARKER_PREFIX.match(self._line_buffer)
                    and not any(value in self._line_buffer for value in (b'"', b"'", b"/", b"\\"))
                ):
                    split = max(self._line_buffer.rfind(b" "), self._line_buffer.rfind(b"\t"))
                    if split >= 0:
                        prefix = bytes(self._line_buffer[: split + 1])
                        del self._line_buffer[: split + 1]
                        self._tokenize(prefix)
                self._check_retained()
                return
            self._check_retained(newline - offset)
            self._line_buffer.extend(chunk[offset:newline])
            self._process_line(bytes(self._line_buffer))
            self._line_buffer.clear()
            offset = newline + 1
            if offset >= len(chunk):
                return

    def finalize(
        self, dependencies: tuple[FileIdentity, ...]
    ) -> PreprocessedTranslationUnitView:
        if self._finalized:
            raise _fail("preprocessed stream is already finalized")
        self._finalized = True
        if self._line_buffer:
            if _MARKER_PREFIX.match(self._line_buffer):
                raise _fail("truncated line marker at end of compiler output")
            self._process_line(bytes(self._line_buffer))
            self._line_buffer.clear()
        if self._in_block_comment:
            raise _fail("compiler output contains an unterminated comment")
        if not self._seen_real_marker:
            raise _fail("compiler output contains no real-file marker")
        if self._configuration.family in {CompilerFamily.GCC, CompilerFamily.CLANG} and len(self._stack) != 1:
            raise _fail("truncated GCC include stack")

        dependency_by_key: dict[str, FileIdentity] = {}
        for identity in dependencies:
            if not isinstance(identity, FileIdentity):
                raise _fail("dependency identity is invalid")
            key = _path_key(identity.canonical)
            if key in dependency_by_key:
                raise _fail("dependency identities have a canonical collision")
            dependency_by_key[key] = identity
        identities: list[FileIdentity] = []
        for marker_path in self._marker_paths:
            identity = dependency_by_key.get(_path_key(marker_path))
            if identity is None:
                raise _fail(f"line marker does not name a dependency: {marker_path}")
            production_identity = next(
                (
                    candidate
                    for candidate in self._production.values()
                    if _path_key(candidate.canonical) == _path_key(marker_path)
                ),
                None,
            )
            if production_identity is not None and identity != production_identity:
                raise _fail("production marker does not match its validated dependency identity")
            identities.append(identity)

        for index, marker_id in enumerate(self._identity_ids):
            identity = identities[marker_id]
            line = self._original_lines[index]
            if line == 0 or line > identity.line_count:
                raise _fail("token source line is outside dependency line range")

        # CompactTokenSequence owns fresh packed arrays. Reserve for all four
        # transient copies plus the immutable spelling/identity tuples first.
        transient = len(self._spelling_ids) * 16 + (
            len(self._spellings) + len(identities)
        ) * _TUPLE_SLOT_BYTES
        self._sample_rss(reserve=transient + _ARRAY_SLACK)
        view = PreprocessedTranslationUnitView(
            configuration=self._configuration,
            tokens=CompactTokenSequence._from_packed(
                self._configuration,
                spellings=tuple(self._spellings),
                identities=tuple(identities),
                spelling_ids=self._spelling_ids,
                identity_ids=self._identity_ids,
                inclusion_ids=self._inclusion_ids,
                original_lines=self._original_lines,
                limits=self._limits,
                rss_reader=self._sample_rss,
            ),
            dependencies=dependencies,
        )
        self._spelling_ids = array("I")
        self._identity_ids = array("I")
        self._inclusion_ids = array("I")
        self._original_lines = array("I")
        self._spellings.clear()
        self._spelling_ids_by_value.clear()
        return view


def reject_source_line_spoofs(
    path: Path, production: Mapping[PurePosixPath, FileIdentity]
) -> None:
    """Reject physical or spliced source directives that can forge provenance."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise _fail(f"cannot read source for line-spoof scan: {path}") from error
    if b"\x00" in raw:
        raise _fail("source contains an embedded NUL")
    logical = raw.replace(b"\\\r\n", b"").replace(b"\\\n", b"")
    source_key = _path_key(path.resolve())
    production_keys = {_path_key(identity.canonical) for identity in production.values()}
    source_is_production = source_key in production_keys
    for match in _SPOOF_MARKER.finditer(logical):
        target = _decode_path_bytes(match.group(1))
        if source_is_production:
            raise _fail("production source contains a line-marker spoof")
        candidates = [Path(target)] if _is_windows_path(target) or Path(target).is_absolute() else [path.parent / target]
        if any(_path_key(candidate) in production_keys for candidate in candidates):
            raise _fail("source line-marker spoof targets a production path")
