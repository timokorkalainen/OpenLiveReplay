#include <QtTest>

#include "recorder_engine/ingest/ffmpegingestsession.h"
#include "recorder_engine/ingest/ingestsession.h"

class TestIngestBackendSelector : public QObject {
    Q_OBJECT
private slots:
    void defaultRoutesEverythingToFfmpeg();
    void nativeSrtFlagRoutesOnlySrtToNative();
    void canConstructFfmpegSession();
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

void TestIngestBackendSelector::canConstructFfmpegSession() {
    FfmpegIngestSession session(0);
    QVERIFY(!session.isOpen());
}

QTEST_GUILESS_MAIN(TestIngestBackendSelector)
#include "tst_ingestbackendselector.moc"
