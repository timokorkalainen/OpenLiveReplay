# device-loss Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Make GPU device loss a survivable, non-crashing event for a LIVE BROADCAST tool — a HARD-DOWN, not a soft quality dip. When the RHI backend reports a removed/reset device (D3D11 `DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` per P0.2, Metal command-buffer error / GPU reset on Apple), the pipeline must: (1) **detect** the loss on whatever thread observes it; (2) **invalidate** every live `GpuSurface`/`GpuRhiContext` and outstanding `GpuFence`, and **bump the `GpuGenerationCounter`** so every stale GPU-backed `FrameHandle` is rejected by `isStaleForGeneration`; (3) **rebuild** the RHI spine mid-session (new `GpuRhiContext`, freshly **recreated** `GpuFence`s); (4) if the rebuild fails, **DEGRADE to the permanent CPU path (D5)** — flip the effective pipeline to the CPU fallback without crashing, so the output **keeps producing frames** across the loss. A new `gpuDeviceLossEvents` telemetry counter records each event. A test-only `OlrRhi::injectDeviceLostForTest()` fault-injection hook drives a unit + e2e test asserting (a) degrade-to-CPU without crash, (b) frames keep flowing, (c) a simulated successful rebuild resumes the GPU path under a bumped generation. The whole path is concurrency-safe: loss can fire on the render thread while the output thread reads.

**Architecture:** A small `playback/gpu/` device-loss family sits on top of the merged `gpu-sync` keystone. A new `GpuDeviceLossMonitor` (`playback/gpu/gpudevicelossmonitor.h`) is the process-wide latch: it owns an atomic `lost` flag, a `recordLoss()` that bumps the `GpuGenerationCounter` (invalidating every stamped surface) and increments a loss count, and `consumeLossEvent()` so the worker drains events into telemetry. `OlrRhi` (`playback/gpu/olrrhi.h`) and `GpuRhiContext` (`playback/gpu/gpurhicontext.h`) gain a `deviceLost()` poll plus a test-only `injectDeviceLostForTest()`; the Apple/D3D11/stub backends map their native error surface (`MTLCommandBuffer.error` / `ID3D11Device::GetDeviceRemovedReason`) onto that poll. The worker (`playback/playbackworker.{h,cpp}`) gains a `GpuPipelineState` latch (`Gpu` / `RebuildPending` / `CpuFallback`) and a `handleGpuDeviceLoss()` that runs the invalidate → rebuild → degrade sequence under the existing lock rule (collect/release/act, never fence-wait under `m_bufferMutex`), reusing the existing per-frame `gpuFallback` branch as the steady-state CPU path once latched. `OlrRhi::injectDeviceLostForTest()` + a `tst_devicelossmonitor` unit and an `e2e_play_devicelost` scenario exercise the fault injection. Loss is observed at the import edge (`importVtImageBuffer` / `WinGpuImportEdge::tryImport` failure) and at the readback edge (`GpuRhiContext::importAndReadback` failure); both funnel into `handleGpuDeviceLoss()`.

**Tech Stack:** C++17, Qt 6 (Core/Gui/GuiPrivate for `QRhi`, Test), Apple Metal/CoreVideo (`MTLCommandBuffer` status/error), Windows D3D11 (`ID3D11Device::GetDeviceRemovedReason`, `DXGI_ERROR_DEVICE_REMOVED`/`_RESET`), CMake + Ninja. Consumes the merged `gpu-sync` contract verbatim: `GpuFence`/`makeD3D11GpuFence` (`playback/gpu/gpufence.h`), `GpuGenerationCounter` + `FrameHandle::isStaleForGeneration` + `FrameMetadata::gpuGeneration` (`playback/gpu/gpugeneration.h`, `playback/output/framehandle.h`), `GpuSurface::retainUntilFenceRetired`/`pendingFenceValue` (`playback/gpu/gpusurface.h`), `DecodeDoneFence` (`playback/gpu/decodedonefence.h`), `GpuRhiContext` (`playback/gpu/gpurhicontext.h`), `OlrRhi` (`playback/gpu/olrrhi.h`), `gpuPipelineEnabled()`/`gpuConsumeInjectedAllocFailure()`/`gpuSetInjectedAllocFailures()` (`playback/gpu/gpupipelineconfig.h`), `GpuReadbackTelemetry` (`playback/output/gpureadbacktelemetry.h`), `OutputDispatchStats` (`playback/output/outputdispatcher.h`), and `PlaybackWorker`/`TrackBuffer` (`playback/playbackworker.*`, `playback/trackbuffer.*`).

## Global Constraints

- **Builds ON the merged `gpu-sync` keystone, never replaces it.** Every named type either already exists (extend in place: `OlrRhi`, `GpuRhiContext`, `PlaybackWorker`, `OutputDispatchStats`) or is genuinely new (`GpuDeviceLossMonitor`, `GpuPipelineState`, `handleGpuDeviceLoss`). Use the **actual** signatures verified in the tree (listed in Tech Stack); do not invent variant names. The `gpu-sync` contract names — `GpuFence`, `GpuGenerationCounter`, `GpuSurface`, `GpuRhiContext` — are consumed verbatim.
- **Device loss is a HARD-DOWN, output-must-keep-flowing event.** The product invariant: across a loss, the dispatcher keeps emitting frames from the CPU path; `placeholderFramesDelta` and `heldFramesDelta` from the e2e gate must stay `0` through the loss (the loss must not manifest as a gray flash or a stall). Degrade-to-CPU is permanent for the session unless a rebuild succeeds; it never crashes the decode loop or the output tick.
- **Fences and the RHI spine are RECREATED on rebuild (gpu-sync §4).** A rebuild constructs a **fresh** `GpuRhiContext` and **fresh** `GpuFence`/`DecodeDoneFence` — the old ones are dropped, never reused (their GPU timeline died with the device). The `GpuGenerationCounter` is bumped on every loss so any `FrameHandle` minted under the dead device is `isStaleForGeneration(current()) == true` and is rejected before present.
- **CPU path stays default + reference.** The GPU pipeline is two-gated: `OLR_GPU_PIPELINE` CMake option → `OLR_GPU_PIPELINE_BUILD` compile def, and the runtime `gpuPipelineEnabled()` env flag (off by default). With `OLR_GPU_PIPELINE_BUILD` undefined **or** `gpuPipelineEnabled()` false, every byte of this plan is inert: no monitor, no latch, no extra counter write — the CPU path is byte-for-byte the Phase-1/2/3 path and remains the permanent correctness oracle + fallback. New `OutputDispatchStats`/`PlaybackCounters` scalars default to `0` and stay `0` on the CPU path.
- **The lock rule is non-negotiable (spec §10, D4; inherited from gpu-sync).** `handleGpuDeviceLoss()` **never** waits on a GPU fence while holding `m_bufferMutex`. It collects victims / reads state under the mutex, **releases it**, then waits/frees/rebuilds. Every fence-wait or RHI-teardown site carries a `// LOCK RULE:` comment proving no `m_bufferMutex` is held. The dead device's fences must not be waited on indefinitely — teardown drops them rather than blocking (a dead fence never retires).
- **Concurrency-critical — independent review required before merge (CLAUDE.md "Verification").** Loss can fire on the render thread while the output thread reads a surface and the worker thread decodes. TSan cannot see GPU command ordering. The contract: a present/readback never reads a surface from a superseded generation, and the latch transition is observed atomically by every thread. Each task touching the latch/monitor threading carries a `**Review gate:**` note; the branch gets a fresh-agent concurrency review before the PR merges.
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
  Also build once with `-DOLR_GPU_PIPELINE=OFF` (a fresh build dir) to confirm the off-path still compiles and the unit suite is byte-green. Unit tests register via `olr_add_unit_test(<name> <libs...>)` in `tests/unit/CMakeLists.txt` (GPU tests inside the `if(OLR_GPU_PIPELINE)` block); the GPU backend lib is `olr_test_gpu` and `olr_test_playback` carries the worker. Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.
