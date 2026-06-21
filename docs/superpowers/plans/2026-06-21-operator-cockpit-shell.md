# Operator Cockpit shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the 3-tab `Main.qml` (where PGM video, transport, and tally are hidden unless the Playback tab is active and the transport scrolls off short windows) into a persistent broadcast **operator cockpit**: a pinned StatusStrip + a flexible PgmStage + a pinned TransportDock, with all configuration in a one-click ConfigDrawer.

**Architecture:** Extract the 2,268-line monolith into focused QML components under `ui/components/` (registered in the existing `OpenLiveReplay` app QML module). Do it in three behavior-preserving waves — config panels, then operator regions, then the shell flip from `TabBar`/`StackLayout` to the cockpit layout + drawer — followed by responsive tuning. Presentation-only: every component binds to the same injected `uiManager`; no C++/engine change.

**Tech Stack:** Qt 6.10.1, QML / QtQuick.Controls (bespoke `OlrStyle` style + `OlrTheme` token singleton, already on `main`), CMake/Ninja, CTest. Reference spec: [docs/superpowers/specs/2026-06-21-operator-cockpit-shell-design.md](../specs/2026-06-21-operator-cockpit-shell-design.md).

## Global Constraints

- **No engine changes.** Do not modify any C++, `UIManager`, `PlaybackTransport`, or the WebSocket control API. Every component reads the same `uiManager` API. If a task seems to need a C++ change, stop — it is out of scope.
- **Dependency-inject `uiManager`.** Every extracted component declares `property var ui` and the shell passes `ui: appWindow.uiManagerRef`. Components reference `ui.*` — never bare `uiManager` (bare `uiManager` is unqualified access and the lint gate promotes `--unqualified` to an error). The shell keeps the single existing aliased access (`property var uiManagerRef: uiManager`, wrapped in `// qmllint disable unqualified`).
- **`Theme` is global** via `import OlrTheme` (singleton). Use `Theme.*` tokens; do not reintroduce inline hex where a token exists. Opportunistically replace inline hex in a region you are already moving, but do not chase hex globally.
- **Style/lint floor:** all new QML must pass `qmllint --unqualified error --Quick.layout-positioning error`. Suppress a genuine false positive inline with `// qmllint disable <category>` / `// qmllint enable <category>` (as `Main.qml:29-31` does), never by weakening the gate.
- **Qt kit:** `~/Qt/6.10.1/macos`. Build dir: `build/c` (this worktree). qmllint: `~/Qt/6.10.1/macos/bin/qmllint`.
- **Frequent commits:** one commit per task (each task leaves the app building, linting, and rendering).
- **The app must stay runnable after every task in Waves A–B** (it is still the 3-tab layout, visually identical). Only Wave C deliberately changes the visible structure.

---

## Conventions used by every task

**Working directory:** `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/operator-cockpit` (this worktree, on latest `origin/main`).

**Move convention.** Most extraction steps *relocate existing QML*. A step that says “move `Main.qml:A–B` into `NewFile.qml`” means: cut that exact line range out of `Main.qml`, paste it into the new file’s root, and apply only the listed adjustments (usually: add the file header/imports, wrap in the stated root type, declare `property var ui`, and rewrite `appWindow.uiManagerRef` → `ui` and any `playbackTab.X`/`appWindow.X` per the task). Do not rewrite the moved markup otherwise. The executor has `Main.qml` open and applies the cut/paste; this plan gives the boundaries, the wrapper, and every adjustment.

**Component file header** (every new `ui/components/*.qml` starts with exactly the imports it needs; the common set is):
```qml
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme
```
(PgmStage additionally `import QtMultimedia`. SourceListPanel additionally needs no extra import. Add `import QtQuick.Window` only if a region uses `Window.*`.)

**Verification gates** (exact commands; “G1…G5” are referenced by tasks):

- **G1 — build:** `cmake --build build/c 2>&1 | tail -3`
  Expected: ends with `Linking CXX executable OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay` and **no** `error:` / `is not a type` / `Required property … was not initialized` lines above it. qmlcachegen compiles every QML file, so a syntax/type/missing-import error in a new component fails here.
- **G2 — strict lint:** `ctest --test-dir build/c -R '^qml_smoke$' --output-on-failure`
  Expected: `1/1 Test #…: qml_smoke … Passed`. (qml_smoke lints with `-I build/c/qml`, so it resolves `OlrTheme` and the app module’s sibling components.)
- **G3 — render a component/preview to a PNG and inspect it:**
  `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./build/c/tests/qmlstyle/uipreview <preview.qml> build/c/preview-<name>.png <w> <h>`
  Expected: exit 0, a PNG written; open/inspect it. (Outputs go under `build/c/` — never `/tmp`.) Each task names the preview file to use.
- **G4 — app loads under the real style (integration):**
  `QT_QPA_PLATFORM=offscreen QT_LOGGING_RULES="qt.quick.controls.style=true" ./build/c/OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay > build/c/app-load.log 2>&1 & APP=$!; sleep 4; kill $APP; wait $APP 2>/dev/null; grep -E 'resolved=true|\.qml:[0-9]+|Cannot assign|is not a type|TypeError|Required property' build/c/app-load.log | grep -vi 'WebSocket\|port 8115\|font'`
  Expected: a `resolved=true` line (style=OlrStyle) and **no** QML error lines.
- **G5 — full unit suite (no engine regressions):** `ctest --test-dir build/c -L 'unit|smoke' --output-on-failure 2>&1 | tail -6`
  Expected: `100% tests passed`.

