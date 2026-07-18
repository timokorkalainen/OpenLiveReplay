"""Fail-closed compile-command normalization for the GPU capability audit."""

from __future__ import annotations

import hashlib
import json
import locale
import os
import re
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CompilerExecutableCapability,
    CompilerFamily,
    CompilerInspection,
    _FilesystemGenerationObserver,
    DependencyDigest,
    DependencyRootAuthority,
    DependencyRootBinding,
    FileIdentity,
    PreprocessConfiguration,
    portable_compiler_inspection_key,
    validate_dependency_root_authority,
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
    "--output", "--dump", "-d", "--define-macro", "--undefine-macro",
    "--include", "--imacros",
    "-isysroot", "--sysroot", "-x", "-target", "--target", "-arch", "-MF",
    "-MT", "-MQ", "-MJ", "-F", "-iframework", "-include-pch", "-include-pth",
    "-Xclang", "-Xpreprocessor", "-Xassembler", "-Xlinker", "-B", "-specs",
    "-wrapper", "-fmodule-file", "-fmodule-map-file",
    "-serialize-diagnostics", "--serialize-diagnostics", "-dependency-file",
    "--dependency-file",
})
_MSVC_VALUE_OPTIONS = frozenset({
    "/D", "/U", "/I", "/FI", "/Fo", "/Fe", "/Fd", "/Fi",
    "/Fp", "/Ft", "/Yu", "/Yc", "/experimental:log",
    "/sourceDependencies", "/scanDependencies", "/external:I", "/AI", "/FU",
    "/ifcOutput", "/reference", "/headerUnit",
})
# cl and clang-cl switches are case-sensitive. This is the exact option grammar
# the audit itself interprets; unknown switches remain compiler-authoritative.
# Exact spellings are accepted with either the '/' or '-' introducer.
_MSVC_CASE_SENSITIVE_EXACT_OPTIONS = _MSVC_VALUE_OPTIONS | frozenset({
    "/c", "/nologo", "/E", "/P", "/EP", "/showIncludes",
    "/PD", "/PH", "/Fx", "/doc", "/TP", "/TC", "/Tp", "/Tc",
    "/FA", "/Fa", "/Fm", "/FR", "/Fr", "/d1PP",
})
_MSVC_CASE_SENSITIVE_PREFIX_OPTIONS = (
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
    "/experimental:log", "/external:I", "/showIncludes:",
    "/headerUnit", "/reference", "/clang:",
    "/Fo", "/Fe", "/Fd", "/Fi", "/Ft", "/Fp", "/FI",
    "/FA", "/Fa", "/Fm", "/FR", "/Fr",
    "/Yu", "/Yc", "/AI", "/FU", "/D", "/U", "/I",
    "/OUT", "/link", "/LD", "/clr:netcore", "/Tp", "/Tc",
    "/doc",
)
# These documented switches deliberately differ only by case from a shorter
# switch above. Recognize them before checking misspellings so, for example,
# /u never acquires /U's operand and /interface never looks like attached /I.
_MSVC_CASE_DISTINCT_EXACT_OPTIONS = frozenset({
    "/C", "/u", "/utf-8", "/interface", "/internalPartition",
    "/exportHeader", "/translateInclude", "/validate-charset",
})
_MSVC_CASE_DISTINCT_PREFIX_OPTIONS = (
    "/arch:", "/diagnostics:", "/execution-charset:", "/favor:",
    "/fp:", "/source-charset:", "/Zc:",
)
# Only spellings that could be mistaken for a source-selection, marker, output,
# or dependency control are rejected case-insensitively. Broad prefix folding
# is incorrect for MSVC: /u versus /U, /C versus /c, /fp versus /Fp, and
# /interface versus /I all have distinct documented meanings.
_MSVC_AMBIGUOUS_EXACT_OPTIONS = frozenset({
    "/c", "/E", "/P", "/EP", "/showIncludes", "/PD", "/PH", "/Fx",
    "/doc", "/TP", "/TC", "/Tp", "/Tc",
})
_MSVC_AMBIGUOUS_PREFIX_OPTIONS = (
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
    "/experimental:log", "/showIncludes:", "/Fo", "/Fe", "/Fd", "/Fi",
    "/Ft", "/FA", "/Fa", "/Fm", "/FR", "/Fr", "/OUT", "/link",
    "/LD", "/clr:netcore", "/Yc", "/Tp", "/Tc", "/doc",
)
_VERSION_SECONDS = 5.0
_VERSION_BYTES = 1024 * 1024
_RUNTIME_ENUMERATION_ENTRIES = 4096
_RUNTIME_CLOSURE_FILES = 256
_RUNTIME_FILE_BYTES = 256 * 1024 * 1024
_RUNTIME_TOTAL_BYTES = 1024 * 1024 * 1024
_RUNTIME_TREE_ENTRIES = 65536
_RUNTIME_TREE_DIRECTORIES = 4096
_compiler_inspection_lock = threading.Lock()
_compiler_capability_lock = threading.Lock()
_compiler_capability_memo: dict[tuple[object, ...], CompilerExecutableCapability] = {}
_compiler_inspection_memo: dict[
    tuple[object, ...],
    CompilerInspection,
] = {}


def _clear_compiler_inspection_memo_for_tests() -> None:
    with _compiler_inspection_lock:
        _compiler_inspection_memo.clear()
    with _compiler_capability_lock:
        for capability in _compiler_capability_memo.values():
            try:
                capability.native_owner.close()
            except AuditInfrastructureError:
                pass
        _compiler_capability_memo.clear()


@dataclass(frozen=True)
class RewrittenCommand:
    arguments: tuple[str, ...]
    dependency_output: Path
    dependency_format: str


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
        text = version_output.decode(
            locale.getpreferredencoding(False), errors="strict"
        )
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
    capability: CompilerExecutableCapability,
    family: CompilerFamily,
    cwd: Path,
    environment: Mapping[str, str],
    pipeline_deadline: float | None = None,
) -> bytes:
    compiler = capability.executable_identity.canonical
    probe_directory: tempfile.TemporaryDirectory[str] | None = None
    if family is CompilerFamily.MSVC:
        probe_directory = tempfile.TemporaryDirectory(prefix="gpu-capability-cl-probe-")
        probe_source = Path(probe_directory.name) / "probe.cpp"
        probe_source.write_bytes(b"\n")
        arguments = ("/nologo", "/Bv", "/EP", "/TP", str(probe_source))
    else:
        arguments = ("--version",)
    try:
        return _run_probe_command(capability, arguments, cwd, environment, pipeline_deadline)
    finally:
        if probe_directory is not None:
            probe_directory.cleanup()


def _run_probe_command(
    capability: CompilerExecutableCapability,
    arguments: tuple[str, ...],
    cwd: Path,
    environment: Mapping[str, str],
    pipeline_deadline: float | None = None,
) -> bytes:
    if not isinstance(capability, CompilerExecutableCapability):
        raise AuditInfrastructureError("compiler probe capability is invalid")
    compiler = capability.executable_identity.canonical
    owner = capability.native_owner
    validate = getattr(owner, "validate", None)
    if not callable(validate):
        raise AuditInfrastructureError("compiler probe capability owner is invalid")
    validate()
    command = (str(compiler), *arguments)
    launch_options = {}
    if capability.platform_kind == "linux":
        executable_fd = getattr(owner, "executable_fd", None)
        if not isinstance(executable_fd, int):
            raise AuditInfrastructureError("exact compiler probe executable fd is unavailable")
        command = (f"/proc/self/fd/{executable_fd}", *arguments)
        launch_options["pass_fds"] = (executable_fd,)
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
                **launch_options,
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
            deadline = min(
                time.monotonic() + _VERSION_SECONDS,
                pipeline_deadline if pipeline_deadline is not None else float("inf"),
            )
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
    validate()
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
        return [sys.executable, "-I", "-S", "-c", helper, *command]

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


def _compiler_metadata_snapshot(compiler: Path) -> tuple[int, int, int, int, int]:
    try:
        metadata = compiler.stat()
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {compiler}") from error
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_size),
        int(metadata.st_mtime_ns),
        int(getattr(metadata, "st_ctime_ns", 0)),
    )


def _regular_file_snapshot(path: Path) -> tuple[int, int | None, int, int, int, int]:
    metadata = path.stat()
    if not stat.S_ISREG(metadata.st_mode):
        raise AuditInfrastructureError(f"compiler capability path is not regular: {path}")
    return (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
        int(metadata.st_size),
        int(metadata.st_mtime_ns),
        int(getattr(metadata, "st_ctime_ns", 0)),
        int(metadata.st_mode),
    )


def _directory_snapshot(path: Path) -> tuple[int, int | None, int]:
    metadata = path.stat()
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise AuditInfrastructureError(f"compiler capability directory is invalid: {path}")
    return (
        int(metadata.st_dev),
        int(metadata.st_ino) if int(metadata.st_ino) != 0 else None,
        int(metadata.st_mode),
    )


def _check_capability_budget(
    deadline: float | None, cancel_event: object | None
) -> None:
    if cancel_event is not None and cancel_event.is_set():
        raise AuditInfrastructureError("compiler capability construction cancelled")
    if deadline is not None and time.monotonic() >= deadline:
        raise AuditInfrastructureError("compiler capability deadline exceeded")


