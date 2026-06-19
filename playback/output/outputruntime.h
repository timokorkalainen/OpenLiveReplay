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
    // Forward identity-skip to the wrapped dispatcher (e.g. tests that assert a
    // per-tick submit of an unchanged frame must disable it).
    void setIdentitySkip(bool enabled);

    void startRuntime();
    void stopRuntime();
    void resetFrameIndex(qint64 nextOutputFrameIndex = 0);
    void resetPlayEpoch();

    OutputDispatchStats dispatchDueTicksForTest(qint64 wallNowMs);
    OutputDispatchStats dispatchDueTicksForTestNs(qint64 wallNowNs);
    OutputDispatchStats stats() const;
    // Tier3 atomic cut: the next output frame index the dispatcher will emit,
    // read under m_mutex (the same lock that guards m_dispatcher's mutation in
    // dispatchTick/resetFrameIndex). SAFE to call from makeOutputSnapshot: that
    // provider is invoked by OutputRuntime::snapshot() OUTSIDE m_mutex (see
    // dispatchDueTicksNs — snapshot() runs between the two locked scopes), so
    // this getter does NOT re-enter m_mutex.
    qint64 dispatcherNextOutputFrameIndex() const;

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
