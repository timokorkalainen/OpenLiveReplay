# gpu-budget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Add a VRAM-bounded GPU-surface window on top of the merged `gpu-sync` keystone, sized to the spec §2 **peak formula** — `aggregate ≤256 decode window + armed-cut staging bank + Σ per-bus compositor outputs + Σ async-readback rings` — *not* a flat 32–64 and *not* `256 × N-feeds`. The CPU `kGlobalFrameBudget = 256` decode window stays exactly as-is (it is the CPU-side memory cap and the correctness oracle); this subproject layers a *separate*, VRAM-resident GPU surface cap that (1) tracks live GPU-surface bytes against a budget computed from the §2 formula, (2) on a GPU surface allocation failure **degrades to a CPU-backed `FrameHandle`** (re-decode / lock-download) rather than ever aborting the decode loop, (3) **prefetches decode-to-GPU on a predicted reposition**, and (4) evicts under VRAM pressure **through the `gpu-sync` eviction guard** (`collectEvictedVictimLocked` → release `m_bufferMutex` → `drainEvictedVictims` fence-wait/free, honoring the spec §10 lock rule and the surface-lifetime fences). Memory-pressure telemetry (`gpuVramBytes`, `gpuOomDegrades`) — placeholder fields already on `OutputDispatchStats`, emitted by `play_harness.cpp` and parsed by `run_playback_e2e.sh` but **never written today** — is populated for the first time here and asserted by a new multi-feed e2e budget-pressure scenario. **Critical:** the budget accounts for the per-feed × staging-bank multiplier under multiview; it is not a single flat number.

**Architecture:** A small, platform-neutral `playback/gpu/` budget family sits between the worker's GPU decode branch and the `gpu-sync` surface lifetime. A new `GpuBudget` (`playback/gpu/gpubudget.h`) is a process-wide accounting object: it computes the byte budget from the §2 peak-formula terms (`GpuBudgetConfig`), tracks live surface bytes via RAII charge/credit, answers `canAllocate(bytes)`, and exposes occupancy for telemetry. A `GpuBudgetCharge` RAII token charges a surface's bytes on mint and credits them when the last `FrameHandle` ref drops (so eviction through `gpu-sync` frees both the surface and its budget charge atomically). A `GpuSurfaceAllocator` (`playback/gpu/gpusurfaceallocator.h`) is the single mint chokepoint the worker calls instead of `importVtImageBuffer` directly: it consults `GpuBudget`, attempts the GPU mint, and on budget-deny or alloc-failure returns a **CPU-backed** `FrameHandle` (via the existing CPU decode path) while bumping `gpuOomDegrades` — the decode loop never sees a null/abort. A `GpuSeekPrefetch` predictor (`playback/gpu/gpuseekprefetch.h`) lets the worker pre-warm the predicted reposition window to GPU under budget headroom. `GpuBudget` occupancy + degrade counts flow to `OutputDispatchStats::gpuVramBytes`/`gpuOomDegrades` through a new `OutputRuntime::recordGpuBudget(qint64 vramBytes, qint64 oomDegrades)` writer (mirroring the `incrementFenceWaitStalls` pattern `gpu-sync` established). All of it is `#ifdef OLR_GPU_PIPELINE_BUILD` + `gpuPipelineEnabled()`-gated; with the flag off every byte is the merged CPU path.

**Tech Stack:** C++17, Qt 6 (Core/Gui/GuiPrivate for `QRhi`, Test), Apple Metal/CoreVideo/IOSurface (surface bytes from `CVPixelBuffer` plane sizes), Windows D3D11 (the `gpu-import-win` edge; surfaces from `ID3D11Texture2D`), CMake + Ninja. Consumes the merged Phase-0/1/2 + `gpu-sync` contracts verbatim: `FrameHandle`/`IFrameData`/`CpuPlanes`/`FrameMetadata`/`FramePayloadKey`/`FramePixelFormat`/`ColorMetadata` (`playback/output/framehandle.h`), `GpuSurface`/`GpuSurfaceDesc` + `retainUntilFenceRetired(uint64_t)`/`pendingFenceValue()` (`playback/gpu/gpusurface.h`), `GpuFrameData`/`makeGpuFrameHandle` (`playback/gpu/gpuframedata.h`), `GpuFence`/`makeD3D11GpuFence` (`playback/gpu/gpufence.h`), `GpuGenerationCounter` + `FrameHandle::isStaleForGeneration` (`playback/gpu/gpugeneration.h`), `EvictedVictim`/`collectEvictedVictimLocked`/`drainEvictedVictims`/`m_renderFence` + `TrackBuffer::insert(..., FrameHandle* evicted)` (`playback/playbackworker.*`, `playback/trackbuffer.*`), `gpuPipelineEnabled`/`gpuForcedPerTrackBudget`/`gpuConsumeInjectedAllocFailure`/`gpuSetInjectedAllocFailures` (`playback/gpu/gpupipelineconfig.h`), `GpuReadbackTelemetry` (`playback/output/gpureadbacktelemetry.h`), `OutputDispatchStats` (`playback/output/outputdispatcher.h`), `OutputRuntime::stats()` (`playback/output/outputruntime.h`).

## Global Constraints

- **Builds ON merged `gpu-sync`, never replaces it.** Every name above already exists in the tree after `gpu-sync` merged — use the **actual** signatures, do not invent variants. New types introduced here (`GpuBudget`, `GpuBudgetConfig`, `GpuBudgetCharge`, `GpuSurfaceAllocator`, `GpuSeekPrefetch`, `OutputRuntime::recordGpuBudget`) are genuinely new. Eviction under VRAM pressure MUST route through the `gpu-sync` two-phase guard (`collectEvictedVictimLocked` under `m_bufferMutex`, then `drainEvictedVictims` with the lock released) — this subproject never adds a second eviction path that fence-waits under a lock.
- **The CPU 256-window stays; the GPU budget is separate.** `kGlobalFrameBudget = 256` and `capFrames()` (playbackworker.cpp:207-222, the per-track `clamp(…, 12, max(12, 256/trackCount))`) are unchanged. The GPU budget is a *VRAM-resident* cap on GPU-backed surfaces only; CPU-backed frames never charge it. The two caps coexist: a frame can be in the CPU window as a CPU-backed handle even after its GPU surface was budget-evicted (the §2 "refs-only, no second copy" model — `OutputFrameCache` holds refs, `TrackBuffer` owns the authoritative surface).
- **OOM-safe degrade is non-negotiable (spec §10 "VRAM blowup" row).** On *any* GPU surface allocation failure OR budget denial, the allocator returns a **CPU-backed `FrameHandle`** (re-decode / lock-download) and bumps `gpuOomDegrades`. It NEVER returns null to the decode loop and NEVER aborts decode. The existing decode branch already has a `gpuFallback` path (playbackworker.cpp:707-713, :693-696) — this subproject routes that fallback through the budget-aware allocator so the degrade is *counted* and *bounded*, not silent.
- **The §2 peak formula is the sizing authority — account for the multiview multiplier.** The budget is `aggregate decode window (≤256 surfaces) + armed-cut staging bank (the second decoder's covered window) + Σ per-bus compositor outputs (feed/PGM/multiview, one per active bus) + Σ async-readback rings (ring-depth × bus × format)`. Under N-feed multiview the staging bank and the decode window are **per-feed populated** (8 feeds → 32 decode surfaces each within the 256 aggregate, PLUS a staging bank that mirrors the covered window per feed). The budget must size against this, NOT a flat 32–64 and NOT `256 × N`. `GpuBudgetConfig` carries every term explicitly so the test pins the multiplier.
- **CPU path stays default + reference.** Two-gated: `OLR_GPU_PIPELINE` CMake option → `OLR_GPU_PIPELINE_BUILD` compile def, and the runtime `gpuPipelineEnabled()` env flag (off by default). With `OLR_GPU_PIPELINE_BUILD` undefined **or** `gpuPipelineEnabled()` false, every byte of this plan is inert: the CPU path is byte-for-byte the merged path and stays the permanent oracle + fallback. Cheap scalar members compile unconditionally; all budget/charge/allocator logic is `#ifdef OLR_GPU_PIPELINE_BUILD`.
- **The lock rule holds (spec §10, D4).** Budget eviction collects victims under `m_bufferMutex`, **releases it**, then fence-waits/frees via `drainEvictedVictims`. A budget credit may run as a `FrameHandle`/`GpuFrameData` ref drops — that credit is a lock-free atomic, NEVER a fence-wait, so it is safe from any thread without a lock-order hazard. Every fence-wait site inherited from `gpu-sync` keeps its `// LOCK RULE:` proof.
- **Concurrency-critical — independent review required before merge (CLAUDE.md "Verification").** The budget charge/credit crosses the worker (mint) and the output/readback threads (last-ref drop). Each task touching that handoff carries a `**Review gate:**` note; the branch gets a fresh-agent concurrency review before the PR merges.
- **Zero-regression gate after every task.** With the flag off:
  ```sh
  cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
  ```
  must pass with identical assertion values, and `e2e_play` (`ctest -R e2e_play`) golden thresholds unchanged. GPU behavioral tests `QSKIP` where no GPU/RHI backend exists (`offscreen`/CI), never hard-fail.
