# Output Continuity Soak — Design

**Date:** 2026-06-18
**Status:** Approved (rung 1 of the output-stability realism ladder)

## Goal

Provide a long-running, opt-in regression guard that proves the broadcast **output bus**
produces a structurally frame-perfect, drift-free stream and holds cadence over wall-clock
minutes. It soaks exactly the code hardened in PR #71 (`OutputFrameClock`,
`OutputBusEngine`, `OutputDispatcher`, the multiview memo, frame identity, the rational
audio-sample math) plus the `OutputRuntime` cadence thread, and fails on any continuity,
drift, deadline, or paused-output regression.

## Honest scope — what this does and does NOT test

This is **rung 1** of a realism ladder. It tests the output *producer*, not the delivered
output. Explicit non-goals (deliberately out of scope here):

- **No real decoded media.** Input is a static synthetic `OutputFrameCache`, not the
  FFmpeg decode → cache path. No real frame-arrival jitter or decode stalls.
- **No real sinks.** A test `ContinuitySink` records frames; this does not exercise
  `NdiOutputSink` (real transport), `QtPreviewSink` (real `QVideoFrame` conversion), or
  `QueuedOutputSink` under load.
- **No external consumer / content check.** It verifies structural continuity (indexes,
  sample tiling, counters), not the pixels/audio a downstream receiver sees, nor A/V sync
  at the far end.
- **No live backpressure phase.** Degraded-not-Error, drop counting, and recovery remain
  covered by the deterministic unit tests shipped in PR #71.

