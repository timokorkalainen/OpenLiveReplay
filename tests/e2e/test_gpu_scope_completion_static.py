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


def ordered(source, markers, message):
    position = -1
    for marker in markers:
        position = source.find(marker, position + 1)
        require(position >= 0, f"{message}: missing or out-of-order {marker!r}")


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_gpu_scope_completion_static.py "
            "<nativevideoencoder_videotoolbox.mm> "
            "<nativevideoencoder_mediafoundation.cpp> "
            "<applegpusurface_apple.mm> <wingpuimportedge.cpp>"
        )

    videotoolbox = Path(sys.argv[1]).read_text(encoding="utf-8")
    mediafoundation = Path(sys.argv[2]).read_text(encoding="utf-8")
    apple_surface = Path(sys.argv[3]).read_text(encoding="utf-8")
    win_import = Path(sys.argv[4]).read_text(encoding="utf-8")

    vt_encode = block(videotoolbox, "bool encodeSurface", "bool encodePixelBuffer")
    require(vt_encode.count("readScope.complete();") == 1,
            "VideoToolbox encodeSurface must complete its read scope exactly once")
    ordered(vt_encode, [
        "GpuSyncReadScope readScope;",
        "readScope.read(surface)",
        "lease.nativeHandle()",
        "CVPixelBufferCreateWithIOSurface",
        "encodePixelBuffer(pb",
        "CVPixelBufferRelease(pb);",
        "readScope.complete();",
        "return encoded;",
    ], "VideoToolbox encodeSurface must retain native access through the encode call")

    mf_sample = block(mediafoundation, "bool MediaFoundationEncoder::buildSurfaceSample",
                      "int64_t MediaFoundationEncoder::resolvePtsTicks")
    require(mf_sample.count("readScope.complete();") == 1,
            "Media Foundation buildSurfaceSample must complete its read scope exactly once")
    ordered(mf_sample, [
        "GpuSyncReadScope readScope;",
        "readScope.read(surface)",
        "lease.nativeHandle()",
        "MFCreateDXGISurfaceBuffer",
        "}();",
        "readScope.complete();",
        "if (!wrapped) return false;",
    ], "Media Foundation must keep the native texture leased until wrapping finishes")

    apple_wrap = block(apple_surface, "CVPixelBufferRef retainApplePixelBufferWrapper",
                       "CpuPlanes readAppleSurfaceToCpu")
    require("scope.withRead(surface" in apple_wrap,
            "Apple wrapper creation must use structured read-scope completion")
    ordered(apple_wrap, [
        "scope.withRead(surface",
        "lease.nativeHandle()",
        "CVPixelBufferCreateWithIOSurface",
        "});",
    ], "Apple wrapper creation must occur inside the structured read callback")

    win_fence = block(win_import, "WinGpuImportEdge::createFenceForSurface",
                      "std::shared_ptr<GpuFence> WinGpuImportEdge::createFence()")
    require("scope.withRead(surface" in win_fence,
            "Windows fence creation must use structured read-scope completion")
    ordered(win_fence, [
        "scope.withRead(surface",
        "lease.nativeHandle()",
        "makeD3D11GpuFence",
        "});",
    ], "Windows fence creation must finish inside the structured read callback")

    win_readback = block(win_import, "CpuPlanes D3D11IGpuFrameData::readToCpu",
                         "CpuPlanes D3D11IGpuFrameData::cachedCpuPlanes")
    require("readScope.complete();" not in win_readback,
            "Windows readback must not reintroduce path-sensitive manual completion")
    ordered(win_readback, [
        "readScope.withRead(m_surface",
        "lease.nativeHandle()",
        "ctx->Map",
        "ctx->Unmap",
        "return true;",
        "});",
        "if (!nativeReadComplete) return out;",
    ], "Windows readback must unmap before its structured read callback completes")

    print("PASS: GPU native-handle scopes cover wrapping, encoding, and readback operations")


if __name__ == "__main__":
    main()