- **Build (run from the worktree root):** configure once with the GPU pipeline ON (this subproject only compiles under it):
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Also build once with `-DOLR_GPU_PIPELINE=OFF` (a fresh build dir) to confirm the off-path still compiles and the unit suite is byte-green. Unit tests register via `olr_add_unit_test(<name> <libs...>)` in `tests/unit/CMakeLists.txt` (GPU tests inside the `if(OLR_GPU_PIPELINE)` block); the GPU backend lib is `olr_test_gpu`, and `olr_test_playback` carries the worker. Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.
- **Format changed lines only** (CI Lint checks changed lines; engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
- **Public-repo professionalism.** Self-contained, professional code/comments/commits; document the present design, no internal notes or private history.

---

## Preconditions (read before Task 1)

- **`gpu-sync` merged.** `playback/gpu/{gpufence.h,gpufence_apple.mm,gpufence_stub.cpp,gpufence_win.cpp,gpugeneration.{h,cpp}}` exist; `GpuSurface::retainUntilFenceRetired(uint64_t)`/`pendingFenceValue()` are virtuals with no-op defaults (Apple + Windows override them); `FrameMetadata` carries `uint64_t gpuGeneration = 0`; `FrameHandle::isStaleForGeneration(uint64_t)` exists; `TrackBuffer::insert` has the 6th `FrameHandle* evicted = nullptr` param; `PlaybackWorker` has `EvictedVictim`, `collectEvictedVictimLocked(FrameHandle&&)`, `drainEvictedVictims()`, `m_renderFence`; `OutputRuntime::incrementFenceWaitStalls()` is the canonical placeholder-counter writer pattern. Verify with `git merge-base --is-ancestor <gpu-sync-sha> origin/main` if unsure.
- **The telemetry placeholders are wired end-to-end but unwritten for budget.** `OutputDispatchStats::gpuVramBytes` and `gpuOomDegrades` (outputdispatcher.h:75, :79) are emitted by `play_harness.cpp` (the `gpuOomDegrades=%lld gpuVramBytes=%lld` lines at :163/:628) and parsed by `run_playback_e2e.sh` (`get gpuVramBytes`/`get gpuOomDegrades` at :257-258, validated as numeric at :879-880). `gpu-sync` populated `fenceWaitStalls`; THIS subproject is the first to populate `gpuVramBytes`/`gpuOomDegrades`. Do NOT add new harness fields — only write the existing ones.
- **The GPU decode branch already has a fallback hook.** playbackworker.cpp:666-727 (macOS) builds a GPU `FrameHandle` via `importVtImageBuffer(imageBuffer, meta, gpuRhi)` and sets `gpuFallback = true` on `!isPresentable()` or an injected alloc failure (`gpuConsumeInjectedAllocFailure()`), then returns `INT64_MIN` to let the caller re-decode on the CPU path. This subproject inserts the budget-aware allocator at that mint site so the fallback is counted/bounded.
- **No GPU compositor / async-readback yet.** `gpu-compositor` and `async-readback` land in the same Phase-4 wave but are independent subprojects. The §2 "Σ per-bus compositor outputs" and "Σ async-readback rings" terms are therefore sized as **configured reservations** here (the budget reserves headroom for them from `GpuBudgetConfig`), not measured from live compositor/ring objects. When those subprojects land they charge against the reservation through the same `GpuBudgetCharge` token; this plan proves the reservation arithmetic and the decode-window + staging-bank live accounting.

---

## Task 1: `GpuBudget` — VRAM byte budget from the §2 peak formula + live occupancy

**Precondition:** `gpu-sync` merged.

**Files:**
- Create: `playback/gpu/gpubudget.h`, `playback/gpu/gpubudget.cpp`
- Test: `tests/unit/tst_gpubudget.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `FramePixelFormat` (`playback/output/framepixelformat.h`).
- Produces:
  ```cpp
  // playback/gpu/gpubudget.h — platform-neutral; no Metal/D3D types.
  // The §2 peak-formula terms. Every term is explicit so the budget accounts for
  // the multiview per-feed x staging-bank multiplier, not a flat number.
  struct GpuBudgetConfig {
      int feedCount = 1;            // active feeds (multiview width)
      int aggregateDecodeWindow = 256;  // spec kGlobalFrameBudget aggregate cap (surfaces)
      int stagingWindowPerFeed = 0;     // armed-cut staging bank: covered window per feed
      int activeBusCount = 1;           // Sigma per-bus compositor outputs (feed/PGM/multiview)
      int readbackRingDepth = 3;        // async-readback ring depth (PGM is depth-1; others 3)
      int width = 1920;
      int height = 1080;
      FramePixelFormat surfaceFormat = FramePixelFormat::Nv12;  // 8-bit NV12 storage (D2)

      // Bytes of one width x height surface in surfaceFormat (NV12 = w*h*3/2).
      qint64 surfaceBytes() const;
      // The peak-formula byte budget:
      //   (aggregateDecodeWindow + stagingWindowPerFeed*feedCount
      //    + activeBusCount + readbackRingDepth*activeBusCount) * surfaceBytes()
      // The decode window is an AGGREGATE (<=256), so it is NOT multiplied by
      // feedCount; the staging bank IS per-feed populated and IS multiplied.
      qint64 peakBudgetBytes() const;
  };

  // Process-wide VRAM accounting for GPU-backed surfaces. Charge on mint, credit
  // on last-ref drop; canAllocate() gates the next mint against the peak budget.
  // Lock-free reads; a single mutex serializes charge/credit/reconfigure.
  class GpuBudget {
  public:
      static GpuBudget& instance();

      void configure(const GpuBudgetConfig& config);  // recompute the byte budget
      qint64 budgetBytes() const;                      // peakBudgetBytes() of current config
      qint64 liveBytes() const;                        // currently charged surface bytes
      bool canAllocate(qint64 bytes) const;            // liveBytes()+bytes <= budgetBytes()

      // Charge/credit are the primitive the RAII GpuBudgetCharge wraps. Direct use
      // only in GpuBudgetCharge; callers use the token.
      void charge(qint64 bytes);
      void credit(qint64 bytes);

      // Count of times a mint was denied/failed and degraded to a CPU handle.
      qint64 oomDegradeCount() const;
      void noteOomDegrade();

      void reset();  // test-only: zero live bytes + degrade count, keep config
  };
  ```

- [ ] **Step 1: Write the failing test (peak-formula arithmetic + the multiview multiplier)**

Create `tests/unit/tst_gpubudget.cpp`:

```cpp
// GpuBudget sizes the VRAM cap from the spec §2 peak formula. The decode window
// is an AGGREGATE (<=256, NOT x feeds); the armed-cut staging bank IS per-feed.
// This pins the multiview multiplier so the budget is never a flat 32-64.
#include <QtTest>

#include "playback/gpu/gpubudget.h"

class TestGpuBudget : public QObject {
    Q_OBJECT
private slots:
    void surfaceBytesIsNv12();
    void peakFormulaCountsStagingPerFeedNotDecodeWindow();
    void canAllocateGatesAgainstBudget();
    void chargeCreditRoundTrips();
};

void TestGpuBudget::surfaceBytesIsNv12() {
    GpuBudgetConfig c;
    c.width = 1920;
    c.height = 1080;
    c.surfaceFormat = FramePixelFormat::Nv12;
    QCOMPARE(c.surfaceBytes(), qint64(1920) * 1080 * 3 / 2);  // 3110400
}

void TestGpuBudget::peakFormulaCountsStagingPerFeedNotDecodeWindow() {
    GpuBudgetConfig c;
    c.feedCount = 8;
    c.aggregateDecodeWindow = 256;   // aggregate, NOT x8
    c.stagingWindowPerFeed = 32;     // staging bank: per-feed covered window
    c.activeBusCount = 3;            // feed + PGM + multiview
    c.readbackRingDepth = 3;
    c.width = 1920;
    c.height = 1080;
    const qint64 sb = c.surfaceBytes();
    // surfaces = 256 (aggregate) + 32*8 (staging per feed) + 3 (bus outputs)
    //          + 3*3 (readback rings) = 256 + 256 + 3 + 9 = 524
    QCOMPARE(c.peakBudgetBytes(), qint64(524) * sb);
    // A flat 32-64 surface budget would be (32..64)*sb -- prove we are not that.
    QVERIFY(c.peakBudgetBytes() > qint64(64) * sb);
}

void TestGpuBudget::canAllocateGatesAgainstBudget() {
    GpuBudgetConfig c;
    c.feedCount = 1;
    c.aggregateDecodeWindow = 4;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    QCOMPARE(b.budgetBytes(), qint64(4) * sb);
    QVERIFY(b.canAllocate(4 * sb));       // exactly fits
    QVERIFY(!b.canAllocate(5 * sb));      // one over the budget
}

void TestGpuBudget::chargeCreditRoundTrips() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 2;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    QCOMPARE(b.liveBytes(), qint64(0));
    b.charge(sb);
    QCOMPARE(b.liveBytes(), sb);
    QVERIFY(!b.canAllocate(2 * sb));  // only one surface of headroom left
    b.credit(sb);
    QCOMPARE(b.liveBytes(), qint64(0));
}

QTEST_GUILESS_MAIN(TestGpuBudget)
#include "tst_gpubudget.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt` (after the `gpu-sync` tests):

```cmake
    olr_add_unit_test(tst_gpubudget olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpubudget
```
Expected: FAIL to compile — `playback/gpu/gpubudget.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpubudget.h` with the interface above.

Create `playback/gpu/gpubudget.cpp`:

