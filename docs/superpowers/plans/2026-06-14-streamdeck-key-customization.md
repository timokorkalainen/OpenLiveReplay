# Stream Deck Key Customization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a learn-style editor that remaps which replay action each Stream Deck key, dial-press, and dial-turn triggers (incl. a new Shuttle variable-speed action), persisted per deck model.

**Architecture:** A new cross-platform `StreamDeckMappingStore` holds three per-model tables (key→action, dialPress→action, dialRotate→action) with move/displace semantics, validation, and AppSettings (de)serialization. A bridge "learn mode" reports raw pressed elements to Qt; `UIManager` binds them through the store, persists, and pushes the maps to the Swift `DeckState`, which the deck layout renders live. `StreamDeckMappingStore` and the shuttle-ladder helper are unit-tested; the bridge has XCTests.

**Tech Stack:** Qt 6.10 (C++17, CMake, QtTest), Swift 5.9 / SwiftUI / Combine, StreamDeckKit 1.3.0, XCTest.

**Spec:** `docs/superpowers/specs/2026-06-14-streamdeck-key-customization-design.md`

---

## Before You Start

- Work in the worktree: `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/streamdeck-integration` (branch `worktree-streamdeck-integration`). All paths below are relative to it.
- **Warnings are errors** (`olr_warnings` adds `-Werror`). New C++ must be warning-clean (init all members, no unused params — use `Q_UNUSED` or omit names).
- Build/test commands used throughout:
  - **macOS app (StreamDeck stub path):** `cmake --build build -j8` (reconfigure if needed: `cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos`). Expect `[100%] Built target OpenLiveReplay`.
  - **C++ unit tests:** configure once with tests on, then build+run a target:
    ```
    cmake -S . -B build-test -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
    cmake --build build-test --target tst_streamdeckmappingstore -j8
    ctest --test-dir build-test -R streamdeckmappingstore --output-on-failure
    ```
  - **Swift bridge tests:**
    ```
    cd ios/streamdeck-bridge && xcodegen generate
    xcodebuild test -project StreamDeckBridge.xcodeproj -scheme StreamDeckBridge \
      -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' | tail -8
    ```
    (substitute any installed iPad sim from `xcrun simctl list devices available`).
  - **iOS device build (final):** see Task 8.

## Action Id Vocabulary (single source of truth)

Unchanged from the integration, plus **Shuttle = 10**. Kept in sync across `DeckAction.swift`, `streamdeck/streamdeckmanager.h` (doc), and `UIManager::dispatchControlAction`:

`0` play/pause · `1` rewind 5× (hold) · `2` forward 5× (hold) · `3` step fwd · `4` go live · `5` capture · `6` multiview · `7` step back · `8` jog (dial-turn) · `9` record toggle · `10` **shuttle (dial-turn)** · `20` timecode display (key) · `21` speed display (key) · `-1` empty.

Element types (learn): `0` key · `1` dial-press · `2` dial-turn.

---

### Task 1: Persist per-model mapping tables in AppSettings

Add the three tables to `AppSettings` and (de)serialize them in `SettingsManager`, mirroring how the MIDI maps are stored. Extend the existing round-trip test.

**Files:**
- Modify: `settingsmanager.h`
- Modify: `settingsmanager.cpp`
- Modify: `tests/unit/tst_settingsmanager.cpp`

- [ ] **Step 1: Extend the failing round-trip test**

In `tests/unit/tst_settingsmanager.cpp`, inside `sampleSettings()` (just before `return s;`), add:

```cpp
    // Stream Deck per-model mapping tables (model id -> index -> action id).
    s.streamDeckKeyMaps.insert(QStringLiteral("plusXL"),
                               QList<int>{9, 0, 4, -1, -1});
    s.streamDeckDialPressMaps.insert(QStringLiteral("plusXL"),
                                     QList<int>{0, 5, -1, -1, -1, -1});
    s.streamDeckDialRotateMaps.insert(QStringLiteral("plusXL"),
                                      QList<int>{8, 10, -1, -1, -1, -1});
```

Then in `roundTripPreservesEverything()`, after the existing assertions that compare `out` to `in` (find where it verifies `midiBindingData2Backward` or the last field), add:

```cpp
    QCOMPARE(out.streamDeckKeyMaps, in.streamDeckKeyMaps);
    QCOMPARE(out.streamDeckDialPressMaps, in.streamDeckDialPressMaps);
    QCOMPARE(out.streamDeckDialRotateMaps, in.streamDeckDialRotateMaps);
```

- [ ] **Step 2: Run the test, expect failure**

Run:
```
cmake -S . -B build-test -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build-test --target tst_settingsmanager -j8
```
Expected: **compile failure** — `AppSettings` has no `streamDeckKeyMaps`.

- [ ] **Step 3: Add the fields to `AppSettings`**

In `settingsmanager.h`, inside `struct AppSettings`, after `QMap<int, int> midiBindingData2Backward;`, add:

```cpp
    // Stream Deck per-model mappings: model id -> (index -> action id, -1 = unbound).
    QMap<QString, QList<int>> streamDeckKeyMaps;
    QMap<QString, QList<int>> streamDeckDialPressMaps;
    QMap<QString, QList<int>> streamDeckDialRotateMaps;
```

- [ ] **Step 4: Serialize/deserialize in `SettingsManager`**

In `settingsmanager.cpp`, find `save()` where it writes the root JSON object (after the MIDI bindings are written, before `QJsonDocument doc(root)` / file write). Add a helper and three writes. Put this free helper at file scope near the top of the file (after the includes):

```cpp
namespace {
// model id -> [action ids]  <->  JSON object of arrays.
QJsonObject streamDeckMapsToJson(const QMap<QString, QList<int>> &maps) {
    QJsonObject obj;
    for (auto it = maps.constBegin(); it != maps.constEnd(); ++it) {
        QJsonArray arr;
        for (int v : it.value()) arr.append(v);
        obj.insert(it.key(), arr);
    }
    return obj;
}

QMap<QString, QList<int>> streamDeckMapsFromJson(const QJsonValue &val) {
    QMap<QString, QList<int>> maps;
    if (!val.isObject()) return maps;
    const QJsonObject obj = val.toObject();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        QList<int> row;
        for (const QJsonValue &v : it.value().toArray()) row.append(v.toInt(-1));
        maps.insert(it.key(), row);
    }
    return maps;
}
} // namespace
```

In `save()`, where the root object is assembled, add:

```cpp
    root["streamDeckKeyMaps"] = streamDeckMapsToJson(settings.streamDeckKeyMaps);
    root["streamDeckDialPressMaps"] = streamDeckMapsToJson(settings.streamDeckDialPressMaps);
    root["streamDeckDialRotateMaps"] = streamDeckMapsToJson(settings.streamDeckDialRotateMaps);
```

In `load()`, where fields are read from the root object (after the MIDI maps are read), add:

```cpp
    settings.streamDeckKeyMaps = streamDeckMapsFromJson(root.value("streamDeckKeyMaps"));
    settings.streamDeckDialPressMaps = streamDeckMapsFromJson(root.value("streamDeckDialPressMaps"));
    settings.streamDeckDialRotateMaps = streamDeckMapsFromJson(root.value("streamDeckDialRotateMaps"));
```

> Note: match the local variable name `save()`/`load()` use for the root object (it may be `root`, `obj`, or `json`). Read the surrounding lines and use the existing name.

- [ ] **Step 5: Run the test, expect pass**

Run:
```
cmake --build build-test --target tst_settingsmanager -j8
ctest --test-dir build-test -R settingsmanager --output-on-failure
```
Expected: **PASS** (round trip preserves the three maps).

- [ ] **Step 6: Commit**

```bash
git add settingsmanager.h settingsmanager.cpp tests/unit/tst_settingsmanager.cpp
git commit -m "feat: persist Stream Deck per-model mapping tables in AppSettings"
```

---

### Task 2: StreamDeckMappingStore + shuttle ladder (TDD)

The cross-platform brain: three tables, move/displace `bind`, validation, `clear`, `resetToDefault`, `bindingLabel`, `loadFrom`/`writeTo` AppSettings, plus a pure shuttle-ladder helper. This is the most-tested unit.

**Files:**
- Create: `streamdeck/streamdeckmappingstore.h`
- Create: `streamdeck/streamdeckmappingstore.cpp`
- Create: `tests/unit/tst_streamdeckmappingstore.cpp`
- Modify: `tests/CMakeLists.txt` (add the store to `olr_test_core`)
- Modify: `tests/unit/CMakeLists.txt` (register the test)

