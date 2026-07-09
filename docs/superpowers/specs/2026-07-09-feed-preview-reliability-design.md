# Feed Preview Reliability Design

## Goal

Keep the embedded PGM and multiview previews, plus the detached multiview
window, attached to the current `FrameProvider` objects and advancing during
continuous playback without undoing the GPU-resident output pipeline.

## Problem

Preview frames reach the output graph and provider capture path, but two
presentation-boundary contracts are unsafe:

1. QML stores replaceable C++ provider objects in untyped `var` properties.
   `UIManager::refreshProviders()` destroys the old providers before emitting
   `playbackProvidersChanged`. A `VideoOutput` can then retain an invalid
   JavaScript wrapper, throw while calling `removeVideoSink()`, and return before
   attaching its replacement.
2. Cross-thread `FrameProvider` delivery queues one callback per serial. Each
   callback refuses to present unless its captured serial is still the newest.
   If producers remain ahead of the UI thread, every queued callback can be
   stale; the preview advances only after production pauses and the queue reaches
   its final callback.

The macOS app oracle currently validates provider JPEGs and visible single-view
markers after configuring sources. It does not reject QML runtime errors from an
earlier multiview reattachment failure, so the broken lifecycle can pass the
gate.

## Design

### Destruction-aware QML attachments

The PGM stage and detached multiview window will type provider references as
`QtObject` instead of `var`. Object-valued QML properties become `null` when the
underlying C++ `QObject` is destroyed, so provider replacement cannot leave a
callable-looking error wrapper behind.

Attachment helpers will also treat detachment as best-effort: clear the stored
reference first, call `removeVideoSink()` only when the old object still exposes
that function, then attach the new provider. A dead old provider needs no
explicit removal because its sink list is destroyed with it. The new provider's
`addVideoSink()` remains required; failure to expose that API is a programming
error that should stay visible in the QML log.

This applies to:

- the embedded multiview output;
- the embedded single-feed PGM output; and
- the detached `MultiviewWindow` output.

### Progress-guaranteed latest-frame delivery

`FrameProvider` will keep at most one queued UI-thread update per `QVideoSink`.
That update reads the provider's latest frame when it executes instead of
requiring an earlier captured serial to remain current.

After presenting a frame, the callback compares the applied serial with the
provider's current serial while coordinating with the per-sink pending state:

- if they match, it clears the pending flag;
- if a newer frame already exists, it queues exactly one follow-up callback;
- if a newer frame arrives after the flag is cleared, normal delivery queues the
  next callback.

The callback and producer coordinate pending state under the sink mutex. The
callback reads the current frame serial while holding that mutex, whereas
producers update the frame before acquiring it. This ordering prevents a frame
from arriving between the final comparison and pending-state clear without
also scheduling another update.

Same-thread sinks continue to receive frames immediately. Existing flush
semantics remain serial-based: applying any frame at or beyond the requested
serial satisfies the flush.

### App-level failure detection

The macOS app driver will scan the captured application log before reporting a
pass and reject QML `TypeError`, `ReferenceError`, or `Binding loop` diagnostics
originating from OpenLiveReplay QML. Expected media decoder diagnostics remain
outside this check.

## Testing

1. Extend the PGM-stage Quick Test fixture with C++ provider objects that are
   destroyed and replaced while multiview or single-view output is active.
   Assert that the replacement provider receives the `QVideoSink`.
2. Exercise the detached multiview window against the same replacement fixture.
3. Add a `FrameProvider` test with a deliberately backlogged sink thread and a
   faster continuous producer. Assert that the visible sink advances before the
   producer stops, while retaining the existing one-update batch coalescing
   behavior.
4. Unit-test the app-log diagnostic scanner and require the real macOS app oracle
   to invoke it.
5. Run the focused Quick/QVideoSink tests, the complete unit label, playback E2E,
   and the real four-feed macOS app visual oracle.

## Non-goals

- Replacing `VideoOutput` with CPU-painted preview items.
- Changing GPU readback cadence, PGM transaction completion, output bus
  rendering, or external sink behavior.
- Redesigning `UIManager` to keep provider objects permanently allocated.