**Commit** (each task’s last step): `git add -A && git commit -m "<message>"`. Messages end with a trailing line `Co-Authored-By: Claude <noreply@anthropic.com>` (per repo CLAUDE.md). Do **not** push until the plan is complete and a human asks; pushing runs the required pre-push hook (never `--no-verify`).

---

## File structure

New files (all under the `OpenLiveReplay` app QML module unless noted):

| File | Responsibility |
|---|---|
| `ui/components/BindingsPanel.qml` | MIDI + Stream Deck mapping cards (config) |
| `ui/components/ProjectSettingsPanel.qml` | name/save-loc/res/fps/audio-latency + CodecSettingsPanel host (config) |
| `ui/components/OutputsPanel.qml` | NDI Outputs table (config) |
| `ui/components/SourceListPanel.qml` | source rows + the 3 metadata/import modal popups + import auto-open (config) |
| `ui/components/PgmStage.qml` | single/multi video display, tally tiles, **owns the playback view-state** (selectedIndex/viewMode/visibleStreamIndexes/grid + the uiManager view Connections + `reattachProviders()`) |
| `ui/components/TransportDock.qml` | scrub timeline + transport keys + timecode (owns `clockTimer`, `showTimeOfDay`, `clockTick`, `holdWasPlaying`, the timecode formatters) |
| `ui/components/StatusStrip.qml` | **new:** record arm/START-STOP + REC timer + clock + per-source tally row + Fullscreen-Multiview button + `[Config]` toggle |
| `ui/components/ConfigDrawer.qml` | **new:** slide-over container hosting the 4 config panels in a section switcher |

Modified files:

| File | Change |
|---|---|
| `Main.qml` | Waves A–B: replace inline blocks with component instances (still 3 tabs). Wave C: replace `TabBar`/`StackLayout` with `StatusStrip` + `PgmStage` + telemetry strip + `TransportDock` + `ConfigDrawer`; rewire the `MultiviewWindow` binding to `PgmStage.visibleStreamIndexes`; keep `folderDialog`/`screenProbe`/`screenMenu`/multiview funcs in the shell. |
| `CMakeLists.txt` | add each `ui/components/*.qml` to `qt_add_qml_module(OpenLiveReplay … QML_FILES …)` (after `CodecSettingsPanel.qml`, line 128). |
| `tests/smoke/CMakeLists.txt` | add each new component to the `qml_smoke` file list. |
| `.github/workflows/ci.yml` | add each new component to the Lint job’s `qmllint …` file list. |

Not extracted this phase (stays inline in the shell): the telemetry panel (`Main.qml` ~923–974), the “External Input Settings” block (~1531–1595), the “Save Config” button (~2253–2263), `folderDialog`, `screenProbe`, `screenMenu`, the record warning timer.

---

# Wave A — config panels (app stays 3-tab, visually identical)

### Task 1: Scaffolding — `ui/components/` + lint/render plumbing + first panel (BindingsPanel)

This task creates the component directory, wires CMake + both lint gates, and lands the first (cleanest) extraction end-to-end so the pipeline is proven.

**Files:**
- Create: `ui/components/BindingsPanel.qml`
- Modify: `CMakeLists.txt:125-128`, `tests/smoke/CMakeLists.txt:19-21`, `.github/workflows/ci.yml:251`, `Main.qml` (Control tab body, currently ~321–652)

**Interfaces:**
- Produces: `BindingsPanel { property var ui }` — a `ColumnLayout` containing the MIDI card + Stream Deck card; all behavior calls `ui.*`. No signals needed (cards own their `expanded` state).
- Consumes: nothing from other tasks.

- [ ] **Step 1: Configure a fresh build** (first build of this worktree).

Run:
```bash
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
```
Expected: `Configuring done` / `Generating done`. Then run **G1** to confirm a clean baseline build, **G5** to confirm a green baseline.

- [ ] **Step 2: Create `ui/components/BindingsPanel.qml`.**

Wrapper + move. New file starts with the common header (above), root `ColumnLayout { id: root; property var ui; Layout.fillWidth: true; spacing: 12 … }`. **Move** `Main.qml`’s `midiCard` Frame (~321–486) and `streamDeckCard` Frame (~488–652) into it as the two children. Adjustments: replace every `appWindow.uiManagerRef` with `ui`; the delegate ids `midiRow`/`sdRow` and the `expanded` properties are local and need no change; the `streamDeckCard` `visible: ui.streamDeck.supported` binding stays.

- [ ] **Step 3: Register the file in the app module.**

In `CMakeLists.txt`, inside `qt_add_qml_module(OpenLiveReplay … QML_FILES …)` (after `CodecSettingsPanel.qml`, line 128), add:
```cmake
        ui/components/BindingsPanel.qml
```

- [ ] **Step 4: Add it to both lint gates.**

`tests/smoke/CMakeLists.txt` — append to the `qml_smoke` `add_test` file list (after the `CodecSettingsPanel.qml` line):
```cmake
        "${CMAKE_SOURCE_DIR}/ui/components/BindingsPanel.qml"
```
`.github/workflows/ci.yml` — in the `qmllint` step, extend the final command’s file list:
```
            Main.qml MultiviewWindow.qml CodecSettingsPanel.qml \
            ui/components/BindingsPanel.qml
```

- [ ] **Step 5: Wire `Main.qml` to use it.** Replace the two moved Frames in the Control tab with one instance:
```qml
                BindingsPanel { Layout.fillWidth: true; ui: appWindow.uiManagerRef }
```

