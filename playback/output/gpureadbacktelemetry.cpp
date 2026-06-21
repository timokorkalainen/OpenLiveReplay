#include "playback/output/gpureadbacktelemetry.h"

GpuReadbackTelemetry& GpuReadbackTelemetry::instance() {
    static GpuReadbackTelemetry telemetry;
    return telemetry;
}

void GpuReadbackTelemetry::recordGpuReadback(const GpuReadbackSurfaceKey& key) {
    QMutexLocker locker(&m_mutex);
    ++m_gpuReadbacks;
    m_readbackKeys.insert(key);
}

void GpuReadbackTelemetry::recordSurface(const GpuReadbackSurfaceKey& key) {
    QMutexLocker locker(&m_mutex);
    m_surfaces.insert(key);
}

GpuReadbackTelemetrySnapshot GpuReadbackTelemetry::snapshot() const {
    QMutexLocker locker(&m_mutex);
    GpuReadbackTelemetrySnapshot snapshot;
    snapshot.gpuReadbacks = m_gpuReadbacks;
    snapshot.uniqueSurfaces = qint64(m_readbackKeys.size());
    snapshot.redundantReadbacks = snapshot.gpuReadbacks - snapshot.uniqueSurfaces;
    if (snapshot.redundantReadbacks < 0) snapshot.redundantReadbacks = 0;
    return snapshot;
}

void GpuReadbackTelemetry::reset() {
    QMutexLocker locker(&m_mutex);
    m_gpuReadbacks = 0;
    m_surfaces.clear();
    m_readbackKeys.clear();
}
