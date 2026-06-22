# UI v2 Phase 3 — Playlist model + Rundown rail Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax. This phase has **C++ engine work** — TDD the pure logic; do not skip the failing-test-first steps.

**Goal:** Give the operator a **Rundown rail** that surfaces the auto-playout cue list (PR #112): a live list of entries with current/next highlight, per-entry speed chips, and recall/play/stop/remove/clear. This requires first exposing the playlist (today opaque to QML) as a model + state on UIManager.

**Architecture:** Add a `PlaylistEntriesModel : QAbstractListModel` wrapping `ReplayPlaylist::entries()`, plus UIManager properties/methods for current/next index and entry edits. Then build `ui/components/RundownRail.qml` and dock it beside PgmStage. Builds on Phase 1 (#145).

**Tech Stack:** Qt 6.10.1, C++ (UIManager, ReplayPlaylist, new model), QML, CMake/CTest. See roadmap (Phase 3) for the full engine surface + the MVP-vs-deferred split.

## Global Constraints

- **MVP scope:** list + current/next highlight + speed chips + recall/play/stop + remove + clear + edit-speed. **Deferred** (do NOT build here): drag-reorder, mid-list insert, in/out re-edit, disk save/load. Note deferrals in the rail (e.g. no reorder handles).
- **No behaviour change to playout timing.** The model is a read/edit view over the existing `ReplayPlaylist`; the `PlaylistPlayout` state machine and `armNextCut` timing are untouched.
- **Concurrency:** `onPlayoutTick()` runs on the UIManager thread (the playout monitor `QTimer`). The new `playlistEntryChanged` emits and model updates happen there — keep them on that thread; the model is owned by UIManager. No cross-thread mutation.
- Components use injected `property var ui`; `ui.*` only. Verify each task: build → unit suite (engine) / `qml_smoke` + render + app-load (QML). Outputs under `build/`. Never push `--no-verify`.
- First verify a baseline: `markIn()`/`markOut()`/`recallEntry(int)`/`playPlaylist(int)`/`stopPlaylistPlayout()`/`playlistCount()`/`playlistPlayoutActive()` are already `Q_INVOKABLE` on UIManager and called from TransportDock — confirm by reading `uimanager.h` before adding new API (don't duplicate).

---

# Engine (C++) — TDD

### Task 1: `ReplayPlaylist` — `removeEntry` / `clear` (pure logic, TDD)

**Files:**
- Modify: `playback/replayplaylist.h`, `playback/replayplaylist.cpp`
- Test: `tests/unit/tst_replayplaylist.cpp` (create if absent; else extend) + register in `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `void ReplayPlaylist::removeEntry(int index)` (no-op if out of range), `void ReplayPlaylist::clear()`. (`setSpeed(int,double)`, `count()`, `entries()`, `recall(int)`, `markIn`, `markOut` already exist.)

- [ ] **Step 1: Write failing tests.**
```cpp
TEST(ReplayPlaylist, RemoveEntryDropsByIndex) {
    ReplayPlaylist p;
    p.markIn("a.mkv", 0); p.markOut(100);
    p.markIn("b.mkv", 200); p.markOut(300);
    ASSERT_EQ(p.count(), 2);
    p.removeEntry(0);
    EXPECT_EQ(p.count(), 1);
    EXPECT_EQ(p.entries()[0].clipPath, QStringLiteral("b.mkv"));
}
TEST(ReplayPlaylist, RemoveOutOfRangeIsNoOp) {
    ReplayPlaylist p; p.markIn("a.mkv", 0);
    p.removeEntry(5); p.removeEntry(-1);
    EXPECT_EQ(p.count(), 1);
}
TEST(ReplayPlaylist, ClearEmpties) {
    ReplayPlaylist p; p.markIn("a.mkv", 0); p.markIn("b.mkv", 1);
    p.clear();
    EXPECT_EQ(p.count(), 0);
}
```
(Match the repo's test framework — check a sibling `tests/unit/tst_*.cpp` for QTest vs gtest; mirror it.)

- [ ] **Step 2: Run, verify fail.** `ctest --test-dir build/c -R tst_replayplaylist --output-on-failure` → FAIL (methods undefined / not built).

- [ ] **Step 3: Implement.** Add to `replayplaylist.cpp`:
```cpp
void ReplayPlaylist::removeEntry(int index) {
    if (index >= 0 && index < m_entries.size()) m_entries.remove(index);
}
void ReplayPlaylist::clear() { m_entries.clear(); }
```
declare both in the header (confirm the member is `QVector<ReplayEntry> m_entries`).

- [ ] **Step 4: Run, verify pass.** Same ctest → PASS.

- [ ] **Step 5: Commit.** `feat(playlist): ReplayPlaylist removeEntry + clear (unit-tested)`.

### Task 2: `PlaylistEntriesModel` (QAbstractListModel, TDD)

**Files:**
- Create: `playback/playlistentriesmodel.h`, `playback/playlistentriesmodel.cpp`
- Test: `tests/unit/tst_playlistentriesmodel.cpp` + register
- Modify: `CMakeLists.txt` (add the two sources to the app target's `SOURCES`)

**Interfaces:**
- Produces:
```cpp
class PlaylistEntriesModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { IndexRole = Qt::UserRole + 1, InMsRole, OutMsRole, DurationMsRole, SpeedRole, ClipPathRole, LabelRole };
    explicit PlaylistEntriesModel(QObject* parent = nullptr);
    void setEntries(const QVector<ReplayEntry>& entries);   // full refresh (begin/endResetModel)
    int rowCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
private:
    QVector<ReplayEntry> m_rows;
};
```
- `DurationMsRole` = `outMs < 0 ? -1 : outMs - inMs`. `LabelRole` = clipPath basename for now (a UIManager-provided label can replace it in Task 3 if cheap).

- [ ] **Step 1: Write failing tests.** roleNames contains `inMs`/`outMs`/`durationMs`/`speed`/`label`; after `setEntries` of 2 rows `rowCount()==2`; `data(index(0), DurationMsRole)` == `outMs-inMs`; open out-point → duration −1.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** the model (begin/endResetModel in `setEntries`; `roleNames()` maps the enum to the QML names `index/inMs/outMs/durationMs/speed/clipPath/label`).

- [ ] **Step 4: Run, verify pass.**

- [ ] **Step 5: Register the type for QML.** In `main.cpp`, `qmlRegisterUncreatableType<PlaylistEntriesModel>("Recorder.Types", 1, 0, "PlaylistEntriesModel", "Owned by UIManager")` (mirror the existing `qmlRegisterUncreatableType<StreamDeckManager>` line).

- [ ] **Step 6: Build + suite.** Build clean; the new unit test + the full unit suite pass.

- [ ] **Step 7: Commit.** `feat(playlist): PlaylistEntriesModel (QAbstractListModel, unit-tested)`.

### Task 3: UIManager — expose the model + current/next + edits

**Files:**
- Modify: `uimanager.h`, `uimanager.cpp`
- Test: extend a UIManager test if one exists; otherwise covered by app-load + the rail.

**Interfaces:**
- Produces on UIManager:
  - `Q_PROPERTY(PlaylistEntriesModel* playlistModel READ playlistModel CONSTANT)` (owns an instance, refreshed from `m_playlist.entries()` whenever the playlist mutates — i.e. after `markIn/markOut/recallEntry/removePlaylistEntry/clearPlaylist/setPlaylistEntrySpeed`).
  - `Q_PROPERTY(int currentPlaylistEntryIndex READ currentPlaylistEntryIndex NOTIFY playlistEntryChanged)` (= `m_playout.active() ? m_playout.currentIndex() : -1`).
  - `Q_PROPERTY(int nextPlaylistEntryIndex READ nextPlaylistEntryIndex NOTIFY playlistEntryChanged)` (current+1 if in range while active, else −1).
  - `Q_INVOKABLE void removePlaylistEntry(int index)`, `Q_INVOKABLE void clearPlaylist()`, `Q_INVOKABLE void setPlaylistEntrySpeed(int index, double speed)` — each mutates `m_playlist`, then refreshes the model (`m_playlistModel->setEntries(m_playlist.entries())`).
  - `signal playlistEntryChanged()`.

- [ ] **Step 1: Add the model member + property.** `PlaylistEntriesModel* m_playlistModel` (new in ctor, parented to `this`); getter returns it. Add a private helper `refreshPlaylistModel()` calling `m_playlistModel->setEntries(m_playlist.entries())`.

- [ ] **Step 2: Refresh on every mutation.** Call `refreshPlaylistModel()` at the end of `markIn()`, `markOut()`, `recallEntry()`, and the three new edit methods. Implement the three edits (`removePlaylistEntry` → `m_playlist.removeEntry(index); refreshPlaylistModel()`, etc.; `setPlaylistEntrySpeed` → `m_playlist.setSpeed(index, speed); refreshPlaylistModel()`).

- [ ] **Step 3: current/next + signal.** Add the getters; expose `PlaylistPlayout::currentIndex()` (already exists). In `onPlayoutTick()` (where the playout advances), after a boundary fires/index changes, `emit playlistEntryChanged()`. Also emit it in `playPlaylist()` (start) and `stopPlaylistPlayout()` (→ -1).

- [ ] **Step 4: Build + suite.** Build clean; unit suite 100% (no timing change — only added emits + a model refresh on the UIManager thread).

- [ ] **Step 5: App-load smoke.** Offscreen app-load: confirm `uiManager.playlistModel` is non-null and no QML errors (the rail isn't built yet; a temporary `console.log(uiManager.playlistModel)` in Main.qml or a probe confirms exposure, then remove it).

- [ ] **Step 6: Commit.** `feat(ui): expose playlist model + current/next + entry edits on UIManager`.

---

# QML — the rail

### Task 4: `RundownRail.qml`

**Files:**
- Create: `ui/components/RundownRail.qml`
- Modify: `CMakeLists.txt` (QML_FILES), `tests/smoke/CMakeLists.txt` + `.github/workflows/ci.yml` (qml_smoke/Lint file lists)

**Interfaces:**
- Produces: `RundownRail { property var ui; property bool expanded: true }` — a `ColumnLayout`/`Frame` with a header (title + collapse toggle + active indicator), a `ListView { model: ui.playlistModel }`, and a footer (Play / Stop / Clear). Collapses to a thin strip showing current/next labels.
- Consumes: `ui.playlistModel` (roles: index/inMs/outMs/durationMs/speed/clipPath/label), `ui.currentPlaylistEntryIndex`, `ui.nextPlaylistEntryIndex`, `ui.playlistPlayoutActive()`, `ui.recallEntry(i)`, `ui.removePlaylistEntry(i)`, `ui.clearPlaylist()`, `ui.setPlaylistEntrySpeed(i, s)`, `ui.playPlaylist(0)`, `ui.stopPlaylistPlayout()`, `ui.recordTimecode(ms)`.

- [ ] **Step 1: Create the file.** Header (`pragma ComponentBehavior: Bound` + QtQuick/Controls/Layouts/OlrTheme). Root `Frame { id: root; property var ui; property bool expanded: true; … }`. Delegate per entry:
```qml
delegate: ItemDelegate {
    required property int index
    required property var model       // model.inMs/outMs/durationMs/speed/label
    width: ListView.view.width
    highlighted: index === root.ui.currentPlaylistEntryIndex
    // edge accent: current = recordOnAir, next = armed
    Rectangle { width: 3; height: parent.height; color:
        index === root.ui.currentPlaylistEntryIndex ? Theme.recordOnAir
        : (index === root.ui.nextPlaylistEntryIndex ? Theme.armed : "transparent") }
    contentItem: RowLayout {
        Label { text: (index + 1); color: Theme.textDim }
        Label { text: model.label; color: Theme.textHi; elide: Text.ElideRight; Layout.fillWidth: true }
        Label { text: root.ui.recordTimecode(model.inMs) + (model.outMs < 0 ? "  —" : "→" + root.ui.recordTimecode(model.outMs)); color: Theme.textBody; font.family: Theme.fontMono }
        // speed chip (armed colour when ≠ 1.0)
        Rectangle { visible: model.speed !== 1.0; color: Theme.armed; radius: Theme.r1
            Label { text: model.speed + "x"; color: Theme.textOnTally } }
        ToolButton { text: "Recall"; onClicked: root.ui.recallEntry(index) }
        ToolButton { text: "✕"; onClicked: root.ui.removePlaylistEntry(index) }
    }
}
```
Header: title "RUNDOWN", a collapse toggle (binds `root.expanded`), and an "● LIVE" indicator when `ui.playlistPlayoutActive()`. Footer: `Button { text: "Play"; onClicked: ui.playPlaylist(0) }`, `Button { text: "Stop"; onClicked: ui.stopPlaylistPlayout() }`, `Button { text: "Clear"; onClicked: ui.clearPlaylist() }`. When `!expanded`, show only a thin strip with the current/next labels.

- [ ] **Step 2: Register** in the three places (CMake QML_FILES, qml_smoke list, ci.yml Lint list).

- [ ] **Step 3: Build + lint.** Build clean; `qml_smoke` Passed.

- [ ] **Step 4: Render with a mock model.** `tests/qmlstyle/preview_rundown.qml`: a stub `ui` whose `playlistModel` is a small inline `ListModel` (rows with index/label/inMs/outMs/durationMs/speed) and `currentPlaylistEntryIndex:0`, `nextPlaylistEntryIndex:1`, stub `recordTimecode`. Render → inspect: rows with current(red edge)/next(amber edge), a speed chip on the slow-mo row, Recall/✕ per row, Play/Stop/Clear footer.

- [ ] **Step 5: Commit.** `feat(ui): RundownRail component`.

### Task 5: Shell — dock the rail beside PgmStage + StatusStrip toggle

**Files:**
- Modify: `Main.qml` (center row), `ui/components/StatusStrip.qml` (a Rundown toggle button + signal)

**Interfaces:**
- StatusStrip gains `signal toggleRundown()` and a `property bool rundownOpen` (highlight). Main.qml wires the toggle to the rail's `expanded`.

- [ ] **Step 1: StatusStrip toggle.** Add a `Button { text: "Rundown"; highlighted: root.rundownOpen; onClicked: root.toggleRundown() }` beside the Config button; declare `signal toggleRundown()` + `property bool rundownOpen`.

- [ ] **Step 2: Dock the rail.** In Main.qml, wrap PgmStage + the rail in a `RowLayout` (replacing the bare PgmStage row): PgmStage `Layout.fillWidth/fillHeight`, then `RundownRail { id: rundownRail; ui: appWindow.uiManagerRef; Layout.fillHeight: true; Layout.preferredWidth: expanded ? Theme.bpSM * 0.6 : 36 }`. Wire `statusStrip.rundownOpen: rundownRail.expanded` and `statusStrip.onToggleRundown: rundownRail.expanded = !rundownRail.expanded`.

- [ ] **Step 3: Build + lint + render the cockpit.** Build clean; `qml_smoke` Passed; render `preview_cockpit.qml` (extend it to include the rail) → inspect the rail beside the stage, collapse/expand.

- [ ] **Step 4: App-load (integration).** Offscreen app-load: style resolves, no QML errors, `playlistModel` binds (the rail shows whatever entries exist — empty at startup is fine).

- [ ] **Step 5: Suite.** `ctest --test-dir build/c -L 'unit|smoke'` 100%.

- [ ] **Step 6: Commit.** `feat(ui): dock RundownRail in the cockpit + StatusStrip toggle`.

## Self-Review
- Roadmap Phase 3 coverage: entries model (Task 2) + UIManager surface (Task 3) + ReplayPlaylist edits (Task 1) + the rail (Task 4) + shell docking/toggle (Task 5). ✓
- MVP scope honoured; reorder/insert/in-out-edit/persistence explicitly deferred (Global Constraints; rail has no reorder handles). ✓
- Engine TDD: Tasks 1–2 have failing-test-first steps; Task 3 keeps the unit suite green and the playout timing untouched; concurrency note covers the `onPlayoutTick` emits. ✓
- Type consistency: model roles `index/inMs/outMs/durationMs/speed/clipPath/label` used identically in the model (Task 2) and the delegate (Task 4); `currentPlaylistEntryIndex`/`nextPlaylistEntryIndex` consistent across Task 3 and Tasks 4–5. ✓
- No bare `uiManager`. ✓
