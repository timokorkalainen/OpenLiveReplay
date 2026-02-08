#include "uimanager.h"
#include <QDateTime>
#include <QDir>
#include <QImageWriter>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>

UIManager::UIManager(ReplayManager *engine, QObject *parent)
    : QObject(parent), m_replayManager(engine) {
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
            m_midiBindings[m_midiLearnAction] = {status, data1};
            m_currentSettings.midiBindings.insert(m_midiLearnAction, qMakePair(status, data1));
            m_settingsManager->save(m_configPath, m_currentSettings);
            m_midiLearnAction = -1;
            emit midiLearnActionChanged();
            m_midiBindingsVersion++;
            emit midiBindingsChanged();
            return;
        }

        int matchedAction = -1;
        for (auto it = m_midiBindings.constBegin(); it != m_midiBindings.constEnd(); ++it) {
            if (it.value().status == status && it.value().data1 == data1) {
                matchedAction = it.key();
                break;
            }
        }
        if (matchedAction < 0) return;

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
    refreshProviders();
}

QStringList UIManager::streamUrls() const { return m_currentSettings.streamUrls; }
QStringList UIManager::streamNames() const { return m_currentSettings.streamNames; }
QString UIManager::saveLocation() const { return m_currentSettings.saveLocation; }
QString UIManager::fileName() const { return m_currentSettings.fileName; }
int UIManager::recordWidth() const { return m_currentSettings.videoWidth; }
int UIManager::recordHeight() const { return m_currentSettings.videoHeight; }
int UIManager::recordFps() const { return m_currentSettings.fps; }
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

void UIManager::setStreamUrls(const QStringList &urls) {
    if (m_currentSettings.streamUrls != urls) {
        m_currentSettings.streamUrls = urls;
        m_replayManager->setStreamUrls(urls);
        if (m_currentSettings.streamNames.size() < urls.size()) {
            while (m_currentSettings.streamNames.size() < urls.size()) {
                m_currentSettings.streamNames.append("");
            }
        } else if (m_currentSettings.streamNames.size() > urls.size()) {
            m_currentSettings.streamNames = m_currentSettings.streamNames.mid(0, urls.size());
        }
        m_replayManager->setStreamNames(m_currentSettings.streamNames);
        refreshProviders();
        if (m_replayManager->isRecording()) {
            restartPlaybackWorker();
        }
        emit streamUrlsChanged();
        emit streamNamesChanged();
    }
}