```cpp
#include "playback/gpu/gpubudget.h"

#include <QMutex>

#include <atomic>

qint64 GpuBudgetConfig::surfaceBytes() const {
    const qint64 w = qMax(0, width);
    const qint64 h = qMax(0, height);
    switch (surfaceFormat) {
        case FramePixelFormat::Nv12:
        case FramePixelFormat::Yuv420p:
            return w * h * 3 / 2;  // 8-bit 4:2:0
        case FramePixelFormat::Rgba8:
            return w * h * 4;
        default:
            return w * h * 3 / 2;
    }
}

qint64 GpuBudgetConfig::peakBudgetBytes() const {
    // Spec §2 peak formula. The decode window is an AGGREGATE cap (<=256), so it
    // is added once, NOT multiplied by feedCount. The armed-cut staging bank is
    // per-feed populated, so it IS multiplied by feedCount. The per-bus compositor
    // outputs and the async-readback rings scale with the active bus count.
    const qint64 decode = qMax(0, aggregateDecodeWindow);
    const qint64 staging = qint64(qMax(0, stagingWindowPerFeed)) * qMax(1, feedCount);
    const qint64 busOutputs = qMax(0, activeBusCount);
    const qint64 readbackRings = qint64(qMax(0, readbackRingDepth)) * qMax(0, activeBusCount);
    const qint64 surfaces = decode + staging + busOutputs + readbackRings;
    return surfaces * surfaceBytes();
}

namespace {
QMutex g_mutex;
qint64 g_budgetBytes = 0;
qint64 g_liveBytes = 0;
qint64 g_oomDegrades = 0;
}  // namespace

GpuBudget& GpuBudget::instance() {
    static GpuBudget inst;
    return inst;
}

void GpuBudget::configure(const GpuBudgetConfig& config) {
    QMutexLocker lk(&g_mutex);
    g_budgetBytes = config.peakBudgetBytes();
}

qint64 GpuBudget::budgetBytes() const {
    QMutexLocker lk(&g_mutex);
    return g_budgetBytes;
}

qint64 GpuBudget::liveBytes() const {
    QMutexLocker lk(&g_mutex);
    return g_liveBytes;
}

bool GpuBudget::canAllocate(qint64 bytes) const {
    QMutexLocker lk(&g_mutex);
    if (g_budgetBytes <= 0) return true;  // unconfigured = no cap (degrade-only build)
    return g_liveBytes + bytes <= g_budgetBytes;
}

void GpuBudget::charge(qint64 bytes) {
    QMutexLocker lk(&g_mutex);
    g_liveBytes += bytes;
}

void GpuBudget::credit(qint64 bytes) {
    QMutexLocker lk(&g_mutex);
    g_liveBytes -= bytes;
    if (g_liveBytes < 0) g_liveBytes = 0;
}

qint64 GpuBudget::oomDegradeCount() const {
    QMutexLocker lk(&g_mutex);
    return g_oomDegrades;
}

void GpuBudget::noteOomDegrade() {
    QMutexLocker lk(&g_mutex);
    ++g_oomDegrades;
}

void GpuBudget::reset() {
    QMutexLocker lk(&g_mutex);
    g_liveBytes = 0;
    g_oomDegrades = 0;
}
```

Wire CMake. In `CMakeLists.txt` inside the `if(OLR_GPU_PIPELINE)` block, add `playback/gpu/gpubudget.{h,cpp}` to the `OpenLiveReplay` `target_sources` for ALL platforms (it is platform-neutral C++ — mirror the `gpugeneration.cpp` add). In `tests/CMakeLists.txt`, add `playback/gpu/gpubudget.cpp` to `olr_test_gpu` and to `olr_test_playback`'s GPU block, mirroring `gpugeneration.cpp`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpubudget && ctest --test-dir build/c -R tst_gpubudget --output-on-failure
```
Expected: PASS (4 tests; the budget is pure arithmetic, runs on any host).

- [ ] **Step 5: Verify the zero-regression gate**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: the full unit suite PASSES (GpuBudget is additive; nothing consumes it yet).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpubudget.h playback/gpu/gpubudget.cpp tests/unit/tst_gpubudget.cpp \
        tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-budget): VRAM byte budget from the §2 peak formula + live occupancy"
```

---

## Task 2: `GpuBudgetCharge` — RAII charge/credit tied to the surface lifetime

**Precondition:** Task 1.

**Files:**
- Modify: `playback/gpu/gpubudget.h` (add the RAII token), `playback/gpu/gpubudget.cpp`
- Test: `tests/unit/tst_gpubudget.cpp` (extend)

**Interfaces:**
- Consumes: `GpuBudget` (Task 1), `GpuSurface`/`GpuSurfaceDesc` (`playback/gpu/gpusurface.h`).
- Produces:
  ```cpp
  // playback/gpu/gpubudget.h
  // RAII budget charge: charges `bytes` to GpuBudget on construction, credits the
  // same `bytes` on destruction. Held by the GPU frame data so the budget is
  // credited exactly when the last FrameHandle ref drops (eviction through
  // gpu-sync frees the surface AND credits the budget atomically). Move-only.
  class GpuBudgetCharge {
  public:
      GpuBudgetCharge() = default;                 // null charge (0 bytes)
      explicit GpuBudgetCharge(qint64 bytes);      // charges bytes
      ~GpuBudgetCharge();                          // credits bytes (if non-null)
      GpuBudgetCharge(GpuBudgetCharge&& other) noexcept;
      GpuBudgetCharge& operator=(GpuBudgetCharge&& other) noexcept;
      GpuBudgetCharge(const GpuBudgetCharge&) = delete;
      GpuBudgetCharge& operator=(const GpuBudgetCharge&) = delete;
      qint64 bytes() const { return m_bytes; }
  private:
      qint64 m_bytes = 0;
  };

  // Bytes of a GpuSurface from its desc (w*h*3/2 for NV12). Free function so the
  // allocator/charge can size a surface without GpuBudgetConfig.
  qint64 gpuSurfaceBytes(const GpuSurface& surface);
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpubudget.cpp`:

```cpp
    void chargeTokenCreditsOnDestruction();
    void chargeTokenMoveTransfersOwnership();
```

```cpp
void TestGpuBudget::chargeTokenCreditsOnDestruction() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 4;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    {
        GpuBudgetCharge charge(sb);
        QCOMPARE(b.liveBytes(), sb);  // charged on construction
    }
    QCOMPARE(b.liveBytes(), qint64(0));  // credited on destruction
}

void TestGpuBudget::chargeTokenMoveTransfersOwnership() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 4;
    c.width = 64;
    c.height = 48;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    GpuBudgetCharge a(sb);
    QCOMPARE(b.liveBytes(), sb);
    {
        GpuBudgetCharge moved = std::move(a);  // ownership transfers; no double charge
        QCOMPARE(b.liveBytes(), sb);
    }                                          // moved-to dtor credits ONCE
    QCOMPARE(b.liveBytes(), qint64(0));
    // a is now null; its dtor must NOT credit again (would go negative -> clamped 0)
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpubudget
```
Expected: FAIL to compile — `GpuBudgetCharge` undeclared.

- [ ] **Step 3: Write the implementation**

Add the `GpuBudgetCharge` class and `gpuSurfaceBytes` declaration to `playback/gpu/gpubudget.h`. Implement in `playback/gpu/gpubudget.cpp`:

```cpp
GpuBudgetCharge::GpuBudgetCharge(qint64 bytes) : m_bytes(bytes > 0 ? bytes : 0) {
    if (m_bytes > 0) GpuBudget::instance().charge(m_bytes);
}

GpuBudgetCharge::~GpuBudgetCharge() {
    if (m_bytes > 0) GpuBudget::instance().credit(m_bytes);
}

GpuBudgetCharge::GpuBudgetCharge(GpuBudgetCharge&& other) noexcept : m_bytes(other.m_bytes) {
    other.m_bytes = 0;  // moved-from holds nothing; its dtor is a no-op
}

GpuBudgetCharge& GpuBudgetCharge::operator=(GpuBudgetCharge&& other) noexcept {
    if (this != &other) {
        if (m_bytes > 0) GpuBudget::instance().credit(m_bytes);
        m_bytes = other.m_bytes;
        other.m_bytes = 0;
    }
    return *this;
}

qint64 gpuSurfaceBytes(const GpuSurface& surface) {
    const GpuSurfaceDesc d = surface.desc();
    GpuBudgetConfig c;
    c.width = d.width;
    c.height = d.height;
    c.surfaceFormat = d.format;
    return c.surfaceBytes();
}
```

Add `#include "playback/gpu/gpusurface.h"` to `gpubudget.cpp` (the header stays free of it — `gpuSurfaceBytes` is declared with a forward `class GpuSurface;` in the header, defined in the .cpp; OR include `gpusurface.h` in the header since both are platform-neutral. Use the forward-decl form in the header to keep includes minimal).

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpubudget && ctest --test-dir build/c -R tst_gpubudget --output-on-failure
```
Expected: PASS (6 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpubudget.h playback/gpu/gpubudget.cpp tests/unit/tst_gpubudget.cpp
git commit -m "feat(gpu-budget): RAII GpuBudgetCharge ties budget credit to surface lifetime"
```

---

## Task 3: Carry a `GpuBudgetCharge` on `GpuFrameData` so eviction credits the budget

