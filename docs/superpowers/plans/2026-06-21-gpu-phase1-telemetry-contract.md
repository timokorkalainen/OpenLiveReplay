# Telemetry Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Extend the playback telemetry/harness counter contract with the five GPU-pipeline gate counters — VRAM occupancy, readback queue depth/drops, fence-wait stalls, GPU-OOM-degrade count, and a copy-on-GPU-path detector (one GPU-backed `readToCpu()` per unique rendered bus surface) — wired through `OutputDispatchStats` → `play_harness` → `run_playback_e2e.sh`, all reading inert/zero on the CPU-only Phase-1 path and ready for Phase-2 GPU wiring.

**Architecture:** New scalar fields are added to `OutputDispatchStats` (playback/output/outputdispatcher.h) and a process-wide `GpuReadbackTelemetry` singleton counts GPU-backed `readToCpu()` calls keyed by rendered-surface identity. The dispatcher samples the readback/copy counters into its stats each tick; `PlaybackWorker::outputStats()` already returns the dispatcher's `OutputDispatchStats`, so `play_harness` reads the new fields and prints them on the `COUNTERS` line, and `run_playback_e2e.sh` parses + asserts them per scenario. On the CPU path every field stays zero — `CpuFrameData::readToCpu()` is not GPU-backed, so the detector never increments, and no GPU subsystem exists yet to push VRAM/queue/fence/OOM numbers.

**Tech Stack:** C++17, Qt 6 (Core/Test), Qt Test (`QTEST_GUILESS_MAIN`, headless via `QT_QPA_PLATFORM=offscreen`), CMake + Ninja, POSIX sh (the e2e driver runs under macOS bash 3.2).

## Global Constraints

- **Keystone-first.** This subproject (`telemetry-contract`, §6) depends on `frame-handle` (the keystone) having merged: it consumes `FrameHandle`, `IFrameData`, `CpuFrameData`, `FramePixelFormat`, `FramePayloadKey`, and `readToCpu()` exactly as the keystone defines them (canonical contract). Do not redefine or fork those types; include their headers (`playback/output/framehandle.h`, `playback/output/framepixelformat.h`).
- **The CPU path stays default and is the permanent correctness reference + fallback.** Every counter added here reads inert/zero on the CPU-only Phase-1 pipeline. No behavior changes to the existing decode/composite/dispatch path — only additive scalar fields and a passive counter.
- **Everything behind flags / additive-only.** New `OutputDispatchStats` fields default to `0`; new `COUNTERS` tokens are appended (never reorder/remove existing tokens); new e2e gates assert `== 0` on the CPU path so they pass today and become live when Phase-2 GPU code starts incrementing.
- **No throwaway prototypes.** The `GpuReadbackTelemetry` counter, the surface-uniqueness key, and the e2e gates are production artifacts that stay in the tree and become the Phase-2 GPU gates verbatim.
- **Existing counters keep their exact meaning.** `placeholderFramesDelta`, `heldFramesDelta`, `maxClockDivergenceMs`, `decodedVideoFrames`, `stagingVideoFramesDecoded` are unchanged. New counters are strictly additional.
- **Public-repo professionalism.** Code, comments, and commit messages are self-contained and describe the present design; no internal notes or references to private history.
- **Format changed lines only.** Some engine files are hand-written Allman; run `git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main -- '*.cpp' '*.h'` and stage only the changed-line reformat. Do not reformat whole files.
- **Build (from the worktree root** `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/gpu-phase0-2-plans`**):** configure once with `cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build a target with `cmake --build build/c --target <name>`; run a unit test with `ctest --test-dir build/c -R <name> --output-on-failure`; run the full unit label with `ctest --test-dir build/c -L unit --output-on-failure`. Use a fresh `build/` dir when switching configurations.

---

## File Structure

- **Create** `playback/output/gpureadbacktelemetry.h` / `gpureadbacktelemetry.cpp` — the process-wide copy-on-GPU-path detector (count GPU-backed `readToCpu()` calls; assert one per unique rendered bus surface).
- **Modify** `playback/output/outputdispatcher.h` — add the five new scalar fields to `OutputDispatchStats` + a `setGpuTelemetrySource`/sampling hook.
- **Modify** `playback/output/outputdispatcher.cpp` — sample the GPU telemetry into `m_stats` each `dispatchTick`; register the per-tick rendered-bus surfaces with the detector.
- **Modify** `tests/e2e/play_harness.cpp` — print the five new counters on the `COUNTERS` line.
- **Modify** `tests/e2e/run_playback_e2e.sh` — parse + default the five new counters; assert `== 0` on `play1x` (and globally) for the CPU path.
- **Create** tests: `tests/unit/tst_gpureadbacktelemetry.cpp`, `tests/unit/tst_outputdispatch_gpustats.cpp`.
- **Modify** `tests/unit/CMakeLists.txt` — register the two new unit tests.
- **Modify** `CMakeLists.txt` — add `gpureadbacktelemetry.cpp` to the playback library source list.

---

## Task 1: `GpuReadbackTelemetry` counter — the copy-on-GPU-path detector

**Files:**
- Create: `playback/output/gpureadbacktelemetry.h`, `playback/output/gpureadbacktelemetry.cpp`
- Test: `tests/unit/tst_gpureadbacktelemetry.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `FramePixelFormat` (`playback/output/framepixelformat.h`, from the `frame-handle` keystone): `enum class FramePixelFormat { Nv12, Yuv420p, Rgba8 };`.
- Produces:
  ```cpp
  // playback/output/gpureadbacktelemetry.h
  // Identity of a rendered bus surface for the copy-on-GPU-path invariant: a
  // GPU-backed readToCpu() is permitted ONCE per (busKey, outputFrameIndex,
  // requested CPU format). Two CPU sinks on the same bus + format share one copy.
  struct GpuReadbackSurfaceKey {
      quint32 busKey = 0;              // OutputBusId stable key (kind<<16 | index)
      qint64  outputFrameIndex = -1;   // the rendered surface's frame index
      FramePixelFormat format = FramePixelFormat::Yuv420p; // the readback target
      bool operator==(const GpuReadbackSurfaceKey& o) const;
  };

  struct GpuReadbackTelemetrySnapshot {
      qint64 gpuReadbacks = 0;         // total GPU-backed readToCpu() calls observed
      qint64 uniqueSurfaces = 0;       // distinct (busKey,frameIndex,format) keys
      qint64 redundantReadbacks = 0;   // gpuReadbacks - uniqueSurfaces (>0 == bug)
  };

  // Process-wide passive accumulator. GPU-backed IFrameData::readToCpu()
  // implementations (Phase 2) call recordGpuReadback() with the surface key the
  // copy is for; CPU-backed frames never call it, so the snapshot stays zero on
  // the Phase-1 CPU path. recordSurface() registers a rendered bus surface so
  // uniqueSurfaces tracks the denominator of the one-copy-per-surface invariant.
  class GpuReadbackTelemetry {
  public:
      static GpuReadbackTelemetry& instance();
      void recordGpuReadback(const GpuReadbackSurfaceKey& key);
      void recordSurface(const GpuReadbackSurfaceKey& key);
      GpuReadbackTelemetrySnapshot snapshot() const;
      void reset();
  private:
      GpuReadbackTelemetry() = default;
      mutable QMutex m_mutex;
      qint64 m_gpuReadbacks = 0;
      QSet<GpuReadbackSurfaceKey> m_surfaces;
      QSet<GpuReadbackSurfaceKey> m_readbackKeys;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpureadbacktelemetry.cpp`:

