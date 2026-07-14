// GpuDeviceLossMonitor is the process-wide device-loss latch. recordLoss bumps
// the GpuGenerationCounter (stale-surface invalidation) and sets the latch;
// consumeLossEvent drains events for telemetry without clearing the latch;
// clearForRebuild clears the latch after a successful rebuild while leaving the
// generation bumped (dead surfaces stay stale).
#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"

#include <QSemaphore>

#include <thread>
#include <atomic>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch = capture(),
                            uintptr_t deviceDomainId = 1) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
            deviceDomainId);
    }
    static bool tryLockEpoch() {
        auto& monitor = GpuDeviceLossMonitor::instance();
        if (!monitor.m_epochMutex.try_lock()) return false;
        monitor.m_epochMutex.unlock();
        return true;
    }
};
#endif

class TestDeviceLossMonitor : public QObject {
    Q_OBJECT
private slots:
    void recordLossSetsLatchAndBumpsGeneration();
    void recordLossIsIdempotentUntilRebuildClearsLatch();
    void consumeLossEventDrainsWithoutClearingLatch();
    void clearForRebuildClearsLatchKeepsGeneration();
    void tokenlessLossUpgradesFromSameDeviceProof();
    void tokenlessLossAcceptsAllAuthoritativeDeadDomains();
    void realLossTracksMultipleDeviceDomains();
    void tokenlessLossStaysTokenlessWithoutDriverProof();
    void realLossPublicationIsAtomicWithEpoch();
    void stalePublicationAfterClearIsRejected();
    void rebuildAuthorityRejectsOldDeviceAcceptsReplacement();
    void currentAuthorityPublicationTracksGuardedEpoch();
    void validatedRecoveryRunsOnceForConcurrentWorkers();
    void validatedRecoveryCallbackRunsAfterEpochUnlock();
    void tokenlessRecoveryCallbackRunsAfterEpochUnlock();
    void rebuildInvalidatesUnconsumedRecoveryAuthority();
    void tokenlessEpochCanUpgradeToValidatedRecovery();
    void expandedDeadDomainProofRunsNewRecoveryRevision();
    void differentRecoveryKeysCompleteIndependentlyAndRemainCached();
    void retiredGenerationsPruneCompletedWithoutErasingActiveOrNewer();
    void resetReturnsToPristine();
};

void TestDeviceLossMonitor::validatedRecoveryCallbackRunsAfterEpochUnlock() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA10) != 0);

    QSemaphore callbackEntered;
    QSemaphore releaseCallback;
    GpuValidatedLossResult result;
    std::thread recovery([&]() {
        result = monitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
            callbackEntered.release();
            releaseCallback.acquire();
            return qsizetype(1);
        });
    });
    const bool entered = callbackEntered.tryAcquire(1, 5000);
    std::atomic<bool> epochAvailable{false};
    std::thread probe;
    if (entered) {
        probe = std::thread([&]() {
            epochAvailable.store(GpuDeviceLossMonitorTestAuthority::tryLockEpoch(),
                                 std::memory_order_release);
        });
        probe.join();
    }
    releaseCallback.release();
    recovery.join();

    QVERIFY(entered);
    QVERIFY2(epochAvailable.load(std::memory_order_acquire),
             "validated recovery callback must run after releasing m_epochMutex");
    QCOMPARE(result.status, GpuValidatedLossStatus::Completed);
    monitor.reset();
}

void TestDeviceLossMonitor::tokenlessRecoveryCallbackRunsAfterEpochUnlock() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    monitor.recordSubmissionFailure(0xA09);

    QSemaphore callbackEntered;
    QSemaphore releaseCallback;
    GpuValidatedLossResult result;
    std::thread recovery([&]() {
        result = monitor.withCoordinatedTokenlessRecovery([&]() {
            callbackEntered.release();
            releaseCallback.acquire();
            return qsizetype(0);
        });
    });
    const bool entered = callbackEntered.tryAcquire(1, 5000);
    std::atomic<bool> epochAvailable{false};
    std::thread probe;
    if (entered) {
        probe = std::thread([&]() {
            epochAvailable.store(GpuDeviceLossMonitorTestAuthority::tryLockEpoch(),
                                 std::memory_order_release);
        });
        probe.join();
    }
    releaseCallback.release();
    recovery.join();

    QVERIFY(entered);
    QVERIFY2(epochAvailable.load(std::memory_order_acquire),
             "tokenless recovery callback must run after releasing m_epochMutex");
    QCOMPARE(result.status, GpuValidatedLossStatus::Completed);
    monitor.reset();
}

