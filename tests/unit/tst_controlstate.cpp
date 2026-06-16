#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "websocket/controlapiadapter.h"
#include "websocket/controlstate.h"

class FakeControlAdapter final : public ControlApiAdapter {
public:
    RecordingState recordingState() const override { return {true, 1500, 1700000000123}; }
    TransportState transportState() const override { return {1200, 1200, 1500, QStringLiteral("00:00:01:06"), true, 1.0, 30, true, 1000}; }
    QVector<SourceState> sourceStates() const override {
        SourceState s;
        s.index = 0;
        s.id = QStringLiteral("cam-a");
        s.name = QStringLiteral("Camera A");
        s.url = QStringLiteral("srt://127.0.0.1:9000");
        s.enabled = true;
        s.connected = true;
        s.duplicateUrl = false;
        s.trimOffsetMs = 33;
        s.metadata.append(QVariantMap{{QStringLiteral("name"), QStringLiteral("driver")}, {QStringLiteral("value"), QStringLiteral("A. Smith")}});
        return {s};
    }
    ViewState viewState() const override { return {4, QVariantList{0, -1, 1, 2}, false, -1}; }
    SettingsState settingsState() const override { return {QStringLiteral("match"), QStringLiteral("/tmp"), 1920, 1080, 30, 40, true, QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("driver")}}}}; }
    MidiState midiState() const override { return {QStringList{QStringLiteral("X-Touch")}, 0, QStringLiteral("X-Touch"), true, -1, 0}; }
    StreamDeckState streamDeckState() const override { return {true, true, QStringLiteral("Deck"), QStringLiteral("Plus"), 8, 4, -1}; }
    ScreensState screensState() const override { return {true, 1, QVariantList{QVariantMap{{QStringLiteral("index"), 0}, {QStringLiteral("label"), QStringLiteral("Display")}}}}; }
    ImportState importState() const override { return {QStringLiteral("http://settings"), QStringLiteral("http://telemetry"), false, QString(), QVariantMap{}}; }
    TelemetryState telemetryState() const override { return {2, QVariantList{QVariantMap{{QStringLiteral("feedId"), QStringLiteral("cam-a")}}}, QVariantMap{{QStringLiteral("cam-a"), QVariantMap{{QStringLiteral("speed"), 88}}}}}; }
};

class TestControlState : public QObject {
    Q_OBJECT
private slots:
    void buildsSnapshotWithExpectedTopLevelObjects();
    void buildsPathPatch();
    void buildsTimecodeMessageFromTransport();
};

void TestControlState::buildsSnapshotWithExpectedTopLevelObjects() {
    FakeControlAdapter adapter;

    const QJsonObject msg = ControlState::snapshotMessage(adapter);
    const QJsonObject state = msg.value(QStringLiteral("state")).toObject();

    QCOMPARE(msg.value(QStringLiteral("type")).toString(), QStringLiteral("state.snapshot"));
    QCOMPARE(state.value(QStringLiteral("recording")).toObject().value(QStringLiteral("active")).toBool(), true);
    QCOMPARE(state.value(QStringLiteral("transport")).toObject().value(QStringLiteral("timecode")).toString(), QStringLiteral("00:00:01:06"));
    QCOMPARE(state.value(QStringLiteral("sources")).toArray().first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("cam-a"));
    QCOMPARE(state.value(QStringLiteral("settings")).toObject().value(QStringLiteral("audioOutputLatencyMs")).toInt(), 40);
    QCOMPARE(state.value(QStringLiteral("telemetry")).toObject().value(QStringLiteral("version")).toInt(), 2);
}

void TestControlState::buildsPathPatch() {
    const QJsonObject value{{QStringLiteral("playing"), true}};

    const QJsonObject msg = ControlState::patchMessage(QStringLiteral("transport"), value);

    QCOMPARE(msg.value(QStringLiteral("type")).toString(), QStringLiteral("state.patch"));
    QCOMPARE(msg.value(QStringLiteral("path")).toString(), QStringLiteral("transport"));
    QCOMPARE(msg.value(QStringLiteral("value")).toObject().value(QStringLiteral("playing")).toBool(), true);
}

void TestControlState::buildsTimecodeMessageFromTransport() {
    FakeControlAdapter adapter;

    const QJsonObject msg = ControlState::timecodeMessage(adapter);

    QCOMPARE(msg.value(QStringLiteral("type")).toString(), QStringLiteral("timecode"));
    QCOMPARE(msg.value(QStringLiteral("positionMs")).toInt(), 1200);
    QCOMPARE(msg.value(QStringLiteral("durationMs")).toInt(), 1500);
    QCOMPARE(msg.value(QStringLiteral("text")).toString(), QStringLiteral("00:00:01:06"));
    QCOMPARE(msg.value(QStringLiteral("followLive")).toBool(), true);
}

QTEST_GUILESS_MAIN(TestControlState)
#include "tst_controlstate.moc"
