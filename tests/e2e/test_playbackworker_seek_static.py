#!/usr/bin/env python3
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_playbackworker_seek_static.py <playbackworker.cpp>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    if "havePrimedSeekPacket" not in source:
        raise AssertionError(
            "accepted exact-offset seeks must keep the validated packet and feed "
            "it into reposition fill instead of seeking back and reading it again"
        )
    if "landedInBand && avio_seek(m_fmtCtx->pb, offset.value(), SEEK_SET)" in source:
        raise AssertionError(
            "accepted exact-offset seek still rewinds to the same offset after "
            "the validation probe; this adds avoidable cold-seek latency"
        )
    if "if (havePrimedSeekPacket)" not in source:
        raise AssertionError(
            "reposition fill loop must consume the primed exact-seek packet first"
        )

    print("PASS: PlaybackWorker exact seek keeps validated packet for cold seeks")


if __name__ == "__main__":
    main()
