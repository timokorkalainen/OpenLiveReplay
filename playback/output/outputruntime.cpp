#include "playback/output/outputruntime.h"

#include "playback/gpu/gpubudget.h"

#include <QDebug>
#include <QElapsedTimer>
#include <cmath>
#include <utility>

namespace {
constexpr qint64 kNsPerSecond = 1000000000;
static_assert(kOutputGpuBudgetTagCount == kGpuBudgetTagCount,
              "OutputDispatchStats GPU tag storage must match GpuBudgetSnapshot tags");

bool latencyTraceEnabled() {
    const QByteArray raw = qgetenv("OLR_E2E_LATENCY_TRACE").trimmed().toLower();
    return !(raw.isEmpty() || raw == "0" || raw == "false" || raw == "off" || raw == "no");
}
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
    refreshCachedStatsLocked();
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
    refreshCachedStatsLocked();
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
        refreshCachedStatsLocked();
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
        refreshCachedStatsLocked();
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
    refreshCachedStatsLocked();
}

void OutputRuntime::resetPlayEpoch() {
    QMutexLocker locker(&m_mutex);
#ifdef OLR_UNIT_TEST
    ++m_playEpochResetCountForTest;
#endif
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingPlayEpochReset = true;
        return;
    }
    waitForDispatchIdleLocked();
    m_dispatcher.resetPlayEpoch();
    refreshCachedStatsLocked();
}

void OutputRuntime::incrementFenceWaitStalls() {
    QMutexLocker locker(&m_mutex);
    if (dispatchActiveOnCurrentThreadLocked()) {
        m_pendingFenceWaitStalls++;
        return;
    }
    waitForDispatchIdleLocked();
    m_dispatcher.incrementFenceWaitStalls();
    refreshCachedStatsLocked();
}

