#!/usr/bin/env python3
import sys
from pathlib import Path


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise SystemExit(message)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control_adapter_wait_static.py <uimanagercontroladapter.cpp>")
    source = Path(sys.argv[1]).read_text()

    require(source, "waitForPgm", "operator PGM wait mode must be explicit")
    require(source, "wantsPgmWait", "adapter must gate PGM waits behind a helper")
    require(source, "seekPlaybackAndWaitForPgm", "strict oracle seek path must remain available")
    require(source, "jogExternalAndWaitForPgm", "strict oracle jog path must remain available")
    require(source, "seekPlayback(", "default seek path must avoid blocking for PGM evidence")
    require(source, "jogExternal(", "default jog path must avoid blocking for PGM evidence")

    seek_block = source[source.find('name == QStringLiteral("transport.seek")'):]
    if "wantsPgmWait(args)" not in seek_block.split('name == QStringLiteral("transport.goLive")')[0]:
        raise SystemExit("transport.seek must branch on waitForPgm before waiting for PGM")

    step_block = source[source.find('name == QStringLiteral("transport.stepFrame")'):]
    if "wantsPgmWait(args)" not in step_block.split('name == QStringLiteral("transport.seek")')[0]:
        raise SystemExit("transport.stepFrame must branch on waitForPgm before waiting for PGM")

    jog_block = source[source.find('name == QStringLiteral("action.jog")'):]
    if "wantsPgmWait(args)" not in jog_block.split('name == QStringLiteral("action.shuttle")')[0]:
        raise SystemExit("action.jog must branch on waitForPgm before waiting for PGM")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
