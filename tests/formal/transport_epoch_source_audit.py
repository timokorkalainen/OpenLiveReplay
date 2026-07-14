#!/usr/bin/env python3
"""Source-coupling audit for the transport epoch commit protocol."""

from __future__ import annotations

import re
import sys
from pathlib import Path


class AuditFailure(RuntimeError):
    pass


def line_number(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def source_line(source: str, offset: int) -> str:
    return source.splitlines()[line_number(source, offset) - 1].strip()


def lexical_code(source: str) -> str:
    """Blank comments and quoted text while preserving offsets and newlines."""
    code = list(source)
    state = "code"
    offset = 0
    while offset < len(source):
        current = source[offset]
        following = source[offset + 1] if offset + 1 < len(source) else ""
        if state == "code":
            if current == "/" and following == "/":
                code[offset] = code[offset + 1] = " "
                state = "line-comment"
                offset += 2
                continue
            if current == "/" and following == "*":
                code[offset] = code[offset + 1] = " "
                state = "block-comment"
                offset += 2
                continue
            if current == '"':
                code[offset] = " "
                state = "string"
            elif current == "'":
                code[offset] = " "
                state = "character"
        elif state == "line-comment":
            if current == "\n":
                state = "code"
            else:
                code[offset] = " "
        elif state == "block-comment":
            if current == "*" and following == "/":
                code[offset] = code[offset + 1] = " "
                state = "code"
                offset += 2
                continue
            if current != "\n":
                code[offset] = " "
        else:
            if current == "\\" and following:
                code[offset] = " "
                if following != "\n":
                    code[offset + 1] = " "
                offset += 2
                continue
            if (state == "string" and current == '"') or (
                state == "character" and current == "'"
            ):
                code[offset] = " "
                state = "code"
            elif current != "\n":
                code[offset] = " "
        offset += 1
    return "".join(code)


def function_span(source: str, signature: str, path: Path) -> tuple[int, int]:
    code = lexical_code(source)
    match = re.search(signature, code, re.MULTILINE)
    if not match:
        raise AuditFailure(f"{path}:1: required function not found")
    opening = code.find("{", match.end())
    if opening < 0:
        raise AuditFailure(
            f"{path}:{line_number(source, match.start())}: function body has no opening brace"
        )
    depth = 0
    for offset in range(opening, len(code)):
        if code[offset] == "{":
            depth += 1
        elif code[offset] == "}":
            depth -= 1
            if depth == 0:
                return opening, offset + 1
    raise AuditFailure(
        f"{path}:{line_number(source, opening)}: function body has no closing brace"
    )


def fail_at(path: Path, source: str, offset: int, reason: str) -> None:
    raise AuditFailure(
        f"{path}:{line_number(source, offset)}: {reason}: {source_line(source, offset)}"
    )


def audit_playbackworker(source: str, path: Path) -> None:
    begin, end = function_span(
        source,
        r"PlaybackWorker::commitOutputStateLocked\s*\([^)]*\)\s*",
        path,
    )
    stores = list(
        re.finditer(r"\bm_committedGeneration\s*\.\s*store\s*\(", lexical_code(source))
    )
    if not stores:
        raise AuditFailure(f"{path}:1: no m_committedGeneration.store found")
    for store in stores:
        if not (begin <= store.start() < end):
            fail_at(
                path,
                source,
                store.start(),
                "m_committedGeneration.store outside commitOutputStateLocked",
            )
    if len(stores) != 1:
        fail_at(
            path,
            source,
            stores[1].start(),
            "more than one m_committedGeneration.store in commitOutputStateLocked",
        )


def audit_outputruntime(source: str, path: Path) -> None:
    begin, end = function_span(source, r"void\s+OutputRuntime::resetPlayEpoch\s*\(\s*\)\s*", path)
    body = lexical_code(source)[begin:end]
    generation = re.search(r"\+\+m_configGeneration\s*;", body)
    active_branch = re.search(r"if\s*\(\s*m_dispatchActive\s*\)", body)
    if not generation:
        fail_at(path, source, begin, "resetPlayEpoch is missing ++m_configGeneration")
    if not active_branch:
        fail_at(path, source, begin, "resetPlayEpoch is missing the m_dispatchActive branch")
    generation_offset = begin + generation.start()
    branch_offset = begin + active_branch.start()
    if generation_offset > branch_offset:
        fail_at(
            path,
            source,
            branch_offset,
            "m_dispatchActive branch precedes ++m_configGeneration",
        )


def require_rejection(check, source: str, path: Path, expected_line: int, expected: str) -> None:
    try:
        check(source, path)
    except AuditFailure as failure:
        message = str(failure)
        required = f"{path}:{expected_line}:"
        if required not in message or expected not in message:
            raise AuditFailure(
                f"synthetic audit rejected for the wrong reason: expected {required} and "
                f"{expected!r}, got {message!r}"
            ) from failure
        return
    raise AuditFailure(f"synthetic violation was accepted: {path}:{expected_line}: {expected}")


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    worker_path = root / "playback" / "playbackworker.cpp"
    runtime_path = root / "playback" / "output" / "outputruntime.cpp"
    worker = worker_path.read_text(encoding="utf-8")
    runtime = runtime_path.read_text(encoding="utf-8")

    audit_playbackworker(worker, worker_path)
    audit_outputruntime(runtime, runtime_path)

    outside_line = len(worker.splitlines()) + 2
    require_rejection(
        audit_playbackworker,
        worker + "\nm_committedGeneration.store(99, std::memory_order_release);\n",
        worker_path,
        outside_line,
        "outside commitOutputStateLocked",
    )

    generation = "    ++m_configGeneration;"
    branch = "    if (m_dispatchActive) {"
    if generation not in runtime or branch not in runtime:
        raise AuditFailure(f"{runtime_path}:1: expected F1 statements not found for order mutation")
    reset_begin, reset_end = function_span(
        runtime, r"void\s+OutputRuntime::resetPlayEpoch\s*\(\s*\)\s*", runtime_path
    )
    reset_body = runtime[reset_begin:reset_end]
    reset_body = reset_body.replace(
        generation, "    /* audit moved generation below branch */", 1
    )
    reset_body = reset_body.replace(branch, branch + "\n" + generation, 1)
    mutated_runtime = runtime[:reset_begin] + reset_body + runtime[reset_end:]
    mutated_begin, mutated_end = function_span(
        mutated_runtime,
        r"void\s+OutputRuntime::resetPlayEpoch\s*\(\s*\)\s*",
        runtime_path,
    )
    branch_offset = mutated_runtime.find(branch, mutated_begin, mutated_end)
    branch_line = line_number(mutated_runtime, branch_offset)
    require_rejection(
        audit_outputruntime,
        mutated_runtime,
        runtime_path,
        branch_line,
        "branch precedes ++m_configGeneration",
    )

    print(
        "transport epoch source audit: PASS "
        "(sole committed-generation owner, F1 order, synthetic violations rejected)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AuditFailure as failure:
        print(f"transport epoch source audit: FAIL: {failure}", file=sys.stderr)
        raise SystemExit(1)
