#include "playback/output/outputruntime.h"

#include "playback/gpu/gpubudget.h"

#include <QElapsedTimer>
#include <cmath>
#include <utility>

namespace {
constexpr qint64 kNsPerSecond = 1000000000;
static_assert(kOutputGpuBudgetTagCount == kGpuBudgetTagCount,
              "OutputDispatchStats GPU tag storage must match GpuBudgetSnapshot tags");
} // namespace

OutputRuntime::OutputRuntime(FrameRate rate, int feedCount, int width, int height,
                             std::shared_ptr<GpuRhiContext> gpuRhi)
    : m_dispatcher(rate, feedCount, width, height, std::move(gpuRhi)) {}

OutputRuntime::~OutputRuntime() {
    stopRuntime();
}

void OutputRuntime::setSnapshotProvider(SnapshotProvider provider) {
    QMutexLocker locker(&m_mutex);
    m_snapshotProvider = std::move(provider);
}

void OutputRuntime::setEndpoints(const QList<OutputEndpoint>& endpoints) {
    QMutexLocker locker(&m_mutex);
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingEndpoints = endpoints;
        m_hasPendingEndpoints = true;
        return;
    }
    m_reconfiguring = true;
    waitForDispatchIdleLocked();
    m_dispatcher.setEndpoints(endpoints);
    ++m_configGeneration;
    m_reconfiguring = false;
    m_dispatchIdle.wakeAll();
}

void OutputRuntime::setIdentitySkip(bool enabled) {
    QMutexLocker locker(&m_mutex);
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingIdentitySkip = enabled;
        m_hasPendingIdentitySkip = true;
        return;
    }
    waitForDispatchIdleLocked();
    m_dispatcher.setIdentitySkip(enabled);
}

void OutputRuntime::startRuntime() {
    {
        QMutexLocker locker(&m_mutex);
        if (dispatchActiveOnCurrentThreadLocked()) {
            m_stopRequested = false;
            m_pendingFrameIndexReset = 0;
            m_hasPendingFrameIndexReset = true;
            return;
        }
        waitForDispatchIdleLocked();
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
        if (dispatchActiveOnCurrentThreadLocked()) {
            m_pendingEndpoints = {};
            m_hasPendingEndpoints = true;
            m_dispatchIdle.wakeAll();
            return;
        }
        m_dispatchIdle.wakeAll();
    }
    if (isRunning()) wait();
    {
        QMutexLocker locker(&m_mutex);
        waitForDispatchIdleLocked();
        m_dispatcher.setEndpoints({});
    }
}

void OutputRuntime::resetFrameIndex(qint64 nextOutputFrameIndex) {
    QMutexLocker locker(&m_mutex);
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingFrameIndexReset = nextOutputFrameIndex;
        m_hasPendingFrameIndexReset = true;
        return;
    }
    waitForDispatchIdleLocked();
    m_dispatcher.resetFrameIndex(nextOutputFrameIndex);
    m_wallStartNs = -1;
}

void OutputRuntime::resetPlayEpoch() {
    QMutexLocker locker(&m_mutex);
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingPlayEpochReset = true;
        return;
    }
    waitForDispatchIdleLocked();
    m_dispatcher.resetPlayEpoch();
}

void OutputRuntime::incrementFenceWaitStalls() {
    QMutexLocker locker(&m_mutex);
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingFenceWaitStalls++;
        return;
    }
    waitForDispatchIdleLocked();
    m_dispatcher.incrementFenceWaitStalls();
}

void OutputRuntime::setGpuRhiContext(std::shared_ptr<GpuRhiContext> gpuRhi) {
    QMutexLocker locker(&m_mutex);
    waitForDispatchIdleLocked();
    m_dispatcher.setGpuRhiContext(std::move(gpuRhi));
}

