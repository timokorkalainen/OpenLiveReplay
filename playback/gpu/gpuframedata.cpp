#include "playback/gpu/gpuframedata.h"

#include <utility>

GpuFrameData::GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                           FramePixelFormat nativeFormat)
    : m_surface(std::move(surface)), m_rhi(std::move(rhi)), m_nativeFormat(nativeFormat) {}

GpuFrameData::~GpuFrameData() = default;

CpuPlanes GpuFrameData::readToCpu(FramePixelFormat target) const {
    if (!m_surface || !m_rhi) return CpuPlanes{};

    CpuPlanes planes = m_rhi->importAndReadback(m_surface, target);
    if (planes.isValid()) {
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
    }
    return planes;
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta) {
    const GpuSurfaceDesc desc = surface ? surface->desc() : GpuSurfaceDesc{};
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    meta.key.format = desc.format;
    auto data = std::make_shared<GpuFrameData>(std::move(surface), std::move(rhi), meta.key.format);
    return FrameHandle(std::move(data), std::move(meta));
}
