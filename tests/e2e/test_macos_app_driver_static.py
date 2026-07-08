#!/usr/bin/env python3
import ast
import importlib.util
import sys
from pathlib import Path


def function(tree, name):
    for node in tree.body:
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
    if "max_screen_elapsed_ms=750.0" not in source:
        raise AssertionError(
            "screen-first jog/cold-seek assertions must use a 750 ms visible "
            "latency ceiling"
        )

    spec = importlib.util.spec_from_file_location("macos_app_driver", driver)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
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

    for required_text in (
        "OLR_APP_E2E_PID",
        "kCGWindowOwnerPID",
        "kCGWindowIsOnscreen",
        "frontRank",
        "_APP_PID = proc.pid",
    ):
        if required_text not in source:
            raise AssertionError(
                "macOS app screenshots must target the launched process PID, "
                f"missing {required_text!r}"
            )

    repo_root = driver.parents[2]
    main_cpp = (repo_root / "main.cpp").read_text(encoding="utf-8")
    if "OLR_APP_E2E_FORCE_ACTIVATE" not in main_cpp:
        raise AssertionError(
            "macOS force-activation must be gated behind the e2e harness env var"
        )

    print("PASS: macOS app driver validates visible OS screenshot markers")


if __name__ == "__main__":
    main()
