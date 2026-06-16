# WebSocket Control API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an unauthenticated local-network WebSocket control API on port `8115` that exposes OpenLiveReplay commands, state changes, events, telemetry, and timecode updates.

**Architecture:** Add a focused `websocket/` module with a protocol/serialization layer, a testable app adapter interface, and a `QWebSocketServer` wrapper. The server owns sockets and JSON protocol handling while all app behavior remains in `UIManager` and `PlaybackTransport`; `UIManager` gains small public adapter methods/signals where existing functionality is private or not currently observable.

**Tech Stack:** C++17, Qt 6 Core/Network/WebSockets/Test, Qt JSON classes, CMake/CTest, existing Qt Test style.

---

## File Structure

- Create `websocket/controlapiadapter.h`: abstract interface between WebSocket protocol code and the app.
- Create `websocket/controlprotocol.h` and `websocket/controlprotocol.cpp`: parse client messages, validate command arguments, build acknowledgements/errors, and serialize common JSON messages.
- Create `websocket/controlstate.h` and `websocket/controlstate.cpp`: convert adapter-provided state into snapshot and patch JSON.
- Create `websocket/controlwebsocketserver.h` and `websocket/controlwebsocketserver.cpp`: own `QWebSocketServer`, client sockets, command dispatch, event publishing, and timecode throttling.
- Create `websocket/uimanagercontroladapter.h` and `websocket/uimanagercontroladapter.cpp`: implement `ControlApiAdapter` by calling `UIManager` and `PlaybackTransport`.
- Modify `uimanager.h` and `uimanager.cpp`: add explicit external action helpers, read-only playback-view accessors, and missing metadata/source change signals.
- Modify `main.cpp`: construct and start `ControlWebSocketServer` on port `8115` after settings load.
- Modify `CMakeLists.txt`: require/link `Qt6::WebSockets`, add WebSocket module sources to the app target.
- Modify `tests/CMakeLists.txt` and `tests/unit/CMakeLists.txt`: add WebSockets to test dependencies and register new unit tests.
- Create `tests/unit/tst_controlprotocol.cpp`: protocol parser and ack/error unit tests.
- Create `tests/unit/tst_controlstate.cpp`: snapshot/patch serialization tests with a fake adapter.
- Create `tests/unit/tst_controlwebsocketserver.cpp`: local `QWebSocket` integration test with a fake adapter and ephemeral port.
- Create `docs/websocket-control-api.md`: concise integration reference.

## Task 1: Baseline And Qt WebSockets Build Plumbing

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Configure the worktree test build before edits**

Run:

```bash
cmake -S . -B build -DOLR_BUILD_TESTS=ON
```

Expected: configure completes. If Qt or FFmpeg discovery fails, record the exact error in the task notes and continue with source-level changes; do not change unrelated dependency setup.

- [ ] **Step 2: Update app CMake to require WebSockets**

In `CMakeLists.txt`, change:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2 Network)
```

to:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2 Network WebSockets)
```

Then add `Qt6::WebSockets` to the existing `target_link_libraries(OpenLiveReplay PRIVATE ...)` block. If no such block exists for Qt targets near the app source list, create one after platform-specific source setup:

```cmake
target_link_libraries(OpenLiveReplay PRIVATE
    Qt6::Quick
    Qt6::Core
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
    OpenLiveReplayMidi
    olr_warnings
    olr_sanitize
)
```

If `OpenLiveReplay` already links some of these later in the file, add only `Qt6::WebSockets` at the nearest existing call to avoid duplicate churn.

- [ ] **Step 3: Update test CMake to require WebSockets**

In `tests/CMakeLists.txt`, change:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent Network)
```

to:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent Network WebSockets)
```

Add `Qt6::WebSockets` to `olr_test_core`:

```cmake
target_link_libraries(olr_test_core
    PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets
    PRIVATE olr_warnings olr_sanitize)
```

- [ ] **Step 4: Reconfigure to prove the module is available**

Run:

```bash
cmake -S . -B build -DOLR_BUILD_TESTS=ON
```

Expected: configure completes and generated build files mention no missing `Qt6WebSockets`.

- [ ] **Step 5: Commit build plumbing**

Run:

```bash
git add CMakeLists.txt tests/CMakeLists.txt
git commit -m "build: link qt websockets"
```

Expected: commit succeeds with only CMake files staged.

## Task 2: Protocol Parser And JSON Message Builders

**Files:**
- Create: `websocket/controlprotocol.h`
- Create: `websocket/controlprotocol.cpp`
- Create: `tests/unit/tst_controlprotocol.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Register a failing protocol test target**

Add to `tests/unit/CMakeLists.txt`:

```cmake
olr_add_unit_test(tst_controlprotocol olr_test_core)
```

Create `tests/unit/tst_controlprotocol.cpp`:

```cpp
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "websocket/controlprotocol.h"

class TestControlProtocol : public QObject {
    Q_OBJECT
private slots:
    void parsesCommandWithIdAndArgs();
    void rejectsMalformedJson();
    void rejectsMissingCommandName();
    void buildsSuccessAck();
    void buildsFailureAck();
    void buildsErrorWithoutId();
};

void TestControlProtocol::parsesCommandWithIdAndArgs() {
    const QByteArray raw =
        "{\"type\":\"command\",\"id\":\"abc-1\",\"name\":\"transport.seek\","
        "\"args\":{\"positionMs\":1234}}";

    const ControlProtocol::ParseResult result = ControlProtocol::parseTextMessage(raw);

    QVERIFY(result.ok);
    QCOMPARE(result.message.type, QStringLiteral("command"));
    QCOMPARE(result.message.id, QStringLiteral("abc-1"));
    QCOMPARE(result.message.name, QStringLiteral("transport.seek"));
    QCOMPARE(result.message.args.value(QStringLiteral("positionMs")).toInt(), 1234);
}

void TestControlProtocol::rejectsMalformedJson() {
    const ControlProtocol::ParseResult result = ControlProtocol::parseTextMessage("{ nope");

    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("bad_json"));
    QVERIFY(result.messageText.contains(QStringLiteral("JSON")));
}

void TestControlProtocol::rejectsMissingCommandName() {
    const ControlProtocol::ParseResult result =
        ControlProtocol::parseTextMessage("{\"type\":\"command\",\"id\":\"abc\"}");

    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("bad_message"));
    QCOMPARE(result.id, QStringLiteral("abc"));
}

void TestControlProtocol::buildsSuccessAck() {
    const QJsonObject ack = ControlProtocol::ack(QStringLiteral("abc-1"));

    QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(ack.value(QStringLiteral("id")).toString(), QStringLiteral("abc-1"));
    QCOMPARE(ack.value(QStringLiteral("ok")).toBool(), true);
}

void TestControlProtocol::buildsFailureAck() {
    const QJsonObject ack = ControlProtocol::ackError(
        QStringLiteral("abc-2"), QStringLiteral("invalid_args"),
        QStringLiteral("positionMs must be an integer"));

    QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(ack.value(QStringLiteral("id")).toString(), QStringLiteral("abc-2"));
    QCOMPARE(ack.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(ack.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_args"));
}

void TestControlProtocol::buildsErrorWithoutId() {
    const QJsonObject error = ControlProtocol::error(
        QStringLiteral("bad_json"), QStringLiteral("Message must be a JSON object"));

    QCOMPARE(error.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(error.value(QStringLiteral("code")).toString(), QStringLiteral("bad_json"));
    QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("Message must be a JSON object"));
}

QTEST_GUILESS_MAIN(TestControlProtocol)
#include "tst_controlprotocol.moc"
```

- [ ] **Step 2: Run the protocol test and verify RED**

Run:

```bash
cmake --build build --target tst_controlprotocol
```

Expected: build fails because `websocket/controlprotocol.h` does not exist.

- [ ] **Step 3: Add the minimal protocol header**

Create `websocket/controlprotocol.h`:

```cpp
#ifndef CONTROLPROTOCOL_H
#define CONTROLPROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

struct ControlCommandMessage {
    QString type;
    QString id;
    QString name;
    QJsonObject args;
};

class ControlProtocol {
public:
    struct ParseResult {
        bool ok = false;
        ControlCommandMessage message;
        QString id;
        QString code;
        QString messageText;
    };

    static ParseResult parseTextMessage(const QByteArray &payload);
    static QJsonObject ack(const QString &id);
    static QJsonObject ackError(const QString &id, const QString &code, const QString &message);
    static QJsonObject error(const QString &code, const QString &message);
    static QByteArray compact(const QJsonObject &object);
};

#endif
```

- [ ] **Step 4: Add the minimal protocol implementation**

Create `websocket/controlprotocol.cpp`:

```cpp
#include "controlprotocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

ControlProtocol::ParseResult ControlProtocol::parseTextMessage(const QByteArray &payload) {
    ParseResult result;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.code = QStringLiteral("bad_json");
        result.messageText = QStringLiteral("Invalid JSON: %1").arg(parseError.errorString());
        return result;
    }
    if (!doc.isObject()) {
        result.code = QStringLiteral("bad_message");
        result.messageText = QStringLiteral("Message must be a JSON object");
        return result;
    }

    const QJsonObject obj = doc.object();
    result.id = obj.value(QStringLiteral("id")).toString();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("command")) {
        result.code = QStringLiteral("unsupported_message");
        result.messageText = QStringLiteral("Unsupported message type");
        return result;
    }

    const QString name = obj.value(QStringLiteral("name")).toString();
    if (name.trimmed().isEmpty()) {
        result.code = QStringLiteral("bad_message");
        result.messageText = QStringLiteral("Command message requires name");
        return result;
    }

    result.ok = true;
    result.message.type = type;
    result.message.id = result.id;
    result.message.name = name;
    const QJsonValue args = obj.value(QStringLiteral("args"));
    result.message.args = args.isObject() ? args.toObject() : QJsonObject{};
    return result;
}

QJsonObject ControlProtocol::ack(const QString &id) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("ack"));
    if (!id.isEmpty()) obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), true);
    return obj;
}

QJsonObject ControlProtocol::ackError(const QString &id, const QString &code, const QString &message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("ack"));
    if (!id.isEmpty()) obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), false);
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    return obj;
}

QJsonObject ControlProtocol::error(const QString &code, const QString &message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("error"));
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    return obj;
}

