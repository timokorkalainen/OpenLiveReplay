#!/usr/bin/env python3
"""Pure-parser and policy coverage for the single-FFmpeg package audit."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
BUILD_SCRIPTS = ROOT / "build-scripts"

import sys

sys.path.insert(0, str(BUILD_SCRIPTS))

import audit_single_ffmpeg as audit  # noqa: E402
from audit_single_ffmpeg import (  # noqa: E402
    AuditFailure,
    Dependency,
    parse_dumpbin_dependencies,
    parse_link_map,
    parse_objdump_dependencies,
    parse_otool_dependencies,
    parse_readelf_dependencies,
    run_audit,
)


FFMPEG_COMPONENTS = {
    "avcodec": 62,
    "avformat": 62,
    "avutil": 60,
    "swresample": 6,
    "swscale": 9,
}

IOS_XCFRAMEWORK_ROOTS = [
    "ios_build/xcframeworks/libavcodec.xcframework",
    "ios_build/xcframeworks/libavformat.xcframework",
    "ios_build/xcframeworks/libavutil.xcframework",
    "ios_build/xcframeworks/libswresample.xcframework",
    "ios_build/xcframeworks/libswscale.xcframework",
    "ios_build/xcframeworks/libsrt.xcframework",
]

IOS_BUILD_CONFIG_STAMP = "ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter"

IOS_PUBLIC_HEADERS = {
    "avcodec": "include/libavcodec/version_major.h",
    "avformat": "include/libavformat/version_major.h",
    "avutil": "include/libavutil/version_major.h",
    "swresample": "include/libswresample/version_major.h",
    "swscale": "include/libswscale/version_major.h",
}

IOS_XCFRAMEWORK_HEADERS = {
    component: (
        f"lib{component}.xcframework/ios-arm64/Headers/lib{component}/version_major.h",
        f"lib{component}.xcframework/ios-arm64-simulator/Headers/lib{component}/version_major.h",
    )
    for component in FFMPEG_COMPONENTS
}


class TemporaryPackage(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name) / "OpenLiveReplay"
        self.root.mkdir()

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def binary(self, relative: str, payload: bytes = b"MZ") -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return path

    def ipa(
        self,
        name: str = "OpenLiveReplay",
        *,
        app_name: str | None = None,
        directory: Path | None = None,
        omit: set[str] | None = None,
        replacements: dict[str, bytes] | None = None,
        unexpected: dict[str, bytes] | None = None,
        symlinks: dict[str, str] | None = None,
    ) -> Path:
        path = (directory or self.root.parent) / f"{name}.ipa"
        payload_app = app_name or self.root.name
        omitted = omit or set()
        replacements = replacements or {}
        unexpected = unexpected or {}
        symlinks = symlinks or {}
        with zipfile.ZipFile(path, "w") as archive:
            payload_root = f"Payload/{payload_app}.app/"
            archive.writestr(payload_root, b"")
            for source in sorted(self.root.rglob("*")):
                if not source.is_file() or source.is_symlink():
                    continue
                relative = source.relative_to(self.root).as_posix()
                if relative not in omitted:
                    archive.writestr(payload_root + relative, replacements.get(relative, source.read_bytes()))
            for relative, content in unexpected.items():
                archive.writestr(payload_root + relative, content)
            for relative, target in symlinks.items():
                entry = zipfile.ZipInfo(payload_root + relative)
                entry.create_system = 3
                entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(entry, target.encode("utf-8"))
        return path

    def policy(self) -> Path:
        return BUILD_SCRIPTS / "single_ffmpeg_policy.json"

    def policy_with(self, mutate) -> Path:
        policy = json.loads(self.policy().read_text(encoding="utf-8"))
        mutate(policy)
        path = self.root.parent / "single-ffmpeg-policy.json"
        path.write_text(json.dumps(policy), encoding="utf-8")
        return path

    def ios_policy(self) -> Path:
        return self.policy_with(
            lambda policy: policy.update({"ios_approved_xcframework_roots": IOS_XCFRAMEWORK_ROOTS})
        )

    def ios_link_map(
        self,
        *,
        components: tuple[str, ...] = tuple(FFMPEG_COMPONENTS),
        include_srt: bool = True,
    ) -> tuple[Path, list[Path]]:
        object_files: list[Path] = []
        for component in components:
            archive = self.root.parent / "ios_build" / "xcframeworks" / f"lib{component}.xcframework" / "ios-arm64" / f"lib{component}.a"
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(f"{component}-archive".encode("ascii"))
            object_files.append(archive)
        if include_srt:
            archive = self.root.parent / "ios_build" / "xcframeworks" / "libsrt.xcframework" / "ios-arm64" / "libsrt.a"
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(b"srt-archive")
            object_files.append(archive)

        link_map = self.root.parent / "OpenLiveReplay-LinkMap-normal-arm64.txt"
        entries = "".join(f"[ {index:2}] {path}(object.o)\n" for index, path in enumerate(object_files, start=1))
        link_map.write_text(f"# Object files:\n{entries}# Sections:\n", encoding="utf-8")
        final_archive = self.ipa()
        return link_map, [*object_files, final_archive]

    def ios_provenance_manifest(self, archives: list[Path], *, build_config_stamp: str | None = None) -> Path:
        manifest = self.root.parent / "ios_build" / "xcframeworks" / "ffmpeg-provenance.json"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(
            json.dumps(
                {
                    "ffmpeg_version": "8.1.1",
                    "build_config_stamp": build_config_stamp
                    or "ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter",
                    "archives": {
                        str(path.resolve()): {
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        }
                        for path in archives[:-1]
                    },
                }
            ),
            encoding="utf-8",
        )
        return manifest

    def controlled_ios_audit_fixture(
        self,
        *,
        header_abi: dict[str, int] | None = None,
    ) -> tuple[Path, Path, list[Path], Path, Path, Path, dict[Path, int]]:
        controlled_root = self.root.parent / "ios_build" / "xcframeworks"
        stamp = controlled_root / ".ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp"
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(IOS_BUILD_CONFIG_STAMP + "\n", encoding="utf-8")

        archives: list[Path] = []
        for component in FFMPEG_COMPONENTS:
            archive = controlled_root / f"lib{component}.xcframework" / "ios-arm64" / f"lib{component}.a"
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(f"controlled-{component}-archive".encode("ascii"))
            archives.append(archive)
        srt_archive = controlled_root / "libsrt.xcframework" / "ios-arm64" / "libsrt.a"
        srt_archive.parent.mkdir(parents=True, exist_ok=True)
        srt_archive.write_bytes(b"controlled-srt-archive")
        archives.append(srt_archive)

        header_abi = header_abi or FFMPEG_COMPONENTS
        headers: dict[Path, int] = {}
        for component, relatives in IOS_XCFRAMEWORK_HEADERS.items():
            for relative in relatives:
                header = controlled_root / relative
                header.parent.mkdir(parents=True, exist_ok=True)
                macro = f"LIB{component.upper()}_VERSION_MAJOR"
                header.write_text(f"#define {macro} {header_abi[component]}\n", encoding="utf-8")
                headers[header] = header_abi[component]

        link_map = self.root.parent / "OpenLiveReplay-LinkMap-normal-arm64.txt"
        entries = "".join(f"[ {index:2}] {path}(object.o)\n" for index, path in enumerate(archives, start=1))
        link_map.write_text(f"# Object files:\n{entries}# Sections:\n", encoding="utf-8")
        final_archive = self.ipa()
        all_archives = [*archives, final_archive]

        manifest = controlled_root / "ffmpeg-provenance.json"
        manifest.write_text(
            json.dumps(
                {
                    "ffmpeg_version": "8.1.1",
                    "build_config_stamp": IOS_BUILD_CONFIG_STAMP,
                    "build_config_stamp_path": str(stamp.resolve()),
                    "archives": {
                        str(path.resolve()): {"sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                        for path in archives
                    },
                    "source_identities": {
                        "ffmpeg": {
                            "sha256": "b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3",
                            "version": "8.1.1",
                        },
                        "srt": {
                            "commit": "52ceecdf5190885914f0f94d01be32441ccb1f4c",
                            "version": "1.5.5-rc.0",
                        },
                    },
                    "public_headers": {
                        str(path.resolve()): {
                            "abi": abi,
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        }
                        for path, abi in headers.items()
                    },
                }
            ),
            encoding="utf-8",
        )
        policy = self.policy_with(
            lambda payload: payload.update(
                {
                    "ios_controlled_build_root": str(controlled_root),
                    "ios_approved_xcframework_roots": [
                        f"ios_build/xcframeworks/{Path(root).name}" for root in IOS_XCFRAMEWORK_ROOTS
                    ],
                    "ios_provenance_manifest": {
                        "path": "ios_build/xcframeworks/ffmpeg-provenance.json",
                        "ffmpeg_version": "8.1.1",
                        "build_config_stamp": IOS_BUILD_CONFIG_STAMP,
                        "build_config_stamp_path": "ios_build/xcframeworks/.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp",
                    },
                }
            )
        )
        return policy, link_map, all_archives, controlled_root, manifest, stamp, headers

    def windows_package(self) -> tuple[Path, dict[Path, list[Dependency]]]:
        app = self.binary("OpenLiveReplay.exe")
        dependencies: dict[Path, list[Dependency]] = {
            app: [Dependency("avcodec-62.dll"), Dependency("avformat-62.dll")]
        }
        for component, major in FFMPEG_COMPONENTS.items():
            library = self.binary(f"{component}-{major}.dll")
            dependencies[library] = []
        srt = self.binary("libsrt.dll")
        dependencies[srt] = []
        return app, dependencies


class ParserTests(unittest.TestCase):
    def test_dependency_adapters_keep_names_and_locations(self) -> None:
        self.assertEqual(
            parse_objdump_dependencies("DLL Name: avcodec-62.dll\nDLL Name: KERNEL32.dll\n"),
            [Dependency("avcodec-62.dll"), Dependency("KERNEL32.dll")],
        )
        self.assertEqual(
            parse_dumpbin_dependencies("Image has the following dependencies:\n\n    avformat-62.dll\n    libsrt.dll\n\n"),
            [Dependency("avformat-62.dll"), Dependency("libsrt.dll")],
        )
        self.assertEqual(
            parse_otool_dependencies(
                "App:\n\t@rpath/libavutil.60.dylib (compatibility version 60.0.0, current version 60.1.0)\n"
            ),
            [Dependency("@rpath/libavutil.60.dylib")],
        )
        self.assertEqual(
            parse_readelf_dependencies(
                " 0x0000000000000001 (NEEDED)             Shared library: [libswresample.so.6]\n"
            ),
            [Dependency("libswresample.so.6")],
        )

    def test_link_map_parser_extracts_object_origins(self) -> None:
        link_map = """# Object files:\n[  1] /project/ios_build/xcframeworks/ffmpeg/libavcodec.a(codec.o)\n[  2] /Qt/plugins/mediaservice/libffmpegmediaplugin.a(plugin.o)\n# Sections:\n"""
        self.assertEqual(
            [entry.path for entry in parse_link_map(link_map)],
            [
                "/project/ios_build/xcframeworks/ffmpeg/libavcodec.a(codec.o)",
                "/Qt/plugins/mediaservice/libffmpegmediaplugin.a(plugin.o)",
            ],
        )

    def test_link_map_parser_retains_archive_member_identity(self) -> None:
        entry = parse_link_map(
            "# Object files:\n[  1] /Qt/lib/libQt6Multimedia.a(qffmpegmediaplugin.o)\n# Sections:\n"
        )[0]
        self.assertIn("qffmpegmediaplugin.o", entry.path)


