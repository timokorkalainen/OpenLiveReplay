#!/usr/bin/env python3
import sys
import re
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
        raise SystemExit("usage: test_videotoolbox_decoder_static.py <nativevideodecoder_videotoolbox.mm>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")

    create_session = block(source,
                           "bool NativeVideoDecoder::Impl::createSession",
                           "bool NativeVideoDecoder::Impl::ensureSession")
    require("createSession(bool requireIOSurface" in create_session,
            "VideoToolbox decoder session creation must take an explicit IOSurface requirement")
    require("if (requireIOSurface)" in create_session,
            "VideoToolbox decoder must add IOSurface output attributes only when requested")
    require("kCVPixelBufferIOSurfacePropertiesKey" in create_session,
            "GPU keep-surface decode still needs IOSurface-backed VideoToolbox output")

    ensure_session = block(source,
                           "bool NativeVideoDecoder::Impl::ensureSession",
                           "bool NativeVideoDecoder::Impl::decode(")
    require(re.search(r"ensureSession\(const CompressedAccessUnit& unit,\s+bool requireIOSurface",
                      ensure_session),
            "VideoToolbox decoder session reuse must include the IOSurface requirement")
    require("sessionRequiresIOSurface == requireIOSurface" in ensure_session,
            "VideoToolbox decoder must not reuse a CPU decode session for keep-surface decode or vice versa")
    require("createSession(requireIOSurface, error)" in ensure_session,
            "VideoToolbox decoder must pass the IOSurface requirement into session creation")

    cpu_decode = block(source,
                       "bool NativeVideoDecoder::Impl::decode(",
                       "bool NativeVideoDecoder::Impl::decodeKeepSurface")
    require("ensureSession(unit, false, error)" in cpu_decode,
            "CPU VideoToolbox decode must not request IOSurface-backed output buffers")

    gpu_decode = block(source,
                       "bool NativeVideoDecoder::Impl::decodeKeepSurface",
                       "NativeVideoDecoder::NativeVideoDecoder")
    require("ensureSession(unit, true, error)" in gpu_decode,
            "GPU keep-surface VideoToolbox decode must request IOSurface-backed output buffers")

    print("PASS: VideoToolbox decoder requests IOSurface output only for keep-surface decode")


if __name__ == "__main__":
    main()
