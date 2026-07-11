#!/usr/bin/env python3
"""Fail-closed package and link-map audit for the controlled FFmpeg runtime."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import zipfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Sequence


CONTROLLED_COMPONENTS = ("avcodec", "avformat", "avutil", "swresample", "swscale")
UNAPPROVED_FFMPEG_COMPONENTS = ("avdevice", "avfilter", "postproc", "avresample")
FFMPEG_FAMILY_COMPONENTS = CONTROLLED_COMPONENTS + UNAPPROVED_FFMPEG_COMPONENTS
COMPONENT_PATTERN = re.compile(
    r"(?:^|[/\\])(?:lib)?(avcodec|avformat|avutil|swresample|swscale)(?:[-.](\d+)|\.so\.(\d+))",
    re.IGNORECASE,
)
COMPONENT_NAME_PATTERN = re.compile(r"(?:^|[/\\])(?:lib)?(avcodec|avformat|avutil|swresample|swscale)(?:[-.]|$)", re.IGNORECASE)
FFMPEG_SHARED_NAME_PATTERN = re.compile(
    rf"^(?:lib)?({'|'.join(FFMPEG_FAMILY_COMPONENTS)})(?:-\d+\.dll|\.dll|\.so(?:\.\d+(?:\.\d+)*)?|(?:\.\d+(?:\.\d+)*)?\.dylib)$",
    re.IGNORECASE,
)
WINDOWS_ABSOLUTE_PATTERN = re.compile(r"^[A-Za-z]:[/\\]")
MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf", b"\xbe\xba\xfe\xca", b"\xbf\xba\xfe\xca",
}
IOS_PUBLIC_HEADER_RELATIVE_PATHS = {
    component: tuple(
        Path(f"lib{component}.xcframework") / slice_name / "Headers" / f"lib{component}" / "version_major.h"
        for slice_name in ("ios-arm64", "ios-arm64-simulator")
    )
    for component in CONTROLLED_COMPONENTS
}


class AuditFailure(RuntimeError):
    """Raised when required audit input is missing."""


@dataclass(frozen=True)
class Dependency:
    name: str
    rpaths: tuple[str, ...] = ()


@dataclass(frozen=True)
class BinaryRecord:
    path: str
    parent: str | None
    resolved_path: str
    component: str | None
    abi: int | None
    sha1: str
    sha256: str
    source_prefix: str


@dataclass(frozen=True)
class LinkMapEntry:
    path: str


@dataclass
class AuditResult:
    errors: list[str] = field(default_factory=list)
    evidence: dict = field(default_factory=dict)


def parse_objdump_dependencies(text: str) -> list[Dependency]:
    return [Dependency(match.group(1).strip()) for match in re.finditer(r"^\s*DLL Name:\s*(.+?)\s*$", text, re.MULTILINE)]


def parse_dumpbin_dependencies(text: str) -> list[Dependency]:
    in_dependencies = False
    dependencies: list[Dependency] = []
    for line in text.splitlines():
        if "Image has the following dependencies" in line:
            in_dependencies = True
            continue
        if not in_dependencies:
            continue
        name = line.strip()
        if not name:
            if dependencies:
                break
            continue
        if name.startswith("Summary"):
            break
        if re.fullmatch(r"[^\s]+\.(?:dll|DLL)", name):
            dependencies.append(Dependency(name))
    return dependencies


def parse_otool_dependencies(text: str) -> list[Dependency]:
    dependencies: list[Dependency] = []
    for line in text.splitlines()[1:]:
        match = re.match(r"\s*(\S+)\s+\(", line)
        if match:
            dependencies.append(Dependency(match.group(1)))
    return dependencies


def parse_readelf_dependencies(text: str) -> list[Dependency]:
    rpaths = tuple(
        path
        for match in re.finditer(r"\((?:RPATH|RUNPATH)\).*?\[([^\]]*)\]", text)
        for path in match.group(1).split(":")
        if path
    )
    return [Dependency(match.group(1), rpaths) for match in re.finditer(r"\(NEEDED\).*?\[([^\]]+)\]", text)]


def parse_link_map(text: str) -> list[LinkMapEntry]:
    entries: list[LinkMapEntry] = []
    in_object_files = False
    for line in text.splitlines():
        if line.startswith("# Object files"):
            in_object_files = True
            continue
        if in_object_files and line.startswith("# "):
            break
        if not in_object_files:
            continue
        match = re.match(r"\s*\[\s*\d+\]\s+(.+?)\s*$", line)
        if match:
            entries.append(LinkMapEntry(match.group(1)))
    return entries


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _component_and_abi(name: str) -> tuple[str | None, int | None]:
    match = COMPONENT_PATTERN.search(name)
    if not match:
        return None, None
    return match.group(1).lower(), int(match.group(2) or match.group(3))


def _controlled_component(name: str) -> str | None:
    component, _ = _component_and_abi(name)
    if component:
        return component
    match = COMPONENT_NAME_PATTERN.search(name)
    return match.group(1).lower() if match else None


def _ffmpeg_family_component(name: str) -> str | None:
    basename = re.split(r"[/\\]", name)[-1]
    match = FFMPEG_SHARED_NAME_PATTERN.fullmatch(basename)
    return match.group(1).lower() if match else None


def _is_srt(name: str) -> bool:
    return bool(re.search(r"(?:^|[/\\])libsrt(?:[-.]|\.so|$)", name, re.IGNORECASE))


def _is_binary(path: Path) -> bool:
    suffixes = "".join(path.suffixes).lower()
    if path.suffix.lower() in {".dll", ".exe", ".dylib", ".so", ".bundle", ".a"} or ".so." in suffixes:
        return True
    component, _ = _component_and_abi(path.name)
    if component or _is_srt(path.name):
        return True
    try:
        magic = path.read_bytes()[:4]
    except OSError:
        return False
    return magic[:2] == b"MZ" or magic == b"\x7fELF" or magic in MACHO_MAGICS


def _relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except (OSError, ValueError):
        return False
    return True


def _package_entries(root: Path) -> tuple[list[Path], list[str]]:
    entries: list[Path] = []
    errors: list[str] = []

    def report_walk_error(error: OSError) -> None:
        failed_path = Path(error.filename) if error.filename else root
        try:
            location = _relative(failed_path, root)
        except ValueError:
            location = str(failed_path)
        errors.append(f"cannot inspect unreadable package entry: {location}")

    for current, directories, filenames in os.walk(root, followlinks=False, onerror=report_walk_error):
        current_path = Path(current)
        for name in sorted([*directories, *filenames]):
            path = current_path / name
            is_junction = getattr(path, "is_junction", lambda: False)()
            if path.is_symlink() or is_junction:
                try:
                    target = path.resolve(strict=True)
                except OSError as error:
                    errors.append(f"cannot inspect broken or unreadable package symlink: {_relative(path, root)}: {error}")
                    continue
                if not _inside(target, root):
                    errors.append(f"symlink escapes package: {_relative(path, root)} -> {target}")
                    continue
                if target.is_dir():
                    continue
                if not target.is_file():
                    errors.append(f"cannot inspect unreadable package entry: {_relative(path, root)}")
                    continue
                entries.append(path)
            elif path.is_file():
                entries.append(path)
    return sorted(entries), errors


def _adapter_command(path: Path, platform: str) -> tuple[list[str], Callable[[str], list[Dependency]]]:
    if platform == "windows":
        return ["objdump", "-p", str(path)], parse_objdump_dependencies
    if platform in {"macos", "ios"}:
        return ["otool", "-L", str(path)], parse_otool_dependencies
    return ["readelf", "-d", str(path)], parse_readelf_dependencies


def command_dependencies(path: Path, platform: str) -> list[Dependency]:
    command, parser = _adapter_command(path, platform)
    try:
        completed = subprocess.run(command, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError) as error:
        if platform != "windows":
            raise AuditFailure(f"cannot inspect {path}: {error}") from error
        try:
            completed = subprocess.run(["dumpbin", "/DEPENDENTS", str(path)], check=True, text=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError) as fallback_error:
            raise AuditFailure(f"cannot inspect {path}: {fallback_error}") from fallback_error
        return parse_dumpbin_dependencies(completed.stdout)
    return parser(completed.stdout)


def _existing_inside(candidate: Path, root: Path) -> Path | None:
    if not candidate.is_file() or not _inside(candidate, root):
        return None
    return candidate.resolve()


def _existing_candidate(candidate: Path) -> Path | None:
    if not candidate.is_file():
        return None
    return candidate.resolve()


def _app_executable_directory(parent: Path, root: Path) -> Path:
    for candidate in (parent, *parent.parents):
        if candidate.name == "Contents" and candidate.parent.suffix == ".app":
            return candidate / "MacOS"
    return root


def _expand_runtime_path(value: str, parent: Path, root: Path) -> Path:
    if value.startswith("@loader_path/"):
        return parent.parent / value.removeprefix("@loader_path/")
    if value.startswith("@executable_path/"):
        return _app_executable_directory(parent, root) / value.removeprefix("@executable_path/")
    if value == "$ORIGIN":
        return parent.parent
    if value.startswith("$ORIGIN/"):
        return parent.parent / value.removeprefix("$ORIGIN/")
    return Path(value)


def _dependency_target(dependency: Dependency, parent: Path, root: Path, platform: str) -> Path | None:
    name = dependency.name
    if os.path.isabs(name) or WINDOWS_ABSOLUTE_PATTERN.match(name):
        return _existing_candidate(Path(name))
    if name.startswith(("@loader_path/", "@executable_path/", "$ORIGIN/")):
        return _existing_candidate(_expand_runtime_path(name, parent, root))
    if name.startswith("@rpath/"):
        suffix = name.removeprefix("@rpath/")
        for rpath in dependency.rpaths:
            candidate = _expand_runtime_path(rpath, parent, root) / suffix
            target = _existing_candidate(candidate)
            if target:
                return target
        return None
    if platform == "linux":
        for rpath in dependency.rpaths:
            target = _existing_candidate(_expand_runtime_path(rpath, parent, root) / name)
            if target:
                return target
        return None
    if platform == "windows":
        search_paths = (root,)
    else:
        search_paths = ()
    for directory in search_paths:
        target = _existing_candidate(directory / name)
        if target:
            return target
    return None


def _source_prefix(root: Path, approved_prefix: Path | None) -> str:
    return str(approved_prefix.resolve() if approved_prefix else root.resolve())


def _record(path: Path, root: Path, parent: Path | None, approved_prefix: Path | None = None) -> BinaryRecord:
    component, abi = _component_and_abi(path.name)
    return BinaryRecord(
        path=_relative(path, root),
        parent=_relative(parent, root) if parent else None,
        resolved_path=str(path.resolve()),
        component=component,
        abi=abi,
        sha1=_sha1(path),
        sha256=_sha256(path),
        source_prefix=_source_prefix(root, approved_prefix),
    )


def _spdx(
    policy: dict,
    platform: str,
    package: Path,
    records: Sequence[BinaryRecord],
    archive_hashes: list[dict],
    final_package_hash: dict | None,
) -> dict:
    srt_external_refs = (
        [
            {
                "referenceCategory": "OTHER",
                "referenceType": "build-config",
                "referenceLocator": policy["srt"]["ios"]["build_config_id"],
            }
        ]
        if platform == "ios"
        else [
            {
                "referenceCategory": "OTHER",
                "referenceType": "git-commit",
                "referenceLocator": policy["srt"]["desktop"]["commit"],
            }
        ]
    )
    packages = [
        {
            "SPDXID": "SPDXRef-FFmpeg",
            "name": "FFmpeg",
            "versionInfo": policy["ffmpeg"]["version"],
            "downloadLocation": "https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz",
            "checksums": [{"algorithm": "SHA256", "checksumValue": policy["ffmpeg"]["sha256"]}],
        },
        {
            "SPDXID": "SPDXRef-SRT",
            "name": "SRT",
            "versionInfo": policy["srt"]["ios"]["version"] if platform == "ios" else policy["srt"]["desktop"]["version"],
            "downloadLocation": "https://github.com/Haivision/srt",
            "externalRefs": srt_external_refs,
        },
    ]
    files = [
        {
            "SPDXID": f"SPDXRef-File-{index}",
            "fileName": record.path,
            "checksums": [
                {"algorithm": "SHA1", "checksumValue": record.sha1},
                {"algorithm": "SHA256", "checksumValue": record.sha256},
            ],
        }
        for index, record in enumerate(records, start=1)
    ]
    files.extend(
        {
            "SPDXID": f"SPDXRef-Archive-{index}",
            "fileName": archive["path"],
            "checksums": [
                {"algorithm": "SHA1", "checksumValue": _sha1(Path(archive["path"]))},
                {"algorithm": "SHA256", "checksumValue": archive["sha256"]},
            ],
        }
        for index, archive in enumerate(archive_hashes, start=1)
    )
    if final_package_hash is not None:
        files.append(
            {
                "SPDXID": "SPDXRef-Final-Package",
                "fileName": final_package_hash["path"],
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": final_package_hash["sha1"]},
                    {"algorithm": "SHA256", "checksumValue": final_package_hash["sha256"]},
                ],
            }
        )
    namespace_input = json.dumps(
        {
            "package": str(package.resolve()),
            "records": [asdict(record) for record in records],
            "archives": archive_hashes,
            "final_package": final_package_hash,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    namespace = hashlib.sha256(namespace_input).hexdigest()
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "OpenLiveReplay controlled FFmpeg runtime audit",
        "documentNamespace": f"https://openlivereplay.invalid/spdx/single-ffmpeg-runtime/{namespace}",
        "creationInfo": {
            "created": "1970-01-01T00:00:00Z",
            "creators": ["Tool: audit_single_ffmpeg.py"],
        },
        "documentDescribes": ["SPDXRef-FFmpeg", "SPDXRef-SRT"],
        "packages": packages,
        "files": files,
    }


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _policy_prefixes(
    policy: dict,
    overrides: dict[str, list[Path]] | None,
    root: Path,
    platform: str,
) -> tuple[dict[str, list[Path]], list[str]]:
    configured = policy.get("controlled_prefixes", {})
    prefixes: dict[str, list[Path]] = {}
    errors: list[str] = []
    for group in ("ffmpeg", "srt"):
        values = list(configured.get(group, []))
        if overrides and group in overrides:
            values.extend(str(path) for path in overrides[group])
        prefixes[group] = []
        if platform != "ios" and not values:
            errors.append(f"missing mandatory {group.upper() if group == 'srt' else 'FFmpeg'} controlled prefix")
        for value in values:
            prefix = Path(value)
            if not prefix.is_dir():
                errors.append(f"controlled prefix does not exist: {prefix}")
                continue
            try:
                canonical_prefix = prefix.resolve(strict=True)
            except OSError as error:
                errors.append(f"cannot canonicalize controlled prefix {prefix}: {error}")
                continue
            prefixes[group].append(canonical_prefix)
            if canonical_prefix == root or _inside(canonical_prefix, root) or _inside(root, canonical_prefix):
                errors.append("controlled prefix must be a canonical build-output root outside the package")
                if canonical_prefix == root:
                    errors.append("controlled prefix must not be the package directory")
    return prefixes, errors


def _approved_prefix(path: Path, group: str, prefixes: dict[str, list[Path]]) -> tuple[Path | None, str | None]:
    available = [prefix for prefix in prefixes[group] if prefix.is_dir()]
    if not available:
        return None, None
    candidates = [candidate for prefix in available for candidate in prefix.rglob(path.name)]
    if not candidates:
        return None, "controlled binary is absent from approved build output"
    try:
        packaged_hash = _sha256(path)
    except OSError:
        return None, "cannot inspect unreadable controlled binary"
    for candidate in candidates:
        try:
            canonical_candidate = candidate.resolve(strict=True)
        except OSError:
            return None, f"cannot inspect unreadable approved build output: {candidate}"
        approved_prefix = next(
            (prefix for prefix in available if canonical_candidate.is_relative_to(prefix)),
            None,
        )
        if approved_prefix is None:
            return None, (
                "approved build output candidate escapes controlled prefix via symlink: "
                f"{candidate} -> {canonical_candidate}"
            )
        if not canonical_candidate.is_file():
            return None, f"approved build output candidate is not a regular file: {candidate}"
        try:
            candidate_hash = _sha256(canonical_candidate)
        except OSError:
            return None, f"cannot inspect unreadable approved build output: {candidate}"
        if candidate_hash == packaged_hash:
            return approved_prefix, None
    return None, "controlled binary SHA-256 differs from approved build output"


def _dependency_is_external(name: str, root: Path) -> bool:
    if not (name.startswith("/") or os.path.isabs(name) or WINDOWS_ABSOLUTE_PATTERN.match(name)):
        return False
    return not _inside(Path(name), root)


def _macho_rpaths(path: Path) -> tuple[str, ...]:
    try:
        completed = subprocess.run(["otool", "-l", str(path)], check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        return ()
    rpaths: list[str] = []
    lines = iter(completed.stdout.splitlines())
    for line in lines:
        if line.strip() != "cmd LC_RPATH":
            continue
        for detail in lines:
            match = re.match(r"\s*path\s+(.+?)\s+\(offset", detail)
            if match:
                rpaths.append(match.group(1))
                break
            if detail.strip().startswith("cmd "):
                break
    return tuple(rpaths)


def _link_map_archive_path(entry: LinkMapEntry) -> Path:
    path = entry.path
    if path.endswith(")") and "(" in path:
        path = path.rsplit("(", 1)[0]
    return Path(path)


def _ios_roots(policy: dict, policy_path: Path, package: Path) -> list[Path]:
    roots: list[Path] = []
    for value in policy["ios_approved_xcframework_roots"]:
        candidate_values = (policy_path.parent / value, policy_path.parent.parent / value, package.parent / value)
        roots.append(next((candidate.resolve() for candidate in candidate_values if candidate.exists()), candidate_values[-1].resolve()))
    return roots


def _configured_path(value: str, policy_path: Path, package: Path) -> Path:
    configured = Path(value)
    if configured.is_absolute():
        return configured.resolve()
    candidates = (policy_path.parent / configured, policy_path.parent.parent / configured, package.parent / configured)
    return next((candidate.resolve() for candidate in candidates if candidate.exists()), candidates[-1].resolve())


def _ios_controlled_build_root(policy: dict, policy_path: Path, package: Path) -> tuple[Path | None, list[str]]:
    configured = policy.get("ios_controlled_build_root")
    if not isinstance(configured, str):
        return None, ["iOS controlled build root is required"]
    root = _configured_path(configured, policy_path, package)
    if not root.is_dir():
        return None, [f"iOS controlled build root does not exist: {root}"]
    if root == package.resolve() or _inside(root, package) or _inside(package.resolve(), root):
        return None, ["iOS controlled build root must be a canonical build-output root outside the package"]
    return root, []


def _ios_manifest_path(policy: dict, policy_path: Path, package: Path, controlled_root: Path) -> tuple[Path, list[str]]:
    configuration = policy.get("ios_provenance_manifest")
    if not isinstance(configuration, dict) or not isinstance(configuration.get("path"), str):
        return controlled_root / "ffmpeg-provenance.json", ["iOS provenance manifest is required"]
    manifest_path = _configured_path(configuration["path"], policy_path, package)
    fixed_path = controlled_root / "ffmpeg-provenance.json"
    if manifest_path != fixed_path:
        return fixed_path, ["iOS provenance manifest must be fixed beneath the canonical controlled iOS build root"]
    return fixed_path, []


def _ios_provenance_manifest(
    policy: dict,
    policy_path: Path,
    package: Path,
) -> tuple[dict | None, Path | None, Path | None, list[str]]:
    controlled_root, root_errors = _ios_controlled_build_root(policy, policy_path, package)
    if controlled_root is None:
        return None, None, None, root_errors
    manifest_path, manifest_path_errors = _ios_manifest_path(policy, policy_path, package, controlled_root)
    if manifest_path_errors:
        return None, controlled_root, None, manifest_path_errors
    if not manifest_path.is_file():
        return None, controlled_root, None, ["iOS provenance manifest is required"]
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, controlled_root, None, [f"cannot read iOS provenance manifest: {manifest_path}: {error}"]
    configuration = policy["ios_provenance_manifest"]
    if manifest.get("ffmpeg_version") != policy["ffmpeg"]["version"] or manifest.get("ffmpeg_version") != configuration["ffmpeg_version"]:
        return None, controlled_root, None, ["iOS provenance manifest FFmpeg version does not match pinned FFmpeg 8.1.1 version"]
    if (
        manifest.get("build_config_stamp") != configuration["build_config_stamp"]
        or configuration["build_config_stamp"] != policy["srt"]["ios"]["build_config_id"]
    ):
        return None, controlled_root, None, ["iOS provenance manifest build-config stamp does not match pinned FFmpeg 8.1.1 stamp"]
    if not isinstance(manifest.get("archives"), dict):
        return None, controlled_root, None, ["iOS provenance manifest archive hashes are required"]
    stamp_path = controlled_root / f".{configuration['build_config_stamp']}.stamp"
    configured_stamp = configuration.get("build_config_stamp_path")
    if not isinstance(configured_stamp, str) or _configured_path(configured_stamp, policy_path, package) != stamp_path:
        return None, controlled_root, None, ["iOS build-config stamp must be fixed beneath the canonical controlled iOS build root"]
    if manifest.get("build_config_stamp_path") != str(stamp_path):
        return None, controlled_root, stamp_path, ["iOS provenance manifest build-config stamp path does not match the controlled build root"]
    try:
        stamp = stamp_path.read_text(encoding="utf-8")
    except OSError as error:
        return None, controlled_root, stamp_path, [f"cannot read iOS build-config stamp: {stamp_path}: {error}"]
    if stamp != f"{configuration['build_config_stamp']}\n":
        return None, controlled_root, stamp_path, ["iOS build-config stamp does not match pinned FFmpeg 8.1.1 stamp"]
    return manifest, controlled_root, stamp_path, []


def _ios_source_identity_errors(manifest: dict | None, policy: dict) -> list[str]:
    if manifest is None:
        return []
    identities = manifest.get("source_identities")
    if not isinstance(identities, dict):
        return ["iOS provenance manifest source identities are required"]
    ffmpeg = identities.get("ffmpeg")
    srt = identities.get("srt")
    errors: list[str] = []
    if not isinstance(ffmpeg, dict) or (
        ffmpeg.get("version") != policy["ffmpeg"]["version"]
        or ffmpeg.get("sha256") != policy["ffmpeg"]["sha256"]
    ):
        errors.append("iOS provenance manifest FFmpeg source identity does not match the locked policy")
    if not isinstance(srt, dict) or (
        srt.get("version") != policy["srt"]["ios"]["version"]
        or srt.get("commit") != policy["srt"]["ios"]["commit"]
    ):
        errors.append("iOS provenance manifest SRT source identity does not match the locked policy")
    return errors


def _manifest_hash_error(manifest: dict | None, path: Path) -> str | None:
    if manifest is None:
        return None
    record = manifest["archives"].get(str(path.resolve()))
    if not isinstance(record, dict) or not isinstance(record.get("sha256"), str):
        return f"iOS provenance manifest is missing archive hash: {path.resolve()}"
    try:
        actual_hash = _sha256(path)
    except OSError:
        return f"cannot inspect unreadable iOS provenance archive: {path.resolve()}"
    if record["sha256"] != actual_hash:
        return f"iOS provenance manifest SHA-256 does not match archive: {path.resolve()}"
    return None


def _ios_public_header_provenance(
    manifest: dict | None,
    controlled_root: Path | None,
    expected_abi: dict[str, int],
) -> tuple[dict[str, int], dict[str, str], list[str]]:
    if manifest is None or controlled_root is None:
        return {}, {}, []
    records = manifest.get("public_headers")
    if not isinstance(records, dict):
        return {}, {}, ["iOS provenance manifest public header hashes are required"]
    verified_abi: dict[str, int] = {}
    public_header_hashes: dict[str, str] = {}
    errors: list[str] = []
    for component, relative_paths in IOS_PUBLIC_HEADER_RELATIVE_PATHS.items():
        component_verified = True
        for relative_path in relative_paths:
            path = (controlled_root / relative_path).resolve()
            record = records.get(str(path))
            if not isinstance(record, dict) or not isinstance(record.get("sha256"), str):
                errors.append(f"iOS provenance manifest is missing public header hash: {path}")
                component_verified = False
                continue
            try:
                data = path.read_bytes()
            except OSError as error:
                errors.append(f"cannot inspect iOS FFmpeg public header: {path}: {error}")
                component_verified = False
                continue
            actual_hash = hashlib.sha256(data).hexdigest()
            if record["sha256"] != actual_hash:
                errors.append(f"iOS provenance manifest SHA-256 does not match public header: {path}")
                component_verified = False
                continue
            macro = f"LIB{component.upper()}_VERSION_MAJOR"
            match = re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\s+(\d+)\b", data.decode("utf-8", errors="replace"), re.MULTILINE)
            if match is None:
                errors.append(f"iOS FFmpeg public header ABI is unavailable: {component}: {path}")
                component_verified = False
                continue
            abi = int(match.group(1))
            if abi != expected_abi[component]:
                errors.append(f"iOS FFmpeg public header ABI mismatch: {component} expected {expected_abi[component]}, found {abi}: {path}")
                component_verified = False
                continue
            if record.get("abi") != abi:
                errors.append(f"iOS provenance manifest ABI does not match public header: {component}: {path}")
                component_verified = False
                continue
            public_header_hashes[str(path)] = actual_hash
        if component_verified:
            verified_abi[component] = expected_abi[component]
    return verified_abi, public_header_hashes, errors


def _audited_regular_file_hashes(package: Path) -> dict[str, str]:
    regular_files: dict[str, str] = {}
    for path in sorted(package.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        regular_files[_relative(path, package)] = _sha256(path)
    return regular_files


def _zip_member_is_symlink(member: zipfile.ZipInfo) -> bool:
    return stat.S_ISLNK(member.external_attr >> 16)


def _zip_member_kind(member: zipfile.ZipInfo) -> str:
    mode = member.external_attr >> 16
    file_type = stat.S_IFMT(mode)
    if file_type == stat.S_IFLNK:
        return "symlink"
    if file_type == stat.S_IFDIR or (file_type == 0 and member.is_dir()):
        return "directory"
    if file_type in {0, stat.S_IFREG}:
        return "regular"
    return "special"


def _zip_member_has_safe_relative_path(relative: str) -> bool:
    return bool(relative) and all(part not in {"", ".", ".."} for part in relative.split("/"))


def _zip_member_sha256(archive: zipfile.ZipFile, member: zipfile.ZipInfo) -> str:
    digest = hashlib.sha256()
    with archive.open(member) as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_ios_final_package(final_package: Path, package: Path) -> None:
    app_name = package.name.removesuffix(".app")
    expected_payload = f"Payload/{app_name}.app/"
    try:
        with zipfile.ZipFile(final_package) as archive:
            exact_app_path = expected_payload.removesuffix("/")
            exact_app_entries = [member for member in archive.infolist() if member.filename == exact_app_path]
            if exact_app_entries:
                raise AuditFailure(
                    f"iOS final package entry {exact_app_path} conflicts with required app directory"
                )
            members = [member for member in archive.infolist() if member.filename.startswith(expected_payload)]
            if not members:
                raise AuditFailure(
                    f"iOS final package does not contain the audited application {expected_payload}"
                )
            archived_files: dict[str, zipfile.ZipInfo] = {}
            duplicate_files: list[str] = []
            invalid_paths: list[str] = []
            symlinks: list[str] = []
            special_files: list[str] = []
            for member in members:
                relative = member.filename.removeprefix(expected_payload)
                if not relative:
                    if not member.filename.endswith("/") or _zip_member_kind(member) != "directory":
                        raise AuditFailure("iOS final package root app directory has an invalid entry mode")
                    if _zip_member_sha256(archive, member) != hashlib.sha256(b"").hexdigest():
                        raise AuditFailure("iOS final package root app directory must be empty")
                    continue
                kind = _zip_member_kind(member)
                if kind == "symlink":
                    symlinks.append(relative.rstrip("/"))
                    continue
                if kind == "special":
                    special_files.append(relative.rstrip("/"))
                    continue
                if kind == "directory":
                    continue
                if not _zip_member_has_safe_relative_path(relative):
                    invalid_paths.append(relative)
                    continue
                if relative in archived_files:
                    duplicate_files.append(relative)
                    continue
                archived_files[relative] = member

            if symlinks:
                raise AuditFailure(f"iOS final package contains symbolic links: {', '.join(sorted(symlinks))}")
            if special_files:
                raise AuditFailure(
                    f"iOS final package contains unsupported Unix file type: {', '.join(sorted(special_files))}"
                )
            if invalid_paths:
                raise AuditFailure(f"iOS final package contains invalid file paths: {', '.join(sorted(invalid_paths))}")
            if duplicate_files:
                raise AuditFailure(f"iOS final package contains duplicate regular files: {', '.join(sorted(duplicate_files))}")

            audited_files = _audited_regular_file_hashes(package)
            missing = sorted(set(audited_files) - set(archived_files))
            if missing:
                raise AuditFailure(f"iOS final package is missing audited regular files: {', '.join(missing)}")
            unexpected = sorted(set(archived_files) - set(audited_files))
            if unexpected:
                raise AuditFailure(f"iOS final package contains unexpected regular files: {', '.join(unexpected)}")
            mismatched = [
                relative
                for relative in sorted(audited_files)
                if audited_files[relative] != _zip_member_sha256(archive, archived_files[relative])
            ]
            if mismatched:
                raise AuditFailure(
                    f"iOS final package file SHA-256 differs from audited package: {', '.join(mismatched)}"
                )
    except (OSError, zipfile.BadZipFile) as error:
        raise AuditFailure("iOS final package must be a valid ZIP .ipa") from error


def run_audit(
    *,
    package: Path,
    platform: str,
    policy_path: Path,
    link_map_path: Path | None = None,
    archive_paths: Iterable[Path] = (),
    final_package_path: Path | None = None,
    dependency_reader: Callable[[Path, str], list[Dependency]] = command_dependencies,
    controlled_prefixes: dict[str, list[Path]] | None = None,
    evidence_path: Path | None = None,
    spdx_path: Path | None = None,
) -> AuditResult:
    root = package.resolve()
    if not root.is_dir():
        raise AuditFailure(f"package does not exist or is not a directory: {package}")
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    archives = sorted(Path(path) for path in archive_paths)
    if platform == "ios" and link_map_path is None:
        raise AuditFailure("iOS audit requires --link-map")
    if platform == "ios" and policy["srt"]["ios"]["final_package_sha256_required"] and final_package_path is None:
        raise AuditFailure("iOS audit requires a --final-package for SHA-256 evidence")
    for archive in archives:
        if not archive.is_file():
            raise AuditFailure(f"archive does not exist: {archive}")
    if final_package_path is not None and not final_package_path.is_file():
        raise AuditFailure(f"final package does not exist: {final_package_path}")
    if platform == "ios" and final_package_path is not None:
        final_package_path = final_package_path.resolve()
        if final_package_path.suffix.casefold() != ".ipa":
            raise AuditFailure("iOS final package must be a real .ipa")
        _validate_ios_final_package(final_package_path, root)
        if any(final_package_path == archive.resolve() for archive in archives):
            raise AuditFailure("iOS final package must be distinct from dependency archives")
        controlled_root, controlled_root_errors = _ios_controlled_build_root(policy, policy_path, root)
        if controlled_root is not None and _inside(final_package_path, controlled_root):
            raise AuditFailure("iOS final package must be outside the controlled build root")

    entries, errors = _package_entries(root)
    prefix_groups, prefix_errors = _policy_prefixes(policy, controlled_prefixes, root, platform)
    errors.extend(prefix_errors)
    binaries = [path for path in entries if _is_binary(path) and _inside(path, root)]

    plugin_names = [name.casefold() for name in policy["forbidden_qt_ffmpeg_plugins"]["names"]]
    plugin_metadata = policy["forbidden_qt_ffmpeg_plugins"]["metadata"]
    for entry in entries:
        relative = _relative(entry, root)
        family_component = _ffmpeg_family_component(entry.name)
        if family_component in UNAPPROVED_FFMPEG_COMPONENTS:
            errors.append(f"{relative}: unapproved FFmpeg shared component {family_component}")
        if any(name in relative.casefold() for name in plugin_names):
            errors.append(f"forbidden Qt FFmpeg plugin path: {relative}")
        try:
            data = entry.read_bytes()
        except OSError:
            errors.append(f"cannot inspect unreadable package entry: {relative}")
            continue
        for metadata in plugin_metadata:
            if metadata.encode("utf-8") in data:
                errors.append(f"forbidden Qt FFmpeg plugin metadata {metadata}: {relative}")

    records: list[BinaryRecord] = []
    expected_abi = policy["ffmpeg"]["abi"]
    seen_components: set[str] = set()
    referenced_targets: set[Path] = set()
    for binary in sorted(binaries):
        component, abi = _component_and_abi(binary.name)
        named_component = _controlled_component(binary.name)
        is_srt = _is_srt(binary.name)
        if named_component:
            if component is None:
                errors.append(f"{_relative(binary, root)}: expected ABI {expected_abi[named_component]}, found unavailable")
            else:
                seen_components.add(component)
                if abi != expected_abi[component]:
                    errors.append(f"{_relative(binary, root)}: expected ABI {expected_abi[component]}, found {abi}")
            _, provenance_error = _approved_prefix(binary, "ffmpeg", prefix_groups)
            if provenance_error:
                errors.append(f"{_relative(binary, root)}: {provenance_error}")
        elif is_srt:
            _, provenance_error = _approved_prefix(binary, "srt", prefix_groups)
            if provenance_error:
                errors.append(f"{_relative(binary, root)}: {provenance_error}")

        try:
            dependencies = dependency_reader(binary, platform)
        except AuditFailure as error:
            errors.append(str(error))
            continue
        for dependency in dependencies:
            family_component = _ffmpeg_family_component(dependency.name)
            if family_component in UNAPPROVED_FFMPEG_COMPONENTS:
                errors.append(
                    f"{_relative(binary, root)} -> {dependency.name}: "
                    f"unapproved FFmpeg shared component {family_component}"
                )
                continue
            dep_component, dep_abi = _component_and_abi(dependency.name)
            named_dependency_component = _controlled_component(dependency.name)
            controlled = named_dependency_component is not None or _is_srt(dependency.name)
            if not controlled:
                continue
            parent = _relative(binary, root)
            if named_dependency_component:
                if dep_component is None:
                    errors.append(f"{parent} -> {dependency.name}: expected ABI {expected_abi[named_dependency_component]}, found unavailable")
                elif dep_abi != expected_abi[dep_component]:
                    errors.append(f"{parent} -> {dependency.name}: expected ABI {expected_abi[dep_component]}, found {dep_abi}")
            rpaths = dependency.rpaths
            if dependency.name.startswith("@rpath/") and not rpaths and platform in {"macos", "ios"}:
                rpaths = _macho_rpaths(binary)
            target = _dependency_target(Dependency(dependency.name, rpaths), binary, root, platform)
            if target is None:
                reason = "controlled dependency resolves outside package" if _dependency_is_external(dependency.name, root) else "controlled dependency is unresolved"
                errors.append(f"{parent} -> {dependency.name}: {reason}")
                continue
            if not _inside(target, root):
                errors.append(f"{parent} -> {dependency.name}: controlled dependency resolves outside package")
                continue
            referenced_targets.add(target)
            target_component, target_abi = _component_and_abi(target.name)
            target_named_component = _controlled_component(target.name)
            if target_named_component:
                seen_components.add(target_named_component)
                if target_component is None:
                    errors.append(f"{parent} -> {dependency.name}: expected ABI {expected_abi[target_named_component]}, found unavailable")
                elif target_abi != expected_abi[target_component]:
                    errors.append(f"{parent} -> {dependency.name}: expected ABI {expected_abi[target_component]}, found {target_abi}")
                approved_prefix, provenance_error = _approved_prefix(target, "ffmpeg", prefix_groups)
            else:
                approved_prefix, provenance_error = _approved_prefix(target, "srt", prefix_groups)
            if provenance_error:
                errors.append(f"{parent} -> {dependency.name}: {provenance_error}")
            records.append(_record(target, root, binary, approved_prefix))

    for binary in sorted(binaries):
        if binary.resolve() in referenced_targets:
            continue
        if _controlled_component(binary.name) or _is_srt(binary.name):
            group = "ffmpeg" if _controlled_component(binary.name) else "srt"
            approved_prefix, _ = _approved_prefix(binary, group, prefix_groups)
            records.append(_record(binary, root, None, approved_prefix))

    if platform != "ios":
        missing = sorted(set(expected_abi) - seen_components)
        if missing:
            errors.append(f"missing controlled FFmpeg ABI components: {', '.join(missing)}")

    ios_manifest: dict | None = None
    ios_controlled_root: Path | None = None
    ios_stamp_path: Path | None = None
    verified_ios_abi: dict[str, int] = {}
    public_header_hashes: dict[str, str] = {}
    if platform == "ios":
        ios_manifest, ios_controlled_root, ios_stamp_path, manifest_errors = _ios_provenance_manifest(policy, policy_path, root)
        errors.extend(manifest_errors)
        errors.extend(_ios_source_identity_errors(ios_manifest, policy))
        if ios_manifest is not None and final_package_path is not None:
            if str(final_package_path) in ios_manifest["archives"]:
                raise AuditFailure("iOS final package must be distinct from provenance origins")
        verified_ios_abi, public_header_hashes, header_errors = _ios_public_header_provenance(
            ios_manifest,
            ios_controlled_root,
            expected_abi,
        )
        errors.extend(header_errors)

    if link_map_path is not None:
        try:
            link_map_text = link_map_path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            errors.append(f"cannot inspect unreadable link map: {link_map_path}: {error}")
            link_map_text = ""
        entries_from_map = parse_link_map(link_map_text)
        approved_roots = (
            [ios_controlled_root / Path(value).name for value in policy["ios_approved_xcframework_roots"]]
            if platform == "ios" and ios_controlled_root is not None
            else _ios_roots(policy, policy_path, root)
        )
        seen_ios_components: set[str] = set()
        ios_link_map_origins: list[dict] = []
        for entry in entries_from_map:
            origin = _link_map_archive_path(entry)
            component = _controlled_component(str(origin))
            srt = _is_srt(str(origin))
            is_forbidden_plugin = any(name in entry.path.casefold() for name in plugin_names)
            if origin.is_file():
                try:
                    archive_data = origin.read_bytes()
                except OSError:
                    errors.append(f"cannot inspect unreadable link-map origin: {origin}")
                else:
                    for metadata in plugin_metadata:
                        if metadata.encode("utf-8") in archive_data:
                            errors.append(f"forbidden Qt FFmpeg plugin metadata {metadata}: {origin}")
            if not (component or srt or is_forbidden_plugin):
                continue
            try:
                canonical = origin.resolve(strict=True)
                in_approved_root = any(canonical.is_relative_to(approved) for approved in approved_roots)
            except (OSError, ValueError):
                canonical = origin
                in_approved_root = False
            expected_root_name = "libsrt.xcframework" if srt else f"lib{component}.xcframework"
            expected_root = next((approved for approved in approved_roots if approved.name == expected_root_name), None)
            in_component_root = expected_root is not None and canonical.is_relative_to(expected_root)
            if not in_approved_root:
                errors.append(f"link map origin outside approved XCFramework roots: {entry.path}")
            if (component or srt) and (not origin.is_file() or not in_component_root):
                errors.append(f"link map origin is not a canonical existing XCFramework file: {entry.path}")
            if component and origin.is_file() and in_component_root:
                seen_ios_components.add(component)
            if srt and origin.is_file() and in_component_root:
                seen_ios_components.add("libsrt")
            if (component or srt) and origin.is_file() and in_component_root:
                provenance_error = _manifest_hash_error(ios_manifest, canonical)
                if provenance_error:
                    errors.append(provenance_error)
                controlled_component = "libsrt" if srt else component
                ios_link_map_origins.append(
                    {
                        "component": controlled_component,
                        "parent": str(link_map_path.resolve()),
                        "path": entry.path,
                        "expected_abi": expected_abi.get(controlled_component),
                        "abi": verified_ios_abi.get(controlled_component),
                        "source_prefix": str(expected_root),
                        "resolved_path": str(canonical),
                        "sha1": _sha1(canonical),
                        "sha256": _sha256(canonical),
                    }
                )
            if is_forbidden_plugin:
                errors.append(f"forbidden Qt FFmpeg plugin link-map origin: {entry.path}")
        if platform == "ios":
            missing_ios = sorted((set(expected_abi) | {"libsrt"}) - seen_ios_components)
            if missing_ios:
                errors.append(f"missing iOS controlled XCFramework components: {', '.join(missing_ios)}")

    if platform == "ios":
        for archive in archives:
            provenance_error = _manifest_hash_error(ios_manifest, archive)
            if provenance_error:
                errors.append(provenance_error)
    archive_hashes = [{"path": str(path), "sha256": _sha256(path)} for path in archives]
    final_package_hash = (
        {
            "path": str(final_package_path),
            "sha1": _sha1(final_package_path),
            "sha256": _sha256(final_package_path),
        }
        if final_package_path is not None
        else None
    )
    unique_records = {tuple(asdict(record).items()): record for record in records}
    ordered_records = sorted(unique_records.values(), key=lambda record: (record.path, record.parent or "", record.resolved_path))
    evidence = {
        "archive_hashes": archive_hashes,
        "final_package_hash": final_package_hash,
        "controlled_binaries": [asdict(record) for record in ordered_records],
        "errors": sorted(set(errors)),
        "platform": platform,
        "policy": {
            "ffmpeg_sha256": policy["ffmpeg"]["sha256"],
            "ffmpeg_version": policy["ffmpeg"]["version"],
            "srt_desktop_commit": policy["srt"]["desktop"]["commit"],
            "srt_desktop_version": policy["srt"]["desktop"]["version"],
            "srt_ios_build_config_id": policy["srt"]["ios"]["build_config_id"],
            "srt_ios_commit": policy["srt"]["ios"]["commit"],
            "srt_ios_version": policy["srt"]["ios"]["version"],
        },
    }
    if link_map_path is not None:
        evidence["ios_link_map_origins"] = sorted(ios_link_map_origins, key=lambda origin: (origin["component"], origin["resolved_path"]))
    if platform == "ios" and ios_controlled_root is not None and ios_manifest is not None and ios_stamp_path is not None:
        evidence["ios_provenance"] = {
            "controlled_build_root": str(ios_controlled_root),
            "manifest_path": str(ios_controlled_root / "ffmpeg-provenance.json"),
            "manifest_sha256": _sha256(ios_controlled_root / "ffmpeg-provenance.json"),
            "build_config_stamp_path": str(ios_stamp_path),
            "build_config_stamp_sha256": _sha256(ios_stamp_path),
            "verified_abi": verified_ios_abi,
            "public_header_hashes": public_header_hashes,
            "source_identities": ios_manifest["source_identities"],
        }
    result = AuditResult(errors=evidence["errors"], evidence=evidence)
    if evidence_path:
        _write_json(evidence_path, evidence)
    if spdx_path:
        _write_json(spdx_path, _spdx(policy, platform, root, ordered_records, archive_hashes, final_package_hash))
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "macos", "linux", "ios"))
    parser.add_argument("--policy", type=Path, default=Path(__file__).with_name("single_ffmpeg_policy.json"))
    parser.add_argument("--link-map", type=Path)
    parser.add_argument("--archive", type=Path, action="append", default=[])
    parser.add_argument("--final-package", type=Path)
    parser.add_argument("--controlled-prefix", action="append", default=[], metavar="GROUP=PATH")
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--spdx", type=Path, required=True)
    arguments = parser.parse_args(argv)
    controlled_prefixes: dict[str, list[Path]] = {"ffmpeg": [], "srt": []}
    for value in arguments.controlled_prefix:
        group, separator, prefix = value.partition("=")
        if separator != "=" or group not in controlled_prefixes or not prefix:
            parser.error("--controlled-prefix must be ffmpeg=PATH or srt=PATH")
        controlled_prefixes[group].append(Path(prefix))
    try:
        result = run_audit(
            package=arguments.package,
            platform=arguments.platform,
            policy_path=arguments.policy,
            link_map_path=arguments.link_map,
            archive_paths=arguments.archive,
            final_package_path=arguments.final_package,
            controlled_prefixes=controlled_prefixes,
            evidence_path=arguments.evidence,
            spdx_path=arguments.spdx,
        )
    except AuditFailure as error:
        print(f"AUDIT FAILED: {error}", file=sys.stderr)
        return 2
    if result.errors:
        for error in result.errors:
            print(f"AUDIT FAILED: {error}", file=sys.stderr)
        return 1
    print(f"AUDIT PASSED: {arguments.package} ({arguments.platform})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
