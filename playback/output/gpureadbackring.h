#ifndef GPUREADBACKRING_H
#define GPUREADBACKRING_H

#include "playback/output/framepixelformat.h"
#include "playback/output/framehandle.h"
#include "playback/output/gpureadbacktelemetry.h"
#include "playback/output/outputbusengine.h"

#include <QHash>
#include <QtGlobal>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

class GpuFence;

struct RingReadyFrame {
    bool ready = false;
    bool readbackFailed = false;
    OutputBusFrame frame;
};

struct SharedGpuReadbackKey {
    GpuReadbackSurfaceKey surface;
    uint64_t gpuGeneration = 0;

    bool operator==(const SharedGpuReadbackKey& other) const {
        return surface == other.surface && gpuGeneration == other.gpuGeneration;
    }
};

inline size_t qHash(const SharedGpuReadbackKey& key, size_t seed = 0) noexcept {
    return qHashMulti(seed, key.surface, key.gpuGeneration);
}

class SharedGpuReadbackCache {
public:
    void clear();
    void retain(const OutputBusFrame& frame, FramePixelFormat format);
    void release(const OutputBusFrame& frame, FramePixelFormat format);
    bool find(const OutputBusFrame& frame, FramePixelFormat format, CpuPlanes* planes) const;
    void store(const OutputBusFrame& frame, FramePixelFormat format, const CpuPlanes& planes);
    CpuPlanes getOrRead(const OutputBusFrame& frame, FramePixelFormat format,
                        const std::function<CpuPlanes()>& reader,
                        const std::function<bool()>& shouldCancel = {});
    void wakeAll();

private:
    struct Entry {
        CpuPlanes planes;
        bool ready = false;
        bool reading = false;
        int waiters = 0;
        int retainedReaders = 0;
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    QHash<SharedGpuReadbackKey, Entry> m_cache;
};

struct RingReadbackJob {
    bool ready = false;
    OutputBusFrame frame;
    FramePixelFormat format = FramePixelFormat::Yuv420p;
};

class GpuReadbackRing {
public:
    explicit GpuReadbackRing(int depth,
                             std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks = nullptr);

    int depth() const { return m_depth; }
    int occupancy() const;
    qint64 drops() const { return m_drops; }

    RingReadyFrame pushAndPop(const OutputBusFrame& frame, uint64_t fenceValue,
                              std::shared_ptr<GpuFence> fence, FramePixelFormat format);
    RingReadbackJob pushAndTakeReady(const OutputBusFrame& frame, uint64_t fenceValue,
                                     std::shared_ptr<GpuFence> fence, FramePixelFormat format);
    RingReadyFrame flushOne(int timeoutMs);
    RingReadbackJob takeReadyAfterWait(int timeoutMs);
    static RingReadyFrame readBack(const RingReadbackJob& job,
                                   const std::shared_ptr<SharedGpuReadbackCache>& sharedReadbacks,
                                   const std::function<bool()>& shouldCancel = {});

private:
    struct PendingFrame {
        OutputBusFrame frame;
        uint64_t fenceValue = 0;
        std::shared_ptr<GpuFence> fence;
        FramePixelFormat format = FramePixelFormat::Yuv420p;
    };

    RingReadyFrame readBackAndPopOldest();
    RingReadbackJob takeOldest();
    bool oldestReady() const;

    int m_depth = 1;
    qint64 m_drops = 0;
    std::shared_ptr<SharedGpuReadbackCache> m_sharedReadbacks;
    QList<PendingFrame> m_pending;
};

#endif // GPUREADBACKRING_H
