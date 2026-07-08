#include <QtTest>
#include <QSemaphore>

#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/framehandle.h"
#include "playback/output/sinkgpucapability.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <atomic>
#include <thread>

namespace {

class RecordingSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment&, FrameRate) override {
        active.store(true, std::memory_order_release);
        return true;
    }

    void stop() override { active.store(false, std::memory_order_release); }
    bool isActive() const override { return active.load(std::memory_order_acquire); }

    bool submit(const OutputBusFrame& frame) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        delivered.append(frame);
        gpuBackedAtSink.append(frame.video.isGpuBacked());
        return true;
    }

    void discardPending() override { discardCalls.fetch_add(1, std::memory_order_acq_rel); }

    qsizetype deliveredCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return delivered.size();
    }

    OutputBusFrame deliveredAt(qsizetype index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return delivered.at(index);
    }

    bool gpuBackedAt(qsizetype index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return gpuBackedAtSink.at(index);
    }

    int discardCount() const { return discardCalls.load(std::memory_order_acquire); }

private:
    mutable std::mutex m_mutex;
    QVector<OutputBusFrame> delivered;
    QVector<bool> gpuBackedAtSink;
    std::atomic_bool active{false};
    std::atomic<int> discardCalls{0};
};

class FlushRecordingSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment&, FrameRate) override {
        active.store(true, std::memory_order_release);
        return true;
    }

    void stop() override { active.store(false, std::memory_order_release); }
    bool isActive() const override { return active.load(std::memory_order_acquire); }

    bool submit(const OutputBusFrame& frame) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        delivered.append(frame);
        return true;
    }

    bool flush(int timeoutMs) override {
        Q_UNUSED(timeoutMs);
        flushCalls.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    int deliveredCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return delivered.size();
    }

    OutputBusFrame deliveredAt(qsizetype index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return delivered.at(index);
    }

    int flushCount() const { return flushCalls.load(std::memory_order_acquire); }

private:
    mutable std::mutex m_mutex;
    QVector<OutputBusFrame> delivered;
    std::atomic_bool active{false};
    std::atomic<int> flushCalls{0};
};

class SubmitFlushOnlySink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }

    bool start(const OutputTargetAssignment&, FrameRate) override {
        active.store(true, std::memory_order_release);
        return true;
    }

    void stop() override { active.store(false, std::memory_order_release); }
    bool isActive() const override { return active.load(std::memory_order_acquire); }

    bool submit(const OutputBusFrame&) override { return false; }

    bool submitAndFlush(const OutputBusFrame& frame, int timeoutMs) override {
        Q_UNUSED(timeoutMs);
        std::lock_guard<std::mutex> lock(m_mutex);
        delivered.append(frame);
        submitAndFlushCalls.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    bool flush(int timeoutMs) override {
        Q_UNUSED(timeoutMs);
        flushCalls.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }

    int deliveredCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return delivered.size();
    }

    int submitAndFlushCount() const { return submitAndFlushCalls.load(std::memory_order_acquire); }

    int flushCount() const { return flushCalls.load(std::memory_order_acquire); }

private:
    mutable std::mutex m_mutex;
    QVector<OutputBusFrame> delivered;
    std::atomic_bool active{false};
    std::atomic<int> submitAndFlushCalls{0};
    std::atomic<int> flushCalls{0};
};

OutputBusFrame cpuFrame(qint64 index, uint64_t gpuGeneration = 0) {
    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = index;
    frame.sampledPlayheadMs = index * 16;
    frame.video = solidYuv420pHandle(16, 16, uchar(16 + int(index)), 128, 128);
    frame.video.metadata().gpuGeneration = gpuGeneration;
    frame.identity.bus = frame.bus;
    frame.identity.outputFrameIndex = index;
    frame.identity.sampledPlayheadMs = frame.sampledPlayheadMs;
    frame.identity.sourcePtsMs = index;
    frame.identity.videoHash = quint32(index + 1);
    return frame;
}

