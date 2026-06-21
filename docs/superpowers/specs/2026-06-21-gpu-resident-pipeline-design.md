# GPU-Resident Video Pipeline — Design Spec

- **Date:** 2026-06-21
- **Status:** Draft for review (design approved in brainstorming; spec under review)
- **Scope:** Program-level design. Each subproject below gets its own spec + implementation plan.
- **Approach:** Keystone-first, strict zero-regression gates. Everything ships behind capability
  flags; the CPU path stays default and becomes the permanent correctness reference + fallback.
  No throwaway prototypes — every artifact is production and stays in the tree.

---

## 1. Motivation & goals

Re-architect the playback/output pipeline so decoded frames stay resident as GPU textures from
decode → composite → output, with CPU readback only at the edges that genuinely require it. The
user confirmed **all** of the following as drivers (none is secondary):

- **CPU headroom / more feeds** — eliminate per-frame GPU→CPU readback, the NV12→I420 deinterleave,
  the nearest-neighbor CPU composite, and the preview upload.
- **Image quality** — replace nearest-neighbor multiview scaling with GPU bilinear/Lanczos; do
  correct in-shader BT.709/601 color + range instead of today's height-inferred guess.
- **New GPU-only capabilities** — motion-interpolated slow-mo, GPU overlays/DVE/wipes, real-time
  effects, eventually 4K/UHD multiview.
- **Future I/O** — the enumerated-but-unbuilt DeckLink SDI/HDMI and ST2110 IP outputs.
- **Raw performance** — lower latency and steadier cadence on the ~1 ms output tick.

### Platforms
macOS, iOS (both Metal) and Windows (D3D) are **all first-class**. Consequence: a portable shader
spine (Qt RHI) owns the middle; only the decode-import and output-export edges are native. iOS
bring-up is deferred to Phase 5 but the edge interface stays platform-symmetric from day one.

### Non-goals (YAGNI)
- No deep decoded replay buffer in VRAM. The existing sliding-window-over-compressed-history model
  is retained (see §2). Deep history stays compressed (MKV).
- No Linux/VAAPI backend (not a current target).
- No speculative GPU effects beyond what a driver subproject scopes.

---

## 2. Current architecture (baseline)

The pipeline is fully **CPU-resident** for pixels. The universal frame type is `MediaVideoFrame`
([playback/output/mediaframe.h](../../../playback/output/mediaframe.h)) — three `QByteArray`
YUV420P planes in RAM, value/COW semantics.

Per-frame copies across GPU boundaries today:

1. **Decode (HW) → CPU.** VideoToolbox decodes to an IOSurface-capable `CVPixelBuffer` (NV12),
   then `copyPixelBufferToAvFrame`
   ([nativevideodecoder_videotoolbox.mm:129-191](../../../recorder_engine/ingest/nativevideodecoder_videotoolbox.mm))
   locks it and `memcpy`s down to a CPU `AVFrame`, deinterleaving NV12→I420 on the CPU.
2. **Store.** `TrackBuffer` ([playback/trackbuffer.h](../../../playback/trackbuffer.h)) holds a
   **decoded-frame window** capped at `kGlobalFrameBudget = 256` frames total across feeds
   ([playbackworker.h:152](../../../playback/playbackworker.h)); deep history is compressed.
   `OutputFrameCache` holds normalized per-feed output frames; published immutably via
   `SharedCacheSlot` (`shared_ptr<const OutputFrameCache>`,
   [sharedcacheslot.h](../../../playback/output/sharedcacheslot.h)).
3. **Composite.** `Yuv420pCompositor::composeGrid`
   ([yuv420pcompositor.cpp:8-61](../../../playback/output/yuv420pcompositor.cpp)) — CPU
   **nearest-neighbor** grid blit (`scalePlaneNearest`).
4. **Sinks.** NDI (`packI420`, [ndisink.cpp](../../../playback/output/ndisink.cpp)), Qt preview
   (`toQVideoFrame` memcpy, [qtpreviewsink.cpp](../../../playback/output/qtpreviewsink.cpp)), and
   H.264 encode all consume CPU frames. The encoder **re-uploads** a CPU frame to a `CVPixelBuffer`
   (`makeI420PixelBuffer`,
   [nativevideoencoder_videotoolbox.mm:27-52](../../../recorder_engine/codec/nativevideoencoder_videotoolbox.mm)).