- [ ] **Step 6: Build + lint.** Run **G1** (compiles the new component; expected clean) then **G2** (qml_smoke Passed — the component lints clean because all access is `ui.*`/`Theme.*`).

- [ ] **Step 7: Visual check.** Create `tests/qmlstyle/preview_bindings.qml`:
```qml
import QtQuick
import OpenLiveReplay
Item {
    width: 720; height: 520
    // ui is omitted: a load/paint smoke — bindings to ui.* are inert (undefined),
    // which renders the empty cards (verifies layout + style, not live data).
    BindingsPanel { anchors.fill: parent }
}
```
Run **G3** with `tests/qmlstyle/preview_bindings.qml … build/c/preview-bindings.png 720 520` and inspect: two dark cards (MIDI, Stream Deck) styled by OlrStyle, no light/Basic fallback, no overlap.

- [ ] **Step 8: App-load + suite.** Run **G4** (no QML errors; record/control tab loads) and **G5** (all green).

- [ ] **Step 9: Commit.**
```bash
git add -A && git commit -m "$(printf 'ui(cockpit): extract BindingsPanel + scaffold ui/components\n\nCo-Authored-By: Claude <noreply@anthropic.com>')"
```

### Task 2: `ProjectSettingsPanel`

**Files:**
- Create: `ui/components/ProjectSettingsPanel.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`, `Main.qml` (Project tab project-settings block ~1217–1341)

**Interfaces:**
- Produces: `ProjectSettingsPanel { property var ui; signal browseFolderRequested() }` — a `ColumnLayout` (name/save-loc/res/fps/audio-latency + `CodecSettingsPanel`).
- Consumes: nothing.

- [ ] **Step 1: Create the file.** Header + root `ColumnLayout { id: root; property var ui; signal browseFolderRequested(); Layout.fillWidth: true; spacing: 12 }`. **Move** `Main.qml:1217–1341` (the “Project Settings” header Text, the two `GridLayout` blocks, and the `CodecSettingsPanel` child) into it. Adjustments: `appWindow.uiManagerRef` → `ui`; the Browse button’s `onClicked: folderDialog.open()` → `onClicked: root.browseFolderRequested()`; the `CodecSettingsPanel` child keeps `controller: ui` (was `controller: appWindow.uiManagerRef`).

- [ ] **Step 2: Register** in `CMakeLists.txt` QML_FILES, `tests/smoke/CMakeLists.txt`, and `ci.yml` (same three edits as Task 1, with `ui/components/ProjectSettingsPanel.qml`).

- [ ] **Step 3: Wire `Main.qml`.** Replace the moved block with:
```qml
                ProjectSettingsPanel {
                    Layout.fillWidth: true
                    ui: appWindow.uiManagerRef
                    onBrowseFolderRequested: folderDialog.open()
                }
```

- [ ] **Step 4: Build + lint.** Run **G1**, **G2** (expected clean/Passed).

- [ ] **Step 5: Visual check.** Add `tests/qmlstyle/preview_project.qml` (same shape as Task 1’s preview, `ProjectSettingsPanel { anchors.fill: parent }`, 640×420). Run **G3** → inspect: the settings form renders dark/aligned; the FPS combo + codec panel present.

- [ ] **Step 6: App-load + suite.** Run **G4**, **G5**.

- [ ] **Step 7: Commit** `ui(cockpit): extract ProjectSettingsPanel`.

### Task 3: `OutputsPanel`

**Files:**
- Create: `ui/components/OutputsPanel.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`, `Main.qml` (NDI Outputs GroupBox ~1343–1529)

**Interfaces:**
- Produces: `OutputsPanel { property var ui }` — root `GroupBox` (`title: "NDI Outputs"`) with the inner `ndiOutputScroll` and the `statusColor(severity)` method.
- Consumes: nothing.

- [ ] **Step 1: Create the file.** Header + root `GroupBox { id: root; property var ui; title: "NDI Outputs"; Layout.fillWidth: true; … }`. **Move** `Main.qml:1343–1529` (the `ndiOutputSettings` GroupBox body) in. Adjustments: `appWindow.uiManagerRef` → `ui`; the Repeater `model: ndiOutputSettings.rows` → `model: root.rows` (define `readonly property var rows: ui ? ui.ndiOutputRows() : []` on root, mirroring the current binding); keep `statusColor()` as a root method; preserve `Layout.minimumWidth: 744` on header+rows; the known internal `ndiOutputScroll.contentWidth` binding loop is pre-existing — do not “fix” it here (out of scope).

- [ ] **Step 2: Register** (3 edits, `ui/components/OutputsPanel.qml`).

- [ ] **Step 3: Wire `Main.qml`.** Replace the moved GroupBox with:
```qml
                OutputsPanel { Layout.fillWidth: true; ui: appWindow.uiManagerRef }
```

- [ ] **Step 4: Build + lint.** **G1**, **G2**.

- [ ] **Step 5: Visual check.** `tests/qmlstyle/preview_outputs.qml` (`OutputsPanel { anchors.fill: parent }`, 820×360). **G3** → inspect: the table header + (empty, since `ui` undefined → `rows: []`) body render dark; columns aligned.

- [ ] **Step 6: App-load + suite.** **G4**, **G5**.

- [ ] **Step 7: Commit** `ui(cockpit): extract OutputsPanel (NDI outputs table)`.

### Task 4: `SourceListPanel` (with the 3 modal popups + import auto-open)