class CountingSurface final : public GpuSurface {
public:
    explicit CountingSurface(uint64_t pendingFence = 0) : m_pendingFence(pendingFence) {}

    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Yuv420p, 16, 16}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    uint64_t m_pendingFence = 0;
};

class ReadyFence final : public GpuFence {
public:
    uint64_t signal() override { return m_completed.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override { return completedValue() >= value; }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> m_completed{1};
};

class CountingGpuFrameData final : public IFrameData {
public:
    explicit CountingGpuFrameData(int y, uint64_t pendingFence = 0,
                                  std::shared_ptr<GpuFence> producerFence = nullptr)
        : m_y(y), m_surface(std::make_shared<CountingSurface>(pendingFence)),
          m_producerFence(producerFence ? std::move(producerFence)
                                        : std::make_shared<ReadyFence>()) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override {
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
        return solidYuv420pHandle(16, 16, uchar(m_y), 128, 128).readToCpu(target);
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_producerFence; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Yuv420p; }
    int readCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    int m_y = 16;
    std::shared_ptr<CountingSurface> m_surface;
    std::shared_ptr<GpuFence> m_producerFence;
    mutable std::atomic<int> m_readCount{0};
};

class FailingGpuFrameData final : public IFrameData {
public:
    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override { return CpuPlanes{}; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_producerFence; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Yuv420p; }

private:
    std::shared_ptr<CountingSurface> m_surface = std::make_shared<CountingSurface>();
    std::shared_ptr<GpuFence> m_producerFence = std::make_shared<ReadyFence>();
};

class BlockingGpuFrameData final : public IFrameData {
public:
    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override {
        m_startCount.fetch_add(1, std::memory_order_acq_rel);
        m_readStarted.release();
        m_releaseReadback.acquire();
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
        return solidYuv420pHandle(16, 16, 96, 128, 128).readToCpu(target);
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_producerFence; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Yuv420p; }
    bool waitForReadStart(int timeoutMs) const { return m_readStarted.tryAcquire(1, timeoutMs); }
    void releaseReadback(int count = 1) const { m_releaseReadback.release(count); }
    int startCount() const { return m_startCount.load(std::memory_order_acquire); }
    int readCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    std::shared_ptr<CountingSurface> m_surface = std::make_shared<CountingSurface>();
    std::shared_ptr<GpuFence> m_producerFence = std::make_shared<ReadyFence>();
    mutable QSemaphore m_readStarted;
    mutable QSemaphore m_releaseReadback;
    mutable std::atomic<int> m_startCount{0};
    mutable std::atomic<int> m_readCount{0};
};

class UnfencedGpuFrameData final : public IFrameData {
public:
    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override {
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
        return solidYuv420pHandle(16, 16, 88, 128, 128).readToCpu(target);
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Yuv420p; }
    int readCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    std::shared_ptr<CountingSurface> m_surface = std::make_shared<CountingSurface>();
    mutable std::atomic<int> m_readCount{0};
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

class CountingFence final : public GpuFence {
public:
    uint64_t signal() override { return ++m_signaled; }
    bool wait(uint64_t, int) override {
        m_waitCalls.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    uint64_t completedValue() const override { return 0; }
    int waitCalls() const { return m_waitCalls.load(std::memory_order_acquire); }

private:
    uint64_t m_signaled = 0;
    std::atomic<int> m_waitCalls{0};
};

OutputBusFrame gpuFrame(qint64 index, const std::shared_ptr<IFrameData>& data,
                        uint64_t gpuGeneration = 1) {
    FrameMetadata meta;
    meta.key.width = 16;
    meta.key.height = 16;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.outputFrameIndex = index;
    meta.gpuGeneration = gpuGeneration;

    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = index;
    frame.sampledPlayheadMs = index * 16;
    frame.video = FrameHandle(data, meta);
    frame.audio.pcm = QByteArray(4, char(index));
    frame.identity.bus = frame.bus;
    frame.identity.outputFrameIndex = index;
    frame.identity.sampledPlayheadMs = frame.sampledPlayheadMs;
    frame.identity.sourcePtsMs = index;
    frame.identity.videoHash = quint32(index + 1);
    frame.identity.audioHash = quint32(index + 11);
    return frame;
}

} // namespace

class TestAsyncGpuReadbackSink : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void cpuFramePassesThroughImmediately();
    void flagOffIsTransparent();
    void flagOffDisablesReadbackStats();
    void unfencedGpuFrameIsDropped();
    void gpuRingDeliversAudioWithItsVideoFrame();
    void sharedReadbackCacheAvoidsDuplicateReadToCpu();
    void sharedReadbackCacheKeepsInFlightEntryAcrossTickClear();
    void stopReturnsWhileWaitingForSharedReadbackOwner();
    void sharedReadbackCacheSeparatesBusSurfaces();
    void sharedReadbackCacheSeparatesGpuGenerations();
    void generationChangeClearsPendingReadbacks();
    void generationChangeDoesNotCountInFlightReadbackAsQueueDrop();
    void cpuGenerationChangeClearsPendingReadbacks();
    void generationChangePreservesExistingDropCount();
    void stopDropsPendingReadbacksWithoutWaitingOnFence();
    void discardPendingForwardsToInnerSink();
    void producerFenceControlsGpuReadbackReadiness();
    void flushDeliversPendingGpuReadbackWithoutFutureSubmit();
    void flushForwardsToInnerSinkAfterReadbackDelivery();
    void submitAndFlushDoesNotDoubleFlushInnerAfterGpuReadback();
    void cpuSubmitAndFlushStillFlushesAfterDrainingPendingGpuReadback();
    void gpuReadbackDoesNotBlockSubmitTick();
    void continuousCadenceResendsLastFrameWhileFencePending();
    void readyReadbackDoesNotDoubleSubmitCadence();
    void cpuClearFrameWithZeroGenerationDropsPendingGpuReadbacks();
    void invalidReadbackCountsAsDropAfterCadenceResend();
};

void TestAsyncGpuReadbackSink::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
}

