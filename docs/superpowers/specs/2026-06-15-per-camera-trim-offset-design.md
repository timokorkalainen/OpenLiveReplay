# Per-Camera Trim Offset (PHASE-6) — Design

**Status:** approved (brainstorm) → spec
**Date:** 2026-06-15
**Branch:** `feat/per-camera-trim-offset`

## 1. Motivation

The frame-sync audit found the recorder has **no way to nudge one camera's timeline**
relative to the others (PHASE-6) — the single most-standard control on a replay
server. The PR #26 measurement harness then quantified the problem: with no shared
reference, the engine bakes in inter-camera arrival-latency differences verbatim
(`intercam_skew` measured ~250 ms baked in, stdev≈0). A per-camera trim is the
first, highest-leverage P0 fix: it lets an operator correct that residual skew by
hand while watching the multiview.

Scope is deliberately small: a per-source signed offset that shifts which captured
content lands on each output tick. It is **fine alignment**, not gross clock
recovery (multi-second offsets are a later roadmap item, P2).

## 2. Decisions (from brainstorm)

- **Units:** milliseconds (the recording timeline is ms-based; ms is the natural
  internal and UI unit).
- **Range:** clamped to **±500 ms** (covers the ~250 ms skew the harness measured,
  with headroom; bounded buffering cost).
- **Live:** adjustable **during recording**, applied each tick — an operator dials
  it in while watching, so it must take effect immediately (not only on reconnect).
- **A/V:** the offset shifts video **and** audio of that source together (shifting
  one without the other would break lip-sync).

## 3. Mechanism

The recording timeline (output file PTS) is driven by the global frame index, which
is identical across all sources (they mux into view-tracks on the same master
pulse). A per-source trim must change **which source content is selected** for a
given output tick, never **where that tick lands** in the file.

Two selection points in `StreamWorker::processEncoderTick`
(`recorder_engine/streamworker.cpp`):

1. **Video jitter gate** (streamworker.cpp:149): releases the newest queued frame
   with `sourcePts <= currentRecordingTimeMs − kJitterBufferMs`. The released frame
   is muxed at `m_latestFrame->pts = m_internalFrameCount` (the global tick,
   streamworker.cpp:179) — **independent of the gate**.
2. **Audio cursor** (`writeAudioForTick`, streamworker.cpp:1006): the write cursor
   `m_audioWriteCursor` runs on the **file** timeline; it reads source samples via
   `srcStart = start − jitterSamples` (streamworker.cpp:1042).

Applying a signed `trimMs` to both selection points (and nothing else) shifts the
source content uniformly while leaving the output frame index and audio write
cursor fixed:

| `trimMs` | video gate target | audio source-read (`srcStart`) | net effect |
|---|---|---|---|
| `+D` (delay)  | `recTime − jitter − D` → selects an **older** frame | `start − jitterSamples − Dsamp` → reads **older** samples | camera appears **later** |
| `−D` (advance) | selects a **newer** frame (freezes on last if it has not arrived) | reads **newer** samples (silence-fills past FIFO end) | camera appears **earlier** |

where `Dsamp = trimMs * kAudioSampleRate / 1000`.

**Why this is correct:** the output file PTS (frame index) and the audio write
cursor are untouched, so (a) the file timeline stays frame-exact, (b) cross-source
file alignment is preserved, (c) audio stays gapless on the file timeline, and (d)
video and audio of the trimmed source shift by the identical amount, so lip-sync
within the source is preserved.

**Edge behavior:**
- `+D` (delay) holds frames in the queue ~`D` ms longer (queue depth grows by up to
  ~500 ms ≈ 15 frames @30fps — a modest bounded memory increase) and reads audio
  from further back in the FIFO (which retains enough history).
- `−D` (advance) asks for content closer to "now". Within the ~200 ms jitter
  margin this works; **past it the source is asking for content that has not
  arrived yet** — video freezes/repeats and audio silence-fills, and because the
  capture-thread pre-drain gate also shifts, the queue can over-drain and the
  result is **erratic (judder), not a clean freeze** (e2e: a −250 ms advance on a
  250 ms-late source produced unstable offsets). Advance is therefore only
  dependable for small corrections within the jitter margin.
- **Recommended operator workflow:** to align a lagging camera, **delay the
  leading cameras (`+D`)** rather than advancing the laggard (`−D`) — you cannot
  show content that has not arrived, so delay is the robust direction. The e2e
  `intercam_trim` proof exercises the delay direction: a +250 ms trim on an
  otherwise-coincident source shifts its measured offset by ≈ −250 ms (untrimmed
  ≈ 0 → trimmed ≈ −250), i.e. the trim delays the source by the set amount.
- **Changing** the offset live causes a one-time discontinuity in the source-read
  position (a small audio click / a single frame jump at the instant of change).
  This is acceptable for a set-and-leave trim that is nudged a few times during
  alignment and then left. Documented, not smoothed in v1.

## 4. Components & data flow

