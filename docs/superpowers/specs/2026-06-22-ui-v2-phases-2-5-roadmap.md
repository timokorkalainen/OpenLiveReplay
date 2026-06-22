# UI v2 — Phases 2–5 roadmap (design + engine surface)

Date: 2026-06-22
Status: design (for review) — feeds the per-phase implementation plans
Track: UI 2.0. Phase 0 (OlrStyle/OlrTheme foundation, PR #113) and Phase 1 (the
operator cockpit shell, PR #145) are done. This roadmap covers the remaining
deferred items from the cockpit spec's non-goals and turns each into a phase plan.

> **Dependency:** every phase here builds on the Phase 1 cockpit components in
> `ui/components/` (StatusStrip, PgmStage, TransportDock, ConfigDrawer). These
> plans assume PR #145 has merged.

## Phases at a glance

| Phase | Deliverable | Nature | Depends on | Plan |
|---|---|---|---|---|
| **2** | On-air PGM tally (one source = red, on-air) | **Presentation-only** (+1 tiny engine counter) | #145 | `…-phase2-onair-tally.md` |
| **3** | Playlist model + **Rundown rail** | **Engine (C++ model) + QML** | #145 | `…-phase3-rundown-rail.md` |
| **4** | Rich **ScrubTimeline** (drag scrub + in/out region markers) | QML + small engine accessor | #145, **#3 (entries model)** | `…-phase4-scrub-timeline.md` |
| **5** | Token & a11y polish sweep + transport-deck priority sizing | **Presentation-only** | #145 (+#3/#4 UI to also tokenize) | `…-phase5-token-polish.md` |

**Recommended order:** 2 → 3 → 4 → 5. Phase 2 is a quick high-value win (a real
on-air indicator). Phase 3 builds the playlist surface the Rundown rail and the
ScrubTimeline markers both consume. Phase 4 reuses Phase 3's entries model. Phase 5
runs last so it tokenizes the UI that Phases 2–4 add.

Phases 2 and 5 are presentation-only (safe, fast). Phase 3 is the flagship and the
**only one with substantial engine work**; Phase 4 needs a small engine accessor
(folded into Phase 3's model).

---

## Phase 2 — On-air PGM tally

**Why:** the cockpit's StatusStrip tally currently shows per-source *health*
(green/amber/red by `sourceLinkHealth`) but never an *on-air* indication. A
broadcast operator's single most important glance is "which source is live to
PGM right now" — and the semantic-tally rule reserves `Theme.recordOnAir` (red)
for exactly that.

**Key finding (it's already wired):** the PGM output bus renders
`state.selectedFeedIndex` (`OutputBusEngine::renderPgm` → `renderSingleSource(..,
state.selectedFeedIndex,..)`). That index is the selected single view, exposed to
QML today as `uiManager.playbackSelectedIndex()` + `uiManager.playbackSingleView()`,
with `playbackViewStateChanged()` firing on change. **So "which source is on-air"
is already known to QML.** No PGM-bus engine state is needed.

**Design:**
- A source is **on-air** when `playbackSingleView === true && playbackSelectedIndex
  === sourceIndex`. In multiview (`playbackSingleView === false`) nothing is on a
  single PGM, so no red (the PGM bus is showing the multiview composite, not one
  source).
- StatusStrip tally precedence becomes: `idle` (not recording) → **`recordOnAir`
  (RED) if on-air** → `error` (disconnected / health 3) → `armed` (health 2) →
  `ready` (healthy). The on-air source wins over its own health colour.
- PgmStage tile borders gain the same on-air red on the selected tile (in single
  view the one big tile; in multiview, no on-air red since none is the single PGM).

**Engine touch (tiny, optional-but-recommended):** add a change counter
`Q_PROPERTY(int playbackViewStateVersion …)` bumped wherever
`playbackViewStateChanged()` fires, mirroring the existing `sourceConnectionVersion`
/`sourceStatsVersion` pattern, so the tally `Repeater` re-evaluates cleanly. (Binding
directly to `playbackSelectedIndex()`/`playbackSingleView()` also works; the counter
just matches the established idiom.) ~5 lines of C++; no behaviour change.

---

## Phase 3 — Playlist model + Rundown rail

**Why:** Phase 1's TransportDock has fire-and-forget "Recall 0 / Play Playlist /
Stop Playout" buttons, but the operator can't *see* the rundown — the EVS cue list
that plays itself (PR #112). A Rundown rail surfaces the entries, what's playing,
what's next, and per-entry speed.

**Key finding (entries are opaque to QML — engine work required):** `ReplayEntry
{clipPath, inMs, outMs, speed}` and `ReplayPlaylist`/`PlaylistPlayout` are C++-only.
QML sees only `playlistCount()`, `playlistPlayoutActive()`, and the fire-and-forget
controls. There is **no entries model, no current/next index, no add/remove/setSpeed
accessors** exposed. This phase adds that surface, then the rail.

### Engine surface to add (C++)

1. **`PlaylistEntriesModel : QAbstractListModel`** (new, `playback/playlistentriesmodel.{h,cpp}`)
   wrapping `ReplayPlaylist::entries()`. Roles: `index`, `inMs`, `outMs`,
   `durationMs` (= `outMs<0 ? -1 : outMs-inMs`), `speed`, `clipPath`, `label`
   (derived via `uiManager.sourceDisplayLabel`-style lookup, or clipPath basename).
   Emits `dataChanged`/`rowsInserted`/`rowsRemoved` so QML rebinds.
2. **UIManager expansions** (uimanager.h/.cpp):
   - `Q_PROPERTY(PlaylistEntriesModel* playlistModel READ playlistModel CONSTANT)`
   - `Q_PROPERTY(int currentPlaylistEntryIndex … NOTIFY playlistEntryChanged)`
   - `Q_PROPERTY(int nextPlaylistEntryIndex … NOTIFY playlistEntryChanged)`
   - (`playlistPlayoutActive()` already exists.)
   - `Q_INVOKABLE void removePlaylistEntry(int index)`
   - `Q_INVOKABLE void clearPlaylist()`
   - `Q_INVOKABLE void setPlaylistEntrySpeed(int index, double speed)`
   - (`recallEntry(int)`, `playPlaylist(int)`, `stopPlaylistPlayout()`,
     `markIn()`, `markOut()` already exist and are called from QML.)
   - Wire `onPlayoutTick()` to emit `playlistEntryChanged(current, next)` when the
     playout advances; expose `PlaylistPlayout::currentIndex()`.
   - `ReplayPlaylist` gains `removeEntry(int)`, `clear()`, `setSpeed(int,double)`
     (setSpeed exists) — pure logic, unit-tested.

   **MVP scope (this phase):** list + current/next highlight + per-entry speed
   chips + recall/play/stop + remove + clear + edit-speed. **Deferred to a later
   iteration:** drag-reorder, mid-list insert, in/out re-edit, save/load to disk
   (the `ReplayPlaylist::toJson/fromJson` exists but wiring persistence is its own
   task). Flag this in the plan.

### QML surface

- **`ui/components/RundownRail.qml`** (new) — a collapsible right-side rail beside
  PgmStage (not buried in the ConfigDrawer; the rundown must be glanceable during
  operation). Toggled from a StatusStrip button; collapses to a thin strip showing
  just current/next. A `ListView` over `ui.playlistModel` with delegates showing
  index, label, in→out timecode, duration, a **speed chip** (`Theme.armed` when
  ≠1.0), and **current/next highlight** (`Theme.recordOnAir` edge on current,
  `Theme.armed` on next). Per-row: Recall, Remove. Footer: Play / Stop / Clear.
- Shell: instantiate `RundownRail` in Main.qml's center row (right of PgmStage,
  `Layout.fillHeight`); add the toggle to StatusStrip.

---

## Phase 4 — Rich ScrubTimeline

**Why:** Phase 1's TransportDock uses a plain `Slider` for scrub. A broadcast
timeline shows the **in/out region(s)**, the playhead, the live edge, and supports
click/drag to seek across `[0, recordedDurationMs − liveBufferMs]`.

**Key finding:** scrub/seek is complete (`scrubPosition`, `seekPlayback(ms)`,
`endScrubGesture()`, `recordedDurationMs`, `liveBufferMs`, `recordTimecode(ms)`).
In/out **points become readable via Phase 3's `playlistModel`** (each entry's
`inMs`/`outMs`). So the timeline's region markers reuse Phase 3 — **no extra engine
beyond Phase 3.** A general **cue-flag/bookmark system** (arbitrary labelled point
markers distinct from playlist in/out) would need new engine state and is **deferred
to a future phase** (noted, not built here).

**Design:**
- **`ui/components/ScrubTimeline.qml`** (new) replaces the scrub `Slider` inside
  TransportDock. A track from 0 to `recordedDurationMs − liveBufferMs`:
  - playhead indicator at `scrubPosition`; accent fill behind it.
  - **in/out region overlays** from `ui.playlistModel` (a `Repeater` shading
    `inMs→outMs` spans; open out-points `outMs<0` render as an in-flag only).
  - live-edge marker at the right.
  - click/drag → `seekPlayback(posForX)`, release → `endScrubGesture()`
    (preserving the current gesture semantics).
  - hover → a timecode tooltip via `recordTimecode`.
- TransportDock swaps its `Slider` for `ScrubTimeline { ui: root.ui }`, keeping the
  same seek/endScrubGesture wiring.

---

## Phase 5 — Token & a11y polish sweep + transport-deck priority

**Why:** the Phase 1 extraction moved ~68 inline hex literals verbatim into the
components (the deferred token sweep), and the transport keys are uniform-sized
(the "deck" should prioritise PLAY/GO-LIVE). Presentation-only.

**Design:**
- **Token sweep:** replace the 68 inline-hex occurrences across `ui/components/*.qml`
  + `Main.qml` with `Theme.*` tokens. Mapping (the common ones):
  `#d32f2f`→`Theme.recordOnAir`, `#4CAF50`/`#2e7d32`/`#8bc34a`→`Theme.ready`,
  `#ff9800`/`#ffb300`/`#f9a825`/`#ffcc80`→`Theme.armed`, `#eeeeee`/`#cccccc`→
  `Theme.textHi`, `#b0b0b0`/`#aaaaaa`/`#999999`→`Theme.textBody`/`Theme.textDim`,
  `#1f1f1f`/`#1b1b1b`/`#181818`→`Theme.panel`/`Theme.panelRaised`,
  `#333`/`#555`/`#666`/`#777`→`Theme.line`/`Theme.lineStrong`. Add a token only if
  none fits (avoid inventing). The warning banner's `#2b2615`/`#5b4818` → add
  `Theme.warnSurface`/`Theme.warnBorder` tokens (a genuine new semantic).
- **a11y/contrast pass:** verify text tokens meet ≥4.5:1 on their surfaces (the
  Theme comments already claim this — confirm with rendered probes), focus rings
  visible, hit targets ≥ `Theme.hCompact`.
- **Transport-deck priority sizing:** in TransportDock, size PLAY/PAUSE to
  `Theme.hPrimary` and GO-LIVE to `Theme.hTransport`, group the shuttle speeds
  visually, so the deck reads hierarchically (the tokens already exist:
  `hPrimary 48 / hTransport 44 / hAction 40`).

---

## Cross-cutting conventions (all phases)

- **`ui` injection:** new components take `property var ui` (set by the shell to
  `appWindow.uiManagerRef`); reference `ui.*` only — no bare `uiManager` (the
  `--unqualified error` lint gate). This is the Phase 1 convention.
- **Verification gates (per task):** build (qmlcachegen compiles QML) → `qml_smoke`
  (strict qmllint; add new components to its file list + the CI Lint list) →
  `uipreview` render to a PNG under `build/` (never `/tmp`) → offscreen app-load
  (style resolves, no QML errors) → the unit/smoke suite. For **engine** tasks
  (Phase 3): TDD the pure-logic `ReplayPlaylist` additions + the model with unit
  tests under `tests/unit/`, run under the existing label.
- **Worktree + push:** work in `.claude/worktrees/<branch>`; push with the pre-push
  hook (never `--no-verify`); docs-only pushes auto-skip the heavy gates.
- **Engine honesty:** Phases 3 (and the tiny Phase 2 counter) touch C++ — these
  are **not** presentation-only; the plans include the C++ tasks + unit tests +
  the concurrency note (the playout monitor runs on a timer; `onPlayoutTick` emits
  must stay on the UIManager thread).

## Open design decisions (please confirm in review)

1. **Rundown rail placement** — recommended a *persistent collapsible right rail*
   (glanceable). Alternative: a section in the ConfigDrawer (less prominent). The
   plan assumes the rail.
2. **Phase 3 MVP scope** — list + current/next + speed + recall/play/stop +
   remove/clear/edit-speed; **deferring** drag-reorder, insert, in/out re-edit, and
   disk persistence. Confirm that split.
3. **Phase 4 cue-flags** — deferring a general labelled-marker system (engine work);
   Phase 4 ships in/out region markers from the playlist model only. Confirm.
4. **Ordering** — 2 → 3 → 4 → 5. Phase 2 could ship immediately after #145.
