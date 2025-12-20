#include "uimanager.h"

UIManager::UIManager(ReplayManager *engine, QObject *parent)
    : QObject(parent), m_replayManager(engine) {
    m_settingsManager = new SettingsManager();
}

QStringList UIManager::streamUrls() const { return m_currentSettings.streamUrls; }
QString UIManager::saveLocation() const { return m_currentSettings.saveLocation; }
QString UIManager::fileName() const { return m_currentSettings.fileName; }
bool UIManager::isRecording() const { return m_replayManager->isRecording(); }

void UIManager::setStreamUrls(const QStringList &urls) {
    if (m_currentSettings.streamUrls != urls) {
        m_currentSettings.streamUrls = urls;
        m_replayManager->setStreamUrls(urls);
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
    m_replayManager->startRecording();
    emit recordingStatusChanged();
}

void UIManager::stopRecording() {
    m_replayManager->stopRecording();
    emit recordingStatusChanged();
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