def _bounded_c_string(data: bytes, offset: int) -> str:
    if not isinstance(offset, int) or offset < 0 or offset >= len(data):
        raise AuditInfrastructureError("compiler runtime binary string is invalid")
    end = data.find(b"\0", offset, min(len(data), offset + 32768))
    if end < 0:
        raise AuditInfrastructureError("compiler runtime binary string is unterminated")
    try:
        value = data[offset:end].decode("utf-8")
    except UnicodeError as error:
        raise AuditInfrastructureError("compiler runtime binary string is invalid") from error
    if not value:
        raise AuditInfrastructureError("compiler runtime binary import is empty")
    return value


def _pe_runtime_import_names(data: bytes) -> tuple[str, ...]:
    if len(data) < 64 or data[:2] != b"MZ":
        raise AuditInfrastructureError("compiler runtime PE image is invalid")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise AuditInfrastructureError("compiler runtime PE image is invalid")
    section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    optional = pe_offset + 24
    if optional + optional_size > len(data) or section_count > 4096:
        raise AuditInfrastructureError("compiler runtime PE headers exceed bounds")
    magic = struct.unpack_from("<H", data, optional)[0]
    directory_offset = optional + (96 if magic == 0x10B else 112 if magic == 0x20B else -1)
    if directory_offset < optional or directory_offset + 16 > optional + optional_size:
        raise AuditInfrastructureError("compiler runtime PE optional header is invalid")
    import_rva, import_size = struct.unpack_from("<II", data, directory_offset + 8)
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        if offset + 40 > len(data):
            raise AuditInfrastructureError("compiler runtime PE section table is invalid")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_offset(rva: int) -> int:
        for address, span, raw_offset, raw_size in sections:
            if address <= rva < address + span:
                relative = rva - address
                if relative >= raw_size or raw_offset + relative >= len(data):
                    break
                return raw_offset + relative
        if rva < len(data):
            return rva
        raise AuditInfrastructureError("compiler runtime PE import RVA is invalid")

    if import_rva == 0 or import_size == 0:
        return ()
    cursor = rva_offset(import_rva)
    names = []
    for _index in range(min(4096, import_size // 20 + 1)):
        if cursor + 20 > len(data):
            raise AuditInfrastructureError("compiler runtime PE imports are truncated")
        descriptor = struct.unpack_from("<IIIII", data, cursor)
        if descriptor == (0, 0, 0, 0, 0):
            return tuple(dict.fromkeys(names))
        names.append(_bounded_c_string(data, rva_offset(descriptor[3])))
        cursor += 20
    raise AuditInfrastructureError("compiler runtime PE import ceiling exceeded")


def _elf_runtime_import_names(data: bytes) -> tuple[str, ...]:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise AuditInfrastructureError("compiler runtime ELF image is invalid")
    elf_class, encoding = data[4], data[5]
    if elf_class not in (1, 2) or encoding not in (1, 2):
        raise AuditInfrastructureError("compiler runtime ELF format is unsupported")
    order = "<" if encoding == 1 else ">"
    if elf_class == 2:
        phoff = struct.unpack_from(order + "Q", data, 32)[0]
        phentsize, phnum = struct.unpack_from(order + "HH", data, 54)
        ph_format = order + "IIQQQQQQ"
        dynamic_format = order + "qQ"
    else:
        phoff = struct.unpack_from(order + "I", data, 28)[0]
        phentsize, phnum = struct.unpack_from(order + "HH", data, 42)
        ph_format = order + "IIIIIIII"
        dynamic_format = order + "iI"
    if phnum > 4096 or phentsize < struct.calcsize(ph_format):
        raise AuditInfrastructureError("compiler runtime ELF program headers exceed bounds")
    loads = []
    dynamic = None
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + struct.calcsize(ph_format) > len(data):
            raise AuditInfrastructureError("compiler runtime ELF program headers are truncated")
        values = struct.unpack_from(ph_format, data, offset)
        if elf_class == 2:
            kind, file_offset, virtual, file_size = values[0], values[2], values[3], values[5]
        else:
            kind, file_offset, virtual, file_size = values[0], values[1], values[2], values[4]
        if kind == 1:
            loads.append((virtual, file_size, file_offset))
        elif kind == 2:
            dynamic = (file_offset, file_size)
    if dynamic is None:
        return ()
    entry_size = struct.calcsize(dynamic_format)
    needed = []
    string_virtual = None
    for cursor in range(dynamic[0], dynamic[0] + dynamic[1], entry_size):
        if cursor + entry_size > len(data):
            raise AuditInfrastructureError("compiler runtime ELF dynamic table is truncated")
        tag, value = struct.unpack_from(dynamic_format, data, cursor)
        if tag == 0:
            break
        if tag == 1:
            needed.append(value)
        elif tag == 5:
            string_virtual = value
        if len(needed) > 4096:
            raise AuditInfrastructureError("compiler runtime ELF import ceiling exceeded")
    if string_virtual is None:
        return ()
    string_offset = None
    for virtual, file_size, file_offset in loads:
        if virtual <= string_virtual < virtual + file_size:
            string_offset = file_offset + string_virtual - virtual
            break
    if string_offset is None:
        raise AuditInfrastructureError("compiler runtime ELF string table is invalid")
    return tuple(dict.fromkeys(_bounded_c_string(data, string_offset + item) for item in needed))


def _macho_runtime_import_names(data: bytes) -> tuple[str, ...]:
    if len(data) < 28:
        raise AuditInfrastructureError("compiler runtime Mach-O image is invalid")
    magic = data[:4]
    formats = {
        b"\xfe\xed\xfa\xce": (">", False), b"\xce\xfa\xed\xfe": ("<", False),
        b"\xfe\xed\xfa\xcf": (">", True), b"\xcf\xfa\xed\xfe": ("<", True),
    }
    if magic in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        order = ">" if magic == b"\xca\xfe\xba\xbe" else "<"
        count = struct.unpack_from(order + "I", data, 4)[0]
        if count > 64:
            raise AuditInfrastructureError("compiler runtime Mach-O fat image exceeds bounds")
        result = []
        for index in range(count):
            entry = 8 + index * 20
            if entry + 20 > len(data):
                raise AuditInfrastructureError("compiler runtime Mach-O fat image is truncated")
            offset, size = struct.unpack_from(order + "II", data, entry + 8)
            if offset + size > len(data):
                raise AuditInfrastructureError("compiler runtime Mach-O slice is invalid")
            result.extend(_macho_runtime_import_names(data[offset:offset + size]))
        return tuple(dict.fromkeys(result))
    if magic not in formats:
        raise AuditInfrastructureError("compiler runtime binary format is unsupported")
    order, is_64 = formats[magic]
    command_count, command_bytes = struct.unpack_from(order + "II", data, 16)
    cursor = 32 if is_64 else 28
    if command_count > 4096 or cursor + command_bytes > len(data):
        raise AuditInfrastructureError("compiler runtime Mach-O commands exceed bounds")
    dylib_commands = {0xC, 0x18, 0x1F, 0x20, 0x23}
    result = []
    for _index in range(command_count):
        if cursor + 8 > len(data):
            raise AuditInfrastructureError("compiler runtime Mach-O command is truncated")
        command, size = struct.unpack_from(order + "II", data, cursor)
        command &= 0x7FFFFFFF
        if size < 8 or cursor + size > len(data):
            raise AuditInfrastructureError("compiler runtime Mach-O command is invalid")
        if command in dylib_commands:
            if size < 24:
                raise AuditInfrastructureError("compiler runtime Mach-O dylib command is invalid")
            name_offset = struct.unpack_from(order + "I", data, cursor + 8)[0]
            result.append(_bounded_c_string(data, cursor + name_offset))
        cursor += size
    return tuple(dict.fromkeys(result))


def _binary_runtime_import_names(path: Path) -> tuple[str, ...]:
    try:
        metadata = path.stat()
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > _RUNTIME_FILE_BYTES:
            raise AuditInfrastructureError("compiler runtime per-file byte ceiling exceeded")
        data = path.read_bytes()
    except OSError as error:
        raise AuditInfrastructureError("compiler runtime binary is unreadable") from error
    if len(data) != metadata.st_size:
        raise AuditInfrastructureError("compiler runtime binary changed while parsing")
    if data.startswith(b"MZ"):
        return _pe_runtime_import_names(data)
    if data.startswith(b"\x7fELF"):
        return _elf_runtime_import_names(data)
    if data[:4] in {
        b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    }:
        return _macho_runtime_import_names(data)
    return ()


def _toolchain_file_index(
    root: Path, deadline: float, cancel_event: object | None
) -> dict[str, tuple[Path, ...]]:
    pending = [root]
    directories = 0
    entries = 0
    result: dict[str, list[Path]] = {}
    while pending:
        _check_capability_budget(deadline, cancel_event)
        directory = pending.pop()
        directories += 1
        if directories > _RUNTIME_TREE_DIRECTORIES:
            raise AuditInfrastructureError("compiler runtime directory enumeration ceiling exceeded")
        try:
            with os.scandir(directory) as iterator:
                for entry in iterator:
                    _check_capability_budget(deadline, cancel_event)
                    entries += 1
                    if entries > _RUNTIME_TREE_ENTRIES:
                        raise AuditInfrastructureError(
                            "compiler runtime tree enumeration ceiling exceeded"
                        )
                    try:
                        if entry.is_symlink():
                            continue
                        if entry.is_dir(follow_symlinks=False):
                            pending.append(Path(entry.path))
                        elif entry.is_file(follow_symlinks=False):
                            result.setdefault(entry.name.casefold(), []).append(
                                Path(entry.path).resolve(strict=True)
                            )
                    except OSError as error:
                        raise AuditInfrastructureError(
                            "compiler runtime tree changed during enumeration"
                        ) from error
        except OSError as error:
            raise AuditInfrastructureError("compiler runtime tree is unreadable") from error
    return {
        name: tuple(sorted(paths, key=lambda item: os.path.normcase(str(item))))
        for name, paths in result.items()
    }


def _resolve_runtime_name(
    name: str,
    importer: Path,
    executable: Path,
    root: Path,
    index: Mapping[str, tuple[Path, ...]],
) -> Path | None:
    expanded = name.replace("@loader_path", str(importer.parent)).replace(
        "@executable_path", str(executable.parent)
    )
    direct = Path(expanded)
    direct_candidates = []
    if direct.is_absolute():
        direct_candidates.append(direct)
    elif not expanded.startswith("@rpath/"):
        direct_candidates.extend((
            importer.parent / direct, executable.parent / direct, root / direct
        ))
    basename = Path(expanded.removeprefix("@rpath/")).name.casefold()
    def valid(candidates):
        matches = []
        for candidate in candidates:
            try:
                resolved = candidate.resolve(strict=True)
                resolved.relative_to(root)
                if resolved.is_file() and resolved not in matches:
                    matches.append(resolved)
            except (OSError, RuntimeError, ValueError):
                continue
        return matches

    matches = valid(direct_candidates)
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise AuditInfrastructureError(
            f"compiler runtime import direct resolution is ambiguous: {name}"
        )
    matches = valid(index.get(basename, ()))
    if not matches:
        return None
    if len(matches) != 1:
        raise AuditInfrastructureError(
            f"compiler runtime import is ambiguous inside toolchain: {name}"
        )
    return matches[0]


def _recursive_runtime_paths(
    seeds: tuple[Path, ...],
    executable: Path,
    root: Path,
    index: Mapping[str, tuple[Path, ...]],
    deadline: float,
    cancel_event: object | None,
) -> tuple[Path, ...]:
    pending = list(seeds)
    result = []
    seen = set()
    while pending:
        _check_capability_budget(deadline, cancel_event)
        current = pending.pop(0).resolve(strict=True)
        key = os.path.normcase(str(current))
        if key in seen:
            continue
        seen.add(key)
        result.append(current)
        if len(result) > _RUNTIME_CLOSURE_FILES:
            raise AuditInfrastructureError("compiler runtime closure file ceiling exceeded")
        for name in _binary_runtime_import_names(current):
            resolved = _resolve_runtime_name(name, current, executable, root, index)
            if resolved is not None:
                pending.append(resolved)
    return tuple(result)


def _content_sha256(
    stream,
    *,
    deadline: float | None = None,
    cancel_event: object | None = None,
    byte_ceiling: int = _RUNTIME_FILE_BYTES,
) -> str:
    digest = hashlib.sha256()
    observed = 0
    stream.seek(0)
    while chunk := stream.read(1024 * 1024):
        _check_capability_budget(deadline, cancel_event)
        observed += len(chunk)
        if observed > byte_ceiling:
            raise AuditInfrastructureError("compiler runtime per-file byte ceiling exceeded")
        digest.update(chunk)
    stream.seek(0)
    return digest.hexdigest()


class _CompilerCapabilityOwner:
    """Held executable/closure files plus exact path-chain snapshots."""

    def __init__(
        self,
        streams: tuple[object, ...],
        file_paths: tuple[Path, ...],
        file_snapshots: tuple[tuple[int, int | None, int, int, int, int], ...],
        file_hashes: tuple[str, ...],
        directory_paths: tuple[Path, ...],
        directory_snapshots: tuple[tuple[int, int | None, int], ...],
        observer: _FilesystemGenerationObserver,
        dependency_root_authority: DependencyRootAuthority,
    ) -> None:
        self.streams = streams
        self.file_paths = file_paths
        self.file_snapshots = file_snapshots
        self.file_hashes = file_hashes
        self.directory_paths = directory_paths
        self.directory_snapshots = directory_snapshots
        self.observer = observer
        self.dependency_root_authority = dependency_root_authority
        self._closed = False
        self._lock = threading.Lock()

    @property
    def executable_fd(self) -> int:
        if self._closed or not self.streams:
            raise AuditInfrastructureError("compiler executable capability is closed")
        return self.streams[0].fileno()

    def validate(self, *, content: bool = True) -> None:
        with self._lock:
            self._validate_locked(content=content)

    def _validate_locked(self, *, content: bool) -> None:
        if self._closed:
            raise AuditInfrastructureError("compiler executable capability is closed")
        self.observer.drain()
        for stream, path, expected in zip(
            self.streams, self.file_paths, self.file_snapshots
        ):
            opened = os.fstat(stream.fileno())
            opened_snapshot = (
                int(opened.st_dev),
                int(opened.st_ino) if int(opened.st_ino) != 0 else None,
                int(opened.st_size),
                int(opened.st_mtime_ns),
                int(getattr(opened, "st_ctime_ns", 0)),
                int(opened.st_mode),
            )
            if (
                opened_snapshot[:4] != expected[:4]
                or _regular_file_snapshot(path) != expected
            ):
                raise AuditInfrastructureError(
                    "compiler executable changed during compiler version probe: "
                    "capability identity changed"
                )
            if content and _content_sha256(stream) != self.file_hashes[
                self.streams.index(stream)
            ]:
                raise AuditInfrastructureError(
                    "compiler executable capability content changed"
                )
        for path, expected in zip(self.directory_paths, self.directory_snapshots):
            if _directory_snapshot(path) != expected:
                raise AuditInfrastructureError("compiler executable path chain changed")
        self.observer.drain()

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            errors: list[BaseException] = []
            try:
                self.observer.close()
            except BaseException as error:
                errors.append(error)
            for stream in reversed(self.streams):
                try:
                    stream.close()
                except BaseException as error:
                    errors.append(error)
            if errors:
                raise AuditInfrastructureError("compiler capability owner cleanup failed") from errors[0]

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _toolchain_binding_for_path(
    path: Path, dependency_roots: DependencyRootAuthority
) -> DependencyRootBinding:
    matches: list[DependencyRootBinding] = []
    for binding in dependency_roots.external_roots:
        try:
            path.relative_to(binding.resolved_root)
        except ValueError:
            continue
        matches.append(binding)
    if len(matches) != 1:
        raise AuditInfrastructureError("compiler is not mapped by exactly one trusted toolchain root")
    return matches[0]


def _path_chain(sentinel: Path, target: Path) -> tuple[Path, ...]:
    try:
        relative = target.relative_to(sentinel)
    except ValueError as error:
        raise AuditInfrastructureError("compiler path escapes trusted toolchain root") from error
    result = [sentinel]
    current = sentinel
    for part in relative.parts:
        current = current / part
        result.append(current)
    return tuple(result)


def _driver_selected_helper_paths(
    capability: CompilerExecutableCapability,
    family: CompilerFamily,
    working_directory: Path,
    environment: Mapping[str, str],
    root: Path,
    index: Mapping[str, tuple[Path, ...]],
    deadline: float,
    cancel_event: object | None,
) -> tuple[Path, ...]:
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        names = ("c1.dll", "c1xx.dll", "c2.dll", "mspdbcore.dll")
        result = []
        for name in names:
            candidates = index.get(name, ())
            if len(candidates) == 1:
                result.append(candidates[0])
        return tuple(result)
    queries = (
        "cc1", "cc1plus", "collect2", "as", "ld"
    ) if family is CompilerFamily.GCC else ("clang", "ld.lld", "as", "ld")
    result = []
    for program in queries:
        _check_capability_budget(deadline, cancel_event)
        output = _run_probe_command(
            capability,
            (f"-print-prog-name={program}",),
            working_directory,
            environment,
            deadline,
        )
        try:
            value = output.decode("utf-8").strip()
        except UnicodeError as error:
            raise AuditInfrastructureError("compiler helper query output is invalid") from error
        if not value or "\n" in value or "\0" in value or len(value) > 32768:
            raise AuditInfrastructureError("compiler helper query output is invalid")
        resolved = _resolve_runtime_name(
            value, capability.executable_identity.canonical,
            capability.executable_identity.canonical, root, index,
        )
        if resolved is not None and resolved not in result:
            result.append(resolved)
    return tuple(result)


def open_compiler_executable_capability(
    compiler: Path,
    dependency_roots: DependencyRootAuthority,
    pipeline_deadline: float,
    *,
    cancel_event: object | None = None,
    compiler_family: CompilerFamily | None = None,
    launcher_environment: Mapping[str, str] | None = None,
    working_directory: Path | None = None,
    _extra_candidates: tuple[Path, ...] = (),
    _query_driver: bool = True,
) -> CompilerExecutableCapability:
    authority = validate_dependency_root_authority(dependency_roots)
    if not isinstance(pipeline_deadline, (int, float)) or isinstance(pipeline_deadline, bool):
        raise AuditInfrastructureError("compiler capability deadline is invalid")
    _check_capability_budget(pipeline_deadline, cancel_event)
    try:
        canonical = compiler.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise AuditInfrastructureError("compiler executable is unavailable") from error
    binding = _toolchain_binding_for_path(canonical, authority)
    if compiler_family is not None and not isinstance(compiler_family, CompilerFamily):
        raise AuditInfrastructureError("compiler capability family is invalid")
    if launcher_environment is not None and not isinstance(launcher_environment, Mapping):
        raise AuditInfrastructureError("compiler capability environment is invalid")
    memo_key = (
        os.path.normcase(str(canonical)),
        authority.source_root,
        authority.external_roots,
        compiler_family,
    )
    with _compiler_capability_lock:
        memoized = _compiler_capability_memo.get(memo_key)
        if memoized is not None:
            try:
                memoized.native_owner.validate(content=False)
            except AuditInfrastructureError:
                try:
                    memoized.native_owner.close()
                finally:
                    _compiler_capability_memo.pop(memo_key, None)
            else:
                return memoized

    candidates = [canonical]
    helper_names = frozenset({
        "as", "as.exe", "clang-cc1", "clang-cc1.exe", "collect2",
        "collect2.exe", "ld", "ld.exe", "ld.lld", "ld.lld.exe",
    })
    siblings = []
    try:
        for entry_count, candidate in enumerate(canonical.parent.iterdir(), 1):
            _check_capability_budget(pipeline_deadline, cancel_event)
            if entry_count > _RUNTIME_ENUMERATION_ENTRIES:
                raise AuditInfrastructureError(
                    "compiler runtime enumeration ceiling exceeded"
                )
            name = candidate.name.casefold()
            if candidate == canonical:
                continue
            is_runtime_library = (
                name.endswith((".dll", ".dylib", ".so")) or ".so." in name
            )
            if name.startswith("cc1") or name in helper_names or is_runtime_library:
                siblings.append(candidate)
                if len(siblings) + 1 > _RUNTIME_CLOSURE_FILES:
                    raise AuditInfrastructureError(
                        "compiler runtime closure file ceiling exceeded"
                    )
    except OSError as error:
        raise AuditInfrastructureError("compiler runtime closure is unreadable") from error
    candidates.extend(sorted(siblings, key=lambda item: item.name.casefold()))
    candidates.extend(_extra_candidates)
    index = _toolchain_file_index(
        binding.resolved_root, pipeline_deadline, cancel_event
    )
    candidates = list(_recursive_runtime_paths(
        tuple(candidates), canonical, binding.resolved_root, index,
        pipeline_deadline, cancel_event,
    ))

    streams: list[object] = []
    observer: _FilesystemGenerationObserver | None = None
    paths: list[Path] = []
    snapshots: list[tuple[int, int | None, int, int, int, int]] = []
    hashes: list[str] = []
    total_bytes = 0
    try:
        seen: set[tuple[int, int | None]] = set()
        for candidate in candidates:
            _check_capability_budget(pipeline_deadline, cancel_event)
            try:
                resolved = candidate.resolve(strict=True)
                expected = _regular_file_snapshot(resolved)
                if expected[2] > _RUNTIME_FILE_BYTES:
                    raise AuditInfrastructureError(
                        "compiler runtime per-file byte ceiling exceeded"
                    )
                total_bytes += expected[2]
                if total_bytes > _RUNTIME_TOTAL_BYTES:
                    raise AuditInfrastructureError(
                        "compiler runtime total byte ceiling exceeded"
                    )
                stream = resolved.open("rb")
            except OSError as error:
                raise AuditInfrastructureError("compiler runtime closure changed while opening") from error
            opened = os.fstat(stream.fileno())
            opened_key = (
                int(opened.st_dev),
                int(opened.st_ino) if int(opened.st_ino) != 0 else None,
            )
            if opened_key != expected[:2] or opened_key in seen:
                stream.close()
                if opened_key in seen:
                    continue
                raise AuditInfrastructureError("compiler runtime closure identity changed")
            seen.add(opened_key)
            paths.append(resolved)
            snapshots.append(expected)
            streams.append(stream)
            hashes.append(_content_sha256(
                stream, deadline=pipeline_deadline, cancel_event=cancel_event
            ))

        chain_paths = _path_chain(binding.resolved_root.parent, canonical.parent)
        chain_snapshots = tuple(_directory_snapshot(path) for path in chain_paths)
        closure: list[DependencyDigest] = []
        for path, snapshot, content in zip(paths, snapshots, hashes):
            relative = PurePosixPath(path.relative_to(binding.resolved_root).as_posix())
            closure.append(
                DependencyDigest(
                    binding.stable_role,
                    relative,
                    FileIdentity(path, None, snapshot[0], snapshot[1], 0, False),
                    content,
                )
            )
        closure.sort(key=lambda item: (item.stable_role, item.role_relative_path.as_posix(), item.sha256))
        closure_digest = hashlib.sha256(
            b"".join(
                len(portable_compiler_inspection_key_value).to_bytes(8, "little")
                + portable_compiler_inspection_key_value
                for item in closure
                for portable_compiler_inspection_key_value in (
                    item.stable_role.encode("ascii")
                    + b"\0"
                    + item.role_relative_path.as_posix().encode("utf-8")
                    + b"\0"
                    + item.sha256.encode("ascii"),
                )
            )
        ).hexdigest()
        platform_kind = "windows" if os.name == "nt" else ("macos" if sys.platform == "darwin" else "linux")
        capability_digest_hasher = hashlib.sha256()
        for value in (
            platform_kind,
            binding.stable_role,
            canonical.relative_to(binding.resolved_root).as_posix(),
            hashes[0],
            closure_digest,
            authority.portable_authority_digest,
        ):
            _hash_field(capability_digest_hasher, value.encode("utf-8"))
        owner = _CompilerCapabilityOwner(
            tuple(streams), tuple(paths), tuple(snapshots), tuple(hashes),
            chain_paths, chain_snapshots,
            _FilesystemGenerationObserver(
                tuple((path, True) for path in chain_paths)
                + tuple((path, False) for path in paths)
            ),
            authority,
        )
        observer = owner.observer
        capability = CompilerExecutableCapability(
            platform_kind,
            FileIdentity(canonical, None, snapshots[0][0], snapshots[0][1], 0, False),
            hashes[0],
            capability_digest_hasher.hexdigest(),
            owner,
            binding,
            tuple(chain_paths),
            tuple(
                FileIdentity(path, None, snapshot[0], snapshot[1], 0, False)
                for path, snapshot in zip(chain_paths, chain_snapshots)
            ),
            closure_digest,
            tuple(closure),
        )
        owner.validate()
        if compiler_family is not None and _query_driver:
            helpers = _driver_selected_helper_paths(
                capability,
                compiler_family,
                working_directory or canonical.parent,
                launcher_environment or os.environ,
                binding.resolved_root,
                index,
                pipeline_deadline,
                cancel_event,
            )
            if any(path not in paths for path in helpers):
                owner.close()
                return open_compiler_executable_capability(
                    canonical,
                    authority,
                    pipeline_deadline,
                    cancel_event=cancel_event,
                    compiler_family=compiler_family,
                    launcher_environment=launcher_environment,
                    working_directory=working_directory,
                    _extra_candidates=helpers,
                    _query_driver=False,
                )
        with _compiler_capability_lock:
            existing = _compiler_capability_memo.get(memo_key)
            if existing is not None:
                owner.close()
                existing.native_owner.validate(content=False)
                return existing
            _compiler_capability_memo[memo_key] = capability
        return capability
    except BaseException:
        if observer is not None:
            try:
                observer.close()
            except Exception:
                pass
        for stream in reversed(streams):
            try:
                stream.close()
            except Exception:
                pass
        raise


def validate_compiler_executable_capability(
    capability: CompilerExecutableCapability,
    dependency_roots: DependencyRootAuthority,
) -> None:
    authority = validate_dependency_root_authority(
        dependency_roots,
    )
    if not isinstance(capability, CompilerExecutableCapability):
        raise AuditInfrastructureError("compiler executable capability is invalid")
    owner = capability.native_owner
    if not isinstance(owner, _CompilerCapabilityOwner):
        raise AuditInfrastructureError("compiler executable capability owner is invalid")
    if owner.dependency_root_authority != authority:
        raise AuditInfrastructureError("compiler capability local dependency authority differs")
    expected_binding = _toolchain_binding_for_path(
        capability.executable_identity.canonical, authority
    )
    if expected_binding != capability.trusted_toolchain_root:
        raise AuditInfrastructureError("compiler executable capability root differs")
    owner.validate(content=False)


def _compiler_fingerprint(
    compiler: Path,
    normalized_version: bytes,
    expected_snapshot: tuple[int, int, int, int, int],
) -> str:
    before_snapshot = _compiler_metadata_snapshot(compiler)
    if before_snapshot != expected_snapshot:
        raise AuditInfrastructureError(
            f"compiler executable changed during compiler version probe: {compiler}"
        )
    hasher = hashlib.sha256()
    _hash_field(hasher, str(compiler).encode("utf-8", errors="surrogatepass"))
    _hash_field(hasher, str(expected_snapshot[2]).encode("ascii"))
    _hash_field(hasher, str(expected_snapshot[3]).encode("ascii"))
    _hash_field(hasher, str(expected_snapshot[4]).encode("ascii"))
    content = hashlib.sha256()
    try:
        with compiler.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                content.update(chunk)
    except OSError as error:
        raise AuditInfrastructureError(f"compiler executable is unreadable: {compiler}") from error
    if _compiler_metadata_snapshot(compiler) != expected_snapshot:
        raise AuditInfrastructureError(f"compiler executable changed while fingerprinting: {compiler}")
    _hash_field(hasher, content.digest())
    _hash_field(hasher, normalized_version)
    return hasher.hexdigest()


def inspect_compiler(
    compiler: Path,
    compiler_family: CompilerFamily,
    launcher_environment: Mapping[str, str],
    dependency_roots: DependencyRootAuthority,
    inspection_cache=None,
    expected_audit_engine_fingerprint: str = "",
    pipeline_deadline: float | None = None,
    limits: AuditLimits | None = None,
    launch_accountant=None,
    *,
    working_directory: Path | None = None,
) -> CompilerInspection:
    """Inspect a held compiler capability and return portable exact evidence."""

    del limits, launch_accountant
    if not isinstance(compiler_family, CompilerFamily):
        raise AuditInfrastructureError("compiler inspection family is invalid")
    if not isinstance(launcher_environment, Mapping):
        raise AuditInfrastructureError("compiler inspection environment is invalid")
    authority = validate_dependency_root_authority(dependency_roots)
    deadline = (
        time.monotonic() + _VERSION_SECONDS
        if pipeline_deadline is None
        else pipeline_deadline
    )
    if not isinstance(deadline, (int, float)) or isinstance(deadline, bool):
        raise AuditInfrastructureError("compiler inspection deadline is invalid")
    if time.monotonic() >= deadline:
        raise AuditInfrastructureError("compiler inspection deadline exceeded")
    environment_digest = _environment_digest(launcher_environment)
    cwd = working_directory or compiler.parent

    compiler_snapshot = _compiler_metadata_snapshot(compiler)
    capability = open_compiler_executable_capability(
        compiler,
        authority,
        deadline,
        compiler_family=compiler_family,
        launcher_environment=launcher_environment,
        working_directory=cwd,
    )
    owner = capability.native_owner
    try:
        validate_compiler_executable_capability(capability, authority)
        if _compiler_metadata_snapshot(compiler) != compiler_snapshot:
            raise AuditInfrastructureError(
                "compiler executable changed during compiler version probe: "
                f"{compiler}"
            )
        arguments = (
            "/nologo", "/Bv", "/EP", "/TP"
        ) if compiler_family is CompilerFamily.MSVC else ("--version",)
        arguments_digest = hashlib.sha256()
        for value in (
            compiler_family.value,
            *arguments,
            environment_digest,
            authority.portable_authority_digest,
        ):
            _hash_field(arguments_digest, value.encode("utf-8"))
        inspection_arguments_digest = arguments_digest.hexdigest()
        memo_key = (
            capability.executable_sha256,
            capability.capability_digest,
            environment_digest,
            compiler_family,
            authority.portable_authority_digest,
            capability.executable_identity.canonical,
            capability.executable_identity.device,
            capability.executable_identity.inode,
        )

        with _compiler_inspection_lock:
            memoized = _compiler_inspection_memo.get(memo_key)
            if memoized is not None:
                validate_compiler_executable_capability(capability, authority)
                return memoized

            cached = None
            if inspection_cache is not None:
                load = getattr(inspection_cache, "load", None)
                if not callable(load):
                    raise AuditInfrastructureError("compiler inspection cache is invalid")
                cached = load(
                    compiler,
                    compiler_family,
                    launcher_environment,
                    authority,
                    expected_audit_engine_fingerprint,
                    capability.capability_digest,
                    capability.resolved_runtime_closure_digest,
                    deadline,
                )
            if cached is not None:
                if (
                    not isinstance(cached, CompilerInspection)
                    or cached.compiler_family is not compiler_family
                    or cached.executable_identity != capability.executable_identity
                    or cached.executable_sha256 != capability.executable_sha256
                    or cached.inspection_arguments_digest != inspection_arguments_digest
                    or cached.executable_capability_digest != capability.capability_digest
                ):
                    raise AuditInfrastructureError("compiler inspection cache authority differs")
                validate_compiler_executable_capability(capability, authority)
                _compiler_inspection_memo[memo_key] = cached
                return cached

            version_output = _probe_compiler_version(
                capability,
                compiler_family,
                cwd,
                launcher_environment,
                deadline,
            )
            validate_compiler_executable_capability(capability, authority)
            family = identify_compiler(compiler, version_output)
            if family is not compiler_family:
                raise AuditInfrastructureError("compiler inspection family changed")
            normalized_version_bytes = _normalize_version_output(version_output)
            normalized_version = normalized_version_bytes.decode("utf-8")
            driver = hashlib.sha256()
            for value in (
                family.value.encode("ascii"),
                capability.executable_sha256.encode("ascii"),
                normalized_version_bytes,
                capability.capability_digest.encode("ascii"),
                authority.portable_authority_digest.encode("ascii"),
            ):
                _hash_field(driver, value)
            inspection = CompilerInspection(
                family,
                capability.executable_identity,
                capability.executable_sha256,
                normalized_version,
                driver.hexdigest(),
                inspection_arguments_digest,
                capability.capability_digest,
            )
            # Force construction of the relocation-stable key before cache publication.
            portable_compiler_inspection_key(inspection)
            if inspection_cache is not None:
                publish = getattr(inspection_cache, "publish", None)
                if not callable(publish):
                    raise AuditInfrastructureError("compiler inspection cache is invalid")
                inspection = publish(
                    compiler,
                    compiler_family,
                    launcher_environment,
                    authority,
                    expected_audit_engine_fingerprint,
                    capability.capability_digest,
                    capability.resolved_runtime_closure_digest,
                    inspection,
                    deadline,
                )
            validate_compiler_executable_capability(capability, authority)
            _compiler_inspection_memo[memo_key] = inspection
            return inspection
    finally:
        # The process-local capability memo owns the held closure. Configuration
        # objects borrow the same validated capability without reopening it.
        pass


def _inspect_compiler(
    compiler: Path,
    family_hint: CompilerFamily,
    working_directory: Path,
    environment: Mapping[str, str],
    environment_digest: str,
    dependency_roots: DependencyRootAuthority,
) -> CompilerInspection:
    if environment_digest != _environment_digest(environment):
        raise AuditInfrastructureError("compiler environment digest mismatch")
    return inspect_compiler(
        compiler,
        family_hint,
        environment,
        dependency_roots,
        pipeline_deadline=time.monotonic() + _VERSION_SECONDS,
        working_directory=working_directory,
    )


def _canonical_argument_path(value: str, cwd: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = cwd / path
    return path.resolve(strict=False)


def _msvc_slash_spelling(value: str) -> str:
    if value.startswith(("-", "/")):
        return f"/{value[1:]}"
    return value


def _reject_msvc_case_variant(value: str) -> None:
    slash_spelling = _msvc_slash_spelling(value)
    _reject_msvc_exact_case_variant(value)
    if (
        slash_spelling in _MSVC_CASE_DISTINCT_EXACT_OPTIONS
        or slash_spelling.startswith(_MSVC_CASE_DISTINCT_PREFIX_OPTIONS)
        or slash_spelling in _MSVC_CASE_SENSITIVE_EXACT_OPTIONS
        or any(
            slash_spelling.startswith(prefix)
            for prefix in _MSVC_CASE_SENSITIVE_PREFIX_OPTIONS
        )
    ):
        return
    # A dash may introduce an unrelated GNU-compatible clang-cl option such as
    # -fmodules. Exact cl options above still enforce case, but only slash
    # spellings are unambiguously members of the attached cl-option grammar.
    if value.startswith("-"):
        return
    folded = slash_spelling.casefold()
    if any(
        folded == option.casefold()
        for option in _MSVC_AMBIGUOUS_EXACT_OPTIONS
    ) or any(
        folded.startswith(prefix.casefold())
        for prefix in _MSVC_AMBIGUOUS_PREFIX_OPTIONS
    ):
        raise AuditInfrastructureError(
            f"case-sensitive compiler option has unsupported spelling: {value}"
        )


def _reject_msvc_exact_case_variant(value: str) -> None:
    slash_spelling = _msvc_slash_spelling(value)
    if (
        slash_spelling in _MSVC_CASE_DISTINCT_EXACT_OPTIONS
        or slash_spelling.startswith(_MSVC_CASE_DISTINCT_PREFIX_OPTIONS)
    ):
        return
    if slash_spelling in _MSVC_CASE_SENSITIVE_EXACT_OPTIONS:
        return
    folded = slash_spelling.casefold()
    ambiguous = any(
        folded == option.casefold()
        for option in _MSVC_AMBIGUOUS_EXACT_OPTIONS
    )
    # Resolve a dash option before clang-cl's GNU compatibility grammar. In
    # particular, -d takes a GNU operand but is not the documented MSVC /D.
    gnu_arity_collision = value.startswith("-") and value in _GNU_VALUE_OPTIONS and any(
        folded == option.casefold() for option in _MSVC_VALUE_OPTIONS
    )
    if ambiguous or gnu_arity_collision:
        raise AuditInfrastructureError(
            f"case-sensitive compiler option has unsupported spelling: {value}"
        )


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
        if (
            not positional_only
            and family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
            and value.startswith(("-", "/"))
        ):
            # Resolve exact cl spellings before clang-cl's GNU compatibility
            # options: for example, -d must not silently acquire GNU arity and
            # hide a source when /D is the case-sensitive cl spelling.
            _reject_msvc_exact_case_variant(value)
        if not positional_only and value in _GNU_VALUE_OPTIONS and family in {
            CompilerFamily.GCC, CompilerFamily.CLANG, CompilerFamily.CLANG_CL
        }:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        msvc_spelling = _msvc_slash_spelling(value)
        if (
            not positional_only
            and family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
            and value.startswith(("-", "/"))
        ):
            _reject_msvc_case_variant(value)
        if (
            not positional_only
            and msvc_spelling == "/sourceDependencies:directives"
            and family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}
        ):
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        slash_spelling = msvc_spelling
        if not positional_only and slash_spelling in {"/TC", "/TP"} and family in {
            CompilerFamily.MSVC, CompilerFamily.CLANG_CL
        }:
            index += 1
            continue
        if not positional_only and slash_spelling in {"/Tc", "/Tp"} and family in {
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
        if (
            not positional_only
            and slash_spelling.startswith(("/Tc", "/Tp"))
            and len(value) > 3
        ):
            candidate = value[3:]
            if candidate == "-":
                raise AuditInfrastructureError("stdin cannot be a compile source")
            sources.append(_canonical_argument_path(candidate, cwd))
            index += 1
            continue
        if not positional_only and msvc_spelling in _MSVC_VALUE_OPTIONS and family in {
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


_GNU_REWRITE_REMOVE_FLAGS = frozenset({
    "-c", "-S", "-E", "-P", "-M", "-MM", "-MD", "-MMD", "-MG", "-MP",
    "--dependencies", "--user-dependencies", "--write-dependencies",
    "--write-user-dependencies", "--print-missing-file-dependencies",
    "--no-line-commands", "-dM", "-dD", "-dN", "-dI", "-dU",
})
_GNU_REWRITE_REMOVE_VALUES = frozenset({
    "-o", "-MF", "-MT", "-MQ", "-MJ", "-serialize-diagnostics",
    "--serialize-diagnostics", "-dependency-file", "--dependency-file",
    "--output", "--dump", "-d",
})
_GNU_REWRITE_ATTACHED_VALUES = (
    "-MF", "-MT", "-MQ", "-MJ", "-o",
)
_GNU_REWRITE_EQUALS_VALUES = (
    "-fdiagnostics-file=", "-fdiagnostics-serialization-file=",
    "-fmodule-output=", "--output=", "--dump=",
)
_GNU_REWRITE_REJECT_PREFIXES = (
    "-save-temps", "--save-temps", "-dumpbase", "-dumpdir",
    "-fmodule-output", "-fpreprocessed", "-fdirectives-only",
    "-frewrite-includes",
)
_GNU_FORWARDERS = frozenset({"-Xclang", "-Xpreprocessor"})
_GNU_FORWARDED_SEPARATE_VALUE_OPTIONS = frozenset({
    "-D", "-U", "-A", "-I", "-isystem", "-iquote", "-idirafter",
    "-iprefix", "-iwithprefix", "-iwithprefixbefore", "-include", "-imacros",
    "-include-pch",
    "-isysroot", "--sysroot", "--define-macro", "--undefine-macro",
    "--include", "--imacros",
    "-F", "-iframework", "-ivfsoverlay", "-resource-dir",
    "-triple", "-aux-triple", "-target-cpu", "-target-feature", "-target-abi",
    "-fmodules-user-build-path", "-fmodule-implementation-of", "-fmodule-feature",
    "-mrelocation-model", "-mthread-model", "-target-linker-version",
})

_CLANG_FRONTEND_DEPENDENCY_OPTIONS = frozenset({
    "-MD", "-MMD", "-M", "-MM", "-MG", "-MP", "-MF", "-MT", "-MQ", "-MJ",
    "-dependency-dot", "-dependency-file", "--dependency-file",
    "--dependencies", "--user-dependencies", "--write-dependencies",
    "--write-user-dependencies", "--print-missing-file-dependencies",
    "-sys-header-deps", "-module-file-deps", "-show-includes",
})
_CLANG_FRONTEND_OUTPUT_OPTIONS = frozenset({
    "-o", "--output", "--dump", "-d", "-dM", "-dD", "-dN", "-dI", "-dU",
    "-serialize-diagnostics", "--serialize-diagnostics",
    "-serialize-diagnostic-file", "-diagnostic-log-file", "-stats-file",
})
_CLANG_FRONTEND_ACTION_PREFIXES = (
    "-emit-", "-dump-", "-ast-", "-analy", "-plugin", "-add-plugin",
    "-load", "-fplugin", "-code-completion", "-fixit", "-verify",
    "-rewrite-", "-print-", "-action", "-execute", "-arcmt-", "-objcmt-",
    "-migrate", "-index-", "-extract-api",
)
_CLANG_FRONTEND_ACTION_OPTIONS = frozenset({
    "-E", "-Eonly", "-syntax-only", "-fsyntax-only", "-module-file-info",
})
_CLANG_FRONTEND_SAFE_FLAGS = frozenset({
    "-undef", "-nostdinc", "-nostdinc++", "-nobuiltininc", "-pthread",
    "-fmodules", "-fimplicit-module-maps", "-fmodules-decluse",
    "-fmodules-strict-decluse", "-fmodules-local-submodule-visibility",
    "-fcxx-exceptions", "-fexceptions", "-fno-exceptions", "-frtti", "-fno-rtti",
    "-fdelayed-template-parsing", "-fblocks", "-fcoroutines", "-fchar8_t",
    "-fno-char8_t", "-pedantic", "-pedantic-errors",
    "-O", "-O0", "-O1", "-O2", "-O3", "-O4", "-Og", "-Os", "-Oz",
    "-Ofast", "-g", "-g0", "-g1", "-g2", "-g3", "-gline-tables-only",
    "-gline-directives-only", "-mrelax-all", "-mnoexecstack",
    "-masm-verbose", "-mconstructor-aliases", "-msoft-float", "-mstackrealign",
    "-fmodules-validate-once-per-build-session",
    "-fmodule-map-file-home-is-cwd", "-disable-llvm-passes",
    "-fms-extensions", "-fms-compatibility", "-fobjc-arc",
    "-fobjc-arc-exceptions", "-fobjc-weak",
})
_CLANG_FRONTEND_SAFE_PREFIXES = (
    "-D", "-U", "-I", "-F", "-W", "-R",
)
_CLANG_FRONTEND_SAFE_EQUALS_OPTIONS = frozenset({
    "-std", "-stdlib", "-triple", "-target-cpu", "-target-feature",
    "-target-abi", "-target-linker-version", "-target-sdk-version",
    "-fmodule-map-file",
    "-fmodule-file", "-fmodule-name", "-fmodule-format",
    "-fmodules-cache-path", "-fmodules-prune-interval",
    "-fmodules-ignore-macro", "-fmodules-prune-after",
    "-fprebuilt-module-path", "-mframe-pointer", "-mrelocation-model",
    "-mthread-model", "-fms-compatibility-version",
    "-fobjc-runtime", "-debug-info-kind", "-dwarf-version", "-debugger-tuning",
})


def _validate_rewrite_source(configuration: PreprocessConfiguration) -> None:
    sources = _source_inputs(
        configuration.arguments,
        configuration.working_directory,
        configuration.family,
    )
    if len(sources) > 1:
        raise AuditInfrastructureError("compile command has multiple source inputs")
    if not sources or not _same_path(sources[0], configuration.source.canonical):
        raise AuditInfrastructureError("compile command source does not match configuration")


def _gnu_forwarded_control(payload: str) -> str | None:
    candidate = payload
    if not candidate.startswith(("-", "@")):
        return None
    lowered = candidate.casefold()
    option_name = candidate.partition("=")[0]
    if candidate in {"-P", "--no-line-commands"} or lowered.startswith(
        "-frewrite-includes"
    ):
        return "marker"
    if candidate in {"-c", "-S", "-x", "-ObjC", "-ObjC++"} or lowered.startswith(
        ("-fpreprocessed", "-fdirectives-only", "-main-file-name")
    ):
        return "source-selection"
    if "cpp-output" in lowered:
        return "source-selection"
    if candidate in _CLANG_FRONTEND_DEPENDENCY_OPTIONS or any(
        candidate.startswith(prefix) and len(candidate) > len(prefix)
        for prefix in ("-MF", "-MT", "-MQ", "-MJ")
    ) or "dependenc" in option_name.casefold():
        return "dependency"
    if candidate == "-o" or candidate.startswith("-o") and len(candidate) > 2:
        return "output"
    if candidate in _CLANG_FRONTEND_OUTPUT_OPTIONS:
        return "output"
    if lowered.startswith(("--output=", "--dump=")) or candidate == "--dump":
        return "output"
    if lowered.startswith((
        "-save-temps", "--save-temps", "-dumpbase", "-dumpdir",
        "-fmodule-output", "-serialize-diagnostics", "--serialize-diagnostics",
        "-serialize-diagnostic-file", "-diagnostic-log-file", "-stats-file",
        "-fdiagnostics-file=", "-fdiagnostics-serialization-file=",
        "-gen-reproducer",
    )):
        return "output"
    if option_name in _CLANG_FRONTEND_ACTION_OPTIONS or option_name.startswith(
        _CLANG_FRONTEND_ACTION_PREFIXES
    ):
        return "action"
    # cc1 is explicitly unstable. Do not allow a future action selector merely
    # because its spelling was absent when this audit was authored.
    if any(
        fragment in option_name.casefold()
        for fragment in (
            "action", "completion", "plugin", "analy", "fixit", "verify",
            "rewrite", "emit", "dump", "output",
        )
    ):
        return "action"
    if candidate.startswith("@"):
        return "response"
    if (
        candidate in _CLANG_FRONTEND_SAFE_FLAGS
        or candidate.startswith(_CLANG_FRONTEND_SAFE_PREFIXES)
        or (
            option_name in _CLANG_FRONTEND_SAFE_EQUALS_OPTIONS
            and "=" in candidate
            and bool(candidate.partition("=")[2])
        )
    ):
        return None
    return "ambiguous"


@dataclass(frozen=True, slots=True)
class _ForwardedSpan:
    text: str
    start: int
    stop: int


class _CommaSpanCursor:
    __slots__ = ("text", "position", "stop")

    def __init__(self, text: str, position: int, stop: int) -> None:
        self.text = text
        self.position = position
        self.stop = stop

    def next_span(self) -> _ForwardedSpan | None:
        if self.position > self.stop:
            return None
        comma = self.text.find(",", self.position, self.stop)
        if comma < 0:
            result = _ForwardedSpan(self.text, self.position, self.stop)
            self.position = self.stop + 1
            return result
        result = _ForwardedSpan(self.text, self.position, comma)
        self.position = comma + 1
        return result


def _span_equals(span: _ForwardedSpan, value: str) -> bool:
    return span.stop - span.start == len(value) and span.text.startswith(
        value, span.start, span.stop
    )


def _span_startswith(span: _ForwardedSpan, value: str) -> bool:
    return span.text.startswith(value, span.start, span.stop)


def _gnu_flattened_forwarded_arguments(
    payloads: Iterable[str],
) -> Iterator[_ForwardedSpan]:
    """Flatten forwarding grammar in linear time without copying suffixes."""

    source = iter(payloads)
    pending: deque[_ForwardedSpan | _CommaSpanCursor] = deque()

    def next_span() -> _ForwardedSpan:
        while pending:
            current = pending[0]
            if isinstance(current, _ForwardedSpan):
                pending.popleft()
                return current
            result = current.next_span()
            if result is not None:
                return result
            pending.popleft()
        value = next(source)
        return _ForwardedSpan(value, 0, len(value))

    while True:
        try:
            span = next_span()
        except StopIteration:
            break
        while True:
            nested_forwarder = next(
                (
                    candidate for candidate in _GNU_FORWARDERS
                    if _span_equals(span, candidate)
                    or _span_startswith(span, f"{candidate}=")
                ),
                None,
            )
            if nested_forwarder is not None:
                if _span_equals(span, nested_forwarder):
                    try:
                        span = next_span()
                    except StopIteration as error:
                        raise AuditInfrastructureError(
                            f"forwarded compiler option requires a value: {nested_forwarder}"
                        ) from error
                else:
                    start = span.start + len(nested_forwarder) + 1
                    if start == span.stop:
                        value = span.text[span.start:span.stop]
                        raise AuditInfrastructureError(
                            f"forwarded compiler option requires a value: {value}"
                        )
                    span = _ForwardedSpan(span.text, start, span.stop)
                continue
            if (
                span.stop - span.start >= 4
                and span.text[span.start:span.start + 4].casefold() == "-wp,"
            ):
                pending.appendleft(
                    _CommaSpanCursor(span.text, span.start + 4, span.stop)
                )
                break
            yield span
            break


def _gnu_forwarded_sequence_control(payloads: Iterable[str]) -> str | None:
    pending_value: str | None = None
    for span in _gnu_flattened_forwarded_arguments(payloads):
        if pending_value is not None:
            pending_value = None
            continue
        value_option = next(
            (
                candidate for candidate in _GNU_FORWARDED_SEPARATE_VALUE_OPTIONS
                if _span_equals(span, candidate)
            ),
            None,
        )
        if value_option is not None:
            pending_value = value_option
            continue
        if span.start == span.stop or span.text[span.start] not in "-@":
            continue
        category = _gnu_forwarded_control(span.text[span.start:span.stop])
        if category:
            return category
    if pending_value is not None:
        raise AuditInfrastructureError(
            f"forwarded compiler option requires a value: {pending_value}"
        )
    return None


def _clang_cl_forwarded_arguments(arguments: tuple[str, ...]) -> Iterator[str]:
    """Yield clang-cl forwarding in the order the driver constructs cc1 args.

    Clang's driver appends -Wp/-Xpreprocessor values while assembling
    preprocessing options, then appends -Xclang values near the end of the cc1
    command. /clang: is clang-cl's spelling of the latter. Preserve argv order
    within each bucket, but never let an operand cross the real bucket order.
    """

    for xclang_bucket in (False, True):
        index = 0
        while index < len(arguments):
            value = arguments[index]
            option = _msvc_option(value)
            if option.startswith("/clang:"):
                payload = value[len(value.partition(":")[0]) + 1 :]
                if not payload:
                    raise AuditInfrastructureError(
                        f"compiler option requires a value: {value}"
                    )
                if xclang_bucket:
                    yield payload
                index += 1
                continue
            if value.startswith("-Wp,"):
                if not xclang_bucket:
                    yield value
                index += 1
                continue
            forwarder = next(
                (
                    candidate for candidate in _GNU_FORWARDERS
                    if value == candidate or value.startswith(f"{candidate}=")
                ),
                None,
            )
            if forwarder is not None:
                current = _gnu_forwarded_argument(arguments, index, forwarder)
                assert current is not None
                _, end = current
                if (forwarder == "-Xclang") == xclang_bucket:
                    yield from arguments[index:end]
                index = end
                continue
            index += 1


def _gnu_forwarded_argument(
    arguments: tuple[str, ...], index: int, forwarder: str
) -> tuple[str, int] | None:
    value = arguments[index]
    if value == forwarder:
        if index + 1 >= len(arguments):
            raise AuditInfrastructureError(f"compiler option requires a value: {value}")
        return arguments[index + 1], index + 2
    elif value.startswith(f"{forwarder}="):
        payload = value.partition("=")[2]
        if not payload:
            raise AuditInfrastructureError(f"compiler option requires a value: {value}")
        return payload, index + 1
    return None


def _gnu_forwarded_span(
    arguments: tuple[str, ...], index: int, forwarder: str
) -> tuple[int, str | None] | None:
    current = _gnu_forwarded_argument(arguments, index, forwarder)
    if current is None:
        return None
    payload, end = current
    if payload not in _GNU_FORWARDED_SEPARATE_VALUE_OPTIONS:
        return end, _gnu_forwarded_sequence_control((payload,))
    if end >= len(arguments):
        raise AuditInfrastructureError(
            f"forwarded compiler option requires a value: {payload}"
        )
    operand = _gnu_forwarded_argument(arguments, end, forwarder)
    if operand is None:
        raise AuditInfrastructureError(
            f"forwarded compiler option requires a forwarded value: {payload}"
        )
    _, operand_end = operand
    return operand_end, None


def _rewrite_gnu(
    configuration: PreprocessConfiguration, dependency_output: Path
) -> RewrittenCommand:
    rewritten: list[str] = [str(configuration.compiler)]
    arguments = configuration.arguments
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == "--":
            raise AuditInfrastructureError("unsupported source-selection option: --")
        if value in _GNU_REWRITE_REMOVE_FLAGS:
            index += 1
            continue
        if value in _GNU_REWRITE_REMOVE_VALUES:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if value in {"--output=", "--dump="}:
            raise AuditInfrastructureError(f"compiler option requires a value: {value}")
        if any(
            value.startswith(prefix) and len(value) > len(prefix)
            for prefix in _GNU_REWRITE_ATTACHED_VALUES
        ) or value.startswith(_GNU_REWRITE_EQUALS_VALUES):
            index += 1
            continue
        if value.startswith(_GNU_REWRITE_REJECT_PREFIXES):
            category = "source-selection" if value.startswith(
                ("-fpreprocessed", "-fdirectives-only", "-frewrite-includes")
            ) else "output"
            raise AuditInfrastructureError(f"unsupported {category} option: {value}")
        if value.startswith("-Wp,"):
            category = _gnu_forwarded_sequence_control((value,))
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}"
                )
        forwarder = next(
            (candidate for candidate in _GNU_FORWARDERS
             if value == candidate or value.startswith(f"{candidate}=")),
            None,
        )
        if forwarder is not None:
            end, category = _gnu_forwarded_span(arguments, index, forwarder)
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}"
                )
            rewritten.extend(arguments[index:end])
            index = end
            continue
        if value == "-x":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError("compiler option requires a value: -x")
            language = arguments[index + 1]
            if "cpp-output" in language.casefold() or "preprocessed" in language.casefold():
                raise AuditInfrastructureError(
                    f"unsupported source-selection language: {language}"
                )
            rewritten.extend((value, language))
            index += 2
            continue
        if value.startswith("-x") and len(value) > 2:
            language = value[2:]
            if "cpp-output" in language.casefold() or "preprocessed" in language.casefold():
                raise AuditInfrastructureError(
                    f"unsupported source-selection language: {language}"
                )
        if value in _GNU_VALUE_OPTIONS:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            rewritten.extend((value, arguments[index + 1]))
            index += 2
            continue
        rewritten.append(value)
        index += 1
    rewritten.extend(("-E", "-MD", "-MF", str(dependency_output)))
    return RewrittenCommand(
        arguments=tuple(rewritten),
        dependency_output=dependency_output,
        dependency_format="gcc-depfile",
    )


