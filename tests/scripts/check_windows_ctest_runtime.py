#!/usr/bin/env python3
"""Check generated and effective Windows runtime environments for CTest."""

from __future__ import annotations

import argparse
import os
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


def require_cache_value(cache: dict[str, str], key: str) -> str:
    value = cache.get(key, "").strip()
    if not value:
        raise SystemExit(f"CMake cache is missing required entry: {key}")
    return value


def require_directory(label: str, value: str | Path) -> Path:
    if not str(value).strip():
        raise SystemExit(f"missing Windows test runtime directory: {label}")
    path = Path(value)
    if not path.is_dir():
        raise SystemExit(
            f"Windows test runtime directory does not exist: {label}: {path}"
        )
    return path.resolve()


def normalize_path(value: str | Path) -> str:
    return os.path.normcase(os.path.normpath(str(Path(value).resolve())))


def configured_runtime(
    cache: dict[str, str], qt_runtime_dir: Path, qt_plugin_dir: Path
) -> tuple[tuple[Path, ...], Path]:
    compiler = Path(require_cache_value(cache, "CMAKE_CXX_COMPILER"))
    if not compiler.is_file():
        raise SystemExit(f"CMake compiler does not exist: {compiler}")
    compiler_runtime = require_directory("compiler runtime", compiler.parent)

    ffmpeg_root = require_directory(
        "FFmpeg root", require_cache_value(cache, "OLR_FFMPEG_ROOT")
    )
    srt_root = require_directory(
        "SRT root", require_cache_value(cache, "OLR_SRT_ROOT")
    )
    runtime_dirs = (
        compiler_runtime,
        require_directory("Qt runtime", qt_runtime_dir),
        require_directory("FFmpeg runtime", ffmpeg_root / "bin"),
        require_directory("FFmpeg libraries", ffmpeg_root / "lib"),
        require_directory("SRT runtime", srt_root / "bin"),
        require_directory("SRT libraries", srt_root / "lib"),
    )
    plugins = require_directory("Qt plugins", qt_plugin_dir)
    return runtime_dirs, plugins


def parse_verbose_runtime(output: str) -> tuple[dict[str, str], list[str]]:
    environment: dict[str, str] = {}
    modifications: list[str] = []
    for line in output.splitlines():
        test_number, separator, detail = line.partition(":")
        if not separator or not test_number.strip().isdigit():
            continue
        entry = detail.strip()
        modification_prefix = "PATH=path_list_prepend:"
        if entry.startswith(modification_prefix):
            modifications.append(entry.removeprefix(modification_prefix))
            continue
        key, value_separator, value = entry.partition("=")
        if value_separator and key in {"PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM"}:
            environment[key] = value
    return environment, modifications


def require_path_prefix(label: str, value: str, expected: tuple[Path, ...]) -> None:
    entries = tuple(part for part in value.split(os.pathsep) if part)
    actual_prefix = tuple(normalize_path(part) for part in entries[: len(expected)])
    expected_prefix = tuple(normalize_path(path) for path in expected)
    if actual_prefix != expected_prefix:
        raise SystemExit(
            f"{label} runtime order mismatch:\n"
            f"expected: {expected_prefix}\n"
            f"actual:   {actual_prefix}"
        )


def check_effective_environment(runtime_dirs: tuple[Path, ...], plugins: Path) -> None:
    require_path_prefix("effective PATH", os.environ.get("PATH", ""), runtime_dirs)
    actual_plugins = os.environ.get("QT_PLUGIN_PATH", "")
    if not actual_plugins or normalize_path(actual_plugins) != normalize_path(plugins):
        raise SystemExit(
            f"effective QT_PLUGIN_PATH mismatch: expected {plugins}, got {actual_plugins}"
        )
    actual_qpa = os.environ.get("QT_QPA_PLATFORM")
    if actual_qpa != "offscreen":
        raise SystemExit(
            f"effective QT_QPA_PLATFORM mismatch: expected offscreen, got {actual_qpa}"
        )


def check_registration(
    ctest: Path, build_dir: Path, runtime_dirs: tuple[Path, ...], plugins: Path
) -> None:
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
    environment, modifications = parse_verbose_runtime(result.stdout)

    if modifications:
        effective_order: list[str] = []
        for runtime_dir in modifications:
            effective_order.insert(0, normalize_path(runtime_dir))
        expected_order = [normalize_path(path) for path in runtime_dirs]
        if effective_order != expected_order:
            raise SystemExit(
                "registered PATH modification order mismatch:\n"
                f"expected effective order: {expected_order}\n"
                f"actual effective order:   {effective_order}"
            )
    else:
        require_path_prefix("registered PATH", environment.get("PATH", ""), runtime_dirs)

    registered_plugins = environment.get("QT_PLUGIN_PATH", "")
    if not registered_plugins or normalize_path(registered_plugins) != normalize_path(
        plugins
    ):
        raise SystemExit(
            "registered QT_PLUGIN_PATH mismatch: "
            f"expected {plugins}, got {registered_plugins}"
        )
    if environment.get("QT_QPA_PLATFORM") != "offscreen":
        raise SystemExit("registered QT_QPA_PLATFORM is not exactly offscreen")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--qt-runtime-dir", type=Path, required=True)
    parser.add_argument("--qt-plugin-dir", type=Path, required=True)
    args = parser.parse_args()

    build_dir = require_directory("CMake build", args.build_dir)
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    runtime_dirs, plugins = configured_runtime(
        cache, args.qt_runtime_dir, args.qt_plugin_dir
    )
    ctest_value = cache.get("CMAKE_CTEST_COMMAND", "").strip()
    ctest = (
        Path(ctest_value)
        if ctest_value
        else Path(require_cache_value(cache, "CMAKE_COMMAND")).with_name("ctest.exe")
    )
    if not ctest.is_file():
        raise SystemExit(f"CTest executable does not exist: {ctest}")

    check_effective_environment(runtime_dirs, plugins)
    check_registration(ctest, build_dir, runtime_dirs, plugins)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
