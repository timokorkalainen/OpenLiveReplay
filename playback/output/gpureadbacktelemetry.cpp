#include "playback/output/gpureadbacktelemetry.h"

namespace {
bool rememberBounded(const GpuReadbackSurfaceKey& key, QSet<GpuReadbackSurfaceKey>& keys,
                     QQueue<GpuReadbackSurfaceKey>& order, int limit) {
    if (keys.contains(key)) return false;

    keys.insert(key);
    order.enqueue(key);
    while (order.size() > limit)
        keys.remove(order.dequeue());
    return true;
}
} // namespace

GpuReadbackTelemetry& GpuReadbackTelemetry::instance() {
    static GpuReadbackTelemetry telemetry;
    return telemetry;
}

void GpuReadbackTelemetry::recordGpuReadback(const GpuReadbackSurfaceKey& key) {
    QMutexLocker locker(&m_mutex);
    ++m_gpuReadbacks;
    if (!rememberBounded(key, m_recentReadbacks, m_readbackOrder, kTrackedKeyLimit)) {
        ++m_redundantReadbacks;
    }
}

void GpuReadbackTelemetry::recordSurface(const GpuReadbackSurfaceKey& key) {
    QMutexLocker locker(&m_mutex);
    if (rememberBounded(key, m_recentSurfaces, m_surfaceOrder, kTrackedKeyLimit))
        ++m_uniqueSurfaces;
}

GpuReadbackTelemetrySnapshot GpuReadbackTelemetry::snapshot() const {
    QMutexLocker locker(&m_mutex);
    GpuReadbackTelemetrySnapshot snapshot;
    snapshot.gpuReadbacks = m_gpuReadbacks;
    snapshot.uniqueSurfaces = m_uniqueSurfaces;
    snapshot.redundantReadbacks = m_redundantReadbacks;
    return snapshot;
}

void GpuReadbackTelemetry::reset() {
    QMutexLocker locker(&m_mutex);
    m_gpuReadbacks = 0;
    m_uniqueSurfaces = 0;
    m_redundantReadbacks = 0;
    m_recentSurfaces.clear();
    m_surfaceOrder.clear();
    m_recentReadbacks.clear();
    m_readbackOrder.clear();
}

#ifdef OLR_UNIT_TEST
int GpuReadbackTelemetry::trackedKeyLimitForTests() {
    return kTrackedKeyLimit;
}

int GpuReadbackTelemetry::trackedSurfaceKeysForTests() const {
    QMutexLocker locker(&m_mutex);
    return static_cast<int>(m_recentSurfaces.size());
}

int GpuReadbackTelemetry::trackedReadbackKeysForTests() const {
    QMutexLocker locker(&m_mutex);
    return static_cast<int>(m_recentReadbacks.size());
}
#endif
