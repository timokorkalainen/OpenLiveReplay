#!/usr/bin/env python3
"""Source-coupling audit for the transport epoch commit protocol."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


class AuditFailure(RuntimeError):
    pass


def line_number(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def source_line(source: str, offset: int) -> str:
    return source.splitlines()[line_number(source, offset) - 1].strip()


@dataclass(frozen=True)
class LexedSource:
    original: str
    comment_code: str
    code: str
    origins: tuple[int, ...]

    def original_offset(self, logical_offset: int) -> int:
        if logical_offset >= len(self.origins):
            return len(self.original)
        return self.origins[logical_offset]


def splice_escaped_newlines(source: str) -> tuple[str, tuple[int, ...]]:
    """Apply C/C++ phase-2 line splicing and retain original-offset provenance."""
    logical: list[str] = []
    origins: list[int] = []
    offset = 0
    while offset < len(source):
        if source[offset] == "\\":
            if offset + 1 < len(source) and source[offset + 1] == "\n":
                offset += 2
                continue
            if (
                offset + 2 < len(source)
                and source[offset + 1] == "\r"
                and source[offset + 2] == "\n"
            ):
                offset += 3
                continue
        logical.append(source[offset])
        origins.append(offset)
        offset += 1
    return "".join(logical), tuple(origins)


def blank_comments_and_quotes(source: str) -> str:
    """Blank comments and quoted text while preserving logical offsets."""
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


def blank_preprocessor_directives(code: str) -> str:
    blanked: list[str] = []
    for line in code.splitlines(keepends=True):
        if line.lstrip().startswith("#"):
            blanked.append("".join("\n" if char == "\n" else " " for char in line))
        else:
            blanked.append(line)
    return "".join(blanked)


def lexical_source(source: str) -> LexedSource:
    logical, origins = splice_escaped_newlines(source)
    comment_code = blank_comments_and_quotes(logical)
    return LexedSource(
        original=source,
        comment_code=comment_code,
        code=blank_preprocessor_directives(comment_code),
        origins=origins,
    )


def logical_function_span(view: LexedSource, signature: str, path: Path) -> tuple[int, int]:
    match = re.search(signature, view.code, re.MULTILINE)
    if not match:
        raise AuditFailure(f"{path}:1: required function not found")
    opening = view.code.find("{", match.end())
    if opening < 0:
        raise AuditFailure(
            f"{path}:{line_number(view.original, view.original_offset(match.start()))}: "
            "function body has no opening brace"
        )
    depth = 0
    for offset in range(opening, len(view.code)):
        if view.code[offset] == "{":
            depth += 1
        elif view.code[offset] == "}":
            depth -= 1
            if depth == 0:
                return opening, offset + 1
    raise AuditFailure(
        f"{path}:{line_number(view.original, view.original_offset(opening))}: "
        "function body has no closing brace"
    )


def function_span(source: str, signature: str, path: Path) -> tuple[int, int]:
    view = lexical_source(source)
    begin, end = logical_function_span(view, signature, path)
    return view.original_offset(begin), view.original_offset(end - 1) + 1


def fail_at(path: Path, view: LexedSource, offset: int, reason: str) -> None:
    original_offset = view.original_offset(offset)
    raise AuditFailure(
        f"{path}:{line_number(view.original, original_offset)}: {reason}: "
        f"{source_line(view.original, original_offset)}"
    )


def top_level_statements(code: str, begin: int, end: int) -> list[tuple[int, int, str]]:
    statements: list[tuple[int, int, str]] = []
    depth = 1
    parentheses = 0
    start = begin + 1
    for offset in range(begin + 1, end - 1):
        char = code[offset]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 1:
                start = offset + 1
        elif depth == 1:
            if char == "(":
                parentheses += 1
            elif char == ")":
                parentheses = max(0, parentheses - 1)
            elif char == ";" and parentheses == 0:
                text = re.sub(r"\s+", "", code[start : offset + 1])
                statements.append((start, offset + 1, text))
                start = offset + 1
    return statements


def statement_containing(
    statements: list[tuple[int, int, str]], offset: int
) -> tuple[int, tuple[int, int, str]] | tuple[None, None]:
    for index, statement in enumerate(statements):
        if statement[0] <= offset < statement[1]:
            return index, statement
    return None, None


def preprocessor_stack_at(view: LexedSource, offset: int) -> list[str]:
    stack: list[str] = []
    cursor = 0
    for line in view.comment_code.splitlines(keepends=True):
        if cursor > offset:
            break
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind = directive.group(1)
            expression = directive.group(2).strip()
            if kind in {"if", "ifdef", "ifndef"}:
                stack.append(f"{kind} {expression}".strip())
            elif kind in {"elif", "else"} and stack:
                stack[-1] = f"{kind} {expression}".strip()
            elif kind == "endif" and stack:
                stack.pop()
        cursor += len(line)
    return stack


def require_production_active(
    view: LexedSource, offset: int, mutation_guard: str, path: Path, reason: str
) -> None:
    stack = preprocessor_stack_at(view, offset)
    allowed_guard = f"ifndef {mutation_guard}"
    if stack not in ([], [allowed_guard]):
        fail_at(path, view, offset, reason)


def audit_playbackworker(source: str, path: Path) -> None:
    view = lexical_source(source)
    begin, end = logical_function_span(
        view,
        r"PlaybackWorker::commitOutputStateLocked\s*\([^)]*\)\s*",
        path,
    )
    stores = list(
        re.finditer(r"\bm_committedGeneration\s*\.\s*store\s*\(", view.code)
    )
    if not stores:
        raise AuditFailure(f"{path}:1: no m_committedGeneration.store found")
    for store in stores:
        if not (begin <= store.start() < end):
            fail_at(
                path,
                view,
                store.start(),
                "m_committedGeneration.store outside commitOutputStateLocked",
            )
    if len(stores) != 1:
        fail_at(
            path,
            view,
            stores[1].start(),
            "more than one m_committedGeneration.store in commitOutputStateLocked",
        )

    store = stores[0]
    resets = list(re.finditer(r"\bresetOutputPlayEpoch\s*\(\s*\)\s*;", view.code[begin:end]))
    reset_offsets = [begin + reset.start() for reset in resets]
    if not reset_offsets:
        fail_at(
            path,
            view,
            store.start(),
            "missing resetOutputPlayEpoch after committed-generation store",
        )
    if len(reset_offsets) != 1:
        fail_at(path, view, reset_offsets[1], "more than one resetOutputPlayEpoch in commit")

    reset_offset = reset_offsets[0]
    if reset_offset < store.start():
        fail_at(
            path,
            view,
            reset_offset,
            "resetOutputPlayEpoch must follow the committed-generation store",
        )
    statements = top_level_statements(view.code, begin, end)
    store_index, store_statement = statement_containing(statements, store.start())
    reset_index, reset_statement = statement_containing(statements, reset_offset)
    if reset_statement is None or reset_statement[2] != "resetOutputPlayEpoch();":
        fail_at(
            path,
            view,
            reset_offset,
            "resetOutputPlayEpoch must be an unconditional top-level statement",
        )
    require_production_active(
        view,
        reset_offset,
        "OLR_MUTATE_SKIP_COMMIT_EPOCH_RESET",
        path,
        "resetOutputPlayEpoch must be production-active",
    )
    if store_statement is None or reset_index != store_index + 1:
        fail_at(
            path,
            view,
            reset_offset,
            "resetOutputPlayEpoch must immediately follow the committed-generation store",
        )


def audit_outputruntime(source: str, path: Path) -> None:
    view = lexical_source(source)
    begin, end = logical_function_span(
        view, r"void\s+OutputRuntime::resetPlayEpoch\s*\(\s*\)\s*", path
    )
    body = view.code[begin:end]
    generation = re.search(r"\+\+m_configGeneration\s*;", body)
    active_branch = re.search(r"if\s*\(\s*m_dispatchActive\s*\)", body)
    if not generation:
        fail_at(path, view, begin, "resetPlayEpoch is missing ++m_configGeneration")
    if not active_branch:
        fail_at(path, view, begin, "resetPlayEpoch is missing the m_dispatchActive branch")
    generation_offset = begin + generation.start()
    branch_offset = begin + active_branch.start()
    if generation_offset > branch_offset:
        fail_at(
            path,
            view,
            branch_offset,
            "m_dispatchActive branch precedes ++m_configGeneration",
        )

    statements = top_level_statements(view.code, begin, end)
    _, generation_statement = statement_containing(statements, generation_offset)
    if generation_statement is None or generation_statement[2] != "++m_configGeneration;":
        fail_at(
            path,
            view,
            generation_offset,
            "++m_configGeneration must be an unconditional top-level statement",
        )
    require_production_active(
        view,
        generation_offset,
        "OLR_MUTATE_SKIP_CONFIG_GENERATION",
        path,
        "++m_configGeneration must be production-active",
    )
    for _, statement_end, statement in statements:
        if statement_end > generation_offset:
            break
        if statement == "return;":
            fail_at(path, view, generation_offset, "++m_configGeneration is unreachable")


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


def require_acceptance(check, source: str, path: Path, description: str) -> None:
    try:
        check(source, path)
    except AuditFailure as failure:
        raise AuditFailure(
            f"synthetic valid source was rejected ({description}): {failure}"
        ) from failure


def replace_once_in_span(
    source: str, begin: int, end: int, old: str, new: str
) -> str:
    body = source[begin:end]
    if old not in body:
        raise AuditFailure("synthetic mutation target not found in required function")
    return source[:begin] + body.replace(old, new, 1) + source[end:]


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    worker_path = root / "playback" / "playbackworker.cpp"
    runtime_path = root / "playback" / "output" / "outputruntime.cpp"
    worker = worker_path.read_text(encoding="utf-8")
    runtime = runtime_path.read_text(encoding="utf-8")

    audit_playbackworker(worker, worker_path)
    audit_outputruntime(runtime, runtime_path)

    escaped_store = worker + (
        "\nm_committedGeneration\\\n"
        "    .store(99, std::memory_order_release);\n"
    )
    escaped_store_offset = escaped_store.find("m_committedGeneration", len(worker))
    require_rejection(
        audit_playbackworker,
        escaped_store,
        worker_path,
        line_number(escaped_store, escaped_store_offset),
        "outside commitOutputStateLocked",
    )

    escaped_identifier = worker + (
        "\nm_committedGenera\\\n"
        "tion.store(99, std::memory_order_release);\n"
    )
    escaped_identifier_offset = escaped_identifier.find("m_committedGenera", len(worker))
    require_rejection(
        audit_playbackworker,
        escaped_identifier,
        worker_path,
        line_number(escaped_identifier, escaped_identifier_offset),
        "outside commitOutputStateLocked",
    )

    spliced_owner = worker.replace(
        "m_committedGeneration.store(commit.seekGeneration, std::memory_order_release);",
        "m_committedGeneration\\\n"
        "        .store(commit.seekGeneration, std::memory_order_release);",
        1,
    )
    require_acceptance(
        audit_playbackworker,
        spliced_owner,
        worker_path,
        "escaped-newline whitespace at the central store",
    )

    commented_owner = worker.replace(
        "m_committedGeneration.store(commit.seekGeneration, std::memory_order_release);",
        "m_committedGeneration /* central owner */\n"
        "        . store (commit.seekGeneration, std::memory_order_release);",
        1,
    )
    require_acceptance(
        audit_playbackworker,
        commented_owner,
        worker_path,
        "whitespace and an inline comment around the central store",
    )

    harmless_mentions = worker + (
        "\n// m_committedGeneration.store(98, std::memory_order_release);\n"
        "const char* auditText = \"m_committedGeneration.store(97)\";\n"
        "#define AUDIT_STORE_DECOY m_committedGeneration.store(96)\n"
        "std::atomic<uint64_t> m_committedGeneration;\n"
    )
    require_acceptance(
        audit_playbackworker,
        harmless_mentions,
        worker_path,
        "comments, strings, macro definitions, and declarations are not stores",
    )

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

    conditional_generation = replace_once_in_span(
        runtime,
        reset_begin,
        reset_end,
        generation,
        "    if (false) ++m_configGeneration;",
    )
    conditional_offset = conditional_generation.find("if (false)", reset_begin)
    require_rejection(
        audit_outputruntime,
        conditional_generation,
        runtime_path,
        line_number(conditional_generation, conditional_offset),
        "must be an unconditional top-level statement",
    )

    braced_generation = replace_once_in_span(
        runtime,
        reset_begin,
        reset_end,
        generation,
        "    if (someCondition) {\n        ++m_configGeneration;\n    }",
    )
    braced_offset = braced_generation.find("++m_configGeneration", reset_begin)
    require_rejection(
        audit_outputruntime,
        braced_generation,
        runtime_path,
        line_number(braced_generation, braced_offset),
        "must be an unconditional top-level statement",
    )

    unreachable_generation = replace_once_in_span(
        runtime,
        reset_begin,
        reset_end,
        generation,
        "    return;\n    ++m_configGeneration;",
    )
    unreachable_offset = unreachable_generation.find("++m_configGeneration", reset_begin)
    require_rejection(
        audit_outputruntime,
        unreachable_generation,
        runtime_path,
        line_number(unreachable_generation, unreachable_offset),
        "is unreachable",
    )

    preprocessor_generation = replace_once_in_span(
        runtime,
        reset_begin,
        reset_end,
        generation,
        "    #if 0\n    ++m_configGeneration;\n    #endif",
    )
    preprocessor_generation_offset = preprocessor_generation.find(
        "++m_configGeneration", reset_begin
    )
    require_rejection(
        audit_outputruntime,
        preprocessor_generation,
        runtime_path,
        line_number(preprocessor_generation, preprocessor_generation_offset),
        "must be production-active",
    )

    committed_store = (
        "    m_committedGeneration.store(commit.seekGeneration, "
        "std::memory_order_release);"
    )
    epoch_reset = "    resetOutputPlayEpoch();"
    if committed_store not in worker or epoch_reset not in worker:
        raise AuditFailure(f"{worker_path}:1: expected F2 statements not found")
    commit_begin, commit_end = function_span(
        worker,
        r"PlaybackWorker::commitOutputStateLocked\s*\([^)]*\)\s*",
        worker_path,
    )

    missing_reset = replace_once_in_span(
        worker, commit_begin, commit_end, epoch_reset, "    /* reset removed */"
    )
    store_offset = missing_reset.find(committed_store.strip())
    require_rejection(
        audit_playbackworker,
        missing_reset,
        worker_path,
        line_number(missing_reset, store_offset),
        "missing resetOutputPlayEpoch after committed-generation store",
    )

    conditional_reset = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        epoch_reset,
        "    if (false) resetOutputPlayEpoch();",
    )
    conditional_reset_offset = conditional_reset.find("if (false)", store_offset)
    require_rejection(
        audit_playbackworker,
        conditional_reset,
        worker_path,
        line_number(conditional_reset, conditional_reset_offset),
        "must be an unconditional top-level statement",
    )

    preprocessor_reset = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        epoch_reset,
        "    #if 0\n    resetOutputPlayEpoch();\n    #endif",
    )
    preprocessor_reset_offset = preprocessor_reset.find(
        "resetOutputPlayEpoch", store_offset
    )
    require_rejection(
        audit_playbackworker,
        preprocessor_reset,
        worker_path,
        line_number(preprocessor_reset, preprocessor_reset_offset),
        "must be production-active",
    )

    reset_before_store = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        committed_store + "\n#ifndef OLR_MUTATE_SKIP_COMMIT_EPOCH_RESET\n" + epoch_reset,
        epoch_reset + "\n#ifndef OLR_MUTATE_SKIP_COMMIT_EPOCH_RESET\n" + committed_store,
    )
    moved_reset_offset = reset_before_store.find("resetOutputPlayEpoch", store_offset - 200)
    require_rejection(
        audit_playbackworker,
        reset_before_store,
        worker_path,
        line_number(reset_before_store, moved_reset_offset),
        "must follow the committed-generation store",
    )

    print(
        "transport epoch source audit: PASS "
        "(sole committed-generation owner, effective F1/F2 order, adversarial self-tests)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AuditFailure as failure:
        print(f"transport epoch source audit: FAIL: {failure}", file=sys.stderr)
        raise SystemExit(1)
