#include <QtTest>

#include "recorder_engine/ingest/decodedframeevidencequeue.h"
#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/ingest/nativevideodecoder.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

class TestNativeVideoDecoder : public QObject {
    Q_OBJECT
private slots:
    void defaultCapabilitiesAreFalse();
    void queryCapabilitiesReportsPlatformBackend();
    void keepSurfaceNullImageBufferIsRejected();
    void videoToolboxNoFrameIsRejected();
    void mediaFoundationCallbacksUseInputPtsDomain();
    void mediaFoundationCallbacksPreserveEvidenceKeys_data();
    void mediaFoundationCallbacksPreserveEvidenceKeys();
    void flushExcessPixelBufferPoolNoOpsWithoutSession();
};

namespace {

[[maybe_unused]] DecodedFrameEvidence callbackEvidence(qint64 pts90k, int64_t marker) {
    return {pts90k, marker, marker * 10, std::nullopt};
}

} // namespace

void TestNativeVideoDecoder::defaultCapabilitiesAreFalse() {
    const NativeVideoDecodeCapabilities caps;
    QVERIFY(!caps.h264);
    QVERIFY(!caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(caps.detail.isEmpty());
}

void TestNativeVideoDecoder::queryCapabilitiesReportsPlatformBackend() {
    const NativeVideoDecodeCapabilities caps = queryNativeVideoDecodeCapabilities();
#if defined(Q_OS_WIN)
    QVERIFY(!caps.detail.isEmpty());
#else
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_TVOS) || defined(Q_OS_WATCHOS)
    QVERIFY(caps.h264);
    QVERIFY(caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(caps.detail.contains(QStringLiteral("VideoToolbox")) || !caps.detail.isEmpty());
#else
    QVERIFY(!caps.h264);
    QVERIFY(!caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(!caps.detail.isEmpty());
#endif
#endif
}

void TestNativeVideoDecoder::keepSurfaceNullImageBufferIsRejected() {
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_TVOS) || defined(Q_OS_WATCHOS)
    QVERIFY(nativeVideoDecoderKeepSurfaceNullImageRejectedForTest());
#else
    QSKIP("VideoToolbox null-image callback seam is Apple-only");
#endif
}

void TestNativeVideoDecoder::videoToolboxNoFrameIsRejected() {
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_TVOS) || defined(Q_OS_WATCHOS)
    QString error;
    QVERIFY(!nativeVideoDecoderNoFrameRejectedForTest(&error));
    QVERIFY(error.contains(QStringLiteral("produced no frame")));
#else
    QSKIP("VideoToolbox no-frame validation seam is Apple-only");
#endif
}

void TestNativeVideoDecoder::mediaFoundationCallbacksUseInputPtsDomain() {
#if defined(Q_OS_WIN)
    constexpr qint64 kInputPts90k = 180'000;
    qint64 cpuPts90k = -1;
    qint64 surfacePts90k = -1;
    QString error;
    QVERIFY2(nativeVideoDecoderMediaFoundationDeliverOutputForTest(
                 kInputPts90k, -1,
                 [&cpuPts90k](AVFrame* frame) {
                     cpuPts90k = frame->pts;
                     av_frame_free(&frame);
                 },
                 {}, &error),
             qPrintable(error));
    QVERIFY2(nativeVideoDecoderMediaFoundationDeliverOutputForTest(
                 kInputPts90k, -1, {},
                 [&surfacePts90k](void*, qint64 pts90k) {
                     surfacePts90k = pts90k;
                     return true;
                 },
                 &error),
             qPrintable(error));
    QCOMPARE(cpuPts90k, kInputPts90k);
    QCOMPARE(surfacePts90k, kInputPts90k);
#else
    QSKIP("Media Foundation PTS-domain seam is Windows-only");
#endif
}

void TestNativeVideoDecoder::mediaFoundationCallbacksPreserveEvidenceKeys_data() {
    QTest::addColumn<bool>("keepSurface");
    QTest::newRow("cpu") << false;
    QTest::newRow("keep-surface") << true;
}

