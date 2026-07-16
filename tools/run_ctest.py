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
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

try:
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
        reject_runtime_dll_shadowing,
        replace_environment_case_insensitively,
        restore_windows_dll_directory,
        set_windows_dll_directory,
        validate_ctest_runtime_properties,
        windows_system_directories,
        qt_plugin_closure,
        qt_qml_closure,
        transitive_runtime_closure,
    )
except ImportError:
    from windows_job import run_process_tree
    from windows_runtime_guard import (
        discover_ctest_graph,
        CMakeArgumentOccurrence,
        parse_cmake_cache,
        require_cache_entries,
        RuntimeAuditLease,
        RuntimeRoots,
        GeneratedLayoutEntry,
        QtConfPaths,
        reject_runtime_dll_shadowing,
        replace_environment_case_insensitively,
        restore_windows_dll_directory,
        set_windows_dll_directory,
        validate_ctest_runtime_properties,
        windows_system_directories,
        qt_plugin_closure,
        qt_qml_closure,
        transitive_runtime_closure,
    )


_SEM_FAILCRITICALERRORS = 0x0001
_SEM_NOGPFAULTERRORBOX = 0x0002
_SEM_NOOPENFILEERRORBOX = 0x8000


def _cmake_bracket(value: str) -> str:
    equals = ""
    while f"]{equals}]" in value:
        equals += "="
    return f"[{equals}[{value}]{equals}]"


def _rewrite_path_identity(
    value: str, replacements: Sequence[tuple[Path, Path]]
) -> str:
    """Rewrite one complete Windows path token, never a string prefix."""
    if not value or not Path(value).is_absolute():
        return value
    candidate = Path(os.path.abspath(value))
    candidate_key = os.path.normcase(str(candidate))
    for original, replacement in sorted(
        replacements, key=lambda item: len(str(item[0])), reverse=True
    ):
        original_key = os.path.normcase(str(Path(os.path.abspath(original))))
        if candidate_key == original_key:
            return replacement.as_posix()
        prefix = original_key.rstrip("\\/") + os.sep
        if candidate_key.startswith(prefix):
            relative = Path(candidate_key[len(prefix) :])
            return (replacement / relative).as_posix()
    return value


def _rewrite_environment_property(
    raw: str,
    replacements: Sequence[tuple[Path, Path]],
    *,
    modification: bool,
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
            replacement = _rewrite_path_identity(value, replacements)
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
            f"{name}={_rewrite_path_identity(remainder, replacements)}"
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


def _quicktest_record_key(test) -> _QuickTestRecordKey:
    occurrence = test.argument_occurrences[0]
    return _QuickTestRecordKey(
        test.definition_file,
        test.scope_file,
        occurrence.start,
        occurrence.end,
    )


def _quicktest_inputs(
    tests, source_root: Path, build_root: Path
) -> tuple[
    dict[_QuickTestRecordKey, tuple[_QuickTestInput, ...]], tuple[Path, ...]
]:
    by_test: dict[_QuickTestRecordKey, list[_QuickTestInput]] = {}
    paths: set[Path] = set()
    for test in tests:
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


def _selected_test_records(graph, args: Sequence[str]):
    """Return a safe superset for common literal CTest positive selectors."""
    if any(
        test.fixtures_setup or test.fixtures_cleanup or test.fixtures_required
        for test in graph.tests
    ):
        return graph.tests
    name_values: list[str] = []
    label_values: list[str] = []
    index = 0
    while index < len(args):
        argument = args[index]
        if argument in (
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
        ) or argument.startswith(("--tests-information=", "--tests-from-file=")):
            return graph.tests
        if argument.startswith(
            (
                "--fixture-exclude-any=",
                "--fixture-exclude-setup=",
                "--fixture-exclude-cleanup=",
            )
        ):
            return graph.tests
        target: list[str] | None = None
        if argument in ("-R", "--tests-regex"):
            target = name_values
        elif argument in ("-L", "--label-regex"):
            target = label_values
        if target is not None:
            if index + 1 >= len(args):
                return graph.tests
            target.append(args[index + 1])
            index += 2
            continue
        for option, values in (
            ("--tests-regex=", name_values),
            ("--label-regex=", label_values),
        ):
            if argument.startswith(option):
                values.append(argument[len(option) :])
                break
        index += 1

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
) -> tuple[Path, ...]:
    root = _validated_closure_path(root, selected_root, lease=lease)
    if root.is_file():
        return (root,)
    files: list[Path] = []
    for current_text, directory_names, file_names in os.walk(root):
        current = _validated_closure_path(
            Path(current_text), selected_root, lease=lease
        )
        retained: list[str] = []
        for name in directory_names:
            child = _validated_closure_path(
                current / name, selected_root, lease=lease
            )
            retained.append(child.name)
        directory_names[:] = retained
        for name in file_names:
            files.append(
                _validated_closure_path(
                    current / name, selected_root, lease=lease
                )
            )
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
    root = _validated_closure_path(
        venv_root if is_venv else executable.parent, lease=lease
    )
    executable_relative = executable.relative_to(root)
    entries: dict[Path, Path | bytes | GeneratedLayoutEntry] = {
        executable_relative: executable
    }

    for adjacent in executable.parent.iterdir():
        if adjacent.is_file() and (
            adjacent.suffix.casefold() == ".dll"
            or adjacent.name.casefold() == (executable.name + ".manifest").casefold()
        ):
            validated = _validated_closure_path(adjacent, root, lease=lease)
            entries[validated.relative_to(root)] = validated

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


