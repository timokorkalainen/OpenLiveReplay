#include "playback/gpu/gpuframedata.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/output/formatcanon.h"
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
                           FramePixelFormat nativeFormat, ColorMetadata color,
                           std::shared_ptr<GpuFence> renderFence)
    : m_surface(std::move(surface)), m_rhi(std::move(rhi)), m_renderFence(std::move(renderFence)),
      m_nativeFormat(nativeFormat), m_color(color), m_telemetryKey(nextTelemetryKey()) {}

GpuFrameData::~GpuFrameData() = default;

bool GpuFrameData::waitForPendingFence(int timeoutMs) const {
    if (!m_surface) return false;
    const uint64_t pendingFence = m_surface->pendingFenceValue();
    if (pendingFence == 0) return true;
    if (!m_renderFence) return true;
    return m_renderFence->wait(pendingFence, timeoutMs);
}

CpuPlanes GpuFrameData::readToCpu(FramePixelFormat target) const {
    if (!m_surface || !m_rhi) return CpuPlanes{};

    QMutexLocker locker(&m_cacheMutex);
    const auto cached = m_cpuCache.constFind(int(target));
    if (cached != m_cpuCache.cend()) return cached.value();

    if (!waitForPendingFence(-1)) return CpuPlanes{};

    const FramePixelFormat readbackTarget =
        m_nativeFormat == FramePixelFormat::Rgba8 &&
                (target == FramePixelFormat::Yuv420p || target == FramePixelFormat::Nv12)
            ? FramePixelFormat::Rgba8
            : target;
    CpuPlanes planes = m_rhi->importAndReadback(m_surface, readbackTarget);
    if (planes.isValid() && planes.format == FramePixelFormat::Rgba8 &&
        target == FramePixelFormat::Yuv420p) {
        planes = formatcanon::exportRgba8ToYuv420p(planes, m_color);
    } else if (planes.isValid() && planes.format == FramePixelFormat::Rgba8 &&
               target == FramePixelFormat::Nv12) {
        planes = formatcanon::exportRgba8ToNv12(planes, m_color);
    }
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
            gpuRetainSurfaceUntilFenceRetired(m_surface, m_renderFence, fenceValue);
        }
        m_cpuCache.insert(int(target), planes);
    }
    return planes;
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta) {
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), meta, nullptr);
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence) {
    const GpuSurfaceDesc desc = surface ? surface->desc() : GpuSurfaceDesc{};
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    meta.key.format = desc.format;
    auto data = std::make_shared<GpuFrameData>(std::move(surface), std::move(rhi), meta.key.format,
                                               meta.color, std::move(renderFence));
    return FrameHandle(std::move(data), meta);
}

qint64 gpuFrameReadToCpuCount() {
    return s_frameReadToCpuCount.load(std::memory_order_acquire);
}

void gpuResetFrameReadToCpuCount() {
    s_frameReadToCpuCount.store(0, std::memory_order_release);
}