- [ ] **Step 1: Write the header**

Create `streamdeck/streamdeckmappingstore.h`:

```cpp
#ifndef STREAMDECKMAPPINGSTORE_H
#define STREAMDECKMAPPINGSTORE_H

#include <QList>
#include <QMap>
#include <QString>
#include "settingsmanager.h"

// Cross-platform owner of the per-model Stream Deck mappings. No Qt signals;
// UIManager emits the QML-facing change notifications. Action ids match
// DeckAction.swift / UIManager::dispatchControlAction (see the plan header).
//
// Element types: 0 = key, 1 = dial-press, 2 = dial-turn.
class StreamDeckMappingStore {
public:
    enum ElementType { Key = 0, DialPress = 1, DialTurn = 2 };

    // Built-in fallback layout for a model+geometry. Mirrors the Swift
    // DeckAction.defaultMapping for keys; dials default to dial 0 rotate=jog,
    // press=play/pause; all other dials unbound.
    void resetToDefault(const QString &model, int keyCount, int dialCount);

    // Apply a learned binding with move/displace semantics. Returns true if
    // applied, false if the (action, element) pairing is invalid (caller keeps
    // listening). Validates: jog(8)/shuttle(10) -> DialTurn only;
    // timecode(20)/speed(21) -> Key only; everything else -> Key or DialPress.
    bool bind(const QString &model, int action, ElementType type, int index,
              int keyCount, int dialCount);

    // Remove an action from whichever control holds it (no-op if unbound).
    void clear(const QString &model, int action);

    // Ensure the model's rows exist and fit the live geometry (creates the
    // default layout if the model is new; clamps saved rows otherwise).
    // Called on connect before pushing maps to the deck.
    void clampToGeometry(const QString &model, int keyCount, int dialCount) {
        ensureModel(model, keyCount, dialCount);
    }

    // Human-readable current binding, e.g. "Key 5", "Dial 2 turn",
    // "Dial 0 press", or "Unassigned".
    QString bindingLabel(const QString &model, int action) const;

    // Accessors (also used to push to the bridge). Absent model -> empty list.
    QList<int> keyMap(const QString &model) const { return m_keyMaps.value(model); }
    QList<int> dialPressMap(const QString &model) const { return m_dialPressMaps.value(model); }
    QList<int> dialRotateMap(const QString &model) const { return m_dialRotateMaps.value(model); }
    bool hasModel(const QString &model) const { return m_keyMaps.contains(model); }

    // AppSettings <-> store. loadFrom prunes indices that exceed the live
    // geometry for the connected model (see ensureModel).
    void loadFrom(const AppSettings &settings);
    void writeTo(AppSettings &settings) const;

    // Validity helper exposed for tests and the editor.
    static bool isValidBinding(int action, ElementType type);

private:
    // Guarantee the three rows for `model` exist and are sized to the geometry,
    // creating the default layout if the model is new.
    void ensureModel(const QString &model, int keyCount, int dialCount);
    QList<int> &rowFor(ElementType type, const QString &model);

    QMap<QString, QList<int>> m_keyMaps;
    QMap<QString, QList<int>> m_dialPressMaps;
    QMap<QString, QList<int>> m_dialRotateMaps;
};

// Pure shuttle-ladder step. Snaps `currentSpeed` to the nearest rung of
// {-5,-2,-1,0,1,2,5}, moves `delta` rungs (clamped), and returns the new
// speed + whether playback should run (false only at rung 0).
struct ShuttleResult { double speed; bool playing; };
ShuttleResult shuttleLadderStep(double currentSpeed, int delta);

#endif // STREAMDECKMAPPINGSTORE_H
```

- [ ] **Step 2: Write the failing tests**

Create `tests/unit/tst_streamdeckmappingstore.cpp`:

```cpp
// Unit tests for StreamDeckMappingStore: move/displace semantics, validation,
// defaults, per-model isolation, AppSettings round trip, and the shuttle ladder.
#include <QtTest>

#include "streamdeck/streamdeckmappingstore.h"

using ET = StreamDeckMappingStore::ElementType;

class TestStreamDeckMappingStore : public QObject {
    Q_OBJECT
private slots:
    void defaultLayoutMatchesPriority();
    void bindMovesActionAndDisplacesOccupant();
    void bindToDialPressAndTurn();
    void validationRejectsBadPairings();
    void clearUnbinds();
    void perModelIsolation();
    void appSettingsRoundTrip();
    void loadPrunesOutOfRangeIndices();
    void resetRestoresDefault();
    void shuttleLadderSnapsClampsAndPauses();
};

void TestStreamDeckMappingStore::defaultLayoutMatchesPriority() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    const QList<int> keys = s.keyMap("plusXL");
    QCOMPARE(keys.size(), 36);
    // priority: record(9), play(0), live(4), capture(5), stepBack(7),
    // stepFwd(3), rew(1), fwd(2), multi(6), tc(20), speed(21), then blank.
    QCOMPARE(keys.mid(0, 11), (QList<int>{9, 0, 4, 5, 7, 3, 1, 2, 6, 20, 21}));
    QCOMPARE(keys.at(11), -1);
    // dial 0 default: rotate jog, press play; others unbound.
    QCOMPARE(s.dialRotateMap("plusXL").at(0), 8);
    QCOMPARE(s.dialPressMap("plusXL").at(0), 0);
    QCOMPARE(s.dialRotateMap("plusXL").at(1), -1);
}

void TestStreamDeckMappingStore::bindMovesActionAndDisplacesOccupant() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    // Capture(5) starts on key 3. Move it to key 20.
    QVERIFY(s.bind("plusXL", 5, ET::Key, 20, 36, 6));
    QCOMPARE(s.keyMap("plusXL").at(20), 5);
    QCOMPARE(s.keyMap("plusXL").at(3), -1);          // vacated
    // Key 0 holds record(9). Bind play(0) onto key 0 -> record displaced.
    QVERIFY(s.bind("plusXL", 0, ET::Key, 0, 36, 6));
    QCOMPARE(s.keyMap("plusXL").at(0), 0);
    QCOMPARE(s.bindingLabel("plusXL", 9), QStringLiteral("Unassigned"));  // displaced
}

void TestStreamDeckMappingStore::bindToDialPressAndTurn() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    QVERIFY(s.bind("plusXL", 5, ET::DialPress, 2, 36, 6));   // capture -> dial 2 press
    QCOMPARE(s.dialPressMap("plusXL").at(2), 5);
    QCOMPARE(s.bindingLabel("plusXL", 5), QStringLiteral("Dial 2 press"));
    QVERIFY(s.bind("plusXL", 10, ET::DialTurn, 3, 36, 6));   // shuttle -> dial 3 turn
    QCOMPARE(s.dialRotateMap("plusXL").at(3), 10);
    QCOMPARE(s.bindingLabel("plusXL", 10), QStringLiteral("Dial 3 turn"));
}

void TestStreamDeckMappingStore::validationRejectsBadPairings() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    QVERIFY(!s.bind("plusXL", 8, ET::Key, 5, 36, 6));        // jog on a key: rejected
    QVERIFY(!s.bind("plusXL", 10, ET::DialPress, 1, 36, 6)); // shuttle on press: rejected
    QVERIFY(!s.bind("plusXL", 20, ET::DialPress, 1, 36, 6)); // timecode on dial: rejected
    QVERIFY(!s.bind("plusXL", 0, ET::DialTurn, 1, 36, 6));   // play on a turn: rejected
    QVERIFY(s.bind("plusXL", 0, ET::DialPress, 1, 36, 6));   // play on press: ok
}

void TestStreamDeckMappingStore::clearUnbinds() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    s.clear("plusXL", 9);  // record was on key 0
    QCOMPARE(s.keyMap("plusXL").at(0), -1);
    QCOMPARE(s.bindingLabel("plusXL", 9), QStringLiteral("Unassigned"));
}

void TestStreamDeckMappingStore::perModelIsolation() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    s.resetToDefault("mini", 6, 0);
    s.bind("plusXL", 0, ET::Key, 30, 36, 6);
    QCOMPARE(s.keyMap("mini").size(), 6);                    // untouched
    QVERIFY(s.keyMap("mini").indexOf(0) != 30);
}

void TestStreamDeckMappingStore::appSettingsRoundTrip() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    s.bind("plusXL", 10, ET::DialTurn, 4, 36, 6);
    AppSettings settings;
    s.writeTo(settings);
    StreamDeckMappingStore s2;
    s2.loadFrom(settings);
    QCOMPARE(s2.keyMap("plusXL"), s.keyMap("plusXL"));
    QCOMPARE(s2.dialRotateMap("plusXL"), s.dialRotateMap("plusXL"));
    QCOMPARE(s2.dialPressMap("plusXL"), s.dialPressMap("plusXL"));
}

void TestStreamDeckMappingStore::loadPrunesOutOfRangeIndices() {
    AppSettings settings;
    // Saved an 8-entry key row; reconnect as a 6-key mini -> clamp to 6.
    settings.streamDeckKeyMaps.insert("mini", QList<int>{9, 0, 4, 5, 7, 3, 6, 20});
    StreamDeckMappingStore s;
    s.loadFrom(settings);                  // raw rows kept verbatim
    s.clampToGeometry("mini", 6, 0);       // simulate connect with live geometry
    QCOMPARE(s.keyMap("mini").size(), 6);
    QCOMPARE(s.keyMap("mini"), (QList<int>{9, 0, 4, 5, 7, 3}));  // tail dropped
}

void TestStreamDeckMappingStore::resetRestoresDefault() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    const QList<int> original = s.keyMap("plusXL");
    s.bind("plusXL", 0, ET::Key, 30, 36, 6);
    s.resetToDefault("plusXL", 36, 6);
    QCOMPARE(s.keyMap("plusXL"), original);
}

void TestStreamDeckMappingStore::shuttleLadderSnapsClampsAndPauses() {
    // From paused/1x, one detent up -> 2x.
    ShuttleResult r = shuttleLadderStep(1.0, +1);
    QCOMPARE(r.speed, 2.0);
    QVERIFY(r.playing);
    // From 1x, one down -> rung 0 -> paused.
    r = shuttleLadderStep(1.0, -1);
    QCOMPARE(r.speed, 0.0);
    QVERIFY(!r.playing);
    // Clamp at the top.
    r = shuttleLadderStep(5.0, +3);
    QCOMPARE(r.speed, 5.0);
    // Snap an off-ladder speed (3.0 -> nearest rung 2) then step up -> 5.
    r = shuttleLadderStep(3.0, +1);
    QCOMPARE(r.speed, 5.0);
}

QTEST_MAIN(TestStreamDeckMappingStore)
#include "tst_streamdeckmappingstore.moc"
```

