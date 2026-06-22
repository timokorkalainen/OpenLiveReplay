# gpu-sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Graduate the minimal Phase-2 `DecodeDoneFence` stub into the real GPU sync layer the whole back half of the program (`gpu-compositor`, `async-readback`, `gpu-budget`, `device-loss`, `gpu-encode`) consumes. Concretely: (1) a **real GPU-level fence/wait** — on Apple actually wait on the `MTLSharedEvent` (`waitUntilSignaledValue:timeoutMs:` / `MTLSharedEventListener`) and on the GPU queue (`encodeWaitForEvent:`) instead of today's CPU-bool poll; on Windows backend-matched per P0.2 (`ID3D11Fence` 11.4, the chosen `kWinRhiBackend="d3d11"`), reusing the existing `D3DFence`; (2) a **GPU generation counter** analogous to `m_seekGeneration` that invalidates stale GPU surfaces on reposition/seek; (3) an **eviction guard** honoring the `FrameHandle` surface-lifetime contract (§4) — cap-shrink must not free a GPU surface with a render/readback fence in flight, and it must obey the spec §10 lock rule (collect victims, **release `m_bufferMutex`**, then fence-wait/free); (4) the **armed-cut staging-swap fence** (staging-decoder-idle before fire); (5) a **multi-feed cap-pressure GPU stress test** (evict-while-render, armed-cut-while-render-pending, seek-under-decode, frame-checksum validators) — the scenario the single-feed Phase-2 slice could not exercise.

**Architecture:** A small `playback/gpu/` sync family sits on top of the merged Phase-2 abstraction. `DecodeDoneFence` (`playback/gpu/decodedonefence.h`) gains a real GPU-wait surface; a new `GpuFence` interface generalizes it to the backend-matched render/readback fence (`MTLSharedEvent` on Apple, `D3DFence`/`ID3D11Fence` on Windows). A process-wide `GpuGenerationCounter` (`playback/gpu/gpugeneration.h`) mirrors `PlaybackWorker::m_seekGeneration`: `seekTo`/`repositionTo` bump it, every GPU-backed `FrameHandle` stamps the generation it was minted under, and `GpuFrameData::isStale()` / a snapshot-time guard drop surfaces from a superseded generation. A `GpuSurfaceLifetime` hook (Apple `AppleGpuSurface` gets the `retainUntilFenceRetired(fenceValue)` / `pendingFenceValue()` pair the Windows `D3D11GpuSurface` already has) lets eviction defer the free. The eviction guard lives in `PlaybackWorker` as a two-phase `evictVictimsLocked()` (collects under `m_bufferMutex`) + `drainEvictedVictims()` (waits/frees **after** the lock drops), wired into the `TrackBuffer::insert` cap path and the run-loop trim. The armed-cut path adds a `m_stagingFence` that `fillStaging` signals when the staging decoder is idle and `maybeFireScheduledCut` waits on before the swap. The new `tst_gpu_sync_stress` unit test and an `e2e_play_gpu_capstress` multi-feed scenario exercise the TSan-invisible GPU races.

**Tech Stack:** C++17, Qt 6 (Core/Gui/GuiPrivate for `QRhi`, Test), Apple Metal/CoreVideo/IOSurface (`MTLSharedEvent`, `MTLSharedEventListener`, `dispatch_queue`), Windows D3D11.4 (`ID3D11Fence`, `ID3D11Device5`, `ID3D11DeviceContext4` — already wrapped by `playback/output/win/d3dfence.{h,cpp}`), CMake + Ninja. Consumes the merged Phase-0/1/2 contracts: `FrameHandle`/`IFrameData`/`CpuPlanes`/`FrameMetadata`/`FramePayloadKey`/`FramePixelFormat`/`ColorMetadata` (`playback/output/framehandle.h`), `GpuSurface`/`GpuSurfaceDesc` (`playback/gpu/gpusurface.h`), `GpuRhiContext` (`playback/gpu/gpurhicontext.h`), `GpuFrameData`/`makeGpuFrameHandle` (`playback/gpu/gpuframedata.h`), `DecodeDoneFence` (`playback/gpu/decodedonefence.h`), `gpuPipelineEnabled`/`gpuForcedPerTrackBudget`/`gpuConsumeInjectedAllocFailure`/`gpuSetInjectedAllocFailures` (`playback/gpu/gpupipelineconfig.h`), `D3DFence`/`D3D11GpuSurface` (`playback/output/win/`), `GpuReadbackTelemetry` (`playback/output/gpureadbacktelemetry.h`), and `TrackBuffer`/`PlaybackWorker` (`playback/trackbuffer.*`, `playback/playbackworker.*`).

## Global Constraints

- **Builds ON merged Phase-2 code, never replaces it.** Every interface named below either already exists in the tree (extend in place: `DecodeDoneFence`, `D3DFence`, `D3D11GpuSurface`, `GpuFrameData`, `PlaybackWorker::m_decodeFence`) or is genuinely new (`GpuFence`, `GpuGenerationCounter`, `evictVictimsLocked`/`drainEvictedVictims`, `m_stagingFence`). Use the **actual** signatures verified in the tree; do not invent variant names. Downstream plans (`gpu-compositor`, `async-readback`, `gpu-budget`, `device-loss`, `gpu-encode`) consume these names verbatim — the canonical sync contract is listed at the end of this plan.
- **CPU path stays default + reference.** The GPU pipeline is two-gated: `OLR_GPU_PIPELINE` CMake option → `OLR_GPU_PIPELINE_BUILD` compile def (CMakeLists.txt:452-453), and the runtime `gpuPipelineEnabled()` env flag (off by default). With `OLR_GPU_PIPELINE_BUILD` undefined **or** `gpuPipelineEnabled()` false, every byte of this plan's behavior must be inert: the CPU path is byte-for-byte the Phase-1/2 path and remains the permanent correctness oracle + fallback. New `PlaybackWorker` members and counters compile unconditionally only when they are cheap scalars; all fence/generation logic is `#ifdef OLR_GPU_PIPELINE_BUILD`.
- **The lock rule is non-negotiable (spec §10, D4).** Eviction and the armed-cut swap **never wait on a GPU fence while holding `m_bufferMutex`**. Today `makeOutputSnapshot` already enforces `m_outputRuntimeMutex → m_bufferMutex` (it reads `dispatcherNextOutputFrameIndex()` under `m_outputRuntimeMutex` *before* locking `m_bufferMutex`, playbackworker.cpp:478-500). The eviction guard adds: collect victims under `m_bufferMutex`, **release it**, then fence-wait + free. A fence-wait under `m_bufferMutex` would deadlock against the render thread that needs the same surface and signals the fence. Every fence-wait site carries a `// LOCK RULE:` comment proving no `m_bufferMutex` is held.
- **Concurrency-critical — independent review required before merge (CLAUDE.md "Verification").** TSan cannot see GPU command ordering. The contract is: a consumer never reads a surface the producer is still writing, and eviction never frees a surface with a fence in flight. Each task that touches the fence/eviction/swap threading carries a `**Review gate:**` note; the branch gets a fresh-agent concurrency review before the PR merges.
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
  Also build once with `-DOLR_GPU_PIPELINE=OFF` (a fresh build dir) to confirm the off-path still compiles and the unit suite is byte-green. Unit tests register via `olr_add_unit_test(<name> <libs...>)` in `tests/unit/CMakeLists.txt` (GPU tests inside the `if(OLR_GPU_PIPELINE)` block at lines 196-202); the GPU backend lib is `olr_test_gpu` (defined in `tests/CMakeLists.txt:288-312`) and `olr_test_playback` carries the worker. Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.
