#include "project/projectsettingsimporter.h"

#include <cmath>

#include <QJsonValue>
#include <QSet>
#include <QUrl>

bool ProjectSettingsImporter::isHttpsUrl(const QString &url) {
    const QUrl parsed(url);
    return parsed.isValid() && parsed.scheme() == QStringLiteral("https") && !parsed.host().isEmpty();
}

bool ProjectSettingsImporter::isValidMetadataFields(const QJsonArray &fields, QString *error) {
    for (const QJsonValue &value : fields) {
        if (!value.isObject()) {
            *error = QStringLiteral("metadataFields entries must be objects");
            return false;
        }
        const QJsonObject obj = value.toObject();
        if (obj.value("name").toString().trimmed().isEmpty()) {
            *error = QStringLiteral("metadataFields entries require non-empty name");
            return false;
        }
        if (obj.contains("display") && !obj.value("display").isBool()) {
            *error = QStringLiteral("metadataFields display must be boolean");
            return false;
        }
    }
    return true;
}

bool ProjectSettingsImporter::isValidSourceMetadata(const QJsonArray &metadata, QString *error) {
    for (const QJsonValue &value : metadata) {
        if (!value.isObject()) {
            *error = QStringLiteral("feed metadata entries must be objects");
            return false;
        }
        const QJsonObject obj = value.toObject();
        if (obj.value("k").toString().trimmed().isEmpty()) {
            *error = QStringLiteral("feed metadata entries require non-empty k");
            return false;
        }
        if (!obj.contains("v")) {
            *error = QStringLiteral("feed metadata entries require v");
            return false;
        }
    }
    return true;
}

ProjectSettingsImportResult ProjectSettingsImporter::importJson(
    const QJsonObject &root, const QString &sourceUrl) const {
    ProjectSettingsImportResult result;
    result.importSettingsUrl = sourceUrl;

    const QString schemaVersion = root.value("schemaVersion").toString();
    if (schemaVersion != QStringLiteral("olr.project-settings.v1")) {
        result.error = QStringLiteral("unsupported schemaVersion");
        return result;
    }

    const QJsonObject telemetry = root.value("telemetry").toObject();
    result.telemetrySseUrl = telemetry.value("sseUrl").toString().trimmed();
    if (!isHttpsUrl(result.telemetrySseUrl)) {
        result.error = QStringLiteral("telemetry.sseUrl must be an HTTPS URL");
        return result;
    }

    const QJsonValue metadataFieldsValue = root.value("metadataFields");
    if (!metadataFieldsValue.isUndefined() && !metadataFieldsValue.isArray()) {
        result.error = QStringLiteral("metadataFields must be an array");
        return result;
    }
    result.metadataFields = metadataFieldsValue.toArray();
    if (!isValidMetadataFields(result.metadataFields, &result.error)) {
        return result;
    }

    const QJsonValue projectValue = root.value("project");
    if (!projectValue.isObject()) {
        result.error = QStringLiteral("project must be an object");
        return result;
    }
    const QJsonObject project = projectValue.toObject();
    const QJsonValue projectIdValue = project.value("id");
    if (!projectIdValue.isString() || projectIdValue.toString().trimmed().isEmpty()) {
        result.error = QStringLiteral("project.id must be a non-empty string");
        return result;
    }
    const QJsonValue projectNameValue = project.value("name");
    if (!projectNameValue.isString() || projectNameValue.toString().trimmed().isEmpty()) {
        result.error = QStringLiteral("project.name must be a non-empty string");
        return result;
    }
    result.projectId = projectIdValue.toString().trimmed();
    result.projectName = projectNameValue.toString().trimmed();

    const QJsonArray feeds = root.value("feeds").toArray();
    if (feeds.isEmpty()) {
        result.error = QStringLiteral("feeds must be a non-empty array");
        return result;
    }

    QSet<QString> ids;
    for (const QJsonValue &value : feeds) {
        if (!value.isObject()) {
            result.error = QStringLiteral("feeds entries must be objects");
            return result;
        }
        const QJsonObject obj = value.toObject();
        SourceSettings source;
        source.id = obj.value("id").toString().trimmed();
        source.name = obj.value("name").toString();
        source.url = obj.value("url").toString().trimmed();
        source.trimOffsetMs = obj.value("trimOffsetMs").toInt(0);

        if (source.id.isEmpty()) {
            result.error = QStringLiteral("feed id must be non-empty");
            return result;
        }
        if (ids.contains(source.id)) {
            result.error = QStringLiteral("duplicate feed id: ") + source.id;
            return result;
        }
        ids.insert(source.id);

        if (source.url.isEmpty()) {
            result.error = QStringLiteral("feed url must be non-empty: ") + source.id;
            return result;
        }

        const QJsonValue telemetryDelayValue = obj.value("telemetryDelayMs");
        if (!telemetryDelayValue.isUndefined()) {
            if (!telemetryDelayValue.isDouble()) {
                result.error = QStringLiteral("telemetryDelayMs must be an integer for feed: ") + source.id;
                return result;
            }
            const double delay = telemetryDelayValue.toDouble();
            if (delay != std::trunc(delay)) {
                result.error = QStringLiteral("telemetryDelayMs must be an integer for feed: ") + source.id;
                return result;
            }
            if (delay < 0 || delay > 10000) {
                result.error = QStringLiteral("telemetryDelayMs must be 0..10000 for feed: ") + source.id;
                return result;
            }
            source.telemetryDelayMs = static_cast<int>(delay);
        }

        const QJsonValue metadataValue = obj.value("metadata");
        if (!metadataValue.isUndefined() && !metadataValue.isArray()) {
            result.error = QStringLiteral("feed metadata must be an array for feed: ") + source.id;
            return result;
        }
        source.metadata = metadataValue.toArray();
        if (!isValidSourceMetadata(source.metadata, &result.error)) {
            result.error += QStringLiteral(" for feed: ") + source.id;
            return result;
        }
        result.sources.append(source);
    }

    result.ok = true;
    return result;
}