void OutputRuntime::recordGpuBudget(const GpuBudgetSnapshot& snapshot) {
    QMutexLocker locker(&m_mutex);
    m_gpuVramBytes = qMax<qint64>(0, snapshot.liveBytes);
    m_gpuBudgetBytes = qMax<qint64>(0, snapshot.budgetBytes);
    m_gpuGatedLiveBytes = qMax<qint64>(0, snapshot.gatedLiveBytes);
    m_gpuOomDegrades = qMax<qint64>(0, snapshot.oomDegrades);
    m_gpuBudgetReportOnly = snapshot.reportOnly;
    for (int i = 0; i < kOutputGpuBudgetTagCount; ++i)
        m_gpuLiveBytesByTag[i] = qMax<qint64>(0, snapshot.liveBytesByTag[i]);
}

void OutputRuntime::recordGpuDeviceLossEvents(qint64 events) {
    m_gpuDeviceLossEvents.store(qMax<qint64>(0, events), std::memory_order_release);
}

OutputDispatchStats OutputRuntime::dispatchDueTicksForTest(qint64 wallNowMs) {
    return dispatchDueTicksNs(wallNowMs * kNsPerSecond / 1000);
}

OutputDispatchStats OutputRuntime::dispatchDueTicksForTestNs(qint64 wallNowNs) {
    return dispatchDueTicksNs(wallNowNs);
}

OutputDispatchStats OutputRuntime::stats() const {
    QMutexLocker locker(&m_mutex);
    if (!dispatchActiveOnCurrentThreadLocked()) waitForDispatchIdleLocked();
    return statsLocked();
}

std::shared_ptr<SharedGpuReadbackCache> OutputRuntime::sharedGpuReadbacks() const {
    QMutexLocker locker(&m_mutex);
    if (!dispatchActiveOnCurrentThreadLocked()) waitForDispatchIdleLocked();
    return m_dispatcher.sharedGpuReadbacks();
}

QList<OutputEndpoint> OutputRuntime::outputEndpointsForTest() const {
    QMutexLocker locker(&m_mutex);
    if (!dispatchActiveOnCurrentThreadLocked()) waitForDispatchIdleLocked();
    return m_dispatcher.endpoints();
}

#ifdef OLR_UNIT_TEST
std::shared_ptr<GpuRhiContext> OutputRuntime::gpuRhiContextForTest() const {
    QMutexLocker locker(&m_mutex);
    if (!dispatchActiveOnCurrentThreadLocked()) waitForDispatchIdleLocked();
    return m_dispatcher.gpuRhiContextForTest();
}
#endif

qint64 OutputRuntime::dispatcherNextOutputFrameIndex() const {
    QMutexLocker locker(&m_mutex);
    if (!dispatchActiveOnCurrentThreadLocked()) waitForDispatchIdleLocked();
    return m_dispatcher.nextOutputFrameIndex();
}

qint64 OutputRuntime::outputFrameForPlayheadMs(qint64 playheadMs) const {
    QMutexLocker locker(&m_mutex);
    if (!dispatchActiveOnCurrentThreadLocked()) waitForDispatchIdleLocked();
    return m_dispatcher.outputFrameForPlayheadMs(playheadMs);
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
        quint64 configGeneration = 0;
        {
            QMutexLocker locker(&m_mutex);
            waitForDispatchIdleLocked();
            while (m_reconfiguring && !m_stopRequested)
                m_dispatchIdle.wait(&m_mutex);
            if (m_stopRequested) break;
            const FrameRate rate = m_dispatcher.frameRate();
            frameIndex = m_dispatcher.nextOutputFrameIndex();
            scheduledNs = frameIndexToNsCeil(rate, frameIndex);
            configGeneration = m_configGeneration;
            if (!rate.isValid() || scheduledNs > elapsedNs) {
                return statsLocked();
            }
        }

        OutputRuntimeSnapshot current = snapshot();
        {
            QMutexLocker locker(&m_mutex);
            waitForDispatchIdleLocked();
            while (m_reconfiguring && !m_stopRequested)
                m_dispatchIdle.wait(&m_mutex);
            if (m_stopRequested) break;
            if (m_configGeneration != configGeneration) continue;
            if (m_dispatcher.nextOutputFrameIndex() != frameIndex) continue;
            m_dispatchActive = true;
            m_dispatchThreadId = QThread::currentThreadId();
        }

        m_dispatcher.dispatchTick(current.cache, current.state);
        recordDispatchTiming(frameIndex, scheduledNs, elapsedNs);

        {
            QMutexLocker locker(&m_mutex);
            applyPendingDispatchMutationsLocked();
            m_dispatchActive = false;
            m_dispatchThreadId = nullptr;
            m_dispatchIdle.wakeAll();
        }
        dispatched++;
    }

    QMutexLocker locker(&m_mutex);
    waitForDispatchIdleLocked();
    if (dispatched == m_maxCatchUpTicks) {
        const FrameRate rate = m_dispatcher.frameRate();
        const qint64 cappedTicks =
            qMax<qint64>(0, dueFrameCount(rate, elapsedNs) - m_dispatcher.nextOutputFrameIndex());
        if (cappedTicks > 0) {
            OutputRuntimeDispatchStats runtime = m_dispatcher.stats().runtime;
            runtime.deadlineMisses++;
            runtime.catchUpCapHits++;
            runtime.cappedCatchUpTicks += cappedTicks;
            runtime.lastDispatchDeadlineMiss = true;
            runtime.lastCappedCatchUpTicks = cappedTicks;
            m_dispatcher.setRuntimeStats(runtime);
        }
    }
    return statsLocked();
}

