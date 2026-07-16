"""Fail-closed compile-command normalization for the GPU capability audit."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath
from typing import Mapping

from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CompilerFamily,
    FileIdentity,
    PreprocessConfiguration,
)


_LAUNCHERS = frozenset({"ccache", "sccache", "distcc", "icecc"})
_CCACHE_VALUE_OPTIONS = frozenset({
    "--compiler",
    "--compiler-check",
    "--compiler-type",
    "--config-path",
    "--dir",
    "--namespace",
    "--set-config",
    "--trim-dir",
    "-o",
})
_CCACHE_FLAG_OPTIONS = frozenset({"--ccache-skip"})
_CCACHE_COMPILER_OPTIONS = frozenset({"--compiler", "--compiler-type"})
_CCACHE_COMPILER_ASSIGNMENTS = frozenset({
    "compiler", "compiler_type", "prefix_command", "prefix_command_cpp"
})
_CCACHE_COMPILER_ENVIRONMENT = frozenset({
    "ccache_compiler", "ccache_compilertype", "ccache_prefix", "ccache_prefix_cpp",
    "ccache_cc",
})
_ASSIGNMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_.-]*=(.*)\Z", re.DOTALL)
_GNU_VALUE_OPTIONS = frozenset({
    "-o", "-D", "-U", "-A", "-I", "-isystem", "-iquote", "-idirafter",
    "-iprefix", "-iwithprefix", "-iwithprefixbefore", "-include", "-imacros",
    "-isysroot", "--sysroot", "-x", "-target", "--target", "-arch", "-MF",
    "-MT", "-MQ", "-MJ", "-F", "-iframework", "-include-pch", "-include-pth",
    "-Xclang", "-Xpreprocessor", "-Xassembler", "-Xlinker", "-B", "-specs",
    "-wrapper", "-fmodule-file", "-fmodule-map-file",
})
_MSVC_VALUE_OPTIONS = frozenset({
    "/d", "/u", "/i", "/fi", "/fo", "/fe", "/fd", "/fp", "/yu", "/yc",
    "/sourcedependencies", "/external:i", "/ai", "/fu", "/ifcoutput",
    "/reference", "/headerunit", "/scanDependencies".casefold(),
})
_VERSION_SECONDS = 5.0
_VERSION_BYTES = 1024 * 1024


def _launcher_name(value: str) -> str:
    return Path(value.replace("\\", "/")).name.casefold().removesuffix(".exe")


def _windows_command_line_split(command: str) -> tuple[str, ...]:
    """Use CommandLineToArgvW on Windows and its documented rules elsewhere."""

    quoted = False
    slashes = 0
    for character in command:
        if character == "\\":
            slashes += 1
            continue
        if character == '"' and slashes % 2 == 0:
            quoted = not quoted
        slashes = 0
    if quoted:
        raise AuditInfrastructureError("unsupported Windows command quoting")

    if os.name == "nt":
        import ctypes

        argc = ctypes.c_int()
        shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        shell32.CommandLineToArgvW.argtypes = (
            ctypes.c_wchar_p,
            ctypes.POINTER(ctypes.c_int),
        )
        shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
        kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
        kernel32.LocalFree.restype = ctypes.c_void_p
        pointer = shell32.CommandLineToArgvW(command, ctypes.byref(argc))
        if not pointer:
            raise AuditInfrastructureError("CommandLineToArgvW failed")
        try:
            return tuple(pointer[index] for index in range(argc.value))
        finally:
            kernel32.LocalFree(pointer)

    arguments: list[str] = []
    index = 0
    length = len(command)
    while index < length:
        while index < length and command[index] in " \t":
            index += 1
        if index == length:
            break
        value: list[str] = []
        quoted = False
        while index < length and (quoted or command[index] not in " \t"):
            if command[index] == "\\":
                start = index
                while index < length and command[index] == "\\":
                    index += 1
                slashes = index - start
                if index < length and command[index] == '"':
                    value.extend("\\" * (slashes // 2))
                    if slashes % 2:
                        value.append('"')
                        index += 1
                    else:
                        if quoted and index + 1 < length and command[index + 1] == '"':
                            value.append('"')
                            index += 2
                        else:
                            quoted = not quoted
                            index += 1
                else:
                    value.extend("\\" * slashes)
                continue
            if command[index] == '"':
                if quoted and index + 1 < length and command[index + 1] == '"':
                    value.append('"')
                    index += 2
                else:
                    quoted = not quoted
                    index += 1
                continue
            value.append(command[index])
            index += 1
        if quoted:
            raise AuditInfrastructureError("unsupported Windows command quoting")
        arguments.append("".join(value))
    return tuple(arguments)


def _token_has_shell_syntax(token: str) -> bool:
    if "$(" in token or "`" in token:
        return True
    if token in {"|", "||", "&&", ";", "<", "<<", ">", ">>", "<>"}:
        return True
    return re.fullmatch(r"(?:\d+|&)?(?:>>?|<<?|<>|>&|<&).*", token) is not None


def _validate_arguments(arguments: tuple[str, ...]) -> None:
    if not arguments or not arguments[0]:
        raise AuditInfrastructureError("compile command is empty")
    for argument in arguments:
        if not isinstance(argument, str):
            raise AuditInfrastructureError("compile command arguments must be strings")
        if not argument or "\0" in argument:
            raise AuditInfrastructureError("compile command contains an empty or NUL argument")
        if _token_has_shell_syntax(argument):
            raise AuditInfrastructureError(f"unsupported shell syntax: {argument}")


def _posix_command_split(command: str) -> tuple[str, ...]:
    lexer = shlex.shlex(command, posix=True, punctuation_chars="|&;<>")
    lexer.whitespace_split = True
    lexer.commenters = ""
    return tuple(lexer)


def decode_compile_entry(
    entry: Mapping[str, object],
    database: Path,
    windows: bool,
) -> tuple[Path, tuple[str, ...]]:
    """Decode one compile database entry without invoking a shell."""

    if not isinstance(entry, Mapping):
        raise AuditInfrastructureError("compile database entry must be an object")
    directory_value = entry.get("directory")
    file_value = entry.get("file")
    if not isinstance(directory_value, str) or not directory_value or "\0" in directory_value:
        raise AuditInfrastructureError("compile command directory must be a non-empty string")
    if not isinstance(file_value, str) or not file_value or "\0" in file_value:
        raise AuditInfrastructureError("compile command file must be a non-empty string")
    directory = Path(directory_value)
    if not directory.is_absolute():
        directory = database.parent / directory
    try:
        directory = directory.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(
            f"compile command directory is unavailable: {directory_value}"
        ) from error
    if not directory.is_dir():
        raise AuditInfrastructureError(f"compile command directory is not a directory: {directory}")

    structured = entry.get("arguments")
    if structured is not None:
        if not isinstance(structured, list):
            raise AuditInfrastructureError("compile command arguments must be an array")
        if any(not isinstance(value, str) for value in structured):
            raise AuditInfrastructureError("compile command arguments must be strings")
        arguments = tuple(structured)
    else:
        command = entry.get("command")
        if not isinstance(command, str) or not command.strip():
            raise AuditInfrastructureError("compile command has neither arguments nor command")
        try:
            arguments = (
                _windows_command_line_split(command)
                if windows
                else _posix_command_split(command)
            )
        except (ValueError, OSError) as error:
            raise AuditInfrastructureError("unsupported compile command quoting") from error
    _validate_arguments(arguments)
    return directory, arguments


def strip_launchers(
    arguments: tuple[str, ...],
) -> tuple[Path, tuple[str, ...], Mapping[str, str]]:
    """Remove only the explicitly supported launcher grammar."""

    if not arguments:
        raise AuditInfrastructureError("compile command has no executable")
    index = 0
    assignments: dict[str, str] = {}
    while index < len(arguments) and _launcher_name(arguments[index]) in _LAUNCHERS:
        launcher = _launcher_name(arguments[index])
        index += 1
        if launcher != "ccache":
            if index < len(arguments) and arguments[index].startswith("-"):
                raise AuditInfrastructureError(
                    f"unsupported {launcher} option: {arguments[index]}"
                )
            continue
        while index < len(arguments):
            token = arguments[index]
            if token == "--":
                index += 1
                break
            option_name = token.partition("=")[0]
            if option_name in _CCACHE_VALUE_OPTIONS:
                if option_name in _CCACHE_COMPILER_OPTIONS:
                    raise AuditInfrastructureError(
                        f"ccache compiler override is unsupported: {token}"
                    )
                if "=" in token:
                    if not token.partition("=")[2]:
                        raise AuditInfrastructureError(
                            f"ccache option requires a value: {option_name}"
                        )
                    index += 1
                else:
                    if index + 1 >= len(arguments):
                        raise AuditInfrastructureError(
                            f"ccache option requires a value: {token}"
                        )
                    index += 2
                continue
            if token in _CCACHE_FLAG_OPTIONS:
                index += 1
                continue
            assignment = _ASSIGNMENT.fullmatch(token)
            if assignment:
                name, _, value = token.partition("=")
                if name.casefold().replace("-", "_") in _CCACHE_COMPILER_ASSIGNMENTS:
                    raise AuditInfrastructureError(
                        f"ccache compiler override is unsupported: {token}"
                    )
                if not value:
                    raise AuditInfrastructureError(f"invalid ccache assignment: {token}")
                if name in assignments:
                    raise AuditInfrastructureError(f"duplicate ccache assignment: {name}")
                assignments[name] = value
                index += 1
                continue
            if token.startswith("-"):
                raise AuditInfrastructureError(f"unsupported ccache option: {token}")
            if "=" in token:
                raise AuditInfrastructureError(f"invalid ccache assignment: {token}")
            break
    if index >= len(arguments):
        raise AuditInfrastructureError("compile command has launchers but no compiler")
    return Path(arguments[index]), tuple(arguments[index + 1 :]), assignments


def _validate_ccache_config_sources(
    arguments: tuple[str, ...], cwd: Path, environment: Mapping[str, str]
) -> None:
    paths: list[str] = []
    index = 0
    while index < len(arguments) and _launcher_name(arguments[index]) in _LAUNCHERS:
        launcher = _launcher_name(arguments[index])
        index += 1
        if launcher != "ccache":
            continue
        while index < len(arguments):
            token = arguments[index]
            if token == "--":
                index += 1
                break
            option_name, separator, attached = token.partition("=")
            if option_name in _CCACHE_VALUE_OPTIONS:
                if separator:
                    value = attached
                    index += 1
                else:
                    if index + 1 >= len(arguments):
                        return  # strip_launchers() reports the precise syntax error.
                    value = arguments[index + 1]
                    index += 2
                if option_name == "--config-path":
                    paths.append(value)
                if option_name == "--set-config":
                    _reject_ccache_config_assignment(value, "--set-config")
                continue
            if token in _CCACHE_FLAG_OPTIONS or _ASSIGNMENT.fullmatch(token):
                index += 1
                continue
            break

    environment_config: str | None = None
    for name, value in environment.items():
        if name.casefold() == "ccache_configpath":
            environment_config = value
            break
    if environment_config:
        paths.append(environment_config)
    canonical_paths: list[Path] = []
    for value in paths:
        path = Path(value)
        if not path.is_absolute():
            path = cwd / path
        try:
            canonical = path.resolve(strict=True)
            metadata = canonical.stat()
        except OSError as error:
            raise AuditInfrastructureError(f"ccache config is unreadable: {path}") from error
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > 1024 * 1024:
            raise AuditInfrastructureError(f"ccache config is invalid or too large: {canonical}")
        if canonical not in canonical_paths:
            canonical_paths.append(canonical)
    if len(canonical_paths) > 1:
        raise AuditInfrastructureError("ambiguous ccache config paths")
    for path in canonical_paths:
        try:
            text = path.read_text(encoding="utf-8-sig", errors="strict")
        except (OSError, UnicodeError) as error:
            raise AuditInfrastructureError(f"ccache config is unreadable: {path}") from error
        for line in text.splitlines():
            content = line.split("#", 1)[0].strip()
            if not content or "=" not in content:
                continue
            _reject_ccache_config_assignment(content, str(path))


def _reject_ccache_config_assignment(value: str, origin: str) -> None:
    name = value.partition("=")[0].strip().casefold().replace("-", "_")
    if name in _CCACHE_COMPILER_ASSIGNMENTS:
        raise AuditInfrastructureError(
            f"ccache config {origin} contains compiler override {name}"
        )


def _family_from_name(path: Path) -> CompilerFamily:
    name = _launcher_name(str(path))
    if name == "clang-cl":
        return CompilerFamily.CLANG_CL
    if name == "cl":
        return CompilerFamily.MSVC
    if re.fullmatch(r"(?:.*-)?clang(?:\+\+)?(?:-\d+(?:\.\d+)*)?", name):
        return CompilerFamily.CLANG
    if re.fullmatch(r"(?:.*-)?(?:gcc|g\+\+)(?:-\d+(?:\.\d+)*)?", name):
        return CompilerFamily.GCC
    raise AuditInfrastructureError(f"unsupported compiler or wrapper: {path}")


def _normalize_version_output(version_output: bytes) -> bytes:
    if not isinstance(version_output, bytes) or not version_output:
        raise AuditInfrastructureError("compiler version probe produced no output")
    if len(version_output) > _VERSION_BYTES:
        raise AuditInfrastructureError("compiler version output limit exceeded")
    if b"\0" in version_output:
        raise AuditInfrastructureError("compiler version probe output is invalid")
    try:
        text = version_output.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise AuditInfrastructureError("compiler version probe output is undecodable") from error
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    normalized = "\n".join(lines).encode("utf-8")
    if not normalized:
        raise AuditInfrastructureError("compiler version probe produced no output")
    return normalized


def identify_compiler(path: Path, version_output: bytes) -> CompilerFamily:
    """Require executable name and bounded version output to agree."""

    family = _family_from_name(path)
    text = _normalize_version_output(version_output).decode("utf-8").casefold()
    clang = "clang version" in text
    msvc = "microsoft" in text and "compiler version" in text
    gcc = (
        re.search(r"(?:^|[^a-z0-9_])gcc(?:[^a-z0-9_]|$)", text) is not None
        or re.search(r"(?:^|[^a-z0-9_])g\+\+(?:[^a-z0-9_]|$)", text) is not None
        or "free software foundation" in text
    )
    expected = {
        CompilerFamily.GCC: gcc and not clang and not msvc,
        CompilerFamily.CLANG: clang and not msvc,
        CompilerFamily.MSVC: msvc and not clang,
        CompilerFamily.CLANG_CL: clang and not msvc,
    }[family]
    if not expected:
        raise AuditInfrastructureError(
            f"contradictory compiler version probe for {path.name}"
        )
    return family


def _response_text(data: bytes, family: CompilerFamily, path: Path) -> str:
    try:
        if data.startswith(b"\xff\xfe"):
            if family not in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
                raise AuditInfrastructureError(
                    f"unsupported response-file encoding: {path}"
                )
            text = data[2:].decode("utf-16-le", errors="strict")
        elif data.startswith(b"\xfe\xff"):
            raise AuditInfrastructureError(f"unsupported response-file encoding: {path}")
        else:
            payload = data[3:] if data.startswith(b"\xef\xbb\xbf") else data
            text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise AuditInfrastructureError(f"invalid response-file encoding: {path}") from error
    if "\0" in text or text.startswith("\ufeff"):
        raise AuditInfrastructureError(f"ambiguous response-file encoding: {path}")
    return text


def _response_arguments(text: str, family: CompilerFamily, path: Path) -> tuple[str, ...]:
    try:
        if family in {CompilerFamily.GCC, CompilerFamily.CLANG}:
            return tuple(shlex.split(text, posix=True))
        # CommandLineToArgvW gives argv[0] special quoting semantics and does
        # not treat CR/LF as whitespace. Driver command files parse each line.
        result: list[str] = []
        for line in text.splitlines():
            if line.strip():
                result.extend(
                    _windows_command_line_split(f'gpu-capability-response {line}')[1:]
                )
        return tuple(result)
    except (ValueError, OSError, AuditInfrastructureError) as error:
        raise AuditInfrastructureError(f"unsupported response-file quoting: {path}") from error


def expand_response_files(
    arguments: tuple[str, ...],
    family: CompilerFamily,
    cwd: Path,
    limits: AuditLimits,
) -> tuple[str, ...]:
    """Recursively expand driver response files under fixed fail-closed bounds."""

    if family not in set(CompilerFamily):
        raise AuditInfrastructureError(f"unsupported compiler family: {family}")
    seen_paths: set[str] = set()
    seen_identities: dict[tuple[int, int], Path] = {}
    active: set[str] = set()
    aggregate_bytes = 0

    def expand(values: tuple[str, ...], depth: int) -> list[str]:
        nonlocal aggregate_bytes
        result: list[str] = []
        for value in values:
            if not value.startswith("@"):
                result.append(value)
                continue
            if len(value) == 1:
                raise AuditInfrastructureError("response-file path is empty")
            next_depth = depth + 1
            if next_depth > limits.response_depth:
                raise AuditInfrastructureError("response-file depth limit exceeded")
            requested = Path(value[1:])
            if not requested.is_absolute():
                requested = cwd / requested
            try:
                canonical = requested.resolve(strict=True)
                metadata = canonical.stat()
            except (OSError, RuntimeError) as error:
                raise AuditInfrastructureError(f"response file is unreadable: {requested}") from error
            if not stat.S_ISREG(metadata.st_mode):
                raise AuditInfrastructureError(f"response file is not regular: {canonical}")
            key = os.path.normcase(str(canonical))
            if key in active:
                raise AuditInfrastructureError(f"response-file cycle: {canonical}")
            identity = (int(metadata.st_dev), int(metadata.st_ino))
            if identity != (0, 0):
                prior = seen_identities.get(identity)
                if prior is not None and os.path.normcase(str(prior)) != key:
                    raise AuditInfrastructureError(
                        f"response-file alias: {canonical} aliases {prior}"
                    )
                seen_identities[identity] = canonical
            if key not in seen_paths:
                seen_paths.add(key)
                if len(seen_paths) > limits.response_files:
                    raise AuditInfrastructureError("response-file count limit exceeded")
            remaining = limits.response_bytes - aggregate_bytes
            if metadata.st_size > remaining:
                raise AuditInfrastructureError("response-file bytes limit exceeded")
            try:
                with canonical.open("rb") as stream:
                    data = stream.read(remaining + 1)
            except OSError as error:
                raise AuditInfrastructureError(f"response file is unreadable: {canonical}") from error
            if len(data) > remaining:
                raise AuditInfrastructureError("response-file bytes limit exceeded")
            if len(data) != metadata.st_size:
                raise AuditInfrastructureError(
                    f"response file changed while expanding: {canonical}"
                )
            aggregate_bytes += len(data)
            nested = _response_arguments(_response_text(data, family, canonical), family, canonical)
            active.add(key)
            try:
                result.extend(expand(nested, next_depth))
            finally:
                active.remove(key)
        return result

    return tuple(expand(arguments, 0))


def _probe_compiler_version(
    compiler: Path,
    family: CompilerFamily,
    cwd: Path,
    environment: Mapping[str, str],
) -> bytes:
    probe_directory: tempfile.TemporaryDirectory[str] | None = None
    if family is CompilerFamily.MSVC:
        probe_directory = tempfile.TemporaryDirectory(prefix="gpu-capability-cl-probe-")
        probe_source = Path(probe_directory.name) / "probe.cpp"
        probe_source.write_bytes(b"\n")
        command = [str(compiler), "/nologo", "/Bv", "/EP", "/TP", str(probe_source)]
    else:
        command = [str(compiler), "--version"]
    try:
        return _run_probe_command(command, compiler, cwd, environment)
    finally:
        if probe_directory is not None:
            probe_directory.cleanup()


def _run_probe_command(
    command: list[str],
    compiler: Path,
    cwd: Path,
    environment: Mapping[str, str],
) -> bytes:
    containment = _ProbeContainment()
    with tempfile.TemporaryFile(mode="w+b") as stdout_stream, tempfile.TemporaryFile(
        mode="w+b"
    ) as stderr_stream:
        process: subprocess.Popen[bytes] | None = None
        try:
            launch_command = containment.prepare_command(command)
            process = subprocess.Popen(
                launch_command,
                cwd=str(cwd),
                env=dict(environment),
                shell=False,
                stdin=(subprocess.PIPE if containment.requires_handshake else subprocess.DEVNULL),
                stdout=stdout_stream,
                stderr=stderr_stream,
                **containment.popen_arguments,
            )
            containment.attach(process)
            containment.release(process)
        except OSError as error:
            containment.close()
            raise AuditInfrastructureError(f"compiler version probe failed: {compiler}") from error
        except AuditInfrastructureError:
            if process is not None:
                process.kill()
                process.wait(timeout=1.0)
            containment.close()
            raise
        try:
            deadline = time.monotonic() + _VERSION_SECONDS
            failure: str | None = None
            while True:
                observed = os.fstat(stdout_stream.fileno()).st_size + os.fstat(
                    stderr_stream.fileno()
                ).st_size
                if observed > _VERSION_BYTES:
                    failure = "compiler version output limit exceeded"
                    break
                if process.poll() is not None:
                    break
                if time.monotonic() >= deadline:
                    failure = "compiler version probe timeout"
                    break
                time.sleep(0.01)
            if failure is not None:
                containment.terminate()
                process.kill()
                process.wait(timeout=1.0)
                raise AuditInfrastructureError(failure)
            returncode = process.wait(timeout=1.0)
        finally:
            # Closing the Windows job or killing the POSIX process group also
            # removes descendants after a nominally successful parent exit.
            containment.close()
        observed = os.fstat(stdout_stream.fileno()).st_size + os.fstat(
            stderr_stream.fileno()
        ).st_size
        if observed > _VERSION_BYTES:
            raise AuditInfrastructureError("compiler version output limit exceeded")
        if returncode != 0:
            raise AuditInfrastructureError(
                f"compiler version probe failed with exit={returncode}: {compiler}"
            )
        stdout_stream.seek(0)
        stderr_stream.seek(0)
        stdout = stdout_stream.read(_VERSION_BYTES + 1)
        stderr = stderr_stream.read(_VERSION_BYTES + 1)
    if stdout and stderr:
        combined = stdout.rstrip(b"\r\n") + b"\n" + stderr
    else:
        combined = stdout or stderr
    if len(combined) > _VERSION_BYTES:
        raise AuditInfrastructureError("compiler version output limit exceeded")
    return combined


class _ProbeContainment:
    """Own the complete version-probe process tree on every supported host."""

    def __init__(self) -> None:
        self._pid: int | None = None
        self._job = _WindowsProbeJob() if os.name == "nt" else None

    @property
    def popen_arguments(self) -> dict[str, object]:
        if os.name == "nt":
            return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        return {"start_new_session": True}

    @property
    def requires_handshake(self) -> bool:
        return os.name == "nt"

    def prepare_command(self, command: list[str]) -> list[str]:
        if os.name != "nt":
            return command
        # The trusted helper cannot spawn the real compiler until the parent
        # assigns it to the kill-on-close job, closing the assignment race.
        helper = (
            "import subprocess,sys;"
            "sys.stdin.buffer.read(1);"
            "raise SystemExit(subprocess.call(sys.argv[1:]))"
        )
        return [sys.executable, "-c", helper, *command]

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        pid = getattr(process, "pid", None)
        if not isinstance(pid, int):
            return  # Deterministic unit-test process double.
        self._pid = pid
        if self._job is not None:
            self._job.attach(process)

    def release(self, process: subprocess.Popen[bytes]) -> None:
        if not self.requires_handshake or self._pid is None:
            return
        if process.stdin is None:
            raise AuditInfrastructureError("compiler probe handshake is unavailable")
        process.stdin.write(b"1")
        process.stdin.close()

    def terminate(self) -> None:
        if self._job is not None:
            self._job.terminate()
            return
        if self._pid is not None:
            try:
                os.killpg(self._pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def close(self) -> None:
        if self._job is not None:
            self._job.close()
            return
        self.terminate()


class _WindowsProbeJob:
    """Kill-on-close Job Object used only by the bounded Windows probe."""

    def __init__(self) -> None:
        if os.name != "nt":
            self._handle = None
            return
        import ctypes
        from ctypes import wintypes

        class IoCounters(ctypes.Structure):
            _fields_ = tuple(
                (name, ctypes.c_ulonglong)
                for name in (
                    "read_operations", "write_operations", "other_operations",
                    "read_bytes", "write_bytes", "other_bytes",
                )
            )

        class BasicLimits(ctypes.Structure):
            _fields_ = (
                ("per_process_user_time", ctypes.c_longlong),
                ("per_job_user_time", ctypes.c_longlong),
                ("limit_flags", wintypes.DWORD),
                ("minimum_working_set", ctypes.c_size_t),
                ("maximum_working_set", ctypes.c_size_t),
                ("active_process_limit", wintypes.DWORD),
                ("affinity", ctypes.c_size_t),
                ("priority_class", wintypes.DWORD),
                ("scheduling_class", wintypes.DWORD),
            )

        class ExtendedLimits(ctypes.Structure):
            _fields_ = (
                ("basic", BasicLimits),
                ("io", IoCounters),
                ("process_memory", ctypes.c_size_t),
                ("job_memory", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t),
                ("peak_job_memory", ctypes.c_size_t),
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = (
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD
        )
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
        kernel32.TerminateJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.CreateJobObjectW(None, None)
        if not handle:
            raise AuditInfrastructureError("cannot create compiler probe job object")
        limits = ExtendedLimits()
        limits.basic.limit_flags = 0x00002000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(
            handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            kernel32.CloseHandle(handle)
            raise AuditInfrastructureError("cannot configure compiler probe job object")
        self._handle = handle
        self._kernel32 = kernel32

    def attach(self, process: subprocess.Popen[bytes]) -> None:
        if self._handle is None:
            return
        raw_handle = getattr(process, "_handle", None)
        if raw_handle is None or not self._kernel32.AssignProcessToJobObject(
            self._handle, raw_handle
        ):
            raise AuditInfrastructureError("cannot contain compiler probe process tree")

    def terminate(self) -> None:
        if self._handle is not None:
            self._kernel32.TerminateJobObject(self._handle, 1)

    def close(self) -> None:
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


def _resolve_compiler(
    compiler: Path,
    cwd: Path,
    environment: Mapping[str, str],
) -> Path:
    _family_from_name(compiler)
    value = str(compiler)
    has_separator = "/" in value or "\\" in value
    if compiler.is_absolute() or has_separator:
        candidate = compiler if compiler.is_absolute() else cwd / compiler
        try:
            canonical = candidate.resolve(strict=True)
        except OSError as error:
            raise AuditInfrastructureError(f"compiler executable is unavailable: {compiler}") from error
    else:
        located = shutil.which(value, path=environment.get("PATH"))
        if located is None:
            raise AuditInfrastructureError(f"compiler executable is unavailable: {compiler}")
        canonical = Path(located).resolve(strict=True)
    try:
        metadata = canonical.stat()
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {canonical}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise AuditInfrastructureError(f"compiler executable is not regular: {canonical}")
    return canonical


def _hash_field(hasher: "hashlib._Hash", value: bytes) -> None:
    hasher.update(len(value).to_bytes(8, "big"))
    hasher.update(value)


def _environment_digest(environment: Mapping[str, str]) -> str:
    if not isinstance(environment, Mapping):
        raise AuditInfrastructureError("compiler environment must be a mapping")
    entries: list[tuple[str, str]] = []
    normalized_names: set[str] = set()
    for name, value in environment.items():
        if not isinstance(name, str) or not isinstance(value, str) or "\0" in name or "\0" in value:
            raise AuditInfrastructureError("compiler environment must contain valid strings")
        normalized = name.casefold() if os.name == "nt" else name
        if normalized in normalized_names:
            raise AuditInfrastructureError(f"ambiguous compiler environment key: {name}")
        normalized_names.add(normalized)
        entries.append((normalized, value))
    hasher = hashlib.sha256()
    for name, value in sorted(entries):
        _hash_field(hasher, name.encode("utf-8", errors="surrogatepass"))
        _hash_field(hasher, value.encode("utf-8", errors="surrogatepass"))
    return hasher.hexdigest()


def _compiler_fingerprint(compiler: Path, normalized_version: bytes) -> str:
    try:
        before = compiler.stat()
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {compiler}") from error
    hasher = hashlib.sha256()
    _hash_field(hasher, str(compiler).encode("utf-8", errors="surrogatepass"))
    _hash_field(hasher, str(before.st_size).encode("ascii"))
    _hash_field(hasher, str(before.st_mtime_ns).encode("ascii"))
    _hash_field(hasher, str(getattr(before, "st_ctime_ns", 0)).encode("ascii"))
    content = hashlib.sha256()
    try:
        with compiler.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                content.update(chunk)
        after = compiler.stat()
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {compiler}") from error
    if (
        before.st_size,
        before.st_mtime_ns,
        getattr(before, "st_ctime_ns", 0),
    ) != (
        after.st_size,
        after.st_mtime_ns,
        getattr(after, "st_ctime_ns", 0),
    ):
        raise AuditInfrastructureError(f"compiler executable changed while fingerprinting: {compiler}")
    _hash_field(hasher, content.digest())
    _hash_field(hasher, normalized_version)
    return hasher.hexdigest()


def _canonical_argument_path(value: str, cwd: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = cwd / path
    return path.resolve(strict=False)


def _source_inputs(
    arguments: tuple[str, ...], cwd: Path, family: CompilerFamily
) -> tuple[Path, ...]:
    sources: list[Path] = []
    index = 0
    positional_only = False
    while index < len(arguments):
        value = arguments[index]
        if value == "-":
            raise AuditInfrastructureError("stdin cannot be a compile source")
        if value == "--" and family in {
            CompilerFamily.GCC, CompilerFamily.CLANG, CompilerFamily.CLANG_CL
        }:
            positional_only = True
            index += 1
            continue
        lowered = value.casefold()
        if not positional_only and value in _GNU_VALUE_OPTIONS and family in {
            CompilerFamily.GCC, CompilerFamily.CLANG, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if not positional_only and lowered in {"/tc", "/tp"} and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a source: {value}")
            candidate = arguments[index + 1]
            if candidate == "-":
                raise AuditInfrastructureError("stdin cannot be a compile source")
            sources.append(_canonical_argument_path(candidate, cwd))
            index += 2
            continue
        if not positional_only and lowered.startswith(("/tc", "/tp")) and len(value) > 3:
            candidate = value[3:]
            if candidate == "-":
                raise AuditInfrastructureError("stdin cannot be a compile source")
            sources.append(_canonical_argument_path(candidate, cwd))
            index += 1
            continue
        if not positional_only and lowered in _MSVC_VALUE_OPTIONS and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if not positional_only and value.startswith("-"):
            index += 1
            continue
        if not positional_only and value.startswith("/") and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            index += 1
            continue
        sources.append(_canonical_argument_path(value, cwd))
        index += 1
    return tuple(sources)


def _same_path(first: Path, second: Path) -> bool:
    return os.path.normcase(str(first)) == os.path.normcase(str(second))


def _reject_driver_dialect_overrides(arguments: tuple[str, ...]) -> None:
    for value in arguments:
        lowered = value.casefold()
        if lowered == "--driver-mode" or lowered.startswith("--driver-mode="):
            raise AuditInfrastructureError(f"explicit compiler driver mode is unsupported: {value}")
        if lowered == "--rsp-quoting" or lowered.startswith("--rsp-quoting="):
            raise AuditInfrastructureError(f"explicit response-file quoting mode is unsupported: {value}")
        if lowered.startswith("/clang:") and (
            "--driver-mode" in lowered or "--rsp-quoting" in lowered
        ):
            raise AuditInfrastructureError(f"hidden compiler driver/response mode is unsupported: {value}")


def _msvc_environment_arguments(
    environment: Mapping[str, str],
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    values: dict[str, str] = {}
    for name, value in environment.items():
        normalized = name.casefold()
        if normalized in {"cl", "_cl_"}:
            if normalized in values:
                raise AuditInfrastructureError(f"ambiguous MSVC environment command line: {name}")
            if len(value) > 32 * 1024 or "\0" in value:
                raise AuditInfrastructureError(f"invalid MSVC environment command line: {name}")
            values[normalized] = value

    def decode(name: str) -> tuple[str, ...]:
        value = values.get(name, "")
        if not value.strip():
            return ()
        try:
            return _windows_command_line_split(f"gpu-capability-environment {value}")[1:]
        except AuditInfrastructureError as error:
            raise AuditInfrastructureError(
                f"unsupported MSVC environment command quoting: {name}"
            ) from error

    return decode("cl"), decode("_cl_")


def make_configuration(
    entry: Mapping[str, object],
    database: Path,
    entry_index: int,
    source_root: Path,
    production: Mapping[PurePosixPath, FileIdentity],
    environment: Mapping[str, str],
    limits: AuditLimits,
) -> PreprocessConfiguration:
    """Build one stable semantic configuration from a compile database entry."""

    if not isinstance(entry_index, int) or isinstance(entry_index, bool) or entry_index < 0:
        raise AuditInfrastructureError("compile database entry index is invalid")
    cwd, decoded = decode_compile_entry(entry, database, windows=os.name == "nt")
    launcher_names: list[str] = []
    for value in decoded:
        name = _launcher_name(value)
        if name not in _LAUNCHERS:
            break
        launcher_names.append(name)
    if "ccache" in launcher_names:
        _validate_ccache_config_sources(decoded, cwd, environment)
        for name in environment:
            if name.casefold() in _CCACHE_COMPILER_ENVIRONMENT:
                raise AuditInfrastructureError(
                    f"ccache compiler environment override is unsupported: {name}"
                )
    compiler_argument, compiler_arguments, _assignments = strip_launchers(decoded)
    _reject_driver_dialect_overrides(compiler_arguments)
    family_hint = _family_from_name(compiler_argument)
    compiler = _resolve_compiler(compiler_argument, cwd, environment)
    version_output = _probe_compiler_version(compiler, family_hint, cwd, environment)
    family = identify_compiler(compiler, version_output)
    normalized_version = _normalize_version_output(version_output)
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        prefix, suffix = _msvc_environment_arguments(environment)
        compiler_arguments = (*prefix, *compiler_arguments, *suffix)
        _reject_driver_dialect_overrides(compiler_arguments)
    expanded_arguments = expand_response_files(
        compiler_arguments, family, cwd, limits
    )
    _reject_driver_dialect_overrides(expanded_arguments)
    _validate_arguments((str(compiler), *expanded_arguments))

    file_value = entry.get("file")
    if not isinstance(file_value, str):
        raise AuditInfrastructureError("compile command file must be a string")
    source_path = Path(file_value)
    if not source_path.is_absolute():
        source_path = cwd / source_path
    try:
        source_path = source_path.resolve(strict=True)
        canonical_root = source_root.resolve(strict=True)
    except OSError as error:
        raise AuditInfrastructureError(f"compile entry source is unavailable: {file_value}") from error
    try:
        source_path.relative_to(canonical_root)
    except ValueError as error:
        raise AuditInfrastructureError(
            f"compile entry source is outside production root: {source_path}"
        ) from error
    identities = [
        identity for identity in production.values()
        if _same_path(identity.canonical, source_path)
    ]
    if len(identities) != 1:
        raise AuditInfrastructureError(
            f"compile entry source is not an enumerated production file: {source_path}"
        )
    source = identities[0]

    source_inputs = _source_inputs(expanded_arguments, cwd, family)
    if len(source_inputs) > 1:
        raise AuditInfrastructureError("compile command has multiple source inputs")
    if not source_inputs:
        raise AuditInfrastructureError("compile command source does not match database entry")
    if not _same_path(source_inputs[0], source_path):
        raise AuditInfrastructureError("compile command source does not match database entry")

    environment_digest = _environment_digest(environment)
    compiler_fingerprint = _compiler_fingerprint(compiler, normalized_version)
    semantic = {
        "schema": 1,
        "family": family.value,
        "compiler": str(compiler),
        "compiler_fingerprint": compiler_fingerprint,
        "working_directory": str(cwd),
        "source": str(source.canonical),
        "arguments": expanded_arguments,
        "environment_digest": environment_digest,
    }
    digest = hashlib.sha256(
        json.dumps(
            semantic,
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    ).hexdigest()
    return PreprocessConfiguration(
        entry_id=f"{database.resolve()}:{entry_index}",
        family=family,
        compiler=compiler,
        working_directory=cwd,
        source=source,
        arguments=expanded_arguments,
        environment_digest=environment_digest,
        digest=digest,
    )