**Precondition:** Task 2. (`gpu-sync` eviction drops the last `FrameHandle` ref; `GpuFrameData`'s dtor then runs and must credit the budget.)

**Files:**
- Modify: `playback/gpu/gpuframedata.h` (add a `GpuBudgetCharge` member + a factory overload that takes one), `playback/gpu/gpuframedata.cpp`
- Test: `tests/unit/tst_gpuframedata.cpp` (extend — Apple-guarded budget-credit slot)

**Interfaces:**
- Consumes: `GpuFrameData`/`makeGpuFrameHandle` (existing — gpuframedata.h:14-38), `GpuBudgetCharge`/`gpuSurfaceBytes` (Task 2).
- Produces (EXTEND the canonical `gpu-sync` render-fence factory into ONE combined 5-arg form that ALSO carries the budget charge; keep the existing 3-arg factory delegating with a null fence + null charge for back-compat / the stub lib). Do NOT add a competing 4-arg `(…, GpuBudgetCharge)` overload — it would collide with `gpu-sync`'s canonical 4-arg `(…, std::shared_ptr<GpuFence> renderFence)` (same arity, same leading three param types) and would drop the render fence (a correctness regression). The budgeted mint threads BOTH the render fence (so every GPU frame still shares the one render/readback timeline `gpu-sync` established) AND the budget charge:
  ```cpp
  // playback/gpu/gpuframedata.h
  // NEW: build a GPU handle that carries BOTH the gpu-sync render fence AND a
  // budget charge. The render fence is the shared render/readback timeline
  // (gpu-sync); the charge is credited when this GpuFrameData's last shared ref
  // drops (the surface is freed) — so budget eviction (gpu-sync
  // drainEvictedVictims) frees the surface AND credits the budget in one
  // ref-drop. The 3-arg factory keeps a null fence + null charge (existing call
  // sites + the off-budget build are unchanged). There is NO 4-arg
  // (…, GpuBudgetCharge) overload — that would shadow gpu-sync's canonical 4-arg
  // (…, renderFence) factory and silently drop the render fence.
  FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                                 std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                                 std::shared_ptr<GpuFence> renderFence,
                                 GpuBudgetCharge charge);
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpuframedata.cpp`:

```cpp
#ifdef __APPLE__
    void droppingGpuHandleCreditsBudget();
#endif
```

```cpp
#ifdef __APPLE__
void TestGpuFrameData::droppingGpuHandleCreditsBudget() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    auto renderFence = GpuFence::create();  // gpu-sync render/readback timeline
    GpuBudget::instance().reset();
    const qint64 bytes = gpuSurfaceBytes(*surface);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    {
        // 5-arg combined mint: render fence (gpu-sync) + budget charge (this plan).
        FrameHandle h = makeGpuFrameHandle(surface, rhi, meta, renderFence, GpuBudgetCharge(bytes));
        QCOMPARE(GpuBudget::instance().liveBytes(), bytes);  // charged while the handle lives
    }
    QCOMPARE(GpuBudget::instance().liveBytes(), qint64(0));  // credited when the handle drops
}
#endif
```

Add the includes near the top of the file (guarded as the other Apple includes already are):

```cpp
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpufence.h"
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpuframedata
```
Expected: FAIL to compile — no 5-arg `makeGpuFrameHandle` overload taking a `std::shared_ptr<GpuFence>` + a `GpuBudgetCharge`.

- [ ] **Step 3: Write the implementation**

In `playback/gpu/gpuframedata.h`, add `#include "playback/gpu/gpubudget.h"`, a private `GpuBudgetCharge m_budgetCharge;` member to `GpuFrameData` (alongside the `std::shared_ptr<GpuFence> m_renderFence;` `gpu-sync` already added), a constructor overload that takes BOTH the render fence and the charge (moving both in), and declare the combined 5-arg `makeGpuFrameHandle`. Do NOT declare a 4-arg `(…, GpuBudgetCharge)` overload — it collides with `gpu-sync`'s canonical 4-arg `(…, std::shared_ptr<GpuFence> renderFence)` factory.

In `playback/gpu/gpuframedata.cpp`, give `GpuFrameData` the fence+charge-taking constructor (stores `std::move(renderFence)` into `m_renderFence` and `std::move(charge)` into `m_budgetCharge`); the charge is dropped (credited) automatically when the `GpuFrameData` is destroyed, and the render fence keeps the `gpu-sync` readback ordering. Implement the combined 5-arg factory mirroring the existing 3-arg one but constructing the `GpuFrameData` with the fence + charge; collapse the `gpu-sync` 4-arg `(…, renderFence)` overload into the 5-arg form by delegating with a null charge, and have the existing 3-arg factory delegate with a null fence + null charge so no existing call site changes behavior:

```cpp
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge charge) {
    // ... same surface/meta validation as the 3-arg factory ...
    auto data = std::make_shared<GpuFrameData>(std::move(surface), std::move(rhi),
                                               meta.key.format, std::move(renderFence),
                                               std::move(charge));
    return FrameHandle(std::move(data), meta);
}

// gpu-sync's render-fence factory now delegates with a null charge.
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence) {
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), meta, std::move(renderFence),
                              GpuBudgetCharge{});
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta) {
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), meta,
                              std::shared_ptr<GpuFence>{}, GpuBudgetCharge{});
}
```

This keeps both axes on ONE signature: the canonical mint the allocator uses (Task 4) passes BOTH the render fence (threaded from the worker's `m_renderFence`) and the budget charge. There is no separate 4-arg `(…, charge)` overload, so the render fence is never dropped.

**Review gate:** the budget credit runs in `~GpuFrameData`, which fires when the last `FrameHandle` ref drops — on the worker thread (cap eviction drain), the output thread (cache trim), or a readback thread. `GpuBudget::credit` is mutex-guarded and lock-free of any fence, so it is safe from any thread; confirm no path holds `m_bufferMutex` across a `~GpuFrameData` that could also fence-wait (it cannot — credit never waits). Flag for review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpuframedata && ctest --test-dir build/c -R tst_gpuframedata --output-on-failure
```
Expected: PASS (existing slots + `droppingGpuHandleCreditsBudget` on a GPU host).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp tests/unit/tst_gpuframedata.cpp
git commit -m "feat(gpu-budget): GpuFrameData carries a budget charge credited on last-ref drop"
```

---

## Task 4: `GpuSurfaceAllocator` — budget-gated mint with OOM-safe degrade to a CPU handle

**Precondition:** Task 3. **This is the spec §10 "VRAM blowup / never let GPU alloc failure crash decode" task.**

**Files:**
- Create: `playback/gpu/gpusurfaceallocator.h`, `playback/gpu/gpusurfaceallocator.cpp`
- Test: `tests/unit/tst_gpusurfaceallocator.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuBudget`/`GpuBudgetCharge`/`gpuSurfaceBytes` (Tasks 1-2), `GpuSurface` (`gpusurface.h`), `FrameHandle`/`FrameMetadata`/`CpuPlanes`/`makeCpuFrameHandle` (`framehandle.h`), the combined 5-arg `makeGpuFrameHandle` (Task 3), `GpuFence` + the worker's `m_renderFence` (`gpufence.h`, `gpu-sync`), `GpuRhiContext` (`gpurhicontext.h`), `gpuConsumeInjectedAllocFailure` (`gpupipelineconfig.h`).
- Produces:
  ```cpp
  // playback/gpu/gpusurfaceallocator.h — platform-neutral.
  // The single mint chokepoint. Given a freshly-decoded surface + the gpu-sync
  // render fence + a CPU-plane fallback producer, it either (a) charges the budget
  // and returns a GPU-backed FrameHandle carrying BOTH the render fence and the
  // charge, or (b) on budget-deny / alloc-failure / injected failure, returns
  // a CPU-backed FrameHandle from the fallback and bumps the OOM-degrade count.
  // It NEVER returns a null handle to the decode loop (spec §10).
  struct GpuMintResult {
      FrameHandle handle;       // always presentable (GPU- or CPU-backed)
      bool degradedToCpu = false;
  };

  // renderFence: the worker's shared render/readback timeline (m_renderFence) —
  // threaded into the GPU mint so every budgeted frame keeps the gpu-sync fence.
  // cpuFallback() produces the CPU planes for this frame (re-decode / lock-download)
  // — invoked ONLY on the degrade path, so the happy GPU path pays nothing.
  GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                                 std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                                 std::shared_ptr<GpuFence> renderFence,
                                 const std::function<CpuPlanes()>& cpuFallback);
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpusurfaceallocator.cpp`:

```cpp
// The surface allocator is the spec §10 OOM-safe degrade chokepoint: a GPU mint
// that the budget denies (or that alloc-fails) returns a CPU-backed handle from
// the fallback and bumps the degrade count -- it NEVER returns null to the decode
// loop. Under budget headroom it returns a charged GPU handle.
#include <QtTest>

#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurfaceallocator.h"
#include "playback/output/framehandle.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpurhicontext.h"
#endif

#include <functional>

class TestGpuSurfaceAllocator : public QObject {
    Q_OBJECT
private slots:
    void budgetDenyDegradesToCpuNeverNull();
#ifdef __APPLE__
    void headroomMintsChargedGpuHandle();
#endif
};

namespace {
CpuPlanes solidPlanes(int w, int h) {
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = w;
    p.height = h;
    p.stride[0] = w;
    p.stride[1] = (w + 1) / 2;
    p.stride[2] = (w + 1) / 2;
    p.plane[0] = QByteArray(w * h, 16);
    p.plane[1] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), 128);
    p.plane[2] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), 128);
    return p;
}
}  // namespace

void TestGpuSurfaceAllocator::budgetDenyDegradesToCpuNeverNull() {
    // Budget = 0 surfaces -> every GPU mint is denied -> degrade to CPU.
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 0;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    // A null surface forces the deny path on any host (no RHI needed). The render
    // fence is also null here; the deny path never reaches the GPU mint anyway.
    GpuMintResult r =
        mintGpuOrDegrade(nullptr, nullptr, meta, nullptr, [] { return solidPlanes(64, 48); });
    QVERIFY(!r.handle.isNull());          // NEVER null
    QVERIFY(r.handle.isPresentable());    // a usable CPU frame
    QVERIFY(!r.handle.isGpuBacked());     // degraded to CPU
    QVERIFY(r.degradedToCpu);
    QVERIFY(b.oomDegradeCount() >= 1);    // counted
}

#ifdef __APPLE__
void TestGpuSurfaceAllocator::headroomMintsChargedGpuHandle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    auto renderFence = GpuFence::create();  // gpu-sync render/readback timeline
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuMintResult r =
        mintGpuOrDegrade(surface, rhi, meta, renderFence, [] { return solidPlanes(64, 48); });
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isGpuBacked());      // budget had headroom -> real GPU mint
    QVERIFY(!r.degradedToCpu);
    QVERIFY(b.liveBytes() > 0);           // charged
}
#endif

QTEST_GUILESS_MAIN(TestGpuSurfaceAllocator)
#include "tst_gpusurfaceallocator.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:

```cmake
    olr_add_unit_test(tst_gpusurfaceallocator olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpusurfaceallocator
```
Expected: FAIL to compile — `playback/gpu/gpusurfaceallocator.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpusurfaceallocator.h` with the interface above (include `<functional>`, `<memory>`, `framehandle.h`; forward-declare `GpuSurface`/`GpuRhiContext`/`GpuFence`).

Create `playback/gpu/gpusurfaceallocator.cpp`:

```cpp
#include "playback/gpu/gpusurfaceallocator.h"

#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpusurface.h"

namespace {
FrameHandle degradeToCpu(FrameMetadata meta, const std::function<CpuPlanes()>& cpuFallback) {
    GpuBudget::instance().noteOomDegrade();
    CpuPlanes planes = cpuFallback ? cpuFallback() : CpuPlanes{};
    if (!planes.isValid()) return FrameHandle{};  // caller's last resort (should not happen)
    return makeCpuFrameHandle(std::move(planes), meta);
}
}  // namespace

GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence,
                               const std::function<CpuPlanes()>& cpuFallback) {
    GpuMintResult result;

    // Injected micro-stress OOM (shared with the gpu-abstraction slice): treat as
    // an alloc failure -> degrade.
    const bool injected = gpuConsumeInjectedAllocFailure();

    if (injected || !surface || !surface->isValid() || !rhi) {
        result.handle = degradeToCpu(meta, cpuFallback);
        result.degradedToCpu = true;
        return result;
    }

    const qint64 bytes = gpuSurfaceBytes(*surface);
    if (!GpuBudget::instance().canAllocate(bytes)) {
        // Budget pressure: degrade rather than blow VRAM. The decode loop never
        // sees a failure; the cap-evict + drain (gpu-sync) frees headroom for the
        // next GPU mint.
        result.handle = degradeToCpu(meta, cpuFallback);
        result.degradedToCpu = true;
        return result;
    }

    // Headroom exists: charge the budget and mint a GPU-backed handle carrying BOTH
    // the gpu-sync render fence AND the budget charge (one combined 5-arg factory —
    // never a 4-arg (…, charge) overload that would drop the render fence). The
    // charge rides on the GpuFrameData and is credited when the surface is freed.
    GpuBudgetCharge charge(bytes);
    FrameHandle gpu = makeGpuFrameHandle(std::move(surface), std::move(rhi), meta,
                                         std::move(renderFence), std::move(charge));
    if (!gpu.isPresentable()) {
        // The GPU mint itself failed after the budget check (rare) -> degrade.
        result.handle = degradeToCpu(meta, cpuFallback);
        result.degradedToCpu = true;
        return result;
    }
    result.handle = std::move(gpu);
    result.degradedToCpu = false;
    return result;
}
```

Wire CMake: add `playback/gpu/gpusurfaceallocator.{h,cpp}` to the `OpenLiveReplay` `if(OLR_GPU_PIPELINE)` `target_sources` (all platforms — platform-neutral), and to `olr_test_gpu` + `olr_test_playback`'s GPU block in `tests/CMakeLists.txt`, mirroring `gpubudget.cpp`.

**Review gate:** this is the OOM-degrade guarantee. The reviewer confirms: (a) every non-happy branch returns a presentable CPU handle (never null) — `cpuFallback` produces valid planes; (b) `gpuConsumeInjectedAllocFailure` is consumed exactly once per mint (it is one-shot); (c) the budget charge is constructed only AFTER `canAllocate` passed and is moved into the handle (no leak on the failure-after-check branch — the `GpuBudgetCharge` dtor credits if `makeGpuFrameHandle` did not consume it); (d) the GPU mint goes through the combined 5-arg `makeGpuFrameHandle(surface, rhi, meta, renderFence, charge)` so the `gpu-sync` render fence is NEVER dropped (there is no 4-arg `(…, charge)` overload to accidentally call). Flag for review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpusurfaceallocator && ctest --test-dir build/c -R tst_gpusurfaceallocator --output-on-failure
```
Expected: PASS (the deny-degrade slot runs on any host; the headroom-mint slot runs on a GPU host, `QSKIP`s otherwise).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpusurfaceallocator.h playback/gpu/gpusurfaceallocator.cpp \
        tests/unit/tst_gpusurfaceallocator.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-budget): budget-gated surface allocator with OOM-safe degrade to CPU handle"
```

---

## Task 5: Route the worker's GPU decode mint through the budget allocator + configure the budget

**Precondition:** Task 4. Grounds in the merged macOS GPU decode branch (playbackworker.cpp:666-727): today it calls `importVtImageBuffer(imageBuffer, meta, gpuRhi)` and sets `gpuFallback` on failure. This task swaps the raw mint for `mintGpuOrDegrade` (counted/bounded degrade) and configures `GpuBudget` from the live feed/staging geometry.

**Files:**
- Modify: `playback/playbackworker.cpp` (configure `GpuBudget` in the GPU graph-init; route the macOS mint site — and the Windows branch at :728-790 — through `mintGpuOrDegrade`), `playback/playbackworker.h` (a small helper decl if needed)
- Test: `tests/unit/tst_playbackworker.cpp` (extend, GPU-gated) OR rely on the Task 7 e2e — pick the unit hook below for a deterministic red-first.

**Interfaces:**
- Consumes: `mintGpuOrDegrade`/`GpuMintResult` (Task 4), `GpuBudget`/`GpuBudgetConfig` (Task 1), the merged decode branch + `capFrames`/`m_outputFeedCount`/`kStagingSpanMs`.
- Produces:
  ```cpp
  // playback/playbackworker.h (private, under OLR_GPU_PIPELINE_BUILD guards or a
  // cheap unconditional decl):
  // Compute the §2 peak-formula budget config from the live geometry and apply it
  // to GpuBudget. Called from the GPU graph-init (initializeOutputGraph) and on a
  // feed-count change. feedCount = m_outputFeedCount; stagingWindowPerFeed derived
  // from kStagingSpanMs / frameDurMs; activeBusCount from the active bus set
  // (feed/PGM/multiview); readbackRingDepth = 3 (PGM async-readback is depth-1, a
  // later async-readback refinement).
  void configureGpuBudget();
  ```

- [ ] **Step 1: Write the failing test**

Add a GPU-gated slot to `tests/unit/tst_playbackworker.cpp` that drives a tiny GPU budget and asserts a degrade is counted (or, if `tst_playbackworker` cannot reach the private mint site, assert the public `outputStats().gpuVramBytes`/`gpuOomDegrades` reflect the budget after a forced-tiny-budget decode). The simplest deterministic red-first hook:

```cpp
#if defined(OLR_GPU_PIPELINE_BUILD)
    void gpuBudgetConfiguredFromGeometry();
#endif
```

```cpp
#if defined(OLR_GPU_PIPELINE_BUILD)
void TestPlaybackWorker::gpuBudgetConfiguredFromGeometry() {
    // After the GPU graph initializes for an N-feed layout, the budget must size
    // to the §2 peak formula (NOT a flat number, NOT 256*N). We assert the budget
    // bytes are at least the aggregate decode window AND grow with the staging
    // bank per feed -- proving the multiview multiplier is present.
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuBudget::instance().reset();
    GpuBudgetConfig oneFeed;
    oneFeed.feedCount = 1;
    oneFeed.aggregateDecodeWindow = 256;
    oneFeed.stagingWindowPerFeed = 32;
    oneFeed.activeBusCount = 1;
    oneFeed.readbackRingDepth = 3;
    GpuBudgetConfig eightFeed = oneFeed;
    eightFeed.feedCount = 8;
    eightFeed.activeBusCount = 3;  // feed + PGM + multiview
    // The staging bank is per-feed, so 8 feeds must budget MORE than 1 feed even
    // though the decode window aggregate is identical.
    QVERIFY(eightFeed.peakBudgetBytes() > oneFeed.peakBudgetBytes());
    qunsetenv("OLR_GPU_PIPELINE");
}
#endif
```

(This unit pins the formula arithmetic the worker applies; the live worker wiring is exercised end-to-end by the Task 7 e2e. If `tst_playbackworker` already includes `gpubudget.h` transitively, no extra include is needed; otherwise add `#include "playback/gpu/gpubudget.h"` under the GPU guard.)

- [ ] **Step 2: Run the test, expect FAIL (then PASS after build)**

```sh
cmake --build build/c --target tst_playbackworker
```
Expected: FAIL to compile until `gpubudget.h` is included / the slot is registered; after reconfigure it builds and the arithmetic assertion PASSES (the worker wiring below is what the e2e covers).

- [ ] **Step 3: Write the worker wiring**

In `playback/playbackworker.cpp`:

1. **Add `configureGpuBudget()`** (under `OLR_GPU_PIPELINE_BUILD`):
   ```cpp
   void PlaybackWorker::configureGpuBudget() {
   #ifdef OLR_GPU_PIPELINE_BUILD
       if (!gpuPipelineEnabled()) return;
       GpuBudgetConfig cfg;
       cfg.feedCount = qMax(1, m_outputFeedCount);
       cfg.aggregateDecodeWindow = kGlobalFrameBudget;  // 256 aggregate, NOT x feeds
       // Staging bank: the armed-cut second decoder covers [target, target+span]
       // per feed. Frames per feed = ceil(kStagingSpanMs / frameDurMs).
       const int64_t dur = qMax<int64_t>(1, frameDurMs());
       cfg.stagingWindowPerFeed = int((int64_t(kStagingSpanMs) + dur - 1) / dur);
       // Active buses: feed bus always; PGM + multiview when their preview
       // providers are attached (Σ per-bus compositor outputs).
       int buses = 1;
       if (m_pgmPreviewProvider) ++buses;
       if (m_multiviewPreviewProvider) ++buses;
       cfg.activeBusCount = buses;
       cfg.readbackRingDepth = 3;  // async-readback default; PGM depth-1 refined later
       cfg.width = m_outputWidth;
       cfg.height = m_outputHeight;
       cfg.surfaceFormat = FramePixelFormat::Nv12;
       GpuBudget::instance().configure(cfg);
   #endif
   }
   ```
   Call it at the tail of `initializeOutputGraph` (after `m_outputFeedCount`/`m_outputWidth`/`m_outputHeight` are set) and in `setBusPreviewProviders`/`rebuildOutputEndpoints` where the active-bus set changes — so a multiview attach re-sizes the budget.

2. **Route the macOS mint through the allocator.** In the macOS GPU decode branch (playbackworker.cpp:698-714), replace the raw `importVtImageBuffer` + manual `gpuFallback` with `mintGpuOrDegrade`. The CPU fallback re-uses the existing software/lock-download decode for this AU (the same path the current `gpuFallback=true` return triggers when the caller re-decodes). Concretely, build a `cpuFallback` lambda that produces the `CpuPlanes` for this frame (via the existing `convertToMediaVideoFrame`/lock-download — wrap the AVFrame conversion the non-GPU branch already performs), then:
   ```cpp
   auto surface = wrapAppleImageBuffer(imageBuffer);  // gpu-abstraction factory
   GpuMintResult mint = mintGpuOrDegrade(
       surface, gpuRhi, meta, m_renderFence,  // gpu-sync render/readback timeline
       [&]() -> CpuPlanes { return cpuPlanesForCurrentAu(); });  // existing CPU decode
   if (mint.degradedToCpu) {
       // Counted by GpuBudget::noteOomDegrade(); surface stats updated in graph tick.
   }
   FrameHandle mediaFrame = std::move(mint.handle);
   if (!mediaFrame.isPresentable()) {
       gpuFallback = true;  // last resort: let the caller re-decode (unchanged)
       return;
   }
   if (decodeFence) decodeFence->signalDecodeDone();
   gpuInserted = commitMediaFrame(mediaFrame, framePtsMs);
   ```
   Keep the existing eviction wiring untouched: `commitMediaFrame` already calls `track->buffer.insert(...)`; THIS subproject does not change the eviction path — the `gpu-sync` `collectEvictedVictimLocked`/`drainEvictedVictims` (if merged into `commitMediaFrame`) continues to free surfaces, and the budget is credited automatically by the `GpuBudgetCharge` riding on the freed `GpuFrameData`.

3. **Windows branch (playbackworker.cpp:728-790):** route its `imported` GPU handle through `mintGpuOrDegrade` the same way (the `WinGpuImportEdge` produces a `GpuSurface`; pass it + `m_renderFence` + a CPU fallback). Guard both branches so the off-build is unchanged.

- [ ] **Step 4: Run the test + targeted worker tests, expect PASS**

```sh
cmake --build build/c --target tst_playbackworker && ctest --test-dir build/c -R tst_playbackworker --output-on-failure
```
Expected: PASS; the CPU path is unchanged (the budget config is a no-op when `gpuPipelineEnabled()` is false).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/playbackworker.cpp playback/playbackworker.h tests/unit/tst_playbackworker.cpp
git commit -m "feat(gpu-budget): route worker GPU mint through the budget allocator; size to §2 formula"
```

---

## Task 6: Surface VRAM occupancy + OOM-degrade count to `OutputDispatchStats` telemetry

**Precondition:** Task 5. Mirrors the `gpu-sync` `incrementFenceWaitStalls()` writer pattern: the placeholder fields `gpuVramBytes`/`gpuOomDegrades` exist on `OutputDispatchStats` (outputdispatcher.h:75,:79), are emitted by `play_harness.cpp` and parsed by `run_playback_e2e.sh`, but are never written for the budget. This task is their first writer.

**Files:**
- Modify: `playback/output/outputruntime.h`/`.cpp` (add `recordGpuBudget(qint64 vramBytes, qint64 oomDegrades)` folding into the stats), `playback/output/outputdispatcher.h`/`.cpp` if the stat is owned there
- Modify: `playback/playbackworker.cpp` (push `GpuBudget` occupancy/degrade into the runtime each tick / on change)
- Test: `tests/unit/tst_outputruntime.cpp` (extend) OR `tst_gpubudget.cpp` if the writer is testable standalone

**Interfaces:**
- Consumes: `GpuBudget::liveBytes()`/`oomDegradeCount()` (Task 1), `OutputRuntime::stats()` (existing), the `incrementFenceWaitStalls` pattern from `gpu-sync`.
- Produces:
  ```cpp
  // playback/output/outputruntime.h (public):
  // Fold the current GPU VRAM occupancy + cumulative OOM-degrade count into the
  // dispatch stats so stats().gpuVramBytes / .gpuOomDegrades reflect the budget.
  // Thread-safe (same internal stats mutex as incrementFenceWaitStalls). The
  // worker pushes these from GpuBudget each tick; the e2e harness reads them.
  void recordGpuBudget(qint64 vramBytes, qint64 oomDegrades);
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_outputruntime.cpp` (or the GPU-gated runtime test):

```cpp
    void recordGpuBudgetSurfacesInStats();
```

```cpp
void TestOutputRuntime::recordGpuBudgetSurfacesInStats() {
    OutputRuntime runtime(FrameRate{60, 1}, /*feedCount*/ 1, 64, 48);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(0));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(0));
    runtime.recordGpuBudget(/*vramBytes*/ 3110400, /*oomDegrades*/ 2);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(3110400));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(2));
    // Idempotent latest-value semantics (not additive): a second push overwrites.
    runtime.recordGpuBudget(6220800, 3);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(6220800));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(3));
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_outputruntime
```
Expected: FAIL — `recordGpuBudget` is not a member of `OutputRuntime`.

- [ ] **Step 3: Write the implementation**

In `playback/output/outputruntime.h`, declare `void recordGpuBudget(qint64 vramBytes, qint64 oomDegrades);` and add two `std::atomic<qint64> m_gpuVramBytes{0}, m_gpuOomDegrades{0};` members (mirroring how `gpu-sync` stored the fence-wait counter). In `outputruntime.cpp`, implement `recordGpuBudget` as a latest-value store (release), and fold both into the `OutputDispatchStats` returned by `stats()`:
```cpp
void OutputRuntime::recordGpuBudget(qint64 vramBytes, qint64 oomDegrades) {
    m_gpuVramBytes.store(vramBytes, std::memory_order_release);
    m_gpuOomDegrades.store(oomDegrades, std::memory_order_release);
}
```
In `stats()`, after building the `OutputDispatchStats`, set:
```cpp
    s.gpuVramBytes = m_gpuVramBytes.load(std::memory_order_acquire);
    s.gpuOomDegrades = m_gpuOomDegrades.load(std::memory_order_acquire);
