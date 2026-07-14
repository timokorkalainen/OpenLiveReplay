#!/usr/bin/env python3
"""Check that compiled unit tests declare their Windows runtime environment."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess


def read_cmake_cache(path: Path) -> dict[str, str]:
    """Return typed entries from a CMake cache without evaluating them."""
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


def expected_runtime_entries(cache: dict[str, str]) -> tuple[str, ...]:
    """Return runtime entries required in verbose CTest registration."""
    compiler_bin = Path(cache["CMAKE_CXX_COMPILER"]).parent
    qt_root = Path(cache["Qt6_DIR"]).parents[2]
    ffmpeg_root = Path(cache["OLR_FFMPEG_ROOT"])
    srt_root = Path(cache["OLR_SRT_ROOT"])
    qt_plugins = qt_root / "bin" / ".." / "plugins"
    return tuple(
        str(entry)
        for entry in (
            compiler_bin,
            qt_root / "bin",
            ffmpeg_root / "bin",
            ffmpeg_root / "lib",
            srt_root / "bin",
            srt_root / "lib",
            f"QT_PLUGIN_PATH={qt_plugins}",
            "QT_QPA_PLATFORM=offscreen",
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    ctest_value = cache.get("CMAKE_CTEST_COMMAND")
    ctest = (
        Path(ctest_value)
        if ctest_value
        else Path(cache["CMAKE_COMMAND"]).with_name("ctest.exe")
    )
    result = subprocess.run(
        [
            str(ctest),
            "--test-dir",
            str(build_dir),
            "-N",
            "-V",
            "-R",
            "^tst_muxer$",
        ],
        text=True,
        capture_output=True,
        check=True,
    )
    output = result.stdout.replace("\\", "/").casefold()
    for required in expected_runtime_entries(cache):
        normalized_required = required.replace("\\", "/").casefold()
        if normalized_required not in output:
            raise SystemExit(f"missing Windows test runtime entry: {required}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