void TestAsyncGpuReadbackSink::cpuFramePassesThroughImmediately() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence);

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(cpuFrame(0)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(0));
    QCOMPARE(observed->gpuBackedAt(0), false);
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
    QCOMPARE(sink.readbackDrops(), qint64(0));
}

void TestAsyncGpuReadbackSink::flagOffIsTransparent() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::AsyncReadbackDedupOk);

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(cpuFrame(7)));

    QCOMPARE(observed->deliveredCount(), 1);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(7));
    QCOMPARE(observed->gpuBackedAt(0), false);
    QCOMPARE(sink.ringDepth(), 1);
}

void TestAsyncGpuReadbackSink::flagOffDisablesReadbackStats() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto inner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::AsyncReadbackDedupOk);

    QVERIFY(sink.start({}, FrameRate{}));
    qint64 depth = -1;
    qint64 drops = -1;
    QVERIFY(!sink.readbackStats(depth, drops));
    QCOMPARE(depth, qint64(0));
    QCOMPARE(drops, qint64(0));
}

void TestAsyncGpuReadbackSink::unfencedGpuFrameIsDropped() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());
    auto data = std::make_shared<UnfencedGpuFrameData>();

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, data, 1)));

    QCOMPARE(observed->deliveredCount(), qsizetype(0));
    QCOMPARE(data->readCount(), 0);
    QCOMPARE(sink.readbackDrops(), qint64(1));
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::gpuRingDeliversAudioWithItsVideoFrame() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    for (qint64 index = 0; index < 4; ++index) {
        auto data = std::make_shared<CountingGpuFrameData>(16 + int(index));
        QVERIFY(sink.submit(gpuFrame(index, data)));
    }

    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 2, 1000);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(0));
    QCOMPARE(observed->deliveredAt(0).audio.pcm.at(0), char(0));
    QCOMPARE(observed->deliveredAt(1).outputFrameIndex, qint64(1));
    QCOMPARE(observed->deliveredAt(1).audio.pcm.at(0), char(1));
    QVERIFY(!observed->deliveredAt(0).video.isGpuBacked());
}

