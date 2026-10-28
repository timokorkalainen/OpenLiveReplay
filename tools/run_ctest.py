#!/usr/bin/env python3
"""Run CTest with the controlled Windows runtime from a CMake build tree."""

from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
import re
import sys
from collections.abc import Mapping, Sequence
from typing import NamedTuple


TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TOOLS_DIR.parent

if __package__:
    from .windows_job import run_process_tree
    from .windows_runtime_guard import (
        discover_ctest_graph,
        CMakeArgumentOccurrence,
        parse_cmake_cache,
        require_cache_entries,
        RuntimeAuditLease,
        RuntimeRoots,
        GeneratedLayoutEntry,
        QtConfPaths,
        WINDOWS_RUNTIME_INJECTION_ENVIRONMENT_VARIABLES,
        reject_runtime_dll_shadowing,
        replace_environment_case_insensitively,
        restore_windows_dll_directory,
        set_windows_dll_directory,
        validate_ctest_runtime_properties,
        windows_system_directories,
        qt_plugin_closure,
        qt_qml_closure,
        transitive_runtime_closure,
        _controlled_family,
    )
else:
    if str(PROJECT_ROOT) not in sys.path:
        sys.path.insert(0, str(PROJECT_ROOT))
    from tools.windows_job import run_process_tree
    from tools.windows_runtime_guard import (
        discover_ctest_graph,
        CMakeArgumentOccurrence,
        parse_cmake_cache,
        require_cache_entries,
        RuntimeAuditLease,
        RuntimeRoots,
        GeneratedLayoutEntry,
        QtConfPaths,
        WINDOWS_RUNTIME_INJECTION_ENVIRONMENT_VARIABLES,
        reject_runtime_dll_shadowing,
        replace_environment_case_insensitively,
        restore_windows_dll_directory,
        set_windows_dll_directory,
        validate_ctest_runtime_properties,
        windows_system_directories,
        qt_plugin_closure,
        qt_qml_closure,
        transitive_runtime_closure,
        _controlled_family,
    )


_SEM_FAILCRITICALERRORS = 0x0001
_SEM_NOGPFAULTERRORBOX = 0x0002
_SEM_NOOPENFILEERRORBOX = 0x8000


def _cmake_bracket(value: str) -> str:
    equals = ""
    while f"]{equals}]" in value:
        equals += "="
    return f"[{equals}[{value}]{equals}]"


def _prepare_path_replacements(
    replacements: Sequence[tuple[Path, Path]],
) -> tuple[tuple[str, str, Path], ...]:
    """Normalize one immutable longest-prefix replacement table."""
    return tuple(
        (
            original_key,
            original_key.rstrip("\\/") + os.sep,
            replacement,
        )
        for original, replacement in sorted(
            replacements, key=lambda item: len(str(item[0])), reverse=True
        )
        for original_key in (
            os.path.normcase(str(Path(os.path.abspath(original)))),
        )
    )


def _rewrite_path_identity(
    value: str,
    replacements: Sequence[tuple[Path, Path]],
    *,
    prepared: tuple[tuple[str, str, Path], ...] | None = None,
) -> str:
    """Rewrite one complete Windows path token, never a string prefix."""
    if not value or not Path(value).is_absolute():
        return value
    candidate = Path(os.path.abspath(value))
    candidate_key = os.path.normcase(str(candidate))
    table = _prepare_path_replacements(replacements) if prepared is None else prepared
    for original_key, prefix, replacement in table:
        if candidate_key == original_key:
            return replacement.as_posix()
        if candidate_key.startswith(prefix):
            relative = Path(candidate_key[len(prefix) :])
            return (replacement / relative).as_posix()
    return value


def _rewrite_environment_property(
    raw: str,
    replacements: Sequence[tuple[Path, Path]],
    *,
    modification: bool,
    prepared_replacements: tuple[tuple[str, str, Path], ...] | None = None,
) -> str:
    rewritten: list[str] = []
    for item in raw.split(";"):
        name, separator, remainder = item.partition("=")
        if not separator:
            rewritten.append(item)
            continue
        if modification:
            operation, operation_separator, value = remainder.partition(":")
            if not operation_separator or name.casefold() not in (
                "path",
                "qml_import_path",
                "qml2_import_path",
            ):
                rewritten.append(item)
                continue
            replacement = _rewrite_path_identity(
                value, replacements, prepared=prepared_replacements
            )
            rewritten.append(f"{name}={operation}:{replacement}")
            continue
        if name.casefold() not in (
            "qt_plugin_path",
            "qt_qpa_platform_plugin_path",
            "qml_import_path",
            "qml2_import_path",
            "olr_ffmpeg_root",
            "olr_srt_root",
            "olr_mingw_bin",
            "olr_qt_bin",
        ):
            rewritten.append(item)
            continue
        rewritten.append(
            f"{name}={_rewrite_path_identity(remainder, replacements, prepared=prepared_replacements)}"
        )
    return ";".join(rewritten)


def _rewrite_cmake_occurrences(
    text: str,
    replacements: Sequence[tuple[CMakeArgumentOccurrence, str]],
) -> str:
    """Rewrite only parsed role-bearing arguments, from right to left."""
    unique: dict[tuple[int, int], tuple[CMakeArgumentOccurrence, str]] = {}
    for occurrence, replacement in replacements:
        key = (occurrence.start, occurrence.end)
        prior = unique.get(key)
        if prior is not None and prior[1] != replacement:
            raise RuntimeError(
                f"conflicting CMake rewrite at {occurrence.start}:{occurrence.end}"
            )
        unique[key] = (occurrence, replacement)

    def render(occurrence: CMakeArgumentOccurrence, value: str) -> str:
        if occurrence.style == "quoted":
            return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
        if occurrence.style == "bracket" or any(
            character.isspace() or character == ")" for character in value
        ):
            return _cmake_bracket(value)
        return value

    for occurrence, replacement in sorted(
        unique.values(), key=lambda item: item[0].start, reverse=True
    ):
        text = (
            text[: occurrence.start]
            + render(occurrence, replacement)
            + text[occurrence.end :]
        )
    return text


class _QuickTestInput(NamedTuple):
    argument: str
    path: Path
    prefix: str
    occurrence: CMakeArgumentOccurrence


class _QuickTestRecordKey(NamedTuple):
    definition_file: Path
    scope_file: Path
    start: int
    end: int


class _DeclaredTestPathInput(NamedTuple):
    definition_file: Path
    argument: str
    path: Path
    prefix: str
    occurrence: CMakeArgumentOccurrence
    role: str


def _quicktest_record_key(test) -> _QuickTestRecordKey:
    occurrence = test.argument_occurrences[0]
    return _QuickTestRecordKey(
        test.definition_file,
        test.scope_file,
        occurrence.start,
        occurrence.end,
    )


def _test_tool_kind(test) -> str:
    ordered_executables = sorted(
        test.executable_arguments, key=lambda item: item[2].start
    )
    if not ordered_executables:
        return "unsupported"
    tool = ordered_executables[0][1].stem.casefold()
    if tool.startswith("python"):
        return "python"
    if tool == "qmllint":
        return "qmllint"
    if tool in ("bash", "sh"):
        return "shell"
    return "unsupported"


def _quicktest_inputs(
    tests, source_root: Path, build_root: Path
) -> tuple[
    dict[_QuickTestRecordKey, tuple[_QuickTestInput, ...]], tuple[Path, ...]
]:
    by_test: dict[_QuickTestRecordKey, list[_QuickTestInput]] = {}
    paths: set[Path] = set()
    for test in tests:
        if _test_tool_kind(test) != "unsupported":
            continue
        arguments = test.arguments
        for index, argument in enumerate(arguments):
            value: str | None = None
            prefix = ""
            if argument.casefold() == "-input":
                if index + 1 >= len(arguments):
                    raise RuntimeError(f"QuickTest -input has no value for {test.name}")
                value = arguments[index + 1]
            elif argument.casefold().startswith("-input="):
                value = argument.partition("=")[2]
                prefix = argument[: len(argument) - len(value)]
            if value is None:
                continue
            if not value or any(marker in value for marker in ("${", "$<", ";")):
                raise RuntimeError(
                    f"QuickTest -input is not a literal path for {test.name}: {value}"
                )
            candidate = Path(value)
            if not candidate.is_absolute():
                candidate = test.working_directory / candidate
            candidate = Path(os.path.abspath(candidate))
            if not candidate.exists():
                raise RuntimeError(
                    f"QuickTest -input does not exist for {test.name}: {candidate}"
                )
            canonical = candidate.resolve()
            if canonical != candidate:
                raise RuntimeError(
                    f"QuickTest -input reparse alias is unsupported: {candidate} -> {canonical}"
                )
            if not any(
                canonical == root or canonical.is_relative_to(root)
                for root in (source_root, build_root)
            ):
                raise RuntimeError(
                    f"QuickTest -input is outside source/build roots: {candidate}"
                )
            paths.add(canonical)
            occurrence_index = index + 1 if not prefix else index
            by_test.setdefault(_quicktest_record_key(test), []).append(
                _QuickTestInput(
                    prefix + value,
                    canonical,
                    prefix,
                    test.argument_occurrences[occurrence_index],
                )
            )
    return (
        {name: tuple(inputs) for name, inputs in by_test.items()},
        tuple(sorted(paths, key=lambda path: str(path).casefold())),
    )


