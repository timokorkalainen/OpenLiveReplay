#include "uimanager.h"
#include <QDateTime>
#include <algorithm>
#include <QDir>
#include <QGuiApplication>
#if defined(Q_OS_IOS)
#include "ios/ios_scene.h"
#endif
#include <QImageWriter>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QTimer>
#include <QScreen>
#include <QVariantMap>
#include <cstdio>

UIManager::UIManager(ReplayManager *engine, QObject *parent)
    : QObject(parent), m_replayManager(engine) {
    m_jogTimer.start();
    connect(m_replayManager, &ReplayManager::masterPulse,
            this, &UIManager::onRecorderPulse,
            Qt::QueuedConnection);
    m_settingsManager = new SettingsManager();
    m_configPath = getSettingsPath("config.json");
    m_transport = new PlaybackTransport(this);
    m_transport->seek(0);
    m_transport->setFps(m_currentSettings.fps);
    connect(m_transport, &PlaybackTransport::posChanged, this, [this]() {
        updateXTouchDisplay();
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

        switch (matchedAction) {
        case 0:
            if (!isRelease) playPause();
            break;
        case 1:
            if (isRelease) {
                if (m_midiHoldAction == 1) {
                    if (m_transport) {
                        m_transport->setSpeed(1.0);
                        m_transport->setPlaying(m_midiHoldWasPlaying);
                    }
                    m_midiHoldAction = -1;
                }
            } else {
                cancelFollowLive();
                m_midiHoldWasPlaying = m_transport ? m_transport->isPlaying() : false;
                m_midiHoldAction = 1;
                if (m_transport) {
                    m_transport->setSpeed(-5.0);
                    m_transport->setPlaying(true);
                }
            }
            break;
        case 2:
            if (isRelease) {
                if (m_midiHoldAction == 2) {
                    if (m_transport) {
                        m_transport->setSpeed(1.0);
                        m_transport->setPlaying(m_midiHoldWasPlaying);
                    }
                    m_midiHoldAction = -1;
                }
            } else {
                cancelFollowLive();
                m_midiHoldWasPlaying = m_transport ? m_transport->isPlaying() : false;
                m_midiHoldAction = 2;
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
        case 8: {
            if (!m_transport) break;

            int forwardValue = m_midiBindingData2Forward.value(matchedAction, -1);
            int backwardValue = m_midiBindingData2Backward.value(matchedAction, -1);

            int deltaSign = 0;
            if (forwardValue >= 0 && data2 == forwardValue) {
                deltaSign = 1;
            } else if (backwardValue >= 0 && data2 == backwardValue) {
                deltaSign = -1;
            } else {
                break;
            }

            m_transport->setPlaying(false);
            cancelFollowLive();

            m_transport->step(deltaSign);

            if (m_playbackWorker) {
                int64_t targetMs = m_transport->currentPos();
                m_playbackWorker->deliverBufferedFrameAtOrBefore(targetMs);
                m_playbackWorker->seekTo(targetMs);
            }
            break;
        }
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
        default:
            if (!isRelease && matchedAction >= 100 && matchedAction < 108) {
                emit feedSelectRequested(matchedAction - 100);
            }
            break;
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
int UIManager::recordFps() const { return m_currentSettings.fps; }
int UIManager::multiviewCount() const { return m_currentSettings.multiviewCount; }
bool UIManager::isRecording() const { return m_replayManager->isRecording(); }
qint64 UIManager::recordingStartEpochMs() const {
    return m_replayManager ? m_replayManager->getRecordingStartEpochMs() : 0;
}
bool UIManager::timeOfDayMode() const {
    return m_currentSettings.showTimeOfDay;
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
    urls.reserve(m_currentSettings.sources.size());
    names.reserve(m_currentSettings.sources.size());
    for (const auto &source : m_currentSettings.sources) {
        urls.append(source.url);
        names.append(source.name);
    }

    // Source configuration: ALL source URLs go to the engine (one worker per source)
    m_replayManager->setSourceUrls(urls);
    m_replayManager->setSourceNames(names);

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
        urls.reserve(m_currentSettings.sources.size());
        names.reserve(m_currentSettings.sources.size());
        for (const auto &source : m_currentSettings.sources) {
            urls.append(source.url);
            names.append(source.name);
        }
        m_replayManager->setSourceUrls(urls);
        m_replayManager->setSourceNames(names);
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
    if (m_currentSettings.saveLocation != path) {
        m_currentSettings.saveLocation = path;
        m_replayManager->setOutputDirectory(path);
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
    if (m_currentSettings.fps != fps) {
        m_currentSettings.fps = fps;
        m_replayManager->setFps(fps);
        if (m_transport) {
            m_transport->setFps(fps);
        }
        if (m_playbackWorker) {
            m_playbackWorker->setFrameBufferMax(fps);
        }
        emit recordFpsChanged();
    }
}

void UIManager::setMultiviewCount(int count) {
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
    m_transport->setPlaying(!m_transport->isPlaying());
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
}

void UIManager::stepFrameBack() {
    if (!m_transport) return;
    m_transport->step(-1);
    m_transport->setPlaying(false);
    cancelFollowLive();

    if (m_playbackWorker) {
        int64_t targetMs = m_transport->currentPos();
        m_playbackWorker->deliverBufferedFrameAtOrBefore(targetMs);
        m_playbackWorker->seekTo(targetMs);
    }
}

void UIManager::goLive() {
    if (!m_transport) return;
    m_transport->setSpeed(1.0);
    scrubToLive();
}

void UIManager::cancelFollowLive() {
    m_followLive = false;
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
    m_playbackSingleView = singleView;
    m_playbackSelectedIndex = selectedIndex;
    updateXTouchLcd();
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
    m_replayManager->startRecording();
    m_followLive = true;

    // 1. Initialize the Playback Worker with our providers
    if (m_playbackWorker) {
        m_playbackWorker->stop();
        delete m_playbackWorker;
    }

    m_playbackWorker = new PlaybackWorker(m_providers, m_transport, this);
    m_playbackWorker->setFrameBufferMax(m_currentSettings.fps);

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

    m_playbackWorker = new PlaybackWorker(m_providers, m_transport, this);
    m_playbackWorker->setFrameBufferMax(m_currentSettings.fps);
    m_playbackWorker->openFile(m_replayManager->getVideoPath());
    m_playbackWorker->start();
    m_transport->seek(0);
    m_transport->setPlaying(true);
    m_followLive = true;
}

void UIManager::stopRecording() {
    m_replayManager->stopRecording();
    m_transport->setPlaying(false);
    m_followLive = false;
    if (m_playbackWorker) {
        m_playbackWorker->stop();
    }

    emit recordingStatusChanged();
    emit recordingStopped();
    emit recordingStartEpochMsChanged();
}

void UIManager::seekPlayback(int64_t ms) {
    if (m_transport) {
        m_transport->seek(ms);
        // Manual seek disables live-follow; user can re-enable via "Live"
        m_followLive = false;
    }
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

void UIManager::updateStreamName(int index, const QString& name) {
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources[index].name = name;
        rebuildSlotMap();
        m_replayManager->setSourceNames(streamNames());
        m_replayManager->setViewNames(activeStreamNames());
        emit streamNamesChanged();
        emit viewSlotMapChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::updateStreamId(int index, const QString& id) {
    if (index >= 0 && index < m_currentSettings.sources.size()) {
        m_currentSettings.sources[index].id = id;
        emit streamIdsChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::loadSettings() {
    if (m_settingsManager->load(m_configPath, m_currentSettings)) {
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
        m_replayManager->setFps(m_currentSettings.fps);
        m_currentSettings.multiviewCount = qBound(1, m_currentSettings.multiviewCount, 16);
        ensureSourceEnabledSize();
        syncActiveStreams();
        if (m_transport) {
            m_transport->setFps(m_currentSettings.fps);
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
        emit recordFpsChanged();
        emit multiviewCountChanged();
        emit timeOfDayModeChanged();
        emit midiPortNameChanged();
        emit viewSlotMapChanged();
        emit sourceEnabledChanged();
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
    m_replayManager->startRecording();
}

void UIManager::onStopRequested() {
    std::fprintf(stderr, "UIManager: Requesting engine stop...\n");
    m_replayManager->stopRecording();
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

    return qMin(qMax(0, m_transport->currentPos()), m_replayManager->getElapsedMs());
}

void UIManager::scrubToLive() {
    m_followLive = true;
    const int64_t liveEdge = recordedDurationMs();
    const int64_t target = qMax<int64_t>(0, liveEdge - m_liveBufferMs);
    m_transport->seek(target);
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
    const int fps = m_currentSettings.fps > 0 ? m_currentSettings.fps : 30;

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
    const int fps = m_currentSettings.fps > 0 ? m_currentSettings.fps : 30;

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

void UIManager::onRecorderPulse(int64_t elapsed, int64_t frameCount) {
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