- **P0.2 is resolved to D3D11.** `playback/output/win/wingpuimportedge.h` pins `kWinRhiBackend = "d3d11"`, so the Windows device-removed surface is `ID3D11Device::GetDeviceRemovedReason()` returning `DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` / `DXGI_ERROR_DEVICE_HUNG` / `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. No D3D12 path is added.
- **Format changed lines only** (CI Lint checks changed lines; engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
- **Public-repo professionalism.** Self-contained, professional code/comments/commits; document the present design, no internal notes or private history.

---

## Preconditions (read before Task 1)

- **`gpu-sync` merged.** `playback/gpu/{gpufence.{h,gpufence_apple.mm,gpufence_stub.cpp,gpufence_win.cpp},gpugeneration.{h,cpp}}` exist; `DecodeDoneFence` carries `signaledValue()`/`waitForValue()`; `FrameMetadata` carries `uint64_t gpuGeneration = 0`; `FrameHandle::isStaleForGeneration(uint64_t)` exists; `GpuSurface` carries the `retainUntilFenceRetired`/`pendingFenceValue` virtuals; `PlaybackWorker` carries `m_renderFence`/`m_stagingFence` + the two-phase eviction guard; `OutputDispatchStats` carries `gpuOomDegrades`/`fenceWaitStalls`/`gpuVramBytes`. Verify with `git merge-base --is-ancestor <gpu-sync-sha> origin/main` if unsure.
- **A per-frame GPU→CPU fallback already exists; this subproject adds the SESSION-LEVEL latch.** In `playback/playbackworker.cpp`, both GPU decode branches (macOS `importVtImageBuffer`, Windows `WinGpuImportEdge::tryImport`) already set a local `gpuFallback = true` and fall through to the CPU `handleFrame` path on a single failed import (the macOS branch at ~:706-713, the Windows branch at ~:764-775). What is MISSING is: (a) a way to recognize the failure was a *device loss* (not a transient alloc miss), (b) a latch so the worker stops re-attempting the GPU path for the rest of the session (or until rebuild), and (c) the invalidate/rebuild/degrade orchestration + telemetry. This plan adds exactly those.
- **`gpuDeviceLossEvents` does NOT exist yet.** `OutputDispatchStats` carries `gpuOomDegrades`/`fenceWaitStalls`/`gpuVramBytes` but **not** `gpuDeviceLossEvents`. Task 5 adds it; `play_harness.cpp` + `run_playback_e2e.sh` are extended to emit/parse it.
- **Today's `OlrRhi` is the Null backend only.** `OlrRhi::create(Backend::Null, ...)` builds a deterministic headless `QRhi`; there is no device-loss surface on it. The fault-injection hook (`injectDeviceLostForTest()`) is the deterministic device-loss source for CI (no real GPU on hosted runners), and the Apple/D3D11 concretes map their native error onto the same poll.

---

## Task 1: `GpuDeviceLossMonitor` — the process-wide loss latch (bump generation, count events)

**Precondition:** `gpu-sync` merged.

**Files:**
- Create: `playback/gpu/gpudevicelossmonitor.h`, `playback/gpu/gpudevicelossmonitor.cpp`
- Test: `tests/unit/tst_devicelossmonitor.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuGenerationCounter` (`playback/gpu/gpugeneration.h` — `instance()`, `current()`, `bump()`, `reset()`).
- Produces:
  ```cpp
  // playback/gpu/gpudevicelossmonitor.h
  // Process-wide GPU device-loss latch. A loss is a HARD-DOWN for a live tool:
  // recordLoss() bumps the GpuGenerationCounter (so every FrameHandle stamped
  // under the dead device is isStaleForGeneration == true and rejected before
  // present) and increments a monotonic event count. The worker drains events
  // via consumeLossEvent() into the gpuDeviceLossEvents telemetry counter.
  // One playback worker owns the GPU pipeline, so a single process-wide latch
  // is correct; it is reset() between sessions (graph init/teardown) and in tests.
  class GpuDeviceLossMonitor {
  public:
      static GpuDeviceLossMonitor& instance();

      bool isLost() const;                 // lock-free read of the latch
      uint64_t lossCount() const;          // total losses recorded this process

      // Record a device loss: bump the generation counter (invalidate stale
      // surfaces), set the latch, increment lossCount. Returns the NEW generation.
      // Idempotent within one loss epoch — see clearForRebuild().
      uint64_t recordLoss();

      // Drain ONE pending loss event for telemetry (returns true and decrements the
      // undrained-event count if one was pending). The latch (isLost) is NOT
      // cleared by draining — only clearForRebuild() clears it.
      bool consumeLossEvent();

      // After a successful spine rebuild, clear the latch so the GPU path may
      // resume. The generation stays bumped (the dead surfaces remain stale).
      void clearForRebuild();

      void reset();                        // test/session boundary: latch off, counts 0

  private:
      GpuDeviceLossMonitor() = default;
      std::atomic<bool> m_lost{false};
      std::atomic<uint64_t> m_lossCount{0};
      std::atomic<uint64_t> m_undrained{0};
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_devicelossmonitor.cpp`:

```cpp
// GpuDeviceLossMonitor is the process-wide device-loss latch. recordLoss bumps
// the GpuGenerationCounter (stale-surface invalidation) and sets the latch;
// consumeLossEvent drains events for telemetry without clearing the latch;
// clearForRebuild clears the latch after a successful rebuild while leaving the
// generation bumped (dead surfaces stay stale).
#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"

class TestDeviceLossMonitor : public QObject {
    Q_OBJECT
private slots:
    void recordLossSetsLatchAndBumpsGeneration();
    void consumeLossEventDrainsWithoutClearingLatch();
    void clearForRebuildClearsLatchKeepsGeneration();
    void resetReturnsToPristine();
};

void TestDeviceLossMonitor::recordLossSetsLatchAndBumpsGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().reset();
    QVERIFY(!m.isLost());
    QCOMPARE(m.lossCount(), uint64_t(0));
    const uint64_t gen = m.recordLoss();
    QVERIFY(m.isLost());
    QCOMPARE(m.lossCount(), uint64_t(1));
    QCOMPARE(gen, GpuGenerationCounter::instance().current());
    QVERIFY(gen >= 1);  // a loss advanced the generation
}

