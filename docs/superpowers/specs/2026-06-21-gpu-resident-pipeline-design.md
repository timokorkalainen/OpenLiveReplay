# GPU-Resident Video Pipeline — Design Spec

- **Date:** 2026-06-21
- **Status:** Draft, revised after an independent fresh-agent review (verdict: proceed-with-fixes;
  all code claims independently verified). Spec under user review.
- **Scope:** Program-level design. Each subproject below gets its own spec + implementation plan.
- **Approach:** Keystone-first, strict zero-regression gates. Everything ships behind capability
  flags; the CPU path stays default and becomes the permanent correctness reference + fallback.
  No throwaway prototypes — every artifact is production and stays in the tree.

---

## 1. Motivation & goals

Re-architect the playback/output pipeline so decoded frames stay resident as GPU textures from
decode → composite → output, with CPU readback only at the edges that genuinely require it.

**Load-bearing drivers (committed scope of this program):**

- **CPU headroom / more feeds** — eliminate per-frame GPU→CPU readback, the NV12→I420 deinterleave,
  the nearest-neighbor CPU composite, and the preview upload.
- **Image quality** — replace nearest-neighbor multiview scaling with GPU bilinear/Lanczos; do
  correct in-shader BT.709/601 color + range instead of today's height-inferred guess.
- **Raw performance** — lower latency and steadier cadence on the ~1 ms output tick.
- **Future I/O** — the enumerated-but-unbuilt DeckLink SDI/HDMI and ST2110 IP outputs.

**Enabled future capabilities (NOT committed scope; the architecture makes them possible, each
gets its own spec + correctness strategy later — Phase 5+):**

- **Motion-interpolated slow-mo.** Today's slow-mo is pure frame-repeat
  (`OutputFrameClock`: `mediaFrameDelta = frameDelta * speed`). True temporal interpolation cannot
  be validated by the CPU-compositor oracle (§9), so it needs a dedicated subproject with its own
  reference/perceptual correctness approach — out of scope here, not load-bearing.
- **GPU overlays / DVE / wipes / real-time effects.** No graphics source or data model exists in
  the tree today. These are bounded **extension points** the GPU compositor enables, not deliverables
  of this program.

### Platforms
macOS, iOS (both Metal) and Windows (D3D) are **all first-class**. Consequence: a portable shader
spine (Qt RHI) owns the middle; only the decode-import and output-export edges are native. iOS
bring-up is deferred to Phase 5 but the edge interface stays platform-symmetric from day one.

### Non-goals (YAGNI)
- No deep decoded replay buffer in VRAM. The existing sliding-window-over-compressed-history model
  is retained (see §2). Deep history stays compressed (MKV).
- **No 10-bit / HDR in this program.** The pipeline is structurally 8-bit today
  (`MediaPixelFormat ∈ {Invalid, Yuv420p}`; both codecs 8-bit; the VT decoder pins 8-bit NV12; D2
  keeps NV12). A 10-bit/HDR path (P010, PQ/HLG, Main10) is a future program; `ColorMetadata.bitDepth`
  is carried **fixed at 8** for forward-compatibility only.
