#!/usr/bin/env python3
"""Reject build-tree DLLs that shadow the configured Windows runtime."""

from __future__ import annotations

import ctypes
from contextlib import nullcontext
import hashlib
import os
from pathlib import Path
import re
import struct
from typing import BinaryIO, NamedTuple
import xml.etree.ElementTree as ET


class RuntimeRoots(NamedTuple):
    compiler_bin: Path
    qt_bin: Path
    ffmpeg_bin: Path
    ffmpeg_lib: Path
    srt_bin: Path
    srt_lib: Path


class RuntimeAuditLease:
    """Hold audited binaries read-only until the controlled child exits."""

    def __init__(self) -> None:
        self._streams: dict[Path, BinaryIO] = {}

    def __enter__(self) -> RuntimeAuditLease:
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        self.close()

    def hold(self, path: Path) -> BinaryIO:
        key = path.absolute()
        existing = self._streams.get(key)
        if existing is not None:
            return existing
        stream = self._open_read_locked(key)
        self._streams[key] = stream
        return stream

    def close(self) -> None:
        streams = tuple(reversed(tuple(self._streams.values())))
        self._streams.clear()
        first_error: BaseException | None = None
        for stream in streams:
            try:
                stream.close()
            except BaseException as error:
                if first_error is None:
                    first_error = error
        if first_error is not None:
            raise first_error

    @staticmethod
    def _open_read_locked(path: Path) -> BinaryIO:
        if os.name != "nt":
            return path.open("rb")

        import msvcrt

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.argtypes = (
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        )
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = kernel32.CreateFileW(
            str(path),
            0x80000000,  # GENERIC_READ
            0x00000001,  # FILE_SHARE_READ: deny replacement/write/delete
            None,
            3,  # OPEN_EXISTING
            0x08000000,  # FILE_FLAG_SEQUENTIAL_SCAN
            None,
        )
        invalid_handle = ctypes.c_void_p(-1).value
        if handle == invalid_handle:
            error = ctypes.get_last_error()
            raise OSError(error, f"cannot hold audited runtime file {path}")
        try:
            descriptor = msvcrt.open_osfhandle(
                int(handle), os.O_RDONLY | getattr(os, "O_BINARY", 0)
            )
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
        return os.fdopen(descriptor, "rb", closefd=True)


def has_embedded_application_manifest(path: Path) -> bool:
    """Return whether PE resource ID 1 is a valid embedded RT_MANIFEST."""
    if os.name != "nt":
        raise OSError("embedded Windows manifests can only be inspected on Windows")

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryExW.argtypes = (
        ctypes.c_wchar_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
    )
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    kernel32.FindResourceW.argtypes = (
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    )
    kernel32.FindResourceW.restype = ctypes.c_void_p
    kernel32.SizeofResource.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    kernel32.SizeofResource.restype = ctypes.c_uint32
    kernel32.LoadResource.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    kernel32.LoadResource.restype = ctypes.c_void_p
    kernel32.LockResource.argtypes = (ctypes.c_void_p,)
    kernel32.LockResource.restype = ctypes.c_void_p
    kernel32.FreeLibrary.argtypes = (ctypes.c_void_p,)
    kernel32.FreeLibrary.restype = ctypes.c_int

    module = kernel32.LoadLibraryExW(
        str(path),
        None,
        0x00000040 | 0x00000020,  # AS_DATAFILE_EXCLUSIVE | AS_IMAGE_RESOURCE
    )
    if not module:
        error = ctypes.get_last_error()
        raise ValueError(
            f"cannot inspect embedded PE resources for {path}: Windows error {error}"
        )
    try:
        resource = kernel32.FindResourceW(
            module,
            ctypes.c_void_p(1),
            ctypes.c_void_p(24),  # RT_MANIFEST
        )
        if not resource:
            error = ctypes.get_last_error()
            if error in (0, 1812, 1813, 1814, 1815):
                return False
            raise ValueError(
                f"cannot inspect embedded PE resource for {path}: Windows error {error}"
            )
        size = int(kernel32.SizeofResource(module, resource))
        loaded = kernel32.LoadResource(module, resource)
        locked = kernel32.LockResource(loaded) if loaded else None
        if not size or not locked:
            raise ValueError(f"malformed embedded PE resource for {path}")
        contents = ctypes.string_at(locked, size)
        try:
            root = ET.fromstring(contents)
        except ET.ParseError as error:
            raise ValueError(
                f"invalid embedded application manifest XML for {path}: {error}"
            ) from error
        if root.tag.rsplit("}", 1)[-1] != "assembly":
            raise ValueError(
                f"invalid embedded application manifest XML root for {path}"
            )
        if root.attrib.get("manifestVersion") != "1.0":
            raise ValueError(
                f"invalid embedded application manifest XML version for {path}"
            )
        return True
    finally:
        kernel32.FreeLibrary(module)


