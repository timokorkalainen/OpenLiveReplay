# AUD-4: single shared A/V anchor per source (PCR on native, first-PES on ffmpeg) — Design

**Status:** approved (brainstorm 2026-06-16)
**Roadmap item:** framesync P0 / AUD-4 (see `broadcast-framesync-roadmap` memory)
**Base branch:** `feat/srt-aud4-anchor`, off `origin/main` (a383f43 — includes #41/JIT-5 + #43/JIT-1).

## Goal

Give each source a **single shared audio/video timeline anchor** instead of anchoring audio and
video independently. Today both ingest paths capture *two* anchors — `recordingClockMs()` at the
first **video** packet's arrival, and *separately* at the first **audio** packet's arrival — so the
wall-clock gap between those two first-arrivals becomes a permanent per-source lip-sync error. The
report-only `lipsync` scenario measures **+63.3 ms** audio lag today (`SYNC_BASELINE.md`), just
outside EBU R37's +40/−60 ms band. One shared anchor makes the recorded A/V offset equal the true
*stream* offset (audio PTS and video PTS/DTS already share the 90 kHz program clock), so the
arrival-jitter skew vanishes.

## Background

Both ingest sessions output recording-timeline-stamped media that StreamWorker trusts verbatim
(video `DecodedVideoFrame.sourcePtsMs`, audio `DecodedAudioChunk.startSample`); nothing downstream
re-aligns A/V, so the **anchor choice is the whole game**.

- **Native** (`nativesrtingestsession.cpp`): `sourcePtsMsForUnit()` anchors on the first video DTS
  (`m_firstDts90k` → `m_anchorStreamTimeMs`); `sourcePtsMsForAudio()` anchors *separately* on the
  first audio PTS (`m_firstAudioPts90k` → `m_audioAnchorStreamTimeMs`). Both map
  `anchorMs + (pts90k − firstX90k)/90`. The MPEG-TS PCR (the program's master clock) is **not**
  currently extracted; `MpegTsParser`/`PesPacket` expose only per-PES `pts90k`/`dts90k`.
- **ffmpeg** (`ffmpegingestsession.cpp` `run()`): identical dual-anchor bug —
  `firstPacketDts`/`anchorStreamTimeMs` for video and `firstAudioDts`/`audioAnchorStreamTimeMs`
  for audio, each captured at its own stream's first packet.

The offset is **not** a timebase mismatch (both are 90 kHz program-clock timestamps) — it is purely
the independent wall-clock anchoring. So one shared anchor fixes it, and the 90 kHz deltas carry the
correct A/V offset.

## Approach

Per source, replace the two anchors with **one** `(anchorTs90k, anchorStreamMs)`. Both streams map:

```
sourceMs = anchorStreamMs + (pts90k − anchorTs90k) / 90
```

The anchor *reference* differs by path (PCR is only reachable where we parse TS ourselves), but the
intra-source A/V result is identical because every timestamp shares the program timebase.

### Native path — recover and anchor to the PCR

`MpegTsParser` already parses PAT→PMT to classify streams, so it knows the PCR PID (the PMT's
`PCR_PID` field). Extend it to extract the **PCR** from the adaptation field of TS packets on that
PID: read the 33-bit 90 kHz base (the 9-bit 27 MHz extension is sub-ms — ignored). Surface it to the
session — e.g. `pushTsPacket()` gains a `qint64* pcr90kOut` (or the parser stores `lastPcr90k` and a
"pcr seen" flag) carrying the most recent PCR.

`NativeSrtIngestSession` holds a single shared anchor `m_anchorTs90k` / `m_anchorStreamTimeMs`,
established on the **first PCR**: `m_anchorTs90k = firstPcr90k`, `m_anchorStreamTimeMs =
recordingClockMs()` at that moment. `sourcePtsMsForUnit()` and `sourcePtsMsForAudio()` both map
against it; the per-stream anchor members (`m_firstDts90k`/`m_anchorStreamTimeMs` for video,
`m_firstAudioPts90k`/`m_audioAnchorStreamTimeMs` for audio) are removed in favor of the shared pair.

**Re-anchor** on the adaptation field's `discontinuity_indicator` (the canonical signal), with the
existing PTS/DTS jump heuristic (`kForwardJump90k`/`kBackwardTolerance90k`) retained as a backstop;
re-anchoring resets the shared `(anchorTs90k, anchorStreamMs)` so both streams realign together.

### ffmpeg path — shared first-PES anchor

In `run()`, collapse the two anchors into one `(anchorTs90k, anchorStreamMs)` set by the **first
packet of either stream** with a valid timestamp (whichever the demuxer delivers first). Both video
and audio map against it; the discontinuity re-anchor logic is unified to act on the shared anchor.

## Error handling / edge cases

- **PES before the first PCR (native):** PCR leads and is frequent (ffmpeg's TS muxer emits it every
  ~20–40 ms), so this is rare and sub-frame. **First-PES fallback:** if a PES needs mapping and no
  PCR has been seen yet, establish the shared anchor from that PES (so PCR-less or
  PCR-late streams still work and nothing is dropped); once a PCR has anchored, PCR wins. This keeps
  PCR-preferred behavior without dropping warm-up media.
- **Discontinuity:** re-establish the shared anchor from the discontinuity packet; a program-clock
  reset hits both streams, so realigning both together is correct.
- **No audio / no video:** a single-stream source simply anchors on whatever it has — unchanged
  behavior, one anchor.

## Testing

1. **Unit — `MpegTsParser` PCR extraction:** feed synthetic 188-byte TS packets carrying a known
   PCR in the adaptation field (PCR_flag set) on the PMT's PCR PID; assert the parser surfaces the
   exact 90 kHz base value, and that packets without a PCR don't spuriously report one. Pure, in
   `olr_test_core` (extends `tst_mpegtsparser.cpp` or a new `tst_mpegtsparser_pcr.cpp`).
2. **A/V-offset gate — ffmpeg path:** promote the report-only `lipsync` scenario (`run_sync_e2e.sh`,
   UDP→ffmpeg ingest, flash vs beep) to a **gate**. The metric is mean `(audio_pts − video_pts)` ms
   (positive = audio lags); assert it within EBU R37 — audio lead ≤ 40 ms, lag ≤ 60 ms, i.e.
   `−40 ≤ offset ≤ +60`. Naturally discriminating: today's **+63 ms exceeds the +60 ms lag bound**;
   AUD-4 brings it to ~0 → passes. The band leaves margin for the documented run-to-run
   marker-quantization jitter. (Match the harness's existing sign convention exactly.)
3. **A/V-offset gate — native path:** a new SRT flash+beep lipsync gate (`OLR_NATIVE_SRT=1`,
   `native-apple-ingest` label) measuring the offset over the PCR-anchored native path, same band.
4. Update `SYNC_BASELINE.md` (and the `lipsync` line) to the post-AUD-4 ~0 ms offset.

## Files touched

- `recorder_engine/ingest/mpegtsparser.{h,cpp}` — PCR PID capture + adaptation-field PCR extraction;
  surface `pcr90k` + discontinuity flag.
- `recorder_engine/ingest/nativesrtingestsession.{h,cpp}` — single shared anchor; PCR-first with
  first-PES fallback; both `sourcePtsMsFor{Unit,Audio}` map against it; drop the dual anchor members.
- `recorder_engine/ingest/ffmpegingestsession.cpp` — single shared first-PES anchor across A/V.
- `tests/unit/tst_mpegtsparser_pcr.cpp` (new) + `tests/unit/CMakeLists.txt` — PCR unit test.
- `tests/e2e/run_sync_e2e.sh` + `tests/e2e/CMakeLists.txt` — lipsync as a gate (ffmpeg path) + a new
  native SRT lipsync gate.
- `tests/e2e/SYNC_BASELINE.md` — refreshed lipsync number.

## Scope (YAGNI)

PCR is used **only as the anchor reference**, not yet a drift servo (that is P2 source-clock
recovery — this lays the groundwork). PCR is not extracted on the ffmpeg path. No cross-source /
inter-camera / genlock changes. No change to the per-source trim or jitter window.

## Success criteria

- Native and ffmpeg sources record with a single shared A/V anchor; the `lipsync` mean offset drops
  from ~+63 ms (exceeds the +60 ms lag bound) to within EBU R37 (lead ≤ 40 ms, lag ≤ 60 ms) on both
  paths (gated).
- `MpegTsParser` extracts the PCR (unit-proven); PCR-less streams still anchor via the first-PES
  fallback.
- No regression in `ctest -L srt`, `ctest -L native-apple-ingest`, `ctest -L unit`, or the existing
  sync-report scenarios.
