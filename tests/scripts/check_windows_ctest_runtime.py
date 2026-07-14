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


def job_can_run_on_windows(job_body: str) -> bool:
    """Classify Windows-capable jobs, failing closed for dynamic runners."""
    match = re.search(r"(?m)^[ \t]*runs-on:[ \t]*(.*?)[ \t]*$", job_body)
    if not match:
        return False
    runner = match.group(1).strip().strip("'\"")
    if not runner:
        following = job_body[match.end() :].lstrip("\r\n")
        sequence = re.match(r"(?m)((?:^[ \t]+-[ \t]*[^\n]+\n?)+)", following)
        if sequence:
            tags = sequence.group(1).casefold()
            if "windows" in tags:
                return True
            if "ubuntu" in tags or "linux" in tags or "macos" in tags:
                return False
        return True
    lowered = runner.casefold()
    if "windows" in lowered:
        return True
    matrix_match = re.fullmatch(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}", runner)
    if matrix_match:
        key = re.escape(matrix_match.group(1))
        inline = re.search(rf"(?m)^\s*{key}:\s*\[([^]]*)\]", job_body)
        if inline:
            return "windows" in inline.group(1).casefold()
        block = re.search(
            rf"(?ms)^\s*{key}:\s*\n((?:\s+-\s*[^\n]+\n?)+)", job_body
        )
        if block:
            return "windows" in block.group(1).casefold()
        return True
    if "${{" in runner:
        return True
    return not ("ubuntu" in lowered or "linux" in lowered or "macos" in lowered)


def find_raw_windows_launches(job_body: str) -> list[tuple[int, str]]:
    """Return direct Windows CTest/test/app/probe shell launches."""
    failures: list[tuple[int, str]] = []
    for line_number, line in enumerate(job_body.splitlines(), start=1):
        command = line.strip()
        if command.startswith("run:"):
            command = command.removeprefix("run:").strip()
        if not command or command in {"|", ">"} or command.startswith("#"):
            continue
        if re.match(r"^(?:command\s+)?ctest\b", command, re.IGNORECASE):
            failures.append((line_number, command))
            continue
        if re.match(
            r"^(?:&\s*|Start-Process\s+|cmd(?:\.exe)?\s+/c\s+).*(?:tst_|rhi_|OpenLiveReplay|\.exe\b)",
            command,
            re.IGNORECASE,
        ):
            failures.append((line_number, command))
            continue
        if re.match(
            r'^"?\$(?:\{)?[A-Za-z_][A-Za-z0-9_]*(?:\})?"?(?:\s|$)',
            command,
            re.IGNORECASE,
        ):
            failures.append((line_number, command))
            continue
        if re.match(
            r'^["\']?(?:\.{1,2}[/\\]|[A-Za-z]:[/\\]|/)[^\s"\']*(?:tst_|rhi_|OpenLiveReplay)[^\s"\']*\.exe(?:["\']|\s|$)',
            command,
            re.IGNORECASE,
        ):
            failures.append((line_number, command))
    return failures


def validate_vscode_windows_contract(tasks: dict, launch: dict) -> list[str]:
    """Validate attach-only debug configurations and cache-derived app tasks."""
    failures: list[str] = []
    task_by_label = {
        task.get("label"): task for task in tasks.get("tasks", []) if task.get("label")
    }
    required_tasks = {
        "OpenLiveReplay: start controlled Windows app": False,
        "OpenLiveReplay: start controlled Windows QML-debug app": True,
    }
    for label, requires_qml in required_tasks.items():
        task = task_by_label.get(label)
        if task is None:
            failures.append(f"missing cache-derived Windows app task: {label}")
            continue
        args = [str(value) for value in task.get("args", [])]
        if (
            task.get("type") != "process"
            or task.get("command") != "python"
            or not any(value.endswith("tools/run_windows_app.py") for value in args)
            or "${command:cmake.buildDirectory}" not in args
            or "${command:cmake.launchTargetPath}" not in args
        ):
            failures.append(f"Windows app task is not cache-derived: {label}")
        if any("windows_build/dist" in value or "${env:PATH}" in value for value in args):
            failures.append(f"Windows app task contains ambient/workspace runtime roots: {label}")
        if requires_qml and not any(
            "qmljsdebugger" in value and "${command:qt-qml.debugPort}" in value
            for value in args
        ):
            failures.append(f"Windows QML-debug task lacks a controlled debugger port: {label}")

    qml_attach = False
    for configuration in launch.get("configurations", []):
        name = configuration.get("name", "<unnamed>")
        windows = configuration.get("windows")
        if (
            configuration.get("type") == "cppdbg"
            and configuration.get("request") == "launch"
            and not isinstance(windows, dict)
        ):
            failures.append(
                f"VS Code app launch is not fail-closed on Windows: {name}"
            )
        if isinstance(windows, dict):
            effective_request = windows.get("request", configuration.get("request"))
            if effective_request != "attach" or windows.get("processId") != "${command:pickProcess}":
                failures.append(f"Windows VS Code app configuration is not attach-only: {name}")
            serialized = json.dumps(windows)
            if (
                "${env:PATH}" in serialized
                or "windows_build/dist" in serialized
                or any(
                    entry.get("name") == "PATH"
                    for entry in windows.get("environment", [])
                    if isinstance(entry, dict)
                )
            ):
                failures.append(f"Windows VS Code configuration has hard-coded or ambient PATH: {name}")
        if configuration.get("type") == "qml":
            if configuration.get("request") == "launch":
                failures.append(f"ambient Windows QML app launch: .vscode/launch.json: {name}")
            if (
                configuration.get("request") == "attach"
                and configuration.get("host") == "localhost"
                and configuration.get("port") == "${command:qt-qml.debugPort}"
            ):
                qml_attach = True
    if not qml_attach:
        failures.append("missing real QML debugger attach configuration: .vscode/launch.json")
    return failures


