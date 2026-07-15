#!/usr/bin/env python3
"""Reject build-tree DLLs that shadow the configured Windows runtime."""

from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
from typing import NamedTuple


class RuntimeRoots(NamedTuple):
    compiler_bin: Path
    qt_bin: Path
    ffmpeg_bin: Path
    ffmpeg_lib: Path
    srt_bin: Path
    srt_lib: Path


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


def _configured_dlls(roots: RuntimeRoots) -> dict[str, Path]:
    """Index configured DLLs in the same precedence order as launcher PATH."""
    configured: dict[str, Path] = {}
    for directory in roots:
        children = sorted(directory.iterdir(), key=lambda path: path.name.casefold())
        for child in children:
            if child.is_file() and child.suffix.casefold() == ".dll":
                configured.setdefault(child.name.casefold(), child)
    return configured


def reject_runtime_dll_shadowing(directory: Path, roots: RuntimeRoots) -> None:
    """Fail when an adjacent controlled-runtime DLL differs from the cache source."""
    if not directory.is_dir():
        return
    configured_dlls = _configured_dlls(roots)
    for adjacent in sorted(directory.iterdir(), key=lambda path: path.name.casefold()):
        if not adjacent.is_file() or adjacent.suffix.casefold() != ".dll":
            continue
        configured = configured_dlls.get(adjacent.name.casefold())
        if configured is None:
            continue
        if _sha256(adjacent) != _sha256(configured):
            raise RuntimeError(
                "incompatible runtime DLL shadows the configured Windows runtime: "
                f"{_describe(adjacent)}; configured peer: {_describe(configured)}. "
                "Remove the stale build tree or reconfigure and rebuild it."
            )
