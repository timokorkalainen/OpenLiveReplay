# PGM-first transactional output design

**Status:** implementation in progress; macOS app strict PGM gate is the current
required full-system proof before iOS validation
**Platforms:** macOS first for fast test turnaround; iOS/iPadOS must meet the same contract
**Related specs:** `2026-06-21-gpu-resident-pipeline-design.md`,
`2026-07-07-ios-frame-residency-design.md`

## Problem

Paused seeking, frame stepping, jogging, and scrub preview currently cross a mixed
synchronous/asynchronous boundary. If the requested playhead is already covered by
the output cache, `PlaybackWorker::seekTo()` can publish and dispatch immediately.
If the request is a cache miss, `seekTo()` records intent and returns; the worker
loop later performs `repositionTo()`, refreshes output, and only then can the new
frame reach PGM.

That is the wrong contract for a professional replay controller. WebSocket,
StreamDeck, MIDI, and QML controls must not be able to acknowledge an operator
command while PGM still shows an older frame. The current behavior can be correct
eventually but still feel broken, because the visible output is not part of the
command transaction.

PGM is the authoritative output. Qt preview is only a preview. Preview must follow
PGM, never block PGM, and never define success for a seek/scrub command.

## Goal

For every paused operator command that changes the playhead:

- choose the target frame deterministically;
- make that frame available through the GPU path;
- render the PGM bus through the GPU path;
- submit the PGM output before command completion;
- update previews after or alongside PGM without delaying PGM;
- report enough telemetry to prove command-to-PGM latency and frame identity.

The practical latency target is **next output frame**. At 60 fps, that means avoiding
any avoidable extra-frame delay. The aspirational engineering target is **under
15 ms command-to-PGM-submit** on the fast path, with a hard regression gate of no
operator-command sample over **25 ms** unless the fixture itself proves the frame
cannot yet exist.

The output path must remain GPU-first because future features, including
GPU-generated slow motion and other PGM image processing, will operate on GPU PGM
frames.

## Non-goals

- Do not make Qt Quick scene graph timing part of the PGM contract.
- Do not use CPU output as the primary PGM path to win latency. CPU readback is
  allowed for sinks that need CPU pixels, such as NDI, but PGM composition and
  processing remain GPU-owned.
- Do not weaken the marker oracle by measuring only command ACK or app preview.
- Do not require a device install for every iteration; macOS app E2E remains the
  first full-system gate.

## Architecture

### 1. Operator commands become transactions

Introduce an operator command transaction for paused `seek`, `stepFrame`, jog, and
scrub samples. A transaction carries:

- command id;
- target playhead;
- direction hint;
- requested output bus, at minimum PGM;
- required deadline metadata for telemetry;
- completion state: committed frame identity, PGM submit result, latency.

The command is complete only after the PGM output lane has accepted the frame for
the committed target. For cold/cache-miss seeks, completion is driven by the worker
thread after `repositionTo()` commits and the PGM lane dispatches. For cache hits,
completion can happen inline.

WebSocket ACK, StreamDeck feedback, and any future external control confirmation
must be tied to transaction completion, not merely to accepting the request.

Operator transaction coverage is stricter than preview displayability. A preview
may hold a lower-cadence source frame between source frames, but an operator seek
or jog step must not certify success from a stale prior frame. Valid operator
coverage is exact target media, the immediately prior source frame within one
output tick, or a timestamp-rounding future frame.

### 2. PGM lane is dispatch-priority lane

Split immediate output dispatch into lanes:

1. **PGM real-time lane:** PGM bus render, GPU processing chain, and required
   broadcast outputs such as PGM NDI/SDI.
2. **Preview lane:** Qt multiview and PGM preview providers.
3. **Auxiliary lane:** non-critical monitoring and diagnostics outputs.

Paused command dispatch runs the PGM lane first and completes the operator
transaction after the PGM lane accepts the correct frame. Preview lane dispatch is
scheduled immediately after, but a slow Qt preview readback or scene-graph update
cannot delay PGM submit or WebSocket ACK.

### 3. GPU PGM frame is the canonical frame

The output bus engine must produce a canonical GPU PGM frame for each transaction.
Future GPU processing inserts between PGM composition and sink fanout:

```
decoded GPU frames
  -> output cache
  -> PGM GPU compositor
  -> GPU PGM processing chain
  -> PGM output fanout
       -> GPU-native outputs
       -> CPU-readback outputs such as NDI
       -> preview followers
```

NDI may require CPU pixels, so it can still use GPU readback, but that readback is
a sink adaptation step after canonical GPU PGM generation. The canonical PGM identity
remains the GPU frame and its marker/source metadata.

PGM must not bypass GPU composition just because the current PGM is a single selected
source. A selected-source PGM still flows through the GPU PGM compositor/processing
slot, then fans out to GPU-native outputs or CPU-readback sinks. This keeps the
latency work compatible with future GPU slow-motion and PGM image-processing stages.

