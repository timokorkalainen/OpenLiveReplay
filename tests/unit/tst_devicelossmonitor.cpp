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
#include <new>

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
    static void replaceLossGeneration(uint64_t generation) {
        GpuDeviceLossMonitor::instance().m_lossGeneration.store(generation,
                                                                std::memory_order_release);
    }
    static void failNextLossPreparation() {
        GpuDeviceLossMonitor::instance().m_failNextLossPreparationForTest = true;
    }
    static void failRegistrationAfterPrimaryInsert() {
        GpuDeviceLossMonitor::instance().m_failRegistrationAfterPrimaryInsertForTest = true;
    }
    static size_t participantCount() {
        auto& monitor = GpuDeviceLossMonitor::instance();
        std::lock_guard<std::mutex> lock(monitor.m_epochMutex);
        return monitor.m_recoveryParticipants.size();
    }
    static bool isTerminalParticipant(uint64_t participantId) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        std::lock_guard<std::mutex> lock(monitor.m_epochMutex);
        return monitor.m_terminalRecoveryParticipants.count(participantId) != 0;
    }
    static bool isCleanupAcknowledged(uint64_t participantId) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        std::lock_guard<std::mutex> lock(monitor.m_epochMutex);
        return monitor.m_cleanupAcknowledgedParticipants.count(participantId) != 0;
    }
    static size_t lastImmediateProofSize() {
        return GpuDeviceLossMonitor::instance().m_lastImmediateProofSizeForTest;
    }
    static void advanceProofRevisionWithoutDelivery() {
        auto& monitor = GpuDeviceLossMonitor::instance();
        std::lock_guard<std::mutex> lock(monitor.m_epochMutex);
        ++monitor.m_realLossRevision;
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
    void coordinatedParticipantsAdvanceAuthorityOnceAndClearAfterAllAcknowledge();
    void terminalObserverPersistsProofAcrossEpochsWithoutBlockingRebuild();
    void replacementAuthorityWaitsForEveryOldDomainCleanup();
    void lateGraphJoinsRecoveryWithoutOwningAnOldDevice();
    void noDeviceRegistrationRemainsCleanupAcknowledgedIfLossStartsImmediately();
    void replacementDeviceLossRestartsRecoveryEpoch();
    void staleRecoveryTicketCannotClearNewerLoss();
    void validatedRecoveryRunsOnceForConcurrentWorkers();
    void validatedRecoveryCallbackRunsAfterEpochUnlock();
    void validatedRecoveryRejectsMismatchedLossGeneration();
    void tokenlessRecoveryCallbackRunsAfterEpochUnlock();
    void rebuildInvalidatesUnconsumedRecoveryAuthority();
    void tokenlessEpochCanUpgradeToValidatedRecovery();
    void expandedDeadDomainProofRunsNewRecoveryRevision();
    void coordinatorAdmissionFailureIsContainedAndRetryable();
    void rejectedCoordinatorWorkIsNotCached();
    void rejectedAttemptWaiterKeepsOriginalAttemptIdentity();
    void undeliveredRealProofBlocksRecoveryCommit();
    void undeliveredZeroParticipantProofBlocksLegacyRecoveryCommit();
    void staleTicketCannotClearNewerProofRevision();
    void zeroParticipantTokenlessCleanupRetriesSameRevision();
    void zeroParticipantRealProofRetriesSameRevision();
    void expandedProofRetryDeliversCompleteRevision();
    void lossPreparationFailureDoesNotAdvanceGenerationOrLatch();
    void registrationFailureRollsBackPrimaryParticipant();
    void terminalPromotionIsTransactionalAndIdempotent();
    void differentRecoveryKeysCompleteIndependentlyAndRemainCached();
    void retiredGenerationsPruneCompletedWithoutErasingActiveOrNewer();
    void resetReturnsToPristine();
};

