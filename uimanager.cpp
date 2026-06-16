#include "uimanager.h"
#include "playback/audioplayer.h"
#include "project/projectimportclient.h"
#include "telemetry/telemetryclient.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <algorithm>
#include <QDir>
#include <QGuiApplication>
#include <QDebug>
#if defined(Q_OS_IOS)
#include "ios/ios_scene.h"
#endif
#include <QImageWriter>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QTimer>
#include <QScreen>
#include <QSet>
#include <QVariantMap>
#include <cstdio>

namespace {

QString telemetryValueToString(const QVariant &value) {
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (json.isObject()) {
        return QString::fromUtf8(QJsonDocument(json.toObject()).toJson(QJsonDocument::Compact));
    }
    if (json.isArray()) {
        return QString::fromUtf8(QJsonDocument(json.toArray()).toJson(QJsonDocument::Compact));
    }
    if (json.isBool()) {
        return json.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (json.isDouble()) {
        return QString::number(json.toDouble());
    }
    if (json.isNull() || json.isUndefined()) {
        return QString();
    }
    return json.toString();
}

QString metadataSummary(const QJsonArray &metadata) {
    QStringList parts;
    for (const QJsonValue &value : metadata) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        const QString entryValue = object.value(QStringLiteral("value")).toString();
        if (name.isEmpty() || entryValue.isEmpty()) continue;
        parts.append(name + QStringLiteral("=") + entryValue);
        if (parts.size() >= 4) break;
    }
    return parts.join(QStringLiteral("  "));
}

} // namespace

UIManager::UIManager(ReplayManager *engine, QObject *parent)
    : QObject(parent), m_replayManager(engine) {
    m_jogTimer.start();
    connect(m_replayManager, &ReplayManager::masterPulse,
            this, &UIManager::onRecorderPulse,
            Qt::QueuedConnection);
    connect(m_replayManager, &ReplayManager::sourceConnectionChanged, this,
            &UIManager::onSourceConnectionChanged, Qt::QueuedConnection);
    connect(m_replayManager, &ReplayManager::sourceStatsUpdated, this,
            &UIManager::onSourceStatsUpdated, Qt::QueuedConnection);
    {
        // Amber lights when the windowed retransmit rate exceeds this fraction of
        // received packets. ARQ retransmits routinely on healthy lossy links, so
        // a presence test would be permanently amber; 2% is an early-stress warning.
        bool ok = false;
        const double pct = qEnvironmentVariable("OLR_SRT_HEALTH_AMBER_PCT").toDouble(&ok);
        if (ok && pct > 0.0) m_srtAmberPct = pct;
    }
    connect(m_replayManager, &ReplayManager::telemetryRecorded, this,
            [this](const QString &feedId, const QJsonObject &payload, qint64 effectiveMs) {
        if (feedId.trimmed().isEmpty()) return;
        TelemetryTimelineEntry entry;
        entry.ptsMs = effectiveMs;
        entry.payload = payload.toVariantMap();
        entry.payload.insert(QStringLiteral("feedId"), feedId);
        m_recordingTelemetry[feedId].append(entry);
        if (m_replayManager && m_replayManager->isRecording()) {
            loadTelemetryTimeline(m_replayManager->getVideoPath(), false);
        }
        m_telemetryVersion++;
        emit telemetryChanged();
    });
    m_settingsManager = new SettingsManager();
    m_importClient = new ProjectImportClient(this);
    connect(m_importClient, &ProjectImportClient::finished, this,
            [this](const QByteArray &body, const QString &sourceUrl) {
        if (sourceUrl != m_currentSettings.importSettingsUrl.trimmed()) {
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            m_pendingImport = ProjectSettingsImportResult{};
            m_hasPendingImport = false;
            m_importPreview.clear();
            m_importPreviewError = parseError.error == QJsonParseError::NoError
                ? QStringLiteral("Import settings response must be a JSON object")
                : parseError.errorString();
            emit importPreviewChanged();
            return;
        }

        ProjectSettingsImportResult result = m_settingsImporter.importJson(doc.object(), sourceUrl);
        if (!result.ok) {
            m_pendingImport = ProjectSettingsImportResult{};
            m_hasPendingImport = false;
            m_importPreview.clear();
            m_importPreviewError = result.error;
            emit importPreviewChanged();
            return;
        }

        QVariantList feeds;
        feeds.reserve(result.sources.size());
        for (const SourceSettings &source : result.sources) {
            QVariantMap feed;
            feed.insert(QStringLiteral("id"), source.id);
            feed.insert(QStringLiteral("name"), source.name);
            feed.insert(QStringLiteral("url"), source.url);
            feed.insert(QStringLiteral("telemetryDelayMs"), source.telemetryDelayMs);
            feed.insert(QStringLiteral("metadata"), source.metadata.toVariantList());
            feeds.append(feed);
        }

        QVariantList metadataFields;
        metadataFields.reserve(result.metadataFields.size());
        for (const QJsonValue &field : result.metadataFields) {
            metadataFields.append(field.toObject().toVariantMap());
        }

        QVariantMap preview;
        preview.insert(QStringLiteral("projectId"), result.projectId);
        preview.insert(QStringLiteral("projectName"), result.projectName);
        preview.insert(QStringLiteral("importSettingsUrl"), result.importSettingsUrl);
        preview.insert(QStringLiteral("telemetrySseUrl"), result.telemetrySseUrl);
        preview.insert(QStringLiteral("feedCount"), result.sources.size());
        preview.insert(QStringLiteral("metadataFields"), metadataFields);
        preview.insert(QStringLiteral("feeds"), feeds);

        m_pendingImport = result;
        m_hasPendingImport = true;
        m_importPreview = preview;
        m_importPreviewError.clear();
        emit importPreviewChanged();
    });
    connect(m_importClient, &ProjectImportClient::failed, this, [this](const QString &message) {
        m_pendingImport = ProjectSettingsImportResult{};
        m_hasPendingImport = false;
        m_importPreview.clear();
        m_importPreviewError = message;
        emit importPreviewChanged();
    });

    m_telemetryClient = new TelemetryClient(this);
    connect(m_telemetryClient, &TelemetryClient::telemetryEvent, this,
            [this](const TelemetryEvent &event) {
        const QString feedId = event.feedId.trimmed();
        if (feedId.isEmpty()) return;
        const auto matchesFeedId = [&feedId](const SourceSettings &source) {
            return source.id == feedId;
        };
        if (std::none_of(m_currentSettings.sources.cbegin(),
                         m_currentSettings.sources.cend(),
                         matchesFeedId)) {
            qWarning() << "TelemetryClient: ignoring telemetry for unknown feedId" << feedId;
            return;
        }

        QJsonObject payload = event.payload;
        payload.insert(QStringLiteral("feedId"), feedId);
        m_liveTelemetry.insert(feedId, payload.toVariantMap());
        m_telemetryVersion++;
        emit telemetryChanged();

        if (m_replayManager && m_replayManager->isRecording()) {
            m_replayManager->recordTelemetryEvent(feedId, payload);
        }
    });
    connect(m_telemetryClient, &TelemetryClient::errorOccurred, this,
            [](const QString &message) {
        qWarning() << "TelemetryClient:" << message;
    });
    m_configPath = getSettingsPath("config.json");
    m_transport = new PlaybackTransport(this);
    m_transport->seek(0);
    m_transport->setFps(FrameRate{m_currentSettings.fpsNum, m_currentSettings.fpsDen}.roundedFps());
    connect(m_transport, &PlaybackTransport::posChanged, this, [this]() {
        updateXTouchDisplay();
    });

    // Audio output for single-view playback
    m_audioPlayer = new AudioPlayer(this);
    m_audioPlayer->start(48000, 2);
    m_audioPlayer->setMuted(true); // start muted until a single view is selected

    // Mute audio when speed is not 1.0
    connect(m_transport, &PlaybackTransport::speedChanged, this, [this](double speed) {
        if (m_audioPlayer) {
            bool normalSpeed = (speed > 0.99 && speed < 1.01);
            m_audioPlayer->setMuted(!normalSpeed || !m_playbackSingleView);
        }
    });
    // Clear audio buffer when pausing
    connect(m_transport, &PlaybackTransport::playingChanged, this, [this](bool playing) {
        if (m_audioPlayer && !playing) {
            m_audioPlayer->clear();
        }
    });

    m_midiManager = new MidiManager(this);
    connect(m_midiManager, &MidiManager::midiMessage, this, [this](int status, int data1, int data2) {
        if (status < 0 || data1 < 0) return;

        const int statusType = status & 0xF0;
        const bool isNoteOff = (statusType == 0x80) || (statusType == 0x90 && data2 <= 0);
        const bool isControlRelease = (statusType == 0xB0 && data2 <= 0);
        const bool isRelease = isNoteOff || isControlRelease;

        if (m_midiLearnAction >= 0) {
            if (m_midiLearnMode == LearnControl) {
                m_midiBindings[m_midiLearnAction] = {status, data1, data2};
                m_currentSettings.midiBindings.insert(m_midiLearnAction, qMakePair(status, data1));
                m_currentSettings.midiBindingData2.insert(m_midiLearnAction, data2);
            } else if (m_midiLearnMode == LearnJogForward || m_midiLearnMode == LearnJogBackward) {
                const auto existing = m_midiBindings.value(m_midiLearnAction, MidiBinding{});
                if (existing.status < 0 || existing.data1 < 0) {
                    m_midiBindings[m_midiLearnAction] = {status, data1, existing.data2};
                    m_currentSettings.midiBindings.insert(m_midiLearnAction, qMakePair(status, data1));
                }

                if (m_midiLearnMode == LearnJogForward) {
                    m_midiBindingData2Forward[m_midiLearnAction] = data2;
                    m_currentSettings.midiBindingData2Forward.insert(m_midiLearnAction, data2);
                } else {
                    m_midiBindingData2Backward[m_midiLearnAction] = data2;
                    m_currentSettings.midiBindingData2Backward.insert(m_midiLearnAction, data2);
                }
            }

            m_settingsManager->save(m_configPath, m_currentSettings);
            m_midiLearnAction = -1;
            m_midiLearnMode = LearnControl;
            emit midiLearnActionChanged();
            m_midiBindingsVersion++;
            emit midiBindingsChanged();
            return;
        }

        QVector<int> candidates;
        for (auto it = m_midiBindings.constBegin(); it != m_midiBindings.constEnd(); ++it) {
            if (it.value().status == status && it.value().data1 == data1) {
                candidates.append(it.key());
            }
        }
        if (candidates.isEmpty()) return;

        auto matchesJogValue = [&](int action) {
            if (action != 8) return false;
            int forwardValue = m_midiBindingData2Forward.value(action, -1);
            int backwardValue = m_midiBindingData2Backward.value(action, -1);
            return (forwardValue >= 0 && data2 == forwardValue) || (backwardValue >= 0 && data2 == backwardValue);
        };

        auto matchesData2 = [&](int action) {
            const auto binding = m_midiBindings.value(action);
            return (binding.data2 >= 0 && data2 == binding.data2);
        };

        int matchedAction = -1;
        for (int action : candidates) {
            if (matchesJogValue(action)) {
                matchedAction = action;
                break;
            }
        }
        if (matchedAction < 0) {
            for (int action : candidates) {
                if (matchesData2(action)) {
                    matchedAction = action;
                    break;
                }
            }
        }
        if (matchedAction < 0) {
            matchedAction = *std::min_element(candidates.constBegin(), candidates.constEnd());
        }

        if (!m_midiLastValues.contains(matchedAction) || m_midiLastValues.value(matchedAction) != data2) {
            m_midiLastValues[matchedAction] = data2;
            m_midiLastValuesVersion++;
            emit midiLastValuesChanged();
        }

        if (matchedAction == 8) {
            int forwardValue = m_midiBindingData2Forward.value(matchedAction, -1);
            int backwardValue = m_midiBindingData2Backward.value(matchedAction, -1);

            int deltaSign = 0;
            if (forwardValue >= 0 && data2 == forwardValue) {
                deltaSign = 1;
            } else if (backwardValue >= 0 && data2 == backwardValue) {
                deltaSign = -1;
            } else {
                return;
            }
            jogStep(deltaSign);
        } else {
            dispatchControlAction(matchedAction, isRelease);
        }
    });
    connect(m_midiManager, &MidiManager::portsChanged, this, &UIManager::midiPortsChanged);
    connect(m_midiManager, &MidiManager::currentPortChanged, this, &UIManager::midiPortIndexChanged);
    connect(m_midiManager, &MidiManager::connectedChanged, this, &UIManager::midiConnectedChanged);
    connect(this, &UIManager::midiConnectedChanged, this, [this]() {
        updateXTouchDisplay();
        updateXTouchLcd();
    });
    connect(this, &UIManager::streamNamesChanged, this, [this]() {
        updateXTouchLcd();
    });
    connect(this, &UIManager::feedSelectRequested, this, [this]() {
        updateXTouchLcd();
    });
    connect(this, &UIManager::timeOfDayModeChanged, this, [this]() {
        updateXTouchDisplay();
    });
    connect(this, &UIManager::recordingStartEpochMsChanged, this, [this]() {
        updateXTouchDisplay();
    });
    connect(m_midiManager, &MidiManager::portsChanged, this, [this]() {
        if (!m_currentSettings.midiPortName.isEmpty()) {
            int idx = m_midiManager->ports().indexOf(m_currentSettings.midiPortName);
            if (idx >= 0) m_midiManager->openPort(idx);
        }
    });

    // --- Stream Deck (mirrors the MIDI manager pattern; stub on non-iOS) ---
    m_streamDeckManager = new StreamDeckManager(this);

    connect(m_streamDeckManager, &StreamDeckManager::actionTriggered, this,
            [this](int actionId, bool pressed) {
        dispatchControlAction(actionId, !pressed);
    });
    connect(m_streamDeckManager, &StreamDeckManager::rotateTriggered, this,
            [this](int actionId, int delta) {
        if (actionId == 10) shuttleStep(delta);
        else jogStep(delta);
    });

    connect(m_streamDeckManager, &StreamDeckManager::learnInput, this,
            [this](int elementType, int index) {
        if (m_streamDeckLearnAction < 0) return;
        const QString model = m_streamDeckManager->deviceModel();
        const bool ok = m_streamDeckStore.bind(
            model, m_streamDeckLearnAction,
            static_cast<StreamDeckMappingStore::ElementType>(elementType), index,
            m_streamDeckManager->keyCount(), m_streamDeckManager->dialCount());
        if (!ok) return;  // invalid pairing — keep listening
        m_streamDeckStore.writeTo(m_currentSettings);
        m_settingsManager->save(m_configPath, m_currentSettings);
        pushStreamDeckMaps();
        m_streamDeckManager->setLearnMode(false);
        m_streamDeckLearnAction = -1;
        emit streamDeckLearnActionChanged();
        m_streamDeckBindingsVersion++;
        emit streamDeckBindingsChanged();
    });
    connect(m_streamDeckManager, &StreamDeckManager::scrubTriggered, this,
            [this](double fraction) {
        cancelFollowLive();
        seekPlayback(qint64(fraction * double(recordedDurationMs())));
    });

    auto pushRecordingState = [this]() {
        m_streamDeckManager->setRecording(isRecording(),
                                          isRecording() ? recordedDurationMs() : 0);
    };
    auto pushTransportState = [this]() {
        if (!m_transport) return;
        m_streamDeckManager->setTransport(m_transport->isPlaying(),
                                          m_transport->speed(),
                                          m_followLive);
    };
    connect(this, &UIManager::recordingStatusChanged, this, pushRecordingState);
    connect(this, &UIManager::recordedDurationMsChanged, this, pushRecordingState);
    connect(m_transport, &PlaybackTransport::playingChanged, this, pushTransportState);
    connect(m_transport, &PlaybackTransport::speedChanged, this, pushTransportState);
    connect(this, &UIManager::followLiveChanged, this, pushTransportState);
    connect(m_transport, &PlaybackTransport::posChanged, this, [this](int64_t) {
        pushDeckTimecode();
        emit playbackTimecodeChanged();
        m_telemetryVersion++;
        emit telemetryChanged();
    });
    // The displayed timecode also changes when the time-of-day toggle flips or
    // the recording's wall-clock anchor is (re)established — keep both the UI
    // and the deck in lockstep on those too.
    connect(this, &UIManager::timeOfDayModeChanged, this, [this]() {
        pushDeckTimecode();
        emit playbackTimecodeChanged();
    });
    connect(this, &UIManager::recordingStartEpochMsChanged, this, [this]() {
        pushDeckTimecode();
        emit playbackTimecodeChanged();
    });
    // Snapshot push so a freshly connected deck lights up correctly even
    // while playback is paused (no posChanged ticks).
    connect(m_streamDeckManager, &StreamDeckManager::connectedChanged, this,
            [this, pushRecordingState, pushTransportState]() {
        if (m_streamDeckManager->connected()) {
            pushRecordingState();
            pushTransportState();
            pushDeckTimecode();
            // Creates the default layout for a new model, or clamps saved rows
            // to the live geometry for a known one.
            const QString model = m_streamDeckManager->deviceModel();
            m_streamDeckStore.clampToGeometry(model,
                m_streamDeckManager->keyCount(), m_streamDeckManager->dialCount());
            pushStreamDeckMaps();
            m_streamDeckBindingsVersion++;
            emit streamDeckBindingsChanged();
        } else if (m_streamDeckLearnAction >= 0) {
            // Deck unplugged mid-learn — cancel listening on BOTH sides
            // (the Swift learning flag is a persistent singleton).
            m_streamDeckManager->setLearnMode(false);
            m_streamDeckLearnAction = -1;
            emit streamDeckLearnActionChanged();
        }
    });

    m_streamDeckManager->start();

    if (auto *app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::screenAdded, this, [this](QScreen*) {
            refreshScreens();
        });
        connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
            refreshScreens();
        });
    }
    refreshScreens();
    refreshProviders();
}

