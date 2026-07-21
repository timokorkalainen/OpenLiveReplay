"""Dependency-validated atomic cache for compiler-authoritative GPU views."""

from __future__ import annotations

import hashlib
import base64
import contextlib
import json
import math
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
    AUDIT_RESULT_SCHEMA_BYTES,
    AuditResultFinding,
    AuditInfrastructureError,
    AuditLimits,
    CompactResultColdSlot,
    CompactResultDraftBounds,
    CompactResultTransportCapability,
    CompactResultTransportReceipt,
    CompactResultReservationOwnership,
    CompactResultMemoryBudget,
    CompactResultOwnership,
    ConfigurationAuditPublicationPermit,
    ConfigurationAuditResult,
    ConfigurationAuditResultTransport,
    ConfigurationAuditTransportOutcome,
    CompactTokenSequence,
    CompilerFamily,
    CompilerInspection,
    DependencyRootAuthority,
    DependencyDigest,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    PerTaskCompactReservation,
    StreamingResultAggregator,
    WorkerStageTimings,
    compact_result_retained_bytes,
    decode_local_dependency_digest,
    encode_local_dependency_digest,
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
AUDIT_CACHE_SCHEMA_BYTES = b"olr-gpu-capability-audit-cache-v1"
_AUDIT_CACHE_MAXIMUM_BYTES = 472 * 1024 * 1024
_AUDIT_CACHE_MAXIMUM_ENTRIES = 1024
_INSPECTION_MAX_TOTAL_BYTES = 32 * 1024 * 1024
_CACHE_ROOT_OVERHEAD_RESERVE = 8 * 1024 * 1024
_AUDIT_RESULT_MAXIMUM_ENCODED_BYTES = 4 * 1024 * 1024
_AUDIT_TRANSPORT_HEADER_FORMAT = "<8s32s32s32sQQ32sI32sIQ32sQdddd"
_AUDIT_TRANSPORT_HEADER_BYTES = struct.calcsize(_AUDIT_TRANSPORT_HEADER_FORMAT)
_AUDIT_TRANSPORT_MAGIC = b"OLRATR01"
_AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES = 16 * 1024
_AUDIT_RESULTS_DIRECTORY = "results"
_AUDIT_ROOT_MARKER = ".audit-result-cache-v1"
_REPARSE_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
_LOCK_NAMESPACE_MAGIC = b"OLRLN001"
_LOCK_NAMESPACE_LANE_COUNT = 8
_LOCK_NAMESPACE_HEADER_FORMAT = "<8sII"
_LOCK_NAMESPACE_HEADER_BYTES = struct.calcsize(_LOCK_NAMESPACE_HEADER_FORMAT)
_LOCK_NAMESPACE_IDENTITY_FORMAT = "<QQ"
_LOCK_NAMESPACE_IDENTITY_BYTES = struct.calcsize(_LOCK_NAMESPACE_IDENTITY_FORMAT)
_LOCK_NAMESPACE_RECORD_BYTES = _LOCK_NAMESPACE_IDENTITY_BYTES + hashlib.sha256().digest_size
_LOCK_NAMESPACE_SLOT_BYTES = _LOCK_NAMESPACE_RECORD_BYTES * 2
_LOCK_NAMESPACE_BYTES = (
    1
    + _LOCK_NAMESPACE_HEADER_BYTES
    + _LOCK_NAMESPACE_LANE_COUNT * _LOCK_NAMESPACE_SLOT_BYTES
)
# The ledger is the lane-generation authority while the pinned cache root and
# namespace-carrier generation remain stable.  A filesystem-only scheme cannot
# discover an open POSIX inode after an uncooperative process replaces every
# name for both that inode and its authority; callers therefore fail closed on
# authority replacement rather than claiming protection across that event.
_hash_cache: dict[tuple[object, ...], str] = {}
_hash_lock = threading.Lock()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditInfrastructureError(message)


_require(
    _AUDIT_CACHE_MAXIMUM_BYTES
    + _INSPECTION_MAX_TOTAL_BYTES
    + _CACHE_ROOT_OVERHEAD_RESERVE
    == 512 * 1024 * 1024,
    "cache partition total is not 512 MiB",
)


class _UnsafeCacheNamespaceError(OSError):
    """A cache pathname is linked, replaced, or otherwise not uniquely owned."""


def _lock_carrier_anchor_path(path: Path) -> Path:
    return path.with_name(f"{path.name}.anchor")


def _before_lock_carrier_temporary_quarantine(_path: Path) -> None:
    """Test seam immediately before atomic lock-temporary quarantine."""


def _after_lock_carrier_temporary_quarantine(
    _path: Path, _quarantine: Path
) -> None:
    """Test seam after quarantine has detached cleanup from the public name."""


def _before_windows_lock_carrier_temporary_delete(_path: Path) -> None:
    """Test seam after exact Windows quarantine-handle verification."""


def _rename_lock_carrier_temporary_to_quarantine(
    source: Path, quarantine: Path
) -> None:
    """Atomically move to an unused private name without replacing a collision."""
    if os.name == "nt":
        os.rename(source, quarantine)
        return
    import ctypes
    import errno

    library = ctypes.CDLL(None, use_errno=True)
    source_bytes = os.fsencode(source)
    quarantine_bytes = os.fsencode(quarantine)
    if sys.platform.startswith("linux"):
        renameat2 = getattr(library, "renameat2", None)
        if renameat2 is None:
            raise OSError(
                errno.ENOTSUP,
                "atomic no-replace quarantine is unavailable",
                str(source),
            )
        renameat2.argtypes = (
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        )
        renameat2.restype = ctypes.c_int
        result = renameat2(
            -100,
            source_bytes,
            -100,
            quarantine_bytes,
            1,
        )
    elif sys.platform == "darwin":
        renamex_np = getattr(library, "renamex_np", None)
        if renamex_np is None:
            raise OSError(
                errno.ENOTSUP,
                "atomic no-replace quarantine is unavailable",
                str(source),
            )
        renamex_np.argtypes = (
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint,
        )
        renamex_np.restype = ctypes.c_int
        result = renamex_np(source_bytes, quarantine_bytes, 0x00000004)
    else:
        raise OSError(
            errno.ENOTSUP,
            "atomic no-replace quarantine is unavailable",
            str(source),
        )
    if result != 0:
        error_number = ctypes.get_errno()
        raise OSError(
            error_number,
            os.strerror(error_number),
            str(quarantine),
        )


def _delete_verified_windows_lock_carrier_temporary(
    path: Path,
    expected_identity: tuple[int, int],
    original_error: BaseException,
) -> None:
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

    class FileId128(ctypes.Structure):
        _fields_ = (("identifier", ctypes.c_ubyte * 16),)

    class FileIdInformation(ctypes.Structure):
        _fields_ = (
            ("volume", ctypes.c_ulonglong),
            ("file_id", FileId128),
        )

    class FileDispositionInformation(ctypes.Structure):
        _fields_ = (("delete_file", wintypes.BOOLEAN),)

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
    kernel32.GetFileInformationByHandleEx.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
    )
    kernel32.GetFileInformationByHandleEx.restype = wintypes.BOOL
    kernel32.SetFileInformationByHandle.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
    )
    kernel32.SetFileInformationByHandle.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL

    delete_access = 0x00010000
    file_read_attributes = 0x00000080
    file_share_read = 0x00000001
    file_share_write = 0x00000002
    file_share_delete = 0x00000004
    open_existing = 3
    file_flag_open_reparse_point = 0x00200000
    file_attribute_directory = 0x00000010
    file_disposition_info = 4
    file_id_info = 18
    handle = kernel32.CreateFileW(
        str(path),
        delete_access | file_read_attributes,
        file_share_read | file_share_write | file_share_delete,
        None,
        open_existing,
        file_flag_open_reparse_point,
        None,
    )
    invalid_handle = ctypes.c_void_p(-1).value
    if handle in (None, invalid_handle):
        error_number = ctypes.get_last_error()
        raise AuditInfrastructureError(
            "cache lock carrier quarantine changed"
        ) from OSError(
            error_number,
            "cannot open cache lock carrier quarantine",
            str(path),
        )

    primary_error: BaseException | None = None
    try:
        information = FileInformation()
        if not kernel32.GetFileInformationByHandle(
            handle, ctypes.byref(information)
        ):
            error_number = ctypes.get_last_error()
            raise AuditInfrastructureError(
                "cannot inspect cache lock carrier quarantine handle"
            ) from OSError(
                error_number,
                "cannot query cache lock carrier quarantine handle",
                str(path),
            )

        opened_device = int(information.volume)
        opened_inode = (
            int(information.index_high) << 32
        ) | int(information.index_low)
        # CPython 3.12+ exposes the 128-bit Windows file ID through st_ino.
        # A successful zero ID retains the legacy index but uses the FileIdInfo
        # volume; 3.12.0 treats query failure as fatal, while 3.12.1+ falls
        # back to the legacy pair.  Python 3.11 never issues the query.
        python_version = tuple(sys.version_info[:3])
        if python_version >= (3, 12):
            identity = FileIdInformation()
            if kernel32.GetFileInformationByHandleEx(
                handle,
                file_id_info,
                ctypes.byref(identity),
                ctypes.sizeof(identity),
            ):
                opened_device = int(identity.volume)
                file_id = int.from_bytes(
                    bytes(identity.file_id.identifier), "little"
                )
                if file_id != 0:
                    opened_inode = file_id
            elif python_version == (3, 12, 0):
                error_number = ctypes.get_last_error()
                raise AuditInfrastructureError(
                    "cannot inspect cache lock carrier quarantine file ID"
                ) from OSError(
                    error_number,
                    "cannot query cache lock carrier quarantine file ID",
                    str(path),
                )

        try:
            metadata = path.lstat()
        except OSError as cleanup_error:
            raise AuditInfrastructureError(
                "cache lock carrier quarantine changed"
            ) from cleanup_error
        if (
            _is_link(metadata)
            or not stat.S_ISREG(metadata.st_mode)
            or bool(information.attributes & _REPARSE_ATTRIBUTE)
            or bool(information.attributes & file_attribute_directory)
            or int(getattr(metadata, "st_nlink", 1)) != 1
            or int(information.links) != 1
            or _file_ownership_identity(metadata) != expected_identity
            or (opened_device, opened_inode) != expected_identity
        ):
            raise AuditInfrastructureError(
                "cache lock carrier temporary was replaced; replacement preserved "
                f"at {path}"
            ) from original_error
        _before_windows_lock_carrier_temporary_delete(path)
        disposition = FileDispositionInformation(True)
        if not kernel32.SetFileInformationByHandle(
            handle,
            file_disposition_info,
            ctypes.byref(disposition),
            ctypes.sizeof(disposition),
        ):
            error_number = ctypes.get_last_error()
            raise AuditInfrastructureError(
                "cannot remove cache lock carrier temporary after "
                f"{type(original_error).__name__}: {original_error}"
            ) from OSError(
                error_number,
                "cannot mark cache lock carrier quarantine for deletion",
                str(path),
            )
    except BaseException as error:
        primary_error = error
        error = None
        raise
    finally:
        preserved_primary = primary_error
        primary_error = None
        close_error = None
        close_failure = None
        diagnostic_note = None
        try:
            if not kernel32.CloseHandle(handle):
                close_error = OSError(
                    ctypes.get_last_error(),
                    "cannot close cache lock carrier quarantine handle",
                    str(path),
                )
                close_failure = AuditInfrastructureError(
                    "cannot close cache lock carrier quarantine handle"
                )
                close_failure.__cause__ = close_error
                close_failure.__suppress_context__ = True
                if preserved_primary is None:
                    try:
                        raise close_failure
                    finally:
                        close_failure = None
                # Arbitrary exception classes can dispatch data descriptors
                # even through unbound BaseException operations.  Attach
                # diagnostics only to the exact, known-safe audit error type.
                if (
                    type(preserved_primary) is AuditInfrastructureError
                    and AuditInfrastructureError.__setattr__
                    is BaseException.__setattr__
                    and AuditInfrastructureError.add_note
                    is BaseException.add_note
                    and "secondary_close_error"
                    not in AuditInfrastructureError.__dict__
                    and "__notes__" not in AuditInfrastructureError.__dict__
                ):
                    try:
                        BaseException.__setattr__(
                            preserved_primary,
                            "secondary_close_error",
                            close_failure,
                        )
                    except BaseException:
                        pass
                    diagnostic_note = (
                        "Secondary CloseHandle failure: "
                        f"{type(close_error).__name__}: {close_error}"
                    )
                    try:
                        BaseException.add_note(
                            preserved_primary, diagnostic_note
                        )
                    except BaseException:
                        pass
        finally:
            preserved_primary = None
            close_error = None
            close_failure = None
            diagnostic_note = None
            original_error = None


def _cleanup_owned_lock_carrier_temporary(
    path: Path,
    expected_identity: tuple[int, int] | None,
    original_error: BaseException,
) -> None:
    if expected_identity is None:
        return
    quarantine = path.with_name(f".quarantine-lock-{uuid.uuid4().hex}")
    _before_lock_carrier_temporary_quarantine(path)
    try:
        _rename_lock_carrier_temporary_to_quarantine(path, quarantine)
    except FileNotFoundError:
        return
    except OSError as cleanup_error:
        raise AuditInfrastructureError(
            "cannot quarantine cache lock carrier temporary after "
            f"{type(original_error).__name__}: {original_error}"
        ) from cleanup_error
    _after_lock_carrier_temporary_quarantine(path, quarantine)
    if os.name == "nt":
        _delete_verified_windows_lock_carrier_temporary(
            quarantine, expected_identity, original_error
        )
        return
    try:
        stream = quarantine.open("r+b")
    except OSError as cleanup_error:
        raise AuditInfrastructureError(
            "cache lock carrier quarantine changed"
        ) from cleanup_error
    with stream:
        try:
            metadata = quarantine.lstat()
            opened = os.fstat(stream.fileno())
        except OSError as cleanup_error:
            raise AuditInfrastructureError(
                "cache lock carrier quarantine changed"
            ) from cleanup_error
        if (
            _is_link(metadata)
            or not stat.S_ISREG(metadata.st_mode)
            or not stat.S_ISREG(opened.st_mode)
            or int(getattr(metadata, "st_nlink", 1)) != 1
            or int(getattr(opened, "st_nlink", 1)) != 1
            or _file_ownership_identity(metadata) != expected_identity
            or _file_ownership_identity(opened) != expected_identity
        ):
            raise AuditInfrastructureError(
                "cache lock carrier temporary was replaced; replacement preserved "
                f"at {quarantine}"
            ) from original_error
        # POSIX has no portable unlink-by-file-descriptor operation.  The final
        # path unlink is scoped to this unexposed 128-bit random private name;
        # an observed replacement is preserved above, but we do not claim to
        # defeat a same-user process that guesses and races that exact name.
        try:
            quarantine.unlink()
            final = os.fstat(stream.fileno())
        except OSError as cleanup_error:
            raise AuditInfrastructureError(
                "cannot remove cache lock carrier temporary after "
                f"{type(original_error).__name__}: {original_error}"
            ) from cleanup_error
        if (
            _file_ownership_identity(final) != expected_identity
            or int(getattr(final, "st_nlink", 0)) != 0
        ):
            raise AuditInfrastructureError(
                "cache lock carrier quarantine changed while removing it"
            ) from original_error


