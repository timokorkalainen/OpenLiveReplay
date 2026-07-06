#include "playback/gpu/gpuframedata.h"

#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/output/formatcanon.h"

#include <QMutexLocker>

#include <utility>

namespace {
constexpr int kReadbackFenceTimeoutMs = 2000;
}

GpuFrameData::GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                           FramePixelFormat nativeFormat, ColorMetadata color,
                           std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge budgetCharge,
                           uint64_t gpuGeneration)
    : m_surface(std::move(surface)), m_rhi(std::move(rhi)), m_renderFence(std::move(renderFence)),
      m_budgetCharge(std::move(budgetCharge)), m_nativeFormat(nativeFormat), m_color(color),
      m_gpuGeneration(gpuGeneration) {}

GpuFrameData::~GpuFrameData() = default;

bool GpuFrameData::waitForPendingFence(int timeoutMs) const {
    if (!m_surface) return false;
    const uint64_t pendingFence = m_surface->pendingFenceValue();
    if (pendingFence == 0) return true;
    if (!m_renderFence) return true;
    return m_renderFence->wait(pendingFence, timeoutMs);
}

CpuPlanes GpuFrameData::cachedCpuPlanes(FramePixelFormat target) const {
    QMutexLocker locker(&m_cacheMutex);
    const auto cached = m_cpuCache.constFind(int(target));
    return cached == m_cpuCache.cend() ? CpuPlanes{} : cached.value();
}

CpuPlanes GpuFrameData::readToCpu(FramePixelFormat target) const {
    const std::shared_ptr<GpuSurface> surface = m_surface;
    const std::shared_ptr<GpuRhiContext> rhi = m_rhi;
    if (!surface) return CpuPlanes{};

    {
        QMutexLocker locker(&m_cacheMutex);
        const auto cached = m_cpuCache.constFind(int(target));
        if (cached != m_cpuCache.cend()) return cached.value();
    }

    if (m_gpuGeneration != 0 && m_gpuGeneration != GpuGenerationCounter::instance().current()) {
        return CpuPlanes{};
    }
    if (GpuDeviceLossMonitor::instance().isLost()) return CpuPlanes{};

    if (!waitForPendingFence(kReadbackFenceTimeoutMs)) return CpuPlanes{};

    {
        QMutexLocker locker(&m_cacheMutex);
        const auto cached = m_cpuCache.constFind(int(target));
        if (cached != m_cpuCache.cend()) return cached.value();
    }

    const FramePixelFormat readbackTarget =
        m_nativeFormat == FramePixelFormat::Rgba8 &&
                (target == FramePixelFormat::Yuv420p || target == FramePixelFormat::Nv12)
            ? FramePixelFormat::Rgba8
            : target;
    CpuPlanes planes;
    if (rhi) {
        planes = rhi->importAndReadback(surface, readbackTarget);
    } else {
#ifdef __APPLE__
        planes = readAppleSurfaceToCpu(surface, readbackTarget, m_color);
#else
        return CpuPlanes{};
#endif
    }
    if (planes.isValid() && planes.format == FramePixelFormat::Rgba8 &&
        target == FramePixelFormat::Yuv420p) {
        planes = formatcanon::exportRgba8ToYuv420p(planes, m_color);
    } else if (planes.isValid() && planes.format == FramePixelFormat::Rgba8 &&
               target == FramePixelFormat::Nv12) {
        planes = formatcanon::exportRgba8ToNv12(planes, m_color);
    }
    if (planes.isValid()) {
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
        gpuRecordFrameReadToCpuReadback();
        if (m_renderFence && surface) {
            const uint64_t fenceValue = m_renderFence->signal();
            gpuRetainSurfaceUntilFenceRetired(surface, m_renderFence, fenceValue);
        }
        QMutexLocker locker(&m_cacheMutex);
        const auto cached = m_cpuCache.constFind(int(target));
        if (cached != m_cpuCache.cend()) return cached.value();
        m_cpuCache.insert(int(target), planes);
    }
    return planes;
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta) {
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), meta, nullptr, GpuBudgetCharge{});
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence) {
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), meta, std::move(renderFence),
                              GpuBudgetCharge{});
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge charge) {
    const GpuSurfaceDesc desc = surface ? surface->desc() : GpuSurfaceDesc{};
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    meta.key.format = desc.format;
    auto data = std::make_shared<GpuFrameData>(std::move(surface), std::move(rhi), meta.key.format,
                                               meta.color, std::move(renderFence),
                                               std::move(charge), meta.gpuGeneration);
    return FrameHandle(std::move(data), meta);
}
