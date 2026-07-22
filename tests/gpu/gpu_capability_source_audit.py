#!/usr/bin/env python3
"""Audit production GPU capability use without widening the public API."""

from __future__ import annotations

import argparse
from array import array
import bisect
from collections.abc import Mapping
import contextvars
import dataclasses
from dataclasses import dataclass
import enum
import hashlib
import hmac
import math
import os
from pathlib import Path, PurePosixPath
import re
import statistics
import struct
import sys
import time
import types
from types import MappingProxyType
from typing import Callable, Iterable

import gpu_capability_model as _gpu_capability_model
import gpu_capability_command as _gpu_capability_command
import gpu_capability_cache as _gpu_capability_cache
import gpu_capability_provenance as _gpu_capability_provenance
import gpu_capability_runner as _gpu_capability_runner
import gpu_capability_process_tree as _gpu_capability_process_tree
import gpu_capability_calibration as _gpu_capability_calibration
from gpu_capability_model import (
    AUDIT_RESULT_SCHEMA_BYTES,
    AuditInfrastructureError,
    AuditLimits,
    CompilerFamily,
    CoverageReport,
    PreprocessedTranslationUnitView,
    SourceLocation,
    build_dependency_root_authority,
    enumerate_production_identities,
    _current_process_rss_bytes,
)


SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm"})
PRODUCTION_ROOTS = ("playback", "recorder_engine")
LEASE_HEADER = PurePosixPath("playback/gpu/gpusurfacelease.h")
REGISTRY_HEADER = PurePosixPath("playback/gpu/gpuretireregistry.h")
OP_SCOPE_HEADER = PurePosixPath("playback/gpu/gpuopscope.h")

# Public read() is retained by the locked design but confined to reviewed,
# synchronous compatibility adapters. Its deliberately narrow grammar requires
# one local scope, one local lease, one top-level lexical body, and a standalone
# completion with no earlier control transfer. New or broader production syntax
# must use withRead(), whose callback is tied to the exact second call argument.
SYNC_READ_ALLOWLIST = frozenset({PurePosixPath("playback/gpu/gpufence.h")})

SURFACE_INTERNAL_HANDLE_PATHS = frozenset({
    PurePosixPath("playback/gpu/gpusurface.h"),
    PurePosixPath("playback/gpu/applegpusurface_apple.mm"),
    PurePosixPath("playback/output/win/d3d11gpusurface.h"),
})

REVIEWED_NATIVE_HANDLE_SINKS = MappingProxyType({
    PurePosixPath("playback/gpu/gpufence.h"): frozenset({"isCompatibleWithNativeHandle"}),
    PurePosixPath("playback/gpu/applegpusurface_apple.mm"): frozenset({
        "CVPixelBufferCreateWithIOSurface", "IOSurfaceGetHeight", "IOSurfaceGetWidth",
    }),
    PurePosixPath("recorder_engine/codec/nativevideoencoder_videotoolbox.mm"): frozenset({
        "CVPixelBufferCreateWithIOSurface", "IOSurfaceGetHeight", "IOSurfaceGetWidth",
    }),
    PurePosixPath("recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"): frozenset({
        "MFCreateDXGISurfaceBuffer",
    }),
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"): frozenset({
        "CopySubresourceRegion",
    }),
})

REVIEWED_NATIVE_HANDLE_METHODS = MappingProxyType({
    PurePosixPath("recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"):
        frozenset({"GetDevice"}),
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"):
        frozenset({"GetDesc", "GetDevice"}),
})

REVIEWED_NATIVE_HANDLE_MEMBER_SINKS = MappingProxyType({
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"):
        frozenset({"CopySubresourceRegion"}),
})

REVIEWED_NATIVE_HANDLE_TYPES = MappingProxyType({
    PurePosixPath("playback/gpu/applegpusurface_apple.mm"):
        frozenset({"IOSurfaceRef"}),
    PurePosixPath("recorder_engine/codec/nativevideoencoder_videotoolbox.mm"):
        frozenset({"IOSurfaceRef"}),
    PurePosixPath("recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"):
        frozenset({"ID3D11Texture2D"}),
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"):
        frozenset({"ID3D11Texture2D"}),
})

_AUDIT_SPELLING_ALTERNATIVES = MappingProxyType({
    b"%:%:": b"##  ",
    b"<:": b"[ ",
    b":>": b"] ",
    b"<%": b"{ ",
    b"%>": b"} ",
    b"%:": b"# ",
})
_DEFAULT_CANDIDATE_PATHS = b"derived-capability-paths"


@dataclass(frozen=True)
class Finding:
    path: PurePosixPath
    line: int
    expression: str
    reason: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: forbidden expression {self.expression}: {self.reason}"


@dataclass(frozen=True)
class AggregatedFinding:
    finding: Finding
    configurations: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class ConservativeAllocationSchema:
    """Portable closed-form upper bounds; never a private CPython layout model."""

    schema_version: int = 1
    allocator_alignment_bytes: int = 16
    maximum_allocation_bytes: int = (1 << 63) - 1
    pointer_bytes_upper_bound: int = 16
    pylong_header_bytes: int = 64
    pylong_digit_bits: int = 15
    pylong_digit_bytes_upper_bound: int = 4
    bytes_header_bytes: int = 128
    bytearray_header_bytes: int = 128
    bytesio_header_bytes: int = 256
    str_header_bytes: int = 128
    list_header_bytes: int = 128
    tuple_header_bytes: int = 128
    dict_header_bytes: int = 256
    dict_index_bytes_upper_bound: int = 8
    dict_entry_slot_count: int = 3
    dict_maximum_load_numerator: int = 2
    dict_maximum_load_denominator: int = 3
    array_header_bytes: int = 128
    object_header_bytes: int = 256
    regex_match_header_bytes: int = 512
    json_scanner_fixed_bytes: int = 4096
    json_decoder_fixed_bytes: int = 4096
    json_encoder_scratch_bytes: int = 4096

    def _nonnegative(self, value: int) -> int:
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise AuditInfrastructureError("allocation arithmetic input is invalid")
        return value

    def checked_add(self, *values: int) -> int:
        total = 0
        for value in values:
            value = self._nonnegative(value)
            if value > self.maximum_allocation_bytes - total:
                raise AuditInfrastructureError("allocation arithmetic addition overflow")
            total += value
        return total

    def checked_multiply(self, left: int, right: int) -> int:
        left = self._nonnegative(left)
        right = self._nonnegative(right)
        if left and right > self.maximum_allocation_bytes // left:
            raise AuditInfrastructureError("allocation arithmetic multiplication overflow")
        return left * right

    def round_up(self, raw_bytes: int) -> int:
        raw_bytes = self._nonnegative(raw_bytes)
        adjusted = self.checked_add(raw_bytes, self.allocator_alignment_bytes - 1)
        return adjusted - adjusted % self.allocator_alignment_bytes

    def pylong_bound(self, value: int) -> int:
        if not isinstance(value, int) or isinstance(value, bool):
            raise AuditInfrastructureError("allocation arithmetic integer is invalid")
        digits = max(
            1,
            (abs(value).bit_length() + self.pylong_digit_bits - 1)
            // self.pylong_digit_bits,
        )
        return self.round_up(self.checked_add(
            self.pylong_header_bytes,
            self.checked_multiply(digits, self.pylong_digit_bytes_upper_bound),
        ))

    def bytes_bound(self, length: int) -> int:
        return self.round_up(self.checked_add(
            self.bytes_header_bytes, self.checked_add(length, 1)
        ))

    def bytearray_bound(self, length: int) -> int:
        return self.round_up(self.checked_add(
            self.bytearray_header_bytes, self.checked_add(length, 1)
        ))

    def bytesio_bound(self, backing_capacity: int) -> int:
        capacity = self.round_up(backing_capacity)
        return self.round_up(self.checked_add(self.bytesio_header_bytes, capacity))

    def string_bound(self, code_points: int) -> int:
        storage = self.checked_multiply(self.checked_add(code_points, 1), 4)
        return self.round_up(self.checked_add(self.str_header_bytes, storage))

    def list_bound(self, slots: int) -> int:
        return self.round_up(self.checked_add(
            self.list_header_bytes,
            self.checked_multiply(slots, self.pointer_bytes_upper_bound),
        ))

    def tuple_bound(self, slots: int) -> int:
        return self.round_up(self.checked_add(
            self.tuple_header_bytes,
            self.checked_multiply(slots, self.pointer_bytes_upper_bound),
        ))

    def dict_capacity(self, entries: int) -> int:
        entries = self._nonnegative(entries)
        if entries == 0:
            return 0
        required = (
            self.checked_add(
                self.checked_multiply(entries, self.dict_maximum_load_denominator),
                self.dict_maximum_load_numerator - 1,
            )
            // self.dict_maximum_load_numerator
        )
        capacity = 1
        while capacity < required:
            capacity = self.checked_multiply(capacity, 2)
        return capacity

    def dict_bound(self, entries: int) -> int:
        capacity = self.dict_capacity(entries)
        indices = self.checked_multiply(capacity, self.dict_index_bytes_upper_bound)
        entry_bytes = self.checked_multiply(
            capacity,
            self.checked_multiply(
                self.dict_entry_slot_count, self.pointer_bytes_upper_bound
            ),
        )
        return self.round_up(self.checked_add(
            self.dict_header_bytes, indices, entry_bytes
        ))

    def array_bound(self, items: int, item_bytes: int = 4) -> int:
        return self.round_up(self.checked_add(
            self.array_header_bytes, self.checked_multiply(items, item_bytes)
        ))

    def object_bound(self, pointer_slots: int) -> int:
        return self.round_up(self.checked_add(
            self.object_header_bytes,
            self.checked_multiply(pointer_slots, self.pointer_bytes_upper_bound),
        ))

    def bytes_objects_bound(self, total_length: int, object_count: int) -> int:
        """Bound separately allocated bytes objects, including every header/slack."""

        return self.checked_add(
            self.checked_multiply(object_count, self.bytes_header_bytes),
            total_length,
            object_count,
            self.checked_multiply(object_count, self.allocator_alignment_bytes - 1),
        )

    def string_objects_bound(self, total_code_points: int, object_count: int) -> int:
        """Bound worst-kind strings without pretending an aggregate has one header."""

        storage = self.checked_multiply(
            self.checked_add(total_code_points, object_count), 4
        )
        return self.checked_add(
            self.checked_multiply(object_count, self.str_header_bytes),
            storage,
            self.checked_multiply(object_count, self.allocator_alignment_bytes - 1),
        )


_CONSERVATIVE_ALLOCATION_SCHEMA = ConservativeAllocationSchema()


def conservative_allocation_schema() -> ConservativeAllocationSchema:
    return _CONSERVATIVE_ALLOCATION_SCHEMA


def audit_measurement_scratch_bound(
    schema: ConservativeAllocationSchema | None = None,
) -> int:
    """Bound the fixed live workspace used before the scalable plan exists."""

    if schema is None:
        return _AUDIT_MEASUREMENT_SCRATCH_BYTES
    return schema.checked_add(
        # Measurement frame, range iterator, fixed helper-call frames and refs.
        schema.checked_multiply(4, schema.object_bound(40)),
        # Simultaneously live packed indices/count results and arithmetic values.
        schema.checked_multiply(
            24, schema.pylong_bound(_gpu_capability_model.UINT32_MAX)
        ),
        # The two frozen result records and their fixed field-reference storage.
        schema.object_bound(2),
        schema.object_bound(len(AuditAllocationShape.__dataclass_fields__)),
        schema.tuple_bound(16),
    )


@dataclass(frozen=True, slots=True)
class AuditAllocationShape:
    text_character_count: int
    maximum_code_point: int
    run_count: int
    origin_count: int
    chunk_count: int
    token_count: int
    json_input_chunk_bytes: int = 0
    json_input_chunk_count: int = 0
    json_token_count: int = 0
    json_string_characters: int = 0
    json_integer_values: tuple[int, ...] = ()
    json_list_slots: int = 0
    json_tuple_slots: int = 0
    json_dict_entries: int = 0
    duplicate_key_count: int = 0
    production_raw_bytes: int = 0
    analysis_required: bool = True
    scope_analysis_required: bool = True
    token_value_characters: int | None = None
    nonspace_character_count: int | None = None
    delimiter_pair_count: int | None = None
    member_call_count: int | None = None
    scope_declaration_count: int | None = None


_AUDIT_MEASUREMENT_SCRATCH_BYTES = audit_measurement_scratch_bound(
    _CONSERVATIVE_ALLOCATION_SCHEMA
)


@dataclass(frozen=True, slots=True)
class AuditAllocationPlan:
    measurement_scratch: int
    input_chunks_and_containers: int
    joined_input_and_join_transient: int
    final_text: int
    mapping_arrays: int
    duplicate_key_structures: int
    analysis_objects: int
    json_scanner_decoder: int
    production_decode_mapping: int
    pre_reserved_bytes: int

    @property
    def phase_charges(self) -> tuple[int, ...]:
        return (
            self.measurement_scratch,
            self.input_chunks_and_containers,
            self.joined_input_and_join_transient,
            self.final_text,
            self.mapping_arrays,
            self.duplicate_key_structures,
            self.analysis_objects,
            self.json_scanner_decoder,
            self.production_decode_mapping,
        )


def audit_allocation_plan(
    shape: AuditAllocationShape,
    schema: ConservativeAllocationSchema | None = None,
) -> AuditAllocationPlan:
    schema = conservative_allocation_schema() if schema is None else schema
    if not isinstance(shape, AuditAllocationShape):
        raise AuditInfrastructureError("audit allocation shape is invalid")
    complete_chunks, final_chunk_characters = divmod(
        shape.text_character_count, AuditBuffer._BLOCK_BYTES
    )
    if final_chunk_characters == 0 and complete_chunks:
        final_chunk_characters = AuditBuffer._BLOCK_BYTES
        complete_chunks -= 1
    audit_chunk_objects = schema.checked_add(
        schema.checked_multiply(
            complete_chunks,
            schema.checked_add(
                schema.bytearray_bound(AuditBuffer._BLOCK_BYTES),
                schema.string_bound(AuditBuffer._BLOCK_BYTES),
            ),
        ),
        schema.bytearray_bound(final_chunk_characters),
        schema.string_bound(final_chunk_characters),
    )
    input_chunks = schema.checked_add(
        schema.list_bound(shape.chunk_count),
        schema.list_bound(shape.json_input_chunk_count),
        audit_chunk_objects,
        schema.bytes_objects_bound(
            shape.json_input_chunk_bytes, shape.json_input_chunk_count
        ),
        # One block-sized spelling slice can overlap the destination chunk.
        schema.bytes_bound(min(shape.text_character_count, AuditBuffer._BLOCK_BYTES)),
    )
    joined_input = schema.checked_add(
        schema.string_bound(shape.text_character_count),
        schema.string_bound(shape.text_character_count),
        schema.bytes_bound(shape.json_input_chunk_bytes),
        schema.bytes_bound(shape.json_input_chunk_bytes),
    )
    final_text = schema.string_bound(shape.text_character_count)
    mapping_arrays = schema.checked_add(
        schema.tuple_bound(AuditBuffer._RUN_COLUMN_COUNT),
        *(
            schema.array_bound(shape.run_count + (1 if index == 9 else 0))
            for index in range(AuditBuffer._RUN_COLUMN_COUNT)
        ),
        # The mutable flags and immutable copy coexist until construction returns.
        schema.bytearray_bound(shape.origin_count),
        schema.bytes_bound(shape.origin_count),
        schema.dict_bound(shape.origin_count),
        schema.checked_multiply(shape.origin_count, schema.tuple_bound(3)),
        schema.checked_multiply(
            shape.origin_count, schema.pylong_bound(shape.origin_count)
        ),
        # A lookup key is built before it is known whether it will be retained.
        schema.tuple_bound(3),
    )
    duplicate_keys = schema.checked_add(
        schema.list_bound(schema.checked_multiply(shape.duplicate_key_count, 2)),
        schema.dict_bound(shape.duplicate_key_count),
        schema.string_bound(shape.json_string_characters),
    )
    token_characters = (
        shape.text_character_count
        if shape.token_value_characters is None
        else shape.token_value_characters
    )
    nonspace_characters = (
        shape.text_character_count
        if shape.nonspace_character_count is None
        else shape.nonspace_character_count
    )
    delimiter_pairs = (
        shape.token_count // 2
        if shape.delimiter_pair_count is None
        else shape.delimiter_pair_count
    )
    member_calls = (
        shape.token_count
        if shape.member_call_count is None
        else shape.member_call_count
    )
    scope_declarations = (
        shape.token_count
        if shape.scope_declaration_count is None
        else shape.scope_declaration_count
    )
    position_integer = schema.pylong_bound(shape.text_character_count)
    token_objects = schema.checked_add(
        schema.list_bound(shape.token_count),
        schema.tuple_bound(shape.token_count),
        schema.checked_multiply(shape.token_count, schema.object_bound(3)),
        schema.string_objects_bound(token_characters, shape.token_count),
        schema.checked_multiply(
            schema.checked_multiply(shape.token_count, 2), position_integer
        ),
        schema.tuple_bound(shape.token_count),
    )
    delimiter_objects = schema.checked_add(
        schema.list_bound(delimiter_pairs),
        schema.list_bound(delimiter_pairs),
        schema.tuple_bound(delimiter_pairs),
        schema.checked_multiply(delimiter_pairs, schema.tuple_bound(2)),
        schema.checked_multiply(
            schema.checked_multiply(delimiter_pairs, 2), position_integer
        ),
    )
    member_call_objects = schema.checked_add(
        schema.tuple_bound(member_calls),
        schema.checked_multiply(member_calls, schema.regex_match_header_bytes),
    )
    scope_objects = 0
    if shape.scope_analysis_required:
        next_nonspace_count = schema.checked_add(shape.text_character_count, 1)
        next_nonspace_objects = schema.checked_add(
            schema.list_bound(next_nonspace_count),
            schema.tuple_bound(next_nonspace_count),
            schema.checked_multiply(nonspace_characters, position_integer),
            position_integer,
        )
        declaration_objects = schema.checked_add(
            schema.list_bound(scope_declarations),
            schema.list_bound(scope_declarations),
            schema.dict_bound(scope_declarations),
            schema.checked_multiply(
                scope_declarations, schema.object_bound(4)
            ),
            schema.string_objects_bound(token_characters, scope_declarations),
            schema.checked_multiply(
                schema.checked_multiply(scope_declarations, 3), position_integer
            ),
        )
        binding_objects = schema.checked_add(
            schema.list_bound(scope_declarations),
            schema.tuple_bound(scope_declarations),
            schema.checked_multiply(
                scope_declarations, schema.object_bound(2)
            ),
        )
        # scope_bindings_linear builds brace/paren maps, ordered position sets,
        # assignment maps and stacks.  build_shadow_index builds the same block
        # work once more for declarators.
        one_block_assignment = schema.checked_add(
            schema.checked_multiply(2, schema.dict_bound(delimiter_pairs)),
            schema.checked_multiply(
                schema.checked_multiply(delimiter_pairs, 2), schema.tuple_bound(2)
            ),
            schema.dict_bound(scope_declarations),
            schema.dict_bound(scope_declarations),
            schema.list_bound(scope_declarations),
            schema.list_bound(delimiter_pairs),
        )
        block_work = schema.checked_multiply(3, one_block_assignment)
        scope_name_index = schema.checked_add(
            schema.dict_bound(scope_declarations),
            schema.checked_multiply(
                scope_declarations, schema.list_bound(1)
            ),
            schema.dict_bound(scope_declarations),
            schema.checked_multiply(
                scope_declarations, schema.tuple_bound(1)
            ),
        )
        shadow_index = schema.checked_add(
            schema.list_bound(shape.token_count),
            schema.dict_bound(shape.token_count),
            schema.dict_bound(shape.token_count),
            schema.checked_multiply(shape.token_count, schema.list_bound(1)),
            schema.checked_multiply(shape.token_count, schema.tuple_bound(2)),
            schema.dict_bound(shape.token_count),
            schema.checked_multiply(shape.token_count, schema.tuple_bound(1)),
        )
        scope_objects = schema.checked_add(
            next_nonspace_objects,
            declaration_objects,
            binding_objects,
            block_work,
            scope_name_index,
            shadow_index,
        )
    analysis = 0
    if shape.analysis_required:
        analysis = schema.checked_add(
            schema.object_bound(6),  # TranslationText
            schema.object_bound(12),  # CompilerAuditAnalysis
            schema.checked_multiply(3, schema.object_bound(1)),  # mapping proxies
            token_objects,
            delimiter_objects,
            member_call_objects,
            scope_objects,
        )
    json_integers = schema.checked_add(*(
        schema.pylong_bound(value) for value in shape.json_integer_values
    )) if shape.json_integer_values else 0
    json_objects = schema.checked_add(
        schema.json_scanner_fixed_bytes,
        schema.json_decoder_fixed_bytes,
        schema.json_encoder_scratch_bytes,
        schema.list_bound(shape.json_list_slots),
        schema.tuple_bound(shape.json_tuple_slots),
        schema.dict_bound(shape.json_dict_entries),
        schema.tuple_bound(shape.json_token_count),
        schema.string_bound(shape.json_string_characters),
        json_integers,
    )
    production_decode = schema.checked_add(
        schema.bytes_bound(shape.production_raw_bytes),
        schema.bytearray_bound(shape.production_raw_bytes),
        schema.bytesio_bound(shape.production_raw_bytes),
        schema.string_bound(shape.production_raw_bytes),
        schema.dict_bound(shape.json_dict_entries),
    )
    measurement_scratch = audit_measurement_scratch_bound(schema)
    charges = (
        measurement_scratch,
        input_chunks, joined_input, final_text, mapping_arrays, duplicate_keys,
        analysis, json_objects, production_decode,
    )
    return AuditAllocationPlan(*charges, schema.checked_add(*charges))


class AllocationConstructionObservation:
    __slots__ = ("reserved_before_allocation", "constructions")

    def __init__(self) -> None:
        self.reserved_before_allocation = False
        self.constructions = 0


class AuditAllocationObservations:
    __slots__ = (
        "events",
        "final_str",
        "chunk_list",
        "join_transient",
        "mapping_arrays",
        "maximum_chunk_characters",
        "pre_reserved_bytes",
        "peak_charged_bytes",
    )

    def __init__(self) -> None:
        self.events: list[str] = []
        self.final_str = AllocationConstructionObservation()
        self.chunk_list = AllocationConstructionObservation()
        self.join_transient = AllocationConstructionObservation()
        self.mapping_arrays = AllocationConstructionObservation()
        self.maximum_chunk_characters = 0
        self.pre_reserved_bytes = 0
        self.peak_charged_bytes = 0


@dataclass(frozen=True, slots=True)
class AllocationReservation:
    reserved_bytes: int

    def require_before_allocation(self, allocation_bytes: int) -> None:
        if (
            not isinstance(allocation_bytes, int)
            or isinstance(allocation_bytes, bool)
            or allocation_bytes < 0
            or allocation_bytes > self.reserved_bytes
        ):
            raise AuditInfrastructureError(
                "allocation exceeds pre-reserved allocation bytes"
            )


def reserve_before_allocation(total_bytes: int) -> AllocationReservation:
    schema = conservative_allocation_schema()
    schema._nonnegative(total_bytes)
    if total_bytes > schema.maximum_allocation_bytes:
        raise AuditInfrastructureError("allocation arithmetic limit exceeded")
    return AllocationReservation(total_bytes)


@dataclass(frozen=True, slots=True)
class CpythonAllocationLayoutDiagnostic:
    python_version: tuple[int, int, int]
    representative_objects_and_bounds: tuple[tuple[object, int], ...]


def probe_cpython_allocation_layout() -> CpythonAllocationLayoutDiagnostic:
    """Diagnostic-only CPython 3.11.9 calibration; production never calls this."""

    version = tuple(sys.version_info[index] for index in range(3))
    if version != (3, 11, 9):
        raise AuditInfrastructureError(
            "CPython 3.11.9 allocation layout diagnostic is unavailable"
        )
    schema = conservative_allocation_schema()
    representatives = (
        (0, schema.pylong_bound(0)),
        (1 << 200, schema.pylong_bound(1 << 200)),
        (b"x" * 257, schema.bytes_bound(257)),
        (bytearray(257), schema.bytearray_bound(257)),
        ("x" * 257, schema.string_bound(257)),
        ([None] * 17, schema.list_bound(17)),
        ((None,) * 17, schema.tuple_bound(17)),
        ({index: index for index in range(17)}, schema.dict_bound(17)),
    )
    return CpythonAllocationLayoutDiagnostic(version, representatives)


@dataclass(frozen=True, slots=True)
class _MeasuredAuditBufferLayout:
    shape: AuditAllocationShape
    retained_bytes: int


def _packed_spelling_occurrences(spellings, spelling_ids, target: bytes) -> int:
    try:
        spelling_id = spellings.index(target)
    except ValueError:
        return 0
    return spelling_ids.count(spelling_id)


_MEMBER_CALL_OPERATOR_SPELLINGS = (b".", b"->", b"::")
_SCOPE_MEMBER_METHOD_SPELLINGS = (b"read", b"withRead", b"complete")
_MEMBER_CALL_METHOD_SPELLINGS = (
    *_SCOPE_MEMBER_METHOD_SPELLINGS,
    b"nativeHandle",
)
_SCOPE_MEMBER_METHODS = frozenset(
    spelling.decode("ascii") for spelling in _SCOPE_MEMBER_METHOD_SPELLINGS
)


def _measure_audit_buffer_layout(tokens) -> _MeasuredAuditBufferLayout:
    schema = conservative_allocation_schema()
    spelling_ids = object.__getattribute__(tokens, "_spelling_ids")
    spellings = object.__getattribute__(tokens, "_spellings")
    identities = object.__getattribute__(tokens, "_identities")
    identity_ids = object.__getattribute__(tokens, "_identity_ids")
    inclusion_ids = object.__getattribute__(tokens, "_inclusion_ids")
    original_lines = object.__getattribute__(tokens, "_original_lines")
    candidate_spellings = capability_candidate_spellings()
    character_count = 0
    maximum_code_point = 0
    run_count = 0
    analysis_required = False
    previous_identity_id = previous_inclusion_id = previous_original_line = None
    token_count = len(tokens)
    for identity_id, inclusion_id, original_line in zip(
        identity_ids, inclusion_ids, original_lines
    ):
        if (
            previous_identity_id is None
            or identity_id != previous_identity_id
            or inclusion_id != previous_inclusion_id
            or original_line != previous_original_line
        ):
            run_count += 1
        previous_identity_id = identity_id
        previous_inclusion_id = inclusion_id
        previous_original_line = original_line

    for spelling_id in spelling_ids:
        spelling = spellings[spelling_id]
        if not analysis_required and spelling in candidate_spellings:
            analysis_required = True
        if spelling and spelling[0] in (ord('"'), ord("'")):
            piece_length = len(spelling)
            piece_maximum = 32 if piece_length else 0
        else:
            piece = _AUDIT_SPELLING_ALTERNATIVES.get(spelling, spelling)
            piece_length = len(piece)
            if maximum_code_point < 126 or not piece.isascii():
                piece_maximum = max(piece, default=0)
            elif maximum_code_point == 126 and b"\x7f" in piece:
                piece_maximum = 127
            else:
                piece_maximum = maximum_code_point
        character_count += piece_length
        maximum_code_point = max(maximum_code_point, piece_maximum)
    if token_count:
        # Each provenance run ends with one newline; tokens within a run are
        # separated by one space. Both totals follow from the packed shape.
        character_count += token_count
        maximum_code_point = max(
            maximum_code_point,
            32 if token_count > run_count else 10,
        )

    opening_delimiters = (
        _packed_spelling_occurrences(spellings, spelling_ids, b"{")
        + _packed_spelling_occurrences(spellings, spelling_ids, b"<%")
    )
    closing_delimiters = (
        _packed_spelling_occurrences(spellings, spelling_ids, b"}")
        + _packed_spelling_occurrences(spellings, spelling_ids, b"%>")
    )
    delimiter_pair_count = min(opening_delimiters, closing_delimiters)
    member_call_count = sum(
        _packed_spelling_occurrences(spellings, spelling_ids, method)
        for method in _MEMBER_CALL_METHOD_SPELLINGS
    )
    scope_member_call_count = sum(
        _packed_spelling_occurrences(spellings, spelling_ids, method)
        for method in _SCOPE_MEMBER_METHOD_SPELLINGS
    )
    scope_declaration_count = _packed_spelling_occurrences(
        spellings, spelling_ids, b"GpuSyncReadScope"
    )
    scope_analysis_required = bool(
        scope_member_call_count or scope_declaration_count
    )
    # Python integers do not wrap.  Validate every accumulated term once before
    # any allocation formula consumes it, instead of paying checked arithmetic
    # for each of millions of packed tokens.
    for measured_value in (
        character_count,
        run_count,
    ):
        schema.checked_add(measured_value)
    if character_count > 0xFFFFFFFF or run_count > 0xFFFFFFFF:
        raise AuditInfrastructureError("normalized audit mapping exceeds unsigned 32-bit range")
    # Synthetic packed fixtures may reserve identity zero as an unused absent
    # sentinel; live PreprocessedStreamBuilder views contain only real
    # identities.  Exclude only that proved-unused sentinel without allocating
    # an attacker-scaled set during premeasurement.
    unused_absent_sentinel = bool(
        identities
        and identities[0] is None
        and all(identity_id != 0 for identity_id in identity_ids)
    )
    origin_count = len(identities) - int(unused_absent_sentinel)
    mapping_bytes = schema.checked_multiply(
        schema.checked_add(
            schema.checked_multiply(run_count, AuditBuffer._RUN_COLUMN_COUNT), 1
        ),
        4,
    )
    retained = schema.checked_add(character_count, mapping_bytes, origin_count)
    chunk_count = (
        character_count + AuditBuffer._BLOCK_BYTES - 1
    ) // AuditBuffer._BLOCK_BYTES
    return _MeasuredAuditBufferLayout(
        AuditAllocationShape(
            text_character_count=character_count,
            maximum_code_point=maximum_code_point,
            run_count=run_count,
            origin_count=origin_count,
            chunk_count=chunk_count,
            token_count=token_count,
            analysis_required=analysis_required,
            scope_analysis_required=scope_analysis_required,
            token_value_characters=character_count,
            nonspace_character_count=character_count,
            delimiter_pair_count=delimiter_pair_count,
            member_call_count=member_call_count,
            scope_declaration_count=scope_declaration_count,
        ),
        retained,
    )


class AuditBuffer:
    """Bounded full-TU grammar text with offsets back to packed provenance runs."""

    _BLOCK_BYTES = 64 * 1024
    _RUN_BLOCK = 4096
    _RUN_COLUMN_COUNT = 10

    def __init__(
        self,
        text: str,
        view: PreprocessedTranslationUnitView,
        columns: tuple[array, ...],
        origin_production: bytes,
        peak_rss_bytes: int,
    ) -> None:
        self.text = text
        self._view = view
        (
            self._run_starts,
            self._run_stops,
            self._token_run_starts,
            self._token_run_stops,
            self._identity_ids,
            self._origin_ids,
            self._inclusion_ids,
            self._original_lines,
            self._run_change_prefix,
            self._production_prefix,
        ) = columns
        self._line_starts = self._run_starts
        self._origin_production = origin_production
        self.peak_rss_bytes = peak_rss_bytes
        self.text_character_count = len(text)
        self.text_max_code_point = max(map(ord, text), default=0)

    @staticmethod
    def _sample_rss(
        rss_reader: Callable[[], int], limit: int, peak: int, *, reserve: int = 0
    ) -> int:
        try:
            current = int(rss_reader())
        except (OSError, TypeError, ValueError) as error:
            raise AuditInfrastructureError("cannot sample coordinator RSS") from error
        if current < 0:
            raise AuditInfrastructureError("coordinator RSS sample is negative")
        peak = max(peak, current)
        if current + reserve >= limit:
            raise AuditInfrastructureError("coordinator RSS limit exceeded")
        return peak

    @staticmethod
    def _audit_spelling(spelling: bytes) -> bytes:
        if spelling.startswith((b'"', b"'")):
            return b" " * len(spelling)
        return _AUDIT_SPELLING_ALTERNATIVES.get(spelling, spelling)

    @classmethod
    def from_preprocessed(
        cls,
        view: PreprocessedTranslationUnitView,
        limits: AuditLimits,
        rss_reader: Callable[[], int],
        *,
        _allocation_observer: AuditAllocationObservations | None = None,
    ) -> "AuditBuffer":
        tokens = view.tokens
        spelling_ids_packed = object.__getattribute__(tokens, "_spelling_ids")
        spellings = object.__getattribute__(tokens, "_spellings")
        identities = object.__getattribute__(tokens, "_identities")
        measurement_scratch = audit_measurement_scratch_bound()
        measurement_reservation = reserve_before_allocation(measurement_scratch)
        peak_rss = cls._sample_rss(
            rss_reader, limits.rss_bytes, 0, reserve=measurement_scratch
        )
        measurement_reservation.require_before_allocation(measurement_scratch)
        measured = _measure_audit_buffer_layout(tokens)
        layout = measured.shape
        if _allocation_observer is not None:
            _allocation_observer.events.append("measure-layout")
        if measured.retained_bytes > limits.retained_token_bytes:
            raise AuditInfrastructureError("normalized audit byte limit exceeded")
        plan = audit_allocation_plan(layout)
        reservation = reserve_before_allocation(plan.pre_reserved_bytes)
        peak_rss = cls._sample_rss(
            rss_reader,
            limits.rss_bytes,
            peak_rss,
            reserve=plan.pre_reserved_bytes,
        )
        if _allocation_observer is not None:
            _allocation_observer.events.append("reserve-all-allocations")
            _allocation_observer.pre_reserved_bytes = plan.pre_reserved_bytes
            _allocation_observer.peak_charged_bytes = plan.pre_reserved_bytes
            for observed in (
                _allocation_observer.final_str,
                _allocation_observer.chunk_list,
                _allocation_observer.join_transient,
                _allocation_observer.mapping_arrays,
            ):
                observed.reserved_before_allocation = True

        reservation.require_before_allocation(plan.mapping_arrays)
        columns = tuple(
            array("I", (0,))
            * (layout.run_count + (1 if index == cls._RUN_COLUMN_COUNT - 1 else 0))
            for index in range(cls._RUN_COLUMN_COUNT)
        )
        if _allocation_observer is not None:
            _allocation_observer.mapping_arrays.constructions += 1
        (
            run_starts,
            run_stops,
            token_starts,
            token_stops,
            identity_ids,
            origin_ids,
            inclusion_ids,
            original_lines,
            change_prefix,
            production_prefix,
        ) = columns
        production_prefix[0] = 0
        origin_ids_by_key: dict[tuple[Path | None, PurePosixPath | None, bool], int] = {}
        origin_production = bytearray()
        reservation.require_before_allocation(plan.input_chunks_and_containers)
        chunks: list[str | None] = [None] * layout.chunk_count
        if _allocation_observer is not None:
            _allocation_observer.chunk_list.constructions += 1
        chunk = bytearray()
        chunk_index = 0
        text_characters = 0

        def flush_full_chunk() -> None:
            nonlocal chunk, chunk_index
            chunks[chunk_index] = chunk.decode("latin-1", errors="strict")
            if _allocation_observer is not None:
                _allocation_observer.maximum_chunk_characters = max(
                    _allocation_observer.maximum_chunk_characters, len(chunk)
                )
            chunk_index += 1
            chunk = bytearray()

        def append(piece: bytes) -> None:
            nonlocal chunk, chunk_index, text_characters
            piece_length = len(piece)
            if piece_length <= cls._BLOCK_BYTES - len(chunk):
                chunk.extend(piece)
                text_characters += piece_length
                if len(chunk) == cls._BLOCK_BYTES:
                    flush_full_chunk()
                return
            offset = 0
            while offset < piece_length:
                available = cls._BLOCK_BYTES - len(chunk)
                take = min(available, piece_length - offset)
                chunk.extend(piece[offset:offset + take])
                offset += take
                text_characters += take
                if len(chunk) == cls._BLOCK_BYTES:
                    flush_full_chunk()

        def append_spaces(count: int) -> None:
            while count:
                take = min(count, cls._BLOCK_BYTES - len(chunk))
                append(b" " * take)
                count -= take

        for run_index, packed_run in enumerate(tokens.iter_runs()):
            normalized_start = text_characters
            for token_index in range(packed_run.start, packed_run.stop):
                if token_index != packed_run.start:
                    if len(chunk) == cls._BLOCK_BYTES - 1:
                        append(b" ")
                    else:
                        chunk.append(32)
                        text_characters += 1
                spelling_id = spelling_ids_packed[token_index]
                spelling = spellings[spelling_id]
                if spelling.startswith((b'"', b"'")):
                    append_spaces(len(spelling))
                else:
                    piece = _AUDIT_SPELLING_ALTERNATIVES.get(spelling, spelling)
                    if len(piece) <= cls._BLOCK_BYTES - len(chunk):
                        chunk.extend(piece)
                        text_characters += len(piece)
                        if len(chunk) == cls._BLOCK_BYTES:
                            flush_full_chunk()
                    else:
                        append(piece)
            if len(chunk) == cls._BLOCK_BYTES - 1:
                append(b"\n")
            else:
                chunk.append(10)
                text_characters += 1
            identity = identities[packed_run.identity_id]
            origin_key = (
                identity.canonical if identity is not None else None,
                identity.relative if identity is not None else None,
                bool(identity is not None and identity.production),
            )
            origin_id = origin_ids_by_key.get(origin_key)
            if origin_id is None:
                origin_id = len(origin_ids_by_key)
                origin_ids_by_key[origin_key] = origin_id
                origin_production.append(int(origin_key[2]))
            changes = change_prefix[run_index - 1] if run_index else 0
            if (
                run_index
                and (
                    origin_ids[run_index - 1] != origin_id
                    or inclusion_ids[run_index - 1] != packed_run.inclusion_instance
                )
            ):
                changes += 1
            run_starts[run_index] = normalized_start
            run_stops[run_index] = text_characters
            token_starts[run_index] = packed_run.start
            token_stops[run_index] = packed_run.stop
            identity_ids[run_index] = packed_run.identity_id
            origin_ids[run_index] = origin_id
            inclusion_ids[run_index] = packed_run.inclusion_instance
            original_lines[run_index] = packed_run.original_line
            change_prefix[run_index] = changes
            production_prefix[run_index + 1] = (
                production_prefix[run_index] + int(origin_key[2])
            )
        if chunk:
            chunks[chunk_index] = chunk.decode("latin-1", errors="strict")
            if _allocation_observer is not None:
                _allocation_observer.maximum_chunk_characters = max(
                    _allocation_observer.maximum_chunk_characters, len(chunk)
                )
            chunk_index += 1
        if text_characters != layout.text_character_count or chunk_index != layout.chunk_count:
            raise AuditInfrastructureError("measured audit text layout changed")
        if len(origin_production) > layout.origin_count:
            raise AuditInfrastructureError("measured audit origin count changed")
        peak_rss = cls._sample_rss(
            rss_reader,
            limits.rss_bytes,
            peak_rss,
            reserve=layout.text_character_count + len(origin_production),
        )
        immutable_origin_production = bytes(origin_production)
        if any(item is None for item in chunks):
            raise AuditInfrastructureError("audit text chunk construction is incomplete")
        reservation.require_before_allocation(plan.joined_input_and_join_transient)
        if _allocation_observer is not None:
            _allocation_observer.join_transient.constructions += 1
        text = "".join(chunks)  # type: ignore[arg-type]
        if _allocation_observer is not None:
            _allocation_observer.final_str.constructions += 1
        if type(text) is not str:
            raise AuditInfrastructureError("audit text is not str")
        if len(text) != layout.text_character_count:
            raise AuditInfrastructureError("audit text length changed")
        if max(map(ord, text), default=0) != layout.maximum_code_point:
            raise AuditInfrastructureError("audit text storage kind changed")
        expected_mapping_counts = (
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count,
            layout.run_count + 1,
        )
        if tuple(map(len, columns)) != expected_mapping_counts:
            raise AuditInfrastructureError("audit mapping count changed")
        del chunks, chunk
        peak_rss = cls._sample_rss(rss_reader, limits.rss_bytes, peak_rss)
        return cls(text, view, columns, immutable_origin_production, peak_rss)

    def reserve_rss(
        self,
        rss_reader: Callable[[], int],
        limit: int,
        reserve: int,
    ) -> None:
        self.peak_rss_bytes = self._sample_rss(
            rss_reader, limit, self.peak_rss_bytes, reserve=reserve
        )

    def _run_index_at(self, offset: int) -> int:
        if not self._run_starts:
            raise IndexError("empty audit buffer has no source location")
        if offset < 0 or offset > len(self.text):
            raise IndexError("audit buffer offset out of range")
        if offset == len(self.text):
            return len(self._run_starts) - 1
        run_index = bisect.bisect_right(self._run_starts, offset) - 1
        if run_index < 0:
            raise IndexError("audit buffer offset has no source location")
        return run_index

    def location_at(self, offset: int) -> SourceLocation:
        run_index = self._run_index_at(offset)
        return SourceLocation(
            identity=self._view.tokens.identity_for(self._identity_ids[run_index]),
            inclusion_instance=self._inclusion_ids[run_index],
            line=self._original_lines[run_index],
            configuration_digest=self._view.configuration.digest,
        )

    def location_for_line(self, line: int) -> SourceLocation:
        if line < 1 or line > len(self._line_starts):
            raise AuditInfrastructureError(
                f"grammar finding references invalid normalized line {line}"
            )
        return self.location_at(self._line_starts[line - 1])

    def candidate_paths(
        self,
        trigger_spellings: frozenset[bytes] | None | bytes = _DEFAULT_CANDIDATE_PATHS,
    ) -> tuple[PurePosixPath, ...]:
        result: set[PurePosixPath] = set()
        candidates = (
            capability_candidate_spellings()
            if trigger_spellings == _DEFAULT_CANDIDATE_PATHS
            else trigger_spellings
        )
        tokens = self._view.tokens
        identities = object.__getattribute__(tokens, "_identities")
        spelling_ids = object.__getattribute__(tokens, "_spelling_ids")
        spellings = object.__getattribute__(tokens, "_spellings")
        for run_index in range(len(self._run_starts)):
            identity = identities[self._identity_ids[run_index]]
            if (
                identity is not None
                and identity.production
                and identity.relative is not None
                and is_production_path(identity.relative)
                and (
                    candidates is None
                    or any(
                        spellings[spelling_ids[token_index]] in candidates
                        for token_index in range(
                            self._token_run_starts[run_index],
                            self._token_run_stops[run_index],
                        )
                    )
                )
            ):
                result.add(identity.relative)
        return tuple(sorted(result, key=PurePosixPath.as_posix))

    def has_capability_spelling(self) -> bool:
        for run_index in range(len(self._run_starts)):
            for token_index in range(
                self._token_run_starts[run_index], self._token_run_stops[run_index]
            ):
                if self.token_spelling(token_index) in capability_candidate_spellings():
                    return True
        return False

    def token_spelling(self, token_index: int) -> bytes:
        return self._view.tokens.spelling_for(
            self._view.tokens.spelling_id_at(token_index)
        )

    def token_provenance_key(self, token_index: int) -> tuple[int, int, bool]:
        run_index = bisect.bisect_right(self._token_run_starts, token_index) - 1
        if run_index < 0 or token_index >= self._token_run_stops[run_index]:
            raise IndexError("packed token index has no audit provenance run")
        origin_id = self._origin_ids[run_index]
        return (
            origin_id,
            self._inclusion_ids[run_index],
            bool(self._origin_production[origin_id]),
        )

    def position_provenance_key(self, offset: int) -> tuple[int, int, bool]:
        run_index = self._run_index_at(offset)
        origin_id = self._origin_ids[run_index]
        return (
            origin_id,
            self._inclusion_ids[run_index],
            bool(self._origin_production[origin_id]),
        )

    def require_same_origin(self, start: int, stop: int) -> None:
        if start < 0 or stop <= start or stop > len(self.text):
            raise AuditInfrastructureError("invalid capability grammar provenance range")
        first_run = self._run_index_at(start)
        last_run = self._run_index_at(stop - 1)
        mismatch = self._run_change_prefix[last_run] != self._run_change_prefix[first_run]
        production = (
            self._production_prefix[last_run + 1]
            > self._production_prefix[first_run]
        )
        if mismatch and production:
            raise AuditInfrastructureError(
                "mixed provenance in guarded GPU capability expression"
            )

    def require_same_positions(self, *positions: int) -> None:
        if not positions:
            return
        keys = [self.position_provenance_key(position) for position in positions]
        if len(set(keys)) > 1 and any(key[2] for key in keys):
            raise AuditInfrastructureError(
                "mixed provenance in guarded GPU capability expression"
            )

    def path_has_spelling(
        self,
        path: PurePosixPath,
        spelling: bytes,
        inclusion_instance: int | None = None,
    ) -> bool:
        for run_index in range(len(self._run_starts)):
            identity = self._view.tokens.identity_for(self._identity_ids[run_index])
            if (
                identity is None
                or identity.relative != path
                or not identity.production
                or (
                    inclusion_instance is not None
                    and self._inclusion_ids[run_index] != inclusion_instance
                )
            ):
                continue
            for token_index in range(
                self._token_run_starts[run_index], self._token_run_stops[run_index]
            ):
                if self.token_spelling(token_index) == spelling:
                    return True
        return False


@dataclass(frozen=True)
class CppToken:
    value: str
    start: int
    end: int


TOKEN_PATTERN = re.compile(
    r"[A-Za-z_]\w*|\d+(?:\.\d+)?|##|::|->|&&|\|\||==|!=|<=|>=|\+\+|--|"
    r"<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|\.\.\.|[^\s]"
)
RAW_LITERAL_PREFIX = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')


_compiler_analysis_context = contextvars.ContextVar(
    "gpu_capability_compiler_analysis", default=None
)


def _compiler_tokens(
    masked: str, start: int = 0, end: int | None = None
) -> list[CppToken]:
    analysis = _compiler_analysis_context.get()
    if analysis is not None and analysis.masked is masked:
        limit = len(masked) if end is None else end
        first = bisect.bisect_left(analysis.token_starts, start)
        last = bisect.bisect_left(analysis.token_starts, limit)
        return [token for token in analysis.tokens[first:last] if token.end <= limit]
    """Tokenize enough C++ punctuation to enforce a conservative local grammar."""
    limit = len(masked) if end is None else end
    return [CppToken(match.group(), match.start(), match.end())
            for match in TOKEN_PATTERN.finditer(masked, start, limit)]


def cpp_tokens(masked: str, start: int = 0, end: int | None = None) -> list[CppToken]:
    """Public attested tokenizer entry; compiler analysis constructs it once."""

    return _compiler_tokens(masked, start, end)


def mask_non_code(source: str) -> str:
    """Replace comments and literals with spaces while preserving offsets/lines."""
    result = list(source)
    state = "code"
    index = 0
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if (current == "'" and index > 0
                    and source[index - 1] in "0123456789abcdefABCDEF"
                    and following in "0123456789abcdefABCDEF"):
                # C++ digit separators are code punctuation, not character quotes.
                index += 1
                continue
            raw = (RAW_LITERAL_PREFIX.match(source, index)
                   if current in {"L", "R", "U", "u"} else None)
            if raw is not None:
                terminator = ")" + raw.group(1) + '"'
                closing = source.find(terminator, raw.end())
                literal_end = len(source) if closing < 0 else closing + len(terminator)
                for literal_index in range(index, literal_end):
                    if source[literal_index] != "\n":
                        result[literal_index] = " "
                index = literal_end
                continue
            if current == "/" and following == "/":
                result[index] = result[index + 1] = " "
                state = "line-comment"
                index += 2
                continue
            if current == "/" and following == "*":
                result[index] = result[index + 1] = " "
                state = "block-comment"
                index += 2
                continue
            if current in {'"', "'"}:
                result[index] = " "
                state = "string" if current == '"' else "character"
                index += 1
                continue
        elif state == "line-comment":
            if current == "\\" and following == "\n":
                index += 2
                continue
            if (current == "\\" and following == "\r"
                    and index + 2 < len(source) and source[index + 2] == "\n"):
                index += 3
                continue
            if current == "\n":
                state = "code"
            else:
                result[index] = " "
            index += 1
            continue
        elif state == "block-comment":
            if current == "*" and following == "/":
                result[index] = result[index + 1] = " "
                state = "code"
                index += 2
                continue
            if current != "\n":
                result[index] = " "
            index += 1
            continue
        else:
            if current == "\\" and following:
                result[index] = " "
                if following != "\n":
                    result[index + 1] = " "
                index += 2
                continue
            terminator = '"' if state == "string" else "'"
            if current == terminator:
                result[index] = " "
                state = "code"
            elif current != "\n":
                result[index] = " "
            index += 1
            continue
        index += 1
    return "".join(result)


def normalize_alternative_tokens(masked: str) -> str:
    """Normalize C++ alternative preprocessing tokens without changing offsets."""
    result = list(masked)
    replacements = (
        ("%:%:", "##"), ("<:", "["), (":>", "]"), ("<%", "{"), ("%>", "}"),
        ("%:", "#"),
    )
    index = 0
    while index < len(masked):
        replacement = next(((spelling, value) for spelling, value in replacements
                            if masked.startswith(spelling, index)), None)
        if replacement is None:
            index += 1
            continue
        spelling, value = replacement
        for offset in range(len(spelling)):
            result[index + offset] = value[offset] if offset < len(value) else " "
        index += len(spelling)
    return "".join(result)


@dataclass(frozen=True)
class TranslationText:
    original: str
    text: str
    masked: str
    source_lines: tuple[int, ...]
    splice_boundaries: tuple[int, ...]
    normalized_line_starts: tuple[int, ...] = ()

    def line_at(self, position: int) -> int:
        if self.normalized_line_starts:
            return max(
                1, bisect.bisect_right(self.normalized_line_starts, position)
            )
        if 0 <= position < len(self.source_lines):
            return self.source_lines[position]
        return self.source_lines[-1] if self.source_lines else 1


def raw_literal_extents(source: str) -> list[tuple[int, int]]:
    """Find raw literals from original spelling before phase-two splice removal."""
    extents: list[tuple[int, int]] = []
    state = "code"
    quote = ""
    index = 0
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            raw = (RAW_LITERAL_PREFIX.match(source, index)
                   if current in {"L", "R", "U", "u"} else None)
            if raw is not None:
                terminator = ")" + raw.group(1) + '"'
                closing = source.find(terminator, raw.end())
                end = len(source) if closing < 0 else closing + len(terminator)
                extents.append((index, end))
                index = end
                continue
            if current == "/" and following == "/":
                state = "line-comment"
                index += 2
                continue
            if current == "/" and following == "*":
                state = "block-comment"
                index += 2
                continue
            if current in {'"', "'"}:
                quote = current
                state = "literal"
        elif state == "line-comment":
            if current == "\n":
                state = "code"
        elif state == "block-comment":
            if current == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif current == "\\" and following:
            index += 2
            continue
        elif current == quote:
            state = "code"
        index += 1
    return extents


def translate_source(source: str) -> TranslationText:
    """Apply raw-aware phase-two splicing, then lexical masking and token normalization."""
    raw_extents = raw_literal_extents(source)
    raw_index = 0
    normalized_chars: list[str] = []
    source_lines: list[int] = []
    splice_boundaries: list[int] = []
    index = 0
    current_line = 1
    while index < len(source):
        while raw_index < len(raw_extents) and index >= raw_extents[raw_index][1]:
            raw_index += 1
        inside_raw = (raw_index < len(raw_extents)
                      and raw_extents[raw_index][0] <= index < raw_extents[raw_index][1])
        if not inside_raw and source[index] == "\\" and index + 1 < len(source):
            if source[index + 1] == "\n":
                splice_boundaries.append(len(normalized_chars))
                current_line += 1
                index += 2
                continue
            if (source[index + 1] == "\r" and index + 2 < len(source)
                    and source[index + 2] == "\n"):
                splice_boundaries.append(len(normalized_chars))
                current_line += 1
                index += 3
                continue
        normalized_chars.append(source[index])
        source_lines.append(current_line)
        if source[index] == "\n":
            current_line += 1
        index += 1
    text = "".join(normalized_chars)
    masked = normalize_alternative_tokens(mask_non_code(text))
    return TranslationText(
        source, text, masked, tuple(source_lines), tuple(splice_boundaries))


def compiler_translation(source: str) -> TranslationText:
    """Use already token-normalized compiler text without per-character line tables."""

    line_starts = array("I", (0,))
    offset = source.find("\n")
    while offset >= 0 and offset + 1 < len(source):
        line_starts.append(offset + 1)
        offset = source.find("\n", offset + 1)
    return TranslationText(source, source, source, (), (), tuple(line_starts))


def brace_pairs(masked: str) -> list[tuple[int, int]]:
    stack: list[int] = []
    pairs: list[tuple[int, int]] = []
    for index, char in enumerate(masked):
        if char == "{":
            stack.append(index)
        elif char == "}" and stack:
            pairs.append((stack.pop(), index))
    return pairs


def enclosing_block(pairs: Iterable[tuple[int, int]], position: int) -> tuple[int, int] | None:
    candidates = [pair for pair in pairs if pair[0] < position < pair[1]]
    return min(candidates, key=lambda pair: pair[1] - pair[0]) if candidates else None


def line_number(source: str, position: int) -> int:
    return source.count("\n", 0, position) + 1


@dataclass(frozen=True)
class ScopeDeclaration:
    name: str
    position: int
    end: int
    name_position: int


def scope_declarations(masked: str) -> list[ScopeDeclaration]:
    declarations: list[ScopeDeclaration] = []
    typed = re.compile(
        r"\b(?:(?:const|volatile)\s+)*GpuSyncReadScope"
        r"(?:\s+(?:const|volatile))*\s*[*&]{0,2}\s*"
        r"(?:(?:const|volatile)\s+)*([A-Za-z_]\w*)"
        r"\s*(?=[;{=(,)])"
    )
    for match in typed.finditer(masked):
        prefix = masked[max(0, match.start() - 16):match.start()]
        if re.search(r"\b(?:class|struct)\s*$", prefix):
            continue
        declarations.append(ScopeDeclaration(
            match.group(1), match.start(), match.end(), match.start(1)))

    inferred = re.compile(
        r"\bauto(?:\s+(?:const|volatile))*\s*[*&]?\s+([A-Za-z_]\w*)\s*="
        r"\s*GpuSyncReadScope\b"
    )
    declarations.extend(
        ScopeDeclaration(match.group(1), match.start(), match.end(), match.start(1))
        for match in inferred.finditer(masked)
    )
    return sorted(set(declarations), key=lambda declaration: declaration.position)


def declaration_block(masked: str, pairs: list[tuple[int, int]],
                      declaration: ScopeDeclaration) -> tuple[int, int] | None:
    following = masked[declaration.end:].lstrip()
    if following.startswith((")", ",")):
        signature_end = masked.find(")", declaration.end)
        body_start = masked.find("{", signature_end + 1) if signature_end >= 0 else -1
        declaration_end = masked.find(";", signature_end + 1) if signature_end >= 0 else -1
        if body_start >= 0 and (declaration_end < 0 or body_start < declaration_end):
            return next((pair for pair in pairs if pair[0] == body_start), None)
    return enclosing_block(pairs, declaration.position)


_MEMBER_CALL_OPERATOR_PATTERN = "|".join(
    re.escape(spelling.decode("ascii"))
    for spelling in _MEMBER_CALL_OPERATOR_SPELLINGS
)
_MEMBER_CALL_METHOD_PATTERN = "|".join(
    re.escape(spelling.decode("ascii"))
    for spelling in _MEMBER_CALL_METHOD_SPELLINGS
)
MEMBER_CALL = re.compile(
    rf"(?:{_MEMBER_CALL_OPERATOR_PATTERN})\s*"
    rf"({_MEMBER_CALL_METHOD_PATTERN})\s*\("
)


def preprocessor_capability_findings(path: PurePosixPath,
                                     translated: TranslationText) -> list[Finding]:
    findings: list[Finding] = []
    unique_capability = re.compile(r"\b(?:GpuSyncReadScope|withRead|nativeHandle)\b")
    nonlocal_jump = re.compile(r"\b(?:_longjmp|longjmp|siglongjmp)\b")
    approved_consumers = REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
    approved_methods = REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())
    approved_types = REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset())

    def pasted_guarded_identifier(text: str) -> bool:
        tokens = re.findall(r"[A-Za-z_]\w*|##|\S", text)
        guarded = {
            "GpuSyncReadScope", "_longjmp", "longjmp", "nativeHandle", "siglongjmp",
            "withRead",
        }
        changed = True
        while changed:
            changed = False
            collapsed: list[str] = []
            index = 0
            while index < len(tokens):
                if (index + 2 < len(tokens) and tokens[index + 1] == "##"
                        and re.fullmatch(r"[A-Za-z_]\w*", tokens[index])
                        and re.fullmatch(r"[A-Za-z_]\w*", tokens[index + 2])):
                    collapsed.append(tokens[index] + tokens[index + 2])
                    index += 3
                    changed = True
                else:
                    collapsed.append(tokens[index])
                    index += 1
            tokens = collapsed
        return any(token in guarded for token in tokens)

    def audit_logical(logical: str, logical_start: int) -> None:
        stripped = logical.lstrip()
        if not stripped.startswith("#"):
            return
        macro_mutation = re.match(r"#\s*(?:define|undef)\b", stripped) is not None
        if "##" in stripped or "%:%:" in stripped:
            findings.append(Finding(
                path, logical_start, "preprocessor token concatenation",
                "token concatenation can reconstruct guarded GPU operations or non-local "
                "control flow"))
        if (macro_mutation and any(re.search(rf"\b{re.escape(name)}\b", stripped)
                                   for name in approved_consumers)):
            findings.append(Finding(
                path, logical_start, "approved native-handle consumer cannot be hidden or shadowed",
                "approved native-handle consumers cannot be replaced by preprocessing"))
        if (macro_mutation and any(re.search(rf"\b{re.escape(name)}\b", stripped)
                                   for name in approved_methods)):
            findings.append(Finding(
                path, logical_start, "approved native-handle method cannot be hidden or shadowed",
                "approved native-handle methods cannot be replaced by preprocessing"))
        if (macro_mutation and any(re.search(rf"\b{re.escape(name)}\b", stripped)
                                   for name in approved_types)):
            findings.append(Finding(
                path, logical_start, "approved native-handle type cannot be hidden or shadowed",
                "approved native-handle types cannot be replaced by preprocessing"))
        if nonlocal_jump.search(stripped):
            findings.append(Finding(
                path, logical_start, "non-local jump preprocessor alias",
                "non-local jump cannot be hidden by preprocessing"))
        elif unique_capability.search(stripped) or pasted_guarded_identifier(stripped):
            findings.append(Finding(
                path, logical_start, "GPU capability preprocessor alias",
                "nativeHandle alias and scope operations cannot be hidden by preprocessing"))

    offset = 0
    for text in translated.masked.splitlines(keepends=True):
        audit_logical(text, translated.line_at(offset))
        offset += len(text)
    return findings


@dataclass(frozen=True)
class MacroDefinition:
    function_like: bool
    replacement: tuple[str, ...]
    parameters: tuple[str, ...] = ()
    variadic: str | None = None


def source_macro_events(masked: str) \
        -> tuple[list[tuple[int, str, MacroDefinition | None]], list[tuple[int, int]]]:
    """Return ordered macro state changes and directive ranges."""
    events: list[tuple[int, str, MacroDefinition | None]] = []
    directives: list[tuple[int, int]] = []
    offset = 0
    for line in masked.splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            directives.append((offset, offset + len(line)))
            undef = re.match(r"#\s*undef\s+([A-Za-z_]\w*)", stripped)
            define = re.match(r"#\s*define\s+([A-Za-z_]\w*)(.*)", stripped)
            if undef is not None:
                events.append((offset + len(line), undef.group(1), None))
            elif define is not None:
                tail = define.group(2).rstrip("\r\n")
                function_like = tail.startswith("(")
                replacement = tail
                parameters: tuple[str, ...] = ()
                variadic: str | None = None
                if function_like:
                    closing = matching_delimiter(tail, 0, "(", ")")
                    if closing is not None:
                        parsed_parameters: list[str] = []
                        for raw_parameter in tail[1:closing].split(","):
                            parameter = raw_parameter.strip()
                            if not parameter:
                                continue
                            if parameter == "...":
                                variadic = "__VA_ARGS__"
                            elif parameter.endswith("..."):
                                variadic = parameter[:-3].strip()
                            else:
                                parsed_parameters.append(parameter)
                        parameters = tuple(parsed_parameters)
                    replacement = tail[closing + 1:] if closing is not None else ""
                events.append((
                    offset + len(line), define.group(1),
                    MacroDefinition(function_like, tuple(
                        token.value for token in _compiler_tokens(replacement)), parameters,
                                    variadic)))
        offset += len(line)
    return events, directives


MAX_SOURCE_ONLY_GENERATED_WORK = 2048
MAX_CONDITIONAL_ENVIRONMENTS = MAX_SOURCE_ONLY_GENERATED_WORK + 1


def source_macro_environment_events(
    masked: str,
) -> tuple[
    list[tuple[int, tuple[dict[str, MacroDefinition], ...] | None]],
    list[tuple[int, int]],
]:
    """Model every reachable conditional macro environment without de-correlation."""

    macro_events, directive_ranges = source_macro_events(masked)
    mutations = {
        (position, name): definition
        for position, name, definition in macro_events
    }

    def clone(
        environments: tuple[dict[str, MacroDefinition], ...] | None,
    ) -> tuple[dict[str, MacroDefinition], ...] | None:
        if environments is None:
            return None
        return tuple(dict(environment) for environment in environments)

    def deduplicate(
        environments: list[dict[str, MacroDefinition]],
    ) -> tuple[dict[str, MacroDefinition], ...] | None:
        unique: dict[
            tuple[tuple[str, MacroDefinition], ...], dict[str, MacroDefinition]
        ] = {}
        for environment in environments:
            key = tuple(sorted(environment.items()))
            unique.setdefault(key, environment)
            if len(unique) > MAX_CONDITIONAL_ENVIRONMENTS:
                return None
        return tuple(dict(environment) for environment in unique.values())

    environments: tuple[dict[str, MacroDefinition], ...] | None = ({},)
    frames: list[dict[str, object]] = []
    events: list[
        tuple[int, tuple[dict[str, MacroDefinition], ...] | None]
    ] = []
    offset = 0
    for line in masked.splitlines(keepends=True):
        stripped = line.lstrip()
        directive = re.match(r"#\s*([A-Za-z_]\w*)\b(.*)", stripped)
        if directive is None:
            offset += len(line)
            continue
        name = directive.group(1)
        event_position = offset + len(line)
        if name in {"if", "ifdef", "ifndef"}:
            frames.append({
                "baseline": clone(environments),
                "completed": [],
                "saw_else": False,
            })
            environments = clone(environments)
        elif name in {"elif", "elifdef", "elifndef", "else"} and frames:
            frame = frames[-1]
            completed = frame["completed"]
            if environments is None:
                frame["completed"] = None
            elif completed is not None:
                completed.extend(dict(environment) for environment in environments)
            environments = clone(frame["baseline"])
            if name == "else":
                frame["saw_else"] = True
        elif name == "endif" and frames:
            frame = frames.pop()
            completed = frame["completed"]
            if environments is None or completed is None:
                environments = None
            else:
                completed.extend(dict(environment) for environment in environments)
                if not frame["saw_else"]:
                    baseline = frame["baseline"]
                    if baseline is None:
                        environments = None
                        events.append((event_position, environments))
                        offset += len(line)
                        continue
                    completed.extend(dict(environment) for environment in baseline)
                environments = deduplicate(completed)
        elif name in {"define", "undef"}:
            mutation = re.match(r"\s*([A-Za-z_]\w*)", directive.group(2))
            if mutation is not None and environments is not None:
                macro_name = mutation.group(1)
                definition = mutations.get((event_position, macro_name))
                changed: list[dict[str, MacroDefinition]] = []
                for environment in environments:
                    updated = dict(environment)
                    if definition is None:
                        updated.pop(macro_name, None)
                    else:
                        updated[macro_name] = definition
                    changed.append(updated)
                environments = deduplicate(changed)
        events.append((event_position, clone(environments)))
        offset += len(line)
    return events, directive_ranges


def guarded_macro_composition_findings(
        path: PurePosixPath, translated: TranslationText,
        *, source_only: bool = False,
        ) -> list[Finding]:
    """Reject source-visible macro calls whose identifier pieces form guarded names."""
    guarded = {
        "GpuSyncReadScope", "_longjmp", "complete", "longjmp", "nativeHandle", "read",
        "siglongjmp", "withRead",
    }
    guarded.update(REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset()))
    guarded.update(REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset()))
    guarded.update(REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset()))
    tokens = _compiler_tokens(translated.masked)
    environment_events, directive_ranges = source_macro_environment_events(
        translated.masked
    )
    sensitive_ranges = (
        _source_only_sensitive_ranges(translated.masked, directive_ranges)
        if source_only else []
    )
    merged_sensitive_ranges: list[tuple[int, int]] = []
    for start, stop, _role in sorted(sensitive_ranges):
        if merged_sensitive_ranges and start <= merged_sensitive_ranges[-1][1]:
            previous_start, previous_stop = merged_sensitive_ranges[-1]
            merged_sensitive_ranges[-1] = (
                previous_start, max(previous_stop, stop)
            )
        else:
            merged_sensitive_ranges.append((start, stop))
    sensitive_range_starts = tuple(
        start for start, _stop in merged_sensitive_ranges
    )

    def token_has_sensitive_role(position: int) -> bool:
        range_index = bisect.bisect_right(sensitive_range_starts, position) - 1
        return (range_index >= 0
                and position < merged_sensitive_ranges[range_index][1])
    all_macro_names = {
        name for _position, name, _definition in source_macro_events(
            translated.masked
        )[0]
    }
    directive_starts = tuple(start for start, _stop in directive_ranges)
    macros: dict[str, MacroDefinition] = {}
    environments: tuple[dict[str, MacroDefinition], ...] | None = ({},)
    event_index = 0
    findings: list[Finding] = []

    class ExpansionDepthExceeded(RuntimeError):
        pass

    class ExpansionTokenComplexityExceeded(RuntimeError):
        pass

    class ExpansionPasteComplexityExceeded(RuntimeError):
        pass

    class ExpansionSyntaxFailure(RuntimeError):
        pass

    @dataclass(frozen=True)
    class ExpansionToken:
        value: str
        hidden: frozenset[str] = frozenset()
        source_index: int | None = None

    maximum_expansion_depth = 96
    maximum_continuous_macro_tokens = 256
    maximum_generated_macro_tokens = MAX_SOURCE_ONLY_GENERATED_WORK
    maximum_macro_paste_operations = 1024

    @dataclass
    class ExpansionBudget:
        remaining_tokens: int = maximum_generated_macro_tokens
        remaining_pastes: int = maximum_macro_paste_operations

        def reserve_tokens(self, count: int) -> None:
            if count < 0 or count > self.remaining_tokens:
                raise ExpansionTokenComplexityExceeded
            self.remaining_tokens -= count

        def reserve_paste(self) -> None:
            if self.remaining_pastes <= 0:
                raise ExpansionPasteComplexityExceeded
            self.remaining_pastes -= 1

    def value_call_arguments(values: tuple[ExpansionToken, ...], opening: int) \
            -> tuple[list[tuple[ExpansionToken, ...]], int] | None:
        depth = 1
        argument_start = opening + 1
        arguments: list[tuple[ExpansionToken, ...]] = []
        for cursor in range(opening + 1, len(values)):
            value = values[cursor].value
            if value == "(":
                depth += 1
            elif value == ")":
                depth -= 1
                if depth == 0:
                    if cursor != opening + 1 or arguments:
                        arguments.append(values[argument_start:cursor])
                    return arguments, cursor
            elif depth == 1 and value == ",":
                arguments.append(values[argument_start:cursor])
                argument_start = cursor + 1
        return None

    def with_hidden(values: tuple[ExpansionToken, ...], hidden: frozenset[str]) \
            -> tuple[ExpansionToken, ...]:
        return tuple(
            ExpansionToken(
                value.value, value.hidden | hidden, value.source_index
            ) for value in values)

    def join_variadic(arguments: list[tuple[ExpansionToken, ...]],
                      hidden: frozenset[str]) -> tuple[ExpansionToken, ...]:
        joined: list[ExpansionToken] = []
        for index, argument in enumerate(arguments):
            if index:
                joined.append(ExpansionToken(",", hidden))
            joined.extend(argument)
        return tuple(joined)

    def paste_tokens(values: list[ExpansionToken], budget: ExpansionBudget,
                     inherited_hidden: frozenset[str]) -> tuple[ExpansionToken, ...]:
        pasted: list[ExpansionToken] = []
        cursor = 0
        while cursor < len(values):
            token = values[cursor]
            if token.value == "##":
                raise ExpansionSyntaxFailure
            current = token
            while cursor + 1 < len(values) and values[cursor + 1].value == "##":
                budget.reserve_paste()
                if cursor + 2 >= len(values) or values[cursor + 2].value == "##":
                    raise ExpansionSyntaxFailure
                right = values[cursor + 2]
                if current.value == "__macro_placemarker__":
                    combined_value = right.value
                elif right.value == "__macro_placemarker__":
                    combined_value = current.value
                else:
                    combined_value = current.value + right.value
                current = ExpansionToken(
                    combined_value,
                    current.hidden | right.hidden | inherited_hidden)
                cursor += 2
            if current.value != "__macro_placemarker__":
                pasted.append(current)
            cursor += 1
        return tuple(pasted)

    def expand_function(name: str, definition: MacroDefinition,
                        supplied: list[tuple[ExpansionToken, ...]], depth: int,
                        inherited_hidden: frozenset[str], budget: ExpansionBudget,
                        suffix_truncated: bool,
                        consumed_sources: set[int]) \
            -> tuple[ExpansionToken, ...]:
        if depth > maximum_expansion_depth:
            raise ExpansionDepthExceeded
        replacement_hidden = inherited_hidden | {name}
        argument_hidden = frozenset({name})
        fixed_count = len(definition.parameters)
        variadic = definition.variadic
        if not supplied and fixed_count:
            supplied = [tuple()]
        if ((variadic is None and len(supplied) != fixed_count)
                or (variadic is not None and len(supplied) < fixed_count)):
            raise ExpansionSyntaxFailure

        fixed_parameters = definition.parameters[:fixed_count]
        raw_arguments = dict(zip(fixed_parameters, supplied[:fixed_count], strict=True))
        replacement = definition.replacement
        parameter_names = set(fixed_parameters)
        if variadic is not None:
            parameter_names.add(variadic)
        normal_substitutions: set[str] = set()
        for replacement_index, value in enumerate(replacement):
            if value not in parameter_names:
                continue
            previous = replacement[replacement_index - 1] if replacement_index else ""
            following = (
                replacement[replacement_index + 1]
                if replacement_index + 1 < len(replacement) else ""
            )
            if previous not in {"#", "##"} and following != "##":
                normal_substitutions.add(value)
        prescanned_arguments = {
            parameter: expand_sequence(
                argument, depth + 1, budget, suffix_truncated,
                consumed_sources
            )
            for parameter, argument in raw_arguments.items()
            if parameter in normal_substitutions
        }
        variadic_has_tokens = False
        if variadic is not None:
            raw_variadic = supplied[fixed_count:]
            raw_arguments[variadic] = join_variadic(raw_variadic, argument_hidden)
            if variadic in normal_substitutions or "__VA_OPT__" in replacement:
                prescanned_variadic = [
                    expand_sequence(
                        argument, depth + 1, budget, suffix_truncated,
                        consumed_sources
                    )
                    for argument in raw_variadic
                ]
                joined_prescanned_variadic = join_variadic(
                    prescanned_variadic, argument_hidden
                )
                variadic_has_tokens = bool(joined_prescanned_variadic)
                prescanned_arguments[variadic] = joined_prescanned_variadic
        if "__VA_OPT__" in replacement:
            if variadic is None:
                raise ExpansionSyntaxFailure
            resolved: list[str] = []
            replacement_cursor = 0
            while replacement_cursor < len(replacement):
                if replacement[replacement_cursor] != "__VA_OPT__":
                    resolved.append(replacement[replacement_cursor])
                    replacement_cursor += 1
                    continue
                if (replacement_cursor + 1 >= len(replacement)
                        or replacement[replacement_cursor + 1] != "("):
                    raise ExpansionSyntaxFailure
                depth_cursor = 1
                closing_cursor = replacement_cursor + 2
                while closing_cursor < len(replacement) and depth_cursor:
                    if replacement[closing_cursor] == "(":
                        depth_cursor += 1
                    elif replacement[closing_cursor] == ")":
                        depth_cursor -= 1
                    closing_cursor += 1
                if depth_cursor:
                    raise ExpansionSyntaxFailure
                if variadic_has_tokens:
                    resolved.extend(
                        replacement[replacement_cursor + 2:closing_cursor - 1]
                    )
                replacement_cursor = closing_cursor
            replacement = tuple(resolved)
        substituted: list[ExpansionToken] = []
        cursor = 0
        while cursor < len(replacement):
            value = replacement[cursor]
            if (value == "#" and cursor + 1 < len(replacement)
                    and replacement[cursor + 1] in raw_arguments):
                substituted.append(ExpansionToken(
                    "__macro_string_literal__", replacement_hidden))
                cursor += 2
                continue
            if value in raw_arguments:
                adjacent_to_paste = (
                    (cursor and replacement[cursor - 1] == "##")
                    or (cursor + 1 < len(replacement)
                        and replacement[cursor + 1] == "##"))
                selected = (raw_arguments[value] if adjacent_to_paste
                            else prescanned_arguments.get(value, raw_arguments[value]))
                selected_with_hidden = with_hidden(selected, argument_hidden)
                if adjacent_to_paste and not selected_with_hidden:
                    selected_with_hidden = (
                        ExpansionToken("__macro_placemarker__", replacement_hidden),)
                substituted.extend(selected_with_hidden)
            else:
                substituted.append(ExpansionToken(value, replacement_hidden))
            cursor += 1

        pasted = paste_tokens(substituted, budget, replacement_hidden)
        budget.reserve_tokens(len(pasted))
        return pasted

    def expand_sequence(values: tuple[ExpansionToken, ...], depth: int = 0,
                        budget: ExpansionBudget | None = None,
                        suffix_truncated: bool = False,
                        consumed_sources: set[int] | None = None) \
            -> tuple[ExpansionToken, ...]:
        if depth > maximum_expansion_depth:
            raise ExpansionDepthExceeded
        if budget is None:
            budget = ExpansionBudget()
        if consumed_sources is None:
            consumed_sources = set()
        expanded: list[ExpansionToken] = []
        cursor = 0
        while cursor < len(values):
            token = values[cursor]
            value = token.value
            definition = macros.get(value)
            if definition is None or value in token.hidden:
                expanded.append(token)
                cursor += 1
                continue
            if not definition.function_like:
                if token.source_index is not None:
                    consumed_sources.add(token.source_index)
                replacement_hidden = token.hidden | {value}
                replacement_values = [
                    ExpansionToken(item, replacement_hidden)
                    for item in definition.replacement]
                replacement = paste_tokens(
                    replacement_values, budget, replacement_hidden)
                budget.reserve_tokens(len(replacement))
                rescanned = expand_sequence(
                    replacement + values[cursor + 1:], depth + 1, budget,
                    suffix_truncated, consumed_sources)
                return tuple(expanded) + rescanned
            if (cursor + 1 >= len(values)
                    or values[cursor + 1].value != "("):
                expanded.append(token)
                cursor += 1
                continue
            parsed = value_call_arguments(values, cursor + 1)
            if parsed is None:
                if suffix_truncated:
                    raise ExpansionTokenComplexityExceeded
                raise ExpansionSyntaxFailure
            arguments, closing = parsed
            consumed_sources.update(
                source_index
                for source_index in (
                    item.source_index for item in values[cursor:closing + 1]
                )
                if source_index is not None
            )
            replacement = expand_function(
                value, definition, arguments, depth + 1,
                token.hidden, budget, suffix_truncated, consumed_sources)
            rescanned = expand_sequence(
                replacement + values[closing + 1:], depth + 1, budget,
                suffix_truncated, consumed_sources)
            return tuple(expanded) + rescanned
        return tuple(expanded)

    paren_stack: list[int] = []
    paren_closings: dict[int, int] = {}
    for token_index, token in enumerate(tokens):
        if token.value == "(":
            paren_stack.append(token_index)
        elif token.value == ")" and paren_stack:
            paren_closings[paren_stack.pop()] = token_index
    reported_postfix_complexity_lines: set[int] = set()
    EnvironmentKey = tuple[tuple[str, MacroDefinition], ...]
    owned_environments_by_token: dict[int, set[EnvironmentKey]] = {}

    def environment_key(
        environment: dict[str, MacroDefinition],
    ) -> EnvironmentKey:
        return tuple(sorted(environment.items()))

    def token_is_in_directive(position: int) -> bool:
        range_index = bisect.bisect_right(directive_starts, position) - 1
        return (range_index >= 0
                and position < directive_ranges[range_index][1])

    for index, token in enumerate(tokens[:-1]):
        while (event_index < len(environment_events)
               and environment_events[event_index][0] <= token.start):
            _event_position, environments = environment_events[event_index]
            event_index += 1
        if (not re.fullmatch(r"[A-Za-z_]\w*", token.value)
                or token_is_in_directive(token.start)):
            continue
        role_sensitive = token_has_sensitive_role(token.start)
        if environments is None:
            if source_only and (token.value in all_macro_names or role_sensitive):
                findings.append(Finding(
                    path,
                    translated.line_at(token.start),
                    "source-only conditional state complexity",
                    "conditional macro environment work exceeds the bounded audit limit; "
                    "rejected fail-closed",
                ))
            continue
        owned_environments = owned_environments_by_token.get(index, set())
        token_environments = tuple(
            environment for environment in environments
            if environment_key(environment) not in owned_environments
        )
        if not token_environments:
            continue
        definitions = [
            environment.get(token.value) for environment in token_environments
        ]
        definition = next(
            (candidate for candidate in definitions if candidate is not None), None
        )
        if definition is None:
            continue
        too_complex = False
        if tokens[index + 1].value == "(":
            closing = paren_closings.get(index + 1)
            if closing is None:
                if any(candidate is not None and candidate.function_like
                       for candidate in definitions):
                    findings.append(Finding(
                        path, translated.line_at(token.start), "macro expansion syntax",
                        "macro expansion syntax is incomplete; rejected fail-closed"))
                continue
            too_complex = closing - index + 1 > maximum_continuous_macro_tokens
            while closing + 1 < len(tokens) and tokens[closing + 1].value == "(":
                postfix_closing = paren_closings.get(closing + 1)
                if postfix_closing is None:
                    break
                if postfix_closing - index + 1 > maximum_continuous_macro_tokens:
                    too_complex = True
                    break
                closing = postfix_closing
        if too_complex:
            line = translated.line_at(token.start)
            if line not in reported_postfix_complexity_lines:
                reported_postfix_complexity_lines.add(line)
                findings.append(Finding(
                    path, line, "macro postfix complexity",
                    "macro postfix complexity exceeds the bounded audit limit; "
                    "rejected fail-closed"))
            continue

        suffix_end = index
        while (suffix_end < len(tokens)
               and suffix_end - index < maximum_continuous_macro_tokens):
            suffix_token = tokens[suffix_end]
            if (suffix_end > index
                    and token_is_in_directive(suffix_token.start)):
                break
            suffix_end += 1
            if suffix_token.value in {";", "{", "}"}:
                break
        suffix_truncated = (
            suffix_end < len(tokens)
            and tokens[suffix_end - 1].value not in {";", "{", "}"}
            and not token_is_in_directive(tokens[suffix_end].start))
        invocation = tuple(
            ExpansionToken(item.value, source_index=source_index)
            for source_index, item in enumerate(
                tokens[index:suffix_end], start=index
            )
        )
        context_sensitive = role_sensitive
        def expand_with(
            environment: dict[str, MacroDefinition],
            candidate: MacroDefinition,
            budget: ExpansionBudget,
        ) -> tuple[tuple[ExpansionToken, ...], set[int]]:
            saved = dict(macros)
            macros.clear()
            macros.update(environment)
            macros[token.value] = candidate
            consumed_sources: set[int] = set()
            try:
                expansion = expand_sequence(
                    invocation,
                    budget=budget,
                    suffix_truncated=suffix_truncated,
                    consumed_sources=consumed_sources,
                )
                return expansion, consumed_sources
            finally:
                macros.clear()
                macros.update(saved)

        def expansion_policy(
            candidate: tuple[ExpansionToken, ...],
        ) -> str | None:
            candidate_spellings = {item.value for item in candidate}
            policy_spellings = set(guarded)
            policy_spellings.update(
                REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
            )
            policy_spellings.update(
                REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset())
            )
            policy_spellings.update(
                REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())
            )
            if path == REGISTRY_HEADER:
                policy_spellings.add("registerRetire")
            if path == OP_SCOPE_HEADER:
                policy_spellings.add("track")
            if candidate_spellings.isdisjoint(policy_spellings):
                return None
            prefix = [
                item.value for item in tokens[:index]
                if not token_is_in_directive(item.start)
            ]
            rendered = " ".join(prefix + [item.value for item in candidate])
            if audit_capability_uses(path, rendered, compiler_view=True):
                return "guarded identifier macro composition"
            if re.search(
                r"(?:&\s*[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*\s*::|\.|->)\s*"
                r"nativeHandle\b",
                rendered,
            ):
                return "guarded identifier macro composition"
            if re.search(r"\b(?:_longjmp|longjmp|siglongjmp)\s*\(", rendered):
                return "guarded identifier macro composition"
            if any(
                re.search(rf"\b{re.escape(name)}\s*\(", rendered)
                for name in REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
            ):
                return "guarded identifier macro composition"
            if any(
                re.search(
                    rf"\b(?:const_cast|dynamic_cast|reinterpret_cast|static_cast)\s*"
                    rf"<[^>]*\b{re.escape(name)}\b",
                    rendered,
                )
                for name in REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset())
            ):
                return "guarded identifier macro composition"
            if any(
                re.search(
                    rf"(?:\.|->)\s*{re.escape(name)}\s*\(", rendered
                )
                for name in REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())
            ):
                return "guarded identifier macro composition"
            policies = (
                (REGISTRY_HEADER, "GpuRetireRegistry", "registerRetire",
                 "GpuRetireRegistry::registerRetire()"),
                (OP_SCOPE_HEADER, "GpuOpScope", "track", "GpuOpScope::track()"),
            )
            for policy_path, class_name, member_name, expression in policies:
                if path != policy_path or member_name not in rendered:
                    continue
                if audit_public_member(
                    path,
                    rendered,
                    class_name,
                    rf"\b{re.escape(member_name)}\s*\(",
                    expression,
                    pretokenized=True,
                ):
                    return expression
            return None

        expansion_budget = ExpansionBudget()
        expansion_records: list[
            tuple[EnvironmentKey, tuple[ExpansionToken, ...], set[int]]
        ] = []
        undefined_environment = False
        try:
            expansion_budget.reserve_tokens(max(0, len(token_environments) - 1))
            for environment, candidate in zip(
                token_environments, definitions, strict=True
            ):
                if candidate is None:
                    undefined_environment = True
                    continue
                expansion, consumed_sources = expand_with(
                    environment, candidate, expansion_budget
                )
                expansion_records.append((
                    environment_key(environment), expansion, consumed_sources
                ))
        except ExpansionDepthExceeded:
            findings.append(Finding(
                path, translated.line_at(token.start),
                ("source-only macro expansion depth" if source_only
                 else "macro expansion depth"),
                "macro expansion depth exceeded the bounded audit limit; rejected fail-closed"))
            continue
        except ExpansionTokenComplexityExceeded:
            findings.append(Finding(
                path, translated.line_at(token.start),
                ("source-only conditional state complexity"
                 if source_only and len(token_environments) > 1 else
                 "source-only macro token complexity" if source_only
                 else "macro expansion complexity"),
                ("conditional environment and expansion work exceeds the bounded "
                 "audit limit; rejected fail-closed"
                 if source_only and len(token_environments) > 1 else
                 "macro expansion complexity exceeded the bounded audit limit; "
                 "rejected fail-closed")))
            continue
        except ExpansionPasteComplexityExceeded:
            findings.append(Finding(
                path, translated.line_at(token.start),
                ("source-only macro paste complexity" if source_only
                 else "macro expansion complexity"),
                "macro token-paste complexity exceeds the bounded audit limit; "
                "rejected fail-closed"))
            continue
        except ExpansionSyntaxFailure:
            findings.append(Finding(
                path, translated.line_at(token.start),
                ("source-only macro expansion syntax" if source_only
                 else "macro expansion syntax"),
                "macro expansion syntax is incomplete; rejected fail-closed"))
            continue

        for environment, _expansion, consumed_sources in expansion_records:
            for consumed_source in consumed_sources:
                if consumed_source > index:
                    owned_environments_by_token.setdefault(
                        consumed_source, set()
                    ).add(environment)
        expansion_results = [
            expansion for _environment, expansion, _consumed in expansion_records
        ]
        expansion_keys = {
            tuple(item.value for item in expansion)
            for expansion in expansion_results
        }
        conditional_effect = (
            len(token_environments) > 1
            and (undefined_environment or len(expansion_keys) > 1)
        )
        policy_expressions = {
            expression
            for expansion in expansion_results
            if (expression := expansion_policy(expansion)) is not None
        }
        if conditional_effect and (context_sensitive or policy_expressions):
            findings.append(Finding(
                path,
                translated.line_at(token.start),
                "source-only conditional macro ambiguity",
                "reachable conditional environments change capability-sensitive "
                "grammar; rejected fail-closed",
            ))
            continue
        if not policy_expressions:
            continue
        policy_expression = sorted(policy_expressions)[0]
        findings.append(Finding(
            path,
            translated.line_at(token.start),
            policy_expression,
            ("macro expansion cannot generate a public GPU retirement surface"
             if policy_expression != "guarded identifier macro composition" else
             "macro arguments cannot be composed into guarded GPU or non-local control names"),
        ))
    return findings


def phase_two_capability_findings(path: PurePosixPath, source: str,
                                  translated: TranslationText | None = None) -> list[Finding]:
    """Normalize splices once, then reject reconstructed guarded identifiers in O(n)."""
    if "\\\n" not in source and "\\\r\n" not in source:
        return []
    guarded = {
        "GpuSyncReadScope", "_longjmp", "complete", "longjmp", "nativeHandle", "read",
        "siglongjmp", "withRead",
    }
    translated = translate_source(source) if translated is None else translated
    masked = translated.masked
    splice_boundaries = translated.splice_boundaries
    pairs = brace_pairs(masked)
    scope_bindings = scope_bindings_linear(masked, pairs)

    calls_by_receiver: dict[str, list[re.Match[str]]] = {}
    for call in MEMBER_CALL.finditer(masked):
        if call.group(1) not in {"read", "complete"}:
            continue
        receiver = receiver_binding_name(masked, call.start())
        if receiver is not None:
            calls_by_receiver.setdefault(receiver, []).append(call)

    gpu_names = {binding.declaration.name for binding in scope_bindings}
    all_tokens = _compiler_tokens(masked)
    known_types = declared_type_names(all_tokens)
    gpu_name_positions: set[int] = set()
    lexical_bindings: dict[str, list[tuple[int, int, bool]]] = {}
    for binding in scope_bindings:
        name_position = binding.declaration.name_position
        gpu_name_positions.add(name_position)
        lexical_bindings.setdefault(binding.declaration.name, []).append(
            (name_position, binding.block[1], True))

    shadow_tokens = [
        (index, token) for index, token in enumerate(all_tokens)
        if token.value in gpu_names and token.start not in gpu_name_positions
        and token_is_declarator_name(all_tokens, index, known_types)
    ]
    shadow_blocks = blocks_for_positions(
        masked, pairs, [token.start for _index, token in shadow_tokens])
    for _index, token in shadow_tokens:
        block = shadow_blocks.get(token.start)
        if block is not None:
            lexical_bindings.setdefault(token.value, []).append(
                (token.start, block[1], False))

    reconstructed_scope_operations: set[int] = set()
    for name, receiver_calls in calls_by_receiver.items():
        bindings = sorted(lexical_bindings.get(name, []))
        receiver_calls.sort(key=lambda call: call.start())
        active: list[tuple[int, bool]] = []
        binding_index = 0
        for call in receiver_calls:
            while binding_index < len(bindings) and bindings[binding_index][0] < call.start():
                start, end, is_gpu = bindings[binding_index]
                while active and active[-1][0] <= start:
                    active.pop()
                active.append((end, is_gpu))
                binding_index += 1
            while active and active[-1][0] <= call.start():
                active.pop()
            if active and active[-1][1]:
                reconstructed_scope_operations.add(call.start(1))

    findings: list[Finding] = []
    boundary_index = 0
    for token in _compiler_tokens(masked):
        while (boundary_index < len(splice_boundaries)
               and splice_boundaries[boundary_index] <= token.start):
            boundary_index += 1
        if token.value not in guarded:
            continue
        if (boundary_index >= len(splice_boundaries)
                or splice_boundaries[boundary_index] >= token.end):
            continue
        if (token.value in {"read", "complete"}
                and token.start not in reconstructed_scope_operations):
            continue
        source_line = translated.line_at(token.start)
        findings.append(Finding(
            path, source_line, "phase-two line splice",
            "phase-two line splice reconstructs a guarded GPU capability token"))
    return findings


def native_handle_declaration_or_internal_call(path: PurePosixPath, source: str,
                                               masked: str, token: CppToken,
                                               tokens: list[CppToken], index: int) -> bool:
    line_start = source.rfind("\n", 0, token.start) + 1
    line_end = source.find("\n", token.end)
    if line_end < 0:
        line_end = len(source)
    line = source[line_start:line_end]
    if re.search(r"\bvoid\s*\*\s*nativeHandle\s*\(\s*\)\s*const\b", line):
        return True
    following = tokens[index + 1].value if index + 1 < len(tokens) else ""
    previous = tokens[index - 1].value if index else ""
    return (path in SURFACE_INTERNAL_HANDLE_PATHS and following == "("
            and previous not in {".", "->", "::", "&"})


def enclosing_call(tokens: list[CppToken], use_index: int) -> tuple[str, int, int] | None:
    depth = 0
    for index in range(use_index - 1, -1, -1):
        value = tokens[index].value
        if value == ")":
            depth += 1
        elif value == "(":
            if depth:
                depth -= 1
                continue
            if index and re.fullmatch(r"[A-Za-z_]\w*", tokens[index - 1].value):
                closing_depth = 1
                for closing in range(index + 1, len(tokens)):
                    if tokens[closing].value == "(":
                        closing_depth += 1
                    elif tokens[closing].value == ")":
                        closing_depth -= 1
                        if closing_depth == 0:
                            return tokens[index - 1].value, index, closing
                return None
            # Transparent parentheses can wrap an otherwise exact argument or condition.
            continue
    return None


def strip_transparent_parentheses(tokens: list[CppToken], start: int,
                                  end: int) -> tuple[int, int]:
    while end - start >= 2 and tokens[start].value == "(" and tokens[end - 1].value == ")":
        depth = 0
        matching = None
        for index in range(start, end):
            if tokens[index].value == "(":
                depth += 1
            elif tokens[index].value == ")":
                depth -= 1
                if depth == 0:
                    matching = index
                    break
        if matching != end - 1:
            break
        start += 1
        end -= 1
    return start, end


def lexical_declaration_active(masked: str, pairs: list[tuple[int, int]],
                               declaration_position: int, use_position: int) -> bool:
    lexical_block = enclosing_block(pairs, declaration_position)
    parens = delimiter_pairs(masked, "(", ")")
    next_nonspace = next_nonspace_indices(masked)
    opening_blocks = {opening: (opening, closing) for opening, closing in pairs}
    containing_parens = sorted(
        (pair for pair in parens if pair[0] < declaration_position < pair[1]),
        key=lambda pair: pair[1] - pair[0])
    for _opening, closing in containing_parens:
        body_start = next_nonspace[min(closing + 1, len(masked))]
        if body_start in opening_blocks:
            lexical_block = opening_blocks[body_start]
            break
    if lexical_block is None:
        return declaration_position < use_position
    return lexical_block[0] < use_position < lexical_block[1]


def approved_call_is_unshadowed(tokens: list[CppToken], name_index: int,
                                allowed_member_calls: frozenset[str],
                                identity_tokens: list[CppToken] | None = None,
                                masked: str | None = None,
                                pairs: list[tuple[int, int]] | None = None) -> bool:
    name_token = tokens[name_index]
    name = name_token.value
    context = tokens if identity_tokens is None else identity_tokens
    context_index = next((index for index, token in enumerate(context)
                          if token.start == name_token.start), None)
    if context_index is None:
        return False
    if context_index and context[context_index - 1].value == "::":
        return False
    if context_index and context[context_index - 1].value in {".", "->"}:
        return name in allowed_member_calls

    def active(position: int) -> bool:
        if masked is None or pairs is None:
            return True
        return lexical_declaration_active(
            masked, pairs, position, name_token.start)

    known_types = declared_type_names(context)
    for index, token in enumerate(context[:context_index]):
        if (token.value == name and token_is_declarator_name(
                context, index, known_types)
                and active(token.start)):
            return False
        if token.value != name:
            continue
        boundary = index - 1
        while boundary >= 0 and context[boundary].value not in {";", "{", "}"}:
            boundary -= 1
        if ("using" in [item.value for item in context[boundary + 1:index + 1]]
                and active(token.start)):
            return False
    return True


def is_direct_call_argument(tokens: list[CppToken], use_start: int, use_end: int,
                            allowed_calls: frozenset[str],
                            allowed_member_calls: frozenset[str] = frozenset(),
                            identity_tokens: list[CppToken] | None = None,
                            masked: str | None = None,
                            pairs: list[tuple[int, int]] | None = None) -> bool:
    call = enclosing_call(tokens, use_start)
    if call is None or call[0] not in allowed_calls:
        return False
    _, opening, closing = call
    if not approved_call_is_unshadowed(
            tokens, opening - 1, allowed_member_calls, identity_tokens, masked, pairs):
        return False
    argument_start = opening + 1
    paren_depth = bracket_depth = brace_depth = 0
    for index in range(opening + 1, closing + 1):
        value = tokens[index].value
        if index == closing or (value == "," and paren_depth == bracket_depth == brace_depth == 0):
            if argument_start <= use_start < index:
                direct_start, direct_end = strip_transparent_parentheses(
                    tokens, argument_start, index)
                return direct_start == use_start and direct_end == use_end
            argument_start = index + 1
            continue
        if value == "(":
            paren_depth += 1
        elif value == ")":
            paren_depth -= 1
        elif value == "[":
            bracket_depth += 1
        elif value == "]":
            bracket_depth -= 1
        elif value == "{":
            brace_depth += 1
        elif value == "}":
            brace_depth -= 1
    return False


def safe_boolean_expression(tokens: list[CppToken], start: int, end: int,
                            alias: str, allow_bare: bool = True) -> bool:
    start, end = strip_transparent_parentheses(tokens, start, end)
    depth = 0
    logical_operators: list[int] = []
    for index in range(start, end):
        value = tokens[index].value
        if value == "(":
            depth += 1
        elif value == ")":
            depth -= 1
        elif depth == 0 and value in {"&&", "||"}:
            logical_operators.append(index)
    if logical_operators:
        boundaries = [start, *[index + 1 for index in logical_operators], end]
        ends = [*logical_operators, end]
        found_alias = False
        for part_start, part_end in zip(boundaries, ends):
            if not any(token.value == alias for token in tokens[part_start:part_end]):
                continue
            found_alias = True
            if not safe_boolean_expression(tokens, part_start, part_end, alias, False):
                return False
        return found_alias
    if end - start == 1 and tokens[start].value == alias:
        return allow_bare
    if start < end and tokens[start].value == "!":
        operand_start, operand_end = strip_transparent_parentheses(tokens, start + 1, end)
        return (operand_end - operand_start == 1
                and tokens[operand_start].value == alias)
    depth = 0
    comparison = None
    for index in range(start, end):
        value = tokens[index].value
        if value == "(":
            depth += 1
        elif value == ")":
            depth -= 1
        elif depth == 0 and value in {"==", "!="}:
            if comparison is not None:
                return False
            comparison = index
    if comparison is None:
        return False
    left_start, left_end = strip_transparent_parentheses(tokens, start, comparison)
    right_start, right_end = strip_transparent_parentheses(tokens, comparison + 1, end)
    left = [token.value for token in tokens[left_start:left_end]]
    right = [token.value for token in tokens[right_start:right_end]]
    safe_null = ([alias], ["nullptr"]), ([alias], ["0"]), (["nullptr"], [alias]), (["0"], [alias])
    return (left, right) in safe_null


def statement_boolean_use(masked: str, pairs: list[tuple[int, int]], position: int,
                          alias: str) -> bool:
    tokens = statement_tokens(masked, pairs, position)
    use = next((index for index, token in enumerate(tokens)
                if token.start == position), None)
    if use is None:
        return False
    end = len(tokens) - 1 if tokens and tokens[-1].value == ";" else len(tokens)
    depth = 0
    equals = None
    for index in range(use - 1, -1, -1):
        value = tokens[index].value
        if value == ")":
            depth += 1
        elif value == "(":
            depth -= 1
        elif depth == 0 and value == "=":
            equals = index
            break
    return equals is not None and safe_boolean_expression(tokens, equals + 1, end, alias)


def is_lambda_body(masked: str, block: tuple[int, int]) -> bool:
    tokens = _compiler_tokens(masked, 0, block[0])
    bracket_stack: list[int] = []
    bracket_pairs: list[tuple[int, int]] = []
    for index, token in enumerate(tokens):
        if token.value == "[":
            bracket_stack.append(index)
        elif token.value == "]" and bracket_stack:
            bracket_pairs.append((bracket_stack.pop(), index))
    allowed_predecessors = {
        "!", "!=", "%", "%=", "&", "&&", "(", "*", "+", ",", "-", "/",
        ":", ";", "<", "=", "==", ">", "?", "[", "^", "co_return", "return",
        "throw", "{", "|", "||", "~",
    }
    for capture_open, capture_close in bracket_pairs:
        previous = tokens[capture_open - 1].value if capture_open else None
        if previous is not None and previous not in allowed_predecessors:
            continue
        # A statement terminator or an earlier body after the capture means this block
        # belongs to later synchronous code, not to that lambda introducer.
        if any(token.value in {";", "{", "}"}
               for token in tokens[capture_close + 1:]):
            continue
        return True
    return False


def stays_in_synchronous_blocks(masked: str, pairs: list[tuple[int, int]],
                                outer: tuple[int, int], position: int) -> bool:
    if not (outer[0] < position < outer[1]):
        return False
    return not any(
        outer[0] < block[0] < position < block[1] <= outer[1]
        and is_lambda_body(masked, block)
        for block in pairs
    )


def type_name_is_source_shadowed(masked: str, pairs: list[tuple[int, int]],
                                 type_name: str, position: int) -> bool:
    prior_tokens = _compiler_tokens(masked, 0, position)
    for index, token in enumerate(prior_tokens):
        if token.value != type_name:
            continue
        boundary = index - 1
        while boundary >= 0 and prior_tokens[boundary].value not in {";", "{", "}"}:
            boundary -= 1
        statement = [item.value for item in prior_tokens[boundary + 1:index + 2]]
        if ("using" in statement or "typedef" in statement
                or any(value in {"class", "enum", "struct", "union"}
                       for value in statement)):
            if lexical_declaration_active(masked, pairs, token.start, position):
                return True
    return False


def local_native_alias(path: PurePosixPath, masked: str, pairs: list[tuple[int, int]],
                       call: re.Match[str]) -> tuple[str, int] | None:
    tokens = statement_tokens(masked, pairs, call.start())
    values = [token.value for token in tokens]
    call_token = next((index for index, token in enumerate(tokens)
                       if token.start == call.start(1)), None)
    if call_token is None:
        return None
    equals = [index for index in range(call_token) if tokens[index].value == "="]
    if len(equals) != 1:
        return None
    equals_index = equals[0]
    lhs = tokens[:equals_index]
    if len(lhs) < 2:
        return None
    alias = lhs[-1]
    if alias is None or alias.value in {"const", "auto", "void"}:
        return None
    if not re.fullmatch(r"[A-Za-z_]\w*", alias.value):
        return None
    declaration = [token.value for token in lhs[:-1]]
    while declaration and declaration[0] in {"const", "volatile"}:
        declaration.pop(0)
    while declaration and declaration[-1] in {"const", "volatile"}:
        declaration.pop()
    if declaration not in (["void", "*"], ["auto", "*"], ["IOSurfaceRef"]):
        return None
    if (declaration == ["IOSurfaceRef"]
            and type_name_is_source_shadowed(
                masked, pairs, "IOSurfaceRef", alias.start)):
        return None

    rhs = values[equals_index + 1:]
    direct = (len(rhs) == 6 and re.fullmatch(r"[A-Za-z_]\w*", rhs[0])
              and rhs[1:] == [".", "nativeHandle", "(", ")", ";"])
    approved_cast = False
    cast_type: list[str] = []
    if rhs and rhs[0] in {"const_cast", "dynamic_cast", "reinterpret_cast", "static_cast"}:
        try:
            cast_close = len(rhs) - 1 - rhs[::-1].index(">")
        except ValueError:
            cast_close = -1
        approved_cast = (
            cast_close >= 3
            and rhs[1] == "<"
            and len(rhs) == cast_close + 9
            and rhs[cast_close + 1:] == ["(", rhs[cast_close + 2], ".", "nativeHandle",
                                         "(", ")", ")", ";"]
            and re.fullmatch(r"[A-Za-z_]\w*", rhs[cast_close + 2]) is not None
            and all(value not in {"(", ")", "{", "}", "[", "]", "=", ",", "?", ":"}
                    for value in rhs[2:cast_close])
        )
        cast_type = rhs[2:cast_close]
    if approved_cast:
        expected_cast = None
        if declaration == ["IOSurfaceRef"]:
            expected_cast = ["IOSurfaceRef"]
        elif declaration == ["auto", "*"] and path in {
                PurePosixPath("recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"),
                PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
        }:
            expected_cast = ["ID3D11Texture2D", "*"]
        approved_cast = cast_type == expected_cast
        if (approved_cast and cast_type and type_name_is_source_shadowed(
                masked, pairs, cast_type[0], alias.start)):
            approved_cast = False
    if not direct and not approved_cast:
        return None
    return alias.value, alias.start


def native_handle_has_direct_consumer(path: PurePosixPath, masked: str,
                                      pairs: list[tuple[int, int]], call: re.Match[str],
                                      binding: HandleBinding) -> bool:
    safe_calls = REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
    safe_member_calls = REVIEWED_NATIVE_HANDLE_MEMBER_SINKS.get(path, frozenset())
    if not safe_calls:
        return False
    tokens = _compiler_tokens(masked, binding.block[0] + 1, binding.block[1])
    identity_tokens = _compiler_tokens(masked, 0, binding.block[1])
    native_index = next((index for index, token in enumerate(tokens)
                         if token.start == call.start(1)), None)
    if native_index is None or native_index < 2 or native_index + 2 >= len(tokens):
        return False
    if [token.value for token in tokens[native_index - 2:native_index + 3]] != [
            binding.name, ".", "nativeHandle", "(", ")"]:
        return False
    return is_direct_call_argument(
        tokens, native_index - 2, native_index + 3, safe_calls, safe_member_calls,
        identity_tokens, masked, pairs)


def native_alias_stays_synchronous(path: PurePosixPath, masked: str,
                                   pairs: list[tuple[int, int]], binding: HandleBinding, alias: str,
                                   declaration_position: int) -> tuple[bool, int]:
    tokens = _compiler_tokens(masked, declaration_position, binding.block[1])
    safe_calls = REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
    safe_member_calls = REVIEWED_NATIVE_HANDLE_MEMBER_SINKS.get(path, frozenset())
    safe_methods = REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())
    alias_block = immediate_block(pairs, declaration_position)
    if alias_block is None:
        return False, declaration_position
    last_use = declaration_position
    consumed = False
    tokens = _compiler_tokens(masked, alias_block[0] + 1, alias_block[1])
    identity_tokens = _compiler_tokens(masked, 0, alias_block[1])
    for index, token in enumerate(tokens):
        if token.value != alias or token.start == declaration_position:
            continue
        if not stays_in_synchronous_blocks(masked, pairs, alias_block, token.start):
            return False, max(last_use, token.start)
        previous = tokens[index - 1].value if index else ""
        following = tokens[index + 1].value if index + 1 < len(tokens) else ""
        if previous in {".", "->", "::"}:
            continue
        last_use = max(last_use, token.start)
        if following == "->" and index + 2 < len(tokens):
            if (tokens[index + 2].value in safe_methods
                    and previous not in {"=", ",", "?", ":"}):
                consumed = True
                continue
            return False, last_use
        if is_direct_call_argument(
                tokens, index, index + 1, safe_calls, safe_member_calls,
                identity_tokens, masked, pairs):
            consumed = True
            continue
        condition = enclosing_call(tokens, index)
        if condition is not None and condition[0] in {"if", "while"}:
            if safe_boolean_expression(tokens, condition[1] + 1, condition[2], alias):
                consumed = True
                continue
            return False, last_use
        if statement_boolean_use(masked, pairs, token.start, alias):
            consumed = True
            continue
        if following == "?" or previous in {"=", ","}:
            return False, last_use
        return False, last_use
    return consumed, last_use


@dataclass(frozen=True)
class ScopeBinding:
    declaration: ScopeDeclaration
    block: tuple[int, int]


def blocks_for_positions(masked: str, pairs: list[tuple[int, int]],
                         positions: list[int]) -> dict[int, tuple[int, int] | None]:
    """Assign positions to their innermost paired interval in one source pass."""
    if not positions:
        return {}
    opening_pairs = {opening: (opening, closing) for opening, closing in pairs}
    closing_pairs = {closing: (opening, closing) for opening, closing in pairs}
    ordered = sorted(set(positions))
    assigned: dict[int, tuple[int, int] | None] = {}
    stack: list[tuple[int, int]] = []
    position_index = 0
    for index in range(len(masked)):
        while position_index < len(ordered) and ordered[position_index] == index:
            assigned[index] = stack[-1] if stack else None
            position_index += 1
        if index in opening_pairs:
            stack.append(opening_pairs[index])
        elif index in closing_pairs and stack and stack[-1][1] == index:
            stack.pop()
    while position_index < len(ordered):
        assigned[ordered[position_index]] = stack[-1] if stack else None
        position_index += 1
    return assigned


def delimiter_pairs(masked: str, opening: str, closing: str) -> list[tuple[int, int]]:
    stack: list[int] = []
    pairs: list[tuple[int, int]] = []
    for index, char in enumerate(masked):
        if char == opening:
            stack.append(index)
        elif char == closing and stack:
            pairs.append((stack.pop(), index))
    return pairs


def next_nonspace_indices(masked: str) -> list[int]:
    following = len(masked)
    result = [following] * (len(masked) + 1)
    for index in range(len(masked) - 1, -1, -1):
        if not masked[index].isspace():
            following = index
        result[index] = following
    return result


def scope_bindings_linear(
    masked: str,
    pairs: list[tuple[int, int]] | tuple[tuple[int, int], ...],
    *,
    next_nonspace: list[int] | tuple[int, ...] | None = None,
) -> list[ScopeBinding]:
    declarations = scope_declarations(masked)
    assigned = blocks_for_positions(
        masked, pairs, [declaration.position for declaration in declarations])
    opening_pairs = {opening: (opening, closing) for opening, closing in pairs}
    paren_pairs = delimiter_pairs(masked, "(", ")")
    declaration_parens = blocks_for_positions(
        masked, paren_pairs, [declaration.position for declaration in declarations])
    next_nonspace = (
        next_nonspace_indices(masked) if next_nonspace is None else next_nonspace
    )
    bindings: list[ScopeBinding] = []
    for declaration in declarations:
        lexical_block = assigned.get(declaration.position)
        following_index = next_nonspace[min(declaration.end, len(masked))]
        following = masked[following_index] if following_index < len(masked) else ""
        parameter_paren = declaration_parens.get(declaration.position)
        if following in {")", ","} and parameter_paren is not None:
            signature_end = parameter_paren[1]
            body_start = next_nonspace[min(signature_end + 1, len(masked))]
            if body_start in opening_pairs:
                lexical_block = opening_pairs[body_start]
        if lexical_block is not None:
            bindings.append(ScopeBinding(declaration, lexical_block))
    return bindings


@dataclass(frozen=True)
class HandleBinding:
    name: str
    position: int
    block: tuple[int, int]
    scope_position: int | None
    read_position: int | None


def receiver_expression(masked: str, call_position: int) -> str:
    """Return the balanced expression immediately left of a member call."""
    index = call_position - 1
    while index >= 0 and masked[index].isspace():
        index -= 1
    end = index + 1
    closing_to_opening = {")": "(", "]": "[", "}": "{"}
    # Include angle brackets so named casts and template-id receivers remain a
    # single expression. Parenthesized argument lists are consumed as balanced
    # units, so this does not absorb an earlier relational expression.
    expression_chars = frozenset("._:<->*&")
    consumed = False
    braced_initializer = False
    while index >= 0:
        char = masked[index]
        if char in closing_to_opening:
            closing = char
            opening = closing_to_opening[closing]
            depth = 1
            index -= 1
            while index >= 0 and depth:
                if masked[index] == closing:
                    depth += 1
                elif masked[index] == opening:
                    depth -= 1
                index -= 1
            consumed = True
            braced_initializer = opening == "{"
            continue
        if char.isalnum() or char in expression_chars:
            consumed = True
            braced_initializer = False
            index -= 1
            continue
        if char.isspace() and consumed and braced_initializer:
            prior = index
            while prior >= 0 and masked[prior].isspace():
                prior -= 1
            if prior >= 0 and (masked[prior].isalnum() or masked[prior] == "_"):
                index = prior
                continue
        break
    return masked[index + 1:end]


def receiver_binding_name(masked: str, call_position: int) -> str | None:
    """Resolve the object binding selected by a member-call receiver.

    The resolver deliberately models only value-preserving receiver forms used
    by the capability audit: redundant parentheses, the comma operator's final
    operand, pointer/reference dereference, named casts, ``std::as_const``,
    ``std::move``/``std::forward``, and ``std::ref(...).get()``.
    Unknown calls and operators remain unresolved rather than being guessed
    from identifiers appearing anywhere in the expression.
    """
    tokens = _compiler_tokens(receiver_expression(masked, call_position))

    def documented_std_call(
        start: int, end: int, allowed: frozenset[str],
    ) -> tuple[int, int] | None:
        opening = next((index for index in range(start, end)
                        if tokens[index].value == "("), None)
        if opening is None or tokens[end - 1].value != ")":
            return None
        function_index = opening - 1
        if (function_index >= start
                and tokens[function_index].value in {">", ">>"}):
            depth = len(tokens[function_index].value)
            function_index -= 1
            while function_index >= start and depth:
                if tokens[function_index].value in {">", ">>"}:
                    depth += len(tokens[function_index].value)
                elif tokens[function_index].value in {"<", "<<"}:
                    depth -= len(tokens[function_index].value)
                function_index -= 1
        if function_index < start or tokens[function_index].value not in allowed:
            return None
        prefix = [token.value for token in tokens[start:function_index]]
        if prefix not in (["std", "::"], ["::", "std", "::"]):
            return None
        return opening + 1, end - 1

    def resolve(start: int, end: int) -> str | None:
        start, end = strip_transparent_parentheses(tokens, start, end)
        depth = 0
        comma = None
        for token_index in range(start, end):
            value = tokens[token_index].value
            if value == "(":
                depth += 1
            elif value == ")":
                depth -= 1
            elif depth == 0 and value == ",":
                comma = token_index
        if comma is not None:
            return resolve(comma + 1, end)
        if (end - start == 2 and tokens[start].value in {"*", "&"}
                and re.fullmatch(r"[A-Za-z_]\w*", tokens[start + 1].value)):
            return tokens[start + 1].value
        if (end - start >= 7 and tokens[start].value in {
                "const_cast", "dynamic_cast", "reinterpret_cast", "static_cast"}
                and tokens[start + 1].value == "<"):
            angle_close = next((index for index in range(start + 2, end)
                                if tokens[index].value == ">"), None)
            if (angle_close is not None and angle_close + 1 < end
                    and tokens[angle_close + 1].value == "("):
                return resolve(angle_close + 1, end)
        if (end - start >= 5 and [token.value for token in tokens[end - 4:end]]
                == [".", "get", "(", ")"]):
            factory_argument = documented_std_call(
                start, end - 4, frozenset({"cref", "ref"})
            )
            if factory_argument is not None:
                return resolve(*factory_argument)
            return None
        std_argument = documented_std_call(
            start, end,
            frozenset({"as_const", "cref", "forward", "move", "ref"}),
        )
        if std_argument is not None:
            return resolve(*std_argument)
        if (end - start == 1
                and re.fullmatch(r"[A-Za-z_]\w*", tokens[start].value)):
            return tokens[start].value
        return None

    return resolve(0, len(tokens))


def receiver_binding_references(masked: str, call_position: int) -> set[str]:
    """Return unqualified value names conservatively referenced by a receiver."""
    receiver = receiver_expression(masked, call_position)
    tokens = _compiler_tokens(receiver)
    receiver_start = call_position - len(receiver.rstrip())
    selector_prefix = masked[max(0, receiver_start - 64):receiver_start]
    dependent_member_receiver = (
        receiver_start >= 0
        and re.search(r"(?:\.|->|::)\s*template\s*$",
                      selector_prefix) is not None)
    references: set[str] = set()
    for index, token in enumerate(tokens):
        if re.fullmatch(r"[A-Za-z_]\w*", token.value) is None:
            continue
        previous = tokens[index - 1].value if index else ""
        following = tokens[index + 1].value if index + 1 < len(tokens) else ""
        dependent_selector = (
            previous == "template" and index >= 2
            and tokens[index - 2].value in {".", "->", "::"})
        if (previous in {".", "->", "::"} or following == "::"
                or dependent_selector or (dependent_member_receiver and index == 0)):
            continue
        references.add(token.value)
    return references


def matching_delimiter(masked: str, opening: int, opener: str, closer: str) -> int | None:
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == opener:
            depth += 1
        elif masked[index] == closer:
            depth -= 1
            if depth == 0:
                return index
    return None


def immediate_block(pairs: list[tuple[int, int]], position: int) -> tuple[int, int] | None:
    return enclosing_block(pairs, position)


def statement_tokens(masked: str, pairs: list[tuple[int, int]], position: int) -> list[CppToken]:
    """Return the semicolon-terminated statement containing position.

    This deliberately only accepts a statement in the call's immediate lexical
    block. A call hidden in a control statement, lambda, initializer, or nested
    body therefore retains those surrounding tokens and cannot resemble the
    canonical capability grammar.
    """
    block = immediate_block(pairs, position)
    if block is None:
        return []
    tokens = _compiler_tokens(masked, block[0] + 1, block[1])
    call_index = next((index for index, token in enumerate(tokens)
                       if token.start <= position < token.end), None)
    if call_index is None:
        return []

    start_index = 0
    paren_depth = 0
    bracket_depth = 0
    brace_depth = 0
    for index in range(call_index):
        value = tokens[index].value
        if value == "(":
            paren_depth += 1
        elif value == ")":
            paren_depth = max(0, paren_depth - 1)
        elif value == "[":
            bracket_depth += 1
        elif value == "]":
            bracket_depth = max(0, bracket_depth - 1)
        elif value == "{":
            brace_depth += 1
        elif value == "}":
            brace_depth = max(0, brace_depth - 1)
            if paren_depth == bracket_depth == brace_depth == 0:
                start_index = index + 1
        elif value == ";" and paren_depth == bracket_depth == brace_depth == 0:
            start_index = index + 1

    paren_depth = bracket_depth = brace_depth = 0
    for index in range(start_index, len(tokens)):
        value = tokens[index].value
        if value == "(":
            paren_depth += 1
        elif value == ")":
            paren_depth = max(0, paren_depth - 1)
        elif value == "[":
            bracket_depth += 1
        elif value == "]":
            bracket_depth = max(0, bracket_depth - 1)
        elif value == "{":
            brace_depth += 1
        elif value == "}":
            brace_depth = max(0, brace_depth - 1)
        elif value == ";" and paren_depth == bracket_depth == brace_depth == 0:
            return tokens[start_index:index + 1]
    return []


def call_closing_token(tokens: list[CppToken], call_name_index: int) -> int | None:
    opening = call_name_index + 1
    if opening >= len(tokens) or tokens[opening].value != "(":
        return None
    depth = 0
    for index in range(opening, len(tokens)):
        if tokens[index].value == "(":
            depth += 1
        elif tokens[index].value == ")":
            depth -= 1
            if depth == 0:
                return index
    return None


def canonical_scope_declaration(masked: str, pairs: list[tuple[int, int]],
                                binding: ScopeBinding) -> bool:
    tokens = statement_tokens(masked, pairs, binding.declaration.position)
    return [token.value for token in tokens] == [
        "GpuSyncReadScope", binding.declaration.name, ";"
    ]


def canonical_read_lease(masked: str, pairs: list[tuple[int, int]], call: re.Match[str],
                         scope_name: str) -> tuple[str, int] | None:
    tokens = statement_tokens(masked, pairs, call.start())
    values = [token.value for token in tokens]
    if len(tokens) < 10 or values[:4] != ["const", "GpuReadLease", values[2], "="]:
        return None
    lease_name = values[2]
    if not re.fullmatch(r"[A-Za-z_]\w*", lease_name):
        return None
    if values[4:8] != [scope_name, ".", "read", "("]:
        return None
    close = call_closing_token(tokens, 6)
    if close is None or values[close:] != [")", ";"]:
        return None
    return lease_name, tokens[2].start


def standalone_completion(masked: str, pairs: list[tuple[int, int]], call: re.Match[str],
                          scope_name: str) -> bool:
    return [token.value for token in statement_tokens(masked, pairs, call.start())] == [
        scope_name, ".", "complete", "(", ")", ";"
    ]


CONTROL_TRANSFER_TOKENS = frozenset({
    "_longjmp", "break", "co_return", "continue", "goto", "longjmp", "return",
    "siglongjmp", "throw",
})


def has_intervening_control_transfer(masked: str, start: int, end: int) -> bool:
    return any(
        token.value in CONTROL_TRANSFER_TOKENS
        for token in _compiler_tokens(masked, start, end)
    )


def has_intervening_potentially_throwing_call(masked: str, start: int, end: int) -> bool:
    tokens = _compiler_tokens(masked, start, end)
    allowed = {
        "alignof", "const_cast", "decltype", "dynamic_cast", "nativeHandle", "read",
        "reinterpret_cast", "sizeof", "static_cast",
    }
    control = {"catch", "for", "if", "switch", "while"}
    for index, token in enumerate(tokens[:-1]):
        if (not re.fullmatch(r"[A-Za-z_]\w*", token.value)
                or tokens[index + 1].value != "("):
            continue
        if token.value not in allowed and token.value not in control:
            return True
    return False


def resolve_scope(scope_bindings: list[ScopeBinding] | Mapping[str, Iterable[ScopeBinding]], masked: str,
                  pairs: list[tuple[int, int]],
                  call: re.Match[str], shadow_index=None) -> ScopeBinding | None:
    receiver = receiver_binding_name(masked, call.start())
    available = (scope_bindings.get(receiver, ())
                 if isinstance(scope_bindings, Mapping) and receiver is not None
                 else scope_bindings if not isinstance(scope_bindings, Mapping) else ())
    candidates = [
        binding for binding in available
        if binding.declaration.position <= call.start() < binding.block[1]
        and binding.block[0] < call.start()
        and receiver == binding.declaration.name
        and not name_is_shadowed(binding.declaration.name, binding.declaration.position,
                                 binding.declaration.end, masked, pairs, call.start(),
                                 shadow_index)
    ]
    if not candidates:
        return None
    return min(candidates,
               key=lambda binding: (binding.block[1] - binding.block[0],
                                    -binding.declaration.position))


def withread_callback_block(masked: str, pairs: list[tuple[int, int]],
                            call: re.Match[str]) -> tuple[int, int] | None:
    opening = call.end() - 1
    closing = matching_delimiter(masked, opening, "(", ")")
    if closing is None:
        return None

    separators: list[int] = []
    paren_depth = bracket_depth = brace_depth = 0
    for index in range(opening + 1, closing):
        value = masked[index]
        if value == "(":
            paren_depth += 1
        elif value == ")":
            paren_depth = max(0, paren_depth - 1)
        elif value == "[":
            bracket_depth += 1
        elif value == "]":
            bracket_depth = max(0, bracket_depth - 1)
        elif value == "{":
            brace_depth += 1
        elif value == "}":
            brace_depth = max(0, brace_depth - 1)
        elif value == "," and paren_depth == bracket_depth == brace_depth == 0:
            separators.append(index)
    if len(separators) != 1:
        return None

    callback_start = separators[0] + 1
    body = masked[callback_start:closing]
    lambda_pattern = re.compile(
        r"^\s*\[[^\]]*\]\s*\(\s*const\s+GpuReadLease\s*&"
        r"(?:\s*[A-Za-z_]\w*)?\s*\)\s*(?:mutable\s*)?(?:noexcept\s*)?"
        r"(?:->[^{}]+)?\{")
    match = lambda_pattern.search(body)
    if match is None:
        return None
    body_opening = callback_start + match.end() - 1
    block = next((pair for pair in pairs if pair[0] == body_opening), None)
    if block is None or block[1] > closing or masked[block[1] + 1:closing].strip():
        return None
    return block


def callback_lease_bindings(masked: str, pairs: list[tuple[int, int]],
                            call: re.Match[str]) -> list[HandleBinding]:
    opening = call.end() - 1
    closing = matching_delimiter(masked, opening, "(", ")")
    if closing is None:
        return []

    separators: list[int] = []
    paren_depth = bracket_depth = brace_depth = 0
    for index in range(opening + 1, closing):
        value = masked[index]
        if value == "(":
            paren_depth += 1
        elif value == ")":
            paren_depth = max(0, paren_depth - 1)
        elif value == "[":
            bracket_depth += 1
        elif value == "]":
            bracket_depth = max(0, bracket_depth - 1)
        elif value == "{":
            brace_depth += 1
        elif value == "}":
            brace_depth = max(0, brace_depth - 1)
        elif value == "," and paren_depth == bracket_depth == brace_depth == 0:
            separators.append(index)
    if len(separators) != 1:
        return []

    callback_start = separators[0] + 1
    body = masked[callback_start:closing]
    lambda_pattern = re.compile(
        r"^\s*\[[^\]]*\]\s*\(\s*const\s+GpuReadLease\s*&\s*"
        r"([A-Za-z_]\w*)\s*\)\s*(?:mutable\s*)?(?:noexcept\s*)?"
        r"(?:->[^{}]+)?\{")
    bindings: list[HandleBinding] = []
    match = lambda_pattern.search(body)
    if match is None:
        return bindings
    body_opening = callback_start + match.end() - 1
    block = next((pair for pair in pairs if pair[0] == body_opening), None)
    if block is None or block[1] > closing or masked[block[1] + 1:closing].strip():
        return bindings
    bindings.append(HandleBinding(match.group(1), callback_start + match.start(1), block,
                                  None, None))
    return bindings


def resolve_handle_binding(bindings: list[HandleBinding], masked: str,
                           pairs: list[tuple[int, int]],
                           call: re.Match[str]) -> HandleBinding | None:
    receiver = receiver_expression(masked, call.start()).strip()
    candidates = [
        binding for binding in bindings
        if binding.position <= call.start() < binding.block[1]
        and binding.block[0] < call.start()
        and receiver == binding.name
        and stays_in_synchronous_blocks(masked, pairs, binding.block, call.start())
    ]
    if not candidates:
        return None
    return min(candidates,
               key=lambda binding: (binding.block[1] - binding.block[0], -binding.position))


def declared_type_names(tokens: list[CppToken]) -> frozenset[str]:
    """Collect source-proven type names needed to disambiguate declarators."""
    names = {
        "auto", "bool", "char", "char16_t", "char32_t", "double", "float",
        "int", "long", "short", "signed", "unsigned", "void", "wchar_t",
    }
    for index, token in enumerate(tokens[:-1]):
        following = tokens[index + 1]
        if (token.value in {"class", "enum", "struct", "typename", "union"}
                and re.fullmatch(r"[A-Za-z_]\w*", following.value)):
            names.add(following.value)
        if (token.value == "using" and index + 2 < len(tokens)
                and re.fullmatch(r"[A-Za-z_]\w*", following.value)
                and tokens[index + 2].value == "="):
            names.add(following.value)
    return frozenset(names)


def token_is_declarator_name(tokens: list[CppToken], index: int,
                             known_types: frozenset[str] | None = None) -> bool:
    """Conservatively recognize a local/parameter declarator at tokens[index]."""
    if index == 0 or index + 1 >= len(tokens):
        return False
    following = tokens[index + 1].value
    if following not in {";", "=", "(", ")", "{", "[", "]", ",", ":"}:
        return False
    previous = tokens[index - 1].value
    known_types = declared_type_names(tokens) if known_types is None else known_types
    redundant_parentheses = (
        previous == "(" and following == ")" and index >= 2
        and tokens[index - 2].value in known_types
        and (index + 2 >= len(tokens)
             or tokens[index + 2].value in {";", "=", ",", ")", "{"})
    )
    binding_open = index - 1 if previous == "[" else next(
        (cursor for cursor in range(index - 1, -1, -1)
         if tokens[cursor].value in {"[", ";", "{", "}"}), -1)
    prefix = binding_open - 1
    while prefix >= 0 and tokens[prefix].value in {"&", "&&", "const", "volatile"}:
        prefix -= 1
    structured_binding = (
        previous in {"[", ","} and following in {"]", ","}
        and binding_open >= 0 and tokens[binding_open].value == "["
        and prefix >= 0 and tokens[prefix].value == "auto"
        and (prefix == 0 or tokens[prefix - 1].value in {"(", ";", "{", "}"})
    )
    boundary = index - 1
    while boundary >= 0 and tokens[boundary].value not in {";", "{", "}"}:
        boundary -= 1
    declaration_prefix = [token.value for token in tokens[boundary + 1:index]]
    comma_declarator = (
        previous == "," and len(declaration_prefix) >= 3
        and re.fullmatch(r"[A-Za-z_]\w*", declaration_prefix[0]) is not None
        and re.fullmatch(r"[A-Za-z_]\w*", declaration_prefix[1]) is not None
        and "(" not in declaration_prefix and "[" not in declaration_prefix
    )
    if redundant_parentheses or structured_binding or comma_declarator:
        return True
    if previous in {".", "->", "::", "(", "[", "=", ",", "?", ":"}:
        return False
    if following == ")" and previous in {"*", "&", "&&"}:
        opening = index - 2
        if (opening < 1 or tokens[opening].value != "("
                or not (re.fullmatch(r"[A-Za-z_]\w*", tokens[opening - 1].value)
                        or tokens[opening - 1].value in {">", ")", "*", "&"})):
            return False

    boundary = index - 1
    paren_depth = bracket_depth = 0
    while boundary >= 0:
        value = tokens[boundary].value
        if value == ")":
            paren_depth += 1
        elif value == "(":
            if paren_depth:
                paren_depth -= 1
            elif boundary > 0 and tokens[boundary - 1].value not in {"decltype", "sizeof"}:
                break
        elif value == "]":
            bracket_depth += 1
        elif value == "[":
            if bracket_depth:
                bracket_depth -= 1
            else:
                break
        elif paren_depth == bracket_depth == 0 and value in {";", "{", "}"}:
            break
        boundary -= 1
    prefix = [token.value for token in tokens[boundary + 1:index]]
    if not prefix:
        return False
    if prefix[0] in {
            "break", "case", "continue", "delete", "else", "goto", "if", "new",
            "return", "switch", "throw", "while"}:
        return False
    if any(value in {".", "->", "?", "||", "&&", "+", "-", "/", "%"}
           for value in prefix):
        return False
    if previous == ")" and "decltype" not in prefix:
        return False
    return (previous in {"*", "&", "&&", ">", ")"}
            or bool(re.fullmatch(r"[A-Za-z_]\w*", previous)))


def lambda_init_capture_shadows(masked: str, pairs: list[tuple[int, int]], name: str,
                                call_position: int) -> bool:
    for opening, closing in pairs:
        if not (opening < call_position < closing):
            continue
        capture_close = masked.rfind("]", 0, opening)
        capture_open = masked.rfind("[", 0, capture_close) if capture_close >= 0 else -1
        if capture_open < 0 or capture_close < 0:
            continue
        suffix = masked[capture_close + 1:opening]
        if not re.fullmatch(
                r"\s*(?:\([^{};]*\)\s*)?(?:mutable\s*)?(?:noexcept\s*)?"
                r"(?:->\s*[^{};]+)?", suffix):
            continue
        capture_tokens = _compiler_tokens(masked, capture_open + 1, capture_close)
        for index, token in enumerate(capture_tokens[:-1]):
            if token.value == name and capture_tokens[index + 1].value == "=":
                return True
    return False


def build_shadow_index(
    masked: str,
    pairs: list[tuple[int, int]] | tuple[tuple[int, int], ...],
    *,
    tokens: list[CppToken] | tuple[CppToken, ...] | None = None,
) -> dict[str, tuple[tuple[int, tuple[int, int] | None], ...]]:
    tokens = _compiler_tokens(masked) if tokens is None else tokens
    known_types = declared_type_names(tokens)
    declarators = [
        token for index, token in enumerate(tokens)
        if re.fullmatch(r"[A-Za-z_]\w*", token.value)
        and token_is_declarator_name(tokens, index, known_types)
    ]
    blocks = blocks_for_positions(masked, pairs, [token.start for token in declarators])
    mutable: dict[str, list[tuple[int, tuple[int, int] | None]]] = {}
    for token in declarators:
        mutable.setdefault(token.value, []).append((token.start, blocks.get(token.start)))
    return {name: tuple(entries) for name, entries in mutable.items()}


@dataclass(frozen=True, slots=True)
class CompilerAuditAnalysis:
    """One immutable parse shared by provenance and every compiler policy key."""

    translated: TranslationText
    masked: str
    tokens: tuple[CppToken, ...]
    has_scope_grammar: bool
    token_starts: tuple[int, ...]
    delimiter_pairs: tuple[tuple[int, int], ...]
    next_nonspace: tuple[int, ...]
    scope_bindings: tuple[ScopeBinding, ...]
    scopes_by_name: Mapping[str, tuple[ScopeBinding, ...]]
    blocks: Mapping[int, tuple[int, int] | None]
    shadow_index: Mapping[
        str, tuple[tuple[int, tuple[int, int] | None], ...]
    ]
    member_calls: tuple[re.Match[str], ...]

    @classmethod
    def from_translation(cls, translated: TranslationText) -> "CompilerAuditAnalysis":
        masked = translated.masked
        tokens = tuple(cpp_tokens(masked))
        pairs = tuple(brace_pairs(masked))
        member_calls = tuple(MEMBER_CALL.finditer(masked))
        has_scope_grammar = (
            any(call.group(1) in _SCOPE_MEMBER_METHODS for call in member_calls)
            or any(token.value == "GpuSyncReadScope" for token in tokens)
        )
        if has_scope_grammar:
            next_nonspace = tuple(next_nonspace_indices(masked))
            bindings = tuple(scope_bindings_linear(
                masked, pairs, next_nonspace=next_nonspace
            ))
            mutable_scopes: dict[str, list[ScopeBinding]] = {}
            for binding in bindings:
                mutable_scopes.setdefault(binding.declaration.name, []).append(binding)
            scopes = MappingProxyType({
                name: tuple(values) for name, values in mutable_scopes.items()
            })
            blocks = MappingProxyType(blocks_for_positions(
                masked, pairs, [binding.declaration.position for binding in bindings]
            ))
            shadow = MappingProxyType(build_shadow_index(
                masked, pairs, tokens=tokens
            ))
        else:
            next_nonspace = ()
            bindings = ()
            scopes = MappingProxyType({})
            blocks = MappingProxyType({})
            shadow = MappingProxyType({})
        return cls(
            translated,
            masked,
            tokens,
            has_scope_grammar,
            tuple(token.start for token in tokens),
            pairs,
            next_nonspace,
            bindings,
            scopes,
            blocks,
            shadow,
            member_calls,
        )


def name_is_shadowed(name: str, declaration_start: int, declaration_end: int,
                     masked: str, pairs: list[tuple[int, int]], call_position: int,
                     shadow_index: dict[str, tuple[
                         tuple[int, tuple[int, int] | None], ...
                     ]] | None = None) -> bool:
    if lambda_init_capture_shadows(masked, pairs, name, call_position):
        return True
    if shadow_index is not None:
        for position, block in shadow_index.get(name, ()):
            if position <= declaration_end:
                continue
            if position >= call_position:
                break
            if block is not None and block[0] < call_position < block[1]:
                return True
        return False
    # Keep tokens following the candidate name available to the declarator
    # classifier.  Truncating at the call's member operator turns an expression
    # such as ``std::as_const(scope).read()`` into the declaration-shaped tail
    # ``as_const(scope)``.
    tokens = _compiler_tokens(masked, declaration_start)
    known_types = declared_type_names(_compiler_tokens(masked))
    for index, token in enumerate(tokens):
        if token.start >= call_position:
            break
        if token.value != name or declaration_start <= token.start <= declaration_end:
            continue
        if not token_is_declarator_name(tokens, index, known_types):
            continue
        block = immediate_block(pairs, token.start)
        if block is not None and block[0] < call_position < block[1]:
            return True
    return False


def handle_binding_is_shadowed(binding: HandleBinding, masked: str,
                               pairs: list[tuple[int, int]], call_position: int,
                               shadow_index=None) -> bool:
    return name_is_shadowed(binding.name, binding.position, binding.position,
                            masked, pairs, call_position, shadow_index)


def lease_binding_stays_local(masked: str, pairs: list[tuple[int, int]],
                              binding: HandleBinding, shadow_index=None) -> bool:
    """Keep the lease in synchronous blocks while excluding nested callable bodies."""
    allowed_methods = {"desc", "nativeHandle", "nativeSubresource", "valid"}
    tokens = _compiler_tokens(masked, binding.position, binding.block[1])
    known_types = declared_type_names(tokens)
    for index, token in enumerate(tokens):
        if token.value != binding.name or token.start == binding.position:
            continue
        if token_is_declarator_name(tokens, index, known_types):
            declaration_tokens = statement_tokens(masked, pairs, token.start)
            if any(item.value == "GpuReadLease" for item in declaration_tokens):
                continue
        if name_is_shadowed(binding.name, binding.position, binding.position,
                            masked, pairs, token.start, shadow_index):
            continue
        if not stays_in_synchronous_blocks(masked, pairs, binding.block, token.start):
            return False
        if (index + 3 < len(tokens)
                and tokens[index + 1].value == "."
                and tokens[index + 2].value in allowed_methods
                and tokens[index + 3].value == "("):
            continue
        return False
    return True


def audit_capability_uses(path: PurePosixPath, source: str,
                          *, compiler_view: bool = False,
                          compiler_translation_text: TranslationText | None = None,
                          compiler_analysis: CompilerAuditAnalysis | None = None,
                          ) -> list[Finding]:
    translated = (compiler_analysis.translated if compiler_analysis is not None
                  else compiler_translation_text if compiler_translation_text is not None
                  else compiler_translation(source) if compiler_view
                  else translate_source(source))
    masked = translated.masked
    pairs = (
        compiler_analysis.delimiter_pairs
        if compiler_analysis is not None else brace_pairs(masked)
    )
    findings: list[Finding] = []
    if not compiler_view:
        findings.extend(preprocessor_capability_findings(path, translated))
        findings.extend(phase_two_capability_findings(path, source, translated))
    calls = list(
        compiler_analysis.member_calls
        if compiler_analysis is not None else MEMBER_CALL.finditer(masked)
    )
    receiver_binding_index = {
        call.start(): (
            receiver_binding_name(masked, call.start()),
            receiver_binding_references(masked, call.start()),
        )
        for call in calls
        if call.group(1) in {"read", "withRead", "complete"}
    }
    scope_bindings = list(
        compiler_analysis.scope_bindings
        if compiler_analysis is not None else scope_bindings_linear(masked, pairs)
    )
    scopes_by_name: Mapping[str, tuple[ScopeBinding, ...] | list[ScopeBinding]]
    if compiler_analysis is not None:
        scopes_by_name = compiler_analysis.scopes_by_name
    else:
        mutable_scopes: dict[str, list[ScopeBinding]] = {}
        for binding in scope_bindings:
            mutable_scopes.setdefault(binding.declaration.name, []).append(binding)
        scopes_by_name = mutable_scopes
    shadow_index = (
        compiler_analysis.shadow_index
        if compiler_analysis is not None else build_shadow_index(masked, pairs)
    )
    bound_scope_positions = {
        binding.declaration.position for binding in scope_bindings
    }
    declarations = (
        scope_declarations(masked)
        if compiler_analysis is None or compiler_analysis.has_scope_grammar
        else ()
    )
    for declaration in declarations:
        if declaration.position not in bound_scope_positions:
            findings.append(
                Finding(path, translated.line_at(declaration.position), "GpuSyncReadScope",
                        "scope declaration is not inside a lexical block")
            )

    reads_by_scope: dict[int, list[re.Match[str]]] = {}
    acquisitions_by_scope: dict[int, list[re.Match[str]]] = {}
    completes_by_scope: dict[int, list[re.Match[str]]] = {}
    handle_bindings: list[HandleBinding] = []
    claimed_reads: set[int] = set()
    canonical_scope_cache: dict[int, bool] = {}

    def scope_is_canonical(scope: ScopeBinding) -> bool:
        position = scope.declaration.position
        if position not in canonical_scope_cache:
            canonical_scope_cache[position] = canonical_scope_declaration(
                masked, pairs, scope
            )
        return canonical_scope_cache[position]

    def audit_withread_callback(call: re.Match[str]) -> None:
        callback_block = withread_callback_block(masked, pairs, call)
        if callback_block is None:
            findings.append(Finding(
                path, translated.line_at(call.start()), "GpuSyncReadScope::withRead()",
                "withRead() requires an inline callback body so completion cannot be "
                "bypassed by hidden control flow"))
            return
        nonlocal_jump = next((
            token for token in _compiler_tokens(masked, callback_block[0] + 1,
                                          callback_block[1])
            if token.value in {"_longjmp", "longjmp", "siglongjmp"}
        ), None)
        if nonlocal_jump is not None:
            findings.append(Finding(
                path, translated.line_at(nonlocal_jump.start), nonlocal_jump.value,
                "non-local jump cannot bypass withRead() completion"))

    for call in calls:
        if call.group(1) not in {"read", "withRead", "complete"}:
            continue
        scope = resolve_scope(scopes_by_name, masked, pairs, call, shadow_index)
        if scope is None:
            receiver_name, receiver_names = receiver_binding_index[call.start()]
            referenced = [
                binding for binding in scope_bindings
                if binding.declaration.position < call.start() < binding.block[1]
                and (receiver_name == binding.declaration.name
                     or (receiver_name is None
                         and binding.declaration.name in receiver_names))
                and not name_is_shadowed(binding.declaration.name,
                                         binding.declaration.position,
                                         binding.declaration.end, masked, pairs,
                                         call.start(), shadow_index)
            ]
            if referenced:
                if call.group(1) == "withRead":
                    audit_withread_callback(call)
                if call.group(1) == "read":
                    claimed_reads.add(call.start())
                    if path not in SYNC_READ_ALLOWLIST:
                        findings.append(
                            Finding(path, translated.line_at(call.start()),
                                    "GpuSyncReadScope::read()",
                                    "public read() is not in the reviewed synchronous-adapter "
                                    "allowlist")
                        )
                    for binding in referenced:
                        findings.append(
                            Finding(path, translated.line_at(call.start()),
                                    f"{binding.declaration.name}.read()",
                                    "read() requires an exact local scope/lease and standalone "
                                    "complete() in the same lexical body"))
                else:
                    findings.append(Finding(
                        path, translated.line_at(call.start()),
                        f"unresolved {call.group(1)}() receiver",
                        "unresolved capability receiver references an active GPU binding"))
            continue
        scope_key = scope.declaration.position
        if call.group(1) == "read":
            claimed_reads.add(call.start())
            if path not in SYNC_READ_ALLOWLIST:
                findings.append(
                    Finding(path, translated.line_at(call.start()),
                            "GpuSyncReadScope::read()",
                            "public read() is not in the reviewed synchronous-adapter allowlist")
                )
            lease = canonical_read_lease(masked, pairs, call, scope.declaration.name)
            call_block = immediate_block(pairs, call.start())
            canonical = (scope_is_canonical(scope)
                         and call_block == scope.block and lease is not None)
            if not canonical:
                findings.append(
                    Finding(path, translated.line_at(call.start()),
                            f"{scope.declaration.name}.read()",
                            "read() requires an exact local scope/lease and standalone "
                            "complete() in the same lexical body")
                )
                continue
            reads_by_scope.setdefault(scope_key, []).append(call)
            acquisitions_by_scope.setdefault(scope_key, []).append(call)
            handle_bindings.append(HandleBinding(lease[0], lease[1], call_block, scope_key,
                                                  call.start()))
        elif call.group(1) == "withRead":
            audit_withread_callback(call)
            canonical_declaration = scope_is_canonical(scope)
            canonical_withread = (
                canonical_declaration and immediate_block(pairs, call.start()) == scope.block)
            if canonical_withread or not canonical_declaration:
                handle_bindings.extend(callback_lease_bindings(masked, pairs, call))
            if canonical_withread:
                acquisitions_by_scope.setdefault(scope_key, []).append(call)
        else:
            if scope_is_canonical(scope):
                completes_by_scope.setdefault(scope_key, []).append(call)

    # A temporary scope has no named binding to resolve, but its type still
    # identifies read() as the reviewed capability.
    for call in calls:
        if call.group(1) != "read" or call.start() in claimed_reads:
            continue
        receiver = receiver_expression(masked, call.start())
        if "GpuSyncReadScope" not in receiver:
            continue
        if path not in SYNC_READ_ALLOWLIST:
            findings.append(
                Finding(path, translated.line_at(call.start()), "GpuSyncReadScope::read()",
                        "public read() is not in the reviewed synchronous-adapter allowlist")
            )
        findings.append(
            Finding(path, translated.line_at(call.start()), "read()",
                    "a temporary read scope cannot call complete() after read/use")
        )

    native_calls = [call for call in calls if call.group(1) == "nativeHandle"]
    native_bindings = {call.start(): resolve_handle_binding(handle_bindings, masked, pairs, call)
                       for call in native_calls}
    for call in native_calls:
        binding = native_bindings[call.start()]
        if (binding is not None
                and handle_binding_is_shadowed(
                    binding, masked, pairs, call.start(), shadow_index)):
            native_bindings[call.start()] = None

    for binding in handle_bindings:
        if not lease_binding_stays_local(masked, pairs, binding, shadow_index):
            findings.append(Finding(
                path, translated.line_at(binding.position), binding.name,
                "lease reference must remain inside the immediate callback body"))

    all_tokens = list(
        compiler_analysis.tokens
        if compiler_analysis is not None else _compiler_tokens(masked)
    )
    native_call_names = {call.start(1) for call in native_calls}
    for index, token in enumerate(all_tokens):
        if token.value != "nativeHandle" or token.start in native_call_names:
            continue
        if native_handle_declaration_or_internal_call(
                path, translated.text, masked, token, all_tokens, index):
            continue
        findings.append(Finding(
            path, translated.line_at(token.start), "nativeHandle reference",
            "nativeHandle reference cannot be aliased, passed through preprocessing, or invoked "
            "through a pointer-to-member"))

    for scope_key, acquisitions in acquisitions_by_scope.items():
        if len(acquisitions) != 1:
            first = min(acquisitions, key=lambda call: call.start())
            findings.append(Finding(
                path, translated.line_at(first.start()), "GpuSyncReadScope acquisition",
                "a synchronous read scope permits exactly one acquisition total: read() or "
                "withRead()"))
            continue
        acquisition = acquisitions[0]
        completions = completes_by_scope.get(scope_key, [])
        if any(call.start() < acquisition.start() for call in completions):
            findings.append(Finding(
                path, translated.line_at(acquisition.start()), "GpuSyncReadScope::complete()",
                "complete() cannot precede acquisition; complete() after read/use is required"))
        if acquisition.group(1) == "withRead" and completions:
            findings.append(Finding(
                path, translated.line_at(completions[0].start()), "GpuSyncReadScope::complete()",
                "withRead() owns completion; explicit complete() is forbidden"))

    valid_read_scopes: set[int] = set()
    for scope_key, reads in reads_by_scope.items():
        if len(reads) != 1:
            first_read = min(reads, key=lambda call: call.start())
            findings.append(
                Finding(path, translated.line_at(first_read.start()), "GpuSyncReadScope::read()",
                        "the conservative direct-read form permits exactly one read()")
            )
            continue
        associated_uses = [
            call.start() for call in native_calls
            if (binding := native_bindings[call.start()]) is not None
            and binding.scope_position == scope_key
        ]
        required_positions = [call.start() for call in reads] + associated_uses
        required_position = max(required_positions)
        scope = next(binding for binding in scope_bindings
                     if binding.declaration.position == scope_key)
        all_completions = completes_by_scope.get(scope_key, [])
        standalone = [
            call for call in all_completions
            if immediate_block(pairs, call.start()) == scope.block
            and standalone_completion(masked, pairs, call, scope.declaration.name)
        ]
        earlier = [call for call in all_completions if call.start() < required_position]
        if earlier:
            findings.append(Finding(
                path, translated.line_at(earlier[0].start()), "GpuSyncReadScope::complete()",
                "native handle access cannot follow complete(); complete() after read/use is "
                "required"))
            continue
        if (len(standalone) == 1 and standalone[0].start() > required_position
                and not has_intervening_control_transfer(
                    masked, min(item.start() for item in reads), standalone[0].start())
                and not has_intervening_potentially_throwing_call(
                    masked, min(item.start() for item in reads), standalone[0].start())):
            valid_read_scopes.add(scope_key)
            continue
        first_read = min(reads, key=lambda call: call.start())
        findings.append(
            Finding(path, translated.line_at(first_read.start()),
                    f"{scope.declaration.name}.read()",
                    "the lexical scope must call complete() after read/use or use withRead()")
        )

    if path == LEASE_HEADER:
        return findings
    for handle_read in native_calls:
        binding = native_bindings[handle_read.start()]
        authorized = binding is not None and (
            binding.scope_position is None
            or (binding.scope_position in valid_read_scopes
                and binding.read_position is not None
                and binding.read_position < handle_read.start())
        )
        if not authorized:
            findings.append(
                Finding(path, translated.line_at(handle_read.start()),
                        "GpuSurface::nativeHandle()",
                        "native handle access must use the lease returned by its specific "
                        "read scope or withRead callback")
            )
            continue
        alias = local_native_alias(path, masked, pairs, handle_read)
        if alias is None:
            if native_handle_has_direct_consumer(
                    path, masked, pairs, handle_read, binding):
                continue
            findings.append(Finding(
                path, translated.line_at(handle_read.start()), "GpuReadLease::nativeHandle()",
                "native handle value must initialize a callback-local alias"))
            continue
        synchronous, last_use = native_alias_stays_synchronous(
            path, masked, pairs, binding, alias[0], alias[1])
        if synchronous and binding.scope_position is not None:
            completion_positions = [
                call.start() for call in completes_by_scope.get(binding.scope_position, [])
                if standalone_completion(masked, pairs, call,
                                         next(scope.declaration.name for scope in scope_bindings
                                              if scope.declaration.position ==
                                              binding.scope_position))
            ]
            synchronous = len(completion_positions) == 1 and last_use < completion_positions[0]
        if not synchronous:
            findings.append(Finding(
                path, translated.line_at(handle_read.start()), alias[0],
                "native handle value must remain inside its synchronous consumption"))
    return findings


def audit_public_member(
    path: PurePosixPath,
    source: str,
    class_name: str,
    member_pattern: str,
    expression: str,
    *,
    candidate_line: Callable[[int], bool] | None = None,
    pretokenized: bool = False,
    compiler_analysis: CompilerAuditAnalysis | None = None,
) -> list[Finding]:
    masked = source if pretokenized else mask_non_code(source)
    class_match = next((
        match for match in re.finditer(
            rf"\bclass\s+{re.escape(class_name)}\b[^{{;]*{{", masked
        )
        if candidate_line is None
        or candidate_line(line_number(source, match.start()))
    ), None)
    if not class_match:
        if candidate_line is not None:
            return []
        return [Finding(path, 1, class_name, "audited class declaration was not found")]
    opening = masked.find("{", class_match.start(), class_match.end())
    pairs = (
        compiler_analysis.delimiter_pairs
        if compiler_analysis is not None else brace_pairs(masked)
    )
    pair = next((candidate for candidate in pairs if candidate[0] == opening), None)
    if pair is None:
        return [Finding(path, line_number(source, opening), class_name,
                        "audited class body is incomplete")]

    access = "private"
    body_start = opening + 1
    offset = body_start
    findings: list[Finding] = []
    for line in masked[body_start:pair[1]].splitlines(keepends=True):
        access_match = re.match(r"\s*(public|private|protected)\s*:", line)
        if access_match:
            access = access_match.group(1)
        member_match = re.search(member_pattern, line)
        if member_match and access == "public":
            position = offset + member_match.start()
            findings.append(
                Finding(path, line_number(source, position), expression,
                        f"{class_name} must not expose this operation publicly")
            )
        offset += len(line)
    return findings


def _validate_capability_expression_provenance(
    buffer: AuditBuffer,
    translated: TranslationText,
    analysis: CompilerAuditAnalysis,
) -> None:
    """Resolve GPU grammar first, then validate only its required token ranges."""

    masked = translated.masked
    pairs = analysis.delimiter_pairs
    calls = list(analysis.member_calls)
    scopes = list(analysis.scope_bindings)
    scopes_by_name = analysis.scopes_by_name
    shadow_index = analysis.shadow_index
    handle_bindings: list[HandleBinding] = []

    def require_tokens(tokens: list[CppToken]) -> None:
        if tokens:
            buffer.require_same_origin(tokens[0].start, tokens[-1].end)

    def call_span(call: re.Match[str]) -> tuple[int, int] | None:
        expression = receiver_expression(masked, call.start())
        expression_end = call.start()
        while expression_end > 0 and masked[expression_end - 1].isspace():
            expression_end -= 1
        start = expression_end - len(expression)
        closing = matching_delimiter(masked, call.end() - 1, "(", ")")
        return None if closing is None else (start, closing + 1)

    for scope in scopes:
        buffer.require_same_origin(
            scope.declaration.position, scope.declaration.end
        )

    for call in calls:
        operation = call.group(1)
        if operation not in {"read", "withRead", "complete"}:
            continue
        scope = resolve_scope(scopes_by_name, masked, pairs, call, shadow_index)
        if scope is None:
            continue
        span = call_span(call)
        if span is not None:
            buffer.require_same_origin(*span)
        buffer.require_same_positions(
            scope.declaration.position, call.start(1)
        )
        if operation == "read":
            lease = canonical_read_lease(
                masked, pairs, call, scope.declaration.name
            )
            if lease is not None:
                statement = statement_tokens(masked, pairs, call.start())
                require_tokens(statement)
                block = immediate_block(pairs, call.start())
                if block is not None:
                    handle_bindings.append(HandleBinding(
                        lease[0], lease[1], block,
                        scope.declaration.position, call.start(),
                    ))
        elif operation == "withRead":
            bindings = callback_lease_bindings(masked, pairs, call)
            for binding in bindings:
                buffer.require_same_positions(
                    scope.declaration.position, call.start(1), binding.position
                )
            handle_bindings.extend(bindings)
        else:
            require_tokens(statement_tokens(masked, pairs, call.start()))

    native_calls = [call for call in calls if call.group(1) == "nativeHandle"]
    for call in native_calls:
        span = call_span(call)
        if span is not None:
            buffer.require_same_origin(*span)
        binding = resolve_handle_binding(handle_bindings, masked, pairs, call)
        if binding is not None:
            positions = [binding.position, call.start(1)]
            if binding.scope_position is not None:
                positions.append(binding.scope_position)
            if binding.read_position is not None:
                positions.append(binding.read_position)
            buffer.require_same_positions(*positions)

    # Native-handle declarations, aliases, casts, typed consumers, methods, and
    # direct sinks are all statement-local in the locked grammar. A declaration
    # is itself a guarded construct, so validate its complete required-token
    # range when any part is production even if nativeHandle came from an
    # external expansion. Wholly external declarations remain out of scope.
    def is_native_handle_declaration(tokens: list[CppToken]) -> bool:
        values = [token.value for token in tokens]
        for index, value in enumerate(values):
            if value != "nativeHandle":
                continue
            if index and values[index - 1] in {".", "->", "::", "&"}:
                continue
            if values[index + 1:index + 4] != ["(", ")", "const"]:
                continue
            prefix = values[:index]
            if "void" in prefix and "*" in prefix:
                return True
        return False

    statement: list[CppToken] = []
    has_production_native = False
    for token in analysis.tokens:
        if token.value in {"{", "}"}:
            statement = []
            has_production_native = False
            continue
        statement.append(token)
        if token.value == "nativeHandle":
            has_production_native = (
                has_production_native
                or buffer.position_provenance_key(token.start)[2]
            )
        if token.value == ";":
            declaration = is_native_handle_declaration(statement)
            any_production = declaration and any(
                buffer.position_provenance_key(item.start)[2]
                for item in statement
            )
            if has_production_native or any_production:
                require_tokens(statement)
            statement = []
            has_production_native = False

    public_policies = (
        ("GpuRetireRegistry", r"\bregisterRetire\s*\("),
        ("GpuOpScope", r"\btrack\s*\("),
    )
    for class_name, member_pattern in public_policies:
        for class_match in re.finditer(
            rf"\bclass\s+{re.escape(class_name)}\b[^{{;]*{{", masked
        ):
            opening = masked.find("{", class_match.start(), class_match.end())
            body = next((pair for pair in pairs if pair[0] == opening), None)
            if body is None:
                continue
            for member in re.finditer(member_pattern, masked[opening + 1:body[1]]):
                member_position = opening + 1 + member.start()
                buffer.require_same_positions(class_match.start(), member_position)
                require_tokens(statement_tokens(masked, pairs, member_position))


@dataclass(frozen=True, slots=True)
class PublicCapabilityPolicy:
    path: PurePosixPath
    class_name: str
    member_pattern: str
    expression: str


@dataclass(frozen=True, slots=True)
class CapabilityRule:
    trigger_spellings: tuple[bytes, ...]
    evaluator: Callable[..., list[Finding]]
    public_policy: PublicCapabilityPolicy | None = None


_CAPABILITY_RULES = (
    CapabilityRule(
        (
            b"GpuSyncReadScope",
            b"_longjmp",
            b"complete",
            b"longjmp",
            b"nativeHandle",
            b"read",
            b"siglongjmp",
            b"withRead",
        ),
        audit_capability_uses,
    ),
    CapabilityRule(
        (b"GpuRetireRegistry", b"registerRetire"),
        audit_public_member,
        PublicCapabilityPolicy(
            REGISTRY_HEADER,
            "GpuRetireRegistry",
            r"\bregisterRetire\s*\(",
            "GpuRetireRegistry::registerRetire()",
        ),
    ),
    CapabilityRule(
        (b"GpuOpScope", b"track"),
        audit_public_member,
        PublicCapabilityPolicy(
            OP_SCOPE_HEADER,
            "GpuOpScope",
            r"\btrack\s*\(",
            "GpuOpScope::track()",
        ),
    ),
)

_CAPABILITY_TRIGGER_SPELLINGS = frozenset(
    spelling
    for rule in _CAPABILITY_RULES
    for spelling in rule.trigger_spellings
)


def capability_candidate_spellings() -> frozenset[bytes]:
    return _CAPABILITY_TRIGGER_SPELLINGS


def view_has_capability_spelling(view: PreprocessedTranslationUnitView) -> bool:
    if not isinstance(view, PreprocessedTranslationUnitView):
        raise AuditInfrastructureError("preprocessed audit view is invalid")
    spellings = object.__getattribute__(view.tokens, "_spellings")
    return not capability_candidate_spellings().isdisjoint(spellings)


def _capability_policy_key(path: PurePosixPath) -> tuple[object, ...]:
    return (
        path == LEASE_HEADER,
        path in SYNC_READ_ALLOWLIST,
        path in SURFACE_INTERNAL_HANDLE_PATHS,
        tuple(sorted(REVIEWED_NATIVE_HANDLE_SINKS.get(path, ()))),
        tuple(sorted(REVIEWED_NATIVE_HANDLE_METHODS.get(path, ()))),
        tuple(sorted(REVIEWED_NATIVE_HANDLE_MEMBER_SINKS.get(path, ()))),
        tuple(sorted(REVIEWED_NATIVE_HANDLE_TYPES.get(path, ()))),
    )


def _map_candidate_findings(
    buffer: AuditBuffer,
    candidate_paths: frozenset[PurePosixPath],
    findings: Iterable[Finding],
) -> list[Finding]:
    mapped: list[Finding] = []
    for finding in findings:
        location = buffer.location_for_line(finding.line)
        identity = location.identity
        if (
            identity is None
            or not identity.production
            or identity.relative not in candidate_paths
        ):
            continue
        mapped.append(Finding(
            identity.relative,
            location.line,
            finding.expression,
            finding.reason,
        ))
    return mapped


def _audit_preprocessed_view_unfiltered(
    view: PreprocessedTranslationUnitView,
    limits: AuditLimits,
    rss_reader: Callable[[], int],
    *,
    _filter_rule_paths: bool = False,
) -> list[Finding]:
    """Apply every rule; the default is the independent reference path."""

    buffer = AuditBuffer.from_preprocessed(view, limits, rss_reader)
    translated = TranslationText(
        buffer.text,
        buffer.text,
        buffer.text,
        (),
        (),
        buffer._line_starts,
    )
    analysis = CompilerAuditAnalysis.from_translation(translated)
    buffer.reserve_rss(rss_reader, limits.rss_bytes, len(buffer.text) * 48)
    context_token = _compiler_analysis_context.set(analysis)
    try:
        _validate_capability_expression_provenance(buffer, translated, analysis)
    finally:
        _compiler_analysis_context.reset(context_token)
    buffer.reserve_rss(rss_reader, limits.rss_bytes, 0)
    findings: list[Finding] = []
    for rule in _CAPABILITY_RULES:
        path_filter = (
            frozenset(rule.trigger_spellings) if _filter_rule_paths else None
        )
        candidate_paths = buffer.candidate_paths(path_filter)
        if rule.public_policy is None:
            grouped_paths: dict[tuple[object, ...], list[PurePosixPath]] = {}
            for path in candidate_paths:
                grouped_paths.setdefault(_capability_policy_key(path), []).append(path)
            for paths in grouped_paths.values():
                representative = paths[0]
                allowed = frozenset(paths)
                buffer.reserve_rss(
                    rss_reader, limits.rss_bytes, len(buffer.text) * 48
                )
                context_token = _compiler_analysis_context.set(analysis)
                try:
                    candidate_findings = rule.evaluator(
                        representative,
                        buffer.text,
                        compiler_view=True,
                        compiler_translation_text=translated,
                        compiler_analysis=analysis,
                    )
                finally:
                    _compiler_analysis_context.reset(context_token)
                buffer.reserve_rss(rss_reader, limits.rss_bytes, 0)
                findings.extend(_map_candidate_findings(
                    buffer,
                    allowed,
                    candidate_findings,
                ))
            continue

        policy = rule.public_policy
        if policy.path not in candidate_paths:
            continue
        if not buffer.path_has_spelling(
            policy.path, policy.class_name.encode("ascii")
        ):
            continue

        def candidate_line(
            line: int,
            *,
            expected: PurePosixPath = policy.path,
        ) -> bool:
            location = buffer.location_for_line(line)
            identity = location.identity
            return bool(
                identity is not None
                and identity.production
                and identity.relative == expected
            )

        buffer.reserve_rss(rss_reader, limits.rss_bytes, len(buffer.text) * 16)
        context_token = _compiler_analysis_context.set(analysis)
        try:
            public_findings = rule.evaluator(
                policy.path,
                buffer.text,
                policy.class_name,
                policy.member_pattern,
                policy.expression,
                candidate_line=candidate_line,
                pretokenized=True,
                compiler_analysis=analysis,
            )
        finally:
            _compiler_analysis_context.reset(context_token)
        buffer.reserve_rss(rss_reader, limits.rss_bytes, 0)
        findings.extend(_map_candidate_findings(
            buffer,
            frozenset((policy.path,)),
            public_findings,
        ))

    unique = set(findings)
    return sorted(
        unique,
        key=lambda finding: (
            finding.path.as_posix(),
            finding.line,
            finding.expression,
            finding.reason,
        ),
    )


def audit_preprocessed_view(
    view: PreprocessedTranslationUnitView,
    limits: AuditLimits,
    rss_reader: Callable[[], int],
) -> list[Finding]:
    """Apply the exact audit only when an attested rule can possibly report."""

    if not view_has_capability_spelling(view):
        return []
    return _audit_preprocessed_view_unfiltered(
        view, limits, rss_reader, _filter_rule_paths=True
    )


def aggregate_findings(
    findings: Iterable[tuple[Finding, str]],
) -> list[AggregatedFinding]:
    configurations: dict[Finding, set[str]] = {}
    for finding, configuration in findings:
        configurations.setdefault(finding, set()).add(configuration)
    return [
        AggregatedFinding(finding, tuple(sorted(configurations[finding])))
        for finding in sorted(
            configurations,
            key=lambda item: (
                item.path.as_posix(), item.line, item.expression, item.reason
            ),
        )
    ]


_RAW_DIRECTIVES = frozenset({
    "define", "elif", "elifdef", "elifndef", "else", "endif", "error",
    "if", "ifdef", "ifndef", "import", "include", "include_next", "pragma",
    "undef", "warning",
})


def _header_operand_after_leading_comments(tail: str) -> str:
    remaining = tail.lstrip()
    while remaining.startswith("/*"):
        closing = remaining.find("*/", 2)
        if closing < 0:
            return ""
        remaining = remaining[closing + 2:].lstrip()
    if remaining.startswith("//"):
        return ""
    return remaining


def _comments_and_whitespace_only(tail: str) -> bool:
    remaining = tail.lstrip()
    while remaining.startswith("/*"):
        closing = remaining.find("*/", 2)
        if closing < 0:
            return False
        remaining = remaining[closing + 2:].lstrip()
    return not remaining or remaining.startswith("//")


def _header_operand_is_well_formed(text_tail: str, masked_tail: str) -> bool:
    operand = _header_operand_after_leading_comments(text_tail)
    if not operand:
        return False
    if operand[0] == '<':
        closing = operand.find(">", 1)
        return closing > 1 and _comments_and_whitespace_only(operand[closing + 1:])
    if operand[0] == '"':
        escaped = False
        closing = None
        for index, char in enumerate(operand[1:], 1):
            if char == '"' and not escaped:
                closing = index
                break
            escaped = char == "\\" and not escaped
            if char != "\\":
                escaped = False
        return (closing is not None
                and _comments_and_whitespace_only(operand[closing + 1:]))
    return bool(masked_tail)


def _directive_expression_is_valid(masked_tail: str, text_tail: str) -> bool:
    """Validate the bounded preprocessing-expression grammar without evaluating it."""

    integer_suffix = (
        r"(?:[uU](?:(?:ll|LL)|[lL]|[zZ])?|"
        r"(?:(?:ll|LL)|[lL])(?:[uU])?|[zZ](?:[uU])?)?"
    )
    integer = re.compile(
        r"(?:"
        r"0[xX][0-9A-Fa-f](?:'?[0-9A-Fa-f])*|"
        r"0[bB][01](?:'?[01])*|"
        r"0(?:'?[0-7])*|"
        r"[1-9](?:'?[0-9])*"
        r")" + integer_suffix
    )
    character = re.compile(
        r"(?:u8|u|U|L)?'(?:\\(?:[^\r\n]|\r?\n)|[^'\\\r\n])+'"
    )
    string_literal = re.compile(
        r'(?:u8|u|U|L)?"(?:\\(?:[^\r\n]|\r?\n)|[^"\\\r\n])*"'
    )
    identifier = re.compile(r"[A-Za-z_]\w*")
    alternatives = {
        "and": "&&", "or": "||", "not": "!", "bitand": "&",
        "bitor": "|", "xor": "^", "compl": "~", "not_eq": "!=",
    }
    punctuators = (
        "&&", "||", "==", "!=", "<=", ">=", "<<", ">>",
        "(", ")", "[", "]", "{", "}", "?", ":", "+", "-", "!",
        "~", "|", "^", "&", "<", ">", "*", "/", "%", ",", ".",
    )

    def character_escape_sequence_is_valid(spelling: str) -> bool:
        opening = spelling.find("'")
        content = spelling[opening + 1:-1]
        cursor = 0
        while cursor < len(content):
            if content[cursor] != "\\":
                cursor += 1
                continue
            cursor += 1
            if cursor >= len(content):
                return False
            escape = content[cursor]
            if escape == "x":
                cursor += 1
                first_hex = cursor
                while cursor < len(content) and content[cursor] in "0123456789abcdefABCDEF":
                    cursor += 1
                if cursor == first_hex:
                    return False
                continue
            if escape in {"u", "U"}:
                digits = 4 if escape == "u" else 8
                sequence = content[cursor + 1:cursor + 1 + digits]
                if (len(sequence) != digits
                        or any(char not in "0123456789abcdefABCDEF"
                               for char in sequence)):
                    return False
                cursor += digits + 1
                continue
            if escape in "01234567":
                cursor += 1
                consumed = 1
                while (cursor < len(content) and consumed < 3
                       and content[cursor] in "01234567"):
                    cursor += 1
                    consumed += 1
                continue
            if escape not in "'\"?\\abfnrtv":
                return False
            cursor += 1
        return True

    def lex_expression(source: str) -> list[str] | None:
        result: list[str] = []
        cursor = 0
        while cursor < len(source):
            if source[cursor].isspace():
                cursor += 1
                continue
            if source.startswith("//", cursor):
                break
            if source.startswith("/*", cursor):
                closing = source.find("*/", cursor + 2)
                if closing < 0:
                    return None
                cursor = closing + 2
                continue
            quoted = string_literal.match(source, cursor)
            if quoted is not None:
                result.append("__opaque_string_argument__")
                cursor = quoted.end()
                continue
            literal = character.match(source, cursor)
            if literal is not None:
                if not character_escape_sequence_is_valid(literal.group()):
                    return None
                result.append("__character_constant__")
                cursor = literal.end()
                continue
            if source[cursor].isdigit():
                number = integer.match(source, cursor)
                if number is None:
                    return None
                result.append("__integer_constant__")
                cursor = number.end()
                continue
            word = identifier.match(source, cursor)
            if word is not None:
                result.append(alternatives.get(word.group(), word.group()))
                cursor = word.end()
                continue
            operator = next(
                (candidate for candidate in punctuators
                 if source.startswith(candidate, cursor)),
                None,
            )
            if operator is None:
                return None
            result.append(operator)
            cursor += len(operator)
        return result

    tokens = lex_expression(text_tail)
    if tokens is None or not tokens:
        return False
    index = 0
    binary_precedence = {
        "||": 1, "&&": 2, "|": 3, "^": 4, "&": 5,
        "==": 6, "!=": 6,
        "<": 7, "<=": 7, ">": 7, ">=": 7,
        "<<": 8, ">>": 8,
        "+": 9, "-": 9,
        "*": 10, "/": 10, "%": 10,
    }

    def consume_balanced_call() -> bool:
        nonlocal index
        if index >= len(tokens) or tokens[index] != "(":
            return False
        stack = [")"]
        index += 1
        pairs = {"(": ")", "[": "]", "{": "}"}
        while index < len(tokens) and stack:
            value = tokens[index]
            if value in pairs:
                stack.append(pairs[value])
            elif value == stack[-1]:
                stack.pop()
            elif value in {")", "]", "}"}:
                return False
            index += 1
        return not stack

    def parse_primary() -> bool:
        nonlocal index
        if index >= len(tokens):
            return False
        value = tokens[index]
        if value in {"+", "-", "!", "~"}:
            index += 1
            return parse_primary()
        if value == "defined":
            index += 1
            if index < len(tokens) and tokens[index] == "(":
                index += 1
                if (index >= len(tokens)
                        or re.fullmatch(r"[A-Za-z_]\w*", tokens[index]) is None):
                    return False
                index += 1
                if index >= len(tokens) or tokens[index] != ")":
                    return False
                index += 1
                return True
            if (index >= len(tokens)
                    or re.fullmatch(r"[A-Za-z_]\w*", tokens[index]) is None):
                return False
            index += 1
            return True
        if value == "(":
            index += 1
            if not parse_expression(0):
                return False
            if index >= len(tokens) or tokens[index] != ")":
                return False
            index += 1
            return True
        is_identifier = (
            value not in {
                "__integer_constant__", "__character_constant__",
                "__opaque_string_argument__",
            }
            and re.fullmatch(r"[A-Za-z_]\w*", value) is not None
        )
        if (not is_identifier
                and value not in {"__integer_constant__", "__character_constant__"}):
            return False
        index += 1
        # A function-like macro invocation is an opaque primary until macro
        # expansion (for example __has_include(<header>) or QT_VERSION_CHECK()).
        if is_identifier and index < len(tokens) and tokens[index] == "(":
            return consume_balanced_call()
        return True

    def parse_expression(minimum_precedence: int) -> bool:
        nonlocal index
        if not parse_primary():
            return False
        while index < len(tokens):
            operator = tokens[index]
            precedence = binary_precedence.get(operator)
            if precedence is None or precedence < minimum_precedence:
                break
            index += 1
            if not parse_expression(precedence + 1):
                return False
        if minimum_precedence == 0 and index < len(tokens) and tokens[index] == "?":
            index += 1
            if not parse_expression(0):
                return False
            if index >= len(tokens) or tokens[index] != ":":
                return False
            index += 1
            if not parse_expression(0):
                return False
        return True

    return parse_expression(0) and index == len(tokens)


def _function_macro_parameters_are_valid(tail: str) -> bool:
    name = re.match(r"[A-Za-z_]\w*", tail)
    if name is None:
        return False
    remainder = tail[name.end():]
    if not remainder.startswith("("):
        return True
    closing = matching_delimiter(remainder, 0, "(", ")")
    if closing is None:
        return False
    raw_parameters = remainder[1:closing]
    if not raw_parameters.strip():
        return True
    parameters = [parameter.strip() for parameter in raw_parameters.split(",")]
    if any(not parameter for parameter in parameters):
        return False
    normalized: list[str] = []
    for index, parameter in enumerate(parameters):
        variadic = parameter == "..." or parameter.endswith("...")
        spelling = parameter[:-3].strip() if parameter.endswith("...") else parameter
        if variadic and index != len(parameters) - 1:
            return False
        if parameter != "..." and re.fullmatch(r"[A-Za-z_]\w*", spelling) is None:
            return False
        if spelling and spelling in normalized:
            return False
        if spelling:
            normalized.append(spelling)
    return True


def _raw_lane_translation(path: PurePosixPath, source: str) \
        -> tuple[TranslationText, list[Finding]]:
    """Mask macro directives while retaining physical/directive diagnostics."""

    translated = translate_source(source)
    masked = list(translated.masked)
    findings: list[Finding] = []
    conditional_stack: list[tuple[int, bool]] = []
    offset = 0
    logical_texts = translated.text.splitlines(keepends=True)
    logical_masks = translated.masked.splitlines(keepends=True)
    for logical_text, logical in zip(logical_texts, logical_masks, strict=True):
        stripped = logical.lstrip()
        if stripped.startswith("#"):
            directive = re.match(r"#\s*([A-Za-z_]\w*)\b(.*)", stripped.rstrip("\r\n"))
            text_directive = re.match(
                r"#\s*([A-Za-z_]\w*)\b(.*)",
                logical_text.lstrip().rstrip("\r\n"),
            )
            name = directive.group(1) if directive is not None else ""
            tail = directive.group(2).strip() if directive is not None else ""
            text_tail = (
                text_directive.group(2).strip()
                if text_directive is not None else ""
            )
            line = translated.line_at(offset + len(logical) - len(stripped))
            operand_required = {
                "define", "elif", "elifdef", "elifndef", "if", "ifdef",
                "ifndef", "undef",
            }
            header_operand_missing = (
                name in {"import", "include", "include_next"}
                and not _header_operand_is_well_formed(text_tail, tail)
            )
            malformed = (
                directive is None
                or name not in _RAW_DIRECTIVES.union({"line"})
                or (name in operand_required
                    and not (text_tail if name in {"if", "elif"} else tail))
                or header_operand_missing
                or (name in {"define", "undef"}
                    and re.match(r"[A-Za-z_]\w*", tail) is None)
                or (name in {"ifdef", "ifndef", "elifdef", "elifndef", "undef"}
                    and re.fullmatch(r"[A-Za-z_]\w*", tail) is None)
                or (name == "define"
                    and not _function_macro_parameters_are_valid(tail))
                or (name in {"if", "elif"}
                    and not _directive_expression_is_valid(tail, text_tail))
                or (name in {"else", "endif"} and bool(tail))
            )
            if name in {"if", "ifdef", "ifndef"}:
                conditional_stack.append((line, False))
            elif name in {"elif", "elifdef", "elifndef"}:
                if not conditional_stack or conditional_stack[-1][1]:
                    malformed = True
            elif name == "else":
                if not conditional_stack or conditional_stack[-1][1]:
                    malformed = True
                else:
                    opening_line, _unused = conditional_stack[-1]
                    conditional_stack[-1] = (opening_line, True)
            elif name == "endif":
                if not conditional_stack:
                    malformed = True
                else:
                    conditional_stack.pop()
            if name == "line":
                findings.append(Finding(
                    path,
                    line,
                    "source-authored #line directive",
                    "source-authored line directives cannot alter capability provenance",
                ))
            elif malformed:
                findings.append(Finding(
                    path,
                    line,
                    "malformed preprocessor directive",
                    "malformed directive syntax is rejected fail-closed",
                ))
            if name in {"define", "undef"}:
                for index in range(offset, offset + len(logical)):
                    if translated.masked[index] not in "\r\n":
                        masked[index] = " "
        offset += len(logical)
    for opening_line, _saw_else in conditional_stack:
        findings.append(Finding(
            path,
            opening_line,
            "unterminated conditional directive",
            "conditional preprocessing directives must be structurally balanced",
        ))
    grammar_text = "".join(masked)
    return TranslationText(
        translated.original,
        grammar_text,
        grammar_text,
        translated.source_lines,
        translated.splice_boundaries,
    ), findings


def _check_policy_deadline(pipeline_deadline: float | None) -> None:
    if pipeline_deadline is None:
        return
    if (
        not isinstance(pipeline_deadline, (int, float))
        or isinstance(pipeline_deadline, bool)
        or not math.isfinite(float(pipeline_deadline))
        or time.monotonic() >= pipeline_deadline
    ):
        raise AuditInfrastructureError("GPU capability policy deadline exceeded")


def audit_raw_sources(
    sources: Mapping[PurePosixPath, str],
    *,
    workspace=None,
    sink=None,
    provenance: str = "raw-source",
    pipeline_deadline: float | None = None,
) -> list[Finding]:
    """Audit source facts that preprocessing may erase, across every branch."""

    if workspace is not None and sink is None:
        raise AuditInfrastructureError(
            "bounded raw policy workspace requires a sink"
        )
    findings: list[Finding] = []
    for path, source in sources.items():
        _check_policy_deadline(pipeline_deadline)
        if not is_production_path(path):
            continue
        scratch = (
            None
            if workspace is None
            else workspace.reserve_policy_scratch(
                len(source), source_only=False
            )
        )
        try:
            path_findings: list[Finding] = []
            _check_policy_deadline(pipeline_deadline)
            translated, directive_findings = _raw_lane_translation(path, source)
            _check_policy_deadline(pipeline_deadline)
            path_findings.extend(directive_findings)
            path_findings.extend(phase_two_capability_findings(
                path, source, translated
            ))
            _check_policy_deadline(pipeline_deadline)
            path_findings.extend(audit_capability_uses(
                path,
                translated.text,
                compiler_view=True,
                compiler_translation_text=translated,
            ))
            _check_policy_deadline(pipeline_deadline)
            if sink is None:
                findings.extend(path_findings)
            else:
                for finding in path_findings:
                    sink(finding, provenance)
                path_findings.clear()
                directive_findings = None
                translated = None
        finally:
            if scratch is not None:
                workspace.release_policy_scratch(scratch)
    if REGISTRY_HEADER in sources:
        _check_policy_deadline(pipeline_deadline)
        source = sources[REGISTRY_HEADER]
        scratch = (
            None if workspace is None else workspace.reserve_policy_scratch(
                len(source), source_only=False
            )
        )
        try:
            translated, _unused = _raw_lane_translation(REGISTRY_HEADER, source)
            _check_policy_deadline(pipeline_deadline)
            header_findings = audit_public_member(
                REGISTRY_HEADER,
                translated.text,
                "GpuRetireRegistry",
                r"\bregisterRetire\s*\(",
                "GpuRetireRegistry::registerRetire()",
                pretokenized=True,
            )
            _check_policy_deadline(pipeline_deadline)
            if sink is None:
                findings.extend(header_findings)
            else:
                for finding in header_findings:
                    sink(finding, provenance)
                header_findings.clear()
                translated = None
        finally:
            if scratch is not None:
                workspace.release_policy_scratch(scratch)
    if OP_SCOPE_HEADER in sources:
        _check_policy_deadline(pipeline_deadline)
        source = sources[OP_SCOPE_HEADER]
        scratch = (
            None if workspace is None else workspace.reserve_policy_scratch(
                len(source), source_only=False
            )
        )
        try:
            translated, _unused = _raw_lane_translation(OP_SCOPE_HEADER, source)
            _check_policy_deadline(pipeline_deadline)
            header_findings = audit_public_member(
                OP_SCOPE_HEADER,
                translated.text,
                "GpuOpScope",
                r"\btrack\s*\(",
                "GpuOpScope::track()",
                pretokenized=True,
            )
            _check_policy_deadline(pipeline_deadline)
            if sink is None:
                findings.extend(header_findings)
            else:
                for finding in header_findings:
                    sink(finding, provenance)
                header_findings.clear()
                translated = None
        finally:
            if scratch is not None:
                workspace.release_policy_scratch(scratch)
    if sink is not None:
        _check_policy_deadline(pipeline_deadline)
        return []
    _check_policy_deadline(pipeline_deadline)
    result = sorted(
        set(findings),
        key=lambda item: (item.path.as_posix(), item.line, item.expression, item.reason),
    )
    _check_policy_deadline(pipeline_deadline)
    return result


_MACRO_LIKE_IDENTIFIER = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


@dataclass(frozen=True)
class SourceBinding:
    name: str
    category: str
    declaration: int
    scope_start: int
    scope_stop: int


@dataclass(frozen=True)
class SourceTypeAlias:
    name: str
    target: tuple[str, ...]
    target_position: int
    declaration: int
    scope_start: int
    scope_stop: int


@dataclass(frozen=True)
class SourceNamespaceDefinition:
    declaration: int
    scope_start: int
    scope_stop: int
    global_definition: bool


def _source_only_declared_bindings(
    masked: str,
    directive_ranges: list[tuple[int, int]],
) -> list[SourceBinding]:
    """Index positioned lexical bindings with balanced declarator parsing."""

    directive_starts = tuple(start for start, _stop in directive_ranges)

    def in_directive(position: int) -> bool:
        range_index = bisect.bisect_right(directive_starts, position) - 1
        return (range_index >= 0
                and position < directive_ranges[range_index][1])

    tokens = [
        token for token in _compiler_tokens(masked)
        if not in_directive(token.start)
    ]
    pairs = brace_pairs(masked)
    brace_closings = {opening: closing for opening, closing in pairs}
    token_scopes: dict[int, tuple[int, int]] = {}
    scope_stack: list[tuple[int, int]] = []
    for token in tokens:
        if token.value == "}" and scope_stack:
            scope_stack.pop()
        token_scopes[token.start] = scope_stack[-1] if scope_stack else (0, len(masked))
        if token.value == "{" and token.start in brace_closings:
            scope_stack.append((token.start, brace_closings[token.start]))
    bindings: list[SourceBinding] = []
    aliases_by_name: dict[str, list[SourceTypeAlias]] = {}
    std_namespace_definitions: list[SourceNamespaceDefinition] = []
    ordinary_type_shadow = ("__source_only_ordinary_type_shadow__",)
    root_scope = (0, len(masked))
    macro_environment_events, _macro_directive_ranges = (
        source_macro_environment_events(masked)
    )
    macro_environment_positions = tuple(
        position for position, _environments in macro_environment_events
    )

    def lexical_scope(position: int) -> tuple[int, int]:
        return token_scopes.get(position, (0, len(masked)))

    def balanced_type_prefix(values: list[str]) -> bool:
        first_identifier = 1 if values and values[0] == "::" else 0
        if (len(values) <= first_identifier
                or not re.fullmatch(
                    r"[A-Za-z_]\w*", values[first_identifier]
                )):
            return False
        forbidden = {".", "->", "?", "+", "-", "/", "%", "!", "||", "&&"}
        if any(value in forbidden for value in values):
            return False
        angle_depth = 0
        for value in values:
            if value == "<":
                angle_depth += 1
            elif value == ">":
                angle_depth -= 1
            elif value == ">>":
                angle_depth -= 2
            if angle_depth < 0:
                return False
        return angle_depth == 0

    def top_level_chunks(items: list[CppToken]) -> list[list[CppToken]]:
        chunks: list[list[CppToken]] = []
        start = 0
        depths = {"(": 0, "[": 0, "{": 0, "<": 0}
        closing = {")": "(", "]": "[", "}": "{", ">": "<"}
        for index, item in enumerate(items):
            value = item.value
            if value in depths:
                depths[value] += 1
            elif value == ">>" and depths["<"]:
                depths["<"] = max(0, depths["<"] - 2)
            elif value in closing and depths[closing[value]]:
                depths[closing[value]] -= 1
            elif value == "," and not any(depths.values()):
                chunks.append(items[start:index])
                start = index + 1
        chunks.append(items[start:])
        return [chunk for chunk in chunks if chunk]

    def declarator_lhs(items: list[CppToken]) -> list[CppToken]:
        angle_depth = 0
        index = 0
        while index < len(items):
            item = items[index]
            value = item.value
            if value == "<":
                angle_depth += 1
            elif value == ">" and angle_depth:
                angle_depth -= 1
            elif value == ">>" and angle_depth:
                angle_depth = max(0, angle_depth - 2)
            elif (not angle_depth and value == "(" and index > 0
                  and items[index - 1].value == "decltype"):
                depth = 1
                index += 1
                while index < len(items) and depth:
                    if items[index].value == "(":
                        depth += 1
                    elif items[index].value == ")":
                        depth -= 1
                    index += 1
                continue
            elif not angle_depth and value in {"=", "(", "{"}:
                return items[:index]
            index += 1
        return items

    type_prefixes = {
        "const", "constexpr", "extern", "inline", "mutable", "register",
        "static", "thread_local", "volatile",
    }

    def outer_type_name(type_values: list[str]) -> str | None:
        values = list(type_values)
        while values and values[0] in type_prefixes:
            values.pop(0)
        while values and values[-1] in {"*", "&", "&&", "const", "volatile"}:
            values.pop()
        if not values or values[0] == "decltype":
            return None
        template = values.index("<") if "<" in values else len(values)
        identifiers = [
            value for value in values[:template]
            if re.fullmatch(r"[A-Za-z_]\w*", value)
        ]
        return identifiers[-1] if identifiers else None

    def std_macro_is_reachable(position: int) -> bool:
        event_index = bisect.bisect_right(
            macro_environment_positions, position
        ) - 1
        environments = (
            ({},) if event_index < 0
            else macro_environment_events[event_index][1]
        )
        return (environments is None
                or any("std" in environment for environment in environments))

    def visible_std_shadow(position: int, globally_qualified: bool) -> bool:
        for alias in aliases_by_name.get("std", ()):
            if (alias.declaration > position
                    or not alias.scope_start < position < alias.scope_stop):
                continue
            if (globally_qualified
                    and (alias.scope_start, alias.scope_stop) != root_scope):
                continue
            return True
        for definition in std_namespace_definitions:
            if (definition.declaration > position
                    or not (definition.scope_start
                            < position < definition.scope_stop)):
                continue
            if globally_qualified and not definition.global_definition:
                continue
            return True
        return False

    def real_std_namespace(position: int, globally_qualified: bool) -> bool:
        return (not std_macro_is_reachable(position)
                and not visible_std_shadow(position, globally_qualified))

    def classify_type_category(
        type_values: list[str],
        position: int,
        seen: frozenset[str],
    ) -> str | None:
        while type_values and type_values[-1] in {"*", "&", "&&", "const", "volatile"}:
            type_values.pop()
        normalized = list(type_values)
        while normalized and normalized[0] in type_prefixes:
            normalized.pop(0)
        if normalized and normalized[0] == "decltype":
            return "ordinary"
        if not balanced_type_prefix(type_values):
            return None
        outer_name = outer_type_name(type_values)
        if outer_name == "reference_wrapper" and "<" in normalized:
            template_opening = normalized.index("<")
            type_head = normalized[:template_opening]
            globally_qualified = type_head == [
                "::", "std", "::", "reference_wrapper"
            ]
            if (type_head in (["std", "::", "reference_wrapper"],
                              ["::", "std", "::", "reference_wrapper"])
                    and real_std_namespace(position, globally_qualified)):
                template_closing = len(normalized) - 1
                while (template_closing > template_opening
                       and normalized[template_closing] != ">"):
                    template_closing -= 1
                if template_closing > template_opening + 1:
                    wrapped = classify_type_category(
                        normalized[template_opening + 1:template_closing],
                        position,
                        seen,
                    )
                    if wrapped is not None:
                        return f"reference_wrapper:{wrapped}"
        if outer_name == "GpuSyncReadScope":
            return "scope"
        if outer_name == "GpuReadLease":
            return "lease"
        if outer_name is not None:
            alias_category = resolve_alias_category(
                outer_name, position, seen
            )
            if alias_category is not None:
                return alias_category
        return "ordinary"

    def resolve_alias_category(name: str, position: int,
                               seen: frozenset[str] = frozenset()) -> str | None:
        if name in seen:
            return "unresolved"
        alias = max((
            candidate for candidate in aliases_by_name.get(name, ())
            if (candidate.declaration <= position
                and candidate.scope_start < position < candidate.scope_stop)
        ), key=lambda candidate: candidate.declaration, default=None)
        if alias is None:
            # A known but not-yet-visible alias target is invalid C++; retain a
            # fail-closed category rather than laundering it into an ordinary
            # type. This also terminates mutually recursive alias graphs.
            return "unresolved" if name in aliases_by_name else None
        if alias.target == ordinary_type_shadow:
            return "ordinary"
        return classify_type_category(
            list(alias.target), alias.target_position, seen | {name}
        )

    def category_for(type_values: list[str], position: int) -> str | None:
        return classify_type_category(type_values, position, frozenset())

    def add_type_alias(items: list[CppToken]) -> bool:
        if not items:
            return False
        name_token: CppToken | None = None
        target: list[CppToken] = []
        if items[0].value == "using" and len(items) >= 4:
            equals = next((
                index for index, item in enumerate(items) if item.value == "="
            ), None)
            if equals == 2 and re.fullmatch(
                    r"[A-Za-z_]\w*", items[1].value):
                name_token = items[1]
                target = items[equals + 1:]
        elif items[0].value == "namespace" and len(items) >= 4:
            equals = next((
                index for index, item in enumerate(items) if item.value == "="
            ), None)
            if equals == 2 and re.fullmatch(
                    r"[A-Za-z_]\w*", items[1].value):
                name_token = items[1]
                target = items[equals + 1:]
        elif items[0].value == "typedef" and len(items) >= 3:
            name_index = next((
                index for index in range(len(items) - 1, 0, -1)
                if re.fullmatch(r"[A-Za-z_]\w*", items[index].value)
            ), None)
            if name_index is not None:
                name_token = items[name_index]
                target = items[1:name_index]
        if name_token is None or not target:
            return False
        scope_start, scope_stop = lexical_scope(name_token.start)
        aliases_by_name.setdefault(name_token.value, []).append(SourceTypeAlias(
            name_token.value, tuple(item.value for item in target),
            target[0].start, name_token.end, scope_start, scope_stop,
        ))
        return True

    def add_ordinary_type_shadow(
        name_token: CppToken,
        forced_scope: tuple[int, int] | None = None,
        declaration: int | None = None,
    ) -> None:
        scope_start, scope_stop = forced_scope or lexical_scope(name_token.start)
        aliases_by_name.setdefault(name_token.value, []).append(SourceTypeAlias(
            name_token.value,
            ordinary_type_shadow,
            name_token.start,
            name_token.end if declaration is None else declaration,
            scope_start,
            scope_stop,
        ))

    # Type declarations and template type parameters live in the same lookup
    # namespace as aliases. Record their lexical shadowing before parsing value
    # declarations so a prior capability alias cannot leak through the new type.
    template_parameter_tokens: set[int] = set()
    token_index = 0
    while token_index + 1 < len(tokens):
        if (tokens[token_index].value != "template"
                or tokens[token_index + 1].value != "<"):
            token_index += 1
            continue
        opening = token_index + 1
        depth = 1
        closing = opening + 1
        while closing < len(tokens) and depth > 0:
            if tokens[closing].value == "<":
                depth += 1
            elif tokens[closing].value == ">":
                depth -= 1
            elif tokens[closing].value == ">>":
                depth -= 2
            closing += 1
        if depth > 0:
            token_index += 1
            continue
        closing -= 1
        template_parameter_tokens.update(range(opening + 1, closing))
        body_index = next((
            cursor for cursor in range(closing + 1, len(tokens))
            if tokens[cursor].value in {"{", ";"}
        ), None)
        template_scope: tuple[int, int] | None = None
        if (body_index is not None and tokens[body_index].value == "{"
                and tokens[body_index].start in brace_closings):
            template_scope = (
                tokens[token_index].start - 1,
                brace_closings[tokens[body_index].start],
            )
        elif body_index is not None:
            template_scope = (
                tokens[token_index].start - 1,
                tokens[body_index].end,
            )
        parameter_depth = 1
        cursor = opening + 1
        while cursor < closing:
            value = tokens[cursor].value
            if value == "<":
                parameter_depth += 1
            elif value == ">":
                parameter_depth -= 1
            elif value == ">>":
                parameter_depth -= 2
            elif (parameter_depth == 1 and value in {"class", "typename"}
                  and cursor + 1 < closing
                  and re.fullmatch(
                      r"[A-Za-z_]\w*", tokens[cursor + 1].value
                  )):
                add_ordinary_type_shadow(
                    tokens[cursor + 1], template_scope,
                    tokens[cursor + 1].end,
                )
                cursor += 1
            cursor += 1
        token_index = closing + 1

    for declaration_index, token in enumerate(tokens):
        if (declaration_index in template_parameter_tokens
                or token.value not in {"class", "enum", "struct", "union"}):
            continue
        name_index = declaration_index + 1
        if (token.value == "enum" and name_index < len(tokens)
                and tokens[name_index].value in {"class", "struct"}):
            name_index += 1
        if (name_index >= len(tokens)
                or name_index in template_parameter_tokens
                or re.fullmatch(
                    r"[A-Za-z_]\w*", tokens[name_index].value
                ) is None
                or (name_index + 1 < len(tokens)
                    and tokens[name_index + 1].value == "::")):
            continue
        add_ordinary_type_shadow(tokens[name_index])

    # Namespace definitions participate in deciding whether a spelled ``std``
    # denotes the external standard namespace. Keep definitions separate from
    # type aliases: unqualified lookup follows the enclosing lexical namespace,
    # while leading ``::std`` is affected only by a root definition.
    for namespace_index, token in enumerate(tokens):
        if token.value != "namespace":
            continue
        cursor = namespace_index + 1
        if (cursor >= len(tokens)
                or re.fullmatch(r"[A-Za-z_]\w*", tokens[cursor].value) is None):
            continue
        component_indices = [cursor]
        cursor += 1
        while cursor < len(tokens) and tokens[cursor].value == "::":
            cursor += 1
            if cursor < len(tokens) and tokens[cursor].value == "inline":
                cursor += 1
            if (cursor >= len(tokens)
                    or re.fullmatch(
                        r"[A-Za-z_]\w*", tokens[cursor].value
                    ) is None):
                component_indices = []
                break
            component_indices.append(cursor)
            cursor += 1
        if (not component_indices or cursor >= len(tokens)
                or tokens[cursor].value != "{"):
            continue
        body_closing = brace_closings.get(tokens[cursor].start)
        if body_closing is None:
            continue
        enclosing_scope = lexical_scope(token.start)
        body_scope = (tokens[cursor].start, body_closing)
        for component_number, component_index in enumerate(component_indices):
            component = tokens[component_index]
            if component.value != "std":
                continue
            first_component = component_number == 0
            visibility_scope = enclosing_scope if first_component else body_scope
            std_namespace_definitions.append(SourceNamespaceDefinition(
                component.end,
                visibility_scope[0],
                visibility_scope[1],
                first_component and enclosing_scope == root_scope,
            ))

    def add_statement_declarations(items: list[CppToken],
                                   forced_scope: tuple[int, int] | None = None) -> None:
        # Member-call statements can contain arbitrarily large nested callback
        # bodies. They cannot begin a declaration, so reject before balanced
        # declarator splitting rather than rescanning every nested suffix.
        if len(items) > 1 and items[1].value in {".", "->"}:
            return
        while len(items) >= 4 and items[0].value == "[" and items[1].value == "[":
            depth = 1
            cursor = 2
            while cursor + 1 < len(items) and depth:
                if items[cursor].value == "[" and items[cursor + 1].value == "[":
                    depth += 1
                    cursor += 2
                    continue
                if items[cursor].value == "]" and items[cursor + 1].value == "]":
                    depth -= 1
                    cursor += 2
                    continue
                cursor += 1
            if depth:
                return
            items = items[cursor:]
        auto_index = next((
            index for index, item in enumerate(items)
            if item.value == "auto"
        ), None)
        allowed_before_auto = {
            "const", "constexpr", "static", "thread_local", "volatile",
        }
        structured_opening = None
        if (auto_index is not None
                and all(item.value in allowed_before_auto
                        for item in items[:auto_index])):
            candidate_opening = next((
                index for index, item in enumerate(items[auto_index + 1:], auto_index + 1)
                if item.value == "["
            ), None)
            if (candidate_opening is not None
                    and all(item.value in {"&", "&&", "const", "volatile"}
                            for item in items[auto_index + 1:candidate_opening])):
                structured_opening = candidate_opening
        if structured_opening is not None:
            depth = 1
            closing = structured_opening + 1
            while closing < len(items) and depth:
                if items[closing].value == "[":
                    depth += 1
                elif items[closing].value == "]":
                    depth -= 1
                closing += 1
            if depth:
                return
            for name_token in items[structured_opening + 1:closing - 1]:
                if re.fullmatch(r"[A-Za-z_]\w*", name_token.value) is None:
                    continue
                scope_start, scope_stop = forced_scope or lexical_scope(name_token.start)
                bindings.append(SourceBinding(
                    name_token.value, "ordinary", name_token.end,
                    scope_start, scope_stop,
                ))
            return
        chunks = top_level_chunks(items)
        if not chunks:
            return
        first_lhs = declarator_lhs(chunks[0])
        if (len(first_lhs) < 2
                or first_lhs[0].value in {
                    "class", "enum", "for", "if", "return", "struct", "switch",
                    "typedef", "union", "using", "while",
                }):
            return
        first_name_index = next((
            index for index in range(len(first_lhs) - 1, -1, -1)
            if re.fullmatch(r"[A-Za-z_]\w*", first_lhs[index].value)
        ), None)
        if first_name_index is None or first_name_index == 0:
            return
        common_type = [item.value for item in first_lhs[:first_name_index]]
        category = category_for(list(common_type), first_lhs[0].start)
        if category is None:
            return
        for chunk_index, chunk in enumerate(chunks):
            lhs = declarator_lhs(chunk)
            name_index = next((
                index for index in range(len(lhs) - 1, -1, -1)
                if re.fullmatch(r"[A-Za-z_]\w*", lhs[index].value)
            ), None)
            if name_index is None or (chunk_index == 0 and name_index == 0):
                continue
            name_token = lhs[name_index]
            if chunk_index:
                prefix = [item.value for item in lhs[:name_index]]
                if any(value not in {"*", "&", "&&", "const", "volatile"}
                       for value in prefix):
                    continue
            scope_start, scope_stop = forced_scope or lexical_scope(name_token.start)
            bindings.append(SourceBinding(
                name_token.value, category, name_token.end, scope_start, scope_stop
            ))

    closing_to_opening: dict[int, int] = {}
    delimiter_stack: list[tuple[str, int]] = []
    opening_for = {")": "(", "]": "[", "}": "{"}
    for index, token in enumerate(tokens):
        if token.value in {"(", "[", "{"}:
            delimiter_stack.append((token.value, index))
        elif token.value in opening_for and delimiter_stack:
            expected = opening_for[token.value]
            if delimiter_stack[-1][0] == expected:
                _value, opening = delimiter_stack.pop()
                closing_to_opening[index] = opening

    for semicolon, token in enumerate(tokens):
        if token.value != ";":
            continue
        cursor = semicolon - 1
        while cursor >= 0:
            value = tokens[cursor].value
            if value in {")", "]"} and cursor in closing_to_opening:
                cursor = closing_to_opening[cursor] - 1
                continue
            if value == "}" and cursor in closing_to_opening:
                following = tokens[cursor + 1].value if cursor + 1 < semicolon else ";"
                if cursor == semicolon - 1 or following == ",":
                    cursor = closing_to_opening[cursor] - 1
                    continue
                break
            if value in {";", "{", "}"}:
                break
            cursor -= 1
        statement_items = tokens[cursor + 1:semicolon]
        if not add_type_alias(statement_items):
            add_statement_declarations(statement_items)

    opening_stack: list[int] = []
    closing_to_opening: dict[int, int] = {}
    for index, token in enumerate(tokens):
        if token.value == "(":
            opening_stack.append(index)
        elif token.value == ")" and opening_stack:
            closing_to_opening[index] = opening_stack.pop()
    for closing, opening in closing_to_opening.items():
        if closing + 1 >= len(tokens) or tokens[closing + 1].value != "{":
            continue
        start = opening + 1
        depth = 0
        segment_start = start
        segments: list[list[CppToken]] = []
        for index in range(start, closing):
            value = tokens[index].value
            if value in {"(", "[", "<"}:
                depth += 1
            elif value in {")", "]", ">"}:
                depth = max(0, depth - 1)
            if value == "," and depth == 0:
                segments.append(tokens[segment_start:index])
                segment_start = index + 1
        segments.append(tokens[segment_start:closing])
        body_opening = tokens[closing + 1]
        body_closing = brace_closings.get(body_opening.start)
        body_pair = (
            (body_opening.start, body_closing)
            if body_closing is not None else None
        )
        if body_pair is None:
            continue
        for segment in segments:
            range_colon = next((
                index for index, item in enumerate(segment)
                if item.value == ":"
            ), None)
            declaration_segment = (
                segment[:range_colon] if range_colon is not None else segment
            )
            add_statement_declarations(declaration_segment, body_pair)

    read_result = re.compile(
        r"\b(?:const\s+)?auto\s+([A-Za-z_]\w*)\s*=\s*"
        r"([A-Za-z_]\w*)\s*\.\s*read\s*\("
    )
    binding_index = _source_binding_index(bindings, masked)
    binding_positions: dict[tuple[str, int], list[int]] = {}
    for binding_position, binding in enumerate(bindings):
        binding_positions.setdefault(
            (binding.name, binding.declaration), []
        ).append(binding_position)
    for match in read_result.finditer(masked):
        scope_binding = _resolve_source_binding(
            binding_index, match.group(2), match.start(2)
        )
        if scope_binding is None or scope_binding.category != "scope":
            continue
        name_position = match.start(1)
        scope_start, scope_stop = lexical_scope(name_position)
        lease_binding = SourceBinding(
            match.group(1), "lease", match.end(1), scope_start, scope_stop
        )
        positions = binding_positions.get(
            (lease_binding.name, lease_binding.declaration), []
        )
        if positions:
            for binding_position in positions:
                bindings[binding_position] = lease_binding
        else:
            binding_positions.setdefault(
                (lease_binding.name, lease_binding.declaration), []
            ).append(len(bindings))
            bindings.append(lease_binding)
    return sorted(set(bindings), key=lambda binding: (
        binding.declaration, binding.scope_start, binding.name, binding.category
    ))


@dataclass(frozen=True)
class SourceBindingBucket:
    records: tuple[SourceBinding, ...]
    declarations: tuple[int, ...]


@dataclass(frozen=True)
class SourceBindingIndex:
    by_name_scope: Mapping[
        str, Mapping[tuple[int, int], SourceBindingBucket]
    ]
    parents: Mapping[tuple[int, int], tuple[int, int] | None]
    children: Mapping[tuple[int, int], tuple[tuple[int, int], ...]]
    child_starts: Mapping[tuple[int, int], tuple[int, ...]]
    scopes_by_token: Mapping[int, tuple[int, int]]
    root: tuple[int, int]
    nearest_scope_cache: dict[
        tuple[str, tuple[int, int]], tuple[int, int] | None
    ]

    def scope_at(self, position: int) -> tuple[int, int]:
        direct = self.scopes_by_token.get(position)
        if direct is not None:
            return direct
        return _source_scope_at_fallback(self, position)

    def nearest_binding_scope(
        self, name: str, scope: tuple[int, int] | None,
    ) -> tuple[int, int] | None:
        path: list[tuple[int, int]] = []
        scoped = self.by_name_scope.get(name, {})
        cursor = scope
        result: tuple[int, int] | None = None
        while cursor is not None:
            key = (name, cursor)
            if key in self.nearest_scope_cache:
                result = self.nearest_scope_cache[key]
                break
            path.append(cursor)
            if cursor in scoped:
                result = cursor
                break
            cursor = self.parents.get(cursor)
        for visited in path:
            self.nearest_scope_cache[(name, visited)] = result
        return result


def _source_scope_at_fallback(
    binding_index: SourceBindingIndex, position: int,
) -> tuple[int, int]:
    scope = binding_index.root
    while True:
        children = binding_index.children.get(scope, ())
        starts = binding_index.child_starts.get(scope, ())
        child_index = bisect.bisect_right(starts, position) - 1
        if child_index < 0:
            return scope
        child = children[child_index]
        if not child[0] < position < child[1]:
            return scope
        scope = child


def _resolve_source_binding(
    binding_index: SourceBindingIndex,
    name: str,
    position: int,
) -> SourceBinding | None:
    scope = binding_index.nearest_binding_scope(
        name, binding_index.scope_at(position)
    )
    scoped = binding_index.by_name_scope.get(name, {})
    while scope is not None:
        bucket = scoped.get(scope)
        if bucket is not None:
            record_index = bisect.bisect_right(
                bucket.declarations, position
            ) - 1
            if record_index >= 0:
                return bucket.records[record_index]
        scope = binding_index.nearest_binding_scope(
            name, binding_index.parents.get(scope)
        )
    return None


def _source_binding_index(
    bindings: Iterable[SourceBinding],
    masked: str,
) -> SourceBindingIndex:
    root = (0, len(masked))
    pairs = sorted(brace_pairs(masked))
    brace_closings = {opening: closing for opening, closing in pairs}
    parents: dict[tuple[int, int], tuple[int, int] | None] = {root: None}
    children_lists: dict[tuple[int, int], list[tuple[int, int]]] = {}
    stack: list[tuple[int, int]] = []
    for pair in pairs:
        while stack and pair[0] > stack[-1][1]:
            stack.pop()
        parent = stack[-1] if stack else root
        parents[pair] = parent
        children_lists.setdefault(parent, []).append(pair)
        stack.append(pair)
    grouped: dict[str, dict[tuple[int, int], list[SourceBinding]]] = {}
    for binding in bindings:
        scope = (binding.scope_start, binding.scope_stop)
        grouped.setdefault(binding.name, {}).setdefault(scope, []).append(binding)
    by_name_scope: dict[
        str, dict[tuple[int, int], SourceBindingBucket]
    ] = {}
    for name, scoped in grouped.items():
        by_name_scope[name] = {}
        for scope, records in scoped.items():
            ordered = tuple(sorted(records, key=lambda binding: binding.declaration))
            by_name_scope[name][scope] = SourceBindingBucket(
                ordered, tuple(binding.declaration for binding in ordered)
            )
    children = {
        scope: tuple(sorted(records))
        for scope, records in children_lists.items()
    }
    scopes_by_token: dict[int, tuple[int, int]] = {}
    lexical_stack: list[tuple[int, int]] = []
    for token in _compiler_tokens(masked):
        if token.value == "}" and lexical_stack:
            lexical_stack.pop()
        scopes_by_token[token.start] = lexical_stack[-1] if lexical_stack else root
        if token.value == "{" and token.start in brace_closings:
            lexical_stack.append((token.start, brace_closings[token.start]))
    return SourceBindingIndex(
        by_name_scope,
        parents,
        children,
        {
            scope: tuple(pair[0] for pair in records)
            for scope, records in children.items()
        },
        scopes_by_token,
        root,
        {},
    )


def _source_only_receiver_proof(
    masked: str,
    call_position: int,
    bindings: SourceBindingIndex,
) -> tuple[str | None, str | None]:
    """Return a proven receiver category and any unresolved value name.

    Complex operator receivers are accepted only when every referenced value is
    an ordinary binding. Calls are limited to the transparent standard-library
    forms understood by ``receiver_binding_name``; arbitrary wrappers and
    member ``get()`` calls do not inherit the category of an identifier merely
    mentioned inside them.
    """

    receiver_name = receiver_binding_name(masked, call_position)
    if receiver_name is not None:
        binding = _resolve_source_binding(
            bindings, receiver_name, call_position
        )
        if binding is None:
            return None, receiver_name
        if binding.category.startswith("reference_wrapper:"):
            return "ordinary", None
        return binding.category, None

    receiver_tokens = _compiler_tokens(receiver_expression(masked, call_position))
    start, stop = strip_transparent_parentheses(
        receiver_tokens, 0, len(receiver_tokens)
    )
    if (stop - start >= 5
            and [token.value for token in receiver_tokens[stop - 4:stop]]
            == [".", "get", "(", ")"]):
        base_start, base_stop = strip_transparent_parentheses(
            receiver_tokens, start, stop - 4
        )
        if (base_stop - base_start == 1
                and re.fullmatch(
                    r"[A-Za-z_]\w*", receiver_tokens[base_start].value
                )):
            wrapper_name = receiver_tokens[base_start].value
            wrapper_binding = _resolve_source_binding(
                bindings, wrapper_name, call_position
            )
            if wrapper_binding is None:
                return None, wrapper_name
            if wrapper_binding.category.startswith("reference_wrapper:"):
                return wrapper_binding.category.split(":", 1)[1], None
        return None, None

    # Parentheses that remain after stripping the whole receiver denote a call
    # or a nested subexpression. The only transparent calls are resolved above.
    if any(
        token.value in {"(", ")", "{", "}"}
        for token in receiver_tokens[start:stop]
    ):
        references = receiver_binding_references(masked, call_position)
        unresolved = next((
            name for name in sorted(references)
            if _resolve_source_binding(bindings, name, call_position) is None
        ), None)
        return None, unresolved

    references = receiver_binding_references(masked, call_position)
    if not references:
        return None, None
    categories: list[str] = []
    for name in sorted(references):
        binding = _resolve_source_binding(bindings, name, call_position)
        if binding is None:
            return None, name
        categories.append(
            "ordinary" if binding.category.startswith("reference_wrapper:")
            else binding.category
        )
    if all(category == "ordinary" for category in categories):
        return "ordinary", None
    return next(
        category for category in categories if category != "ordinary"
    ), None


def _source_only_sensitive_ranges(
    masked: str, directive_ranges: list[tuple[int, int]],
) -> list[tuple[int, int, str]]:
    """Precompute capability-sensitive receiver/member/callback call ranges."""

    directive_starts = tuple(start for start, _stop in directive_ranges)

    def in_directive(position: int) -> bool:
        range_index = bisect.bisect_right(directive_starts, position) - 1
        return (range_index >= 0
                and position < directive_ranges[range_index][1])

    bindings = _source_binding_index(
        _source_only_declared_bindings(masked, directive_ranges), masked
    )
    parenthesis_closings = {
        opening: closing for opening, closing in delimiter_pairs(masked, "(", ")")
    }
    ranges: list[tuple[int, int, str]] = []
    for call in MEMBER_CALL.finditer(masked):
        if in_directive(call.start()):
            continue
        operation = call.group(1)
        if operation not in {"read", "withRead", "complete", "nativeHandle"}:
            continue
        receiver_category, _unresolved = _source_only_receiver_proof(
            masked, call.start(), bindings
        )
        if receiver_category == "ordinary":
            continue
        receiver = receiver_expression(masked, call.start())
        receiver_start = max(0, call.start() - len(receiver.rstrip()))
        closing = parenthesis_closings.get(call.end() - 1)
        call_stop = closing + 1 if closing is not None else call.end()
        ranges.append((receiver_start, call.start(), "receiver"))
        ranges.append((call.start(), call.end(), "member"))
        if operation == "withRead":
            ranges.append((call.end(), call_stop, "callback"))
        elif operation == "complete":
            ranges.append((call.start(), call_stop, "completion"))
    generic_member = re.compile(r"(?:\.|->|::)\s*([A-Za-z_]\w*)\s*\(")
    for call in generic_member.finditer(masked):
        if in_directive(call.start()) or masked.startswith("::", call.start()):
            continue
        receiver_category, _unresolved = _source_only_receiver_proof(
            masked, call.start(), bindings
        )
        if receiver_category == "ordinary":
            continue
        receiver = receiver_expression(masked, call.start())
        receiver_start = max(0, call.start() - len(receiver.rstrip()))
        ranges.append((receiver_start, call.start(), "receiver"))
        ranges.append((call.start(), call.end(), "member"))
    return ranges


def _source_only_unknown_macro_findings(
    path: PurePosixPath, translated: TranslationText
) -> list[Finding]:
    """Reject unresolved macro-like names only where they can change capability grammar."""

    masked = translated.masked
    environment_events, directive_ranges = source_macro_environment_events(masked)
    directive_starts = tuple(start for start, _stop in directive_ranges)
    sensitive: list[tuple[int, str]] = []
    sensitive_intervals: list[tuple[int, int]] = []

    def in_directive(position: int) -> bool:
        range_index = bisect.bisect_right(directive_starts, position) - 1
        return (range_index >= 0
                and position < directive_ranges[range_index][1])

    def add_interval(start: int, stop: int) -> None:
        if start < stop:
            sensitive_intervals.append((start, stop))

    parenthesis_closings: dict[int, int] = {}
    parenthesis_stack: list[int] = []
    for token in _compiler_tokens(masked):
        if token.value == "(":
            parenthesis_stack.append(token.start)
        elif token.value == ")" and parenthesis_stack:
            parenthesis_closings[parenthesis_stack.pop()] = token.start

    bindings = _source_binding_index(
        _source_only_declared_bindings(masked, directive_ranges), masked
    )
    for call in MEMBER_CALL.finditer(masked):
        if in_directive(call.start()):
            continue
        operation = call.group(1)
        if operation not in {"read", "withRead", "complete", "nativeHandle"}:
            continue
        receiver = receiver_expression(masked, call.start())
        receiver_start = call.start() - len(receiver.rstrip())
        receiver_category, unresolved_receiver = _source_only_receiver_proof(
            masked, call.start(), bindings
        )
        if receiver_category == "ordinary":
            continue
        add_interval(max(0, receiver_start), call.start())
        if receiver_category is None:
            marker = unresolved_receiver or operation
            receiver_position = masked.rfind(
                marker, max(0, receiver_start), call.end()
            )
            if receiver_position >= 0:
                sensitive.append((receiver_position, marker))
        closing = parenthesis_closings.get(call.end() - 1)
        if operation == "withRead" and closing is not None:
            add_interval(call.end(), closing)

    merged_intervals: list[tuple[int, int]] = []
    for start, stop in sorted(sensitive_intervals):
        if merged_intervals and start <= merged_intervals[-1][1]:
            previous_start, previous_stop = merged_intervals[-1]
            merged_intervals[-1] = (previous_start, max(previous_stop, stop))
        else:
            merged_intervals.append((start, stop))
    for start, stop in merged_intervals:
        for match in _MACRO_LIKE_IDENTIFIER.finditer(masked, start, stop):
            prefix_end = match.start()
            while prefix_end > start and masked[prefix_end - 1].isspace():
                prefix_end -= 1
            prefix = masked[max(start, prefix_end - 2):prefix_end]
            if (in_directive(match.start())
                    or prefix.endswith(".")
                    or prefix.endswith("->")
                    or prefix.endswith("::")):
                continue
            sensitive.append((match.start(1), match.group(1)))

    known_capability_methods = {
        "complete", "desc", "nativeHandle", "nativeSubresource", "read",
        "valid", "withRead",
    }
    unknown_member = re.compile(r"(?:\.|->|::)\s*([A-Za-z_]\w*)\s*\(")
    for match in unknown_member.finditer(masked):
        if in_directive(match.start()):
            continue
        # A qualified static/type call is not an object receiver binding.
        if masked.startswith("::", match.start()):
            continue
        receiver_category, _unresolved = _source_only_receiver_proof(
            masked, match.start(), bindings
        )
        if (receiver_category != "ordinary"
                and match.group(1) not in known_capability_methods):
            sensitive.append((match.start(1), match.group(1)))

    findings: list[Finding] = []
    environments: tuple[dict[str, MacroDefinition], ...] | None = ({},)
    event_index = 0
    for position, name in sorted(set(sensitive)):
        while (event_index < len(environment_events)
               and environment_events[event_index][0] <= position):
            _event_position, environments = environment_events[event_index]
            event_index += 1
        # Conditional alternatives are audited by the per-environment expansion
        # phase. This phase reports names that are unresolved in every reachable
        # environment, never a lexical-final branch approximation.
        if environments is None or any(name in environment for environment in environments):
            continue
        findings.append(Finding(
            path,
            translated.line_at(position),
            "source-only macro ambiguity",
            f"unresolved macro-like {name} occurs in a capability-sensitive context",
        ))
    return findings


def audit_source_only(
    path: PurePosixPath,
    source: str,
    *,
    workspace=None,
    sink=None,
    provenance: str = "source-only",
    pipeline_deadline: float | None = None,
) -> list[Finding]:
    """Conservatively audit one path without claiming compiler coverage."""

    _check_policy_deadline(pipeline_deadline)
    if not is_production_path(path):
        return []
    if workspace is not None and sink is None:
        raise AuditInfrastructureError(
            "bounded source-only policy workspace requires a sink"
        )
    scratch = (
        None
        if workspace is None
        else workspace.reserve_policy_scratch(len(source), source_only=True)
    )
    try:
        _check_policy_deadline(pipeline_deadline)
        translated = translate_source(source)
        _check_policy_deadline(pipeline_deadline)
        findings = list(audit_raw_sources(
            {path: source}, workspace=workspace, sink=sink,
            provenance=provenance,
            pipeline_deadline=pipeline_deadline,
        ))
        _check_policy_deadline(pipeline_deadline)
        composition_findings = guarded_macro_composition_findings(
            path, translated, source_only=True
        )
        _check_policy_deadline(pipeline_deadline)
        unknown_findings = _source_only_unknown_macro_findings(path, translated)
        _check_policy_deadline(pipeline_deadline)
        if sink is not None:
            for finding in composition_findings:
                sink(finding, provenance)
            composition_findings.clear()
            for finding in unknown_findings:
                sink(finding, provenance)
            unknown_findings.clear()
            findings.clear()
            translated = None
            _check_policy_deadline(pipeline_deadline)
            return []
        findings.extend(composition_findings)
        findings.extend(unknown_findings)
        result = sorted(
            set(findings),
            key=lambda item: (
                item.path.as_posix(), item.line, item.expression, item.reason
            ),
        )
        _check_policy_deadline(pipeline_deadline)
        return result
    finally:
        if scratch is not None:
            workspace.release_policy_scratch(scratch)


def audit_pipeline(
    sources: Mapping[PurePosixPath, str],
    views: Iterable[PreprocessedTranslationUnitView],
    coverage: CoverageReport,
) -> tuple[list[AggregatedFinding], CoverageReport]:
    """Union the three disjoint audit lanes without inventing native coverage."""

    observations: list[tuple[Finding, str]] = [
        (finding, "raw-source") for finding in audit_raw_sources(sources)
    ]
    for view in views:
        observations.extend(
            (finding, view.configuration.digest)
            for finding in audit_preprocessed_view(
                view, AuditLimits(), _current_process_rss_bytes
            )
        )
    for path in sorted(coverage.source_only, key=PurePosixPath.as_posix):
        source = sources.get(path)
        if source is None:
            raise AuditInfrastructureError(
                f"source-only coverage references missing production path: {path}"
            )
        observations.extend(
            (finding, "source-only")
            for finding in audit_source_only(path, source)
        )
    return aggregate_findings(observations), coverage


def premeasure_streaming_policy_growth(
    sources: Mapping[PurePosixPath, str],
    compact_finding_count: int,
    *,
    configuration_provenance_count: int = 0,
    authoritative_paths: tuple[PurePosixPath, ...] = (),
    path_render_bytes: int = AuditLimits().compact_result_path_bytes,
    pipeline_deadline: float | None = None,
) -> int:
    """Conservatively own policy observations, findings and provenance fan-out."""

    if (
        not isinstance(sources, Mapping)
        or not isinstance(compact_finding_count, int)
        or isinstance(compact_finding_count, bool)
        or compact_finding_count < 0
        or not isinstance(configuration_provenance_count, int)
        or isinstance(configuration_provenance_count, bool)
        or configuration_provenance_count < 0
        or not isinstance(authoritative_paths, tuple)
        or not isinstance(path_render_bytes, int)
        or isinstance(path_render_bytes, bool)
        or path_render_bytes <= 0
    ):
        raise AuditInfrastructureError(
            "streaming policy growth inputs are invalid"
        )
    schema = conservative_allocation_schema()
    _check_policy_deadline(pipeline_deadline)
    for path in authoritative_paths:
        if not isinstance(path, PurePosixPath):
            raise AuditInfrastructureError(
                "streaming policy growth inputs are invalid"
            )
    spellings = (
        "nativeHandle",
        "GpuSyncReadScope",
        "GpuReadLease",
        "GpuSurface",
        "GpuRetireRegistry",
        "registerRetire",
        "GpuOpScope",
        "track",
        "withRead",
        "read",
        "complete",
    )
    candidate_count = 0
    directive_count = 0
    phase_two_count = 0
    capability_source_characters = 0
    path_bytes = schema.checked_multiply(len(sources), path_render_bytes)
    source_characters = 0
    maximum_raw_source_characters = 0
    maximum_source_only_characters = 0
    for path, source in sources.items():
        _check_policy_deadline(pipeline_deadline)
        if not isinstance(path, PurePosixPath) or not isinstance(source, str):
            raise AuditInfrastructureError(
                "streaming policy growth source is invalid"
            )
        source_characters = schema.checked_add(
            source_characters, len(source)
        )
        maximum_raw_source_characters = max(
            maximum_raw_source_characters, len(source)
        )
        maximum_source_only_characters = max(
            maximum_source_only_characters, len(source)
        )
        capability_occurrences = 0
        for spelling in spellings:
            _check_policy_deadline(pipeline_deadline)
            occurrences = source.count(spelling)
            _check_policy_deadline(pipeline_deadline)
            capability_occurrences = schema.checked_add(
                capability_occurrences, occurrences
            )
            candidate_count = schema.checked_add(
                candidate_count, occurrences
            )
        if capability_occurrences:
            capability_source_characters = schema.checked_add(
                capability_source_characters, len(source)
            )
        directive_count = schema.checked_add(
            directive_count, source.count("#")
        )
        _check_policy_deadline(pipeline_deadline)
        phase_two_count = schema.checked_add(
            phase_two_count,
            source.count("\\\n"),
            source.count("\\\r\n"),
        )
        in_macro = False
        macro_length = 0
        index = 0
        while index < len(source):
            _check_policy_deadline(pipeline_deadline)
            chunk_end = min(len(source), index + (64 * 1024))
            while index < chunk_end:
                character = source[index]
                macro_character = character == "_" or character.isupper()
                macro_continuation = macro_character or character.isdigit()
                if not in_macro and macro_character:
                    in_macro = True
                    macro_length = 1
                elif in_macro and macro_continuation:
                    macro_length += 1
                elif in_macro:
                    if macro_length >= 3:
                        candidate_count = schema.checked_add(candidate_count, 1)
                    in_macro = False
                    macro_length = 0
                index += 1
        if in_macro and macro_length >= 3:
            candidate_count = schema.checked_add(candidate_count, 1)
        _check_policy_deadline(pipeline_deadline)
    policy_finding_upper = schema.checked_add(
        schema.checked_multiply(candidate_count, 16),
        # One capability root can feed an attacker-sized alias/event graph.
        # A character count is a strict upper bound on lexical graph nodes.
        capability_source_characters,
        # Raw directive validation and source-only conditional state can each
        # emit several diagnostics for one source-authored directive.
        schema.checked_multiply(directive_count, 8),
        # Phase-two splice reconstruction is observed by both policy lanes.
        schema.checked_multiply(phase_two_count, 16),
        schema.checked_multiply(len(sources), 8),
    )
    observation_count = schema.checked_add(
        policy_finding_upper, compact_finding_count
    )
    provenance_count = schema.checked_add(
        policy_finding_upper,
        configuration_provenance_count,
    )
    value = schema.checked_add(
        65536,
        # Both policy lanes tokenize one source at a time.  Own their maximum
        # live grammar state from source length, independently of whether a
        # source happens to contain a capability spelling.  The source-only
        # lane carries the richer alias/scope/event grammar; these closed
        # per-character bounds exceed measured CPython peaks with headroom.
        schema.checked_multiply(
            maximum_raw_source_characters, schema.object_bound(0)
        ),
        schema.checked_multiply(
            maximum_source_only_characters, schema.object_bound(8)
        ),
        schema.checked_multiply(len(sources), schema.object_bound(8)),
        schema.checked_multiply(path_bytes, 4),
        schema.list_bound(observation_count),
        schema.checked_multiply(
            observation_count, schema.tuple_bound(2)
        ),
        schema.dict_bound(observation_count),
        schema.list_bound(provenance_count),
        schema.tuple_bound(provenance_count),
        schema.checked_multiply(
            observation_count, schema.object_bound(4)
        ),
        schema.string_objects_bound(
            schema.checked_add(
                schema.checked_multiply(source_characters, 4),
                schema.checked_multiply(path_bytes, 4),
                schema.checked_multiply(provenance_count, 64),
            ),
            schema.checked_add(
                schema.checked_multiply(observation_count, 4),
                provenance_count,
            ),
        ),
    )
    return value


def audit_sources(sources: Mapping[PurePosixPath, str]) -> list[Finding]:
    """Compatibility spelling for the raw lane; compiler tables are intentionally absent."""

    return audit_raw_sources(sources)


def is_production_path(path: PurePosixPath) -> bool:
    return (bool(path.parts) and path.parts[0] in PRODUCTION_ROOTS
            and path.suffix.lower() in SOURCE_SUFFIXES)


def mutation_self_tests() -> None:
    cases = (
        (PurePosixPath("playback/bad.cpp"),
         "void bad(GpuSurface* surface) { (void) surface->nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); scope.complete(); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); (void) lease.nativeHandle(); }",
         "complete()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { (void) lease.nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope{}; "
         "const GpuReadLease lease = (scope).read(s); scope.complete(); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { "
         "auto scope = GpuSyncReadScope{}; const GpuReadLease lease = scope.read(s); "
         "scope.complete(); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope const scope = makeScope(); "
         "const GpuReadLease lease = std::as_const(scope).read(s); scope.complete(); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { "
         "const GpuReadLease lease = GpuSyncReadScope {}.read(s); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(GpuSyncReadScope* const scope, const std::shared_ptr<GpuSurface>& s) { "
         "const GpuReadLease lease = (*scope).read(s); scope->complete(); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope{}; "
         "const GpuReadLease lease = (scope).read(s); "
         "(void) std::as_const(lease).nativeHandle(); }",
         "complete()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& surface) { "
         "(void) surface.get()->nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(GpuSurface& surface) { (void) (surface).nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(GpuSurface& surface) { (void) surface.GpuSurface::nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s, GpuSurface& surface) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); scope.complete(); "
         "(void) surface.nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s, GpuSurface& surface) { "
         "GpuSyncReadScope scope; scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); (void) surface.nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.complete(); const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); void* handle = lease.nativeHandle(); "
         "maybeThrows(handle); scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& a, "
         "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope scope; "
         "scope.withRead(a, [](const GpuReadLease& first) { "
         "void* handle = first.nativeHandle(); }); "
         "const GpuReadLease second = scope.read(b); void* handle = second.nativeHandle(); "
         "scope.complete(); }",
         "exactly one acquisition"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.complete(); scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); }); }",
         "complete() cannot precede acquisition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); void* handle = lease.nativeHandle(); "
         "scope.complete(); scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& a, "
         "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope first; "
         "const GpuReadLease firstLease = first.read(a); first.complete(); "
         "GpuSyncReadScope second; const GpuReadLease secondLease = second.read(b); "
         "(void) secondLease.nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& a, "
         "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(a); (void) lease.nativeHandle(); "
         "scope.complete(); { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(b); (void) lease.nativeHandle(); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s, GpuSurface& surface) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "{ GpuSurface& lease = surface; (void) lease.nativeHandle(); } scope.complete(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); (void) lease.nativeHandle(); "
         "{ OtherScope scope; scope.complete(); } }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ OtherScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); } scope.complete(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ OtherScope scope; scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); (void) lease.nativeHandle(); "
         "auto finish = [&]() { scope.complete(); }; (void) finish; }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool finish, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (finish) { scope.complete(); } }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); (void) lease.nativeHandle(); "
         "[&]() { scope.complete(); }(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ OtherScope scope(s); const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); scope.complete(); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ auto scope(makeOtherScope()); const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); scope.complete(); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ auto scope{makeOtherScope()}; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); scope.complete(); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ OtherScope scope[1]; const GpuReadLease lease = scope[0].read(s); "
         "(void) lease.nativeHandle(); scope[0].complete(); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ decltype(makeOtherScope()) scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); scope.complete(); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "[scope = OtherScope{} , &s]() { const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); scope.complete(); }(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "[scope = OtherScope{}, &s]() { scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); }(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "[&s](OtherScope scope) { scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); }(OtherScope{}); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "[&s](auto scope) { scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); }(OtherScope{}); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ OtherScope other, scope; scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "{ auto [scope, ignored] = makeOtherScopes(); "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool enabled, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; if (enabled) { scope.withRead(s, "
         "[](const GpuReadLease& lease) { (void) lease.nativeHandle(); }); } }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(makeSurface([](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); }), [](const GpuReadLease&) {}); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, wrap([](const GpuReadLease& lease) { "
         "(void) lease.nativeHandle(); })); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool finish, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (finish) scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool finish, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); finish ? scope.complete() : void(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool finish, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); finish && (scope.complete(), true); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool finish, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); finish || (scope.complete(), true); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool stop, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (stop) return; scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool stop, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (stop) throw Failure{}; scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool stop, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (stop) goto done; scope.complete(); done:; }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); (void) lease.nativeHandle(); "
         "return scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); (void) lease.nativeHandle(); "
         "(scope.complete(), other()); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& a, "
         "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope scope; "
         "const GpuReadLease first = scope.read(a); (void) first.nativeHandle(); "
         "scope.complete(); const GpuReadLease second = scope.read(b); "
         "(void) second.nativeHandle(); scope.complete(); }",
         "exactly one read()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "task bad(bool stop, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (stop) co_return; scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool stop, const std::shared_ptr<GpuSurface>& s) { while (true) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (stop) break; scope.complete(); } }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool stop, const std::shared_ptr<GpuSurface>& s) { while (true) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "(void) lease.nativeHandle(); if (stop) continue; scope.complete(); } }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); void* escaped = lease.nativeHandle(); "
         "scope.complete(); consume(escaped); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "uintptr_t bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); const uintptr_t escaped = "
         "reinterpret_cast<uintptr_t>(lease.nativeHandle()); scope.complete(); return escaped; }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [=] { "
         "(void) isCompatibleWithNativeHandle(handle); }; later(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&] { "
         "(void) isCompatibleWithNativeHandle(handle); }; later(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&]() constexpr { "
         "(void) isCompatibleWithNativeHandle(handle); }; later(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&]() consteval { "
         "(void) isCompatibleWithNativeHandle(handle); }; later(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&]<typename T>(T) { "
         "(void) isCompatibleWithNativeHandle(handle); }; later(1); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&] [[nodiscard]] () { "
         "(void) isCompatibleWithNativeHandle(handle); }; later(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&]<typename T>() "
         "requires true { (void) isCompatibleWithNativeHandle(handle); }; "
         "later.operator()<int>(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = [&]<typename T>() "
         "constexpr noexcept -> bool requires true { "
         "return isCompatibleWithNativeHandle(handle); }; "
         "(void) later.operator()<int>(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); auto later = <: = :><typename T>() "
         "constexpr noexcept -> bool requires true { "
         "return isCompatibleWithNativeHandle(handle); }; "
         "(void) later.operator()<int>(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { g_lease = &lease; }); }",
         "lease reference must remain inside the immediate callback body"),
        (PurePosixPath("playback/bad.cpp"),
         "void C::bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [&](const GpuReadLease& lease) { m_lease = &lease; }); }",
         "lease reference must remain inside the immediate callback body"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { leases.push_back(&lease); }); }",
         "lease reference must remain inside the immediate callback body"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { auto later = [&lease] { "
         "consume(lease.valid()); }; later(); }); }",
         "lease reference must remain inside the immediate callback body"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "storeGlobally(static_cast<const void*>(&lease)); }); }",
         "lease reference must remain inside the immediate callback body"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { void* escaped = nullptr; "
         "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
         "escaped = lease.nativeHandle(); }); consume(escaped); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/bad.cpp"),
         "void C::bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [&](const GpuReadLease& lease) { "
         "m_handle = lease.nativeHandle(); }); consume(m_handle); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [&](const GpuReadLease& lease) { "
         "handles.push_back(lease.nativeHandle()); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/bad.cpp"),
         "NativeBox bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "return scope.withRead(s, [](const GpuReadLease& lease) { "
         "return NativeBox(lease.nativeHandle()); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = wrap(lease.nativeHandle()); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = storeGlobally(lease.nativeHandle()); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "static void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool enabled, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
         "void* handle = enabled ? lease.nativeHandle() : nullptr; "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "::g_handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(g_handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define PERSIST static\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "PERSIST void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define PERSIST thread_local\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "PERSIST void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { void* escaped = nullptr; "
         "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); if (escaped = handle) consume(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { void* escaped = nullptr; "
         "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle((escaped = handle, handle)); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "NativeBox holder{lease.nativeHandle()}; consume(holder); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& a, "
         "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope scope; "
         "scope.withRead(a, [](const GpuReadLease& lease) { consume(lease.nativeHandle()); }); "
         "scope.withRead(b, [](const GpuReadLease& lease) { consume(lease.nativeHandle()); }); }",
         "exactly one acquisition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& a, "
         "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope scope; "
         "const GpuReadLease first = scope.read(a); consume(first.nativeHandle()); "
         "scope.withRead(b, [](const GpuReadLease& second) { "
         "consume(second.nativeHandle()); }); scope.complete(); }",
         "exactly one acquisition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.complete(); const GpuReadLease lease = scope.read(s); "
         "consume(lease.nativeHandle()); scope.complete(); }",
         "complete() cannot precede acquisition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); scope.complete(); "
         "consume(lease.nativeHandle()); scope.complete(); }",
         "native handle access cannot follow complete()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "bool bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.read(s); void* handle = lease.nativeHandle(); "
         "scope.complete(); return handle != nullptr; }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { void* escaped = nullptr; "
         "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); escaped = handle ? handle : nullptr; }); "
         "consume(escaped); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { const char* text = R\"(raw quote \" "
         "tail)\"; (void) text; (void) lease.nativeHandle(); }",
         "GpuSurface::nativeHandle()"),
        (PurePosixPath("playback/bad.cpp"),
         "#define HANDLE nativeHandle\n"
         "void bad(const GpuReadLease& lease) { (void) lease.HANDLE(); }",
         "nativeHandle alias"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { "
         "auto access = &GpuReadLease::nativeHandle; (void) (lease.*access)(); }",
         "nativeHandle reference"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { (void) lease.native" + chr(92) +
         "\nHandle(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/bad.cpp"),
         "#define HANDLE native" + chr(92) + "\nHandle\n"
         "void bad(const GpuReadLease& lease) { (void) lease.HANDLE(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { auto access = "
         "&GpuReadLease::native" + chr(92) + "\nHandle; (void) (lease.*access)(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(bool stop, const std::shared_ptr<GpuSurface>& s) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = scope.read(s); "
         "consume(lease.nativeHandle()); if (stop) longjmp(env, 1); scope.complete(); }",
         "complete() after read/use"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "longjmp(env, 1); }); }",
         "non-local jump cannot bypass withRead() completion"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "_longjmp(env, 1); }); }",
         "non-local jump cannot bypass withRead() completion"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "siglongjmp(env, 1); }); }",
         "non-local jump cannot bypass withRead() completion"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); long" +
         chr(92) + "\njmp(env, 1); }); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define JUMP longjmp\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "JUMP(env, 1); }); }",
         "non-local jump preprocessor alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define JUMP _longjmp\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "JUMP(env, 1); }); }",
         "non-local jump preprocessor alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define JUMP siglongjmp\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "JUMP(env, 1); }); }",
         "non-local jump preprocessor alias"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT_IMPL(left, right) left ## right\n"
         "#define CAT(left, right) CAT_IMPL(left, right)\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.CAT(native, Handle)(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "preprocessor token concatenation"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT_IMPL(left, right) left %:%: right\n"
         "#define CAT(left, right) CAT_IMPL(left, right)\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(handle); CAT(long, jmp)(env, 1); }); }",
         "preprocessor token concatenation"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) left ## right\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.CAT(native, Handle)(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) left ## right\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(handle); CAT(long, jmp)(env, 1); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define isCompatibleWithNativeHandle(value) persist(value)\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); }); }",
         "approved native-handle consumer cannot be hidden or shadowed"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { void* escaped = nullptr; "
         "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
         "auto isCompatibleWithNativeHandle = [&](void* value) { escaped = value; return true; }; "
         "void* handle = lease.nativeHandle(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) evil::isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); if (handle != HandleSink{}) consume(); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "template<typename Consumer> void bad(const std::shared_ptr<GpuSurface>& s, "
         "Consumer isCompatibleWithNativeHandle) { GpuSyncReadScope scope; "
         "scope.withRead(s, [&](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { bool "
         "(*isCompatibleWithNativeHandle)(void*) = persist; GpuSyncReadScope scope; "
         "scope.withRead(s, [&](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "using evil::isCompatibleWithNativeHandle; void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/gpu/applegpusurface_apple.mm"),
         "struct HandleSink { explicit HandleSink(void*); "
         "friend bool operator!=(const HandleSink&, decltype(nullptr)); }; "
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { using IOSurfaceRef = HandleSink; "
         "IOSurfaceRef handle = static_cast<IOSurfaceRef>(lease.nativeHandle()); "
         "if (handle != nullptr) consume(); }); }",
         "native handle value must initialize a callback-local alias"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "#define GetDevice(out) GetDevice(out); persist(texture)\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
         "static_cast<ID3D11Texture2D*>(lease.nativeHandle()); if (texture) "
         "texture->GetDevice(&device); }); }",
         "approved native-handle method cannot be hidden or shadowed"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "/* comment *" + chr(92) + "\n/ #define GetDevice(out) persist(texture)\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
         "static_cast<ID3D11Texture2D*>(lease.nativeHandle()); if (texture) "
         "texture->GetDevice(&device); }); }",
         "approved native-handle method cannot be hidden or shadowed"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "/* comment *" + chr(92) + "\r\n/ #define ID3D11Texture2D HandleSink\r\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
         "static_cast<ID3D11Texture2D*>(lease.nativeHandle()); if (texture) "
         "texture->GetDevice(&device); }); }",
         "approved native-handle type cannot be hidden or shadowed"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.CAT(native, CAT_I(Han, dle))(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) left ## right\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.CAT(nat" + chr(92) + "\nive, Handle)(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) left ## right\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
         "CAT(lo" + chr(92) + "\nng, jmp)(env, 1); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "#define ID3D11Texture2D HandleSink\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
         "static_cast<ID3D11Texture2D*>(lease.nativeHandle()); if (texture) "
         "texture->GetDevice(&device); }); }",
         "approved native-handle type cannot be hidden or shadowed"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "#define CAT(left, right) left ## right\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { auto* texture = "
         "static_cast<CAT(ID3D11, Texture2D)*>(lease.nativeHandle()); if (texture) "
         "texture->GetDevice(&device); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/applegpusurface_apple.mm"),
         "#define IOSurfaceRef HandleSink\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { IOSurfaceRef ioSurface = "
         "static_cast<IOSurfaceRef>(lease.nativeHandle()); "
         "IOSurfaceGetWidth(ioSurface); }); }",
         "approved native-handle type cannot be hidden or shadowed"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "template<typename Consumer> void bad(const std::shared_ptr<GpuSurface>& s, "
         "Consumer (isCompatibleWithNativeHandle)) { GpuSyncReadScope scope; "
         "scope.withRead(s, [&](const GpuReadLease& lease) { void* handle = "
         "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); }); }",
         "native handle value must remain inside its synchronous consumption"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { (void) lease.nat" + chr(92) +
         "\nive" + chr(92) + "\nHandle(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const GpuReadLease& lease) { auto access = "
         "&GpuReadLease::nat" + chr(92) + "\nive" + chr(92) +
         "\nHandle; (void) (lease.*access)(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = scope.re" + chr(92) +
         "\nad(s); (void) lease.nativeHandle(); scope.complete(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "const GpuReadLease lease = (scope).re" + chr(92) +
         "\nad(s); (void) lease.nativeHandle(); scope.complete(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s, OtherScope other) { "
         "GpuSyncReadScope scope; const GpuReadLease lease = (other, scope).re" +
         chr(92) + "\nad(s); (void) lease.nativeHandle(); scope.complete(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope* scope, const std::shared_ptr<GpuSurface>& s) { "
         "const GpuReadLease lease = (*scope).re" + chr(92) +
         "\nad(s); (void) lease.nativeHandle(); scope->complete(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "const GpuReadLease lease = static_cast<GpuSyncReadScope&>(scope).re" +
         chr(92) + "\nad(s); (void) lease.nativeHandle(); scope.complete(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "template<typename Fn> void bad(GpuSyncReadScope& scope, Fn fn = "
         "[](int value) { return value; }) { const GpuReadLease lease = scope.re" +
         chr(92) + "\nad(surface); (void) lease.nativeHandle(); scope.complete(); }",
         "phase-two line splice"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "observe(scope); const GpuReadLease lease = scope.read(s); scope.complete(); }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "observe(scope); scope.re" + chr(92) + "\nad(s); }",
         "phase-two line splice"),
        (PurePosixPath("playback/bad.cpp"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "auto observe = [scope] {}; const GpuReadLease lease = scope.read(s); "
         "scope.complete(); (void) observe; }",
         "GpuSyncReadScope::read()"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "auto observe = [scope] {}; scope.re" + chr(92) +
         "\nad(s); (void) observe; }",
         "phase-two line splice"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "scope.withRead(s, [](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "std::move(scope).withRead(s, [](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "std::forward<GpuSyncReadScope&>(scope).withRead(s, "
         "[](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "std::ref(scope).get().withRead(s, "
         "[](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "transform(scope).withRead(s, [](const GpuReadLease&) {}); }",
         "unresolved capability receiver"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "transform(scope).withRead(s, callback); }",
         "withRead() requires an inline callback body"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "transform(scope).withRead(s, "
         "[](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "trans" + chr(92) + "\nform(scope).withRead(s, "
         "[](const GpuReadLease&) { long" + chr(92) + "\njmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "std::mo" + chr(92) + "\nve(scope).withRead(s, "
         "[](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "(scope).withRead(s, [](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope* scope, const std::shared_ptr<GpuSurface>& s) { "
         "(*scope).withRead(s, [](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
         "static_cast<GpuSyncReadScope&>(scope).withRead(s, "
         "[](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void bad(GpuSyncReadScope& scope, OtherScope& other, "
         "const std::shared_ptr<GpuSurface>& s) { (other, scope).withRead(s, "
         "[](const GpuReadLease&) { longjmp(env, 1); }); }",
         "non-local jump"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT native\n"
         "#define RIGHT Handle\nvoid bad(const GpuReadLease& lease) { "
         "lease.CAT(LEFT, RIGHT)(); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define PASTE(left, right) PASTE_I(left, right)\n"
         "#define PASTE_I(left, right) left ## right\n"
         "#define LEFT native\n#define RIGHT Handle\n"
         "void bad(const GpuReadLease& lease) { lease.PASTE(LEFT, RIGHT)(); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT GpuSync\n"
         "#define RIGHT ReadScope\nvoid bad() { CAT(LEFT, RIGHT) scope; }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT lo\n"
         "#define RIGHT ngjmp\nvoid bad() { CAT(LEFT, RIGHT)(env, 1); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT re\n#define RIGHT ad\n"
         "void bad(GpuSyncReadScope& scope) { scope.CAT(LEFT, RIGHT)({}); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT with\n"
         "#define RIGHT Read\nvoid bad(GpuSyncReadScope& scope) { "
         "scope.CAT(LEFT, RIGHT)({}, [](const GpuReadLease&) {}); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT com\n"
         "#define RIGHT plete\nvoid bad(GpuSyncReadScope& scope) { "
         "scope.CAT(LEFT, RIGHT)(); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT isCompatibleWith\n"
         "#define RIGHT NativeHandle\nvoid bad(void* handle) { "
         "(void) CAT(LEFT, RIGHT)(handle); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT ID3D11\n"
         "#define RIGHT Texture2D\nvoid bad(void* handle) { "
         "auto* texture = static_cast<CAT(LEFT, RIGHT)*>(handle); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/output/win/wingpuimportedge.cpp"),
         "#define CAT(left, right) CAT_I(left, right)\n"
         "#define CAT_I(left, right) left ## right\n#define LEFT Get\n"
         "#define RIGHT Device\nvoid bad(ID3D11Texture2D* texture) { "
         "texture->CAT(LEFT, RIGHT)(&device); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "void jumps(const GpuReadLease&) { longjmp(env, 1); } "
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, jumps); }",
         "withRead() requires an inline callback body"),
    )
    for path, source, expected in cases:
        findings = audit_capability_uses(path, source)
        if "#define" in source or "#undef" in source:
            findings.extend(audit_source_only(path, source))
        rendered = "\n".join(finding.render() for finding in findings)
        if expected not in rendered:
            raise AssertionError(
                f"source-audit mutation survived ({expected}):\nsource: {source}\n{rendered}")

    ordinary_join = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void safe() { join(native, Handle); }")
    if ordinary_join:
        raise AssertionError("ordinary identifier arguments were treated as macro paste:\n" +
                             "\n".join(finding.render() for finding in ordinary_join))

    safe_join_macro = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "#define JOIN(a, b) consume(a, b)\n"
        "void safe() { JOIN(native, Handle); }")
    if safe_join_macro:
        raise AssertionError("non-paste function macro was treated as paste:\n" +
                             "\n".join(finding.render() for finding in safe_join_macro))

    mapped_splice = "// line 1\nlease.nat" + chr(92) + "\nive" + chr(92) + "\nHandle();\n"
    mapped_findings = phase_two_capability_findings(
        PurePosixPath("playback/bad.cpp"), mapped_splice)
    if len(mapped_findings) != 1 or mapped_findings[0].line != 2:
        raise AssertionError(
            "phase-two normalization did not preserve the guarded token's source line")

    registry_public = "class GpuRetireRegistry final { public: void registerRetire(); };"
    registry_findings = audit_public_member(REGISTRY_HEADER, registry_public,
                                            "GpuRetireRegistry", r"\bregisterRetire\s*\(",
                                            "GpuRetireRegistry::registerRetire()")
    if not registry_findings:
        raise AssertionError("public registerRetire mutation survived")

    op_scope_public = "class GpuOpScope final { public: void track(); };"
    op_findings = audit_public_member(OP_SCOPE_HEADER, op_scope_public, "GpuOpScope",
                                      r"\btrack\s*\(", "GpuOpScope::track()")
    if not op_findings:
        raise AssertionError("public track mutation survived")

    safe = (
        "bool safe(const std::shared_ptr<GpuSurface>& s) { bool compatible = false; "
        "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
        "GpuSurfaceDesc desc = lease.desc(); uint32_t subresource = lease.nativeSubresource(); "
        "bool valid = lease.valid(); (void) desc; (void) subresource; (void) valid; "
        "void* handle = lease.nativeHandle(); "
        "compatible = isCompatibleWithNativeHandle(handle); }); return compatible; }"
    )
    safe_findings = audit_capability_uses(PurePosixPath("playback/gpu/gpufence.h"), safe)
    if safe_findings:
        raise AssertionError("withRead pass control was rejected:\n" +
                             "\n".join(finding.render() for finding in safe_findings))

    safe_exception = (
        "void safe(const std::shared_ptr<GpuSurface>& s) { try { GpuSyncReadScope scope; "
        "scope.withRead(s, [](const GpuReadLease& lease) { void* handle = "
        "lease.nativeHandle(); (void) isCompatibleWithNativeHandle(handle); "
        "throw ExpectedFailure{}; }); } catch (const ExpectedFailure&) {} }"
    )
    safe_exception_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_exception)
    if safe_exception_findings:
        raise AssertionError("ordinary exception pass control was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_exception_findings))

    preserving_receivers = (
        "void safe(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
        "std::move(scope).withRead(s, [](const GpuReadLease&) {}); } "
        "void forwarded(GpuSyncReadScope& scope, "
        "const std::shared_ptr<GpuSurface>& s) { "
        "std::forward<GpuSyncReadScope&>(scope).withRead(s, "
        "[](const GpuReadLease&) {}); } "
        "void referred(GpuSyncReadScope& scope, const std::shared_ptr<GpuSurface>& s) { "
        "std::ref(scope).get().withRead(s, [](const GpuReadLease&) {}); } "
        "void nestedForward(GpuSyncReadScope& scope, "
        "const std::shared_ptr<GpuSurface>& s) { "
        "std::forward<std::type_identity_t<GpuSyncReadScope>>(scope).withRead("
        "s, [](const GpuReadLease&) {}); }")
    preserving_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), preserving_receivers)
    if preserving_findings:
        raise AssertionError("value-preserving receiver controls were rejected:\n" +
                             "\n".join(finding.render() for finding in preserving_findings))

    qualified_receiver_controls = (
        "void safe(GpuSyncReadScope& scope, Other& object, "
        "const std::shared_ptr<GpuSurface>& s) { "
        "object.scope().withRead(s, [](const GpuReadLease&) {}); "
        "ns::scope().withRead(s, [](const GpuReadLease&) {}); "
        "object.template scope<int>().withRead(s, [](const GpuReadLease&) {}); "
        "pointer->template scope<int>().withRead(s, [](const GpuReadLease&) {}); "
        "(void) scope; }")
    qualified_receiver_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), qualified_receiver_controls)
    if qualified_receiver_findings:
        raise AssertionError("qualified same-name receivers were rejected:\n" +
                             "\n".join(
                                 finding.render() for finding in qualified_receiver_findings))

    safe_boolean = (
        "bool safe(const std::shared_ptr<GpuSurface>& s) { bool present = false; "
        "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
        "void* handle = lease.nativeHandle(); present = handle != nullptr; }); "
        "return present; }"
    )
    safe_boolean_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_boolean)
    if safe_boolean_findings:
        raise AssertionError("boolean result pass control was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_boolean_findings))

    safe_nested_blocks = (
        "bool safe(bool enabled, const std::shared_ptr<GpuSurface>& s) { bool present = false; "
        "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
        "if (enabled) { void* handle = lease.nativeHandle(); "
        "while (handle != nullptr) { present = isCompatibleWithNativeHandle(handle); break; } "
        "} else { bool valid = lease.valid(); (void) valid; } }); return present; }"
    )
    safe_nested_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_nested_blocks)
    if safe_nested_findings:
        raise AssertionError("nested synchronous block pass control was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_nested_findings))

    safe_parenthesized = (
        "bool safe(const std::shared_ptr<GpuSurface>& s) { bool present = false; "
        "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
        "void* handle = lease.nativeHandle(); "
        "present = isCompatibleWithNativeHandle((handle)); if ((handle)) consume(); }); "
        "return present; }"
    )
    safe_parenthesized_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_parenthesized)
    if safe_parenthesized_findings:
        raise AssertionError("parenthesized immediate pass control was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_parenthesized_findings))

    safe_direct_consumer = (
        "bool safe(const std::shared_ptr<GpuSurface>& s) { bool present = false; "
        "GpuSyncReadScope scope; scope.withRead(s, [&](const GpuReadLease& lease) { "
        "present = isCompatibleWithNativeHandle(lease.nativeHandle()); }); return present; }"
    )
    safe_direct_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_direct_consumer)
    if safe_direct_findings:
        raise AssertionError("direct immediate lease consumer was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_direct_findings))

    safe_read = (
        "void safe(const std::shared_ptr<GpuSurface>& a, "
        "const std::shared_ptr<GpuSurface>& b) { GpuSyncReadScope scope; "
        "const GpuReadLease lease = scope.read(a); void* firstHandle = lease.nativeHandle(); "
        "bool firstValid = firstHandle != nullptr; (void) firstValid; "
        "scope.complete(); { GpuSyncReadScope scope; "
        "const GpuReadLease lease = scope.read(b); void* secondHandle = lease.nativeHandle(); "
        "bool secondValid = secondHandle != nullptr; (void) secondValid; "
        "scope.complete(); } }"
    )
    safe_read_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_read)
    if safe_read_findings:
        raise AssertionError("ordered, shadowed read pass control was rejected:\n" +
                             "\n".join(finding.render() for finding in safe_read_findings))

    safe_control_flow = (
        "void safe(bool enabled, const std::shared_ptr<GpuSurface>& s) { "
        "if (enabled) { GpuSyncReadScope scope; "
        "const GpuReadLease lease = scope.read(s); void* firstHandle = lease.nativeHandle(); "
        "bool firstValid = firstHandle != nullptr; (void) firstValid; "
        "scope.complete(); } GpuSyncReadScope scope; "
        "const GpuReadLease lease = scope.read(s); void* secondHandle = lease.nativeHandle(); "
        "bool secondValid = secondHandle != nullptr; (void) secondValid; "
        "scope.complete(); } "
        "void safeConditional(bool enabled, const std::shared_ptr<GpuSurface>& s) { "
        "GpuSyncReadScope scope; if (enabled) scope.withRead(s, "
        "[](const GpuReadLease& lease) { void* handle = lease.nativeHandle(); "
        "(void) isCompatibleWithNativeHandle(handle); }); }"
    )
    safe_control_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_control_flow)
    if safe_control_findings:
        raise AssertionError("conservative read/withRead pass control was rejected:\n" +
                              "\n".join(finding.render() for finding in safe_control_findings))

    safe_owned_apple = (
        "CVPixelBufferRef safe(const std::shared_ptr<GpuSurface>& s) { "
        "CVPixelBufferRef result = nullptr; GpuSyncReadScope scope; "
        "scope.withRead(s, [&](const GpuReadLease& lease) { IOSurfaceRef ioSurface = "
        "static_cast<IOSurfaceRef>(lease.nativeHandle()); if (!ioSurface) return; "
        "CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &result); "
        "}); return result; }"
    )
    safe_owned_apple_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/applegpusurface_apple.mm"), safe_owned_apple)
    if safe_owned_apple_findings:
        raise AssertionError("owned Apple result pass control was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_owned_apple_findings))

    safe_owned_fence = (
        "std::shared_ptr<GpuFence> safe(const std::shared_ptr<GpuSurface>& s) { "
        "std::shared_ptr<GpuFence> result; GpuSyncReadScope scope; "
        "scope.withRead(s, [&](const GpuReadLease& lease) { "
        "auto* texture = static_cast<ID3D11Texture2D*>(lease.nativeHandle()); "
        "ComPtr<ID3D11Device> device; if (texture) texture->GetDevice(&device); "
        "result = makeD3D11GpuFence(device.Get(), 1); }); return result; }"
    )
    safe_owned_fence_findings = audit_capability_uses(
        PurePosixPath("playback/output/win/wingpuimportedge.cpp"), safe_owned_fence)
    if safe_owned_fence_findings:
        raise AssertionError("owned fence result pass control was rejected:\n" +
                             "\n".join(finding.render()
                                       for finding in safe_owned_fence_findings))

    safe_syntax = r'''
        struct OtherScope {};
        #define SAFE_SUM(left, right) ((left) + \
                                       (right))
        #define READ_SOCKET(socket) ((socket).read( \
                                     ))
        #define MARK_JOB_DONE(job) ((job).complete( \
                                    ))
        void safe(UnrelatedSocket& socket) {
            // GpuSyncReadScope scope; scope.read(surface);
            const char* text = "surface.get()->nativeHandle()";
            socket.read();
            socket.re\
ad();
            (void) text;
        }
        void safeJob(UnrelatedJob& job) {
            job.com\
plete();
        }
        void safeSplitReceiver(GpuSyncReadScope& scope, UnrelatedSocket& socket,
                               UnrelatedJob& job) {
            (scope, socket).re\
ad();
            (scope, job).com\
plete();
        }
        void safeShadowedSplit(const std::shared_ptr<GpuSurface>& surface) {
            GpuSyncReadScope scope;
            {
                OtherScope scope;
                scope.re\
ad(surface);
            }
            (void) scope;
        }
        void safeParenthesizedShadow(const std::shared_ptr<GpuSurface>& surface) {
            GpuSyncReadScope scope;
            { OtherScope (scope); scope.re\
ad(surface); }
            (void) scope;
        }
        void safeStructuredShadow(const std::shared_ptr<GpuSurface>& surface) {
            GpuSyncReadScope scope;
            { auto [scope] = makeOtherScope(); scope.re\
ad(surface); }
            (void) scope;
        }
        void safeDirectiveText() {
            const char* raw = R"tag(
#define CAT(left, right) left ## right
#define HANDLE nativeHandle
)tag";
            /*
#define JUMP longjmp
#define GetDevice(out) persist(out)
            */
            (void) raw;
        }
        void safeSplicedLineComment() {
            // comment continues across splice \
#define nativeHandle hidden
        }
        void safeRawFakeTerminator() {
            const char* raw = R"tag(prefix )tag\
" still raw
#define nativeHandle hidden
)tag";
            (void) raw;
        }
        void safeReceiver(GpuSyncReadScope* scope, UnrelatedSocket& socket) {
            socket.read();
            if (scope) socket.read();
            scope->withRead({}, [](const GpuReadLease&) {});
        }
        void unrelatedShadow(GpuSyncReadScope& scope) {
            // OtherScope scope; scope.read(); scope.complete();
            const char* text = "OtherScope scope; scope.withRead();";
            { OtherScope scope; scope.read(); scope.withRead(); scope.complete(); }
            (void) scope;
            (void) text;
        }
        class DerivedSurface : public GpuSurface {
            void* nativeHandle() const override;
        };
    '''
    safe_syntax_findings = audit_capability_uses(
        PurePosixPath("playback/safe_syntax.cpp"), safe_syntax)
    if safe_syntax_findings:
        raise AssertionError("comments, strings, or unrelated calls were rejected:\n" +
                             "\n".join(finding.render() for finding in safe_syntax_findings))

    safe_crlf_comment = (
        "void safe() { // comment continues across splice " + chr(92) + "\r\n"
        "#define nativeHandle hidden\r\n}\r\n")
    safe_crlf_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_crlf_comment)
    if safe_crlf_findings:
        raise AssertionError("CRLF-spliced line comment ended early:\n" +
                             "\n".join(finding.render() for finding in safe_crlf_findings))

    safe_earlier_shadow = (
        "template<typename Consumer> void earlier(Consumer isCompatibleWithNativeHandle) { "
        "(void) isCompatibleWithNativeHandle; } "
        "bool later(const std::shared_ptr<GpuSurface>& surface) { bool compatible = false; "
        "GpuSyncReadScope scope; scope.withRead(surface, [&](const GpuReadLease& lease) { "
        "void* handle = lease.nativeHandle(); "
        "compatible = isCompatibleWithNativeHandle(handle); }); return compatible; }"
    )
    earlier_shadow_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"), safe_earlier_shadow)
    if earlier_shadow_findings:
        raise AssertionError("out-of-scope consumer shadow was retained:\n" +
                             "\n".join(finding.render()
                                       for finding in earlier_shadow_findings))

    ignored_sources = {
        PurePosixPath("tests/gpu/generated_fixture.cpp"):
            "void ignored(GpuSurface& surface) { surface.nativeHandle(); }",
        PurePosixPath("build/generated/gpu_fixture.cpp"):
            "void ignored(GpuSyncReadScope& scope) { scope.read({}); }",
    }
    ignored_findings = audit_sources(ignored_sources)
    if ignored_findings:
        raise AssertionError("non-production source was rejected:\n" +
                             "\n".join(finding.render() for finding in ignored_findings))


def nested_phase_two_source(count: int) -> str:
    openings = "".join(
        f"{{ GpuSyncReadScope s{index}; s{index}.re" + chr(92) + "\nad(surface);"
        for index in range(count))
    return "void nested() {" + openings + ("}" * count) + "}"


def nested_macro_postfix_source(count: int) -> str:
    return "#define M() safe\nvoid nested() { " + ("M()(" * count) + "value" + (")" * count) + "; }"


def nested_ambiguity_source(count: int) -> str:
    return (
        "GpuSyncReadScope scope;\n"
        + "scope.withRead(surface, [&] {\n" * count
        + "safe();\n"
        + "});\n" * count
    )


def nested_ordinary_callback_source(count: int) -> str:
    return (
        "void nested() {\n"
        + "".join(
            f"Logger scope{index}; scope{index}.withRead(surface, [&] {{\n"
            for index in range(count)
        )
        + "safe();\n"
        + "});\n" * count
        + "}\n"
    )


def isolated_use_before_declaration_source(count: int) -> str:
    return "".join(
        f"void isolated{index}() {{ scope.withRead(); Logger scope; }}\n"
        for index in range(count)
    )


def read_result_binding_source(count: int) -> str:
    return "".join(
        f"void read_result{index}() {{ GpuSyncReadScope scope; "
        "auto lease = scope.read(surface); lease.unknown(); }}\n"
        for index in range(count)
    )


def adjacent_sensitive_source(count: int) -> str:
    return (
        "void audit() { GpuSyncReadScope scope;\n"
        + "scope.withRead(surface, callback);\n" * count
        + "}\n"
    )


def performance_self_tests() -> str:
    """Run the noisy scaling check independently from functional mutations."""
    path = PurePosixPath("playback/gpu/gpufence.h")
    counts = (4096, 8192, 16384)
    sources = {count: nested_phase_two_source(count) for count in counts}
    # Warm parser regex caches and allocator paths before collecting evidence.
    for _ in range(2):
        warmup = phase_two_capability_findings(path, nested_phase_two_source(1024))
        if len(warmup) != 1024:
            raise AssertionError("phase-two performance warmup lost findings")

    samples: dict[int, list[float]] = {count: [] for count in counts}
    orders = (
        counts, tuple(reversed(counts)), (8192, 16384, 4096),
        (16384, 4096, 8192), (4096, 16384, 8192),
    )
    for order in orders:
        for count in order:
            started = time.perf_counter()
            findings = phase_two_capability_findings(path, sources[count])
            samples[count].append(time.perf_counter() - started)
            if len(findings) != count:
                raise AssertionError(
                    f"nested phase-two binding control lost findings at {count}")

    # Score the middle of the fastest three samples. Two arbitrarily delayed
    # samples therefore cannot fail an otherwise linear implementation.
    scores = {
        count: statistics.median(sorted(samples[count])[:3]) for count in counts
    }
    adjacent = (scores[8192] / scores[4096], scores[16384] / scores[8192])
    aggregate = scores[16384] / scores[4096]
    if max(adjacent) > 3.25 or aggregate > 5.75:
        raise AssertionError(
            "nested phase-two declaration assignment is not near-linear: "
            + ", ".join(
                f"{count}={scores[count]:.4f}s samples="
                + "/".join(f"{sample:.4f}" for sample in samples[count])
                for count in counts)
            + f", ratios={adjacent[0]:.3f}/{adjacent[1]:.3f}/{aggregate:.3f}")

    postfix_sources = {
        count: translate_source(nested_macro_postfix_source(count))
        for count in counts
    }
    postfix_samples: dict[int, list[float]] = {count: [] for count in counts}
    for order in (counts, tuple(reversed(counts)), (8192, 16384, 4096)):
        for count in order:
            started = time.perf_counter()
            findings = guarded_macro_composition_findings(
                path, postfix_sources[count], source_only=True)
            postfix_samples[count].append(time.perf_counter() - started)
            if not any("macro postfix complexity" in finding.reason
                       for finding in findings):
                raise AssertionError(
                    f"nested macro postfix control did not fail closed at {count}")
    postfix_scores = {
        count: min(postfix_samples[count]) for count in counts
    }
    postfix_adjacent = (
        postfix_scores[8192] / postfix_scores[4096],
        postfix_scores[16384] / postfix_scores[8192],
    )
    postfix_aggregate = postfix_scores[16384] / postfix_scores[4096]
    if max(postfix_adjacent) > 3.25 or postfix_aggregate > 5.75:
        raise AssertionError(
            "nested macro postfix audit is not near-linear: "
            + ", ".join(
                f"{count}={postfix_scores[count]:.4f}s samples="
                + "/".join(f"{sample:.4f}" for sample in postfix_samples[count])
                for count in counts)
            + f", ratios={postfix_adjacent[0]:.3f}/"
            f"{postfix_adjacent[1]:.3f}/{postfix_aggregate:.3f}")

    wide_sources = {
        count: translate_source(
            "#define WIDE(value) " + " ## ".join("value" for _ in range(count))
            + "\nvoid bounded(const GpuReadLease& lease) { lease.WIDE(n)(); }")
        for count in counts
    }
    wide_samples: dict[int, list[float]] = {count: [] for count in counts}
    for order in (counts, tuple(reversed(counts)), (8192, 16384, 4096)):
        for count in order:
            started = time.perf_counter()
            findings = guarded_macro_composition_findings(
                path, wide_sources[count], source_only=True)
            wide_samples[count].append(time.perf_counter() - started)
            if not any("complexity" in finding.reason
                       for finding in findings):
                raise AssertionError(
                    f"wide macro replacement did not fail closed at {count}")
    wide_scores = {count: min(wide_samples[count]) for count in counts}
    wide_adjacent = (
        wide_scores[8192] / wide_scores[4096],
        wide_scores[16384] / wide_scores[8192],
    )
    wide_aggregate = wide_scores[16384] / wide_scores[4096]
    if max(wide_adjacent) > 3.25 or wide_aggregate > 5.75:
        raise AssertionError(
            "wide macro replacement audit is not near-linear: "
            + ", ".join(
                f"{count}={wide_scores[count]:.4f}s samples="
                + "/".join(f"{sample:.4f}" for sample in wide_samples[count])
                for count in counts)
            + f", ratios={wide_adjacent[0]:.3f}/"
            f"{wide_adjacent[1]:.3f}/{wide_aggregate:.3f}")
    ambiguity_counts = (100, 200, 400, 800)
    ambiguity_sources = {
        count: translate_source(nested_ambiguity_source(count))
        for count in ambiguity_counts
    }
    ambiguity_samples: dict[int, list[float]] = {
        count: [] for count in ambiguity_counts
    }
    for order in (
        ambiguity_counts,
        tuple(reversed(ambiguity_counts)),
        (200, 800, 100, 400),
    ):
        for count in order:
            started = time.perf_counter()
            _source_only_unknown_macro_findings(path, ambiguity_sources[count])
            ambiguity_samples[count].append(time.perf_counter() - started)
    ambiguity_scores = {
        count: min(ambiguity_samples[count]) for count in ambiguity_counts
    }
    ambiguity_adjacent = tuple(
        ambiguity_scores[right] / ambiguity_scores[left]
        for left, right in zip(ambiguity_counts, ambiguity_counts[1:])
    )
    ambiguity_aggregate = (
        ambiguity_scores[800] / ambiguity_scores[100]
    )
    if max(ambiguity_adjacent) > 3.25 or ambiguity_aggregate > 12.0:
        raise AssertionError(
            "nested withRead ambiguity audit is not near-linear: "
            + ", ".join(
                f"{count}={ambiguity_scores[count]:.4f}s samples="
                + "/".join(
                    f"{sample:.4f}" for sample in ambiguity_samples[count]
                )
                for count in ambiguity_counts
            )
            + ", ratios="
            + "/".join(f"{ratio:.3f}" for ratio in ambiguity_adjacent)
            + f"/{ambiguity_aggregate:.3f}"
        )
    nested_sources = {
        count: nested_ambiguity_source(count) for count in ambiguity_counts
    }
    nested_samples: dict[int, list[float]] = {
        count: [] for count in ambiguity_counts
    }
    for order in (ambiguity_counts, tuple(reversed(ambiguity_counts))):
        for count in order:
            started = time.perf_counter()
            nested_findings = audit_source_only(path, nested_sources[count])
            nested_samples[count].append(time.perf_counter() - started)
            if not nested_findings:
                raise AssertionError(
                    f"end-to-end nested withRead control lost findings at {count}"
                )
    nested_scores = {
        count: min(nested_samples[count]) for count in ambiguity_counts
    }
    nested_adjacent = tuple(
        nested_scores[right] / nested_scores[left]
        for left, right in zip(ambiguity_counts, ambiguity_counts[1:])
    )
    nested_aggregate = nested_scores[800] / nested_scores[100]
    if max(nested_adjacent) > 3.25 or nested_aggregate > 12.0:
        raise AssertionError(
            "end-to-end nested withRead audit is not near-linear: "
            + ", ".join(
                f"{count}={nested_scores[count]:.4f}s samples="
                + "/".join(f"{sample:.4f}" for sample in nested_samples[count])
                for count in ambiguity_counts
            )
            + ", ratios=" + "/".join(
                f"{ratio:.3f}" for ratio in nested_adjacent
            )
            + f"/{nested_aggregate:.3f}"
        )
    ordinary_sources = {
        count: nested_ordinary_callback_source(count)
        for count in ambiguity_counts
    }
    ordinary_samples: dict[int, list[float]] = {
        count: [] for count in ambiguity_counts
    }
    for order in (ambiguity_counts, tuple(reversed(ambiguity_counts))):
        for count in order:
            started = time.perf_counter()
            ordinary_findings = audit_source_only(path, ordinary_sources[count])
            ordinary_samples[count].append(time.perf_counter() - started)
            if ordinary_findings:
                raise AssertionError(
                    "ordinary nested callback control produced capability findings "
                    f"at {count}: {ordinary_findings[:3]}"
                )
    ordinary_scores = {
        count: min(ordinary_samples[count]) for count in ambiguity_counts
    }
    ordinary_adjacent = tuple(
        ordinary_scores[right] / ordinary_scores[left]
        for left, right in zip(ambiguity_counts, ambiguity_counts[1:])
    )
    ordinary_aggregate = ordinary_scores[800] / ordinary_scores[100]
    if max(ordinary_adjacent) > 3.25 or ordinary_aggregate > 12.0:
        raise AssertionError(
            "ordinary nested callback audit is not near-linear: "
            + ", ".join(
                f"{count}={ordinary_scores[count]:.4f}s samples="
                + "/".join(f"{sample:.4f}" for sample in ordinary_samples[count])
                for count in ambiguity_counts
            )
            + ", ratios=" + "/".join(
                f"{ratio:.3f}" for ratio in ordinary_adjacent
            )
            + f"/{ordinary_aggregate:.3f}"
        )
    isolated_counts = (500, 1000, 2000, 4000)
    isolated_translations = {
        count: translate_source(isolated_use_before_declaration_source(count))
        for count in isolated_counts
    }
    isolated_binding_samples: dict[int, list[float]] = {
        count: [] for count in isolated_counts
    }
    for order in (isolated_counts, tuple(reversed(isolated_counts))):
        for count in order:
            translated = isolated_translations[count]
            _events, ranges = source_macro_events(translated.masked)
            started = time.perf_counter()
            isolated_bindings = _source_only_declared_bindings(
                translated.masked, ranges
            )
            _source_binding_index(isolated_bindings, translated.masked)
            isolated_binding_samples[count].append(time.perf_counter() - started)
            if len(isolated_bindings) != count:
                raise AssertionError(
                    f"isolated binding index lost declarations at {count}"
                )
    isolated_binding_scores = {
        count: min(isolated_binding_samples[count]) for count in isolated_counts
    }
    isolated_binding_adjacent = tuple(
        isolated_binding_scores[right] / isolated_binding_scores[left]
        for left, right in zip(isolated_counts, isolated_counts[1:])
    )
    isolated_binding_aggregate = (
        isolated_binding_scores[4000] / isolated_binding_scores[500]
    )
    if (max(isolated_binding_adjacent) > 3.25
            or isolated_binding_aggregate > 12.0):
        raise AssertionError(
            "isolated positioned-binding index is not near-linear: "
            + ", ".join(
                f"{count}={isolated_binding_scores[count]:.4f}s samples="
                + "/".join(
                    f"{sample:.4f}" for sample in isolated_binding_samples[count]
                )
                for count in isolated_counts
            )
            + ", ratios=" + "/".join(
                f"{ratio:.3f}" for ratio in isolated_binding_adjacent
            )
            + f"/{isolated_binding_aggregate:.3f}"
        )
    read_result_counts = (500, 1000, 2000, 4000)
    read_result_translations = {
        count: translate_source(read_result_binding_source(count))
        for count in read_result_counts
    }
    read_result_samples: dict[int, list[float]] = {
        count: [] for count in read_result_counts
    }
    for order in (read_result_counts, tuple(reversed(read_result_counts))):
        for count in order:
            translated = read_result_translations[count]
            _events, ranges = source_macro_events(translated.masked)
            started = time.perf_counter()
            read_bindings = _source_only_declared_bindings(
                translated.masked, ranges
            )
            read_result_samples[count].append(time.perf_counter() - started)
            if sum(
                binding.category == "lease" for binding in read_bindings
            ) != count:
                raise AssertionError(
                    f"read-result binding inference lost leases at {count}"
                )
    read_result_scores = {
        count: min(read_result_samples[count]) for count in read_result_counts
    }
    read_result_adjacent = tuple(
        read_result_scores[right] / read_result_scores[left]
        for left, right in zip(read_result_counts, read_result_counts[1:])
    )
    read_result_aggregate = (
        read_result_scores[4000] / read_result_scores[500]
    )
    if (max(read_result_adjacent) > 3.25
            or read_result_aggregate > 12.0):
        raise AssertionError(
            "read-result binding inference is not near-linear: "
            + ", ".join(
                f"{count}={read_result_scores[count]:.4f}s samples="
                + "/".join(
                    f"{sample:.4f}" for sample in read_result_samples[count]
                )
                for count in read_result_counts
            )
            + ", ratios=" + "/".join(
                f"{ratio:.3f}" for ratio in read_result_adjacent
            )
            + f"/{read_result_aggregate:.3f}"
        )
    isolated_audit_counts = (2000, 4000, 8000)
    isolated_audit_sources = {
        count: isolated_use_before_declaration_source(count)
        for count in isolated_audit_counts
    }
    isolated_audit_scores: dict[int, float] = {}
    for count in isolated_audit_counts:
        started = time.perf_counter()
        isolated_findings = audit_source_only(
            path, isolated_audit_sources[count]
        )
        isolated_audit_scores[count] = time.perf_counter() - started
        if len(isolated_findings) < count:
            raise AssertionError(
                f"isolated full audit lost unresolved receivers at {count}"
            )
    isolated_audit_adjacent = (
        isolated_audit_scores[4000] / isolated_audit_scores[2000],
        isolated_audit_scores[8000] / isolated_audit_scores[4000],
    )
    isolated_audit_aggregate = (
        isolated_audit_scores[8000] / isolated_audit_scores[2000]
    )
    if (max(isolated_audit_adjacent) > 3.25
            or isolated_audit_aggregate > 5.75):
        raise AssertionError(
            "isolated full source audit is not near-linear: "
            + ", ".join(
                f"{count}={isolated_audit_scores[count]:.4f}s"
                for count in isolated_audit_counts
            )
            + f", ratios={isolated_audit_adjacent[0]:.3f}/"
            f"{isolated_audit_adjacent[1]:.3f}/"
            f"{isolated_audit_aggregate:.3f}"
        )
    adjacent_sources = {
        count: adjacent_sensitive_source(count) for count in counts
    }
    adjacent_scores: dict[int, float] = {}
    for count in counts:
        started = time.perf_counter()
        adjacent_findings = audit_source_only(path, adjacent_sources[count])
        adjacent_scores[count] = time.perf_counter() - started
        if len(adjacent_findings) < count:
            raise AssertionError(
                f"adjacent sensitive-call audit lost findings at {count}"
            )
    adjacent_call_ratios = (
        adjacent_scores[8192] / adjacent_scores[4096],
        adjacent_scores[16384] / adjacent_scores[8192],
    )
    adjacent_call_aggregate = adjacent_scores[16384] / adjacent_scores[4096]
    if max(adjacent_call_ratios) > 3.25 or adjacent_call_aggregate > 5.75:
        raise AssertionError(
            "adjacent sensitive-call audit is not near-linear: "
            + ", ".join(
                f"{count}={adjacent_scores[count]:.4f}s" for count in counts
            )
            + f", ratios={adjacent_call_ratios[0]:.3f}/"
            f"{adjacent_call_ratios[1]:.3f}/{adjacent_call_aggregate:.3f}"
        )
    return (
        ", ".join(f"{count}={scores[count]:.4f}s" for count in counts)
        + f", ratios={adjacent[0]:.3f}/{adjacent[1]:.3f}/{aggregate:.3f}; "
        + "postfix "
        + ", ".join(f"{count}={postfix_scores[count]:.4f}s" for count in counts)
        + f", ratios={postfix_adjacent[0]:.3f}/"
        f"{postfix_adjacent[1]:.3f}/{postfix_aggregate:.3f}; wide "
        + ", ".join(f"{count}={wide_scores[count]:.4f}s" for count in counts)
        + f", ratios={wide_adjacent[0]:.3f}/"
        f"{wide_adjacent[1]:.3f}/{wide_aggregate:.3f}; ambiguity "
        + ", ".join(
            f"{count}={ambiguity_scores[count]:.4f}s"
            for count in ambiguity_counts
        )
        + ", ratios="
        + "/".join(f"{ratio:.3f}" for ratio in ambiguity_adjacent)
        + f"/{ambiguity_aggregate:.3f}; nested "
        + ", ".join(
            f"{count}={nested_scores[count]:.4f}s"
            for count in ambiguity_counts
        )
        + ", ratios=" + "/".join(
            f"{ratio:.3f}" for ratio in nested_adjacent
        )
        + f"/{nested_aggregate:.3f}; ordinary "
        + ", ".join(
            f"{count}={ordinary_scores[count]:.4f}s"
            for count in ambiguity_counts
        )
        + ", ratios=" + "/".join(
            f"{ratio:.3f}" for ratio in ordinary_adjacent
        )
        + f"/{ordinary_aggregate:.3f}; isolated-bindings "
        + ", ".join(
            f"{count}={isolated_binding_scores[count]:.4f}s"
            for count in isolated_counts
        )
        + ", ratios=" + "/".join(
            f"{ratio:.3f}" for ratio in isolated_binding_adjacent
        )
        + f"/{isolated_binding_aggregate:.3f}; read-results "
        + ", ".join(
            f"{count}={read_result_scores[count]:.4f}s"
            for count in read_result_counts
        )
        + ", ratios=" + "/".join(
            f"{ratio:.3f}" for ratio in read_result_adjacent
        )
        + f"/{read_result_aggregate:.3f}; isolated-audit "
        + ", ".join(
            f"{count}={isolated_audit_scores[count]:.4f}s"
            for count in isolated_audit_counts
        )
        + f", ratios={isolated_audit_adjacent[0]:.3f}/"
        f"{isolated_audit_adjacent[1]:.3f}/"
        f"{isolated_audit_aggregate:.3f}; adjacent "
        + ", ".join(
            f"{count}={adjacent_scores[count]:.4f}s" for count in counts
        )
        + f", ratios={adjacent_call_ratios[0]:.3f}/"
        f"{adjacent_call_ratios[1]:.3f}/{adjacent_call_aggregate:.3f}")


def load_production_sources(root: Path) -> dict[PurePosixPath, str]:
    sources: dict[PurePosixPath, str] = {}
    for directory in PRODUCTION_ROOTS:
        for path in (root / directory).rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                relative = PurePosixPath(path.relative_to(root).as_posix())
                sources[relative] = path.read_text(encoding="utf-8")
    return sources


def parse_live_compiler_options(
    values: tuple[str, ...],
) -> dict[CompilerFamily, Path]:
    """Parse repeated ``FAMILY=PATH`` selections without probing compilers."""

    selected: dict[CompilerFamily, Path] = {}
    for value in values:
        if not isinstance(value, str) or "=" not in value:
            raise AuditInfrastructureError(
                "live compiler must use FAMILY=PATH"
            )
        family_value, separator, path_value = value.partition("=")
        if not separator or not family_value or not path_value:
            raise AuditInfrastructureError(
                "live compiler must use FAMILY=PATH"
            )
        try:
            family = CompilerFamily(family_value)
        except ValueError as error:
            raise AuditInfrastructureError(
                f"unsupported live compiler family: {family_value}"
            ) from error
        if family in selected:
            raise AuditInfrastructureError(
                f"duplicate live compiler family: {family.value}"
            )
        selected[family] = Path(path_value).expanduser().resolve(strict=False)
    return selected


def parse_required_live_families(
    values: tuple[str, ...],
) -> frozenset[CompilerFamily]:
    required: set[CompilerFamily] = set()
    for value in values:
        try:
            family = CompilerFamily(value)
        except ValueError as error:
            raise AuditInfrastructureError(
                f"unsupported live compiler family: {value}"
            ) from error
        if family in required:
            raise AuditInfrastructureError(
                f"duplicate required live compiler family: {family.value}"
            )
        required.add(family)
    return frozenset(required)


@dataclass(frozen=True)
class LiveExecutionBudget:
    """One monotonic deadline shared by every live fixture and family."""

    deadline: float
    clock: Callable[[], float]

    @classmethod
    def start(
        cls,
        total_seconds: float = AuditLimits().total_seconds,
        clock: Callable[[], float] = time.monotonic,
    ) -> "LiveExecutionBudget":
        if (
            not callable(clock)
            or not isinstance(total_seconds, (int, float))
            or isinstance(total_seconds, bool)
            or total_seconds <= 0
        ):
            raise AuditInfrastructureError("live execution budget is invalid")
        return cls(float(clock()) + float(total_seconds), clock)

    def remaining_seconds(self) -> float:
        remaining = self.deadline - float(self.clock())
        if remaining <= 0:
            raise AuditInfrastructureError(
                "live compiler total execution deadline exceeded"
            )
        return remaining


def run_live_only(
    compilers: Mapping[CompilerFamily, Path],
    required: frozenset[CompilerFamily],
    *,
    suite_runner: Callable[[Path, CompilerFamily, LiveExecutionBudget], None],
    printer: Callable[[str], None] = print,
    clock: Callable[[], float] = time.monotonic,
    total_seconds: float = AuditLimits().total_seconds,
) -> None:
    """Run selected families and report every optional absence explicitly."""

    if (not isinstance(compilers, Mapping)
            or not isinstance(required, frozenset)
            or not callable(suite_runner)
            or not callable(printer)):
        raise AuditInfrastructureError("live compiler inputs are invalid")
    missing = sorted(
        required - set(compilers), key=lambda family: family.value
    )
    if missing:
        raise AuditInfrastructureError(
            "required live compiler family is missing: "
            + ", ".join(family.value for family in missing)
        )
    budget = LiveExecutionBudget.start(total_seconds, clock)
    for family in CompilerFamily:
        compiler = compilers.get(family)
        if compiler is None:
            printer(
                "SKIP: optional live compiler family "
                f"{family.value}: no executable selected"
            )
            continue
        if not isinstance(compiler, Path):
            raise AuditInfrastructureError(
                f"live compiler path is invalid: {family.value}"
            )
        try:
            canonical = compiler.resolve(strict=True)
        except OSError as error:
            raise AuditInfrastructureError(
                f"live compiler executable is unavailable: {family.value}={compiler}"
            ) from error
        if not canonical.is_file():
            raise AuditInfrastructureError(
                f"live compiler executable is not a file: {family.value}={canonical}"
            )
        budget.remaining_seconds()
        try:
            suite_runner(canonical, family, budget)
        except OSError as error:
            raise AuditInfrastructureError(
                f"live compiler execution failed: {family.value}: {error}"
            ) from error
        budget.remaining_seconds()
        printer(f"PASS: live compiler capability parity: {family.value}={canonical}")


def schedule_correctness_configuration_audits(
    *,
    decision: object,
    pipeline_deadline: float,
    source_root: Path,
    configurations: tuple[object, ...],
    dependency_roots: object,
    capability_registry: object,
    initial_digest_map: Mapping[object, object],
    prepared_cache: object,
    limits: AuditLimits,
    expected_audit_engine_fingerprint: str,
    inspection_probe_invocations: int,
    run_accountant: object,
    compact_observer: object,
):
    """Correctness-run entry using the same frozen runtime contract."""

    runtime_contract = (
        _gpu_capability_calibration.runtime_contract_from_platform_decision(
            decision, pipeline_deadline
        )
    )
    return _gpu_capability_runner.schedule_configuration_audits(
        source_root,
        configurations,
        dependency_roots,
        capability_registry,
        initial_digest_map,
        prepared_cache,
        limits,
        expected_audit_engine_fingerprint,
        runtime_contract,
        inspection_probe_invocations=inspection_probe_invocations,
        run_accountant=run_accountant,
        compact_observer=compact_observer,
    )


def execute_prepared_correctness_audits(**prepared: object):
    """Enter the real Task-7 scheduler with already validated run inputs."""

    return schedule_correctness_configuration_audits(**prepared)


def run_correctness_only_cli(args: argparse.Namespace) -> None:
    """Validate the Task-7 CLI boundary before Task 10 supplies preparation.

    The native decision-key derivation, strict artifact load, cache/snapshot
    ownership, and final policy lifecycle are deliberately owned by Task 10.
    This function is the stable CLI seam that Task 10 completes; it must not
    invent a default worker count or fabricate a decision in the meantime.
    """

    if (
        not isinstance(args, argparse.Namespace)
        or not isinstance(args.source_root, Path)
        or not isinstance(args.compile_commands, Path)
        or not isinstance(args.worker_decision, Path)
        or not isinstance(args.dependency_root, list)
        or any(not isinstance(item, str) or "=" not in item
               for item in args.dependency_root)
    ):
        raise AuditInfrastructureError(
            "correctness-only orchestration input is invalid")
    raise AuditInfrastructureError(
        "correctness-only native decision preparation belongs to Task 10")


AUDIT_ENGINE_GRAPH_SCHEMA_BYTES = b"olr-gpu-capability-live-graph-v8"
AUDIT_ENGINE_STAGE_BYTES = b"task-9-compiled-provenance-scanner"
DECISION_ENGINE_GRAPH_SCHEMA_BYTES = b"olr-gpu-capability-decision-live-graph-v1"
_PREPROCESS_CONFIGURATION_CONSTRUCTOR_INVENTORY = MappingProxyType({
    "gpu_capability_command.py": 1,
    "gpu_capability_runner.py": 1,
    "test_gpu_capability_audit_lanes.py": 1,
    "test_gpu_capability_cache.py": 2,
    "test_gpu_capability_command.py": 1,
    "test_gpu_capability_model.py": 1,
    "test_gpu_capability_provenance.py": 1,
    "test_gpu_capability_runner.py": 8,
})
_AUDIT_ENGINE_TARGET_MODULES = (
    _gpu_capability_model,
    sys.modules[__name__],
    _gpu_capability_command,
    _gpu_capability_cache,
    _gpu_capability_provenance,
    _gpu_capability_runner,
)
_AUDIT_RUNTIME_STATE_EXCLUSIONS = MappingProxyType({
    "gpu_capability_source_audit": frozenset({"_compiler_analysis_context"}),
    "gpu_capability_model": frozenset(),
    "gpu_capability_command": frozenset({
        "_compiler_capability_lock",
        "_compiler_capability_memo",
        "_compiler_inspection_lock",
        "_compiler_inspection_memo",
        "_parent_compiler_launch_observer",
    }),
    "gpu_capability_cache": frozenset({"_hash_cache", "_hash_lock"}),
    "gpu_capability_provenance": frozenset(),
    "gpu_capability_runner": frozenset({
        "_WORKER_CACHE",
        "_WORKER_CAPABILITIES",
        "_WORKER_CANCEL_EVENT",
        "_WORKER_ENGINE",
        "_WORKER_GENERATION",
        "_WORKER_INDEX",
        "_WORKER_LIMITS",
        "_WORKER_MAXIMUM_TASKS",
        "_WORKER_PREATTESTED_ENGINE",
        "_WORKER_PRODUCTION",
        "_WORKER_RECYCLE_RSS_BYTES",
        "_WORKER_RSS",
    }),
})
_DECISION_ENGINE_TARGET_MODULES = (
    _gpu_capability_process_tree,
    _gpu_capability_calibration,
)
_DECISION_RUNTIME_STATE_EXCLUSIONS = MappingProxyType({
    "gpu_capability_process_tree": frozenset(),
    "gpu_capability_calibration": frozenset(),
})
_CLASS_STRUCTURAL_MEMBER_EXCLUSIONS = frozenset({
    "__dict__",
    "__doc__",
    "__module__",
    "__qualname__",
    "__slotnames__",
    "__weakref__",
})
_DATACLASS_GENERATED_MEMBER_EXCLUSIONS = frozenset({
    "__dataclass_fields__",
    "__dataclass_params__",
})
_ENUM_LIVE_STATE_MEMBERS = frozenset({
    "_hashable_values_",
    "_member_map_",
    "_member_names_",
    "_unhashable_values_",
    "_unhashable_values_map_",
    "_value2member_map_",
})
_CLASS_RUNTIME_STATE_EXCLUSIONS = MappingProxyType({
    "gpu_capability_model.CompactTokenSequence": frozenset({"_abc_impl"}),
    "gpu_capability_model._ReadOnlyPackedColumn": frozenset({"_abc_impl"}),
})


def _capture_module_origins_and_identities_at_import(
    module_names: tuple[str, ...],
) -> tuple[tuple[str, str, tuple[object, ...]], ...]:
    captured: list[tuple[str, str, tuple[object, ...]]] = []
    for module_name in module_names:
        module = sys.modules.get(module_name)
        specification = getattr(module, "__spec__", None)
        origin = getattr(specification, "origin", None)
        origin_role = f"python-module:{module_name}"
        if not isinstance(origin, str) or origin in ("built-in", "frozen"):
            captured.append((module_name, origin_role, (origin, None, None, None, None)))
            continue
        try:
            metadata = os.stat(origin, follow_symlinks=False)
            identity = (
                origin,
                int(metadata.st_dev),
                int(metadata.st_ino),
                int(metadata.st_size),
                int(metadata.st_mtime_ns),
            )
        except OSError:
            identity = (origin, None, None, None, None)
        captured.append((module_name, origin_role, identity))
    return tuple(captured)


_IMPORTED_MODULE_IDENTITIES = _capture_module_origins_and_identities_at_import(
    tuple(module.__name__ for module in _AUDIT_ENGINE_TARGET_MODULES)
)
_DECISION_IMPORTED_MODULE_IDENTITIES = _capture_module_origins_and_identities_at_import(
    tuple(module.__name__ for module in _DECISION_ENGINE_TARGET_MODULES)
)


def _canonical_module_origin_spelling(value: str) -> str:
    if not isinstance(value, str) or not value:
        raise AuditInfrastructureError("audit engine module origin is invalid")
    return os.path.normcase(os.path.normpath(os.path.abspath(value)))


def _build_module_origin_path_roles(
    identities: tuple[tuple[str, str, tuple[object, ...]], ...],
) -> MappingProxyType:
    roles: dict[str, str] = {}
    for _module_name, role, identity in identities:
        origin = identity[0]
        if origin is None:
            continue
        canonical = _canonical_module_origin_spelling(str(origin))
        previous = roles.get(canonical)
        if previous is not None and previous != role:
            raise AuditInfrastructureError(
                "audit engine module origins have conflicting semantic roles"
            )
        roles[canonical] = role
    return MappingProxyType(roles)


_MODULE_ORIGIN_PATH_ROLES = _build_module_origin_path_roles(
    _IMPORTED_MODULE_IDENTITIES + _DECISION_IMPORTED_MODULE_IDENTITIES
)


def _update_framed(digest, value: bytes) -> None:
    if not isinstance(value, bytes):
        raise AuditInfrastructureError("audit engine frame is invalid")
    digest.update(struct.pack("<Q", len(value)))
    digest.update(value)


def _semantic_origin_role(value: object) -> str:
    module_name = getattr(value, "__module__", None)
    qualified_name = getattr(value, "__qualname__", None)
    if not isinstance(module_name, str):
        module_name = type(value).__module__
    if not isinstance(qualified_name, str):
        qualified_name = getattr(value, "__name__", type(value).__qualname__)
    return f"{module_name}:{qualified_name}"


def _normalized_semantic_string(value: str) -> str:
    if not os.path.isabs(value):
        return value
    try:
        canonical = _canonical_module_origin_spelling(value)
    except (AuditInfrastructureError, OSError, ValueError):
        return value
    return _MODULE_ORIGIN_PATH_ROLES.get(canonical, value)


def _normalized_code_filename(value: str) -> str:
    try:
        canonical = _canonical_module_origin_spelling(value)
    except (AuditInfrastructureError, OSError, ValueError):
        return value
    return _MODULE_ORIGIN_PATH_ROLES.get(canonical, value)


class _LiveSemanticEncoder:
    """Deterministic structural encoder for the already-loaded audit engine."""

    def __init__(self, owned_module_names: frozenset[str] | None = None) -> None:
        if owned_module_names is None:
            owned_module_names = frozenset(
                module.__name__ for module in _AUDIT_ENGINE_TARGET_MODULES)
        if (not isinstance(owned_module_names, frozenset)
                or any(not isinstance(name, str) or not name
                       for name in owned_module_names)):
            raise AuditInfrastructureError(
                "semantic encoder module ownership is invalid")
        self._owned_module_names = owned_module_names
        self._require_frozen_dataclass_classes = owned_module_names == frozenset(
            module.__name__ for module in _AUDIT_ENGINE_TARGET_MODULES)
        # Retain the object as well as its ID. Structural encoding constructs
        # short-lived tuples; keeping them alive prevents a recycled ID from
        # being mistaken for a semantic cycle later in the same walk.
        self._seen: dict[int, tuple[int, object]] = {}

    @staticmethod
    def _frame(tag: bytes, pieces: Iterable[bytes] = ()) -> bytes:
        payload = bytearray(tag)
        for piece in pieces:
            payload.extend(struct.pack("<Q", len(piece)))
            payload.extend(piece)
        return bytes(payload)

    def _cycle_or_mark(self, value: object) -> bytes | None:
        identity = id(value)
        previous = self._seen.get(identity)
        if previous is not None and previous[1] is value:
            return self._frame(b"cycle", (str(previous[0]).encode("ascii"),))
        self._seen[identity] = (len(self._seen), value)
        return None

    def encode(self, value: object) -> bytes:
        if value is None:
            return b"none"
        if value is Ellipsis:
            return b"ellipsis"
        if isinstance(value, bool):
            return b"bool:1" if value else b"bool:0"
        if isinstance(value, int):
            return self._frame(b"int", (str(value).encode("ascii"),))
        if isinstance(value, float):
            return self._frame(b"float", (struct.pack("!d", value),))
        if isinstance(value, complex):
            return self._frame(
                b"complex", (struct.pack("!d", value.real), struct.pack("!d", value.imag))
            )
        if isinstance(value, bytes):
            return self._frame(b"bytes", (value,))
        if isinstance(value, str):
            return self._frame(
                b"str", (_normalized_semantic_string(value).encode("utf-8"),)
            )
        if isinstance(value, slice):
            return self._frame(
                b"slice",
                (
                    self.encode(value.start),
                    self.encode(value.stop),
                    self.encode(value.step),
                ),
            )
        if isinstance(value, PurePosixPath):
            if value.is_absolute() or any(part in ("", ".", "..") for part in value.parts):
                raise AuditInfrastructureError("audit engine contains an invalid POSIX path")
            return self._frame(b"posix-path", (value.as_posix().encode("utf-8"),))
        if isinstance(value, Path):
            normalized = _normalized_semantic_string(str(value))
            return self._frame(b"native-path", (normalized.encode("utf-8"),))
        if isinstance(value, re.Pattern):
            return self._frame(
                b"regex", (self.encode(value.pattern), self.encode(value.flags))
            )
        if isinstance(value, enum.Enum):
            return self._frame(
                b"enum-member",
                (
                    _semantic_origin_role(type(value)).encode("utf-8"),
                    value.name.encode("utf-8"),
                    self.encode(value.value),
                ),
            )
        if value is dataclasses._HAS_DEFAULT_FACTORY:
            return b"dataclass-default-factory"
        if isinstance(value, types.CodeType):
            return self._encode_code(value)
        if isinstance(value, types.FunctionType):
            if value.__module__ in self._owned_module_names:
                return self._encode_function(value)
            return self._frame(b"imported-function", (_semantic_origin_role(value).encode("utf-8"),))
        if isinstance(value, (types.BuiltinFunctionType, types.BuiltinMethodType)):
            return self._frame(b"imported-callable", (_semantic_origin_role(value).encode("utf-8"),))
        if isinstance(value, type):
            if value.__module__ in self._owned_module_names:
                return self._encode_class(value)
            return self._frame(b"imported-class", (_semantic_origin_role(value).encode("utf-8"),))
        if isinstance(value, types.ModuleType):
            return self._frame(b"imported-module", (value.__name__.encode("utf-8"),))
        if isinstance(value, types.GenericAlias):
            return self._frame(
                b"generic-alias",
                (self.encode(value.__origin__), self.encode(value.__args__)),
            )
        if type(value).__module__ == "typing":
            return self._frame(
                b"typing-object", (repr(value).encode("utf-8"),))
        if isinstance(value, tuple):
            cycle = self._cycle_or_mark(value)
            if cycle is not None:
                return cycle
            return self._frame(b"tuple", tuple(self.encode(item) for item in value))
        if isinstance(value, frozenset):
            cycle = self._cycle_or_mark(value)
            if cycle is not None:
                return cycle
            ordered_items = sorted((
                (_LiveSemanticEncoder(self._owned_module_names).encode(item), item)
                for item in value
            ), key=lambda encoded_item: encoded_item[0])
            element_encodings = tuple(item[0] for item in ordered_items)
            if len(set(element_encodings)) != len(element_encodings):
                raise AuditInfrastructureError(
                    "audit engine frozenset has duplicate semantic elements"
                )
            return self._frame(
                b"frozenset",
                tuple(self.encode(item) for _element_encoding, item in ordered_items),
            )
        if isinstance(value, MappingProxyType):
            cycle = self._cycle_or_mark(value)
            if cycle is not None:
                return cycle
            # Establish canonical key order using isolated encoders before the
            # main walk assigns cycle ordinals. Otherwise a shared immutable
            # value is expanded under whichever key was inserted first.
            ordered_items = sorted((
                (
                    _LiveSemanticEncoder(self._owned_module_names).encode(key),
                    key,
                    item,
                )
                for key, item in value.items()
            ), key=lambda encoded_item: encoded_item[0])
            key_encodings = tuple(item[0] for item in ordered_items)
            if len(set(key_encodings)) != len(key_encodings):
                raise AuditInfrastructureError(
                    "audit engine mapping has duplicate semantic keys"
                )
            pairs = tuple(
                (self.encode(key), self.encode(item))
                for _key_encoding, key, item in ordered_items
            )
            return self._frame(
                b"mapping-proxy",
                tuple(self._frame(b"item", pair) for pair in pairs),
            )
        if dataclasses.is_dataclass(value) and not isinstance(value, type):
            parameters = getattr(type(value), "__dataclass_params__", None)
            if parameters is None or not parameters.frozen:
                raise AuditInfrastructureError("audit engine dataclass state is mutable")
            cycle = self._cycle_or_mark(value)
            if cycle is not None:
                return cycle
            pieces = [_semantic_origin_role(type(value)).encode("utf-8")]
            for field in dataclasses.fields(value):
                field_value = getattr(value, field.name)
                if type(value) is _gpu_capability_model.AuditLimits and field.name == "workers":
                    field_value = "host-cpu-derived-worker-default"
                pieces.append(self._frame(b"field", (field.name.encode("utf-8"), self.encode(field_value))))
            return self._frame(b"frozen-dataclass", pieces)
        raise AuditInfrastructureError(
            "audit engine contains unsupported loaded semantic object: "
            f"{type(value).__module__}.{type(value).__qualname__}"
        )

    def _encode_code(self, code: types.CodeType) -> bytes:
        cycle = self._cycle_or_mark(code)
        if cycle is not None:
            return cycle
        metadata = (
            code.co_argcount,
            code.co_posonlyargcount,
            code.co_kwonlyargcount,
            code.co_nlocals,
            code.co_stacksize,
            code.co_flags,
        )
        return self._frame(
            b"code",
            (
                self.encode(metadata),
                self.encode(code.co_code),
                self.encode(code.co_consts),
                self.encode(code.co_names),
                self.encode(code.co_varnames),
                self.encode(code.co_freevars),
                self.encode(code.co_cellvars),
                self.encode(code.co_linetable),
                self.encode(code.co_exceptiontable),
                self.encode(_normalized_code_filename(code.co_filename)),
                self.encode(code.co_name),
                self.encode(code.co_qualname),
            ),
        )

    def _encode_function(self, function: types.FunctionType) -> bytes:
        cycle = self._cycle_or_mark(function)
        if cycle is not None:
            return cycle
        pieces = [
            _semantic_origin_role(function).encode("utf-8"),
            self._encode_code(function.__code__),
            self.encode(function.__defaults__),
        ]
        keyword_defaults = function.__kwdefaults__ or {}
        pieces.append(self._frame(
            b"keyword-defaults",
            tuple(
                self._frame(b"item", (key.encode("utf-8"), self.encode(value)))
                for key, value in sorted(keyword_defaults.items())
            ),
        ))
        pieces.append(self._frame(
            b"annotations",
            tuple(
                self._frame(b"item", (key.encode("utf-8"), self.encode(value)))
                for key, value in sorted(function.__annotations__.items())
            ),
        ))
        if function.__closure__ is None:
            pieces.append(b"no-closure")
        else:
            closure_values: list[bytes] = []
            for free_name, cell in zip(
                function.__code__.co_freevars, function.__closure__, strict=True
            ):
                try:
                    cell_value = cell.cell_contents
                except ValueError:
                    closure_values.append(b"empty-cell")
                    continue
                if (
                    function.__name__ == "__repr__"
                    and free_name == "repr_running"
                    and isinstance(cell_value, set)
                ):
                    closure_values.append(b"dataclass-repr-runtime-guard")
                else:
                    closure_values.append(self.encode(cell_value))
            pieces.append(self._frame(b"closure", closure_values))
        global_pieces: list[bytes] = []
        exclusions = _AUDIT_RUNTIME_STATE_EXCLUSIONS.get(function.__module__, frozenset())
        for name in _global_names_from_code(function.__code__):
            if name not in function.__globals__:
                continue
            if name in exclusions:
                encoded = b"excluded-runtime-state"
            elif name in {
                    "_IMPORTED_MODULE_IDENTITIES",
                    "_DECISION_IMPORTED_MODULE_IDENTITIES"}:
                identities = (
                    _IMPORTED_MODULE_IDENTITIES
                    if name == "_IMPORTED_MODULE_IDENTITIES"
                    else _DECISION_IMPORTED_MODULE_IDENTITIES
                )
                encoded = self.encode(tuple(
                    (module_name, role)
                    for module_name, role, _identity in identities
                ))
            elif name == "_MODULE_ORIGIN_PATH_ROLES":
                encoded = self.encode(frozenset(_MODULE_ORIGIN_PATH_ROLES.values()))
            else:
                global_value = function.__globals__[name]
                if (isinstance(global_value, (types.FunctionType, type))
                        and getattr(global_value, "__module__", None)
                        in self._owned_module_names):
                    encoded = self._frame(
                        b"owned-global-reference",
                        (_semantic_origin_role(global_value).encode("utf-8"),),
                    )
                else:
                    encoded = self.encode(global_value)
            global_pieces.append(self._frame(b"global", (name.encode("utf-8"), encoded)))
        pieces.append(self._frame(b"globals", global_pieces))
        return self._frame(b"function", pieces)

    def _encode_enum_live_sequence(self, name: str, value: object) -> bytes:
        if not isinstance(value, list):
            raise AuditInfrastructureError(
                f"audit engine Enum live sequence is invalid: {name}"
            )
        cycle = self._cycle_or_mark(value)
        if cycle is not None:
            return cycle
        return self._frame(
            b"enum-live-sequence",
            (name.encode("ascii"), *(self.encode(item) for item in value)),
        )

    def _encode_enum_live_mapping(
        self,
        name: str,
        value: object,
        *,
        preserve_order: bool,
    ) -> bytes:
        if not isinstance(value, dict):
            raise AuditInfrastructureError(
                f"audit engine Enum live mapping is invalid: {name}"
            )
        cycle = self._cycle_or_mark(value)
        if cycle is not None:
            return cycle
        if preserve_order:
            ordered_items = tuple(value.items())
        else:
            encoded_items = sorted(
                (
                    (
                        _LiveSemanticEncoder(
                            self._owned_module_names).encode(key),
                        key,
                        item,
                    )
                    for key, item in value.items()
                ),
                key=lambda encoded_item: encoded_item[0],
            )
            key_encodings = tuple(item[0] for item in encoded_items)
            if len(set(key_encodings)) != len(key_encodings):
                raise AuditInfrastructureError(
                    "audit engine Enum live mapping has duplicate semantic keys"
                )
            ordered_items = tuple(
                (key, item) for _key_encoding, key, item in encoded_items
            )
        return self._frame(
            b"enum-live-mapping",
            (
                name.encode("ascii"),
                *(
                    self._frame(b"item", (self.encode(key), self.encode(item)))
                    for key, item in ordered_items
                ),
            ),
        )

    def _encode_enum_live_state(
        self,
        class_object: enum.EnumMeta,
        name: str,
        value: object,
    ) -> bytes:
        if name == "_member_names_":
            if not isinstance(value, list) or any(
                not isinstance(member_name, str)
                or member_name not in class_object._member_map_
                for member_name in value
            ):
                raise AuditInfrastructureError(
                    "audit engine Enum member names are invalid"
                )
            return self._encode_enum_live_sequence(name, value)
        if name in {"_hashable_values_", "_unhashable_values_"}:
            return self._encode_enum_live_sequence(name, value)
        if name in {"_member_map_", "_value2member_map_"}:
            if not isinstance(value, dict) or any(
                not isinstance(member, class_object) for member in value.values()
            ):
                raise AuditInfrastructureError(
                    f"audit engine Enum member mapping is invalid: {name}"
                )
            if name == "_member_map_" and any(
                not isinstance(member_name, str) for member_name in value
            ):
                raise AuditInfrastructureError(
                    "audit engine Enum member-name mapping is invalid"
                )
            return self._encode_enum_live_mapping(
                name,
                value,
                preserve_order=name == "_member_map_",
            )
        return self._encode_enum_live_mapping(name, value, preserve_order=False)

    def _encode_class_descriptor(
        self,
        class_object: type,
        member_name: str,
        descriptor: object,
    ) -> bytes:
        if isinstance(descriptor, types.MemberDescriptorType):
            descriptor_kind = b"member-descriptor"
        elif isinstance(descriptor, types.GetSetDescriptorType):
            descriptor_kind = b"getset-descriptor"
        else:
            raise AuditInfrastructureError(
                "audit engine class descriptor type is unsupported"
            )
        descriptor_name = getattr(descriptor, "__name__", None)
        descriptor_owner = getattr(descriptor, "__objclass__", None)
        if (
            not isinstance(member_name, str)
            or not member_name
            or not isinstance(descriptor_name, str)
            or not descriptor_name
            or not isinstance(descriptor_owner, type)
        ):
            raise AuditInfrastructureError(
                "audit engine class descriptor state is invalid"
            )
        return self._frame(
            descriptor_kind,
            (
                member_name.encode("utf-8"),
                descriptor_name.encode("utf-8"),
                _semantic_origin_role(descriptor_owner).encode("utf-8"),
                self.encode(descriptor_owner is class_object),
            ),
        )

    def _encode_class(self, class_object: type) -> bytes:
        cycle = self._cycle_or_mark(class_object)
        if cycle is not None:
            return cycle
        pieces = [
            _semantic_origin_role(class_object).encode("utf-8"),
            self.encode(class_object.__bases__),
        ]
        if issubclass(class_object, enum.Enum):
            pieces.append(self._frame(
                b"enum-members",
                tuple(
                    self._frame(b"member", (name.encode("utf-8"), self.encode(member.value)))
                    for name, member in class_object.__members__.items()
                ),
            ))
        if dataclasses.is_dataclass(class_object):
            parameters = getattr(class_object, "__dataclass_params__", None)
            if (parameters is None
                    or (self._require_frozen_dataclass_classes
                        and not parameters.frozen)):
                raise AuditInfrastructureError("audit engine dataclass class is mutable")
            parameter_pieces = []
            for parameter_name in (
                "init",
                "repr",
                "eq",
                "order",
                "unsafe_hash",
                "frozen",
                "match_args",
                "kw_only",
                "slots",
                "weakref_slot",
            ):
                parameter_value = getattr(
                    parameters, parameter_name, "parameter-unavailable"
                )
                parameter_pieces.append(self._frame(
                    b"parameter",
                    (
                        parameter_name.encode("ascii"),
                        self.encode(parameter_value),
                    ),
                ))
            pieces.append(self._frame(b"dataclass-parameters", parameter_pieces))
            field_pieces: list[bytes] = []
            for field in dataclasses.fields(class_object):
                default = field.default
                default_bytes = (
                    b"missing"
                    if default is dataclasses.MISSING
                    else self.encode(default)
                )
                factory = field.default_factory
                factory_bytes = (
                    b"missing"
                    if factory is dataclasses.MISSING
                    else self.encode(factory)
                )
                field_pieces.append(self._frame(
                    b"field",
                    (
                        field.name.encode("utf-8"),
                        self.encode(field.type),
                        default_bytes,
                        factory_bytes,
                    ),
                ))
            pieces.append(self._frame(b"dataclass-fields", field_pieces))
        class_role = f"{class_object.__module__}.{class_object.__qualname__}"
        runtime_exclusions = _CLASS_RUNTIME_STATE_EXCLUSIONS.get(
            class_role, frozenset()
        )
        missing_runtime_exclusions = runtime_exclusions - set(class_object.__dict__)
        if missing_runtime_exclusions:
            raise AuditInfrastructureError(
                "audit engine class runtime-state exclusion does not exist: "
                + ", ".join(sorted(missing_runtime_exclusions))
            )
        for name, member in sorted(class_object.__dict__.items()):
            if name in _CLASS_STRUCTURAL_MEMBER_EXCLUSIONS:
                continue
            if (
                dataclasses.is_dataclass(class_object)
                and name in _DATACLASS_GENERATED_MEMBER_EXCLUSIONS
            ):
                continue
            if name in runtime_exclusions:
                continue
            encoded: bytes | None = None
            if isinstance(member, staticmethod):
                encoded = self._frame(b"staticmethod", (self.encode(member.__func__),))
            elif isinstance(member, classmethod):
                encoded = self._frame(b"classmethod", (self.encode(member.__func__),))
            elif isinstance(member, property):
                encoded = self._frame(
                    b"property",
                    tuple(
                        self.encode(accessor) if accessor is not None else b"none"
                        for accessor in (member.fget, member.fset, member.fdel)
                    ),
                )
            elif isinstance(member, types.FunctionType):
                encoded = self.encode(member)
            elif isinstance(member, (types.MemberDescriptorType, types.GetSetDescriptorType)):
                encoded = self._encode_class_descriptor(class_object, name, member)
            elif isinstance(class_object, enum.EnumMeta) and isinstance(member, class_object):
                continue
            elif (
                isinstance(class_object, enum.EnumMeta)
                and name in _ENUM_LIVE_STATE_MEMBERS
            ):
                encoded = self._encode_enum_live_state(class_object, name, member)
            elif name == "__annotations__":
                if not isinstance(member, dict) or any(
                    not isinstance(key, str) for key in member
                ):
                    raise AuditInfrastructureError(
                        "audit engine class annotations are invalid"
                    )
                encoded = self._frame(
                    b"class-annotations",
                    tuple(
                        self._frame(
                            b"annotation",
                            (key.encode("utf-8"), self.encode(value)),
                        )
                        for key, value in sorted(member.items())
                    ),
                )
            else:
                encoded = self.encode(member)
            pieces.append(self._frame(b"class-member", (name.encode("utf-8"), encoded)))
        return self._frame(b"class", pieces)


def _global_names_from_code(code: types.CodeType) -> tuple[str, ...]:
    if not isinstance(code, types.CodeType):
        raise AuditInfrastructureError("audit engine code object is invalid")
    pending = [code]
    seen: set[int] = set()
    names: set[str] = set()
    while pending:
        current = pending.pop()
        identity = id(current)
        if identity in seen:
            continue
        seen.add(identity)
        names.update(current.co_names)
        pending.extend(
            constant
            for constant in current.co_consts
            if isinstance(constant, types.CodeType)
        )
    return tuple(sorted(names))


def _marshal_live_semantic_object(
    loaded_object: object,
    owned_module_names: frozenset[str] | None = None,
) -> bytes:
    return _LiveSemanticEncoder(owned_module_names).encode(loaded_object)


def _is_semantic_constant_name(name: str) -> bool:
    return (
        not name.startswith("__")
        and (not name.startswith("_WORKER_")
             or name == "_WORKER_SELECTION_ORDER")
        and any(character.isalpha() for character in name)
        and name.upper() == name
        and name not in {
            "_IMPORTED_MODULE_IDENTITIES",
            "_DECISION_IMPORTED_MODULE_IDENTITIES",
        }
    )


def _walk_live_semantic_graph_cycle_safe(
    *,
    target_modules: tuple[types.ModuleType, ...],
    runtime_state_exclusions: Mapping[str, frozenset[str]],
) -> tuple[tuple[str, object], ...]:
    module_names = tuple(module.__name__ for module in target_modules)
    if set(runtime_state_exclusions) != set(module_names):
        raise AuditInfrastructureError(
            "audit engine runtime-state exclusion modules are not exact"
        )
    roots: dict[str, object] = {}
    for module in target_modules:
        excluded = runtime_state_exclusions[module.__name__]
        if not isinstance(excluded, frozenset) or any(
            not isinstance(name, str) or not name for name in excluded
        ):
            raise AuditInfrastructureError(
                "audit engine runtime-state exclusion manifest is invalid"
            )
        missing = excluded - set(vars(module))
        if missing:
            raise AuditInfrastructureError(
                "audit engine runtime-state exclusion does not exist: "
                + ", ".join(sorted(missing))
            )
        for name in excluded:
            value = vars(module)[name]
            owned = (
                isinstance(value, (types.FunctionType, type))
                and getattr(value, "__module__", None) == module.__name__
            )
            if owned or _is_semantic_constant_name(name):
                raise AuditInfrastructureError(
                    "audit engine runtime-state exclusion names a semantic symbol: "
                    f"{module.__name__}.{name}"
                )
        for name, value in vars(module).items():
            if name in excluded:
                continue
            owned = (
                isinstance(value, (types.FunctionType, type))
                and getattr(value, "__module__", None) == module.__name__
            )
            if owned or _is_semantic_constant_name(name):
                roots[f"{module.__name__}.{name}"] = value
    return tuple(sorted(roots.items()))


def _enumerate_live_semantic_graph() -> tuple[tuple[str, object], ...]:
    return _walk_live_semantic_graph_cycle_safe(
        target_modules=_AUDIT_ENGINE_TARGET_MODULES,
        runtime_state_exclusions=_AUDIT_RUNTIME_STATE_EXCLUSIONS,
    )


def _enumerate_live_decision_semantic_graph(
    audit_digest: str,
) -> tuple[tuple[str, object], ...]:
    if (not isinstance(audit_digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", audit_digest) is None):
        raise AuditInfrastructureError("decision audit digest is invalid")
    graph = list(_walk_live_semantic_graph_cycle_safe(
        target_modules=_DECISION_ENGINE_TARGET_MODULES,
        runtime_state_exclusions=_DECISION_RUNTIME_STATE_EXCLUSIONS,
    ))
    graph.append(("decision-input.audit-engine-fingerprint", audit_digest.encode("ascii")))
    return tuple(sorted(graph))


def _audit_engine_fingerprint_from_marshaled_graph(
    graph: tuple[tuple[str, bytes], ...],
) -> str:
    if not isinstance(graph, tuple):
        raise AuditInfrastructureError("marshaled audit engine graph is invalid")
    names: list[str] = []
    digest = hashlib.sha256()
    _update_framed(digest, AUDIT_ENGINE_GRAPH_SCHEMA_BYTES)
    _update_framed(digest, AUDIT_ENGINE_STAGE_BYTES)
    for entry in graph:
        if (
            not isinstance(entry, tuple)
            or len(entry) != 2
            or not isinstance(entry[0], str)
            or not isinstance(entry[1], bytes)
        ):
            raise AuditInfrastructureError("marshaled audit engine entry is invalid")
        name, payload = entry
        try:
            encoded_name = name.encode("ascii")
        except UnicodeEncodeError as error:
            raise AuditInfrastructureError("audit engine component name is invalid") from error
        names.append(name)
        _update_framed(digest, encoded_name)
        _update_framed(digest, payload)
    if tuple(names) != tuple(sorted(names)) or len(set(names)) != len(names):
        raise AuditInfrastructureError("marshaled audit engine graph is not unique and sorted")
    for module_name, origin_role, _local_identity in _IMPORTED_MODULE_IDENTITIES:
        _update_framed(digest, f"module-root:{module_name}".encode("ascii"))
        _update_framed(digest, origin_role.encode("ascii"))
    return digest.hexdigest()


def audit_engine_fingerprint() -> str:
    graph = tuple(
        (name, _marshal_live_semantic_object(loaded_object))
        for name, loaded_object in _enumerate_live_semantic_graph()
    )
    return _audit_engine_fingerprint_from_marshaled_graph(graph)


def _attest_loaded_audit_engine(expected: str | None = None) -> str:
    actual = audit_engine_fingerprint()
    if expected is not None and (
        not isinstance(expected, str)
        or re.fullmatch(r"[0-9a-f]{64}", expected) is None
        or not hmac.compare_digest(actual, expected)
    ):
        raise AuditInfrastructureError("loaded audit engine attestation mismatch")
    return actual


def decision_engine_fingerprint(
    expected_audit_engine_fingerprint: str | None = None,
) -> str:
    audit_digest = _attest_loaded_audit_engine(expected_audit_engine_fingerprint)
    graph = _enumerate_live_decision_semantic_graph(audit_digest)
    digest = hashlib.sha256()
    _update_framed(digest, DECISION_ENGINE_GRAPH_SCHEMA_BYTES)
    _update_framed(digest, audit_digest.encode("ascii"))
    names: list[str] = []
    for name, value in graph:
        names.append(name)
        _update_framed(digest, name.encode("ascii"))
        _update_framed(digest, _marshal_live_semantic_object(
            value,
            frozenset(module.__name__
                      for module in _DECISION_ENGINE_TARGET_MODULES),
        ))
    if tuple(names) != tuple(sorted(names)) or len(names) != len(set(names)):
        raise AuditInfrastructureError(
            "decision engine graph is not unique and sorted")
    for module_name, origin_role, _local_identity in _DECISION_IMPORTED_MODULE_IDENTITIES:
        _update_framed(digest, f"module-root:{module_name}".encode("ascii"))
        _update_framed(digest, origin_role.encode("ascii"))
    return digest.hexdigest()


def profile_compiler_view(
    source_root: Path,
    database: Path,
    source: PurePosixPath,
) -> str:
    """Run one representative real compiler view as a developer diagnostic."""

    root = source_root.resolve(strict=True)
    database = database.resolve(strict=True)
    external_roots: dict[str, Path] = {}
    if os.name == "nt":
        # Keep the diagnostic authority as narrow as the real compiler view.
        # Drive roots end in a separator and are intentionally rejected by the
        # canonical dependency identity validator.
        for role, candidate in (
            ("qt-toolchain", Path("C:/Qt")),
            ("windows-system", Path(os.environ.get("SystemRoot", "C:/Windows"))),
        ):
            if candidate.is_dir():
                external_roots[role] = candidate.resolve(strict=True)
        shared_checkout = root.parents[2] if len(root.parents) > 2 else root.parent
        sibling_dependencies = shared_checkout / "windows_build"
        if sibling_dependencies.is_dir():
            external_roots["workspace-dependencies"] = sibling_dependencies
    authority = build_dependency_root_authority(root, external_roots)
    # The diagnostic deliberately exercises a full Qt translation unit.  The
    # production defaults remain unchanged; allow the Windows stream pump and
    # Python provenance parser enough time to construct this one profile view.
    profile_limits = dataclasses.replace(
        AuditLimits(), invocation_seconds=300.0, total_seconds=600.0
    )
    deadline = time.monotonic() + profile_limits.total_seconds
    configurations = _gpu_capability_runner.collect_configurations(
        root,
        (database,),
        dict(os.environ),
        authority,
        deadline,
    )
    matches = tuple(
        configuration
        for configuration in configurations
        if configuration.source.relative == source
    )
    if not matches:
        raise AuditInfrastructureError(
            f"profile source has no compile configuration: {source}"
        )
    production = enumerate_production_identities(
        root, profile_limits, deadline
    )
    configuration = matches[0]
    build_started = time.perf_counter()
    view, _discovery, _stages = (
        _gpu_capability_runner.stabilize_and_parse_configuration(
            configuration,
            authority,
            production,
            profile_limits,
            deadline,
        )
    )
    build_elapsed = time.perf_counter() - build_started
    started = time.perf_counter()
    findings = audit_preprocessed_view(
        view, profile_limits, _current_process_rss_bytes
    )
    elapsed = time.perf_counter() - started
    if findings:
        raise AuditInfrastructureError(
            "profile source produced capability findings: "
            + "; ".join(
                findings[index].render()
                for index in range(min(4, len(findings)))
            )
        )
    if elapsed >= 5.0:
        raise AuditInfrastructureError(
            f"representative warm view audit exceeded five seconds: {elapsed:.3f}s"
        )
    return (
        f"source={source} configurations={len(matches)} "
        f"view={build_elapsed:.3f}s audit={elapsed:.3f}s"
    )


def main(argv: tuple[str, ...] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    # Preserve the committed CTest interface while the compiler-authoritative
    # lane consumes preprocessed views instead of the deleted macro table.
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--profile-view", type=Path)
    parser.add_argument("--profile-source", type=PurePosixPath)
    parser.add_argument("--performance-only", action="store_true")
    parser.add_argument("--correctness-only", action="store_true")
    parser.add_argument("--worker-decision", type=Path)
    parser.add_argument("--dependency-root", action="append", default=[])
    parser.add_argument("--live-only", action="store_true")
    parser.add_argument("--live-compiler", action="append", default=[])
    parser.add_argument("--require-live-family", action="append", default=[])
    args = parser.parse_args(argv)

    if args.correctness_only:
        if (
            args.profile_view is not None
            or args.profile_source is not None
            or args.performance_only
            or args.live_only
            or args.live_compiler
            or args.require_live_family
        ):
            parser.error("--correctness-only cannot be combined with another mode")
        if args.compile_commands is None or args.worker_decision is None:
            parser.error(
                "--correctness-only requires --compile-commands and --worker-decision")
        try:
            run_correctness_only_cli(args)
        except (AuditInfrastructureError, OSError) as error:
            print(f"FAIL: GPU capability correctness audit: {error}")
            return 2
        print("PASS: GPU capability correctness audit")
        return 0

    if (args.profile_view is None) != (args.profile_source is None):
        parser.error("--profile-view and --profile-source must be supplied together")
    if args.profile_view is not None:
        if (
            args.performance_only
            or args.live_only
            or args.compile_commands is not None
            or args.live_compiler
            or args.require_live_family
        ):
            parser.error("profile mode cannot be combined with other audit modes")
        try:
            result = profile_compiler_view(
                args.source_root, args.profile_view, args.profile_source
            )
        except (AuditInfrastructureError, OSError) as error:
            print(f"FAIL: GPU capability compiler-view profile: {error}")
            return 2
        print("PASS: GPU capability compiler-view profile: " + result)
        return 0

    if args.live_only:
        if args.performance_only or args.compile_commands is not None:
            parser.error(
                "--live-only cannot be combined with compiler audit or performance mode"
            )
        try:
            compilers = parse_live_compiler_options(tuple(args.live_compiler))
            required = parse_required_live_families(
                tuple(args.require_live_family)
            )
            from test_gpu_capability_live_compilers import (
                run_live_compiler_suite,
            )
            run_live_only(
                compilers,
                required,
                suite_runner=run_live_compiler_suite,
            )
        except AuditInfrastructureError as error:
            print(f"FAIL: GPU capability live compiler infrastructure: {error}")
            return 2
        print("PASS: GPU capability live compiler parity")
        return 0
    if args.live_compiler or args.require_live_family:
        parser.error("live compiler selections require --live-only")
    if args.performance_only:
        print("PASS: GPU capability source-audit performance: " + performance_self_tests())
        return 0
    mutation_self_tests()
    root = args.source_root.resolve()
    sources = load_production_sources(root)
    findings = audit_raw_sources(sources)
    if findings:
        for finding in sorted(findings, key=lambda item: (str(item.path), item.line, item.expression)):
            print(finding.render())
        return 1
    print("PASS: GPU capability source audit and mutation self-tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