- [ ] **Step 3: Register the store in the test library + the test**

In `tests/CMakeLists.txt`, add the store source to `olr_test_core` (the pure-Qt-Core lib). Change its `qt_add_library` list to include the new file:

```cmake
qt_add_library(olr_test_core STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/recordingclock.cpp"
    "${CMAKE_SOURCE_DIR}/settingsmanager.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbacktransport.cpp"
    "${CMAKE_SOURCE_DIR}/streamdeck/streamdeckmappingstore.cpp"
)
```

In `tests/unit/CMakeLists.txt`, add at the end:

```cmake
olr_add_unit_test(tst_streamdeckmappingstore olr_test_core)
```

- [ ] **Step 4: Run the test, expect failure**

Run:
```
cmake -S . -B build-test -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build-test --target tst_streamdeckmappingstore -j8
```
Expected: **link/compile failure** — `streamdeckmappingstore.cpp` not yet written.

- [ ] **Step 5: Implement the store**

Create `streamdeck/streamdeckmappingstore.cpp`:

```cpp
#include "streamdeck/streamdeckmappingstore.h"

#include <array>
#include <cmath>

namespace {

constexpr int kUnbound = -1;

// Key fill priority — mirrors DeckAction.keyPriority in DeckAction.swift.
const QList<int> kKeyPriority = {9, 0, 4, 5, 7, 3, 1, 2, 6, 20, 21};

// The shuttle speed ladder, ascending. Index of 0.0 is the pause rung.
const std::array<double, 7> kShuttleLadder = {-5.0, -2.0, -1.0, 0.0, 1.0, 2.0, 5.0};

QList<int> defaultKeyRow(int keyCount) {
    QList<int> row;
    row.reserve(keyCount);
    for (int i = 0; i < keyCount; ++i)
        row.append(i < kKeyPriority.size() ? kKeyPriority.at(i) : kUnbound);
    return row;
}

QList<int> blankRow(int n) { return QList<int>(n, kUnbound); }

// Drop the action from a row if present (move semantics helper).
void removeAction(QList<int> &row, int action) {
    for (int &v : row)
        if (v == action) v = kUnbound;
}

// Resize a saved row to the live geometry: truncate overflow, pad with -1.
QList<int> fit(const QList<int> &saved, int n) {
    QList<int> row = saved.mid(0, n);
    while (row.size() < n) row.append(kUnbound);
    return row;
}

} // namespace

bool StreamDeckMappingStore::isValidBinding(int action, ElementType type) {
    const bool rotateOnly = (action == 8 || action == 10);   // jog, shuttle
    const bool keyOnly = (action == 20 || action == 21);     // timecode, speed
    switch (type) {
    case DialTurn:  return rotateOnly;
    case Key:       return !rotateOnly;        // displays + discrete on keys
    case DialPress: return !rotateOnly && !keyOnly;
    }
    return false;
}

void StreamDeckMappingStore::resetToDefault(const QString &model, int keyCount, int dialCount) {
    m_keyMaps[model] = defaultKeyRow(keyCount);
    QList<int> rotate = blankRow(dialCount);
    QList<int> press = blankRow(dialCount);
    if (dialCount > 0) {
        rotate[0] = 8;   // jog
        press[0] = 0;    // play/pause
    }
    m_dialRotateMaps[model] = rotate;
    m_dialPressMaps[model] = press;
}

void StreamDeckMappingStore::ensureModel(const QString &model, int keyCount, int dialCount) {
    if (!m_keyMaps.contains(model)) {
        resetToDefault(model, keyCount, dialCount);
        return;
    }
    // Clamp pre-existing (possibly saved-from-other-geometry) rows to fit.
    m_keyMaps[model] = fit(m_keyMaps.value(model), keyCount);
    m_dialRotateMaps[model] = fit(m_dialRotateMaps.value(model), dialCount);
    m_dialPressMaps[model] = fit(m_dialPressMaps.value(model), dialCount);
}

QList<int> &StreamDeckMappingStore::rowFor(ElementType type, const QString &model) {
    switch (type) {
    case Key:       return m_keyMaps[model];
    case DialPress: return m_dialPressMaps[model];
    case DialTurn:  return m_dialRotateMaps[model];
    }
    return m_keyMaps[model];
}

bool StreamDeckMappingStore::bind(const QString &model, int action, ElementType type,
                                  int index, int keyCount, int dialCount) {
    if (!isValidBinding(action, type)) return false;
    ensureModel(model, keyCount, dialCount);
    QList<int> &target = rowFor(type, model);
    if (index < 0 || index >= target.size()) return false;
    // Move: remove this action from all three tables first...
    removeAction(m_keyMaps[model], action);
    removeAction(m_dialPressMaps[model], action);
    removeAction(m_dialRotateMaps[model], action);
    // ...then place it (displacing whatever occupied the target slot).
    rowFor(type, model)[index] = action;
    return true;
}

void StreamDeckMappingStore::clear(const QString &model, int action) {
    if (!m_keyMaps.contains(model)) return;
    removeAction(m_keyMaps[model], action);
    removeAction(m_dialPressMaps[model], action);
    removeAction(m_dialRotateMaps[model], action);
}

QString StreamDeckMappingStore::bindingLabel(const QString &model, int action) const {
    const int k = m_keyMaps.value(model).indexOf(action);
    if (k >= 0) return QStringLiteral("Key %1").arg(k);
    const int p = m_dialPressMaps.value(model).indexOf(action);
    if (p >= 0) return QStringLiteral("Dial %1 press").arg(p);
    const int r = m_dialRotateMaps.value(model).indexOf(action);
    if (r >= 0) return QStringLiteral("Dial %1 turn").arg(r);
    return QStringLiteral("Unassigned");
}

void StreamDeckMappingStore::loadFrom(const AppSettings &settings) {
    m_keyMaps = settings.streamDeckKeyMaps;
    m_dialPressMaps = settings.streamDeckDialPressMaps;
    m_dialRotateMaps = settings.streamDeckDialRotateMaps;
}

void StreamDeckMappingStore::writeTo(AppSettings &settings) const {
    settings.streamDeckKeyMaps = m_keyMaps;
    settings.streamDeckDialPressMaps = m_dialPressMaps;
    settings.streamDeckDialRotateMaps = m_dialRotateMaps;
}

ShuttleResult shuttleLadderStep(double currentSpeed, int delta) {
    // Snap currentSpeed to the nearest rung.
    int idx = 0;
    double best = std::abs(currentSpeed - kShuttleLadder[0]);
    for (int i = 1; i < static_cast<int>(kShuttleLadder.size()); ++i) {
        const double d = std::abs(currentSpeed - kShuttleLadder[i]);
        if (d < best) { best = d; idx = i; }
    }
    idx += delta;
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(kShuttleLadder.size()))
        idx = static_cast<int>(kShuttleLadder.size()) - 1;
    const double speed = kShuttleLadder[static_cast<size_t>(idx)];
    return ShuttleResult{speed, speed != 0.0};
}
```

