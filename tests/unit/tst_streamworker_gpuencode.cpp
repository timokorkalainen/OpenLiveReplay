#include <QtTest>

#include <QTemporaryDir>

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
#include <array>
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

class BlockingFailureSurfaceEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override { return false; }

    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entered = true;
        }
        m_cv.notify_all();

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_released; });
        return false;
    }

    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }

    bool waitForCall(int timeoutMs) {
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

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_entered = false;
    bool m_released = false;
};

class DelayedSurfaceEncoder final : public NativeVideoEncoder {
public:
    explicit DelayedSurfaceEncoder(bool blockSecondCall = false)
        : m_blockSecondCall(blockSecondCall) {}

    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override { return false; }

    bool encodeSurface(GpuSurface*, int64_t ptsTicks, const ColorMetadata&,
                       const PacketCallback& onPacket, QString*) override {
        std::optional<int64_t> outputPts;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_surfaceCalls;
            m_cv.notify_all();
            if (m_blockSecondCall && m_surfaceCalls == 2)
                m_cv.wait(lock, [&] { return m_secondCallReleased; });
            outputPts = m_pendingPts;
            m_pendingPts = ptsTicks;
        }

        if (outputPts) onPacket(QByteArray::fromHex("000001b300100113"), *outputPts, true);
        return true;
    }

    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }

    bool waitForCalls(int count, int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [&] { return m_surfaceCalls >= count; });
    }

    void releaseSecondCall() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_secondCallReleased = true;
        }
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<int64_t> m_pendingPts;
    int m_surfaceCalls = 0;
    bool m_blockSecondCall = false;
    bool m_secondCallReleased = false;
};

class CountingSurfaceEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override { return false; }
    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        surfaceCalls.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }

    std::atomic<int> surfaceCalls{0};
};

class TwoPacketSurfaceEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override { return false; }
    bool encodeSurface(GpuSurface*, int64_t ptsTicks, const ColorMetadata&,
                       const PacketCallback& onPacket, QString*) override {
        onPacket(QByteArray::fromHex("000001b300100113"), ptsTicks, true);
        onPacket(QByteArray::fromHex("000001b300100114"), ptsTicks, false);
        return true;
    }
    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }
};

TimecodeEvidence gpuEvidence(int64_t frameOfDay, int64_t arrivalSessionFrame) {
    TimecodeEvidence value;
    value.frameOfDay = frameOfDay;
    value.labelRate = {30, 1};
    value.sourceGeneration = 7;
    value.timingGeneration = 11;
    value.arrivalSessionFrame = arrivalSessionFrame;
    value.sessionRate = {30, 1};
    return value;
}
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
    void delayedGpuOutputUsesEvidenceForPacketPts();
    void delayedGpuOutputAfterFallbackDropsOldEvidence();
    void resetBetweenLatestValidationAndGpuSubmissionRejectsFrame();
    void delayedOldSessionGpuFailureDoesNotLatchFallback();
    void successfulGpuImportRotatedBeforeReturnIsRejected();
    void importedGpuFrameCannotCrossLaterSameSessionRotation();
    void resetBetweenFallbackCheckAndLatchCannotDisableReplacementCarrier();
    void concurrentFallbackRacersLatchAndRotateExactlyOnce();
    void gpuTwoPacketBatchRejectsBeforePartialCommit();
    void gpuEncodeFallbackDisablesGpuFrameIngestPreference();
    void gpuFallbackRotatesTokenButRetainsLiveSession();
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