```
(If `gpu-sync` already folds `fenceWaitStalls` into `stats()` this way, add these two next to it — same merge point.)

In `playback/playbackworker.cpp`, push the budget into the runtime where the worker already surfaces stats (e.g. the run-loop tick / `emitTelemetry`), under the GPU guard:
```cpp
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled() && m_outputRuntime) {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);  // LOCK RULE: independent of m_bufferMutex
        m_outputRuntime->recordGpuBudget(GpuBudget::instance().liveBytes(),
                                         GpuBudget::instance().oomDegradeCount());
    }
#endif
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_outputruntime && ctest --test-dir build/c -R tst_outputruntime --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/outputruntime.h playback/output/outputruntime.cpp \
        playback/playbackworker.cpp tests/unit/tst_outputruntime.cpp
git commit -m "feat(gpu-budget): surface VRAM occupancy + OOM-degrade count via OutputDispatchStats"
```

---

## Task 7: Seek-prefetch — decode-to-GPU on a predicted reposition under budget headroom

**Precondition:** Tasks 4-6.

**Files:**
- Create: `playback/gpu/gpuseekprefetch.h`, `playback/gpu/gpuseekprefetch.cpp`
- Test: `tests/unit/tst_gpuseekprefetch.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`, `playback/playbackworker.cpp` (consult the predictor at the reposition site)

**Interfaces:**
- Consumes: `GpuBudget::canAllocate`/`budgetBytes`/`liveBytes` (Task 1), `frameDurMs`/`kLeadMs`/`kStagingSpanMs` constants, the reposition path (`repositionTo`, playbackworker.cpp).
- Produces:
  ```cpp
  // playback/gpu/gpuseekprefetch.h — platform-neutral; pure prediction policy.
  // Predicts the window a reposition will need and how many surfaces to pre-warm
  // to GPU, bounded by current budget headroom so prefetch never forces an
  // OOM-degrade of the live window.
  struct GpuPrefetchPlan {
      int64_t startMs = -1;       // first ms to prefetch (inclusive)
      int64_t endMs = -1;         // last ms (inclusive)
      int surfaceCount = 0;       // surfaces to pre-warm (0 = no prefetch)
  };

  class GpuSeekPrefetch {
  public:
      // targetMs: the predicted reposition target; dir: +1 forward / -1 reverse.
      // frameDurMs/leadMs size the predicted window; surfaceBytes sizes the
      // headroom check. The plan pre-warms only as many surfaces as fit in
      // (budgetBytes - liveBytes) so prefetch yields to the live decode window.
      static GpuPrefetchPlan planPrefetch(int64_t targetMs, int dir, int64_t frameDurMs,
                                          int64_t leadMs, qint64 surfaceBytes,
                                          qint64 budgetBytes, qint64 liveBytes);
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpuseekprefetch.cpp`:

```cpp
// Seek-prefetch predicts the reposition window and pre-warms surfaces to GPU,
// bounded by budget headroom so prefetch never evicts the live window. No
// headroom -> no prefetch; ample headroom -> the full predicted window.
#include <QtTest>

#include "playback/gpu/gpuseekprefetch.h"

class TestGpuSeekPrefetch : public QObject {
    Q_OBJECT
private slots:
    void forwardPlanCoversLeadWindow();
    void prefetchBoundedByHeadroom();
    void noHeadroomNoPrefetch();
};

void TestGpuSeekPrefetch::forwardPlanCoversLeadWindow() {
    // 60fps (dur=16ms), lead=500ms forward from t=1000 -> ~31 frames; budget has
    // ample headroom -> plan covers the whole lead window.
    const GpuPrefetchPlan p = GpuSeekPrefetch::planPrefetch(
        /*targetMs*/ 1000, /*dir*/ 1, /*frameDurMs*/ 16, /*leadMs*/ 500,
        /*surfaceBytes*/ 3110400, /*budgetBytes*/ qint64(3110400) * 256, /*liveBytes*/ 0);
    QCOMPARE(p.startMs, int64_t(1000));
    QCOMPARE(p.endMs, int64_t(1500));
    QVERIFY(p.surfaceCount >= 30 && p.surfaceCount <= 32);
}

void TestGpuSeekPrefetch::prefetchBoundedByHeadroom() {
    // Only 4 surfaces of headroom left -> prefetch at most 4 even if the window
    // wants 31.
    const GpuPrefetchPlan p = GpuSeekPrefetch::planPrefetch(
        1000, 1, 16, 500, 3110400, qint64(3110400) * 100, qint64(3110400) * 96);
    QCOMPARE(p.surfaceCount, 4);
}

void TestGpuSeekPrefetch::noHeadroomNoPrefetch() {
    const GpuPrefetchPlan p = GpuSeekPrefetch::planPrefetch(
        1000, 1, 16, 500, 3110400, qint64(3110400) * 100, qint64(3110400) * 100);
    QCOMPARE(p.surfaceCount, 0);
    QCOMPARE(p.startMs, int64_t(-1));  // nothing to prefetch
}

QTEST_GUILESS_MAIN(TestGpuSeekPrefetch)
#include "tst_gpuseekprefetch.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:

```cmake
    olr_add_unit_test(tst_gpuseekprefetch olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpuseekprefetch
```
Expected: FAIL to compile — `playback/gpu/gpuseekprefetch.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpuseekprefetch.h` with the interface above, and `playback/gpu/gpuseekprefetch.cpp`:

```cpp
#include "playback/gpu/gpuseekprefetch.h"

#include <algorithm>

GpuPrefetchPlan GpuSeekPrefetch::planPrefetch(int64_t targetMs, int dir, int64_t frameDurMs,
                                              int64_t leadMs, qint64 surfaceBytes,
                                              qint64 budgetBytes, qint64 liveBytes) {
    GpuPrefetchPlan plan;
    if (frameDurMs <= 0 || surfaceBytes <= 0) return plan;

    // Headroom in surfaces: how many we can pre-warm without forcing an eviction.
    const qint64 headroomBytes = budgetBytes - liveBytes;
    const int headroomSurfaces = headroomBytes > 0 ? int(headroomBytes / surfaceBytes) : 0;
    if (headroomSurfaces <= 0) return plan;  // no prefetch: yield to the live window

    // Predicted window: [target, target+lead] forward, [target-lead, target] reverse.
    const int64_t windowFrames = (leadMs + frameDurMs - 1) / frameDurMs;  // ceil
    const int want = int(std::min<int64_t>(windowFrames, headroomSurfaces));
    if (want <= 0) return plan;

    if (dir >= 0) {
        plan.startMs = targetMs;
        plan.endMs = targetMs + leadMs;
    } else {
        plan.startMs = targetMs - leadMs;
        plan.endMs = targetMs;
    }
    plan.surfaceCount = want;
    return plan;
}
```

Wire CMake (all platforms — platform-neutral): add `gpuseekprefetch.{h,cpp}` to the `OpenLiveReplay` `if(OLR_GPU_PIPELINE)` `target_sources`, `olr_test_gpu`, and `olr_test_playback`'s GPU block.

In `playback/playbackworker.cpp`, consult the predictor at the reposition site (under `OLR_GPU_PIPELINE_BUILD` + `gpuPipelineEnabled()`): when `seekTo`/`repositionTo` is about to decode the target window, compute a `GpuPrefetchPlan` from `GpuBudget::instance()` and the live geometry, and bias the GPU decode of the predicted window (the existing `decodePacketIntoBank` loop already decodes that window — the prefetch plan only confirms there is GPU headroom to mint those frames as GPU-backed rather than degrading them; when `plan.surfaceCount == 0` the window still decodes, just CPU-backed). Keep this advisory: prefetch never blocks decode, never over-allocates past `headroomSurfaces`, and is a no-op on the CPU path.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpuseekprefetch && ctest --test-dir build/c -R tst_gpuseekprefetch --output-on-failure
```
Expected: PASS (3 tests; pure prediction, runs on any host).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpuseekprefetch.h playback/gpu/gpuseekprefetch.cpp \
        tests/unit/tst_gpuseekprefetch.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt \
        CMakeLists.txt playback/playbackworker.cpp
git commit -m "feat(gpu-budget): seek-prefetch decode-to-GPU on predicted reposition under headroom"
```

---

## Task 8: Multi-feed budget-pressure e2e — assert occupancy, bounded degrade, no crash

**Precondition:** Tasks 1-7. This is the spec §10 "VRAM blowup under multiview" gate: a forced-small GPU budget under multi-feed cap-pressure must (a) keep `liveBytes <= budgetBytes` (no blowup), (b) degrade to CPU under pressure WITHOUT crashing the decode loop, (c) report `gpuVramBytes > 0` and a bounded `gpuOomDegrades`, and (d) hold `placeholderFramesDelta==0` / `heldFramesDelta==0` (degrade does not gray-flash).

**Files:**
- Create: `tests/unit/tst_gpu_budget_stress.cpp` (multi-thread mint/evict under a tiny budget, asserting the live-bytes invariant + counted degrade)
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `tests/e2e/run_playback_e2e.sh` (a `gpubudget` scenario branch), `tests/e2e/play_harness.cpp` (drive multi-feed under a tiny forced budget), `tests/CMakeLists.txt` (register `e2e_play_gpubudget`)

**Interfaces:**
- Consumes: `GpuBudget`/`GpuBudgetConfig`/`GpuBudgetCharge`, `mintGpuOrDegrade`, the e2e counters already emitted (`gpuVramBytes`, `gpuOomDegrades`, `gpuReadToCpuCount`, `placeholderFramesDelta`, `heldFramesDelta`). No new product interface.

- [ ] **Step 1: Write the failing unit stress test**

Create `tests/unit/tst_gpu_budget_stress.cpp`:

```cpp
// Multi-feed budget-pressure stress (spec §10 VRAM-blowup row): under a tiny GPU
// budget, concurrent mints must never push liveBytes past budgetBytes, every
// over-budget mint must degrade to a counted CPU handle (never null), and the
// live bytes must return to 0 once all handles drop.
#include <QtTest>

#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpusurfaceallocator.h"
#include "playback/output/framehandle.h"

#include <atomic>
#include <thread>
#include <vector>

class TestGpuBudgetStress : public QObject {
    Q_OBJECT
private slots:
    void concurrentMintsNeverExceedBudgetAndNeverNull();
};

namespace {
CpuPlanes solid(int w, int h) {
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = w;
    p.height = h;
    p.stride[0] = w;
    p.stride[1] = (w + 1) / 2;
    p.stride[2] = (w + 1) / 2;
    p.plane[0] = QByteArray(w * h, 16);
    p.plane[1] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), 128);
    p.plane[2] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), 128);
    return p;
}
}  // namespace

void TestGpuBudgetStress::concurrentMintsNeverExceedBudgetAndNeverNull() {
    // Tiny budget: 4 surfaces. Many threads mint concurrently; the allocator must
    // degrade over-budget mints to CPU (never null) and never let liveBytes exceed
    // the budget. (Null surface forces the deny path -> deterministic on any host.)
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 4;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    std::atomic<bool> sawNull{false};
    std::atomic<bool> sawBlowup{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 256; ++i) {
                FrameMetadata meta;
                meta.key.format = FramePixelFormat::Nv12;
                meta.key.width = 64;
                meta.key.height = 48;
                // Null surface -> deny -> CPU degrade (proves never-null under load).
                // The render fence is null on the deny path; it never reaches the mint.
                GpuMintResult r =
                    mintGpuOrDegrade(nullptr, nullptr, meta, nullptr, [] { return solid(64, 48); });
                if (r.handle.isNull()) sawNull.store(true, std::memory_order_release);
                if (b.liveBytes() > b.budgetBytes())
                    sawBlowup.store(true, std::memory_order_release);
            }
        });
    }
    for (auto& t : ts) t.join();
    QVERIFY(!sawNull.load(std::memory_order_acquire));    // decode loop never gets null
    QVERIFY(!sawBlowup.load(std::memory_order_acquire));  // liveBytes never exceeds budget
    QVERIFY(b.oomDegradeCount() > 0);                     // degrades were counted
    QCOMPARE(b.liveBytes(), qint64(0));                   // all CPU handles -> no charge
}

QTEST_GUILESS_MAIN(TestGpuBudgetStress)
#include "tst_gpu_budget_stress.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:

```cmake
    olr_add_unit_test(tst_gpu_budget_stress olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL (then PASS after build)**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpu_budget_stress
```
Expected: FAIL (unregistered target) before the file exists; after adding + reconfigure, builds and PASSES on any host (the deny path is deterministic).

- [ ] **Step 3: Add the e2e multi-feed budget-pressure scenario**

In `tests/e2e/play_harness.cpp`, add a `gpubudget` scenario that, under `OLR_GPU_PIPELINE=1`, drives a ≥4-feed fixture with a FORCED tiny per-track frame budget (`OLR_GPU_FORCE_BUDGET` via `gpuForcedPerTrackBudget()`) so the GPU budget is exceeded and mints degrade to CPU under pressure. Reuse the harness's existing GPU counter emit (`gpuOomDegrades=%lld gpuVramBytes=%lld` at :163/:628) — the counters are already wired and parsed.

In `tests/e2e/run_playback_e2e.sh`, add a `gpubudget)` case (near `play1x)`) that:
- requires `OLR_GPU_PIPELINE=1` (else `SKIP` — CI macOS is CPU-oracle-only per §9);
- exports `OLR_GPU_FORCE_BUDGET=12` (the floor cap) and a ≥4-view fixture;
- asserts `placeholderFramesDelta` and `heldFramesDelta` are `0` (degrade does not gray-flash), `gpuVramBytes` is `> 0` and numeric (occupancy reported), `gpuOomDegrades` is numeric and bounded (e.g. `<= decodedVideoFrames` — every degrade is at most one per decoded frame), and `gpuReadToCpuCount > 0` (the degraded frames materialized to CPU).

Register the scenario as an `e2e_play` invocation in `tests/CMakeLists.txt` (mirror the existing `armedcut`/`gpucapstress` registration) so `ctest -R e2e_play_gpubudget` runs it; gate it to run only when `OLR_GPU_PIPELINE` is ON and a GPU host is present (the script self-skips otherwise).

- [ ] **Step 4: Run the e2e budget-pressure scenario (GPU host)**

```sh
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R e2e_play_gpubudget --output-on-failure
# or directly:
OLR_GPU_PIPELINE=1 OLR_GPU_FORCE_BUDGET=12 tests/e2e/run_playback_e2e.sh \
    build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness gpubudget 4 9311
