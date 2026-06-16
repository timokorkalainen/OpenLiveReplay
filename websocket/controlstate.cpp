#include "controlstate.h"

#include "controlapiadapter.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {

QJsonArray variantListToArray(const QVariantList &list) {
    return QJsonArray::fromVariantList(list);
}

QJsonObject variantMapToObject(const QVariantMap &map) {
    return QJsonObject::fromVariantMap(map);
}

QJsonObject sourceObject(const SourceState &source) {
    QJsonObject obj;
    obj.insert(QStringLiteral("index"), source.index);
    obj.insert(QStringLiteral("id"), source.id);
    obj.insert(QStringLiteral("name"), source.name);
    obj.insert(QStringLiteral("url"), source.url);
    obj.insert(QStringLiteral("enabled"), source.enabled);
    obj.insert(QStringLiteral("connected"), source.connected);
    obj.insert(QStringLiteral("duplicateUrl"), source.duplicateUrl);
    obj.insert(QStringLiteral("trimOffsetMs"), source.trimOffsetMs);
    obj.insert(QStringLiteral("metadata"), variantListToArray(source.metadata));
    return obj;
}

} // namespace

QJsonObject ControlState::snapshotMessage(const ControlApiAdapter &adapter) {
    QJsonObject state;
    state.insert(QStringLiteral("recording"), recordingObject(adapter));
    state.insert(QStringLiteral("transport"), transportObject(adapter));

    QJsonArray sources;
    for (const SourceState &source : adapter.sourceStates()) {
        sources.append(sourceObject(source));
    }
    state.insert(QStringLiteral("sources"), sources);

    const ViewState view = adapter.viewState();
    QJsonObject viewObj;
    viewObj.insert(QStringLiteral("multiviewCount"), view.multiviewCount);
    viewObj.insert(QStringLiteral("slotMap"), variantListToArray(view.slotMap));
    viewObj.insert(QStringLiteral("singleView"), view.singleView);
    viewObj.insert(QStringLiteral("selectedIndex"), view.selectedIndex);
    state.insert(QStringLiteral("views"), viewObj);

    state.insert(QStringLiteral("settings"), settingsObject(adapter));

    const MidiState midi = adapter.midiState();
    QJsonObject midiObj;
    midiObj.insert(QStringLiteral("ports"), QJsonArray::fromStringList(midi.ports));
    midiObj.insert(QStringLiteral("portIndex"), midi.portIndex);
    midiObj.insert(QStringLiteral("portName"), midi.portName);
    midiObj.insert(QStringLiteral("connected"), midi.connected);
    midiObj.insert(QStringLiteral("learnAction"), midi.learnAction);
    midiObj.insert(QStringLiteral("learnMode"), midi.learnMode);
    state.insert(QStringLiteral("midi"), midiObj);

    const StreamDeckState deck = adapter.streamDeckState();
    QJsonObject deckObj;
    deckObj.insert(QStringLiteral("supported"), deck.supported);
    deckObj.insert(QStringLiteral("connected"), deck.connected);
    deckObj.insert(QStringLiteral("deviceName"), deck.deviceName);
    deckObj.insert(QStringLiteral("deviceModel"), deck.deviceModel);
    deckObj.insert(QStringLiteral("keyCount"), deck.keyCount);
    deckObj.insert(QStringLiteral("dialCount"), deck.dialCount);
    deckObj.insert(QStringLiteral("learnAction"), deck.learnAction);
    state.insert(QStringLiteral("streamDeck"), deckObj);

    const ScreensState screens = adapter.screensState();
    QJsonObject screensObj;
    screensObj.insert(QStringLiteral("ready"), screens.ready);
    screensObj.insert(QStringLiteral("count"), screens.count);
    screensObj.insert(QStringLiteral("options"), variantListToArray(screens.options));
    state.insert(QStringLiteral("screens"), screensObj);

    const ImportState import = adapter.importState();
    QJsonObject importObj;
    importObj.insert(QStringLiteral("settingsUrl"), import.settingsUrl);
    importObj.insert(QStringLiteral("telemetrySseUrl"), import.telemetrySseUrl);
    importObj.insert(QStringLiteral("previewReady"), import.previewReady);
    importObj.insert(QStringLiteral("previewError"), import.previewError);
    importObj.insert(QStringLiteral("preview"), variantMapToObject(import.preview));
    state.insert(QStringLiteral("import"), importObj);

    state.insert(QStringLiteral("telemetry"), telemetryObject(adapter));

    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("state.snapshot"));
    msg.insert(QStringLiteral("state"), state);
    return msg;
}