def _declared_test_path_inputs(
    tests,
) -> tuple[
    dict[_QuickTestRecordKey, tuple[_DeclaredTestPathInput, ...]],
    tuple[_DeclaredTestPathInput, ...],
]:
    """Resolve literal path roles declared by supported test-tool syntaxes."""
    by_test: dict[_QuickTestRecordKey, list[_DeclaredTestPathInput]] = {}
    all_inputs: list[_DeclaredTestPathInput] = []
    path_options = {
        "qmllint": {"-i": "qml-import"},
        "python": {
            "--source-root": "source-root",
            "--root": "source-root",
            "--build-dir": "build-root",
            "--qt-runtime-dir": "qt-runtime",
            "--qt-plugin-dir": "qt-plugins",
            "--qt-qml-dir": "qt-qml",
            "--project-qml-dir": "qml-import",
            "--data": "data",
        },
    }

    def is_untyped_path(value: str, candidate: Path) -> bool:
        normalized = value.replace("\\", "/")
        suffix = Path(normalized).suffix.removeprefix(".")
        return (
            candidate.is_absolute()
            or bool(candidate.drive)
            or normalized.startswith(("/", "~/"))
            or "/" in normalized
            or any(part in (".", "..") for part in normalized.split("/"))
            or bool(suffix and any(character.isalpha() for character in suffix))
        )

    def is_unittest_selector(value: str) -> bool:
        parts = value.split(".")
        return (
            len(parts) >= 2
            and all(part.isidentifier() for part in parts)
            and any(
                part[:1].isupper()
                or part.startswith("test_")
                or part in ("test", "tests")
                for part in parts
            )
        )

    for test in tests:
        ordered_executables = sorted(
            test.executable_arguments, key=lambda item: item[2].start
        )
        if not ordered_executables:
            continue
        tool_kind = _test_tool_kind(test)
        executable_offsets = {
            (occurrence.start, occurrence.end)
            for _argument, _path, occurrence in test.executable_arguments
        }
        consumed: set[int] = set()

        def add_input(index: int, value: str, prefix: str, role: str) -> None:
            if not value or any(marker in value for marker in ("${", "$<", ";")):
                raise RuntimeError(
                    f"declared CTest {role} is not a literal path for "
                    f"{test.name}: {value}"
                )
            candidate = Path(value)
            if not candidate.is_absolute():
                candidate = test.working_directory / candidate
            lexical = Path(os.path.abspath(candidate))
            if not lexical.exists():
                raise RuntimeError(
                    f"declared CTest {role} does not exist for {test.name}: {lexical}"
                )
            canonical = lexical.resolve()
            if os.path.normcase(str(lexical)) != os.path.normcase(str(canonical)):
                raise RuntimeError(
                    f"declared CTest {role} reparse alias is unsupported: "
                    f"{lexical} -> {canonical}"
                )
            item = _DeclaredTestPathInput(
                test.definition_file,
                prefix + value,
                canonical,
                prefix,
                test.argument_occurrences[index],
                role,
            )
            by_test.setdefault(_quicktest_record_key(test), []).append(item)
            all_inputs.append(item)
            consumed.add(index)

        options = path_options.get(tool_kind, {})
        python_target_seen = False
        python_options_terminated = False
        python_module_target: str | None = None
        python_unittest_discovery = False
        python_value_options = {
            "-c",
            "-m",
            "-W",
            "-X",
            "--check-hash-based-pycs",
        }
        python_flag_options = {
            "--help",
            "--help-env",
            "--help-xoptions",
            "--help-all",
            "--version",
        }
        python_hash_modes = {"always", "default", "never"}
        python_unittest_targets = {"unittest", "unittest.__main__"}
        unittest_flags = {
            "-h",
            "--help",
            "-v",
            "--verbose",
            "-q",
            "--quiet",
            "--locals",
            "-f",
            "--failfast",
            "-c",
            "--catch",
            "-b",
            "--buffer",
        }
        unittest_opaque_options = {"-k", "--durations"}
        unittest_discovery_opaque_options = {"-p", "--pattern"}
        unittest_path_options = {
            "-s": "unittest-start-directory",
            "--start-directory": "unittest-start-directory",
            "-t": "unittest-top-level-directory",
            "--top-level-directory": "unittest-top-level-directory",
        }
        qmllint_diagnostic_levels = {"disable", "info", "warning", "error"}
        index = 0
        while index < len(test.arguments):
            argument = test.arguments[index]
            occurrence = test.argument_occurrences[index]
            if (occurrence.start, occurrence.end) in executable_offsets:
                consumed.add(index)
                index += 1
                continue
            lowered = argument.casefold()
            if tool_kind == "unsupported" and (
                lowered in ("-input",) or lowered.startswith("-input=")
            ):
                consumed.add(index)
                if lowered == "-input" and index + 1 < len(test.arguments):
                    consumed.add(index + 1)
                    index += 2
                else:
                    index += 1
                continue
            if (
                tool_kind == "python"
                and not python_target_seen
                and argument == "-"
            ):
                consumed.add(index)
                python_target_seen = True
                index += 1
                continue
            if (
                tool_kind == "python"
                and not python_target_seen
                and not python_options_terminated
            ):
                if argument == "--":
                    consumed.add(index)
                    python_options_terminated = True
                    index += 1
                    continue
                if argument in python_value_options:
                    consumed.add(index)
                    if index + 1 >= len(test.arguments):
                        raise RuntimeError(
                            f"Python {argument} has no value for {test.name}"
                        )
                    if (
                        argument == "--check-hash-based-pycs"
                        and test.arguments[index + 1] not in python_hash_modes
                    ):
                        raise RuntimeError(
                            "Python --check-hash-based-pycs value must be "
                            f"always, default, or never for {test.name}"
                        )
                    if argument == "-m":
                        python_module_target = test.arguments[index + 1].casefold()
                    consumed.add(index + 1)
                    python_target_seen = argument in ("-c", "-m")
                    index += 2
                    continue
                attached_value_option = next(
                    (
                        option
                        for option in ("-c", "-m", "-W", "-X")
                        if argument.startswith(option) and len(argument) > len(option)
                    ),
                    None,
                )
                attached_hash_mode = None
                if argument.startswith("--check-hash-based-pycs="):
                    attached_hash_mode = argument.partition("=")[2]
                    if attached_hash_mode not in python_hash_modes:
                        raise RuntimeError(
                            "Python --check-hash-based-pycs value must be "
                            f"always, default, or never for {test.name}"
                        )
                if (
                    attached_value_option is not None
                    or attached_hash_mode is not None
                ):
                    consumed.add(index)
                    python_target_seen = attached_value_option in ("-c", "-m")
                    if attached_value_option == "-m":
                        python_module_target = argument[len("-m") :].casefold()
                    index += 1
                    continue
                if argument in python_flag_options or re.fullmatch(
                    r"-[bBdEhiIOPqRsSuvVx?]+", argument
                ):
                    consumed.add(index)
                    index += 1
                    continue
            if (
                tool_kind == "python"
                and python_module_target in python_unittest_targets
            ):
                if argument == "discover" and not python_unittest_discovery:
                    consumed.add(index)
                    python_unittest_discovery = True
                    index += 1
                    continue
                if argument in unittest_flags:
                    consumed.add(index)
                    index += 1
                    continue
                if argument in unittest_opaque_options or (
                    python_unittest_discovery
                    and argument in unittest_discovery_opaque_options
                ):
                    consumed.add(index)
                    if index + 1 >= len(test.arguments):
                        raise RuntimeError(
                            f"Python unittest {argument} has no value for {test.name}"
                        )
                    consumed.add(index + 1)
                    index += 2
                    continue
                unittest_attached_opaque = (
                    argument.startswith("-k") and len(argument) > 2
                ) or argument.startswith("--durations=")
                if python_unittest_discovery:
                    unittest_attached_opaque = unittest_attached_opaque or (
                        (argument.startswith("-p") and len(argument) > 2)
                        or argument.startswith("--pattern=")
                    )
                if unittest_attached_opaque:
                    consumed.add(index)
                    index += 1
                    continue
                if python_unittest_discovery:
                    unittest_path_role = unittest_path_options.get(argument)
                    if unittest_path_role is not None:
                        consumed.add(index)
                        if index + 1 >= len(test.arguments):
                            raise RuntimeError(
                                f"Python unittest {argument} has no path for {test.name}"
                            )
                        add_input(
                            index + 1,
                            test.arguments[index + 1],
                            "",
                            unittest_path_role,
                        )
                        index += 2
                        continue
                    unittest_path_option = next(
                        (
                            option
                            for option in ("-s", "-t")
                            if argument.startswith(option)
                            and len(argument) > len(option)
                        ),
                        None,
                    )
                    unittest_path_value = ""
                    unittest_path_prefix = ""
                    if unittest_path_option is not None:
                        unittest_path_value = argument[len(unittest_path_option) :]
                        unittest_path_prefix = unittest_path_option
                        unittest_path_role = unittest_path_options[
                            unittest_path_option
                        ]
                    else:
                        long_option, equal, long_value = argument.partition("=")
                        unittest_path_role = (
                            unittest_path_options.get(long_option) if equal else None
                        )
                        if unittest_path_role is not None:
                            unittest_path_value = long_value
                            unittest_path_prefix = long_option + "="
                    if unittest_path_role is not None:
                        add_input(
                            index,
                            unittest_path_value,
                            unittest_path_prefix,
                            unittest_path_role,
                        )
                        index += 1
                        continue
            if (
                tool_kind == "python"
                and python_module_target in python_unittest_targets
                and is_unittest_selector(argument)
            ):
                consumed.add(index)
                index += 1
                continue
            if (
                tool_kind == "qmllint"
                and re.fullmatch(r"--[A-Za-z_][A-Za-z0-9_.-]*", argument)
                and index + 1 < len(test.arguments)
                and test.arguments[index + 1].casefold()
                in qmllint_diagnostic_levels
            ):
                consumed.add(index)
                consumed.add(index + 1)
                index += 2
                continue
            role = options.get(lowered)
            if role is not None:
                consumed.add(index)
                if index + 1 >= len(test.arguments):
                    raise RuntimeError(
                        f"CTest {argument} has no path for {test.name}"
                    )
                add_input(index + 1, test.arguments[index + 1], "", role)
                index += 2
                continue
            option_name, separator, option_value = argument.partition("=")
            role = options.get(option_name.casefold()) if separator else None
            if role is not None:
                add_input(
                    index,
                    option_value,
                    argument[: len(argument) - len(option_value)],
                    role,
                )
                index += 1
                continue
            if (
                tool_kind == "python"
                and argument.startswith("-")
                and not separator
                and index + 1 < len(test.arguments)
            ):
                next_value = test.arguments[index + 1]
                next_candidate = Path(next_value)
                if not next_candidate.is_absolute():
                    next_candidate = test.working_directory / next_candidate
                if next_candidate.exists() or is_untyped_path(
                    next_value, Path(next_value)
                ):
                    raise RuntimeError(
                        "unsupported CTest argument path role for "
                        f"{test.name}: {argument} {next_value}"
                    )
            suffix = Path(argument).suffix.casefold()
            positional_role: str | None = None
            if tool_kind == "qmllint" and suffix in (".qml", ".js", ".mjs"):
                positional_role = "qml-source"
            elif tool_kind == "python" and suffix == ".py":
                positional_role = "python-script"
            elif tool_kind == "python" and not argument.startswith("-"):
                positional_candidate = Path(argument)
                if not positional_candidate.is_absolute():
                    positional_candidate = (
                        test.working_directory / positional_candidate
                    )
                if positional_candidate.exists():
                    positional_role = "python-input"
            elif tool_kind == "shell" and suffix in (".sh", ".bash"):
                positional_role = "shell-script"
            if positional_role is not None:
                add_input(index, argument, "", positional_role)
                if tool_kind == "python" and not python_target_seen:
                    python_target_seen = True
            index += 1

        for index, argument in enumerate(test.arguments):
            if index in consumed:
                continue
            candidate_value = argument
            if argument.startswith("-"):
                _option, separator, candidate_value = argument.partition("=")
                if not separator:
                    _option, separator, candidate_value = argument.partition(":")
                    if not separator:
                        if not is_untyped_path(argument, Path(argument)):
                            continue
                        candidate_value = argument
            candidate = Path(candidate_value)
            if not candidate.is_absolute():
                candidate = test.working_directory / candidate
            if candidate.exists() or is_untyped_path(candidate_value, Path(candidate_value)):
                raise RuntimeError(
                    "unsupported CTest argument path role for "
                    f"{test.name}: {argument}"
                )

    return (
        {key: tuple(inputs) for key, inputs in by_test.items()},
        tuple(all_inputs),
    )


