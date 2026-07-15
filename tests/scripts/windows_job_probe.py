#!/usr/bin/env python3
"""Helper process tree for Windows Job Object integration tests."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.windows_job import run_process_tree


def _sleep_forever() -> None:
    while True:
        time.sleep(60)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("parent", "child", "grandchild"))
    parser.add_argument("state_file", type=Path)
    args = parser.parse_args()

    if args.mode == "grandchild":
        _sleep_forever()

    if args.mode == "child":
        grandchild = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "grandchild",
                str(args.state_file),
            ]
        )
        temporary_state = args.state_file.with_suffix(".tmp")
        temporary_state.write_text(
            json.dumps({"child": os.getpid(), "grandchild": grandchild.pid}),
            encoding="utf-8",
        )
        temporary_state.replace(args.state_file)
        _sleep_forever()

    return run_process_tree(
        [sys.executable, str(Path(__file__).resolve()), "child", str(args.state_file)],
        enabled=True,
    )


if __name__ == "__main__":
    raise SystemExit(main())