The trickiest extraction: it carries three modal `Popup`s and an auto-open hook currently in the shell `Connections`.

**Files:**
- Create: `ui/components/SourceListPanel.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`, `Main.qml` (source list header+ListView ~1597–1746, the three popups ~1749–2251, and the shell `Connections.onImportPreviewChanged` ~150–156)

**Interfaces:**
- Produces:
  - `SourceListPanel { property var ui; function maybeAutoOpenImport() }` — root `ColumnLayout`; owns `importPreviewPopup`, `metadataFieldsEditor`, `metadataEditor` internally.
  - `maybeAutoOpenImport()` runs the old auto-open guard: `if (ui.importPreviewReady && !ui.isRecording && !importPreviewPopup.opened) importPreviewPopup.open()`.
- Consumes: nothing.

- [ ] **Step 1: Create the file.** Header + root `ColumnLayout { id: root; property var ui; Layout.fillWidth: true; spacing: 8 }`. **Move** the header RowLayout + `streamList` ListView (`Main.qml:1597–1746`) and the three `Popup`s (`1749–2251`) into it. Adjustments: `appWindow.uiManagerRef` → `ui`; the header “Metadata Fields” button keeps `onClicked: metadataFieldsEditor.openEditor()` and per-row “Metadata” buttons keep `onClicked: metadataEditor.openFor(streamRow.index)` (all now siblings inside this component); the popups’ `anchors.centerIn: Overlay.overlay` is unchanged (global). Add the method:
```qml
    function maybeAutoOpenImport() {
        if (ui && ui.importPreviewReady && !ui.isRecording && !importPreviewPopup.opened)
            importPreviewPopup.open()
    }
```

- [ ] **Step 2: Rewire the shell auto-open.** In `Main.qml`’s root `Connections.onImportPreviewChanged` (~150–156), replace the body that referenced `importPreviewPopup` with a call to the panel instance (the panel id assigned in Step 3):
```qml
        function onImportPreviewChanged() {
            sourceListPanel.maybeAutoOpenImport()
        }
```

- [ ] **Step 3: Register + wire `Main.qml`.** Three registration edits (`ui/components/SourceListPanel.qml`). Replace the moved source-list block in the Project tab with:
```qml
                SourceListPanel {
                    id: sourceListPanel
                    Layout.fillWidth: true
                    ui: appWindow.uiManagerRef
                }
```
Delete the three popup definitions from their old location in `Main.qml` (they now live in the component). Leave the “External Input Settings” block (~1531–1595) and the “Save Config” button (~2253–2263) in `Main.qml`.

- [ ] **Step 4: Build + lint.** **G1**, **G2**. (Build catches a missed popup reference or a stray `importPreviewPopup` id left in the shell.)

- [ ] **Step 5: Visual check.** `tests/qmlstyle/preview_sources.qml` (`SourceListPanel { anchors.fill: parent }`, 700×360). **G3** → inspect: the header (Input Sources / Metadata Fields / + Add Stream) renders; the (empty) list area is dark.

- [ ] **Step 6: App-load + suite.** **G4** (watch for any `importPreviewPopup is not defined`), **G5**.

- [ ] **Step 7: Commit** `ui(cockpit): extract SourceListPanel (rows + metadata/import popups)`.

---

# Wave B — operator regions (app still 3-tab)

### Task 5: `PgmStage` (video display + the playback view-state controller)

PgmStage owns the view state currently on `playbackTab` (since after the shell flip there is no `playbackTab`). The telemetry panel stays in the shell.

**Files:**
- Create: `ui/components/PgmStage.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`, `Main.qml` (playbackTab properties/funcs/Connections ~663–752 and the `playbackView` Item ~763–921; leave the telemetry panel ~923–974)

**Interfaces:**
- Produces (the shell and TransportDock read these):
  - `PgmStage { property var ui }`
  - `property int selectedIndex` (init −1), `property string viewMode` (init "multi")
  - `property var visibleStreamIndexes` — **read by the shell** to bind `MultiviewWindow.visibleStreamIndexes`
  - `readonly property int streamCount`, `gridColumns`, `gridRows`
  - `property var pgmProvider: ui ? ui.pgmPreviewProvider : null`, `multiviewProvider: ui ? ui.multiviewPreviewProvider : null`
  - `function updateVisibleStreams()`, `function reattachProviders()` (was `reattachPreviewProviders()`)
- Consumes: nothing.

- [ ] **Step 1: Create the file.** Header **plus** `import QtMultimedia`. Root `Item { id: root; property var ui; Layout.fillWidth: true; Layout.fillHeight: true … }`. **Move into the root:** the `playbackTab` property declarations (`Main.qml:663–673`, dropping `showTimeOfDay`/`clockTick`/`holdWasPlaying` which go to TransportDock — keep `selectedIndex`, `visibleStreamIndexes`, `streamCount`, `pgmProvider`, `multiviewProvider`, `viewMode`, `gridColumns`, `gridRows`), `updateVisibleStreams()` (699–706), `reattachPreviewProviders()` renamed `reattachProviders()` (708–711), the `Component.onCompleted` init (713–718), the view `Connections` (720–752), and the `playbackView` Item body (763–921). Adjustments: `appWindow.uiManagerRef` → `ui`; `playbackTab.X` → `root.X` (or bare `X`); the click handlers’ `ui.setPlaybackViewState(...)` stay; `onVisibleChanged` init (754–761) folds into `Component.onCompleted` (the stage is always visible in the cockpit). Replace inline hex (`#003080`/`#333333`/`#eeeeee`) with the nearest `Theme.*` token.

