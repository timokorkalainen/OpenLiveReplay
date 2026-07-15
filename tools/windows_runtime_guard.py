#!/usr/bin/env python3
"""Reject build-tree DLLs that shadow the configured Windows runtime."""

from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
import re
import struct
from typing import NamedTuple


class RuntimeRoots(NamedTuple):
    compiler_bin: Path
    qt_bin: Path
    ffmpeg_bin: Path
    ffmpeg_lib: Path
    srt_bin: Path
    srt_lib: Path


_FFMPEG_FAMILY = re.compile(
    r"^(avcodec|avdevice|avfilter|avformat|avutil|postproc|swresample|swscale)-\d+\.dll$",
    re.IGNORECASE,
)
_QT_FAMILY = re.compile(r"^qt\d+([a-z0-9_]+)\.dll$", re.IGNORECASE)
_SRT_FAMILY = re.compile(r"^(?:lib)?srt(?:-[a-z0-9_.-]+)?\.dll$", re.IGNORECASE)
_TOOLCHAIN_FAMILY = re.compile(
    r"^(lib(?:atomic|gcc_[a-z0-9_]+|gfortran|gomp|quadmath|stdc\+\+|winpthread))(?:-\d+)?\.dll$",
    re.IGNORECASE,
)


def _controlled_family(name: str) -> tuple[str, str] | None:
    lowered = name.casefold()
    match = _FFMPEG_FAMILY.fullmatch(lowered)
    if match:
        return "OLR_FFMPEG_ROOT", match.group(1).casefold()
    match = _QT_FAMILY.fullmatch(lowered)
    if match:
        return "Qt6_DIR", match.group(1).casefold()
    if _SRT_FAMILY.fullmatch(lowered):
        return "OLR_SRT_ROOT", "srt"
    match = _TOOLCHAIN_FAMILY.fullmatch(lowered)
    if match:
        return "CMAKE_CXX_COMPILER", match.group(1).casefold()
    return None


class _VsFixedFileInfo(ctypes.Structure):
    _fields_ = [
        ("signature", ctypes.c_uint32),
        ("structure_version", ctypes.c_uint32),
        ("file_version_ms", ctypes.c_uint32),
        ("file_version_ls", ctypes.c_uint32),
        ("product_version_ms", ctypes.c_uint32),
        ("product_version_ls", ctypes.c_uint32),
        ("file_flags_mask", ctypes.c_uint32),
        ("file_flags", ctypes.c_uint32),
        ("file_os", ctypes.c_uint32),
        ("file_type", ctypes.c_uint32),
        ("file_subtype", ctypes.c_uint32),
        ("file_date_ms", ctypes.c_uint32),
        ("file_date_ls", ctypes.c_uint32),
    ]


