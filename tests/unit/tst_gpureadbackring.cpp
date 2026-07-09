#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/output/framehandle.h"
#include "playback/output/gpureadbackring.h"

namespace {

OutputBusFrame frameFor(qint64 index) {
    OutputBusFrame frame;
    frame.bus = OutputBusId::pgm();
    frame.outputFrameIndex = index;
    frame.sampledPlayheadMs = index * 16;
    frame.video = solidYuv420pHandle(16, 16, uchar(16 + int(index)), 128, 128);
    frame.audio.pcm = QByteArray(4, char(index));
    frame.identity.bus = frame.bus;
    frame.identity.outputFrameIndex = index;
    frame.identity.sampledPlayheadMs = frame.sampledPlayheadMs;
    frame.identity.videoHash = quint32(index + 1);
    frame.identity.audioHash = quint32(index + 10);
    return frame;
}

uint64_t retiredSignal(const std::shared_ptr<GpuFence>& fence) {
    const uint64_t value = fence->signal();
    if (!fence->wait(value, 1000)) return 0;
    return value;
}

} // namespace

class TestGpuReadbackRing : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void depthOneIsSubFrameWhenFenceRetired();
    void depthThreeLagsByTwo();
    void pendingFenceHoldsFrameBack();
    void flushWaitsForOldestFrame();
    void overwriteCountsAsDrop();
    void retainedSharedReadbackSurvivesPrunesForDelayedPeer();
    void retainedFailedSharedReadbackIsReusedByDelayedPeer();

private:
    std::shared_ptr<GpuFence> m_fence;
};

void TestGpuReadbackRing::initTestCase() {
    m_fence = GpuFence::create();
    if (!m_fence) QSKIP("no GPU fence backend on this host");
}

void TestGpuReadbackRing::depthOneIsSubFrameWhenFenceRetired() {
    GpuReadbackRing ring(1);

    RingReadyFrame ready =
        ring.pushAndPop(frameFor(0), retiredSignal(m_fence), m_fence, FramePixelFormat::Yuv420p);

    QVERIFY(ready.ready);
    QCOMPARE(ready.frame.outputFrameIndex, qint64(0));
    QVERIFY(!ready.frame.video.isGpuBacked());
    QCOMPARE(ready.frame.audio.pcm.at(0), char(0));
    QCOMPARE(ring.occupancy(), 0);
}

void TestGpuReadbackRing::depthThreeLagsByTwo() {
    GpuReadbackRing ring(3);

    QVERIFY(
        !ring.pushAndPop(frameFor(0), retiredSignal(m_fence), m_fence, FramePixelFormat::Yuv420p)
             .ready);
    QVERIFY(
        !ring.pushAndPop(frameFor(1), retiredSignal(m_fence), m_fence, FramePixelFormat::Yuv420p)
             .ready);
    RingReadyFrame ready =
        ring.pushAndPop(frameFor(2), retiredSignal(m_fence), m_fence, FramePixelFormat::Yuv420p);

    QVERIFY(ready.ready);
    QCOMPARE(ready.frame.outputFrameIndex, qint64(0));
    QCOMPARE(ready.frame.audio.pcm.at(0), char(0));
    QCOMPARE(ring.occupancy(), 2);
}

void TestGpuReadbackRing::pendingFenceHoldsFrameBack() {
    GpuReadbackRing ring(1);
    const uint64_t future = m_fence->completedValue() + 100;

    RingReadyFrame ready = ring.pushAndPop(frameFor(0), future, m_fence, FramePixelFormat::Yuv420p);

    QVERIFY(!ready.ready);
    QCOMPARE(ring.occupancy(), 1);
}

void TestGpuReadbackRing::flushWaitsForOldestFrame() {
    GpuReadbackRing ring(3);
    const uint64_t value = m_fence->signal();
    QVERIFY(!ring.pushAndPop(frameFor(3), value, m_fence, FramePixelFormat::Yuv420p).ready);
    QVERIFY(m_fence->wait(value, 1000));

    RingReadyFrame ready = ring.flushOne(1000);

    QVERIFY(ready.ready);
    QCOMPARE(ready.frame.outputFrameIndex, qint64(3));
    QCOMPARE(ready.frame.audio.pcm.at(0), char(3));
    QCOMPARE(ring.occupancy(), 0);
}

void TestGpuReadbackRing::overwriteCountsAsDrop() {
    GpuReadbackRing ring(3);
    const uint64_t future = m_fence->completedValue() + 100;

    for (qint64 i = 0; i < 5; ++i)
        ring.pushAndPop(frameFor(i), future, m_fence, FramePixelFormat::Yuv420p);

    QVERIFY(ring.drops() >= 1);
    QCOMPARE(ring.occupancy(), 3);
}

void TestGpuReadbackRing::retainedSharedReadbackSurvivesPrunesForDelayedPeer() {
    SharedGpuReadbackCache cache;
    const OutputBusFrame frame = frameFor(7);
    int reads = 0;

    auto read = [&]() {
        ++reads;
        return frame.video.readToCpu(FramePixelFormat::Yuv420p);
    };

    cache.retain(frame, FramePixelFormat::Yuv420p);
    cache.retain(frame, FramePixelFormat::Yuv420p);

    QVERIFY(cache.getOrRead(frame, FramePixelFormat::Yuv420p, read).isValid());
    cache.release(frame, FramePixelFormat::Yuv420p);
    QCOMPARE(reads, 1);

    cache.clear();
    cache.clear();
    QVERIFY(cache.getOrRead(frame, FramePixelFormat::Yuv420p, read).isValid());
    cache.release(frame, FramePixelFormat::Yuv420p);
    QCOMPARE(reads, 1);

    cache.clear();
    QVERIFY(cache.getOrRead(frame, FramePixelFormat::Yuv420p, read).isValid());
    QCOMPARE(reads, 1);

    qputenv("OLR_GPU_READBACK_CACHE_MB", "0");
    cache.clear();
    qunsetenv("OLR_GPU_READBACK_CACHE_MB");
    QVERIFY(cache.getOrRead(frame, FramePixelFormat::Yuv420p, read).isValid());
    QCOMPARE(reads, 2);
}

void TestGpuReadbackRing::retainedFailedSharedReadbackIsReusedByDelayedPeer() {
    SharedGpuReadbackCache cache;
    const OutputBusFrame frame = frameFor(8);
    int reads = 0;

    auto failRead = [&]() {
        ++reads;
        return CpuPlanes{};
    };

    cache.retain(frame, FramePixelFormat::Yuv420p);
    cache.retain(frame, FramePixelFormat::Yuv420p);

    QVERIFY(!cache.getOrRead(frame, FramePixelFormat::Yuv420p, failRead).isValid());
    cache.release(frame, FramePixelFormat::Yuv420p);
    QCOMPARE(reads, 1);

    cache.clear();
    QVERIFY(!cache.getOrRead(frame, FramePixelFormat::Yuv420p, failRead).isValid());
    cache.release(frame, FramePixelFormat::Yuv420p);
    QCOMPARE(reads, 1);

    cache.clear();
    QVERIFY(!cache.getOrRead(frame, FramePixelFormat::Yuv420p, failRead).isValid());
    QCOMPARE(reads, 2);
}

QTEST_GUILESS_MAIN(TestGpuReadbackRing)
#include "tst_gpureadbackring.moc"
