#!/usr/bin/env python3
"""Run CTest with the controlled Windows runtime from a CMake build tree."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import sys
from collections.abc import Mapping, Sequence


TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

try:
    from .windows_job import run_process_tree
    from .windows_runtime_guard import (
        discover_ctest_graph,
        parse_cmake_cache,
        require_cache_entries,
        RuntimeAuditLease,
        RuntimeRoots,
        reject_runtime_dll_shadowing,
        validate_ctest_runtime_properties,
        windows_system_directories,
    )
except ImportError:
    from windows_job import run_process_tree
    from windows_runtime_guard import (
        discover_ctest_graph,
        parse_cmake_cache,
        require_cache_entries,
        RuntimeAuditLease,
        RuntimeRoots,
        reject_runtime_dll_shadowing,
        validate_ctest_runtime_properties,
        windows_system_directories,
    )


_SEM_FAILCRITICALERRORS = 0x0001
_SEM_NOGPFAULTERRORBOX = 0x0002
_SEM_NOOPENFILEERRORBOX = 0x8000


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

        graph = discover_ctest_graph(canonical_build, lease=lease)
        validate_ctest_runtime_properties(
            graph,
            runtime_dirs=runtime_dirs,
            qt_plugins=qt_plugins,
            qt_platform_plugins=qt_platform_plugins,
            qt_qml=qt_qml,
            source_root=source_root,
            build_dir=canonical_build,
            ffmpeg_root=ffmpeg_root,
            srt_root=srt_root,
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

        system_dir, windows_dir = windows_system_directories()
        env = dict(inherited_env)
        env["SystemRoot"] = str(windows_dir)
        env["PATH"] = os.pathsep.join(
            str(path) for path in (*runtime_dirs, system_dir, windows_dir)
        )
        env["QT_PLUGIN_PATH"] = str(qt_plugins)
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt_platform_plugins)
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["QML_IMPORT_PATH"] = str(qt_qml)
        env["QML2_IMPORT_PATH"] = str(qt_qml)
        return env, ctest
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


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    build_dir = find_build_directory(args, Path.cwd())
    with RuntimeAuditLease() as audit_lease:
        env, ctest = prepare_windows_runtime(
            build_dir, os.environ, audit_lease=audit_lease
        )
        build_dir = audit_lease.canonical_directory(build_dir)
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
            return run_process_tree(
                command,
                enabled=os.name == "nt",
                env=env,
                private_desktop=True,
            )
        finally:
            _restore_windows_error_mode(old_mode)


if __name__ == "__main__":
    raise SystemExit(main())