UIManager::~UIManager() {
    // On app quit, ~QObject deletes our child members in creation order:
    // m_transport, m_audioPlayer, MidiManager, FrameProviders... with the
    // PlaybackWorker destroyed LAST. Its decode thread holds RAW pointers to
    // the transport / audio player / providers and keeps dereferencing them
    // until its own dtor stops it — by which point those objects are already
    // freed (use-after-free crash on quit-while-recording). Tear down in a
    // safe order BEFORE the members destruct:
    //   1. Stop + delete the worker (raw pointers into other members).
    //   2. Flush the muxer by stopping any in-progress recording.
    // PlaybackWorker::stop() is idempotent (sets m_running=false,
    // requestInterruption, wait()), so calling it before delete is safe.
    // Manually deleting the parented worker and nulling the pointer avoids a
    // double-delete: QObject removes destroyed children from its child list.
    if (m_playbackWorker) {
        m_playbackWorker->stop();
        delete m_playbackWorker;
        m_playbackWorker = nullptr;
    }
    if (m_telemetryClient) {
        m_telemetryClient->stop();
    }
    if (m_replayManager && m_replayManager->isRecording()) {
        m_replayManager->stopRecording();
    }
}

void UIManager::dispatchControlAction(int action, bool isRelease)
{
    // Case 8 (jog) intentionally absent: jog events carry a delta and go
    // through jogStep(), not this press/release dispatch.
    switch (action) {
    case 0:
        if (!isRelease) playPause();
        break;
    case 1:
        if (isRelease) {
            if (m_holdAction == 1) {
                if (m_transport) {
                    m_transport->setSpeed(1.0);
                    m_transport->setPlaying(m_holdWasPlaying);
                }
                m_holdAction = -1;
            }
        } else {
            if (m_holdAction != -1) break;  // ignore overlapping hold from any device
            cancelFollowLive();
            m_holdWasPlaying = m_transport ? m_transport->isPlaying() : false;
            m_holdAction = 1;
            if (m_transport) {
                m_transport->setSpeed(-5.0);
                m_transport->setPlaying(true);
            }
        }
        break;
    case 2:
        if (isRelease) {
            if (m_holdAction == 2) {
                if (m_transport) {
                    m_transport->setSpeed(1.0);
                    m_transport->setPlaying(m_holdWasPlaying);
                }
                m_holdAction = -1;
            }
        } else {
            if (m_holdAction != -1) break;  // ignore overlapping hold from any device
            cancelFollowLive();
            m_holdWasPlaying = m_transport ? m_transport->isPlaying() : false;
            m_holdAction = 2;
            if (m_transport) {
                m_transport->setSpeed(5.0);
                m_transport->setPlaying(true);
            }
        }
        break;
    case 3:
        if (!isRelease) stepFrame();
        break;
    case 7:
        if (!isRelease) stepFrameBack();
        break;
    case 4:
        if (!isRelease) goLive();
        break;
    case 5:
        if (!isRelease) captureCurrent();
        break;
    case 6:
        if (!isRelease) {
            setPlaybackViewState(false, -1);
            emit multiviewRequested();
        }
        break;
    case 9:
        if (!isRelease) {
            if (isRecording()) stopRecording();
            else startRecording();
        }
        break;
    default:
        if (!isRelease && action >= 100 && action < 108) {
            emit feedSelectRequested(action - 100);
        }
        break;
    }
}

void UIManager::dispatchExternalAction(int action, bool pressed) {
    dispatchControlAction(action, !pressed);
}

void UIManager::jogExternal(int delta) {
    jogStep(delta);
}

