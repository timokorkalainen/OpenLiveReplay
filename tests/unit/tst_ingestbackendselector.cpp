#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/ingest/nativesrtingestsession.h"
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
#include "recorder_engine/ingest/nativertmpingestsession.h"
#endif

#include <algorithm>

class TestIngestBackendSelector : public QObject {
    Q_OBJECT
private slots:
    void srtRoutesToNativeSrt();
    void rtmpRoutesToNativeRtmp();
    void unsupportedSchemesAreRejected();
    void srtIsNativeByDefaultWithoutEnv();
    void rtmpIsNativeByDefault();
    void nativeFailureStopsNativeRetryWithoutFfmpegFallback();
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
    void malformedLegacyVideoPacketStaysMalformed();
    void nativeRtmpConnectAdvertisesEnhancedCodecCapabilities();
    void nativeRtmpAcknowledgesServerReceiveWindows();
    void nativeRtmpAudioFollowsSharedVideoAnchor();
    void nativeRtmpVideoBindsToSharedAnchorWhenAudioArrivesFirst();
    void nativeRtmpAudioFollowsVideoReanchorWithoutDrift();
    void nativeRtmpAudioDiscontinuityKeepsSharedAnchor();
#endif
    void sharedAnchorMapsStreamTime();
};

void TestIngestBackendSelector::srtRoutesToNativeSrt() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::rtmpRoutesToNativeRtmp() {
    IngestBackendOptions opts;
    opts.preferNativeRtmp = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
}
void TestIngestBackendSelector::unsupportedSchemesAreRejected() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    opts.preferNativeRtmp = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("udp://127.0.0.1:1234")), opts),
             IngestBackendKind::Unsupported);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("file:///tmp/x.ts")), opts),
             IngestBackendKind::Unsupported);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("http://example.test/x")), opts),
             IngestBackendKind::Unsupported);
}
void TestIngestBackendSelector::srtIsNativeByDefaultWithoutEnv() {
    qunsetenv("OLR_NATIVE_SRT");
    const IngestBackendOptions opts = ingestBackendOptionsFromEnvironment(
        QUrl(QStringLiteral("srt://127.0.0.1:9000")), true, false);
    QVERIFY(opts.preferNativeSrt);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::rtmpIsNativeByDefault() {
    const IngestBackendOptions opts = ingestBackendOptionsFromEnvironment(
        QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), false, true);
    QVERIFY(opts.preferNativeRtmp);
}

void TestIngestBackendSelector::nativeFailureStopsNativeRetryWithoutFfmpegFallback() {
    QVERIFY(shouldStopNativeRtmpAfterFailure(IngestFailureKind::UnsupportedProfile));
    QVERIFY(shouldStopNativeRtmpAfterFailure(IngestFailureKind::DecodeCapability));
    QVERIFY(shouldStopNativeRtmpAfterFailure(IngestFailureKind::MalformedStream));
    QVERIFY(!shouldStopNativeRtmpAfterFailure(IngestFailureKind::TransientNetwork));
    QVERIFY(!shouldStopNativeRtmpAfterFailure(IngestFailureKind::None));
}

#if defined(OLR_NATIVE_RTMP_AVAILABLE)
void TestIngestBackendSelector::malformedLegacyVideoPacketStaysMalformed() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    QStringList logLines;
    IngestCallbacks callbacks;
    callbacks.logInfo = [&logLines](const QString& message) { logLines.append(message); };
    session.m_callbacks = callbacks;

    session.processVideoMessage(0, QByteArray::fromHex("02"));

    QCOMPARE(session.lastFailureKind(), IngestFailureKind::MalformedStream);
    QVERIFY(session.m_unsupportedReason.isEmpty());
    QVERIFY(std::any_of(logLines.cbegin(), logLines.cend(), [](const QString& message) {
        return message.contains(QStringLiteral("Native RTMP video parse failed"));
    }));
}

void TestIngestBackendSelector::nativeRtmpConnectAdvertisesEnhancedCodecCapabilities() {
    QCOMPARE(NativeRtmpIngestSession::connectCodecProfile(),
             RtmpConnectCodecProfile::EnhancedAvcHevcAac);
}