void TestDeviceLossMonitor::
    coordinatedParticipantsAdvanceAuthorityOnceAndClearAfterAllAcknowledge() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t firstParticipant = monitor.registerRecoveryParticipant();
    const uint64_t secondParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(firstParticipant != 0);
    QVERIFY(secondParticipant != 0);
    QVERIFY(firstParticipant != secondParticipant);

    const uint64_t initialAuthority = monitor.currentDeviceAuthorityEpoch();
    const uint64_t lossGeneration = monitor.recordLoss();
    QVERIFY(monitor.acknowledgeRecoveryCleanup(firstParticipant, lossGeneration));
    QVERIFY(monitor.acknowledgeRecoveryCleanup(secondParticipant, lossGeneration));
    const GpuRecoveryTicket first = monitor.beginRebuild(firstParticipant);
    const uint64_t replacementAuthority = monitor.currentDeviceAuthorityEpoch();
    const GpuRecoveryTicket second = monitor.beginRebuild(secondParticipant);

    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QCOMPARE(first.lossGeneration(), lossGeneration);
    QCOMPARE(second.lossGeneration(), lossGeneration);
    QCOMPARE(first.authorityEpoch(), replacementAuthority);
    QCOMPARE(second.authorityEpoch(), replacementAuthority);
    QVERIFY(replacementAuthority != initialAuthority);
    QCOMPARE(monitor.currentDeviceAuthorityEpoch(), replacementAuthority);

    QVERIFY(monitor.clearForRebuild(first));
    QVERIFY(monitor.isLost());
    QVERIFY(monitor.clearForRebuild(second));
    QVERIFY(!monitor.isLost());

    monitor.unregisterRecoveryParticipant(firstParticipant);
    monitor.unregisterRecoveryParticipant(secondParticipant);
    monitor.reset();
}

void TestDeviceLossMonitor::terminalObserverPersistsProofAcrossEpochsWithoutBlockingRebuild() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t terminalParticipant = monitor.registerTerminalRecoveryParticipant();
    const uint64_t workerParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(terminalParticipant != 0);
    QVERIFY(workerParticipant != 0);

    const uint64_t tokenlessGeneration = monitor.recordLoss();
    QVERIFY(monitor.acknowledgeRecoveryCleanup(workerParticipant, tokenlessGeneration));
    const GpuRecoveryTicket tokenlessTicket = monitor.beginRebuild(workerParticipant);
    QVERIFY(tokenlessTicket.isValid());
    QVERIFY(monitor.clearForRebuild(tokenlessTicket));
    QVERIFY(!monitor.isLost());
    QVERIFY(!monitor.authorizesTerminalDeadDomain(
        terminalParticipant, GpuSurfaceCompatibility{0xA7701, tokenlessTicket.authorityEpoch()}));

    constexpr uintptr_t firstDomain = 0xA7701;
    const uint64_t firstAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t firstGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(firstAuthority, firstDomain);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(workerParticipant, firstGeneration));
    const GpuRecoveryTicket firstTicket = monitor.beginRebuild(workerParticipant);
    QVERIFY(firstTicket.isValid());
    QVERIFY(monitor.clearForRebuild(firstTicket));
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.authorizesTerminalDeadDomain(
        terminalParticipant, GpuSurfaceCompatibility{firstDomain, firstAuthority}));
    QVERIFY(!monitor.authorizesTerminalDeadDomain(
        terminalParticipant, GpuSurfaceCompatibility{firstDomain + 1, firstAuthority}));

    constexpr uintptr_t secondDomain = 0xA7702;
    const uint64_t secondAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t secondGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(secondAuthority, secondDomain);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(workerParticipant, secondGeneration));
    const GpuRecoveryTicket secondTicket = monitor.beginRebuild(workerParticipant);
    QVERIFY(secondTicket.isValid());
    QVERIFY(monitor.clearForRebuild(secondTicket));
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.authorizesTerminalDeadDomain(
        terminalParticipant, GpuSurfaceCompatibility{firstDomain, firstAuthority}));
    QVERIFY(monitor.authorizesTerminalDeadDomain(
        terminalParticipant, GpuSurfaceCompatibility{secondDomain, secondAuthority}));

    QVERIFY(monitor.unregisterTerminalRecoveryParticipant(terminalParticipant));
    QVERIFY(!monitor.authorizesTerminalDeadDomain(
        terminalParticipant, GpuSurfaceCompatibility{firstDomain, firstAuthority}));
    QVERIFY(monitor.unregisterRecoveryParticipant(workerParticipant).has_value());
    monitor.reset();
}

