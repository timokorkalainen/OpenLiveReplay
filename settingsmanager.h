#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>

// Simple data structure to hold our application state
struct AppSettings {
    QStringList streamUrls;
    QString saveLocation;
    QString fileName;
};

class SettingsManager {
public:
    explicit SettingsManager();
    // Saves settings to a JSON file at the specified path
    bool save(const QString &path, const AppSettings &settings);

    // Loads settings from a JSON file into the provided struct reference
    void extracted(AppSettings &settings, QJsonArray &urlArray);
    bool load(const QString &path, AppSettings &settings);
};

#endif // SETTINGSMANAGER_H
