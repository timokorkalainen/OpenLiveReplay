# Output Validation And NDI Hardening Design

**Status:** approved for implementation
**Date:** 2026-06-17

## Goal

Add measurable proof that every clean output target receives the same backend-produced frame for the same logical bus tick, and make NDI output operationally observable enough to harden before DeckLink/ST 2110 work begins.

## Scope

This slice extends the broadcast output bus landed in PR #54. It does not change playback semantics, add DeckLink, add ST 2110, or redesign the UI. It adds identity and health data to the frames and sinks already flowing through the output runtime.

## Design

Each `OutputBusFrame` gains a deterministic `OutputFrameIdentity`. The identity records the logical bus, output frame index, sampled playhead, source feed, source PTS, placeholder state, audio-silence state, and lightweight hashes of the video and audio payloads. The identity is produced by `OutputBusEngine` immediately after the frame payloads are selected or composed.

`OutputDispatcher` keeps aggregate stats and adds per-target stats keyed by target assignment id. Target stats record attempted submissions, successful submissions, sink failures, placeholder frames, silent audio frames, repeated payloads, and the last identity delivered to that target. Repeated payloads are detected by comparing video/audio hashes and source identity while allowing `outputFrameIndex` to advance.

`QueuedOutputSink` continues to protect the output runtime from blocking sinks, but its dropped-frame counter becomes part of target health. The dispatcher owns target-level submission and failure stats; queue overflow remains a sink-level stat exposed for adapters that need it.

`NdiOutputSink` gains a status snapshot. It reports runtime unavailable, create failure, active, stopped, and send failure states through a testable enum plus a short diagnostic string. Runtime discovery checks explicit overrides, standard SDK locations, and common NDI Tools app bundles so an installed local NDI runtime can be used without per-run library overrides.

NDI sends require broadcast-valid video and audio. Missing or malformed audio is a send failure instead of a silent success, because every enabled clean output must carry an audio frame even when that frame is silence.

## Tests

Tests are written first:

- Qt preview and NDI assignments for the same bus/tick receive matching frame identities.
- Paused output advances output frame index while repeating payload identity.
- PGM source switches on the next tick and per-target stats show the new source.
- A failing sink increments only that target's failures while other targets keep receiving frames.
- NDI status reports unavailable runtime, create failure, active, stopped, and send failure.
- NDI runtime smoke routes cache-backed app output through `OutputDispatcher` into `NdiOutputSink`, then verifies receiver video cadence and non-silent audio. Longer soaks compare captured frames against submitted output frames.

## Non-Goals

- No DeckLink or ST 2110 implementation.
- No OMT/AJA implementation.
- No output UI redesign.
- No pass-through.
- No new playback/decode semantics.