_MSVC_REWRITE_REMOVE_FLAGS = frozenset({
    "/c", "/nologo", "/E", "/P", "/EP", "/showIncludes",
    "/PD", "/PH", "/Fx", "/doc",
})
_MSVC_REWRITE_REMOVE_VALUES = frozenset({
    "/Fo", "/Fe", "/Fd", "/Fi",
    "/Ft", "/experimental:log",
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
})
_MSVC_REWRITE_ATTACHED_VALUES = (
    "/sourceDependencies", "/scanDependencies", "/ifcOutput",
    "/experimental:log", "/doc",
    "/Fo", "/Fe", "/Fd", "/Fi", "/Ft",
)
_MSVC_REWRITE_OPTIONAL_OUTPUT_PREFIXES = ("/FA", "/Fa", "/Fm", "/FR", "/Fr")
_MSVC_REWRITE_REJECT_PREFIXES = (
    "/OUT", "/link", "/Yc", "/LD", "/clr:netcore",
)
_MSVC_REWRITE_PRESERVE_VALUES = frozenset({
    "/D", "/U", "/I", "/Fp", "/Yu", "/external:I", "/AI", "/FU",
    "/reference", "/headerUnit",
})


def _msvc_option(value: str) -> str:
    return _msvc_slash_spelling(value)