void TestAsyncGpuReadbackSink::sharedReadbackCacheAvoidsDuplicateReadToCpu() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto sharedReadbacks = std::make_shared<SharedGpuReadbackCache>();
    auto data = std::make_shared<CountingGpuFrameData>(48);
    const OutputBusFrame frame = gpuFrame(9, data);

    auto firstInner = std::make_unique<RecordingSink>();
    auto secondInner = std::make_unique<RecordingSink>();
    RecordingSink* first = firstInner.get();
    RecordingSink* second = secondInner.get();

    AsyncGpuReadbackSink firstSink(std::move(firstInner), 1, FramePixelFormat::Yuv420p,
                                   SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                   sharedReadbacks);
    AsyncGpuReadbackSink secondSink(std::move(secondInner), 1, FramePixelFormat::Yuv420p,
                                    SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                    sharedReadbacks);

    QVERIFY(firstSink.start({}, FrameRate{}));
    QVERIFY(secondSink.start({}, FrameRate{}));
    QVERIFY(firstSink.submit(frame));
    QVERIFY(secondSink.submit(frame));

    QTRY_COMPARE_WITH_TIMEOUT(first->deliveredCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(second->deliveredCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(data->readCount(), 1, 1000);
    QVERIFY(!first->deliveredAt(0).video.isGpuBacked());
    QVERIFY(!second->deliveredAt(0).video.isGpuBacked());
}

void TestAsyncGpuReadbackSink::sharedReadbackCacheKeepsInFlightEntryAcrossTickClear() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto sharedReadbacks = std::make_shared<SharedGpuReadbackCache>();
    auto data = std::make_shared<BlockingGpuFrameData>();
    const OutputBusFrame frame = gpuFrame(9, data);

    auto firstInner = std::make_unique<RecordingSink>();
    auto secondInner = std::make_unique<RecordingSink>();
    RecordingSink* first = firstInner.get();
    RecordingSink* second = secondInner.get();

    AsyncGpuReadbackSink firstSink(std::move(firstInner), 1, FramePixelFormat::Yuv420p,
                                   SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                   sharedReadbacks);
    AsyncGpuReadbackSink secondSink(std::move(secondInner), 1, FramePixelFormat::Yuv420p,
                                    SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                    sharedReadbacks);

    QVERIFY(firstSink.start({}, FrameRate{}));
    QVERIFY(secondSink.start({}, FrameRate{}));
    QVERIFY(firstSink.submit(frame));
    QVERIFY2(data->waitForReadStart(1000), "first readback did not start");

    sharedReadbacks->clear();
    QVERIFY(secondSink.submit(frame));
    QTest::qWait(50);
    const int readStarts = data->startCount();
    data->releaseReadback(readStarts);

    QTRY_COMPARE_WITH_TIMEOUT(first->deliveredCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(second->deliveredCount(), 1, 1000);
    QCOMPARE(readStarts, 1);
    QCOMPARE(data->readCount(), 1);
}

void TestAsyncGpuReadbackSink::stopReturnsWhileWaitingForSharedReadbackOwner() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto sharedReadbacks = std::make_shared<SharedGpuReadbackCache>();
    auto data = std::make_shared<BlockingGpuFrameData>();
    const OutputBusFrame frame = gpuFrame(10, data);

    auto firstInner = std::make_unique<RecordingSink>();
    auto secondInner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink firstSink(std::move(firstInner), 1, FramePixelFormat::Yuv420p,
                                   SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                   sharedReadbacks);
    AsyncGpuReadbackSink secondSink(std::move(secondInner), 1, FramePixelFormat::Yuv420p,
                                    SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                    sharedReadbacks);

    QVERIFY(firstSink.start({}, FrameRate{}));
    QVERIFY(secondSink.start({}, FrameRate{}));
    QVERIFY(firstSink.submit(frame));
    QVERIFY2(data->waitForReadStart(1000), "first readback did not start");
    QVERIFY(secondSink.submit(frame));
    QTest::qWait(50);

    std::atomic<bool> stopReturned{false};
    std::thread stopThread([&]() {
        secondSink.stop();
        stopReturned.store(true, std::memory_order_release);
    });
    QTest::qWait(50);
    const bool returnedBeforeOwnerReadbackFinished = stopReturned.load(std::memory_order_acquire);

    data->releaseReadback(data->startCount());
    stopThread.join();

    QVERIFY2(returnedBeforeOwnerReadbackFinished,
             "stop() waited for another sink's in-flight shared readback");
}

