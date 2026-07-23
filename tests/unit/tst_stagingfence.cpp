#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static size_t participantCount() {
        auto& monitor = GpuDeviceLossMonitor::instance();
        std::lock_guard<std::mutex> lock(monitor.m_epochMutex);
        return monitor.m_recoveryParticipants.size();
    }
};
#endif

class TestStagingFence : public QObject {
    Q_OBJECT
private slots:
    void swapWaitsForStagingFence();
    void workerCutDefersUntilStagingFenceCompletes();
    void shutdownForceDrainsGpuRetireQueueBeforeDroppingFence();
    void forceDrainPreservesUnretiredFramesAfterBoundedFenceFailure();
    void shutdownHandsOffUnretiredFramesAfterBoundedFenceFailure();
    void forceDrainPreservesUnretiredFramesWhenFencesThrow();
    void shutdownHandsOffUnretiredFramesWhenFenceThrows();
    void repeatedPublishDoesNotRetireUnchangedSnapshot();
    void windowsGpuImportRequiresBothFences();
    void markStagingCoveredSignalsFenceForGpuPath();
};

class ManualFence final : public GpuFence {
public:
    uint64_t signal() override { return m_completed.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override { return completedValue() >= value; }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

private:
    std::atomic<uint64_t> m_completed{0};
};

class CompletingFence final : public GpuFence {
public:
    uint64_t signal() override { return m_completed.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override {
        waitCalls++;
        m_completed.store(value, std::memory_order_release);
        return true;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }

    int waitCalls = 0;

private:
    std::atomic<uint64_t> m_completed{0};
};

class FailingFence final : public GpuFence {
public:
    uint64_t signal() override { return 1; }
    bool wait(uint64_t value, int) override {
        waitCalls++;
        return m_completed.load(std::memory_order_acquire) >= value;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

    int waitCalls = 0;

private:
    std::atomic<uint64_t> m_completed{0};
};

class ThrowingFence final : public GpuFence {
public:
    enum class ThrowPoint { CompletedValue, Wait };

    explicit ThrowingFence(ThrowPoint throwPoint) : m_throwPoint(throwPoint) {}

    uint64_t signal() override { return 1; }
    bool wait(uint64_t, int) override {
        waitCalls++;
        if (m_completed.load(std::memory_order_acquire)) return true;
        if (m_throwPoint == ThrowPoint::Wait) throw std::runtime_error("wait failed");
        return false;
    }
    uint64_t completedValue() const override {
        if (m_completed.load(std::memory_order_acquire)) return 1;
        if (m_throwPoint == ThrowPoint::CompletedValue)
            throw std::runtime_error("completedValue failed");
        return 0;
    }
    void complete() { m_completed.store(true, std::memory_order_release); }

    int waitCalls = 0;

private:
    ThrowPoint m_throwPoint;
    std::atomic<bool> m_completed{false};
};

class RetireSurface final : public GpuSurface {
public:
    explicit RetireSurface(uint64_t pendingFence) { retainUntilFenceRetired(pendingFence); }

    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 4, 4}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
};

class RetireFrameData final : public IFrameData {
public:
    RetireFrameData(std::shared_ptr<RetireSurface> surface, std::shared_ptr<GpuFence> fence,
                    uint64_t value)
        : m_surface(std::move(surface)), m_synchronization{std::move(fence), value, true} {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override { return {}; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    GpuFrameSynchronization gpuSynchronization() const override { return m_synchronization; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<RetireSurface> m_surface;
    GpuFrameSynchronization m_synchronization;
};

FrameHandle retireGpuFrame(uint64_t pendingFence, const std::shared_ptr<GpuFence>& fence) {
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = qint64(pendingFence);
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    return FrameHandle(std::make_shared<RetireFrameData>(
                           std::make_shared<RetireSurface>(pendingFence), fence, pendingFence),
                       meta);
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const QByteArray& value)
        : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)), m_previous(qgetenv(name)) {
        qputenv(m_name, value);
    }

    ~ScopedEnv() {
        if (m_hadValue)
            qputenv(m_name, m_previous);
        else
            qunsetenv(m_name);
    }

private:
    const char* m_name = nullptr;
    bool m_hadValue = false;
    QByteArray m_previous;
};

void TestStagingFence::swapWaitsForStagingFence() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");

    QVERIFY(!fence->wait(1, 50));
    const uint64_t staged = fence->signal();
    // Keep this a real backend wait, but allow for first-submit latency under
    // TSan and shared/virtualized CI GPUs.
    QVERIFY(fence->wait(staged, 5000));
}

void TestStagingFence::workerCutDefersUntilStagingFenceCompletes() {
    ScopedEnv gpuEnabled("OLR_GPU_PIPELINE", "1");
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto stagingFence = std::make_shared<ManualFence>();

    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_cutArmed.store(true, std::memory_order_release);
    worker.m_stagingCovers.store(true, std::memory_order_release);
    worker.m_scheduledCutFrame.store(5, std::memory_order_release);
    worker.m_scheduledCutTargetMs.store(1000, std::memory_order_release);
    worker.m_armSeekGen.store(worker.m_seekGeneration.load(std::memory_order_acquire),
                              std::memory_order_release);
    worker.m_stagingFence = stagingFence;
    worker.m_stagedFenceValue.store(1, std::memory_order_release);

    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.maybeFireScheduledCut(5);
    }
    QCOMPARE(worker.cutsFired(), 0);
    QCOMPARE(worker.m_scheduledCutFrame.load(std::memory_order_acquire), qint64(5));
    QCOMPARE(transport.currentPos(), int64_t(0));