> Note on `loadPrunesOutOfRangeIndices`: `loadFrom` stores the raw saved rows; `ensureModel` (called via `resetToDefault` only when the model is absent, or directly on connect through `UIManager`) clamps to the live geometry. The test calls `resetToDefault` which overwrites — adjust the test if you prefer to assert clamping via a dedicated `ensureModel`-exposing path. If clamping-on-load is what you want asserted, change the test's final two lines to load then connect through `UIManager` semantics; for this unit test, assert that `fit()` clamps by calling `bind` with the smaller geometry instead. Keep the test and impl consistent — the rule is: **rows are clamped to the connected geometry before use.**

- [ ] **Step 6: Run the tests, expect pass**

Run:
```
cmake --build build-test --target tst_streamdeckmappingstore -j8
ctest --test-dir build-test -R streamdeckmappingstore --output-on-failure
```
Expected: **PASS**, all slots green. (If `loadPrunesOutOfRangeIndices` is awkward against the final impl, make it assert `fit` behavior directly via a connect-then-check using `bind` with `keyCount=6`; keep impl unchanged.)

- [ ] **Step 7: Commit**

```bash
git add streamdeck/streamdeckmappingstore.h streamdeck/streamdeckmappingstore.cpp \
        tests/unit/tst_streamdeckmappingstore.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "feat: StreamDeckMappingStore (move/displace, validation, shuttle ladder) + tests"
```

---

### Task 3: Swift — Shuttle action + DeckState dial tables (TDD)

Add the Shuttle action and the two dial tables to the Swift side, with lookups, all change-guarded like the key table. Extend the bridge XCTests.

**Files:**
- Modify: `ios/streamdeck-bridge/Sources/DeckAction.swift`
- Modify: `ios/streamdeck-bridge/Sources/DeckState.swift`
- Modify: `ios/streamdeck-bridge/Tests/DeckStateTests.swift`

- [ ] **Step 1: Write failing tests**

In `ios/streamdeck-bridge/Tests/DeckStateTests.swift`, add these test methods to the existing `DeckStateTests` class:

```swift
    func testShuttleActionExists() {
        XCTAssertEqual(DeckAction(rawValue: 10), .shuttle)
        XCTAssertEqual(DeckAction.shuttle.label, "Shuttle")
    }

    func testDialMappingLookups() {
        let state = DeckState()
        state.setDialMapping(rotate: [8, 10, -1], press: [0, -1, 5], forModel: "plusXL")
        XCTAssertEqual(state.rotateAction(forDial: 0, model: "plusXL"), .jog)
        XCTAssertEqual(state.rotateAction(forDial: 1, model: "plusXL"), .shuttle)
        XCTAssertNil(state.rotateAction(forDial: 2, model: "plusXL"))
        XCTAssertEqual(state.pressAction(forDial: 0, model: "plusXL"), .playPause)
        XCTAssertNil(state.pressAction(forDial: 1, model: "plusXL"))
        XCTAssertEqual(state.pressAction(forDial: 2, model: "plusXL"), .capture)
        XCTAssertNil(state.rotateAction(forDial: 9, model: "plusXL"))   // out of range
    }

    func testUnchangedDialMappingDoesNotPublish() {
        let state = DeckState()
        state.setDialMapping(rotate: [8], press: [0], forModel: "plus")
        let count = changeCount(of: state) {
            state.setDialMapping(rotate: [8], press: [0], forModel: "plus")
        }
        XCTAssertEqual(count, 0)
    }
```

(`changeCount(of:during:)` already exists in the test file from the integration.)

- [ ] **Step 2: Run tests, expect failure**

Run the Swift bridge test command (see Before You Start). Expected: **compile failure** — `.shuttle`, `setDialMapping`, `rotateAction`, `pressAction` undefined.

- [ ] **Step 3: Add `.shuttle` to DeckAction**

In `ios/streamdeck-bridge/Sources/DeckAction.swift`:

In the enum, after `case record = 9`, add:
```swift
    case shuttle = 10       // dial rotation only, variable-speed
```

In `symbolName`, add a case (before the closing brace of the switch):
```swift
        case .shuttle: return "dial.high.fill"
```

In `label`, add:
```swift
        case .shuttle: return "Shuttle"
```

> `keyPriority` and `defaultMapping` are unchanged — Shuttle is opt-in, never in the default layout.

- [ ] **Step 4: Add dial tables + lookups to DeckState**

In `ios/streamdeck-bridge/Sources/DeckState.swift`:

After `@Published private(set) var keyMappings: [String: [Int]] = [:]`, add:
```swift
    /// model id -> dial-index -> rotate action id (-1 unbound).
    @Published private(set) var dialRotateMappings: [String: [Int]] = [:]
    /// model id -> dial-index -> press action id (-1 unbound).
    @Published private(set) var dialPressMappings: [String: [Int]] = [:]
```

After `setKeyMapping(_:forModel:)`, add:
```swift
    func setDialMapping(rotate: [Int], press: [Int], forModel model: String) {
        if dialRotateMappings[model] != rotate { dialRotateMappings[model] = rotate }
        if dialPressMappings[model] != press { dialPressMappings[model] = press }
    }

    func rotateAction(forDial index: Int, model: String) -> DeckAction? {
        guard let m = dialRotateMappings[model], index >= 0, index < m.count else { return nil }
        return DeckAction(rawValue: m[index])
    }

    func pressAction(forDial index: Int, model: String) -> DeckAction? {
        guard let m = dialPressMappings[model], index >= 0, index < m.count else { return nil }
        return DeckAction(rawValue: m[index])
    }
```

- [ ] **Step 5: Run tests, expect pass**

Run the Swift bridge test command. Expected: **TEST SUCCEEDED**, including the three new tests.

- [ ] **Step 6: Commit**

```bash
git add ios/streamdeck-bridge/Sources/DeckAction.swift \
        ios/streamdeck-bridge/Sources/DeckState.swift \
        ios/streamdeck-bridge/Tests/DeckStateTests.swift
git commit -m "feat: Swift Shuttle action + DeckState dial mapping tables"
```

---

### Task 4: Swift — learn mode, raw-element emit, per-dial behavior

Centralize event resolution in the bridge: views report raw elements; the bridge either dispatches the mapped action (normal) or reports the raw element (learn). Replace `onJog` with `onRotate(actionId, delta)`. Rewire the dial strip to fire per-dial bound actions and label each dial; keys gain a learn branch.

**Files:**
- Modify: `ios/streamdeck-bridge/Sources/OLRStreamDeckBridge.swift`
- Modify: `ios/streamdeck-bridge/Sources/ReplayDeckLayout.swift`
- Modify: `ios/streamdeck-bridge/Tests/DeckStateTests.swift`

- [ ] **Step 1: Bridge — learn state, raw-element emit, onRotate, setDialMapping, currentModel**

In `OLRStreamDeckBridge.swift`:

Replace the `onJog` callback declaration:
```swift
    /// Signed dial-rotation delta (positive = clockwise).
    @objc public var onJog: ((Int) -> Void)?
```
with:
```swift
    /// (rotateActionId, signed delta). Fired only outside learn mode.
    @objc public var onRotate: ((Int, Int) -> Void)?
    /// (elementType, index) during learn mode: 0 = key, 1 = dial-press, 2 = dial-turn.
    @objc public var onLearnInput: ((Int, Int) -> Void)?
```

Add stored state after `private var cancellables = Set<AnyCancellable>()`:
```swift
    private var learning = false
    private var currentModel = "unknown"
```

