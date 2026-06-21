# Operator Cockpit shell — design

Date: 2026-06-21
Status: approved (brainstorm)
Track: UI 2.0, Phase 1 (the structural shell; follows the merged Phase 0 foundation — OlrStyle/OlrTheme, PR #113)

## Problem

The app's live-operator surface is one of three tabs. PGM video, the transport
controls, and tally live only inside the **Playback** tab; switching to **Control**
or **Project** hides the entire operator interface. On a short window the transport
row scrolls off the bottom of the Playback tab and becomes unreachable. The whole UI
is a single 2,268-line `Main.qml` with no extracted components, so it is hard to make
responsive and hard to reason about.

This is not a broadcast-console layout: an operator cannot keep eyes on PGM + tally
and a hand on the transport while doing anything else.

## Goals

- A **persistent operator deck**: PGM/multiview, transport, and tally are always on
  screen and never clip, regardless of window size or what else is open.
- **Config one click away**: sources, NDI outputs, project settings, and device
  bindings open over the deck without losing PGM/transport, and close just as fast.
- **Glanceable source health**: the operator re-arms dropped sources and tweaks trim
  mid-show, so per-source tally/health is always visible.
- **Multiview inline or detached**: the stage shows the multiview grid inline, or the
  multiview detaches to a second screen (existing `MultiviewWindow`) and the stage
  becomes the large PGM/selected angle.
- **Reflows, never clips** down to small windows (the original complaint).

These goals come from the brainstorm: config is *adjusted occasionally* live (not set-
and-forget, not constantly co-visible), and the deployment is *both* inline and
detached-multiview.

## Non-goals (this phase)

Presentation-only restructure. Explicitly out of scope:

- No engine, `UIManager`, or `PlaybackTransport` changes. Every region binds to the
  same existing API.
- The rich **ScrubTimeline** (draggable in/out handles, cue-flag markers) — Phase 3.
- The **Rundown rail** that surfaces the automatic playlist playout (PR #112) — Phase 3.
- A real single-red-PGM tally (needs PGM-bus state) — later.
- A full inline-hex → token sweep. Tokens are applied opportunistically as each region
  is extracted; the ~40 remaining inline hex literals are not chased globally here.

## Architecture — the cockpit shell

`Main.qml` stops being a `TabBar + StackLayout(3 tabs)` and becomes a thin shell that
composes four persistent regions plus an overlay drawer. Vertical stack, top to bottom:

```
+----------------------------------------------------------+
| StatusStrip   (persistent, fixed height ~hControl)       |
|   (*)REC 00:12:44   01:12:44:18   CAM1 CAM2 CAM3  [Cfg]   |
+----------------------------------------------------------+
|                                                          |
|   PgmStage    (the only region that flexes; fillHeight)  |
|     single PGM / multiview grid; detachable to screen 2  |
|                                                          |
+----------------------------------------------------------+
| TransportDock (persistent, pinned bottom — never scrolls)|
|   <=========O==============>  in | out                   |
|   <<  <<  <  [ ||/PLAY ]  >  >>  >>   GO LIVE   grab      |
+----------------------------------------------------------+
                                   [Cfg] -> ConfigDrawer slides
                                   over the stage; PGM keeps playing
```

### Regions

- **StatusStrip** (top, persistent). One glanceable row: record arm + START/STOP +
  REC elapsed; the master clock/timecode (click toggles time-of-day); a compact
  per-source **tally row** (CAM chips colored by health — `idle`/`ready`/`armed`/
  `error`); the `Fullscreen Multiview` control; and the `[Config]` toggle. Record moves
  here from the old Control tab because arm/start/stop is an operator action, not config.

- **PgmStage** (center, the only flexible region). The existing single ⇄ multiview
  display and its toggle, tally borders on tiles. Detaches to `MultiviewWindow` on a
  second screen; when detached the stage shows the large PGM/selected angle. Shrinks
  rather than pushing other regions off-screen.

- **TransportDock** (bottom, persistent, **pinned**). The scrub timeline with in/out
  markers, then the transport key row: PLAY/PAUSE (primary), frame step, shuttle speeds,
  GO LIVE, frame grab, mark in/out. Fixed to the window bottom — the core fix, since
  transport can no longer scroll out of reach.

- **ConfigDrawer** (overlay, on demand). A slide-over panel over a scrim; the deck keeps
  rendering behind it. Esc or scrim-click closes. Hosts the reorganized config.

### Component inventory (extracted from `Main.qml`)

The shell requires breaking the monolith into focused, independently testable files.
New components live in a `ui/components/` subdirectory, registered in the **existing
`OpenLiveReplay` app QML module** (alongside `Main.qml`/`MultiviewWindow.qml`/
`CodecSettingsPanel.qml`) — so they are already on the QML import path with no new
CMake module to wire:

| Component | Responsibility | Source today |
|---|---|---|
| `StatusStrip.qml` | record/clock/tally row + Config toggle | Control tab header + Playback timecode |
| `PgmStage.qml` | single/multi video display + toggle + tally tiles | Playback `playbackView` (Main.qml video region) |
| `TransportDock.qml` | scrub + transport keys + timecode readout | Playback transport + scrub + mark in/out |
| `ConfigDrawer.qml` | the slide-over container + section switcher | new |
| `SourceListPanel.qml` | source rows: enable/trim/id/name/metadata/url/delete | Project input-sources list |
| `OutputsPanel.qml` | the NDI Outputs table | Project NDI outputs |
| `ProjectSettingsPanel.qml` | name/save-location/res/fps/audio-latency/metadata | Project settings + codec panel host |
| `BindingsPanel.qml` | MIDI + Stream Deck mapping | Control tab MIDI/StreamDeck |

`CodecSettingsPanel.qml` and `MultiviewWindow.qml` already exist and are reused.
`PgmStage` and `MultiviewWindow` share the grid/tile logic where practical (extract a
small `ViewTile`/grid helper if it falls out cleanly; not forced).

Config panels are **moved, not rewritten** — same controls and `uiManager` bindings,
relocated into focused files. `Main.qml` shrinks to a shell that instantiates the four
regions + the drawer and exposes `uiManager`.

## The config drawer

- Slides in from the right, ~`bpSM` wide, with a `scrim` over the stage. Semi-modal:
  PGM/transport stay live and visible; Esc or scrim-click dismisses. Below `bpSM` it
  takes full width.
- Internally a vertical section switcher (Sources / Outputs / Project / Bindings), **each
  section its own scroll panel** — replacing today's single giant Project scroll.
- Optimized for the live-adjust workflow: re-arm a source, change a trim offset, or check
  NDI output health without leaving PGM.

## Responsive behavior

The original "doesn't fit small windows / no scrollbars / too large" complaint is solved
structurally: **StatusStrip and TransportDock are fixed-height and always pinned; only
PgmStage flexes**, so the operator essentials never clip — the stage shrinks instead.

Using `Theme.bp*` breakpoints (already defined):
- `< bpMD`: tally chips condense to dots; shuttle-speed keys collapse into a compact
  group; button labels shorten.
- `< bpSM`: StatusStrip wraps (clock/tally onto a second line); drawer goes full-width.
- `windowMinH` can drop from its current value, since the stage shrinks rather than the
  transport being pushed off — re-tuned during implementation against rendered mockups.

## Data flow

Unchanged. Every component binds to the existing `uiManager` context property and
`PlaybackTransport`. No new C++, no new signals. The restructure is pure QML composition;
the engine and the WebSocket control API are untouched.

## Testing

- **Component-load smoke**: each extracted component instantiates under OlrStyle with no
  QML errors (extends the `tst_olrstyle` offscreen approach in `tests/qmlstyle/`).
- **Visual review**: `uipreview` renders the assembled cockpit and each breakpoint
  (`bpSM`/`bpMD`/`bpLG`) to PNG for inspection — including the drawer open/closed and the
  stage in single vs multi.
- **Lint**: `qml_smoke` + the Lint job cover the new files (`--unqualified`/
  `--Quick.layout-positioning` as errors); the new module is added to qmllint's import
  path the same way `OlrTheme`/`OlrStyle` are.
- **No engine regressions**: the unit suite stays green (the change touches no C++).

## Success criteria

1. PGM, transport, and tally are visible and operable at the same time, in every window
   state, including at the (re-tuned) minimum size — verified by rendered mockups.
2. Switching to any config does not hide PGM or transport; the drawer opens/closes over a
   live deck.
3. `Main.qml` is a thin composition shell; each region is a focused file that loads and
   lints clean on its own.
4. No engine/`uiManager` change; unit suite + style/lint gates green.

## Risks / open questions (resolve at plan time)

- **Drawer vs. inline config trade-off** if `< bpSM` and the drawer is full-width — the
  deck is hidden while configuring on a tiny window. Acceptable (rare), but confirm the
  Esc/close affordance is obvious.
- **Grid sharing** between `PgmStage` and `MultiviewWindow`: extract a shared tile/grid
  only if it is clean; otherwise leave the small duplication rather than over-couple.
- **Keyboard shortcut** for the Config toggle and transport keys — define the minimal set
  now; full shortcut map is a later concern.
