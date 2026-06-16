#ifndef CONTROLSTATE_H
#define CONTROLSTATE_H

#include <QJsonObject>
#include <QString>

class ControlApiAdapter;

class ControlState {
public:
    static QJsonObject snapshotMessage(const ControlApiAdapter &adapter);
    static QJsonObject patchMessage(const QString &path, const QJsonObject &value);
    static QJsonObject timecodeMessage(const ControlApiAdapter &adapter);

    static QJsonObject recordingObject(const ControlApiAdapter &adapter);
    static QJsonObject transportObject(const ControlApiAdapter &adapter);
    static QJsonObject settingsObject(const ControlApiAdapter &adapter);
    static QJsonObject telemetryObject(const ControlApiAdapter &adapter);
};

#endif
