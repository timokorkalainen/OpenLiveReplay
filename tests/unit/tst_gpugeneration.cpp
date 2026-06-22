// GpuGenerationCounter invalidates stale GPU surfaces after a seek/reposition,
// mirroring PlaybackWorker::m_seekGeneration. CPU frames are generation-agnostic.
#include <QtTest>

#include "playback/gpu/gpugeneration.h"
#include "playback/output/framehandle.h"

class TestGpuGeneration : public QObject {
    Q_OBJECT
private slots:
    void resetStartsAtLiveGpuGeneration();
    void bumpAdvancesMonotonically();
    void cpuHandleIsNeverStale();
    void gpuHandleStaleAfterBump();
};

void TestGpuGeneration::resetStartsAtLiveGpuGeneration() {
    auto& generation = GpuGenerationCounter::instance();
    generation.resetForTest();
    QVERIFY(generation.current() >= uint64_t(1));
}

void TestGpuGeneration::bumpAdvancesMonotonically() {
    auto& generation = GpuGenerationCounter::instance();
    generation.resetForTest();
    QCOMPARE(generation.current(), uint64_t(1));
    QCOMPARE(generation.bump(), uint64_t(2));
    QCOMPARE(generation.bump(), uint64_t(3));
    QCOMPARE(generation.current(), uint64_t(3));
}

void TestGpuGeneration::cpuHandleIsNeverStale() {
    auto& generation = GpuGenerationCounter::instance();
    generation.resetForTest();
    FrameHandle handle = solidYuv420pHandle(16, 16, 16, 128, 128);
    generation.bump();
    QVERIFY(!handle.isStaleForGeneration(generation.current()));
}

void TestGpuGeneration::gpuHandleStaleAfterBump() {
    auto& generation = GpuGenerationCounter::instance();
    generation.resetForTest();
    const uint64_t mintedAt = generation.bump();
    FrameHandle handle = solidYuv420pHandle(16, 16, 16, 128, 128);
    handle.metadata().gpuGeneration = mintedAt;
    QVERIFY(!handle.isStaleForGeneration(generation.current()));
    generation.bump();
    QVERIFY(handle.isStaleForGeneration(generation.current()));
}

QTEST_GUILESS_MAIN(TestGpuGeneration)
#include "tst_gpugeneration.moc"
