#include <QtTest>
#include <QJsonObject>

#include "telemetry/sseparser.h"

class TestSseParser : public QObject {
    Q_OBJECT
private slots:
    void parsesSingleJsonDataEvent();
    void joinsMultipleDataLinesWithNewline();
    void parsesChunkedEvent();
    void ignoresCommentsAndKeepsEventType();
    void ignoresBlocksWithoutDataField();
    void rejectsMalformedJson();
    void keepsErrorWhenLaterEventInSamePushSucceeds();
    void clearsStaleErrorForNoDataAndIncompletePushes();
    void rejectsOversizedIncompleteBuffer();
    void rejectsOversizedCompleteEvent();
    void inheritsLastEventIdAcrossEvents();
    void blankIdResetsLastEventId();
    void resetClearsLastEventId();
    void rejectsEmptyDataField();
    void clearsLastErrorAfterSuccessfulEvent();
    void rejectsNonObjectJson();
    void parsesCrlfSeparators();
    void defaultsEventTypeToMessage();
    void rejectsMissingOrBlankFeedId();
    void parsesMultipleEventsInOrder();
    void parsesSeparatorSplitAcrossChunks();
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

void TestSseParser::joinsMultipleDataLinesWithNewline() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        "event: telemetry\n"
        "data: {\"feedId\":\"cam-main\",\n"
        "data: \"status\":\"ok\",\"values\":{\"batteryPercent\":91}}\n"
        "\n");

    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
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

void TestSseParser::ignoresBlocksWithoutDataField() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        ": heartbeat\n"
        "event: telemetry\n"
        "id: 42\n"
        "\n");

    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().isEmpty());
}

void TestSseParser::rejectsMalformedJson() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push("data: { nope }\n\n");
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("JSON")));
}

void TestSseParser::keepsErrorWhenLaterEventInSamePushSucceeds() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        "data: { nope }\n"
        "\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\n"
        "\n");

    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QVERIFY(parser.lastError().contains(QStringLiteral("JSON")));
}

void TestSseParser::clearsStaleErrorForNoDataAndIncompletePushes() {
    SseParser parser;
    QVERIFY(parser.push("data: { nope }\n\n").isEmpty());
    QVERIFY(!parser.lastError().isEmpty());

    QVERIFY(parser.push(": heartbeat\n\n").isEmpty());
    QVERIFY(parser.lastError().isEmpty());

    QVERIFY(parser.push("data: { nope }\n\n").isEmpty());
    QVERIFY(!parser.lastError().isEmpty());

    QVERIFY(parser.push("data: {\"feedId\":\"cam-main\"").isEmpty());
    QVERIFY(parser.lastError().isEmpty());
}

void TestSseParser::rejectsOversizedIncompleteBuffer() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(QByteArray(1024 * 1024 + 1, 'x'));
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("SSE buffer exceeded")));
}

void TestSseParser::rejectsOversizedCompleteEvent() {
    SseParser parser;
    QByteArray chunk("data: ");
    chunk.append(QByteArray(1024 * 1024 + 1, 'x'));
    chunk.append("\n\n");

    const QList<TelemetryEvent> events = parser.push(chunk);
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("SSE buffer exceeded")));
}

void TestSseParser::inheritsLastEventIdAcrossEvents() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        "id: 42\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\n"
        "\n"
        "data: {\"feedId\":\"cam-side\",\"values\":{}}\n"
        "\n");

    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QCOMPARE(events[0].lastEventId, QStringLiteral("42"));
    QCOMPARE(events[1].feedId, QStringLiteral("cam-side"));
    QCOMPARE(events[1].lastEventId, QStringLiteral("42"));
}

void TestSseParser::blankIdResetsLastEventId() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        "id: 42\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\n"
        "\n"
        "id:\n"
        "\n"
        "data: {\"feedId\":\"cam-side\",\"values\":{}}\n"
        "\n");

    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].lastEventId, QStringLiteral("42"));
    QVERIFY(events[1].lastEventId.isEmpty());
}

void TestSseParser::resetClearsLastEventId() {
    SseParser parser;
    QCOMPARE(parser.push(
        "id: 42\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\n"
        "\n").size(), 1);
    QVERIFY(parser.push("data: { nope }\n\n").isEmpty());
    QVERIFY(!parser.lastError().isEmpty());
    QVERIFY(parser.push("data: {\"feed").isEmpty());

    parser.reset();
    QVERIFY(parser.lastError().isEmpty());

    const QList<TelemetryEvent> orphanedTailEvents = parser.push("Id\":\"cam-main\",\"values\":{}}\n\n");
    QVERIFY(orphanedTailEvents.isEmpty());
    QVERIFY(parser.lastError().isEmpty());

    const QList<TelemetryEvent> events = parser.push(
        "data: {\"feedId\":\"cam-side\",\"values\":{}}\n"
        "\n");
    QCOMPARE(events.size(), 1);
    QVERIFY(events[0].lastEventId.isEmpty());
}

void TestSseParser::rejectsEmptyDataField() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push("data:\n\n");
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("empty data")));
}

void TestSseParser::clearsLastErrorAfterSuccessfulEvent() {
    SseParser parser;
    QVERIFY(parser.push("data: { nope }\n\n").isEmpty());
    QVERIFY(!parser.lastError().isEmpty());

    const QList<TelemetryEvent> events = parser.push("data: {\"feedId\":\"cam-main\",\"values\":{}}\n\n");
    QCOMPARE(events.size(), 1);
    QVERIFY(parser.lastError().isEmpty());
}

void TestSseParser::rejectsNonObjectJson() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push("data: []\n\n");
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("JSON object")));
}

void TestSseParser::parsesCrlfSeparators() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        "event: telemetry\r\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\r\n"
        "\r\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QCOMPARE(events[0].eventType, QStringLiteral("telemetry"));
}

void TestSseParser::defaultsEventTypeToMessage() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push("data: {\"feedId\":\"cam-main\",\"values\":{}}\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].eventType, QStringLiteral("message"));
}

void TestSseParser::rejectsMissingOrBlankFeedId() {
    SseParser parser;
    QVERIFY(parser.push("data: {\"values\":{}}\n\n").isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("feedId")));

    const QList<TelemetryEvent> events = parser.push("data: {\"feedId\":\"   \",\"values\":{}}\n\n");
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("feedId")));
}

void TestSseParser::parsesMultipleEventsInOrder() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        "data: {\"feedId\":\"cam-main\",\"values\":{\"index\":1}}\n"
        "\n"
        "data: {\"feedId\":\"cam-side\",\"values\":{\"index\":2}}\n"
        "\n");

    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QCOMPARE(events[0].payload.value("values").toObject().value("index").toInt(), 1);
    QCOMPARE(events[1].feedId, QStringLiteral("cam-side"));
    QCOMPARE(events[1].payload.value("values").toObject().value("index").toInt(), 2);
}

void TestSseParser::parsesSeparatorSplitAcrossChunks() {
    SseParser parser;
    QVERIFY(parser.push("data: {\"feedId\":\"cam-main\",\"values\":{}}\n").isEmpty());

    const QList<TelemetryEvent> events = parser.push("\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
}

QTEST_GUILESS_MAIN(TestSseParser)
#include "tst_sseparser.moc"
