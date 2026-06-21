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
- **MPEG-2 (`setupEncoder`):** keep `encCtx->time_base = {1, m_targetFps}` (the integer-fps coding clock)
  and set only `encCtx->framerate = {fpsNum, fpsDen}` — `framerate` is the field `mpeg2video` writes into
  the sequence-header `frame_rate_code` (the 1000/1001 variants), while `time_base` governs the coded PTS.
  **This separation is load-bearing:** the MPEG-2 path stamps `frame->pts = frame_index` and rescales the
  *output packet* from `encCtx->time_base` to the muxer, so a rational `time_base` would push the muxed
  video PTS onto the 29.97 grid (~33.367ms/frame) while audio/metadata stay on the integer-fps grid
  (~33.333ms) — a slow A/V drift. (See the adversarial-review correction at the end.) Update the "exactly
  representable" warning to accept the rational 30000/1001 & 60000/1001.
- **Native H.264 (config at `streamworker.cpp:551`):** pass `{w, h, fpsNum, fpsDen, bitrate}` (the config
  struct already has `fpsNum`/`fpsDen`, `nativevideoencoder.h:25`). NOTE: only MediaFoundation (Windows)
  honors `fpsNum/fpsDen` for the ES; VideoToolbox (macOS) ignores them, so on macOS the H.264 **container**
  rate (the muxer fields below) is authoritative and the ES VUI is unchanged.

### Part 2 — Rational rate through the container (Muxer)

- Thread `fpsNum/fpsDen` into `Muxer::init` (extend the canonical overload; keep the existing `int fps`
  callers working by deriving an integer where only one is needed, or add rational params with sensible
  defaults). Set `st->avg_frame_rate = {fpsNum, fpsDen}` and `st->r_frame_rate = {fpsNum, fpsDen}`
  (`muxer.cpp:136-137`). No change to `time_base` (stays ms) or packet PTS.

### Part 3 — Drop-frame timecode display (UIManager) — SPLIT TO A FOLLOW-UP PR

> **Scope update (during implementation):** the on-screen timecode is computed in **two** places
> that must stay byte-identical — `UIManager::playbackTimecode()` (C++, drives the StreamDeck) and the
> QML label in `Main.qml` — and uses a `HH:MM:SS.FF` (period) separator rather than SMPTE `:`/`;`.
> Making it drop-frame is therefore a coupled C++/QML UI change (separator + DF renumbering kept in sync
> across both), a distinct logical unit from the engine-side rate fix. Parts 1 & 2 (the actual
> file-correctness bug) ship first; Part 3 follows as its own PR. The design below stands for that PR.

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

## Adversarial-review correction (post-implementation)

A ruthless multi-lens adversarial review caught a real regression in the first implementation and the
fixes below were applied:

- **MPEG-2 video PTS drift (important — fixed).** The first cut set the MPEG-2 encoder
  `time_base = {fpsDen, fpsNum}`. Because the MPEG-2 path stamps `frame->pts = frame_index` and rescales
  the *output packet* from `encCtx->time_base` to the muxer, that pushed the muxed video PTS onto the 29.97
  grid (~33.367ms/frame) while audio/metadata stayed on the integer-fps grid (~33.333ms) — a slow A/V
  drift (~6ms/6s, ~3.6s/hour) on exactly the 29.97/59.94 rates this work targets, contradicting the
  "timing-identical" scope claim. **Fix:** keep `time_base = {1, m_targetFps}` (integer-fps coding clock)
  and carry the rational rate only via `framerate` (ES) + the container `avg/r_frame_rate`. Same fix on the
  blue-fill MPEG-2 encoder. The H.264 path was never affected (it already rescales PTS via `{1,fps}`), and
  integer 30/1 was byte-identical throughout.
- **Test had no timing teeth (fixed).** The e2e/unit gates checked only the advertised-rate *metadata*, so
  they could not catch the PTS drift. The `rational` e2e now also asserts the mean muxed-video frame
  interval stays on the integer-fps cadence (~33.333ms, bounded `< 33.35ms`), which fails if a rational
  rate ever leaks back into the coded PTS.
- **ES claim narrowed (honesty).** "Both the elementary stream and the container" is only fully true for
  MPEG-2 (whose `framerate` drives the sequence-header rate) and for MediaFoundation H.264. **VideoToolbox
  (macOS) H.264 ignores `fpsNum/fpsDen`**, so on macOS the H.264 ES VUI is unchanged and the **container**
  rate is the authoritative advertised rate (which is what MKV playback uses anyway).

### Deferred (documented, not in this PR)

- H.264 / 59.94 rational e2e cells and an ES-level (not just container) rate assertion (TVC-2/F4).
- A UIManager unit test for the two `setFpsRational` call sites (needs a `ReplayManager::fpsNum/fpsDen`
  getter seam) (TVC-3).
- VideoToolbox `kVTCompressionPropertyKey_ExpectedFrameRate` / SPS-VUI injection for a true macOS H.264 ES
  rate (only if ES-level signalling is later required; container is authoritative today).
