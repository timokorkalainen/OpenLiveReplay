#include <QtTest>

#include "recorder_engine/ingest/ffmpegingestsession.h"
#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/ingest/nativefallbackpolicy.h"
#include "recorder_engine/ingest/nativesrtingestsession.h"
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
#include "recorder_engine/ingest/nativertmpingestsession.h"
#endif

#include <algorithm>

class TestIngestBackendSelector : public QObject {
    Q_OBJECT
private slots:
    void defaultRoutesEverythingToFfmpeg();
    void nativeSrtFlagRoutesOnlySrtToNative();
    void nativeRtmpFlagRoutesOnlyRtmpToNative();
    void environmentDefaultsRtmpAndRtmpsToNative();
    void legacyRtmpFfmpegOverridesAreIgnored();
    void nativeRtmpDisableEnvIsIgnored();
    void nativeFailureStopsNativeRetryWithoutFfmpegFallback();
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
    void malformedLegacyVideoPacketStaysMalformed();
    void nativeRtmpConnectAdvertisesEnhancedCodecCapabilities();
    void nativeRtmpAcknowledgesServerReceiveWindows();
    void nativeRtmpAudioUsesVideoTimelineAnchor();
    void nativeRtmpVideoUsesAudioTimelineAnchorWhenAudioArrivesFirst();
    void nativeRtmpAudioDiscontinuityUsesFreshAudioAnchor();
#endif
    void canConstructFfmpegSession();
    void nativeFailureReasonStartsEmpty();
    void nativeDecodeCapabilityErrorsRequestFallback();
    void nativeDecodeTransientErrorsDoNotRequestFallback();
    void sharedAnchorMapsStreamTime();
};

class EmptySession final : public IngestSession {
public:
    bool open(const QUrl&, const IngestCallbacks&) override { return false; }
    void run() override {}
    void requestStop() override {}
};

class ScopedEnv {
public:
    explicit ScopedEnv(const char* name) : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)),
                                           m_value(qgetenv(name)) {}
    ~ScopedEnv() {
        if (m_hadValue) {
            qputenv(m_name, m_value);
        } else {
            qunsetenv(m_name);
        }
    }

private:
    const char* m_name = nullptr;
    bool m_hadValue = false;
    QByteArray m_value;
};

void TestIngestBackendSelector::defaultRoutesEverythingToFfmpeg() {
    const IngestBackendOptions opts;

    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::Ffmpeg);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::nativeSrtFlagRoutesOnlySrtToNative() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;

    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::nativeRtmpFlagRoutesOnlyRtmpToNative() {
    IngestBackendOptions opts;
    opts.preferNativeRtmp = true;

    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::environmentDefaultsRtmpAndRtmpsToNative() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qunsetenv("OLR_NATIVE_RTMP");
    qunsetenv("OLR_FFMPEG_RTMP");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);

    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
}

void TestIngestBackendSelector::legacyRtmpFfmpegOverridesAreIgnored() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qunsetenv("OLR_NATIVE_RTMP");
    qputenv("OLR_FFMPEG_RTMP", "1");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);

    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
}

void TestIngestBackendSelector::nativeRtmpDisableEnvIsIgnored() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qputenv("OLR_FFMPEG_RTMP", "1");
    qputenv("OLR_NATIVE_RTMP", "0");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);

    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
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

void TestIngestBackendSelector::nativeRtmpAudioUsesVideoTimelineAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 100;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    QCOMPARE(session.sourcePtsMsForVideo(0, 0), int64_t(100));

    clockMs = 5000;
    QCOMPARE(session.sourcePtsMsForAudio(0), int64_t(100));
}

void TestIngestBackendSelector::nativeRtmpVideoUsesAudioTimelineAnchorWhenAudioArrivesFirst() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 100;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    QCOMPARE(session.sourcePtsMsForAudio(0), int64_t(100));

    clockMs = 5000;
    QCOMPARE(session.sourcePtsMsForVideo(0, 0), int64_t(100));
    QCOMPARE(session.sourcePtsMsForAudio(40), int64_t(140));
}

void TestIngestBackendSelector::nativeRtmpAudioDiscontinuityUsesFreshAudioAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 100;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    QCOMPARE(session.sourcePtsMsForVideo(0, 0), int64_t(100));
    QCOMPARE(session.sourcePtsMsForAudio(0), int64_t(100));

    clockMs = 5000;
    QCOMPARE(session.sourcePtsMsForAudio(10000), int64_t(5000));
}
#endif

void TestIngestBackendSelector::canConstructFfmpegSession() {
    FfmpegIngestSession session(0);
    QVERIFY(!session.isOpen());
}

void TestIngestBackendSelector::nativeFailureReasonStartsEmpty() {
    EmptySession session;
    QVERIFY(session.nativeFallbackReason().isEmpty());
}

void TestIngestBackendSelector::nativeDecodeCapabilityErrorsRequestFallback() {
    QVERIFY(nativeDecodeErrorRequestsFallback(QStringLiteral("Native decoder is unavailable.")));
    QVERIFY(nativeDecodeErrorRequestsFallback(QStringLiteral("Media Foundation decode is not implemented.")));
    QVERIFY(nativeDecodeErrorRequestsFallback(QStringLiteral("Unsupported codec profile.")));
    QVERIFY(nativeDecodeErrorRequestsFallback(QStringLiteral(
        "VideoToolbox decompression session creation failed (-12908).")));
}

void TestIngestBackendSelector::nativeDecodeTransientErrorsDoNotRequestFallback() {
    QVERIFY(!nativeDecodeErrorRequestsFallback(QStringLiteral(
        "VideoToolbox format description creation failed (-12712).")));
    QVERIFY(!nativeDecodeErrorRequestsFallback(QStringLiteral("VideoToolbox needs codec parameter sets before decoding.")));
    QVERIFY(!nativeDecodeErrorRequestsFallback(QStringLiteral("H.264 decode requires SPS/PPS before frames.")));
    QVERIFY(!nativeDecodeErrorRequestsFallback(QStringLiteral("H.265 decode requires VPS before frames.")));
    QVERIFY(!nativeDecodeErrorRequestsFallback(QStringLiteral("Native decoder received an empty access unit.")));
}

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
