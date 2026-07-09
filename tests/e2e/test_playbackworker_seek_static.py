#!/usr/bin/env python3
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_playbackworker_seek_static.py <playbackworker.cpp>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    reposition_start = source.find("void PlaybackWorker::repositionTo(")
    preroll_start = source.find("void PlaybackWorker::fillStaging()")
    if reposition_start < 0 or preroll_start < 0:
        raise AssertionError("could not locate repositionTo/fillStaging boundaries")
    reposition = source[reposition_start:preroll_start]

    if "nearestAtOrBefore(anchor)" in reposition:
        raise AssertionError(
            "repositionTo must not use FrameIndex byte offsets for Matroska cold "
            "seeks; raw byte landing has produced EBML-boundary failures"
        )
    if "avio_seek(m_fmtCtx->pb" in reposition:
        raise AssertionError(
            "repositionTo must enter MKV through avformat/av_seek_frame, not raw "
            "avio_seek byte offsets"
        )

    apple_gpu_start = source.find("#if defined(OLR_GPU_PIPELINE_BUILD) && defined(__APPLE__)")
    if apple_gpu_start < 0:
        raise AssertionError("could not locate Apple GPU playback decode path")
    decode_keep_surface = source.find("track->nativeDecoder->decodeKeepSurface",
                                      apple_gpu_start)
    if decode_keep_surface < 0:
        raise AssertionError("could not locate Apple keep-surface decode call")
    pre_decode = source[apple_gpu_start:decode_keep_surface]
    if "GpuBudget::instance().canAllocate" not in pre_decode:
        raise AssertionError(
            "Apple GPU playback must preflight the residency budget before "
            "decodeKeepSurface; otherwise VideoToolbox creates throwaway "
            "IOSurfaces after the budget is already full"
        )

    print("PASS: PlaybackWorker cold seek and GPU decode budget guards are present")


if __name__ == "__main__":
    main()
