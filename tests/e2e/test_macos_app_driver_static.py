#!/usr/bin/env python3
import ast
import collections
import importlib.util
import sys
import threading
import time
from pathlib import Path


def function(tree, name):
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return node
    raise AssertionError(f"missing function {name}")


def calls(node, name):
    for child in ast.walk(node):
        if isinstance(child, ast.Call):
            target = child.func
            if isinstance(target, ast.Name) and target.id == name:
                return True
            if isinstance(target, ast.Attribute) and target.attr == name:
                return True
    return False


def source_segment(source, node):
    return ast.get_source_segment(source, node) or ""


def true_screen_first_calls(tree):
    count = 0
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        target = node.func
        if not (isinstance(target, ast.Name) and target.id == "assert_preview_at_frame"):
            continue
        for keyword in node.keywords:
            if keyword.arg == "screen_first" and isinstance(keyword.value, ast.Constant):
                if keyword.value.value is True:
                    count += 1
    return count


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_macos_app_driver_static.py <macos_app_driver.py>")
    driver = Path(sys.argv[1])
    repo_root = driver.parents[2]
    source = driver.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(driver))

    screen_assert = function(tree, "assert_screen_preview_at_frame")
    for required_call in ("screenshot", "decode_marker_from_screen"):
        if not calls(screen_assert, required_call):
            raise AssertionError(
                f"assert_screen_preview_at_frame must call {required_call}"
            )

    preview_assert = function(tree, "assert_preview_at_frame")
    if not calls(preview_assert, "assert_screen_preview_at_frame"):
        raise AssertionError(
            "assert_preview_at_frame must validate the visible OS screenshot, "
            "not only capture.current JPEGs"
        )
    preview_source = source_segment(source, preview_assert)
    if "screen_first" not in preview_source:
        raise AssertionError(
            "assert_preview_at_frame must support a screen-first path so app "
            "tests prove prompt visible updates, not only eventual provider captures"
        )
    screen_assert = function(tree, "assert_screen_preview_at_frame")
    screen_source = source_segment(source, screen_assert)
    if "max_elapsed_ms" not in screen_source:
        raise AssertionError(
            "visible screen marker assertions must enforce a latency ceiling, "
            "not only eventual convergence"
        )
    if true_screen_first_calls(tree) < 2:
        raise AssertionError(
            "app visual e2e must use screen_first=True for both jog steps and "
            "cold seeks"
        )
    screenshot_fn = function(tree, "screenshot")
    screenshot_source = source_segment(source, screenshot_fn)
    cache_refresh = "_WINDOW_INFO_CACHE.pop(_APP_PID, None)"
    first_cache_refresh = screenshot_source.find(cache_refresh)
    first_window_lookup = screenshot_source.find("window_info = app_window_info(_APP_PID)")
    if first_cache_refresh < 0 or first_window_lookup < 0 or first_cache_refresh > first_window_lookup:
        raise AssertionError(
            "screenshot() must refresh macOS window front-rank before every capture"
        )
    if "max_screen_elapsed_ms=750.0" in source:
        raise AssertionError(
            "slow macOS OS screenshots must not be used as the low-latency "
            "oracle; command-to-output latency belongs to PGM NDI"
        )
    for required_text in (
        "--ndi-recv-probe",
        "--require-ndi-latency",
        "--latency-threshold-ms",
        "class NdiMarkerWatcher",
        "--stream-markers",
        "outputs.ndi.setSenderName",
        "outputs.ndi.setEnabled",
        "APP_NDI_LATENCY",
        "APP_STEP_NDI_LATENCY",
        "APP_COLD_SEEK_LATENCY",
        "APP_NDI_IGNORED",
        "ackElapsedMs",
        "commandCompletedBeforeMarker",
        "pgmTransaction",
        "waitForPgm",
        "ackAfterPgmTransaction",
        "command.completed missing PGM transaction metadata",
        "postAckElapsedMs",
        "threading.Thread",
        "ignoredMarkers",
        "lastDrainedMarker",
        "lastDrainedFramesDecoded",
        "expectedTimecode",
        "staleOrWrongTimecodeMarkers",
        "baselineAlreadyAtExpectedTimecode",
        "assert_cold_seek_distance",
        "max_ndi_latency_ms",
        "required_ndi_latency_samples",
        "expectedNdiLatencySamples",
        "expected_timecode",
    ):
        if required_text not in source:
            raise AssertionError(
                "macOS app visual e2e must measure command-to-PGM-NDI marker "
                f"latency; missing {required_text!r}"
            )
    if "len(ndi_latency_samples) != required_ndi_latency_samples" not in source:
        raise AssertionError(
            "strict macOS app visual e2e must fail/skip unless every jog and "
            "cold-seek command produced a PGM NDI latency sample"
        )
    if "for index in range(len(sources) - 1, 0, -1)" in source:
        raise AssertionError(
            "macOS app visual e2e must keep four configured sources; trimming to "
            "source 0 misses the production multi-feed path"
        )
    for required_text in (
        'ap.add_argument("--srt-url", action="append"',
        "configure_sources",
        "APP_CONFIGURED_SOURCES",
        "recording_video_stream_count",
        "marker_timelines_from_recording",
        "APP_RECORDING_STREAM_SIGNATURE",
        "recording stream marker signatures are not distinct",
        "videoStreams",
        "len(args.srt_url)",
    ):
        if required_text not in source:
            raise AssertionError(
                "macOS app visual e2e must configure and validate four SRT feeds; "
                f"missing {required_text!r}"
            )
    wrapper = (repo_root / "tests/e2e/run_macos_app_visual_e2e.sh").read_text(
        encoding="utf-8"
    )
    if "--require-ndi-latency" not in wrapper:
        raise AssertionError(
            "macOS visual wrapper must request strict PGM NDI latency checks "
            "when an NDI receiver probe is supplied"
        )
    if "OLR_APP_E2E_LATENCY_THRESHOLD_MS" not in wrapper or "--latency-threshold-ms" not in wrapper:
        raise AssertionError(
            "macOS visual wrapper must expose the PGM NDI latency threshold as "
            "OLR_APP_E2E_LATENCY_THRESHOLD_MS so the 15ms gate is reproducible"
        )
    for required_text in (
        "SOURCE_COUNT=4",
        "MARKER_OFFSET_FRAMES",
        "SRT_PORTS",
        "UDP_PORTS",
        '"$MARKER_SRC" "$WORKDIR/marker-$index" 45 "$((index * MARKER_OFFSET_FRAMES))"',
        '"$WORKDIR/marker-$index.mkv"',
        'DRIVER_ARGS+=(--srt-url "$(srt_caller_url "${SRT_PORTS[$index]}")")',
    ):
        if required_text not in wrapper:
            raise AssertionError(
                "macOS visual wrapper must stand up four independent SRT feeds; "
                f"missing {required_text!r}"
            )
    drive_steps = function(tree, "drive_steps")
    drive_steps_source = source_segment(source, drive_steps)
    if "run_command_with_ndi_latency" not in drive_steps_source:
        raise AssertionError(
            "frame-by-frame jog steps must measure command-to-PGM-NDI latency, "
            "not only cold seeks"
        )

    spec = importlib.util.spec_from_file_location("macos_app_driver", driver)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    watcher = object.__new__(module.NdiMarkerWatcher)
    watcher.proc = None
    watcher.stdout_condition = threading.Condition()
    watcher.stdout_lines = collections.deque([
        "NDIMARKER source=unit marker=168 elapsedMs=10 framesDecoded=11 timecode=399660000\n",
        "NDIMARKER source=unit marker=168 elapsedMs=11 framesDecoded=12 timecode=40000000\n",
    ])
    watcher.reader_done = False
    watcher.lastDrainedMarker = "168"
    watcher.lastDrainedProbeElapsedMs = "9"
    watcher.lastDrainedFramesDecoded = 10
    sample = watcher.wait_for_marker(
        168,
        "unit",
        time.perf_counter(),
        timeout=0.5,
        min_frames_decoded=10,
        expected_timecode=40000000,
    )
    if "framesDecoded=12" not in sample["line"]:
        raise AssertionError(
            "NDI latency oracle must ignore a repeated visual marker whose programme "
            "timecode does not match the commanded playhead"
        )

    class HeldTargetWatcher:
        lastDrainedMarker = None
        lastDrainedProbeElapsedMs = None
        lastDrainedFramesDecoded = None
        lastDrainedTimecode = None

        def drain(self):
            self.lastDrainedMarker = "168"
            self.lastDrainedProbeElapsedMs = "9"
            self.lastDrainedFramesDecoded = 10
            self.lastDrainedTimecode = 40000000
            return 1

        def wait_for_marker(self, *args, **kwargs):
            raise AssertionError("wait_for_marker should not be called for held target baseline")

    try:
        module.run_command_with_ndi_latency(
            HeldTargetWatcher(),
            "held-target",
            168,
            15.0,
            lambda: ({}, {"done": True,
                          "pgmTransaction": {"completed": True, "submittedPgm": True,
                                             "timedOut": False}}, 5.0),
            expected_timecode=40000000,
        )
    except AssertionError as exc:
        if "already at expected timecode before command" not in str(exc):
            raise
    else:
        raise AssertionError(
            "NDI latency oracle must reject a baseline that is already at the "
            "target programme timecode before the command"
        )

    candidates = module.screen_marker_crop_candidates((0, 0, 2536, 1720), 2536, 1720)
    feed_crops = [crop for crop in candidates if crop[0].startswith("feed0-")]
    if not feed_crops:
        raise AssertionError(
            "screen marker decoding must try the visible feed-0 tile, "
            "not only broad stage crops"
        )
    if not any(40 <= x <= 80 and 210 <= y <= 250 and 800 <= width <= 900
               and 450 <= height <= 500
               for _, x, y, width, height in feed_crops):
        raise AssertionError(
            "screen marker decoding must include a feed-0 tile crop calibrated "
            "for macOS window screenshots"
        )
    retina_candidates = module.screen_marker_crop_candidates((0, 0, 2624, 1808), 2624, 1808)
    single_crops = [crop for crop in retina_candidates if crop[0].startswith("single-view-video")]
    if not any(100 <= x <= 125 and 240 <= y <= 260 and 1650 <= width <= 1750
               and 930 <= height <= 990
               for _, x, y, width, height in single_crops):
        raise AssertionError(
            "screen marker decoding must include the single-view 16:9 video crop, "
            "not the surrounding stage chrome"
        )
    display_rect_candidates = module.screen_marker_crop_candidates((0, 0, 2400, 1584),
                                                                   2400, 1584)
    display_rect_crops = [
        crop for crop in display_rect_candidates
        if crop[0].startswith("single-view-display-rect")
    ]
    if not any(0 <= x <= 8 and 145 <= y <= 160 and 1660 <= width <= 1700
               and 930 <= height <= 960
               for _, x, y, width, height in display_rect_crops):
        raise AssertionError(
            "screen marker decoding must include a calibrated display-rect "
            "single-view crop for screencapture -R output"
        )

    for required_text in (
        "OLR_APP_E2E_PID",
        "kCGWindowOwnerPID",
        "kCGWindowIsOnscreen",
        "frontRank",
        "activate_app_by_pid",
        "NSRunningApplication",
        "_SCREENSHOT_WINDOW_BOUNDS",
        "APP_SCREENSHOT_FALLBACK_WINDOW",
        "_APP_PID = proc.pid",
    ):
        if required_text not in source:
            raise AssertionError(
                "macOS app screenshots must target the launched process PID, "
                f"missing {required_text!r}"
            )
    if "capture_rect = " not in source or '"-R", capture_rect' not in source:
        raise AssertionError(
            "macOS app screenshots must capture the visible display rectangle, "
            "not only the stale per-window backing store"
        )
    if "captured_width, captured_height = image_dimensions(path)" not in screenshot_source:
        raise AssertionError(
            "display-rect screenshot crops must use captured pixel dimensions, "
            "not point-sized CGWindow bounds"
        )
    for required_text in (
        "prefer_window",
        "mode=window",
        "screenshot(window_shot, prefer_window=True)",
    ):
        if required_text not in source:
            raise AssertionError(
                "macOS visible marker screenshots must fall back to an OS "
                "window-id capture when display-rect pixels belong to another "
                f"front window; missing {required_text!r}"
            )

    main_cpp = (repo_root / "main.cpp").read_text(encoding="utf-8")
    if "OLR_APP_E2E_FORCE_ACTIVATE" not in main_cpp:
        raise AssertionError(
            "macOS force-activation must be gated behind the e2e harness env var"
        )
    uimanager_cpp = (repo_root / "uimanager.cpp").read_text(encoding="utf-8")
    pos_changed_marker = (
        "connect(m_transport, &PlaybackTransport::posChanged, this, [this](int64_t)"
    )
    pos_changed_index = uimanager_cpp.find(pos_changed_marker)
    pos_changed_end = uimanager_cpp.find("\n    });", pos_changed_index)
    pos_changed_body = (
        uimanager_cpp[pos_changed_index:pos_changed_end]
        if pos_changed_index >= 0 and pos_changed_end >= 0
        else ""
    )
    if "emit scrubPositionChanged()" not in pos_changed_body:
        raise AssertionError(
            "UI scrub position must update immediately on every transport seek/step, "
            "not wait for the next recorder pulse"
        )
    ndi_recv_probe_cpp = (repo_root / "tests/e2e/ndi_recv_probe.cpp").read_text(
        encoding="utf-8"
    )
    for required_text in ("--stream-markers", "NDIWAIT", "NDIMARKER"):
        if required_text not in ndi_recv_probe_cpp:
            raise AssertionError(
                "ndi_recv_probe must expose a streaming marker mode for "
                f"command-to-output latency tests; missing {required_text!r}"
            )

    print("PASS: macOS app driver validates visible OS screenshot markers")


if __name__ == "__main__":
    main()