void TestStreamWorkerGpuEncode::delayedGpuOutputUsesEvidenceForPacketPts() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-delayed-gpu"), 1, 16, 16, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);
    auto encoder = std::make_unique<DelayedSurfaceEncoder>();
    auto* encoderPtr = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.m_gpuEncodePump =
        std::make_unique<GpuEncodePump>(worker.m_nativeEncoder.get(), nullptr, 4);
    worker.m_gpuEncodePump->start();
    worker.m_latestGpuFrame = makeGpuHandle();
    worker.beginCaptureSession();
    worker.m_latestGpuFrameCarrierToken = worker.snapshotActiveCarrierToken();

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int sourceIndex, uint64_t, TimecodeEvidence value) {
            QCOMPARE(sourceIndex, 0);
            delivered.append(value);
        },
        Qt::QueuedConnection));

    const TimecodeEvidence first = gpuEvidence(100, 80);
    std::atomic_store_explicit(&worker.m_latestGpuFrameTimecodeEvidence,
                               std::make_shared<const TimecodeEvidence>(first),
                               std::memory_order_release);
    worker.m_latestGpuFrameTimecode100ns.store(100, std::memory_order_release);
    worker.m_internalFrameCount = 80;
    worker.processEncoderTick(nullptr, 2666, 0, 0);
    QVERIFY2(encoderPtr->waitForCalls(1, 1000), "first GPU encode did not start");
    QCOMPARE(delivered.size(), 0);

    const TimecodeEvidence second = gpuEvidence(101, 81);
    std::atomic_store_explicit(&worker.m_latestGpuFrameTimecodeEvidence,
                               std::make_shared<const TimecodeEvidence>(second),
                               std::memory_order_release);
    worker.m_latestGpuFrameTimecode100ns.store(200, std::memory_order_release);
    worker.m_internalFrameCount = 81;
    worker.processEncoderTick(nullptr, 2700, 0, 0);

    QVERIFY2(encoderPtr->waitForCalls(2, 1000), "second GPU encode did not start");
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, 5000);
    QCOMPARE(delivered.front().frameOfDay, first.frameOfDay);
    QCOMPARE(delivered.front().arrivalSessionFrame, int64_t(80));

    worker.m_gpuEncodePump->stop();
    muxer.close();
}

void TestStreamWorkerGpuEncode::delayedGpuOutputAfterFallbackDropsOldEvidence() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-delayed-gpu-reset"), 1, 16, 16, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);
    auto encoder = std::make_unique<DelayedSurfaceEncoder>(true);
    auto* encoderPtr = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.m_gpuEncodePump =
        std::make_unique<GpuEncodePump>(worker.m_nativeEncoder.get(), nullptr, 4);
    worker.m_gpuEncodePump->start();
    worker.m_latestGpuFrame = makeGpuHandle();
    worker.beginCaptureSession();
    worker.m_latestGpuFrameCarrierToken = worker.snapshotActiveCarrierToken();

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int, uint64_t, TimecodeEvidence value) { delivered.append(value); },
        Qt::QueuedConnection));

    const TimecodeEvidence first = gpuEvidence(200, 90);
    std::atomic_store_explicit(&worker.m_latestGpuFrameTimecodeEvidence,
                               std::make_shared<const TimecodeEvidence>(first),
                               std::memory_order_release);
    worker.m_latestGpuFrameTimecode100ns.store(100, std::memory_order_release);
    worker.m_internalFrameCount = 90;
    worker.processEncoderTick(nullptr, 3000, 0, 0);
    QVERIFY2(encoderPtr->waitForCalls(1, 1000), "first GPU encode did not start");

    const TimecodeEvidence second = gpuEvidence(201, 91);
    std::atomic_store_explicit(&worker.m_latestGpuFrameTimecodeEvidence,
                               std::make_shared<const TimecodeEvidence>(second),
                               std::memory_order_release);
    worker.m_latestGpuFrameTimecode100ns.store(200, std::memory_order_release);
    worker.m_internalFrameCount = 91;
    worker.processEncoderTick(nullptr, 3033, 0, 0);
    QVERIFY2(encoderPtr->waitForCalls(2, 1000), "second GPU encode did not start");

    worker.latchGpuEncodeCpuFallback();
    encoderPtr->releaseSecondCall();
    QTRY_VERIFY_WITH_TIMEOUT(worker.m_gpuEncodePump->framesEncoded() >= 2, 2000);
    QTest::qWait(100);
    QCOMPARE(delivered.size(), 0);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    worker.m_gpuEncodePump->stop();
    muxer.close();
}

void TestStreamWorkerGpuEncode::resetBetweenLatestValidationAndGpuSubmissionRejectsFrame() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("gpu-validation-reset"), 1, 16, 16, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);
    worker.beginCaptureSession();
    auto encoder = std::make_unique<CountingSurfaceEncoder>();
    auto* encoderPtr = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.m_gpuEncodePump =
        std::make_unique<GpuEncodePump>(worker.m_nativeEncoder.get(), nullptr, 4);
    worker.m_gpuEncodePump->start();
    worker.m_latestGpuFrame = makeGpuHandle();
    worker.m_latestGpuFrameCarrierToken = worker.snapshotActiveCarrierToken();
    std::atomic_store_explicit(&worker.m_latestGpuFrameTimecodeEvidence,
                               std::make_shared<const TimecodeEvidence>(gpuEvidence(100, 1)),
                               std::memory_order_release);
    worker.m_beforeMuxEvidenceSubmissionForTest = [&worker] { worker.clearMuxFrameEvidence(); };
    const uint64_t epochBeforeReset = worker.currentCarrierEpoch();

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 33, 0, 0);
    QTest::qWait(100);

    QCOMPARE(encoderPtr->surfaceCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(worker.m_muxFrameEvidence.size(), 0);
    QVERIFY(!worker.m_gpuEncodeCpuFallback.load(std::memory_order_acquire));
    QCOMPARE(worker.currentCarrierEpoch(), epochBeforeReset + 1);
    worker.m_gpuEncodePump->stop();
    muxer.close();
}