```cpp
// Unit tests for GpuReadbackTelemetry — the copy-on-GPU-path detector that
// enforces "one GPU-backed readToCpu() per unique rendered bus surface". On the
// Phase-1 CPU path nothing calls recordGpuReadback(), so the snapshot is zero;
// these tests exercise the accumulator directly to pin the Phase-2 invariant.
#include <QtTest>

#include "playback/output/gpureadbacktelemetry.h"

class TestGpuReadbackTelemetry : public QObject {
    Q_OBJECT
private slots:
    void init() { GpuReadbackTelemetry::instance().reset(); }

    void freshSnapshotIsZero();
    void oneReadbackPerSurfaceIsNotRedundant();
    void twoReadbacksForSameSurfaceAreRedundant();
    void distinctFormatsAreDistinctSurfaces();
};

void TestGpuReadbackTelemetry::freshSnapshotIsZero() {
    const auto s = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(s.gpuReadbacks, qint64(0));
    QCOMPARE(s.uniqueSurfaces, qint64(0));
    QCOMPARE(s.redundantReadbacks, qint64(0));
}

void TestGpuReadbackTelemetry::oneReadbackPerSurfaceIsNotRedundant() {
    auto& t = GpuReadbackTelemetry::instance();
    // Two CPU sinks on bus 0, frame 7, I420 share exactly one GPU readback.
    const GpuReadbackSurfaceKey k{0u, 7, FramePixelFormat::Yuv420p};
    t.recordSurface(k);
    t.recordGpuReadback(k);
    const auto s = t.snapshot();
    QCOMPARE(s.gpuReadbacks, qint64(1));
    QCOMPARE(s.uniqueSurfaces, qint64(1));
    QCOMPARE(s.redundantReadbacks, qint64(0));
}

void TestGpuReadbackTelemetry::twoReadbacksForSameSurfaceAreRedundant() {
    auto& t = GpuReadbackTelemetry::instance();
    const GpuReadbackSurfaceKey k{0u, 7, FramePixelFormat::Yuv420p};
    t.recordSurface(k);
    t.recordGpuReadback(k);
    t.recordGpuReadback(k); // a second copy of the SAME surface == the bug
    const auto s = t.snapshot();
    QCOMPARE(s.gpuReadbacks, qint64(2));
    QCOMPARE(s.uniqueSurfaces, qint64(1));
    QCOMPARE(s.redundantReadbacks, qint64(1));
}

void TestGpuReadbackTelemetry::distinctFormatsAreDistinctSurfaces() {
    auto& t = GpuReadbackTelemetry::instance();
    const GpuReadbackSurfaceKey i420{0u, 7, FramePixelFormat::Yuv420p};
    const GpuReadbackSurfaceKey nv12{0u, 7, FramePixelFormat::Nv12};
    t.recordSurface(i420);
    t.recordSurface(nv12);
    t.recordGpuReadback(i420);
    t.recordGpuReadback(nv12);
    const auto s = t.snapshot();
    QCOMPARE(s.gpuReadbacks, qint64(2));
    QCOMPARE(s.uniqueSurfaces, qint64(2));   // same bus+frame, different CPU format
    QCOMPARE(s.redundantReadbacks, qint64(0));
}

QTEST_GUILESS_MAIN(TestGpuReadbackTelemetry)
#include "tst_gpureadbacktelemetry.moc"
```

