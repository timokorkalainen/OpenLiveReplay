#!/usr/bin/env python3
"""Remove Qt's package-local FFmpeg backend without touching the Qt installation."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

import audit_single_ffmpeg as audit


class FilterFailure(RuntimeError):
    """Raised when the Qt FFmpeg plugin cannot be removed safely."""


@dataclass(frozen=True)
class FilterResult:
    deleted: tuple[str, ...]


PLUGIN_PATHS = {
    "windows": {"multimedia/ffmpegmediaplugin.dll"},
    "macos": {
        "Contents/PlugIns/multimedia/ffmpegmediaplugin.dylib",
        "Contents/PlugIns/multimedia/libffmpegmediaplugin.dylib",
    },
    "linux": {"usr/plugins/multimedia/libffmpegmediaplugin.so"},
}


def _windows_objdump() -> str:
    candidates: list[Path] = []
    if os.environ.get("OLR_OBJDUMP"):
        candidates.append(Path(os.environ["OLR_OBJDUMP"]))
    if os.environ.get("OLR_MINGW_ROOT"):
        candidates.append(Path(os.environ["OLR_MINGW_ROOT"]) / "bin" / "objdump.exe")
    candidates.append(Path("C:/Qt/Tools/mingw1310_64/bin/objdump.exe"))
    candidates.extend(sorted(Path("C:/Qt/Tools").glob("mingw*_64/bin/objdump.exe")))
    on_path = shutil.which("objdump")
    if on_path:
        candidates.append(Path(on_path))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise FilterFailure("cannot inspect PE dependencies: objdump.exe was not found")


def _tool_command(path: Path, platform: str) -> tuple[list[str], Callable[[str], list[audit.Dependency]]]:
    if platform == "windows":
        return [_windows_objdump(), "-p", str(path)], audit.parse_objdump_dependencies
    if platform == "macos":
        tool = shutil.which("otool")
        if tool:
            return [tool, "-L", str(path)], audit.parse_otool_dependencies
    if platform == "linux":
        tool = shutil.which("readelf")
        if tool:
            return [tool, "-d", str(path)], audit.parse_readelf_dependencies
    raise FilterFailure(f"cannot inspect {platform} dependencies: required platform tool is unavailable")


def command_dependencies(path: Path, platform: str) -> list[audit.Dependency]:
    command, parser = _tool_command(path, platform)
    try:
        completed = subprocess.run(command, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise FilterFailure(f"cannot inspect {path}: {error}") from error
    return parser(completed.stdout)


def _relative(path: Path, package: Path) -> str:
    return path.relative_to(package).as_posix()


def _plugin_path(entries: list[Path], package: Path, platform: str, metadata: list[str]) -> Path:
    expected_paths = {path.casefold() for path in PLUGIN_PATHS[platform]}
    candidates = [entry for entry in entries if _relative(entry, package).casefold() in expected_paths]
    if not candidates:
        raise FilterFailure(f"Qt FFmpeg plugin is absent for {platform}")
    verified: list[Path] = []
    for candidate in candidates:
        try:
            data = candidate.read_bytes()
        except OSError as error:
            raise FilterFailure(f"cannot inspect Qt FFmpeg plugin metadata: {_relative(candidate, package)}") from error
        if any(value.encode("utf-8") in data for value in metadata):
            verified.append(candidate)
    if not verified:
        raise FilterFailure("Qt FFmpeg plugin filename has no required metadata")
    if len(verified) != 1 or len(candidates) != 1:
        locations = ", ".join(_relative(candidate, package) for candidate in sorted(candidates))
        raise FilterFailure(f"ambiguous Qt FFmpeg plugin ownership: {locations}")
    return verified[0].resolve()


def _dependency_edges(
    binaries: list[Path],
    package: Path,
    platform: str,
    dependency_reader: Callable[[Path, str], list[audit.Dependency]],
    unresolved_controlled_allowed_from: set[Path] | None = None,
) -> dict[Path, set[Path]]:
    allowed_unresolved_parents = unresolved_controlled_allowed_from or set()
    binaries_by_name: dict[str, set[Path]] = {}
    for binary in binaries:
        binaries_by_name.setdefault(binary.name.casefold(), set()).add(binary.resolve())
    edges: dict[Path, set[Path]] = {}
    for binary in binaries:
        parent = binary.resolve()
        edges[parent] = set()
        try:
            dependencies = dependency_reader(binary, platform)
        except (FilterFailure, audit.AuditFailure) as error:
            raise FilterFailure(str(error)) from error
        for dependency in dependencies:
            rpaths = dependency.rpaths
            if dependency.name.startswith("@rpath/") and not rpaths and platform == "macos":
                rpaths = audit._macho_rpaths(binary)
            target = audit._dependency_target(audit.Dependency(dependency.name, rpaths), binary, package, platform)
            controlled = audit._controlled_component(dependency.name) is not None
            if target is None:
                if controlled and parent in allowed_unresolved_parents:
                    candidates = binaries_by_name.get(Path(dependency.name).name.casefold(), set())
                    if len(candidates) > 1:
                        locations = ", ".join(
                            sorted(_relative(candidate, package) for candidate in candidates)
                        )
                        raise FilterFailure(
                            f"ambiguous ownership for {dependency.name}: {locations}"
                        )
                    if candidates:
                        target = next(iter(candidates))
                    else:
                        continue
                elif controlled:
                    raise FilterFailure(
                        f"cannot determine ownership: {_relative(binary, package)} -> {dependency.name} is unresolved"
                    )
                else:
                    continue
            if not audit._inside(target, package):
                if controlled:
                    raise FilterFailure(
                        f"cannot determine ownership: {_relative(binary, package)} -> {dependency.name} is outside the package"
                    )
                continue
            edges[parent].add(target.resolve())
    return edges


def _closure(root: Path, edges: dict[Path, set[Path]]) -> set[Path]:
    seen: set[Path] = set()
    pending = [root]
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        pending.extend(sorted(edges.get(current, set()), key=str, reverse=True))
    return seen


def filter_package(
    *,
    package: Path,
    platform: str,
    policy_path: Path,
    dependency_reader: Callable[[Path, str], list[audit.Dependency]] = command_dependencies,
    allow_absent: bool = False,
) -> FilterResult:
    if platform not in PLUGIN_PATHS:
        raise FilterFailure(f"unsupported desktop platform: {platform}")
    root = package.resolve()
    if not root.is_dir():
        raise FilterFailure(f"package does not exist or is not a directory: {package}")
    try:
        policy = json.loads(policy_path.read_text(encoding="utf-8"))
        metadata = list(policy["forbidden_qt_ffmpeg_plugins"]["metadata"])
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise FilterFailure(f"cannot read Qt FFmpeg policy: {policy_path}") from error

    entries, errors = audit._package_entries(root)
    if errors:
        raise FilterFailure("; ".join(sorted(errors)))
    binaries = sorted(
        [entry for entry in entries if audit._is_binary(entry) and audit._inside(entry, root)],
        key=lambda entry: _relative(entry, root),
    )
    try:
        plugin = _plugin_path(entries, root, platform, metadata)
    except FilterFailure as error:
        if allow_absent and str(error).startswith("Qt FFmpeg plugin is absent"):
            return FilterResult(())
        raise
    edges = _dependency_edges(
        binaries,
        root,
        platform,
        dependency_reader,
        unresolved_controlled_allowed_from={plugin},
    )
    plugin_closure = _closure(plugin, edges)

    owners: dict[Path, set[Path]] = {}
    for parent, targets in edges.items():
        for target in targets:
            owners.setdefault(target, set()).add(parent)

    controlled_paths: dict[Path, list[Path]] = {}
    for binary in binaries:
        if audit._controlled_component(binary.name) is not None:
            controlled_paths.setdefault(binary.resolve(), []).append(binary)

    removals: list[Path] = [entry for entry in entries if entry.resolve() == plugin]
    for library, package_paths in sorted(controlled_paths.items(), key=lambda item: str(item[0])):
        if library not in plugin_closure:
            continue
        external_owners = owners.get(library, set()) - plugin_closure
        if external_owners:
            consumers = ", ".join(sorted(_relative(owner, root) for owner in external_owners))
            raise FilterFailure(
                f"ambiguous ownership for {_relative(package_paths[0], root)}: also referenced by {consumers}"
            )
        removals.extend(package_paths)

    unique_removals = sorted(set(removals), key=lambda path: _relative(path, root))
    for path in unique_removals:
        path.unlink()
    return FilterResult(tuple(_relative(path, root) for path in unique_removals))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=tuple(sorted(PLUGIN_PATHS)))
    parser.add_argument("--policy", type=Path, default=Path(__file__).with_name("single_ffmpeg_policy.json"))
    parser.add_argument("--allow-absent", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        result = filter_package(
            package=arguments.package,
            platform=arguments.platform,
            policy_path=arguments.policy,
            allow_absent=arguments.allow_absent,
        )
    except FilterFailure as error:
        print(f"FILTER FAILED: {error}", file=sys.stderr)
        return 1
    print("FILTER REMOVED: " + ", ".join(result.deleted))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
