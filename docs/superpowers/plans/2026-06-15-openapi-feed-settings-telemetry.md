# OpenAPI Feed Settings And Telemetry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a consumer-only OpenAPI-compatible feed settings import flow plus a single project SSE telemetry stream whose feed-keyed events are recorded and replayed from the MKV timeline.

**Architecture:** Keep import validation, SSE parsing, recording storage, and playback lookup in separate testable units. Store dynamic telemetry as dedicated Matroska text subtitle tracks keyed by configured feed ID, separate from existing per-view metadata subtitle tracks. UIManager coordinates import/apply, starts one SSE client during recording, and exposes telemetry-at-playhead to QML.

**Tech Stack:** Qt 6 C++17/QML, Qt Network (`QNetworkAccessManager`), Qt Test, FFmpeg/Matroska, existing CMake test targets.

---

## Scope And Storage Choice

This plan implements the full approved v1 spec in one vertical sequence because the subsystems are coupled: imported settings define the single SSE URL and feed IDs, SSE events must be recorded with those feed IDs, and playback must read those same feed timelines from the file.

Telemetry storage will use one Matroska text subtitle track per configured feed. Each packet payload is compact JSON. Each telemetry track carries stream metadata:

- `title`: `Feed <feedId> Telemetry`
- `olr_track_type`: `feed_telemetry`
- `olr_feed_id`: feed ID
- `olr_feed_name`: feed name

Existing per-view metadata subtitle tracks remain unchanged. New feed telemetry tracks are appended after per-view subtitle tracks.

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `settingsmanager.h/.cpp` | modify | add imported settings URL, project telemetry SSE URL, and per-source `telemetryDelayMs` persistence |
| `project/projectimportclient.h/.cpp` | create | fetch direct HTTPS settings JSON and return bytes/error to UIManager |
| `project/projectsettingsimporter.h/.cpp` | create | validate provider JSON and convert it into OLR settings fields |
| `telemetry/telemetryevent.h` | create | shared telemetry event struct and JSON helpers |
| `telemetry/sseparser.h/.cpp` | create | pure parser for SSE event stream chunks |
| `telemetry/telemetryclient.h/.cpp` | create | Qt Network SSE client, one project stream |
| `playback/telemetrytimelinereader.h/.cpp` | create | read feed telemetry tracks from an MKV and query by playhead |
| `recorder_engine/muxer.h/.cpp` | modify | create feed telemetry tracks and write telemetry packets |
| `recorder_engine/replaymanager.h/.cpp` | modify | accept feed telemetry config and route telemetry events to muxer with receive time + delay |
| `uimanager.h/.cpp` | modify | expose import preview/apply and telemetry-at-playhead to QML; own network client |
| `Main.qml` | modify | add settings URL input, Read button, single-summary preview, Apply button, and compact telemetry display |
| `CMakeLists.txt` | modify | add Qt Network and new source files |
| `tests/CMakeLists.txt` | modify | add Qt Network and new test support sources |
| `tests/unit/CMakeLists.txt` | modify | register new unit tests |
| `tests/unit/tst_projectsettingsimporter.cpp` | create | import validation and replace conversion tests |
| `tests/unit/tst_sseparser.cpp` | create | SSE chunk parsing tests |
| `tests/unit/tst_telemetrytimelinereader.cpp` | create | write/read telemetry track round-trip |
| `tests/e2e/telemetry_harness.cpp` | create | synthetic recording with telemetry packets |
| `tests/e2e/run_telemetry_e2e.sh` | create | record, chase-read, close, reopen, verify same feed timeline |
| `tests/e2e/CMakeLists.txt` | modify | build/register telemetry e2e |

## Build Commands Used In Tasks

Use the same Qt prefix already documented in the repo unless your local kit differs:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build
ctest --test-dir build --output-on-failure
```

For focused tests, build first, then run the named CTest:

```bash
cmake --build build --target tst_projectsettingsimporter
ctest --test-dir build -R '^tst_projectsettingsimporter$' --output-on-failure
```

---

### Task 1: Settings Persistence For Import And Telemetry Delay

**Files:**
- Modify: `settingsmanager.h`
- Modify: `settingsmanager.cpp`
- Modify: `tests/unit/tst_settingsmanager.cpp`

- [ ] **Step 1: Add the failing round-trip assertions**

In `tests/unit/tst_settingsmanager.cpp`, extend `sampleSettings()`:

```cpp
s.importSettingsUrl = QStringLiteral("https://provider.example/project-settings.json");
s.telemetrySseUrl = QStringLiteral("https://provider.example/telemetry");
```

Set delays on both sources:

```cpp
a.telemetryDelayMs = 800;
b.telemetryDelayMs = 1200;
```

In `roundTripPreservesEverything()`, after `showTimeOfDay` comparison, add:

```cpp
QCOMPARE(out.importSettingsUrl, in.importSettingsUrl);
QCOMPARE(out.telemetrySseUrl, in.telemetrySseUrl);
```

After existing trim comparisons, add:

```cpp
QCOMPARE(out.sources[0].telemetryDelayMs, in.sources[0].telemetryDelayMs);
QCOMPARE(out.sources[1].telemetryDelayMs, in.sources[1].telemetryDelayMs);
```

- [ ] **Step 2: Run the focused test to verify it fails**

Run:

```bash
cmake --build build --target tst_settingsmanager
ctest --test-dir build -R '^tst_settingsmanager$' --output-on-failure
```

Expected: compile failure because `AppSettings::importSettingsUrl`, `AppSettings::telemetrySseUrl`, and `SourceSettings::telemetryDelayMs` do not exist.

- [ ] **Step 3: Add settings fields**

In `settingsmanager.h`, update `SourceSettings`:

```cpp
struct SourceSettings {
    QString id;
    QString name;
    QString url;
    QJsonArray metadata;
    int trimOffsetMs = 0; // per-source timeline trim (+delay / -advance), ms
    int telemetryDelayMs = 0; // positive delay so telemetry aligns with video, ms
};
```

In `AppSettings`, add the imported provider fields near source metadata:

```cpp
QList<SourceSettings> sources;
QJsonArray metadataFields; // Global field definitions: [{name, display}]
QString importSettingsUrl;
QString telemetrySseUrl;
QString saveLocation;
```

- [ ] **Step 4: Persist the new settings**

In `SettingsManager::save()`, after `root["metadataFields"] = settings.metadataFields;`, add:

```cpp
root["importSettingsUrl"] = settings.importSettingsUrl;
root["telemetrySseUrl"] = settings.telemetrySseUrl;
```

In the source save loop, after `obj["trimOffsetMs"] = source.trimOffsetMs;`, add:

```cpp
obj["telemetryDelayMs"] = source.telemetryDelayMs;
```

In `SettingsManager::load()`, after `settings.metadataFields = root["metadataFields"].toArray();`, add:

```cpp
settings.importSettingsUrl = root["importSettingsUrl"].toString();
settings.telemetrySseUrl = root["telemetrySseUrl"].toString();
```

In the source load loop, after `source.trimOffsetMs = obj["trimOffsetMs"].toInt(0);`, add:

```cpp
source.telemetryDelayMs = qBound(0, obj["telemetryDelayMs"].toInt(0), 10000);
```

Add `#include <QtGlobal>` to `settingsmanager.cpp` if `qBound` is not already visible.

- [ ] **Step 5: Run the focused test to verify it passes**

Run:

```bash
cmake --build build --target tst_settingsmanager
ctest --test-dir build -R '^tst_settingsmanager$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add settingsmanager.h settingsmanager.cpp tests/unit/tst_settingsmanager.cpp
git commit -m "feat(settings): persist imported telemetry config"
```

---

### Task 2: Project Settings Importer

**Files:**
- Create: `project/projectsettingsimporter.h`
- Create: `project/projectsettingsimporter.cpp`
- Create: `tests/unit/tst_projectsettingsimporter.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Add the failing importer tests**

Create `tests/unit/tst_projectsettingsimporter.cpp`:

```cpp
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
```

- [ ] **Step 2: Wire the test target and verify it fails**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_projectsettingsimporter olr_test_core)
```

