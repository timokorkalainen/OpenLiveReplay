# Broadcast Clean Output Bus Design

**Status:** approved (brainstorm) -> spec
**Date:** 2026-06-17

## Goal

Redesign playback output so OpenLiveReplay produces frame-perfect, continuous,
clean broadcast feeds from the app's MKV replay timeline. Qt must display the
same backend-produced frames that can be sent to external outputs; Qt must not
be the component that decides frame timing, composition, freeze behavior, or
audio routing.

The target outputs are clean live-TV mixer inputs:

- one optional output for each playback feed;
- one optional clean multiview output;
- one optional clean PGM output that follows the latest selected playback feed.

All outputs follow one shared playback playhead. None has an independent
timeline.

## Current State

The current playback path decodes the MKV and pushes `QVideoFrame` objects into
`FrameProvider`, which then feeds QML `VideoOutput`. Local audio monitoring is
routed through `AudioPlayer` and only follows the selected single-view playback.

That model is adequate for UI preview, but it is not a broadcast playout model:

- `QVideoFrame` is the primary frame currency.
- Qt display timing is too close to the playback output decision point.
- There is no backend-owned clean output bus.
- There is no always-running output frame clock.
- External outputs would have to scrape or duplicate UI behavior.

Broadcast output must invert this ownership: the backend creates the frame
sequence first, then Qt and hardware outputs render or transmit it.

## Output Contract

The app owns a set of logical clean output buses:

```text
FeedOutput[0..N-1]  clean feed video + that feed's own audio
MultiviewOutput     clean tiled video + silence
PgmOutput           clean selected-feed video + selected-feed audio
```

Each logical bus is available to Qt preview. Each bus may also have zero or more
physical or network output target assignments. The logical bus does not care
whether the assignment is DeckLink SDI/HDMI, DeckLink IP/ST 2110, NDI, future
OMT, future AJA, or another output adapter.

This separation is mandatory:

```text
clean logical bus        output target assignment
FeedOutput[0]       ->   DeckLink device 1 / SDI output 1
FeedOutput[0]       ->   NDI sender "OLR Feed 1"
FeedOutput[1]       ->   DeckLink IP / ST 2110 flow
MultiviewOutput     ->   DeckLink HDMI monitor output
PgmOutput           ->   NDI sender "OLR PGM"
```

Output targets are optional and independent. Turning an NDI sender on or off for
FeedOutput[0] must not change the clean bus frame sequence, Qt preview, or any
other assigned target.

The buses are clean:

- no source labels;
- no borders;
- no tally;
- no telemetry overlays;
- no timecode overlays;
- no UI chrome.

Any labels, borders, status indicators, telemetry, or operator-facing overlays
remain QML-only decorations layered above the clean frames.

## Playback Semantics

All output buses follow the same `PlaybackTransport` state:

- playhead position;
- play/pause;
- speed;
- step;
- jog;
- shuttle;
- live-follow.

The output clock never stops while playback output is active. For every output
tick, each enabled or previewed bus emits a frame at the configured output frame
rate.

Pause means repeated frames:

```text
paused at frame 412
output tick 1000 -> frame 412
output tick 1001 -> frame 412
output tick 1002 -> frame 412
```

Step and jog update the playhead. Between updates, all buses keep repeating the
current playhead frame.

Shuttle and variable-speed playback keep the output clock continuous. The
playhead sampling may repeat frames, skip frames, or move backward depending on
speed, but output emission never stalls.

At normal 1x forward playback, each feed bus outputs its own audio, and PGM
outputs the selected feed's audio. During pause, step, jog, reverse, or non-1x
shuttle, output audio is silence in this phase. Scrub audio is a future feature.

## Architecture

Proposed backend shape:

```text
PlaybackTransport
  -> OutputFrameClock
  -> PlaybackDecodeEngine
       reads MKV/chase-play file
       keeps decoded video/audio cache warm around the playhead
  -> OutputBusEngine
       samples the shared playhead on exact output ticks
       produces clean FeedOutput, MultiviewOutput, and PgmOutput frames
  -> OutputSinks
       QtPreviewSink
       DeckLinkSdiHdmiSink
       DeckLinkIpSt2110Sink
       NdiSink
       future OmtSink
       future AjaSink
```

`PlaybackDecodeEngine` replaces the current "decode due frames straight into
Qt" role. It decodes each MKV feed once into shared media caches keyed by feed
index and media PTS.

`OutputBusEngine` is the only component that decides which media frame belongs
on output tick N. It produces deterministic video and audio frames for every
logical output bus.

`QtPreviewSink` adapts backend-produced frames to `QVideoFrame`. QML may render
those frames in single-view, multiview, or fullscreen preview, but it does not
choose broadcast frames.

`OutputTargetManager` owns the physical/network output assignments. It maps
logical buses to concrete output channels and sink implementations. It is not
allowed to re-time, re-select, or decorate the bus frames.

## Media Frame Model

Introduce backend-native frame structs before converting to Qt or hardware SDK
types.

Video frame fields:

- bus id;
- output frame index;
- source PTS or playhead PTS;
- width and height;
- rational frame rate;
- pixel format;
- colorimetry and range;
- field order/progressive flag;
- frame payload or shared buffer handle.

Audio frame fields:

- bus id;
- start sample;
- sample rate;
- channel count;
- sample format;
- PCM payload.

The first implementation can keep CPU buffers. The API should not require Qt
types, so DeckLink/ST 2110 support can be added without routing through QML.

## Output Clock

The output clock must use rational frame timing, not integer milliseconds.

The current `int fps` model is not enough for broadcast. The output design must
support at least:

- 25p and 50p;
- 29.97p and 59.94p;
- 23.976p where supported;
- future interlaced modes if required by DeckLink/ST 2110 outputs.

Qt timers may observe state changes, but they must not drive broadcast output
ticks. DeckLink output should schedule frames against DeckLink's hardware
playout timeline. ST 2110 output should schedule RTP timestamps from the output
clock or the selected hardware/PTP-backed clock.

## Continuity Rules

Every bus must remain continuous.

If video for the target playhead frame is available, emit it.

If the target frame is not yet decoded but a previous valid frame exists for the
bus, repeat the last valid frame and count a hold event.

If no valid frame has ever existed for that bus, emit clean black or clean blue
according to configuration.

For audio, emit exact sample spans at 48 kHz. If source audio is missing,
outside 1x playback, or unavailable for the target span, emit silence.

No sink may block the output bus engine. Physical sinks receive frames through
bounded queues; underrun, overrun, and dropped/held-frame counters are reported
as health telemetry.

## Logical Outputs

### Feed Outputs

Each playback feed has one clean output bus:

- video: that feed at the shared playhead;
- audio: that feed's own audio at the shared playhead;
- pause/step/jog/reverse/non-1x audio: silence;
- missing video: repeat last valid frame, else clean placeholder.

### PGM Output

PGM follows the latest selected playback feed:

- video: selected feed at the shared playhead;
- audio: selected feed audio at the shared playhead;
- selection change: immediate cut on the next output frame;
- no selected feed: clean placeholder video and silence.

PGM selection is backend state. Qt single-view display subscribes to the same
state, rather than defining it.

### Multiview Output

Multiview is a clean tiled video output built from the feed buses at the same
playhead.

The first implementation has no labels, borders, tally, clocks, or UI overlays.
It carries silence. If audio-follow-PGM or a mix-minus style multiview audio
mode is needed later, it should be an explicit configuration.

## Physical Sink Requirements

Physical and network sinks are adapters. They consume already-produced clean bus
frames and put them onto an output channel. They may convert pixel format,
sample layout, packetization, or SDK-specific frame wrappers, but they must not
decide what content the bus carries.

Every target assignment has:

- target id;
- source logical bus id;
- sink kind (`decklink-sdi-hdmi`, `decklink-ip-st2110`, `ndi`, future `omt`,
  future `aja`);
- enabled flag;
- sink-specific device/channel/network settings;
- health state and counters.

Multiple target assignments may consume the same logical bus. A sink failure
must degrade only that assignment; the logical bus and other sinks continue.

### Qt Preview Sink

Qt preview receives the backend-produced clean bus frames and converts them to
`QVideoFrame`.

QML may display:

- per-feed clean frames with UI overlays added visually;
- clean PGM frame;
- clean multiview frame;
- operator labels and status decorations above the frame.

Qt preview must not modify the backend clean frame payload.

### DeckLink SDI/HDMI Sink

DeckLink SDI/HDMI output attaches one logical bus to one DeckLink output
channel. It must:

- enumerate devices and output capabilities;
- validate display mode and pixel format;
- enable video and audio output;
- preroll video/audio;
- schedule frames continuously;
- report reference lock, buffered frame count, underrun, and device loss;
- convert backend pixel format to a DeckLink-supported format such as UYVY or
  v210 as required by the selected mode.

The DeckLink SDK playback path expects applications to check supported modes,
enable video/audio output, preroll, schedule video frames, and keep scheduling
frames during callbacks:
https://sdk-doc.blackmagicdesign.com/decklink-sdk/HighLevel/playback.html

### DeckLink IP / ST 2110 Sink

The first ST 2110 output target should be DeckLink IP hardware, not a full
software ST 2110 sender.

DeckLink IP cards expose SMPTE 2110 IP flows and SDP handling through the
DeckLink SDK:
https://sdk-doc.blackmagicdesign.com/decklink-sdk/HighLevel/ipflows.html

Full software ST 2110 output remains a later, separate project because it
requires PTP clocking, RTP packetization, ST 2110-21 timing, SDP generation,
multicast/network tuning, and likely NMOS IS-04/IS-05 registration and
connection management.

SMPTE describes ST 2110 as separate synchronized essence streams over IP for
real-time professional media:
https://www.smpte.org/standards/st2110

AMWA NMOS IS-04 provides discovery/registration of nodes, devices, senders,
receivers, sources, and flows:
https://specs.amwa.tv/is-04/

### NDI Sink