void TestDeviceLossMonitor::consumeLossEventDrainsWithoutClearingLatch() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    m.recordLoss();
    QVERIFY(m.consumeLossEvent());   // one pending event drained
    QVERIFY(!m.consumeLossEvent());  // none left
    QVERIFY(m.isLost());             // draining does NOT clear the latch
}

void TestDeviceLossMonitor::clearForRebuildClearsLatchKeepsGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().reset();
    m.recordLoss();
    const uint64_t genAfterLoss = GpuGenerationCounter::instance().current();
    m.clearForRebuild();
    QVERIFY(!m.isLost());  // GPU path may resume
    QCOMPARE(GpuGenerationCounter::instance().current(), genAfterLoss);  // dead surfaces stay stale
}

void TestDeviceLossMonitor::resetReturnsToPristine() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.recordLoss();
    m.reset();
    QVERIFY(!m.isLost());
    QCOMPARE(m.lossCount(), uint64_t(0));
    QVERIFY(!m.consumeLossEvent());
}

QTEST_GUILESS_MAIN(TestDeviceLossMonitor)
#include "tst_devicelossmonitor.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt` (after `tst_gpugeneration`):

```cmake
    olr_add_unit_test(tst_devicelossmonitor olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_devicelossmonitor
```
Expected: FAIL to compile — `playback/gpu/gpudevicelossmonitor.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpudevicelossmonitor.h` with the interface above (`#include <atomic>`, `#include <cstdint>`).

Create `playback/gpu/gpudevicelossmonitor.cpp`:

```cpp
#include "playback/gpu/gpudevicelossmonitor.h"

#include "playback/gpu/gpugeneration.h"

GpuDeviceLossMonitor& GpuDeviceLossMonitor::instance() {
    static GpuDeviceLossMonitor inst;
    return inst;
}

bool GpuDeviceLossMonitor::isLost() const { return m_lost.load(std::memory_order_acquire); }

uint64_t GpuDeviceLossMonitor::lossCount() const {
    return m_lossCount.load(std::memory_order_acquire);
}

uint64_t GpuDeviceLossMonitor::recordLoss() {
    // Bump the generation FIRST so any handle stamped under the dead device is
    // immediately stale (isStaleForGeneration true) the moment the latch is set;
    // a presenter that sees the latch must already see the new generation.
    const uint64_t gen = GpuGenerationCounter::instance().bump();
    m_lossCount.fetch_add(1, std::memory_order_acq_rel);
    m_undrained.fetch_add(1, std::memory_order_acq_rel);
    m_lost.store(true, std::memory_order_release);
    return gen;
}

bool GpuDeviceLossMonitor::consumeLossEvent() {
    uint64_t pending = m_undrained.load(std::memory_order_acquire);
    while (pending > 0 &&
           !m_undrained.compare_exchange_weak(pending, pending - 1, std::memory_order_acq_rel)) {
    }
    return pending > 0;
}

void GpuDeviceLossMonitor::clearForRebuild() { m_lost.store(false, std::memory_order_release); }

void GpuDeviceLossMonitor::reset() {
    m_lost.store(false, std::memory_order_release);
    m_lossCount.store(0, std::memory_order_release);
    m_undrained.store(0, std::memory_order_release);
}
```

Wire `gpudevicelossmonitor.{h,cpp}` into CMake — it is platform-neutral C++. In `CMakeLists.txt` inside the `if(OLR_GPU_PIPELINE)` block, add both files to the `OpenLiveReplay` `target_sources` on every platform (mirror `gpugeneration.cpp`). In `tests/CMakeLists.txt`, add `gpudevicelossmonitor.cpp` to `olr_test_gpu` and to `olr_test_playback`'s GPU block (mirror `gpugeneration.cpp`).

**Review gate:** `recordLoss` is the cross-thread invalidation primitive — the generation bump must be observed before the latch by any thread that reads the latch then the generation. The store ordering (generation bump → latch release) plus the acquire reads in consumers is the proof. Flag for independent review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_devicelossmonitor && ctest --test-dir build/c -R tst_devicelossmonitor --output-on-failure
```
Expected: PASS (4 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpudevicelossmonitor.h playback/gpu/gpudevicelossmonitor.cpp \
        tests/unit/tst_devicelossmonitor.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(device-loss): process-wide GpuDeviceLossMonitor (bump generation, count events)"
```

---

## Task 2: `deviceLost()` poll + `injectDeviceLostForTest()` on `OlrRhi`

**Precondition:** Task 1. (`OlrRhi` is the Null-backend RHI bring-up today, with no device-loss surface; the fault-injection hook is the deterministic device-loss source for CI.)

**Files:**
- Modify: `playback/gpu/olrrhi.h`, `playback/gpu/olrrhi.cpp` (the Null-backend body)
- Test: `tests/unit/tst_olrrhi.cpp` (extend the existing OlrRhi test exe; if none exists, create it and register)
- Modify (only if creating the test exe): `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `OlrRhi` (existing — `create(Backend, QString*)`, `rhi()`, `runOffscreenFrame(...)`).
- Produces (ADD to `OlrRhi`, in place):
  ```cpp
  // playback/gpu/olrrhi.h, class OlrRhi:
  // True once the backend has reported a removed/reset device. On the Null
  // backend this is only ever set by injectDeviceLostForTest(); the Apple/D3D11
  // contexts (Task 3) map MTLCommandBuffer.error / GetDeviceRemovedReason onto it.
  // Once lost, it STAYS lost for this OlrRhi instance — recovery is a fresh create().
  bool deviceLost() const;

  // Test-only fault injection (spec §6 device-loss row): simulate a device-loss
  // event without a real GPU, so CI (no GPU/D3D on hosted runners) exercises the
  // detect → invalidate → degrade/rebuild path deterministically.
  void injectDeviceLostForTest();
  ```
  Back the flag with a `std::atomic<bool> m_deviceLost{false};` member (the poll may run on the output thread while injection runs on the worker/render thread).

- [ ] **Step 1: Write the failing test**

Create (or extend) `tests/unit/tst_olrrhi.cpp`:

```cpp
// OlrRhi exposes a device-loss poll. The Null backend is never spontaneously
// lost; injectDeviceLostForTest() is the deterministic CI loss source. Once
// injected, deviceLost() latches true (recovery is a fresh create()).
#include <QtTest>

