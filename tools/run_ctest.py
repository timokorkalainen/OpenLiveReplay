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
        ("compiler runtime", compiler_bin),
        ("Qt runtime", qt_root / "bin"),
        ("FFmpeg runtime", ffmpeg_root / "bin"),
        ("FFmpeg libraries", ffmpeg_root / "lib"),
        ("SRT runtime", srt_root / "bin"),
        ("SRT libraries", srt_root / "lib"),
    )
    qt_plugins = qt_root / "plugins"
    qt_qml = qt_root / "qml"

    # CMake records the authoritative CTest path in normal build trees. Older
    # caches may omit it, so fall back to the sibling executable beside CMake.
    ctest_value = cache.get("CMAKE_CTEST_COMMAND")
    ctest = (
        Path(ctest_value)
        if ctest_value
        else Path(cache["CMAKE_COMMAND"]).with_name("ctest.exe")
    )

    for label, path in (
        *runtime_dirs,
        ("Qt plugins", qt_plugins),
        ("Qt QML imports", qt_qml),
    ):
        if not path.is_dir():
            raise FileNotFoundError(f"{label} directory does not exist: {path}")
    if not ctest.is_file():
        raise FileNotFoundError(f"CTest executable does not exist: {ctest}")

    env = dict(inherited_env)
    path_entries = [str(path) for _label, path in runtime_dirs]
    if env.get("PATH"):
        path_entries.append(env["PATH"])
    env["PATH"] = os.pathsep.join(path_entries)
    env["QT_PLUGIN_PATH"] = str(qt_plugins)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    for variable in ("QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
        qml_entries = [str(qt_qml)]
        if env.get(variable):
            qml_entries.append(env[variable])
        env[variable] = os.pathsep.join(qml_entries)

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
    command = [str(ctest), *args, "--interactive-debug-mode", "0"]
    old_mode = _set_windows_error_mode()
    try:
        return subprocess.run(command, env=env, check=False).returncode
    finally:
        _restore_windows_error_mode(old_mode)


if __name__ == "__main__":
    raise SystemExit(main())
