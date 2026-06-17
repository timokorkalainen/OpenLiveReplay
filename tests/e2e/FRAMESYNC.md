# Frame-Sync Acceptance Rig

This rig is the acceptance instrument for broadcast ingest timing work. Every
later timing phase is measured against these cells before its claims become
gates.

## What It Measures

- **Lip-sync:** nearest beep onset minus flash onset. The current gate is EBU
  R37-compatible at `-40..+60 ms` for `audio - video`; the target band is
  `+/-20 ms`.
- **Inter-camera phase:** flash-onset spread across two synchronized views.
  With common timecode this becomes a `<= 1 frame` gate; without common TC it is
  a bounded report, because clockless IP cannot prove frame-accurate phase.
- **Drift:** least-squares slope of `flash index -> recorded PTS`, recovered
  source-clock ppm, and A/V offset drift over the run. The zero-skew CTest cell
  gates A/V offset regression drift at less than one frame; video flash slope is
  reported as a diagnostic because onset quantization can move by a frame on
  short local runs. The injected-skew cell is report-only, but it fails if the
  rig cannot observe a nonzero recovered clock ppm.
- **Timecode:** recorded MKV `tmcd` or `timecode` tag versus the injected
  `OLR_MARKER_TC`. Until the engine writes `tmcd` in Phase 3, this reports
  `n/a` rather than failing.

## Running It

Build `sync_harness`, then run the CTest label:

```bash
cmake --build build/bcast --target sync_harness
( cd build/bcast && ctest -L framesync --output-on-failure )
```

Run the full transport matrix directly:

```bash
tests/e2e/run_framesync_matrix.sh build/bcast/tests/e2e/sync_harness
```

The matrix covers `{lipsync, intercam, drift, drift_skew, timecode} x
`{srt, rtmp, ndi}`. SRT is active by default. NDI is active when the optional
local `ndi_marker_sender` target is built from an installed NDI SDK/runtime.
RTMP cells currently skip with exit code 77 until their marker-source fixture is
wired.

For NDI cells, `run_framesync_e2e.sh` starts a local `ndi_marker_sender`
process that publishes one or two deterministic `OLR-FS-*` sources. The sender
emits the same full-frame flash, 1 kHz beep, fixed timecode, and optional skew
pattern as the SRT fixture, but directly through the NDI SDK.

## Timing Core Status

Phase 1 is live for native SRT/RTMP:

- `SourceClock` maps sender timestamps to the recording timeline. SRT uses PCR
  quality with exact 90 kHz units; RTMP uses FLV millisecond quality.
- `DriftEstimator` exposes recovered clock ppm, and `sync_harness
  --report-stats` prints `clockppm` and `clockq`.
- `SourceClock::toSessionMs()` applies the recovered sender/session slope when
  mapping media timestamps, so video frames and audio chunks share one corrected
  timeline. The record-side FIFO consumes that common timeline without an extra
  audio-only ppm correction.

`StreamWorker` owns the per-backend source clocks and passes them into recreated
native sessions, so same-URL reconnects can retain recovered clock state. A
source URL/backend change resets the owned clocks.

## Environment Knobs

- `OLR_FRAMESYNC_TRANSPORT=srt|rtmp|ndi` selects a transport for
  `run_framesync_e2e.sh`.
- `OLR_FRAMESYNC_SECS=20` sets the recording duration.
- `OLR_FRAMESYNC_GATE=1` makes a scenario enforce its band.
- `OLR_FRAMESYNC_SKEW_PPM=200` injects deterministic media PTS/PCR skew in the
  `drift_skew` scenario.
- `OLR_MARKER_TC=10:00:00:00` sets the injected start timecode.
- `OLR_FLASH_CODEC=avc|hevc` selects the marker video codec.
- `OLR_NDI_MARKER_SENDER=/path/to/ndi_marker_sender` overrides the auto-detected
  sibling of `sync_harness`.
- `OLR_NDI_DISCOVERY_SECS=2` controls how long the driver waits for the local
  marker source to appear in NDI discovery before recording.

Local FFmpeg builds may omit `drawtext`. In that case the TC marker still sets
container timecode and emits a warning, but it does not burn visible timecode
text into the video.

## Skew Injector

The SRT drift skew cell uses `OLR_MARKER_SKEW_PPM` internally to
stretch/compress the FFmpeg marker media timestamps before the UDP->SRT bridge.
The NDI drift skew cell asks `ndi_marker_sender` to pace its SDK submissions at
the requested ppm offset. A single machine otherwise shares one wall clock, so
real drift is nearly zero by construction.

`tests/e2e/lossy_udp_relay.py` still has a ppm packet-release skew self-test for
network impairment experiments, but frame-sync acceptance does not treat packet
delay as media-clock skew.

Self-test:

```bash
python3 tests/e2e/lossy_udp_relay.py --selftest
```

## Caveats

Clockless SRT/RTMP can maintain A/V sync and bound inter-camera phase, but true
frame-accurate inter-camera lock requires common timecode or an external
reference. The rig is intentionally honest about that: it reports the achieved
accuracy first, then gates only the guarantees the transport can actually make.
