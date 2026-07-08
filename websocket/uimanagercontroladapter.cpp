#include "uimanagercontroladapter.h"

#include "playback/playbacktransport.h"
#include "streamdeck/streamdeckmanager.h"
#include "uimanager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QDebug>

namespace {

constexpr int kControlPgmWaitTimeoutMs = 250;

bool controlLatencyTraceEnabled() {
    const QByteArray raw = qgetenv("OLR_E2E_LATENCY_TRACE").trimmed().toLower();
    return raw == "1" || raw == "true" || raw == "on" || raw == "yes";
}

QJsonObject pgmTransactionDetails(const PlaybackWorker::OperatorSeekResult& result) {
    QJsonObject identity;
    identity.insert(QStringLiteral("busKind"), static_cast<int>(result.pgmIdentity.bus.kind));
    identity.insert(QStringLiteral("busIndex"), result.pgmIdentity.bus.index);
    identity.insert(QStringLiteral("sampledPlayheadMs"),
                    static_cast<double>(result.pgmIdentity.sampledPlayheadMs));
    identity.insert(QStringLiteral("sourcePtsMs"),
                    static_cast<double>(result.pgmIdentity.sourcePtsMs));
    identity.insert(QStringLiteral("sourceFeedIndex"), result.pgmIdentity.sourceFeedIndex);
    identity.insert(QStringLiteral("videoPlaceholder"), result.pgmIdentity.videoPlaceholder);

    QJsonObject transaction;
    transaction.insert(QStringLiteral("completed"), result.completed);
    transaction.insert(QStringLiteral("submittedPgm"), result.submittedPgm);
    transaction.insert(QStringLiteral("timedOut"), result.timedOut);
    transaction.insert(QStringLiteral("targetMs"), static_cast<double>(result.targetMs));
    transaction.insert(QStringLiteral("generation"),
                       QString::number(static_cast<qulonglong>(result.generation)));
    transaction.insert(QStringLiteral("elapsedNs"), static_cast<double>(result.elapsedNs));
    transaction.insert(QStringLiteral("message"), result.message);
    transaction.insert(QStringLiteral("identity"), identity);

    return QJsonObject{{QStringLiteral("pgmTransaction"), transaction}};
}

CommandResult resultForOperatorSeek(const QString& commandName,
                                    const PlaybackWorker::OperatorSeekResult& result) {
    if (controlLatencyTraceEnabled()) {
        qInfo().noquote() << QStringLiteral(
                                 "OLR_LATENCY control.command name=%1 targetMs=%2 generation=%3 "
                                 "completed=%4 submittedPgm=%5 timedOut=%6 elapsedNs=%7 "
                                 "sampledMs=%8 sourcePtsMs=%9 placeholder=%10 message=\"%11\"")
                                 .arg(commandName)
                                 .arg(result.targetMs)
                                 .arg(static_cast<qulonglong>(result.generation))
                                 .arg(result.completed ? 1 : 0)
                                 .arg(result.submittedPgm ? 1 : 0)
                                 .arg(result.timedOut ? 1 : 0)
                                 .arg(result.elapsedNs)
                                 .arg(result.pgmIdentity.sampledPlayheadMs)
                                 .arg(result.pgmIdentity.sourcePtsMs)
                                 .arg(result.pgmIdentity.videoPlaceholder ? 1 : 0)
                                 .arg(result.message);
    }
    if (result.completed && result.submittedPgm) {
        return CommandResult::success(pgmTransactionDetails(result));
    }
    const QString code =
        result.timedOut ? QStringLiteral("timeout") : QStringLiteral("pgm_not_submitted");
    const QString detail =
        result.message.isEmpty() ? QStringLiteral("PGM output was not submitted") : result.message;
    return CommandResult::failure(code, QStringLiteral("%1 (targetMs=%2 generation=%3)")
                                            .arg(detail)
                                            .arg(result.targetMs)
                                            .arg(static_cast<qulonglong>(result.generation)));
}

} // namespace