In `attach(_:)`, record the model right after `let model = deckModelIdentifier(for: device)`:
```swift
        currentModel = model
```

Add the learn toggle and dial-map setter near the other `@objc` setters:
```swift
    @objc public func setLearnMode(_ active: Bool) { learning = active }

    @objc public func setDialMapping(rotate: [Int], press: [Int], forModel model: String) {
        state.setDialMapping(rotate: rotate, press: press, forModel: model)
    }
```

Replace the `emitAction`/`emitJog`/`emitScrub` funnel at the bottom with raw-element entry points that resolve against the current model:
```swift
    // MARK: Raw element events from the layout. The bridge resolves the bound
    // action (normal) or reports the raw element (learn) — the single place
    // mapping is applied, so learn mode is honored everywhere.

    func emitKey(_ keyIndex: Int, pressed: Bool) {
        if learning {
            if pressed { onLearnInput?(0, keyIndex) }
            return
        }
        guard let action = state.action(forKey: keyIndex, model: currentModel),
              action != .timecodeDisplay, action != .speedDisplay else { return }
        onAction?(action.rawValue, pressed)
    }

    func emitDialPress(_ dial: Int, pressed: Bool) {
        if learning {
            if pressed { onLearnInput?(1, dial) }
            return
        }
        guard let action = state.pressAction(forDial: dial, model: currentModel) else { return }
        onAction?(action.rawValue, pressed)
    }

    func emitDialRotate(_ dial: Int, delta: Int) {
        if learning {
            onLearnInput?(2, dial)
            return
        }
        guard let action = state.rotateAction(forDial: dial, model: currentModel) else { return }
        onRotate?(action.rawValue, delta)
    }

    /// Touch-strip seek — never part of learn, never mapped.
    func emitScrub(_ fraction: Double) { onScrub?(fraction) }
```

Delete the old `emitAction`/`emitJog` methods (their callers are rewired in Step 2). Also remove `bridge.onJog = nil` is handled in the .mm (Task 5); leave the Swift `onJog` removed.

- [ ] **Step 2: Rewire the layout views to raw-element emit + per-dial behavior**

In `ReplayDeckLayout.swift`:

**ReplayKeyView** — change the press handler to report the raw key index (the bridge resolves/guards):
```swift
        StreamDeckKeyView { pressed in
            isPressed = pressed
            OLRStreamDeckBridge.shared.emitKey(keyIndex, pressed: pressed)
        } content: {
            keyBody
        }
```
(Remove the `guard let action = content.action …` line — guarding now lives in `emitKey`. `isPressed` may flip for display/blank keys but they render no pressed state difference, which is fine; if you prefer, keep `isPressed` updates gated on `content.action != nil`.)

**ReplayDialStrip** — replace the whole `StreamDeckDialAreaLayout(...)` call so every dial fires its bound press/rotate and shows its label. Replace the `var body` with:
```swift
    var body: some View {
        StreamDeckDialAreaLayout(
            rotate: { dialIndex, rotation in
                OLRStreamDeckBridge.shared.emitDialRotate(dialIndex, delta: rotation)
            },
            press: { dialIndex, pressed in
                OLRStreamDeckBridge.shared.emitDialPress(dialIndex, pressed: pressed)
            },
            touch: { location in
                guard stripSize.width > 0 else { return }
                let f = min(max(location.x / stripSize.width, 0), 1)
                OLRStreamDeckBridge.shared.emitScrub(Double(f))
            }
        ) { dialIndex in
            DialSegment(
                segmentIndex: dialIndex,
                segmentCount: dialCount,
                fraction: fraction,
                timecodeText: dialIndex == 0 ? timecodeText : "",
                pressLabel: pressLabels.indices.contains(dialIndex) ? pressLabels[dialIndex] : "",
                rotateLabel: rotateLabels.indices.contains(dialIndex) ? rotateLabels[dialIndex] : "",
                speedText: speedText)
        }
        .onReceive(state.objectWillChange.receive(on: RunLoop.main)) { _ in
            if state.positionFraction != fraction { fraction = state.positionFraction }
            if state.timecodeText != timecodeText { timecodeText = state.timecodeText }
            if state.speedText != speedText { speedText = state.speedText }
            refreshLabels()
        }
    }
```

Add the label state + helper to `ReplayDialStrip`. Add the stored model and label arrays:
```swift
    @State private var speedText: String
    @State private var pressLabels: [String]
    @State private var rotateLabels: [String]
    let model: String
```
Update its `init` to seed them:
```swift
    init(state: DeckState, dialCount: Int, model: String) {
        self.state = state
        self.dialCount = dialCount
        self.model = model
        _fraction = State(initialValue: state.positionFraction)
        _timecodeText = State(initialValue: state.timecodeText)
        _speedText = State(initialValue: state.speedText)
        _pressLabels = State(initialValue: Self.labels(state.dialPressMappings[model], count: dialCount))
        _rotateLabels = State(initialValue: Self.labels(state.dialRotateMappings[model], count: dialCount))
    }

    private func refreshLabels() {
        let p = Self.labels(state.dialPressMappings[model], count: dialCount)
        let r = Self.labels(state.dialRotateMappings[model], count: dialCount)
        if p != pressLabels { pressLabels = p }
        if r != rotateLabels { rotateLabels = r }
    }

    private static func labels(_ row: [Int]?, count: Int) -> [String] {
        (0..<count).map { i in
            guard let row, i < row.count, let a = DeckAction(rawValue: row[i]) else { return "" }
            return a.label
        }
    }
```

Update the caller in `ReplayDeckLayout.body` to pass the model:
```swift
            case .plus, .plusXL:
                ReplayDialStrip(
                    state: state,
                    dialCount: max(streamDeck.capabilities.dialCount, 1),
                    model: model)
```

Rename `ScrubBarSegment` to `DialSegment` and extend it to overlay the bound labels and (for a shuttle-bound dial) the live speed. Replace the struct with:
```swift
/// One dial section: scrub-bar background (shared across the strip) + this
/// dial's bound action labels. Segment 0 also shows the timecode. A dial whose
/// rotate label is "Shuttle" shows the live speed instead of the timecode slot.
struct DialSegment: View {

    let segmentIndex: Int
    let segmentCount: Int
    let fraction: Double
    let timecodeText: String
    let pressLabel: String
    let rotateLabel: String
    let speedText: String

    var body: some View {
        GeometryReader { geo in
            let globalProgress = fraction * Double(segmentCount)
            let localFill = min(max(globalProgress - Double(segmentIndex), 0), 1)
            ZStack(alignment: .topLeading) {
                Rectangle().fill(Color(white: 0.15))
                Rectangle().fill(Color.blue).frame(width: geo.size.width * localFill)
                VStack(alignment: .leading, spacing: 0) {
                    if !rotateLabel.isEmpty {
                        Text(rotateLabel == "Shuttle" ? "Shuttle \(speedText)" : rotateLabel)
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(Color(white: 0.85))
                    }
                    if !pressLabel.isEmpty {
                        Text("• \(pressLabel)")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundColor(Color(white: 0.7))
                    }
                    if segmentIndex == 0 {
                        Text(timecodeText)
                            .font(.system(size: 13, weight: .bold).monospacedDigit())
                            .foregroundColor(.white)
                    }
                }
                .padding(.leading, 6)
                .padding(.top, 4)
            }
        }
    }
}
```

Update the two `#Preview` blocks: they call `ReplayDialStrip` indirectly via `ReplayDeckLayout`, so no change is needed there, but add a dial mapping to the "+" preview so the labels render — after `state.setKeyMapping(...)` in the `.plus` preview add:
```swift
            state.setDialMapping(rotate: [8, 10, -1, -1], press: [0, 5, -1, -1], forModel: "plus")
```

- [ ] **Step 3: Add a learn-routing test**

