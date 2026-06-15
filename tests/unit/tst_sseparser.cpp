#include <QtTest>
#include <QJsonObject>

#include "telemetry/sseparser.h"

class TestSseParser : public QObject {
    Q_OBJECT
private slots:
    void parsesSingleJsonDataEvent();
    void parsesChunkedEvent();
    void ignoresCommentsAndKeepsEventType();
    void rejectsMalformedJson();
};

void TestSseParser::parsesSingleJsonDataEvent() {
    SseParser parser;
    const QByteArray chunk =
        "event: telemetry\n"
        "data: {\"feedId\":\"cam-main\",\"status\":\"ok\",\"values\":{\"batteryPercent\":91}}\n"
        "\n";

    const QList<TelemetryEvent> events = parser.push(chunk);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QCOMPARE(events[0].eventType, QStringLiteral("telemetry"));
    QCOMPARE(events[0].payload.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(events[0].payload.value("values").toObject().value("batteryPercent").toInt(), 91);
}

void TestSseParser::parsesChunkedEvent() {
    SseParser parser;
    QVERIFY(parser.push("data: {\"feed").isEmpty());
    const QList<TelemetryEvent> events = parser.push("Id\":\"cam-main\",\"values\":{}}\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
}

void TestSseParser::ignoresCommentsAndKeepsEventType() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        ": heartbeat\n"
        "event: telemetry\n"
        "id: 42\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\n"
        "\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].eventType, QStringLiteral("telemetry"));
    QCOMPARE(events[0].lastEventId, QStringLiteral("42"));
}

void TestSseParser::rejectsMalformedJson() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push("data: { nope }\n\n");
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("JSON")));
}

QTEST_GUILESS_MAIN(TestSseParser)
#include "tst_sseparser.moc"