OutputDispatchStats OutputRuntime::statsLocked() const {
    OutputDispatchStats stats = m_dispatcher.stats();
    stats.gpuVramBytes = m_gpuVramBytes;
    stats.gpuBudgetBytes = m_gpuBudgetBytes;
    stats.gpuGatedLiveBytes = m_gpuGatedLiveBytes;
    stats.gpuBudgetReportOnly = m_gpuBudgetReportOnly;
    stats.gpuLiveBytesByTag = m_gpuLiveBytesByTag;
    stats.gpuOomDegrades = m_gpuOomDegrades;
    stats.gpuDeviceLossEvents = m_gpuDeviceLossEvents.load(std::memory_order_acquire);
    return stats;
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
    runtime.lastDispatchDeadlineMiss = false;
    runtime.lastCappedCatchUpTicks = 0;
    m_dispatcher.setRuntimeStats(runtime);
}

bool OutputRuntime::dispatchActiveOnCurrentThreadLocked() const {
    return m_dispatchActive && m_dispatchThreadId == QThread::currentThreadId();
}

void OutputRuntime::applyPendingDispatchMutationsLocked() {
    if (m_hasPendingFrameIndexReset) {
        m_dispatcher.resetFrameIndex(m_pendingFrameIndexReset);
        m_wallStartNs = -1;
        m_hasPendingFrameIndexReset = false;
    }
    if (m_pendingPlayEpochReset) {
        m_dispatcher.resetPlayEpoch();
        m_pendingPlayEpochReset = false;
    }
    if (m_hasPendingIdentitySkip) {
        m_dispatcher.setIdentitySkip(m_pendingIdentitySkip);
        m_hasPendingIdentitySkip = false;
    }
    while (m_pendingFenceWaitStalls > 0) {
        m_dispatcher.incrementFenceWaitStalls();
        --m_pendingFenceWaitStalls;
    }
    if (m_hasPendingEndpoints) {
        m_dispatcher.setEndpoints(m_pendingEndpoints);
        m_pendingEndpoints = {};
        m_hasPendingEndpoints = false;
        ++m_configGeneration;
    }
}

void OutputRuntime::waitForDispatchIdleLocked() const {
    while (m_dispatchActive)
        m_dispatchIdle.wait(&m_mutex);
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

qint64 OutputRuntime::dueFrameCount(FrameRate rate, qint64 elapsedNs) {
    if (!rate.isValid() || elapsedNs < 0) return 0;

    const long double numerator = static_cast<long double>(elapsedNs) * rate.numerator;
    const long double denominator =
        static_cast<long double>(kNsPerSecond) * static_cast<long double>(rate.denominator);
    return static_cast<qint64>(std::floor(numerator / denominator)) + 1;
}