QByteArray ControlProtocol::compact(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
```

- [ ] **Step 5: Add protocol sources to test core**

In `tests/CMakeLists.txt`, add to `olr_test_core` sources:

```cmake
"${CMAKE_SOURCE_DIR}/websocket/controlprotocol.cpp"
```

Add `target_include_directories(olr_test_core PUBLIC "${CMAKE_SOURCE_DIR}")` already exists; keep it as-is.

- [ ] **Step 6: Run protocol tests and verify GREEN**

Run:

```bash
cmake --build build --target tst_controlprotocol
ctest --test-dir build --output-on-failure -R tst_controlprotocol
```

Expected: build succeeds and `tst_controlprotocol` passes.

- [ ] **Step 7: Commit protocol parser**

Run:

```bash
git add websocket/controlprotocol.h websocket/controlprotocol.cpp tests/unit/tst_controlprotocol.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add websocket control protocol parser"
```

Expected: commit succeeds.

## Task 3: Adapter Interface And State Serialization

**Files:**
- Create: `websocket/controlapiadapter.h`
- Create: `websocket/controlstate.h`
- Create: `websocket/controlstate.cpp`
- Create: `tests/unit/tst_controlstate.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Register a failing state serialization test**

Add to `tests/unit/CMakeLists.txt`:

```cmake
olr_add_unit_test(tst_controlstate olr_test_core)
```

Create `tests/unit/tst_controlstate.cpp`:

```cpp
#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>

#include "websocket/controlapiadapter.h"
#include "websocket/controlstate.h"

class FakeControlAdapter final : public ControlApiAdapter {
public:
    RecordingState recordingState() const override { return {true, 1500, 1700000000123}; }
    TransportState transportState() const override { return {1200, 1200, 1500, QStringLiteral("00:00:01:06"), true, 1.0, 30, true, 1000}; }
    QVector<SourceState> sourceStates() const override {
        SourceState s;
        s.index = 0;
        s.id = QStringLiteral("cam-a");
        s.name = QStringLiteral("Camera A");
        s.url = QStringLiteral("srt://127.0.0.1:9000");
        s.enabled = true;
        s.connected = true;
        s.duplicateUrl = false;
        s.trimOffsetMs = 33;
        s.metadata.append(QVariantMap{{QStringLiteral("name"), QStringLiteral("driver")}, {QStringLiteral("value"), QStringLiteral("A. Smith")}});
        return {s};
    }
    ViewState viewState() const override { return {4, QVariantList{0, -1, 1, 2}, false, -1}; }
    SettingsState settingsState() const override { return {QStringLiteral("match"), QStringLiteral("/tmp"), 1920, 1080, 30, 40, true, QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("driver")}}}}; }
    MidiState midiState() const override { return {QStringList{QStringLiteral("X-Touch")}, 0, QStringLiteral("X-Touch"), true, -1, 0}; }
    StreamDeckState streamDeckState() const override { return {true, true, QStringLiteral("Deck"), QStringLiteral("Plus"), 8, 4, -1}; }
    ScreensState screensState() const override { return {true, 1, QVariantList{QVariantMap{{QStringLiteral("index"), 0}, {QStringLiteral("label"), QStringLiteral("Display")}}}}; }
    ImportState importState() const override { return {QStringLiteral("http://settings"), QStringLiteral("http://telemetry"), false, QString(), QVariantMap{}}; }
    TelemetryState telemetryState() const override { return {2, QVariantList{QVariantMap{{QStringLiteral("feedId"), QStringLiteral("cam-a")}}}, QVariantMap{{QStringLiteral("cam-a"), QVariantMap{{QStringLiteral("speed"), 88}}}}}; }
};

class TestControlState : public QObject {
    Q_OBJECT
private slots:
    void buildsSnapshotWithExpectedTopLevelObjects();
    void buildsPathPatch();
    void buildsTimecodeMessageFromTransport();
};

void TestControlState::buildsSnapshotWithExpectedTopLevelObjects() {
    FakeControlAdapter adapter;

    const QJsonObject msg = ControlState::snapshotMessage(adapter);
    const QJsonObject state = msg.value(QStringLiteral("state")).toObject();

    QCOMPARE(msg.value(QStringLiteral("type")).toString(), QStringLiteral("state.snapshot"));
    QCOMPARE(state.value(QStringLiteral("recording")).toObject().value(QStringLiteral("active")).toBool(), true);
    QCOMPARE(state.value(QStringLiteral("transport")).toObject().value(QStringLiteral("timecode")).toString(), QStringLiteral("00:00:01:06"));
    QCOMPARE(state.value(QStringLiteral("sources")).toArray().first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("cam-a"));
    QCOMPARE(state.value(QStringLiteral("settings")).toObject().value(QStringLiteral("audioOutputLatencyMs")).toInt(), 40);
    QCOMPARE(state.value(QStringLiteral("telemetry")).toObject().value(QStringLiteral("version")).toInt(), 2);
}

void TestControlState::buildsPathPatch() {
    const QJsonObject value{{QStringLiteral("playing"), true}};

    const QJsonObject msg = ControlState::patchMessage(QStringLiteral("transport"), value);

    QCOMPARE(msg.value(QStringLiteral("type")).toString(), QStringLiteral("state.patch"));
    QCOMPARE(msg.value(QStringLiteral("path")).toString(), QStringLiteral("transport"));
    QCOMPARE(msg.value(QStringLiteral("value")).toObject().value(QStringLiteral("playing")).toBool(), true);
}

void TestControlState::buildsTimecodeMessageFromTransport() {
    FakeControlAdapter adapter;

    const QJsonObject msg = ControlState::timecodeMessage(adapter);

    QCOMPARE(msg.value(QStringLiteral("type")).toString(), QStringLiteral("timecode"));
    QCOMPARE(msg.value(QStringLiteral("positionMs")).toInt(), 1200);
    QCOMPARE(msg.value(QStringLiteral("durationMs")).toInt(), 1500);
    QCOMPARE(msg.value(QStringLiteral("text")).toString(), QStringLiteral("00:00:01:06"));
    QCOMPARE(msg.value(QStringLiteral("followLive")).toBool(), true);
}

QTEST_GUILESS_MAIN(TestControlState)
#include "tst_controlstate.moc"
```

- [ ] **Step 2: Run state test and verify RED**

Run:

```bash
cmake --build build --target tst_controlstate
```

Expected: build fails because `websocket/controlapiadapter.h` and `websocket/controlstate.h` do not exist.

- [ ] **Step 3: Add adapter state structs and interface**

Create `websocket/controlapiadapter.h`:

```cpp
#ifndef CONTROLAPIADAPTER_H
#define CONTROLAPIADAPTER_H

#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <QVector>

struct RecordingState {
    bool active = false;
    qint64 durationMs = 0;
    qint64 startEpochMs = 0;
};

struct TransportState {
    qint64 positionMs = 0;
    qint64 scrubPositionMs = 0;
    qint64 durationMs = 0;
    QString timecode;
    bool playing = false;
    double speed = 1.0;
    int fps = 30;
    bool followLive = false;
    int liveBufferMs = 1000;
};

struct SourceState {
    int index = -1;
    QString id;
    QString name;
    QString url;
    bool enabled = false;
    bool connected = false;
    bool duplicateUrl = false;
    int trimOffsetMs = 0;
    QVariantList metadata;
};

struct ViewState {
    int multiviewCount = 1;
    QVariantList slotMap;
    bool singleView = false;
    int selectedIndex = -1;
};

struct SettingsState {
    QString fileName;
    QString saveLocation;
    int recordWidth = 1920;
    int recordHeight = 1080;
    int recordFps = 30;
    int audioOutputLatencyMs = 0;
    bool timeOfDayMode = false;
    QVariantList metadataFields;
};

struct MidiState {
    QStringList ports;
    int portIndex = -1;
    QString portName;
    bool connected = false;
    int learnAction = -1;
    int learnMode = 0;
};

struct StreamDeckState {
    bool supported = false;
    bool connected = false;
    QString deviceName;
    QString deviceModel;
    int keyCount = 0;
    int dialCount = 0;
    int learnAction = -1;
};

struct ScreensState {
    bool ready = false;
    int count = 0;
    QVariantList options;
};

struct ImportState {
    QString settingsUrl;
    QString telemetrySseUrl;
    bool previewReady = false;
    QString previewError;
    QVariantMap preview;
};

struct TelemetryState {
    int version = 0;
    QVariantList rows;
    QVariantMap state;
};

class ControlApiAdapter {
public:
    virtual ~ControlApiAdapter() = default;

    virtual RecordingState recordingState() const = 0;
    virtual TransportState transportState() const = 0;
    virtual QVector<SourceState> sourceStates() const = 0;
    virtual ViewState viewState() const = 0;
    virtual SettingsState settingsState() const = 0;
    virtual MidiState midiState() const = 0;
    virtual StreamDeckState streamDeckState() const = 0;
    virtual ScreensState screensState() const = 0;
    virtual ImportState importState() const = 0;
    virtual TelemetryState telemetryState() const = 0;
};

#endif
```

- [ ] **Step 4: Add state serializer header**

Create `websocket/controlstate.h`:

```cpp
#ifndef CONTROLSTATE_H
#define CONTROLSTATE_H

#include <QJsonObject>
#include <QString>

class ControlApiAdapter;

class ControlState {
public:
    static QJsonObject snapshotMessage(const ControlApiAdapter &adapter);
    static QJsonObject patchMessage(const QString &path, const QJsonObject &value);
    static QJsonObject timecodeMessage(const ControlApiAdapter &adapter);

    static QJsonObject recordingObject(const ControlApiAdapter &adapter);
    static QJsonObject transportObject(const ControlApiAdapter &adapter);
    static QJsonObject settingsObject(const ControlApiAdapter &adapter);
    static QJsonObject telemetryObject(const ControlApiAdapter &adapter);
};

#endif
```

- [ ] **Step 5: Add state serializer implementation**

Create `websocket/controlstate.cpp`:

```cpp
#include "controlstate.h"

#include "controlapiadapter.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {

QJsonArray variantListToArray(const QVariantList &list) {
    return QJsonArray::fromVariantList(list);
}

QJsonObject variantMapToObject(const QVariantMap &map) {
    return QJsonObject::fromVariantMap(map);
}

QJsonObject sourceObject(const SourceState &source) {
    QJsonObject obj;
    obj.insert(QStringLiteral("index"), source.index);
    obj.insert(QStringLiteral("id"), source.id);
    obj.insert(QStringLiteral("name"), source.name);
    obj.insert(QStringLiteral("url"), source.url);
    obj.insert(QStringLiteral("enabled"), source.enabled);
    obj.insert(QStringLiteral("connected"), source.connected);
    obj.insert(QStringLiteral("duplicateUrl"), source.duplicateUrl);
    obj.insert(QStringLiteral("trimOffsetMs"), source.trimOffsetMs);
    obj.insert(QStringLiteral("metadata"), variantListToArray(source.metadata));
    return obj;
}

} // namespace

QJsonObject ControlState::snapshotMessage(const ControlApiAdapter &adapter) {
    QJsonObject state;
    state.insert(QStringLiteral("recording"), recordingObject(adapter));
    state.insert(QStringLiteral("transport"), transportObject(adapter));

    QJsonArray sources;
    for (const SourceState &source : adapter.sourceStates()) {
        sources.append(sourceObject(source));
    }
    state.insert(QStringLiteral("sources"), sources);

    const ViewState view = adapter.viewState();
    QJsonObject viewObj;
    viewObj.insert(QStringLiteral("multiviewCount"), view.multiviewCount);
    viewObj.insert(QStringLiteral("slotMap"), variantListToArray(view.slotMap));
    viewObj.insert(QStringLiteral("singleView"), view.singleView);
    viewObj.insert(QStringLiteral("selectedIndex"), view.selectedIndex);
    state.insert(QStringLiteral("views"), viewObj);

    state.insert(QStringLiteral("settings"), settingsObject(adapter));

    const MidiState midi = adapter.midiState();
    QJsonObject midiObj;
    midiObj.insert(QStringLiteral("ports"), QJsonArray::fromStringList(midi.ports));
    midiObj.insert(QStringLiteral("portIndex"), midi.portIndex);
    midiObj.insert(QStringLiteral("portName"), midi.portName);
    midiObj.insert(QStringLiteral("connected"), midi.connected);
    midiObj.insert(QStringLiteral("learnAction"), midi.learnAction);
    midiObj.insert(QStringLiteral("learnMode"), midi.learnMode);
    state.insert(QStringLiteral("midi"), midiObj);

    const StreamDeckState deck = adapter.streamDeckState();
    QJsonObject deckObj;
    deckObj.insert(QStringLiteral("supported"), deck.supported);
    deckObj.insert(QStringLiteral("connected"), deck.connected);
    deckObj.insert(QStringLiteral("deviceName"), deck.deviceName);
    deckObj.insert(QStringLiteral("deviceModel"), deck.deviceModel);
    deckObj.insert(QStringLiteral("keyCount"), deck.keyCount);
    deckObj.insert(QStringLiteral("dialCount"), deck.dialCount);
    deckObj.insert(QStringLiteral("learnAction"), deck.learnAction);
    state.insert(QStringLiteral("streamDeck"), deckObj);

    const ScreensState screens = adapter.screensState();
    QJsonObject screensObj;
    screensObj.insert(QStringLiteral("ready"), screens.ready);
    screensObj.insert(QStringLiteral("count"), screens.count);
    screensObj.insert(QStringLiteral("options"), variantListToArray(screens.options));
    state.insert(QStringLiteral("screens"), screensObj);

    const ImportState import = adapter.importState();
    QJsonObject importObj;
    importObj.insert(QStringLiteral("settingsUrl"), import.settingsUrl);
    importObj.insert(QStringLiteral("telemetrySseUrl"), import.telemetrySseUrl);
    importObj.insert(QStringLiteral("previewReady"), import.previewReady);
    importObj.insert(QStringLiteral("previewError"), import.previewError);
    importObj.insert(QStringLiteral("preview"), variantMapToObject(import.preview));
    state.insert(QStringLiteral("import"), importObj);

    state.insert(QStringLiteral("telemetry"), telemetryObject(adapter));

    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("state.snapshot"));
    msg.insert(QStringLiteral("state"), state);
    return msg;
}

QJsonObject ControlState::patchMessage(const QString &path, const QJsonObject &value) {
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("state.patch"));
    msg.insert(QStringLiteral("path"), path);
    msg.insert(QStringLiteral("value"), value);
    return msg;
}

QJsonObject ControlState::timecodeMessage(const ControlApiAdapter &adapter) {
    const TransportState transport = adapter.transportState();
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("timecode"));
    msg.insert(QStringLiteral("positionMs"), transport.scrubPositionMs);
    msg.insert(QStringLiteral("durationMs"), transport.durationMs);
    msg.insert(QStringLiteral("text"), transport.timecode);
    msg.insert(QStringLiteral("followLive"), transport.followLive);
    msg.insert(QStringLiteral("playing"), transport.playing);
    msg.insert(QStringLiteral("speed"), transport.speed);
    return msg;
}

QJsonObject ControlState::recordingObject(const ControlApiAdapter &adapter) {
    const RecordingState recording = adapter.recordingState();
    QJsonObject obj;
    obj.insert(QStringLiteral("active"), recording.active);
    obj.insert(QStringLiteral("durationMs"), recording.durationMs);
    obj.insert(QStringLiteral("startEpochMs"), recording.startEpochMs);
    return obj;
}

QJsonObject ControlState::transportObject(const ControlApiAdapter &adapter) {
    const TransportState transport = adapter.transportState();
    QJsonObject obj;
    obj.insert(QStringLiteral("positionMs"), transport.positionMs);
    obj.insert(QStringLiteral("scrubPositionMs"), transport.scrubPositionMs);
    obj.insert(QStringLiteral("durationMs"), transport.durationMs);
    obj.insert(QStringLiteral("timecode"), transport.timecode);
    obj.insert(QStringLiteral("playing"), transport.playing);
    obj.insert(QStringLiteral("speed"), transport.speed);
    obj.insert(QStringLiteral("fps"), transport.fps);
    obj.insert(QStringLiteral("followLive"), transport.followLive);
    obj.insert(QStringLiteral("liveBufferMs"), transport.liveBufferMs);
    return obj;
}

QJsonObject ControlState::settingsObject(const ControlApiAdapter &adapter) {
    const SettingsState settings = adapter.settingsState();
    QJsonObject obj;
    obj.insert(QStringLiteral("fileName"), settings.fileName);
    obj.insert(QStringLiteral("saveLocation"), settings.saveLocation);
    obj.insert(QStringLiteral("recordWidth"), settings.recordWidth);
    obj.insert(QStringLiteral("recordHeight"), settings.recordHeight);
    obj.insert(QStringLiteral("recordFps"), settings.recordFps);
    obj.insert(QStringLiteral("audioOutputLatencyMs"), settings.audioOutputLatencyMs);
    obj.insert(QStringLiteral("timeOfDayMode"), settings.timeOfDayMode);
    obj.insert(QStringLiteral("metadataFields"), variantListToArray(settings.metadataFields));
    return obj;
}

QJsonObject ControlState::telemetryObject(const ControlApiAdapter &adapter) {
    const TelemetryState telemetry = adapter.telemetryState();
    QJsonObject obj;
    obj.insert(QStringLiteral("version"), telemetry.version);
    obj.insert(QStringLiteral("rows"), variantListToArray(telemetry.rows));
    obj.insert(QStringLiteral("state"), variantMapToObject(telemetry.state));
    return obj;
}
```

- [ ] **Step 6: Add state sources to test core**

In `tests/CMakeLists.txt`, add to `olr_test_core` sources:

```cmake
"${CMAKE_SOURCE_DIR}/websocket/controlstate.cpp"
```

- [ ] **Step 7: Run state tests and verify GREEN**

Run:

```bash
cmake --build build --target tst_controlstate
ctest --test-dir build --output-on-failure -R tst_controlstate
```

Expected: `tst_controlstate` passes.

- [ ] **Step 8: Commit state serialization**

Run:

```bash
git add websocket/controlapiadapter.h websocket/controlstate.h websocket/controlstate.cpp tests/unit/tst_controlstate.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: serialize websocket control state"
```

Expected: commit succeeds.

## Task 4: Command Dispatch Interface

**Files:**
- Modify: `websocket/controlapiadapter.h`
- Modify: `websocket/controlprotocol.h`
- Modify: `websocket/controlprotocol.cpp`
- Modify: `tests/unit/tst_controlprotocol.cpp`

- [ ] **Step 1: Add failing command validation tests**

Append these slots to `TestControlProtocol` in `tests/unit/tst_controlprotocol.cpp`:

```cpp
void validatesSeekArgs();
void rejectsSeekWithoutPosition();
void validatesActionDispatchDefaultsPressed();
```

Add implementations before `QTEST_GUILESS_MAIN`:

```cpp
void TestControlProtocol::validatesSeekArgs() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("seek-1"),
        QStringLiteral("transport.seek"),
        QJsonObject{{QStringLiteral("positionMs"), 42}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(validation.ok);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("positionMs")).toInt(), 42);
}

void TestControlProtocol::rejectsSeekWithoutPosition() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("seek-2"),
        QStringLiteral("transport.seek"),
        QJsonObject{}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(!validation.ok);
    QCOMPARE(validation.code, QStringLiteral("invalid_args"));
    QVERIFY(validation.message.contains(QStringLiteral("positionMs")));
}

void TestControlProtocol::validatesActionDispatchDefaultsPressed() {
    const ControlCommandMessage command{
        QStringLiteral("command"),
        QStringLiteral("action-1"),
        QStringLiteral("action.dispatch"),
        QJsonObject{{QStringLiteral("actionId"), 0}}
    };

    const ControlProtocol::CommandValidation validation = ControlProtocol::validateCommand(command);

    QVERIFY(validation.ok);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("actionId")).toInt(), 0);
    QCOMPARE(validation.normalizedArgs.value(QStringLiteral("pressed")).toBool(), true);
}
```

- [ ] **Step 2: Run protocol tests and verify RED**

Run:

```bash
cmake --build build --target tst_controlprotocol
```

Expected: build fails because `CommandValidation` and `validateCommand()` do not exist.

- [ ] **Step 3: Extend adapter with command methods**

In `websocket/controlapiadapter.h`, add this struct before `class ControlApiAdapter`:

```cpp
struct CommandResult {
    bool ok = true;
    QString code;
    QString message;

    static CommandResult success() { return {}; }
    static CommandResult failure(const QString &failureCode, const QString &failureMessage) {
        CommandResult result;
        result.ok = false;
        result.code = failureCode;
        result.message = failureMessage;
        return result;
    }
};
```

Add these pure virtual methods to `ControlApiAdapter`:

```cpp
virtual CommandResult executeCommand(const QString &name, const QJsonObject &args) = 0;
```

Update `FakeControlAdapter` in `tests/unit/tst_controlstate.cpp` with:

```cpp
CommandResult executeCommand(const QString &, const QJsonObject &) override { return CommandResult::success(); }
```

- [ ] **Step 4: Add command validation declarations**

In `websocket/controlprotocol.h`, add inside `ControlProtocol`:

```cpp
struct CommandValidation {
    bool ok = false;
    QString code;
    QString message;
    QJsonObject normalizedArgs;
};

static CommandValidation validateCommand(const ControlCommandMessage &command);
```

- [ ] **Step 5: Implement command validation**

In `websocket/controlprotocol.cpp`, add helper functions in an anonymous namespace after includes:

```cpp
namespace {

bool hasInteger(const QJsonObject &args, const QString &key) {
    const QJsonValue value = args.value(key);
    return value.isDouble() && qFuzzyCompare(value.toDouble(), double(value.toInt()));
}

bool hasNumber(const QJsonObject &args, const QString &key) {
    return args.value(key).isDouble();
}

bool hasBool(const QJsonObject &args, const QString &key) {
    return args.value(key).isBool();
}

bool hasString(const QJsonObject &args, const QString &key) {
    return args.value(key).isString();
}

ControlProtocol::CommandValidation valid(QJsonObject args = {}) {
    ControlProtocol::CommandValidation validation;
    validation.ok = true;
    validation.normalizedArgs = args;
    return validation;
}

ControlProtocol::CommandValidation invalid(const QString &message) {
    ControlProtocol::CommandValidation validation;
    validation.ok = false;
    validation.code = QStringLiteral("invalid_args");
    validation.message = message;
    return validation;
}

} // namespace
```

Add method implementation:

```cpp
ControlProtocol::CommandValidation ControlProtocol::validateCommand(const ControlCommandMessage &command) {
    const QJsonObject args = command.args;
    const QString name = command.name;

    if (name == QStringLiteral("transport.playPause") ||
        name == QStringLiteral("transport.play") ||
        name == QStringLiteral("transport.pause") ||
        name == QStringLiteral("transport.goLive") ||
        name == QStringLiteral("transport.cancelFollowLive") ||
        name == QStringLiteral("recording.start") ||
        name == QStringLiteral("recording.stop") ||
        name == QStringLiteral("recording.toggle") ||
        name == QStringLiteral("view.showMultiview") ||
        name == QStringLiteral("capture.current") ||
        name == QStringLiteral("sources.add") ||
        name == QStringLiteral("settings.save") ||
        name == QStringLiteral("import.read") ||
        name == QStringLiteral("import.applyPreview") ||
        name == QStringLiteral("midi.refreshPorts") ||
        name == QStringLiteral("streamDeck.resetDefaults")) {
        return valid(args);
    }

    if (name == QStringLiteral("transport.seek")) {
        return hasInteger(args, QStringLiteral("positionMs"))
            ? valid(args)
            : invalid(QStringLiteral("transport.seek requires integer args.positionMs"));
    }
    if (name == QStringLiteral("transport.setSpeed")) {
        if (!hasNumber(args, QStringLiteral("speed"))) return invalid(QStringLiteral("transport.setSpeed requires number args.speed"));
        if (args.contains(QStringLiteral("playing")) && !hasBool(args, QStringLiteral("playing"))) return invalid(QStringLiteral("transport.setSpeed args.playing must be boolean"));
        return valid(args);
    }
    if (name == QStringLiteral("transport.holdSpeed")) {
        if (!hasNumber(args, QStringLiteral("speed"))) return invalid(QStringLiteral("transport.holdSpeed requires number args.speed"));
        if (!hasBool(args, QStringLiteral("active"))) return invalid(QStringLiteral("transport.holdSpeed requires boolean args.active"));
        return valid(args);
    }
    if (name == QStringLiteral("transport.stepFrame")) {
        return hasInteger(args, QStringLiteral("frames"))
            ? valid(args)
            : invalid(QStringLiteral("transport.stepFrame requires integer args.frames"));
    }
    if (name == QStringLiteral("view.setPlaybackViewState")) {
        if (!hasBool(args, QStringLiteral("singleView"))) return invalid(QStringLiteral("view.setPlaybackViewState requires boolean args.singleView"));
        if (!hasInteger(args, QStringLiteral("selectedIndex"))) return invalid(QStringLiteral("view.setPlaybackViewState requires integer args.selectedIndex"));
        return valid(args);
    }
    if (name == QStringLiteral("view.selectFeed") ||
        name == QStringLiteral("view.toggleSourceEnabled") ||
        name == QStringLiteral("sources.remove") ||
        name == QStringLiteral("midi.setPortIndex")) {
        return hasInteger(args, QStringLiteral("index"))
            ? valid(args)
            : invalid(name + QStringLiteral(" requires integer args.index"));
    }
    if (name == QStringLiteral("capture.snapshot")) {
        if (!hasBool(args, QStringLiteral("singleView"))) return invalid(QStringLiteral("capture.snapshot requires boolean args.singleView"));
        if (!hasInteger(args, QStringLiteral("selectedIndex"))) return invalid(QStringLiteral("capture.snapshot requires integer args.selectedIndex"));
        if (!hasInteger(args, QStringLiteral("playheadMs"))) return invalid(QStringLiteral("capture.snapshot requires integer args.playheadMs"));
        return valid(args);
    }
    if (name == QStringLiteral("sources.updateUrl")) {
        if (!hasInteger(args, QStringLiteral("index"))) return invalid(QStringLiteral("sources.updateUrl requires integer args.index"));
        if (!hasString(args, QStringLiteral("url"))) return invalid(QStringLiteral("sources.updateUrl requires string args.url"));
        return valid(args);
    }
    if (name == QStringLiteral("sources.updateName")) {
        if (!hasInteger(args, QStringLiteral("index"))) return invalid(QStringLiteral("sources.updateName requires integer args.index"));
        if (!hasString(args, QStringLiteral("name"))) return invalid(QStringLiteral("sources.updateName requires string args.name"));
        return valid(args);
    }
    if (name == QStringLiteral("sources.updateId")) {
        if (!hasInteger(args, QStringLiteral("index"))) return invalid(QStringLiteral("sources.updateId requires integer args.index"));
        if (!hasString(args, QStringLiteral("id"))) return invalid(QStringLiteral("sources.updateId requires string args.id"));
        return valid(args);
    }
    if (name == QStringLiteral("sources.setTrimOffset")) {
        if (!hasInteger(args, QStringLiteral("index"))) return invalid(QStringLiteral("sources.setTrimOffset requires integer args.index"));
        if (!hasInteger(args, QStringLiteral("ms"))) return invalid(QStringLiteral("sources.setTrimOffset requires integer args.ms"));
        return valid(args);
    }
    if (name == QStringLiteral("sources.setMetadata")) {
        if (!hasInteger(args, QStringLiteral("index"))) return invalid(QStringLiteral("sources.setMetadata requires integer args.index"));
        if (!args.value(QStringLiteral("items")).isArray()) return invalid(QStringLiteral("sources.setMetadata requires array args.items"));
        return valid(args);
    }
    if (name == QStringLiteral("settings.setProject")) {
        if (args.contains(QStringLiteral("fileName")) && !hasString(args, QStringLiteral("fileName"))) return invalid(QStringLiteral("settings.setProject args.fileName must be string"));
        if (args.contains(QStringLiteral("saveLocation")) && !hasString(args, QStringLiteral("saveLocation"))) return invalid(QStringLiteral("settings.setProject args.saveLocation must be string"));
        return valid(args);
    }
    if (name == QStringLiteral("settings.setRecordingFormat")) {
        if (args.contains(QStringLiteral("width")) && !hasInteger(args, QStringLiteral("width"))) return invalid(QStringLiteral("settings.setRecordingFormat args.width must be integer"));
        if (args.contains(QStringLiteral("height")) && !hasInteger(args, QStringLiteral("height"))) return invalid(QStringLiteral("settings.setRecordingFormat args.height must be integer"));
        if (args.contains(QStringLiteral("fps")) && !hasInteger(args, QStringLiteral("fps"))) return invalid(QStringLiteral("settings.setRecordingFormat args.fps must be integer"));
        return valid(args);
    }
    if (name == QStringLiteral("settings.setAudioOutputLatency")) {
        return hasInteger(args, QStringLiteral("ms"))
            ? valid(args)
            : invalid(QStringLiteral("settings.setAudioOutputLatency requires integer args.ms"));
    }
    if (name == QStringLiteral("settings.setTimeOfDayMode")) {
        return hasBool(args, QStringLiteral("enabled"))
            ? valid(args)
            : invalid(QStringLiteral("settings.setTimeOfDayMode requires boolean args.enabled"));
    }
    if (name == QStringLiteral("settings.setMetadataFields")) {
        return args.value(QStringLiteral("fields")).isArray()
            ? valid(args)
            : invalid(QStringLiteral("settings.setMetadataFields requires array args.fields"));
    }
    if (name == QStringLiteral("import.setUrl")) {
        return hasString(args, QStringLiteral("url"))
            ? valid(args)
            : invalid(QStringLiteral("import.setUrl requires string args.url"));
    }
    if (name == QStringLiteral("midi.beginLearn") ||
        name == QStringLiteral("midi.beginLearnJogForward") ||
        name == QStringLiteral("midi.beginLearnJogBackward") ||
        name == QStringLiteral("midi.clearBinding") ||
        name == QStringLiteral("streamDeck.beginLearn") ||
        name == QStringLiteral("streamDeck.clearBinding")) {
        return hasInteger(args, QStringLiteral("action"))
            ? valid(args)
            : invalid(name + QStringLiteral(" requires integer args.action"));
    }
    if (name == QStringLiteral("action.dispatch")) {
        if (!hasInteger(args, QStringLiteral("actionId"))) return invalid(QStringLiteral("action.dispatch requires integer args.actionId"));
        QJsonObject normalized = args;
        if (!normalized.contains(QStringLiteral("pressed"))) {
            normalized.insert(QStringLiteral("pressed"), true);
        } else if (!hasBool(normalized, QStringLiteral("pressed"))) {
            return invalid(QStringLiteral("action.dispatch args.pressed must be boolean"));
        }
        return valid(normalized);
    }
    if (name == QStringLiteral("action.jog")) {
        return hasInteger(args, QStringLiteral("delta"))
            ? valid(args)
            : invalid(QStringLiteral("action.jog requires integer args.delta"));
    }

    CommandValidation validation;
    validation.ok = false;
    validation.code = QStringLiteral("unknown_command");
    validation.message = QStringLiteral("Unknown command: %1").arg(name);
    return validation;
}
```

- [ ] **Step 6: Run protocol and state tests**

Run:

```bash
cmake --build build --target tst_controlprotocol tst_controlstate
ctest --test-dir build --output-on-failure -R 'tst_control(protocol|state)'
```

Expected: both tests pass.

- [ ] **Step 7: Commit command validation**

Run:

```bash
git add websocket/controlapiadapter.h websocket/controlprotocol.h websocket/controlprotocol.cpp tests/unit/tst_controlprotocol.cpp tests/unit/tst_controlstate.cpp
git commit -m "feat: validate websocket control commands"
```

Expected: commit succeeds.

## Task 5: WebSocket Server With Fake Adapter

**Files:**
- Create: `websocket/controlwebsocketserver.h`
- Create: `websocket/controlwebsocketserver.cpp`
- Create: `tests/unit/tst_controlwebsocketserver.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Register failing WebSocket server test**

Add to `tests/unit/CMakeLists.txt`:

```cmake
olr_add_unit_test(tst_controlwebsocketserver olr_test_core)
```

Create `tests/unit/tst_controlwebsocketserver.cpp`:

```cpp
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QWebSocket>

#include "websocket/controlapiadapter.h"
#include "websocket/controlwebsocketserver.h"

class ServerFakeAdapter final : public QObject, public ControlApiAdapter {
    Q_OBJECT
public:
    RecordingState recordingState() const override { return {}; }
    TransportState transportState() const override {
        TransportState state;
        state.timecode = QStringLiteral("00:00:00:00");
        state.fps = 30;
        return state;
    }
    QVector<SourceState> sourceStates() const override { return {}; }
    ViewState viewState() const override { return {}; }
    SettingsState settingsState() const override { return {}; }
    MidiState midiState() const override { return {}; }
    StreamDeckState streamDeckState() const override { return {}; }
    ScreensState screensState() const override { return {}; }
    ImportState importState() const override { return {}; }
    TelemetryState telemetryState() const override { return {}; }
    CommandResult executeCommand(const QString &name, const QJsonObject &args) override {
        lastCommand = name;
        lastArgs = args;
        return CommandResult::success();
    }

    QString lastCommand;
    QJsonObject lastArgs;
};

class TestControlWebSocketServer : public QObject {
    Q_OBJECT
private slots:
    void sendsSnapshotAndTimecodeOnConnect();
    void dispatchesCommandAndSendsAck();
    void sendsErrorForBadJson();
};

static QJsonObject parseObject(const QString &message) {
    return QJsonDocument::fromJson(message.toUtf8()).object();
}

void TestControlWebSocketServer::sendsSnapshotAndTimecodeOnConnect() {
    ServerFakeAdapter adapter;
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket client;
    QStringList messages;
    connect(&client, &QWebSocket::textMessageReceived, this, [&messages](const QString &message) {
        messages.append(message);
    });

    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/api/ws").arg(server.serverPort())));

    QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 2, 2000);
    QCOMPARE(parseObject(messages.at(0)).value(QStringLiteral("type")).toString(), QStringLiteral("state.snapshot"));
    QCOMPARE(parseObject(messages.at(1)).value(QStringLiteral("type")).toString(), QStringLiteral("timecode"));
}