Register it in `tests/unit/CMakeLists.txt` immediately after the `tst_outputdispatcher_holdlast` line (`olr_add_unit_test(tst_outputdispatcher_holdlast olr_test_playback)`):

```cmake
olr_add_unit_test(tst_gpureadbacktelemetry olr_test_playback)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_gpureadbacktelemetry`
Expected: FAIL to compile — `fatal error: 'playback/output/gpureadbacktelemetry.h' file not found`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/output/gpureadbacktelemetry.h`:

```cpp
#ifndef GPUREADBACKTELEMETRY_H
#define GPUREADBACKTELEMETRY_H

#include <QMutex>
#include <QSet>
#include <QtGlobal>

#include "playback/output/framepixelformat.h"

// Identity of a rendered bus surface for the copy-on-GPU-path invariant: a
// GPU-backed readToCpu() is permitted ONCE per (busKey, outputFrameIndex,
// requested CPU format). Two CPU sinks on the same bus + format share one copy.
struct GpuReadbackSurfaceKey {
    quint32 busKey = 0;            // OutputBusId stable key (kind<<16 | index)
    qint64 outputFrameIndex = -1; // the rendered surface's frame index
    FramePixelFormat format = FramePixelFormat::Yuv420p; // the readback target
    bool operator==(const GpuReadbackSurfaceKey& o) const {
        return busKey == o.busKey && outputFrameIndex == o.outputFrameIndex &&
               format == o.format;
    }
};

inline uint qHash(const GpuReadbackSurfaceKey& k, uint seed = 0) noexcept {
    return ::qHash(k.busKey, seed) ^ ::qHash(k.outputFrameIndex, seed) ^
           ::qHash(static_cast<int>(k.format), seed);
}

struct GpuReadbackTelemetrySnapshot {
    qint64 gpuReadbacks = 0;       // total GPU-backed readToCpu() calls observed
    qint64 uniqueSurfaces = 0;     // distinct (busKey,frameIndex,format) keys
    qint64 redundantReadbacks = 0; // gpuReadbacks - uniqueSurfaces (>0 == bug)
};

// Process-wide passive accumulator. GPU-backed IFrameData::readToCpu()
// implementations (Phase 2) call recordGpuReadback() with the surface key the
// copy is for; CPU-backed frames never call it, so the snapshot stays zero on
// the Phase-1 CPU path. recordSurface() registers a rendered bus surface so
// uniqueSurfaces tracks the denominator of the one-copy-per-surface invariant.
class GpuReadbackTelemetry {
public:
    static GpuReadbackTelemetry& instance();

    void recordGpuReadback(const GpuReadbackSurfaceKey& key);
    void recordSurface(const GpuReadbackSurfaceKey& key);
    GpuReadbackTelemetrySnapshot snapshot() const;
    void reset();

private:
    GpuReadbackTelemetry() = default;
    mutable QMutex m_mutex;
    qint64 m_gpuReadbacks = 0;
    QSet<GpuReadbackSurfaceKey> m_surfaces;
    QSet<GpuReadbackSurfaceKey> m_readbackKeys;
};

#endif // GPUREADBACKTELEMETRY_H
```

Create `playback/output/gpureadbacktelemetry.cpp`:

```cpp
#include "playback/output/gpureadbacktelemetry.h"

GpuReadbackTelemetry& GpuReadbackTelemetry::instance() {
    static GpuReadbackTelemetry t;
    return t;
}

void GpuReadbackTelemetry::recordGpuReadback(const GpuReadbackSurfaceKey& key) {
    QMutexLocker lock(&m_mutex);
    ++m_gpuReadbacks;
    m_readbackKeys.insert(key);
}

void GpuReadbackTelemetry::recordSurface(const GpuReadbackSurfaceKey& key) {
    QMutexLocker lock(&m_mutex);
    m_surfaces.insert(key);
}

