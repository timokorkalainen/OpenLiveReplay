# async-readback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Stop CPU sinks (NDI, Qt preview, screenshot) from stalling the ~1 ms output dispatch tick. P0.3 proved a synchronous import→composite→readback costs ~1.78 ms (readback alone ~1.18 ms), well over budget — so a GPU-backed `OutputBusFrame` cannot be read back inline on the tick. This subproject introduces an `AsyncGpuReadbackSink` that wraps any CPU `IOutputSink` with an **N-deep readback ring** (render frame N, deliver the frame read back ~N-2 ticks ago), driven by the `gpu-sync` `GpuFence` so a readback frame is only consumed once its GPU work has retired. Exactly **one readback per unique rendered bus surface / requested CPU format** is performed (the dispatcher renders one `OutputBusFrame` per distinct bus; all CPU sinks on that bus share that single readback). Cadence is routed per the D10 `SinkGpuCapability`: `GpuNative` (bypass — texture path), `AsyncReadbackDedupOk` (preview — skip readback on an unchanged payload via `samePayloadAs`, but still advance delivery indices), `NeedsContinuousCadence` (NDI `maxGap<=2` — a readback or a re-sent prior surface every tick). The PGM bus gets a **depth-1 (sub-frame, low-latency)** ring; all other CPU sinks get **depth-3**. The **AV-sync-under-lag hard gate** (§9) is the merge precondition: audio travels with the delayed video so the `OutputBusFrame` stays atomic, and both the AV-sync MAX gate (`<=100 ms` via `run_sync_e2e.sh`) and NDI marker-continuity (`maxGap<=2`) must pass through the readback path **before** NDI migrates behind the wrapper. The copy-on-GPU-path detector is extended to cover the ring (one GPU `readToCpu` per unique bus surface, including hold-last/placeholder re-emits). NDI and Qt preview are migrated behind the wrapper.

**Architecture:** A new `playback/output/asyncgpureadbacksink.{h,cpp}` mirrors the existing `QueuedOutputSink` wrapper idiom (`playback/output/queuedoutputsink.h`: own an inner `IOutputSink`, present an `IOutputSink` face, do the heavy work off the dispatch path) but replaces the blind queue with a **fenced readback ring**. A standalone `playback/output/gpureadbackring.{h,cpp}` owns the ring mechanics (push a GPU-backed `OutputBusFrame` + its `GpuFence` value at tick T; pop the CPU-resident frame from tick T-(depth-1) once its fence has `completedValue() >= value`), independent of `IOutputSink` so it is unit-testable headless with a stub fence. A `SinkGpuCapability` enum + a `gpuCapabilityFor(OutputTargetKind)` mapping (from the P0.4 table) drives the per-tick cadence decision. A per-bus **readback memo** (`SharedReadback`, keyed by `GpuReadbackSurfaceKey`) lets all CPU sinks on one bus share a single `readToCpu`; the dispatcher renders one `OutputBusFrame` per bus, so the memo is per-bus-per-tick. The `OutputDispatcher::dispatchTick` loop is extended to perform at most one shared readback per rendered bus surface and to route each endpoint through its capability. The CPU path stays default + the permanent oracle: with `OLR_GPU_PIPELINE` off (or the frame CPU-backed), the wrapper is a transparent pass-through to the inner sink — zero behavior change, byte-green goldens. `PlaybackWorker::rebuildOutputEndpoints` wraps the NDI and preview sinks in `AsyncGpuReadbackSink` only when the GPU pipeline is enabled.

**Tech Stack:** C++17, Qt 6 (Core/Gui/Test), CMake + Ninja. Consumes the merged Phase-0/1/2 contracts and the `gpu-sync` keystone (Phase 3). No new third-party deps. GPU behavioral tests run headless under `QT_QPA_PLATFORM=offscreen` and `QSKIP`/degrade where no GPU/RHI backend exists. The AV-sync + NDI continuity gates run through the existing e2e shell drivers (`tests/e2e/run_sync_e2e.sh`, `run_ndi_output_e2e.sh`, `run_ndi_playback_e2e.sh`, `run_ndi_e2e_pipe.sh`).

## Global Constraints

- **Builds ON merged Phase-0/1/2 + the `gpu-sync` keystone; never replaces them.** Consume these names verbatim (do not invent variants):
  - `FrameHandle` / `IFrameData` / `CpuPlanes` / `FrameMetadata` / `FramePayloadKey` / `FramePixelFormat` / `ColorMetadata` (`playback/output/framehandle.h`); `FrameHandle::readToCpu(FramePixelFormat)`, `FrameHandle::isGpuBacked()`, `FrameHandle::isStaleForGeneration(uint64_t)`, `FrameMetadata::gpuGeneration`.
  - `OutputBusFrame` / `OutputBusId` / `OutputFrameIdentity::samePayloadAs` (`playback/output/outputbusengine.h`); `IOutputSink` / `OutputSinkStatus` (`playback/output/outputsink.h`); `OutputTargetKind` (`playback/output/outputtypes.h`).
  - `OutputDispatcher::dispatchTick` + `OutputDispatchStats` fields `gpuReadbacks`, `redundantGpuReadbacks`, `readbackQueueDepth`, `readbackDrops`, `fenceWaitStalls` (`playback/output/outputdispatcher.h`).
  - `GpuReadbackTelemetry` (`recordGpuReadback(GpuReadbackSurfaceKey)`, `recordSurface(...)`, `snapshot()`), `GpuReadbackSurfaceKey { busKey, outputFrameIndex, format }` (`playback/output/gpureadbacktelemetry.h`).
  - **`gpu-sync` keystone** (Phase 3, `playback/gpu/`): `GpuFence` — `signal()->uint64_t`, `wait(uint64_t,int)->bool`, `completedValue()->uint64_t`, `static std::shared_ptr<GpuFence> create()`; `makeGpuFrameHandle(surface, rhi, meta, renderFence)` (the 4-arg overload); `GpuSurface::retainUntilFenceRetired(uint64_t)` / `pendingFenceValue()`; `GpuGenerationCounter::instance()`.
- **CPU path stays default + reference.** The GPU pipeline is two-gated: the `OLR_GPU_PIPELINE` CMake option → `OLR_GPU_PIPELINE_BUILD` compile def, and the runtime `gpuPipelineEnabled()` env flag (off by default; `playback/gpu/gpupipelineconfig.h`). With `OLR_GPU_PIPELINE_BUILD` undefined **or** `gpuPipelineEnabled()` false, every byte of this plan's behavior must be inert: `AsyncGpuReadbackSink::submit` is a direct pass-through to the inner sink, the dispatcher does no readback routing, and `rebuildOutputEndpoints` wraps nothing. The CPU path is byte-for-byte the Phase-1/2/3 path and remains the permanent correctness oracle + fallback.
- **A CPU-backed `OutputBusFrame` is never read back.** The ring engages only when `frame.video.isGpuBacked()`. A CPU-origin frame (SW decode / NDI ingest / the CPU compositor) is submitted to the inner sink unchanged — `readToCpu` on it is already a no-op, but the wrapper must not even ring-buffer it, so the CPU lane keeps its exact today-cadence and zero added latency.
- **One readback per unique rendered bus surface / requested CPU format.** A tick may render the feed, PGM, and multiview buses — three distinct surfaces, three readbacks. All CPU sinks consuming the *same* bus at the *same* tick share one readback (the shared `SharedReadback` memo). The copy-detector asserts `gpuReadbacks == uniqueSurfaces` (no redundant readback of a surface already read), covering hold-last and placeholder re-emit paths. This is the `telemetry-contract` invariant carried forward.
- **AV-sync is atomic and a hard merge gate.** Audio travels WITH the delayed video: the ring stores the whole `OutputBusFrame` (video handle + `MediaAudioFrame` + identity), so when the readback for tick T retires, the *same tick's* audio is delivered alongside the readback video — the A/V pair is never split across the lag. The AV-sync MAX gate (`<=100 ms`, `run_sync_e2e.sh lipsync` with `OLR_AV_SYNC_GATE=1`) and NDI marker-continuity (`maxGap<=2`, `run_ndi_output_e2e.sh` / `run_ndi_playback_e2e.sh`) MUST pass through the readback path **before** the NDI migration task lands. Splitting the A/V pair is the single most likely regression — Task 7 is the gate that catches it.
- **Concurrency-critical — independent review required before merge (CLAUDE.md "Verification").** The ring is touched by the dispatch (worker) thread that pushes and by the inner sink path that pops; the `gpu-sync` lock rule applies (never block on a `GpuFence` while holding a buffer/runtime mutex). Each task that touches the ring/fence threading carries a `**Review gate:**` note; the branch gets a fresh-agent concurrency review before the PR merges.
- **Zero-regression gate after every task.** With the flag off:
  ```sh
  cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
  ```
  must pass with identical assertion values, and `e2e_play` (`ctest -R e2e_play`) golden thresholds unchanged. GPU behavioral tests `QSKIP` where no GPU/RHI backend exists (`offscreen`/CI), never hard-fail.
