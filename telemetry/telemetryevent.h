#ifndef TELEMETRYEVENT_H
#define TELEMETRYEVENT_H

#include <QJsonObject>
#include <QString>

struct TelemetryEvent {
    QString feedId;
    QString eventType;
    QString lastEventId;
    QJsonObject payload;
};

#endif // TELEMETRYEVENT_H
