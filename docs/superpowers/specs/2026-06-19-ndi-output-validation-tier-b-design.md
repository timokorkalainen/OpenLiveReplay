# NDI Output Validation — Tier (b): MKV decode → real worker → NDI out — Design

**Date:** 2026-06-19
**Status:** Approved (sub-project 2 of the NDI output validation lane; stacks on the foundation/tier-a branch)

## Goal

Validate the full production playback→output path: play a marker MKV through the **real
`PlaybackWorker`** (decode → output frame cache → output bus → `NdiOutputSink`) with NDI
output enabled, and capture the result with the foundation's `ndi_recv_probe`, asserting the
same continuity / A-V-sync / cadence gates as tier (a) plus the worker's own playback health
(`reposition == 0`). Where tier (a) proved the sink+transport in isolation, tier (b) proves
the whole decode→render→sink pipeline.

## Key finding: no production change

`PlaybackWorker::setExternalOutputTargets(const QList<OutputTargetAssignment>&)` is already a
**public** method (playbackworker.h:68; impl playbackworker.cpp:84-88). It stores the
assignments and flags `m_outputTargetsDirty`; the worker thread rebuilds endpoints
(`rebuildOutputEndpoints`) and constructs/owns a `QueuedOutputSink(NdiOutputSink)` for each
enabled `OutputTargetKind::Ndi` assignment, handing them to its `OutputRuntime`. So tier (b)
needs **no production source change** — only a test-harness hook that calls the existing
public method. The sink lifetime is fully owned by the worker.

## Architecture (all new code under tests/)

### Components

- **`ndi_marker_mkv_source` (new harness)** — writes the foundation's `ndi_output_marker`
  content for a configurable duration as **raw YUV420P** video frames to a `.yuv` file and
  **raw S16 interleaved stereo** to a `.pcm` file. Pure: reuses `ndi_output_marker`
  (`ndiMarkerLumaPlane` for the Y plane, neutral 128 chroma, `ndiMarkerAudioS16`); no NDI.
  256×144 @ 30 fps, 48 kHz, matching the marker config defaults (so the probe decodes it).

- **`play_harness` hook (test-harness change)** — if the env var `OLR_NDI_OUTPUT_SENDER` is
  non-empty, after `worker.openFile(file)` and before `worker.start()`, build an
  `OutputTargetAssignment{ id="ndi-tier-b", sourceBus=OutputBusId::feed(0),
  kind=OutputTargetKind::Ndi, enabled=true, settings["senderName"]=<env> }` and call
  `worker.setExternalOutputTargets({assignment})`. Unset env → unchanged behaviour (existing
  playback e2e tests are unaffected). `sourceBus=feed(0)` taps the raw decoded feed at source
  resolution (256×144), which is what the probe's fixed-cell decode expects.