GpuReadbackTelemetrySnapshot GpuReadbackTelemetry::snapshot() const {
    QMutexLocker lock(&m_mutex);
    GpuReadbackTelemetrySnapshot s;
    s.gpuReadbacks = m_gpuReadbacks;
    // The invariant denominator is the set of surfaces actually read back, not
    // every rendered surface (a surface no CPU sink reads needs no copy).
    s.uniqueSurfaces = qint64(m_readbackKeys.size());
    s.redundantReadbacks = s.gpuReadbacks - s.uniqueSurfaces;
    if (s.redundantReadbacks < 0) s.redundantReadbacks = 0;
    return s;
}

void GpuReadbackTelemetry::reset() {
    QMutexLocker lock(&m_mutex);
    m_gpuReadbacks = 0;
    m_surfaces.clear();
    m_readbackKeys.clear();
}
```

Add the source to the playback library in `CMakeLists.txt`. Find the playback `output/` source list that already names `playback/output/outputdispatcher.cpp` and add alongside it (same target, same `target_sources`/list block):

```cmake
        playback/output/gpureadbacktelemetry.h playback/output/gpureadbacktelemetry.cpp
```

> Locate the exact line with `git grep -n "playback/output/outputdispatcher.cpp" CMakeLists.txt` and insert the new pair on the adjacent line so both the app and `olr_test_playback` (which compiles the same list) pick it up.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build/c --target tst_gpureadbacktelemetry && ctest --test-dir build/c -R tst_gpureadbacktelemetry --output-on-failure`
Expected: PASS (4 tests) — fresh snapshot all-zero; one-readback-per-surface non-redundant; double-readback redundant=1; distinct formats → 2 unique surfaces.

- [ ] **Step 5: Commit**

```bash
git add playback/output/gpureadbacktelemetry.h playback/output/gpureadbacktelemetry.cpp \
        tests/unit/tst_gpureadbacktelemetry.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(telemetry): GpuReadbackTelemetry copy-on-GPU-path detector"
```

---

## Task 2: Extend `OutputDispatchStats` with the five GPU gate counters

**Files:**
- Modify: `playback/output/outputdispatcher.h`, `playback/output/outputdispatcher.cpp`
- Test: `tests/unit/tst_outputdispatch_gpustats.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuReadbackTelemetry` (Task 1); the existing `OutputDispatcher::dispatchTick(const OutputFrameCache&, const PlaybackStateSnapshot&) -> OutputDispatchStats` and `OutputDispatcher::stats() const -> OutputDispatchStats` (`playback/output/outputdispatcher.h:98-100`).
- Produces: five new zero-default scalar fields on `OutputDispatchStats`, populated from the `GpuReadbackTelemetry` snapshot each tick:
  ```cpp
  qint64 gpuVramBytes = 0;          // current GPU surface VRAM occupancy (Phase 2)
  qint64 readbackQueueDepth = 0;    // max async-readback ring depth seen (Phase 2)
  qint64 readbackDrops = 0;         // async-readback frames dropped under backpressure
  qint64 fenceWaitStalls = 0;       // GPU fence-wait stalls on the output tick
  qint64 gpuOomDegrades = 0;        // GPU-OOM → CPU-handle degrade events
  qint64 gpuReadbacks = 0;          // GPU-backed readToCpu() calls (copy detector)
  qint64 redundantGpuReadbacks = 0; // > unique rendered bus surfaces (== the bug)
  ```

> Phase 1 wires the two copy-detector fields (`gpuReadbacks`, `redundantGpuReadbacks`) from the `GpuReadbackTelemetry` snapshot — they read zero because no GPU-backed frame exists yet. The other five fields are declared and default-zero now; Phase-2 GPU subprojects (`gpu-budget`, `async-readback`, `gpu-sync`) populate them via the same snapshot path. All seven travel through `dispatchTick`/`stats()` unchanged for callers.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_outputdispatch_gpustats.cpp`:

```cpp
// Verifies OutputDispatchStats carries the GPU-pipeline gate counters and that
// on the Phase-1 CPU path they all read zero: a CPU-only dispatchTick never
// triggers a GPU-backed readToCpu(), so gpuReadbacks/redundantGpuReadbacks and
// the VRAM/queue/fence/OOM fields are all 0. This pins the inert-on-CPU contract.
#include <QtTest>

#include "playback/output/framehandle.h"
#include "playback/output/gpureadbacktelemetry.h"
#include "playback/output/outputdispatcher.h"

static FrameHandle video(int feed, qint64 pts, uchar y) {
    FrameHandle h = solidYuv420pHandle(4, 4, y, 128, 128);
    h.metadata().key.feedIndex = feed;
    h.metadata().key.ptsMs = pts;
    return h;
}

class CollectingSink final : public IOutputSink {
public:
    explicit CollectingSink(OutputTargetKind kind) : m_kind(kind) {}
    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame&) override { return m_active; }

private:
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    bool m_active = false;
};

class TestOutputDispatchGpuStats : public QObject {
    Q_OBJECT
private slots:
    void init() { GpuReadbackTelemetry::instance().reset(); }

    void newFieldsDefaultZero();
    void cpuPathLeavesGpuCountersZero();
    void redundantReadbackSurfacesThroughStats();
};

