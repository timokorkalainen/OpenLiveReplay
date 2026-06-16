#include <QtTest>
#include <QUrl>
#include <QUrlQuery>

#include "recorder_engine/ingest/ingestsession.h"

class TestSrtOptions : public QObject {
    Q_OBJECT
private slots:
    void srtUrlGetsAllOptions();
    void latencyIsMicrosecondsForFfmpeg();
    void connectTimeoutIsMilliseconds();
    void nonSrtUrlUntouched();
    void existingOptionPreserved();
    void caseInsensitiveScheme();
    void jitterWindowSrtUsesFloor();
    void jitterWindowNonSrtUsesDefault();
};

void TestSrtOptions::srtUrlGetsAllOptions() {
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))));
    QVERIFY(q.hasQueryItem(QStringLiteral("latency")));
    QVERIFY(q.hasQueryItem(QStringLiteral("rcvlatency")));
    QVERIFY(q.hasQueryItem(QStringLiteral("peerlatency")));
    QCOMPARE(q.queryItemValue(QStringLiteral("transtype")), QStringLiteral("live"));
    QVERIFY(q.hasQueryItem(QStringLiteral("connect_timeout")));
    QCOMPARE(q.queryItemValue(QStringLiteral("linger")), QStringLiteral("0"));
}

void TestSrtOptions::latencyIsMicrosecondsForFfmpeg() {
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))));
    const QString expected = QString::number(qint64(kSrtLatencyMs) * 1000);
    QCOMPARE(q.queryItemValue(QStringLiteral("latency")), expected);
    QCOMPARE(q.queryItemValue(QStringLiteral("rcvlatency")), expected);
    QCOMPARE(q.queryItemValue(QStringLiteral("peerlatency")), expected);
}

void TestSrtOptions::connectTimeoutIsMilliseconds() {
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))));
    QCOMPARE(q.queryItemValue(QStringLiteral("connect_timeout")),
             QString::number(kSrtConnectTimeoutMs));
}

void TestSrtOptions::nonSrtUrlUntouched() {
    const QUrl in(QStringLiteral("udp://127.0.0.1:1234"));
    QCOMPARE(augmentSrtUrl(in), in);
    const QUrl rtmp(QStringLiteral("rtmp://h/live/a"));
    QCOMPARE(augmentSrtUrl(rtmp), rtmp);
}

void TestSrtOptions::existingOptionPreserved() {
    // A user-supplied option (any of them) wins and is not duplicated.
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral(
        "srt://127.0.0.1:9000?latency=200000&connect_timeout=2000&transtype=file"))));
    QCOMPARE(q.queryItemValue(QStringLiteral("latency")), QStringLiteral("200000"));
    QCOMPARE(q.allQueryItemValues(QStringLiteral("latency")).size(), 1);
    QCOMPARE(q.queryItemValue(QStringLiteral("connect_timeout")), QStringLiteral("2000"));
    QCOMPARE(q.queryItemValue(QStringLiteral("transtype")), QStringLiteral("file"));
}

void TestSrtOptions::caseInsensitiveScheme() {
    // Both helpers lower-case the scheme, so an upper-case SRT:// is still SRT.
    QVERIFY(QUrlQuery(augmentSrtUrl(QUrl(QStringLiteral("SRT://127.0.0.1:9000"))))
                .hasQueryItem(QStringLiteral("latency")));
    QCOMPARE(jitterWindowMs(QStringLiteral("SRT"), 80, 200), 80);
}

void TestSrtOptions::jitterWindowSrtUsesFloor() {
    QCOMPARE(jitterWindowMs(QStringLiteral("srt"), 80, 200), 80);
}

void TestSrtOptions::jitterWindowNonSrtUsesDefault() {
    QCOMPARE(jitterWindowMs(QStringLiteral("udp"), 80, 200), 200);
    QCOMPARE(jitterWindowMs(QStringLiteral("rtmp"), 80, 200), 200);
    QCOMPARE(jitterWindowMs(QString(), 80, 200), 200);
}

QTEST_GUILESS_MAIN(TestSrtOptions)
#include "tst_srt_options.moc"
