#include "uimanager.h"

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
        emit recordFpsChanged();
    }
}

void UIManager::setTimeOfDayMode(bool enabled) {
    if (m_currentSettings.showTimeOfDay == enabled) return;
    m_currentSettings.showTimeOfDay = enabled;
    emit timeOfDayModeChanged();
    m_settingsManager->save(m_configPath, m_currentSettings);
}

void UIManager::openStreams() {
    refreshProviders();
    if (m_replayManager->isRecording()) {
        restartPlaybackWorker();
    }
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
        qDebug() << "Settings saved successfully.";
    }
}
void UIManager::onStartRequested() {
    // UI Manager can do final validation before telling engine to work
    if (m_replayManager->isRecording()) return;

    qDebug() << "UIManager: Requesting engine start...";
    m_replayManager->startRecording();
}

void UIManager::onStopRequested() {
    qDebug() << "UIManager: Requesting engine stop...";
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

void UIManager::onRecorderPulse(int64_t elapsed, int64_t frameCount) {
    emit recordedDurationMsChanged();
    emit scrubPositionChanged();
    emit recordingStartEpochMsChanged();

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
