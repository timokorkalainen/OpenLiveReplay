#include <QtTest>

#include "recorder_engine/ingest/ffmpegingestsession.h"
#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/ingest/nativefallbackpolicy.h"
#include "recorder_engine/ingest/nativesrtingestsession.h"

class TestIngestBackendSelector : public QObject {
    Q_OBJECT
private slots:
    void defaultRoutesEverythingToFfmpeg();
    void nativeSrtFlagRoutesOnlySrtToNative();
    void nativeRtmpFlagRoutesOnlyRtmpToNative();
    void environmentKeepsRtmpOnFfmpegByDefaultUntilReady();
    void environmentOptInRoutesRtmpAndRtmpsToNative();
    void environmentCanForceRtmpBackToFfmpeg();
    void nativeFailureFallbackPolicy();
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

void TestIngestBackendSelector::environmentKeepsRtmpOnFfmpegByDefaultUntilReady() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qunsetenv("OLR_NATIVE_RTMP");
    qunsetenv("OLR_FFMPEG_RTMP");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);

    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::environmentOptInRoutesRtmpAndRtmpsToNative() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qputenv("OLR_NATIVE_RTMP", "1");
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

void TestIngestBackendSelector::environmentCanForceRtmpBackToFfmpeg() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qputenv("OLR_FFMPEG_RTMP", "1");
    qunsetenv("OLR_NATIVE_RTMP");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);

    qunsetenv("OLR_FFMPEG_RTMP");
    qputenv("OLR_NATIVE_RTMP", "0");
    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::nativeFailureFallbackPolicy() {
    QVERIFY(shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::UnsupportedProfile));
    QVERIFY(shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::DecodeCapability));
    QVERIFY(shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::MalformedStream));
    QVERIFY(!shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::TransientNetwork));
    QVERIFY(!shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::None));
}

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
