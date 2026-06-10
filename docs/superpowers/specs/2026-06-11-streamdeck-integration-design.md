# Stream Deck Integration Design

**Date:** 2026-06-11
**Status:** Approved design, pending implementation plan
**Target:** iPadOS builds of OpenLiveReplay

## Goal

Integrate Elgato's [Stream Deck Kit for iPad](https://github.com/elgatosf/streamdeck-kit-ipad)
so a USB-C-connected Stream Deck acts as a physical replay control surface:
transport actions on keys/dials, live status (REC state, timecode, speed,
scrub position) rendered on the device.

## Requirements (agreed)

- **Scope:** Fixed default layout now, architected so per-key remapping can be
  added later without rework.
- **Hardware:** All Stream Deck models (Mini, MK.2, XL, +, Neo) via one
  adaptive layout definition.
- **Feedback:** Full status surface — state-aware icons plus live text
  (REC elapsed, timecode key, speed key) and a scrub bar on the Stream Deck +
  touch strip.
- **Actions:** Transport set only — the nine actions the MIDI system
  dispatches (play/pause, rewind 5×, forward 5×, frame step ±, go live,
  capture, show multiview, jog) plus a new record-toggle action (id 9),
  since recording has no MIDI action today.
- **Build strategy:** Prebuilt Swift bridge xcframework (see below).

## Constraints

- OpenLiveReplay is a Qt 6 (QML/C++) app built with CMake; the SDK is a
  Swift 5.9+/SwiftUI SPM package, iPadOS 16+, iPad with Apple silicon.
- Requires Elgato's *Stream Deck Connect* app installed with its device driver
  enabled, plus the `com.apple.developer.driverkit.communicates-with-drivers`
  entitlement.
- macOS builds get no Stream Deck support (SDK is iPad-only); the feature must
  compile out cleanly.

## Architecture

```
                    ┌──────────── QML UI (Main.qml) ────────────┐
                    │   settings/status section for Stream Deck │
                    └──────────────────┬────────────────────────┘
                                       │ Q_PROPERTY bindings
   UIManager (C++) ◄── actions 0–8 ── StreamDeckManager (streamdeck/streamdeckmanager.{h,mm})
        │ state signals ──────────────────►│        QObject, ObjC++ on iOS, stub elsewhere
        │                                  │ @objc calls / callbacks
        │                 StreamDeckBridge.xcframework  (ios/streamdeck-bridge/, Swift)
        │                    ├─ OLRStreamDeckBridge   @objc facade: start/stop/setState/events
        │                    ├─ DeckState             granular observable fields per key
        │                    ├─ ReplayDeckLayout      adaptive SwiftUI StreamDeckLayout
        │                    └─ StreamDeckKit         resolved via SPM
        │                                  │ USB-C (Elgato driver)
        └────────────► glass: timecode,   Stream Deck hardware
                       REC state, speed
```

### Components

1. **`StreamDeckBridge.xcframework`** — standalone Xcode project at
   `ios/streamdeck-bridge/`. Owns all Swift: `StreamDeckSession` setup, the
   adaptive SwiftUI layout, state rendering. Public surface is a single
   `@objc` NSObject facade:
   - `start()` / `stop()`
   - `setKeyMapping(_:)` — key-index → action-id table (data, not code)
   - `setState(...)` — plain primitive args, coalesced
   - event handler blocks: `(actionId, pressed)` and `jog(delta)`
   - `isDriverAppInstalled` (via `canOpenURL("elgato-device-driver://")`)
   - `showSimulator()` (DEBUG only)

   No Swift types cross the boundary; C++ consumes the generated ObjC header.

2. **`StreamDeckManager`** — QObject at `streamdeck/streamdeckmanager.{h,mm}`,
   mirroring `midi/midimanager.{h,cpp}`:
   - Q_PROPERTYs: `supported`, `connected`, `deviceName`, `deviceModel`,
     `driverAppInstalled`
   - Dispatches incoming events through the **same action numbering 0–8**
     the MIDI bindings use (one shared action vocabulary)
   - Subscribes to UIManager/PlaybackTransport signals, coalesces/throttles,
     pushes state to the bridge
   - `streamdeck/streamdeckmanager_stub.cpp` compiles on non-iOS platforms:
     same header, `supported == false`, no `#ifdef`s in callers

3. **UIManager wiring** — instantiate StreamDeckManager alongside
   MidiManager; expose to QML as a context property for the settings section.

4. **Build plumbing**
   - `build-scripts/build_streamdeck_bridge.sh`: XcodeGen generates the
     bridge `.xcodeproj` from a committed `project.yml` (SPM pin:
     streamdeck-kit-ipad 1.3.0), then xcodebuild archive → xcframework into
     the gitignored `ios_build/xcframeworks/` (same pattern as
     `build_ffmpeg_ios_srt.sh`). Requires `brew install xcodegen`.
   - CMake: `OLR_ENABLE_STREAMDECK` option, default ON for iOS, forced OFF
     elsewhere; links + embeds the xcframework; fatal configure error naming
     the build script if the framework is missing
   - Entitlements file gains
     `com.apple.developer.driverkit.communicates-with-drivers`
   - `ios/Info.plist` gains `LSApplicationQueriesSchemes: elgato-device-driver`

### Action vocabulary

Integer action ids match the existing MIDI action numbering exactly:

| Id | Action | Trigger semantics |
|----|--------|-------------------|
| 0 | Play/Pause | press |
| 1 | Rewind 5× | hold (active while pressed) |
| 2 | Forward 5× | hold (active while pressed) |
| 3 | Step frame forward | press |
| 4 | Go live | press |
| 5 | Capture snapshot | press |
| 6 | Show multiview | press |
| 7 | Step frame backward | press |
| 8 | Jog | rotation delta, `step(±1)` per detent |
| 9 | Record start/stop | press — **new**, Stream Deck only |
| 20 | Timecode display | none (display-only key) |
| 21 | Speed display | none (display-only key) |

