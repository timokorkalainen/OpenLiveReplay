#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "recorder_engine/replaymanager.h"
#include "uimanager.h"
#include "playback/frameprovider.h"
#include "streamdeck/streamdeckmanager.h"
#include "websocket/controlstate.h"
#include "websocket/controlwebsocketserver.h"
#include "websocket/uimanagercontroladapter.h"
#include <QHostAddress>
#include <QWarning>

#include <QString>
using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    ReplayManager replayManager;
    UIManager uiManager(&replayManager);

    /*QDirIterator it(":", QDirIterator::Subdirectories);
    while (it.hasNext()) {
        qDebug() << it.next();
    }*/

    uiManager.loadSettings();

    UIManagerControlAdapter controlAdapter(&uiManager);
    ControlWebSocketServer controlServer(&controlAdapter);
    if (!controlServer.listen(QHostAddress::Any, 8115)) {
        qWarning() << "WebSocket control API failed to listen on port 8115:"
                   << controlServer.lastError();
    }

    auto publishRecording = [&controlServer]() { controlServer.publishPatch(QStringLiteral("recording")); };
    auto publishTransport = [&controlServer]() {
        controlServer.publishPatch(QStringLiteral("transport"));
        controlServer.publishTimecodeNow();
    };
    auto publishSettings = [&controlServer]() { controlServer.publishPatch(QStringLiteral("settings")); };
    auto publishTelemetry = [&controlServer]() { controlServer.publishPatch(QStringLiteral("telemetry")); };

    auto publishFullSnapshot = [&controlServer, &controlAdapter]() {
        controlServer.publishPatchObject(QStringLiteral("snapshot"), ControlState::snapshotMessage(controlAdapter).value(QStringLiteral("state")).toObject());
    };
    auto publishSources = publishFullSnapshot;
    auto publishViews = publishFullSnapshot;
    auto publishImport = publishFullSnapshot;
    auto publishMidi = publishFullSnapshot;
    auto publishStreamDeck = publishFullSnapshot;
    auto publishScreens = publishFullSnapshot;

    QObject::connect(&uiManager, &UIManager::recordingStatusChanged, &controlServer, publishRecording);
    QObject::connect(&uiManager, &UIManager::recordingStartEpochMsChanged, &controlServer, publishRecording);
    QObject::connect(&uiManager, &UIManager::recordedDurationMsChanged, &controlServer, publishRecording);
    QObject::connect(&uiManager, &UIManager::recordingStarted, &controlServer, [&controlServer]() {
        controlServer.publishEvent(QStringLiteral("recording.started"));
    });
    QObject::connect(&uiManager, &UIManager::recordingStopped, &controlServer, [&controlServer]() {
        controlServer.publishEvent(QStringLiteral("recording.stopped"));
    });
    QObject::connect(&uiManager, &UIManager::recordingFailed, &controlServer, [&controlServer](const QString &reason) {
        controlServer.publishEvent(QStringLiteral("recording.failed"), QJsonObject{{QStringLiteral("reason"), reason}});
    });
    QObject::connect(&uiManager, &UIManager::playbackTimecodeChanged, &controlServer, [&controlServer]() {
        controlServer.scheduleTimecode();
    });
    QObject::connect(&uiManager, &UIManager::followLiveChanged, &controlServer, publishTransport);

    QObject::connect(&uiManager, &UIManager::saveLocationChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::fileNameChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::recordWidthChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::recordHeightChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::recordFpsChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::audioOutputLatencyChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::timeOfDayModeChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::metadataFieldsChanged, &controlServer, publishSettings);
    QObject::connect(&uiManager, &UIManager::telemetryChanged, &controlServer, publishTelemetry);

    QObject::connect(&uiManager, &UIManager::streamUrlsChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::streamNamesChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::streamIdsChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::sourceEnabledChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::sourceConnectionChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::sourceTrimChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::sourceMetadataChanged, &controlServer, publishSources);
    QObject::connect(&uiManager, &UIManager::viewSlotMapChanged, &controlServer, publishViews);
    QObject::connect(&uiManager, &UIManager::multiviewCountChanged, &controlServer, publishViews);
    QObject::connect(&uiManager, &UIManager::playbackViewStateChanged, &controlServer, publishViews);
    QObject::connect(&uiManager, &UIManager::importSettingsUrlChanged, &controlServer, publishImport);
    QObject::connect(&uiManager, &UIManager::importPreviewChanged, &controlServer, publishImport);
    QObject::connect(&uiManager, &UIManager::telemetryConfigChanged, &controlServer, publishImport);
    QObject::connect(&uiManager, &UIManager::midiPortsChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::midiPortIndexChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::midiConnectedChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::midiLearnActionChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::midiBindingsChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::midiLastValuesChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::midiPortNameChanged, &controlServer, publishMidi);
    QObject::connect(&uiManager, &UIManager::streamDeckLearnActionChanged, &controlServer, publishStreamDeck);
    QObject::connect(&uiManager, &UIManager::streamDeckBindingsChanged, &controlServer, publishStreamDeck);
    QObject::connect(&uiManager, &UIManager::screensChanged, &controlServer, publishScreens);

    if (const auto transport = uiManager.transport()) {
        QObject::connect(transport, &PlaybackTransport::posChanged, &controlServer, [&controlServer]() {
            controlServer.scheduleTimecode();
        });
        QObject::connect(transport, &PlaybackTransport::playingChanged, &controlServer, publishTransport);
        QObject::connect(transport, &PlaybackTransport::speedChanged, &controlServer, publishTransport);
        QObject::connect(transport, &PlaybackTransport::fpsChanged, &controlServer, publishTransport);
    }

    qmlRegisterType<FrameProvider>("Recorder.Types", 1, 0, "FrameProvider");
    qmlRegisterType<PlaybackTransport>("Recorder.Types", 1, 0, "PlaybackTransport");
    qmlRegisterUncreatableType<StreamDeckManager>(
        "Recorder.Types", 1, 0, "StreamDeckManager",
        "Exposed via uiManager.streamDeck");

    QQmlApplicationEngine qmlEngine;

    // This makes the 'uiManager' object globally available in QML
    qmlEngine.rootContext()->setContextProperty("uiManager", &uiManager);

    QObject::connect(
        &qmlEngine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    //qmlEngine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    //qmlEngine.load(QUrl(u":/qt/qml/OpenLiveReplay/Main.qml"_s));
    qmlEngine.load(QUrl(u"qrc:/qt/qml/OpenLiveReplay/Main.qml"_s));

    return app.exec();
}
