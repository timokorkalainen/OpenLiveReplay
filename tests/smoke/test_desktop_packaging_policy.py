#!/usr/bin/env python3
"""Policy tests for the desktop package isolation scripts."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILD_SCRIPTS = ROOT / "build-scripts"
sys.path.insert(0, str(BUILD_SCRIPTS))

import audit_single_ffmpeg as audit  # noqa: E402
import filter_qt_ffmpeg_plugin as filter_plugin  # noqa: E402


class QtFfmpegPluginFilterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.package = Path(self.temporary_directory.name) / "OpenLiveReplay"
        self.package.mkdir()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write(self, relative: str, contents: bytes = b"binary") -> Path:
        path = self.package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def reader(self, edges: dict[str, list[str]]):
        def read_dependencies(path: Path, platform: str) -> list[audit.Dependency]:
            return [audit.Dependency(name) for name in edges.get(path.name, [])]

        return read_dependencies

    def test_removes_only_ffmpeg_libraries_owned_by_qt_ffmpeg_plugin(self) -> None:
        plugin = self.write(
            "multimedia/ffmpegmediaplugin.dll",
            b"QFFmpegMediaPlugin",
        )
        self.write("OpenLiveReplay.exe")
        native_plugin = self.write("plugins/platforms/qwindows.dll")
        ffmpeg_libraries = [
            self.write("avcodec-62.dll"),
            self.write("avformat-62.dll"),
            self.write("avutil-60.dll"),
            self.write("swresample-6.dll"),
            self.write("swscale-9.dll"),
        ]
        edges = {
            plugin.name: ["avformat-62.dll"],
            "avformat-62.dll": ["avcodec-62.dll", "avutil-60.dll", "swresample-6.dll", "swscale-9.dll"],
        }

        result = filter_plugin.filter_package(
            package=self.package,
            platform="windows",
            policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
            dependency_reader=self.reader(edges),
        )

        self.assertEqual(
            result.deleted,
            tuple(sorted(path.relative_to(self.package).as_posix() for path in [plugin, *ffmpeg_libraries])),
        )
        self.assertTrue(native_plugin.is_file())
        self.assertTrue((self.package / "OpenLiveReplay.exe").is_file())
        self.assertFalse(plugin.exists())
        self.assertTrue(all(not path.exists() for path in ffmpeg_libraries))

    def test_rejects_shared_ffmpeg_library_without_deleting_package_files(self) -> None:
        plugin = self.write(
            "multimedia/ffmpegmediaplugin.dll",
            b"QFFmpegMediaPlugin",
        )
        shared_library = self.write("avutil-60.dll")
        self.write("OpenLiveReplay.exe")
        edges = {
            plugin.name: ["avutil-60.dll"],
            "OpenLiveReplay.exe": ["avutil-60.dll"],
        }

        with self.assertRaisesRegex(filter_plugin.FilterFailure, "ambiguous ownership.*avutil-60\\.dll"):
            filter_plugin.filter_package(
                package=self.package,
                platform="windows",
                policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
                dependency_reader=self.reader(edges),
            )

        self.assertTrue(plugin.is_file())
        self.assertTrue(shared_library.is_file())

    def test_rejects_plugin_filename_without_qt_ffmpeg_metadata(self) -> None:
        self.write("multimedia/ffmpegmediaplugin.dll", b"not Qt metadata")

        with self.assertRaisesRegex(filter_plugin.FilterFailure, "metadata"):
            filter_plugin.filter_package(
                package=self.package,
                platform="windows",
                policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
                dependency_reader=self.reader({}),
            )

    def test_requires_platform_specific_plugin_path_and_metadata(self) -> None:
        cases = {
            "windows": "multimedia/ffmpegmediaplugin.dll",
            "linux": "usr/plugins/multimedia/libffmpegmediaplugin.so",
            "macos": "Contents/PlugIns/multimedia/libffmpegmediaplugin.dylib",
        }
        for platform, relative in cases.items():
            with self.subTest(platform=platform):
                self.write(relative, b"QFFmpegMediaPlugin")
                result = filter_plugin.filter_package(
                    package=self.package,
                    platform=platform,
                    policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
                    dependency_reader=self.reader({}),
                )
                self.assertEqual(result.deleted, (relative,))
                self.package.mkdir(exist_ok=True)

    def test_linux_appdir_filter_removes_plugin_and_solely_owned_dependencies(self) -> None:
        plugin = self.write(
            "usr/plugins/multimedia/libffmpegmediaplugin.so",
            b"QFFmpegMediaPlugin",
        )
        dependencies = [
            self.write("usr/lib/libavcodec.so.62"),
            self.write("usr/lib/libavutil.so.60"),
        ]
        native_plugin = self.write("usr/plugins/platforms/libqwayland.so")

        edges = {
            plugin.name: [audit.Dependency("libavcodec.so.62", ("$ORIGIN/../../lib",))],
            "libavcodec.so.62": [audit.Dependency("libavutil.so.60", ("$ORIGIN",))],
        }

        result = filter_plugin.filter_package(
            package=self.package,
            platform="linux",
            policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
            dependency_reader=lambda path, platform: edges.get(path.name, []),
        )

        self.assertEqual(
            result.deleted,
            tuple(sorted(path.relative_to(self.package).as_posix() for path in [plugin, *dependencies])),
        )
        self.assertTrue(native_plugin.is_file())

    def test_linux_old_flattened_plugin_path_is_strictly_absent_or_allowed(self) -> None:
        plugin = self.write("usr/multimedia/libffmpegmediaplugin.so", b"QFFmpegMediaPlugin")

        with self.assertRaisesRegex(filter_plugin.FilterFailure, "absent for linux"):
            filter_plugin.filter_package(
                package=self.package,
                platform="linux",
                policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
                dependency_reader=self.reader({}),
            )

        result = filter_plugin.filter_package(
            package=self.package,
            platform="linux",
            policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
            dependency_reader=self.reader({}),
            allow_absent=True,
        )
        self.assertEqual(result.deleted, ())
        self.assertTrue(plugin.is_file())

    def test_rejects_plugin_outside_platform_specific_multimedia_path(self) -> None:
        plugin = self.write("plugins/multimedia/ffmpegmediaplugin.dll", b"QFFmpegMediaPlugin")

        with self.assertRaisesRegex(filter_plugin.FilterFailure, "absent for windows"):
            filter_plugin.filter_package(
                package=self.package,
                platform="windows",
                policy_path=BUILD_SCRIPTS / "single_ffmpeg_policy.json",
                dependency_reader=self.reader({}),
            )

        self.assertTrue(plugin.is_file())


class DesktopPackagingScriptPolicyTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def assert_before(self, text: str, first: str, second: str) -> None:
        self.assertIn(first, text)
        self.assertIn(second, text)
        self.assertLess(text.index(first), text.index(second), f"{first} must precede {second}")

    def test_packagers_use_package_local_filter_audit_and_qt_configuration(self) -> None:
        requirements = {
            "build-scripts/build_windows_app.sh": ("windeployqt", "cp \"$ROOT_DIR/qt.conf\" \"$APPDIR/qt.conf\"", "Packaging zip"),
            "build-scripts/build_macos_app.sh": ("macdeployqt", "cp \"$ROOT_DIR/qt.conf\" \"$APP/Contents/Resources/qt.conf\"", "ditto -c -k"),
            "build-scripts/build_linux_app.sh": ("cp -a \"$QT_LIB_DIR/\"libQt6*.so*", "cp \"$ROOT_DIR/qt.conf\" \"$APPDIR/usr/bin/qt.conf\"", "tar -czf"),
        }
        for relative, (deployment, qt_conf_copy, archive) in requirements.items():
            script = self.read(relative)
            self.assertIn(qt_conf_copy, script)
            self.assertIn("filter_qt_ffmpeg_plugin.py", script)
            self.assertIn("audit_single_ffmpeg.py", script)
            self.assertIn("--controlled-prefix \"ffmpeg=", script)
            self.assertIn("--controlled-prefix \"srt=", script)
            self.assert_before(script, deployment, "filter_qt_ffmpeg_plugin.py")
            self.assert_before(script, "filter_qt_ffmpeg_plugin.py", "audit_single_ffmpeg.py")
            self.assert_before(script, "audit_single_ffmpeg.py", archive)
            for mutation in ("rm -rf \"$OLR_QT_ROOT", "rm -f \"$OLR_QT_ROOT", "cp ", "mv "):
                if mutation in {"cp ", "mv "}:
                    self.assertNotIn(f'{mutation}"$OLR_QT_ROOT', script)
                else:
                    self.assertNotIn(mutation, script)

    def test_qt_conf_uses_relative_package_paths(self) -> None:
        config = self.read("qt.conf")
        self.assertIn("[Paths]", config)
        self.assertIn("Prefix = .", config)
        self.assertIn("Plugins = .", config)
        self.assertIn("QmlImports = qml", config)

    def test_linux_packager_preserves_audio_and_rendering_plugins_without_multimedia(self) -> None:
        script = self.read("build-scripts/build_linux_app.sh")
        self.assertIn('"$APPDIR/usr/plugins"', script)
        self.assertIn('cp -a "$QT_PLUGIN_DIR/$plugin" "$APPDIR/usr/plugins/"', script)
        self.assertIn(r's/^Plugins = \.$/Plugins = plugins/', script)
        for directory in (
            "audio",
            "xcbglintegrations",
            "egldeviceintegrations",
            "scenegraph",
            "wayland-decoration-client",
            "wayland-graphics-integration-client",
            "wayland-shell-integration",
        ):
            self.assertIn(directory, script)
        self.assertNotIn("multimedia", script)

    def test_linux_packager_installs_sanitizing_launcher_and_forced_rpaths(self) -> None:
        script = self.read("build-scripts/build_linux_app.sh")
        self.assertIn('cp "$BUILD_DIR/bin/OpenLiveReplay" "$APPDIR/usr/bin/OpenLiveReplay.bin"', script)
        self.assertIn("cat > \"$APPDIR/usr/bin/OpenLiveReplay\"", script)
        for variable in (
            "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
            "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
        ):
            self.assertIn(variable, script)
        self.assertIn('exec "$SCRIPT_DIR/OpenLiveReplay.bin" "$@"', script)
        launcher = script[script.index("cat > \"$APPDIR/usr/bin/OpenLiveReplay\"") : script.index("EOF\n", script.index("cat > \"$APPDIR/usr/bin/OpenLiveReplay\""))]
        self.assertNotIn("unset QT_QPA_PLATFORM\n", launcher)
        self.assertIn("patchelf --force-rpath --set-rpath '$ORIGIN/../lib'", script)
        self.assertIn('find "$APPDIR/usr" -type f -name', script)
        self.assertIn('os.path.relpath(sys.argv[2], sys.argv[1])', script)
        self.assertIn('patchelf --force-rpath --set-rpath "$library_rpath"', script)

    def test_macos_packager_uses_bundle_local_qt_plugin_configuration(self) -> None:
        script = self.read("build-scripts/build_macos_app.sh")
        self.assertIn('cp "$ROOT_DIR/qt.conf" "$APP/Contents/Resources/qt.conf"', script)
        self.assertIn("Prefix = ../", script)
        self.assertIn("s/^Plugins = \\.$/Plugins = PlugIns/", script)

    def test_macos_release_requires_exact_controlled_dependencies(self) -> None:
        presets = json.loads(self.read("CMakePresets.json"))
        release = next(
            preset for preset in presets["configurePresets"] if preset["name"] == "macos-release"
        )
        self.assertEqual(release["cacheVariables"]["OLR_CONTROLLED_DEPS_REQUIRED"], "ON")

        cmake = self.read("CMakeLists.txt")
        branch = cmake[cmake.index("elseif(APPLE) # macOS Desktop") : cmake.index("elseif(UNIX AND NOT APPLE)")]
        self.assertIn("if(OLR_CONTROLLED_DEPS_REQUIRED)", branch)
        self.assertIn("requires OLR_FFMPEG_ROOT and OLR_SRT_ROOT", branch)
        for variable in (
            "OLR_FFMPEG_AVFORMAT_LIBRARY", "OLR_FFMPEG_AVCODEC_LIBRARY",
            "OLR_FFMPEG_AVUTIL_LIBRARY", "OLR_FFMPEG_SWSCALE_LIBRARY",
            "OLR_FFMPEG_SWRESAMPLE_LIBRARY", "OLR_SRT_LIBRARY",
        ):
            self.assertIn(variable, branch)
            self.assertIn(f'"${{{variable}}}"', branch)
        self.assertIn("unset(${_olr_controlled_dependency} CACHE)", branch)
        self.assertGreaterEqual(branch.count("NO_DEFAULT_PATH"), 7)
        controlled = branch[:branch.index("else()")]
        self.assertNotIn("OLR_BREW_PREFIX", controlled)

    def test_release_workflow_runs_each_desktop_packager(self) -> None:
        workflow = self.read(".github/workflows/build.yml")
        self.assertIn("build-scripts/build_windows_app.sh", workflow)
        self.assertIn("build-scripts/build_macos_app.sh", workflow)
        self.assertIn("build-scripts/build_linux_app.sh", workflow)
        self.assertIn("OpenLiveReplay-linux.tar.gz", workflow)

    def test_release_workflow_installs_all_required_qt_modules(self) -> None:
        workflow = self.read(".github/workflows/build.yml")
        modules = "modules: qtmultimedia qtshadertools qtwebsockets"
        self.assertEqual(workflow.count(modules), 3)


if __name__ == "__main__":
    unittest.main()
