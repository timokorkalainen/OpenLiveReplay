#include <QtTest>

#include "playback/frameprovider.h"
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpurecoverycoordinator.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/iosgpulifecyclesink.h"
#include "playback/output/outputdispatcher.h"
#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/qtpreviewsink.h"
#include "playback/output/queuedoutputsink.h"
#ifdef _WIN32
#include "playback/output/win/wingpuimportedge.h"
#endif
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <memory>
#include <atomic>
#include <thread>
#include <utility>

#include <QElapsedTimer>
#include <QProcess>
#include <QSemaphore>
#include <QThread>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch, uintptr_t deviceDomainId) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
            deviceDomainId);
    }
    static void setProofDeliveryGate(QSemaphore* accepted, QSemaphore* proceed) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        monitor.m_proofAcceptedForTest = accepted;
        monitor.m_continueProofDeliveryForTest = proceed;
    }
    static void setRecoveryAttemptingGate(QSemaphore* attempting) {
        GpuDeviceLossMonitor::instance().m_recoveryAttemptingForTest = attempting;
    }
    static void setNoLossUnregisterGate(QSemaphore* entered, QSemaphore* proceed) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        monitor.m_beforeNoLossUnregisterEraseForTest = entered;
        monitor.m_continueNoLossUnregisterEraseForTest = proceed;
    }
    static void setAfterDeliveryLockGate(QSemaphore* entered, QSemaphore* proceed) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        monitor.m_afterDeliveryLockForTest = entered;
        monitor.m_continueAfterDeliveryLockForTest = proceed;
    }
    static void setEpochLockGate(QSemaphore* entered, QSemaphore* proceed) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        monitor.m_epochLockHeldForTest = entered;
        monitor.m_continueEpochLockForTest = proceed;
    }
    static bool proofDeliveryMutexAvailable() {
        auto& mutex = GpuDeviceLossMonitor::instance().m_proofDeliveryMutex;
        if (!mutex.try_lock()) return false;
        mutex.unlock();
        return true;
    }
    static bool epochMutexAvailable() {
        auto& mutex = GpuDeviceLossMonitor::instance().m_epochMutex;
        if (!mutex.try_lock()) return false;
        mutex.unlock();
        return true;
    }
    static size_t participantCount() {
        auto& monitor = GpuDeviceLossMonitor::instance();
        std::lock_guard<std::mutex> lock(monitor.m_epochMutex);
        return monitor.m_recoveryParticipants.size();
    }
};
#endif

class TestGpuDeviceLostWorker : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void lossCountReflectsRecordedEvents();
    void readbackObservedLossRecordsProcessLatch();
    void staleGenerationReadbackDoesNotRecordNewLoss();
    void lostContextRebuildsFreshGpuSpine();
    void rebuildFailureLatchesCpuFallback();
    void repeatedLossesLatchCpuFallbackAfterBudget();
    void backgroundSuspendDefersGpuRebuildUntilForeground();
    void repeatedBackgroundSuspendResumeDoesNotConsumeDeviceLossBudget();
    void lossSanitizesDecoderTrackBuffers();
    void deferredReleasesSurviveGpuWorkerThreadChurn();
    void boundedLossWaitDoesNotHoldRecoveryEpochLock();
    void lateDeadDomainProofReleasesQuarantineAutomatically();
    void tokenlessUpgradeDuringRecoveryReleasesQuarantineAutomatically();
    void acceptedProofDeliveryCompletesBeforeRebuildCanClearState();
    void lateProofPublisherAndWorkerShareExactRecovery();
    void workerLifecycleDoesNotResetAnotherWorkersLoss();
    void deviceLostTeardownCleansRegistryWithoutClearingSurvivor();
    void memoryPressureFallbackCleansValidatedRetainsBeforeUnregister();
    void concurrentRetireRestoreMergesNewEvictionsDuringLoss();
    void runtimeDeviceLossHandoffsUnprovenSanitizedRetirement();
    void memoryPressureFallbackHandoffsIncompleteSanitizedRetirement();
    void terminalPressureCarriersDoNotBlockLaterRecoveryAndRetainExactProof();
    void cleanupOnlyProofReleasesMatchingTerminalCarrierAfterTokenlessClear();
    void cpuFallbackRetriesRejectedParticipantTeardownWithoutRebuild();
    void lateExternalProofWithoutParticipantsCleansAndRetiresEpoch();
    void tokenlessUnregisterKeepsIncompleteRetainUntilOldAuthorityProof();
    void multiParticipantUnregisterCleansBeforeFinalEpochClear();
    void registrationDuringUnregisterCleanupJoinsPendingEpoch();
    void noLossUnregisterSerializesTokenlessPublisher();
    void waitingOldAuthorityPublisherCleansAfterTokenlessUnregister();
    void lossPublishersAcquireDeliveryBeforeEpoch();
    void rebuildInProgressAllowsOtherParticipantUnregister();
    void proofFromOlderRetiredAuthorityCleansAfterNewerEpoch();
    void lossTelemetryIsObservedIndependentlyByEachWorker();
    void blockedRenderThreadPollAndTeardownStayBounded();
    void blockedRenderThreadCoalescesTimedOutLossPolls();
    void shutdownPendingDeviceLossPollQuarantinesOwners();
    void shutdownPollDetectsLossBeforeRetirementDrain();
    void shutdownExactLossReleasesDeadAndQuarantinesMixedUnprovenOwner();
    void shutdownTokenlessLossQuarantinesUnprovenOwner();
    void shutdownAllocationFailureReleasesTerminalOwnersBeforeGpuRoots();
    void terminalHandoffKeepsExactProofUntilMixedOwnersRetire();
    void terminalHandoffAcceptsLateExactProof();
    void exceptionalPreLossTerminalHandoffDoesNotBlockLaterRebuild();
    void terminalHandoffPollsRetainedRhiFromCpuFallback();
    void terminalHandoffPollsOneRetainedContextPerRoundRobinReap();
    void terminalHandoffPollsOneBackendPerReapWithinDualRootSlot();
    void terminalHandoffClearsOneTerminalCarrierPerReap();
    void terminalReaperUsesBoundedWaitInsteadOfCompletedValue();
    void terminalHandoffRetriesRejectedTerminalUnregister();
    void startedWorkerShutdownFailureUsesTerminalOwnerHandoff();
    void shutdownProvenDeadOwnerSkipsFenceWait();
    void rejectedExactCleanupRetriesBeforeRebuild();
    void replacementLossBeforeFailedRebuildCommitRemainsRecoverable();
    void activeLossInitializationDefersWhileLifecycleSuspended();
    void lossRecoveryCommitsSubmittedIdentityAndEpoch();
    void armedCutDeviceLossCommitsRecoveryEpochBeforeHoldLast();
    void pendingSeekDeviceLossPublishesPlaceholderSafeRecovery();
    void pendingSeekMemoryPressurePublishesPlaceholderSafeRecovery();
    void suspendedRecoveryDispatchesCommittedCpuIdentityOnce();

private:
    std::shared_ptr<GpuRhiContext> createTestRhi() const;
    bool installTestGpuSpine(PlaybackWorker& worker) const;
};

namespace {

class TestGpuSurface final : public GpuSurface {
public:
    explicit TestGpuSurface(GpuSurfaceCompatibility compatibility = {})
        : m_compatibility(compatibility) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }
    void* nativeHandle() const override { return reinterpret_cast<void*>(quintptr(0x1)); }

private:
    GpuSurfaceCompatibility m_compatibility;
};

class BlockingLossFence final : public GpuFence {
public:
    BlockingLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t value, int) override {
        waitEntered.release();
        releaseWait.acquire();
        m_completed.store(value, std::memory_order_release);
        return true;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }

    QSemaphore waitEntered;
    QSemaphore releaseWait;

private:
    uint64_t m_signalled = 0;
    std::atomic<uint64_t> m_completed{0};
};

class BlockingIncompleteLossFence final : public GpuFence {
public:
    BlockingIncompleteLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t, int) override {
        waitEntered.release();
        releaseWait.acquire();
        return false;
    }
    uint64_t completedValue() const override { return 0; }

    QSemaphore waitEntered;
    QSemaphore releaseWait;

private:
    uint64_t m_signalled = 0;
};

class IncompleteLossFence final : public GpuFence {
public:
    IncompleteLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t, int) override { return false; }
    uint64_t completedValue() const override { return 0; }

private:
    uint64_t m_signalled = 0;
};

class ObservableLossFence final : public GpuFence {
public:
    ObservableLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch, bool completeOnWait)
        : GpuFence(deviceDomainId, authorityEpoch), m_completeOnWait(completeOnWait) {}

    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t value, int) override {
        m_waitCalls.fetch_add(1, std::memory_order_acq_rel);
        if (m_completed.load(std::memory_order_acquire) >= value) return true;
        if (!m_completeOnWait) return false;
        m_completed.store(value, std::memory_order_release);
        return true;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    int waitCalls() const { return m_waitCalls.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

private:
    const bool m_completeOnWait;
    uint64_t m_signalled = 0;
    std::atomic<uint64_t> m_completed{0};
    std::atomic<int> m_waitCalls{0};
};

class WaitOnlyLossFence final : public GpuFence {
public:
    WaitOnlyLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return 1; }
    bool wait(uint64_t, int timeoutMs) override {
        m_waitCalls.fetch_add(1, std::memory_order_acq_rel);
        return timeoutMs == 0 && m_complete.load(std::memory_order_acquire);
    }
    uint64_t completedValue() const override {
        m_completedCalls.fetch_add(1, std::memory_order_acq_rel);
        throw std::runtime_error("completedValue must not be used by terminal teardown");
    }
    void complete() { m_complete.store(true, std::memory_order_release); }
    void resetCompletedCalls() { m_completedCalls.store(0, std::memory_order_release); }
    int completedCalls() const { return m_completedCalls.load(std::memory_order_acquire); }
    int waitCalls() const { return m_waitCalls.load(std::memory_order_acquire); }

private:
    std::atomic<bool> m_complete{false};
    mutable std::atomic<int> m_completedCalls{0};
    std::atomic<int> m_waitCalls{0};
};

class CompatibleLossSurface final : public GpuSurface {
public:
    CompatibleLossSurface(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : m_compatibility{deviceDomainId, authorityEpoch} {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override { return reinterpret_cast<void*>(0xB10C); }

private:
    GpuSurfaceCompatibility m_compatibility;
};

struct TeardownOrderState {
    std::atomic<int> destroyed{0};
    std::atomic<bool> rhiAliveAtSurfaceDestruction{false};
};

class TeardownOrderSurface final : public GpuSurface {
public:
    TeardownOrderSurface(std::weak_ptr<GpuRhiContext> rhi,
                         std::shared_ptr<TeardownOrderState> state,
                         GpuSurfaceCompatibility compatibility)
        : m_rhi(std::move(rhi)), m_state(std::move(state)), m_compatibility(compatibility) {}
    ~TeardownOrderSurface() override {
        m_state->rhiAliveAtSurfaceDestruction.store(!m_rhi.expired(), std::memory_order_release);
        m_state->destroyed.fetch_add(1, std::memory_order_acq_rel);
    }
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override {
        static int handle = 0;
        return &handle;
    }

private:
    std::weak_ptr<GpuRhiContext> m_rhi{};
    std::shared_ptr<TeardownOrderState> m_state{};
    GpuSurfaceCompatibility m_compatibility{};
};

struct SubmittedAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

class CachedGpuFrameData final : public IFrameData {
public:
    explicit CachedGpuFrameData(CpuPlanes planes)
        : m_planes(std::move(planes)), m_surface(std::make_shared<TestGpuSurface>()) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override {
        m_readToCpuCalls++;
        return CpuPlanes{};
    }
    CpuPlanes cachedCpuPlanes(FramePixelFormat target) const override {
        return target == m_planes.format ? m_planes : CpuPlanes{};
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return m_planes.format; }
    int readToCpuCalls() const { return m_readToCpuCalls; }

private:
    CpuPlanes m_planes;
    std::shared_ptr<TestGpuSurface> m_surface;
    mutable int m_readToCpuCalls = 0;
};

class RecordingNdiSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override {
        if (!m_active) return false;
        frames.append(frame);
        return true;
    }

    QVector<OutputBusFrame> frames;

private:
    bool m_active = false;
};

class PublishLossOnStopSink final : public IOutputSink {
public:
    PublishLossOnStopSink(uint64_t authority, uintptr_t deviceDomain)
        : m_authority(authority), m_deviceDomain(deviceDomain) {}

    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }
    void stop() override {
        if (m_active && m_publishedGeneration == 0) {
            m_publishedGeneration =
                GpuDeviceLossMonitorTestAuthority::publish(m_authority, m_deviceDomain);
        }
        m_active = false;
    }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame&) override { return m_active; }
    uint64_t publishedGeneration() const { return m_publishedGeneration; }

private:
    const uint64_t m_authority;
    const uintptr_t m_deviceDomain;
    bool m_active = false;
    uint64_t m_publishedGeneration = 0;
};

