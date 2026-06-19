# Codec Benchmark Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `CodecBenchmark` engine that measures, off the GUI thread, how many concurrent feeds each codec (H.264 hardware, MPEG-2 software) sustains in real time on this device, and recommends a codec + safe feed count.

**Architecture:** Three layers, each testable in isolation. (1) A pure decision core — ramp sequence, sustain predicate, safe-feed-from-ceiling, recommendation — no threads or codecs. (2) An orchestrator that walks the concurrency ramp on its own worker threads (never the GUI thread), stops at the first failing step, emits coalesced progress, and returns a result; tested with a fake codec runner of programmable cost. (3) Real codec runners (H.264 via `NativeVideoEncoder`+`NativeVideoDecoder`; MPEG-2 via FFmpeg) plus a deterministic synthetic frame source, and a device-keyed result cache. No UI (that is Plan 4).

**Tech Stack:** C++17, Qt 6 (Core/Test), FFmpeg (libav*), the existing `NativeVideoEncoder`/`NativeVideoDecoder`, std::thread.

## Global Constraints

- **Runs OFF the GUI thread, never in it.** `CodecBenchmark` owns a controller thread + N pipeline threads; the GUI thread only starts it (async), receives **coalesced** progress (one update per ramp step, never per frame), and receives the final result. No codec work, allocation, or timing on the GUI thread.
- **Measures BOTH codecs.** MPEG-2 is always measured; H.264 is measured only if `queryNativeVideoEncodeCapabilities().h264` (and a decoder is available). Never skip MPEG-2 because H.264 exists.
- **Unit of work = encode one frame + decode one frame** per frame interval (ingest decode is excluded — it is hardware and constant).
- **Ramp = `1, 2, 4, 8, 12, 16, 20, 24, 28, 32`, STOP at the first failing step** (higher N would also fail). Per codec, independently.
- **Sustain criterion:** step N passes iff processed frame-pairs ≥ `N × fps × durationSec × 0.95` AND no single frame exceeded its interval budget. **Safe feed count** = largest passing N that held with ≥ 1.2× headroom.
- **Hardware-only H.264** (Plan 2 invariant): the H.264 runner uses `NativeVideoEncoder`/`NativeVideoDecoder` only; never FFmpeg software h264.
- **Deterministic:** synthetic frames are generated procedurally (no RNG); tests of the decision core and orchestrator use injected timings and are reproducible.
- **Bounded & time-boxed:** N capped at 32 (the ramp's last step); the whole run target < 60 s (early-stop keeps it well under); if the ramp reaches 32 without failing, that is logged as "ceiling not reached" (no silent truncation). Cancellation is supported and prompt.
- Build (run from the worktree root): configure once with `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`; test `ctest --test-dir build/claude-debug -R <name> -V`. Use `build/claude-debug` only.

---

## File Structure

- **Create** `recorder_engine/benchmark/benchmarktypes.h` — `BenchmarkConfig`, `RampStepResult`, `CodecBenchmarkResult`, `VideoCodecChoice` reuse.
- **Create** `recorder_engine/benchmark/benchmarkplan.{h,cpp}` — pure decision functions (ramp steps, sustain predicate, safe-feed, recommendation).
- **Create** `recorder_engine/benchmark/codecrunner.h` — abstract `CodecRunner` (runs N concurrent encode+decode pipelines for a duration → `RampStepResult`).
- **Create** `recorder_engine/benchmark/codecbenchmark.{h,cpp}` — the orchestrator (worker-threaded ramp, stop-on-fail, progress, result).
- **Create** `recorder_engine/benchmark/syntheticframes.{h,cpp}` — deterministic YUV420P frame source.
- **Create** `recorder_engine/benchmark/realcodecrunners.{h,cpp}` — `Mpeg2CodecRunner` (FFmpeg) + `H264CodecRunner` (native).
- **Create** `recorder_engine/benchmark/benchmarkcache.{h,cpp}` — device-keyed JSON save/load + invalidation.
- **Create** `recorder_engine/benchmark/runcodecbenchmark.{h,cpp}` — top-level entry tying probe + runners + cache.
- **Modify** `CMakeLists.txt` — add the benchmark sources to the engine library (`olr_engine`/`olr_test_engine`).
- **Create** tests: `tests/unit/tst_benchmarkplan.cpp`, `tst_codecbenchmark.cpp`, `tst_benchmarkcache.cpp`, `tst_realcodecbenchmark.cpp`; register in `tests/unit/CMakeLists.txt`.

---

## Task 1: Pure decision core (ramp, sustain, safe-feed, recommendation)

**Files:**
- Create: `recorder_engine/benchmark/benchmarktypes.h`, `recorder_engine/benchmark/benchmarkplan.h`, `recorder_engine/benchmark/benchmarkplan.cpp`
- Test: `tests/unit/tst_benchmarkplan.cpp`
- Modify: `CMakeLists.txt`, `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  - ```cpp
    struct RampStepResult {
      int concurrency = 0;        // N
      int framesProcessed = 0;    // encode+decode pairs completed in the window
      int framesRequired = 0;     // N * fps * durationSec
      bool budgetMet = true;      // no single frame exceeded its interval budget
      double avgEncodeMs = 0.0;
      double avgDecodeMs = 0.0;
    };
    ```
  - `QVector<int> benchmarkRampSteps();` → `{1,2,4,8,12,16,20,24,28,32}`
  - `bool rampStepSustained(const RampStepResult& r);` → `framesProcessed >= 0.95*framesRequired && budgetMet`
  - `bool rampStepHasHeadroom(const RampStepResult& r);` → `framesProcessed >= 1.2*framesRequired && budgetMet`
  - `int safeFeedCount(const QVector<RampStepResult>& steps);` → largest N that `rampStepHasHeadroom`; 0 if none.
  - `int ceilingFeedCount(const QVector<RampStepResult>& steps);` → largest N that `rampStepSustained`; 0 if none.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_benchmarkplan.cpp`:

```cpp
// Unit tests for the pure benchmark decision core — ramp steps, sustain/headroom
// predicates, and safe/ceiling feed-count derivation. No threads, no codecs.
#include <QtTest>

#include "recorder_engine/benchmark/benchmarkplan.h"

class TestBenchmarkPlan : public QObject {
    Q_OBJECT
private slots:
    void rampStepsAreExact();
    void sustainPredicate();
    void headroomPredicate();
    void safeAndCeilingFromSteps();
};

void TestBenchmarkPlan::rampStepsAreExact() {
    QCOMPARE(benchmarkRampSteps(), (QVector<int>{1, 2, 4, 8, 12, 16, 20, 24, 28, 32}));
}

void TestBenchmarkPlan::sustainPredicate() {
    RampStepResult r; r.concurrency = 4; r.framesRequired = 600;
    r.framesProcessed = 600; r.budgetMet = true;
    QVERIFY(rampStepSustained(r));
    r.framesProcessed = 570; QVERIFY(rampStepSustained(r));   // exactly 0.95
    r.framesProcessed = 560; QVERIFY(!rampStepSustained(r));  // below 0.95
    r.framesProcessed = 600; r.budgetMet = false; QVERIFY(!rampStepSustained(r));
}

void TestBenchmarkPlan::headroomPredicate() {
    RampStepResult r; r.framesRequired = 600; r.budgetMet = true;
    r.framesProcessed = 720; QVERIFY(rampStepHasHeadroom(r));   // exactly 1.2
    r.framesProcessed = 719; QVERIFY(!rampStepHasHeadroom(r));
}

void TestBenchmarkPlan::safeAndCeilingFromSteps() {
    // N=1,2,4 strong; N=8 sustained but tight (no headroom); N=12 fails.
    auto mk = [](int n, int req, int got) {
        RampStepResult r; r.concurrency = n; r.framesRequired = req;
        r.framesProcessed = got; r.budgetMet = true; return r;
    };
    QVector<RampStepResult> steps{
        mk(1, 150, 300), mk(2, 300, 600), mk(4, 600, 1200),
        mk(8, 1200, 1260),   // sustained (>=0.95*1200=1140) but < headroom (1440)
        mk(12, 1800, 1500),  // not sustained (< 1710)
    };
    QCOMPARE(ceilingFeedCount(steps), 8); // largest sustained
    QCOMPARE(safeFeedCount(steps), 4);    // largest with 1.2x headroom
}

QTEST_GUILESS_MAIN(TestBenchmarkPlan)
#include "tst_benchmarkplan.moc"
```

Register in `tests/unit/CMakeLists.txt` (with the other `olr_test_engine` tests): `olr_add_unit_test(tst_benchmarkplan olr_test_engine)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_benchmarkplan`
Expected: FAIL to compile — `benchmarkplan.h` not found.

- [ ] **Step 3: Write the implementation**

Create `recorder_engine/benchmark/benchmarktypes.h`:

```cpp
#ifndef OLR_BENCHMARKTYPES_H
#define OLR_BENCHMARKTYPES_H

#include "recorder_engine/codec/videocodecchoice.h"

#include <QString>

struct BenchmarkConfig {
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrate = 30'000'000;
    int durationMsPerStep = 3000; // measurement window per ramp step
};

struct RampStepResult {
    int concurrency = 0;
    int framesProcessed = 0;
    int framesRequired = 0;
    bool budgetMet = true;
    double avgEncodeMs = 0.0;
    double avgDecodeMs = 0.0;
};

struct CodecBenchmarkResult {
    bool h264Available = false;
    int  h264SafeFeeds = -1;   // -1 = not measured / unavailable
    int  mpeg2SafeFeeds = -1;
    double h264EncodeMs = 0.0, h264DecodeMs = 0.0;
    double mpeg2EncodeMs = 0.0, mpeg2DecodeMs = 0.0;
    VideoCodecChoice recommended = VideoCodecChoice::Mpeg2Software;
    QString deviceLabel;
    QString resolution;     // e.g. "1920x1080@30"
    QString timestamp;      // ISO-8601, stamped by the caller
    bool ceilingReached = false; // true if a codec ramp hit N=32 without failing
};

#endif // OLR_BENCHMARKTYPES_H
```

Create `recorder_engine/benchmark/benchmarkplan.h`:

```cpp
#ifndef OLR_BENCHMARKPLAN_H
#define OLR_BENCHMARKPLAN_H

#include "recorder_engine/benchmark/benchmarktypes.h"

#include <QVector>

// The concurrency ramp, coarse early then fine through the high end.
QVector<int> benchmarkRampSteps();

// Step N keeps up with real time (>=95% of required pairs, no frame over budget).
bool rampStepSustained(const RampStepResult& r);
// Step N has comfortable margin (>=120% of required pairs, no frame over budget).
bool rampStepHasHeadroom(const RampStepResult& r);

// Largest N that sustained; 0 if none.
int ceilingFeedCount(const QVector<RampStepResult>& steps);
// Largest N that had headroom (the recommended safe feed count); 0 if none.
int safeFeedCount(const QVector<RampStepResult>& steps);

#endif // OLR_BENCHMARKPLAN_H
```

Create `recorder_engine/benchmark/benchmarkplan.cpp`:

```cpp
#include "recorder_engine/benchmark/benchmarkplan.h"

QVector<int> benchmarkRampSteps() {
    return {1, 2, 4, 8, 12, 16, 20, 24, 28, 32};
}

bool rampStepSustained(const RampStepResult& r) {
    return r.budgetMet && r.framesProcessed >= 0.95 * r.framesRequired;
}

bool rampStepHasHeadroom(const RampStepResult& r) {
    return r.budgetMet && r.framesProcessed >= 1.2 * r.framesRequired;
}

int ceilingFeedCount(const QVector<RampStepResult>& steps) {
    int best = 0;
    for (const RampStepResult& r : steps)
        if (rampStepSustained(r) && r.concurrency > best) best = r.concurrency;
    return best;
}

int safeFeedCount(const QVector<RampStepResult>& steps) {
    int best = 0;
    for (const RampStepResult& r : steps)
        if (rampStepHasHeadroom(r) && r.concurrency > best) best = r.concurrency;
    return best;
}
```

Add the benchmark sources to the engine library source list in `CMakeLists.txt` (alongside the `recorder_engine/codec/avcc.*` entries from Plan 2):

```cmake
        recorder_engine/benchmark/benchmarktypes.h
        recorder_engine/benchmark/benchmarkplan.h recorder_engine/benchmark/benchmarkplan.cpp
```

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_benchmarkplan && ctest --test-dir build/claude-debug -R tst_benchmarkplan -V`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/benchmark/ tests/unit/tst_benchmarkplan.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat: pure codec-benchmark decision core (ramp, sustain, safe-feed)"
```

---

## Task 2: Orchestrator + CodecRunner interface (fake-codec tested)

**Files:**
- Create: `recorder_engine/benchmark/codecrunner.h`, `recorder_engine/benchmark/codecbenchmark.h`, `recorder_engine/benchmark/codecbenchmark.cpp`
- Test: `tests/unit/tst_codecbenchmark.cpp`
- Modify: `CMakeLists.txt`, `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 (`benchmarkRampSteps`, `safeFeedCount`, `ceilingFeedCount`, `RampStepResult`, `BenchmarkConfig`).
- Produces:
  - ```cpp
    class CodecRunner {
    public:
      virtual ~CodecRunner() = default;
      // Run `concurrency` independent encode+decode pipelines for the config's
      // window; return the aggregate measurement. Blocking; called off the GUI thread.
      virtual RampStepResult runStep(int concurrency, const BenchmarkConfig&) = 0;
      virtual bool available() const = 0;   // false → codec not measured
    };
    ```
  - ```cpp
    class CodecBenchmark {
    public:
      using ProgressFn = std::function<void(int concurrency, bool sustained)>; // coalesced, per step
      // Walks the ramp, stops at the first non-sustained step; fills safe/ceiling
      // and avg ms (from the largest sustained step). Returns the per-codec partial.
      struct CodecResult { int safeFeeds; int ceiling; double encodeMs; double decodeMs;
                           bool ceilingReached; QVector<RampStepResult> steps; };
      static CodecResult rampCodec(CodecRunner& runner, const BenchmarkConfig&,
                                   const ProgressFn& onStep, const std::atomic<bool>& cancel);
    };
    ```

> `rampCodec` is the testable orchestration unit: it is pure control flow over a `CodecRunner`, so a `FakeCodecRunner` with programmable per-N outcomes makes the ramp/stop-on-fail/safe-feed deterministically testable with no real codecs or GUI thread. The full async/threaded driver that calls `rampCodec` for both codecs lives in Task 4 (`runCodecBenchmark`).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_codecbenchmark.cpp`:

```cpp
// Unit tests for the benchmark orchestrator's ramp control flow, using a fake
// CodecRunner with programmable per-N outcomes. Deterministic, no real codecs.
#include <QtTest>
#include <atomic>

#include "recorder_engine/benchmark/codecbenchmark.h"
#include "recorder_engine/benchmark/codecrunner.h"

namespace {
// Fake runner: "sustains" for N <= maxSustain (with headroom up to maxHeadroom),
// records the ramp it was asked to run, so tests can assert ordering + stop-on-fail.
class FakeRunner : public CodecRunner {
public:
    int maxHeadroom; int maxSustain; QVector<int> visited;
    FakeRunner(int headroom, int sustain) : maxHeadroom(headroom), maxSustain(sustain) {}
    bool available() const override { return true; }
    RampStepResult runStep(int n, const BenchmarkConfig& c) override {
        visited.append(n);
        RampStepResult r; r.concurrency = n;
        r.framesRequired = n * c.fps * (c.durationMsPerStep / 1000);
        if (n <= maxHeadroom)      r.framesProcessed = r.framesRequired * 2;   // strong
        else if (n <= maxSustain)  r.framesProcessed = r.framesRequired;       // tight
        else                       r.framesProcessed = r.framesRequired / 2;   // fails
        r.budgetMet = (n <= maxSustain);
        r.avgEncodeMs = 1.0; r.avgDecodeMs = 0.5;
        return r;
    }
};
} // namespace

class TestCodecBenchmark : public QObject {
    Q_OBJECT
private slots:
    void stopsAtFirstFailingStep();
    void safeFeedIsLargestWithHeadroom();
    void cancellationStopsRamp();
    void unavailableRunnerYieldsNoFeeds();
};

void TestCodecBenchmark::stopsAtFirstFailingStep() {
    FakeRunner runner(/*headroom*/8, /*sustain*/8); // N=12 fails
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    // Visited 1,2,4,8,12 then stopped (no 16+).
    QCOMPARE(runner.visited, (QVector<int>{1, 2, 4, 8, 12}));
    QCOMPARE(res.ceiling, 8);
    QVERIFY(!res.ceilingReached);
}

void TestCodecBenchmark::safeFeedIsLargestWithHeadroom() {
    FakeRunner runner(/*headroom*/4, /*sustain*/8); // 1,2,4 headroom; 8 tight; 12 fails
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    QCOMPARE(res.safeFeeds, 4);
    QCOMPARE(res.ceiling, 8);
}

void TestCodecBenchmark::cancellationStopsRamp() {
    FakeRunner runner(32, 32);
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{false};
    int calls = 0;
    auto res = CodecBenchmark::rampCodec(runner, cfg, [&](int,bool){ if (++calls == 2) cancel = true; }, cancel);
    QVERIFY(runner.visited.size() <= 3); // stopped shortly after cancel
    (void)res;
}

void TestCodecBenchmark::unavailableRunnerYieldsNoFeeds() {
    class Unavail : public CodecRunner {
        bool available() const override { return false; }
        RampStepResult runStep(int, const BenchmarkConfig&) override { return {}; }
    } runner;
    BenchmarkConfig cfg;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    QCOMPARE(res.safeFeeds, 0);
    QCOMPARE(res.ceiling, 0);
}

QTEST_GUILESS_MAIN(TestCodecBenchmark)
#include "tst_codecbenchmark.moc"
```

Register: `olr_add_unit_test(tst_codecbenchmark olr_test_engine)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_codecbenchmark`
Expected: FAIL to compile — `codecbenchmark.h` / `codecrunner.h` not found.

- [ ] **Step 3: Write the implementation**

Create `recorder_engine/benchmark/codecrunner.h`:

```cpp
#ifndef OLR_CODECRUNNER_H
#define OLR_CODECRUNNER_H

#include "recorder_engine/benchmark/benchmarktypes.h"

// Runs N concurrent encode+decode pipelines for a measurement window and reports
// the aggregate. Implementations: real (FFmpeg MPEG-2, native H.264) and fake (tests).
class CodecRunner {
public:
    virtual ~CodecRunner() = default;
    virtual RampStepResult runStep(int concurrency, const BenchmarkConfig& config) = 0;
    virtual bool available() const = 0;
};

#endif // OLR_CODECRUNNER_H
```

Create `recorder_engine/benchmark/codecbenchmark.h`:

```cpp
#ifndef OLR_CODECBENCHMARK_H
#define OLR_CODECBENCHMARK_H

#include "recorder_engine/benchmark/benchmarkplan.h"
#include "recorder_engine/benchmark/codecrunner.h"

#include <atomic>
#include <functional>

class CodecBenchmark {
public:
    using ProgressFn = std::function<void(int concurrency, bool sustained)>;
    struct CodecResult {
        int safeFeeds = 0;
        int ceiling = 0;
        double encodeMs = 0.0;
        double decodeMs = 0.0;
        bool ceilingReached = false;
        QVector<RampStepResult> steps;
    };
    // Walk the ramp on the CALLING thread (the caller is already a worker thread).
    // Stops at the first non-sustained step, on cancel, or after the last ramp step.
    static CodecResult rampCodec(CodecRunner& runner, const BenchmarkConfig& config,
                                 const ProgressFn& onStep, const std::atomic<bool>& cancel);
};

#endif // OLR_CODECBENCHMARK_H
```

Create `recorder_engine/benchmark/codecbenchmark.cpp`:

```cpp
#include "recorder_engine/benchmark/codecbenchmark.h"

CodecBenchmark::CodecResult CodecBenchmark::rampCodec(
    CodecRunner& runner, const BenchmarkConfig& config,
    const ProgressFn& onStep, const std::atomic<bool>& cancel) {
    CodecResult out;
    if (!runner.available()) return out;

    const QVector<int> steps = benchmarkRampSteps();
    for (int i = 0; i < steps.size(); ++i) {
        if (cancel.load(std::memory_order_acquire)) break;
        const int n = steps[i];
        RampStepResult r = runner.runStep(n, config);
        out.steps.append(r);
        const bool sustained = rampStepSustained(r);
        if (onStep) onStep(n, sustained);
        if (!sustained) break;                  // stop at first failing step
        if (n == steps.last()) out.ceilingReached = true; // reached 32 still sustaining
    }
    out.safeFeeds = safeFeedCount(out.steps);
    out.ceiling = ceilingFeedCount(out.steps);
    // Report avg ms from the largest sustained step (most representative of real load).
    for (const RampStepResult& r : out.steps)
        if (rampStepSustained(r) && r.concurrency == out.ceiling) {
            out.encodeMs = r.avgEncodeMs; out.decodeMs = r.avgDecodeMs;
        }
    return out;
}
```

Add `codecrunner.h`, `codecbenchmark.{h,cpp}` to the engine library source list in `CMakeLists.txt`.

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_codecbenchmark && ctest --test-dir build/claude-debug -R tst_codecbenchmark -V`
Expected: PASS (4 tests) — ramp visits `1,2,4,8,12` then stops; safe=4; cancel stops early; unavailable→0.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/benchmark/ tests/unit/tst_codecbenchmark.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat: codec-benchmark ramp orchestrator (stop-on-fail, fake-runner tested)"
```

---

## Task 3: Real codec runners + deterministic synthetic frames

**Files:**
- Create: `recorder_engine/benchmark/syntheticframes.{h,cpp}`, `recorder_engine/benchmark/realcodecrunners.{h,cpp}`
- Test: `tests/unit/tst_realcodecbenchmark.cpp`
- Modify: `CMakeLists.txt`, `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `CodecRunner`, `RampStepResult`, `BenchmarkConfig` (Task 2); `NativeVideoEncoder`/`NativeVideoDecoder` (Plan 2 / ingest); FFmpeg MPEG-2.
- Produces:
  - `AVFrame* makeSyntheticFrame(int width, int height, int seq);` — deterministic YUV420P frame with spatial detail + per-`seq` motion (no RNG). Caller frees.
  - `class Mpeg2CodecRunner : public CodecRunner` — N pipelines, each: FFmpeg MPEG-2 encode (intra, like StreamWorker) → FFmpeg MPEG-2 decode. `available()` always true.
  - `class H264CodecRunner : public CodecRunner` — N pipelines, each: `NativeVideoEncoder` → `NativeVideoDecoder` (parameter sets from the encoder's avcC). `available()` = `queryNativeVideoEncodeCapabilities().h264 && queryNativeVideoDecodeCapabilities().h264`.

> Each pipeline runs on its own `std::thread` for the window; the runner spawns N threads, each loops `makeSyntheticFrame → encode → decode` pacing to `fps`, counts completed pairs and flags any frame that overran its `1000/fps` ms budget, then joins and aggregates into one `RampStepResult`. This matches the per-source worker model. The MPEG-2 encoder mirrors `StreamWorker::setupEncoder` (AV_CODEC_ID_MPEG2VIDEO, YUV420P, gop_size=1, bit_rate, time_base {1,fps}); decode via `avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO)`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_realcodecbenchmark.cpp` (small scale; H.264 part skips without HW):