- **Build (run from the worktree root):** configure once with the GPU pipeline ON (the readback ring only compiles its GPU branch under it):
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Also build once with `-DOLR_GPU_PIPELINE=OFF` (a fresh build dir) to confirm the off-path still compiles and the unit suite is byte-green. Unit tests register via `olr_add_unit_test(<name> <libs...>)` in `tests/unit/CMakeLists.txt` (GPU tests inside the `if(OLR_GPU_PIPELINE)` block); the GPU backend lib is `olr_test_gpu` and `olr_test_playback` carries the worker/dispatcher. Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.
- **Format changed lines only** (CI Lint checks changed lines; engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
- **Public-repo professionalism.** Self-contained, professional code/comments/commits; document the present design, no internal notes or private history.

---

## Preconditions (read before Task 1)

- **Phase 0/1/2 merged + the `gpu-sync` keystone merged** (`docs/superpowers/plans/2026-06-21-gpu-phase3-gpu-sync.md`). Specifically these must exist in the tree:
  - `playback/gpu/gpufence.h` with `class GpuFence { virtual uint64_t signal(); virtual bool wait(uint64_t,int); virtual uint64_t completedValue() const; static std::shared_ptr<GpuFence> create(); }` and the stub backend that runs deterministically headless.
  - The 4-arg `makeGpuFrameHandle(surface, rhi, meta, renderFence)` overload in `playback/gpu/gpuframedata.h`, and `GpuFrameData` stamping `m_surface->retainUntilFenceRetired(renderFence->signal())` on readback.
  - `GpuSurface::retainUntilFenceRetired(uint64_t)` / `pendingFenceValue()` virtuals on `playback/gpu/gpusurface.h` (added by `gpu-sync` Task 4).
  - `FrameHandle::isStaleForGeneration(uint64_t)` + `FrameMetadata::gpuGeneration` (added by `gpu-sync` Task 3).
  - The telemetry placeholders `readbackQueueDepth` / `readbackDrops` on `OutputDispatchStats` (`playback/output/outputdispatcher.h:76-78`) emitted by `play_harness.cpp` and parsed by `run_playback_e2e.sh` — **this subproject is the FIRST to write non-zero values into them.**
  Verify with `git merge-base --is-ancestor <gpu-sync-sha> origin/main` if unsure.
- **Today's dispatch renders one `OutputBusFrame` per distinct bus and shares it across endpoints** (`OutputDispatcher::dispatchTick`, `playback/output/outputdispatcher.cpp:81-143`): the `rendered` hash keys by `OutputBusId`; identity-skip via `tstats.lastIdentity.samePayloadAs(frame.identity)` already lives at :121-126. The readback routing slots into this exact structure — render once per bus, read back once per bus, route per endpoint capability.
- **`QueuedOutputSink` is the wrapper precedent** (`playback/output/queuedoutputsink.h`): own a `std::unique_ptr<IOutputSink> m_inner`, forward `kind()`/`start()`/`stop()`, present `OutputSinkStatus`. `AsyncGpuReadbackSink` mirrors this shape; the NDI sink is already wrapped (`QueuedOutputSink(std::make_unique<NdiOutputSink>())`, playbackworker.cpp:441), so the readback wrapper composes OUTSIDE the queue: `AsyncGpuReadbackSink(QueuedOutputSink(NdiOutputSink))`.
- **P0.4 sink classification (the D10 routing table):** Qt preview = `AsyncReadbackDedupOk`; NDI = `NeedsContinuousCadence` (`maxGap<=2`); DeckLink = `GpuNative` where the SDK allows else `NeedsContinuousCadence`; AJA/OMT = CPU-frame async-readback (`NeedsContinuousCadence`-like). Only NDI + Qt preview are implemented sinks today; the rest are enum entries this plan classifies but does not wire.

---

## Open decision carried by this subproject (document, then decide in Task 7)

**Multiview-monitor ~33 ms A/V lead (spec §9 "Local-monitor lip-sync lead").** The worker-side `AudioPlayer` (real-time `QAudioSink` on the device clock) keeps playing in real time while a preview's video rides the render-N/read-N-2 path, creating a monitor-side audio **lead of ~2 frames (~33 ms @ 60 fps)**. PGM gets the depth-1 (sub-frame) ring as its mitigation; **multiview previews do not** (depth-3). The `OutputBusFrame`/NDI AV-sync gate structurally cannot observe this monitor-only lead (it measures the *output* A/V pair, which stays atomic). Resolution options, decided in Task 7 with measurement in hand:
- **(a) Delay `AudioPlayer`** by the preview ring depth so the monitor audio rides the same lag as the preview video, or
- **(b) Document-and-accept** the bounded ~33 ms monitor lead (within EBU R37's +40/-60 ms band for a non-PGM monitor).
This plan implements the depth split + the atomic-pair guarantee; Task 7 records the measured monitor lead and the chosen option in the plan + a code comment. It does **not** silently mitigate.

---

## Task 1: `SinkGpuCapability` + `gpuCapabilityFor(OutputTargetKind)` — the D10 routing table

**Precondition:** Phase 0/1/2 + `gpu-sync` merged.

**Files:**
- Create: `playback/output/sinkgpucapability.h`, `playback/output/sinkgpucapability.cpp`
- Test: `tests/unit/tst_sinkgpucapability.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt` (add the two files to `olr_test_playback`), `CMakeLists.txt` (compile the `.cpp` into `OpenLiveReplay`)

**Interfaces:**
- Consumes: `OutputTargetKind` (`playback/output/outputtypes.h`).
- Produces:
  ```cpp
  // playback/output/sinkgpucapability.h
  // D10 sink cadence routing (P0.4 classification). The dispatcher uses this to
  // decide, per endpoint, whether to bypass readback (GpuNative), skip readback on
  // an unchanged payload (AsyncReadbackDedupOk), or guarantee a frame every tick
  // (NeedsContinuousCadence). The CPU path treats every sink as a pass-through; this
  // routing only takes effect when the rendered frame is GPU-backed.
  enum class SinkGpuCapability {
      GpuNative,              // texture-capable: bypass CPU readback entirely
      AsyncReadbackDedupOk,   // preview: skip readback on samePayloadAs, advance indices
      NeedsContinuousCadence, // NDI (maxGap<=2): a readback or a re-sent prior surface every tick
  };

  // Maps an implemented/enumerated sink kind to its cadence capability (P0.4).
  SinkGpuCapability gpuCapabilityFor(OutputTargetKind kind);
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_sinkgpucapability.cpp`:
```cpp
// The D10 routing table: each sink kind maps to its P0.4 cadence capability.
// Qt preview dedups; NDI needs continuous cadence; DeckLink is GPU-native where
// the SDK allows (this table marks it GpuNative as the default intent); AJA/OMT
// are CPU-frame async-readback sinks (continuous cadence).
#include <QtTest>

#include "playback/output/outputtypes.h"
#include "playback/output/sinkgpucapability.h"

class TestSinkGpuCapability : public QObject {
    Q_OBJECT
private slots:
    void previewDedups();
    void ndiNeedsContinuousCadence();
    void aja_omt_areContinuousCadence();
    void decklinkIsGpuNative();
};

void TestSinkGpuCapability::previewDedups() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::QtPreview),
             SinkGpuCapability::AsyncReadbackDedupOk);
}
void TestSinkGpuCapability::ndiNeedsContinuousCadence() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::Ndi),
             SinkGpuCapability::NeedsContinuousCadence);
}
void TestSinkGpuCapability::aja_omt_areContinuousCadence() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::Aja),
             SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::Omt),
             SinkGpuCapability::NeedsContinuousCadence);
}
void TestSinkGpuCapability::decklinkIsGpuNative() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::DeckLinkSdiHdmi),
             SinkGpuCapability::GpuNative);
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::DeckLinkIpSt2110),
             SinkGpuCapability::GpuNative);
}

QTEST_MAIN(TestSinkGpuCapability)
#include "tst_sinkgpucapability.moc"
```
Register in `tests/unit/CMakeLists.txt` (in the general/non-GPU block — this header has no GPU deps):
```cmake
    olr_add_unit_test(tst_sinkgpucapability olr_test_playback)
```

- [ ] **Step 2: Run the test, expect FAIL**
```sh
cmake --build build/c --target tst_sinkgpucapability
```
Expected: FAIL to compile — `playback/output/sinkgpucapability.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/output/sinkgpucapability.h` with the enum + declaration above. Create `playback/output/sinkgpucapability.cpp`:
```cpp
#include "playback/output/sinkgpucapability.h"

SinkGpuCapability gpuCapabilityFor(OutputTargetKind kind) {
    switch (kind) {
    case OutputTargetKind::QtPreview:
        return SinkGpuCapability::AsyncReadbackDedupOk;
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110:
        // GPU-native where the SDK exposes GPUDirect; new-io-targets falls back to
        // NeedsContinuousCadence at wire time where it does not.
        return SinkGpuCapability::GpuNative;
    case OutputTargetKind::Ndi:
    case OutputTargetKind::Omt:
    case OutputTargetKind::Aja:
        // CPU-frame software SDKs: a frame every tick (maxGap<=2).
        return SinkGpuCapability::NeedsContinuousCadence;
    }
    return SinkGpuCapability::NeedsContinuousCadence;
}
```
Add `playback/output/sinkgpucapability.cpp` to the `OpenLiveReplay` target in `CMakeLists.txt` and to `olr_test_playback` in `tests/CMakeLists.txt` (alongside the other `playback/output/*.cpp` sink files).

- [ ] **Step 4: Run the test, expect PASS**
```sh
cmake --build build/c --target tst_sinkgpucapability && ctest --test-dir build/c -R tst_sinkgpucapability --output-on-failure
```
Expected: PASS (4 tests).

- [ ] **Step 5: Zero-regression + commit**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/sinkgpucapability.h playback/output/sinkgpucapability.cpp \
        tests/unit/tst_sinkgpucapability.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(async-readback): SinkGpuCapability routing table (D10 / P0.4)"
```

---

## Task 2: `GpuReadbackRing` — fenced render-N / read-N-(depth-1) ring (the mechanics, sink-agnostic)

**Precondition:** Task 1.

**Files:**
- Create: `playback/output/gpureadbackring.h`, `playback/output/gpureadbackring.cpp`
- Test: `tests/unit/tst_gpureadbackring.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `OutputBusFrame` (`playback/output/outputbusengine.h`), `GpuFence` (`playback/gpu/gpufence.h`), `FrameHandle::isGpuBacked()`/`readToCpu()` (`playback/output/framehandle.h`).
- Produces:
  ```cpp
  // playback/output/gpureadbackring.h
  // Pipelines GPU->CPU readback so the producer never blocks on a download. push()
  // records a GPU-backed OutputBusFrame and the GpuFence value its render/readback
  // work targets at tick T; pop() returns the OutputBusFrame from tick T-(depth-1),
  // CPU-resident, ONCE its fence value has retired (completedValue() >= value). A
  // depth-1 ring is the sub-frame low-latency PGM path (push then immediately pop
  // the same frame, gated only by fence retirement). A depth-3 ring lags by 2.
  // The whole OutputBusFrame is stored (video + audio + identity) so the A/V pair
  // is delivered atomically under lag (spec §9). Single-producer/single-consumer on
  // the dispatch thread; no internal locking beyond the fence wait.
  struct RingReadyFrame {
      bool ready = false;          // false => not yet retired, or ring not yet filled
      OutputBusFrame frame;        // CPU-resident video (readToCpu done) + its audio
  };

  class GpuReadbackRing {
  public:
      // depth>=1. depth==1 is PGM sub-frame mode; depth==3 is the default CPU-sink lag.
      explicit GpuReadbackRing(int depth);

      int depth() const;
      int occupancy() const;       // entries currently held (telemetry: readbackQueueDepth)
      qint64 drops() const;        // entries overwritten before pop (telemetry: readbackDrops)

      // Record a GPU-backed frame + the fence value gating its readback. Returns the
      // CPU-resident frame that is now (depth-1) ticks old AND whose fence has retired,
      // or {ready=false} if the ring is not yet filled or that frame's fence is still
      // in flight. `format` is the CPU layout the sink needs (I420 for NDI, etc.).
      RingReadyFrame pushAndPop(const OutputBusFrame& gpuFrame, uint64_t fenceValue,
                                std::shared_ptr<GpuFence> fence, FramePixelFormat format);

      // Drain the oldest still-pending entry, blocking on its fence up to timeoutMs.
      // Used at stop()/flush. Returns {ready=false} when empty. timeoutMs<0 = forever.
      RingReadyFrame flushOne(int timeoutMs);
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpureadbackring.cpp` — drive the ring with the deterministic stub `GpuFence` (no GPU needed):
```cpp
// The readback ring pipelines GPU->CPU download so the producer never blocks.
// Depth-1 = PGM sub-frame: push a frame whose fence is already retired and pop it
// the same call. Depth-3 = lag by 2: the first two pushes yield nothing; the third
// yields tick-0's frame. A still-in-flight fence holds the frame back (not ready).
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/output/framehandle.h"
#include "playback/output/gpureadbackring.h"

namespace {
OutputBusFrame gpuFrameFor(qint64 idx) {
    OutputBusFrame f;
    f.outputFrameIndex = idx;
    // A solid CPU handle stands in for a GPU surface here; the ring only needs
    // readToCpu() to succeed. (GpuFrameData-backed variants run in tst_gpuframedata.)
    f.video = solidYuv420pHandle(16, 16, uchar(16 + idx), 128, 128);
    f.identity.outputFrameIndex = idx;
    return f;
}
} // namespace

class TestGpuReadbackRing : public QObject {
    Q_OBJECT
private slots:
    void depthOneIsSubFrameWhenFenceRetired();
    void depthThreeLagsByTwo();
    void pendingFenceHoldsFrameBack();
    void overwriteCountsAsDrop();
};

void TestGpuReadbackRing::depthOneIsSubFrameWhenFenceRetired() {
    GpuReadbackRing ring(1);
    auto fence = GpuFence::create();
    QVERIFY(fence != nullptr);
    const uint64_t v = fence->signal();           // already retired on the stub
    QVERIFY(fence->wait(v, 1000));
    RingReadyFrame r = ring.pushAndPop(gpuFrameFor(0), v, fence, FramePixelFormat::Yuv420p);
    QVERIFY(r.ready);
    QCOMPARE(r.frame.outputFrameIndex, qint64(0));
}

void TestGpuReadbackRing::depthThreeLagsByTwo() {
    GpuReadbackRing ring(3);
    auto fence = GpuFence::create();
    auto retired = [&] { const uint64_t v = fence->signal(); fence->wait(v, 1000); return v; };
    QVERIFY(!ring.pushAndPop(gpuFrameFor(0), retired(), fence, FramePixelFormat::Yuv420p).ready);
    QVERIFY(!ring.pushAndPop(gpuFrameFor(1), retired(), fence, FramePixelFormat::Yuv420p).ready);
    RingReadyFrame r = ring.pushAndPop(gpuFrameFor(2), retired(), fence, FramePixelFormat::Yuv420p);
    QVERIFY(r.ready);
    QCOMPARE(r.frame.outputFrameIndex, qint64(0));   // tick-2 delivers tick-0
}

void TestGpuReadbackRing::pendingFenceHoldsFrameBack() {
    GpuReadbackRing ring(1);
    auto fence = GpuFence::create();
    const uint64_t future = fence->completedValue() + 5;  // never signaled to 5
    RingReadyFrame r = ring.pushAndPop(gpuFrameFor(0), future, fence, FramePixelFormat::Yuv420p);
    QVERIFY(!r.ready);                                     // fence in flight -> not ready
}

void TestGpuReadbackRing::overwriteCountsAsDrop() {
    GpuReadbackRing ring(3);
    auto fence = GpuFence::create();
    const uint64_t future = fence->completedValue() + 100;  // all stay pending
    for (int i = 0; i < 5; ++i)
        ring.pushAndPop(gpuFrameFor(i), future, fence, FramePixelFormat::Yuv420p);
    QVERIFY(ring.drops() >= 1);   // pushes past depth overwrite un-popped pending entries
}

QTEST_MAIN(TestGpuReadbackRing)
#include "tst_gpureadbackring.moc"
```
Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:
```cmake
    olr_add_unit_test(tst_gpureadbackring olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**
```sh
cmake --build build/c --target tst_gpureadbackring
```
Expected: FAIL to compile — `playback/output/gpureadbackring.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/output/gpureadbackring.h` (interface above) and `playback/output/gpureadbackring.cpp`. Store a fixed-size deque of `{ OutputBusFrame gpuFrame; uint64_t fenceValue; std::shared_ptr<GpuFence> fence; FramePixelFormat format; bool readBack=false; OutputBusFrame cpuFrame; }`. `pushAndPop`:
1. Append the new pending entry; if appending would exceed `depth`, the entry being displaced (front) that has not yet been popped counts as a `drop` (increment `m_drops`).
2. Identify the entry that is now `(depth-1)` positions behind the head (the one to deliver this tick). If its `fence->completedValue() >= fenceValue`, perform the readback once: `cpuFrame = gpuFrame; cpuFrame.video = makeCpuFrameHandle(gpuFrame.video.readToCpu(format), gpuFrame.video.metadata());` (audio + identity copied verbatim — the atomic A/V pair), mark `readBack=true`, return `{ready=true, frame=cpuFrame}`. Else return `{ready=false}`.
3. Maintain `m_occupancy` = entries held but not yet delivered.

`flushOne(timeoutMs)` pops the oldest pending entry, blocks on its fence up to `timeoutMs` via `fence->wait(fenceValue, timeoutMs)`, reads it back, and returns it (`{ready=false}` if empty or the wait times out — bump no drop on a clean flush).

**LOCK RULE:** `pushAndPop`/`flushOne` only ever call `fence->wait`/`completedValue` — they must NOT be called while the caller holds `m_bufferMutex`/`m_outputRuntimeMutex`. The ring lives entirely on the dispatch thread; document this on the class.

Wire `gpureadbackring.{h,cpp}` into the `OpenLiveReplay` `if(OLR_GPU_PIPELINE)` block in `CMakeLists.txt`, `olr_test_playback`'s GPU block and `olr_test_gpu` in `tests/CMakeLists.txt` (mirroring `gpuframedata`).

- [ ] **Step 4: Run the test, expect PASS**
```sh
cmake --build build/c --target tst_gpureadbackring && ctest --test-dir build/c -R tst_gpureadbackring --output-on-failure
```
Expected: PASS (4 tests).

**Review gate:** the ring is the single-producer/single-consumer pipeline element. Confirm no fence wait happens under a worker mutex and that the displaced-entry drop accounting cannot drop a frame whose fence already retired (a retired-but-undelivered front entry should be delivered, not dropped). Flag for independent review.

- [ ] **Step 5: Zero-regression + commit**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/gpureadbackring.h playback/output/gpureadbackring.cpp \
        tests/unit/tst_gpureadbackring.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(async-readback): fenced GpuReadbackRing (render-N/read-N-(depth-1), atomic A/V)"
```

---

## Task 3: `AsyncGpuReadbackSink` — the IOutputSink wrapper (pass-through when CPU/flag-off)

**Precondition:** Task 2.

**Files:**
- Create: `playback/output/asyncgpureadbacksink.h`, `playback/output/asyncgpureadbacksink.cpp`
- Test: `tests/unit/tst_asyncgpureadbacksink.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `IOutputSink` / `OutputSinkStatus` (`playback/output/outputsink.h`), `GpuReadbackRing` (Task 2), `SinkGpuCapability` (Task 1), `gpuPipelineEnabled()` (`playback/gpu/gpupipelineconfig.h`), `FramePixelFormat`.
- Produces:
  ```cpp
  // playback/output/asyncgpureadbacksink.h
  // Wraps a CPU IOutputSink so a GPU-backed OutputBusFrame is read back through a
  // fenced ring instead of synchronously on the ~1 ms dispatch tick (P0.3). The
  // ring depth and the CPU format are chosen at construction (PGM=1 sub-frame,
  // others=3; format = the inner sink's required layout). With the GPU pipeline
  // off OR a CPU-backed frame, submit() is a transparent pass-through to the inner
  // sink (byte-for-byte today's behavior). Cadence under NeedsContinuousCadence:
  // when a tick's readback is not yet ready, re-submit the inner sink's PRIOR
  // delivered surface so maxGap<=2 holds; under AsyncReadbackDedupOk: skip the
  // submit on samePayloadAs while still advancing delivery indices.
  class AsyncGpuReadbackSink final : public IOutputSink {
  public:
      AsyncGpuReadbackSink(std::unique_ptr<IOutputSink> inner, int ringDepth,
                           FramePixelFormat cpuFormat, SinkGpuCapability capability);
      ~AsyncGpuReadbackSink() override;

      OutputTargetKind kind() const override;
      bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
      void stop() override;                       // flushes the ring through the inner sink
      bool isActive() const override;
      bool submit(const OutputBusFrame& frame) override;
      OutputSinkStatus outputStatus() const override;  // augments inner status with ring depth/drops

      int ringDepth() const;
      qint64 readbackDrops() const;
      qint64 readbackQueueDepth() const;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_asyncgpureadbacksink.cpp` — a recording fake inner sink captures what reaches the real sink:
```cpp
// The wrapper is a transparent pass-through for CPU frames / flag-off, and a
// fenced ring for GPU frames. We assert: (1) a CPU frame reaches the inner sink
// unchanged and immediately (no lag); (2) a GPU frame is delivered to the inner
// sink CPU-resident (isGpuBacked()==false at the inner sink) after the ring lag;
// (3) under NeedsContinuousCadence a not-yet-ready tick re-sends the prior surface
// so the inner sink sees a frame every tick (no gap).
#include <QtTest>

#include "playback/gpu/gpupipelineconfig.h"
#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/framehandle.h"
#include "playback/output/sinkgpucapability.h"

#include <vector>

namespace {
class RecordingSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }
    bool start(const OutputTargetAssignment&, FrameRate) override { m_active = true; return true; }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& f) override {
        delivered.push_back(f);
        gpuBackedAtSink.push_back(f.video.isGpuBacked());
        return true;
    }
    std::vector<OutputBusFrame> delivered;
    std::vector<bool> gpuBackedAtSink;
    bool m_active = false;
};

OutputBusFrame cpuFrame(qint64 idx) {
    OutputBusFrame f;
    f.outputFrameIndex = idx;
    f.video = solidYuv420pHandle(16, 16, uchar(16 + idx), 128, 128);  // CPU-backed
    f.identity.outputFrameIndex = idx;
    f.identity.sourcePtsMs = idx;     // distinct payloads (defeat dedup)
    return f;
}
} // namespace

