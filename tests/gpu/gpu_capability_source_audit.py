#!/usr/bin/env python3
"""Audit production GPU capability use without widening the public API."""

from __future__ import annotations

import argparse
from array import array
import bisect
from collections.abc import Mapping
import ctypes
from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Callable, Iterable

from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    PreprocessedTranslationUnitView,
    SourceLocation,
)


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm"}
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

REVIEWED_NATIVE_HANDLE_SINKS = {
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
}

REVIEWED_NATIVE_HANDLE_METHODS = {
    PurePosixPath("recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"):
        frozenset({"GetDevice"}),
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"):
        frozenset({"GetDesc", "GetDevice"}),
}

REVIEWED_NATIVE_HANDLE_MEMBER_SINKS = {
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"):
        frozenset({"CopySubresourceRegion"}),
}

REVIEWED_NATIVE_HANDLE_TYPES = {
    PurePosixPath("playback/gpu/applegpusurface_apple.mm"):
        frozenset({"IOSurfaceRef"}),
    PurePosixPath("recorder_engine/codec/nativevideoencoder_videotoolbox.mm"):
        frozenset({"IOSurfaceRef"}),
    PurePosixPath("recorder_engine/codec/nativevideoencoder_mediafoundation.cpp"):
        frozenset({"ID3D11Texture2D"}),
    PurePosixPath("playback/output/win/wingpuimportedge.cpp"):
        frozenset({"ID3D11Texture2D"}),
}

_CAPABILITY_CANDIDATE_SPELLINGS = frozenset({
    b"GpuOpScope",
    b"GpuRetireRegistry",
    b"GpuSyncReadScope",
    b"_longjmp",
    b"complete",
    b"longjmp",
    b"nativeHandle",
    b"read",
    b"registerRetire",
    b"siglongjmp",
    b"track",
    b"withRead",
}).union(
    name.encode("ascii")
    for table in (
        REVIEWED_NATIVE_HANDLE_SINKS,
        REVIEWED_NATIVE_HANDLE_METHODS,
        REVIEWED_NATIVE_HANDLE_MEMBER_SINKS,
        REVIEWED_NATIVE_HANDLE_TYPES,
    )
    for names in table.values()
    for name in names
)


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
        alternatives = {
            b"%:%:": b"##  ",
            b"<:": b"[ ",
            b":>": b"] ",
            b"<%": b"{ ",
            b"%>": b"} ",
            b"%:": b"# ",
        }
        return alternatives.get(spelling, spelling)

    @classmethod
    def from_preprocessed(
        cls,
        view: PreprocessedTranslationUnitView,
        limits: AuditLimits,
        rss_reader: Callable[[], int],
    ) -> "AuditBuffer":
        tokens = view.tokens
        normalized = bytearray()
        columns = tuple(array("I") for _unused in range(cls._RUN_COLUMN_COUNT))
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
        production_prefix.append(0)
        origin_ids_by_key: dict[tuple[Path | None, PurePosixPath | None, bool], int] = {}
        origin_production = bytearray()
        peak_rss = cls._sample_rss(rss_reader, limits.rss_bytes, 0)
        bytes_since_sample = 0

        def mapping_bytes(extra_runs: int = 0) -> int:
            runs = len(run_starts) + extra_runs
            return (runs * (cls._RUN_COLUMN_COUNT - 1) + runs + 1) * 4

        def retained_bytes(
            *, extra_normalized: int = 0, extra_runs: int = 0,
            extra_origins: int = 0,
        ) -> int:
            return (
                len(normalized)
                + extra_normalized
                + mapping_bytes(extra_runs)
                + len(origin_production)
                + extra_origins
            )

        def append(piece: bytes) -> None:
            nonlocal bytes_since_sample, peak_rss
            projected = retained_bytes(extra_normalized=len(piece))
            if projected > limits.retained_token_bytes:
                raise AuditInfrastructureError("normalized audit byte limit exceeded")
            if bytes_since_sample + len(piece) >= cls._BLOCK_BYTES:
                peak_rss = cls._sample_rss(
                    rss_reader,
                    limits.rss_bytes,
                    peak_rss,
                    reserve=cls._BLOCK_BYTES,
                )
                bytes_since_sample = 0
            normalized.extend(piece)
            bytes_since_sample += len(piece)

        for packed_run in tokens.iter_runs():
            if len(run_starts) % cls._RUN_BLOCK == 0:
                reserve = cls._RUN_BLOCK * (cls._RUN_COLUMN_COUNT * 4 + 1)
                peak_rss = cls._sample_rss(
                    rss_reader, limits.rss_bytes, peak_rss, reserve=reserve
                )
            if retained_bytes(extra_runs=1) > limits.retained_token_bytes:
                raise AuditInfrastructureError("normalized audit byte limit exceeded")
            normalized_start = len(normalized)
            for token_index in range(packed_run.start, packed_run.stop):
                if token_index != packed_run.start:
                    append(b" ")
                spelling_id = tokens.spelling_id_at(token_index)
                append(cls._audit_spelling(tokens.spelling_for(spelling_id)))
            append(b"\n")
            if retained_bytes(extra_runs=1) > limits.retained_token_bytes:
                raise AuditInfrastructureError("normalized audit byte limit exceeded")
            identity = tokens.identity_for(packed_run.identity_id)
            origin_key = (
                identity.canonical if identity is not None else None,
                identity.relative if identity is not None else None,
                bool(identity is not None and identity.production),
            )
            origin_id = origin_ids_by_key.get(origin_key)
            if origin_id is None:
                if (
                    retained_bytes(extra_runs=1, extra_origins=1)
                    > limits.retained_token_bytes
                ):
                    raise AuditInfrastructureError("normalized audit byte limit exceeded")
                identity_bytes = sum(
                    len(str(value).encode("utf-8", errors="surrogatepass"))
                    for value in origin_key[:2] if value is not None
                )
                peak_rss = cls._sample_rss(
                    rss_reader,
                    limits.rss_bytes,
                    peak_rss,
                    reserve=1025 + identity_bytes,
                )
                origin_id = len(origin_ids_by_key)
                origin_ids_by_key[origin_key] = origin_id
                origin_production.append(int(origin_key[2]))
            changes = change_prefix[-1] if change_prefix else 0
            if (
                origin_ids
                and (
                    origin_ids[-1] != origin_id
                    or inclusion_ids[-1] != packed_run.inclusion_instance
                )
            ):
                changes += 1
            run_starts.append(normalized_start)
            run_stops.append(len(normalized))
            token_starts.append(packed_run.start)
            token_stops.append(packed_run.stop)
            identity_ids.append(packed_run.identity_id)
            origin_ids.append(origin_id)
            inclusion_ids.append(packed_run.inclusion_instance)
            original_lines.append(packed_run.original_line)
            change_prefix.append(changes)
            production_prefix.append(production_prefix[-1] + int(origin_key[2]))
        if retained_bytes() > limits.retained_token_bytes:
            raise AuditInfrastructureError("normalized audit byte limit exceeded")
        peak_rss = cls._sample_rss(
            rss_reader,
            limits.rss_bytes,
            peak_rss,
            reserve=len(normalized) + len(origin_production),
        )
        immutable_origin_production = bytes(origin_production)
        try:
            text = normalized.decode("latin-1", errors="strict")
        except UnicodeDecodeError as error:  # pragma: no cover - latin-1 is total
            raise AuditInfrastructureError("cannot normalize preprocessed audit bytes") from error
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

    def candidate_paths(self) -> tuple[PurePosixPath, ...]:
        result: set[PurePosixPath] = set()
        for run_index in range(len(self._run_starts)):
            identity = self._view.tokens.identity_for(self._identity_ids[run_index])
            if (
                identity is not None
                and identity.production
                and identity.relative is not None
                and is_production_path(identity.relative)
            ):
                result.add(identity.relative)
        return tuple(sorted(result, key=PurePosixPath.as_posix))

    def has_capability_spelling(self) -> bool:
        for run_index in range(len(self._run_starts)):
            for token_index in range(
                self._token_run_starts[run_index], self._token_run_stops[run_index]
            ):
                if self.token_spelling(token_index) in _CAPABILITY_CANDIDATE_SPELLINGS:
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