def find_build_directory(args: Sequence[str], cwd: Path) -> Path:
    """Return the build directory selected by CTest's ``--test-dir`` option."""
    selected: str | None = None
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "--test-dir":
            if index + 1 >= len(args):
                raise ValueError("--test-dir requires a build directory")
            value = args[index + 1]
            if not value:
                raise ValueError("--test-dir requires a build directory")
            selected = value
            index += 2
            continue
        if arg.startswith("--test-dir="):
            value = arg.partition("=")[2]
            if not value:
                raise ValueError("--test-dir requires a build directory")
            selected = value
        index += 1
    return (cwd / selected).resolve() if selected is not None else cwd.resolve()


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
        reject_runtime_dll_shadowing(
            canonical_build,
            roots,
            executables=(*graph.executables, ctest),
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
            selected_tests = _selected_test_records(graph, ctest_args)
            quicktest_inputs_by_test, quicktest_inputs = _quicktest_inputs(
                selected_tests, source_root, canonical_build
            )
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
            if quicktest_inputs:
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
            needs_project_qml = bool(quicktest_inputs) or any(
                executable.name.casefold() in qt_payload_names
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
            )
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
                raise RuntimeError(
                    f"unsupported CTest QML import path: {qml_path}"
                )

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
            runtime_tool_destinations: dict[Path, Path] = {}
            runtime_directory_set = set(runtime_dirs)
            configured_runtime_names = {
                child.name.casefold()
                for directory in runtime_dirs
                for child in directory.iterdir()
                if child.is_file() and child.suffix.casefold() == ".dll"
            }
            adjacent_dependency_seeds = {
                adjacent
                for executable in selected_executables
                if executable.parent not in runtime_directory_set
                for adjacent in executable.parent.glob("*.dll")
                if adjacent.is_file()
                and adjacent.name.casefold() not in configured_runtime_names
            }
            runtime_closure = set(
                transitive_runtime_closure(
                    (
                        *selected_executables,
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
                    if not child.is_file() or (
                        child not in runtime_closure
                        and child not in selected_executables
                    ):
                        continue
                    destination = relative_directory / child.name
                    runtime_entries[destination] = child
                    if child in selected_executables:
                        runtime_tool_destinations[child] = destination
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
                    runtime_destination = runtime_tool_destinations.get(executable)
                    if runtime_destination is not None:
                        external_launch_executables[executable] = (
                            runtime_layout / runtime_destination
                        )
                        continue
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

            graph_texts: dict[Path, tuple[str, str, str]] = {}
            for path in graph.inputs:
                stream = lease.hold(path)
                stream.seek(0)
                payload = stream.read()
                stream.seek(0)
                graph_texts[path] = (
                    payload.decode("utf-8", errors="replace"),
                    lease.canonical_file(path).as_posix(),
                    hashlib.sha256(payload).hexdigest(),
                )
            working_overrides: dict[Path, list[str]] = {}
            for test in selected_tests:
                working_overrides.setdefault(test.scope_file, []).append(
                    "set_tests_properties("
                    f"{_cmake_bracket(test.name)} PROPERTIES WORKING_DIRECTORY "
                    f"{_cmake_bracket(str(test.working_directory))})"
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
                for test in selected_tests:
                    if test.definition_file != graph_input:
                        continue
                    for _raw, occurrence in (
                        *test.environment_values,
                        *test.environment_modification_values,
                    ):
                        declared_occurrences.append(
                            (occurrence.start, occurrence.end, "environment-rewrite")
                        )
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
                declared_overrides = tuple(sorted(working_overrides.get(graph_input, ())))

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
                    for test in selected_tests:
                        if test.definition_file != graph_input:
                            continue
                        for raw, occurrence in test.environment_values:
                            replacement = _rewrite_environment_property(
                                raw, replacements, modification=False
                            )
                            if replacement != raw:
                                occurrence_replacements.append(
                                    (occurrence, replacement)
                                )
                        for raw, occurrence in test.environment_modification_values:
                            replacement = _rewrite_environment_property(
                                raw, replacements, modification=True
                            )
                            if replacement != raw:
                                occurrence_replacements.append(
                                    (occurrence, replacement)
                                )
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
                            "\n# Controlled launch preserves original CTest "
                            "working directories.\n"
                        )
                        rewritten += "\n".join(overrides) + "\n"
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
                    "selected_tests": selected_test_names,
                },
                dependencies=(
                    runtime_layout,
                    launch_plugins,
                    launch_qml,
                    *source_dependencies,
                    *external_dependencies,
                ),
                qt_paths=QtConfPaths(launch_plugins, launch_qml),
                entry_limit=16384,
                byte_limit=2 << 30,
            )
            lease.launch_build_directory = launch_build

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
        build_dir = (
            audit_lease.launch_build_directory
            or audit_lease.canonical_directory(build_dir)
        )
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
