#include "controlprotocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

ControlProtocol::ParseResult ControlProtocol::parseTextMessage(const QByteArray &payload) {
    ParseResult result;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.code = QStringLiteral("bad_json");
        result.messageText = QStringLiteral("Invalid JSON: %1").arg(parseError.errorString());
        return result;
    }
    if (!doc.isObject()) {
        result.code = QStringLiteral("bad_message");
        result.messageText = QStringLiteral("Message must be a JSON object");
        return result;
    }

    const QJsonObject obj = doc.object();
    result.id = obj.value(QStringLiteral("id")).toString();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("command")) {
        result.code = QStringLiteral("unsupported_message");
        result.messageText = QStringLiteral("Unsupported message type");
        return result;
    }

    const QString name = obj.value(QStringLiteral("name")).toString();
    if (name.trimmed().isEmpty()) {
        result.code = QStringLiteral("bad_message");
        result.messageText = QStringLiteral("Command message requires name");
        return result;
    }

    result.ok = true;
    result.message.type = type;
    result.message.id = result.id;
    result.message.name = name;
    const QJsonValue args = obj.value(QStringLiteral("args"));
    result.message.args = args.isObject() ? args.toObject() : QJsonObject{};
    return result;
}

QJsonObject ControlProtocol::ack(const QString &id) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("ack"));
    if (!id.isEmpty()) obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), true);
    return obj;
}

QJsonObject ControlProtocol::ackError(const QString &id, const QString &code, const QString &message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("ack"));
    if (!id.isEmpty()) obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), false);
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    return obj;
}

QJsonObject ControlProtocol::error(const QString &code, const QString &message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("error"));
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    return obj;
}

QByteArray ControlProtocol::compact(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
