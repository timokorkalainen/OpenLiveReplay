#include <QtTest>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QWebSocket>

#include "websocket/controlstate.h"
#include "websocket/controlwebsocketserver.h"

class ServerFakeAdapter final : public QObject, public ControlApiAdapter {
public:
    QString lastCommand;
    QJsonObject lastArgs;

    RecordingState recordingState() const override {
        return {};
    }

    TransportState transportState() const override {
        return {0, 0, 0, QStringLiteral("00:00:00:00"), false, 1.0, 30, false, 1000};
    }

    QVector<SourceState> sourceStates() const override {
        return {};
    }

    ViewState viewState() const override {
        return {};
    }

    SettingsState settingsState() const override {
        return {};
    }

    MidiState midiState() const override {
        return {};
    }

    StreamDeckState streamDeckState() const override {
        return {};
    }

    ScreensState screensState() const override {
        return {};
    }

    ImportState importState() const override {
        return {};
    }

    TelemetryState telemetryState() const override {
        return {};
    }

    CommandResult executeCommand(const QString &name, const QJsonObject &args) override {
        lastCommand = name;
        lastArgs = args;
        return CommandResult::success();
    }
};

class TestControlWebSocketServer : public QObject {
    Q_OBJECT
private slots:
    void sendsSnapshotAndTimecodeOnConnect();
    void dispatchesCommandAndSendsAck();
    void sendsErrorForBadJson();
};

void TestControlWebSocketServer::sendsSnapshotAndTimecodeOnConnect() {
    ServerFakeAdapter adapter;
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket socket;
    QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
    socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/api/ws").arg(server.serverPort())));

    QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 2, 2000);
    QCOMPARE(messages.count(), 2);

    const QJsonObject first = QJsonDocument::fromJson(messages.at(0).at(0).toString().toUtf8()).object();
    const QJsonObject second = QJsonDocument::fromJson(messages.at(1).at(0).toString().toUtf8()).object();

    QCOMPARE(first.value(QStringLiteral("type")).toString(), QStringLiteral("state.snapshot"));
    QCOMPARE(second.value(QStringLiteral("type")).toString(), QStringLiteral("timecode"));
    QCOMPARE(second.value(QStringLiteral("text")).toString(), QStringLiteral("00:00:00:00"));
}

void TestControlWebSocketServer::dispatchesCommandAndSendsAck() {
    ServerFakeAdapter adapter;
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket socket;
    QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
    socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/api/ws").arg(server.serverPort())));

    QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 2, 2000);

    socket.sendTextMessage(
        QStringLiteral("{\"type\":\"command\",\"id\":\"seek-1\",\"name\":\"transport.seek\",\"args\":{\"positionMs\":321}}"));

    QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 3, 2000);
    const QJsonObject ack = QJsonDocument::fromJson(messages.at(2).at(0).toString().toUtf8()).object();
    QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(ack.value(QStringLiteral("id")).toString(), QStringLiteral("seek-1"));
    QCOMPARE(ack.value(QStringLiteral("ok")).toBool(), true);

    QCOMPARE(adapter.lastCommand, QStringLiteral("transport.seek"));
    QCOMPARE(adapter.lastArgs.value(QStringLiteral("positionMs")).toInt(), 321);
}

void TestControlWebSocketServer::sendsErrorForBadJson() {
    ServerFakeAdapter adapter;
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket socket;
    QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
    socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/api/ws").arg(server.serverPort())));

    QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 2, 2000);

    socket.sendTextMessage(QStringLiteral("{ nope"));

    QTRY_COMPARE_WITH_TIMEOUT(messages.count(), 3, 2000);
    const QJsonObject err = QJsonDocument::fromJson(messages.at(2).at(0).toString().toUtf8()).object();
    QCOMPARE(err.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(err.value(QStringLiteral("code")).toString(), QStringLiteral("bad_json"));
}

QTEST_GUILESS_MAIN(TestControlWebSocketServer)
#include "tst_controlwebsocketserver.moc"