    stagingFence->complete(1);
    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.maybeFireScheduledCut(5);
    }
    QCOMPARE(worker.cutsFired(), 1);
    QCOMPARE(worker.m_scheduledCutFrame.load(std::memory_order_acquire), qint64(-1));
    QCOMPARE(transport.currentPos(), int64_t(1000));
}

void TestStagingFence::shutdownForceDrainsGpuRetireQueueBeforeDroppingFence() {
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto fence = std::make_shared<CompletingFence>();

    worker.m_renderFence = fence;
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(1, fence));
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(2, fence));
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 2);

    worker.shutdownOutputGraph();

    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(fence->waitCalls, 2);
    QVERIFY(worker.m_renderFence == nullptr);
}

void TestStagingFence::forceDrainPreservesUnretiredFramesAfterBoundedFenceFailure() {
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto fence = std::make_shared<FailingFence>();

    worker.m_renderFence = fence;
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(1, fence));

    QVERIFY(!worker.forceDrainEvictedGpuFrames());

    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 1);
    QVERIFY(fence->waitCalls > 0);

    fence->complete(1);
    QVERIFY(worker.forceDrainEvictedGpuFrames());
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
}

void TestStagingFence::shutdownHandsOffUnretiredFramesAfterBoundedFenceFailure() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto fence = std::make_shared<FailingFence>();
    auto rootOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhiRoot(rootOwner, nullptr);
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhiRoot;
    rootOwner.reset();

    worker.m_renderFence = fence;
    std::atomic_store_explicit(&worker.m_gpuRhi, rhiRoot, std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(1, fence));

    worker.shutdownOutputGraph();

    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    rhiRoot.reset();
    QVERIFY(!retainedRoot.expired());

    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(retainedRoot.expired());
    monitor.reset();
}

void TestStagingFence::forceDrainPreservesUnretiredFramesWhenFencesThrow() {
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto completedValueFence =
        std::make_shared<ThrowingFence>(ThrowingFence::ThrowPoint::CompletedValue);
    auto waitFence = std::make_shared<ThrowingFence>(ThrowingFence::ThrowPoint::Wait);
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(1, completedValueFence));
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(2, waitFence));

    bool threw = false;
    bool drained = true;
    try {
        drained = worker.forceDrainEvictedGpuFrames();
    } catch (...) {
        threw = true;
    }

    QVERIFY(!threw);
    QVERIFY(!drained);
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 2);

    completedValueFence->complete();
    waitFence->complete();
    QVERIFY(worker.forceDrainEvictedGpuFrames());
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
}

void TestStagingFence::shutdownHandsOffUnretiredFramesWhenFenceThrows() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto fence = std::make_shared<ThrowingFence>(ThrowingFence::ThrowPoint::Wait);
    auto rootOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhiRoot(rootOwner, nullptr);
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhiRoot;
    rootOwner.reset();

    worker.m_renderFence = fence;
    std::atomic_store_explicit(&worker.m_gpuRhi, rhiRoot, std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_gpuFrameRetireQueue.collect(retireGpuFrame(1, fence));

    bool threw = false;
    try {
        worker.shutdownOutputGraph();
    } catch (...) {
        threw = true;
    }

    QVERIFY(!threw);
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    rhiRoot.reset();
    QVERIFY(!retainedRoot.expired());

    fence->complete();
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(retainedRoot.expired());
    monitor.reset();
}

void TestStagingFence::repeatedPublishDoesNotRetireUnchangedSnapshot() {
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto fence = std::make_shared<ManualFence>();

    worker.m_renderFence = fence;
    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_outputCache->insertVideoFrame(retireGpuFrame(1, fence));

    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.publishOutputCacheLocked();
        worker.publishOutputCacheLocked();
    }

    const int queuedForRetire = worker.m_gpuFrameRetireQueue.size();
    fence->complete(1);
    worker.forceDrainEvictedGpuFrames();
    QCOMPARE(queuedForRetire, 0);
}

void TestStagingFence::windowsGpuImportRequiresBothFences() {
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);

    QVERIFY(!worker.ensureWindowsGpuImportFencesReadyForDecode());
}

void TestStagingFence::markStagingCoveredSignalsFenceForGpuPath() {
    ScopedEnv gpuEnabled("OLR_GPU_PIPELINE", "1");
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto fence = std::make_shared<ManualFence>();

    worker.m_stagingFence = fence;
    worker.m_stagedFenceValue.store(0, std::memory_order_release);

    worker.markStagingCovered();

    QVERIFY(worker.m_stagingCovers.load(std::memory_order_acquire));
    QCOMPARE(worker.m_stagedFenceValue.load(std::memory_order_acquire), uint64_t(1));
}

QTEST_GUILESS_MAIN(TestStagingFence)
#include "tst_stagingfence.moc"