void TestAsyncGpuReadbackSink::sharedReadbackCacheSeparatesBusSurfaces() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto sharedReadbacks = std::make_shared<SharedGpuReadbackCache>();
    auto data = std::make_shared<CountingGpuFrameData>(50);
    OutputBusFrame feed = gpuFrame(9, data);
    feed.bus = OutputBusId::feed(2000006);
    feed.identity.bus = feed.bus;
    OutputBusFrame pgm = feed;
    pgm.bus = OutputBusId::pgm();
    pgm.identity.bus = pgm.bus;

    auto feedInner = std::make_unique<RecordingSink>();
    auto pgmInner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink feedSink(std::move(feedInner), 1, FramePixelFormat::Yuv420p,
                                  SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                  sharedReadbacks);
    AsyncGpuReadbackSink pgmSink(std::move(pgmInner), 1, FramePixelFormat::Yuv420p,
                                 SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                 sharedReadbacks);

    QVERIFY(feedSink.start({}, FrameRate{}));
    QVERIFY(pgmSink.start({}, FrameRate{}));
    QVERIFY(feedSink.submit(feed));
    QVERIFY(pgmSink.submit(pgm));

    QTRY_COMPARE_WITH_TIMEOUT(data->readCount(), 2, 1000);
}

void TestAsyncGpuReadbackSink::sharedReadbackCacheSeparatesGpuGenerations() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto sharedReadbacks = std::make_shared<SharedGpuReadbackCache>();
    auto data = std::make_shared<CountingGpuFrameData>(52);

    auto firstInner = std::make_unique<RecordingSink>();
    auto secondInner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink firstSink(std::move(firstInner), 1, FramePixelFormat::Yuv420p,
                                   SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                   sharedReadbacks);
    AsyncGpuReadbackSink secondSink(std::move(secondInner), 1, FramePixelFormat::Yuv420p,
                                    SinkGpuCapability::NeedsContinuousCadence, GpuFence::create(),
                                    sharedReadbacks);

    QVERIFY(firstSink.start({}, FrameRate{}));
    QVERIFY(secondSink.start({}, FrameRate{}));
    QVERIFY(firstSink.submit(gpuFrame(9, data, 1)));
    QVERIFY(secondSink.submit(gpuFrame(9, data, 2)));

    QTRY_COMPARE_WITH_TIMEOUT(data->readCount(), 2, 1000);
}