def cpp_tokens(masked: str, start: int = 0, end: int | None = None) -> list[CppToken]:
    """Tokenize enough C++ punctuation to enforce a conservative local grammar."""
    limit = len(masked) if end is None else end
    return [CppToken(match.group(), match.start(), match.end())
            for match in TOKEN_PATTERN.finditer(masked, start, limit)]


def mask_non_code(source: str) -> str:
    """Replace comments and literals with spaces while preserving offsets/lines."""
    result = list(source)
    state = "code"
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


MEMBER_CALL = re.compile(r"(?:\.|->|::)\s*(read|withRead|complete|nativeHandle)\s*\(")


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
                        token.value for token in cpp_tokens(replacement)), parameters,
                                    variadic)))
        offset += len(line)
    return events, directives


def guarded_macro_composition_findings(
        path: PurePosixPath, translated: TranslationText,
        compiler_macros: Mapping[str, MacroDefinition] | frozenset[str] = frozenset()
        ) -> list[Finding]:
    """Reject source-visible macro calls whose identifier pieces form guarded names."""
    guarded = {
        "GpuSyncReadScope", "_longjmp", "complete", "longjmp", "nativeHandle", "read",
        "siglongjmp", "withRead",
    }
    guarded.update(REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset()))
    guarded.update(REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset()))
    guarded.update(REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset()))
    tokens = cpp_tokens(translated.masked)
    macro_events, directive_ranges = source_macro_events(translated.masked)
    macros: dict[str, MacroDefinition] = (
        dict(compiler_macros) if isinstance(compiler_macros, Mapping) else {})
    event_index = 0
    findings: list[Finding] = []

    class ExpansionDepthExceeded(RuntimeError):
        pass

    class ExpansionComplexityExceeded(RuntimeError):
        pass

    class ExpansionSyntaxFailure(RuntimeError):
        pass

    @dataclass(frozen=True)
    class ExpansionToken:
        value: str
        hidden: frozenset[str] = frozenset()

    maximum_expansion_depth = 96
    maximum_continuous_macro_tokens = 256
    maximum_generated_macro_tokens = 2048
    maximum_macro_paste_operations = 1024

    @dataclass
    class ExpansionBudget:
        remaining_tokens: int = maximum_generated_macro_tokens
        remaining_pastes: int = maximum_macro_paste_operations

        def reserve_tokens(self, count: int) -> None:
            if count < 0 or count > self.remaining_tokens:
                raise ExpansionComplexityExceeded
            self.remaining_tokens -= count

        def reserve_paste(self) -> None:
            if self.remaining_pastes <= 0:
                raise ExpansionComplexityExceeded
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
            ExpansionToken(value.value, value.hidden | hidden) for value in values)

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
                        suffix_truncated: bool) \
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
        prescanned_arguments = {
            parameter: expand_sequence(
                with_hidden(argument, argument_hidden), depth + 1, budget,
                suffix_truncated)
            for parameter, argument in raw_arguments.items()
        }
        if variadic is not None:
            raw_variadic = supplied[fixed_count:]
            raw_arguments[variadic] = join_variadic(raw_variadic, argument_hidden)
            prescanned_arguments[variadic] = join_variadic([
                expand_sequence(
                    with_hidden(argument, argument_hidden), depth + 1, budget,
                    suffix_truncated)
                for argument in raw_variadic
            ], argument_hidden)

        replacement = definition.replacement
        substituted: list[ExpansionToken] = []
        cursor = 0
        while cursor < len(replacement):
            value = replacement[cursor]
            if (value == "#" and cursor + 1 < len(replacement)
                    and replacement[cursor + 1] in raw_arguments):
                budget.reserve_tokens(1)
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
                            else prescanned_arguments[value])
                selected_with_hidden = with_hidden(selected, argument_hidden)
                if adjacent_to_paste and not selected_with_hidden:
                    selected_with_hidden = (
                        ExpansionToken("__macro_placemarker__", replacement_hidden),)
                budget.reserve_tokens(len(selected_with_hidden))
                substituted.extend(selected_with_hidden)
            else:
                budget.reserve_tokens(1)
                substituted.append(ExpansionToken(value, replacement_hidden))
            cursor += 1

        return paste_tokens(substituted, budget, replacement_hidden)

    def expand_sequence(values: tuple[ExpansionToken, ...], depth: int = 0,
                        budget: ExpansionBudget | None = None,
                        suffix_truncated: bool = False) \
            -> tuple[ExpansionToken, ...]:
        if depth > maximum_expansion_depth:
            raise ExpansionDepthExceeded
        if budget is None:
            budget = ExpansionBudget()
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
                replacement_hidden = token.hidden | {value}
                budget.reserve_tokens(len(definition.replacement))
                replacement_values = [
                    ExpansionToken(item, replacement_hidden)
                    for item in definition.replacement]
                replacement = paste_tokens(
                    replacement_values, budget, replacement_hidden)
                rescanned = expand_sequence(
                    replacement + values[cursor + 1:], depth + 1, budget,
                    suffix_truncated)
                return tuple(expanded) + rescanned
            if (cursor + 1 >= len(values)
                    or values[cursor + 1].value != "("):
                expanded.append(token)
                cursor += 1
                continue
            parsed = value_call_arguments(values, cursor + 1)
            if parsed is None:
                if suffix_truncated:
                    raise ExpansionComplexityExceeded
                raise ExpansionSyntaxFailure
            arguments, closing = parsed
            replacement = expand_function(
                value, definition, arguments, depth + 1,
                token.hidden, budget, suffix_truncated)
            rescanned = expand_sequence(
                replacement + values[closing + 1:], depth + 1, budget,
                suffix_truncated)
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

    for index, token in enumerate(tokens[:-1]):
        while (event_index < len(macro_events)
               and macro_events[event_index][0] <= token.start):
            _position, name, definition = macro_events[event_index]
            if definition is None:
                macros.pop(name, None)
            else:
                macros[name] = definition
            event_index += 1
        definition = macros.get(token.value)
        if (not re.fullmatch(r"[A-Za-z_]\w*", token.value)
                or definition is None
                or any(start <= token.start < end for start, end in directive_ranges)):
            continue
        too_complex = False
        if definition.function_like:
            if tokens[index + 1].value != "(":
                continue
            closing = paren_closings.get(index + 1)
            if closing is None:
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
                    and any(start <= suffix_token.start < end
                            for start, end in directive_ranges)):
                break
            suffix_end += 1
            if suffix_token.value in {";", "{", "}"}:
                break
        suffix_truncated = (
            suffix_end < len(tokens)
            and tokens[suffix_end - 1].value not in {";", "{", "}"}
            and not any(start <= tokens[suffix_end].start < end
                        for start, end in directive_ranges))
        invocation = tuple(
            ExpansionToken(item.value) for item in tokens[index:suffix_end])
        try:
            expansion = expand_sequence(
                invocation, suffix_truncated=suffix_truncated)
        except ExpansionDepthExceeded:
            findings.append(Finding(
                path, translated.line_at(token.start), "macro expansion depth",
                "macro expansion depth exceeded the bounded audit limit; rejected fail-closed"))
            continue
        except ExpansionComplexityExceeded:
            findings.append(Finding(
                path, translated.line_at(token.start), "macro expansion complexity",
                "macro expansion complexity exceeded the bounded audit limit; "
                "rejected fail-closed"))
            continue
        except ExpansionSyntaxFailure:
            findings.append(Finding(
                path, translated.line_at(token.start), "macro expansion syntax",
                "macro expansion syntax is incomplete; rejected fail-closed"))
            continue
        if not expansion or expansion[0].value not in guarded:
            continue
        findings.append(Finding(
            path, translated.line_at(token.start), "guarded identifier macro composition",
            "macro arguments cannot be composed into guarded GPU or non-local control names"))
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
    all_tokens = cpp_tokens(masked)
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
    for token in cpp_tokens(masked):
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
    tokens = cpp_tokens(masked, 0, block[0])
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
    prior_tokens = cpp_tokens(masked, 0, position)
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
    tokens = cpp_tokens(masked, binding.block[0] + 1, binding.block[1])
    identity_tokens = cpp_tokens(masked, 0, binding.block[1])
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
    tokens = cpp_tokens(masked, declaration_position, binding.block[1])
    safe_calls = REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
    safe_member_calls = REVIEWED_NATIVE_HANDLE_MEMBER_SINKS.get(path, frozenset())
    safe_methods = REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())
    alias_block = immediate_block(pairs, declaration_position)
    if alias_block is None:
        return False, declaration_position
    last_use = declaration_position
    consumed = False
    tokens = cpp_tokens(masked, alias_block[0] + 1, alias_block[1])
    identity_tokens = cpp_tokens(masked, 0, alias_block[1])
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


