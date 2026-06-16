# WebSocket Control API Design

**Date:** 2026-06-16
**Status:** Draft, awaiting user review
**Target:** Desktop and iOS builds of OpenLiveReplay

## Goal

Expose OpenLiveReplay's local control and state surface over an unauthenticated
WebSocket API so local-network clients, including a future PC StreamDeck
plugin, can control the app and subscribe to live state, events, telemetry, and
timecode updates.

The API must cover the behavior currently reachable from the QML UI, MIDI
controls, and the iOS StreamDeck bridge. The WebSocket layer must not introduce
a second control model; it should adapt JSON messages to the existing
`UIManager`, `PlaybackTransport`, MIDI action vocabulary, and StreamDeck action
vocabulary.

## Requirements

- Start automatically with the app and listen on `0.0.0.0:8115`.
- Use no authentication or authorization.
- Keep the app usable if the server cannot bind; log the failure and continue.
- Send every new client a full state snapshot immediately after connection.
- Publish state changes, lifecycle events, source connection changes, telemetry
  changes, and timecode/playhead updates.
- Throttle timecode updates to at most 10 Hz while preserving immediate updates
  for important state transitions.
- Accept semantic commands for UI-like clients and action-id commands for
  StreamDeck-style clients.
- Reuse the existing action ids documented by `streamdeck/streamdeckmanager.h`
  and dispatched by `UIManager::dispatchControlAction`.
- Keep message payloads plain JSON objects with optional client-supplied request
  ids for command acknowledgements.

## Non-Goals

- No remote video/audio streaming over this socket.
- No authentication, pairing, TLS, or network ACLs in this iteration.
- No browser UI or separate web dashboard.
- No replacement for the provider OpenAPI/SSE integration documented under
  `docs/openapi/`; this API controls OpenLiveReplay itself.
- No persistent external-client profiles yet. PC StreamDeck plugin settings can
  live on the client side for now.

## Architecture

Add a small `websocket/` service, tentatively `ControlWebSocketServer`, built on
Qt's WebSocket support and owned by the app lifetime. It can be constructed in
`main.cpp` beside `UIManager`, or owned by `UIManager` if implementation shows
that signal wiring is cleaner there. In both cases the service receives a
pointer or reference to `UIManager` and reaches `PlaybackTransport` through
`UIManager::transport()`.

The server is a thin adapter:

```
Local client
  ⇄ ws://<host>:8115/api/ws
ControlWebSocketServer
  ⇄ UIManager / PlaybackTransport signals and invokables
ReplayManager, MIDI, StreamDeck, playback worker, telemetry client
```

The service owns:

- Listening socket and client connection lifecycle.
- JSON parse/validation and command acknowledgements.
- Snapshot and patch serialization.
- Timecode coalescing/throttling.
- Mapping WebSocket commands onto existing app methods.

The service does not own:

- Recording behavior.
- Playback scheduling.
- Stream Deck layout state.
- MIDI device IO.
- Settings persistence beyond calling existing `UIManager` methods.

This keeps QML, MIDI, iOS StreamDeck, and WebSocket clients on one app behavior
surface.

## Endpoint

- URL: `ws://<host>:8115/api/ws`
- Bind address: `QHostAddress::Any`
- Authentication: none
- Message format: one JSON object per WebSocket text message
- Binary messages: ignored or rejected with `unsupported_message`

Qt WebSocket availability should be added explicitly to CMake with the relevant
Qt component. If the current Qt package does not provide it in a target
environment, the build should fail clearly for that target rather than silently
omitting the API.

## Message Model

### Client Command

```json
{
  "type": "command",
  "id": "abc-1",
  "name": "transport.playPause",
  "args": {}
}
```

`id` is optional. If supplied, acknowledgements and errors echo it.

### Acknowledgement

```json
{
  "type": "ack",
  "id": "abc-1",
  "ok": true
}
```

Failed command:

```json
{
  "type": "ack",
  "id": "abc-1",
  "ok": false,
  "code": "invalid_args",
  "message": "sources.updateUrl requires integer args.index and string args.url"
}
```

### Initial Snapshot

