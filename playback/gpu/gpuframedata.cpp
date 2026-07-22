#include "playback/gpu/gpuframedata.h"

#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/output/formatcanon.h"

#include <QMutexLocker>

#include <array>
#include <utility>

namespace {
constexpr int kReadbackFenceTimeoutMs = 2000;

qint64 cpuPlanesBytes(const CpuPlanes& planes) {
    qint64 bytes = 0;
    for (const QByteArray& plane : planes.plane) {
        bytes += qint64(plane.size());
    }
    return bytes;
}
} // namespace

GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept {
    GpuReadbackResult readback;
    if (!rhi || !surface) return readback;
    const std::shared_ptr<GpuFence> readbackFence = rhi->readbackFence();
    if (!readbackFence) return readback;

    GpuRetireRegistry registry;
    GpuOpScope operation(readbackFence, registry);
    auto adapter = [&](const GpuScopedNativeView<1>& view) noexcept {
        readback = rhi->importAndReadback(view.get<0>(), target);
        return readback.outcome;
    };
    (void) operation.submit(adapter,
                            GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    return readback;
}

struct GpuFrameData::CpuCacheEntry {
    CpuCacheEntry(CpuPlanes cachedPlanes, GpuBudgetCharge cacheCharge)
        : planes(std::move(cachedPlanes)), charge(std::move(cacheCharge)) {}

    CpuPlanes planes;
    GpuBudgetCharge charge;
};

GpuFrameData::GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                           FramePixelFormat nativeFormat, ColorMetadata color,
                           std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge budgetCharge,
                           uint64_t gpuGeneration, uint64_t renderFenceValue)
    : m_surface(std::move(surface)), m_rhi(std::move(rhi)), m_renderFence(std::move(renderFence)),
      m_renderFenceValue(renderFenceValue), m_budgetCharge(std::move(budgetCharge)),
      m_nativeFormat(nativeFormat), m_color(color), m_gpuGeneration(gpuGeneration) {}

GpuFrameData::~GpuFrameData() = default;

bool GpuFrameData::waitForPendingFence(int timeoutMs) const {
    if (!m_surface) return false;
    if (m_renderFenceValue == 0) return true;
    if (!m_renderFence) return false;
    return m_renderFence->wait(m_renderFenceValue, timeoutMs);
}

CpuPlanes GpuFrameData::cachedCpuPlanes(FramePixelFormat target) const {
    QMutexLocker locker(&m_cacheMutex);
    const auto cached = m_cpuCache.constFind(int(target));
    return cached == m_cpuCache.cend() ? CpuPlanes{} : cached.value()->planes;
}

CpuPlanes GpuFrameData::readToCpu(FramePixelFormat target) const {
    const std::shared_ptr<GpuSurface> surface = m_surface;
    const std::shared_ptr<GpuRhiContext> rhi = m_rhi;
    if (!surface) return CpuPlanes{};

    {
        QMutexLocker locker(&m_cacheMutex);
        const auto cached = m_cpuCache.constFind(int(target));
        if (cached != m_cpuCache.cend()) return cached.value()->planes;
    }

    if (m_gpuGeneration != 0 && m_gpuGeneration != GpuGenerationCounter::instance().current()) {
        return CpuPlanes{};
    }
    if (GpuDeviceLossMonitor::instance().isLost()) return CpuPlanes{};

    if (!waitForPendingFence(kReadbackFenceTimeoutMs)) return CpuPlanes{};

    {
        QMutexLocker locker(&m_cacheMutex);
        const auto cached = m_cpuCache.constFind(int(target));
        if (cached != m_cpuCache.cend()) return cached.value()->planes;
    }

    const FramePixelFormat readbackTarget =
        m_nativeFormat == FramePixelFormat::Rgba8 &&
                (target == FramePixelFormat::Yuv420p || target == FramePixelFormat::Nv12)
            ? FramePixelFormat::Rgba8
            : target;
    CpuPlanes planes;
    if (rhi) {
        planes = std::move(submitGpuReadback(rhi, surface, readbackTarget).planes);
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
        QMutexLocker locker(&m_cacheMutex);
        const auto cached = m_cpuCache.constFind(int(target));
        if (cached != m_cpuCache.cend()) return cached.value()->planes;
        m_cpuCache.insert(
            int(target),
            std::make_shared<CpuCacheEntry>(
                planes, GpuBudgetCharge(cpuPlanesBytes(planes), GpuBudgetTag::CpuReadbackCache)));
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
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), std::move(meta),
                              std::move(renderFence), 0, std::move(charge));
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence, uint64_t renderFenceValue,
                               GpuBudgetCharge charge) {
    const GpuSurfaceDesc desc = surface ? surface->desc() : GpuSurfaceDesc{};
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    meta.key.format = desc.format;
    auto data = std::make_shared<GpuFrameData>(
        std::move(surface), std::move(rhi), meta.key.format, meta.color, std::move(renderFence),
        std::move(charge), meta.gpuGeneration, renderFenceValue);
    return FrameHandle(std::move(data), meta);
}
