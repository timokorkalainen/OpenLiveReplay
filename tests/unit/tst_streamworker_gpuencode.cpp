#include <QtTest>

#if defined(OLR_GPU_PIPELINE_BUILD) && defined(__APPLE__)
#include "playback/gpu/appleiosurface.h"
#endif
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpusurface.h"
#include "recorder_engine/codec/gpuencodepump.h"
#include "recorder_engine/ingest/gpudecodedframe.h"
#include "recorder_engine/streamworker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#if defined(OLR_GPU_PIPELINE_BUILD) && defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#if __has_include(<IOSurface/IOSurfaceRef.h>)
#include <IOSurface/IOSurfaceRef.h>
#elif __has_include(<IOSurface/IOSurface.h>)
#include <IOSurface/IOSurface.h>
#endif
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

#ifdef OLR_GPU_PIPELINE_BUILD
AVFrame* makeYuvFrame(int width, int height, int64_t pts) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    frame->pts = pts;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}
#endif

#ifdef OLR_GPU_PIPELINE_BUILD
class FakeSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return const_cast<FakeSurface*>(this); }
};

FrameHandle makeGpuHandle() {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;
    return makeGpuFrameHandle(std::make_shared<FakeSurface>(), nullptr, meta);
}

class BlockingSurfaceEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override { return false; }

    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        surfaceCalls.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entered = true;
        }
        m_cv.notify_all();

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_released; });
        // This fake only exercises pump queueing. A successful submission that
        // produces no immediate packet also models a hardware encoder buffering
        // output for a later call, without involving an uninitialized test Muxer.
        return true;
    }

    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }

    bool waitForFirstCall(int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return m_entered; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_cv.notify_all();
    }

    std::atomic<int> surfaceCalls{0};

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_entered = false;
    bool m_released = false;
};
#endif

} // namespace

class TestStreamWorkerGpuEncode : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void pumpIsNullWhenPipelineFlagOff();
#ifdef OLR_GPU_PIPELINE_BUILD
    void jitterPullCarriesGpuFrameAndClearsOnCpuFrame();
    void paintBlueClearsGpuOnlyLatestFrame();
    void gpuDecodedFrameHelperWrapsAppleSurface();
    void gpuDecodedFrameHelperChargesIngestWrap();
    void gpuEncodeImportChargesRecorderWrap();
    void gpuEncodePumpStartsWhenGpuPipelineEnabled();
    void queuesGpuEncodeWhilePreviousSurfaceEncodeIsInFlight();
    void gpuEncodeFallbackDisablesGpuFrameIngestPreference();
    void appleDefaultsToCpuIngestWhenGpuPipelineEnabled();
    void gpuRecordSurfaceEncodeOptInPrefersGpuFramesWhereSupported();
    void gpuOnlyQueuedFrameBeforeEncodeFallbackDoesNotClearCpuLatest();
    void gpuOnlyQueuedFrameAfterEncodeFallbackDoesNotClearCpuLatest();
    void frameQueueBackstopHonorsByteCap();
#endif
};

void TestStreamWorkerGpuEncode::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
    qunsetenv("OLR_GPU_RECORD_SURFACE_ENCODE");
    qunsetenv("OLR_FRAME_QUEUE_BACKSTOP_MB");
}

void TestStreamWorkerGpuEncode::pumpIsNullWhenPipelineFlagOff() {
    qunsetenv("OLR_GPU_PIPELINE");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 320, 240, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    QVERIFY(worker.gpuEncodePumpForTest() == nullptr);
}