OutputTargetAssignment ndiFeedAssignment() {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::Ndi;
    assignment.enabled = true;
    return assignment;
}

const OutputEndpoint* findEndpoint(const QList<OutputEndpoint>& endpoints, OutputTargetKind kind,
                                   OutputBusId bus) {
    for (const OutputEndpoint& endpoint : endpoints) {
        if (endpoint.assignment.kind == kind && endpoint.assignment.sourceBus == bus)
            return &endpoint;
    }
    return nullptr;
}

const AsyncGpuReadbackSink* asAsyncSink(const OutputEndpoint* endpoint) {
    return endpoint ? dynamic_cast<const AsyncGpuReadbackSink*>(endpoint->sink) : nullptr;
}

CpuPlanes yuvPlanes(int width, int height, uchar y, uchar u, uchar v) {
    return solidYuv420pHandle(width, height, y, u, v).readToCpu();
}

FrameHandle gpuFrame(const std::shared_ptr<GpuRhiContext>& rhi, qint64 ptsMs,
                     uint64_t gpuGeneration, qint64 decodedSequence) {
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = ptsMs;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.format = FramePixelFormat::Nv12;
    meta.gpuGeneration = gpuGeneration;
    meta.decodedSequence = decodedSequence;
    return makeGpuFrameHandle(std::make_shared<TestGpuSurface>(), rhi, meta);
}

FrameHandle placeholderFrame(qint64 ptsMs) {
    FrameHandle frame = solidYuv420pHandle(64, 48, 16, 128, 128);
    frame.metadata().key.feedIndex = 0;
    frame.metadata().key.ptsMs = ptsMs;
    frame.metadata().key.isPlaceholder = true;
    return frame;
}

} // namespace

void TestGpuDeviceLostWorker::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
    setIosGpuLifecycleSink(nullptr);
    GpuDeviceLossMonitorTestAuthority::setNoLossUnregisterGate(nullptr, nullptr);
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(nullptr, nullptr);
    GpuDeviceLossMonitorTestAuthority::setRecoveryAttemptingGate(nullptr);
    GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(nullptr, nullptr);
    GpuDeviceLossMonitorTestAuthority::setEpochLockGate(nullptr, nullptr);
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuDeviceLostWorker::lossCountReflectsRecordedEvents() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();

    OutputDispatchStats stats;
    QCOMPARE(stats.gpuDeviceLossEvents, qint64(0));

    const uint64_t participant = monitor.registerRecoveryParticipant();
    QVERIFY(participant != 0);
    QCOMPARE(monitor.lossCount(), uint64_t(0));
    monitor.recordLoss();
    monitor.recordLoss();
    QCOMPARE(monitor.lossCount(), uint64_t(1));
    QVERIFY(monitor.consumeLossEvent());
    QVERIFY(!monitor.consumeLossEvent());

    QVERIFY(monitor.unregisterRecoveryParticipant(participant).has_value());
    monitor.recordLoss();
    QCOMPARE(monitor.lossCount(), uint64_t(2));
    QVERIFY(monitor.consumeLossEvent());
    QVERIFY(!monitor.consumeLossEvent());
}

void TestGpuDeviceLostWorker::workerLifecycleDoesNotResetAnotherWorkersLoss() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider firstProvider;
    PlaybackTransport firstTransport;
    firstTransport.setFrameRate(25, 1);
    PlaybackWorker first({&firstProvider}, &firstTransport);
    first.initializeOutputGraph(1, 64, 48);
    if (first.m_gpuRecoveryParticipantId == 0) QSKIP("no GPU RHI backend available");
    QVERIFY(first.m_gpuRecoveryParticipantId != 0);

    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t lossGeneration = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA701);
    QVERIFY(lossGeneration != 0);
    const uint64_t lossCount = monitor.lossCount();

    FrameProvider secondProvider;
    PlaybackTransport secondTransport;
    secondTransport.setFrameRate(25, 1);
    PlaybackWorker second({&secondProvider}, &secondTransport);
    second.initializeOutputGraph(1, 64, 48);
    QVERIFY(second.m_gpuRecoveryParticipantId != 0);
    QVERIFY(second.m_gpuRecoveryParticipantId != first.m_gpuRecoveryParticipantId);
    QCOMPARE(GpuGenerationCounter::instance().current(), lossGeneration);
    QCOMPARE(monitor.lossCount(), lossCount);
    QVERIFY(monitor.isLost());
    QVERIFY(monitor.realLossToken().has_value());

    second.shutdownOutputGraph();
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.currentLossGenerationForTest(), lossGeneration);
    QCOMPARE(monitor.lossCount(), lossCount);
    QVERIFY(monitor.realLossToken().has_value());

    first.shutdownOutputGraph();
    monitor.reset();
}

void TestGpuDeviceLostWorker::deviceLostTeardownCleansRegistryWithoutClearingSurvivor() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider firstProvider;
    PlaybackTransport firstTransport;
    firstTransport.setFrameRate(25, 1);
    PlaybackWorker first({&firstProvider}, &firstTransport);
    first.initializeOutputGraph(1, 64, 48);
    if (first.m_gpuRecoveryParticipantId == 0) QSKIP("no GPU RHI backend available");
    FrameProvider survivorProvider;
    PlaybackTransport survivorTransport;
    survivorTransport.setFrameRate(25, 1);
    PlaybackWorker survivor({&survivorProvider}, &survivorTransport);
    survivor.initializeOutputGraph(1, 64, 48);

    constexpr uintptr_t deadDomain = 0xA702;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deadDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deadDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain) != 0);

    first.shutdownOutputGraph();

    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY2(monitor.isLost(), "tearing down one graph must preserve a surviving worker's loss");
    QVERIFY(monitor.realLossToken().has_value());
    QVERIFY(survivor.m_gpuRecoveryParticipantId != 0);

    survivor.shutdownOutputGraph();
    monitor.reset();
}

void TestGpuDeviceLostWorker::memoryPressureFallbackCleansValidatedRetainsBeforeUnregister() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_outputRuntime->stopRuntime();
    monitor.reset();
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    constexpr uintptr_t deadDomain = 0xA704;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deadDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deadDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    PublishLossOnStopSink sink(authority, deadDomain);
    worker.m_outputRuntime->setEndpoints({{ndiFeedAssignment(), &sink}});

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);

    QVERIFY(sink.publishedGeneration() != 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::concurrentRetireRestoreMergesNewEvictionsDuringLoss() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();
    auto firstFence = std::make_shared<ObservableLossFence>(0xA7041, authority, false);
    auto secondFence = std::make_shared<ObservableLossFence>(0xA7042, authority, false);
    FrameMetadata firstMetadata;
    firstMetadata.key.feedIndex = 0;
    firstMetadata.key.ptsMs = 0;
    FrameMetadata secondMetadata = firstMetadata;
    secondMetadata.key.ptsMs = 40;
    auto firstSurface = std::make_shared<TeardownOrderSurface>(
        rhi, std::make_shared<TeardownOrderState>(), GpuSurfaceCompatibility{0xA7041, authority});
    auto secondSurface = std::make_shared<TeardownOrderSurface>(
        rhi, std::make_shared<TeardownOrderState>(), GpuSurfaceCompatibility{0xA7042, authority});
    FrameHandle first = makeGpuFrameHandle(firstSurface, rhi, firstMetadata, firstFence, 1, {});
    FrameHandle second = makeGpuFrameHandle(secondSurface, rhi, secondMetadata, secondFence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    PlaybackWorker worker({&provider}, &transport);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_gpuFrameRetireQueue.collect(first);
    QSemaphore paused;
    QSemaphore proceed;
    worker.m_gpuRetireDrainPausedForTest = &paused;
    worker.m_gpuRetireDrainContinueForTest = &proceed;
    std::thread drainer([&] { worker.drainEvictedGpuFrames(); });
    QVERIFY2(paused.tryAcquire(1, 5000), "retire drain did not reach deterministic pause");
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_gpuFrameRetireQueue.collect(second);
    }
    monitor.recordLoss();
    proceed.release();
    drainer.join();
    worker.m_gpuRetireDrainPausedForTest = nullptr;
    worker.m_gpuRetireDrainContinueForTest = nullptr;

    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 2);
    monitor.reset();
    worker.m_gpuRecoveryParticipantId = 0;
    firstFence->complete(1);
    secondFence->complete(1);
    worker.drainEvictedGpuFrames();
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
}

void TestGpuDeviceLostWorker::runtimeDeviceLossHandoffsUnprovenSanitizedRetirement() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhi;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t deadDomain = 0xA704C;
    constexpr uintptr_t liveDomain = 0xA704D;
    const auto deadState = std::make_shared<TeardownOrderState>();
    const auto liveState = std::make_shared<TeardownOrderState>();
    auto deadFence = std::make_shared<ObservableLossFence>(deadDomain, authority, false);
    auto liveFence = std::make_shared<ObservableLossFence>(liveDomain, authority, false);
    FrameMetadata deadMetadata;
    deadMetadata.key.feedIndex = 0;
    deadMetadata.key.ptsMs = 0;
    FrameMetadata liveMetadata = deadMetadata;
    liveMetadata.key.ptsMs = 40;
    auto deadSurface = std::make_shared<TeardownOrderSurface>(
        rhi, deadState, GpuSurfaceCompatibility{deadDomain, authority});
    auto liveSurface = std::make_shared<TeardownOrderSurface>(
        rhi, liveState, GpuSurfaceCompatibility{liveDomain, authority});
    FrameHandle deadFrame = makeGpuFrameHandle(deadSurface, rhi, deadMetadata, deadFence, 1, {});
    FrameHandle liveFrame = makeGpuFrameHandle(liveSurface, rhi, liveMetadata, liveFence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    std::atomic_store_explicit(&worker.m_gpuRhi, rhi, std::memory_order_release);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    const uint64_t workerParticipant = worker.m_gpuRecoveryParticipantId;
    QVERIFY(workerParticipant != 0);
    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    worker.m_outputCache->insertVideoFrame(placeholderFrame(-40));
    worker.m_outputCache->insertVideoFrame(deadFrame);
    worker.m_outputCache->insertVideoFrame(liveFrame);

    lifecycle.onEnterBackground();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain) != 0);
    deadSurface.reset();
    liveSurface.reset();
    deadFrame = {};
    liveFrame = {};
    rhi.reset();
    worker.handleGpuDeviceLoss();

    QCOMPARE(deadFence->waitCalls(), 0);
    QVERIFY(liveFence->waitCalls() > 0);
    QCOMPARE(deadState->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(liveState->destroyed.load(std::memory_order_acquire), 0);
    QVERIFY(deadState->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(!retainedRoot.expired());
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, workerParticipant);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));
    QVERIFY(monitor.isLost());
    QVERIFY(worker.m_outputCache);
    const QVector<FrameHandle> cpuFrames = worker.m_outputCache->videoFramesSnapshot();
    QCOMPARE(cpuFrames.size(), 1);
    QVERIFY(!cpuFrames.constFirst().isGpuBacked());
    QVERIFY(cpuFrames.constFirst().metadata().key.isPlaceholder);

    liveFence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(liveState->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(liveState->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoot.expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QCOMPARE(worker.m_gpuRecoveryParticipantId, workerParticipant);

    worker.shutdownOutputGraph();
    monitor.reset();
}

void TestGpuDeviceLostWorker::memoryPressureFallbackHandoffsIncompleteSanitizedRetirement() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhi;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA704E;
    const auto state = std::make_shared<TeardownOrderState>();
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    std::atomic_store_explicit(&worker.m_gpuRhi, rhi, std::memory_order_release);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    worker.m_outputCache->insertVideoFrame(placeholderFrame(-40));
    worker.m_outputCache->insertVideoFrame(frame);

    surface.reset();
    frame = {};
    rhi.reset();
    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);

    QVERIFY(fence->waitCalls() > 0);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QVERIFY(!retainedRoot.expired());
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(!monitor.isLost());
    QVERIFY(worker.m_outputCache);
    const QVector<FrameHandle> cpuFrames = worker.m_outputCache->videoFramesSnapshot();
    QCOMPARE(cpuFrames.size(), 1);
    QVERIFY(!cpuFrames.constFirst().isGpuBacked());
    QVERIFY(cpuFrames.constFirst().metadata().key.isPlaceholder);

    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoot.expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestGpuDeviceLostWorker::terminalPressureCarriersDoNotBlockLaterRecoveryAndRetainExactProof() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr std::array<uintptr_t, 2> domains{0xA704F, 0xA7050};
    std::array<std::shared_ptr<TeardownOrderState>, 2> states{
        std::make_shared<TeardownOrderState>(), std::make_shared<TeardownOrderState>()};
    std::array<std::shared_ptr<ObservableLossFence>, 2> fences{
        std::make_shared<ObservableLossFence>(domains[0], authority, false),
        std::make_shared<ObservableLossFence>(domains[1], authority, false)};
    std::array<std::weak_ptr<GpuRhiContext>, 2> retainedRoots;

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    for (size_t index = 0; index < domains.size(); ++index) {
        auto rhiOwner = std::make_shared<int>(0);
        std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
        rhiOwner.reset();
        retainedRoots[index] = rhi;
        FrameMetadata metadata;
        metadata.key.feedIndex = 0;
        metadata.key.ptsMs = qint64(index * 40);
        auto surface = std::make_shared<TeardownOrderSurface>(
            rhi, states[index], GpuSurfaceCompatibility{domains[index], authority});
        FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fences[index], 1, {});
        auto worker =
            std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
        std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
        worker->m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                         std::memory_order_release);
        worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
        QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
        worker->m_gpuFrameRetireQueue.collect(frame);
        surface.reset();
        frame = {};
        rhi.reset();

        worker->evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);
        QCOMPARE(states[index]->destroyed.load(std::memory_order_acquire), 0);
        worker.reset();
    }
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));
    QVERIFY(!retainedRoots[0].expired());
    QVERIFY(!retainedRoots[1].expired());

    const uint64_t recoveryParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(recoveryParticipant != 0);
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, domains[0]);
    QVERIFY(generation != 0);
    const GpuValidatedLossResult delivery = monitor.withValidatedDeadDomains(
        [](const GpuValidatedDeadDomains&) { return qsizetype(0); });
    QCOMPARE(delivery.status, GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(recoveryParticipant, generation));
    const GpuRecoveryTicket ticket = monitor.beginRebuild(recoveryParticipant);

    QVERIFY2(ticket.isValid(), "terminal carriers blocked the later worker rebuild");
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.unregisterRecoveryParticipant(recoveryParticipant).has_value());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 0);

    for (int attempt = 0; attempt < 4; ++attempt)
        PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 0);
    QVERIFY(states[0]->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoots[0].expired());
    QVERIFY(!retainedRoots[1].expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));

    fences[1]->complete(1);
    for (int attempt = 0; attempt < 4; ++attempt)
        PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(states[1]->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoots[1].expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestGpuDeviceLostWorker::cleanupOnlyProofReleasesMatchingTerminalCarrierAfterTokenlessClear() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t oldAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr std::array<uintptr_t, 2> domains{0xA7051, 0xA7052};
    std::array<std::shared_ptr<TeardownOrderState>, 2> states{
        std::make_shared<TeardownOrderState>(), std::make_shared<TeardownOrderState>()};
    std::array<std::shared_ptr<ObservableLossFence>, 2> fences{
        std::make_shared<ObservableLossFence>(domains[0], oldAuthority, false),
        std::make_shared<ObservableLossFence>(domains[1], oldAuthority, false)};
    std::array<std::weak_ptr<GpuRhiContext>, 2> retainedRoots;

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    for (size_t index = 0; index < domains.size(); ++index) {
        auto rhiOwner = std::make_shared<int>(0);
        std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
        rhiOwner.reset();
        retainedRoots[index] = rhi;
        FrameMetadata metadata;
        metadata.key.feedIndex = 0;
        metadata.key.ptsMs = qint64(index * 40);
        auto surface = std::make_shared<TeardownOrderSurface>(
            rhi, states[index], GpuSurfaceCompatibility{domains[index], oldAuthority});
        FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fences[index], 1, {});
        auto worker =
            std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
        std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
        worker->m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                         std::memory_order_release);
        worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
        QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
        worker->m_gpuFrameRetireQueue.collect(frame);
        surface.reset();
        frame = {};
        rhi.reset();

        worker->evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);
        QCOMPARE(states[index]->destroyed.load(std::memory_order_acquire), 0);
        worker.reset();
    }
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));

    const uint64_t recoveryParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(recoveryParticipant != 0);
    const uint64_t tokenlessGeneration = monitor.recordLoss();
    QVERIFY(monitor.acknowledgeRecoveryCleanup(recoveryParticipant, tokenlessGeneration));
    const GpuRecoveryTicket ticket = monitor.beginRebuild(recoveryParticipant);
    QVERIFY(ticket.isValid());
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(!monitor.isLost());
    QVERIFY(GpuDeviceLossMonitorTestAuthority::capture() != oldAuthority);
    QVERIFY(monitor.unregisterRecoveryParticipant(recoveryParticipant).has_value());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));

    // No global retirement-registry entry matches this late old-backend proof.
    // Its zero abandonment result must not prevent terminal proof persistence.