def _msvc_attached_required_output(option: str) -> bool:
    for prefix in _MSVC_REWRITE_ATTACHED_VALUES:
        if not option.startswith(prefix) or len(option) == len(prefix):
            continue
        payload = option[len(prefix):]
        if payload == ":":
            raise AuditInfrastructureError(
                f"compiler option requires a value: {option}"
            )
        return True
    return False


def _rewrite_msvc(
    configuration: PreprocessConfiguration, dependency_output: Path
) -> RewrittenCommand:
    rewritten: list[str] = [str(configuration.compiler)]
    arguments = configuration.arguments
    if configuration.family is CompilerFamily.CLANG_CL:
        category = _gnu_forwarded_sequence_control(
            _clang_cl_forwarded_arguments(arguments)
        )
        if category:
            raise AuditInfrastructureError(
                f"hidden {category} option is unsupported: /clang:"
            )
    index = 0
    while index < len(arguments):
        value = arguments[index]
        option = _msvc_option(value)
        if value.startswith(("-", "/")):
            _reject_msvc_case_variant(value)
        slash_spelling = option
        if slash_spelling == "/FI":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            rewritten.extend((value, arguments[index + 1]))
            index += 2
            continue
        if slash_spelling.startswith("/FI"):
            rewritten.append(value)
            index += 1
            continue
        if configuration.family is CompilerFamily.CLANG_CL and slash_spelling == "/d1PP":
            index += 1
            continue
        if slash_spelling.startswith(_MSVC_REWRITE_OPTIONAL_OUTPUT_PREFIXES):
            index += 1
            continue
        forwarder = next(
            (candidate for candidate in _GNU_FORWARDERS
             if value == candidate or value.startswith(f"{candidate}=")),
            None,
        )
        if forwarder is not None:
            if configuration.family is CompilerFamily.CLANG_CL:
                current = _gnu_forwarded_argument(arguments, index, forwarder)
                assert current is not None
                _, end = current
                rewritten.extend(arguments[index:end])
                index = end
                continue
            end, category = _gnu_forwarded_span(arguments, index, forwarder)
            if category:
                raise AuditInfrastructureError(
                    f"hidden {category} option is unsupported: {value}"
                )
            rewritten.extend(arguments[index:end])
            index = end
            continue
        if option == "/sourceDependencies:directives":
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if option.startswith("/clang:"):
            rewritten.append(value)
            index += 1
            continue
        if option in _MSVC_REWRITE_REMOVE_FLAGS or option.startswith("/showIncludes:"):
            index += 1
            continue
        if option in _MSVC_REWRITE_REMOVE_VALUES:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            index += 2
            continue
        if _msvc_attached_required_output(option):
            index += 1
            continue
        if option in _MSVC_REWRITE_PRESERVE_VALUES:
            if index + 1 >= len(arguments):
                raise AuditInfrastructureError(f"compiler option requires a value: {value}")
            rewritten.extend((value, arguments[index + 1]))
            index += 2
            continue
        if option.startswith(_MSVC_REWRITE_REJECT_PREFIXES):
            category = "source-selection" if option.startswith(("/link", "/yc")) else "output"
            raise AuditInfrastructureError(f"unsupported {category} option: {value}")
        rewritten.append(value)
        index += 1
    rewritten.extend(("/nologo", "/E", "/sourceDependencies", str(dependency_output)))
    return RewrittenCommand(
        arguments=tuple(rewritten),
        dependency_output=dependency_output,
        dependency_format="msvc-json",
    )