void TestOutputDispatchGpuStats::newFieldsDefaultZero() {
    OutputDispatchStats s;
    QCOMPARE(s.gpuVramBytes, qint64(0));
    QCOMPARE(s.readbackQueueDepth, qint64(0));
    QCOMPARE(s.readbackDrops, qint64(0));
    QCOMPARE(s.fenceWaitStalls, qint64(0));
    QCOMPARE(s.gpuOomDegrades, qint64(0));
    QCOMPARE(s.gpuReadbacks, qint64(0));
    QCOMPARE(s.redundantGpuReadbacks, qint64(0));
}

void TestOutputDispatchGpuStats::cpuPathLeavesGpuCountersZero() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{feed0, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.selectedFeedIndex = 0;

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 77));
    for (int i = 0; i < 5; ++i) dispatcher.dispatchTick(cache, state);

    const OutputDispatchStats s = dispatcher.stats();
    QCOMPARE(s.gpuReadbacks, qint64(0));        // CPU frames never read back on GPU
    QCOMPARE(s.redundantGpuReadbacks, qint64(0));
    QCOMPARE(s.gpuVramBytes, qint64(0));
    QCOMPARE(s.readbackDrops, qint64(0));
    QCOMPARE(s.fenceWaitStalls, qint64(0));
    QCOMPARE(s.gpuOomDegrades, qint64(0));
}

void TestOutputDispatchGpuStats::redundantReadbackSurfacesThroughStats() {
    // Simulate a Phase-2 GPU path: one surface read back twice (the bug). The
    // dispatcher samples GpuReadbackTelemetry into its stats, so the redundancy
    // surfaces through OutputDispatchStats — proving the wiring path is live.
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{feed0, &sink}});

    const GpuReadbackSurfaceKey k{0u, 3, FramePixelFormat::Yuv420p};
    GpuReadbackTelemetry::instance().recordSurface(k);
    GpuReadbackTelemetry::instance().recordGpuReadback(k);
    GpuReadbackTelemetry::instance().recordGpuReadback(k); // redundant copy

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.selectedFeedIndex = 0;
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 77));
    dispatcher.dispatchTick(cache, state);

    const OutputDispatchStats s = dispatcher.stats();
    QCOMPARE(s.gpuReadbacks, qint64(2));
    QCOMPARE(s.redundantGpuReadbacks, qint64(1));
}

QTEST_GUILESS_MAIN(TestOutputDispatchGpuStats)
#include "tst_outputdispatch_gpustats.moc"
```

Register it in `tests/unit/CMakeLists.txt` immediately after the `tst_gpureadbacktelemetry` line:

```cmake
olr_add_unit_test(tst_outputdispatch_gpustats olr_test_playback)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_outputdispatch_gpustats`
Expected: FAIL to compile — `'class OutputDispatchStats' has no member named 'gpuVramBytes'` (and the six siblings).

- [ ] **Step 3: Write the minimal implementation**

In `playback/output/outputdispatcher.h`, add the seven fields to `OutputDispatchStats` (after `maxClockDivergenceMs` at line 74, before `OutputRuntimeDispatchStats runtime;`):

```cpp
    // GPU-pipeline gate counters (telemetry-contract, §6). All read zero on the
    // Phase-1 CPU path; Phase-2 GPU subprojects populate them. gpuReadbacks /
    // redundantGpuReadbacks come from the GpuReadbackTelemetry copy detector:
    // exactly one GPU-backed readToCpu() per unique rendered bus surface, so
    // redundantGpuReadbacks > 0 is the copy-on-GPU-path bug.
    qint64 gpuVramBytes = 0;          // current GPU surface VRAM occupancy
    qint64 readbackQueueDepth = 0;    // max async-readback ring depth seen
    qint64 readbackDrops = 0;         // async-readback frames dropped (backpressure)
    qint64 fenceWaitStalls = 0;       // GPU fence-wait stalls on the output tick
    qint64 gpuOomDegrades = 0;        // GPU-OOM -> CPU-handle degrade events
    qint64 gpuReadbacks = 0;          // GPU-backed readToCpu() calls
    qint64 redundantGpuReadbacks = 0; // gpuReadbacks beyond the unique-surface count
```

Add the telemetry include at the top of `playback/output/outputdispatcher.cpp` (after the existing `#include "playback/output/outputframeclock.h"` at line 3):

```cpp
#include "playback/output/gpureadbacktelemetry.h"
```

In `OutputDispatcher::dispatchTick`, sample the GPU telemetry into `m_stats` just before `m_stats.ticks++;` (line 135). The detector is process-wide and passive, so sampling its snapshot every tick keeps the counters current with zero coupling to the CPU render path:

```cpp
    const GpuReadbackTelemetrySnapshot gpu = GpuReadbackTelemetry::instance().snapshot();
    m_stats.gpuReadbacks = gpu.gpuReadbacks;
    m_stats.redundantGpuReadbacks = gpu.redundantReadbacks;

    m_stats.ticks++;
```