void UIManager::shuttleExternal(int delta) {
    shuttleStep(delta);
}

void UIManager::selectFeedExternal(int index) {
    emit feedSelectRequested(index);
}

void UIManager::jogStep(int delta)
{
    if (!m_transport || delta == 0) return;

    m_transport->setPlaying(false);
    cancelFollowLive();

    m_transport->step(delta);

    // Clamp forward jog to the live edge (never backward).
    if (delta > 0) {
        const int64_t liveEdge = recordedDurationMs();
        if (m_transport->currentPos() > liveEdge) {
            m_transport->seek(liveEdge);
        }
    }

    if (m_playbackWorker) {
        m_playbackWorker->seekTo(m_transport->currentPos());
    }
}

void UIManager::setFollowLive(bool on)
{
    if (m_followLive == on) return;
    m_followLive = on;
    emit followLiveChanged();
}

static int nextSourceIdSeed(const QList<SourceSettings> &sources) {
    int maxId = 0;
    for (const auto &source : sources) {
        bool ok = false;
        const int value = source.id.trimmed().toInt(&ok);
        if (ok && value > maxId) {
            maxId = value;
        }
    }
    return maxId + 1;
}

QStringList UIManager::streamUrls() const {
    QStringList urls;
    urls.reserve(m_currentSettings.sources.size());
    for (const auto &source : m_currentSettings.sources) {
        urls.append(source.url);
    }
    return urls;
}

QStringList UIManager::streamNames() const {
    QStringList names;
    names.reserve(m_currentSettings.sources.size());
    for (const auto &source : m_currentSettings.sources) {
        names.append(source.name);
    }
    return names;
}

QStringList UIManager::streamIds() const {
    QStringList ids;
    ids.reserve(m_currentSettings.sources.size());
    for (const auto &source : m_currentSettings.sources) {
        ids.append(source.id);
    }
    return ids;
}
QString UIManager::saveLocation() const { return m_currentSettings.saveLocation; }
QString UIManager::fileName() const { return m_currentSettings.fileName; }
int UIManager::recordWidth() const { return m_currentSettings.videoWidth; }
int UIManager::recordHeight() const { return m_currentSettings.videoHeight; }
int UIManager::recordFps() const {
    return FrameRate{m_currentSettings.fpsNum, m_currentSettings.fpsDen}.roundedFps();
}

QStringList UIManager::frameRatePresetLabels() const {
    QStringList out;
    for (const FrameRatePreset& p : frameRatePresets())
        out << QString::fromLatin1(p.label);
    return out;
}

int UIManager::frameRateIndex() const {
    const FrameRate cur{m_currentSettings.fpsNum, m_currentSettings.fpsDen};
    const auto& presets = frameRatePresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].rate == cur) return int(i);
    return -1; // a non-preset rate loaded from config
}

void UIManager::setFrameRateIndex(int index) {
    if (m_replayManager && m_replayManager->isRecording()) return;
    const auto& presets = frameRatePresets();
    if (index < 0 || index >= int(presets.size())) return;
    const FrameRate r = presets[size_t(index)].rate;
    if (m_currentSettings.fpsNum == r.num && m_currentSettings.fpsDen == r.den) return;
    m_currentSettings.fpsNum = r.num;
    m_currentSettings.fpsDen = r.den;
    if (m_replayManager) m_replayManager->setFrameRate(r);
    if (m_transport) m_transport->setFps(r.roundedFps());
    emit frameRateChanged();
}
int UIManager::multiviewCount() const { return m_currentSettings.multiviewCount; }
bool UIManager::isRecording() const { return m_replayManager->isRecording(); }
qint64 UIManager::recordingStartEpochMs() const {
    return m_replayManager ? m_replayManager->getRecordingStartEpochMs() : 0;
}
bool UIManager::timeOfDayMode() const {
    return m_currentSettings.showTimeOfDay;
}

QString UIManager::importSettingsUrl() const {
    return m_currentSettings.importSettingsUrl;
}

int UIManager::liveBufferMs() const {
    return m_liveBufferMs;
}

QStringList UIManager::midiPorts() const {
    return m_midiManager ? m_midiManager->ports() : QStringList();
}

int UIManager::midiPortIndex() const {
    return m_midiManager ? m_midiManager->currentPort() : -1;
}

bool UIManager::midiConnected() const {
    return m_midiManager ? m_midiManager->connected() : false;
}

int UIManager::midiLearnAction() const {
    return m_midiLearnAction;
}

QString UIManager::midiPortName() const {
    return m_currentSettings.midiPortName;
}

int UIManager::midiBindingsVersion() const {
    return m_midiBindingsVersion;
}

int UIManager::midiLastValuesVersion() const {
    return m_midiLastValuesVersion;
}

int UIManager::activeViewCount() const {
    return qBound(1, m_currentSettings.multiviewCount, 16);
}

void UIManager::ensureSourceEnabledSize() {
    while (m_sourceEnabled.size() < m_currentSettings.sources.size()) {
        m_sourceEnabled.append(true);
    }
}

void UIManager::rebuildSlotMap() {
    const int viewCount = activeViewCount();
    const int sourceCount = m_currentSettings.sources.size();
    ensureSourceEnabledSize();

    // Preserve existing assignments: a source already in a slot stays there
    QList<int> newMap(viewCount, -1);
    QSet<int> assignedSources;

    // 1. Keep sources that are still enabled and were already in a slot
    for (int v = 0; v < qMin(viewCount, m_viewSlotMap.size()); ++v) {
        int src = m_viewSlotMap[v];
        if (src >= 0 && src < sourceCount && m_sourceEnabled[src]) {
            newMap[v] = src;
            assignedSources.insert(src);
        }
    }

    // 2. Fill empty slots with enabled sources that aren't assigned yet (in order)
    int nextFreeSlot = 0;
    for (int s = 0; s < sourceCount; ++s) {
        if (!m_sourceEnabled[s] || assignedSources.contains(s)) continue;
        // Find next empty slot
        while (nextFreeSlot < viewCount && newMap[nextFreeSlot] != -1) {
            ++nextFreeSlot;
        }
        if (nextFreeSlot >= viewCount) break; // No more room
        newMap[nextFreeSlot] = s;
        assignedSources.insert(s);
        ++nextFreeSlot;
    }

    m_viewSlotMap = newMap;
}

QStringList UIManager::activeStreamUrls() const {
    QStringList urls;
    for (int v = 0; v < m_viewSlotMap.size(); ++v) {
        int src = m_viewSlotMap[v];
        if (src >= 0 && src < m_currentSettings.sources.size()) {
            urls.append(m_currentSettings.sources[src].url);
        } else {
            urls.append(""); // Empty = blue view
        }
    }
    return urls;
}

QStringList UIManager::activeStreamNames() const {
    QStringList names;
    for (int v = 0; v < m_viewSlotMap.size(); ++v) {
        int src = m_viewSlotMap[v];
        if (src >= 0 && src < m_currentSettings.sources.size()) {
            names.append(m_currentSettings.sources[src].name);
        } else {
            names.append("");
        }
    }
    return names;
}