void TestDeviceLossMonitor::staleRecoveryTicketCannotClearNewerLoss() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t oldParticipant = monitor.registerRecoveryParticipant();
    const uint64_t oldGeneration = monitor.recordLoss();
    QVERIFY(monitor.acknowledgeRecoveryCleanup(oldParticipant, oldGeneration));
    const GpuRecoveryTicket stale = monitor.beginRebuild(oldParticipant);
    QVERIFY(stale.isValid());
    QCOMPARE(stale.lossGeneration(), oldGeneration);

    // Teardown is an acknowledgement for the old participant. A later graph and
    // loss must not be clearable by work that retained the old immutable ticket.
    monitor.unregisterRecoveryParticipant(oldParticipant);
    QVERIFY(!monitor.isLost());
    const uint64_t newParticipant = monitor.registerRecoveryParticipant();
    const uint64_t newGeneration = monitor.recordLoss();
    QVERIFY(newGeneration > oldGeneration);

    QVERIFY(!monitor.clearForRebuild(stale));
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.currentLossGenerationForTest(), newGeneration);

    QVERIFY(monitor.acknowledgeRecoveryCleanup(newParticipant, newGeneration));
    const GpuRecoveryTicket current = monitor.beginRebuild(newParticipant);
    QVERIFY(current.isValid());
    QVERIFY(monitor.clearForRebuild(current));
    QVERIFY(!monitor.isLost());
    monitor.unregisterRecoveryParticipant(newParticipant);
    monitor.reset();
}

void TestDeviceLossMonitor::replacementAuthorityWaitsForEveryOldDomainCleanup() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t firstParticipant = monitor.registerRecoveryParticipant();
    const uint64_t delayedParticipant = monitor.registerRecoveryParticipant();
    const uint64_t oldAuthority = monitor.currentDeviceAuthorityEpoch();
    const uint64_t generation = monitor.recordLoss();

    QVERIFY(monitor.acknowledgeRecoveryCleanup(firstParticipant, generation));
    QVERIFY(!monitor.beginRebuild(firstParticipant).isValid());
    QCOMPARE(monitor.currentDeviceAuthorityEpoch(), oldAuthority);

    // The delayed participant still owns an old-authority device. Its proof must
    // remain acceptable until it has polled and acknowledged local cleanup.
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(oldAuthority, 0xA16), generation);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(delayedParticipant, generation));

    const GpuRecoveryTicket delayed = monitor.beginRebuild(delayedParticipant);
    const GpuRecoveryTicket first = monitor.beginRebuild(firstParticipant);
    QVERIFY(delayed.isValid());
    QVERIFY(first.isValid());
    QVERIFY(monitor.currentDeviceAuthorityEpoch() != oldAuthority);
    QCOMPARE(delayed.authorityEpoch(), first.authorityEpoch());
    QVERIFY(monitor.clearForRebuild(delayed));
    QVERIFY(monitor.clearForRebuild(first));
    QVERIFY(!monitor.isLost());
    monitor.unregisterRecoveryParticipant(firstParticipant);
    monitor.unregisterRecoveryParticipant(delayedParticipant);
    monitor.reset();
}

void TestDeviceLossMonitor::lateGraphJoinsRecoveryWithoutOwningAnOldDevice() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t oldParticipant = monitor.registerRecoveryParticipant();
    const uint64_t generation = monitor.recordLoss();
    QVERIFY(monitor.acknowledgeRecoveryCleanup(oldParticipant, generation));
    const GpuRecoveryTicket oldTicket = monitor.beginRebuild(oldParticipant);
    QVERIFY(oldTicket.isValid());

    // This registration races after recovery began, but before commit. The new
    // graph owns no old-authority device, so it joins already cleanup-acknowledged.
    const uint64_t lateParticipant = monitor.registerRecoveryParticipant(false);
    const GpuRecoveryTicket lateTicket = monitor.beginRebuild(lateParticipant);
    QVERIFY(lateTicket.isValid());
    QCOMPARE(lateTicket.lossGeneration(), generation);
    QCOMPARE(lateTicket.authorityEpoch(), oldTicket.authorityEpoch());

    QVERIFY(monitor.clearForRebuild(oldTicket));
    QVERIFY(monitor.isLost());
    QVERIFY(monitor.clearForRebuild(lateTicket));
    QVERIFY(!monitor.isLost());
    monitor.unregisterRecoveryParticipant(oldParticipant);
    monitor.unregisterRecoveryParticipant(lateParticipant);
    monitor.reset();
}