UIManagerControlAdapter::UIManagerControlAdapter(UIManager* uiManager, QObject* parent)
    : QObject(parent), m_uiManager(uiManager) {}

RecordingState UIManagerControlAdapter::recordingState() const {
    if (!m_uiManager) return {};
    return {m_uiManager->isRecording(), m_uiManager->recordedDurationMs(),
            m_uiManager->recordingStartEpochMs()};
}

TransportState UIManagerControlAdapter::transportState() const {
    if (!m_uiManager) return {};

    PlaybackTransport* transport = m_uiManager->transport();

    TransportState state;
    state.positionMs = transport ? transport->currentPos() : 0;
    state.scrubPositionMs = m_uiManager->scrubPosition();
    state.durationMs = m_uiManager->recordedDurationMs();
    state.timecode = m_uiManager->playbackTimecode();
    state.playing = transport ? transport->isPlaying() : false;
    state.speed = transport ? transport->speed() : 1.0;
    state.fps = transport ? transport->fps() : m_uiManager->recordFps();
    state.followLive = m_uiManager->followLive();
    state.liveBufferMs = m_uiManager->liveBufferMs();
    return state;
}

QVector<SourceState> UIManagerControlAdapter::sourceStates() const {
    QVector<SourceState> states;
    if (!m_uiManager) return states;

    const QStringList ids = m_uiManager->streamIds();
    const QStringList names = m_uiManager->streamNames();
    const QStringList urls = m_uiManager->streamUrls();
    states.reserve(urls.size());
    for (int i = 0; i < urls.size(); ++i) {
        SourceState state;
        state.index = i;
        state.id = ids.value(i);
        state.name = names.value(i);
        state.url = urls.value(i);
        state.enabled = m_uiManager->isSourceEnabled(i);
        state.connected = m_uiManager->isSourceConnected(i);
        state.duplicateUrl = m_uiManager->hasDuplicateUrl(i);
        state.trimOffsetMs = m_uiManager->sourceTrimOffset(i);
        state.metadata = m_uiManager->sourceMetadataItems(i);
        states.append(state);
    }
    return states;
}

ViewState UIManagerControlAdapter::viewState() const {
    if (!m_uiManager) return {};

    ViewState state;
    state.multiviewCount = m_uiManager->multiviewCount();
    state.slotMap = m_uiManager->viewSlotMap();
    state.singleView = m_uiManager->playbackSingleView();
    state.selectedIndex = m_uiManager->playbackSelectedIndex();
    return state;
}

SettingsState UIManagerControlAdapter::settingsState() const {
    if (!m_uiManager) return {};

    SettingsState state;
    state.fileName = m_uiManager->fileName();
    state.saveLocation = m_uiManager->saveLocation();
    state.recordWidth = m_uiManager->recordWidth();
    state.recordHeight = m_uiManager->recordHeight();
    state.recordFps = m_uiManager->recordFps();
    state.audioOutputLatencyMs = m_uiManager->audioOutputLatencyMs();
    state.timeOfDayMode = m_uiManager->timeOfDayMode();
    state.metadataFields = m_uiManager->metadataFieldDefinitions();
    return state;
}

MidiState UIManagerControlAdapter::midiState() const {
    if (!m_uiManager) return {};

    return {m_uiManager->midiPorts(),       m_uiManager->midiPortIndex(),
            m_uiManager->midiPortName(),    m_uiManager->midiConnected(),
            m_uiManager->midiLearnAction(), m_uiManager->midiLearnMode()};
}

StreamDeckState UIManagerControlAdapter::streamDeckState() const {
    if (!m_uiManager) return {};

    StreamDeckManager* deck = m_uiManager->streamDeck();
    if (!deck) return {};

    return {deck->supported(),
            deck->connected(),
            deck->deviceName(),
            deck->deviceModel(),
            deck->keyCount(),
            deck->dialCount(),
            m_uiManager->streamDeckLearnAction()};
}