void TestControlWebSocketServer::dispatchesCommandAndSendsAck() {
    ServerFakeAdapter adapter;
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket client;
    QStringList messages;
    connect(&client, &QWebSocket::textMessageReceived, this, [&messages](const QString &message) {
        messages.append(message);
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/api/ws").arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 2, 2000);

    client.sendTextMessage(QStringLiteral("{\"type\":\"command\",\"id\":\"seek-1\",\"name\":\"transport.seek\",\"args\":{\"positionMs\":321}}"));

    QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 3, 2000);
    const QJsonObject ack = parseObject(messages.last());
    QCOMPARE(adapter.lastCommand, QStringLiteral("transport.seek"));
    QCOMPARE(adapter.lastArgs.value(QStringLiteral("positionMs")).toInt(), 321);
    QCOMPARE(ack.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(ack.value(QStringLiteral("id")).toString(), QStringLiteral("seek-1"));
    QCOMPARE(ack.value(QStringLiteral("ok")).toBool(), true);
}

void TestControlWebSocketServer::sendsErrorForBadJson() {
    ServerFakeAdapter adapter;
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket client;
    QStringList messages;
    connect(&client, &QWebSocket::textMessageReceived, this, [&messages](const QString &message) {
        messages.append(message);
    });
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/api/ws").arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 2, 2000);

    client.sendTextMessage(QStringLiteral("{ nope"));

    QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 3, 2000);
    const QJsonObject error = parseObject(messages.last());
    QCOMPARE(error.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(error.value(QStringLiteral("code")).toString(), QStringLiteral("bad_json"));
}

QTEST_GUILESS_MAIN(TestControlWebSocketServer)
#include "tst_controlwebsocketserver.moc"
```

- [ ] **Step 2: Run server test and verify RED**

Run:

```bash
cmake --build build --target tst_controlwebsocketserver
```

Expected: build fails because `controlwebsocketserver.h` does not exist.

- [ ] **Step 3: Add server header**

Create `websocket/controlwebsocketserver.h`:

```cpp
#ifndef CONTROLWEBSOCKETSERVER_H
#define CONTROLWEBSOCKETSERVER_H

#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

class QWebSocket;
class QWebSocketServer;
class ControlApiAdapter;

class ControlWebSocketServer : public QObject {
    Q_OBJECT
public:
    explicit ControlWebSocketServer(ControlApiAdapter *adapter, QObject *parent = nullptr);
    ~ControlWebSocketServer() override;

    bool listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 8115);
    quint16 serverPort() const;
    QString lastError() const { return m_lastError; }

public slots:
    void publishPatch(const QString &path);
    void publishEvent(const QString &name, const QJsonObject &data = {});
    void publishTimecodeNow();
    void scheduleTimecode();

private slots:
    void handleNewConnection();
    void handleTextMessage(const QString &message);
    void handleBinaryMessage(const QByteArray &message);
    void handleSocketDisconnected();

private:
    void sendJson(QWebSocket *socket, const QJsonObject &object);
    void broadcastJson(const QJsonObject &object);

    ControlApiAdapter *m_adapter = nullptr;
    QWebSocketServer *m_server = nullptr;
    QSet<QWebSocket*> m_clients;
    QTimer m_timecodeTimer;
    QString m_lastError;
};

#endif
```

- [ ] **Step 4: Add server implementation**

Create `websocket/controlwebsocketserver.cpp`:

```cpp
#include "controlwebsocketserver.h"

#include "controlapiadapter.h"
#include "controlprotocol.h"
#include "controlstate.h"

#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

ControlWebSocketServer::ControlWebSocketServer(ControlApiAdapter *adapter, QObject *parent)
    : QObject(parent), m_adapter(adapter),
      m_server(new QWebSocketServer(QStringLiteral("OpenLiveReplay Control API"),
                                    QWebSocketServer::NonSecureMode, this)) {
    connect(m_server, &QWebSocketServer::newConnection,
            this, &ControlWebSocketServer::handleNewConnection);
    m_timecodeTimer.setSingleShot(true);
    m_timecodeTimer.setInterval(100);
    connect(&m_timecodeTimer, &QTimer::timeout, this, &ControlWebSocketServer::publishTimecodeNow);
}

ControlWebSocketServer::~ControlWebSocketServer() {
    for (QWebSocket *client : std::as_const(m_clients)) {
        client->close();
        client->deleteLater();
    }
}

bool ControlWebSocketServer::listen(const QHostAddress &address, quint16 port) {
    if (!m_adapter) {
        m_lastError = QStringLiteral("ControlWebSocketServer requires an adapter");
        return false;
    }
    if (!m_server->listen(address, port)) {
        m_lastError = m_server->errorString();
        return false;
    }
    m_lastError.clear();
    return true;
}

quint16 ControlWebSocketServer::serverPort() const {
    return m_server ? m_server->serverPort() : 0;
}

void ControlWebSocketServer::handleNewConnection() {
    QWebSocket *socket = m_server->nextPendingConnection();
    if (!socket) return;
    m_clients.insert(socket);
    connect(socket, &QWebSocket::textMessageReceived,
            this, &ControlWebSocketServer::handleTextMessage);
    connect(socket, &QWebSocket::binaryMessageReceived,
            this, &ControlWebSocketServer::handleBinaryMessage);
    connect(socket, &QWebSocket::disconnected,
            this, &ControlWebSocketServer::handleSocketDisconnected);

    sendJson(socket, ControlState::snapshotMessage(*m_adapter));
    sendJson(socket, ControlState::timecodeMessage(*m_adapter));
}

void ControlWebSocketServer::handleTextMessage(const QString &message) {
    auto *socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    const ControlProtocol::ParseResult parsed =
        ControlProtocol::parseTextMessage(message.toUtf8());
    if (!parsed.ok) {
        if (!parsed.id.isEmpty()) {
            sendJson(socket, ControlProtocol::ackError(parsed.id, parsed.code, parsed.messageText));
        } else {
            sendJson(socket, ControlProtocol::error(parsed.code, parsed.messageText));
        }
        return;
    }

    const ControlProtocol::CommandValidation validation =
        ControlProtocol::validateCommand(parsed.message);
    if (!validation.ok) {
        sendJson(socket, ControlProtocol::ackError(parsed.message.id, validation.code, validation.message));
        return;
    }

    const CommandResult result = m_adapter->executeCommand(parsed.message.name, validation.normalizedArgs);
    if (result.ok) {
        sendJson(socket, ControlProtocol::ack(parsed.message.id));
    } else {
        sendJson(socket, ControlProtocol::ackError(parsed.message.id, result.code, result.message));
    }
}

void ControlWebSocketServer::handleBinaryMessage(const QByteArray &) {
    auto *socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;
    sendJson(socket, ControlProtocol::error(QStringLiteral("unsupported_message"),
                                            QStringLiteral("Binary messages are not supported")));
}

void ControlWebSocketServer::handleSocketDisconnected() {
    auto *socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;
    m_clients.remove(socket);
    socket->deleteLater();
}

void ControlWebSocketServer::publishPatch(const QString &path) {
    if (!m_adapter) return;
    QJsonObject value;
    if (path == QStringLiteral("recording")) value = ControlState::recordingObject(*m_adapter);
    else if (path == QStringLiteral("transport")) value = ControlState::transportObject(*m_adapter);
    else if (path == QStringLiteral("settings")) value = ControlState::settingsObject(*m_adapter);
    else if (path == QStringLiteral("telemetry")) value = ControlState::telemetryObject(*m_adapter);
    else return;
    broadcastJson(ControlState::patchMessage(path, value));
}

void ControlWebSocketServer::publishEvent(const QString &name, const QJsonObject &data) {
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("event"));
    event.insert(QStringLiteral("name"), name);
    event.insert(QStringLiteral("data"), data);
    broadcastJson(event);
}