def _qml_property_paths(tests) -> tuple[Path, ...]:
    paths: set[Path] = set()
    for test in tests:
        for assignment in test.environment:
            name, separator, value = assignment.partition("=")
            if separator and name.casefold() in ("qml_import_path", "qml2_import_path"):
                paths.add(Path(os.path.abspath(value)).resolve())
        for modification in test.environment_modification:
            name, separator, operation_value = modification.partition("=")
            _operation, operation_separator, value = operation_value.partition(":")
            if (
                separator
                and operation_separator
                and name.casefold() in ("qml_import_path", "qml2_import_path")
            ):
                paths.add(Path(os.path.abspath(value)).resolve())
    return tuple(sorted(paths, key=lambda path: str(path).casefold()))


def _selected_test_records(
    graph, arguments: Sequence[_CTestArgument]
):
    """Return a safe superset for common literal CTest positive selectors."""
    if any(
        test.fixtures_setup or test.fixtures_cleanup or test.fixtures_required
        for test in graph.tests
    ):
        return graph.tests
    name_values: list[str] = []
    label_values: list[str] = []
    for argument in arguments:
        option = argument.option
        if option is None or (
            option not in _CTEST_VALUE_OPTION_ARITY
            and option not in _CTEST_OPTIONAL_PARALLEL_OPTIONS
            and option not in _CTEST_SELECTION_NEUTRAL_FLAGS
        ):
            return graph.tests
        if option in (
            "-I",
            "--tests-information",
            "-U",
            "--union",
            "--rerun-failed",
            "--tests-from-file",
            "-FA",
            "-FS",
            "-FC",
            "--fixture-exclude-any",
            "--fixture-exclude-setup",
            "--fixture-exclude-cleanup",
        ):
            return graph.tests
        target: list[str] | None = None
        if option in ("-R", "--tests-regex"):
            target = name_values
        elif option in ("-L", "--label-regex"):
            target = label_values
        if target is not None:
            if len(argument.values) != 1:
                return graph.tests
            target.append(argument.values[0])

    selected = tuple(graph.tests)
    if len(name_values) == 1:
        match = re.fullmatch(r"\^([-A-Za-z0-9_:]+)\$", name_values[0])
        if match:
            selected = tuple(
                test for test in selected if test.name == match.group(1)
            )
    if len(label_values) == 1 and re.fullmatch(
        r"[-A-Za-z0-9_:]+", label_values[0]
    ):
        literal = label_values[0]
        selected = tuple(
            test
            for test in selected
            if any(literal in label for label in test.labels)
        )
    return selected


def _validate_rendered_test_commands(
    graph,
    selected_names: frozenset[str],
    original_build: Path,
    staged_executables: frozenset[Path],
) -> None:
    """Fail closed if a runnable rendered command escaped immutable staging."""
    for test in graph.tests:
        if test.name not in selected_names:
            continue
        if not test.executable_arguments:
            raise RuntimeError(
                f"rendered CTest command has no staged executable: {test.name}"
            )
        for _argument, executable, _occurrence in test.executable_arguments:
            if executable == original_build or executable.is_relative_to(
                original_build
            ):
                raise RuntimeError(
                    "rendered CTest command still points to original build tree: "
                    f"{test.name}: {executable}"
                )
            if executable not in staged_executables:
                raise RuntimeError(
                    "rendered CTest command points to an unstaged external "
                    f"executable: {test.name}: {executable}"
                )


def _source_snapshot_exclusions(source_root: Path, build_root: Path) -> tuple[Path, ...]:
    excluded = {build_root, source_root / ".git", source_root / ".claude"}
    for name in (
        "build",
        "build-tests",
        "windows_build",
        "macos_build",
        "linux_build",
        ".venv",
        "venv",
    ):
        candidate = source_root / name
        if candidate.exists():
            excluded.add(candidate)
    return tuple(excluded)


class _ExternalToolClosure(NamedTuple):
    root: Path
    executable_relative: Path
    entries: dict[Path, Path | bytes | GeneratedLayoutEntry]


def _validated_closure_path(
    path: Path,
    selected_root: Path | None = None,
    *,
    lease: RuntimeAuditLease | None = None,
) -> Path:
    lexical = Path(os.path.abspath(path))
    if not lexical.exists():
        raise RuntimeError(f"external tool closure path does not exist: {lexical}")
    canonical = lexical.resolve()
    if os.path.normcase(str(lexical)) != os.path.normcase(str(canonical)):
        raise RuntimeError(
            f"external tool closure contains a reparse alias: {lexical} -> {canonical}"
        )
    if selected_root is not None and not canonical.is_relative_to(
        selected_root.resolve()
    ):
        raise RuntimeError(
            f"external tool closure escapes its install root: {lexical}"
        )
    if lease is not None:
        held = (
            lease.canonical_directory(lexical)
            if lexical.is_dir()
            else lease.canonical_file(lexical)
        )
        if os.path.normcase(str(lexical)) != os.path.normcase(str(held)):
            raise RuntimeError(
                f"external tool closure contains a reparse alias: {lexical} -> {held}"
            )
        canonical = held
    return canonical


def _tree_closure_files(
    root: Path,
    selected_root: Path,
    *,
    lease: RuntimeAuditLease | None = None,
    entry_limit: int | None = None,
    byte_limit: int | None = None,
) -> tuple[Path, ...]:
    for label, limit in (("entry", entry_limit), ("byte", byte_limit)):
        if limit is not None and (type(limit) is not int or limit < 0):
            raise RuntimeError(
                f"external tool closure {label} limit must be a "
                "nonnegative integer"
            )
    root = _validated_closure_path(root, selected_root, lease=lease)
    if root.is_file():
        if entry_limit is not None and entry_limit < 1:
            raise RuntimeError("external tool closure entry limit exceeded")
        size = int(
            os.fstat(lease.hold(root).fileno()).st_size
            if lease is not None
            else root.stat().st_size
        )
        if byte_limit is not None and size > byte_limit:
            raise RuntimeError("external tool closure byte limit exceeded")
        return (root,)
    files: list[Path] = []
    pending = [root]
    traversed_entries = 0
    traversed_bytes = 0
    while pending:
        current = pending.pop()
        with os.scandir(current) as children:
            for directory_entry in children:
                if (
                    entry_limit is not None
                    and traversed_entries >= entry_limit
                ):
                    raise RuntimeError(
                        "external tool closure entry limit exceeded"
                    )
                traversed_entries += 1
                child = _validated_closure_path(
                    current / directory_entry.name,
                    selected_root,
                    lease=lease,
                )
                if child.is_dir():
                    pending.append(child)
                    continue
                size = int(
                    os.fstat(lease.hold(child).fileno()).st_size
                    if lease is not None
                    else child.stat().st_size
                )
                if (
                    byte_limit is not None
                    and traversed_bytes + size > byte_limit
                ):
                    raise RuntimeError(
                        "external tool closure byte limit exceeded"
                    )
                traversed_bytes += size
                files.append(child)
    return tuple(files)


def _python_base_entries(
    base: Path,
    prefix: Path,
    seeds: Sequence[Path],
    *,
    lease: RuntimeAuditLease | None = None,
) \
        -> dict[Path, Path | bytes | GeneratedLayoutEntry]:
    base = _validated_closure_path(base, lease=lease)
    entries: dict[Path, Path | bytes | GeneratedLayoutEntry] = {}
    import_seeds = list(seeds)
    for child in base.iterdir():
        if child.is_file() and (
            child.name.casefold().startswith("python")
            and child.suffix.casefold() in (".dll", ".zip")
        ):
            validated = _validated_closure_path(child, base, lease=lease)
            entries[prefix / validated.relative_to(base)] = validated
            if validated.suffix.casefold() == ".dll":
                import_seeds.append(validated)
    dlls = base / "DLLs"
    runtime_directories = [base]
    if dlls.is_dir():
        runtime_directories.append(
            _validated_closure_path(dlls, base, lease=lease)
        )
        for child in _tree_closure_files(dlls, base, lease=lease):
            entries[prefix / child.relative_to(base)] = child
            if child.suffix.casefold() in (".dll", ".exe", ".pyd"):
                import_seeds.append(child)
    for dependency in transitive_runtime_closure(
        import_seeds, tuple(runtime_directories)
    ):
        entries[prefix / dependency.relative_to(base)] = dependency
    library = base / "Lib"
    excluded = {
        "__pycache__", "ensurepip", "idlelib", "site-packages", "test",
        "tests", "tkinter", "turtledemo",
    }
    if library.is_dir():
        library = _validated_closure_path(library, base, lease=lease)
        for current_text, directory_names, file_names in os.walk(library):
            current = _validated_closure_path(
                Path(current_text), base, lease=lease
            )
            retained_directories: list[str] = []
            for name in directory_names:
                if name.casefold() in excluded:
                    continue
                retained_directories.append(
                    _validated_closure_path(
                        current / name, base, lease=lease
                    ).name
                )
            directory_names[:] = retained_directories
            for name in file_names:
                child = _validated_closure_path(
                    current / name, base, lease=lease
                )
                entries[prefix / child.relative_to(base)] = child
    return entries


