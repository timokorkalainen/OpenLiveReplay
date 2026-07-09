#ifndef GPUREADBACKTELEMETRY_H
#define GPUREADBACKTELEMETRY_H

#include "playback/output/framepixelformat.h"
#include "playback/output/outputtypes.h"

#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QtGlobal>

struct GpuReadbackSurfaceKey {
    OutputBusId bus;
    qint64 outputFrameIndex = -1;
    FramePixelFormat format = FramePixelFormat::Yuv420p;
    uint64_t gpuGeneration = 0;
    int sourceFeedIndex = -1;
    qint64 sourcePtsMs = 0;
    qint64 sourceDecodedSequence = 0;
    quint32 videoHash = 0;
    int width = 0;
    int height = 0;
    bool placeholder = false;

    bool operator==(const GpuReadbackSurfaceKey& other) const {
        return bus == other.bus && outputFrameIndex == other.outputFrameIndex &&
               format == other.format && gpuGeneration == other.gpuGeneration &&
               sourceFeedIndex == other.sourceFeedIndex && sourcePtsMs == other.sourcePtsMs &&
               sourceDecodedSequence == other.sourceDecodedSequence &&
               videoHash == other.videoHash && width == other.width && height == other.height &&
               placeholder == other.placeholder;
    }
};

inline size_t qHash(const GpuReadbackSurfaceKey& key, size_t seed = 0) noexcept {
    return qHashMulti(seed, key.bus, key.outputFrameIndex, int(key.format), key.gpuGeneration,
                      key.sourceFeedIndex, key.sourcePtsMs, key.sourceDecodedSequence,
                      key.videoHash, key.width, key.height, key.placeholder);
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

#ifdef OLR_UNIT_TEST
    static int trackedKeyLimitForTests();
    int trackedSurfaceKeysForTests() const;
    int trackedReadbackKeysForTests() const;
#endif

private:
    GpuReadbackTelemetry() = default;
    static constexpr int kTrackedKeyLimit = 8192;

    mutable QMutex m_mutex;
    qint64 m_gpuReadbacks = 0;
    qint64 m_uniqueSurfaces = 0;
    qint64 m_redundantReadbacks = 0;
    QSet<GpuReadbackSurfaceKey> m_recentSurfaces;
    QQueue<GpuReadbackSurfaceKey> m_surfaceOrder;
    QSet<GpuReadbackSurfaceKey> m_recentReadbacks;
    QQueue<GpuReadbackSurfaceKey> m_readbackOrder;
};

#endif // GPUREADBACKTELEMETRY_H
