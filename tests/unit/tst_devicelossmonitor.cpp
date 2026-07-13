// GpuDeviceLossMonitor is the process-wide device-loss latch. recordLoss bumps
// the GpuGenerationCounter (stale-surface invalidation) and sets the latch;
// consumeLossEvent drains events for telemetry without clearing the latch;
// clearForRebuild clears the latch after a successful rebuild while leaving the
// generation bumped (dead surfaces stay stale).
#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"

#include <thread>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch = capture()) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch);
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
    void tokenlessLossStaysTokenlessWithoutDriverProof();
    void realLossPublicationIsAtomicWithEpoch();
    void stalePublicationAfterClearIsRejected();
    void rebuildAuthorityRejectsOldDeviceAcceptsReplacement();
    void resetReturnsToPristine();
};

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
    const uint64_t lossGeneration = m.recordSubmissionFailure();
    QVERIFY(!m.realLossToken().has_value());

    QCOMPARE(GpuDeviceLossMonitorTestAuthority::publish(deviceAuthorityEpoch), lossGeneration);
    const auto token = m.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(token->observedGeneration(), lossGeneration);
    QCOMPARE(m.lossCount(), uint64_t(1));
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
