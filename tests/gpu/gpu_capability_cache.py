"""Dependency-validated atomic cache for compiler-authoritative GPU views."""

from __future__ import annotations

import hashlib
import base64
import json
import os
import stat
import struct
import sys
import threading
import time
import uuid
from array import array
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CompactTokenSequence,
    CompilerFamily,
    CompilerInspection,
    DependencyRootAuthority,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    _current_process_rss_bytes,
    _preprocessed_view_semantic_digest,
    decode_compiler_inspection,
    encode_compiler_inspection,
    validate_dependency_root_authority,
    _HeldCompilerCapabilityMismatch,
)


_SCHEMA = 1
_PAYLOAD_MAGIC = b"OLRGPC01"
_INCOMPLETE_SECONDS = 60 * 60
_COMPLETE_SECONDS = 14 * 24 * 60 * 60
_IO_BLOCK_BYTES = 64 * 1024
_MAX_MANIFEST_BYTES = 4 * 1024 * 1024
_MAX_RECORD_BYTES = 16 * 1024 * 1024
_DEFAULT_CACHE_OPERATION_SECONDS = 240.0
_REPARSE_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
_hash_cache: dict[tuple[object, ...], str] = {}
_hash_lock = threading.Lock()


class _UnsafeCacheNamespaceError(OSError):
    """A cache pathname is linked, replaced, or otherwise not uniquely owned."""


def _check_cache_rss(reserve: int = 0) -> None:
    limit = AuditLimits().rss_bytes
    rss = _current_process_rss_bytes()
    if (
        not isinstance(rss, int)
        or isinstance(rss, bool)
        or rss < 0
        or rss > limit
        or reserve > limit - rss
    ):
        raise AuditInfrastructureError("coordinator RSS limit exceeded while loading cache")


class _PublicationGuard:
    """Cross-process advisory lock that keeps an active temp entry alive."""

    def __init__(
        self,
        path: Path,
        deadline: float,
        cancel_event: object | None = None,
    ) -> None:
        self.path = path
        self.deadline = deadline
        self.cancel_event = cancel_event
        self.stream = None

    def _check_budget(self) -> None:
        if self.cancel_event is not None:
            is_set = getattr(self.cancel_event, "is_set", None)
            if not callable(is_set):
                raise AuditInfrastructureError(
                    "cache publication cancellation event is invalid"
                )
            cancelled = is_set()
            if not isinstance(cancelled, bool):
                raise AuditInfrastructureError(
                    "cache publication cancellation event is invalid"
                )
            if cancelled:
                raise AuditInfrastructureError("cache publication cancelled")
        if time.monotonic() >= self.deadline:
            raise AuditInfrastructureError("cache publication deadline exceeded")

    @staticmethod
    def _lock(stream, *, blocking: bool) -> bool:
        stream.seek(0)
        if os.name == "nt":
            import msvcrt

            mode = msvcrt.LK_LOCK if blocking else msvcrt.LK_NBLCK
            try:
                msvcrt.locking(stream.fileno(), mode, 1)
                return True
            except OSError:
                return False
        import fcntl

        operation = fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB)
        try:
            fcntl.flock(stream.fileno(), operation)
            return True
        except BlockingIOError:
            return False

    @staticmethod
    def _unlock(stream) -> None:
        stream.seek(0)
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            return
        import fcntl

        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)

    def __enter__(self) -> "_PublicationGuard":
        self.stream = self.path.open("x+b")
        self.stream.write(b"1")
        self.stream.flush()
        try:
            while True:
                acquired = self._lock(self.stream, blocking=False)
                try:
                    self._check_budget()
                except BaseException:
                    if acquired:
                        self._unlock(self.stream)
                    raise
                if acquired:
                    return self
                remaining = max(0.0, self.deadline - time.monotonic())
                time.sleep(min(0.01, remaining))
        except BaseException as error:
            self.stream.close()
            self.stream = None
            try:
                self.path.unlink()
            except OSError as cleanup_error:
                raise AuditInfrastructureError(
                    "cannot remove cache publication guard after "
                    f"{type(error).__name__}: {error}"
                ) from cleanup_error
            raise

    def __exit__(self, _type, _value, _traceback) -> None:
        assert self.stream is not None
        try:
            self._unlock(self.stream)
        finally:
            self.stream.close()
            self.stream = None


class _SharedCacheFileLock:
    """Deadline-bounded cross-process lock over one ordinary carrier file."""

    def __init__(
        self,
        path: Path,
        root: Path,
        root_identity: tuple[int, int | None],
        deadline: float,
        cancel_event: object | None = None,
    ) -> None:
        self.path = path
        self.root = root
        self.root_identity = root_identity
        self.deadline = deadline
        self.cancel_event = cancel_event
        self.stream = None

    def _check_budget(self) -> None:
        if self.cancel_event is not None:
            is_set = getattr(self.cancel_event, "is_set", None)
            if not callable(is_set):
                raise AuditInfrastructureError("cache lock cancellation event is invalid")
            cancelled = is_set()
            if not isinstance(cancelled, bool):
                raise AuditInfrastructureError("cache lock cancellation event is invalid")
            if cancelled:
                raise AuditInfrastructureError("cache lock cancelled")
        if time.monotonic() >= self.deadline:
            raise AuditInfrastructureError("cache lock deadline exceeded")

    def _assert_root(self) -> None:
        metadata = _ordinary_directory(self.root)
        if _directory_identity(metadata) != self.root_identity:
            raise AuditInfrastructureError("cache root was replaced")

    def __enter__(self) -> "_SharedCacheFileLock":
        if (
            not isinstance(self.deadline, (int, float))
            or isinstance(self.deadline, bool)
        ):
            raise AuditInfrastructureError("cache lock deadline is invalid")
        self._check_budget()
        try:
            self._assert_root()
            try:
                stream = self.path.open("x+b")
                stream.write(b"1")
                stream.flush()
                os.fsync(stream.fileno())
            except FileExistsError:
                stream = self.path.open("r+b")
            self.stream = stream
            carrier = _regular_unlinked_file(self.path)
            opened = os.fstat(stream.fileno())
            if (
                int(opened.st_dev) != int(carrier.st_dev)
                or int(opened.st_ino) != int(carrier.st_ino)
                or int(getattr(opened, "st_nlink", 1)) != 1
                or int(opened.st_size) != 1
            ):
                raise OSError("cache lock carrier changed while opening")
            while not _PublicationGuard._lock(stream, blocking=False):
                self._check_budget()
                time.sleep(min(0.01, max(0.0, self.deadline - time.monotonic())))
            self._check_budget()
            self._assert_root()
            return self
        except BaseException as error:
            if self.stream is not None:
                self.stream.close()
                self.stream = None
            if isinstance(error, AuditInfrastructureError):
                raise
            raise AuditInfrastructureError(
                "cache lock namespace is unsafe"
            ) from error

    def __exit__(self, _type, _value, _traceback) -> None:
        assert self.stream is not None
        try:
            _PublicationGuard._unlock(self.stream)
        finally:
            self.stream.close()
            self.stream = None


def _temporary_publication_is_active(path: Path) -> bool:
    lock_path = path / "active.lock"
    try:
        _regular_unlinked_file(lock_path)
        with lock_path.open("r+b") as stream:
            if not _PublicationGuard._lock(stream, blocking=False):
                return True
            _PublicationGuard._unlock(stream)
    except OSError:
        return False
    return False


def _is_link(metadata: os.stat_result) -> bool:
    return stat.S_ISLNK(metadata.st_mode) or bool(
        getattr(metadata, "st_file_attributes", 0) & _REPARSE_ATTRIBUTE
    )


def _path_key(path: Path) -> str:
    return os.path.normcase(os.path.normpath(str(path)))


