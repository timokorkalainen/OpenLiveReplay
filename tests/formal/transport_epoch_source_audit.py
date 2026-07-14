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
    """Return complete statements in the function body's outermost scope.

    A closing compound brace is a statement boundary too. Keeping that span is
    essential for reachability checks: discarding ``{ return; }`` would make the
    statements on either side appear adjacent even though production cannot
    reach the latter statement.
    """
    statements: list[tuple[int, int, str]] = []
    depth = 1
    parentheses = 0
    brackets = 0
    start = begin + 1
    offset = begin + 1
    while offset < end - 1:
        char = code[offset]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 1:
                following = offset + 1
                while following < end - 1 and code[following].isspace():
                    following += 1
                continuation = re.match(r"(?:else|catch|while)\b", code[following:])
                if not continuation and (
                    following >= end - 1 or code[following] not in ";,"
                ):
                    text = re.sub(r"\s+", "", code[start : offset + 1])
                    if text:
                        statements.append((start, offset + 1, text))
                    start = offset + 1
        elif depth == 1:
            if char == "(":
                parentheses += 1
            elif char == ")":
                parentheses = max(0, parentheses - 1)
            elif char == "[":
                brackets += 1
            elif char == "]":
                brackets = max(0, brackets - 1)
            elif char == ";" and parentheses == 0 and brackets == 0:
                text = re.sub(r"\s+", "", code[start : offset + 1])
                if text:
                    statements.append((start, offset + 1, text))
                start = offset + 1
        offset += 1
    return statements


def statement_containing(
    statements: list[tuple[int, int, str]], offset: int
) -> tuple[int, tuple[int, int, str]] | tuple[None, None]:
    for index, statement in enumerate(statements):
        if statement[0] <= offset < statement[1]:
            return index, statement
    return None, None


KNOWN_PRODUCTION_MACROS = {
    "OLR_UNIT_TEST": False,
}


def production_condition_value(kind: str, expression: str) -> bool | None:
    """Evaluate the small known part of a production preprocessor condition.

    Unknown build selectors remain indeterminate. That is deliberate: a
    transfer under an unknown selector may be production-active, while a
    required operation under it is not unconditionally production-active.
    """
    expression = expression.strip()
    if kind in {"ifdef", "ifndef"}:
        defined = KNOWN_PRODUCTION_MACROS.get(expression)
        if defined is None:
            return None
        return defined if kind == "ifdef" else not defined

    while expression.startswith("(") and expression.endswith(")"):
        expression = expression[1:-1].strip()
    if expression in {"0", "false"}:
        return False
    if expression in {"1", "true"}:
        return True
    negated_defined = re.fullmatch(
        r"!\s*defined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|\s+([A-Za-z_]\w*))",
        expression,
    )
    if negated_defined:
        name = negated_defined.group(1) or negated_defined.group(2)
        defined = KNOWN_PRODUCTION_MACROS.get(name)
        return None if defined is None else not defined
    defined_match = re.fullmatch(
        r"defined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|\s+([A-Za-z_]\w*))",
        expression,
    )
    if defined_match:
        name = defined_match.group(1) or defined_match.group(2)
        return KNOWN_PRODUCTION_MACROS.get(name)
    return None


@dataclass
class ProductionConditional:
    parent_possible: bool
    parent_guaranteed: bool
    conditions: list[bool | None]
    possible: bool
    guaranteed: bool


def set_conditional_branch(frame: ProductionConditional, condition: bool | None) -> None:
    prior = frame.conditions
    prior_can_all_be_false = not any(value is True for value in prior)
    prior_must_all_be_false = all(value is False for value in prior)
    condition_can_be_true = condition is not False
    condition_must_be_true = condition is True
    frame.possible = (
        frame.parent_possible and prior_can_all_be_false and condition_can_be_true
    )
    frame.guaranteed = (
        frame.parent_guaranteed and prior_must_all_be_false and condition_must_be_true
    )
    frame.conditions.append(condition)


def production_branch_state(view: LexedSource, offset: int) -> tuple[bool, bool]:
    """Return whether code may be and is guaranteed to be active in production."""
    stack: list[ProductionConditional] = []
    cursor = 0
    for line in view.comment_code.splitlines(keepends=True):
        if cursor > offset:
            break
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind = directive.group(1)
            expression = directive.group(2).strip()
            if kind in {"if", "ifdef", "ifndef"}:
                parent_possible = stack[-1].possible if stack else True
                parent_guaranteed = stack[-1].guaranteed if stack else True
                frame = ProductionConditional(
                    parent_possible,
                    parent_guaranteed,
                    [],
                    parent_possible,
                    parent_guaranteed,
                )
                set_conditional_branch(
                    frame, production_condition_value(kind, expression)
                )
                stack.append(frame)
            elif kind == "elif" and stack:
                set_conditional_branch(
                    stack[-1], production_condition_value("if", expression)
                )
            elif kind == "else" and stack:
                set_conditional_branch(stack[-1], True)
            elif kind == "endif" and stack:
                stack.pop()
        cursor += len(line)
    if not stack:
        return True, True
    return stack[-1].possible, stack[-1].guaranteed


