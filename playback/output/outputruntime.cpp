#include "playback/output/outputruntime.h"

#include <QElapsedTimer>
#include <utility>

namespace {
constexpr qint64 kNsPerSecond = 1000000000;
} // namespace

OutputRuntime::OutputRuntime(FrameRate rate, int feedCount, int width, int height)
    : m_dispatcher(rate, feedCount, width, height) {}

OutputRuntime::~OutputRuntime() {
    stopRuntime();
}

void OutputRuntime::setSnapshotProvider(SnapshotProvider provider) {
    QMutexLocker locker(&m_mutex);
    m_snapshotProvider = std::move(provider);
}

void OutputRuntime::setEndpoints(const QList<OutputEndpoint>& endpoints) {
    QMutexLocker locker(&m_mutex);
    m_dispatcher.setEndpoints(endpoints);
}

void OutputRuntime::startRuntime() {
    {
        QMutexLocker locker(&m_mutex);
        m_stopRequested = false;
        m_wallStartNs = -1;
        m_dispatcher.resetFrameIndex();
    }
    if (!isRunning()) start();
}

void OutputRuntime::stopRuntime() {
    {
        QMutexLocker locker(&m_mutex);
        m_stopRequested = true;
    }
    if (isRunning()) wait();
    {
        QMutexLocker locker(&m_mutex);
        m_dispatcher.setEndpoints({});
    }
}

void OutputRuntime::resetFrameIndex(qint64 nextOutputFrameIndex) {
    QMutexLocker locker(&m_mutex);
    m_dispatcher.resetFrameIndex(nextOutputFrameIndex);
    m_wallStartNs = -1;
}

void OutputRuntime::resetPlayEpoch() {
    QMutexLocker locker(&m_mutex);
    m_dispatcher.resetPlayEpoch();
}

OutputDispatchStats OutputRuntime::dispatchDueTicksForTest(qint64 wallNowMs) {
    return dispatchDueTicksNs(wallNowMs * kNsPerSecond / 1000);
}

OutputDispatchStats OutputRuntime::dispatchDueTicksForTestNs(qint64 wallNowNs) {
    return dispatchDueTicksNs(wallNowNs);
}

OutputDispatchStats OutputRuntime::stats() const {
    QMutexLocker locker(&m_mutex);
    return m_dispatcher.stats();
}

void OutputRuntime::run() {
    QElapsedTimer timer;
    timer.start();

    while (true) {
        {
            QMutexLocker locker(&m_mutex);
            if (m_stopRequested) break;
        }
        dispatchDueTicksNs(timer.nsecsElapsed());
        QThread::msleep(1);
    }
}

OutputRuntimeSnapshot OutputRuntime::snapshot() const {
    SnapshotProvider provider;
    {
        QMutexLocker locker(&m_mutex);
        provider = m_snapshotProvider;
    }
    return provider ? provider() : OutputRuntimeSnapshot();
}

OutputDispatchStats OutputRuntime::dispatchDueTicksNs(qint64 wallNowNs) {
    qint64 elapsedNs = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_wallStartNs < 0) m_wallStartNs = wallNowNs;
        elapsedNs = qMax<qint64>(0, wallNowNs - m_wallStartNs);
    }

    int dispatched = 0;
    while (dispatched < m_maxCatchUpTicks) {
        qint64 frameIndex = 0;
        qint64 scheduledNs = 0;
        {
            QMutexLocker locker(&m_mutex);
            if (m_stopRequested) break;
            const FrameRate rate = m_dispatcher.frameRate();
            frameIndex = m_dispatcher.nextOutputFrameIndex();
            scheduledNs = frameIndexToNsCeil(rate, frameIndex);
            if (!rate.isValid() || scheduledNs > elapsedNs) {
                return m_dispatcher.stats();
            }
        }

        OutputRuntimeSnapshot current = snapshot();
        {
            QMutexLocker locker(&m_mutex);
            if (m_stopRequested) break;
            m_dispatcher.dispatchTick(current.cache, current.state);
            recordDispatchTiming(frameIndex, scheduledNs, elapsedNs);
        }
        dispatched++;
    }

    QMutexLocker locker(&m_mutex);
    if (dispatched == m_maxCatchUpTicks) {
        OutputRuntimeDispatchStats runtime = m_dispatcher.stats().runtime;
        runtime.catchUpCapHits++;
        runtime.cappedCatchUpTicks++;
        m_dispatcher.setRuntimeStats(runtime);
    }
    return m_dispatcher.stats();
}

void OutputRuntime::recordDispatchTiming(qint64 outputFrameIndex, qint64 scheduledNs,
                                         qint64 wallNowNs) {
    OutputDispatchStats stats = m_dispatcher.stats();
    OutputRuntimeDispatchStats runtime = stats.runtime;
    const qint64 latenessNs = wallNowNs - scheduledNs;
    runtime.hasLastDispatchTiming = true;
    runtime.lastScheduledFrameIndex = outputFrameIndex;
    runtime.lastDispatchedFrameIndex = outputFrameIndex;
    runtime.lastScheduledNs = scheduledNs;
    runtime.lastDispatchWallNs = wallNowNs;
    runtime.lastLatenessNs = latenessNs;
    runtime.maxLatenessNs = qMax(runtime.maxLatenessNs, latenessNs);
    if (latenessNs > 0) runtime.deadlineMisses++;
    m_dispatcher.setRuntimeStats(runtime);
}

qint64 OutputRuntime::frameIndexToNsCeil(FrameRate rate, qint64 frameIndex) {
    if (!rate.isValid() || frameIndex <= 0) return 0;

    const qint64 scaledFrames = frameIndex * qint64(rate.denominator);
    const qint64 wholeSeconds = scaledFrames / rate.numerator;
    const qint64 remainderFrames = scaledFrames % rate.numerator;
    const qint64 fractionalNs =
        (remainderFrames * kNsPerSecond + rate.numerator - 1) / rate.numerator;
    return wholeSeconds * kNsPerSecond + fractionalNs;
}