void TestAsyncGpuReadbackSink::generationChangeClearsPendingReadbacks() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    for (qint64 index = 0; index < 3; ++index) {
        auto data = std::make_shared<CountingGpuFrameData>(16 + int(index));
        QVERIFY(sink.submit(gpuFrame(index, data, 1)));
    }
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);
    QCOMPARE(observed->deliveredAt(0).video.metadata().gpuGeneration, uint64_t(1));

    auto gen2 = std::make_shared<CountingGpuFrameData>(80);
    QVERIFY(sink.submit(gpuFrame(3, gen2, 2)));
    QCOMPARE(observed->deliveredCount(), 1);
    QCOMPARE(sink.readbackDrops(), qint64(2));
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));

    QVERIFY(sink.submit(gpuFrame(4, std::make_shared<CountingGpuFrameData>(81), 2)));
    QVERIFY(sink.submit(gpuFrame(5, std::make_shared<CountingGpuFrameData>(82), 2)));
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 2, 1000);
    QCOMPARE(observed->deliveredAt(1).outputFrameIndex, qint64(3));
    QCOMPARE(observed->deliveredAt(1).video.metadata().gpuGeneration, uint64_t(2));
}

void TestAsyncGpuReadbackSink::generationChangeDoesNotCountInFlightReadbackAsQueueDrop() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    auto blocking = std::make_shared<BlockingGpuFrameData>();
    QVERIFY(sink.submit(gpuFrame(0, blocking, 1)));
    QVERIFY(sink.submit(gpuFrame(1, std::make_shared<CountingGpuFrameData>(41), 1)));
    QVERIFY(sink.submit(gpuFrame(2, std::make_shared<CountingGpuFrameData>(42), 1)));
    QVERIFY(sink.submit(gpuFrame(3, std::make_shared<CountingGpuFrameData>(43), 1)));
    QVERIFY2(blocking->waitForReadStart(1000), "readback did not start");
    QVERIFY(sink.submit(gpuFrame(4, std::make_shared<CountingGpuFrameData>(80), 2)));

    const qint64 readbackDrops = sink.readbackDrops();
    blocking->releaseReadback();
    QCOMPARE(readbackDrops, qint64(3));
}

void TestAsyncGpuReadbackSink::cpuGenerationChangeClearsPendingReadbacks() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, std::make_shared<CountingGpuFrameData>(40), 1)));
    QVERIFY(sink.submit(gpuFrame(1, std::make_shared<CountingGpuFrameData>(41), 1)));
    QCOMPARE(observed->deliveredCount(), 0);
    QCOMPARE(sink.readbackQueueDepth(), qint64(2));

    QVERIFY(sink.submit(cpuFrame(2, 2)));
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(2));
    QCOMPARE(observed->deliveredAt(0).video.metadata().gpuGeneration, uint64_t(2));
    QCOMPARE(sink.readbackDrops(), qint64(2));
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));

    sink.stop();
    QCOMPARE(observed->deliveredCount(), 1);
}

void TestAsyncGpuReadbackSink::cpuClearFrameWithZeroGenerationDropsPendingGpuReadbacks() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, std::make_shared<CountingGpuFrameData>(40), 1)));
    QVERIFY(sink.submit(gpuFrame(1, std::make_shared<CountingGpuFrameData>(41), 1)));
    QCOMPARE(observed->deliveredCount(), 0);
    QCOMPARE(sink.readbackQueueDepth(), qint64(2));

    QVERIFY(sink.submit(cpuFrame(2)));

    QCOMPARE(observed->deliveredCount(), 1);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(2));
    QCOMPARE(observed->deliveredAt(0).video.metadata().gpuGeneration, uint64_t(0));
    QCOMPARE(sink.readbackDrops(), qint64(2));
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::generationChangePreservesExistingDropCount() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    const uint64_t pendingFence = 9999;
    for (qint64 index = 0; index < 5; ++index) {
        auto data = std::make_shared<CountingGpuFrameData>(60 + int(index), pendingFence);
        QVERIFY(sink.submit(gpuFrame(index, data, 1)));
    }
    QCOMPARE(sink.readbackDrops(), qint64(2));
    QCOMPARE(sink.readbackQueueDepth(), qint64(3));

    QVERIFY(sink.submit(gpuFrame(5, std::make_shared<CountingGpuFrameData>(80), 2)));
    QCOMPARE(sink.readbackDrops(), qint64(5));
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));
}