- [ ] **Step 2: Register** (3 edits, `ui/components/PgmStage.qml`).

- [ ] **Step 3: Wire `Main.qml`.** In the Playback tab, replace the `playbackView` Item (and the now-moved playbackTab state) with:
```qml
                PgmStage {
                    id: pgmStage
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ui: appWindow.uiManagerRef
                }
```
Update the two references to the old playback state:
  - the `appWindow.playbackTab` alias (`Main.qml:32`) and the `MultiviewWindow` binding (`Main.qml:79–81`): change the binding to read `pgmStage.visibleStreamIndexes` (and update/remove the `playbackTab` alias — replace with `property alias pgmStage: pgmStage` if any shell code needs it; the `Component.onCompleted` lines 57–58 that set `playbackTab.selectedIndex/viewMode` move to `pgmStage.selectedIndex/viewMode` or are dropped since PgmStage initializes itself).
  - the `clockTimer.restart()` call site moves with TransportDock (Task 6) — for now the timecode block still lives in the shell Playback tab and references the (still-present) `playbackTab.*` timecode state; keep that state until Task 6. **Adjustment:** to keep the app compiling between Task 5 and Task 6, leave `showTimeOfDay`/`clockTick`/`holdWasPlaying`/`clockTimer`/`formatTimecode`/`formatTimeOfDay` on a minimal retained `playbackTab`-scoped holder (a bare `QtObject { id: playbackClock … }` in the Playback tab) OR sequence Task 6 immediately after with a single combined build. **Recommended:** treat Tasks 5+6 as one commit boundary if the intermediate state is awkward — do Step-by-step but run G1 only after Task 6 wires TransportDock. (Mark this explicitly when executing.)

- [ ] **Step 4: Build + lint.** **G1**, **G2** (after Task 6 if you deferred per Step 3’s note).

- [ ] **Step 5: Visual check.** `tests/qmlstyle/preview_stage.qml` (`PgmStage { anchors.fill: parent }`, 640×360). **G3** → inspect: the dark stage area renders (no video without `ui`, but the multiview grid scaffolding/tally borders should lay out without error).

- [ ] **Step 6: App-load + suite.** **G4** (the real app provides `ui`, so PGM/multiview should attach providers — watch for `attachProvider` / `reattachProviders` errors), **G5**.

- [ ] **Step 7: Commit** `ui(cockpit): extract PgmStage (video + view-state controller)`.

### Task 6: `TransportDock` (scrub + transport keys + timecode + clock)

**Files:**
- Create: `ui/components/TransportDock.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`, `Main.qml` (the transport block ~976–1199, plus the timecode state moved out of playbackTab: `showTimeOfDay`/`clockTick`/`holdWasPlaying`/`clockTimer`/`formatTimecode`/`formatTimeOfDay`)

**Interfaces:**
- Produces: `TransportDock { property var ui }` — root `ColumnLayout`; owns the scrub `Slider`, the mark/recall/playlist row, and the transport-keys row + both timecode displays. Self-contained (timecode state + `clockTimer` live inside).
- Consumes: nothing (reads `ui.*`).

- [ ] **Step 1: Create the file.** Header. Root `ColumnLayout { id: root; property var ui; Layout.fillWidth: true; spacing: 8 }`. **Move in:** the timecode state from playbackTab (`showTimeOfDay` → `property bool showTimeOfDay: ui ? ui.timeOfDayMode : false`, `clockTick` int, `holdWasPlaying` bool), the `clockTimer` Timer (691–697), `formatTimecode()` (678–680 → `return ui.recordTimecode(ms)`), `formatTimeOfDay()` (682–689, unchanged), and the whole transport block (`976–1199`: scrubBar, Mark/Recall/Playlist row, transport-keys row, both timecode Texts). Adjustments: `appWindow.uiManagerRef` → `ui`; `playbackTab.X` → `root.X`; the scrub `value:` binding → `scrubBar.pressed ? scrubBar.value : (ui ? ui.scrubPosition : 0)`; keep the intentionally-invisible scrub `handle`; the duration Text’s `onVisibleChanged: clockTimer.restart()` stays (clockTimer is now a sibling).

- [ ] **Step 2: Register** (3 edits, `ui/components/TransportDock.qml`).

- [ ] **Step 3: Wire `Main.qml`.** In the Playback tab, replace the moved transport block (and the retained timecode holder from Task 5, if any) with:
```qml
                TransportDock { Layout.fillWidth: true; ui: appWindow.uiManagerRef }
```

- [ ] **Step 4: Build + lint.** **G1**, **G2**.

- [ ] **Step 5: Visual check.** `tests/qmlstyle/preview_transport.qml` (`TransportDock { anchors.fill: parent }`, 900×120). **G3** → inspect: scrub bar + transport keys + a `--:--:--` timecode render dark/aligned; PLAY is the primary key.

- [ ] **Step 6: App-load + suite.** **G4**, **G5**.

- [ ] **Step 7: Commit** `ui(cockpit): extract TransportDock (scrub + transport + timecode)`.

---

# Wave C — the cockpit shell flip

At the end of Wave B the Playback tab is `PgmStage` + telemetry + `TransportDock`; the Project tab is `ProjectSettingsPanel` + `OutputsPanel` + external-input + `SourceListPanel` + Save; the Control tab is the record control + multiview-count + `BindingsPanel`. Wave C makes the operator regions persistent and moves the config into a drawer.