void UIManager::syncActiveStreams() {
    rebuildSlotMap();

    QStringList urls;
    QStringList names;
    QList<QByteArray> metadata;
    QList<int> trims;
    urls.reserve(m_currentSettings.sources.size());
    names.reserve(m_currentSettings.sources.size());
    metadata.reserve(m_currentSettings.sources.size());
    trims.reserve(m_currentSettings.sources.size());
    for (const auto &source : m_currentSettings.sources) {
        urls.append(source.url);
        names.append(source.name);
        trims.append(source.trimOffsetMs);

        // Build a compact JSON blob for per-frame subtitle metadata
        QJsonObject obj;
        obj.insert("id", source.id);
        obj.insert("name", source.name);
        obj.insert("metadata", source.metadata); // QJsonArray
        metadata.append(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    // Source configuration: ALL source URLs go to the engine (one worker per source)
    m_replayManager->setSourceUrls(urls);
    m_replayManager->setSourceNames(names);
    m_replayManager->setSourceMetadata(metadata);
    m_replayManager->setSourceTrims(trims);
    updateReplayTelemetryFeeds();

    // View configuration: how many recording tracks, and their display names
    m_replayManager->setViewCount(activeViewCount());
    m_replayManager->setViewNames(activeStreamNames());

    // Pass the current view→source mapping (purely virtual)
    m_replayManager->updateViewMapping(m_viewSlotMap);

    refreshProviders();
    emit viewSlotMapChanged();
}

QVariantList UIManager::viewSlotMap() const {
    QVariantList list;
    for (int src : m_viewSlotMap) {
        list.append(src);
    }
    return list;
}

void UIManager::toggleSourceEnabled(int sourceIndex) {
    ensureSourceEnabledSize();
    if (sourceIndex < 0 || sourceIndex >= m_sourceEnabled.size()) return;

    const bool enabling = !m_sourceEnabled[sourceIndex];
    m_sourceEnabled[sourceIndex] = enabling;
    m_sourceEnabledVersion++;
    emit sourceEnabledChanged();

    if (!enabling) {
        // Toggle OFF: find this source's current slot and clear it
        for (int v = 0; v < m_viewSlotMap.size(); ++v) {
            if (m_viewSlotMap[v] == sourceIndex) {
                m_viewSlotMap[v] = -1;
                break;
            }
        }

        // Auto-fill: if there are enabled sources not currently in any
        // view, put them into the now-empty slot(s).  Example: 2 Views,
        // 4 Sources — turning Source 1 off frees View 1, so Source 3
        // (first unassigned enabled source) instantly takes its place.
        for (int v = 0; v < m_viewSlotMap.size(); ++v) {
            if (m_viewSlotMap[v] != -1) continue; // slot occupied
            for (int s = 0; s < m_currentSettings.sources.size(); ++s) {
                if (!m_sourceEnabled[s]) continue;
                bool inUse = false;
                for (int vv = 0; vv < m_viewSlotMap.size(); ++vv) {
                    if (m_viewSlotMap[vv] == s) { inUse = true; break; }
                }
                if (!inUse) {
                    m_viewSlotMap[v] = s;
                    break;
                }
            }
        }
    } else {
        // Toggle ON: put the source into the first empty slot
        for (int v = 0; v < m_viewSlotMap.size(); ++v) {
            if (m_viewSlotMap[v] == -1) {
                m_viewSlotMap[v] = sourceIndex;
                break;
            }
        }
    }

    if (m_replayManager->isRecording()) {
        // Purely virtual: just update which source writes to which view-track.
        // ZERO FFmpeg impact — no URL changes, no reconnects.
        m_replayManager->updateViewMapping(m_viewSlotMap);
        m_replayManager->setViewNames(activeStreamNames());
    } else {
        // Not recording: update pre-configured URLs/names and providers
        QStringList urls;
        QStringList names;
        QList<QByteArray> metadata;
        QList<int> trims;
        urls.reserve(m_currentSettings.sources.size());
        names.reserve(m_currentSettings.sources.size());
        metadata.reserve(m_currentSettings.sources.size());
        trims.reserve(m_currentSettings.sources.size());
        for (const auto &source : m_currentSettings.sources) {
            urls.append(source.url);
            names.append(source.name);
            trims.append(source.trimOffsetMs);
            QJsonObject obj;
            obj.insert("id", source.id);
            obj.insert("name", source.name);
            obj.insert("metadata", source.metadata);
            metadata.append(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }
        m_replayManager->setSourceUrls(urls);
        m_replayManager->setSourceNames(names);
        m_replayManager->setSourceMetadata(metadata);
        m_replayManager->setSourceTrims(trims);
        m_replayManager->setViewCount(activeViewCount());
        m_replayManager->setViewNames(activeStreamNames());
        m_replayManager->updateViewMapping(m_viewSlotMap);
        refreshProviders();
    }

    emit viewSlotMapChanged();
}

bool UIManager::isSourceEnabled(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= m_sourceEnabled.size()) return true;
    return m_sourceEnabled[sourceIndex];
}

bool UIManager::isSourceConnected(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= m_sourceConnected.size()) return false;
    return m_sourceConnected[sourceIndex];
}

bool UIManager::hasDuplicateUrl(int sourceIndex) const {
    const QList<SourceSettings>& sources = m_currentSettings.sources;
    if (sourceIndex < 0 || sourceIndex >= sources.size()) return false;
    const QString mine = sources[sourceIndex].url.trimmed();
    if (mine.isEmpty()) return false; // empty = "no source", never a clash
    for (int i = 0; i < sources.size(); ++i) {
        if (i == sourceIndex) continue;
        if (sources[i].url.trimmed() == mine) return true;
    }
    return false;
}

void UIManager::resetSourceConnection() {
    if (m_sourceConnected.isEmpty()) return;
    m_sourceConnected.fill(false);
    m_sourceConnectionVersion++;
    emit sourceConnectionChanged();
    resetSourceStats(int(m_sourceConnected.size()));
}

void UIManager::updateReplayTelemetryFeeds() {
    if (!m_replayManager) return;

    QStringList feedIds;
    QStringList feedNames;
    QList<int> telemetryDelaysMs;
    feedIds.reserve(m_currentSettings.sources.size());
    feedNames.reserve(m_currentSettings.sources.size());
    telemetryDelaysMs.reserve(m_currentSettings.sources.size());
    for (const SourceSettings &source : m_currentSettings.sources) {
        feedIds.append(source.id);
        feedNames.append(source.name);
        telemetryDelaysMs.append(source.telemetryDelayMs);
    }
    m_replayManager->setTelemetryFeeds(feedIds, feedNames, telemetryDelaysMs);
}

void UIManager::clearImportPreview() {
    m_pendingImport = ProjectSettingsImportResult{};
    m_hasPendingImport = false;
    m_importPreview.clear();
    m_importPreviewError.clear();
}

bool UIManager::loadTelemetryTimeline(const QString &filePath, bool notify) {
    m_hasTelemetryTimeline = false;
    if (filePath.trimmed().isEmpty()) {
        return false;
    }

    if (!m_telemetryTimelineReader.load(filePath)) {
        if (!notify) {
            return false;
        }
        qWarning() << "UIManager: failed to load telemetry timeline"
                   << filePath << m_telemetryTimelineReader.lastError();
        return false;
    }

    m_hasTelemetryTimeline = true;
    if (notify) {
        m_telemetryVersion++;
        emit telemetryChanged();
    }
    return true;
}

QVariantMap UIManager::recordingTelemetryStateAt(qint64 playheadMs) const {
    QVariantMap state;
    for (auto it = m_recordingTelemetry.constBegin(); it != m_recordingTelemetry.constEnd(); ++it) {
        const QList<TelemetryTimelineEntry> &entries = it.value();
        const TelemetryTimelineEntry *latest = nullptr;
        for (const TelemetryTimelineEntry &entry : entries) {
            if (entry.ptsMs > playheadMs) {
                break;
            }
            latest = &entry;
        }
        if (latest) {
            state.insert(it.key(), latest->payload);
        }
    }
    return state;
}

void UIManager::onSourceConnectionChanged(int sourceIndex, bool connected) {
    if (sourceIndex < 0) return;
    while (m_sourceConnected.size() <= sourceIndex)
        m_sourceConnected.append(false);
    if (m_sourceConnected[sourceIndex] == connected) return; // no UI churn
    m_sourceConnected[sourceIndex] = connected;
    if (connected) {
        // Re-baseline SRT stats on a real disconnect->connect transition: the new
        // socket restarts its cumulative counters from 0. Below the debounce guard
        // so a redundant connected=true can never wipe a healthy source's baseline.
        if (sourceIndex < int(m_sourceStats.size())) {
            m_sourceStats[sourceIndex].seen = false;
            m_sourceStats[sourceIndex].health = int(SrtHealth::NA);
        }
    }
    m_sourceConnectionVersion++;
    emit sourceConnectionChanged();
}

void UIManager::onSourceStatsUpdated(int sourceIndex, SrtStats stats) {
    if (sourceIndex < 0) return;
    if (int(m_sourceStats.size()) <= sourceIndex) m_sourceStats.resize(sourceIndex + 1);
    SrtStatsEntry& e = m_sourceStats[sourceIndex];
    if (!e.seen) {
        // First snapshot since (re)connect: establish the baseline, render Green.
        e.seen = true;
        e.health = int(SrtHealth::Green);
    } else {
        e.health = int(srtHealth(e.last, stats, m_srtAmberPct));
    }
    e.last = stats;
    m_sourceStatsVersion++;
    emit sourceStatsChanged();
}

int UIManager::sourceLinkHealth(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= int(m_sourceStats.size())) return int(SrtHealth::NA);
    return m_sourceStats[sourceIndex].health;
}

bool UIManager::sourceHasSrtStats(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= int(m_sourceStats.size())) return false;
    return m_sourceStats[sourceIndex].seen;
}

QString UIManager::sourceStatsTooltip(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= int(m_sourceStats.size()) ||
        !m_sourceStats[sourceIndex].seen) {
        return QString();
    }
    const SrtStats& s = m_sourceStats[sourceIndex].last;
    const QLocale loc;
    QString pct = QStringLiteral("0.0");
    if (s.recvTotal > 0)
        pct = QString::number(100.0 * double(s.retransTotal) / double(s.recvTotal), 'f', 1);
    return QStringLiteral("SRT link\nrecv      %1\nretrans   %2  (%3%)\nloss det  %4\ndropped   %5")
        .arg(loc.toString(qlonglong(s.recvTotal)), loc.toString(qlonglong(s.retransTotal)), pct,
             loc.toString(qlonglong(s.lossTotal)), loc.toString(qlonglong(s.dropTotal)));
}

void UIManager::resetSourceStats(int count) {
    m_sourceStats.assign(count < 0 ? 0 : count, SrtStatsEntry{});
    m_sourceStatsVersion++;
    emit sourceStatsChanged();
}

void UIManager::setStreamUrls(const QStringList &urls) {
    if (streamUrls() != urls) {
        QList<SourceSettings> updated = m_currentSettings.sources;
        const int minSize = qMin(updated.size(), urls.size());
        for (int i = 0; i < minSize; ++i) {
            updated[i].url = urls[i];
        }
        if (urls.size() > updated.size()) {
            int nextId = nextSourceIdSeed(updated);
            for (int i = updated.size(); i < urls.size(); ++i) {
                SourceSettings source;
                source.id = QString::number(nextId++);
                source.name = "";
                source.url = urls[i];
                updated.append(source);
            }
        } else if (urls.size() < updated.size()) {
            updated = updated.mid(0, urls.size());
        }
        m_currentSettings.sources = updated;
        syncActiveStreams();
        if (m_replayManager->isRecording()) {
            restartPlaybackWorker();
        }
        emit streamUrlsChanged();
        emit streamNamesChanged();
        emit streamIdsChanged();
    }
}

void UIManager::setStreamNames(const QStringList &names) {
    if (streamNames() != names) {
        QList<SourceSettings> updated = m_currentSettings.sources;
        const int minSize = qMin(updated.size(), names.size());
        for (int i = 0; i < minSize; ++i) {
            updated[i].name = names[i];
        }
        if (names.size() > updated.size()) {
            int nextId = nextSourceIdSeed(updated);
            for (int i = updated.size(); i < names.size(); ++i) {
                SourceSettings source;
                source.id = QString::number(nextId++);
                source.name = names[i];
                source.url = "";
                updated.append(source);
            }
        } else if (names.size() < updated.size()) {
            updated = updated.mid(0, names.size());
        }
        m_currentSettings.sources = updated;
        QStringList sourceNames = streamNames();
        m_replayManager->setSourceNames(sourceNames);
        m_replayManager->setViewNames(activeStreamNames());
        updateReplayTelemetryFeeds();
        emit streamNamesChanged();
        emit streamUrlsChanged();
        emit streamIdsChanged();
    }
}

void UIManager::setStreamIds(const QStringList &ids) {
    if (streamIds() != ids) {
        QList<SourceSettings> updated = m_currentSettings.sources;
        const int minSize = qMin(updated.size(), ids.size());
        for (int i = 0; i < minSize; ++i) {
            updated[i].id = ids[i];
        }
        if (ids.size() > updated.size()) {
            for (int i = updated.size(); i < ids.size(); ++i) {
                SourceSettings source;
                source.id = ids[i];
                source.name = "";
                source.url = "";
                updated.append(source);
            }
        } else if (ids.size() < updated.size()) {
            updated = updated.mid(0, ids.size());
        }
        m_currentSettings.sources = updated;
        updateReplayTelemetryFeeds();
        emit streamIdsChanged();
        emit streamUrlsChanged();
        emit streamNamesChanged();
    }
}

void UIManager::setSaveLocationFromUrl(const QUrl &folderUrl) {
    QString localPath = folderUrl.toLocalFile();
    if (!localPath.isEmpty()) {
        setSaveLocation(localPath);
    }
}

void UIManager::setSaveLocation(const QString &path) {
    // Normalize free-text input: expand ~, clean separators, and refuse
    // relative paths (they would resolve against the process cwd).
    QString normalized = path.trimmed();
    if (normalized == QStringLiteral("~")) {
        normalized = QDir::homePath();
    } else if (normalized.startsWith(QStringLiteral("~/"))) {
        normalized = QDir::homePath() + normalized.mid(1);
    }
    if (!normalized.isEmpty()) {
        normalized = QDir::cleanPath(normalized);
        if (QDir::isRelativePath(normalized)) {
            qWarning() << "UIManager: ignoring relative save location" << path;
            return;
        }
    }
    if (m_currentSettings.saveLocation != normalized) {
        m_currentSettings.saveLocation = normalized;
        m_replayManager->setOutputDirectory(normalized);
        emit saveLocationChanged();
    }
}