#ifdef OLR_GPU_PIPELINE_BUILD
void TestStreamWorkerGpuEncode::jitterPullCarriesGpuFrameAndClearsOnCpuFrame() {
    qputenv("OLR_GPU_PIPELINE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_latestFrame = av_frame_alloc();
    QVERIFY(worker.m_latestFrame != nullptr);

    StreamWorker::QueuedFrame gpuQueued;
    gpuQueued.frame = makeYuvFrame(16, 16, 0);
    QVERIFY(gpuQueued.frame != nullptr);
    gpuQueued.sourcePts = 0;
    gpuQueued.gpuFrame = makeGpuHandle();
    gpuQueued.gpuFenceValue = 42;

    {
        QMutexLocker locker(&worker.m_frameMutex);
        worker.m_frameQueue.enqueue(gpuQueued);
    }

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 0, 0, 0);

    QVERIFY(worker.m_latestGpuFrame.isGpuBacked());
    QCOMPARE(worker.m_latestGpuFenceValue, uint64_t(42));

    StreamWorker::QueuedFrame cpuQueued;
    cpuQueued.frame = makeYuvFrame(16, 16, 40);
    QVERIFY(cpuQueued.frame != nullptr);
    cpuQueued.sourcePts = 40;

    {
        QMutexLocker locker(&worker.m_frameMutex);
        worker.m_frameQueue.enqueue(cpuQueued);
    }

    worker.m_internalFrameCount = 3;
    worker.processEncoderTick(nullptr, 0, 0, 0);

    QVERIFY(worker.m_latestGpuFrame.isNull());
    QCOMPARE(worker.m_latestGpuFenceValue, uint64_t(0));

    av_frame_free(&worker.m_latestFrame);
}

void TestStreamWorkerGpuEncode::paintBlueClearsGpuOnlyLatestFrame() {
    qputenv("OLR_GPU_PIPELINE", "1");

    StreamWorker worker(QStringLiteral("old-source"), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_latestGpuFrame = makeGpuHandle();
    worker.m_latestGpuFenceValue = 7;
    worker.m_latestGpuFrameTimecode100ns = 5678;
    worker.m_latestFrameTimecode100ns = 1234;
    worker.m_paintBlue = 1;

    worker.processEncoderTick(nullptr, 0, 0, 0);

    QVERIFY(worker.m_latestGpuFrame.isNull());
    QCOMPARE(worker.m_latestGpuFenceValue, uint64_t(0));
    QCOMPARE(worker.m_latestGpuFrameTimecode100ns.load(std::memory_order_acquire), int64_t(-1));
    QCOMPARE(worker.m_latestFrameTimecode100ns.load(std::memory_order_acquire), int64_t(-1));
}

void TestStreamWorkerGpuEncode::gpuDecodedFrameHelperWrapsAppleSurface() {
#ifndef __APPLE__
    QSKIP("GPU decoded-frame helper currently wraps Apple CVImageBuffer surfaces");
#else
    auto surface = makeAppleNv12Surface(16, 16);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");

    auto* ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    CVPixelBufferRef pixelBuffer = nullptr;
    const CVReturn rc =
        CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pixelBuffer);
    if (rc != kCVReturnSuccess || !pixelBuffer) {
        QSKIP("could not create a CVPixelBuffer wrapper for IOSurface");
    }

    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    const FrameHandle handle = makeGpuDecodedFrameHandle(pixelBuffer, unit, 16, 16, 123);
    CVPixelBufferRelease(pixelBuffer);

    QVERIFY(handle.isGpuBacked());
    QCOMPARE(handle.metadata().key.format, FramePixelFormat::Nv12);
    QCOMPARE(handle.metadata().key.width, 16);
    QCOMPARE(handle.metadata().key.height, 16);
    QCOMPARE(handle.metadata().key.ptsMs, qint64(123));
    QCOMPARE(handle.metadata().gpuGeneration, GpuGenerationCounter::instance().current());
#endif
}

