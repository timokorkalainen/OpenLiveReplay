# Frame-Perfect Armed Cut on H.264 — Design

**Date:** 2026-06-21
**Status:** Approved (next-phase pick after the H.264 validation lane; grounded against main @ 51972a3)

## Goal

Make the armed-cut **pre-roll** bank hardware-decode H.264 so operator **Recall** is frame-perfect on
the production-recommended codec. Today `openPrerollContext()` skips all H.264 streams (hardware-only
licensing constraint; no `NativeVideoDecoder` wiring in the pre-roll path), so the pre-roll bank is
empty on an H.264 recording, `armNextCut` returns false, and `UIManager::recallEntry` degrades to a
plain seek (validated by `e2e_play_armedcut_h264`). This phase upgrades that degradation to a
frame-perfect armed cut, matching MPEG-2 behavior.

## Key finding: the decoder is synchronous — port the primary bank, no async bridge

`NativeVideoDecoder::decode(unit, onFrame, err)` (`recorder_engine/ingest/nativevideodecoder.h:21-37`)
*looks* async (a `std::function` callback) but is **synchronous in practice**: VideoToolbox blocks on
`VTDecompressionSessionWaitForAsynchronousFrames` and MediaFoundation `drainSync`s, so the `onFrame`
callback fires **inline on the calling (worker) thread before `decode()` returns**. The **primary
decode bank already runs this exact pattern** (`playbackworker.cpp:557-636`): per access unit, convert
avcC→Annex-B, build a `CompressedAccessUnit{codec, pts90k, dts90k, annexB, parameterSets}`, call
`decode()`, and the inline callback `convertToMediaVideoFrame()`s (output is already `YUV420P`) and
`insertVideoFrame`s. All-intra mezzanine ⇒ one access unit → exactly one frame.

Therefore `fillStaging`'s bounded synchronous fill loop can call `decode()` exactly where it currently
calls `avcodec_send_packet/receive_frame`, and the staged-PTS coverage watermark
(`m_stagingNewestRefPtsMs >= coverTo`) keeps working because the callback updates it inline before the
loop re-checks. **No frame queue, no async bridge, no decode-with-timeout.** `DecoderTrack` already
carries `std::unique_ptr<NativeVideoDecoder> nativeDecoder`, `H26xParameterSets h264ParamSets`, and
`codecWidth/codecHeight` (`playbackworker.h:34-49`) — no header/struct change.

## Design (production: playback/playbackworker.cpp only)

1. **`openPrerollContext`** — replace the unconditional H.264 `continue` (currently ~`:1022`) with the
   avcC-parse + `NativeVideoDecoder` construction copied from the primary bank (`:1464-1522`): if
   `queryNativeVideoDecodeCapabilities().h264 && extradata_size >= 8`, parse avcC → SPS/PPS into
   `track->h264ParamSets`, set `track->nativeDecoder = make_unique<NativeVideoDecoder>(w, h)`,
   `track->codecWidth/Height`, `track->provider = nullptr`, and **advance `feedIndex`** (do not skip).
   Keep the `continue` **only** as the no-HW / avcC-parse-fail fallback (so a degenerate file still
   disables the feature gracefully). Advancing feedIndex keeps pre-roll `feedIndex N` aligned with
   primary `providerIndex N` (the homogeneous-codec invariant holds).

2. **`fillStaging`** — branch the per-track decode: `if (track->nativeDecoder) { native } else { existing
   FFmpeg send/receive }`. The native branch is a near-verbatim port of the primary `handleFrame` body
   (`:559-634`) but: inserts into `m_prerollStagingCache` (not `m_outputCache`), updates
   `m_stagingNewestRefPtsMs` for the ref stream, and omits the primary-only state (`m_bufferMutex`,
   `track->buffer`, `FrameIndex`) — pre-roll staging is worker-private. Coverage/EOF detection unchanged.