void TestDeviceLossMonitor::
    noDeviceRegistrationRemainsCleanupAcknowledgedIfLossStartsImmediately() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();

    const GpuRecoveryRegistration registration = monitor.registerRecoveryParticipantSnapshot(false);
    QVERIFY(registration.isValid());
    QCOMPARE(registration.lossGeneration(), uint64_t(0));

    // This is the former initializeOutputGraph race: loss begins after the atomic
    // registration snapshot. The participant is coherently treated as part of the
    // new loss and the barrier waits until its normal worker path acknowledges cleanup.
    const uint64_t generation = monitor.recordLoss();
    QVERIFY(!monitor.beginRebuild(registration.participantId()).isValid());
    QVERIFY(monitor.acknowledgeRecoveryCleanup(registration.participantId(), generation));
    const GpuRecoveryTicket ticket = monitor.beginRebuild(registration.participantId());
    QVERIFY(ticket.isValid());
    QCOMPARE(ticket.lossGeneration(), generation);
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(!monitor.isLost());

    monitor.unregisterRecoveryParticipant(registration.participantId());
    monitor.reset();
}

void TestDeviceLossMonitor::replacementDeviceLossRestartsRecoveryEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t firstParticipant = monitor.registerRecoveryParticipant();
    const uint64_t delayedParticipant = monitor.registerRecoveryParticipant();
    const uint64_t oldGeneration = monitor.recordLoss();
    QVERIFY(monitor.acknowledgeRecoveryCleanup(firstParticipant, oldGeneration));
    QVERIFY(monitor.acknowledgeRecoveryCleanup(delayedParticipant, oldGeneration));
    const GpuRecoveryTicket first = monitor.beginRebuild(firstParticipant);
    const GpuRecoveryTicket delayed = monitor.beginRebuild(delayedParticipant);
    QVERIFY(first.isValid());
    QVERIFY(delayed.isValid());
    QVERIFY(monitor.clearForRebuild(first));
    QVERIFY(monitor.isLost());

    const uint64_t replacementLoss =
        GpuDeviceLossMonitorTestAuthority::publish(first.authorityEpoch(), 0xA17);
    QVERIFY(replacementLoss > oldGeneration);
    QCOMPARE(monitor.currentLossGenerationForTest(), replacementLoss);
    QVERIFY(!monitor.clearForRebuild(delayed));
    QVERIFY(monitor.isLost());

    monitor.unregisterRecoveryParticipant(firstParticipant);
    monitor.unregisterRecoveryParticipant(delayedParticipant);
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestDeviceLossMonitor::validatedRecoveryRejectsMismatchedLossGeneration() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA08);
    QVERIFY(generation != 0);
    GpuDeviceLossMonitorTestAuthority::replaceLossGeneration(generation + 1);

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

void TestDeviceLossMonitor::validatedRecoveryCallbackRunsAfterEpochUnlock() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA12) != 0);
    bool delivered = false;
    QCOMPARE(monitor
                 .withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
                     delivered = true;
                     return qsizetype(1);
                 })
                 .status,
             GpuValidatedLossStatus::Completed);
    QVERIFY(delivered);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
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

