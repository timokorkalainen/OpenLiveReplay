#ifndef OUTPUTRUNTIME_H
#define OUTPUTRUNTIME_H

#include "playback/output/outputdispatcher.h"

#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <functional>

struct GpuBudgetSnapshot;

struct OutputRuntimeSnapshot {
    OutputFrameCache cache;
    PlaybackStateSnapshot state;

    OutputRuntimeSnapshot() : cache(0, 2, 2) {}
};

class OutputRuntime final : public QThread {
public:
    using SnapshotProvider = std::function<OutputRuntimeSnapshot()>;

    OutputRuntime(FrameRate rate, int feedCount, int width, int height,
                  std::shared_ptr<GpuRhiContext> gpuRhi = nullptr);
    ~OutputRuntime() override;

    void setSnapshotProvider(SnapshotProvider provider);
    void setEndpoints(const QList<OutputEndpoint>& endpoints);
    // Forward identity-skip to the wrapped dispatcher (e.g. tests that assert a
    // per-tick submit of an unchanged frame must disable it).
    void setIdentitySkip(bool enabled);

    void startRuntime();
    void stopRuntime();
    void resetFrameIndex(qint64 nextOutputFrameIndex = 0);
    void resetPlayEpoch();
    void incrementFenceWaitStalls();
    void setGpuRhiContext(std::shared_ptr<GpuRhiContext> gpuRhi);
    void recordGpuBudget(const GpuBudgetSnapshot& snapshot);
    void recordGpuDeviceLossEvents(qint64 events);

    OutputDispatchStats dispatchDueTicksForTest(qint64 wallNowMs);
    OutputDispatchStats dispatchDueTicksForTestNs(qint64 wallNowNs);
    OutputDispatchStats dispatchImmediate();
    OutputDispatchStats stats() const;
    std::shared_ptr<SharedGpuReadbackCache> sharedGpuReadbacks() const;
    // Test support: snapshot of live endpoint sink chains.
    QList<OutputEndpoint> outputEndpointsForTest() const;
#ifdef OLR_UNIT_TEST
    std::shared_ptr<GpuRhiContext> gpuRhiContextForTest() const;
    int playEpochResetCountForTest() const;
#endif
    // Tier3 atomic cut: the next output frame index the dispatcher will emit,
    // read under m_mutex after any active dispatch tick has finished. SAFE to call
    // from makeOutputSnapshot: that provider is invoked by OutputRuntime::snapshot()
    // before dispatchDueTicksNs marks a tick active, so this getter does NOT wait on
    // the tick it is preparing.
    qint64 dispatcherNextOutputFrameIndex() const;
    // The output frame index at which the sampled playhead will reach `playheadMs`
    // (honors the play epoch's speed); -1 if not advancing forward. Same locking as
    // dispatcherNextOutputFrameIndex. Used to schedule an armed cut at an exact
    // playhead (a playlist entry's out-point) for frame-perfect playout transitions.
    qint64 outputFrameForPlayheadMs(qint64 playheadMs) const;

protected:
    void run() override;

private:
    OutputRuntimeSnapshot snapshot() const;
    OutputDispatchStats dispatchDueTicksNs(qint64 wallNowNs);
    OutputDispatchStats statsLocked() const;
    void recordDispatchTiming(qint64 outputFrameIndex, qint64 scheduledNs, qint64 wallNowNs);
    bool dispatchActiveOnCurrentThreadLocked() const;
    void applyPendingDispatchMutationsLocked();
    void waitForDispatchIdleLocked() const;
    static qint64 frameIndexToNsCeil(FrameRate rate, qint64 frameIndex);
    static qint64 dueFrameCount(FrameRate rate, qint64 elapsedNs);

    mutable QMutex m_mutex;
    mutable QWaitCondition m_dispatchIdle;
    OutputDispatcher m_dispatcher;
    SnapshotProvider m_snapshotProvider;
    qint64 m_wallStartNs = -1;
    bool m_stopRequested = false;
    bool m_dispatchActive = false;
    Qt::HANDLE m_dispatchThreadId = nullptr;
    bool m_reconfiguring = false;
    quint64 m_configGeneration = 0;
    bool m_hasPendingEndpoints = false;
    QList<OutputEndpoint> m_pendingEndpoints;
    bool m_hasPendingIdentitySkip = false;
    bool m_pendingIdentitySkip = true;
    bool m_hasPendingFrameIndexReset = false;
    qint64 m_pendingFrameIndexReset = 0;
    bool m_pendingPlayEpochReset = false;
    int m_pendingFenceWaitStalls = 0;
#ifdef OLR_UNIT_TEST
    int m_playEpochResetCountForTest = 0;
#endif
    qint64 m_gpuVramBytes = 0;
    qint64 m_gpuBudgetBytes = 0;
    qint64 m_gpuGatedLiveBytes = 0;
    qint64 m_gpuOomDegrades = 0;
    bool m_gpuBudgetReportOnly = false;
    std::array<qint64, kOutputGpuBudgetTagCount> m_gpuLiveBytesByTag{};
    std::atomic<qint64> m_gpuDeviceLossEvents{0};
    int m_maxCatchUpTicks = 8;
};

#endif // OUTPUTRUNTIME_H