In `DeckStateTests.swift`, add:
```swift
    @MainActor
    func testLearnModeReportsRawElementsAndSuppressesDispatch() {
        let bridge = OLRStreamDeckBridge.shared
        var learned: [(Int, Int)] = []
        var dispatched: [Int] = []
        bridge.onLearnInput = { learned.append(($0, $1)) }
        bridge.onAction = { id, _ in dispatched.append(id) }
        bridge.state.setKeyMapping([9, 0, 4], forModel: "plusXL")
        // Force the resolution model (normally set in attach()).
        bridge.setDialMapping(rotate: [8], press: [0], forModel: "plusXL")

        bridge.setLearnMode(true)
        bridge.emitKey(1, pressed: true)        // would be play(0) normally
        bridge.emitDialRotate(0, delta: 1)      // would be jog normally
        XCTAssertEqual(dispatched, [])          // suppressed
        XCTAssertEqual(learned.count, 2)
        XCTAssertEqual(learned[0].0, 0)         // key element
        XCTAssertEqual(learned[1].0, 2)         // dial-turn element

        bridge.setLearnMode(false)
        bridge.onLearnInput = nil
        bridge.onAction = nil
    }
```
> This test exercises the bridge resolution against `currentModel`. Because `currentModel` is private and set in `attach()`, add a test-only seam: a `@objc func _setCurrentModelForTesting(_ model: String)` under `#if DEBUG` in the bridge that assigns `currentModel`, and call `bridge._setCurrentModelForTesting("plusXL")` before the asserts. Keep it `#if DEBUG`.

Add to the bridge (under `#if DEBUG`):
```swift
#if DEBUG
extension OLRStreamDeckBridge {
    @objc func _setCurrentModelForTesting(_ model: String) { currentModel = model }
}
#endif
```
(`currentModel` must be accessible to the extension — it is, same file/module.)

- [ ] **Step 4: Build + test**

```
cd ios/streamdeck-bridge && xcodegen generate
xcodebuild test -project StreamDeckBridge.xcodeproj -scheme StreamDeckBridge \
  -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' | tail -10
```
Expected: **TEST SUCCEEDED**. Also visually confirm via the `#Preview`s that the "+" dial sections show "Jog"/"Shuttle"/"• Play"/"• Capture".

- [ ] **Step 5: Commit**

```bash
cd /Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/streamdeck-integration
git add ios/streamdeck-bridge/Sources/OLRStreamDeckBridge.swift \
        ios/streamdeck-bridge/Sources/ReplayDeckLayout.swift \
        ios/streamdeck-bridge/Tests/DeckStateTests.swift
git commit -m "feat: Swift learn mode, raw-element emit, per-dial bound actions + labels"
```

---

### Task 5: StreamDeckManager — learn mode, geometry, rotate generalization

Surface deck geometry to QML, generalize the jog path to `rotateTriggered(actionId, delta)`, add `learnInput`, learn-mode toggle, and map-push methods. Update the `.mm` to consume the new bridge API; the stub no-ops.

**Files:**
- Modify: `streamdeck/streamdeckmanager.h`
- Modify: `streamdeck/streamdeckmanager.mm`
- Modify: `streamdeck/streamdeckmanager_stub.cpp`

- [ ] **Step 1: Header — properties, signals, methods**

In `streamdeck/streamdeckmanager.h`:

Add Q_PROPERTYs after the `deviceModel` one:
```cpp
    Q_PROPERTY(int keyCount READ keyCount NOTIFY connectedChanged)
    Q_PROPERTY(int dialCount READ dialCount NOTIFY connectedChanged)
```
Add getters near `deviceModel()`:
```cpp
    int keyCount() const { return m_keyCount; }
    int dialCount() const { return m_dialCount; }
```
Add invokables near `showSimulator()`:
```cpp
    Q_INVOKABLE void setLearnMode(bool active);
    // Push the active model's maps to the deck (called by UIManager after edits
    // and on connect). Lists are index -> action id (-1 unbound).
    void pushKeyMap(const QString &model, const QList<int> &keyMap);
    void pushDialMaps(const QString &model,
                      const QList<int> &rotateMap, const QList<int> &pressMap);
```
Replace the `jogTriggered` signal:
```cpp
    void jogTriggered(int delta);
```
with:
```cpp
    void rotateTriggered(int actionId, int delta);
    void learnInput(int elementType, int index);
```
Update `handleDeviceConnected` signature (private) to carry geometry:
```cpp
    void handleDeviceConnected(const QString &name, const QString &model,
                               int keyCount, int dialCount);
```
Add members near `m_deviceModel`:
```cpp
    int m_keyCount = 0;
    int m_dialCount = 0;
```
Update the doc comment's action list to include `10 shuttle (dial-turn)`.

- [ ] **Step 2: `.mm` — consume new bridge API**

In `streamdeck/streamdeckmanager.mm`:

In `~StreamDeckManager`, replace `bridge.onJog = nil;` with:
```cpp
    bridge.onRotate = nil;
    bridge.onLearnInput = nil;
```

Replace the `bridge.onJog = ^(NSInteger delta) { … };` block with:
```cpp
    bridge.onRotate = ^(NSInteger actionId, NSInteger delta) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, actionId, delta]() {
            emit s->rotateTriggered(int(actionId), int(delta));
        }, Qt::QueuedConnection);
    };

    bridge.onLearnInput = ^(NSInteger elementType, NSInteger index) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, elementType, index]() {
            emit s->learnInput(int(elementType), int(index));
        }, Qt::QueuedConnection);
    };
```

Update the `onDeviceConnected` block to pass geometry through:
```cpp
    bridge.onDeviceConnected = ^(NSString *name, NSString *model,
                                 NSInteger keyCount, NSInteger dialCount) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        const QString qname = QString::fromNSString(name);
        const QString qmodel = QString::fromNSString(model);
        const int kc = int(keyCount);
        const int dc = int(dialCount);
        QMetaObject::invokeMethod(s, [s, qname, qmodel, kc, dc]() {
            s->handleDeviceConnected(qname, qmodel, kc, dc);
        }, Qt::QueuedConnection);
    };
```

Update `handleDeviceConnected` definition:
```cpp
void StreamDeckManager::handleDeviceConnected(const QString &name, const QString &model,
                                              int keyCount, int dialCount)
{
    m_deviceName = name;
    m_deviceModel = model;
    m_keyCount = keyCount;
    m_dialCount = dialCount;
    m_connected = true;
    emit connectedChanged();
}
```

In `handleDeviceDisconnected`, reset geometry (after clearing name/model):
```cpp
    m_keyCount = 0;
    m_dialCount = 0;
```