### 4. Preview is a follower

Qt preview consumes the last committed PGM or multiview frame and updates as quickly
as possible. It must not:

- block PGM lane completion;
- hold command ACK hostage;
- force a synchronous 60 ms readback on the PGM lane;
- decide whether a command succeeded.

Preview correctness remains tested, but PGM output correctness and latency are the
primary gates.

### 5. Cold seek path is explicit

Cold seek means every target is more than five seconds away from the previous
playhead. The transaction path must treat cold seek as first-class:

- exact seek / frame index lookup;
- decode or reuse the target window;
- publish the target cache generation;
- render PGM immediately;
- complete the transaction from PGM dispatch evidence.

The worker must be woken immediately when a paused command enqueues a seek. Polling a
paused loop at 10 ms intervals is already most of a 60 fps frame budget and cannot be
part of the command path.

If the frame is genuinely unavailable because the recording has not reached it, the
transaction fails or degrades explicitly. It must not silently hold a stale frame.

## Timing Contract

The main success metric is command-to-PGM-output latency:

- **Preferred target:** PGM submit under 15 ms on common cache-hit and warm seek paths.
- **Hard gate:** no PGM marker sample over 25 ms in the macOS app E2E sequence.
- **Cadence rule:** for 25 fps video, the operator may only see the new frame when
  the next video/output cadence presents it; the system must still avoid adding an
  extra frame of avoidable delay.
- **60 fps requirement:** the architecture must be capable of delivering command
  results on the next 60 Hz output frame when the frame is available.

Telemetry must distinguish:

- command accepted;
- target frame committed;
- canonical GPU PGM frame rendered;
- CPU readback completed, when applicable;
- PGM sink accepted frame;
- preview updated.

## Testing Contract

The permanent test ladder is:

1. Unit tests for transaction state, seek commit behavior, and lane ordering.
2. Headless playback harness tests for marker identity and cold seek correctness.
3. macOS app E2E driven through WebSocket, with OS screenshots for preview evidence
   and PGM NDI marker probing for output latency.
4. iOS local SRT oracle after the macOS gate is reliable.
5. Physical-device OS screenshot checks when OS tooling is available.

Required E2E operator sequence:

- start stream and record;
- wait at least 30 seconds;
- pause;
- seek to a marked frame;
- step back 60 frames, one frame at a time, with 500 ms separation;
- step forward 15 frames;
- step back another 30 frames;
- perform cold seeks where each target is more than five seconds from the previous
  playhead, not merely from the original playhead.

For strict latency mode, every PGM NDI sample must identify the expected marker and
must be under the configured threshold. A single miss fails the run.

## Implementation Notes

- The first implementation should add the transaction abstraction and PGM lane split
  without replacing every output sink.
- The worker should expose a waitable transaction completion path for WebSocket and
  test drivers. UI calls can use the same path opportunistically but must not freeze
  the UI indefinitely; bounded timeout and explicit telemetry are required.
- `OutputRuntime::dispatchImmediate()` should grow a PGM-first variant rather than
  relying on endpoint order plus a monolithic all-output flush.
- Paused `seekTo()` must wake the worker instead of relying on the existing paused
  polling sleep before `repositionTo()` can run.
- Qt preview synchronous flush should be opt-in for preview tests, not part of PGM
  command completion.
- The transaction result should carry the committed frame identity so tests can
  assert the command delivered the exact target, not just any non-placeholder frame.

## Risks

- NDI CPU readback may still dominate some samples. The design keeps this visible by
  separating canonical GPU PGM render latency from readback/sink latency.
- WebSocket ACKs that wait for transaction completion can become slower on true cold
  decode misses. That is acceptable only if the delay reflects real frame work and
  is reported; silent stale output is worse.
- Qt preview may appear to lag behind PGM under load. That is acceptable if PGM is
  correct and preview catches up promptly, but it must be surfaced in telemetry so
  operators and tests do not confuse preview lag with PGM failure.
- Future GPU slow-motion processing must fit into the PGM GPU processing chain
  without reintroducing CPU ownership of PGM.

## Acceptance Criteria

- A paused cache-miss seek cannot return success to WebSocket before the PGM lane
  accepts the committed target frame.
- PGM lane dispatch is not blocked by Qt preview readback or Qt scene graph timing.
- The macOS app E2E reports all expected PGM NDI samples with correct marker identity
  and no sample above 25 ms.
- Latency telemetry separately reports command, commit, GPU PGM render, readback,
  sink submit, and preview update timings.
- iOS retains the same original `maps.rally.promo` source configuration for normal
  use, while marker SRT sources are used only by tests.
- No implementation claims are made without fresh unit, macOS E2E, and later iOS
  oracle evidence.
