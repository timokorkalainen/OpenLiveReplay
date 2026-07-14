#!/usr/bin/env python3
"""Contain inherited Windows error-dialog behavior around child creation."""

from __future__ import annotations

from contextlib import contextmanager
import ctypes
from collections.abc import Iterator


WINDOWS_CHILD_ERROR_MODE = 0x8003


def _set_error_mode(mode: int) -> int:
    return int(ctypes.windll.kernel32.SetErrorMode(mode))


@contextmanager
def suppress_windows_error_dialogs(enabled: bool) -> Iterator[None]:
    """Set the inherited no-dialog error mode while a Windows child is created."""
    if not enabled:
        yield
        return

    previous = _set_error_mode(WINDOWS_CHILD_ERROR_MODE)
    try:
        yield
    finally:
        _set_error_mode(previous)
