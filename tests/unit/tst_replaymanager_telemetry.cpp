#include <QtTest>
#include <QFileInfo>
#include <QFuture>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>

#include "playback/telemetrytimelinereader.h"
#include "recorder_engine/replaymanager.h"

extern "C" {
#include <libavcodec/packet.h>
}

class TestReplayManagerTelemetry : public QObject {
    Q_OBJECT

private slots:
    void recordTelemetryEventWritesDelayedPayload();
    void recordTelemetryEventRejectsInvalidStates();
    void recordTelemetryEventClampsDelay();
    void telemetryFeedConfigIgnoresEmptyAndKeepsFirstDuplicate();
    void setTelemetryFeedsDuringRecordingDoesNotRemapActiveLayout();
    void recordTelemetryEventWorksFromWorkerThread();
    void committedVideoTailDefaultsToUnknown();
    void stopRecordingWakesBackpressuredTelemetryWriter();
    void stopRecordingDrainsMuxerBeforeDeletingWorkers();

private:
    static void configureMinimalRecording(ReplayManager& manager, const QString& outputDir);
};

void TestReplayManagerTelemetry::configureMinimalRecording(ReplayManager& manager,
                                                           const QString& outputDir) {
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
    manager.setTelemetryFeeds({QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")}, {800});

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
    QCOMPARE(event.value(QStringLiteral("nested")).toMap().value(QStringLiteral("ok")).toBool(),
             true);
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
    manager.setTelemetryFeeds({QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")}, {800});

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
    manager.setTelemetryFeeds({QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")},
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

void TestReplayManagerTelemetry::telemetryFeedConfigIgnoresEmptyAndKeepsFirstDuplicate() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    configureMinimalRecording(manager, dir.path());
    manager.setTelemetryFeeds({QString(), QStringLiteral("cam-main"), QStringLiteral("cam-main"),
                               QStringLiteral("cam-side")},
                              {QStringLiteral("Empty"), QStringLiteral("Main First"),
                               QStringLiteral("Main Duplicate"), QStringLiteral("Side")},
                              {50, 200, 900, 300});

    manager.startRecording();
    QVERIFY(manager.isRecording());
    QVERIFY(!manager.recordTelemetryEvent(QString(), QJsonObject{}));
    QVERIFY(manager.recordTelemetryEvent(QStringLiteral("cam-main"),
                                         QJsonObject{{QStringLiteral("sequence"), 1}}));
    QVERIFY(manager.recordTelemetryEvent(QStringLiteral("cam-side"),
                                         QJsonObject{{QStringLiteral("sequence"), 2}}));

    const QString recordingPath = manager.getVideoPath();
    manager.stopRecording();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(recordingPath), qPrintable(reader.lastError()));
    QCOMPARE(reader.feedIds(),
             QStringList({QStringLiteral("cam-main"), QStringLiteral("cam-side")}));

    const QVariantMap main = reader.stateAt(20000).value(QStringLiteral("cam-main")).toMap();
    QCOMPARE(main.value(QStringLiteral("sequence")).toInt(), 1);
    QCOMPARE(main.value(QStringLiteral("olrTelemetryDelayMs")).toInt(), 200);
    QCOMPARE(main.value(QStringLiteral("olrEffectiveMs")).toLongLong(),
             main.value(QStringLiteral("olrReceiveMs")).toLongLong() + 200);
}

void TestReplayManagerTelemetry::setTelemetryFeedsDuringRecordingDoesNotRemapActiveLayout() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    configureMinimalRecording(manager, dir.path());
    manager.setTelemetryFeeds({QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")}, {0});

    manager.startRecording();
    QVERIFY(manager.isRecording());
    manager.setTelemetryFeeds({QStringLiteral("cam-side")}, {QStringLiteral("Side Camera")}, {0});

    QVERIFY(!manager.recordTelemetryEvent(QStringLiteral("cam-side"),
                                          QJsonObject{{QStringLiteral("ignored"), true}}));
    QVERIFY(manager.recordTelemetryEvent(QStringLiteral("cam-main"),
                                         QJsonObject{{QStringLiteral("active"), true}}));

    const QString recordingPath = manager.getVideoPath();
    manager.stopRecording();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(recordingPath), qPrintable(reader.lastError()));
    QCOMPARE(reader.feedIds(), QStringList{QStringLiteral("cam-main")});
    const QVariantMap state = reader.stateAt(20000);
    QVERIFY(state.contains(QStringLiteral("cam-main")));
    QVERIFY(!state.contains(QStringLiteral("cam-side")));
    QCOMPARE(
        state.value(QStringLiteral("cam-main")).toMap().value(QStringLiteral("active")).toBool(),
        true);
}

void TestReplayManagerTelemetry::recordTelemetryEventWorksFromWorkerThread() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    configureMinimalRecording(manager, dir.path());
    manager.setTelemetryFeeds({QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")}, {100});

    manager.startRecording();
    QVERIFY(manager.isRecording());

    QFuture<bool> result = QtConcurrent::run([&manager] {
        return manager.recordTelemetryEvent(QStringLiteral("cam-main"),
                                            QJsonObject{{QStringLiteral("threaded"), true}});
    });
    result.waitForFinished();
    QVERIFY(result.result());

    const QString recordingPath = manager.getVideoPath();
    manager.stopRecording();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(recordingPath), qPrintable(reader.lastError()));
    const QVariantMap event = reader.stateAt(20000).value(QStringLiteral("cam-main")).toMap();
    QCOMPARE(event.value(QStringLiteral("threaded")).toBool(), true);
    QCOMPARE(event.value(QStringLiteral("olrTelemetryDelayMs")).toInt(), 100);
}

void TestReplayManagerTelemetry::committedVideoTailDefaultsToUnknown() {
    ReplayManager manager;
    QCOMPARE(manager.committedVideoTailMs(), qint64(-1));
}

void TestReplayManagerTelemetry::stopRecordingWakesBackpressuredTelemetryWriter() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "60000");
    auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    manager.setTelemetryFeeds({QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")}, {0});
    manager.m_muxer->setOutputDirectory(dir.path());
    QVERIFY(manager.m_muxer->init(QStringLiteral("replaymanager_telemetry_backpressure"), 1, 320,
                                  240, 30, {QStringLiteral("Program")},
                                  {QStringLiteral("cam-main")}, {QStringLiteral("Main Camera")},
                                  48000, 2));
    manager.m_isRecording = true;

    auto makePacket = [&manager]() {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return pkt;
        if (av_new_packet(pkt, 1) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->stream_index = manager.m_muxer->subtitleTrackOffset();
        pkt->pts = 0;
        pkt->dts = 0;
        pkt->duration = 1;
        return pkt;
    };

    for (int i = 0; i < 4096; ++i) {
        AVPacket* pkt = makePacket();
        QVERIFY(pkt != nullptr);
        manager.m_muxer->writePacket(pkt);
        av_packet_free(&pkt);
    }

    std::atomic<bool> telemetryStarted{false};
    QFuture<bool> telemetry = QtConcurrent::run([&] {
        telemetryStarted.store(true, std::memory_order_release);
        return manager.recordTelemetryEvent(QStringLiteral("cam-main"),
                                            QJsonObject{{QStringLiteral("blocked"), true}});
    });
    QTRY_VERIFY_WITH_TIMEOUT(telemetryStarted.load(std::memory_order_acquire), 1000);
    QTest::qWait(50);
    QVERIFY(!telemetry.isFinished());

    QFuture<void> stop = QtConcurrent::run([&] { manager.stopRecording(); });

    QElapsedTimer timer;
    timer.start();
    while (!stop.isFinished() && timer.elapsed() < 1000) {
        QTest::qWait(20);
    }
    const bool stopFinishedWithoutManualDrain = stop.isFinished();
    if (!stopFinishedWithoutManualDrain) {
        manager.m_muxer->beginShutdownDrain();
    }
    stop.waitForFinished();
    telemetry.waitForFinished();

    QVERIFY2(stopFinishedWithoutManualDrain,
             "stopRecording must wake a telemetry writer blocked on muxer backpressure");
    QVERIFY(!telemetry.result());
}

void TestReplayManagerTelemetry::stopRecordingDrainsMuxerBeforeDeletingWorkers() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager manager;
    manager.m_muxer->setOutputDirectory(dir.path());
    QVERIFY(manager.m_muxer->init(QStringLiteral("replaymanager_stop_order"), 1, 320, 240, 30,
                                  {QStringLiteral("Program")}, 48000, 2, QString()));

    auto* worker = new StreamWorker(QString(), 0, manager.m_muxer, nullptr, 320, 240, 30, 30, 1,
                                    VideoCodecChoice::H264Hardware);
    manager.m_workers.append(worker);
    manager.m_isRecording = true;

    std::atomic<bool> workerDestroyed{false};
    std::atomic<bool> completionRan{false};
    std::atomic<bool> completionSawWorkerAlive{false};
    QObject::connect(worker, &QObject::destroyed,
                     [&] { workerDestroyed.store(true, std::memory_order_release); });

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    QVERIFY(av_new_packet(pkt, 2) == 0);
    pkt->data[0] = '{';
    pkt->data[1] = '}';
    pkt->stream_index = manager.m_muxer->subtitleTrackOffset();
    pkt->pts = 0;
    pkt->dts = 0;
    pkt->duration = 1;

    manager.m_muxer->writePacket(pkt, [&](bool) {
        completionSawWorkerAlive.store(!workerDestroyed.load(std::memory_order_acquire),
                                       std::memory_order_release);
        completionRan.store(true, std::memory_order_release);
    });
    av_packet_free(&pkt);

    manager.stopRecording();

    QVERIFY(completionRan.load(std::memory_order_acquire));
    QVERIFY(completionSawWorkerAlive.load(std::memory_order_acquire));
    QVERIFY(workerDestroyed.load(std::memory_order_acquire));
}

QTEST_GUILESS_MAIN(TestReplayManagerTelemetry)
#include "tst_replaymanager_telemetry.moc"