> The other five fields (`gpuVramBytes`, `readbackQueueDepth`, `readbackDrops`, `fenceWaitStalls`, `gpuOomDegrades`) are left at their zero default here — they have no Phase-1 producer. Phase-2 `gpu-budget`/`async-readback`/`gpu-sync` extend `GpuReadbackTelemetrySnapshot` and this same sampling block to fill them; their e2e gates (Task 4) already assert `== 0`, so a Phase-2 regression that bumps them trips immediately.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build/c --target tst_outputdispatch_gpustats && ctest --test-dir build/c -R tst_outputdispatch_gpustats --output-on-failure`
Expected: PASS (3 tests) — fields default zero; CPU path leaves all GPU counters zero; an injected double-readback surfaces as `redundantGpuReadbacks=1` through `stats()`.

- [ ] **Step 5: Run the full unit label to confirm no sibling regression**

Run: `ctest --test-dir build/c -L unit --output-on-failure`
Expected: PASS — the additive struct fields and passive sampling do not change `tst_outputdispatcher`, `tst_outputdispatcher_holdlast`, `tst_outputruntime`, or any other dispatcher-touching test's assertion values.

- [ ] **Step 6: Commit**

```bash
git add playback/output/outputdispatcher.h playback/output/outputdispatcher.cpp \
        tests/unit/tst_outputdispatch_gpustats.cpp tests/unit/CMakeLists.txt
git commit -m "feat(telemetry): GPU gate counters on OutputDispatchStats (inert on CPU path)"
```

---

## Task 3: Emit the five GPU counters on the `play_harness` COUNTERS line

**Files:**
- Modify: `tests/e2e/play_harness.cpp`

**Interfaces:**
- Consumes: `OutputDispatchStats` (Task 2) via the existing `worker.outputStats()` (`tests/e2e/play_harness.cpp:143`, `playback/playbackworker.h:125`). The harness already reads `os.placeholderFrames`, `os.heldFrames`, `os.skippedDuplicateFrames`, `os.maxClockDivergenceMs` — the new fields are siblings on the same struct.
- Produces: five additional tokens appended to the single parseable `COUNTERS` line, in BOTH the `finish` lambda (line 148) and the unknown-scenario fallback (line 606): `gpuReadbacks`, `redundantGpuReadbacks`, `readbackQueueDepth`, `readbackDrops`, `gpuOomDegrades`, plus `fenceWaitStalls`, `gpuVramBytes`.

> The COUNTERS line is consumed positionally-by-name (the driver's `get key` greps `key=<digits>`), so appending tokens at the end is safe and non-breaking. Append all seven GPU fields so the contract is complete even though five have no Phase-1 producer.

- [ ] **Step 1: Extend the `finish` lambda's printf**

In `tests/e2e/play_harness.cpp`, the `finish` lambda (lines 138-162) already captures `const OutputDispatchStats os = worker.outputStats();`. Append the seven GPU tokens to the format string and argument list. Change the closing of the format string from:

```cpp
               "maxBoundaryLandingErrMs=%lld armNextCutArmed=%d decodedVideoFrames=%lld "
               "stagingVideoFramesDecoded=%lld\n",
```

to:

```cpp
               "maxBoundaryLandingErrMs=%lld armNextCutArmed=%d decodedVideoFrames=%lld "
               "stagingVideoFramesDecoded=%lld gpuReadbacks=%lld redundantGpuReadbacks=%lld "
               "readbackQueueDepth=%lld readbackDrops=%lld fenceWaitStalls=%lld "
               "gpuOomDegrades=%lld gpuVramBytes=%lld\n",
```

and append to the argument list, immediately after the existing `(long long) c.stagingVideoFramesDecoded);` (i.e. replace the trailing `);` of that printf with the new args):

```cpp
               (long long) c.stagingVideoFramesDecoded, (long long) os.gpuReadbacks,
               (long long) os.redundantGpuReadbacks, (long long) os.readbackQueueDepth,
               (long long) os.readbackDrops, (long long) os.fenceWaitStalls,
               (long long) os.gpuOomDegrades, (long long) os.gpuVramBytes);
```

- [ ] **Step 2: Extend the unknown-scenario fallback printf identically**

In the `else` branch (lines 597-620), the fallback already has `const OutputDispatchStats os = worker.outputStats();` at line 601. Apply the SAME format-string and argument-list extension to the fallback printf (lines 606-617) so the unknown-scenario line stays parseable with the full token set:

```cpp
                   "maxBoundaryLandingErrMs=%lld armNextCutArmed=%d "
                   "decodedVideoFrames=%lld stagingVideoFramesDecoded=%lld "
                   "gpuReadbacks=%lld redundantGpuReadbacks=%lld readbackQueueDepth=%lld "
                   "readbackDrops=%lld fenceWaitStalls=%lld gpuOomDegrades=%lld "
                   "gpuVramBytes=%lld\n",