void OutputRuntime::setGpuRhiContext(std::shared_ptr<GpuRhiContext> gpuRhi) {
    QMutexLocker locker(&m_mutex);
    waitForDispatchIdleLocked();
    m_dispatcher.setGpuRhiContext(std::move(gpuRhi));
    refreshCachedStatsLocked();
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

OutputDispatchStats OutputRuntime::dispatchImmediate() {
    return dispatchImmediateWithReport(OutputDispatchRequest{}).stats;
}

OutputDispatchReport
OutputRuntime::dispatchImmediateWithReport(const OutputDispatchRequest& request) {
    const bool traceLatency = latencyTraceEnabled();
    OutputDispatchReport report;
    QElapsedTimer traceTimer;
    if (traceLatency) traceTimer.start();
    qint64 frameIndex = 0;
    quint64 configGeneration = 0;
    qint64 waitReadyNs = 0;
    qint64 snapshotNs = 0;
    qint64 dispatchNs = 0;
    bool immediateRegistered = false;
    auto clearImmediateRequestLocked = [&]() {
        if (!immediateRegistered) return;
        m_immediateDispatchRequests.fetch_sub(1, std::memory_order_acq_rel);
        immediateRegistered = false;
        m_dispatchIdle.wakeAll();
    };

    m_immediateDispatchRequests.fetch_add(1, std::memory_order_acq_rel);
    m_immediateDispatchGeneration.fetch_add(1, std::memory_order_acq_rel);
    immediateRegistered = true;

    {
        QMutexLocker locker(&m_mutex);
        m_dispatchIdle.wakeAll();
        waitForDispatchIdleLocked();
        while (m_reconfiguring && !m_stopRequested)
            m_dispatchIdle.wait(&m_mutex);
        if (m_stopRequested) {
            clearImmediateRequestLocked();
            report.stats = statsLocked();
            return report;
        }
        applyPendingDispatchMutationsLocked();
        frameIndex = m_dispatcher.nextOutputFrameIndex();
        configGeneration = m_configGeneration;
    }
    if (traceLatency) waitReadyNs = traceTimer.nsecsElapsed();

    QElapsedTimer snapshotTimer;
    if (traceLatency) snapshotTimer.start();
    OutputRuntimeSnapshot current = snapshot();
    if (traceLatency) snapshotNs = snapshotTimer.nsecsElapsed();
    bool shouldDispatch = false;
    {
        QMutexLocker locker(&m_mutex);
        waitForDispatchIdleLocked();
        while (m_reconfiguring && !m_stopRequested)
            m_dispatchIdle.wait(&m_mutex);
        if (!m_stopRequested && m_configGeneration == configGeneration &&
            m_dispatcher.nextOutputFrameIndex() == frameIndex) {
            m_dispatchActive = true;
            m_dispatchThreadId = QThread::currentThreadId();
            shouldDispatch = true;
        }
    }

    if (shouldDispatch) {
        QElapsedTimer dispatchTimer;
        if (traceLatency) dispatchTimer.start();
        report = m_dispatcher.dispatchTickWithReport(
            current.cache, current.state, OutputDispatchFlushMode::PausedImmediate, request);
        if (traceLatency) dispatchNs = dispatchTimer.nsecsElapsed();
    }

    {
        QMutexLocker locker(&m_mutex);
        applyPendingDispatchMutationsLocked();
        m_dispatchActive = false;
        m_dispatchThreadId = nullptr;
        clearImmediateRequestLocked();
        m_dispatchIdle.wakeAll();
        if (traceLatency) {
            qInfo().noquote()
                << QStringLiteral(
                       "OLR_LATENCY output.dispatchImmediate frameIndex=%1 shouldDispatch=%2 "
                       "playing=%3 playheadMs=%4 waitReadyNs=%5 snapshotNs=%6 dispatchNs=%7 "
                       "totalNs=%8")
                       .arg(frameIndex)
                       .arg(shouldDispatch ? 1 : 0)
                       .arg(current.state.playing ? 1 : 0)
                       .arg(current.state.playheadMs)
                       .arg(waitReadyNs)
                       .arg(snapshotNs)
                       .arg(dispatchNs)
                       .arg(traceTimer.nsecsElapsed());
        }
        refreshCachedStatsLocked();
        report.stats = cachedStatsLocked();
        return report;
    }
}

OutputDispatchStats OutputRuntime::stats() const {
    QMutexLocker locker(&m_mutex);
    if (m_dispatchActive && !dispatchActiveOnCurrentThreadLocked()) return cachedStatsLocked();
    waitForDispatchIdleLocked();
    refreshCachedStatsLocked();
    return cachedStatsLocked();
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

int OutputRuntime::playEpochResetCountForTest() const {
    QMutexLocker locker(&m_mutex);
    return m_playEpochResetCountForTest;
}

bool OutputRuntime::immediateDispatchPendingForTest() const {
    return m_immediateDispatchRequests.load(std::memory_order_acquire) > 0;
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
    const quint64 immediateGenerationAtEntry =
        m_immediateDispatchGeneration.load(std::memory_order_acquire);
    const bool immediatePendingAtEntry =
        m_immediateDispatchRequests.load(std::memory_order_acquire) > 0;
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
            if (immediatePendingAtEntry ||
                m_immediateDispatchRequests.load(std::memory_order_acquire) > 0 ||
                m_immediateDispatchGeneration.load(std::memory_order_acquire) !=
                    immediateGenerationAtEntry)
                return statsLocked();
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
            if (immediatePendingAtEntry ||
                m_immediateDispatchRequests.load(std::memory_order_acquire) > 0 ||
                m_immediateDispatchGeneration.load(std::memory_order_acquire) !=
                    immediateGenerationAtEntry)
                return statsLocked();
            if (m_configGeneration != configGeneration) continue;
            if (m_dispatcher.nextOutputFrameIndex() != frameIndex) continue;
            m_dispatchActive = true;
            m_dispatchThreadId = QThread::currentThreadId();
        }

        const OutputDispatchFlushMode flushMode = current.state.playing
                                                      ? OutputDispatchFlushMode::Default
                                                      : OutputDispatchFlushMode::PausedPgmCadence;
        m_dispatcher.dispatchTick(current.cache, current.state, flushMode);
        recordDispatchTiming(frameIndex, scheduledNs, elapsedNs);

        {
            QMutexLocker locker(&m_mutex);
            applyPendingDispatchMutationsLocked();
            m_dispatchActive = false;
            m_dispatchThreadId = nullptr;
            refreshCachedStatsLocked();
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
    refreshCachedStatsLocked();
    return cachedStatsLocked();
}

OutputDispatchStats OutputRuntime::statsLocked() const {
    return withRuntimeCountersLocked(m_dispatcher.stats());
}

OutputDispatchStats OutputRuntime::cachedStatsLocked() const {
    return withRuntimeCountersLocked(m_cachedStats);
}

OutputDispatchStats OutputRuntime::withRuntimeCountersLocked(OutputDispatchStats stats) const {
    stats.gpuVramBytes = m_gpuVramBytes;
    stats.gpuBudgetBytes = m_gpuBudgetBytes;
    stats.gpuGatedLiveBytes = m_gpuGatedLiveBytes;
    stats.gpuBudgetReportOnly = m_gpuBudgetReportOnly;
    stats.gpuLiveBytesByTag = m_gpuLiveBytesByTag;
    stats.gpuOomDegrades = m_gpuOomDegrades;
    stats.gpuDeviceLossEvents = m_gpuDeviceLossEvents.load(std::memory_order_acquire);
    return stats;
}

void OutputRuntime::refreshCachedStatsLocked() const {
    m_cachedStats = statsLocked();
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