#ifdef _WIN32
    auto importEdge = WinGpuImportEdge::createUnavailableForTest();
    QVERIFY(importEdge);
    monitor.failNextTerminalProofMergeForTest();
    QVERIFY(importEdge->observeDeviceRemovedForTest(DXGI_ERROR_DEVICE_REMOVED, oldAuthority,
                                                    domains[0]));
    QVERIFY2(!importEdge->deviceLostStickyForTest(),
             "failed terminal proof persistence must leave import-edge polling retryable");
    for (int attempt = 0; attempt < 2; ++attempt)
        PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 0);

    QVERIFY(importEdge->observeDeviceRemovedForTest(DXGI_ERROR_DEVICE_REMOVED, oldAuthority,
                                                    domains[0]));
    QVERIFY(importEdge->deviceLostStickyForTest());
#else
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(oldAuthority, domains[0]) != 0);
#endif
    QVERIFY(!monitor.isLost());

    for (int attempt = 0; attempt < 4; ++attempt)
        PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 0);
    QVERIFY(states[0]->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoots[0].expired());
    QVERIFY(!retainedRoots[1].expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));

    fences[1]->complete(1);
    for (int attempt = 0; attempt < 4; ++attempt)
        PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(states[1]->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoots[1].expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestGpuDeviceLostWorker::cpuFallbackRetriesRejectedParticipantTeardownWithoutRebuild() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_outputRuntime->stopRuntime();
    monitor.reset();
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    constexpr uintptr_t deadDomain = 0xA704B;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    PublishLossOnStopSink sink(authority, deadDomain);
    worker.m_outputRuntime->setEndpoints({{ndiFeedAssignment(), &sink}});
    coordinator.failNextAdmissionForTest();

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);
    const uint64_t pendingGeneration = monitor.currentLossGenerationForTest();

    QVERIFY(pendingGeneration != 0);
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    QCOMPARE(worker.m_gpuPendingRecoveryGeneration, pendingGeneration);
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QVERIFY(!worker.m_gpuRhi);

    worker.sampleGpuMemoryPressure(1250);

    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QCOMPARE(worker.m_gpuPendingRecoveryGeneration, uint64_t(0));
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QVERIFY(!worker.m_gpuRhi);
    QVERIFY(!monitor.isLost());
}

void TestGpuDeviceLostWorker::lateExternalProofWithoutParticipantsCleansAndRetiresEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    constexpr uintptr_t deadDomain = 0xA705;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deadDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deadDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    const uint64_t firstGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain);
    QVERIFY(firstGeneration != 0);

    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
    QCOMPARE(GpuRecoveryCoordinator::instance().cachedRecoveryCountForTest(), size_t(0));

    constexpr uintptr_t freshDomain = 0xA708;
    const uint64_t freshAuthority = monitor.currentDeviceAuthorityForTest();
    auto freshFence = std::make_shared<IncompleteLossFence>(freshDomain, freshAuthority);
    auto freshSurface = std::make_shared<CompatibleLossSurface>(freshDomain, freshAuthority);
    GpuOpScope freshOperation(freshFence, registry);
    QCOMPARE(freshOperation
                 .submitRetained(
                     adapter,
                     GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{freshSurface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    const uint64_t freshGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(freshAuthority, freshDomain);
    QVERIFY(freshGeneration > firstGeneration);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(!monitor.isLost());
}

void TestGpuDeviceLostWorker::tokenlessUnregisterKeepsIncompleteRetainUntilOldAuthorityProof() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    constexpr uintptr_t deadDomain = 0xA706;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deadDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deadDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(monitor.recordSubmissionFailure(deadDomain) != 0);

    const std::optional<qsizetype> cleanup = monitor.unregisterRecoveryParticipant(participant);

    QVERIFY(cleanup.has_value());
    QCOMPARE(*cleanup, qsizetype(0));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain) != 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::multiParticipantUnregisterCleansBeforeFinalEpochClear() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    constexpr uintptr_t deadDomain = 0xA707;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t firstParticipant = monitor.registerRecoveryParticipant();
    const uint64_t finalParticipant = monitor.registerRecoveryParticipant();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deadDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deadDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain) != 0);

    const std::optional<qsizetype> firstCleanup =
        monitor.unregisterRecoveryParticipant(firstParticipant);

    QVERIFY(firstCleanup.has_value());
    QCOMPARE(*firstCleanup, qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.currentDeviceAuthorityForTest(), authority);

    const std::optional<qsizetype> finalCleanup =
        monitor.unregisterRecoveryParticipant(finalParticipant);
    QVERIFY(finalCleanup.has_value());
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
}

void TestGpuDeviceLostWorker::registrationDuringUnregisterCleanupJoinsPendingEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    constexpr uintptr_t deviceDomain = 0xA709;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t firstParticipant = monitor.registerRecoveryParticipant();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<BlockingLossFence>(deviceDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QVERIFY(monitor.recordSubmissionFailure(deviceDomain) != 0);

    std::optional<qsizetype> firstCleanup;
    std::thread cleanupThread(
        [&]() { firstCleanup = monitor.unregisterRecoveryParticipant(firstParticipant); });
    const bool cleanupWaiting = fence->waitEntered.tryAcquire(1, 5000);
    const uint64_t joiningParticipant =
        cleanupWaiting ? monitor.registerRecoveryParticipant() : uint64_t(0);
    fence->releaseWait.release();
    cleanupThread.join();

    QVERIFY(cleanupWaiting);
    QVERIFY(firstCleanup.has_value());
    QVERIFY(joiningParticipant != 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.currentDeviceAuthorityForTest(), authority);

    const std::optional<qsizetype> finalCleanup =
        monitor.unregisterRecoveryParticipant(joiningParticipant);
    QVERIFY(finalCleanup.has_value());
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
}

void TestGpuDeviceLostWorker::noLossUnregisterSerializesTokenlessPublisher() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    constexpr uintptr_t deviceDomain = 0xA70A;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deviceDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);

    QSemaphore unregisterEntered;
    QSemaphore continueUnregister;
    GpuDeviceLossMonitorTestAuthority::setNoLossUnregisterGate(&unregisterEntered,
                                                               &continueUnregister);
    std::optional<qsizetype> unregisterResult;
    std::thread unregisterThread(
        [&]() { unregisterResult = monitor.unregisterRecoveryParticipant(participant); });
    const bool entered = unregisterEntered.tryAcquire(1, 5000);
    const bool unregisterHeldDelivery =
        entered && !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
    const bool unregisterHeldEpoch =
        entered && !GpuDeviceLossMonitorTestAuthority::epochMutexAvailable();
    QSemaphore publisherAcquiredDelivery;
    QSemaphore continuePublisher;
    GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(&publisherAcquiredDelivery,
                                                                &continuePublisher);
    uint64_t tokenlessGeneration = 0;
    std::thread publisherThread(
        [&]() { tokenlessGeneration = monitor.recordSubmissionFailure(deviceDomain); });
    continueUnregister.release();
    unregisterThread.join();
    const bool publisherEnteredAfterUnregister = publisherAcquiredDelivery.tryAcquire(1, 5000);
    const bool publisherHeldDelivery =
        publisherEnteredAfterUnregister &&
        !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
    const bool publisherHadNotLockedEpoch =
        publisherEnteredAfterUnregister && GpuDeviceLossMonitorTestAuthority::epochMutexAvailable();
    continuePublisher.release();
    publisherThread.join();
    GpuDeviceLossMonitorTestAuthority::setNoLossUnregisterGate(nullptr, nullptr);
    GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(nullptr, nullptr);

    QVERIFY(entered);
    QVERIFY(unregisterHeldDelivery);
    QVERIFY(unregisterHeldEpoch);
    QVERIFY(publisherEnteredAfterUnregister);
    QVERIFY(publisherHeldDelivery);
    QVERIFY(publisherHadNotLockedEpoch);
    QVERIFY(unregisterResult.has_value());
    QVERIFY(tokenlessGeneration != 0);
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deviceDomain) != 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::waitingOldAuthorityPublisherCleansAfterTokenlessUnregister() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    constexpr uintptr_t deviceDomain = 0xA70B;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<BlockingIncompleteLossFence>(deviceDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QVERIFY(monitor.recordSubmissionFailure(deviceDomain) != 0);

    std::optional<qsizetype> unregisterResult;
    std::thread unregisterThread(
        [&]() { unregisterResult = monitor.unregisterRecoveryParticipant(participant); });
    const bool cleanupWaiting = fence->waitEntered.tryAcquire(1, 5000);
    const bool unregisterHeldDelivery =
        cleanupWaiting && !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
    QSemaphore proofAccepted;
    QSemaphore continueProof;
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(&proofAccepted, &continueProof);
    uint64_t proofGeneration = 0;
    std::thread publisherThread([&]() {
        proofGeneration = GpuDeviceLossMonitorTestAuthority::publish(authority, deviceDomain);
    });
    fence->releaseWait.release();
    unregisterThread.join();
    const bool publisherEnteredAfterUnregister = proofAccepted.tryAcquire(1, 5000);
    const bool publisherHeldDelivery =
        publisherEnteredAfterUnregister &&
        !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
    const bool publisherReleasedEpoch =
        publisherEnteredAfterUnregister && GpuDeviceLossMonitorTestAuthority::epochMutexAvailable();
    continueProof.release();
    publisherThread.join();
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(nullptr, nullptr);

    QVERIFY(cleanupWaiting);
    QVERIFY(unregisterHeldDelivery);
    QVERIFY(publisherEnteredAfterUnregister);
    QVERIFY(publisherHeldDelivery);
    QVERIFY(publisherReleasedEpoch);
    QVERIFY(unregisterResult.has_value());
    QCOMPARE(*unregisterResult, qsizetype(0));
    QVERIFY(proofGeneration != 0);
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::rebuildInProgressAllowsOtherParticipantUnregister() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t rebuildingParticipant = monitor.registerRecoveryParticipant();
    const uint64_t departingParticipant = monitor.registerRecoveryParticipant();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA70C);
    QVERIFY(generation != 0);
    const GpuValidatedLossResult cleanup = monitor.withValidatedDeadDomains(
        [](const GpuValidatedDeadDomains&) { return qsizetype(0); });
    QCOMPARE(cleanup.status, GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(rebuildingParticipant, generation));
    QVERIFY(monitor.acknowledgeRecoveryCleanup(departingParticipant, generation));

    const GpuRecoveryTicket ticket = monitor.beginRebuild(rebuildingParticipant);
    QVERIFY(ticket.isValid());
    monitor.beginRebuild();
    const std::optional<qsizetype> unregisterResult =
        monitor.unregisterRecoveryParticipant(departingParticipant);

    QVERIFY(unregisterResult.has_value());
    QVERIFY(monitor.isLost());
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(!monitor.isLost());
}