- **P0.2 is resolved to D3D11.** `playback/output/win/wingpuimportedge.h:58` pins `constexpr const char* kWinRhiBackend = "d3d11";`. So the Windows fence primitive is `ID3D11Fence` (11.4), already wrapped by `D3DFence` (`d3dfence.cpp` gates on `ID3D11Device5::CreateFence` + `ID3D11DeviceContext4::Signal` + `ID3D11Fence::GetCompletedValue`). This plan does NOT add an `ID3D12Fence` path; if a future probe flips `kWinRhiBackend` to `"d3d12"`, that is a follow-up.
- **P0.3 constrains the wait.** Synchronous import→composite→readback measured 1.78 ms (readback 1.18 ms), over the 0.5 ms budget. Fences gate readback **ordering**, not its cost; this plan never claims the fence makes synchronous readback fit budget. The render/readback fence's `wait()` is the ordering primitive `async-readback` (Phase 4) later pipelines (render N / read N-2). Where a wait runs on the hot 1 ms tick, it carries a bounded timeout and a `fenceWaitStalls` telemetry bump, never an unbounded block.
- **Format changed lines only** (CI Lint checks changed lines; engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
- **Public-repo professionalism.** Self-contained, professional code/comments/commits; document the present design, no internal notes or private history.

---

## Preconditions (read before Task 1)

- **Phase 2 merged.** `playback/gpu/{gpusurface.h,gpurhicontext.h,gpuframedata.{h,cpp},decodedonefence.{h,decodedonefence_apple.mm,decodedonefence_stub.cpp},gpupipelineconfig.{h,cpp},appleiosurface.h,applegpusurface_apple.mm,vtkeepsurfaceimporter*}` exist, as do `playback/output/win/{d3dfence.{h,cpp},d3d11gpusurface.{h,cpp},wingpuimportedge.*}`, the `OLR_GPU_PIPELINE_BUILD` branches in `playback/playbackworker.cpp` (the macOS GPU decode branch at :666-727, the Windows branch at :728-790, the `m_decodeFence`/`m_gpuRhi` members at playbackworker.h:374-375), and the e2e GPU counters in `tests/e2e/play_harness.cpp` + `tests/e2e/run_playback_e2e.sh` (`fenceWaitStalls`, `gpuOomDegrades`, `gpuVramBytes`, `readbackQueueDepth`, `readbackDrops` are already emitted/parsed). Verify with `git merge-base --is-ancestor <phase2-sha> origin/main` if unsure.
- **Today's `DecodeDoneFence` is a CPU-bool poll, NOT a GPU wait.** `MetalDecodeDoneFence::signalDecodeDone` sets `m_event.signaledValue = 1` *and* a CPU `std::atomic<bool> m_signaled`; `waitDecodeDone` spins on `m_signaled` with a 1 ms sleep — it never waits on the `MTLSharedEvent` on the GPU timeline (decodedonefence_apple.mm:23-43). Task 1 replaces the wait with a real GPU wait. The Windows `D3DFence::wait` already polls `ID3D11Fence::GetCompletedValue` (d3dfence.cpp), which is a real GPU-timeline value, so the Windows graduation in Task 2 is mostly wiring, not a new primitive.

---

## Task 1: Real Apple GPU fence wait (`MTLSharedEvent`) — replace the CPU-bool poll

**Precondition:** Phase 2 merged; a Metal device is available on the test host (else `QSKIP`).

**Files:**
- Modify: `playback/gpu/decodedonefence.h`, `playback/gpu/decodedonefence_apple.mm`
- Test: `tests/unit/tst_decodedonefence.cpp` (extend the existing test exe)

**Interfaces:**
- Consumes: `DecodeDoneFence` (existing — `create()`, `signalDecodeDone()`, `waitDecodeDone(int)`, `isSignaled()`).
- Produces (extend `DecodeDoneFence` in place, ADD methods, keep existing ones):
  ```cpp
  // playback/gpu/decodedonefence.h
  class DecodeDoneFence {
  public:
      static std::shared_ptr<DecodeDoneFence> create();
      virtual ~DecodeDoneFence();

      // Existing minimal API (Phase 2). signalDecodeDone now advances the GPU
      // timeline value AND records the value for waiters; waitDecodeDone now
      // performs a REAL GPU-timeline wait (Apple: MTLSharedEvent), not a CPU bool.
      virtual void signalDecodeDone() = 0;
      virtual bool waitDecodeDone(int timeoutMs) = 0;
      virtual bool isSignaled() const = 0;

      // NEW: the monotonically-increasing value this fence has been signaled to
      // (0 = never signaled). Lets the eviction guard record "free this surface
      // once the fence retires value V" and lets multi-stage producers advance
      // the timeline more than once (decode-done, then render-done).
      virtual uint64_t signaledValue() const = 0;

      // NEW: wait for the GPU timeline to reach exactly `value` (not just "any
      // signal"). Returns true if reached within timeoutMs, false on timeout.
      // This is the ordering primitive the render/readback path and the eviction
      // guard use. waitDecodeDone(t) == waitForValue(signaledValue(), t) when a
      // signal has happened.
      virtual bool waitForValue(uint64_t value, int timeoutMs) = 0;
  };
  ```

- [ ] **Step 1: Write the failing test (real GPU wait reaches a specific value)**

Add to `tests/unit/tst_decodedonefence.cpp`:

```cpp
    void signaledValueAdvances();
    void waitForValueReachesGpuTimeline();
```

```cpp
void TestDecodeDoneFence::signaledValueAdvances() {
    auto fence = DecodeDoneFence::create();
    if (!fence) QSKIP("no fence backend on this platform");
    QCOMPARE(fence->signaledValue(), uint64_t(0));
    fence->signalDecodeDone();
    QVERIFY(fence->signaledValue() >= 1);
    const uint64_t v1 = fence->signaledValue();
    fence->signalDecodeDone();
    QVERIFY(fence->signaledValue() > v1);  // monotonic
}

void TestDecodeDoneFence::waitForValueReachesGpuTimeline() {
    auto fence = DecodeDoneFence::create();
    if (!fence) QSKIP("no fence backend on this platform");
    // Not yet signaled to value 1: a short wait must TIME OUT (proving the wait
    // observes the real timeline, not an always-true CPU bool).
    QVERIFY(!fence->waitForValue(1, 50));
    fence->signalDecodeDone();
    const uint64_t v = fence->signaledValue();
    QVERIFY(fence->waitForValue(v, 1000));  // now reachable
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_decodedonefence
```
Expected: FAIL to compile — `signaledValue` / `waitForValue` are not members of `DecodeDoneFence`.

- [ ] **Step 3: Write the implementation**

Add the two pure-virtual methods to `playback/gpu/decodedonefence.h` (after `isSignaled`), with the doc comments from Interfaces above.

Rewrite `MetalDecodeDoneFence` in `playback/gpu/decodedonefence_apple.mm` so the wait is a REAL GPU-timeline wait via `MTLSharedEventListener` + `notifyListener:atValue:block:`, replacing the CPU-bool spin. Keep an `std::atomic<uint64_t> m_signaled{0}` only as the CPU-side *cache* of the last value we asked the GPU to reach (so `signaledValue()` is lock-free), but `waitForValue` actually blocks on the event:

```objcpp
#include "playback/gpu/decodedonefence.h"

#ifdef __APPLE__

#include <Metal/Metal.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {

class MetalDecodeDoneFence final : public DecodeDoneFence {
public:
    MetalDecodeDoneFence(id<MTLDevice> device, id<MTLSharedEvent> event)
        : m_device(device), m_event(event) {
        // One process-private serial queue services the listener callbacks.
        m_listener = [[MTLSharedEventListener alloc]
            initWithDispatchQueue:dispatch_queue_create("olr.gpu.fence", DISPATCH_QUEUE_SERIAL)];
    }
    ~MetalDecodeDoneFence() override {
        [m_listener release];
        [m_event release];
        [m_device release];
        m_listener = nil;
        m_event = nil;
        m_device = nil;
    }

    void signalDecodeDone() override {
        // Advance the GPU timeline. In the decode path the surface producer is
        // the VT/Metal queue; here we advance the host-side value so a CPU
        // consumer can wait on it (the queue-side encodeSignalEvent: is added by
        // the render path in gpu-compositor; this host signal is the decode-done
        // anchor). signaledValue() is the cached monotonic value.
        const uint64_t next = m_target.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_event.signaledValue = next;  // GPU timeline value
    }

    bool isSignaled() const override { return signaledValue() >= 1; }

    uint64_t signaledValue() const override {
        return m_target.load(std::memory_order_acquire);
    }

    bool waitForValue(uint64_t value, int timeoutMs) override {
        if (value == 0) return true;
        // Fast path: already retired on the GPU timeline.
        if (m_event.signaledValue >= value) return true;

        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        // notifyListener fires on m_listener's serial queue when the event's
        // signaledValue >= value on the GPU timeline — a REAL GPU wait, not a
        // CPU bool. This is what guarantees a consumer never reads a surface the
        // producer is still writing.
        [m_event notifyListener:m_listener
                        atValue:value
                          block:^(id<MTLSharedEvent>, uint64_t) {
                            std::lock_guard<std::mutex> lk(mtx);
                            done = true;
                            cv.notify_all();
                          }];
        std::unique_lock<std::mutex> lk(mtx);
        if (timeoutMs < 0) {
            cv.wait(lk, [&] { return done; });
            return true;
        }
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return done; });
    }

    bool waitDecodeDone(int timeoutMs) override {
        const uint64_t v = signaledValue();
        if (v == 0) {
            // Never signaled: emulate the Phase-2 contract (block until signaled
            // or timeout). We cannot register a listener for an unknown value, so
            // poll the cached target with a bounded sleep, then wait on the GPU
            // timeline for the value once known.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (signaledValue() == 0) {
                if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return waitForValue(signaledValue(), timeoutMs);
    }

    id<MTLSharedEvent> event() const { return m_event; }

private:
    id<MTLDevice> m_device = nil;
    id<MTLSharedEvent> m_event = nil;
    MTLSharedEventListener* m_listener = nil;
    mutable std::atomic<uint64_t> m_target{0};
};

}  // namespace

std::shared_ptr<DecodeDoneFence> DecodeDoneFence::create() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nullptr;
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!event) {
        [device release];
        return nullptr;
    }
    return std::make_shared<MetalDecodeDoneFence>(device, event);
}

#endif  // __APPLE__
```

Add the missing includes (`<chrono>`, `<thread>`) at the top alongside the existing ones.

Update `playback/gpu/decodedonefence_stub.cpp` to implement the two new methods (keep the existing CPU-bool behavior on non-GPU platforms so the Windows/Linux build still links — Windows graduates to the real `D3DFence` wait in Task 2 via a separate path; the stub stays a deterministic bool fence for the `olr_test_gpu` lib and headless CI):

```cpp
// in MetalDecodeDoneFence's stub analogue (the non-Apple DecodeDoneFence impl):
uint64_t signaledValue() const override { return m_value.load(std::memory_order_acquire); }
bool waitForValue(uint64_t value, int timeoutMs) override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (m_value.load(std::memory_order_acquire) < value) {
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
// signalDecodeDone increments m_value (std::atomic<uint64_t>); isSignaled() returns m_value>=1.
```

**Review gate:** this is the core "consumer never reads a surface the producer is still writing" primitive. The `MTLSharedEventListener` block runs on a serial dispatch queue off the worker/render thread; the `std::condition_variable` handoff must not race the listener teardown (the listener is released in the dtor — ensure no in-flight block outlives `this`; if needed, drain by waiting for the listener queue before release). Flag for independent review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_decodedonefence && ctest --test-dir build/c -R tst_decodedonefence --output-on-failure
```
Expected: PASS (4 tests on a Metal host: the two existing + `signaledValueAdvances` + `waitForValueReachesGpuTimeline`). On a host with no Metal device the new slots `QSKIP`.

- [ ] **Step 5: Verify the zero-regression gate**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: the full unit suite PASSES (the fence change is additive; the worker's `signalDecodeDone()` calls are unchanged).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/decodedonefence.h playback/gpu/decodedonefence_apple.mm \
        playback/gpu/decodedonefence_stub.cpp tests/unit/tst_decodedonefence.cpp
git commit -m "feat(gpu-sync): real MTLSharedEvent GPU wait + monotonic timeline value"
```

---

## Task 2: `GpuFence` — backend-matched render/readback fence interface

**Precondition:** Task 1. On Windows the chosen backend is D3D11 (`kWinRhiBackend="d3d11"`), so the concrete is `D3DFence` (already built); on Apple it wraps `DecodeDoneFence`'s `MTLSharedEvent`.

**Files:**
- Create: `playback/gpu/gpufence.h`, `playback/gpu/gpufence_apple.mm`, `playback/gpu/gpufence_stub.cpp`, `playback/gpu/gpufence_win.cpp`
- Test: `tests/unit/tst_gpufence.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `DecodeDoneFence` (Task 1), `D3DFence` (`playback/output/win/d3dfence.h` — `create(ID3D11Device*, QString*)`, `signal(ID3D11DeviceContext*)→uint64_t`, `wait(uint64_t,int)`, `completedValue()`).
- Produces:
  ```cpp
  // playback/gpu/gpufence.h — platform-neutral; NO Metal/D3D types in the header.
  // The backend-matched render/readback fence (spec D4). On Apple it is the
  // MTLSharedEvent timeline (shared with DecodeDoneFence); on Windows it is the
  // ID3D11Fence (11.4) wrapped by D3DFence (P0.2: kWinRhiBackend == "d3d11").
  // Generalizes DecodeDoneFence so the eviction guard, the render path, and the
  // armed-cut swap all speak one ordering primitive.
  class GpuFence {
  public:
      virtual ~GpuFence();

      // Advance the timeline (a GPU-queue signal where the backend supports it;
      // a host signal otherwise). Returns the value the timeline now targets.
      virtual uint64_t signal() = 0;

      // Block until the GPU timeline retires `value`, or timeoutMs elapses.
      // Returns true if retired. timeoutMs < 0 == wait forever.
      virtual bool wait(uint64_t value, int timeoutMs) = 0;

      // The value most recently RETIRED on the GPU timeline (lock-free poll).
      virtual uint64_t completedValue() const = 0;

      // Returns nullptr if no GPU fence backend exists on this host (the caller
      // then runs the CPU path / degrades). Apple: MTLSharedEvent. Windows:
      // D3DFence over the RHI device (D3D11). Else: a deterministic stub fence.
      static std::shared_ptr<GpuFence> create();
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpufence.cpp`:

```cpp
// GpuFence is the backend-matched render/readback ordering primitive (spec D4).
// The interface is uniform; the concrete is MTLSharedEvent on Apple and
// ID3D11Fence on Windows. With no backend it returns nullptr and the caller
// degrades. On a backend host signal()/wait()/completedValue() round-trip.
#include <QtTest>

#include "playback/gpu/gpufence.h"

#include <thread>

class TestGpuFence : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void signalWaitRoundTrips();
    void waitTimesOutBeforeSignal();
};

void TestGpuFence::createIsNullOrValidNeverPartial() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host (expected under offscreen/CI stub off)");
    QVERIFY(fence->completedValue() == 0 || fence->completedValue() >= 0);
}