```
Expected: PASS with `placeholderFramesDelta=0 heldFramesDelta=0 gpuVramBytes>0`, a bounded `gpuOomDegrades`, and no crash. On a non-GPU host the script prints a SKIP; on the CPU lane (`OLR_GPU_PIPELINE` unset) the scenario is skipped, never failed.

- [ ] **Step 5: Run the full unit + sanitizer pre-flight**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
# Reproduce the CI TSan lane for the budget charge/credit threading:
OLR_PREPUSH_FULL=1 .githooks/pre-push  # runs the ASan/TSan passes + e2e
# Off-path byte-green check (fresh build dir):
cmake -S . -B build/off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
cmake --build build/off && ctest --test-dir build/off -L unit --output-on-failure
```
Expected: full unit suite PASSES; no new TSan/ASan findings on the budget charge/credit; the GPU-off suite is byte-green.

- [ ] **Step 6: Commit + final review + push**

**Review gate (final, whole-subproject):** per CLAUDE.md, the playback worker's threading + every GPU-residency change gets an independent fresh-agent concurrency review before merge. The reviewer verifies: (a) the budget credit (in `~GpuBudgetCharge` / `~GpuFrameData`) never runs under `m_bufferMutex` across a fence-wait — credit is a lock-free atomic, never a fence-wait; (b) budget eviction still routes through the `gpu-sync` two-phase guard (collect locked, drain unlocked); (c) `mintGpuOrDegrade` never returns null to the decode loop on any branch; (d) the §2 peak-formula config multiplies the staging bank per feed and keeps the decode window aggregate (the multiview multiplier is present). Open the PR with the per-branch push:

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add tests/unit/tst_gpu_budget_stress.cpp tests/unit/CMakeLists.txt \
        tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/CMakeLists.txt