- **`run_ndi_playback_e2e.sh` (new driver)** —
  1. run `ndi_marker_mkv_source` → `marker.yuv` + `marker.pcm` (hermetic temp dir);
  2. mux to a worker-decodable `marker.mkv` via ffmpeg, **intra-only, near-lossless** so the
     high-contrast 8×8 counter cells survive decode (default `-c:v ffv1` lossless, with a
     high-quality intra mpeg2 fallback if ffv1 is rejected by the worker);
  3. start `play_harness marker.mkv play1x 1` in the background with
     `OLR_NDI_OUTPUT_SENDER="OLR NDI Playback Probe $$"`; `sleep` for NDI registration;
  4. run `ndi_recv_probe "<sender>" <seconds>` to capture and measure;
  5. assert the probe report (`drops==0 dupes==0 reorders==0 avSyncMaxFrames∈[0,1]
     maxGapFrames≤2 framesReceived≥floor`) AND the worker's `COUNTERS` line
     (`reposition==0`, `audioPushes>0`);
  6. SKIP (exit 77) if no NDI runtime, no ffmpeg, or the player exits 77 (sink couldn't start).

- **CTest `e2e_ndi_playback`** under the opt-in label **`ndi-output`** (already excluded from
  default/pre-push/CI), `SKIP_RETURN_CODE 77`, `RUN_SERIAL TRUE`.

### Data flow

```
ndi_output_marker -> ndi_marker_mkv_source -> .yuv/.pcm -> ffmpeg mux -> marker.mkv
  -> play_harness (real PlaybackWorker: decode -> cache -> output bus -> NdiOutputSink)
  -> [NDI transport] -> ndi_recv_probe -> continuity/A-V/cadence -> driver asserts
```

## Metrics and thresholds

- Probe (reused, identical to tier a): `drops==0`, `dupes==0`, `reorders==0`,
  `avSyncMaxFrames∈[0,1]`, `maxGapFrames≤2`, `framesReceived ≥ seconds×fps×0.5`.
- Worker playback health (from the `COUNTERS` line): `reposition==0` (no seek storm),
  `audioPushes>0` (the audio path ran).

## Critical risk and how it is self-checking

The probe decodes the marker at **fixed pixel positions assuming 256-wide** frames. Tier (b)
relies on the **feed(0) bus emitting at source resolution (256×144)** — `renderFeed(0)`
returns the cached decoded frame, not a composited/scaled buffer. If the worker instead
scaled the output, the probe's cell sampling would land on wrong pixels and produce a **loud
FAIL** (garbage indices → huge drops/reorders), never a vacuous pass. The plan verifies this
empirically (the gate passing proves the resolution path is correct).

## Marker survival through encode

The 8×8 counter/flash cells are full-contrast (luma 16 vs 235) and large, so they survive
lossy intra encode; the driver defaults to **ffv1 lossless** to remove codec loss as a
variable entirely (the worker decodes ffv1 via FFmpeg). If the worker rejects ffv1, fall back
to intra mpeg2 at high quality (`-q:v 2 -intra`). Audio is `pcm_s16le` (lossless), matching
the worker's expected mezzanine audio.

**PTS floor-alignment (load-bearing).** The free-running output clock samples the frame cache
at `playheadMs = floor(N*1000/30)` (0, 33, 66, 100, …). The synthetic fixture's source PTS
must match that lattice exactly, or the floor-sampling clock double-samples/skips frames —
purely a source-vs-output-clock *phase* artifact (the documented "not genlock-grade"
limitation), not an NDI-transport fault, but it would swamp the transport signal under test.
The mux therefore forces `PTS = trunc(N*1000/30)` via `-vf settb=1/1000,setpts=trunc(N*1000/30)
-enc_time_base 1:1000` rather than a plain `-video_track_timescale 1000` (which rounds and
yields ~60 dupe+drop pairs per 6s). This isolation is non-vacuous: a genuine transport
drop/dupe/reorder still fails the `==0` gates. Real recorded fixtures (record_harness, ms
timebase) are already aligned, so this is specific to the synthetic raw-frame fixture.

## Gating, error handling, testing

- Opt-in `ndi-output` label; runtime/ffmpeg gating via the existing skip-77 contract; SKIP
  when the NDI runtime is absent, ffmpeg is missing, or the player's NDI sink can't start.
- Hermetic temp dir for `.yuv/.pcm/.mkv`, cleaned on a trap; background player killed on exit.
- The marker source's raw-frame output is deterministic and could be unit-checked, but the
  foundation's `tst_ndioutputmarker` already pins the codec; tier (b) adds no new pure logic,
  so it ships as the opt-in integration gate plus the small harness/driver.

## Non-goals

- No NDI **ingest** (that is tier (c): NDI source → ingest → record → this playback path).
- No production source changes.
- No new metrics beyond the reused probe + the worker's existing counters.

## File summary

- Create: `tests/e2e/ndi_marker_mkv_source.cpp`, `tests/e2e/run_ndi_playback_e2e.sh`.
- Modify: `tests/e2e/play_harness.cpp` (env-gated NDI-output hook),
  `tests/e2e/CMakeLists.txt` (new target + `e2e_ndi_playback` test under label `ndi-output`).
- No production source changes; reuses `ndi_output_marker`, `ndi_recv_probe`, `play_harness`.