```json
{
  "type": "state.snapshot",
  "state": {}
}
```

### State Patch

```json
{
  "type": "state.patch",
  "path": "transport",
  "value": {}
}
```

Patches replace the object at `path`. They are not RFC 6902 JSON Patch
operations.

### Event

```json
{
  "type": "event",
  "name": "recording.started",
  "data": {}
}
```

### Timecode

```json
{
  "type": "timecode",
  "positionMs": 12345,
  "durationMs": 67000,
  "text": "00:00:12:10",
  "followLive": false,
  "playing": true,
  "speed": 1.0
}
```

## State Snapshot

The snapshot should include externally useful state, not QObject pointers or
video sinks.

Proposed top-level shape:

```json
{
  "recording": {
    "active": false,
    "durationMs": 0,
    "startEpochMs": 0
  },
  "transport": {
    "positionMs": 0,
    "scrubPositionMs": 0,
    "durationMs": 0,
    "timecode": "00:00:00:00",
    "playing": false,
    "speed": 1.0,
    "fps": 30,
    "followLive": false,
    "liveBufferMs": 1000
  },
  "sources": [
    {
      "index": 0,
      "id": "1",
      "name": "",
      "url": "",
      "enabled": true,
      "connected": false,
      "duplicateUrl": false,
      "trimOffsetMs": 0,
      "metadata": []
    }
  ],
  "views": {
    "multiviewCount": 4,
    "slotMap": [0, 1, 2, 3],
    "singleView": false,
    "selectedIndex": -1
  },
  "settings": {
    "fileName": "",
    "saveLocation": "",
    "recordWidth": 1920,
    "recordHeight": 1080,
    "recordFps": 30,
    "audioOutputLatencyMs": 0,
    "timeOfDayMode": false,
    "metadataFields": []
  },
  "midi": {
    "ports": [],
    "portIndex": -1,
    "portName": "",
    "connected": false,
    "learnAction": -1,
    "learnMode": 0
  },
  "streamDeck": {
    "supported": false,
    "connected": false,
    "deviceName": "",
    "deviceModel": "",
    "keyCount": 0,
    "dialCount": 0,
    "learnAction": -1
  },
  "screens": {
    "ready": false,
    "count": 0,
    "options": []
  },
  "import": {
    "settingsUrl": "",
    "telemetrySseUrl": "",
    "previewReady": false,
    "previewError": "",
    "preview": {}
  },
  "telemetry": {
    "version": 0,
    "rows": [],
    "state": {}
  }
}
```

`views.singleView` and `views.selectedIndex` are currently internal UIManager
fields. If implementation needs to publish them, add read-only accessors and a
change signal rather than duplicating state in the WebSocket server.

## Command Surface

Commands are named strings under stable groups. Unknown commands fail without
side effects.

### Transport

- `transport.playPause`
- `transport.play`
- `transport.pause`
- `transport.setSpeed` with `{ "speed": number, "playing": boolean? }`
- `transport.holdSpeed` with `{ "speed": number, "active": boolean }`
- `transport.stepFrame` with `{ "frames": integer }`
- `transport.seek` with `{ "positionMs": integer }`
- `transport.goLive`
- `transport.cancelFollowLive`

`transport.holdSpeed` exists for button press/release clients. On `active:
true`, the server stores the current playing state for that connection and sets
the requested speed/playing state. On `active: false`, it restores speed `1.0`
and the stored playing state if present.

### Recording

- `recording.start`
- `recording.stop`
- `recording.toggle`

### View

- `view.setPlaybackViewState` with `{ "singleView": boolean, "selectedIndex": integer }`
- `view.showMultiview`
- `view.selectFeed` with `{ "index": integer }`
- `view.toggleSourceEnabled` with `{ "index": integer }`

`view.selectFeed` emits the same feed selection path used by action ids
`100..107`.

### Capture

- `capture.current`
- `capture.snapshot` with `{ "singleView": boolean, "selectedIndex": integer, "playheadMs": integer }`

### Sources