def _external_tool_closure(
    executable: Path,
    *,
    lease: RuntimeAuditLease | None = None,
) -> _ExternalToolClosure:
    """Return a bounded, explicit install-relative closure for an external tool."""
    executable = _validated_closure_path(executable, lease=lease)
    venv_root = executable.parent.parent
    venv_config = venv_root / "pyvenv.cfg"
    is_python = executable.stem.casefold().startswith("python")
    is_venv = (
        is_python
        and executable.parent.name.casefold() == "scripts"
        and venv_config.is_file()
    )
    git_root = executable.parent.parent
    git_usr_bin = git_root / "usr" / "bin"
    is_git_for_windows_bash = (
        executable.name.casefold() == "bash.exe"
        and executable.parent.name.casefold() == "bin"
        and (git_usr_bin / "bash.exe").is_file()
        and (git_usr_bin / "msys-2.0.dll").is_file()
    )
    root = _validated_closure_path(
        venv_root
        if is_venv
        else git_root
        if is_git_for_windows_bash
        else executable.parent,
        lease=lease,
    )
    executable_relative = executable.relative_to(root)
    entries: dict[Path, Path | bytes | GeneratedLayoutEntry] = {
        executable_relative: executable
    }

    if lease is not None and not is_python:
        lease.hold_dll_tree(executable.parent)
    imported_runtime = (
        set()
        if is_python
        else set(transitive_runtime_closure((executable,), (executable.parent,)))
    )
    for adjacent in sorted(
        executable.parent.iterdir(), key=lambda path: path.name.casefold()
    ):
        if not adjacent.is_file():
            continue
        is_manifest = adjacent.name.casefold() == (
            executable.name + ".manifest"
        ).casefold()
        if adjacent.suffix.casefold() != ".dll" and not is_manifest:
            continue
        validated = _validated_closure_path(adjacent, root, lease=lease)
        family = _controlled_family(adjacent.name)
        is_tool_local_dynamic_family = (
            family is not None and family[0] != "Qt6_DIR"
        )
        if (
            not is_python
            and not is_manifest
            and validated not in imported_runtime
            and not is_tool_local_dynamic_family
        ):
            continue
        entries[validated.relative_to(root)] = validated

    if is_git_for_windows_bash:
        for candidate in _tree_closure_files(
            git_usr_bin,
            root,
            lease=lease,
            entry_limit=4096,
            byte_limit=256 << 20,
        ):
            entries[candidate.relative_to(root)] = candidate
        # Git for Windows' bin/bash.exe wrapper accepts the install root only
        # when the sibling mingw64/bin directory exists, before it dispatches
        # to usr/bin/bash.exe.  The directory is the marker; no MinGW payload
        # participates in these shell-script tests.
        entries[Path("mingw64/bin/.olr-runtime-root")] = b""

    if is_python:
        if is_venv:
            config = _validated_closure_path(venv_config, root, lease=lease)
            if lease is None:
                encoded_config = config.read_bytes()
                config_identity = config
            else:
                stream = lease.hold(config)
                stream.seek(0)
                encoded_config = stream.read()
                stream.seek(0)
                config_identity = lease.canonical_file(config)
            contents = encoded_config.decode("utf-8")
            match = re.search(r"(?im)^\s*home\s*=\s*(.+?)\s*$", contents)
            if match is None:
                raise RuntimeError(f"Python venv has no home entry: {config}")
            base = _validated_closure_path(Path(match.group(1)), lease=lease)
            entries.update(
                _python_base_entries(
                    base, Path("base"), (executable,), lease=lease
                )
            )

            def rewrite_venv_config(staged: Path, text: str = contents) -> bytes:
                home = (staged / "base").as_posix()
                return re.sub(
                    r"(?im)^(\s*home\s*=\s*).+?$",
                    lambda found: found.group(1) + home,
                    text,
                    count=1,
                ).encode("utf-8")

            entries[Path("pyvenv.cfg")] = GeneratedLayoutEntry(
                "python-venv-config-rewrite-v2",
                {
                    "source_final_path": config_identity.as_posix(),
                    "source_sha256": hashlib.sha256(encoded_config).hexdigest(),
                },
                rewrite_venv_config,
            )
        else:
            entries.update(
                _python_base_entries(
                    root, Path("."), (executable,), lease=lease
                )
            )

    descriptor = executable.with_name(executable.name + ".olr-runtime-files")
    if descriptor.is_file():
        descriptor = _validated_closure_path(descriptor, root, lease=lease)
        entries[descriptor.relative_to(root)] = descriptor
        for line in descriptor.read_text(encoding="utf-8").splitlines():
            value = line.strip()
            if not value or value.startswith("#"):
                continue
            relative = Path(value)
            if (
                relative.is_absolute()
                or bool(relative.anchor)
                or bool(relative.drive)
                or ".." in relative.parts
            ):
                raise RuntimeError(
                    f"external tool closure has unsafe path {value!r}: {descriptor}"
                )
            closure_root = _validated_closure_path(
                root / relative, root, lease=lease
            )
            for candidate in _tree_closure_files(
                closure_root, root, lease=lease
            ):
                entries[candidate.relative_to(root)] = candidate
    return _ExternalToolClosure(root, executable_relative, entries)


class _CTestArgument(NamedTuple):
    tokens: tuple[str, ...]
    option: str | None
    values: tuple[str, ...]
    attached_prefix: str | None = None


# Repetition stays useful for flakes without permitting accidental soak runs.
_CTEST_REPEAT_CAP = 16
# Never let CTest's native/unbounded zero mode oversubscribe a shared host.
_CTEST_PARALLEL_CAP = max(1, min(16, os.cpu_count() or 1))
_CTEST_OPTIONAL_PARALLEL_OPTIONS = frozenset({"-j", "--parallel"})
_CTEST_SELECTION_NEUTRAL_FLAGS = frozenset(
    {
        "--debug",
        "--extra-verbose",
        "--failover",
        "--label-summary",
        "--no-compress-output",
        "--output-on-failure",
        "--print-labels",
        "--progress",
        "--quiet",
        "--schedule-random",
        "--show-only",
        "--stop-on-failure",
        "--subproject-summary",
        "--verbose",
        "-F",
        "-N",
        "-Q",
        "-U",
        "-V",
        "-VV",
    }
)


_CTEST_VALUE_OPTION_ARITY = {
    "--build-and-test": 2,
    "--build-config": 1,
    "--build-generator": 1,
    "--build-makeprogram": 1,
    "--build-noclean": 0,
    "--build-options": 1,
    "--build-project": 1,
    "--build-target": 1,
    "--build-two-config": 0,
    "--collect-instrumentation": 1,
    "--dashboard": 1,
    "--exclude-from-file": 1,
    "--exclude-regex": 1,
    "--extra-submit": 1,
    "--fixture-exclude-any": 1,
    "--fixture-exclude-cleanup": 1,
    "--fixture-exclude-setup": 1,
    "--interactive-debug-mode": 1,
    "--label-exclude": 1,
    "--label-regex": 1,
    "--max-width": 1,
    "--no-tests": 1,
    "--output-junit": 1,
    "--output-log": 1,
    "--overwrite": 1,
    "--preset": 1,
    "--presets-file": 1,
    "--repeat": 1,
    "--repeat-until-fail": 1,
    "--resource-spec-file": 1,
    "--schedule-random-seed": 1,
    "--script": 1,
    "--script-new-process": 1,
    "--stop-time": 1,
    "--submit-index": 1,
    "--test-action": 1,
    "--test-command": 1,
    "--test-dir": 1,
    "--test-load": 1,
    "--test-model": 1,
    "--test-output-size-failed": 1,
    "--test-output-size-passed": 1,
    "--test-output-truncation": 1,
    "--tests-from-file": 1,
    "--tests-information": 1,
    "--tests-regex": 1,
    "--timeout": 1,
    "-C": 1,
    "-D": 1,
    "-E": 1,
    "-FA": 1,
    "-FC": 1,
    "-FS": 1,
    "-I": 1,
    "-L": 1,
    "-LE": 1,
    "-M": 1,
    "-O": 1,
    "-R": 1,
    "-S": 1,
    "-SP": 1,
    "-T": 1,
}

_CTEST_SHORT_VALUE_OPTIONS = tuple(
    sorted(
        (
            option
            for option, arity in _CTEST_VALUE_OPTION_ARITY.items()
            if arity == 1 and option.startswith("-") and not option.startswith("--")
        ),
        key=len,
        reverse=True,
    )
)

_CTEST_UNSAFE_OPTIONS = frozenset(
    {
        "--",
        "--build-and-test",
        "--build-options",
        "--collect-instrumentation",
        "--dashboard",
        "--extra-submit",
        "--list-presets",
        "--output-junit",
        "--output-log",
        "--overwrite",
        "--preset",
        "--presets-file",
        "--rerun-failed",
        "--script",
        "--script-new-process",
        "--submit-index",
        "--test-action",
        "--test-command",
        "--test-model",
        "-D",
        "-M",
        "-O",
        "-S",
        "-SP",
        "-T",
    }
)


def _parse_ctest_arguments(args: Sequence[str]) -> tuple[_CTestArgument, ...]:
    """Split the outer CTest CLI while honoring documented option arity."""
    parsed: list[_CTestArgument] = []
    index = 0
    while index < len(args):
        token = args[index]
        if token in _CTEST_OPTIONAL_PARALLEL_OPTIONS:
            stop = index + 1
            if stop < len(args) and re.fullmatch(r"[0-9]+", args[stop]):
                stop += 1
            tokens = tuple(args[index:stop])
            parsed.append(_CTestArgument(tokens, token, tokens[1:]))
            index = stop
            continue

        arity = _CTEST_VALUE_OPTION_ARITY.get(token)
        if arity is not None:
            stop = min(len(args), index + 1 + arity)
            tokens = tuple(args[index:stop])
            parsed.append(_CTestArgument(tokens, token, tokens[1:]))
            index = stop
            continue

        if token.startswith("--") and "=" in token:
            option, _, value = token.partition("=")
            parsed.append(_CTestArgument((token,), option, (value,), option + "="))
            index += 1
            continue

        if token.startswith("-j") and token != "-j":
            value = token[2:]
            prefix = "-j"
            if value.startswith("="):
                value = value[1:]
                prefix += "="
            parsed.append(_CTestArgument((token,), "-j", (value,), prefix))
            index += 1
            continue

        attached_option = next(
            (
                option
                for option in _CTEST_SHORT_VALUE_OPTIONS
                if token.startswith(option) and token != option
            ),
            None,
        )
        if attached_option is not None:
            value = token[len(attached_option) :]
            prefix = attached_option
            if value.startswith("="):
                value = value[1:]
                prefix += "="
            parsed.append(
                _CTestArgument((token,), attached_option, (value,), prefix)
            )
            index += 1
            continue

        parsed.append(
            _CTestArgument((token,), token if token.startswith("-") else None, ())
        )
        index += 1
    return tuple(parsed)


def _unsafe_ctest_option(option: str | None) -> bool:
    return option in _CTEST_UNSAFE_OPTIONS or bool(
        option and option.startswith("--build-")
    )


def find_build_directory(args: Sequence[str], cwd: Path) -> Path:
    """Return the build directory selected by CTest's ``--test-dir`` option."""
    selected: str | None = None
    for argument in _parse_ctest_arguments(args):
        if _unsafe_ctest_option(argument.option):
            raise RuntimeError(
                "unsupported controlled CTest option has no immutable semantics: "
                f"{argument.tokens[0]}"
            )
        if argument.option != "--test-dir":
            continue
        if len(argument.values) != 1 or not argument.values[0]:
            raise ValueError("--test-dir requires a build directory")
        selected = argument.values[0]
    return (cwd / selected).resolve() if selected is not None else cwd.resolve()