NDI output attaches one logical bus to one NDI sender instance. It must:

- expose one sender name per target assignment;
- send clean video and the bus audio without overlays;
- keep sender timing tied to the bus output frame sequence;
- support the same always-continuous behavior as hardware sinks;
- report sender active/error state and dropped/held-frame counters;
- use the full-quality NDI path where available rather than lower-bandwidth
  variants when the goal is broadcast mixer input quality.

NDI is a network output target, not a separate playback mode. Enabling NDI for a
feed must not cause an extra decode path or independent playhead.

### Future OMT Sink

OMT should follow the same target-assignment contract as NDI: one logical bus
feeds one OMT sender instance, with no independent timeline and no overlays.
The output bus remains the source of truth; the OMT sink only adapts frames and
audio into the OMT transport.

### Future AJA Sink

AJA card support should use the same hardware-sink contract as DeckLink:
enumerate devices/channels, validate modes, schedule continuous video/audio, and
report reference/device health. The logical bus contract must not contain
DeckLink-specific assumptions that would prevent AJA output later.

## Implementation Phases

### Phase 1: Backend Clean Bus Foundation

- Introduce backend `MediaVideoFrame` / `MediaAudioFrame` types.
- Split playback decode from Qt delivery.
- Build shared per-feed decoded media caches.
- Add `OutputFrameClock` with rational frame timing.
- Add `OutputBusEngine` for feed, PGM, and multiview buses.
- Add `OutputTargetManager` and a target-assignment model, even if only Qt
  preview consumes it initially.
- Make Qt preview consume output bus frames.

No DeckLink or ST 2110 output is required in this phase.

### Phase 2: Frame-Perfect Semantics

- Add tests for pause frame repetition.
- Add tests for step/jog updates while output remains continuous.
- Add tests for 1x feed audio, PGM audio-follow-selection, and silence in pause
  or non-1x modes.
- Add health counters for held video frames, placeholder frames, silence spans,
  and decode-cache misses.

### Phase 3: NDI Output

- Add NDI sender target assignments.
- Attach any logical bus to one or more NDI senders.
- Preserve shared playhead/frame-clock behavior.
- Report sender health and per-target counters.

### Phase 4: DeckLink SDI/HDMI Output

- Add DeckLink device discovery and mode validation.
- Attach one logical bus to one DeckLink SDI/HDMI output channel.
- Implement scheduled video and audio playout.
- Add device/reference/buffer health reporting.

### Phase 5: DeckLink IP / ST 2110 Output

- Attach logical buses to DeckLink IP sender flows.
- Surface SDP and flow status.
- Treat NMOS/controller integration as optional at first if the hardware can be
  configured externally.

### Phase 6: Future Output Adapters

- Add OMT using the network-sender target contract.
- Add AJA using the hardware-output target contract.
- Keep target assignment independent from logical bus generation.

### Phase 7: Quality Upgrade

The current recording format is MPEG-2 intra, 8-bit, 4:2:0. Transporting it over
DeckLink/ST 2110 is still uncompressed after decode, but it does not recover the
quality lost at recording.

For high-end broadcast use, add a mezzanine recording mode later:

- ProRes 422 or DNxHR where licensing/build constraints permit;
- FFV1 or another high-quality intra format for open workflows;
- 10-bit 4:2:2 preservation where ingest supports it.

## Testing

Unit tests:

- output frame clock emits exact frame indices for integer and fractional frame
  rates;
- pause repeats the same source frame on every output tick;
- step changes the selected source frame and then repeats it;
- feed outputs use their own audio at 1x;
- PGM audio follows selected feed;
- non-1x and paused audio are silence;
- missing video emits repeat-last or placeholder without stopping.

Integration tests:

- use a synthetic multi-feed MKV with distinct video markers and distinct audio
  tones per feed;
- drive play, pause, step, jog, shuttle, and feed selection;
- assert every logical output bus emits the expected frame sequence;
- assert audio tone routing for feed outputs and PGM;
- assert Qt preview receives the same backend frame identity as the output bus.

Hardware tests:

- DeckLink output starts, prerolls, and schedules continuously;
- unplug/device-loss is reported without crashing the app;
- reference lock state is reported;
- output never stalls when decode cache misses occur.

## Non-Goals

- No pass-through input-to-output routing.
- No overlays in broadcast output buses.
- No independent playheads per output.
- No SRT or RTMP output.
- No software-native ST 2110 sender in the first output milestone.
- No OMT or AJA output in the first output milestone.
- No scrub audio in pause, jog, reverse, or shuttle modes in the first output
  milestone.

## Risks

Moving Qt behind the backend output bus is a large architectural change. The
safe route is to first preserve current UI behavior through `QtPreviewSink`
while changing who owns the frames.

Multiple physical outputs may stress CPU conversion and memory bandwidth. The
logical bus API should allow shared buffers and later GPU or hardware-specific
zero-copy paths, but the first version should prioritize correctness and
testability.

DeckLink and ST 2110 timing are stricter than Qt preview timing. The output bus
must report held frames and underruns honestly instead of hiding them.
