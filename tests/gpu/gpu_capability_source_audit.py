#!/usr/bin/env python3
"""Audit production GPU capability use without widening the public API."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import statistics
import time
from typing import Iterable


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


@dataclass(frozen=True)
class Finding:
    path: PurePosixPath
    line: int
    expression: str
    reason: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: forbidden expression {self.expression}: {self.reason}"


@dataclass(frozen=True)
class CppToken:
    value: str
    start: int
    end: int


TOKEN_PATTERN = re.compile(
    r"[A-Za-z_]\w*|\d+(?:\.\d+)?|::|->|&&|\|\||==|!=|<=|>=|\+\+|--|"
    r"<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|\.\.\.|[^\s]"
)


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
            raw = (re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', source[index:])
                   if current in {"L", "R", "U", "u"} else None)
            if raw is not None:
                terminator = ")" + raw.group(1) + '"'
                closing = source.find(terminator, index + raw.end())
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


def preprocessor_capability_findings(path: PurePosixPath, source: str) -> list[Finding]:
    findings: list[Finding] = []
    unique_capability = re.compile(r"\b(?:GpuSyncReadScope|withRead|nativeHandle)\b")
    nonlocal_jump = re.compile(r"\b(?:_longjmp|longjmp|siglongjmp)\b")
    approved_consumers = REVIEWED_NATIVE_HANDLE_SINKS.get(path, frozenset())
    approved_methods = REVIEWED_NATIVE_HANDLE_METHODS.get(path, frozenset())

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
        if "##" in stripped or "%:%:" in stripped:
            findings.append(Finding(
                path, logical_start, "preprocessor token concatenation",
                "token concatenation can reconstruct guarded GPU operations or non-local "
                "control flow"))
        if any(re.search(rf"\b{re.escape(name)}\b", stripped)
               for name in approved_consumers):
            findings.append(Finding(
                path, logical_start, "approved native-handle consumer cannot be hidden or shadowed",
                "approved native-handle consumers cannot be replaced by preprocessing"))
        if any(re.search(rf"\b{re.escape(name)}\b", stripped)
               for name in approved_methods):
            findings.append(Finding(
                path, logical_start, "approved native-handle method cannot be hidden or shadowed",
                "approved native-handle methods cannot be replaced by preprocessing"))
        if nonlocal_jump.search(stripped):
            findings.append(Finding(
                path, logical_start, "non-local jump preprocessor alias",
                "non-local jump cannot be hidden by preprocessing"))
        elif unique_capability.search(stripped) or pasted_guarded_identifier(stripped):
            findings.append(Finding(
                path, logical_start, "GPU capability preprocessor alias",
                "nativeHandle alias and scope operations cannot be hidden by preprocessing"))

    directive_source = mask_non_code(source)
    logical = ""
    logical_start = 1
    for line, text in enumerate(directive_source.splitlines(keepends=True), 1):
        if not logical:
            logical_start = line
        splice = re.search(r"\\\r?\n$", text)
        logical += text[:splice.start()] if splice else text
        if splice:
            continue
        audit_logical(logical, logical_start)
        logical = ""
    if logical:
        audit_logical(logical, logical_start)
    return findings


def guarded_macro_composition_findings(path: PurePosixPath, source: str,
                                       masked: str) -> list[Finding]:
    """Reject source-visible macro calls whose identifier pieces form guarded names."""
    guarded = {
        "GpuSyncReadScope", "_longjmp", "complete", "longjmp", "nativeHandle", "read",
        "siglongjmp", "withRead",
    }
    tokens = cpp_tokens(masked)
    findings: list[Finding] = []
    for index, token in enumerate(tokens[:-1]):
        if (not re.fullmatch(r"[A-Za-z_]\w*", token.value)
                or tokens[index + 1].value != "("):
            continue
        depth = 1
        arguments: list[list[str]] = [[]]
        closing = None
        for cursor in range(index + 2, len(tokens)):
            value = tokens[cursor].value
            if value == "(":
                depth += 1
            elif value == ")":
                depth -= 1
                if depth == 0:
                    closing = cursor
                    break
            if depth == 1 and value == ",":
                arguments.append([])
            elif depth >= 1:
                arguments[-1].append(value)
        if closing is None or len(arguments) < 2:
            continue
        if not all(len(argument) == 1
                   and re.fullmatch(r"[A-Za-z_]\w*", argument[0])
                   for argument in arguments):
            continue
        if "".join(argument[0] for argument in arguments) not in guarded:
            continue
        findings.append(Finding(
            path, line_number(source, token.start), "guarded identifier macro composition",
            "macro arguments cannot be composed into guarded GPU or non-local control names"))
    return findings


def phase_two_capability_findings(path: PurePosixPath, source: str) -> list[Finding]:
    """Normalize splices once, then reject reconstructed guarded identifiers in O(n)."""
    if "\\\n" not in source and "\\\r\n" not in source:
        return []
    guarded = {
        "GpuSyncReadScope", "_longjmp", "complete", "longjmp", "nativeHandle", "read",
        "siglongjmp", "withRead",
    }
    normalized_chars: list[str] = []
    source_lines: list[int] = []
    splice_boundaries: list[int] = []
    index = 0
    current_line = 1
    while index < len(source):
        if source[index] == "\\" and index + 1 < len(source):
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

    normalized = "".join(normalized_chars)
    masked = normalize_alternative_tokens(mask_non_code(normalized))
    pairs = brace_pairs(masked)
    scope_bindings = scope_bindings_linear(masked, pairs)

    def result_receiver_name(call: re.Match[str]) -> str | None:
        expression = receiver_expression(masked, call.start())
        tokens = cpp_tokens(expression)
        start, end = strip_transparent_parentheses(tokens, 0, len(tokens))
        while start < end:
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
            if comma is None:
                break
            start, end = strip_transparent_parentheses(tokens, comma + 1, end)
        if end - start != 1 or not re.fullmatch(r"[A-Za-z_]\w*", tokens[start].value):
            return None
        return tokens[start].value

    calls_by_receiver: dict[str, list[re.Match[str]]] = {}
    for call in MEMBER_CALL.finditer(masked):
        if call.group(1) not in {"read", "complete"}:
            continue
        receiver = result_receiver_name(call)
        if receiver is not None:
            calls_by_receiver.setdefault(receiver, []).append(call)

    gpu_names = {binding.declaration.name for binding in scope_bindings}
    all_tokens = cpp_tokens(masked)
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
        and token_is_declarator_name(all_tokens, index)
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
        source_line = source_lines[token.start] if token.start < len(source_lines) else current_line
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


def approved_call_is_unshadowed(tokens: list[CppToken], name_index: int,
                                allowed_member_calls: frozenset[str],
                                identity_tokens: list[CppToken] | None = None) -> bool:
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
    for index, token in enumerate(context[:context_index]):
        if token.value == name and token_is_declarator_name(context, index):
            return False
        if token.value != name:
            continue
        boundary = index - 1
        while boundary >= 0 and context[boundary].value not in {";", "{", "}"}:
            boundary -= 1
        if "using" in [item.value for item in context[boundary + 1:index + 1]]:
            return False
    return True


def is_direct_call_argument(tokens: list[CppToken], use_start: int, use_end: int,
                            allowed_calls: frozenset[str],
                            allowed_member_calls: frozenset[str] = frozenset(),
                            identity_tokens: list[CppToken] | None = None) -> bool:
    call = enclosing_call(tokens, use_start)
    if call is None or call[0] not in allowed_calls:
        return False
    _, opening, closing = call
    if not approved_call_is_unshadowed(
            tokens, opening - 1, allowed_member_calls, identity_tokens):
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


def type_name_is_source_shadowed(masked: str, type_name: str, position: int) -> bool:
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
            and type_name_is_source_shadowed(masked, "IOSurfaceRef", alias.start)):
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
                masked, cast_type[0], alias.start)):
            approved_cast = False
    if not direct and not approved_cast:
        return None
    return alias.value, alias.start


def native_handle_has_direct_consumer(path: PurePosixPath, masked: str,
                                      call: re.Match[str], binding: HandleBinding) -> bool:
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
        identity_tokens)


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
                identity_tokens):
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
    """Assign positions to their innermost lexical blocks in one source pass."""
    if not positions:
        return {}
    opening_pairs = {opening: (opening, closing) for opening, closing in pairs}
    ordered = sorted(set(positions))
    assigned: dict[int, tuple[int, int] | None] = {}
    stack: list[tuple[int, int]] = []
    position_index = 0
    for index, char in enumerate(masked):
        while position_index < len(ordered) and ordered[position_index] == index:
            assigned[index] = stack[-1] if stack else None
            position_index += 1
        if char == "{" and index in opening_pairs:
            stack.append(opening_pairs[index])
        elif char == "}" and stack and stack[-1][1] == index:
            stack.pop()
    while position_index < len(ordered):
        assigned[ordered[position_index]] = stack[-1] if stack else None
        position_index += 1
    return assigned


def scope_bindings_linear(masked: str,
                          pairs: list[tuple[int, int]]) -> list[ScopeBinding]:
    declarations = scope_declarations(masked)
    assigned = blocks_for_positions(
        masked, pairs, [declaration.position for declaration in declarations])
    opening_pairs = {opening: (opening, closing) for opening, closing in pairs}
    bindings: list[ScopeBinding] = []
    for declaration in declarations:
        lexical_block = assigned.get(declaration.position)
        following = masked[declaration.end:].lstrip()
        if following.startswith((")", ",")):
            signature_end = masked.find(")", declaration.end)
            body_start = masked.find("{", signature_end + 1) if signature_end >= 0 else -1
            declaration_end = masked.find(";", signature_end + 1) if signature_end >= 0 else -1
            if (body_start >= 0 and (declaration_end < 0 or body_start < declaration_end)
                    and body_start in opening_pairs):
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
    expression_chars = frozenset("._:->*&")
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


def resolve_scope(scope_bindings: list[ScopeBinding], masked: str,
                  pairs: list[tuple[int, int]],
                  call: re.Match[str]) -> ScopeBinding | None:
    receiver = receiver_expression(masked, call.start()).strip()
    candidates = [
        binding for binding in scope_bindings
        if binding.declaration.position <= call.start() < binding.block[1]
        and binding.block[0] < call.start()
        and receiver == binding.declaration.name
        and not name_is_shadowed(binding.declaration.name, binding.declaration.position,
                                 binding.declaration.end, masked, pairs, call.start())
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


def token_is_declarator_name(tokens: list[CppToken], index: int) -> bool:
    """Conservatively recognize a local/parameter declarator at tokens[index]."""
    if index == 0 or index + 1 >= len(tokens):
        return False
    following = tokens[index + 1].value
    if following not in {";", "=", "(", ")", "{", "[", ",", ":"}:
        return False
    previous = tokens[index - 1].value
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


def name_is_shadowed(name: str, declaration_start: int, declaration_end: int,
                     masked: str, pairs: list[tuple[int, int]], call_position: int) -> bool:
    if lambda_init_capture_shadows(masked, pairs, name, call_position):
        return True
    tokens = cpp_tokens(masked, declaration_start, call_position)
    for index, token in enumerate(tokens):
        if token.value != name or declaration_start <= token.start <= declaration_end:
            continue
        if not token_is_declarator_name(tokens, index):
            continue
        block = immediate_block(pairs, token.start)
        if block is not None and block[0] < call_position < block[1]:
            return True
    return False


def handle_binding_is_shadowed(binding: HandleBinding, masked: str,
                               pairs: list[tuple[int, int]], call_position: int) -> bool:
    return name_is_shadowed(binding.name, binding.position, binding.position,
                            masked, pairs, call_position)


def lease_binding_stays_local(masked: str, pairs: list[tuple[int, int]],
                              binding: HandleBinding) -> bool:
    """Keep the lease in synchronous blocks while excluding nested callable bodies."""
    allowed_methods = {"desc", "nativeHandle", "nativeSubresource", "valid"}
    tokens = cpp_tokens(masked, binding.position, binding.block[1])
    for index, token in enumerate(tokens):
        if token.value != binding.name or token.start == binding.position:
            continue
        if token_is_declarator_name(tokens, index):
            declaration_tokens = statement_tokens(masked, pairs, token.start)
            if any(item.value == "GpuReadLease" for item in declaration_tokens):
                continue
        if name_is_shadowed(binding.name, binding.position, binding.position,
                            masked, pairs, token.start):
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


def audit_capability_uses(path: PurePosixPath, source: str) -> list[Finding]:
    masked = normalize_alternative_tokens(mask_non_code(source))
    pairs = brace_pairs(masked)
    findings = preprocessor_capability_findings(path, source)
    findings.extend(guarded_macro_composition_findings(path, source, masked))
    findings.extend(phase_two_capability_findings(path, source))
    calls = list(MEMBER_CALL.finditer(masked))
    scope_bindings = scope_bindings_linear(masked, pairs)
    bound_scope_positions = {
        binding.declaration.position for binding in scope_bindings
    }
    for declaration in scope_declarations(masked):
        if declaration.position not in bound_scope_positions:
            findings.append(
                Finding(path, line_number(source, declaration.position), "GpuSyncReadScope",
                        "scope declaration is not inside a lexical block")
            )

    reads_by_scope: dict[int, list[re.Match[str]]] = {}
    acquisitions_by_scope: dict[int, list[re.Match[str]]] = {}
    completes_by_scope: dict[int, list[re.Match[str]]] = {}
    handle_bindings: list[HandleBinding] = []
    claimed_reads: set[int] = set()
    for call in calls:
        if call.group(1) not in {"read", "withRead", "complete"}:
            continue
        scope = resolve_scope(scope_bindings, masked, pairs, call)
        if scope is None:
            if call.group(1) == "read":
                receiver = receiver_expression(masked, call.start())
                referenced = next((
                    binding for binding in scope_bindings
                    if binding.declaration.position < call.start() < binding.block[1]
                    and re.search(rf"\b{re.escape(binding.declaration.name)}\b", receiver)
                    and not name_is_shadowed(binding.declaration.name,
                                             binding.declaration.position,
                                             binding.declaration.end, masked, pairs,
                                             call.start())
                ), None)
                if referenced is not None:
                    claimed_reads.add(call.start())
                    if path not in SYNC_READ_ALLOWLIST:
                        findings.append(
                            Finding(path, line_number(source, call.start()),
                                    "GpuSyncReadScope::read()",
                                    "public read() is not in the reviewed synchronous-adapter "
                                    "allowlist")
                        )
                    findings.append(
                        Finding(path, line_number(source, call.start()),
                                f"{referenced.declaration.name}.read()",
                                "read() requires an exact local scope/lease and standalone "
                                "complete() in the same lexical body")
                    )
            continue
        scope_key = scope.declaration.position
        if call.group(1) == "read":
            claimed_reads.add(call.start())
            if path not in SYNC_READ_ALLOWLIST:
                findings.append(
                    Finding(path, line_number(source, call.start()),
                            "GpuSyncReadScope::read()",
                            "public read() is not in the reviewed synchronous-adapter allowlist")
                )
            lease = canonical_read_lease(masked, pairs, call, scope.declaration.name)
            call_block = immediate_block(pairs, call.start())
            canonical = (canonical_scope_declaration(masked, pairs, scope)
                         and call_block == scope.block and lease is not None)
            if not canonical:
                findings.append(
                    Finding(path, line_number(source, call.start()),
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
            if (canonical_scope_declaration(masked, pairs, scope)
                    and immediate_block(pairs, call.start()) == scope.block):
                acquisitions_by_scope.setdefault(scope_key, []).append(call)
                handle_bindings.extend(callback_lease_bindings(masked, pairs, call))
                callback_block = withread_callback_block(masked, pairs, call)
                if callback_block is None:
                    findings.append(Finding(
                        path, line_number(source, call.start()), "GpuSyncReadScope::withRead()",
                        "withRead() requires an inline callback body so completion cannot be "
                        "bypassed by hidden control flow"))
                else:
                    nonlocal_jump = next((
                        token for token in cpp_tokens(masked, callback_block[0] + 1,
                                                      callback_block[1])
                        if token.value in {"_longjmp", "longjmp", "siglongjmp"}
                    ), None)
                    if nonlocal_jump is not None:
                        findings.append(Finding(
                            path, line_number(source, nonlocal_jump.start), nonlocal_jump.value,
                            "non-local jump cannot bypass withRead() completion"))
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
                Finding(path, line_number(source, call.start()), "GpuSyncReadScope::read()",
                        "public read() is not in the reviewed synchronous-adapter allowlist")
            )
        findings.append(
            Finding(path, line_number(source, call.start()), "read()",
                    "a temporary read scope cannot call complete() after read/use")
        )

    native_calls = [call for call in calls if call.group(1) == "nativeHandle"]
    native_bindings = {call.start(): resolve_handle_binding(handle_bindings, masked, pairs, call)
                       for call in native_calls}
    for call in native_calls:
        binding = native_bindings[call.start()]
        if (binding is not None
                and handle_binding_is_shadowed(binding, masked, pairs, call.start())):
            native_bindings[call.start()] = None

    for binding in handle_bindings:
        if not lease_binding_stays_local(masked, pairs, binding):
            findings.append(Finding(
                path, line_number(source, binding.position), binding.name,
                "lease reference must remain inside the immediate callback body"))

    all_tokens = cpp_tokens(masked)
    native_call_names = {call.start(1) for call in native_calls}
    for index, token in enumerate(all_tokens):
        if token.value != "nativeHandle" or token.start in native_call_names:
            continue
        if native_handle_declaration_or_internal_call(
                path, source, masked, token, all_tokens, index):
            continue
        findings.append(Finding(
            path, line_number(source, token.start), "nativeHandle reference",
            "nativeHandle reference cannot be aliased, passed through preprocessing, or invoked "
            "through a pointer-to-member"))

    for scope_key, acquisitions in acquisitions_by_scope.items():
        if len(acquisitions) != 1:
            first = min(acquisitions, key=lambda call: call.start())
            findings.append(Finding(
                path, line_number(source, first.start()), "GpuSyncReadScope acquisition",
                "a synchronous read scope permits exactly one acquisition total: read() or "
                "withRead()"))
            continue
        acquisition = acquisitions[0]
        completions = completes_by_scope.get(scope_key, [])
        if any(call.start() < acquisition.start() for call in completions):
            findings.append(Finding(
                path, line_number(source, acquisition.start()), "GpuSyncReadScope::complete()",
                "complete() cannot precede acquisition; complete() after read/use is required"))
        if acquisition.group(1) == "withRead" and completions:
            findings.append(Finding(
                path, line_number(source, completions[0].start()), "GpuSyncReadScope::complete()",
                "withRead() owns completion; explicit complete() is forbidden"))

    valid_read_scopes: set[int] = set()
    for scope_key, reads in reads_by_scope.items():
        if len(reads) != 1:
            first_read = min(reads, key=lambda call: call.start())
            findings.append(
                Finding(path, line_number(source, first_read.start()), "GpuSyncReadScope::read()",
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
                path, line_number(source, earlier[0].start()), "GpuSyncReadScope::complete()",
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
            Finding(path, line_number(source, first_read.start()),
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
                Finding(path, line_number(source, handle_read.start()),
                        "GpuSurface::nativeHandle()",
                        "native handle access must use the lease returned by its specific "
                        "read scope or withRead callback")
            )
            continue
        alias = local_native_alias(path, masked, pairs, handle_read)
        if alias is None:
            if native_handle_has_direct_consumer(path, masked, handle_read, binding):
                continue
            findings.append(Finding(
                path, line_number(source, handle_read.start()), "GpuReadLease::nativeHandle()",
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
                path, line_number(source, handle_read.start()), alias[0],
                "native handle value must remain inside its synchronous consumption"))
    return findings


def audit_public_member(path: PurePosixPath, source: str, class_name: str,
                        member_pattern: str, expression: str) -> list[Finding]:
    masked = mask_non_code(source)
    class_match = re.search(rf"\bclass\s+{re.escape(class_name)}\b[^{{;]*{{", masked)
    if not class_match:
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


def audit_sources(sources: dict[PurePosixPath, str]) -> list[Finding]:
    findings: list[Finding] = []
    for path, source in sources.items():
        if not is_production_path(path):
            continue
        findings.extend(audit_capability_uses(path, source))
    if REGISTRY_HEADER in sources:
        findings.extend(audit_public_member(REGISTRY_HEADER, sources[REGISTRY_HEADER],
                                            "GpuRetireRegistry", r"\bregisterRetire\s*\(",
                                            "GpuRetireRegistry::registerRetire()"))
    if OP_SCOPE_HEADER in sources:
        findings.extend(audit_public_member(OP_SCOPE_HEADER, sources[OP_SCOPE_HEADER],
                                            "GpuOpScope", r"\btrack\s*\(",
                                            "GpuOpScope::track()"))
    return findings


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
         "#include <vendor_cat.h>\n"
         "void bad(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
         "scope.withRead(s, [](const GpuReadLease& lease) { "
         "void* handle = lease.CAT(native, Handle)(); "
         "(void) isCompatibleWithNativeHandle(handle); }); }",
         "guarded identifier macro composition"),
        (PurePosixPath("playback/gpu/gpufence.h"),
         "#include <vendor_cat.h>\n"
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

    def phase_two_adversarial_source(count: int) -> str:
        declarations = " ".join(
            f"GpuSyncReadScope s{index};" for index in range(count))
        calls = " ".join(
            f"s{index}.re" + chr(92) + "\nad(surface);" for index in range(count))
        return "void probe() { " + declarations + calls + " }"

    def median_phase_two_runtime(count: int) -> float:
        samples: list[float] = []
        source = phase_two_adversarial_source(count)
        for _ in range(3):
            started = time.perf_counter()
            findings = phase_two_capability_findings(
                PurePosixPath("playback/gpu/gpufence.h"), source)
            samples.append(time.perf_counter() - started)
            if len(findings) != count:
                raise AssertionError("phase-two adversarial binding control lost findings")
        return statistics.median(samples)

    small_runtime = median_phase_two_runtime(64)
    large_runtime = median_phase_two_runtime(256)
    if large_runtime > max(1.0, small_runtime * 8.0):
        raise AssertionError(
            "phase-two reconstructed scope lookup is not near-linear: "
            f"64={small_runtime:.4f}s, 256={large_runtime:.4f}s")

    def nested_phase_two_source(count: int) -> str:
        openings = "".join(
            f"{{ GpuSyncReadScope s{index}; s{index}.re" + chr(92) + "\nad(surface);"
            for index in range(count))
        return "void nested() {" + openings + ("}" * count) + "}"

    def median_nested_runtime(count: int) -> float:
        source = nested_phase_two_source(count)
        samples: list[float] = []
        for _ in range(2):
            started = time.perf_counter()
            findings = phase_two_capability_findings(
                PurePosixPath("playback/gpu/gpufence.h"), source)
            samples.append(time.perf_counter() - started)
            if len(findings) != count:
                raise AssertionError("nested phase-two binding control lost findings")
        return statistics.median(samples)

    nested_small = median_nested_runtime(1024)
    nested_large = median_nested_runtime(2048)
    if nested_large > nested_small * 3.25:
        raise AssertionError(
            "nested phase-two declaration assignment is not near-linear: "
            f"1024={nested_small:.4f}s, 2048={nested_large:.4f}s")

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
    args = parser.parse_args()

    mutation_self_tests()
    root = args.source_root.resolve()
    findings = audit_sources(load_production_sources(root))
    if findings:
        for finding in sorted(findings, key=lambda item: (str(item.path), item.line, item.expression)):
            print(finding.render())
        return 1
    print("PASS: GPU capability source audit and mutation self-tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
