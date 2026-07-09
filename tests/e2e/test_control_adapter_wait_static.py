#!/usr/bin/env python3
import sys
from pathlib import Path


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise SystemExit(message)


def forbid(source: str, needle: str, message: str) -> None:
    if needle in source:
        raise SystemExit(message)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control_adapter_wait_static.py <uimanagercontroladapter.cpp>")
    source = Path(sys.argv[1]).read_text()

    require(source, "waitForPgm", "operator PGM wait mode must be explicit")
    require(source, "wantsPgmWait", "adapter must gate PGM waits behind a helper")
    require(source, "seekPlaybackAsyncPgm", "transport.seek must use the async PGM path")
    require(source, "jogExternalAsyncPgm", "step/jog must use the async PGM path")
    require(source, "registerPending", "transactional commands must register a pending completion")
    require(source, "seekPlayback(", "default seek path must avoid blocking for PGM evidence")
    require(source, "jogExternal(", "default jog path must avoid blocking for PGM evidence")
    forbid(source, "seekPlaybackAndWaitForPgm", "the adapter must never block on PGM")
    forbid(source, "jogExternalAndWaitForPgm", "the adapter must never block on PGM")

    seek_block = source[source.find('name == QStringLiteral("transport.seek")'):]
    if "wantsPgmWait(args)" not in seek_block.split('name == QStringLiteral("transport.goLive")')[0]:
        raise SystemExit("transport.seek must branch on waitForPgm before waiting for PGM")

    step_block = source[source.find('name == QStringLiteral("transport.stepFrame")'):]
    if "wantsPgmWait(args)" not in step_block.split('name == QStringLiteral("transport.seek")')[0]:
        raise SystemExit("transport.stepFrame must branch on waitForPgm before waiting for PGM")

    jog_block = source[source.find('name == QStringLiteral("action.jog")'):]
    if "wantsPgmWait(args)" not in jog_block.split('name == QStringLiteral("action.shuttle")')[0]:
        raise SystemExit("action.jog must branch on waitForPgm before waiting for PGM")

    # The completion relay is wired on UIManager (Task 5), not the adapter itself;
    # confirm the wiring guard is present alongside the adapter's registry usage.
    uimanager_cpp = Path(sys.argv[1]).resolve().parent.parent / "uimanager.cpp"
    uimanager_source = uimanager_cpp.read_text()
    require(uimanager_source, "wirePlaybackWorkerCompletion",
            "UIManager must wire playback-worker completion relay for the adapter to consume")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