void TestDeviceLossMonitor::coordinatorAdmissionFailureIsContainedAndRetryable() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    QVERIFY(participant != 0);
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA16) != 0);

    coordinator.failNextAdmissionForTest();
    bool failedCallbackCalled = false;
    const GpuValidatedLossResult failed =
        monitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
            failedCallbackCalled = true;
            return qsizetype(3);
        });
    QCOMPARE(failed.status, GpuValidatedLossStatus::Rejected);
    QVERIFY(!failedCallbackCalled);
    QVERIFY(monitor.isLost());

    int retryCallbacks = 0;
    const GpuValidatedLossResult retried =
        monitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains&) {
            ++retryCallbacks;
            return qsizetype(7);
        });
    QCOMPARE(retried.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(retried.abandoned, qsizetype(7));
    QCOMPARE(retryCallbacks, 1);

    const std::optional<qsizetype> cleanup = monitor.unregisterRecoveryParticipant(participant);
    QVERIFY(cleanup.has_value());
    QCOMPARE(*cleanup, qsizetype(7));
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestDeviceLossMonitor::rejectedCoordinatorWorkIsNotCached() {
    auto& coordinator = GpuRecoveryCoordinator::instance();
    coordinator.resetForTest();
    int attempts = 0;
    const GpuValidatedLossResult failed = coordinator.coordinate(0xA17, 1, [&]() {
        ++attempts;
        throw std::bad_alloc();
        return GpuValidatedLossResult{};
    });
    QCOMPARE(failed.status, GpuValidatedLossStatus::Rejected);
    QCOMPARE(coordinator.cachedRecoveryCountForTest(), size_t(0));

    const GpuValidatedLossResult retried = coordinator.coordinate(0xA17, 1, [&]() {
        ++attempts;
        return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(9)};
    });
    QCOMPARE(retried.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(retried.abandoned, qsizetype(9));
    QCOMPARE(attempts, 2);
    coordinator.resetForTest();
}

void TestDeviceLossMonitor::rejectedAttemptWaiterKeepsOriginalAttemptIdentity() {
    auto& coordinator = GpuRecoveryCoordinator::instance();
    coordinator.resetForTest();
    QSemaphore attemptOneEntered;
    QSemaphore releaseAttemptOne;
    QSemaphore followerWaiting;
    QSemaphore rejectedPublished;
    QSemaphore continueRejectedNotification;
    QSemaphore retryEntered;
    QSemaphore followerReturned;
    coordinator.m_followerWaitingForTest = &followerWaiting;
    coordinator.m_afterRejectedPublicationForTest = &rejectedPublished;
    coordinator.m_continueRejectedNotificationForTest = &continueRejectedNotification;

    GpuValidatedLossResult leaderResult;
    std::thread leader([&]() {
        leaderResult = coordinator.coordinate(0xA170, 1, [&]() {
            attemptOneEntered.release();
            releaseAttemptOne.acquire();
            return GpuValidatedLossResult{};
        });
    });
    QVERIFY(attemptOneEntered.tryAcquire(1, 5000));

    std::atomic<int> followerCallbacks{0};
    GpuValidatedLossResult followerResult;
    std::thread follower([&]() {
        followerResult = coordinator.coordinate(0xA170, 1, [&]() {
            followerCallbacks.fetch_add(1, std::memory_order_acq_rel);
            return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(11)};
        });
        followerReturned.release();
    });
    QVERIFY(followerWaiting.tryAcquire(1, 5000));

    releaseAttemptOne.release();
    QVERIFY(rejectedPublished.tryAcquire(1, 5000));

    bool retryObservedFollower = false;
    GpuValidatedLossResult retryResult;
    std::thread retry([&]() {
        retryResult = coordinator.coordinate(0xA170, 1, [&]() {
            retryEntered.release();
            retryObservedFollower = followerReturned.tryAcquire(1, 1000);
            return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, qsizetype(22)};
        });
    });
    QVERIFY(retryEntered.tryAcquire(1, 5000));
    continueRejectedNotification.release();

    leader.join();
    follower.join();
    retry.join();
    coordinator.m_followerWaitingForTest = nullptr;
    coordinator.m_afterRejectedPublicationForTest = nullptr;
    coordinator.m_continueRejectedNotificationForTest = nullptr;

    QCOMPARE(leaderResult.status, GpuValidatedLossStatus::Rejected);
    QCOMPARE(followerResult.status, GpuValidatedLossStatus::Rejected);
    QCOMPARE(followerCallbacks.load(std::memory_order_acquire), 0);
    QVERIFY(retryObservedFollower);
    QCOMPARE(retryResult.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(retryResult.abandoned, qsizetype(22));
    coordinator.resetForTest();
}

void TestDeviceLossMonitor::undeliveredRealProofBlocksRecoveryCommit() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA171);
    QVERIFY(generation != 0);

    QVERIFY(!monitor.acknowledgeRecoveryCleanup(participant, generation));
    QVERIFY(!monitor.beginRebuild(participant).isValid());
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.realLossTokens().size(), size_t(1));
    monitor.reset();
}

