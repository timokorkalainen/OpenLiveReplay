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
            magic = u16(optional)
            if magic == 0x20B:
                directory_count_offset = optional + 108
                directories_offset = optional + 112
            elif magic == 0x10B:
                directory_count_offset = optional + 92
                directories_offset = optional + 96
            else:
                return set()
            if optional + optional_size > file_size:
                raise ValueError("truncated PE optional header")
            directory_count = u32(directory_count_offset)
            sections_offset = optional + optional_size
            sections: list[tuple[int, int, int]] = []
            for index in range(section_count):
                section = sections_offset + index * 40
                if section + 40 > file_size:
                    raise ValueError("truncated PE section table")
                virtual_size = u32(section + 8)
                virtual_address = u32(section + 12)
                raw_size = u32(section + 16)
                raw_offset = u32(section + 20)
                sections.append(
                    (virtual_address, max(virtual_size, raw_size), raw_offset)
                )

            def rva_offset(rva: int) -> int:
                for virtual_address, span, raw_offset in sections:
                    if virtual_address <= rva < virtual_address + span:
                        result = raw_offset + rva - virtual_address
                        if result >= file_size:
                            break
                        return result
                raise ValueError(f"PE RVA is outside file sections: 0x{rva:x}")

            def c_string(offset: int) -> str:
                value = read_at(offset, min(1025, file_size - offset))
                end = value.find(b"\0")
                if end < 0:
                    raise ValueError("unterminated PE import name")
                return value[:end].decode("ascii", errors="strict")

            imports: set[str] = set()
            if directory_count > 1:
                import_rva = u32(directories_offset + 8)
                if import_rva:
                    descriptor = rva_offset(import_rva)
                    for _ in range(65536):
                        if descriptor + 20 > file_size:
                            raise ValueError("truncated PE import descriptor")
                        fields = struct.unpack("<IIIII", read_at(descriptor, 20))
                        if not any(fields):
                            break
                        name_rva = fields[3]
                        imports.add(c_string(rva_offset(name_rva)).casefold())
                        descriptor += 20
                    else:
                        raise ValueError("excessive PE import descriptor count")

            # Delay import descriptor (data-directory index 13). Attributes bit 0
            # selects RVAs; old VA-form descriptors are uncommon and intentionally
            # ignored rather than guessed without the image base.
            if directory_count > 13:
                delay_rva = u32(directories_offset + 13 * 8)
                if delay_rva:
                    descriptor = rva_offset(delay_rva)
                    for _ in range(65536):
                        if descriptor + 32 > file_size:
                            raise ValueError("truncated PE delay-import descriptor")
                        fields = struct.unpack("<IIIIIIII", read_at(descriptor, 32))
                        if not any(fields):
                            break
                        attributes, name_rva = fields[0], fields[1]
                        if attributes & 1:
                            imports.add(c_string(rva_offset(name_rva)).casefold())
                        descriptor += 32
                    else:
                        raise ValueError("excessive PE delay-import descriptor count")
            return imports
    except OSError as error:
        raise OSError(f"cannot inspect PE imports for {path}: {error}") from error


def registered_test_executables(build_dir: Path) -> tuple[Path, ...]:
    """Return existing build-contained executable commands registered with CTest."""
    build_dir = build_dir.resolve()
    executables: set[Path] = set()
    for testfile in build_dir.rglob("CTestTestfile.cmake"):
        text = testfile.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(
            r'(?im)^\s*add_test\([^\r\n]*?"([^"]+\.exe)"', text
        ):
            command = Path(match.group(1))
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
            if _sha256(adjacent) != _sha256(configured):
                raise RuntimeError(
                    "incompatible runtime DLL shadows the configured Windows runtime: "
                    f"{_describe(adjacent)}; configured peer: {_describe(configured)}. "
                    "Remove the stale build tree or reconfigure and rebuild it."
                )
        return found

    adjacent_dlls(directory)

    pending = list(executables)
    inspected: set[Path] = set()
    while pending:
        binary = pending.pop().resolve()
        if binary in inspected or not binary.is_file():
            continue
        inspected.add(binary)
        binary_adjacent = adjacent_dlls(binary.parent)
        for imported_name in pe_imports(binary):
            imported_name = imported_name.casefold()
            family = _controlled_family(imported_name)
            if family is not None:
                candidates = authoritative.get(family, {})
                if imported_name not in candidates:
                    raise RuntimeError(
                        f"{binary} imports stale controlled runtime {imported_name}; "
                        f"active {family[0]} family provides "
                        f"{_expected_family_text(candidates)}. Reconfigure and rebuild "
                        "the selected executable."
                    )
            imported_adjacent = binary_adjacent.get(imported_name)
            if imported_adjacent is not None and imported_adjacent not in inspected:
                pending.append(imported_adjacent)