def reject_dotlocal_dev_override() -> None:
    """Fail if machine policy can override manifest-based DotLocal isolation."""
    if os.name != "nt":
        return
    import winreg

    subkey = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
    views = (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY)
    for view in views:
        try:
            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                subkey,
                0,
                access=winreg.KEY_READ | view,
            ) as key:
                try:
                    value, value_type = winreg.QueryValueEx(
                        key, "DevOverrideEnable"
                    )
                except FileNotFoundError:
                    continue
        except FileNotFoundError:
            continue
        except OSError as error:
            raise RuntimeError(
                "cannot read HKLM IFEO DevOverrideEnable DotLocal policy: "
                f"{error}"
            ) from error
        if value_type != winreg.REG_DWORD:
            raise RuntimeError(
                "HKLM IFEO DevOverrideEnable has unsafe registry type; "
                "remove the value or set a REG_DWORD value of 0"
            )
        if value != 0:
            raise RuntimeError(
                "HKLM IFEO DevOverrideEnable is enabled; embedded manifests no "
                "longer disable DotLocal redirection. Set both registry views to 0."
            )


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


def _sha256(path: Path, *, stream: BinaryIO | None = None) -> str:
    digest = hashlib.sha256()
    previous = stream.tell() if stream is not None else None
    with (path.open("rb") if stream is None else nullcontext(stream)) as source:
        source.seek(0)
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    if stream is not None and previous is not None:
        stream.seek(previous)
    return digest.hexdigest()


def _describe(path: Path, *, stream: BinaryIO | None = None) -> str:
    return (
        f"{path} (version {_file_version(path)}, "
        f"sha256 {_sha256(path, stream=stream)})"
    )


def pe_imports(path: Path, *, stream: BinaryIO | None = None) -> set[str]:
    """Return normal and delay-load PE import basenames without loading the file."""

    try:
        previous = stream.tell() if stream is not None else None
        with (path.open("rb") if stream is None else nullcontext(stream)) as source:
            stream = source
            stream.seek(0)
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
                    if attributes == 1:
                        name_rva = name_value
                    elif attributes == 0:
                        if name_value < image_base:
                            raise ValueError(
                                "PE delay-import VA is below image base"
                            )
                        name_rva = name_value - image_base
                        if name_rva > 0xFFFFFFFF:
                            raise ValueError("PE delay-import VA exceeds RVA range")
                    else:
                        raise ValueError(
                            "unsupported PE delay-import descriptor attributes: "
                            f"0x{attributes:x}"
                        )
                    imports.add(c_string(name_rva).casefold())
                if not terminated:
                    raise ValueError(
                        "PE delay-import descriptor has no terminator inside declared size"
                    )
            return imports
    except OSError as error:
        raise OSError(f"cannot inspect PE imports for {path}: {error}") from error
    finally:
        if stream is not None and previous is not None:
            stream.seek(previous)


def _cmake_command_arguments(
    text: str, command_name: str
) -> tuple[tuple[str, ...], ...]:
    """Parse literal arguments from generated CMake commands."""
    commands: list[tuple[str, ...]] = []
    pattern = rf"(?i)\b{re.escape(command_name)}\s*\("
    for command_match in re.finditer(pattern, text):
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
                    raise ValueError(
                        f"unterminated quoted {command_name} argument"
                    )
                arguments.append("".join(value))
                continue
            bracket = re.match(r"\[(=*)\[", text[index:])
            if bracket:
                opening = bracket.group(0)
                closing = "]" + bracket.group(1) + "]"
                value_start = index + len(opening)
                value_end = text.find(closing, value_start)
                if value_end < 0:
                    raise ValueError(
                        f"unterminated bracket {command_name} argument"
                    )
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
            raise ValueError(f"unterminated {command_name} command")
        if index >= len(text) or text[index] != ")":
            raise ValueError(f"unterminated {command_name} command")
        commands.append(tuple(arguments))
    return tuple(commands)