### Task 7: `StatusStrip` (new persistent top row)

**Files:**
- Create: `ui/components/StatusStrip.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`

**Interfaces:**
- Produces:
  - `StatusStrip { property var ui; property bool configOpen; signal toggleConfig(); signal fullscreenMultiviewRequested(real x, real y) }`
  - A fixed-height `RowLayout` (`Theme.hControl`-ish) with: record arm/START-STOP + REC elapsed; the master clock/timecode (click toggles `ui.timeOfDayMode`); a per-source **tally row** (a `Repeater` over `ui.multiviewCount` CAM chips colored by `ui.sourceLinkHealth(i)`/`isSourceConnected(i)` using `Theme.idle/ready/armed/error`); a `Fullscreen Multiview ▾` button (emits `fullscreenMultiviewRequested`); a `[Config]` toggle button (`highlighted: configOpen`, emits `toggleConfig`).
- Consumes: nothing.

- [ ] **Step 1: Create the file** with full new QML. Skeleton:
```qml
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

RowLayout {
    id: root
    property var ui
    property bool configOpen: false
    signal toggleConfig()
    signal fullscreenMultiviewRequested(real x, real y)
    Layout.fillWidth: true
    implicitHeight: Theme.hControl
    spacing: Theme.s3

    // Record: arm/START-STOP + elapsed. Move the record Button + status Text that
    // currently live in Main.qml's Control tab (~the record RowLayout) into here,
    // replacing appWindow.uiManagerRef -> ui and appWindow.recordingError ->
    // (expose via a `property string recordingError` set by the shell — see Task 9).
    Button {
        text: ui && ui.isRecording ? "STOP" : "RECORD"
        highlighted: ui ? ui.isRecording : false
        onClicked: if (ui) ui.toggleRecording()   // use the SAME call the old button used
    }
    Label {
        text: ui ? Qt.formatTime(new Date(ui.recordedDurationMs || 0), "hh:mm:ss") : "00:00:00"
        font.family: Theme.fontMono
        color: Theme.textBody
    }

    Item { Layout.fillWidth: true }   // spacer

    // Per-source tally row.
    Row {
        spacing: Theme.s2
        Repeater {
            model: ui ? ui.multiviewCount : 0
            delegate: Rectangle {
                required property int index
                width: 44; height: Theme.hCompact; radius: Theme.r1
                color: Theme.panelRaised
                border.width: Theme.tallyBorder
                border.color: !ui ? Theme.idle
                    : (ui.isSourceConnected(index) ? Theme.ready
                       : (ui.isRecording ? Theme.error : Theme.idle))
                Label { anchors.centerIn: parent; text: "CAM" + (index + 1); font.pixelSize: Theme.fsMicro; color: Theme.textHi }
            }
        }
    }

    Button {
        text: "Fullscreen Multiview ▾"
        onClicked: root.fullscreenMultiviewRequested(x, y + height + 4)
    }
    Button {
        text: "Config"
        highlighted: root.configOpen
        onClicked: root.toggleConfig()
    }
}
```
(When moving the record control, copy the *exact* method names the current record button calls — confirm against `Main.qml`’s Control-tab record `Button`; the placeholders `ui.toggleRecording()` / `ui.recordedDurationMs` must be replaced with the real members the old button used.)

- [ ] **Step 2: Register** (3 edits, `ui/components/StatusStrip.qml`).

- [ ] **Step 3: Build + lint.** **G1**, **G2**.

- [ ] **Step 4: Visual check.** `tests/qmlstyle/preview_statusstrip.qml` (`StatusStrip { width: parent.width; anchors.top: parent.top }` in an 1000×80 dark Item). **G3** → inspect: one dark row — record button, mono clock, tally chips, Fullscreen + Config buttons; nothing clipped.

- [ ] **Step 5: Commit** `ui(cockpit): add StatusStrip (persistent record/clock/tally/config row)`.

### Task 8: `ConfigDrawer` (new slide-over hosting the config panels)

**Files:**
- Create: `ui/components/ConfigDrawer.qml`
- Modify: `CMakeLists.txt`, `tests/smoke/CMakeLists.txt`, `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `ConfigDrawer { property var ui; signal browseFolderRequested() }` — a `Drawer` (edge `Qt.RightEdge`) whose content is a section switcher (a left `TabBar`/`Column` of section buttons + a `StackLayout`) over the four panels. Width `Math.min(parent.width, Theme.bpSM * 1.4)`; full-width below `bpSM`. Forwards `ProjectSettingsPanel.onBrowseFolderRequested` up as its own `browseFolderRequested`.
- Consumes: `SourceListPanel`, `OutputsPanel`, `ProjectSettingsPanel`, `BindingsPanel` (Wave A).

- [ ] **Step 1: Create the file** with full new QML. Skeleton:
```qml
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