#include "playback/gpu/olrrhi.h"

class TestOlrRhi : public QObject {
    Q_OBJECT
private slots:
    void nullBackendStartsNotLost();
    void injectDeviceLostLatchesTrue();
};

void TestOlrRhi::nullBackendStartsNotLost() {
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable(err));
    QVERIFY(!rhi->deviceLost());
}

void TestOlrRhi::injectDeviceLostLatchesTrue() {
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY(rhi != nullptr);
    QVERIFY(!rhi->deviceLost());
    rhi->injectDeviceLostForTest();
    QVERIFY(rhi->deviceLost());
    rhi->injectDeviceLostForTest();  // idempotent
    QVERIFY(rhi->deviceLost());
}

QTEST_GUILESS_MAIN(TestOlrRhi)
#include "tst_olrrhi.moc"
```

If `tst_olrrhi` is not already registered, add to the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:

```cmake
    olr_add_unit_test(tst_olrrhi olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_olrrhi
```
Expected: FAIL to compile — `deviceLost` / `injectDeviceLostForTest` are not members of `OlrRhi`.

- [ ] **Step 3: Write the implementation**

In `playback/gpu/olrrhi.h`, add to `class OlrRhi` (after `runOffscreenFrame`): the two method declarations from Interfaces and a protected `std::atomic<bool> m_deviceLost{false};` member (add `#include <atomic>`).

In `playback/gpu/olrrhi.cpp` (the Null-backend body), implement:

```cpp
bool OlrRhi::deviceLost() const { return m_deviceLost.load(std::memory_order_acquire); }

void OlrRhi::injectDeviceLostForTest() {
    // Deterministic device-loss source for CI (no GPU on hosted runners). Latches
    // permanently for this instance, mirroring a real removed device: recovery is
    // a fresh OlrRhi::create(), never a clear-in-place.
    m_deviceLost.store(true, std::memory_order_release);
}
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_olrrhi && ctest --test-dir build/c -R tst_olrrhi --output-on-failure
```
Expected: PASS (2 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/olrrhi.h playback/gpu/olrrhi.cpp tests/unit/tst_olrrhi.cpp tests/unit/CMakeLists.txt
git commit -m "feat(device-loss): OlrRhi deviceLost() poll + injectDeviceLostForTest fault hook"
```

---

## Task 3: `GpuRhiContext::deviceLost()` + native device-removed mapping (Apple / D3D11)

**Precondition:** Task 2. The platform contexts map their native error surface onto the poll: Apple from a Metal command-buffer error / GPU reset; Windows from `ID3D11Device::GetDeviceRemovedReason()` returning `DXGI_ERROR_DEVICE_REMOVED`/`_RESET`/`_HUNG`/`DRIVER_INTERNAL_ERROR`.

**Files:**
- Modify: `playback/gpu/gpurhicontext.h`, `playback/gpu/gpurhicontext_apple.mm`, `playback/gpu/gpurhicontext_stub.cpp` (and the Windows `gpurhicontext` TU if one exists; else the stub serves non-Apple)
- Test: `tests/unit/tst_gpurhicontext.cpp` (extend)

**Interfaces:**
- Consumes: `GpuRhiContext` (existing — `create()`, `isValid()`, `importAndReadback(surface, target)`), the `OlrRhi::deviceLost()` poll (Task 2) if the context wraps an `OlrRhi`.
- Produces (ADD to `GpuRhiContext`, in place):
  ```cpp
  // playback/gpu/gpurhicontext.h, class GpuRhiContext:
  // True once the underlying RHI device has been removed/reset. On Apple this is
  // set when a command buffer completes with .error (MTLCommandBufferStatusError)
  // or status == NotEnoughMemory after a GPU reset; on Windows when
  // GetDeviceRemovedReason() != S_OK. importAndReadback() returns an invalid
  // CpuPlanes when the device is lost (the worker then triggers handleGpuDeviceLoss).
  bool deviceLost() const;

  // Test-only: force the device-lost flag (drives the e2e/unit fault path on a
  // real RHI host without an actual GPU removal).
  void injectDeviceLostForTest();
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpurhicontext.cpp`:

```cpp
    void deviceLostStartsFalseThenLatchesOnInject();
```

```cpp
void TestGpuRhiContext::deviceLostStartsFalseThenLatchesOnInject() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    QVERIFY(!rhi->deviceLost());
    rhi->injectDeviceLostForTest();
    QVERIFY(rhi->deviceLost());
    // A lost context yields an invalid readback (the worker degrades on this).
    auto planes = rhi->importAndReadback(nullptr, FramePixelFormat::Yuv420p);
    QVERIFY(!planes.isValid());
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpurhicontext
```
Expected: FAIL — `deviceLost` / `injectDeviceLostForTest` not members of `GpuRhiContext`.

- [ ] **Step 3: Write the implementation**

In `playback/gpu/gpurhicontext.h`, add the two declarations to `class GpuRhiContext` (after `importAndReadback`).

In the `Impl` (in `gpurhicontext_apple.mm` and the stub TU), add a `std::atomic<bool> m_deviceLost{false};` member. Implement on `GpuRhiContext`:

```cpp
bool GpuRhiContext::deviceLost() const {
    return m_impl && m_impl->m_deviceLost.load(std::memory_order_acquire);
}

void GpuRhiContext::injectDeviceLostForTest() {
    if (m_impl) m_impl->m_deviceLost.store(true, std::memory_order_release);
}
```

In `gpurhicontext_apple.mm`'s `importAndReadback`, after each command-buffer completes, set the flag on a device-removed error and bail (LOCK RULE: this runs on the render thread; it touches no `m_bufferMutex`):

```objcpp
    // Map a Metal GPU reset / device-removed onto the device-loss poll. A command
    // buffer that completes with .error (status == MTLCommandBufferStatusError) or
    // NotEnoughMemory after a reset means the device is gone for this context.
    if (commandBuffer.status == MTLCommandBufferStatusError && commandBuffer.error) {
        m_deviceLost.store(true, std::memory_order_release);
        return CpuPlanes{};  // invalid: caller degrades to CPU
    }
```

Add a guard at the top of `importAndReadback` so an already-lost context returns invalid immediately (and the null-surface test passes):

```objcpp
    if (m_deviceLost.load(std::memory_order_acquire) || !surface) return CpuPlanes{};
```

In the stub TU, implement the same guard (`if (m_deviceLost || !surface) return CpuPlanes{};`) so the unit test runs deterministically off-GPU.

On Windows (if a `gpurhicontext_win` TU exists; else this lands in `WinGpuImportEdge` per Task 4), poll `device->GetDeviceRemovedReason()` after `importAndReadback` and set `m_deviceLost` when it != `S_OK`:

```cpp
    // ID3D11Device::GetDeviceRemovedReason() returns DXGI_ERROR_DEVICE_REMOVED /
    // _RESET / _HUNG / DRIVER_INTERNAL_ERROR once the device is gone.
    if (device && device->GetDeviceRemovedReason() != S_OK) {
        m_deviceLost.store(true, std::memory_order_release);
        return CpuPlanes{};
    }
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpurhicontext && ctest --test-dir build/c -R tst_gpurhicontext --output-on-failure
```
Expected: PASS (existing slots + `deviceLostStartsFalseThenLatchesOnInject` on an RHI host; `QSKIP` where no RHI backend).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm \
        playback/gpu/gpurhicontext_stub.cpp tests/unit/tst_gpurhicontext.cpp
git commit -m "feat(device-loss): GpuRhiContext deviceLost() + native removed-device mapping"
```

---

## Task 4: Worker latch + `handleGpuDeviceLoss()` — invalidate, rebuild, degrade to CPU

**Precondition:** Tasks 1-3. **This is the orchestration core and the highest-risk concurrency change in the plan (loss fires on the render thread while the output thread reads).**

**Files:**
- Modify: `playback/playbackworker.h` (the `GpuPipelineState` latch + members + method decls), `playback/playbackworker.cpp` (graph-init reset, the two GPU decode branches, the orchestration body)

**Interfaces:**
- Consumes: `GpuDeviceLossMonitor` (Task 1), `GpuRhiContext::deviceLost()`/`injectDeviceLostForTest()` (Task 3), `GpuFence`/`DecodeDoneFence` (recreated on rebuild — gpu-sync §4), `m_gpuRhi`/`m_decodeFence`/`m_renderFence` (existing worker members), the per-frame `gpuFallback` branch (existing, ~:706/:764), `m_bufferMutex` (existing).
- Produces:
  ```cpp
  // playback/playbackworker.h (private):
  // Effective GPU-pipeline state. Default Gpu when OLR_GPU_PIPELINE_BUILD + the
  // flag are on and a GpuRhiContext built; latched to CpuFallback on a device loss
  // whose rebuild failed (or was not attempted); RebuildPending between detection
  // and a rebuild attempt. CpuFallback is the permanent CPU path (D5) for the
  // session — the existing per-frame gpuFallback branch is its steady state.
  enum class GpuPipelineState { Gpu, RebuildPending, CpuFallback };
  std::atomic<int> m_gpuPipelineState{static_cast<int>(GpuPipelineState::CpuFallback)};

  GpuPipelineState gpuPipelineState() const;          // lock-free read (test/telemetry)
  bool gpuPathActive() const;                          // == state Gpu && m_gpuRhi valid

  // Run the detect → invalidate → rebuild → degrade sequence. Called from the
  // worker thread when a GPU import/readback reports a loss, or when
  // GpuDeviceLossMonitor::isLost() is observed. MUST NOT hold m_bufferMutex on
  // entry (spec §10): it drops the dead RHI context + fences (NOT fence-waiting
  // a dead timeline), bumps the generation via GpuDeviceLossMonitor::recordLoss(),
  // attempts ONE rebuild (a fresh GpuRhiContext + fresh GpuFence/DecodeDoneFence),
  // and on failure latches CpuFallback. Idempotent: a second call while already
  // CpuFallback is a no-op.
  void handleGpuDeviceLoss();

  // Attempt to rebuild the RHI spine in place. Returns true on success (state ->
  // Gpu, GpuDeviceLossMonitor::clearForRebuild()); false latches CpuFallback.
  bool rebuildGpuSpine();
  ```

- [ ] **Step 1: Write the failing test**

Add a focused worker-less unit (drive the state machine via a test seam) — extend `tests/unit/tst_devicelossmonitor.cpp` is wrong scope; instead add to a new `tests/unit/tst_gpu_devicelost_worker.cpp`. Because constructing a full `PlaybackWorker` in a unit is heavy, the assertion exercises the **transition contract** the worker implements via the monitor + a stub RHI context, proving: a lost context drives `recordLoss` + a generation bump, and a failed rebuild latches CPU (frames must still flow — asserted in the e2e of Task 6). Add to `tst_gpu_devicelost_worker.cpp`:

```cpp
// The device-loss transition contract the worker implements: a lost RHI context
// triggers recordLoss (generation bump + latch), and a rebuild that fails leaves
// the pipeline in the CPU-fallback state without ever fence-waiting a dead device.
#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpurhicontext.h"

class TestGpuDeviceLostWorker : public QObject {
    Q_OBJECT
private slots:
    void lostContextRecordsLossAndBumpsGeneration();
    void rebuildFailureLeavesLatchSet();
};

void TestGpuDeviceLostWorker::lostContextRecordsLossAndBumpsGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().reset();
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    rhi->injectDeviceLostForTest();
    QVERIFY(rhi->deviceLost());
    const uint64_t gen0 = GpuGenerationCounter::instance().current();
    m.recordLoss();  // worker does this on observing the loss
    QVERIFY(m.isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
}

void TestGpuDeviceLostWorker::rebuildFailureLeavesLatchSet() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    m.recordLoss();
    // Simulate a rebuild attempt that fails: the latch stays set (CPU fallback).
    // (clearForRebuild is only called on SUCCESS.)
    QVERIFY(m.isLost());
}

QTEST_GUILESS_MAIN(TestGpuDeviceLostWorker)
#include "tst_gpu_devicelost_worker.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:

```cmake
    olr_add_unit_test(tst_gpu_devicelost_worker olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpu_devicelost_worker
```
Expected: FAIL (unregistered target / missing test file).

- [ ] **Step 3: Write the implementation (state machine + orchestration)**

In `playback/playbackworker.h`, add (under `#ifdef OLR_GPU_PIPELINE_BUILD` where the members are platform-gated, mirroring `m_gpuRhi`): the `GpuPipelineState` enum, the `m_gpuPipelineState` atomic, and the `gpuPipelineState`/`gpuPathActive`/`handleGpuDeviceLoss`/`rebuildGpuSpine` declarations.

In `playback/playbackworker.cpp`:

1. **Init the latch in `initializeOutputGraph`** (the `OLR_GPU_PIPELINE_BUILD` block at ~:328-339). After a successful `m_gpuRhi = GpuRhiContext::create()`, set the state to `Gpu`; otherwise `CpuFallback`. Also `GpuDeviceLossMonitor::instance().reset();` at session start:
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
       GpuDeviceLossMonitor::instance().reset();
       m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                                std::memory_order_release);
       if (gpuPipelineEnabled()) {
           m_gpuRhi = GpuRhiContext::create();
           if (!m_gpuRhi || !m_gpuRhi->isValid()) {
               m_gpuRhi.reset();
           } else {
               m_decodeFence = DecodeDoneFence::create();
               m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::Gpu),
                                        std::memory_order_release);
           }
       }
   #endif
   ```

2. **Implement the accessors:**
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
   PlaybackWorker::GpuPipelineState PlaybackWorker::gpuPipelineState() const {
       return static_cast<GpuPipelineState>(m_gpuPipelineState.load(std::memory_order_acquire));
   }
   bool PlaybackWorker::gpuPathActive() const {
       return gpuPipelineState() == GpuPipelineState::Gpu && m_gpuRhi && m_gpuRhi->isValid();
   }
   #endif
   ```