void TestGpuDeviceLostWorker::proofFromOlderRetiredAuthorityCleansAfterNewerEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    GpuRetireRegistry registry;
    SubmittedAdapter adapter;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    struct RetiredEpoch {
        uint64_t authority = 0;
        uint64_t generation = 0;
        GpuRetirementDisposition retirement = GpuRetirementDisposition::None;
        bool unregisterCompleted = false;
        bool stillLost = false;
    };
    auto leaveIncompleteTokenlessEpoch = [&](uintptr_t deviceDomain) -> RetiredEpoch {
        const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
        const uint64_t participant = monitor.registerRecoveryParticipant();
        auto fence = std::make_shared<IncompleteLossFence>(deviceDomain, authority);
        auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
        GpuOpScope operation(fence, registry);
        const GpuRetirementDisposition retirement =
            operation
                .submitRetained(adapter,
                                GpuSurfacePack<1>(
                                    std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}))
                .retirement;
        const uint64_t generation = monitor.recordLoss();
        const bool unregisterCompleted =
            monitor.unregisterRecoveryParticipant(participant).has_value();
        return RetiredEpoch{authority, generation, retirement, unregisterCompleted,
                            monitor.isLost()};
    };

    constexpr uintptr_t firstDomain = 0xA70D;
    constexpr uintptr_t secondDomain = 0xA70E;
    const RetiredEpoch first = leaveIncompleteTokenlessEpoch(firstDomain);
    QCOMPARE(first.retirement, GpuRetirementDisposition::Published);
    QVERIFY(first.generation != 0);
    QVERIFY(first.unregisterCompleted);
    QVERIFY(!first.stillLost);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    const RetiredEpoch second = leaveIncompleteTokenlessEpoch(secondDomain);
    QCOMPARE(second.retirement, GpuRetirementDisposition::Published);
    QVERIFY(second.generation != 0);
    QVERIFY(second.unregisterCompleted);
    QVERIFY(!second.stillLost);
    QVERIFY(second.authority != first.authority);
    QVERIFY(second.generation > first.generation);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);

    const uint64_t currentAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t lossCountBeforeProof = monitor.lossCount();
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(first.authority, 0xA70F), uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(currentAuthority + 1000, firstDomain),
             uint64_t(0));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(first.authority, firstDomain) != 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(!monitor.isLost());
    QCOMPARE(monitor.currentLossGenerationForTest(), uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::capture(), currentAuthority);
    QCOMPARE(monitor.lossCount(), lossCountBeforeProof);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(second.authority, secondDomain) != 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::lossPublishersAcquireDeliveryBeforeEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    struct Observation {
        bool epochHeld = false;
        bool deliveryEntered = false;
        bool deliveryOwnedAtGate = false;
        bool deliveryOwnedWhileWaitingForEpoch = false;
        uint64_t generation = 0;
    };
    auto exercise = [&](bool authoritative) {
        monitor.reset();
        GpuGenerationCounter::instance().resetForTest();
        const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
        QSemaphore epochHeld;
        QSemaphore continueEpoch;
        QSemaphore deliveryEntered;
        QSemaphore continueDelivery;
        GpuDeviceLossMonitorTestAuthority::setEpochLockGate(&epochHeld, &continueEpoch);
        std::thread epochHolder([&]() { (void) GpuDeviceLossMonitorTestAuthority::capture(); });
        Observation observation;
        observation.epochHeld = epochHeld.tryAcquire(1, 5000);
        GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(&deliveryEntered,
                                                                    &continueDelivery);
        std::thread publisher([&]() {
            observation.generation =
                authoritative ? GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA70F)
                              : monitor.recordSubmissionFailure(0xA70F);
        });
        observation.deliveryEntered = deliveryEntered.tryAcquire(1, 5000);
        observation.deliveryOwnedAtGate =
            observation.deliveryEntered &&
            !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
        continueDelivery.release();
        observation.deliveryOwnedWhileWaitingForEpoch =
            observation.deliveryEntered &&
            !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
        continueEpoch.release();
        epochHolder.join();
        publisher.join();
        GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(nullptr, nullptr);
        GpuDeviceLossMonitorTestAuthority::setEpochLockGate(nullptr, nullptr);
        return observation;
    };

    const Observation tokenless = exercise(false);
    QVERIFY(tokenless.epochHeld);
    QVERIFY(tokenless.deliveryEntered);
    QVERIFY(tokenless.deliveryOwnedAtGate);
    QVERIFY(tokenless.deliveryOwnedWhileWaitingForEpoch);
    QVERIFY(tokenless.generation != 0);

    const Observation authoritative = exercise(true);
    QVERIFY(authoritative.epochHeld);
    QVERIFY(authoritative.deliveryEntered);
    QVERIFY(authoritative.deliveryOwnedAtGate);
    QVERIFY(authoritative.deliveryOwnedWhileWaitingForEpoch);
    QVERIFY(authoritative.generation != 0);
}

void TestGpuDeviceLostWorker::lossTelemetryIsObservedIndependentlyByEachWorker() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();

    FrameProvider firstProvider;
    PlaybackTransport firstTransport;
    firstTransport.setFrameRate(25, 1);
    PlaybackWorker first({&firstProvider}, &firstTransport);
    first.initializeOutputGraph(1, 64, 48);
    FrameProvider secondProvider;
    PlaybackTransport secondTransport;
    secondTransport.setFrameRate(25, 1);
    PlaybackWorker second({&secondProvider}, &secondTransport);
    second.initializeOutputGraph(1, 64, 48);

    monitor.recordLoss();
    QCOMPARE(first.outputStats().gpuDeviceLossEvents, qint64(1));
    QCOMPARE(second.outputStats().gpuDeviceLossEvents, qint64(1));
    QCOMPARE(first.outputStats().gpuDeviceLossEvents, qint64(1));
    QCOMPARE(second.outputStats().gpuDeviceLossEvents, qint64(1));

    first.shutdownOutputGraph();
    second.shutdownOutputGraph();
    monitor.reset();
}

void TestGpuDeviceLostWorker::blockedRenderThreadPollAndTeardownStayBounded() {
#ifndef _WIN32
    QSKIP("deadline-aware D3D render-thread polling is Windows-specific");
#else
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) QSKIP("no WARP RHI backend available");

    const auto entered = std::make_shared<QSemaphore>();
    const auto release = std::make_shared<QSemaphore>();
    const auto exited = std::make_shared<QSemaphore>();
    const uint64_t quarantineCount = GpuRhiContext::quarantinedContextCountForTest();
    QVERIFY(rhi->queueBlockingRenderJobForTest(entered, release, exited));
    QVERIFY(entered->tryAcquire(1, 5000));

    QElapsedTimer pollElapsed;
    pollElapsed.start();
    QVERIFY(!rhi->pollDeviceLoss());
    QVERIFY2(pollElapsed.elapsed() <= 150, "device-loss polling exceeded its 100 ms budget");

    QElapsedTimer teardownElapsed;
    teardownElapsed.start();
    rhi.reset();
    QVERIFY2(teardownElapsed.elapsed() <= 150, "RHI teardown exceeded its 100 ms budget");
    QCOMPARE(GpuRhiContext::quarantinedContextCountForTest(), quarantineCount + 1);
    release->release();
    QVERIFY2(exited->tryAcquire(1, 5000), "quarantined render job did not exit");
#endif
}

void TestGpuDeviceLostWorker::blockedRenderThreadCoalescesTimedOutLossPolls() {
#ifndef _WIN32
    QSKIP("deadline-aware D3D render-thread polling is Windows-specific");
#else
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) QSKIP("no WARP RHI backend available");

    const auto entered = std::make_shared<QSemaphore>();
    const auto release = std::make_shared<QSemaphore>();
    const auto exited = std::make_shared<QSemaphore>();
    QVERIFY(rhi->queueBlockingRenderJobForTest(entered, release, exited));
    QVERIFY(entered->tryAcquire(1, 5000));

    QVERIFY(!rhi->pollDeviceLoss());
    QVERIFY(!rhi->pollDeviceLoss());
    QCOMPARE(rhi->deviceLossPollExecutionCountForTest(), 0);

    release->release();
    QVERIFY2(exited->tryAcquire(1, 5000), "blocked render job did not exit");
    QTRY_COMPARE_WITH_TIMEOUT(rhi->deviceLossPollExecutionCountForTest(), 1, 5000);
#endif
}

void TestGpuDeviceLostWorker::shutdownPendingDeviceLossPollQuarantinesOwners() {
#ifndef _WIN32
    QSKIP("timed D3D render-thread polling is Windows-specific");
#else
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) QSKIP("no WARP RHI backend available");
    const auto rhiFence = rhi->createFence();
    if (!rhiFence) QSKIP("no WARP fence available");
    const GpuFenceIdentity identity = rhiFence->identity();

    const auto entered = std::make_shared<QSemaphore>();
    const auto release = std::make_shared<QSemaphore>();
    const auto exited = std::make_shared<QSemaphore>();
    QVERIFY(rhi->queueBlockingRenderJobForTest(entered, release, exited));
    QVERIFY(entered->tryAcquire(1, 5000));
    QVERIFY(!rhi->pollDeviceLoss());
    QVERIFY(rhi->deviceLossPollPending());

    const auto state = std::make_shared<TeardownOrderState>();
    auto fence = std::make_shared<ObservableLossFence>(identity.deviceDomainId,
                                                       identity.authorityEpoch, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{identity.deviceDomainId, identity.authorityEpoch});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});
    FrameProvider provider;
    PlaybackTransport transport;
    PlaybackWorker worker({&provider}, &transport);
    std::atomic_store_explicit(&worker.m_gpuRhi, rhi, std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    worker.m_outputCache->insertVideoFrame(frame);
    surface.reset();
    frame = {};

    worker.shutdownOutputGraph();
    QVERIFY(rhi->deviceLossPollPending());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);

    release->release();
    QVERIFY(exited->tryAcquire(1, 5000));
    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
#endif
}

void TestGpuDeviceLostWorker::shutdownPollDetectsLossBeforeRetirementDrain() {
#ifndef _WIN32
    QSKIP("D3D device-removal polling is Windows-specific");
#else
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) QSKIP("no WARP RHI backend available");
    const auto rhiFence = rhi->createFence();
    if (!rhiFence) QSKIP("no D3D fence available");

    const GpuFenceIdentity identity = rhiFence->identity();
    const auto fence =
        std::make_shared<IncompleteLossFence>(identity.deviceDomainId, identity.authorityEpoch);
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface =
        std::make_shared<CompatibleLossSurface>(identity.deviceDomainId, identity.authorityEpoch);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    worker.m_gpuRhi = rhi;
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    const uint64_t lossCount = monitor.lossCount();
    rhi->injectPollOnlyDeviceLostForTest();
    QVERIFY(!rhi->deviceLost());

    worker.shutdownOutputGraph();

    QCOMPARE(monitor.lossCount(), lossCount + 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(!monitor.isLost());
#endif
}