def _stage_controlled_ctest_arguments(
    args: Sequence[str],
    lease: RuntimeAuditLease,
    *,
    parsed_arguments: Sequence[_CTestArgument] | None = None,
) -> tuple[str, ...]:
    """Normalize CTest mode selection and snapshot every supported input file."""
    file_options = {
        "--exclude-from-file",
        "--resource-spec-file",
        "--tests-from-file",
    }
    information_options = {"-I", "--tests-information"}
    rewritten: list[str] = []
    staged_entries: dict[Path, Path] = {}
    staged_occurrences: list[tuple[int, str, Path]] = []
    repeat_count = 0
    parallel_count = 0

    def add_input(value: str, option: str, index: int, prefix: str) -> None:
        if not value:
            raise RuntimeError(f"controlled CTest option {option} requires a file")
        lexical = Path(os.path.abspath(value))
        if not lexical.is_file():
            raise RuntimeError(
                f"controlled CTest input file does not exist for {option}: {lexical}"
            )
        canonical = lease.canonical_file(lexical)
        if os.path.normcase(str(lexical)) != os.path.normcase(str(canonical)):
            raise RuntimeError(
                f"controlled CTest input file is a reparse alias for {option}: "
                f"{lexical} -> {canonical}"
            )
        destination = Path(f"{len(staged_entries):04d}-{lexical.name}")
        staged_entries[destination] = lexical
        staged_occurrences.append((index, prefix, destination))

    parsed_sequence = tuple(
        _parse_ctest_arguments(args)
        if parsed_arguments is None
        else parsed_arguments
    )
    if tuple(token for item in parsed_sequence for token in item.tokens) != tuple(args):
        raise RuntimeError("controlled CTest parse does not match caller arguments")

    for parsed in parsed_sequence:
        argument = parsed.tokens[0]
        option = parsed.option
        if _unsafe_ctest_option(option):
            raise RuntimeError(
                f"unsupported controlled CTest option has no immutable semantics: {argument}"
            )
        if option in ("--repeat", "--repeat-until-fail"):
            repeat_count += 1
            if repeat_count != 1 or len(parsed.values) != 1:
                raise RuntimeError(
                    "controlled CTest repeat must be one bounded until-fail count"
                )
            value = parsed.values[0]
            if option == "--repeat":
                match = re.fullmatch(r"until-fail:([0-9]+)", value)
                count = int(match.group(1)) if match is not None else 0
            else:
                count = int(value) if re.fullmatch(r"[0-9]+", value) else 0
            if count < 1 or count > _CTEST_REPEAT_CAP:
                raise RuntimeError(
                    "controlled CTest repeat requires an until-fail count "
                    f"between 1 and {_CTEST_REPEAT_CAP}"
                )
        if option in _CTEST_OPTIONAL_PARALLEL_OPTIONS:
            parallel_count += 1
            value = parsed.values[0] if len(parsed.values) == 1 else ""
            count = int(value) if re.fullmatch(r"[0-9]+", value) else 0
            if (
                parallel_count != 1
                or count < 1
                or count > _CTEST_PARALLEL_CAP
            ):
                raise RuntimeError(
                    "controlled CTest parallelism requires one explicit positive "
                    f"value between 1 and {_CTEST_PARALLEL_CAP}"
                )
        if option == "--test-dir":
            if len(parsed.values) != 1 or not parsed.values[0]:
                raise ValueError("--test-dir requires a build directory")
            continue

        if option in file_options or option in information_options:
            if len(parsed.values) != 1:
                raise RuntimeError(
                    f"controlled CTest option {option} requires a value"
                )
            value = parsed.values[0]
            rewritten.extend(parsed.tokens)
            if option in file_options or not re.fullmatch(r"[0-9,]+", value):
                if parsed.attached_prefix is None:
                    occurrence = len(rewritten) - 1
                    prefix = ""
                else:
                    occurrence = len(rewritten) - 1
                    prefix = parsed.attached_prefix
                add_input(value, option, occurrence, prefix)
            continue

        rewritten.extend(parsed.tokens)

    if staged_entries:
        layout = lease.stage_readonly_layout(
            "ctest-inputs",
            staged_entries,
            context={"policy": "ctest-input-files-v1"},
            entry_limit=256,
            byte_limit=64 << 20,
        )
        for occurrence, prefix, destination in staged_occurrences:
            rewritten[occurrence] = prefix + str(layout / destination)
    return tuple(rewritten)


def read_cmake_cache(path: Path) -> dict[str, str]:
    """Parse typed CMake cache entries as data without evaluating their values."""
    return {key: entry.value for key, entry in parse_cmake_cache(path).items()}