- `sources.add`
- `sources.remove` with `{ "index": integer }`
- `sources.updateUrl` with `{ "index": integer, "url": string }`
- `sources.updateName` with `{ "index": integer, "name": string }`
- `sources.updateId` with `{ "index": integer, "id": string }`
- `sources.setTrimOffset` with `{ "index": integer, "ms": integer }`
- `sources.setMetadata` with `{ "index": integer, "items": array }`

### Settings

- `settings.setProject` with `{ "fileName": string?, "saveLocation": string? }`
- `settings.setRecordingFormat` with `{ "width": integer?, "height": integer?, "fps": integer? }`
- `settings.setAudioOutputLatency` with `{ "ms": integer }`
- `settings.setTimeOfDayMode` with `{ "enabled": boolean }`
- `settings.setMetadataFields` with `{ "fields": array }`
- `settings.save`

Changing FPS while recording should return `not_allowed`, matching the QML
intent that FPS is disabled during recording.

### Import

- `import.setUrl` with `{ "url": string }`
- `import.read`
- `import.applyPreview`

### MIDI

- `midi.refreshPorts`
- `midi.setPortIndex` with `{ "index": integer }`
- `midi.beginLearn` with `{ "action": integer }`
- `midi.beginLearnJogForward` with `{ "action": integer }`
- `midi.beginLearnJogBackward` with `{ "action": integer }`
- `midi.clearBinding` with `{ "action": integer }`

Binding labels and last values are state/query data. A future protocol revision
can add dedicated read commands without changing the event model.

### Stream Deck

- `streamDeck.beginLearn` with `{ "action": integer }`
- `streamDeck.clearBinding` with `{ "action": integer }`
- `streamDeck.resetDefaults`

Hardware simulator commands remain iOS/debug UI concerns and are not required
for the local-network API.

### Shared Action Vocabulary

- `action.dispatch` with `{ "actionId": integer, "pressed": boolean? }`
- `action.jog` with `{ "delta": integer }`

`pressed` defaults to `true`. The server maps it to the same press/release
semantics used by StreamDeckManager. Action ids:

| Id | Action |
|----|--------|
| 0 | Play/Pause |
| 1 | Rewind 5x hold |
| 2 | Forward 5x hold |
| 3 | Step frame forward |
| 4 | Go live |
| 5 | Capture snapshot |
| 6 | Show multiview |
| 7 | Step frame backward |
| 8 | Jog |
| 9 | Record toggle |
| 10 | Shuttle dial |
| 20 | Timecode display only |
| 21 | Speed display only |
| 100..107 | Feed select |

Display-only ids acknowledge successfully but do not dispatch.

## Publishing Rules

On client connect:

1. Accept the socket.
2. Send one `state.snapshot`.
3. Send a current `timecode` message so simple button-display clients do not
   have to inspect the full snapshot.

Immediate patches:

- `recording`: `recordingStatusChanged`, `recordingStartEpochMsChanged`,
  `recordedDurationMsChanged`
- `transport`: `playingChanged`, `speedChanged`, `fpsChanged`,
  `followLiveChanged`, seek/step commands, `playbackTimecodeChanged`
- `sources`: stream URL/name/id changes, source enabled changes, source trim
  changes, source connection changes
- `views`: multiview count and slot-map changes
- `settings`: project path/name, format, audio latency, metadata field changes
- `midi`: port/connect/learn/binding changes
- `streamDeck`: connection/learn/binding changes
- `screens`: screen list changes
- `import`: import URL, preview, telemetry config changes
- `telemetry`: telemetry changes

Events:

- `recording.started`
- `recording.stopped`
- `recording.failed`
- `source.connected`
- `source.disconnected`
- `import.previewReady`
- `import.previewFailed`

Timecode:

- Coalesce `PlaybackTransport::posChanged`, recorder pulses, and timecode text
  changes into at most one `timecode` message every 100 ms.
- Send a trailing update after playback pauses, a seek completes, or a step
  command executes, so button displays settle on the exact final frame.
- Use `UIManager::playbackTimecode()` as the single source of truth for the
  display string.

Telemetry:

- On `telemetryChanged`, publish a `state.patch` for `telemetry` containing
  `version`, `rows`, and raw `state` from `telemetryAtPlayhead()`.
