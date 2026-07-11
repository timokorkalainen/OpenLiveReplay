#!/usr/bin/env python3
import sys
from pathlib import Path


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_pgmstage_videooutput_static.py "
            "<ui/components/PgmStage.qml> <MultiviewWindow.qml> "
            "<ui/components/PreviewSurface.qml>"
        )

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    multiview_source = Path(sys.argv[2]).read_text(encoding="utf-8")
    preview_surface = Path(sys.argv[3]).read_text(encoding="utf-8")

    require(source, "PreviewSurface", "PgmStage must use the platform preview surface")
    require(multiview_source, "PreviewSurface", "MultiviewWindow must use the platform preview surface")
    require(preview_surface, "import QtMultimedia", "PreviewSurface must provide native VideoOutput")
    require(preview_surface, "import Recorder.Types", "PreviewSurface must provide direct frame painting")
    require(preview_surface, 'Qt.platform.os === "linux"', "direct preview must be Linux-only")
    require(preview_surface, "FramePreviewItem", "Linux must render without a Qt media backend")
    require(preview_surface, "VideoOutput", "non-Linux platforms must retain native VideoOutput")
    require(preview_surface, "addVideoSink(videoSink)", "native preview must attach provider sinks")
    require(preview_surface, "removeVideoSink(videoSink)", "native preview must detach provider sinks")
    require(preview_surface, "property bool active", "preview surfaces must be explicitly active-gated")
    require(preview_surface, "updateAttachment()", "native preview must detach when inactive or hidden")

    for name, preview_source in (
        ("PreviewSurface", preview_surface),
    ):
        require(
            preview_source,
            "property QtObject attachedProvider",
            f"{name} must null provider references when their C++ QObject is destroyed",
        )
        require(
            preview_source,
            "var previousProvider =",
            f"{name} must snapshot its old provider before replacement",
        )
        require(
            preview_source,
            "attachedProvider = null",
            f"{name} must clear the old provider before best-effort cleanup",
        )
        require(
            preview_source,
            'typeof previousProvider.removeVideoSink === "function"',
            f"{name} must tolerate an already-destroyed old provider",
        )

    print("PASS: previews use direct painting on Linux and native VideoOutput elsewhere")


if __name__ == "__main__":
    main()
