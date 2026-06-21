#include "playback/gpu/gpurhicontext.h"

#ifndef __APPLE__

#include <utility>

class GpuRhiContext::Impl {};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
GpuRhiContext::~GpuRhiContext() = default;

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
    return nullptr;
}

bool GpuRhiContext::isValid() const {
    return false;
}

CpuPlanes GpuRhiContext::importAndReadback(const std::shared_ptr<GpuSurface>&, FramePixelFormat) {
    return CpuPlanes{};
}

#endif // !__APPLE__