In `tests/CMakeLists.txt`, add the new importer source to `olr_test_core`:

```cmake
"${CMAKE_SOURCE_DIR}/project/projectsettingsimporter.cpp"
```

In root `CMakeLists.txt`, add the new source/header to `qt_add_qml_module(... SOURCES ...)`:

```cmake
project/projectsettingsimporter.h project/projectsettingsimporter.cpp
```

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build --target tst_projectsettingsimporter
```

Expected: compile failure because `project/projectsettingsimporter.h` does not exist.

- [ ] **Step 3: Create the importer header**

Create `project/projectsettingsimporter.h`:

```cpp
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
```

- [ ] **Step 4: Create the importer implementation**

Create `project/projectsettingsimporter.cpp`:

```cpp
#include "project/projectsettingsimporter.h"

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

    result.metadataFields = root.value("metadataFields").toArray();
    if (!isValidMetadataFields(result.metadataFields, &result.error)) {
        return result;
    }

    const QJsonObject project = root.value("project").toObject();
    result.projectId = project.value("id").toString();
    result.projectName = project.value("name").toString();

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
        source.metadata = obj.value("metadata").toArray();
        source.trimOffsetMs = obj.value("trimOffsetMs").toInt(0);
        source.telemetryDelayMs = obj.value("telemetryDelayMs").toInt(0);

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
        if (source.telemetryDelayMs < 0 || source.telemetryDelayMs > 10000) {
            result.error = QStringLiteral("telemetryDelayMs must be 0..10000 for feed: ") + source.id;
            return result;
        }
        if (!isValidSourceMetadata(source.metadata, &result.error)) {
            result.error += QStringLiteral(" for feed: ") + source.id;
            return result;
        }
        result.sources.append(source);
    }

    result.ok = true;
    return result;
}
```

- [ ] **Step 5: Run the importer tests**

Run:

```bash
cmake --build build --target tst_projectsettingsimporter
ctest --test-dir build -R '^tst_projectsettingsimporter$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
        project/projectsettingsimporter.h project/projectsettingsimporter.cpp \
        tests/unit/tst_projectsettingsimporter.cpp
git commit -m "feat(import): validate project settings JSON"
```

---

### Task 3: Telemetry Event Model And SSE Parser

**Files:**
- Create: `telemetry/telemetryevent.h`
- Create: `telemetry/sseparser.h`
- Create: `telemetry/sseparser.cpp`
- Create: `tests/unit/tst_sseparser.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Add the failing parser tests**

Create `tests/unit/tst_sseparser.cpp`:

```cpp
#include <QtTest>
#include <QJsonObject>

#include "telemetry/sseparser.h"

class TestSseParser : public QObject {
    Q_OBJECT
private slots:
    void parsesSingleJsonDataEvent();
    void parsesChunkedEvent();
    void ignoresCommentsAndKeepsEventType();
    void rejectsMalformedJson();
};

void TestSseParser::parsesSingleJsonDataEvent() {
    SseParser parser;
    const QByteArray chunk =
        "event: telemetry\n"
        "data: {\"feedId\":\"cam-main\",\"status\":\"ok\",\"values\":{\"batteryPercent\":91}}\n"
        "\n";

    const QList<TelemetryEvent> events = parser.push(chunk);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QCOMPARE(events[0].eventType, QStringLiteral("telemetry"));
    QCOMPARE(events[0].payload.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(events[0].payload.value("values").toObject().value("batteryPercent").toInt(), 91);
}

void TestSseParser::parsesChunkedEvent() {
    SseParser parser;
    QVERIFY(parser.push("data: {\"feed").isEmpty());
    const QList<TelemetryEvent> events = parser.push("Id\":\"cam-main\",\"values\":{}}\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
}

void TestSseParser::ignoresCommentsAndKeepsEventType() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push(
        ": heartbeat\n"
        "event: telemetry\n"
        "id: 42\n"
        "data: {\"feedId\":\"cam-main\",\"values\":{}}\n"
        "\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].eventType, QStringLiteral("telemetry"));
    QCOMPARE(events[0].lastEventId, QStringLiteral("42"));
}

void TestSseParser::rejectsMalformedJson() {
    SseParser parser;
    const QList<TelemetryEvent> events = parser.push("data: { nope }\n\n");
    QVERIFY(events.isEmpty());
    QVERIFY(parser.lastError().contains(QStringLiteral("JSON")));
}

QTEST_GUILESS_MAIN(TestSseParser)
#include "tst_sseparser.moc"
```

- [ ] **Step 2: Wire the test and verify it fails**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_sseparser olr_test_core)
```

In `tests/CMakeLists.txt`, add to `olr_test_core`:

```cmake
"${CMAKE_SOURCE_DIR}/telemetry/sseparser.cpp"
```

In root `CMakeLists.txt`, add to application sources:

```cmake
telemetry/telemetryevent.h
telemetry/sseparser.h telemetry/sseparser.cpp
```

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build --target tst_sseparser
```

Expected: compile failure because telemetry headers do not exist.

- [ ] **Step 3: Create `TelemetryEvent`**

Create `telemetry/telemetryevent.h`:

```cpp
#ifndef TELEMETRYEVENT_H
#define TELEMETRYEVENT_H

#include <QJsonObject>
#include <QString>

struct TelemetryEvent {
    QString feedId;
    QString eventType;
    QString lastEventId;
    QJsonObject payload;
};

#endif // TELEMETRYEVENT_H
```

- [ ] **Step 4: Create the parser header**

Create `telemetry/sseparser.h`:

```cpp
#ifndef SSEPARSER_H
#define SSEPARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

#include "telemetry/telemetryevent.h"

class SseParser {
public:
    QList<TelemetryEvent> push(const QByteArray &chunk);
    QString lastError() const { return m_lastError; }
    void reset();

private:
    QList<TelemetryEvent> parseBufferedEvents();
    void parseEventBlock(const QByteArray &block, QList<TelemetryEvent> *events);

    QByteArray m_buffer;
    QString m_lastError;
};

#endif // SSEPARSER_H
```

- [ ] **Step 5: Create the parser implementation**

Create `telemetry/sseparser.cpp`:

```cpp
#include "telemetry/sseparser.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

void SseParser::reset() {
    m_buffer.clear();
    m_lastError.clear();
}

QList<TelemetryEvent> SseParser::push(const QByteArray &chunk) {
    m_buffer.append(chunk);
    return parseBufferedEvents();
}

QList<TelemetryEvent> SseParser::parseBufferedEvents() {
    QList<TelemetryEvent> events;
    for (;;) {
        int sep = m_buffer.indexOf("\n\n");
        int sepLen = 2;
        const int crlfSep = m_buffer.indexOf("\r\n\r\n");
        if (crlfSep >= 0 && (sep < 0 || crlfSep < sep)) {
            sep = crlfSep;
            sepLen = 4;
        }
        if (sep < 0) break;

        const QByteArray block = m_buffer.left(sep);
        m_buffer.remove(0, sep + sepLen);
        parseEventBlock(block, &events);
    }
    return events;
}

void SseParser::parseEventBlock(const QByteArray &block, QList<TelemetryEvent> *events) {
    QByteArray data;
    QString eventType;
    QString lastEventId;

    const QList<QByteArray> lines = block.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty() || line.startsWith(':')) continue;

        const int colon = line.indexOf(':');
        const QByteArray field = colon >= 0 ? line.left(colon) : line;
        QByteArray value = colon >= 0 ? line.mid(colon + 1) : QByteArray();
        if (value.startsWith(' ')) value.remove(0, 1);

        if (field == "event") {
            eventType = QString::fromUtf8(value);
        } else if (field == "id") {
            lastEventId = QString::fromUtf8(value);
        } else if (field == "data") {
            if (!data.isEmpty()) data.append('\n');
            data.append(value);
        }
    }

    if (data.isEmpty()) return;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        m_lastError = QStringLiteral("SSE data JSON parse error: ") + parseError.errorString();
        return;
    }

    const QJsonObject payload = doc.object();
    const QString feedId = payload.value("feedId").toString().trimmed();
    if (feedId.isEmpty()) {
        m_lastError = QStringLiteral("SSE telemetry event missing feedId");
        return;
    }

    TelemetryEvent event;
    event.feedId = feedId;
    event.eventType = eventType.isEmpty() ? QStringLiteral("message") : eventType;
    event.lastEventId = lastEventId;
    event.payload = payload;
    events->append(event);
}
```