def scope_bindings_linear(masked: str,
                          pairs: list[tuple[int, int]]) -> list[ScopeBinding]:
    declarations = scope_declarations(masked)
    assigned = blocks_for_positions(
        masked, pairs, [declaration.position for declaration in declarations])
    opening_pairs = {opening: (opening, closing) for opening, closing in pairs}
    paren_pairs = delimiter_pairs(masked, "(", ")")
    declaration_parens = blocks_for_positions(
        masked, paren_pairs, [declaration.position for declaration in declarations])
    next_nonspace = next_nonspace_indices(masked)
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
    tokens = cpp_tokens(receiver_expression(masked, call_position))

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
            return resolve(start, end - 4)
        opening = next((index for index in range(start, end)
                        if tokens[index].value == "("), None)
        if opening is not None and tokens[end - 1].value == ")":
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
            if (function_index >= start and tokens[function_index].value in {
                    "as_const", "cref", "forward", "move", "ref"}):
                return resolve(opening + 1, end - 1)
        if (end - start == 1
                and re.fullmatch(r"[A-Za-z_]\w*", tokens[start].value)):
            return tokens[start].value
        return None

    return resolve(0, len(tokens))


def receiver_binding_references(masked: str, call_position: int) -> set[str]:
    """Return unqualified value names conservatively referenced by a receiver."""
    receiver = receiver_expression(masked, call_position)
    tokens = cpp_tokens(receiver)
    receiver_start = masked.rfind(receiver, 0, call_position)
    dependent_member_receiver = (
        receiver_start >= 0
        and re.search(r"(?:\.|->|::)\s*template\s*$",
                      masked[:receiver_start]) is not None)
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
    tokens = cpp_tokens(masked, block[0] + 1, block[1])
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
    return any(token.value in CONTROL_TRANSFER_TOKENS for token in cpp_tokens(masked, start, end))


def has_intervening_potentially_throwing_call(masked: str, start: int, end: int) -> bool:
    tokens = cpp_tokens(masked, start, end)
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


def resolve_scope(scope_bindings: list[ScopeBinding] | dict[str, list[ScopeBinding]], masked: str,
                  pairs: list[tuple[int, int]],
                  call: re.Match[str], shadow_index=None) -> ScopeBinding | None:
    receiver = receiver_binding_name(masked, call.start())
    available = (scope_bindings.get(receiver, ())
                 if isinstance(scope_bindings, dict) and receiver is not None
                 else scope_bindings if not isinstance(scope_bindings, dict) else ())
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
        capture_tokens = cpp_tokens(masked, capture_open + 1, capture_close)
        for index, token in enumerate(capture_tokens[:-1]):
            if token.value == name and capture_tokens[index + 1].value == "=":
                return True
    return False