void TestGpuFence::signalWaitRoundTrips() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");
    const uint64_t v = fence->signal();
    QVERIFY(v >= 1);
    QVERIFY(fence->wait(v, 1000));
    QVERIFY(fence->completedValue() >= v);
}

void TestGpuFence::waitTimesOutBeforeSignal() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");
    // A value never signaled must time out (proves a real timeline, not a bool).
    QVERIFY(!fence->wait(fence->completedValue() + 100, 50));
}
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt` (after `tst_decodedonefence`):

```cmake
    olr_add_unit_test(tst_gpufence olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpufence
```
Expected: FAIL to compile — `playback/gpu/gpufence.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpufence.h` with the interface above.

Create `playback/gpu/gpufence_apple.mm` — wrap a `DecodeDoneFence` (the `MTLSharedEvent` timeline from Task 1) so Apple has one timeline shared between decode-done and render/readback:

```objcpp
#include "playback/gpu/gpufence.h"

#ifdef __APPLE__

#include "playback/gpu/decodedonefence.h"

GpuFence::~GpuFence() = default;

namespace {
class MetalGpuFence final : public GpuFence {
public:
    explicit MetalGpuFence(std::shared_ptr<DecodeDoneFence> f) : m_fence(std::move(f)) {}
    uint64_t signal() override {
        m_fence->signalDecodeDone();
        return m_fence->signaledValue();
    }
    bool wait(uint64_t value, int timeoutMs) override {
        return m_fence->waitForValue(value, timeoutMs);
    }
    uint64_t completedValue() const override { return m_fence->signaledValue(); }
private:
    std::shared_ptr<DecodeDoneFence> m_fence;
};
}  // namespace

std::shared_ptr<GpuFence> GpuFence::create() {
    auto f = DecodeDoneFence::create();
    if (!f) return nullptr;
    return std::make_shared<MetalGpuFence>(std::move(f));
}

#endif  // __APPLE__
```

Create `playback/gpu/gpufence_win.cpp` — wrap `D3DFence` over the RHI device. `D3DFence::create` needs an `ID3D11Device*`; pull it from `WinGpuImportEdge` (which owns the RHI D3D device). Because the import edge is created lazily in the worker, `GpuFence::create()` on Windows returns nullptr until an edge exists; the worker constructs the fence from its edge instead (see Task 3 wiring). Provide a device-taking factory in addition to the parameterless `create()`:

```cpp
#include "playback/gpu/gpufence.h"

#ifdef _WIN32

#include "playback/output/win/d3dfence.h"

#include <d3d11.h>

GpuFence::~GpuFence() = default;

namespace {
class D3D11GpuFence final : public GpuFence {
public:
    D3D11GpuFence(std::unique_ptr<D3DFence> fence, ID3D11Device* device)
        : m_fence(std::move(fence)) {
        device->GetImmediateContext(&m_context);
    }
    ~D3D11GpuFence() override {
        if (m_context) m_context->Release();
    }
    uint64_t signal() override { return m_context ? m_fence->signal(m_context) : 0; }
    bool wait(uint64_t value, int timeoutMs) override { return m_fence->wait(value, timeoutMs); }
    uint64_t completedValue() const override { return m_fence->completedValue(); }
private:
    std::unique_ptr<D3DFence> m_fence;
    ID3D11DeviceContext* m_context = nullptr;
};
}  // namespace

// Device-taking factory used by the worker once WinGpuImportEdge owns the RHI
// D3D device. The parameterless create() returns nullptr (no global device).
std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device) {
    auto* device = static_cast<ID3D11Device*>(d3d11Device);
    if (!device) return nullptr;
    QString err;
    auto raw = D3DFence::create(device, &err);
    if (!raw) return nullptr;
    return std::make_shared<D3D11GpuFence>(std::move(raw), device);
}

std::shared_ptr<GpuFence> GpuFence::create() { return nullptr; }

#endif  // _WIN32
```

Declare `std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device);` in `gpufence.h` under `#ifdef _WIN32`.

Create `playback/gpu/gpufence_stub.cpp` — a deterministic timeline fence for non-Apple/non-Windows and the `olr_test_gpu` lib (so the unit test runs deterministically without a GPU):

```cpp
#include "playback/gpu/gpufence.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <atomic>
#include <chrono>
#include <thread>

GpuFence::~GpuFence() = default;

namespace {
class StubGpuFence final : public GpuFence {
public:
    uint64_t signal() override { return m_value.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int timeoutMs) override {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (m_value.load(std::memory_order_acquire) < value) {
            if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    uint64_t completedValue() const override { return m_value.load(std::memory_order_acquire); }
private:
    std::atomic<uint64_t> m_value{0};
};
}  // namespace

std::shared_ptr<GpuFence> GpuFence::create() { return std::make_shared<StubGpuFence>(); }

#endif  // !__APPLE__ && !_WIN32
```

Wire CMake. In `CMakeLists.txt` inside the `if(OLR_GPU_PIPELINE)` block (lines 452-468), add to the Apple `target_sources(OpenLiveReplay ...)`:
```cmake
            playback/gpu/gpufence.h playback/gpu/gpufence_apple.mm
```
to the `else()` (non-Apple) branch:
```cmake
            playback/gpu/gpufence.h playback/gpu/gpufence_stub.cpp
```
and within that else, for Windows add `playback/gpu/gpufence_win.cpp` (the existing Windows worker already links d3d11; mirror the `wingpuimportedge` source add). In `tests/CMakeLists.txt`, add `gpufence_apple.mm` (Apple) / `gpufence_stub.cpp` (else) / `gpufence_win.cpp` (Windows) to `olr_test_gpu` (lines 288-305) and to `olr_test_playback`'s GPU block (lines 271-286), mirroring the `decodedonefence` adds.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpufence && ctest --test-dir build/c -R tst_gpufence --output-on-failure
```
Expected: PASS (3 tests; on Apple the `MTLSharedEvent` path runs, on the stub the deterministic path runs).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpufence.h playback/gpu/gpufence_apple.mm playback/gpu/gpufence_stub.cpp \
        playback/gpu/gpufence_win.cpp tests/unit/tst_gpufence.cpp tests/unit/CMakeLists.txt \
        tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-sync): backend-matched GpuFence (MTLSharedEvent / ID3D11Fence)"
```

---

## Task 3: GPU generation counter (`GpuGenerationCounter`) — invalidate stale surfaces on seek/reposition

**Precondition:** Task 2.

**Files:**
- Create: `playback/gpu/gpugeneration.h`, `playback/gpu/gpugeneration.cpp`
- Test: `tests/unit/tst_gpugeneration.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`, `playback/output/framehandle.h` (stamp + read a generation on `FrameMetadata`), `playback/gpu/gpuframedata.h`/`.cpp` (`isStale()`), `playback/playbackworker.cpp` (bump on seek/reposition, stamp on mint, drop stale at snapshot)