### VRAM budget (for a GPU-resident port)
The decode window is design-bounded at 256 frames (~0.76 GiB at 1080p NV12), **fixed regardless of
feed count** (8 feeds → 32 frames each). A faithful port that keeps normalized per-feed output
caches lands ~1.8 GiB at 8×1080p; an efficient port whose compositor samples decode textures
directly trends toward the ~0.85 GiB floor. On Apple Silicon (unified memory) the "VRAM constraint"
is mild; it is real on discrete-GPU Windows. **Caveat (from review):** under multiview the peak is
`N-feeds × window + staging-bank + compositor-output-cache`, not a single flat budget — the GPU
budget must account for the per-feed multiplier (§6 `gpu-budget`).

---

## 3. Target architecture

```
                    ┌─────────────── native edge ───────────────┐
 ingest ─► decode (VideoToolbox / MediaFoundation)               │
          │  HW: keep IOSurface/CVPixelBuffer or D3D11 texture    │
          │  SW (MPEG-2/NDI ingest): CPU-origin                   │
          ▼                                                       │
   ┌──────────────┐   FrameHandle (GPU- or CPU-backed, immutable, refcounted)
   │ FrameHandle  │◄──────────────────────────────────────────────┘
   └──────┬───────┘
          ▼
   TrackBuffer / OutputFrameCache  (store handles; metadata-keyed dedup/memo)
          ▼
   ┌────────────────────── RHI spine (portable shaders) ──────────────────────┐
   │  GPU compositor: YUV→RGB (BT.601/709), bilinear/Lanczos scale, grid,      │
   │  PGM select, overlays, interpolation                                      │
   └──────────────────────────────┬────────────────────────────────────────────┘
                                   ▼ GpuSurface output
        ┌──────────────── dispatch (fenced) ─────────────────┐
        │ GPU-native sinks: encode, future SDI  ── stay GPU   │
        │ CPU-only sinks: NDI, preview, screenshot ── async   │
        │   readback (render N, read N-2); PGM sub-frame mode  │
        └─────────────────────────────────────────────────────┘
```

- **RHI spine, native edges.** Qt RHI (Metal/D3D11) owns the compositor/shader middle — one
  GLSL→`.qsb` codebase, plus a deterministic WARP reference on Windows CI. Native only at
  decode-import (CVPixelBuffer→Metal via `CVMetalTextureCache`; MF→`ID3D11Texture2D`) and
  output-export.
- **Mixed-origin via one handle.** HW-decoded frames are GPU-origin; MPEG-2 software decode and NDI
  ingest are CPU-origin. A single `FrameHandle` self-describes residency; consumers call
  `readToCpu()` (no-op for CPU-origin, fenced download for GPU-origin). No residency-forcing, no
  duplicate cache classes.

---

## 4. Keystone — the `FrameHandle` seam

The program pivots on replacing `MediaVideoFrame` with an opaque, ref-counted, **immutable** handle
separating metadata (CPU-cheap) from pixel residency (CPU or GPU).

- **`FrameMetadata`** — `width, height, stride[3], format, feedIndex, ptsMs, outputFrameIndex,
  isPlaceholder` + **`ColorMetadata`** (`matrix` 601/709/2020, `range` video/full, `primaries`,
  `transfer`, `chromaFormat`, `bitDepth`). This struct — not pixel content — is the key for the
  dispatcher's identity-skip dedup and the multiview memo descriptor.
- **`FrameHandle`** — value wrapper over `shared_ptr<const IFrameData>`. **Copy = refcount bump**
  (cheap, aliasing). Contents immutable, so a handle shared into the memo or a published snapshot is
  never mutated by a later composite.
- **`IFrameData`** — `isGpuBacked()`/`isCpuBacked()`; `readToCpu() → CpuPlanes` (no-op for
  CPU-origin, fenced GPU→CPU download for GPU-origin — the single chokepoint every CPU sink funnels
  through); `gpuSurface() → GpuSurface*` (null on CPU frames).

**Copy/equality contract (explicit, from review):** a handle copy is a refcount bump; identity is
by `FrameMetadata`, never pixel content. A unit test pins that a memoized GPU handle is not mutated
by a later composite (the multiview memo's byte-identical-reuse and identity-skip must survive
aliasing).

**Zero-regression gate:** Phase 1 ships **CPU-backed only**. `readToCpu()` returns the existing
`QByteArray` planes; all ~30+ access sites — including `OutputBusFrame`-by-value and
`MultiviewComposite.video` — funnel through the handle API. The gate is **the entire existing unit
+ e2e suite byte-for-byte green** before any GPU code exists.

---