void TestDeviceLossMonitor::validatedRecoveryRunsOnceForConcurrentWorkers() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA11) != 0);

    std::atomic<int> callbacks{0};
    std::atomic<bool> proofObserved{true};
    GpuValidatedLossResult results[2];
    auto recover = [&](int index) {
        results[index] =
            monitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains& domains) {
                callbacks.fetch_add(1, std::memory_order_acq_rel);
                if (!domains.hasAuthoritativeProof())
                    proofObserved.store(false, std::memory_order_release);
                return qsizetype(7);
            });
    };
    std::thread first(recover, 0);
    std::thread second(recover, 1);
    first.join();
    second.join();

    QCOMPARE(callbacks.load(std::memory_order_acquire), 1);
    QVERIFY(proofObserved.load(std::memory_order_acquire));
    QCOMPARE(results[0].status, GpuValidatedLossStatus::Completed);
    QCOMPARE(results[1].status, GpuValidatedLossStatus::Completed);
    QCOMPARE(results[0].abandoned, qsizetype(7));
    QCOMPARE(results[1].abandoned, qsizetype(7));
    monitor.reset();
}

void TestDeviceLossMonitor::rebuildInvalidatesUnconsumedRecoveryAuthority() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA12) != 0);
    monitor.beginRebuild();

    bool called = false;
    const GpuValidatedLossResult result =
        monitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
            called = true;
            return qsizetype(1);
        });
    QVERIFY(!called);
    QCOMPARE(result.status, GpuValidatedLossStatus::Rejected);
    monitor.reset();
}

void TestDeviceLossMonitor::tokenlessEpochCanUpgradeToValidatedRecovery() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = monitor.recordSubmissionFailure(0xA13);
    bool called = false;
    QCOMPARE(monitor
                 .withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
                     called = true;
                     return qsizetype(1);
                 })
                 .status,
             GpuValidatedLossStatus::Rejected);
    QVERIFY(!called);

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA13), generation);
    // Publication coordinated the upgraded revision. This read is a follower and
    // must not rerun its callback.
    QCOMPARE(monitor
                 .withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
                     called = true;
                     return qsizetype(1);
                 })
                 .status,
             GpuValidatedLossStatus::Completed);
    QVERIFY(!called);
    monitor.reset();
}

void TestDeviceLossMonitor::expandedDeadDomainProofRunsNewRecoveryRevision() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA14);
    int callbacks = 0;
    QCOMPARE(monitor
                 .withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
                     ++callbacks;
                     return qsizetype(callbacks);
                 })
                 .abandoned,
             qsizetype(1));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA15), generation);
    // The late publisher completed revision 2 with the registry callback. A later
    // monitor read follows that exact result instead of invoking this callback.
    QCOMPARE(monitor
                 .withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
                     ++callbacks;
                     return qsizetype(callbacks);
                 })
                 .abandoned,
             qsizetype(0));
    QCOMPARE(callbacks, 1);
    monitor.reset();
}