void TestIngestBackendSelector::nativeRtmpAcknowledgesServerReceiveWindows() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    session.configureAcknowledgementWindow(10);
    quint32 sequence = 0;
    QVERIFY(!session.noteIncomingChunkBytes(9, &sequence));
    QCOMPARE(sequence, quint32(0));
    QVERIFY(session.noteIncomingChunkBytes(1, &sequence));
    QCOMPARE(sequence, quint32(10));
    QVERIFY(!session.noteIncomingChunkBytes(9, &sequence));
    QCOMPARE(sequence, quint32(0));
    QVERIFY(session.noteIncomingChunkBytes(1, &sequence));
    QCOMPARE(sequence, quint32(20));

    NativeRtmpIngestSession alreadyReadSession(0, 640, 480, nullptr);
    QVERIFY(!alreadyReadSession.noteIncomingChunkBytes(12, &sequence));
    alreadyReadSession.configureAcknowledgementWindow(10);
    QVERIFY(alreadyReadSession.noteIncomingChunkBytes(0, &sequence));
    QCOMPARE(sequence, quint32(12));
}

void TestIngestBackendSelector::nativeRtmpAudioFollowsSharedVideoAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 10000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // First video establishes the shared anchor: FLV ms 1000 -> recording clock 10000.
    QCOMPARE(session.sourcePtsMsForVideo(1000, 1000), int64_t(10000));

    // Co-timed audio binds to the SAME shared anchor; it must not read a fresh clock.
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(1000), int64_t(10000));

    // Later audio advances against the unchanged anchor.
    QCOMPARE(session.sourcePtsMsForAudio(1100), int64_t(10100));
}

void TestIngestBackendSelector::nativeRtmpVideoBindsToSharedAnchorWhenAudioArrivesFirst() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 100;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // Audio with no prior video establishes the shared anchor against the clock.
    QCOMPARE(session.sourcePtsMsForAudio(0), int64_t(100));

    // A later co-timed video maps against THAT shared anchor, not a fresh clock read.
    clockMs = 5000;
    QCOMPARE(session.sourcePtsMsForVideo(0, 0), int64_t(100));
    QCOMPARE(session.sourcePtsMsForAudio(40), int64_t(140));
}

void TestIngestBackendSelector::nativeRtmpAudioFollowsVideoReanchorWithoutDrift() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 10000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // Shared anchor established by first video at FLV ms 1000 -> clock 10000.
    QCOMPARE(session.sourcePtsMsForVideo(1000, 1000), int64_t(10000));
    QCOMPARE(session.sourcePtsMsForAudio(1000), int64_t(10000));

    // Video forward jump (delta 8000 > kForwardJumpMs=3000) re-anchors to clock 20000.
    clockMs = 20000;
    QCOMPARE(session.sourcePtsMsForVideo(9000, 9000), int64_t(20000));

    // Audio at the same FLV clock FOLLOWS the new shared anchor: no A/V drift.
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(9000), int64_t(20000));
}

void TestIngestBackendSelector::nativeRtmpAudioDiscontinuityKeepsSharedAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 10000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // Shared anchor established by first video at FLV ms 1000 -> clock 10000.
    QCOMPARE(session.sourcePtsMsForVideo(1000, 1000), int64_t(10000));

    // Audio advances normally; m_prevAudioPtsMs becomes 1000.
    QCOMPARE(session.sourcePtsMsForAudio(1000), int64_t(10000));

    // An AUDIO discontinuity NOT co-timed with any video re-anchor (delta
    // 5000 - 1000 = 4000 > kForwardJumpMs=3000) flushes the audio decoder but
    // must NOT move the shared anchor — video owns re-anchoring. The clock is
    // moved to a sentinel the audio path must ignore: if it wrongly re-anchored
    // to the fresh clock the result would be 99999, not 14000.
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(5000), int64_t(14000));

    // The shared anchor is unchanged (media 1000 / stream 10000), so the next
    // audio packet still maps against it.
    QCOMPARE(session.sourcePtsMsForAudio(5040), int64_t(14040));
}
#endif

void TestIngestBackendSelector::sharedAnchorMapsStreamTime() {
    // The single shared A/V anchor maps a 90kHz stream timestamp onto the recording
    // timeline: sourceMs = anchorStreamMs + (pts90k - anchorTs90k)/90; -1 on any
    // negative input. (1000 + (108000-90000)/90 = 1200.)
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(108000, 90000, 1000), 1200);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(-1, 90000, 1000), -1);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(108000, -1, 1000), -1);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(108000, 90000, -1), -1);
}

QTEST_GUILESS_MAIN(TestIngestBackendSelector)
#include "tst_ingestbackendselector.moc"
