#!/usr/bin/env python3
import sys
from pathlib import Path


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_pgmstage_videooutput_static.py <ui/components/PgmStage.qml>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")

    require(source, "import QtMultimedia", "PgmStage must use QtMultimedia VideoOutput")
    require(source, "component PreviewVideoOutput", "PgmStage must centralize preview sink attachment")
    require(source, "VideoOutput", "PgmStage must render provider frames through VideoOutput")
    require(source, "addVideoSink(videoSink)", "PgmStage VideoOutput must attach provider sinks")
    require(source, "removeVideoSink(videoSink)", "PgmStage VideoOutput must detach provider sinks")
    require(source, "property bool active", "PgmStage preview sinks must be explicitly active-gated")
    require(source, "updateAttachment()", "PgmStage preview sinks must detach when inactive or hidden")

    if "FramePreviewItem" in source:
        raise AssertionError(
            "PgmStage must not use FramePreviewItem; QQuickPaintedItem/toImage is too costly "
            "for the primary iOS playback surface"
        )

    print("PASS: PgmStage uses VideoOutput preview sinks instead of painted direct previews")


if __name__ == "__main__":
    main()
