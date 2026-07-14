#!/usr/bin/env python3
"""Run CTest with the controlled Windows runtime from a CMake build tree."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import subprocess
import sys
from collections.abc import Mapping, Sequence


_REQUIRED_CACHE_KEYS = (
    "CMAKE_CXX_COMPILER",
    "CMAKE_COMMAND",
    "Qt6_DIR",
    "OLR_FFMPEG_ROOT",
    "OLR_SRT_ROOT",
)

_SEM_FAILCRITICALERRORS = 0x0001
_SEM_NOGPFAULTERRORBOX = 0x0002
_SEM_NOOPENFILEERRORBOX = 0x8000


def find_build_directory(args: Sequence[str], cwd: Path) -> Path:
    """Return the build directory selected by CTest's ``--test-dir`` option."""
    for index, arg in enumerate(args):
        if arg == "--test-dir":
            if index + 1 >= len(args):
                raise ValueError("--test-dir requires a build directory")
            value = args[index + 1]
            if not value:
                raise ValueError("--test-dir requires a build directory")
            return (cwd / value).resolve()
        if arg.startswith("--test-dir="):
            value = arg.partition("=")[2]
            if not value:
                raise ValueError("--test-dir requires a build directory")
            return (cwd / value).resolve()
    return cwd.resolve()


def read_cmake_cache(path: Path) -> dict[str, str]:
    """Parse typed CMake cache entries as data without evaluating their values."""
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        declaration, separator, value = line.partition("=")
        if not separator:
            continue
        key, type_separator, _cache_type = declaration.partition(":")
        if not type_separator or not key:
            continue
        entries[key] = value
    return entries


def prepare_windows_runtime(
    build_dir: Path, inherited_env: Mapping[str, str]
) -> tuple[dict[str, str], Path]:
    """Create a child environment using runtime paths recorded by the build."""
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    for key in _REQUIRED_CACHE_KEYS:
        if not cache.get(key):
            raise ValueError(f"CMake cache is missing required entry: {key}")

    compiler_bin = Path(cache["CMAKE_CXX_COMPILER"]).parent
    qt_root = Path(cache["Qt6_DIR"]).parents[2]
    ffmpeg_root = Path(cache["OLR_FFMPEG_ROOT"])
    srt_root = Path(cache["OLR_SRT_ROOT"])
    runtime_dirs = (
        compiler_bin,
        qt_root / "bin",
        ffmpeg_root / "bin",
        ffmpeg_root / "lib",
        srt_root / "bin",
        srt_root / "lib",
    )

    env = dict(inherited_env)
    path_entries = [str(path) for path in runtime_dirs]
    if env.get("PATH"):
        path_entries.append(env["PATH"])
    env["PATH"] = os.pathsep.join(path_entries)
    env["QT_PLUGIN_PATH"] = str(qt_root / "plugins")
    env.setdefault("QT_QPA_PLATFORM", "offscreen")

    ctest = Path(cache["CMAKE_COMMAND"]).with_name("ctest.exe")
    return env, ctest


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
    env, ctest = prepare_windows_runtime(build_dir, os.environ)
    command = [str(ctest), "--interactive-debug-mode", "0", *args]
    old_mode = _set_windows_error_mode()
    try:
        return subprocess.run(command, env=env, check=False).returncode
    finally:
        _restore_windows_error_mode(old_mode)


if __name__ == "__main__":
    raise SystemExit(main())