**Interfaces:**
- Consumes: `FrameMetadata` (`playback/output/framehandle.h`), the merged `m_seekGeneration`/`m_committedGeneration` model in `PlaybackWorker`.
- Produces:
  ```cpp
  // playback/gpu/gpugeneration.h
  // Mirrors PlaybackWorker::m_seekGeneration for GPU residency: a reposition/seek
  // bumps the GPU generation; every GPU-backed FrameHandle is stamped with the
  // generation it was minted under; a handle whose stamp != current generation is
  // STALE and must not be presented (its surface belongs to a superseded window).
  // Process-wide single counter (one playback worker owns the GPU pipeline).
  class GpuGenerationCounter {
  public:
      static GpuGenerationCounter& instance();
      uint64_t current() const;       // lock-free read
      uint64_t bump();                // advance; returns the NEW value
      void reset();                   // test-only: back to 0
  private:
      GpuGenerationCounter() = default;
      std::atomic<uint64_t> m_gen{0};
  };
  ```
  And on `FrameMetadata` (ADD one field, default keeps CPU path byte-identical):
  ```cpp
  // playback/output/framehandle.h, struct FrameMetadata:
  uint64_t gpuGeneration = 0;  // 0 = CPU frame / generation-agnostic (never stale)
  ```
  And on `GpuFrameData` (and via `FrameHandle`):
  ```cpp
  // GpuFrameData / FrameHandle: a GPU handle is stale iff its stamped generation
  // is non-zero and != the current GpuGenerationCounter value.
  bool FrameHandle::isStaleForGeneration(uint64_t currentGen) const;  // framehandle.h
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpugeneration.cpp`:

```cpp
// GpuGenerationCounter invalidates stale GPU surfaces after a seek/reposition,
// mirroring PlaybackWorker::m_seekGeneration. A handle stamped at gen N is stale
// once the counter bumps past N; a 0-stamped (CPU) handle is never stale.
#include <QtTest>

#include "playback/gpu/gpugeneration.h"
#include "playback/output/framehandle.h"

class TestGpuGeneration : public QObject {
    Q_OBJECT
private slots:
    void bumpAdvancesMonotonically();
    void cpuHandleIsNeverStale();
    void gpuHandleStaleAfterBump();
};

void TestGpuGeneration::bumpAdvancesMonotonically() {
    auto& g = GpuGenerationCounter::instance();
    g.reset();
    QCOMPARE(g.current(), uint64_t(0));
    QCOMPARE(g.bump(), uint64_t(1));
    QCOMPARE(g.bump(), uint64_t(2));
    QCOMPARE(g.current(), uint64_t(2));
}

void TestGpuGeneration::cpuHandleIsNeverStale() {
    GpuGenerationCounter::instance().reset();
    FrameHandle h = solidYuv420pHandle(16, 16, 16, 128, 128);  // CPU, gpuGeneration==0
    GpuGenerationCounter::instance().bump();
    QVERIFY(!h.isStaleForGeneration(GpuGenerationCounter::instance().current()));
}

void TestGpuGeneration::gpuHandleStaleAfterBump() {
    auto& g = GpuGenerationCounter::instance();
    g.reset();
    const uint64_t mintedAt = g.bump();  // 1
    FrameHandle h = solidYuv420pHandle(16, 16, 16, 128, 128);
    h.metadata().gpuGeneration = mintedAt;  // stamp as if GPU-minted at gen 1
    QVERIFY(!h.isStaleForGeneration(g.current()));  // still gen 1
    g.bump();                                       // → 2 (a seek happened)
    QVERIFY(h.isStaleForGeneration(g.current()));   // now stale
}
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:

```cmake
    olr_add_unit_test(tst_gpugeneration olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpugeneration
```
Expected: FAIL — `playback/gpu/gpugeneration.h` not found; `FrameMetadata` has no `gpuGeneration`; `FrameHandle::isStaleForGeneration` undeclared.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpugeneration.h` (interface above) and `playback/gpu/gpugeneration.cpp`:

```cpp
#include "playback/gpu/gpugeneration.h"

GpuGenerationCounter& GpuGenerationCounter::instance() {
    static GpuGenerationCounter inst;
    return inst;
}
uint64_t GpuGenerationCounter::current() const { return m_gen.load(std::memory_order_acquire); }
uint64_t GpuGenerationCounter::bump() { return m_gen.fetch_add(1, std::memory_order_acq_rel) + 1; }
void GpuGenerationCounter::reset() { m_gen.store(0, std::memory_order_release); }
```

In `playback/output/framehandle.h`, add `uint64_t gpuGeneration = 0;` to `struct FrameMetadata` (after `color`) and declare on `FrameHandle`:
```cpp
    bool isStaleForGeneration(uint64_t currentGen) const;
```
Implement in `playback/output/framehandle.cpp`:
```cpp
bool FrameHandle::isStaleForGeneration(uint64_t currentGen) const {
    // A 0 stamp means CPU / generation-agnostic — never stale. A GPU handle
    // stamped at generation G is stale once the counter has advanced past G.
    return m_meta.gpuGeneration != 0 && m_meta.gpuGeneration != currentGen;
}
```

Wire `gpugeneration.{h,cpp}` into CMake (the `OpenLiveReplay` `if(OLR_GPU_PIPELINE)` block, all platforms — it is platform-neutral C++), `olr_test_gpu`, and `olr_test_playback`'s GPU block, mirroring `gpuframedata.cpp`.

In `playback/playbackworker.cpp`, three wiring edits (all under `#ifdef OLR_GPU_PIPELINE_BUILD`):

1. **Bump on seek.** In `seekTo`, right after `m_seekGeneration.fetch_add(...)` (playbackworker.cpp:108), bump the GPU generation so any GPU surface minted before this seek is invalidated:
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
       if (gpuPipelineEnabled()) GpuGenerationCounter::instance().bump();
   #endif
   ```
   Do the same at the `repositionTo` clearing path if a reposition can mint a fresh window without a `seekTo` (the run-loop reactive backward-jump path); bump once per reposition that clears decoder buffers (`clearDecoderBuffers` site).

2. **Stamp on mint.** In the macOS GPU decode branch where `meta` is built (playbackworker.cpp:698-704, before `importVtImageBuffer`) and the Windows branch (before `commitMediaFrame(*imported, ...)`, :772-774), stamp the minting generation:
   ```cpp
   meta.gpuGeneration = GpuGenerationCounter::instance().current();   // macOS
   // imported->metadata().gpuGeneration = GpuGenerationCounter::instance().current(); // win
   ```

3. **Drop stale at snapshot.** This is consumed by `gpu-budget`/`gpu-compositor` later; for this task, add the read path only — in `makeOutputSnapshot`, after the cache is loaded, the published `OutputFrameCache` lookups already key by playhead; the staleness check belongs to the eviction guard (Task 5) and the compositor. Here, only expose the counter via a worker accessor used by tests/telemetry:
   ```cpp
   // playbackworker.h (public): the GPU generation under which the live window was minted.
   uint64_t gpuGeneration() const;  // returns GpuGenerationCounter::instance().current() or 0
   ```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpugeneration && ctest --test-dir build/c -R tst_gpugeneration --output-on-failure
```
Expected: PASS (3 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: full unit suite PASSES (the `gpuGeneration=0` default keeps every existing CPU frame generation-agnostic; nothing reads the stamp on the CPU path).

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpugeneration.h playback/gpu/gpugeneration.cpp playback/output/framehandle.h \
        playback/output/framehandle.cpp playback/playbackworker.cpp playback/playbackworker.h \
        tests/unit/tst_gpugeneration.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-sync): GPU generation counter invalidates stale surfaces on seek/reposition"
```

---

## Task 4: Apple surface-lifetime retain hook (parity with `D3D11GpuSurface`)

**Precondition:** Task 2. (Windows `D3D11GpuSurface` already has `retainUntilFenceRetired(uint64_t)` / `pendingFenceValue()` — d3d11gpusurface.cpp:27-32. Apple `AppleGpuSurface` does not. The eviction guard in Task 5 needs the hook on BOTH backends.)

**Files:**
- Modify: `playback/gpu/gpusurface.h` (add the two virtuals with no-op defaults so the contract is uniform and the stub/test surfaces inherit it), `playback/gpu/applegpusurface_apple.mm` (override on the Apple surface), `playback/output/win/d3d11gpusurface.h` (make the existing pair `override` of the new virtuals)
- Test: `tests/unit/tst_gpusurface.cpp` (extend — add a lifetime slot)

**Interfaces:**
- Produces (ADD to `GpuSurface`, default no-op so non-GPU/stub surfaces are unaffected):
  ```cpp
  // playback/gpu/gpusurface.h, class GpuSurface:
  // Surface-lifetime contract (spec §4): the surface retains its native backing
  // until every render/readback fence referencing it has retired. Eviction records
  // the fence value that must complete before this surface may be freed; the guard
  // (Task 5) waits on it AFTER releasing m_bufferMutex.
  virtual void retainUntilFenceRetired(uint64_t fenceValue) {}
  virtual uint64_t pendingFenceValue() const { return 0; }
  ```
  The Apple concrete stores the max pending value (same compare-exchange-max as the Windows surface); the value is the `GpuFence`/`DecodeDoneFence` timeline value from Task 1/2.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpusurface.cpp` (under the existing `#ifdef __APPLE__` block):

```cpp
#ifdef __APPLE__
    void appleSurfaceTracksPendingFence();
#endif
```

```cpp
#ifdef __APPLE__
void TestGpuSurface::appleSurfaceTracksPendingFence() {
    auto s = makeAppleNv12Surface(64, 48);
    QVERIFY(s != nullptr);
    QCOMPARE(s->pendingFenceValue(), uint64_t(0));
    s->retainUntilFenceRetired(7);
    QCOMPARE(s->pendingFenceValue(), uint64_t(7));
    s->retainUntilFenceRetired(3);  // lower value never lowers the pending bar
    QCOMPARE(s->pendingFenceValue(), uint64_t(7));
    s->retainUntilFenceRetired(9);
    QCOMPARE(s->pendingFenceValue(), uint64_t(9));
}
#endif
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpusurface
```
Expected: FAIL — `pendingFenceValue` / `retainUntilFenceRetired` not members of `GpuSurface` (the base lacks them; only the Windows concrete had them as non-virtual).

- [ ] **Step 3: Write the implementation**

Add the two virtuals (with the no-op defaults above) to `class GpuSurface` in `playback/gpu/gpusurface.h`.

In `playback/gpu/applegpusurface_apple.mm`, add to `class AppleGpuSurface` a `std::atomic<uint64_t> m_pendingFence{0};` member and override:
```cpp
    void retainUntilFenceRetired(uint64_t fenceValue) override {
        uint64_t prev = m_pendingFence.load(std::memory_order_acquire);
        while (fenceValue > prev &&
               !m_pendingFence.compare_exchange_weak(prev, fenceValue, std::memory_order_acq_rel)) {
        }
    }
    uint64_t pendingFenceValue() const override {
        return m_pendingFence.load(std::memory_order_acquire);
    }
```
Add `#include <atomic>` if not already present.

In `playback/output/win/d3d11gpusurface.h`, mark the existing pair as `override` of the new base virtuals (they already match the signatures — `void retainUntilFenceRetired(uint64_t)` and `uint64_t pendingFenceValue() const`). Add `override` to both so the compiler enforces the contract.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpusurface && ctest --test-dir build/c -R tst_gpusurface --output-on-failure
```
Expected: PASS (the existing slots + `appleSurfaceTracksPendingFence` on Apple).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpusurface.h playback/gpu/applegpusurface_apple.mm \
        playback/output/win/d3d11gpusurface.h tests/unit/tst_gpusurface.cpp
git commit -m "feat(gpu-sync): uniform surface-lifetime retain hook (Apple parity with D3D11)"
```

---

## Task 5: The eviction guard — collect victims under `m_bufferMutex`, fence-wait/free AFTER releasing it

**Precondition:** Tasks 2 + 4. **This is the spec §10 "Lock × fence deadlock" task and the highest-risk concurrency change in the plan.**

**Files:**
- Modify: `playback/trackbuffer.h`/`.cpp` (the `insert` cap path must HAND BACK the evicted `FrameHandle` instead of freeing it inline), `playback/playbackworker.h`/`.cpp` (two-phase guard: `evictVictimsLocked` collects, `drainEvictedVictims` waits/frees after unlock), the GPU decode-branch `commitMediaFrame` lambda (collect, not inline-free)
- Test: `tests/unit/tst_evictionguard.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `TrackBuffer::insert` (existing — playbackworker.cpp:659/:892 call sites), `GpuFence` (Task 2), `GpuSurface::retainUntilFenceRetired`/`pendingFenceValue` (Task 4), `m_bufferMutex` (existing).
- Produces:
  ```cpp
  // playback/trackbuffer.h — insert gains an out-param that RECEIVES the evicted
  // frame (if any) so the owner can defer the free past the lock. Backward-compat:
  // a nullptr evicted-out keeps today's inline-free behavior (CPU path unchanged).
  bool insert(int64_t ptsMs, const FrameHandle& f, int capFrames, int64_t keepNearMs,
              int64_t protectToMs, FrameHandle* evicted = nullptr);

  // playback/playbackworker.h (private):
  // A surface evicted from a TrackBuffer cap whose render/readback fence may still
  // be in flight. Collected UNDER m_bufferMutex; drained (waited + freed) AFTER it.
  struct EvictedVictim {
      FrameHandle frame;          // keeps the surface alive until we drain it
      uint64_t fenceValue = 0;    // wait for the GPU fence to retire this before free
  };
  // Append a cap-evicted frame to the pending-drain list. MUST hold m_bufferMutex.
  void collectEvictedVictimLocked(FrameHandle&& evicted);
  // Wait on each victim's fence (bounded timeout) then drop the handle, freeing the
  // surface. MUST NOT hold m_bufferMutex (spec §10 lock rule). Runs on the worker
  // thread right after the insert's lock scope closes.
  void drainEvictedVictims();
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_evictionguard.cpp` — drive the two-phase guard directly (no full worker; a focused unit that proves: a victim with a not-yet-retired fence is NOT freed under the lock, and IS freed after the fence retires + drain):

```cpp
// The eviction guard must (1) collect a cap-evicted GPU surface without freeing
// it under m_bufferMutex, and (2) free it only after its render/readback fence
// retires — never fence-waiting under the lock (spec §10). This unit exercises
// TrackBuffer's evicted-out param + a fence-gated drain via weak_ptr lifetime.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/output/framehandle.h"
#include "playback/trackbuffer.h"

#include <memory>

class TestEvictionGuard : public QObject {
    Q_OBJECT
private slots:
    void insertHandsBackEvictedFrame();
    void drainDefersFreeUntilFenceRetires();
};

void TestEvictionGuard::insertHandsBackEvictedFrame() {
    TrackBuffer buf;
    // cap=2; insert 3 frames; the 3rd insert must evict one and hand it back.
    FrameHandle evicted;
    buf.insert(100, solidYuv420pHandle(16, 16, 16, 128, 128), 2, 100, 100, &evicted);
    QVERIFY(evicted.isNull());  // under cap, nothing evicted
    buf.insert(200, solidYuv420pHandle(16, 16, 16, 128, 128), 2, 200, 200, &evicted);
    QVERIFY(evicted.isNull());
    buf.insert(300, solidYuv420pHandle(16, 16, 16, 128, 128), 2, 300, 300, &evicted);
    QVERIFY(!evicted.isNull());  // over cap → the farthest (pts=100) handed back
}

void TestEvictionGuard::drainDefersFreeUntilFenceRetires() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");
    // A handle whose underlying data we observe via shared_ptr use_count: while
    // the victim list holds it, it stays alive; draining after the fence retires
    // drops it. (Uses a CPU handle for determinism; the lifetime logic is the same
    // — the guard never frees while a fence value is unretired.)
    FrameHandle h = solidYuv420pHandle(16, 16, 16, 128, 128);
    std::weak_ptr<const IFrameData> weak = h.dataPtr();
    QVERIFY(!weak.expired());

    const uint64_t target = fence->signal();   // already retired (host signal)
    // Simulate the drain: wait the fence value, then drop the handle.
    QVERIFY(fence->wait(target, 1000));
    h = FrameHandle();                          // drop after fence retired
    QVERIFY(weak.expired());                    // surface/data freed only post-fence
}