Drawer {
    id: root
    property var ui
    signal browseFolderRequested()
    edge: Qt.RightEdge
    width: Math.min(parent ? parent.width : Theme.bpSM, Theme.bpSM * 1.4)
    height: parent ? parent.height : 0
    modal: true
    dim: true

    contentItem: ColumnLayout {
        spacing: 0
        TabBar {
            id: sectionTabs
            Layout.fillWidth: true
            TabButton { text: "Sources" }
            TabButton { text: "Outputs" }
            TabButton { text: "Project" }
            TabButton { text: "Bindings" }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sectionTabs.currentIndex
            ScrollView { contentWidth: availableWidth; clip: true
                SourceListPanel { width: parent.width; ui: root.ui } }
            ScrollView { contentWidth: availableWidth; clip: true
                OutputsPanel { width: parent.width; ui: root.ui } }
            ScrollView { contentWidth: availableWidth; clip: true
                ProjectSettingsPanel { width: parent.width; ui: root.ui; onBrowseFolderRequested: root.browseFolderRequested() } }
            ScrollView { contentWidth: availableWidth; clip: true
                BindingsPanel { width: parent.width; ui: root.ui } }
        }
    }
}
```
(Each section is its own scroll — the spec’s “no single giant Project scroll”.)

- [ ] **Step 2: Register** (3 edits, `ui/components/ConfigDrawer.qml`).

- [ ] **Step 3: Build + lint.** **G1**, **G2**.

- [ ] **Step 4: Visual check.** `tests/qmlstyle/preview_drawer.qml`: an Item with a `ConfigDrawer { id: d; Component.onCompleted: d.open() }` sized 520×640. **G3** → inspect: the drawer panel renders dark with the 4 section tabs and the Sources panel; the section TabBar styled by OlrStyle.

- [ ] **Step 5: Commit** `ui(cockpit): add ConfigDrawer (slide-over config sections)`.

### Task 9: `Main.qml` shell restructure (the flip)

**Files:**
- Modify: `Main.qml` (replace `TabBar`+`StackLayout` body ~186–2265 with the cockpit; keep the shell header 10–185 + `folderDialog`/`screenProbe`/`screenMenu`/timers).

**Interfaces:**
- Consumes: `StatusStrip`, `PgmStage`, `TransportDock`, `ConfigDrawer` + the telemetry panel (kept inline).
- Produces: the assembled cockpit window.

- [ ] **Step 1: Replace the layout body.** Swap the `ColumnLayout { TabBar; StackLayout{ Control; Playback; Project } }` (≈186–2265) for:
```qml
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        StatusStrip {
            id: statusStrip
            Layout.fillWidth: true
            ui: appWindow.uiManagerRef
            configOpen: configDrawer.opened
            recordingError: appWindow.recordingError          // add `property string recordingError` to StatusStrip
            onToggleConfig: configDrawer.opened ? configDrawer.close() : configDrawer.open()
            onFullscreenMultiviewRequested: (x, y) => { screenMenu.x = x; screenMenu.y = y; screenMenu.open() }
        }

        PgmStage {
            id: pgmStage
            Layout.fillWidth: true
            Layout.fillHeight: true
            ui: appWindow.uiManagerRef
        }

        // Telemetry strip — kept inline (the existing ~923–974 panel), bound to
        // ui.telemetryRowsAtPlayhead(); visible only when it has rows.
        // (Paste the existing telemetry GroupBox here, appWindow.uiManagerRef -> kept.)

        TransportDock {
            Layout.fillWidth: true
            ui: appWindow.uiManagerRef
        }
    }

    ConfigDrawer {
        id: configDrawer
        ui: appWindow.uiManagerRef
        parent: Overlay.overlay
        onBrowseFolderRequested: folderDialog.open()
    }
