# Timecode Carrier Epoch and Accepted-Packet Design

## Goal

Bind every source-timecode observation and recording start-timecode candidate to the exact frame
carrier and encoded packet that produced it, including delayed MPEG-2, native CPU, and GPU output.
Source replacement, disconnect, fallback, and shutdown must make every older carrier permanently
stale even when producer generation counters restart.

## Carrier authority

`StreamWorker` owns a monotonically increasing carrier epoch and an immutable shared carrier token.
The token contains both the ingest-session identity and carrier epoch. Every invalidation creates a
new token; tokens are never mutated after publication. An ingest callback captures its immutable
session identity. At each frame entry it may snapshot the current carrier token only while that
token still has the captured session identity, so a callback from a replaced or disconnected
session cannot stamp itself with the new epoch.

GPU fallback swaps the active token to a new epoch while retaining the live ingest-session identity.
Subsequent CPU submissions therefore snapshot the new token when they enter StreamWorker's
queued/latest state; in-flight GPU/native submissions retain their old immutable token and remain
stale. `QueuedFrame`, CPU/GPU latest-frame state, and `DecodedFrameEvidence` carry the token's epoch
snapshot.

The existing bounded `DecodedFrameEvidenceQueue` remains the only packet-evidence mapping. Native
CPU, GPU, and MPEG-2 submissions enqueue by the opaque PTS given to the encoder. Output takes by the
actual packet PTS before any mux time-base rescale. Missing, duplicate, and out-of-order output keep
the queue's existing bounded semantics.

## Completion and ReplayManager validation

Native/GPU packet callbacks and MPEG-2 received packets use one StreamWorker packet-write helper.
It takes the mapped evidence by actual packet PTS, submits the packet to `Muxer`, and emits evidence
only after a successful asynchronous write while the carrier epoch is still current.

`frameTimecode` carries the immutable carrier epoch alongside the copied `TimecodeEvidence`.
`ReplayManager::onFrameTimecode` validates that epoch against the source worker's current atomic
epoch at queued-event receipt. This second validation closes the race where an epoch changes after
StreamWorker's completion check but before Qt delivers the queued event. With no live worker, the
unit-test seam accepts only its explicit test epoch policy.

## Accepted-packet start timecode

Per-frame StreamWorker calls to `Muxer::setStartTimecodeCandidate` are removed. A packet write may
carry an optional start-timecode candidate. `Muxer::writePacket` clones and capacity-checks the
packet first. Only after the packet is accepted does it record the first valid accepted candidate
under `m_qMutex`, in the same critical section and order as the queue push.

The writer snapshots the first accepted candidate under `m_qMutex`, releases that mutex, and only
then publishes the candidate under `m_headerMutex`. It also calls `headerWriteDeferred` and
`ensureHeaderWritten` only while `m_qMutex` is released. The producer never takes
`m_headerMutex`. This preserves a single lock order and makes an accepted candidate visible before
the writer can commit the deferred header. Explicit start metadata passed to `init` remains allowed
to preexist and remains first-wins.

An earlier rejected packet never touches candidate state. Among accepted candidates, queue
acceptance order wins. An earlier accepted packet without a candidate does not prevent a later
accepted candidate inside the header grace window from winning.

## Reset behavior

Disconnect, non-empty or empty URL replacement, source-session replacement, GPU fallback, and stop
swap the active carrier token and clear the bounded packet-evidence queue. Identity/rate or explicit
discontinuity changes do the same before the new frame enters queued/latest state. Old queued/latest
frames are discarded by epoch comparison even if they survive a race with invalidation.

No mutex is held across encoder calls, muxer queue waits, disk writes, callbacks, or Qt signal
delivery. The frame queue, carrier token, evidence queue, mux queue, and mux header each keep their
existing short leaf critical sections.

## Verification

Tests must prove:

- delayed MPEG-2 output binds to its input PTS despite a newer frame and is suppressed by a reset
  after packet/evidence matching;
- old callbacks/queued/latest carriers are rejected across non-empty URL replacement, disconnect,
  fallback, and stop, including restarted producer generations;
- a queued `frameTimecode` event is rejected when the worker epoch changes before receipt;
- a production-shaped `GpuEncodePump` plus real `Muxer` maps delayed old PTS and suppresses in-flight
  output after reset/fallback;
- rejected packet candidates cannot win, accepted candidates obey acceptance order, and a later
  accepted candidate inside the grace window can follow an earlier candidate-less packet.

All Windows test execution uses the reviewed guarded launcher; raw test executables are forbidden.