```

and the matching trailing args after `(long long) c.stagingVideoFramesDecoded`:

```cpp
                   (long long) c.stagingVideoFramesDecoded, (long long) os.gpuReadbacks,
                   (long long) os.redundantGpuReadbacks, (long long) os.readbackQueueDepth,
                   (long long) os.readbackDrops, (long long) os.fenceWaitStalls,
                   (long long) os.gpuOomDegrades, (long long) os.gpuVramBytes);
```

- [ ] **Step 3: Build the harness and verify the line prints the new tokens**

Run: `cmake --build build/c --target play_harness`
Expected: PASS (compiles — the fields exist from Task 2).

Then run the harness once against a fixture and confirm the tokens are present and zero on the CPU path. The e2e driver records its own fixture; for a quick local check, run the driver in a way that prints the `[pb-e2e] COUNTERS …` line:

Run: `ctest --test-dir build/c -R e2e_play --output-on-failure 2>&1 | grep -m1 'COUNTERS' || true`
Expected: the printed `COUNTERS …` line ends with `… gpuReadbacks=0 redundantGpuReadbacks=0 readbackQueueDepth=0 readbackDrops=0 fenceWaitStalls=0 gpuOomDegrades=0 gpuVramBytes=0`.

> If the e2e target is not configured/registered in this build dir, build `play_harness` and `record_harness` and invoke `tests/e2e/run_playback_e2e.sh <play_harness> <record_harness> play1x 2 <srtPort>` directly (CLAUDE.md), then grep the `COUNTERS` line — the tokens must be present and zero.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/play_harness.cpp
git commit -m "feat(telemetry): emit GPU gate counters on play_harness COUNTERS line"
```

---

## Task 4: Parse + assert the GPU counters in `run_playback_e2e.sh` (CPU-path zero gate)

**Files:**
- Modify: `tests/e2e/run_playback_e2e.sh`

**Interfaces:**
- Consumes: the extended `COUNTERS` line (Task 3) via the existing `get()` helper (`tests/e2e/run_playback_e2e.sh:231`): `get() { printf '%s\n' "$COUNTERS" | sed -n "s/.*[[:space:]]$1=\([0-9]*\).*/\1/p"; }`.
- Produces: seven new parsed vars + a global CPU-path gate asserting the copy-on-GPU-path detector and the GPU resource counters are all zero. The gate is scenario-independent (runs for every scenario) because the whole Phase-1 suite is CPU-only.

> POSIX/bash-3.2 constraints (macOS): no associative arrays, no `[[ ]]`-only constructs the file avoids; reuse the existing `get`, `num`, and `[ … ]` style verbatim.

- [ ] **Step 1: Parse + default the seven new counters**

In `tests/e2e/run_playback_e2e.sh`, after the existing `stagingVideoFramesDecoded="$(get stagingVideoFramesDecoded)"` (line 250), add:

```sh
gpuReadbacks="$(get gpuReadbacks)"
redundantGpuReadbacks="$(get redundantGpuReadbacks)"
readbackQueueDepth="$(get readbackQueueDepth)"
readbackDrops="$(get readbackDrops)"
fenceWaitStalls="$(get fenceWaitStalls)"
gpuOomDegrades="$(get gpuOomDegrades)"
gpuVramBytes="$(get gpuVramBytes)"
```

After the existing `[ -n "$stagingVideoFramesDecoded" ] || stagingVideoFramesDecoded="?"` (line 269), add the matching defaults:

```sh
[ -n "$gpuReadbacks" ] || gpuReadbacks="?"
[ -n "$redundantGpuReadbacks" ] || redundantGpuReadbacks="?"
[ -n "$readbackQueueDepth" ] || readbackQueueDepth="?"
[ -n "$readbackDrops" ] || readbackDrops="?"
[ -n "$fenceWaitStalls" ] || fenceWaitStalls="?"
[ -n "$gpuOomDegrades" ] || gpuOomDegrades="?"
[ -n "$gpuVramBytes" ] || gpuVramBytes="?"
```

- [ ] **Step 2: Add the scenario-independent CPU-path GPU gate**

In `tests/e2e/run_playback_e2e.sh`, the per-scenario `case "$SCENARIO" in … esac` block ends and is followed by the final `if [ "$fail" -ne 0 ]` summary. Insert this gate AFTER the `esac` (so it runs for every scenario) and BEFORE the summary. Find the `esac` that closes the scenario `case` (search for the line `esac` after the `playlist`/`armedcut-*` branches) and add immediately after it:

```sh
# --- Phase-1 GPU telemetry contract: every counter is inert (zero) on the CPU
# path. The copy-on-GPU-path detector is the load-bearing one: redundantGpuReadbacks>0
# means a GPU-backed surface was read back to CPU more than once per unique
# rendered bus surface. On the CPU path no GPU readback happens at all, so the
# whole set must be exactly 0 — and these same gates go live unchanged when
# OLR_GPU_PIPELINE GPU code lands in Phase 2.
for gpucnt in gpuReadbacks redundantGpuReadbacks readbackQueueDepth readbackDrops \
              fenceWaitStalls gpuOomDegrades gpuVramBytes; do
    eval "gpuval=\$$gpucnt"
    if ! num "$gpuval" || [ "$gpuval" -ne 0 ]; then
        echo "FAIL: GPU telemetry counter $gpucnt=$gpuval, expected 0 on the CPU path — Phase-1 counters must read inert/zero"
        fail=1
    fi
done
```