git commit -m "test(gpu-budget): multi-feed budget-pressure e2e (occupancy, bounded degrade, no crash)"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin gpu-phase4-gpu-budget
```

---

## Canonical budget contract (downstream plans consume these names verbatim)

`gpu-compositor` and `async-readback` (same Phase-4 wave) and `new-io-targets` (Phase 5) charge against the budget through these names. Do not rename or vary them.

**VRAM budget + occupancy** — `playback/gpu/gpubudget.h` (NEW):
```cpp
struct GpuBudgetConfig {  // the §2 peak-formula terms; surfaceBytes()/peakBudgetBytes()
    int feedCount; int aggregateDecodeWindow; int stagingWindowPerFeed;
    int activeBusCount; int readbackRingDepth; int width; int height;
    FramePixelFormat surfaceFormat;
};
class GpuBudget {                 // process-wide; instance()
    void configure(const GpuBudgetConfig&); qint64 budgetBytes() const; qint64 liveBytes() const;
    bool canAllocate(qint64 bytes) const; void charge(qint64); void credit(qint64);
    qint64 oomDegradeCount() const; void noteOomDegrade(); void reset();
};
class GpuBudgetCharge { ... };    // RAII charge/credit, move-only; rides on GpuFrameData
qint64 gpuSurfaceBytes(const GpuSurface&);
```

**OOM-safe mint chokepoint** — `playback/gpu/gpusurfaceallocator.h` (NEW):
```cpp
struct GpuMintResult { FrameHandle handle; bool degradedToCpu; };   // handle is NEVER null
GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence,   // gpu-sync timeline
                               const std::function<CpuPlanes()>& cpuFallback);
