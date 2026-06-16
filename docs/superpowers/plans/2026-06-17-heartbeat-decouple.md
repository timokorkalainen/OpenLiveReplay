# Heartbeat decouple-from-fps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the recording heartbeat (master-pulse scheduler) at a fixed, fps-independent cadence instead of `int(1000/fps)`, and isolate the wall-clock→frame derivation behind one pure, unit-tested helper.

**Architecture:** `onTimerTick` already derives the frame index from the wall-clock (`elapsedMs*fps/1000`) and emits one `masterPulse` per frame with catch-up — the timer is just a scheduler. We extract the derive+catch-up math into a pure free function `heartbeatFrameSpan()`, unit-test it, then make `onTimerTick` call it and start the timer at a fixed `kHeartbeatIntervalMs` with an fps-independent catch-up cap.

**Tech Stack:** C++17, Qt6 Core (QTimer/QElapsedTimer), Qt Test, CMake/Ninja/CTest.

**Spec:** `docs/superpowers/specs/2026-06-17-heartbeat-decouple-design.md`

**Base branch:** `feat/heartbeat-decouple` (off latest `origin/main`). Build dir `build/srt`. **Local-only; do not push** unless told. Format C++ with `/opt/homebrew/opt/llvm/bin/clang-format` — these files are clang-format-clean, but format only the lines you add via `git clang-format` if a long line wraps.

**Verified facts (this branch):**
- Timer start: `replaymanager.cpp:250-251` — `const int intervalMs = qMax(1, static_cast<int>(1000.0 / m_fps)); m_heartbeat->start(intervalMs);`
- `onTimerTick`: `replaymanager.cpp:362-392` (derives `derivedFrame`, per-frame emit, catch-up `maxPerTick = qMax(1, m_fps/4)`, backlog skip `derivedFrame - from >= m_fps`).
- `replaymanager.h`: `m_globalFrameCount` (:103), `m_fps` (:128), `m_heartbeat` (:130), `onTimerTick` (:96).
- App target source list: `CMakeLists.txt:82` (`recorder_engine/replaymanager.cpp ...`).
- `olr_test_core` source list: `tests/CMakeLists.txt` (the `qt_add_library(olr_test_core STATIC ...)` block, first entry `recorder_engine/recordingclock.cpp`). `olr_test_engine` links `olr_test_core` PUBLIC, so it inherits the helper transitively.
- Unit-test registration: `tests/unit/CMakeLists.txt` uses `olr_add_unit_test(name lib)`.
- The codebase uses **global free functions** (no namespace) for helpers like `selectIngestBackend`, `srtHealth`, `augmentSrtUrl`.

---

### Task 1: Pure `heartbeatFrameSpan` helper + unit test