def build_shadow_index(
    masked: str, pairs: list[tuple[int, int]]
) -> dict[str, tuple[tuple[int, tuple[int, int] | None], ...]]:
    tokens = cpp_tokens(masked)
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
    tokens = cpp_tokens(masked, declaration_start)
    known_types = declared_type_names(cpp_tokens(masked))
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
    tokens = cpp_tokens(masked, binding.position, binding.block[1])
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
                          compiler_macros: Mapping[str, MacroDefinition] | frozenset[str]
                          = frozenset(), *, compiler_view: bool = False,
                          compiler_translation_text: TranslationText | None = None
                          ) -> list[Finding]:
    translated = (compiler_translation_text if compiler_translation_text is not None
                  else compiler_translation(source) if compiler_view
                  else translate_source(source))
    masked = translated.masked
    pairs = brace_pairs(masked)
    findings: list[Finding] = []
    if not compiler_view:
        findings.extend(preprocessor_capability_findings(path, translated))
        findings.extend(guarded_macro_composition_findings(
            path, translated, compiler_macros))
        findings.extend(phase_two_capability_findings(path, source, translated))
    calls = list(MEMBER_CALL.finditer(masked))
    scope_bindings = scope_bindings_linear(masked, pairs)
    scopes_by_name: dict[str, list[ScopeBinding]] = {}
    for binding in scope_bindings:
        scopes_by_name.setdefault(binding.declaration.name, []).append(binding)
    shadow_index = build_shadow_index(masked, pairs)
    bound_scope_positions = {
        binding.declaration.position for binding in scope_bindings
    }
    for declaration in scope_declarations(masked):
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

    def audit_withread_callback(call: re.Match[str]) -> None:
        callback_block = withread_callback_block(masked, pairs, call)
        if callback_block is None:
            findings.append(Finding(
                path, translated.line_at(call.start()), "GpuSyncReadScope::withRead()",
                "withRead() requires an inline callback body so completion cannot be "
                "bypassed by hidden control flow"))
            return
        nonlocal_jump = next((
            token for token in cpp_tokens(masked, callback_block[0] + 1,
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
            receiver_name = receiver_binding_name(masked, call.start())
            receiver_names = receiver_binding_references(masked, call.start())
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
            canonical = (canonical_scope_declaration(masked, pairs, scope)
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
            canonical_declaration = canonical_scope_declaration(masked, pairs, scope)
            canonical_withread = (
                canonical_declaration and immediate_block(pairs, call.start()) == scope.block)
            if canonical_withread or not canonical_declaration:
                handle_bindings.extend(callback_lease_bindings(masked, pairs, call))
            if canonical_withread:
                acquisitions_by_scope.setdefault(scope_key, []).append(call)
        else:
            if canonical_scope_declaration(masked, pairs, scope):
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

    all_tokens = cpp_tokens(masked)
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
    pair = next((candidate for candidate in brace_pairs(masked) if candidate[0] == opening), None)
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
    buffer: AuditBuffer, translated: TranslationText
) -> None:
    """Resolve GPU grammar first, then validate only its required token ranges."""

    masked = translated.masked
    pairs = brace_pairs(masked)
    calls = list(MEMBER_CALL.finditer(masked))
    scopes = scope_bindings_linear(masked, pairs)
    scopes_by_name: dict[str, list[ScopeBinding]] = {}
    for scope in scopes:
        scopes_by_name.setdefault(scope.declaration.name, []).append(scope)
    shadow_index = build_shadow_index(masked, pairs)
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
    for token in cpp_tokens(masked):
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


def audit_preprocessed_view(
    view: PreprocessedTranslationUnitView,
    limits: AuditLimits,
    rss_reader: Callable[[], int],
) -> list[Finding]:
    """Apply the established capability grammar to one authoritative full-TU view."""

    buffer = AuditBuffer.from_preprocessed(view, limits, rss_reader)
    if not buffer.has_capability_spelling():
        return []
    candidate_paths = buffer.candidate_paths()
    translated = TranslationText(
        buffer.text,
        buffer.text,
        buffer.text,
        (),
        (),
        buffer._line_starts,
    )
    buffer.reserve_rss(rss_reader, limits.rss_bytes, len(buffer.text) * 48)
    _validate_capability_expression_provenance(buffer, translated)
    buffer.reserve_rss(rss_reader, limits.rss_bytes, 0)
    findings: list[Finding] = []
    grouped_paths: dict[tuple[object, ...], list[PurePosixPath]] = {}
    for path in candidate_paths:
        grouped_paths.setdefault(_capability_policy_key(path), []).append(path)
    for paths in grouped_paths.values():
        representative = paths[0]
        allowed = frozenset(paths)
        buffer.reserve_rss(rss_reader, limits.rss_bytes, len(buffer.text) * 48)
        candidate_findings = audit_capability_uses(
            representative,
            buffer.text,
            compiler_view=True,
            compiler_translation_text=translated,
        )
        buffer.reserve_rss(rss_reader, limits.rss_bytes, 0)
        findings.extend(_map_candidate_findings(
            buffer,
            allowed,
            candidate_findings,
        ))

    public_policies = (
        (
            REGISTRY_HEADER,
            "GpuRetireRegistry",
            r"\bregisterRetire\s*\(",
            "GpuRetireRegistry::registerRetire()",
        ),
        (
            OP_SCOPE_HEADER,
            "GpuOpScope",
            r"\btrack\s*\(",
            "GpuOpScope::track()",
        ),
    )
    candidate_path_set = set(candidate_paths)
    for path, class_name, member_pattern, expression in public_policies:
        if path not in candidate_path_set:
            continue
        if not buffer.path_has_spelling(path, class_name.encode("ascii")):
            continue

        def candidate_line(
            line: int,
            *,
            expected: PurePosixPath = path,
        ) -> bool:
            location = buffer.location_for_line(line)
            identity = location.identity
            return bool(
                identity is not None
                and identity.production
                and identity.relative == expected
            )

        buffer.reserve_rss(rss_reader, limits.rss_bytes, len(buffer.text) * 16)
        public_findings = audit_public_member(
            path,
            buffer.text,
            class_name,
            member_pattern,
            expression,
            candidate_line=candidate_line,
            pretokenized=True,
        )
        buffer.reserve_rss(rss_reader, limits.rss_bytes, 0)
        findings.extend(_map_candidate_findings(
            buffer,
            frozenset((path,)),
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


def audit_sources(sources: dict[PurePosixPath, str],
                  compiler_macros: dict[PurePosixPath, dict[str, MacroDefinition]] | None = None) \
        -> list[Finding]:
    findings: list[Finding] = []
    for path, source in sources.items():
        if not is_production_path(path):
            continue
        path_macros = (compiler_macros.get(path, frozenset())
                       if compiler_macros is not None else frozenset())
        findings.extend(audit_capability_uses(path, source, path_macros))
        if compiler_macros is not None:
            findings.extend(compiler_macro_findings(
                path, source, compiler_macros.get(path, frozenset())))
    if REGISTRY_HEADER in sources:
        findings.extend(audit_public_member(REGISTRY_HEADER, sources[REGISTRY_HEADER],
                                            "GpuRetireRegistry", r"\bregisterRetire\s*\(",
                                            "GpuRetireRegistry::registerRetire()"))
    if OP_SCOPE_HEADER in sources:
        findings.extend(audit_public_member(OP_SCOPE_HEADER, sources[OP_SCOPE_HEADER],
                                            "GpuOpScope", r"\btrack\s*\(",
                                            "GpuOpScope::track()"))
    return findings


def compiler_macro_findings(path: PurePosixPath, source: str,
                            macro_names: Mapping[str, MacroDefinition] | frozenset[str]
                            ) -> list[Finding]:
    reviewed: dict[str, str] = {}
    reviewed.update({name: "approved native-handle consumer"
                     for name in REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())})
    reviewed.update({name: "approved native-handle method"
                     for name in REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())})
    reviewed.update({name: "approved native-handle type"
                     for name in REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset())})
    findings: list[Finding] = []
    for name, expression in reviewed.items():
        if name not in macro_names or not re.search(rf"\b{re.escape(name)}\b", source):
            continue
        findings.append(Finding(
            path, line_number(source, re.search(rf"\b{re.escape(name)}\b", source).start()),
            f"{expression} cannot be hidden or shadowed",
            "the production compiler reports the reviewed spelling as a macro"))
    return findings


def reviewed_macro_names(path: PurePosixPath) -> frozenset[str]:
    names = set(REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset()))
    names.update(REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset()))
    names.update(REVIEWED_NATIVE_HANDLE_TYPES.get(path, frozenset()))
    return frozenset(names)