| Unit | Change | Responsibility |
|---|---|---|
| `settingsmanager.h/.cpp` | add `int trimOffsetMs = 0;` to `SourceSettings`; (de)serialize in `config.json` | persistence |
| `recorder_engine/streamworker.h/.cpp` | `std::atomic<int> m_trimOffsetMs{0}` + `setTrimOffsetMs(int)` (clamped ±500); apply in the video gate (:149) and `writeAudioForTick` (:1042, incl. the corresponding FIFO-trim and the `track<0` drop path) | apply the trim |
| `recorder_engine/replaymanager.h/.cpp` | pass initial trim at worker creation; `updateSourceTrim(int sourceIndex, int ms)` → `worker->setTrimOffsetMs(ms)` | route trim to the right worker |
| `uimanager.h/.cpp` | `Q_INVOKABLE void setSourceTrimOffset(int idx, int ms)` (persist + route live) and `Q_INVOKABLE int sourceTrimOffset(int idx) const`; a `sourceTrimVersion` counter + `NOTIFY` for QML rebind, mirroring the `sourceEnabled`/`sourceConnection` pattern | expose to QML, persist |
| `Main.qml` | a compact ms `SpinBox` (from −500 to 500, step 33) in the source-row delegate, next to the connection dot; `onValueModified` → `uiManagerRef.setSourceTrimOffset(index, value)` | operator control |

**Data flow (live edit):** SpinBox → `UIManager::setSourceTrimOffset(idx, ms)` →
clamps, writes `m_currentSettings.sources[idx].trimOffsetMs`, saves config, calls
`ReplayManager::updateSourceTrim(idx, ms)` → `m_workers[idx]->setTrimOffsetMs(ms)`
(atomic; the capture/tick thread reads it next tick). Mirrors the existing live
`updateUrl`/`setSourceMetadata` paths.

**Data flow (recording start):** `ReplayManager::startRecording` already constructs
one `StreamWorker` per source; pass `m_sourceTrims[s]` into the worker (new ctor
arg or a `setTrimOffsetMs` call right after construction, matching how
`setSourceMetadata` is seeded).

## 5. UI

The source-row delegate (`Main.qml`, the `streamList` `Repeater`) already holds:
index · ON/OFF · connection dot · ID · Name · Metadata · URL · ⚠dup · ×. Insert a
labelled `SpinBox` ("trim" + ms) after the connection dot:

```qml
SpinBox {
    from: -500; to: 500; stepSize: 33
    value: appWindow.uiManagerRef.sourceTrimVersion >= 0
           ? appWindow.uiManagerRef.sourceTrimOffset(streamRow.index) : 0
    editable: true
    Layout.preferredWidth: 110
    onValueModified: appWindow.uiManagerRef.setSourceTrimOffset(streamRow.index, value)
    ToolTip.text: "Timeline trim (ms): + delays this camera, − advances it"
}
```

(Exact width/label refined in the plan; `stepSize: 33` ≈ one frame at 30 fps while
keeping the stored value in ms.)

## 6. Error handling / bounds

- `setTrimOffsetMs` and `setSourceTrimOffset` **clamp** to [−500, 500]; out-of-range
  config values load clamped.
- A trim on a source not currently mapped to a view is stored and applied when it is
  mapped (the worker holds the value regardless of `m_viewTrack`).
- No change to the discontinuity/reconnect logic: on re-anchor the trim still
  applies at the gate, so a reconnecting camera keeps its operator-set trim.

## 7. Testing

- **Unit:** `setTrimOffsetMs` clamping; `SourceSettings` round-trips `trimOffsetMs`
  through save/load (extend `tst_settingsmanager`).
- **E2E (the proof):** `sync_harness` gains a `--trim <ms>` flag that, after
  `startRecording()`, calls `rm.updateSourceTrim(n−1, ms)` to trim the **last**
  configured source (view 1 in the 2-source skew scenario; the only source, view 0,
  in single-source `lipsync`) — using the same `ReplayManager` API the live UI path
  uses. Add a
  `intercam_trim` scenario to `run_sync_e2e.sh` (built in PR #26): it runs the exact
  two-source skew setup as `intercam_skew` but passes `--trim <T>` (default the
  injected skew, e.g. 250), then asserts the measured `intercam_offset_ms` moved
  toward zero by ≈ `T` vs the untrimmed `intercam_skew` number (the scenario prints
  both so the correction is visible). It also runs once with a non-zero trim through
  the `lipsync` measurement to confirm the A/V offset is **unchanged** (lip-sync
  preserved under a trim). These stay report-only (`sync-report` label).
- **Manual:** in-app, set a trim on a live source and confirm the multiview shifts.

## 8. Out of scope (YAGNI)

- Auto-alignment / measuring the skew for the operator (a later step).
- Sub-frame interpolation or click-free live retiming (the one-time adjustment click
  is acceptable).
- Gross (multi-second) offset / clock recovery (P2).
- Per-source trim for >2 view layouts beyond what the existing source list supports
  (the control is per-source, so it already covers all sources).

## 9. Risks

- **Positive-offset buffering:** a +500 ms trim deepens that source's frame queue by
  ~15 frames. Bounded and per-source; negligible against the existing decode buffers.
  Noted, not mitigated.
- **Audio FIFO history:** reading `srcStart − Dsamp` requires the FIFO to retain that
  much history. The FIFO already buffers ≥ jitter; the plan must confirm it retains
  ≥ `jitter + 500 ms` (or the advance/delay silently silence-fills). The e2e
  `lipsync`-with-trim check catches a regression here.
- **Synthetic-localhost verification noise:** as documented for PR #26, the harness
  numbers vary run-to-run; the `intercam_trim` assertion compares the *shift* (trim
  applied − baseline) which is far more stable than either absolute number.
