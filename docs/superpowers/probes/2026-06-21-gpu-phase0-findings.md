# GPU Phase 0 — Gating Probe Findings

Records the measured result and go/no-go decision for each Phase-0 probe
(spec §7, §11). Each probe gates a Phase-2 decision; the CPU pipeline stays the
default and reference throughout.

## P0.1 — macOS VT IOSurface-backed CVPixelBuffer (gates the macOS import edge)

- **Question (§11 Q1a):** does the VT decode session yield an IOSurface-backed
  CVPixelBuffer (precondition for CVMetalTextureCache zero-copy import)?
- **Method:** request `kCVPixelBufferIOSurfacePropertiesKey` on the decode
  session; assert `CVPixelBufferGetIOSurface()!=nullptr` on the decoded buffer
  (`tests/unit/tst_vtiosurface.cpp`).
- **Result:** PASS on macOS 26.5.1 (25F80), Apple M4 Pro, arm64:
  `ctest --test-dir build/c -R tst_vtiosurface --output-on-failure` reported
  `100% tests passed, 0 tests failed out of 1`.
- **Decision:** GO for the macOS zero-copy import edge (`gpu-abstraction`). If
  the buffer were not IOSurface-backed, NO-GO would escalate to a forced
  `CVPixelBufferPoolCreate` with IOSurface attrs or a CPU-detour import.
- **Behavior-preserving:** `ctest --test-dir build/c -R tst_nativevideodecoder
  --output-on-failure` reported `100% tests passed, 0 tests failed out of 1`;
  the CPU readback path is unaffected.

## P0.1b — VT session reconfig cost (gates feed-flip stall budget)

- **Question (§11 Q1b):** at what cost does a mid-stream session reconfig
  (geometry/parameter-set change → reset()+recreate) come?
- **Method:** alternate two distinct-geometry IDR AUs; time `decode()` including
  session recreate, report median over eight reconfigs
  (`TestVtIOSurface::reconfigCostIsBounded`).
- **Result:** median = 5.871 ms on macOS 26.5.1 (25F80), Apple M4 Pro, arm64.
- **Decision:** GO. Median reconfiguration cost is below one 60 fps frame
  interval (~16.7 ms), so the Phase-2 macOS import edge can treat resolution
  flips as frame-boundary work. If this cost regresses above one frame interval,
  `gpu-abstraction` should pre-warm or keep a fallback session for geometry
  changes.
