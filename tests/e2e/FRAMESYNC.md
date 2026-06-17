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
- **Drift:** least-squares slope of `flash index -> recorded PTS`, plus ppm and
  slip-in-frames over the run. Phase 1 turns this into an A/V-offset drift gate:
  less than one frame over the run.
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

The matrix covers `{lipsync, intercam, drift, timecode} x {srt, rtmp, ndi}`.
SRT is active now. RTMP and NDI cells currently skip with exit code 77 until
their fixtures are wired.

## Environment Knobs

- `OLR_FRAMESYNC_TRANSPORT=srt|rtmp|ndi` selects a transport for
  `run_framesync_e2e.sh`.
- `OLR_FRAMESYNC_SECS=20` sets the recording duration.
- `OLR_FRAMESYNC_GATE=1` makes a scenario enforce its band.
- `OLR_FRAMESYNC_SKEW_PPM=200` injects a deterministic rate skew in the drift
  scenario.
- `OLR_MARKER_TC=10:00:00:00` sets the injected start timecode.
- `OLR_FLASH_CODEC=avc|hevc` selects the marker video codec.

Local FFmpeg builds may omit `drawtext`. In that case the TC marker still sets
container timecode and emits a warning, but it does not burn visible timecode
text into the video.

## Skew Injector

`tests/e2e/lossy_udp_relay.py` has a ppm skew mode used by the drift cell. A
single machine otherwise shares one wall clock, so real drift is nearly zero by
construction. The injector releases downstream SRT data on a stretched or
compressed schedule and records `skew_ppm` plus `data_forwarded` in its stats.

Self-test:

```bash
python3 tests/e2e/lossy_udp_relay.py --selftest
```

## Caveats

Clockless SRT/RTMP can maintain A/V sync and bound inter-camera phase, but true
frame-accurate inter-camera lock requires common timecode or an external
reference. The rig is intentionally honest about that: it reports the achieved
accuracy first, then gates only the guarantees the transport can actually make.
