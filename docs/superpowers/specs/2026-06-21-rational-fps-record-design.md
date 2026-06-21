# Rational fps through encode/mux + drop-frame timecode (T3.2) — Design

**Date:** 2026-06-21
**Status:** Approved phase (broadcast-readiness roadmap T3.2) — grounded against `main` @ `db8bcfc`.

## Summary

A 29.97 / 59.94 recording currently advertises an **integer** frame rate end-to-end: the muxer stamps
`avg_frame_rate = r_frame_rate = {fps, 1}` and the encoder is configured `time_base = {1, fps}`,
`framerate = {fps, 1}`, so a 30000/1001 source is recorded as **30/1** in both the elementary stream and
the container — the file lies about its rate. The UI timecode is also integer-fps and non-drop. This phase
threads the **rational** rate (`fpsNum/fpsDen`, already in settings) through the encoder + muxer so the
recorded file advertises the true rate, and wires the operator timecode display to the existing
`olr::Timecode` **drop-frame** math.

This is the record-side counterpart to the output-side fix (T1.2), which already made `PlaybackWorker`
build its output `FrameRate` from `m_transport->frameRate()` (rational).

## Grounding (current code)

- **Settings already carry the rational rate.** `SettingsManager` has `fpsNum`/`fpsDen`
  (`settingsmanager.h:37-38`), normalized via `FrameRate::fromFraction` on load
  (`settingsmanager.cpp:235-241`), so 29.97 is stored as 30000/1001. `UIManager` already feeds them to the
  transport (`uimanager.cpp:376`).
- **The encoder is integer.** `StreamWorker::setupEncoder` sets `encCtx->time_base = {1, m_targetFps}` and
  `encCtx->framerate = {m_targetFps, 1}` (`streamworker.cpp:612-613`), and the native encoder config is
  built `{m_targetWidth, m_targetHeight, m_targetFps, 1, 30'000'000}` (`streamworker.cpp:551`) — the `1`
  is the fps denominator. `m_targetFps` is an `int` (`streamworker.h:216`), set from a `targetFps` ctor arg
  (`streamworker.cpp:49`). The existing MPEG-2 warning (`streamworker.cpp:596-609`) already notes the
  container/ES rate mismatch for non-integer rates.
- **The muxer is integer.** `Muxer::init(..., int fps, ...)` sets `st->avg_frame_rate = {fps, 1}` and
  `st->r_frame_rate = {fps, 1}` (`muxer.cpp:136-137`). Packet PTS uses `time_base = {1, 1000}` (ms,
  `muxer.cpp:133,466,494`) — **independent of the advertised rate**, so changing the rate fields is pure
  metadata, with no effect on timing.
- **The DF timecode lib exists but is unused by the UI.** `olr::Timecode`
  (`recorder_engine/timing/timecode.h`): `TimecodeRate{num,den}`, `isDropFrameRate(rate)` (true for the
  29.97/59.94 1001-families), `framesToTimecode(frameIndex, rate, dropFrame)` → `HH:MM:SS:FF` (NDF) or
  `HH:MM:SS;FF` (DF). The UI instead hand-rolls `frames = (ms % 1000) / (1000.0/fps)` with integer `fps`
  and no drop-frame (`uimanager.cpp:2662-2691` `formatTimecodeForFile`/`formatTimecodeForDisplay`,
  `:2717,2720` `updateXTouchDisplay`).

## Design

### Part 1 — Rational rate through the encoder (StreamWorker)

- Add `m_targetFpsNum`/`m_targetFpsDen` to `StreamWorker` alongside the existing `m_targetFps` (keep
  `m_targetFps` = rounded integer for the unchanged internal ms-cadence math — see Scope boundary).
  Plumb them through the `StreamWorker` ctor (new params, defaulted) from `ReplayManager`.
- **MPEG-2 (`setupEncoder`):** `encCtx->time_base = {fpsDen, fpsNum}`, `encCtx->framerate = {fpsNum,
  fpsDen}` (`streamworker.cpp:612-613`). MPEG-2's sequence header can signal the 1000/1001 variants, so
  29.97/59.94 now write the correct ES rate. Update the "exactly representable" warning switch to accept
  the rational 30000/1001 & 60000/1001 (warn only on genuinely non-representable rates).
