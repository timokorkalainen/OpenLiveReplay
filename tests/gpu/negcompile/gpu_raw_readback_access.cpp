#include "playback/gpu/gpurhicontext.h"

GpuReadbackResult forbiddenRawReadback(GpuRhiContext& context,
                                       const std::shared_ptr<GpuSurface>& surface) {
    return context.importAndReadback(surface, FramePixelFormat::Yuv420p);
}
