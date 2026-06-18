# NDI Output Validation — Tier (c): NDI in → record → MKV → playback → NDI out — Design

**Date:** 2026-06-19
**Status:** Approved (sub-project 3 of the NDI output validation lane; stacks on tier (b))

## Goal

Prove the **whole** broadcast pipe is reliable end to end: a marker stream enters over **real
NDI ingest**, is **recorded** to an MKV, **played back** through the real `PlaybackWorker` with
**NDI output**, and is captured by the foundation's `ndi_recv_probe`. Tier (a) proved the output
transport; tier (b) proved decode→output; tier (c) adds the one untested segment — **NDI
ingest → record** — and proves the integrated pipe carries ordered, live, A-V-synced content
without sustained loss, stalls, reorders, or a seek storm.

## Key finding: the pipe is rate-matched, not genlocked — so the gate measures robust
## invariants, not frame-exact equality

Three independent ~30 fps clocks sit in this pipe: the marker **sender**, the **recorder**, and
the playback **output clock**. Grounded in the code:

- The recorder ticks on its own 30 fps grid and samples whichever ingested frame most recently
  arrived (`streamworker.cpp` — arrival queue sampled at `frameIndex*1000/fps`), writing
  **grid-anchored, av_rescale-rounded PTS** (0, 33, 67, 100, … ms; `muxer.cpp` time_base
  `{1,1000}`). So the recording rate-matches the sender: small clock differences yield occasional
  duplicated or skipped marker counters, never reordering.
