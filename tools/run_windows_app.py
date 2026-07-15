#!/usr/bin/env python3
"""Launch a build-tree app with runtime roots from its active CMake cache."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
from collections.abc import Mapping, Sequence


TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from windows_error_mode import suppress_windows_error_dialogs
from windows_runtime_guard import RuntimeRoots, reject_runtime_dll_shadowing


_REQUIRED_CACHE_KEYS = (
    "CMAKE_CXX_COMPILER",
    "Qt6_DIR",
    "OLR_FFMPEG_ROOT",
    "OLR_SRT_ROOT",
)


def read_cmake_cache(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        declaration, separator, value = line.partition("=")
        if not separator:
            continue
        key, type_separator, _cache_type = declaration.partition(":")
        if type_separator and key:
            entries[key] = value
    return entries


def _require_directory(label: str, path: Path, cache_key: str) -> Path:
    if not path.is_dir():
        raise FileNotFoundError(
            f"{label} directory from {cache_key} does not exist: {path}; "
            "reconfigure or recreate the selected build directory"
        )
    return path


def prepare_windows_app_runtime(
    build_dir: Path,
    executable: Path,
    inherited_env: Mapping[str, str],
) -> dict[str, str]:
    """Return a cache-derived child environment with no ambient DLL search path."""
    build_dir = build_dir.resolve()
    executable = executable.resolve()
    try:
        executable.relative_to(build_dir)
    except ValueError as error:
        raise ValueError(
            f"executable must belong to the selected build directory: {executable}"
        ) from error
    if not executable.is_file():
        raise FileNotFoundError(f"app executable does not exist: {executable}")

    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    for key in _REQUIRED_CACHE_KEYS:
        if not cache.get(key):
            raise ValueError(f"CMake cache is missing required entry: {key}")

    compiler_bin = Path(cache["CMAKE_CXX_COMPILER"]).parent
    qt_root = Path(cache["Qt6_DIR"]).parents[2]
    ffmpeg_root = Path(cache["OLR_FFMPEG_ROOT"])
    srt_root = Path(cache["OLR_SRT_ROOT"])
    runtime_dirs = (
        _require_directory("compiler runtime", compiler_bin, "CMAKE_CXX_COMPILER"),
        _require_directory("Qt runtime", qt_root / "bin", "Qt6_DIR"),
        _require_directory("FFmpeg runtime", ffmpeg_root / "bin", "OLR_FFMPEG_ROOT"),
        _require_directory("FFmpeg libraries", ffmpeg_root / "lib", "OLR_FFMPEG_ROOT"),
        _require_directory("SRT runtime", srt_root / "bin", "OLR_SRT_ROOT"),
        _require_directory("SRT libraries", srt_root / "lib", "OLR_SRT_ROOT"),
    )
    qt_plugins = _require_directory("Qt plugins", qt_root / "plugins", "Qt6_DIR")
    qt_platform_plugins = _require_directory(
        "Qt platform plugins", qt_plugins / "platforms", "Qt6_DIR"
    )
    qt_qml = _require_directory("Qt QML imports", qt_root / "qml", "Qt6_DIR")

    reject_runtime_dll_shadowing(
        executable.parent,
        RuntimeRoots(
            compiler_bin,
            qt_root / "bin",
            ffmpeg_root / "bin",
            ffmpeg_root / "lib",
            srt_root / "bin",
            srt_root / "lib",
        ),
    )

    system_root = Path(inherited_env.get("SystemRoot", "C:/Windows"))
    system_dirs = (system_root / "System32", system_root)
    env = dict(inherited_env)
    env["PATH"] = os.pathsep.join(str(path) for path in (*runtime_dirs, *system_dirs))
    env["QT_PLUGIN_PATH"] = str(qt_plugins)
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt_platform_plugins)
    env["QML_IMPORT_PATH"] = str(qt_qml)
    env["QML2_IMPORT_PATH"] = str(qt_qml)
    return env


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
    env = prepare_windows_app_runtime(build_dir, executable, os.environ)
    app_args = list(args.app_args)
    if app_args[:1] == ["--"]:
        app_args.pop(0)
    command = [str(executable), *app_args]
    with suppress_windows_error_dialogs(True):
        return subprocess.run(
            command, cwd=executable.parent, env=env, check=False
        ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
