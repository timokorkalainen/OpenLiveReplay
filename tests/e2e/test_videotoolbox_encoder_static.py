#!/usr/bin/env python3
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def block(source, begin, end):
    start = source.find(begin)
    require(start >= 0, f"could not locate {begin!r}")
    stop = source.find(end, start)
    require(stop >= 0, f"could not locate {end!r} after {begin!r}")
    return source[start:stop]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_videotoolbox_encoder_static.py <nativevideoencoder_videotoolbox.mm>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")

    cpu_input = block(source,
                      "CVPixelBufferRef makeI420PixelBuffer",
                      "QByteArray extractAvcC")
    require("kCVPixelBufferIOSurfacePropertiesKey" not in cpu_input,
            "CPU VideoToolbox encode input must not force IOSurface-backed CVPixelBuffers")
    require("CVPixelBufferCreate(kCFAllocatorDefault" in cpu_input,
            "CPU VideoToolbox encode input must still allocate a CVPixelBuffer")

    gpu_surface = block(source,
                        "bool encodeSurface",
                        "bool encodePixelBuffer")
    require("CVPixelBufferCreateWithIOSurface" in gpu_surface,
            "GPU surface encode must preserve the IOSurface-backed wrapping path")

    print("PASS: VideoToolbox CPU encode input avoids IOSurface-backed CVPixelBuffers")


if __name__ == "__main__":
    main()
