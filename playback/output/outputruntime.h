#ifndef OUTPUTRUNTIME_H
#define OUTPUTRUNTIME_H

#include "playback/output/outputdispatcher.h"

#include <QMutex>
#include <QThread>
#include <functional>

struct OutputRuntimeSnapshot {
    OutputFrameCache cache;
    PlaybackStateSnapshot state;

    OutputRuntimeSnapshot() : cache(0, 2, 2) {}
};

class OutputRuntime final : public QThread {
public:
    using SnapshotProvider = std::function<OutputRuntimeSnapshot()>;

    OutputRuntime(FrameRate rate, int feedCount, int width, int height);
    ~OutputRuntime() override;

    void setSnapshotProvider(SnapshotProvider provider);
    void setEndpoints(const QList<OutputEndpoint>& endpoints);

    void startRuntime();
    void stopRuntime();
    void resetFrameIndex(qint64 nextOutputFrameIndex = 0);
    void resetPlayEpoch();

    OutputDispatchStats dispatchDueTicksForTest(qint64 wallNowMs);
    OutputDispatchStats dispatchDueTicksForTestNs(qint64 wallNowNs);
    OutputDispatchStats stats() const;

protected:
    void run() override;

private:
    OutputRuntimeSnapshot snapshot() const;
    OutputDispatchStats dispatchDueTicksNs(qint64 wallNowNs);
    void recordDispatchTiming(qint64 outputFrameIndex, qint64 scheduledNs, qint64 wallNowNs);
    static qint64 frameIndexToNsCeil(FrameRate rate, qint64 frameIndex);
    static qint64 dueFrameCount(FrameRate rate, qint64 elapsedNs);

    mutable QMutex m_mutex;
    OutputDispatcher m_dispatcher;
    SnapshotProvider m_snapshotProvider;
    qint64 m_wallStartNs = -1;
    bool m_stopRequested = false;
    int m_maxCatchUpTicks = 8;
};

#endif // OUTPUTRUNTIME_H