Verified against `uimanager.cpp`: MIDI action 6 dispatches *show multiview*
(`setPlaybackViewState(false, -1)` + `multiviewRequested()`), **not** record —
recording has no MIDI action at all. The Stream Deck REC key therefore uses a
new action id 9 that calls `startRecording()`/`stopRecording()` based on
`isRecording()`. Ids 100–107 (feed select) exist in the MIDI system but are
out of scope per the transport-only decision. Ids 20/21 are display-only
pseudo-actions used in key-mapping tables; they never dispatch.

This is why the event callback carries `pressed: Bool` — hold actions 1/2
need press *and* release, identical to the MIDI implementation.

## Default adaptive layout

One prioritized action list fills whatever the connected device offers
(the SDK reports rows/columns/dials per model):

Priority order: REC (9), Play/Pause (0), Go Live (4), Capture (5),
Step ← (7), Step → (3), Rew 5× (1), Fwd 5× (2), Multiview (6),
Timecode (20), Speed (21).

| Device | Keys | Dials / extras |
|---|---|---|
| MK.2 (15), XL (32), + XL (36 keys, 6 dials) | All 11 in priority order, rest blank | + XL: dial 1 jog as on + |
| Stream Deck + (8 keys, 4 dials) | Top 8 of the same order | Dial 1: rotate = jog, press = play/pause. Touch strip: scrub bar with position + timecode |
| Neo (8 keys) | Top 8 | Two touch points = step ←/→; info bar = timecode |
| Mini (6 keys) | REC, Play/Pause, Go Live, Capture, Step ←, Step → | — |
| Pedal (3 keys, no displays) | Play/Pause, Step ←, Step → | — |

The default profile (key-index → action-id per device shape) is computed on
the C++ side and handed to the bridge via `setKeyMapping`. Future user
remapping = persist that table in `AppSettings` and edit it from QML; no
bridge/Swift changes.

## Data flow

**Inbound (deck → app):** key/dial event → Swift handler → `@objc` event
block → StreamDeckManager marshals onto the Qt thread
(`QMetaObject::invokeMethod`, queued) → same dispatch switch as the MIDI
path → UIManager action. Jog forwards one `step(±1)` per detent; detents are
physically rate-limited, no flood control needed.

**Outbound (app → glass):** UIManager/PlaybackTransport signals →
StreamDeckManager coalesces into a single `setState(...)` call:

- **Booleans** (recording, playing, isLive): pushed immediately,
  event-driven.
- **Timecode / elapsed / scrub position:** derived from the existing 16 ms
  transport tick, **downsampled to ≤10 Hz and only pushed when the rendered
  string actually changes**. No timers on the Swift side; C++ is the single
  clock authority.
- Swift diffs incoming state into granular per-key observable fields, so
  SwiftUI invalidates only dirty keys and the SDK transmits only changed key
  images over USB. Static icons are SF Symbols (no asset catalog needed);
  only text overlays vary.

**Performance contract:** a ticking timecode re-renders one key (plus the
touch strip on the +), never the whole deck.

## Lifecycle & error handling

- `bridge.start()` once at startup (StreamDeckManager ctor, iOS path).
  The new-device handler attaches `ReplayDeckLayout` and immediately pushes a
  full state snapshot, so mid-session hot-plug lights up correctly.
- Unplug/replug is the SDK's responsibility; StreamDeckManager reflects
  `connected`/`deviceName` to QML.
- SDK facts (verified against v1.3.0 source): layouts are plain SwiftUI views
  (the `@StreamDeckView` macro was removed in 1.1.0); session state is
  Combine-based (`@Published`); all callbacks are `@MainActor` on
  `RunLoop.main` — which on iOS is the same thread as Qt's main thread; the
  session auto-stops on `didEnterBackground` and re-fires `newDeviceHandler`
  on foreground, so the re-render + state snapshot happens automatically.
- The Go Live key highlight requires exposing the currently-private
  `m_followLive` as a `followLive` Q_PROPERTY with a change signal on
  UIManager (small, justified addition).
- QML settings section shows three states: *unsupported platform* (hidden),
  *driver missing* (explainer + link to Stream Deck Connect), *ready /
  connected* (device name + model).
- Nothing throws across the language boundary; the facade absorbs errors and
  degrades to "disconnected". State pushes before `start()` or while
  disconnected are dropped silently.

## Testing

- **XCTest** in the bridge project for real logic: state diffing (which keys
  go dirty per state change) and profile → layout adaptation per device
  shape.
- **StreamDeckSimulator** (ships with the SDK): debug-only button in the QML
  settings section presents it over the root view controller — full path
  (Qt → bridge → layout → rendering → key events → Qt) testable with zero
  hardware.
- **Manual hardware checklist:** hot-plug/unplug mid-session, hold actions
  (rew/fwd release), jog direction and feel, timecode update cadence,
  app background/foreground, driver app missing.

No C++ test harness exists in the project; none is introduced for this
feature.

## Approaches considered

1. **Prebuilt Swift bridge xcframework (chosen)** — SPM resolves where it
   works natively (Xcode); CMake links/embeds like the existing FFmpeg
   frameworks; clean ObjC boundary; independently buildable/testable.
2. **CMake-compiled Swift** — single build, but fragile mixed-language target
   inside Qt's iOS toolchain, and StreamDeckKit would still need prebuilding
   since CMake cannot resolve SPM. Rejected.
3. **Xcode project injection** — script-inject the SPM reference into the
   CMake-generated Xcode project. Brittle across regenerations and version
   bumps. Rejected.