void TestDeviceLossMonitor::differentRecoveryKeysCompleteIndependentlyAndRemainCached() {
    auto& coordinator = GpuRecoveryCoordinator::instance();
    coordinator.resetForTest();
    QSemaphore oldEntered;
    QSemaphore releaseOld;
    std::atomic<int> oldCallbacks{0};
    std::atomic<int> newCallbacks{0};
    GpuValidatedLossResult oldResult;
    std::thread oldRecovery([&]() {
        oldResult = coordinator.coordinate(0x101, 1, [&]() {
            oldCallbacks.fetch_add(1, std::memory_order_acq_rel);
            oldEntered.release();
            releaseOld.acquire();
            return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(11)};
        });
    });
    const bool oldIsActive = oldEntered.tryAcquire(1, 5000);

    GpuValidatedLossResult newResult;
    if (oldIsActive) {
        newResult = coordinator.coordinate(0x202, 1, [&]() {
            newCallbacks.fetch_add(1, std::memory_order_acq_rel);
            return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(22)};
        });
    }
    releaseOld.release();
    oldRecovery.join();
    const GpuValidatedLossResult newFollower = coordinator.coordinate(0x202, 1, [&]() {
        newCallbacks.fetch_add(1, std::memory_order_acq_rel);
        return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(33)};
    });

    QVERIFY(oldIsActive);
    QCOMPARE(oldResult.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(newResult.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(newResult.abandoned, qsizetype(22));
    QCOMPARE(newFollower.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(newFollower.abandoned, qsizetype(22));
    QCOMPARE(oldCallbacks.load(std::memory_order_acquire), 1);
    QCOMPARE(newCallbacks.load(std::memory_order_acquire), 1);
    coordinator.resetForTest();
}

void TestDeviceLossMonitor::retiredGenerationsPruneCompletedWithoutErasingActiveOrNewer() {
    auto& coordinator = GpuRecoveryCoordinator::instance();
    coordinator.resetForTest();
    QCOMPARE(coordinator
                 .coordinate(0x301, 1,
                             []() {
                                 return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                                               qsizetype(11)};
                             })
                 .status,
             GpuValidatedLossStatus::Completed);
    QCOMPARE(coordinator
                 .coordinate(0x302, 1,
                             []() {
                                 return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                                               qsizetype(22)};
                             })
                 .status,
             GpuValidatedLossStatus::Completed);

    QSemaphore activeEntered;
    QSemaphore releaseActive;
    GpuValidatedLossResult activeResult;
    std::thread active([&]() {
        activeResult = coordinator.coordinate(0x301, 2, [&]() {
            activeEntered.release();
            releaseActive.acquire();
            return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(33)};
        });
    });
    const bool entered = activeEntered.tryAcquire(1, 5000);
    if (entered) coordinator.retireGenerationsThrough(0x301);

    int retiredCallbacks = 0;
    const GpuValidatedLossResult retired = coordinator.coordinate(0x301, 1, [&]() {
        ++retiredCallbacks;
        return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(44)};
    });
    int newerCallbacks = 0;
    const GpuValidatedLossResult newer = coordinator.coordinate(0x302, 1, [&]() {
        ++newerCallbacks;
        return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(55)};
    });
    const size_t cachedWhileActive = coordinator.cachedRecoveryCountForTest();
    releaseActive.release();
    active.join();

    QVERIFY(entered);
    QCOMPARE(retired.status, GpuValidatedLossStatus::Rejected);
    QCOMPARE(retiredCallbacks, 0);
    QCOMPARE(newer.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(newer.abandoned, qsizetype(22));
    QCOMPARE(newerCallbacks, 0);
    QCOMPARE(cachedWhileActive, size_t(2));
    QCOMPARE(activeResult.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(coordinator.cachedRecoveryCountForTest(), size_t(1));
    coordinator.retireGenerationsThrough(0x302);
    QCOMPARE(coordinator.cachedRecoveryCountForTest(), size_t(0));
    coordinator.resetForTest();
}

void TestDeviceLossMonitor::recordLossSetsLatchAndBumpsGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().resetForTest();
    QVERIFY(!m.isLost());
    QCOMPARE(m.lossCount(), uint64_t(0));
    const uint64_t gen = m.recordLoss();
    QVERIFY(m.isLost());
    QCOMPARE(m.lossCount(), uint64_t(1));
    QCOMPARE(gen, GpuGenerationCounter::instance().current());
    QVERIFY(gen >= 2); // a loss advanced the generation
}

void TestDeviceLossMonitor::recordLossIsIdempotentUntilRebuildClearsLatch() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().resetForTest();

    const uint64_t firstGen = m.recordLoss();
    const uint64_t duplicateGen = m.recordLoss();
    QCOMPARE(duplicateGen, firstGen);
    QCOMPARE(m.lossCount(), uint64_t(1));
    QVERIFY(m.consumeLossEvent());
    QVERIFY(!m.consumeLossEvent());

    m.clearForRebuild();
    const uint64_t secondGen = m.recordLoss();
    QVERIFY(secondGen > firstGen);
    QCOMPARE(m.lossCount(), uint64_t(2));
    QVERIFY(m.consumeLossEvent());
}

void TestDeviceLossMonitor::consumeLossEventDrainsWithoutClearingLatch() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    m.recordLoss();
    QVERIFY(m.consumeLossEvent());  // one pending event drained
    QVERIFY(!m.consumeLossEvent()); // none left
    QVERIFY(m.isLost());            // draining does NOT clear the latch
}

void TestDeviceLossMonitor::clearForRebuildClearsLatchKeepsGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().resetForTest();
    m.recordLoss();
    const uint64_t genAfterLoss = GpuGenerationCounter::instance().current();
    m.clearForRebuild();
    QVERIFY(!m.isLost()); // GPU path may resume
    QCOMPARE(GpuGenerationCounter::instance().current(),
             genAfterLoss); // dead surfaces stay stale
}

