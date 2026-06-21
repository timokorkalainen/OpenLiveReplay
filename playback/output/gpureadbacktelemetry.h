#ifndef GPUREADBACKTELEMETRY_H
#define GPUREADBACKTELEMETRY_H

#include "playback/output/framepixelformat.h"

#include <QMutex>
#include <QSet>
#include <QtGlobal>

struct GpuReadbackSurfaceKey {
    quint32 busKey = 0;
    qint64 outputFrameIndex = -1;
    FramePixelFormat format = FramePixelFormat::Yuv420p;

    bool operator==(const GpuReadbackSurfaceKey& other) const {
        return busKey == other.busKey && outputFrameIndex == other.outputFrameIndex &&
               format == other.format;
    }
};

inline size_t qHash(const GpuReadbackSurfaceKey& key, size_t seed = 0) noexcept {
    return qHashMulti(seed, key.busKey, key.outputFrameIndex, int(key.format));
}

struct GpuReadbackTelemetrySnapshot {
    qint64 gpuReadbacks = 0;
    qint64 uniqueSurfaces = 0;
    qint64 redundantReadbacks = 0;
};

class GpuReadbackTelemetry {
public:
    static GpuReadbackTelemetry& instance();

    void recordGpuReadback(const GpuReadbackSurfaceKey& key);
    void recordSurface(const GpuReadbackSurfaceKey& key);
    GpuReadbackTelemetrySnapshot snapshot() const;
    void reset();

private:
    GpuReadbackTelemetry() = default;

    mutable QMutex m_mutex;
    qint64 m_gpuReadbacks = 0;
    QSet<GpuReadbackSurfaceKey> m_surfaces;
    QSet<GpuReadbackSurfaceKey> m_readbackKeys;
};

#endif // GPUREADBACKTELEMETRY_H