void TestGpuDeviceLostWorker::shutdownExactLossReleasesDeadAndQuarantinesMixedUnprovenOwner() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhi;

    const auto deadState = std::make_shared<TeardownOrderState>();
    const auto liveState = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t deadDomain = 0xA110A;
    constexpr uintptr_t liveDomain = 0xA110B;
    auto deadFence = std::make_shared<ObservableLossFence>(deadDomain, authority, false);
    auto liveFence = std::make_shared<ObservableLossFence>(liveDomain, authority, false);
    FrameMetadata deadMetadata;
    deadMetadata.key.feedIndex = 0;
    deadMetadata.key.ptsMs = 0;
    FrameMetadata liveMetadata = deadMetadata;
    liveMetadata.key.ptsMs = 40;
    auto deadSurface = std::make_shared<TeardownOrderSurface>(
        rhi, deadState, GpuSurfaceCompatibility{deadDomain, authority});
    auto liveSurface = std::make_shared<TeardownOrderSurface>(
        rhi, liveState, GpuSurfaceCompatibility{liveDomain, authority});
    FrameHandle deadFrame = makeGpuFrameHandle(deadSurface, rhi, deadMetadata, deadFence, 1, {});
    FrameHandle liveFrame = makeGpuFrameHandle(liveSurface, rhi, liveMetadata, liveFence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    std::atomic_store_explicit(&worker.m_gpuRhi, rhi, std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_gpuFrameRetireQueue.collect(deadFrame);
    worker.m_gpuFrameRetireQueue.collect(liveFrame);
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 2);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain) != 0);

    deadSurface.reset();
    liveSurface.reset();
    deadFrame = {};
    liveFrame = {};
    rhi.reset();
    worker.shutdownOutputGraph();

    QCOMPARE(deadFence->waitCalls(), 0);
    QVERIFY(liveFence->waitCalls() > 0);
    QCOMPARE(deadState->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(liveState->destroyed.load(std::memory_order_acquire), 0);
    QVERIFY(deadState->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(!retainedRoot.expired());
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(monitor.isLost());

    liveFence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(liveState->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(liveState->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoot.expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestGpuDeviceLostWorker::shutdownTokenlessLossQuarantinesUnprovenOwner() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhi;

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA110D;
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    std::atomic_store_explicit(&worker.m_gpuRhi, rhi, std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_gpuFrameRetireQueue.collect(frame);
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 1);
    monitor.recordLoss();

    surface.reset();
    frame = {};
    rhi.reset();
    worker.shutdownOutputGraph();

    QVERIFY(fence->waitCalls() > 0);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QVERIFY(!retainedRoot.expired());
    QCOMPARE(worker.m_gpuFrameRetireQueue.size(), 0);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(monitor.isLost());

    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(retainedRoot.expired());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestGpuDeviceLostWorker::shutdownAllocationFailureReleasesTerminalOwnersBeforeGpuRoots() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA110C;
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xDEAD) != 0);
    worker->m_outputCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    worker->m_stagingCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    worker->m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;
    GpuRecoveryCoordinator::instance().failNextAdmissionForTest();

    surface.reset();
    frame = {};
    rhi.reset();
    QElapsedTimer teardownElapsed;
    teardownElapsed.start();
    worker.reset();

    QVERIFY2(teardownElapsed.elapsed() < 2000, "exceptional teardown exceeded its bounded wait");
    QVERIFY(fence->waitCalls() > 0);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(monitor.isLost());

    fence->complete(1);
    auto reaper = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    reaper->m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                     std::memory_order_release);
    reaper->m_lastPressureSampleMs = 0;
    reaper->sampleGpuMemoryPressure(250);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    reaper.reset();
    monitor.reset();
}

void TestGpuDeviceLostWorker::terminalHandoffKeepsExactProofUntilMixedOwnersRetire() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();

    const auto deadState = std::make_shared<TeardownOrderState>();
    const auto liveState = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t deadDomain = 0xA1110;
    constexpr uintptr_t liveDomain = 0xA1111;
    auto deadFence = std::make_shared<ObservableLossFence>(deadDomain, authority, false);
    auto liveFence = std::make_shared<ObservableLossFence>(liveDomain, authority, false);
    FrameMetadata deadMetadata;
    deadMetadata.key.feedIndex = 0;
    deadMetadata.key.ptsMs = 0;
    FrameMetadata liveMetadata = deadMetadata;
    liveMetadata.key.ptsMs = 40;
    auto deadSurface = std::make_shared<TeardownOrderSurface>(
        rhi, deadState, GpuSurfaceCompatibility{deadDomain, authority});
    auto liveSurface = std::make_shared<TeardownOrderSurface>(
        rhi, liveState, GpuSurfaceCompatibility{liveDomain, authority});
    FrameHandle deadFrame = makeGpuFrameHandle(deadSurface, rhi, deadMetadata, deadFence, 1, {});
    FrameHandle liveFrame = makeGpuFrameHandle(liveSurface, rhi, liveMetadata, liveFence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain) != 0);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(deadFrame);
    published->insertVideoFrame(liveFrame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;
    GpuRecoveryCoordinator::instance().failNextAdmissionForTest();

    deadSurface.reset();
    liveSurface.reset();
    deadFrame = {};
    liveFrame = {};
    rhi.reset();
    worker.reset();

    QCOMPARE(deadFence->waitCalls(), 0);
    QVERIFY(liveFence->waitCalls() > 0);
    QCOMPARE(deadState->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(liveState->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(monitor.isLost());

    liveFence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(deadState->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(liveState->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(deadState->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QVERIFY(liveState->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestGpuDeviceLostWorker::terminalHandoffAcceptsLateExactProof() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA1112;
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;

    surface.reset();
    frame = {};
    rhi.reset();
    worker.reset();

    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(!monitor.isLost());
    const int waitCallsBeforeProof = fence->waitCalls();

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, domain) != 0);
    QVERIFY(monitor.isLost());
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();

    QCOMPARE(fence->waitCalls(), waitCallsBeforeProof);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestGpuDeviceLostWorker::exceptionalPreLossTerminalHandoffDoesNotBlockLaterRebuild() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA1114;
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto terminalWorker =
        std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&terminalWorker->m_gpuRhi, rhi, std::memory_order_release);
    terminalWorker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(terminalWorker->m_gpuRecoveryParticipantId != 0);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    terminalWorker->m_publishedCache.publish(std::move(published));
    terminalWorker->m_failShutdownAfterPublishedCacheDetachForTest = true;

    surface.reset();
    frame = {};
    rhi.reset();
    terminalWorker.reset();

    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(!monitor.isLost());

    const uint64_t recoveryParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(recoveryParticipant != 0);
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, domain);
    QVERIFY(generation != 0);
    const GpuValidatedLossResult delivery = monitor.withValidatedDeadDomains(
        [](const GpuValidatedDeadDomains&) { return qsizetype(0); });
    QCOMPARE(delivery.status, GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(recoveryParticipant, generation));
    const GpuRecoveryTicket ticket = monitor.beginRebuild(recoveryParticipant);

    QVERIFY2(ticket.isValid(), "exceptional terminal carrier blocked a later worker rebuild");
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.unregisterRecoveryParticipant(recoveryParticipant).has_value());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestGpuDeviceLostWorker::terminalHandoffPollsRetainedRhiFromCpuFallback() {
#ifndef _WIN32
    QSKIP("poll-only retained QRhi device-loss injection is Windows-specific");
#else
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) QSKIP("no WARP RHI backend available");
    const auto rhiFence = rhi->createFence();
    if (!rhiFence) QSKIP("no WARP fence available");
    const GpuFenceIdentity identity = rhiFence->identity();
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhi;

    const auto state = std::make_shared<TeardownOrderState>();
    auto fence = std::make_shared<ObservableLossFence>(identity.deviceDomainId,
                                                       identity.authorityEpoch, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{identity.deviceDomainId, identity.authorityEpoch});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;

    surface.reset();
    frame = {};
    worker.reset();

    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(!monitor.isLost());
    rhi->injectPollOnlyDeviceLostForTest();
    rhi.reset();
    QVERIFY(!retainedRoot.expired());

    PlaybackWorker reaper({&provider}, &transport);
    reaper.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::CpuFallback),
                                    std::memory_order_release);
    reaper.m_lastPressureSampleMs = 0;
    reaper.sampleGpuMemoryPressure(250);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();

    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    QVERIFY(retainedRoot.expired());
    monitor.reset();
#endif
}

void TestGpuDeviceLostWorker::terminalHandoffPollsOneRetainedContextPerRoundRobinReap() {
#ifndef _WIN32
    QSKIP("poll-only retained QRhi device-loss injection is Windows-specific");
#else
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();

    std::array<std::shared_ptr<GpuRhiContext>, 2> rhis;
    std::array<std::weak_ptr<GpuRhiContext>, 2> retainedRoots;
    std::array<std::shared_ptr<TeardownOrderState>, 2> states;
    std::array<std::shared_ptr<ObservableLossFence>, 2> fences;
    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    for (size_t index = 0; index < rhis.size(); ++index) {
        rhis[index] = GpuRhiContext::createWarpForTest();
        if (!rhis[index] || !rhis[index]->isValid()) QSKIP("no WARP RHI backend available");
        const auto rhiFence = rhis[index]->createFence();
        if (!rhiFence) QSKIP("no WARP fence available");
        const GpuFenceIdentity identity = rhiFence->identity();
        retainedRoots[index] = rhis[index];
        states[index] = std::make_shared<TeardownOrderState>();
        fences[index] = std::make_shared<ObservableLossFence>(identity.deviceDomainId,
                                                              identity.authorityEpoch, false);
        FrameMetadata metadata;
        metadata.key.feedIndex = 0;
        metadata.key.ptsMs = qint64(index) * 40;
        auto surface = std::make_shared<TeardownOrderSurface>(
            rhis[index], states[index],
            GpuSurfaceCompatibility{identity.deviceDomainId, identity.authorityEpoch});
        FrameHandle frame =
            makeGpuFrameHandle(surface, rhis[index], metadata, fences[index], 1, {});

        auto worker =
            std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
        std::atomic_store_explicit(&worker->m_gpuRhi, rhis[index], std::memory_order_release);
        worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
        QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
        auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
        published->insertVideoFrame(frame);
        worker->m_publishedCache.publish(std::move(published));
        worker->m_failShutdownAfterPublishedCacheDetachForTest = true;
        surface.reset();
        frame = {};
        worker.reset();
    }

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));
    const int firstBaseline = rhis[0]->deviceLossPollExecutionCountForTest();
    const int secondBaseline = rhis[1]->deviceLossPollExecutionCountForTest();
    rhis[0]->injectPollOnlyDeviceLostForTest();
    rhis[1]->injectPollOnlyDeviceLostForTest();

    PlaybackWorker::resetTerminalGpuOwnerPollCursorForTest();
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(rhis[0]->deviceLossPollExecutionCountForTest() - firstBaseline, 1);
    QCOMPARE(rhis[1]->deviceLossPollExecutionCountForTest() - secondBaseline, 0);
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 0);

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(rhis[0]->deviceLossPollExecutionCountForTest() - firstBaseline, 1);
    QCOMPARE(rhis[1]->deviceLossPollExecutionCountForTest() - secondBaseline, 1);
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 0);

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());

    rhis = {};
    QVERIFY(retainedRoots[0].expired());
    QVERIFY(retainedRoots[1].expired());
    monitor.reset();
#endif
}

void TestGpuDeviceLostWorker::terminalHandoffPollsOneBackendPerReapWithinDualRootSlot() {
#ifndef _WIN32
    QSKIP("dual retained RHI/import roots are Windows-specific");
#else
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) QSKIP("no WARP RHI backend available");
    auto importRoot = WinGpuImportEdge::createUnavailableForTest();
    QVERIFY(importRoot != nullptr);
    const auto rhiFence = rhi->createFence();
    if (!rhiFence) QSKIP("no WARP fence available");
    const GpuFenceIdentity identity = rhiFence->identity();
    auto fence = std::make_shared<ObservableLossFence>(identity.deviceDomainId,
                                                       identity.authorityEpoch, false);
    const auto state = std::make_shared<TeardownOrderState>();
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{identity.deviceDomainId, identity.authorityEpoch});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});
    FrameProvider provider;
    PlaybackTransport transport;
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_winGpuImportEdge = std::move(importRoot);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;
    surface.reset();
    frame = {};
    const int rhiBaseline = rhi->deviceLossPollExecutionCountForTest();
    WinGpuImportEdge::resetDeviceLossPollCountForTest();
    PlaybackWorker::resetTerminalGpuOwnerPollCursorForTest();
    worker.reset();
    QCOMPARE(rhi->deviceLossPollExecutionCountForTest() - rhiBaseline, 1);
    QCOMPARE(WinGpuImportEdge::deviceLossPollCountForTest(), 0);

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(rhi->deviceLossPollExecutionCountForTest() - rhiBaseline, 1);
    QCOMPARE(WinGpuImportEdge::deviceLossPollCountForTest(), 1);

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(rhi->deviceLossPollExecutionCountForTest() - rhiBaseline, 2);
    QCOMPARE(WinGpuImportEdge::deviceLossPollCountForTest(), 1);

    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
