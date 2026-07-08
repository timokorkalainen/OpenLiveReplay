#!/usr/bin/env python3
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_gpucompositor_static.py <gpucompositor.cpp>")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    for required in (
        "dummyLumaTexture",
        "dummyChromaTexture",
        "ensureDummyTextures",
        "lumaTextures.push_back(dummyLumaTexture.get())",
        "chromaTextures.push_back(dummyChromaTexture.get())",
    ):
        if required not in source:
            raise AssertionError(
                "GPU compositor must reuse one absent-source texture pair so "
                f"one-source PGM does not allocate 15 dummy luma/chroma pairs; missing {required!r}"
            )
    print("PASS: GPU compositor reuses absent-source dummy textures")


if __name__ == "__main__":
    main()