void ControlWebSocketServer::publishTimecodeNow() {
    if (!m_adapter) return;
    m_timecodeTimer.stop();
    broadcastJson(ControlState::timecodeMessage(*m_adapter));
}

void ControlWebSocketServer::scheduleTimecode() {
    if (!m_timecodeTimer.isActive()) {
        m_timecodeTimer.start();
    }
}

void ControlWebSocketServer::sendJson(QWebSocket *socket, const QJsonObject &object) {
    if (!socket) return;
    socket->sendTextMessage(QString::fromUtf8(ControlProtocol::compact(object)));
}

void ControlWebSocketServer::broadcastJson(const QJsonObject &object) {
    const QString payload = QString::fromUtf8(ControlProtocol::compact(object));
    for (QWebSocket *client : std::as_const(m_clients)) {
        client->sendTextMessage(payload);
    }
}
```

- [ ] **Step 5: Add server source to test core**

In `tests/CMakeLists.txt`, add to `olr_test_core` sources:

```cmake
"${CMAKE_SOURCE_DIR}/websocket/controlwebsocketserver.cpp"
```

- [ ] **Step 6: Run server tests and verify GREEN**

Run:

```bash
cmake --build build --target tst_controlwebsocketserver
ctest --test-dir build --output-on-failure -R tst_controlwebsocketserver
```

Expected: `tst_controlwebsocketserver` passes.

- [ ] **Step 7: Commit testable WebSocket server**

Run:

```bash
git add websocket/controlwebsocketserver.h websocket/controlwebsocketserver.cpp tests/unit/tst_controlwebsocketserver.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add websocket control server"
```

Expected: commit succeeds.

## Task 6: UIManager External Control Surface

**Files:**
- Modify: `uimanager.h`
- Modify: `uimanager.cpp`

- [ ] **Step 1: Add public methods and signals to `uimanager.h`**

In `uimanager.h`, add public invokables near the existing playback invokables:

```cpp
Q_INVOKABLE void dispatchExternalAction(int action, bool pressed);
Q_INVOKABLE void jogExternal(int delta);
Q_INVOKABLE void shuttleExternal(int delta);
Q_INVOKABLE void selectFeedExternal(int index);
Q_INVOKABLE bool playbackSingleView() const { return m_playbackSingleView; }
Q_INVOKABLE int playbackSelectedIndex() const { return m_playbackSelectedIndex; }
```

Add signals near existing state signals:

```cpp
void playbackViewStateChanged();
void metadataFieldsChanged();
void sourceMetadataChanged();
```

- [ ] **Step 2: Implement external methods in `uimanager.cpp`**

Add after `UIManager::dispatchControlAction`:

```cpp
void UIManager::dispatchExternalAction(int action, bool pressed)
{
    dispatchControlAction(action, !pressed);
}

