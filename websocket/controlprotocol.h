#ifndef CONTROLPROTOCOL_H
#define CONTROLPROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

struct ControlCommandMessage {
    QString type;
    QString id;
    QString name;
    QJsonObject args;
};

class ControlProtocol {
public:
    struct ParseResult {
        bool ok = false;
        ControlCommandMessage message;
        QString id;
        QString code;
        QString messageText;
    };
    struct CommandValidation {
        bool ok = false;
        QString code;
        QString message;
        QJsonObject normalizedArgs;
    };

    static ParseResult parseTextMessage(const QByteArray &payload);
    static CommandValidation validateCommand(const ControlCommandMessage &command);
    static QJsonObject ack(const QString &id);
    static QJsonObject ackError(const QString &id, const QString &code, const QString &message);
    static QJsonObject error(const QString &code, const QString &message);
    static QByteArray compact(const QJsonObject &object);
};

#endif
