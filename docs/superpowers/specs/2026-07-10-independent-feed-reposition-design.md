# Independent Feed Reposition Design

## Problem

During live-follow startup, a reposition can repeatedly seek to an early target such as 40 ms.
The reposition fill loop currently stops when `newestPtsMin()` reaches its fill target. That helper
deliberately ignores empty or stalled tracks for steady playback, so an interleaved recording can
stop filling after only the first feeds have decoded. Multiview then applies an all-feed coverage
gate, rejects the partial fill, seeks back to the same anchor, and repeats forever. The output holds
old blue frames while the loop consumes a CPU core.

Captured telemetry reproduced this with four feeds: feeds 0 and 1 reached 80 ms after 20 packets,
feeds 2 and 3 remained empty, and every retry restarted from zero before their packets arrived.

## Design

Feeds remain independent. A late feed must not prevent another feed from committing or force a
global rewind.

- Multiview uses feed 0 as the timeline reference. It does not require every feed to cover a seek
  target before committing.
- Single-view uses the selected feed as the timeline reference.
- Reposition fill completion uses the reference feed, rather than the steady-playback
  `newestPtsMin()` aggregate. It keeps reading until that feed reaches the fill target, EOF, the
  packet budget, interruption, or a superseding seek.
- A successful reposition merges every frame decoded so far and leaves the shared demuxer at its
  current forward position. Normal fill then continues through the remaining interleaved packets,
  allowing each other feed to populate independently without another seek.
- The existing all-feed mode remains available for internal callers and tests. When explicitly
  enabled, reposition fill waits for every feed instead of using the reference feed.
- Existing packet-budget, generation, GPU freshness, and operator-seek checks remain unchanged.

## Components

`UIManager` will stop enabling all-feed seek coverage for multiview. `PlaybackWorker` will add a
small reposition-fill readiness helper that chooses feed 0 when no feed is selected, the selected
feed in single-view, or every feed only when the legacy all-feed flag is explicitly enabled.

No output-rendering changes are needed. `OutputBusEngine` already resolves each multiview cell from
its own feed cache and emits a placeholder only for the individual feed that has no frame.

## Error Handling

If the reference feed cannot reach the target before EOF or the packet budget, the existing retry
path remains active. A missing non-reference feed does not cause a retry. Superseding seeks,
interruptions, stale GPU generations, and operator PGM transaction rules retain their current
behavior.

## Testing

- A focused playback-worker unit test will prove that a selected late feed keeps reposition filling
  after earlier feeds have reached the target.
- A multiview unit case will prove that feed 0 can commit while other feeds are still empty.
- Existing coverage-mode tests will continue to prove that explicitly requested all-feed behavior
  waits for every feed.
- The playback-worker unit target, relevant playback E2E scenarios, and the Windows debug build will
  be run before launching the repaired app for manual reproduction.
