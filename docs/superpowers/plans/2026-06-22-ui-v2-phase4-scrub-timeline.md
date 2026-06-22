# UI v2 Phase 4 — Rich ScrubTimeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace the plain scrub `Slider` in TransportDock with a broadcast-style **ScrubTimeline**: a track over `[0, recordedDurationMs − liveBufferMs]` showing the playhead, the live edge, and **in/out region markers** from the playlist, with click/drag to seek.

**Architecture:** Pure QML widget reusing existing seek/scrub API (`scrubPosition`, `seekPlayback`, `endScrubGesture`, `recordedDurationMs`, `liveBufferMs`, `recordTimecode`) and **Phase 3's `ui.playlistModel`** for the in/out region overlays — so **no new engine work** beyond Phase 3. A general labelled cue-flag system is explicitly **deferred** (would need new engine state). Builds on Phase 1 (#145) and **depends on Phase 3** (the entries model).

**Tech Stack:** Qt 6.10.1, QML, CMake/CTest. See roadmap (Phase 4).

## Global Constraints

- **Depends on Phase 3** (`ui.playlistModel` with roles `inMs/outMs/durationMs/speed/...`). If Phase 3 isn't merged, the region-markers step degrades gracefully (model absent → no overlays) but the timeline + scrub still works.
- **Preserve the exact scrub gesture semantics** of the current Slider: dragging calls `seekPlayback(ms)`; release calls `endScrubGesture()`; the displayed position is the live `scrubPosition` except while actively dragging.
- **No general cue-flag/bookmark system** here (deferred — needs engine state).
- Component uses injected `property var ui`; `ui.*` only. Verify each task: build → `qml_smoke` → `uipreview` render → app-load → suite. Outputs under `build/`. Never push `--no-verify`.

---

### Task 1: `ScrubTimeline.qml` — track, playhead, seek

**Files:**
- Create: `ui/components/ScrubTimeline.qml`
- Modify: `CMakeLists.txt` (QML_FILES), `tests/smoke/CMakeLists.txt` + `.github/workflows/ci.yml` (lint lists)

**Interfaces:**
- Produces: `ScrubTimeline { property var ui }` — root `Item` with `implicitHeight ≈ Theme.hControl`. Internally maps position↔x over `[0, durMax]` where `durMax = Math.max(0, ui.recordedDurationMs - ui.liveBufferMs)`.
- Consumes: `ui.scrubPosition`, `ui.recordedDurationMs`, `ui.liveBufferMs`, `ui.seekPlayback(ms)`, `ui.endScrubGesture()`, `ui.recordTimecode(ms)`.

- [ ] **Step 1: Create the file.** Header (`pragma ComponentBehavior: Bound` + QtQuick/Controls/Layouts/OlrTheme). Skeleton:
```qml
Item {
    id: root
    property var ui
    readonly property bool hasUi: ui !== null && ui !== undefined
    readonly property real durMax: hasUi ? Math.max(0, ui.recordedDurationMs - ui.liveBufferMs) : 0
    property bool dragging: false
    property real dragMs: 0
    implicitHeight: Theme.hControl
    function msToX(ms) { return durMax > 0 ? (ms / durMax) * track.width : 0 }
    function xToMs(x)  { return durMax > 0 ? Math.max(0, Math.min(durMax, (x / track.width) * durMax)) : 0 }
    readonly property real shownMs: dragging ? dragMs : (hasUi ? ui.scrubPosition : 0)

    Rectangle {                      // the track
        id: track
        anchors.fill: parent
        anchors.topMargin: Theme.s2; anchors.bottomMargin: Theme.s2
        radius: Theme.r1
        color: Theme.panelPressed
        Rectangle {                  // accent fill up to the playhead
            width: root.msToX(root.shownMs); height: parent.height
            color: Theme.accent; opacity: 0.35; radius: Theme.r1
        }
        Rectangle {                  // live-edge marker (right end)
            x: parent.width - width; width: 2; height: parent.height; color: Theme.recordOnAir; opacity: 0.6
        }
        Rectangle {                  // playhead
            x: Math.max(0, root.msToX(root.shownMs) - width / 2)
            width: 2; height: parent.height; color: Theme.textHi
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onPressed: (m) => { root.dragging = true; root.dragMs = root.xToMs(m.x); if (root.hasUi) root.ui.seekPlayback(root.dragMs) }
            onPositionChanged: (m) => { if (root.dragging) { root.dragMs = root.xToMs(m.x); if (root.hasUi) root.ui.seekPlayback(root.dragMs) } }
            onReleased: { root.dragging = false; if (root.hasUi) root.ui.endScrubGesture() }
            ToolTip.visible: containsMouse
            ToolTip.text: root.hasUi ? root.ui.recordTimecode(root.xToMs(mouseX)) : ""
        }
    }
}
```