void TestStreamWorkerGpuEncode::delayedOldSessionGpuFailureDoesNotLatchFallback() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    Muxer muxer;
    StreamWorker worker(QString(), 0, &muxer, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);
    auto encoder = std::make_unique<BlockingFailureSurfaceEncoder>();
    auto* encoderPtr = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.m_gpuEncodePump =
        std::make_unique<GpuEncodePump>(worker.m_nativeEncoder.get(), nullptr, 4);
    worker.m_gpuEncodePump->start();
    worker.m_latestGpuFrame = makeGpuHandle();
    const uint64_t oldSession = worker.beginCaptureSession();
    worker.m_latestGpuFrameCarrierToken = worker.snapshotCarrierTokenForSession(oldSession);
    QVERIFY(worker.m_latestGpuFrameCarrierToken);

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 33, 0, 0);
    QVERIFY2(encoderPtr->waitForCall(1000), "GPU encode did not enter old-session failure");

    worker.endCaptureSession(oldSession);
    const uint64_t newSession = worker.beginCaptureSession();
    QVERIFY(newSession != oldSession);
    const uint64_t replacementEpoch = worker.currentCarrierEpoch();
    encoderPtr->release();
    QTRY_VERIFY_WITH_TIMEOUT(worker.m_gpuEncodePump->queueDrops() >= 1, 2000);

    QVERIFY(!worker.m_gpuEncodeCpuFallback.load(std::memory_order_acquire));
    QCOMPARE(worker.currentCarrierEpoch(), replacementEpoch);
    worker.m_gpuEncodePump->stop();
}

void TestStreamWorkerGpuEncode::successfulGpuImportRotatedBeforeReturnIsRejected() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const uint64_t session = worker.beginCaptureSession();
    FrameMetadata metadata;
    metadata.key.format = FramePixelFormat::Nv12;
    metadata.key.width = 16;
    metadata.key.height = 16;

    worker.m_gpuImportForTest = [](void*, const FrameMetadata&) {
        return ImportedGpuVideoFrame{makeGpuHandle(), 17};
    };
    worker.m_afterGpuImportForTest = [&worker] { worker.rotateCarrier(true); };

    const ImportedGpuVideoFrame imported =
        worker.importGpuVideoFrameForSession(session, reinterpret_cast<void*>(1), metadata);
    QVERIFY(imported.frame.isNull());
    QVERIFY(!worker.m_gpuEncodeCpuFallback.load(std::memory_order_acquire));
}

void TestStreamWorkerGpuEncode::importedGpuFrameCannotCrossLaterSameSessionRotation() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const uint64_t session = worker.beginCaptureSession();
    const auto importedCarrier = worker.snapshotCarrierTokenForSession(session);
    QVERIFY(importedCarrier);

    DecodedVideoFrame decoded;
    decoded.gpuFrame = makeGpuHandle();
    decoded.gpuCarrierSessionIdentity = importedCarrier->sessionIdentity;
    decoded.gpuCarrierEpoch = importedCarrier->epoch;
    worker.rotateCarrier(true);

    worker.enqueueDecodedVideoFrameForSession(std::move(decoded), session);
    QMutexLocker frameLock(&worker.m_frameMutex);
    QVERIFY(worker.m_frameQueue.isEmpty());
}