def _regular_unlinked_file(path: Path) -> os.stat_result:
    metadata = path.lstat()
    if _is_link(metadata) or not stat.S_ISREG(metadata.st_mode):
        raise _UnsafeCacheNamespaceError(
            f"cache path is not an ordinary file: {path}"
        )
    if int(getattr(metadata, "st_nlink", 1)) != 1:
        raise _UnsafeCacheNamespaceError(
            f"cache path has filesystem aliases: {path}"
        )
    return metadata


def _ordinary_directory(path: Path) -> os.stat_result:
    metadata = path.lstat()
    if _is_link(metadata) or not stat.S_ISDIR(metadata.st_mode):
        raise _UnsafeCacheNamespaceError(
            f"cache path is not an ordinary directory: {path}"
        )
    return metadata


def _directory_identity(metadata: os.stat_result) -> tuple[int, int | None]:
    inode = int(metadata.st_ino) if int(metadata.st_ino) != 0 else None
    return int(metadata.st_dev), inode


def _open_dependency(path: Path):
    return path.open("rb")


def _dependency_digest(identity: FileIdentity, *, force: bool = False) -> str:
    path = identity.canonical
    if not path.is_absolute():
        raise OSError("dependency path is not absolute")
    metadata = path.lstat()
    if _is_link(metadata) or not stat.S_ISREG(metadata.st_mode):
        raise _UnsafeCacheNamespaceError("dependency is not an ordinary file")
    if int(getattr(metadata, "st_nlink", 1)) != 1:
        raise _UnsafeCacheNamespaceError("dependency has filesystem aliases")
    canonical = path.resolve(strict=True)
    if _path_key(canonical) != _path_key(path):
        raise _UnsafeCacheNamespaceError(
            "dependency resolves through a canonical alias"
        )
    device = int(metadata.st_dev)
    inode = int(metadata.st_ino) if int(metadata.st_ino) != 0 else None
    if identity.device is not None and device != identity.device:
        raise OSError("dependency device changed")
    if identity.inode is not None and inode != identity.inode:
        raise OSError("dependency inode changed")
    digest = hashlib.sha256()
    with _open_dependency(path) as stream:
        opened = os.fstat(stream.fileno())
        if (
            int(opened.st_dev) != device
            or (inode is not None and int(opened.st_ino) != inode)
            or int(opened.st_size) != int(metadata.st_size)
            or int(getattr(opened, "st_nlink", 1)) != 1
        ):
            raise OSError("dependency changed while hashing")
        signature = (
            _path_key(canonical),
            int(opened.st_dev),
            int(opened.st_ino) if int(opened.st_ino) != 0 else None,
            int(opened.st_size),
            int(opened.st_mtime_ns),
            int(getattr(opened, "st_ctime_ns", 0)),
        )
        if not force:
            with _hash_lock:
                cached = _hash_cache.get(signature)
            if cached is not None:
                return cached
        while True:
            block = stream.read(_IO_BLOCK_BYTES)
            if not block:
                break
            digest.update(block)
        final = os.fstat(stream.fileno())
        if (
            int(final.st_size) != int(opened.st_size)
            or int(final.st_mtime_ns) != int(opened.st_mtime_ns)
            or int(getattr(final, "st_ctime_ns", 0))
            != int(getattr(opened, "st_ctime_ns", 0))
        ):
            raise OSError("dependency changed while hashing")
    result = digest.hexdigest()
    with _hash_lock:
        if force:
            _hash_cache[signature] = result
            return result
        return _hash_cache.setdefault(signature, result)


@dataclass(frozen=True)
class _DependencySnapshot:
    identity: FileIdentity
    sha256: str


def _identity_document(identity: FileIdentity | None) -> dict[str, object] | None:
    if identity is None:
        return None
    return {
        "canonical": str(identity.canonical),
        "relative": str(identity.relative) if identity.relative is not None else None,
        "device": identity.device,
        "inode": identity.inode,
        "line_count": identity.line_count,
        "production": identity.production,
    }


def _identity_from_document(
    document: object,
    dependencies: dict[str, FileIdentity],
) -> FileIdentity | None:
    if document is None:
        return None
    if not isinstance(document, dict):
        raise ValueError("identity record is malformed")
    canonical_value = document.get("canonical")
    relative_value = document.get("relative")
    if not isinstance(canonical_value, str) or (
        relative_value is not None and not isinstance(relative_value, str)
    ):
        raise ValueError("identity path is malformed")
    identity = dependencies.get(_path_key(Path(canonical_value)))
    if identity is None or _identity_document(identity) != document:
        raise ValueError("token identity is not a validated dependency")
    return identity


class _DigestWriter:
    def __init__(self, stream) -> None:
        self.stream = stream
        self.digest = hashlib.sha256()
        self.count = 0

    def write(self, data) -> None:
        self.stream.write(data)
        self.digest.update(data)
        self.count += len(data)


def _write_u64(writer: _DigestWriter, value: int) -> None:
    writer.write(struct.pack("<Q", value))


def _write_record(writer: _DigestWriter, value: bytes) -> None:
    _write_u64(writer, len(value))
    writer.write(value)


