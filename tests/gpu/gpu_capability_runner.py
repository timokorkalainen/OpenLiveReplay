"""Bounded subprocess execution for compiler-authoritative GPU audit views."""

from __future__ import annotations

import collections
import contextlib
import base64
import hashlib
import hmac
import json
import math
import multiprocessing
import os
import queue
import re
import secrets
import selectors
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import socket
from array import array
from collections.abc import Callable, Mapping
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from types import MappingProxyType

from gpu_capability_cache import (
    CompilerInspectionCache,
    ConfigurationAuditCache,
    PreprocessCache,
    _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES,
    decode_configuration_audit_result_transport,
    encode_configuration_audit_result_transport,
    receive_configuration_audit_transport,
    send_configuration_audit_transport,
)
from gpu_capability_command import (
    CompilerProcessHandleCarrier,
    RewrittenCommand,
    _CompilerCapabilityOwner,
    _environment_digest,
    _held_compiler_launch,
    _regular_file_snapshot,
    _resume_suspended_windows_process,
    _native_process_start_token,
    decode_compile_entry,
    make_configuration,
    launch_compiler_process,
    compiler_digest_from_capability,
    decision_environment_digest,
    inspect_compiler,
    normalize_decision_arguments,
    rewrite_preprocess_command,
    strip_launchers,
    validate_compiler_executable_capability,
)
from gpu_capability_model import (
    AUDIT_RESULT_SCHEMA_BYTES,
    AuditResultFinding,
    AuditInfrastructureError,
    AuditLimits,
    CanonicalAuditContentSummary,
    CachePublicationPermit,
    CachePublicationRequested,
    CompactResultColdSlot,
    CompactAccountingObserver,
    CompactResultMemoryBudget,
    CompactResultOwnership,
    CompactResultPreparseBounds,
    CompactResultSlot,
    CompactResultTransportAllocation,
    CompactResultTransportCapability,
    CompactResultDraftBounds,
    CompilerAuditRun,
    CompilerFamily,
    CompilerExecutableCapability,
    CompilerLaunchEvent,
    CompilerLaunchPurpose,
    CompilerPgidReported,
    CompilerExecPermit,
    ConfigurationCollection,
    DecisionConfigurationRecord,
    ConfigurationAuditOutcome,
    ConfigurationAuditOutcomeOwner,
    ConfigurationAuditResult,
    ConfigurationAuditTask,
    ConfigurationAuditTransportOutcome,
    CoverageReport,
    DependencyDigest,
    DependencyRootAuthority,
    FileIdentity,
    PerTaskCompactReservation,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    ProcessStartIdentity,
    WorkerStageTimings,
    WorkerEngineReady,
    WorkerContained,
    LinuxWorkerContainment,
    LinuxRunMemoryMeasurements,
    LinuxPhaseSnapshot,
    MacOSPhaseSnapshot,
    MacOSInspectionPgidReported,
    MacOSRunMemoryMeasurements,
    MacOSWorkerSessionReported,
    WorkerCapabilitiesAccepted,
    WorkerFailure,
    WorkerPayloadPermit,
    WorkerPayloadReady,
    WorkerRetire,
    WorkerRetireAck,
    WorkerRuntimeContract,
    WorkerStop,
    WorkerStopped,
    StreamingAuditSummary,
    StreamingAuditMeasurements,
    StreamingResultAggregator,
    WindowsPhaseSnapshot,
    WindowsRunMemoryMeasurements,
    compact_result_conservative_decoded_bytes,
    compact_result_retained_bytes,
    _FilesystemGenerationObserver,
    _EnumeratedProductionTable,
    _current_process_rss_bytes,
    _preprocessed_view_semantic_digest,
    enumerate_production_identities,
    encode_dependency_root_authority,
    decode_dependency_root_authority,
    requires_compile_entry,
    validate_dependency_root_authority,
)
from gpu_capability_provenance import (
    PreprocessedStreamBuilder,
    parse_gcc_dependencies,
    parse_msvc_dependencies,
    validate_dependency_identities,
)


_IO_CHUNK_BYTES = 256 * 1024
_POLL_SECONDS = 0.01
_REAP_SECONDS = 1.0
_DEPENDENCY_FILE_BYTES = 256 * 1024 * 1024
_DEPENDENCY_TOTAL_BYTES = 1024 * 1024 * 1024

CONTROL_MAX_BYTES = 64 * 1024
TASK_MAX_BYTES = 256 * 1024
COMMAND_MAX_BYTES = 64 * 1024
_PIPE_KERNEL_CAPACITY_BYTES = 64 * 1024
_PROCESS_TREE_MEMORY_LIMIT_BYTES = 512 << 20
_FAILURE_REAP_SECONDS = 30.0
_PIPELINE_HARD_SECONDS = 180.0


def _production_allocation_event(_event: str) -> None:
    """Fault-injection/ordering boundary for production snapshot allocations."""


def _scheduler_compiler_launch_event(_event: CompilerLaunchEvent) -> None:
    """Test observer for authenticated parent-side compiler launch frames."""


def _scheduler_cache_publication_event(_event: CachePublicationRequested) -> None:
    """Test observer for authenticated parent-side cache publication requests."""


def _scheduler_initial_digest_map_event(_initial_digest_map) -> None:
    """Test observer for the exact outer-owned initial production map."""


def _generation_teardown_transition(_label: str, _state) -> None:
    """Fault-injection boundary after one teardown transition commits."""


def _generation_teardown_before_transition(_label: str, _state) -> None:
    """Fault-injection boundary before one teardown transition begins."""


def _generation_setup_transition(_label: str, _state) -> None:
    """Fault-injection boundary after one setup acquisition commits."""


def _generation_setup_before_transition(_label: str, _state) -> None:
    """Fault-injection boundary before one setup acquisition begins."""


def _shutdown_transition(_label: str, _reactor) -> None:
    """Fault-injection boundary after one shutdown transition commits."""


def _shutdown_before_transition(_label: str, _reactor) -> None:
    """Fault-injection boundary before one shutdown transition begins."""


_CONDITIONALLY_SELECTED_TRANSLATION_UNITS = MappingProxyType({
    PurePosixPath("playback/gpu/gpusurface_apple.mm"): "apple",
    PurePosixPath("playback/gpu/gpufence_apple.mm"): "apple",
    PurePosixPath("playback/gpu/applegpusurface_apple.mm"): "apple",
    PurePosixPath(
        "recorder_engine/codec/nativevideoencoder_videotoolbox.mm"
    ): "apple",
    PurePosixPath("playback/gpu/gpufence_win.cpp"): "windows",
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"): "windows",
    PurePosixPath("playback/output/win/d3d11gpusurface.cpp"): "windows",
    PurePosixPath(
        "recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"
    ): "windows",
})


@dataclass(frozen=True, slots=True)
class _ConservativeAllocationSchema:
    json_decoded_multiplier: int = 9
    json_decoded_fixed_bytes: int = 8192
    canonical_encoder_fixed_bytes: int = 4096


def conservative_allocation_schema() -> _ConservativeAllocationSchema:
    return _ConservativeAllocationSchema()


class _CompactResultPreparseCursor:
    """Allocation-bounded fixed-grammar scanner for canonical result bytes."""

    __slots__ = ("payload", "offset")

    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0

    def expect(self, token: bytes) -> None:
        if not self.payload.startswith(token, self.offset):
            raise ValueError("compact result payload syntax differs")
        self.offset += len(token)

    def expect_ascii_string(self, expected: bytes) -> None:
        self.expect(b'"')
        self.expect(expected)
        self.expect(b'"')

    @staticmethod
    def _hex_value(value: int) -> int:
        if 0x30 <= value <= 0x39:
            return value - 0x30
        if 0x61 <= value <= 0x66:
            return value - 0x61 + 10
        raise ValueError("compact result payload unicode escape is invalid")

    def _unicode_escape(self) -> int:
        if self.offset + 4 > len(self.payload):
            raise ValueError("compact result payload string is truncated")
        value = 0
        for _ in range(4):
            value = (value << 4) | self._hex_value(self.payload[self.offset])
            self.offset += 1
        return value

    def string_bytes(self, maximum_bytes: int | None = None) -> int:
        self.expect(b'"')
        decoded_bytes = 0
        while self.offset < len(self.payload):
            value = self.payload[self.offset]
            self.offset += 1
            if value == 0x22:
                return decoded_bytes
            if value == 0x5C:
                if self.offset >= len(self.payload):
                    break
                escape = self.payload[self.offset]
                self.offset += 1
                if escape in b'"\\/bfnrt':
                    decoded_bytes += 1
                elif escape == 0x75:
                    codepoint = self._unicode_escape()
                    if 0xD800 <= codepoint <= 0xDBFF:
                        self.expect(b"\\u")
                        low = self._unicode_escape()
                        if not 0xDC00 <= low <= 0xDFFF:
                            raise ValueError(
                                "compact result payload surrogate is invalid"
                            )
                        decoded_bytes += 4
                    elif 0xDC00 <= codepoint <= 0xDFFF:
                        raise ValueError(
                            "compact result payload surrogate is invalid"
                        )
                    elif codepoint <= 0x7F:
                        decoded_bytes += 1
                    elif codepoint <= 0x7FF:
                        decoded_bytes += 2
                    else:
                        decoded_bytes += 3
                else:
                    raise ValueError("compact result payload escape is invalid")
            elif 0x20 <= value <= 0x7E:
                decoded_bytes += 1
            else:
                raise ValueError("compact result payload string is invalid")
            if maximum_bytes is not None and decoded_bytes > maximum_bytes:
                raise AuditInfrastructureError(
                    "compact result payload text limit exceeded"
                )
        raise ValueError("compact result payload string is truncated")

    def integer_or_null(self) -> None:
        if self.payload.startswith(b"null", self.offset):
            self.offset += 4
            return
        negative = False
        if self.offset < len(self.payload) and self.payload[self.offset] == 0x2D:
            negative = True
            self.offset += 1
        if self.offset >= len(self.payload):
            raise ValueError("compact result payload integer is truncated")
        first = self.payload[self.offset]
        if first == 0x30:
            self.offset += 1
            if negative:
                raise ValueError("compact result payload integer is noncanonical")
            if (
                self.offset < len(self.payload)
                and 0x30 <= self.payload[self.offset] <= 0x39
            ):
                raise ValueError("compact result payload integer is noncanonical")
            return
        if not 0x31 <= first <= 0x39:
            raise ValueError("compact result payload integer is invalid")
        self.offset += 1
        while (
            self.offset < len(self.payload)
            and 0x30 <= self.payload[self.offset] <= 0x39
        ):
            self.offset += 1

    def boolean(self) -> None:
        if self.payload.startswith(b"true", self.offset):
            self.offset += 4
            return
        if self.payload.startswith(b"false", self.offset):
            self.offset += 5
            return
        raise ValueError("compact result payload boolean is invalid")


def _expected_compact_ascii(value: str, label: str) -> bytes:
    if not isinstance(value, str) or not value:
        raise AuditInfrastructureError(f"compact result {label} is invalid")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise AuditInfrastructureError(
            f"compact result {label} is invalid"
        ) from error
    if any(byte < 0x20 or byte in (0x22, 0x5C) for byte in encoded):
        raise AuditInfrastructureError(f"compact result {label} is invalid")
    return encoded


def preparse_compact_result(
    payload: bytes,
    *,
    expected_configuration_digest: str,
    expected_audit_engine_fingerprint: str,
    limits: AuditLimits,
) -> CompactResultPreparseBounds:
    """Bound a canonical payload without constructing decoded result objects."""
    if (
        not isinstance(payload, bytes)
        or not payload
        or len(payload) > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
        or not isinstance(limits, AuditLimits)
    ):
        raise AuditInfrastructureError("compact result payload is invalid")
    configuration_digest = _expected_compact_ascii(
        expected_configuration_digest, "configuration digest"
    )
    engine = _expected_compact_ascii(
        expected_audit_engine_fingerprint, "engine fingerprint"
    )
    cursor = _CompactResultPreparseCursor(payload)
    retained_bytes = 1024
    try:
        cursor.expect(b'{"audit_engine_fingerprint":')
        cursor.expect_ascii_string(engine)
        cursor.expect(b',"configuration_digest":')
        cursor.expect_ascii_string(configuration_digest)
        cursor.expect(b',"dependencies":[')
        dependency_count = 0
        while not payload.startswith(b"]", cursor.offset):
            if dependency_count:
                cursor.expect(b",")
            if dependency_count >= limits.compact_result_dependencies:
                raise AuditInfrastructureError(
                    "compact result payload dependency limit exceeded"
                )
            cursor.expect(b'{"canonical":')
            canonical_bytes = cursor.string_bytes(limits.compact_result_path_bytes)
            cursor.expect(b',"device":')
            cursor.integer_or_null()
            cursor.expect(b',"inode":')
            cursor.integer_or_null()
            cursor.expect(b',"line_count":')
            cursor.integer_or_null()
            cursor.expect(b',"production":')
            cursor.boolean()
            cursor.expect(b',"relative":')
            if payload.startswith(b"null", cursor.offset):
                cursor.offset += 4
                relative_bytes = 0
            else:
                relative_bytes = cursor.string_bytes(
                    limits.compact_result_path_bytes
                )
            cursor.expect(b',"role_relative_path":')
            role_relative_bytes = cursor.string_bytes(
                limits.compact_result_path_bytes
            )
            cursor.expect(b',"sha256":')
            cursor.string_bytes()
            cursor.expect(b',"stable_role":')
            stable_role_bytes = cursor.string_bytes(
                limits.compact_result_path_bytes
            )
            cursor.expect(b"}")
            retained_bytes += (
                384
                + canonical_bytes
                + relative_bytes
                + role_relative_bytes
                + stable_role_bytes
            )
            dependency_count += 1
        cursor.expect(b"]")

        cursor.expect(b',"findings":[')
        finding_count = 0
        while not payload.startswith(b"]", cursor.offset):
            if finding_count:
                cursor.expect(b",")
            if finding_count >= limits.compact_result_findings:
                raise AuditInfrastructureError(
                    "compact result payload finding limit exceeded"
                )
            cursor.expect(b'{"expression":')
            expression_bytes = cursor.string_bytes(
                limits.compact_result_expression_bytes
            )
            cursor.expect(b',"line":')
            cursor.integer_or_null()
            cursor.expect(b',"path":')
            path_bytes = cursor.string_bytes(limits.compact_result_path_bytes)
            cursor.expect(b',"reason":')
            reason_bytes = cursor.string_bytes(
                limits.compact_result_reason_bytes
            )
            cursor.expect(b"}")
            retained_bytes += (
                320 + expression_bytes + path_bytes + reason_bytes
            )
            finding_count += 1
        cursor.expect(b"]")

        cursor.expect(b',"reached_production":[')
        reached_count = 0
        while not payload.startswith(b"]", cursor.offset):
            if reached_count:
                cursor.expect(b",")
            if reached_count >= limits.compact_result_reached:
                raise AuditInfrastructureError(
                    "compact result payload reached-production limit exceeded"
                )
            reached_bytes = cursor.string_bytes(limits.compact_result_path_bytes)
            retained_bytes += 128 + reached_bytes
            reached_count += 1
        cursor.expect(b"]")
        cursor.expect(b',"schema":')
        cursor.expect_ascii_string(AUDIT_RESULT_SCHEMA_BYTES)
        cursor.expect(b"}")
        if cursor.offset != len(payload):
            raise ValueError("compact result payload has trailing data")
    except ValueError as error:
        raise AuditInfrastructureError("compact result payload is invalid") from error

    if retained_bytes > limits.compact_result_bytes:
        raise AuditInfrastructureError(
            "compact result payload retained result limit exceeded"
        )
    decoded_bytes = compact_result_conservative_decoded_bytes(len(payload))
    return CompactResultPreparseBounds(
        len(payload), decoded_bytes, retained_bytes
    )


def _transition_worker_payload_to_decode(
    ownership: CompactResultOwnership,
    bounds: CompactResultPreparseBounds,
    layout: CompactResultTransportAllocation,
) -> CompactResultOwnership:
    if (
        not isinstance(ownership, CompactResultOwnership)
        or not ownership.committed
        or not isinstance(bounds, CompactResultPreparseBounds)
        or not isinstance(layout, CompactResultTransportAllocation)
        or layout.receiver_payload_bytes != bounds.encoded_bytes
        or layout.json_decoded_transient_bytes < bounds.conservative_decoded_bytes
        or layout.retained_result_bytes != bounds.conservative_retained_bytes
    ):
        raise AuditInfrastructureError(
            "compact result payload decode transition is invalid"
        )
    return ownership.replace_committed(
        layout.decode_reservation_bytes,
        label=f"{ownership.label}:decode",
        semantic_event="decode",
    )


def _transport_allocation_bound(
    encoded_bytes: int,
    bounds: CompactResultPreparseBounds,
    allocation_schema: _ConservativeAllocationSchema,
    pipe_kernel_capacity: int,
) -> CompactResultTransportAllocation:
    if (
        not isinstance(encoded_bytes, int)
        or isinstance(encoded_bytes, bool)
        or encoded_bytes < 0
        or not isinstance(bounds, CompactResultPreparseBounds)
        or encoded_bytes != bounds.encoded_bytes
        or not isinstance(allocation_schema, _ConservativeAllocationSchema)
        or not isinstance(pipe_kernel_capacity, int)
        or isinstance(pipe_kernel_capacity, bool)
        or pipe_kernel_capacity < 0
    ):
        raise AuditInfrastructureError(
            "compact result transport allocation inputs are invalid"
        )
    canonical_scratch = (
        encoded_bytes + allocation_schema.canonical_encoder_fixed_bytes
    )
    sender_payload = encoded_bytes
    pipe_frame = encoded_bytes + 4
    receiver_payload = encoded_bytes
    decoded_transient = max(
        bounds.conservative_decoded_bytes,
        allocation_schema.json_decoded_fixed_bytes
        + allocation_schema.json_decoded_multiplier * encoded_bytes,
    )
    retained = bounds.conservative_retained_bytes
    counting_peak = canonical_scratch
    permit = (
        canonical_scratch
        + sender_payload
        + pipe_frame
        + pipe_kernel_capacity
        + receiver_payload
    )
    decode = receiver_payload + decoded_transient + retained
    return CompactResultTransportAllocation(
        counting_peak,
        canonical_scratch,
        sender_payload,
        pipe_frame,
        pipe_kernel_capacity,
        receiver_payload,
        decoded_transient,
        retained,
        permit,
        decode,
        max(counting_peak, permit, decode),
    )


def maximum_compact_result_slot(
    limits: AuditLimits,
    allocation_schema: _ConservativeAllocationSchema,
    pipe_kernel_capacity: int,
) -> CompactResultSlot:
    if not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("compact result limits are invalid")
    encoded = _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
    bounds = CompactResultPreparseBounds(
        encoded,
        allocation_schema.json_decoded_fixed_bytes
        + allocation_schema.json_decoded_multiplier * encoded,
        limits.compact_result_bytes,
    )
    layout = _transport_allocation_bound(
        encoded, bounds, allocation_schema, pipe_kernel_capacity
    )
    return CompactResultSlot(
        layout.peak_pending_bytes,
        (
            "canonical_encoder_scratch_bytes",
            "sender_payload_bytes",
            "pipe_frame_bytes",
            "pipe_kernel_capacity_bytes",
            "receiver_payload_bytes",
            "json_decoded_transient_bytes",
            "retained_result_bytes",
        ),
    )


class BoundedFrameChannel:
    """One-way Connection wrapper that permits bounded byte frames only."""

    __slots__ = ("_connection", "maximum_bytes", "_can_receive", "_closed")

    def __init__(self, connection, maximum_bytes: int, *, can_receive: bool) -> None:
        if (
            not isinstance(maximum_bytes, int)
            or isinstance(maximum_bytes, bool)
            or maximum_bytes <= 0
            or maximum_bytes > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
        ):
            raise AuditInfrastructureError("bounded frame limit is invalid")
        self._connection = connection
        self.maximum_bytes = maximum_bytes
        self._can_receive = can_receive
        self._closed = False

    @classmethod
    def create(cls, maximum_bytes: int):
        receiver, sender = multiprocessing.get_context("spawn").Pipe(
            duplex=False
        )
        return (
            cls(receiver, maximum_bytes, can_receive=True),
            cls(sender, maximum_bytes, can_receive=False),
        )

    @property
    def connection(self):
        if self._closed:
            raise AuditInfrastructureError("bounded frame endpoint is closed")
        return self._connection

    def send_bytes_before(
        self, payload: bytes, deadline: float, *, cancel_event=None
    ) -> None:
        if self._can_receive or self._closed:
            raise AuditInfrastructureError("bounded frame sender is closed")
        if (
            not isinstance(payload, bytes)
            or len(payload) > self.maximum_bytes
            or not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or not math.isfinite(deadline)
        ):
            raise AuditInfrastructureError("bounded frame write is invalid")
        if cancel_event is not None and cancel_event.is_set():
            raise AuditInfrastructureError("bounded frame write was cancelled")
        if time.monotonic() >= deadline:
            raise AuditInfrastructureError("bounded frame write deadline exceeded")
        if os.name == "nt":
            import _winapi

            def write_overlapped(frame: bytes) -> None:
                overlapped, error_code = _winapi.WriteFile(
                    self._connection.fileno(), frame, overlapped=True
                )
                while error_code == _winapi.ERROR_IO_PENDING:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        overlapped.cancel()
                        overlapped.GetOverlappedResult(True)
                        raise AuditInfrastructureError(
                            "bounded frame write deadline exceeded"
                        )
                    wait_result = _winapi.WaitForMultipleObjects(
                        [overlapped.event],
                        False,
                        max(1, min(50, math.ceil(remaining * 1000))),
                    )
                    if wait_result == _winapi.WAIT_TIMEOUT:
                        if cancel_event is not None and cancel_event.is_set():
                            overlapped.cancel()
                            overlapped.GetOverlappedResult(True)
                            raise AuditInfrastructureError(
                                "bounded frame write was cancelled"
                            )
                        continue
                    if wait_result != _winapi.WAIT_OBJECT_0:
                        raise OSError("overlapped pipe wait failed")
                    break
                written, completion_error = overlapped.GetOverlappedResult(True)
                if completion_error != 0 or written != len(frame):
                    raise OSError("overlapped pipe write failed")

            try:
                write_overlapped(payload)
            except AuditInfrastructureError:
                raise
            except (AttributeError, BrokenPipeError, EOFError, OSError, ValueError) as error:
                raise AuditInfrastructureError(
                    "bounded frame peer closed during write"
                ) from error
            return
        descriptor = None
        selector = None
        try:
            descriptor = self._connection.fileno()
            was_blocking = os.get_blocking(descriptor)
            os.set_blocking(descriptor, False)
            selector = selectors.DefaultSelector()
            selector.register(descriptor, selectors.EVENT_WRITE)
            frame = memoryview(struct.pack("!i", len(payload)) + payload)
            offset = 0
            while offset < len(frame):
                if cancel_event is not None and cancel_event.is_set():
                    raise AuditInfrastructureError(
                        "bounded frame write was cancelled"
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AuditInfrastructureError(
                        "bounded frame write deadline exceeded"
                    )
                if not selector.select(min(0.05, remaining)):
                    continue
                try:
                    written = os.write(
                        descriptor,
                        frame[offset:offset + (64 * 1024)],
                    )
                except BlockingIOError:
                    continue
                if written <= 0:
                    raise OSError("bounded frame pipe write returned zero")
                offset += written
        except AuditInfrastructureError:
            raise
        except (AttributeError, BrokenPipeError, EOFError, OSError, ValueError) as error:
            raise AuditInfrastructureError(
                "bounded frame peer closed during write"
            ) from error
        finally:
            if selector is not None:
                selector.close()
            if descriptor is not None:
                try:
                    os.set_blocking(descriptor, was_blocking)
                except OSError:
                    pass

    def receive_bytes_before(
        self, deadline: float, *, cancel_event=None, worker_alive=None
    ) -> bytes:
        if not self._can_receive or self._closed:
            raise AuditInfrastructureError("bounded frame receiver is closed")
        if (
            not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or not math.isfinite(deadline)
            or (worker_alive is not None and not callable(worker_alive))
        ):
            raise AuditInfrastructureError("bounded frame receive is invalid")
        while True:
            if cancel_event is not None and cancel_event.is_set():
                raise AuditInfrastructureError(
                    "bounded frame receive was cancelled"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AuditInfrastructureError(
                    "bounded frame receive deadline exceeded"
                )
            try:
                if not self._connection.poll(min(0.05, remaining)):
                    if worker_alive is not None and not worker_alive():
                        raise AuditInfrastructureError(
                            "bounded frame peer closed before receive"
                        )
                    continue
                return self._connection.recv_bytes(self.maximum_bytes + 1)
            except AuditInfrastructureError:
                raise
            except (EOFError, OSError, ValueError) as error:
                raise AuditInfrastructureError(
                    "bounded frame peer closed or frame was truncated"
                ) from error

    def poll(self, timeout: float = 0.0) -> bool:
        if not self._can_receive or self._closed:
            return False
        try:
            return bool(self._connection.poll(timeout))
        except (EOFError, OSError, ValueError):
            return True

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._connection.close()
        except (OSError, ValueError):
            pass


class _DeadlineBoundPayloadConnection:
    """send_bytes adapter that preserves cache transport framing semantics."""

    __slots__ = ("channel", "deadline", "cancel_event")

    def __init__(self, channel, deadline: float, cancel_event) -> None:
        self.channel = channel
        self.deadline = deadline
        self.cancel_event = cancel_event

    def send_bytes(self, payload: bytes) -> None:
        self.channel.send_bytes_before(
            payload, self.deadline, cancel_event=self.cancel_event
        )


class BoundedPayloadChannel(BoundedFrameChannel):
    """Separate 4-MiB authenticated payload endpoint with bounded waits."""

    @classmethod
    def create(cls):
        receiver, sender = multiprocessing.get_context("spawn").Pipe(
            duplex=False
        )
        return (
            cls(
                receiver,
                _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES,
                can_receive=True,
            ),
            cls(
                sender,
                _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES,
                can_receive=False,
            ),
        )

    def send_transport_before(
        self,
        outcome: ConfigurationAuditTransportOutcome,
        deadline: float,
        *,
        cancel_event=None,
    ) -> None:
        if self._can_receive or self._closed:
            raise AuditInfrastructureError("bounded payload sender is closed")
        send_configuration_audit_transport(
            _DeadlineBoundPayloadConnection(self, deadline, cancel_event),
            outcome,
        )

    def receive_transport_before(
        self,
        capability: CompactResultTransportCapability,
        deadline: float,
        *,
        cancel_event=None,
        worker_alive=None,
    ) -> ConfigurationAuditTransportOutcome:
        if not self._can_receive or self._closed:
            raise AuditInfrastructureError("bounded payload receiver is closed")
        return receive_configuration_audit_transport(
            self.connection,
            capability,
            deadline,
            cancel_event=cancel_event,
            worker_alive=worker_alive,
        )


def _bounded_wire_int(value: object, label: str, maximum: int) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 0
        or value > maximum
    ):
        raise AuditInfrastructureError(f"{label} is invalid")
    return value


def _bounded_diagnostic(error: BaseException) -> str:
    text = f"{type(error).__name__}: {error}".replace("\0", "?")
    encoded = text.encode("utf-8", errors="replace")[:4096]
    return encoded.decode("utf-8", errors="ignore") or "worker failed"


def _control_document(message) -> dict[str, object]:
    base = {
        "worker_index": message.worker_index,
        "generation": message.generation,
    }
    if isinstance(message, WorkerStop):
        return {**base, "tag": "worker-stop"}
    if isinstance(message, WorkerStopped):
        return {**base, "tag": "worker-stopped"}
    if isinstance(message, WorkerRetireAck):
        return {**base, "tag": "worker-retire-ack"}
    if isinstance(message, WorkerRetire):
        return {**base, "tag": "worker-retire", "reason": message.reason}
    if isinstance(message, WorkerFailure):
        return {
            **base,
            "tag": "worker-failure",
            "task_id": message.task_id,
            "diagnostic": message.diagnostic,
        }
    if isinstance(message, WorkerEngineReady):
        return {
            **base,
            "tag": "worker-engine-ready",
            "worker_pid": message.worker_pid,
            "audit_engine_fingerprint": message.audit_engine_fingerprint,
            "capability_digests": list(message.capability_digests),
        }
    if isinstance(message, WorkerCapabilitiesAccepted):
        return {
            **base,
            "tag": "worker-capabilities-accepted",
            "capability_digests": list(message.capability_digests),
        }
    if isinstance(message, WorkerContained):
        return {
            **base,
            "tag": "worker-contained",
            "worker_pid": message.worker_pid,
            "containment_identity": message.containment_identity,
        }
    if isinstance(message, MacOSWorkerSessionReported):
        return {
            **base,
            "tag": "macos-worker-session-reported",
            "pid": message.pid,
            "pgid": message.pgid,
            "bsd_start_identity": message.bsd_start_identity,
        }
    if isinstance(message, CompilerPgidReported):
        return {
            **base,
            "tag": "macos-compiler-pgid-reported",
            "task_id": message.task_id,
            "purpose": message.purpose.value,
            "pid": message.pid,
            "pgid": message.pgid,
            "bsd_start_identity": message.bsd_start_identity,
            "executable_identity": _identity_wire_document(
                message.executable_identity
            ),
            "executable_sha256": message.executable_sha256,
            "driver_fingerprint": message.driver_fingerprint,
        }
    if isinstance(message, CompilerExecPermit):
        return {
            **base,
            "tag": "macos-compiler-exec-permit",
            "task_id": message.task_id,
            "pgid": message.pgid,
        }
    if isinstance(message, WorkerPayloadPermit):
        return {**base, "tag": "worker-payload-permit", "task_id": message.task_id}
    if isinstance(message, WorkerPayloadReady):
        return {
            **base,
            "tag": "worker-payload-ready",
            "task_id": message.task_id,
            "configuration_digest": message.configuration_digest,
            "audit_engine_fingerprint": message.audit_engine_fingerprint,
            "pipe_nonce": message.pipe_nonce,
            "serial": message.serial,
            "nonce": message.nonce,
            "encoded_bytes": message.encoded_bytes,
            "encoded_sha256": message.encoded_sha256,
            "charged_bytes": message.charged_bytes,
            "conservative_decoded_bytes": message.conservative_decoded_bytes,
            "conservative_retained_bytes": message.conservative_retained_bytes,
            "counting_pass_peak_bytes": message.counting_pass_peak_bytes,
            "stdout_bytes": message.stdout_bytes,
            "stages": asdict(message.stages),
        }
    if isinstance(message, CachePublicationRequested):
        return {**base, "tag": "cache-publication-requested", "task_id": message.task_id}
    if isinstance(message, CompilerLaunchEvent):
        if message.worker_index is None or message.generation is None:
            raise AuditInfrastructureError("audit compiler launch frame is invalid")
        return {
            **base,
            "tag": "compiler-launch",
            "task_id": message.task_id,
            "purpose": message.purpose.value,
            "process_start": asdict(message.process_start),
        }
    raise AuditInfrastructureError("worker control message is invalid")


def encode_control_message(message) -> bytes:
    try:
        payload = json.dumps(
            _control_document(message),
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise AuditInfrastructureError("worker control message is invalid") from error
    if len(payload) > CONTROL_MAX_BYTES:
        raise AuditInfrastructureError("worker control frame exceeds limit")
    return payload


def _decode_stage_timings(value: object) -> WorkerStageTimings:
    if not isinstance(value, dict) or set(value) != {
        "discovery_seconds", "accepted_parse_seconds", "audit_seconds",
        "publish_seconds",
    }:
        raise AuditInfrastructureError("worker stage timings are invalid")
    return WorkerStageTimings(**value)


def decode_control_message(payload: bytes):
    if not isinstance(payload, bytes) or len(payload) > CONTROL_MAX_BYTES:
        raise AuditInfrastructureError("worker control frame exceeds limit")
    try:
        value = json.loads(payload.decode("ascii"))
        if (
            not isinstance(value, dict)
            or json.dumps(value, ensure_ascii=True, sort_keys=True,
                          separators=(",", ":")).encode("ascii") != payload
        ):
            raise ValueError("noncanonical")
        tag = value.get("tag")
        worker_index = _bounded_wire_int(
            value.get("worker_index"), "worker control index", (1 << 32) - 1
        )
        generation = _bounded_wire_int(
            value.get("generation"), "worker control generation", (1 << 64) - 1
        )
        common = {"worker_index": worker_index, "generation": generation}
        if tag == "worker-stop" and set(value) == {
            "tag", "worker_index", "generation"
        }:
            return WorkerStop(**common)
        if tag == "worker-stopped" and set(value) == {
            "tag", "worker_index", "generation"
        }:
            return WorkerStopped(**common)
        if tag == "worker-retire-ack" and set(value) == {
            "tag", "worker_index", "generation"
        }:
            return WorkerRetireAck(**common)
        if tag == "worker-retire" and set(value) == {
            "tag", "worker_index", "generation", "reason"
        }:
            return WorkerRetire(**common, reason=value["reason"])
        if tag == "worker-failure" and set(value) == {
            "tag", "worker_index", "generation", "task_id", "diagnostic"
        }:
            return WorkerFailure(
                **common, task_id=value["task_id"], diagnostic=value["diagnostic"]
            )
        if tag == "worker-engine-ready" and set(value) == {
            "tag", "worker_index", "generation", "worker_pid",
            "audit_engine_fingerprint", "capability_digests",
        }:
            digests = value["capability_digests"]
            if not isinstance(digests, list):
                raise ValueError("digests")
            return WorkerEngineReady(
                **common,
                worker_pid=value["worker_pid"],
                audit_engine_fingerprint=value["audit_engine_fingerprint"],
                capability_digests=tuple(digests),
            )
        if tag == "worker-capabilities-accepted" and set(value) == {
            "tag", "worker_index", "generation", "capability_digests",
        }:
            digests = value["capability_digests"]
            if not isinstance(digests, list):
                raise ValueError("digests")
            return WorkerCapabilitiesAccepted(
                **common, capability_digests=tuple(digests)
            )
        if tag == "worker-contained" and set(value) == {
            "tag", "worker_index", "generation", "worker_pid",
            "containment_identity",
        }:
            return WorkerContained(
                **common,
                worker_pid=value["worker_pid"],
                containment_identity=value["containment_identity"],
            )
        if tag == "macos-worker-session-reported" and set(value) == {
            "tag", "worker_index", "generation", "pid", "pgid",
            "bsd_start_identity",
        }:
            return MacOSWorkerSessionReported(
                **common,
                pid=value["pid"],
                pgid=value["pgid"],
                bsd_start_identity=value["bsd_start_identity"],
            )
        if tag == "macos-compiler-pgid-reported" and set(value) == {
            "tag", "worker_index", "generation", "task_id", "purpose",
            "pid", "pgid", "bsd_start_identity", "executable_identity",
            "executable_sha256", "driver_fingerprint",
        }:
            return CompilerPgidReported(
                **common,
                task_id=value["task_id"],
                purpose=CompilerLaunchPurpose(value["purpose"]),
                pid=value["pid"],
                pgid=value["pgid"],
                bsd_start_identity=value["bsd_start_identity"],
                executable_identity=_identity_from_wire_document(
                    value["executable_identity"]
                ),
                executable_sha256=value["executable_sha256"],
                driver_fingerprint=value["driver_fingerprint"],
            )
        if tag == "macos-compiler-exec-permit" and set(value) == {
            "tag", "worker_index", "generation", "task_id", "pgid",
        }:
            return CompilerExecPermit(
                **common, task_id=value["task_id"], pgid=value["pgid"]
            )
        if tag == "worker-payload-permit" and set(value) == {
            "tag", "worker_index", "generation", "task_id"
        }:
            return WorkerPayloadPermit(**common, task_id=value["task_id"])
        if tag == "worker-payload-ready" and set(value) == {
            "tag", "worker_index", "generation", "task_id", "encoded_bytes",
            "encoded_sha256", "charged_bytes", "conservative_decoded_bytes",
            "conservative_retained_bytes", "counting_pass_peak_bytes",
            "stdout_bytes", "stages", "configuration_digest",
            "audit_engine_fingerprint", "pipe_nonce", "serial", "nonce",
        }:
            return WorkerPayloadReady(
                **common,
                task_id=value["task_id"],
                configuration_digest=value["configuration_digest"],
                audit_engine_fingerprint=value["audit_engine_fingerprint"],
                pipe_nonce=value["pipe_nonce"],
                serial=value["serial"],
                nonce=value["nonce"],
                encoded_bytes=value["encoded_bytes"],
                encoded_sha256=value["encoded_sha256"],
                charged_bytes=value["charged_bytes"],
                conservative_decoded_bytes=value[
                    "conservative_decoded_bytes"
                ],
                conservative_retained_bytes=value[
                    "conservative_retained_bytes"
                ],
                counting_pass_peak_bytes=value["counting_pass_peak_bytes"],
                stdout_bytes=value["stdout_bytes"],
                stages=_decode_stage_timings(value["stages"]),
            )
        if tag == "cache-publication-requested" and set(value) == {
            "tag", "worker_index", "generation", "task_id"
        }:
            return CachePublicationRequested(**common, task_id=value["task_id"])
        if tag == "compiler-launch" and set(value) == {
            "tag", "worker_index", "generation", "task_id", "purpose",
            "process_start",
        }:
            start = value["process_start"]
            if not isinstance(start, dict) or set(start) != {
                "platform_kind", "pid", "native_start_token", "handle_cookie"
            }:
                raise ValueError("process start")
            return CompilerLaunchEvent(
                CompilerLaunchPurpose(value["purpose"]),
                ProcessStartIdentity(**start),
                worker_index=worker_index,
                task_id=value["task_id"],
                generation=generation,
            )
    except AuditInfrastructureError:
        raise

    except (AttributeError, KeyError, TypeError, ValueError, UnicodeError,
            json.JSONDecodeError) as error:
        raise AuditInfrastructureError("worker control frame is invalid") from error
    raise AuditInfrastructureError("worker control frame is invalid")


def _identity_wire_document(identity: FileIdentity) -> dict[str, object]:
    if not isinstance(identity, FileIdentity):
        raise AuditInfrastructureError("task source identity is invalid")
    return {
        "canonical": str(identity.canonical),
        "relative": (
            None if identity.relative is None else identity.relative.as_posix()
        ),
        "device": identity.device,
        "inode": identity.inode,
        "line_count": identity.line_count,
        "production": identity.production,
    }


def _identity_from_wire_document(value: object) -> FileIdentity:
    if not isinstance(value, dict) or set(value) != {
        "canonical", "relative", "device", "inode", "line_count", "production"
    }:
        raise AuditInfrastructureError("task source identity is invalid")
    relative = value["relative"]
    if relative is not None and not isinstance(relative, str):
        raise AuditInfrastructureError("task source identity is invalid")
    return FileIdentity(
        Path(value["canonical"]),
        None if relative is None else PurePosixPath(relative),
        value["device"],
        value["inode"],
        value["line_count"],
        value["production"],
    )


def _transport_capability_document(
    capability: CompactResultTransportCapability,
) -> dict[str, object]:
    if not isinstance(capability, CompactResultTransportCapability):
        raise AuditInfrastructureError("worker transport capability is invalid")
    return {
        "task_id": capability.task_id,
        "generation": capability.generation,
        "configuration_digest": capability.configuration_digest,
        "audit_engine_fingerprint": capability.audit_engine_fingerprint,
        "worker_slot": capability.worker_slot,
        "pipe_nonce": capability.pipe_nonce,
        "maximum_bytes": capability.maximum_bytes,
        "serial": capability.serial,
        "nonce": capability.nonce,
    }


def encode_task_frame(
    ordinal: int,
    worker_index: int,
    task: ConfigurationAuditTask,
    transport_capability: CompactResultTransportCapability,
) -> bytes:
    if (
        not isinstance(task, ConfigurationAuditTask)
        or task.compact_reservation is None
        or not isinstance(transport_capability, CompactResultTransportCapability)
        or transport_capability.task_id != task.task_id
        or transport_capability.generation != task.generation
        or transport_capability.worker_slot != worker_index
    ):
        raise AuditInfrastructureError("worker task frame is invalid")
    _bounded_wire_int(ordinal, "worker task ordinal", (1 << 32) - 1)
    _bounded_wire_int(worker_index, "worker task index", (1 << 32) - 1)
    configuration = task.configuration
    document = {
        "tag": "worker-task",
        "ordinal": ordinal,
        "worker_index": worker_index,
        "generation": task.generation,
        "task_id": task.task_id,
        "dependency_root_authority_digest": (
            task.dependency_root_authority.portable_authority_digest
        ),
        "configuration": {
            "entry_id": configuration.entry_id,
            "family": configuration.family.value,
            "compiler": str(configuration.compiler),
            "working_directory": str(configuration.working_directory),
            "source": _identity_wire_document(configuration.source),
            "arguments": list(configuration.arguments),
            "environment_digest": configuration.environment_digest,
            "digest": configuration.digest,
            "dependency_root_authority_digest": (
                configuration.dependency_root_authority_digest
            ),
            "compiler_capability_digest": (
                configuration.compiler_capability_digest
            ),
        },
        "transport_capability": _transport_capability_document(
            transport_capability
        ),
    }
    try:
        payload = json.dumps(
            document, ensure_ascii=True, sort_keys=True, separators=(",", ":")
        ).encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise AuditInfrastructureError("worker task frame is invalid") from error
    if len(payload) > TASK_MAX_BYTES:
        raise AuditInfrastructureError("worker task frame exceeds limit")
    return payload


def decode_task_frame(
    payload: bytes,
    expected_authority: DependencyRootAuthority,
    capability_registry: Mapping[str, object] | None = None,
):
    if not isinstance(payload, bytes) or len(payload) > TASK_MAX_BYTES:
        raise AuditInfrastructureError("worker task frame exceeds limit")
    if capability_registry is None:
        capability_registry = globals().get("_WORKER_CAPABILITIES")
    if not isinstance(capability_registry, Mapping):
        raise AuditInfrastructureError("worker capability registry is unavailable")
    authority = validate_dependency_root_authority(expected_authority)
    if authority is not expected_authority:
        raise AuditInfrastructureError(
            "worker dependency-root authority identity changed"
        )
    try:
        value = json.loads(payload.decode("ascii"))
        if json.dumps(
            value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
        ).encode("ascii") != payload:
            raise ValueError("noncanonical")
        if not isinstance(value, dict) or set(value) != {
            "tag", "ordinal", "worker_index", "generation", "task_id",
            "dependency_root_authority_digest", "configuration",
            "transport_capability",
        } or value["tag"] != "worker-task":
            raise ValueError("schema")
        ordinal = _bounded_wire_int(
            value["ordinal"], "worker task ordinal", (1 << 32) - 1
        )
        worker_index = _bounded_wire_int(
            value["worker_index"], "worker task index", (1 << 32) - 1
        )
        generation = _bounded_wire_int(
            value["generation"], "worker task generation", (1 << 64) - 1
        )
        authority_digest = value["dependency_root_authority_digest"]
        if authority_digest != authority.portable_authority_digest:
            raise AuditInfrastructureError(
                "worker dependency-root authority digest differs"
            )
        configuration_value = value["configuration"]
        if not isinstance(configuration_value, dict) or set(configuration_value) != {
            "entry_id", "family", "compiler", "working_directory", "source",
            "arguments", "environment_digest", "digest",
            "dependency_root_authority_digest", "compiler_capability_digest",
        }:
            raise ValueError("configuration")
        arguments = configuration_value["arguments"]
        if (
            not isinstance(arguments, list)
            or len(arguments) > 8192
            or any(not isinstance(argument, str) or "\0" in argument
                   or len(argument.encode("utf-8")) > (1 << 20)
                   for argument in arguments)
        ):
            raise AuditInfrastructureError("worker task compile arguments exceed limit")
        capability_digest = configuration_value["compiler_capability_digest"]
        compiler_capability = capability_registry.get(capability_digest)
        if compiler_capability is None or getattr(
            compiler_capability, "capability_digest", None
        ) != capability_digest:
            raise AuditInfrastructureError(
                "worker compiler capability digest differs"
            )
        configuration = PreprocessConfiguration(
            entry_id=configuration_value["entry_id"],
            family=CompilerFamily(configuration_value["family"]),
            compiler=Path(configuration_value["compiler"]),
            working_directory=Path(configuration_value["working_directory"]),
            source=_identity_from_wire_document(configuration_value["source"]),
            arguments=tuple(arguments),
            environment_digest=configuration_value["environment_digest"],
            digest=configuration_value["digest"],
            dependency_root_authority_digest=(
                configuration_value["dependency_root_authority_digest"]
            ),
            compiler_capability_digest=capability_digest,
            compiler_capability=compiler_capability,
        )
        try:
            source_relative = configuration.source.canonical.relative_to(
                authority.source_root.resolved_root
            )
        except ValueError as error:
            raise AuditInfrastructureError(
                "worker dependency-root authority source differs"
            ) from error
        if (
            configuration.source.relative is None
            or PurePosixPath(source_relative.as_posix())
            != configuration.source.relative
        ):
            raise AuditInfrastructureError(
                "worker dependency-root authority source differs"
            )
        trusted_binding = getattr(
            compiler_capability, "trusted_toolchain_root", None
        )
        if trusted_binding not in authority.external_roots:
            raise AuditInfrastructureError(
                "worker dependency-root authority capability differs"
            )
        capability_value = value["transport_capability"]
        if not isinstance(capability_value, dict) or set(capability_value) != {
            "task_id", "generation", "configuration_digest",
            "audit_engine_fingerprint", "worker_slot", "pipe_nonce",
            "maximum_bytes", "serial", "nonce",
        }:
            raise ValueError("transport capability")
        transport_capability = CompactResultTransportCapability(
            **capability_value
        )
        if (
            value["task_id"] != transport_capability.task_id
            or generation != transport_capability.generation
            or worker_index != transport_capability.worker_slot
            or configuration.digest
            != transport_capability.configuration_digest
            or configuration.dependency_root_authority_digest
            != authority.portable_authority_digest
        ):
            raise AuditInfrastructureError("worker task generation differs")
        reservation = PerTaskCompactReservation.for_worker_transport(
            transport_capability
        )
        task = ConfigurationAuditTask(
            value["task_id"], generation, configuration, authority, reservation
        )
        return ordinal, task, transport_capability
    except AuditInfrastructureError:
        raise
    except (KeyError, TypeError, ValueError, UnicodeError, json.JSONDecodeError) as error:
        raise AuditInfrastructureError("worker task frame is invalid") from error


def _dependency_wire_document(dependency: DependencyDigest) -> dict[str, object]:
    if not isinstance(dependency, DependencyDigest):
        raise AuditInfrastructureError("production snapshot dependency is invalid")
    return {
        "stable_role": dependency.stable_role,
        "role_relative_path": dependency.role_relative_path.as_posix(),
        "identity": _identity_wire_document(dependency.identity),
        "sha256": dependency.sha256,
    }


def _dependency_from_wire_document(value: object) -> DependencyDigest:
    if not isinstance(value, dict) or set(value) != {
        "stable_role", "role_relative_path", "identity", "sha256"
    }:
        raise AuditInfrastructureError("production snapshot dependency is invalid")
    return DependencyDigest(
        value["stable_role"],
        PurePosixPath(value["role_relative_path"]),
        _identity_from_wire_document(value["identity"]),
        value["sha256"],
    )


_CAPABILITY_FD_CHUNK = 64
_CAPABILITY_FD_HEADER_FORMAT = "!32sIII"


def _posix_capability_socket_type() -> socket.SocketKind:
    if sys.platform == "darwin":
        return socket.SOCK_DGRAM
    if sys.platform.startswith("linux"):
        return socket.SOCK_SEQPACKET
    raise AuditInfrastructureError(
        "worker compiler capability transfer platform is unsupported"
    )


def _validate_posix_capability_socket(endpoint: socket.socket) -> None:
    try:
        socket_type = endpoint.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE)
    except OSError as error:
        raise AuditInfrastructureError(
            "worker compiler capability transfer endpoint is invalid"
        ) from error
    if socket_type not in {socket.SOCK_DGRAM, socket.SOCK_SEQPACKET}:
        raise AuditInfrastructureError(
            "worker compiler capability transfer endpoint is not record-oriented"
        )


def _send_posix_capability_streams(
    endpoint: socket.socket,
    streams: tuple[object, ...],
    cookie: str,
    deadline: float,
    cancel_event,
) -> None:
    if os.name == "nt" or not isinstance(endpoint, socket.socket):
        raise AuditInfrastructureError(
            "worker compiler capability transfer endpoint is invalid"
        )
    _validate_posix_capability_socket(endpoint)
    chunks = max(1, math.ceil(len(streams) / _CAPABILITY_FD_CHUNK))
    endpoint.setblocking(False)
    selector = selectors.DefaultSelector()
    selector.register(endpoint, selectors.EVENT_WRITE)
    try:
        for chunk_index in range(chunks):
            first = chunk_index * _CAPABILITY_FD_CHUNK
            selected = streams[first:first + _CAPABILITY_FD_CHUNK]
            descriptors = array("i", (stream.fileno() for stream in selected))
            header = struct.pack(
                _CAPABILITY_FD_HEADER_FORMAT,
                bytes.fromhex(cookie), chunk_index, chunks, len(selected)
            )
            while True:
                if cancel_event.is_set():
                    raise AuditInfrastructureError(
                        "worker compiler capability transfer was cancelled"
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AuditInfrastructureError(
                        "worker compiler capability transfer deadline exceeded"
                    )
                if not selector.select(min(0.05, remaining)):
                    continue
                try:
                    sent = endpoint.sendmsg(
                        [header],
                        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, descriptors)],
                    )
                except BlockingIOError:
                    continue
                if sent != len(header):
                    raise AuditInfrastructureError(
                        "worker compiler capability transfer was truncated"
                    )
                break
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "worker compiler capability transfer failed"
        ) from error
    finally:
        selector.close()


def _receive_posix_capability_streams(
    endpoint: socket.socket,
    expected_count: int,
    cookie: str,
    deadline: float,
    cancel_event,
) -> tuple[object, ...]:
    if (
        os.name == "nt"
        or not isinstance(endpoint, socket.socket)
        or not isinstance(expected_count, int)
        or expected_count <= 0
    ):
        raise AuditInfrastructureError(
            "worker compiler capability receive is invalid"
        )
    expected_chunks = max(
        1, math.ceil(expected_count / _CAPABILITY_FD_CHUNK)
    )
    received_fds: list[int] = []
    streams: list[object] = []
    _validate_posix_capability_socket(endpoint)
    endpoint.setblocking(False)
    selector = selectors.DefaultSelector()
    selector.register(endpoint, selectors.EVENT_READ)
    try:
        for chunk_index in range(expected_chunks):
            while True:
                if cancel_event.is_set():
                    raise AuditInfrastructureError(
                        "worker compiler capability receive was cancelled"
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AuditInfrastructureError(
                        "worker compiler capability receive deadline exceeded"
                    )
                if not selector.select(min(0.05, remaining)):
                    continue
                try:
                    header, ancillary, flags, _address = endpoint.recvmsg(
                        struct.calcsize(_CAPABILITY_FD_HEADER_FORMAT),
                        socket.CMSG_SPACE(_CAPABILITY_FD_CHUNK * array("i").itemsize),
                    )
                except BlockingIOError:
                    continue
                break
            chunk_fds = array("i")
            ancillary_valid = True
            rights_messages = 0
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    rights_messages += 1
                    if len(payload) % chunk_fds.itemsize != 0:
                        ancillary_valid = False
                    else:
                        chunk_fds.frombytes(payload)
                else:
                    ancillary_valid = False
            chunk_descriptors = list(chunk_fds)
            try:
                if flags & (
                    getattr(socket, "MSG_TRUNC", 0)
                    | getattr(socket, "MSG_CTRUNC", 0)
                ):
                    raise AuditInfrastructureError(
                        "worker compiler capability receive was truncated"
                    )
                if len(header) != struct.calcsize(_CAPABILITY_FD_HEADER_FORMAT):
                    raise AuditInfrastructureError(
                        "worker compiler capability receive header differs"
                    )
                raw_cookie, observed_index, observed_chunks, observed_count = (
                    struct.unpack(_CAPABILITY_FD_HEADER_FORMAT, header)
                )
                expected_chunk_count = min(
                    _CAPABILITY_FD_CHUNK,
                    expected_count - chunk_index * _CAPABILITY_FD_CHUNK,
                )
                if (
                    raw_cookie.hex() != cookie
                    or observed_index != chunk_index
                    or observed_chunks != expected_chunks
                    or observed_count != expected_chunk_count
                ):
                    raise AuditInfrastructureError(
                        "worker compiler capability receive generation differs"
                    )
                if (
                    not ancillary_valid
                    or rights_messages != 1
                    or len(chunk_descriptors) != observed_count
                ):
                    raise AuditInfrastructureError(
                        "worker compiler capability receive count differs"
                    )
                for descriptor in chunk_descriptors:
                    os.set_inheritable(descriptor, False)
                received_fds.extend(chunk_descriptors)
                chunk_descriptors.clear()
            finally:
                for descriptor in chunk_descriptors:
                    try:
                        os.close(descriptor)
                    except OSError:
                        pass
        if len(received_fds) != expected_count:
            raise AuditInfrastructureError(
                "worker compiler capability receive total differs"
            )
        pending_fds = received_fds
        received_fds = []
        for index, descriptor in enumerate(pending_fds):
            try:
                streams.append(os.fdopen(descriptor, "rb", closefd=True))
            except BaseException:
                for pending_descriptor in pending_fds[index:]:
                    try:
                        os.close(pending_descriptor)
                    except OSError:
                        pass
                raise
        result = tuple(streams)
        streams.clear()
        return result
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "worker compiler capability receive failed"
        ) from error
    finally:
        selector.close()
        for stream in reversed(streams):
            try:
                stream.close()
            except BaseException:
                pass
        for descriptor in received_fds:
            try:
                os.close(descriptor)
            except OSError:
                pass


def _duplicate_windows_capability_streams(
    process,
    streams: tuple[object, ...],
) -> tuple[int, ...]:
    if os.name != "nt":
        raise AuditInfrastructureError(
            "worker compiler capability handle transfer is unavailable"
        )
    import _winapi
    import msvcrt

    handles = []
    target_process = None
    try:
        target_process = _winapi.OpenProcess(
            0x0040, False, int(process.pid)
        )
        for stream in streams:
            handles.append(_winapi.DuplicateHandle(
                _winapi.GetCurrentProcess(),
                msvcrt.get_osfhandle(stream.fileno()),
                target_process,
                0,
                False,
                _winapi.DUPLICATE_SAME_ACCESS,
            ))
        return tuple(int(handle) for handle in handles)
    except (AttributeError, OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "worker compiler capability handle transfer failed"
        ) from error
    finally:
        if target_process is not None:
            _winapi.CloseHandle(target_process)


def _windows_capability_streams(handles: tuple[int, ...]) -> tuple[object, ...]:
    if os.name != "nt" or not handles:
        raise AuditInfrastructureError(
            "worker compiler capability handles are invalid"
        )
    import msvcrt

    streams = []
    try:
        for handle in handles:
            descriptor = msvcrt.open_osfhandle(int(handle), os.O_RDONLY)
            streams.append(os.fdopen(descriptor, "rb", closefd=True))
        return tuple(streams)
    except (OSError, ValueError) as error:
        for stream in reversed(streams):
            stream.close()
        raise AuditInfrastructureError(
            "worker compiler capability handle receive failed"
        ) from error


def _generation_duplicate_stream_before_close(
    _duplicate, _stream_index: int
) -> None:
    return None


def _generation_duplicate_stream_close(
    _duplicate, _stream_index: int
) -> None:
    return None


def _generation_release_transition(
    _kind: str,
    _stage: str,
    _position: str,
    _worker_index: int,
    _generation: int,
) -> None:
    return None


def _macos_reconciliation_completed(
    _scope: str,
    _worker_index: int,
    _generation: int,
    _pgid: int,
) -> None:
    return None


class _GenerationCompilerCapabilityDuplicate:
    __slots__ = (
        "registry", "digest", "worker_index", "generation", "streams",
        "transferred_pid", "closed", "_closed_stream_indices",
    )

    def __init__(
        self,
        registry: "CompilerCapabilityRegistry",
        digest: str,
        worker_index: int,
        generation: int,
        streams: tuple[object, ...],
    ) -> None:
        self.registry = registry
        self.digest = digest
        self.worker_index = worker_index
        self.generation = generation
        self.streams = streams
        self.transferred_pid: int | None = None
        self.closed = False
        self._closed_stream_indices: set[int] = set()

    def close(self) -> None:
        if self.closed:
            return
        errors = []
        for stream_index in reversed(range(len(self.streams))):
            if stream_index in self._closed_stream_indices:
                continue
            stream = self.streams[stream_index]
            try:
                _generation_duplicate_stream_before_close(
                    self, stream_index
                )
                stream.close()
            except BaseException as error:
                if getattr(stream, "closed", False) is True:
                    self._closed_stream_indices.add(stream_index)
                errors.append(error)
                continue
            self._closed_stream_indices.add(stream_index)
            try:
                _generation_duplicate_stream_close(self, stream_index)
            except BaseException as error:
                errors.append(error)
        self.closed = len(self._closed_stream_indices) == len(self.streams)
        if errors:
            raise AuditInfrastructureError(
                "compiler capability generation duplicate cleanup failed"
            ) from errors[0]


class CompilerCapabilityRegistry:
    """Single parent owner for held compiler capabilities and transfers."""

    def __init__(self, dependency_roots: DependencyRootAuthority) -> None:
        self.dependency_roots = validate_dependency_root_authority(
            dependency_roots
        )
        self._capabilities: dict[str, CompilerExecutableCapability] = {}
        self._generation_duplicates: dict[
            tuple[int, int], dict[str, _GenerationCompilerCapabilityDuplicate]
        ] = {}
        self._acknowledged_generations: set[tuple[int, int]] = set()
        self._released_generations: set[tuple[int, int]] = set()
        self._acknowledgement_removal_incomplete: set[
            tuple[int, int]
        ] = set()
        self._closed = False
        self._lock = threading.Lock()

    def register(self, capability: CompilerExecutableCapability) -> str:
        validate_compiler_executable_capability(
            capability, self.dependency_roots
        )
        digest = capability.capability_digest
        with self._lock:
            if self._closed:
                raise AuditInfrastructureError(
                    "compiler capability registry is closed"
                )
            existing = self._capabilities.get(digest)
            if existing is None:
                self._capabilities[digest] = capability
            elif existing.native_owner is not capability.native_owner:
                raise AuditInfrastructureError(
                    "compiler capability registry owner differs"
                )
        return digest

    def duplicate_for_generation(
        self, digest: str, worker_index: int, generation: int
    ) -> _GenerationCompilerCapabilityDuplicate:
        if (
            not isinstance(digest, str)
            or not isinstance(worker_index, int)
            or isinstance(worker_index, bool)
            or worker_index < 0
            or not isinstance(generation, int)
            or isinstance(generation, bool)
            or generation < 0
        ):
            raise AuditInfrastructureError(
                "compiler capability generation duplicate is invalid"
            )
        key = (worker_index, generation)
        with self._lock:
            if (
                self._closed
                or key in self._acknowledged_generations
                or key in self._released_generations
            ):
                raise AuditInfrastructureError(
                    "compiler capability generation is unavailable"
                )
            capability = self._capabilities.get(digest)
            generation_duplicates = self._generation_duplicates.setdefault(
                key, {}
            )
            if capability is None or digest in generation_duplicates:
                raise AuditInfrastructureError(
                    "compiler capability generation duplicate differs"
                )
            streams = []
            try:
                for source in capability.native_owner.streams:
                    descriptor = os.dup(source.fileno())
                    os.set_inheritable(descriptor, False)
                    streams.append(os.fdopen(descriptor, "rb", closefd=True))
            except BaseException:
                for stream in reversed(streams):
                    stream.close()
                raise
            duplicate = _GenerationCompilerCapabilityDuplicate(
                self, digest, worker_index, generation, tuple(streams)
            )
            generation_duplicates[digest] = duplicate
            return duplicate

    def transfer_duplicate_to_child(
        self,
        duplicate: _GenerationCompilerCapabilityDuplicate,
        process,
    ) -> None:
        pid = getattr(process, "pid", None)
        with self._lock:
            actual = self._generation_duplicates.get(
                (duplicate.worker_index, duplicate.generation), {}
            ).get(duplicate.digest)
            if (
                actual is not duplicate
                or duplicate.registry is not self
                or duplicate.closed
                or duplicate.transferred_pid is not None
                or not isinstance(pid, int)
                or isinstance(pid, bool)
                or pid <= 0
            ):
                raise AuditInfrastructureError(
                    "compiler capability generation transfer differs"
                )
            duplicate.transferred_pid = pid

    def acknowledge_generation(
        self,
        worker_index: int,
        generation: int,
        capability_digests: tuple[str, ...],
    ) -> None:
        key = (worker_index, generation)
        with self._lock:
            duplicates = self._generation_duplicates.get(key)
            if (
                duplicates is None
                and key in self._acknowledged_generations
                and key in self._acknowledgement_removal_incomplete
            ):
                self._acknowledgement_removal_incomplete.discard(key)
                return
            if (
                duplicates is None
                or capability_digests != tuple(sorted(duplicates))
                or any(
                    duplicate.transferred_pid is None
                    for duplicate in duplicates.values()
                )
            ):
                raise AuditInfrastructureError(
                    "compiler capability generation acknowledgement differs"
                )
            if key not in self._acknowledged_generations:
                errors = []
                for digest in reversed(sorted(duplicates)):
                    try:
                        duplicates[digest].close()
                    except BaseException as error:
                        errors.append(error)
                if errors:
                    raise AuditInfrastructureError(
                        "compiler capability generation acknowledgement cleanup failed"
                    ) from errors[0]
                _generation_release_transition(
                    "acknowledge", "terminal", "before", *key
                )
                self._acknowledged_generations.add(key)
                _generation_release_transition(
                    "acknowledge", "terminal", "after", *key
                )
            if key in self._generation_duplicates:
                _generation_release_transition(
                    "acknowledge", "remove", "before", *key
                )
                self._acknowledgement_removal_incomplete.add(key)
                del self._generation_duplicates[key]
                _generation_release_transition(
                    "acknowledge", "remove", "after", *key
                )
                self._acknowledgement_removal_incomplete.discard(key)

    def release_generation(self, worker_index: int, generation: int) -> None:
        key = (worker_index, generation)
        with self._lock:
            if (
                key in self._released_generations
                and key not in self._generation_duplicates
                and key not in self._acknowledged_generations
            ):
                return
            duplicates = self._generation_duplicates.get(key)
            acknowledged = key in self._acknowledged_generations
            if key not in self._released_generations:
                if duplicates is None and not acknowledged:
                    raise AuditInfrastructureError(
                        "compiler capability generation is unavailable"
                    )
                errors = []
                if duplicates is not None:
                    for digest in reversed(sorted(duplicates)):
                        try:
                            duplicates[digest].close()
                        except BaseException as error:
                            errors.append(error)
                if errors:
                    raise AuditInfrastructureError(
                        "compiler capability generation release failed"
                    ) from errors[0]
                _generation_release_transition(
                    "release", "terminal", "before", *key
                )
                self._released_generations.add(key)
                _generation_release_transition(
                    "release", "terminal", "after", *key
                )
            if (
                key in self._generation_duplicates
                or key in self._acknowledged_generations
            ):
                _generation_release_transition(
                    "release", "remove", "before", *key
                )
                self._generation_duplicates.pop(key, None)
                self._acknowledged_generations.discard(key)
                _generation_release_transition(
                    "release", "remove", "after", *key
                )

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            if self._generation_duplicates or self._acknowledged_generations:
                raise AuditInfrastructureError(
                    "compiler capability registry has live generations"
                )
            self._closed = True
            errors = []
            for duplicates in self._generation_duplicates.values():
                for duplicate in duplicates.values():
                    try:
                        duplicate.close()
                    except BaseException as error:
                        errors.append(error)
            owners = []
            seen = set()
            for capability in self._capabilities.values():
                owner = capability.native_owner
                if id(owner) not in seen:
                    seen.add(id(owner))
                    owners.append(owner)
            self._capabilities.clear()
            for owner in reversed(owners):
                try:
                    owner.close()
                except BaseException as error:
                    errors.append(error)
            if errors:
                raise AuditInfrastructureError(
                    "compiler capability registry cleanup failed"
                ) from errors[0]


def _encode_worker_bootstrap(
    configurations: tuple[PreprocessConfiguration, ...],
    authority: DependencyRootAuthority,
    production_snapshot,
    limits: AuditLimits,
    engine: str,
    deadline: float,
    *,
    transfer_cookie: str,
    transferred_handles: Mapping[str, tuple[int, ...]],
) -> bytes:
    capabilities: dict[str, dict[str, object]] = {}
    transfer_owners: dict[str, _CompilerCapabilityOwner] = {}
    for configuration in configurations:
        owner = configuration.compiler_capability.native_owner
        digest = configuration.compiler_capability_digest
        existing = transfer_owners.setdefault(digest, owner)
        if existing is not owner:
            raise AuditInfrastructureError(
                "compiler capability bootstrap owner differs"
            )
    transfer_offsets: dict[str, int] = {}
    transfer_count = 0
    for digest in sorted(transfer_owners):
        transfer_offsets[digest] = transfer_count
        transfer_count += len(transfer_owners[digest].streams)
    for configuration in configurations:
        capability = configuration.compiler_capability
        owner = capability.native_owner
        if not isinstance(owner, _CompilerCapabilityOwner):
            raise AuditInfrastructureError(
                "compiler capability bootstrap owner is invalid"
            )
        document = {
            "digest": configuration.compiler_capability_digest,
            "platform_kind": capability.platform_kind,
            "executable_identity": _identity_wire_document(
                capability.executable_identity
            ),
            "executable_sha256": capability.executable_sha256,
            "trusted_toolchain_role": (
                capability.trusted_toolchain_root.stable_role
            ),
            "directory_chain_owners": [
                str(path) for path in capability.directory_chain_owners
            ],
            "directory_chain_identities": [
                _identity_wire_document(identity)
                for identity in capability.directory_chain_identities
            ],
            "resolved_runtime_closure_digest": (
                capability.resolved_runtime_closure_digest
            ),
            "resolved_runtime_closure": [
                _dependency_wire_document(dependency)
                for dependency in capability.resolved_runtime_closure
            ],
            "owner": {
                "file_paths": [str(path) for path in owner.file_paths],
                "file_snapshots": [list(value) for value in owner.file_snapshots],
                "file_hashes": list(owner.file_hashes),
                "alias_paths": [str(path) for path in owner.alias_paths],
                "alias_snapshots": [list(value) for value in owner.alias_snapshots],
                "directory_paths": [str(path) for path in owner.directory_paths],
                "directory_snapshots": [
                    list(value) for value in owner.directory_snapshots
                ],
                "macos_shared_cache_uuid": owner.macos_shared_cache_uuid,
                "transfer_offset": transfer_offsets.get(
                    configuration.compiler_capability_digest,
                    0,
                ),
                "transfer_count": len(owner.streams),
                "transferred_handles": list(transferred_handles.get(
                    configuration.compiler_capability_digest, ()
                )),
            },
        }
        digest = configuration.compiler_capability_digest
        existing = capabilities.setdefault(digest, document)
        if existing != document:
            raise AuditInfrastructureError(
                "compiler capability bootstrap differs"
            )
    if not hasattr(production_snapshot, "items"):
        raise AuditInfrastructureError("production snapshot generation is invalid")
    snapshot = []
    for raw_path, dependency in production_snapshot.items():
        path = (
            raw_path
            if isinstance(raw_path, PurePosixPath)
            else PurePosixPath(str(raw_path).replace("\\", "/"))
        )
        if path != dependency.role_relative_path:
            raise AuditInfrastructureError(
                "production snapshot generation is invalid"
            )
        snapshot.append(_dependency_wire_document(dependency))
    document = {
        "tag": "worker-bootstrap",
        "authority": base64.b64encode(
            encode_dependency_root_authority(authority)
        ).decode("ascii"),
        "capabilities": [capabilities[key] for key in sorted(capabilities)],
        "production_snapshot": sorted(
            snapshot, key=lambda value: value["role_relative_path"]
        ),
        "limits": asdict(limits),
        "audit_engine_fingerprint": engine,
        "pipeline_deadline": deadline,
        "capability_transfer_cookie": transfer_cookie,
        "capability_transfer_count": transfer_count,
    }
    payload = json.dumps(
        document, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    if len(payload) > TASK_MAX_BYTES:
        raise AuditInfrastructureError("worker startup bootstrap exceeds limit")
    return payload


def _decode_worker_bootstrap(payload: bytes):
    if not isinstance(payload, bytes) or len(payload) > TASK_MAX_BYTES:
        raise AuditInfrastructureError("worker startup bootstrap exceeds limit")
    try:
        value = json.loads(payload.decode("ascii"))
        if json.dumps(
            value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
        ).encode("ascii") != payload:
            raise ValueError("noncanonical")
        if not isinstance(value, dict) or set(value) != {
            "tag", "authority", "capabilities", "production_snapshot",
            "limits", "audit_engine_fingerprint", "pipeline_deadline",
            "capability_transfer_cookie", "capability_transfer_count",
        } or value["tag"] != "worker-bootstrap":
            raise ValueError("schema")
        deadline = value["pipeline_deadline"]
        if (
            not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or not math.isfinite(deadline)
            or time.monotonic() >= deadline
        ):
            raise AuditInfrastructureError("pipeline deadline exceeded during startup")
        authority = decode_dependency_root_authority(
            base64.b64decode(value["authority"], validate=True)
        )
        limits_value = value["limits"]
        if not isinstance(limits_value, dict):
            raise ValueError("limits")
        limits = AuditLimits(**limits_value)
        snapshot_values = value["production_snapshot"]
        if not isinstance(snapshot_values, list):
            raise ValueError("snapshot")
        snapshot: dict[PurePosixPath, DependencyDigest] = {}
        for item in snapshot_values:
            dependency = _dependency_from_wire_document(item)
            if dependency.role_relative_path in snapshot:
                raise AuditInfrastructureError(
                    "production snapshot generation is ambiguous"
                )
            snapshot[dependency.role_relative_path] = dependency
        capabilities = value["capabilities"]
        if not isinstance(capabilities, list):
            raise ValueError("capabilities")
        engine = value["audit_engine_fingerprint"]
        transfer_cookie = value["capability_transfer_cookie"]
        transfer_count = value["capability_transfer_count"]
        if (
            not isinstance(engine, str)
            or not isinstance(transfer_cookie, str)
            or len(transfer_cookie) != 64
            or any(byte not in "0123456789abcdef" for byte in transfer_cookie)
            or not isinstance(transfer_count, int)
            or isinstance(transfer_count, bool)
            or transfer_count <= 0
        ):
            raise ValueError("engine")
        return (
            authority,
            tuple(capabilities),
            snapshot,
            limits,
            engine,
            float(deadline),
            transfer_cookie,
            transfer_count,
        )
    except AuditInfrastructureError:
        raise
    except (KeyError, TypeError, ValueError, UnicodeError, json.JSONDecodeError) as error:
        raise AuditInfrastructureError("worker startup bootstrap is invalid") from error


class _GenerationWorkerControlEndpoint:
    def __init__(
        self, worker_index: int, generation: int,
        sender: BoundedFrameChannel, deadline: float,
    ) -> None:
        self.worker_index = worker_index
        self.generation = generation
        self.sender = sender
        self.deadline = deadline
        self.active_carriers: dict[ProcessStartIdentity, object] = {}
        self.launches: dict[str, list[CompilerLaunchEvent]] = {}
        self.publication_contexts: dict[
            str, tuple[str, str, tuple[DependencyDigest, ...]]
        ] = {}

    def send(self, frame, deadline: float) -> None:
        if isinstance(frame, CompilerLaunchEvent):
            self.launches.setdefault(frame.task_id or "", []).append(frame)
        self.sender.send_bytes_before(
            encode_control_message(frame), min(deadline, self.deadline),
            cancel_event=_WORKER_CANCEL_EVENT,
        )

    def register_compiler_process_launch(self, event, carrier) -> None:
        if event.process_start in self.active_carriers:
            raise AuditInfrastructureError(
                "compiler process carrier was registered twice"
            )
        self.active_carriers[event.process_start] = carrier

    def complete_compiler_process_launch(self, event, carrier) -> None:
        if self.active_carriers.pop(event.process_start, None) is not carrier:
            raise AuditInfrastructureError("compiler process carrier differs")

    def fail_compiler_process_launch(self, event, carrier) -> None:
        if self.active_carriers.get(event.process_start) is carrier:
            self.active_carriers.pop(event.process_start)

    def seal_audit_launch_protocol(
        self, task_id: str, generation: int, deadline: float
    ) -> None:
        if generation != self.generation or time.monotonic() >= min(
            deadline, self.deadline
        ):
            raise AuditInfrastructureError(
                "audit compiler launch protocol generation differs"
            )
        events = self.launches.get(task_id, [])
        if tuple(event.purpose for event in events) != (
            CompilerLaunchPurpose.AUDIT_DISCOVERY,
            CompilerLaunchPurpose.AUDIT_ACCEPTED,
        ):
            raise AuditInfrastructureError("audit compiler launch protocol differs")

    def set_publication_context(
        self, task_id: str, generation: int, configuration_digest: str,
        engine: str, dependencies: tuple[DependencyDigest, ...],
    ) -> None:
        if generation != self.generation or task_id in self.publication_contexts:
            raise AuditInfrastructureError(
                "root publication request generation differs"
            )
        self.publication_contexts[task_id] = (
            configuration_digest, engine, dependencies
        )


class _GenerationWorkerCommandEndpoint:
    def __init__(
        self, control: _GenerationWorkerControlEndpoint,
        receiver: BoundedFrameChannel,
    ) -> None:
        self.control = control
        self.receiver = receiver

    def receive(
        self, task: ConfigurationAuditTask, cancel_event, deadline: float,
        maximum_quantum_seconds: float,
    ) -> CachePublicationPermit:
        payload = self.receiver.receive_bytes_before(
            deadline, cancel_event=cancel_event
        )
        message = decode_control_message(payload)
        if not isinstance(message, WorkerPayloadPermit) or (
            message.worker_index != self.control.worker_index
            or message.generation != self.control.generation
            or message.task_id != task.task_id
        ):
            raise AuditInfrastructureError(
                "root publication permit generation differs"
            )
        context = self.control.publication_contexts.pop(task.task_id, None)
        if context is None:
            raise AuditInfrastructureError(
                "root publication context is unavailable"
            )
        configuration_digest, engine, dependencies = context
        return CachePublicationPermit(
            configuration_digest,
            engine,
            dependencies,
            task_id=task.task_id,
            generation=task.generation,
        )

    def receive_macos_exec_permit(
        self, task: ConfigurationAuditTask, task_ordinal: int,
        expected_pgid: int, cancel_event, deadline: float,
    ) -> CompilerExecPermit:
        payload = self.receiver.receive_bytes_before(
            deadline, cancel_event=cancel_event
        )
        permit = decode_control_message(payload)
        if (
            not isinstance(permit, CompilerExecPermit)
            or permit.worker_index != self.control.worker_index
            or permit.generation != self.control.generation
            or permit.task_id != task_ordinal
            or permit.pgid != expected_pgid
        ):
            raise AuditInfrastructureError(
                "macOS compiler exec permit generation differs"
            )
        return permit


def _compiler_capability_from_bootstrap(
    document: object,
    authority: DependencyRootAuthority,
    deadline: float,
    cancel_event,
    transferred_streams: tuple[object, ...],
) -> CompilerExecutableCapability:
    if not isinstance(document, dict) or set(document) != {
        "digest", "platform_kind", "executable_identity", "executable_sha256",
        "trusted_toolchain_role", "directory_chain_owners",
        "directory_chain_identities", "resolved_runtime_closure_digest",
        "resolved_runtime_closure", "owner",
    }:
        raise AuditInfrastructureError(
            "worker compiler capability bootstrap is invalid"
        )
    owner_document = document["owner"]
    if not isinstance(owner_document, dict) or set(owner_document) != {
        "file_paths", "file_snapshots", "file_hashes", "alias_paths",
        "alias_snapshots", "directory_paths", "directory_snapshots",
        "macos_shared_cache_uuid", "transfer_offset", "transfer_count",
        "transferred_handles",
    }:
        raise AuditInfrastructureError(
            "worker compiler capability bootstrap is invalid"
        )

    def paths(name: str) -> tuple[Path, ...]:
        values = owner_document[name]
        if not isinstance(values, list) or any(
            not isinstance(value, str) for value in values
        ):
            raise AuditInfrastructureError(
                "worker compiler capability bootstrap is invalid"
            )
        return tuple(Path(value) for value in values)

    def tuples(name: str) -> tuple[tuple[object, ...], ...]:
        values = owner_document[name]
        if not isinstance(values, list) or any(
            not isinstance(value, list) for value in values
        ):
            raise AuditInfrastructureError(
                "worker compiler capability bootstrap is invalid"
            )
        return tuple(tuple(value) for value in values)

    file_paths = paths("file_paths")
    alias_paths = paths("alias_paths")
    directory_paths = paths("directory_paths")
    file_snapshots = tuples("file_snapshots")
    alias_snapshots = tuples("alias_snapshots")
    directory_snapshots = tuples("directory_snapshots")
    file_hashes_value = owner_document["file_hashes"]
    if (
        not file_paths
        or not isinstance(file_hashes_value, list)
        or any(not isinstance(value, str) for value in file_hashes_value)
        or len(file_paths) != len(file_snapshots)
        or len(file_paths) != len(file_hashes_value)
        or len(alias_paths) != len(alias_snapshots)
        or len(directory_paths) != len(directory_snapshots)
        or not isinstance(transferred_streams, tuple)
        or len(transferred_streams) != len(file_paths)
    ):
        raise AuditInfrastructureError(
            "worker compiler capability bootstrap is invalid"
        )
    bindings = tuple(
        binding for binding in authority.external_roots
        if binding.stable_role == document["trusted_toolchain_role"]
    )
    if len(bindings) != 1:
        raise AuditInfrastructureError(
            "worker compiler capability root differs"
        )
    streams = list(transferred_streams)
    native_owner = None
    observer = None
    try:
        for stream, _path, expected in zip(
            streams, file_paths, file_snapshots
        ):
            if time.monotonic() >= deadline or cancel_event.is_set():
                raise AuditInfrastructureError(
                    "worker compiler capability transfer deadline exceeded"
                )
            metadata = os.fstat(stream.fileno())
            observed = (
                int(metadata.st_dev),
                int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
                int(metadata.st_size),
                int(metadata.st_mtime_ns),
                int(getattr(metadata, "st_ctime_ns", 0)),
                int(metadata.st_mode),
            )
            if observed[:4] != expected[:4]:
                raise AuditInfrastructureError(
                    "worker compiler capability transferred identity differs"
                )
        platform_kind = document["platform_kind"]
        if platform_kind == "macos":
            observer = _FilesystemGenerationObserver(
                tuple((path, True) for path in directory_paths)
                + tuple((path, False) for path in alias_paths)
                + tuple((path, False) for path in file_paths)
            )
        native_owner = _CompilerCapabilityOwner(
            tuple(streams),
            file_paths,
            file_snapshots,
            tuple(file_hashes_value),
            alias_paths,
            alias_snapshots,
            directory_paths,
            directory_snapshots,
            observer,
            authority,
            macos_shared_cache_uuid=owner_document[
                "macos_shared_cache_uuid"
            ],
            validate_paths=platform_kind == "macos",
        )
        observer = None
        capability = CompilerExecutableCapability(
            document["platform_kind"],
            _identity_from_wire_document(document["executable_identity"]),
            document["executable_sha256"],
            document["digest"],
            native_owner,
            bindings[0],
            tuple(Path(value) for value in document["directory_chain_owners"]),
            tuple(
                _identity_from_wire_document(value)
                for value in document["directory_chain_identities"]
            ),
            document["resolved_runtime_closure_digest"],
            tuple(
                _dependency_from_wire_document(value)
                for value in document["resolved_runtime_closure"]
            ),
        )
        validate_compiler_executable_capability(
            capability,
            authority,
            deadline=deadline,
            cancel_event=cancel_event,
        )
        return capability
    except BaseException:
        if native_owner is not None:
            try:
                native_owner.close()
            except BaseException:
                pass
        if native_owner is None:
            if observer is not None:
                try:
                    observer.close()
                except BaseException:
                    pass
            for stream in reversed(streams):
                try:
                    stream.close()
                except BaseException:
                    pass
        raise

def _audit_worker_generation_main(
    worker_index: int,
    generation: int,
    startup_receiver: BoundedFrameChannel,
    task_receiver: BoundedFrameChannel,
    command_receiver: BoundedFrameChannel,
    event_sender: BoundedFrameChannel,
    payload_sender,
    capability_transfer_receiver,
    native_containment,
    worker_scratch_root: str,
    cancel_event,
    cache_root: str,
    pipeline_deadline: float,
    maximum_tasks: int,
    recycle_rss_bytes: int,
) -> None:
    global _WORKER_INDEX, _WORKER_GENERATION, _WORKER_PRODUCTION
    global _WORKER_LIMITS, _WORKER_CANCEL_EVENT, _WORKER_ENGINE
    global _WORKER_CACHE, _WORKER_RSS, _WORKER_CAPABILITIES
    global _WORKER_MAXIMUM_TASKS, _WORKER_RECYCLE_RSS_BYTES
    global _WORKER_PREATTESTED_ENGINE

    capabilities: dict[str, object] = {}
    current_task_id: str | None = None
    control = _GenerationWorkerControlEndpoint(
        worker_index, generation, event_sender, pipeline_deadline
    )
    if sys.platform.startswith("linux"):
        if not isinstance(native_containment, LinuxWorkerContainment):
            raise AuditInfrastructureError(
                "Linux worker generation containment is unavailable"
            )
        from gpu_capability_process_tree import acknowledge_linux_worker_containment

        acknowledge_linux_worker_containment(native_containment, os.getpid())
        control.send(
            WorkerContained(
                worker_index, generation, os.getpid(),
                f"linux:{os.getpid()}:{generation}",
            ),
            pipeline_deadline,
        )
    elif sys.platform == "darwin":
        os.setsid()

        class _SelfProcess:
            pid = os.getpid()

        start_identity = _native_process_start_token(
            _SelfProcess(), "macos"
        ).removeprefix("macos-proc:")
        control.send(
            MacOSWorkerSessionReported(
                worker_index, generation, os.getpid(), os.getpgrp(),
                start_identity,
            ),
            pipeline_deadline,
        )
    parent_temporary_root = tempfile.gettempdir()
    worker_temporary_root = str(Path(worker_scratch_root).resolve())
    if (
        not Path(worker_temporary_root).is_dir()
        or any(Path(worker_temporary_root).iterdir())
    ):
        raise AuditInfrastructureError(
            "worker scratch generation ownership differs"
        )
    tempfile.tempdir = worker_temporary_root
    try:
        bootstrap_payload = startup_receiver.receive_bytes_before(
            pipeline_deadline, cancel_event=cancel_event
        )
        (
            authority,
            capability_documents,
            production_snapshot,
            limits,
            expected_engine,
            decoded_deadline,
            transfer_cookie,
            transfer_count,
        ) = _decode_worker_bootstrap(bootstrap_payload)
        if decoded_deadline != pipeline_deadline:
            raise AuditInfrastructureError(
                "worker startup deadline differs"
            )
        if os.name == "nt":
            control.send(
                WorkerContained(
                    worker_index,
                    generation,
                    os.getpid(),
                    f"windows:{os.getpid()}:{generation}",
                ),
                pipeline_deadline,
            )
        from gpu_capability_source_audit import _attest_loaded_audit_engine

        engine = _attest_loaded_audit_engine(expected_engine)
        _WORKER_PREATTESTED_ENGINE = engine
        if os.name == "nt":
            transferred_streams = ()
        else:
            transferred_streams = _receive_posix_capability_streams(
                capability_transfer_receiver,
                transfer_count,
                transfer_cookie,
                pipeline_deadline,
                cancel_event,
            )
        for document in capability_documents:
            owner_document = document.get("owner")
            if not isinstance(owner_document, dict):
                raise AuditInfrastructureError(
                    "worker compiler capability transfer metadata is invalid"
                )
            offset = owner_document.get("transfer_offset")
            count = owner_document.get("transfer_count")
            handles = owner_document.get("transferred_handles")
            if (
                not isinstance(offset, int)
                or isinstance(offset, bool)
                or offset < 0
                or not isinstance(count, int)
                or isinstance(count, bool)
                or count <= 0
                or not isinstance(handles, list)
                or any(
                    not isinstance(handle, int) or isinstance(handle, bool)
                    or handle <= 0
                    for handle in handles
                )
            ):
                raise AuditInfrastructureError(
                    "worker compiler capability transfer metadata is invalid"
                )
            if os.name == "nt":
                if len(handles) != count:
                    raise AuditInfrastructureError(
                        "worker compiler capability handle count differs"
                    )
                capability_streams = _windows_capability_streams(
                    tuple(handles)
                )
            else:
                if handles or offset + count > len(transferred_streams):
                    raise AuditInfrastructureError(
                        "worker compiler capability descriptor count differs"
                    )
                capability_streams = transferred_streams[offset:offset + count]
            capability = _compiler_capability_from_bootstrap(
                document,
                authority,
                pipeline_deadline,
                cancel_event,
                capability_streams,
            )
            if capability.capability_digest != document["digest"]:
                raise AuditInfrastructureError(
                    "worker compiler capability digest differs"
                )
            if document["digest"] in capabilities:
                raise AuditInfrastructureError(
                    "worker compiler capability bootstrap is duplicated"
                )
            capabilities[document["digest"]] = capability
        control.send(
            WorkerCapabilitiesAccepted(
                worker_index,
                generation,
                tuple(sorted(capabilities)),
            ),
            pipeline_deadline,
        )
        _WORKER_INDEX = worker_index
        _WORKER_GENERATION = generation
        _WORKER_PRODUCTION = {
            path: dependency.identity
            for path, dependency in production_snapshot.items()
            if dependency.stable_role == "production"
        }
        _WORKER_LIMITS = limits
        _WORKER_CANCEL_EVENT = cancel_event
        _WORKER_ENGINE = engine
        _WORKER_CACHE = ConfigurationAuditCache(Path(cache_root))
        _WORKER_RSS = _WorkerRssSampler()
        _WORKER_CAPABILITIES = MappingProxyType(capabilities)
        _WORKER_MAXIMUM_TASKS = maximum_tasks
        _WORKER_RECYCLE_RSS_BYTES = recycle_rss_bytes
        control.send(
            WorkerEngineReady(
                worker_index,
                generation,
                os.getpid(),
                engine,
                tuple(sorted(capabilities)),
            ),
            pipeline_deadline,
        )
        command = _GenerationWorkerCommandEndpoint(control, command_receiver)
        completed = 0
        while True:
            payload = task_receiver.receive_bytes_before(
                pipeline_deadline, cancel_event=cancel_event
            )
            try:
                document = json.loads(payload.decode("ascii"))
            except (UnicodeError, ValueError, json.JSONDecodeError):
                document = None
            if isinstance(document, dict) and document.get("tag") == "worker-stop":
                stop = decode_control_message(payload)
                if not isinstance(stop, WorkerStop) or (
                    stop.worker_index != worker_index
                    or stop.generation != generation
                ):
                    raise AuditInfrastructureError(
                        "worker stop generation differs"
                    )
                control.send(
                    WorkerStopped(worker_index, generation), pipeline_deadline
                )
                return
            ordinal, task, transport_capability = decode_task_frame(
                payload, authority, _WORKER_CAPABILITIES
            )
            del ordinal
            current_task_id = task.task_id
            outcome = audit_configuration_worker(
                task,
                authority,
                production_snapshot,
                control,
                command,
                pipeline_deadline,
            )
            receipt = outcome.transport.receipt
            bounds = preparse_compact_result(
                outcome.transport.payload,
                expected_configuration_digest=task.configuration.digest,
                expected_audit_engine_fingerprint=_WORKER_ENGINE,
                limits=_WORKER_LIMITS,
            )
            layout = _transport_allocation_bound(
                receipt.encoded_bytes,
                bounds,
                conservative_allocation_schema(),
                _PIPE_KERNEL_CAPACITY_BYTES,
            )
            control.send(
                WorkerPayloadReady(
                    worker_index,
                    generation,
                    task.task_id,
                    receipt.configuration_digest,
                    receipt.audit_engine_fingerprint,
                    receipt.pipe_nonce,
                    receipt.serial,
                    receipt.nonce,
                    receipt.encoded_bytes,
                    receipt.payload_sha256,
                    receipt.charged_bytes,
                    bounds.conservative_decoded_bytes,
                    bounds.conservative_retained_bytes,
                    layout.counting_pass_peak_bytes,
                    receipt.stdout_bytes,
                    receipt.stages,
                ),
                pipeline_deadline,
            )
            permit = decode_control_message(
                command_receiver.receive_bytes_before(
                    pipeline_deadline, cancel_event=cancel_event
                )
            )
            if not isinstance(permit, WorkerPayloadPermit) or (
                permit.worker_index != worker_index
                or permit.generation != generation
                or permit.task_id != task.task_id
            ):
                raise AuditInfrastructureError(
                    "worker result payload permit differs"
                )
            payload_sender.send_transport_before(
                outcome,
                pipeline_deadline,
                cancel_event=cancel_event,
            )
            completed += 1
            current_task_id = None
            resident = _WORKER_RSS.sample()
            should_retire = completed >= _WORKER_MAXIMUM_TASKS or (
                _WORKER_RECYCLE_RSS_BYTES > 0
                and resident >= _WORKER_RECYCLE_RSS_BYTES
            )
            if should_retire:
                reason = (
                    "task-limit"
                    if completed >= _WORKER_MAXIMUM_TASKS
                    else "resident-memory"
                )
                control.send(
                    WorkerRetire(worker_index, generation, reason),
                    pipeline_deadline,
                )
                acknowledgement = decode_control_message(
                    command_receiver.receive_bytes_before(
                        pipeline_deadline, cancel_event=cancel_event
                    )
                )
                if not isinstance(acknowledgement, WorkerRetireAck) or (
                    acknowledgement.worker_index != worker_index
                    or acknowledgement.generation != generation
                ):
                    raise AuditInfrastructureError(
                        "worker retirement acknowledgement differs"
                    )
                return
    except BaseException as error:
        try:
            diagnostic = _bounded_diagnostic(error)
            event_sender.send_bytes_before(
                encode_control_message(WorkerFailure(
                    worker_index, generation, current_task_id, diagnostic
                )),
                pipeline_deadline,
            )
        except BaseException:
            pass
        try:
            cancel_event.set()
        except BaseException:
            pass
    finally:
        for capability in capabilities.values():
            try:
                capability.native_owner.close()
            except BaseException:
                pass
        _WORKER_CAPABILITIES = None
        _WORKER_PREATTESTED_ENGINE = None
        for endpoint in (
            startup_receiver, task_receiver, command_receiver, event_sender
        ):
            endpoint.close()
        try:
            payload_sender.close()
        except BaseException:
            pass
        if capability_transfer_receiver is not None:
            try:
                capability_transfer_receiver.close()
            except BaseException:
                pass
        tempfile.tempdir = parent_temporary_root


class OrdinalDispatchWindow:
    """Bounded in-flight ordinal window anchored at the first result gap."""

    def __init__(self, task_count: int, worker_count: int) -> None:
        if any(
            not isinstance(value, int) or isinstance(value, bool) or value <= 0
            for value in (task_count, worker_count)
        ):
            raise AuditInfrastructureError("ordinal dispatch window is invalid")
        self.task_count = task_count
        self.worker_count = worker_count
        self.next_unaggregated = 0
        self._accepted: set[int] = set()
        self._dispatched: set[int] = set()

    def dispatchable_ordinals(self) -> tuple[int, ...]:
        upper = min(
            self.task_count,
            self.next_unaggregated + self.worker_count,
        )
        values = tuple(
            ordinal for ordinal in range(self.next_unaggregated, upper)
            if ordinal not in self._accepted and ordinal not in self._dispatched
        )
        self._dispatched.update(values)
        return values

    def accept(self, ordinal: int) -> None:
        if (
            not isinstance(ordinal, int)
            or isinstance(ordinal, bool)
            or ordinal < self.next_unaggregated
            or ordinal >= self.task_count
            or ordinal not in self._dispatched
            or ordinal in self._accepted
        ):
            raise AuditInfrastructureError("accepted worker ordinal is invalid")
        self._accepted.add(ordinal)
        while self.next_unaggregated in self._accepted:
            self.next_unaggregated += 1


class _GenerationState:
    __slots__ = (
        "worker_index", "generation", "process", "tree", "identity",
        "startup_sender", "task_sender", "command_sender", "event_receiver",
        "payload_receiver", "contained", "ready", "pending", "launch_events",
        "tasks_completed", "expects_retire", "maximum_resident_bytes",
        "active_descendants", "capabilities_accepted",
        "scratch_root", "native_carrier", "teardown_completed",
        "startup_receiver", "task_receiver", "command_receiver",
        "event_sender", "payload_sender", "capability_transfer_parent",
        "capability_transfer_child", "platform_kind", "process_started",
        "native_generation_acquired", "generation_job_created",
        "generation_job_assigned", "generation_job_assignment_state",
        "registry_generation_acquired",
        "native_create_requested", "process_start_attempted",
        "identity_confirmed", "promoted", "setup_completed",
        "setup_finalized",
    )

    def __init__(
        self, worker_index, generation, process=None, tree=None, identity=None,
        startup_sender=None, task_sender=None, command_sender=None,
        event_receiver=None, payload_receiver=None, scratch_root=None,
        native_carrier=None,
    ) -> None:
        self.worker_index = worker_index
        self.generation = generation
        self.process = process
        self.tree = tree
        self.identity = identity
        self.startup_sender = startup_sender
        self.task_sender = task_sender
        self.command_sender = command_sender
        self.event_receiver = event_receiver
        self.payload_receiver = payload_receiver
        self.contained = False
        self.ready = False
        self.capabilities_accepted = False
        self.pending = None
        self.launch_events = []
        self.tasks_completed = 0
        self.expects_retire = False
        self.maximum_resident_bytes = 0
        self.active_descendants = {}
        self.scratch_root = scratch_root
        self.native_carrier = native_carrier
        self.teardown_completed: set[str] = set()
        self.startup_receiver = None
        self.task_receiver = None
        self.command_receiver = None
        self.event_sender = None
        self.payload_sender = None
        self.capability_transfer_parent = None
        self.capability_transfer_child = None
        self.platform_kind = None
        self.process_started = False
        self.native_generation_acquired = native_carrier is not None
        self.generation_job_created = False
        self.generation_job_assigned = False
        self.generation_job_assignment_state = None
        self.registry_generation_acquired = False
        self.native_create_requested = False
        self.process_start_attempted = False
        self.identity_confirmed = False
        self.promoted = False
        self.setup_completed: set[str] = set()
        self.setup_finalized = False


@dataclass(frozen=True, slots=True)
class _PendingCoordinatorTask:
    ordinal: int
    configuration: PreprocessConfiguration
    task_id: str
    reservation: PerTaskCompactReservation
    capability: CompactResultTransportCapability
    queued_ownership: CompactResultOwnership


def _validate_worker_payload_ready_receipt(
    event: WorkerPayloadReady,
    pending: _PendingCoordinatorTask,
) -> None:
    """Authenticate the complete ready receipt before publication handoff."""

    if (
        not isinstance(event, WorkerPayloadReady)
        or not isinstance(pending, _PendingCoordinatorTask)
    ):
        raise AuditInfrastructureError(
            "worker result payload receipt is invalid"
        )
    capability = pending.capability
    reservation = pending.reservation
    if (
        event.task_id != pending.task_id
        or event.generation != reservation.generation
        or event.configuration_digest != pending.configuration.digest
        or event.configuration_digest != capability.configuration_digest
        or event.audit_engine_fingerprint
        != capability.audit_engine_fingerprint
        or event.worker_index != capability.worker_slot
        or event.pipe_nonce != capability.pipe_nonce
        or event.serial != capability.serial
        or not hmac.compare_digest(event.nonce, capability.nonce)
        or event.charged_bytes > capability.maximum_bytes
    ):
        raise AuditInfrastructureError(
            "worker result payload receipt capability differs"
        )
    declared = CompactResultPreparseBounds(
        event.encoded_bytes,
        event.conservative_decoded_bytes,
        event.conservative_retained_bytes,
    )
    layout = _transport_allocation_bound(
        event.encoded_bytes,
        declared,
        conservative_allocation_schema(),
        _PIPE_KERNEL_CAPACITY_BYTES,
    )
    if layout.counting_pass_peak_bytes != event.counting_pass_peak_bytes:
        raise AuditInfrastructureError(
            "worker result payload counting declaration differs"
        )


def _decode_authenticated_worker_payload(
    event: WorkerPayloadReady,
    pending: _PendingCoordinatorTask,
    worker_outcome: ConfigurationAuditTransportOutcome,
    aggregator: StreamingResultAggregator,
) -> tuple[ConfigurationAuditOutcomeOwner, CompactResultOwnership, int]:
    """Authenticate declarations and acquire shared decode ownership first."""
    if (
        not isinstance(event, WorkerPayloadReady)
        or not isinstance(pending, _PendingCoordinatorTask)
        or not isinstance(worker_outcome, ConfigurationAuditTransportOutcome)
        or not isinstance(aggregator, StreamingResultAggregator)
    ):
        raise AuditInfrastructureError("worker result payload is invalid")
    receipt = worker_outcome.transport.receipt
    if (
        receipt.task_id != event.task_id
        or receipt.generation != event.generation
        or receipt.worker_slot != event.worker_index
        or receipt.configuration_digest != event.configuration_digest
        or receipt.audit_engine_fingerprint
        != event.audit_engine_fingerprint
        or receipt.pipe_nonce != event.pipe_nonce
        or receipt.serial != event.serial
        or not hmac.compare_digest(receipt.nonce, event.nonce)
        or receipt.encoded_bytes != event.encoded_bytes
        or receipt.payload_sha256 != event.encoded_sha256
        or receipt.charged_bytes != event.charged_bytes
        or receipt.stdout_bytes != event.stdout_bytes
        or receipt.stages != event.stages
    ):
        raise AuditInfrastructureError(
            "worker result payload declaration differs"
        )
    bounds = preparse_compact_result(
        worker_outcome.transport.payload,
        expected_configuration_digest=pending.configuration.digest,
        expected_audit_engine_fingerprint=receipt.audit_engine_fingerprint,
        limits=aggregator._limits,
    )
    declared = CompactResultPreparseBounds(
        event.encoded_bytes,
        event.conservative_decoded_bytes,
        event.conservative_retained_bytes,
    )
    if bounds != declared:
        raise AuditInfrastructureError(
            "worker result payload allocation declaration differs"
        )
    layout = _transport_allocation_bound(
        event.encoded_bytes,
        bounds,
        conservative_allocation_schema(),
        _PIPE_KERNEL_CAPACITY_BYTES,
    )
    if layout.counting_pass_peak_bytes != event.counting_pass_peak_bytes:
        raise AuditInfrastructureError(
            "worker result payload counting declaration differs"
        )

    cold_slot_bytes = aggregator.cold_slot_reserved_bytes
    cold_ownership = aggregator.take_cold_slot(
        f"task:{event.task_id}:decode-capacity"
    )
    decode_ownership: CompactResultOwnership | None = None
    owner: ConfigurationAuditOutcomeOwner | None = None
    retained_ownership: CompactResultOwnership | None = None
    try:
        decode_ownership = _transition_worker_payload_to_decode(
            cold_ownership, bounds, layout
        )
        if not pending.queued_ownership.released:
            pending.queued_ownership.release()
        owner = receive_configuration_audit_outcome(
            pending.reservation, pending.capability, worker_outcome
        )
        retained_bytes = compact_result_retained_bytes(owner.outcome.result)
        retained_ownership = decode_ownership.replace_committed(
            retained_bytes,
            label=f"task:{event.task_id}:retained-result",
            semantic_event="retain-result",
        )
        decode_ownership = None
        return owner, retained_ownership, cold_slot_bytes
    except BaseException:
        if owner is not None and owner.active:
            owner.close()
        for ownership in (
            retained_ownership, decode_ownership, cold_ownership
        ):
            if ownership is not None and not ownership.released:
                ownership.release()
        raise


@dataclass(frozen=True, slots=True)
class _OwnedForestSample:
    platform_kind: str
    maximum_observed_resident_bytes: int
    accounting_complete: bool
    surviving_processes: tuple[int, ...]


def _combine_owned_generation_memory(
    parent_resident_bytes: int,
    live_slot_resident_bytes: Mapping[int, int],
    archived_generations: tuple[tuple[int, int, int], ...] | list[tuple[int, int, int]],
) -> int:
    if (
        not isinstance(parent_resident_bytes, int)
        or isinstance(parent_resident_bytes, bool)
        or parent_resident_bytes < 0
        or not isinstance(live_slot_resident_bytes, Mapping)
    ):
        raise AuditInfrastructureError("owned generation memory sample is invalid")
    slot_maxima: dict[int, int] = {}
    for worker_index, resident in live_slot_resident_bytes.items():
        _bounded_wire_int(worker_index, "worker memory slot", (1 << 32) - 1)
        if not isinstance(resident, int) or isinstance(resident, bool) or resident < 0:
            raise AuditInfrastructureError("owned generation memory sample is invalid")
        slot_maxima[worker_index] = resident
    for worker_index, generation, resident in archived_generations:
        _bounded_wire_int(worker_index, "worker memory slot", (1 << 32) - 1)
        _bounded_wire_int(generation, "worker memory generation", (1 << 64) - 1)
        if not isinstance(resident, int) or isinstance(resident, bool) or resident < 0:
            raise AuditInfrastructureError("owned generation memory sample is invalid")
        slot_maxima[worker_index] = max(
            slot_maxima.get(worker_index, 0), resident
        )
    return parent_resident_bytes + sum(slot_maxima.values())


def _native_descendant_processes(
    root_pid: int,
) -> tuple[tuple[int, str, int], ...]:
    """Return complete live descendants with PID/start token/residency."""
    _bounded_wire_int(root_pid, "worker root PID", (1 << 32) - 1)
    from gpu_capability_process_tree import native_process_resident_bytes

    parents: dict[int, int] = {}
    start_tokens: dict[int, str] = {}
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class ProcessEntry32(ctypes.Structure):
            _fields_ = (
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateToolhelp32Snapshot.argtypes = (
            wintypes.DWORD, wintypes.DWORD
        )
        kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel32.Process32FirstW.argtypes = (
            wintypes.HANDLE, ctypes.POINTER(ProcessEntry32)
        )
        kernel32.Process32NextW.argtypes = (
            wintypes.HANDLE, ctypes.POINTER(ProcessEntry32)
        )
        kernel32.OpenProcess.argtypes = (
            wintypes.DWORD, wintypes.BOOL, wintypes.DWORD
        )
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
        invalid = ctypes.c_void_p(-1).value
        if snapshot == invalid:
            raise AuditInfrastructureError(
                "native process-tree enumeration failed"
            )
        try:
            entry = ProcessEntry32()
            entry.dwSize = ctypes.sizeof(entry)
            ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
            while ok:
                parents[int(entry.th32ProcessID)] = int(
                    entry.th32ParentProcessID
                )
                ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            kernel32.CloseHandle(snapshot)

        def windows_start_token(pid: int) -> str:
            handle = kernel32.OpenProcess(0x0400, False, pid)
            if not handle:
                raise AuditInfrastructureError(
                    "native process start identity is unavailable"
                )
            try:
                creation = wintypes.FILETIME()
                exit_time = wintypes.FILETIME()
                kernel = wintypes.FILETIME()
                user = wintypes.FILETIME()
                if not kernel32.GetProcessTimes(
                    handle,
                    ctypes.byref(creation),
                    ctypes.byref(exit_time),
                    ctypes.byref(kernel),
                    ctypes.byref(user),
                ):
                    raise AuditInfrastructureError(
                        "native process start identity is unavailable"
                    )
                value = (int(creation.dwHighDateTime) << 32) | int(
                    creation.dwLowDateTime
                )
                return f"windows-filetime-{value}"
            finally:
                kernel32.CloseHandle(handle)

        start_token = windows_start_token
    elif sys.platform.startswith("linux"):
        for stat_path in Path("/proc").glob("[0-9]*/stat"):
            try:
                text = stat_path.read_text(encoding="ascii")
                close = text.rfind(")")
                fields = text[close + 2:].split()
                pid = int(stat_path.parent.name)
                parents[pid] = int(fields[1])
                start_tokens[pid] = f"linux-start-{fields[19]}"
            except (OSError, ValueError, IndexError):
                continue

        def start_token(pid: int) -> str:
            token = start_tokens.get(pid)
            if token is None:
                raise AuditInfrastructureError(
                    "native process start identity is unavailable"
                )
            return token
    else:
        return ()

    descendants: set[int] = set()
    frontier = {root_pid}
    while frontier:
        children = {
            pid for pid, parent_pid in parents.items()
            if parent_pid in frontier and pid not in descendants
        }
        descendants.update(children)
        frontier = children
    result = []
    for pid in sorted(descendants):
        try:
            result.append((
                pid, start_token(pid), native_process_resident_bytes(pid)
            ))
        except AuditInfrastructureError:
            if parents.get(pid) is not None:
                raise
    return tuple(result)


def _owned_process_identity(platform_kind: str, pid: int, token: str):
    from gpu_capability_process_tree import OwnedProcessIdentity

    return OwnedProcessIdentity(platform_kind, pid, token)


def _worker_scratch_parent() -> Path:
    if sys.platform.startswith("linux"):
        shared_memory = Path("/dev/shm")
        if shared_memory.is_dir() and os.access(shared_memory, os.W_OK):
            return shared_memory
    return Path(tempfile.gettempdir())


def _release_generation_job_accounting(
    run_accountant, state, deadline: float, accounting_lock
) -> None:
    acquired = getattr(state, "generation_job_created", None)
    if acquired is False:
        return
    archive_generation_job = getattr(
        run_accountant, "archive_generation_job", None
    )
    discard_generation_job = getattr(
        run_accountant, "discard_generation_job", None
    )
    assignment_completed = getattr(
        run_accountant, "generation_job_assignment_completed", None
    )
    assignment_state = getattr(
        run_accountant, "generation_job_assignment_state", None
    )
    if callable(assignment_state):
        state_value = assignment_state(
            state.worker_index, state.generation
        )
        if state_value not in {
            "unassigned", "aggregate-only", "generation-assigned"
        }:
            raise AuditInfrastructureError(
                "generation Job assignment progress differs"
            )
    elif callable(assignment_completed):
        assigned = assignment_completed(
            state.worker_index, state.generation
        )
        if not isinstance(assigned, bool):
            raise AuditInfrastructureError(
                "generation Job assignment progress differs"
            )
        state_value = "generation-assigned" if assigned else "unassigned"
    else:
        assigned = getattr(state, "generation_job_assigned", True)
        state_value = "generation-assigned" if assigned else "unassigned"
    state.generation_job_assignment_state = state_value
    state.generation_job_assigned = state_value == "generation-assigned"
    if state_value == "generation-assigned" and callable(
        archive_generation_job
    ):
        with accounting_lock:
            archive_generation_job(
                state.worker_index, state.generation, deadline
            )
        return
    if state_value == "aggregate-only":
        finalize_partial = getattr(
            run_accountant,
            "finalize_aggregate_only_generation_job",
            None,
        )
        if callable(finalize_partial):
            with accounting_lock:
                finalize_partial(state.worker_index, state.generation)
            return
    if state_value == "unassigned" and callable(discard_generation_job):
        with accounting_lock:
            discard_generation_job(state.worker_index, state.generation)
        return
    if acquired is not None:
        raise AuditInfrastructureError(
            "generation accounting cleanup authority is unavailable"
        )


def _generation_has_accounted_processes(state) -> bool:
    if getattr(state, "generation_job_created", False):
        assignment_state = getattr(
            state, "generation_job_assignment_state", None
        )
        if assignment_state is not None:
            if assignment_state not in {
                "unassigned", "aggregate-only", "generation-assigned"
            }:
                raise AuditInfrastructureError(
                    "generation Job assignment progress differs"
                )
            return assignment_state != "unassigned"
        return getattr(state, "generation_job_assigned", False)
    return getattr(state, "process_started", True)


class _LinuxGenerationLifecycle:
    """Bind each spawned generation to a Task-6 rendezvous-owned cgroup leaf."""

    def __init__(self, client, deadline: float) -> None:
        if (
            not all(callable(getattr(client, name, None)) for name in (
                "create_leaf", "acknowledge_leaf", "release_leaf"
            ))
            or not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
        ):
            raise AuditInfrastructureError("Linux generation lifecycle is invalid")
        self.client = client
        self.deadline = float(deadline)
        self.live: dict[tuple[int, int], LinuxWorkerContainment] = {}
        self.requested: set[tuple[int, int]] = set()
        self.released: set[tuple[int, int]] = set()
        self._external_release_progress: set[tuple[int, int]] = set()
        self.run_path: Path | None = None
        self.service_root: Path | None = None
        self.baseline_run_events: dict[str, int] | None = None
        self.baseline_service_events: dict[str, int] | None = None
        self._sealed_task_phase_snapshot: LinuxPhaseSnapshot | None = None

    @staticmethod
    def _read_events(path: Path) -> dict[str, int]:
        try:
            values = {
                name: int(value)
                for name, value in (
                    line.split() for line in path.read_text(
                        encoding="ascii"
                    ).splitlines()
                )
            }
        except (OSError, ValueError) as error:
            raise AuditInfrastructureError(
                "Linux cgroup events are unavailable"
            ) from error
        if not {"oom", "oom_kill", "max"}.issubset(values):
            raise AuditInfrastructureError("Linux cgroup events are incomplete")
        return values

    @staticmethod
    def _read_integer(path: Path) -> int:
        try:
            value = int(path.read_text(encoding="ascii").strip())
        except (OSError, ValueError) as error:
            raise AuditInfrastructureError(
                f"Linux cgroup {path.name} is unavailable"
            ) from error
        if value < 0:
            raise AuditInfrastructureError(
                f"Linux cgroup {path.name} is invalid"
            )
        return value

    def create_generation(self, worker_index: int,
                          generation: int) -> LinuxWorkerContainment:
        key = (worker_index, generation)
        if key in self.live or key in self.requested or key in self.released:
            raise AuditInfrastructureError("Linux generation leaf is duplicated")
        self.requested.add(key)
        carrier = self.client.create_leaf(
            worker_index, generation, self.deadline
        )
        if not isinstance(carrier, LinuxWorkerContainment):
            raise AuditInfrastructureError("Linux generation leaf carrier is invalid")
        self.live[key] = carrier
        run_path = Path(carrier.cgroup_run_path)
        if self.run_path is None:
            self.run_path = run_path
            self.service_root = run_path.parent
            if run_path.exists():
                self.baseline_run_events = self._read_events(
                    run_path / "memory.events"
                )
                self.baseline_service_events = self._read_events(
                    self.service_root / "memory.events"
                )
            elif sys.platform.startswith("linux"):
                raise AuditInfrastructureError(
                    "Linux generation run cgroup is unavailable"
                )
        elif self.run_path != run_path:
            raise AuditInfrastructureError("Linux generation run cgroup differs")
        return carrier

    def accept_worker_contained(self, event: WorkerContained,
                                carrier: LinuxWorkerContainment) -> None:
        key = (event.worker_index, event.generation)
        if self.live.get(key) is not carrier:
            raise AuditInfrastructureError("Linux worker containment carrier differs")
        self.client.acknowledge_leaf(
            event.worker_index,
            event.generation,
            event.worker_pid,
            carrier,
            self.deadline,
        )

    @staticmethod
    def _force_empty(carrier: LinuxWorkerContainment, deadline: float) -> None:
        leaf = Path(carrier.cgroup_leaf_path)
        try:
            (leaf / "cgroup.kill").write_text("1", encoding="ascii")
        except OSError as error:
            raise AuditInfrastructureError(
                "Linux generation cgroup kill failed"
            ) from error
        while time.monotonic() < deadline:
            try:
                members = (leaf / "cgroup.procs").read_text(
                    encoding="ascii"
                ).split()
            except OSError as error:
                raise AuditInfrastructureError(
                    "Linux generation cgroup membership is unavailable"
                ) from error
            if not members:
                return
            time.sleep(0.01)
        raise AuditInfrastructureError("Linux generation cgroup cleanup timed out")

    def release_generation(self, worker_index: int, generation: int,
                           carrier: LinuxWorkerContainment | None, deadline: float,
                           *, force: bool) -> None:
        key = (worker_index, generation)
        if key in self.released and key not in self.live and key not in self.requested:
            return
        actual = self.live.get(key)
        if key not in self.requested:
            raise AuditInfrastructureError("Linux generation leaf differs")
        if carrier is not None and actual is not carrier:
            raise AuditInfrastructureError("Linux generation leaf differs")
        if force and actual is not None:
            self._force_empty(actual, deadline)
        external_progress = getattr(
            self, "_external_release_progress", None
        )
        if external_progress is None:
            external_progress = set()
            self._external_release_progress = external_progress
        if key not in external_progress:
            _generation_release_transition(
                "linux", "external", "before", *key
            )
            self.client.release_leaf(worker_index, generation, deadline)
            external_progress.add(key)
            _generation_release_transition(
                "linux", "external", "after", *key
            )
        if key not in self.released:
            _generation_release_transition(
                "linux", "terminal", "before", *key
            )
            self.released.add(key)
            _generation_release_transition(
                "linux", "terminal", "after", *key
            )
        if key in self.live or key in self.requested:
            _generation_release_transition(
                "linux", "remove", "before", *key
            )
            self.live.pop(key, None)
            self.requested.discard(key)
            _generation_release_transition(
                "linux", "remove", "after", *key
            )

    def memory_measurements(self, *, require_empty: bool) -> LinuxRunMemoryMeasurements:
        if (
            self.run_path is None
            or self.service_root is None
            or self.baseline_run_events is None
            or self.baseline_service_events is None
        ):
            raise AuditInfrastructureError("Linux cgroup accounting is unavailable")
        run_events = self._read_events(self.run_path / "memory.events")
        service_events = self._read_events(
            self.service_root / "memory.events"
        )

        def delta(current, baseline, name):
            value = current[name] - baseline[name]
            if value < 0:
                raise AuditInfrastructureError("Linux cgroup event counter regressed")
            return value

        survivors = 0
        for carrier in self.live.values():
            try:
                survivors += len((
                    Path(carrier.cgroup_leaf_path) / "cgroup.procs"
                ).read_text(encoding="ascii").split())
            except OSError as error:
                raise AuditInfrastructureError(
                    "Linux generation cgroup membership is unavailable"
                ) from error
        values = LinuxRunMemoryMeasurements(
            self._read_integer(self.run_path / "memory.current"),
            self._read_integer(self.run_path / "memory.peak"),
            self._read_integer(self.run_path / "memory.max"),
            self._read_integer(self.run_path / "memory.high"),
            delta(run_events, self.baseline_run_events, "oom"),
            delta(run_events, self.baseline_run_events, "oom_kill"),
            delta(run_events, self.baseline_run_events, "max"),
            delta(service_events, self.baseline_service_events, "oom"),
            delta(service_events, self.baseline_service_events, "oom_kill"),
            delta(service_events, self.baseline_service_events, "max"),
            survivors,
            True,
        )
        if any((
            values.cgroup_oom_count_delta,
            values.cgroup_oom_kill_count_delta,
            values.cgroup_max_event_count_delta,
            values.service_root_oom_count_delta,
            values.service_root_oom_kill_count_delta,
            values.service_root_max_event_count_delta,
        )):
            raise AuditInfrastructureError("Linux cgroup memory event increased")
        if require_empty and survivors:
            raise AuditInfrastructureError("Linux cgroup survivors remain")
        return values

    def seal_task_phase(self, archived_generation_count: int) -> LinuxPhaseSnapshot:
        existing = getattr(self, "_sealed_task_phase_snapshot", None)
        if existing is not None:
            if existing.archived_generation_count != archived_generation_count:
                raise AuditInfrastructureError(
                    "Linux task phase generation count differs"
                )
            return existing
        if self.live or self.requested:
            raise AuditInfrastructureError("Linux generation leaves remain")
        snapshot = LinuxPhaseSnapshot(
            "linux",
            "tasks",
            self.memory_measurements(require_empty=True),
            archived_generation_count,
        )
        self._sealed_task_phase_snapshot = snapshot
        return snapshot


class _MacOSGenerationLifecycle:
    """Own Task-6 registered worker/compiler PGIDs through reconciliation."""

    def __init__(
        self,
        accountant,
        configurations: tuple[PreprocessConfiguration, ...],
        deadline: float,
        *,
        provider=None,
        permit_authority=None,
    ) -> None:
        if (
            not callable(getattr(accountant, "register_group", None))
            or not callable(getattr(accountant, "reconcile_group", None))
            or not callable(getattr(accountant, "memory_measurements", None))
            or not isinstance(configurations, tuple)
            or not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
        ):
            raise AuditInfrastructureError("macOS generation lifecycle is invalid")
        if provider is None or permit_authority is None:
            from gpu_capability_process_tree import (
                MacOSExecPermitAuthority,
                MacOSLibprocProvider,
            )

            provider = MacOSLibprocProvider()
            by_identity = {
                (
                    configuration.compiler_capability.executable_identity,
                    configuration.compiler_capability.executable_sha256,
                ): configuration.compiler_capability_digest
                for configuration in configurations
            }

            def verify_executable(identity, digest):
                fingerprint = by_identity.get((identity, digest))
                if fingerprint is None:
                    raise AuditInfrastructureError(
                        "unregistered or untrusted macOS compiler exec"
                    )
                return fingerprint

            permit_authority = MacOSExecPermitAuthority(
                accountant,
                tuple(sorted(set(by_identity.values()))),
                executable_verifier=verify_executable,
            )
        self.accountant = accountant
        self.provider = provider
        self.permit_authority = permit_authority
        self.deadline = float(deadline)
        self.worker_groups: dict[tuple[int, int], int] = {}
        self.compiler_groups: dict[tuple[int, int, int], list[int]] = {}
        self._worker_session_intents: dict[
            tuple[int, int], MacOSWorkerSessionReported
        ] = {}
        self._compiler_adoption_intents: dict[
            tuple[int, int, int, int], CompilerPgidReported
        ] = {}
        self._compiler_permit_receipts: dict[
            tuple[int, int, int, int], CompilerExecPermit
        ] = {}
        self.released_generations: set[tuple[int, int]] = set()
        self._release_reconciliation_progress: set[
            tuple[int, int, int]
        ] = set()
        self._sealed_task_phase_snapshot: MacOSPhaseSnapshot | None = None

    def accept_worker_session(self, report: MacOSWorkerSessionReported) -> None:
        key = (report.worker_index, report.generation)
        intents = getattr(self, "_worker_session_intents", None)
        if intents is None:
            intents = {}
            self._worker_session_intents = intents
        existing_intent = intents.get(key)
        if existing_intent is not None and existing_intent != report:
            raise AuditInfrastructureError(
                "macOS worker session intent differs"
            )
        if key in self.released_generations:
            raise AuditInfrastructureError("macOS worker PGID is duplicated")
        if key in self.worker_groups:
            if existing_intent == report and self.worker_groups[key] == report.pgid:
                return
            raise AuditInfrastructureError("macOS worker PGID is duplicated")
        identity, _resident, _parent = self.provider._identity_and_residency(
            report.pid, report.pgid
        )
        expected = _owned_process_identity(
            "macos", report.pid, report.bsd_start_identity
        )
        if identity != expected:
            raise AuditInfrastructureError("macOS worker session identity differs")
        if existing_intent is None:
            intents[key] = report
        self.accountant.register_group(
            report.pgid, identity,
            f"worker:{report.worker_index}:{report.generation}",
        )
        self.worker_groups[key] = report.pgid

    def permit_compiler(self, report: CompilerPgidReported) -> CompilerExecPermit:
        key = (report.worker_index, report.generation, report.task_id)
        adoption_key = (*key, report.pgid)
        intents = getattr(self, "_compiler_adoption_intents", None)
        if intents is None:
            intents = {}
            self._compiler_adoption_intents = intents
        existing_intent = intents.get(adoption_key)
        if existing_intent is None:
            intents[adoption_key] = report
        elif existing_intent != report:
            raise AuditInfrastructureError(
                "macOS compiler adoption intent differs"
            )
        receipts = getattr(self, "_compiler_permit_receipts", None)
        if receipts is None:
            receipts = {}
            self._compiler_permit_receipts = receipts
        groups = self.compiler_groups.setdefault(key, [])
        if report.pgid in groups:
            permit = receipts.get(adoption_key)
            if permit is not None:
                return permit
            raise AuditInfrastructureError("macOS compiler PGID is duplicated")
        permit = self.permit_authority.permit_compiler(report)
        receipts[adoption_key] = permit
        groups.append(report.pgid)
        return permit

    def reconcile_task(self, worker_index: int, generation: int,
                       task_id: int) -> None:
        key = (worker_index, generation, task_id)
        groups = self.compiler_groups.get(key, [])
        while groups:
            pgid = groups[0]
            if self.accountant.reconcile_group(pgid, self.provider) is not True:
                raise AuditInfrastructureError(
                    "macOS compiler PGID reconciliation did not complete"
                )
            _macos_reconciliation_completed(
                "reconcile-task", worker_index, generation, pgid
            )
            del groups[0]
        self.compiler_groups.pop(key, None)

    def observe(self) -> None:
        self.provider.observe(self.accountant, _current_process_rss_bytes())

    def _kill_and_wait_empty(self, pgid: int, deadline: float) -> None:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        reconcile_survivors = getattr(self.provider, "reconcile_survivors", None)
        if not callable(reconcile_survivors):
            return
        while time.monotonic() < deadline:
            if not reconcile_survivors(self.accountant, pgid):
                return
            time.sleep(0.01)
        raise AuditInfrastructureError("macOS PGID cleanup timed out")

    def release_generation(self, worker_index: int, generation: int,
                           _carrier, deadline: float, *, force: bool) -> None:
        worker_key = (worker_index, generation)
        if (
            worker_key in self.released_generations
            and worker_key not in self.worker_groups
            and not any(key[:2] == worker_key for key in self.compiler_groups)
        ):
            return
        progress = getattr(self, "_release_reconciliation_progress", None)
        if progress is None:
            progress = set()
            self._release_reconciliation_progress = progress
        remaining = [key for key in self.compiler_groups
                     if key[:2] == (worker_index, generation)]
        for key in remaining:
            for pgid in self.compiler_groups[key]:
                progress_key = (worker_index, generation, pgid)
                if progress_key in progress:
                    continue
                if force:
                    self._kill_and_wait_empty(pgid, deadline)
                if self.accountant.reconcile_group(
                    pgid, self.provider
                ) is not True:
                    raise AuditInfrastructureError(
                        "macOS compiler PGID reconciliation did not complete"
                    )
                _macos_reconciliation_completed(
                    "release-compiler", worker_index, generation, pgid
                )
                progress.add(progress_key)
        pgid = self.worker_groups.get(worker_key)
        if pgid is None and worker_key not in self.released_generations:
            raise AuditInfrastructureError("macOS worker PGID is unavailable")
        worker_progress = (worker_index, generation, pgid)
        if pgid is not None and worker_progress not in progress:
            if force:
                self._kill_and_wait_empty(pgid, deadline)
            if time.monotonic() >= deadline:
                raise AuditInfrastructureError(
                    "macOS PGID reconciliation deadline exceeded"
                )
            if self.accountant.reconcile_group(pgid, self.provider) is not True:
                raise AuditInfrastructureError(
                    "macOS worker PGID reconciliation did not complete"
                )
            _macos_reconciliation_completed(
                "release-worker", worker_index, generation, pgid
            )
            progress.add(worker_progress)
        if worker_key not in self.released_generations:
            _generation_release_transition(
                "macos", "terminal", "before", *worker_key
            )
            self.released_generations.add(worker_key)
            _generation_release_transition(
                "macos", "terminal", "after", *worker_key
            )
        if worker_key in self.worker_groups or any(
            key[:2] == worker_key for key in self.compiler_groups
        ):
            _generation_release_transition(
                "macos", "remove", "before", *worker_key
            )
            self.worker_groups.pop(worker_key, None)
            for key in tuple(self.compiler_groups):
                if key[:2] == worker_key:
                    self.compiler_groups.pop(key)
            _generation_release_transition(
                "macos", "remove", "after", *worker_key
            )

    def seal_task_phase(self, archived_generation_count: int) -> MacOSPhaseSnapshot:
        existing = getattr(self, "_sealed_task_phase_snapshot", None)
        if existing is not None:
            if existing.archived_generation_count != archived_generation_count:
                raise AuditInfrastructureError(
                    "macOS task phase generation count differs"
                )
            return existing
        if self.worker_groups or self.compiler_groups:
            raise AuditInfrastructureError("macOS registered PGID survivors remain")
        self.observe()
        snapshot = MacOSPhaseSnapshot(
            "macos",
            "tasks",
            self.accountant.memory_measurements(),
            archived_generation_count,
        )
        self._sealed_task_phase_snapshot = snapshot
        return snapshot


class GenerationReactor:
    """Single parent reader for every live generation's bounded outputs."""

    def __init__(
        self,
        *,
        configurations: tuple[PreprocessConfiguration, ...],
        dependency_roots: DependencyRootAuthority,
        production_snapshot,
        cache_root: Path,
        limits: AuditLimits,
        engine: str,
        runtime_contract: WorkerRuntimeContract,
        run_accountant,
        capability_registry,
        result_budget: CompactResultMemoryBudget,
        worker_count: int,
        cleanup_deadline: float,
    ) -> None:
        self.configurations = configurations
        self.dependency_roots = dependency_roots
        self.production_snapshot = production_snapshot
        self.cache_root = cache_root
        self.limits = limits
        self.engine = engine
        self.runtime_contract = runtime_contract
        if (
            not isinstance(cleanup_deadline, (int, float))
            or isinstance(cleanup_deadline, bool)
            or not math.isfinite(float(cleanup_deadline))
            or cleanup_deadline < runtime_contract.pipeline_deadline
        ):
            raise AuditInfrastructureError(
                "worker cleanup deadline is invalid"
            )
        self.cleanup_deadline = float(cleanup_deadline)
        self.run_accountant = run_accountant
        self.capability_registry = capability_registry
        self.result_budget = result_budget
        self.compact_accounting_observer = result_budget.observer
        maximum_encoded = _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES
        maximum_bounds = CompactResultPreparseBounds(
            maximum_encoded,
            conservative_allocation_schema().json_decoded_fixed_bytes
            + conservative_allocation_schema().json_decoded_multiplier
            * maximum_encoded,
            limits.compact_result_bytes,
        )
        self.maximum_queued_counting_bytes = _transport_allocation_bound(
            maximum_encoded,
            maximum_bounds,
            conservative_allocation_schema(),
            _PIPE_KERNEL_CAPACITY_BYTES,
        ).counting_pass_peak_bytes
        self.context = multiprocessing.get_context("spawn")
        self.cancel_event = self.context.Event()
        if isinstance(
            self.run_accountant, LinuxCompilerAuditAttemptAccountant
        ):
            self.run_accountant.register_spawn_resource_tracker()
        self.states: list[_GenerationState | None] = [None] * worker_count
        self.next_generations = [0] * worker_count
        self.publication_requests: collections.deque[
            tuple[int, int, str]
        ] = collections.deque()
        self.active_publication: tuple[int, int, str] | None = None
        self.archived_generation_telemetry: list[tuple[int, int, int]] = []
        self.archived_scratch_roots: list[Path] = []
        self.audit_launch_count = 0
        self.stdout_bytes = 0
        self.maximum_encoded_result_bytes = 0
        self.maximum_conservative_decoded_bytes = 0
        self.maximum_conservative_retained_bytes = 0
        self.worker_pids: list[int] = []
        self.registry_generation_duplicates: dict[
            tuple[int, int], tuple[tuple[str, object], ...]
        ] = {}
        self.deferred_events: collections.deque[
            tuple[_GenerationState, object]
        ] = collections.deque()
        self.task_phase_snapshot = None
        self._lifecycle_task_phase_snapshot = None
        self._run_accountant_task_phase_snapshot = None
        self._task_phase_raw_snapshot = None
        self._task_phase_memory = None
        self._task_phase_generation_identities = None
        self._sealed_task_phase_snapshot = None
        self._accounting_stop = threading.Event()
        self._accounting_lock = threading.Lock()
        self._accounting_error: BaseException | None = None
        self._accounting_thread = None
        self._shutdown_completed: set[str] = set()
        self._closed = False
        self.native_lifecycle = None
        if sys.platform.startswith("linux"):
            client = getattr(run_accountant, "rendezvous_client", run_accountant)
            hello = getattr(client, "hello", None)
            if not callable(hello):
                raise AuditInfrastructureError(
                    "Linux cgroup rendezvous accountant is unavailable"
                )
            hello(runtime_contract.pipeline_deadline)
            self.native_lifecycle = _LinuxGenerationLifecycle(
                client, runtime_contract.pipeline_deadline
            )
        elif sys.platform == "darwin":
            self.native_lifecycle = _MacOSGenerationLifecycle(
                run_accountant, configurations, runtime_contract.pipeline_deadline
            )
        try:
            if os.name == "nt" and callable(
                getattr(self.run_accountant, "observe", None)
            ):
                self._accounting_thread = threading.Thread(
                    target=self._pump_windows_accounting,
                    name="gpu-audit-job-accounting",
                    daemon=True,
                )
                self._accounting_thread.start()
            for worker_index in range(worker_count):
                self.spawn_next_generation(worker_index)
            self._wait_all_ready()
        except BaseException as error:
            try:
                self.abort_and_reap_until_closed(
                    emergency_cleanup_deadline(
                        _generation_cleanup_ceiling(self)
                    )
                )
            except BaseException as cleanup_error:
                error.add_note(
                    f"worker startup cleanup also failed: {cleanup_error}"
                )
            raise

    def _pump_windows_accounting(self) -> None:
        observe = self.run_accountant.observe
        while not self._accounting_stop.is_set():
            try:
                with self._accounting_lock:
                    observe(_current_process_rss_bytes())
            except BaseException as error:
                self._accounting_error = error
                self.cancel_event.set()
                return
            self._accounting_stop.wait(0.001)

    def _stop_accounting_pump(self) -> None:
        self._accounting_stop.set()
        thread = self._accounting_thread
        if thread is not None:
            thread.join(timeout=1.0)
            if thread.is_alive():
                raise AuditInfrastructureError(
                    "Windows Job accounting pump did not stop"
                )
        if self._accounting_error is not None:
            raise AuditInfrastructureError(
                "Windows Job accounting pump failed"
            ) from self._accounting_error

    @property
    def live_generation_count(self) -> int:
        return sum(state is not None for state in self.states)

    @property
    def has_pending_tasks(self) -> bool:
        return any(
            state is not None and state.pending is not None
            for state in self.states
        )

    @property
    def has_expected_retirement(self) -> bool:
        return any(
            state is not None and state.expects_retire
            for state in self.states
        )

    def idle_states(self) -> tuple[_GenerationState, ...]:
        return tuple(
            state for state in self.states
            if state is not None and state.ready and state.pending is None
            and not state.expects_retire
        )

    def spawn_next_generation(self, worker_index: int) -> _GenerationState:
        if self.states[worker_index] is not None:
            raise AuditInfrastructureError(
                "replacement generation slot is not empty"
            )
        if time.monotonic() >= self.runtime_contract.pipeline_deadline:
            raise AuditInfrastructureError(
                "pipeline deadline exceeded before replacement generation"
            )
        generation = self.next_generations[worker_index]
        self.next_generations[worker_index] += 1
        state = _GenerationState(worker_index, generation)
        self.states[worker_index] = state

        def own_pair(pair, label):
            del label
            first, second = pair
            return first, second

        native_carrier = None
        try:
            startup_receiver, startup_sender = own_pair(
                BoundedFrameChannel.create(TASK_MAX_BYTES),
                "startup channel setup cleanup",
            )
            state.startup_receiver = startup_receiver
            state.startup_sender = startup_sender
            task_receiver, task_sender = own_pair(
                BoundedFrameChannel.create(TASK_MAX_BYTES),
                "task channel setup cleanup",
            )
            state.task_receiver = task_receiver
            state.task_sender = task_sender
            command_receiver, command_sender = own_pair(
                BoundedFrameChannel.create(COMMAND_MAX_BYTES),
                "command channel setup cleanup",
            )
            state.command_receiver = command_receiver
            state.command_sender = command_sender
            event_receiver, event_sender = own_pair(
                BoundedFrameChannel.create(CONTROL_MAX_BYTES),
                "event channel setup cleanup",
            )
            state.event_receiver = event_receiver
            state.event_sender = event_sender
            payload_receiver, payload_sender = own_pair(
                BoundedPayloadChannel.create(),
                "payload channel setup cleanup",
            )
            state.payload_receiver = payload_receiver
            state.payload_sender = payload_sender
            scratch_root = Path(tempfile.mkdtemp(
                prefix=f".gpu-capability-worker-{worker_index}-{generation}-",
                dir=_worker_scratch_parent(),
            ))
            scratch_root = scratch_root.resolve()
            state.scratch_root = scratch_root
            capability_transfer_parent = None
            capability_transfer_child = None
            if os.name != "nt":
                capability_transfer_parent, capability_transfer_child = own_pair(
                    socket.socketpair(
                        socket.AF_UNIX,
                        _posix_capability_socket_type(),
                    ),
                    "capability socket setup cleanup",
                )
                state.capability_transfer_parent = capability_transfer_parent
                state.capability_transfer_child = capability_transfer_child
            platform_kind = (
                "windows" if os.name == "nt"
                else ("macos" if sys.platform == "darwin" else "linux")
            )
            state.platform_kind = platform_kind
            from gpu_capability_process_tree import OwnedProcessIdentity, OwnedProcessTree

            tree = OwnedProcessTree(
                platform_kind, f"audit-worker-{worker_index}-{generation}"
            )
            state.tree = tree

            def create_native_leaf():
                state.native_create_requested = True
                carrier = self.native_lifecycle.create_generation(
                    worker_index, generation
                )
                state.native_carrier = carrier
                state.native_generation_acquired = True
                return carrier

            native_carrier = None
            if isinstance(self.native_lifecycle, _LinuxGenerationLifecycle):
                native_carrier = self._complete_generation_setup_transition(
                    state, "native-leaf-create", create_native_leaf
                )

            def construct_process():
                created = self.context.Process(
                    target=_audit_worker_generation_main,
                    args=(
                        worker_index,
                        generation,
                        startup_receiver,
                        task_receiver,
                        command_receiver,
                        event_sender,
                        payload_sender,
                        capability_transfer_child,
                        state.native_carrier,
                        str(scratch_root),
                        self.cancel_event,
                        str(self.cache_root),
                        self.runtime_contract.pipeline_deadline,
                        self.runtime_contract.maximum_tasks_per_worker,
                        self.runtime_contract.recycle_rss_bytes,
                    ),
                    name=f"gpu-audit-{worker_index}-{generation}",
                )
                state.process = created
                return created

            process = self._complete_generation_setup_transition(
                state, "process-construct", construct_process
            )
        except BaseException as setup_error:
            try:
                cleanup_failures = self.abort_and_reap_until_closed(
                    emergency_cleanup_deadline(
                        _generation_cleanup_ceiling(self)
                    )
                )
                for cleanup_error in cleanup_failures:
                    setup_error.add_note(
                        f"worker setup cleanup retry failed: {cleanup_error}"
                    )
                    for note in getattr(cleanup_error, "__notes__", ()):
                        setup_error.add_note(
                            f"worker setup cleanup detail: {note}"
                        )
            except BaseException as cleanup_error:
                setup_error.add_note(
                    f"worker setup cleanup also failed: {cleanup_error}"
                )
            raise setup_error
        generation_job_created = False
        generation_duplicates: list[
            tuple[str, _GenerationCompilerCapabilityDuplicate]
        ] = []
        release_generation = None
        try:
            duplicate_for_generation = getattr(
                self.capability_registry,
                "duplicate_for_generation",
                None,
            )
            transfer_duplicate = getattr(
                self.capability_registry,
                "transfer_duplicate_to_child",
                None,
            )
            acknowledge_generation = getattr(
                self.capability_registry,
                "acknowledge_generation",
                None,
            )
            release_generation = getattr(
                self.capability_registry,
                "release_generation",
                None,
            )
            if not all(callable(value) for value in (
                duplicate_for_generation,
                transfer_duplicate,
                acknowledge_generation,
                release_generation,
            )):
                raise AuditInfrastructureError(
                    "compiler capability registry lifecycle is invalid"
                )
            for digest in sorted({
                configuration.compiler_capability_digest
                for configuration in self.configurations
            }):
                def acquire_registry_duplicate(digest=digest):
                    duplicate = duplicate_for_generation(
                        digest, worker_index, generation
                    )
                    state.registry_generation_acquired = True
                    if (
                        not isinstance(
                            duplicate, _GenerationCompilerCapabilityDuplicate
                        )
                        or duplicate.digest != digest
                        or duplicate.worker_index != worker_index
                        or duplicate.generation != generation
                        or duplicate.closed
                    ):
                        raise AuditInfrastructureError(
                            "compiler capability generation duplicate differs"
                        )
                    generation_duplicates.append((digest, duplicate))
                    self.registry_generation_duplicates[
                        (worker_index, generation)
                    ] = tuple(generation_duplicates)
                    return duplicate

                self._complete_generation_setup_transition(
                    state,
                    f"registry-duplicate:{digest}",
                    acquire_registry_duplicate,
                )
            create_generation_job = getattr(
                self.run_accountant, "create_generation_job", None
            )
            if callable(create_generation_job):
                def create_job():
                    with self._accounting_lock:
                        create_generation_job(worker_index, generation)
                    state.generation_job_created = True

                self._complete_generation_setup_transition(
                    state, "accounting-job-create", create_job
                )
                generation_job_created = state.generation_job_created

            def start_process():
                state.process_start_attempted = True
                try:
                    process.start()
                finally:
                    pid = getattr(process, "pid", None)
                    if isinstance(pid, int) and not isinstance(pid, bool) and pid > 0:
                        state.process_started = True

            self._complete_generation_setup_transition(
                state, "process-start", start_process
            )
            if capability_transfer_child is not None:
                capability_transfer_child.close()
                state.capability_transfer_child = None
            if not isinstance(process.pid, int) or process.pid <= 0:
                raise AuditInfrastructureError(
                    "worker process identity is unavailable"
                )
            identity = OwnedProcessIdentity(
                platform_kind,
                process.pid,
                f"spawn-{process.pid}-{generation}-{time.monotonic_ns()}",
            )
            tree.register(identity, None, "audit-worker")
            state.identity = identity
            state.identity_confirmed = True
            assign_generation_process = getattr(
                self.run_accountant, "assign_generation_process", None
            )
            if callable(assign_generation_process):
                def assign_job():
                    with self._accounting_lock:
                        assign_generation_process(
                            worker_index,
                            generation,
                            int(process.sentinel),
                            process.pid,
                            f"worker:{worker_index}:{generation}",
                        )
                    state.generation_job_assigned = True

                self._complete_generation_setup_transition(
                    state, "accounting-job-assign", assign_job
                )

            def promote_state():
                if process.pid not in self.worker_pids:
                    self.worker_pids.append(process.pid)
                state.promoted = True

            self._complete_generation_setup_transition(
                state, "state-promotion", promote_state
            )
            startup_receiver.close()
            state.startup_receiver = None
            task_receiver.close()
            state.task_receiver = None
            command_receiver.close()
            state.command_receiver = None
            event_sender.close()
            state.event_sender = None
            payload_sender.close()
            state.payload_sender = None
            capability_sources: dict[str, CompilerExecutableCapability] = {}
            for configuration in self.configurations:
                existing = capability_sources.setdefault(
                    configuration.compiler_capability_digest,
                    configuration.compiler_capability,
                )
                if existing is not configuration.compiler_capability:
                    raise AuditInfrastructureError(
                        "compiler capability generation source differs"
                    )
            transfer_cookie = os.urandom(32).hex()
            transferred_handles: dict[str, tuple[int, ...]] = {}
            duplicate_sources = dict(generation_duplicates)
            if os.name == "nt":
                for digest, duplicate in duplicate_sources.items():
                    transferred_handles[digest] = (
                        _duplicate_windows_capability_streams(
                            process,
                            duplicate.streams,
                        )
                    )
            bootstrap = _encode_worker_bootstrap(
                self.configurations,
                self.dependency_roots,
                self.production_snapshot,
                self.limits,
                self.engine,
                self.runtime_contract.pipeline_deadline,
                transfer_cookie=transfer_cookie,
                transferred_handles=transferred_handles,
            )
            try:
                startup_sender.send_bytes_before(
                    bootstrap,
                    self.runtime_contract.pipeline_deadline,
                    cancel_event=self.cancel_event,
                )
                if os.name != "nt":
                    _send_posix_capability_streams(
                        capability_transfer_parent,
                        tuple(
                            stream
                            for digest in sorted(duplicate_sources)
                            for stream in duplicate_sources[digest].streams
                        ),
                        transfer_cookie,
                        self.runtime_contract.pipeline_deadline,
                        self.cancel_event,
                    )
                for _digest, duplicate in generation_duplicates:
                    transfer_duplicate(duplicate, process)
            except BaseException as startup_error:
                if event_receiver.poll(1.0):
                    failure = decode_control_message(
                        event_receiver.receive_bytes_before(
                            time.monotonic() + 1.0
                        )
                    )
                    if isinstance(failure, WorkerFailure):
                        raise AuditInfrastructureError(
                            failure.diagnostic
                        ) from startup_error
                raise
            startup_sender.close()
            state.startup_sender = None
            if capability_transfer_parent is not None:
                capability_transfer_parent.close()
                state.capability_transfer_parent = None
            state.setup_finalized = True
            return state
        except BaseException as spawn_error:
            try:
                cleanup_failures = self.abort_and_reap_until_closed(
                    emergency_cleanup_deadline(
                        _generation_cleanup_ceiling(self)
                    )
                )
                for cleanup_error in cleanup_failures:
                    spawn_error.add_note(
                        f"worker reactor cleanup retry failed: {cleanup_error}"
                    )
                    for note in getattr(cleanup_error, "__notes__", ()):
                        spawn_error.add_note(
                            f"worker reactor cleanup detail: {note}"
                        )
            except BaseException as cleanup_error:
                spawn_error.add_note(
                    "worker reactor cleanup also failed: "
                    f"{cleanup_error}"
                )
            raise spawn_error

    def _wait_all_ready(self) -> None:
        while any(
            state is not None and not state.ready for state in self.states
        ):
            state, event = self._next_event_raw()
            if isinstance(event, WorkerContained):
                if isinstance(self.native_lifecycle, _LinuxGenerationLifecycle):
                    if not state.contained:
                        raise AuditInfrastructureError(
                            "Linux worker containment acknowledgement differs"
                        )
                else:
                    if state.contained or event.worker_pid != state.process.pid:
                        raise AuditInfrastructureError(
                            "worker containment acknowledgement differs"
                        )
                    state.contained = True
            elif isinstance(event, MacOSWorkerSessionReported):
                if (
                    not isinstance(
                        self.native_lifecycle, _MacOSGenerationLifecycle
                    )
                    or not state.contained
                    or event.pid != state.process.pid
                    or event.pgid != state.process.pid
                ):
                    raise AuditInfrastructureError(
                        "macOS worker session acknowledgement differs"
                    )
            elif isinstance(event, WorkerCapabilitiesAccepted):
                expected_digests = tuple(sorted({
                    configuration.compiler_capability_digest
                    for configuration in self.configurations
                }))
                if (
                    not state.contained
                    or state.capabilities_accepted
                    or state.ready
                    or event.capability_digests != expected_digests
                ):
                    raise AuditInfrastructureError(
                        "worker compiler capability acceptance differs"
                    )
                acknowledge_generation = getattr(
                    self.capability_registry,
                    "acknowledge_generation",
                    None,
                )
                if not callable(acknowledge_generation):
                    raise AuditInfrastructureError(
                        "compiler capability registry acknowledgement is invalid"
                    )
                acknowledge_generation(
                    state.worker_index,
                    state.generation,
                    event.capability_digests,
                )
                self.registry_generation_duplicates.pop(
                    (state.worker_index, state.generation), None
                )
                state.capabilities_accepted = True
            elif isinstance(event, WorkerEngineReady):
                expected_digests = tuple(sorted({
                    configuration.compiler_capability_digest
                    for configuration in self.configurations
                }))
                if (
                    not state.contained
                    or not state.capabilities_accepted
                    or state.ready
                    or event.worker_pid != state.process.pid
                    or event.audit_engine_fingerprint != self.engine
                    or event.capability_digests != expected_digests
                ):
                    raise AuditInfrastructureError(
                        "worker audit engine fingerprint readiness differs"
                    )
                state.ready = True
            elif isinstance(event, WorkerFailure):
                raise AuditInfrastructureError(event.diagnostic)
            elif state.ready:
                self.deferred_events.append((state, event))
            else:
                raise AuditInfrastructureError(
                    "worker emitted task event before readiness"
                )

    def _sample_owned_forest(self) -> _OwnedForestSample:
        from gpu_capability_process_tree import (
            OwnedProcessIdentity, native_process_resident_bytes,
        )

        parent_resident = _current_process_rss_bytes()
        if isinstance(self.native_lifecycle, _LinuxGenerationLifecycle):
            memory = self.native_lifecycle.memory_measurements(
                require_empty=False
            )
            return _OwnedForestSample(
                "linux",
                memory.cgroup_peak_accounted_memory_bytes,
                memory.accounting_complete,
                tuple(
                    state.process.pid for state in self.states
                    if state is not None and state.process.is_alive()
                ),
            )
        if isinstance(self.native_lifecycle, _MacOSGenerationLifecycle):
            self.native_lifecycle.observe()
            memory = self.native_lifecycle.accountant.memory_measurements()
            return _OwnedForestSample(
                "macos",
                memory.maximum_observed_aggregate_resident_bytes,
                memory.known_unreconciled_descendant_count == 0,
                tuple(
                    state.process.pid for state in self.states
                    if state is not None and state.process.is_alive()
                ),
            )
        observe_run = getattr(self.run_accountant, "observe", None)
        authoritative_windows_resident = None
        if callable(observe_run) and os.name == "nt":
            if self._accounting_error is not None:
                raise AuditInfrastructureError(
                    "Windows Job accounting pump failed"
                ) from self._accounting_error
            authoritative_windows_resident = getattr(
                self.run_accountant,
                "_simultaneous_peak",
                None,
            )
        surviving = []
        live_slot_resident: dict[int, int] = {}
        accounting_complete = True
        platform_kind = (
            "windows" if os.name == "nt"
            else ("macos" if sys.platform == "darwin" else "linux")
        )
        for state in self.states:
            if state is None or not state.process.is_alive():
                continue
            surviving.append(state.process.pid)
            try:
                resident = native_process_resident_bytes(state.process.pid)
                state.tree.observe_resident_bytes(
                    state.identity,
                    current_bytes=resident,
                    peak_bytes=max(resident, state.maximum_resident_bytes),
                )
                descendants = _native_descendant_processes(state.process.pid)
                observed_pids = {pid for pid, _token, _resident in descendants}
                for pid, token, descendant_resident in descendants:
                    identity = state.active_descendants.get(pid)
                    if identity is None:
                        identity = OwnedProcessIdentity(
                            platform_kind, pid, token
                        )
                        state.tree.register(
                            identity, state.identity, "audit-compiler-descendant"
                        )
                        state.active_descendants[pid] = identity
                    elif identity.native_start_identity != token:
                        raise AuditInfrastructureError(
                            "owned process start identity changed"
                        )
                    state.tree.observe_resident_bytes(
                        identity,
                        current_bytes=descendant_resident,
                        peak_bytes=descendant_resident,
                    )
                for pid in tuple(state.active_descendants):
                    if pid in observed_pids:
                        continue
                    identity = state.active_descendants.pop(pid)
                    state.tree.mark_exited(identity)
                    state.tree.reap(identity)
                generation_resident = resident + sum(
                    value[2] for value in descendants
                )
                state.maximum_resident_bytes = max(
                    state.maximum_resident_bytes, generation_resident
                )
                live_slot_resident[state.worker_index] = generation_resident
            except AuditInfrastructureError:
                accounting_complete = False
        total = _combine_owned_generation_memory(
            parent_resident,
            live_slot_resident,
            self.archived_generation_telemetry,
        )
        if authoritative_windows_resident is not None:
            total = authoritative_windows_resident
        return _OwnedForestSample(
            platform_kind,
            total,
            accounting_complete,
            tuple(surviving),
        )

    def next_event(self) -> tuple[_GenerationState, object]:
        if self.deferred_events:
            return self.deferred_events.popleft()
        return self._next_event_raw()

    def _next_event_raw(self) -> tuple[_GenerationState, object]:
        while True:
            if time.monotonic() >= self.runtime_contract.pipeline_deadline:
                raise AuditInfrastructureError("pipeline deadline exceeded")
            for state in self.states:
                if state is None:
                    continue
                if state.event_receiver.poll(0):
                    payload = state.event_receiver.receive_bytes_before(
                        self.runtime_contract.pipeline_deadline,
                        worker_alive=state.process.is_alive,
                    )
                    event = decode_control_message(payload)
                    if (
                        event.worker_index != state.worker_index
                        or event.generation != state.generation
                    ):
                        raise AuditInfrastructureError(
                            "worker control generation differs"
                        )
                    if (
                        isinstance(event, WorkerContained)
                        and isinstance(
                            self.native_lifecycle, _LinuxGenerationLifecycle
                        )
                    ):
                        if state.contained or event.worker_pid != state.process.pid:
                            raise AuditInfrastructureError(
                                "Linux worker containment acknowledgement differs"
                            )
                        self.native_lifecycle.accept_worker_contained(
                            event, state.native_carrier
                        )
                        state.contained = True
                    elif isinstance(event, MacOSWorkerSessionReported):
                        if (
                            not isinstance(
                                self.native_lifecycle,
                                _MacOSGenerationLifecycle,
                            )
                            or state.contained
                            or event.pid != state.process.pid
                            or event.pgid != state.process.pid
                        ):
                            raise AuditInfrastructureError(
                                "macOS worker session acknowledgement differs"
                            )
                        self.native_lifecycle.accept_worker_session(event)
                        state.contained = True
                        state.native_generation_acquired = True
                    elif isinstance(event, CompilerPgidReported):
                        if (
                            state.pending is None
                            or not isinstance(
                                self.native_lifecycle,
                                _MacOSGenerationLifecycle,
                            )
                            or event.task_id != state.pending.ordinal
                        ):
                            raise AuditInfrastructureError(
                                "macOS compiler PGID task differs"
                            )
                        permit = self.native_lifecycle.permit_compiler(event)
                        self._send_command(state, permit)
                    if isinstance(
                        event,
                        (
                            MacOSWorkerSessionReported,
                            WorkerFailure,
                            WorkerStopped,
                        ),
                    ):
                        # These events can race with an immediate worker exit.
                        # Dispatch them before sampling the registered native
                        # process group so a queued bounded failure diagnostic
                        # or normal stop cannot be masked by a missing leader.
                        return state, event
                    if isinstance(event, WorkerPayloadReady):
                        if state.pending is None:
                            raise AuditInfrastructureError(
                                "worker result payload task is unavailable"
                            )
                        _validate_worker_payload_ready_receipt(
                            event, state.pending
                        )
                        if isinstance(
                            self.native_lifecycle, _MacOSGenerationLifecycle
                        ):
                            self.native_lifecycle.reconcile_task(
                                state.worker_index,
                                state.generation,
                                state.pending.ordinal,
                            )
                    sample = self._sample_owned_forest()
                    _enforce_platform_memory_contract(
                        sample, self.result_budget
                    )
                    return state, event
                if not state.process.is_alive() and state.process.exitcode is not None:
                    raise AuditInfrastructureError(
                        "worker exited before WorkerStopped"
                    )
            if self.cancel_event.is_set():
                raise AuditInfrastructureError("worker audit was cancelled")
            sample = self._sample_owned_forest()
            _enforce_platform_memory_contract(sample, self.result_budget)
            time.sleep(min(
                0.01,
                max(0.0, self.runtime_contract.pipeline_deadline - time.monotonic()),
            ))

    def send_task(
        self,
        state: _GenerationState,
        ordinal: int,
        configuration: PreprocessConfiguration,
        slot: CompactResultSlot,
    ) -> bool:
        if state.pending is not None or not state.ready or state.expects_retire:
            raise AuditInfrastructureError("worker generation is not idle")
        task_id = f"audit-{ordinal}-{configuration.digest[:16]}"
        if self.maximum_queued_counting_bytes > (
            self.result_budget.maximum_bytes - self.result_budget.live_bytes
        ):
            return False
        queued_ownership = self.result_budget.reserve(
            self.maximum_queued_counting_bytes,
            label=f"task:{task_id}:queued-transport",
            semantic_event="reserve-dispatch",
        ).commit()
        reservation = None
        capability = None
        try:
            reservation = PerTaskCompactReservation(
                task_id, state.generation, slot.worst_case_live_bytes
            )
            capability = reservation.issue_worker_transport_capability(
                task_id,
                state.generation,
                configuration.digest,
                self.engine,
                state.worker_index,
            )
            task = ConfigurationAuditTask(
                task_id,
                state.generation,
                configuration,
                self.dependency_roots,
                reservation,
            )
            frame = encode_task_frame(
                ordinal, state.worker_index, task, capability
            )
            state.task_sender.send_bytes_before(
                frame,
                self.runtime_contract.pipeline_deadline,
                cancel_event=self.cancel_event,
            )
        except BaseException:
            if (
                reservation is not None
                and capability is not None
                and not reservation.released
            ):
                reservation.release_worker_transport_capability(
                    capability, "worker-dispatch-failure"
                )
            if not queued_ownership.released:
                queued_ownership.release()
            raise
        state.pending = _PendingCoordinatorTask(
            ordinal,
            configuration,
            task_id,
            reservation,
            capability,
            queued_ownership,
        )
        state.launch_events.clear()
        return True

    def _send_command(self, state: _GenerationState, message) -> None:
        state.command_sender.send_bytes_before(
            encode_control_message(message),
            self.runtime_contract.pipeline_deadline,
            cancel_event=self.cancel_event,
        )

    def grant_next_publication(self) -> None:
        if self.active_publication is not None:
            return
        while self.publication_requests:
            worker_index, generation, task_id = self.publication_requests.popleft()
            state = self.states[worker_index]
            if state is None or state.generation != generation:
                continue
            pending = state.pending
            if pending is None or pending.task_id != task_id:
                raise AuditInfrastructureError(
                    "root publication request task differs"
                )
            self._send_command(
                state, WorkerPayloadPermit(worker_index, generation, task_id)
            )
            pending.queued_ownership.record_semantic(
                "activate-publication"
            )
            self.active_publication = (worker_index, generation, task_id)
            return

    def handle_task_event(
        self,
        state: _GenerationState,
        event,
        aggregator: StreamingResultAggregator,
    ) -> tuple[str, int] | None:
        pending = state.pending
        if isinstance(event, CompilerPgidReported):
            if (
                pending is None
                or not isinstance(
                    self.native_lifecycle, _MacOSGenerationLifecycle
                )
                or event.task_id != pending.ordinal
                or event.pgid not in self.native_lifecycle.compiler_groups.get(
                    (state.worker_index, state.generation, pending.ordinal), ()
                )
            ):
                raise AuditInfrastructureError(
                    "macOS compiler PGID task differs"
                )
            return None
        if isinstance(event, CompilerLaunchEvent):
            if pending is None or event.task_id != pending.task_id:
                raise AuditInfrastructureError(
                    "audit compiler launch task differs"
                )
            state.launch_events.append(event)
            _scheduler_compiler_launch_event(event)
            if len(state.launch_events) > 2:
                raise AuditInfrastructureError(
                    "audit compiler launch protocol differs"
                )
            return None
        if isinstance(event, CachePublicationRequested):
            if pending is None or event.task_id != pending.task_id:
                raise AuditInfrastructureError(
                    "root publication request task differs"
                )
            request = (state.worker_index, state.generation, event.task_id)
            _scheduler_cache_publication_event(event)
            if request == self.active_publication or request in self.publication_requests:
                raise AuditInfrastructureError(
                    "root publication request was duplicated"
                )
            if len(self.publication_requests) >= len(self.states):
                raise AuditInfrastructureError(
                    "root publication request queue exceeds workers"
                )
            self.publication_requests.append(request)
            self.grant_next_publication()
            return None
        if isinstance(event, WorkerPayloadReady):
            if pending is None or event.task_id != pending.task_id:
                raise AuditInfrastructureError(
                    "worker result payload task differs"
                )
            active = (state.worker_index, state.generation, event.task_id)
            if self.active_publication != active:
                raise AuditInfrastructureError(
                    "worker result payload publication differs"
                )
            _validate_worker_payload_ready_receipt(event, pending)
            if tuple(item.purpose for item in state.launch_events) != (
                CompilerLaunchPurpose.AUDIT_DISCOVERY,
                CompilerLaunchPurpose.AUDIT_ACCEPTED,
            ):
                raise AuditInfrastructureError(
                    "audit compiler launch protocol differs"
                )
            pending.queued_ownership.record_semantic(
                "release-publication"
            )
            self.active_publication = None
            self._send_command(
                state,
                WorkerPayloadPermit(
                    state.worker_index, state.generation, pending.task_id
                ),
            )
            pending.queued_ownership.record_semantic("send-pipe")
            # Publication capacity is generation-bound but independent from
            # the current payload receive/decode.  Advance the FIFO before
            # waiting on payload bytes so another worker cannot be stranded.
            self.grant_next_publication()
            owner = None
            retained = None
            cold_slot_bytes = 0
            try:
                worker_outcome = state.payload_receiver.receive_transport_before(
                    pending.capability,
                    self.runtime_contract.pipeline_deadline,
                    cancel_event=self.cancel_event,
                    worker_alive=state.process.is_alive,
                )
                owner, retained, cold_slot_bytes = (
                    _decode_authenticated_worker_payload(
                        event, pending, worker_outcome, aggregator
                    )
                )
                outcome = owner.outcome
                aggregator.accept_validated_result(
                    pending.configuration, outcome.result, retained
                )
                if event.stdout_bytes > (1 << 64) - 1 - self.stdout_bytes:
                    raise AuditInfrastructureError(
                        "authenticated worker stdout measurement overflows"
                    )
                self.stdout_bytes += event.stdout_bytes
                self.maximum_encoded_result_bytes = max(
                    self.maximum_encoded_result_bytes, event.encoded_bytes
                )
                self.maximum_conservative_decoded_bytes = max(
                    self.maximum_conservative_decoded_bytes,
                    event.conservative_decoded_bytes,
                )
                self.maximum_conservative_retained_bytes = max(
                    self.maximum_conservative_retained_bytes,
                    event.conservative_retained_bytes,
                )
            except BaseException as error:
                _release_rejected_worker_transport(
                    pending.reservation,
                    pending.capability,
                    error,
                    "worker payload reactor",
                )
                raise
            finally:
                if retained is not None and not retained.released:
                    retained.release()
                if owner is not None and owner.active:
                    owner.close()
            aggregator.reserve_cold_slot(
                CompactResultColdSlot(cold_slot_bytes)
            )
            state.pending = None
            state.tasks_completed += 1
            self.audit_launch_count += 2
            state.expects_retire = (
                state.tasks_completed
                >= self.runtime_contract.maximum_tasks_per_worker
            ) or (
                self.runtime_contract.recycle_rss_bytes > 0
                and state.maximum_resident_bytes
                >= self.runtime_contract.recycle_rss_bytes
            )
            self.grant_next_publication()
            return None
        if isinstance(event, WorkerRetire):
            if state.pending is not None or (
                not state.expects_retire and event.reason != "resident-memory"
            ):
                raise AuditInfrastructureError(
                    "worker retired with outstanding work"
                )
            state.expects_retire = True
            self._send_command(
                state,
                WorkerRetireAck(state.worker_index, state.generation),
            )
            worker_index = state.worker_index
            self._archive_generation(state, require_stopped=False)
            return ("retired", worker_index)
        if isinstance(event, WorkerFailure):
            raise AuditInfrastructureError(event.diagnostic)
        raise AuditInfrastructureError("invalid worker protocol message")

    def _archive_generation(
        self, state: _GenerationState, *, require_stopped: bool
    ) -> None:
        if require_stopped == state.expects_retire:
            raise AuditInfrastructureError(
                "worker generation shutdown protocol differs"
            )
        remaining = self.runtime_contract.pipeline_deadline - time.monotonic()
        if remaining <= 0:
            raise AuditInfrastructureError("pipeline deadline exceeded during reap")
        state.process.join(timeout=remaining)
        if state.process.is_alive():
            raise AuditInfrastructureError("worker generation did not exit")
        if state.process.exitcode != 0:
            raise AuditInfrastructureError(
                "worker generation exited unsuccessfully"
            )
        self._generation_progress(state).add("process-reaped")
        self._teardown_generation_ownership(
            state, self.runtime_contract.pipeline_deadline, force=False
        )

    @staticmethod
    def _generation_progress(state) -> set[str]:
        progress = getattr(state, "teardown_completed", None)
        if progress is None:
            progress = set()
            state.teardown_completed = progress
        if not isinstance(progress, set) or any(
            not isinstance(label, str) for label in progress
        ):
            raise AuditInfrastructureError(
                "generation teardown progress is invalid"
            )
        return progress

    @staticmethod
    def _complete_generation_setup_transition(state, label: str, action):
        progress = getattr(state, "setup_completed", None)
        if progress is None:
            progress = set()
            state.setup_completed = progress
        if not isinstance(progress, set) or any(
            not isinstance(value, str) for value in progress
        ):
            raise AuditInfrastructureError(
                "generation setup progress is invalid"
            )
        if label in progress:
            return None
        _generation_setup_before_transition(label, state)
        result = action()
        progress.add(label)
        _generation_setup_transition(label, state)
        return result

    def _complete_generation_transition(
        self, state, label: str, action
    ) -> None:
        progress = self._generation_progress(state)
        if label in progress:
            return
        _generation_teardown_before_transition(label, state)
        action()
        progress.add(label)
        _generation_teardown_transition(label, state)

    def _release_generation_process_tree(self, state) -> None:
        from gpu_capability_process_tree import native_process_resident_bytes

        tree = getattr(state, "tree", None)
        identity = getattr(state, "identity", None)
        active_descendants = getattr(state, "active_descendants", None)
        if tree is None or identity is None or active_descendants is None:
            return
        for pid, descendant_identity in tuple(active_descendants.items()):
            try:
                native_process_resident_bytes(pid)
            except AuditInfrastructureError:
                tree.mark_exited(descendant_identity)
                tree.reap(descendant_identity)
                active_descendants.pop(pid)
            else:
                raise AuditInfrastructureError(
                    "worker process-tree survivors remain"
                )
        tree.mark_exited(identity)
        tree.reap(identity)
        if tree.surviving_owned_process_count != 0:
            raise AuditInfrastructureError(
                "worker process-tree survivors remain"
            )

    def _teardown_generation_ownership(
        self, state, deadline: float, *, force: bool
    ) -> None:
        key = (state.worker_index, state.generation)

        def release_native():
            acquired = getattr(
                state,
                "native_generation_acquired",
                self.native_lifecycle is not None,
            )
            acquired = acquired or getattr(
                state, "native_create_requested", False
            )
            if self.native_lifecycle is not None and acquired:
                self.native_lifecycle.release_generation(
                    state.worker_index,
                    state.generation,
                    state.native_carrier,
                    deadline,
                    force=force,
                )

        self._complete_generation_transition(
            state, "native-release", release_native
        )
        self._complete_generation_transition(
            state,
            "process-tree-release",
            lambda: self._release_generation_process_tree(state),
        )

        def archive_accounting():
            _release_generation_job_accounting(
                self.run_accountant,
                state,
                deadline,
                self._accounting_lock,
            )

        self._complete_generation_transition(
            state, "accounting-archive", archive_accounting
        )

        def detach_publication():
            self.publication_requests = collections.deque(
                request for request in self.publication_requests
                if request[:2] != key
            )
            if (
                self.active_publication is not None
                and self.active_publication[:2] == key
            ):
                self.active_publication = None

        self._complete_generation_transition(
            state, "publication-detach", detach_publication
        )
        pending = state.pending

        def release_reservation():
            if pending is not None and not pending.reservation.released:
                pending.reservation.release_worker_transport_capability(
                    pending.capability, "worker-failure"
                )

        self._complete_generation_transition(
            state, "reservation-release", release_reservation
        )

        def release_registry():
            release_generation = getattr(
                self.capability_registry, "release_generation", None
            )
            acquired = getattr(state, "registry_generation_acquired", None)
            if callable(release_generation) and acquired is not False:
                release_generation(state.worker_index, state.generation)

        self._complete_generation_transition(
            state, "registry-release", release_registry
        )
        self._complete_generation_transition(
            state,
            "registry-duplicate-pop",
            lambda: self.registry_generation_duplicates.pop(key, None),
        )
        telemetry = (
            state.worker_index,
            state.generation,
            getattr(state, "maximum_resident_bytes", 0),
        )

        def append_telemetry():
            required = _generation_has_accounted_processes(state)
            if required and telemetry not in self.archived_generation_telemetry:
                self.archived_generation_telemetry.append(telemetry)

        self._complete_generation_transition(
            state, "telemetry-append", append_telemetry
        )

        def remove_scratch():
            if state.scratch_root is None:
                return
            try:
                shutil.rmtree(state.scratch_root)
            except FileNotFoundError:
                pass

        self._complete_generation_transition(
            state, "scratch-removal", remove_scratch
        )
        def append_scratch():
            if (
                state.scratch_root is not None
                and state.scratch_root not in self.archived_scratch_roots
            ):
                self.archived_scratch_roots.append(state.scratch_root)

        self._complete_generation_transition(
            state, "scratch-append", append_scratch
        )

        def release_queued():
            if pending is not None and not pending.queued_ownership.released:
                pending.queued_ownership.release()

        self._complete_generation_transition(
            state, "queued-release", release_queued
        )
        for label, endpoint in (
            ("startup-receiver-close", getattr(state, "startup_receiver", None)),
            ("startup-endpoint-close", state.startup_sender),
            ("task-receiver-close", getattr(state, "task_receiver", None)),
            ("task-endpoint-close", state.task_sender),
            ("command-receiver-close", getattr(state, "command_receiver", None)),
            ("command-endpoint-close", state.command_sender),
            ("event-endpoint-close", state.event_receiver),
            ("event-sender-close", getattr(state, "event_sender", None)),
            ("payload-endpoint-close", state.payload_receiver),
            ("payload-sender-close", getattr(state, "payload_sender", None)),
            (
                "capability-transfer-parent-close",
                getattr(state, "capability_transfer_parent", None),
            ),
            (
                "capability-transfer-child-close",
                getattr(state, "capability_transfer_child", None),
            ),
        ):
            if endpoint is not None:
                self._complete_generation_transition(
                    state, label, endpoint.close
                )

        def remove_deferred_events():
            deferred_events = getattr(self, "deferred_events", None)
            if deferred_events is None:
                return
            self.deferred_events = collections.deque(
                item for item in deferred_events if item[0] is not state
            )

        self._complete_generation_transition(
            state, "deferred-events-remove", remove_deferred_events
        )

        def remove_worker_pid():
            process = getattr(state, "process", None)
            pid = getattr(process, "pid", None)
            worker_pids = getattr(self, "worker_pids", None)
            if (
                not getattr(state, "setup_finalized", True)
                and worker_pids is not None
                and pid in worker_pids
            ):
                worker_pids.remove(pid)

        self._complete_generation_transition(
            state, "worker-pid-remove", remove_worker_pid
        )
        self._complete_generation_transition(
            state,
            "state-clear",
            lambda: self.states.__setitem__(state.worker_index, None),
        )

    def shutdown_reap(self, deadline: float) -> None:
        try:
            self._shutdown_reap_normal(deadline)
        except BaseException as primary_error:
            try:
                self.abort_and_reap_until_closed(
                    emergency_cleanup_deadline(
                        _generation_cleanup_ceiling(
                            self, fallback_deadline=deadline
                        )
                    )
                )
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"emergency generation cleanup also failed: {cleanup_error!r}")
                for note in getattr(cleanup_error, "__notes__", ()):
                    primary_error.add_note(
                        f"emergency generation cleanup detail: {note}")
            raise

    def _shutdown_reap_normal(self, deadline: float) -> None:
        if self._closed:
            return
        for state in tuple(self.states):
            if state is None:
                continue
            if state.pending is not None:
                raise AuditInfrastructureError(
                    "normal shutdown has outstanding worker task"
                )
            state.task_sender.send_bytes_before(
                encode_control_message(
                    WorkerStop(state.worker_index, state.generation)
                ),
                deadline,
            )
        remaining = sum(state is not None for state in self.states)
        while remaining:
            state, event = self.next_event()
            if not isinstance(event, WorkerStopped):
                raise AuditInfrastructureError(
                    "worker exited before WorkerStopped"
                )
            self._archive_generation(state, require_stopped=True)
            remaining -= 1
        self._finalize_shutdown(deadline)
        self._closed = True

    def _shutdown_progress(self) -> set[str]:
        progress = getattr(self, "_shutdown_completed", None)
        if progress is None:
            progress = set()
            self._shutdown_completed = progress
        if not isinstance(progress, set) or any(
            not isinstance(label, str) for label in progress
        ):
            raise AuditInfrastructureError(
                "generation shutdown progress is invalid"
            )
        return progress

    def _complete_shutdown_transition(self, label: str, action) -> None:
        progress = self._shutdown_progress()
        if label in progress:
            return
        _shutdown_before_transition(label, self)
        action()
        progress.add(label)
        _shutdown_transition(label, self)

    def _finalize_shutdown(self, deadline: float) -> None:
        def require_empty_queues():
            if (
                any(state is not None for state in self.states)
                or getattr(self, "publication_requests", ())
                or getattr(self, "active_publication", None) is not None
                or getattr(self, "deferred_events", ())
            ):
                raise AuditInfrastructureError(
                    "generation shutdown queues are not empty"
                )

        self._complete_shutdown_transition(
            "queues-empty", require_empty_queues
        )
        self._complete_shutdown_transition(
            "accounting-pump-stop", self._stop_accounting_pump
        )

        def seal_lifecycle_snapshot():
            snapshot = getattr(
                self, "_lifecycle_task_phase_snapshot", None
            )
            if (
                snapshot is None
                and isinstance(
                    self.native_lifecycle, _LinuxGenerationLifecycle
                )
            ):
                snapshot = self.native_lifecycle.seal_task_phase(
                    len(self.archived_generation_telemetry)
                )
                self._lifecycle_task_phase_snapshot = snapshot
            if isinstance(self.native_lifecycle, _LinuxGenerationLifecycle):
                if type(snapshot) is not LinuxPhaseSnapshot:
                    raise AuditInfrastructureError(
                        "Linux task phase snapshot differs"
                    )
            elif (
                snapshot is None
                and isinstance(
                    self.native_lifecycle, _MacOSGenerationLifecycle
                )
            ):
                snapshot = self.native_lifecycle.seal_task_phase(
                    len(self.archived_generation_telemetry)
                )
                self._lifecycle_task_phase_snapshot = snapshot
            if isinstance(self.native_lifecycle, _MacOSGenerationLifecycle):
                if type(snapshot) is not MacOSPhaseSnapshot:
                    raise AuditInfrastructureError(
                        "macOS task phase snapshot differs"
                    )

        self._complete_shutdown_transition(
            "lifecycle-task-snapshot", seal_lifecycle_snapshot
        )

        def seal_task_phase_native():
            if _is_unix_attempt_accountant(self.run_accountant):
                snapshot = getattr(
                    self, "_run_accountant_task_phase_snapshot", None
                )
                if snapshot is None:
                    lifecycle_snapshot = getattr(
                        self, "_lifecycle_task_phase_snapshot", None
                    )
                    if (
                        type(self.run_accountant)
                        is LinuxCompilerAuditAttemptAccountant
                        and type(lifecycle_snapshot) is LinuxPhaseSnapshot
                    ):
                        snapshot = (
                            self.run_accountant
                            .seal_phase_from_lifecycle_snapshot(
                                "tasks", deadline, lifecycle_snapshot
                            )
                        )
                    else:
                        snapshot = self.run_accountant.seal_phase(
                            "tasks", deadline
                        )
                    self._run_accountant_task_phase_snapshot = snapshot
                expected_type = (
                    LinuxPhaseSnapshot
                    if isinstance(
                        self.run_accountant,
                        LinuxCompilerAuditAttemptAccountant,
                    )
                    else MacOSPhaseSnapshot
                )
                if type(snapshot) is not expected_type:
                    raise AuditInfrastructureError(
                        "Unix attempt task phase snapshot differs"
                    )
            elif _is_windows_native_accountant(self.run_accountant):
                self.run_accountant.seal_phase(
                    deadline, _current_process_rss_bytes()
                )

        self._complete_shutdown_transition(
            "task-phase-native-seal", seal_task_phase_native
        )

        def capture_raw_snapshot():
            if _is_windows_native_accountant(self.run_accountant):
                from gpu_capability_process_tree import (
                    WindowsJobAccountingSnapshot,
                )
                captured = self.run_accountant.snapshot()
                if type(captured) is not WindowsJobAccountingSnapshot:
                    raise AuditInfrastructureError(
                        "Windows phase accounting snapshot differs"
                    )
                self._task_phase_raw_snapshot = captured
            elif _is_unix_attempt_accountant(self.run_accountant):
                captured = getattr(
                    self, "_run_accountant_task_phase_snapshot", None
                )
                expected_type = (
                    LinuxPhaseSnapshot
                    if isinstance(
                        self.run_accountant,
                        LinuxCompilerAuditAttemptAccountant,
                    )
                    else MacOSPhaseSnapshot
                )
                if type(captured) is not expected_type:
                    raise AuditInfrastructureError(
                        "Unix attempt task phase snapshot differs"
                    )
                self._task_phase_raw_snapshot = captured

        self._complete_shutdown_transition(
            "task-phase-raw-snapshot", capture_raw_snapshot
        )

        def validate_phase_memory():
            from gpu_capability_process_tree import WindowsJobAccountingSnapshot

            raw = getattr(self, "_task_phase_raw_snapshot", None)
            if _is_windows_native_accountant(self.run_accountant):
                if type(raw) is not WindowsJobAccountingSnapshot:
                    raise AuditInfrastructureError(
                        "Windows phase accounting snapshot differs"
                    )
                memory = raw.memory_measurements()
                if type(memory) is not WindowsRunMemoryMeasurements:
                    raise AuditInfrastructureError(
                        "Windows phase memory accounting differs"
                    )
                if (
                    not memory.accounting_complete
                    or memory.surviving_job_process_count != 0
                ):
                    raise AuditInfrastructureError(
                        "Windows tasks phase survivors remain"
                    )
                identities = _canonical_archived_generation_identities(
                    self.archived_generation_telemetry
                )
                _validate_windows_phase_generation_identities(
                    raw.archived_generation_identities,
                    identities,
                    "tasks",
                )
                self._task_phase_generation_identities = identities
                self._task_phase_memory = memory
                return
            if _is_unix_attempt_accountant(self.run_accountant):
                if type(raw) is LinuxPhaseSnapshot:
                    platform_kind = "linux"
                    expected_memory_type = LinuxRunMemoryMeasurements
                elif type(raw) is MacOSPhaseSnapshot:
                    platform_kind = "macos"
                    expected_memory_type = MacOSRunMemoryMeasurements
                else:
                    raise AuditInfrastructureError(
                        "Unix lifecycle task phase snapshot differs"
                    )
                expected_count = len(self.archived_generation_telemetry)
                lifecycle_snapshot = getattr(
                    self, "_lifecycle_task_phase_snapshot", None
                )
                if (
                    raw.platform_kind != platform_kind
                    or raw.phase != "tasks"
                    or raw.archived_generation_count != 0
                    or type(lifecycle_snapshot) is not type(raw)
                    or lifecycle_snapshot.platform_kind != platform_kind
                    or lifecycle_snapshot.phase != "tasks"
                    or lifecycle_snapshot.archived_generation_count
                    != expected_count
                ):
                    raise AuditInfrastructureError(
                        "Unix task phase snapshots differ"
                    )
                memory = raw.memory
                if (
                    type(memory) is not expected_memory_type
                    or memory != lifecycle_snapshot.memory
                ):
                    raise AuditInfrastructureError(
                        "Unix lifecycle and attempt phase memory differ"
                    )
                if type(memory) is LinuxRunMemoryMeasurements:
                    complete = (
                        platform_kind == "linux"
                        and memory.accounting_complete
                        and memory.surviving_cgroup_process_count == 0
                    )
                elif type(memory) is MacOSRunMemoryMeasurements:
                    complete = (
                        platform_kind == "macos"
                        and memory.accounting_complete
                        and memory.surviving_registered_process_count == 0
                        and memory.known_unreconciled_descendant_count == 0
                    )
                else:
                    raise AuditInfrastructureError(
                        "Unix attempt task phase memory backend differs"
                    )
                if not complete:
                    raise AuditInfrastructureError(
                        "Unix attempt task phase survivors remain"
                    )
                self._task_phase_generation_identities = expected_count
                self._task_phase_memory = memory

        self._complete_shutdown_transition(
            "task-phase-memory-validation", validate_phase_memory
        )

        def construct_typed_snapshot():
            memory = getattr(self, "_task_phase_memory", None)
            if memory is None:
                return
            if type(memory) is WindowsRunMemoryMeasurements:
                platform_kind = "windows"
            elif type(memory) is LinuxRunMemoryMeasurements:
                platform_kind = "linux"
            elif type(memory) is MacOSRunMemoryMeasurements:
                platform_kind = "macos"
            else:
                raise AuditInfrastructureError(
                    "task phase memory backend differs"
                )
            self._sealed_task_phase_snapshot = (
                _construct_task_phase_snapshot(
                    platform_kind,
                    "tasks",
                    memory,
                    self._task_phase_generation_identities,
                )
            )

        self._complete_shutdown_transition(
            "task-phase-typed-snapshot", construct_typed_snapshot
        )

        def store_task_phase_snapshot():
            snapshot = getattr(self, "_sealed_task_phase_snapshot", None)
            if snapshot is not None:
                if not isinstance(
                    snapshot,
                    (WindowsPhaseSnapshot, LinuxPhaseSnapshot, MacOSPhaseSnapshot),
                ):
                    raise AuditInfrastructureError(
                        "sealed task phase snapshot differs"
                    )
                self.task_phase_snapshot = snapshot

        self._complete_shutdown_transition(
            "task-phase-snapshot-store", store_task_phase_snapshot
        )

    def abort_and_reap(self, deadline: float) -> None:
        cleanup_errors: list[tuple[str, BaseException]] = []

        def attempt(label, action):
            try:
                return action()
            except BaseException as cleanup_error:
                cleanup_errors.append((label, cleanup_error))
                return None

        attempt("worker cancellation", self.cancel_event.set)
        for state in tuple(self.states):
            if state is None:
                continue
            progress = self._generation_progress(state)
            if "process-reaped" not in progress:
                process = getattr(state, "process", None)
                process_started = getattr(
                    state, "process_started", process is not None
                )
                start_attempted = getattr(
                    state, "process_start_attempted", process_started
                )
                if process is None or (not process_started and not start_attempted):
                    progress.add("process-reaped")
                    try:
                        self._teardown_generation_ownership(
                            state, deadline, force=True
                        )
                    except BaseException as cleanup_error:
                        cleanup_errors.append((
                            "generation ownership teardown", cleanup_error
                        ))
                    continue
                alive = attempt("worker liveness", process.is_alive)
                if alive is False and not process_started:
                    progress.add("process-reaped")
                    try:
                        self._teardown_generation_ownership(
                            state, deadline, force=True
                        )
                    except BaseException as cleanup_error:
                        cleanup_errors.append((
                            "generation ownership teardown", cleanup_error
                        ))
                    continue
                if alive is not False:
                    attempt("worker termination", process.terminate)
                attempt(
                    "worker join",
                    lambda: process.join(timeout=max(
                        0.0, deadline - time.monotonic())),
                )
                still_alive = attempt(
                    "worker post-join liveness", process.is_alive
                )
                if still_alive is not False:
                    attempt("worker kill", process.kill)
                    attempt(
                        "worker post-kill join",
                        lambda: process.join(timeout=max(
                            0.0, deadline - time.monotonic())),
                    )
                    still_alive = attempt(
                        "worker post-kill liveness", process.is_alive
                    )
                if still_alive is not False:
                    cleanup_errors.append((
                        "worker reap confirmation",
                        AuditInfrastructureError(
                            "worker liveness remains unconfirmed"
                        ),
                    ))
                    continue
                progress.add("process-reaped")
            try:
                self._teardown_generation_ownership(
                    state, deadline, force=True
                )
            except BaseException as cleanup_error:
                cleanup_errors.append((
                    "generation ownership teardown", cleanup_error
                ))
        all_reaped = all(state is None for state in self.states)
        if all_reaped:
            try:
                self._finalize_shutdown(deadline)
            except BaseException as cleanup_error:
                cleanup_errors.append((
                    "generation shutdown finalization", cleanup_error
                ))
        self._closed = (
            all_reaped
            and "task-phase-snapshot-store" in self._shutdown_progress()
            and not cleanup_errors
        )
        if cleanup_errors:
            error = AuditInfrastructureError(
                "generation cleanup is incomplete"
            )
            for label, additional in cleanup_errors:
                error.add_note(f"{label} failed: {additional}")
            raise error from cleanup_errors[0][1]

    def abort_and_reap_until_closed(
        self, deadline: float
    ) -> tuple[BaseException, ...]:
        if (
            not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or not math.isfinite(deadline)
        ):
            raise AuditInfrastructureError(
                "worker abort retry deadline is invalid"
            )
        cleanup_ceiling = getattr(self, "cleanup_deadline", deadline)
        deadline = min(float(deadline), float(cleanup_ceiling))
        failures: list[BaseException] = []
        while not getattr(self, "_closed", False) and time.monotonic() < deadline:
            try:
                self.abort_and_reap(deadline)
            except BaseException as cleanup_error:
                failures.append(cleanup_error)
                if self._closed:
                    break
                time.sleep(0.001)
        if getattr(self, "_closed", False):
            return tuple(failures)
        error = AuditInfrastructureError(
            "generation cleanup did not reach closed state"
        )
        for failure in failures:
            error.add_note(f"cleanup retry failed: {failure!r}")
            for note in getattr(failure, "__notes__", ()):
                error.add_note(f"cleanup retry detail: {note}")
        if failures:
            raise error from failures[-1]
        raise error


def _enforce_platform_memory_contract(tree_sample, compact_observer) -> None:
    if (
        getattr(tree_sample, "platform_kind", None)
        not in {"windows", "linux", "macos"}
        or not isinstance(getattr(tree_sample, "accounting_complete", None), bool)
        or not tree_sample.accounting_complete
    ):
        raise AuditInfrastructureError("process-tree memory contract is incomplete")
    resident = getattr(tree_sample, "maximum_observed_resident_bytes", None)
    if (
        not isinstance(resident, int)
        or isinstance(resident, bool)
        or resident < 0
    ):
        raise AuditInfrastructureError("process-tree memory contract is invalid")
    compact_peak = getattr(compact_observer, "peak_live_bytes", None)
    if (
        not isinstance(compact_peak, int)
        or isinstance(compact_peak, bool)
        or compact_peak < 0
        or compact_peak > (128 << 20)
    ):
        raise AuditInfrastructureError("compact result memory limit exceeded")
    if (
        tree_sample.platform_kind in {"windows", "linux"}
        and resident >= _PROCESS_TREE_MEMORY_LIMIT_BYTES
    ):
        raise AuditInfrastructureError("process-tree memory contract exceeded")

_WORKER_INDEX: int | None = None
_WORKER_GENERATION: int | None = None
_WORKER_PRODUCTION: Mapping[PurePosixPath, FileIdentity] | None = None
_WORKER_LIMITS: AuditLimits | None = None
_WORKER_CANCEL_EVENT: object | None = None
_WORKER_ENGINE: str | None = None
_WORKER_CACHE: object | None = None
_WORKER_CAPABILITIES: Mapping[str, object] | None = None
_WORKER_MAXIMUM_TASKS: int = 1
_WORKER_RECYCLE_RSS_BYTES: int = 0
_WORKER_PREATTESTED_ENGINE: str | None = None


class _WorkerRssSampler:
    def sample(self) -> int:
        return _current_process_rss_bytes()


_WORKER_RSS: object = _WorkerRssSampler()


class _ScheduledAuditSession:
    """Parent-owned deterministic result and worker/resource lifecycle."""

    def __init__(
        self,
        *,
        aggregate: StreamingAuditSummary,
        cache_hits: int,
        cache_misses: int,
        inspection_probe_invocations: int,
        audit_compiler_invocations: int,
        runtime_contract: WorkerRuntimeContract,
        cache_root: Path,
        result_budget: CompactResultMemoryBudget,
        compact_accounting_observer: CompactAccountingObserver,
        run_accountant,
        capability_registry,
        reactor=None,
        reorder_pending_count: int = 0,
        cache_maximum_encoded_result_bytes: int = 0,
        cache_maximum_conservative_decoded_bytes: int = 0,
        cache_maximum_conservative_retained_bytes: int = 0,
        cleanup_deadline: float | None = None,
    ) -> None:
        if (
            not isinstance(reorder_pending_count, int)
            or isinstance(reorder_pending_count, bool)
            or reorder_pending_count < 0
        ):
            raise AuditInfrastructureError(
                "scheduled reorder window is invalid"
            )
        cache_measurements = (
            cache_maximum_encoded_result_bytes,
            cache_maximum_conservative_decoded_bytes,
            cache_maximum_conservative_retained_bytes,
        )
        if any(
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
            for value in cache_measurements
        ) or (cache_hits > 0 and any(value <= 0 for value in cache_measurements)):
            raise AuditInfrastructureError(
                "scheduled warm-cache measurements are unavailable"
            )
        reactor_measurements = (
            0 if reactor is None else getattr(reactor, "maximum_encoded_result_bytes", None),
            0 if reactor is None else getattr(reactor, "maximum_conservative_decoded_bytes", None),
            0 if reactor is None else getattr(reactor, "maximum_conservative_retained_bytes", None),
        )
        reactor_stdout = (
            0 if reactor is None else getattr(reactor, "stdout_bytes", None)
        )
        if cache_misses > 0 and (
            reactor is None
            or not isinstance(reactor_stdout, int)
            or isinstance(reactor_stdout, bool)
            or reactor_stdout < 0
            or any(
                not isinstance(value, int)
                or isinstance(value, bool)
                or value <= 0
                for value in reactor_measurements
            )
        ):
            raise AuditInfrastructureError(
                "scheduled cold-miss measurements are unavailable"
            )
        self._aggregate: StreamingAuditSummary | None = aggregate
        self._aggregate_consumed = False
        self.cache_hits = cache_hits
        self.cache_misses = cache_misses
        self.inspection_probe_invocations = inspection_probe_invocations
        self.audit_compiler_invocations = audit_compiler_invocations
        self.expected_audit_compiler_invocations = 2 * cache_misses
        self.runtime_contract = runtime_contract
        selected_cleanup_deadline = (
            runtime_contract.pipeline_deadline
            if cleanup_deadline is None
            else cleanup_deadline
        )
        if (
            not isinstance(selected_cleanup_deadline, (int, float))
            or isinstance(selected_cleanup_deadline, bool)
            or not math.isfinite(float(selected_cleanup_deadline))
            or selected_cleanup_deadline < runtime_contract.pipeline_deadline
        ):
            raise AuditInfrastructureError(
                "scheduled cleanup deadline is invalid"
            )
        self.cleanup_deadline = float(selected_cleanup_deadline)
        self.cache_root = cache_root
        self.result_budget = result_budget
        self.compact_accounting_observer = compact_accounting_observer
        self.run_accountant = run_accountant
        self.capability_registry = capability_registry
        self.compact_result_peak_live_bytes = result_budget.peak_live_bytes
        self.reactor = reactor
        self.stdout_bytes = reactor_stdout
        self.maximum_encoded_result_bytes = max(
            cache_maximum_encoded_result_bytes, reactor_measurements[0]
        )
        self.maximum_conservative_decoded_bytes = max(
            cache_maximum_conservative_decoded_bytes,
            reactor_measurements[1],
        )
        self.maximum_conservative_retained_bytes = max(
            cache_maximum_conservative_retained_bytes,
            reactor_measurements[2],
        )
        self._reorder_pending_count = reorder_pending_count
        self.worker_pids = (
            () if reactor is None else tuple(reactor.worker_pids)
        )
        reactor_states = () if reactor is None else getattr(reactor, "states", None)
        if not isinstance(reactor_states, (tuple, list)):
            raise AuditInfrastructureError("scheduled worker states are unavailable")
        started_worker_count = len(reactor_states)
        expected_worker_count = min(runtime_contract.workers, cache_misses)
        if started_worker_count != expected_worker_count:
            raise AuditInfrastructureError(
                "scheduled worker count differs from cold misses"
            )
        self.worker_counts_started = (
            () if started_worker_count == 0 else (started_worker_count,)
        )
        self._shutdown = reactor is None

    def consume_aggregate(self) -> StreamingAuditSummary:
        if self._reorder_pending_count != 0 or (
            self.reactor is not None and self.reactor.has_pending_tasks
        ):
            raise AuditInfrastructureError(
                "scheduled reorder window is not empty"
            )
        if self._aggregate_consumed or self._aggregate is None:
            raise AuditInfrastructureError("scheduled aggregate was already consumed")
        aggregate = self._aggregate
        self._aggregate = None
        self._aggregate_consumed = True
        return aggregate

    @property
    def compact_result_budget(self) -> CompactResultMemoryBudget:
        return self.result_budget

    @property
    def pending_result_reference_count(self) -> int:
        aggregate_count = 0 if self._aggregate is None else 1
        reactor_count = 0
        if self.reactor is not None:
            reactor_count = sum(
                state is not None and state.pending is not None
                for state in self.reactor.states
            )
        return aggregate_count + reactor_count

    def shutdown_reap(self, deadline: float) -> None:
        if (
            not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or not math.isfinite(deadline)
        ):
            raise AuditInfrastructureError("worker shutdown deadline is invalid")
        if self._shutdown:
            return
        self.reactor.shutdown_reap(
            min(float(deadline), self.runtime_contract.pipeline_deadline)
        )
        self._shutdown = True

    def abort_and_reap(self, deadline: float) -> None:
        if self._shutdown:
            return
        if self.reactor is not None:
            self.reactor.abort_and_reap_until_closed(
                min(float(deadline), self.cleanup_deadline)
            )
        self._shutdown = True


def schedule_configuration_audits(
    source_root: Path,
    configurations: tuple[PreprocessConfiguration, ...],
    dependency_roots: DependencyRootAuthority,
    capability_registry,
    initial_digest_map,
    prepared_cache,
    limits: AuditLimits,
    engine: str,
    runtime_contract: WorkerRuntimeContract,
    *,
    inspection_probe_invocations: int,
    run_accountant,
    compact_observer: CompactAccountingObserver,
    cleanup_deadline: float | None = None,
) -> _ScheduledAuditSession:
    """Validate all warm entries before creating any native worker resource."""
    _scheduler_initial_digest_map_event(initial_digest_map)
    if (
        not isinstance(source_root, Path)
        or not isinstance(configurations, tuple)
        or not configurations
        or len(configurations) > 251
        or any(not isinstance(value, PreprocessConfiguration)
               for value in configurations)
        or len({value.digest for value in configurations}) != len(configurations)
        or not isinstance(limits, AuditLimits)
        or not isinstance(runtime_contract, WorkerRuntimeContract)
        or not isinstance(inspection_probe_invocations, int)
        or isinstance(inspection_probe_invocations, bool)
        or inspection_probe_invocations < 0
        or not callable(getattr(prepared_cache, "load_many", None))
        or not isinstance(engine, str)
        or not engine
        or not isinstance(compact_observer, CompactAccountingObserver)
    ):
        raise AuditInfrastructureError("audit scheduler inputs are invalid")
    selected_cleanup_deadline = (
        runtime_contract.pipeline_deadline
        if cleanup_deadline is None
        else cleanup_deadline
    )
    if (
        not isinstance(selected_cleanup_deadline, (int, float))
        or isinstance(selected_cleanup_deadline, bool)
        or not math.isfinite(float(selected_cleanup_deadline))
        or selected_cleanup_deadline < runtime_contract.pipeline_deadline
    ):
        raise AuditInfrastructureError("audit scheduler cleanup deadline is invalid")
    selected_cleanup_deadline = float(selected_cleanup_deadline)
    if time.monotonic() >= runtime_contract.pipeline_deadline:
        raise AuditInfrastructureError("pipeline deadline exceeded before scheduling")
    authority = validate_dependency_root_authority(dependency_roots)
    if any(
        value.dependency_root_authority_digest
        != authority.portable_authority_digest
        for value in configurations
    ):
        raise AuditInfrastructureError("scheduler dependency-root authority differs")
    from gpu_capability_source_audit import _attest_loaded_audit_engine

    loaded_engine = _attest_loaded_audit_engine(engine)
    result_budget = CompactResultMemoryBudget(
        maximum_bytes=128 << 20, observer=compact_observer
    )
    aggregator = StreamingResultAggregator(
        configurations,
        result_budget,
        limits,
        require_main_provenance=True,
    )
    slot_template = maximum_compact_result_slot(
        limits,
        conservative_allocation_schema(),
        _PIPE_KERNEL_CAPACITY_BYTES,
    )
    if slot_template.worst_case_live_bytes > (128 << 20):
        raise AuditInfrastructureError(
            "single compact-result slot exceeds 128 MiB"
        )
    maximum_cold_slot = CompactResultColdSlot(
        slot_template.worst_case_live_bytes
    )
    try:
        batch = prepared_cache.load_many(
            configurations,
            authority,
            loaded_engine,
            initial_digest_map,
            result_budget,
            aggregator,
            maximum_cold_slot,
            runtime_contract.pipeline_deadline,
        )
        if not hasattr(batch, "misses") or not hasattr(batch, "hit_count"):
            raise AuditInfrastructureError("audit cache batch is invalid")
        cache_measurements = (
            getattr(batch, "maximum_encoded_result_bytes", None),
            getattr(batch, "maximum_conservative_decoded_bytes", None),
            getattr(batch, "maximum_conservative_retained_bytes", None),
        )
        if any(
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
            for value in cache_measurements
        ) or (batch.hit_count > 0 and any(value <= 0 for value in cache_measurements)):
            raise AuditInfrastructureError(
                "audit cache measurements are unavailable"
            )
        cache_root = getattr(prepared_cache, "root", source_root)
        if not isinstance(cache_root, Path):
            cache_root = source_root
        if not batch.misses:
            if aggregator.accepted_count != len(configurations):
                raise AuditInfrastructureError(
                    "scheduled reorder window is not empty"
                )
            aggregate = aggregator.finish()
            return _ScheduledAuditSession(
                aggregate=aggregate,
                cache_hits=batch.hit_count,
                cache_misses=0,
                inspection_probe_invocations=inspection_probe_invocations,
                audit_compiler_invocations=0,
                runtime_contract=runtime_contract,
                cache_root=cache_root,
                result_budget=result_budget,
                compact_accounting_observer=compact_observer,
                run_accountant=run_accountant,
                capability_registry=capability_registry,
                reorder_pending_count=0,
                cache_maximum_encoded_result_bytes=cache_measurements[0],
                cache_maximum_conservative_decoded_bytes=cache_measurements[1],
                cache_maximum_conservative_retained_bytes=cache_measurements[2],
                cleanup_deadline=selected_cleanup_deadline,
            )

        miss_digests = {configuration.digest for configuration in batch.misses}
        if len(miss_digests) != len(batch.misses) or not miss_digests.issubset(
            {configuration.digest for configuration in configurations}
        ):
            raise AuditInfrastructureError("audit cache batch misses are invalid")
        worker_count = min(runtime_contract.workers, len(batch.misses))
        reactor = GenerationReactor(
            configurations=configurations,
            dependency_roots=authority,
            production_snapshot=initial_digest_map,
            cache_root=cache_root,
            limits=limits,
            engine=loaded_engine,
            runtime_contract=runtime_contract,
            run_accountant=run_accountant,
            capability_registry=capability_registry,
            result_budget=result_budget,
            worker_count=worker_count,
            cleanup_deadline=selected_cleanup_deadline,
        )
        ordinals = {
            configuration.digest: ordinal
            for ordinal, configuration in enumerate(configurations)
        }
        remaining = collections.deque(sorted(
            (
                (ordinals[configuration.digest], configuration)
                for configuration in batch.misses
            ),
            key=lambda item: item[0],
        ))
        try:
            while (
                remaining
                or reactor.has_pending_tasks
                or reactor.has_expected_retirement
            ):
                next_unaggregated = aggregator.next_unaggregated_ordinal
                upper = next_unaggregated + worker_count - 1
                idle = collections.deque(reactor.idle_states())
                while remaining and idle and remaining[0][0] <= upper:
                    ordinal, configuration = remaining[0]
                    state = idle[0]
                    if not reactor.send_task(
                        state, ordinal, configuration, slot_template
                    ):
                        if not reactor.has_pending_tasks:
                            raise AuditInfrastructureError(
                                "remaining compact task cannot be admitted"
                            )
                        break
                    remaining.popleft()
                    idle.popleft()
                if (
                    remaining
                    and not reactor.has_pending_tasks
                    and not reactor.has_expected_retirement
                    and (not reactor.idle_states()
                         or remaining[0][0] > upper)
                ):
                    raise AuditInfrastructureError(
                        "remaining compact task cannot be admitted"
                    )
                if not (
                    remaining
                    or reactor.has_pending_tasks
                    or reactor.has_expected_retirement
                ):
                    break
                state, event = reactor.next_event()
                transition = reactor.handle_task_event(
                    state, event, aggregator
                )
                if transition is not None:
                    _kind, worker_index = transition
                    if remaining or reactor.has_pending_tasks:
                        reactor.spawn_next_generation(worker_index)
                        reactor._wait_all_ready()
                if (
                    (remaining or reactor.has_pending_tasks)
                    and reactor.live_generation_count == 0
                ):
                    reactor.spawn_next_generation(0)
                    reactor._wait_all_ready()
            if reactor.audit_launch_count != 2 * len(batch.misses):
                raise AuditInfrastructureError(
                    "audit compiler invocation accounting differs"
                )
            if aggregator.accepted_count != len(configurations):
                raise AuditInfrastructureError(
                    "scheduled reorder window is not empty"
                )
            aggregate = aggregator.finish()
        except BaseException as error:
            try:
                reactor.abort_and_reap_until_closed(
                    emergency_cleanup_deadline(selected_cleanup_deadline)
                )
            except BaseException as cleanup_error:
                error.add_note(
                    f"worker emergency cleanup also failed: {cleanup_error}"
                )
            raise
        return _ScheduledAuditSession(
            aggregate=aggregate,
            cache_hits=batch.hit_count,
            cache_misses=len(batch.misses),
            inspection_probe_invocations=inspection_probe_invocations,
            audit_compiler_invocations=reactor.audit_launch_count,
            runtime_contract=runtime_contract,
            cache_root=cache_root,
            result_budget=result_budget,
            compact_accounting_observer=compact_observer,
            run_accountant=run_accountant,
            capability_registry=capability_registry,
            reactor=reactor,
            reorder_pending_count=0,
            cache_maximum_encoded_result_bytes=cache_measurements[0],
            cache_maximum_conservative_decoded_bytes=cache_measurements[1],
            cache_maximum_conservative_retained_bytes=cache_measurements[2],
            cleanup_deadline=selected_cleanup_deadline,
        )
    except BaseException:
        raise


def _check_dependency_budget(
    deadline: float | None, cancel_event: object | None
) -> None:
    if cancel_event is not None:
        is_set = getattr(cancel_event, "is_set", None)
        if not callable(is_set):
            raise AuditInfrastructureError("dependency cancellation event is invalid")
        if is_set():
            raise AuditInfrastructureError("dependency hashing cancelled")
    if deadline is not None and time.monotonic() >= deadline:
        raise AuditInfrastructureError("dependency hashing deadline exceeded")


@dataclass(frozen=True)
class ExecutionResult:
    stderr_tail: bytes
    elapsed_seconds: float
    observed_stdout_bytes: int


def _validate_execution_inputs(
    command: RewrittenCommand,
    configuration: PreprocessConfiguration,
    limits: AuditLimits,
    deadline: float,
    consume_stdout: Callable[[bytes], None],
) -> None:
    if not isinstance(command, RewrittenCommand) or not command.arguments:
        raise AuditInfrastructureError("rewritten preprocess command is invalid")
    if not isinstance(configuration, PreprocessConfiguration):
        raise AuditInfrastructureError("preprocess configuration is invalid")
    if not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("audit limits are invalid")
    if not isinstance(deadline, (int, float)) or isinstance(deadline, bool):
        raise AuditInfrastructureError("global deadline is invalid")
    if not callable(consume_stdout):
        raise AuditInfrastructureError("stdout consumer is invalid")
    numeric_limits = {
        "invocation timeout": limits.invocation_seconds,
        "stdout limit": limits.stdout_bytes,
        "stderr limit": limits.stderr_bytes,
        "RSS limit": limits.rss_bytes,
    }
    for label, value in numeric_limits.items():
        if (
            not isinstance(value, (int, float))
            or isinstance(value, bool)
            or value < 0
        ):
            raise AuditInfrastructureError(f"{label} is invalid")
    if limits.invocation_seconds <= 0:
        raise AuditInfrastructureError("invocation timeout is invalid")


def _stderr_tail_text(stderr_tail: bytes) -> str:
    return repr(stderr_tail)


def _diagnostic(
    configuration: PreprocessConfiguration,
    reason: str,
    *,
    exit_status: int | str,
    elapsed_seconds: float,
    observed_stdout_bytes: int,
    stderr_tail: bytes,
) -> AuditInfrastructureError:
    return AuditInfrastructureError(
        f"GPU capability preprocessing failed: digest={configuration.digest} "
        f"family={configuration.family.value} source={configuration.source.canonical} "
        f"exit={exit_status} elapsed={elapsed_seconds:.3f}s "
        f"stdout_bytes={observed_stdout_bytes} "
        f"stderr_tail={_stderr_tail_text(stderr_tail)} reason={reason}"
    )


class _ProcessContainment:
    """Own one compiler process tree until completion or forced teardown."""

    def __init__(self) -> None:
        self._pid: int | None = None
        self._job = _WindowsJob() if os.name == "nt" else None

    @property
    def popen_arguments(self) -> dict[str, object]:
        if os.name == "nt":
            return {
                "creationflags": (
                    subprocess.CREATE_NEW_PROCESS_GROUP
                    | getattr(subprocess, "CREATE_SUSPENDED", 0x00000004)
                )
            }
        return {"start_new_session": True}

    @property
    def requires_handshake(self) -> bool:
        return os.name == "nt"

    def prepare_command(self, command: tuple[str, ...]) -> list[str]:
        return list(command)

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        self._pid = process.pid
        if self._job is not None:
            self._job.attach(process)

    def release(self, process: subprocess.Popen[bytes]) -> None:
        if not self.requires_handshake:
            return
        _resume_suspended_windows_process(process)
        if process.stdin is not None:
            process.stdin.close()

    def terminate(self) -> None:
        if self._job is not None:
            self._job.terminate()
            return
        pid = self._pid
        self._pid = None
        if pid is not None:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def close(self) -> None:
        if self._job is not None:
            self._job.close()
        else:
            self._pid = None


class _WindowsJob:
    """Kill-on-close Windows Job Object with race-free child release."""

    def __init__(self) -> None:
        if os.name != "nt":
            self._handle = None
            return
        import ctypes
        from ctypes import wintypes

        class _IoCounters(ctypes.Structure):
            _fields_ = tuple(
                (name, ctypes.c_ulonglong)
                for name in (
                    "read_operations",
                    "write_operations",
                    "other_operations",
                    "read_bytes",
                    "write_bytes",
                    "other_bytes",
                )
            )

        class _BasicLimits(ctypes.Structure):
            _fields_ = (
                ("per_process_user_time", ctypes.c_longlong),
                ("per_job_user_time", ctypes.c_longlong),
                ("limit_flags", wintypes.DWORD),
                ("minimum_working_set", ctypes.c_size_t),
                ("maximum_working_set", ctypes.c_size_t),
                ("active_process_limit", wintypes.DWORD),
                ("affinity", ctypes.c_size_t),
                ("priority_class", wintypes.DWORD),
                ("scheduling_class", wintypes.DWORD),
            )

        class _ExtendedLimits(ctypes.Structure):
            _fields_ = (
                ("basic", _BasicLimits),
                ("io", _IoCounters),
                ("process_memory", ctypes.c_size_t),
                ("job_memory", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t),
                ("peak_job_memory", ctypes.c_size_t),
            )

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
        kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
        kernel32.TerminateJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.CreateJobObjectW(None, None)
        if not handle:
            raise AuditInfrastructureError("cannot create compiler process job object")
        limits = _ExtendedLimits()
        limits.basic.limit_flags = 0x00002000
        if not kernel32.SetInformationJobObject(
            handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            kernel32.CloseHandle(handle)
            raise AuditInfrastructureError("cannot configure compiler process job object")
        self._handle = handle
        self._kernel32 = kernel32

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        if self._handle is None:
            return
        raw_handle = getattr(process, "_handle", None)
        if raw_handle is None or not self._kernel32.AssignProcessToJobObject(
            self._handle, raw_handle
        ):
            raise AuditInfrastructureError("cannot contain compiler process tree")

    def terminate(self) -> None:
        if self._handle is not None:
            handle = self._handle
            if self._kernel32.TerminateJobObject(handle, 1):
                return
            closed = bool(self._kernel32.CloseHandle(handle))
            if closed:
                self._handle = None
            fallback = "succeeded" if closed else "failed"
            raise AuditInfrastructureError(
                "cannot terminate compiler process job object; "
                f"kill-on-close fallback {fallback}"
            )

    def close(self) -> None:
        if self._handle is not None:
            handle = self._handle
            if self._kernel32.CloseHandle(handle):
                self._handle = None
                return
            terminated = bool(self._kernel32.TerminateJobObject(handle, 1))
            closed = bool(self._kernel32.CloseHandle(handle))
            if closed:
                self._handle = None
            raise AuditInfrastructureError(
                "cannot close compiler process job object; "
                f"termination fallback {'succeeded' if terminated else 'failed'}, "
                f"close retry {'succeeded' if closed else 'failed'}"
            )


def _terminate_and_reap(
    process: subprocess.Popen[bytes], containment: _ProcessContainment
) -> None:
    containment_error: AuditInfrastructureError | None = None
    try:
        containment.terminate()
    except AuditInfrastructureError as error:
        containment_error = error
    try:
        process.wait(timeout=_REAP_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=_REAP_SECONDS)
    if containment_error is not None:
        raise containment_error


def _launch_compiler_process(
    prepared_arguments,
    *,
    configuration: PreprocessConfiguration,
    environment: Mapping[str, str],
    containment: _ProcessContainment,
    capability,
    launch_options: Mapping[str, object],
    launch_context: _AuditLaunchContext | None,
    launch_purpose: CompilerLaunchPurpose | None,
) -> tuple[subprocess.Popen[bytes], CompilerProcessHandleCarrier]:
    purpose = (
        launch_purpose
        if launch_purpose is not None
        else CompilerLaunchPurpose.INSPECTION
    )
    return launch_compiler_process(
        prepared_arguments,
        cwd=configuration.working_directory,
        environment=environment,
        containment=containment,
        platform_kind=capability.platform_kind,
        stdin=(
            subprocess.PIPE if containment.requires_handshake else subprocess.DEVNULL
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        launch_options=launch_options,
        purpose=purpose,
        launch_observer=launch_context,
        worker_index=(_WORKER_INDEX if launch_context is not None else None),
        task_id=(launch_context.task_id if launch_context is not None else None),
        generation=(
            launch_context.generation if launch_context is not None else None
        ),
    )


def run_bounded_preprocessor(
    command: RewrittenCommand,
    configuration: PreprocessConfiguration,
    limits: AuditLimits,
    deadline: float,
    consume_stdout: Callable[[bytes], None],
    cancel_event: object | None = None,
    *,
    launch_context: _AuditLaunchContext | None = None,
    launch_purpose: CompilerLaunchPurpose | None = None,
) -> ExecutionResult:
    """Execute one rewritten command without retaining its stdout stream."""

    _validate_execution_inputs(command, configuration, limits, deadline, consume_stdout)
    if (launch_context is None) != (launch_purpose is None):
        raise AuditInfrastructureError("compiler launch observation binding is invalid")
    started = time.monotonic()
    if cancel_event is not None and not callable(getattr(cancel_event, "is_set", None)):
        raise AuditInfrastructureError("preprocess cancellation event is invalid")
    if cancel_event is not None and cancel_event.is_set():
        raise _diagnostic(
            configuration,
            "coordinator cancelled before launch",
            exit_status="not-started",
            elapsed_seconds=0.0,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    if deadline <= started:
        raise _diagnostic(
            configuration,
            "global deadline exceeded before launch",
            exit_status="not-started",
            elapsed_seconds=0.0,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    try:
        environment = dict(os.environ)
        actual_environment_digest = _environment_digest(environment)
    except (AuditInfrastructureError, OSError, RuntimeError, ValueError) as error:
        raise _diagnostic(
            configuration,
            f"cannot snapshot compiler environment: {error}",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        ) from error
    if actual_environment_digest != configuration.environment_digest:
        raise _diagnostic(
            configuration,
            "compiler environment digest mismatch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    if cancel_event is not None and cancel_event.is_set():
        raise _diagnostic(
            configuration,
            "coordinator cancelled before launch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    invocation_deadline = started + limits.invocation_seconds
    effective_deadline = min(invocation_deadline, deadline)
    deadline_reason = (
        "global deadline exceeded" if deadline <= invocation_deadline else "timeout"
    )
    if _current_process_rss_bytes() > limits.rss_bytes:
        raise _diagnostic(
            configuration,
            "coordinator RSS limit exceeded before launch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    capability = configuration.compiler_capability
    if capability.capability_digest != configuration.compiler_capability_digest:
        raise _diagnostic(
            configuration,
            "compiler executable capability digest mismatch",
            exit_status="not-started",
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=0,
            stderr_tail=b"",
        )
    capability_owner = capability.native_owner
    validate_owner = getattr(capability_owner, "validate", None)
    if not callable(validate_owner):
        raise AuditInfrastructureError("compiler executable capability owner is invalid")
    validate_owner(
        content=False, deadline=effective_deadline,
        cancel_event=cancel_event,
    )
    if not command.arguments:
        raise AuditInfrastructureError("rewritten preprocess command is empty")
    requested_executable = Path(command.arguments[0])
    if (
        not requested_executable.is_absolute()
        or os.path.normcase(os.path.normpath(str(requested_executable)))
        != os.path.normcase(os.path.normpath(
            str(capability.executable_identity.canonical)
        ))
    ):
        raise AuditInfrastructureError("compiler launch does not consume the held capability")
    containment = _ProcessContainment()
    process: subprocess.Popen[bytes] | None = None
    process_carrier: CompilerProcessHandleCarrier | None = None
    stdout_events: queue.Queue[bytes | BaseException | None] = queue.Queue(maxsize=2)
    stop_readers = threading.Event()
    stderr_tail = bytearray()
    stderr_error: list[BaseException] = []
    stderr_lock = threading.Lock()

    def read_stdout() -> None:
        if process is None or process.stdout is None:
            raise AuditInfrastructureError("preprocessor stdout reader is unavailable")
        try:
            while not stop_readers.is_set():
                chunk = process.stdout.read(_IO_CHUNK_BYTES)
                if not chunk:
                    break
                while not stop_readers.is_set():
                    try:
                        stdout_events.put(chunk, timeout=_POLL_SECONDS)
                        break
                    except queue.Full:
                        continue
        except BaseException as error:
            while not stop_readers.is_set():
                try:
                    stdout_events.put(error, timeout=_POLL_SECONDS)
                    break
                except queue.Full:
                    continue
        finally:
            while not stop_readers.is_set():
                try:
                    stdout_events.put(None, timeout=_POLL_SECONDS)
                    break
                except queue.Full:
                    continue

    def read_stderr() -> None:
        if process is None or process.stderr is None:
            raise AuditInfrastructureError("preprocessor stderr reader is unavailable")
        try:
            while True:
                chunk = process.stderr.read(_IO_CHUNK_BYTES)
                if not chunk:
                    break
                with stderr_lock:
                    if limits.stderr_bytes:
                        stderr_tail.extend(chunk)
                        excess = len(stderr_tail) - limits.stderr_bytes
                        if excess > 0:
                            del stderr_tail[:excess]
        except BaseException as error:
            stderr_error.append(error)

    stdout_thread: threading.Thread | None = None
    stderr_thread: threading.Thread | None = None
    observed_stdout_bytes = 0
    failure: tuple[str, int | str, float] | None = None
    try:
        try:
            launch_arguments, launch_options = _held_compiler_launch(
                capability, command.arguments
            )
            prepared_arguments = containment.prepare_command(launch_arguments)
            now = time.monotonic()
            if cancel_event is not None and cancel_event.is_set():
                raise AuditInfrastructureError(
                    "coordinator cancelled before launch"
                )
            if now >= effective_deadline:
                raise AuditInfrastructureError(
                    f"{deadline_reason} before launch"
                )
            validate_owner(
                content=False, deadline=effective_deadline,
                cancel_event=cancel_event,
            )
            now = time.monotonic()
            if cancel_event is not None and cancel_event.is_set():
                raise AuditInfrastructureError(
                    "coordinator cancelled before launch"
                )
            if now >= effective_deadline:
                raise AuditInfrastructureError(
                    f"{deadline_reason} before launch"
                )
            process, process_carrier = _launch_compiler_process(
                prepared_arguments,
                configuration=configuration,
                environment=environment,
                containment=containment,
                capability=capability,
                launch_options=launch_options,
                launch_context=launch_context,
                launch_purpose=launch_purpose,
            )
        except (OSError, AuditInfrastructureError) as error:
            if process is not None:
                _terminate_and_reap(process, containment)
            raise _diagnostic(
                configuration,
                str(error),
                exit_status="launch-failed",
                elapsed_seconds=time.monotonic() - started,
                observed_stdout_bytes=0,
                stderr_tail=b"",
            ) from error

        stdout_thread = threading.Thread(target=read_stdout, name="gpu-audit-stdout", daemon=True)
        stderr_thread = threading.Thread(target=read_stderr, name="gpu-audit-stderr", daemon=True)
        stdout_thread.start()
        stderr_thread.start()
        stdout_complete = False
        tree_terminated = False
        while not stdout_complete:
            now = time.monotonic()
            if cancel_event is not None and cancel_event.is_set():
                failure = (
                    "coordinator cancelled after another configuration failed",
                    "terminated",
                    now - started,
                )
                break
            if now >= effective_deadline:
                failure = (
                    deadline_reason,
                    "terminated",
                    now - started,
                )
                break
            rss = _current_process_rss_bytes()
            if rss > limits.rss_bytes:
                failure = (
                    "coordinator RSS limit exceeded",
                    "terminated",
                    now - started,
                )
                break
            if process.poll() is not None and not tree_terminated:
                try:
                    containment.terminate()
                except AuditInfrastructureError as error:
                    failure = (
                        str(error),
                        process.returncode,
                        time.monotonic() - started,
                    )
                    tree_terminated = True
                    break
                tree_terminated = True
            try:
                event = stdout_events.get(timeout=min(_POLL_SECONDS, effective_deadline - now))
            except queue.Empty:
                continue
            if event is None:
                stdout_complete = True
                continue
            if isinstance(event, BaseException):
                failure = (
                    f"cannot read compiler stdout: {event}",
                    "terminated",
                    time.monotonic() - started,
                )
                break
            observed_stdout_bytes += len(event)
            if observed_stdout_bytes > limits.stdout_bytes:
                failure = (
                    "stdout output limit exceeded",
                    "terminated",
                    time.monotonic() - started,
                )
                break
            try:
                consume_stdout(event)
            except Exception as error:
                failure = (
                    str(error),
                    "terminated",
                    time.monotonic() - started,
                )
                break

        while failure is None and process.poll() is None:
            now = time.monotonic()
            if cancel_event is not None and cancel_event.is_set():
                failure = (
                    "coordinator cancelled after another configuration failed",
                    "terminated",
                    now - started,
                )
                break
            if now >= effective_deadline:
                failure = (
                    deadline_reason,
                    "terminated",
                    now - started,
                )
                break
            if _current_process_rss_bytes() > limits.rss_bytes:
                failure = (
                    "coordinator RSS limit exceeded",
                    "terminated",
                    now - started,
                )
                break
            time.sleep(min(_POLL_SECONDS, effective_deadline - now))

        if failure is not None:
            reason, exit_status, failure_elapsed = failure
            actual_returncode = process.poll()
            if actual_returncode is not None:
                exit_status = actual_returncode
            try:
                if not tree_terminated:
                    _terminate_and_reap(process, containment)
                    tree_terminated = True
            except AuditInfrastructureError as cleanup_error:
                reason = f"{reason}; containment cleanup failed: {cleanup_error}"
            failure = (reason, exit_status, failure_elapsed)
        elif not tree_terminated:
            try:
                # A compiler must not leave a descendant holding either output pipe.
                containment.terminate()
            except AuditInfrastructureError as error:
                failure = (
                    str(error),
                    process.returncode,
                    time.monotonic() - started,
                )
            tree_terminated = True
        stop_readers.set()
        for thread in (stdout_thread, stderr_thread):
            if thread is not None:
                thread.join(timeout=_REAP_SECONDS)
        if (
            failure is None
            and any(
                thread is not None and thread.is_alive()
                for thread in (stdout_thread, stderr_thread)
            )
        ):
            failure = (
                "compiler output reader did not terminate",
                "terminated",
                time.monotonic() - started,
            )
        if stderr_error and failure is None:
            failure = (
                f"cannot read compiler stderr: {stderr_error[0]}",
                process.returncode if process.returncode is not None else "terminated",
                time.monotonic() - started,
            )
        if failure is not None:
            reason, exit_status, elapsed_seconds = failure
            with stderr_lock:
                complete_stderr_tail = bytes(stderr_tail)
            raise _diagnostic(
                configuration,
                reason,
                exit_status=exit_status,
                elapsed_seconds=elapsed_seconds,
                observed_stdout_bytes=observed_stdout_bytes,
                stderr_tail=complete_stderr_tail,
            )
        if process.returncode != 0:
            with stderr_lock:
                complete_stderr_tail = bytes(stderr_tail)
            raise _diagnostic(
                configuration,
                "compiler returned a nonzero status",
                exit_status=process.returncode,
                elapsed_seconds=time.monotonic() - started,
                observed_stdout_bytes=observed_stdout_bytes,
                stderr_tail=complete_stderr_tail,
            )
        validate_owner(
            content=False, deadline=effective_deadline,
            cancel_event=cancel_event,
        )
        with stderr_lock:
            complete_stderr_tail = bytes(stderr_tail)
        return ExecutionResult(
            stderr_tail=complete_stderr_tail,
            elapsed_seconds=time.monotonic() - started,
            observed_stdout_bytes=observed_stdout_bytes,
        )
    finally:
        active_error = sys.exc_info()[1]
        cleanup_errors = []

        def record_cleanup_error(label: str, error: BaseException) -> None:
            cleanup_errors.append((label, error))

        stop_readers.set()
        if process is not None and process.poll() is None:
            try:
                _terminate_and_reap(process, containment)
            except BaseException as error:
                record_cleanup_error("containment cleanup", error)
        if stdout_thread is not None:
            try:
                stdout_thread.join(timeout=_REAP_SECONDS)
            except BaseException as error:
                record_cleanup_error("stdout reader join", error)
        if stderr_thread is not None:
            try:
                stderr_thread.join(timeout=_REAP_SECONDS)
            except BaseException as error:
                record_cleanup_error("stderr reader join", error)
        if process is not None:
            if process.stdout is not None:
                try:
                    process.stdout.close()
                except BaseException as error:
                    record_cleanup_error("compiler stdout close", error)
            if process.stderr is not None:
                try:
                    process.stderr.close()
                except BaseException as error:
                    record_cleanup_error("compiler stderr close", error)
        try:
            containment.close()
        except BaseException as error:
            record_cleanup_error("containment close", error)
        if process is not None and process_carrier is not None:
            try:
                process_carrier.complete_after_exit()
            except BaseException as error:
                record_cleanup_error("process carrier completion", error)
        if active_error is not None:
            for label, error in cleanup_errors:
                active_error.add_note(f"{label} also failed: {error}")
        elif cleanup_errors:
            label, error = cleanup_errors[0]
            with stderr_lock:
                cleanup_stderr_tail = bytes(stderr_tail)
            cleanup_failure = _diagnostic(
                configuration,
                f"{label} failed: {error}",
                exit_status=(
                    process.returncode
                    if process is not None and process.returncode is not None
                    else "terminated"
                ),
                elapsed_seconds=time.monotonic() - started,
                observed_stdout_bytes=observed_stdout_bytes,
                stderr_tail=cleanup_stderr_tail,
            )
            for extra_label, extra_error in cleanup_errors[1:]:
                cleanup_failure.add_note(
                    f"{extra_label} also failed: {extra_error}"
                )
            raise cleanup_failure from error


def _source_root(
    production: Mapping[PurePosixPath, FileIdentity],
) -> Path:
    roots: set[Path] = set()
    for relative, identity in production.items():
        if relative != identity.relative or not identity.production:
            raise AuditInfrastructureError("production identity table is inconsistent")
        root = identity.canonical
        for _part in relative.parts:
            root = root.parent
        roots.add(root)
    if len(roots) != 1:
        raise AuditInfrastructureError("production identities do not share one source root")
    return next(iter(roots))


def _absolute_dependencies(
    paths: tuple[Path, ...], working_directory: Path
) -> tuple[Path, ...]:
    return tuple(
        path if path.is_absolute() else (working_directory / path).resolve(strict=False)
        for path in paths
    )


@dataclass(frozen=True, slots=True)
class StreamDigest:
    sha256: str
    byte_count: int


@dataclass(frozen=True, slots=True)
class PreprocessDiscovery:
    stream: StreamDigest
    dependencies: tuple[DependencyDigest, ...]
    dependency_identities: tuple[FileIdentity, ...]


@dataclass(frozen=True, slots=True)
class PreprocessStageTimings:
    discovery_seconds: float
    accepted_parse_seconds: float
    compiler_stdout_bytes: int = 0

    def __post_init__(self) -> None:
        if (
            any(
                not isinstance(value, (int, float))
                or isinstance(value, bool)
                or value < 0
                for value in (
                    self.discovery_seconds,
                    self.accepted_parse_seconds,
                )
            )
            or not isinstance(self.compiler_stdout_bytes, int)
            or isinstance(self.compiler_stdout_bytes, bool)
            or self.compiler_stdout_bytes < 0
        ):
            raise AuditInfrastructureError("preprocess stage telemetry is invalid")


class _StageTimer:
    __slots__ = ("_started", "elapsed_seconds")

    def __init__(self) -> None:
        self._started = 0.0
        self.elapsed_seconds = 0.0

    def __enter__(self) -> "_StageTimer":
        self._started = time.monotonic()
        return self

    def __exit__(self, _kind, _value, _traceback) -> None:
        self.elapsed_seconds = time.monotonic() - self._started


def _stage_timer() -> _StageTimer:
    return _StageTimer()


@dataclass(frozen=True, slots=True)
class _CompactAuditDraft:
    configuration_digest: str
    audit_engine_fingerprint: str
    dependencies: tuple[DependencyDigest, ...]
    reached_production: tuple[PurePosixPath, ...]
    findings: tuple[AuditResultFinding, ...]
    task_id: str
    generation: int

    def materialize_under(
        self,
        reservation: PerTaskCompactReservation,
        publication_permit: CachePublicationPermit,
    ) -> ConfigurationAuditResult:
        reservation.validate(self.task_id, self.generation)
        publication_permit.validate_for_task(
            self.task_id,
            self.generation,
            self.configuration_digest,
            self.audit_engine_fingerprint,
            self.dependencies,
        )
        ownership = reservation.begin_result_ownership(
            self.task_id, self.generation
        )
        try:
            return ConfigurationAuditResult(
                self.configuration_digest,
                self.audit_engine_fingerprint,
                self.dependencies,
                self.reached_production,
                self.findings,
                ownership,
            )
        except BaseException:
            ownership.release("worker-materialization-failure")
            raise


def _compact_findings(findings) -> tuple[AuditResultFinding, ...]:
    if not isinstance(findings, (tuple, list)):
        raise AuditInfrastructureError("audit findings collection is invalid")
    compact: list[AuditResultFinding] = []
    for finding in findings:
        path = getattr(finding, "path", None)
        line = getattr(finding, "line", None)
        expression = getattr(finding, "expression", None)
        reason = getattr(finding, "reason", None)
        if (
            not isinstance(path, PurePosixPath)
            or not isinstance(line, int)
            or isinstance(line, bool)
            or not isinstance(expression, str)
            or not isinstance(reason, str)
        ):
            raise AuditInfrastructureError("audit finding cannot be compacted")
        compact.append(AuditResultFinding(path, line, expression, reason))
    result = tuple(sorted(
        compact,
        key=lambda item: (
            item.path.as_posix(), item.line, item.expression, item.reason
        ),
    ))
    if len(set(
        (item.path.as_posix(), item.line, item.expression, item.reason)
        for item in result
    )) != len(result):
        raise AuditInfrastructureError("compact audit findings are duplicated")
    return result


def _json_ascii_string_bytes(value: str) -> int:
    if not isinstance(value, str):
        raise AuditInfrastructureError("canonical JSON string is invalid")
    total = 2
    for character in value:
        codepoint = ord(character)
        if character in {'"', "\\"} or character in "\b\f\n\r\t":
            total += 2
        elif codepoint < 0x20:
            total += 6
        elif codepoint <= 0x7f:
            total += 1
        elif codepoint <= 0xffff:
            total += 6
        else:
            total += 12
    return total


def _json_member_bytes(name: str, value_bytes: int) -> int:
    return _json_ascii_string_bytes(name) + 1 + value_bytes


def _canonical_audit_result_json_bytes(
    configuration_digest: str,
    audit_engine_fingerprint: str,
    dependencies: tuple[DependencyDigest, ...],
    raw_findings,
) -> int:
    dependency_array_bytes = 2
    for index, dependency in enumerate(dependencies):
        identity = dependency.identity
        values = (
            ("canonical", _json_ascii_string_bytes(str(identity.canonical))),
            ("device", len(str(identity.device))),
            ("inode", 4 if identity.inode is None else len(str(identity.inode))),
            ("line_count", len(str(identity.line_count))),
            ("production", 4 if identity.production else 5),
            (
                "relative",
                4 if identity.relative is None else _json_ascii_string_bytes(
                    identity.relative.as_posix()
                ),
            ),
            (
                "role_relative_path",
                _json_ascii_string_bytes(dependency.role_relative_path.as_posix()),
            ),
            ("sha256", _json_ascii_string_bytes(dependency.sha256)),
            ("stable_role", _json_ascii_string_bytes(dependency.stable_role)),
        )
        dependency_array_bytes += (1 if index else 0) + 2 + 8 + sum(
            _json_member_bytes(name, size) for name, size in values
        )

    reached_array_bytes = 2
    reached_index = 0
    for dependency in dependencies:
        identity = dependency.identity
        if identity.production and identity.relative is not None:
            reached_array_bytes += (
                (1 if reached_index else 0)
                + _json_ascii_string_bytes(identity.relative.as_posix())
            )
            reached_index += 1

    finding_array_bytes = 2
    for index, finding in enumerate(raw_findings):
        path = finding.path.as_posix()
        line = finding.line
        expression = finding.expression
        reason = finding.reason
        finding_array_bytes += (1 if index else 0) + 2 + 3 + sum((
            _json_member_bytes("expression", _json_ascii_string_bytes(expression)),
            _json_member_bytes("line", len(str(line))),
            _json_member_bytes("path", _json_ascii_string_bytes(path)),
            _json_member_bytes("reason", _json_ascii_string_bytes(reason)),
        ))

    top_values = (
        ("audit_engine_fingerprint", _json_ascii_string_bytes(audit_engine_fingerprint)),
        ("configuration_digest", _json_ascii_string_bytes(configuration_digest)),
        ("dependencies", dependency_array_bytes),
        ("findings", finding_array_bytes),
        ("reached_production", reached_array_bytes),
        ("schema", _json_ascii_string_bytes(AUDIT_RESULT_SCHEMA_BYTES.decode("ascii"))),
    )
    return 2 + 5 + sum(
        _json_member_bytes(name, size) for name, size in top_values
    )


def _bounded_compact_result_draft(
    configuration_digest: str,
    audit_engine_fingerprint: str,
    dependencies: tuple[DependencyDigest, ...],
    raw_findings,
    task_id: str,
    generation: int,
    reservation: PerTaskCompactReservation,
) -> _CompactAuditDraft:
    if (
        not isinstance(dependencies, tuple)
        or any(not isinstance(item, DependencyDigest) for item in dependencies)
        or not isinstance(raw_findings, (tuple, list))
    ):
        raise AuditInfrastructureError("compact result draft is invalid")
    reached_count = 0
    reached_path_bytes = 0
    for dependency in dependencies:
        identity = dependency.identity
        if identity.production and identity.relative is not None:
            reached_count += 1
            reached_path_bytes += len(
                identity.relative.as_posix().encode("utf-8")
            )
    finding_count = 0
    finding_path_bytes = 0
    expression_bytes = 0
    reason_bytes = 0
    for finding in raw_findings:
        path = getattr(finding, "path", None)
        line = getattr(finding, "line", None)
        expression = getattr(finding, "expression", None)
        reason = getattr(finding, "reason", None)
        if (
            not isinstance(path, PurePosixPath)
            or not isinstance(line, int)
            or isinstance(line, bool)
            or line <= 0
            or not isinstance(expression, str)
            or not isinstance(reason, str)
        ):
            raise AuditInfrastructureError("audit finding cannot be compacted")
        finding_count += 1
        finding_path_bytes += len(path.as_posix().encode("utf-8"))
        expression_bytes += len(expression.encode("utf-8"))
        reason_bytes += len(reason.encode("utf-8"))
    dependency_path_bytes = sum(
        len(item.role_relative_path.as_posix().encode("utf-8"))
        + len(str(item.identity.canonical).encode("utf-8"))
        + len(item.stable_role.encode("ascii"))
        + (
            len(item.identity.relative.as_posix().encode("utf-8"))
            if item.identity.relative is not None
            else 0
        )
        for item in dependencies
    )
    bounds = CompactResultDraftBounds(
        len(dependencies),
        reached_count,
        finding_count,
        dependency_path_bytes + reached_path_bytes + finding_path_bytes,
        expression_bytes,
        reason_bytes,
    )
    encoded_bytes = _canonical_audit_result_json_bytes(
        configuration_digest,
        audit_engine_fingerprint,
        dependencies,
        raw_findings,
    )
    if encoded_bytes > _AUDIT_RESULT_MAXIMUM_ENCODED_BYTES:
        raise AuditInfrastructureError("encoded compact audit result limit exceeded")
    reservation.require_within_pre_dispatch_reservation(
        task_id, encoded_bytes, bounds, 4096
    )
    reservation.record_exact_canonical_json(task_id, encoded_bytes)
    reached_production = tuple(sorted(
        (
            dependency.identity.relative
            for dependency in dependencies
            if dependency.identity.production
            and dependency.identity.relative is not None
        ),
        key=PurePosixPath.as_posix,
    ))
    if len(set(reached_production)) != len(reached_production):
        raise AuditInfrastructureError("fresh production reachability is duplicated")
    findings = _compact_findings(raw_findings)
    return _CompactAuditDraft(
        configuration_digest,
        audit_engine_fingerprint,
        dependencies,
        reached_production,
        findings,
        task_id,
        generation,
    )


def validate_production_dependency_snapshots(
    dependencies: tuple[DependencyDigest, ...], production_snapshot
) -> None:
    if not isinstance(dependencies, tuple) or any(
        not isinstance(item, DependencyDigest) for item in dependencies
    ):
        raise AuditInfrastructureError("fresh production dependencies are invalid")
    if not hasattr(production_snapshot, "items"):
        raise AuditInfrastructureError("production snapshot generation is invalid")
    snapshot: dict[str, DependencyDigest] = {}
    for raw_path, dependency in production_snapshot.items():
        if not isinstance(dependency, DependencyDigest):
            raise AuditInfrastructureError("production snapshot generation is invalid")
        key = (
            raw_path.as_posix()
            if isinstance(raw_path, PurePosixPath)
            else str(raw_path).replace("\\", "/")
        )
        if key in snapshot:
            raise AuditInfrastructureError("production snapshot generation is ambiguous")
        snapshot[key] = dependency
    for dependency in dependencies:
        if dependency.stable_role != "production":
            continue
        expected = snapshot.get(dependency.role_relative_path.as_posix())
        if expected is None or expected != dependency:
            raise AuditInfrastructureError("production snapshot generation differs")


class _AuditLaunchContext:
    __slots__ = (
        "_control_endpoint", "_command_endpoint", "_task", "_deadline",
        "_macos_authorizations",
    )

    def __init__(self, control_endpoint, command_endpoint,
                 task: ConfigurationAuditTask, deadline: float) -> None:
        self._control_endpoint = control_endpoint
        self._command_endpoint = command_endpoint
        self._task = task
        self._deadline = deadline
        self._macos_authorizations = 0

    @property
    def task_id(self) -> str:
        return self._task.task_id

    @property
    def generation(self) -> int:
        return self._task.generation

    @property
    def macos_launch_deadline(self) -> float:
        return self._deadline

    @property
    def macos_launch_cancel_event(self):
        return _WORKER_CANCEL_EVENT

    def authorize_macos_compiler_exec(
        self, process_start: ProcessStartIdentity,
    ) -> CompilerExecPermit:
        if (
            process_start.platform_kind != "macos"
            or not isinstance(_WORKER_INDEX, int)
            or isinstance(_WORKER_INDEX, bool)
            or _WORKER_INDEX < 0
            or self._macos_authorizations >= 2
        ):
            raise AuditInfrastructureError(
                "macOS compiler exec authorization is invalid"
            )
        match = re.fullmatch(
            r"audit-([0-9]+)-[0-9a-f]{16}", self._task.task_id
        )
        if match is None:
            raise AuditInfrastructureError(
                "macOS compiler exec task identity is invalid"
            )
        task_ordinal = int(match.group(1))
        purpose = (
            CompilerLaunchPurpose.AUDIT_DISCOVERY,
            CompilerLaunchPurpose.AUDIT_ACCEPTED,
        )[self._macos_authorizations]
        capability = self._task.configuration.compiler_capability
        report = CompilerPgidReported(
            _WORKER_INDEX,
            self._task.generation,
            task_ordinal,
            purpose,
            process_start.pid,
            process_start.pid,
            process_start.native_start_token,
            capability.executable_identity,
            capability.executable_sha256,
            capability.capability_digest,
        )
        _send_control_frame_before(
            self._control_endpoint, report, self._deadline
        )
        permit = self._command_endpoint.receive_macos_exec_permit(
            self._task,
            task_ordinal,
            process_start.pid,
            _WORKER_CANCEL_EVENT,
            self._deadline,
        )
        self._macos_authorizations += 1
        return permit

    def register_compiler_process_launch(
        self, event: CompilerLaunchEvent, carrier: CompilerProcessHandleCarrier
    ) -> None:
        if (
            event.task_id != self._task.task_id
            or event.generation != self._task.generation
        ):
            raise AuditInfrastructureError("compiler process carrier generation differs")
        register = getattr(
            self._control_endpoint, "register_compiler_process_launch", None
        )
        if not callable(register):
            raise AuditInfrastructureError(
                "parent compiler process launch observer is unavailable"
            )
        register(event, carrier)
        _send_control_frame_before(self._control_endpoint, event, self._deadline)

    def complete_compiler_process_launch(
        self, event: CompilerLaunchEvent, carrier: CompilerProcessHandleCarrier
    ) -> None:
        complete = getattr(
            self._control_endpoint, "complete_compiler_process_launch", None
        )
        if not callable(complete):
            raise AuditInfrastructureError(
                "parent compiler process completion observer is unavailable"
            )
        complete(event, carrier)

    def fail_compiler_process_launch(
        self, event: CompilerLaunchEvent, carrier: CompilerProcessHandleCarrier
    ) -> None:
        fail = getattr(
            self._control_endpoint, "fail_compiler_process_launch", None
        )
        if callable(fail):
            fail(event, carrier)

    def record_process_start(
        self, purpose: CompilerLaunchPurpose, process_start: ProcessStartIdentity
    ) -> None:
        _send_control_frame_before(
            self._control_endpoint,
            CompilerLaunchEvent(
                purpose,
                process_start,
                worker_index=_WORKER_INDEX,
                task_id=self._task.task_id,
                generation=self._task.generation,
            ),
            self._deadline,
        )

    def seal(self) -> None:
        seal = getattr(self._control_endpoint, "seal_audit_launch_protocol", None)
        if not callable(seal):
            raise AuditInfrastructureError("audit compiler launch protocol seal is unavailable")
        seal(self._task.task_id, self._task.generation, self._deadline)


def _send_control_frame_before(control_endpoint, frame, deadline: float) -> None:
    if not isinstance(deadline, (int, float)) or isinstance(deadline, bool):
        raise AuditInfrastructureError("worker control deadline is invalid")
    if time.monotonic() >= deadline:
        raise AuditInfrastructureError("worker control deadline exceeded")
    send = getattr(control_endpoint, "send", None)
    if not callable(send):
        raise AuditInfrastructureError("worker control endpoint is invalid")
    send(frame, deadline)


def _receive_bounded_command_frame_before(
    command_endpoint,
    task: ConfigurationAuditTask,
    cancel_event,
    deadline: float,
    *,
    maximum_quantum_seconds: float,
):
    if (
        not isinstance(maximum_quantum_seconds, (int, float))
        or isinstance(maximum_quantum_seconds, bool)
        or maximum_quantum_seconds <= 0
        or maximum_quantum_seconds > 0.05
        or time.monotonic() >= deadline
    ):
        raise AuditInfrastructureError("worker command receive boundary is invalid")
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError("worker cancelled before publication permit")
    receive = getattr(command_endpoint, "receive", None)
    if not callable(receive):
        raise AuditInfrastructureError("worker command endpoint is invalid")
    return receive(task, cancel_event, deadline, maximum_quantum_seconds)


def _require_valid_dispatch_reservation(
    reservation, task: ConfigurationAuditTask, generation: int
) -> PerTaskCompactReservation:
    if not isinstance(reservation, PerTaskCompactReservation):
        raise AuditInfrastructureError("worker dispatch reservation is unavailable")
    if task.generation != generation:
        raise AuditInfrastructureError("worker dispatch reservation generation differs")
    reservation.validate(task.task_id, generation)
    return reservation


def audit_configuration_worker(
    task,
    dependency_roots,
    production_snapshot,
    control_endpoint,
    command_endpoint,
    deadline,
):
    if not isinstance(task, ConfigurationAuditTask):
        raise AuditInfrastructureError("configuration audit task is invalid")
    reservation = (
        task.compact_reservation
        if isinstance(task.compact_reservation, PerTaskCompactReservation)
        else None
    )
    publication_permit: CachePublicationPermit | None = None
    try:
        if task.dependency_root_authority is not dependency_roots:
            raise AuditInfrastructureError(
                "worker dependency-root authority identity changed"
            )
        if not isinstance(_WORKER_GENERATION, int) or isinstance(
            _WORKER_GENERATION, bool
        ):
            raise AuditInfrastructureError("worker generation is unavailable")
        reservation = _require_valid_dispatch_reservation(
            task.compact_reservation, task, _WORKER_GENERATION
        )
        reservation.require_before_discovery(task.task_id, _WORKER_GENERATION)
        if _WORKER_CANCEL_EVENT is not None and _WORKER_CANCEL_EVENT.is_set():
            raise AuditInfrastructureError("worker cancelled before discovery")
        if (
            not isinstance(_WORKER_INDEX, int)
            or isinstance(_WORKER_INDEX, bool)
            or _WORKER_INDEX < 0
            or not isinstance(_WORKER_PRODUCTION, Mapping)
            or not isinstance(_WORKER_LIMITS, AuditLimits)
            or not isinstance(_WORKER_ENGINE, str)
            or _WORKER_CACHE is None
            or not callable(getattr(_WORKER_RSS, "sample", None))
        ):
            raise AuditInfrastructureError("worker runtime binding is invalid")
        from gpu_capability_source_audit import (
            _attest_loaded_audit_engine,
            audit_preprocessed_view,
        )

        engine = (
            _WORKER_ENGINE
            if _WORKER_PREATTESTED_ENGINE == _WORKER_ENGINE
            else _attest_loaded_audit_engine(_WORKER_ENGINE)
        )
        launch_context = _AuditLaunchContext(
            control_endpoint, command_endpoint, task, deadline
        )
        view, discovery, preprocess_stages = stabilize_and_parse_configuration(
            task.configuration,
            dependency_roots,
            _WORKER_PRODUCTION,
            _WORKER_LIMITS,
            deadline,
            _WORKER_CANCEL_EVENT,
            launch_context=launch_context,
        )
        launch_context.seal()
        validate_production_dependency_snapshots(
            discovery.dependencies, production_snapshot
        )
        with _stage_timer() as audit_time:
            findings = audit_preprocessed_view(
                view, _WORKER_LIMITS, _WORKER_RSS.sample
            )
        draft = _bounded_compact_result_draft(
            task.configuration.digest,
            engine,
            discovery.dependencies,
            findings,
            task.task_id,
            task.generation,
            reservation,
        )
        del view
        set_publication_context = getattr(
            control_endpoint, "set_publication_context", None
        )
        if callable(set_publication_context):
            set_publication_context(
                task.task_id,
                task.generation,
                task.configuration.digest,
                engine,
                discovery.dependencies,
            )
        _send_control_frame_before(
            control_endpoint,
            CachePublicationRequested(
                _WORKER_INDEX, _WORKER_GENERATION, task.task_id
            ),
            deadline,
        )
        received = _receive_bounded_command_frame_before(
            command_endpoint,
            task,
            _WORKER_CANCEL_EVENT,
            deadline,
            maximum_quantum_seconds=0.05,
        )
        if not isinstance(received, CachePublicationPermit):
            raise AuditInfrastructureError("root publication permit is invalid")
        publication_permit = received
        if (
            _WORKER_CANCEL_EVENT is not None
            and _WORKER_CANCEL_EVENT.is_set()
        ):
            raise AuditInfrastructureError(
                "worker cancelled before compact-result materialization"
            )
        result = draft.materialize_under(reservation, publication_permit)
        with _stage_timer() as publish_time:
            accepted = _WORKER_CACHE.publish(
                task.configuration,
                dependency_roots,
                result,
                publication_permit,
                deadline,
            )
        if not isinstance(accepted, ConfigurationAuditResult):
            raise AuditInfrastructureError("cache publication result is invalid")
        if accepted is not result:
            ownership = result._transport_ownership
            if ownership is None:
                raise AuditInfrastructureError(
                    "cache publication result ownership is unavailable"
                )
            rebound = ownership.rebind_materialized_result()
            accepted = ConfigurationAuditResult(
                accepted.configuration_digest,
                accepted.audit_engine_fingerprint,
                accepted.dependencies,
                accepted.reached_production,
                accepted.findings,
                rebound,
            )
        stages = WorkerStageTimings(
            preprocess_stages.discovery_seconds,
            preprocess_stages.accepted_parse_seconds,
            audit_time.elapsed_seconds,
            publish_time.elapsed_seconds,
        )
        transport = encode_configuration_audit_result_transport(
            accepted,
            reservation.worker_transport_capability,
            preprocess_stages.compiler_stdout_bytes,
            stages,
        )
        del accepted
        del result
        del draft
        return ConfigurationAuditTransportOutcome(
            transport=transport,
            stdout_bytes=preprocess_stages.compiler_stdout_bytes,
            stages=stages,
        )
    finally:
        active_error = sys.exc_info()[1]
        cleanup_error: BaseException | None = None
        if publication_permit is not None and not publication_permit.released:
            try:
                publication_permit.release_root_publication()
            except BaseException as error:
                if active_error is not None:
                    active_error.add_note(
                        f"root publication permit cleanup also failed: {error}"
                    )
                else:
                    cleanup_error = error
        keep_receiver_ownership = (
            active_error is None
            and cleanup_error is None
            and reservation is not None
            and reservation.owner_phase == "receiver-retained-result"
        )
        if (
            reservation is not None
            and not reservation.released
            and not keep_receiver_ownership
        ):
            try:
                reservation.release(
                    "worker-failure" if active_error is not None else "worker-return"
                )
            except BaseException as error:
                if active_error is not None:
                    active_error.add_note(
                        f"compact reservation cleanup also failed: {error}"
                    )
                elif cleanup_error is not None:
                    cleanup_error.add_note(
                        f"compact reservation cleanup also failed: {error}"
                    )
                else:
                    cleanup_error = error
        if active_error is None and cleanup_error is not None:
            raise cleanup_error


def _release_rejected_worker_transport(
    reservation: PerTaskCompactReservation,
    capability: CompactResultTransportCapability,
    error: BaseException,
    context: str,
) -> None:
    if (
        reservation.released
        or reservation.owner_phase != "worker-transport-dispatched"
    ):
        return
    try:
        reservation.release_worker_transport_capability(
            capability, "receiver-transport-rejected"
        )
        return
    except BaseException as capability_error:
        error.add_note(f"{context} capability cleanup also failed: {capability_error}")
    if reservation.released:
        return
    try:
        reservation.release("receiver-transport-rejected")
    except BaseException as fallback_error:
        error.add_note(f"{context} fallback cleanup also failed: {fallback_error}")


def receive_configuration_audit_outcome(
    reservation: PerTaskCompactReservation,
    capability: CompactResultTransportCapability,
    worker_outcome: ConfigurationAuditTransportOutcome,
) -> ConfigurationAuditOutcomeOwner:
    if (
        not isinstance(reservation, PerTaskCompactReservation)
        or not isinstance(capability, CompactResultTransportCapability)
    ):
        raise AuditInfrastructureError("worker audit transport outcome is invalid")
    try:
        if not isinstance(worker_outcome, ConfigurationAuditTransportOutcome):
            raise AuditInfrastructureError(
                "worker audit transport outcome is invalid"
            )
        result = decode_configuration_audit_result_transport(
            worker_outcome.transport, reservation, capability
        )
        try:
            return ConfigurationAuditOutcomeOwner(ConfigurationAuditOutcome(
                result, worker_outcome.stdout_bytes, worker_outcome.stages
            ))
        except BaseException as error:
            ownership = result._transport_ownership
            if (
                ownership is not None
                and ownership.active
                and ownership.phase == "receiver-retained-result"
            ):
                try:
                    result.release_transport_ownership()
                except BaseException as cleanup_error:
                    error.add_note(
                        "received outcome construction cleanup also failed: "
                        f"{cleanup_error}"
                    )
            raise
    except BaseException as error:
        _release_rejected_worker_transport(
            reservation, capability, error, "worker transport rejection"
        )
        raise


def receive_configuration_audit_outcome_from_pipe(
    reservation: PerTaskCompactReservation,
    capability: CompactResultTransportCapability,
    connection,
    deadline: float,
    *,
    cancel_event=None,
    worker_alive=None,
) -> ConfigurationAuditOutcomeOwner:
    """Receive, authenticate, decode, and retain one worker result linearly."""
    if (
        not isinstance(reservation, PerTaskCompactReservation)
        or not isinstance(capability, CompactResultTransportCapability)
    ):
        raise AuditInfrastructureError("worker audit transport owner is invalid")
    try:
        worker_outcome = receive_configuration_audit_transport(
            connection,
            capability,
            deadline,
            cancel_event=cancel_event,
            worker_alive=worker_alive,
        )
        return receive_configuration_audit_outcome(
            reservation, capability, worker_outcome
        )
    except BaseException as error:
        _release_rejected_worker_transport(
            reservation, capability, error, "worker transport pipe"
        )
        raise


class _StreamDigestConsumer:
    def __init__(self, downstream: Callable[[bytes], None] | None = None) -> None:
        self._digest = hashlib.sha256()
        self._count = 0
        self._downstream = downstream

    def __call__(self, chunk: bytes) -> None:
        self._digest.update(chunk)
        self._count += len(chunk)
        if self._downstream is not None:
            self._downstream(chunk)

    def finish(self) -> StreamDigest:
        return StreamDigest(self._digest.hexdigest(), self._count)


def _dependency_binding(
    path: Path, authority: DependencyRootAuthority
) -> tuple[str, PurePosixPath]:
    matches: list[tuple[str, PurePosixPath]] = []
    for binding in (authority.source_root, *authority.external_roots):
        try:
            relative = path.relative_to(binding.resolved_root)
        except ValueError:
            continue
        matches.append(
            (binding.stable_role, PurePosixPath(relative.as_posix()))
        )
    if len(matches) != 1:
        raise AuditInfrastructureError(
            "dependency is not mapped by exactly one dependency-root authority"
        )
    return matches[0]


def _hash_dependency_identity(
    identity: FileIdentity,
    authority: DependencyRootAuthority,
    deadline: float | None = None,
    cancel_event: object | None = None,
    expected_snapshot: tuple[int, int | None, int, int, int] | None = None,
) -> DependencyDigest:
    path = identity.canonical
    stable_role, relative = _dependency_binding(path, authority)
    digest = hashlib.sha256()
    try:
        _check_dependency_budget(deadline, cancel_event)
        before = path.stat()
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            while chunk := stream.read(1024 * 1024):
                _check_dependency_budget(deadline, cancel_event)
                digest.update(chunk)
            after_open = os.fstat(stream.fileno())
        after = path.stat()
    except OSError as error:
        raise AuditInfrastructureError("dependency changed while hashing") from error
    snapshot = lambda item: (
        int(item.st_dev),
        int(item.st_ino) if int(item.st_ino) != 0 else None,
        int(item.st_size),
        int(item.st_mtime_ns),
        int(getattr(item, "st_ctime_ns", 0)),
    )
    expected = snapshot(before)
    if expected_snapshot is not None and expected != expected_snapshot:
        raise AuditInfrastructureError("dependency changed before hashing")
    if (
        snapshot(opened)[:4] != expected[:4]
        or snapshot(after_open)[:4] != expected[:4]
        or snapshot(after) != expected
    ):
        raise AuditInfrastructureError("dependency changed while hashing")
    if (identity.device, identity.inode) != expected[:2]:
        raise AuditInfrastructureError("dependency identity changed while hashing")
    return DependencyDigest(stable_role, relative, identity, digest.hexdigest())


def _dependency_digests(
    identities: tuple[FileIdentity, ...],
    authority: DependencyRootAuthority,
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[DependencyDigest, ...]:
    snapshots = []
    total_bytes = 0
    for identity in identities:
        _check_dependency_budget(deadline, cancel_event)
        try:
            metadata = identity.canonical.stat()
        except OSError as error:
            raise AuditInfrastructureError("dependency changed before hashing") from error
        snapshot = (
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) else None,
            int(metadata.st_size), int(metadata.st_mtime_ns),
            int(getattr(metadata, "st_ctime_ns", 0)),
        )
        if snapshot[2] > _DEPENDENCY_FILE_BYTES:
            raise AuditInfrastructureError("dependency per-file byte ceiling exceeded")
        total_bytes += snapshot[2]
        if total_bytes > _DEPENDENCY_TOTAL_BYTES:
            raise AuditInfrastructureError("dependency total byte ceiling exceeded")
        snapshots.append(snapshot)
    return tuple(
        _hash_dependency_identity(
            identity, authority, deadline, cancel_event, snapshot
        )
        for identity, snapshot in zip(identities, snapshots)
    )


def _parse_dependency_output(
    rewritten: RewrittenCommand,
    configuration: PreprocessConfiguration,
    authority: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    deadline: float | None = None,
    cancel_event: object | None = None,
) -> tuple[FileIdentity, ...]:
    if rewritten.dependency_format == "gcc-depfile":
        paths = parse_gcc_dependencies(
            rewritten.dependency_output, deadline=deadline,
            cancel_event=cancel_event,
        )
    elif rewritten.dependency_format == "msvc-json":
        paths = parse_msvc_dependencies(
            rewritten.dependency_output, deadline=deadline,
            cancel_event=cancel_event,
        )
    else:
        raise AuditInfrastructureError(
            f"unsupported dependency format: {rewritten.dependency_format}"
        )
    return validate_dependency_identities(
        _absolute_dependencies(paths, configuration.working_directory),
        authority.source_root.resolved_root,
        production,
        deadline=deadline,
        cancel_event=cancel_event,
    )


_GENERATION_GUARD_IDENTITY_METADATA_BYTES = 96
_GENERATION_GUARD_OWNER_METADATA_BYTES = 128
_GENERATION_GUARD_INDEX_METADATA_BYTES = 72


@dataclass(frozen=True, slots=True)
class GenerationGuardPreflight:
    dependencies: tuple[DependencyDigest, ...]
    dependencies_by_path: Mapping[str, DependencyDigest]
    directory_paths: tuple[Path, ...]
    file_paths: tuple[Path, ...]
    metadata_bytes: int
    dependency_root_authority: DependencyRootAuthority


@dataclass(frozen=True, slots=True)
class DependencyGenerationGuard:
    dependency: DependencyDigest
    held_file_owner: object
    held_parent_owner: object
    armed_stat: FileIdentity
    armed_size: int
    armed_mtime_ns: int
    armed_ctime_ns: int
    platform_watch: object
    directory_chain_owners: tuple[object, ...]
    directory_chain_identities: tuple[FileIdentity, ...]


def _checked_guard_metadata_add(total: int, amount: int, ceiling: int) -> int:
    if amount < 0 or total > sys.maxsize - amount:
        raise AuditInfrastructureError("dependency generation metadata overflow")
    result = total + amount
    if result > ceiling:
        raise AuditInfrastructureError("dependency generation metadata byte ceiling exceeded")
    return result


def preflight_dependency_generation_guards(
    discovery: PreprocessDiscovery,
    dependency_roots: DependencyRootAuthority,
    limits: AuditLimits,
) -> GenerationGuardPreflight:
    authority = validate_dependency_root_authority(dependency_roots)
    if not isinstance(discovery, PreprocessDiscovery) or not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("dependency generation preflight is invalid")
    dependencies_by_path: dict[str, DependencyDigest] = {}
    directory_paths: dict[str, Path] = {}
    for dependency in discovery.dependencies:
        if not isinstance(dependency, DependencyDigest):
            raise AuditInfrastructureError("dependency generation preflight is invalid")
        path = dependency.identity.canonical
        key = os.path.normcase(str(path))
        if key in dependencies_by_path:
            raise AuditInfrastructureError("dependency generation path is duplicate or ambiguous")
        dependencies_by_path[key] = dependency
        role, _relative = _dependency_binding(path, authority)
        binding = next(
            item for item in (authority.source_root, *authority.external_roots)
            if item.stable_role == role
        )
        try:
            relative_parent = path.parent.relative_to(binding.resolved_root)
        except ValueError as error:
            raise AuditInfrastructureError("dependency generation path escaped authority") from error
        current = binding.resolved_root.parent
        directory_paths.setdefault(os.path.normcase(str(current)), current)
        current = binding.resolved_root
        directory_paths.setdefault(os.path.normcase(str(current)), current)
        for part in relative_parent.parts:
            current = current / part
            directory_paths.setdefault(os.path.normcase(str(current)), current)
    if not dependencies_by_path:
        raise AuditInfrastructureError("dependency generation file ceiling exceeded")
    if len(dependencies_by_path) > limits.unique_dependency_handles:
        raise AuditInfrastructureError("dependency generation file ceiling exceeded")
    if len(directory_paths) > limits.unique_generation_guard_directories:
        raise AuditInfrastructureError("dependency generation directory ceiling exceeded")
    metadata_bytes = 0
    fixed = (
        _GENERATION_GUARD_IDENTITY_METADATA_BYTES
        + _GENERATION_GUARD_OWNER_METADATA_BYTES
        + _GENERATION_GUARD_INDEX_METADATA_BYTES
    )
    for path in (*directory_paths.values(), *(item.identity.canonical for item in dependencies_by_path.values())):
        encoded_bytes = len(os.fsencode(str(path)))
        metadata_bytes = _checked_guard_metadata_add(
            metadata_bytes, fixed + encoded_bytes,
            limits.generation_guard_metadata_bytes,
        )
    return GenerationGuardPreflight(
        tuple(dependencies_by_path.values()),
        MappingProxyType(dependencies_by_path),
        tuple(directory_paths.values()),
        tuple(item.identity.canonical for item in dependencies_by_path.values()),
        metadata_bytes, authority,
    )


class _DependencyGenerationGuards:
    """OS-observed held-file/path-chain validation between the two runs."""

    def __init__(
        self,
        preflight: GenerationGuardPreflight,
        authority: DependencyRootAuthority,
        deadline: float | None = None,
    ) -> None:
        if not isinstance(preflight, GenerationGuardPreflight):
            raise AuditInfrastructureError("dependency generation preflight is invalid")
        if preflight.dependency_root_authority is not authority:
            raise AuditInfrastructureError("dependency generation authority was replaced")
        _check_dependency_budget(deadline, None)
        self._authority = authority
        self._discovery_dependencies = preflight.dependencies
        self._dependencies_by_path = preflight.dependencies_by_path
        self._streams: list[object] = []
        self._observer: _FilesystemGenerationObserver | None = None
        self.guards: tuple[DependencyGenerationGuard, ...] = ()
        try:
            self._directories = tuple(
                (path, self._directory_snapshot(path))
                for path in preflight.directory_paths
            )
            self._files = tuple(
                (dependency.identity.canonical,
                 self._file_snapshot(dependency.identity.canonical.stat()))
                for dependency in preflight.dependencies
            )
            for path, snapshot in self._files:
                _check_dependency_budget(deadline, None)
                stream = path.open("rb")
                opened = os.fstat(stream.fileno())
                if (
                    self._file_snapshot(opened)[:4] != snapshot[:4]
                    or self._file_snapshot(path.stat()) != snapshot
                ):
                    stream.close()
                    raise AuditInfrastructureError("dependency changed while guards were armed")
                self._streams.append(stream)
            self._observer = _FilesystemGenerationObserver(
                tuple((path, True) for path, _snapshot in self._directories)
                + tuple((path, False) for path, _snapshot in self._files)
            )
            self._observer.drain()
            directory_identities = tuple(
                FileIdentity(path, None, snapshot[0], snapshot[1], 0, False)
                for path, snapshot in self._directories
            )
            directory_paths = preflight.directory_paths
            self.guards = tuple(
                DependencyGenerationGuard(
                    dependency, stream, path.parent, dependency.identity,
                    snapshot[2], snapshot[3], snapshot[4], self,
                    directory_paths,
                    directory_identities,
                )
                for dependency, stream, (path, snapshot) in zip(
                    preflight.dependencies, self._streams, self._files
                )
            )
        except BaseException as primary_error:
            try:
                self.close()
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"production table cleanup also failed: {cleanup_error!r}"
                )
            raise

    @staticmethod
    def _file_snapshot(metadata) -> tuple[int, int | None, int, int, int]:
        return (
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            int(metadata.st_size),
            int(metadata.st_mtime_ns),
            int(getattr(metadata, "st_ctime_ns", 0)),
        )

    @staticmethod
    def _directory_snapshot(path: Path) -> tuple[int, int | None, int]:
        metadata = path.lstat()
        if (
            not stat.S_ISDIR(metadata.st_mode)
            or stat.S_ISLNK(metadata.st_mode)
            or bool(getattr(metadata, "st_file_attributes", 0) & 0x400)
        ):
            raise AuditInfrastructureError("dependency directory guard is invalid")
        return (
            int(metadata.st_dev),
            int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
            int(metadata.st_mode),
        )

    def validate_and_hash(
        self,
        deadline: float | None = None,
        cancel_event: object | None = None,
    ) -> tuple[DependencyDigest, ...]:
        if self._observer is None:
            raise AuditInfrastructureError("dependency generation guards are not armed")
        self._observer.drain()
        for path, expected in self._directories:
            if self._directory_snapshot(path) != expected:
                raise AuditInfrastructureError("dependency directory generation changed")
        result: list[DependencyDigest] = []
        total_bytes = 0
        for _path, expected in self._files:
            _check_dependency_budget(deadline, cancel_event)
            if expected[2] > _DEPENDENCY_FILE_BYTES:
                raise AuditInfrastructureError("dependency per-file byte ceiling exceeded")
            total_bytes += expected[2]
            if total_bytes > _DEPENDENCY_TOTAL_BYTES:
                raise AuditInfrastructureError("dependency total byte ceiling exceeded")
        for stream, (path, expected) in zip(self._streams, self._files):
            opened = os.fstat(stream.fileno())
            if (
                self._file_snapshot(opened)[:4] != expected[:4]
                or self._file_snapshot(path.stat()) != expected
            ):
                raise AuditInfrastructureError("dependency generation changed")
            dependency = self._dependencies_by_path.get(
                os.path.normcase(str(path))
            )
            if dependency is None or dependency.identity.canonical != path:
                raise AuditInfrastructureError(
                    "dependency generation lookup changed"
                )
            digest = hashlib.sha256()
            stream.seek(0)
            while chunk := stream.read(1024 * 1024):
                _check_dependency_budget(deadline, cancel_event)
                digest.update(chunk)
            stream.seek(0)
            if self._file_snapshot(os.fstat(stream.fileno()))[:4] != expected[:4]:
                raise AuditInfrastructureError("dependency generation changed while hashing")
            result.append(DependencyDigest(
                dependency.stable_role,
                dependency.role_relative_path,
                dependency.identity,
                digest.hexdigest(),
            ))
        self._observer.drain()
        for path, expected in self._directories:
            if self._directory_snapshot(path) != expected:
                raise AuditInfrastructureError("dependency directory generation changed")
        return tuple(result)

    def bind(self, dependencies: tuple[DependencyDigest, ...]) -> "_DependencyGenerationGuards":
        if dependencies != self._discovery_dependencies:
            raise AuditInfrastructureError("dependency guard binding changed")
        self._discovery_dependencies = dependencies
        return self

    def close(self) -> None:
        errors: list[BaseException] = []
        observer = getattr(self, "_observer", None)
        if observer is not None:
            try:
                observer.close()
            except BaseException as error:
                errors.append(error)
            self._observer = None
        for stream in reversed(getattr(self, "_streams", ())):
            try:
                stream.close()
            except BaseException as error:
                errors.append(error)
        self._streams = []
        if errors:
            raise AuditInfrastructureError("dependency generation guard cleanup failed") from errors[0]


def arm_dependency_generation_guards(
    preflight: GenerationGuardPreflight,
    dependency_roots: DependencyRootAuthority,
    deadline: float,
) -> tuple[DependencyGenerationGuard, ...]:
    authority = validate_dependency_root_authority(dependency_roots)
    if preflight.dependency_root_authority is not authority:
        raise AuditInfrastructureError("dependency generation authority was replaced")
    owner = _DependencyGenerationGuards(preflight, authority, deadline)
    return owner.guards


def _guard_owner(
    guards: tuple[DependencyGenerationGuard, ...],
) -> _DependencyGenerationGuards:
    if not guards or any(
        not isinstance(guard, DependencyGenerationGuard) for guard in guards
    ):
        raise AuditInfrastructureError("dependency generation guards are invalid")
    owner = guards[0].platform_watch
    if not isinstance(owner, _DependencyGenerationGuards) or any(
        guard.platform_watch is not owner for guard in guards
    ) or owner.guards != guards:
        raise AuditInfrastructureError("dependency generation guards are invalid")
    return owner


def validate_and_hash_guarded_dependencies(
    guards: tuple[DependencyGenerationGuard, ...],
    deadline: float,
    cancel_event: object | None = None,
) -> tuple[DependencyDigest, ...]:
    owner = _guard_owner(guards)
    try:
        _check_dependency_budget(deadline, cancel_event)
        result = owner.validate_and_hash(deadline, cancel_event)
        _check_dependency_budget(deadline, cancel_event)
        return result
    finally:
        owner.close()


def discover_configuration(
    configuration: PreprocessConfiguration,
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
    cancel_event: object | None = None,
    *,
    launch_context: _AuditLaunchContext | None = None,
) -> PreprocessDiscovery:
    authority = validate_dependency_root_authority(
        dependency_roots,
        expected_digest=configuration.dependency_root_authority_digest,
    )
    validate_compiler_executable_capability(
        configuration.compiler_capability, authority,
        deadline=deadline, cancel_event=cancel_event,
    )
    consumer = _StreamDigestConsumer()
    with tempfile.TemporaryDirectory(
        prefix=".gpu-capability-discovery-",
    ) as temporary:
        suffix = ".json" if configuration.family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        } else ".d"
        rewritten = rewrite_preprocess_command(
            configuration, Path(temporary) / f"dependencies{suffix}"
        )
        run_bounded_preprocessor(
            rewritten,
            configuration,
            limits,
            deadline,
            consumer,
            cancel_event,
            launch_context=launch_context,
            launch_purpose=(
                CompilerLaunchPurpose.AUDIT_DISCOVERY
                if launch_context is not None else None
            ),
        )
        identities = _parse_dependency_output(
            rewritten, configuration, authority, production,
            deadline, cancel_event,
        )
        dependencies = _dependency_digests(
            identities, authority, deadline, cancel_event
        )
    return PreprocessDiscovery(consumer.finish(), dependencies, identities)


def stabilize_and_parse_configuration(
    configuration: PreprocessConfiguration,
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
    cancel_event: object | None = None,
    publication: Callable[[
        PreprocessedTranslationUnitView,
        PreprocessDiscovery,
        Callable[[], None],
    ], PreprocessedTranslationUnitView] | None = None,
    *,
    launch_context: _AuditLaunchContext | None = None,
) -> tuple[PreprocessedTranslationUnitView, PreprocessDiscovery, PreprocessStageTimings]:
    authority = validate_dependency_root_authority(
        dependency_roots,
        expected_digest=configuration.dependency_root_authority_digest,
    )
    validate_compiler_executable_capability(
        configuration.compiler_capability, authority,
        deadline=deadline, cancel_event=cancel_event,
    )
    discovery_started = time.monotonic()
    discovery = discover_configuration(
        configuration,
        dependency_roots,
        production,
        limits,
        deadline,
        cancel_event,
        launch_context=launch_context,
    )
    discovery_seconds = time.monotonic() - discovery_started
    preflight = preflight_dependency_generation_guards(
        discovery, authority, limits
    )
    guards = arm_dependency_generation_guards(
        preflight, authority, deadline
    )
    guard_owner = _guard_owner(guards)
    accepted_started = time.monotonic()

    def validate_accepted_generation() -> None:
        _check_dependency_budget(deadline, cancel_event)
        dependencies = guard_owner.validate_and_hash(deadline, cancel_event)
        _check_dependency_budget(deadline, cancel_event)
        if dependencies != discovery.dependencies:
            raise AuditInfrastructureError("dependency closure changed")

    try:
        if cancel_event is not None and cancel_event.is_set():
            raise AuditInfrastructureError("preprocessing cancelled before accepted launch")
        builder = PreprocessedStreamBuilder(
            configuration,
            production,
            limits,
            _current_process_rss_bytes,
            deadline=deadline,
            cancel_event=cancel_event,
        )
        consumer = _StreamDigestConsumer(builder.feed)
        with tempfile.TemporaryDirectory(
            prefix=".gpu-capability-accepted-",
        ) as temporary:
            suffix = ".json" if configuration.family in {
                CompilerFamily.MSVC, CompilerFamily.CLANG_CL
            } else ".d"
            rewritten = rewrite_preprocess_command(
                configuration, Path(temporary) / f"dependencies{suffix}"
            )
            guard_owner.validate_and_hash(deadline, cancel_event)
            run_bounded_preprocessor(
                rewritten,
                configuration,
                limits,
                deadline,
                consumer,
                cancel_event,
                launch_context=launch_context,
                launch_purpose=(
                    CompilerLaunchPurpose.AUDIT_ACCEPTED
                    if launch_context is not None else None
                ),
            )
            accepted_identities = _parse_dependency_output(
                rewritten, configuration, authority, production,
                deadline, cancel_event,
            )
            _check_dependency_budget(deadline, cancel_event)
            accepted_dependencies = guard_owner.validate_and_hash(
                deadline, cancel_event
            )
            _check_dependency_budget(deadline, cancel_event)
            if tuple(item.identity for item in accepted_dependencies) != accepted_identities:
                raise AuditInfrastructureError("dependency closure changed")
        accepted_stream = consumer.finish()
        if accepted_stream.byte_count != discovery.stream.byte_count:
            raise AuditInfrastructureError("raw preprocessed byte count changed")
        if accepted_stream.sha256 != discovery.stream.sha256:
            raise AuditInfrastructureError("raw preprocessed output changed")
        if accepted_dependencies != discovery.dependencies:
            raise AuditInfrastructureError("dependency closure changed")
        view = builder.finalize(accepted_identities)
        if publication is None:
            validate_accepted_generation()
        else:
            view = publication(view, discovery, validate_accepted_generation)
    finally:
        guard_owner.close()
    return (
        view,
        discovery,
        PreprocessStageTimings(
            discovery_seconds,
            time.monotonic() - accepted_started,
            discovery.stream.byte_count + accepted_stream.byte_count,
        ),
    )


def preprocess_configuration(
    configuration: PreprocessConfiguration,
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    deadline: float,
    cancel_event: threading.Event | None = None,
) -> PreprocessedTranslationUnitView:
    """Preprocess, validate dependencies, then finalize one complete packed view."""

    started = time.monotonic()
    execution: ExecutionResult | None = None
    try:
        source_root = _source_root(production)
        builder = PreprocessedStreamBuilder(
            configuration,
            production,
            limits,
            _current_process_rss_bytes,
            deadline=deadline,
            cancel_event=cancel_event,
        )
        with tempfile.TemporaryDirectory(
            prefix=".gpu-capability-dependencies-",
            dir=str(configuration.working_directory),
        ) as temporary:
            suffix = ".json" if configuration.family in {
                CompilerFamily.MSVC,
                CompilerFamily.CLANG_CL,
            } else ".d"
            dependency_output = Path(temporary) / f"dependencies{suffix}"
            rewritten = rewrite_preprocess_command(configuration, dependency_output)
            execution = run_bounded_preprocessor(
                rewritten,
                configuration,
                limits,
                deadline,
                builder.feed,
                cancel_event,
            )
            if rewritten.dependency_format == "gcc-depfile":
                dependency_paths = parse_gcc_dependencies(dependency_output)
            elif rewritten.dependency_format == "msvc-json":
                dependency_paths = parse_msvc_dependencies(dependency_output)
            else:
                raise AuditInfrastructureError(
                    f"unsupported dependency format: {rewritten.dependency_format}"
                )
            dependencies = validate_dependency_identities(
                _absolute_dependencies(
                    dependency_paths, configuration.working_directory
                ),
                source_root,
                production,
            )
            return builder.finalize(dependencies)
    except (AuditInfrastructureError, OSError) as error:
        if str(error).startswith("GPU capability preprocessing failed:"):
            raise
        raise _diagnostic(
            configuration,
            str(error),
            exit_status=(0 if execution is not None else "not-started"),
            elapsed_seconds=(
                execution.elapsed_seconds
                if execution is not None
                else time.monotonic() - started
            ),
            observed_stdout_bytes=(
                execution.observed_stdout_bytes if execution is not None else 0
            ),
            stderr_tail=(execution.stderr_tail if execution is not None else b""),
        ) from error


def load_or_preprocess(
    configuration: PreprocessConfiguration,
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    cache: PreprocessCache,
    limits: AuditLimits,
    deadline: float,
    cancel_event: object | None = None,
) -> PreprocessedTranslationUnitView:
    """Return a validated cache hit or publish one complete compiler result."""

    if not isinstance(cache, PreprocessCache) and not callable(
        getattr(cache, "load_many", None)
    ):
        raise AuditInfrastructureError("preprocess cache is invalid")
    authority = validate_dependency_root_authority(
        dependency_roots,
        expected_digest=configuration.dependency_root_authority_digest,
    )
    validate_compiler_executable_capability(
        configuration.compiler_capability, authority,
        deadline=deadline, cancel_event=cancel_event,
    )
    cached = cache.load(configuration, deadline, cancel_event)
    if cached is not None:
        return cached
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError(
            f"configuration {configuration.digest} cancelled before preprocessing"
        )
    def publish_accepted(
        accepted: PreprocessedTranslationUnitView,
        discovery: PreprocessDiscovery,
        validate_generation: Callable[[], None],
    ) -> PreprocessedTranslationUnitView:
        try:
            snapshots = cache._snapshot_dependencies(
                discovery.dependency_identities, force=True
            )
        except OSError as error:
            raise AuditInfrastructureError(
                "dependency changed during preprocessing"
            ) from error
        if len(snapshots) != len(discovery.dependencies) or any(
            snapshot.identity != dependency.identity
            or snapshot.sha256 != dependency.sha256
            for snapshot, dependency in zip(snapshots, discovery.dependencies)
        ):
            raise AuditInfrastructureError(
                "dependency content changed during preprocessing"
            )
        if accepted.configuration != configuration:
            raise AuditInfrastructureError(
                "preprocessor returned a mismatched accepted configuration"
            )
        return cache._publish_stabilized(
            accepted,
            snapshots,
            final_validation=validate_generation,
            deadline=deadline,
            cancel_event=cancel_event,
        )

    accepted, _discovery, _stages = stabilize_and_parse_configuration(
        configuration,
        authority,
        production,
        limits,
        deadline,
        cancel_event,
        publication=publish_accepted,
    )
    return accepted


def _json_object_without_duplicates(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for name, value in pairs:
        if name in result:
            raise AuditInfrastructureError(
                f"compile database object has duplicate key: {name}"
            )
        result[name] = value
    return result


def _configuration_semantics(
    configuration: PreprocessConfiguration,
) -> tuple[object, ...]:
    return (
        configuration.family,
        configuration.compiler,
        configuration.working_directory,
        configuration.source,
        configuration.arguments,
        configuration.environment_digest,
        configuration.digest,
        configuration.dependency_root_authority_digest,
        configuration.compiler_capability_digest,
    )


def _database_path(root: Path, database: Path) -> Path:
    if not isinstance(database, Path):
        raise AuditInfrastructureError("compile database path is invalid")
    candidate = database if database.is_absolute() else root / database
    try:
        canonical = candidate.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile database is unavailable: {candidate}"
        ) from error
    if not canonical.is_file():
        raise AuditInfrastructureError(
            f"compile database must be one ordinary file: {canonical}"
        )
    return canonical


def _load_database_entries(database: Path) -> tuple[tuple[int, Mapping[str, object]], ...]:
    try:
        document = json.loads(
            database.read_text(encoding="utf-8-sig"),
            object_pairs_hook=_json_object_without_duplicates,
        )
    except AuditInfrastructureError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AuditInfrastructureError(
            f"cannot decode compile database {database}: {error}"
        ) from error
    if not isinstance(document, list):
        raise AuditInfrastructureError("compile database root must be an array")
    indexed: list[tuple[int, Mapping[str, object]]] = []
    for index, entry in enumerate(document):
        if not isinstance(entry, Mapping):
            raise AuditInfrastructureError(
                f"compile database entry must be an object: {database}:{index}"
            )
        indexed.append((index, entry))
    try:
        return tuple(
            sorted(
                indexed,
                key=lambda item: (
                    json.dumps(
                        item[1],
                        ensure_ascii=True,
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                    item[0],
                ),
            )
        )
    except (TypeError, ValueError) as error:
        raise AuditInfrastructureError(
            f"compile database contains a non-JSON entry: {database}"
        ) from error


def _entry_is_production(
    entry: Mapping[str, object],
    database: Path,
    source_root: Path,
    production: Mapping[PurePosixPath, FileIdentity],
) -> bool:
    directory_value = entry.get("directory")
    file_value = entry.get("file")
    if (
        not isinstance(directory_value, str)
        or not directory_value
        or "\0" in directory_value
    ):
        raise AuditInfrastructureError(
            "compile command directory must be a non-empty string"
        )
    if not isinstance(file_value, str) or not file_value or "\0" in file_value:
        raise AuditInfrastructureError(
            "compile command file must be a non-empty string"
        )
    lexical_directory = Path(directory_value)
    if not lexical_directory.is_absolute():
        lexical_directory = database.parent / lexical_directory
    lexical_directory = Path(os.path.abspath(lexical_directory))
    lexical_source = Path(file_value)
    if not lexical_source.is_absolute():
        lexical_source = lexical_directory / lexical_source
    lexical_source = Path(os.path.abspath(lexical_source))
    try:
        lexical_directory.relative_to(source_root)
        relative = lexical_source.relative_to(source_root)
    except ValueError as error:
        raise AuditInfrastructureError(
            "compile command directory or source is outside production root: "
            f"{lexical_source}"
        ) from error
    if not relative.parts or relative.parts[0].casefold() not in {
        "playback",
        "recorder_engine",
    }:
        return False

    working_directory, _arguments = decode_compile_entry(
        entry,
        database,
        windows=os.name == "nt",
    )
    source = Path(file_value)
    if not source.is_absolute():
        source = working_directory / source
    try:
        source = source.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile entry source is unavailable: {file_value}"
        ) from error

    for identity in production.values():
        if source == identity.canonical:
            return True
    try:
        metadata = source.stat()
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile entry source is unavailable: {source}"
        ) from error
    source_file_id = (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) else None,
    )
    for identity in production.values():
        identity_file_id = (identity.device, identity.inode)
        if identity_file_id[1] is not None and source_file_id == identity_file_id:
            raise AuditInfrastructureError(
                f"compile entry source aliases production file: {source}"
            )
    return False


def collect_configurations(
    root: Path,
    databases: tuple[Path, ...],
    environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    pipeline_deadline: float,
) -> tuple[PreprocessConfiguration, ...]:
    """Normalize every database entry and coalesce only semantic duplicates."""

    if not isinstance(root, Path):
        raise AuditInfrastructureError("production root is invalid")
    lexical_root = root.absolute()
    authority = validate_dependency_root_authority(dependency_roots)
    if (
        not isinstance(pipeline_deadline, (int, float))
        or isinstance(pipeline_deadline, bool)
        or time.monotonic() >= pipeline_deadline
    ):
        raise AuditInfrastructureError("configuration collection deadline exceeded")
    if not isinstance(databases, tuple) or not databases:
        raise AuditInfrastructureError("at least one compile database is required")
    # This validates environment keys and values before compiler probing begins.
    _environment_digest(environment)
    limits = AuditLimits()
    production = enumerate_production_identities(
        lexical_root, limits, pipeline_deadline
    )
    if time.monotonic() >= pipeline_deadline:
        raise AuditInfrastructureError("configuration collection deadline exceeded")
    # Enumeration has already rejected every aliasing root component, so this
    # resolution cannot hide a symlink/reparse traversal from identity checks.
    canonical_root = lexical_root.resolve(strict=True)
    if canonical_root != authority.source_root.resolved_root:
        raise AuditInfrastructureError("collection root differs from dependency authority")
    canonical_databases = sorted(
        {_database_path(lexical_root, database) for database in databases},
        key=lambda path: (str(path).casefold(), str(path)),
    )
    by_digest: dict[str, PreprocessConfiguration] = {}
    for database in canonical_databases:
        for entry_index, entry in _load_database_entries(database):
            if time.monotonic() >= pipeline_deadline:
                raise AuditInfrastructureError(
                    "configuration collection deadline exceeded"
                )
            if not _entry_is_production(
                entry,
                database,
                canonical_root,
                production,
            ):
                continue
            configuration = make_configuration(
                entry,
                database,
                entry_index,
                canonical_root,
                production,
                environment,
                limits,
                authority,
                None,
                "",
                pipeline_deadline,
            )
            previous = by_digest.get(configuration.digest)
            if previous is None:
                by_digest[configuration.digest] = configuration
            elif _configuration_semantics(previous) != _configuration_semantics(
                configuration
            ):
                raise AuditInfrastructureError(
                    f"configuration digest collision: {configuration.digest}"
                )
    result = tuple(by_digest[digest] for digest in sorted(by_digest))
    production.close()
    return result


def _decision_root_context(configuration: PreprocessConfiguration, database: Path,
                           dependency_roots: DependencyRootAuthority,
                           source_root: Path) -> tuple[str, Path, Path]:
    bindings = (dependency_roots.source_root, *dependency_roots.external_roots)
    working_matches = []
    compiler_matches = []
    for binding in bindings:
        try:
            configuration.working_directory.relative_to(binding.resolved_root)
            working_matches.append(binding)
        except ValueError:
            pass
        try:
            configuration.compiler.relative_to(binding.resolved_root)
            compiler_matches.append(binding)
        except ValueError:
            pass
    if len(working_matches) > 1 or len(compiler_matches) != 1:
        raise AuditInfrastructureError("decision root roles are ambiguous")
    if working_matches:
        working = working_matches[0]
        role = working.stable_role
        build_root = working.resolved_root
    else:
        role = "build"
        build_root = database.parent
    toolchain_root = compiler_matches[0].resolved_root
    if source_root == toolchain_root or build_root == toolchain_root:
        raise AuditInfrastructureError("decision typed roots are ambiguous")
    return role, build_root, toolchain_root


def _inspection_counter(accountant: object) -> int:
    value = getattr(accountant, "inspection_probe_invocations", None)
    if (not isinstance(value, int) or isinstance(value, bool) or value < 0):
        raise AuditInfrastructureError("inspection invocation counter is unavailable")
    return value


def collect_configurations_with_decision_records(
    source_root: Path,
    compile_commands: tuple[Path, ...],
    launcher_environment: Mapping[str, str],
    production: Mapping[PurePosixPath, FileIdentity],
    dependency_roots: DependencyRootAuthority,
    capability_registry: object,
    inspection_cache: object,
    expected_audit_engine_fingerprint: str,
    limits: AuditLimits,
    pipeline_deadline: float,
    launch_accountant: object,
) -> ConfigurationCollection:
    """Collect immutable configurations plus relocation-safe decision sidecars."""

    from gpu_capability_source_audit import _attest_loaded_audit_engine

    _attest_loaded_audit_engine(expected_audit_engine_fingerprint)
    if (not isinstance(source_root, Path)
            or not isinstance(compile_commands, tuple) or not compile_commands
            or any(not isinstance(path, Path) for path in compile_commands)
            or not isinstance(launcher_environment, Mapping)
            or not isinstance(production, Mapping)
            or not isinstance(dependency_roots, DependencyRootAuthority)
            or dependency_roots.source_root.resolved_root != source_root
            or not isinstance(limits, AuditLimits)
            or not isinstance(pipeline_deadline, (int, float))
            or isinstance(pipeline_deadline, bool)
            or time.monotonic() >= pipeline_deadline):
        raise AuditInfrastructureError("decision configuration collection is invalid")
    before = _inspection_counter(launch_accountant)
    register = getattr(capability_registry, "register", None)
    if not callable(register):
        raise AuditInfrastructureError("compiler capability registry is invalid")
    by_digest: dict[str, PreprocessConfiguration] = {}
    records: dict[str, DecisionConfigurationRecord] = {}
    canonical_source = source_root.resolve(strict=True)
    for requested_database in compile_commands:
        database = _database_path(canonical_source, requested_database)
        for entry_index, entry in _load_database_entries(database):
            if time.monotonic() >= pipeline_deadline:
                raise AuditInfrastructureError(
                    "decision configuration collection deadline exceeded")
            if not _entry_is_production(entry, database, canonical_source, production):
                continue
            configuration = make_configuration(
                entry, database, entry_index, canonical_source, production,
                launcher_environment, limits, dependency_roots, inspection_cache,
                expected_audit_engine_fingerprint, pipeline_deadline,
                launch_accountant)
            existing = by_digest.get(configuration.digest)
            if existing is not None:
                semantics_differ = (
                    _configuration_semantics(existing)
                    != _configuration_semantics(configuration))
                duplicate_owner = configuration.compiler_capability.native_owner
                retained_owner = existing.compiler_capability.native_owner
                if duplicate_owner is not retained_owner:
                    close_duplicate = getattr(duplicate_owner, "close", None)
                    if not callable(close_duplicate):
                        raise AuditInfrastructureError(
                            "duplicate compiler capability owner is invalid")
                    close_duplicate()
                if semantics_differ:
                    raise AuditInfrastructureError(
                        f"configuration digest collision: {configuration.digest}")
                continue
            registered = register(configuration.compiler_capability)
            if registered != configuration.compiler_capability_digest:
                raise AuditInfrastructureError("compiler capability registry digest differs")
            inspection = inspect_compiler(
                configuration.compiler, configuration.family,
                launcher_environment, dependency_roots, inspection_cache,
                expected_audit_engine_fingerprint, pipeline_deadline, limits,
                launch_accountant, working_directory=configuration.working_directory,
                preprocess_arguments=configuration.arguments,
                compiler_capability=configuration.compiler_capability)
            role, build_root, toolchain_root = _decision_root_context(
                configuration, database, dependency_roots, canonical_source)
            _compiler, _launcher_arguments, assignments = strip_launchers(
                decode_compile_entry(entry, database, os.name == "nt")[1])
            rewritten = rewrite_preprocess_command(
                configuration, configuration.working_directory
                / ".olr-decision-dependencies")
            try:
                relative_source = configuration.source.canonical.relative_to(
                    canonical_source)
            except ValueError as error:
                raise AuditInfrastructureError(
                    "decision production source is outside source root") from error
            record = DecisionConfigurationRecord(
                configuration_digest=configuration.digest,
                compiler_family=configuration.family,
                compiler_digest=compiler_digest_from_capability(
                    configuration.family, configuration.compiler_capability,
                    inspection.normalized_version, deadline=pipeline_deadline),
                production_source=PurePosixPath(relative_source.as_posix()),
                normalized_decision_arguments=normalize_decision_arguments(
                    rewritten, configuration.working_directory, role,
                    canonical_source, build_root, toolchain_root),
                decision_environment_digest=decision_environment_digest(
                    configuration.family, launcher_environment, assignments,
                    configuration.working_directory, role, canonical_source,
                    build_root, toolchain_root),
                working_directory_role=role,
                compiler_capability_digest=configuration.compiler_capability_digest,
            )
            by_digest[configuration.digest] = configuration
            records[configuration.digest] = record
    after = _inspection_counter(launch_accountant)
    if after < before:
        raise AuditInfrastructureError("inspection invocation counter regressed")
    return ConfigurationCollection(
        configurations=tuple(by_digest.values()),
        decision_records=MappingProxyType(records),
        inspection_probe_invocations=after - before,
        dependency_root_authority=dependency_roots,
        capability_registry=capability_registry,
    )


def _configuration_is_objcpp(configuration: PreprocessConfiguration) -> bool:
    if configuration.source.canonical.suffix.casefold() == ".mm":
        return True
    arguments = configuration.arguments
    for index, argument in enumerate(arguments):
        lowered = argument.casefold()
        if lowered == "-x" and index + 1 < len(arguments):
            if arguments[index + 1].casefold() in {"objective-c++", "objective-c++-cpp-output"}:
                return True
        if lowered.startswith("-x") and lowered[2:] in {
            "objective-c++",
            "objective-c++-cpp-output",
        }:
            return True
        if lowered.startswith("/clang:-x") and lowered[9:] in {
            "objective-c++",
            "objective-c++-cpp-output",
        }:
            return True
    return False


def _is_windows_backend_path(path: PurePosixPath) -> bool:
    parts = tuple(part.casefold() for part in path.parts)
    stem = path.stem.casefold()
    return "win" in parts[:-1] or stem.endswith(("_win", "_mediafoundation"))


def _validated_orchestration_inputs(
    configurations: tuple[PreprocessConfiguration, ...],
    production: Mapping[PurePosixPath, FileIdentity],
    cache,
    limits: AuditLimits,
) -> tuple[PreprocessConfiguration, ...]:
    if not isinstance(configurations, tuple) or not configurations:
        raise AuditInfrastructureError("no usable compile configurations")
    if not isinstance(cache, (PreprocessCache, ConfigurationAuditCache)):
        raise AuditInfrastructureError("orchestration cache is invalid")
    if isinstance(cache, ConfigurationAuditCache) and not cache.is_prepared:
        raise AuditInfrastructureError("result cache is not prepared")
    if not isinstance(limits, AuditLimits):
        raise AuditInfrastructureError("audit limits are invalid")
    if (
        not isinstance(limits.workers, int)
        or isinstance(limits.workers, bool)
        or limits.workers <= 0
        or not isinstance(limits.total_seconds, (int, float))
        or isinstance(limits.total_seconds, bool)
        or limits.total_seconds <= 0
    ):
        raise AuditInfrastructureError("orchestration limits are invalid")
    _source_root(production)
    by_digest: dict[str, PreprocessConfiguration] = {}
    for configuration in configurations:
        if not isinstance(configuration, PreprocessConfiguration):
            raise AuditInfrastructureError("preprocess configuration is invalid")
        relative = configuration.source.relative
        if relative is None or production.get(relative) != configuration.source:
            raise AuditInfrastructureError(
                "configuration source is not in the production identity table: "
                f"{configuration.digest}"
            )
        previous = by_digest.get(configuration.digest)
        if previous is None:
            by_digest[configuration.digest] = configuration
        elif _configuration_semantics(previous) != _configuration_semantics(
            configuration
        ):
            raise AuditInfrastructureError(
                f"configuration digest collision: {configuration.digest}"
            )
    return tuple(by_digest[digest] for digest in sorted(by_digest))


def _validate_pipeline_cache_authorities(
    cache, inspection_cache
) -> None:
    if not isinstance(cache, ConfigurationAuditCache):
        raise AuditInfrastructureError("result cache is invalid")
    if not cache.is_prepared:
        raise AuditInfrastructureError("result cache is not prepared")
    if not isinstance(inspection_cache, CompilerInspectionCache):
        raise AuditInfrastructureError("compiler inspection cache is invalid")
    if cache.root_identity != inspection_cache.root_identity:
        raise AuditInfrastructureError("inspection cache root identity mismatch")


def _coverage_path(
    identity: FileIdentity | None,
    production: Mapping[PurePosixPath, FileIdentity],
) -> PurePosixPath | None:
    if identity is None:
        return None
    if not identity.production:
        return None
    relative = identity.relative
    if relative is None or production.get(relative) != identity:
        raise AuditInfrastructureError(
            f"preprocessed view contains an unknown production identity: {identity.canonical}"
        )
    return relative


def _view_production_provenance(
    view: PreprocessedTranslationUnitView,
    production: Mapping[PurePosixPath, FileIdentity],
) -> frozenset[PurePosixPath]:
    reached: set[PurePosixPath] = set()
    tokens = view.tokens
    for run in tokens.iter_runs():
        relative = _coverage_path(tokens.identity_for(run.identity_id), production)
        if relative is not None:
            reached.add(relative)
    return frozenset(reached)


def compute_active_sources(
    configurations: tuple[PreprocessConfiguration, ...],
    production: Mapping[PurePosixPath, FileIdentity],
) -> tuple[frozenset[PurePosixPath], frozenset[PurePosixPath]]:
    """Select build-active translation units from the locked exact platform table."""

    if (
        not isinstance(configurations, tuple)
        or any(
            not isinstance(configuration, PreprocessConfiguration)
            for configuration in configurations
        )
        or not isinstance(production, Mapping)
        or any(
            not isinstance(path, PurePosixPath)
            or not isinstance(identity, FileIdentity)
            or identity.relative != path
            for path, identity in production.items()
        )
    ):
        raise AuditInfrastructureError("active source inputs are invalid")
    configured_families = frozenset(
        configuration.family for configuration in configurations
    )
    has_objcpp = any(
        _configuration_is_objcpp(configuration)
        for configuration in configurations
    )
    has_windows_backend = any(
        configuration.source.relative is not None
        and _CONDITIONALLY_SELECTED_TRANSLATION_UNITS.get(
            configuration.source.relative
        ) == "windows"
        for configuration in configurations
    )
    active: set[PurePosixPath] = set()
    for path in production:
        if path.suffix.casefold() not in {".c", ".cc", ".cpp", ".cxx", ".mm"}:
            continue
        selected = _CONDITIONALLY_SELECTED_TRANSLATION_UNITS.get(path)
        if selected == "apple" and not has_objcpp:
            continue
        if selected == "windows" and not has_windows_backend:
            continue
        if selected is None and not configured_families:
            continue
        active.add(path)
    configured_sources = frozenset(
        configuration.source.relative
        for configuration in configurations
        if configuration.source.relative is not None
    )
    return frozenset(active), configured_sources


@dataclass(frozen=True, slots=True)
class ProductionSourceSnapshot:
    identity: FileIdentity
    raw_bytes: bytes
    sha256: str

    def __post_init__(self) -> None:
        if not isinstance(self.identity, FileIdentity) or not isinstance(
            self.raw_bytes, bytes
        ):
            raise AuditInfrastructureError("production source snapshot is invalid")
        if (
            not isinstance(self.sha256, str)
            or len(self.sha256) != 64
            or any(character not in "0123456789abcdef" for character in self.sha256)
            or hashlib.sha256(self.raw_bytes).hexdigest() != self.sha256
        ):
            raise AuditInfrastructureError("production source snapshot digest differs")


class _ProductionMemoryBudget:
    __slots__ = ("current_bytes", "peak_bytes", "_by_category")

    def __init__(self) -> None:
        self.current_bytes = 0
        self.peak_bytes = 0
        self._by_category: dict[str, int] = {}

    def reserve(
        self, category: str, byte_count: int, category_limit: int
    ) -> "_ProductionReservation":
        shared_nonraw_bytes = (
            self._by_category.get("decoded-retained", 0) + byte_count
            if category == "decoded-transient"
            else byte_count
        )
        if (
            category not in {"raw", "decoded-transient", "decoded-retained"}
            or not isinstance(byte_count, int)
            or isinstance(byte_count, bool)
            or byte_count < 0
            or not isinstance(category_limit, int)
            or isinstance(category_limit, bool)
            or category_limit < 0
            or byte_count > category_limit - self._by_category.get(category, 0)
            or (
                category == "decoded-transient"
                and shared_nonraw_bytes > category_limit
            )
        ):
            raise AuditInfrastructureError(
                f"production {category.replace('-', ' ')} limit exceeded"
            )
        self._by_category[category] = (
            self._by_category.get(category, 0) + byte_count
        )
        self.current_bytes += byte_count
        self.peak_bytes = max(self.peak_bytes, self.current_bytes)
        return _ProductionReservation(self, category, byte_count)

    def _release(self, category: str, byte_count: int) -> None:
        current = self._by_category.get(category, 0)
        if byte_count > current or byte_count > self.current_bytes:
            raise AuditInfrastructureError(
                "production allocation ownership underflow"
            )
        self._by_category[category] = current - byte_count
        self.current_bytes -= byte_count

    def preflight_transfer(
        self, category: str, byte_count: int, category_limit: int
    ) -> "_PendingProductionReservation":
        if (
            category != "decoded-retained"
            or not isinstance(byte_count, int)
            or isinstance(byte_count, bool)
            or byte_count < 0
            or byte_count > category_limit - self._by_category.get(category, 0)
        ):
            raise AuditInfrastructureError(
                "production decoded retained limit exceeded"
            )
        return _PendingProductionReservation(self, category, byte_count)

    def commit_split_transfer(
        self,
        old: "_ProductionReservation",
        pending: "_PendingProductionReservation",
        remaining_transient_bytes: int,
    ) -> tuple["_ProductionReservation", "_ProductionReservation"]:
        if (
            old.released
            or old._budget is not self
            or old.category != "decoded-transient"
            or pending._budget is not self
            or pending.committed
            or not isinstance(remaining_transient_bytes, int)
            or isinstance(remaining_transient_bytes, bool)
            or remaining_transient_bytes < 0
            or remaining_transient_bytes + pending.byte_count != old.byte_count
        ):
            raise AuditInfrastructureError(
                "production allocation transfer is invalid"
            )
        old_current = self._by_category.get(old.category, 0)
        if old.byte_count > old_current:
            raise AuditInfrastructureError(
                "production allocation ownership underflow"
            )
        self._by_category[old.category] = (
            old_current - old.byte_count + remaining_transient_bytes
        )
        self._by_category[pending.category] = (
            self._by_category.get(pending.category, 0) + pending.byte_count
        )
        old.released = True
        pending.committed = True
        return (
            _ProductionReservation(
                self, old.category, remaining_transient_bytes
            ),
            _ProductionReservation(
                self, pending.category, pending.byte_count
            ),
        )


class _ProductionReservation:
    __slots__ = ("_budget", "category", "byte_count", "released")

    def __init__(
        self, budget: _ProductionMemoryBudget, category: str, byte_count: int
    ) -> None:
        self._budget = budget
        self.category = category
        self.byte_count = byte_count
        self.released = False

    def release(self) -> None:
        if self.released:
            raise AuditInfrastructureError(
                "production allocation ownership was already released"
            )
        self._budget._release(self.category, self.byte_count)
        self.released = True

    def __del__(self) -> None:
        try:
            if not self.released:
                self.release()
        except BaseException:
            pass


class _PendingProductionReservation:
    __slots__ = ("_budget", "category", "byte_count", "committed")

    def __init__(self, budget, category: str, byte_count: int) -> None:
        self._budget = budget
        self.category = category
        self.byte_count = byte_count
        self.committed = False


@dataclass(frozen=True, slots=True)
class _HeldProductionFile:
    path: PurePosixPath
    identity: FileIdentity
    stream: object
    opened_identity: tuple[int, int | None]
    opened_generation: tuple[int, int, int]
    initial_sha256: str


def _open_production_descriptor(path: Path):
    try:
        before = path.lstat()
    except OSError as error:
        raise AuditInfrastructureError(
            "production snapshot public path is unavailable"
        ) from error
    if (
        stat.S_ISLNK(before.st_mode)
        or bool(getattr(before, "st_file_attributes", 0) & 0x400)
        or not stat.S_ISREG(before.st_mode)
        or int(getattr(before, "st_nlink", 1)) != 1
    ):
        raise AuditInfrastructureError(
            "production snapshot public path is unsafe"
        )
    if os.name != "nt":
        flags = (
            os.O_RDONLY
            | os.O_NOFOLLOW
            | getattr(os, "O_CLOEXEC", 0)
        )
        descriptor = os.open(path, flags)
    else:
        import ctypes
        import msvcrt
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_file = kernel32.CreateFileW
        create_file.argtypes = (
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        )
        create_file.restype = wintypes.HANDLE
        handle = create_file(
            str(path),
            0x80000000,
            0x00000001,
            None,
            3,
            0x00000080 | 0x00200000,
            None,
        )
        numeric = ctypes.cast(handle, ctypes.c_void_p).value
        invalid = ctypes.c_void_p(-1).value
        if numeric in (None, invalid):
            raise AuditInfrastructureError(
                "production snapshot descriptor open failed"
            )
        try:
            descriptor = msvcrt.open_osfhandle(int(numeric), os.O_RDONLY)
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
    try:
        stream = os.fdopen(descriptor, "rb", closefd=True)
    except BaseException:
        os.close(descriptor)
        raise
    try:
        opened = os.fstat(stream.fileno())
        after = path.lstat()
        identities = {
            (int(value.st_dev), int(value.st_ino))
            for value in (before, opened, after)
        }
        if (
            len(identities) != 1
            or any(not stat.S_ISREG(value.st_mode) for value in (opened, after))
            or any(
                int(getattr(value, "st_nlink", 1)) != 1
                for value in (opened, after)
            )
        ):
            raise AuditInfrastructureError(
                "production snapshot public path generation differs"
            )
        return stream
    except BaseException:
        stream.close()
        raise


class _ImmutableProductionMapping:
    __slots__ = (
        "_values", "_backing", "_budget", "_ownership", "_closed"
    )

    def __init__(self, values, budget, ownership, *, take_dict=False) -> None:
        if take_dict:
            if type(values) is not dict:
                raise AuditInfrastructureError(
                    "owned production mapping carrier is invalid"
                )
            owned_values = values
        else:
            owned_values = dict(values)
        self._backing = owned_values
        self._values = MappingProxyType(self._backing)
        self._budget = budget
        self._ownership = ownership
        self._closed = False

    def __getitem__(self, key):
        self._require_open()
        return self._values[key]

    def __iter__(self):
        self._require_open()
        return iter(self._values)

    def __len__(self):
        self._require_open()
        return len(self._values)

    def items(self):
        self._require_open()
        return self._values.items()

    def values(self):
        self._require_open()
        return self._values.values()

    def get(self, key, default=None):
        self._require_open()
        return self._values.get(key, default)

    def _require_open(self) -> None:
        if self._closed:
            raise AuditInfrastructureError("production mapping is closed")

    @property
    def allocation_budget(self) -> _ProductionMemoryBudget:
        return self._budget

    def close(self) -> None:
        if self._closed:
            return
        self._backing.clear()
        self._closed = True
        if self._ownership is not None and not self._ownership.released:
            self._ownership.release()

    def __del__(self) -> None:
        try:
            self.close()
        except BaseException:
            pass


def _production_snapshot_structure_bound(
    production: Mapping[PurePosixPath, FileIdentity],
) -> int:
    """Own every scalable snapshot carrier before opening the first file."""

    from gpu_capability_source_audit import (
        conservative_allocation_schema as policy_allocation_schema,
    )

    schema = policy_allocation_schema()
    source_count = len(production)
    return schema.checked_add(
        4096,
        schema.checked_multiply(3, schema.list_bound(source_count)),
        schema.checked_multiply(
            _production_snapshot_structure_dict_count(),
            schema.dict_bound(source_count),
        ),
        schema.checked_multiply(
            source_count,
            schema.checked_add(
                schema.object_bound(10),  # held descriptor carrier
                schema.object_bound(6),   # dependency digest
                schema.object_bound(4),   # final source snapshot
                schema.tuple_bound(2),    # sorted table entry
                schema.string_bound(64),  # digest text
                schema.bytes_objects_bound(0, 1),
            ),
        ),
        schema.checked_multiply(
            5, schema.object_bound(8)
        ),  # mapping proxies, prepared/held tables and raw owner
    )


def _production_snapshot_structure_dict_count() -> int:
    # Frozen production table, initial digest map and finalized snapshot map
    # coexist from the final read through the policy boundary.
    return 3


class _PreparedProductionTable:
    __slots__ = (
        "table", "_backing", "budget", "structure_ownership", "claimed",
        "_enumerated_owner",
    )

    def __init__(self, production, limits: AuditLimits) -> None:
        self.budget = _ProductionMemoryBudget()
        self.structure_ownership = None
        self.table = None
        self._backing = None
        self._enumerated_owner = None
        self.claimed = False
        descriptor_ceiling = limits.unique_dependency_handles
        if os.name == "nt":
            import ctypes

            getmaxstdio = ctypes.cdll.msvcrt._getmaxstdio
            getmaxstdio.restype = ctypes.c_int
            descriptor_ceiling = min(
                descriptor_ceiling, max(0, int(getmaxstdio()) - 64)
            )
        else:
            import resource

            soft_limit, _hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
            if soft_limit != resource.RLIM_INFINITY:
                descriptor_ceiling = min(
                    descriptor_ceiling, max(0, int(soft_limit) - 64)
                )
        if len(production) > descriptor_ceiling:
            raise AuditInfrastructureError(
                "production snapshot descriptor ceiling exceeded"
            )
        if isinstance(production, _EnumeratedProductionTable):
            if (
                production.claimed
                or production._closed
                or production.table is None
                or production.structure_ownership is None
                or production.structure_ownership.released
            ):
                raise AuditInfrastructureError(
                    "enumerated production table ownership is invalid"
                )
            production.claimed = True
            self._enumerated_owner = production
            self.budget = production.budget
            self.structure_ownership = production.structure_ownership
            self._backing = production._backing
            self.table = production.table
            return
        structure_bytes = _production_snapshot_structure_bound(production)
        try:
            _production_allocation_event("reserve-snapshot-structure")
            self.structure_ownership = self.budget.reserve(
                "decoded-retained",
                structure_bytes,
                limits.production_decoded_retained_bytes,
            )
            _production_allocation_event("construct-frozen-table")
            self._backing = {}
            for path, identity in production.items():
                self._backing[path] = identity
            self.table = MappingProxyType(self._backing)
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        if self._backing is not None:
            self._backing.clear()
        if (
            not self.claimed
            and self.structure_ownership is not None
            and not self.structure_ownership.released
        ):
            self.structure_ownership.release()


def _prepare_production_table(production, limits) -> _PreparedProductionTable:
    return _PreparedProductionTable(production, limits)


class HeldProductionSnapshot:
    """Owns the one descriptor per production path through the policy boundary."""

    def __init__(
        self,
        production: Mapping[PurePosixPath, FileIdentity],
        limits: AuditLimits,
        pipeline_deadline: float,
        prepared_table: _PreparedProductionTable | None = None,
    ) -> None:
        if (
            not isinstance(production, Mapping)
            or not isinstance(limits, AuditLimits)
            or not isinstance(pipeline_deadline, (int, float))
            or isinstance(pipeline_deadline, bool)
            or not math.isfinite(pipeline_deadline)
        ):
            raise AuditInfrastructureError("production snapshot inputs are invalid")
        prepared = (
            _prepare_production_table(production, limits)
            if prepared_table is None
            else prepared_table
        )
        if prepared_table is None:
            production = prepared.table
        if (
            not isinstance(prepared, _PreparedProductionTable)
            or prepared.claimed
            or prepared.table is not production
            or prepared.structure_ownership is None
            or prepared.structure_ownership.released
        ):
            raise AuditInfrastructureError(
                "prepared production table ownership is invalid"
            )
        prepared.claimed = True
        self._budget = prepared.budget
        self._limits = limits
        self._files: list[_HeldProductionFile] = []
        self._structure_ownership = None
        self._raw_ownerships: list[_ProductionReservation] = []
        self._finalized_mappings: list[_ImmutableProductionMapping] = []
        self._initial_digest_backing = None
        self._prepared_table = prepared
        self._closed = False
        self._finalized = False
        try:
            self._structure_ownership = prepared.structure_ownership
            _production_allocation_event("reserve-raw")
            self._raw_ownerships.append(self._budget.reserve(
                "raw",
                limits.production_raw_aggregate_bytes,
                limits.production_raw_aggregate_bytes,
            ))
            _production_allocation_event("construct-initial-digest-map")
            initial: dict[PurePosixPath, DependencyDigest] = {}
            raw_total = 0
            for path, identity in production.items():
                self._check_deadline(pipeline_deadline)
                if (
                    not isinstance(path, PurePosixPath)
                    or not isinstance(identity, FileIdentity)
                    or identity.relative != path
                    or not identity.production
                ):
                    raise AuditInfrastructureError(
                        "production snapshot identity table is invalid"
                    )
                _production_allocation_event("open")
                stream = _open_production_descriptor(identity.canonical)
                try:
                    metadata = os.fstat(stream.fileno())
                    opened_identity = (
                        int(metadata.st_dev), int(metadata.st_ino) or None
                    )
                    expected_identity = (identity.device, identity.inode)
                    if expected_identity != opened_identity:
                        raise AuditInfrastructureError(
                            "production snapshot generation differs"
                        )
                    size = int(metadata.st_size)
                    if size > limits.production_raw_per_file_bytes:
                        raise AuditInfrastructureError(
                            "production raw per-file limit exceeded"
                        )
                    if size > limits.production_raw_aggregate_bytes - raw_total:
                        raise AuditInfrastructureError(
                            "production raw aggregate limit exceeded"
                        )
                    raw_total += size
                    _production_allocation_event("read")
                    raw = stream.read(size)
                    if len(raw) != size:
                        raise AuditInfrastructureError(
                            "production snapshot unstable read"
                        )
                    after = os.fstat(stream.fileno())
                    if self._stat_generation(after) != self._stat_generation(metadata):
                        raise AuditInfrastructureError(
                            "production snapshot generation differs"
                        )
                    digest = hashlib.sha256(raw).hexdigest()
                    initial[path] = DependencyDigest(
                        "production", path, identity, digest
                    )
                    self._files.append(_HeldProductionFile(
                        path,
                        identity,
                        stream,
                        opened_identity,
                        self._stat_generation(metadata),
                        digest,
                    ))
                    del raw
                except BaseException:
                    stream.close()
                    raise
        except BaseException:
            self._close_no_raise()
            raise
        self._initial_digest_backing = initial
        self.initial_digest_map = MappingProxyType(
            self._initial_digest_backing
        )

    @staticmethod
    def _stat_generation(metadata) -> tuple[int, int, int]:
        return (
            int(metadata.st_size),
            int(metadata.st_mtime_ns),
            int(metadata.st_ctime_ns),
        )

    @staticmethod
    def _check_deadline(deadline: float) -> None:
        if (
            not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or not math.isfinite(deadline)
            or time.monotonic() >= deadline
        ):
            raise AuditInfrastructureError("production snapshot deadline exceeded")

    def __enter__(self) -> "HeldProductionSnapshot":
        if self._closed:
            raise AuditInfrastructureError("production snapshot is closed")
        return self

    def finalize_policy_boundary(
        self, pipeline_deadline: float
    ) -> Mapping[PurePosixPath, ProductionSourceSnapshot]:
        if self._closed or self._finalized:
            raise AuditInfrastructureError(
                "production snapshot policy boundary is unavailable"
            )
        _production_allocation_event("construct-final-snapshot-map")
        finalized: dict[PurePosixPath, ProductionSourceSnapshot] = {}
        for held in self._files:
            self._check_deadline(pipeline_deadline)
            before = os.fstat(held.stream.fileno())
            current_identity = (int(before.st_dev), int(before.st_ino) or None)
            try:
                path_metadata = held.identity.canonical.lstat()
                current_canonical = held.identity.canonical.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                raise AuditInfrastructureError(
                    "production snapshot generation differs"
                ) from error
            path_identity = (
                int(path_metadata.st_dev), int(path_metadata.st_ino) or None
            )
            if (
                current_identity != held.opened_identity
                or path_identity != held.opened_identity
                or current_canonical != held.identity.canonical
                or stat.S_ISLNK(path_metadata.st_mode)
                or self._stat_generation(before) != held.opened_generation
            ):
                raise AuditInfrastructureError(
                    "production snapshot generation differs"
                )
            held.stream.seek(0)
            _production_allocation_event("read-final")
            raw = held.stream.read(held.opened_generation[0])
            after = os.fstat(held.stream.fileno())
            if (
                self._stat_generation(after) != held.opened_generation
                or hashlib.sha256(raw).hexdigest() != held.initial_sha256
            ):
                raise AuditInfrastructureError(
                    "production snapshot generation differs"
                )
            finalized[held.path] = ProductionSourceSnapshot(
                held.identity, raw, held.initial_sha256
            )
        self._finalized = True
        finalized_mapping = _ImmutableProductionMapping(
            finalized, self._budget, None, take_dict=True
        )
        self._finalized_mappings.append(finalized_mapping)
        return finalized_mapping

    def release_finalized_sources(
        self, mapping: _ImmutableProductionMapping
    ) -> None:
        if mapping not in self._finalized_mappings:
            raise AuditInfrastructureError(
                "production raw mapping ownership differs"
            )
        mapping.close()
        self._finalized_mappings.remove(mapping)
        for ownership in reversed(self._raw_ownerships):
            if not ownership.released:
                ownership.release()
        self._raw_ownerships.clear()

    def _close_no_raise(self) -> None:
        for held in reversed(self._files):
            try:
                held.stream.close()
            except BaseException:
                pass
        self._files.clear()
        for mapping in reversed(self._finalized_mappings):
            try:
                mapping.close()
            except BaseException:
                pass
        self._finalized_mappings.clear()
        for ownership in reversed(self._raw_ownerships):
            if not ownership.released:
                try:
                    ownership.release()
                except BaseException:
                    pass
        self._raw_ownerships.clear()
        if self._initial_digest_backing is not None:
            self._initial_digest_backing.clear()
        try:
            self._prepared_table.close()
        except BaseException:
            pass
        if (
            self._structure_ownership is not None
            and not self._structure_ownership.released
        ):
            try:
                self._structure_ownership.release()
            except BaseException:
                pass
        self._closed = True

    def __exit__(self, exc_type, exc, traceback) -> None:
        cleanup_errors: list[BaseException] = []
        for held in reversed(self._files):
            try:
                held.stream.close()
            except BaseException as error:
                cleanup_errors.append(error)
        self._files.clear()
        for mapping in reversed(self._finalized_mappings):
            try:
                mapping.close()
            except BaseException as error:
                cleanup_errors.append(error)
        self._finalized_mappings.clear()
        for ownership in reversed(self._raw_ownerships):
            if not ownership.released:
                try:
                    ownership.release()
                except BaseException as error:
                    cleanup_errors.append(error)
        self._raw_ownerships.clear()
        if self._initial_digest_backing is not None:
            self._initial_digest_backing.clear()
        try:
            self._prepared_table.close()
        except BaseException as error:
            cleanup_errors.append(error)
        if (
            self._structure_ownership is not None
            and not self._structure_ownership.released
        ):
            try:
                self._structure_ownership.release()
            except BaseException as error:
                cleanup_errors.append(error)
        self._closed = True
        if cleanup_errors and exc is None:
            raise AuditInfrastructureError(
                "production snapshot descriptor cleanup failed"
            ) from cleanup_errors[0]
        if cleanup_errors and exc is not None:
            exc.add_note(
                f"production snapshot cleanup also failed: {cleanup_errors[0]!r}"
            )


def snapshot_production_sources(
    production: Mapping[PurePosixPath, FileIdentity],
    limits: AuditLimits,
    pipeline_deadline: float,
    *,
    prepared_table: _PreparedProductionTable | None = None,
) -> HeldProductionSnapshot:
    return HeldProductionSnapshot(
        production, limits, pipeline_deadline, prepared_table
    )


@dataclass(frozen=True, slots=True)
class _ProductionDecodeAllocationBounds:
    transient_bytes: int
    retained_bytes: int
    decoder_scratch_bytes: int

    def __post_init__(self) -> None:
        if (
            any(
                not isinstance(value, int)
                or isinstance(value, bool)
                or value < 0
                for value in (
                    self.transient_bytes,
                    self.retained_bytes,
                    self.decoder_scratch_bytes,
                )
            )
            or self.transient_bytes
            != self.retained_bytes + self.decoder_scratch_bytes
        ):
            raise AuditInfrastructureError(
                "production decode allocation bounds are invalid"
            )


def _production_decode_allocation_bounds(
    sources: Mapping[PurePosixPath, ProductionSourceSnapshot],
) -> _ProductionDecodeAllocationBounds:
    """Premeasure every scalable decode object from raw sizes only."""

    from gpu_capability_source_audit import (
        conservative_allocation_schema as policy_allocation_schema,
    )

    if not isinstance(sources, _ImmutableProductionMapping):
        raise AuditInfrastructureError("production decode sources are invalid")
    schema = policy_allocation_schema()
    source_count = len(sources)
    raw_total = 0
    maximum_integer = 0
    for path, source in sources.items():
        if not isinstance(path, PurePosixPath) or not isinstance(
            source, ProductionSourceSnapshot
        ):
            raise AuditInfrastructureError("production decode source is invalid")
        raw_size = len(source.raw_bytes)
        raw_total = schema.checked_add(raw_total, raw_size)
        maximum_integer = max(maximum_integer, raw_size)
    retained = schema.checked_add(
        schema.string_objects_bound(raw_total, source_count),
        schema.dict_bound(source_count),
        schema.object_bound(4),
    )
    decoder_scratch = schema.checked_add(
        schema.json_decoder_fixed_bytes,
        schema.object_bound(32),
        schema.dict_bound(source_count),
        schema.list_bound(source_count),
        schema.list_bound(source_count),
        schema.checked_multiply(source_count, schema.tuple_bound(2)),
        schema.checked_multiply(
            source_count, schema.pylong_bound(maximum_integer)
        ),
    )
    return _ProductionDecodeAllocationBounds(
        schema.checked_add(retained, decoder_scratch),
        retained,
        decoder_scratch,
    )


def _probe_production_decode_schema() -> None:
    """Fail closed if representative runtime objects exceed the closed schema."""

    from gpu_capability_source_audit import (
        conservative_allocation_schema as policy_allocation_schema,
    )

    schema = policy_allocation_schema()
    if (
        sys.getsizeof(0) > schema.pylong_bound(0)
        or sys.getsizeof("") > schema.string_bound(0)
        or sys.getsizeof([]) > schema.list_bound(0)
        or sys.getsizeof(()) > schema.tuple_bound(0)
        or sys.getsizeof({}) > schema.dict_bound(0)
    ):
        raise AuditInfrastructureError(
            "production decode allocation schema probe failed"
        )


def _strict_utf8_code_point_count(
    raw: bytes, pipeline_deadline: float
) -> int:
    index = 0
    count = 0
    size = len(raw)
    next_deadline_check = 0
    while index < size:
        if index >= next_deadline_check:
            HeldProductionSnapshot._check_deadline(pipeline_deadline)
            next_deadline_check = index + (64 * 1024)
        first = raw[index]
        if first <= 0x7F:
            index += 1
        elif 0xC2 <= first <= 0xDF:
            if index + 1 >= size or raw[index + 1] & 0xC0 != 0x80:
                raise AuditInfrastructureError("production source is not strict UTF-8")
            index += 2
        elif 0xE0 <= first <= 0xEF:
            if index + 2 >= size:
                raise AuditInfrastructureError("production source is not strict UTF-8")
            second, third = raw[index + 1], raw[index + 2]
            if (
                second & 0xC0 != 0x80
                or third & 0xC0 != 0x80
                or (first == 0xE0 and second < 0xA0)
                or (first == 0xED and second >= 0xA0)
            ):
                raise AuditInfrastructureError("production source is not strict UTF-8")
            index += 3
        elif 0xF0 <= first <= 0xF4:
            if index + 3 >= size:
                raise AuditInfrastructureError("production source is not strict UTF-8")
            second, third, fourth = raw[index + 1:index + 4]
            if (
                second & 0xC0 != 0x80
                or third & 0xC0 != 0x80
                or fourth & 0xC0 != 0x80
                or (first == 0xF0 and second < 0x90)
                or (first == 0xF4 and second >= 0x90)
            ):
                raise AuditInfrastructureError("production source is not strict UTF-8")
            index += 4
        else:
            raise AuditInfrastructureError("production source is not strict UTF-8")
        count += 1
    HeldProductionSnapshot._check_deadline(pipeline_deadline)
    return count


def decode_validated_production_sources(
    sources: Mapping[PurePosixPath, ProductionSourceSnapshot],
    limits: AuditLimits,
    pipeline_deadline: float,
) -> Mapping[PurePosixPath, str]:
    """Strictly decode a finalized immutable snapshot under conservative caps."""

    if not isinstance(sources, _ImmutableProductionMapping) or not isinstance(
        limits, AuditLimits
    ):
        raise AuditInfrastructureError("production decode inputs are invalid")
    raw_total = 0
    for path, source in sources.items():
        HeldProductionSnapshot._check_deadline(pipeline_deadline)
        if not isinstance(path, PurePosixPath) or not isinstance(
            source, ProductionSourceSnapshot
        ):
            raise AuditInfrastructureError("production decode source is invalid")
        raw_size = len(source.raw_bytes)
        if raw_size > limits.production_raw_per_file_bytes:
            raise AuditInfrastructureError("production raw per-file limit exceeded")
        if raw_size > limits.production_raw_aggregate_bytes - raw_total:
            raise AuditInfrastructureError("production raw aggregate limit exceeded")
        raw_total += raw_size
    bounds = _production_decode_allocation_bounds(sources)
    if bounds.transient_bytes > limits.production_decoded_transient_bytes:
        raise AuditInfrastructureError(
            "production decoded transient limit exceeded"
        )
    if bounds.retained_bytes > limits.production_decoded_retained_bytes:
        raise AuditInfrastructureError(
            "production decoded retained limit exceeded"
        )
    transient = None
    scratch = None
    retained = None
    retained_pending = None
    decoded_values = None
    try:
        _production_allocation_event("reserve-decode")
        transient = sources.allocation_budget.reserve(
            "decoded-transient",
            bounds.transient_bytes,
            limits.production_decoded_transient_bytes,
        )
        _production_allocation_event("reserve-retained-strings")
        retained_pending = sources.allocation_budget.preflight_transfer(
            "decoded-retained",
            bounds.retained_bytes,
            limits.production_decoded_retained_bytes,
        )
        _production_allocation_event("transfer-retained-before-construction")
        scratch, retained = sources.allocation_budget.commit_split_transfer(
            transient, retained_pending, bounds.decoder_scratch_bytes
        )
        transient = None
        retained_pending = None
        _production_allocation_event("decode")
        _probe_production_decode_schema()
        for _path, source in sources.items():
            _strict_utf8_code_point_count(
                source.raw_bytes, pipeline_deadline
            )
        _production_allocation_event("construct-strings")
        decoded_values = {}
        for path, source in sources.items():
            HeldProductionSnapshot._check_deadline(pipeline_deadline)
            try:
                decoded_values[path] = source.raw_bytes.decode(
                    "utf-8", errors="strict"
                )
            except UnicodeDecodeError as error:
                raise AuditInfrastructureError(
                    f"production source is not strict UTF-8: {path}"
                ) from error
            HeldProductionSnapshot._check_deadline(pipeline_deadline)
        _production_allocation_event("construct-decoded-map")
        result = _ImmutableProductionMapping(
            decoded_values,
            sources.allocation_budget,
            retained,
            take_dict=True,
        )
        retained = None
        decoded_values = None
        if scratch is not None and not scratch.released:
            scratch.release()
        scratch = None
        HeldProductionSnapshot._check_deadline(pipeline_deadline)
        return result
    except BaseException:
        if decoded_values is not None:
            decoded_values.clear()
        if retained is not None and not retained.released:
            retained.release()
        if scratch is not None and not scratch.released:
            scratch.release()
        if transient is not None and not transient.released:
            transient.release()
        raise


def bounded_uninspected_configuration_digest(
    source_root: Path,
    compile_commands: tuple[Path, ...],
    launcher_environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    pipeline_deadline: float,
) -> str:
    """Hash process-free raw configuration inputs before compiler inspection."""

    if (
        not isinstance(source_root, Path)
        or not isinstance(compile_commands, tuple)
        or not compile_commands
        or any(not isinstance(path, Path) for path in compile_commands)
        or not isinstance(launcher_environment, Mapping)
        or not isinstance(dependency_roots, DependencyRootAuthority)
    ):
        raise AuditInfrastructureError(
            "uninspected configuration inputs are invalid"
        )
    digest = hashlib.sha256()

    def framed(payload: bytes) -> None:
        digest.update(struct.pack("<Q", len(payload)))
        digest.update(payload)

    framed(b"olr-gpu-uninspected-configuration-v1")
    framed(dependency_roots.portable_authority_digest.encode("ascii"))
    total = 0
    for requested in sorted(compile_commands, key=lambda path: str(path)):
        HeldProductionSnapshot._check_deadline(pipeline_deadline)
        database = _database_path(source_root, requested)
        try:
            size = database.stat().st_size
        except OSError as error:
            raise AuditInfrastructureError(
                "compile database is unavailable before inspection"
            ) from error
        if size < 0 or size > (64 << 20) - total:
            raise AuditInfrastructureError(
                "uninspected compile database limit exceeded"
            )
        total += size
        try:
            payload = database.read_bytes()
        except OSError as error:
            raise AuditInfrastructureError(
                "compile database is unavailable before inspection"
            ) from error
        if len(payload) != size:
            raise AuditInfrastructureError(
                "compile database changed during uninspected hashing"
            )
        framed(str(database).encode("utf-8"))
        framed(payload)
    environment_payload = json.dumps(
        sorted((str(key), str(value)) for key, value in launcher_environment.items()),
        ensure_ascii=True,
        separators=(",", ":"),
    ).encode("ascii")
    if len(environment_payload) > (4 << 20):
        raise AuditInfrastructureError(
            "uninspected launcher environment limit exceeded"
        )
    framed(environment_payload)
    return digest.hexdigest()


def _generation_cleanup_ceiling(
    reactor: GenerationReactor,
    *,
    fallback_deadline: float | None = None,
) -> float:
    configured = getattr(reactor, "cleanup_deadline", None)
    if configured is not None:
        return float(configured)
    runtime_contract = getattr(reactor, "runtime_contract", None)
    if runtime_contract is None:
        if fallback_deadline is None:
            raise AuditInfrastructureError(
                "worker cleanup ceiling is unavailable"
            )
        return float(fallback_deadline)
    return (
        float(runtime_contract.pipeline_deadline)
        + _FAILURE_REAP_SECONDS
    )


def emergency_cleanup_deadline(cleanup_ceiling: float) -> float:
    if (
        not isinstance(cleanup_ceiling, (int, float))
        or isinstance(cleanup_ceiling, bool)
        or not math.isfinite(float(cleanup_ceiling))
    ):
        raise AuditInfrastructureError(
            "emergency cleanup ceiling is invalid"
        )
    return min(
        time.monotonic() + _FAILURE_REAP_SECONDS,
        float(cleanup_ceiling),
    )


def _attempt_phase_seal_transition(
    _platform: str, _stage: str, _position: str, _phase: str
) -> None:
    return None


@dataclass(frozen=True)
class _LinuxCoordinatorProcessSnapshot:
    pid: int
    parent_pid: int
    start_identity: str
    cgroup_path: Path
    executable_path: Path
    argv: tuple[bytes, ...]


def _read_linux_coordinator_process_snapshot(
    pid: int,
) -> _LinuxCoordinatorProcessSnapshot:
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise AuditInfrastructureError(
            "Linux coordinator process identity is invalid"
        )
    proc = Path("/proc") / str(pid)
    try:
        stat_text = (proc / "stat").read_text(encoding="ascii")
        close = stat_text.rindex(")")
        fields = stat_text[close + 2 :].split()
        parent_pid = int(fields[1])
        start_ticks = fields[19]
        boot_id = Path("/proc/sys/kernel/random/boot_id").read_text(
            encoding="ascii"
        ).strip()
        cgroup_path = None
        for line in (proc / "cgroup").read_text(
            encoding="ascii"
        ).splitlines():
            hierarchy, controllers, relative = line.split(":", 2)
            if hierarchy == "0" and controllers == "":
                cgroup_path = (
                    Path("/sys/fs/cgroup") / relative.lstrip("/")
                ).resolve(strict=True)
                break
        executable_path = (proc / "exe").resolve(strict=True)
        argv = tuple((proc / "cmdline").read_bytes().split(b"\0"))
        if argv and argv[-1] == b"":
            argv = argv[:-1]
    except (OSError, ValueError, IndexError) as error:
        raise AuditInfrastructureError(
            "Linux coordinator process identity is unavailable"
        ) from error
    if (
        parent_pid <= 0
        or not boot_id
        or not start_ticks.isdigit()
        or cgroup_path is None
        or not argv
        or any(not value for value in argv)
    ):
        raise AuditInfrastructureError(
            "Linux coordinator process identity is invalid"
        )
    return _LinuxCoordinatorProcessSnapshot(
        pid,
        parent_pid,
        f"linux-proc:{boot_id}:{start_ticks}",
        cgroup_path,
        executable_path,
        argv,
    )


class _LinuxCoordinatorProcessCarrier:
    _RESOURCE_TRACKER_COMMAND = re.compile(
        br"from multiprocessing\.resource_tracker import main;"
        br"main\((?:0|[1-9][0-9]*)\)"
    )

    def __init__(self, pid: int, coordinator: Path) -> None:
        before = _read_linux_coordinator_process_snapshot(pid)
        self._validate_snapshot(before, coordinator)
        pidfd_open = getattr(os, "pidfd_open", None)
        if not callable(pidfd_open):
            raise AuditInfrastructureError(
                "Linux coordinator process pidfd authority is unavailable"
            )
        try:
            pidfd = pidfd_open(pid)
        except OSError as error:
            raise AuditInfrastructureError(
                "Linux coordinator process pidfd retention failed"
            ) from error
        try:
            after = _read_linux_coordinator_process_snapshot(pid)
            self._validate_snapshot(after, coordinator)
            if after != before:
                raise AuditInfrastructureError(
                    "Linux coordinator process identity changed during retention"
                )
            self.pid = pid
            self._snapshot = after
            self._pidfd: int | None = pidfd
        except BaseException as primary:
            try:
                os.close(pidfd)
            except BaseException as cleanup_error:
                primary.add_note(
                    "Linux coordinator pidfd cleanup failed: "
                    f"{cleanup_error}"
                )
            raise

    @classmethod
    def _validate_snapshot(cls, snapshot, coordinator: Path) -> None:
        from multiprocessing import util as multiprocessing_util

        expected_executable = Path(sys.executable).resolve(strict=True)
        argv = snapshot.argv
        interpreter_flags = tuple(
            os.fsencode(argument)
            for argument in multiprocessing_util._args_from_interpreter_flags()
        )
        command_index = 1 + len(interpreter_flags)
        try:
            invoked_executable = Path(os.fsdecode(argv[0])).resolve(strict=True)
        except (OSError, TypeError, ValueError) as error:
            raise AuditInfrastructureError(
                "Linux spawn resource tracker identity is invalid"
            ) from error
        if (
            snapshot.pid <= 0
            or snapshot.parent_pid != os.getpid()
            or snapshot.cgroup_path != coordinator.resolve(strict=True)
            or snapshot.executable_path != expected_executable
            or invoked_executable != expected_executable
            or len(argv) != command_index + 2
            or argv[1:command_index] != interpreter_flags
            or argv[command_index] != b"-c"
            or cls._RESOURCE_TRACKER_COMMAND.fullmatch(
                argv[command_index + 1]
            ) is None
        ):
            raise AuditInfrastructureError(
                "Linux spawn resource tracker identity is invalid"
            )

    def validate(self, coordinator: Path) -> None:
        if self._pidfd is None:
            raise AuditInfrastructureError(
                "Linux coordinator process carrier is closed"
            )
        try:
            os.fstat(self._pidfd)
        except OSError as error:
            raise AuditInfrastructureError(
                "Linux coordinator process pidfd identity is unavailable"
            ) from error
        current = _read_linux_coordinator_process_snapshot(self.pid)
        self._validate_snapshot(current, coordinator)
        if current != self._snapshot:
            raise AuditInfrastructureError(
                "Linux coordinator process identity changed"
            )

    def close(self) -> None:
        if self._pidfd is not None:
            pidfd = self._pidfd
            self._pidfd = None
            os.close(pidfd)


class LinuxCompilerAuditAttemptAccountant:
    """Account inspection and task phases in one delegated native cgroup run."""

    def __init__(self, rendezvous_client) -> None:
        required = (
            "coordinator_accounting_paths", "create_leaf",
            "acknowledge_leaf", "release_leaf",
        )
        if not all(callable(getattr(rendezvous_client, name, None))
                   for name in required):
            raise AuditInfrastructureError(
                "Linux attempt accountant authority is incomplete")
        coordinator, run, service_root = (
            rendezvous_client.coordinator_accounting_paths())
        if not all(isinstance(path, Path) for path in (
                coordinator, run, service_root)):
            raise AuditInfrastructureError(
                "Linux attempt accountant cgroup authority is invalid")
        self._client = rendezvous_client
        self._coordinator = coordinator
        self._run = run
        self._service_root = service_root
        self._baseline_run = self._read_events(run / "memory.events")
        self._baseline_service = self._read_events(
            service_root / "memory.events")
        self.inspection_probe_invocations = 0
        self._phase: str | None = None
        self._sealed: set[str] = set()
        self._sealed_snapshots: dict[str, LinuxPhaseSnapshot] = {}
        self._active_carriers: dict[ProcessStartIdentity, object] = {}
        self._pending_inspection: tuple[str, object] | None = None
        self._coordinator_process_carriers: dict[
            int, _LinuxCoordinatorProcessCarrier
        ] = {}

    @staticmethod
    def _read_events(path: Path) -> dict[str, int]:
        return _LinuxGenerationLifecycle._read_events(path)

    @staticmethod
    def _read_integer(path: Path) -> int:
        return _LinuxGenerationLifecycle._read_integer(path)

    def hello(self, deadline: float) -> None:
        hello = getattr(self._client, "hello", None)
        if not callable(hello):
            raise AuditInfrastructureError(
                "Linux rendezvous hello authority is unavailable")
        hello(deadline)

    def create_leaf(self, worker_index: int, generation: int, deadline: float):
        return self._client.create_leaf(worker_index, generation, deadline)

    def acknowledge_leaf(self, worker_index: int, generation: int,
                         worker_pid: int, carrier, deadline: float) -> None:
        self._client.acknowledge_leaf(
            worker_index, generation, worker_pid, carrier, deadline)

    def release_leaf(self, worker_index: int, generation: int,
                     deadline: float) -> None:
        self._client.release_leaf(worker_index, generation, deadline)

    def begin_phase(self, phase: str, deadline: float) -> None:
        expected = "inspection" if self._phase is None else "tasks"
        if (
            phase != expected
            or phase in self._sealed
            or phase in self._sealed_snapshots
            or not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or time.monotonic() >= deadline
            or (phase == "tasks" and "inspection" not in self._sealed)
        ):
            raise AuditInfrastructureError(
                "Linux attempt accountant phase is invalid")
        self._phase = phase

    def prepare_compiler_inspection_launch(self, capability,
                                           deadline: float) -> str:
        if (
            self._phase != "inspection"
            or self._pending_inspection is not None
            or time.monotonic() >= deadline
        ):
            raise AuditInfrastructureError(
                "Linux inspection launch preparation is invalid")
        token = secrets.token_hex(16)
        self._pending_inspection = (token, capability)
        return token

    def register_compiler_process_launch(self, event, carrier) -> None:
        if (
            self._phase != "inspection"
            or event.purpose is not CompilerLaunchPurpose.INSPECTION
            or event.process_start in self._active_carriers
            or self._pending_inspection is None
        ):
            raise AuditInfrastructureError(
                "Linux inspection launch registration is invalid")
        self._active_carriers[event.process_start] = carrier
        self.inspection_probe_invocations += 1

    def complete_compiler_process_launch(self, event, carrier) -> None:
        if self._active_carriers.pop(event.process_start, None) is not carrier:
            raise AuditInfrastructureError(
                "Linux inspection process carrier differs")

    def fail_compiler_process_launch(self, event, carrier) -> None:
        if self._active_carriers.get(event.process_start) is carrier:
            self._active_carriers.pop(event.process_start)

    def finish_compiler_inspection_launch_preparation(self, token: str) -> None:
        if (self._pending_inspection is None
                or self._pending_inspection[0] != token):
            raise AuditInfrastructureError(
                "Linux inspection launch preparation differs")
        self._pending_inspection = None

    def cancel_compiler_inspection_launch_preparation(self, token: str) -> None:
        if (self._pending_inspection is not None
                and self._pending_inspection[0] == token):
            self._pending_inspection = None

    def _retain_spawn_resource_tracker(self, *, required: bool) -> None:
        from multiprocessing import resource_tracker

        pid = getattr(resource_tracker._resource_tracker, "_pid", None)
        if pid is None and not required:
            return
        if (
            not isinstance(pid, int)
            or isinstance(pid, bool)
            or pid <= 0
        ):
            raise AuditInfrastructureError(
                "Linux spawn resource tracker registration is invalid"
            )
        existing = self._coordinator_process_carriers.get(pid)
        if existing is not None:
            if len(self._coordinator_process_carriers) != 1:
                raise AuditInfrastructureError(
                    "Linux spawn resource tracker registration differs"
                )
            existing.validate(self._coordinator)
            return
        if self._coordinator_process_carriers:
            raise AuditInfrastructureError(
                "Linux spawn resource tracker registration differs"
            )
        carrier = _LinuxCoordinatorProcessCarrier(pid, self._coordinator)
        try:
            self._coordinator_process_carriers[pid] = carrier
        except BaseException as primary:
            try:
                carrier.close()
            except BaseException as cleanup_error:
                primary.add_note(
                    "Linux coordinator carrier cleanup failed: "
                    f"{cleanup_error}"
                )
            raise

    def adopt_existing_spawn_resource_tracker(self) -> None:
        if (
            self._phase is not None
            or self._sealed
            or self._sealed_snapshots
            or self._coordinator_process_carriers
        ):
            raise AuditInfrastructureError(
                "Linux existing resource tracker adoption is invalid"
            )
        self._retain_spawn_resource_tracker(required=False)

    def register_spawn_resource_tracker(self) -> None:
        if (
            self._phase != "tasks"
            or "inspection" not in self._sealed
        ):
            raise AuditInfrastructureError(
                "Linux spawn resource tracker registration is invalid"
            )
        self._retain_spawn_resource_tracker(required=True)

    def close(self) -> None:
        carriers = getattr(self, "_coordinator_process_carriers", None)
        if carriers is None:
            return
        failures = []
        for carrier in tuple(carriers.values()):
            try:
                carrier.close()
            except BaseException as error:
                failures.append(error)
        carriers.clear()
        if failures:
            error = AuditInfrastructureError(
                "Linux coordinator process carrier cleanup failed"
            )
            for failure in failures:
                error.add_note(str(failure))
            raise error

    def _surviving_pids(self) -> set[int]:
        pids: set[int] = set()
        try:
            for path in self._run.rglob("cgroup.procs"):
                pids.update(int(value) for value in path.read_text(
                    encoding="ascii").split())
        except (OSError, ValueError) as error:
            raise AuditInfrastructureError(
                "Linux attempt cgroup membership is unavailable") from error
        pids.discard(os.getpid())
        for pid, carrier in self._coordinator_process_carriers.items():
            carrier.validate(self._coordinator)
            if pid not in pids:
                raise AuditInfrastructureError(
                    "Linux coordinator process carrier is unavailable"
                )
            pids.discard(pid)
        return pids

    def memory_measurements(self) -> LinuxRunMemoryMeasurements:
        run = self._read_events(self._run / "memory.events")
        service = self._read_events(self._service_root / "memory.events")

        def delta(current, baseline, name):
            value = current[name] - baseline[name]
            if value < 0:
                raise AuditInfrastructureError(
                    "Linux attempt cgroup event counter regressed")
            return value

        survivors = len(self._surviving_pids())
        values = LinuxRunMemoryMeasurements(
            self._read_integer(self._run / "memory.current"),
            self._read_integer(self._run / "memory.peak"),
            self._read_integer(self._run / "memory.max"),
            self._read_integer(self._run / "memory.high"),
            delta(run, self._baseline_run, "oom"),
            delta(run, self._baseline_run, "oom_kill"),
            delta(run, self._baseline_run, "max"),
            delta(service, self._baseline_service, "oom"),
            delta(service, self._baseline_service, "oom_kill"),
            delta(service, self._baseline_service, "max"),
            survivors,
            survivors == 0,
        )
        if any((
            values.cgroup_oom_count_delta,
            values.cgroup_oom_kill_count_delta,
            values.cgroup_max_event_count_delta,
            values.service_root_oom_count_delta,
            values.service_root_oom_kill_count_delta,
            values.service_root_max_event_count_delta,
        )):
            raise AuditInfrastructureError(
                "Linux attempt cgroup memory event increased")
        return values

    def seal_phase(self, phase: str, deadline: float):
        existing = self._sealed_snapshots.get(phase)
        if existing is None:
            if (
                phase != self._phase
                or phase in self._sealed
                or self._active_carriers
                or self._pending_inspection is not None
                or time.monotonic() >= deadline
            ):
                raise AuditInfrastructureError(
                    "Linux attempt accountant phase seal is invalid")
            memory = self.memory_measurements()
            if not memory.accounting_complete:
                raise AuditInfrastructureError(
                    "Linux attempt accountant phase has survivors")
            existing = LinuxPhaseSnapshot("linux", phase, memory, 0)
            _attempt_phase_seal_transition(
                "linux", "snapshot", "before", phase
            )
            self._sealed_snapshots[phase] = existing
            _attempt_phase_seal_transition(
                "linux", "snapshot", "after", phase
            )
        if phase not in self._sealed:
            _attempt_phase_seal_transition(
                "linux", "sealed", "before", phase
            )
            self._sealed.add(phase)
            _attempt_phase_seal_transition(
                "linux", "sealed", "after", phase
            )
        return existing

    def seal_phase_from_lifecycle_snapshot(
        self,
        phase: str,
        deadline: float,
        lifecycle_snapshot: LinuxPhaseSnapshot,
    ) -> LinuxPhaseSnapshot:
        existing = self._sealed_snapshots.get(phase)
        if existing is not None:
            if (
                type(lifecycle_snapshot) is not LinuxPhaseSnapshot
                or existing.memory != lifecycle_snapshot.memory
            ):
                raise AuditInfrastructureError(
                    "Linux lifecycle task phase snapshot differs"
                )
            if phase not in self._sealed:
                _attempt_phase_seal_transition(
                    "linux", "sealed", "before", phase
                )
                self._sealed.add(phase)
                _attempt_phase_seal_transition(
                    "linux", "sealed", "after", phase
                )
            return existing
        if (
            phase != "tasks"
            or phase != self._phase
            or phase in self._sealed
            or self._active_carriers
            or self._pending_inspection is not None
            or time.monotonic() >= deadline
            or type(lifecycle_snapshot) is not LinuxPhaseSnapshot
            or lifecycle_snapshot.platform_kind != "linux"
            or lifecycle_snapshot.phase != phase
            or type(lifecycle_snapshot.memory)
            is not LinuxRunMemoryMeasurements
            or not lifecycle_snapshot.memory.accounting_complete
            or lifecycle_snapshot.memory.surviving_cgroup_process_count != 0
        ):
            raise AuditInfrastructureError(
                "Linux lifecycle task phase snapshot differs"
            )
        if self._surviving_pids():
            raise AuditInfrastructureError(
                "Linux attempt accountant phase has survivors"
            )
        existing = LinuxPhaseSnapshot(
            "linux", phase, lifecycle_snapshot.memory, 0
        )
        _attempt_phase_seal_transition(
            "linux", "snapshot", "before", phase
        )
        self._sealed_snapshots[phase] = existing
        _attempt_phase_seal_transition(
            "linux", "snapshot", "after", phase
        )
        _attempt_phase_seal_transition(
            "linux", "sealed", "before", phase
        )
        self._sealed.add(phase)
        _attempt_phase_seal_transition(
            "linux", "sealed", "after", phase
        )
        return existing


def _is_unix_attempt_accountant(run_accountant) -> bool:
    from gpu_capability_process_tree import MacOSCompilerAuditAttemptAccountant

    return isinstance(
        run_accountant,
        (LinuxCompilerAuditAttemptAccountant, MacOSCompilerAuditAttemptAccountant),
    )


def _is_windows_native_accountant(run_accountant) -> bool:
    from gpu_capability_process_tree import WindowsNativeRunAccountant

    return isinstance(run_accountant, WindowsNativeRunAccountant)


def _seal_windows_accountant_phase(
    run_accountant,
    phase: str,
    deadline: float,
    archived_generation_identities: tuple[tuple[int, int], ...],
) -> WindowsPhaseSnapshot:
    from gpu_capability_process_tree import WindowsJobAccountingSnapshot

    if not _is_windows_native_accountant(run_accountant):
        raise AuditInfrastructureError("Windows run accountant differs")
    archived_generation_identities = _canonical_generation_identities(
        archived_generation_identities
    )
    journals = getattr(run_accountant, "_phase_finalization_journals", None)
    if journals is None:
        journals = {}
        run_accountant._phase_finalization_journals = journals
    journal = journals.setdefault(phase, {})
    if journal.get("expected_identities") not in {
        None, archived_generation_identities
    }:
        raise AuditInfrastructureError(
            f"Windows {phase} phase generation identities differ"
        )
    journal["expected_identities"] = archived_generation_identities
    if not journal.get("native_sealed", False):
        run_accountant.seal_phase(deadline, _current_process_rss_bytes())
        journal["native_sealed"] = True
    if "raw_snapshot" not in journal:
        captured = run_accountant.snapshot()
        if type(captured) is not WindowsJobAccountingSnapshot:
            raise AuditInfrastructureError(
                "Windows phase accounting snapshot differs"
            )
        journal["raw_snapshot"] = captured
    captured = journal["raw_snapshot"]
    if "memory" not in journal:
        memory = captured.memory_measurements()
        if type(memory) is not WindowsRunMemoryMeasurements:
            raise AuditInfrastructureError(
                "Windows phase memory accounting differs"
            )
        if (
            not memory.accounting_complete
            or memory.surviving_job_process_count != 0
        ):
            raise AuditInfrastructureError(
                f"Windows {phase} phase survivors remain"
            )
        _validate_windows_phase_generation_identities(
            captured.archived_generation_identities,
            archived_generation_identities,
            phase,
        )
        journal["memory"] = memory
    if "typed_snapshot" not in journal:
        journal["typed_snapshot"] = _construct_task_phase_snapshot(
            "windows",
            phase,
            journal["memory"],
            archived_generation_identities,
        )
    return journal["typed_snapshot"]


def _canonical_generation_identities(
    identities,
) -> tuple[tuple[int, int], ...]:
    if (
        not isinstance(identities, tuple)
        or any(
            not isinstance(identity, tuple)
            or len(identity) != 2
            or any(
                not isinstance(value, int)
                or isinstance(value, bool)
                or value < 0
                for value in identity
            )
            for identity in identities
        )
        or identities != tuple(sorted(set(identities)))
    ):
        raise AuditInfrastructureError(
            "archived generation identities are not canonical"
        )
    return identities


def _canonical_archived_generation_identities(
    archived_generations,
) -> tuple[tuple[int, int], ...]:
    identities = tuple(sorted(
        (worker_index, generation)
        for worker_index, generation, _resident in archived_generations
    ))
    return _canonical_generation_identities(identities)


def _validate_windows_phase_generation_identities(
    actual, expected, phase: str
) -> None:
    actual = _canonical_generation_identities(actual)
    expected = _canonical_generation_identities(expected)
    if (
        phase not in {"inspection", "tasks"}
        or (phase == "inspection" and (actual or expected))
        or (phase == "tasks" and actual != expected)
    ):
        raise AuditInfrastructureError(
            f"Windows {phase} phase generation identities differ"
        )


def _construct_task_phase_snapshot(
    platform_kind: str, phase: str, memory, archived_generations
):
    if platform_kind == "windows" and type(
        memory
    ) is WindowsRunMemoryMeasurements:
        return WindowsPhaseSnapshot(
            "windows",
            phase,
            memory,
            _canonical_generation_identities(archived_generations),
        )
    if platform_kind == "linux" and type(
        memory
    ) is LinuxRunMemoryMeasurements:
        return LinuxPhaseSnapshot(
            "linux", phase, memory, archived_generations
        )
    if platform_kind == "macos" and type(
        memory
    ) is MacOSRunMemoryMeasurements:
        return MacOSPhaseSnapshot(
            "macos", phase, memory, archived_generations
        )
    raise AuditInfrastructureError("native phase memory backend differs")


def _phase_snapshot(
    memory, platform_kind: str, phase: str, archived_generation_count: int
):
    if platform_kind == "linux" and type(memory) is LinuxRunMemoryMeasurements:
        return LinuxPhaseSnapshot(
            "linux", phase, memory, archived_generation_count
        )
    if platform_kind == "macos" and type(memory) is MacOSRunMemoryMeasurements:
        return MacOSPhaseSnapshot(
            "macos", phase, memory, archived_generation_count
        )
    raise AuditInfrastructureError("native phase memory backend differs")


def _accountant_memory(run_accountant):
    direct = getattr(run_accountant, "memory_measurements", None)
    if callable(direct):
        return direct()
    snapshot = getattr(run_accountant, "snapshot", None)
    if callable(snapshot):
        captured = snapshot()
        convert = getattr(captured, "memory_measurements", None)
        if callable(convert):
            return convert()
    raise AuditInfrastructureError("run accountant memory is unavailable")


def _validated_unix_phase_snapshot_memory(
    snapshot, platform_kind: str, phase: str, archived_generation_count
):
    if platform_kind == "linux":
        snapshot_type = LinuxPhaseSnapshot
        memory_type = LinuxRunMemoryMeasurements
    elif platform_kind == "macos":
        snapshot_type = MacOSPhaseSnapshot
        memory_type = MacOSRunMemoryMeasurements
    else:
        raise AuditInfrastructureError("Unix phase snapshot differs")
    count = getattr(snapshot, "archived_generation_count", None)
    if (
        type(snapshot) is not snapshot_type
        or snapshot.platform_kind != platform_kind
        or snapshot.phase != phase
        or type(snapshot.memory) is not memory_type
        or not isinstance(count, int)
        or isinstance(count, bool)
        or count < 0
        or (
            archived_generation_count is not None
            and count != archived_generation_count
        )
    ):
        raise AuditInfrastructureError("Unix phase snapshot differs")
    memory = snapshot.memory
    if type(memory) is LinuxRunMemoryMeasurements:
        complete = (
            memory.accounting_complete
            and memory.surviving_cgroup_process_count == 0
        )
    else:
        complete = (
            memory.accounting_complete
            and memory.surviving_registered_process_count == 0
            and memory.known_unreconciled_descendant_count == 0
        )
    if not complete:
        raise AuditInfrastructureError("Unix phase snapshot survivors remain")
    return memory


def _final_memory_from_sealed_phases(
    inspection_phase, task_phase, platform_kind: str
):
    if platform_kind in {"linux", "macos"}:
        _validated_unix_phase_snapshot_memory(
            inspection_phase, platform_kind, "inspection", 0
        )
        return _validated_unix_phase_snapshot_memory(
            task_phase, platform_kind, "tasks", None
        )
    if platform_kind == "windows":
        if (
            type(inspection_phase) is not WindowsPhaseSnapshot
            or inspection_phase.platform_kind != "windows"
            or inspection_phase.phase != "inspection"
            or inspection_phase.archived_generation_identities
            or type(task_phase) is not WindowsPhaseSnapshot
            or task_phase.platform_kind != "windows"
            or task_phase.phase != "tasks"
            or type(task_phase.memory) is not WindowsRunMemoryMeasurements
        ):
            raise AuditInfrastructureError("Windows phase snapshot differs")
        return task_phase.memory
    raise AuditInfrastructureError("native phase snapshot differs")


def _seal_accountant_phase(
    run_accountant, phase: str, pipeline_deadline: float, platform_kind: str
):
    if platform_kind == "windows" and _is_windows_native_accountant(
        run_accountant
    ):
        return _seal_windows_accountant_phase(
            run_accountant, phase, pipeline_deadline, ()
        )
    if platform_kind == "windows":
        return _construct_task_phase_snapshot(
            "windows",
            phase,
            _accountant_memory(run_accountant),
            (),
        )
    if _is_unix_attempt_accountant(run_accountant):
        result = run_accountant.seal_phase(phase, pipeline_deadline)
        expected_platform = (
            "linux"
            if isinstance(
                run_accountant, LinuxCompilerAuditAttemptAccountant
            )
            else "macos"
        )
        if platform_kind != expected_platform:
            raise AuditInfrastructureError("Unix phase snapshot differs")
        _validated_unix_phase_snapshot_memory(
            result, platform_kind, phase, 0
        )
        return result
    return _phase_snapshot(
        _accountant_memory(run_accountant), platform_kind, phase, 0
    )


def _begin_accountant_phase(
    run_accountant, phase: str, deadline: float, platform_kind: str
) -> None:
    if platform_kind == "windows" and _is_windows_native_accountant(
        run_accountant
    ):
        if (
            phase not in {"inspection", "tasks"}
            or not isinstance(deadline, (int, float))
            or isinstance(deadline, bool)
            or time.monotonic() >= deadline
        ):
            raise AuditInfrastructureError(
                "Windows run accountant phase is invalid"
            )
        if phase == "inspection":
            _construct_task_phase_snapshot(
                "windows",
                phase,
                _accountant_memory(run_accountant),
                (),
            )
        return
    begin = getattr(run_accountant, "begin_phase", None)
    if not callable(begin):
        raise AuditInfrastructureError(
            "run accountant phase authority is unavailable"
        )
    begin(phase, deadline)
    if phase == "inspection":
        memory = _accountant_memory(run_accountant)
        if platform_kind == "windows":
            _construct_task_phase_snapshot(
                "windows", phase, memory, ()
            )
        else:
            _phase_snapshot(memory, platform_kind, phase, 0)


class _BoundedPolicyWorkspace:
    """Private ledger and exact-key sink backed by one compact reservation."""

    __slots__ = (
        "capacity", "current", "peak", "finding_provenance", "_path_text",
        "_path_render_bytes", "_schema",
    )

    def __init__(
        self,
        capacity: int,
        path_render_bytes: int = AuditLimits().compact_result_path_bytes,
    ) -> None:
        from gpu_capability_source_audit import conservative_allocation_schema

        if (
            not isinstance(capacity, int)
            or isinstance(capacity, bool)
            or capacity < 0
            or not isinstance(path_render_bytes, int)
            or isinstance(path_render_bytes, bool)
            or path_render_bytes <= 0
        ):
            raise AuditInfrastructureError("policy workspace capacity is invalid")
        self.capacity = capacity
        self.current = 0
        self.peak = 0
        self.finding_provenance = {}
        self._path_text = {}
        self._path_render_bytes = path_render_bytes
        self._schema = conservative_allocation_schema()

    def _reserve(self, byte_count: int) -> None:
        if byte_count > self.capacity - self.current:
            raise AuditInfrastructureError(
                "policy workspace exceeds remaining compact capacity"
            )
        self.current += byte_count
        self.peak = max(self.peak, self.current)

    def _policy_scratch_charge(
        self, source_characters: int, *, source_only: bool
    ) -> int:
        per_character = self._schema.object_bound(8 if source_only else 0)
        return self._schema.checked_add(
            65536,
            self._schema.checked_multiply(source_characters, per_character),
        )

    def preflight_policy_sources(self, sources, source_only) -> None:
        maximum = 0
        for path, source in sources.items():
            raw = self._policy_scratch_charge(len(source), source_only=False)
            required = raw
            if path in source_only:
                required = self._schema.checked_add(
                    raw,
                    self._policy_scratch_charge(
                        len(source), source_only=True
                    ),
                )
            maximum = max(maximum, required)
        if maximum > self.capacity - self.current:
            raise AuditInfrastructureError(
                "policy workspace preflight exceeds remaining compact capacity"
            )

    def reserve_policy_scratch(self, source_characters: int, *, source_only: bool) -> int:
        charge = self._policy_scratch_charge(
            source_characters, source_only=source_only
        )
        self._reserve(charge)
        return charge

    def release_policy_scratch(self, charge: int) -> None:
        if not isinstance(charge, int) or charge < 0 or charge > self.current:
            raise AuditInfrastructureError("policy workspace scratch underflows")
        self.current -= charge

    def reserve_transient(self, byte_count: int) -> int:
        self._reserve(byte_count)
        return byte_count

    def release_transient(self, charge: int) -> None:
        self.release_policy_scratch(charge)

    def retain_path_text(self, path: PurePosixPath) -> str:
        if not isinstance(path, PurePosixPath):
            raise AuditInfrastructureError("policy path is invalid")
        maximum = self._schema.checked_add(
            self._schema.object_bound(4),
            self._schema.dict_bound(1),
            self._schema.string_bound(self._path_render_bytes),
            self._schema.bytes_bound(4 * self._path_render_bytes),
        )
        self._reserve(maximum)
        retained = False
        try:
            existing = self._path_text.get(path)
            if existing is not None:
                return existing
            text = path.as_posix()
            encoded = text.encode("utf-8")
            encoded_length = len(encoded)
            if (
                not text
                or len(text) > self._path_render_bytes
                or encoded_length > self._path_render_bytes
            ):
                raise AuditInfrastructureError("policy path exceeds render bound")
            self._path_text[path] = text
            actual = self._schema.checked_add(
                self._schema.dict_bound(1),
                self._schema.string_bound(len(text)),
            )
            encoded = None
            self.current -= maximum - actual
            retained = True
            return text
        finally:
            if not retained:
                self.release_transient(maximum)

    def path_text(self, path: PurePosixPath) -> str:
        lookup_charge = self.reserve_transient(self._schema.object_bound(2))
        try:
            try:
                return self._path_text[path]
            except KeyError as error:
                raise AuditInfrastructureError(
                    "policy path text was not retained"
                ) from error
        finally:
            self.release_transient(lookup_charge)

    def record(self, finding, provenance: str) -> None:
        from gpu_capability_source_audit import Finding

        if not isinstance(finding, Finding) or not isinstance(provenance, str):
            raise AuditInfrastructureError("policy finding sink input is invalid")
        try:
            path_text = self.path_text(finding.path)
        except AuditInfrastructureError:
            path_text = self.retain_path_text(finding.path)
        lookup_charge = self.reserve_transient(self._schema.object_bound(2))
        try:
            provenances = self.finding_provenance.get(finding)
        finally:
            self.release_transient(lookup_charge)
        if provenances is None:
            key_charge = self._schema.checked_add(
                self._schema.object_bound(8),
                self._schema.tuple_bound(4),
                self._schema.string_objects_bound(
                    self._schema.checked_add(
                        len(path_text),
                        len(finding.expression),
                        len(finding.reason),
                    ),
                    3,
                ),
                self._schema.dict_bound(1),
            )
            provenance_charge = self._schema.checked_add(
                self._schema.dict_bound(1),
                self._schema.string_bound(len(provenance)),
            )
            self._reserve(self._schema.checked_add(key_charge, provenance_charge))
            self.finding_provenance[finding] = {provenance}
        elif provenance not in provenances:
            provenance_charge = self._schema.checked_add(
                self._schema.object_bound(2),
                self._schema.string_bound(len(provenance)),
            )
            self._reserve(provenance_charge)
            provenances.add(provenance)

    def reserve_finding_construction(self, finding) -> int:
        path_text = self.path_text(finding.path)
        return self.reserve_transient(self._schema.checked_add(
            self._schema.object_bound(4),
            self._schema.string_objects_bound(
                self._schema.checked_add(
                    len(path_text),
                    len(finding.expression),
                    len(finding.reason),
                ),
                3,
            ),
        ))

    def sorted_findings(self):
        charge = self.reserve_transient(
            self._schema.checked_add(
                self._schema.checked_multiply(
                    2, self._schema.list_bound(len(self.finding_provenance))
                ),
                self._schema.checked_multiply(
                    len(self.finding_provenance), self._schema.tuple_bound(4)
                ),
            )
        )
        try:
            return sorted(
                self.finding_provenance,
                key=lambda item: (
                    self._path_text[item.path],
                    item.line,
                    item.expression,
                    item.reason,
                ),
            ), charge
        except BaseException:
            self.release_transient(charge)
            raise

    def encode_for_hash(self, value: str) -> bytes:
        charge = self.reserve_transient(
            self._schema.bytes_bound(4 * len(value))
        )
        try:
            return value.encode("utf-8"), charge
        except BaseException:
            self.release_transient(charge)
            raise

    def encode_path_for_hash(self, path: PurePosixPath) -> tuple[bytes, int]:
        return self.encode_for_hash(self.path_text(path))

    def line_text(self, line: int | None) -> tuple[str, int]:
        charge = self.reserve_transient(self._schema.string_bound(32))
        try:
            return str(line), charge
        except BaseException:
            self.release_transient(charge)
            raise

    def pack_hash_frame(self, value: int) -> tuple[bytes, int]:
        charge = self.reserve_transient(self._schema.bytes_bound(8))
        try:
            return struct.pack("<Q", value), charge
        except BaseException:
            self.release_transient(charge)
            raise

    def update_hash_value(self, canonical, value: str) -> None:
        encoded, encoded_charge = self.encode_for_hash(value)
        frame = None
        frame_charge = None
        try:
            frame, frame_charge = self.pack_hash_frame(len(encoded))
            canonical.update(frame)
            canonical.update(encoded)
        finally:
            frame = None
            if frame_charge is not None:
                self.release_transient(frame_charge)
            encoded = None
            self.release_transient(encoded_charge)

    def update_hash_path(self, canonical, path: PurePosixPath) -> None:
        encoded, encoded_charge = self.encode_path_for_hash(path)
        frame = None
        frame_charge = None
        try:
            frame, frame_charge = self.pack_hash_frame(len(encoded))
            canonical.update(frame)
            canonical.update(encoded)
        finally:
            frame = None
            if frame_charge is not None:
                self.release_transient(frame_charge)
            encoded = None
            self.release_transient(encoded_charge)

    def sorted_paths(self, paths) -> tuple[list[PurePosixPath], int]:
        charge = self.reserve_transient(self._schema.checked_multiply(
            2, self._schema.list_bound(len(paths))
        ))
        try:
            return sorted(paths, key=self._path_text.__getitem__), charge
        except BaseException:
            self.release_transient(charge)
            raise

    def clear(self) -> None:
        self.finding_provenance.clear()
        self._path_text.clear()
        self.current = 0


class BoundedCanonicalAuditContentSummaryBuilder:
    """Build final policy state only while its compact growth owner is live."""

    __slots__ = (
        "_limits", "_budget", "_growth", "_workspace",
        "_authoritative", "_source_only", "_released",
    )

    def __init__(
        self, limits: AuditLimits, budget: CompactResultMemoryBudget
    ) -> None:
        if not isinstance(limits, AuditLimits) or not isinstance(
            budget, CompactResultMemoryBudget
        ):
            raise AuditInfrastructureError("bounded summary builder is invalid")
        self._limits = limits
        self._budget = budget
        self._growth = None
        self._workspace = None
        self._authoritative = None
        self._source_only = None
        self._released = False

    def premeasure_policy_merge_working_state(
        self,
        source_text: Mapping[PurePosixPath, str],
        aggregate: StreamingAuditSummary,
        pipeline_deadline: float,
    ) -> int:
        from gpu_capability_source_audit import (
            conservative_allocation_schema,
            premeasure_streaming_policy_growth,
        )

        if self._growth is not None or not isinstance(
            aggregate, StreamingAuditSummary
        ):
            raise AuditInfrastructureError("bounded summary premeasure is invalid")
        HeldProductionSnapshot._check_deadline(pipeline_deadline)
        schema = conservative_allocation_schema()
        configuration_provenance_count = 0
        for item in aggregate.findings:
            configuration_provenance_count = schema.checked_add(
                configuration_provenance_count, len(item.configurations)
            )
        required = premeasure_streaming_policy_growth(
            source_text,
            len(aggregate.findings),
            configuration_provenance_count=configuration_provenance_count,
            authoritative_paths=tuple(aggregate.reached_production),
            path_render_bytes=self._limits.compact_result_path_bytes,
            pipeline_deadline=pipeline_deadline,
        )
        remaining = self._budget.remaining_bytes
        if required > remaining:
            raise AuditInfrastructureError(
                "policy premeasure exceeds remaining compact capacity"
            )
        self._growth = remaining
        return self._growth

    def _require_reserved_backing(
        self, ownership: object
    ) -> None:
        if (
            self._growth is None
            or not self._budget.owns(ownership)
            or ownership._pending_growth is not None
            or not ownership.ownerships
        ):
            raise AuditInfrastructureError(
                "bounded summary backing ownership is unavailable"
            )
        backing = ownership.ownerships[-1]
        if (
            backing.label != "final policy aggregate growth"
            or not backing.committed
            or backing.byte_count < self._growth
        ):
            raise AuditInfrastructureError(
                "bounded summary backing ownership differs"
            )

    def merge_findings_into_reserved_backing_state(
        self,
        source_text: Mapping[PurePosixPath, str],
        aggregate: StreamingAuditSummary,
        production: Mapping[PurePosixPath, FileIdentity],
        active: frozenset[PurePosixPath],
        ownership,
        pipeline_deadline: float,
    ) -> None:
        from gpu_capability_source_audit import (
            Finding,
            audit_raw_sources,
            audit_source_only,
        )

        self._require_reserved_backing(ownership)
        if self._workspace is not None or self._released:
            raise AuditInfrastructureError("bounded summary merge is unavailable")
        policy_sources = (
            source_text._values
            if isinstance(source_text, _ImmutableProductionMapping)
            else source_text
        )
        workspace = _BoundedPolicyWorkspace(
            self._growth, self._limits.compact_result_path_bytes
        )
        self._workspace = workspace
        workspace._reserve(workspace._schema.checked_add(
            workspace._schema.dict_bound(len(aggregate.reached_production)),
            workspace._schema.dict_bound(len(production)),
        ))
        authoritative = frozenset(aggregate.reached_production)
        if not authoritative.issubset(production):
            raise AuditInfrastructureError(
                "authoritative coverage references missing production"
            )
        if any(path not in authoritative for path in active):
            raise AuditInfrastructureError(
                "active source lacks authoritative compiler coverage"
            )
        source_only = frozenset(
            path for path in production if path not in authoritative
        )
        for path in production:
            workspace.retain_path_text(path)
        for item in aggregate.findings:
            finding_charge = workspace.reserve_finding_construction(
                item.finding
            )
            try:
                finding = Finding(
                    item.finding.path,
                    item.finding.line,
                    item.finding.expression,
                    item.finding.reason,
                )
                for digest in item.configurations:
                    workspace.record(finding, digest)
            finally:
                workspace.release_transient(finding_charge)
        workspace.preflight_policy_sources(policy_sources, source_only)
        audit_raw_sources(
            policy_sources,
            workspace=workspace,
            sink=workspace.record,
            provenance="raw-source",
            pipeline_deadline=pipeline_deadline,
        )
        ordered_source_only, source_sort_charge = workspace.sorted_paths(
            source_only
        )
        try:
            for path in ordered_source_only:
                HeldProductionSnapshot._check_deadline(pipeline_deadline)
                audit_source_only(
                    path,
                    policy_sources[path],
                    workspace=workspace,
                    sink=workspace.record,
                    provenance="source-only",
                    pipeline_deadline=pipeline_deadline,
                )
        finally:
            ordered_source_only.clear()
            workspace.release_transient(source_sort_charge)
        self._authoritative = authoritative
        self._source_only = source_only

    def build_bounded_digest_and_count_summary(
        self, aggregate: StreamingAuditSummary
    ) -> CanonicalAuditContentSummary:
        if (
            self._released
            or self._workspace is None
            or self._authoritative is None
            or self._source_only is None
        ):
            raise AuditInfrastructureError("bounded summary state is unavailable")
        workspace = self._workspace
        canonical_charge = workspace.reserve_transient(
            workspace._schema.object_bound(8)
        )
        canonical = hashlib.sha256()
        try:
            for value in aggregate.configurations:
                workspace.update_hash_value(canonical, value)
            sorted_findings, finding_sort_charge = workspace.sorted_findings()
            try:
                for finding in sorted_findings:
                    provenances = workspace.finding_provenance[finding]
                    provenance_sort_charge = workspace.reserve_transient(
                        workspace._schema.checked_multiply(
                            2, workspace._schema.list_bound(len(provenances))
                        )
                    )
                    ordered_provenances = None
                    try:
                        ordered_provenances = sorted(provenances)
                        workspace.update_hash_path(canonical, finding.path)
                        line, line_charge = workspace.line_text(finding.line)
                        try:
                            workspace.update_hash_value(canonical, line)
                        finally:
                            line = None
                            workspace.release_transient(line_charge)
                        workspace.update_hash_value(
                            canonical, finding.expression
                        )
                        workspace.update_hash_value(canonical, finding.reason)
                        for provenance in ordered_provenances:
                            workspace.update_hash_value(canonical, provenance)
                    finally:
                        if ordered_provenances is not None:
                            ordered_provenances.clear()
                        workspace.release_transient(provenance_sort_charge)
            finally:
                sorted_findings.clear()
                workspace.release_transient(finding_sort_charge)
            for paths, prefix in (
                (self._authoritative, b"A"),
                (self._source_only, b"S"),
            ):
                ordered_paths, path_sort_charge = workspace.sorted_paths(paths)
                try:
                    for path in ordered_paths:
                        canonical.update(prefix)
                        workspace.update_hash_path(canonical, path)
                finally:
                    ordered_paths.clear()
                    workspace.release_transient(path_sort_charge)
            configuration_sort_charge = workspace.reserve_transient(
                workspace._schema.checked_multiply(
                    2,
                    workspace._schema.list_bound(len(aggregate.configurations)),
                )
            )
            workspace._reserve(workspace._schema.checked_add(
                workspace._schema.tuple_bound(len(aggregate.configurations)),
                workspace._schema.string_bound(64),
                workspace._schema.object_bound(8),
            ))
            ordered_configurations = None
            try:
                ordered_configurations = tuple(sorted(aggregate.configurations))
                digest = canonical.hexdigest()
                return CanonicalAuditContentSummary(
                    digest,
                    ordered_configurations,
                    len(workspace.finding_provenance),
                    len(self._authoritative),
                    len(self._source_only),
                )
            finally:
                workspace.release_transient(configuration_sort_charge)
        finally:
            canonical = None
            workspace.release_transient(canonical_charge)

    def release_backing_state(self) -> None:
        if self._released:
            return
        if self._workspace is not None:
            self._workspace.clear()
        self._workspace = None
        self._authoritative = None
        self._source_only = None
        self._released = True


def _canonical_streaming_summary(
    aggregate: StreamingAuditSummary,
    builder: BoundedCanonicalAuditContentSummaryBuilder,
) -> CanonicalAuditContentSummary:
    return builder.build_bounded_digest_and_count_summary(aggregate)


def run_compiler_audit_pipeline(
    source_root: Path,
    compile_commands: tuple[Path, ...],
    launcher_environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    capability_registry,
    cache,
    inspection_cache,
    limits: AuditLimits,
    mode: str,
    decision_path: Path,
    operation_deadline: float,
    run_accountant,
    evidence_suballocator,
    expected_audit_engine_fingerprint: str | None = None,
) -> CompilerAuditRun:
    """Run the production audit without exposing full views or compact results."""

    return _run_compiler_audit_pipeline_core(
        source_root,
        compile_commands,
        launcher_environment,
        dependency_roots,
        capability_registry,
        cache,
        inspection_cache,
        limits,
        mode,
        decision_path,
        operation_deadline,
        run_accountant,
        evidence_suballocator,
        expected_audit_engine_fingerprint,
        smoke_runtime_contract=None,
        cleanup_deadline=None,
    )


def run_compiler_audit_smoke_pipeline(
    source_root: Path,
    compile_commands: tuple[Path, ...],
    launcher_environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    capability_registry,
    cache,
    inspection_cache,
    limits: AuditLimits,
    runtime_contract: WorkerRuntimeContract,
    operation_deadline: float,
    cleanup_deadline: float,
    run_accountant,
    expected_audit_engine_fingerprint: str | None = None,
) -> CompilerAuditRun:
    """Run one native configuration without decision or evidence authority."""

    if (
        not isinstance(runtime_contract, WorkerRuntimeContract)
        or runtime_contract.workers != 1
        or runtime_contract.maximum_tasks_per_worker != 1
        or time.monotonic() >= runtime_contract.pipeline_deadline
    ):
        raise AuditInfrastructureError(
            "compiler audit smoke runtime contract is invalid")
    return _run_compiler_audit_pipeline_core(
        source_root,
        compile_commands,
        launcher_environment,
        dependency_roots,
        capability_registry,
        cache,
        inspection_cache,
        limits,
        "cold",
        None,
        operation_deadline,
        run_accountant,
        None,
        expected_audit_engine_fingerprint,
        smoke_runtime_contract=runtime_contract,
        cleanup_deadline=cleanup_deadline,
    )


def _run_compiler_audit_pipeline_core(
    source_root: Path,
    compile_commands: tuple[Path, ...],
    launcher_environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    capability_registry,
    cache,
    inspection_cache,
    limits: AuditLimits,
    mode: str,
    decision_path: Path | None,
    operation_deadline: float,
    run_accountant,
    evidence_suballocator,
    expected_audit_engine_fingerprint: str | None,
    *,
    smoke_runtime_contract: WorkerRuntimeContract | None,
    cleanup_deadline: float | None,
) -> CompilerAuditRun:
    """Shared production orchestration with one explicit authority source."""

    pipeline_started_at = time.monotonic()

    from gpu_capability_calibration import (
        canonical_platform_kind,
        configuration_set_digest,
        effective_worker_capacity,
        finalize_platform_worker_decision,
        linux_calibration_envelope,
        macos_calibration_envelope,
        platform_worker_decision_key,
        prevalidate_platform_worker_decision,
        runtime_contract_from_platform_decision,
        windows_calibration_envelope,
    )
    from gpu_capability_source_audit import _attest_loaded_audit_engine

    smoke_only = smoke_runtime_contract is not None
    if (
        mode not in {"cold", "warm", "audit"}
        or (smoke_only and mode != "cold")
        or (smoke_only and decision_path is not None)
        or (smoke_only and evidence_suballocator is not None)
        or (not smoke_only and not isinstance(decision_path, Path))
        or not isinstance(limits, AuditLimits)
        or not isinstance(limits.total_seconds, (int, float))
        or isinstance(limits.total_seconds, bool)
        or not math.isfinite(float(limits.total_seconds))
        or limits.total_seconds <= 0.0
        or limits.total_seconds > _PIPELINE_HARD_SECONDS
        or not isinstance(operation_deadline, (int, float))
        or isinstance(operation_deadline, bool)
        or pipeline_started_at >= operation_deadline
        or time.monotonic() >= operation_deadline
    ):
        raise AuditInfrastructureError("compiler audit pipeline inputs are invalid")
    total_seconds = float(limits.total_seconds)
    effective_deadline = min(
        operation_deadline, pipeline_started_at + total_seconds
    )
    cleanup_ceiling = (
        effective_deadline if cleanup_deadline is None else cleanup_deadline
    )
    if (
        not isinstance(cleanup_ceiling, (int, float))
        or isinstance(cleanup_ceiling, bool)
        or not math.isfinite(float(cleanup_ceiling))
        or cleanup_ceiling < effective_deadline
        or (not smoke_only and cleanup_ceiling != effective_deadline)
    ):
        raise AuditInfrastructureError(
            "compiler audit cleanup deadline is invalid"
        )
    cleanup_ceiling = float(cleanup_ceiling)
    if time.monotonic() >= effective_deadline:
        raise AuditInfrastructureError("pipeline deadline expired")
    _validate_pipeline_cache_authorities(cache, inspection_cache)
    engine = _attest_loaded_audit_engine(expected_audit_engine_fingerprint)
    platform_kind = canonical_platform_kind(sys.platform)
    if smoke_only and platform_kind not in {"linux", "macos"}:
        raise AuditInfrastructureError(
            "compiler audit smoke requires Linux or macOS")
    if (
        platform_kind == "linux"
        and isinstance(
            run_accountant, LinuxCompilerAuditAttemptAccountant
        )
    ):
        run_accountant.adopt_existing_spawn_resource_tracker()
    _begin_accountant_phase(
        run_accountant, "inspection", effective_deadline, platform_kind
    )
    production = enumerate_production_identities(
        source_root, limits, effective_deadline
    )
    prepared_production = _prepare_production_table(production, limits)
    production_table = prepared_production.table
    session_needing_abort = None
    aggregate = None
    summary = None
    summary_builder = None
    source_text = None
    summary_reservation = None
    summary_committed = False
    try:
        if smoke_only:
            uninspected = None
            prevalidated = None
        else:
            uninspected = bounded_uninspected_configuration_digest(
                source_root,
                compile_commands,
                launcher_environment,
                dependency_roots,
                effective_deadline,
            )
            prevalidated = prevalidate_platform_worker_decision(
                decision_path,
                platform_kind,
                engine,
                uninspected,
                effective_deadline,
            )
        _production_allocation_event("collect")
        collection = collect_configurations_with_decision_records(
            source_root,
            compile_commands,
            launcher_environment,
            production_table,
            dependency_roots,
            capability_registry,
            inspection_cache,
            engine,
            limits,
            effective_deadline,
            run_accountant,
        )
        if (
            collection.dependency_root_authority is not dependency_roots
            or collection.capability_registry is not capability_registry
        ):
            raise AuditInfrastructureError(
                "configuration collection replaced caller authority"
            )
        inspection_phase = _seal_accountant_phase(
            run_accountant, "inspection", effective_deadline, platform_kind
        )
        ordered = _validated_orchestration_inputs(
            collection.configurations, production_table, cache, limits
        )
        if smoke_only and len(ordered) != 1:
            raise AuditInfrastructureError(
                "compiler audit smoke requires exactly one configuration")
        active, configured_sources = compute_active_sources(
            ordered, production_table
        )
        missing_commands = active - configured_sources
        if missing_commands:
            raise AuditInfrastructureError(
                "active source has no compile command: "
                + ", ".join(
                    path.as_posix() for path in sorted(missing_commands)
                )
            )
        if smoke_only:
            runtime_contract = WorkerRuntimeContract(
                smoke_runtime_contract.workers,
                smoke_runtime_contract.maximum_tasks_per_worker,
                smoke_runtime_contract.recycle_rss_bytes,
                min(smoke_runtime_contract.pipeline_deadline,
                    effective_deadline),
            )
            if time.monotonic() >= runtime_contract.pipeline_deadline:
                raise AuditInfrastructureError(
                    "compiler audit smoke deadline expired")
        else:
            configuration_digest = configuration_set_digest(
                ordered, collection.decision_records, dependency_roots
            )
            compiler_digests = {
                record.compiler_digest
                for record in collection.decision_records.values()
            }
            if len(compiler_digests) != 1:
                raise AuditInfrastructureError(
                    "configuration decision compilers differ"
                )
            envelope = {
                "windows": windows_calibration_envelope,
                "linux": linux_calibration_envelope,
                "macos": macos_calibration_envelope,
            }[platform_kind]()
            expected_key = platform_worker_decision_key(
                platform_kind,
                next(iter(compiler_digests)),
                configuration_digest,
                uninspected,
                engine,
                effective_worker_capacity(limits),
                envelope,
            )
            decision = finalize_platform_worker_decision(
                prevalidated, expected_key, effective_deadline
            )
            runtime_contract = runtime_contract_from_platform_decision(
                decision, effective_deadline
            )
        _production_allocation_event("snapshot")
        with snapshot_production_sources(
            production_table,
            limits,
            effective_deadline,
            prepared_table=prepared_production,
        ) as held_sources:
            _begin_accountant_phase(
                run_accountant, "tasks", effective_deadline, platform_kind
            )
            compact_observer = CompactAccountingObserver()
            session_needing_abort = schedule_configuration_audits(
                source_root,
                ordered,
                dependency_roots,
                capability_registry,
                held_sources.initial_digest_map,
                cache,
                limits,
                engine,
                runtime_contract,
                inspection_probe_invocations=(
                    collection.inspection_probe_invocations
                ),
                run_accountant=run_accountant,
                compact_observer=compact_observer,
                cleanup_deadline=cleanup_ceiling,
            )
            aggregate = session_needing_abort.consume_aggregate()
            if (
                aggregate.budget_ownership.budget
                is not session_needing_abort.compact_result_budget
                or session_needing_abort.pending_result_reference_count != 0
            ):
                raise AuditInfrastructureError(
                    "scheduled aggregate ownership differs"
                )
            sources = held_sources.finalize_policy_boundary(effective_deadline)
            try:
                source_text = decode_validated_production_sources(
                    sources, limits, effective_deadline
                )
            finally:
                held_sources.release_finalized_sources(sources)
                sources = None
            result_budget = session_needing_abort.compact_result_budget
            summary_builder = BoundedCanonicalAuditContentSummaryBuilder(
                limits, result_budget
            )
            growth = summary_builder.premeasure_policy_merge_working_state(
                source_text._values,
                aggregate,
                effective_deadline,
            )
            result_budget.reserve_aggregate_growth(
                aggregate.budget_ownership, growth
            )
            result_budget.commit_aggregate_growth(
                aggregate.budget_ownership
            )
            summary_builder.merge_findings_into_reserved_backing_state(
                source_text,
                aggregate,
                production_table,
                active,
                aggregate.budget_ownership,
                effective_deadline,
            )
            source_text.close()
            source_text = None
            if not smoke_only:
                reserve_summary = getattr(
                    evidence_suballocator, "reserve_summary", None)
                commit_summary = getattr(
                    evidence_suballocator, "commit_summary", None)
                if not callable(reserve_summary) or not callable(commit_summary):
                    raise AuditInfrastructureError(
                        "evidence summary suballocator is invalid"
                    )
                if getattr(
                        evidence_suballocator, "maximum_bytes", 524288
                ) != 524288:
                    raise AuditInfrastructureError(
                        "evidence summary suballocator bound differs"
                    )
                cancel_summary = getattr(
                    evidence_suballocator, "cancel_summary", None
                )
                if not callable(cancel_summary):
                    raise AuditInfrastructureError(
                        "evidence summary cancellation is invalid"
                    )
                summary_reservation = reserve_summary(
                    len(aggregate.configurations), effective_deadline
                )
            result_budget.prevalidate_aggregate_release(
                aggregate.budget_ownership
            )
            summary = _canonical_streaming_summary(
                aggregate,
                summary_builder,
            )
            if not smoke_only:
                commit_summary(summary_reservation, summary)
                summary_committed = True
                summary_reservation = None
            summary_builder.release_backing_state()
            summary_builder = None
            result_budget.release_aggregate_after_summary(
                aggregate.budget_ownership
            )
            aggregate = None
            session_needing_abort.shutdown_reap(effective_deadline)
            completed_session = session_needing_abort
            session_needing_abort = None
        completed_reactor = getattr(completed_session, "reactor", None)
        task_phase = getattr(
            completed_reactor, "task_phase_snapshot", None
        )
        if completed_reactor is None:
            task_phase = _seal_accountant_phase(
                run_accountant, "tasks", effective_deadline, platform_kind
            )
        elif not isinstance(
            task_phase,
            (WindowsPhaseSnapshot, LinuxPhaseSnapshot, MacOSPhaseSnapshot),
        ):
            raise AuditInfrastructureError(
                "scheduled task phase snapshot is unavailable"
            )
        cache_bytes, cache_entries = cache.measure(effective_deadline)
        memory = _final_memory_from_sealed_phases(
            inspection_phase, task_phase, platform_kind
        )
        elapsed = time.monotonic() - pipeline_started_at
        if elapsed >= total_seconds or time.monotonic() >= effective_deadline:
            raise AuditInfrastructureError("pipeline deadline expired")
        measurements = StreamingAuditMeasurements(
            pipeline_started_at,
            elapsed,
            len(ordered),
            completed_session.cache_hits,
            completed_session.cache_misses,
            completed_session.inspection_probe_invocations,
            completed_session.audit_compiler_invocations,
            completed_session.stdout_bytes,
            memory,
            cache_bytes,
            cache_entries,
            completed_session.result_budget.peak_live_bytes,
            completed_session.result_budget.retained_bytes,
            completed_session.maximum_encoded_result_bytes,
            completed_session.maximum_conservative_decoded_bytes,
            completed_session.maximum_conservative_retained_bytes,
            runtime_contract,
            completed_session.cache_root,
            completed_session.worker_counts_started,
            (
                "enumerate-production",
                "collect",
                "snapshot",
                "cache",
                "schedule",
                "finalize",
                "decode",
                "coverage",
                "raw",
                "source-only",
                "shutdown-reap",
            ),
            inspection_phase,
            task_phase,
        )
        return CompilerAuditRun(summary, measurements)
    except BaseException as primary_error:
        try:
            prepared_production.close()
        except BaseException as cleanup_error:
            primary_error.add_note(
                f"production table cleanup also failed: {cleanup_error!r}"
            )
        if source_text is not None:
            try:
                source_text.close()
                source_text = None
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"decoded source cleanup also failed: {cleanup_error!r}"
                )
        if summary_builder is not None:
            try:
                summary_builder.release_backing_state()
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"summary builder cleanup also failed: {cleanup_error!r}"
                )
        if summary_reservation is not None:
            cancel_summary = getattr(
                evidence_suballocator, "cancel_summary", None
            )
            if callable(cancel_summary):
                try:
                    cancel_summary(summary_reservation)
                    summary_reservation = None
                except BaseException as cleanup_error:
                    primary_error.add_note(
                        "summary reservation cancellation also failed: "
                        f"{cleanup_error!r}"
                    )
        if aggregate is not None and not aggregate.budget_ownership.released:
            try:
                aggregate.release()
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"aggregate ownership release also failed: {cleanup_error!r}"
                )
        if summary is not None and summary_committed:
            release_summary = getattr(
                evidence_suballocator, "release_summary", None
            )
            if callable(release_summary):
                try:
                    release_summary(summary)
                except BaseException as cleanup_error:
                    primary_error.add_note(
                        f"summary ownership release also failed: {cleanup_error!r}"
                    )
        if session_needing_abort is not None:
            try:
                session_needing_abort.abort_and_reap(
                    emergency_cleanup_deadline(cleanup_ceiling)
                )
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"emergency abort/reap also failed: {cleanup_error!r}"
                )
        raise

# Retain-all equivalence orchestration is test-only; see the reference fixture.
