#include "playback/gpu/gpuframedata.h"

#include "playback/gpu/gpufence.h"
#include "playback/output/gpureadbacktelemetry.h"

#include <QMutexLocker>

#include <utility>

namespace {

std::atomic<qint64> s_frameReadToCpuCount{0};
std::atomic<quint32> s_nextTelemetryKey{1};

quint32 nextTelemetryKey() {
    quint32 key = s_nextTelemetryKey.fetch_add(1, std::memory_order_acq_rel);
    if (key == 0) key = s_nextTelemetryKey.fetch_add(1, std::memory_order_acq_rel);
    return key;
}

} // namespace

GpuFrameData::GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                           FramePixelFormat nativeFormat, std::shared_ptr<GpuFence> renderFence)
    : m_surface(std::move(surface)), m_rhi(std::move(rhi)), m_renderFence(std::move(renderFence)),
      m_nativeFormat(nativeFormat),
      m_telemetryKey(nextTelemetryKey()) {}

GpuFrameData::~GpuFrameData() = default;

CpuPlanes GpuFrameData::readToCpu(FramePixelFormat target) const {
    if (!m_surface || !m_rhi) return CpuPlanes{};

    QMutexLocker locker(&m_cacheMutex);
    const auto cached = m_cpuCache.constFind(int(target));
    if (cached != m_cpuCache.cend()) return cached.value();

    CpuPlanes planes = m_rhi->importAndReadback(m_surface, target);
    if (planes.isValid()) {
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
        s_frameReadToCpuCount.fetch_add(1, std::memory_order_acq_rel);
        GpuReadbackSurfaceKey key;
        key.busKey = m_telemetryKey;
        key.outputFrameIndex = 0;
        key.format = target;
        GpuReadbackTelemetry::instance().recordSurface(key);
        GpuReadbackTelemetry::instance().recordGpuReadback(key);
        if (m_renderFence && m_surface) {
            const uint64_t fenceValue = m_renderFence->signal();
            if (fenceValue != 0)
                m_surface->retainUntilFenceRetired(fenceValue);
        }
        m_cpuCache.insert(int(target), planes);
    }
    return planes;
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta) {
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), std::move(meta), nullptr);
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence) {
    const GpuSurfaceDesc desc = surface ? surface->desc() : GpuSurfaceDesc{};
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    meta.key.format = desc.format;
    auto data = std::make_shared<GpuFrameData>(std::move(surface), std::move(rhi), meta.key.format,
                                              std::move(renderFence));
    return FrameHandle(std::move(data), meta);
}

qint64 gpuFrameReadToCpuCount() {
    return s_frameReadToCpuCount.load(std::memory_order_acquire);
}

void gpuResetFrameReadToCpuCount() {
    s_frameReadToCpuCount.store(0, std::memory_order_release);
}