class PolicyAndAuditTests(TemporaryPackage):
    def test_policy_locks_existing_builder_versions_and_abi(self) -> None:
        policy = json.loads(self.policy().read_text(encoding="utf-8"))
        self.assertEqual(policy["ffmpeg"]["version"], "8.1.1")
        self.assertEqual(
            policy["ffmpeg"]["sha256"],
            "b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3",
        )
        self.assertEqual(policy["ffmpeg"]["abi"], FFMPEG_COMPONENTS)
        self.assertEqual(policy["srt"]["desktop"]["version"], "1.5.4")
        self.assertEqual(policy["srt"]["desktop"]["commit"], "a8c6b65520f814c5bd8f801be48c33ceece7c4a6")
        self.assertEqual(policy["srt"]["ios"]["version"], "1.5.5-rc.0")
        self.assertEqual(policy["srt"]["ios"]["commit"], "52ceecdf5190885914f0f94d01be32441ccb1f4c")
        self.assertEqual(
            policy["srt"]["ios"]["build_config_id"],
            "ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter",
        )
        self.assertTrue(policy["srt"]["ios"]["final_package_sha256_required"])
        self.assertNotIn("source_sha256", policy["srt"]["ios"])

    def test_valid_windows_package_emits_deterministic_evidence_and_spdx(self) -> None:
        _, dependencies = self.windows_package()
        ffmpeg_prefix = self.root.parent / "approved-ffmpeg"
        srt_prefix = self.root.parent / "approved-srt"
        ffmpeg_prefix.mkdir()
        srt_prefix.mkdir()
        for component, major in FFMPEG_COMPONENTS.items():
            (ffmpeg_prefix / f"{component}-{major}.dll").write_bytes(
                (self.root / f"{component}-{major}.dll").read_bytes()
            )
        (srt_prefix / "libsrt.dll").write_bytes((self.root / "libsrt.dll").read_bytes())
        evidence = self.root.parent / "evidence.json"
        spdx = self.root.parent / "evidence.spdx.json"
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: dependencies.get(path, []),
            controlled_prefixes={"ffmpeg": [ffmpeg_prefix], "srt": [srt_prefix]},
            evidence_path=evidence,
            spdx_path=spdx,
        )
        self.assertEqual(result.errors, [])
        payload = json.loads(evidence.read_text(encoding="utf-8"))
        self.assertEqual(payload["policy"]["ffmpeg_version"], "8.1.1")
        self.assertEqual(
            [entry["abi"] for entry in payload["controlled_binaries"] if entry["component"] == "avcodec"],
            [62],
        )
        self.assertEqual(payload["controlled_binaries"], sorted(payload["controlled_binaries"], key=lambda item: item["path"]))
        spdx_payload = json.loads(spdx.read_text(encoding="utf-8"))
        self.assertEqual(spdx_payload["spdxVersion"], "SPDX-2.3")
        self.assertEqual(spdx_payload["creationInfo"]["created"], "1970-01-01T00:00:00Z")
        self.assertEqual(spdx_payload["documentDescribes"], ["SPDXRef-FFmpeg", "SPDXRef-SRT"])

    def test_rejects_current_old_windows_abi_and_qt_ffmpeg_plugin(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        old_libraries = [
            self.binary("bin/avcodec-61.dll"),
            self.binary("bin/avformat-61.dll"),
            self.binary("bin/avutil-59.dll"),
            self.binary("bin/swresample-5.dll"),
            self.binary("bin/swscale-8.dll"),
        ]
        plugin = self.binary("plugins/multimedia/ffmpegmediaplugin.dll", b"MZQFFmpegMediaPlugin")
        dependencies = {app: [Dependency(path.name) for path in old_libraries]}
        dependencies.update({path: [] for path in old_libraries})
        dependencies[plugin] = []
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: dependencies.get(path, []),
        )
        message = "\n".join(result.errors)
        self.assertIn("OpenLiveReplay.exe -> avcodec-61.dll", message)
        self.assertIn("expected ABI 62, found 61", message)
        self.assertIn("plugins/multimedia/ffmpegmediaplugin.dll", message)
        self.assertIn("QFFmpegMediaPlugin", message)

    def test_rejects_unresolved_controlled_dependency_and_junction_escape(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        outside = self.root.parent / "outside"
        outside.mkdir()
        junction = self.root / "bin"
        completed = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(junction), str(outside)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("avformat-62.dll")] if path == app else [],
        )
        message = "\n".join(result.errors)
        self.assertIn("symlink escapes package: bin", message)
        self.assertIn("OpenLiveReplay.exe -> avformat-62.dll: controlled dependency is unresolved", message)

    def test_rejects_controlled_dependency_outside_package(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        result = run_audit(
            package=self.root,
            platform="linux",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("/usr/lib/libavcodec.so.62")] if path == app else [],
        )
        self.assertIn(
            "OpenLiveReplay.exe -> /usr/lib/libavcodec.so.62: controlled dependency resolves outside package",
            result.errors,
        )

    def test_ios_requires_archive_hash_and_rejects_unapproved_link_map_origin(self) -> None:
        archive = self.root.parent / "dependency.a"
        archive.write_bytes(b"archive")
        final_package = self.ipa()
        link_map = self.root.parent / "OpenLiveReplay-LinkMap-normal-arm64.txt"
        link_map.write_text(
            "# Object files:\n"
            "[  1] /project/ios_build/xcframeworks/ffmpeg/libavcodec.a(codec.o)\n"
            "[  2] /Qt/plugins/mediaservice/libffmpegmediaplugin.a(plugin.o)\n",
            encoding="utf-8",
        )
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.policy(),
            link_map_path=link_map,
            archive_paths=[archive],
            final_package_path=final_package,
        )
        message = "\n".join(result.errors)
        self.assertIn("link map origin outside approved XCFramework roots", message)
        self.assertIn("libffmpegmediaplugin.a", message)
        self.assertEqual(result.evidence["archive_hashes"][0]["sha256"], hashlib.sha256(b"archive").hexdigest())

    def test_ios_fails_closed_without_link_map_or_final_archive(self) -> None:
        with self.assertRaises(AuditFailure) as raised:
            run_audit(package=self.root, platform="ios", policy_path=self.policy())
        self.assertIn("iOS audit requires --link-map", str(raised.exception))

    def test_policy_uses_real_per_component_ios_xcframework_roots(self) -> None:
        policy = json.loads(self.policy().read_text(encoding="utf-8"))
        self.assertEqual(policy["ios_approved_xcframework_roots"], IOS_XCFRAMEWORK_ROOTS)

    def test_ios_rejects_spoofed_and_nonexistent_xcframework_origins(self) -> None:
        policy = self.ios_policy()
        nonexistent = self.root.parent / "ios_build" / "xcframeworks" / "libavcodec.xcframework" / "ios-arm64" / "libavcodec.a"
        link_map = self.root.parent / "OpenLiveReplay-LinkMap-normal-arm64.txt"
        link_map.write_text(
            "# Object files:\n"
            "[  1] /spoof/ios_build/xcframeworks/libavcodec.xcframework/ios-arm64/libavcodec.a(codec.o)\n"
            f"[  2] {nonexistent}(codec.o)\n"
            "# Sections:\n",
            encoding="utf-8",
        )
        final_archive = self.ipa()
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=[],
            final_package_path=final_archive,
        )
        message = "\n".join(result.errors)
        self.assertIn("link map origin is not a canonical existing XCFramework file", message)

    def test_ios_requires_every_component_and_srt_archive_with_hash_evidence(self) -> None:
        link_map, archives = self.ios_link_map(components=("avcodec",), include_srt=False)
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.ios_policy(),
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertEqual(
            result.evidence["archive_hashes"],
            [
                {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for path in sorted(archives[:-1])
            ],
        )
        self.assertIn("missing iOS controlled XCFramework components: avformat, avutil, libsrt, swresample, swscale", result.errors)

    def test_ios_requires_final_package_separately_from_dependency_archives(self) -> None:
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        with self.assertRaisesRegex(AuditFailure, "final-package"):
            run_audit(
                package=self.root,
                platform="ios",
                policy_path=policy,
                link_map_path=link_map,
                archive_paths=archives[:-1],
            )

    def test_ios_final_package_must_be_a_distinct_ipa_outside_the_controlled_root(self) -> None:
        policy, link_map, archives, controlled_root, _, _, _ = self.controlled_ios_audit_fixture()
        final_package = archives[-1]
        not_an_ipa = self.root.parent / "OpenLiveReplay.zip"
        not_an_ipa.write_bytes(b"not-an-ipa")
        controlled_ipa = self.ipa(directory=controlled_root)

        for final, dependency_archives, diagnostic in (
            (not_an_ipa, archives[:-1], "iOS final package must be a real .ipa"),
            (final_package, [*archives[:-1], final_package], "iOS final package must be distinct from dependency archives"),
            (controlled_ipa, archives[:-1], "iOS final package must be outside the controlled build root"),
        ):
            with self.subTest(final=final):
                with self.assertRaisesRegex(AuditFailure, diagnostic):
                    run_audit(
                        package=self.root,
                        platform="ios",
                        policy_path=policy,
                        link_map_path=link_map,
                        archive_paths=dependency_archives,
                        final_package_path=final,
                    )

    def test_ios_final_package_must_be_a_matching_ipa_zip(self) -> None:
        self.binary("Info.plist", b"audited-plist")
        self.binary("Resources/asset.dat", b"audited-content")
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        valid = self.ipa("OpenLiveReplay-valid")
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=valid,
        )
        self.assertEqual(result.errors, [])

        plain_ipa = self.root.parent / "OpenLiveReplay-plain.ipa"
        plain_ipa.write_bytes(b"not-a-zip")
        unrelated_ipa = self.ipa("OpenLiveReplay-unrelated", app_name="OtherApplication")

        for final_package, diagnostic in (
            (plain_ipa, "iOS final package must be a valid ZIP .ipa"),
            (unrelated_ipa, "iOS final package does not contain the audited application Payload/OpenLiveReplay.app/"),
            (
                self.ipa(
                    "OpenLiveReplay-mismatched",
                    replacements={"Info.plist": b"other-plist", "Resources/asset.dat": b"other-content"},
                ),
                "iOS final package file SHA-256 differs from audited package: Info.plist, Resources/asset.dat",
            ),
            (
                self.ipa("OpenLiveReplay-missing", omit={"Resources/asset.dat"}),
                "iOS final package is missing audited regular files: Resources/asset.dat",
            ),
            (
                self.ipa("OpenLiveReplay-unexpected", unexpected={"Resources/unexpected.dat": b"unexpected"}),
                "iOS final package contains unexpected regular files: Resources/unexpected.dat",
            ),
            (
                self.ipa(
                    "OpenLiveReplay-symlink",
                    omit={"Resources/asset.dat"},
                    symlinks={"Resources/asset.dat": "Info.plist"},
                ),
                "iOS final package contains symbolic links: Resources/asset.dat",
            ),
        ):
            with self.subTest(final_package=final_package):
                with self.assertRaisesRegex(AuditFailure, diagnostic):
                    run_audit(
                        package=self.root,
                        platform="ios",
                        policy_path=policy,
                        link_map_path=link_map,
                        archive_paths=archives[:-1],
                        final_package_path=final_package,
                    )

    def test_ios_final_package_rejects_special_unix_modes_even_with_matching_bytes(self) -> None:
        self.binary("Info.plist", b"audited-plist")
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        modes = {
            "fifo": stat.S_IFIFO,
            "character-device": stat.S_IFCHR,
            "socket": stat.S_IFSOCK,
        }
        for label, file_type in modes.items():
            final_package = self.root.parent / f"OpenLiveReplay-{label}.ipa"
            with zipfile.ZipFile(final_package, "w") as archive:
                archive.writestr("Payload/OpenLiveReplay.app/", b"")
                entry = zipfile.ZipInfo("Payload/OpenLiveReplay.app/Info.plist")
                entry.create_system = 3
                entry.external_attr = (file_type | 0o644) << 16
                archive.writestr(entry, b"audited-plist")
            with self.subTest(file_type=label):
                with self.assertRaisesRegex(AuditFailure, "unsupported Unix file type"):
                    run_audit(
                        package=self.root,
                        platform="ios",
                        policy_path=policy,
                        link_map_path=link_map,
                        archive_paths=archives[:-1],
                        final_package_path=final_package,
                    )

    def test_ios_final_package_rejects_exact_app_entry_conflicting_with_directory(self) -> None:
        self.binary("Info.plist", b"audited-plist")
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        for label, file_type in (("directory", stat.S_IFDIR), ("regular", 0), ("fifo", stat.S_IFIFO)):
            final_package = self.root.parent / f"OpenLiveReplay-exact-{label}.ipa"
            with zipfile.ZipFile(final_package, "w") as archive:
                entry = zipfile.ZipInfo("Payload/OpenLiveReplay.app")
                entry.create_system = 3
                entry.external_attr = (file_type | 0o644) << 16
                archive.writestr(entry, b"conflicting-entry")
                archive.writestr("Payload/OpenLiveReplay.app/", b"")
                archive.writestr("Payload/OpenLiveReplay.app/Info.plist", b"audited-plist")
            with self.subTest(entry_type=label):
                with self.assertRaisesRegex(AuditFailure, "conflicts with required app directory"):
                    run_audit(
                        package=self.root,
                        platform="ios",
                        policy_path=policy,
                        link_map_path=link_map,
                        archive_paths=archives[:-1],
                        final_package_path=final_package,
                    )

    def test_ios_final_package_requires_a_clean_trailing_slash_app_directory_entry(self) -> None:
        self.binary("Info.plist", b"audited-plist")
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        cases = {
            "directory-with-data": (stat.S_IFDIR, b"unexpected-data"),
            "regular": (stat.S_IFREG, b""),
            "fifo": (stat.S_IFIFO, b""),
            "portable-zero-type-with-data": (0, b"unexpected-data"),
        }
        for label, (file_type, data) in cases.items():
            final_package = self.root.parent / f"OpenLiveReplay-root-{label}.ipa"
            with zipfile.ZipFile(final_package, "w") as archive:
                entry = zipfile.ZipInfo("Payload/OpenLiveReplay.app/")
                entry.create_system = 3
                entry.external_attr = (file_type | 0o755) << 16
                archive.writestr(entry, data)
                archive.writestr("Payload/OpenLiveReplay.app/Info.plist", b"audited-plist")
            with self.subTest(entry_type=label):
                with self.assertRaisesRegex(AuditFailure, "root app directory"):
                    run_audit(
                        package=self.root,
                        platform="ios",
                        policy_path=policy,
                        link_map_path=link_map,
                        archive_paths=archives[:-1],
                        final_package_path=final_package,
                    )

    def test_ios_final_package_must_not_be_a_provenance_origin(self) -> None:
        policy, link_map, archives, _, manifest, _, _ = self.controlled_ios_audit_fixture()
        final_package = archives[-1]
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        payload["archives"][str(final_package.resolve())] = {
            "sha256": hashlib.sha256(final_package.read_bytes()).hexdigest(),
        }
        manifest.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(AuditFailure, "iOS final package must be distinct from provenance origins"):
            run_audit(
                package=self.root,
                platform="ios",
                policy_path=policy,
                link_map_path=link_map,
                archive_paths=archives[:-1],
                final_package_path=final_package,
            )

    def test_policy_defaults_match_real_ios_builder_provenance_layout(self) -> None:
        policy = json.loads(self.policy().read_text(encoding="utf-8"))
        self.assertEqual(policy["ios_controlled_build_root"], "ios_build/xcframeworks")
        self.assertEqual(policy["ios_provenance_manifest"]["path"], "ios_build/xcframeworks/ffmpeg-provenance.json")
        self.assertEqual(
            policy["ios_provenance_manifest"]["build_config_stamp_path"],
            "ios_build/xcframeworks/.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp",
        )

    def test_real_ios_xcframework_headers_and_manifest_defaults_verify(self) -> None:
        policy, link_map, archives, controlled_root, manifest, stamp, headers = self.controlled_ios_audit_fixture()
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertEqual(result.errors, [])
        self.assertEqual(manifest, controlled_root / "ffmpeg-provenance.json")
        self.assertEqual(stamp, controlled_root / ".ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp")
        self.assertTrue(all("/Headers/lib" in path.as_posix() for path in headers))

    def test_windows_bare_dll_resolution_uses_only_the_package_root(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        self.binary("bin/avcodec-62.dll")
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("avcodec-62.dll")] if path == app else [],
        )
        self.assertIn(
            "OpenLiveReplay.exe -> avcodec-62.dll: controlled dependency is unresolved",
            result.errors,
        )

    def test_framework_binary_uses_the_app_bundle_executable_directory(self) -> None:
        framework = self.binary("OpenLiveReplay.app/Contents/Frameworks/Helper.dylib", b"\xfe\xed\xfa\xcf")
        self.binary("OpenLiveReplay.app/Contents/Frameworks/libavcodec.62.dylib", b"\xfe\xed\xfa\xcf")
        result = run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("@executable_path/../Frameworks/libavcodec.62.dylib")]
            if path == framework
            else [],
        )
        self.assertNotIn(
            "OpenLiveReplay.app/Contents/Frameworks/Helper.dylib -> @executable_path/../Frameworks/libavcodec.62.dylib: controlled dependency is unresolved",
            result.errors,
        )

    def test_macos_framework_directory_and_binary_symlinks_are_scanned_safely(self) -> None:
        framework = self.root / "OpenLiveReplay.app/Contents/Frameworks/Codec.framework"
        version = framework / "Versions/A"
        version.mkdir(parents=True)
        binary = version / "Codec"
        binary.write_bytes(b"\xfe\xed\xfa\xcfpayload")
        try:
            os.symlink("A", framework / "Versions/Current", target_is_directory=True)
            os.symlink("Versions/Current/Codec", framework / "Codec")
        except OSError as error:
            self.skipTest(f"host cannot create framework symlinks: {error}")
        inspected: list[Path] = []
        result = run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: inspected.append(path) or [],
        )
        self.assertFalse(any("Versions/Current" in error for error in result.errors), result.errors)
        self.assertIn(binary, inspected)
        self.assertIn(framework / "Codec", inspected)

    def test_controlled_prefix_rejects_matching_symlink_file_escape(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        packaged = self.binary("avcodec-62.dll", b"matching")
        approved = self.root.parent / "approved-prefix"
        approved.mkdir()
        escaped = self.root.parent / "outside-prefix" / packaged.name
        escaped.parent.mkdir()
        escaped.write_bytes(packaged.read_bytes())
        try:
            os.symlink(escaped, approved / packaged.name)
        except OSError as error:
            self.skipTest(f"host cannot create file symlinks: {error}")
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency(packaged.name)] if path == app else [],
            controlled_prefixes={"ffmpeg": [approved], "srt": [approved]},
        )
        self.assertIn(
            "approved build output candidate escapes controlled prefix via symlink",
            "\n".join(result.errors),
        )

    def test_rejects_unapproved_ffmpeg_family_components_in_package_and_dependencies(self) -> None:
        forms = {
            "windows": ("OpenLiveReplay.exe", "avdevice-62.dll"),
            "linux": ("usr/bin/OpenLiveReplay", "libavfilter.so.11"),
            "macos": ("OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay", "libpostproc.59.dylib"),
        }
        for platform, (app_name, extra_name) in forms.items():
            with self.subTest(platform=platform):
                package = self.root / platform
                package.mkdir()
                app = package / app_name
                app.parent.mkdir(parents=True, exist_ok=True)
                magic = b"MZ" if platform == "windows" else (b"\x7fELF" if platform == "linux" else b"\xfe\xed\xfa\xcf")
                app.write_bytes(magic)
                extra = package / extra_name
                extra.write_bytes(magic + b"extra")
                result = run_audit(
                    package=package,
                    platform=platform,
                    policy_path=self.policy(),
                    dependency_reader=lambda path, _platform, app=app, extra_name=extra_name: [Dependency(extra_name)] if path == app else [],
                    controlled_prefixes={"ffmpeg": [self.root.parent], "srt": [self.root.parent]},
                )
                self.assertIn("unapproved FFmpeg shared component", "\n".join(result.errors))
        legacy = self.binary("avresample-4.dll", b"MZlegacy")
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda *_: [],
            controlled_prefixes={"ffmpeg": [self.root.parent], "srt": [self.root.parent]},
        )
        self.assertIn(f"{legacy.name}: unapproved FFmpeg shared component avresample", result.errors)

    def test_ios_link_map_evidence_does_not_copy_expected_abi_when_header_mismatches(self) -> None:
        header_abi = {**FFMPEG_COMPONENTS, "avcodec": 61}
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture(header_abi=header_abi)
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        avcodec_origin = next(origin for origin in result.evidence["ios_link_map_origins"] if origin["component"] == "avcodec")
        self.assertEqual(avcodec_origin["expected_abi"], 62)
        self.assertIsNone(avcodec_origin["abi"])

    def test_rejects_nonexistent_tokenized_and_internal_absolute_controlled_targets(self) -> None:
        app = self.binary("MacOS/OpenLiveReplay", b"\xfe\xed\xfa\xcf")
        missing_absolute = self.root / "Frameworks" / "libavutil.60.dylib"
        dependencies = [
            Dependency("@loader_path/../Frameworks/libavcodec.62.dylib"),
            Dependency("$ORIGIN/../lib/libavformat.so.62"),
            Dependency(str(missing_absolute)),
        ]
        result = run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: dependencies if path == app else [],
        )
        self.assertIn(
            "MacOS/OpenLiveReplay -> @loader_path/../Frameworks/libavcodec.62.dylib: controlled dependency is unresolved",
            result.errors,
        )
        self.assertIn(
            "MacOS/OpenLiveReplay -> $ORIGIN/../lib/libavformat.so.62: controlled dependency is unresolved",
            result.errors,
        )
        self.assertIn(
            f"MacOS/OpenLiveReplay -> {missing_absolute}: controlled dependency is unresolved",
            result.errors,
        )

    def test_linux_resolves_controlled_dependency_from_bare_origin_rpath(self) -> None:
        app = self.binary("usr/bin/OpenLiveReplay", b"\x7fELF")
        library = self.binary("usr/bin/libavcodec.so.62", b"\x7fELF")

        target = audit._dependency_target(
            Dependency("libavcodec.so.62", ("$ORIGIN",)),
            app,
            self.root,
            "linux",
        )

        self.assertEqual(target, library.resolve())

    def test_requires_declared_rpath_instead_of_unique_basename_fallback(self) -> None:
        app = self.binary("MacOS/OpenLiveReplay", b"\xfe\xed\xfa\xcf")
        self.binary("Frameworks/libavcodec.62.dylib", b"\xfe\xed\xfa\xcf")
        result = run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("@rpath/libavcodec.62.dylib")] if path == app else [],
        )
        self.assertIn(
            "MacOS/OpenLiveReplay -> @rpath/libavcodec.62.dylib: controlled dependency is unresolved",
            result.errors,
        )

    def test_dependency_evidence_contains_parent_edge_record(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        library = self.binary("avcodec-62.dll")
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("avcodec-62.dll")] if path == app else [],
        )
        record = next(entry for entry in result.evidence["controlled_binaries"] if entry["path"] == "avcodec-62.dll")
        self.assertEqual(record["parent"], "OpenLiveReplay.exe")
        self.assertEqual(record["resolved_path"], str(library.resolve()))

    def test_desktop_rejects_missing_and_hash_mismatched_controlled_prefix_inputs(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        self.binary("bin/avcodec-62.dll", b"packaged")
        approved = self.root.parent / "approved-build-output"
        approved.mkdir()
        (approved / "avcodec-62.dll").write_bytes(b"approved")
        missing = self.root.parent / "missing-build-output"
        policy = self.policy_with(
            lambda payload: payload.update(
                {
                    "controlled_prefixes": {
                        "ffmpeg": [str(approved), str(missing)],
                        "srt": [],
                    }
                }
            )
        )
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=policy,
            dependency_reader=lambda path, _platform: [Dependency("avcodec-62.dll")] if path == app else [],
        )
        message = "\n".join(result.errors)
        self.assertIn("controlled prefix does not exist", message)
        self.assertIn("controlled binary SHA-256 differs from approved build output", message)

    def test_rejects_qffmpeg_archive_member_in_link_map(self) -> None:
        link_map = self.root.parent / "OpenLiveReplay-LinkMap-normal-arm64.txt"
        link_map.write_text(
            "# Object files:\n[  1] /Qt/lib/libQt6Multimedia.a(qffmpegmediaplugin.o)\n# Sections:\n",
            encoding="utf-8",
        )
        final_archive = self.ipa()
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.ios_policy(),
            link_map_path=link_map,
            archive_paths=[],
            final_package_path=final_archive,
        )
        self.assertIn("forbidden Qt FFmpeg plugin link-map origin: /Qt/lib/libQt6Multimedia.a(qffmpegmediaplugin.o)", result.errors)

    def test_ios_adapter_uses_otool_and_otool_parser(self) -> None:
        binary = self.binary("OpenLiveReplay", b"\xfe\xed\xfa\xcf")
        commands: list[list[str]] = []

        def run(command, **_kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=f"{binary}:\n\t@rpath/libavcodec.62.dylib (compatibility version 62.0.0, current version 62.1.0)\n",
            )

        with patch.object(audit.subprocess, "run", side_effect=run):
            dependencies = audit.command_dependencies(binary, "ios")

        self.assertEqual(commands, [["otool", "-L", str(binary)]])
        self.assertEqual(dependencies, [Dependency("@rpath/libavcodec.62.dylib")])

    def test_recognizes_all_fat_macho_magic_values_as_binaries(self) -> None:
        magics = (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf", b"\xbe\xba\xfe\xca", b"\xbf\xba\xfe\xca")
        binaries = [self.binary(f"fat-{index}", magic) for index, magic in enumerate(magics)]
        inspected: list[Path] = []
        run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: inspected.append(path) or [],
        )
        self.assertEqual(inspected, binaries)

    def test_rejects_unversioned_controlled_dependencies_in_relative_and_system_paths(self) -> None:
        app = self.binary("OpenLiveReplay.exe")
        dependencies = [
            Dependency("libavcodec.dylib"),
            Dependency("avcodec.dll"),
            Dependency("/usr/lib/libavcodec.dylib"),
            Dependency(r"C:\\Windows\\System32\\avcodec.dll"),
        ]
        result = run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: dependencies if path == app else [],
        )
        message = "\n".join(result.errors)
        for dependency in dependencies:
            self.assertIn(f"OpenLiveReplay.exe -> {dependency.name}: expected ABI 62, found unavailable", message)

    def test_spdx_file_entries_include_sha1_and_sha256(self) -> None:
        _, dependencies = self.windows_package()
        spdx = self.root.parent / "evidence.spdx.json"
        run_audit(
            package=self.root,
            platform="windows",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: dependencies.get(path, []),
            spdx_path=spdx,
        )
        payload = json.loads(spdx.read_text(encoding="utf-8"))
        algorithms = {checksum["algorithm"] for entry in payload["files"] for checksum in entry["checksums"]}
        self.assertEqual(algorithms, {"SHA1", "SHA256"})

    def test_ios_spdx_includes_final_ipa_sha1_and_sha256(self) -> None:
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        spdx = self.root.parent / "ios-evidence.spdx.json"
        final_package = archives[-1]
        run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=final_package,
            spdx_path=spdx,
        )
        payload = json.loads(spdx.read_text(encoding="utf-8"))
        final_file = next(entry for entry in payload["files"] if entry["fileName"] == str(final_package))
        self.assertEqual(final_file["SPDXID"], "SPDXRef-Final-Package")
        self.assertEqual(
            final_file["checksums"],
            [
                {"algorithm": "SHA1", "checksumValue": hashlib.sha1(final_package.read_bytes()).hexdigest()},
                {"algorithm": "SHA256", "checksumValue": hashlib.sha256(final_package.read_bytes()).hexdigest()},
            ],
        )

    def test_ios_audit_verifies_pinned_source_identities_in_the_provenance_manifest(self) -> None:
        policy, link_map, archives, _, manifest, _, _ = self.controlled_ios_audit_fixture()
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        payload["source_identities"]["ffmpeg"]["sha256"] = "0" * 64
        payload["source_identities"]["srt"]["commit"] = "0" * 40
        manifest.write_text(json.dumps(payload), encoding="utf-8")

        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertIn("iOS provenance manifest FFmpeg source identity does not match the locked policy", result.errors)
        self.assertIn("iOS provenance manifest SRT source identity does not match the locked policy", result.errors)

    def test_spdx_namespace_is_package_unique_and_deterministic(self) -> None:
        first = self.root.parent / "first.spdx.json"
        repeat = self.root.parent / "repeat.spdx.json"
        second = self.root.parent / "second.spdx.json"
        run_audit(package=self.root, platform="windows", policy_path=self.policy(), dependency_reader=lambda *_: [], spdx_path=first)
        run_audit(package=self.root, platform="windows", policy_path=self.policy(), dependency_reader=lambda *_: [], spdx_path=repeat)
        other_package = self.root.parent / "OtherOpenLiveReplay"
        other_package.mkdir()
        run_audit(package=other_package, platform="windows", policy_path=self.policy(), dependency_reader=lambda *_: [], spdx_path=second)
        first_namespace = json.loads(first.read_text(encoding="utf-8"))["documentNamespace"]
        repeat_namespace = json.loads(repeat.read_text(encoding="utf-8"))["documentNamespace"]
        second_namespace = json.loads(second.read_text(encoding="utf-8"))["documentNamespace"]
        self.assertEqual(first_namespace, repeat_namespace)
        self.assertNotEqual(first_namespace, second_namespace)

    def test_ios_spdx_reports_ios_srt_version(self) -> None:
        link_map, archives = self.ios_link_map()
        spdx = self.root.parent / "ios-evidence.spdx.json"
        run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.ios_policy(),
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
            spdx_path=spdx,
        )
        payload = json.loads(spdx.read_text(encoding="utf-8"))
        srt = next(package for package in payload["packages"] if package["SPDXID"] == "SPDXRef-SRT")
        self.assertEqual(srt["versionInfo"], "1.5.5-rc.0")

    def test_desktop_requires_nonpackage_ffmpeg_and_srt_prefixes_before_hash_acceptance(self) -> None:
        missing_prefix_errors = {}
        package_prefix_errors = {}
        policy = self.policy_with(
            lambda payload: payload.update(
                {
                    "controlled_prefixes": {
                        "ffmpeg": [str(self.root)],
                        "srt": [str(self.root)],
                    }
                }
            )
        )
        for platform in ("windows", "macos", "linux"):
            missing_prefix_errors[platform] = run_audit(
                package=self.root,
                platform=platform,
                policy_path=self.policy(),
                dependency_reader=lambda *_: [],
            ).errors
            package_prefix_errors[platform] = run_audit(
                package=self.root,
                platform=platform,
                policy_path=policy,
                dependency_reader=lambda *_: [],
            ).errors

        self.assertTrue(
            all(
                "missing mandatory FFmpeg controlled prefix" in errors
                and "missing mandatory SRT controlled prefix" in errors
                for errors in missing_prefix_errors.values()
            ),
            missing_prefix_errors,
        )
        self.assertTrue(
            all("controlled prefix must not be the package directory" in errors for errors in package_prefix_errors.values()),
            package_prefix_errors,
        )

    def test_ios_rejects_archives_without_matching_pinned_provenance_manifest_and_stamp(self) -> None:
        link_map, archives = self.ios_link_map()
        policy = self.ios_policy()
        payload = json.loads(policy.read_text(encoding="utf-8"))
        payload["ios_provenance_manifest"] = {
            "path": str(self.root.parent / "ios_build" / "xcframeworks" / "ffmpeg-provenance.json"),
            "ffmpeg_version": "8.1.1",
            "build_config_stamp": "ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter",
            "build_config_stamp_path": str(
                self.root.parent / "ios_build" / "xcframeworks" / ".ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp"
            ),
        }
        policy.write_text(json.dumps(payload), encoding="utf-8")

        without_manifest = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertIn("iOS provenance manifest is required", without_manifest.errors)

        self.ios_provenance_manifest(archives, build_config_stamp="untrusted-build-stamp")
        mismatched_manifest = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertIn(
            "iOS provenance manifest build-config stamp does not match pinned FFmpeg 8.1.1 stamp",
            mismatched_manifest.errors,
        )

    def test_ios_rejects_qt_multimedia_archive_bytes_with_ffmpeg_plugin_marker(self) -> None:
        link_map, archives = self.ios_link_map()
        qt_archive = self.root.parent / "Qt" / "lib" / "libQt6Multimedia.a"
        qt_archive.parent.mkdir(parents=True, exist_ok=True)
        qt_archive.write_bytes(b"static archive: QFFmpegMediaPlugin")
        link_map.write_text(
            link_map.read_text(encoding="utf-8").replace(
                "# Sections:", f"[ 99] {qt_archive}(plugin.o)\n# Sections:"
            ),
            encoding="utf-8",
        )

        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.ios_policy(),
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertIn(
            f"forbidden Qt FFmpeg plugin metadata QFFmpegMediaPlugin: {qt_archive}",
            result.errors,
        )

    def test_linux_bare_controlled_soname_requires_existing_origin_runpath(self) -> None:
        app = self.binary("OpenLiveReplay", b"\x7fELF")
        self.binary("lib/libavcodec.so.62", b"\x7fELF")

        without_runpath = run_audit(
            package=self.root,
            platform="linux",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("libavcodec.so.62")] if path == app else [],
        )
        self.assertIn(
            "OpenLiveReplay -> libavcodec.so.62: controlled dependency is unresolved",
            without_runpath.errors,
        )

        with_runpath = run_audit(
            package=self.root,
            platform="linux",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("libavcodec.so.62", rpaths=("$ORIGIN/lib",))]
            if path == app
            else [],
        )
        self.assertNotIn(
            "OpenLiveReplay -> libavcodec.so.62: controlled dependency is unresolved",
            with_runpath.errors,
        )

    def test_ios_link_map_evidence_contains_complete_controlled_edge_fields(self) -> None:
        link_map, archives = self.ios_link_map()
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.ios_policy(),
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        required = {"parent", "expected_abi", "abi", "source_prefix", "resolved_path", "sha1", "sha256"}
        self.assertTrue(
            all(required <= entry.keys() for entry in result.evidence["ios_link_map_origins"]),
            result.evidence["ios_link_map_origins"],
        )

    def test_unreadable_relevant_package_archive_fails_closed(self) -> None:
        archive = self.binary("plugins/libQt6Multimedia.a", b"static archive")
        original_read_bytes = Path.read_bytes

        def read_bytes(path: Path) -> bytes:
            if path == archive:
                raise PermissionError("injected unreadable archive")
            return original_read_bytes(path)

        with patch.object(Path, "read_bytes", read_bytes):
            result = run_audit(
                package=self.root,
                platform="windows",
                policy_path=self.policy(),
                dependency_reader=lambda *_: [],
            )
        self.assertIn("cannot inspect unreadable package entry: plugins/libQt6Multimedia.a", result.errors)

    def test_ios_spdx_uses_pinned_ios_srt_provenance_without_desktop_commit(self) -> None:
        link_map, archives = self.ios_link_map()
        spdx = self.root.parent / "ios-evidence.spdx.json"
        run_audit(
            package=self.root,
            platform="ios",
            policy_path=self.ios_policy(),
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
            spdx_path=spdx,
        )
        srt = next(
            package
            for package in json.loads(spdx.read_text(encoding="utf-8"))["packages"]
            if package["SPDXID"] == "SPDXRef-SRT"
        )
        self.assertEqual(
            srt["externalRefs"],
            [
                {
                    "referenceCategory": "OTHER",
                    "referenceType": "build-config",
                    "referenceLocator": "ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter",
                }
            ],
        )

    def test_rejects_controlled_prefixes_equal_to_containing_or_inside_the_package(self) -> None:
        nested_prefix = self.root / "approved-build-output"
        nested_prefix.mkdir()
        diagnostics: dict[str, list[str]] = {}
        for name, prefix in {
            "equal": self.root,
            "containing": self.root.parent,
            "inside": nested_prefix,
        }.items():
            policy = self.policy_with(
                lambda payload, prefix=prefix: payload.update(
                    {"controlled_prefixes": {"ffmpeg": [str(prefix)], "srt": [str(prefix)]}}
                )
            )
            diagnostics[name] = run_audit(
                package=self.root,
                platform="windows",
                policy_path=policy,
                dependency_reader=lambda *_: [],
            ).errors

        required = "controlled prefix must be a canonical build-output root outside the package"
        self.assertTrue(
            all(any(required in error for error in errors) for errors in diagnostics.values()),
            diagnostics,
        )

    def test_ios_rejects_caller_manifest_and_unverified_public_header_provenance(self) -> None:
        policy, link_map, archives, controlled_root, manifest, stamp, headers = self.controlled_ios_audit_fixture()
        caller_manifest = self.root.parent / "caller-input" / "ffmpeg-provenance.json"
        caller_manifest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(manifest, caller_manifest)

        policy_payload = json.loads(policy.read_text(encoding="utf-8"))
        policy_payload["ios_provenance_manifest"]["path"] = str(caller_manifest)
        policy.write_text(json.dumps(policy_payload), encoding="utf-8")
        caller_manifest_errors = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        ).errors

        policy_payload["ios_provenance_manifest"]["path"] = "ios_build/xcframeworks/ffmpeg-provenance.json"
        policy.write_text(json.dumps(policy_payload), encoding="utf-8")
        manifest_payload = json.loads(manifest.read_text(encoding="utf-8"))
        header = next(iter(headers))
        manifest_payload["public_headers"][str(header.resolve())]["sha256"] = "0" * 64
        manifest.write_text(json.dumps(manifest_payload), encoding="utf-8")
        header_hash_errors = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        ).errors

        manifest_payload = json.loads(manifest.read_text(encoding="utf-8"))
        header.write_text("#define LIBAVCODEC_VERSION_MAJOR 61\n", encoding="utf-8")
        manifest_payload["public_headers"][str(header.resolve())]["sha256"] = hashlib.sha256(header.read_bytes()).hexdigest()
        manifest.write_text(json.dumps(manifest_payload), encoding="utf-8")
        header_abi_errors = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        ).errors

        self.assertTrue(
            all(
                expected in errors
                for expected, errors in (
                    (
                        "iOS provenance manifest must be fixed beneath the canonical controlled iOS build root",
                        caller_manifest_errors,
                    ),
                    (
                        f"iOS provenance manifest SHA-256 does not match public header: {header.resolve()}",
                        header_hash_errors,
                    ),
                    (
                        f"iOS FFmpeg public header ABI mismatch: avcodec expected 62, found 61: {header.resolve()}",
                        header_abi_errors,
                    ),
                )
            )
        )
        self.assertEqual(stamp.read_text(encoding="utf-8"), IOS_BUILD_CONFIG_STAMP + "\n")
        self.assertTrue(all(path.is_relative_to(controlled_root) for path in headers))

    def test_ios_scans_direct_qt_multimedia_object_metadata(self) -> None:
        policy, link_map, archives, _, _, _, _ = self.controlled_ios_audit_fixture()
        qt_object = self.root.parent / "Qt" / "lib" / "qtmultimedia_backend.o"
        qt_object.parent.mkdir(parents=True, exist_ok=True)
        qt_object.write_bytes(b"object metadata: QFFmpegMediaPlugin")
        link_map.write_text(
            link_map.read_text(encoding="utf-8").replace(
                "# Sections:", f"[ 99] {qt_object}\n# Sections:"
            ),
            encoding="utf-8",
        )

        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertIn(
            f"forbidden Qt FFmpeg plugin metadata QFFmpegMediaPlugin: {qt_object}",
            result.errors,
        )

    def test_rejects_first_existing_external_loader_candidate_before_internal_copy(self) -> None:
        external = self.root.parent / "external-loader-path"
        external.mkdir()
        mac_app = self.binary("OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay", b"\xfe\xed\xfa\xcf")
        self.binary("OpenLiveReplay.app/Contents/Frameworks/libavcodec.62.dylib", b"\xfe\xed\xfa\xcf")
        (external / "libavcodec.62.dylib").write_bytes(b"\xfe\xed\xfa\xcf")
        mac_errors = run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [
                Dependency("@rpath/libavcodec.62.dylib", (str(external), "@loader_path/../Frameworks"))
            ]
            if path == mac_app
            else [],
        ).errors

        linux_app = self.binary("OpenLiveReplay", b"\x7fELF")
        self.binary("lib/libavcodec.so.62", b"\x7fELF")
        (external / "libavcodec.so.62").write_bytes(b"\x7fELF")
        linux_errors = run_audit(
            package=self.root,
            platform="linux",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [
                Dependency("libavcodec.so.62", (str(external), "$ORIGIN/lib"))
            ]
            if path == linux_app
            else [],
        ).errors

        self.assertTrue(
            all(
                expected in errors
                for expected, errors in (
                    (
                        "OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay -> @rpath/libavcodec.62.dylib: controlled dependency resolves outside package",
                        mac_errors,
                    ),
                    (
                        "OpenLiveReplay -> libavcodec.so.62: controlled dependency resolves outside package",
                        linux_errors,
                    ),
                )
            )
        )

    def test_resolves_executable_path_from_the_app_executable_directory(self) -> None:
        app = self.binary("OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay", b"\xfe\xed\xfa\xcf")
        self.binary("OpenLiveReplay.app/Contents/Frameworks/libavcodec.62.dylib", b"\xfe\xed\xfa\xcf")
        result = run_audit(
            package=self.root,
            platform="macos",
            policy_path=self.policy(),
            dependency_reader=lambda path, _platform: [Dependency("@executable_path/../Frameworks/libavcodec.62.dylib")]
            if path == app
            else [],
        )
        self.assertNotIn(
            "OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay -> @executable_path/../Frameworks/libavcodec.62.dylib: controlled dependency is unresolved",
            result.errors,
        )

    def test_ios_valid_controlled_provenance_emits_verified_abi_and_hash_evidence(self) -> None:
        policy, link_map, archives, controlled_root, manifest, stamp, headers = self.controlled_ios_audit_fixture()
        result = run_audit(
            package=self.root,
            platform="ios",
            policy_path=policy,
            link_map_path=link_map,
            archive_paths=archives[:-1],
            final_package_path=archives[-1],
        )
        self.assertEqual(result.errors, [])
        self.assertIn("ios_provenance", result.evidence)
        provenance = result.evidence.get("ios_provenance", {})
        self.assertEqual(provenance.get("controlled_build_root"), str(controlled_root.resolve()))
        self.assertEqual(provenance.get("manifest_path"), str(manifest.resolve()))
        self.assertEqual(provenance.get("manifest_sha256"), hashlib.sha256(manifest.read_bytes()).hexdigest())
        self.assertEqual(provenance.get("build_config_stamp_path"), str(stamp.resolve()))
        self.assertEqual(provenance.get("build_config_stamp_sha256"), hashlib.sha256(stamp.read_bytes()).hexdigest())
        self.assertEqual(provenance.get("verified_abi"), FFMPEG_COMPONENTS)
        self.assertEqual(
            provenance.get("public_header_hashes"),
            {
                str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(headers)
            },
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