def check_windows_quick_start_docs(text: str) -> list[str]:
    """Reject bare CTest in the general quick-start section unless non-Windows-qualified."""
    relevant = text.partition("The gate **hard-fails")[0]
    failures: list[str] = []
    previous_nonblank = ""
    for line_number, line in enumerate(relevant.splitlines(), start=1):
        if re.search(r"(?:^|[\s`])ctest\s", line) and not re.search(
            r"macOS/Linux", line, re.IGNORECASE
        ):
            if not re.search(r"macOS/Linux", previous_nonblank, re.IGNORECASE):
                failures.append(
                    f"bare CTest in Windows-capable test quick start: tests/README.md:{line_number}"
                )
        if line.strip():
            previous_nonblank = line
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

    launch_text = (root / ".vscode/launch.json").read_text(encoding="utf-8")
    launch = json.loads(re.sub(r"(?m)^\s*//.*$", "", launch_text))
    failures.extend(validate_vscode_windows_contract(tasks, launch))

    quick_start = (root / "tests/README.md").read_text(encoding="utf-8")
    failures.extend(check_windows_quick_start_docs(quick_start))

    workflow_paths = sorted((root / ".github/workflows").glob("*.y*ml"))
    windows_job_count = 0
    for workflow_path in workflow_paths:
        workflow = workflow_path.read_text(encoding="utf-8")
        workflow_windows_jobs: list[str] = []
        for windows_job_match in re.finditer(
            r"(?ms)^  [A-Za-z0-9_-]+:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
            workflow,
        ):
            windows_job = windows_job_match.group(1)
            if not job_can_run_on_windows(windows_job):
                continue
            windows_job_count += 1
            workflow_windows_jobs.append(windows_job)
            for relative_line, command in find_raw_windows_launches(windows_job):
                line = (
                    workflow.count(
                        "\n", 0, windows_job_match.start(1)
                    )
                    + relative_line
                )
                failures.append(
                    "raw Windows CI test/probe launch: "
                    f"{workflow_path.relative_to(root)}:{line}: "
                    f"{command}"
                )
        if workflow_windows_jobs and not any(
            "windows_source_launch_policy" in job for job in workflow_windows_jobs
        ):
            failures.append(
                "Windows workflow does not explicitly select windows_source_launch_policy: "
                f"{workflow_path.relative_to(root)}"
            )
    if not windows_job_count:
        failures.append("Windows CI job not found: .github/workflows")

    launch_names = {
        configuration.get("name")
        for configuration in launch.get("configurations", [])
        if configuration.get("name")
    }
    build_docs = (root / "docs/build-and-run.md").read_text(encoding="utf-8")
    for match in re.finditer(
        r"Launch the result with the `([^`]+)` VS Code", build_docs
    ):
        if match.group(1) not in launch_names:
            failures.append(
                "missing documented Windows VS Code launch configuration: "
                f"docs/build-and-run.md: {match.group(1)}"
            )
    for required_claim in (
        "tools/run_windows_app.py",
        "OpenLiveReplay: start controlled Windows app",
        "OpenLiveReplay: start controlled Windows QML-debug app",
        "OpenLiveReplay: Attach C++ and QML (Windows)",
    ):
        if required_claim not in build_docs:
            failures.append(
                "controlled Windows app documentation is missing enforced path: "
                f"{required_claim}"
            )

    smoke_cmake = (root / "tests/smoke/CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(
        r"add_test\(NAME\s+windows_source_launch_policy\b", smoke_cmake
    ) or not re.search(
        r"set_tests_properties\(windows_source_launch_policy\s+PROPERTIES\s+LABELS\s+\"smoke;ci\"",
        smoke_cmake,
    ):
        failures.append(
            "windows_source_launch_policy is not registered as a Windows smoke;ci CTest"
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