```

- [ ] **Step 2: Rewire shared state.** (a) The `MultiviewWindow` binding (`Main.qml:79–81`) now reads `pgmStage.visibleStreamIndexes`. (b) `Component.onCompleted` (57–58) sets `pgmStage.selectedIndex = -1; pgmStage.viewMode = "multi"` (or rely on PgmStage’s own init and delete those lines). (c) `screenMenu` stays in the shell, opened by `StatusStrip.onFullscreenMultiviewRequested`. (d) The record control + multiview-count `SpinBox` that were in the Control tab: record moves into StatusStrip (Task 7); the multiview-count SpinBox moves into `ConfigDrawer`’s Project section (add it to `ProjectSettingsPanel`) or a small section — **decide at execution; default: add the view-count SpinBox to `ProjectSettingsPanel`** and drop the Control tab. (e) Delete the now-unused `playbackTab` alias.

- [ ] **Step 3: Build + lint.** **G1** (this is the big one — expect a few missing-reference fixups), **G2**.

- [ ] **Step 4: Render the whole cockpit.** `tests/qmlstyle/preview_cockpit.qml`: paste the cockpit `ColumnLayout` from Step 1 into a dark Item (1120×680) with stub-free components (no `ui` → inert). **G3** → inspect: StatusStrip pinned top, stage filling center, TransportDock pinned bottom; drawer closed.

- [ ] **Step 5: App-load (critical integration).** **G4** — the real app builds the full cockpit with a live `ui`. Confirm `resolved=true`, no QML errors, and (from the log) that PgmStage attaches providers. Also manually confirm (a second G4 run is fine) there’s no `playbackTab`/`importPreviewPopup`/`clockTimer is not defined`.

- [ ] **Step 6: Suite.** **G5** (all green; no engine change so unit tests are unaffected).

- [ ] **Step 7: Commit** `ui(cockpit): flip Main.qml shell to the persistent cockpit + drawer`.

---

# Wave D — responsive + finalize

### Task 10: Responsive pinning + breakpoint condensation + window floor

**Files:**
- Modify: `ui/components/StatusStrip.qml`, `ui/components/TransportDock.qml`, `ui/components/ConfigDrawer.qml`, `Main.qml` (window min)

**Interfaces:** none new.

- [ ] **Step 1: Confirm pinning.** Verify StatusStrip + TransportDock have fixed `implicitHeight` and `Layout.fillHeight` is **only** on `PgmStage`, so the deck never clips. (No code change if Tasks 7/9 already did this — assert it.)

- [ ] **Step 2: Condense StatusStrip below `bpMD`.** In `StatusStrip.qml`, drive the tally chip from full to dot and shorten labels by window width. Add `property int avail: root.width`; for each chip `width: root.avail < Theme.bpMD ? Theme.dotSize + 8 : 44` and hide the `CAM…` label when `root.avail < Theme.bpMD`; shorten the Fullscreen button text to `▾` below `bpSM`.

- [ ] **Step 3: Condense TransportDock below `bpMD`.** Wrap the six shuttle-speed `Button`s in a container that collapses to a compact group (smaller `implicitWidth`, drop the `×` suffix label) when `root.width < Theme.bpMD`; keep PLAY/PAUSE + GO LIVE full-size always.

- [ ] **Step 4: Drawer full-width below `bpSM`.** In `ConfigDrawer.qml`: `width: root.parent && root.parent.width < Theme.bpSM ? root.parent.width : Math.min(root.parent ? root.parent.width : Theme.bpSM, Theme.bpSM * 1.4)`.

- [ ] **Step 5: Re-tune the window floor.** Lower `Theme.windowMinH` (in `ui/theme/Theme.qml`) to the smallest height where StatusStrip + a minimal PgmStage + TransportDock all fit (find it empirically in Step 6). Keep `windowMinW` or lower to where the condensed StatusStrip fits.

- [ ] **Step 6: Render at every breakpoint.** Run **G3** on `preview_cockpit.qml` at `480×<floor>`, `520×600`, `840×640`, `1200×760`. Inspect each PNG: at every size StatusStrip + transport are fully visible and unclipped; only the stage shrinks; below `bpMD` the tally is dots and speeds are compact.

- [ ] **Step 7: Build + lint + app-load + suite.** **G1**, **G2**, **G4**, **G5**.

- [ ] **Step 8: Commit** `ui(cockpit): responsive pinning + breakpoint condensation + window floor`.

### Task 11: Finalize — gallery, docs, full verification

**Files:**
- Modify: `tests/qmlstyle/gallery_preview.qml` (optional: add a cockpit thumbnail), the spec/plan status line.

- [ ] **Step 1: Full gate sweep.** Run **G1**, **G2**, **G5**, and **G4** once more; capture a final `preview_cockpit.png` (closed + open drawer) via **G3** for the PR.

- [ ] **Step 2: qmllint parity with CI.** Reproduce the CI Lint command locally (synthesized OlrTheme stub + the new file list) and confirm exit 0 over `Main.qml MultiviewWindow.qml CodecSettingsPanel.qml ui/components/*.qml`. (Mirror the `ci.yml` qmllint step’s `LINT_IMPORTS` recipe.)

- [ ] **Step 3: Clean up preview files.** Keep `gallery_preview.qml`; the per-task `preview_*.qml` dev files may stay under `tests/qmlstyle/` (not registered, not tests) or be removed — remove the throwaways to keep the tree tidy.

- [ ] **Step 4: Commit** `ui(cockpit): finalize — full verification + cockpit gallery`.

---

## Self-Review

**Spec coverage (each spec section → task):**
- Persistent StatusStrip + PgmStage + pinned TransportDock → Tasks 5, 6, 7, 9, 10. ✓
- Config one click away in a drawer, PGM live behind → Tasks 8, 9. ✓
- Glanceable per-source tally → StatusStrip (Task 7). ✓
- Multiview inline or detached → PgmStage keeps the single/multi display; `MultiviewWindow` binding rewired to `pgmStage.visibleStreamIndexes` (Task 9, Step 2a); detach via the kept shell `screenMenu`/multiview funcs (Task 9). ✓
- Reflows, never clips → Task 10 (pinning + condensation + floor). ✓
- 8 component extractions, presentation-only, same `uiManager` → Tasks 1–8 (`ui` injection; Global Constraints). ✓
- New components in `ui/components/` in the app QML module → every task’s registration step. ✓
- Defer rich ScrubTimeline / Rundown rail / hex sweep / one-red-PGM tally → not in any task (explicit non-goals; “File structure / Not extracted”). ✓
- Testing: component compile (G1), strict lint (G2/qml_smoke + CI list), render (G3), app-load (G4), unit suite (G5). ✓

**Placeholder scan:** The StatusStrip record control (Task 7, Step 1) and the multiview-count SpinBox placement (Task 9, Step 2d) are flagged “confirm the exact member names / decide at execution” — these are genuinely keyed to current `Main.qml` member names the executor must read in place; the surrounding code and the exact source ranges are given. No `TODO`/`add error handling`/“similar to Task N” placeholders elsewhere; new components show full QML; moves give exact ranges + adjustments.

**Type consistency:** `reattachPreviewProviders()` (old) is consistently renamed `reattachProviders()` and only referenced inside PgmStage. `visibleStreamIndexes` is the single name read by the shell’s `MultiviewWindow` binding. `ui` is the injected `uiManager` everywhere. `configDrawer.opened` / `configOpen` / `toggleConfig` are consistent between StatusStrip (Task 7) and the shell (Task 9). `browseFolderRequested` is consistent across ProjectSettingsPanel (Task 2) → ConfigDrawer (Task 8) → shell `folderDialog.open()` (Task 9).

**Known sequencing note:** Tasks 5↔6 share the playbackTab timecode state; the plan calls out treating them as one build/commit boundary if the intermediate compile is awkward (Task 5, Step 3). This is intentional, not a gap.
