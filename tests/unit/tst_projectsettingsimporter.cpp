#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "project/projectsettingsimporter.h"

class TestProjectSettingsImporter : public QObject {
    Q_OBJECT
private slots:
    void validSettingsConvertToAppSettingsPatch();
    void duplicateFeedIdsFail();
    void missingTelemetryUrlFails();
    void invalidDelayFails();
    void metadataShapeIsPreserved();
};

static QJsonObject validRoot() {
    return QJsonObject{
        {"schemaVersion", "olr.project-settings.v1"},
        {"project", QJsonObject{{"id", "event-1"}, {"name", "Final"}}},
        {"links", QJsonObject{{"openapi", "https://provider.example/openapi.json"}}},
        {"telemetry", QJsonObject{{"sseUrl", "https://provider.example/telemetry"}}},
        {"metadataFields", QJsonArray{
            QJsonObject{{"name", "angle"}, {"display", true}},
            QJsonObject{{"name", "operator"}, {"display", true}}
        }},
        {"feeds", QJsonArray{
            QJsonObject{
                {"id", "cam-main"},
                {"name", "Main"},
                {"url", "srt://10.0.0.20:9000"},
                {"telemetryDelayMs", 800},
                {"metadata", QJsonArray{
                    QJsonObject{{"k", "angle"}, {"v", "wide"}},
                    QJsonObject{{"k", "operator"}, {"v", "Aino"}}
                }}
            },
            QJsonObject{
                {"id", "cam-reverse"},
                {"name", "Reverse"},
                {"url", "srt://10.0.0.21:9001"},
                {"metadata", QJsonArray{}}
            }
        }}
    };
}

void TestProjectSettingsImporter::validSettingsConvertToAppSettingsPatch() {
    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(validRoot(), QStringLiteral("https://provider.example/project.json"));

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.importSettingsUrl, QStringLiteral("https://provider.example/project.json"));
    QCOMPARE(result.telemetrySseUrl, QStringLiteral("https://provider.example/telemetry"));
    QCOMPARE(result.metadataFields.size(), 2);
    QCOMPARE(result.sources.size(), 2);
    QCOMPARE(result.sources[0].id, QStringLiteral("cam-main"));
    QCOMPARE(result.sources[0].name, QStringLiteral("Main"));
    QCOMPARE(result.sources[0].url, QStringLiteral("srt://10.0.0.20:9000"));
    QCOMPARE(result.sources[0].telemetryDelayMs, 800);
    QCOMPARE(result.sources[1].telemetryDelayMs, 0);
}

void TestProjectSettingsImporter::duplicateFeedIdsFail() {
    QJsonObject root = validRoot();
    QJsonArray feeds = root.value("feeds").toArray();
    QJsonObject second = feeds.at(1).toObject();
    second["id"] = "cam-main";
    feeds[1] = second;
    root["feeds"] = feeds;

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("duplicate feed id")));
}

void TestProjectSettingsImporter::missingTelemetryUrlFails() {
    QJsonObject root = validRoot();
    root["telemetry"] = QJsonObject{};

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("telemetry.sseUrl")));
}

void TestProjectSettingsImporter::invalidDelayFails() {
    QJsonObject root = validRoot();
    QJsonArray feeds = root.value("feeds").toArray();
    QJsonObject first = feeds.at(0).toObject();
    first["telemetryDelayMs"] = 12000;
    feeds[0] = first;
    root["feeds"] = feeds;

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("telemetryDelayMs")));
}

void TestProjectSettingsImporter::metadataShapeIsPreserved() {
    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(validRoot(), QStringLiteral("https://provider.example/project.json"));

    QVERIFY(result.ok);
    QCOMPARE(result.sources[0].metadata,
             QJsonArray({QJsonObject{{"k", "angle"}, {"v", "wide"}},
                         QJsonObject{{"k", "operator"}, {"v", "Aino"}}}));
}

QTEST_GUILESS_MAIN(TestProjectSettingsImporter)
#include "tst_projectsettingsimporter.moc"
