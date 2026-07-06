#include "gpudecodedframe.h"

#include <QtGlobal>

#include <utility>

#if defined(OLR_GPU_PIPELINE_BUILD)
#include "colorvui.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/output/colormetadatapolicy.h"
#endif

#if defined(OLR_GPU_PIPELINE_BUILD) && defined(__APPLE__)
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"
#endif

#if defined(OLR_GPU_PIPELINE_BUILD)
namespace {

ColorMetadata colorMetadataForAccessUnit(const CompressedAccessUnit& unit, int height) {
    VuiColorInfo vui;
    if (unit.codec == NativeVideoCodec::H264 && !unit.parameterSets.h264Sps.isEmpty()) {
        vui = parseSpsColorVui(unit.codec, unit.parameterSets.h264Sps.first());
    } else if (unit.codec == NativeVideoCodec::Hevc && !unit.parameterSets.hevcSps.isEmpty()) {
        vui = parseSpsColorVui(unit.codec, unit.parameterSets.hevcSps.first());
    }
    return resolveColorMetadata(vui, height, 2, 2, 2, 2);
}

} // namespace
#endif

FrameMetadata gpuDecodedFrameMetadata(const CompressedAccessUnit& unit, int width, int height,
                                      qint64 ptsMs) {
    FrameMetadata meta;
    meta.key.ptsMs = ptsMs;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = width;
    meta.key.height = height;
#if defined(OLR_GPU_PIPELINE_BUILD)
    meta.color = colorMetadataForAccessUnit(unit, height);
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
#else
    Q_UNUSED(unit);
#endif
    return meta;
}

FrameHandle makeGpuDecodedFrameHandle(void* nativeDecodedImage, const CompressedAccessUnit& unit,
                                      int width, int height, qint64 ptsMs) {
#if defined(OLR_GPU_PIPELINE_BUILD) && defined(__APPLE__)
    auto surface = wrapAppleImageBuffer(nativeDecodedImage);
    if (!surface) return {};

    FrameMetadata meta = gpuDecodedFrameMetadata(unit, width, height, ptsMs);
    return makeGpuFrameHandle(std::move(surface), nullptr, meta);
#else
    Q_UNUSED(nativeDecodedImage);
    Q_UNUSED(unit);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(ptsMs);
    return {};
#endif
}