def prepare_windows_runtime(
    build_dir: Path,
    inherited_env: Mapping[str, str],
    *,
    audit_lease: RuntimeAuditLease | None = None,
    ctest_args: Sequence[str] = (),
) -> tuple[dict[str, str], Path]:
    """Create a child environment using runtime paths recorded by the build."""
    owned = audit_lease is None
    lease = RuntimeAuditLease() if audit_lease is None else audit_lease
    try:
        cache_path = build_dir / "CMakeCache.txt"
        cache_descriptor = lease._describe_held_source(cache_path)
        cache_descriptor.stream.seek(0)
        cache_payload = cache_descriptor.stream.read()
        cache_descriptor.stream.seek(0)
        schema = {
            "CMAKE_CXX_COMPILER": ("FILEPATH", "UNINITIALIZED"),
            "CMAKE_COMMAND": ("INTERNAL", "FILEPATH"),
            "Qt6_DIR": ("PATH",),
            "OLR_FFMPEG_ROOT": ("PATH",),
            "OLR_SRT_ROOT": ("PATH",),
            "CMAKE_HOME_DIRECTORY": ("INTERNAL", "PATH"),
        }
        cache = require_cache_entries(
            cache_path, schema, absolute_paths=tuple(schema)
        )
        all_entries = parse_cmake_cache(cache_path)
        ctest_entry = all_entries.get("CMAKE_CTEST_COMMAND")
        if ctest_entry is not None and ctest_entry.cache_type not in (
            "INTERNAL",
            "FILEPATH",
        ):
            raise ValueError(
                "CMake cache entry CMAKE_CTEST_COMMAND has wrong type: "
                f"{ctest_entry.cache_type}"
            )
        if ctest_entry is not None and not Path(ctest_entry.value).is_absolute():
            raise ValueError("CMAKE_CTEST_COMMAND must be an absolute path")

        def canonical_file(label: str, value: Path, cache_key: str) -> Path:
            try:
                return lease.canonical_file(value)
            except OSError as error:
                raise FileNotFoundError(
                    f"{label} from {cache_key} does not exist: {value}; "
                    "reconfigure or recreate the selected build directory"
                ) from error

        def canonical_directory(label: str, value: Path, cache_key: str) -> Path:
            try:
                return lease.canonical_directory(value)
            except OSError as error:
                raise FileNotFoundError(
                    f"{label} directory from {cache_key} does not exist: {value}; "
                    "reconfigure or recreate the selected build directory"
                ) from error

        compiler = canonical_file(
            "compiler runtime executable", Path(cache["CMAKE_CXX_COMPILER"]), "CMAKE_CXX_COMPILER"
        )
        cmake = canonical_file(
            "CMake executable", Path(cache["CMAKE_COMMAND"]), "CMAKE_COMMAND"
        )
        qt_config = canonical_directory(
            "Qt configuration", Path(cache["Qt6_DIR"]), "Qt6_DIR"
        )
        qt_root = qt_config.parents[2]
        ffmpeg_root = canonical_directory(
            "FFmpeg root", Path(cache["OLR_FFMPEG_ROOT"]), "OLR_FFMPEG_ROOT"
        )
        srt_root = canonical_directory(
            "SRT root", Path(cache["OLR_SRT_ROOT"]), "OLR_SRT_ROOT"
        )
        source_root = canonical_directory(
            "source root", Path(cache["CMAKE_HOME_DIRECTORY"]), "CMAKE_HOME_DIRECTORY"
        )
        canonical_build = canonical_directory("build", build_dir, "--test-dir")
        ctest = canonical_file(
            "CTest executable",
            Path(ctest_entry.value) if ctest_entry else cmake.with_name("ctest.exe"),
            "CMAKE_CTEST_COMMAND",
        )

        runtime_dirs = (
            canonical_directory("compiler runtime", compiler.parent, "CMAKE_CXX_COMPILER"),
            canonical_directory("Qt runtime", qt_root / "bin", "Qt6_DIR"),
            canonical_directory("FFmpeg runtime", ffmpeg_root / "bin", "OLR_FFMPEG_ROOT"),
            canonical_directory("FFmpeg libraries", ffmpeg_root / "lib", "OLR_FFMPEG_ROOT"),
            canonical_directory("SRT runtime", srt_root / "bin", "OLR_SRT_ROOT"),
            canonical_directory("SRT libraries", srt_root / "lib", "OLR_SRT_ROOT"),
        )
        qt_plugins = canonical_directory("Qt plugins", qt_root / "plugins", "Qt6_DIR")
        qt_platform_plugins = canonical_directory(
            "Qt platform plugins", qt_plugins / "platforms", "Qt6_DIR"
        )
        qt_qml = canonical_directory("Qt QML imports", qt_root / "qml", "Qt6_DIR")
        for directory in (*runtime_dirs, qt_plugins, qt_qml):
            lease.hold_dll_tree(directory)
        launch_qml = qt_qml

        graph = discover_ctest_graph(canonical_build, lease=lease)
        validate_ctest_runtime_properties(
            graph,
            runtime_dirs=runtime_dirs,
            qt_plugins=qt_plugins,
            qt_platform_plugins=qt_platform_plugins,
            ffmpeg_root=ffmpeg_root,
            srt_root=srt_root,
            qt_qml=qt_qml,
            source_root=source_root,
            build_root=canonical_build,
        )
        roots = RuntimeRoots(*runtime_dirs)
        application_runtime_executables = tuple(
            executable
            for executable in graph.executables
            if executable.is_relative_to(canonical_build)
        )
        reject_runtime_dll_shadowing(
            canonical_build,
            roots,
            executables=(*application_runtime_executables, ctest),
            lease=lease,
            require_embedded_manifests=os.name == "nt",
        )
        for working_directory in graph.working_directories:
            reject_runtime_dll_shadowing(
                working_directory,
                roots,
                lease=lease,
            )

        launch_build = canonical_build
        launch_runtime_dirs = runtime_dirs
        launch_plugins = qt_plugins
        launch_platform_plugins = qt_platform_plugins
        launch_qml = qt_qml
        launch_ctest = ctest
        if audit_lease is not None and os.name == "nt":
            parsed_ctest_args = _parse_ctest_arguments(ctest_args)
            controlled_ctest_args = _stage_controlled_ctest_arguments(
                ctest_args,
                lease,
                parsed_arguments=parsed_ctest_args,
            )
            lease.launch_ctest_arguments = controlled_ctest_args
            selected_tests = _selected_test_records(graph, parsed_ctest_args)
            missing_executables = tuple(
                test.name
                for test in selected_tests
                if not test.executable_arguments
            )
            if missing_executables:
                raise RuntimeError(
                    "selected CTest tests do not resolve an existing held "
                    "executable or interpreter: " + ", ".join(missing_executables)
                )
            quicktest_inputs_by_test, quicktest_inputs = _quicktest_inputs(
                selected_tests, source_root, canonical_build
            )
            declared_inputs_by_test, declared_inputs = _declared_test_path_inputs(
                selected_tests
            )
            declared_build_roots = {
                item.path for item in declared_inputs if item.role == "build-root"
            }
            if any(path != canonical_build for path in declared_build_roots):
                raise RuntimeError(
                    "declared CTest build root must match the selected build directory"
                )
            needs_build_registration_contract = bool(declared_build_roots)
            build_inputs = tuple(
                path
                for path in quicktest_inputs
                if path == canonical_build or path.is_relative_to(canonical_build)
            )
            if build_inputs:
                raise RuntimeError(
                    "QuickTest -input inside the mutable build tree is unsupported; "
                    "generate the input below the source root or embed it as a resource"
                )

            launch_source = source_root
            source_dependencies: list[Path] = []
            source_declared_inputs = tuple(
                item
                for item in declared_inputs
                if item.path == source_root or item.path.is_relative_to(source_root)
                if not (
                    item.path == canonical_build
                    or item.path.is_relative_to(canonical_build)
                )
                if not (
                    item.path == qt_qml
                    or item.path.is_relative_to(qt_qml)
                    or item.path == qt_plugins
                    or item.path.is_relative_to(qt_plugins)
                    or any(
                        item.path == runtime_dir
                        or item.path.is_relative_to(runtime_dir)
                        for runtime_dir in runtime_dirs
                    )
                )
            )
            if quicktest_inputs or source_declared_inputs:
                launch_source = lease.stage_readonly_tree(
                    source_root,
                    entry_limit=32768,
                    byte_limit=512 << 20,
                    excluded=_source_snapshot_exclusions(
                        source_root, canonical_build
                    ),
                )
                source_dependencies.append(launch_source)
            lease.launch_source_directory = launch_source

            selected_executables = {
                executable
                for test in selected_tests
                for _argument, executable, _occurrence in test.executable_arguments
            }
            qt_payload_names = {
                "openlivereplay.exe",
                "qmllint.exe",
            }
            needs_project_qml = (
                bool(quicktest_inputs)
                or any(
                    item.role in ("qml-source", "qml-import")
                    for item in declared_inputs
                )
                or any(
                    executable.name.casefold() in qt_payload_names
                    for executable in selected_executables
                )
            )
            uses_qmllint = any(
                executable.name.casefold() == "qmllint.exe"
                for executable in selected_executables
            )
            plugin_families = (
                (
                    "iconengines",
                    "imageformats",
                    "multimedia",
                    "networkinformation",
                    "styles",
                    "tls",
                )
                if needs_project_qml
                else ()
            ) + (("qmllint",) if uses_qmllint else ())
            plugin_files = qt_plugin_closure(
                qt_plugins, plugin_families, lease=lease
            )
            plugin_entries: dict[Path, Path | bytes] = {
                path.relative_to(qt_plugins): path for path in plugin_files
            }
            plugin_entries.setdefault(
                Path("platforms") / ".closed", b"closed plugin namespace\n"
            )
            launch_plugins = lease.stage_readonly_layout(
                "qt-plugins",
                plugin_entries,
                context={
                    "policy": "ctest-qt-plugins-v2",
                    "families": plugin_families,
                    "platform_policy": ("qwindows.dll", "qoffscreen.dll"),
                },
                byte_limit=256 << 20,
            )
            launch_platform_plugins = launch_plugins / qt_platform_plugins.relative_to(
                qt_plugins
            )
            qml_files = qt_qml_closure(
                qt_qml,
                (source_root,) if needs_project_qml else (),
                lease=lease,
            )
            qml_entries: dict[Path, Path | bytes] = {
                path.relative_to(qt_qml): path for path in qml_files
            }
            qml_entries.setdefault(Path(".closed"), b"closed QML namespace\n")
            launch_qml = lease.stage_readonly_layout(
                "qt-qml",
                qml_entries,
                context={
                    "policy": "ctest-qt-qml-v2",
                    "source_root": source_root.as_posix(),
                    "project_qml": needs_project_qml,
                },
                byte_limit=512 << 20,
            )

            qml_replacements: list[tuple[Path, Path]] = [
                (qt_plugins, launch_plugins),
                (qt_platform_plugins, launch_platform_plugins),
                (qt_qml, launch_qml),
            ]
            if launch_source != source_root:
                qml_replacements.extend(
                    (
                        qml_input,
                        launch_source / qml_input.relative_to(source_root),
                    )
                    for qml_input in quicktest_inputs
                )
            for qml_path in _qml_property_paths(selected_tests):
                if qml_path == qt_qml or qml_path.is_relative_to(qt_qml):
                    continue
                if qml_path == canonical_build or qml_path.is_relative_to(
                    canonical_build
                ):
                    staged_qml_path = lease.stage_readonly_tree(
                        qml_path,
                        entry_limit=16384,
                        byte_limit=512 << 20,
                    )
                    source_dependencies.append(staged_qml_path)
                    qml_replacements.append((qml_path, staged_qml_path))
                    continue
                if qml_path == source_root or qml_path.is_relative_to(source_root):
                    if launch_source == source_root:
                        raise RuntimeError(
                            "CTest QML import path reaches mutable source without a "
                            "QuickTest -input snapshot"
                        )
                    qml_replacements.append(
                        (
                            qml_path,
                            launch_source / qml_path.relative_to(source_root),
                        )
                    )
                    continue
                raise RuntimeError(
                    f"unsupported CTest QML import path: {qml_path}"
                )

            declared_input_replacements: dict[
                tuple[Path, int, int], Path | None
            ] = {}
            staged_declared_paths: dict[Path, Path | None] = {}
            for item in declared_inputs:
                source = item.path
                if source in staged_declared_paths:
                    replacement = staged_declared_paths[source]
                elif source == canonical_build:
                    replacement = None
                elif source.is_relative_to(canonical_build):
                    if source.is_dir():
                        replacement = lease.stage_readonly_tree(
                            source,
                            entry_limit=16384,
                            byte_limit=512 << 20,
                        )
                    else:
                        replacement = lease.stage_readonly_layout(
                            f"ctest-input-{len(staged_declared_paths)}",
                            {Path(source.name): source},
                            context={
                                "policy": "ctest-declared-input-v1",
                                "source": source.as_posix(),
                                "role": item.role,
                            },
                            byte_limit=512 << 20,
                        ) / source.name
                    source_dependencies.append(
                        replacement if replacement.is_dir() else replacement.parent
                    )
                elif source == qt_qml or source.is_relative_to(qt_qml):
                    replacement = launch_qml / source.relative_to(qt_qml)
                elif source == qt_plugins or source.is_relative_to(qt_plugins):
                    replacement = launch_plugins / source.relative_to(qt_plugins)
                elif any(
                    source == runtime_dir or source.is_relative_to(runtime_dir)
                    for runtime_dir in runtime_dirs
                ):
                    replacement = None
                elif source == source_root or source.is_relative_to(source_root):
                    replacement = launch_source / source.relative_to(source_root)
                else:
                    raise RuntimeError(
                        "declared CTest path is outside approved immutable roots: "
                        f"{source} ({item.role})"
                    )
                if item.role == "qml-import" and replacement is not None:
                    existing_qml_replacement = next(
                        (
                            destination
                            for candidate, destination in qml_replacements
                            if candidate == source
                        ),
                        None,
                    )
                    if (
                        existing_qml_replacement is not None
                        and existing_qml_replacement != replacement
                    ):
                        raise RuntimeError(
                            "conflicting staged CTest QML import paths: "
                            f"{source} -> {existing_qml_replacement} / "
                            f"{replacement}"
                        )
                    if existing_qml_replacement is None:
                        qml_replacements.append((source, replacement))
                staged_declared_paths[source] = replacement
                declared_input_replacements[
                    (
                        item.definition_file,
                        item.occurrence.start,
                        item.occurrence.end,
                    )
                ] = replacement

            runtime_entries: dict[Path, Path | bytes] = {
                Path("loader") / ".closed": b"closed loader namespace\n"
            }
            runtime_relative_dirs = (
                Path("mingw") / "bin",
                Path("qt") / "bin",
                Path("ffmpeg") / "bin",
                Path("ffmpeg") / "lib",
                Path("srt") / "bin",
                Path("srt") / "lib",
            )
            for relative_directory in runtime_relative_dirs:
                runtime_entries[relative_directory / ".closed"] = (
                    b"closed runtime namespace\n"
                )
            if needs_build_registration_contract:
                runtime_entries[runtime_relative_dirs[0] / compiler.name] = compiler
            runtime_directory_set = set(runtime_dirs)
            build_selected_executables = {
                executable
                for executable in selected_executables
                if executable.is_relative_to(canonical_build)
            }
            configured_runtime_names = {
                child.name.casefold()
                for directory in runtime_dirs
                for child in directory.iterdir()
                if child.is_file() and child.suffix.casefold() == ".dll"
            }
            adjacent_dependency_seeds = {
                adjacent
                for executable in build_selected_executables
                if executable.parent not in runtime_directory_set
                for adjacent in executable.parent.glob("*.dll")
                if adjacent.is_file()
                and adjacent.name.casefold() not in configured_runtime_names
            }
            runtime_closure = set(
                transitive_runtime_closure(
                    (
                        *build_selected_executables,
                        *adjacent_dependency_seeds,
                        *plugin_files,
                        *(path for path in qml_files if path.suffix.casefold() == ".dll"),
                    ),
                    runtime_dirs,
                )
            )
            for directory, relative_directory in zip(
                runtime_dirs, runtime_relative_dirs, strict=True
            ):
                for child in sorted(
                    directory.iterdir(), key=lambda path: path.name.casefold()
                ):
                    if not child.is_file() or child not in runtime_closure:
                        continue
                    destination = relative_directory / child.name
                    runtime_entries[destination] = child
            runtime_layout = lease.stage_readonly_layout(
                "runtime",
                runtime_entries,
                context={
                    "policy": "ctest-runtime-v2",
                    "runtime_roots": tuple(
                        directory.as_posix() for directory in runtime_dirs
                    ),
                    "relative_directories": tuple(
                        path.as_posix() for path in runtime_relative_dirs
                    ),
                    "selected_executables": tuple(
                        sorted(path.as_posix() for path in selected_executables)
                    ),
                },
                dependencies=(launch_plugins, launch_qml),
                qt_paths=QtConfPaths(launch_plugins, launch_qml),
                byte_limit=2 << 30,
            )
            launch_runtime = runtime_layout / "loader"
            launch_runtime_dirs = tuple(
                runtime_layout / path for path in runtime_relative_dirs
            )
            lease.launch_runtime_directory = launch_runtime
            lease.launch_runtime_directories = launch_runtime_dirs
            for item in declared_inputs:
                key = (
                    item.definition_file,
                    item.occurrence.start,
                    item.occurrence.end,
                )
                if declared_input_replacements.get(key) is not None:
                    continue
                for runtime_dir, staged_runtime_dir in zip(
                    runtime_dirs, launch_runtime_dirs, strict=True
                ):
                    if item.path == runtime_dir or item.path.is_relative_to(runtime_dir):
                        declared_input_replacements[key] = (
                            staged_runtime_dir / item.path.relative_to(runtime_dir)
                        )
                        break
            ctest_entries: dict[Path, Path] = {Path(ctest.name): ctest}
            for adjacent in sorted(
                ctest.parent.iterdir(), key=lambda path: path.name.casefold()
            ):
                if adjacent == ctest or not adjacent.is_file():
                    continue
                if adjacent.suffix.casefold() == ".dll" or adjacent.name.casefold() == (
                    ctest.name + ".manifest"
                ).casefold():
                    ctest_entries[Path(adjacent.name)] = adjacent
            ctest_layout = lease.stage_readonly_layout(
                "ctest-tool",
                ctest_entries,
                context={
                    "policy": "ctest-tool-v2",
                    "ctest": ctest.as_posix(),
                },
                dependencies=(runtime_layout, launch_plugins, launch_qml),
                qt_paths=QtConfPaths(launch_plugins, launch_qml),
                byte_limit=256 << 20,
            )
            launch_ctest = ctest_layout / ctest.name

            build_entries: list[
                tuple[Path, Path | bytes | GeneratedLayoutEntry]
            ] = []
            build_destinations: set[str] = set()

            def append_build_entry(
                destination: Path,
                entry: Path | bytes | GeneratedLayoutEntry,
                *,
                if_absent: bool = False,
            ) -> None:
                identity = os.path.normcase(str(destination))
                if identity in build_destinations:
                    if if_absent:
                        return
                    raise RuntimeError(
                        f"duplicate CTest build layout destination: {destination}"
                    )
                build_destinations.add(identity)
                build_entries.append((destination, entry))

            if needs_build_registration_contract:
                staged_cache_values = {
                    "CMAKE_CXX_COMPILER": launch_runtime_dirs[0] / compiler.name,
                    "OLR_FFMPEG_ROOT": runtime_layout / "ffmpeg",
                    "OLR_SRT_ROOT": runtime_layout / "srt",
                    "CMAKE_CTEST_COMMAND": launch_ctest,
                }
                declared_cache_values = tuple(
                    sorted(
                        (key, value.as_posix())
                        for key, value in staged_cache_values.items()
                    )
                )

                def rewrite_staged_cache(
                    _staged_build: Path,
                    *,
                    original: bytes = cache_payload,
                    replacements: tuple[tuple[str, str], ...] = declared_cache_values,
                ) -> bytes:
                    replacement_map = dict(replacements)
                    replaced: set[str] = set()
                    rewritten: list[str] = []
                    for line in original.decode("utf-8").splitlines(keepends=True):
                        declaration, separator, _value = line.partition("=")
                        key, type_separator, _cache_type = declaration.partition(":")
                        replacement = replacement_map.get(key)
                        if not separator or not type_separator or replacement is None:
                            rewritten.append(line)
                            continue
                        ending = (
                            "\r\n"
                            if line.endswith("\r\n")
                            else "\n" if line.endswith("\n") else ""
                        )
                        rewritten.append(f"{declaration}={replacement}{ending}")
                        replaced.add(key)
                    missing = sorted(set(replacement_map) - replaced)
                    if missing:
                        raise RuntimeError(
                            "staged CMake cache is missing rewrite entries: "
                            + ", ".join(missing)
                        )
                    return "".join(rewritten).encode("utf-8")

                append_build_entry(
                    Path("CMakeCache.txt"),
                    GeneratedLayoutEntry(
                        "ctest-cmake-cache-rewrite-v1",
                        {
                            "source_final_path": cache_descriptor.final_path.as_posix(),
                            "source_sha256": cache_descriptor.sha256,
                            "replacements": declared_cache_values,
                        },
                        rewrite_staged_cache,
                    ),
                )

            executable_replacements: list[tuple[Path, Path]] = []
            executable_destinations: dict[Path, Path] = {}
            external_launch_executables: dict[Path, Path] = {}
            external_dependencies: list[Path] = []

            def external_entry_identity(
                destination: Path,
                source: Path | bytes | GeneratedLayoutEntry,
            ) -> tuple[str, str, str]:
                if isinstance(source, Path):
                    return (destination.as_posix(), "source", source.as_posix())
                if isinstance(source, bytes):
                    return (
                        destination.as_posix(),
                        "literal",
                        hashlib.sha256(source).hexdigest(),
                    )
                return (destination.as_posix(), "generated", source.policy)

            for executable in sorted(
                selected_executables, key=lambda path: str(path).casefold()
            ):
                if executable == canonical_build or executable.is_relative_to(
                    canonical_build
                ):
                    relative = executable.relative_to(canonical_build)
                else:
                    closure = _external_tool_closure(executable, lease=lease)
                    staged_external = lease.stage_readonly_layout(
                        f"external-tool-{len(external_launch_executables)}",
                        closure.entries,
                        context={
                            "policy": "ctest-external-tool-v2",
                            "root": closure.root.as_posix(),
                            "executable_relative": closure.executable_relative.as_posix(),
                            "entries": tuple(
                                sorted(
                                    external_entry_identity(destination, source)
                                    for destination, source in closure.entries.items()
                                )
                            ),
                        },
                        dependencies=(runtime_layout, launch_plugins, launch_qml),
                        qt_paths=QtConfPaths(launch_plugins, launch_qml),
                        entry_limit=32768,
                        byte_limit=1 << 30,
                    )
                    external_dependencies.append(staged_external)
                    external_launch_executables[executable] = (
                        staged_external / closure.executable_relative
                    )
                    continue
                append_build_entry(relative, executable)
                executable_replacements.append((executable, relative))
                executable_destinations[executable] = relative
                for adjacent in sorted(
                    executable.parent.glob("*.dll"),
                    key=lambda path: path.name.casefold(),
                ):
                    if adjacent.name.casefold() in configured_runtime_names:
                        continue
                    destination = relative.parent / adjacent.name
                    append_build_entry(destination, adjacent, if_absent=True)

            working_directory_relatives = tuple(
                sorted(
                    {
                        test.working_directory.relative_to(canonical_build)
                        for test in selected_tests
                    },
                    key=lambda path: path.as_posix().casefold(),
                )
            )
            for working_relative in working_directory_relatives:
                append_build_entry(
                    working_relative / ".olr-working-directory.closed",
                    b"closed CTest working directory\n",
                    if_absent=True,
                )

            graph_texts: dict[Path, tuple[str, str, str]] = {}
            for path in graph.inputs:
                descriptor = lease._describe_held_source(path)
                stream = descriptor.stream
                stream.seek(0)
                payload = stream.read()
                stream.seek(0)
                graph_texts[path] = (
                    payload.decode("utf-8", errors="replace"),
                    descriptor.final_path.as_posix(),
                    descriptor.sha256,
                )
            working_overrides: dict[Path, list[tuple[str, Path]]] = {}
            for test in selected_tests:
                working_overrides.setdefault(test.scope_file, []).append(
                    (
                        test.name,
                        test.working_directory.relative_to(canonical_build),
                    )
                )

            selected_test_names = tuple(sorted(test.name for test in selected_tests))
            for graph_input, graph_source in graph_texts.items():
                original_text, source_final_path, source_sha256 = graph_source
                relative = graph_input.relative_to(canonical_build)

                declared_replacements = [
                    (source.as_posix(), destination.as_posix())
                    for source, destination in qml_replacements
                ]
                declared_replacements.extend(
                    (source.as_posix(), f"$BUILD/{destination.as_posix()}")
                    for source, destination in executable_replacements
                )
                declared_replacements.extend(
                    (source.as_posix(), destination.as_posix())
                    for source, destination in external_launch_executables.items()
                )
                declared_replacements.extend(
                    (
                        source.as_posix(),
                        f"$BUILD/{source.relative_to(canonical_build).as_posix()}",
                    )
                    for source in graph.inputs
                )
                declared_replacements.extend(
                    (source.as_posix(), destination.as_posix())
                    for source, destination in zip(
                        runtime_dirs, launch_runtime_dirs, strict=True
                    )
                )
                declared_replacements.extend(
                    (
                        (ffmpeg_root.as_posix(), (runtime_layout / "ffmpeg").as_posix()),
                        (srt_root.as_posix(), (runtime_layout / "srt").as_posix()),
                    )
                )
                declared_replacements = sorted(
                    declared_replacements,
                    key=lambda pair: (pair[0].casefold(), pair[1].casefold()),
                )

                declared_occurrences: list[tuple[int, int, str]] = []
                for path_argument in graph.path_arguments:
                    if path_argument.definition_file == graph_input:
                        declared_occurrences.append(
                            (
                                path_argument.occurrence.start,
                                path_argument.occurrence.end,
                                f"$BUILD/{path_argument.path.relative_to(canonical_build).as_posix()}",
                            )
                        )
                for test in graph.tests:
                    if test.definition_file != graph_input:
                        continue
                    for _raw, occurrence in (
                        *test.environment_values,
                        *test.environment_modification_values,
                    ):
                        declared_occurrences.append(
                            (occurrence.start, occurrence.end, "environment-rewrite")
                        )
                for test in selected_tests:
                    if test.definition_file != graph_input:
                        continue
                    for quicktest_input in quicktest_inputs_by_test.get(
                        _quicktest_record_key(test), ()
                    ):
                        declared_occurrences.append(
                            (
                                quicktest_input.occurrence.start,
                                quicktest_input.occurrence.end,
                                "quicktest-input-rewrite",
                            )
                        )
                    for declared_input in declared_inputs_by_test.get(
                        _quicktest_record_key(test), ()
                    ):
                        declared_occurrences.append(
                            (
                                declared_input.occurrence.start,
                                declared_input.occurrence.end,
                                f"declared-{declared_input.role}-rewrite",
                            )
                        )
                    for _argument, executable, occurrence in test.executable_arguments:
                        destination = external_launch_executables.get(executable)
                        declared_destination = (
                            destination.as_posix()
                            if destination is not None
                            else "$BUILD/"
                            + executable_destinations[executable].as_posix()
                        )
                        declared_occurrences.append(
                            (occurrence.start, occurrence.end, declared_destination)
                        )
                declared_occurrences.sort()
                declared_overrides = tuple(
                    sorted(
                        (name, path.as_posix())
                        for name, path in working_overrides.get(graph_input, ())
                    )
                )

                def rewrite_graph(
                    staged_build: Path,
                    *,
                    graph_input: Path = graph_input,
                    original_text: str = original_text,
                    relative: Path = relative,
                ) -> bytes:
                    replacements = list(qml_replacements)
                    replacements.extend(
                        (path, staged_build / relative_path)
                        for path, relative_path in executable_replacements
                    )
                    replacements.extend(external_launch_executables.items())
                    replacements.extend(
                        (
                            path,
                            staged_build / path.relative_to(canonical_build),
                        )
                        for path in graph.inputs
                    )
                    replacements.extend(zip(runtime_dirs, launch_runtime_dirs))
                    replacements.extend(
                        (
                            (ffmpeg_root, runtime_layout / "ffmpeg"),
                            (srt_root, runtime_layout / "srt"),
                        )
                    )
                    prepared_replacements = _prepare_path_replacements(replacements)
                    occurrence_replacements: list[
                        tuple[CMakeArgumentOccurrence, str]
                    ] = []
                    for path_argument in graph.path_arguments:
                        if path_argument.definition_file != graph_input:
                            continue
                        staged_path = (
                            staged_build
                            / path_argument.path.relative_to(canonical_build)
                        )
                        replacement = _rewrite_path_identity(
                            path_argument.argument,
                            ((path_argument.path, staged_path),),
                        )
                        if replacement != path_argument.argument:
                            occurrence_replacements.append(
                                (path_argument.occurrence, replacement)
                            )
                    for test in graph.tests:
                        if test.definition_file != graph_input:
                            continue
                        for raw, occurrence in test.environment_values:
                            replacement = _rewrite_environment_property(
                                raw,
                                replacements,
                                modification=False,
                                prepared_replacements=prepared_replacements,
                            )
                            if replacement != raw:
                                occurrence_replacements.append(
                                    (occurrence, replacement)
                                )
                        for raw, occurrence in test.environment_modification_values:
                            replacement = _rewrite_environment_property(
                                raw,
                                replacements,
                                modification=True,
                                prepared_replacements=prepared_replacements,
                            )
                            if replacement != raw:
                                occurrence_replacements.append(
                                    (occurrence, replacement)
                                )
                    for test in selected_tests:
                        if test.definition_file != graph_input:
                            continue
                        for quicktest_input in quicktest_inputs_by_test.get(
                            _quicktest_record_key(test), ()
                        ):
                            staged_input = (
                                launch_source
                                / quicktest_input.path.relative_to(source_root)
                            )
                            occurrence_replacements.append(
                                (
                                    quicktest_input.occurrence,
                                    quicktest_input.prefix + staged_input.as_posix(),
                                )
                            )
                        for declared_input in declared_inputs_by_test.get(
                            _quicktest_record_key(test), ()
                        ):
                            key = (
                                declared_input.definition_file,
                                declared_input.occurrence.start,
                                declared_input.occurrence.end,
                            )
                            staged_input = declared_input_replacements.get(key)
                            if staged_input is None:
                                if declared_input.path != canonical_build:
                                    raise RuntimeError(
                                        "declared CTest path has no staged mapping: "
                                        f"{declared_input.path}"
                                    )
                                staged_input = staged_build
                            occurrence_replacements.append(
                                (
                                    declared_input.occurrence,
                                    declared_input.prefix + staged_input.as_posix(),
                                )
                            )
                        for argument, executable, occurrence in test.executable_arguments:
                            candidate = (
                                argument.partition("=")[2]
                                if argument.startswith("-") and "=" in argument
                                else argument
                            )
                            prefix = argument[: len(argument) - len(candidate)]
                            staged_executable = external_launch_executables.get(
                                executable
                            )
                            if staged_executable is None:
                                staged_executable = (
                                    staged_build
                                    / executable_destinations[executable]
                                )
                            occurrence_replacements.append(
                                (
                                    occurrence,
                                    prefix + staged_executable.as_posix(),
                                )
                            )
                    rewritten = _rewrite_cmake_occurrences(
                        original_text, occurrence_replacements
                    )
                    overrides = working_overrides.get(graph_input, ())
                    if overrides:
                        rewritten += (
                            "\n# Controlled launch uses immutable staged CTest "
                            "working directories.\n"
                        )
                        rewritten += "\n".join(
                            "set_tests_properties("
                            f"{_cmake_bracket(name)} PROPERTIES WORKING_DIRECTORY "
                            f"{_cmake_bracket(str(staged_build / path))})"
                            for name, path in overrides
                        )
                        rewritten += "\n"
                    return rewritten.encode("utf-8")

                append_build_entry(
                    relative,
                    GeneratedLayoutEntry(
                        "ctest-graph-rewrite-v2",
                        {
                            "source_final_path": source_final_path,
                            "source_sha256": source_sha256,
                            "selected_test_names": selected_test_names,
                            "replacement_paths": tuple(declared_replacements),
                            "occurrence_replacements": tuple(declared_occurrences),
                            "working_overrides": declared_overrides,
                        },
                        rewrite_graph,
                    ),
                )

            launch_build = lease.stage_readonly_layout(
                "ctest-build",
                build_entries,
                context={
                    "policy": "ctest-build-v2",
                    "graph_policy": "ctest-graph-rewrite-v2",
                    "build_registration_contract": (
                        "cmake-cache-plus-ctest-graph-v1"
                        if needs_build_registration_contract
                        else None
                    ),
                    "selected_tests": selected_test_names,
                },
                dependencies=tuple(
                    dict.fromkeys(
                        (
                            runtime_layout,
                            launch_plugins,
                            launch_qml,
                            *source_dependencies,
                            *external_dependencies,
                        )
                    )
                ),
                qt_paths=QtConfPaths(launch_plugins, launch_qml),
                entry_limit=16384,
                byte_limit=2 << 30,
            )
            rendered_graph = discover_ctest_graph(launch_build, lease=lease)
            staged_executables = frozenset(
                {
                    *(launch_build / path for path in executable_destinations.values()),
                    *external_launch_executables.values(),
                }
            )
            _validate_rendered_test_commands(
                rendered_graph,
                frozenset(selected_test_names),
                canonical_build,
                staged_executables,
            )
            lease.launch_build_directory = launch_build
            lease.launch_ctest_graph_files = tuple(
                sorted(
                    (
                        graph_input.relative_to(canonical_build)
                        for graph_input in graph_texts
                    ),
                    key=lambda path: path.as_posix().casefold(),
                )
            )

        system_dir, windows_dir = windows_system_directories()
        env = replace_environment_case_insensitively(
            inherited_env,
            {
                "SystemRoot": str(windows_dir),
                "PATH": os.pathsep.join(
                    str(path)
                    for path in (*launch_runtime_dirs, system_dir, windows_dir)
                ),
                "QT_PLUGIN_PATH": str(launch_plugins),
                "QT_QPA_PLATFORM_PLUGIN_PATH": str(launch_platform_plugins),
                "QT_QPA_PLATFORM": "offscreen",
                "QML_IMPORT_PATH": str(launch_qml),
                "QML2_IMPORT_PATH": str(launch_qml),
            },
            removed=WINDOWS_RUNTIME_INJECTION_ENVIRONMENT_VARIABLES,
            removed_prefixes=("BASH_FUNC_",),
        )
        return env, launch_ctest
    finally:
        if owned:
            lease.close()