#endif
}

void TestGpuDeviceLostWorker::terminalHandoffClearsOneTerminalCarrierPerReap() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();

    std::array<std::shared_ptr<GpuRhiContext>, 2> rhis;
    std::array<std::weak_ptr<GpuRhiContext>, 2> retainedRoots;
    std::array<std::shared_ptr<TeardownOrderState>, 2> states;
    std::array<std::shared_ptr<ObservableLossFence>, 2> fences;
    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    for (size_t index = 0; index < rhis.size(); ++index) {
        auto owner = std::make_shared<int>(0);
        rhis[index] = std::shared_ptr<GpuRhiContext>(owner, nullptr);
        owner.reset();
        retainedRoots[index] = rhis[index];
        states[index] = std::make_shared<TeardownOrderState>();
        const uintptr_t domain = 0xA1120 + index;
        fences[index] = std::make_shared<ObservableLossFence>(domain, authority, false);
        FrameMetadata metadata;
        metadata.key.feedIndex = 0;
        metadata.key.ptsMs = qint64(index) * 40;
        auto surface = std::make_shared<TeardownOrderSurface>(
            rhis[index], states[index], GpuSurfaceCompatibility{domain, authority});
        FrameHandle frame =
            makeGpuFrameHandle(surface, rhis[index], metadata, fences[index], 1, {});

        auto worker =
            std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
        std::atomic_store_explicit(&worker->m_gpuRhi, rhis[index], std::memory_order_release);
        worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
        QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
        auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
        published->insertVideoFrame(frame);
        worker->m_publishedCache.publish(std::move(published));
        worker->m_failShutdownAfterPublishedCacheDetachForTest = true;
        surface.reset();
        frame = {};
        worker.reset();
    }

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(2));
    fences[0]->complete(1);
    fences[1]->complete(1);
    rhis = {};

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire) +
                 states[1]->destroyed.load(std::memory_order_acquire),
             1);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(states[0]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(states[1]->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(retainedRoots[0].expired());
    QVERIFY(retainedRoots[1].expired());
    monitor.reset();
}

void TestGpuDeviceLostWorker::terminalReaperUsesBoundedWaitInsteadOfCompletedValue() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto owner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(owner, nullptr);
    owner.reset();
    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA1130;
    auto fence = std::make_shared<WaitOnlyLossFence>(domain, authority);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});
    FrameProvider provider;
    PlaybackTransport transport;
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;
    surface.reset();
    frame = {};
    rhi.reset();
    worker.reset();

    fence->resetCompletedCalls();
    fence->complete();
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(fence->completedCalls(), 0);
    QVERIFY(fence->waitCalls() > 0);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestGpuDeviceLostWorker::terminalHandoffRetriesRejectedTerminalUnregister() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();
    const std::weak_ptr<GpuRhiContext> retainedRoot = rhi;

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xA1113;
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker->m_publishedCache.publish(std::move(published));
    worker->m_failShutdownAfterPublishedCacheDetachForTest = true;

    surface.reset();
    frame = {};
    rhi.reset();
    worker.reset();

    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(!retainedRoot.expired());

    monitor.failNextTerminalUnregisterForTest();
    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();

    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));
    QVERIFY(!retainedRoot.expired());

    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(retainedRoot.expired());
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestGpuDeviceLostWorker::startedWorkerShutdownFailureUsesTerminalOwnerHandoff() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t unrelatedDomain = 0x51DE;
    auto fence = std::make_shared<ObservableLossFence>(unrelatedDomain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{unrelatedDomain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    std::atomic_store_explicit(&worker.m_gpuRhi, rhi, std::memory_order_release);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    auto published = std::make_shared<OutputFrameCache>(1, 16, 16);
    published->insertVideoFrame(frame);
    worker.m_publishedCache.publish(std::move(published));
    worker.m_failShutdownAfterPublishedCacheDetachForTest = true;
    QSemaphore terminalHandoffCompleted;
    worker.m_gpuTerminalShutdownCompletedForTest = &terminalHandoffCompleted;
    worker.openFile(QStringLiteral("missing-started-worker-shutdown-fixture.ts"));

    surface.reset();
    frame = {};
    rhi.reset();
    QElapsedTimer elapsed;
    elapsed.start();
    worker.start();
    const bool handoffCompleted = terminalHandoffCompleted.tryAcquire(1, 3000);
    worker.stop();
    worker.m_gpuTerminalShutdownCompletedForTest = nullptr;

    QVERIFY2(handoffCompleted, "run-loop terminal owner handoff did not complete");
    QVERIFY2(elapsed.elapsed() < 4000, "started worker did not exit within the bounded handoff");
    QVERIFY(!worker.isRunning());
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 0);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(1));

    fence->complete(1);
    PlaybackWorker::reapTerminalGpuOwnerQuarantineForTest();
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestGpuDeviceLostWorker::shutdownProvenDeadOwnerSkipsFenceWait() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    auto rhiOwner = std::make_shared<int>(0);
    std::shared_ptr<GpuRhiContext> rhi(rhiOwner, nullptr);
    rhiOwner.reset();

    const auto state = std::make_shared<TeardownOrderState>();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    constexpr uintptr_t domain = 0xD34D;
    auto fence = std::make_shared<ObservableLossFence>(domain, authority, false);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    auto surface = std::make_shared<TeardownOrderSurface>(
        rhi, state, GpuSurfaceCompatibility{domain, authority});
    FrameHandle frame = makeGpuFrameHandle(surface, rhi, metadata, fence, 1, {});

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    auto worker = std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&provider}, &transport);
    std::atomic_store_explicit(&worker->m_gpuRhi, rhi, std::memory_order_release);
    worker->m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    QVERIFY(worker->m_gpuRecoveryParticipantId != 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, domain) != 0);
    worker->m_outputCache = std::make_unique<OutputFrameCache>(1, 16, 16);
    worker->m_outputCache->insertVideoFrame(frame);
    worker->m_failShutdownAfterRuntimeDetachForTest = true;
    GpuRecoveryCoordinator::instance().failNextAdmissionForTest();

    surface.reset();
    frame = {};
    rhi.reset();
    QElapsedTimer teardownElapsed;
    teardownElapsed.start();
    worker.reset();

    QVERIFY2(teardownElapsed.elapsed() < 250, "proven-dead owner waited on an unusable fence");
    QCOMPARE(fence->waitCalls(), 0);
    QCOMPARE(state->destroyed.load(std::memory_order_acquire), 1);
    QVERIFY(state->rhiAliveAtSurfaceDestruction.load(std::memory_order_acquire));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestGpuDeviceLostWorker::rejectedExactCleanupRetriesBeforeRebuild() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t deadDomain = 0xD34DCAFE;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deadDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deadDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, deadDomain);
    QVERIFY(generation != 0);
    coordinator.failNextAdmissionForTest();

    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);
    QCOMPARE(worker.m_gpuPendingRecoveryGeneration, generation);
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.realLossTokens().size(), size_t(1));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QCOMPARE(monitor.currentDeviceAuthorityForTest(), authority);

    worker.handleGpuDeviceLoss();

    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.currentDeviceAuthorityForTest() != authority);
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
}

void TestGpuDeviceLostWorker::replacementLossBeforeFailedRebuildCommitRemainsRecoverable() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    QSemaphore beforeCommit;
    QSemaphore continueCommit;
    worker.m_gpuBeforeRecoveryCommitForTest = &beforeCommit;
    worker.m_gpuContinueRecoveryCommitForTest = &continueCommit;

    std::thread recovery([&]() { worker.handleGpuDeviceLoss(); });
    const bool reachedCommitWindow = beforeCommit.tryAcquire(1, 5000);
    const uint64_t oldGeneration = worker.m_gpuPendingRecoveryGeneration;
    const uint64_t replacementAuthority = monitor.currentDeviceAuthorityForTest();
    const uint64_t replacementGeneration =
        reachedCommitWindow
            ? GpuDeviceLossMonitorTestAuthority::publish(replacementAuthority, 0xA703)
            : 0;
    continueCommit.release();
    recovery.join();
    worker.m_gpuBeforeRecoveryCommitForTest = nullptr;
    worker.m_gpuContinueRecoveryCommitForTest = nullptr;

    QVERIFY(reachedCommitWindow);
    QVERIFY(replacementGeneration > oldGeneration);
    QCOMPARE(monitor.currentLossGenerationForTest(), replacementGeneration);
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);

    // The stale clear must leave the worker eligible to process the newer epoch.
    worker.handleGpuDeviceLoss();
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(worker.m_gpuRecoveryParticipantId, uint64_t(0));
    QVERIFY(!monitor.isLost());
}

void TestGpuDeviceLostWorker::activeLossInitializationDefersWhileLifecycleSuspended() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);
    lifecycle.onEnterBackground();
    const uint64_t existingParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(existingParticipant != 0);
    const uint64_t generation = monitor.recordLoss();

    FrameProvider provider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&provider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    QVERIFY(monitor.unregisterRecoveryParticipant(existingParticipant).has_value());

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);
    QCOMPARE(worker.m_gpuPendingRecoveryGeneration, generation);
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    QVERIFY(worker.m_gpuRebuildDeferredForSuspend.load(std::memory_order_acquire));
    QVERIFY(monitor.isLost());

    worker.shutdownOutputGraph();
    QVERIFY(!monitor.isLost());
}

std::shared_ptr<GpuRhiContext> TestGpuDeviceLostWorker::createTestRhi() const {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) rhi = GpuRhiContext::createWarpForTest();
    return rhi;
}

bool TestGpuDeviceLostWorker::installTestGpuSpine(PlaybackWorker& worker) const {
    auto rhi = createTestRhi();
    if (!rhi || !rhi->isValid()) return false;

    auto decodeFence = DecodeDoneFence::create();
    auto renderFence = rhi->createFence();
    auto stagingFence = rhi->createFence();

    worker.m_gpuRhi = std::move(rhi);
    worker.m_decodeFence = std::move(decodeFence);
    worker.m_renderFence = std::move(renderFence);
    worker.m_stagingFence = std::move(stagingFence);
    worker.m_stagedFenceValue.store(0, std::memory_order_release);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    return true;
}

void TestGpuDeviceLostWorker::readbackObservedLossRecordsProcessLatch() {
    GpuGenerationCounter::instance().resetForTest();
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    QVERIFY(participant != 0);

    auto rhi = createTestRhi();
    if (!rhi || !rhi->isValid()) QSKIP("no test RHI backend available");

    rhi->injectDeviceLostForTest();
    const uint64_t gen0 = GpuGenerationCounter::instance().current();
    const auto readbackFence = GpuRhiContextTestAuthority::readbackFenceForTest(rhi);
    QVERIFY(readbackFence);
    const GpuFenceIdentity readbackIdentity = readbackFence->identity();
    const CpuPlanes planes =
        GpuRhiContextTestAuthority::importAndReadback(
            rhi,
            std::make_shared<TestGpuSurface>(GpuSurfaceCompatibility{
                readbackIdentity.deviceDomainId, readbackIdentity.authorityEpoch}),
            FramePixelFormat::Yuv420p)
            .planes;

    QVERIFY(!planes.isValid());
    QVERIFY(monitor.isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
    QVERIFY(monitor.unregisterRecoveryParticipant(participant).has_value());
}

void TestGpuDeviceLostWorker::staleGenerationReadbackDoesNotRecordNewLoss() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    auto rhi = createTestRhi();
    if (!rhi || !rhi->isValid()) QSKIP("no test RHI backend available");

    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    const FrameHandle stale = makeGpuFrameHandle(std::make_shared<TestGpuSurface>(), rhi, meta);

    rhi->injectDeviceLostForTest();
    GpuDeviceLossMonitor::instance().recordLoss();
    GpuDeviceLossMonitor::instance().clearForRebuild();
    const uint64_t stableGeneration = GpuGenerationCounter::instance().current();
    const uint64_t stableLossCount = GpuDeviceLossMonitor::instance().lossCount();

    const CpuPlanes planes = stale.readToCpu(FramePixelFormat::Yuv420p);

    QVERIFY(!planes.isValid());
    QCOMPARE(GpuGenerationCounter::instance().current(), stableGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), stableLossCount);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
}

void TestGpuDeviceLostWorker::lostContextRebuildsFreshGpuSpine() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setFeedPreviewProvidersEnabled(true);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");
    QVERIFY(worker.gpuPathActive());
    QVERIFY(worker.m_gpuRhi);
    QVERIFY(worker.m_decodeFence);
