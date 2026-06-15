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
    void importedMetadataUsesUiConsumedShape();
    void providerTrimOffsetIsIgnored();
    void nonArrayMetadataFieldsFails();
    void nonArrayFeedMetadataFails();
    void invalidFeedMetadataEntriesFail_data();
    void invalidFeedMetadataEntriesFail();
    void nonIntegerTelemetryDelayFails_data();
    void nonIntegerTelemetryDelayFails();
    void invalidProjectIdentityFails_data();
    void invalidProjectIdentityFails();
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
                    QJsonObject{{"name", "angle"}, {"value", "wide"}},
                    QJsonObject{{"name", "operator"}, {"value", "Aino"}}
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
             QJsonArray({QJsonObject{{"name", "angle"}, {"value", "wide"}},
                         QJsonObject{{"name", "operator"}, {"value", "Aino"}}}));
}

void TestProjectSettingsImporter::importedMetadataUsesUiConsumedShape() {
    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(validRoot(), QStringLiteral("https://provider.example/project.json"));

    QVERIFY(result.ok);
    const QJsonObject metadata = result.sources[0].metadata.at(0).toObject();
    QCOMPARE(metadata.value("name").toString(), QStringLiteral("angle"));
    QCOMPARE(metadata.value("value").toString(), QStringLiteral("wide"));
    QVERIFY(!metadata.contains("k"));
    QVERIFY(!metadata.contains("v"));
}

void TestProjectSettingsImporter::providerTrimOffsetIsIgnored() {
    QJsonObject root = validRoot();
    QJsonArray feeds = root.value("feeds").toArray();
    QJsonObject first = feeds.at(0).toObject();
    first["trimOffsetMs"] = 250;
    feeds[0] = first;
    root["feeds"] = feeds;

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(result.ok);
    QCOMPARE(result.sources[0].trimOffsetMs, 0);
}

void TestProjectSettingsImporter::nonArrayMetadataFieldsFails() {
    QJsonObject root = validRoot();
    root["metadataFields"] = QJsonObject{};

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("metadataFields")));
}

void TestProjectSettingsImporter::nonArrayFeedMetadataFails() {
    QJsonObject root = validRoot();
    QJsonArray feeds = root.value("feeds").toArray();
    QJsonObject first = feeds.at(0).toObject();
    first["metadata"] = QStringLiteral("bad");
    feeds[0] = first;
    root["feeds"] = feeds;

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("metadata")));
}

void TestProjectSettingsImporter::invalidFeedMetadataEntriesFail_data() {
    QTest::addColumn<QJsonObject>("metadataEntry");

    QTest::newRow("missing-name") << QJsonObject{{"value", "wide"}};
    QTest::newRow("empty-name") << QJsonObject{{"name", ""}, {"value", "wide"}};
    QTest::newRow("non-string-name") << QJsonObject{{"name", 7}, {"value", "wide"}};
    QTest::newRow("missing-value") << QJsonObject{{"name", "angle"}};
    QTest::newRow("non-string-value") << QJsonObject{{"name", "angle"}, {"value", 7}};
    QTest::newRow("legacy-kv-shape") << QJsonObject{{"k", "angle"}, {"v", "wide"}};
}

void TestProjectSettingsImporter::invalidFeedMetadataEntriesFail() {
    QFETCH(QJsonObject, metadataEntry);

    QJsonObject root = validRoot();
    QJsonArray feeds = root.value("feeds").toArray();
    QJsonObject first = feeds.at(0).toObject();
    first["metadata"] = QJsonArray{metadataEntry};
    feeds[0] = first;
    root["feeds"] = feeds;

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("metadata")));
}

void TestProjectSettingsImporter::nonIntegerTelemetryDelayFails_data() {
    QTest::addColumn<QJsonValue>("delayValue");

    QTest::newRow("string") << QJsonValue(QStringLiteral("800"));
    QTest::newRow("object") << QJsonValue(QJsonObject{});
}

void TestProjectSettingsImporter::nonIntegerTelemetryDelayFails() {
    QFETCH(QJsonValue, delayValue);

    QJsonObject root = validRoot();
    QJsonArray feeds = root.value("feeds").toArray();
    QJsonObject first = feeds.at(0).toObject();
    first["telemetryDelayMs"] = delayValue;
    feeds[0] = first;
    root["feeds"] = feeds;

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("telemetryDelayMs")));
}

void TestProjectSettingsImporter::invalidProjectIdentityFails_data() {
    QTest::addColumn<QJsonValue>("projectValue");

    QTest::newRow("missing-project") << QJsonValue(QJsonValue::Undefined);
    QTest::newRow("non-object-project") << QJsonValue(QStringLiteral("event-1"));
    QTest::newRow("missing-id") << QJsonValue(QJsonObject{{"name", "Final"}});
    QTest::newRow("missing-name") << QJsonValue(QJsonObject{{"id", "event-1"}});
    QTest::newRow("empty-id") << QJsonValue(QJsonObject{{"id", ""}, {"name", "Final"}});
    QTest::newRow("non-string-id") << QJsonValue(QJsonObject{{"id", 7}, {"name", "Final"}});
    QTest::newRow("non-string-name") << QJsonValue(QJsonObject{{"id", "event-1"}, {"name", 7}});
}

void TestProjectSettingsImporter::invalidProjectIdentityFails() {
    QFETCH(QJsonValue, projectValue);

    QJsonObject root = validRoot();
    if (projectValue.isUndefined()) {
        root.remove("project");
    } else {
        root["project"] = projectValue;
    }

    ProjectSettingsImporter importer;
    ProjectSettingsImportResult result =
        importer.importJson(root, QStringLiteral("https://provider.example/project.json"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("project")));
}

QTEST_GUILESS_MAIN(TestProjectSettingsImporter)
#include "tst_projectsettingsimporter.moc"