void TestDeviceLossMonitor::undeliveredZeroParticipantProofBlocksLegacyRecoveryCommit() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    coordinator.failNextAdmissionForTest();
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA173) != 0);
    QVERIFY(monitor.isLost());

    monitor.beginRebuild();
    QCOMPARE(monitor.currentDeviceAuthorityForTest(), authority);
    monitor.clearForRebuild();

    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.realLossTokens().size(), size_t(1));
    monitor.reset();
}

void TestDeviceLossMonitor::staleTicketCannotClearNewerProofRevision() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t participant = monitor.registerRecoveryParticipant();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA172);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);
    QVERIFY(monitor.acknowledgeRecoveryCleanup(participant, generation));
    const GpuRecoveryTicket stale = monitor.beginRebuild(participant);
    QVERIFY(stale.isValid());

    GpuDeviceLossMonitorTestAuthority::advanceProofRevisionWithoutDelivery();

    QVERIFY(!monitor.clearForRebuild(stale));
    QVERIFY(monitor.isLost());
    monitor.reset();
}

void TestDeviceLossMonitor::zeroParticipantTokenlessCleanupRetriesSameRevision() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    coordinator.failNextAdmissionForTest();

    const uint64_t generation = monitor.recordLoss();
    QVERIFY(generation != 0);
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.recordLoss(), generation);
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestDeviceLossMonitor::zeroParticipantRealProofRetriesSameRevision() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    coordinator.failNextAdmissionForTest();

    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA19);
    QVERIFY(generation != 0);
    QVERIFY(monitor.isLost());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA19), generation);
    QVERIFY(!monitor.isLost());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::lastImmediateProofSize(), size_t(1));
    monitor.reset();
}

void TestDeviceLossMonitor::expandedProofRetryDeliversCompleteRevision() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& coordinator = GpuRecoveryCoordinator::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    coordinator.failNextAdmissionForTest();

    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA20);
    QVERIFY(generation != 0);
    QVERIFY(monitor.isLost());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA21), generation);
    QVERIFY(!monitor.isLost());
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::lastImmediateProofSize(), size_t(2));
    monitor.reset();
}

void TestDeviceLossMonitor::lossPreparationFailureDoesNotAdvanceGenerationOrLatch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t tokenlessGeneration = GpuGenerationCounter::instance().current();
    GpuDeviceLossMonitorTestAuthority::failNextLossPreparation();
    QVERIFY_EXCEPTION_THROWN(monitor.recordLoss(), std::bad_alloc);
    QCOMPARE(GpuGenerationCounter::instance().current(), tokenlessGeneration);
    QVERIFY(!monitor.isLost());

    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t authoritativeGeneration = GpuGenerationCounter::instance().current();
    GpuDeviceLossMonitorTestAuthority::failNextLossPreparation();
    QVERIFY_EXCEPTION_THROWN(GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA18),
                             std::bad_alloc);
    QCOMPARE(GpuGenerationCounter::instance().current(), authoritativeGeneration);
    QVERIFY(!monitor.isLost());
    monitor.reset();
}

void TestDeviceLossMonitor::registrationFailureRollsBackPrimaryParticipant() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    GpuDeviceLossMonitorTestAuthority::failRegistrationAfterPrimaryInsert();
    QVERIFY_EXCEPTION_THROWN(monitor.registerRecoveryParticipantSnapshot(), std::bad_alloc);
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
    monitor.reset();
}