def compile_entry_file(entry: dict[str, object], database: Path) -> Path:
    directory = Path(str(entry["directory"]))
    if not directory.is_absolute():
        directory = database.parent / directory
    source = Path(str(entry["file"]))
    return (source if source.is_absolute() else directory / source).resolve()


def compile_entry_arguments(entry: dict[str, object]) -> list[str]:
    structured = entry.get("arguments")
    if isinstance(structured, list):
        return [str(argument) for argument in structured]
    command = entry.get("command")
    if not isinstance(command, str):
        raise RuntimeError("compile command has neither arguments nor command")
    if os.name != "nt":
        return shlex.split(command, posix=True)
    argc = ctypes.c_int()
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    shell32.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p
    argv = shell32.CommandLineToArgvW(command, ctypes.byref(argc))
    if not argv:
        raise OSError(ctypes.get_last_error(), "CommandLineToArgvW failed")
    try:
        return [argv[index] for index in range(argc.value)]
    finally:
        kernel32.LocalFree(argv)


def compiler_index_after_launchers(arguments: list[str]) -> int:
    """Locate the compiler while rejecting ambiguous launcher option syntax."""
    launchers = {"ccache", "distcc", "icecc", "sccache"}
    ccache_value_options = {
        "--compiler", "--compiler-check", "--compiler-type", "--config-path", "--dir",
        "--namespace", "--set-config", "--trim-dir", "-o",
    }
    ccache_flag_options = {"--ccache-skip"}
    index = 0
    while index < len(arguments) - 1:
        launcher = Path(arguments[index]).name.lower().removesuffix(".exe")
        if launcher not in launchers:
            break
        index += 1
        if launcher != "ccache":
            continue
        while index < len(arguments):
            option = arguments[index]
            if option == "--":
                index += 1
                break
            option_name = option.partition("=")[0]
            if option_name in ccache_value_options:
                if "=" in option:
                    index += 1
                else:
                    if index + 1 >= len(arguments):
                        raise RuntimeError(f"ccache option requires a value: {option}")
                    index += 2
                continue
            if option in ccache_flag_options:
                index += 1
                continue
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*=.*", option):
                index += 1
                continue
            if option.startswith("-"):
                raise RuntimeError(f"unsupported ccache launcher option: {option}")
            break
    if index >= len(arguments):
        raise RuntimeError("compile command has launchers but no compiler")
    return index


def compiler_probe_command(arguments: list[str]) -> list[str]:
    if not arguments:
        raise RuntimeError("compile command has no executable")
    compiler_index = compiler_index_after_launchers(arguments)

    compiler = Path(arguments[compiler_index]).name.lower().removesuffix(".exe")
    probe_flags = ["/nologo", "/EP", "/d1PP"] if compiler == "cl" else ["-dM", "-E"]
    filtered: list[str] = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        lowered = value.lower()
        if index < compiler_index:
            filtered.append(value)
            index += 1
            continue
        if value in {"-c", "-MD", "-MMD"} or lowered == "/c":
            index += 1
            continue
        if value in {"-o", "-MF", "-MT", "-MQ"}:
            index += 2
            continue
        if (value.startswith(("-MF", "-MT", "-MQ"))
                or lowered.startswith(("/fo", "/fd"))):
            index += 1
            continue
        filtered.append(value)
        index += 1
    insertion = compiler_index + 1
    return [*filtered[:insertion], *probe_flags, *filtered[insertion:]]


COMPILER_MACRO_SENTINELS = frozenset({"__cplusplus", "__GNUC__", "__clang__", "_MSC_VER"})


def compiler_macro_definitions(output: str) -> dict[str, MacroDefinition]:
    events, _directives = source_macro_events(output)
    definitions: dict[str, MacroDefinition] = {}
    for _position, name, definition in events:
        if definition is None:
            definitions.pop(name, None)
        else:
            definitions[name] = definition
    return definitions


def source_requires_compile_entry(path: PurePosixPath, all_entry_paths: set[PurePosixPath]) -> bool:
    if path.suffix.lower() in {".h", ".hh", ".hpp"}:
        return False
    apple_active = any(candidate.suffix.lower() == ".mm" for candidate in all_entry_paths)
    windows_active = any(
        "win" in candidate.parts
        or candidate.name.endswith(("_mediafoundation.cpp", "_win.cpp"))
        for candidate in all_entry_paths)
    if path.suffix.lower() == ".mm":
        return apple_active
    if ("win" in path.parts
            or path.name.endswith(("_mediafoundation.cpp", "_win.cpp"))):
        return windows_active
    return True