QJsonObject ControlState::patchMessage(const QString &path, const QJsonObject &value) {
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("state.patch"));
    msg.insert(QStringLiteral("path"), path);
    msg.insert(QStringLiteral("value"), value);
    return msg;
}

QJsonObject ControlState::timecodeMessage(const ControlApiAdapter &adapter) {
    const TransportState transport = adapter.transportState();
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("timecode"));
    msg.insert(QStringLiteral("positionMs"), transport.scrubPositionMs);
    msg.insert(QStringLiteral("durationMs"), transport.durationMs);
    msg.insert(QStringLiteral("text"), transport.timecode);
    msg.insert(QStringLiteral("followLive"), transport.followLive);
    msg.insert(QStringLiteral("playing"), transport.playing);
    msg.insert(QStringLiteral("speed"), transport.speed);
    return msg;
}

QJsonObject ControlState::recordingObject(const ControlApiAdapter &adapter) {
    const RecordingState recording = adapter.recordingState();
    QJsonObject obj;
    obj.insert(QStringLiteral("active"), recording.active);
    obj.insert(QStringLiteral("durationMs"), recording.durationMs);
    obj.insert(QStringLiteral("startEpochMs"), recording.startEpochMs);
    return obj;
}

QJsonObject ControlState::transportObject(const ControlApiAdapter &adapter) {
    const TransportState transport = adapter.transportState();
    QJsonObject obj;
    obj.insert(QStringLiteral("positionMs"), transport.positionMs);
    obj.insert(QStringLiteral("scrubPositionMs"), transport.scrubPositionMs);
    obj.insert(QStringLiteral("durationMs"), transport.durationMs);
    obj.insert(QStringLiteral("timecode"), transport.timecode);
    obj.insert(QStringLiteral("playing"), transport.playing);
    obj.insert(QStringLiteral("speed"), transport.speed);
    obj.insert(QStringLiteral("fps"), transport.fps);
    obj.insert(QStringLiteral("followLive"), transport.followLive);
    obj.insert(QStringLiteral("liveBufferMs"), transport.liveBufferMs);
    return obj;
}

QJsonObject ControlState::settingsObject(const ControlApiAdapter &adapter) {
    const SettingsState settings = adapter.settingsState();
    QJsonObject obj;
    obj.insert(QStringLiteral("fileName"), settings.fileName);
    obj.insert(QStringLiteral("saveLocation"), settings.saveLocation);
    obj.insert(QStringLiteral("recordWidth"), settings.recordWidth);
    obj.insert(QStringLiteral("recordHeight"), settings.recordHeight);
    obj.insert(QStringLiteral("recordFps"), settings.recordFps);
    obj.insert(QStringLiteral("audioOutputLatencyMs"), settings.audioOutputLatencyMs);
    obj.insert(QStringLiteral("timeOfDayMode"), settings.timeOfDayMode);
    obj.insert(QStringLiteral("metadataFields"), variantListToArray(settings.metadataFields));
    return obj;
}

QJsonObject ControlState::telemetryObject(const ControlApiAdapter &adapter) {
    const TelemetryState telemetry = adapter.telemetryState();
    QJsonObject obj;
    obj.insert(QStringLiteral("version"), telemetry.version);
    obj.insert(QStringLiteral("rows"), variantListToArray(telemetry.rows));
    obj.insert(QStringLiteral("state"), variantMapToObject(telemetry.state));
    return obj;
}