void TestAsyncGpuReadbackSink::stopDropsPendingReadbacksWithoutWaitingOnFence() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto fence = std::make_shared<CountingFence>();
    auto inner = std::make_unique<RecordingSink>();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, fence);

    QVERIFY(sink.start({}, FrameRate{}));
    auto data = std::make_shared<CountingGpuFrameData>(70, 9999);
    QVERIFY(sink.submit(gpuFrame(0, data, 1)));
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));

    sink.stop();

    QCOMPARE(fence->waitCalls(), 0);
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::discardPendingForwardsToInnerSink() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, std::make_shared<CountingGpuFrameData>(74), 1)));
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));

    sink.discardPending();

    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
    QCOMPARE(observed->discardCount(), 1);
}

void TestAsyncGpuReadbackSink::producerFenceControlsGpuReadbackReadiness() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto wrapperFence = std::make_shared<CountingFence>();
    auto producerFence = std::make_shared<ManualFence>();
    producerFence->complete(7);

    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, wrapperFence);

    QVERIFY(sink.start({}, FrameRate{}));
    auto data = std::make_shared<CountingGpuFrameData>(90, 7, producerFence);
    QVERIFY(sink.submit(gpuFrame(0, data, 1)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(0));
    QTRY_COMPARE_WITH_TIMEOUT(data->readCount(), 1, 1000);
    QCOMPARE(wrapperFence->waitCalls(), 0);
}

void TestAsyncGpuReadbackSink::flushDeliversPendingGpuReadbackWithoutFutureSubmit() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    auto producerFence = std::make_shared<ManualFence>();
    auto data = std::make_shared<CountingGpuFrameData>(53, 7, producerFence);

    QVERIFY(sink.submit(gpuFrame(1, data, 1)));
    QCOMPARE(observed->deliveredCount(), qsizetype(0));
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));

    producerFence->complete(7);
    QVERIFY(sink.flush(1000));
    QCOMPARE(observed->deliveredCount(), qsizetype(1));
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(1));
    QVERIFY(!observed->gpuBackedAt(0));
    QCOMPARE(data->readCount(), 1);
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::flushForwardsToInnerSinkAfterReadbackDelivery() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<FlushRecordingSink>();
    FlushRecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    auto producerFence = std::make_shared<ManualFence>();
    auto data = std::make_shared<CountingGpuFrameData>(54, 9, producerFence);

    QVERIFY(sink.submit(gpuFrame(2, data, 1)));
    producerFence->complete(9);
    QVERIFY(sink.flush(1000));

    QCOMPARE(observed->deliveredCount(), 1);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(2));
    QCOMPARE(observed->flushCount(), 1);
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::submitAndFlushDoesNotDoubleFlushInnerAfterGpuReadback() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<SubmitFlushOnlySink>();
    SubmitFlushOnlySink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    auto producerFence = std::make_shared<ManualFence>();
    auto data = std::make_shared<CountingGpuFrameData>(55, 11, producerFence);
    producerFence->complete(11);

    QVERIFY(sink.submitAndFlush(gpuFrame(3, data, 1), 1000));

    QCOMPARE(observed->deliveredCount(), 1);
    QCOMPARE(observed->submitAndFlushCount(), 1);
    QCOMPARE(observed->flushCount(), 0);
    QCOMPARE(data->readCount(), 1);
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::cpuSubmitAndFlushStillFlushesAfterDrainingPendingGpuReadback() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<FlushRecordingSink>();
    FlushRecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    auto producerFence = std::make_shared<ManualFence>();
    auto data = std::make_shared<CountingGpuFrameData>(56, 12, producerFence);
    QVERIFY(sink.submit(gpuFrame(4, data, 0)));
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));

    producerFence->complete(12);
    QVERIFY(sink.submitAndFlush(cpuFrame(5), 1000));

    QCOMPARE(observed->deliveredCount(), 2);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(5));
    QCOMPARE(observed->deliveredAt(1).outputFrameIndex, qint64(4));
    QCOMPARE(observed->flushCount(), 2);
    QCOMPARE(sink.readbackQueueDepth(), qint64(0));
}

