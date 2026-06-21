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

## P0.3 — RHI per-frame overhead on import→composite→readback (gates D1/D11)

- **Question (§11 Q3):** is RHI per-frame overhead within the <0.5 ms budget on
  an import→composite→readback path, and does the one-RHI render-thread model
  hold?
- **Method:** `tests/probes/rhi_import_probe` with `OLR_BUILD_GPU_PROBES=ON`;
  one QRhi(Metal) created on a dedicated render thread, 1920×1080, 240 frames
  (20 warm-up), median of upload+offscreen pass+copy+readback. Exact RHI
  headers pinned to Qt 6.10.1 private headers:
  `$QT_PREFIX/lib/QtGui.framework/Versions/A/Headers/6.10.1/QtGui/rhi/qrhi.h`
  and `qrhi_platform.h`.
- **Result:** `RHI per-frame overhead: 1.7763 ms (import=0.0010
  composite=0.5735 readback=1.1789)`; `budget=0.5000 ms -> OVER` on macOS
  26.5.1 (25F80), Apple M4 Pro, arm64.
- **Decision:** CONDITIONAL / NO-GO for any synchronous per-frame
  import→composite→readback path in the hot budget. Readback dominates the
  measured cost, so Phase 2 may keep the RHI spine and single-render-thread
  model behind `OLR_GPU_PIPELINE`, but it must not claim synchronous readback
  fits the <0.5 ms budget. Later `async-readback` work must pipeline readback
  (render N / read N-2) or keep GPU-native sinks on texture paths before this
  becomes a production cadence path.
- **D11 note:** the probe creates and runs the QRhi loop off the GUI thread; the
  dedicated render-thread model held for the measured path.

## P0.4 — Sink GPU-texture vs CPU-frame capability (gates D10 routing + new-io-targets)

- **Question (§11 Q4):** which sinks accept GPU textures vs require CPU frames?
- **Method:** inspect implemented sinks (NDI/Qt preview) in-tree; classify
  enumerated DeckLink/AJA/OMT targets from SDK interchange types.

| sink | interchange today | classification (D10) | source |
|------|-------------------|----------------------|--------|
| Qt preview | `QVideoFrame` mapped and filled with CPU plane copies | `AsyncReadbackDedupOk` | `playback/output/qtpreviewsink.cpp:20-41` |
| NDI | `NDIlib_video_frame_v2_t` with CPU `p_data` (`I420`) | `NeedsContinuousCadence` (`maxGap<=2`) | `playback/output/ndisink.cpp:88-112` |
| DeckLink | `IDeckLinkVideoFrame` CPU bytes; GPUDirect on some SDK/SKU paths | `GpuNative` where SDK allows, else `NeedsContinuousCadence` | SDK survey |
| AJA NTV2 | AutoCirculate host buffers | async readback (CPU-frame) | SDK survey |
| OMT | software SDK CPU frame | async readback (CPU-frame) | SDK survey |

- **Decision:** GO. `async-readback` routes NDI/preview through the CPU
  readback edge (D7); `new-io-targets` treats AJA/OMT as CPU-frame
  async-readback and DeckLink as GPU-native only where the SDK exposes it. No
  sink blocks the program: forced readback is "needs-readback", not a blocker
  (spec §10).