void UIManager::setFileName(const QString &name) {
    if (m_currentSettings.fileName != name) {
        m_currentSettings.fileName = name;
        m_replayManager->setBaseFileName(name);
        emit fileNameChanged();
    }
}

void UIManager::setRecordWidth(int width) {
    if (width <= 0) return;
    if (m_currentSettings.videoWidth != width) {
        m_currentSettings.videoWidth = width;
        m_replayManager->setVideoWidth(width);
        emit recordWidthChanged();
    }
}

void UIManager::setRecordHeight(int height) {
    if (height <= 0) return;
    if (m_currentSettings.videoHeight != height) {
        m_currentSettings.videoHeight = height;
        m_replayManager->setVideoHeight(height);
        emit recordHeightChanged();
    }
}

void UIManager::setRecordFps(int fps) {
    if (fps <= 0) return;
    // Changing fps mid-recording desyncs the workers (frozen at their
    // construction-time fps) from the heartbeat: lowering freezes all output,
    // raising corrupts the timeline. Refuse while recording; the QML SpinBox
    // also disables, but guard the engine in case that's bypassed.
    if (m_replayManager && m_replayManager->isRecording()) return;
    // The remote control API speaks integer fps; map it onto a 1/1 rational.
    if (m_currentSettings.fpsNum != fps || m_currentSettings.fpsDen != 1) {
        m_currentSettings.fpsNum = fps;
        m_currentSettings.fpsDen = 1;
        m_replayManager->setFps(fps);
        if (m_transport) {
            m_transport->setFps(fps);
        }
        emit frameRateChanged();
    }
}

int UIManager::audioOutputLatencyMs() const {
    return m_currentSettings.audioOutputLatencyMs;
}

void UIManager::setAudioOutputLatencyMs(int ms) {
    const int clamped = qBound(0, ms, 500); // keep in sync with AudioPlayer::kMaxOutputLatencyMs
    if (m_currentSettings.audioOutputLatencyMs == clamped) return;
    m_currentSettings.audioOutputLatencyMs = clamped;
    if (m_audioPlayer) {
        m_audioPlayer->setOutputLatencyOffsetMs(clamped);
        // A runtime offset change only re-positions an already-aligned stream via a
        // re-align; clear() drops alignment so the next push adopts the new latency
        // (with a de-click fade). Without this the change is inaudible until a seek.
        m_audioPlayer->clear();
    }
    emit audioOutputLatencyChanged();
    m_settingsManager->save(m_configPath, m_currentSettings);
}