def _initialize_lock_carrier_anchor(anchor: Path) -> None:
    temporary = anchor.with_name(f".tmp-lock-{uuid.uuid4().hex}")
    temporary_identity: tuple[int, int] | None = None
    try:
        with temporary.open("x+b") as stream:
            opened = os.fstat(stream.fileno())
            opened_identity = _file_ownership_identity(opened)
            temporary_identity = opened_identity
            visible = temporary.lstat()
            if (
                opened_identity is None
                or _is_link(visible)
                or not stat.S_ISREG(opened.st_mode)
                or not stat.S_ISREG(visible.st_mode)
                or int(getattr(opened, "st_nlink", 1)) != 1
                or int(getattr(visible, "st_nlink", 1)) != 1
                or _file_ownership_identity(visible) != opened_identity
            ):
                raise OSError("cache lock carrier temporary ownership is unsafe")
            if stream.write(b"1") != 1:
                raise OSError("cache lock carrier temporary write was incomplete")
            stream.flush()
            os.fsync(stream.fileno())
        try:
            if os.name == "nt":
                os.rename(temporary, anchor)
            else:
                os.link(temporary, anchor)
                temporary.unlink()
        except FileExistsError as error:
            _cleanup_owned_lock_carrier_temporary(
                temporary, temporary_identity, error
            )
    except BaseException as error:
        _cleanup_owned_lock_carrier_temporary(
            temporary, temporary_identity, error
        )
        raise


def _open_lock_carrier_pair(
    path: Path,
    allowed_sizes: tuple[int, ...] = (1,),
):
    anchor = _lock_carrier_anchor_path(path)
    try:
        anchor.lstat()
    except FileNotFoundError:
        try:
            existing = _regular_unlinked_file(path)
        except FileNotFoundError:
            _initialize_lock_carrier_anchor(anchor)
        else:
            if int(existing.st_size) not in allowed_sizes:
                raise OSError("cache lock carrier is uninitialized")
            try:
                os.link(path, anchor)
            except FileExistsError:
                pass
    anchor_metadata = anchor.lstat()
    if (
        _is_link(anchor_metadata)
        or not stat.S_ISREG(anchor_metadata.st_mode)
        or int(anchor_metadata.st_size) not in allowed_sizes
        or int(getattr(anchor_metadata, "st_nlink", 0)) not in (1, 2)
    ):
        raise OSError("cache lock carrier anchor is unsafe")
    try:
        path.lstat()
    except FileNotFoundError:
        if int(getattr(anchor_metadata, "st_nlink", 0)) != 1:
            raise OSError("cache lock carrier anchor has an unexpected alias")
        try:
            os.link(anchor, path, follow_symlinks=False)
        except FileExistsError:
            pass

    anchor_stream = None
    stream = None
    try:
        anchor_stream = anchor.open("r+b")
        stream = path.open("r+b")
        _SharedCacheFileLock._assert_carrier_at(
            path, stream, anchor_stream, allowed_sizes
        )
        return stream, anchor_stream
    except BaseException as error:
        if stream is not None:
            stream.close()
        if anchor_stream is not None:
            anchor_stream.close()
        raise


def _read_lock_namespace_bytes(stream, offset: int, count: int) -> bytes:
    stream.seek(offset)
    value = stream.read(count)
    if len(value) != count:
        raise OSError("cache lock namespace ledger is incomplete")
    return value


def _lock_namespace_record(lane: int, identity: tuple[int, int]) -> bytes:
    device, inode = identity
    if not (0 <= device < 2**64 and 0 < inode < 2**64):
        raise OSError("cache lock lane identity is not representable")
    value = struct.pack(_LOCK_NAMESPACE_IDENTITY_FORMAT, device, inode)
    digest = hashlib.sha256(
        b"OpenLiveReplay cache lock lane generation\0"
        + lane.to_bytes(4, "little")
        + value
    ).digest()
    return value + digest


def _decode_lock_namespace_record(lane: int, value: bytes) -> tuple[int, int]:
    if len(value) != _LOCK_NAMESPACE_RECORD_BYTES:
        raise OSError("cache lock namespace generation record is incomplete")
    identity_bytes = value[:_LOCK_NAMESPACE_IDENTITY_BYTES]
    identity = struct.unpack(_LOCK_NAMESPACE_IDENTITY_FORMAT, identity_bytes)
    if value != _lock_namespace_record(lane, identity):
        raise OSError("cache lock namespace generation record is invalid")
    return identity


def _lock_namespace_lane_index(path: Path) -> int:
    _prefix, separator, suffix = path.name.rpartition("-")
    if not separator or not suffix.endswith(".lock"):
        raise OSError("cache lock lane name is invalid")
    digits = suffix[:-len(".lock")]
    if not digits.isascii() or not digits.isdecimal():
        raise OSError("cache lock lane name is invalid")
    lane = int(digits)
    if not 0 <= lane < _LOCK_NAMESPACE_LANE_COUNT:
        raise OSError("cache lock lane is outside the namespace ledger")
    return lane


def _validate_lock_namespace_ledger(stream) -> None:
    header = _read_lock_namespace_bytes(
        stream, 1, _LOCK_NAMESPACE_HEADER_BYTES
    )
    if header != struct.pack(
        _LOCK_NAMESPACE_HEADER_FORMAT,
        _LOCK_NAMESPACE_MAGIC,
        _LOCK_NAMESPACE_LANE_COUNT,
        _LOCK_NAMESPACE_RECORD_BYTES,
    ):
        raise OSError("cache lock namespace ledger header is invalid")
    empty = bytes(_LOCK_NAMESPACE_RECORD_BYTES)
    for lane in range(_LOCK_NAMESPACE_LANE_COUNT):
        offset = (
            1
            + _LOCK_NAMESPACE_HEADER_BYTES
            + lane * _LOCK_NAMESPACE_SLOT_BYTES
        )
        slot = _read_lock_namespace_bytes(
            stream, offset, _LOCK_NAMESPACE_SLOT_BYTES
        )
        first = slot[:_LOCK_NAMESPACE_RECORD_BYTES]
        second = slot[_LOCK_NAMESPACE_RECORD_BYTES:]
        if first == empty and second == empty:
            continue
        if first != second:
            raise OSError("cache lock namespace generation commit is incomplete")
        _decode_lock_namespace_record(lane, first)


def _ensure_lock_namespace_ledger(path: Path, stream, anchor_stream) -> None:
    opened = os.fstat(stream.fileno())
    size = int(opened.st_size)
    if size == 1:
        body = struct.pack(
            _LOCK_NAMESPACE_HEADER_FORMAT,
            _LOCK_NAMESPACE_MAGIC,
            _LOCK_NAMESPACE_LANE_COUNT,
            _LOCK_NAMESPACE_RECORD_BYTES,
        ) + bytes(_LOCK_NAMESPACE_LANE_COUNT * _LOCK_NAMESPACE_SLOT_BYTES)
        stream.seek(1)
        if stream.write(body) != len(body):
            raise OSError("cache lock namespace ledger upgrade was incomplete")
        stream.flush()
        os.fsync(stream.fileno())
    elif size != _LOCK_NAMESPACE_BYTES:
        raise OSError("cache lock namespace ledger size is invalid")
    _SharedCacheFileLock._assert_carrier_at(
        path, stream, anchor_stream, (_LOCK_NAMESPACE_BYTES,)
    )
    _validate_lock_namespace_ledger(stream)


def _bind_lock_lane_generation(namespace_stream, lane_path: Path, lane_stream) -> None:
    lane = _lock_namespace_lane_index(lane_path)
    identity = _file_ownership_identity(os.fstat(lane_stream.fileno()))
    if identity is None:
        raise OSError("cache lock lane identity is unavailable")
    offset = (
        1
        + _LOCK_NAMESPACE_HEADER_BYTES
        + lane * _LOCK_NAMESPACE_SLOT_BYTES
    )
    slot = _read_lock_namespace_bytes(
        namespace_stream, offset, _LOCK_NAMESPACE_SLOT_BYTES
    )
    empty = bytes(_LOCK_NAMESPACE_RECORD_BYTES)
    first = slot[:_LOCK_NAMESPACE_RECORD_BYTES]
    second = slot[_LOCK_NAMESPACE_RECORD_BYTES:]
    if first == empty and second == empty:
        record = _lock_namespace_record(lane, identity)
        namespace_stream.seek(offset)
        committed = record + record
        if namespace_stream.write(committed) != len(committed):
            raise OSError("cache lock lane generation commit was incomplete")
        namespace_stream.flush()
        os.fsync(namespace_stream.fileno())
        slot = _read_lock_namespace_bytes(
            namespace_stream, offset, _LOCK_NAMESPACE_SLOT_BYTES
        )
        first = slot[:_LOCK_NAMESPACE_RECORD_BYTES]
        second = slot[_LOCK_NAMESPACE_RECORD_BYTES:]
    if first != second:
        raise OSError("cache lock lane generation commit is incomplete")
    recorded = _decode_lock_namespace_record(lane, first)
    if recorded != identity:
        raise OSError("cache lock lane generation changed")


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
        self.temporary_identity: tuple[int, int] | None = None

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
        self.temporary_identity = _file_ownership_identity(
            os.fstat(self.stream.fileno())
        )
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
            _cleanup_owned_temporary_file(
                self.path,
                self.temporary_identity,
                "cache publication guard temporary",
                error,
            )
            raise

    def __exit__(self, _type, _value, _traceback) -> None:
        _require(self.stream is not None, "publication guard stream is unavailable")
        try:
            self._unlock(self.stream)
        finally:
            self.stream.close()
            self.stream = None


class _SharedCacheFileLock:
    """Deadline-bounded lock over a carrier with a persistent hard-link anchor."""

    def __init__(
        self,
        path: Path,
        root: Path,
        root_identity: tuple[int, int | None],
        deadline: float,
        cancel_event: object | None = None,
        namespace_path: Path | None = None,
    ) -> None:
        self.path = path
        self.root = root
        self.root_identity = root_identity
        self.deadline = deadline
        self.cancel_event = cancel_event
        self.namespace_path = namespace_path
        self.stream = None
        self.anchor_stream = None
        self.namespace_stream = None
        self.namespace_anchor_stream = None
        self._namespace_locked = False
        self._root_fd: int | None = None

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

    def _assert_carrier(self, stream) -> None:
        _require(self.anchor_stream is not None, "lock carrier anchor is unavailable")
        allowed_sizes = (
            (1, _LOCK_NAMESPACE_BYTES)
            if self.namespace_path is None
            else (1,)
        )
        self._assert_carrier_at(
            self.path, stream, self.anchor_stream, allowed_sizes
        )

    @staticmethod
    def _assert_carrier_at(
        path: Path,
        stream,
        anchor_stream,
        allowed_sizes: tuple[int, ...] = (1,),
    ) -> None:
        anchor_path = _lock_carrier_anchor_path(path)
        carrier = path.lstat()
        anchor = anchor_path.lstat()
        opened = os.fstat(stream.fileno())
        opened_anchor = os.fstat(anchor_stream.fileno())
        metadata = (carrier, anchor, opened, opened_anchor)
        if (
            any(_is_link(value) for value in (carrier, anchor))
            or any(not stat.S_ISREG(value.st_mode) for value in metadata)
            or any(int(value.st_size) not in allowed_sizes for value in metadata)
            or any(int(getattr(value, "st_nlink", 0)) != 2 for value in metadata)
            or len({
                (int(value.st_dev), int(value.st_ino)) for value in metadata
            }) != 1
        ):
            raise OSError("cache lock carrier changed while opening")

    def _acquire_root_anchor(self) -> None:
        if os.name == "nt":
            return
        import fcntl

        flags = (
            os.O_RDONLY
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_NOFOLLOW", 0)
            | getattr(os, "O_CLOEXEC", 0)
        )
        fd = os.open(self.root, flags)
        self._root_fd = fd
        opened = os.fstat(fd)
        if (
            not stat.S_ISDIR(opened.st_mode)
            or _directory_identity(opened) != self.root_identity
        ):
            raise AuditInfrastructureError("cache root was replaced")
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                self._check_budget()
                time.sleep(min(
                    0.01, max(0.0, self.deadline - time.monotonic())
                ))
        self._check_budget()
        self._assert_root()
        if _directory_identity(os.fstat(fd)) != self.root_identity:
            raise AuditInfrastructureError("cache root was replaced")

    def _release_root_anchor(self) -> None:
        if self._root_fd is None:
            return
        fd = self._root_fd
        self._root_fd = None
        try:
            if os.name != "nt":
                import fcntl

                fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)

    def _release_namespace(self) -> None:
        if self.namespace_stream is None:
            if self.namespace_anchor_stream is not None:
                self.namespace_anchor_stream.close()
                self.namespace_anchor_stream = None
            return
        stream = self.namespace_stream
        self.namespace_stream = None
        try:
            if self._namespace_locked:
                _PublicationGuard._unlock(stream)
        finally:
            self._namespace_locked = False
            stream.close()
            if self.namespace_anchor_stream is not None:
                self.namespace_anchor_stream.close()
                self.namespace_anchor_stream = None

    def __enter__(self) -> "_SharedCacheFileLock":
        if (
            not isinstance(self.deadline, (int, float))
            or isinstance(self.deadline, bool)
        ):
            raise AuditInfrastructureError("cache lock deadline is invalid")
        self._check_budget()
        try:
            self._assert_root()
            self._acquire_root_anchor()
            if self.namespace_path is not None and self.namespace_path != self.path:
                namespace_stream, namespace_anchor_stream = _open_lock_carrier_pair(
                    self.namespace_path,
                    (1, _LOCK_NAMESPACE_BYTES),
                )
                self.namespace_stream = namespace_stream
                self.namespace_anchor_stream = namespace_anchor_stream
                self._assert_carrier_at(
                    self.namespace_path,
                    namespace_stream,
                    namespace_anchor_stream,
                    (1, _LOCK_NAMESPACE_BYTES),
                )
                while not _PublicationGuard._lock(
                    namespace_stream, blocking=False
                ):
                    self._check_budget()
                    time.sleep(min(
                        0.01, max(0.0, self.deadline - time.monotonic())
                    ))
                self._namespace_locked = True
                _ensure_lock_namespace_ledger(
                    self.namespace_path,
                    namespace_stream,
                    namespace_anchor_stream,
                )
                self._assert_carrier_at(
                    self.namespace_path,
                    namespace_stream,
                    namespace_anchor_stream,
                    (_LOCK_NAMESPACE_BYTES,),
                )
            carrier_sizes = (
                (1, _LOCK_NAMESPACE_BYTES)
                if self.namespace_path is None
                else (1,)
            )
            stream, anchor_stream = _open_lock_carrier_pair(
                self.path, carrier_sizes
            )
            self.stream = stream
            self.anchor_stream = anchor_stream
            self._assert_carrier(stream)
            if self.namespace_stream is not None:
                _bind_lock_lane_generation(
                    self.namespace_stream,
                    self.path,
                    stream,
                )
            acquired = _PublicationGuard._lock(stream, blocking=False)
            if self.namespace_stream is not None:
                _require(self.namespace_path is not None, "lock namespace path is unavailable")
                _require(
                    self.namespace_anchor_stream is not None,
                    "lock namespace anchor is unavailable",
                )
                self._assert_carrier_at(
                    self.namespace_path,
                    self.namespace_stream,
                    self.namespace_anchor_stream,
                    (_LOCK_NAMESPACE_BYTES,),
                )
            self._assert_root()
            if (
                self._root_fd is not None
                and _directory_identity(os.fstat(self._root_fd))
                != self.root_identity
            ):
                raise AuditInfrastructureError("cache root was replaced")
            self._release_namespace()
            self._release_root_anchor()
            while not acquired:
                self._check_budget()
                time.sleep(min(0.01, max(0.0, self.deadline - time.monotonic())))
                acquired = _PublicationGuard._lock(stream, blocking=False)
            self._assert_carrier(stream)
            self._check_budget()
            self._assert_root()
            self._assert_carrier(stream)
            return self
        except BaseException as error:
            try:
                if self.stream is not None:
                    self.stream.close()
                    self.stream = None
                if self.anchor_stream is not None:
                    self.anchor_stream.close()
                    self.anchor_stream = None
            finally:
                try:
                    self._release_namespace()
                finally:
                    self._release_root_anchor()
            if isinstance(error, AuditInfrastructureError):
                raise
            raise AuditInfrastructureError(
                "cache lock namespace is unsafe"
            ) from error

    def __exit__(self, _type, _value, _traceback) -> None:
        _require(self.stream is not None, "shared cache lock stream is unavailable")
        try:
            _PublicationGuard._unlock(self.stream)
        finally:
            try:
                self.stream.close()
                self.stream = None
                if self.anchor_stream is not None:
                    self.anchor_stream.close()
                    self.anchor_stream = None
            finally:
                try:
                    self._release_namespace()
                finally:
                    self._release_root_anchor()


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
        _require(
            self.stream is not None and self.identity is not None,
            "held cache file is not open",
        )
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
        _require(self.stream is not None, "held cache file stream is unavailable")
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