def _set_windows_error_mode() -> int:
    flags = (
        _SEM_FAILCRITICALERRORS
        | _SEM_NOGPFAULTERRORBOX
        | _SEM_NOOPENFILEERRORBOX
    )
    return int(ctypes.windll.kernel32.SetErrorMode(flags))


def _restore_windows_error_mode(old_mode: int) -> None:
    ctypes.windll.kernel32.SetErrorMode(old_mode)


def _set_windows_dll_directory(directory: Path) -> str | None:
    """Remove CWD from the inherited loader search and select protected runtime."""
    return set_windows_dll_directory(directory)


def _restore_windows_dll_directory(previous: str | None) -> None:
    restore_windows_dll_directory(previous)


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    build_dir = find_build_directory(args, Path.cwd())
    with RuntimeAuditLease() as audit_lease:
        env, ctest = prepare_windows_runtime(
            build_dir,
            os.environ,
            audit_lease=audit_lease,
            ctest_args=args,
        )
        if audit_lease.launch_ctest_arguments is not None:
            args = list(audit_lease.launch_ctest_arguments)
        build_dir = (
            audit_lease.launch_build_directory
            or audit_lease.canonical_directory(build_dir)
        )
        if os.name == "nt":
            build_dir = audit_lease.create_ctest_controller()
            cmake_config = audit_lease.launch_cmake_config_directory
            if (
                cmake_config is None
                or cmake_config.parent != build_dir
                or not cmake_config.is_dir()
            ):
                raise RuntimeError(
                    "controlled CTest launch has no private CMake config directory"
                )
            env["CMAKE_CONFIG_DIR"] = str(cmake_config)
            # MSYS2 tools default to /tmp, which is intentionally absent from
            # the immutable Git-for-Windows closure.  Give shell tests the
            # controller's private, ephemeral writable output directory.
            env["TMPDIR"] = str(build_dir / "Testing" / "Temporary")
        runtime_dir = audit_lease.launch_runtime_directory
        if os.name == "nt" and runtime_dir is None:
            raise RuntimeError("controlled Windows launch has no protected runtime")
        command = [
            str(ctest),
            *args,
            "--test-dir",
            str(build_dir),
            "--interactive-debug-mode",
            "0",
        ]
        old_mode = _set_windows_error_mode()
        try:
            old_dll_directory = (
                _set_windows_dll_directory(runtime_dir)
                if runtime_dir is not None
                else None
            )
            try:
                return run_process_tree(
                    command,
                    enabled=os.name == "nt",
                    env=env,
                    private_desktop=True,
                )
            finally:
                if runtime_dir is not None:
                    _restore_windows_dll_directory(old_dll_directory)
        finally:
            _restore_windows_error_mode(old_mode)


if __name__ == "__main__":
    raise SystemExit(main())