void UIManager::setMultiviewCount(int count) {
    // The muxer's stream layout is frozen at startRecording; changing the view
    // count mid-recording flows through syncActiveStreams -> setViewCount and
    // makes a raised count write video packets into audio/subtitle tracks.
    // The QML SpinBox already disables while recording — guard the engine too.
    if (m_replayManager && m_replayManager->isRecording()) return;
    const int clamped = qBound(1, count, 16);
    if (m_currentSettings.multiviewCount != clamped) {
        m_currentSettings.multiviewCount = clamped;
        syncActiveStreams();
        if (m_replayManager->isRecording()) {
            restartPlaybackWorker();
        }
        emit multiviewCountChanged();
        emit viewSlotMapChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::setTimeOfDayMode(bool enabled) {
    if (m_currentSettings.showTimeOfDay == enabled) return;
    m_currentSettings.showTimeOfDay = enabled;
    emit timeOfDayModeChanged();
    m_settingsManager->save(m_configPath, m_currentSettings);
}

void UIManager::setImportSettingsUrl(const QString &url) {
    const QString trimmed = url.trimmed();
    if (m_currentSettings.importSettingsUrl == trimmed) return;

    m_currentSettings.importSettingsUrl = trimmed;
    if (m_importClient) {
        m_importClient->cancel();
    }
    if (m_hasPendingImport || !m_importPreview.isEmpty() || !m_importPreviewError.isEmpty()) {
        clearImportPreview();
        emit importPreviewChanged();
    }
    emit importSettingsUrlChanged();
}

void UIManager::setMidiPortIndex(int index) {
    if (!m_midiManager) return;
    if (index < 0) {
        m_midiManager->closePort();
        return;
    }
    m_midiManager->openPort(index);
    if (index >= 0 && index < m_midiManager->ports().size()) {
        m_currentSettings.midiPortName = m_midiManager->ports().at(index);
        emit midiPortNameChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::beginMidiLearn(int action) {
    if (m_midiLearnAction == action) return;
    m_midiLearnAction = action;
    m_midiLearnMode = LearnControl;
    emit midiLearnActionChanged();
}

void UIManager::beginMidiLearnJogForward(int action) {
    if (m_midiLearnAction == action && m_midiLearnMode == LearnJogForward) return;
    m_midiLearnAction = action;
    m_midiLearnMode = LearnJogForward;
    emit midiLearnActionChanged();
}

void UIManager::beginMidiLearnJogBackward(int action) {
    if (m_midiLearnAction == action && m_midiLearnMode == LearnJogBackward) return;
    m_midiLearnAction = action;
    m_midiLearnMode = LearnJogBackward;
    emit midiLearnActionChanged();
}

void UIManager::clearMidiBinding(int action) {
    m_midiBindings.remove(action);
    m_currentSettings.midiBindings.remove(action);
    m_currentSettings.midiBindingData2.remove(action);
    m_midiBindingData2Forward.remove(action);
    m_midiBindingData2Backward.remove(action);
    m_currentSettings.midiBindingData2Forward.remove(action);
    m_currentSettings.midiBindingData2Backward.remove(action);
    m_midiLastValues.remove(action);
    m_settingsManager->save(m_configPath, m_currentSettings);
    m_midiBindingsVersion++;
    emit midiBindingsChanged();
}

QString UIManager::midiBindingLabel(int action) const {
    if (!m_midiBindings.contains(action)) return QString("Unassigned");
    const auto binding = m_midiBindings.value(action);
    if (binding.status < 0 || binding.data1 < 0) return QString("Unassigned");
    QString base = QString("0x%1 0x%2").arg(binding.status, 2, 16, QChar('0')).arg(binding.data1, 2, 16, QChar('0')).toUpper();
    if (binding.data2 >= 0) {
        base += QString(" (0x%1)").arg(binding.data2, 2, 16, QChar('0')).toUpper();
    }
    if (m_midiBindingData2Forward.contains(action) || m_midiBindingData2Backward.contains(action)) {
        if (m_midiBindingData2Forward.contains(action)) {
            base += QString(" F:0x%1").arg(m_midiBindingData2Forward.value(action), 2, 16, QChar('0')).toUpper();
        }
        if (m_midiBindingData2Backward.contains(action)) {
            base += QString(" B:0x%1").arg(m_midiBindingData2Backward.value(action), 2, 16, QChar('0')).toUpper();
        }
    }
    return base;
}

int UIManager::midiLastValue(int action) const {
    return m_midiLastValues.value(action, -1);
}

void UIManager::playPause() {
    if (!m_transport) return;
    cancelFollowLive();
    const bool willPlay = !m_transport->isPlaying();
    // A shuttle dial can pause at speed 0; resuming must not "play" frozen.
    if (willPlay && qAbs(m_transport->speed()) < 0.01) {
        m_transport->setSpeed(1.0);
    }
    m_transport->setPlaying(willPlay);
}

void UIManager::rewind5x() {
    if (!m_transport) return;
    cancelFollowLive();
    m_transport->setSpeed(-5.0);
    m_transport->setPlaying(true);
}

void UIManager::forward5x() {
    if (!m_transport) return;
    cancelFollowLive();
    m_transport->setSpeed(5.0);
    m_transport->setPlaying(true);
}

void UIManager::stepFrame() {
    if (!m_transport) return;
    m_transport->step(1);
    m_transport->setPlaying(false);
    cancelFollowLive();

    // Clamp the upper bound to the live edge: the transport only clamps >= 0,
    // so stepping forward past the last recorded frame would walk the hidden
    // position arbitrarily far ahead (controls look dead, audio goes silent).
    const int64_t liveEdge = recordedDurationMs();
    if (m_transport->currentPos() > liveEdge) {
        m_transport->seek(liveEdge);
    }

    if (m_playbackWorker) {
        int64_t targetMs = m_transport->currentPos();
        m_playbackWorker->seekTo(targetMs);
    }
}

void UIManager::stepFrameBack() {
    if (!m_transport) return;
    m_transport->step(-1);
    m_transport->setPlaying(false);
    cancelFollowLive();

    if (m_playbackWorker) {
        int64_t targetMs = m_transport->currentPos();
        m_playbackWorker->seekTo(targetMs);
    }
}

void UIManager::goLive() {
    if (!m_transport) return;
    m_transport->setSpeed(1.0);
    m_transport->setPlaying(true);
    scrubToLive();
}

void UIManager::cancelFollowLive() {
    setFollowLive(false);
}

void UIManager::captureCurrent() {
    captureSnapshot(m_playbackSingleView, m_playbackSelectedIndex, scrubPosition());
}

void UIManager::requestNewWindowScene() {
#if defined(Q_OS_IOS)
    requestIosNewScene();
#endif
}

void UIManager::setPlaybackViewState(bool singleView, int selectedIndex) {
    const bool changed =
        (m_playbackSingleView != singleView) || (m_playbackSelectedIndex != selectedIndex);
    m_playbackSingleView = singleView;
    m_playbackSelectedIndex = selectedIndex;

    // Route audio for the selected track (or mute in multiview)
    if (m_playbackWorker) {
        m_playbackWorker->setActiveAudioView(singleView ? selectedIndex : -1);
    }
    if (m_audioPlayer) {
        bool normalSpeed = m_transport && (m_transport->speed() > 0.99 && m_transport->speed() < 1.01);
        m_audioPlayer->setMuted(!singleView || selectedIndex < 0 || !normalSpeed);
    }

    updateXTouchLcd();
    if (changed) {
        emit playbackViewStateChanged();
    }
}

void UIManager::openStreams() {
    refreshProviders();
    if (m_replayManager->isRecording()) {
        restartPlaybackWorker();
    }
}

void UIManager::refreshMidiPorts() {
    if (m_midiManager) m_midiManager->refreshPorts();
}

void UIManager::startRecording() {
    // Distinguish the cheap "no sources" cause up front so the surfaced
    // message is actionable; otherwise it's a muxer/encoder init failure.
    const bool hadSources = !m_replayManager->getSourceUrls().isEmpty();
    m_recordingTelemetry.clear();
    m_hasTelemetryTimeline = false;

    m_replayManager->startRecording();
    if (!m_replayManager->isRecording()) {
        const QString reason =
            hadSources
                ? QStringLiteral("Failed to start recording (could not initialize output file)")
                : QStringLiteral("Failed to start recording (no input sources configured)");
        qWarning() << "UIManager:" << reason << "; not launching playback";
        emit recordingFailed(reason);
        return;
    }

    if (m_telemetryClient) {
        m_telemetryClient->stop();
    }
    m_liveTelemetry.clear();
    m_telemetryVersion++;
    emit telemetryChanged();

    const QString telemetryUrl = m_currentSettings.telemetrySseUrl.trimmed();
    if (!telemetryUrl.isEmpty() && m_telemetryClient) {
        m_telemetryClient->start(QUrl(telemetryUrl));
    }

    setFollowLive(true);

    // Start every source in the "not connected" state; the workers report
    // their real state as each feed comes up. Sizing to the source count
    // makes the reset emit so the indicators repaint on a re-start.
    m_sourceConnected = QList<bool>(m_replayManager->getSourceUrls().size(), false);
    m_sourceConnectionVersion++;
    emit sourceConnectionChanged();
    resetSourceStats(m_replayManager->getSourceUrls().size());

    // 1. Initialize the Playback Worker with our providers
    if (m_playbackWorker) {
        m_playbackWorker->stop();
        delete m_playbackWorker;
    }

    m_playbackWorker = new PlaybackWorker(m_providers, m_transport, m_audioPlayer, this);

    // 2. Point it to the file being recorded
    //QString filePath = m_replayManager->getOutputDirectory() + "/" + m_replayManager->getBaseFileName() + ".mkv";
    m_playbackWorker->openFile(m_replayManager->getVideoPath());

    m_playbackWorker->start();
    m_transport->seek(0);
    m_transport->setPlaying(true);

    emit recordingStatusChanged();
    emit recordingStarted();
    emit recordingStartEpochMsChanged();
}

void UIManager::restartPlaybackWorker() {
    if (m_playbackWorker) {
        m_playbackWorker->stop();
        delete m_playbackWorker;
        m_playbackWorker = nullptr;
    }

    m_playbackWorker = new PlaybackWorker(m_providers, m_transport, m_audioPlayer, this);
    m_playbackWorker->openFile(m_replayManager->getVideoPath());
    m_playbackWorker->start();
    m_transport->seek(0);
    m_transport->setPlaying(true);
    setFollowLive(true);
}

void UIManager::stopRecording() {
    if (m_telemetryClient) {
        m_telemetryClient->stop();
    }

    const QString recordingPath = m_replayManager->getVideoPath();
    m_replayManager->stopRecording();
    loadTelemetryTimeline(recordingPath);
    m_transport->setPlaying(false);
    setFollowLive(false);
    if (m_playbackWorker) {
        m_playbackWorker->stop();
    }

    // Workers are torn down above; any queued connectionChanged is dropped
    // with them, so clear the state ourselves to avoid a stale "connected".
    resetSourceConnection();

    emit recordingStatusChanged();
    emit recordingStopped();
    emit recordingStartEpochMsChanged();
}

void UIManager::seekPlayback(int64_t ms) {
    if (m_transport) {
        m_transport->seek(ms);
        // Manual seek disables live-follow; user can re-enable via "Live"
        setFollowLive(false);
    }
    // Route the scrub through the scheduler so it classifies the seek
    // (reuse fast-path vs. full reposition) instead of showing a stale frame.
    if (m_playbackWorker) m_playbackWorker->seekTo(ms);
    if (m_audioPlayer) m_audioPlayer->clear();
}

void UIManager::updateUrl(int index, const QString &url) {
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources[index].url = url;
        // Update the source worker directly (real FFmpeg reconnect)
        m_replayManager->updateSourceUrl(index, url);
        emit streamUrlsChanged();

        // Auto-save to JSON
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

int UIManager::sourceTrimOffset(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= m_currentSettings.sources.size()) return 0;
    return m_currentSettings.sources[sourceIndex].trimOffsetMs;
}

void UIManager::setSourceTrimOffset(int sourceIndex, int ms) {
    if (sourceIndex < 0 || sourceIndex >= m_currentSettings.sources.size()) return;
    const int clamped = qBound(
        -500, ms, 500); // keep in sync with StreamWorker::kMaxTrimMs + Main.qml SpinBox range
    if (m_currentSettings.sources[sourceIndex].trimOffsetMs == clamped) return;
    m_currentSettings.sources[sourceIndex].trimOffsetMs = clamped;
    m_replayManager->updateSourceTrim(sourceIndex, clamped); // live (no-op if not recording)
    m_sourceTrimVersion++;
    emit sourceTrimChanged();
    m_settingsManager->save(m_configPath, m_currentSettings);
}

void UIManager::updateStreamName(int index, const QString& name) {
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources[index].name = name;
        rebuildSlotMap();
        m_replayManager->setSourceNames(streamNames());
        m_replayManager->setViewNames(activeStreamNames());
        updateReplayTelemetryFeeds();
        emit streamNamesChanged();
        emit viewSlotMapChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::updateStreamId(int index, const QString& id) {
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources[index].id = id;
        updateReplayTelemetryFeeds();
        emit streamIdsChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

QString UIManager::sourceDisplayLabel(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= m_currentSettings.sources.size())
        return QString();

    const SourceSettings &src = m_currentSettings.sources[sourceIndex];

    // Build "ID Name" header
    QString label = src.id;
    if (!src.name.trimmed().isEmpty()) {
        label += " " + src.name.trimmed();
    }

    // Build a lookup of stored values for this source
    QHash<QString, QString> valueMap;
    for (const QJsonValue &val : src.metadata) {
        const QJsonObject obj = val.toObject();
        valueMap.insert(obj.value("name").toString(), obj.value("value").toString());
    }

    // Append metadata fields with display=true
    for (const QJsonValue &val : m_currentSettings.metadataFields) {
        const QJsonObject fieldDef = val.toObject();
        const bool display = fieldDef.contains("display") ? fieldDef.value("display").toBool() : true;
        if (!display) continue;
        const QString fieldName = fieldDef.value("name").toString();
        const QString fieldValue = valueMap.value(fieldName);
        if (!fieldValue.trimmed().isEmpty()) {
            label += "    " + fieldName + ": " + fieldValue;
        }
    }

    return label;
}

QVariantList UIManager::metadataFieldDefinitions() const {
    QVariantList items;
    const QJsonArray &arr = m_currentSettings.metadataFields;
    for (const QJsonValue &val : arr) {
        const QJsonObject obj = val.toObject();
        QVariantMap row;
        row.insert("name", obj.value("name").toString());
        row.insert("display", obj.contains("display") ? obj.value("display").toBool() : true);
        items.append(row);
    }
    return items;
}

void UIManager::setMetadataFieldDefinitions(const QVariantList &fields) {
    QJsonArray arr;
    for (const auto &item : fields) {
        const QVariantMap row = item.toMap();
        const QString name = row.value("name").toString().trimmed();
        if (name.isEmpty()) continue;
        QJsonObject obj;
        obj.insert("name", name);
        const bool display = row.value("display", true).toBool();
        if (!display) {
            obj.insert("display", false);
        }
        arr.append(obj);
    }
    m_currentSettings.metadataFields = arr;
    emit metadataFieldsChanged();
    emit viewSlotMapChanged();
    saveSettings();
}

QVariantList UIManager::sourceMetadataItems(int index) const {
    QVariantList items;
    if (index < 0 || index >= m_currentSettings.sources.size()) return items;

    // Build a lookup from the source's stored metadata values
    QHash<QString, QString> valueMap;
    const QJsonArray &arr = m_currentSettings.sources[index].metadata;
    for (const QJsonValue &val : arr) {
        const QJsonObject obj = val.toObject();
        valueMap.insert(obj.value("name").toString(), obj.value("value").toString());
    }

    // Return one row per globally-defined field, merged with any stored value
    const QJsonArray &fields = m_currentSettings.metadataFields;
    for (const QJsonValue &val : fields) {
        const QJsonObject fieldDef = val.toObject();
        const QString fieldName = fieldDef.value("name").toString();
        QVariantMap row;
        row.insert("name", fieldName);
        row.insert("value", valueMap.value(fieldName, ""));
        items.append(row);
    }
    return items;
}

void UIManager::setSourceMetadataItems(int index, const QVariantList &items) {
    if (index < 0 || index >= m_currentSettings.sources.size()) return;
    QJsonArray arr;
    for (const auto &item : items) {
        const QVariantMap row = item.toMap();
        const QString name = row.value("name").toString().trimmed();
        if (name.isEmpty()) continue;
        const QString value = row.value("value").toString();
        if (value.isEmpty()) continue; // Only store non-empty values
        QJsonObject obj;
        obj.insert("name", name);
        obj.insert("value", value);
        arr.append(obj);
    }
    m_currentSettings.sources[index].metadata = arr;
    emit sourceMetadataChanged();
    emit viewSlotMapChanged();
    saveSettings();
}

void UIManager::readImportSettings() {
    clearImportPreview();
    emit importPreviewChanged();

    const QString url = m_currentSettings.importSettingsUrl.trimmed();
    if (url.isEmpty()) {
        m_importPreviewError = QStringLiteral("Import settings URL is empty");
        emit importPreviewChanged();
        return;
    }

    m_importClient->fetch(QUrl(url));
}

void UIManager::applyImportPreview() {
    if (!m_hasPendingImport || !m_pendingImport.ok) return;
    if (m_replayManager && m_replayManager->isRecording()) return;

    const QString previousImportUrl = m_currentSettings.importSettingsUrl;
    m_currentSettings.sources = m_pendingImport.sources;
    m_currentSettings.metadataFields = m_pendingImport.metadataFields;
    m_currentSettings.importSettingsUrl = m_pendingImport.importSettingsUrl;
    m_currentSettings.telemetrySseUrl = m_pendingImport.telemetrySseUrl;

    m_sourceEnabled = QList<bool>(m_currentSettings.sources.size(), true);
    m_sourceEnabledVersion++;
    m_sourceConnected = QList<bool>(m_currentSettings.sources.size(), false);
    m_sourceConnectionVersion++;
    resetSourceStats(m_currentSettings.sources.size());
    m_sourceTrimVersion++;
    m_liveTelemetry.clear();
    m_recordingTelemetry.clear();
    m_hasTelemetryTimeline = false;
    m_telemetryVersion++;

    syncActiveStreams();
    m_settingsManager->save(m_configPath, m_currentSettings);

    clearImportPreview();

    emit streamUrlsChanged();
    emit streamNamesChanged();
    emit streamIdsChanged();
    if (m_currentSettings.importSettingsUrl != previousImportUrl) {
        emit importSettingsUrlChanged();
    }
    emit telemetryConfigChanged();
    emit sourceEnabledChanged();
    emit sourceConnectionChanged();
    emit sourceTrimChanged();
    emit telemetryChanged();
    emit importPreviewChanged();
}

QVariantMap UIManager::telemetryAtPlayhead() {
    const qint64 playheadMs = scrubPosition();
    if (m_replayManager && m_replayManager->isRecording()) {
        QVariantMap state = m_hasTelemetryTimeline
            ? m_telemetryTimelineReader.stateAt(playheadMs)
            : QVariantMap{};
        const QVariantMap pendingState = recordingTelemetryStateAt(playheadMs);
        for (auto it = pendingState.constBegin(); it != pendingState.constEnd(); ++it) {
            state.insert(it.key(), it.value());
        }
        if (!state.isEmpty()) {
            return state;
        }
    }
    if (m_hasTelemetryTimeline) {
        return m_telemetryTimelineReader.stateAt(playheadMs);
    }
    if (!m_recordingTelemetry.isEmpty()) {
        return recordingTelemetryStateAt(playheadMs);
    }
    return m_liveTelemetry;
}

QVariantList UIManager::telemetryRowsAtPlayhead() {
    const QVariantMap state = telemetryAtPlayhead();
    QVariantList rows;
    QSet<QString> emitted;

    auto appendRow = [&rows, &emitted, &state](
                         const QString &feedId,
                         const QString &feedName,
                         const QJsonArray &metadata = {}) {
        const QVariant value = state.value(feedId);
        const QVariantMap payload = value.isValid() ? value.toMap() : QVariantMap{};
        QVariantList items;
        QStringList summaryParts;
        for (auto it = payload.constBegin(); it != payload.constEnd(); ++it) {
            const QString key = it.key();
            if (key == QStringLiteral("feedId")) continue;
            const QString displayValue = telemetryValueToString(it.value());

            QVariantMap item;
            item.insert(QStringLiteral("name"), key);
            item.insert(QStringLiteral("value"), displayValue);
            items.append(item);
            if (summaryParts.size() < 6 && !displayValue.isEmpty()) {
                summaryParts.append(key + QStringLiteral("=") + displayValue);
            }
        }
        const QString staticSummary = metadataSummary(metadata);
        if (summaryParts.isEmpty()) {
            summaryParts.append(QStringLiteral("No telemetry at playhead"));
        }
        if (!staticSummary.isEmpty()) {
            summaryParts.append(QStringLiteral("metadata: ") + staticSummary);
        }

        QVariantMap row;
        row.insert(QStringLiteral("feedId"), feedId);
        row.insert(QStringLiteral("feedName"), feedName);
        row.insert(QStringLiteral("items"), items);
        row.insert(QStringLiteral("summary"), summaryParts.join(QStringLiteral("  ")));
        rows.append(row);
        emitted.insert(feedId);
    };

    for (const SourceSettings &source : m_currentSettings.sources) {
        appendRow(source.id, source.name, source.metadata);
    }

    for (auto it = state.constBegin(); it != state.constEnd(); ++it) {
        if (!emitted.contains(it.key())) {
            appendRow(it.key(), QString());
        }
    }

    return rows;
}

void UIManager::loadSettings() {
    if (m_settingsManager->load(m_configPath, m_currentSettings)) {
        m_streamDeckStore.loadFrom(m_currentSettings);
        int nextId = nextSourceIdSeed(m_currentSettings.sources);
        for (auto &source : m_currentSettings.sources) {
            if (source.id.trimmed().isEmpty()) {
                source.id = QString::number(nextId++);
            }
        }
        // Apply to engine
        m_replayManager->setOutputDirectory(m_currentSettings.saveLocation);
        m_replayManager->setBaseFileName(m_currentSettings.fileName);
        m_replayManager->setVideoWidth(m_currentSettings.videoWidth);
        m_replayManager->setVideoHeight(m_currentSettings.videoHeight);
        m_replayManager->setFrameRate(
            FrameRate{m_currentSettings.fpsNum, m_currentSettings.fpsDen});
        m_currentSettings.multiviewCount = qBound(1, m_currentSettings.multiviewCount, 16);
        ensureSourceEnabledSize();
        syncActiveStreams();
        if (m_transport) {
            m_transport->setFps(
                FrameRate{m_currentSettings.fpsNum, m_currentSettings.fpsDen}.roundedFps());
        }

        m_midiBindings.clear();
        m_midiBindingData2Forward.clear();
        m_midiBindingData2Backward.clear();
        for (auto it = m_currentSettings.midiBindings.constBegin(); it != m_currentSettings.midiBindings.constEnd(); ++it) {
            int data2 = m_currentSettings.midiBindingData2.value(it.key(), -1);
            m_midiBindings.insert(it.key(), {it.value().first, it.value().second, data2});
        }
        for (auto it = m_currentSettings.midiBindingData2Forward.constBegin(); it != m_currentSettings.midiBindingData2Forward.constEnd(); ++it) {
            m_midiBindingData2Forward.insert(it.key(), it.value());
        }
        for (auto it = m_currentSettings.midiBindingData2Backward.constBegin(); it != m_currentSettings.midiBindingData2Backward.constEnd(); ++it) {
            m_midiBindingData2Backward.insert(it.key(), it.value());
        }
        m_midiBindingsVersion++;
        emit midiBindingsChanged();

        if (m_midiManager && !m_currentSettings.midiPortName.isEmpty()) {
            int idx = m_midiManager->ports().indexOf(m_currentSettings.midiPortName);
            if (idx >= 0) m_midiManager->openPort(idx);
        }

        refreshProviders();

        // Sync QML
        emit streamUrlsChanged();
        emit streamNamesChanged();
        emit streamIdsChanged();
        emit saveLocationChanged();
        emit fileNameChanged();
        emit recordWidthChanged();
        emit recordHeightChanged();
        emit frameRateChanged();
        emit multiviewCountChanged();
        emit timeOfDayModeChanged();
        emit importSettingsUrlChanged();
        emit telemetryConfigChanged();
        emit midiPortNameChanged();
        emit viewSlotMapChanged();
        emit sourceEnabledChanged();
        m_sourceTrimVersion++;
        emit sourceTrimChanged();
        m_currentSettings.audioOutputLatencyMs =
            qBound(0, m_currentSettings.audioOutputLatencyMs, 500);
        if (m_audioPlayer)
            m_audioPlayer->setOutputLatencyOffsetMs(m_currentSettings.audioOutputLatencyMs);
        emit audioOutputLatencyChanged();
    }
}

void UIManager::addStream() {
    SourceSettings source;
    const int nextId = nextSourceIdSeed(m_currentSettings.sources);
    source.id = QString::number(nextId);
    source.name = "";
    source.url = "";
    m_currentSettings.sources.append(source);
    m_sourceEnabled.append(true);
    syncActiveStreams();
    emit streamUrlsChanged();
    emit streamNamesChanged();
    emit streamIdsChanged();
    m_sourceEnabledVersion++;
    emit sourceEnabledChanged();
    // Note: ReplayManager handles the worker creation during startRecording()
}

void UIManager::removeStream(int index) {
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources.removeAt(index);
        if (index >= 0 && index < m_sourceEnabled.size()) {
            m_sourceEnabled.removeAt(index);
        }
        syncActiveStreams();
        emit streamUrlsChanged();
        emit streamNamesChanged();
        emit streamIdsChanged();
        m_sourceEnabledVersion++;
        emit sourceEnabledChanged();
    }
}

void UIManager::saveSettings() {
    if (m_settingsManager->save(m_configPath, m_currentSettings)) {
        std::fprintf(stderr, "Settings saved successfully.\n");
    }
}
void UIManager::onStartRequested() {
    // UI Manager can do final validation before telling engine to work
    if (m_replayManager->isRecording()) return;

    std::fprintf(stderr, "UIManager: Requesting engine start...\n");
    startRecording();
}

void UIManager::onStopRequested() {
    std::fprintf(stderr, "UIManager: Requesting engine stop...\n");
    stopRecording();
}

void UIManager::updateStreamUrl(int index, const QString& url) {
    // Hot-swap: update the source worker directly (real FFmpeg reconnect)
    m_replayManager->updateSourceUrl(index, url);

    // Auto-save whenever a URL is modified
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources[index].url = url;
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

QVariantList UIManager::playbackProviders() const {
    QVariantList list;
    for (auto* p : m_providers) {
        list.append(QVariant::fromValue(p));
    }
    return list;
}

QVariantList UIManager::screenOptions() const {
    return m_screenOptions;
}

bool UIManager::screensReady() const {
    return !m_screenOptions.isEmpty();
}

int UIManager::screenCount() const {
    return m_screens.size();
}

void UIManager::refreshScreens() {
    QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        if (auto *primary = QGuiApplication::primaryScreen()) {
            screens.append(primary);
        }
    }

    m_screens = screens;
    m_screenOptions.clear();

    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens.at(i);
        QString name = screen ? screen->name().trimmed() : QString();
        QSize size = screen ? screen->size() : QSize();

        QString label = name.isEmpty()
                            ? QString("Display %1").arg(i + 1)
                            : name;
        if (size.isValid()) {
            label = QString("%1 — %2×%3").arg(label).arg(size.width()).arg(size.height());
        }

        QVariantMap entry;
        entry.insert("index", i);
        entry.insert("label", label);
        m_screenOptions.append(entry);
    }

    emit screensChanged();
}

QScreen* UIManager::screenAt(int index) const {
    if (index < 0 || index >= m_screens.size()) return nullptr;
    return m_screens.at(index);
}

void UIManager::refreshProviders() {
    // The playback worker holds raw pointers to the providers: stop it
    // before deleting them or its decode thread dereferences freed
    // FrameProviders. Callers that need playback running afterwards
    // (openStreams, setStreamUrls, ...) restart it via
    // restartPlaybackWorker().
    if (m_playbackWorker) {
        m_playbackWorker->stop();
        delete m_playbackWorker;
        m_playbackWorker = nullptr;
    }

    // Cleanup old providers
    qDeleteAll(m_providers);
    m_providers.clear();

    // Create a provider for every stream URL
    int count = activeStreamUrls().size();
    for (int i = 0; i < count; ++i) {
        m_providers.append(new FrameProvider(this));
    }
    emit playbackProvidersChanged();
}

int64_t UIManager::recordedDurationMs() {
    // Get this from your Master Clock / Recording Engine
    return m_replayManager->getElapsedMs();
}

int64_t UIManager::scrubPosition() {

    if(!m_transport) return 0;
    if(!m_replayManager) return 0;

    const int64_t pos = qMax<int64_t>(0, m_transport->currentPos());
    return qMin<int64_t>(pos, m_replayManager->getElapsedMs());
}

void UIManager::scrubToLive() {
    setFollowLive(true);
    const int64_t liveEdge = recordedDurationMs();
    const int64_t target = qMax<int64_t>(0, liveEdge - m_liveBufferMs);
    m_transport->seek(target);
    // Route the jump-to-live-edge through the scheduler too, or the worker
    // sees a position discontinuity with no seek and tail-holds a stale frame.
    if (m_playbackWorker) m_playbackWorker->seekTo(target);
}

static QString sanitizeFileToken(const QString& input) {
    QString out = input.trimmed();
    out.replace(QRegularExpression("[\\\\/:*?\"<>|]+"), "_");
    out.replace(QRegularExpression("\\s+"), "_");
    if (out.isEmpty()) return QString("UNNAMED");
    return out;
}

static QString formatTimecodeForFile(int64_t ms, int fps) {
    if (ms < 0) ms = 0;
    int totalSeconds = static_cast<int>(ms / 1000);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;
    int frames = static_cast<int>((ms % 1000) / (1000.0 / qMax(1, fps)));

    return QString("%1%2%3%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

static QString formatTimecodeForDisplay(int64_t ms, int fps) {
    if (ms < 0) ms = 0;
    const int totalSeconds = static_cast<int>(ms / 1000);
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;
    const int frames = static_cast<int>((ms % 1000) / (1000.0 / qMax(1, fps)));
    return QString("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

void UIManager::updateXTouchDisplay() {
    if (!m_midiManager || !m_midiManager->isXTouchConnected()) return;

    const int64_t playheadMs = scrubPosition();
    QString displayText;
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    const int fps = recordFps() > 0 ? recordFps() : 30;

    const qint64 startEpochMs = m_replayManager ? m_replayManager->getRecordingStartEpochMs() : 0;
    const bool showTimeOfDay = m_currentSettings.showTimeOfDay && startEpochMs > 0;

    if (showTimeOfDay) {
        const qint64 epochMs = startEpochMs + playheadMs;
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(epochMs);
        hours = dt.time().hour();
        minutes = dt.time().minute();
        seconds = dt.time().second();
        displayText = dt.toString("HH:mm:ss");
    } else {
        const int totalSeconds = static_cast<int>(playheadMs / 1000);
        hours = totalSeconds / 3600;
        minutes = (totalSeconds % 3600) / 60;
        seconds = totalSeconds % 60;
        displayText = formatTimecodeForDisplay(playheadMs, fps);
    }

    frames = static_cast<int>((playheadMs % 1000) / (1000.0 / qMax(1, fps)));

    if (!m_xTouchLastSend.isValid()) {
        m_xTouchLastSend.start();
    }

    if (m_xTouchLastSend.elapsed() < m_xTouchMinIntervalMs) {
        return;
    }

    if (displayText != m_xTouchLastText) {
        m_xTouchLastText = displayText;
    }
    m_xTouchLastSend.restart();
    const QString digits = QString("   %1%2%3%4")
                               .arg(hours, 2, 10, QChar('0'))
                               .arg(minutes, 2, 10, QChar('0'))
                               .arg(seconds, 2, 10, QChar('0'))
                               .arg(frames, 2, 10, QChar('0'));
    const quint8 dots1 = (1 << 4) | (1 << 6);
    const quint8 dots2 = 0;
    m_midiManager->sendXTouchSegmentDisplay(digits, dots1, dots2);
}

void UIManager::updateXTouchLcd() {
    if (!m_midiManager || !m_midiManager->isXTouchConnected()) return;

    QString label;
    if (m_playbackSingleView && m_playbackSelectedIndex >= 0
        && m_playbackSelectedIndex < m_currentSettings.sources.size()) {
        const QString name = m_currentSettings.sources[m_playbackSelectedIndex].name.trimmed();
        label = name.isEmpty()
                    ? QString("CAM %1").arg(m_playbackSelectedIndex + 1)
                    : name;
    } else if (m_playbackSingleView && m_playbackSelectedIndex >= 0) {
        label = QString("CAM %1").arg(m_playbackSelectedIndex + 1);
    }

    m_midiManager->sendXTouchLcdText(label);
}

void UIManager::captureSnapshot(bool singleView, int selectedIndex, int64_t playheadMs) {
    if (m_providers.isEmpty()) return;

    const QString projectName = sanitizeFileToken(m_currentSettings.fileName);
    const int fps = recordFps() > 0 ? recordFps() : 30;

    const qint64 startEpochMs = m_replayManager ? m_replayManager->getRecordingStartEpochMs() : 0;
    const qint64 playheadEpochMs = (startEpochMs > 0) ? (startEpochMs + playheadMs) : QDateTime::currentMSecsSinceEpoch();

    const QString recTimeOfDay = QDateTime::fromMSecsSinceEpoch(playheadEpochMs).toString("HHmmss");
    const QString playheadTime = formatTimecodeForFile(playheadMs, fps);

    QString outputDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/videos";
    QDir dir(outputDir);
    if (!dir.exists()) dir.mkpath(".");

    auto saveImageForIndex = [&](int index) {
        if (index < 0 || index >= m_providers.size()) return;

        const QString feedName = (index < m_currentSettings.sources.size() && !m_currentSettings.sources[index].name.trimmed().isEmpty())
                     ? sanitizeFileToken(m_currentSettings.sources[index].name)
                                 : QString("CAM%1").arg(index + 1);

        QImage image = m_providers[index]->latestImage();
        if (image.isNull()) return;

        const QString fileName = QString("%1_%2_%3_%4.jpg")
            .arg(projectName)
            .arg(feedName)
            .arg(recTimeOfDay)
            .arg(playheadTime);

        const QString fullPath = dir.absoluteFilePath(fileName);
        QImageWriter writer(fullPath, "jpg");
        writer.setQuality(95);
        writer.write(image);
    };

    if (singleView) {
        saveImageForIndex(selectedIndex);
    } else {
        for (int i = 0; i < m_providers.size(); ++i) {
            saveImageForIndex(i);
        }
    }
}

void UIManager::onRecorderPulse(int64_t frameIndex, int64_t elapsedMs) {
    Q_UNUSED(frameIndex);
    Q_UNUSED(elapsedMs);
    emit recordedDurationMsChanged();
    emit scrubPositionChanged();
    emit recordingStartEpochMsChanged();
    updateXTouchDisplay();

    if (m_followLive && m_transport && m_transport->isPlaying()) {
        const int64_t liveEdge = recordedDurationMs();
        const int64_t target = qMax<int64_t>(0, liveEdge - m_liveBufferMs);
        const int64_t current = m_transport->currentPos();
        if (qAbs(current - target) > 50) {
            m_transport->seek(target);
            // Classify the yank via the scheduler: small in-window corrections
            // hit the 0-seek reuse path; only a real jump repositions.
            if (m_playbackWorker) m_playbackWorker->seekTo(target);
        }
    }
}

QString UIManager::getSettingsPath(QString fileName) {
    // 1. Get the Documents directory for your app
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    // 2. Create a subfolder if you want to be organized
    QDir dir(docPath);
    if (!dir.exists("settings")) {
        dir.mkdir("settings");
    }

    // 3. Construct the full filename
    return docPath + "/settings/" + fileName;
}

void UIManager::pushStreamDeckMaps()
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    const QString model = m_streamDeckManager->deviceModel();
    m_streamDeckManager->pushKeyMap(model, m_streamDeckStore.keyMap(model));
    m_streamDeckManager->pushDialMaps(model,
        m_streamDeckStore.dialRotateMap(model),
        m_streamDeckStore.dialPressMap(model));
}

QString UIManager::playbackTimecode()
{
    // MUST stay identical to the QML playback timecode (Main.qml): the deck and
    // the on-screen label render this one string. Source = scrubPosition.
    const qint64 pos = scrubPosition();
    if (m_currentSettings.showTimeOfDay && recordingStartEpochMs() > 0) {
        // Wall-clock time at the playhead (not a live clock — moves with pos).
        return QDateTime::fromMSecsSinceEpoch(recordingStartEpochMs() + pos)
            .toString(QStringLiteral("HH:mm:ss"));
    }
    const qint64 ms = pos < 0 ? 0 : pos;
    const int fps = recordFps() > 0 ? recordFps() : 30;
    const qint64 totalSeconds = ms / 1000;
    const int frames = int(double(ms % 1000) / (1000.0 / double(fps)));
    return QStringLiteral("%1:%2:%3.%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}

void UIManager::pushDeckTimecode()
{
    if (!m_streamDeckManager) return;
    const qint64 dur = recordedDurationMs();
    const qint64 pos = scrubPosition();
    const double frac = dur > 0 ? qBound(0.0, double(pos) / double(dur), 1.0) : 0.0;
    m_streamDeckManager->setPosition(playbackTimecode(), frac);
}

void UIManager::beginStreamDeckLearn(int action)
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    if (m_streamDeckLearnAction == action) {       // toggle off
        m_streamDeckLearnAction = -1;
        m_streamDeckManager->setLearnMode(false);
        emit streamDeckLearnActionChanged();
        return;
    }
    m_streamDeckLearnAction = action;
    m_streamDeckManager->setLearnMode(true);
    emit streamDeckLearnActionChanged();
}

void UIManager::clearStreamDeckBinding(int action)
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    const QString model = m_streamDeckManager->deviceModel();
    m_streamDeckStore.clear(model, action);
    m_streamDeckStore.writeTo(m_currentSettings);
    m_settingsManager->save(m_configPath, m_currentSettings);
    pushStreamDeckMaps();
    m_streamDeckBindingsVersion++;
    emit streamDeckBindingsChanged();
}

void UIManager::resetStreamDeckDefaults()
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    const QString model = m_streamDeckManager->deviceModel();
    m_streamDeckStore.resetToDefault(model,
        m_streamDeckManager->keyCount(), m_streamDeckManager->dialCount());
    m_streamDeckStore.writeTo(m_currentSettings);
    m_settingsManager->save(m_configPath, m_currentSettings);
    pushStreamDeckMaps();
    m_streamDeckBindingsVersion++;
    emit streamDeckBindingsChanged();
}

QString UIManager::streamDeckBindingLabel(int action) const
{
    if (!m_streamDeckManager) return QStringLiteral("Unassigned");
    return m_streamDeckStore.bindingLabel(m_streamDeckManager->deviceModel(), action);
}

void UIManager::shuttleStep(int delta)
{
    if (!m_transport) return;
    cancelFollowLive();
    const ShuttleResult r = shuttleLadderStep(m_transport->speed(), delta);
    m_transport->setSpeed(r.speed);
    m_transport->setPlaying(r.playing);
    if (m_playbackWorker && !r.playing) {
        m_playbackWorker->seekTo(m_transport->currentPos());
    }
}