_CACHE_ENTRY_FILES = frozenset(
    {"active.lock", "manifest.json", "payload.bin", "payload.json"}
)


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

        _require(self._fd is not None, "held directory descriptor is unavailable")
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


def _file_ownership_identity(
    metadata: os.stat_result,
) -> tuple[int, int] | None:
    inode = int(metadata.st_ino)
    if inode == 0:
        return None
    return int(metadata.st_dev), inode


def _before_file_temporary_cleanup(_path: Path) -> None:
    """Test seam for deterministic temporary replacement-race coverage."""


def _cleanup_owned_temporary_file(
    path: Path,
    expected_identity: tuple[int, int] | None,
    label: str,
    original_error: BaseException,
) -> None:
    if expected_identity is None:
        return
    _before_file_temporary_cleanup(path)
    try:
        metadata = _regular_unlinked_file(path)
    except FileNotFoundError:
        return
    except OSError as cleanup_error:
        raise AuditInfrastructureError(f"{label} was replaced") from cleanup_error
    if _file_ownership_identity(metadata) != expected_identity:
        raise AuditInfrastructureError(f"{label} was replaced") from original_error
    try:
        path.unlink()
    except OSError as cleanup_error:
        raise AuditInfrastructureError(
            f"cannot remove {label} after "
            f"{type(original_error).__name__}: {original_error}"
        ) from cleanup_error


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
            self.root / ".compiler-inspection-root.lock",
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
                _require(held.stream is not None, "held inspection stream is unavailable")
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
        with self._key_lock(
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


def audit_cache_key(configuration_digest: str, engine: str) -> str:
    for label, value in (
        ("configuration", configuration_digest),
        ("audit engine", engine),
    ):
        if (
            not isinstance(value, str)
            or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
        ):
            raise AuditInfrastructureError(f"{label} digest is invalid")
    digest = hashlib.sha256()
    for value in (
        AUDIT_CACHE_SCHEMA_BYTES,
        configuration_digest.encode("ascii"),
        engine.encode("ascii"),
    ):
        digest.update(struct.pack("<Q", len(value)))
        digest.update(value)
    return digest.hexdigest()


def _strict_json_document(payload: bytes) -> dict[str, object]:
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON object key")
            result[key] = value
        return result

    document = json.loads(
        payload.decode("ascii"),
        object_pairs_hook=unique_object,
        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
    )
    if not isinstance(document, dict):
        raise ValueError("audit result document is not an object")
    return document


def _encode_audit_result_payload(result: ConfigurationAuditResult) -> bytes:
    if not isinstance(result, ConfigurationAuditResult):
        raise AuditInfrastructureError("compact audit result is invalid")
    compact_result_retained_bytes(result)
    dependencies = [
        json.loads(encode_local_dependency_digest(item).decode("ascii"))
        for item in result.dependencies
    ]
    document = {
        "schema": AUDIT_RESULT_SCHEMA_BYTES.decode("ascii"),
        "configuration_digest": result.configuration_digest,
        "audit_engine_fingerprint": result.audit_engine_fingerprint,
        "dependencies": dependencies,
        "reached_production": [
            path.as_posix() for path in result.reached_production
        ],
        "findings": [
            {
                "path": finding.path.as_posix(),
                "line": finding.line,
                "expression": finding.expression,
                "reason": finding.reason,
            }
            for finding in result.findings
        ],
    }
    payload = json.dumps(
        document, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    if len(payload) > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES:
        raise AuditInfrastructureError("encoded compact audit result limit exceeded")
    return payload


def _decode_audit_result_payload(
    payload: bytes,
    *,
    configuration_digest: str,
    engine: str,
) -> ConfigurationAuditResult:
    if (
        not isinstance(payload, bytes)
        or len(payload) > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
    ):
        raise ValueError("encoded compact audit result limit exceeded")
    cursor = _CanonicalAuditResultCursor(payload)
    limits = AuditLimits()
    cursor.expect(b'{"audit_engine_fingerprint":')
    if cursor.string() != engine:
        raise ValueError("compact audit result engine differs")
    cursor.expect(b',"configuration_digest":')
    if cursor.string() != configuration_digest:
        raise ValueError("compact audit result configuration differs")
    cursor.expect(b',"dependencies":[')
    dependencies: list[DependencyDigest] = []
    previous_dependency_key: tuple[str, str, str] | None = None
    while cursor.payload[cursor.offset:cursor.offset + 1] != b"]":
        if dependencies:
            cursor.expect(b",")
        if len(dependencies) >= limits.compact_result_dependencies:
            raise AuditInfrastructureError(
                "audit result limit exceeded for dependencies"
            )
        cursor.expect(b'{"canonical":')
        canonical = cursor.string(limits.compact_result_path_bytes)
        cursor.expect(b',"device":')
        device = cursor.integer_or_null()
        cursor.expect(b',"inode":')
        inode = cursor.integer_or_null()
        cursor.expect(b',"line_count":')
        line_count = cursor.integer_or_null()
        cursor.expect(b',"production":')
        production = cursor.boolean()
        cursor.expect(b',"relative":')
        if cursor.payload[cursor.offset:cursor.offset + 4] == b"null":
            cursor.offset += 4
            relative = None
        else:
            relative = _bounded_relative_path(
                cursor.string(limits.compact_result_path_bytes),
                limits.compact_result_path_bytes,
            )
        cursor.expect(b',"role_relative_path":')
        role_relative = _bounded_relative_path(
            cursor.string(limits.compact_result_path_bytes),
            limits.compact_result_path_bytes,
        )
        cursor.expect(b',"sha256":')
        sha256 = cursor.string()
        cursor.expect(b',"stable_role":')
        stable_role = cursor.string()
        cursor.expect(b"}")
        dependency = DependencyDigest(
            stable_role,
            role_relative,
            FileIdentity(
                Path(canonical), relative, device, inode, line_count, production
            ),
            sha256,
        )
        dependency_key = (
            dependency.stable_role,
            dependency.role_relative_path.as_posix(),
            dependency.sha256,
        )
        if (
            previous_dependency_key is not None
            and dependency_key <= previous_dependency_key
        ):
            raise AuditInfrastructureError(
                "audit result dependencies are not unique and sorted"
            )
        previous_dependency_key = dependency_key
        dependencies.append(dependency)
    cursor.expect(b"]")
    cursor.expect(b',"findings":[')
    findings: list[AuditResultFinding] = []
    previous_finding_key: tuple[str, int, str, str] | None = None
    while cursor.payload[cursor.offset:cursor.offset + 1] != b"]":
        if findings:
            cursor.expect(b",")
        if len(findings) >= limits.compact_result_findings:
            raise AuditInfrastructureError(
                "audit result limit exceeded for findings"
            )
        cursor.expect(b'{"expression":')
        expression = cursor.string(limits.compact_result_expression_bytes)
        cursor.expect(b',"line":')
        line = cursor.integer_or_null()
        cursor.expect(b',"path":')
        path = _bounded_relative_path(
            cursor.string(limits.compact_result_path_bytes),
            limits.compact_result_path_bytes,
        )
        cursor.expect(b',"reason":')
        reason = cursor.string(limits.compact_result_reason_bytes)
        cursor.expect(b"}")
        finding = AuditResultFinding(path, line, expression, reason)
        finding_key = (path.as_posix(), finding.line, expression, reason)
        if previous_finding_key is not None and finding_key <= previous_finding_key:
            raise AuditInfrastructureError(
                "audit result findings are not unique and sorted"
            )
        previous_finding_key = finding_key
        findings.append(finding)
    cursor.expect(b"]")
    cursor.expect(b',"reached_production":[')
    reached: list[PurePosixPath] = []
    previous_reached: str | None = None
    while cursor.payload[cursor.offset:cursor.offset + 1] != b"]":
        if reached:
            cursor.expect(b",")
        if len(reached) >= limits.compact_result_reached:
            raise AuditInfrastructureError(
                "audit result limit exceeded for reached paths"
            )
        reached_path = _bounded_relative_path(
            cursor.string(limits.compact_result_path_bytes),
            limits.compact_result_path_bytes,
        )
        reached_key = reached_path.as_posix()
        if previous_reached is not None and reached_key <= previous_reached:
            raise AuditInfrastructureError(
                "audit result reached-production paths are not unique and sorted"
            )
        previous_reached = reached_key
        reached.append(reached_path)
    cursor.expect(b"]")
    cursor.expect(b',"schema":')
    if cursor.string() != AUDIT_RESULT_SCHEMA_BYTES.decode("ascii"):
        raise ValueError("compact audit result schema differs")
    cursor.expect(b"}")
    if cursor.offset != len(payload):
        raise ValueError("compact audit result has trailing data")
    return ConfigurationAuditResult(
        configuration_digest,
        engine,
        tuple(dependencies),
        tuple(reached),
        tuple(findings),
    )


class _CanonicalAuditResultCursor:
    """Fixed-grammar cursor used to preparse compact metadata without a JSON DOM."""

    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0

    def expect(self, token: bytes) -> None:
        end = self.offset + len(token)
        if self.payload[self.offset:end] != token:
            raise ValueError("canonical compact audit result syntax differs")
        self.offset = end

    def string(self, maximum_bytes: int | None = None) -> str:
        start = self.offset
        if start >= len(self.payload) or self.payload[start] != 0x22:
            raise ValueError("compact audit result string is invalid")
        index = self.payload.find(b'"', start + 1)
        while index >= 0:
            backslashes = 0
            preceding = index - 1
            while preceding > start and self.payload[preceding] == 0x5C:
                backslashes += 1
                preceding -= 1
            if backslashes % 2 == 0:
                index += 1
                break
            index = self.payload.find(b'"', index + 1)
        if index < 0:
            raise ValueError("compact audit result string is truncated")
        token = self.payload[start:index]
        decoded = json.loads(token.decode("ascii"))
        if (
            not isinstance(decoded, str)
            or json.dumps(decoded, ensure_ascii=True, separators=(",", ":")).encode(
                "ascii"
            )
            != token
        ):
            raise ValueError("compact audit result string is noncanonical")
        if maximum_bytes is not None and len(decoded.encode("utf-8")) > maximum_bytes:
            raise AuditInfrastructureError("audit result limit exceeded for text")
        self.offset = index
        return decoded

    def integer_or_null(self) -> int | None:
        if self.payload[self.offset:self.offset + 4] == b"null":
            self.offset += 4
            return None
        start = self.offset
        if self.offset < len(self.payload) and self.payload[self.offset] == 0x2D:
            self.offset += 1
        if self.offset >= len(self.payload):
            raise ValueError("compact audit result integer is truncated")
        if self.payload[self.offset] == 0x30:
            self.offset += 1
        elif 0x31 <= self.payload[self.offset] <= 0x39:
            while (
                self.offset < len(self.payload)
                and 0x30 <= self.payload[self.offset] <= 0x39
            ):
                self.offset += 1
        else:
            raise ValueError("compact audit result integer is invalid")
        token = self.payload[start:self.offset]
        value = int(token.decode("ascii"))
        if str(value).encode("ascii") != token:
            raise ValueError("compact audit result integer is noncanonical")
        return value

    def boolean(self) -> bool:
        if self.payload[self.offset:self.offset + 4] == b"true":
            self.offset += 4
            return True
        if self.payload[self.offset:self.offset + 5] == b"false":
            self.offset += 5
            return False
        raise ValueError("compact audit result boolean is invalid")


@dataclass(frozen=True, slots=True)
class _AuditResultMetadata:
    dependencies: tuple[DependencyDigest, ...]
    retained_result_bytes: int


def _bounded_relative_path(value: str, maximum_bytes: int) -> PurePosixPath:
    if (
        not value
        or len(value.encode("utf-8")) > maximum_bytes
        or "\\" in value
        or value.startswith("/")
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise AuditInfrastructureError("audit result path is invalid or too large")
    return PurePosixPath(value)


def _preparse_audit_result_payload(
    payload: bytes,
    *,
    configuration_digest: str,
    engine: str,
) -> _AuditResultMetadata:
    """Validate canonical structure while retaining dependency metadata only."""
    if (
        not isinstance(payload, bytes)
        or len(payload) > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
    ):
        raise ValueError("encoded compact audit result limit exceeded")
    cursor = _CanonicalAuditResultCursor(payload)
    limits = AuditLimits()
    cursor.expect(b'{"audit_engine_fingerprint":')
    if cursor.string() != engine:
        raise ValueError("compact audit result engine differs")
    cursor.expect(b',"configuration_digest":')
    if cursor.string() != configuration_digest:
        raise ValueError("compact audit result configuration differs")
    cursor.expect(b',"dependencies":[')
    dependencies: list[DependencyDigest] = []
    dependency_retained_bytes = 0
    previous_dependency_key: tuple[str, str, str] | None = None
    while cursor.payload[cursor.offset:cursor.offset + 1] != b"]":
        if dependencies:
            cursor.expect(b",")
        if len(dependencies) >= limits.compact_result_dependencies:
            raise AuditInfrastructureError("audit result limit exceeded for dependencies")
        cursor.expect(b'{"canonical":')
        canonical = cursor.string(limits.compact_result_path_bytes)
        cursor.expect(b',"device":')
        device = cursor.integer_or_null()
        cursor.expect(b',"inode":')
        inode = cursor.integer_or_null()
        cursor.expect(b',"line_count":')
        line_count = cursor.integer_or_null()
        cursor.expect(b',"production":')
        production = cursor.boolean()
        cursor.expect(b',"relative":')
        if cursor.payload[cursor.offset:cursor.offset + 4] == b"null":
            cursor.offset += 4
            relative = None
        else:
            relative = _bounded_relative_path(
                cursor.string(limits.compact_result_path_bytes),
                limits.compact_result_path_bytes,
            )
        cursor.expect(b',"role_relative_path":')
        role_relative = _bounded_relative_path(
            cursor.string(limits.compact_result_path_bytes),
            limits.compact_result_path_bytes,
        )
        cursor.expect(b',"sha256":')
        sha256 = cursor.string()
        cursor.expect(b',"stable_role":')
        stable_role = cursor.string()
        cursor.expect(b"}")
        dependency = DependencyDigest(
            stable_role,
            role_relative,
            FileIdentity(
                Path(canonical), relative, device, inode, line_count, production
            ),
            sha256,
        )
        dependency_key = (
            dependency.stable_role,
            dependency.role_relative_path.as_posix(),
            dependency.sha256,
        )
        if previous_dependency_key is not None and dependency_key <= previous_dependency_key:
            raise AuditInfrastructureError(
                "audit result dependencies are not unique and sorted"
            )
        previous_dependency_key = dependency_key
        dependencies.append(dependency)
        dependency_retained_bytes += (
            384
            + len(dependency.stable_role.encode("ascii"))
            + len(dependency.role_relative_path.as_posix().encode("utf-8"))
            + len(str(dependency.identity.canonical).encode("utf-8"))
            + (
                len(dependency.identity.relative.as_posix().encode("utf-8"))
                if dependency.identity.relative is not None
                else 0
            )
        )
    cursor.expect(b']')
    cursor.expect(b',"findings":[')
    finding_count = 0
    finding_retained_bytes = 0
    previous_finding_key: tuple[str, int, str, str] | None = None
    while cursor.payload[cursor.offset:cursor.offset + 1] != b"]":
        if finding_count:
            cursor.expect(b",")
        if finding_count >= limits.compact_result_findings:
            raise AuditInfrastructureError("audit result limit exceeded for findings")
        cursor.expect(b'{"expression":')
        expression = cursor.string(limits.compact_result_expression_bytes)
        cursor.expect(b',"line":')
        line = cursor.integer_or_null()
        cursor.expect(b',"path":')
        path = _bounded_relative_path(
            cursor.string(limits.compact_result_path_bytes),
            limits.compact_result_path_bytes,
        )
        cursor.expect(b',"reason":')
        reason = cursor.string(limits.compact_result_reason_bytes)
        cursor.expect(b"}")
        finding = AuditResultFinding(path, line, expression, reason)
        finding_key = (path.as_posix(), finding.line, expression, reason)
        if previous_finding_key is not None and finding_key <= previous_finding_key:
            raise AuditInfrastructureError("audit result findings are not unique and sorted")
        previous_finding_key = finding_key
        finding_count += 1
        finding_retained_bytes += (
            320
            + len(path.as_posix().encode("utf-8"))
            + len(expression.encode("utf-8"))
            + len(reason.encode("utf-8"))
        )
    cursor.expect(b']')
    cursor.expect(b',"reached_production":[')
    reached_count = 0
    reached_retained_bytes = 0
    previous_reached: str | None = None
    while cursor.payload[cursor.offset:cursor.offset + 1] != b"]":
        if reached_count:
            cursor.expect(b",")
        if reached_count >= limits.compact_result_reached:
            raise AuditInfrastructureError("audit result limit exceeded for reached paths")
        reached = _bounded_relative_path(
            cursor.string(limits.compact_result_path_bytes),
            limits.compact_result_path_bytes,
        ).as_posix()
        if previous_reached is not None and reached <= previous_reached:
            raise AuditInfrastructureError(
                "audit result reached-production paths are not unique and sorted"
            )
        previous_reached = reached
        reached_count += 1
        reached_retained_bytes += 128 + len(reached.encode("utf-8"))
    cursor.expect(b']')
    cursor.expect(b',"schema":')
    if cursor.string() != AUDIT_RESULT_SCHEMA_BYTES.decode("ascii"):
        raise ValueError("compact audit result schema differs")
    cursor.expect(b"}")
    if cursor.offset != len(payload):
        raise ValueError("compact audit result has trailing data")
    retained = (
        1024
        + dependency_retained_bytes
        + reached_retained_bytes
        + finding_retained_bytes
    )
    if retained > limits.compact_result_bytes:
        raise AuditInfrastructureError("per-entry decoded result limit exceeded")
    return _AuditResultMetadata(tuple(dependencies), retained)


def encode_configuration_audit_result(result: ConfigurationAuditResult) -> bytes:
    return _encode_audit_result_payload(result)


def decode_configuration_audit_result(payload: bytes) -> ConfigurationAuditResult:
    if (
        not isinstance(payload, bytes)
        or len(payload) > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
    ):
        raise AuditInfrastructureError("encoded compact audit result limit exceeded")
    try:
        cursor = _CanonicalAuditResultCursor(payload)
        cursor.expect(b'{"audit_engine_fingerprint":')
        engine = cursor.string()
        cursor.expect(b',"configuration_digest":')
        configuration_digest = cursor.string()
        return _decode_audit_result_payload(
            payload,
            configuration_digest=configuration_digest,
            engine=engine,
        )
    except AuditInfrastructureError:
        raise
    except (
        UnicodeError,
        ValueError,
        TypeError,
        KeyError,
        RecursionError,
        json.JSONDecodeError,
    ) as error:
        raise AuditInfrastructureError("compact audit result payload is invalid") from error


def encode_configuration_audit_result_transport(
    result: ConfigurationAuditResult,
    capability: CompactResultTransportCapability,
    stdout_bytes: int,
    stages: WorkerStageTimings,
) -> ConfigurationAuditResultTransport:
    ownership = result._transport_ownership
    if (
        not isinstance(ownership, CompactResultReservationOwnership)
        or not ownership.active
        or ownership.phase != "worker-materialized-result"
    ):
        raise AuditInfrastructureError(
            "compact audit result transport ownership is unavailable"
        )
    payload = encode_configuration_audit_result(result)
    serialized = ownership.transfer("serialized-pipe")
    try:
        receipt = serialized.reservation.complete_worker_transport(
            capability, payload, stdout_bytes, stages
        )
        return ConfigurationAuditResultTransport(payload, receipt)
    except BaseException as error:
        if serialized.active and not serialized.reservation.released:
            try:
                serialized.release("transport-carrier-failure")
            except BaseException as cleanup_error:
                error.add_note(
                    "transport carrier cleanup also failed: "
                    f"{cleanup_error}"
                )
        raise


def _encode_configuration_audit_transport_header(
    outcome: ConfigurationAuditTransportOutcome,
) -> bytes:
    if not isinstance(outcome, ConfigurationAuditTransportOutcome):
        raise AuditInfrastructureError("worker audit transport outcome is invalid")
    receipt = outcome.transport.receipt
    stages = receipt.stages
    try:
        return struct.pack(
            _AUDIT_TRANSPORT_HEADER_FORMAT,
            _AUDIT_TRANSPORT_MAGIC,
            hashlib.sha256(receipt.task_id.encode("utf-8")).digest(),
            bytes.fromhex(receipt.configuration_digest),
            bytes.fromhex(receipt.audit_engine_fingerprint),
            receipt.generation,
            receipt.serial,
            bytes.fromhex(receipt.nonce),
            receipt.worker_slot,
            bytes.fromhex(receipt.pipe_nonce),
            receipt.encoded_bytes,
            receipt.charged_bytes,
            bytes.fromhex(receipt.payload_sha256),
            receipt.stdout_bytes,
            float(stages.discovery_seconds),
            float(stages.accepted_parse_seconds),
            float(stages.audit_seconds),
            float(stages.publish_seconds),
        )
    except (OverflowError, struct.error, ValueError) as error:
        raise AuditInfrastructureError(
            "worker audit transport header is invalid"
        ) from error


def send_configuration_audit_transport(
    connection,
    outcome: ConfigurationAuditTransportOutcome,
) -> None:
    header = _encode_configuration_audit_transport_header(outcome)
    try:
        connection.send_bytes(header)
        connection.send_bytes(outcome.transport.payload)
    except (AttributeError, EOFError, OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "cannot send worker audit transport"
        ) from error


def _receive_audit_transport_frame(
    connection,
    maximum_bytes: int,
    deadline: float,
    cancel_event,
    worker_alive,
) -> bytes:
    def wait_quantum() -> float:
        if cancel_event is not None and cancel_event.is_set():
            raise AuditInfrastructureError("worker audit transport was cancelled")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AuditInfrastructureError(
                "worker audit transport deadline exceeded"
            )
        return min(0.05, remaining)

    def require_worker_after_stall() -> None:
        if worker_alive is not None and not worker_alive():
            raise AuditInfrastructureError(
                "worker exited before audit transport completed"
            )

    if os.name == "nt":
        import _winapi

        try:
            handle = connection.fileno()
        except (AttributeError, OSError, ValueError) as error:
            raise AuditInfrastructureError(
                "worker audit transport pipe is invalid"
            ) from error
        while True:
            try:
                if not connection.poll(wait_quantum()):
                    require_worker_after_stall()
                    continue
                _available_bytes, message_bytes = _winapi.PeekNamedPipe(
                    handle, 0
                )
                if message_bytes > maximum_bytes:
                    raise AuditInfrastructureError(
                        "worker audit transport frame is malformed or over limit"
                    )
                if message_bytes <= 0:
                    raise AuditInfrastructureError(
                        "worker audit transport frame is truncated or over limit"
                    )
                payload, error_code = _winapi.ReadFile(handle, message_bytes)
            except (AttributeError, EOFError, OSError, ValueError) as error:
                raise AuditInfrastructureError(
                    "worker audit transport frame is truncated or over limit"
                ) from error
            if error_code != 0 or len(payload) != message_bytes:
                raise AuditInfrastructureError(
                    "worker audit transport frame is truncated or over limit"
                )
            return payload

    try:
        descriptor = connection.fileno()
        was_blocking = os.get_blocking(descriptor)
        os.set_blocking(descriptor, False)
    except (AttributeError, OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "worker audit transport pipe is invalid"
        ) from error
    framing = bytearray()
    payload = bytearray()
    expected_bytes = None
    try:
        while True:
            quantum = wait_quantum()
            target = framing if expected_bytes is None else payload
            remaining = (
                4 - len(framing)
                if expected_bytes is None
                else expected_bytes - len(payload)
            )
            if remaining == 0:
                if expected_bytes is None:
                    expected_bytes = struct.unpack("!i", framing)[0]
                    if expected_bytes < 0 or expected_bytes > maximum_bytes:
                        raise AuditInfrastructureError(
                            "worker audit transport frame is malformed or over limit"
                        )
                    if expected_bytes == 0:
                        return b""
                    continue
                return bytes(payload)
            try:
                chunk = os.read(descriptor, min(64 * 1024, remaining))
            except BlockingIOError:
                chunk = None
            except OSError as error:
                raise AuditInfrastructureError(
                    "worker audit transport frame is truncated or over limit"
                ) from error
            if chunk:
                target.extend(chunk)
                continue
            if chunk == b"":
                raise AuditInfrastructureError(
                    "worker audit transport frame is truncated or over limit"
                )
            try:
                ready = connection.poll(quantum)
            except (AttributeError, EOFError, OSError, ValueError) as error:
                raise AuditInfrastructureError(
                    "worker audit transport frame is truncated or over limit"
                ) from error
            if not ready:
                require_worker_after_stall()
    finally:
        try:
            os.set_blocking(descriptor, was_blocking)
        except OSError:
            pass


def receive_configuration_audit_transport(
    connection,
    capability: CompactResultTransportCapability,
    deadline: float,
    *,
    cancel_event=None,
    worker_alive=None,
) -> ConfigurationAuditTransportOutcome:
    if (
        not isinstance(capability, CompactResultTransportCapability)
        or not isinstance(deadline, (int, float))
        or isinstance(deadline, bool)
        or not math.isfinite(deadline)
        or (worker_alive is not None and not callable(worker_alive))
        or (
            cancel_event is not None
            and not callable(getattr(cancel_event, "is_set", None))
        )
    ):
        raise AuditInfrastructureError("worker audit transport receive is invalid")
    header = _receive_audit_transport_frame(
        connection,
        _AUDIT_TRANSPORT_HEADER_BYTES,
        float(deadline),
        cancel_event,
        worker_alive,
    )
    if len(header) != _AUDIT_TRANSPORT_HEADER_BYTES:
        raise AuditInfrastructureError("worker audit transport header is malformed")
    try:
        (
            magic,
            task_hash,
            configuration_digest,
            audit_engine_fingerprint,
            generation,
            serial,
            nonce,
            worker_slot,
            pipe_nonce,
            encoded_bytes,
            charged_bytes,
            payload_sha256,
            stdout_bytes,
            discovery_seconds,
            accepted_parse_seconds,
            audit_seconds,
            publish_seconds,
        ) = struct.unpack(_AUDIT_TRANSPORT_HEADER_FORMAT, header)
    except struct.error as error:
        raise AuditInfrastructureError(
            "worker audit transport header is malformed"
        ) from error
    expected_header = (
        magic == _AUDIT_TRANSPORT_MAGIC
        and task_hash == hashlib.sha256(capability.task_id.encode("utf-8")).digest()
        and configuration_digest.hex() == capability.configuration_digest
        and audit_engine_fingerprint.hex()
        == capability.audit_engine_fingerprint
        and generation == capability.generation
        and serial == capability.serial
        and nonce.hex() == capability.nonce
        and worker_slot == capability.worker_slot
        and pipe_nonce.hex() == capability.pipe_nonce
        and 0 < encoded_bytes <= _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
        and 0 < charged_bytes <= capability.maximum_bytes
    )
    if not expected_header:
        raise AuditInfrastructureError(
            "worker audit transport header authentication failed"
        )
    stages = WorkerStageTimings(
        discovery_seconds,
        accepted_parse_seconds,
        audit_seconds,
        publish_seconds,
    )
    payload = _receive_audit_transport_frame(
        connection,
        _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES,
        float(deadline),
        cancel_event,
        worker_alive,
    )
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError("worker audit transport was cancelled")
    if (
        len(payload) != encoded_bytes
        or hashlib.sha256(payload).digest() != payload_sha256
    ):
        raise AuditInfrastructureError(
            "worker audit transport payload authentication failed"
        )
    receipt = CompactResultTransportReceipt(
        capability.task_id,
        generation,
        configuration_digest.hex(),
        audit_engine_fingerprint.hex(),
        worker_slot,
        pipe_nonce.hex(),
        serial,
        nonce.hex(),
        encoded_bytes,
        charged_bytes,
        payload_sha256.hex(),
        stdout_bytes,
        stages,
    )
    transport = ConfigurationAuditResultTransport(payload, receipt)
    return ConfigurationAuditTransportOutcome(
        transport, receipt.stdout_bytes, receipt.stages
    )


def decode_configuration_audit_result_transport(
    transport: ConfigurationAuditResultTransport,
    reservation: PerTaskCompactReservation,
    capability: CompactResultTransportCapability,
) -> ConfigurationAuditResult:
    if (
        not isinstance(transport, ConfigurationAuditResultTransport)
    ):
        raise AuditInfrastructureError("compact audit result transport is invalid")
    decoding = None
    retained = None
    try:
        reservation.authenticate_worker_transport_envelope(
            capability, transport.receipt, transport.payload
        )
        decoded = _decode_audit_result_payload(
            transport.payload,
            configuration_digest=capability.configuration_digest,
            engine=capability.audit_engine_fingerprint,
        )
        path_bytes = sum(
            len(item.stable_role.encode("ascii"))
            + len(item.role_relative_path.as_posix().encode("utf-8"))
            + len(str(item.identity.canonical).encode("utf-8"))
            + (
                len(item.identity.relative.as_posix().encode("utf-8"))
                if item.identity.relative is not None
                else 0
            )
            for item in decoded.dependencies
        ) + sum(
            len(path.as_posix().encode("utf-8"))
            for path in decoded.reached_production
        ) + sum(
            len(item.path.as_posix().encode("utf-8"))
            for item in decoded.findings
        )
        bounds = CompactResultDraftBounds(
            len(decoded.dependencies),
            len(decoded.reached_production),
            len(decoded.findings),
            path_bytes,
            sum(
                len(item.expression.encode("utf-8"))
                for item in decoded.findings
            ),
            sum(len(item.reason.encode("utf-8")) for item in decoded.findings),
        )
        authenticated_charge = PerTaskCompactReservation.exact_transport_charge(
            len(transport.payload), bounds, 4096
        )
        decoding = reservation.begin_receiver_transport_decode(
            capability,
            transport.receipt,
            transport.payload,
            authenticated_charge,
        )
        retained = decoding.transfer("receiver-retained-result")
        return ConfigurationAuditResult(
            decoded.configuration_digest,
            decoded.audit_engine_fingerprint,
            decoded.dependencies,
            decoded.reached_production,
            decoded.findings,
            retained,
        )
    except BaseException as error:
        active = retained if retained is not None else decoding
        if active is not None and active.active:
            try:
                active.release("receiver-decode-failure")
            except BaseException as cleanup_error:
                error.add_note(
                    "receiver decode ownership cleanup also failed: "
                    f"{cleanup_error}"
                )
        elif (
            not reservation.released
            and reservation.owner_phase == "worker-transport-dispatched"
        ):
            try:
                reservation.release_worker_transport_capability(
                    capability, "receiver-transport-rejected"
                )
            except BaseException as cleanup_error:
                error.add_note(
                    "receiver transport rejection cleanup also failed: "
                    f"{cleanup_error}"
                )
                if not reservation.released:
                    try:
                        reservation.release("receiver-transport-rejected")
                    except BaseException as fallback_error:
                        error.add_note(
                            "receiver transport fallback cleanup also failed: "
                            f"{fallback_error}"
                        )
        if isinstance(error, AuditInfrastructureError):
            raise
        if isinstance(
            error,
            (UnicodeError, ValueError, TypeError, KeyError, RecursionError),
        ):
            raise AuditInfrastructureError(
                "compact audit result transport payload is invalid"
            ) from error
        raise


encode_compact_audit_result = encode_configuration_audit_result
decode_compact_audit_result = decode_configuration_audit_result


def _held_dependency_stat(metadata: os.stat_result) -> tuple[int, int | None, int, int, int, int]:
    return (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
        int(metadata.st_size),
        int(metadata.st_mtime_ns),
        int(getattr(metadata, "st_ctime_ns", 0)),
        int(getattr(metadata, "st_nlink", 1)),
    )


class _HeldDependencyHandle:
    def __init__(self, dependency: DependencyDigest) -> None:
        self.dependency = dependency
        self.stream = None
        self.opened_stat: tuple[int, int | None, int, int, int, int] | None = None
        self.hash_count = 0
        self.deadline: float | None = None

    def __enter__(self) -> "_HeldDependencyHandle":
        path = self.dependency.identity.canonical
        before = path.lstat()
        if (
            _is_link(before)
            or not stat.S_ISREG(before.st_mode)
            or int(getattr(before, "st_nlink", 1)) != 1
        ):
            raise _UnsafeCacheNamespaceError("dependency path is linked or unsafe")
        if os.name == "nt":
            import ctypes
            import msvcrt
            from ctypes import wintypes

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CreateFileW.argtypes = (
                wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                wintypes.HANDLE,
            )
            kernel32.CreateFileW.restype = wintypes.HANDLE
            handle = kernel32.CreateFileW(
                str(path), 0x80000000, 0x00000001, None, 3,
                0x00000080 | 0x00200000, None,
            )
            invalid = ctypes.c_void_p(-1).value
            if handle in (None, invalid):
                raise OSError(ctypes.get_last_error(), "cannot hold dependency")
            try:
                descriptor = msvcrt.open_osfhandle(int(handle), os.O_RDONLY)
                self.stream = os.fdopen(descriptor, "rb", closefd=True)
            except BaseException:
                kernel32.CloseHandle(handle)
                raise
        else:
            flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
            self.stream = os.fdopen(os.open(path, flags), "rb", closefd=True)
        opened = os.fstat(self.stream.fileno())
        expected = self.dependency.identity
        opened_generation = _held_dependency_stat(opened)
        path_generation = _held_dependency_stat(before)
        if (
            not stat.S_ISREG(opened.st_mode)
            or int(getattr(opened, "st_nlink", 1)) != 1
            or opened_generation[:4] != path_generation[:4]
            or opened_generation[5] != path_generation[5]
            or (expected.device is not None and int(opened.st_dev) != expected.device)
            or (
                expected.inode is not None
                and int(opened.st_ino) != expected.inode
            )
        ):
            self.stream.close()
            self.stream = None
            raise OSError("dependency identity changed while opening")
        self.opened_stat = opened_generation
        return self

    def verify_path(self) -> None:
        _require(
            self.stream is not None and self.opened_stat is not None,
            "held dependency is not open",
        )
        current = self.dependency.identity.canonical.lstat()
        opened = os.fstat(self.stream.fileno())
        if (
            _is_link(current)
            or not stat.S_ISREG(current.st_mode)
            or _held_dependency_stat(opened)[:2] != self.opened_stat[:2]
            or _held_dependency_stat(opened)[5] != 1
            or _held_dependency_stat(current)[:2] != self.opened_stat[:2]
        ):
            raise OSError("dependency path no longer names held identity")

    def __exit__(self, _type, _value, _traceback) -> None:
        if self.stream is not None:
            self.stream.close()
            self.stream = None


def _open_dependency_handle(dependency: DependencyDigest) -> _HeldDependencyHandle:
    return _HeldDependencyHandle(dependency)


def _hash_held_dependency(held: _HeldDependencyHandle) -> str:
    if not isinstance(held, _HeldDependencyHandle) or held.stream is None:
        raise AuditInfrastructureError("held dependency handle is invalid")
    before = _held_dependency_stat(os.fstat(held.stream.fileno()))
    held.stream.seek(0)
    digest = hashlib.sha256()
    while block := held.stream.read(_IO_BLOCK_BYTES):
        if held.deadline is not None and time.monotonic() >= held.deadline:
            raise AuditInfrastructureError("dependency hashing deadline exceeded")
        digest.update(block)
    after = _held_dependency_stat(os.fstat(held.stream.fileno()))
    if (
        before != after
        or before[:2] != held.opened_stat[:2]
        or before[5] != 1
        or (held.hash_count == 0 and before != held.opened_stat)
    ):
        raise AuditInfrastructureError("dependency is unstable while hashing")
    held.verify_path()
    if held.deadline is not None and time.monotonic() >= held.deadline:
        raise AuditInfrastructureError("dependency hashing deadline exceeded")
    held.hash_count += 1
    return digest.hexdigest()


@dataclass(frozen=True, slots=True)
class ConfigurationAuditLoadBatch:
    hit_count: int
    misses: tuple[PreprocessConfiguration, ...]
    cold_slot_reserved_bytes: int


@dataclass(frozen=True, slots=True)
class _AuditCacheCandidate:
    configuration: PreprocessConfiguration
    key: str
    entry_identity: tuple[int, int | None]
    payload_identity: tuple[int, int | None, int, int]
    dependencies: tuple[DependencyDigest, ...]
    manifest_identity: tuple[int, int | None, int, int] | None = None
    payload_sha256: str = ""
    decoded_result_bytes: int = 0
    metadata_ownership: CompactResultOwnership | None = None


class _TracebackLifetimeOwnerships:
    """Release temporary ownerships on success; retain them with failures."""

    __slots__ = ("_ownerships",)

    def __init__(self) -> None:
        self._ownerships: list[CompactResultOwnership] = []

    def __enter__(self) -> "_TracebackLifetimeOwnerships":
        return self

    def add(self, ownership: CompactResultOwnership) -> None:
        if (
            not isinstance(ownership, CompactResultOwnership)
            or ownership.released
        ):
            raise AuditInfrastructureError(
                "traceback lifetime ownership is invalid"
            )
        self._ownerships.append(ownership)

    def __exit__(self, error_type, _error, _traceback) -> bool:
        if error_type is None:
            release_errors: list[BaseException] = []
            for ownership in reversed(self._ownerships):
                if not ownership.released:
                    try:
                        ownership.release()
                    except BaseException as release_error:
                        release_errors.append(release_error)
            self._ownerships.clear()
            if release_errors:
                error = AuditInfrastructureError(
                    "temporary ownership cleanup is incomplete"
                )
                for additional in release_errors:
                    error.add_note(
                        f"temporary ownership release failed: {additional}"
                    )
                raise error from release_errors[0]
        return False


class ConfigurationAuditCache:
    """Atomic compact-result cache with held-handle dependency validation."""

    def __init__(
        self,
        root: Path,
        maximum_bytes: int = _AUDIT_CACHE_MAXIMUM_BYTES,
        maximum_entries: int = _AUDIT_CACHE_MAXIMUM_ENTRIES,
    ) -> None:
        if not isinstance(root, Path) or not root.is_absolute():
            raise AuditInfrastructureError("audit cache root must be absolute")
        if (
            not isinstance(maximum_bytes, int)
            or isinstance(maximum_bytes, bool)
            or maximum_bytes < 0
            or maximum_bytes > _AUDIT_CACHE_MAXIMUM_BYTES
            or not isinstance(maximum_entries, int)
            or isinstance(maximum_entries, bool)
            or maximum_entries < 0
            or maximum_entries > _AUDIT_CACHE_MAXIMUM_ENTRIES
        ):
            raise AuditInfrastructureError("audit cache bounds are invalid")
        try:
            root.mkdir(parents=True, exist_ok=True)
            metadata = _ordinary_directory(root)
        except OSError as error:
            raise AuditInfrastructureError("audit cache root is unavailable") from error
        self.root = root
        self.maximum_bytes = maximum_bytes
        self.maximum_entries = maximum_entries
        self._root_identity = _directory_identity(metadata)
        self._publication_lock = threading.Lock()
        self.observed_entry_key: str | None = None
        self.observed_lane: int | None = None
        self.observed_manifest_key: str | None = None

    @staticmethod
    def _operation_deadline(pipeline_deadline: float) -> float:
        if (
            not isinstance(pipeline_deadline, (int, float))
            or isinstance(pipeline_deadline, bool)
            or not math.isfinite(float(pipeline_deadline))
        ):
            raise AuditInfrastructureError("audit cache deadline is invalid")
        started = time.monotonic()
        deadline = min(float(pipeline_deadline), started + _DEFAULT_CACHE_OPERATION_SECONDS)
        if started >= deadline:
            raise AuditInfrastructureError("audit cache deadline exceeded")
        return deadline

    def _assert_root(self) -> None:
        try:
            metadata = _ordinary_directory(self.root)
        except OSError as error:
            raise AuditInfrastructureError("audit cache root was replaced") from error
        if _directory_identity(metadata) != self._root_identity:
            raise AuditInfrastructureError("audit cache root was replaced")

    def _root_lock(self, deadline: float) -> _SharedCacheFileLock:
        return _SharedCacheFileLock(
            self.root / ".preprocess-root.lock",
            self.root,
            self._root_identity,
            deadline,
        )

    def _key_lock(self, key: str, deadline: float) -> _SharedCacheFileLock:
        lane = hashlib.sha256(
            f"preprocess:{key}".encode("utf-8")
        ).digest()[0] % _LOCK_NAMESPACE_LANE_COUNT
        self.observed_lane = lane
        return _SharedCacheFileLock(
            self.root / f".preprocess-key-{lane}.lock",
            self.root,
            self._root_identity,
            deadline,
            namespace_path=self.root / ".preprocess-root.lock",
        )

    @property
    def _results_root(self) -> Path:
        return self.root / _AUDIT_RESULTS_DIRECTORY

    def _entry_path(self, key: str) -> Path:
        self.observed_entry_key = key
        return self._results_root / key[:2] / key

    def _access_path(self, key: str) -> Path:
        return self._results_root / key[:2] / f".access-{key}"

    def _assert_no_full_view_partition(self) -> None:
        with os.scandir(self.root) as entries:
            for entry in entries:
                if (
                    len(entry.name) == 64
                    and all(character in "0123456789abcdef" for character in entry.name)
                ):
                    raise AuditInfrastructureError(
                        "legacy full-view partition is present in compact audit cache root"
                    )

    def _prepare_locked(self) -> None:
        self._assert_root()
        self._assert_no_full_view_partition()
        marker = self.root / _AUDIT_ROOT_MARKER
        marker_payload: bytes | None = None
        try:
            with _HeldCacheFile(marker) as held:
                _require(held.stream is not None, "held cache marker stream is unavailable")
                payload = held.stream.read(len(AUDIT_CACHE_SCHEMA_BYTES) + 1)
                held.verify()
            marker_payload = payload
            if marker_payload != AUDIT_CACHE_SCHEMA_BYTES:
                raise AuditInfrastructureError("audit cache root marker is invalid")
        except FileNotFoundError:
            temporary = self.root / f".tmp-audit-marker-{uuid.uuid4().hex}"
            temporary_identity: tuple[int, int] | None = None
            try:
                with temporary.open("xb") as stream:
                    temporary_identity = _file_ownership_identity(
                        os.fstat(stream.fileno())
                    )
                    stream.write(AUDIT_CACHE_SCHEMA_BYTES)
                    stream.flush()
                    os.fsync(stream.fileno())
                try:
                    os.link(temporary, marker)
                except FileExistsError:
                    pass
                temporary.unlink()
            except BaseException as error:
                _cleanup_owned_temporary_file(
                    temporary,
                    temporary_identity,
                    "audit cache root marker temporary",
                    error,
                )
                raise
        except _UnsafeCacheNamespaceError as error:
            raise AuditInfrastructureError("audit cache root marker is unsafe") from error
        if marker_payload is None:
            try:
                with _HeldCacheFile(marker) as held:
                    _require(held.stream is not None, "held cache marker stream is unavailable")
                    marker_payload = held.stream.read(
                        len(AUDIT_CACHE_SCHEMA_BYTES) + 1
                    )
                    held.verify()
            except OSError as error:
                raise AuditInfrastructureError("audit cache root marker is unsafe") from error
        if marker_payload != AUDIT_CACHE_SCHEMA_BYTES:
            raise AuditInfrastructureError("audit cache root marker is invalid")
        try:
            self._results_root.mkdir(exist_ok=True)
            _ordinary_directory(self._results_root)
        except OSError as error:
            raise AuditInfrastructureError("audit cache results namespace is unsafe") from error
        now = time.time()
        with os.scandir(self._results_root) as bucket_entries:
            for bucket_entry in bucket_entries:
                bucket = Path(bucket_entry.path)
                metadata = bucket_entry.stat(follow_symlinks=False)
                if _is_link(metadata) or not stat.S_ISDIR(metadata.st_mode):
                    raise AuditInfrastructureError("audit cache result bucket is unsafe")
                with os.scandir(bucket) as raw_entries:
                    for raw in raw_entries:
                        if not raw.name.startswith(".tmp-"):
                            continue
                        temporary = Path(raw.path)
                        temporary_metadata = raw.stat(follow_symlinks=False)
                        if (
                            _is_link(temporary_metadata)
                            or not stat.S_ISDIR(temporary_metadata.st_mode)
                        ):
                            raise AuditInfrastructureError(
                                "audit cache crash temporary is unsafe"
                            )
                        if now - temporary_metadata.st_mtime > _INCOMPLETE_SECONDS:
                            if _temporary_publication_is_active(temporary):
                                continue
                            if not _remove_held_flat_directory(temporary):
                                raise AuditInfrastructureError(
                                    "cannot clean audit cache crash temporary"
                                )

    def prepare(self, pipeline_deadline: float) -> None:
        deadline = self._operation_deadline(pipeline_deadline)
        try:
            with self._root_lock(deadline):
                self._prepare_locked()
        except AuditInfrastructureError:
            raise
        except OSError as error:
            raise AuditInfrastructureError("audit cache namespace is unsafe") from error

    def _prepare_for_operation(self, deadline: float) -> None:
        try:
            with self._root_lock(deadline):
                self._prepare_locked()
        except AuditInfrastructureError:
            raise
        except OSError as error:
            raise AuditInfrastructureError("audit cache namespace is unsafe") from error

    def _ensure_bucket(self, key: str) -> Path:
        bucket = self._results_root / key[:2]
        try:
            bucket.mkdir(exist_ok=True)
            _ordinary_directory(bucket)
        except OSError as error:
            raise AuditInfrastructureError("audit cache result bucket is unsafe") from error
        return bucket

    def _manifest_bytes(self, key: str, payload: bytes) -> bytes:
        self.observed_manifest_key = key
        document = {
            "schema": AUDIT_CACHE_SCHEMA_BYTES.decode("ascii"),
            "key": key,
            "payload_bytes": len(payload),
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
        }
        return json.dumps(
            document, ensure_ascii=True, sort_keys=True, separators=(",", ":")
        ).encode("ascii")

    def _read_authenticated_entry(
        self,
        configuration: PreprocessConfiguration,
        key: str,
        engine: str,
    ) -> tuple[_AuditCacheCandidate, ConfigurationAuditResult] | None:
        entry = self._entry_path(key)
        manifest_payload = None
        manifest = None
        payload = None
        result = None
        candidate = None
        normal_exit = False
        try:
            with _HeldDirectory(entry) as held_directory:
                names = frozenset(os.listdir(entry))
                if names != frozenset({"manifest.json", "payload.json"}):
                    raise _UnsafeCacheNamespaceError(
                        "audit cache entry carrier set is invalid"
                    )
                manifest_path = entry / "manifest.json"
                payload_path = entry / "payload.json"
                with _HeldCacheFile(manifest_path) as manifest_held:
                    _require(
                        manifest_held.stream is not None,
                        "held audit manifest stream is unavailable",
                    )
                    manifest_payload = manifest_held.stream.read(
                        _AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES + 1
                    )
                    manifest_held.verify()
                    manifest_identity = manifest_held.identity
                if len(manifest_payload) > _AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES:
                    raise ValueError("audit result manifest is too large")
                manifest = _strict_json_document(manifest_payload)
                if tuple(sorted(manifest)) != (
                    "key", "payload_bytes", "payload_sha256", "schema"
                ):
                    raise ValueError("audit result manifest schema is invalid")
                with _HeldCacheFile(payload_path) as payload_held:
                    _require(
                        payload_held.stream is not None,
                        "held audit payload stream is unavailable",
                    )
                    payload = payload_held.stream.read(
                        _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES + 1
                    )
                    payload_held.verify()
                    payload_identity = payload_held.identity
                if (
                    manifest["schema"] != AUDIT_CACHE_SCHEMA_BYTES.decode("ascii")
                    or manifest["key"] != key
                    or not isinstance(manifest["payload_bytes"], int)
                    or isinstance(manifest["payload_bytes"], bool)
                    or manifest["payload_bytes"] != len(payload)
                    or not isinstance(manifest["payload_sha256"], str)
                    or manifest["payload_sha256"]
                    != hashlib.sha256(payload).hexdigest()
                    or self._manifest_bytes(key, payload) != manifest_payload
                ):
                    raise ValueError("audit result manifest authentication failed")
                try:
                    result = _decode_audit_result_payload(
                        payload,
                        configuration_digest=configuration.digest,
                        engine=engine,
                    )
                except AuditInfrastructureError:
                    normal_exit = True
                    return None
                _require(
                    held_directory.identity is not None,
                    "held audit directory identity is unavailable",
                )
                _require(
                    payload_identity is not None,
                    "held audit payload identity is unavailable",
                )
                candidate = _AuditCacheCandidate(
                    configuration,
                    key,
                    held_directory.identity,
                    payload_identity,
                    result.dependencies,
                    manifest_identity,
                    hashlib.sha256(payload).hexdigest(),
                    compact_result_retained_bytes(result),
                )
                normal_exit = True
                return candidate, result
        except FileNotFoundError:
            normal_exit = True
            return None
        except _UnsafeCacheNamespaceError as error:
            raise AuditInfrastructureError("audit cache namespace is unsafe or linked") from error
        except (
            UnicodeError,
            ValueError,
            TypeError,
            KeyError,
            RecursionError,
            json.JSONDecodeError,
        ):
            normal_exit = True
            return None
        except OSError as error:
            raise AuditInfrastructureError("audit cache namespace is unsafe") from error
        finally:
            if normal_exit:
                candidate = None
                result = None
                payload = None
                manifest = None
                manifest_payload = None

    def _read_authenticated_candidate_metadata(
        self,
        configuration: PreprocessConfiguration,
        key: str,
        engine: str,
        result_budget: CompactResultMemoryBudget,
    ) -> _AuditCacheCandidate | None:
        """Authenticate one entry and retain only charged dependency metadata."""
        entry = self._entry_path(key)
        manifest_scratch: CompactResultOwnership | None = None
        scratch: CompactResultOwnership | None = None
        metadata_ownership: CompactResultOwnership | None = None
        manifest_payload = None
        manifest = None
        payload = None
        metadata = None
        candidate = None
        normal_exit = False
        try:
            with _HeldDirectory(entry) as held_directory:
                if frozenset(os.listdir(entry)) != frozenset(
                    {"manifest.json", "payload.json"}
                ):
                    raise _UnsafeCacheNamespaceError(
                        "audit cache entry carrier set is invalid"
                    )
                manifest_path = entry / "manifest.json"
                payload_path = entry / "payload.json"
                with _HeldCacheFile(manifest_path) as manifest_held:
                    _require(
                        manifest_held.stream is not None,
                        "held audit manifest stream is unavailable",
                    )
                    _require(
                        manifest_held.identity is not None,
                        "held audit manifest identity is unavailable",
                    )
                    if manifest_held.identity[2] > _AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES:
                        raise ValueError("audit result manifest is too large")
                    manifest_scratch = result_budget.reserve(
                        4096 + 8 * manifest_held.identity[2],
                        label="compact result manifest workspace",
                    ).commit()
                    manifest_payload = manifest_held.stream.read(
                        _AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES + 1
                    )
                    manifest_held.verify()
                    manifest_identity = manifest_held.identity
                if len(manifest_payload) > _AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES:
                    raise ValueError("audit result manifest is too large")
                manifest = _strict_json_document(manifest_payload)
                if tuple(sorted(manifest)) != (
                    "key", "payload_bytes", "payload_sha256", "schema"
                ):
                    raise ValueError("audit result manifest schema is invalid")
                with _HeldCacheFile(payload_path) as payload_held:
                    _require(
                        payload_held.stream is not None,
                        "held audit payload stream is unavailable",
                    )
                    _require(
                        payload_held.identity is not None,
                        "held audit payload identity is unavailable",
                    )
                    payload_size = payload_held.identity[2]
                    if payload_size > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES:
                        raise ValueError("encoded compact audit result limit exceeded")
                    scratch_bytes = (
                        8192 + 8 * len(manifest_payload) + 9 * payload_size
                    )
                    scratch = result_budget.reserve(
                        scratch_bytes,
                        label="compact result metadata preparse workspace",
                    ).commit()
                    payload = payload_held.stream.read(payload_size + 1)
                    payload_held.verify()
                    payload_identity = payload_held.identity
                payload_sha256 = hashlib.sha256(payload).hexdigest()
                if (
                    manifest["schema"] != AUDIT_CACHE_SCHEMA_BYTES.decode("ascii")
                    or manifest["key"] != key
                    or not isinstance(manifest["payload_bytes"], int)
                    or isinstance(manifest["payload_bytes"], bool)
                    or manifest["payload_bytes"] != len(payload)
                    or manifest["payload_sha256"] != payload_sha256
                    or self._manifest_bytes(key, payload) != manifest_payload
                ):
                    raise ValueError("audit result manifest authentication failed")
                metadata = _preparse_audit_result_payload(
                    payload,
                    configuration_digest=configuration.digest,
                    engine=engine,
                )
                metadata_bytes = (
                    512
                    + 8 * len(metadata.dependencies)
                    + sum(
                        384
                        + len(dependency.stable_role.encode("ascii"))
                        + len(
                            dependency.role_relative_path.as_posix().encode("utf-8")
                        )
                        + len(str(dependency.identity.canonical).encode("utf-8"))
                        + (
                            len(
                                dependency.identity.relative.as_posix().encode(
                                    "utf-8"
                                )
                            )
                            if dependency.identity.relative is not None
                            else 0
                        )
                        for dependency in metadata.dependencies
                    )
                )
                metadata_ownership = result_budget.reserve(
                    metadata_bytes,
                    label="batch retained candidate metadata",
                ).commit()
                _require(
                    held_directory.identity is not None,
                    "held audit directory identity is unavailable",
                )
                _require(
                    payload_identity is not None,
                    "held audit payload identity is unavailable",
                )
                candidate = _AuditCacheCandidate(
                    configuration,
                    key,
                    held_directory.identity,
                    payload_identity,
                    metadata.dependencies,
                    manifest_identity,
                    payload_sha256,
                    metadata.retained_result_bytes,
                    metadata_ownership,
                )
                metadata_ownership = None
                normal_exit = True
                return candidate
        except FileNotFoundError:
            normal_exit = True
            return None
        except _UnsafeCacheNamespaceError as error:
            raise AuditInfrastructureError(
                "audit cache namespace is unsafe or linked"
            ) from error
        except AuditInfrastructureError:
            raise
        except (
            UnicodeError,
            ValueError,
            TypeError,
            KeyError,
            RecursionError,
            json.JSONDecodeError,
        ):
            normal_exit = True
            return None
        except OSError as error:
            raise AuditInfrastructureError("audit cache namespace is unsafe") from error
        finally:
            if normal_exit:
                candidate = None
                metadata = None
                payload = None
                manifest = None
                manifest_payload = None
                if scratch is not None and not scratch.released:
                    scratch.release()
                if manifest_scratch is not None and not manifest_scratch.released:
                    manifest_scratch.release()
                if (
                    metadata_ownership is not None
                    and not metadata_ownership.released
                ):
                    metadata_ownership.release()

    def _record_access(self, key: str) -> None:
        path = self._access_path(key)
        temporary = path.with_name(f".tmp-access-{key}-{uuid.uuid4().hex}")
        identity: tuple[int, int] | None = None
        try:
            with temporary.open("xb") as stream:
                identity = _file_ownership_identity(os.fstat(stream.fileno()))
                stream.write(b"1")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        except OSError as error:
            _cleanup_owned_temporary_file(
                temporary, identity, "audit result access temporary", error
            )

    @staticmethod
    def _snapshot_map(production_snapshot) -> dict[str, DependencyDigest]:
        if not hasattr(production_snapshot, "items"):
            raise AuditInfrastructureError("production snapshot map is invalid")
        result: dict[str, DependencyDigest] = {}
        for raw_path, value in production_snapshot.items():
            if not isinstance(value, DependencyDigest):
                raise AuditInfrastructureError("production snapshot entry is invalid")
            key = (
                raw_path.as_posix()
                if isinstance(raw_path, PurePosixPath)
                else str(raw_path).replace("\\", "/")
            )
            if key in result:
                raise AuditInfrastructureError("production snapshot is ambiguous")
            result[key] = value
        return result

    @staticmethod
    def _production_matches(
        dependencies: tuple[DependencyDigest, ...],
        snapshot: dict[str, DependencyDigest],
    ) -> bool:
        for dependency in dependencies:
            if dependency.stable_role != "production":
                continue
            current = snapshot.get(dependency.role_relative_path.as_posix())
            if current is None or current != dependency:
                return False
        return True

    @staticmethod
    def _validate_dependency_authority(
        dependencies: tuple[DependencyDigest, ...],
        authority: DependencyRootAuthority,
    ) -> None:
        roots = {
            authority.source_root.stable_role: authority.source_root.resolved_root,
            **{
                binding.stable_role: binding.resolved_root
                for binding in authority.external_roots
            },
        }
        for dependency in dependencies:
            root = roots.get(dependency.stable_role)
            if root is None:
                raise AuditInfrastructureError("audit result dependency authority differs")
            expected = root / Path(dependency.role_relative_path.as_posix())
            if _path_key(expected) != _path_key(dependency.identity.canonical):
                raise AuditInfrastructureError("audit result dependency path is outside authority")

    def load(
        self,
        configuration: PreprocessConfiguration,
        dependency_roots: DependencyRootAuthority,
        engine: str,
        production_snapshot,
        pipeline_deadline: float,
    ) -> ConfigurationAuditResult | None:
        if not isinstance(configuration, PreprocessConfiguration):
            raise AuditInfrastructureError("audit cache configuration is invalid")
        deadline = self._operation_deadline(pipeline_deadline)
        authority = validate_dependency_root_authority(dependency_roots)
        snapshot = self._snapshot_map(production_snapshot)
        self._prepare_for_operation(deadline)
        key = audit_cache_key(configuration.digest, engine)
        with self._key_lock(key, deadline):
            loaded = self._read_authenticated_entry(configuration, key, engine)
        if loaded is None:
            return None
        candidate, result = loaded
        self._validate_dependency_authority(candidate.dependencies, authority)
        if not self._production_matches(candidate.dependencies, snapshot):
            return None
        handles: list[_HeldDependencyHandle] = []
        try:
            with contextlib.ExitStack() as stack:
                for dependency in candidate.dependencies:
                    handle = stack.enter_context(_open_dependency_handle(dependency))
                    handle.deadline = deadline
                    handles.append(handle)
                if any(
                    _hash_held_dependency(handle) != handle.dependency.sha256
                    for handle in handles
                ):
                    return None
                if any(
                    _hash_held_dependency(handle) != handle.dependency.sha256
                    for handle in handles
                ):
                    return None
        except _UnsafeCacheNamespaceError as error:
            raise AuditInfrastructureError("dependency namespace is linked or unsafe") from error
        except OSError:
            return None
        self._record_access(key)
        return result

    def load_many(
        self,
        configurations: tuple[PreprocessConfiguration, ...],
        dependency_roots: DependencyRootAuthority,
        engine: str,
        production_snapshot,
        result_budget: CompactResultMemoryBudget,
        aggregator: StreamingResultAggregator,
        maximum_cold_slot: CompactResultColdSlot,
        pipeline_deadline: float,
    ) -> ConfigurationAuditLoadBatch:
        if (
            not isinstance(configurations, tuple)
            or len(configurations) > _AUDIT_CACHE_MAXIMUM_ENTRIES
            or any(not isinstance(item, PreprocessConfiguration) for item in configurations)
            or len({item.digest for item in configurations}) != len(configurations)
        ):
            raise AuditInfrastructureError("audit cache batch configurations are invalid")
        if (
            not isinstance(result_budget, CompactResultMemoryBudget)
            or not isinstance(aggregator, StreamingResultAggregator)
            or aggregator.budget is not result_budget
        ):
            raise AuditInfrastructureError("audit cache batch ownership is invalid")
        deadline = self._operation_deadline(pipeline_deadline)
        authority = validate_dependency_root_authority(dependency_roots)
        snapshot = self._snapshot_map(production_snapshot)
        self._prepare_for_operation(deadline)
        return self._load_many_prepared(
            configurations,
            engine,
            snapshot,
            result_budget,
            aggregator,
            maximum_cold_slot,
            deadline,
            authority,
        )

    def _load_many_prepared(
        self,
        configurations: tuple[PreprocessConfiguration, ...],
        engine: str,
        snapshot: dict[str, DependencyDigest],
        result_budget: CompactResultMemoryBudget,
        aggregator: StreamingResultAggregator,
        maximum_cold_slot: CompactResultColdSlot,
        deadline: float,
        authority: DependencyRootAuthority,
    ) -> ConfigurationAuditLoadBatch:
        candidates: dict[str, _AuditCacheCandidate] = {}
        misses: set[str] = set()
        checkpoint = aggregator._checkpoint()
        observer = result_budget.observer
        semantic_transaction = (
            None
            if observer is None
            else observer.begin_semantic_transaction()
        )
        commit_semantics = False

        try:
            with _TracebackLifetimeOwnerships() as ownership_cleanup, contextlib.ExitStack() as dependency_stack:
                for configuration in configurations:
                    if time.monotonic() >= deadline:
                        raise AuditInfrastructureError("audit cache deadline exceeded")
                    key = audit_cache_key(configuration.digest, engine)
                    with self._key_lock(key, deadline):
                        candidate = self._read_authenticated_candidate_metadata(
                            configuration, key, engine, result_budget
                        )
                    if candidate is None:
                        misses.add(configuration.digest)
                        continue
                    if candidate.metadata_ownership is not None:
                        ownership_cleanup.add(candidate.metadata_ownership)
                    self._validate_dependency_authority(
                        candidate.dependencies, authority
                    )
                    if not self._production_matches(candidate.dependencies, snapshot):
                        misses.add(configuration.digest)
                        candidate_ownership = candidate.metadata_ownership
                        candidate = None
                        if (
                            candidate_ownership is not None
                            and not candidate_ownership.released
                        ):
                            candidate_ownership.release()
                        continue
                    candidates[configuration.digest] = candidate

                validation_metadata_bytes = 512 + sum(
                    544 + len(str(dependency.identity.canonical).encode("utf-8"))
                    for candidate in candidates.values()
                    for dependency in candidate.dependencies
                )
                validation_metadata_ownership = result_budget.reserve(
                    validation_metadata_bytes,
                    label="dependency validation metadata",
                ).commit()
                ownership_cleanup.add(validation_metadata_ownership)
                dependency_users: dict[str, list[_AuditCacheCandidate]] = {}
                dependency_representatives: dict[str, DependencyDigest] = {}
                expected_dependencies: dict[
                    tuple[str, str], DependencyDigest
                ] = {}
                limits = AuditLimits()
                metadata_bytes = 0
                for candidate in candidates.values():
                    candidate_paths: set[str] = set()
                    for dependency in candidate.dependencies:
                        path_key = _path_key(dependency.identity.canonical)
                        if path_key in candidate_paths:
                            raise AuditInfrastructureError(
                                "audit result dependency paths are ambiguous"
                            )
                        candidate_paths.add(path_key)
                        if path_key not in dependency_representatives:
                            metadata_bytes += 512 + len(
                                str(dependency.identity.canonical).encode("utf-8")
                            )
                        metadata_bytes += 32
                        if (
                            len(dependency_representatives)
                            + int(path_key not in dependency_representatives)
                            > limits.unique_dependency_handles
                            or metadata_bytes
                            > limits.dependency_handle_metadata_bytes
                        ):
                            raise AuditInfrastructureError(
                                "dependency handle or metadata ceiling exceeded before open"
                            )
                        dependency_users.setdefault(path_key, []).append(candidate)
                        dependency_representatives.setdefault(path_key, dependency)
                        expected_dependencies[
                            (candidate.configuration.digest, path_key)
                        ] = dependency

                held: dict[str, _HeldDependencyHandle] = {}
                try:
                    for path_key, dependency in dependency_representatives.items():
                        handle = dependency_stack.enter_context(
                            _open_dependency_handle(dependency)
                        )
                        handle.deadline = deadline
                        held[path_key] = handle
                except _UnsafeCacheNamespaceError as error:
                    raise AuditInfrastructureError(
                        "dependency namespace is linked or unsafe"
                    ) from error
                except OSError:
                    misses.update(candidates)

                initial: dict[str, str] = {}
                for path_key, handle in held.items():
                    initial[path_key] = _hash_held_dependency(handle)
                    opened_identity = handle.opened_stat[:2]
                    for candidate in dependency_users[path_key]:
                        dependency = expected_dependencies[
                            (candidate.configuration.digest, path_key)
                        ]
                        if (
                            dependency.identity.device,
                            dependency.identity.inode,
                        ) != opened_identity:
                            misses.add(candidate.configuration.digest)
                for path_key, users in dependency_users.items():
                    for candidate in users:
                        expected = expected_dependencies[
                            (candidate.configuration.digest, path_key)
                        ].sha256
                        if initial.get(path_key) != expected:
                            misses.add(candidate.configuration.digest)

                if misses:
                    aggregator.reserve_cold_slot(maximum_cold_slot)
                accepted_digests: list[str] = []
                hit_count = 0
                for configuration in configurations:
                    if configuration.digest in misses:
                        continue
                    candidate = candidates[configuration.digest]
                    result_ownership = result_budget.reserve(
                        candidate.decoded_result_bytes,
                        label="batch retained result limit",
                    ).commit()
                    decode_workspace: CompactResultOwnership | None = None
                    decoded = None
                    decoded_candidate = None
                    result = None
                    accepted = False
                    release_result_ownership = False
                    release_decode_workspace = False
                    try:
                        decode_workspace = result_budget.reserve(
                            8192 + 9 * candidate.payload_identity[2],
                            label="compact result decode workspace",
                        ).commit()
                        with self._key_lock(candidate.key, deadline):
                            decoded = self._read_authenticated_entry(
                                configuration, candidate.key, engine
                            )
                        release_decode_workspace = True
                        decode_workspace.release()
                        decode_workspace = None
                        if decoded is None:
                            misses.add(configuration.digest)
                            if aggregator.cold_slot_reserved_bytes == 0:
                                aggregator.reserve_cold_slot(maximum_cold_slot)
                            release_result_ownership = True
                            continue
                        decoded_candidate, result = decoded
                        if (
                            decoded_candidate.entry_identity != candidate.entry_identity
                            or decoded_candidate.manifest_identity is None
                            or candidate.manifest_identity is None
                            or decoded_candidate.manifest_identity[:2]
                            != candidate.manifest_identity[:2]
                            or decoded_candidate.payload_identity[:2]
                            != candidate.payload_identity[:2]
                            or decoded_candidate.payload_identity[2]
                            != candidate.payload_identity[2]
                            or decoded_candidate.payload_sha256
                            != candidate.payload_sha256
                            or decoded_candidate.dependencies != candidate.dependencies
                            or decoded_candidate.decoded_result_bytes
                            != candidate.decoded_result_bytes
                        ):
                            raise AuditInfrastructureError(
                                "audit cache payload generation was replaced during batch load"
                            )
                        self._validate_dependency_authority(
                            result.dependencies, authority
                        )
                        production_matches = self._production_matches(
                            result.dependencies, snapshot
                        )
                        if not production_matches:
                            misses.add(configuration.digest)
                            if aggregator.cold_slot_reserved_bytes == 0:
                                aggregator.reserve_cold_slot(maximum_cold_slot)
                            release_result_ownership = True
                            continue
                        result_ownership.record_semantic(
                            "retain-hit",
                            delta_bytes=result_ownership.byte_count,
                        )
                        aggregator.accept_validated_result(
                            configuration,
                            result,
                            result_ownership,
                            caller_retains_ownership=True,
                        )
                        accepted = True
                        release_result_ownership = True
                    finally:
                        # The aggregate copies every retained field.  Destroy all
                        # decoded-result aliases before releasing their exact
                        # charge; loop reassignment is not a lifetime boundary.
                        result = None
                        decoded_candidate = None
                        decoded = None
                        if (
                            decode_workspace is not None
                            and release_decode_workspace
                            and not decode_workspace.released
                        ):
                            decode_workspace.release()
                        if (
                            release_result_ownership
                            and not result_ownership.released
                        ):
                            result_ownership.release(
                                semantic_event="release-result"
                            )
                    if accepted:
                        accepted_digests.append(configuration.digest)
                        hit_count += 1

                final: dict[str, str] = {}
                try:
                    for path_key, handle in held.items():
                        final[path_key] = _hash_held_dependency(handle)
                except OSError:
                    final.clear()
                final_drift = False
                for path_key, users in dependency_users.items():
                    for candidate in users:
                        expected = expected_dependencies[
                            (candidate.configuration.digest, path_key)
                        ].sha256
                        if final.get(path_key) != expected:
                            final_drift = True
                            break
                    if final_drift:
                        break
                if final_drift:
                    aggregator._rollback_to(checkpoint)
                    misses.update(accepted_digests)
                    hit_count = 0
                    if aggregator.cold_slot_reserved_bytes == 0:
                        aggregator.reserve_cold_slot(maximum_cold_slot)
                else:
                    for digest in accepted_digests:
                        self._record_access(candidates[digest].key)
                    commit_semantics = True

                ordered_misses = tuple(
                    configuration for configuration in configurations
                    if configuration.digest in misses
                )
                batch = ConfigurationAuditLoadBatch(
                    hit_count,
                    ordered_misses,
                    aggregator.cold_slot_reserved_bytes,
                )
                candidate = None
                decoded_candidate = None
                result = None
                dependency = None
                users = None
                handle = None
                candidates.clear()
                dependency_users.clear()
                dependency_representatives.clear()
                expected_dependencies.clear()
                held.clear()
                initial.clear()
                final.clear()
            if semantic_transaction is not None:
                observer.finish_semantic_transaction(
                    semantic_transaction, commit=commit_semantics
                )
                semantic_transaction = None
            return batch
        except BaseException as load_error:
            if semantic_transaction is not None:
                try:
                    observer.finish_semantic_transaction(
                        semantic_transaction, commit=False
                    )
                except BaseException as cleanup_error:
                    load_error.add_note(
                        "semantic transaction rollback also failed: "
                        f"{cleanup_error}"
                    )
            try:
                aggregator._rollback_to(checkpoint)
            except BaseException as cleanup_error:
                load_error.add_note(
                    f"aggregate rollback also failed: {cleanup_error}"
                )
            raise load_error

    def _remove_audit_entry(self, entry: Path, key: str) -> bool:
        quarantine = entry.with_name(f".quarantine-{key}-{uuid.uuid4().hex}")
        try:
            identity = _directory_identity(_ordinary_directory(entry))
            os.rename(entry, quarantine)
        except FileNotFoundError:
            return True
        except OSError:
            return False
        removed = _remove_held_flat_directory(
            quarantine, expected_identity=identity
        )
        if removed:
            try:
                self._access_path(key).unlink()
            except FileNotFoundError:
                pass
            except OSError:
                return False
        return removed

    def cleanup(self, now: float, pipeline_deadline: float) -> None:
        if not isinstance(now, (int, float)) or isinstance(now, bool):
            raise AuditInfrastructureError("audit cache cleanup time is invalid")
        deadline = self._operation_deadline(pipeline_deadline)
        self._prepare_for_operation(deadline)
        complete: list[tuple[float, int, str, Path]] = []
        for raw_bucket in os.scandir(self._results_root):
            if (
                len(raw_bucket.name) != 2
                or any(character not in "0123456789abcdef" for character in raw_bucket.name)
            ):
                raise AuditInfrastructureError("audit cache result bucket name is invalid")
            bucket = Path(raw_bucket.path)
            metadata = raw_bucket.stat(follow_symlinks=False)
            if _is_link(metadata) or not stat.S_ISDIR(metadata.st_mode):
                raise AuditInfrastructureError("audit cache result bucket is unsafe")
            for raw_entry in os.scandir(bucket):
                name = raw_entry.name
                if name.startswith((".access-", ".tmp-", ".quarantine-")):
                    continue
                entry = Path(raw_entry.path)
                metadata = raw_entry.stat(follow_symlinks=False)
                if (
                    len(name) != 64
                    or any(character not in "0123456789abcdef" for character in name)
                    or name[:2] != raw_bucket.name
                    or _is_link(metadata)
                    or not stat.S_ISDIR(metadata.st_mode)
                ):
                    raise AuditInfrastructureError("audit cache entry namespace is unsafe")
                size = 0
                authentic = False
                try:
                    names = frozenset(os.listdir(entry))
                    if names != frozenset({"manifest.json", "payload.json"}):
                        raise ValueError("carrier set")
                    manifest_path = entry / "manifest.json"
                    payload_path = entry / "payload.json"
                    manifest_metadata = _regular_unlinked_file(manifest_path)
                    payload_metadata = _regular_unlinked_file(payload_path)
                    if (
                        manifest_metadata.st_size
                        > _AUDIT_RESULT_MANIFEST_MAXIMUM_BYTES
                        or payload_metadata.st_size
                        > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
                    ):
                        raise ValueError("entry size")
                    manifest_payload = manifest_path.read_bytes()
                    payload = payload_path.read_bytes()
                    manifest = _strict_json_document(manifest_payload)
                    authentic = (
                        manifest.get("schema")
                        == AUDIT_CACHE_SCHEMA_BYTES.decode("ascii")
                        and manifest.get("key") == name
                        and manifest.get("payload_bytes") == len(payload)
                        and manifest.get("payload_sha256")
                        == hashlib.sha256(payload).hexdigest()
                    )
                    size = int(manifest_metadata.st_size + payload_metadata.st_size)
                except _UnsafeCacheNamespaceError as error:
                    raise AuditInfrastructureError(
                        "audit cache entry namespace is unsafe"
                    ) from error
                except (OSError, ValueError, TypeError, UnicodeError):
                    authentic = False
                if not authentic:
                    if now - metadata.st_mtime > _INCOMPLETE_SECONDS:
                        with self._key_lock(name, deadline):
                            if not self._remove_audit_entry(entry, name):
                                raise AuditInfrastructureError(
                                    "cannot remove incomplete audit cache entry"
                                )
                    continue
                try:
                    last_access = _regular_unlinked_file(
                        self._access_path(name)
                    ).st_mtime
                    size += 1
                except FileNotFoundError:
                    last_access = metadata.st_mtime
                except OSError as error:
                    raise AuditInfrastructureError(
                        "audit cache access namespace is unsafe"
                    ) from error
                if now - last_access > _COMPLETE_SECONDS:
                    with self._key_lock(name, deadline):
                        if not self._remove_audit_entry(entry, name):
                            raise AuditInfrastructureError(
                                "cannot remove expired audit cache entry"
                            )
                    continue
                complete.append((last_access, size, name, entry))
        total_bytes = sum(item[1] for item in complete)
        total_entries = len(complete)
        for _access, size, key, entry in sorted(complete):
            if total_bytes <= self.maximum_bytes and total_entries <= self.maximum_entries:
                break
            with self._key_lock(key, deadline):
                if self._remove_audit_entry(entry, key):
                    total_bytes -= size
                    total_entries -= 1

    def publish(
        self,
        configuration: PreprocessConfiguration,
        dependency_roots: DependencyRootAuthority,
        result: ConfigurationAuditResult,
        publication_permit: ConfigurationAuditPublicationPermit,
        pipeline_deadline: float,
    ) -> ConfigurationAuditResult:
        if not isinstance(configuration, PreprocessConfiguration) or not isinstance(
            result, ConfigurationAuditResult
        ):
            raise AuditInfrastructureError("audit result publication is invalid")
        deadline = self._operation_deadline(pipeline_deadline)
        authority = validate_dependency_root_authority(dependency_roots)
        if (
            not isinstance(publication_permit, ConfigurationAuditPublicationPermit)
            or publication_permit.configuration_digest != configuration.digest
            or publication_permit.audit_engine_fingerprint
            != result.audit_engine_fingerprint
            or publication_permit.dependencies != result.dependencies
            or result.configuration_digest != configuration.digest
        ):
            raise AuditInfrastructureError("audit result publication generation permit differs")
        self._validate_dependency_authority(result.dependencies, authority)
        payload = _encode_audit_result_payload(result)
        key = audit_cache_key(configuration.digest, result.audit_engine_fingerprint)
        manifest = self._manifest_bytes(key, payload)
        self._prepare_for_operation(deadline)
        bucket = self._ensure_bucket(key)
        temporary = bucket / f".tmp-{key}-{uuid.uuid4().hex}"
        entry = self._entry_path(key)
        temporary_identity: tuple[int, int | None] | None = None
        try:
            with contextlib.ExitStack() as stack:
                held = [
                    stack.enter_context(_open_dependency_handle(dependency))
                    for dependency in result.dependencies
                ]
                for handle in held:
                    handle.deadline = deadline
                if any(
                    _hash_held_dependency(handle) != handle.dependency.sha256
                    for handle in held
                ):
                    raise AuditInfrastructureError(
                        "audit result publication generation permit content differs"
                    )
                temporary.mkdir()
                temporary_identity = _directory_identity(_ordinary_directory(temporary))
                with _PublicationGuard(temporary / "active.lock", deadline):
                    with (temporary / "payload.json").open("xb") as stream:
                        stream.write(payload)
                        stream.flush()
                        os.fsync(stream.fileno())
                    with (temporary / "manifest.json").open("xb") as stream:
                        stream.write(manifest)
                        stream.flush()
                        os.fsync(stream.fileno())
                    if any(
                        _hash_held_dependency(handle) != handle.dependency.sha256
                        for handle in held
                    ):
                        raise AuditInfrastructureError(
                            "audit result publication generation changed"
                        )
            (temporary / "active.lock").unlink()
            with self._key_lock(key, deadline):
                with self._publication_lock:
                    existing = self._read_authenticated_entry(
                        configuration, key, result.audit_engine_fingerprint
                    )
                    if existing is not None:
                        _candidate, winner = existing
                        if winner != result:
                            raise AuditInfrastructureError(
                                "concurrent audit result winner differs"
                            )
                        if not _remove_held_flat_directory(
                            temporary, expected_identity=temporary_identity
                        ):
                            raise AuditInfrastructureError(
                                "cannot remove losing audit result temporary"
                            )
                        self._record_access(key)
                        return winner
                    stale: Path | None = None
                    stale_identity: tuple[int, int | None] | None = None
                    try:
                        stale_identity = _directory_identity(
                            _ordinary_directory(entry)
                        )
                    except FileNotFoundError:
                        pass
                    except OSError as error:
                        raise AuditInfrastructureError(
                            "audit result namespace is unsafe"
                        ) from error
                    else:
                        stale = bucket / f".quarantine-{key}-{uuid.uuid4().hex}"
                        os.rename(entry, stale)
                    try:
                        os.rename(temporary, entry)
                    except FileExistsError:
                        existing = self._read_authenticated_entry(
                            configuration, key, result.audit_engine_fingerprint
                        )
                        if existing is None or existing[1] != result:
                            raise AuditInfrastructureError(
                                "concurrent audit result winner differs"
                            )
                        if not _remove_held_flat_directory(
                            temporary, expected_identity=temporary_identity
                        ):
                            raise AuditInfrastructureError(
                                "cannot remove losing audit result temporary"
                            )
                        return existing[1]
                    finally:
                        if stale is not None and not _remove_held_flat_directory(
                            stale, expected_identity=stale_identity
                        ):
                            raise AuditInfrastructureError(
                                "cannot remove stale audit result entry"
                            )
            self._record_access(key)
            return result
        except _UnsafeCacheNamespaceError as error:
            raise AuditInfrastructureError("audit result namespace is unsafe") from error
        except BaseException as error:
            if temporary_identity is not None:
                try:
                    removed = _remove_held_flat_directory(
                        temporary, expected_identity=temporary_identity
                    )
                except OSError:
                    removed = False
                if not removed:
                    try:
                        current = _ordinary_directory(temporary)
                    except FileNotFoundError:
                        pass
                    except OSError as cleanup_error:
                        raise AuditInfrastructureError(
                            "audit result publication temporary was replaced"
                        ) from cleanup_error
                    else:
                        if _directory_identity(current) == temporary_identity:
                            raise AuditInfrastructureError(
                                "cannot remove audit result publication temporary"
                            ) from error
            if isinstance(error, AuditInfrastructureError):
                raise
            raise AuditInfrastructureError("cannot publish compact audit result") from error


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
            self.root / ".preprocess-root.lock",
        )

    def _acquire_key_barrier(
        self,
        key: str,
        deadline: float,
        cancel_event: object | None = None,
    ) -> _SharedCacheFileLock:
        key_lock = self._key_lock(key, deadline, cancel_event)
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
        temporary_identity: tuple[int, int] | None = None
        try:
            self._assert_root_identity()
            with temporary.open("xb") as stream:
                temporary_identity = _file_ownership_identity(
                    os.fstat(stream.fileno())
                )
                stream.write(b"1\n")
                stream.flush()
                os.fsync(stream.fileno())
            timestamp = time.time() if now is None else now
            os.utime(temporary, (timestamp, timestamp))
            self._assert_root_identity()
            os.replace(temporary, self._access_path(key))
            self._assert_root_identity()
        except OSError as error:
            _cleanup_owned_temporary_file(
                temporary,
                temporary_identity,
                "cache access temporary",
                error,
            )

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
                _require(
                    held.stream is not None and held.identity is not None,
                    "held preprocess payload is not open",
                )
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
            if temporary_identity is not None:
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