void UIManager::setStreamNames(const QStringList &names) {
    if (m_currentSettings.streamNames != names) {
        m_currentSettings.streamNames = names;
        if (m_currentSettings.streamNames.size() < m_currentSettings.streamUrls.size()) {
            while (m_currentSettings.streamNames.size() < m_currentSettings.streamUrls.size()) {
                m_currentSettings.streamNames.append("");
            }
        } else if (m_currentSettings.streamNames.size() > m_currentSettings.streamUrls.size()) {
            m_currentSettings.streamNames = m_currentSettings.streamNames.mid(0, m_currentSettings.streamUrls.size());
        }
        m_replayManager->setStreamNames(m_currentSettings.streamNames);
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
    emit midiLearnActionChanged();
}

void UIManager::clearMidiBinding(int action) {
    m_midiBindings.remove(action);
    m_currentSettings.midiBindings.remove(action);
    m_settingsManager->save(m_configPath, m_currentSettings);
    m_midiBindingsVersion++;
    emit midiBindingsChanged();
}

QString UIManager::midiBindingLabel(int action) const {
    if (!m_midiBindings.contains(action)) return QString("Unassigned");
    const auto binding = m_midiBindings.value(action);
    if (binding.status < 0 || binding.data1 < 0) return QString("Unassigned");
    return QString("0x%1 0x%2").arg(binding.status, 2, 16, QChar('0')).arg(binding.data1, 2, 16, QChar('0')).toUpper();
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
    if (index >= 0 && index < m_currentSettings.streamUrls.size()) {
        m_currentSettings.streamUrls[index] = url;
        m_replayManager->updateTrackUrl(index, url); // Hot-swap if recording
        emit streamUrlsChanged();

        // Auto-save to JSON
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::updateStreamName(int index, const QString& name) {
    if (index >= 0 && index < m_currentSettings.streamNames.size()) {
        m_currentSettings.streamNames[index] = name;
        m_replayManager->setStreamNames(m_currentSettings.streamNames);
        emit streamNamesChanged();
        m_settingsManager->save(m_configPath, m_currentSettings);
    }
}

void UIManager::loadSettings() {
    if (m_settingsManager->load(m_configPath, m_currentSettings)) {
        // Apply to engine
        m_replayManager->setStreamUrls(m_currentSettings.streamUrls);
        m_replayManager->setOutputDirectory(m_currentSettings.saveLocation);
        m_replayManager->setBaseFileName(m_currentSettings.fileName);
        m_replayManager->setVideoWidth(m_currentSettings.videoWidth);
        m_replayManager->setVideoHeight(m_currentSettings.videoHeight);
        m_replayManager->setFps(m_currentSettings.fps);
        if (m_currentSettings.streamNames.size() < m_currentSettings.streamUrls.size()) {
            while (m_currentSettings.streamNames.size() < m_currentSettings.streamUrls.size()) {
                m_currentSettings.streamNames.append("");
            }
        } else if (m_currentSettings.streamNames.size() > m_currentSettings.streamUrls.size()) {
            m_currentSettings.streamNames = m_currentSettings.streamNames.mid(0, m_currentSettings.streamUrls.size());
        }
        m_replayManager->setStreamNames(m_currentSettings.streamNames);
        if (m_transport) {
            m_transport->setFps(m_currentSettings.fps);
        }

        m_midiBindings.clear();
        for (auto it = m_currentSettings.midiBindings.constBegin(); it != m_currentSettings.midiBindings.constEnd(); ++it) {
            m_midiBindings.insert(it.key(), {it.value().first, it.value().second});
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
        emit saveLocationChanged();
        emit fileNameChanged();
        emit recordWidthChanged();
        emit recordHeightChanged();
        emit recordFpsChanged();
        emit timeOfDayModeChanged();
        emit midiPortNameChanged();
    }
}

void UIManager::addStream() {
    QStringList urls = m_currentSettings.streamUrls;
    urls.append(""); // Add an empty entry
    setStreamUrls(urls);
    m_currentSettings.streamNames.append("");
    setStreamNames(m_currentSettings.streamNames);
    // Note: ReplayManager handles the worker creation during startRecording()
}

void UIManager::removeStream(int index) {
    QStringList urls = m_currentSettings.streamUrls;
    if (index >= 0 && index < urls.size()) {
        urls.removeAt(index);
        setStreamUrls(urls);
        if (index >= 0 && index < m_currentSettings.streamNames.size()) {
            m_currentSettings.streamNames.removeAt(index);
            setStreamNames(m_currentSettings.streamNames);
        }
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
    // This allows hot-swapping through the UI
    m_replayManager->updateTrackUrl(index, url);

    // Auto-save whenever a URL is modified
    AppSettings current;
    current.streamUrls = m_replayManager->getStreamUrls();
    current.streamNames = m_replayManager->getStreamNames();
    current.saveLocation = m_replayManager->getOutputDirectory();
    current.fileName = m_replayManager->getBaseFileName();
    m_settingsManager->save(m_configPath, current);
}

QVariantList UIManager::playbackProviders() const {
    QVariantList list;
    for (auto* p : m_providers) {
        list.append(QVariant::fromValue(p));
    }
    return list;
}

void UIManager::refreshProviders() {
    // Cleanup old providers
    qDeleteAll(m_providers);
    m_providers.clear();

    // Create a provider for every stream URL
    int count = m_replayManager->getStreamUrls().size();
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
        && m_playbackSelectedIndex < m_currentSettings.streamNames.size()) {
        const QString name = m_currentSettings.streamNames[m_playbackSelectedIndex].trimmed();
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

        const QString feedName = (index < m_currentSettings.streamNames.size() && !m_currentSettings.streamNames[index].trimmed().isEmpty())
                                 ? sanitizeFileToken(m_currentSettings.streamNames[index])
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