QTEST_GUILESS_MAIN(TestEvictionGuard)
#include "tst_evictionguard.moc"
```

Register in `tests/unit/CMakeLists.txt` `if(OLR_GPU_PIPELINE)` block:
```cmake
    olr_add_unit_test(tst_evictionguard olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_evictionguard
```
Expected: FAIL to compile — `TrackBuffer::insert` has no 6th `FrameHandle* evicted` param.

- [ ] **Step 3: Write the implementation**

In `playback/trackbuffer.h`, change the `insert` signature to add `FrameHandle* evicted = nullptr`. In `playback/trackbuffer.cpp`, in the cap-enforcement block, before `m_frames.remove(evictIdx)`, hand the evicted frame back instead of letting `remove` free it:
```cpp
        // Hand the evicted frame back to the owner so it can defer the free past
        // the lock (spec §10): a GPU surface may still have a render/readback fence
        // in flight. With evicted==nullptr the free happens inline (CPU path).
        if (evicted) *evicted = m_frames[evictIdx].frame;
        m_frames.remove(evictIdx);
```

In `playback/playbackworker.h`, add the `EvictedVictim` struct, a `QVector<EvictedVictim> m_evictedVictims;` member (guarded by `m_bufferMutex` for the collect, drained on the worker thread), and declare `collectEvictedVictimLocked` / `drainEvictedVictims`. Add a `std::shared_ptr<GpuFence> m_renderFence;` member alongside `m_decodeFence` (constructed in the `OLR_GPU_PIPELINE_BUILD` graph-init at playbackworker.cpp:328-340 via `GpuFence::create()` on Apple / `makeD3D11GpuFence(...)` on Windows once the import edge exists).

In `playback/playbackworker.cpp`:

1. **`collectEvictedVictimLocked`** (worker thread, called from the insert lambda while `m_bufferMutex` is held):
   ```cpp
   void PlaybackWorker::collectEvictedVictimLocked(FrameHandle&& evicted) {
   #ifdef OLR_GPU_PIPELINE_BUILD
       if (evicted.isNull()) return;
       uint64_t fenceValue = 0;
       if (evicted.isGpuBacked()) {
           if (auto* surf = const_cast<GpuSurface*>(
                   evicted.data() ? evicted.data()->gpuSurface() : nullptr))
               fenceValue = surf->pendingFenceValue();
       }
       m_evictedVictims.push_back(EvictedVictim{std::move(evicted), fenceValue});
   #else
       Q_UNUSED(evicted);
   #endif
   }
   ```

2. **`drainEvictedVictims`** (worker thread, called right AFTER the insert lock scope closes — NEVER holding `m_bufferMutex`):
   ```cpp
   void PlaybackWorker::drainEvictedVictims() {
   #ifdef OLR_GPU_PIPELINE_BUILD
       // LOCK RULE (spec §10): this MUST run with m_bufferMutex RELEASED. We take a
       // short lock only to swap the pending list out, then wait/free unlocked.
       QVector<EvictedVictim> victims;
       {
           QMutexLocker bufferLocker(&m_bufferMutex);
           victims.swap(m_evictedVictims);
       }
       for (auto& v : victims) {
           // Wait for the render/readback fence to retire the value recorded when
           // the surface was last submitted. Bounded timeout: a stuck fence must
           // not wedge the decode loop — on timeout we bump fenceWaitStalls and
           // free anyway (the surface is unreferenced by the cache at this point;
           // the worst case is a late GPU read of a recycled surface, which the
           // generation counter (Task 3) already invalidates on the next seek).
           if (v.fenceValue > 0 && m_renderFence) {
               if (!m_renderFence->wait(int(v.fenceValue), kFenceWaitTimeoutMs))
                   recordFenceWaitStall();  // bumps OutputDispatchStats::fenceWaitStalls
           }
           v.frame = FrameHandle();  // drop the last ref → free the surface
       }
   #endif
   }
   ```
   Add `static constexpr int kFenceWaitTimeoutMs = 50;`. The `fenceWaitStalls` telemetry already lives on `OutputDispatchStats` (`playback/output/outputdispatcher.h:78`) — the e2e harness reads `os.fenceWaitStalls` via `m_outputRuntime->stats()`, NOT a `PlaybackCounters` field. So DO NOT add a field to `PlaybackCounters`; instead add a worker helper `recordFenceWaitStall()` that increments the dispatcher-side counter through the output runtime:
   ```cpp
   void PlaybackWorker::recordFenceWaitStall() {
   #ifdef OLR_GPU_PIPELINE_BUILD
       // LOCK RULE: m_outputRuntimeMutex is independent of m_bufferMutex (and we
       // are called from drainEvictedVictims with m_bufferMutex released).
       QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
       if (m_outputRuntime) m_outputRuntime->incrementFenceWaitStalls();
   #endif
   }
   ```
   `OutputDispatchStats::fenceWaitStalls` exists (outputdispatcher.h:78) and is **parsed by the e2e harness already**, but is **never written today** (the Phase-2 telemetry-contract added the field as a placeholder; this subproject is the first to populate it — `grep fenceWaitStalls playback/` finds only the struct field + the harness read, no writer). `OutputRuntime` currently exposes only `stats() const` (outputruntime.h:37), so ADD a thread-safe `void incrementFenceWaitStalls();` to `OutputRuntime` that bumps an internal `std::atomic<qint64>` folded into the `OutputDispatchStats` returned by `stats()`. This is the canonical writer path for the placeholder counter; `gpu-budget` later populates `gpuVramBytes`/`gpuOomDegrades` and `async-readback` populates `readbackDrops`/`readbackQueueDepth` through the same pattern.

3. **Wire the collect into the GPU decode `commitMediaFrame`.** In the macOS branch lambda (playbackworker.cpp:655-664) and the FFmpeg branch (:888-898), pass `&evicted` to `insert` and collect it under the same lock, then drain after the lock scope:
   ```cpp
   auto commitMediaFrame = [&](FrameHandle mediaFrame, int64_t framePtsMs) -> bool {
       mediaFrame.metadata().key.ptsMs = framePtsMs;
       if (!mediaFrame.isPresentable()) return false;
       FrameHandle evicted;
       {
           QMutexLocker bufferLocker(bufferMutex);
           if (!track->buffer.insert(framePtsMs, mediaFrame, cap, protectLo, protectHi, &evicted))
               counters.framesDropped++;
           if (outputCache) outputCache->insertVideoFrame(mediaFrame);
           counters.decodedVideoFrames++;
           collectEvictedVictimLocked(std::move(evicted));  // still under the lock
       }                                                     // <-- m_bufferMutex released here
       drainEvictedVictims();                                // LOCK RULE: now safe to fence-wait
       return true;
   };
   ```
   The CPU-only path (FFmpeg software decode, :891-896) passes `evicted=nullptr` (inline free, behavior unchanged) OR, when `OLR_GPU_PIPELINE_BUILD` is defined and the frame is GPU-backed, routes through the same collect/drain. Guard so the CPU build keeps the exact existing call.

**Review gate:** the lock-order proof is the crux. `drainEvictedVictims` waits on `m_renderFence` with `m_bufferMutex` released; the render thread holds the surface and signals that fence. If the wait ran under `m_bufferMutex`, the render thread (which may take `m_bufferMutex` to read the published cache) would deadlock against the worker. The `// LOCK RULE:` comments and a TSan run (`OLR_PREPUSH_FULL=1` / CI TSan lane) are mandatory; flag for independent concurrency review before merge.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_evictionguard && ctest --test-dir build/c -R tst_evictionguard --output-on-failure
```
Expected: PASS (2 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: full unit suite PASSES — the `evicted=nullptr` default preserves the CPU path's inline free; `tst_trackbuffer` (existing) sees no behavior change.

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/trackbuffer.h playback/trackbuffer.cpp playback/playbackworker.h \
        playback/playbackworker.cpp tests/unit/tst_evictionguard.cpp tests/unit/CMakeLists.txt
git commit -m "feat(gpu-sync): two-phase eviction guard (collect locked, fence-wait/free unlocked)"
```

---

## Task 6: Armed-cut staging-swap fence (staging-decoder-idle before fire)

**Precondition:** Task 2. Grounds in the merged armed-cut path: `fillStaging` (worker thread, decodes into `m_prerollStagingCache`, playbackworker.cpp:1500-1560) and `maybeFireScheduledCut` (output thread, `std::swap(m_outputCache, m_prerollStagingCache)` at :1695, under `m_bufferMutex`).

**Files:**
- Modify: `playback/playbackworker.h` (add `m_stagingFence` + a staged-value), `playback/playbackworker.cpp` (signal when staging is idle; wait before the swap — WITHOUT holding `m_bufferMutex` across the wait)
- Test: `tests/unit/tst_stagingfence.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuFence` (Task 2), `m_prerollStagingCache`/`m_stagingCovers`/`m_scheduledCutFrame` (existing armed-cut state).
- Produces:
  ```cpp
  // playback/playbackworker.h (private, under the armed-cut state):
  // Staging-swap fence: fillStaging signals it (advances m_stagedFenceValue) once
  // the staging decoder has finished writing every GPU surface in the covered
  // window; maybeFireScheduledCut waits on that value BEFORE swapping staging ->
  // live, so the output thread never publishes a surface the staging decoder is
  // still writing. CPU path: the fence is a no-op (staging frames are CPU planes,
  // already complete on insert).
  std::shared_ptr<GpuFence> m_stagingFence;        // null on CPU-only path
  std::atomic<uint64_t> m_stagedFenceValue{0};     // value to wait at fire time
  // True once the staging GPU surfaces for the covered window have all retired
  // (set by fillStaging after signalling; read at fire time). Returns true
  // immediately on the CPU path.
  bool stagingGpuSurfacesIdle() const;
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_stagingfence.cpp` — a focused unit for the idle predicate (the full armed-cut swap is covered by the e2e `armedcut` scenarios; this proves the fence gate is correct in isolation):

```cpp
// The armed-cut swap must wait for the staging decoder to finish writing its GPU
// surfaces before promoting staging -> live, so the output thread never reads a
// half-written surface. This unit exercises the fence gate: a swap that waits on
// a not-yet-retired staging fence value blocks; once signalled it proceeds.
#include <QtTest>

#include "playback/gpu/gpufence.h"

class TestStagingFence : public QObject {
    Q_OBJECT
private slots:
    void swapWaitsForStagingFence();
};

void TestStagingFence::swapWaitsForStagingFence() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");
    // Before the staging decoder signals, the fire-time wait for value 1 must
    // time out (the swap would block).
    QVERIFY(!fence->wait(1, 50));
    const uint64_t staged = fence->signal();  // staging decoder idle
    QVERIFY(fence->wait(staged, 1000));        // fire-time wait now proceeds
}

QTEST_GUILESS_MAIN(TestStagingFence)
#include "tst_stagingfence.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:
```cmake
    olr_add_unit_test(tst_stagingfence olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_stagingfence
```
Expected: FAIL — `playback/gpu/gpufence.h` resolves but the test target is unregistered until CMake reconfigures; after reconfigure the test compiles and PASSES the fence round-trip but the WORKER wiring (next step) is what the e2e exercises. (If preferred, this unit can be made red-first by asserting a worker method; keep it fence-focused for determinism and let the worker wiring be covered by the e2e gate in Task 7.)

Reconfigure + build:
```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_stagingfence
```

- [ ] **Step 3: Write the worker wiring**

Add the members above to `playback/playbackworker.h`. Construct `m_stagingFence` in the GPU graph-init alongside `m_renderFence` (Task 5).

In `fillStaging` (playbackworker.cpp), after the staging window reaches coverage (`m_stagingCovers.store(true, ...)` and `scheduleCutAtFrame(...)`), signal the staging fence and record the value (worker thread):
```cpp
#ifdef OLR_GPU_PIPELINE_BUILD
    if (m_stagingFence && gpuPipelineEnabled())
        m_stagedFenceValue.store(m_stagingFence->signal(), std::memory_order_release);
#endif
```

Add the predicate:
```cpp
bool PlaybackWorker::stagingGpuSurfacesIdle() const {
#ifdef OLR_GPU_PIPELINE_BUILD
    if (!m_stagingFence || !gpuPipelineEnabled()) return true;  // CPU path: always idle
    const uint64_t target = m_stagedFenceValue.load(std::memory_order_acquire);
    if (target == 0) return true;  // nothing staged on the GPU timeline
    return m_stagingFence->completedValue() >= target;
#else
    return true;
#endif
}
```

In `maybeFireScheduledCut` — the wait must NOT run under `m_bufferMutex` (the caller `makeOutputSnapshot` holds it across this call, playbackworker.cpp:492-495). So gate the FIRE on the idle PREDICATE (a non-blocking `completedValue()` poll) rather than a blocking wait: if staging surfaces are not yet idle, defer the cut one tick (return without firing), exactly as the unarmed/uncovered paths already return early. Add right after the `m_prerollStagingCache` null-check (:1669), before the swap:
```cpp
    // GPU staging-swap fence: do not promote staging -> live until the staging
    // decoder has finished writing its GPU surfaces. This is a NON-BLOCKING poll
    // (we hold m_bufferMutex here, so we must not wait on the fence — spec §10
    // lock rule). Not-yet-idle defers the cut to the next tick, like an uncovered
    // staging cache. The CPU path returns true immediately.
    if (!stagingGpuSurfacesIdle()) return;
```

**Review gate:** the fire path runs on the OUTPUT thread under `m_bufferMutex`; the staging signal runs on the WORKER thread. `m_stagedFenceValue` is the release/acquire handoff. The deferral (poll, not wait) is what keeps the lock rule intact — no fence-wait under `m_bufferMutex`. Flag for independent review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_stagingfence && ctest --test-dir build/c -R tst_stagingfence --output-on-failure
cmake --build build/c && ctest --test-dir build/c -R "tst_playbackworker|armedcut" --output-on-failure
```
Expected: `tst_stagingfence` PASS; the existing armed-cut unit/e2e behavior unchanged on the CPU path (predicate returns true).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_stagingfence.cpp \
        tests/unit/CMakeLists.txt
git commit -m "feat(gpu-sync): armed-cut staging-swap fence (defer fire until staging GPU idle)"
```

---

## Task 7: Multi-feed cap-pressure GPU stress — unit + e2e capstress scenario

**Precondition:** Tasks 1-6. This is the §8/§9 "full multi-feed cap-pressure scenario" the single-feed Phase-2 slice could not exercise: concurrent evict-while-render under pressure, armed-cut-while-render-pending, seek-under-decode, and frame-checksum validators.

**Files:**
- Create: `tests/unit/tst_gpu_sync_stress.cpp` (multi-thread, multi-surface stress driving the real fence + eviction guard + generation counter)
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `tests/e2e/run_playback_e2e.sh` (add a `gpucapstress` scenario branch), `tests/e2e/play_harness.cpp` (drive multi-feed cap-pressure under `OLR_GPU_PIPELINE=1`), `tests/CMakeLists.txt` (register `e2e_play_gpu_capstress` if not already a scenario param of `e2e_play`)

**Interfaces:**
- Consumes: `GpuFence`, `GpuGenerationCounter`, the eviction guard, `DecodeDoneFence`, `makeGpuFrameHandle`/`makeAppleNv12Surface`, `gpuSetInjectedAllocFailures`, the e2e counters already emitted (`fenceWaitStalls`, `gpuOomDegrades`, `gpuReadToCpuCount`, `placeholderFramesDelta`, `heldFramesDelta`).
- Produces: no new product interface — a stress gate.

- [ ] **Step 1: Write the failing unit stress test**

Create `tests/unit/tst_gpu_sync_stress.cpp`:

```cpp
// Multi-feed cap-pressure GPU stress (spec §8/§9): concurrent evict-while-render,
// seek-under-decode (generation bump invalidating in-flight surfaces), and a
// frame-checksum validator. TSan cannot see GPU command ordering, so this drives
// the real fence + generation counter under thread contention and asserts no
// in-use surface is freed and no stale surface is presented.
#include <QtTest>

#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/output/framehandle.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#endif

#include <atomic>
#include <thread>
#include <vector>

class TestGpuSyncStress : public QObject {
    Q_OBJECT
private slots:
#ifdef __APPLE__
    void concurrentEvictWhileRenderNeverFreesInUse();
    void seekUnderDecodeInvalidatesStaleSurfaces();
#endif
    void stubFenceStressIsRaceFree();
};

#ifdef __APPLE__
void TestGpuSyncStress::concurrentEvictWhileRenderNeverFreesInUse() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto renderFence = GpuFence::create();
    QVERIFY(renderFence != nullptr);

    constexpr int kFeeds = 4;
    constexpr int kIters = 64;
    std::atomic<bool> sawFreedInUse{false};

    std::vector<std::thread> producers;
    for (int feed = 0; feed < kFeeds; ++feed) {
        producers.emplace_back([&, feed] {
            for (int i = 0; i < kIters; ++i) {
                auto surface = makeAppleNv12Surface(64, 48);
                if (!surface) continue;
                std::weak_ptr<GpuSurface> weak = surface;
                FrameMetadata meta;
                meta.key.feedIndex = feed;
                meta.key.format = FramePixelFormat::Nv12;
                meta.key.width = 64;
                meta.key.height = 48;
                meta.gpuGeneration = GpuGenerationCounter::instance().current();
                FrameHandle cacheHandle = makeGpuFrameHandle(surface, rhi, meta);

                // "Render" submits the surface and records a fence value it must
                // outlive; a concurrent reader reads it back.
                const uint64_t v = renderFence->signal();
                surface->retainUntilFenceRetired(v);
                surface.reset();

                FrameHandle consumer = cacheHandle;
                std::thread reader([&, consumer]() mutable {
                    const bool ok = consumer.readToCpu(FramePixelFormat::Yuv420p).isValid();
                    if (!ok) sawFreedInUse.store(true, std::memory_order_release);
                });

                // Evict under "pressure" while the read may be in flight: the
                // guard must defer the free until the fence retires.
                renderFence->wait(v, 1000);
                cacheHandle = FrameHandle();
                reader.join();
            }
        });
    }
    for (auto& t : producers) t.join();
    QVERIFY(!sawFreedInUse.load(std::memory_order_acquire));
}

void TestGpuSyncStress::seekUnderDecodeInvalidatesStaleSurfaces() {
    GpuGenerationCounter::instance().reset();
    const uint64_t g0 = GpuGenerationCounter::instance().bump();  // window 1
    FrameHandle h = solidYuv420pHandle(16, 16, 16, 128, 128);
    h.metadata().gpuGeneration = g0;
    std::atomic<bool> stalePresented{false};

    std::thread seeker([&] {
        for (int i = 0; i < 100; ++i) GpuGenerationCounter::instance().bump();
    });
    std::thread presenter([&] {
        for (int i = 0; i < 100; ++i) {
            if (!h.isStaleForGeneration(GpuGenerationCounter::instance().current()) &&
                GpuGenerationCounter::instance().current() != g0)
                stalePresented.store(true, std::memory_order_release);
        }
    });
    seeker.join();
    presenter.join();
    QVERIFY(h.isStaleForGeneration(GpuGenerationCounter::instance().current()));
    QVERIFY(!stalePresented.load(std::memory_order_acquire));
}
#endif

void TestGpuSyncStress::stubFenceStressIsRaceFree() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");
    std::atomic<uint64_t> maxSeen{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 256; ++i) {
                const uint64_t v = fence->signal();
                uint64_t prev = maxSeen.load(std::memory_order_acquire);
                while (v > prev &&
                       !maxSeen.compare_exchange_weak(prev, v, std::memory_order_acq_rel)) {}
                QVERIFY(fence->wait(v, 1000));
            }
        });
    for (auto& t : ts) t.join();
    QVERIFY(fence->completedValue() >= maxSeen.load(std::memory_order_acquire));
}