- [ ] **Step 6: Run parser tests**

Run:

```bash
cmake --build build --target tst_sseparser
ctest --test-dir build -R '^tst_sseparser$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
        telemetry/telemetryevent.h telemetry/sseparser.h telemetry/sseparser.cpp \
        tests/unit/tst_sseparser.cpp
git commit -m "feat(telemetry): parse SSE JSON events"
```

---

### Task 4: Feed Telemetry Tracks In The Muxer

**Files:**
- Modify: `recorder_engine/muxer.h`
- Modify: `recorder_engine/muxer.cpp`
- Modify: `tests/unit/tst_muxer.cpp`

- [ ] **Step 1: Add failing muxer tests for telemetry tracks**

In `tests/unit/tst_muxer.cpp`, add private slots:

```cpp
void initBuildsTelemetryTrackLayout();
void writeTelemetryPacketAcceptsFeedIndex();
```

Add the tests:

```cpp
void TestMuxer::initBuildsTelemetryTrackLayout() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList viewNames{QStringLiteral("View 1"), QStringLiteral("View 2")};
    const QStringList feedIds{QStringLiteral("cam-main"), QStringLiteral("cam-reverse"), QStringLiteral("cam-overhead")};
    const QStringList feedNames{QStringLiteral("Main"), QStringLiteral("Reverse"), QStringLiteral("Overhead")};

    QVERIFY(m.init(QStringLiteral("olr_unit_telemetry_layout"), 2, 640, 480, 30,
                   viewNames, feedIds, feedNames, 48000, 2));

    QCOMPARE(m.audioTrackOffset(), 2);
    QCOMPARE(m.subtitleTrackOffset(), 4);
    QCOMPARE(m.telemetryTrackOffset(), 6);
    QVERIFY(m.getStream(8) != nullptr);
    QVERIFY(m.getStream(9) == nullptr);

    AVStream *telemetry = m.getStream(m.telemetryTrackOffset());
    QVERIFY(telemetry != nullptr);
    QCOMPARE(QString::fromUtf8(av_dict_get(telemetry->metadata, "olr_track_type", nullptr, 0)->value),
             QStringLiteral("feed_telemetry"));
    QCOMPARE(QString::fromUtf8(av_dict_get(telemetry->metadata, "olr_feed_id", nullptr, 0)->value),
             QStringLiteral("cam-main"));
    m.close();
}

void TestMuxer::writeTelemetryPacketAcceptsFeedIndex() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList viewNames{QStringLiteral("View 1")};
    const QStringList feedIds{QStringLiteral("cam-main")};
    const QStringList feedNames{QStringLiteral("Main")};

    QVERIFY(m.init(QStringLiteral("olr_unit_telemetry_packet"), 1, 320, 240, 30,
                   viewNames, feedIds, feedNames, 48000, 2));
    m.writeTelemetryPacket(0, 1234, QByteArrayLiteral("{\"feedId\":\"cam-main\",\"values\":{\"batteryPercent\":91}}"));
    m.writeTelemetryPacket(1, 1234, QByteArrayLiteral("{\"feedId\":\"unknown\"}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_telemetry_packet")));
    QVERIFY(fi.exists());
    QVERIFY(fi.size() > 0);
}
```

- [ ] **Step 2: Run the muxer test to verify it fails**

Run:

```bash
cmake --build build --target tst_muxer
ctest --test-dir build -R '^tst_muxer$' --output-on-failure
```

Expected: compile failure because `Muxer::telemetryTrackOffset()` and the extended `init()` signature do not exist.

- [ ] **Step 3: Extend the muxer interface**

In `recorder_engine/muxer.h`, change `init` to:

```cpp
bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
          const QStringList& streamNames,
          int audioSampleRate = 48000, int audioChannels = 2);

bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
          const QStringList& streamNames,
          const QStringList& telemetryFeedIds,
          const QStringList& telemetryFeedNames,
          int audioSampleRate = 48000, int audioChannels = 2);
```

Add:

```cpp
void writeTelemetryPacket(int feedIndex, int64_t ptsMs, const QByteArray& jsonData);
int telemetryTrackOffset() const { return m_telemetryTrackOffset; }
```

Add private members:

```cpp
int m_telemetryTrackOffset = 0; // Index of first per-feed telemetry track
int m_telemetryTrackCount = 0;
```

- [ ] **Step 4: Create telemetry tracks in `Muxer::init`**

Keep the existing implementation as the compatibility overload:

```cpp
bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps,
                 const QStringList& streamNames, int audioSampleRate, int audioChannels) {
    return init(filename, videoTrackCount, width, height, fps,
                streamNames, QStringList{}, QStringList{}, audioSampleRate, audioChannels);
}
```

Move the current `init` body into the new telemetry-aware overload:

```cpp
bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps,
                 const QStringList& streamNames,
                 const QStringList& telemetryFeedIds,
                 const QStringList& telemetryFeedNames,
                 int audioSampleRate, int audioChannels) {
```

After the existing per-view subtitle track loop, add:

```cpp
// 2c. Add one text subtitle track per configured feed for dynamic telemetry.
m_telemetryTrackOffset = m_subtitleTrackOffset + videoTrackCount;
m_telemetryTrackCount = telemetryFeedIds.size();
for (int i = 0; i < telemetryFeedIds.size(); ++i) {
    AVStream* st = avformat_new_stream(m_outCtx, nullptr);
    st->id = m_telemetryTrackOffset + i;
    st->codecpar->codec_id = AV_CODEC_ID_TEXT;
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->time_base = {1, 1000};

    const QString feedId = telemetryFeedIds.at(i);
    const QString feedName = i < telemetryFeedNames.size() ? telemetryFeedNames.at(i) : QString();
    const QString title = QString("Feed %1 Telemetry").arg(feedId);
    av_dict_set(&st->metadata, "title", title.toUtf8().constData(), 0);
    av_dict_set(&st->metadata, "olr_track_type", "feed_telemetry", 0);
    av_dict_set(&st->metadata, "olr_feed_id", feedId.toUtf8().constData(), 0);
    av_dict_set(&st->metadata, "olr_feed_name", feedName.toUtf8().constData(), 0);
}
```

In `close()`, after `m_initialized = false;`, reset:

```cpp
m_telemetryTrackOffset = 0;
m_telemetryTrackCount = 0;
```

- [ ] **Step 5: Implement `writeTelemetryPacket`**

In `recorder_engine/muxer.cpp`, add:

```cpp
void Muxer::writeTelemetryPacket(int feedIndex, int64_t ptsMs, const QByteArray& jsonData) {
    if (!m_initialized || !m_outCtx || jsonData.isEmpty()) return;
    if (feedIndex < 0 || feedIndex >= m_telemetryTrackCount) return;

    const int trackIndex = m_telemetryTrackOffset + feedIndex;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_outCtx->nb_streams)) return;

    AVStream* st = m_outCtx->streams[trackIndex];
    if (!st) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    if (av_new_packet(pkt, jsonData.size()) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, jsonData.constData(), jsonData.size());

    pkt->stream_index = trackIndex;
    pkt->pts = av_rescale_q(ptsMs, {1, 1000}, st->time_base);
    pkt->dts = pkt->pts;
    pkt->duration = av_rescale_q(1, {1, 1000}, st->time_base);

    writePacket(pkt);
    av_packet_free(&pkt);
}
```

- [ ] **Step 6: Preserve existing `init()` call sites**

Run the build after adding the compatibility overload. Existing calls such as
`m.init(name, 1, 320, 240, 30, names, 48000, 2)` must continue compiling and must
call the no-telemetry overload. Update only the new telemetry tests and
`ReplayManager::startRecording()` later in Task 6 to use the telemetry-aware
overload.

- [ ] **Step 7: Run muxer tests**