## 5. Design-decision register

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Portable shader spine | **Qt RHI**, native only at import/export edges | All 3 platforms first-class; 3× native multiplies the test matrix; RHI gives WARP determinism on Windows CI. IOSurface/D3D-texture interop is native regardless — isolate at edges. Budget <0.5 ms/frame RHI overhead, **measured in Phase 0** before committing. |
| D2 | On-GPU canonical layout | **Keep NV12, deinterleave chroma in shader** | Zero import conversion, matches decoder output, lowest VRAM. Cost: golden reconstruction must model NV12 chroma rounding *on top of* nearest-neighbor (see §9). |
| D3 | Golden strategy | **CPU compositor = permanent reference + fallback**; GPU exact on WARP, ±1 LSB on local-GPU lane | macOS has no deterministic headless Metal, so a CPU reference is needed regardless; it doubles as runtime fallback (RHI-unavailable / GPU-OOM). Integer-reconstruct shader math for WARP-exact; reserve ε for vendor-rounding on the local GPU lane. |
| D4 | GPU threading | **Fenced multi-thread mirroring the snapshot model** + GPU generation counter | Worker produces surfaces, signals a decode-done fence before publish; output thread waits before reading; eviction waits on render fences. Minimal extension of the proven lock-light producer/consumer; directly closes the TSan-invisible races. Single-GPU-thread would serialize decode+dispatch. |
| D5 | Keep CPU pipeline as runtime fallback | **Yes, permanent**, behind the compositor-select flag, default until GPU proven per-platform | It is the CI oracle, the headless-macOS path, and the safe degradation target. Handle abstraction contains divergence to the compositor + sink-readback layers. |
| D6 | Mixed-origin frames | **Single self-describing handle**; `readToCpu()` no-op (CPU) / fenced download (GPU) | MPEG-2 SW + NDI ingest are CPU-origin; HW decode GPU-origin. Forcing uniform residency wastes uploads or defeats zero-copy. Compositor uploads a CPU-origin input on demand only when it composites it. |
| D7 | Readback placement | **Async pipelined** (render N, read N-2), one readback per output frame fanned out to all CPU sinks; **dedicated sub-frame mode for operator PGM preview from day one** | Sync readback stalls the 1 ms cadence at 60 fps. PGM preview gets a low-latency path up front (user decision); NDI/others accept 2–3 frames. GPU-native sinks bypass via `SinkGpuCapability`. |
| D8 | iOS timing | **Symmetric edge interface now, bring-up Phase 5** | No macOS-only architecting, but don't pay iOS main-thread/thermal/VRAM cost before the spine is proven. |

---

## 6. Subproject decomposition

IDs, dependencies, and sizes. The decomposition incorporates the adversarial review's additions
(format canonicalization, shader toolchain, AV-sync-under-readback, device-loss, telemetry
contract, recorder-encode rescope, iOS).

| id | name | depends on | risk | size |
|----|------|-----------|------|------|
| `frame-handle` | Opaque ref-counted FrameHandle + FrameMetadata + copy/equality contract; CPU-only, zero-regression | — | high | XL |
| `color-metadata` | `ColorMetadata` plumbed decode→sink; replace height>576 heuristic; **proven no-op on goldens first** | `frame-handle` | medium | M |
| `telemetry-contract` | Extend harness counter contract: VRAM occupancy, readback queue depth/drops, fence-wait stalls, GPU-OOM-degrade, **copy-on-GPU-path detector** (makes slice criteria measurable) | `frame-handle` | medium | M |
| `format-canon` | Decide + enforce on-GPU chroma layout (D2: NV12 kept, shader deinterleave); every shader/golden/encoder agrees | `frame-handle` | medium | S |
| `shader-toolchain` | `qt_add_shaders`/`qsb` in CMake, shader source layout, Metal/HLSL/SPIR-V cross-compile, CI shader-compile step | `format-canon` | medium | M |
| `gpu-abstraction` | `GpuSurface` (IOSurface/CVPixelBuffer, ID3D11Texture2D), RHI device bring-up, VT keep-surface import + lazy `readToCpu`; Windows MF-D3D11 import as parallel deliverable | `frame-handle`, `format-canon` | high | XL |
| `gpu-sync` | Fences (MTLSharedEvent/D3D12) + GPU generation counter + eviction guard; **explicit lock-hierarchy care** (`m_outputRuntimeMutex → m_bufferMutex`); armed-cut staging swap fence | `gpu-abstraction` | high | L |
| `gpu-compositor` | RHI grid + PGM select + YUV→RGB + bilinear/Lanczos; CPU compositor retained as reference; GPU texture memo | `gpu-abstraction`, `color-metadata`, `shader-toolchain` | high | L |
| `async-readback` | `AsyncGpuReadbackSink` (N-deep ring) + `SinkGpuCapability` routing; **PGM sub-frame mode**; **AV-sync-under-lag as hard gate**; NDI migration | `gpu-sync`, `telemetry-contract` | high | L |
| `gpu-budget` | VRAM-bounded GPU window (per-feed × staging multiplier) + OOM-safe degrade to CPU handle + seek-prefetch | `gpu-sync` | medium | M |
| `device-loss` | RHI device-removed detection, surface invalidation, spine rebuild, degrade-to-CPU policy + fault-injection e2e | `gpu-sync` | medium | M |
| `gpu-encode` | GPU-native encode **rescoped to recorder `StreamWorker`** (separate jitter-pull pipeline), not a playback sink; color VUI authored from `ColorMetadata` | `gpu-sync`, `color-metadata` | medium | M |
| `ios-bringup` | iOS Metal bring-up + main-thread-render/thermal handling + smoke tests | `gpu-compositor`, `async-readback` | medium | M |
| `new-io-targets` | DeckLink SDI/HDMI, ST2110, AJA, OMT sinks — GPU-texture where SDK allows, async readback otherwise | `async-readback`, `gpu-budget` | high | L |