void UIManager::jogExternal(int delta)
{
    jogStep(delta);
}

void UIManager::shuttleExternal(int delta)
{
    shuttleStep(delta);
}

void UIManager::selectFeedExternal(int index)
{
    emit feedSelectRequested(index);
}
```

- [ ] **Step 3: Emit view and metadata change signals**

In `UIManager::setPlaybackViewState`, after updating members, add:

```cpp
emit playbackViewStateChanged();
```

In `UIManager::setMetadataFieldDefinitions`, after assigning `m_currentSettings.metadataFields = arr;`, add:

```cpp
emit metadataFieldsChanged();
emit viewSlotMapChanged();
m_settingsManager->save(m_configPath, m_currentSettings);
```

In `UIManager::setSourceMetadataItems`, after assigning source metadata, add:

```cpp
emit sourceMetadataChanged();
emit viewSlotMapChanged();
m_settingsManager->save(m_configPath, m_currentSettings);
```

- [ ] **Step 4: Build the app target**

Run:

```bash
cmake --build build --target OpenLiveReplay
```

Expected: app target builds. If FFmpeg platform dependencies block the app build, build `tst_controlstate` and record the app-build dependency failure for final verification.

- [ ] **Step 5: Commit UIManager control surface**

Run:

```bash
git add uimanager.h uimanager.cpp
git commit -m "feat: expose external control hooks"
```

Expected: commit succeeds.

## Task 7: UIManager Adapter

**Files:**
- Create: `websocket/uimanagercontroladapter.h`
- Create: `websocket/uimanagercontroladapter.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create adapter header**