**Files:**
- Create: `recorder_engine/heartbeat.h`
- Create: `recorder_engine/heartbeat.cpp`
- Modify: `tests/CMakeLists.txt` (add to `olr_test_core`)
- Modify: `CMakeLists.txt` (add to the app target)
- Create: `tests/unit/tst_heartbeat.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/unit/tst_heartbeat.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/heartbeat.h"

class TestHeartbeat : public QObject {
    Q_OBJECT
private slots:
    void notAdvancedReturnsEmpty();
    void mapsElapsedToFrameAtStart();
    void singleFrameAdvance();
    void lateTickCappedAtMaxPerTick();
    void backlogSkipWhenFarBehind();
    void fpsZeroReturnsEmpty();
};

void TestHeartbeat::notAdvancedReturnsEmpty() {
    // elapsed 0 ms, no frames yet: derivedFrame 0 <= lastFrame 0 -> empty (to < from).
    const FrameSpan s = heartbeatFrameSpan(0, 30, 0, 8, 30);
    QVERIFY(s.to < s.from);
}

void TestHeartbeat::mapsElapsedToFrameAtStart() {
    // 100 ms @30fps -> frame 3; from a cold start (lastFrame 0) emit 1..3 (catch-up).
    const FrameSpan s = heartbeatFrameSpan(100, 30, 0, 8, 30);
    QCOMPARE(s.from, qint64(1));
    QCOMPARE(s.to, qint64(3));
}

void TestHeartbeat::singleFrameAdvance() {
    // 100 ms @30fps -> frame 3; already at frame 2 -> emit just frame 3.
    const FrameSpan s = heartbeatFrameSpan(100, 30, 2, 8, 30);
    QCOMPARE(s.from, qint64(3));
    QCOMPARE(s.to, qint64(3));
}

void TestHeartbeat::lateTickCappedAtMaxPerTick() {
    // 1000 ms @30fps -> frame 30; cold start would want 1..30 but the burst is
    // capped at maxPerTick(8) -> 1..8 (remainder drains on later ticks). Not far
    // enough behind to trigger the backlog skip (29 < maxBacklog 30).
    const FrameSpan s = heartbeatFrameSpan(1000, 30, 0, 8, 30);
    QCOMPARE(s.from, qint64(1));
    QCOMPARE(s.to, qint64(8));
}

void TestHeartbeat::backlogSkipWhenFarBehind() {
    // 2000 ms @30fps -> frame 60, > 1 s behind (60-1 >= maxBacklog 30): skip ahead
    // to frame 31 and emit 31..38 (capped at 8) so recording resumes near real time.
    const FrameSpan s = heartbeatFrameSpan(2000, 30, 0, 8, 30);
    QCOMPARE(s.from, qint64(31));
    QCOMPARE(s.to, qint64(38));
}

void TestHeartbeat::fpsZeroReturnsEmpty() {
    const FrameSpan s = heartbeatFrameSpan(1000, 0, 0, 8, 30);
    QVERIFY(s.to < s.from);
}

QTEST_GUILESS_MAIN(TestHeartbeat)
#include "tst_heartbeat.moc"
```

- [ ] **Step 2: Register the test** — in `tests/unit/CMakeLists.txt`, after `olr_add_unit_test(tst_recordingclock   olr_test_core)`:

```cmake
olr_add_unit_test(tst_heartbeat        olr_test_core)
```

- [ ] **Step 3: Verify it fails to build** — Run: `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target tst_heartbeat`. Expected: fatal error — `recorder_engine/heartbeat.h` not found (and `heartbeatFrameSpan`/`FrameSpan` undeclared).

- [ ] **Step 4: Create the header** — `recorder_engine/heartbeat.h`:

```cpp
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <cstdint>

// Inclusive range of frame indices to emit on one heartbeat tick. Empty when to < from.
struct FrameSpan {
    int64_t from = 1;
    int64_t to = 0;
};

// Which frame indices a heartbeat tick should emit, given the wall-clock elapsed
// time since recording start. The recording timeline is wall-clock-derived: the
// target frame is elapsedMs*fps/1000. We emit lastFrame+1 .. target, but:
//   - skip ahead when more than maxBacklogFrames behind (a long stall resumes near
//     real time instead of replaying a huge backlog), and
//   - cap the burst at maxPerTick frames (the remainder drains on later ticks) so a
//     catch-up never freezes the caller's thread.
// Returns an empty span (to < from) when the frame count has not advanced or fps<=0.
// fps is the ONLY frame-rate dependence; P1 (rational fps) swaps it here alone.
FrameSpan heartbeatFrameSpan(int64_t elapsedMs, int fps, int64_t lastFrame,
                             int maxPerTick, int64_t maxBacklogFrames);

#endif  // HEARTBEAT_H
```

- [ ] **Step 5: Create the implementation** — `recorder_engine/heartbeat.cpp`:

```cpp
#include "heartbeat.h"

#include <algorithm>

FrameSpan heartbeatFrameSpan(int64_t elapsedMs, int fps, int64_t lastFrame,
                             int maxPerTick, int64_t maxBacklogFrames) {
    FrameSpan span;
    span.from = lastFrame + 1;
    span.to = lastFrame;  // empty until we know there is at least one frame to emit
    if (fps <= 0) {
        return span;
    }
    const int64_t derivedFrame = (elapsedMs * fps) / 1000;
    if (derivedFrame <= lastFrame) {
        return span;  // frame count has not advanced
    }
    int64_t from = lastFrame + 1;
    if (maxBacklogFrames > 0 && derivedFrame - from >= maxBacklogFrames) {
        from = derivedFrame - maxBacklogFrames + 1;  // resume near real time
    }
    const int64_t cap = std::max<int64_t>(1, maxPerTick);
    span.from = from;
    span.to = std::min<int64_t>(derivedFrame, from + cap - 1);
    return span;
}
```

- [ ] **Step 6: Add the source to both build targets.**

In `tests/CMakeLists.txt`, in the `qt_add_library(olr_test_core STATIC` list, immediately after the `recorder_engine/recordingclock.cpp` line:
```cmake
    "${CMAKE_SOURCE_DIR}/recorder_engine/heartbeat.cpp"
```

In `CMakeLists.txt` (root app target), immediately after the `recorder_engine/replaymanager.cpp recorder_engine/replaymanager.h` line (`:82`):
```cmake
        recorder_engine/heartbeat.h recorder_engine/heartbeat.cpp
```

- [ ] **Step 7: Build + run the test, expect PASS** — Run: `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target tst_heartbeat && ( cd build/srt && ctest -R tst_heartbeat --output-on-failure )`. Expected: PASS, 6 test functions.

- [ ] **Step 8: Format changed lines + commit**
```bash
git add recorder_engine/heartbeat.h recorder_engine/heartbeat.cpp \
        tests/unit/tst_heartbeat.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(engine): pure heartbeatFrameSpan() helper (wall-clock -> frame span) + unit test"
```

---

### Task 2: Fixed heartbeat interval + delegate the span math in onTimerTick

**Files:**
- Modify: `recorder_engine/replaymanager.h`
- Modify: `recorder_engine/replaymanager.cpp`

- [ ] **Step 1: Add the constants** — in `recorder_engine/replaymanager.h`, immediately before the member `int m_fps = 30;` (line ~128), add:

```cpp
    // Heartbeat scheduler cadence — fixed and fps-INDEPENDENT. The recording
    // timeline is wall-clock-derived in onTimerTick (see heartbeatFrameSpan), so the
    // timer is only a scheduler. ~125 Hz oversamples every supported frame rate
    // (<=60 fps); surplus wakes are cheap no-ops (the empty-span early-out).
    // kMaxFramesPerTick caps a post-stall catch-up burst so the GUI thread never
    // freezes; the remainder drains on later ticks.
    static constexpr int kHeartbeatIntervalMs = 8;
    static constexpr int kMaxFramesPerTick = 8;
```

- [ ] **Step 2: Include the helper** — in `recorder_engine/replaymanager.cpp`, add near the existing includes (with the other recorder_engine includes):
```cpp
#include "heartbeat.h"
```

- [ ] **Step 3: Start the timer at the fixed interval** — in `startRecording`, replace (`:250-251`):
```cpp
    const int intervalMs = qMax(1, static_cast<int>(1000.0 / m_fps));
    m_heartbeat->start(intervalMs);
```
with:
```cpp
    m_heartbeat->start(kHeartbeatIntervalMs);
```