def _write_payload(path: Path, view: PreprocessedTranslationUnitView) -> tuple[str, int]:
    tokens = view.tokens
    spellings = object.__getattribute__(tokens, "_spellings")
    identities = object.__getattribute__(tokens, "_identities")
    columns = tuple(
        object.__getattribute__(tokens, slot)
        for slot in (
            "_spelling_ids",
            "_identity_ids",
            "_inclusion_ids",
            "_original_lines",
        )
    )
    with path.open("xb") as stream:
        writer = _DigestWriter(stream)
        writer.write(_PAYLOAD_MAGIC)
        _write_u64(writer, len(spellings))
        for spelling in spellings:
            _write_record(writer, spelling)
        _write_u64(writer, len(identities))
        for identity in identities:
            encoded = json.dumps(
                _identity_document(identity),
                ensure_ascii=True,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("ascii")
            _write_record(writer, encoded)
        _write_u64(writer, len(tokens))
        writer.write(struct.pack("<BB", array("I").itemsize, int(sys.byteorder == "little")))
        for column in columns:
            byte_view = memoryview(column).cast("B")
            _write_u64(writer, len(byte_view))
            for offset in range(0, len(byte_view), _IO_BLOCK_BYTES):
                writer.write(byte_view[offset : offset + _IO_BLOCK_BYTES])
            byte_view.release()
        stream.flush()
        os.fsync(stream.fileno())
        return writer.digest.hexdigest(), writer.count


class _PayloadReader:
    def __init__(self, stream, remaining: int) -> None:
        self.stream = stream
        self.remaining = remaining
        self.digest = hashlib.sha256()
        self.count = 0

    def read_exact(self, size: int) -> bytes:
        if size < 0 or size > self.remaining:
            raise ValueError("payload record exceeds file bounds")
        chunks: list[bytes] = []
        remaining = size
        while remaining:
            block = self.stream.read(min(remaining, _IO_BLOCK_BYTES))
            if not block:
                raise ValueError("payload is truncated")
            chunks.append(block)
            self.digest.update(block)
            self.count += len(block)
            remaining -= len(block)
        self.remaining -= size
        return b"".join(chunks)

    def u64(self) -> int:
        return struct.unpack("<Q", self.read_exact(8))[0]

    def record(self) -> bytes:
        size = self.u64()
        if size > _MAX_RECORD_BYTES:
            raise ValueError("payload record is too large")
        _check_cache_rss(size)
        return self.read_exact(size)


def _read_column(reader: _PayloadReader, token_count: int) -> array:
    size = reader.u64()
    expected = token_count * 4
    if size != expected:
        raise ValueError("packed column byte count is invalid")
    result = array("I")
    remaining = size
    while remaining:
        _check_cache_rss(min(remaining, _IO_BLOCK_BYTES) + 4096)
        block = reader.read_exact(min(remaining, _IO_BLOCK_BYTES))
        if len(block) % 4:
            raise ValueError("packed column block is misaligned")
        result.frombytes(block)
        remaining -= len(block)
    return result


def _read_payload(
    stream,
    configuration: PreprocessConfiguration,
    dependencies: tuple[FileIdentity, ...],
    size: int,
    expected_digest: str,
) -> CompactTokenSequence:
    limits = AuditLimits()
    _check_cache_rss()
    dependency_by_path = {
        _path_key(identity.canonical): identity for identity in dependencies
    }
    reader = _PayloadReader(stream, size)
    if reader.read_exact(len(_PAYLOAD_MAGIC)) != _PAYLOAD_MAGIC:
        raise ValueError("payload magic is invalid")
    spelling_count = reader.u64()
    if spelling_count > (limits.retained_token_bytes // 8) + 1:
        raise ValueError("spelling table is too large")
    _check_cache_rss(spelling_count * 8)
    spellings = tuple(reader.record() for _ in range(spelling_count))
    identity_count = reader.u64()
    if identity_count > len(dependencies) + 1:
        raise ValueError("identity table is too large")
    _check_cache_rss(identity_count * 8)
    identities = tuple(
        _identity_from_document(
            json.loads(reader.record().decode("ascii")), dependency_by_path
        )
        for _ in range(identity_count)
    )
    token_count = reader.u64()
    if token_count > limits.retained_token_bytes // 16:
        raise ValueError("packed token count is too large")
    _check_cache_rss(token_count * 16)
    itemsize, little_endian = struct.unpack("<BB", reader.read_exact(2))
    if itemsize != 4 or little_endian != int(sys.byteorder == "little"):
        raise ValueError("payload packed integer representation is incompatible")
    columns = tuple(_read_column(reader, token_count) for _ in range(4))
    if reader.remaining != 0:
        raise ValueError("payload has trailing data")
    if reader.count != size or reader.digest.hexdigest() != expected_digest:
        raise ValueError("cache payload digest changed while parsing")
    return CompactTokenSequence._from_owned_packed(
        configuration,
        spellings=spellings,
        identities=identities,
        spelling_ids=columns[0],
        identity_ids=columns[1],
        inclusion_ids=columns[2],
        original_lines=columns[3],
        limits=limits,
    )


def _file_identity_tuple(metadata: os.stat_result) -> tuple[int, int | None, int, int]:
    inode = int(metadata.st_ino) if int(metadata.st_ino) != 0 else None
    return (
        int(metadata.st_dev),
        inode,
        int(metadata.st_size),
        int(metadata.st_mtime_ns),
    )


class _HeldCacheFile:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.stream = None
        self.identity = None

    def __enter__(self) -> "_HeldCacheFile":
        before = _regular_unlinked_file(self.path)
        stream = self.path.open("rb")
        opened = os.fstat(stream.fileno())
        if (
            not stat.S_ISREG(opened.st_mode)
            or int(getattr(opened, "st_nlink", 1)) != 1
            or _file_identity_tuple(opened) != _file_identity_tuple(before)
        ):
            stream.close()
            raise _UnsafeCacheNamespaceError(
                "cache file changed while opening"
            )
        self.stream = stream
        self.identity = _file_identity_tuple(opened)
        return self

    def verify(self) -> None:
        assert self.stream is not None and self.identity is not None
        opened = os.fstat(self.stream.fileno())
        try:
            current = _regular_unlinked_file(self.path)
        except FileNotFoundError as error:
            raise _UnsafeCacheNamespaceError(
                "cache file was removed while held"
            ) from error
        if (
            _file_identity_tuple(opened) != self.identity
            or _file_identity_tuple(current) != self.identity
        ):
            raise _UnsafeCacheNamespaceError("cache file changed while held")

    def __exit__(self, _type, _value, _traceback) -> None:
        assert self.stream is not None
        self.stream.close()
        self.stream = None


def _before_payload_parse(_path: Path) -> None:
    """Test seam for deterministic replacement-race coverage."""


def _write_json_fsynced(path: Path, document: object) -> None:
    encoded = json.dumps(
        document,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")
    with path.open("xb") as stream:
        stream.write(encoded)
        stream.flush()
        os.fsync(stream.fileno())


_CACHE_ENTRY_FILES = frozenset({"active.lock", "manifest.json", "payload.bin"})


class _HeldDirectory:
    """Verified non-reparse directory held against replacement while inspected."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.identity: tuple[int, int | None] | None = None
        self._fd: int | None = None
        self._handle = None
        self._kernel32 = None

    def __enter__(self) -> "_HeldDirectory":
        before = _ordinary_directory(self.path)
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class FileInformation(ctypes.Structure):
                _fields_ = (
                    ("attributes", wintypes.DWORD),
                    ("creation", wintypes.FILETIME),
                    ("access", wintypes.FILETIME),
                    ("write", wintypes.FILETIME),
                    ("volume", wintypes.DWORD),
                    ("size_high", wintypes.DWORD),
                    ("size_low", wintypes.DWORD),
                    ("links", wintypes.DWORD),
                    ("index_high", wintypes.DWORD),
                    ("index_low", wintypes.DWORD),
                )

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
            kernel32.GetFileInformationByHandle.argtypes = (
                wintypes.HANDLE,
                ctypes.POINTER(FileInformation),
            )
            kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
            kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
            kernel32.CloseHandle.restype = wintypes.BOOL
            handle = kernel32.CreateFileW(
                str(self.path),
                0x0001,
                0x00000001 | 0x00000002,
                None,
                3,
                0x02000000 | 0x00200000,
                None,
            )
            invalid = ctypes.c_void_p(-1).value
            if handle in (None, invalid):
                raise OSError(ctypes.get_last_error(), "cannot hold cache directory")
            information = FileInformation()
            if not kernel32.GetFileInformationByHandle(handle, ctypes.byref(information)):
                error = ctypes.get_last_error()
                kernel32.CloseHandle(handle)
                raise OSError(error, "cannot inspect held cache directory")
            if not (information.attributes & 0x10) or information.attributes & _REPARSE_ATTRIBUTE:
                kernel32.CloseHandle(handle)
                raise OSError("held cache path is not an ordinary directory")
            after = _ordinary_directory(self.path)
            if _directory_identity(before) != _directory_identity(after):
                kernel32.CloseHandle(handle)
                raise OSError("cache directory changed while opening")
            self.identity = _directory_identity(after)
            self._handle = handle
            self._kernel32 = kernel32
            return self

        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(self.path, flags)
        opened = os.fstat(fd)
        if (
            not stat.S_ISDIR(opened.st_mode)
            or _directory_identity(opened) != _directory_identity(before)
        ):
            os.close(fd)
            raise OSError("cache directory changed while opening")
        self.identity = _directory_identity(opened)
        self._fd = fd
        return self

    def delete_known_files(self) -> None:
        if os.name == "nt":
            names = os.listdir(self.path)
            metadata = [(name, (self.path / name).lstat()) for name in names]
            if any(
                name not in _CACHE_ENTRY_FILES
                or _is_link(item)
                or not stat.S_ISREG(item.st_mode)
                or int(getattr(item, "st_nlink", 1)) != 1
                for name, item in metadata
            ):
                raise OSError("cache directory contains an unexpected path")
            for name, _item in metadata:
                (self.path / name).unlink()
            return

        assert self._fd is not None
        names = os.listdir(self._fd)
        metadata = [
            (name, os.stat(name, dir_fd=self._fd, follow_symlinks=False))
            for name in names
        ]
        if any(
            name not in _CACHE_ENTRY_FILES
            or stat.S_ISLNK(item.st_mode)
            or not stat.S_ISREG(item.st_mode)
            or int(getattr(item, "st_nlink", 1)) != 1
            for name, item in metadata
        ):
            raise OSError("cache directory contains an unexpected path")
        for name, _item in metadata:
            os.unlink(name, dir_fd=self._fd)

    def __exit__(self, _type, _value, _traceback) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None
        self.identity = None


def _remove_held_flat_directory(
    path: Path,
    *,
    after_hold: Callable[[Path], None] | None = None,
    expected_identity: tuple[int, int | None] | None = None,
) -> bool:
    try:
        with _HeldDirectory(path) as held:
            if expected_identity is not None and held.identity != expected_identity:
                return False
            if after_hold is not None:
                after_hold(path)
            held.delete_known_files()
        path.rmdir()
        return True
    except OSError:
        return False


def _after_cleanup_quarantine(_path: Path) -> None:
    """Test seam for deterministic cleanup replacement-race coverage."""


_COMPILER_INSPECTION_SCHEMA = "olr-gpu-compiler-inspection-cache-v3"
_COMPILER_INSPECTION_MAX_BYTES = 512 * 1024


def _inspection_environment_digest(environment) -> str:
    if not isinstance(environment, dict) and not hasattr(environment, "items"):
        raise AuditInfrastructureError("compiler inspection environment is invalid")
    entries: list[tuple[str, str]] = []
    seen: set[str] = set()
    for name, value in environment.items():
        if not isinstance(name, str) or not isinstance(value, str) or "\0" in name or "\0" in value:
            raise AuditInfrastructureError("compiler inspection environment is invalid")
        normalized = name.casefold() if os.name == "nt" else name
        if normalized in seen:
            raise AuditInfrastructureError("compiler inspection environment is ambiguous")
        seen.add(normalized)
        entries.append((normalized, value))
    digest = hashlib.sha256()
    for name, value in sorted(entries):
        for item in (name.encode("utf-8"), value.encode("utf-8")):
            digest.update(len(item).to_bytes(8, "big"))
            digest.update(item)
    return digest.hexdigest()


def _held_compiler_snapshot(compiler: Path) -> tuple[FileIdentity, str]:
    if not isinstance(compiler, Path) or not compiler.is_absolute():
        raise AuditInfrastructureError("compiler inspection executable is invalid")
    try:
        canonical = compiler.resolve(strict=True)
        before = _regular_unlinked_file(canonical)
        digest = hashlib.sha256()
        with canonical.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if _file_identity_tuple(opened) != _file_identity_tuple(before):
                raise OSError("compiler changed while opening")
            while chunk := stream.read(_IO_BLOCK_BYTES):
                digest.update(chunk)
            after_open = os.fstat(stream.fileno())
        after = _regular_unlinked_file(canonical)
    except OSError as error:
        raise AuditInfrastructureError("compiler inspection executable changed") from error
    if _file_identity_tuple(after_open) != _file_identity_tuple(before) or _file_identity_tuple(after) != _file_identity_tuple(before):
        raise AuditInfrastructureError("compiler inspection executable changed")
    inode = int(before.st_ino) if int(before.st_ino) != 0 else None
    return (
        FileIdentity(canonical, None, int(before.st_dev), inode, 0, False),
        digest.hexdigest(),
    )


def compiler_inspection_cache_key(
    compiler: Path,
    compiler_family: CompilerFamily,
    launcher_environment,
    dependency_roots: DependencyRootAuthority,
    expected_audit_engine_fingerprint: str,
    *,
    executable_capability_digest: str,
    resolved_runtime_closure_digest: str,
) -> str:
    authority = validate_dependency_root_authority(dependency_roots)
    if not isinstance(compiler_family, CompilerFamily):
        raise AuditInfrastructureError("compiler inspection family is invalid")
    if expected_audit_engine_fingerprint:
        if (
            not isinstance(expected_audit_engine_fingerprint, str)
            or len(expected_audit_engine_fingerprint) != 64
            or any(character not in "0123456789abcdef" for character in expected_audit_engine_fingerprint)
        ):
            raise AuditInfrastructureError("audit engine fingerprint is invalid")
    try:
        canonical = compiler.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError("compiler inspection executable is invalid") from error
    matches = []
    for binding in authority.external_roots:
        try:
            canonical.relative_to(binding.resolved_root)
        except ValueError:
            continue
        matches.append(binding)
    if len(matches) != 1:
        raise AuditInfrastructureError(
            "compiler inspection executable is outside the local dependency authority"
        )
    for label, value in (
        ("compiler executable capability", executable_capability_digest),
        ("compiler resolved runtime closure", resolved_runtime_closure_digest),
    ):
        if (
            not isinstance(value, str) or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
        ):
            raise AuditInfrastructureError(f"{label} digest is invalid")
    digest = hashlib.sha256()
    for value in (
        _COMPILER_INSPECTION_SCHEMA,
        compiler_family.value,
        _inspection_environment_digest(launcher_environment),
        authority.portable_authority_digest,
        expected_audit_engine_fingerprint,
        executable_capability_digest,
        resolved_runtime_closure_digest,
    ):
        encoded = value.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "little"))
        digest.update(encoded)
    return digest.hexdigest()


class CompilerInspectionCache:
    """Exact atomic compiler-inspection records under a local bound root."""

    def __init__(self, root: Path) -> None:
        if not isinstance(root, Path) or not root.is_absolute():
            raise AuditInfrastructureError("compiler inspection cache root is invalid")
        try:
            root.mkdir(parents=True, exist_ok=True)
            metadata = _ordinary_directory(root)
        except OSError as error:
            raise AuditInfrastructureError("compiler inspection cache root is unavailable") from error
        self.root = root
        self._root_identity = _directory_identity(metadata)

    def _assert_root(self) -> None:
        try:
            current = _ordinary_directory(self.root)
        except OSError as error:
            raise AuditInfrastructureError("compiler inspection cache root was replaced") from error
        if _directory_identity(current) != self._root_identity:
            raise AuditInfrastructureError("compiler inspection cache root was replaced")

    def _path(self, key: str) -> Path:
        return self.root / f".compiler-inspection-{key}.json"

    def _root_lock(
        self, deadline: float, cancel_event: object | None = None
    ) -> _SharedCacheFileLock:
        return _SharedCacheFileLock(
            self.root / ".compiler-inspection-root.lock",
            self.root,
            self._root_identity,
            deadline,
            cancel_event,
        )

    def _key_lock(
        self, key: str, deadline: float, cancel_event: object | None = None
    ) -> _SharedCacheFileLock:
        lane = hashlib.sha256(
            f"compiler-inspection:{key}".encode("utf-8")
        ).digest()[0] % 8
        return _SharedCacheFileLock(
            self.root / f".compiler-inspection-key-{lane}.lock",
            self.root,
            self._root_identity,
            deadline,
            cancel_event,
        )

    def load(
        self,
        compiler: Path,
        compiler_family: CompilerFamily,
        launcher_environment,
        dependency_roots: DependencyRootAuthority,
        expected_audit_engine_fingerprint: str,
        executable_capability_digest: str,
        resolved_runtime_closure_digest: str,
        pipeline_deadline: float,
        *,
        held_executable_identity: FileIdentity | None = None,
        held_executable_sha256: str | None = None,
    ) -> CompilerInspection | None:
        authority = validate_dependency_root_authority(dependency_roots)
        operation_started = time.monotonic()
        operation_deadline = min(
            pipeline_deadline,
            operation_started + _DEFAULT_CACHE_OPERATION_SECONDS,
        )
        if operation_started >= operation_deadline:
            raise AuditInfrastructureError("compiler inspection cache deadline exceeded")
        key = compiler_inspection_cache_key(
            compiler, compiler_family, launcher_environment, authority,
            expected_audit_engine_fingerprint,
            executable_capability_digest=executable_capability_digest,
            resolved_runtime_closure_digest=resolved_runtime_closure_digest,
        )
        path = self._path(key)
        try:
            self._assert_root()
            metadata = _regular_unlinked_file(path)
            with _HeldCacheFile(path) as held:
                assert held.stream is not None
                payload = held.stream.read(_COMPILER_INSPECTION_MAX_BYTES + 1)
                held.verify()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise AuditInfrastructureError(
                "compiler inspection cache namespace is unsafe"
            ) from error
        try:
            if (
                metadata.st_size > _COMPILER_INSPECTION_MAX_BYTES
                or len(payload) > _COMPILER_INSPECTION_MAX_BYTES
            ):
                raise ValueError("compiler inspection manifest is too large")
            document = json.loads(payload.decode("ascii"))
            if not isinstance(document, dict) or tuple(document) != (
                "schema", "key", "dependency_root_authority_digest",
                "executable_capability_digest", "resolved_runtime_closure_digest",
                "inspection_bytes", "inspection_sha256", "inspection"
            ):
                raise ValueError("compiler inspection manifest schema is invalid")
            if (
                document["schema"] != _COMPILER_INSPECTION_SCHEMA
                or document["key"] != key
                or document["dependency_root_authority_digest"] != authority.portable_authority_digest
                or document["executable_capability_digest"] != executable_capability_digest
                or document["resolved_runtime_closure_digest"] != resolved_runtime_closure_digest
                or not isinstance(document["inspection_bytes"], int)
                or isinstance(document["inspection_bytes"], bool)
                or document["inspection_bytes"] < 0
                or document["inspection_bytes"] > _COMPILER_INSPECTION_MAX_BYTES
                or not isinstance(document["inspection_sha256"], str)
                or len(document["inspection_sha256"]) != 64
                or any(character not in "0123456789abcdef" for character in document["inspection_sha256"])
                or not isinstance(document["inspection"], str)
            ):
                raise ValueError("compiler inspection manifest is invalid")
            embedded = base64.b64decode(
                document["inspection"].encode("ascii"), validate=True
            )
            if (
                len(embedded) != document["inspection_bytes"]
                or hashlib.sha256(embedded).hexdigest()
                != document["inspection_sha256"]
            ):
                raise ValueError("compiler inspection payload authentication failed")
        except (
            UnicodeError, ValueError, TypeError, json.JSONDecodeError,
        ):
            return None
        try:
            compiler_before = compiler.stat()
        except OSError:
            return None
        compiler_generation = (
            int(compiler_before.st_dev), int(compiler_before.st_ino) or None,
            int(compiler_before.st_size), int(compiler_before.st_mtime_ns),
            int(getattr(compiler_before, "st_ctime_ns", 0)),
        )
        try:
            inspection = decode_compiler_inspection(
                embedded,
                validation_deadline=operation_deadline,
                held_executable_identity=held_executable_identity,
                held_executable_sha256=held_executable_sha256,
            )
        except _HeldCompilerCapabilityMismatch:
            raise
        except AuditInfrastructureError:
            if time.monotonic() >= operation_deadline:
                raise
            try:
                compiler_after = compiler.stat()
            except OSError as error:
                raise AuditInfrastructureError(
                    "compiler executable changed during cache decode"
                ) from error
            if (
                int(compiler_after.st_dev), int(compiler_after.st_ino) or None,
                int(compiler_after.st_size), int(compiler_after.st_mtime_ns),
                int(getattr(compiler_after, "st_ctime_ns", 0)),
            ) != compiler_generation:
                raise
            return None
        try:
            compiler_after = compiler.stat()
        except OSError as error:
            raise AuditInfrastructureError(
                "compiler executable changed during cache decode"
            ) from error
        if (
            int(compiler_after.st_dev), int(compiler_after.st_ino) or None,
            int(compiler_after.st_size), int(compiler_after.st_mtime_ns),
            int(getattr(compiler_after, "st_ctime_ns", 0)),
        ) != compiler_generation:
            raise AuditInfrastructureError(
                "compiler executable content changed during cache decode"
            )
        try:
            if (
                inspection.compiler_family is not compiler_family
                or inspection.executable_capability_digest
                != executable_capability_digest
            ):
                raise ValueError("compiler inspection executable differs")
            return inspection
        except (
            ValueError, TypeError,
        ):
            return None

    def publish(
        self,
        compiler: Path,
        compiler_family: CompilerFamily,
        launcher_environment,
        dependency_roots: DependencyRootAuthority,
        expected_audit_engine_fingerprint: str,
        executable_capability_digest: str,
        resolved_runtime_closure_digest: str,
        inspection: CompilerInspection,
        pipeline_deadline: float,
        cancel_event: object | None = None,
    ) -> CompilerInspection:
        authority = validate_dependency_root_authority(dependency_roots)
        operation_started = time.monotonic()
        operation_deadline = min(
            pipeline_deadline,
            operation_started + _DEFAULT_CACHE_OPERATION_SECONDS,
        )
        if operation_started >= operation_deadline:
            raise AuditInfrastructureError("compiler inspection cache deadline exceeded")
        key = compiler_inspection_cache_key(
            compiler, compiler_family, launcher_environment, authority,
            expected_audit_engine_fingerprint,
            executable_capability_digest=executable_capability_digest,
            resolved_runtime_closure_digest=resolved_runtime_closure_digest,
        )
        if (
            not isinstance(inspection, CompilerInspection)
            or inspection.compiler_family is not compiler_family
            or inspection.executable_capability_digest
            != executable_capability_digest
        ):
            raise AuditInfrastructureError(
                "compiler inspection publication capability digest differs"
            )
        embedded = encode_compiler_inspection(inspection)
        document = {
            "schema": _COMPILER_INSPECTION_SCHEMA,
            "key": key,
            "dependency_root_authority_digest": authority.portable_authority_digest,
            "executable_capability_digest": executable_capability_digest,
            "resolved_runtime_closure_digest": resolved_runtime_closure_digest,
            "inspection_bytes": len(embedded),
            "inspection_sha256": hashlib.sha256(embedded).hexdigest(),
            "inspection": base64.b64encode(embedded).decode("ascii"),
        }
        encoded = json.dumps(
            document, ensure_ascii=True, separators=(",", ":")
        ).encode("ascii")
        if len(encoded) > _COMPILER_INSPECTION_MAX_BYTES:
            raise AuditInfrastructureError("compiler inspection manifest is too large")
        temporary = self.root / f".tmp-inspection-{uuid.uuid4().hex}"
        temporary_identity: tuple[int, int | None, int, int] | None = None
        path = self._path(key)
        with self._root_lock(
            operation_deadline, cancel_event
        ), self._key_lock(
            key, operation_deadline, cancel_event
        ):
            try:
                self._assert_root()
                with temporary.open("xb") as stream:
                    try:
                        stream.write(encoded)
                        stream.flush()
                        os.fsync(stream.fileno())
                    finally:
                        temporary_identity = _file_identity_tuple(
                            os.fstat(stream.fileno())
                        )
                self._assert_root()
                try:
                    _regular_unlinked_file(path)
                except FileNotFoundError:
                    pass
                except OSError as error:
                    raise AuditInfrastructureError(
                        "compiler inspection cache namespace is unsafe"
                    ) from error
                else:
                    winner = self.load(
                        compiler,
                        compiler_family,
                        launcher_environment,
                        authority,
                        expected_audit_engine_fingerprint,
                        executable_capability_digest,
                        resolved_runtime_closure_digest,
                        operation_deadline,
                        held_executable_identity=inspection.executable_identity,
                        held_executable_sha256=inspection.executable_sha256,
                    )
                    if winner is not None:
                        if winner != inspection:
                            raise AuditInfrastructureError(
                                "concurrent compiler inspection winner differs"
                            )
                        if _file_identity_tuple(
                            _regular_unlinked_file(temporary)
                        ) != temporary_identity:
                            raise AuditInfrastructureError(
                                "compiler inspection publication temporary was replaced"
                            )
                        temporary.unlink()
                        return winner
                os.replace(temporary, path)
                self._assert_root()
            except BaseException as error:
                cleanup_failure: BaseException | None = None
                if temporary_identity is not None:
                    try:
                        current_identity = _file_identity_tuple(
                            _regular_unlinked_file(temporary)
                        )
                        if current_identity != temporary_identity:
                            cleanup_failure = AuditInfrastructureError(
                                "compiler inspection publication temporary was replaced"
                            )
                        else:
                            temporary.unlink()
                    except FileNotFoundError:
                        pass
                    except OSError as cleanup_error:
                        cleanup_failure = cleanup_error
                if cleanup_failure is not None:
                    raise AuditInfrastructureError(
                        "cannot remove compiler inspection publication temporary "
                        f"after {type(error).__name__}: {error}"
                    ) from cleanup_failure
                if isinstance(error, AuditInfrastructureError):
                    raise
                if not isinstance(error, OSError):
                    raise
                raise AuditInfrastructureError("cannot publish compiler inspection") from error
        return inspection


class PreprocessCache:
    def __init__(
        self,
        root: Path,
        maximum_bytes: int = 512 * 1024 * 1024,
        maximum_entries: int = 256,
    ) -> None:
        if not isinstance(root, Path) or not root.is_absolute():
            raise AuditInfrastructureError("cache root must be an absolute path")
        if (
            not isinstance(maximum_bytes, int)
            or isinstance(maximum_bytes, bool)
            or maximum_bytes < 0
            or not isinstance(maximum_entries, int)
            or isinstance(maximum_entries, bool)
            or maximum_entries < 0
        ):
            raise AuditInfrastructureError("cache bounds are invalid")
        try:
            root.mkdir(parents=True, exist_ok=True)
            root_metadata = _ordinary_directory(root)
        except OSError as error:
            raise AuditInfrastructureError(f"cache root is unavailable: {root}") from error
        self.root = root
        self.maximum_bytes = maximum_bytes
        self.maximum_entries = maximum_entries
        self._root_identity = _directory_identity(root_metadata)
        self._publication_lock = threading.Lock()

    @staticmethod
    def _operation_deadline(deadline: float | None) -> float:
        internal_deadline = time.monotonic() + _DEFAULT_CACHE_OPERATION_SECONDS
        return internal_deadline if deadline is None else min(
            deadline, internal_deadline
        )

    @staticmethod
    def _check_publication_budget(
        deadline: float, cancel_event: object | None
    ) -> None:
        if cancel_event is not None:
            is_set = getattr(cancel_event, "is_set", None)
            if not callable(is_set):
                raise AuditInfrastructureError(
                    "cache publication cancellation event is invalid"
                )
            cancelled = is_set()
            if not isinstance(cancelled, bool):
                raise AuditInfrastructureError(
                    "cache publication cancellation event is invalid"
                )
            if cancelled:
                raise AuditInfrastructureError("cache publication cancelled")
        if time.monotonic() >= deadline:
            raise AuditInfrastructureError("cache publication deadline exceeded")

    def _root_lock(
        self, deadline: float, cancel_event: object | None = None
    ) -> _SharedCacheFileLock:
        return _SharedCacheFileLock(
            self.root / ".preprocess-root.lock",
            self.root,
            self._root_identity,
            deadline,
            cancel_event,
        )

    def _key_lock(
        self,
        key: str,
        deadline: float,
        cancel_event: object | None = None,
    ) -> _SharedCacheFileLock:
        lane = hashlib.sha256(f"preprocess:{key}".encode("utf-8")).digest()[0] % 8
        return _SharedCacheFileLock(
            self.root / f".preprocess-key-{lane}.lock",
            self.root,
            self._root_identity,
            deadline,
            cancel_event,
        )

    def _acquire_key_barrier(
        self,
        key: str,
        deadline: float,
        cancel_event: object | None = None,
    ) -> _SharedCacheFileLock:
        key_lock = self._key_lock(key, deadline, cancel_event)
        with self._root_lock(deadline, cancel_event):
            key_lock.__enter__()
        return key_lock

    def _assert_root_identity(self) -> None:
        try:
            metadata = _ordinary_directory(self.root)
        except OSError as error:
            raise AuditInfrastructureError("cache root was replaced") from error
        if _directory_identity(metadata) != self._root_identity:
            raise AuditInfrastructureError("cache root identity was replaced")

    def _configuration_key(
        self,
        configuration: PreprocessConfiguration,
    ) -> str:
        if not isinstance(configuration, PreprocessConfiguration):
            raise AuditInfrastructureError("preprocess configuration is invalid")
        semantic = {
            "schema": _SCHEMA,
            "digest": configuration.digest,
            "family": configuration.family.value,
            "compiler": str(configuration.compiler),
            "working_directory": str(configuration.working_directory),
            "source": str(configuration.source.canonical),
            "arguments": configuration.arguments,
            "environment_digest": configuration.environment_digest,
            "dependency_root_authority_digest": configuration.dependency_root_authority_digest,
            "compiler_capability_digest": configuration.compiler_capability_digest,
        }
        return hashlib.sha256(
            json.dumps(
                semantic,
                ensure_ascii=True,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
        ).hexdigest()

    def _access_path(self, key: str) -> Path:
        return self.root / f".access-{key}"

    def _record_access(self, key: str, now: float | None = None) -> None:
        temporary = self.root / f".tmp-access-{key}-{uuid.uuid4().hex}"
        try:
            self._assert_root_identity()
            with temporary.open("xb") as stream:
                stream.write(b"1\n")
                stream.flush()
                os.fsync(stream.fileno())
            timestamp = time.time() if now is None else now
            os.utime(temporary, (timestamp, timestamp))
            self._assert_root_identity()
            os.replace(temporary, self._access_path(key))
            self._assert_root_identity()
        except OSError:
            try:
                temporary.unlink()
            except OSError:
                pass

    def _load_manifest(self, entry: Path, key: str) -> dict[str, object]:
        _ordinary_directory(entry)
        manifest_path = entry / "manifest.json"
        metadata = _regular_unlinked_file(manifest_path)
        if metadata.st_size > _MAX_MANIFEST_BYTES:
            raise ValueError("cache manifest is too large")
        document = json.loads(manifest_path.read_text(encoding="ascii"))
        if (
            not isinstance(document, dict)
            or document.get("schema") != _SCHEMA
            or document.get("key") != key
        ):
            raise ValueError("cache manifest schema or key is invalid")
        return document

    def _validated_dependencies(
        self, document: dict[str, object]
    ) -> tuple[FileIdentity, ...]:
        records = document.get("dependencies")
        if not isinstance(records, list) or not records:
            raise ValueError("cache dependency manifest is invalid")
        result: list[FileIdentity] = []
        seen_paths: set[str] = set()
        seen_filesystem_ids: set[tuple[int, int]] = set()
        for record in records:
            if not isinstance(record, dict):
                raise ValueError("cache dependency record is invalid")
            canonical = record.get("canonical")
            relative = record.get("relative")
            digest = record.get("sha256")
            device = record.get("device")
            inode = record.get("inode")
            line_count = record.get("line_count")
            production = record.get("production")
            if (
                not isinstance(canonical, str)
                or not Path(canonical).is_absolute()
                or (relative is not None and not isinstance(relative, str))
                or not isinstance(digest, str)
                or len(digest) != 64
                or (device is not None and not isinstance(device, int))
                or (inode is not None and not isinstance(inode, int))
                or not isinstance(line_count, int)
                or line_count < 0
                or not isinstance(production, bool)
            ):
                raise ValueError("cache dependency record fields are invalid")
            path_key = _path_key(Path(canonical))
            filesystem_id = (device, inode) if device is not None and inode is not None else None
            if path_key in seen_paths or (
                filesystem_id is not None and filesystem_id in seen_filesystem_ids
            ):
                raise ValueError("cache dependency aliases another dependency")
            seen_paths.add(path_key)
            if filesystem_id is not None:
                seen_filesystem_ids.add(filesystem_id)
            identity = FileIdentity(
                canonical=Path(canonical),
                relative=PurePosixPath(relative) if relative is not None else None,
                device=device,
                inode=inode,
                line_count=line_count,
                production=production,
            )
            if _dependency_digest(identity) != digest:
                raise ValueError("cache dependency content changed")
            result.append(identity)
        return tuple(result)

    def load(
        self,
        configuration: PreprocessConfiguration,
        deadline: float | None = None,
        cancel_event: object | None = None,
    ) -> PreprocessedTranslationUnitView | None:
        key = self._configuration_key(configuration)
        operation_deadline = self._operation_deadline(deadline)
        key_lock = self._acquire_key_barrier(
            key, operation_deadline, cancel_event
        )
        try:
            return self._load_unlocked(configuration, key)
        finally:
            key_lock.__exit__(None, None, None)

    def _load_unlocked(
        self,
        configuration: PreprocessConfiguration,
        key: str,
    ) -> PreprocessedTranslationUnitView | None:
        entry = self.root / key
        self._assert_root_identity()
        try:
            document = self._load_manifest(entry, key)
            dependencies = self._validated_dependencies(document)
            payload = entry / "payload.bin"
            expected_size = document.get("payload_bytes")
            expected_digest = document.get("payload_sha256")
            with _HeldCacheFile(payload) as held:
                assert held.stream is not None and held.identity is not None
                if (
                    not isinstance(expected_size, int)
                    or expected_size < 0
                    or expected_size != held.identity[2]
                    or not isinstance(expected_digest, str)
                    or len(expected_digest) != 64
                ):
                    raise ValueError("cache payload metadata is invalid")
                _before_payload_parse(payload)
                held.verify()
                tokens = _read_payload(
                    held.stream,
                    configuration,
                    dependencies,
                    expected_size,
                    expected_digest,
                )
                held.verify()
        except _UnsafeCacheNamespaceError as error:
            raise AuditInfrastructureError(
                "cache namespace is unsafe"
            ) from error
        except (
            AuditInfrastructureError,
            FileNotFoundError,
            json.JSONDecodeError,
            OSError,
            UnicodeError,
            ValueError,
            KeyError,
            TypeError,
            struct.error,
        ):
            return None
        self._record_access(key)
        return PreprocessedTranslationUnitView(configuration, tokens, dependencies)

    def _snapshot_dependencies(
        self,
        dependencies: tuple[FileIdentity, ...],
        *,
        force: bool,
    ) -> tuple[_DependencySnapshot, ...]:
        return tuple(
            _DependencySnapshot(identity, _dependency_digest(identity, force=force))
            for identity in dependencies
        )

    @staticmethod
    def _same_dependencies(
        dependencies: tuple[FileIdentity, ...],
        snapshots: tuple[_DependencySnapshot, ...],
    ) -> bool:
        return len(dependencies) == len(snapshots) and all(
            _identity_document(identity) == _identity_document(snapshot.identity)
            for identity, snapshot in zip(dependencies, snapshots)
        )

    def _validate_stable_dependencies(
        self,
        dependencies: tuple[FileIdentity, ...],
        snapshots: tuple[_DependencySnapshot, ...],
    ) -> None:
        if not self._same_dependencies(dependencies, snapshots):
            raise AuditInfrastructureError(
                "dependency closure changed during preprocessing"
            )
        try:
            current = self._snapshot_dependencies(dependencies, force=True)
        except OSError as error:
            raise AuditInfrastructureError(
                "dependency changed during preprocessing"
            ) from error
        if current != snapshots:
            raise AuditInfrastructureError(
                "dependency content changed during preprocessing"
            )

    def publish(
        self,
        view: PreprocessedTranslationUnitView,
    ) -> None:
        try:
            snapshots = self._snapshot_dependencies(view.dependencies, force=True)
        except OSError as error:
            raise AuditInfrastructureError(
                f"cannot snapshot GPU capability dependencies: {error}"
            ) from error
        self._publish_stabilized(view, snapshots)

    @staticmethod
    def _accept_winner(
        local: PreprocessedTranslationUnitView,
        winner: PreprocessedTranslationUnitView,
    ) -> PreprocessedTranslationUnitView:
        if _preprocessed_view_semantic_digest(local) != _preprocessed_view_semantic_digest(
            winner
        ):
            raise AuditInfrastructureError(
                "concurrent cache winner differs from stabilized preprocessing"
            )
        return winner

    @staticmethod
    def _discard_losing_temporary(
        temporary: Path,
        expected_identity: tuple[int, int | None],
    ) -> None:
        if not _remove_held_flat_directory(
            temporary, expected_identity=expected_identity
        ):
            try:
                current_identity = _directory_identity(
                    _ordinary_directory(temporary)
                )
            except FileNotFoundError:
                return
            except OSError as error:
                raise AuditInfrastructureError(
                    "losing cache publication temporary was replaced"
                ) from error
            if current_identity != expected_identity:
                raise AuditInfrastructureError(
                    "losing cache publication temporary was replaced"
                )
            raise AuditInfrastructureError(
                "cannot remove losing cache publication temporary"
            )

    def _publish_stabilized(
        self,
        view: PreprocessedTranslationUnitView,
        snapshots: tuple[_DependencySnapshot, ...],
        *,
        final_validation: Callable[[], None] | None = None,
        deadline: float | None = None,
        cancel_event: object | None = None,
    ) -> PreprocessedTranslationUnitView:
        if not isinstance(view, PreprocessedTranslationUnitView):
            raise AuditInfrastructureError("preprocessed view is invalid")
        if not isinstance(snapshots, tuple) or not all(
            isinstance(snapshot, _DependencySnapshot) for snapshot in snapshots
        ):
            raise AuditInfrastructureError("dependency snapshot is invalid")
        if final_validation is not None and not callable(final_validation):
            raise AuditInfrastructureError("publication validation is invalid")
        if not self._same_dependencies(view.dependencies, snapshots):
            raise AuditInfrastructureError(
                "dependency closure changed during preprocessing"
            )
        key = self._configuration_key(view.configuration)
        operation_deadline = self._operation_deadline(deadline)
        temporary = self.root / f".tmp-{key}-{uuid.uuid4().hex}"
        entry = self.root / key
        temporary_identity: tuple[int, int | None] | None = None
        published_identity: tuple[int, int | None] | None = None
        try:
            self._assert_root_identity()
            temporary.mkdir()
            temporary_identity = _directory_identity(_ordinary_directory(temporary))
            with _PublicationGuard(
                temporary / "active.lock", operation_deadline, cancel_event
            ):
                dependency_records: list[dict[str, object]] = []
                for snapshot in snapshots:
                    dependency_records.append(
                        {
                            **(_identity_document(snapshot.identity) or {}),
                            "sha256": snapshot.sha256,
                        }
                    )
                payload_digest, payload_bytes = _write_payload(
                    temporary / "payload.bin", view
                )
                _write_json_fsynced(
                    temporary / "manifest.json",
                    {
                        "schema": _SCHEMA,
                        "key": key,
                        "dependencies": dependency_records,
                        "payload_sha256": payload_digest,
                        "payload_bytes": payload_bytes,
                    },
                )
                self._validate_stable_dependencies(view.dependencies, snapshots)
            (temporary / "active.lock").unlink()
            key_lock = self._acquire_key_barrier(
                key, operation_deadline, cancel_event
            )
            publication_acquired = False
            try:
                while True:
                    self._check_publication_budget(
                        operation_deadline, cancel_event
                    )
                    remaining = max(
                        0.0, operation_deadline - time.monotonic()
                    )
                    if self._publication_lock.acquire(
                        timeout=min(0.01, remaining)
                    ):
                        publication_acquired = True
                        break
                self._check_publication_budget(operation_deadline, cancel_event)
            except BaseException:
                if publication_acquired:
                    self._publication_lock.release()
                key_lock.__exit__(None, None, None)
                raise
            try:
                try:
                    if entry.exists():
                        winner = self._load_unlocked(view.configuration, key)
                        if winner is not None:
                            winner = self._accept_winner(view, winner)
                            self._discard_losing_temporary(
                                temporary, temporary_identity
                            )
                            if final_validation is not None:
                                final_validation()
                            return winner
                        stale = self.root / f".stale-{key}-{uuid.uuid4().hex}"
                        stale_identity = _directory_identity(
                            _ordinary_directory(entry)
                        )
                        try:
                            os.rename(entry, stale)
                        except FileNotFoundError:
                            stale = None
                            stale_identity = None
                        try:
                            published_identity = _directory_identity(
                                _ordinary_directory(temporary)
                            )
                            os.rename(temporary, entry)
                        except FileExistsError:
                            winner = self._load_unlocked(view.configuration, key)
                            if winner is None:
                                raise
                            winner = self._accept_winner(view, winner)
                            self._discard_losing_temporary(
                                temporary, temporary_identity
                            )
                            if final_validation is not None:
                                final_validation()
                            return winner
                        finally:
                            if stale is not None and not _remove_held_flat_directory(
                                stale, expected_identity=stale_identity
                            ):
                                raise AuditInfrastructureError(
                                    "cannot remove stale cache entry"
                                )
                    else:
                        try:
                            published_identity = _directory_identity(
                                _ordinary_directory(temporary)
                            )
                            os.rename(temporary, entry)
                        except FileExistsError:
                            winner = self._load_unlocked(view.configuration, key)
                            if winner is None:
                                raise
                            winner = self._accept_winner(view, winner)
                            self._discard_losing_temporary(
                                temporary, temporary_identity
                            )
                            if final_validation is not None:
                                final_validation()
                            return winner
                    self._record_access(key)
                    if final_validation is not None:
                        final_validation()
                    return view
                except (AuditInfrastructureError, OSError, ValueError) as error:
                    if published_identity is not None and not _remove_held_flat_directory(
                        entry, expected_identity=published_identity
                    ):
                        try:
                            current_identity = _directory_identity(
                                _ordinary_directory(entry)
                            )
                        except (FileNotFoundError, OSError):
                            current_identity = None
                        if current_identity == published_identity:
                            raise AuditInfrastructureError(
                                "cannot remove invalid cache publication"
                            ) from error
                    if isinstance(error, AuditInfrastructureError):
                        raise
                    raise AuditInfrastructureError(
                        f"cannot publish GPU capability cache: {error}"
                    ) from error
            finally:
                self._publication_lock.release()
                key_lock.__exit__(None, None, None)
        except (AuditInfrastructureError, OSError, ValueError) as error:
            if not _remove_held_flat_directory(
                temporary, expected_identity=temporary_identity
            ):
                try:
                    temporary.lstat()
                except FileNotFoundError:
                    pass
                else:
                    raise AuditInfrastructureError(
                        "cannot remove cache publication temporary after "
                        f"{type(error).__name__}: {error}"
                    ) from error
            if published_identity is not None and not _remove_held_flat_directory(
                entry, expected_identity=published_identity
            ):
                try:
                    current_identity = _directory_identity(
                        _ordinary_directory(entry)
                    )
                except (FileNotFoundError, OSError):
                    current_identity = None
                if current_identity == published_identity:
                    raise AuditInfrastructureError(
                        "cannot remove invalid cache publication"
                    ) from error
            if isinstance(error, AuditInfrastructureError):
                raise
            raise AuditInfrastructureError(f"cannot publish GPU capability cache: {error}") from error

    def _entry_info(self, entry: Path) -> tuple[int, float] | None:
        key = entry.name
        try:
            document = self._load_manifest(entry, key)
            payload = entry / "payload.bin"
            manifest = entry / "manifest.json"
            payload_metadata = _regular_unlinked_file(payload)
            manifest_metadata = _regular_unlinked_file(manifest)
            if document.get("payload_bytes") != payload_metadata.st_size:
                return None
            access = self._access_path(key)
            try:
                access_metadata = _regular_unlinked_file(access)
                last_access = access_metadata.st_mtime
                access_bytes = access_metadata.st_size
            except OSError:
                last_access = manifest_metadata.st_mtime
                access_bytes = 0
            return int(
                payload_metadata.st_size + manifest_metadata.st_size + access_bytes
            ), last_access
        except (OSError, ValueError, json.JSONDecodeError, UnicodeError):
            return None

    def _remove_entry(self, entry: Path) -> bool:
        try:
            self._assert_root_identity()
            quarantine = self.root / f".quarantine-{entry.name}-{uuid.uuid4().hex}"
            os.rename(entry, quarantine)
            self._assert_root_identity()
        except (AuditInfrastructureError, OSError):
            return False
        removed = _remove_held_flat_directory(
            quarantine,
            after_hold=_after_cleanup_quarantine,
        )
        if removed:
            self._remove_sidecar(entry.name)
        return removed

    def _remove_sidecar(self, key: str) -> bool:
        access = self._access_path(key)
        quarantine = self.root / f".quarantine-access-{key}-{uuid.uuid4().hex}"
        try:
            self._assert_root_identity()
            os.rename(access, quarantine)
            self._assert_root_identity()
            metadata = quarantine.lstat()
            if stat.S_ISDIR(metadata.st_mode) and not _is_link(metadata):
                return False
            quarantine.unlink()
            return True
        except FileNotFoundError:
            return True
        except (AuditInfrastructureError, OSError):
            return False

    def cleanup(self, now: float) -> None:
        if not isinstance(now, (int, float)) or isinstance(now, bool):
            raise AuditInfrastructureError("cache cleanup time is invalid")
        try:
            self._assert_root_identity()
            entries = list(os.scandir(self.root))
        except OSError as error:
            raise AuditInfrastructureError("cache cleanup cannot inspect its root") from error

        complete: list[tuple[float, int, Path]] = []
        sidecar_keys: set[str] = set()
        for raw_entry in entries:
            if raw_entry.name.startswith(".access-"):
                sidecar_keys.add(raw_entry.name.removeprefix(".access-"))
                continue
            path = Path(raw_entry.path)
            try:
                metadata = raw_entry.stat(follow_symlinks=False)
            except OSError:
                continue
            if _is_link(metadata) or not stat.S_ISDIR(metadata.st_mode):
                continue
            info = self._entry_info(path)
            if info is None:
                if now - metadata.st_mtime > _INCOMPLETE_SECONDS:
                    if path.name.startswith(".tmp-") and _temporary_publication_is_active(path):
                        continue
                    if len(path.name) == 64 and all(
                        character in "0123456789abcdef" for character in path.name
                    ):
                        self._remove_entry(path)
                    else:
                        quarantine = self.root / f".quarantine-incomplete-{uuid.uuid4().hex}"
                        try:
                            os.rename(path, quarantine)
                            _remove_held_flat_directory(quarantine)
                        except OSError:
                            pass
                continue
            size, last_access = info
            if now - last_access > _COMPLETE_SECONDS:
                self._remove_entry(path)
                continue
            complete.append((last_access, size, path))

        live_keys = {path.name for _, _, path in complete if path.exists()}
        for key in sidecar_keys - live_keys:
            self._remove_sidecar(key)

        total_size = sum(size for _, size, _ in complete)
        total_entries = len(complete)
        for _last_access, size, path in sorted(complete):
            if total_size <= self.maximum_bytes and total_entries <= self.maximum_entries:
                break
            if self._remove_entry(path):
                total_size -= size
                total_entries -= 1