Create `websocket/uimanagercontroladapter.h`:

```cpp
#ifndef UIMANAGERCONTROLADAPTER_H
#define UIMANAGERCONTROLADAPTER_H

#include "controlapiadapter.h"

#include <QObject>

class UIManager;

class UIManagerControlAdapter : public QObject, public ControlApiAdapter {
    Q_OBJECT
public:
    explicit UIManagerControlAdapter(UIManager *uiManager, QObject *parent = nullptr);

    RecordingState recordingState() const override;
    TransportState transportState() const override;
    QVector<SourceState> sourceStates() const override;
    ViewState viewState() const override;
    SettingsState settingsState() const override;
    MidiState midiState() const override;
    StreamDeckState streamDeckState() const override;
    ScreensState screensState() const override;
    ImportState importState() const override;
    TelemetryState telemetryState() const override;
    CommandResult executeCommand(const QString &name, const QJsonObject &args) override;

private:
    UIManager *m_uiManager = nullptr;
};

#endif
```

- [ ] **Step 2: Create adapter implementation**

Create `websocket/uimanagercontroladapter.cpp`:

```cpp
#include "uimanagercontroladapter.h"

#include "uimanager.h"
#include "playback/playbacktransport.h"
#include "streamdeck/streamdeckmanager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

UIManagerControlAdapter::UIManagerControlAdapter(UIManager *uiManager, QObject *parent)
    : QObject(parent), m_uiManager(uiManager) {}

RecordingState UIManagerControlAdapter::recordingState() const {
    return {m_uiManager->isRecording(), m_uiManager->recordedDurationMs(), m_uiManager->recordingStartEpochMs()};
}

TransportState UIManagerControlAdapter::transportState() const {
    PlaybackTransport *transport = m_uiManager->transport();
    TransportState state;
    state.positionMs = transport ? transport->currentPos() : 0;
    state.scrubPositionMs = m_uiManager->scrubPosition();
    state.durationMs = m_uiManager->recordedDurationMs();
    state.timecode = m_uiManager->playbackTimecode();
    state.playing = transport ? transport->isPlaying() : false;
    state.speed = transport ? transport->speed() : 1.0;
    state.fps = transport ? transport->fps() : m_uiManager->recordFps();
    state.followLive = m_uiManager->followLive();
    state.liveBufferMs = m_uiManager->liveBufferMs();
    return state;
}

QVector<SourceState> UIManagerControlAdapter::sourceStates() const {
    QVector<SourceState> states;
    const QStringList ids = m_uiManager->streamIds();
    const QStringList names = m_uiManager->streamNames();
    const QStringList urls = m_uiManager->streamUrls();
    states.reserve(urls.size());
    for (int i = 0; i < urls.size(); ++i) {
        SourceState state;
        state.index = i;
        state.id = ids.value(i);
        state.name = names.value(i);
        state.url = urls.value(i);
        state.enabled = m_uiManager->isSourceEnabled(i);
        state.connected = m_uiManager->isSourceConnected(i);
        state.duplicateUrl = m_uiManager->hasDuplicateUrl(i);
        state.trimOffsetMs = m_uiManager->sourceTrimOffset(i);
        state.metadata = m_uiManager->sourceMetadataItems(i);
        states.append(state);
    }
    return states;
}

ViewState UIManagerControlAdapter::viewState() const {
    ViewState state;
    state.multiviewCount = m_uiManager->multiviewCount();
    state.slotMap = m_uiManager->viewSlotMap();
    state.singleView = m_uiManager->playbackSingleView();
    state.selectedIndex = m_uiManager->playbackSelectedIndex();
    return state;
}

SettingsState UIManagerControlAdapter::settingsState() const {
    SettingsState state;
    state.fileName = m_uiManager->fileName();
    state.saveLocation = m_uiManager->saveLocation();
    state.recordWidth = m_uiManager->recordWidth();
    state.recordHeight = m_uiManager->recordHeight();
    state.recordFps = m_uiManager->recordFps();
    state.audioOutputLatencyMs = m_uiManager->audioOutputLatencyMs();
    state.timeOfDayMode = m_uiManager->timeOfDayMode();
    state.metadataFields = m_uiManager->metadataFieldDefinitions();
    return state;
}

MidiState UIManagerControlAdapter::midiState() const {
    return {m_uiManager->midiPorts(), m_uiManager->midiPortIndex(),
            m_uiManager->midiPortName(), m_uiManager->midiConnected(),
            m_uiManager->midiLearnAction(), m_uiManager->midiLearnMode()};
}

StreamDeckState UIManagerControlAdapter::streamDeckState() const {
    StreamDeckManager *deck = m_uiManager->streamDeck();
    if (!deck) return {};
    return {deck->supported(), deck->connected(), deck->deviceName(), deck->deviceModel(),
            deck->keyCount(), deck->dialCount(), m_uiManager->streamDeckLearnAction()};
}

ScreensState UIManagerControlAdapter::screensState() const {
    return {m_uiManager->screensReady(), m_uiManager->screenCount(), m_uiManager->screenOptions()};
}

ImportState UIManagerControlAdapter::importState() const {
    return {m_uiManager->importSettingsUrl(), m_uiManager->telemetrySseUrl(),
            m_uiManager->importPreviewReady(), m_uiManager->importPreviewError(),
            m_uiManager->importPreview()};
}

TelemetryState UIManagerControlAdapter::telemetryState() const {
    return {m_uiManager->telemetryVersion(),
            m_uiManager->telemetryRowsAtPlayhead(),
            m_uiManager->telemetryAtPlayhead()};
}

CommandResult UIManagerControlAdapter::executeCommand(const QString &name, const QJsonObject &args) {
    if (!m_uiManager) return CommandResult::failure(QStringLiteral("failed"), QStringLiteral("UIManager is unavailable"));
    PlaybackTransport *transport = m_uiManager->transport();

    if (name == QStringLiteral("transport.playPause")) m_uiManager->playPause();
    else if (name == QStringLiteral("transport.play")) { if (transport) transport->setPlaying(true); }
    else if (name == QStringLiteral("transport.pause")) { if (transport) transport->setPlaying(false); }
    else if (name == QStringLiteral("transport.setSpeed")) {
        if (transport) {
            transport->setSpeed(args.value(QStringLiteral("speed")).toDouble());
            if (args.contains(QStringLiteral("playing"))) transport->setPlaying(args.value(QStringLiteral("playing")).toBool());
        }
    }
    else if (name == QStringLiteral("transport.holdSpeed")) {
        if (transport) {
            if (args.value(QStringLiteral("active")).toBool()) {
                transport->setSpeed(args.value(QStringLiteral("speed")).toDouble());
                transport->setPlaying(true);
            } else {
                transport->setSpeed(1.0);
            }
        }
    }
    else if (name == QStringLiteral("transport.stepFrame")) m_uiManager->jogExternal(args.value(QStringLiteral("frames")).toInt());
    else if (name == QStringLiteral("transport.seek")) m_uiManager->seekPlayback(args.value(QStringLiteral("positionMs")).toVariant().toLongLong());
    else if (name == QStringLiteral("transport.goLive")) m_uiManager->goLive();
    else if (name == QStringLiteral("transport.cancelFollowLive")) m_uiManager->cancelFollowLive();
    else if (name == QStringLiteral("recording.start")) m_uiManager->startRecording();
    else if (name == QStringLiteral("recording.stop")) m_uiManager->stopRecording();
    else if (name == QStringLiteral("recording.toggle")) { if (m_uiManager->isRecording()) m_uiManager->stopRecording(); else m_uiManager->startRecording(); }
    else if (name == QStringLiteral("view.setPlaybackViewState")) m_uiManager->setPlaybackViewState(args.value(QStringLiteral("singleView")).toBool(), args.value(QStringLiteral("selectedIndex")).toInt());
    else if (name == QStringLiteral("view.showMultiview")) { m_uiManager->setPlaybackViewState(false, -1); m_uiManager->dispatchExternalAction(6, true); }
    else if (name == QStringLiteral("view.selectFeed")) m_uiManager->selectFeedExternal(args.value(QStringLiteral("index")).toInt());
    else if (name == QStringLiteral("view.toggleSourceEnabled")) m_uiManager->toggleSourceEnabled(args.value(QStringLiteral("index")).toInt());
    else if (name == QStringLiteral("capture.current")) m_uiManager->captureCurrent();
    else if (name == QStringLiteral("capture.snapshot")) m_uiManager->captureSnapshot(args.value(QStringLiteral("singleView")).toBool(), args.value(QStringLiteral("selectedIndex")).toInt(), args.value(QStringLiteral("playheadMs")).toVariant().toLongLong());
    else if (name == QStringLiteral("sources.add")) m_uiManager->addStream();
    else if (name == QStringLiteral("sources.remove")) m_uiManager->removeStream(args.value(QStringLiteral("index")).toInt());
    else if (name == QStringLiteral("sources.updateUrl")) m_uiManager->updateUrl(args.value(QStringLiteral("index")).toInt(), args.value(QStringLiteral("url")).toString());
    else if (name == QStringLiteral("sources.updateName")) m_uiManager->updateStreamName(args.value(QStringLiteral("index")).toInt(), args.value(QStringLiteral("name")).toString());
    else if (name == QStringLiteral("sources.updateId")) m_uiManager->updateStreamId(args.value(QStringLiteral("index")).toInt(), args.value(QStringLiteral("id")).toString());
    else if (name == QStringLiteral("sources.setTrimOffset")) m_uiManager->setSourceTrimOffset(args.value(QStringLiteral("index")).toInt(), args.value(QStringLiteral("ms")).toInt());
    else if (name == QStringLiteral("sources.setMetadata")) m_uiManager->setSourceMetadataItems(args.value(QStringLiteral("index")).toInt(), args.value(QStringLiteral("items")).toArray().toVariantList());
    else if (name == QStringLiteral("settings.setProject")) {
        if (args.contains(QStringLiteral("fileName"))) m_uiManager->setFileName(args.value(QStringLiteral("fileName")).toString());
        if (args.contains(QStringLiteral("saveLocation"))) m_uiManager->setSaveLocation(args.value(QStringLiteral("saveLocation")).toString());
    }
    else if (name == QStringLiteral("settings.setRecordingFormat")) {
        if (m_uiManager->isRecording() && args.contains(QStringLiteral("fps"))) return CommandResult::failure(QStringLiteral("not_allowed"), QStringLiteral("FPS cannot be changed while recording"));
        if (args.contains(QStringLiteral("width"))) m_uiManager->setRecordWidth(args.value(QStringLiteral("width")).toInt());
        if (args.contains(QStringLiteral("height"))) m_uiManager->setRecordHeight(args.value(QStringLiteral("height")).toInt());
        if (args.contains(QStringLiteral("fps"))) m_uiManager->setRecordFps(args.value(QStringLiteral("fps")).toInt());
    }
    else if (name == QStringLiteral("settings.setAudioOutputLatency")) m_uiManager->setAudioOutputLatencyMs(args.value(QStringLiteral("ms")).toInt());
    else if (name == QStringLiteral("settings.setTimeOfDayMode")) m_uiManager->setTimeOfDayMode(args.value(QStringLiteral("enabled")).toBool());
    else if (name == QStringLiteral("settings.setMetadataFields")) m_uiManager->setMetadataFieldDefinitions(args.value(QStringLiteral("fields")).toArray().toVariantList());
    else if (name == QStringLiteral("settings.save")) m_uiManager->saveSettings();
    else if (name == QStringLiteral("import.setUrl")) m_uiManager->setImportSettingsUrl(args.value(QStringLiteral("url")).toString());
    else if (name == QStringLiteral("import.read")) m_uiManager->readImportSettings();
    else if (name == QStringLiteral("import.applyPreview")) m_uiManager->applyImportPreview();
    else if (name == QStringLiteral("midi.refreshPorts")) m_uiManager->refreshMidiPorts();
    else if (name == QStringLiteral("midi.setPortIndex")) m_uiManager->setMidiPortIndex(args.value(QStringLiteral("index")).toInt());
    else if (name == QStringLiteral("midi.beginLearn")) m_uiManager->beginMidiLearn(args.value(QStringLiteral("action")).toInt());
    else if (name == QStringLiteral("midi.beginLearnJogForward")) m_uiManager->beginMidiLearnJogForward(args.value(QStringLiteral("action")).toInt());
    else if (name == QStringLiteral("midi.beginLearnJogBackward")) m_uiManager->beginMidiLearnJogBackward(args.value(QStringLiteral("action")).toInt());
    else if (name == QStringLiteral("midi.clearBinding")) m_uiManager->clearMidiBinding(args.value(QStringLiteral("action")).toInt());
    else if (name == QStringLiteral("streamDeck.beginLearn")) m_uiManager->beginStreamDeckLearn(args.value(QStringLiteral("action")).toInt());
    else if (name == QStringLiteral("streamDeck.clearBinding")) m_uiManager->clearStreamDeckBinding(args.value(QStringLiteral("action")).toInt());
    else if (name == QStringLiteral("streamDeck.resetDefaults")) m_uiManager->resetStreamDeckDefaults();
    else if (name == QStringLiteral("action.dispatch")) m_uiManager->dispatchExternalAction(args.value(QStringLiteral("actionId")).toInt(), args.value(QStringLiteral("pressed")).toBool(true));
    else if (name == QStringLiteral("action.jog")) m_uiManager->jogExternal(args.value(QStringLiteral("delta")).toInt());
    else return CommandResult::failure(QStringLiteral("unknown_command"), QStringLiteral("Unknown command"));

    return CommandResult::success();
}
```

