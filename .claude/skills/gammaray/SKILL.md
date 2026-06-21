---
name: gammaray
description: >-
  Use when inspecting or debugging the running OpenLiveReplay desktop app at
  runtime with KDAB GammaRay — live QML scene/Inspector, the QObject tree,
  thread affinity (PlaybackWorker / StreamWorker), signal/slot connections,
  timers, and live properties/methods on the `uiManager` object. Covers building
  GammaRay against the project's Qt, launching and attaching on macOS, and an
  app-specific tool playbook. Not for the headless e2e/soak/unit harnesses or
  the iOS build.
---

# GammaRay — runtime introspection for OpenLiveReplay

[GammaRay](https://github.com/KDAB/GammaRay) (KDAB) injects a probe into a
running Qt application and exposes its internals: the QObject tree, the QML
scene graph with an item picker, live signal/slot connections, a property
editor, timers, meta-objects, and an event monitor. Use it to debug the **live
desktop GUI app** — UI layout, QML bindings, object/thread topology, and the
signal wiring between `UIManager` and the rest of the engine.

**Not for** the headless harnesses under `tests/` (e2e, soak, unit) — they are
non-GUI and short-lived, and GammaRay's value is the live QObject/QML tree of
the interactive app. Not for the iOS build (desktop only).

## The one hard rule: the probe must match the app's Qt

GammaRay injects a probe whose ABI is keyed to the **target's Qt version**. This
app is built against **Qt 6.10.1** (`~/Qt/6.10.1/macos`), so GammaRay must be
built against that same Qt.

> Do **not** `brew install gammaray`. Homebrew's GammaRay is built against
> Homebrew's Qt (a different minor, e.g. 6.11) and pulls in a second full Qt; a
> mismatched probe will not introspect this app's QtQuick scene reliably. If you
> ever bump the project's Qt, **rebuild GammaRay** against the new version.

## Setup

GammaRay must be built **against the app's Qt** (see the hard rule above), so it
is a one-time source build. Full build / install / verify / uninstall steps live
in the setup doc — **[docs/gammaray.md](../../../docs/gammaray.md)**.

After installing, the macOS launcher lives *inside* the app bundle (there is no
`~/.local/bin/gammaray` by default):

```sh
GR=~/.local/GammaRay.app/Contents/MacOS/gammaray
```

Before a session, confirm the installed probe matches the app's Qt:

```sh
"$GR" --list-probes   # expect: qt6_10-arm64 (Qt 6.10 (release, arm64))
```

## Launching against the app

**First, get a launchable, current build.** Build the app per `CLAUDE.md` into a
fresh dir (e.g. `build/c`). The bundle's dynamic-library paths are baked at
configure time, so a bundle configured at a *different* checkout path will fail
to launch with `Library not loaded: @rpath/librtmidi.7.dylib` — that is a stale
bundle, not a GammaRay fault. Sanity-check it runs standalone first:

```sh
APP="$PWD/build/c/OpenLiveReplay.app/Contents/MacOS/OpenLiveReplay"
"$APP" &            # should open the app window
kill %1
```

### Launch mode — recommended on macOS

GammaRay starts the app and injects via `DYLD_INSERT_LIBRARIES` at launch — the
most reliable path on macOS. Opens the GammaRay UI attached to a fresh instance:

```sh
"$GR" "$APP"
```

### Headless / remote (no local GammaRay window)

Inject the probe and start the in-process server only; connect a UI later or
from another machine. Handy over SSH or in CI:

```sh
"$GR" -i preload --inject-only --listen tcp://0.0.0.0:11732 "$APP"
# the app's stderr prints:  GammaRay server listening on: tcp://0.0.0.0:11732
# then connect a UI:        "$GR" --connect 127.0.0.1:11732
#                  or GUI:  File ▸ Connect ▸ tcp://<host>:11732
```

The `GammaRay server listening on:` line means the probe loaded successfully
inside the target. You can confirm it the same way the setup was verified — the
server runs *inside* the app process, so the listening socket is owned by the
`OpenLiveReplay` PID:

```sh
lsof -nP -iTCP:11732 -sTCP:LISTEN   # COMMAND should be OpenLiveR…
```

### Attach to an already-running instance

```sh
"$GR" --pid <pid>   # macOS uses the lldb injector; needs debugging permission
```

Prefer launch mode on macOS — attach can be blocked by hardened runtime or a
missing debug entitlement. A local **Debug** build (unsigned, no hardened
runtime) injects cleanly.

## App-specific playbook

The app exposes a single façade to QML — the **`uiManager`** context property
(`UIManager`, see `main.cpp`) — plus worker `QThread`s. Map the task to a tool:

| Goal | GammaRay tool | Where to look |
|---|---|---|
| Read/poke app state live | **Objects** → Properties / Methods | Select `UIManager` (the `uiManager` context property). Edit properties live, invoke its `Q_INVOKABLE` methods, watch the `*Changed` signals fire. |
| Debug the QML UI | **Quick Scenes / Inspector** | Pick items in `Main.qml` / `MultiviewWindow.qml`; inspect geometry, anchors, the scene graph, overdraw, and **binding loops** (the app currently logs a couple — `ScrollView.contentWidth` at `Main.qml:1340`, `GroupBox.implicitWidth` at `Main.qml:922`). |
| Verify thread affinity (concurrency) | **Objects**, grouped by thread | Confirm `PlaybackWorker` and the per-source `StreamWorker`s sit on their own threads (the playback worker is `moveToThread(self)` + `start(HighPriority)`). `Muxer` uses a raw `std::thread`, so it appears as a non-Qt thread. Catch accidental cross-thread parent/child relationships. |
| Audit signal/slot wiring | **Connections** + signal monitor | The large `UIManager::*Changed → ControlWebSocketServer` web (see `main.cpp`) and `PlaybackTransport` signals (`posChanged` / `playingChanged` / `speedChanged` / `fpsChanged`). Spot missing or duplicate connections; watch live emission rates. |
| Hunt runaway timers / busy loops | **Timers** (Timer Top) | Wakeups/sec per `QTimer` in `UIManager`, `ReplayManager`, `ControlWebSocketServer`. |
| Explore a type's API | **Meta Object Browser** | `PlaybackTransport`, `FrameProvider`, `UIManager`, `StreamDeckManager` — enums, properties, invokables. |
| Trace events to one object | **Event Monitor** | Select a QObject and watch its event stream. |

> The app surfaces state via `uiManager` properties and the WebSocket control
> JSON (port `8115`), **not** via `QAbstractItemModel`s, so GammaRay's *Models*
> tool has little to show for this project.

## Heads-up when launching the real app

Launching it starts real subsystems: it opens the **WebSocket control server on
:8115**, may begin **SRT/NDI ingest** for saved sources, initializes audio
output, and on first run macOS may prompt for **Local Network** permission. The
probe attaches at `QGuiApplication` construction (before networking), so
introspection works regardless. Quit the app when done.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `No probe found for ABI qt6_XX`, or a wrong Qt in `--list-probes` | Probe/Qt mismatch. Rebuild GammaRay against `~/Qt/6.10.1/macos` (see [docs/gammaray.md](../../../docs/gammaray.md)). |
| `Library not loaded: @rpath/librtmidi.7.dylib` → `Injector error: Process crashed` | The app **bundle is stale** (library paths baked at an old checkout path), not a GammaRay fault. Use a build configured at the current repo path, or rebuild per `CLAUDE.md`. |
| `--pid` attach fails on macOS | Use launch mode instead; ensure a local unsigned Debug build. |
| No GammaRay window over SSH / headless | Use `-i preload --inject-only --listen …` and connect a UI via `--connect`. |
