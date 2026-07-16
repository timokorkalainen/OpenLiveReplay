"""Shared immutable model for the compiler-authoritative GPU capability audit."""

from __future__ import annotations

import enum
import os
import stat
from array import array
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import overload


UINT32_MAX = (1 << 32) - 1
_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm"})
_TRANSLATION_UNIT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".mm"})
_PRODUCTION_ROOTS = ("playback", "recorder_engine")
_REPARSE_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class AuditInfrastructureError(RuntimeError):
    pass


class CompilerFamily(enum.Enum):
    GCC = "gcc"
    CLANG = "clang"
    MSVC = "msvc"
    CLANG_CL = "clang-cl"


@dataclass(frozen=True)
class AuditLimits:
    response_depth: int = 8
    response_files: int = 32
    response_bytes: int = 4 * 1024 * 1024
    invocation_seconds: float = 60.0
    total_seconds: float = 240.0
    stdout_bytes: int = 128 * 1024 * 1024
    stderr_bytes: int = 1024 * 1024
    retained_token_bytes: int = 384 * 1024 * 1024
    rss_bytes: int = 512 * 1024 * 1024
    workers: int = min(8, os.cpu_count() or 1)


@dataclass(frozen=True)
class FileIdentity:
    canonical: Path
    relative: PurePosixPath | None
    device: int | None
    inode: int | None
    line_count: int
    production: bool


@dataclass(frozen=True)
class PreprocessConfiguration:
    entry_id: str
    family: CompilerFamily
    compiler: Path
    working_directory: Path
    source: FileIdentity
    arguments: tuple[str, ...]
    environment_digest: str
    digest: str


@dataclass(frozen=True, slots=True)
class SourceLocation:
    identity: FileIdentity | None
    inclusion_instance: int
    line: int
    configuration_digest: str


@dataclass(frozen=True, slots=True)
class PreprocessedToken:
    spelling: bytes
    location: SourceLocation


@dataclass(frozen=True, slots=True)
class PackedTokenRun:
    start: int
    stop: int
    identity_id: int
    inclusion_instance: int
    original_line: int


@dataclass(frozen=True)
class PreprocessedTranslationUnitView:
    configuration: PreprocessConfiguration
    tokens: "CompactTokenSequence"
    dependencies: tuple[FileIdentity, ...]


@dataclass(frozen=True)
class CoverageReport:
    authoritative: frozenset[PurePosixPath]
    source_only: frozenset[PurePosixPath]
    configurations: tuple[str, ...]