void TestStreamWorkerGpuEncode::gpuDecodedFrameHelperChargesIngestWrap() {
#ifndef __APPLE__
    QSKIP("GPU decoded-frame helper currently wraps Apple CVImageBuffer surfaces");
#else
    GpuBudget::instance().reset();

    auto surface = makeAppleNv12Surface(16, 16);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");
    const qint64 bytes = gpuSurfaceBytes(*surface);

    auto* ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    CVPixelBufferRef pixelBuffer = nullptr;
    const CVReturn rc =
        CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pixelBuffer);
    if (rc != kCVReturnSuccess || !pixelBuffer) {
        QSKIP("could not create a CVPixelBuffer wrapper for IOSurface");
    }

    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    {
        const FrameHandle handle = makeGpuDecodedFrameHandle(pixelBuffer, unit, 16, 16, 123);
        QVERIFY(handle.isGpuBacked());
        QCOMPARE(GpuBudget::instance().liveBytes(GpuBudgetTag::IngestWrap), bytes);
        QCOMPARE(GpuBudget::instance().gatedLiveBytes(), qint64(0));
    }
    CVPixelBufferRelease(pixelBuffer);
    QCOMPARE(GpuBudget::instance().liveBytes(GpuBudgetTag::IngestWrap), qint64(0));
#endif
}

void TestStreamWorkerGpuEncode::gpuEncodeImportChargesRecorderWrap() {
#ifndef __APPLE__
    QSKIP("GPU encode import currently wraps Apple CVImageBuffer surfaces in this test");
#else
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuBudget::instance().reset();

    auto surface = makeAppleNv12Surface(16, 16);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");
    const qint64 bytes = gpuSurfaceBytes(*surface);

    auto* ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    CVPixelBufferRef pixelBuffer = nullptr;
    const CVReturn rc =
        CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pixelBuffer);
    if (rc != kCVReturnSuccess || !pixelBuffer) {
        QSKIP("could not create a CVPixelBuffer wrapper for IOSurface");
    }

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;

    {
        const ImportedGpuVideoFrame imported =
            worker.importGpuVideoFrameForEncode(pixelBuffer, meta);
        QVERIFY(imported.frame.isGpuBacked());
        QCOMPARE(GpuBudget::instance().liveBytes(GpuBudgetTag::RecorderWrap), bytes);
        QCOMPARE(GpuBudget::instance().gatedLiveBytes(), qint64(0));
    }
    CVPixelBufferRelease(pixelBuffer);
    QCOMPARE(GpuBudget::instance().liveBytes(GpuBudgetTag::RecorderWrap), qint64(0));
#endif
}

void TestStreamWorkerGpuEncode::gpuEncodePumpStartsWhenGpuPipelineEnabled() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_nativeEncoder = std::make_unique<BlockingSurfaceEncoder>();

    QVERIFY(worker.ensureGpuEncodePumpStartedForTest());
    QVERIFY(worker.gpuEncodePumpForTest() != nullptr);
}

void TestStreamWorkerGpuEncode::queuesGpuEncodeWhilePreviousSurfaceEncodeIsInFlight() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    Muxer muxer;
    StreamWorker worker(QString(), 0, &muxer, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);

    auto encoder = std::make_unique<BlockingSurfaceEncoder>();
    auto* encoderPtr = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.m_gpuEncodePump =
        std::make_unique<GpuEncodePump>(worker.m_nativeEncoder.get(), nullptr, 4);
    worker.m_gpuEncodePump->start();
    worker.m_latestGpuFrame = makeGpuHandle();

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 33, 0, 0);
    QVERIFY2(encoderPtr->waitForFirstCall(1000), "first GPU encode did not start");

    worker.m_internalFrameCount = 2;
    worker.processEncoderTick(nullptr, 66, 0, 0);

    encoderPtr->release();
    QTRY_COMPARE_WITH_TIMEOUT(encoderPtr->surfaceCalls.load(std::memory_order_acquire), 2, 2000);
    worker.m_gpuEncodePump->stop();
}

void TestStreamWorkerGpuEncode::gpuEncodeFallbackDisablesGpuFrameIngestPreference() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    QVERIFY(worker.preferGpuVideoFramesForIngestForTest());

    worker.latchGpuEncodeCpuFallback();

    QVERIFY(!worker.preferGpuVideoFramesForIngestForTest());
}