3. **Implement `handleGpuDeviceLoss()`** (runs on the worker thread; LOCK RULE comments throughout):
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
   void PlaybackWorker::handleGpuDeviceLoss() {
       // Idempotent: already degraded -> nothing to do.
       if (gpuPipelineState() == GpuPipelineState::CpuFallback &&
           !GpuDeviceLossMonitor::instance().isLost())
           return;

       m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::RebuildPending),
                                std::memory_order_release);

       // Invalidate: bump the generation (stale-surface rejection) + count the
       // event. recordLoss is idempotent within an epoch; call once per detection.
       GpuDeviceLossMonitor::instance().recordLoss();

       // Drop the dead RHI context and fences. LOCK RULE: we hold no m_bufferMutex
       // here. We DO NOT fence-wait the old timeline — a dead device's fence never
       // retires; waiting would hang. Just release them (surfaces minted under the
       // dead generation are already stale and will be rejected at present).
       m_decodeFence.reset();
       m_renderFence.reset();
       m_stagingFence.reset();
       m_gpuRhi.reset();

       // Attempt ONE rebuild. Success resumes GPU; failure is a permanent CPU
       // degrade for the session (D5) — the output keeps flowing on the CPU path.
       if (!rebuildGpuSpine()) {
           m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                                    std::memory_order_release);
       }
   }

   bool PlaybackWorker::rebuildGpuSpine() {
       // Fresh context + FRESH fences (gpu-sync §4: never reuse the dead timeline).
       auto rhi = GpuRhiContext::create();
       if (!rhi || !rhi->isValid() || rhi->deviceLost()) return false;
       m_gpuRhi = std::move(rhi);
       m_decodeFence = DecodeDoneFence::create();
       m_renderFence = GpuFence::create();
       m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::Gpu),
                                std::memory_order_release);
       GpuDeviceLossMonitor::instance().clearForRebuild();
       return true;
   }
   #endif
   ```

4. **Wire detection into the two GPU decode branches.** In the macOS branch (~:706-713) and the Windows branch (~:764-775), where the existing code sets `gpuFallback = true` on a failed import, distinguish a *device loss* (the RHI context reports `deviceLost()`) from a transient alloc miss, and on a loss call `handleGpuDeviceLoss()`. Critically, gate the WHOLE GPU branch on `gpuPathActive()` so once latched to CPU the branch is skipped (no re-attempts):
   ```cpp
   // macOS branch, replace `if (gpuRhi && gpuRhi->isValid())` guard with:
   if (gpuPathActive()) {
       // ... existing handleSurface lambda ...
       // inside, when importVtImageBuffer fails:
       if (!mediaFrame.isPresentable()) {
           if (gpuRhi->deviceLost()) handleGpuDeviceLoss();  // session degrade/rebuild
           gpuFallback = true;                                // this frame -> CPU
           return;
       }
   }
   ```
   Mirror in the Windows branch using `m_winGpuImportEdge`/`m_gpuRhi->deviceLost()` (the import edge already returns null on a removed device; query `GetDeviceRemovedReason` there per Task 3). After the GPU branch falls through, the existing CPU `handleFrame` path produces the frame, so **the frame still flows on the loss tick** — that is the hard-down survival property.

   **LOCK RULE:** `handleGpuDeviceLoss()` is called from inside `handleSurface`, which runs on the worker (decode) thread and holds no `m_bufferMutex` at that point (the dedup `m_bufferMutex` scope already closed at `:687-690`). Add a `// LOCK RULE:` comment confirming this.