def _cmake_add_test_arguments(text: str) -> tuple[tuple[str, ...], ...]:
    """Parse literal arguments from generated ``add_test(...)`` commands."""
    return _cmake_command_arguments(text, "add_test")


def _test_working_directories(text: str, testfile: Path) -> dict[str, Path]:
    working_directories: dict[str, Path] = {}

    def record(tests: tuple[str, ...], value: str) -> None:
        configured = Path(value)
        working = (
            configured
            if configured.is_absolute()
            else testfile.parent / configured
        )
        for test in tests:
            working_directories[test.casefold()] = working

    for arguments in _cmake_command_arguments(text, "set_tests_properties"):
        try:
            properties_index = next(
                index
                for index, argument in enumerate(arguments)
                if argument.casefold() == "properties"
            )
        except StopIteration:
            continue
        tests = arguments[:properties_index]
        properties = arguments[properties_index + 1 :]
        for index in range(0, len(properties) - 1, 2):
            if properties[index].casefold() != "working_directory":
                continue
            record(tests, properties[index + 1])

    for arguments in _cmake_command_arguments(text, "set_property"):
        if not arguments or arguments[0].casefold() != "test":
            continue
        try:
            property_index = next(
                index
                for index, argument in enumerate(arguments)
                if argument.casefold() == "property"
            )
        except StopIteration:
            continue
        if property_index + 2 >= len(arguments):
            continue
        if arguments[property_index + 1].casefold() != "working_directory":
            continue
        tests = tuple(
            argument
            for argument in arguments[1:property_index]
            if argument.casefold() not in ("append", "append_string")
        )
        record(tests, arguments[property_index + 2])
    return working_directories


def _test_name(arguments: tuple[str, ...]) -> str | None:
    if not arguments:
        return None
    if arguments[0].casefold() != "name":
        return arguments[0]
    return arguments[1] if len(arguments) > 1 else None


def _executable_argument(argument: str) -> str | None:
    if argument.startswith("-") and "=" in argument:
        value = argument.partition("=")[2]
        if value.casefold().endswith(".exe"):
            return value
    if argument.casefold().endswith(".exe"):
        return argument
    return None


def registered_test_executables(build_dir: Path) -> tuple[Path, ...]:
    """Return existing build-contained executable commands registered with CTest."""
    build_dir = build_dir.resolve()
    executables: set[Path] = set()
    for testfile in build_dir.rglob("CTestTestfile.cmake"):
        text = testfile.read_text(encoding="utf-8", errors="replace")
        working_directories = _test_working_directories(text, testfile)
        for arguments in _cmake_add_test_arguments(text):
            test_name = _test_name(arguments)
            base = working_directories.get(
                test_name.casefold() if test_name else "", testfile.parent
            )
            for argument in arguments:
                candidate = _executable_argument(argument)
                if candidate is None:
                    continue
                command = Path(candidate)
                lexical = command if command.is_absolute() else base / command
                lexical = Path(os.path.abspath(lexical))
                try:
                    lexical.relative_to(build_dir)
                except ValueError:
                    continue
                executable = lexical.resolve()
                try:
                    executable.relative_to(build_dir)
                except ValueError as error:
                    raise RuntimeError(
                        "CTest executable is lexically inside the build tree but "
                        f"resolves outside it: {lexical} -> {executable}"
                    ) from error
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


