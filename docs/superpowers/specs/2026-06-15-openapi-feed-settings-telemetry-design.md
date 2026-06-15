# OpenAPI Feed Settings And Telemetry - Design

**Status:** approved (brainstorm) -> spec
**Date:** 2026-06-15
**Branch:** `feat/openapi-feed-telemetry`

## 1. Motivation

OpenLiveReplay needs a standard external provider contract for event/project input
configuration and per-feed telemetry. The operator should not manually recreate
feed IDs, names, URLs, metadata, and telemetry wiring for each event. Instead, the
operator enters one public or pre-signed HTTPS URL, reads the provider settings,
previews the imported inputs, and applies them only after confirmation.

Telemetry must also be part of the recording, not only a live display overlay. It
must be replayable during chase playback while recording and after reopening the
recorded file later. The replayed telemetry state must follow the playhead and
feed identity, independent of the operator's current multiview layout.

## 2. Decisions From Brainstorm

- OpenLiveReplay is a **consumer-only** client in v1. It does not serve/export the
  API.
- The operator-entered URL returns the project settings JSON directly.
- The settings JSON may include `schemaVersion` and optional `links.openapi`.
- Authentication is out of scope for v1: provider URLs are public or pre-signed
  HTTPS URLs.
- The imported project has **one single project-level SSE telemetry stream**.
  Telemetry events carry `feedId`.
- Applying imported settings **replaces** the current input source list and
  imported metadata definitions.
- The user keeps local control of project filename, save location, output
  resolution, FPS, multiview count, and which configured feeds are displayed.
- Static feed metadata uses the current OLR format: global `metadataFields` plus
  per-feed `metadata` entries shaped as `{ "name": "...", "value": "..." }`.
- Telemetry timing uses OLR receive time on the recording timeline, plus a
  per-feed positive `telemetryDelayMs` in the range `0..10000`. Provider event
  timestamps are preserved for audit/debug but do not decide replay position.
- Telemetry is recorded per configured feed, including feeds that are not currently
  assigned to a displayed/recorded view.

## 3. Provider Contract

The provider exposes a settings endpoint that returns `application/json`. The
operator pastes this endpoint URL into OLR.

Example settings response:

```json
{
  "schemaVersion": "olr.project-settings.v1",
  "project": {
    "id": "event-2026-final",
    "name": "Final Match"
  },
  "links": {
    "openapi": "https://provider.example/openapi.json"
  },
  "telemetry": {
    "sseUrl": "https://provider.example/events/final/telemetry"
  },
  "metadataFields": [
    { "name": "angle", "display": true },
    { "name": "operator", "display": true }
  ],
  "feeds": [
    {
      "id": "cam-main",
      "name": "Main",
      "url": "srt://10.0.0.20:9000",
      "telemetryDelayMs": 800,
      "metadata": [
        { "name": "angle", "value": "wide" },
        { "name": "operator", "value": "Aino" }
      ]
    }
  ]
}
```

The accompanying OpenAPI document standardizes:

| Endpoint | Response | Purpose |
|---|---|---|
| `GET /project-settings` | `application/json` | project/feed settings import |
| `GET /telemetry` | `text/event-stream` | single project telemetry stream |

Example telemetry SSE event payload:

```json
{
  "feedId": "cam-main",
  "timestamp": "2026-06-15T12:00:01.250Z",
  "status": "ok",
  "values": {
    "batteryPercent": 91,
    "signalDb": -63,
    "gps": { "lat": 60.1699, "lon": 24.9384 }
  }
}
```

Telemetry payload rules:

- `feedId` is required and must match an imported feed ID.
- `timestamp` is optional but recommended. OLR preserves it as provider time.
- `status` is optional and provider-defined, with common values such as `ok`,
  `warning`, `error`, or `offline`.
- `values` is a flexible JSON object. OLR should preserve unknown provider fields.

## 4. Import UI

The Project tab gains an external settings import control near Input Sources:

1. URL text field.
2. `Read` button.
3. Preview popup or panel after a successful fetch.
4. `Apply` button inside the preview.

The preview uses the approved **single imported summary** layout. It shows:

- imported project ID/name
- imported feed count
- metadata fields
- telemetry SSE URL
- each feed's ID, name, video URL, metadata summary, and telemetry delay
- a clear note that Apply will replace the current input sources

No local settings change during Read. Apply is disabled while recording. On Apply,
OLR replaces imported-owned settings and leaves local output/display choices
unchanged.

Apply writes:

- `AppSettings.sources`
- `AppSettings.metadataFields`
- imported settings URL
- project telemetry SSE URL
- per-feed telemetry delay

Apply preserves:

- save location
- file name
- output width/height/FPS
- multiview count
- current local controller bindings

After Apply, OLR refreshes source/view mapping using the existing source update
paths.

## 5. Validation And Error Handling

Validation before preview:

- Settings URL must be HTTPS, except optional local-development allowances.
- Response must parse as a JSON object.
- `schemaVersion` must be known or compatible. Missing version may be rejected or
  treated as v1 only if the shape is otherwise exact.
