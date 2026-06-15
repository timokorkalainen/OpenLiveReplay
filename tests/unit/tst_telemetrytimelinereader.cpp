#include <QtTest>
#include <QTemporaryDir>

#include "playback/telemetrytimelinereader.h"
#include "recorder_engine/muxer.h"

class TestTelemetryTimelineReader : public QObject {
    Q_OBJECT

private slots:
    void readsLatestFeedTelemetryByPlayhead();
};

void TestTelemetryTimelineReader::readsLatestFeedTelemetryByPlayhead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(dir.path());
    const QStringList viewNames{QStringLiteral("View 1")};
    const QStringList feedIds{QStringLiteral("cam-main"), QStringLiteral("cam-reverse")};
    const QStringList feedNames{QStringLiteral("Main"), QStringLiteral("Reverse")};
    QVERIFY(muxer.init(QStringLiteral("telemetry_reader"),
                       1,
                       320,
                       240,
                       30,
                       viewNames,
                       feedIds,
                       feedNames,
                       48000,
                       2));

    muxer.writeTelemetryPacket(0, 100, QByteArrayLiteral(
        "{\"feedId\":\"cam-main\",\"values\":{\"batteryPercent\":90}}"));
    muxer.writeTelemetryPacket(0, 150, QByteArrayLiteral("{malformed"));
    muxer.writeTelemetryPacket(1, 200, QByteArrayLiteral(
        "{\"feedId\":\"cam-reverse\",\"values\":{\"signalDb\":-61}}"));
    muxer.writeTelemetryPacket(0, 300, QByteArrayLiteral(
        "{\"feedId\":\"cam-main\",\"values\":{\"batteryPercent\":88}}"));
    muxer.close();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(dir.filePath(QStringLiteral("telemetry_reader.mkv"))),
             qPrintable(reader.lastError()));
    QCOMPARE(reader.feedIds(), feedIds);
    QVERIFY(reader.lastError().isEmpty());

    const QVariantMap at50 = reader.stateAt(50);
    QVERIFY(at50.isEmpty());

    const QVariantMap at150 = reader.stateAt(150);
    QCOMPARE(at150.size(), 1);
    QCOMPARE(at150.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             90);
    QVERIFY(!at150.contains(QStringLiteral("cam-reverse")));

    const QVariantMap at250 = reader.stateAt(250);
    QCOMPARE(at250.size(), 2);
    QCOMPARE(at250.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             90);
    QCOMPARE(at250.value(QStringLiteral("cam-reverse"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("signalDb"))
                 .toInt(),
             -61);

    const QVariantMap at350 = reader.stateAt(350);
    QCOMPARE(at350.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             88);
    QCOMPARE(at350.value(QStringLiteral("cam-reverse"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("signalDb"))
                 .toInt(),
             -61);
}

QTEST_GUILESS_MAIN(TestTelemetryTimelineReader)
#include "tst_telemetrytimelinereader.moc"