3. **Post-seek session reset (risk mitigation).** Right after the backward `av_seek_frame` +
   `avformat_flush` in `fillStaging` (~`:1156`), call `track->nativeDecoder->reset()` for native tracks
   so the VT/MF session starts clean after the seek — guarantees PTS fidelity (the
   `maxClockDivergenceMs<=1500` gate depends on it). Cheap; all-intra makes it safe.

4. **Teardown cleanup gap.** The pre-roll teardown (~`:2034-2037`) frees `track->codecCtx` but not the
   native decoder. Add `track->nativeDecoder.reset();` before `delete track`, mirroring the primary
   `clearDecoders` lambda (`:1412`) — prevents a VT/MF session leak/UAF.

## Armed-cut hardening to preserve (do NOT regress)

The cut path was hardened by #94 (re-anchor output clock at cut + divergence gate), #98 (re-arm data
race guard), #104 (decoder-follow for backward cuts + post-seek codec flush), #107 (seek-vs-cut policy).
The native pre-roll path must preserve: (a) staging is written **only on the worker thread, only via
`insertVideoFrame`**, with correct `feedIndex`/`ptsMs`; (b) `m_stagingCovers` remains the sole
scheduling gate; (c) EOF still forces coverage so the cut can't hang; (d) PTS fidelity (the divergence
gate). The native `decode()` runs entirely inside `fillStaging` on the worker — it adds no new
cross-thread state, so the swap/divergence/follow/seek-vs-cut logic is untouched. (Note: the #104
post-seek flush only flushes `codecCtx`, not the native decoder — benign for all-intra; documented.)

## Validation: flip the e2e gate from "degradation" to "frame-perfect"

`e2e_play_armedcut_h264` currently asserts the degradation (`armNextCutArmed==0`, `cutsFired==0`,
`reposition>=1`). After this change, armed cut works on H.264, so rework the `armedcut-h264` play_harness
scenario to mirror the MPEG-2 `armedcut` scenario (arm + a queued re-arm → `cutsFired==2`) and flip the
assertions to mirror the MPEG-2 `armedcut` gate:
- `placeholderFramesDelta == 0` (no gray flash across the cut),
- `reposition == 0` (the cut fired; no coarse-seek fallback),
- `cutsFired == 2` (queued re-arm fired),
- `cutFollowReposition == 0` (forward cut, no backward decoder-follow),
- `maxClockDivergenceMs <= 1500` (frame-accurate, epoch re-anchored),
- **`decodedVideoFrames >= 30`** (non-vacuity: proves real HW frames were decoded into staging, not an
  empty/placeholder cache "firing" a cut). `armNextCutArmed` flips to `1` (armed) — assert that too.

This makes the gate prove the feature works (the same standard as MPEG-2), and the `decodedVideoFrames`
floor is the load-bearing non-vacuity guard.

## Risks

- **PTS fidelity of the native path** (feeds `maxClockDivergenceMs`): mitigated by the post-seek
  `nativeDecoder->reset()` (design #3) + deriving `ptsMs` from the packet PTS exactly as the primary path.
- **Session leak/UAF on teardown**: fixed by design #4.
- **Regressing MPEG-2 armed cut / other scenarios**: the change is gated behind `track->nativeDecoder`
  (only H.264 tracks); MPEG-2 takes the unchanged FFmpeg branch. Verified by the unaffected
  `e2e_play_armedcut` (mpeg2) + `e2e_play_armedcut_back` gates.

## Non-goals

- No change to the primary live decode bank (already HW-decodes H.264).
- No B-frame / non-intra handling (the mezzanine is all-intra by construction).
- No header/`DecoderTrack` change (fields already exist).
- HEVC pre-roll (out of scope; this is the H.264 feature).

## File summary

- Modify: `playback/playbackworker.cpp` (`openPrerollContext`, `fillStaging`, pre-roll teardown).
- Modify: `tests/e2e/play_harness.cpp` (`armedcut-h264` scenario), `tests/e2e/run_playback_e2e.sh`
  (`armedcut-h264` assertion case).
- No production header change; no `DecoderTrack` change.