void TestDeviceLossMonitor::terminalPromotionIsTransactionalAndIdempotent() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t ordinaryParticipant = monitor.registerRecoveryParticipant();
    const uint64_t recoveryParticipant = monitor.registerRecoveryParticipant();
    QVERIFY(ordinaryParticipant != 0);
    QVERIFY(recoveryParticipant != 0);
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(authority, 0xA19);
    QVERIFY(generation != 0);
    const GpuValidatedLossResult delivery = monitor.withValidatedDeadDomains(
        [](const GpuValidatedDeadDomains&) { return qsizetype(0); });
    QCOMPARE(delivery.status, GpuValidatedLossStatus::Completed);

    monitor.failNextTerminalPromotionForTest();
    QVERIFY(!monitor.promoteRecoveryParticipantToTerminal(ordinaryParticipant));
    QVERIFY(!GpuDeviceLossMonitorTestAuthority::isTerminalParticipant(ordinaryParticipant));
    QVERIFY(!GpuDeviceLossMonitorTestAuthority::isCleanupAcknowledged(ordinaryParticipant));

    QVERIFY(monitor.promoteRecoveryParticipantToTerminal(ordinaryParticipant));
    QVERIFY(monitor.promoteRecoveryParticipantToTerminal(ordinaryParticipant));
    QVERIFY(GpuDeviceLossMonitorTestAuthority::isTerminalParticipant(ordinaryParticipant));
    QVERIFY(GpuDeviceLossMonitorTestAuthority::isCleanupAcknowledged(ordinaryParticipant));
    QVERIFY(monitor.acknowledgeRecoveryCleanup(recoveryParticipant, generation));
    const GpuRecoveryTicket ticket = monitor.beginRebuild(recoveryParticipant);
    QVERIFY(ticket.isValid());
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(monitor.unregisterRecoveryParticipant(recoveryParticipant).has_value());
    QVERIFY(monitor.unregisterTerminalRecoveryParticipant(ordinaryParticipant));
    QCOMPARE(GpuDeviceLossMonitorTestAuthority::participantCount(), size_t(0));
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
    QVERIFY(m.registerRecoveryParticipant() != 0);
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
    const uint64_t firstParticipant = m.registerRecoveryParticipant();
    QVERIFY(firstParticipant != 0);

    const uint64_t firstGen = m.recordLoss();
    const uint64_t duplicateGen = m.recordLoss();
    QCOMPARE(duplicateGen, firstGen);
    QCOMPARE(m.lossCount(), uint64_t(1));
    QVERIFY(m.consumeLossEvent());
    QVERIFY(!m.consumeLossEvent());

    QVERIFY(m.unregisterRecoveryParticipant(firstParticipant).has_value());
    QVERIFY(m.registerRecoveryParticipant() != 0);
    const uint64_t secondGen = m.recordLoss();
    QVERIFY(secondGen > firstGen);
    QCOMPARE(m.lossCount(), uint64_t(2));
    QVERIFY(m.consumeLossEvent());
}

void TestDeviceLossMonitor::consumeLossEventDrainsWithoutClearingLatch() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    QVERIFY(m.registerRecoveryParticipant() != 0);
    m.recordLoss();
    QVERIFY(m.consumeLossEvent());  // one pending event drained
    QVERIFY(!m.consumeLossEvent()); // none left
    QVERIFY(m.isLost());            // draining does NOT clear the latch
}

void TestDeviceLossMonitor::clearForRebuildClearsLatchKeepsGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t participant = m.registerRecoveryParticipant();
    const uint64_t generation = m.recordLoss();
    const uint64_t genAfterLoss = GpuGenerationCounter::instance().current();
    QVERIFY(m.acknowledgeRecoveryCleanup(participant, generation));
    const GpuRecoveryTicket ticket = m.beginRebuild(participant);
    QVERIFY(ticket.isValid());
    QVERIFY(m.clearForRebuild(ticket));
    QVERIFY(!m.isLost()); // GPU path may resume
    QCOMPARE(GpuGenerationCounter::instance().current(),
             genAfterLoss); // dead surfaces stay stale
}

void TestDeviceLossMonitor::tokenlessLossUpgradesFromSameDeviceProof() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().resetForTest();
    QVERIFY(m.registerRecoveryParticipant() != 0);
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
    QVERIFY(m.registerRecoveryParticipant() != 0);
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
    QVERIFY(m.registerRecoveryParticipant() != 0);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
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
    QVERIFY(monitor.registerRecoveryParticipant() != 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(replacementAuthority) != 0);
    QVERIFY(monitor.realLossToken().has_value());
}

void TestDeviceLossMonitor::currentAuthorityPublicationTracksGuardedEpoch() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t initialAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(monitor.isCurrentDeviceAuthority(initialAuthority));
    QVERIFY(!monitor.isCurrentDeviceAuthority(0));

    monitor.recordLoss();
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
