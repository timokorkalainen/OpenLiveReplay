#ifndef CONTROLAPIADAPTER_H
#define CONTROLAPIADAPTER_H

#include <QVariantList>
#include <QVariantMap>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

struct RecordingState {
    bool active = false;
    qint64 durationMs = 0;
    qint64 startEpochMs = 0;
};

struct TransportState {
    qint64 positionMs = 0;
    qint64 scrubPositionMs = 0;
    qint64 durationMs = 0;
    QString timecode;
    bool playing = false;
    double speed = 1.0;
    int fps = 30;
    bool followLive = false;
    int liveBufferMs = 1000;
};

struct SourceState {
    int index = -1;
    QString id;
    QString name;
    QString url;
    bool enabled = false;
    bool connected = false;
    bool duplicateUrl = false;
    int trimOffsetMs = 0;
    QVariantList metadata;
};

struct ViewState {
    int multiviewCount = 1;
    QVariantList slotMap;
    bool singleView = false;
    int selectedIndex = -1;
};

struct SettingsState {
    QString fileName;
    QString saveLocation;
    int recordWidth = 1920;
    int recordHeight = 1080;
    int recordFps = 30;
    int audioOutputLatencyMs = 0;
    bool timeOfDayMode = false;
    QVariantList metadataFields;
};

struct MidiState {
    QStringList ports;
    int portIndex = -1;
    QString portName;
    bool connected = false;
    int learnAction = -1;
    int learnMode = 0;
};

struct StreamDeckState {
    bool supported = false;
    bool connected = false;
    QString deviceName;
    QString deviceModel;
    int keyCount = 0;
    int dialCount = 0;
    int learnAction = -1;
};

struct ScreensState {
    bool ready = false;
    int count = 0;
    QVariantList options;
};

struct ImportState {
    QString settingsUrl;
    QString telemetrySseUrl;
    bool previewReady = false;
    QString previewError;
    QVariantMap preview;
};

struct TelemetryState {
    int version = 0;
    QVariantList rows;
    QVariantMap state;
};

struct CommandResult {
    bool ok = true;
    QString code;
    QString message;

    static CommandResult success() { return {}; }
    static CommandResult failure(const QString &failureCode, const QString &failureMessage) {
        CommandResult result;
        result.ok = false;
        result.code = failureCode;
        result.message = failureMessage;
        return result;
    }
};

class ControlApiAdapter {
public:
    virtual ~ControlApiAdapter() = default;

    virtual RecordingState recordingState() const = 0;
    virtual TransportState transportState() const = 0;
    virtual QVector<SourceState> sourceStates() const = 0;
    virtual ViewState viewState() const = 0;
    virtual SettingsState settingsState() const = 0;
    virtual MidiState midiState() const = 0;
    virtual StreamDeckState streamDeckState() const = 0;
    virtual ScreensState screensState() const = 0;
    virtual ImportState importState() const = 0;
    virtual TelemetryState telemetryState() const = 0;

    virtual CommandResult executeCommand(const QString &name, const QJsonObject &args) = 0;
};

#endif