def require_production_active(
    view: LexedSource, offset: int, path: Path, reason: str
) -> None:
    _, guaranteed = production_branch_state(view, offset)
    if not guaranteed:
        fail_at(path, view, offset, reason)


def production_control_flow_barrier(
    view: LexedSource, statement: tuple[int, int, str]
) -> int | None:
    statement_code = view.code[statement[0] : statement[1]]
    for barrier in re.finditer(r"\b(?:co_return|return|throw|goto)\b", statement_code):
        barrier_offset = statement[0] + barrier.start()
        possible, _ = production_branch_state(view, barrier_offset)
        if possible:
            return barrier_offset
    return None


def explicitly_permitted_intervening_statement(
    view: LexedSource, statement: tuple[int, int, str]
) -> bool:
    """Allow only production-inactive code or a demonstrably inert local scope."""
    statement_code = view.code[statement[0] : statement[1]]
    first_code = re.search(r"\S", statement_code)
    if first_code is None:
        return True
    first_offset = statement[0] + first_code.start()
    possible, _ = production_branch_state(view, first_offset)
    if not possible:
        return True

    inert_scope = re.fullmatch(
        r"\{constbool([A-Za-z_]\w*)=(true|false);\(void\)([A-Za-z_]\w*);\}",
        statement[2],
    )
    return bool(inert_scope and inert_scope.group(1) == inert_scope.group(3))


def audit_load_bearing_macro_definitions(view: LexedSource, path: Path) -> None:
    """Reject preprocessor rewrites of either side of the F2 hand-off.

    The normal code view deliberately blanks directives before structural
    matching.  Inspect the comment/string-blanked directive view separately so
    a production-only function-like macro cannot leave the audited call text in
    place while compiling it away.  A macro named ``store`` is also forbidden:
    it can rewrite the member call token even without mentioning the atomic.
    Harmless macros remain valid.
    """
    load_bearing_names = {
        "m_committedGeneration",
        "resetOutputPlayEpoch",
    }
    load_bearing_macro_names = load_bearing_names | {"store"}
    directive_pattern = re.compile(
        r"(?m)^[ \t]*#[ \t]*(define|undef)\b([^\n]*)"
    )
    for directive in directive_pattern.finditer(view.comment_code):
        kind = directive.group(1)
        tail = directive.group(2)
        name_match = re.search(r"\b[A-Za-z_]\w*\b", tail)
        if name_match is None:
            continue
        macro_name = name_match.group(0)
        replacement = tail[name_match.end() :]
        replacement_names = set(re.findall(r"\b[A-Za-z_]\w*\b", replacement))
        if macro_name not in load_bearing_macro_names and not (
            replacement_names & load_bearing_names
        ):
            continue
        macro_offset = directive.start(2) + name_match.start()
        fail_at(
            path,
            view,
            macro_offset,
            f"macro {'definition' if kind == 'define' else 'undefinition'} touches "
            "load-bearing transport epoch identifier",
        )


def audit_playbackworker(source: str, path: Path) -> None:
    view = lexical_source(source)
    mutation_switch = re.search(r"\bOLR_MUTATE_[A-Za-z0-9_]*\b", view.comment_code)
    if mutation_switch:
        fail_at(
            path,
            view,
            mutation_switch.start(),
            "shipping source contains a transport mutation switch",
        )
    audit_load_bearing_macro_definitions(view, path)
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
    if store_statement is None or not re.fullmatch(
        r"m_committedGeneration\.store\(commit\.seekGeneration,"
        r"std::memory_order_release\);",
        store_statement[2],
    ):
        fail_at(
            path,
            view,
            store.start(),
            "m_committedGeneration.store must be an unconditional top-level statement",
        )
    require_production_active(
        view,
        store.start(),
        path,
        "m_committedGeneration.store must be production-active",
    )
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
        path,
        "resetOutputPlayEpoch must be production-active",
    )
    assert store_index is not None and reset_index is not None
    for statement in statements[store_index + 1 : reset_index]:
        barrier = production_control_flow_barrier(view, statement)
        if barrier is not None:
            fail_at(
                path,
                view,
                barrier,
                "production control-flow barrier separates committed-generation store "
                "from epoch reset",
            )
        if not explicitly_permitted_intervening_statement(view, statement):
            statement_code = view.code[statement[0] : statement[1]]
            first_code = re.search(r"\S", statement_code)
            assert first_code is not None
            fail_at(
                path,
                view,
                statement[0] + first_code.start(),
                "only explicitly permitted test-only constructs may separate "
                "committed-generation store from epoch reset",
            )