def _validate_uint32(value: int, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0 or value > UINT32_MAX:
        raise AuditInfrastructureError(f"{label} must be an unsigned 32-bit integer")
    return value


class CompactTokenSequence(Sequence[PreprocessedToken]):
    """Packed token columns with immutable token views materialized on demand."""

    __slots__ = (
        "_configuration",
        "_spellings",
        "_identities",
        "_spelling_ids",
        "_identity_ids",
        "_inclusion_ids",
        "_original_lines",
        "_frozen",
    )

    def __setattr__(self, name: str, value: object) -> None:
        if getattr(self, "_frozen", False):
            raise AttributeError("CompactTokenSequence is immutable")
        object.__setattr__(self, name, value)

    def __init__(
        self,
        configuration: PreprocessConfiguration,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        spelling_ids: array,
        identity_ids: array,
        inclusion_ids: array,
        original_lines: array,
    ) -> None:
        supplied_columns = (spelling_ids, identity_ids, inclusion_ids, original_lines)
        if any(
            not isinstance(column, array) or column.typecode != "I"
            for column in supplied_columns
        ):
            raise AuditInfrastructureError("packed token columns must use array('I')")
        if array("I").itemsize != 4 or any(
            column.itemsize != 4 for column in supplied_columns
        ):
            raise AuditInfrastructureError("packed token columns require four-byte array('I') items")
        columns = tuple(
            memoryview(column.tobytes()).cast("I") for column in supplied_columns
        )
        lengths = {len(column) for column in columns}
        if len(lengths) != 1:
            raise AuditInfrastructureError("packed token columns have different lengths")
        if any(not isinstance(spelling, bytes) for spelling in spellings):
            raise AuditInfrastructureError("token spellings must be bytes")
        if len(set(spellings)) != len(spellings):
            raise AuditInfrastructureError("token spelling table is not interned")
        if len(set(identities)) != len(identities):
            raise AuditInfrastructureError("token identity table is not interned")
        if len(spellings) > UINT32_MAX + 1 or len(identities) > UINT32_MAX + 1:
            raise AuditInfrastructureError("token intern table exceeds unsigned 32-bit IDs")
        if any(spelling_id >= len(spellings) for spelling_id in spelling_ids):
            raise AuditInfrastructureError("packed spelling ID does not exist in the spelling table")
        if any(identity_id >= len(identities) for identity_id in identity_ids):
            raise AuditInfrastructureError("packed identity ID does not exist in the identity table")

        self._configuration = configuration
        self._spellings = spellings
        self._identities = identities
        self._spelling_ids = columns[0]
        self._identity_ids = columns[1]
        self._inclusion_ids = columns[2]
        self._original_lines = columns[3]
        self._frozen = True

    @classmethod
    def empty(cls, configuration: PreprocessConfiguration) -> "CompactTokenSequence":
        return cls(
            configuration,
            (),
            (),
            array("I"),
            array("I"),
            array("I"),
            array("I"),
        )

    @classmethod
    def _from_packed(
        cls,
        configuration: PreprocessConfiguration,
        *,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        spelling_ids: array,
        identity_ids: array,
        inclusion_ids: array,
        original_lines: array,
    ) -> "CompactTokenSequence":
        return cls(
            configuration,
            spellings,
            identities,
            spelling_ids,
            identity_ids,
            inclusion_ids,
            original_lines,
        )

    @classmethod
    def _from_token_fields(
        cls,
        configuration: PreprocessConfiguration,
        *,
        spellings: tuple[bytes, ...],
        identities: tuple[FileIdentity | None, ...],
        fields: Iterable[tuple[int, int, int, int]],
    ) -> "CompactTokenSequence":
        spelling_ids = array("I")
        identity_ids = array("I")
        inclusion_ids = array("I")
        original_lines = array("I")
        for spelling_id, identity_id, inclusion_id, original_line in fields:
            values = (
                _validate_uint32(spelling_id, "spelling ID"),
                _validate_uint32(identity_id, "identity ID"),
                _validate_uint32(inclusion_id, "inclusion instance"),
                _validate_uint32(original_line, "original line"),
            )
            if values[0] >= len(spellings):
                raise AuditInfrastructureError("packed spelling ID does not exist in the spelling table")
            if values[1] >= len(identities):
                raise AuditInfrastructureError("packed identity ID does not exist in the identity table")
            spelling_ids.append(values[0])
            identity_ids.append(values[1])
            inclusion_ids.append(values[2])
            original_lines.append(values[3])
        return cls._from_packed(
            configuration,
            spellings=spellings,
            identities=identities,
            spelling_ids=spelling_ids,
            identity_ids=identity_ids,
            inclusion_ids=inclusion_ids,
            original_lines=original_lines,
        )

    def __len__(self) -> int:
        return len(self._spelling_ids)

    def _normalize_index(self, index: int) -> int:
        if index < 0:
            index += len(self)
        if index < 0 or index >= len(self):
            raise IndexError("compact token index out of range")
        return index

    def _token_at(self, index: int) -> PreprocessedToken:
        index = self._normalize_index(index)
        location = SourceLocation(
            identity=self._identities[self._identity_ids[index]],
            inclusion_instance=self._inclusion_ids[index],
            line=self._original_lines[index],
            configuration_digest=self._configuration.digest,
        )
        return PreprocessedToken(self._spellings[self._spelling_ids[index]], location)

    @overload
    def __getitem__(self, index: int) -> PreprocessedToken:
        ...

    @overload
    def __getitem__(self, index: slice) -> list[PreprocessedToken]:
        ...

    def __getitem__(self, index: int | slice) -> PreprocessedToken | list[PreprocessedToken]:
        if isinstance(index, slice):
            return [self._token_at(position) for position in range(*index.indices(len(self)))]
        if not isinstance(index, int) or isinstance(index, bool):
            raise TypeError("compact token indices must be integers or slices")
        return self._token_at(index)

    def __iter__(self) -> Iterator[PreprocessedToken]:
        for index in range(len(self)):
            yield self._token_at(index)

    @property
    def packed_bytes(self) -> int:
        return sum(len(column) * column.itemsize for column in self._packed_columns())

    def spelling_id_at(self, index: int) -> int:
        return self._spelling_ids[self._normalize_index(index)]

    def spelling_for(self, spelling_id: int) -> bytes:
        if spelling_id < 0 or spelling_id >= len(self._spellings):
            raise IndexError("spelling ID out of range")
        return self._spellings[spelling_id]

    def identity_for(self, identity_id: int) -> FileIdentity | None:
        if identity_id < 0 or identity_id >= len(self._identities):
            raise IndexError("identity ID out of range")
        return self._identities[identity_id]

    def iter_runs(self) -> Iterator[PackedTokenRun]:
        size = len(self)
        start = 0
        while start < size:
            identity_id = self._identity_ids[start]
            inclusion_instance = self._inclusion_ids[start]
            original_line = self._original_lines[start]
            stop = start + 1
            while (
                stop < size
                and self._identity_ids[stop] == identity_id
                and self._inclusion_ids[stop] == inclusion_instance
                and self._original_lines[stop] == original_line
            ):
                stop += 1
            yield PackedTokenRun(
                start,
                stop,
                identity_id,
                inclusion_instance,
                original_line,
            )
            start = stop

    def _packed_columns(
        self,
    ) -> tuple[memoryview, memoryview, memoryview, memoryview]:
        return (
            self._spelling_ids,
            self._identity_ids,
            self._inclusion_ids,
            self._original_lines,
        )


def requires_compile_entry(
    path: PurePosixPath,
    configured_families: frozenset[CompilerFamily],
    has_objcpp: bool,
    has_windows_backend: bool,
) -> bool:
    """Return whether this production translation unit is active in the supplied build."""

    suffix = path.suffix.casefold()
    if suffix not in _TRANSLATION_UNIT_SUFFIXES or not configured_families:
        return False

    parts = tuple(part.casefold() for part in path.parts)
    stem = path.stem.casefold()
    apple_specific = suffix == ".mm" or stem.endswith("_apple")
    windows_specific = "win" in parts[:-1] or stem.endswith(("_win", "_mediafoundation"))
    if apple_specific and not has_objcpp:
        return False
    if windows_specific and not has_windows_backend:
        return False
    return True


def _display_relative(path: Path, root: Path) -> PurePosixPath:
    try:
        return PurePosixPath(path.relative_to(root).as_posix())
    except ValueError:
        return PurePosixPath(path.as_posix())


def _is_reparse(stat_result: os.stat_result) -> bool:
    attributes = getattr(stat_result, "st_file_attributes", 0)
    return bool(attributes & _REPARSE_ATTRIBUTE)


def _check_casefold_collision(
    relative: PurePosixPath,
    seen: dict[str, PurePosixPath],
) -> None:
    folded = relative.as_posix().casefold()
    previous = seen.get(folded)
    if previous is not None and previous != relative:
        raise AuditInfrastructureError(
            f"{relative}: case-fold collision with production path {previous}"
        )
    seen[folded] = relative


def _walk_production_entries(directory: Path, root: Path) -> Iterator[tuple[Path, os.stat_result]]:
    try:
        entries = sorted(os.scandir(directory), key=lambda entry: entry.name.casefold())
    except OSError as error:
        relative = _display_relative(directory, root)
        raise AuditInfrastructureError(f"{relative}: cannot enumerate production path: {error}") from error

    for entry in entries:
        path = Path(entry.path)
        relative = _display_relative(path, root)
        try:
            # DirEntry.stat() on Windows may omit the volume/file index fields;
            # os.stat() supplies the stable identity needed to detect hard links.
            metadata = os.stat(path, follow_symlinks=False)
        except OSError as error:
            raise AuditInfrastructureError(f"{relative}: cannot inspect production path: {error}") from error
        if entry.is_symlink():
            raise AuditInfrastructureError(f"{relative}: production path crosses a symlink")
        if _is_reparse(metadata):
            raise AuditInfrastructureError(f"{relative}: production path crosses a reparse point")
        if stat.S_ISDIR(metadata.st_mode):
            yield from _walk_production_entries(path, root)
        else:
            yield path, metadata


def _physical_line_count(source: str) -> int:
    if not source:
        return 0
    normalized = source.replace("\r\n", "\n").replace("\r", "\n")
    return normalized.count("\n") + (0 if normalized.endswith("\n") else 1)


def enumerate_production_identities(root: Path) -> dict[PurePosixPath, FileIdentity]:
    """Enumerate unambiguous canonical identities under the two production roots."""

    lexical_root = Path(root).absolute()
    try:
        root_metadata = lexical_root.lstat()
    except OSError as error:
        raise AuditInfrastructureError(
            f"{lexical_root}: source root cannot be inspected: {error}"
        ) from error
    if lexical_root.is_symlink():
        raise AuditInfrastructureError(
            f"{lexical_root}: source root crosses a symlink"
        )
    if _is_reparse(root_metadata):
        raise AuditInfrastructureError(
            f"{lexical_root}: source root crosses a reparse point"
        )
    if not stat.S_ISDIR(root_metadata.st_mode):
        raise AuditInfrastructureError(f"{lexical_root}: source root is not a directory")
    try:
        canonical_root = lexical_root.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(f"{lexical_root}: source root cannot be resolved: {error}") from error
    identities: dict[PurePosixPath, FileIdentity] = {}
    casefolded: dict[str, PurePosixPath] = {}
    filesystem_ids: dict[tuple[int, int], PurePosixPath] = {}

    for production_root_name in _PRODUCTION_ROOTS:
        production_root = lexical_root / production_root_name
        try:
            root_metadata = production_root.lstat()
        except FileNotFoundError:
            continue
        except OSError as error:
            raise AuditInfrastructureError(
                f"{production_root_name}: cannot inspect production root: {error}"
            ) from error
        if production_root.is_symlink():
            raise AuditInfrastructureError(
                f"{production_root_name}: production path crosses a symlink"
            )
        if _is_reparse(root_metadata):
            raise AuditInfrastructureError(
                f"{production_root_name}: production path crosses a reparse point"
            )
        if not stat.S_ISDIR(root_metadata.st_mode):
            raise AuditInfrastructureError(f"{production_root_name}: production root is not a directory")

        for candidate, metadata in _walk_production_entries(production_root, lexical_root):
            relative = _display_relative(candidate, lexical_root)
            if candidate.suffix.casefold() not in _SOURCE_SUFFIXES:
                continue
            if not stat.S_ISREG(metadata.st_mode):
                raise AuditInfrastructureError(f"{relative}: production path is not a regular file")
            try:
                canonical = candidate.resolve(strict=True)
            except OSError as error:
                raise AuditInfrastructureError(f"{relative}: production path cannot be resolved: {error}") from error
            try:
                canonical.relative_to(canonical_root)
            except ValueError as error:
                raise AuditInfrastructureError(
                    f"{relative}: resolved production path is outside the source root"
                ) from error

            _check_casefold_collision(relative, casefolded)
            raw_device = getattr(metadata, "st_dev", None)
            raw_inode = getattr(metadata, "st_ino", None)
            device = int(raw_device) if isinstance(raw_device, int) else None
            inode = int(raw_inode) if isinstance(raw_inode, int) and raw_inode != 0 else None
            if device is not None and inode is not None:
                filesystem_id = (device, inode)
                previous = filesystem_ids.get(filesystem_id)
                if previous is not None:
                    raise AuditInfrastructureError(
                        f"{relative}: filesystem identity aliases production path {previous}"
                    )
                filesystem_ids[filesystem_id] = relative

            try:
                source = candidate.read_bytes().decode("utf-8", errors="strict")
            except UnicodeDecodeError as error:
                raise AuditInfrastructureError(f"{relative}: production file is not valid UTF-8") from error
            except OSError as error:
                raise AuditInfrastructureError(f"{relative}: production file is unreadable: {error}") from error

            identities[relative] = FileIdentity(
                canonical=canonical,
                relative=relative,
                device=device,
                inode=inode,
                line_count=_physical_line_count(source),
                production=True,
            )

    return dict(sorted(identities.items(), key=lambda item: item[0].as_posix()))