---

## 7. Phasing & sequencing

Keystone-first, strict gates. Each phase gates the next; everything behind flags; CPU default.

- **Phase 0 — Gating probes** (mostly measurement; the one code change — requesting IOSurface
  backing on the VT decode session — is the first piece of the real import edge, not a throwaway):
  1. Confirm VideoToolbox can produce an **IOSurface-backed, RHI-importable** surface and at what
     reconfig cost (today the decode session does **not** request `kCVPixelBufferIOSurfacePropertiesKey`).
  2. Measure RHI per-frame overhead vs the <0.5 ms budget (RHI is not currently linked).
  3. Probe NDI / DeckLink / AJA GPU-texture capability per platform.
  4. Confirm RHI↔IOSurface / RHI↔D3D-texture interop (`copyTexture` or native detour).
  5. Audit whether existing golden fixtures carry SPS-VUI / CMFormatDescription color tags; if
     untagged, define a default-tagging policy so `color-metadata` is a provable no-op.
- **Phase 1 — Keystone (CPU-only, byte-green gate):** `frame-handle`, `color-metadata` (no-op
  proven), `telemetry-contract`.
- **Phase 2 — GPU edge + permanent vertical slice:** `format-canon`, `shader-toolchain`,
  `gpu-abstraction`, and the slice (§8).
- **Phase 3 — Sync + compositor:** `gpu-sync`, `gpu-compositor`, **multi-feed cap-pressure stress**
  (the single-feed slice never exercises evict-while-render or VRAM pressure).
- **Phase 4 — Output at scale:** `async-readback` (AV-sync-under-lag hard gate + PGM sub-frame
  mode), `gpu-budget`, `device-loss`.
- **Phase 5 — Capabilities:** `gpu-encode`, `ios-bringup`, `new-io-targets`.

---

## 8. The permanent vertical slice (Phase 2)

macOS only, one feed, behind `OLR_GPU_PIPELINE` (default off): VideoToolbox decodes H.264 keeping
the IOSurface-backed `CVPixelBuffer`, wraps it in a GPU-backed `FrameHandle`, flows through the
existing single-source render path, and reaches Qt preview via one lazy readback. No compositor
changes, no multiview, no NDI. **Ships and stays** (CPU path remains default + reference).

**Note (from review):** the real preview consumer is `FrameProvider` (holds `m_lastFrame`, serves
`latestImage()→toImage()` for screenshots, broadcasts `QVideoFrame` cross-thread to `QVideoSink` on
the GUI thread). The slice must address screenshot CPU-QImage path and GUI-thread affinity, not just
`QtPreviewSink`.

**Success criteria:**
- `OLR_GPU_PIPELINE=0`: entire existing unit + `e2e_play` suite byte-for-byte green (handle
  migration is a true no-op).
- `OLR_GPU_PIPELINE=1` on a GPU-capable macOS host: single-feed `play1x` shows correct picture,
  `placeholderFramesDelta==0`, `heldFramesDelta==0` (no gray flash / stalls from readback).
- No full-frame GPU→CPU download except the single preview readback (verified by the
  `copy-on-GPU-path detector` counter from `telemetry-contract`).
