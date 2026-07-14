#!/usr/bin/env python3
"""Check generated and effective Windows runtime environments for CTest."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
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
) -> tuple[tuple[Path, ...], Path, Path]:
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
    qml = require_directory("Qt QML imports", qt_runtime_dir.parent / "qml")
    return runtime_dirs, plugins, qml


def parse_verbose_runtime(output: str) -> tuple[dict[str, str], dict[str, list[str]]]:
    environment: dict[str, str] = {}
    modifications: dict[str, list[str]] = {}
    for line in output.splitlines():
        test_number, separator, detail = line.partition(":")
        if not separator or not test_number.strip().isdigit():
            continue
        entry = detail.strip()
        for key in ("PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
            modification_prefix = f"{key}=path_list_prepend:"
            if entry.startswith(modification_prefix):
                modifications.setdefault(key, []).append(
                    entry.removeprefix(modification_prefix)
                )
                break
        else:
            key, value_separator, value = entry.partition("=")
            if value_separator and key in {
                "PATH",
                "QT_PLUGIN_PATH",
                "QT_QPA_PLATFORM",
                "QML_IMPORT_PATH",
                "QML2_IMPORT_PATH",
            }:
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


def check_effective_environment(
    runtime_dirs: tuple[Path, ...], plugins: Path, qml: Path
) -> None:
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
    require_path_prefix("effective QML_IMPORT_PATH", os.environ.get("QML_IMPORT_PATH", ""), (qml,))
    require_path_prefix("effective QML2_IMPORT_PATH", os.environ.get("QML2_IMPORT_PATH", ""), (qml,))


def check_registration(
    ctest: Path,
    build_dir: Path,
    runtime_dirs: tuple[Path, ...],
    plugins: Path,
    qml: Path,
) -> None:
    for test_name in ("tst_muxer", "tst_qml_codecsettings", "tst_olrstyle"):
        result = subprocess.run(
            [
                str(ctest),
                "--test-dir",
                str(build_dir),
                "-N",
                "-V",
                "-R",
                f"^{test_name}$",
            ],
            text=True,
            capture_output=True,
            check=True,
        )
        environment, modifications = parse_verbose_runtime(result.stdout)

        if modifications.get("PATH"):
            effective_order: list[str] = []
            for runtime_dir in modifications["PATH"]:
                effective_order.insert(0, normalize_path(runtime_dir))
            expected_order = [normalize_path(path) for path in runtime_dirs]
            if effective_order != expected_order:
                raise SystemExit(
                    f"{test_name} registered PATH modification order mismatch:\n"
                    f"expected effective order: {expected_order}\n"
                    f"actual effective order:   {effective_order}"
                )
        else:
            require_path_prefix(
                f"{test_name} registered PATH",
                environment.get("PATH", ""),
                runtime_dirs,
            )

        registered_plugins = environment.get("QT_PLUGIN_PATH", "")
        if not registered_plugins or normalize_path(registered_plugins) != normalize_path(
            plugins
        ):
            raise SystemExit(
                f"{test_name} registered QT_PLUGIN_PATH mismatch: "
                f"expected {plugins}, got {registered_plugins}"
            )
        if environment.get("QT_QPA_PLATFORM") != "offscreen":
            raise SystemExit(
                f"{test_name} registered QT_QPA_PLATFORM is not exactly offscreen"
            )
        for variable in ("QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
            if modifications.get(variable):
                actual = tuple(Path(value) for value in reversed(modifications[variable]))
                require_path_prefix(
                    f"{test_name} registered {variable} modification",
                    os.pathsep.join(str(path) for path in actual),
                    (qml,),
                )
            else:
                require_path_prefix(
                    f"{test_name} registered {variable}",
                    environment.get(variable, ""),
                    (qml,),
                )


def scan_policy_violations(
    checks: dict[str, tuple[Path, str]],
) -> list[str]:
    """Return source-policy matches with stable file and line diagnostics."""
    failures: list[str] = []
    for label, (path, pattern) in checks.items():
        candidates = (
            sorted(candidate for candidate in path.rglob("*") if candidate.is_file())
            if path.is_dir()
            else [path]
        )
        for candidate in candidates:
            text = candidate.read_text(encoding="utf-8")
            for match in re.finditer(pattern, text):
                line = text.count("\n", 0, match.start()) + 1
                failures.append(f"{label}: {candidate}:{line}: {match.group(0).strip()}")
    return failures


def check_source_policy(root: Path) -> None:
    checks = {
        "raw test executable": (
            root / "docs/superpowers/plans",
            r"tst_[A-Za-z0-9_]+\.exe",
        ),
        "raw Windows SRT documentation CTest": (
            root / "tests/e2e/SRT_README.md",
            r"(?m)^\s*ctest\s",
        ),
        "raw Windows application documentation launch": (
            root / "docs/build-and-run.md",
            r"(?mi)^\s*(?:\.{0,2}[/\\])?[^\r\n]*OpenLiveReplay\.exe(?:\s|$)",
        ),
        "pre-push bypass": (
            root / ".githooks/pre-push",
            r'(?m)^\s*(?!command ctest "\$@"\s*$)'
            r'(?:[A-Za-z_][A-Za-z0-9_]*="\$\()?'
            r'(?:command\s+)?ctest\s',
        ),
    }
    failures = scan_policy_violations(checks)

    tasks = json.loads((root / ".vscode/tasks.json").read_text(encoding="utf-8"))
    for task in tasks.get("tasks", []):
        if task.get("command") != "ctest":
            continue
        windows = task.get("windows", {})
        windows_args = windows.get("args", [])
        if (
            task.get("type") != "process"
            or windows.get("command") != "python"
            or not any(str(arg).endswith("tools/run_ctest.py") for arg in windows_args)
        ):
            failures.append("plain Windows VS Code CTest launch: .vscode/tasks.json")

    workflow_paths = sorted((root / ".github/workflows").glob("*.y*ml"))
    windows_job_count = 0
    for workflow_path in workflow_paths:
        workflow = workflow_path.read_text(encoding="utf-8")
        for windows_job_match in re.finditer(
            r"(?ms)^  [A-Za-z0-9_-]+:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
            workflow,
        ):
            windows_job = windows_job_match.group(1)
            if not re.search(r"(?m)^\s*runs-on:\s*windows", windows_job):
                continue
            windows_job_count += 1
            raw_launch_pattern = (
                r"(?m)^\s*(?:(?:run:\s*)?(?:command\s+)?ctest\s|"
                r'"?\$\{?[A-Za-z_][A-Za-z0-9_]*(?:test|probe|app|exe|binary)'
                r'[A-Za-z0-9_]*\}?"?(?:\s|$)|'
                r'(?:[A-Za-z]:)?[/\\][^\s]*(?:tst_|rhi_|OpenLiveReplay)'
                r"[^\s]*\.exe(?:\s|$))"
            )
            for match in re.finditer(raw_launch_pattern, windows_job, re.IGNORECASE):
                line = (
                    workflow.count(
                        "\n", 0, windows_job_match.start(1) + match.start()
                    )
                    + 1
                )
                failures.append(
                    "raw Windows CI test/probe launch: "
                    f"{workflow_path.relative_to(root)}:{line}: "
                    f"{match.group(0).strip()}"
                )
    if not windows_job_count:
        failures.append("Windows CI job not found: .github/workflows")

    launch_text = (root / ".vscode/launch.json").read_text(encoding="utf-8")
    launch = json.loads(re.sub(r"(?m)^\s*//.*$", "", launch_text))
    launch_names = {
        configuration.get("name")
        for configuration in launch.get("configurations", [])
        if configuration.get("name")
    }
    for configuration in launch.get("configurations", []):
        if configuration.get("type") == "qml" and configuration.get("request") == "launch":
            failures.append(
                "ambient Windows QML app launch: .vscode/launch.json: "
                f"{configuration.get('name', '<unnamed>')}"
            )

    build_docs = (root / "docs/build-and-run.md").read_text(encoding="utf-8")
    for match in re.finditer(
        r"Launch the result with the `([^`]+)` VS Code", build_docs
    ):
        if match.group(1) not in launch_names:
            failures.append(
                "missing documented Windows VS Code launch configuration: "
                f"docs/build-and-run.md: {match.group(1)}"
            )

    if failures:
        raise SystemExit("\n".join(failures))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--qt-runtime-dir", type=Path)
    parser.add_argument("--qt-plugin-dir", type=Path)
    args = parser.parse_args()

    if args.source_root is not None:
        check_source_policy(args.source_root.resolve())
        return 0
    if args.build_dir is None or args.qt_runtime_dir is None or args.qt_plugin_dir is None:
        parser.error(
            "runtime mode requires --build-dir, --qt-runtime-dir, and --qt-plugin-dir"
        )

    build_dir = require_directory("CMake build", args.build_dir)
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    runtime_dirs, plugins, qml = configured_runtime(
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

    check_effective_environment(runtime_dirs, plugins, qml)
    check_registration(ctest, build_dir, runtime_dirs, plugins, qml)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
