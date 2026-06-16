# Decouple the recording heartbeat from fps — Design

**Status:** approved (brainstorm 2026-06-17)
**Roadmap item:** framesync P0 (last one) — "decouple the heartbeat timer from fps" (see
`broadcast-framesync-roadmap` memory). Prerequisite that smooths the path to P1 (rational fps).
**Base branch:** `feat/heartbeat-decouple`, off `origin/main` (includes #41/#43/#46).

## Goal

Make the recording heartbeat (the master-pulse scheduler) fire at a **fixed, fps-independent
cadence** instead of `int(1000/fps)`. The engine's recording timeline is already wall-clock-derived,
so this removes the last integer-fps dependency from the *scheduler* itself — the single thing P1
(replace int fps with `AVRational`) would otherwise be unable to express for a fractional rate like
29.97.

## Background (current architecture)

The heartbeat is a `QTimer` (`m_heartbeat`, `replaymanager.cpp:11-12`) started at
`int(1000.0/m_fps)` ms (`replaymanager.cpp:250-251`). Its slot `onTimerTick`
(`replaymanager.cpp:362-392`) does **not** count ticks — it reads the wall-clock
(`RecordingClock::elapsedMs()`), derives the target frame `derivedFrame = (elapsedMs*m_fps)/1000`,
returns early if the frame count hasn't advanced, and otherwise emits one `masterPulse(f, frameMs)`
**per frame** (with `frameMs = (f*1000)/m_fps`), draining catch-up for late ticks:

```cpp
int64_t from = m_globalFrameCount + 1;
if (derivedFrame - from >= m_fps) from = derivedFrame - m_fps + 1; // skip if >1s behind
const int64_t maxPerTick = qMax(1, m_fps / 4);                     // drain a few per tick
const int64_t to = qMin(derivedFrame, from + maxPerTick - 1);
for (int64_t f = from; f <= to; ++f) { m_globalFrameCount = f; emit masterPulse(f, (f*1000)/m_fps); ... }
```

So `frameIndex` is wall-clock-derived; downstream (`StreamWorker::onMasterPulse` →
`processEncoderTick`, file-time `(frameIndex*1000)/fps`, audio cursor, encoder PTS = frameIndex on
`{1,fps}`) consumes the emitted frames. The **only** fps coupling in the heartbeat is:

1. **Timer interval** `int(1000/fps)` — fps-derived and integer-truncated (33 ms at 30fps, not
   33.33; cannot express 29.97).
2. **Catch-up cap** `maxPerTick = m_fps/4`.

The frame→ms math (`f*1000/fps`), encoder/muxer timebase (`{1,fps}` → rescaled to `{1,1000}`), and
output frame-rate metadata are integer-fps but are **P1's** concern, not the heartbeat's.

## Design

### 1. Fixed scheduler interval

Add `static constexpr int kHeartbeatIntervalMs = 8;` (≈125 Hz) to `ReplayManager`. In
`startRecording`, start the timer at `kHeartbeatIntervalMs` instead of `int(1000/fps)`. The timer is
now a pure scheduler whose rate is independent of fps. 8 ms is finer than any supported frame
interval (≤60 fps = 16.6 ms), so each output frame gets ≥1 tick without catch-up under normal load;
the `derivedFrame <= m_globalFrameCount` early-return makes the surplus wakes cheap no-ops (one
clock read + compare) — only ~fps ticks/sec actually emit.

### 2. fps-independent catch-up cap

Add `static constexpr int kMaxFramesPerTick = 8;` and use it in place of `qMax(1, m_fps/4)`. It
drains a post-stall backlog over a few ticks without freezing the GUI thread (at 8 ms/tick, 8
frames/tick = ~1000 fps drain, so a 1 s backlog clears in ~4 ticks). The 1-second backlog *skip*
(`derivedFrame - from >= m_fps`) is kept verbatim — it is a *time* bound (one second of frames),
correct at any scheduler rate, and still legitimately expressed in frames-per-second.

### 3. Pure, testable frame-span helper

Extract the derived-frame + catch-up-bounds computation from `onTimerTick` into a pure free function
in `recorder_engine/heartbeat.{h,cpp}` (new, tiny):

```cpp
struct FrameSpan { int64_t from = 1; int64_t to = 0; }; // empty when to < from
// Frames to emit this tick: derive the target frame from wall-clock, advance from
// lastFrame, skip ahead if more than maxBacklogFrames behind, cap at maxPerTick.
FrameSpan heartbeatFrameSpan(int64_t elapsedMs, int fps, int64_t lastFrame,
                             int maxPerTick, int64_t maxBacklogFrames);
```

`onTimerTick` becomes: read `elapsedMs`, call `heartbeatFrameSpan(elapsedMs, m_fps,
m_globalFrameCount, kMaxFramesPerTick, m_fps)`, and loop `for (f = span.from; f <= span.to)` emitting
`masterPulse(f, (f*1000)/m_fps)` + `writeBlueFrames` exactly as today. This isolates the timeline
math behind one well-tested seam — and is the single place P1 later swaps integer `fps` for an
`AVRational` boundary computation.

`heartbeat.{h,cpp}` is added to the `olr_test_core` CMake target (pure Qt-Core, no FFmpeg), so the
helper is unit-testable without the engine.

## Error handling / edge cases

- **No advance** (`derivedFrame <= lastFrame`): helper returns an empty span (`to < from`);
  `onTimerTick` emits nothing — same as today's early return.
- **Late tick / catch-up:** helper returns `from..to` with `to-from+1 <= maxPerTick`; remaining
  backlog drains on subsequent ticks (unchanged behavior).
- **Long stall (>1 s behind):** helper jumps `from` to `derivedFrame - maxBacklogFrames + 1`, so the
  recording resumes near real time instead of replaying a huge backlog (unchanged behavior).
- **fps clamp:** `m_fps` is set via `setFps` and defaults to 30; the helper guards `fps <= 0`
  (returns empty) to avoid divide-by-zero, mirroring the engine's existing assumption that fps > 0.

## Testing

1. **Unit — `heartbeatFrameSpan`** (`tests/unit/tst_heartbeat.cpp`, `olr_test_core`):
   not-yet-advanced → empty; exactly one new frame → single-frame span; a tick 5 frames late →
   span capped at `maxPerTick`; >maxBacklog behind → `from` skips ahead to
   `derivedFrame-maxBacklog+1`; `fps<=0` → empty; the 30fps mapping (`elapsedMs=100` → frame 3).
2. **E2e — no regression** (`run_record_e2e.sh`): records 6 s, asserts the video packet count is in
   the expected band (~fps×duration) and audio/video end timestamps agree within 0.75 s. The output
   frame count is wall-clock×fps (unchanged by the scheduler rate), so this passes; it confirms the
   fixed 8 ms scheduler produces the same recording. Run `e2e_record_stereo` + `e2e_record_mono`.

## Files touched

- `recorder_engine/heartbeat.{h,cpp}` (new) — `FrameSpan` + pure `heartbeatFrameSpan`.
- `recorder_engine/replaymanager.{h,cpp}` — `kHeartbeatIntervalMs`/`kMaxFramesPerTick`; start the
  timer at the fixed interval; `onTimerTick` delegates the span math to the helper.
- `tests/unit/tst_heartbeat.cpp` (new) + `tests/unit/CMakeLists.txt` — the helper's unit test.
- `tests/CMakeLists.txt` — add `heartbeat.cpp` to `olr_test_core`.

## Success criteria

- The heartbeat timer interval is a fixed `kHeartbeatIntervalMs`, with no `1000/fps` anywhere in the
  timer setup; the catch-up cap is fps-independent.
- `heartbeatFrameSpan` is pure and unit-tested (catch-up, backlog-skip, no-advance, fps guard).
- `run_record_e2e.sh` (`e2e_record_stereo`/`e2e_record_mono`) and `ctest -L unit` pass — the
  recording's frame count + A/V sync are unchanged.
- The integer-fps frame math remains untouched (left for P1), but it now lives behind the single
  `heartbeatFrameSpan` seam.