void TestNativeVideoDecoder::mediaFoundationCallbacksPreserveEvidenceKeys() {
#if defined(Q_OS_WIN)
    QFETCH(bool, keepSurface);

    int64_t unmatchedCpuSourcePtsMs = -999;
    bool unmatchedCpuHasTimecodeEvidence = true;

    auto deliver = [keepSurface, &unmatchedCpuSourcePtsMs, &unmatchedCpuHasTimecodeEvidence](
                       qint64 samplePts90k, DecodedFrameEvidenceQueue* queue,
                       qint64* deliveredPts90k, std::optional<DecodedFrameEvidence>* matched) {
        QString error;
        NativeVideoDecoder::FrameCallback onFrame;
        NativeVideoDecoder::KeepSurfaceCallback onSurface;
        if (keepSurface) {
            onSurface = [queue, deliveredPts90k, matched](void*, qint64 pts90k) {
                *deliveredPts90k = pts90k;
                *matched = queue->takeForOutputPts(pts90k);
                return true;
            };
        } else {
            onFrame = [queue, deliveredPts90k, matched, &unmatchedCpuSourcePtsMs,
                       &unmatchedCpuHasTimecodeEvidence](AVFrame* frame) {
                *matched = queue->takeForOutputPts(frame->pts);
                DecodedVideoFrame decodedFrame =
                    decodedCpuVideoFrameForOutput(frame, *matched ? &**matched : nullptr);
                *deliveredPts90k = decodedFrame.frame->pts;
                if (!*matched) {
                    unmatchedCpuSourcePtsMs = decodedFrame.sourcePtsMs;
                    unmatchedCpuHasTimecodeEvidence = decodedFrame.timecodeEvidence.has_value();
                }
                av_frame_free(&decodedFrame.frame);
            };
        }
        return nativeVideoDecoderMediaFoundationDeliverOutputForTest(
            samplePts90k, AV_NOPTS_VALUE, std::move(onFrame), std::move(onSurface), &error);
    };

    // 3000 and the 3003-tick 30000/1001 cadence are not integer 100 ns values.
    // Deliver them out of order so the callback PTS, rather than submission order,
    // must select the exact evidence record.
    DecodedFrameEvidenceQueue reordered(64);
    QVector<qint64> cadence;
    for (int index = 0; index < 24; ++index) {
        const qint64 pts90k = 3000 + qint64(index) * 3003;
        cadence.append(pts90k);
        reordered.enqueue(callbackEvidence(pts90k, index + 1));
    }
    std::swap(cadence[0], cadence[1]);
    std::swap(cadence[10], cadence[11]);
    for (const qint64 expectedPts90k : cadence) {
        qint64 deliveredPts90k = 0;
        std::optional<DecodedFrameEvidence> matched;
        QVERIFY(deliver(expectedPts90k, &reordered, &deliveredPts90k, &matched));
        QCOMPARE(deliveredPts90k, expectedPts90k);
        QVERIFY(matched.has_value());
        QCOMPARE(matched->codecPts90k, expectedPts90k);
    }
    QCOMPARE(reordered.size(), qsizetype(0));

    DecodedFrameEvidenceQueue duplicates(4);
    duplicates.enqueue(callbackEvidence(3000, 101));
    duplicates.enqueue(callbackEvidence(3000, 102));
    for (const int64_t expectedMarker : {int64_t(101), int64_t(102)}) {
        qint64 deliveredPts90k = 0;
        std::optional<DecodedFrameEvidence> matched;
        QVERIFY(deliver(3000, &duplicates, &deliveredPts90k, &matched));
        QCOMPARE(deliveredPts90k, qint64(3000));
        QVERIFY(matched.has_value());
        QCOMPARE(matched->sourcePtsMs, expectedMarker);
    }

    DecodedFrameEvidenceQueue miss(4);
    miss.enqueue(callbackEvidence(3000, 201));
    qint64 deliveredMissPts90k = 0;
    std::optional<DecodedFrameEvidence> matchedMiss;
    QVERIFY(deliver(3003, &miss, &deliveredMissPts90k, &matchedMiss));
    QCOMPARE(deliveredMissPts90k, qint64(3003));
    QVERIFY(!matchedMiss.has_value());
    QCOMPARE(miss.size(), qsizetype(1));
    if (!keepSurface) {
        QCOMPARE(unmatchedCpuSourcePtsMs, int64_t(33));
        QVERIFY(!unmatchedCpuHasTimecodeEvidence);
    }

    qint64 deliveredNoPts = 0;
    std::optional<DecodedFrameEvidence> matchedNoPts;
    QVERIFY(deliver(AV_NOPTS_VALUE, &miss, &deliveredNoPts, &matchedNoPts));
    QCOMPARE(deliveredNoPts, qint64(AV_NOPTS_VALUE));
    QVERIFY(!matchedNoPts.has_value());
    QCOMPARE(miss.size(), qsizetype(1));
#else
    QSKIP("Media Foundation callback association is Windows-only");
#endif
}

void TestNativeVideoDecoder::flushExcessPixelBufferPoolNoOpsWithoutSession() {
    NativeVideoDecoder decoder(0, 0);
    decoder.flushExcessPixelBufferPool();
}

QTEST_GUILESS_MAIN(TestNativeVideoDecoder)
#include "tst_nativevideodecoder.moc"