def load_compiler_macro_tables(root: Path, compile_commands: Path | None,
                               sources: dict[PurePosixPath, str]) \
        -> dict[PurePosixPath, dict[str, MacroDefinition]]:
    if compile_commands is None:
        return {}
    if not compile_commands.is_file():
        raise RuntimeError(f"compile database does not exist: {compile_commands}")
    entries = json.loads(compile_commands.read_text(encoding="utf-8-sig"))
    if not isinstance(entries, list):
        raise RuntimeError("compile database root must be an array")
    source_by_absolute = {
        str((root / path).resolve()).replace("\\", "/").casefold(): path
        for path in sources
    }
    resolved_entries: list[tuple[dict[str, object], PurePosixPath | None]] = []
    all_entry_paths: set[PurePosixPath] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError("compile database entry must be an object")
        entry_file = compile_entry_file(entry, compile_commands)
        absolute = str(entry_file).replace("\\", "/").casefold()
        path = source_by_absolute.get(absolute)
        try:
            all_entry_paths.add(PurePosixPath(entry_file.relative_to(root).as_posix()))
        except ValueError:
            pass
        resolved_entries.append((entry, path))

    relevant = {
        path for path, source in sources.items()
        if reviewed_macro_names(path).intersection(re.findall(r"[A-Za-z_]\w*", source))
    }
    available = {path for _entry, path in resolved_entries if path in relevant}
    missing = sorted(
        (path for path in relevant
         if source_requires_compile_entry(path, all_entry_paths) and path not in available),
        key=str)
    if missing:
        raise RuntimeError(
            "compile database has no relevant entry for: "
            + ", ".join(str(path) for path in missing))

    tables: dict[PurePosixPath, dict[str, MacroDefinition]] = {}
    failures: list[str] = []
    for entry, path in resolved_entries:
        if path is None:
            continue
        if path not in relevant:
            continue
        arguments = compile_entry_arguments(entry)
        command = compiler_probe_command(arguments)
        directory = Path(str(entry["directory"]))
        if not directory.is_absolute():
            directory = compile_commands.parent / directory
        completed = subprocess.run(
            command, cwd=directory, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=20, check=False)
        if completed.returncode != 0:
            failures.append(
                f"{path} ({completed.returncode}): "
                + completed.stderr.strip()[-400:])
            continue
        definitions = compiler_macro_definitions(completed.stdout)
        if not definitions or not COMPILER_MACRO_SENTINELS.intersection(definitions):
            failures.append(f"{path}: compiler macro dump is empty or lacks a sentinel")
            continue
        table = tables.setdefault(path, {})
        for name, definition in definitions.items():
            previous = table.get(name)
            if previous is not None and previous != definition:
                failures.append(
                    f"{path}: compiler macro {name} differs between configurations")
                continue
            table[name] = definition
    if failures:
        raise RuntimeError("compiler macro probe failed: " + "; ".join(failures))
    return tables


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
        rendered = "\n".join(finding.render() for finding in audit_capability_uses(path, source))
        if expected not in rendered:
            raise AssertionError(
                f"source-audit mutation survived ({expected}):\nsource: {source}\n{rendered}")

    compiler_spoof_path = PurePosixPath("playback/output/win/wingpuimportedge.cpp")
    compiler_spoof_source = (
        "auto* texture = static_cast<ID3D11Texture2D*>(lease.nativeHandle());")
    compiler_spoof = compiler_macro_findings(
        compiler_spoof_path, compiler_spoof_source, frozenset({"ID3D11Texture2D"}))
    if not compiler_spoof:
        raise AssertionError("compiler-reported native-handle type macro survived")
    if compiler_macro_findings(compiler_spoof_path, compiler_spoof_source, frozenset()):
        raise AssertionError("absent compiler macro produced a spoof finding")

    with tempfile.TemporaryDirectory(prefix="gpu audit ") as temporary:
        fixture_root = Path(temporary) / "compile db fixture"
        fixture_source = fixture_root / compiler_spoof_path
        fixture_source.parent.mkdir(parents=True)
        fixture_source.write_text(compiler_spoof_source, encoding="utf-8")
        fake_compiler = fixture_root / "fake compiler.py"
        fake_compiler.write_text(
            "import sys\n"
            "if '--fail' in sys.argv: raise SystemExit(9)\n"
            "print('#define __cplusplus 202002L')\n"
            "if '-DCONFIG_A' in sys.argv: print('#define ID3D11Texture2D SpoofA')\n"
            "if '-DCONFIG_B' in sys.argv: print('#define GetDevice SpoofB')\n",
            encoding="utf-8")
        assignment_wrapper = fixture_root / "compiler_check=content"
        assignment_wrapper.write_text(
            "import runpy, sys\n"
            "script = sys.argv[1]\n"
            "sys.argv = sys.argv[1:]\n"
            "runpy.run_path(script, run_name='__main__')\n",
            encoding="utf-8")
        launcher = fixture_root / ("ccache.exe" if os.name == "nt" else "ccache")
        shutil.copy2(sys.executable, launcher)
        if os.name == "nt":
            for dll in Path(sys.executable).parent.glob("python*.dll"):
                shutil.copy2(dll, fixture_root / dll.name)
        relative_file = compiler_spoof_path.as_posix()
        first_arguments = [str(launcher), str(fake_compiler), "-DCONFIG_A", "-c",
                           relative_file, "-o", "first.o"]
        second_arguments = [str(launcher), str(fake_compiler), "-DCONFIG_B", "-c",
                            relative_file, "-o", "second.o"]
        assignment_arguments = [
            str(launcher), assignment_wrapper.name, str(fake_compiler), "-DCONFIG_A",
            "-c", relative_file, "-o", "assignment.o"]
        quoted_command = (subprocess.list2cmdline(second_arguments) if os.name == "nt"
                          else shlex.join(second_arguments))
        compile_database = fixture_root / "compile_commands.json"
        compile_database.write_text(json.dumps([
            {"directory": str(fixture_root), "file": relative_file,
             "arguments": first_arguments},
            {"directory": str(fixture_root), "file": relative_file,
             "command": quoted_command},
            {"directory": str(fixture_root), "file": relative_file,
             "arguments": assignment_arguments},
        ]), encoding="utf-8")
        loaded = load_compiler_macro_tables(
            fixture_root, compile_database, {compiler_spoof_path: compiler_spoof_source})
        if set(loaded.get(compiler_spoof_path, {})) != {
                "ID3D11Texture2D", "GetDevice", "__cplusplus"}:
            raise AssertionError(
                "compiler loader did not resolve ccache, quoted paths, relative files, "
                f"and duplicate configurations: {loaded}")

        failed_database = fixture_root / "failed_compile_commands.json"
        failed_database.write_text(json.dumps([{
            "directory": str(fixture_root), "file": relative_file,
            "arguments": [str(launcher), str(fake_compiler), "--fail", "-c",
                          relative_file],
        }]), encoding="utf-8")
        try:
            load_compiler_macro_tables(
                fixture_root, failed_database, {compiler_spoof_path: compiler_spoof_source})
        except RuntimeError:
            pass
        else:
            raise AssertionError("failed relevant compiler probe did not fail closed")

        empty_compiler = fixture_root / "empty compiler.py"
        empty_compiler.write_text("raise SystemExit(0)\n", encoding="utf-8")
        empty_database = fixture_root / "empty_compile_commands.json"
        empty_database.write_text(json.dumps([{
            "directory": str(fixture_root), "file": relative_file,
            "arguments": [str(launcher), str(empty_compiler), "-c", relative_file],
        }]), encoding="utf-8")
        try:
            load_compiler_macro_tables(
                fixture_root, empty_database, {compiler_spoof_path: compiler_spoof_source})
        except RuntimeError:
            pass
        else:
            raise AssertionError("empty successful compiler macro probe did not fail closed")

        missing_database = fixture_root / "missing_compile_commands.json"
        missing_database.write_text(json.dumps([{
            "directory": str(fixture_root),
            "file": "playback/output/win/other.cpp",
            "arguments": [str(launcher), str(fake_compiler), "-c",
                          "playback/output/win/other.cpp"],
        }]), encoding="utf-8")
        try:
            load_compiler_macro_tables(
                fixture_root, missing_database, {compiler_spoof_path: compiler_spoof_source})
        except RuntimeError:
            pass
        else:
            raise AssertionError("missing relevant compile command did not fail closed")

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

    compiler_safe_join = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void safe() { JOIN(native, Handle); }",
        {"JOIN": MacroDefinition(
            True, ("consume", "(", "a", ",", "b", ")"), ("a", "b"))})
    if compiler_safe_join:
        raise AssertionError("compiler-reported non-paste macro was treated as paste:\n" +
                             "\n".join(finding.render() for finding in compiler_safe_join))

    compiler_wrapped_paste = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.PASTE(LEFT, RIGHT)(); }",
        {
            "PASTE": MacroDefinition(
                True, ("PASTE_I", "(", "left", ",", "right", ")"),
                ("left", "right")),
            "PASTE_I": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
            "LEFT": MacroDefinition(False, ("native",)),
            "RIGHT": MacroDefinition(False, ("Handle",)),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_wrapped_paste):
        raise AssertionError("compiler-reported two-stage paste wrapper survived")

    compiler_variadic_paste = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.PASTE(native, Handle)(); }",
        {
            "PASTE": MacroDefinition(
                True, ("PASTE_I", "(", "__VA_ARGS__", ")"), (),
                "__VA_ARGS__"),
            "PASTE_I": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_variadic_paste):
        raise AssertionError("compiler variadic paste forwarding survived")

    compiler_prescanned_paste = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.PASTE(ID(native), Handle)(); }",
        {
            "PASTE": MacroDefinition(
                True, ("PASTE_I", "(", "left", ",", "right", ")"),
                ("left", "right")),
            "PASTE_I": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
            "ID": MacroDefinition(True, ("value",), ("value",)),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_prescanned_paste):
        raise AssertionError("compiler argument prescan paste survived")

    compiler_zero_arg = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.TOKEN()(); }",
        {"TOKEN": MacroDefinition(True, ("nativeHandle",), ())})
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_zero_arg):
        raise AssertionError("compiler zero-argument guarded macro survived")

    compiler_callable_alias = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.ALIAS(native, Handle)(); }",
        {
            "ALIAS": MacroDefinition(False, ("PASTE",)),
            "PASTE": MacroDefinition(True, ("left", "##", "right"),
                                     ("left", "right")),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_callable_alias):
        raise AssertionError("compiler callable object alias survived rescan")

    compiler_postfix_callable_alias = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { "
        "lease.WRAP()(native, Handle)(); }",
        {
            "WRAP": MacroDefinition(True, ("PASTE",), ()),
            "PASTE": MacroDefinition(True, ("left", "##", "right"),
                                     ("left", "right")),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_postfix_callable_alias):
        raise AssertionError("compiler postfix callable alias survived rescan")

    compiler_token_origin_alias = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { "
        "lease.F(call)(F(native), Handle)(); }",
        {
            "F": MacroDefinition(True, ("F_I", "(", "value", ")"), ("value",)),
            "F_I": MacroDefinition(True, ("F_", "##", "value"), ("value",)),
            "F_call": MacroDefinition(False, ("PASTE",)),
            "F_native": MacroDefinition(False, ("native",)),
            "PASTE": MacroDefinition(
                True, ("PASTE_I", "(", "left", ",", "right", ")"),
                ("left", "right")),
            "PASTE_I": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_token_origin_alias):
        raise AssertionError("original postfix macro was hidden by replacement state")

    compiler_object_tail_alias = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { "
        "lease.OPEN native, Handle)(); }",
        {
            "OPEN": MacroDefinition(False, ("PASTE", "(")),
            "PASTE": MacroDefinition(
                True, ("PASTE_I", "(", "left", ",", "right", ")"),
                ("left", "right")),
            "PASTE_I": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
        })
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_object_tail_alias):
        raise AssertionError("compiler object macro did not consume its original tail")

    compiler_safe_object_tail = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void safe(const GpuReadLease& lease) { "
        "lease.OPEN other, Handle)(); }",
        {
            "OPEN": MacroDefinition(False, ("PASTE", "(")),
            "PASTE": MacroDefinition(
                True, ("PASTE_I", "(", "left", ",", "right", ")"),
                ("left", "right")),
            "PASTE_I": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
        })
    if compiler_safe_object_tail:
        raise AssertionError("safe compiler object-tail macro was rejected:\n" +
                             "\n".join(
                                 finding.render() for finding in compiler_safe_object_tail))

    compiler_malformed_replacement = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void malformed(const GpuReadLease& lease) { lease.OPEN(); }",
        {
            "OPEN": MacroDefinition(True, ("PASTE", "("), ()),
            "PASTE": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
        })
    if not any("macro expansion syntax" in finding.reason
               for finding in compiler_malformed_replacement):
        raise AssertionError("malformed compiler replacement did not fail closed structurally")

    compiler_wide_replacement = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bounded(const GpuReadLease& lease) { lease.WIDE(n)(); }",
        {
            "WIDE": MacroDefinition(
                True, tuple(
                    token
                    for index in range(4096)
                    for token in (("value", "##") if index < 4095 else ("value",))),
                ("value",)),
        })
    if not any("macro expansion complexity" in finding.reason
               for finding in compiler_wide_replacement):
        raise AssertionError("wide compiler replacement did not fail closed deliberately")

    compiler_guarded_object_alias = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.GUARD(); }",
        {"GUARD": MacroDefinition(False, ("nativeHandle",))})
    if not any("guarded identifier macro composition" in finding.expression
               for finding in compiler_guarded_object_alias):
        raise AssertionError("compiler guarded object alias survived rescan")

    compiler_raw_paste = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void safe(const GpuReadLease& lease) { lease.CAT(LEFT, RIGHT)(); }",
        {
            "CAT": MacroDefinition(
                True, ("left", "##", "right"), ("left", "right")),
            "LEFT": MacroDefinition(False, ("native",)),
            "RIGHT": MacroDefinition(False, ("Handle",)),
        })
    if compiler_raw_paste:
        raise AssertionError("raw paste operands were incorrectly prescanned:\n" +
                             "\n".join(finding.render() for finding in compiler_raw_paste))

    deep_macros: dict[str, MacroDefinition] = {
        f"WRAP{index}": MacroDefinition(
            True,
            ((f"WRAP{index + 1}", "(", "value", ")")
             if index < 799 else ("value",)),
            ("value",))
        for index in range(800)
    }
    deep_macro_findings = audit_capability_uses(
        PurePosixPath("playback/gpu/gpufence.h"),
        "void bad(const GpuReadLease& lease) { lease.WRAP0(nativeHandle)(); }",
        deep_macros)
    if not any("macro expansion depth" in finding.reason
               for finding in deep_macro_findings):
        raise AssertionError("deep macro expansion did not fail closed deliberately")

    recursive_macro_controls = (
        audit_capability_uses(
            PurePosixPath("playback/gpu/gpufence.h"), "void safe() { F(); }",
            {"F": MacroDefinition(True, ("F", "(", ")"), ())})
        + audit_capability_uses(
            PurePosixPath("playback/gpu/gpufence.h"), "void safe() { F(); }",
            {
                "F": MacroDefinition(True, ("G", "(", ")"), ()),
                "G": MacroDefinition(True, ("F", "(", ")"), ()),
            }))
    if recursive_macro_controls:
        raise AssertionError("benign recursive macro control was rejected:\n" +
                             "\n".join(
                                 finding.render() for finding in recursive_macro_controls))

    windows_command = (
        '"C:\\Program Files\\ccache\\ccache.exe" --config-path "ccache config.conf" '
        '"C:\\Program Files\\LLVM\\bin\\clang++.exe" '
        '-I"C:\\SDK Path\\include" -c playback\\gpu\\file.cpp -o file.obj')
    if os.name == "nt":
        windows_arguments = compile_entry_arguments({"command": windows_command})
        if windows_arguments != [
                r"C:\Program Files\ccache\ccache.exe", "--config-path", "ccache config.conf",
                r"C:\Program Files\LLVM\bin\clang++.exe", r"-IC:\SDK Path\include",
                "-c", r"playback\gpu\file.cpp", "-o", "file.obj"]:
            raise AssertionError(
                f"Windows compile command was split incorrectly: {windows_arguments}")
        windows_probe = compiler_probe_command(windows_arguments)
        if windows_probe[3:6] != [
                r"C:\Program Files\LLVM\bin\clang++.exe", "-dM", "-E"]:
            raise AssertionError(
                f"Windows parsing/launcher pipeline misplaced probe flags: {windows_probe}")
    optioned_probe = compiler_probe_command([
        "ccache", "--config-path", "ccache.conf", "g++", "-c", "file.cpp", "-o", "file.o"])
    if optioned_probe != [
            "ccache", "--config-path", "ccache.conf", "g++", "-dM", "-E", "file.cpp"]:
        raise AssertionError(f"ccache options hid the compiler probe insertion: {optioned_probe}")
    assignment_probe = compiler_probe_command([
        "ccache", "compiler_check=content", "g++", "-c", "file.cpp"])
    if assignment_probe != [
            "ccache", "compiler_check=content", "g++", "-dM", "-E", "file.cpp"]:
        raise AssertionError(
            f"ccache assignment hid the compiler probe insertion: {assignment_probe}")

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
    return "void nested() { " + ("M()(" * count) + "value" + (")" * count) + "; }"


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
    postfix_macros = {"M": MacroDefinition(True, ("safe",), ())}
    postfix_samples: dict[int, list[float]] = {count: [] for count in counts}
    for order in (counts, tuple(reversed(counts)), (8192, 16384, 4096)):
        for count in order:
            started = time.perf_counter()
            findings = guarded_macro_composition_findings(
                path, postfix_sources[count], postfix_macros)
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

    wide_source = translate_source(
        "void bounded(const GpuReadLease& lease) { lease.WIDE(n)(); }")
    wide_macros = {
        count: {
            "WIDE": MacroDefinition(
                True, tuple(
                    token
                    for index in range(count)
                    for token in (("value", "##") if index < count - 1 else ("value",))),
                ("value",)),
        }
        for count in counts
    }
    wide_samples: dict[int, list[float]] = {count: [] for count in counts}
    for order in (counts, tuple(reversed(counts)), (8192, 16384, 4096)):
        for count in order:
            started = time.perf_counter()
            findings = guarded_macro_composition_findings(
                path, wide_source, wide_macros[count])
            wide_samples[count].append(time.perf_counter() - started)
            if not any("macro expansion complexity" in finding.reason
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
    return (
        ", ".join(f"{count}={scores[count]:.4f}s" for count in counts)
        + f", ratios={adjacent[0]:.3f}/{adjacent[1]:.3f}/{aggregate:.3f}; "
        + "postfix "
        + ", ".join(f"{count}={postfix_scores[count]:.4f}s" for count in counts)
        + f", ratios={postfix_adjacent[0]:.3f}/"
        f"{postfix_adjacent[1]:.3f}/{postfix_aggregate:.3f}; wide "
        + ", ".join(f"{count}={wide_scores[count]:.4f}s" for count in counts)
        + f", ratios={wide_adjacent[0]:.3f}/"
        f"{wide_adjacent[1]:.3f}/{wide_aggregate:.3f}")


def load_production_sources(root: Path) -> dict[PurePosixPath, str]:
    sources: dict[PurePosixPath, str] = {}
    for directory in PRODUCTION_ROOTS:
        for path in (root / directory).rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                relative = PurePosixPath(path.relative_to(root).as_posix())
                sources[relative] = path.read_text(encoding="utf-8")
    return sources


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--performance-only", action="store_true")
    args = parser.parse_args()

    if args.performance_only:
        print("PASS: GPU capability source-audit performance: " + performance_self_tests())
        return 0
    mutation_self_tests()
    root = args.source_root.resolve()
    sources = load_production_sources(root)
    compiler_macros = load_compiler_macro_tables(root, args.compile_commands, sources)
    findings = audit_sources(sources, compiler_macros)
    if findings:
        for finding in sorted(findings, key=lambda item: (str(item.path), item.line, item.expression)):
            print(finding.render())
        return 1
    print("PASS: GPU capability source audit and mutation self-tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
