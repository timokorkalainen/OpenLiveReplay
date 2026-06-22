# UI v2 Phase 2 — On-air PGM tally Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Show the operator which source is **on-air** — exactly one source renders in `Theme.recordOnAir` (red) when it is the selected single PGM view — in the StatusStrip tally and the PgmStage tile border.

**Architecture:** The PGM bus already renders `selectedFeedIndex` (`OutputBusEngine::renderPgm`), exposed to QML as `uiManager.playbackSelectedIndex()` + `uiManager.playbackSingleView()`. So this is presentation-only: derive on-air from those, with a small `playbackViewStateVersion` counter (mirroring `sourceConnectionVersion`) so the tally `Repeater` rebinds. Builds on the Phase 1 cockpit (PR #145).

**Tech Stack:** Qt 6.10.1, QML, C++ (UIManager), CMake/CTest. See roadmap `docs/superpowers/specs/2026-06-22-ui-v2-phases-2-5-roadmap.md` (Phase 2).

## Global Constraints

- Build on the merged Phase 1 cockpit (`ui/components/StatusStrip.qml`, `PgmStage.qml`). Branch off `origin/main` after #145 lands.
- Components use the injected `property var ui`; reference `ui.*` only (lint `--unqualified error`).
- The on-air red (`Theme.recordOnAir`) is reserved for the single on-air source; never colour a healthy-but-not-on-air source red.
- Verify each task: build → `qml_smoke` → `uipreview` render → offscreen app-load → unit suite. Outputs under `build/`, never `/tmp`. Never push `--no-verify`.

---

### Task 1: Engine — `playbackViewStateVersion` change counter

**Files:**
- Modify: `uimanager.h` (property + getter + member), `uimanager.cpp` (bump where `playbackViewStateChanged()` is emitted)
- Test: `tests/unit/` (extend an existing UIManager test or add a focused one if a harness exists; otherwise this is covered by the QML binding + app-load)

**Interfaces:**
- Produces: `Q_PROPERTY(int playbackViewStateVersion READ playbackViewStateVersion NOTIFY playbackViewStateVersionChanged)` — increments on every playback view-state change (single/multi or selected index).

- [ ] **Step 1: Add the property.** In `uimanager.h`, beside the existing `sourceConnectionVersion`/`sourceStatsVersion` properties, add:
```cpp
Q_PROPERTY(int playbackViewStateVersion READ playbackViewStateVersion NOTIFY playbackViewStateVersionChanged)
```
add getter `int playbackViewStateVersion() const { return m_playbackViewStateVersion; }`, member `int m_playbackViewStateVersion = 0;`, and signal `void playbackViewStateVersionChanged();`.

- [ ] **Step 2: Bump it.** In `uimanager.cpp`, find every site that emits `playbackViewStateChanged()` (notably `setPlaybackViewState(...)`). Immediately after each emit, add:
```cpp
++m_playbackViewStateVersion;
emit playbackViewStateVersionChanged();
```

- [ ] **Step 3: Build + suite.** `cmake --build build/c` (expected: clean), then `ctest --test-dir build/c -L unit --output-on-failure` (expected: 100% — no behaviour change, just a counter).

- [ ] **Step 4: Commit.** `git commit -m "feat(ui): add playbackViewStateVersion counter for on-air tally rebinding"` (+ Co-Authored-By trailer).

### Task 2: StatusStrip — on-air red tally precedence

**Files:**
- Modify: `ui/components/StatusStrip.qml` (the tally chip colour function/binding)

**Interfaces:**
- Consumes: `ui.playbackSingleView`, `ui.playbackSelectedIndex`, `ui.playbackViewStateVersion`, plus the existing `ui.isRecording`, `ui.isSourceConnected(i)`, `ui.sourceLinkHealth(i)`, `ui.sourceConnectionVersion`, `ui.sourceStatsVersion`.

- [ ] **Step 1: Replace the tally colour logic.** In StatusStrip's per-source chip, define the colour as a function that references the version counters (so it re-evaluates) and applies on-air precedence:
```qml
function tallyColor(index) {
    if (!root.hasUi || !root.ui.isRecording) return Theme.idle
    // touch version counters so the binding re-evaluates on change:
    var _v = root.ui.playbackViewStateVersion + root.ui.sourceConnectionVersion + root.ui.sourceStatsVersion
    if (root.ui.playbackSingleView && root.ui.playbackSelectedIndex === index)
        return Theme.recordOnAir           // ON-AIR — the one red
    if (!root.ui.isSourceConnected(index)) return Theme.error
    var h = root.ui.sourceLinkHealth(index)
    if (h === 3) return Theme.error
    if (h === 2) return Theme.armed
    return Theme.ready
}
```
Bind each chip's `border.color` (and any fill) to `root.tallyColor(index)`.

- [ ] **Step 2: Build + lint.** `cmake --build build/c` (expected clean); `ctest --test-dir build/c -R '^qml_smoke$' --output-on-failure` (expected Passed).

- [ ] **Step 3: Render with an on-air mock.** Create `tests/qmlstyle/preview_onair.qml`: a dark Item hosting `StatusStrip` with a stub `ui` QtObject providing `isRecording:true`, `playbackSingleView:true`, `playbackSelectedIndex:1`, `multiviewCount:4`, `playbackViewStateVersion:0`, `sourceConnectionVersion:0`, `sourceStatsVersion:0`, and `isSourceConnected`/`sourceLinkHealth` stub functions. Run `uipreview` → inspect: chip index 1 is **red** (on-air), others green/idle.

- [ ] **Step 4: App-load.** Offscreen app-load (style resolves, no QML errors).

- [ ] **Step 5: Commit.** `feat(ui): on-air red tally in StatusStrip`.

### Task 3: PgmStage — on-air border on the selected PGM tile

**Files:**
- Modify: `ui/components/PgmStage.qml` (single-view tile border; multiview tile borders stay health-only)

**Interfaces:**
- Consumes: same on-air signals; `selectedIndex`/`viewMode` already exist on PgmStage.

- [ ] **Step 1: On-air border.** In the **single-view** tile, set the border to `Theme.recordOnAir` (the single view IS the PGM source). In the **multiview** grid, leave tile borders as today's health colour (no single PGM in multiview — matches the PGM bus rendering the composite). If a future design wants a multiview on-air highlight, that requires the bus to expose a per-tile PGM concept (out of scope).

- [ ] **Step 2: Build + lint + render.** Build clean; `qml_smoke` Passed; render `tests/qmlstyle/preview_stage.qml` (from Phase 1) in single-view with a stub on-air `ui` → inspect the red border.

- [ ] **Step 3: App-load + suite.** Offscreen app-load clean; `ctest --test-dir build/c -L 'unit|smoke'` 100%.

- [ ] **Step 4: Commit.** `feat(ui): on-air border on the PGM single-view tile`.

## Self-Review
- Roadmap Phase 2 coverage: on-air red in StatusStrip (Task 2) + PgmStage (Task 3); the rebinding counter (Task 1). ✓
- No bare `uiManager` (all `ui.*`). ✓
- Engine change is additive (a counter); unit suite must stay green (Task 1 Step 3). ✓
- Multiview correctly shows no single on-air red (matches the PGM bus). Documented. ✓