Add the new methods at the end of the file:
```cpp
void StreamDeckManager::setLearnMode(bool active)
{
    [[OLRStreamDeckBridge shared] setLearnMode:active];
}

static NSArray<NSNumber *> *toNSNumbers(const QList<int> &list)
{
    NSMutableArray<NSNumber *> *arr = [NSMutableArray arrayWithCapacity:list.size()];
    for (int v : list) [arr addObject:@(v)];
    return arr;
}

void StreamDeckManager::pushKeyMap(const QString &model, const QList<int> &keyMap)
{
    [[OLRStreamDeckBridge shared] setKeyMapping:toNSNumbers(keyMap)
                                       forModel:model.toNSString()];
}

void StreamDeckManager::pushDialMaps(const QString &model,
                                     const QList<int> &rotateMap, const QList<int> &pressMap)
{
    [[OLRStreamDeckBridge shared] setDialMappingWithRotate:toNSNumbers(rotateMap)
                                                     press:toNSNumbers(pressMap)
                                                  forModel:model.toNSString()];
}
```
> Verify the generated ObjC selector for `setDialMapping(rotate:press:forModel:)` — it is `setDialMappingWithRotate:press:forModel:`. If the compiler disagrees, grep the generated header: `grep "setDialMapping\|setKeyMapping" ios_build/xcframeworks/StreamDeckBridge.xcframework/ios-arm64/StreamDeckBridge.framework/Headers/StreamDeckBridge-Swift.h` (after Task 4's framework rebuild).

- [ ] **Step 3: Stub — no-op the new surface**

In `streamdeck/streamdeckmanager_stub.cpp`, add:
```cpp
int StreamDeckManager::keyCount() const { return 0; }
int StreamDeckManager::dialCount() const { return 0; }
void StreamDeckManager::setLearnMode(bool) {}
void StreamDeckManager::pushKeyMap(const QString &, const QList<int> &) {}
void StreamDeckManager::pushDialMaps(const QString &, const QList<int> &, const QList<int> &) {}
```
> `keyCount()/dialCount()` are inline in the header (`{ return m_keyCount; }`) — if so, do NOT redefine them in the stub; only stub the non-inline methods. Match the header: inline getters need no stub entry. Define only `setLearnMode`, `pushKeyMap`, `pushDialMaps`.

- [ ] **Step 4: Build macOS (stub path)**

Run: `cmake --build build -j8`
Expected: builds clean (stub compiles with the new methods). This proves the header + stub agree. The `.mm` is validated in Task 8's iOS build.

- [ ] **Step 5: Commit**

```bash
git add streamdeck/streamdeckmanager.h streamdeck/streamdeckmanager.mm streamdeck/streamdeckmanager_stub.cpp
git commit -m "feat: StreamDeckManager learn mode, geometry props, rotate generalization, map push"
```

---

### Task 6: UIManager — store ownership, learn wiring, shuttle, QML API

Wire it all together: own a `StreamDeckMappingStore`, expose the editor API to QML, route learn input through the store with persistence + live push, generalize the rotate connection, and add `shuttleStep`.

**Files:**
- Modify: `uimanager.h`
- Modify: `uimanager.cpp`

- [ ] **Step 1: Header — store, API, members**

In `uimanager.h`:

Add include near `#include "streamdeck/streamdeckmanager.h"`:
```cpp
#include "streamdeck/streamdeckmappingstore.h"
```
Add Q_PROPERTYs after the `streamDeck` one:
```cpp
    Q_PROPERTY(int streamDeckLearnAction READ streamDeckLearnAction NOTIFY streamDeckLearnActionChanged)
    Q_PROPERTY(int streamDeckBindingsVersion READ streamDeckBindingsVersion NOTIFY streamDeckBindingsChanged)
```
Add getters near `streamDeck()`:
```cpp
    int streamDeckLearnAction() const { return m_streamDeckLearnAction; }
    int streamDeckBindingsVersion() const { return m_streamDeckBindingsVersion; }
```
Add invokables near the MIDI learn invokables:
```cpp
    Q_INVOKABLE void beginStreamDeckLearn(int action);
    Q_INVOKABLE void clearStreamDeckBinding(int action);
    Q_INVOKABLE void resetStreamDeckDefaults();
    Q_INVOKABLE QString streamDeckBindingLabel(int action) const;
```
Add signals:
```cpp
    void streamDeckLearnActionChanged();
    void streamDeckBindingsChanged();
```
Add private members near `m_streamDeckManager`:
```cpp
    StreamDeckMappingStore m_streamDeckStore;
    int m_streamDeckLearnAction = -1;
    int m_streamDeckBindingsVersion = 0;
    void pushStreamDeckMaps();
    void shuttleStep(int delta);
```

- [ ] **Step 2: cpp — load store, wire learn/rotate/connect, define API**

In `uimanager.cpp`, in the constructor's Stream Deck section (where `m_streamDeckManager` is created and connected), make these changes:

Replace the jog connection:
```cpp
    connect(m_streamDeckManager, &StreamDeckManager::jogTriggered, this,
            [this](int delta) {
        jogStep(delta);
    });
```
with:
```cpp
    connect(m_streamDeckManager, &StreamDeckManager::rotateTriggered, this,
            [this](int actionId, int delta) {
        if (actionId == 10) shuttleStep(delta);
        else jogStep(delta);
    });

    connect(m_streamDeckManager, &StreamDeckManager::learnInput, this,
            [this](int elementType, int index) {
        if (m_streamDeckLearnAction < 0) return;
        const QString model = m_streamDeckManager->deviceModel();
        const bool ok = m_streamDeckStore.bind(
            model, m_streamDeckLearnAction,
            static_cast<StreamDeckMappingStore::ElementType>(elementType), index,
            m_streamDeckManager->keyCount(), m_streamDeckManager->dialCount());
        if (!ok) return;  // invalid pairing — keep listening
        m_streamDeckStore.writeTo(m_currentSettings);
        m_settingsManager->save(m_configPath, m_currentSettings);
        pushStreamDeckMaps();
        m_streamDeckManager->setLearnMode(false);
        m_streamDeckLearnAction = -1;
        emit streamDeckLearnActionChanged();
        m_streamDeckBindingsVersion++;
        emit streamDeckBindingsChanged();
    });
```

Add a connect so a fresh device gets the persisted/default maps and the editor refreshes — add inside the existing `connect(m_streamDeckManager, &StreamDeckManager::connectedChanged, …)` lambda (the one that pushes a state snapshot), after the snapshot pushes:
```cpp
        if (m_streamDeckManager->connected()) {
            const QString model = m_streamDeckManager->deviceModel();
            // Creates the default layout for a new model, or clamps saved rows
            // to the live geometry for a known one (see clampToGeometry).
            m_streamDeckStore.clampToGeometry(model,
                m_streamDeckManager->keyCount(), m_streamDeckManager->dialCount());
            pushStreamDeckMaps();
            m_streamDeckBindingsVersion++;
            emit streamDeckBindingsChanged();
        }
```
(`clampToGeometry` was defined on the store in Task 2.)

Load the store from settings in `loadSettings()` (find where `m_currentSettings` is populated from `SettingsManager::load`), add after the load:
```cpp
    m_streamDeckStore.loadFrom(m_currentSettings);
```

Add the method definitions (anywhere sensible, e.g. after the MIDI learn methods):
```cpp
void UIManager::pushStreamDeckMaps()
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    const QString model = m_streamDeckManager->deviceModel();
    m_streamDeckManager->pushKeyMap(model, m_streamDeckStore.keyMap(model));
    m_streamDeckManager->pushDialMaps(model,
        m_streamDeckStore.dialRotateMap(model),
        m_streamDeckStore.dialPressMap(model));
}

void UIManager::beginStreamDeckLearn(int action)
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    if (m_streamDeckLearnAction == action) {       // toggle off
        m_streamDeckLearnAction = -1;
        m_streamDeckManager->setLearnMode(false);
        emit streamDeckLearnActionChanged();
        return;
    }
    m_streamDeckLearnAction = action;
    m_streamDeckManager->setLearnMode(true);
    emit streamDeckLearnActionChanged();
}

void UIManager::clearStreamDeckBinding(int action)
{
    if (!m_streamDeckManager) return;
    const QString model = m_streamDeckManager->deviceModel();
    m_streamDeckStore.clear(model, action);
    m_streamDeckStore.writeTo(m_currentSettings);
    m_settingsManager->save(m_configPath, m_currentSettings);
    pushStreamDeckMaps();
    m_streamDeckBindingsVersion++;
    emit streamDeckBindingsChanged();
}

void UIManager::resetStreamDeckDefaults()
{
    if (!m_streamDeckManager || !m_streamDeckManager->connected()) return;
    const QString model = m_streamDeckManager->deviceModel();
    m_streamDeckStore.resetToDefault(model,
        m_streamDeckManager->keyCount(), m_streamDeckManager->dialCount());
    m_streamDeckStore.writeTo(m_currentSettings);
    m_settingsManager->save(m_configPath, m_currentSettings);
    pushStreamDeckMaps();
    m_streamDeckBindingsVersion++;
    emit streamDeckBindingsChanged();
}

QString UIManager::streamDeckBindingLabel(int action) const
{
    if (!m_streamDeckManager) return QStringLiteral("Unassigned");
    return m_streamDeckStore.bindingLabel(m_streamDeckManager->deviceModel(), action);
}

void UIManager::shuttleStep(int delta)
{
    if (!m_transport) return;
    cancelFollowLive();
    const ShuttleResult r = shuttleLadderStep(m_transport->speed(), delta);
    m_transport->setSpeed(r.speed);
    m_transport->setPlaying(r.playing);
    if (m_playbackWorker && !r.playing) {
        m_playbackWorker->seekTo(m_transport->currentPos());
    }
}
```

Cancel learn on disconnect — in the `connectedChanged` lambda, when `!connected()`:
```cpp
        if (!m_streamDeckManager->connected() && m_streamDeckLearnAction >= 0) {
            m_streamDeckLearnAction = -1;
            emit streamDeckLearnActionChanged();
        }
```

- [ ] **Step 3: Build macOS (stub path)**

Run: `cmake --build build -j8`
Expected: clean build. (Stub returns `keyCount()/dialCount() == 0`, `connected() == false`, so all editor calls are inert on macOS — the code still compiles and links.)

- [ ] **Step 4: Commit**

```bash
git add uimanager.h uimanager.cpp streamdeck/streamdeckmappingstore.h
git commit -m "feat: UIManager Stream Deck learn wiring, shuttle, mapping API"
```

---

### Task 7: QML — Button Mapping editor

Add the learn-style editor to the Stream Deck card.

**Files:**
- Modify: `Main.qml`

- [ ] **Step 1: Add the editor group**

In `Main.qml`, inside the Stream Deck card's expanded `ColumnLayout` (the one holding the connection `GroupBox`, added in the integration), after that `GroupBox`, add a new `GroupBox`:

```qml
                                GroupBox {
                                    title: "Button Mapping"
                                    Layout.fillWidth: true
                                    visible: appWindow.uiManagerRef.streamDeck.connected

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 8

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: "Click Learn, then press a key or turn/press a dial."
                                                color: "#aaa"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
                                            }
                                            Button {
                                                text: "Reset to default"
                                                onClicked: appWindow.uiManagerRef.resetStreamDeckDefaults()
                                            }
                                        }

                                        Repeater {
                                            model: [
                                                { name: "Record",      action: 9,  gesture: "key or dial" },
                                                { name: "Play/Pause",  action: 0,  gesture: "key or dial" },
                                                { name: "Go Live",     action: 4,  gesture: "key or dial" },
                                                { name: "Capture",     action: 5,  gesture: "key or dial" },
                                                { name: "Prev Frame",  action: 7,  gesture: "key or dial" },
                                                { name: "Next Frame",  action: 3,  gesture: "key or dial" },
                                                { name: "Rewind 5×",   action: 1,  gesture: "key or dial" },
                                                { name: "Forward 5×",  action: 2,  gesture: "key or dial" },
                                                { name: "Multiview",   action: 6,  gesture: "key or dial" },
                                                { name: "Jog",         action: 8,  gesture: "turn a dial" },
                                                { name: "Shuttle",     action: 10, gesture: "turn a dial" },
                                                { name: "Timecode",    action: 20, gesture: "press a key" },
                                                { name: "Speed",       action: 21, gesture: "press a key" }
                                            ]

                                            delegate: RowLayout {
                                                id: sdRow
                                                required property var modelData
                                                Layout.fillWidth: true
                                                spacing: 8

                                                Text {
                                                    text: sdRow.modelData.name
                                                    color: "#eeeeee"
                                                    Layout.preferredWidth: 110
                                                }
                                                Text {
                                                    text: (appWindow.uiManagerRef.streamDeckBindingsVersion >= 0
                                                           ? appWindow.uiManagerRef.streamDeckBindingLabel(sdRow.modelData.action)
                                                           : "")
                                                    color: appWindow.uiManagerRef.streamDeckLearnAction === sdRow.modelData.action
                                                           ? "#ff9800" : "#aaa"
                                                    Layout.fillWidth: true
                                                }
                                                Button {
                                                    text: appWindow.uiManagerRef.streamDeckLearnAction === sdRow.modelData.action
                                                          ? "Listening… (" + sdRow.modelData.gesture + ")"
                                                          : "Learn"
                                                    onClicked: appWindow.uiManagerRef.beginStreamDeckLearn(sdRow.modelData.action)
                                                }
                                                Button {
                                                    text: "Clear"
                                                    onClicked: appWindow.uiManagerRef.clearStreamDeckBinding(sdRow.modelData.action)
                                                }
                                            }
                                        }
                                    }
                                }
```
> Match the surrounding indentation and the `appWindow.uiManagerRef` accessor exactly as the connection `GroupBox` uses them.

- [ ] **Step 2: qmllint**

Run:
```
$HOME/Qt/6.10.1/macos/bin/qmllint -I $HOME/Qt/6.10.1/macos/qml Main.qml MultiviewWindow.qml; echo "exit=$?"
```
Expected: `exit=0` (warnings only; no new warnings inside the new group — verify none reference the Button Mapping line range).

- [ ] **Step 3: Build + launch macOS (card hidden via stub)**

Run: `cmake --build build -j8` then launch the app briefly; the Button Mapping group is hidden on macOS (`connected == false`). Confirms the QML loads.

- [ ] **Step 4: Commit**

```bash
git add Main.qml
git commit -m "feat: Stream Deck Button Mapping learn-style editor (QML)"
```

---

### Task 8: Verification (build, bridge tests, device, simulator)

No new code — the acceptance pass.

- [ ] **Step 1: C++ unit suite**

```
cmake -S . -B build-test -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build-test -j8
ctest --test-dir build-test --output-on-failure
```
Expected: all tests pass, including `tst_streamdeckmappingstore` and `tst_settingsmanager`.

- [ ] **Step 2: Swift bridge tests**

```
cd ios/streamdeck-bridge && xcodegen generate
xcodebuild test -project StreamDeckBridge.xcodeproj -scheme StreamDeckBridge \
  -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' | tail -8
```
Expected: **TEST SUCCEEDED**.

- [ ] **Step 3: iOS device build (StreamDeck ON)**

```
cd /Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/streamdeck-integration
env -u GIT_CONFIG_COUNT -u GIT_CONFIG_KEY_0 -u GIT_CONFIG_VALUE_0 \
  $HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B build-ios -G Xcode \
    -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DOLR_ENABLE_STREAMDECK=ON \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YEM49PC4TS -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic
env -u GIT_CONFIG_COUNT -u GIT_CONFIG_KEY_0 -u GIT_CONFIG_VALUE_0 \
  cmake --build build-ios --config Debug -- -allowProvisioningUpdates \
    -destination 'platform=iOS,id=00008132-000245C01E45001C' \
    DEVELOPMENT_TEAM=YEM49PC4TS CODE_SIGN_STYLE=Automatic
```
Expected: `** BUILD SUCCEEDED **` (validates the `.mm` selectors and the whole iOS link). If the bridge xcframework needs rebuilding, the custom command runs it; FFmpeg is cached. Verify the `setDialMapping`/`setKeyMapping` selectors compiled — if not, fix per Task 5 Step 2's grep note and rebuild.

- [ ] **Step 4: Install + simulator/hardware pass**

Install (`xcrun devicectl device install app --device 263A7B78-7154-55FE-8CD6-786962AEC3DB build-ios/Debug-iphoneos/OpenLiveReplay.app`) and on the iPad with the + XL connected:

| Check | Expected |
|---|---|
| Editor visible | Button Mapping group appears when the deck is connected; hidden when not |
| Learn a key | Click Learn on Capture → "Listening…" → press a deck key → that key shows Capture, label updates to "Key N", deck re-renders live |
| Displace | Learn Play onto a key already holding Record → Record becomes Unassigned |
| Dial press | Learn Multiview, press a dial → "Dial N press"; pressing that dial opens multiview |
| Jog dial | Learn Jog, turn a dial → that dial scrubs frame-by-frame |
| Shuttle dial | Learn Shuttle, turn another dial → playback runs the speed ladder (1×→2×→5×, 0 pauses); the dial section shows "Shuttle 2×" |
| Clear | Clear a row → control blanks on the deck |
| Reset | Reset to default → built-in layout returns |
| Persist | Relaunch the app → custom mapping is restored |
| Invalid gesture | While learning Jog, press a key → ignored, still listening |

- [ ] **Step 5: Update spec status + final commit**

In `docs/superpowers/specs/2026-06-14-streamdeck-key-customization-design.md`, change `**Status:** Approved design, pending implementation plan` → `**Status:** Implemented`.

```bash
git add docs/superpowers/specs/2026-06-14-streamdeck-key-customization-design.md
git commit -m "docs: mark Stream Deck key-customization spec implemented"
```

Then push and let CI run (skip the iOS pre-push hook, which fails in this sandbox):
```bash
SKIP_IOS_BUILD=1 git push origin worktree-streamdeck-integration:streamdeck-integration
```

---

## Self-Review notes

- **Spec coverage:** data model (T1–T2), validation + move/displace (T2), shuttle ladder (T2/T6), Swift dial tables + shuttle action (T3), learn mode + raw emit + per-dial render (T4), geometry/rotate/learn bridge (T5), UIManager wiring + persistence + push-on-connect (T6), QML editor (T7), all tests (T2/T3/T4/T8). Touch-strip-stays-scrub: preserved in T4 (`emitScrub` unchanged, scrub bar still the dial background).
- **Type consistency:** `ElementType {Key=0,DialPress=1,DialTurn=2}` matches the Swift `onLearnInput` element codes and `learnInput` signal. `rotateTriggered(actionId, delta)` ↔ Swift `onRotate(Int,Int)`. `pushKeyMap`/`pushDialMaps` ↔ `setKeyMapping`/`setDialMapping` selectors. Action ids identical across `DeckAction.swift`, store, `UIManager`.
- **Known seams to confirm during execution:** (a) the generated ObjC selector names for the Swift setters (grep the header); (b) `clampToGeometry` helper added to the store for the connect path; (c) the `loadPrunesOutOfRangeIndices` test asserts the clamp rule the impl actually uses — keep them in sync.