def audit_outputruntime(source: str, path: Path) -> None:
    view = lexical_source(source)
    mutation_switch = re.search(r"\bOLR_MUTATE_[A-Za-z0-9_]*\b", view.comment_code)
    if mutation_switch:
        fail_at(
            path,
            view,
            mutation_switch.start(),
            "shipping source contains a transport mutation switch",
        )
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
        path,
        "++m_configGeneration must be production-active",
    )
    for statement_span in statements:
        _, statement_end, statement = statement_span
        if statement_end > generation_offset:
            break
        barrier = production_control_flow_barrier(view, statement_span)
        if barrier is not None:
            if statement == "return;":
                fail_at(path, view, generation_offset, "++m_configGeneration is unreachable")
            fail_at(
                path,
                view,
                barrier,
                "production control-flow barrier precedes ++m_configGeneration",
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

    guarded_worker = worker.replace(
        "    resetOutputPlayEpoch();",
        "#ifndef OLR_MUTATE_SKIP_COMMIT_EPOCH_RESET\n"
        "    resetOutputPlayEpoch();\n"
        "#endif",
        1,
    )
    guarded_worker_offset = guarded_worker.find(
        "OLR_MUTATE_SKIP_COMMIT_EPOCH_RESET"
    )
    require_rejection(
        audit_playbackworker,
        guarded_worker,
        worker_path,
        line_number(guarded_worker, guarded_worker_offset),
        "shipping source contains a transport mutation switch",
    )

    guarded_reset_begin, guarded_reset_end = function_span(
        runtime,
        r"void\s+OutputRuntime::resetPlayEpoch\s*\(\s*\)\s*",
        runtime_path,
    )
    guarded_runtime = replace_once_in_span(
        runtime,
        guarded_reset_begin,
        guarded_reset_end,
        "    ++m_configGeneration;",
        "#ifndef OLR_MUTATE_SKIP_CONFIG_GENERATION\n"
        "    ++m_configGeneration;\n"
        "#endif",
    )
    guarded_runtime_offset = guarded_runtime.find("OLR_MUTATE_SKIP_CONFIG_GENERATION")
    require_rejection(
        audit_outputruntime,
        guarded_runtime,
        runtime_path,
        line_number(guarded_runtime, guarded_runtime_offset),
        "shipping source contains a transport mutation switch",
    )

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
        "#define AUDIT_HARMLESS_DECOY(value) (value)\n"
        "std::atomic<uint64_t> m_committedGeneration;\n"
    )
    require_acceptance(
        audit_playbackworker,
        harmless_mentions,
        worker_path,
        "comments, strings, harmless macro definitions, and declarations are not stores",
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

    production_braced_return = replace_once_in_span(
        runtime,
        reset_begin,
        reset_end,
        generation,
        "    #ifndef OLR_UNIT_TEST\n"
        "    { return; }\n"
        "    #endif\n"
        + generation,
    )
    production_braced_return_offset = production_braced_return.find(
        "return;", reset_begin
    )
    require_rejection(
        audit_outputruntime,
        production_braced_return,
        runtime_path,
        line_number(production_braced_return, production_braced_return_offset),
        "production control-flow barrier precedes ++m_configGeneration",
    )

    nested_generation_barriers = (
        (
            "    if (condition) {\n"
            "        switch (mode) {\n"
            "        case 0: return;\n"
            "        default: break;\n"
            "        }\n"
            "    }\n",
            "return;",
        ),
        ("    if (condition) { throw failure; }\n", "throw"),
        ("    if (condition) { goto afterGeneration; }\n", "goto"),
        ("    if (condition) co_return;\n", "co_return"),
    )
    for barrier, token in nested_generation_barriers:
        barrier_generation = replace_once_in_span(
            runtime,
            reset_begin,
            reset_end,
            generation,
            barrier + generation,
        )
        barrier_offset = barrier_generation.find(token, reset_begin)
        require_rejection(
            audit_outputruntime,
            barrier_generation,
            runtime_path,
            line_number(barrier_generation, barrier_offset),
            "production control-flow barrier precedes ++m_configGeneration",
        )

    harmless_generation_scope = replace_once_in_span(
        runtime,
        reset_begin,
        reset_end,
        generation,
        "    { int harmless = 0; (void)harmless; } // return; is only a comment\n"
        "    #ifdef OLR_UNIT_TEST\n"
        "    { return; }\n"
        "    #endif\n"
        + generation,
    )
    require_acceptance(
        audit_outputruntime,
        harmless_generation_scope,
        runtime_path,
        "transfer-free scopes, comments, and test-only returns before F1",
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

    production_alias = worker[:commit_begin] + (
        "\n#ifndef OLR_UNIT_TEST\n"
        "#define resetOutputPlayEpoch() ((void)0)\n"
        "#endif\n"
    ) + worker[commit_begin:]
    production_alias_offset = production_alias.find(
        "#define resetOutputPlayEpoch", commit_begin
    )
    require_rejection(
        audit_playbackworker,
        production_alias,
        worker_path,
        line_number(production_alias, production_alias_offset),
        "macro definition touches load-bearing transport epoch identifier",
    )

    committed_generation_alias = worker[:commit_begin] + (
        "\n#define COMMITTED_GENERATION_ALIAS m_committedGeneration\n"
    ) + worker[commit_begin:]
    committed_generation_alias_offset = committed_generation_alias.find(
        "#define COMMITTED_GENERATION_ALIAS", commit_begin
    )
    require_rejection(
        audit_playbackworker,
        committed_generation_alias,
        worker_path,
        line_number(committed_generation_alias, committed_generation_alias_offset),
        "macro definition touches load-bearing transport epoch identifier",
    )

    store_token_rewrite = worker[:commit_begin] + (
        "\n#define store(...) loadWithoutPublishing(__VA_ARGS__)\n"
    ) + worker[commit_begin:]
    store_token_rewrite_offset = store_token_rewrite.find("#define store", commit_begin)
    require_rejection(
        audit_playbackworker,
        store_token_rewrite,
        worker_path,
        line_number(store_token_rewrite, store_token_rewrite_offset),
        "macro definition touches load-bearing transport epoch identifier",
    )

    reset_undefinition = worker[:commit_begin] + (
        "\n#undef resetOutputPlayEpoch\n"
    ) + worker[commit_begin:]
    reset_undefinition_offset = reset_undefinition.find(
        "#undef resetOutputPlayEpoch", commit_begin
    )
    require_rejection(
        audit_playbackworker,
        reset_undefinition,
        worker_path,
        line_number(reset_undefinition, reset_undefinition_offset),
        "macro undefinition touches load-bearing transport epoch identifier",
    )

    preprocessor_store = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        committed_store,
        "    #ifdef OLR_UNIT_TEST\n"
        + committed_store
        + "\n    #endif",
    )
    preprocessor_store_offset = preprocessor_store.find(
        "m_committedGeneration.store", commit_begin
    )
    require_rejection(
        audit_playbackworker,
        preprocessor_store,
        worker_path,
        line_number(preprocessor_store, preprocessor_store_offset),
        "m_committedGeneration.store must be production-active",
    )

    intervening_call = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        committed_store + "\n" + epoch_reset,
        committed_store
        + "\n    exposeCommittedGenerationBeforeEpochReset();\n"
        + epoch_reset,
    )
    intervening_call_offset = intervening_call.find(
        "exposeCommittedGenerationBeforeEpochReset", commit_begin
    )
    require_rejection(
        audit_playbackworker,
        intervening_call,
        worker_path,
        line_number(intervening_call, intervening_call_offset),
        "only explicitly permitted test-only constructs may separate "
        "committed-generation store from epoch reset",
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

    production_braced_result_return = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        committed_store + "\n" + epoch_reset,
        committed_store
        + "\n#ifndef OLR_UNIT_TEST\n"
        + "    { return result; }\n"
        + "#endif\n"
        + epoch_reset,
    )
    production_result_return_offset = production_braced_result_return.find(
        "return result", store_offset
    )
    require_rejection(
        audit_playbackworker,
        production_braced_result_return,
        worker_path,
        line_number(production_braced_result_return, production_result_return_offset),
        "production control-flow barrier separates committed-generation store from epoch reset",
    )

    harmless_reset_scope = replace_once_in_span(
        worker,
        commit_begin,
        commit_end,
        committed_store + "\n" + epoch_reset,
        committed_store
        + "\n    { const bool harmless = true; (void)harmless; } // throw is a comment\n"
        + "#ifdef OLR_UNIT_TEST\n"
        + "    { return result; }\n"
        + "#endif\n"
        + epoch_reset,
    )
    require_acceptance(
        audit_playbackworker,
        harmless_reset_scope,
        worker_path,
        "transfer-free scopes, comments, and test-only returns between F2 operations",
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
        committed_store + "\n" + epoch_reset,
        epoch_reset + "\n" + committed_store,
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