def rewrite_preprocess_command(
    configuration: PreprocessConfiguration, dependency_output: Path
) -> RewrittenCommand:
    """Rewrite one normalized compile command into a fail-closed preprocess command."""

    if not isinstance(configuration, PreprocessConfiguration):
        raise AuditInfrastructureError("preprocess configuration is invalid")
    if not isinstance(dependency_output, Path) or not dependency_output.is_absolute():
        raise AuditInfrastructureError("dependency output must be an absolute path")
    _validate_arguments((str(configuration.compiler), *configuration.arguments))
    _validate_rewrite_source(configuration)
    if configuration.family in {CompilerFamily.GCC, CompilerFamily.CLANG}:
        return _rewrite_gnu(configuration, dependency_output)
    if configuration.family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        return _rewrite_msvc(configuration, dependency_output)
    raise AuditInfrastructureError(
        f"unsupported compiler family: {configuration.family}"
    )


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


def _reject_msvc_environment_arguments(environment: Mapping[str, str]) -> None:
    for name, value in environment.items():
        normalized = name.casefold()
        if normalized in {"cl", "_cl_"} and value.strip():
            raise AuditInfrastructureError(
                f"MSVC environment command line {name} is unsupported"
            )


def make_configuration(
    entry: Mapping[str, object],
    database: Path,
    entry_index: int,
    source_root: Path,
    production: Mapping[PurePosixPath, FileIdentity],
    environment: Mapping[str, str],
    limits: AuditLimits,
    dependency_roots: DependencyRootAuthority,
    inspection_cache=None,
    expected_audit_engine_fingerprint: str = "",
    pipeline_deadline: float | None = None,
) -> PreprocessConfiguration:
    """Build one stable semantic configuration from a compile database entry."""

    authority = validate_dependency_root_authority(dependency_roots)
    if authority.source_root.resolved_root != source_root.resolve(strict=False):
        raise AuditInfrastructureError("compile source root differs from dependency authority")
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
    environment_digest = _environment_digest(environment)
    deadline = pipeline_deadline or (time.monotonic() + limits.total_seconds)
    inspection = inspect_compiler(
        compiler,
        family_hint,
        environment,
        authority,
        inspection_cache,
        expected_audit_engine_fingerprint,
        deadline,
        limits,
        working_directory=cwd,
    )
    family = inspection.compiler_family
    compiler_fingerprint = inspection.driver_fingerprint
    compiler_capability = open_compiler_executable_capability(
        compiler, authority, deadline, compiler_family=family,
        launcher_environment=environment, working_directory=cwd,
    )
    if compiler_capability.capability_digest != inspection.executable_capability_digest:
        compiler_capability.native_owner.close()
        raise AuditInfrastructureError("compiler capability changed after inspection")
    if family in {CompilerFamily.MSVC, CompilerFamily.CLANG_CL}:
        _reject_msvc_environment_arguments(environment)
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

    semantic = {
        "schema": 1,
        "family": family.value,
        "compiler": str(compiler),
        "compiler_fingerprint": compiler_fingerprint,
        "compiler_metadata": _compiler_metadata_snapshot(compiler),
        "working_directory": str(cwd),
        "source": str(source.canonical),
        "arguments": expanded_arguments,
        "environment_digest": environment_digest,
    }
    semantic_bytes = json.dumps(
        semantic,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    digest_builder = hashlib.sha256()
    _hash_field(digest_builder, semantic_bytes)
    _hash_field(
        digest_builder, authority.portable_authority_digest.encode("ascii")
    )
    _hash_field(digest_builder, compiler_capability.capability_digest.encode("ascii"))
    digest = digest_builder.hexdigest()
    return PreprocessConfiguration(
        entry_id=f"{database.resolve()}:{entry_index}",
        family=family,
        compiler=compiler,
        working_directory=cwd,
        source=source,
        arguments=expanded_arguments,
        environment_digest=environment_digest,
        digest=digest,
        dependency_root_authority_digest=authority.portable_authority_digest,
        compiler_capability_digest=compiler_capability.capability_digest,
        compiler_capability=compiler_capability,
    )
