#include <QtTest>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>

#include "playback/telemetrytimelinereader.h"
#include "recorder_engine/replaymanager.h"

class TestReplayManagerTelemetry : public QObject {
    Q_OBJECT

private slots:
    void recordTelemetryEventWritesDelayedPayload();
    void recordTelemetryEventRejectsInvalidStates();
    void recordTelemetryEventClampsDelay();

private:
    static void configureMinimalRecording(ReplayManager &manager, const QString &outputDir);
};

void TestReplayManagerTelemetry::configureMinimalRecording(ReplayManager &manager,
                                                           const QString &outputDir) {
    manager.setOutputDirectory(outputDir);
    manager.setBaseFileName(QStringLiteral("replaymanager_telemetry"));
    manager.setSourceUrls({QString()});
    manager.setViewCount(1);
    manager.setViewNames({QStringLiteral("Program")});
    manager.updateViewMapping({-1});
    manager.setVideoWidth(320);
    manager.setVideoHeight(240);
    manager.setFps(30);
}

void TestReplayManagerTelemetry::recordTelemetryEventWritesDelayedPayload() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    configureMinimalRecording(manager, dir.path());
    manager.setTelemetryFeeds({QStringLiteral("cam-main")},
                              {QStringLiteral("Main Camera")},
                              {800});

    manager.startRecording();
    QVERIFY(manager.isRecording());

    QVERIFY(manager.recordTelemetryEvent(
        QStringLiteral("cam-main"),
        QJsonObject{
            {QStringLiteral("feedId"), QStringLiteral("payload-spoof")},
            {QStringLiteral("speedKmh"), 88},
            {QStringLiteral("nested"), QJsonObject{{QStringLiteral("ok"), true}}},
        }));

    const QString recordingPath = manager.getVideoPath();
    manager.stopRecording();
    QVERIFY2(QFileInfo::exists(recordingPath), qPrintable(recordingPath));

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(recordingPath), qPrintable(reader.lastError()));
    QCOMPARE(reader.feedIds(), QStringList{QStringLiteral("cam-main")});

    const QVariantMap afterDelay = reader.stateAt(20000);
    QCOMPARE(afterDelay.size(), 1);
    const QVariantMap event = afterDelay.value(QStringLiteral("cam-main")).toMap();
    QCOMPARE(event.value(QStringLiteral("feedId")).toString(), QStringLiteral("cam-main"));
    QCOMPARE(event.value(QStringLiteral("speedKmh")).toInt(), 88);
    QCOMPARE(event.value(QStringLiteral("nested")).toMap().value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(event.value(QStringLiteral("olrTelemetryDelayMs")).toInt(), 800);

    const qint64 receiveMs = event.value(QStringLiteral("olrReceiveMs")).toLongLong();
    const qint64 effectiveMs = event.value(QStringLiteral("olrEffectiveMs")).toLongLong();
    QVERIFY(receiveMs >= 0);
    QCOMPARE(effectiveMs, receiveMs + 800);
    QVERIFY(reader.stateAt(effectiveMs - 1).isEmpty());
    QVERIFY(reader.stateAt(effectiveMs).contains(QStringLiteral("cam-main")));
}

void TestReplayManagerTelemetry::recordTelemetryEventRejectsInvalidStates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    configureMinimalRecording(manager, dir.path());
    manager.setTelemetryFeeds({QStringLiteral("cam-main")},
                              {QStringLiteral("Main Camera")},
                              {800});

    QVERIFY(!manager.recordTelemetryEvent(QStringLiteral("cam-main"), QJsonObject{}));

    manager.startRecording();
    QVERIFY(manager.isRecording());
    QVERIFY(!manager.recordTelemetryEvent(QStringLiteral("missing-feed"), QJsonObject{}));
    manager.stopRecording();
}

void TestReplayManagerTelemetry::recordTelemetryEventClampsDelay() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    configureMinimalRecording(manager, dir.path());
    manager.setTelemetryFeeds({QStringLiteral("cam-main")},
                              {QStringLiteral("Main Camera")},
                              {20000});

    manager.startRecording();
    QVERIFY(manager.isRecording());
    QVERIFY(manager.recordTelemetryEvent(QStringLiteral("cam-main"),
                                         QJsonObject{{QStringLiteral("batteryPercent"), 91}}));

    const QString recordingPath = manager.getVideoPath();
    manager.stopRecording();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(recordingPath), qPrintable(reader.lastError()));
    const QVariantMap event = reader.stateAt(30000).value(QStringLiteral("cam-main")).toMap();
    QCOMPARE(event.value(QStringLiteral("olrTelemetryDelayMs")).toInt(), 10000);
    QCOMPARE(event.value(QStringLiteral("olrEffectiveMs")).toLongLong(),
             event.value(QStringLiteral("olrReceiveMs")).toLongLong() + 10000);
}

QTEST_GUILESS_MAIN(TestReplayManagerTelemetry)
#include "tst_replaymanager_telemetry.moc"
