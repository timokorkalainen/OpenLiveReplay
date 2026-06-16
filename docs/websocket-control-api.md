# WebSocket Control API

OpenLiveReplay exposes an unauthenticated WebSocket control API intended for trusted local-network clients (for example StreamDeck integrations).

## Endpoint

Connect to:

```text
ws://<app-host>:8115/api/ws
```

The server listens on port `8115`; the current server implementation accepts the connection on that port regardless of path, but `/api/ws` is the intended endpoint for clients.

## Connection Flow

On connect, the server sends two messages immediately:

```json
{ "type": "state.snapshot", "state": { ... } }
```

```json
{
  "type": "timecode",
  "positionMs": 0,
  "durationMs": 0,
  "text": "00:00:00:00",
  "followLive": false,
  "playing": false,
  "speed": 1.0
}
```

## Command Message Shape

Send JSON text messages with:

```json
{
  "type": "command",
  "id": "button-1",
  "name": "transport.playPause",
  "args": {}
}
```

`id` is echoed in acknowledgements.

### Ack and Error Replies

Success acknowledgement:

```json
{ "type": "ack", "id": "button-1", "ok": true }
```

Failed acknowledgement:

```json
{
  "type": "ack",
  "id": "button-1",
  "ok": false,
  "code": "invalid_args",
  "message": "transport.seek requires integer args.positionMs"
}
```

Malformed JSON without id returns an `error` message:

```json
{ "type": "error", "code": "bad_json", "message": "Invalid JSON: ..." }
```

## StreamDeck-Style Action Examples

Play/pause dispatch:

```json
{ "type": "command", "id": "play", "name": "action.dispatch", "args": { "actionId": 0 } }
```

Rewind press / release:

```json
{ "type": "command", "id": "rew-down", "name": "action.dispatch", "args": { "actionId": 1, "pressed": true } }
{ "type": "command", "id": "rew-up", "name": "action.dispatch", "args": { "actionId": 1, "pressed": false } }
```

Jog one frame:

```json
{ "type": "command", "id": "jog-1", "name": "action.jog", "args": { "delta": 1 } }
```

Shuttle one detent:

```json
{ "type": "command", "id": "shuttle-1", "name": "action.shuttle", "args": { "delta": 1 } }
```

StreamDeck action id `10` is a shuttle dial action and requires `action.shuttle`
with a `delta`; sending it through `action.dispatch` is rejected.

## Common Commands

- `transport.playPause`
- `transport.seek` with `{ "positionMs": 1200 }`
- `transport.setSpeed` with `{ "speed": 0.5, "playing": true }`
- `transport.holdSpeed` with `{ "active": true, "speed": 0.5 }` on press and `{ "active": false }` on release
- `recording.toggle`
- `view.showMultiview`
- `view.selectFeed` with `{ "index": 0 }`
- `sources.updateUrl` with `{ "index": 0, "url": "srt://example" }`
- `settings.save`

## State Updates

The server publishes:

- `state.patch` for updated paths:

```json
{ "type": "state.patch", "path": "transport", "value": {} }
```

- `event` for lifecycle / control events:

```json
{ "type": "event", "name": "recording.started", "data": {} }
```

- `timecode` messages whenever playback timing changes, throttled to at most 10 Hz.

In this version, `state.patch` with `path: "snapshot"` is also used for broad compound state changes.

## Error Codes

- `bad_json`: payload is not valid JSON
- `bad_message`: JSON is invalid protocol shape (for example, missing `name`)
- `unsupported_message`: unsupported websocket payload type or unsupported message type
- `unknown_command`: command `name` is not recognized
- `invalid_args`: command arguments are missing or have the wrong type
- `not_allowed`: command is valid but not allowed in the current app state
- `failed`: command execution failed