Run:

```bash
cmake --build build --target tst_muxer
ctest --test-dir build -R '^tst_muxer$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add recorder_engine/muxer.h recorder_engine/muxer.cpp tests/unit/tst_muxer.cpp
git commit -m "feat(muxer): add feed telemetry tracks"
```

---

### Task 5: Telemetry Timeline Reader

**Files:**
- Create: `playback/telemetrytimelinereader.h`
- Create: `playback/telemetrytimelinereader.cpp`
- Create: `tests/unit/tst_telemetrytimelinereader.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Add the failing reader test**

Create `tests/unit/tst_telemetrytimelinereader.cpp`:

```cpp
#include <QtTest>
#include <QTemporaryDir>

#include "recorder_engine/muxer.h"
#include "playback/telemetrytimelinereader.h"

class TestTelemetryTimelineReader : public QObject {
    Q_OBJECT
private slots:
    void readsFeedTelemetryByPlayhead();
};

void TestTelemetryTimelineReader::readsFeedTelemetryByPlayhead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(dir.path());
    const QStringList viewNames{QStringLiteral("View 1")};
    const QStringList feedIds{QStringLiteral("cam-main"), QStringLiteral("cam-reverse")};
    const QStringList feedNames{QStringLiteral("Main"), QStringLiteral("Reverse")};
    QVERIFY(muxer.init(QStringLiteral("telemetry_reader"), 1, 320, 240, 30,
                       viewNames, feedIds, feedNames, 48000, 2));

    muxer.writeTelemetryPacket(0, 100, QByteArrayLiteral("{\"feedId\":\"cam-main\",\"olrEffectiveMs\":100,\"values\":{\"batteryPercent\":90}}"));
    muxer.writeTelemetryPacket(0, 300, QByteArrayLiteral("{\"feedId\":\"cam-main\",\"olrEffectiveMs\":300,\"values\":{\"batteryPercent\":88}}"));
    muxer.writeTelemetryPacket(1, 200, QByteArrayLiteral("{\"feedId\":\"cam-reverse\",\"olrEffectiveMs\":200,\"values\":{\"signalDb\":-61}}"));
    muxer.close();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(dir.filePath(QStringLiteral("telemetry_reader.mkv"))), qPrintable(reader.lastError()));

    QVariantMap at250 = reader.stateAt(250);
    QCOMPARE(at250.value("cam-main").toMap().value("values").toMap().value("batteryPercent").toInt(), 90);
    QCOMPARE(at250.value("cam-reverse").toMap().value("values").toMap().value("signalDb").toInt(), -61);

    QVariantMap at350 = reader.stateAt(350);
    QCOMPARE(at350.value("cam-main").toMap().value("values").toMap().value("batteryPercent").toInt(), 88);
}

QTEST_GUILESS_MAIN(TestTelemetryTimelineReader)
#include "tst_telemetrytimelinereader.moc"
```

- [ ] **Step 2: Wire the test and verify it fails**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_telemetrytimelinereader olr_test_engine olr_test_playback)
```

In `tests/CMakeLists.txt`, add to `olr_test_playback`:

```cmake
"${CMAKE_SOURCE_DIR}/playback/telemetrytimelinereader.cpp"
```

In root `CMakeLists.txt`, add:

```cmake
playback/telemetrytimelinereader.h playback/telemetrytimelinereader.cpp
```

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build --target tst_telemetrytimelinereader
```

Expected: compile failure because the reader does not exist.

- [ ] **Step 3: Create the reader header**

Create `playback/telemetrytimelinereader.h`:

```cpp
#ifndef TELEMETRYTIMELINEREADER_H
#define TELEMETRYTIMELINEREADER_H

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QVariantMap>

class TelemetryTimelineReader {
public:
    bool load(const QString &filePath);
    QVariantMap stateAt(qint64 playheadMs) const;
    QString lastError() const { return m_lastError; }
    QStringList feedIds() const { return m_events.keys(); }

private:
    struct Entry {
        qint64 ptsMs = 0;
        QJsonObject payload;
    };

    QString m_lastError;
    QMap<QString, QList<Entry>> m_events;
};

#endif // TELEMETRYTIMELINEREADER_H
```

- [ ] **Step 4: Create the reader implementation**

Create `playback/telemetrytimelinereader.cpp`:

```cpp
#include "playback/telemetrytimelinereader.h"

#include <QJsonDocument>
#include <QJsonParseError>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

bool TelemetryTimelineReader::load(const QString &filePath) {
    m_lastError.clear();
    m_events.clear();

    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        m_lastError = QStringLiteral("failed to open telemetry file");
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        m_lastError = QStringLiteral("failed to read stream info");
        return false;
    }

    QMap<int, QString> streamToFeed;
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        AVStream *stream = fmt->streams[i];
        AVDictionaryEntry *type = av_dict_get(stream->metadata, "olr_track_type", nullptr, 0);
        AVDictionaryEntry *feed = av_dict_get(stream->metadata, "olr_feed_id", nullptr, 0);
        if (type && feed && QString::fromUtf8(type->value) == QStringLiteral("feed_telemetry")) {
            streamToFeed.insert(static_cast<int>(i), QString::fromUtf8(feed->value));
        }
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&fmt);
        m_lastError = QStringLiteral("failed to allocate packet");
        return false;
    }

    while (av_read_frame(fmt, pkt) >= 0) {
        const QString feedId = streamToFeed.value(pkt->stream_index);
        if (!feedId.isEmpty() && pkt->size > 0) {
            QJsonParseError parseError;
            const QByteArray bytes(reinterpret_cast<const char*>(pkt->data), pkt->size);
            const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                AVStream *stream = fmt->streams[pkt->stream_index];
                Entry entry;
                entry.ptsMs = av_rescale_q(pkt->pts, stream->time_base, AVRational{1, 1000});
                entry.payload = doc.object();
                m_events[feedId].append(entry);
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return true;
}

QVariantMap TelemetryTimelineReader::stateAt(qint64 playheadMs) const {
    QVariantMap result;
    for (auto it = m_events.constBegin(); it != m_events.constEnd(); ++it) {
        const QList<Entry> &entries = it.value();
        const Entry *latest = nullptr;
        for (const Entry &entry : entries) {
            if (entry.ptsMs <= playheadMs) {
                latest = &entry;
            } else {
                break;
            }
        }
        if (latest) {
            result.insert(it.key(), latest->payload.toVariantMap());
        }
    }
    return result;
}
```

- [ ] **Step 5: Run reader tests**

Run:

```bash
cmake --build build --target tst_telemetrytimelinereader
ctest --test-dir build -R '^tst_telemetrytimelinereader$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
        playback/telemetrytimelinereader.h playback/telemetrytimelinereader.cpp \
        tests/unit/tst_telemetrytimelinereader.cpp
git commit -m "feat(playback): read recorded telemetry timelines"
```

---

### Task 6: ReplayManager Telemetry Recording API

**Files:**
- Modify: `recorder_engine/replaymanager.h`
- Modify: `recorder_engine/replaymanager.cpp`
- Create: `tests/unit/tst_replaymanager_telemetry.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Add the failing ReplayManager telemetry test**

Create `tests/unit/tst_replaymanager_telemetry.cpp`:

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "recorder_engine/replaymanager.h"
#include "playback/telemetrytimelinereader.h"

class TestReplayManagerTelemetry : public QObject {
    Q_OBJECT
private slots:
    void recordsTelemetryWithFeedDelay();
};

void TestReplayManagerTelemetry::recordsTelemetryWithFeedDelay() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ReplayManager rm;
    rm.setOutputDirectory(dir.path());
    rm.setBaseFileName(QStringLiteral("telemetry_rm"));
    rm.setVideoWidth(320);
    rm.setVideoHeight(240);
    rm.setFps(30);
    rm.setSourceUrls(QStringList{QStringLiteral("testsrc://unused")});
    rm.setSourceNames(QStringList{QStringLiteral("Main")});
    rm.setViewCount(1);
    rm.setViewNames(QStringList{QStringLiteral("Main")});
    rm.updateViewMapping(QList<int>{0});
    rm.setTelemetryFeeds(QStringList{QStringLiteral("cam-main")},
                         QStringList{QStringLiteral("Main")},
                         QList<int>{800});

    rm.startRecording();
    QVERIFY(rm.isRecording());
    QTest::qWait(30);

    QJsonObject payload{
        {"feedId", "cam-main"},
        {"status", "ok"},
        {"values", QJsonObject{{"batteryPercent", 91}}}
    };
    QVERIFY(rm.recordTelemetryEvent(QStringLiteral("cam-main"), payload));
    QTest::qWait(30);
    rm.stopRecording();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(rm.getVideoPath()), qPrintable(reader.lastError()));
    const QVariantMap beforeDelay = reader.stateAt(500);
    QVERIFY(!beforeDelay.contains(QStringLiteral("cam-main")));
    const QVariantMap afterDelay = reader.stateAt(1200);
    QCOMPARE(afterDelay.value("cam-main").toMap().value("values").toMap().value("batteryPercent").toInt(), 91);
}