- [ ] **Step 3: Add app sources to CMake**

In `CMakeLists.txt`, add these sources to `qt_add_qml_module(OpenLiveReplay ... SOURCES ...)`:

```cmake
websocket/controlapiadapter.h
websocket/controlprotocol.h websocket/controlprotocol.cpp
websocket/controlstate.h websocket/controlstate.cpp
websocket/controlwebsocketserver.h websocket/controlwebsocketserver.cpp
websocket/uimanagercontroladapter.h websocket/uimanagercontroladapter.cpp
```

- [ ] **Step 4: Build app target**

Run:

```bash
cmake --build build --target OpenLiveReplay
```

Expected: app target builds, or fails only on existing platform multimedia/FFmpeg dependency discovery noted in Task 6.

- [ ] **Step 5: Commit adapter**

Run:

```bash
git add websocket/uimanagercontroladapter.h websocket/uimanagercontroladapter.cpp CMakeLists.txt
git commit -m "feat: adapt websocket api to ui manager"
```

Expected: commit succeeds.

## Task 8: Real App Server Wiring And State Publications

**Files:**
- Modify: `main.cpp`
- Modify: `websocket/controlwebsocketserver.h`
- Modify: `websocket/controlwebsocketserver.cpp`

- [ ] **Step 1: Include WebSocket server and adapter in `main.cpp`**

Add includes:

```cpp
#include "websocket/controlwebsocketserver.h"
#include "websocket/controlstate.h"
#include "websocket/uimanagercontroladapter.h"
#include <QHostAddress>
#include <QWarning>
```

After `uiManager.loadSettings();`, add:

```cpp
UIManagerControlAdapter controlAdapter(&uiManager);
ControlWebSocketServer controlServer(&controlAdapter);
if (!controlServer.listen(QHostAddress::Any, 8115)) {
    qWarning() << "WebSocket control API failed to listen on port 8115:"
               << controlServer.lastError();
}
```

- [ ] **Step 2: Add a method for complete object patches**

In `websocket/controlwebsocketserver.h`, replace:

```cpp
void publishPatch(const QString &path);
```

with:

```cpp
void publishPatch(const QString &path);
void publishPatchObject(const QString &path, const QJsonObject &value);
```

In `websocket/controlwebsocketserver.cpp`, add:

```cpp
void ControlWebSocketServer::publishPatchObject(const QString &path, const QJsonObject &value) {
    broadcastJson(ControlState::patchMessage(path, value));
}
```

- [ ] **Step 3: Wire UIManager and transport signals in `main.cpp`**

After the listen block, add:

