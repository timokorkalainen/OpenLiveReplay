#include "settingsmanager.h"
#include <QDebug>

SettingsManager::SettingsManager() {}

bool SettingsManager::save(const QString &path, const AppSettings &settings) {
    QJsonObject root;

    // Save the separate file path strings
    root["saveLocation"] = settings.saveLocation;
    root["fileName"] = settings.fileName;
    root["videoWidth"] = settings.videoWidth;
    root["videoHeight"] = settings.videoHeight;
    root["fps"] = settings.fps;
    root["showTimeOfDay"] = settings.showTimeOfDay;
    root["midiPortName"] = settings.midiPortName;

    QJsonArray midiArray;
    for (auto it = settings.midiBindings.constBegin(); it != settings.midiBindings.constEnd(); ++it) {
        QJsonObject obj;
        obj["action"] = it.key();
        obj["status"] = it.value().first;
        obj["data1"] = it.value().second;
        if (settings.midiBindingData2.contains(it.key())) {
            obj["data2"] = settings.midiBindingData2.value(it.key());
        }
        if (settings.midiBindingData2Forward.contains(it.key())) {
            obj["data2Forward"] = settings.midiBindingData2Forward.value(it.key());
        }
        if (settings.midiBindingData2Backward.contains(it.key())) {
            obj["data2Backward"] = settings.midiBindingData2Backward.value(it.key());
        }
        midiArray.append(obj);
    }
    root["midiBindings"] = midiArray;

    // Convert QStringList to QJsonArray
    QJsonArray urlArray;
    for (const QString &url : settings.streamUrls) {
        urlArray.append(url);
    }
    root["streams"] = urlArray;

    QJsonArray nameArray;
    for (const QString &name : settings.streamNames) {
        nameArray.append(name);
    }
    root["streamNames"] = nameArray;

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
    settings.videoWidth = root["videoWidth"].toInt(settings.videoWidth);
    settings.videoHeight = root["videoHeight"].toInt(settings.videoHeight);
    settings.fps = root["fps"].toInt(settings.fps);
    settings.showTimeOfDay = root["showTimeOfDay"].toBool(settings.showTimeOfDay);
    settings.midiPortName = root["midiPortName"].toString();
    settings.midiBindings.clear();
    settings.midiBindingData2.clear();
    settings.midiBindingData2Forward.clear();
    settings.midiBindingData2Backward.clear();
    QJsonArray midiArray = root["midiBindings"].toArray();
    for (const QJsonValue &val : midiArray) {
        QJsonObject obj = val.toObject();
        int action = obj["action"].toInt(-1);
        int status = obj["status"].toInt(-1);
        int data1 = obj["data1"].toInt(-1);
        int data2 = obj["data2"].toInt(-1);
        int data2Forward = obj["data2Forward"].toInt(-1);
        int data2Backward = obj["data2Backward"].toInt(-1);
        if (action >= 0 && status >= 0 && data1 >= 0) {
            settings.midiBindings.insert(action, qMakePair(status, data1));
            if (data2 >= 0) {
                settings.midiBindingData2.insert(action, data2);
            }
            if (data2Forward >= 0) {
                settings.midiBindingData2Forward.insert(action, data2Forward);
            }
            if (data2Backward >= 0) {
                settings.midiBindingData2Backward.insert(action, data2Backward);
            }
        }
    }

    // Parse the stream list
    settings.streamUrls.clear();
    QJsonArray urlArray = root["streams"].toArray();
    extracted(settings, urlArray);

    settings.streamNames.clear();
    QJsonArray nameArray = root["streamNames"].toArray();
    for (const QJsonValue &val : nameArray) {
        settings.streamNames.append(val.toString());
    }

    return true;
}
