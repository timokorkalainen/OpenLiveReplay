#!/usr/bin/env python3
"""Tests for the Qt Multimedia API boundary scanner."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCANNER = Path(__file__).with_name("check_qt_media_api_boundary.py")


class QtMediaApiBoundaryTest(unittest.TestCase):
    def run_scanner(self, root):
        return subprocess.run(
            [sys.executable, str(SCANNER), "--root", str(root)],
            capture_output=True,
            text=True,
            check=False,
        )

    def write_file(self, root, relative_path, content):
        path = root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def make_directory_symlink(self, link, target):
        try:
            link.symlink_to(target, target_is_directory=True)
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"directory symlinks are unavailable: {error}")

    def test_allows_raw_audio_and_application_fed_video_apis(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "player.cpp",
                "QAudioSink audio;\nQMediaDevices devices;\nQVideoFrame frame;\nQVideoSink sink;\n",
            )
            self.write_file(root, "Preview.qml", "VideoOutput { }\n")

            result = self.run_scanner(root)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, "")

    def test_reports_every_prohibited_token_with_relative_path_and_line(self):
        forbidden_tokens = [
            "QMediaPlayer",
            "MediaPlayer",
            "QMediaRecorder",
            "MediaRecorder",
            "QAudioDecoder",
            "QCamera",
            "Camera",
            "QMediaCaptureSession",
            "CaptureSession",
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "media.cpp",
                "// This comment keeps the first diagnostic off line one.\n"
                + "\n".join(forbidden_tokens)
                + "\n",
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            for line_number, token in enumerate(forbidden_tokens, start=2):
                self.assertIn(f"media.cpp:{line_number}: {token}", result.stdout)

    def test_ignores_comments_and_excluded_paths(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "commented.cpp",
                "// QMediaPlayer\n/* QMediaRecorder\n   QAudioDecoder */\nQAudioSink sink;\n",
            )
            self.write_file(root, "Commented.qml", "// MediaPlayer\n/* Camera */\nVideoOutput { }\n")
            self.write_file(root, "tests/fixture.cpp", "QCamera camera;\n")
            self.write_file(root, "docs/notes.qml", "MediaRecorder { }\n")
            self.write_file(root, "build/generated.cpp", "QMediaCaptureSession session;\n")
            self.write_file(root, ".claude/cache.qml", "CaptureSession { }\n")
            self.write_file(root, ".git/hooks/ignored.cpp", "QAudioDecoder decoder;\n")

            result = self.run_scanner(root)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reports_prohibited_token_in_nested_production_output_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(root, "playback/output/player.cpp", "QMediaPlayer player;\n")

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("playback/output/player.cpp:1: QMediaPlayer", result.stdout)

    def test_ignores_top_level_generated_build_output_root_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for directory in (
                "build",
                "build-debug",
                "cmake-build-debug",
                "out",
                "output",
                "windows_build",
                "macos_build",
                "linux_build",
                "ios_build",
            ):
                self.write_file(root, f"{directory}/generated.cpp", "QMediaPlayer player;\n")

            result = self.run_scanner(root)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_traverses_in_root_directory_symlink_without_following_cycles(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repository"
            root.mkdir()
            self.write_file(root, "tests/linked-source/player.cpp", "QMediaRecorder recorder;\n")
            self.make_directory_symlink(root / "linked-production", root / "tests/linked-source")
            self.make_directory_symlink(root / "tests/linked-source/cycle", root)

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("linked-production/player.cpp:1: QMediaRecorder", result.stdout)

    def test_rejects_directory_symlink_that_escapes_scan_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            root = temp_path / "repository"
            root.mkdir()
            outside = temp_path / "outside"
            outside.mkdir()
            self.write_file(outside, "player.cpp", "QMediaPlayer player;\n")
            self.make_directory_symlink(root / "escape", outside)

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("symlink escapes scan root: escape", result.stderr)
            self.assertNotIn("player.cpp:1: QMediaPlayer", result.stdout)

    def test_rejects_source_file_symlink_that_escapes_scan_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            root = temp_path / "repository"
            root.mkdir()
            outside = temp_path / "outside.cpp"
            outside.write_text("QMediaPlayer player;\n", encoding="utf-8")
            try:
                (root / "escape.cpp").symlink_to(outside)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"source-file symlinks are unavailable: {error}")

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source-file symlink escapes scan root: escape.cpp", result.stderr)
            self.assertNotIn("escape.cpp:1: QMediaPlayer", result.stdout)

    def test_preserves_line_number_after_multiline_comment(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "comment-lines.cpp",
                "/* QMediaPlayer\n"
                "   QMediaRecorder */\n"
                "QAudioSink sink;\n"
                "\n"
                "QCamera camera;\n",
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("comment-lines.cpp:5: QCamera", result.stdout)

    def test_reports_code_after_valid_cpp_raw_string_literals(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "raw-literals.cpp",
                'R"tag(text " // )tag"; QMediaPlayer player;\n'
                'u8R"(text " // )"; QMediaRecorder recorder;\n'
                'uR"wide(text " // )wide"; QAudioDecoder decoder;\n'
                'UR"utf(text " // )utf"; QCamera camera;\n'
                'LR"locale(text " // )locale"; QMediaCaptureSession session;\n',
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            for line_number, token in enumerate(
                (
                    "QMediaPlayer",
                    "QMediaRecorder",
                    "QAudioDecoder",
                    "QCamera",
                    "QMediaCaptureSession",
                ),
                start=1,
            ):
                self.assertIn(f"raw-literals.cpp:{line_number}: {token}", result.stdout)

    def test_preserves_line_number_after_multiline_cpp_raw_string_literal(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "raw-lines.cpp",
                'R"tag(text " // still raw\n'
                'second line)tag";\n'
                "\n"
                "QCamera camera;\n",
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("raw-lines.cpp:4: QCamera", result.stdout)

    def test_cmake_requires_python_and_always_registers_boundary_tests(self):
        cmake_contents = Path(__file__).with_name("CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("find_package(Python3 REQUIRED COMPONENTS Interpreter)", cmake_contents)
        self.assertNotIn("find_program(OLR_PYTHON3_EXECUTABLE", cmake_contents)
        self.assertIn('COMMAND "${Python3_EXECUTABLE}"', cmake_contents)
        self.assertIn("qt_media_api_boundary_unit", cmake_contents)
        self.assertIn("qt_media_api_boundary_scan", cmake_contents)

    def test_uses_word_boundaries_for_prohibited_tokens(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(root, "boundary.cpp", "QMediaPlayerFactory factory;\nMediaPlayers players;\n")

            result = self.run_scanner(root)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reports_tokens_after_line_comments_inside_qml_templates(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "line-comment-template.qml",
                "property string text: `//` QMediaPlayer\n",
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("line-comment-template.qml:1: QMediaPlayer", result.stdout)

    def test_reports_tokens_after_block_comments_inside_qml_templates(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "block-comment-template.qml",
                "property string text: `/*\n*/` QMediaRecorder\n",
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("block-comment-template.qml:2: QMediaRecorder", result.stdout)

    def test_keeps_escaped_backticks_inside_qml_templates_and_scans_interpolations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            self.write_file(
                root,
                "escaped-template.qml",
                "property string text: `escaped \\` // still in template` QAudioDecoder\n"
                "property string interpolated: `${QCamera}`\n",
            )

            result = self.run_scanner(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("escaped-template.qml:1: QAudioDecoder", result.stdout)
            self.assertIn("escaped-template.qml:2: QCamera", result.stdout)


if __name__ == "__main__":
    unittest.main()
