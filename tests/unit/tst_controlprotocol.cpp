#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "websocket/controlprotocol.h"

class TestControlProtocol : public QObject {
    Q_OBJECT
private slots:
    void parsesCommandWithIdAndArgs();
    void rejectsMalformedJson();
    void rejectsMissingCommandName();
    void validatesSeekArgs();
    void validatesSeekArgsAcceptsLargeInteger();
    void rejectsSeekWithFractionalPosition();
    void rejectsSeekWithoutPosition();
    void validatesActionDispatchDefaultsPressed();
    void rejectsActionDispatchForShuttleId();
    void validatesActionShuttleDelta();
    void buildsSuccessAck();
    void buildsFailureAck();
    void buildsErrorWithoutId();
};

void TestControlProtocol::parsesCommandWithIdAndArgs() {
    const QByteArray raw =
        "{\"type\":\"command\",\"id\":\"abc-1\",\"name\":\"transport.seek\","
        "\"args\":{\"positionMs\":1234}}";

    const ControlProtocol::ParseResult result = ControlProtocol::parseTextMessage(raw);

    QVERIFY(result.ok);
    QCOMPARE(result.message.type, QStringLiteral("command"));
    QCOMPARE(result.message.id, QStringLiteral("abc-1"));
    QCOMPARE(result.message.name, QStringLiteral("transport.seek"));
    QCOMPARE(result.message.args.value(QStringLiteral("positionMs")).toInt(), 1234);
}

void TestControlProtocol::rejectsMalformedJson() {
    const ControlProtocol::ParseResult result = ControlProtocol::parseTextMessage("{ nope");

    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("bad_json"));
    QVERIFY(result.messageText.contains(QStringLiteral("JSON")));
}

void TestControlProtocol::rejectsMissingCommandName() {
    const ControlProtocol::ParseResult result =
        ControlProtocol::parseTextMessage("{\"type\":\"command\",\"id\":\"abc\"}");

    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("bad_message"));
    QCOMPARE(result.id, QStringLiteral("abc"));
}

void TestControlProtocol::validatesSeekArgs() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("seek-1"),
        QStringLiteral("transport.seek"),
        QJsonObject{{QStringLiteral("positionMs"), 42}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(validation.ok);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("positionMs")).toInt(), 42);
}

void TestControlProtocol::validatesSeekArgsAcceptsLargeInteger() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("seek-large"),
        QStringLiteral("transport.seek"),
        QJsonObject{{QStringLiteral("positionMs"), 9007199254740991LL}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(validation.ok);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("positionMs")).toDouble(),
             9007199254740991.0);
}

void TestControlProtocol::rejectsSeekWithFractionalPosition() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("seek-fraction"),
        QStringLiteral("transport.seek"),
        QJsonObject{{QStringLiteral("positionMs"), 42.5}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(!validation.ok);
    QCOMPARE(validation.code, QStringLiteral("invalid_args"));
    QVERIFY(validation.message.contains(QStringLiteral("positionMs")));
}

void TestControlProtocol::rejectsSeekWithoutPosition() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("seek-2"),
        QStringLiteral("transport.seek"),
        QJsonObject{}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(!validation.ok);
    QCOMPARE(validation.code, QStringLiteral("invalid_args"));
    QVERIFY(validation.message.contains(QStringLiteral("positionMs")));
}

void TestControlProtocol::validatesActionDispatchDefaultsPressed() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("action-1"),
        QStringLiteral("action.dispatch"),
        QJsonObject{{QStringLiteral("actionId"), 0}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(validation.ok);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("actionId")).toInt(), 0);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("pressed")).toBool(), true);
}

void TestControlProtocol::rejectsActionDispatchForShuttleId() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("action-shuttle-wrong"),
        QStringLiteral("action.dispatch"),
        QJsonObject{{QStringLiteral("actionId"), 10}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(!validation.ok);
    QCOMPARE(validation.code, QStringLiteral("invalid_args"));
    QVERIFY(validation.message.contains(QStringLiteral("action.shuttle")));
}

void TestControlProtocol::validatesActionShuttleDelta() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("shuttle-1"),
        QStringLiteral("action.shuttle"),
        QJsonObject{{QStringLiteral("delta"), -1}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(validation.ok);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("delta")).toInt(), -1);
}

void TestControlProtocol::buildsSuccessAck() {
    const QJsonObject ack = ControlProtocol::ack(QStringLiteral("abc-1"));

    QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(ack.value(QStringLiteral("id")).toString(), QStringLiteral("abc-1"));
    QCOMPARE(ack.value(QStringLiteral("ok")).toBool(), true);
}

void TestControlProtocol::buildsFailureAck() {
    const QJsonObject ack = ControlProtocol::ackError(
        QStringLiteral("abc-2"), QStringLiteral("invalid_args"),
        QStringLiteral("positionMs must be an integer"));

    QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(ack.value(QStringLiteral("id")).toString(), QStringLiteral("abc-2"));
    QCOMPARE(ack.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(ack.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_args"));
}

void TestControlProtocol::buildsErrorWithoutId() {
    const QJsonObject error = ControlProtocol::error(
        QStringLiteral("bad_json"), QStringLiteral("Message must be a JSON object"));

    QCOMPARE(error.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(error.value(QStringLiteral("code")).toString(), QStringLiteral("bad_json"));
    QCOMPARE(error.value(QStringLiteral("message")).toString(),
             QStringLiteral("Message must be a JSON object"));
}

QTEST_GUILESS_MAIN(TestControlProtocol)
#include "tst_controlprotocol.moc"