class TestAsyncGpuReadbackSink : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { qunsetenv("OLR_GPU_PIPELINE"); }  // restore default
    void cpuFramePassesThroughImmediately();
    void flagOffIsTransparent();
};

void TestAsyncGpuReadbackSink::cpuFramePassesThroughImmediately() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* raw = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence);
    sink.start({}, FrameRate{});
    sink.submit(cpuFrame(0));
    // A CPU frame is never ring-buffered: it reaches the inner sink this tick.
    QCOMPARE(int(raw->delivered.size()), 1);
    QCOMPARE(raw->delivered[0].outputFrameIndex, qint64(0));
}

void TestAsyncGpuReadbackSink::flagOffIsTransparent() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* raw = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::AsyncReadbackDedupOk);
    sink.start({}, FrameRate{});
    sink.submit(cpuFrame(7));
    QCOMPARE(int(raw->delivered.size()), 1);
    QCOMPARE(raw->delivered[0].outputFrameIndex, qint64(7));  // unchanged, no lag
}

QTEST_MAIN(TestAsyncGpuReadbackSink)
#include "tst_asyncgpureadbacksink.moc"
```
> **Note:** `gpuPipelineEnabled()` (`playback/gpu/gpupipelineconfig.h`) reads the `OLR_GPU_PIPELINE` environment variable; the merged header exposes no boolean test setter. Flip the runtime flag in tests with the env-toggle pattern the sibling `gpu-compositor` and `gpu-budget` plans use — `qputenv("OLR_GPU_PIPELINE", "1")` to enable, `qunsetenv("OLR_GPU_PIPELINE")` to restore the default — and restore it in `cleanup()`. The actual gating still goes through `gpuPipelineEnabled()`; do not invent a setter.

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:
```cmake
    olr_add_unit_test(tst_asyncgpureadbacksink olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**
```sh
cmake --build build/c --target tst_asyncgpureadbacksink
```
Expected: FAIL to compile — `playback/output/asyncgpureadbacksink.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/output/asyncgpureadbacksink.{h,cpp}`. Members: `std::unique_ptr<IOutputSink> m_inner;`, `GpuReadbackRing m_ring;`, `FramePixelFormat m_cpuFormat;`, `SinkGpuCapability m_capability;`, `OutputBusFrame m_lastDelivered;` (for cadence re-send), `bool m_hasLastDelivered=false;`, `OutputFrameIdentity m_lastIdentity;`, `bool m_hasLastIdentity=false;`. `kind()`/`start()`/`isActive()` forward to `m_inner`. `submit(frame)`:
```cpp
    if (!gpuPipelineEnabled() || !frame.video.isGpuBacked()) {
        // CPU lane: never ring-buffer; today's exact behavior + zero added latency.
        return m_inner->submit(frame);
    }
    // GPU lane: read back through the fenced ring (one readback per surface).
    // The render fence was stamped when the surface was minted/composited; we read
    // the gating value from the surface's pendingFenceValue() (gpu-sync contract).
    uint64_t fenceValue = 0;
    if (GpuSurface* s = frame.video.data() ? frame.video.data()->gpuSurface() : nullptr)
        fenceValue = s->pendingFenceValue();
    RingReadyFrame r = m_ring.pushAndPop(frame, fenceValue, m_sharedRenderFence, m_cpuFormat);
    if (r.ready) {
        if (m_capability == SinkGpuCapability::AsyncReadbackDedupOk && m_hasLastIdentity &&
            m_lastIdentity.samePayloadAs(r.frame.identity)) {
            // Dedup: skip the submit but advance delivery bookkeeping.
            m_lastIdentity = r.frame.identity;
            return true;
        }
        const bool ok = m_inner->submit(r.frame);
        m_lastDelivered = r.frame; m_hasLastDelivered = true;
        m_lastIdentity = r.frame.identity; m_hasLastIdentity = true;
        return ok;
    }
    // Not yet ready this tick.
    if (m_capability == SinkGpuCapability::NeedsContinuousCadence && m_hasLastDelivered) {
        // Re-send the prior CPU-resident surface so maxGap<=2 holds (NDI).
        return m_inner->submit(m_lastDelivered);
    }
    return true;  // dedup/native: a gap here is acceptable; indices already advanced
```
`m_sharedRenderFence` is the process `GpuFence` shared with the worker's `m_renderFence`; pass it into the ctor (add a `std::shared_ptr<GpuFence>` ctor param defaulting to `GpuFence::create()` so unit tests can inject the stub). `stop()` drains the ring: `while (true) { auto r = m_ring.flushOne(2); if (!r.ready) break; m_inner->submit(r.frame); }` then `m_inner->stop();`. `outputStatus()` returns `m_inner->outputStatus()` with `currentQueueDepth = m_ring.occupancy()` and `droppedFrames += m_ring.drops()` folded in. Include `playback/gpu/gpusurface.h` and `playback/gpu/gpufence.h`.

Wire `asyncgpureadbacksink.{h,cpp}` into the `OpenLiveReplay` `if(OLR_GPU_PIPELINE)` block, `olr_test_playback`'s GPU block, and `olr_test_gpu`.

- [ ] **Step 4: Run the test, expect PASS**
```sh
cmake --build build/c --target tst_asyncgpureadbacksink && ctest --test-dir build/c -R tst_asyncgpureadbacksink --output-on-failure
```
Expected: PASS (2 tests; GPU-frame round-trip behavior is exercised in Task 4 with real `GpuFrameData`).

**Review gate:** confirm the CPU-lane pass-through is truly zero-overhead and the `stop()` drain cannot deadlock on a fence (bounded 2 ms `flushOne` timeout). Flag for independent review.

- [ ] **Step 5: Zero-regression + commit**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/asyncgpureadbacksink.h playback/output/asyncgpureadbacksink.cpp \
        tests/unit/tst_asyncgpureadbacksink.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(async-readback): AsyncGpuReadbackSink wrapper (pass-through CPU, fenced ring GPU)"
```

---

## Task 4: GPU-frame round-trip + shared readback (one readback per surface, atomic A/V)

**Precondition:** Tasks 2 + 3. Needs a `GpuFrameData`-backed handle, so the behavioral assertions `QSKIP` without a GPU/RHI host; the structural assertions (telemetry counts, A/V atomicity) run on the stub.

**Files:**
- Modify: `playback/output/asyncgpureadbacksink.cpp` (record the shared readback into `GpuReadbackTelemetry` exactly once per surface), `playback/output/gpureadbackring.cpp` (carry the bus surface key for the telemetry record)
- Test: `tests/unit/tst_asyncgpureadbacksink.cpp` (extend), `tests/unit/tst_gpureadbackring.cpp` (extend)

**Interfaces:**
- Consumes: `makeGpuFrameHandle(surface, rhi, meta, renderFence)` (`gpu-sync`), `GpuReadbackTelemetry::recordGpuReadback`/`recordSurface`/`snapshot` + `GpuReadbackSurfaceKey { busKey, outputFrameIndex, format }` (`playback/output/gpureadbacktelemetry.h`).
- Produces: no new public signature; closes the "one readback per unique rendered bus surface, shared by all that bus's CPU sinks" contract on the wrapper side and the atomic A/V guarantee.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_asyncgpureadbacksink.cpp`:
```cpp
    void audioTravelsWithDelayedVideo();
#ifdef __APPLE__
    void gpuFrameDeliveredCpuResidentAfterLag();
    void sharedReadbackRecordsOneSurface();
#endif
```
```cpp
void TestAsyncGpuReadbackSink::audioTravelsWithDelayedVideo() {
    // The ring must keep video and its OWN tick's audio together. Tag each tick's
    // audio with a unique first byte; after the lag, the delivered audio byte must
    // match the delivered video's tick (no A/V split under lag — spec §9).
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* raw = inner.get();
    AsyncGpuReadbackSink sink(std::move(inner), 3, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence,
                              /*fence for test*/ GpuFence::create());
    sink.start({}, FrameRate{});
    for (qint64 i = 0; i < 4; ++i) {
        OutputBusFrame f = cpuFrame(i);                 // CPU here exercises the structural path
        f.audio.pcm = QByteArray(4, char(i));           // tag this tick's audio
        sink.submit(f);
    }
    // CPU frames pass straight through, so audio[i] pairs with video tick i exactly.
    QVERIFY(!raw->delivered.empty());
    for (const OutputBusFrame& d : raw->delivered)
        QCOMPARE(d.audio.pcm.at(0), char(d.outputFrameIndex));
}

#ifdef __APPLE__
void TestAsyncGpuReadbackSink::gpuFrameDeliveredCpuResidentAfterLag() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    qputenv("OLR_GPU_PIPELINE", "1");
    auto inner = std::make_unique<RecordingSink>();
    RecordingSink* raw = inner.get();
    auto fence = GpuFence::create();
    AsyncGpuReadbackSink sink(std::move(inner), 1, FramePixelFormat::Yuv420p,
                              SinkGpuCapability::NeedsContinuousCadence, fence);
    sink.start({}, FrameRate{});
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta; meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64; meta.key.height = 48;
    OutputBusFrame f; f.outputFrameIndex = 0;
    f.video = makeGpuFrameHandle(surface, rhi, meta, fence);
    QVERIFY(f.video.isGpuBacked());
    sink.submit(f);                                    // depth-1: pushed + popped
    QVERIFY(!raw->delivered.empty());
    QVERIFY(!raw->delivered.back().video.isGpuBacked());  // CPU-resident at the inner sink
}

void TestAsyncGpuReadbackSink::sharedReadbackRecordsOneSurface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuReadbackTelemetry::instance().reset();
    // Two CPU sinks on the SAME bus surface must produce ONE readback, not two.
    // (Wrapper-level: feed both wrappers the same GPU frame at the same tick; the
    //  shared GpuReadbackSurfaceKey memo records the surface once.)
    // ... build one GPU OutputBusFrame, submit to two AsyncGpuReadbackSink wrappers
    //     that share the SharedReadback memo, assert snapshot().gpuReadbacks == 1.
    GpuReadbackTelemetrySnapshot s = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(s.gpuReadbacks, qint64(1));
    QCOMPARE(s.redundantReadbacks, qint64(0));
}
#endif
```

- [ ] **Step 2: Run the test, expect FAIL**
```sh
cmake --build build/c --target tst_asyncgpureadbacksink
```
Expected: FAIL — `audioTravelsWithDelayedVideo` fails if the ring splits A/V (or the 5-arg ctor with an injected fence does not exist yet); the Apple slots fail the shared-readback count (`gpuReadbacks != 1`) because the per-surface memo is not yet recording.

- [ ] **Step 3: Write the implementation**

Add the 5-arg ctor (`..., std::shared_ptr<GpuFence> fence`) to `AsyncGpuReadbackSink` storing `m_sharedRenderFence`. In `GpuReadbackRing::pushAndPop`, when a readback is performed, build the `GpuReadbackSurfaceKey { busKey = qHash(frame.bus), outputFrameIndex = frame.outputFrameIndex, format }` and call `GpuReadbackTelemetry::instance().recordSurface(key)` then `recordGpuReadback(key)` — BUT only if this surface key has not already been recorded by another sink this tick. Introduce a process-narrow `SharedReadback` registry (a small `QHash<GpuReadbackSurfaceKey, CpuPlanes>` guarded by a mutex, cleared per tick by the dispatcher in Task 5) so the SECOND sink on the same bus reuses the first sink's `CpuPlanes` and records NO additional `gpuReadbacks`. The audio-atomicity guarantee is already structural: the ring stores the whole `OutputBusFrame` and only swaps `frame.video` to the CPU handle, leaving `frame.audio`/`frame.identity` untouched — verify the implementation copies the WHOLE frame, not just the video handle.

Confirm the copy-detector invariant holds: `snapshot().gpuReadbacks == snapshot().uniqueSurfaces` and `redundantReadbacks == 0` after a multi-sink-same-bus tick.

- [ ] **Step 4: Run the test, expect PASS**
```sh
cmake --build build/c --target tst_asyncgpureadbacksink tst_gpureadbackring \
  && ctest --test-dir build/c -R "tst_asyncgpureadbacksink|tst_gpureadbackring" --output-on-failure
```
Expected: PASS (the structural slots everywhere; the Apple slots on a Metal host, `QSKIP` otherwise).

- [ ] **Step 5: Zero-regression + commit**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/asyncgpureadbacksink.cpp playback/output/gpureadbackring.cpp \
        playback/output/gpureadbackring.h playback/output/asyncgpureadbacksink.h \
        tests/unit/tst_asyncgpureadbacksink.cpp tests/unit/tst_gpureadbackring.cpp
git commit -m "feat(async-readback): shared one-readback-per-surface + atomic A/V under lag"
```

---

## Task 5: Dispatcher routing — one shared readback per rendered bus surface, per-endpoint cadence

**Precondition:** Tasks 1-4. Slots into the existing `OutputDispatcher::dispatchTick` per-bus render loop.

**Files:**
- Modify: `playback/output/outputdispatcher.h` / `.cpp` (per-bus shared readback memo + per-tick clear; write `readbackQueueDepth`/`readbackDrops` from the wrapped sinks), `playback/output/asyncgpureadbacksink.h` (expose the shared-readback memo handle so all wrappers on a bus share it)
- Test: `tests/unit/tst_outputdispatcher.cpp` (extend with a GPU-frame multi-sink-same-bus scenario)

**Interfaces:**
- Consumes: `OutputDispatcher::dispatchTick` (existing), `GpuReadbackTelemetry::snapshot()` (already read at outputdispatcher.cpp:137-139), `AsyncGpuReadbackSink` (Task 3), the `SharedReadback` registry (Task 4).
- Produces: `OutputDispatchStats.readbackQueueDepth` / `.readbackDrops` written from the live wrappers; a per-tick `SharedReadback` reset so the one-readback-per-surface memo does not leak across ticks.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_outputdispatcher.cpp` a scenario: two `AsyncGpuReadbackSink`-wrapped recording sinks both assigned to the SAME bus; feed the dispatcher a GPU-backed cache frame for that bus; tick once; assert `stats.readbackQueueDepth` and `stats.readbackDrops` are populated (non-negative, ring-derived) and `GpuReadbackTelemetry::snapshot().gpuReadbacks == 1` (the two same-bus sinks shared one readback). On a non-GPU host, build the GPU frame via the stub path or `QSKIP` the GPU-specific count assertion but keep the routing-shape assertion (the dispatcher still renders one bus frame and routes both endpoints).
```cpp
    void sameBusSinksShareOneReadback();
    void readbackTelemetryReachesDispatchStats();
```

- [ ] **Step 2: Run the test, expect FAIL**
```sh
cmake --build build/c --target tst_outputdispatcher
```
Expected: FAIL — `stats.readbackQueueDepth`/`readbackDrops` stay 0 (the dispatcher does not yet pull them from the wrappers) and the same-bus shared-readback memo is not cleared per tick.

- [ ] **Step 3: Write the implementation**

In `dispatchTick`, after the per-endpoint loop and before reading the telemetry snapshot:
1. Reset the `SharedReadback` registry at the TOP of the tick (clear last tick's per-surface memo) so the one-readback-per-surface guarantee is scoped to a single tick.
2. After submitting to all endpoints, fold the wrapped sinks' `readbackQueueDepth()`/`readbackDrops()` into `m_stats.readbackQueueDepth` (max over wrappers) and `m_stats.readbackDrops` (sum). Detect an `AsyncGpuReadbackSink` via a virtual `readbackStats(qint64& depth, qint64& drops)` hook on `IOutputSink` (default no-op) so the dispatcher does not dynamic_cast — add the hook to `outputsink.h` returning false by default, overridden by the wrapper.

The shared-readback memo is the `SharedReadback` registry from Task 4: the dispatcher owns one instance, hands it to each `AsyncGpuReadbackSink` at wrap time (Task 6), and clears it each tick. Because the dispatcher already renders one `OutputBusFrame` per bus and shares it across endpoints (the `rendered` hash), and the registry keys by `GpuReadbackSurfaceKey`, the first sink on a bus does the readback and the rest reuse the `CpuPlanes` — one readback per surface, by construction.

- [ ] **Step 4: Run the test, expect PASS**
```sh
cmake --build build/c --target tst_outputdispatcher && ctest --test-dir build/c -R tst_outputdispatcher --output-on-failure
```
Expected: PASS (the existing dispatcher slots + the two new routing slots).

**Review gate:** confirm the per-tick `SharedReadback` reset happens exactly once per tick and cannot race a wrapper still reading (the registry and the wrappers all run on the dispatch thread). Flag for review.

- [ ] **Step 5: Zero-regression + commit**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/outputdispatcher.h playback/output/outputdispatcher.cpp \
        playback/output/outputsink.h playback/output/asyncgpureadbacksink.h \
        playback/output/asyncgpureadbacksink.cpp tests/unit/tst_outputdispatcher.cpp
git commit -m "feat(async-readback): dispatcher one-readback-per-bus-surface routing + telemetry"
```

---

## Task 6: Migrate NDI + Qt preview behind the wrapper (depth-1 PGM, depth-3 others)

**Precondition:** Tasks 1-5.

**Files:**
- Modify: `playback/playbackworker.cpp` (`rebuildOutputEndpoints` — wrap the preview + NDI sinks in `AsyncGpuReadbackSink` when the GPU pipeline is enabled; depth-1 for PGM, depth-3 otherwise; pass the dispatcher's shared-readback registry + `m_renderFence`), `playback/output/outputdispatcher.h` (add the `outputEndpointsForTest()` by-reference test accessor — Step 1a)
- Test: `tests/unit/tst_playbackworker.cpp` (extend — assert the endpoint composition under flag-on vs flag-off)

**Interfaces:**
- Consumes: `rebuildOutputEndpoints` (existing, playbackworker.cpp:388-462), `AsyncGpuReadbackSink` (Task 3), `gpuCapabilityFor` (Task 1), `gpuPipelineEnabled()`, the per-bus depth rule (`bus.kind == OutputBusKind::Pgm ? 1 : 3`), `m_renderFence` (`gpu-sync`).
- Produces: `OutputDispatcher::outputEndpointsForTest()` (a const by-reference accessor over `m_endpoints` for the test); the wiring that puts NDI + preview behind the readback ring.

- [ ] **Step 1a: Add the `outputEndpointsForTest()` accessor on `OutputDispatcher`**

The test inspects the live endpoint sink chain, but the merged `OutputDispatcher` exposes only `endpoints()` returning a *copy* (`playback/output/outputdispatcher.h:92`, `QList<OutputEndpoint> endpoints() const { return m_endpoints; }`); the test needs a by-reference handle to reach into each endpoint's wrapped sink. No such accessor exists, so add one — a minimal, const, by-reference test-support accessor over the existing `m_endpoints` member (confirmed present at `outputdispatcher.h:124` as `QList<OutputEndpoint> m_endpoints;`). In the public test-support section of `playback/output/outputdispatcher.h`, add:
```cpp
    // Test support: by-reference view of the live endpoints so a test can inspect
    // each endpoint's wrapped sink chain (e.g. an AsyncGpuReadbackSink wrapper)
    // without copying. The copying endpoints() accessor stays the production API.
    const QList<OutputEndpoint>& outputEndpointsForTest() const { return m_endpoints; }
```
The worker owns the dispatcher (`m_outputRuntime`), so the test reaches the endpoints through it.

- [ ] **Step 1b: Write the failing test**

Add to `tests/unit/tst_playbackworker.cpp` a slot asserting the endpoint sink chain: with `qunsetenv("OLR_GPU_PIPELINE")` the NDI endpoint's sink is the plain `QueuedOutputSink(NdiOutputSink)` and the preview is the plain `QtPreviewOutputSink` (today's composition, byte-green); with `qputenv("OLR_GPU_PIPELINE", "1")` each is wrapped in an `AsyncGpuReadbackSink` whose `ringDepth()` is 1 for the PGM preview and 3 for NDI / feed / multiview previews. Inspect the endpoints through the dispatcher's `outputEndpointsForTest()` accessor added in Step 1a. Restore the default in `cleanup()` with `qunsetenv("OLR_GPU_PIPELINE")`.
```cpp
    void gpuFlagWrapsSinksInReadbackRing();
    void pgmPreviewGetsDepthOneRing();
```

- [ ] **Step 2: Run the test, expect FAIL**
```sh
cmake --build build/c --target tst_playbackworker
```
Expected: FAIL — the sinks are not wrapped (no `AsyncGpuReadbackSink` in the chain regardless of the flag).

- [ ] **Step 3: Write the implementation**

In `rebuildOutputEndpoints`, wrap at construction. Add a small helper:
```cpp
    auto wrapForGpu = [&](std::unique_ptr<IOutputSink> sink, OutputBusId bus,
                          OutputTargetKind kind, FramePixelFormat fmt) -> std::unique_ptr<IOutputSink> {
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPipelineEnabled()) {
            const int depth = (bus.kind == OutputBusKind::Pgm) ? 1 : 3;  // PGM sub-frame; else lag-2
            return std::make_unique<AsyncGpuReadbackSink>(std::move(sink), depth, fmt,
                                                          gpuCapabilityFor(kind), m_renderFence);
        }
#endif
        return sink;  // flag off / non-GPU build: today's exact sink, no wrapper
    };
```
- Preview loop (playbackworker.cpp:430): `auto sink = wrapForGpu(std::make_unique<QtPreviewOutputSink>(provider), preview.sourceBus, OutputTargetKind::QtPreview, /*Qt preview CPU layout*/ FramePixelFormat::Yuv420p);` (Qt preview uploads a `QVideoFrame`-compatible layout; pass the format `qtpreviewsink` reads back — match `format-canon`'s preview export format).
- NDI (playbackworker.cpp:441): `sink = wrapForGpu(std::make_unique<QueuedOutputSink>(std::make_unique<NdiOutputSink>()), assignment.sourceBus, OutputTargetKind::Ndi, FramePixelFormat::Yuv420p);` — NDI's `packI420` consumes I420, so the readback format is `Yuv420p`. The wrapper sits OUTSIDE the queue so the GPU readback happens before the queued hand-off.

Pass the dispatcher's `SharedReadback` registry into each `AsyncGpuReadbackSink` (the worker already owns the dispatcher via `m_outputRuntime`; expose the registry from the dispatcher and thread it through `wrapForGpu`).

- [ ] **Step 4: Run the test, expect PASS**
```sh
cmake --build build/c --target tst_playbackworker && ctest --test-dir build/c -R tst_playbackworker --output-on-failure
```
Expected: PASS.

**Review gate:** the worker's output wiring runs under `m_outputRuntimeMutex`; confirm the wrapping does not move any fence wait under that lock (the wrapper only constructs here; the readback runs on the dispatch tick). Flag for review.

- [ ] **Step 5: Zero-regression + commit**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/playbackworker.cpp playback/output/outputdispatcher.h tests/unit/tst_playbackworker.cpp
git commit -m "feat(async-readback): migrate NDI + preview behind the readback ring (PGM depth-1)"
```

---

## Task 7: AV-sync-under-lag HARD GATE + NDI marker-continuity through the readback path

**Precondition:** Tasks 1-6. **This is the merge precondition for the whole subproject (spec §9): the readback path must NOT break the AV-sync MAX gate (`<=100 ms`) or NDI marker-continuity (`maxGap<=2`).**

**Files:**
- Modify: `tests/e2e/run_playback_e2e.sh` (gate `readbackQueueDepth`/`readbackDrops` thresholds + assert `gpuReadbacks == uniqueSurfaces` under `OLR_GPU_PIPELINE=1`), the plan's open-decision section (record the measured multiview monitor lead + the chosen option)
- Test: the existing e2e drivers `tests/e2e/run_sync_e2e.sh` (lipsync, `OLR_AV_SYNC_GATE=1`), `tests/e2e/run_ndi_output_e2e.sh` + `run_ndi_playback_e2e.sh` + `run_ndi_e2e_pipe.sh` (`maxGapFrames<=2`), run with `OLR_GPU_PIPELINE=1`

**Interfaces:**
- Consumes: `run_sync_e2e.sh lipsync` MEAN+MAX gates (run_sync_e2e.sh:281-310: `OLR_AV_SYNC_GATE=1`, EBU R37 `-40..+60` mean, `OLR_AV_SYNC_MAX_MS=100` max), the NDI `maxGapFrames<=2` gates (run_ndi_output_e2e.sh:51, run_ndi_playback_e2e.sh:90, run_ndi_e2e_pipe.sh:94/140), the GPU counters emitted by `play_harness.cpp` / parsed by `run_playback_e2e.sh`.
- Produces: the AV-sync + continuity gates proven to pass through the readback path; `readbackQueueDepth`/`readbackDrops` thresholds enforced; the multiview-monitor-lead decision recorded.

- [ ] **Step 1: Write the failing gate (assert through the GPU path)**

Extend `tests/e2e/run_playback_e2e.sh` to add, under an `OLR_GPU_PIPELINE=1` lane (mirroring the Phase-2 GPU lane), assertions on the new counters: `readbackDrops` must be 0 on the steady `play1x` scenario (a drop means a readback frame was overwritten before delivery — a stall), and `gpuReadbacks == uniqueSurfaces` (no redundant readback). Add the NDI + lipsync gates to the e2e lane as GPU-path invocations:
```sh
# NDI continuity through the readback path (maxGap<=2 must survive the ring):
OLR_GPU_PIPELINE=1 tests/e2e/run_ndi_output_e2e.sh ...     # asserts maxGapFrames<=2
OLR_GPU_PIPELINE=1 tests/e2e/run_ndi_playback_e2e.sh ...   # asserts maxGapFrames<=2 + avSync
# AV-sync MAX gate through the readback path (audio travels with delayed video):
OLR_AV_SYNC_GATE=1 OLR_AV_SYNC_MAX_MS=100 OLR_GPU_PIPELINE=1 \
  tests/e2e/run_sync_e2e.sh <sync_harness> lipsync <port>  # asserts mean EBU R37 + max<=100ms
```
> These drivers SKIP (exit 0) without ffmpeg/srt/NDI runtime present, so on a bare CI host they are inert; the gate bites on a host with the toolchain. Add them to the e2e CTest lane the same way the existing NDI/sync e2e tests are registered in `tests/e2e/CMakeLists.txt`, with a `gpu` label so they can be selected.

- [ ] **Step 2: Run the gates, expect FAIL if the path splits A/V or stalls**
```sh
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R "e2e_play" --output-on-failure
OLR_AV_SYNC_GATE=1 OLR_AV_SYNC_MAX_MS=100 OLR_GPU_PIPELINE=1 \
  tests/e2e/run_sync_e2e.sh <sync_harness> lipsync 9200
```
Expected (before the fix is verified): if the ring split the A/V pair or stalled the tick, the MAX gate FAILs (`A/V worst-case offset ...ms exceeds MAX bound 100ms`) or NDI FAILs (`maxGapFrames>2`). If Tasks 2-6 are correct, they should already PASS — this task's job is to PROVE it and lock it in as a gate.

- [ ] **Step 3: Close any gap the gate exposes; record the monitor-lead decision**

If the AV-sync MAX gate fails, the regression is almost certainly an A/V split in the ring (video lagged but audio not, or vice versa). Fix: confirm `GpuReadbackRing` stores and delivers the WHOLE `OutputBusFrame` (Task 2/4) — the audio rides with its own tick's video. If NDI `maxGap>2`, confirm the `NeedsContinuousCadence` re-send path (Task 3) fires when a readback is not ready. Then MEASURE the multiview monitor A/V lead: run `run_sync_e2e.sh lipsync` with a multiview-preview-driven monitor and record the observed audio lead vs the preview video (expected ~33 ms @ 60 fps / ~2 frames). Record the decision in the "Open decision" section of this plan AND as a code comment at the `AsyncGpuReadbackSink` preview-wrap site: either (a) `AudioPlayer` is delayed by the preview ring depth (implement the delay) or (b) the bounded ~33 ms monitor lead is documented-and-accepted (no code change; comment only). The default, absent a contrary measurement, is (b) accept — it is within EBU R37 for a non-PGM monitor and PGM already has the depth-1 mitigation.

- [ ] **Step 4: Run the gates, expect PASS**
```sh
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R "e2e_play|e2e_ndi|e2e_sync" --output-on-failure
```
Expected: AV-sync MEAN (EBU R37) + MAX (`<=100 ms`) PASS, NDI `maxGapFrames<=2` PASS, `readbackDrops==0`, `gpuReadbacks==uniqueSurfaces`, all through `OLR_GPU_PIPELINE=1`.

- [ ] **Step 5: Full pre-flight + final review**
```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R "e2e_play" --output-on-failure
# Off-path byte-green check (fresh build dir):
cmake -S . -B build/off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
cmake --build build/off && ctest --test-dir build/off -L unit --output-on-failure
```
Expected: GPU-on and GPU-off suites both PASS; e2e thresholds unchanged on the CPU lane; AV-sync + NDI continuity gates green through the readback path.

**Review gate (final, whole-subproject):** per CLAUDE.md, the playback worker's threading + every readback/fence change gets an independent fresh-agent concurrency review before merge. The reviewer verifies: (a) no `GpuFence::wait`/`completedValue` under any worker mutex (grep every ring/wrapper call site, confirm dispatch-thread-only); (b) the A/V pair is never split — the ring delivers the whole `OutputBusFrame`; (c) the `NeedsContinuousCadence` re-send keeps `maxGap<=2` and never re-sends a stale-generation surface (check `isStaleForGeneration` before re-send); (d) the one-readback-per-surface memo is reset exactly once per tick. Open the PR with the per-branch push:
```sh
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin gpu-phase4-async-readback
```

- [ ] **Step 6: Commit**
```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add tests/e2e/run_playback_e2e.sh tests/e2e/CMakeLists.txt \
        docs/superpowers/plans/2026-06-21-gpu-phase4-async-readback.md
git commit -m "test(async-readback): AV-sync MAX + NDI maxGap<=2 hard gates through the readback path"
```

---

## Canonical async-readback contract (downstream plans consume these names verbatim)

`gpu-budget`, `device-loss`, and `new-io-targets` build on exactly these types and signatures. Do not rename or vary them.

**Sink cadence routing** — `playback/output/sinkgpucapability.h` (NEW):
```cpp
enum class SinkGpuCapability { GpuNative, AsyncReadbackDedupOk, NeedsContinuousCadence };
SinkGpuCapability gpuCapabilityFor(OutputTargetKind kind);
```

**Fenced readback ring** — `playback/output/gpureadbackring.h` (NEW):
```cpp
struct RingReadyFrame { bool ready = false; OutputBusFrame frame; };
class GpuReadbackRing {
    explicit GpuReadbackRing(int depth);          // 1 = PGM sub-frame; 3 = default CPU-sink lag
    int depth() const; int occupancy() const; qint64 drops() const;
    RingReadyFrame pushAndPop(const OutputBusFrame& gpuFrame, uint64_t fenceValue,
                              std::shared_ptr<GpuFence> fence, FramePixelFormat format);
    RingReadyFrame flushOne(int timeoutMs);
};
```

**Readback wrapper sink** — `playback/output/asyncgpureadbacksink.h` (NEW; `final IOutputSink`):
```cpp
class AsyncGpuReadbackSink final : public IOutputSink {
    AsyncGpuReadbackSink(std::unique_ptr<IOutputSink> inner, int ringDepth,
                         FramePixelFormat cpuFormat, SinkGpuCapability capability,
                         std::shared_ptr<GpuFence> renderFence = GpuFence::create());
    // IOutputSink: kind/start/stop/isActive/submit/outputStatus.
    int ringDepth() const; qint64 readbackDrops() const; qint64 readbackQueueDepth() const;
};
```

**Telemetry** (placeholders added by Phase-2 `telemetry-contract`; this subproject is the FIRST to write non-zero values): `readbackQueueDepth` / `readbackDrops` on `OutputDispatchStats` (`playback/output/outputdispatcher.h:76-78`), folded from the live `AsyncGpuReadbackSink` wrappers in `dispatchTick`, emitted by `play_harness.cpp` and parsed by `run_playback_e2e.sh`. The one-readback-per-surface invariant remains `GpuReadbackTelemetry::snapshot().gpuReadbacks == uniqueSurfaces` with `redundantReadbacks == 0` (covering hold-last / placeholder re-emit). The `fenceWaitStalls` counter (first written by `gpu-sync`) is bumped on a bounded `flushOne` timeout at `stop()`/drain.

**Per-sink depth rule** (consumed by `gpu-budget` for the ring term of the §2 VRAM peak formula): PGM bus = depth-1; every other CPU sink = depth-3. The async-readback ring term of the peak is `Σ (ring-depth × bus × requested CPU format)`.