void TestAsyncGpuReadbackSink::gpuReadbackDoesNotBlockSubmitTick() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    auto data = std::make_shared<BlockingGpuFrameData>();
    std::atomic<bool> submitReturned{false};
    bool submitResult = false;
    std::thread submitThread([&]() {
        submitResult = sink.submit(gpuFrame(0, data, 1));
        submitReturned.store(true, std::memory_order_release);
    });

    QVERIFY2(data->waitForReadStart(1000), "readback did not start");
    QTest::qWait(50);
    const bool returnedWhileReadbackBlocked = submitReturned.load(std::memory_order_acquire);
    data->releaseReadback();
    submitThread.join();

    QVERIFY(submitResult);
    QVERIFY2(returnedWhileReadbackBlocked, "GPU readback ran inline on the submit tick");
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);
    QCOMPARE(data->readCount(), 1);
}

void TestAsyncGpuReadbackSink::continuousCadenceResendsLastFrameWhileFencePending() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, std::make_shared<CountingGpuFrameData>(42), 1)));
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);

    auto producerFence = std::make_shared<ManualFence>();
    auto pending = std::make_shared<CountingGpuFrameData>(43, 2, producerFence);
    QVERIFY(sink.submit(gpuFrame(1, pending, 1)));

    QCOMPARE(observed->deliveredCount(), qsizetype(2));
    QCOMPARE(observed->deliveredAt(1).outputFrameIndex, qint64(0));
    QCOMPARE(pending->readCount(), 0);
    QCOMPARE(sink.readbackQueueDepth(), qint64(1));

    producerFence->complete(2);
    QVERIFY(sink.submit(gpuFrame(2, std::make_shared<CountingGpuFrameData>(44), 1)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 3, 1000);
    QCOMPARE(observed->deliveredAt(2).outputFrameIndex, qint64(1));
    QCOMPARE(pending->readCount(), 1);
}

void TestAsyncGpuReadbackSink::readyReadbackDoesNotDoubleSubmitCadence() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, std::make_shared<CountingGpuFrameData>(42), 1)));
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);

    auto data = std::make_shared<BlockingGpuFrameData>();
    QVERIFY(sink.submit(gpuFrame(1, data, 1)));
    QVERIFY2(data->waitForReadStart(1000), "readback did not start");

    const qsizetype deliveredWhileReadbackBlocked = observed->deliveredCount();

    data->releaseReadback();
    QCOMPARE(deliveredWhileReadbackBlocked, qsizetype(1));
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 2, 1000);
    QCOMPARE(observed->deliveredAt(1).outputFrameIndex, qint64(1));
    QCOMPARE(data->readCount(), 1);
}

void TestAsyncGpuReadbackSink::invalidReadbackCountsAsDropAfterCadenceResend() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* observed = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, GpuFence::create());

    QVERIFY(sink.start({}, FrameRate{}));
    QVERIFY(sink.submit(gpuFrame(0, std::make_shared<CountingGpuFrameData>(42), 1)));
    QTRY_COMPARE_WITH_TIMEOUT(observed->deliveredCount(), 1, 1000);
    QCOMPARE(observed->deliveredAt(0).outputFrameIndex, qint64(0));
    QCOMPARE(sink.readbackDrops(), qint64(0));

    QVERIFY(sink.submit(gpuFrame(1, std::make_shared<FailingGpuFrameData>(), 1)));

    QTRY_COMPARE_WITH_TIMEOUT(sink.readbackDrops(), qint64(1), 1000);
    QCOMPARE(observed->deliveredCount(), 2);
    QCOMPARE(observed->deliveredAt(1).outputFrameIndex, qint64(0));
}

QTEST_GUILESS_MAIN(TestAsyncGpuReadbackSink)
#include "tst_asyncgpureadbacksink.moc"
