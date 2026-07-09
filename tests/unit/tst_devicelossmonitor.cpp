// GpuDeviceLossMonitor is the process-wide device-loss latch. recordLoss bumps
// the GpuGenerationCounter (stale-surface invalidation) and sets the latch;
// consumeLossEvent drains events for telemetry without clearing the latch;
// clearForRebuild clears the latch after a successful rebuild while leaving the
// generation bumped (dead surfaces stay stale).
#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"

class TestDeviceLossMonitor : public QObject {
    Q_OBJECT
private slots:
    void recordLossSetsLatchAndBumpsGeneration();
    void recordLossIsIdempotentUntilRebuildClearsLatch();
    void consumeLossEventDrainsWithoutClearingLatch();
    void clearForRebuildClearsLatchKeepsGeneration();
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