- `telemetry.sseUrl` is required and must be HTTPS in production.
- `feeds` must be a non-empty array.
- Feed IDs must be non-empty and unique.
- Feed video `url` values must be non-empty.
- `telemetryDelayMs` defaults to `0` and must be clamped or rejected outside
  `0..10000`.
- `metadataFields` and per-feed `metadata` use the current OLR shape.

Operational errors:

- Fetch failures show an import error and leave current settings untouched.
- Invalid JSON/schema shows actionable validation errors in the preview area.
- SSE disconnects degrade telemetry status but do not stop recording.
- Unknown `feedId` events are ignored or logged with throttling.
- Malformed telemetry events are dropped with throttled warnings.
- Recording continues if telemetry is unavailable.

## 6. Recording Data Flow

Recording must persist telemetry as durable, time-indexed data keyed by configured
feed ID, not by view slot.

When recording starts:

1. OLR opens the single project SSE stream from the imported settings.
2. Each telemetry JSON event is parsed and routed by `feedId`.
3. OLR captures receive time relative to recording start.
4. Effective replay time is:

   ```text
   effectiveTelemetryTimeMs = receiveTimeMs + feed.telemetryDelayMs
   ```

5. OLR writes the full telemetry event plus OLR timing fields into the recording.

Recorded telemetry should preserve at least:

- feed ID
- OLR receive time
- OLR effective replay time
- provider timestamp, if present
- status, if present
- full `values` object
- original/raw event object when practical

## 7. Storage And Replay

Telemetry storage must be independent of existing per-view metadata subtitles.
Existing per-view metadata can continue to support static/source display metadata,
but dynamic telemetry must not be tied to the current view mapping. Otherwise
telemetry would be lost or duplicated when feeds are hidden, remapped, or not
currently displayed.

The implementation plan should choose a Matroska-compatible in-file structure,
preferably dedicated feed telemetry tracks or an equivalent durable structure that:

- can grow during recording for chase playback
- survives normal close/reopen
- carries enough feed identity to rebuild timelines without the current config
- can be queried by playhead time
- does not require the provider API to be reachable during file replay

During chase replay, playback reads the growing recorded telemetry timeline from
the same recording file path used for video/audio. After reopening a file, playback
reconstructs feed telemetry timelines from the recording itself.

The UI displays telemetry according to the playback playhead:

- At live edge, it shows delayed live telemetry aligned with the video.
- When scrubbing backward, it shows historical telemetry at that time.
- When a feed has no event yet at the playhead, it shows an empty/unknown state.

## 8. Components

Suggested implementation boundaries:

| Component | Responsibility |
|---|---|
| `ProjectImportClient` | fetch direct settings JSON and return parsed JSON or network error |
| `ProjectSettingsImporter` | validate schema and convert imported JSON into OLR settings/telemetry config |
| `TelemetryClient` | own the single SSE connection and parse JSON event payloads |
| `TelemetryRecorder` | stamp receive time, apply per-feed delay, and write telemetry to the muxer |
| `TelemetryTimelineReader` | read recorded telemetry and answer "state at playhead time" |
| `UIManager` integration | expose import preview/apply state and telemetry-at-playhead data to QML |
| `Main.qml` | add import controls, preview UI, and telemetry display surfaces |

The implementation should keep the units small: importing/validation should be
testable without QML or FFmpeg, SSE parsing without the recorder, and timeline
lookup without live network access.

## 9. Testing

- Unit: project settings import validation, including duplicate IDs, missing SSE
  URL, invalid delays, malformed metadata, and replace semantics.
- Unit: telemetry event parsing, unknown feed handling, and malformed payload drop.
- Unit: telemetry timestamping with `receiveTimeMs + telemetryDelayMs`.
- Unit or integration: settings persistence for imported settings URL, SSE URL,
  and per-feed telemetry delay.
- Muxer/storage: prove telemetry records are written with feed identity and
  timeline timestamps.
- Playback: prove telemetry can be read by playhead time from a reopened file.
- QML smoke/lint: import controls and preview UI.
- E2E/harness: record synthetic video plus synthetic SSE telemetry, chase-read
  during recording, close, reopen, and verify the same telemetry timeline appears.

## 10. Out Of Scope

- OLR serving/exporting this API.
- App-managed authentication, token refresh, or secret storage.
- Multiple SSE streams.
- Provider timestamp based replay alignment.
- Editing provider settings from OLR.
- Auto-detecting telemetry/video latency.
- A full telemetry visualization system beyond enough display to prove live and
  replay behavior.

## 11. Risks

- **Matroska telemetry representation:** FFmpeg/Matroska support for data-like
  tracks needs a careful proof. The implementation plan should spike the smallest
  reliable in-file representation before wiring UI.
- **Chase-read visibility:** telemetry packets must flush often enough for replay
  during recording, matching the existing low-latency file-writing expectations.
- **SSE network stability:** telemetry disconnects must not disturb video
  recording. Reconnect/backoff policy belongs in the implementation plan.
- **Delay semantics:** positive delay means events appear later on replay. This is
  correct because telemetry usually arrives before the matching image, but the UI
  should label it clearly as telemetry delay.
- **Unknown provider fields:** preserving flexible JSON makes the standard useful,
  but display should avoid assuming every field is known or scalar.
