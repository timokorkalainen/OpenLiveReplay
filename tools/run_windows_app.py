#!/usr/bin/env python3
"""Launch a build-tree app with runtime roots from its active CMake cache."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
from collections.abc import Mapping, Sequence


TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

try:
    from .windows_error_mode import suppress_windows_error_dialogs
    from .windows_job import run_process_tree
    from . import windows_runtime_guard as runtime_guard
except ImportError:
    from windows_error_mode import suppress_windows_error_dialogs
    from windows_job import run_process_tree
    import windows_runtime_guard as runtime_guard


def read_cmake_cache(path: Path) -> dict[str, str]:
    return {
        key: entry.value
        for key, entry in runtime_guard.parse_cmake_cache(path).items()
    }

def prepare_windows_app_runtime(
    build_dir: Path,
    executable: Path,
    inherited_env: Mapping[str, str],
    *,
    audit_lease: runtime_guard.RuntimeAuditLease | None = None,
) -> dict[str, str]:
    """Return a cache-derived child environment with no ambient DLL search path."""
    owned = audit_lease is None
    lease = runtime_guard.RuntimeAuditLease() if audit_lease is None else audit_lease
    try:
        lexical_build = Path(os.path.abspath(build_dir))
        lexical_executable = Path(os.path.abspath(executable))
        try:
            lexical_executable.relative_to(lexical_build)
        except ValueError as error:
            raise ValueError(
                "executable must belong to the selected build directory: "
                f"{lexical_executable}"
            ) from error
        build_dir = lease.canonical_directory(lexical_build)
        executable = lease.canonical_file(lexical_executable)
        try:
            executable.relative_to(build_dir)
        except ValueError as error:
            raise ValueError(
                "executable resolves outside the selected build directory: "
                f"{lexical_executable} -> {executable}"
            ) from error

        schema = {
            "CMAKE_CXX_COMPILER": ("FILEPATH", "UNINITIALIZED"),
            "CMAKE_COMMAND": ("INTERNAL", "FILEPATH"),
            "Qt6_DIR": ("PATH",),
            "OLR_FFMPEG_ROOT": ("PATH",),
            "OLR_SRT_ROOT": ("PATH",),
            "CMAKE_HOME_DIRECTORY": ("INTERNAL", "PATH"),
        }
        cache = runtime_guard.require_cache_entries(
            build_dir / "CMakeCache.txt",
            schema,
            absolute_paths=tuple(schema),
        )
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
            "compiler executable", Path(cache["CMAKE_CXX_COMPILER"]), "CMAKE_CXX_COMPILER"
        )
        canonical_file("CMake executable", Path(cache["CMAKE_COMMAND"]), "CMAKE_COMMAND")
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

        runtime_guard.reject_runtime_dll_shadowing(
            executable.parent,
            runtime_guard.RuntimeRoots(*runtime_dirs),
            executables=(executable,),
            lease=lease,
            require_embedded_manifests=os.name == "nt",
        )

        launch_runtime_dirs = runtime_dirs
        launch_plugins = qt_plugins
        launch_platform_plugins = qt_platform_plugins
        launch_executable = executable
        if audit_lease is not None and os.name == "nt":
            configured_runtime_names = {
                child.name.casefold()
                for directory in runtime_dirs
                for child in directory.iterdir()
                if child.is_file() and child.suffix.casefold() == ".dll"
            }
            adjacent_app_dlls = tuple(
                adjacent
                for adjacent in sorted(
                    executable.parent.iterdir(),
                    key=lambda path: path.name.casefold(),
                )
                if adjacent != executable
                and adjacent.is_file()
                and adjacent.suffix.casefold() == ".dll"
                and adjacent.name.casefold() not in configured_runtime_names
            )
            plugin_files = runtime_guard.qt_plugin_closure(
                qt_plugins,
                platforms=("qwindows.dll", "qoffscreen.dll"),
            )
            plugin_entries: dict[Path, Path | bytes] = {
                path.relative_to(qt_plugins): path for path in plugin_files
            }
            plugin_entries.setdefault(
                Path("platforms") / ".closed", b"closed plugin namespace\n"
            )
            launch_plugins = lease.stage_readonly_layout(
                "app-qt-plugins",
                plugin_entries,
                context={
                    "policy": "app-qt-plugins-v2",
                    "platforms": ("qwindows.dll", "qoffscreen.dll"),
                },
                byte_limit=256 << 20,
            )
            launch_platform_plugins = (
                launch_plugins / qt_platform_plugins.relative_to(qt_plugins)
            )
            qml_files = runtime_guard.qt_qml_closure(qt_qml, (source_root,))
            qml_entries: dict[Path, Path | bytes] = {
                path.relative_to(qt_qml): path for path in qml_files
            }
            qml_entries.setdefault(Path(".closed"), b"closed QML namespace\n")
            launch_qml = lease.stage_readonly_layout(
                "app-qt-qml",
                qml_entries,
                context={
                    "policy": "app-qt-qml-v2",
                    "source_root": source_root.as_posix(),
                },
                byte_limit=512 << 20,
            )

            runtime_relative_dirs = (
                Path("mingw") / "bin",
                Path("qt") / "bin",
                Path("ffmpeg") / "bin",
                Path("ffmpeg") / "lib",
                Path("srt") / "bin",
                Path("srt") / "lib",
            )
            runtime_entries: dict[Path, Path | bytes] = {
                Path("loader") / ".closed": b"closed loader namespace\n"
            }
            runtime_closure = set(
                runtime_guard.transitive_runtime_closure(
                    (
                        executable,
                        *adjacent_app_dlls,
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
                    if child.is_file() and child in runtime_closure:
                        runtime_entries[relative_directory / child.name] = child
            runtime_layout = lease.stage_readonly_layout(
                "app-runtime",
                runtime_entries,
                context={
                    "policy": "app-runtime-v2",
                    "runtime_roots": tuple(
                        directory.as_posix() for directory in runtime_dirs
                    ),
                    "relative_directories": tuple(
                        path.as_posix() for path in runtime_relative_dirs
                    ),
                },
                dependencies=(launch_plugins, launch_qml),
                qt_paths=runtime_guard.QtConfPaths(launch_plugins, launch_qml),
                byte_limit=2 << 30,
            )
            loader_directory = runtime_layout / "loader"
            launch_runtime_dirs = tuple(
                runtime_layout / path for path in runtime_relative_dirs
            )
            lease.launch_runtime_directory = loader_directory
            lease.launch_runtime_directories = launch_runtime_dirs

            app_entries: dict[Path, Path | bytes] = {
                Path(executable.name): executable,
            }
            for adjacent in adjacent_app_dlls:
                app_entries[Path(adjacent.name)] = adjacent
            app_layout = lease.stage_readonly_layout(
                "app",
                app_entries,
                context={
                    "policy": "app-layout-v2",
                    "executable": executable.name,
                },
                dependencies=(runtime_layout, launch_plugins, launch_qml),
                qt_paths=runtime_guard.QtConfPaths(launch_plugins, launch_qml),
                byte_limit=1 << 30,
            )
            launch_executable = app_layout / executable.name
            lease.launch_executable = launch_executable

        system_dir, windows_dir = runtime_guard.windows_system_directories()
        env = runtime_guard.replace_environment_case_insensitively(
            inherited_env,
            {
                "SystemRoot": str(windows_dir),
                "PATH": os.pathsep.join(
                    str(path)
                    for path in (*launch_runtime_dirs, system_dir, windows_dir)
                ),
                "QT_PLUGIN_PATH": str(launch_plugins),
                "QT_QPA_PLATFORM_PLUGIN_PATH": str(launch_platform_plugins),
                "QML_IMPORT_PATH": str(launch_qml),
                "QML2_IMPORT_PATH": str(launch_qml),
            },
        )
        return env
    finally:
        if owned:
            lease.close()


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("app_args", nargs=argparse.REMAINDER)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(list(sys.argv[1:] if argv is None else argv))
    build_dir = args.build_dir.resolve()
    executable = args.executable.resolve()
    with runtime_guard.RuntimeAuditLease() as audit_lease:
        env = prepare_windows_app_runtime(
            build_dir, executable, os.environ, audit_lease=audit_lease
        )
        executable = (
            audit_lease.launch_executable
            or audit_lease.canonical_file(executable)
        )
        app_args = list(args.app_args)
        if app_args[:1] == ["--"]:
            app_args.pop(0)
        command = [str(executable), *app_args]
        with suppress_windows_error_dialogs(True):
            runtime_directory = audit_lease.launch_runtime_directory
            previous = (
                runtime_guard.set_windows_dll_directory(runtime_directory)
                if runtime_directory is not None
                else None
            )
            try:
                return run_process_tree(
                    command,
                    enabled=os.name == "nt",
                    cwd=executable.parent,
                    env=env,
                    private_desktop=True,
                )
            finally:
                if runtime_directory is not None:
                    runtime_guard.restore_windows_dll_directory(previous)


if __name__ == "__main__":
    raise SystemExit(main())