- [ ] **Step 3: Run the e2e gate on the headline scenario**

Run: `ctest --test-dir build/c -R e2e_play --output-on-failure`
Expected: PASS — the `[pb-e2e] COUNTERS …` line shows the seven new tokens at `0`, and the new `for gpucnt …` loop adds no `FAIL:` line. The existing `play1x` reposition/audio gates still pass (the telemetry change is additive).

> If `e2e_play` runs multiple scenarios, all pass: the gate asserts zero, which holds for every CPU-path scenario. If only `play1x` is wired, run the other scenarios directly via `tests/e2e/run_playback_e2e.sh <play_harness> <record_harness> <scenario> 2 <srtPort>` (distinct SRT port per concurrent run) to confirm the global gate holds across `seekplay armedcut farback playlist`.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/run_playback_e2e.sh
git commit -m "test(telemetry): assert GPU gate counters read zero on the CPU path"
```

---

## Task 5: Document the counter contract + run the full suite

**Files:**
- Modify: `tests/e2e/play_harness.cpp` (header comment block, lines 13-18)

**Interfaces:**
- Consumes: the complete COUNTERS token set (Tasks 3-4).
- Produces: an updated harness header comment enumerating the GPU telemetry tokens and their CPU-path-zero / Phase-2-live meaning, so the contract is self-documenting in the public tree.

- [ ] **Step 1: Update the harness header comment**

In `tests/e2e/play_harness.cpp`, the header block (lines 13-18) documents the COUNTERS line. Extend it to name the GPU telemetry tokens. Replace the existing lines:

```cpp
// At the end the worker's counters() are printed as one parseable line:
//   COUNTERS reposition=.. reuseSeek=.. reverseChunkSeek=.. eofTailSeek=..
//            skipForward=.. audioPushes=.. framesDropped=..
```

with:

```cpp
// At the end the worker's counters() are printed as one parseable line:
//   COUNTERS reposition=.. reuseSeek=.. reverseChunkSeek=.. eofTailSeek=..
//            skipForward=.. audioPushes=.. framesDropped=.. (… existing tokens …)
// The GPU-pipeline telemetry contract (telemetry-contract, §6) appends:
//   gpuReadbacks=.. redundantGpuReadbacks=.. readbackQueueDepth=.. readbackDrops=..
//   fenceWaitStalls=.. gpuOomDegrades=.. gpuVramBytes=..
// On the Phase-1 CPU path all seven read 0 (no GPU-backed readToCpu, no GPU
// resources); run_playback_e2e.sh gates them at 0. redundantGpuReadbacks is the
// copy-on-GPU-path detector: > 0 means a rendered bus surface was read back to
// CPU more than once. Phase-2 GPU subprojects populate the rest via the same path.
```

- [ ] **Step 2: Build the harness (comment-only change must still compile)**

Run: `cmake --build build/c --target play_harness`
Expected: PASS (comment-only edit).

- [ ] **Step 3: Run the full unit label + the e2e gate**

Run: `ctest --test-dir build/c -L unit --output-on-failure && ctest --test-dir build/c -R e2e_play --output-on-failure`
Expected: PASS — the two new unit tests (`tst_gpureadbacktelemetry`, `tst_outputdispatch_gpustats`) pass, no sibling dispatcher test regresses, and the e2e CPU-path GPU gate reports all-zero counters.

- [ ] **Step 4: Format changed lines only**

Run:
```bash
CF=/opt/homebrew/opt/llvm/bin/clang-format
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'
```
If it reports diffs, stage the touched files and apply:
```bash
git add -A
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add -A
```
Expected: only the lines this plan changed are reformatted; the hand-Allman engine files are otherwise untouched.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/play_harness.cpp
git commit -m "docs(telemetry): document GPU counter contract in play_harness header"
```

---

## Verification checklist (whole subproject)

- [ ] `ctest --test-dir build/c -R tst_gpureadbacktelemetry --output-on-failure` — PASS (4)
- [ ] `ctest --test-dir build/c -R tst_outputdispatch_gpustats --output-on-failure` — PASS (3)
- [ ] `ctest --test-dir build/c -L unit --output-on-failure` — PASS (no sibling regression)
- [ ] `ctest --test-dir build/c -R e2e_play --output-on-failure` — PASS; `COUNTERS` line ends with the seven GPU tokens at `0`; the global `for gpucnt …` gate adds no FAIL
- [ ] Existing counters (`placeholderFramesDelta`, `heldFramesDelta`, `maxClockDivergenceMs`, `decodedVideoFrames`, `stagingVideoFramesDecoded`) unchanged in meaning and value
- [ ] `git clang-format --diff --commit origin/main` clean for the touched lines