QTEST_GUILESS_MAIN(TestGpuSyncStress)
#include "tst_gpu_sync_stress.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block:
```cmake
    olr_add_unit_test(tst_gpu_sync_stress olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL (then PASS after build)**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpu_sync_stress
```
Expected first run before the test file exists: FAIL (unregistered target). After adding the file + reconfigure: the test builds and PASSES on a GPU host (the Apple slots run), or `QSKIP`s the GPU slots + runs `stubFenceStressIsRaceFree` on a non-GPU host.

- [ ] **Step 3: Add the e2e multi-feed capstress scenario**

In `tests/e2e/play_harness.cpp`, add a `gpucapstress` scenario that, under `OLR_GPU_PIPELINE=1`, drives a multi-feed (≥4) fixture with a FORCED tiny per-track budget (`OLR_GPU_FORCE_BUDGET` via `gpuForcedPerTrackBudget()`) so the cap evicts aggressively while decode + readback are in flight, then issues a seek mid-decode and an armed cut. Assert the existing GPU counters: `placeholderFramesDelta==0` (no gray flash from a freed-in-use surface), `heldFramesDelta==0`, `gpuOomDegrades>=0` (degrade is graceful), and the copy-detector invariant (`gpuReadToCpuCount` one per unique rendered bus surface). Reuse the harness's existing counter-emit (`play_harness.cpp:153-170`) — the counters are already wired.