QTEST_GUILESS_MAIN(TestReplayManagerTelemetry)
#include "tst_replaymanager_telemetry.moc"
```

- [ ] **Step 2: Wire the test and verify it fails**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_replaymanager_telemetry olr_test_engine olr_test_playback)
```

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build --target tst_replaymanager_telemetry
```

Expected: compile failure because `ReplayManager::setTelemetryFeeds()` and `recordTelemetryEvent()` do not exist.

- [ ] **Step 3: Add ReplayManager telemetry API**

In `recorder_engine/replaymanager.h`, add public methods near source configuration:

```cpp
void setTelemetryFeeds(const QStringList &feedIds,
                       const QStringList &feedNames,
                       const QList<int> &telemetryDelaysMs);
bool recordTelemetryEvent(const QString &feedId, const QJsonObject &payload);
```

Add private members:

```cpp
QStringList m_telemetryFeedIds;
QStringList m_telemetryFeedNames;
QList<int> m_telemetryDelaysMs;
QHash<QString, int> m_telemetryFeedIndexById;
```

Add includes:

```cpp
#include <QHash>
#include <QJsonObject>
```

- [ ] **Step 4: Pass telemetry tracks to the muxer**

In `ReplayManager::startRecording()`, replace the muxer init call with:

```cpp
if (!m_muxer->init(m_sessionFileName, m_viewCount, m_videoWidth, m_videoHeight, m_fps,
                   m_viewNames, m_telemetryFeedIds, m_telemetryFeedNames)) {
    qDebug() << "ReplayManager: Failed to init Muxer with base name" << m_sessionFileName;
    return;
}
```

- [ ] **Step 5: Implement telemetry config and event recording**

In `recorder_engine/replaymanager.cpp`, add:

```cpp
void ReplayManager::setTelemetryFeeds(const QStringList &feedIds,
                                      const QStringList &feedNames,
                                      const QList<int> &telemetryDelaysMs) {
    m_telemetryFeedIds = feedIds;
    m_telemetryFeedNames = feedNames;
    m_telemetryDelaysMs = telemetryDelaysMs;
    m_telemetryFeedIndexById.clear();
    for (int i = 0; i < m_telemetryFeedIds.size(); ++i) {
        m_telemetryFeedIndexById.insert(m_telemetryFeedIds.at(i), i);
    }
}

bool ReplayManager::recordTelemetryEvent(const QString &feedId, const QJsonObject &payload) {
    if (!m_isRecording || !m_clock || !m_muxer) return false;
    const int feedIndex = m_telemetryFeedIndexById.value(feedId, -1);
    if (feedIndex < 0) return false;

    const int64_t receiveMs = qMax<int64_t>(0, m_clock->elapsedMs());
    const int delayMs = feedIndex < m_telemetryDelaysMs.size()
        ? qBound(0, m_telemetryDelaysMs.at(feedIndex), 10000)
        : 0;
    const int64_t effectiveMs = receiveMs + delayMs;

    QJsonObject recorded = payload;
    recorded.insert("feedId", feedId);
    recorded.insert("olrReceiveMs", static_cast<qint64>(receiveMs));
    recorded.insert("olrEffectiveMs", static_cast<qint64>(effectiveMs));
    recorded.insert("olrTelemetryDelayMs", delayMs);

    const QByteArray bytes = QJsonDocument(recorded).toJson(QJsonDocument::Compact);
    m_muxer->writeTelemetryPacket(feedIndex, effectiveMs, bytes);
    return true;
}
```

Add includes to `replaymanager.cpp`:

```cpp
#include <QJsonDocument>
#include <QtGlobal>
```

- [ ] **Step 6: Run ReplayManager telemetry test**

Run:

```bash
cmake --build build --target tst_replaymanager_telemetry
ctest --test-dir build -R '^tst_replaymanager_telemetry$' --output-on-failure
```

Expected: PASS. If `testsrc://unused` causes worker noise but recording still starts because the muxer initializes, the test is acceptable; the telemetry track is independent of live video connection.

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp \
        tests/unit/CMakeLists.txt tests/unit/tst_replaymanager_telemetry.cpp
git commit -m "feat(recording): record feed telemetry events"
```

---

### Task 7: Qt Network SSE Client

**Files:**
- Create: `telemetry/telemetryclient.h`
- Create: `telemetry/telemetryclient.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add Qt Network to build files**

In root `CMakeLists.txt`, change:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2)
```

to:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2 Network)
```

Add `Qt6::Network` to `target_link_libraries(OpenLiveReplay ...)` if the target link block is explicit later in the file. If the QML module target inherits through the executable target, add:

```cmake
target_link_libraries(OpenLiveReplay PRIVATE Qt6::Network)
```

In `tests/CMakeLists.txt`, change:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent)
```

to:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent Network)
```

Add `Qt6::Network` to `olr_test_core` link libraries.

- [ ] **Step 2: Create the telemetry client header**

Create `telemetry/telemetryclient.h`:

```cpp
#ifndef TELEMETRYCLIENT_H
#define TELEMETRYCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>

#include "telemetry/sseparser.h"

class TelemetryClient : public QObject {
    Q_OBJECT
public:
    explicit TelemetryClient(QObject *parent = nullptr);

    void start(const QUrl &url);
    void stop();
    bool running() const { return m_reply != nullptr; }
    QString lastError() const { return m_lastError; }

signals:
    void telemetryEvent(const TelemetryEvent &event);
    void connectedChanged(bool connected);
    void errorOccurred(const QString &message);

private slots:
    void onReadyRead();
    void onFinished();
    void onError(QNetworkReply::NetworkError code);

private:
    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    SseParser m_parser;
    QString m_lastError;
};

#endif // TELEMETRYCLIENT_H
```

- [ ] **Step 3: Create the telemetry client implementation**

Create `telemetry/telemetryclient.cpp`:

```cpp
#include "telemetry/telemetryclient.h"

#include <QNetworkRequest>

TelemetryClient::TelemetryClient(QObject *parent)
    : QObject(parent) {
}

void TelemetryClient::start(const QUrl &url) {
    stop();
    m_lastError.clear();
    m_parser.reset();

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Cache-Control", "no-cache");
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &TelemetryClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &TelemetryClient::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &TelemetryClient::onError);
    emit connectedChanged(true);
}

void TelemetryClient::stop() {
    if (!m_reply) return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    emit connectedChanged(false);
}

void TelemetryClient::onReadyRead() {
    if (!m_reply) return;
    const QList<TelemetryEvent> events = m_parser.push(m_reply->readAll());
    for (const TelemetryEvent &event : events) {
        emit telemetryEvent(event);
    }
    if (!m_parser.lastError().isEmpty()) {
        m_lastError = m_parser.lastError();
        emit errorOccurred(m_lastError);
    }
}

void TelemetryClient::onFinished() {
    if (!m_reply) return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    emit connectedChanged(false);
}

void TelemetryClient::onError(QNetworkReply::NetworkError) {
    if (!m_reply) return;
    m_lastError = m_reply->errorString();
    emit errorOccurred(m_lastError);
}
```

- [ ] **Step 4: Add files to app/test source lists**