- GPU-surface readback matches CPU-path decode of the same frame within **±1 LSB/channel**.
- No new ASan/UBSan findings; seek-under-decode stress does not crash or read a freed surface.

---

## 9. Correctness & test strategy

- **CPU compositor as permanent oracle** (D3/D5). GPU output validated against it: **exact on
  Windows WARP** (integer-reconstructed shader math), **±1 LSB on a local-GPU lane**.
- **NV12 double-rounding (D2):** the determinism harness must reconstruct goldens from NV12 chroma
  (interleaved UV, shader deinterleave) *plus* nearest-neighbor scaling — two rounding sources, not
  one. This is an explicit harness requirement, not an afterthought.
- **GPU race stress as first-class gates** (TSan cannot see GPU command ordering): seek-under-decode,
  armed-cut-while-render-pending, evict-while-render, frame-checksum validators. Multi-feed +
  cap-pressure scenario added in Phase 3 (the slice excludes it).
- **AV-sync under readback lag** (hard gate inside `async-readback`, before NDI migrates): define how
  audio stays sample-aligned when video is delayed N frames; the **AV-sync MAX gate (100 ms)** and
  **NDI marker-continuity (`maxGap≤2`)** must pass through the readback path. `OutputBusFrame` carries
  video+audio together — this is the single most likely regression.
- **Telemetry counter contract** (`telemetry-contract`): existing gate counters
  (`placeholderFramesDelta`, `heldFramesDelta`, `maxClockDivergenceMs`, `decodedVideoFrames`,
  `stagingVideoFramesDecoded`) keep their meaning; new counters added and asserted in `play_harness`.
- **Concurrency review** (per CLAUDE.md): the playback worker's threading + every `gpu-sync` change
  gets an independent review before merge.

---

## 10. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Keystone churn corrupts pipeline (30+ sites + by-value `OutputBusFrame`/`MultiviewComposite`) | Land CPU-only first; whole suite byte-green as gate; explicit copy/equality contract + aliasing unit test |
| TSan-invisible GPU races | Fences + GPU generation counter + checksum stress gates as first-class |
| **Lock × fence deadlock** | `TrackBuffer::insert` evicts under `m_bufferMutex` in the decode hot loop; a render-fence wait held across that mutex can cycle with the `m_outputRuntimeMutex → m_bufferMutex` order. Design eviction to drop the mutex before fence-wait; review the lock+fence hierarchy explicitly |
| Pixel-exact golden loss / NV12 double rounding | CPU reference oracle; integer-reconstruct for WARP-exact; ε only on local-GPU lane; harness models NV12 chroma rounding |
| Readback stall on 1 ms cadence | Async pipelined, one copy per output frame fanned out; identity-skip dedup; PGM sub-frame mode profiled |
| **VRAM blowup under multiview** | GPU budget accounts for `N-feeds × window + staging-bank + compositor-output-cache`, not a flat 32–64; OOM-safe degrade to CPU; never let GPU alloc failure crash decode |
| GPU device loss (hard-down for live) | `device-loss` subproject: detect, invalidate, rebuild spine, degrade to CPU; fault-injection e2e |
| Audio drift under readback lag | AV-sync-under-lag hard gate before NDI migration |
| SDK GPU-capability unknowns | Phase 0 probes; treat forced-readback as `needs-readback`, not blocker |
| Color regression (zero metadata today) | `color-metadata` early; extract from CMFormatDescription/IMFMediaType/SPS-VUI; round-trip unit test; fixture color-tag audit |
| Platform-sequencing debt | Keep `GpuSurface` edge interface platform-symmetric; Windows MF-D3D11 import parallel inside `gpu-abstraction` |

---

## 11. Open questions (resolved in Phase 0)

1. Does the VT decode session yield an IOSurface-backed, RHI-importable buffer, and at what reconfig
   cost?
2. Is RHI per-frame overhead within the <0.5 ms budget on representative hardware?
3. Which sinks (NDI/DeckLink/AJA/Qt preview) actually accept GPU textures per platform?
4. Does RHI↔IOSurface / RHI↔D3D-texture interop work without a CPU detour?
5. Do existing golden fixtures carry color tags? If not, what is the default-tagging policy?
6. Is macOS CI headless (no GPU)? If so, the GPU compositor's automated correctness coverage is
   WARP-on-Windows + local-GPU only — confirm and document.

---

## 12. Subproject lifecycle

Each subproject (§6) becomes its own `docs/superpowers/specs/` spec + implementation plan when its
phase is reached, following the existing per-feature spec convention in this directory. This
document is the program-level umbrella they hang from.