```cpp
// Integration test: real MPEG-2 (FFmpeg) and H.264 (native) runners produce
// plausible measurements at small scale. H.264 skips where no hardware encoder.
#include <QtTest>
#include <atomic>

#include "recorder_engine/benchmark/realcodecrunners.h"
#include "recorder_engine/benchmark/codecbenchmark.h"

extern "C" { #include <libavutil/frame.h> }

class TestRealCodecBenchmark : public QObject {
    Q_OBJECT
private slots:
    void syntheticFrameIsDeterministic();
    void mpeg2RunnerMeasuresOneStep();
    void h264RunnerRampsWhenAvailable();
};

void TestRealCodecBenchmark::syntheticFrameIsDeterministic() {
    AVFrame* a = makeSyntheticFrame(320, 240, 5);
    AVFrame* b = makeSyntheticFrame(320, 240, 5);
    QVERIFY(a && b);
    QCOMPARE(memcmp(a->data[0], b->data[0], a->linesize[0] * 240), 0); // same seq → identical
    AVFrame* c = makeSyntheticFrame(320, 240, 6);
    QVERIFY(memcmp(a->data[0], c->data[0], a->linesize[0] * 240) != 0); // motion → differs
    av_frame_free(&a); av_frame_free(&b); av_frame_free(&c);
}

void TestRealCodecBenchmark::mpeg2RunnerMeasuresOneStep() {
    Mpeg2CodecRunner runner;
    QVERIFY(runner.available());
    BenchmarkConfig cfg; cfg.width = 320; cfg.height = 240; cfg.fps = 30; cfg.durationMsPerStep = 500;
    RampStepResult r = runner.runStep(1, cfg);
    QCOMPARE(r.concurrency, 1);
    QVERIFY(r.framesProcessed > 0);
    QVERIFY(r.framesRequired > 0);
}

void TestRealCodecBenchmark::h264RunnerRampsWhenAvailable() {
    H264CodecRunner runner;
    if (!runner.available()) QSKIP("no hardware H.264 on this platform");
    BenchmarkConfig cfg; cfg.width = 640; cfg.height = 480; cfg.fps = 30; cfg.durationMsPerStep = 500;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    QVERIFY(res.ceiling >= 1); // at least 1 feed sustains on any HW-capable device
}

QTEST_GUILESS_MAIN(TestRealCodecBenchmark)
#include "tst_realcodecbenchmark.moc"
```