- No motion interpolation or overlay/DVE compositing in committed scope (see "Enabled future
  capabilities" above).
- No Linux/VAAPI backend (not a current target).

---

## 2. Current architecture (baseline)

The pipeline is fully **CPU-resident** for pixels. The universal frame type is `MediaVideoFrame`
([playback/output/mediaframe.h](../../../playback/output/mediaframe.h)) — three `QByteArray`
YUV420P planes in RAM, value/COW semantics.

Per-frame copies across CPU↔GPU boundaries today (a cross-cutting inventory, not a single pipeline):

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
   ([yuv420pcompositor.cpp:23-61](../../../playback/output/yuv420pcompositor.cpp), via the
   file-local `scalePlaneNearest` helper at :8-20) — CPU **nearest-neighbor** grid blit.
4. **Sinks (playback `OutputDispatcher`).** NDI (`packI420`,
   [ndisink.cpp](../../../playback/output/ndisink.cpp)) and Qt preview (`toQVideoFrame` memcpy,
   [qtpreviewsink.cpp](../../../playback/output/qtpreviewsink.cpp)) consume CPU frames.
   **Separately**, H.264 **encode-on-record** lives in the recorder's `StreamWorker`
   ([recorder_engine/streamworker.cpp](../../../recorder_engine/streamworker.cpp)) — a distinct
   jitter-pull pipeline, cleanly disjoint from the playback `OutputDispatcher` — where it
   **re-uploads** a CPU frame to a `CVPixelBuffer` (`makeI420PixelBuffer`,
   [nativevideoencoder_videotoolbox.mm:27-52](../../../recorder_engine/codec/nativevideoencoder_videotoolbox.mm)).
   `gpu-encode` (§6) targets that recorder path, not a playback sink.

### VRAM budget (for a GPU-resident port)
The decode window is design-bounded at 256 frames (~0.76 GiB at 1080p NV12), **fixed regardless of
feed count** (8 feeds → 32 frames each; `capFrames` clamps to `max(12, 256/trackCount)`). On Apple
Silicon (unified memory) the "VRAM constraint" is mild; it is real on discrete-GPU Windows.

The multiview peak is **not** a flat number. The decode window is an **aggregate** cap of ≤256
surfaces (per-feed cap = `max(12, 256/trackCount)`,
[playbackworker.cpp:169](../../../playback/playbackworker.cpp)) — it is **not** `256 × N-feeds`. The
mandated accounting is:

```
peak ≈ aggregate decode window (≤256 surfaces)
     + armed-cut staging bank (second decoder)
     + Σ per-bus compositor output surfaces (feed / PGM / multiview — one per active bus)
     + Σ async-readback rings (ring-depth × bus × requested CPU format)
```

An earlier "~1.8 GiB at 8×1080p" estimate counted only the decode window + normalized per-feed
output caches; it omits the staging bank, the per-bus compositor outputs, and the readback rings.
**Per D9, there is no separate normalized per-feed output cache term** — `OutputFrameCache` holds
refs (refcount bumps) aliasing the `TrackBuffer` surfaces, not copies; the only fresh compositor
surfaces are the per-bus outputs. That collapse is what drops the efficient port toward the ~0.85 GiB
floor. The GPU budget (§6 `gpu-budget`) sizes against this full formula.

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
   │  GPU compositor: YUV→RGB (BT.601/709), bilinear/Lanczos scale,            │
   │  grid, PGM select                                                          │
   │  ── extension points (Phase 5+, not built here): overlays/DVE, interpolation
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

- **`FrameMetadata`** splits **payload identity** from **presentation metadata** — this separation
  is load-bearing, not cosmetic:
  - **`FramePayloadKey`** (identity / dedup): `feedIndex`, `ptsMs`, content `videoHash`, `format`,
    geometry, `isPlaceholder` — what makes two frames *the same picture*.
  - **Presentation metadata** (per-handle, **NOT** identity): `outputFrameIndex`, `sampledPlayheadMs`.
  - **`ColorMetadata`** (`matrix` 601/709/2020, `range` video/full, `primaries`, `transfer`,
    `chromaFormat`, `bitDepth` — `bitDepth` fixed at 8, see §1 non-goals).

  Duplicate-skip already keys on payload and **deliberately ignores** `outputFrameIndex` /
  `sampledPlayheadMs` (`OutputFrameIdentity::samePayloadAs`,
  [outputbusengine.h:20-25](../../../playback/output/outputbusengine.h)), whereas `operator==`
  includes them. If the handle's identity used the full metadata (incl. `outputFrameIndex`),
  identity-skip would **regress**. Dedup/memo are already content/metadata-keyed (`videoHashFor`
  hashes metadata, never pixels) so they survive refcounted handles as refcount bumps with no
  readback — provided identity is the `FramePayloadKey`, not the presentation fields.
- **`FrameHandle`** — value wrapper over `shared_ptr<const IFrameData>` (pixels) **plus a per-handle
  `FrameMetadata`**. **Copy = refcount bump** of the immutable pixel data; metadata is a cheap
  per-handle value. This split is load-bearing: today `OutputBusEngine` **mutates** `ptsMs` /
  `outputFrameIndex` on a COW-shared `MediaVideoFrame` after sharing it
  ([outputbusengine.cpp](../../../playback/output/outputbusengine.cpp)). An immutable handle cannot
  allow that, so metadata must be a cheap per-handle override **over shared immutable pixels** — set
  scalars without touching (or copying) the pixel payload.
- **`IFrameData`** — `isGpuBacked()`/`isCpuBacked()`; `readToCpu(targetFormat) → CpuPlanes` (no-op
  for a CPU-origin frame already in `targetFormat`, fenced GPU→CPU download + convert otherwise — the
  single chokepoint every CPU sink funnels through); `gpuSurface() → GpuSurface*` (null on CPU
  frames).

**Pixel-format model (owned by `format-canon`).** `format` is an explicit enum with a known
plane-count per value, covering at least `Nv12` (2-plane biplanar — the GPU-resident default per D2),
`Yuv420p` (3-plane planar — today's CPU format), and `Rgba8` (compositor working/output). Today's
`MediaVideoFrame::isValid()` hard-codes 3-plane `Yuv420p`
([mediaframe.h:24](../../../playback/output/mediaframe.h)); the handle carries plane-count by format
instead. **`readToCpu()` yields the format the requesting sink needs** — I420 for NDI (`packI420`,
[ndisink.cpp:132](../../../playback/output/ndisink.cpp)), a `QVideoFrame`-compatible layout for Qt
preview ([qtpreviewsink.cpp:20](../../../playback/output/qtpreviewsink.cpp)) — doing the
NV12→I420 (or RGB→I420) conversion at the readback edge.

**Surface lifetime (handle contract, not just `gpu-sync`).** A GPU-backed handle **retains** its
native surface (`CVPixelBufferRef`/IOSurface on Apple, `ComPtr<ID3D11Texture2D>` on Windows) until
**all** GPU-render and readback fences referencing it have retired. Lifetime is the handle's
responsibility; eviction/backpressure (`gpu-sync`, `gpu-budget`) must not free a surface with a fence
in flight. **Hold-last / placeholder paths stay refcount-only:** the dispatcher's `m_lastGoodFrame`
re-emit and the placeholder path store and re-paint **handles**, never triggering `readToCpu()` — the
copy-on-GPU-path detector (`telemetry-contract`) covers those paths too.

**Copy/equality contract (explicit):** a handle copy is a refcount bump of shared pixels; identity
is by `FrameMetadata`, never pixel content. Concrete unit test: take a memo-aliased handle, override
`ptsMs`/`outputFrameIndex` on the copy, and assert (a) the pixel payload is byte-identical and
refcount-shared with the original and (b) the original's metadata is unchanged.

**Zero-regression gate (precise meaning):** Phase 1 ships **CPU-backed only**. `readToCpu()` returns
the existing `QByteArray` planes; all ~30+ access sites — including `OutputBusFrame`-by-value,
`MultiviewComposite.video`, and `TrackBuffer::Frame.frame` — funnel through the handle API.
**Byte-for-byte green applies to assertion *values* and golden *outputs*, not test *source*.** ~9–10
unit-test files directly index `.planeY/.planeU/.planeV` (e.g.
[tst_outputbusengine.cpp](../../../tests/unit/tst_outputbusengine.cpp)) and **will be mechanically
rewritten** through `readToCpu()`. To bound that churn, provide a thin `MediaVideoFrame`-compatible
value view backed by `readToCpu()` so plane-indexing test code migrates with minimal edits. The
product behavior and golden values are unchanged; only test sources edit.

---

## 5. Design-decision register

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Portable shader spine | **Qt RHI**, native only at import/export edges | All 3 platforms first-class; 3× native multiplies the test matrix; RHI gives WARP determinism on Windows CI. IOSurface/D3D-texture interop is native regardless — isolate at edges. Budget <0.5 ms/frame RHI overhead, **measured in Phase 0**. **Accepted costs:** QRhi has limited cross-Qt-version compatibility guarantees → Qt-minor-version coupling; a `Qt6::ShaderTools` build dependency; and likely native/private handles (`QRhiTexture::nativeTexture`, platform IOSurface/D3D interop) at the edges. |
| D2 | On-GPU canonical layout | **Keep NV12 (input/storage), deinterleave chroma in shader; composite in `Rgba8`** (8-bit only) | NV12 input = zero import conversion, matches decoder output, lowest VRAM. **Composite output is a separate choice:** the compositor samples NV12→RGB and renders/scales in `Rgba8` (working format); the export edge converts to each sink's CPU format (I420 for NDI, NV12 for encode). Golden reconstruction must model NV12 chroma rounding *on top of* nearest-neighbor (see §9). NV12 blocks a 10-bit/P010 path until a future HDR program adds one. `format-canon` owns the canonical set. |
| D3 | Golden strategy | **CPU compositor = permanent reference + fallback**; GPU exact on WARP, ±1 LSB on local-GPU lane | macOS has no deterministic headless Metal, so a CPU reference is needed regardless; it doubles as runtime fallback (RHI-unavailable / GPU-OOM). Integer-reconstruct shader math for WARP-exact; reserve ε for vendor-rounding on the local GPU lane. |
| D4 | GPU threading | **Fenced multi-thread mirroring the snapshot model** + GPU generation counter | Worker produces surfaces, signals a decode-done fence before publish; output thread waits before reading; eviction waits on render fences. Minimal extension of the proven lock-light producer/consumer; directly closes the TSan-invisible races. Single-GPU-thread would serialize decode+dispatch. **Sync primitive must match the RHI backend (Phase-0 decision):** Apple = `MTLSharedEvent`; Windows = `ID3D12Fence` *iff* the RHI D3D12 backend is chosen, else `ID3D11Fence` (11.4) / keyed-mutex / `ID3D11Query` for D3D11 — do not assume D3D12 fences on a D3D11 device. |
| D5 | Keep CPU pipeline as runtime fallback | **Yes, permanent**, behind the compositor-select flag, default until GPU proven per-platform | It is the CI oracle, the headless-macOS path, and the safe degradation target. Handle abstraction contains divergence to the compositor + sink-readback layers. |
| D6 | Mixed-origin frames | **Single self-describing handle**; `readToCpu()` no-op (CPU) / fenced download (GPU) | MPEG-2 SW + NDI ingest are CPU-origin; HW decode GPU-origin. Forcing uniform residency wastes uploads or defeats zero-copy. Compositor uploads a CPU-origin input on demand only when it composites it. |
| D7 | Readback placement | **Async pipelined** (render N, read N-2), **one readback per unique rendered bus surface / requested CPU format**, shared by all CPU sinks on that bus; **PGM ring depth-1 (sub-frame) from day one, other CPU sinks depth-3** | Sync readback stalls the 1 ms cadence at 60 fps. A tick renders per distinct bus (feed/PGM/multiview), so the invariant is per-surface, not per-tick. PGM preview gets the low-latency depth-1 path up front (user decision); NDI/multiview accept depth-3 (2–3 frames). The multiview monitor's ~33 ms (@60 fps) A/V lead vs the real-time `AudioPlayer` is **written-accepted** (§9), not mitigated. GPU-native sinks bypass via `SinkGpuCapability`. |
| D8 | iOS timing | **Symmetric edge interface now, bring-up Phase 5** | No macOS-only architecting, but don't pay iOS main-thread/thermal/VRAM cost before the spine is proven. |
| D9 | Single decode-window authority | **`TrackBuffer` owns the authoritative GPU surface window; `OutputFrameCache`, staging, and the inactive-graph snapshot hold `FrameHandle` refs (refcount bumps), never second copies** | Code-confirmed double storage: each decoded frame is inserted into both `track->buffer` **and** `m_outputCache` ([playbackworker.cpp:625-627](../../../playback/playbackworker.cpp)), and the inactive-graph path copies every TrackBuffer frame into a fresh cache (:2247-2256). Holding GPU surfaces in both doubles VRAM; refs-only collapses it — this is what makes the ~0.85 GiB floor real. |
| D10 | Sink cadence contract | **Per-sink capability: `GpuNative` / `AsyncReadbackDedupOk` / `NeedsContinuousCadence`** | The dispatcher identity-skips `submit()` on `samePayloadAs` ([outputdispatcher.cpp:116-122](../../../playback/output/outputdispatcher.cpp)). Async readback must not fight that or NDI's `maxGap≤2`: dedup-ok sinks (preview) skip readback on unchanged payload while advancing delivery indices; continuous-cadence sinks (NDI) get a readback (or a re-sent prior surface) every tick. `async-readback` owns the policy. |
| D11 | RHI render thread | **One `QRhi`, one dedicated render thread, `beginOffscreenFrame`/`endOffscreenFrame`; explicit handoff from worker/output threads** | RHI is not just fences: a `QRhi` is single-threaded, textures have thread affinity, and cross-thread use is constrained. The fenced producer/consumer (D4) rides on top of a single-render-thread RHI model. The Phase-0 overhead probe must exercise cross-thread import→composite→readback, not a clear pass. |

---

## 6. Subproject decomposition

15 subprojects with IDs, dependencies, and sizes. Incorporates the review's additions (format
canonicalization, shader toolchain, the **split-out Windows import edge**, AV-sync-under-readback,
device-loss, telemetry contract, recorder-encode rescope, iOS).

| id | name | depends on | risk | size |
|----|------|-----------|------|------|
| `frame-handle` | Opaque ref-counted FrameHandle + FrameMetadata (per-handle override over shared immutable pixels) + copy/equality contract + compat value-view; CPU-only, zero-regression; `OutputFrameCache`/staging/snapshot hold handle **refs**, not copies (D9) | — | high | XL |
| `color-metadata` | `ColorMetadata` plumbed decode→sink; replace height>576 heuristic; **proven no-op on goldens first** (Phase-0 default-tagging must reproduce today's height>576 → BT709/601 + video-range output until a fixture is re-goldened; M size is contingent on the audit finding uniform tags) | `frame-handle` | medium | M |
| `telemetry-contract` | Extend harness counter contract: VRAM occupancy, readback queue depth/drops, fence-wait stalls, GPU-OOM-degrade, **copy-on-GPU-path detector** (count GPU-backed `readToCpu()` calls; assert one per **unique rendered bus surface**, not per output frame; covers the hold-last/placeholder paths too) | `frame-handle` | medium | M |
| `format-canon` | Owns the canonical `format` enum + plane-count model (`Nv12`/`Yuv420p`/`Rgba8`), the on-GPU chroma layout (D2: NV12 kept, shader deinterleave), the **compositor working/output format (`Rgba8`)**, and the **per-sink export conversion**; **delivers the NV12-deinterleave + nearest-neighbor integer reference reconstructor (the golden oracle) BEFORE `gpu-compositor` lands**; every shader/golden/encoder agrees | `frame-handle` | medium | M |
| `shader-toolchain` | `qt_add_shaders`/`qsb` in CMake, shader source layout, Metal/HLSL/SPIR-V cross-compile, CI shader-compile step | `format-canon` | medium | M |
| `gpu-abstraction` | **macOS** `GpuSurface` (IOSurface/CVPixelBuffer) + RHI device/render-thread bring-up (D11: one `QRhi`, dedicated render thread) + VT keep-surface import + lazy `readToCpu`; the cross-platform `GpuSurface`/import edge interface stays **platform-neutral** (no `CVPixelBuffer`/`ID3D11Texture2D` types in the shared header) | `frame-handle`, `format-canon` | high | L |
| `gpu-import-win` | **Windows** MF→`ID3D11Texture2D` import-and-keep edge + RHI D3D interop + minimal import+readback slice; own Phase-0 probe (does MF yield a GPU-resident, RHI-importable texture, and at what reconfig cost — existing D3D11/`IMFDXGIDeviceManager` plumbing is HW-decode-to-CPU only, **not** the import edge) | `frame-handle`, `format-canon`, `gpu-abstraction` | high | L |
| `gpu-sync` | Backend-matched fences (MTLSharedEvent on Apple; ID3D12Fence/ID3D11Fence per the chosen D3D backend) + GPU generation counter + eviction guard honoring the handle's surface-lifetime contract (§4); **enforced lock rule** (collect victims, release `m_bufferMutex`, *then* fence-wait/free — never fence-wait under the mutex; `makeOutputSnapshot` already enforces `m_outputRuntimeMutex → m_bufferMutex`); armed-cut staging swap fence | `gpu-abstraction` | high | L |
| `gpu-compositor` | RHI grid + PGM select + YUV→RGB + bilinear/Lanczos; CPU compositor retained as reference; GPU texture memo. (Overlays/interpolation are extension points, not built here.) | `gpu-abstraction`, `color-metadata`, `shader-toolchain` | high | L |
| `async-readback` | `AsyncGpuReadbackSink` (**PGM depth-1 ring, other CPU sinks depth-3**) + `SinkGpuCapability` cadence routing (D10: `GpuNative`/`AsyncReadbackDedupOk`/`NeedsContinuousCadence`); **resolves identity-skip × NDI `maxGap≤2`**; **AV-sync-under-lag as hard gate** (audio travels with the delayed video; OutputBusFrame stays atomic); NDI migration | `gpu-sync`, `telemetry-contract` | high | L |
| `gpu-budget` | VRAM-bounded GPU window — single `TrackBuffer` authority (D9), `OutputFrameCache` refs-only — sized against the peak formula (§2) + OOM-safe degrade to CPU handle + seek-prefetch | `gpu-sync` | medium | M |
| `device-loss` | RHI device-removed detection, surface invalidation, spine rebuild, degrade-to-CPU policy + fault-injection e2e | `gpu-sync` | medium | M |
| `gpu-encode` | GPU-native encode **on the recorder `StreamWorker`** (separate jitter-pull pipeline), not a playback sink; color VUI authored from `ColorMetadata` | `gpu-sync`, `color-metadata` | medium | M |
| `ios-bringup` | iOS Metal bring-up + main-thread-render/thermal handling + smoke tests | `gpu-compositor`, `async-readback` | medium | M |
| `new-io-targets` | DeckLink SDI/HDMI + ST2110 as **GPU-texture-capable hardware** sinks (where the SDK allows); **OMT + AJA as NDI-style async-readback** (CPU-frame software SDKs) | `async-readback`, `gpu-budget` | high | L |

---

## 7. Phasing & sequencing

Keystone-first, strict gates. Each phase gates the next; everything behind flags; CPU default.

- **Phase 0 — Gating probes** (mostly measurement; the one code change — requesting IOSurface
  backing on the VT decode session — is the first piece of the real import edge, not a throwaway):
  1. **macOS:** confirm VideoToolbox can produce an **IOSurface-backed, RHI-importable** surface and
     at what reconfig cost (today the decode session does **not** request
     `kCVPixelBufferIOSurfacePropertiesKey`).
  2. **Windows (symmetric):** confirm MF→`ID3D11Texture2D` yields a GPU-resident, RHI-importable
     texture and at what reconfig cost (existing D3D11 plumbing is HW-decode-to-CPU only).
  3. Measure RHI per-frame overhead vs the <0.5 ms budget on a **realistic cross-thread import →
     composite → readback** path (not a trivial clear pass) — a `QRhi` is single-threaded with
     texture thread-affinity (D11), so the threading model is part of the probe. RHI not yet linked.
  4. Probe NDI / DeckLink / AJA / OMT GPU-texture vs CPU-frame capability per platform.
  5. Confirm RHI↔IOSurface / RHI↔D3D-texture interop (`copyTexture` or native detour).
  6. Audit whether existing golden fixtures carry SPS-VUI / CMFormatDescription color tags; if
     untagged, define a default-tagging policy so `color-metadata` is a provable no-op.
- **Phase 1 — Keystone (CPU-only, byte-green gate):** `frame-handle`, `color-metadata` (no-op
  proven), `telemetry-contract`.
- **Phase 2 — GPU edge + permanent vertical slice:** `format-canon` (incl. the NV12 reference
  reconstructor, before any compositor work), `shader-toolchain`, `gpu-abstraction`, `gpu-import-win`
  (Windows import+readback slice), and the macOS slice (§8) — **including the early micro-stress
  probe** (§8) and a **minimal `gpu-sync` fence stub** (decode-done fence before publish) so the
  micro-stress exercises eviction *with* the sync primitive that ships, not without it.
- **Phase 3 — Sync + compositor:** `gpu-sync`, `gpu-compositor`, **multi-feed cap-pressure stress**.
  (Correction: the single-feed slice **does** exercise CPU-frame eviction today — the window-derived
  cap is ~47–111 frames/feed, not 256. What is *unexercised* until here is **concurrent GPU
  evict-while-render under pressure** and **OOM-degrade** — see §8's micro-stress for the early
  signal.)
- **Phase 4 — Output at scale:** `async-readback` (AV-sync-under-lag hard gate + PGM sub-frame
  mode), `gpu-budget`, `device-loss`.
- **Phase 5 — Capabilities:** `gpu-encode`, `ios-bringup`, `new-io-targets`.

---

## 8. The permanent vertical slice (Phase 2)

macOS, one feed, behind `OLR_GPU_PIPELINE` (default off): VideoToolbox decodes H.264 keeping the
IOSurface-backed `CVPixelBuffer`, wraps it in a GPU-backed `FrameHandle`, flows through the existing
single-source render path, and reaches Qt preview via one lazy readback. No compositor changes, no
multiview, no NDI. **Ships and stays** (CPU path remains default + reference). A **Windows sibling
slice** (`gpu-import-win`) proves MF→D3D11 import+readback symmetrically.

**Note:** the real preview consumer is `FrameProvider` (holds `m_lastFrame`, serves
`latestImage()→toImage()` for screenshots, broadcasts `QVideoFrame` cross-thread to `QVideoSink` on
the GUI thread). The slice must address the screenshot CPU-`QImage` path and GUI-thread affinity, not
just `QtPreviewSink`.

**Early micro-stress probe (in this slice, not deferred to Phase 3):** with a forced tiny per-track
frame budget and injected GPU-alloc failure, exercise **concurrent evict-while-render** and
**OOM-degrade-to-CPU** on the single feed — giving the program's two highest-rated risks a signal
before `gpu-sync`/`gpu-compositor` land. The slice pulls in a **minimal decode-done fence** (the
first piece of `gpu-sync`) so eviction-under-render is tested against the real sync primitive, not a
fenceless approximation.

**Success criteria:**
- `OLR_GPU_PIPELINE=0`: the existing unit + `e2e_play` suite passes with **identical assertion values
  and golden outputs** (handle migration is behavior-preserving). Test *sources* that index
  `.planeY/.U/.V` are mechanically migrated via the compat value-view; this is expected churn, not a
  regression. Playback counters are emitted by `play_harness` and the thresholds asserted by
  `tests/e2e/run_playback_e2e.sh`.
- `OLR_GPU_PIPELINE=1` on a GPU-capable macOS host: single-feed `play1x` shows correct picture,
  `placeholderFramesDelta==0`, `heldFramesDelta==0` (no gray flash / stalls from readback).
- No full-frame GPU→CPU download except the single preview readback — asserted by the
  copy-on-GPU-path detector: one GPU-backed `readToCpu()` per **unique rendered bus surface** (a tick
  may render feed + PGM + multiview buses, each its own surface; CPU sinks on the same bus share one
  readback). The single-feed slice renders one bus, so the count is 1.
- GPU-surface readback matches CPU-path decode of the same frame within **±1 LSB/channel**.
- Micro-stress: concurrent evict-while-render does not read a freed surface; injected GPU-OOM
  degrades to the CPU handle without crashing the decode loop.
- No new ASan/UBSan findings; seek-under-decode stress does not crash.

---

## 9. Correctness & test strategy

- **CPU compositor as permanent oracle — but only for a GPU nearest-neighbor compatibility mode.**
  A `Rgba8`-out GPU shader that integer-reconstructs today's CPU nearest-neighbor math is validated
  against the CPU oracle: **exact on Windows WARP**, **±1 LSB on a local-GPU lane**. This proves the
  plumbing + color path are correct. **The quality scaler (bilinear/Lanczos) is by definition NOT
  byte/LSB-comparable** to the nearest-neighbor CPU compositor; it is validated **separately** with
  its own goldens + perceptual tolerance (PSNR/SSIM), never against the oracle. The compat shader is
  retained permanently as the oracle-test path.
- **NV12 double-rounding (D2):** the determinism harness must reconstruct goldens from NV12 chroma
  (interleaved UV, shader deinterleave) *plus* nearest-neighbor scaling — two rounding sources, not
  one. `format-canon` **delivers this integer reference reconstructor before `gpu-compositor`**, so
  the compositor is debugged against a fixed oracle, not a moving one.
- **Color is a deliberate two-step golden change, not a regression.** Phase-1 `color-metadata` is a
  **no-op** (default-tagging reproduces today's `height>576` heuristic → goldens unchanged). When
  real tags are honored (Phase-3 GPU compositor), appearance **intentionally** changes (correct
  BT.601/709 + range instead of the height guess) → tagged fixtures are **re-goldened on purpose**.
  The two events are tracked separately so the Phase-3 shift is never read as a regression.
- **CI reality (resolves open Q7):** headless hosted runners have no GPU/D3D
  ([ci.yml:462](../../../.github/workflows/ci.yml)). GPU-compositor correctness coverage is therefore
  **Windows WARP + an optional local-GPU lane**; **macOS CI stays CPU-oracle-only** (no automated
  GPU-compositor coverage on macOS — documented and accepted).
- **GPU race stress as first-class gates** (TSan cannot see GPU command ordering): seek-under-decode,
  armed-cut-while-render-pending, evict-while-render, frame-checksum validators. An early
  single-feed micro-stress runs in the Phase-2 slice (§8); the full multi-feed cap-pressure scenario
  lands in Phase 3.
- **AV-sync under readback lag** (hard gate inside `async-readback`, before NDI migrates).
  **Direction:** audio travels with the delayed video so `OutputBusFrame` stays atomic — the
  video+audio pair is read back / dispatched together, preserving sample alignment under N-frame
  lag. The **AV-sync MAX gate (≤100 ms)** and **NDI marker-continuity (`maxGap≤2`)** must pass
  through the readback path; these gates live in `tests/e2e/run_sync_e2e.sh` and the NDI
  recv-probe/analysis + NDI e2e shell drivers (not `play_harness`). `OutputBusFrame` carrying
  video+audio together is what makes the atomic-pair approach possible — and is the single most
  likely regression if violated.
- **Local-monitor lip-sync lead:** the worker-side `AudioPlayer` (real-time `QAudioSink` on the
  device clock) keeps playing in real time while preview video rides D7's render-N/read-N-2 path,
  creating a monitor-side audio **lead of ~2 frames (~33 ms @ 60 fps)** that the OutputBusFrame/NDI
  AV-sync gate structurally cannot observe. PGM gets D7's sub-frame mitigation; **multiview previews
  do not.** Resolution: either delay `AudioPlayer` to match preview readback, or document and accept
  the bounded monitor lead — decided in `async-readback`.
- **Telemetry counter contract** (`telemetry-contract`): existing gate counters
  (`placeholderFramesDelta`, `heldFramesDelta`, `maxClockDivergenceMs`, `decodedVideoFrames`,
  `stagingVideoFramesDecoded`) keep their meaning; new counters are emitted by the harnesses and the
  thresholds asserted by the e2e shell drivers.
- **Concurrency review** (per CLAUDE.md): the playback worker's threading + every `gpu-sync` change
  gets an independent review before merge.

---

## 10. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Keystone churn corrupts pipeline (30+ sites + by-value `OutputBusFrame`/`MultiviewComposite`/`TrackBuffer::Frame`) | Land CPU-only first; behavior-preserving gate (values + goldens); compat value-view bounds test churn; explicit copy/equality + aliasing unit test |
| TSan-invisible GPU races | Fences + GPU generation counter + checksum stress gates; early single-feed micro-stress in the Phase-2 slice |
| **Lock × fence deadlock** | Enforced `gpu-sync` rule: eviction collects victims, **releases `m_bufferMutex`, then** waits on render fences / frees — never fence-wait under the mutex (today eviction is a plain `m_frames.remove()` with no fence; `makeOutputSnapshot` already enforces `m_outputRuntimeMutex → m_bufferMutex`). Add TSan + lock-order annotation checks |
| Pixel-exact golden loss / NV12 double rounding | CPU reference oracle; integer-reconstruct for WARP-exact; ε only on local-GPU lane; harness models NV12 chroma rounding |
| **Quality scaler not oracle-validatable** | GPU nearest-neighbor compat shader covers the bit/LSB oracle; the bilinear/Lanczos scaler gets its own PSNR/SSIM goldens + tolerance, validated separately |
| **Identity-skip regression** | Handle identity is `FramePayloadKey` (excludes `outputFrameIndex`/`sampledPlayheadMs`), matching `samePayloadAs`; unit test pins that presentation-only changes do not break dedup |
| Readback stall on 1 ms cadence | Async pipelined, one copy per unique rendered bus surface shared by that bus's CPU sinks; identity-skip dedup; PGM sub-frame mode profiled |
| **VRAM blowup under multiview** | GPU budget sized to the peak formula (N-feeds × window + staging-bank + compositor-output-cache), not a flat 32–64; OOM-safe degrade to CPU; never let GPU alloc failure crash decode |
| **Two top risks retire late** | Early micro-stress (tiny per-track budget + GPU-alloc-failure injection) in the Phase-2 single-feed slice surfaces evict-while-render + OOM-degrade before Phase 3 |
| **Hidden second native edge (Windows)** | `gpu-import-win` split out with its own Phase-0 probe, slice, and risk/size — symmetric to the macOS VT edge |
| GPU device loss (hard-down for live) | `device-loss` subproject: detect, invalidate, rebuild spine, degrade to CPU; fault-injection e2e |
| Audio drift under readback lag / monitor lead | Atomic video+audio pair under lag; explicit AudioPlayer-delay-or-accept decision for the monitor lead |
| SDK GPU-capability unknowns | Phase 0 probes; OMT/AJA treated as CPU-frame async-readback sinks, not GPU-texture; forced-readback is `needs-readback`, not blocker |
| Color regression (zero metadata today) | `color-metadata` early; extract from CMFormatDescription/IMFMediaType/SPS-VUI; round-trip unit test; fixture color-tag audit + default-tagging policy |

---

## 11. Open questions (resolved in Phase 0)

1. Does the VT decode session yield an IOSurface-backed, RHI-importable buffer, and at what reconfig
   cost? (macOS edge)
2. Does MF→`ID3D11Texture2D` yield a GPU-resident, RHI-importable texture, and at what reconfig
   cost? (Windows edge, symmetric) — and **which RHI D3D backend (D3D11 vs D3D12)**, fixing the
   matching fence primitive (D4)?
3. Is RHI per-frame overhead within the <0.5 ms budget on representative hardware?
4. Which sinks (NDI/DeckLink/AJA/OMT/Qt preview) accept GPU textures vs require CPU frames, per
   platform?
5. Does RHI↔IOSurface / RHI↔D3D-texture interop work without a CPU detour?
6. Do existing golden fixtures carry color tags? If not, what is the default-tagging policy?
7. **Resolved (see §9):** headless hosted runners have no GPU/D3D
   ([ci.yml:462](../../../.github/workflows/ci.yml)); macOS CI stays CPU-oracle-only, and
   GPU-compositor coverage is Windows WARP + an optional local-GPU lane.

---

## 12. Subproject lifecycle & out-of-scope futures

Each subproject (§6) becomes its own `docs/superpowers/specs/` spec + implementation plan when its
phase is reached, following the existing per-feature spec convention in this directory. This
document is the program-level umbrella they hang from.

**Explicitly out of this program (future, architecture-enabled):** motion-interpolated slow-mo (needs
its own correctness strategy — the CPU oracle cannot validate interpolated frames), GPU
overlays/DVE/wipes (needs a graphics source + data model that does not exist today), and 10-bit/HDR
(P010/PQ/HLG — blocked by the D2 NV12 choice until a future path adds it). Each is a separate program
with its own spec; the GPU compositor and `FrameHandle` make them possible but do not deliver them.
