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
    QStringList streamNames;
    QString saveLocation;
    QString fileName;
    int videoWidth = 1920;
    int videoHeight = 1080;
    int fps = 30;
    bool showTimeOfDay = false;
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
