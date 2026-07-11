#!/usr/bin/env python3
"""Static contract for deterministic iOS Qt Multimedia plugin selection."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class IosQtMediaPluginPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        cls.pre_push = (ROOT / ".githooks" / "pre-push").read_text(encoding="utf-8")

    def active_lines(self, text: str) -> str:
        without_bracket_comments = re.sub(r"#\[(=*)\[.*?\]\1\]", "", text, flags=re.DOTALL)
        return "\n".join(
            line for line in without_bracket_comments.splitlines() if not line.lstrip().startswith("#")
        )

    def between(self, text: str, start: str, end: str) -> str:
        self.assertIn(start, text)
        self.assertIn(end, text)
        return text[text.index(start) : text.index(end, text.index(start))]

    def test_active_lines_removes_cmake_bracket_comments(self) -> None:
        commented = "#[=[\nqt_import_plugins(OpenLiveReplay INCLUDE Fake)\n]=]\n# ignored\nactive()"
        self.assertEqual(self.active_lines(commented).strip(), "active()")

    def test_cmake_fails_closed_on_exact_qt_plugin_targets(self) -> None:
        ios = self.between(
            self.active_lines(self.cmake),
            "if(IOS)\n    target_link_libraries(OpenLiveReplay PRIVATE OlrStyleplugin)",
            "\nif(WIN32)",
        )
        self.assertIn("set(OLR_QT_DARWIN_MEDIA_PLUGIN Qt6::QDarwinMediaPlugin)", ios)
        self.assertIn("set(OLR_QT_FFMPEG_MEDIA_PLUGIN Qt6::QFFmpegMediaPlugin)", ios)
        self.assertIn(
            "if(NOT TARGET ${OLR_QT_DARWIN_MEDIA_PLUGIN})\n        message(FATAL_ERROR",
            ios,
        )
        self.assertIn(
            "if(NOT TARGET ${OLR_QT_FFMPEG_MEDIA_PLUGIN})\n        message(FATAL_ERROR",
            ios,
        )

    def test_cmake_includes_darwin_and_excludes_ffmpeg_plugin(self) -> None:
        selection = """qt_import_plugins(OpenLiveReplay
        INCLUDE ${OLR_QT_DARWIN_MEDIA_PLUGIN}
        EXCLUDE ${OLR_QT_FFMPEG_MEDIA_PLUGIN})"""
        active_cmake = self.active_lines(self.cmake)
        self.assertIn(selection, active_cmake)
        self.assertNotIn("qt_add_ios_ffmpeg_libraries", active_cmake)

    def test_cmake_generates_a_deterministic_xcode_link_map(self) -> None:
        active_cmake = self.active_lines(self.cmake)
        self.assertIn('XCODE_ATTRIBUTE_LD_GENERATE_MAP_FILE "YES"', active_cmake)
        self.assertIn(
            'XCODE_ATTRIBUTE_LD_MAP_FILE_PATH "$(TARGET_TEMP_DIR)/$(PRODUCT_NAME)-LinkMap-$(CURRENT_VARIANT)-$(CURRENT_ARCH).txt"',
            active_cmake,
        )

    def test_pre_push_audits_unsigned_ipa_and_all_controlled_archives(self) -> None:
        active_hook = self.active_lines(self.pre_push)
        audit = self.between(
            active_hook,
            'python3 "$REPO_ROOT/build-scripts/audit_single_ffmpeg.py"',
            '\necho "[pre-push] iOS build and single-FFmpeg audit OK',
        )
        required = (
            '--package "$APP_BUNDLE"',
            "--platform ios",
            '--link-map "$LINK_MAP"',
            '--final-package "$FINAL_IPA"',
            "--evidence \"$IOS_AUDIT_DIR/OpenLiveReplay-ios-evidence.json\"",
            "--spdx \"$IOS_AUDIT_DIR/OpenLiveReplay-ios.spdx.json\"",
            "libavcodec.xcframework/ios-arm64/libavcodec.a",
            "libavformat.xcframework/ios-arm64/libavformat.a",
            "libavutil.xcframework/ios-arm64/libavutil.a",
            "libswscale.xcframework/ios-arm64/libswscale.a",
            "libswresample.xcframework/ios-arm64/libswresample.a",
            "libsrt.xcframework/ios-arm64/libsrt.a",
            'zip -qry "$FINAL_IPA" Payload',
        )
        for value in required:
            with self.subTest(value=value):
                self.assertIn(value, audit if value.startswith("--") or ".xcframework/" in value else active_hook)
        self.assertIn('CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO', active_hook)
        self.assertIn('if [ "$APP_BUNDLE_COUNT" -ne 1 ]; then', active_hook)
        self.assertIn('if [ "$LINK_MAP_COUNT" -ne 1 ]; then', active_hook)
        self.assertLess(
            active_hook.index('cmake --build "$BUILD_DIR"'),
            active_hook.index("audit_single_ffmpeg.py"),
        )

    def test_pre_push_portably_finds_or_fails_on_missing_ios_qt(self) -> None:
        active_hook = self.active_lines(self.pre_push)
        self.assertNotIn("sort -V", active_hook)
        self.assertIn("olr_newest_qt_kit()", active_hook)
        self.assertIn('newest="$(olr_newest_qt_kit ios)"', active_hook)
        missing = self.between(
            active_hook,
            'if [ ! -x "$QT_CMAKE" ]; then',
            '\necho "[pre-push] Building iOS',
        )
        self.assertIn("exit 1", missing)
        self.assertNotIn("push allowed", missing)


if __name__ == "__main__":
    unittest.main(verbosity=2)