void TestStreamWorkerGpuEncode::resetBetweenFallbackCheckAndLatchCannotDisableReplacementCarrier() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.beginCaptureSession();
    const auto failureCarrier = worker.snapshotActiveCarrierToken();
    QVERIFY(failureCarrier);
    const uint64_t submissionId =
        worker.acquireEncodeSubmission(true, 0, nullptr, nullptr, *failureCarrier, 0);
    QVERIFY(submissionId != 0);
    const uint64_t epochBeforeReset = worker.currentCarrierEpoch();

    worker.m_beforeGpuFallbackTryForTest = [&worker] { worker.rotateCarrier(true); };
    worker.failEncodeSubmission(submissionId);

    QVERIFY(!worker.m_gpuEncodeCpuFallback.load(std::memory_order_acquire));
    QCOMPARE(worker.currentCarrierEpoch(), epochBeforeReset + 1);
}

void TestStreamWorkerGpuEncode::concurrentFallbackRacersLatchAndRotateExactlyOnce() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.beginCaptureSession();
    const auto failureCarrier = worker.snapshotActiveCarrierToken();
    QVERIFY(failureCarrier);
    const uint64_t epochBeforeFallback = worker.currentCarrierEpoch();

    std::mutex gateMutex;
    std::condition_variable gateCv;
    int ready = 0;
    bool race = false;
    std::atomic<int> winners{0};
    auto racer = [&] {
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            ++ready;
            gateCv.notify_all();
            gateCv.wait(lock, [&] { return race; });
        }
        if (worker.tryLatchGpuEncodeCpuFallback(*failureCarrier))
            winners.fetch_add(1, std::memory_order_acq_rel);
    };
    std::thread first(racer);
    std::thread second(racer);
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        QVERIFY(gateCv.wait_for(lock, std::chrono::seconds(1), [&] { return ready == 2; }));
        race = true;
    }
    gateCv.notify_all();
    first.join();
    second.join();

    QCOMPARE(winners.load(std::memory_order_acquire), 1);
    QVERIFY(worker.m_gpuEncodeCpuFallback.load(std::memory_order_acquire));
    QCOMPARE(worker.currentCarrierEpoch(), epochBeforeFallback + 1);
}

void TestStreamWorkerGpuEncode::gpuTwoPacketBatchRejectsBeforePartialCommit() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-gpu-two-packet-capacity"), 1, 16, 16, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);
    worker.m_nativeEncoder = std::make_unique<TwoPacketSurfaceEncoder>();
    worker.m_gpuEncodePump =
        std::make_unique<GpuEncodePump>(worker.m_nativeEncoder.get(), nullptr, 4);
    worker.m_gpuEncodePump->start();
    worker.m_latestGpuFrame = makeGpuHandle();
    worker.beginCaptureSession();
    worker.m_latestGpuFrameCarrierToken = worker.snapshotActiveCarrierToken();
    QVERIFY(worker.m_latestGpuFrameCarrierToken);
    std::atomic_store_explicit(&worker.m_latestGpuFrameTimecodeEvidence,
                               std::make_shared<const TimecodeEvidence>(gpuEvidence(100, 1)),
                               std::memory_order_release);

    std::array<uint64_t, StreamWorker::kMuxCompletionPoolCapacity - 1> heldCompletions{};
    for (uint64_t& id : heldCompletions) {
        id = worker.reserveMuxCompletion(0);
        QVERIFY(id != 0);
    }

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 33, 0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(worker.m_gpuEncodePump->framesEncoded() >= 1, 2000);
    QTest::qWait(100);
    // processEncoderTick still admits its one silence audio packet; the rejected
    // two-packet video access unit must not consume any additional sequences.
    QCOMPARE(muxer.m_nextQueuedPacketSequence, uint64_t(2));
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    for (uint64_t id : heldCompletions)
        worker.releaseMuxCompletionReservation(id);
    worker.m_gpuEncodePump->stop();
    muxer.close();
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

void TestStreamWorkerGpuEncode::gpuFallbackRotatesTokenButRetainsLiveSession() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_RECORD_SURFACE_ENCODE", "1");

    StreamWorker worker(QString(), 0, nullptr, nullptr, 16, 16, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const uint64_t sessionIdentity = worker.beginCaptureSession();
    const auto oldToken = worker.snapshotCarrierTokenForSession(sessionIdentity);
    QVERIFY(oldToken != nullptr);

    worker.latchGpuEncodeCpuFallback();

    const auto newToken = worker.snapshotCarrierTokenForSession(sessionIdentity);
    QVERIFY(newToken != nullptr);
    QVERIFY(!worker.carrierTokenIsCurrent(oldToken));
    QVERIFY(worker.carrierTokenIsCurrent(newToken));
    QVERIFY(newToken->epoch > oldToken->epoch);
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
