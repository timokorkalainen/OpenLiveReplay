# NDI Output Validation — Foundation + Tier (a) — Design

**Date:** 2026-06-18
**Status:** Approved (sub-project 1 of the NDI output validation lane)

## Goal

Prove the broadcast NDI **output transport** is reliable end-to-end: a real `NdiOutputSink`
sends a known marker stream over NDI, an external receiver captures it, and the run fails on
any dropped/duplicated/reordered frame, audio-video desync, or cadence stall. This is the
rung-5 "are our outputs actually good" answer the in-process soak (PR #81) deliberately did
not provide.

This sub-project delivers the **shared foundation** — an NDI receiver/measurement harness
plus gating — and **tier (a)**, the minimal output path (`NdiOutputSink` + transport only).
Tiers (b) MKV decode→output and (c) NDI in→record→playback→out reuse this foundation and are
separate follow-on sub-projects.

## Scope and non-goals

In scope:

- A standalone NDI receiver harness (`ndi_recv_probe`) that captures and measures.
- A pixel-encoded per-frame counter added to the marker source so the receiver can detect
  drops/dupes/reorders exactly, even though `NdiOutputSink` synthesizes the NDI timecode.
- Tier (a): a sender harness that submits marker-bearing `OutputBusFrame`s to a real
  `NdiOutputSink` at cadence.
- A driver script + opt-in CTest label, gated to skip cleanly when no NDI runtime/source is
  present.

Out of scope (explicit non-goals for this sub-project):

- No production source changes. The public `PlaybackWorker` NDI-output API lands with tier
  (b), the first tier that drives the real worker.
- No real decode / `PlaybackWorker` path (tier b).
- No NDI ingest → record → playback loopback (tier c).
- Not a hosted-CI gate: NDI transport needs a runtime, so this is self-hosted/opt-in only.
- No content-integrity assertion beyond what the frame-counter and flash/beep markers imply.

## Architecture

### Components

- **Frame-identity marker (new, in `tests/e2e/`)** — a generator that produces, per frame
  index, a YUV420P luma image carrying:
  - a **pixel-encoded frame counter** (a small block-coded region of the luma plane: each
    bit a black/white cell, MSB-first, read back by thresholding) — survives I420 transport
    and `NdiOutputSink`'s synthesized timecode;
  - a **video flash** on marker frames (full-frame bright luma) for A-V sync;
  - matching **S16 stereo audio** with an **audio beep** (tone burst) on the same marker
    frames.
  This may extend the existing `ndi_marker_pattern` or be a focused new generator; the
  decision is finalized in the plan. It must be pure/deterministic and unit-testable
  (encode frame N → decode → N).

- **`ndi_recv_probe` (new harness)** — loads the NDI runtime via `ndiruntimepaths`, finds the
  OLR sender via `NDIlib_find_*`, receives with `NDIlib_recv_*` (ABI already declared in
  `playback/output/ndiabi.h`), and for a bounded capture window:
  - decodes each video frame's pixel counter → the received frame-index sequence;
  - records each frame's wall-clock arrival time;
  - detects the video flash (luma threshold) and audio beep (RMS threshold) positions;
  - computes the metrics below and prints one parseable report line.
  It exits 0 always (the driver decides pass/fail from the report) except on a hard capture
  error.

- **Tier (a) sender (`ndi_output_sender` harness, new)** — constructs `OutputBusFrame`s whose
  video/audio are the marker content for a monotonically increasing frame index, and submits
  them to a real `NdiOutputSink` at the nominal cadence for the run duration. Minimal path:
  the sink + transport only, no dispatcher/bus-engine/decode.

- **Driver `run_ndi_output_e2e.sh` (new)** — starts the sender, runs `ndi_recv_probe` against
  the sender's source name, greps the report, asserts the thresholds, and exits non-zero on
  any breach. Reuses the runtime/skip pattern (skip → exit 77) so a machine without the NDI
  runtime is SKIPPED, not failed.

### Data flow

```
marker frames → OutputBusFrame → NdiOutputSink → [NDI transport] → ndi_recv_probe
            → decode counter + flash/beep + arrival times → report → driver asserts
```

## Metrics and thresholds

The probe reports and the driver asserts (configurable via env, defaults shown):

- **Frame continuity:** every sent frame index is received exactly once and in order across
  the steady window. Pass: `drops == 0 && dupes == 0 && reorders == 0`. (Startup/teardown
  frames outside the steady window are excluded.)
- **A-V sync:** the audio beep aligns with its video flash within `|offset| <= 1` frame.
- **Cadence:** no inter-arrival gap exceeds `2x` the nominal frame period. Pass:
  `maxGapFrames <= 2`. `meanRateHz` is reported for diagnostics but not asserted — a mean rate
  over a short wall-clock capture window is itself jittery, so `maxGapFrames` carries the
  cadence gate.
- **Liveness:** received frame count is at least a floor for the run (a stalled/empty capture
  fails rather than trivially passing).

### Report format

```
NDIRECV source=<name> framesReceived=<n> drops=<n> dupes=<n> reorders=<n> \
        avSyncMaxFrames=<n> maxGapFrames=<n> meanRateHz=<f>
```

## Gating and CI

- New CTest `e2e_ndi_output` under an opt-in label **`ndi-output`** (not `ci`, not `e2e`),
  excluded from the default and pre-push selections (add `ndi-output` to the pre-push `-LE`),
  and not selected by GitHub CI's `-L ci`. It runs only via explicit `ctest -L ndi-output`.
- The harness/driver SKIP (exit 77, CTest `SKIP_RETURN_CODE 77`) when: the NDI runtime is not
  installed, the SDK headers/libs were not configured, or no OLR source is discovered within
  a short find timeout — so the lane is a no-op on machines without NDI rather than a failure.

## Error handling

- Bounded capture window with a hard timeout; if the source never appears, SKIP.
- The sender and probe are separate processes; the driver manages lifetimes and cleans up on
  exit (trap), mirroring the existing e2e drivers.
- A transport-level receive error (runtime returns failure) fails the probe distinctly from a
  metric breach, so a broken runtime is not mistaken for a flaky transport.

## Testing

- The marker generate↔decode round-trip (encode index N → render → decode → N; flash/beep
  detection on synthetic frames) is a deterministic **unit test** — no NDI runtime needed.
- The `e2e_ndi_output` gate is the opt-in integration test; a short window (a few seconds)
  for the smoke, env-overridable for a longer soak.
- The probe's analysis functions (counter decode, continuity, A-V offset, cadence) are pure
  and unit-testable against synthetic captured-frame sequences, independent of the runtime.

## Implementation notes and known limitations

Refinements that emerged from running the real loopback (and were verified correct in review):

- **Receive format is UYVY, not I420.** The probe requests NDI's fastest color format, which
  for our no-alpha I420 source is delivered as UYVY. The probe extracts the luma byte
  (`col*2+1`) into a tight plane before decoding the marker. Continuity is therefore a real
  decode, not a vacuous always-zero.
- **A-V sync is jitter around the median offset.** A buffered NDI receiver sees a stable
  audio-vs-video pipeline latency (a constant offset of ~14–16 frames); that is not desync.
  The metric subtracts the median signed offset and reports the max deviation, so a locked
  feed scores 0 and only genuine drift counts. Pinned by `tst_ndirecvanalysis`.
- **Continuity over-reports drops when a reorder occurs.** A single out-of-order frame
  registers as one reorder plus the forward-jump drops it implies. Harmless for the gate
  (all of drops/dupes/reorders must be 0 to pass), but the `drops` number overstates true
  loss for diagnosis when reorders are present.

Known limitation / follow-up:

- **A-V beep↔flash pairing assumes 1:1, in-order chunking.** The probe pairs the kth video
  flash with the kth audio beep (RMS over each received audio frame). It relies on the
  receiver getting one audio frame per video frame, which holds for our sink + loopback but
  is not guaranteed by NDI: under audio re-chunking a beep could be double-counted or diluted
  below the RMS threshold, desynchronizing the pairing. This can only produce a false FAIL
  (never a vacuous pass), so the gate stays trustworthy; a follow-up should match beeps to
  flashes by audio sample position (or detect beep onsets) instead of by ordinal to remove the
  flakiness risk on a loaded self-hosted runner.

## File summary

- Create: `tests/e2e/ndi_output_marker.{h,cpp}` (or an extension of `ndi_marker_pattern`),
  `tests/e2e/ndi_recv_probe.cpp`, `tests/e2e/ndi_output_sender.cpp`,
  `tests/e2e/run_ndi_output_e2e.sh`, `tests/unit/tst_ndioutputmarker.cpp`,
  `tests/unit/tst_ndirecvanalysis.cpp`.
- Modify: `tests/e2e/CMakeLists.txt` (new targets + `e2e_ndi_output` under label `ndi-output`,
  `SKIP_RETURN_CODE 77`), `tests/unit/CMakeLists.txt`, `.githooks/pre-push` (`-LE` add
  `ndi-output`).
- No production source changes.

### Build vs runtime gating

`ndi_recv_probe` and the tier-(a) sender build **unconditionally** — both load the NDI runtime
dynamically via `ndiruntimepaths` and the ABI in `ndiabi.h` (exactly as `NdiOutputSink` does),
so neither needs the NDI SDK headers/libs at build time. Gating is purely at **run** time: the
test SKIPs (exit 77) when the runtime is absent or no source is found. (This differs from the
framesync `ndi_marker_sender`, which links the real NDI SDK and is build-guarded by
`OLR_NDI_SDK_DIR`; the rung-5 lane intentionally avoids that build dependency.)