Register linking the native encoder backend (mirror `tst_h264_roundtrip`'s linkage of `olr_test_nativevideoencoder` + the decoder lib): `olr_add_unit_test(tst_realcodecbenchmark olr_test_engine)` then `target_link_libraries(tst_realcodecbenchmark PRIVATE olr_test_nativevideoencoder olr_test_nativevideodecoder)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_realcodecbenchmark`
Expected: FAIL to compile — `realcodecrunners.h` not found.

- [ ] **Step 3: Write `syntheticframes.{h,cpp}`**

`syntheticframes.h`:

```cpp
#ifndef OLR_SYNTHETICFRAMES_H
#define OLR_SYNTHETICFRAMES_H
extern "C" { struct AVFrame; }
// Deterministic YUV420P frame: a diagonal gradient + a block that shifts with seq,
// giving real spatial detail and inter-frame motion so encode cost is representative.
// No RNG. Caller owns the returned frame (av_frame_free).
AVFrame* makeSyntheticFrame(int width, int height, int seq);
#endif // OLR_SYNTHETICFRAMES_H
```

`syntheticframes.cpp` — allocate YUV420P, fill Y with `(x + y + seq*4) & 0xff`, paint a moving 32×32 block at `(seq*8 % (w-32), seq*4 % (h-32))` with a contrasting luma, set U/V to a mild gradient `(x*2)&0xff` / `(y*2)&0xff`. (Deterministic in `seq`.)

- [ ] **Step 4: Write `realcodecrunners.{h,cpp}`**

Implement `Mpeg2CodecRunner` and `H264CodecRunner` per the Interfaces + the note. Each `runStep(n, cfg)`:
1. Compute `framesRequired = n * cfg.fps * cfg.durationMsPerStep/1000` and `budgetMs = 1000/cfg.fps`.
2. Spawn `n` std::threads; each thread builds its own encoder + decoder, then loops for `cfg.durationMsPerStep` wall-ms: `makeSyntheticFrame(seq++)` → encode → feed encoded output to its decoder → on a produced frame, `++pairs`; time each encode and decode with `QElapsedTimer`; if a single iteration exceeds `budgetMs`, set a per-thread `overran` flag. Pace to `fps` only if ahead (so a slow device just runs flat out and under-delivers, which the sustain math detects).
3. Join all threads; aggregate `framesProcessed = sum(pairs)`, `budgetMet = none overran`, and average encode/decode ms. Return the `RampStepResult`.
- H.264 thread: `NativeVideoEncoder::create(cfg→Config)`; prime one frame to get `avccExtradata()`, parse SPS/PPS, build a `CompressedAccessUnit` per encoded packet, decode via `NativeVideoDecoder`. (Reuse the avcC-parse approach from `tst_h264_roundtrip`/PlaybackWorker — factor a tiny `parseAvcc(extradata, &sps, &pps)` helper into `avcc.{h,cpp}` if not already present, and unit-cover it there.)
- MPEG-2 thread: FFmpeg encoder (mirror `StreamWorker::setupEncoder`) + FFmpeg `AV_CODEC_ID_MPEG2VIDEO` decoder.

- [ ] **Step 5: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_realcodecbenchmark && ctest --test-dir build/claude-debug -R tst_realcodecbenchmark -V`
Expected: PASS — synthetic determinism holds; MPEG-2 runner measures a step; H.264 runner ramps (on this Mac, not skipped) with ceiling ≥ 1.

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/benchmark/ recorder_engine/codec/avcc.* tests/unit/tst_realcodecbenchmark.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat: real MPEG-2 + native H.264 benchmark runners + synthetic frames"
```

---

## Task 4: Top-level entry + device-keyed result cache

**Files:**
- Create: `recorder_engine/benchmark/benchmarkcache.{h,cpp}`, `recorder_engine/benchmark/runcodecbenchmark.{h,cpp}`
- Test: `tests/unit/tst_benchmarkcache.cpp`
- Modify: `CMakeLists.txt`, `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: everything above; `CodecBenchmarkResult` (Task 1).
- Produces:
  - `QString benchmarkDeviceLabel();` — stable device id (e.g. `QSysInfo::prettyProductName() + " " + QSysInfo::currentCpuArchitecture()`).
  - `bool saveBenchmarkResult(const QString& path, const CodecBenchmarkResult&);`
  - `bool loadBenchmarkResult(const QString& path, CodecBenchmarkResult& out);`
  - `bool benchmarkResultMatches(const CodecBenchmarkResult& cached, const QString& deviceLabel, const QString& resolution);` — cache validity (same device + resolution).
  - `CodecBenchmarkResult runCodecBenchmark(const BenchmarkConfig&, const CodecBenchmark::ProgressFn&, const std::atomic<bool>& cancel);` — builds both real runners, calls `rampCodec` for each, assembles the full `CodecBenchmarkResult` (incl. `recommended` = H.264 when available and `h264SafeFeeds >= mpeg2SafeFeeds`, else MPEG-2; `deviceLabel`/`resolution` filled; `ceilingReached` if either codec hit 32). Pure orchestration over the runners; the caller runs it on a worker thread.

> The async/threaded wrapper that the GUI calls (start on a controller thread, marshal coalesced progress + result back) is thin and belongs to Plan 4's UI wiring; Plan 3 delivers `runCodecBenchmark` as a blocking call meant to be invoked from a worker thread, plus the cache.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_benchmarkcache.cpp`:

```cpp
// Unit tests for benchmark result caching: round-trip + device/resolution invalidation.
#include <QtTest>
#include <QTemporaryDir>

#include "recorder_engine/benchmark/benchmarkcache.h"

class TestBenchmarkCache : public QObject {
    Q_OBJECT
private slots:
    void roundTrip();
    void invalidatesOnDeviceOrResolutionChange();
    void deviceLabelIsNonEmpty();
};

void TestBenchmarkCache::roundTrip() {
    QTemporaryDir dir; const QString path = dir.filePath("bench.json");
    CodecBenchmarkResult in;
    in.h264Available = true; in.h264SafeFeeds = 12; in.mpeg2SafeFeeds = 5;
    in.h264EncodeMs = 1.8; in.recommended = VideoCodecChoice::H264Hardware;
    in.deviceLabel = "TestChip arm64"; in.resolution = "1920x1080@30"; in.timestamp = "2026-06-19T00:00:00Z";
    QVERIFY(saveBenchmarkResult(path, in));
    CodecBenchmarkResult out;
    QVERIFY(loadBenchmarkResult(path, out));
    QCOMPARE(out.h264SafeFeeds, 12);
    QCOMPARE(out.mpeg2SafeFeeds, 5);
    QCOMPARE(out.recommended, VideoCodecChoice::H264Hardware);
    QCOMPARE(out.deviceLabel, in.deviceLabel);
    QCOMPARE(out.resolution, in.resolution);
}

void TestBenchmarkCache::invalidatesOnDeviceOrResolutionChange() {
    CodecBenchmarkResult c; c.deviceLabel = "ChipA arm64"; c.resolution = "1920x1080@30";
    QVERIFY(benchmarkResultMatches(c, "ChipA arm64", "1920x1080@30"));
    QVERIFY(!benchmarkResultMatches(c, "ChipB arm64", "1920x1080@30"));
    QVERIFY(!benchmarkResultMatches(c, "ChipA arm64", "1280x720@30"));
}

void TestBenchmarkCache::deviceLabelIsNonEmpty() {
    QVERIFY(!benchmarkDeviceLabel().isEmpty());
}

QTEST_GUILESS_MAIN(TestBenchmarkCache)
#include "tst_benchmarkcache.moc"
```

Register: `olr_add_unit_test(tst_benchmarkcache olr_test_engine)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_benchmarkcache`
Expected: FAIL to compile — `benchmarkcache.h` not found.

- [ ] **Step 3: Write `benchmarkcache.{h,cpp}`**

Implement `benchmarkDeviceLabel` (QSysInfo), JSON save/load of all `CodecBenchmarkResult` fields (codec enum via `videoCodecToString`/`videoCodecFromString`), and `benchmarkResultMatches` (deviceLabel + resolution equality). Use `QJsonDocument`/`QJsonObject` like `SettingsManager`.

- [ ] **Step 4: Write `runcodecbenchmark.{h,cpp}`**

Implement `runCodecBenchmark`: construct `Mpeg2CodecRunner` + `H264CodecRunner`; `rampCodec` each (H.264 only if `available()`); fill `CodecBenchmarkResult` (`h264Available`, safe feeds, ms, `recommended` per the rule, `deviceLabel = benchmarkDeviceLabel()`, `resolution = WxH@fps`, `ceilingReached`). Leave `timestamp` for the caller to stamp.

- [ ] **Step 5: Run tests**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_benchmarkcache && ctest --test-dir build/claude-debug -R tst_benchmarkcache -V`
Expected: PASS (3 tests).

- [ ] **Step 6: Build all + full unit suite (regression)**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug && ctest --test-dir build/claude-debug -L unit`
Expected: PASS — full build clean, all unit tests green (including the new benchmark tests; the real-codec test exercises VideoToolbox on macOS).

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/benchmark/ tests/unit/tst_benchmarkcache.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat: codec-benchmark top-level entry + device-keyed result cache"
```

---

## Self-Review

**Spec coverage (this plan = subsystem 3 of the design doc):**
- Off-GUI-thread (controller + N pipeline threads; GUI only gets coalesced progress + result) → Task 2 (`rampCodec` is called from a worker thread; per-step coalesced `onStep`) + Task 3 (N std::threads per step) + Task 4 (`runCodecBenchmark` is a blocking worker-thread call). The thin async/threaded GUI wrapper is explicitly deferred to Plan 4's UI wiring; Plan 3 keeps all codec work off the GUI thread by contract (blocking call meant for a worker). ✓
- Measures BOTH codecs; H.264 only if available → Task 4 + `H264CodecRunner::available()`. ✓
- Ramp `1,2,4,8,12,16,20,24,28,32` stop-on-first-fail → Task 1 (`benchmarkRampSteps`) + Task 2 (`rampCodec` breaks on first non-sustained), tested in `tst_codecbenchmark`. ✓
- Sustain criterion (≥0.95 + budget) + safe feed (≥1.2× headroom) → Task 1, tested. ✓
- Recommendation (H.264 if available and `h264SafeFeeds >= mpeg2SafeFeeds`) → Task 4. ✓
- Deterministic synthetic frames → Task 3 (`makeSyntheticFrame`, tested for determinism + motion). ✓
- Cap at 32 + `ceilingReached` log flag (no silent truncation) → Task 1/2 (`ceilingReached`). ✓
- Cancellation → Task 2 (`std::atomic<bool> cancel`), tested. ✓
- Device-keyed cache + invalidation → Task 4, tested. ✓
- Hardware-only H.264 (uses native enc/dec, no software h264) → Task 3 (`H264CodecRunner`). ✓
- Settings/UI surfacing, the benchmark panel, gating → **out of scope** (Plan 4). Not gaps.

**Placeholder scan:** Tasks 1, 2, 4 have complete code for the pure/orchestration/cache layers and full test code. Task 3's runner bodies (the per-thread encode+decode loop) are specified concretely — exact encoder/decoder setup, the `framesRequired`/`budgetMs` math, threading model, and aggregation — but the ~loop body is left for the implementer to assemble from the named existing patterns (`StreamWorker::setupEncoder` for MPEG-2, the `tst_h264_roundtrip` encode+decode for H.264). This is the intended granularity for code that wires several existing APIs together, and it is gated by `tst_realcodecbenchmark`.

**Type consistency:** `RampStepResult`/`BenchmarkConfig`/`CodecBenchmarkResult` (Task 1) are used unchanged in Tasks 2-4. `CodecRunner::runStep(int, const BenchmarkConfig&)` + `available()` (Task 2) match `Mpeg2CodecRunner`/`H264CodecRunner` (Task 3) and the `FakeRunner` in the test. `CodecBenchmark::rampCodec(runner, config, onStep, cancel) -> CodecResult` (Task 2) is called identically in Task 3's test and Task 4's `runCodecBenchmark`. The avcC parse helper (`parseAvcc`) is added to `avcc.{h,cpp}` (Task 3) where the builder already lives, keeping the avcC layout in one place.