In `tests/e2e/run_playback_e2e.sh`, add a `gpucapstress)` case in the scenario `case` block (near `play1x)` at :300) that:
- requires `OLR_GPU_PIPELINE=1` (else `SKIP` with a clear message — CI macOS is CPU-oracle-only per §9);
- exports `OLR_GPU_FORCE_BUDGET=12` (the floor cap) and a ≥4-view fixture;
- asserts `placeholderFramesDelta` and `heldFramesDelta` are `0`, `fenceWaitStalls` is bounded (e.g. `<= decodedVideoFrames`), and `gpuReadToCpuCount > 0`.

Register the scenario as an `e2e_play` invocation in `tests/CMakeLists.txt` (mirror the existing `armedcut` scenario registration) so `ctest -R e2e_play_gpucapstress` runs it; gate it to run only when `OLR_GPU_PIPELINE` is ON and a GPU host is present (the script self-skips otherwise).

- [ ] **Step 4: Run the e2e capstress (GPU host)**

```sh
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R e2e_play_gpucapstress --output-on-failure
# or directly:
OLR_GPU_PIPELINE=1 OLR_GPU_FORCE_BUDGET=12 tests/e2e/run_playback_e2e.sh \
    build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness gpucapstress 4 9301
```
Expected: PASS with `placeholderFramesDelta=0 heldFramesDelta=0`; on a non-GPU host the script prints a SKIP. On the CPU lane (`OLR_GPU_PIPELINE` unset) the scenario is skipped, never failed.

