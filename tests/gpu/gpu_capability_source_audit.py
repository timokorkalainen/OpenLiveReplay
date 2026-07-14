#!/usr/bin/env python3
"""Audit production GPU capability use without widening the public API."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
from typing import Iterable


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm"}
PRODUCTION_ROOTS = ("playback", "recorder_engine")
LEASE_HEADER = PurePosixPath("playback/gpu/gpusurfacelease.h")
REGISTRY_HEADER = PurePosixPath("playback/gpu/gpuretireregistry.h")
OP_SCOPE_HEADER = PurePosixPath("playback/gpu/gpuopscope.h")

# Public read() is retained by the locked design but confined to reviewed,
# synchronous compatibility adapters. New production code should use withRead().
SYNC_READ_ALLOWLIST = frozenset({PurePosixPath("playback/gpu/gpufence.h")})


@dataclass(frozen=True)
class Finding:
    path: PurePosixPath
    line: int
    expression: str
    reason: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: forbidden expression {self.expression}: {self.reason}"


def mask_non_code(source: str) -> str:
    """Replace comments and literals with spaces while preserving offsets/lines."""
    result = list(source)
    state = "code"
    index = 0
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
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


def audit_capability_uses(path: PurePosixPath, source: str) -> list[Finding]:
    masked = mask_non_code(source)
    pairs = brace_pairs(masked)
    findings: list[Finding] = []

    if path != LEASE_HEADER:
        direct_patterns = (
            re.compile(r"\b[A-Za-z_]\w*\s*->\s*nativeHandle\s*\("),
            re.compile(r"\bGpuSurface\s*::\s*nativeHandle\s*\("),
        )
        for pattern in direct_patterns:
            for match in pattern.finditer(masked):
                findings.append(
                    Finding(path, line_number(source, match.start()), "GpuSurface::nativeHandle()",
                            "direct base-surface access is confined to gpusurfacelease.h")
                )

    valid_scope_blocks: list[tuple[int, int]] = []
    scope_pattern = re.compile(r"\bGpuSyncReadScope\s+([A-Za-z_]\w*)\s*;")
    for declaration in scope_pattern.finditer(masked):
        name = declaration.group(1)
        block = enclosing_block(pairs, declaration.start())
        if block is None:
            findings.append(
                Finding(path, line_number(source, declaration.start()), "GpuSyncReadScope",
                        "scope declaration is not inside a lexical block")
            )
            continue
        block_text = masked[declaration.start(): block[1]]
        with_read = re.search(rf"\b{re.escape(name)}\s*\.\s*withRead\s*\(", block_text)
        direct_reads = list(re.finditer(rf"\b{re.escape(name)}\s*\.\s*read\s*\(", block_text))
        if direct_reads and path not in SYNC_READ_ALLOWLIST:
            read_position = declaration.start() + direct_reads[0].start()
            findings.append(
                Finding(path, line_number(source, read_position), "GpuSyncReadScope::read()",
                        "public read() is not in the reviewed synchronous-adapter allowlist")
            )
        completed = re.search(rf"\b{re.escape(name)}\s*\.\s*complete\s*\(", block_text)
        if direct_reads and not completed:
            read_position = declaration.start() + direct_reads[0].start()
            findings.append(
                Finding(path, line_number(source, read_position), f"{name}.read()",
                        "the lexical scope must call complete() or use withRead()")
            )
        if with_read or (direct_reads and completed):
            valid_scope_blocks.append((declaration.start(), block[1]))

    if path != LEASE_HEADER:
        for handle_read in re.finditer(r"\b[A-Za-z_]\w*\s*\.\s*nativeHandle\s*\(", masked):
            if not any(start <= handle_read.start() <= end for start, end in valid_scope_blocks):
                findings.append(
                    Finding(path, line_number(source, handle_read.start()), "lease.nativeHandle()",
                            "native handle reads must be inside a completed GpuSyncReadScope")
                )
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
         "lease.nativeHandle()"),
    )
    for path, source, expected in cases:
        rendered = "\n".join(finding.render() for finding in audit_capability_uses(path, source))
        if expected not in rendered:
            raise AssertionError(f"source-audit mutation survived ({expected}):\n{rendered}")

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
        "void safe(const std::shared_ptr<GpuSurface>& s) { GpuSyncReadScope scope; "
        "scope.withRead(s, [](const GpuReadLease& lease) { return lease.nativeHandle(); }); }"
    )
    safe_findings = audit_capability_uses(PurePosixPath("playback/safe.cpp"), safe)
    if safe_findings:
        raise AssertionError("withRead pass control was rejected:\n" +
                             "\n".join(finding.render() for finding in safe_findings))


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