- **Native H.264 (config at `streamworker.cpp:551`):** pass `{w, h, fpsNum, fpsDen, bitrate}` instead of
  `{w, h, fps, 1, bitrate}` (the config struct already has `fpsNum`/`fpsDen`,
  `nativevideoencoder.h:25`).

### Part 2 — Rational rate through the container (Muxer)

- Thread `fpsNum/fpsDen` into `Muxer::init` (extend the canonical overload; keep the existing `int fps`
  callers working by deriving an integer where only one is needed, or add rational params with sensible
  defaults). Set `st->avg_frame_rate = {fpsNum, fpsDen}` and `st->r_frame_rate = {fpsNum, fpsDen}`
  (`muxer.cpp:136-137`). No change to `time_base` (stays ms) or packet PTS.

### Part 3 — Drop-frame timecode display (UIManager)

- Replace the hand-rolled frame math in `formatTimecodeForDisplay` and `updateXTouchDisplay` (and
  `formatTimecodeForFile`) with `olr::Timecode`: convert the playhead ms to an absolute frame index
  (`frame = llround(ms * fpsNum / (1000.0 * fpsDen))`) and call
  `olr::Timecode::framesToTimecode(frame, {fpsNum, fpsDen}, isDropFrameRate(...))`. This yields a proper
  `HH:MM:SS;FF` drop-frame label for 29.97/59.94 and `HH:MM:SS:FF` otherwise. Use `m_currentSettings`
  `fpsNum/fpsDen` (already present). The file-name timecode keeps its separator-less form (strip the lib's
  separators) so existing filenames are unaffected in shape.

## Scope boundary (explicit)

- **Do NOT change the internal recording cadence / timing math** that uses integer `m_targetFps`
  (`streamworker.cpp:131,165,257-259,404,740`): those compute ms windows / synthetic backstop / audio
  cursor and are **arrival-anchored** recording internals, not the advertised rate. The recorded PTS are
  millisecond-precision and arrival-timestamped; the advertised `avg_frame_rate`/ES rate is metadata read
  by players for display/timecode. Fixing the metadata + display is the T3.2 goal; reworking the
  arrival-anchored cadence is a separate, higher-risk change and is **out of scope**. `m_targetFps` stays
  as the rounded integer for those sites.
- **No change to packet PTS / `time_base`** (stays `{1,1000}`), so recordings remain timing-identical;
  only the advertised rate fields and the UI label change.

## Tests

- **Unit (`olr::Timecode`):** already covered by `tst_timecode.cpp`; add a UI-adapter test only if the
  ms→frame conversion lives in a testable helper.
- **Unit (muxer rate):** extend `tst_muxer` to assert that a 30000/1001 init writes
  `avg_frame_rate == 30000/1001` (and integer rates unchanged) on the video stream(s).
- **Unit (UI TC):** a focused test that `formatTimecodeForDisplay`/the frame-index helper produces
  `HH:MM:SS;FF` drop-frame output for 29.97 and `HH:MM:SS:FF` for 30 (extract the conversion into a
  free/static helper so it is unit-testable without a full UIManager).
- **e2e:** extend an existing record gate (or add a focused one) to ffprobe the recorded MKV and assert
  `r_frame_rate=30000/1001` for a `--fps 30000/1001` (or `--fps-num/--fps-den`) recording — proving the
  container advertises the true rate end to end. (Record harness fps arg may need a rational form.)

## Files

- `recorder_engine/streamworker.{h,cpp}` — rational encoder rate + native config; new ctor params.
- `recorder_engine/muxer.{h,cpp}` — rational `avg_frame_rate`/`r_frame_rate`; `init` rational params.
- `recorder_engine/replaymanager.cpp` — pass `fpsNum/fpsDen` from settings to StreamWorker + Muxer.
- `uimanager.cpp` — drop-frame TC display via `olr::Timecode`.
- `tests/unit/tst_muxer.cpp` (rate), a UI-TC unit test, and an e2e ffprobe rate assertion.
- Possibly `tests/e2e/record_harness` + driver if a rational `--fps` is needed for the e2e.

## Verification

- `ctest -L unit` (full suite — recorder/muxer change can ripple).
- The record e2e gates (`e2e_record_*`) + a new rational-rate ffprobe assertion.
- clang-format / clang-tidy clean; independent review of the StreamWorker/Muxer plumbing.
