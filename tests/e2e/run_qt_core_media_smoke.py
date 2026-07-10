#!/usr/bin/env python3
"""Run Qt core-media surfaces and the real app from an isolated package."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path
from typing import NamedTuple, Sequence


EXPECTED_FFMPEG_ABIS = {
    "avcodec": "62",
    "avformat": "62",
    "avutil": "60",
    "swresample": "6",
    "swscale": "9",
}


class PackageLayout(NamedTuple):
    package_root: Path
    executable_dir: Path
    plugin_dir: Path
    qml_dir: Path
    app_executable: Path


class ProcessEvidenceError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        stdout: str = "",
        stderr: str = "",
        modules: Sequence[Path] = (),
    ) -> None:
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr
        self.modules = list(modules)


def package_layout(package: Path, platform: str) -> PackageLayout:
    package = package.resolve()
    if platform == "windows":
        return PackageLayout(package, package, package, package / "qml", package / "OpenLiveReplay.exe")
    if platform == "linux":
        root = package / "usr"
        return PackageLayout(
            package,
            root / "bin",
            root / "plugins",
            root / "qml",
            root / "bin" / "OpenLiveReplay",
        )
    if platform == "macos":
        contents = package / "Contents"
        return PackageLayout(
            package,
            contents / "MacOS",
            contents / "PlugIns",
            contents / "Resources" / "qml",
            contents / "MacOS" / "OpenLiveReplay",
        )
    raise RuntimeError(f"unsupported desktop platform: {platform}")


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except (OSError, ValueError):
        return False


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _write_json(path: Path, content: object) -> None:
    _write_text(path, json.dumps(content, indent=2, sort_keys=True) + "\n")


def _module_snapshot(modules: Sequence[Path]) -> list[str]:
    return sorted({str(Path(path).resolve()) for path in modules}, key=str.casefold)


def _write_process_evidence(
    evidence_dir: Path,
    prefix: str,
    stdout: str,
    stderr: str,
    modules: Sequence[Path],
) -> None:
    _write_text(evidence_dir / f"{prefix}-stdout.log", stdout)
    _write_text(evidence_dir / f"{prefix}-stderr.log", stderr)
    _write_json(evidence_dir / f"{prefix}-loaded-modules.json", _module_snapshot(modules))


def _component_abi(name: str, component: str, platform: str) -> str | None:
    escaped = re.escape(component)
    patterns = {
        "windows": rf"^(?:lib)?{escaped}-(\d+)\.dll$",
        "linux": rf"^lib{escaped}\.so\.(\d+)(?:\..*)?$",
        "macos": rf"^lib{escaped}\.(\d+)(?:\..*)?\.dylib$",
    }
    match = re.match(patterns[platform], name, flags=re.IGNORECASE)
    return match.group(1) if match else None


def validate_loaded_ffmpeg_modules(
    modules: Sequence[Path], package: Path, platform: str, *, require_any: bool = True
) -> dict[str, str]:
    observed: dict[str, str] = {}
    for module in sorted({Path(path) for path in modules}, key=lambda path: str(path).casefold()):
        name = module.name.casefold()
        if "ffmpegmediaplugin" in name or "qt6ffmpegmediapluginimpl" in name:
            raise RuntimeError(f"Qt FFmpeg module loaded: {module}")
        for component, expected_abi in EXPECTED_FFMPEG_ABIS.items():
            if component not in name:
                continue
            abi = _component_abi(module.name, component, platform)
            if abi is None:
                raise RuntimeError(f"unversioned or unrecognized FFmpeg module loaded: {module}")
            if abi != expected_abi:
                raise RuntimeError(
                    f"unexpected FFmpeg ABI for {component}: {abi} != {expected_abi} ({module})"
                )
            if not _inside(module, package):
                raise RuntimeError(f"controlled FFmpeg module loaded outside isolated package: {module}")
            observed[component] = abi
            break
    if require_any and not observed:
        raise RuntimeError("packaged app did not load any controlled FFmpeg components")
    return observed


def validate_native_backend_modules(
    modules: Sequence[Path],
    package: Path,
    platform: str,
    environment: dict[str, str],
) -> dict[str, object]:
    ordered = sorted({Path(path) for path in modules}, key=lambda path: str(path).casefold())
    qt_ffmpeg = [
        path
        for path in ordered
        if "ffmpegmediaplugin" in path.name.casefold()
        or "qt6ffmpegmediapluginimpl" in path.name.casefold()
    ]
    if qt_ffmpeg:
        raise RuntimeError(f"Qt FFmpeg backend/plugin loaded: {qt_ffmpeg[0]}")

    selected = environment.get("QT_MEDIA_BACKEND")
    if platform == "linux":
        if selected is not None:
            raise RuntimeError(
                f"Linux must clear inherited QT_MEDIA_BACKEND, found {selected!r}"
            )
        return {"selection": "default", "environment": None, "module": None}

    expected = "windows" if platform == "windows" else "darwin"
    if selected != expected:
        raise RuntimeError(
            f"native Qt backend mismatch: QT_MEDIA_BACKEND={selected!r}, expected {expected!r}"
        )
    if platform == "windows":
        candidates = [path for path in ordered if path.name.casefold() == "windowsmediaplugin.dll"]
        required_name = "windowsmediaplugin.dll"
    else:
        stock_names = {
            "darwinmediaplugin.dylib",
            "libdarwinmediaplugin.dylib",
            "qdarwinmediaplugin.dylib",
            "libqdarwinmediaplugin.dylib",
        }
        candidates = [path for path in ordered if path.name.casefold() in stock_names]
        required_name = "libdarwinmediaplugin.dylib"
    if not candidates:
        raise RuntimeError(f"native Qt backend module was not loaded: {required_name}")
    outside = [path for path in candidates if not _inside(path, package)]
    if outside:
        raise RuntimeError(f"native Qt backend module loaded outside isolated package: {outside[0]}")
    module = candidates[0].resolve()
    return {"selection": expected, "environment": selected, "module": str(module)}


def validate_media_payload(
    payload: object, platform: str, allow_no_audio_device: bool
) -> dict[str, object]:
    if not isinstance(payload, dict):
        raise RuntimeError(f"media harness payload must be a JSON object, got {type(payload).__name__}")
    expected_backend = (
        "default" if platform == "linux" else ("windows" if platform == "windows" else "darwin")
    )
    if payload.get("backend") != expected_backend:
        raise RuntimeError(
            f"media harness selected backend {payload.get('backend')!r}, expected {expected_backend!r}"
        )
    frames = payload.get("videoFramesObserved")
    if isinstance(frames, bool) or not isinstance(frames, int):
        raise RuntimeError(f"media harness videoFramesObserved is not an integer: {payload}")
    if frames < 2:
        raise RuntimeError(f"media harness observed fewer than two video frames: {payload}")
    audio = payload.get("audio")
    if audio != "started" and not (allow_no_audio_device and audio == "no-device"):
        raise RuntimeError(f"media harness audio did not start: {payload}")
    return payload


def reject_qt_ffmpeg_logs(log_text: str) -> None:
    lowered = log_text.casefold()
    markers = (
        "qt.multimedia.ffmpeg",
        "qffmpegmediaplugin",
        "ffmpegmediaplugin",
        "using ffmpeg backend",
        "ffmpeg backend initialized",
    )
    found = [marker for marker in markers if marker in lowered]
    if found:
        raise RuntimeError(f"Qt FFmpeg backend log detected: {', '.join(found)}")


def _windows_modules(pid: int) -> list[Path]:
    from ctypes import wintypes

    process_query_information = 0x0400
    process_vm_read = 0x0010
    list_modules_all = 0x03
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    psapi.EnumProcessModulesEx.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.HMODULE),
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.DWORD,
    )
    psapi.EnumProcessModulesEx.restype = wintypes.BOOL
    psapi.GetModuleFileNameExW.argtypes = (
        wintypes.HANDLE,
        wintypes.HMODULE,
        wintypes.LPWSTR,
        wintypes.DWORD,
    )
    psapi.GetModuleFileNameExW.restype = wintypes.DWORD
    handle = kernel32.OpenProcess(process_query_information | process_vm_read, False, pid)
    if not handle:
        raise RuntimeError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
    try:
        modules = (wintypes.HMODULE * 4096)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(
            handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), list_modules_all
        ):
            raise RuntimeError(f"EnumProcessModulesEx failed: {ctypes.get_last_error()}")
        count = min(needed.value // ctypes.sizeof(wintypes.HMODULE), len(modules))
        paths: list[Path] = []
        for module in modules[:count]:
            buffer = ctypes.create_unicode_buffer(32768)
            if psapi.GetModuleFileNameExW(handle, module, buffer, len(buffer)):
                paths.append(Path(buffer.value))
        return paths
    finally:
        kernel32.CloseHandle(handle)


def _linux_modules(pid: int) -> list[Path]:
    maps = Path(f"/proc/{pid}/maps")
    try:
        lines = maps.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise RuntimeError(f"cannot inspect {maps}: {error}") from error
    paths: list[Path] = []
    for line in lines:
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and fields[5].startswith("/"):
            paths.append(Path(fields[5].removesuffix(" (deleted)")))
    return paths


def _macos_modules(pid: int) -> list[Path]:
    lsof = Path("/usr/sbin/lsof")
    if not lsof.is_file():
        raise RuntimeError("/usr/sbin/lsof is required for macOS loaded-module inspection")
    completed = subprocess.run(
        [str(lsof), "-Fn", "-p", str(pid)], check=True, text=True, capture_output=True
    )
    return [Path(line[1:]) for line in completed.stdout.splitlines() if line.startswith("n/")]


def loaded_modules(pid: int, platform: str) -> list[Path]:
    if platform == "windows":
        return _windows_modules(pid)
    if platform == "linux":
        return _linux_modules(pid)
    if platform == "macos":
        return _macos_modules(pid)
    raise RuntimeError(f"unsupported platform for module inspection: {platform}")


def _run_checked(command: Sequence[str], *, env: dict[str, str] | None = None) -> None:
    completed = subprocess.run(command, env=env, text=True, capture_output=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def _prepare_harness(harness: Path, layout: PackageLayout, platform: str) -> Path:
    destination = layout.executable_dir / harness.name
    shutil.copy2(harness, destination)
    destination.chmod(destination.stat().st_mode | 0o111)
    if platform == "linux":
        patchelf = shutil.which("patchelf")
        if not patchelf:
            raise RuntimeError("patchelf is required for the isolated Linux harness")
        _run_checked([patchelf, "--set-rpath", "$ORIGIN/../lib", str(destination)])
    elif platform == "macos":
        otool = shutil.which("otool")
        install_name_tool = shutil.which("install_name_tool")
        if not otool or not install_name_tool:
            raise RuntimeError("otool and install_name_tool are required for the isolated macOS harness")
        details = subprocess.run([otool, "-l", str(destination)], check=True, text=True, capture_output=True).stdout
        rpaths = re.findall(r"path (\S+) \(offset \d+\)", details)
        for rpath in rpaths:
            _run_checked([install_name_tool, "-delete_rpath", rpath, str(destination)])
        _run_checked(
            [install_name_tool, "-add_rpath", "@executable_path/../Frameworks", str(destination)]
        )
    return destination


def _isolated_environment(layout: PackageLayout, platform: str, documents_root: Path) -> dict[str, str]:
    environment = os.environ.copy()
    for name in (
        "DYLD_FALLBACK_FRAMEWORK_PATH",
        "DYLD_FALLBACK_LIBRARY_PATH",
        "DYLD_FRAMEWORK_PATH",
        "DYLD_LIBRARY_PATH",
        "LD_LIBRARY_PATH",
        "QML2_IMPORT_PATH",
        "QML_IMPORT_PATH",
        "QT_MEDIA_BACKEND",
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
    ):
        environment.pop(name, None)
    environment["QT_PLUGIN_PATH"] = str(layout.plugin_dir)
    environment["QML2_IMPORT_PATH"] = str(layout.qml_dir)
    environment["QML_IMPORT_PATH"] = str(layout.qml_dir)
    environment["OLR_DOCUMENTS_ROOT"] = str(documents_root)
    if platform == "windows":
        environment["QT_MEDIA_BACKEND"] = "windows"
        system_root = Path(environment.get("SystemRoot", "C:/Windows"))
        environment["PATH"] = os.pathsep.join(
            (str(layout.executable_dir), str(system_root / "System32"), str(system_root))
        )
    elif platform == "macos":
        environment["QT_MEDIA_BACKEND"] = "darwin"
        environment["DYLD_FRAMEWORK_PATH"] = str(layout.package_root / "Contents" / "Frameworks")
        environment["DYLD_LIBRARY_PATH"] = str(layout.package_root / "Contents" / "Frameworks")
    else:
        environment["LD_LIBRARY_PATH"] = str(layout.package_root / "usr" / "lib")
        environment["QT_QPA_PLATFORM"] = "offscreen"
    return environment


def _free_control_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _read_json_while_alive(
    process: subprocess.Popen[str], platform: str, timeout: float
) -> tuple[dict[str, object], list[Path], str, str]:
    assert process.stdout is not None
    result: queue.Queue[tuple[str, object]] = queue.Queue(maxsize=1)

    def read_line() -> None:
        try:
            result.put(("line", process.stdout.readline()))
        except BaseException as error:
            result.put(("error", error))

    threading.Thread(target=read_line, name="qt-media-json-reader", daemon=True).start()
    try:
        kind, value = result.get(timeout=timeout)
    except queue.Empty as error:
        raise TimeoutError(f"media harness did not emit JSON within {timeout:.1f}s") from error
    if kind == "error":
        raise ProcessEvidenceError(f"cannot read media harness JSON: {value}")
    line = str(value)
    if not line:
        stdout, stderr = process.communicate(timeout=1)
        raise ProcessEvidenceError(
            f"media harness exited before JSON (code {process.returncode})\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}",
            stdout=stdout,
            stderr=stderr,
        )
    try:
        payload = json.loads(line)
    except json.JSONDecodeError as error:
        raise ProcessEvidenceError(
            f"media harness emitted invalid JSON: {line.rstrip()}",
            stdout=line,
        ) from error
    try:
        modules = loaded_modules(process.pid, platform)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        raise ProcessEvidenceError(
            f"cannot inspect media harness loaded modules: {error}", stdout=line
        ) from error
    stdout, stderr = process.communicate(timeout=timeout)
    return payload, modules, line + stdout, stderr


def _run_harness(
    harness: Path,
    layout: PackageLayout,
    platform: str,
    environment: dict[str, str],
    allow_no_audio_device: bool,
    timeout: float,
    evidence_dir: Path,
) -> tuple[dict[str, object], dict[str, object]]:
    command = [str(harness), "--hold-ms", "1500"]
    if allow_no_audio_device:
        command.append("--allow-no-audio-device")
    _write_process_evidence(evidence_dir, "harness", "", "", [])
    process: subprocess.Popen[str] | None = None
    modules: list[Path] = []
    stdout = ""
    stderr = ""
    try:
        process = subprocess.Popen(
            command,
            cwd=layout.executable_dir,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        payload, modules, stdout, stderr = _read_json_while_alive(process, platform, timeout)
    except BaseException as error:
        stdout = getattr(error, "stdout", "")
        stderr = getattr(error, "stderr", "")
        modules = list(getattr(error, "modules", []))
        if process is not None:
            if process.poll() is None and not modules:
                try:
                    modules = loaded_modules(process.pid, platform)
                except (OSError, RuntimeError, subprocess.SubprocessError):
                    pass
            if process.poll() is None:
                process.kill()
            try:
                remaining_stdout, remaining_stderr = process.communicate(timeout=5)
                stdout += remaining_stdout
                stderr += remaining_stderr
            except subprocess.TimeoutExpired:
                process.kill()
                remaining_stdout, remaining_stderr = process.communicate()
                stdout += remaining_stdout
                stderr += remaining_stderr
        _write_process_evidence(evidence_dir, "harness", stdout, stderr, modules)
        raise
    _write_process_evidence(evidence_dir, "harness", stdout, stderr, modules)
    assert process is not None
    _write_json(evidence_dir / "harness-media.json", payload)
    if process.returncode != 0:
        raise RuntimeError(
            f"media harness failed ({process.returncode})\nstdout:\n{stdout}\nstderr:\n{stderr}"
        )
    reject_qt_ffmpeg_logs(stdout + stderr)
    validate_loaded_ffmpeg_modules(modules, layout.package_root, platform, require_any=False)
    backend_evidence = validate_native_backend_modules(
        modules, layout.package_root, platform, environment
    )
    media = validate_media_payload(payload, platform, allow_no_audio_device)
    _write_json(evidence_dir / "harness-media.json", media)
    return media, backend_evidence


def _inspect_packaged_app(
    layout: PackageLayout,
    platform: str,
    environment: dict[str, str],
    timeout: float,
    evidence_dir: Path,
) -> tuple[dict[str, str], list[str]]:
    app_environment = environment.copy()
    app_environment["OLR_CONTROL_PORT"] = str(_free_control_port())
    _write_process_evidence(evidence_dir, "packaged-app", "", "", [])
    process: subprocess.Popen[str] | None = None
    modules: list[Path] = []
    stdout = ""
    stderr = ""
    communicated = False
    try:
        process = subprocess.Popen(
            [str(layout.app_executable)],
            cwd=layout.executable_dir,
            env=app_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.05)
            if time.monotonic() + 0.5 >= deadline:
                break
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            communicated = True
            raise RuntimeError(
                f"packaged app exited before module inspection ({process.returncode})\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            )
        modules = loaded_modules(process.pid, platform)
    finally:
        if process is not None:
            if process.poll() is None:
                process.terminate()
            if not communicated:
                try:
                    stdout, stderr = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    stdout, stderr = process.communicate()
        _write_process_evidence(evidence_dir, "packaged-app", stdout, stderr, modules)
    reject_qt_ffmpeg_logs(stdout + stderr)
    observed = validate_loaded_ffmpeg_modules(modules, layout.package_root, platform)
    ffmpeg_paths = sorted(
        str(path) for path in modules if any(component in path.name.casefold() for component in EXPECTED_FFMPEG_ABIS)
    )
    return observed, ffmpeg_paths


def _audit_package(
    package: Path,
    platform: str,
    controlled_prefixes: Sequence[str],
    audit_script: Path,
    policy: Path,
    evidence_dir: Path,
) -> tuple[Path, Path]:
    evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence = evidence_dir / f"OpenLiveReplay-{platform}-runtime-evidence.json"
    spdx = evidence_dir / f"OpenLiveReplay-{platform}-runtime.spdx.json"
    command = [
        sys.executable,
        str(audit_script),
        "--package",
        str(package),
        "--platform",
        platform,
        "--policy",
        str(policy),
    ]
    for prefix in controlled_prefixes:
        command.extend(("--controlled-prefix", prefix))
    command.extend(("--evidence", str(evidence), "--spdx", str(spdx)))
    _run_checked(command)
    return evidence, spdx


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "macos", "linux"))
    parser.add_argument("--controlled-prefix", action="append", required=True, default=[])
    parser.add_argument("--allow-no-audio-device", action="store_true")
    parser.add_argument("--startup-timeout", type=float, default=8.0)
    parser.add_argument(
        "--audit-script",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "build-scripts" / "audit_single_ffmpeg.py",
    )
    parser.add_argument(
        "--policy",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "build-scripts" / "single_ffmpeg_policy.json",
    )
    parser.add_argument("--evidence-dir", type=Path)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    evidence_dir = (
        args.evidence_dir.resolve()
        if args.evidence_dir
        else args.package.resolve().parent / "runtime-evidence"
    )
    evidence_dir.mkdir(parents=True, exist_ok=True)
    error_path = evidence_dir / "runtime-error.txt"
    error_path.unlink(missing_ok=True)
    try:
        if not args.package.is_dir():
            raise RuntimeError(f"package is not a directory: {args.package}")
        if not args.harness.is_file():
            raise RuntimeError(f"media harness does not exist: {args.harness}")
        with tempfile.TemporaryDirectory(prefix="olr-qt-core-media-") as temporary:
            temporary_root = Path(temporary)
            isolated_package = temporary_root / args.package.name
            shutil.copytree(args.package, isolated_package, symlinks=True)
            layout = package_layout(isolated_package, args.platform)
            for required in (
                layout.executable_dir,
                layout.plugin_dir,
                layout.qml_dir,
                layout.app_executable,
            ):
                if not required.exists():
                    raise RuntimeError(f"isolated package is incomplete: {required}")
            evidence, spdx = _audit_package(
                layout.package_root,
                args.platform,
                args.controlled_prefix,
                args.audit_script.resolve(),
                args.policy.resolve(),
                evidence_dir,
            )
            harness = _prepare_harness(args.harness.resolve(), layout, args.platform)
            environment = _isolated_environment(
                layout, args.platform, temporary_root / "documents"
            )
            media, backend_evidence = _run_harness(
                harness,
                layout,
                args.platform,
                environment,
                args.allow_no_audio_device,
                args.startup_timeout,
                evidence_dir,
            )
            _write_json(evidence_dir / "harness-media.json", media)
            abis, module_paths = _inspect_packaged_app(
                layout, args.platform, environment, args.startup_timeout, evidence_dir
            )
            result = {
                "auditEvidence": str(evidence),
                "backendEvidence": backend_evidence,
                "harnessLoadedModulesEvidence": str(
                    evidence_dir / "harness-loaded-modules.json"
                ),
                "loadedFFmpegAbis": abis,
                "loadedFFmpegModules": module_paths,
                "media": media,
                "packagedAppLoadedModulesEvidence": str(
                    evidence_dir / "packaged-app-loaded-modules.json"
                ),
                "spdx": str(spdx),
            }
            _write_json(evidence_dir / "runtime-result.json", result)
            print(json.dumps(result, sort_keys=True), flush=True)
    except BaseException as error:
        _write_text(error_path, traceback.format_exc())
        _write_json(
            evidence_dir / "runtime-result.json",
            {
                "diagnostic": str(error_path),
                "error": str(error),
                "status": "failed",
            },
        )
        raise
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