- The output clock advances `playheadMs = floor(frameIndex*1000/30)` (0, 33, 66, 100, …;
  `framerate.h`) and the cache returns the largest `pts ≤ playheadMs` (`outputframecache.cpp`).
  Playing the recorder's **rounded** PTS through this **floor** clock reproduces tier (b)'s
  systematic phase artifact (~⅓ dupes + ⅓ drops) — but here it is **unavoidable** (a real
  recording cannot be floor-aligned the way tier (b)'s synthetic fixture was).

The existing `e2e_play_*` gates already concede this: they assert only the worker's `reposition`
counter, **never output `drops==0`** (`run_playback_e2e.sh`). The system is explicitly **not
genlock-grade** (arrival-anchoring, integer fps, no reference clock). Therefore tier (c) gates
**robust pipe invariants** and **reports** raw dupes/drops as diagnostics:

- **Ordering is strict** end to end: `reorders == 0` (every segment preserves frame order).
- **No sustained loss/stall:** `maxGapFrames` bounded.
- **Liveness:** received/decoded frame counts meet a floor.
- **A-V stays locked:** `avSyncMaxFrames` bounded (looser than tier b — more pipe jitter).
- **Worker health:** `reposition == 0`, `audioPushes > 0`.
- Catastrophic ingest loss is still caught (see Stage A's drop ceiling + coverage below); benign
  rate-match dupes/drops are reported, not gated to zero.

## Key finding: no production change

Reuses the existing public seams only: `ndi_output_sender` (tier a) as the marker NDI **source**;
`record_harness --url ndi:<name>` (existing native NDI ingest) to **record**; `play_harness` with
the tier-(b) `OLR_NDI_OUTPUT_SENDER` hook to **play with NDI out**; `ndi_recv_probe` (foundation)
to **capture**. Only new test code: one analyzer tool + one driver + the CTest registration.

## Architecture (all new code under tests/)

### Data flow

```
ndi_output_sender (marker NDI source, 256x144 @30)            [tier a, reused]
  -> [NDI transport] -> record_harness --url ndi:<src> --width 256 --height 144  [existing ingest]
       -> marker.mkv  (OLR_VIEWS=1: single marker view, mpeg2 luma-lossless)
  --- Stage A: ffmpeg decode marker.mkv -> raw luma -> marker_yuv_probe -------- (NDI in -> record)
  --- Stage B: play_harness marker.mkv play1x 1 (OLR_NDI_OUTPUT_SENDER set) ----- (record -> NDI out)
       -> [NDI transport] -> ndi_recv_probe -> continuity / A-V / cadence
```

### Components

- **`marker_yuv_probe` (new harness)** — reads raw single-plane luma frames from **stdin**
  (`width*height` bytes/frame), decodes the marker counter per frame (`ndiMarkerDecodeIndex`),
  runs `ndiAnalyzeContinuity` over the decoded index sequence, and prints one parseable line:
  `MKVMARK framesDecoded=<n> drops=<n> dupes=<n> reorders=<n> maxGapFrames=<n> firstIndex=<n>
  lastIndex=<n>`. Pure analysis of the **recorded** file (no NDI). It validates the marker config
  is 256×144 (errors loudly otherwise — the marker geometry requires it). Exit 0 (report), 2 (bad
  args), 1 (no frames decoded / dimension mismatch). Reuses `ndi_output_marker` + `ndi_recv_analysis`.

- **`run_ndi_e2e_pipe.sh` (new driver)** —
  1. start `ndi_output_sender "OLR NDI Pipe SRC $$" <senderSecs>` (the marker NDI source); sleep
     for NDI discovery; SKIP-77 if it exits 77 (no runtime);
  2. URL-encode the source name; record it: `OLR_VIEWS=1 record_harness --url "ndi:<enc>" --name
     olr_ndi_pipe --outdir $WORK --seconds <recSecs> --width 256 --height 144 --fps 30` → MKV path
     (stdout last line); **assert the recorded video stream is exactly 256×144** (ffprobe) — a
     scaled recording would corrupt the marker cells (self-check, like tier b);
  3. **Stage A** (NDI in → record): `ffmpeg -i marker.mkv -map 0:v:0 -f rawvideo -pix_fmt gray - |
     marker_yuv_probe 256 144`; assert ingest+record integrity (below);
  4. stop the source sender (free the NDI name);
  5. **Stage B** (record → NDI out): `OLR_NDI_OUTPUT_SENDER="OLR NDI Pipe OUT $$" play_harness
     marker.mkv play1x 1` in the background; `ndi_recv_probe "OLR NDI Pipe OUT" <capSecs>`; assert
     output continuity/A-V/cadence + the worker `COUNTERS`;
  6. SKIP-77 if the NDI runtime, ffmpeg, ffprobe, or the source is unavailable; clean up on a trap.

- **CTest `e2e_ndi_pipe`** under the opt-in label **`ndi-output`** (already excluded from
  default/CI/pre-push), `SKIP_RETURN_CODE 77`, `RUN_SERIAL TRUE`, generous `TIMEOUT` (record +
  play windows are serial).

## Metrics and thresholds (starting values — tune empirically against the real loopback)

These are **starting** thresholds; the implementer runs the real loopback (the NDI runtime is
present on the dev machine), records the observed numbers, and sets each threshold to pass cleanly
with margin while still failing on catastrophic loss — exactly as tier (b) tuned its mux. Document
the observed numbers and any change in the report.

**Stage A — recorded MKV (NDI ingest + record integrity), `MKVMARK` line:**
- `reorders == 0` (strict — ordering preserved through ingest+record).
- `maxGapFrames <= 2` (no single large skip).
- `framesDecoded >= recSecs * 30 * 0.9` (the recorder ticks at 30 fps; a near-empty/failed record
  fails).
- **Coverage / catastrophic-loss gate:** `drops <= max(3, ceil(0.05 * framesDecoded))` — a clean
  loopback rate-matches with only a few drops; a real ingest break (heavy NDI loss → the recorder
  re-samples stale frames and the sender's counters go missing) drives `drops` far past 5% and
  fails. `dupes` reported, not gated.

**Stage B — NDI output (record → playback → out), `NDIRECV` + `COUNTERS` lines:**
- `reorders == 0` (strict).
- `maxGapFrames <= 3` (looser than Stage A — the floor/round phase artifact widens gaps slightly).
- `framesReceived >= capSecs * 30 * 0.5` (liveness).
- `avSyncMaxFrames in [0, 2]` (A-V locked; looser than tier b's `[0,1]` for the extra pipe jitter).
- Worker `reposition == 0`, `audioPushes > 0`.
- `drops`/`dupes` **reported, not gated to zero** — the recorder-round-PTS × output-floor-sampling
  phase artifact makes nonzero dupes/drops expected here (documented; tier b proved the transport
  itself is lossless when the fixture is aligned).

## Why this is non-vacuous

A real regression still fails a **gated** metric, not just the reported diagnostics:
- NDI ingest broken / heavy loss → Stage A `drops` ceiling and/or `framesDecoded` floor fail.
- Frames corrupted/reordered anywhere → `reorders > 0` (either stage) fails.
- Playback stall / decoder hang / sink crash → Stage B `framesReceived` floor or `maxGapFrames`
  fails; worker `reposition` storm or dead audio fails.
- A scaled (resolution-corrupted) recording → the 256×144 ffprobe assertion fails before Stage A.
- A-V drift through the pipe → `avSyncMaxFrames` fails.
The phase-artifact dupes/drops are the **only** thing not gated to zero, and tiers (a)/(b) already
proved the transport and decode→output are frame-exact under controlled timing, so nothing real is
hidden by reporting them here.

## Marker survival through the pipe

- **Ingest→record:** `ndiframeconvert` direct-memcpys a matching-resolution I420 frame (256×144 in,
  256×144 record target) — **no scaling** — and the recorder encodes mpeg2 at ~30 Mbps intra; the
  full-contrast 8×8 luma cells (16 vs 235) are luma-near-lossless and chroma subsampling does not
  touch luma. The marker counter survives. The driver pins the record target to 256×144 and
  asserts it.
- **Record→playback→out:** identical to tier (b) (mpeg2/ffv1 decode → feed(0) at source resolution
  → `NdiOutputSink` → UYVY at the receiver; the probe extracts luma `col*2+1`).
- **Audio:** the marker beep is pcm_s16le lossless through NDI + record; A-V pairing reuses the
  foundation probe (its 1:1-chunking limitation is documented in the tier-a spec).

## Gating, error handling, testing

- Opt-in `ndi-output` label; SKIP-77 when the NDI runtime, ffmpeg, ffprobe, or the source is
  absent. Hermetic temp dir (sender out, record outdir, MKV) cleaned on a trap; background
  sender and player killed on exit.
- `marker_yuv_probe`'s decode↔continuity is pure; the foundation's `tst_ndioutputmarker` /
  `tst_ndirecvanalysis` already pin the codec and the continuity/cadence math, so tier (c) adds no
  new unit test — it ships as the opt-in integration gate plus the small analyzer + driver.

## Non-goals

- No production source changes.
- No new metrics beyond the reused continuity/cadence + the worker's existing counters.
- Not frame-exact (genlock) verification of the full pipe — that is not what the system provides;
  ordering/liveness/sync/no-sustained-loss is the reliability contract tier (c) verifies.
- Not a hosted-CI gate (needs the NDI runtime) — self-hosted/opt-in only.

## File summary

- Create: `tests/e2e/marker_yuv_probe.cpp`, `tests/e2e/run_ndi_e2e_pipe.sh`.
- Modify: `tests/e2e/CMakeLists.txt` (new `marker_yuv_probe` target + `e2e_ndi_pipe` test under
  label `ndi-output`, `SKIP_RETURN_CODE 77`).
- No production source changes; reuses `ndi_output_sender`, `record_harness`, `play_harness`,
  `ndi_recv_probe`, `ndi_output_marker`, `ndi_recv_analysis`.