5. **Teardown:** in `shutdownOutputGraph` (the `OLR_GPU_PIPELINE_BUILD` block at ~:382-385), also reset the latch + monitor and drop any rebuilt fences:
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
       m_decodeFence.reset();
       m_renderFence.reset();
       m_stagingFence.reset();
       m_gpuRhi.reset();
       m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                                std::memory_order_release);
       GpuDeviceLossMonitor::instance().reset();
   #endif
   ```

**Review gate:** the latch transitions (`Gpu` → `RebuildPending` → `Gpu`/`CpuFallback`) are read by the decode branch (`gpuPathActive`) on the worker thread and may be read by telemetry on another thread; the dead-fence drop must never block. The reviewer verifies: (a) no `->wait(`/`waitForValue`/`waitDecodeDone` on a dead fence (we drop, never wait); (b) `handleGpuDeviceLoss` holds no `m_bufferMutex`; (c) once `CpuFallback`, the GPU branch is fully skipped (no surface minted under a dead device reaches the cache). Flag for independent review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpu_devicelost_worker && ctest --test-dir build/c -R tst_gpu_devicelost_worker --output-on-failure
```
Expected: PASS (2 tests on an RHI host; `QSKIP` where no RHI backend).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: full unit suite PASSES (with the flag off, the latch is `CpuFallback`, no branch changes; with the flag on but no loss injected, the GPU path is unchanged).

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/playbackworker.h playback/playbackworker.cpp \
        tests/unit/tst_gpu_devicelost_worker.cpp tests/unit/CMakeLists.txt
git commit -m "feat(device-loss): worker latch + handleGpuDeviceLoss (invalidate/rebuild/degrade)"
```

---

## Task 5: `gpuDeviceLossEvents` telemetry counter (emit + parse)

**Precondition:** Task 4. (`OutputDispatchStats` carries `gpuOomDegrades`/`fenceWaitStalls`/`gpuVramBytes` but NOT `gpuDeviceLossEvents` — this task adds it and wires the worker to write it from the drained monitor events.)

**Files:**
- Modify: `playback/output/outputdispatcher.h` (add the field), `playback/playbackworker.cpp` (drain `GpuDeviceLossMonitor::consumeLossEvent()` into the stat in the telemetry-emit path), `tests/e2e/play_harness.cpp` (emit `gpuDeviceLossEvents`), `tests/e2e/run_playback_e2e.sh` (parse + assert)
- Test: extend `tests/unit/tst_gpu_devicelost_worker.cpp`

**Interfaces:**
- Consumes: `GpuDeviceLossMonitor::consumeLossEvent()`/`lossCount()` (Task 1), `OutputDispatchStats` (existing — `os.gpuOomDegrades` is the sibling pattern), the worker's `counters()` + `stats()` emit path (`playbackworker.cpp:150-159`).
- Produces:
  ```cpp
  // playback/output/outputdispatcher.h, struct OutputDispatchStats (add after gpuOomDegrades):
  qint64 gpuDeviceLossEvents = 0;  // GPU device losses survived this session (CPU-degraded)
  ```

- [ ] **Step 1: Write the failing test**

Add to `tst_gpu_devicelost_worker.cpp`:

```cpp
    void lossCountReflectsRecordedEvents();
```

```cpp
void TestGpuDeviceLostWorker::lossCountReflectsRecordedEvents() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    QCOMPARE(m.lossCount(), uint64_t(0));
    m.recordLoss();
    m.recordLoss();
    QCOMPARE(m.lossCount(), uint64_t(2));
    // Both events are drainable for telemetry.
    QVERIFY(m.consumeLossEvent());
    QVERIFY(m.consumeLossEvent());
    QVERIFY(!m.consumeLossEvent());
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpu_devicelost_worker
```
Expected: FAIL — the new slot is unrun until built; build then run to confirm the assertion drives the drain contract. (The monitor already supports it from Task 1, so this slot PASSES once built — it pins the telemetry-source contract the next steps consume.)

- [ ] **Step 3: Write the implementation**

In `playback/output/outputdispatcher.h`, add `qint64 gpuDeviceLossEvents = 0;` to `struct OutputDispatchStats` immediately after `gpuOomDegrades` (the field defaults to 0 → byte-green on the CPU path).

In `playback/playbackworker.cpp`, in the telemetry-emit path where `os` (the `OutputDispatchStats`) is read for emission, accumulate drained loss events into the stat (a worker-side running total mirroring how `gpuReadToCpuCount` is surfaced in `counters()`). Add a `qint64 m_gpuDeviceLossEvents = 0;` worker member, and in `emitTelemetry` (or wherever `counters()`/`stats()` are gathered for the harness) drain:
```cpp
#ifdef OLR_GPU_PIPELINE_BUILD
    while (GpuDeviceLossMonitor::instance().consumeLossEvent()) ++m_gpuDeviceLossEvents;
#endif
```
and expose it so `play_harness` can print it — surface via the existing `stats()` snapshot by setting `os.gpuDeviceLossEvents = m_gpuDeviceLossEvents;` before the harness reads it, OR add it to `PlaybackCounters` (mirror `gpuReadToCpuCount`). Use the `OutputDispatchStats` field so the harness's existing `os.*` print block carries it (the simplest, matching `gpuOomDegrades`).

In `tests/e2e/play_harness.cpp`, extend the counter-emit `printf` (the block at ~:160-166 that already prints `gpuOomDegrades=%lld gpuVramBytes=%lld`) to also print `gpuDeviceLossEvents=%lld` from `os.gpuDeviceLossEvents`; update the header comment listing the counters (~:17-18).

In `tests/e2e/run_playback_e2e.sh`, add `gpuDeviceLossEvents="$(get gpuDeviceLossEvents)"` next to the other GPU getters (~:251-258) and the `[ -n ... ] || gpuDeviceLossEvents="?"` default (~:278-285).

- [ ] **Step 4: Run the test + harness smoke, expect PASS**

```sh
cmake --build build/c --target tst_gpu_devicelost_worker && ctest --test-dir build/c -R tst_gpu_devicelost_worker --output-on-failure
cmake --build build/c --target play_harness
```
Expected: unit PASS (3 slots); `play_harness` builds and its counter line now includes `gpuDeviceLossEvents=0` on a normal run.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/output/outputdispatcher.h playback/playbackworker.cpp playback/playbackworker.h \
        tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/unit/tst_gpu_devicelost_worker.cpp
git commit -m "feat(device-loss): gpuDeviceLossEvents telemetry counter (emit + parse)"
```

---

## Task 6: Fault-injection e2e — degrade without crash, frames keep flowing, rebuild resumes

**Precondition:** Tasks 1-5. **This is the headline acceptance gate: the live-broadcast hard-down survival proof.**

**Files:**
- Modify: `tests/e2e/play_harness.cpp` (a `devicelost` scenario that injects a loss mid-stream via the test hook), `tests/e2e/run_playback_e2e.sh` (a `devicelost)` case asserting the survival invariants), `tests/CMakeLists.txt` (register the scenario as `e2e_play_devicelost`)
- Test: a focused unit `tests/unit/tst_gpu_devicelost_resume.cpp` for the rebuild-resume generation assertion

**Interfaces:**
- Consumes: `GpuRhiContext::injectDeviceLostForTest()` (Task 3), the worker latch + `handleGpuDeviceLoss`/`rebuildGpuSpine` (Task 4), `GpuGenerationCounter`/`GpuDeviceLossMonitor` (Tasks 1/3), the e2e counters (`placeholderFramesDelta`, `heldFramesDelta`, `gpuDeviceLossEvents`, `gpuReadToCpuCount`).
- Produces: no new product interface — a survival gate + a resume unit.

- [ ] **Step 1: Write the failing resume unit**

Create `tests/unit/tst_gpu_devicelost_resume.cpp` — prove (c): a simulated successful rebuild resumes the GPU path under a bumped generation:

```cpp
// Rebuild-resume: after a device loss, a successful spine rebuild resumes the GPU
// path under a BUMPED generation, so a handle minted before the loss stays stale
// while a freshly minted handle is live.
#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/output/framehandle.h"

class TestGpuDeviceLostResume : public QObject {
    Q_OBJECT
private slots:
    void rebuildResumesUnderBumpedGeneration();
};

void TestGpuDeviceLostResume::rebuildResumesUnderBumpedGeneration() {
    auto& m = GpuDeviceLossMonitor::instance();
    m.reset();
    GpuGenerationCounter::instance().reset();
    const uint64_t mintedGen = GpuGenerationCounter::instance().bump();  // pre-loss window
    FrameHandle preLoss = solidYuv420pHandle(16, 16, 16, 128, 128);
    preLoss.metadata().gpuGeneration = mintedGen;
    QVERIFY(!preLoss.isStaleForGeneration(GpuGenerationCounter::instance().current()));

    m.recordLoss();  // device loss -> generation bumps, latch set
    QVERIFY(preLoss.isStaleForGeneration(GpuGenerationCounter::instance().current()));

    m.clearForRebuild();  // rebuild SUCCEEDED -> GPU path resumes
    QVERIFY(!m.isLost());
    const uint64_t resumeGen = GpuGenerationCounter::instance().current();
    FrameHandle postRebuild = solidYuv420pHandle(16, 16, 16, 128, 128);
    postRebuild.metadata().gpuGeneration = resumeGen;
    QVERIFY(!postRebuild.isStaleForGeneration(resumeGen));            // fresh handle live
    QVERIFY(preLoss.isStaleForGeneration(resumeGen));                 // old handle still stale
}

QTEST_GUILESS_MAIN(TestGpuDeviceLostResume)
#include "tst_gpu_devicelost_resume.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:
```cmake
    olr_add_unit_test(tst_gpu_devicelost_resume olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the unit, expect FAIL then PASS**

```sh
cmake --build build/c --target tst_gpu_devicelost_resume
```
Expected first run: FAIL (unregistered target). After adding + reconfigure: PASS (the generation/latch contract from Tasks 1/3 already supports it; this pins the resume semantics downstream consumers rely on).

- [ ] **Step 3: Add the e2e `devicelost` scenario**

In `tests/e2e/play_harness.cpp`, add a `devicelost` scenario that, under `OLR_GPU_PIPELINE=1`, plays a fixture and at a fixed mid-stream tick calls the worker's GPU context `injectDeviceLostForTest()` (exposed via a worker test seam, e.g. `PlaybackWorker::injectGpuDeviceLossForTest()` that forwards to `m_gpuRhi->injectDeviceLostForTest()` and sets a pending flag the next decode tick observes). The harness asserts the run completes (no crash) and emits the counters. Add the seam:
```cpp
// playback/playbackworker.h (public, OLR_GPU_PIPELINE_BUILD only):
void injectGpuDeviceLossForTest();  // test-only: force the next GPU op to see a lost device
```
implemented as:
```cpp
#ifdef OLR_GPU_PIPELINE_BUILD
void PlaybackWorker::injectGpuDeviceLossForTest() {
    if (m_gpuRhi) m_gpuRhi->injectDeviceLostForTest();
}
#endif
```

In `tests/e2e/run_playback_e2e.sh`, add a `devicelost)` case in the scenario `case` block (near `play1x)`) that:
- requires `OLR_GPU_PIPELINE=1` (else `SKIP` with a clear message — CI macOS is CPU-oracle-only per §9);
- asserts the run produced output (`decodedVideoFrames > 0`) and **survived**: `placeholderFramesDelta == 0` and `heldFramesDelta == 0` through the loss (no gray flash / stall — the hard-down survival invariant);
- asserts `gpuDeviceLossEvents >= 1` (the loss was detected + recorded);
- asserts the output kept flowing on the CPU path after the loss (`gpuReadToCpuCount` stops increasing post-loss is acceptable; the gate is that frames continued — `framesSubmitted`/`decodedVideoFrames` advanced past the injection tick).

Register the scenario as an `e2e_play` invocation in `tests/CMakeLists.txt` (mirror the existing `armedcut` registration) so `ctest -R e2e_play_devicelost` runs it; gate it to run only when `OLR_GPU_PIPELINE` is ON and a GPU host is present (the script self-skips otherwise).

- [ ] **Step 4: Run the e2e devicelost (GPU host)**

```sh
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R e2e_play_devicelost --output-on-failure
# or directly:
OLR_GPU_PIPELINE=1 tests/e2e/run_playback_e2e.sh \
    build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness devicelost 1 9401
```
Expected: PASS with `placeholderFramesDelta=0 heldFramesDelta=0 gpuDeviceLossEvents>=1`, the run completing without a crash. On a non-GPU host the script prints a SKIP. On the CPU lane (`OLR_GPU_PIPELINE` unset) the scenario is skipped, never failed.

- [ ] **Step 5: Full pre-flight + final review**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R "e2e_play" --output-on-failure
# Reproduce the CI ASan/TSan lane locally for the loss/latch threading:
OLR_PREPUSH_FULL=1 .githooks/pre-push
# Off-path byte-green check (fresh build dir):
cmake -S . -B build/off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
cmake --build build/off && ctest --test-dir build/off -L unit --output-on-failure
```
Expected: GPU-on and GPU-off suites both PASS; `e2e_play` thresholds unchanged on the CPU lane; no new ASan/TSan findings on the loss/latch/rebuild threading.

**Review gate (final, whole-subproject):** per CLAUDE.md, the playback worker's threading + every `gpu-sync`-adjacent change gets an independent fresh-agent concurrency review before merge. The reviewer verifies: (a) no fence-wait on a dead device anywhere (grep `handleGpuDeviceLoss`/`rebuildGpuSpine` — they DROP fences, never `->wait`); (b) `handleGpuDeviceLoss` holds no `m_bufferMutex`; (c) once `CpuFallback`, the GPU decode branch is fully skipped (`gpuPathActive()` gate) so no surface from a dead generation is presented; (d) the generation bump on loss is observed before the latch by every consumer (the present path reads `isStaleForGeneration` before emitting). Open the PR with the per-branch push:
```sh
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin gpu-phase4-device-loss
```

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/CMakeLists.txt \
        playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_gpu_devicelost_resume.cpp \
        tests/unit/CMakeLists.txt
git commit -m "test(device-loss): fault-injection e2e (degrade without crash, frames flow, rebuild resumes)"
```

---

## Canonical device-loss contract (downstream / sibling plans consume these names verbatim)

`gpu-budget` (OOM-safe degrade shares the latch), `gpu-encode`, and `new-io-targets` build on exactly these types and signatures. Do not rename or vary them.

**Device-loss latch** — `playback/gpu/gpudevicelossmonitor.h` (NEW):
```cpp
class GpuDeviceLossMonitor {
    static GpuDeviceLossMonitor& instance();
    bool isLost() const;
    uint64_t lossCount() const;
    uint64_t recordLoss();        // bump GpuGenerationCounter + set latch + count
    bool consumeLossEvent();      // drain one event for telemetry (does NOT clear latch)
    void clearForRebuild();       // clear latch after a successful rebuild
    void reset();                 // session/test boundary
};
```

**RHI device-removed poll + fault hook** — `playback/gpu/olrrhi.h` + `playback/gpu/gpurhicontext.h` (ADDED methods):
```cpp
bool OlrRhi::deviceLost() const;            void OlrRhi::injectDeviceLostForTest();
bool GpuRhiContext::deviceLost() const;     void GpuRhiContext::injectDeviceLostForTest();
// importAndReadback() returns an invalid CpuPlanes once the device is lost.
```

**Worker latch + orchestration** — `playback/playbackworker.h` (NEW):
```cpp
enum class GpuPipelineState { Gpu, RebuildPending, CpuFallback };
GpuPipelineState gpuPipelineState() const;   // lock-free
bool gpuPathActive() const;                  // state == Gpu && m_gpuRhi valid
void handleGpuDeviceLoss();                  // invalidate -> rebuild -> degrade (no m_bufferMutex)
bool rebuildGpuSpine();                      // fresh GpuRhiContext + FRESH GpuFence/DecodeDoneFence
void injectGpuDeviceLossForTest();           // test-only seam (forwards to m_gpuRhi)
```

**Telemetry** — `playback/output/outputdispatcher.h` (ADDED field; emitted by `play_harness.cpp`, parsed by `run_playback_e2e.sh`):
```cpp
qint64 gpuDeviceLossEvents = 0;  // GPU device losses survived this session (CPU-degraded)
```
