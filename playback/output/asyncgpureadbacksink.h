#ifndef ASYNCGPUREADBACKSINK_H
#define ASYNCGPUREADBACKSINK_H

#include "playback/output/gpureadbackring.h"
#include "playback/output/outputsink.h"
#include "playback/output/sinkgpucapability.h"

#include <QList>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

class GpuFence;

class AsyncGpuReadbackSink final : public IOutputSink {
public:
    AsyncGpuReadbackSink(std::unique_ptr<IOutputSink> inner, int ringDepth,
                         FramePixelFormat cpuFormat, SinkGpuCapability capability,
                         std::shared_ptr<GpuFence> renderFence = nullptr,
                         std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks = nullptr,
                         bool innerAlreadyStarted = false);
    ~AsyncGpuReadbackSink() override;

    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override;
    bool submit(const OutputBusFrame& frame) override;
    bool submitAndFlush(const OutputBusFrame& frame, int timeoutMs) override;
    bool prewarmReadback(const OutputBusFrame& frame) override;
    bool flush(int timeoutMs) override;
    void discardPending() override;
    OutputSinkStatus outputStatus() const override;
    bool readbackStats(qint64& depth, qint64& drops) const override;
    bool needsContinuousCadence() const override;

    int ringDepth() const { return m_ringDepth; }
    qint64 readbackDrops() const;
    qint64 readbackQueueDepth() const;
    std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks() const { return m_sharedReadbacks; }

private:
    struct QueuedReadbackJob {
        RingReadbackJob job;
        uint64_t epoch = 0;
        bool deliverToInner = true;
    };

    void workerLoop();
    bool submitGpuFrameAndFlush(const OutputBusFrame& frame, int timeoutMs, bool traceLatency);
    bool flushReadbacks(int timeoutMs);
    void rememberDelivered(const OutputBusFrame& frame);
    void rememberDeliveredLocked(const OutputBusFrame& frame);
    bool usesLatestOnlyPreviewQueue() const;
    void releaseQueuedReadbackJobLocked(const QueuedReadbackJob& queued);
    void queueReadyReadbackJobLocked(RingReadbackJob&& ready, bool deliverToInner = true);
    void clearPendingReadbacksLocked();

    std::unique_ptr<IOutputSink> m_inner;
    bool m_innerAlreadyStarted = false;
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    int m_ringDepth = 1;
    GpuReadbackRing m_ring;
    FramePixelFormat m_cpuFormat = FramePixelFormat::Yuv420p;
    SinkGpuCapability m_capability = SinkGpuCapability::NeedsContinuousCadence;
    std::shared_ptr<GpuFence> m_renderFence;
    std::shared_ptr<SharedGpuReadbackCache> m_sharedReadbacks;

    mutable std::mutex m_mutex;
    mutable std::mutex m_innerMutex;
    std::condition_variable m_wake;
    QList<QueuedReadbackJob> m_jobs;
    std::unique_ptr<QThread> m_thread;
    OutputBusFrame m_lastDelivered;
    OutputFrameIdentity m_lastIdentity;
    uint64_t m_lastGpuGeneration = 0;
    uint64_t m_epoch = 0;
    qint64 m_generationDrops = 0;
    qint64 m_asyncReadbackDrops = 0;
    bool m_hasLastDelivered = false;
    bool m_hasLastIdentity = false;
    bool m_hasGpuGeneration = false;
    std::atomic_bool m_active{false};
    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_readbackEnabled{false};
    std::atomic_bool m_needsReadbackCadence{false};
    bool m_readbackInFlight = false;
    std::atomic_bool m_cancelReadbacks{false};
};

#endif // ASYNCGPUREADBACKSINK_H