- [ ] **Step 2: Register** in CMake QML_FILES + qml_smoke list + ci.yml Lint list.

- [ ] **Step 3: Build + lint.** Build clean; `qml_smoke` Passed.

- [ ] **Step 4: Render.** `tests/qmlstyle/preview_scrub.qml`: a stub `ui` with `recordedDurationMs:60000`, `liveBufferMs:1000`, `scrubPosition:18000`, stub `recordTimecode`. Render 900×60 → inspect: track with accent fill to ~30%, a playhead line, a live-edge marker.

- [ ] **Step 5: Commit.** `feat(ui): ScrubTimeline component (track + playhead + click/drag seek)`.

### Task 2: In/out region markers from the playlist model

**Files:**
- Modify: `ui/components/ScrubTimeline.qml`

**Interfaces:**
- Consumes: `ui.playlistModel` (Phase 3; roles `inMs`/`outMs`).

- [ ] **Step 1: Overlay the regions.** Inside `track`, add a `Repeater { model: root.hasUi ? root.ui.playlistModel : null; ... }` whose delegate shades each entry's in→out span:
```qml
Repeater {
    model: root.hasUi ? root.ui.playlistModel : 0
    delegate: Item {
        required property var model       // model.inMs / model.outMs
        Rectangle {                       // in→out region (or just an in-flag if open)
            x: root.msToX(model.inMs)
            width: model.outMs < 0 ? 2 : Math.max(2, root.msToX(model.outMs) - root.msToX(model.inMs))
            height: track.height
            color: model.outMs < 0 ? Theme.armed : Theme.accent
            opacity: model.outMs < 0 ? 1.0 : 0.25
        }
    }
}
```
(Open out-points render as a thin `armed` in-flag; closed regions as a translucent `accent` band. Guard `durMax > 0`.)

- [ ] **Step 2: Build + lint.** Build clean; `qml_smoke` Passed.

- [ ] **Step 3: Render with regions.** Extend `preview_scrub.qml`'s stub `ui` with a `playlistModel` inline `ListModel` (rows: `{inMs:5000,outMs:12000}`, `{inMs:20000,outMs:-1}`). Render → inspect: a translucent band 5–12s, an amber in-flag at 20s.

- [ ] **Step 4: Commit.** `feat(ui): ScrubTimeline in/out region markers from the playlist model`.

### Task 3: Swap into TransportDock

**Files:**
- Modify: `ui/components/TransportDock.qml` (replace the scrub `Slider` with `ScrubTimeline`)

**Interfaces:**
- TransportDock already injects `ui`; pass it through.

- [ ] **Step 1: Replace the Slider.** Swap the scrub `Slider` (and its `from/to/value/onPressedChanged` wiring) for:
```qml
ScrubTimeline { Layout.fillWidth: true; ui: root.ui }
```
Remove the now-dead Slider seek wiring (the timeline owns it). Keep the timecode readouts.

- [ ] **Step 2: Build + lint + render the dock.** Build clean; `qml_smoke` Passed; render `preview_transport.qml` (Phase 1) → inspect the timeline in place of the slider.

- [ ] **Step 3: App-load (integration).** Offscreen app-load: style resolves, no QML errors; scrubbing wiring intact (the timeline calls the same seek API).

- [ ] **Step 4: Suite.** `ctest --test-dir build/c -L 'unit|smoke'` 100%.

- [ ] **Step 5: Commit.** `feat(ui): use ScrubTimeline in TransportDock (replaces the scrub slider)`.

## Self-Review
- Roadmap Phase 4 coverage: track + playhead + click/drag seek (Task 1) + in/out region markers reusing the Phase 3 model (Task 2) + swap into the dock (Task 3). ✓
- No new engine work (reuses Phase 3 + existing seek API). The cue-flag system is explicitly deferred. ✓
- Scrub gesture semantics preserved (`seekPlayback` on drag, `endScrubGesture` on release, `scrubPosition` when idle). ✓
- Degrades gracefully if Phase 3's `playlistModel` is absent (`model: 0` → no overlays). ✓
- `msToX`/`xToMs`/`durMax`/`shownMs` names consistent across tasks. No bare `uiManager`. ✓