void TestStreamWorkerGpuEncode::appleDefaultsToCpuIngestWhenGpuPipelineEnabled() {
    qputenv("OLR_GPU_PIPELINE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);

#if defined(__APPLE__)
    QVERIFY(!worker.preferGpuVideoFramesForIngestForTest());
#else
    QVERIFY(worker.preferGpuVideoFramesForIngestForTest());
#endif
}

void TestStreamWorkerGpuEncode::gpuRecordSurfaceEncodeOptInPrefersGpuFramesWhereSupported() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);

#if defined(Q_OS_IOS)
    // iOS GPU playback is enabled by default, but recorder surface encode remains disabled until
    // the live ingest path has a proven non-stalling VideoToolbox surface-encode strategy.
    QVERIFY(!worker.preferGpuVideoFramesForIngestForTest());
#else
    QVERIFY(worker.preferGpuVideoFramesForIngestForTest());
#endif
}

void TestStreamWorkerGpuEncode::gpuOnlyQueuedFrameBeforeEncodeFallbackDoesNotClearCpuLatest() {
    qputenv("OLR_GPU_PIPELINE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_latestFrame = makeYuvFrame(16, 16, 0);
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_latestFrameTimecode100ns = 1000;

    StreamWorker::QueuedFrame gpuOnly;
    gpuOnly.sourcePts = 0;
    gpuOnly.sourceTimecode100ns = 2000;
    gpuOnly.gpuFrame = makeGpuHandle();
    {
        QMutexLocker locker(&worker.m_frameMutex);
        worker.m_frameQueue.enqueue(gpuOnly);
    }

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 0, 0, 0);

    QVERIFY(worker.m_latestFrame != nullptr);
    QVERIFY(worker.m_latestFrame->data[0] != nullptr);
    QCOMPARE(worker.m_latestFrameTimecode100ns.load(std::memory_order_acquire), int64_t(1000));
    QVERIFY(worker.m_latestGpuFrame.isGpuBacked());
    QCOMPARE(worker.m_latestGpuFrameTimecode100ns.load(std::memory_order_acquire), int64_t(2000));

    av_frame_free(&worker.m_latestFrame);
}

void TestStreamWorkerGpuEncode::gpuOnlyQueuedFrameAfterEncodeFallbackDoesNotClearCpuLatest() {
    qputenv("OLR_GPU_PIPELINE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_latestFrame = makeYuvFrame(16, 16, 0);
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_gpuEncodeCpuFallback.store(true, std::memory_order_release);

    StreamWorker::QueuedFrame gpuOnly;
    gpuOnly.sourcePts = 0;
    gpuOnly.gpuFrame = makeGpuHandle();
    {
        QMutexLocker locker(&worker.m_frameMutex);
        worker.m_frameQueue.enqueue(gpuOnly);
    }

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 0, 0, 0);

    QVERIFY(worker.m_latestFrame != nullptr);
    QVERIFY(worker.m_latestFrame->data[0] != nullptr);
    QCOMPARE(worker.m_frameQueue.size(), 0);

    av_frame_free(&worker.m_latestFrame);
}

void TestStreamWorkerGpuEncode::frameQueueBackstopHonorsByteCap() {
    qputenv("OLR_FRAME_QUEUE_BACKSTOP_MB", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 640, 480, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);

    for (int i = 0; i < 3; ++i) {
        StreamWorker::QueuedFrame queued;
        queued.frame = makeYuvFrame(640, 480, i * 40);
        QVERIFY(queued.frame != nullptr);
        queued.sourcePts = i * 40;
        worker.m_frameQueue.enqueue(queued);
    }

    worker.trimFrameQueueBackstopLocked(-1);

    QCOMPARE(worker.m_frameQueue.size(), 2);
    QCOMPARE(worker.m_frameQueue.at(0).sourcePts, int64_t(40));
    QCOMPARE(worker.m_frameQueue.at(1).sourcePts, int64_t(80));

    while (!worker.m_frameQueue.isEmpty()) {
        StreamWorker::QueuedFrame queued = worker.m_frameQueue.dequeue();
        av_frame_free(&queued.frame);
    }
}
#endif

QTEST_GUILESS_MAIN(TestStreamWorkerGpuEncode)
#include "tst_streamworker_gpuencode.moc"