ScreensState UIManagerControlAdapter::screensState() const {
    if (!m_uiManager) return {};

    return {m_uiManager->screensReady(), m_uiManager->screenCount(), m_uiManager->screenOptions()};
}

ImportState UIManagerControlAdapter::importState() const {
    if (!m_uiManager) return {};

    return {m_uiManager->importSettingsUrl(), m_uiManager->telemetrySseUrl(),
            m_uiManager->importPreviewReady(), m_uiManager->importPreviewError(),
            m_uiManager->importPreview()};
}

TelemetryState UIManagerControlAdapter::telemetryState() const {
    if (!m_uiManager) return {};

    return {m_uiManager->telemetryVersion(), m_uiManager->telemetryRowsAtPlayhead(),
            m_uiManager->telemetryAtPlayhead()};
}

QVariantMap UIManagerControlAdapter::outputState() const {
    if (!m_uiManager) return {};
    return m_uiManager->previewOutputState();
}

CommandResult UIManagerControlAdapter::executeCommand(const QString& name,
                                                      const QJsonObject& args) {
    if (!m_uiManager) {
        return CommandResult::failure(QStringLiteral("failed"),
                                      QStringLiteral("UIManager is unavailable"));
    }

    PlaybackTransport* transport = m_uiManager->transport();

    if (name == QStringLiteral("transport.playPause")) {
        m_uiManager->playPause();
    } else if (name == QStringLiteral("transport.play")) {
        if (transport) transport->setPlaying(true);
    } else if (name == QStringLiteral("transport.pause")) {
        if (transport) transport->setPlaying(false);
    } else if (name == QStringLiteral("transport.setSpeed")) {
        if (transport) {
            transport->setSpeed(args.value(QStringLiteral("speed")).toDouble());
            if (args.contains(QStringLiteral("playing"))) {
                transport->setPlaying(args.value(QStringLiteral("playing")).toBool());
            }
        }
    } else if (name == QStringLiteral("transport.holdSpeed")) {
        const bool active = args.value(QStringLiteral("active")).toBool();
        const QString clientId = args.value(QStringLiteral("_clientId")).toString();
        if (!active) {
            if (m_holdSpeedClientId.isEmpty() || m_holdSpeedClientId != clientId) {
                return CommandResult::success();
            }
            if (transport) {
                transport->setPlaying(m_holdSpeedWasPlaying);
                transport->setSpeed(1.0);
            }
            m_holdSpeedClientId.clear();
            m_holdSpeedWasPlaying = false;
        } else {
            if (!m_holdSpeedClientId.isEmpty() && m_holdSpeedClientId != clientId) {
                return CommandResult::failure(
                    QStringLiteral("not_allowed"),
                    QStringLiteral("Another hold speed command is already active"));
            }
            if (transport) {
                if (m_holdSpeedClientId.isEmpty()) {
                    m_holdSpeedWasPlaying = transport->isPlaying();
                    m_holdSpeedClientId = clientId;
                }
                transport->setSpeed(args.value(QStringLiteral("speed")).toDouble());
                transport->setPlaying(true);
            }
        }
    } else if (name == QStringLiteral("transport.stepFrame")) {
        return resultForOperatorSeek(
            name, m_uiManager->jogExternalAndWaitForPgm(
                      args.value(QStringLiteral("frames")).toInt(), kControlPgmWaitTimeoutMs));
    } else if (name == QStringLiteral("transport.seek")) {
        return resultForOperatorSeek(
            name, m_uiManager->seekPlaybackAndWaitForPgm(
                      args.value(QStringLiteral("positionMs")).toVariant().toLongLong(),
                      kControlPgmWaitTimeoutMs));
    } else if (name == QStringLiteral("transport.goLive")) {
        m_uiManager->goLive();
    } else if (name == QStringLiteral("transport.cancelFollowLive")) {
        m_uiManager->cancelFollowLive();
    } else if (name == QStringLiteral("recording.start")) {
        m_uiManager->startRecording();
    } else if (name == QStringLiteral("recording.stop")) {
        m_uiManager->stopRecording();
    } else if (name == QStringLiteral("recording.toggle")) {
        if (m_uiManager->isRecording())
            m_uiManager->stopRecording();
        else
            m_uiManager->startRecording();
    } else if (name == QStringLiteral("view.setPlaybackViewState")) {
        m_uiManager->setPlaybackViewState(args.value(QStringLiteral("singleView")).toBool(),
                                          args.value(QStringLiteral("selectedIndex")).toInt());
    } else if (name == QStringLiteral("view.showMultiview")) {
        m_uiManager->dispatchExternalAction(6, true);
    } else if (name == QStringLiteral("view.selectFeed")) {
        m_uiManager->selectFeedExternal(args.value(QStringLiteral("index")).toInt());
    } else if (name == QStringLiteral("view.toggleSourceEnabled")) {
        m_uiManager->toggleSourceEnabled(args.value(QStringLiteral("index")).toInt());
    } else if (name == QStringLiteral("capture.current")) {
        m_uiManager->captureCurrent();
    } else if (name == QStringLiteral("capture.snapshot")) {
        m_uiManager->captureSnapshot(
            args.value(QStringLiteral("singleView")).toBool(),
            args.value(QStringLiteral("selectedIndex")).toInt(),
            args.value(QStringLiteral("playheadMs")).toVariant().toLongLong());
    } else if (name == QStringLiteral("sources.add")) {
        m_uiManager->addStream();
    } else if (name == QStringLiteral("sources.remove")) {
        m_uiManager->removeStream(args.value(QStringLiteral("index")).toInt());
    } else if (name == QStringLiteral("sources.updateUrl")) {
        m_uiManager->updateUrl(args.value(QStringLiteral("index")).toInt(),
                               args.value(QStringLiteral("url")).toString());
    } else if (name == QStringLiteral("sources.updateName")) {
        m_uiManager->updateStreamName(args.value(QStringLiteral("index")).toInt(),
                                      args.value(QStringLiteral("name")).toString());
    } else if (name == QStringLiteral("sources.updateId")) {
        m_uiManager->updateStreamId(args.value(QStringLiteral("index")).toInt(),
                                    args.value(QStringLiteral("id")).toString());
    } else if (name == QStringLiteral("sources.setTrimOffset")) {
        m_uiManager->setSourceTrimOffset(args.value(QStringLiteral("index")).toInt(),
                                         args.value(QStringLiteral("ms")).toInt());
    } else if (name == QStringLiteral("sources.setMetadata")) {
        m_uiManager->setSourceMetadataItems(
            args.value(QStringLiteral("index")).toInt(),
            args.value(QStringLiteral("items")).toArray().toVariantList());
    } else if (name == QStringLiteral("settings.setProject")) {
        if (args.contains(QStringLiteral("fileName"))) {
            m_uiManager->setFileName(args.value(QStringLiteral("fileName")).toString());
        }
        if (args.contains(QStringLiteral("saveLocation"))) {
            m_uiManager->setSaveLocation(args.value(QStringLiteral("saveLocation")).toString());
        }
    } else if (name == QStringLiteral("settings.setRecordingFormat")) {
        if (m_uiManager->isRecording() && args.contains(QStringLiteral("fps"))) {
            return CommandResult::failure(QStringLiteral("not_allowed"),
                                          QStringLiteral("FPS cannot be changed while recording"));
        }
        if (args.contains(QStringLiteral("width"))) {
            m_uiManager->setRecordWidth(args.value(QStringLiteral("width")).toInt());
        }
        if (args.contains(QStringLiteral("height"))) {
            m_uiManager->setRecordHeight(args.value(QStringLiteral("height")).toInt());
        }
        if (args.contains(QStringLiteral("fps"))) {
            m_uiManager->setRecordFps(args.value(QStringLiteral("fps")).toInt());
        }
    } else if (name == QStringLiteral("settings.setAudioOutputLatency")) {
        m_uiManager->setAudioOutputLatencyMs(args.value(QStringLiteral("ms")).toInt());
    } else if (name == QStringLiteral("settings.setTimeOfDayMode")) {
        m_uiManager->setTimeOfDayMode(args.value(QStringLiteral("enabled")).toBool());
    } else if (name == QStringLiteral("settings.setMetadataFields")) {
        m_uiManager->setMetadataFieldDefinitions(
            args.value(QStringLiteral("fields")).toArray().toVariantList());
    } else if (name == QStringLiteral("outputs.ndi.setEnabled")) {
        m_uiManager->setNdiOutputEnabled(args.value(QStringLiteral("busKind")).toString(),
                                         args.value(QStringLiteral("feedIndex")).toInt(),
                                         args.value(QStringLiteral("enabled")).toBool());
    } else if (name == QStringLiteral("outputs.ndi.setSenderName")) {
        m_uiManager->setNdiOutputSenderName(args.value(QStringLiteral("busKind")).toString(),
                                            args.value(QStringLiteral("feedIndex")).toInt(),
                                            args.value(QStringLiteral("senderName")).toString());
    } else if (name == QStringLiteral("settings.save")) {
        m_uiManager->saveSettings();
    } else if (name == QStringLiteral("import.setUrl")) {
        m_uiManager->setImportSettingsUrl(args.value(QStringLiteral("url")).toString());
    } else if (name == QStringLiteral("import.read")) {
        m_uiManager->readImportSettings();
    } else if (name == QStringLiteral("import.applyPreview")) {
        m_uiManager->applyImportPreview();
    } else if (name == QStringLiteral("midi.refreshPorts")) {
        m_uiManager->refreshMidiPorts();
    } else if (name == QStringLiteral("midi.setPortIndex")) {
        m_uiManager->setMidiPortIndex(args.value(QStringLiteral("index")).toInt());
    } else if (name == QStringLiteral("midi.beginLearn")) {
        m_uiManager->beginMidiLearn(args.value(QStringLiteral("action")).toInt());
    } else if (name == QStringLiteral("midi.beginLearnJogForward")) {
        m_uiManager->beginMidiLearnJogForward(args.value(QStringLiteral("action")).toInt());
    } else if (name == QStringLiteral("midi.beginLearnJogBackward")) {
        m_uiManager->beginMidiLearnJogBackward(args.value(QStringLiteral("action")).toInt());
    } else if (name == QStringLiteral("midi.clearBinding")) {
        m_uiManager->clearMidiBinding(args.value(QStringLiteral("action")).toInt());
    } else if (name == QStringLiteral("streamDeck.beginLearn")) {
        m_uiManager->beginStreamDeckLearn(args.value(QStringLiteral("action")).toInt());
    } else if (name == QStringLiteral("streamDeck.clearBinding")) {
        m_uiManager->clearStreamDeckBinding(args.value(QStringLiteral("action")).toInt());
    } else if (name == QStringLiteral("streamDeck.resetDefaults")) {
        m_uiManager->resetStreamDeckDefaults();
    } else if (name == QStringLiteral("action.dispatch")) {
        m_uiManager->dispatchExternalAction(args.value(QStringLiteral("actionId")).toInt(),
                                            args.value(QStringLiteral("pressed")).toBool());
    } else if (name == QStringLiteral("action.jog")) {
        return resultForOperatorSeek(
            name, m_uiManager->jogExternalAndWaitForPgm(args.value(QStringLiteral("delta")).toInt(),
                                                        kControlPgmWaitTimeoutMs));
    } else if (name == QStringLiteral("action.shuttle")) {
        m_uiManager->shuttleExternal(args.value(QStringLiteral("delta")).toInt());
    } else {
        return CommandResult::failure(QStringLiteral("unknown_command"),
                                      QStringLiteral("Unknown command"));
    }

    return CommandResult::success();
}