- Timecode updates do not need to include telemetry payloads; clients that need
  telemetry subscribe to patches.

## Error Handling

Protocol errors:

- `bad_json`: message is not valid JSON.
- `bad_message`: valid JSON but not an object or missing required top-level
  fields.
- `unsupported_message`: binary or unsupported message type.
- `unknown_command`: command name is not registered.
- `invalid_args`: command arguments have the wrong type or required fields are
  missing.

Command-state errors:

- `not_allowed`: command is valid but not allowed in the current app state.
- `failed`: command was attempted but the app rejected it or could not complete
  it.

Errors with request ids are sent as failed `ack` messages. Errors without ids
use:

```json
{
  "type": "error",
  "code": "bad_json",
  "message": "Message must be a JSON object"
}
```

## Validation and Threading

All app mutations should run on the Qt main thread. WebSocket callbacks already
arrive on the server object's thread; keep the server in the main thread and use
direct calls to `UIManager` methods. If a future revision moves networking to
another thread, use queued invocations for all `UIManager` and
`PlaybackTransport` calls.

Validate command arguments before mutating app state. Bounds that already exist
in `UIManager` should still be checked at the protocol layer when possible so
clients receive useful errors instead of silent no-ops.

## Testing

Use TDD for implementation.

Recommended test slices:

1. Protocol parser/dispatcher unit tests with a fake command adapter:
   malformed JSON, unknown commands, invalid args, successful ack with echoed
   id.
2. Snapshot serialization tests using a lightweight fake state provider or
   focused serializer helpers.
3. Timecode throttling tests with a controllable timer or separated coalescer
   helper.
4. Qt integration test: start the WebSocket server on an ephemeral test port,
   connect with `QWebSocket`, receive a snapshot, send a command, and verify
   acknowledgement plus relevant state publication.
5. Existing targeted tests for `PlaybackTransport`, telemetry, and StreamDeck
   mapping remain the behavioral safety net for lower-level components.

Implementation should add a test target only for code that can run headless in
CI. If direct `UIManager` integration is too heavy because it pulls multimedia
and FFmpeg, keep the server testable through a narrow adapter interface and
cover the real wiring with compile/build verification.

## Documentation

During implementation, add a concise user-facing protocol document at
`docs/websocket-control-api.md` with:

- Endpoint and port.
- Snapshot shape.
- Command list.
- Example StreamDeck-style button messages.
- Example subscription flow.
- Error codes.

The design document remains the implementation rationale; the protocol document
becomes the integration reference.

## Approaches Considered

1. **One WebSocket endpoint with typed JSON messages (chosen).**
   One persistent socket carries commands, acknowledgements, state snapshots,
   state patches, events, and timecode. This is simple for StreamDeck plugins
   and still expressive enough for richer clients.

2. **RPC-style WebSocket.**
   Every command is a strict request/response RPC and all state changes are
   separate notifications. This is clean but adds ceremony without improving
   the first target client.

3. **Minimal action bus.**
   Only expose action ids and a few status messages. This is fastest, but it
   fails the requirement to expose UI-level settings, sources, import preview,
   telemetry, and device status.

## Open Implementation Notes

- `UIManager::dispatchControlAction`, `jogStep`, and `shuttleStep` are private.
  Add explicit public adapter methods such as `dispatchExternalAction()`,
  `jogExternal()`, and `shuttleExternal()` rather than making the WebSocket
  server a friend or duplicating the dispatch switch.
- `UIManager` currently does not emit change signals for metadata field updates
  or source metadata updates. Add a focused signal or reuse a settings/source
  signal during implementation so WebSocket clients get patches after those
  commands.
- `addStream()` and `removeStream()` currently do not save settings. Preserve
  existing behavior unless implementation reveals QML relies on explicit manual
  save; document this in the protocol docs if unchanged.
- `view.selectFeed` may require exposing a method rather than emitting
  `feedSelectRequested` from outside `UIManager`.
- `QWebSocketServer` lives in Qt's WebSockets module, not just Qt Network. CMake
  should add the exact Qt component and link target during implementation.