def _reject_runtime_dll_shadowing(
    directory: Path,
    roots: RuntimeRoots,
    *,
    executables: tuple[Path, ...] = (),
    lease: RuntimeAuditLease,
    require_embedded_manifests: bool = False,
) -> None:
    """Fail when an adjacent controlled-runtime DLL differs from the cache source."""
    if not directory.is_dir():
        return
    configured_dlls = _configured_dlls(roots)
    authoritative = _authoritative_families(roots)
    adjacent_cache: dict[Path, dict[str, Path]] = {}
    digest_cache: dict[tuple[int, int, int, int], str] = {}

    def digest(path: Path) -> str:
        stream = lease.hold(path)
        stat = os.fstat(stream.fileno())
        key = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
        cached = digest_cache.get(key)
        if cached is None:
            cached = _sha256(path, stream=stream)
            digest_cache[key] = cached
        return cached

    def describe(path: Path) -> str:
        return _describe(path, stream=lease.hold(path))

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
            lease.hold(adjacent)
            name = adjacent.name.casefold()
            found[name] = adjacent
            family = _controlled_family(name)
            if family is not None:
                candidates = authoritative.get(family, {})
                configured = candidates.get(name)
                if configured is None and not (
                    family[0] == "Qt6_DIR" and not candidates
                ):
                    raise RuntimeError(
                        "stale controlled-family DLL is not provided by active "
                        f"{family[0]}: {describe(adjacent)}; active family provides "
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
                    f"{describe(adjacent)}; configured peer: {describe(configured)}. "
                    "Remove the stale build tree or reconfigure and rebuild it."
                )
        return found

    adjacent_dlls(directory)

    resolved_executables = tuple(executable.resolve() for executable in executables)
    for executable in resolved_executables:
        redirection = Path(f"{executable}.local")
        if redirection.exists():
            raise RuntimeError(
                f"Windows {redirection} redirection is forbidden for controlled launches"
            )
        if executable.is_file():
            lease.hold(executable)
            if require_embedded_manifests:
                try:
                    embedded = has_embedded_application_manifest(executable)
                except (OSError, ValueError) as error:
                    raise RuntimeError(
                        f"cannot validate embedded RT_MANIFEST for {executable}: "
                        f"{error}. Rebuild the repository target or install a CTest "
                        "toolchain executable with a valid embedded application manifest."
                    ) from error
                if not embedded:
                    raise RuntimeError(
                        f"selected Windows executable lacks embedded RT_MANIFEST ID 1: "
                        f"{executable}. External .manifest files do not disable the "
                        "DotLocal race; rebuild the target or install a manifest-bearing "
                        "CTest toolchain."
                    )
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
        for imported_name in pe_imports(binary, stream=lease.hold(binary)):
            imported_name = imported_name.casefold()
            resolved = binary_adjacent.get(imported_name)
            if resolved is None:
                resolved = configured_dlls.get(imported_name)
            family = _controlled_family(imported_name)
            if family is not None:
                candidates = authoritative.get(family, {})
                configured = candidates.get(imported_name)
                if configured is None and not (
                    family[0] == "Qt6_DIR" and not candidates
                ):
                    raise RuntimeError(
                        f"{binary} imports stale controlled runtime {imported_name}; "
                        f"active {family[0]} family provides "
                        f"{_expected_family_text(candidates)}. Reconfigure and rebuild "
                        "the selected executable."
                    )
                if configured is None:
                    if resolved is not None:
                        pending.append((resolved, executable_dir))
                    continue
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
                        f"search path resolves incompatible peer {describe(resolved)}; "
                        f"authoritative {family[0]} peer: {describe(configured)}. "
                        "Reconfigure the dependency roots so authoritative runtime "
                        "families resolve first."
                    )
            if resolved is not None:
                pending.append((resolved, executable_dir))


def reject_runtime_dll_shadowing(
    directory: Path,
    roots: RuntimeRoots,
    *,
    executables: tuple[Path, ...] = (),
    lease: RuntimeAuditLease | None = None,
    require_embedded_manifests: bool = False,
) -> None:
    """Audit runtime provenance, optionally holding files for child lifetime."""
    owned_lease = lease is None
    active_lease = RuntimeAuditLease() if lease is None else lease
    try:
        if require_embedded_manifests:
            reject_dotlocal_dev_override()
        _reject_runtime_dll_shadowing(
            directory,
            roots,
            executables=executables,
            lease=active_lease,
            require_embedded_manifests=require_embedded_manifests,
        )
    finally:
        if owned_lease:
            active_lease.close()