#ifdef _WIN32
    // The Windows render/staging fences are created lazily from the Media
    // Foundation import device, not from the QRhi spine.
    QVERIFY(!worker.m_renderFence);
    QVERIFY(!worker.m_stagingFence);
#else
    QVERIFY(worker.m_renderFence);
    QVERIFY(worker.m_stagingFence);
#endif

    auto lostRhi = worker.m_gpuRhi;
    lostRhi->injectDeviceLostForTest();
    QVERIFY(lostRhi->deviceLost());
    const uint64_t gen0 = GpuGenerationCounter::instance().current();

    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
    QVERIFY(worker.gpuPathActive());
    QVERIFY(worker.m_gpuRhi);
    QVERIFY(worker.m_gpuRhi != lostRhi);
    QVERIFY(!worker.m_gpuRhi->deviceLost());
    QVERIFY(worker.m_decodeFence);
#ifdef _WIN32
    QVERIFY(!worker.m_renderFence);
    QVERIFY(!worker.m_stagingFence);
#else
    QVERIFY(worker.m_renderFence);
    QVERIFY(worker.m_stagingFence);
#endif
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
    const OutputDispatchStats stats = worker.outputStats();
    QCOMPARE(stats.gpuDeviceLossEvents, qint64(1));
}

void TestGpuDeviceLostWorker::rebuildFailureLatchesCpuFallback() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setFeedPreviewProvidersEnabled(true);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    QVERIFY(installTestGpuSpine(worker));
    worker.rebuildOutputEndpoints();

    auto lostRhi = worker.m_gpuRhi;
    lostRhi->injectDeviceLostForTest();
    const uint64_t gen0 = GpuGenerationCounter::instance().current();
    qunsetenv("OLR_GPU_PIPELINE");

    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QVERIFY(!worker.gpuPathActive());
    QVERIFY(!worker.m_gpuRhi);
    QVERIFY(!worker.m_decodeFence);
    QVERIFY(!worker.m_renderFence);
    QVERIFY(!worker.m_stagingFence);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
    const OutputDispatchStats lossStats = worker.outputStats();
    QCOMPARE(lossStats.gpuDeviceLossEvents, qint64(1));
    QCOMPARE(worker.outputStats().gpuDeviceLossEvents, qint64(1));

    qputenv("OLR_GPU_PIPELINE", "1");
    worker.rebuildOutputEndpoints();
    QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
    const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();
    const OutputEndpoint* preview =
        findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::feed(0));
    const OutputEndpoint* ndi =
        findEndpoint(endpoints, OutputTargetKind::Ndi, OutputBusId::feed(0));
    QVERIFY(preview);
    QVERIFY(ndi);
    QVERIFY(!asAsyncSink(preview));
    QVERIFY(!asAsyncSink(ndi));
    QVERIFY(dynamic_cast<const QtPreviewOutputSink*>(preview->sink));
    QVERIFY(dynamic_cast<const QueuedOutputSink*>(ndi->sink));

    const uint64_t stableGeneration = GpuGenerationCounter::instance().current();
    const uint64_t stableLossCount = GpuDeviceLossMonitor::instance().lossCount();
    worker.handleGpuDeviceLoss();
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(GpuGenerationCounter::instance().current(), stableGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), stableLossCount);
}

void TestGpuDeviceLostWorker::repeatedLossesLatchCpuFallbackAfterBudget() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");

    for (int i = 0; i < PlaybackWorker::kDeviceLossRebuildBudget; ++i) {
        QVERIFY(worker.m_gpuRhi);
        worker.m_gpuRhi->injectDeviceLostForTest();
        worker.handleGpuDeviceLoss();
        QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
        QVERIFY(worker.gpuPathActive());
    }

    QVERIFY(worker.m_gpuRhi);
    worker.m_gpuRhi->injectDeviceLostForTest();
    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QVERIFY(!worker.gpuPathActive());
    QVERIFY(!worker.m_gpuRhi);
}

void TestGpuDeviceLostWorker::backgroundSuspendDefersGpuRebuildUntilForeground() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");
    QVERIFY(worker.m_gpuRhi);

    lifecycle.onEnterBackground();
    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);
    QVERIFY(!worker.m_gpuRhi);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());

    lifecycle.onEnterForeground();
    worker.resumeDeferredGpuRebuild();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
    QVERIFY(worker.gpuPathActive());
    QVERIFY(worker.m_gpuRhi);
    QCOMPARE(worker.m_gpuDeviceLossRebuildsRemaining.load(std::memory_order_acquire),
             PlaybackWorker::kDeviceLossRebuildBudget);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
}

void TestGpuDeviceLostWorker::repeatedBackgroundSuspendResumeDoesNotConsumeDeviceLossBudget() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");

    for (int i = 0; i < PlaybackWorker::kDeviceLossRebuildBudget + 1; ++i) {
        lifecycle.onEnterBackground();
        worker.handleGpuDeviceLoss();
        QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);

        lifecycle.onEnterForeground();
        worker.resumeDeferredGpuRebuild();

        QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
        QVERIFY(worker.gpuPathActive());
        QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
        QCOMPARE(worker.m_gpuDeviceLossRebuildsRemaining.load(std::memory_order_acquire),
                 PlaybackWorker::kDeviceLossRebuildBudget);
    }
}

void TestGpuDeviceLostWorker::deferredReleasesSurviveGpuWorkerThreadChurn() {
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("repeatedBackgroundSuspendResumeDoesNotConsumeDeviceLossBudget"),
                 QStringLiteral("boundedLossWaitDoesNotHoldRecoveryEpochLock"),
                 QStringLiteral("-silent")});
    QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(30000), qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void TestGpuDeviceLostWorker::boundedLossWaitDoesNotHoldRecoveryEpochLock() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t deviceDomain = 0xB10C;
    const uint64_t authority = monitor.currentDeviceAuthorityForTest();

    GpuRetireRegistry registry;
    auto fence = std::make_shared<BlockingLossFence>(deviceDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    std::thread recovery([&]() { worker.handleGpuDeviceLoss(); });
    const bool waitStarted = fence->waitEntered.tryAcquire(1, 5000);
    std::atomic<bool> rebuildReturned{false};
    std::thread rebuild;
    if (waitStarted) {
        rebuild = std::thread([&]() {
            monitor.beginRebuild();
            rebuildReturned.store(true, std::memory_order_release);
        });
    }

    QElapsedTimer deadline;
    deadline.start();
    while (waitStarted && !rebuildReturned.load(std::memory_order_acquire) &&
           deadline.elapsed() < 1000)
        QThread::msleep(1);
    const bool rebuildReturnedBeforeFenceRelease = rebuildReturned.load(std::memory_order_acquire);
    fence->releaseWait.release();
    if (rebuild.joinable()) rebuild.join();
    recovery.join();

    QVERIFY(waitStarted);
    QVERIFY2(rebuildReturnedBeforeFenceRelease,
             "bounded fence wait must run after the recovery epoch mutex is released");
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
}

void TestGpuDeviceLostWorker::lateDeadDomainProofReleasesQuarantineAutomatically() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t firstDomain = 0xA110;
    constexpr uintptr_t lateDomain = 0xB220;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto firstFence = std::make_shared<IncompleteLossFence>(firstDomain, authority);
    auto lateFence = std::make_shared<IncompleteLossFence>(lateDomain, authority);
    auto firstSurface = std::make_shared<CompatibleLossSurface>(firstDomain, authority);
    auto lateSurface = std::make_shared<CompatibleLossSurface>(lateDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope firstOperation(firstFence, registry);
    GpuOpScope lateOperation(lateFence, registry);
    QCOMPARE(firstOperation
                 .submitRetained(
                     adapter,
                     GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{firstSurface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(
        lateOperation
            .submitRetained(
                adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{lateSurface}))
            .retirement,
        GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, firstDomain) != 0);
    worker.handleGpuDeviceLoss();
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, lateDomain) != 0);

    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::tokenlessUpgradeDuringRecoveryReleasesQuarantineAutomatically() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t deviceDomain = 0xC330;
    constexpr uintptr_t completingLiveDomain = 0xC440;
    constexpr uintptr_t persistentLiveDomain = 0xC550;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto deadFence = std::make_shared<IncompleteLossFence>(deviceDomain, authority);
    auto completingLiveFence =
        std::make_shared<ObservableLossFence>(completingLiveDomain, authority, true);
    auto persistentLiveFence =
        std::make_shared<ObservableLossFence>(persistentLiveDomain, authority, false);
    auto deadSurface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    auto completingLiveSurface =
        std::make_shared<CompatibleLossSurface>(completingLiveDomain, authority);
    auto persistentLiveSurface =
        std::make_shared<CompatibleLossSurface>(persistentLiveDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope deadOperation(deadFence, registry);
    GpuOpScope completingLiveOperation(completingLiveFence, registry);
    GpuOpScope persistentLiveOperation(persistentLiveFence, registry);
    QCOMPARE(
        deadOperation
            .submitRetained(
                adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{deadSurface}))
            .retirement,
        GpuRetirementDisposition::Published);
    QCOMPARE(
        completingLiveOperation
            .submitRetained(adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{
                                         completingLiveSurface}))
            .retirement,
        GpuRetirementDisposition::Published);
    QCOMPARE(
        persistentLiveOperation
            .submitRetained(adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{
                                         persistentLiveSurface}))
            .retirement,
        GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 3);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    QSemaphore beforeTokenless;
    QSemaphore continueTokenless;
    worker.m_gpuBeforeTokenlessRecoveryEnteredForTest = &beforeTokenless;
    worker.m_gpuContinueTokenlessRecoveryForTest = &continueTokenless;

    std::thread recovery([&]() { worker.handleGpuDeviceLoss(); });
    const bool reachedUpgradeWindow = beforeTokenless.tryAcquire(1, 5000);
    const bool proofPublished = reachedUpgradeWindow && GpuDeviceLossMonitorTestAuthority::publish(
                                                            authority, deviceDomain) != 0;
    const qsizetype pendingAfterPublisher = registry.pendingRetainCount();
    const int completingWaitsAfterPublisher = completingLiveFence->waitCalls();
    const int persistentWaitsAfterPublisher = persistentLiveFence->waitCalls();
    continueTokenless.release();
    recovery.join();
    worker.m_gpuBeforeTokenlessRecoveryEnteredForTest = nullptr;
    worker.m_gpuContinueTokenlessRecoveryForTest = nullptr;

    const qsizetype pendingAfterWorker = registry.pendingRetainCount();
    const int completingWaitsAfterWorker = completingLiveFence->waitCalls();
    const int persistentWaitsAfterWorker = persistentLiveFence->waitCalls();
    const auto stateAfterRejectedCleanup = worker.gpuPipelineState();
    worker.handleGpuDeviceLoss();
    const auto stateAfterRetry = worker.gpuPipelineState();
    const bool completingCleanupPublished =
        GpuDeviceLossMonitorTestAuthority::publish(authority, completingLiveDomain) != 0;
    const bool persistentCleanupPublished =
        GpuDeviceLossMonitorTestAuthority::publish(authority, persistentLiveDomain) != 0;
    const qsizetype pendingAfterCleanup = registry.pendingRetainCount();

    QVERIFY(reachedUpgradeWindow);
    QVERIFY(proofPublished);
    QCOMPARE(stateAfterRejectedCleanup, PlaybackWorker::GpuPipelineState::RebuildPending);
    QCOMPARE(stateAfterRetry, PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(pendingAfterPublisher, pendingBefore + 2);
    QCOMPARE(completingWaitsAfterPublisher, 0);
    QCOMPARE(persistentWaitsAfterPublisher, 0);
    QCOMPARE(pendingAfterWorker, pendingBefore + 2);
    QCOMPARE(completingWaitsAfterWorker, 0);
    QCOMPARE(persistentWaitsAfterWorker, 0);
    QVERIFY(completingCleanupPublished);
    QVERIFY(persistentCleanupPublished);
    QCOMPARE(pendingAfterCleanup, pendingBefore);
}

