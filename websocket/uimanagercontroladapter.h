#ifndef UIMANAGERCONTROLADAPTER_H
#define UIMANAGERCONTROLADAPTER_H

#include "controlapiadapter.h"

#include <QObject>

class UIManager;

class UIManagerControlAdapter : public QObject, public ControlApiAdapter {
    Q_OBJECT
public:
    explicit UIManagerControlAdapter(UIManager* uiManager, QObject* parent = nullptr);

    RecordingState recordingState() const override;
    TransportState transportState() const override;
    QVector<SourceState> sourceStates() const override;
    ViewState viewState() const override;
    SettingsState settingsState() const override;
    MidiState midiState() const override;
    StreamDeckState streamDeckState() const override;
    ScreensState screensState() const override;
    ImportState importState() const override;
    TelemetryState telemetryState() const override;
    CommandResult executeCommand(const QString& name, const QJsonObject& args) override;

private:
    UIManager* m_uiManager = nullptr;
    QString m_holdSpeedClientId;
    bool m_holdSpeedWasPlaying = false;
};

#endif
