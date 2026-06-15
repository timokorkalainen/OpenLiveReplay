#ifndef PROJECTSETTINGSIMPORTER_H
#define PROJECTSETTINGSIMPORTER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "settingsmanager.h"

struct ProjectSettingsImportResult {
    bool ok = false;
    QString error;
    QString projectId;
    QString projectName;
    QString importSettingsUrl;
    QString telemetrySseUrl;
    QJsonArray metadataFields;
    QList<SourceSettings> sources;
};

class ProjectSettingsImporter {
public:
    ProjectSettingsImportResult importJson(const QJsonObject &root,
                                           const QString &sourceUrl) const;

private:
    static bool isHttpsUrl(const QString &url);
    static bool isValidMetadataFields(const QJsonArray &fields, QString *error);
    static bool isValidSourceMetadata(const QJsonArray &metadata, QString *error);
};

#endif // PROJECTSETTINGSIMPORTER_H