void TestGpuDeviceLostWorker::acceptedProofDeliveryCompletesBeforeRebuildCanClearState() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t firstDomain = 0xD440;
    constexpr uintptr_t lateDomain = 0xE550;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(lateDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(lateDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QVERIFY(monitor.registerRecoveryParticipant() != 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, firstDomain) != 0);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QSemaphore proofAccepted;
    QSemaphore continueDelivery;
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(&proofAccepted, &continueDelivery);
    std::thread publisher(
        [&]() { (void) GpuDeviceLossMonitorTestAuthority::publish(authority, lateDomain); });
    const bool accepted = proofAccepted.tryAcquire(1, 5000);
    std::atomic<bool> rebuildCleared{false};
    QSemaphore rebuildAttempting;
    std::thread rebuild;
    if (accepted) {
        rebuild = std::thread([&]() {
            rebuildAttempting.release();
            monitor.beginRebuild();
            monitor.clearForRebuild();
            rebuildCleared.store(true, std::memory_order_release);
        });
    }
    const bool rebuildReachedBegin = accepted && rebuildAttempting.tryAcquire(1, 5000);

    QElapsedTimer deadline;
    deadline.start();
    while (rebuildReachedBegin && !rebuildCleared.load(std::memory_order_acquire) &&
           deadline.elapsed() < 250)
        QThread::msleep(1);
    const bool clearedBeforeDelivery = rebuildCleared.load(std::memory_order_acquire);
    continueDelivery.release();
    publisher.join();
    if (rebuild.joinable()) rebuild.join();
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(nullptr, nullptr);

    QVERIFY(accepted);
    QVERIFY(rebuildReachedBegin);
    QVERIFY2(!clearedBeforeDelivery,
             "accepted proof must be delivered before rebuild clears state");
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::lateProofPublisherAndWorkerShareExactRecovery() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t firstDomain = 0xF660;
    constexpr uintptr_t lateDomain = 0xF770;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(lateDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(lateDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(operation
                 .submitRetained(adapter, GpuSurfacePack<1>(
                                              std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuRecoveryParticipantId = monitor.registerRecoveryParticipant();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, firstDomain) != 0);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);

    QSemaphore proofAccepted;
    QSemaphore continueDelivery;
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(&proofAccepted, &continueDelivery);
    GpuRetireRegistry::resetStorageProbeForTest();
    std::thread publisher([&]() {
        GpuRetireRegistry::setStorageProbeEnabledForTest(true);
        (void) GpuDeviceLossMonitorTestAuthority::publish(authority, lateDomain);
        GpuRetireRegistry::setStorageProbeEnabledForTest(false);
    });
    const bool accepted = proofAccepted.tryAcquire(1, 5000);
    const bool publisherHeldDelivery =
        accepted && !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
    const bool publisherReleasedEpoch =
        accepted && GpuDeviceLossMonitorTestAuthority::epochMutexAvailable();

    QSemaphore recoveryAcquiredDelivery;
    QSemaphore continueRecovery;
    GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(&recoveryAcquiredDelivery,
                                                                &continueRecovery);
    std::thread recovery;
    if (accepted) {
        recovery = std::thread([&]() {
            GpuRetireRegistry::setStorageProbeEnabledForTest(true);
            worker.handleGpuDeviceLoss();
            GpuRetireRegistry::setStorageProbeEnabledForTest(false);
        });
    }
    continueDelivery.release();
    publisher.join();
    const bool recoveryEnteredAfterPublisher =
        accepted && recoveryAcquiredDelivery.tryAcquire(1, 5000);
    const bool recoveryHeldDelivery =
        recoveryEnteredAfterPublisher &&
        !GpuDeviceLossMonitorTestAuthority::proofDeliveryMutexAvailable();
    const bool recoveryHadNotLockedEpoch =
        recoveryEnteredAfterPublisher && GpuDeviceLossMonitorTestAuthority::epochMutexAvailable();
    continueRecovery.release();
    if (recovery.joinable()) recovery.join();
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(nullptr, nullptr);
    GpuDeviceLossMonitorTestAuthority::setAfterDeliveryLockGate(nullptr, nullptr);
    const GpuRetireStorageSnapshot storage = GpuRetireRegistry::storageSnapshotForTest();

    QVERIFY(accepted);
    QVERIFY(publisherHeldDelivery);
    QVERIFY(publisherReleasedEpoch);
    QVERIFY(recoveryEnteredAfterPublisher);
    QVERIFY(recoveryHeldDelivery);
    QVERIFY(recoveryHadNotLockedEpoch);
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    // Revision 2 carries both exact domains so a rejected revision 1 cannot be
    // skipped. The domains intentionally hash to distinct retirement shards;
    // the worker follows the cached revision-2 result and adds no third visit.
    QCOMPARE(storage.abandonmentShardVisits, uint64_t(2));
}

void TestGpuDeviceLostWorker::lossSanitizesDecoderTrackBuffers() {
    qunsetenv("OLR_GPU_PIPELINE");
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.width = 16;
    meta.key.height = 16;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    meta.decodedSequence = 7;
    const auto data = std::make_shared<CachedGpuFrameData>(yuvPlanes(16, 16, 80, 90, 100));
    QVERIFY(track->buffer.insert(100, FrameHandle(data, meta), 10, 0, 1000));
    worker.m_decoderBank.append(track);

    worker.handleGpuDeviceLoss();

    FrameHandle recovered;
    int64_t pts = -1;
    QVERIFY(track->buffer.frameAt(100, recovered, pts));
    QCOMPARE(pts, int64_t(100));
    QVERIFY(!recovered.isGpuBacked());
    QCOMPARE(recovered.metadata().decodedSequence, qint64(7));
    QCOMPARE(recovered.metadata().key.ptsMs, qint64(100));
    QCOMPARE(data->readToCpuCalls(), 0);

    worker.m_decoderBank.clear();
    delete track;
}

void TestGpuDeviceLostWorker::lossRecoveryCommitsSubmittedIdentityAndEpoch() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    RecordingNdiSink sink;
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    if (!installTestGpuSpine(worker)) QSKIP("no test RHI backend available");
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);

    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    meta.decodedSequence = 17;
    const auto data = std::make_shared<CachedGpuFrameData>(yuvPlanes(64, 48, 72, 90, 110));
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(FrameHandle(data, meta));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.m_gpuRhi->injectDeviceLostForTest();
    worker.handleGpuDeviceLoss();

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        const std::optional<FrameHandle> recovered = worker.m_outputCache->videoFrameAt(0, 100);
        QVERIFY(recovered.has_value());
        QVERIFY(!recovered->isGpuBacked());
        QVERIFY(worker.recoveredCachePlayheadLocked(100, GpuGenerationCounter::instance().current())
                    .has_value());
    }
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(100));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(100));
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(2));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(1));
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire),
             GpuGenerationCounter::instance().current());
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
        worker.m_outputRuntime->resetFrameIndex(1000000);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setEndpoints({{ndiFeedAssignment(), &sink}});
    }
    worker.m_outputRuntime->dispatchImmediate();
    QCOMPARE(sink.frames.size(), 1);
    const OutputFrameIdentity identity = sink.frames.constFirst().identity;
    QCOMPARE(identity.sampledPlayheadMs, qint64(100));
    QCOMPARE(identity.sourcePtsMs, qint64(100));
    QVERIFY(!identity.videoPlaceholder);
    QCOMPARE(identity.videoGpuGeneration, uint64_t(0));
    QCOMPARE(identity.sourceDecodedSequence, qint64(17));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->setEndpoints({});
    }
}

void TestGpuDeviceLostWorker::armedCutDeviceLossCommitsRecoveryEpochBeforeHoldLast() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);
    transport.setPlaying(true);
    RecordingNdiSink sink;
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 64;
    worker.m_outputHeight = 48;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(1000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);
    QVERIFY(installTestGpuSpine(worker));
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 64, 48);
        worker.m_outputCache->insertVideoFrame(gpuFrame(worker.m_gpuRhi, 1000, lostGeneration, 11));
        worker.publishOutputCacheLocked();
        worker.m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 64, 48);
        worker.m_prerollStagingCache->insertVideoFrame(
            gpuFrame(worker.m_gpuRhi, 2000, lostGeneration, 22));
    }
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 64, 48);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{ndiFeedAssignment(), &sink}});
    }

    worker.m_outputRuntime->dispatchImmediate();
    QCOMPARE(sink.frames.size(), 1);
    QCOMPARE(sink.frames.constFirst().identity.sourceDecodedSequence, qint64(11));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->resetFrameIndex(1000000);
    }
    worker.m_scheduledCutFrame.store(1, std::memory_order_release);
    worker.m_scheduledCutTargetMs.store(2000, std::memory_order_release);
    worker.m_armSeekGen.store(2, std::memory_order_release);
    worker.m_stagingCovers.store(true, std::memory_order_release);
    worker.m_cutArmed.store(true, std::memory_order_release);
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }
    const uint64_t generationBeforeLoss = GpuGenerationCounter::instance().current();
    const uint64_t lossCountBefore = GpuDeviceLossMonitor::instance().lossCount();
    worker.m_gpuRecoveryParticipantId =
        GpuDeviceLossMonitor::instance().registerRecoveryParticipant();
    QVERIFY(worker.m_gpuRecoveryParticipantId != 0);
    worker.m_gpuRhi->injectDeviceLostForTest();
    QVERIFY(worker.m_gpuRhi->deviceLost());
    const uint64_t recoveryGeneration = GpuDeviceLossMonitor::instance().recordLoss();
    QCOMPARE(recoveryGeneration, generationBeforeLoss + 1);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());

    worker.m_outputRuntime->dispatchImmediate();

    QCOMPARE(GpuGenerationCounter::instance().current(), recoveryGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), recoveryGeneration);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }
    QCOMPARE(sink.frames.size(), 2);
    QVERIFY(sink.frames.constLast().identity.videoPlaceholder);
    QVERIFY(!sink.frames.constLast().video.isGpuBacked());
    QVERIFY(sink.frames.constLast().identity.sourceDecodedSequence != qint64(11));
    QCOMPARE(worker.cutsFired(), 0);
    QVERIFY(!worker.m_cutArmed.load(std::memory_order_acquire));
    QCOMPARE(worker.m_scheduledCutFrame.load(std::memory_order_acquire), qint64(-1));

    worker.makeOutputSnapshot();
    QCOMPARE(GpuGenerationCounter::instance().current(), recoveryGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }

    worker.handleGpuDeviceLoss();
    QCOMPARE(GpuGenerationCounter::instance().current(), recoveryGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    QCOMPARE(worker.cutsFired(), 0);
    QVERIFY(!worker.m_cutArmed.load(std::memory_order_acquire));
    worker.m_outputRuntime->setEndpoints({});
}

void TestGpuDeviceLostWorker::pendingSeekDeviceLossPublishesPlaceholderSafeRecovery() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_outputRuntime->stopRuntime();
    QVERIFY(installTestGpuSpine(worker));
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(gpuFrame(worker.m_gpuRhi, 0, lostGeneration, 31));
        worker.m_outputCache->insertVideoFrame(placeholderFrame(100));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.handleGpuDeviceLoss();
    const uint64_t recoveryGeneration = GpuGenerationCounter::instance().current();

    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), recoveryGeneration);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    QVERIFY(!published->videoFrameAtFreshForGeneration(0, 0, recoveryGeneration).has_value());
    for (const FrameHandle& frame : published->videoFramesSnapshot())
        QVERIFY(!frame.isGpuBacked());
}

void TestGpuDeviceLostWorker::pendingSeekMemoryPressurePublishesPlaceholderSafeRecovery() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_outputRuntime->stopRuntime();
    QVERIFY(installTestGpuSpine(worker));
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(gpuFrame(worker.m_gpuRhi, 0, lostGeneration, 41));
        worker.m_outputCache->insertVideoFrame(placeholderFrame(100));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);
    const uint64_t recoveryGeneration = GpuGenerationCounter::instance().current();

    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), recoveryGeneration);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    QVERIFY(!published->videoFrameAtFreshForGeneration(0, 0, recoveryGeneration).has_value());
    for (const FrameHandle& frame : published->videoFramesSnapshot())
        QVERIFY(!frame.isGpuBacked());
}

void TestGpuDeviceLostWorker::suspendedRecoveryDispatchesCommittedCpuIdentityOnce() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    FrameProvider feedProvider;
    QObject previewConsumer;
    feedProvider.addDirectPreviewConsumer(&previewConsumer);
    QSignalSpy frameSpy(&feedProvider, &FrameProvider::frameChanged);
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    QVERIFY(installTestGpuSpine(worker));
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(1, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(100, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(100, std::memory_order_release);
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.gpuGeneration = lostGeneration;
    meta.decodedSequence = 51;
    const auto data = std::make_shared<CachedGpuFrameData>(yuvPlanes(64, 48, 76, 92, 108));
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(FrameHandle(data, meta));
        worker.publishOutputCacheLocked();
    }
    worker.setFeedPreviewProvidersEnabled(true);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->resetFrameIndex(1000000);
    }
    lifecycle.onEnterBackground();

    worker.handleGpuDeviceLoss();

    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(100));
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    const std::optional<FrameHandle> recovered = published->videoFrameAt(0, 100);
    QVERIFY(recovered.has_value());
    QVERIFY(!recovered->isGpuBacked());
    QCOMPARE(recovered->metadata().decodedSequence, qint64(51));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        const OutputEndpoint* preview =
            findEndpoint(worker.m_outputRuntime->outputEndpointsForTest(),
                         OutputTargetKind::QtPreview, OutputBusId::feed(0));
        QVERIFY(preview);
        QVERIFY(!asAsyncSink(preview));
    }

    lifecycle.onEnterForeground();
    worker.resumeDeferredGpuRebuild();
    worker.m_outputRuntime->stopRuntime();
    QCOMPARE(frameSpy.count(), 1);
}

QTEST_GUILESS_MAIN(TestGpuDeviceLostWorker)
#include "tst_gpu_devicelost_worker.moc"