void TestDeviceLossMonitor::tokenlessLossUpgradesFromSameDeviceProof() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t deviceAuthorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t lossGeneration = m.recordSubmissionFailure(1);
    QVERIFY(!m.realLossToken().has_value());

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(deviceAuthorityEpoch), lossGeneration);
    const auto token = m.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(token->observedGeneration(), lossGeneration);
    QCOMPARE(m.lossCount(), uint64_t(1));
}

void TestDeviceLossMonitor::tokenlessLossAcceptsAllAuthoritativeDeadDomains() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = m.recordSubmissionFailure(11);
    QVERIFY(!m.realLossToken().has_value());

    // Match production poll order when both an RHI device (22) and an import
    // device (11) died in the same reset after domain 11 failed submission.
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 22), generation);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 11), generation);
    const auto tokens = m.realLossTokens();
    QCOMPARE(tokens.size(), size_t(2));
    QCOMPARE(tokens.at(0).deviceDomainId(), uintptr_t(22));
    QCOMPARE(tokens.at(1).deviceDomainId(), uintptr_t(11));
    m.reset();
}

void TestDeviceLossMonitor::realLossTracksMultipleDeviceDomains() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 11);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 22), generation);
    const auto tokens = m.realLossTokens();
    QCOMPARE(tokens.size(), size_t(2));
    QCOMPARE(tokens.at(0).deviceDomainId(), uintptr_t(11));
    QCOMPARE(tokens.at(1).deviceDomainId(), uintptr_t(22));
    m.reset();
}

void TestDeviceLossMonitor::tokenlessLossStaysTokenlessWithoutDriverProof() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    m.recordLoss();
    QVERIFY(!m.realLossToken().has_value());

    m.clearForRebuild();
    QVERIFY(!m.isLost());
    QVERIFY(!m.realLossToken().has_value());

    m.recordLoss();
    QVERIFY(!m.realLossToken().has_value());
    m.reset();
    QVERIFY(!m.isLost());
    QVERIFY(!m.realLossToken().has_value());
}

void TestDeviceLossMonitor::realLossPublicationIsAtomicWithEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish();

    QVERIFY(monitor.isLost());
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(token->observedGeneration(), generation);
}

void TestDeviceLossMonitor::stalePublicationAfterClearIsRejected() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t oldDeviceAuthorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    monitor.recordLoss();
    monitor.clearForRebuild();

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(oldDeviceAuthorityEpoch), uint64_t(0));
    QVERIFY(!monitor.isLost());
    QVERIFY(!monitor.realLossToken().has_value());
}

void TestDeviceLossMonitor::rebuildAuthorityRejectsOldDeviceAcceptsReplacement() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t oldAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    monitor.recordLoss();

    monitor.beginRebuild();
    const uint64_t replacementAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(replacementAuthority != oldAuthority);
    monitor.clearForRebuild();

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(oldAuthority), uint64_t(0));
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(replacementAuthority) != 0);
    QVERIFY(monitor.realLossToken().has_value());
}

void TestDeviceLossMonitor::currentAuthorityPublicationTracksGuardedEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t initialAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(monitor.isCurrentDeviceAuthority(initialAuthority));
    QVERIFY(!monitor.isCurrentDeviceAuthority(0));

    monitor.beginRebuild();
    const uint64_t replacementAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(replacementAuthority != initialAuthority);
    QVERIFY(!monitor.isCurrentDeviceAuthority(initialAuthority));
    QVERIFY(monitor.isCurrentDeviceAuthority(replacementAuthority));

    monitor.clearForRebuild();
    QVERIFY(monitor.isCurrentDeviceAuthority(replacementAuthority));
    monitor.reset();
    const uint64_t resetAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(resetAuthority != replacementAuthority);
    QVERIFY(!monitor.isCurrentDeviceAuthority(replacementAuthority));
    QVERIFY(monitor.isCurrentDeviceAuthority(resetAuthority));
}

void TestDeviceLossMonitor::resetReturnsToPristine() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.recordLoss();
    m.reset();
    QVERIFY(!m.isLost());
    QCOMPARE(m.lossCount(), uint64_t(0));
    QVERIFY(!m.consumeLossEvent());
}

QTEST_GUILESS_MAIN(TestDeviceLossMonitor)
#include "tst_devicelossmonitor.moc"