def _file_version(path: Path) -> str:
    if os.name != "nt":
        return "unknown"
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = (ctypes.c_wchar_p, ctypes.c_void_p)
    version.GetFileVersionInfoSizeW.restype = ctypes.c_uint32
    version.GetFileVersionInfoW.argtypes = (
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    )
    version.GetFileVersionInfoW.restype = ctypes.c_int
    version.VerQueryValueW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint32),
    )
    version.VerQueryValueW.restype = ctypes.c_int

    size = version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return "unknown"
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, buffer):
        return "unknown"
    value = ctypes.c_void_p()
    length = ctypes.c_uint32()
    if not version.VerQueryValueW(buffer, "\\", ctypes.byref(value), ctypes.byref(length)):
        return "unknown"
    if length.value < ctypes.sizeof(_VsFixedFileInfo):
        return "unknown"
    info = ctypes.cast(value, ctypes.POINTER(_VsFixedFileInfo)).contents
    if info.signature != 0xFEEF04BD:
        return "unknown"
    return ".".join(
        str(component)
        for component in (
            info.file_version_ms >> 16,
            info.file_version_ms & 0xFFFF,
            info.file_version_ls >> 16,
            info.file_version_ls & 0xFFFF,
        )
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _describe(path: Path) -> str:
    return f"{path} (version {_file_version(path)}, sha256 {_sha256(path)})"


def pe_imports(path: Path) -> set[str]:
    """Return normal and delay-load PE import basenames without loading the file."""

    try:
        with path.open("rb") as stream:
            file_size = os.fstat(stream.fileno()).st_size

            def read_at(offset: int, size: int) -> bytes:
                if offset < 0 or size < 0 or offset + size > file_size:
                    raise ValueError("truncated PE field")
                stream.seek(offset)
                value = stream.read(size)
                if len(value) != size:
                    raise ValueError("truncated PE field")
                return value

            def u16(offset: int) -> int:
                return struct.unpack("<H", read_at(offset, 2))[0]

            def u32(offset: int) -> int:
                return struct.unpack("<I", read_at(offset, 4))[0]

            if file_size < 64 or read_at(0, 2) != b"MZ":
                return set()
            pe_offset = u32(0x3C)
            if pe_offset + 24 > file_size or read_at(pe_offset, 4) != b"PE\0\0":
                return set()
            section_count = u16(pe_offset + 6)
            optional_size = u16(pe_offset + 20)
            optional = pe_offset + 24
            optional_end = optional + optional_size
            if optional_end > file_size:
                raise ValueError("truncated PE optional header")
            if optional_size < 2:
                raise ValueError("truncated PE optional header magic")
            magic = u16(optional)
            if magic == 0x20B:
                directory_count_offset = optional + 108
                directories_offset = optional + 112
                image_base_offset = optional + 24
                image_base_size = 8
            elif magic == 0x10B:
                directory_count_offset = optional + 92
                directories_offset = optional + 96
                image_base_offset = optional + 28
                image_base_size = 4
            else:
                return set()
            if directory_count_offset + 4 > optional_end:
                raise ValueError("PE data directories exceed optional header")
            if image_base_offset + image_base_size > optional_end:
                raise ValueError("PE image base exceeds optional header")
            directory_count = u32(directory_count_offset)
            available_directories = max(0, (optional_end - directories_offset) // 8)
            if directory_count > available_directories:
                raise ValueError("PE data directories exceed optional header")
            image_base = (
                struct.unpack("<Q", read_at(image_base_offset, 8))[0]
                if image_base_size == 8
                else u32(image_base_offset)
            )

            def data_directory(index: int) -> tuple[int, int]:
                if index >= directory_count:
                    return 0, 0
                offset = directories_offset + index * 8
                if offset + 8 > optional_end:
                    raise ValueError("PE data directories exceed optional header")
                return struct.unpack("<II", read_at(offset, 8))

            sections_offset = optional_end
            sections: list[tuple[int, int, int]] = []
            for index in range(section_count):
                section = sections_offset + index * 40
                if section + 40 > file_size:
                    raise ValueError("truncated PE section table")
                virtual_address = u32(section + 12)
                raw_size = u32(section + 16)
                raw_offset = u32(section + 20)
                if raw_size and raw_offset + raw_size > file_size:
                    raise ValueError("truncated PE raw section data")
                sections.append((virtual_address, raw_size, raw_offset))

            def rva_location(rva: int, size: int = 1) -> tuple[int, int]:
                if rva < 0 or size < 0 or rva + size > 0x100000000:
                    raise ValueError(f"PE RVA is outside raw section data: 0x{rva:x}")
                for virtual_address, raw_size, raw_offset in sections:
                    delta = rva - virtual_address
                    if 0 <= delta < raw_size and size <= raw_size - delta:
                        return raw_offset + delta, raw_size - delta
                raise ValueError(f"PE RVA is outside raw section data: 0x{rva:x}")

            def c_string(rva: int) -> str:
                offset, available = rva_location(rva)
                value = read_at(offset, min(1025, available))
                end = value.find(b"\0")
                if end < 0:
                    raise ValueError("unterminated PE import name")
                return value[:end].decode("ascii", errors="strict")

            imports: set[str] = set()
            import_rva, import_size = data_directory(1)
            if import_rva or import_size:
                if not import_rva or import_size < 20:
                    raise ValueError("malformed PE import descriptor directory")
                if import_size // 20 > 65536:
                    raise ValueError("excessive PE import descriptor count")
                terminated = False
                for offset in range(0, import_size - 19, 20):
                    descriptor, _available = rva_location(import_rva + offset, 20)
                    fields = struct.unpack("<IIIII", read_at(descriptor, 20))
                    if not any(fields):
                        terminated = True
                        break
                    imports.add(c_string(fields[3]).casefold())
                if not terminated:
                    raise ValueError(
                        "PE import descriptor has no terminator inside declared size"
                    )

            delay_rva, delay_size = data_directory(13)
            if delay_rva or delay_size:
                if not delay_rva or delay_size < 32:
                    raise ValueError("malformed PE delay-import descriptor directory")
                if delay_size // 32 > 65536:
                    raise ValueError("excessive PE delay-import descriptor count")
                terminated = False
                for offset in range(0, delay_size - 31, 32):
                    descriptor, _available = rva_location(delay_rva + offset, 32)
                    fields = struct.unpack("<IIIIIIII", read_at(descriptor, 32))
                    if not any(fields):
                        terminated = True
                        break
                    attributes, name_value = fields[0], fields[1]
                    if attributes & 1:
                        name_rva = name_value
                    else:
                        if name_value < image_base:
                            raise ValueError(
                                "PE delay-import VA is below image base"
                            )
                        name_rva = name_value - image_base
                        if name_rva > 0xFFFFFFFF:
                            raise ValueError("PE delay-import VA exceeds RVA range")
                    imports.add(c_string(name_rva).casefold())
                if not terminated:
                    raise ValueError(
                        "PE delay-import descriptor has no terminator inside declared size"
                    )
            return imports
    except OSError as error:
        raise OSError(f"cannot inspect PE imports for {path}: {error}") from error


def _cmake_add_test_arguments(text: str) -> tuple[tuple[str, ...], ...]:
    """Parse literal arguments from generated ``add_test(...)`` commands."""
    commands: list[tuple[str, ...]] = []
    for command_match in re.finditer(r"(?i)\badd_test\s*\(", text):
        index = command_match.end()
        arguments: list[str] = []
        while index < len(text):
            while index < len(text) and text[index].isspace():
                index += 1
            if index >= len(text) or text[index] == ")":
                break
            if text[index] == '"':
                index += 1
                value: list[str] = []
                while index < len(text):
                    character = text[index]
                    if character == '"':
                        index += 1
                        break
                    if character == "\\" and index + 1 < len(text):
                        index += 1
                        value.append(text[index])
                    else:
                        value.append(character)
                    index += 1
                else:
                    raise ValueError("unterminated quoted add_test argument")
                arguments.append("".join(value))
                continue
            bracket = re.match(r"\[(=*)\[", text[index:])
            if bracket:
                opening = bracket.group(0)
                closing = "]" + bracket.group(1) + "]"
                value_start = index + len(opening)
                value_end = text.find(closing, value_start)
                if value_end < 0:
                    raise ValueError("unterminated bracket add_test argument")
                arguments.append(text[value_start:value_end])
                index = value_end + len(closing)
                continue
            value_start = index
            while (
                index < len(text)
                and not text[index].isspace()
                and text[index] != ")"
            ):
                index += 1
            arguments.append(text[value_start:index])
        else:
            raise ValueError("unterminated add_test command")
        if index >= len(text) or text[index] != ")":
            raise ValueError("unterminated add_test command")
        commands.append(tuple(arguments))
    return tuple(commands)


def registered_test_executables(build_dir: Path) -> tuple[Path, ...]:
    """Return existing build-contained executable commands registered with CTest."""
    build_dir = build_dir.resolve()
    executables: set[Path] = set()
    for testfile in build_dir.rglob("CTestTestfile.cmake"):
        text = testfile.read_text(encoding="utf-8", errors="replace")
        for arguments in _cmake_add_test_arguments(text):
            for argument in arguments:
                candidate = argument.rpartition("=")[2]
                if not candidate.casefold().endswith(".exe"):
                    continue
                command = Path(candidate)
                executable = (
                    command if command.is_absolute() else testfile.parent / command
                ).resolve()
                try:
                    executable.relative_to(build_dir)
                except ValueError:
                    continue
                if executable.is_file():
                    executables.add(executable)
    return tuple(sorted(executables, key=lambda path: str(path).casefold()))


def _configured_dlls(roots: RuntimeRoots) -> dict[str, Path]:
    """Index configured DLLs in the same precedence order as launcher PATH."""
    configured: dict[str, Path] = {}
    for directory in roots:
        children = sorted(directory.iterdir(), key=lambda path: path.name.casefold())
        for child in children:
            if child.is_file() and child.suffix.casefold() == ".dll":
                configured.setdefault(child.name.casefold(), child)
    return configured


def _authoritative_families(
    roots: RuntimeRoots,
) -> dict[tuple[str, str], dict[str, Path]]:
    authoritative: dict[tuple[str, str], dict[str, Path]] = {}
    owners = (
        ("CMAKE_CXX_COMPILER", (roots.compiler_bin,)),
        ("Qt6_DIR", (roots.qt_bin,)),
        ("OLR_FFMPEG_ROOT", (roots.ffmpeg_bin, roots.ffmpeg_lib)),
        ("OLR_SRT_ROOT", (roots.srt_bin, roots.srt_lib)),
    )
    for owner, directories in owners:
        for directory in directories:
            for child in sorted(directory.iterdir(), key=lambda path: path.name.casefold()):
                if not child.is_file() or child.suffix.casefold() != ".dll":
                    continue
                family = _controlled_family(child.name)
                if family is None or family[0] != owner:
                    continue
                authoritative.setdefault(family, {}).setdefault(
                    child.name.casefold(), child
                )
    return authoritative


def _expected_family_text(candidates: dict[str, Path]) -> str:
    if not candidates:
        return "no matching DLL"
    return ", ".join(str(path) for path in candidates.values())


def reject_runtime_dll_shadowing(
    directory: Path,
    roots: RuntimeRoots,
    *,
    executables: tuple[Path, ...] = (),
) -> None:
    """Fail when an adjacent controlled-runtime DLL differs from the cache source."""
    if not directory.is_dir():
        return
    configured_dlls = _configured_dlls(roots)
    authoritative = _authoritative_families(roots)
    adjacent_cache: dict[Path, dict[str, Path]] = {}
    digest_cache: dict[Path, str] = {}

    def digest(path: Path) -> str:
        key = path.absolute()
        cached = digest_cache.get(key)
        if cached is None:
            cached = _sha256(path)
            digest_cache[key] = cached
        return cached

    def adjacent_dlls(parent: Path) -> dict[str, Path]:
        parent = parent.resolve()
        cached = adjacent_cache.get(parent)
        if cached is not None:
            return cached
        adjacent_cache[parent] = found = {}
        if not parent.is_dir():
            return found
        for adjacent in sorted(parent.iterdir(), key=lambda path: path.name.casefold()):
            if not adjacent.is_file() or adjacent.suffix.casefold() != ".dll":
                continue
            name = adjacent.name.casefold()
            found[name] = adjacent
            family = _controlled_family(name)
            if family is not None:
                candidates = authoritative.get(family, {})
                configured = candidates.get(name)
                if configured is None:
                    raise RuntimeError(
                        "stale controlled-family DLL is not provided by active "
                        f"{family[0]}: {_describe(adjacent)}; active family provides "
                        f"{_expected_family_text(candidates)}. Reconfigure and rebuild "
                        "the selected build tree."
                    )
            else:
                configured = configured_dlls.get(name)
            if configured is None:
                continue
            if digest(adjacent) != digest(configured):
                raise RuntimeError(
                    "incompatible runtime DLL shadows the configured Windows runtime: "
                    f"{_describe(adjacent)}; configured peer: {_describe(configured)}. "
                    "Remove the stale build tree or reconfigure and rebuild it."
                )
        return found

    adjacent_dlls(directory)

    resolved_executables = tuple(executable.resolve() for executable in executables)
    pending = [(executable, executable.parent) for executable in resolved_executables]
    inspected: set[tuple[Path, Path]] = set()
    while pending:
        binary, executable_dir = pending.pop()
        inspection = (binary, executable_dir)
        if inspection in inspected or not binary.is_file():
            continue
        if len(inspected) >= 4096:
            raise RuntimeError("Windows runtime import closure exceeds 4096 binaries")
        inspected.add(inspection)
        binary_adjacent = adjacent_dlls(executable_dir)
        for imported_name in pe_imports(binary):
            imported_name = imported_name.casefold()
            resolved = binary_adjacent.get(imported_name)
            if resolved is None:
                resolved = configured_dlls.get(imported_name)
            family = _controlled_family(imported_name)
            if family is not None:
                candidates = authoritative.get(family, {})
                configured = candidates.get(imported_name)
                if configured is None:
                    raise RuntimeError(
                        f"{binary} imports stale controlled runtime {imported_name}; "
                        f"active {family[0]} family provides "
                        f"{_expected_family_text(candidates)}. Reconfigure and rebuild "
                        "the selected executable."
                    )
                if resolved is None:
                    raise RuntimeError(
                        f"{binary} imports controlled runtime {imported_name}, but "
                        "Windows search order resolves no configured peer"
                    )
                if (
                    resolved != configured
                    and digest(resolved) != digest(configured)
                ):
                    raise RuntimeError(
                        f"{binary} imports {imported_name}, but an earlier Windows "
                        f"search path resolves incompatible peer {_describe(resolved)}; "
                        f"authoritative {family[0]} peer: {_describe(configured)}. "
                        "Reconfigure the dependency roots so authoritative runtime "
                        "families resolve first."
                    )
            if resolved is not None:
                pending.append((resolved, executable_dir))
