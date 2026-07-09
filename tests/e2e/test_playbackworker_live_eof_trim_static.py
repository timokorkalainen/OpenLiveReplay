#!/usr/bin/env python3
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_playbackworker_live_eof_trim_static.py <playbackworker.cpp>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    eof_start = source.find("if (hitEof) {")
    non_eof_start = source.find("if (nonEofErr)", eof_start)
    if eof_start < 0 or non_eof_start < 0:
        raise AssertionError("could not locate live EOF recovery block")

    eof_block = source[eof_start:non_eof_start]
    recovery_marker = "Per-insert publish was removed"
    recovery_start = eof_block.find(recovery_marker)
    if recovery_start < 0:
        raise AssertionError("could not locate live EOF recovery publish marker")

    recovery_publish = eof_block[recovery_start:]
    publish_pos = recovery_publish.find("publishOutputCacheLocked();")
    if publish_pos < 0:
        raise AssertionError("live EOF recovery must publish recovered frames")

    before_publish = recovery_publish[:publish_pos]
    if "trimWindow" not in before_publish or "track->buffer.trim" not in before_publish:
        raise AssertionError(
            "live EOF recovery decodes into GPU-backed caches and skips the normal "
            "run-loop trim; it must trim track buffers and output cache before publish"
        )

    seek_pos = eof_block.find("int sret = av_seek_frame")
    drain_pos = eof_block.find("for (int i = 0; i < kEofDrain", seek_pos)
    if seek_pos < 0 or drain_pos < 0:
        raise AssertionError("could not locate live EOF recovery seek/drain")
    premature_latch = eof_block[seek_pos:drain_pos]
    if "m_sizeAtLastEof = sz" in premature_latch:
        raise AssertionError(
            "live EOF recovery must not latch the file size before the bounded drain "
            "actually covers the recovery target"
        )
    if "recoveredNewest >= recoveryTargetMs" not in recovery_publish:
        raise AssertionError(
            "live EOF recovery must only latch the file size after the recovered cache "
            "covers the recovery target; otherwise static files can freeze at a partial tail"
        )

    print("PASS: live EOF recovery trims GPU-backed caches before publish")


if __name__ == "__main__":
    main()
