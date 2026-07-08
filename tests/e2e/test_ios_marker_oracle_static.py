#!/usr/bin/env python3
import sys
from pathlib import Path


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_ios_marker_oracle_static.py <ios_marker_srt_oracle.py>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    require(source, "--ndi-recv-probe", "iOS oracle must expose a PGM NDI receiver probe option")
    require(source, "--ndi-sender-name", "iOS oracle must allow a stable PGM NDI sender name")
    require(source, "outputs.ndi.setSenderName", "iOS oracle must configure PGM NDI sender name")
    require(source, "outputs.ndi.setEnabled", "iOS oracle must enable PGM NDI output")
    require(source, "NDIMARKER", "iOS oracle must consume marker lines from ndi_recv_probe")
    require(source, "wait_for_marker", "iOS oracle must wait for expected PGM NDI markers")
    require(source, "pgmTransaction", "iOS oracle must verify websocket PGM transaction ACKs")
    require(source, "framesDecoded", "iOS oracle must drain stale NDI receiver frames")
    require(source, "--cold-seek-frames", "iOS oracle must include cold seek coverage")
    require(source, "coldSeekCount", "iOS oracle summary must report cold seek coverage")
    print("PASS: iOS marker oracle exposes PGM NDI marker validation")


if __name__ == "__main__":
    main()
