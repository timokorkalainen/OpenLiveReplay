"""Unit tests for the build-derived Windows CTest launcher."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tools import run_ctest


class RunCtestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name)
        self.build_dir = self.root / "build"
        self.compiler_bin = self.root / "mingw" / "bin"
        self.qt_root = self.root / "Qt" / "6.8.3" / "mingw_64"
        self.ffmpeg_root = self.root / "ffmpeg"
        self.srt_root = self.root / "srt"
        self.cmake_bin = self.root / "cmake" / "bin"
        self.ambient_bin = self.root / "ambient" / "bin"

        self.build_dir.mkdir()
        self.cmake_bin.mkdir(parents=True)
        (self.cmake_bin / "ctest.exe").touch()
        self.cache_values = {
            "CMAKE_CXX_COMPILER": str(self.compiler_bin / "g++.exe"),
            "CMAKE_COMMAND": str(self.cmake_bin / "cmake.exe"),
            "Qt6_DIR": str(self.qt_root / "lib" / "cmake" / "Qt6"),
            "OLR_FFMPEG_ROOT": str(self.ffmpeg_root),
            "OLR_SRT_ROOT": str(self.srt_root),
        }
        self.write_cache()

    def write_cache(self) -> None:
        lines = [f"{key}:FILEPATH={value}" for key, value in self.cache_values.items()]
        (self.build_dir / "CMakeCache.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8"
        )

    def test_find_build_directory_accepts_split_test_dir(self):
        cwd = self.root / "working"
        actual = run_ctest.find_build_directory(
            ["-L", "unit", "--test-dir", "../build"], cwd
        )
        self.assertEqual(actual, (cwd / "../build").resolve())

    def test_find_build_directory_accepts_equals_test_dir(self):
        actual = run_ctest.find_build_directory(
            [f"--test-dir={self.build_dir}", "--output-on-failure"], self.root
        )
        self.assertEqual(actual, self.build_dir.resolve())

    def test_find_build_directory_defaults_to_working_directory(self):
        self.assertEqual(run_ctest.find_build_directory(["-L", "unit"], self.root), self.root)

    def test_read_cmake_cache_parses_typed_values_without_executing_them(self):
        cache_path = self.build_dir / "parser-cache.txt"
        cache_path.write_text(
            "# generated cache\n"
            "// documentation\n"
            "CMAKE_COMMAND:INTERNAL=C:/Program Files/CMake/bin/cmake.exe\n"
            "OLR_FFMPEG_ROOT:PATH=C:/deps/ffmpeg=controlled\n"
            "MALFORMED_LINE\n",
            encoding="utf-8",
        )

        self.assertEqual(
            run_ctest.read_cmake_cache(cache_path),
            {
                "CMAKE_COMMAND": "C:/Program Files/CMake/bin/cmake.exe",
                "OLR_FFMPEG_ROOT": "C:/deps/ffmpeg=controlled",
            },
        )

    def test_windows_runtime_precedes_inherited_path(self):
        env, ctest = run_ctest.prepare_windows_runtime(
            self.build_dir, {"PATH": str(self.ambient_bin)}
        )

        self.assertEqual(
            env["PATH"].split(os.pathsep)[:6],
            [
                str(self.compiler_bin),
                str(self.qt_root / "bin"),
                str(self.ffmpeg_root / "bin"),
                str(self.ffmpeg_root / "lib"),
                str(self.srt_root / "bin"),
                str(self.srt_root / "lib"),
            ],
        )
        self.assertEqual(env["PATH"].split(os.pathsep)[6:], [str(self.ambient_bin)])
        self.assertEqual(ctest, self.cmake_bin / "ctest.exe")

    def test_windows_runtime_sets_controlled_qt_defaults(self):
        inherited = {"PATH": str(self.ambient_bin)}

        env, _ = run_ctest.prepare_windows_runtime(self.build_dir, inherited)

        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.qt_root / "plugins"))
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
        self.assertNotIn("QT_PLUGIN_PATH", inherited)
        self.assertNotIn("QT_QPA_PLATFORM", inherited)

    def test_windows_runtime_preserves_explicit_qpa_platform(self):
        env, _ = run_ctest.prepare_windows_runtime(
            self.build_dir,
            {"PATH": str(self.ambient_bin), "QT_QPA_PLATFORM": "windows"},
        )

        self.assertEqual(env["QT_QPA_PLATFORM"], "windows")

    def test_windows_runtime_reports_missing_required_cache_key(self):
        del self.cache_values["OLR_SRT_ROOT"]
        self.write_cache()

        with self.assertRaisesRegex(ValueError, "OLR_SRT_ROOT"):
            run_ctest.prepare_windows_runtime(self.build_dir, {"PATH": ""})

    @mock.patch("tools.run_ctest._restore_windows_error_mode")
    @mock.patch("tools.run_ctest._set_windows_error_mode", return_value=17)
    @mock.patch("tools.run_ctest.subprocess.run")
    def test_main_propagates_child_exit_code(
        self,
        run: mock.Mock,
        set_error_mode: mock.Mock,
        restore_error_mode: mock.Mock,
    ):
        run.return_value = subprocess.CompletedProcess([], 23)

        result = run_ctest.main(["--test-dir", str(self.build_dir), "-L", "unit"])

        self.assertEqual(result, 23)
        command = run.call_args.args[0]
        self.assertEqual(
            command,
            [
                str(self.cmake_bin / "ctest.exe"),
                "--interactive-debug-mode",
                "0",
                "--test-dir",
                str(self.build_dir),
                "-L",
                "unit",
            ],
        )
        self.assertFalse(run.call_args.kwargs["check"])
        self.assertEqual(
            run.call_args.kwargs["env"]["QT_PLUGIN_PATH"],
            str(self.qt_root / "plugins"),
        )
        set_error_mode.assert_called_once_with()
        restore_error_mode.assert_called_once_with(17)


if __name__ == "__main__":
    unittest.main()