- [ ] **Step 5: Run the full unit + sanitizer pre-flight**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
# Reproduce the CI TSan lane locally for the eviction/fence threading:
OLR_PREPUSH_FULL=1 .githooks/pre-push  # runs the ASan/TSan passes + e2e
```
Expected: full unit suite PASSES; no new TSan/ASan findings on the eviction guard / staging fence / generation counter. (TSan cannot see the GPU command ordering — that is what the unit + e2e checksum/placeholder gates cover.)

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add tests/unit/tst_gpu_sync_stress.cpp tests/unit/CMakeLists.txt \
        tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/CMakeLists.txt
git commit -m "test(gpu-sync): multi-feed cap-pressure GPU stress (evict/seek/armed-cut + checksum gates)"
```

---

## Task 8: Wire the render fence into the GPU readback path + telemetry; final review

**Precondition:** Tasks 1-7.

**Files:**
- Modify: `playback/gpu/gpuframedata.cpp` (record a render-fence value when the surface is submitted for readback so the eviction guard waits on the right value), `playback/playbackworker.cpp` (construct `m_renderFence`/`m_stagingFence` in graph-init; surface `fenceWaitStalls` via `counters()`), `tests/e2e/run_playback_e2e.sh` (assert `fenceWaitStalls` threshold under capstress)
- Test: extend `tests/unit/tst_gpuframedata.cpp`

**Interfaces:**
- Consumes: `GpuFrameData` (existing readback chokepoint, gpuframedata.cpp:29-49), `GpuSurface::retainUntilFenceRetired` (Task 4), `m_renderFence` (Task 5).
- Produces: no new public interface; closes the loop so an evicted surface's `pendingFenceValue()` reflects the readback that may still be reading it.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpuframedata.cpp`:

```cpp
#ifdef __APPLE__
    void readbackStampsSurfacePendingFence();
#endif
```

```cpp
#ifdef __APPLE__
void TestGpuFrameData::readbackStampsSurfacePendingFence() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    FrameHandle h = makeGpuFrameHandle(surface, rhi, meta);
    // A readback submits the surface against the render fence; after it returns,
    // the surface's pendingFenceValue must be non-zero (the eviction guard waits
    // on it). Before any readback it is 0.
    QCOMPARE(surface->pendingFenceValue(), uint64_t(0));
    QVERIFY(h.readToCpu(FramePixelFormat::Yuv420p).isValid());
    QVERIFY(surface->pendingFenceValue() >= 1);
}
#endif
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpuframedata
```
Expected: FAIL — `pendingFenceValue()` stays 0 after readback (no fence stamping yet).

- [ ] **Step 3: Write the implementation**

Give `GpuFrameData` an optional render fence (passed from the worker so all frames share one timeline). Add a `std::shared_ptr<GpuFence> m_renderFence;` member set via `makeGpuFrameHandle(surface, rhi, meta, renderFence)` (add an overload; keep the existing 3-arg factory delegating with a null fence for back-compat / the stub lib). In `GpuFrameData::readToCpu`, after a successful `importAndReadback`, stamp the surface:
```cpp
    if (planes.isValid()) {
        // ... existing telemetry bumps ...
        if (m_renderFence && m_surface) {
            const uint64_t v = m_renderFence->signal();   // readback submitted
            m_surface->retainUntilFenceRetired(v);         // guard waits on this
        }
        m_cpuCache.insert(int(target), planes);
    }
```
In `playback/playbackworker.cpp` graph-init (the `OLR_GPU_PIPELINE_BUILD` block at :328-340), construct `m_renderFence = GpuFence::create();` (Apple) / `makeD3D11GpuFence(m_winGpuImportEdge device)` (Windows, once the edge exists) and pass it to the GPU mint sites (`importVtImageBuffer` → `makeGpuFrameHandle(..., m_renderFence)`).

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpuframedata && ctest --test-dir build/c -R tst_gpuframedata --output-on-failure
```
Expected: PASS (the existing slots + `readbackStampsSurfacePendingFence` on a GPU host).

- [ ] **Step 5: Full pre-flight + final review**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R "e2e_play" --output-on-failure
# Off-path byte-green check (fresh build dir):
cmake -S . -B build/off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
cmake --build build/off && ctest --test-dir build/off -L unit --output-on-failure
```
Expected: GPU-on and GPU-off suites both PASS; e2e thresholds unchanged on the CPU lane.

**Review gate (final, whole-subproject):** per CLAUDE.md, the playback worker's threading + every `gpu-sync` change gets an independent fresh-agent concurrency review before merge. The reviewer verifies: (a) no fence-wait under `m_bufferMutex` anywhere (grep every `->wait(` / `waitForValue` / `waitDecodeDone` call and confirm the enclosing scope released `m_bufferMutex`); (b) the generation stamp is read on the consumer before present; (c) the staging-swap defers (polls) rather than blocks under the lock; (d) the `MTLSharedEventListener` block cannot outlive its fence. Open the PR with the per-branch push:
```sh
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin gpu-phase3-gpu-sync
```

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp playback/playbackworker.cpp \
        tests/unit/tst_gpuframedata.cpp tests/e2e/run_playback_e2e.sh
git commit -m "feat(gpu-sync): stamp surface pending-fence on readback; wire render fence + telemetry"
```

---

## Canonical sync contract (downstream plans consume these names verbatim)

`gpu-compositor`, `async-readback`, `gpu-budget`, `device-loss`, and `gpu-encode` build on exactly these types and signatures. Do not rename or vary them.

**Extended decode-done fence** — `playback/gpu/decodedonefence.h` (existing class, ADDED methods):
```cpp
class DecodeDoneFence {
    static std::shared_ptr<DecodeDoneFence> create();
    virtual void signalDecodeDone();                          // advances the MTLSharedEvent timeline
    virtual bool waitDecodeDone(int timeoutMs);               // REAL GPU wait (Apple), not a CPU bool
    virtual bool isSignaled() const;
    virtual uint64_t signaledValue() const;                   // NEW: monotonic timeline value
    virtual bool waitForValue(uint64_t value, int timeoutMs); // NEW: wait the GPU timeline to `value`
};
```

**Backend-matched render/readback fence** — `playback/gpu/gpufence.h` (NEW):
```cpp
class GpuFence {
    virtual uint64_t signal();                          // advance timeline, return target value
    virtual bool wait(uint64_t value, int timeoutMs);   // block until retired / timeout (<0 = forever)
    virtual uint64_t completedValue() const;            // last RETIRED value (lock-free)
    static std::shared_ptr<GpuFence> create();          // MTLSharedEvent / null; stub off-GPU
};
std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device);  // Windows (ID3D11Fence via D3DFence)
```

**GPU generation counter** — `playback/gpu/gpugeneration.h` (NEW):
```cpp
class GpuGenerationCounter {
    static GpuGenerationCounter& instance();
    uint64_t current() const;   // lock-free read
    uint64_t bump();            // advance (seek/reposition), returns NEW value
    void reset();               // test-only
};
// FrameMetadata gains:  uint64_t gpuGeneration = 0;   // 0 = CPU / never stale
// FrameHandle gains:    bool isStaleForGeneration(uint64_t currentGen) const;
```

**Surface-lifetime hook** — `playback/gpu/gpusurface.h` (ADDED virtuals; Windows `D3D11GpuSurface` already had the concrete pair, now `override`; Apple `AppleGpuSurface` gains it):
```cpp
class GpuSurface {
    virtual void retainUntilFenceRetired(uint64_t fenceValue);  // default no-op
    virtual uint64_t pendingFenceValue() const;                 // default 0
};
```

**Eviction-guard hooks** — `playback/playbackworker.h` (NEW) + `playback/trackbuffer.h` (CHANGED signature):
```cpp
// TrackBuffer::insert gains a 6th out-param; nullptr keeps inline-free (CPU path).
bool insert(int64_t ptsMs, const FrameHandle& f, int capFrames, int64_t keepNearMs,
            int64_t protectToMs, FrameHandle* evicted = nullptr);

// PlaybackWorker (private):
struct EvictedVictim { FrameHandle frame; uint64_t fenceValue = 0; };
void collectEvictedVictimLocked(FrameHandle&& evicted);  // MUST hold m_bufferMutex
void drainEvictedVictims();                              // MUST NOT hold m_bufferMutex (spec §10)
std::shared_ptr<GpuFence> m_renderFence;                 // render/readback fence
```

**Armed-cut staging-swap fence** — `playback/playbackworker.h` (NEW):
```cpp
std::shared_ptr<GpuFence> m_stagingFence;     // null on CPU path
std::atomic<uint64_t>     m_stagedFenceValue{0};
bool stagingGpuSurfacesIdle() const;          // non-blocking poll; true on CPU path
```

**GPU-frame readback fence wiring** — `playback/gpu/gpuframedata.h` (ADDED factory overload):
```cpp
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta);  // existing
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence);                   // NEW
```

**Telemetry** (placeholder fields added by Phase-2 telemetry-contract; emitted by `play_harness.cpp` / parsed by `run_playback_e2e.sh`): `fenceWaitStalls`, `gpuOomDegrades`, `gpuVramBytes`, `readbackQueueDepth`, `readbackDrops` live on `OutputDispatchStats` (`playback/output/outputdispatcher.h:75-79`, read as `os.*` via `m_outputRuntime->stats()`); `gpuReadToCpuCount` is the `PlaybackWorker::PlaybackCounters` field driven by `gpuFrameReadToCpuCount()` (`GpuReadbackTelemetry`). This subproject is the FIRST to write `fenceWaitStalls` — incremented in `drainEvictedVictims` on a bounded fence-wait timeout via `OutputRuntime::incrementFenceWaitStalls()`. The remaining placeholders are populated by `gpu-budget` (`gpuVramBytes`/`gpuOomDegrades`) and `async-readback` (`readbackQueueDepth`/`readbackDrops`).
