#include "uimanager.h"

UIManager::UIManager(ReplayManager *engine, QObject *parent)
    : QObject(parent), m_replayManager(engine) {
    m_settingsManager = new SettingsManager();
    m_transport = new PlaybackTransport(this);
    refreshProviders();
}

QStringList UIManager::streamUrls() const { return m_currentSettings.streamUrls; }
QString UIManager::saveLocation() const { return m_currentSettings.saveLocation; }
QString UIManager::fileName() const { return m_currentSettings.fileName; }
bool UIManager::isRecording() const { return m_replayManager->isRecording(); }

void UIManager::setStreamUrls(const QStringList &urls) {
    if (m_currentSettings.streamUrls != urls) {
        m_currentSettings.streamUrls = urls;
        m_replayManager->setStreamUrls(urls);
        refreshProviders();
        emit streamUrlsChanged();
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

void UIManager::startRecording() {
    refreshProviders();
    m_replayManager->startRecording();

    // 1. Initialize the Playback Worker with our providers
    if (m_playbackWorker) {
        m_playbackWorker->stop();
        delete m_playbackWorker;
    }

    m_playbackWorker = new PlaybackWorker(m_providers, m_transport, this);

    // 2. Point it to the file being recorded
    QString filePath = m_replayManager->getOutputDirectory() + "/" + m_replayManager->getBaseFileName() + ".mkv";
    m_playbackWorker->openFile(filePath);

    m_playbackWorker->start();
    m_transport->seek(0);
    m_transport->setPlaying(true);

    emit recordingStatusChanged();
    emit recordingStarted();
}

void UIManager::stopRecording() {
    m_replayManager->stopRecording();
    m_transport->setPlaying(false);
    if (m_playbackWorker) {
        m_playbackWorker->stop();
    }

    emit recordingStatusChanged();
    emit recordingStopped();
}

void UIManager::seekPlayback(int64_t ms) {
    if (m_transport) {
        m_transport->seek(ms);
    }
}

void UIManager::updateUrl(int index, const QString &url) {
    if (index >= 0 && index < m_currentSettings.streamUrls.size()) {
        m_currentSettings.streamUrls[index] = url;
        m_replayManager->updateTrackUrl(index, url); // Hot-swap if recording
        emit streamUrlsChanged();

        // Auto-save to JSON
        m_settingsManager->save("./config.json", m_currentSettings);
    }
}

void UIManager::loadSettings() {
    if (m_settingsManager->load("./config.json", m_currentSettings)) {
        // Apply to engine
        m_replayManager->setStreamUrls(m_currentSettings.streamUrls);
        m_replayManager->setOutputDirectory(m_currentSettings.saveLocation);
        m_replayManager->setBaseFileName(m_currentSettings.fileName);

        // Sync QML
        emit streamUrlsChanged();
        emit saveLocationChanged();
        emit fileNameChanged();
    }
}

void UIManager::addStream() {
    QStringList urls = m_currentSettings.streamUrls;
    urls.append(""); // Add an empty entry
    setStreamUrls(urls);
    // Note: ReplayManager handles the worker creation during startRecording()
}

void UIManager::removeStream(int index) {
    QStringList urls = m_currentSettings.streamUrls;
    if (index >= 0 && index < urls.size()) {
        urls.removeAt(index);
        setStreamUrls(urls);
    }
}

void UIManager::saveSettings() {
    if (m_settingsManager->save("config.json", m_currentSettings)) {
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
