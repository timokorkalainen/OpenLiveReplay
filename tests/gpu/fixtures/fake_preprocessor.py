#!/usr/bin/env python3
"""Deterministic subprocess used by the bounded GPU capability runner tests."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


CHUNK_BYTES = 4096


def _write_chunks(stream, data: bytes) -> None:
    for offset in range(0, len(data), CHUNK_BYTES):
        stream.write(data[offset : offset + CHUNK_BYTES])
        stream.flush()


def _make_escape(path: Path) -> str:
    return path.as_posix().replace("\\", "\\\\").replace(" ", "\\ ")


def _write_dependencies(
    path: Path | None,
    family: str,
    source: Path,
    mode: str,
) -> None:
    if path is None or mode == "missing":
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if mode == "malformed":
        path.write_bytes(b'{"Data":{"Source":')
        return
    if family == "msvc":
        path.write_text(
            json.dumps({"Data": {"Source": str(source), "Includes": []}}),
            encoding="utf-8",
        )
    else:
        path.write_text(f"fixture.o: {_make_escape(source)}\n", encoding="utf-8")


def _source_from_unknown(arguments: list[str]) -> Path:
    candidates = [
        Path(value)
        for value in arguments
        if value.casefold().endswith((".c", ".cc", ".cpp", ".cxx", ".mm"))
    ]
    if len(candidates) != 1:
        raise SystemExit("fake preprocessor requires exactly one source")
    return candidates[0].resolve(strict=False)


def _marker(family: str, path: Path) -> bytes:
    escaped = str(path).replace("\\", "\\\\").replace('"', '\\"')
    if family == "msvc":
        return f'#line 1 "{escaped}"\n'.encode("utf-8")
    return f'# 1 "{escaped}" 1\n'.encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        add_help=False, allow_abbrev=False, prefix_chars="-/"
    )
    parser.add_argument(
        "--fixture-mode",
        choices=(
            "success",
            "dense",
            "fail",
            "sleep",
            "overflow",
            "malformed",
            "child-sleep",
            "child-exit",
        ),
        required=True,
    )
    parser.add_argument("--family", choices=("gcc", "msvc"), default="gcc")
    parser.add_argument("--token-count", type=int, default=16)
    parser.add_argument("--stdout-bytes", type=int, default=0)
    parser.add_argument("--stderr-bytes", type=int, default=0)
    parser.add_argument("--sleep-seconds", type=float, default=30.0)
    parser.add_argument("--child-pid-file", type=Path)
    parser.add_argument("--outside-path", type=Path)
    parser.add_argument(
        "--dependency-mode",
        choices=("valid", "missing", "malformed", "outside"),
        default="valid",
    )
    parser.add_argument("-MF", dest="gcc_dependencies", type=Path)
    parser.add_argument("/sourceDependencies", dest="msvc_dependencies", type=Path)
    options, unknown = parser.parse_known_args()
    source = _source_from_unknown(unknown)
    dependency_path = options.gcc_dependencies or options.msvc_dependencies

    dependency_source = source
    if options.dependency_mode == "outside" and options.outside_path is not None:
        dependency_source = options.outside_path.resolve(strict=False)
    _write_dependencies(
        dependency_path,
        options.family,
        dependency_source,
        options.dependency_mode,
    )

    if options.stderr_bytes:
        prefix = b"BEGIN-OF-STDERR|"
        suffix = b"|TAIL-OF-STDERR"
        middle = b"e" * max(0, options.stderr_bytes - len(prefix) - len(suffix))
        _write_chunks(sys.stderr.buffer, (prefix + middle + suffix)[: options.stderr_bytes])

    if options.fixture_mode == "fail":
        _write_chunks(sys.stdout.buffer, _marker(options.family, source) + b"partial_token\n")
        return 9
    if options.fixture_mode == "sleep":
        time.sleep(options.sleep_seconds)
        return 0
    if options.fixture_mode == "child-sleep":
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(120)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if options.child_pid_file is not None:
            options.child_pid_file.write_text(str(child.pid), encoding="ascii")
        time.sleep(options.sleep_seconds)
        return 0
    if options.fixture_mode == "child-exit":
        _write_chunks(sys.stdout.buffer, _marker(options.family, source) + b"complete\n")
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(120)"],
            stdin=subprocess.DEVNULL,
        )
        if options.child_pid_file is not None:
            options.child_pid_file.write_text(str(child.pid), encoding="ascii")
        return 0
    if options.fixture_mode == "overflow":
        remaining = options.stdout_bytes
        while remaining:
            chunk = b"x" * min(CHUNK_BYTES, remaining)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            remaining -= len(chunk)
        return 0
    if options.fixture_mode == "malformed":
        _write_chunks(sys.stdout.buffer, b'# 1 "bad\x00path" 1\ninvalid\n')
        return 0

    marker_path = source
    if options.dependency_mode == "outside" and options.outside_path is not None:
        marker_path = source
    _write_chunks(sys.stdout.buffer, _marker(options.family, marker_path))
    if options.fixture_mode == "dense":
        remaining = options.token_count
        tokens_per_chunk = CHUNK_BYTES // 2
        while remaining:
            count = min(tokens_per_chunk, remaining)
            sys.stdout.buffer.write(b"x " * count)
            sys.stdout.buffer.flush()
            remaining -= count
        _write_chunks(sys.stdout.buffer, b"\n")
    else:
        _write_chunks(sys.stdout.buffer, b"lease . nativeHandle ( ) ;\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
