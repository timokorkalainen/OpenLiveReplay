# OpenLiveReplay Provider API v1

OpenLiveReplay is a consumer of this contract. The operator enters one public or
pre-signed HTTPS settings URL that returns `ProjectSettings` JSON directly.
OpenLiveReplay reads it, shows a preview, and applies it only after operator
confirmation.

Files:

- `olr-provider-v1.openapi.json` describes the provider contract.
- `project-settings.example.json` is an importable settings response.
- `telemetry.sse.example` shows the single SSE stream format.

Contract summary:

- Settings JSON may include `schemaVersion: "olr.project-settings.v1"`.
  Missing `schemaVersion` is treated as v1 when the document shape is valid.
- `links.openapi` is optional and may point back to the OpenAPI document.
- `telemetry.sseUrl` is required and must be HTTPS.
- There is exactly one project-level SSE telemetry stream.
- Every telemetry event must include `feedId`, matching one configured feed.
- `telemetryDelayMs` is per feed, defaults to `0`, and must be `0..10000`.
- Feed metadata uses the OpenLiveReplay shape `{ "name": "...", "value": "..." }`.
