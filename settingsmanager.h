#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QList>
#include <QMap>

// Simple data structure to hold our application state
struct SourceSettings {
    QString id;
    QString name;
    QString url;
    QJsonArray metadata;
    int trimOffsetMs = 0; // per-source timeline trim (+delay / -advance), ms
};

struct AppSettings {
    QList<SourceSettings> sources;
    QJsonArray metadataFields; // Global field definitions: [{name, display}]
    QString saveLocation;
    QString fileName;
    int videoWidth = 1920;
    int videoHeight = 1080;
    int fps = 30;
    int multiviewCount = 4;
    bool showTimeOfDay = false;
    int audioOutputLatencyMs = 0; // playback output-device latency comp, ms (0..500)
    QString midiPortName;
    QMap<int, QPair<int,int>> midiBindings;
    QMap<int, int> midiBindingData2;
    QMap<int, int> midiBindingData2Forward;
    QMap<int, int> midiBindingData2Backward;

    // Stream Deck per-model mappings: model id -> (index -> action id, -1 = unbound).
    QMap<QString, QList<int>> streamDeckKeyMaps;
    QMap<QString, QList<int>> streamDeckDialPressMaps;
    QMap<QString, QList<int>> streamDeckDialRotateMaps;
};

class SettingsManager {
public:
    explicit SettingsManager();
    // Saves settings to a JSON file at the specified path
    bool save(const QString &path, const AppSettings &settings);

    // Loads settings from a JSON file into the provided struct reference
    bool load(const QString &path, AppSettings &settings);
};

#endif // SETTINGSMANAGER_H
