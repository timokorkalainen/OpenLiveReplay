#include "playback/gpu/gpucompositor.h"

#include "playback/gpu/gpurhicontext.h"

#include <utility>

class GpuCompositor::Impl {
public:
    std::shared_ptr<GpuRhiContext> rhi;
};

GpuCompositor::GpuCompositor(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GpuCompositor::~GpuCompositor() = default;

std::shared_ptr<GpuCompositor> GpuCompositor::create(std::shared_ptr<GpuRhiContext> rhi) {
    if (!rhi || !rhi->isValid()) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->rhi = std::move(rhi);
    return std::shared_ptr<GpuCompositor>(new GpuCompositor(std::move(impl)));
}

#ifndef __APPLE__
std::shared_ptr<GpuSurface>
GpuCompositor::uploadFrameToNv12SurfaceForTest(const FrameHandle&,
                                               const std::shared_ptr<GpuRhiContext>&) {
    return nullptr;
}
#endif

bool GpuCompositor::isValid() const {
    return m_impl && m_impl->rhi && m_impl->rhi->isValid();
}

FrameHandle GpuCompositor::composeGrid(const QList<FrameHandle>&, int, int, ColorMetadata,
                                       ScaleQuality) const {
    return FrameHandle{};
}

FrameHandle GpuCompositor::composePgm(const FrameHandle&, int, int, ColorMetadata,
                                      ScaleQuality) const {
    return FrameHandle{};
}
