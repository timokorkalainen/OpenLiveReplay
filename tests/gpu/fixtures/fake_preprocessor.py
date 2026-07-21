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
    dependencies: tuple[Path, ...] = (),
) -> None:
    if path is None or mode == "missing":
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if mode == "malformed":
        path.write_bytes(b'{"Data":{"Source":')
        return
    if family == "msvc":
        path.write_text(
            json.dumps({
                "Data": {
                    "Source": str(source),
                    "Includes": [str(item) for item in dependencies],
                }
            }),
            encoding="utf-8",
        )
    else:
        values = (source, *dependencies)
        path.write_text(
            "fixture.o: " + " ".join(_make_escape(item) for item in values) + "\n",
            encoding="utf-8",
        )


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
            "different-second-output",
            "different-second-byte-count",
            "wait-for-cancel",
            "mutate-restore",
            "discovery-mutate-restore",
            "swap-root-restore",
            "allocation-hold",
            "cpu-burn",
            "coordinator-crash",
        ),
        required=True,
    )
    parser.add_argument("--family", choices=("gcc", "msvc"), default="gcc")
    parser.add_argument("--token-count", type=int, default=16)
    parser.add_argument("--stdout-bytes", type=int, default=0)
    parser.add_argument("--stderr-bytes", type=int, default=0)
    parser.add_argument("--sleep-seconds", type=float, default=30.0)
    parser.add_argument("--child-pid-file", type=Path)
    parser.add_argument("--allocation-bytes", type=int, default=0)
    parser.add_argument("--allocation-ready-file", type=Path)
    parser.add_argument("--allocation-release-file", type=Path)
    parser.add_argument("--cpu-burn-seconds", type=float, default=0.0)
    parser.add_argument("--cpu-ready-directory", type=Path)
    parser.add_argument("--cpu-release-file", type=Path)
    parser.add_argument("--outside-path", type=Path)
    parser.add_argument("--invocation-counter", type=Path)
    parser.add_argument("--extra-dependency", type=Path)
    parser.add_argument(
        "--dependency-schedule",
        choices=("stable", "reorder", "add", "remove"),
        default="stable",
    )
    parser.add_argument("--mutate-path", type=Path)
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

    invocation = (
        2
        if dependency_path is not None
        and dependency_path.parent.name.startswith(".gpu-capability-accepted-")
        else 1
    )
    if options.invocation_counter is not None:
        try:
            invocation = int(options.invocation_counter.read_text(encoding="ascii")) + 1
        except FileNotFoundError:
            pass
        options.invocation_counter.write_text(str(invocation), encoding="ascii")

    dependency_source = source
    if options.dependency_mode == "outside" and options.outside_path is not None:
        dependency_source = options.outside_path.resolve(strict=False)
    dependency_extras: tuple[Path, ...] = ()
    if options.extra_dependency is not None:
        extra = options.extra_dependency.resolve(strict=False)
        if options.dependency_schedule == "reorder":
            dependency_extras = (extra,) if invocation == 1 else ()
            if invocation > 1:
                dependency_source, extra = extra, dependency_source
                dependency_extras = (extra,)
        elif options.dependency_schedule == "add":
            dependency_extras = () if invocation == 1 else (extra,)
        elif options.dependency_schedule == "remove":
            dependency_extras = (extra,) if invocation == 1 else ()
        else:
            dependency_extras = (extra,)
    _write_dependencies(
        dependency_path,
        options.family,
        dependency_source,
        options.dependency_mode,
        dependency_extras,
    )

    discovery_mutated = False
    mutation_invocation = (
        invocation == 1
        if options.fixture_mode == "discovery-mutate-restore"
        else invocation > 1
    )
    if mutation_invocation and options.fixture_mode in {
        "mutate-restore", "discovery-mutate-restore", "swap-root-restore"
    }:
        if options.mutate_path is None:
            raise SystemExit("mutation mode requires --mutate-path")
        target = options.mutate_path.resolve(strict=True)
        try:
            if options.fixture_mode in {"mutate-restore", "discovery-mutate-restore"}:
                original = target.read_bytes()
                target.write_bytes(original + b"changed")
                target.write_bytes(original)
                discovery_mutated = options.fixture_mode == "discovery-mutate-restore"
            else:
                moved = target.with_name(target.name + ".swapped")
                target.rename(moved)
                moved.rename(target)
        except OSError:
            sys.stderr.write("guard prevented generation change\n")
            return 91

    held_allocation = None
    if options.fixture_mode == "allocation-hold":
        if options.allocation_bytes <= 0 or options.allocation_bytes > (512 << 20):
            raise SystemExit("allocation-hold requires bounded --allocation-bytes")
        held_allocation = bytearray(options.allocation_bytes)
        for offset in range(0, len(held_allocation), 4096):
            held_allocation[offset] = 1
        if options.allocation_ready_file is not None:
            options.allocation_ready_file.write_text(
                str(len(held_allocation)), encoding="ascii")
        if options.allocation_release_file is not None:
            deadline = time.monotonic() + options.sleep_seconds
            while not options.allocation_release_file.exists():
                if time.monotonic() >= deadline:
                    raise SystemExit("allocation observation acknowledgement timed out")
                time.sleep(0.01)
        else:
            time.sleep(options.sleep_seconds)

    if options.fixture_mode in {"cpu-burn", "dense"} and (
        options.fixture_mode == "cpu-burn" or options.cpu_burn_seconds > 0
    ):
        if not 0.0 <= options.cpu_burn_seconds <= 5.0:
            raise SystemExit("cpu-burn requires bounded --cpu-burn-seconds")
        if options.cpu_ready_directory is None:
            ready_value = os.environ.get("OLR_CPU_READY_DIRECTORY")
            options.cpu_ready_directory = (
                None if ready_value is None else Path(ready_value)
            )
        if options.cpu_release_file is None:
            release_value = os.environ.get("OLR_CPU_RELEASE_FILE")
            options.cpu_release_file = (
                None if release_value is None else Path(release_value)
            )
        if options.cpu_ready_directory is not None:
            options.cpu_ready_directory.mkdir(parents=True, exist_ok=True)
            (options.cpu_ready_directory / f"{os.getpid()}-{invocation}.ready").write_text(
                "ready", encoding="ascii"
            )
        if options.cpu_release_file is not None:
            deadline = time.monotonic() + 10.0
            while not options.cpu_release_file.exists():
                if time.monotonic() >= deadline:
                    raise SystemExit("cpu-burn release timed out")
                time.sleep(0.005)
        finish = time.process_time() + options.cpu_burn_seconds
        accumulator = 1
        while time.process_time() < finish:
            accumulator = (accumulator * 1_103_515_245 + 12_345) & 0xFFFFFFFF
        del accumulator

    if options.fixture_mode == "coordinator-crash":
        return 86

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
    if options.fixture_mode == "wait-for-cancel":
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
        output = b"lease . nativeHandle ( ) ;\n"
        if discovery_mutated:
            output = b"lease . nativeHandle ( ) ; /* discovery mutation */\n"
        if options.fixture_mode == "different-second-output" and invocation > 1:
            output = b"Lease . nativeHandle ( ) ;\n"
        elif options.fixture_mode == "different-second-byte-count" and invocation > 1:
            output += b" "
        _write_chunks(sys.stdout.buffer, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