In root `CMakeLists.txt`, add:

```cmake
telemetry/telemetryclient.h telemetry/telemetryclient.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_core`:

```cmake
"${CMAKE_SOURCE_DIR}/telemetry/telemetryclient.cpp"
```

- [ ] **Step 5: Build app and focused parser tests**

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build --target OpenLiveReplay tst_sseparser
ctest --test-dir build -R '^tst_sseparser$' --output-on-failure
```

Expected: build succeeds and parser test still passes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt \
        telemetry/telemetryclient.h telemetry/telemetryclient.cpp
git commit -m "feat(telemetry): add SSE network client"
```

---

### Task 8: UIManager Import Apply And Telemetry Recording Integration

**Files:**
- Create: `project/projectimportclient.h`
- Create: `project/projectimportclient.cpp`
- Modify: `uimanager.h`
- Modify: `uimanager.cpp`
- Modify: `recorder_engine/replaymanager.h/.cpp` if helper accessors are needed
- Modify: `tests/unit/tst_projectsettingsimporter.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `ProjectImportClient`**

Create `project/projectimportclient.h`:

```cpp
#ifndef PROJECTIMPORTCLIENT_H
#define PROJECTIMPORTCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>

class ProjectImportClient : public QObject {
    Q_OBJECT
public:
    explicit ProjectImportClient(QObject *parent = nullptr);
    void fetch(const QUrl &url);

signals:
    void finished(const QByteArray &body, const QString &finalUrl);
    void failed(const QString &message);

private:
    QNetworkAccessManager m_network;
};

#endif // PROJECTIMPORTCLIENT_H
```

Create `project/projectimportclient.cpp`:

```cpp
#include "project/projectimportclient.h"

#include <QNetworkRequest>

ProjectImportClient::ProjectImportClient(QObject *parent)
    : QObject(parent) {
}

void ProjectImportClient::fetch(const QUrl &url) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
        emit failed(QStringLiteral("Import settings URL must be HTTPS"));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorText = reply->errorString();
        const QString finalUrl = reply->url().toString();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            emit failed(errorText);
            return;
        }
        emit finished(body, finalUrl);
    });
}
```

In root `CMakeLists.txt`, add:

```cmake
project/projectimportclient.h project/projectimportclient.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_core`:

```cmake
"${CMAKE_SOURCE_DIR}/project/projectimportclient.cpp"
```

- [ ] **Step 2: Add UIManager properties and invokables**

In `uimanager.h`, include:

```cpp
#include "project/projectimportclient.h"
#include "project/projectsettingsimporter.h"
#include "telemetry/telemetryclient.h"
```

Add Q_PROPERTY entries:

```cpp
Q_PROPERTY(QString importSettingsUrl READ importSettingsUrl WRITE setImportSettingsUrl NOTIFY importSettingsUrlChanged)
Q_PROPERTY(QString importPreviewError READ importPreviewError NOTIFY importPreviewChanged)
Q_PROPERTY(QVariantMap importPreview READ importPreview NOTIFY importPreviewChanged)
Q_PROPERTY(bool importPreviewReady READ importPreviewReady NOTIFY importPreviewChanged)
Q_PROPERTY(QString telemetrySseUrl READ telemetrySseUrl NOTIFY telemetryConfigChanged)
Q_PROPERTY(int telemetryVersion READ telemetryVersion NOTIFY telemetryChanged)
```

Add getters/invokables:

```cpp
QString importSettingsUrl() const;
void setImportSettingsUrl(const QString &url);
QString importPreviewError() const { return m_importPreviewError; }
QVariantMap importPreview() const { return m_importPreview; }
bool importPreviewReady() const { return m_pendingImport.ok; }
QString telemetrySseUrl() const { return m_currentSettings.telemetrySseUrl; }
int telemetryVersion() const { return m_telemetryVersion; }

Q_INVOKABLE void readImportSettings();
Q_INVOKABLE void applyImportPreview();
Q_INVOKABLE QVariantMap telemetryAtPlayhead() const;
```

Add signals:

```cpp
void importSettingsUrlChanged();
void importPreviewChanged();
void telemetryConfigChanged();
void telemetryChanged();
```

Add private members:

```cpp
ProjectSettingsImportResult m_pendingImport;
QVariantMap m_importPreview;
QString m_importPreviewError;
TelemetryClient *m_telemetryClient = nullptr;
ProjectImportClient *m_projectImportClient = nullptr;
QVariantMap m_liveTelemetryByFeed;
int m_telemetryVersion = 0;
```

- [ ] **Step 3: Construct and connect network clients**

In `UIManager::UIManager`, after MIDI/Stream Deck construction is fine, add:

```cpp
m_projectImportClient = new ProjectImportClient(this);
connect(m_projectImportClient, &ProjectImportClient::finished, this,
        [this](const QByteArray &bytes, const QString &sourceUrl) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        m_importPreviewError = QStringLiteral("Import JSON parse error: ") + parseError.errorString();
        emit importPreviewChanged();
        return;
    }

    ProjectSettingsImporter importer;
    m_pendingImport = importer.importJson(doc.object(), sourceUrl);
    if (!m_pendingImport.ok) {
        m_importPreviewError = m_pendingImport.error;
        emit importPreviewChanged();
        return;
    }

    m_importPreview = previewFromImport(m_pendingImport);
    emit importPreviewChanged();
});
connect(m_projectImportClient, &ProjectImportClient::failed, this, [this](const QString &message) {
    m_importPreviewError = message;
    emit importPreviewChanged();
});

m_telemetryClient = new TelemetryClient(this);
connect(m_telemetryClient, &TelemetryClient::telemetryEvent, this, [this](const TelemetryEvent &event) {
    QVariantMap payload = event.payload.toVariantMap();
    m_liveTelemetryByFeed.insert(event.feedId, payload);
    m_telemetryVersion++;
    emit telemetryChanged();
    if (m_replayManager && m_replayManager->isRecording()) {
        m_replayManager->recordTelemetryEvent(event.feedId, event.payload);
    }
});
connect(m_telemetryClient, &TelemetryClient::errorOccurred, this, [this](const QString &message) {
    qWarning() << "TelemetryClient:" << message;
});
```

- [ ] **Step 4: Add import URL setter and preview helpers**

In `uimanager.cpp`, implement:

```cpp
QString UIManager::importSettingsUrl() const {
    return m_currentSettings.importSettingsUrl;
}