```

**Combined GPU mint factory** — `playback/gpu/gpuframedata.h` (EXTENDS the `gpu-sync` render-fence factory; NO competing 4-arg `(…, charge)` overload):
```cpp
// One combined 5-arg form carries BOTH the gpu-sync render fence AND the budget
// charge, so every budgeted GPU frame keeps the shared render/readback timeline.
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge charge);
```

**Seek-prefetch policy** — `playback/gpu/gpuseekprefetch.h` (NEW):
```cpp
struct GpuPrefetchPlan { int64_t startMs; int64_t endMs; int surfaceCount; };
class GpuSeekPrefetch {
    static GpuPrefetchPlan planPrefetch(int64_t targetMs, int dir, int64_t frameDurMs,
                                        int64_t leadMs, qint64 surfaceBytes,
                                        qint64 budgetBytes, qint64 liveBytes);
};
```

**Telemetry writer** — `playback/output/outputruntime.h` (NEW writer for existing placeholders):
```cpp
void OutputRuntime::recordGpuBudget(qint64 vramBytes, qint64 oomDegrades);  // -> stats().gpuVramBytes/.gpuOomDegrades
```
`gpuVramBytes` / `gpuOomDegrades` live on `OutputDispatchStats` (outputdispatcher.h:75,:79), emitted by `play_harness.cpp`, parsed by `run_playback_e2e.sh`. This subproject is the FIRST to write them (mirroring the `gpu-sync` `incrementFenceWaitStalls` pattern).