```cpp
auto publishRecording = [&controlServer]() { controlServer.publishPatch(QStringLiteral("recording")); };
auto publishTransport = [&controlServer]() {
    controlServer.publishPatch(QStringLiteral("transport"));
    controlServer.publishTimecodeNow();
};
auto publishSettings = [&controlServer]() { controlServer.publishPatch(QStringLiteral("settings")); };
auto publishTelemetry = [&controlServer]() { controlServer.publishPatch(QStringLiteral("telemetry")); };

QObject::connect(&uiManager, &UIManager::recordingStatusChanged, &controlServer, publishRecording);
QObject::connect(&uiManager, &UIManager::recordingStartEpochMsChanged, &controlServer, publishRecording);
QObject::connect(&uiManager, &UIManager::recordedDurationMsChanged, &controlServer, publishRecording);
QObject::connect(&uiManager, &UIManager::recordingStarted, &controlServer, [&controlServer]() {
    controlServer.publishEvent(QStringLiteral("recording.started"));
});
QObject::connect(&uiManager, &UIManager::recordingStopped, &controlServer, [&controlServer]() {
    controlServer.publishEvent(QStringLiteral("recording.stopped"));
});
QObject::connect(&uiManager, &UIManager::recordingFailed, &controlServer, [&controlServer](const QString &reason) {
    controlServer.publishEvent(QStringLiteral("recording.failed"), QJsonObject{{QStringLiteral("reason"), reason}});
});

QObject::connect(uiManager.transport(), &PlaybackTransport::posChanged, &controlServer, [&controlServer]() {
    controlServer.scheduleTimecode();
});
QObject::connect(uiManager.transport(), &PlaybackTransport::playingChanged, &controlServer, publishTransport);
QObject::connect(uiManager.transport(), &PlaybackTransport::speedChanged, &controlServer, publishTransport);
QObject::connect(uiManager.transport(), &PlaybackTransport::fpsChanged, &controlServer, publishTransport);
QObject::connect(&uiManager, &UIManager::playbackTimecodeChanged, &controlServer, [&controlServer]() {
    controlServer.scheduleTimecode();
});
QObject::connect(&uiManager, &UIManager::followLiveChanged, &controlServer, publishTransport);

QObject::connect(&uiManager, &UIManager::saveLocationChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::fileNameChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::recordWidthChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::recordHeightChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::recordFpsChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::audioOutputLatencyChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::timeOfDayModeChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::metadataFieldsChanged, &controlServer, publishSettings);
QObject::connect(&uiManager, &UIManager::telemetryChanged, &controlServer, publishTelemetry);
```

- [ ] **Step 4: Add source/view/import/device patch helpers in `main.cpp`**

Add helper lambdas near the previous signal wiring:

```cpp
auto publishFullSnapshot = [&controlServer, &controlAdapter]() {
    controlServer.publishPatchObject(QStringLiteral("snapshot"), ControlState::snapshotMessage(controlAdapter).value(QStringLiteral("state")).toObject());
};
auto publishSources = publishFullSnapshot;
auto publishViews = publishFullSnapshot;
auto publishImport = publishFullSnapshot;
auto publishMidi = publishFullSnapshot;
auto publishStreamDeck = publishFullSnapshot;
auto publishScreens = publishFullSnapshot;
```

Add connections:

```cpp
QObject::connect(&uiManager, &UIManager::streamUrlsChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::streamNamesChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::streamIdsChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::sourceEnabledChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::sourceConnectionChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::sourceTrimChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::sourceMetadataChanged, &controlServer, publishSources);
QObject::connect(&uiManager, &UIManager::viewSlotMapChanged, &controlServer, publishViews);
QObject::connect(&uiManager, &UIManager::multiviewCountChanged, &controlServer, publishViews);
QObject::connect(&uiManager, &UIManager::playbackViewStateChanged, &controlServer, publishViews);
QObject::connect(&uiManager, &UIManager::importSettingsUrlChanged, &controlServer, publishImport);
QObject::connect(&uiManager, &UIManager::importPreviewChanged, &controlServer, publishImport);
QObject::connect(&uiManager, &UIManager::telemetryConfigChanged, &controlServer, publishImport);
QObject::connect(&uiManager, &UIManager::midiPortsChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::midiPortIndexChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::midiConnectedChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::midiLearnActionChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::midiBindingsChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::midiLastValuesChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::midiPortNameChanged, &controlServer, publishMidi);
QObject::connect(&uiManager, &UIManager::streamDeckLearnActionChanged, &controlServer, publishStreamDeck);
QObject::connect(&uiManager, &UIManager::streamDeckBindingsChanged, &controlServer, publishStreamDeck);
QObject::connect(&uiManager, &UIManager::screensChanged, &controlServer, publishScreens);
```

This uses a full snapshot-valued patch for compound sections in the first implementation. A later revision can add fine-grained source/view serializers without changing the client protocol.

- [ ] **Step 5: Build app target**

Run:

```bash
cmake --build build --target OpenLiveReplay
```

Expected: `OpenLiveReplay` builds.

- [ ] **Step 6: Commit app wiring**

Run:

```bash
git add main.cpp websocket/controlwebsocketserver.h websocket/controlwebsocketserver.cpp
git commit -m "feat: start websocket control api"
```

Expected: commit succeeds.

## Task 9: Protocol Documentation

**Files:**
- Create: `docs/websocket-control-api.md`

- [ ] **Step 1: Add user-facing protocol reference**

Create `docs/websocket-control-api.md`:

```markdown
# WebSocket Control API

OpenLiveReplay starts an unauthenticated WebSocket control API on:

```text
ws://<app-host>:8115/api/ws
```

The server is intended for trusted local-network clients such as PC StreamDeck
plugins. It has no authentication.

## Connection Flow

On connect, the server sends:

```json
{ "type": "state.snapshot", "state": {} }
```

Then it sends the current display timecode:

```json
{
  "type": "timecode",
  "positionMs": 0,
  "durationMs": 0,
  "text": "00:00:00:00",
  "followLive": false,
  "playing": false,
  "speed": 1.0
}
```

## Commands

Send commands as JSON text messages:

```json
{
  "type": "command",
  "id": "button-1",
  "name": "transport.playPause",
  "args": {}
}
```

Successful acknowledgement:

```json
{ "type": "ack", "id": "button-1", "ok": true }
```

Failed acknowledgement:

```json
{
  "type": "ack",
  "id": "button-1",
  "ok": false,
  "code": "invalid_args",
  "message": "transport.seek requires integer args.positionMs"
}
```

## StreamDeck-Style Actions

Press play/pause:

```json
{ "type": "command", "id": "play", "name": "action.dispatch", "args": { "actionId": 0 } }
```

Hold rewind:

```json
{ "type": "command", "id": "rew-down", "name": "action.dispatch", "args": { "actionId": 1, "pressed": true } }
```

Release rewind:

```json
{ "type": "command", "id": "rew-up", "name": "action.dispatch", "args": { "actionId": 1, "pressed": false } }
```

Jog one frame forward:

```json
{ "type": "command", "id": "jog-1", "name": "action.jog", "args": { "delta": 1 } }
```

## Common Semantic Commands

- `transport.playPause`
- `transport.seek` with `{ "positionMs": 1200 }`
- `transport.setSpeed` with `{ "speed": 0.5, "playing": true }`
- `recording.toggle`
- `view.showMultiview`
- `view.selectFeed` with `{ "index": 0 }`
- `sources.updateUrl` with `{ "index": 0, "url": "srt://example" }`
- `settings.save`

## State Updates

The server publishes patches:

```json
{ "type": "state.patch", "path": "transport", "value": {} }
```

and events:

```json
{ "type": "event", "name": "recording.started", "data": {} }
```

Timecode messages are throttled to at most 10 Hz.

## Error Codes

- `bad_json`
- `bad_message`
- `unsupported_message`
- `unknown_command`
- `invalid_args`
- `not_allowed`
- `failed`
```

- [ ] **Step 2: Commit documentation**

Run:

```bash
git add docs/websocket-control-api.md
git commit -m "docs: document websocket control api"
```

Expected: commit succeeds.

## Task 10: Final Verification And PR Prep

**Files:**
- No source edits expected.

- [ ] **Step 1: Run focused unit tests**

Run:

```bash
cmake --build build --target tst_controlprotocol tst_controlstate tst_controlwebsocketserver
ctest --test-dir build --output-on-failure -R 'tst_control(protocol|state|websocketserver)'
```

Expected: all three WebSocket API tests pass.

- [ ] **Step 2: Build the app target**

Run:

```bash
cmake --build build --target OpenLiveReplay
```

Expected: app target builds. If app build is blocked by environment-specific FFmpeg or Qt kit configuration, record the exact failure and ensure WebSocket unit tests still pass.

- [ ] **Step 3: Run existing nearby regression tests**

Run:

```bash
cmake --build build --target tst_playbacktransport tst_sseparser tst_telemetryclient tst_streamdeckmappingstore
ctest --test-dir build --output-on-failure -R 'tst_(playbacktransport|sseparser|telemetryclient|streamdeckmappingstore)'
```

Expected: all selected existing tests pass.

- [ ] **Step 4: Inspect git history and status**

Run:

```bash
git status --short
git log --oneline --decorate -8
```

Expected: status is clean and recent commits are the WebSocket control API commits.

- [ ] **Step 5: Push branch**

Run:

```bash
git push -u origin codex/websocket-control-api
```

Expected: branch pushes successfully.

- [ ] **Step 6: Open PR**

Use the GitHub PR workflow available in the environment. PR title:

```text
Add WebSocket control API
```

PR body:

```markdown
## Summary

- adds an unauthenticated local-network WebSocket control API on port 8115
- exposes command dispatch, state snapshots/patches, events, telemetry, and timecode updates
- documents the protocol for local integrations such as PC StreamDeck plugins

## Tests

- [ ] `ctest --test-dir build --output-on-failure -R 'tst_control(protocol|state|websocketserver)'`
- [ ] `ctest --test-dir build --output-on-failure -R 'tst_(playbacktransport|sseparser|telemetryclient|streamdeckmappingstore)'`
- [ ] `cmake --build build --target OpenLiveReplay`
```

Mark each test item according to the actual verification result before opening the PR.

## Self-Review Checklist

- Spec coverage:
  - Port `8115`, no auth, auto-start: Tasks 1 and 8.
  - Snapshot on connect: Tasks 3 and 5.
  - Commands for UI/MIDI/StreamDeck functionality: Tasks 4, 6, and 7.
  - State changes/events/timecode/telemetry: Tasks 5 and 8.
  - Documentation: Task 9.
  - PR workflow: Task 10.
- Placeholder scan: plan contains no unfinished implementation markers or unspecified steps.
- Type consistency: adapter state structs are introduced in Task 3, command result is added in Task 4, and later tasks use those exact names.
