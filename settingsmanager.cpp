#include "settingsmanager.h"
#include <QDebug>

SettingsManager::SettingsManager() {}

bool SettingsManager::save(const QString &path, const AppSettings &settings) {
    QJsonObject root;

    // Save the separate file path strings
    root["saveLocation"] = settings.saveLocation;
    root["fileName"] = settings.fileName;

    // Convert QStringList to QJsonArray
    QJsonArray urlArray;
    for (const QString &url : settings.streamUrls) {
        urlArray.append(url);
    }
    root["streams"] = urlArray;

    QJsonDocument doc(root);
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "SettingsManager: Failed to open file for writing:" << path;
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

void SettingsManager::extracted(AppSettings &settings, QJsonArray &urlArray) {
    for (const QJsonValue &val : urlArray) {
        settings.streamUrls.append(val.toString());
    }
}
bool SettingsManager::load(const QString &path, AppSettings &settings) {
    QFile file(path);

    if (!file.exists()) {
        qDebug() << "SettingsManager: Config file does not exist:" << path;
        return false;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "SettingsManager: Failed to open file for reading:" << path;
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull()) {
        qDebug() << "SettingsManager: JSON Parse Error:" << error.errorString();
        return false;
    }

    if (!doc.isObject()) {
        qDebug() << "SettingsManager: JSON document is not an object.";
        return false;
    }

    QJsonObject root = doc.object();

    // Parse individual settings
    settings.saveLocation = root["saveLocation"].toString();
    settings.fileName = root["fileName"].toString();

    // Parse the stream list
    settings.streamUrls.clear();
    QJsonArray urlArray = root["streams"].toArray();
    extracted(settings, urlArray);

    return true;
}
