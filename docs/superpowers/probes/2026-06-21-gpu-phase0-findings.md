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

## P0.2 — Windows MF→ID3D11Texture2D import + RHI D3D backend (gates D4 fence primitive)

- **Question (§11 Q2):** does the Windows import edge have a GPU-resident,
  RHI-importable D3D texture precondition, and which Qt RHI D3D backend fixes
  the matching fence primitive?
- **Method:** `tests/probes/rhi_d3d_probe.cpp` is a Windows-only probe behind
  `OLR_BUILD_GPU_PROBES=ON`. It reports D3D11/D3D12 QRhi availability, wraps a
  synthetic `ID3D11Texture2D` owned by the QRhi device through
  `QRhiTexture::createFrom`, and prints the chosen backend. The real
  MediaFoundation decoder-to-D3D texture binding remains the `gpu-import-win`
  Phase-2 slice; this probe proves the QRhi↔D3D11 import precondition and
  backend selection.
- **Result (macOS host):** off-Windows guard PASS. `cmake --build build/c
  --target rhi_d3d_probe` reports `ninja: error: unknown target
  'rhi_d3d_probe'`, so the probe is not linked into non-Windows builds.
- **Result (Windows runner):** `build-test-windows` now configures with
  `OLR_BUILD_GPU_PROBES=ON`, compiles `rhi_d3d_probe`, and runs it as a
  non-blocking diagnostic because hosted Windows runners may not expose a
  usable D3D device. The diagnostic log captures the four backend/wrap lines on
  any runner with D3D available.
- **Decision (D4):** CI-gated until the Windows run records the chosen backend.
  If D3D11 is chosen, `gpu-sync` uses ID3D11Fence (11.4) /
  keyed-mutex / ID3D11Query rather than assuming D3D12 fences. If D3D12 is
  chosen, `gpu-sync` uses ID3D12Fence.

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

## P0.5 — RHI↔IOSurface interop without a CPU detour (gates the macOS zero-copy edge)

- **Question (§11 Q5):** does RHI↔IOSurface interop work without a CPU detour?
- **Method:** create an IOSurface-backed `CVPixelBuffer` →
  `CVMetalTextureCache` → `id<MTLTexture>` → `QRhiTexture::createFrom` using
  the `MTLDevice` owned by the QRhi (`rhi_import_probe --interop`).
- **Result:** `RHI<->IOSurface interop: OK (zero-copy wrap of an
  IOSurface-backed Metal texture)` on macOS 26.5.1 (25F80), Apple M4 Pro, arm64.
- **Decision:** GO. `gpu-abstraction` can import VT IOSurfaces into RHI
  zero-copy via `CVMetalTextureCache` + `QRhiTexture::createFrom`. If this
  regresses, fall back to a Metal blit into an RHI-owned texture (still
  GPU-resident, one extra GPU copy, no CPU detour) and record that import-edge
  cost.

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

## P0.6 — Golden-fixture color-tag audit + default-tagging policy (gates color-metadata no-op)

- **Question (§11 Q6):** do existing golden fixtures carry color tags? If not,
  what default-tagging policy makes color-metadata a provable no-op?
- **Method:** `tests/probes/audit_fixture_color_tags.sh` regenerates a fixture
  via the e2e ffmpeg recipe and ffprobes its color metadata.
- **Result:** `color_space=unknown color_primaries=unknown
  color_transfer=unknown color_range=tv`; `CLASSIFICATION: UNTAGGED`.
- **Finding:** fixtures do not carry colorimetry tags (matrix/primaries/transfer
  are unknown). The e2e recipe sets no `-color_*` flags; the range is `tv`
  (video). The height>576 heuristic in `qtpreviewsink.cpp:23-25` is the only
  matrix decision today.
- **Default-tagging policy (gates color-metadata Phase 1):** an untagged frame
  gets `ColorMetadata{ matrix = height>576 ? Bt709 : Bt601, range=Video,
  primaries/transfer matching matrix, chromaFormat=Yuv420, bitDepth=8 }`,
  byte-identical to today. A tagged fixture honors its tags; re-goldening a
  tagged fixture is a deliberate future event, not an accidental regression.
- **Decision:** GO. Color-metadata Phase 1 can land as a proven no-op for current
  fixtures; the audit confirms no fixture currently forces an appearance change.

## Phase-0 sign-off matrix

| probe | measured result | gates | decision |
|------|-----------------|-------|----------|
| P0.1 | VT decode yields IOSurface-backed `CVPixelBuffer` | macOS import edge | GO |
| P0.1b | VT reconfig median 5.871 ms (<16.7 ms frame) | feed-flip stall budget | GO |
| P0.2 | Windows `rhi_d3d_probe` compiles in CI and runs as a diagnostic; off-Windows target absent | D4 fence primitive | CI-GATED |
| P0.3 | import→composite→synchronous-readback median 1.7763 ms (>0.5 ms) | D1/D11 | CONDITIONAL / NO-GO for synchronous hot-path readback |
| P0.4 | sinks classified as GPU-native vs CPU-readback cadence paths | D10 routing | GO |
| P0.5 | IOSurface→Metal→QRhiTexture wraps without CPU detour | macOS zero-copy edge | GO |
| P0.6 | fixtures are untagged except video range; default policy matches current heuristic | color-metadata no-op | GO |

## Default-lane verification

- `cmake -S . -B build/unit -G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`: PASS
  (`OLR_BUILD_GPU_PROBES` left at default `OFF`).
- `cmake --build build/unit`: PASS (`651/651` targets).
- `ctest --test-dir build/unit -L unit --output-on-failure`: PASS (`76/76`
  tests, `100% tests passed`, real time 32.75 sec).