void UIManager::setImportSettingsUrl(const QString &url) {
    const QString trimmed = url.trimmed();
    if (m_currentSettings.importSettingsUrl == trimmed) return;
    m_currentSettings.importSettingsUrl = trimmed;
    emit importSettingsUrlChanged();
}
```

Add a helper near anonymous namespace functions:

```cpp
static QVariantMap previewFromImport(const ProjectSettingsImportResult &result) {
    QVariantMap preview;
    preview.insert("projectId", result.projectId);
    preview.insert("projectName", result.projectName);
    preview.insert("telemetrySseUrl", result.telemetrySseUrl);
    preview.insert("feedCount", result.sources.size());
    preview.insert("metadataFieldCount", result.metadataFields.size());

    QVariantList feeds;
    for (const SourceSettings &source : result.sources) {
        QVariantMap row;
        row.insert("id", source.id);
        row.insert("name", source.name);
        row.insert("url", source.url);
        row.insert("telemetryDelayMs", source.telemetryDelayMs);
        row.insert("metadataCount", source.metadata.size());
        feeds.append(row);
    }
    preview.insert("feeds", feeds);
    return preview;
}
```

- [ ] **Step 5: Implement `readImportSettings()`**

Implement:

```cpp
void UIManager::readImportSettings() {
    m_importPreview.clear();
    m_importPreviewError.clear();
    m_pendingImport = ProjectSettingsImportResult{};
    emit importPreviewChanged();

    const QUrl url(m_currentSettings.importSettingsUrl);
    if (m_projectImportClient) {
        m_projectImportClient->fetch(url);
    }
}
```

- [ ] **Step 6: Implement Apply**

In `uimanager.cpp`, implement:

```cpp
void UIManager::applyImportPreview() {
    if (!m_pendingImport.ok) return;
    if (m_replayManager && m_replayManager->isRecording()) return;

    m_currentSettings.sources = m_pendingImport.sources;
    m_currentSettings.metadataFields = m_pendingImport.metadataFields;
    m_currentSettings.importSettingsUrl = m_pendingImport.importSettingsUrl;
    m_currentSettings.telemetrySseUrl = m_pendingImport.telemetrySseUrl;

    QStringList feedIds;
    QStringList feedNames;
    QList<int> delays;
    for (const SourceSettings &source : m_currentSettings.sources) {
        feedIds.append(source.id);
        feedNames.append(source.name);
        delays.append(source.telemetryDelayMs);
    }
    m_replayManager->setTelemetryFeeds(feedIds, feedNames, delays);

    syncActiveStreams();
    m_settingsManager->save(m_configPath, m_currentSettings);
    emit streamUrlsChanged();
    emit streamNamesChanged();
    emit streamIdsChanged();
    emit telemetryConfigChanged();
    emit importSettingsUrlChanged();
}
```

In `UIManager::syncActiveStreams()`, after source URL/name/trim construction, also build feed IDs/names/delays and call `m_replayManager->setTelemetryFeeds(...)` so loaded settings seed the recorder before the first recording.

- [ ] **Step 7: Start and stop the telemetry client with recording**

In `UIManager::startRecording()`, after `emit recordingStarted();`, add:

```cpp
if (m_telemetryClient && !m_currentSettings.telemetrySseUrl.trimmed().isEmpty()) {
    m_liveTelemetryByFeed.clear();
    m_telemetryVersion++;
    emit telemetryChanged();
    m_telemetryClient->start(QUrl(m_currentSettings.telemetrySseUrl));
}
```

In `UIManager::stopRecording()`, after stopping the replay manager, add:

```cpp
if (m_telemetryClient) {
    m_telemetryClient->stop();
}
```

If `stopRecording()` currently emits signals before/after engine stop, place this near the existing recording lifecycle cleanup so telemetry stops promptly.

- [ ] **Step 8: Expose live telemetry map for QML**

Implement:

```cpp
QVariantMap UIManager::telemetryAtPlayhead() const {
    return m_liveTelemetryByFeed;
}
```

Task 10 replaces this live-only map with playhead-backed timeline reading after the reader is integrated into UIManager. This step gives the QML surface something stable to bind to while recording integration lands.

- [ ] **Step 9: Build app**

Run:

```bash
cmake --build build --target OpenLiveReplay
```

Expected: build succeeds.

- [ ] **Step 10: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt \
        project/projectimportclient.h project/projectimportclient.cpp \
        uimanager.h uimanager.cpp
git commit -m "feat(import): apply provider feed settings"
```

---

### Task 9: QML Import Preview UI

**Files:**
- Modify: `Main.qml`

- [ ] **Step 1: Add Project tab import controls**

In `Main.qml`, inside the Project tab before `Input Sources`, add a compact import section:

```qml
GroupBox {
    title: "External Input Settings"
    Layout.fillWidth: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            TextField {
                Layout.fillWidth: true
                text: appWindow.uiManagerRef.importSettingsUrl
                placeholderText: "https://provider.example/project-settings.json"
                enabled: !appWindow.uiManagerRef.isRecording
                onEditingFinished: appWindow.uiManagerRef.importSettingsUrl = text
            }
            Button {
                text: "Read"
                enabled: !appWindow.uiManagerRef.isRecording
                onClicked: appWindow.uiManagerRef.readImportSettings()
            }
        }

        Text {
            visible: appWindow.uiManagerRef.importPreviewError !== ""
            text: appWindow.uiManagerRef.importPreviewError
            color: "#ff9800"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Button {
            visible: appWindow.uiManagerRef.importPreviewReady
            text: "Preview Imported Sources"
            onClicked: importPreviewPopup.open()
        }
    }
}
```

- [ ] **Step 2: Add the single-summary preview popup**

Near the existing metadata popups, add:

```qml
Popup {
    id: importPreviewPopup
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(appWindow.width * 0.92, 720)
    height: Math.min(appWindow.height * 0.82, 560)

    contentItem: Rectangle {
        color: "#1f1f1f"
        border.color: "#333"
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            Text {
                text: "Imported Input Settings"
                color: "#eeeeee"
                font.pixelSize: 18
                font.bold: true
                Layout.fillWidth: true
            }

            Text {
                text: {
                    var p = appWindow.uiManagerRef.importPreview
                    var name = p.projectName || p.projectId || "Imported project"
                    return name + " · " + (p.feedCount || 0) + " feeds · telemetry: " + (p.telemetrySseUrl || "")
                }
                color: "#b0b0b0"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Text {
                text: "Applying will replace the current input sources. Recording output settings and multiview count stay unchanged."
                color: "#ffcc80"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ListView {
                    id: importFeedsList
                    model: appWindow.uiManagerRef.importPreview.feeds || []
                    spacing: 8
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        width: importFeedsList.width
                        height: 74
                        color: "#181818"
                        border.color: "#333"
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 2

                            Text {
                                text: (modelData.name || modelData.id) + " · " + modelData.id
                                color: "#eeeeee"
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData.url || ""
                                color: "#aaaaaa"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: "telemetry delay +" + (modelData.telemetryDelayMs || 0) + " ms · metadata fields " + (modelData.metadataCount || 0)
                                color: "#777777"
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: "Cancel"
                    onClicked: importPreviewPopup.close()
                }
                Button {
                    text: "Apply"
                    enabled: !appWindow.uiManagerRef.isRecording
                    onClicked: {
                        appWindow.uiManagerRef.applyImportPreview()
                        importPreviewPopup.close()
                    }
                }
            }
        }
    }
}
```

- [ ] **Step 3: Run QML lint/smoke**

Run:

```bash
cmake --build build
ctest --test-dir build -R 'qml|smoke' --output-on-failure
```

Expected: QML smoke/lint passes. If the smoke test name differs, run `ctest --test-dir build -N` and use the listed QML test name.

- [ ] **Step 4: Commit**

```bash
git add Main.qml
git commit -m "feat(ui): preview imported feed settings"
```

---

### Task 10: Playback Telemetry At Playhead

**Files:**
- Modify: `uimanager.h`
- Modify: `uimanager.cpp`
- Modify: `Main.qml`

- [ ] **Step 1: Add reader state to UIManager**

In `uimanager.h`, include:

```cpp
#include "playback/telemetrytimelinereader.h"
```

Add private members:

```cpp
TelemetryTimelineReader m_telemetryReader;
QString m_telemetryReaderPath;
mutable qint64 m_lastTelemetryQueryMs = -1;
mutable QVariantMap m_lastTelemetryStateAtPlayhead;
```

- [ ] **Step 2: Load telemetry reader when recording starts and playback worker opens the file**

In `UIManager::startRecording()`, after `m_playbackWorker->openFile(m_replayManager->getVideoPath());`, add:

```cpp
m_telemetryReaderPath = m_replayManager->getVideoPath();
m_telemetryReader.load(m_telemetryReaderPath);
m_lastTelemetryQueryMs = -1;
m_lastTelemetryStateAtPlayhead.clear();
m_telemetryVersion++;
emit telemetryChanged();
```

In `UIManager::stopRecording()`, after the replay manager stops, reload once:

```cpp
if (!m_telemetryReaderPath.isEmpty()) {
    m_telemetryReader.load(m_telemetryReaderPath);
    m_lastTelemetryQueryMs = -1;
    m_lastTelemetryStateAtPlayhead.clear();
    m_telemetryVersion++;
    emit telemetryChanged();
}
```

- [ ] **Step 3: Query reader by playhead with chase reload**

Replace `telemetryAtPlayhead()` implementation:

```cpp
QVariantMap UIManager::telemetryAtPlayhead() const {
    const qint64 playhead = scrubPosition();
    if (m_telemetryReaderPath.isEmpty()) {
        return m_liveTelemetryByFeed;
    }

    if (m_replayManager && m_replayManager->isRecording()
        && qAbs(playhead - m_lastTelemetryQueryMs) > 250) {
        const_cast<TelemetryTimelineReader&>(m_telemetryReader).load(m_telemetryReaderPath);
    }

    if (m_lastTelemetryQueryMs != playhead) {
        m_lastTelemetryStateAtPlayhead = m_telemetryReader.stateAt(playhead);
        m_lastTelemetryQueryMs = playhead;
    }
    return m_lastTelemetryStateAtPlayhead;
}
```

This is intentionally simple for v1. It favors correctness over perfect efficiency; the implementation can later make the reader incremental if profiling shows the reload is too expensive.

- [ ] **Step 4: Emit telemetryChanged as playback moves**

In the existing `connect(m_transport, &PlaybackTransport::posChanged, ...)` block, add:

```cpp
m_telemetryVersion++;
emit telemetryChanged();
```

- [ ] **Step 5: Add compact telemetry display in playback labels**

In `Main.qml`, add a small telemetry line under the playback view controls:

```qml
Text {
    Layout.fillWidth: true
    color: "#aaaaaa"
    font.family: "Menlo"
    font.pixelSize: 11
    elide: Text.ElideRight
    text: {
        var version = appWindow.uiManagerRef.telemetryVersion
        var state = appWindow.uiManagerRef.telemetryAtPlayhead()
        var keys = Object.keys(state)
        if (keys.length === 0) return "Telemetry: --"
        var first = keys[0]
        var values = state[first].values || {}
        var parts = []
        if (values.batteryPercent !== undefined) parts.push("battery " + values.batteryPercent + "%")
        if (values.signalDb !== undefined) parts.push("signal " + values.signalDb + " dB")
        if (parts.length === 0) parts.push("updated")
        return "Telemetry " + first + ": " + parts.join(" · ")
    }
}
```

- [ ] **Step 6: Build app and run reader test**

Run:

```bash
cmake --build build --target OpenLiveReplay tst_telemetrytimelinereader
ctest --test-dir build -R '^tst_telemetrytimelinereader$' --output-on-failure
```

Expected: build succeeds and reader test passes.

- [ ] **Step 7: Commit**

```bash
git add uimanager.h uimanager.cpp Main.qml
git commit -m "feat(playback): show telemetry at playhead"
```

---

### Task 11: Telemetry E2E Harness

**Files:**
- Create: `tests/e2e/telemetry_harness.cpp`
- Create: `tests/e2e/run_telemetry_e2e.sh`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Create telemetry harness**

Create `tests/e2e/telemetry_harness.cpp`:

```cpp
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include "recorder_engine/replaymanager.h"
#include "playback/telemetrytimelinereader.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption outdirOpt(QStringList{"outdir"}, "Output directory", "dir");
    parser.addOption(outdirOpt);
    parser.process(app);

    const QString outdir = parser.value(outdirOpt);
    if (outdir.isEmpty()) return 2;

    ReplayManager rm;
    rm.setOutputDirectory(outdir);
    rm.setBaseFileName(QStringLiteral("telemetry_e2e"));
    rm.setVideoWidth(320);
    rm.setVideoHeight(240);
    rm.setFps(30);
    rm.setSourceUrls(QStringList{QStringLiteral("testsrc://unused")});
    rm.setSourceNames(QStringList{QStringLiteral("Main")});
    rm.setViewCount(1);
    rm.setViewNames(QStringList{QStringLiteral("Main")});
    rm.updateViewMapping(QList<int>{0});
    rm.setTelemetryFeeds(QStringList{QStringLiteral("cam-main")},
                         QStringList{QStringLiteral("Main")},
                         QList<int>{500});

    rm.startRecording();
    if (!rm.isRecording()) return 3;

    QTimer::singleShot(100, [&rm]() {
        rm.recordTelemetryEvent(QStringLiteral("cam-main"),
            QJsonObject{{"feedId", "cam-main"},
                        {"values", QJsonObject{{"batteryPercent", 91}}}});
    });
    QTimer::singleShot(400, [&rm]() {
        rm.recordTelemetryEvent(QStringLiteral("cam-main"),
            QJsonObject{{"feedId", "cam-main"},
                        {"values", QJsonObject{{"batteryPercent", 89}}}});
    });
    QTimer::singleShot(900, [&]() {
        const QString path = rm.getVideoPath();
        rm.stopRecording();

        TelemetryTimelineReader reader;
        if (!reader.load(path)) {
            qWarning("%s", qPrintable(reader.lastError()));
            app.exit(4);
            return;
        }
        const QVariantMap state = reader.stateAt(1000);
        const int battery = state.value("cam-main").toMap().value("values").toMap().value("batteryPercent").toInt();
        if (battery != 89) {
            qWarning("unexpected battery value %d", battery);
            app.exit(5);
            return;
        }
        qInfo("PASS telemetry_e2e %s", qPrintable(path));
        app.exit(0);
    });

    return app.exec();
}
```

- [ ] **Step 2: Create e2e shell runner**

Create `tests/e2e/run_telemetry_e2e.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

HARNESS="${1:?usage: run_telemetry_e2e.sh <telemetry_harness> <workdir>}"
WORKDIR="${2:-$(mktemp -d)}"
mkdir -p "$WORKDIR"

"$HARNESS" --outdir "$WORKDIR"
```

Make it executable:

```bash
chmod +x tests/e2e/run_telemetry_e2e.sh
```

- [ ] **Step 3: Register e2e target**

In `tests/e2e/CMakeLists.txt`, add:

```cmake
qt_add_executable(telemetry_harness telemetry_harness.cpp)
target_link_libraries(telemetry_harness PRIVATE
    Qt6::Core Qt6::Test olr_test_engine olr_test_playback olr_warnings olr_sanitize)

add_test(NAME e2e_telemetry_replay
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_telemetry_e2e.sh"
            $<TARGET_FILE:telemetry_harness>
            "${CMAKE_BINARY_DIR}/telemetry-e2e")
set_tests_properties(e2e_telemetry_replay PROPERTIES
    LABELS "e2e"
    RUN_SERIAL TRUE
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
```

- [ ] **Step 4: Run telemetry e2e**

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build --target telemetry_harness
ctest --test-dir build -R '^e2e_telemetry_replay$' --output-on-failure
```

Expected: PASS with `PASS telemetry_e2e <path>`.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/CMakeLists.txt tests/e2e/telemetry_harness.cpp tests/e2e/run_telemetry_e2e.sh
git commit -m "test(e2e): verify telemetry replay timeline"
```

---

### Task 12: Full Verification And Polish

**Files:**
- Modify only files required by failures from full verification.

- [ ] **Step 1: Run formatting on changed C++ files**

Run:

```bash
xcrun clang-format -i \
  project/projectsettingsimporter.h project/projectsettingsimporter.cpp \
  telemetry/telemetryevent.h telemetry/sseparser.h telemetry/sseparser.cpp \
  telemetry/telemetryclient.h telemetry/telemetryclient.cpp \
  playback/telemetrytimelinereader.h playback/telemetrytimelinereader.cpp \
  recorder_engine/muxer.h recorder_engine/muxer.cpp \
  recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp \
  settingsmanager.h settingsmanager.cpp uimanager.h uimanager.cpp \
  tests/unit/tst_projectsettingsimporter.cpp tests/unit/tst_sseparser.cpp \
  tests/unit/tst_telemetrytimelinereader.cpp tests/unit/tst_replaymanager_telemetry.cpp \
  tests/e2e/telemetry_harness.cpp
```

Expected: command exits 0.

- [ ] **Step 2: Run full build and tests**

Run:

```bash
cmake -S . -B build -G Ninja -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: all configured tests pass.

- [ ] **Step 3: Inspect final diff**

Run:

```bash
git diff --stat HEAD
git diff --check
git status --short
```

Expected:

- `git diff --check` prints nothing and exits 0.
- `git status --short` shows only intentional modified/untracked files.

- [ ] **Step 4: Commit verification fixes**

If formatting or verification changed files, commit them:

```bash
git add .
git commit -m "chore: polish feed telemetry import"
```

If no files changed, do not create an empty commit.