The end-to-end "are our outputs glitch-free and A/V-synced" question is **rung 5** — a real
source → worker → `NdiOutputSink` → external NDI-receiver soak — and is the separately
specced next workstream (the roadmap's required NDI validation lane). This spec does not
implement it.

## Architecture

A standalone wall-clock binary plus a driver script and an opt-in CTest label. **No
production code changes** — the harness uses only public `OutputRuntime` / `OutputFrameCache`
/ `IOutputSink` APIs (the same surface `tst_outputruntime` already uses).

### Components

- **`tests/e2e/soak_harness.cpp`** — the binary. Builds a static cache, wires an
  `OutputRuntime`, registers sinks, runs the phase schedule over a configurable duration,
  and prints one parseable report line.
- **`ContinuitySink`** (defined in the harness) — an `IOutputSink` that computes O(1)
  running invariants per submitted frame and never blocks the cadence thread:
  - tracks `lastOutputFrameIndex`, counting any index gap (`expected != actual`);
  - tracks audio tiling: `lastAudioEndSample`, counting any seam
    (`frame.audio.startSample != lastAudioEndSample`) once streaming has started;
  - counts placeholder frames and frames whose identity `samePayloadAs` the previous
    (repeated payload);
  - records total frames and a per-bus breakdown (feed bus and multiview bus).
- **Static cache builder** — pre-loads small synthetic YUV420P video frames (one per
  source frame period) and matching S16 stereo audio for `feedCount` feeds, covering the
  configured duration (capped at a memory bound; past the end the nearest-≤ lookup holds
  the last frame and audio resolves to silence — still continuous).
- **Snapshot provider** — returns the static cache (by shallow COW copy, mirroring
  `makeOutputSnapshot`) plus a `PlaybackStateSnapshot` whose playhead advances
  monotonically with wall-clock during play phases and holds during the paused phase.

### Phase schedule

Driven by `OLR_SOAK_SECONDS` (default 120; a small value, e.g. 3, gives a fast CI
self-check). The output frame rate is configurable via `OLR_SOAK_FPS_NUM`/`OLR_SOAK_FPS_DEN`
and **defaults to 30000/1001 (29.97)** so the run soaks the rational audio-sample math at
scale. The schedule alternates several **steady 1×** and **paused** segments (e.g. 5
segments over the duration), so multiple play epochs are exercised and the audio tiling is
validated continuously across resumes:

1. **steady 1×** — `playing=true, speed=1.0`; the output clock advances the sampled playhead
   from the anchored epoch; per-frame audio spans must tile exactly.
2. **paused** — `playing=false`; playhead holds at an in-range value; output keeps ticking
   (frozen video + silent audio).
3. repeat steady/paused for the remaining segments.

Note: the *deterministic* odd-epoch audio-seam case (the exact PR #71 bug trigger) is already
covered by the unit test `ntscAudioSpansStayContiguousAcrossOddPlayEpoch`. The soak adds the
over-time / cadence / leak dimension and validates tiling across many real resumes; it does
not need to force a specific epoch parity (`OutputRuntime` resets the frame index to 0 on
start and ticks continuously, so epoch parity isn't pinnable via the public API — and a
production change for it is out of scope).

The `OutputRuntime` runs its real cadence thread throughout; the harness samples
`outputStats()` and the sinks at the end (and may log periodic progress to stderr).

## Invariants and thresholds

Emitted as a single parseable line consumed by the driver script, which asserts:

- **Frame continuity:** every bus's `outputFrameIndex` increments by exactly 1 with **zero
  gaps** across the whole run (`indexGaps == 0`).
- **Audio continuity:** within each continuous play run, audio spans tile exactly —
  `start[N] == start[N-1] + count[N-1]` — with **zero seams** (`audioSeams == 0`). Exact
  tiling *is* the no-drift guarantee at any rate (the per-frame count is derived as
  `nextStart − start`), so no separate drift metric is needed. The tiling baseline resets
  across a pause (silent audio) and re-establishes on resume; seams are only counted between
  consecutive non-silent (playing) frames.
- **Cadence:** `runtime.deadlineMisses == 0` and `runtime.maxLatenessNs` below a generous
  bound (e.g. one frame period) during steady phases.
- **Paused continuity:** during the paused segment, indexes stay gap-free, repeated-payload
  is detected (`repeatedPayloadFrames > 0`), and **no** placeholder frames appear while the
  source is present (`placeholderFrames == 0`).
- **Liveness:** total frames delivered ≈ `durationSeconds × fps` within a tolerance, so a
  silently-stalled runtime fails rather than trivially passing.

### Report format

```
SOAK bus=feed frames=<n> indexGaps=<n> audioSeams=<n> placeholders=<n> repeated=<n>
SOAK bus=multiview frames=<n> indexGaps=<n> audioSeams=<n> placeholders=<n> repeated=<n>
RUNTIME deadlineMisses=<n> catchUpCapHits=<n> maxLatenessNs=<n> ticks=<n>
```

## Driver script and CI

- **`tests/e2e/run_output_soak.sh`** — runs `soak_harness`, greps the report, and exits
  non-zero on any violated invariant (mirrors `run_playback_e2e.sh`).
- **CTest:** registered as `e2e_output_soak` under an **opt-in label** `soak`, excluded
  from the default and pre-push gates (which already use `-LE` exclusions). CI runs it only
  when explicitly selected (`ctest -L soak`); the default run sets `OLR_SOAK_SECONDS` small
  for a fast smoke when invoked, while local/nightly soaks use the multi-minute default.

## Deferred review test-gaps (same PR)

Three deterministic unit tests left open by the PR #71 adversarial review, unrelated to the
soak binary but closing the review loop:

- **[10]** `tst_outputdispatcher`: play-epoch re-anchoring on speed change and
  reverse/shuttle playback, driven through the dispatcher.
- **[11]** `tst_queuedoutputsink`: restart (stop → start) state reset, and worker drain
  ordering under rapid stop.
- **[12]** `tst_broadcastoutputsettings`: a feed whose `lastIdentity.videoPlaceholder` is
  true maps to `Degraded` end-to-end.

## Testing approach

- The harness has a fast self-check mode (`OLR_SOAK_SECONDS=3`) used to develop the
  assertions TDD-style: first confirm the report parses and the invariants hold against the
  known-good output bus, then confirm a deliberately corrupted expectation fails.
- The three unit test-gaps follow strict red→green TDD.
- Full output unit suite + `e2e_play_*` remain green; the new soak is additive and opt-in.

## File summary

- Create: `tests/e2e/soak_harness.cpp`, `tests/e2e/run_output_soak.sh`.
- Modify: `tests/e2e/CMakeLists.txt` (new target + `e2e_output_soak` test under label `soak`).
- Modify: `tests/unit/tst_outputdispatcher.cpp`, `tests/unit/tst_queuedoutputsink.cpp`,
  `tests/unit/tst_broadcastoutputsettings.cpp` (the three test-gaps).
- No production source changes.
