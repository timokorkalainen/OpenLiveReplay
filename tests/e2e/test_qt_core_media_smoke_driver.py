#!/usr/bin/env python3
"""Unit tests for the isolated Qt core-media package driver."""

from __future__ import annotations

import importlib.util
import io
import json
import os
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


DRIVER_PATH = Path(__file__).with_name("run_qt_core_media_smoke.py")
ROOT = Path(__file__).resolve().parents[2]


def load_driver():
    spec = importlib.util.spec_from_file_location("qt_core_media_driver", DRIVER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Qt core-media driver")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def yaml_section(text: str, key: str, indent: int) -> str:
    lines = text.splitlines()
    marker = f"{' ' * indent}{key}:"
    try:
        start = next(index for index, line in enumerate(lines) if line == marker)
    except StopIteration as error:
        raise AssertionError(f"YAML section not found: {key}") from error
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        leading = len(line) - len(line.lstrip())
        if leading <= indent:
            end = index
            break
    return "\n".join(lines[start:end])


def yaml_run_commands(job_body: str) -> str:
    lines = job_body.splitlines()
    commands: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        stripped = line.lstrip()
        if stripped.startswith("- "):
            stripped = stripped[2:].lstrip()
        if not stripped.startswith("run:"):
            index += 1
            continue
        run_indent = len(line) - len(stripped)
        value = stripped[len("run:") :].strip()
        if value not in ("|", ">", ""):
            commands.append(value)
            index += 1
            continue
        index += 1
        while index < len(lines):
            command_line = lines[index]
            if command_line.strip():
                command_indent = len(command_line) - len(command_line.lstrip())
                if command_indent <= run_indent:
                    break
                if not command_line.lstrip().startswith("#"):
                    commands.append(command_line.strip())
            index += 1
    return "\n".join(commands)


class DriverPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.driver = load_driver()

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        package = Path(self.temporary.name) / "OpenLiveReplay"
        package.mkdir()
        self.package = package.resolve()

    def module(self, relative: str) -> Path:
        path = self.package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"module")
        return path

    def test_accepts_only_controlled_windows_ffmpeg_abis_inside_package(self) -> None:
        modules = [
            self.module("avcodec-62.dll"),
            self.module("avformat-62.dll"),
            self.module("avutil-60.dll"),
            self.module("swresample-6.dll"),
            self.module("swscale-9.dll"),
        ]
        observed = self.driver.validate_loaded_ffmpeg_modules(modules, self.package, "windows")
        self.assertEqual(observed["avcodec"], "62")
        self.assertEqual(set(observed), {"avcodec", "avformat", "avutil", "swresample", "swscale"})

    def test_runtime_requires_some_but_not_every_ffmpeg_component_loaded(self) -> None:
        modules = [self.module("avcodec-62.dll"), self.module("avutil-60.dll")]
        observed = self.driver.validate_loaded_ffmpeg_modules(modules, self.package, "windows")
        self.assertEqual(observed, {"avcodec": "62", "avutil": "60"})
        with self.assertRaisesRegex(RuntimeError, "did not load any controlled FFmpeg"):
            self.driver.validate_loaded_ffmpeg_modules([], self.package, "windows")

    def test_rejects_old_outside_and_qt_ffmpeg_modules(self) -> None:
        old = self.module("lib/libavcodec.so.61")
        outside = Path(self.temporary.name) / "system" / "libavutil.so.60"
        outside.parent.mkdir()
        outside.write_bytes(b"outside")
        qt_plugin = self.module("plugins/multimedia/libffmpegmediaplugin.so")

        with self.assertRaisesRegex(RuntimeError, "unexpected FFmpeg ABI"):
            self.driver.validate_loaded_ffmpeg_modules([old], self.package, "linux")
        with self.assertRaisesRegex(RuntimeError, "outside isolated package"):
            self.driver.validate_loaded_ffmpeg_modules([outside], self.package, "linux")
        with self.assertRaisesRegex(RuntimeError, "Qt FFmpeg"):
            self.driver.validate_loaded_ffmpeg_modules([qt_plugin], self.package, "linux")

    def test_rejects_unapproved_loaded_ffmpeg_family_components(self) -> None:
        for platform, name in (
            ("windows", "avdevice-62.dll"),
            ("linux", "libavfilter.so.11.2"),
            ("macos", "libpostproc.59.dylib"),
            ("windows", "libavresample-4.dll"),
        ):
            with self.subTest(platform=platform, name=name):
                module = self.module(f"extras/{platform}/{name}")
                with self.assertRaisesRegex(RuntimeError, "unapproved FFmpeg module loaded"):
                    self.driver.validate_loaded_ffmpeg_modules([module], self.package, platform)

    def test_rejects_qt_ffmpeg_backend_logs(self) -> None:
        self.driver.reject_qt_ffmpeg_logs("qt.multimedia.symbolsresolver: backend=darwin")
        with self.assertRaisesRegex(RuntimeError, "Qt FFmpeg backend"):
            self.driver.reject_qt_ffmpeg_logs("qt.multimedia.ffmpeg: Using FFmpeg backend")

    def test_windows_requires_package_local_native_backend_module(self) -> None:
        plugin = self.module("multimedia/windowsmediaplugin.dll")
        evidence = self.driver.validate_native_backend_modules(
            [plugin], self.package, "windows", {"QT_MEDIA_BACKEND": "windows"}
        )
        self.assertEqual(evidence["selection"], "windows")
        self.assertEqual(evidence["module"], str(plugin.resolve()))

        with self.assertRaisesRegex(RuntimeError, "windowsmediaplugin.dll"):
            self.driver.validate_native_backend_modules(
                [], self.package, "windows", {"QT_MEDIA_BACKEND": "windows"}
            )

    def test_macos_accepts_stock_darwin_backend_module_names(self) -> None:
        for name in ("libdarwinmediaplugin.dylib", "darwinmediaplugin.dylib"):
            with self.subTest(name=name):
                plugin = self.module(f"PlugIns/multimedia/{name}")
                evidence = self.driver.validate_native_backend_modules(
                    [plugin], self.package, "macos", {"QT_MEDIA_BACKEND": "darwin"}
                )
                self.assertEqual(evidence["selection"], "darwin")
                self.assertEqual(evidence["module"], str(plugin.resolve()))

    def test_linux_clears_inherited_backend_and_rejects_qt_ffmpeg_plugin(self) -> None:
        layout = self.driver.package_layout(self.package, "linux")
        with mock.patch.dict(os.environ, {"QT_MEDIA_BACKEND": "ffmpeg"}):
            environment = self.driver._isolated_environment(
                layout, "linux", self.package / "documents"
            )
        self.assertNotIn("QT_MEDIA_BACKEND", environment)
        evidence = self.driver.validate_native_backend_modules(
            [], self.package, "linux", environment
        )
        self.assertEqual(evidence["selection"], "default")
        self.assertIsNone(evidence["module"])

        qt_ffmpeg = self.module("usr/plugins/multimedia/libffmpegmediaplugin.so")
        with self.assertRaisesRegex(RuntimeError, "Qt FFmpeg"):
            self.driver.validate_native_backend_modules(
                [qt_ffmpeg], self.package, "linux", environment
            )

    def test_media_payload_validation_rejects_malformed_and_backend_mismatch(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "JSON object"):
            self.driver.validate_media_payload([], "windows", False)
        with self.assertRaisesRegex(RuntimeError, "selected backend"):
            self.driver.validate_media_payload(
                {"backend": "ffmpeg", "audio": "started", "videoFramesObserved": 2},
                "windows",
                False,
            )
        with self.assertRaisesRegex(RuntimeError, "videoFramesObserved"):
            self.driver.validate_media_payload(
                {
                    "backend": "windows",
                    "audio": "started",
                    "videoFramesObserved": "bad",
                    "videoPath": "native-video-output",
                },
                "windows",
                False,
            )
        with self.assertRaisesRegex(RuntimeError, "video path"):
            self.driver.validate_media_payload(
                {
                    "backend": "default",
                    "audio": "started",
                    "videoFramesObserved": 2,
                    "videoPath": "native-video-output",
                },
                "linux",
                False,
            )

    def test_media_payload_validation_accepts_platform_video_paths(self) -> None:
        for platform, backend, video_path in (
            ("linux", "default", "direct-frame"),
            ("windows", "windows", "native-video-output"),
            ("macos", "darwin", "native-video-output"),
        ):
            payload = {
                "backend": backend,
                "audio": "started",
                "videoFramesObserved": 2,
                "videoPath": video_path,
            }
            self.assertIs(
                self.driver.validate_media_payload(payload, platform, False), payload
            )

    def test_platform_layout_uses_package_local_paths(self) -> None:
        windows = self.driver.package_layout(self.package, "windows")
        self.assertEqual(windows.executable_dir, self.package)
        self.assertEqual(windows.plugin_dir, self.package)
        self.assertEqual(windows.qml_dir, self.package / "qml")

        linux = self.driver.package_layout(self.package, "linux")
        self.assertEqual(linux.executable_dir, self.package / "usr" / "bin")
        self.assertEqual(linux.plugin_dir, self.package / "usr" / "plugins")
        self.assertEqual(linux.qml_dir, self.package / "usr" / "qml")

        app = self.package / "OpenLiveReplay.app"
        macos = self.driver.package_layout(app, "macos")
        self.assertEqual(macos.executable_dir, app / "Contents" / "MacOS")
        self.assertEqual(macos.plugin_dir, app / "Contents" / "PlugIns")
        self.assertEqual(macos.qml_dir, app / "Contents" / "Resources" / "qml")

    def test_windows_environment_does_not_inherit_global_dll_search_path(self) -> None:
        layout = self.driver.package_layout(self.package, "windows")
        previous = os.environ.get("PATH")
        os.environ["PATH"] = "C:\\global-qt;C:\\global-ffmpeg"
        try:
            environment = self.driver._isolated_environment(
                layout, "windows", self.package / "documents"
            )
        finally:
            if previous is None:
                os.environ.pop("PATH", None)
            else:
                os.environ["PATH"] = previous
        self.assertNotIn("global-qt", environment["PATH"])
        self.assertNotIn("global-ffmpeg", environment["PATH"])
        self.assertTrue(environment["PATH"].startswith(str(self.package)))

    def test_linux_environment_replaces_poisoned_loader_and_plugin_paths(self) -> None:
        layout = self.driver.package_layout(self.package, "linux")
        poisoned = {
            "LD_LIBRARY_PATH": "/outside/ffmpeg",
            "QT_PLUGIN_PATH": "/outside/plugins",
            "QT_QPA_PLATFORM_PLUGIN_PATH": "/outside/platforms",
            "QML2_IMPORT_PATH": "/outside/qml2",
            "QML_IMPORT_PATH": "/outside/qml",
            "QT_QPA_PLATFORM": "minimal",
        }
        with mock.patch.dict(os.environ, poisoned, clear=False):
            environment = self.driver._isolated_environment(layout, "linux", self.package / "documents")
        self.assertEqual(environment["LD_LIBRARY_PATH"], str(self.package / "usr/lib"))
        self.assertEqual(environment["QT_PLUGIN_PATH"], str(self.package / "usr/plugins"))
        self.assertEqual(environment["QML2_IMPORT_PATH"], str(self.package / "usr/qml"))
        self.assertNotIn("QT_QPA_PLATFORM_PLUGIN_PATH", environment)
        self.assertNotIn("/outside", "\n".join(environment.values()))
        self.assertEqual(environment["QT_QPA_PLATFORM"], "offscreen")

    def test_websocket_health_probe_accepts_101_and_persists_evidence(self) -> None:
        response = b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n"
        connection = mock.MagicMock()
        connection.recv.side_effect = [response, b""]
        process = mock.MagicMock()
        process.poll.return_value = None
        evidence = Path(self.temporary.name) / "health.json"
        with mock.patch.object(self.driver.socket, "create_connection", return_value=connection):
            payload = self.driver._require_websocket_health(4567, process, 0.1, evidence)
        self.assertEqual(payload["statusCode"], 101)
        self.assertEqual(json.loads(evidence.read_text(encoding="utf-8"))["status"], "healthy")
        self.assertIn(b"Upgrade: websocket", connection.sendall.call_args.args[0])

    def test_websocket_health_probe_rejects_timeout_refusal_and_malformed_response(self) -> None:
        process = mock.MagicMock()
        process.poll.return_value = None
        for error, expected in (
            (TimeoutError("timed out"), "timed out"),
            (ConnectionRefusedError("refused"), "refused"),
        ):
            with self.subTest(expected=expected):
                evidence = Path(self.temporary.name) / f"health-{expected}.json"
                with (
                    mock.patch.object(self.driver.socket, "create_connection", side_effect=error),
                    mock.patch.object(self.driver.time, "monotonic", side_effect=[0.0, 1.0]),
                    mock.patch.object(self.driver.time, "sleep"),
                ):
                    with self.assertRaisesRegex(RuntimeError, expected):
                        self.driver._require_websocket_health(4567, process, 0.1, evidence)
                self.assertEqual(json.loads(evidence.read_text(encoding="utf-8"))["status"], "failed")
        connection = mock.MagicMock()
        connection.recv.return_value = b"HTTP/1.1 200 OK\r\n\r\n"
        evidence = Path(self.temporary.name) / "health-malformed.json"
        with mock.patch.object(self.driver.socket, "create_connection", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "expected HTTP 101"):
                self.driver._require_websocket_health(4567, process, 0.1, evidence)
        self.assertEqual(json.loads(evidence.read_text(encoding="utf-8"))["status"], "failed")

    def test_json_read_timeout_does_not_wait_for_blocked_reader(self) -> None:
        class SlowStream:
            def readline(self) -> str:
                time.sleep(2)
                return ""

        class FakeProcess:
            stdout = SlowStream()
            pid = 1

        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.driver._read_json_while_alive(FakeProcess(), "linux", 0.05)
        self.assertLess(time.monotonic() - started, 0.5)

    def test_module_inspection_failure_preserves_emitted_json(self) -> None:
        line = '{"backend":"windows","audio":"started","videoFramesObserved":2}\n'

        class FakeProcess:
            stdout = io.StringIO(line)
            pid = 43

        with mock.patch.object(
            self.driver, "loaded_modules", side_effect=RuntimeError("module snapshot failed")
        ):
            with self.assertRaisesRegex(
                self.driver.ProcessEvidenceError, "module snapshot failed"
            ) as raised:
                self.driver._read_json_while_alive(FakeProcess(), "windows", 0.1)
        self.assertEqual(raised.exception.stdout, line)

    def test_harness_failure_cleans_up_and_persists_process_evidence(self) -> None:
        class FakeProcess:
            stdout = io.StringIO()
            stderr = io.StringIO()
            pid = 41
            returncode = None

            def __init__(self) -> None:
                self.killed = False

            def poll(self):
                return self.returncode

            def kill(self) -> None:
                self.killed = True
                self.returncode = -9

            def communicate(self, timeout=None):
                del timeout
                return "partial harness stdout", "partial harness stderr"

        process = FakeProcess()
        evidence_dir = Path(self.temporary.name) / "evidence"
        layout = self.driver.package_layout(self.package, "windows")
        plugin = self.module("multimedia/windowsmediaplugin.dll")
        with (
            mock.patch.object(self.driver.subprocess, "Popen", return_value=process),
            mock.patch.object(
                self.driver, "_read_json_while_alive", side_effect=RuntimeError("malformed payload")
            ),
            mock.patch.object(self.driver, "loaded_modules", return_value=[plugin]),
        ):
            with self.assertRaisesRegex(RuntimeError, "malformed payload"):
                self.driver._run_harness(
                    self.package / "qt_core_media_smoke.exe",
                    layout,
                    "windows",
                    {"QT_MEDIA_BACKEND": "windows"},
                    False,
                    0.1,
                    evidence_dir,
                )
        self.assertTrue(process.killed)
        self.assertEqual(
            (evidence_dir / "harness-stdout.log").read_text(encoding="utf-8"),
            "partial harness stdout",
        )
        self.assertEqual(
            (evidence_dir / "harness-stderr.log").read_text(encoding="utf-8"),
            "partial harness stderr",
        )
        self.assertEqual(
            json.loads((evidence_dir / "harness-loaded-modules.json").read_text(encoding="utf-8")),
            [str(plugin.resolve())],
        )

    def test_backend_validation_failure_still_persists_media_json(self) -> None:
        class FakeProcess:
            returncode = 0

        evidence_dir = Path(self.temporary.name) / "evidence"
        layout = self.driver.package_layout(self.package, "windows")
        payload = {"backend": "windows", "audio": "started", "videoFramesObserved": 2}
        with (
            mock.patch.object(self.driver.subprocess, "Popen", return_value=FakeProcess()),
            mock.patch.object(
                self.driver,
                "_read_json_while_alive",
                return_value=(payload, [], json.dumps(payload) + "\n", ""),
            ),
        ):
            with self.assertRaisesRegex(RuntimeError, "windowsmediaplugin.dll"):
                self.driver._run_harness(
                    self.package / "qt_core_media_smoke.exe",
                    layout,
                    "windows",
                    {"QT_MEDIA_BACKEND": "windows"},
                    False,
                    0.1,
                    evidence_dir,
                )
        self.assertEqual(
            json.loads((evidence_dir / "harness-media.json").read_text(encoding="utf-8")),
            payload,
        )

    def test_packaged_app_persists_output_modules_and_terminates(self) -> None:
        class FakeProcess:
            pid = 42
            returncode = None

            def __init__(self) -> None:
                self.terminated = False

            def poll(self):
                return self.returncode

            def terminate(self) -> None:
                self.terminated = True
                self.returncode = -15

            def communicate(self, timeout=None):
                del timeout
                return "packaged app stdout", "packaged app stderr"

        process = FakeProcess()
        evidence_dir = Path(self.temporary.name) / "evidence"
        layout = self.driver.package_layout(self.package, "windows")
        ffmpeg = self.module("avcodec-62.dll")
        with (
            mock.patch.object(self.driver.subprocess, "Popen", return_value=process),
            mock.patch.object(
                self.driver, "_require_websocket_health", return_value={"status": "healthy"}
            ) as health,
            mock.patch.object(self.driver, "loaded_modules", return_value=[ffmpeg]),
            mock.patch.object(self.driver.time, "sleep"),
            mock.patch.object(self.driver.time, "monotonic", side_effect=[0.0, 0.0, 0.0]),
        ):
            observed, paths = self.driver._inspect_packaged_app(
                layout, "windows", {}, 0.01, evidence_dir
            )
        self.assertTrue(process.terminated)
        health.assert_called_once()
        self.assertEqual(observed, {"avcodec": "62"})
        self.assertEqual(paths, [str(ffmpeg)])
        self.assertEqual(
            (evidence_dir / "packaged-app-stdout.log").read_text(encoding="utf-8"),
            "packaged app stdout",
        )
        self.assertEqual(
            json.loads(
                (evidence_dir / "packaged-app-loaded-modules.json").read_text(encoding="utf-8")
            ),
            [str(ffmpeg.resolve())],
        )

    def test_packaged_app_health_failure_cleans_up_and_persists_evidence(self) -> None:
        class FakeProcess:
            pid = 42
            returncode = None

            def __init__(self) -> None:
                self.terminated = False

            def poll(self):
                return self.returncode

            def terminate(self) -> None:
                self.terminated = True
                self.returncode = -15

            def communicate(self, timeout=None):
                del timeout
                return "startup stdout", "startup stderr"

        process = FakeProcess()
        connection = mock.MagicMock()
        connection.recv.return_value = b"HTTP/1.1 200 OK\r\n\r\n"
        evidence_dir = Path(self.temporary.name) / "failed-health-evidence"
        layout = self.driver.package_layout(self.package, "windows")
        with (
            mock.patch.object(self.driver.subprocess, "Popen", return_value=process),
            mock.patch.object(self.driver.socket, "create_connection", return_value=connection),
        ):
            with self.assertRaisesRegex(RuntimeError, "expected HTTP 101"):
                self.driver._inspect_packaged_app(layout, "windows", {}, 0.1, evidence_dir)
        self.assertTrue(process.terminated)
        health = json.loads(
            (evidence_dir / "packaged-app-health.json").read_text(encoding="utf-8")
        )
        self.assertEqual(health["status"], "failed")
        self.assertIn("HTTP/1.1 200 OK", health["response"])
        self.assertEqual(
            (evidence_dir / "packaged-app-stdout.log").read_text(encoding="utf-8"),
            "startup stdout",
        )

    def _minimal_main_inputs(self) -> tuple[Path, Path, Path]:
        package = Path(self.temporary.name) / "release"
        package.mkdir()
        (package / "qml").mkdir()
        (package / "OpenLiveReplay.exe").write_bytes(b"app")
        harness = Path(self.temporary.name) / "qt_core_media_smoke.exe"
        harness.write_bytes(b"harness")
        evidence_dir = Path(self.temporary.name) / "runtime-evidence"
        return package, harness, evidence_dir

    def test_main_persists_diagnostic_when_audit_fails(self) -> None:
        package, harness, evidence_dir = self._minimal_main_inputs()
        with mock.patch.object(self.driver, "_audit_package", side_effect=RuntimeError("audit ABI failed")):
            with self.assertRaisesRegex(RuntimeError, "audit ABI failed"):
                self.driver.main(
                    [
                        "--package", str(package),
                        "--harness", str(harness),
                        "--platform", "windows",
                        "--controlled-prefix", f"ffmpeg={package}",
                        "--evidence-dir", str(evidence_dir),
                    ]
                )
        diagnostic = (evidence_dir / "runtime-error.txt").read_text(encoding="utf-8")
        self.assertIn("audit ABI failed", diagnostic)
        failed_result = json.loads(
            (evidence_dir / "runtime-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(failed_result["status"], "failed")
        self.assertIn("audit ABI failed", failed_result["error"])

    def test_main_persists_media_and_final_result_json(self) -> None:
        package, harness, evidence_dir = self._minimal_main_inputs()
        audit = evidence_dir / "audit.json"
        spdx = evidence_dir / "runtime.spdx.json"
        media = {"backend": "windows", "audio": "started", "videoFramesObserved": 2}
        backend = {"selection": "windows", "module": "windowsmediaplugin.dll"}
        with (
            mock.patch.object(self.driver, "_audit_package", return_value=(audit, spdx)),
            mock.patch.object(self.driver, "_prepare_harness", return_value=harness),
            mock.patch.object(self.driver, "_run_harness", return_value=(media, backend)),
            mock.patch.object(
                self.driver,
                "_inspect_packaged_app",
                return_value=({"avcodec": "62"}, ["avcodec-62.dll"]),
            ),
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                self.driver.main(
                    [
                        "--package", str(package),
                        "--harness", str(harness),
                        "--platform", "windows",
                        "--controlled-prefix", f"ffmpeg={package}",
                        "--evidence-dir", str(evidence_dir),
                    ]
                ),
                0,
            )
        self.assertEqual(
            json.loads((evidence_dir / "harness-media.json").read_text(encoding="utf-8")), media
        )
        result = json.loads((evidence_dir / "runtime-result.json").read_text(encoding="utf-8"))
        self.assertEqual(result["backendEvidence"], backend)

    def test_main_audits_release_copy_before_injecting_harness(self) -> None:
        source = DRIVER_PATH.read_text(encoding="utf-8")
        main = source[source.index("def main(") :]
        self.assertLess(main.index("_audit_package("), main.index("_prepare_harness("))

    def test_windows_module_api_declares_64_bit_safe_signatures(self) -> None:
        source = DRIVER_PATH.read_text(encoding="utf-8")
        windows = source[source.index("def _windows_modules") : source.index("def _linux_modules")]
        for declaration in (
            "kernel32.OpenProcess.argtypes",
            "kernel32.OpenProcess.restype",
            "psapi.EnumProcessModulesEx.argtypes",
            "psapi.GetModuleFileNameExW.argtypes",
        ):
            self.assertIn(declaration, windows)

    def test_release_workflow_runs_runtime_smoke_on_every_desktop(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        self.assertEqual(workflow.count("run_qt_core_media_smoke.py"), 3)
        self.assertGreaterEqual(workflow.count("--allow-no-audio-device"), 3)
        self.assertGreaterEqual(workflow.count("runtime-evidence"), 3)
        self.assertIn('--harness build-tests/qt_core_media_smoke.exe', workflow)
        self.assertEqual(workflow.count('--harness build/qt_core_media_smoke'), 2)
        self.assertNotIn('find build-tests -type f', workflow)
        self.assertNotIn('find build -type f -name qt_core_media_smoke', workflow)

    def test_ci_and_full_pre_push_enforce_runtime_policy(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        pre_push = (ROOT / ".githooks" / "pre-push").read_text(encoding="utf-8")
        macos_commands = yaml_run_commands(yaml_section(ci, "build-test-macos", 2))
        windows_commands = yaml_run_commands(yaml_section(ci, "build-test-windows", 2))
        linux_commands = yaml_run_commands(yaml_section(ci, "build-test-linux", 2))
        self.assertIn("-L ci", macos_commands)
        self.assertNotIn("-L smoke", macos_commands)
        self.assertIn("-L smoke", windows_commands)
        self.assertIn("-L smoke", linux_commands)
        self.assertIn("-E '^gpu_capability_calibration_smoke$'", linux_commands)
        self.assertIn("prepare-linux-cgroup", linux_commands)
        direct_calibration = (
            "tests.gpu.test_gpu_capability_runner.ProcessCoordinatorTests."
            "test_task9_native_calibration_smoke_one_configuration"
        )
        self.assertIn(direct_calibration, linux_commands)
        self.assertEqual(linux_commands.count("gpu_capability_calibration_smoke"), 1)
        self.assertEqual(linux_commands.count(direct_calibration), 1)
        self.assertLess(
            linux_commands.index("-E '^gpu_capability_calibration_smoke$'"),
            linux_commands.index("prepare-linux-cgroup"),
        )
        self.assertLess(
            linux_commands.index("prepare-linux-cgroup"),
            linux_commands.index(direct_calibration),
        )
        delegated_prefix = linux_commands[
            linux_commands.index("prepare-linux-cgroup"):
            linux_commands.index(direct_calibration)
        ]
        self.assertNotIn("ctest", delegated_prefix)
        self.assertIn("--property=RuntimeMaxSec=180", linux_commands)
        expected = {
            "macos": (macos_commands, "build-scripts/build_macos_app.sh", "build/OpenLiveReplay.app"),
            "windows": (windows_commands, "build-scripts/build_windows_app.sh", "windows_build/dist/OpenLiveReplay"),
            "linux": (linux_commands, "build-scripts/build_linux_app.sh", "linux_build/dist/OpenLiveReplay"),
        }
        for platform, (commands, packager, package) in expected.items():
            with self.subTest(platform=platform):
                self.assertIn(packager, commands)
                self.assertIn("run_qt_core_media_smoke.py", commands)
                self.assertIn(f"--package {package}", commands)
                self.assertIn(f"--platform {platform}", commands)
                self.assertIn("--allow-no-audio-device", commands)
                self.assertIn("--evidence-dir runtime-evidence/", commands)
                self.assertLess(commands.index("qt_core_media_smoke"), commands.index(packager))
                self.assertLess(commands.index(packager), commands.index("run_qt_core_media_smoke.py"))
        self.assertGreaterEqual(ci.count("build-scripts/single_ffmpeg_policy.json"), 3)
        self.assertGreaterEqual(ci.count("if: always()"), 6)
        self.assertIn("brew install ninja ffmpeg srt ccache pkg-config", macos_commands)
        for tool in ("build-essential", "cmake", "curl", "git", "nasm", "xz-utils"):
            self.assertIn(tool, linux_commands)
        for platform in expected:
            self.assertIn(f"runtime-evidence-{platform}", ci)
        comment_only = "  demo:\n    # ctest -L smoke\n    steps:\n      - run: echo test\n"
        self.assertNotIn(
            "-L smoke", yaml_run_commands(yaml_section(comment_only, "demo", 2))
        )
        self.assertIn("run_qt_core_media_smoke.py", pre_push)
        self.assertIn("OLR_QT_MEDIA_SMOKE_ALLOW_NO_AUDIO", pre_push)
        self.assertIn("SKIP_QT_MEDIA_SMOKE", pre_push)
        self.assertIn('RUNTIME_HARNESS="$REPO_ROOT/build/qt_core_media_smoke"', pre_push)
        self.assertNotIn('find build -type f -name qt_core_media_smoke', pre_push)

    def test_docs_state_stock_qt_requirements_and_runtime_command(self) -> None:
        build_run = (ROOT / "docs" / "build-and-run.md").read_text(encoding="utf-8")
        windows = (ROOT / "docs" / "windows-build.md").read_text(encoding="utf-8")
        for text in (build_run, windows):
            self.assertIn("run_qt_core_media_smoke.py", text)
            self.assertIn("Vulkan headers", text)
            self.assertIn("Qt source build", text)

    def test_macos_packager_uses_portable_qt_kit_discovery(self) -> None:
        script = (ROOT / "build-scripts" / "build_macos_app.sh").read_text(encoding="utf-8")
        self.assertNotIn("sort -V", script)


if __name__ == "__main__":
    unittest.main(verbosity=2)