- [ ] **Step 4: Delegate the span math in `onTimerTick`** — replace the entire body of `onTimerTick` (`:362-392`):
```cpp
void ReplayManager::onTimerTick() {
    if (!m_clock) return;

    const int64_t elapsedMs = m_clock->elapsedMs();
    const int64_t derivedFrame = (elapsedMs * m_fps) / 1000;

    // Only emit when the frame count actually advances.
    if (derivedFrame <= m_globalFrameCount) return;

    // Emit one pulse PER FRAME so late timer ticks don't leave
    // frame-index holes in the video tracks.  Catch-up is capped at one
    // second of backlog (longer stalls resume from the current frame),
    // and drained a few frames per tick so the catch-up itself never
    // freezes the main thread — the remainder follows on later ticks.
    int64_t from = m_globalFrameCount + 1;
    if (derivedFrame - from >= m_fps) {
        from = derivedFrame - m_fps + 1;
    }
    const int64_t maxPerTick = qMax(1, m_fps / 4);
    const int64_t to = qMin(derivedFrame, from + maxPerTick - 1);
    for (int64_t f = from; f <= to; ++f) {
        m_globalFrameCount = f;
        const int64_t frameMs = (f * 1000) / m_fps;

        // 1. Emit masterPulse — source workers do jitter pull + encode
        emit masterPulse(f, frameMs);

        // 2. Write blue frames for any unmapped view-tracks
        writeBlueFrames(frameMs);
    }
}
```
with:
```cpp
void ReplayManager::onTimerTick() {
    if (!m_clock) return;

    // The recording timeline is wall-clock-derived; the (fixed-rate) timer is just a
    // scheduler. heartbeatFrameSpan() turns the elapsed wall-clock into the frame
    // range to emit this tick (with catch-up for late ticks + a 1-second backlog
    // skip). maxBacklogFrames = m_fps == one second of frames.
    const int64_t elapsedMs = m_clock->elapsedMs();
    const FrameSpan span =
        heartbeatFrameSpan(elapsedMs, m_fps, m_globalFrameCount, kMaxFramesPerTick, m_fps);

    for (int64_t f = span.from; f <= span.to; ++f) {
        m_globalFrameCount = f;
        const int64_t frameMs = (f * 1000) / m_fps;

        // 1. Emit masterPulse — source workers do jitter pull + encode
        emit masterPulse(f, frameMs);

        // 2. Write blue frames for any unmapped view-tracks
        writeBlueFrames(frameMs);
    }
}
```
(Behaviour is identical except the catch-up cap is now `kMaxFramesPerTick` (8) instead of `m_fps/4`, and the timer is fixed-rate. The backlog skip stays at `m_fps` = one second.)

- [ ] **Step 5: Build the app + engine** — Run: `cmake --build build/srt --target OpenLiveReplay record_harness`. Expected: clean compile + link. (`heartbeatFrameSpan` resolves via `heartbeat.cpp`, now in both targets.)

- [ ] **Step 6: No-regression on the record e2e** — the output frame count is wall-clock×fps, so a different scheduler rate must not change it. Run: `( cd build/srt && ctest -L e2e -R "e2e_record_stereo|e2e_record_mono" --output-on-failure )`. Expected: both pass (each records 6 s and checks the video packet count band + A/V end-timestamps within 0.75 s). If either FAILS, capture the output and report — do NOT loosen the gate.

- [ ] **Step 7: Format changed lines + commit**
```bash
git add recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp
git commit -m "feat(engine): fixed fps-independent heartbeat interval via heartbeatFrameSpan"
```

---

## After all tasks
- `( cd build/srt && ctest -L unit --output-on-failure )` — all unit tests incl. `tst_heartbeat`.
- `( cd build/srt && ctest -L e2e --output-on-failure )` — the record + playback e2e (the heartbeat drives both record and the scrub/duration UI; confirm no regression).
- Quick sanity: `grep -n "1000.0 / m_fps\|1000 / m_fps" recorder_engine/replaymanager.cpp` should NOT match the timer-interval line anymore (the only remaining `*1000)/m_fps` is the frame→ms math, which is intentional/P1).
- Dispatch a final code review over the branch.
- Use superpowers:finishing-a-development-branch. **Do NOT push** unless explicitly told.
