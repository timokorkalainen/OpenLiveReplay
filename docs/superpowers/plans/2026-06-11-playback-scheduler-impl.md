# Playback Scheduler Redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. **The authoritative design is the spec** at `docs/superpowers/specs/2026-06-11-playback-scheduler-redesign.md` (v3) — its §-numbers are referenced throughout; implement those sections exactly. This plan adds file paths, interfaces, tests, and acceptance gates.

**Goal:** Replace `PlaybackWorker`'s decode loop with a windowed, demand-driven, bidirectional frame scheduler that eliminates the runtime-confirmed ~90-seek/s playback storm and makes scrub/step/reverse/live-chase smooth and A/V-synced.

**Architecture:** A bounded per-track decoded-frame **window** around the playhead (`TrackBuffer`); delivery = "largest PTS ≤ P" with direction-aware dedup; audio decoupled from the video window via a worker-side **`AudioFrameQueue`** that releases only `pts ≤ P + kAudioLeadMs` into the untouched 500 ms AudioPlayer ring; reverse exploits intra-only frames (fill-a-chunk-then-deliver); forward overrun degrades via bounded skip-forward, not repositioning; live-EOF un-latches only on file growth. Telemetry counters make every behavior falsifiable from a headless harness.

**Tech Stack:** C++17, Qt 6.10 (Core/Multimedia/Gui/Test), FFmpeg (libav*), CMake + Ninja, CTest. Build/test commands in "Environment" below.

---

## Environment (every build/test step uses these)

- Worktree: `/tmp/olr-sched` (branch `fix/playback-scheduler`, created in Task 0).
- Configure once (tests are opt-in — `-DOLR_BUILD_TESTS=ON` is REQUIRED): `cmake -S /tmp/olr-sched -B /tmp/olr-sched/build -G Ninja -DCMAKE_MAKE_PROGRAM=/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/Users/timo.korkalainen/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`
- The build dir is already configured with tests ON. Re-run the configure line only if CMakeLists changes.
- Build all: `/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build`
- Run a unit test: `ctest --test-dir /tmp/olr-sched/build -R <name> -V`
- ffmpeg/ffprobe at `/opt/homebrew/bin`. Ninja at `/Users/timo.korkalainen/Qt/Tools/Ninja/ninja`. cmake at `/opt/homebrew/bin/cmake`.
- **Note on sanitizers:** tests link `olr_sanitize` (ASan/UBSan). Code must be leak/UB-clean.

---

## File structure (decisions locked here)

- `playback/trackbuffer.h` + `.cpp` — **NEW.** Pure windowed frame store: `(ptsMs, QVideoFrame)` sorted unique-by-PTS, with window queries/trim. No ffmpeg, no threads. One responsibility: the per-track decoded-frame window.
- `playback/audioframequeue.h` + `.cpp` — **NEW.** Worker-side PTS-tagged PCM queue that releases against the playhead. No ffmpeg device; just buffering + a release cursor. One responsibility: pacing audio to `P + kAudioLeadMs`.
- `playback/playbackworker.h` / `.cpp` — **REWRITE the run loop + add telemetry**; `DecoderTrack` holds a `TrackBuffer`; new private scheduler helpers; counters struct + accessors.
- `playback/audioplayer.h` / `.cpp` — **MINIMAL:** add an `audioClear` counter + accessor (the only behavior-neutral change; internals stay out of scope per spec §10).
- `uimanager.cpp` / `.h` — `seekPlayback`→`seekTo`, `lastMoveDir`, remove `setFrameBufferMax` calls (spec §7).
- `tests/unit/tst_trackbuffer.cpp`, `tests/unit/tst_audioframequeue.cpp` — **NEW** unit tests.
- `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt` — wire a new `olr_test_playback` lib + the two unit tests.
- `tests/e2e/play_harness.cpp` + `run_playback_e2e.sh` + `CMakeLists.txt` — **NEW** headless playback harness + CTest scenarios (spec §11).

---

## Task 0: Branch

**Files:** none (git only)

- [ ] **Step 1: Create the branch in the worktree**
```bash
cd /tmp/olr-sched && git checkout -B fix/playback-scheduler origin/main
```
- [ ] **Step 2: Configure + baseline build to confirm the tree is green**
```bash
cmake -S /tmp/olr-sched -B /tmp/olr-sched/build -G Ninja -DCMAKE_MAKE_PROGRAM=/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/Users/timo.korkalainen/Qt/6.10.1/macos
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build
```
Expected: links `appOpenLiveReplay` and the existing test exes with no errors.

---

## Task 1: `TrackBuffer` — windowed per-track frame store (unit-tested first)

**Files:**
- Create: `playback/trackbuffer.h`, `playback/trackbuffer.cpp`
- Create: `tests/unit/tst_trackbuffer.cpp`
- Modify: `tests/CMakeLists.txt` (add `olr_test_playback` lib), `tests/unit/CMakeLists.txt` (register test)

**Interface (implement exactly — these names are referenced by later tasks):**
```cpp
// playback/trackbuffer.h
#ifndef TRACKBUFFER_H
#define TRACKBUFFER_H
#include <QVideoFrame>
#include <QVector>
#include <cstdint>

// One video track's decoded-frame window. Frames are kept sorted ascending
// by PTS and unique by PTS. Pure data structure: no ffmpeg, no threads.
// The owner serializes access (PlaybackWorker::m_bufferMutex).
class TrackBuffer {
public:
    struct Frame { int64_t ptsMs = -1; QVideoFrame frame; };

    // Insert sorted, unique by PTS (existing PTS is replaced). Enforces the
    // frame cap by evicting the entry farthest (in PTS) from keepNearMs,
    // but never one whose PTS is in [keepNearMs, protectToMs] (the live
    // fill edge). Returns false if the frame was dropped by the cap.
    bool insert(int64_t ptsMs, const QVideoFrame& f,
                int capFrames, int64_t keepNearMs, int64_t protectToMs);

    // The frame to display at playhead: largest PTS <= playheadMs.
    // Returns false (out untouched) if no such frame.
    bool frameAt(int64_t playheadMs, QVideoFrame& out, int64_t& outPtsMs) const;

    // True iff a frame exists within +/- toleranceMs of targetMs.
    bool hasFrameNear(int64_t targetMs, int64_t toleranceMs) const;

    int64_t newestPts() const;          // -1 if empty
    int64_t oldestPts() const;          // -1 if empty
    bool isEmpty() const { return m_frames.isEmpty(); }
    int  size() const { return m_frames.size(); }

    // Drop frames with PTS < keepFromMs or PTS > keepToMs.
    void trim(int64_t keepFromMs, int64_t keepToMs);
    void clear() { m_frames.clear(); }

private:
    QVector<Frame> m_frames; // sorted ascending by ptsMs, unique
};
#endif
```

- [ ] **Step 1: Write the failing unit test** `tests/unit/tst_trackbuffer.cpp`:
```cpp
#include <QtTest>
#include "playback/trackbuffer.h"

// A distinct, mappable frame per pts so identity is checkable.
static QVideoFrame makeFrame() {
    QVideoFrameFormat fmt(QSize(16, 16), QVideoFrameFormat::Format_YUV420P);
    return QVideoFrame(fmt);
}

class TestTrackBuffer : public QObject {
    Q_OBJECT
private slots:
    void insertKeepsSortedUniqueAndCap();
    void frameAtPicksLargestLeqPlayhead();
    void frameAtEmptyAndBeforeFirst();
    void hasFrameNearTolerance();
    void trimDropsOutsideRange();
    void capProtectsFillEdge();
};

void TestTrackBuffer::insertKeepsSortedUniqueAndCap() {
    TrackBuffer b;
    // Insert out of order; cap large so nothing drops.
    QVERIFY(b.insert(300, makeFrame(), 100, 0, 100000));
    QVERIFY(b.insert(100, makeFrame(), 100, 0, 100000));
    QVERIFY(b.insert(200, makeFrame(), 100, 0, 100000));
    QCOMPARE(b.size(), 3);
    QCOMPARE(b.oldestPts(), int64_t(100));
    QCOMPARE(b.newestPts(), int64_t(300));
    // Duplicate PTS replaces, not grows.
    QVERIFY(b.insert(200, makeFrame(), 100, 0, 100000));
    QCOMPARE(b.size(), 3);
}

void TestTrackBuffer::frameAtPicksLargestLeqPlayhead() {
    TrackBuffer b;
    b.insert(100, makeFrame(), 100, 0, 100000);
    b.insert(200, makeFrame(), 100, 0, 100000);
    b.insert(300, makeFrame(), 100, 0, 100000);
    QVideoFrame out; int64_t pts = -1;
    QVERIFY(b.frameAt(250, out, pts)); QCOMPARE(pts, int64_t(200));
    QVERIFY(b.frameAt(300, out, pts)); QCOMPARE(pts, int64_t(300)); // exact
    QVERIFY(b.frameAt(100, out, pts)); QCOMPARE(pts, int64_t(100));
}

void TestTrackBuffer::frameAtEmptyAndBeforeFirst() {
    TrackBuffer b;
    QVideoFrame out; int64_t pts = -1;
    QVERIFY(!b.frameAt(50, out, pts));           // empty
    b.insert(100, makeFrame(), 100, 0, 100000);
    QVERIFY(!b.frameAt(50, out, pts));           // before first
}

void TestTrackBuffer::hasFrameNearTolerance() {
    TrackBuffer b;
    b.insert(1000, makeFrame(), 100, 0, 100000);
    QVERIFY(b.hasFrameNear(1015, 16));   // within 16ms
    QVERIFY(!b.hasFrameNear(1100, 16));  // 100ms away
}

void TestTrackBuffer::trimDropsOutsideRange() {
    TrackBuffer b;
    for (int64_t p = 0; p <= 1000; p += 100) b.insert(p, makeFrame(), 100, 0, 100000);
    b.trim(300, 700);
    QCOMPARE(b.oldestPts(), int64_t(300));
    QCOMPARE(b.newestPts(), int64_t(700));
}

void TestTrackBuffer::capProtectsFillEdge() {
    TrackBuffer b;
    // cap=3, playhead near 1000, protect up to 1300 (the fill edge).
    // Insert 4 frames around+ahead of the playhead; the farthest-from-1000
    // BELOW the protect range is evicted, never the protected edge frames.
    b.insert(900,  makeFrame(), 3, 1000, 1300);
    b.insert(1000, makeFrame(), 3, 1000, 1300);
    b.insert(1100, makeFrame(), 3, 1000, 1300);
    b.insert(1300, makeFrame(), 3, 1000, 1300); // would be 4; cap evicts 900
    QCOMPARE(b.size(), 3);
    QCOMPARE(b.oldestPts(), int64_t(1000));      // 900 evicted (farthest, unprotected)
    QVERIFY(b.hasFrameNear(1300, 1));            // protected edge kept
}
QTEST_APPLESS_MAIN(TestTrackBuffer)
#include "tst_trackbuffer.moc"
```

- [ ] **Step 2: Wire build so the test compiles (and fails to link `TrackBuffer`).** In `tests/CMakeLists.txt`, after the `olr_test_engine` block, add a playback lib:
```cmake
# --- Playback sources (need Qt Multimedia for QVideoFrame; ffmpeg for the worker) ---
qt_add_library(olr_test_playback STATIC
    "${CMAKE_SOURCE_DIR}/playback/trackbuffer.cpp"
)
target_include_directories(olr_test_playback PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(olr_test_playback
    PUBLIC olr_test_core Qt6::Core Qt6::Multimedia Qt6::Gui
    PRIVATE olr_warnings olr_sanitize)
```
In `tests/unit/CMakeLists.txt` add: `olr_add_unit_test(tst_trackbuffer olr_test_playback)`

- [ ] **Step 3: Run the test, expect FAIL (missing `trackbuffer.cpp`/link error)**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build tst_trackbuffer
```
Expected: build/link failure (no `TrackBuffer` symbols) — confirms the test drives new code.

- [ ] **Step 4: Implement `playback/trackbuffer.cpp`** to satisfy the interface and tests. `insert`: binary-search the sorted vector; replace on equal PTS; else insert at position; if `size() > capFrames`, evict the entry maximizing `|pts − keepNearMs|` among entries **not** in `[keepNearMs, protectToMs]` (if all are protected, evict the global farthest). `frameAt`: reverse scan / upper_bound for largest `pts ≤ playheadMs`. `hasFrameNear`: scan for any `|pts − targetMs| ≤ toleranceMs`. `trim`: drop outside `[keepFromMs, keepToMs]`. Keep it allocation-light (operate in place on `m_frames`).

- [ ] **Step 5: Run the test, expect PASS**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build tst_trackbuffer && ctest --test-dir /tmp/olr-sched/build -R tst_trackbuffer -V
```
Expected: `Passed   tst_trackbuffer`.

- [ ] **Step 6: Commit**
```bash
cd /tmp/olr-sched && git add playback/trackbuffer.h playback/trackbuffer.cpp tests/unit/tst_trackbuffer.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "Add TrackBuffer: windowed per-track decoded-frame store (unit-tested)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `AudioFrameQueue` — worker-side PTS pacing queue (unit-tested first)

**Files:**
- Create: `playback/audioframequeue.h`, `.cpp`
- Create: `tests/unit/tst_audioframequeue.cpp`
- Modify: `tests/CMakeLists.txt` (add `audioframequeue.cpp` to `olr_test_playback`), `tests/unit/CMakeLists.txt`

**Interface:**
```cpp
// playback/audioframequeue.h
#ifndef AUDIOFRAMEQUEUE_H
#define AUDIOFRAMEQUEUE_H
#include <QByteArray>
#include <QQueue>
#include <cstdint>

// Holds decoded PCM audio frames (interleaved S16 stereo) tagged with their
// media PTS, and releases them paced against the playhead. Decouples the
// audio push position from the video decode window (spec §6.7): the video
// window may lead ~500ms, but only frames within kAudioLeadMs of the
// playhead are released into the (500ms-capped) AudioPlayer ring.
class AudioFrameQueue {
public:
    struct Frame { int64_t ptsMs; QByteArray pcm; };

    void enqueue(int64_t ptsMs, const char* data, int bytes);

    // Pop the next frame iff its PTS <= playheadMs + leadMs. Returns false
    // when nothing is due. FIFO by enqueue order (== PTS order in normal play).
    bool releaseDue(int64_t playheadMs, int64_t leadMs, Frame& out);

    // Drop everything older than (playheadMs - keepBehindMs): bounds memory.
    void dropOlderThan(int64_t playheadMs, int64_t keepBehindMs);

    void clear();
    bool isEmpty() const { return m_q.isEmpty(); }
    int  spanMs() const; // newest.pts - oldest.pts, 0 if <2

private:
    QQueue<Frame> m_q;
};
#endif
```

- [ ] **Step 1: Write the failing test** `tests/unit/tst_audioframequeue.cpp`:
```cpp
#include <QtTest>
#include "playback/audioframequeue.h"

static QByteArray pcm(int bytes) { return QByteArray(bytes, '\0'); }

class TestAudioFrameQueue : public QObject {
    Q_OBJECT
private slots:
    void releasesOnlyWithinLead();
    void releaseIsFifo();
    void dropOlderThanBoundsMemory();
    void clearEmpties();
};

void TestAudioFrameQueue::releasesOnlyWithinLead() {
    AudioFrameQueue q;
    q.enqueue(1000, pcm(8).constData(), 8);
    q.enqueue(1100, pcm(8).constData(), 8);
    q.enqueue(1400, pcm(8).constData(), 8);
    AudioFrameQueue::Frame f;
    // playhead 1000, lead 200 -> releases up to pts 1200: 1000 then 1100.
    QVERIFY(q.releaseDue(1000, 200, f)); QCOMPARE(f.ptsMs, int64_t(1000));
    QVERIFY(q.releaseDue(1000, 200, f)); QCOMPARE(f.ptsMs, int64_t(1100));
    QVERIFY(!q.releaseDue(1000, 200, f)); // 1400 > 1200, not due
    // playhead advances; 1400 becomes due.
    QVERIFY(q.releaseDue(1300, 200, f)); QCOMPARE(f.ptsMs, int64_t(1400));
}

void TestAudioFrameQueue::releaseIsFifo() {
    AudioFrameQueue q;
    q.enqueue(100, pcm(4).constData(), 4);
    q.enqueue(200, pcm(4).constData(), 4);
    AudioFrameQueue::Frame f;
    QVERIFY(q.releaseDue(10000, 0, f)); QCOMPARE(f.ptsMs, int64_t(100));
    QVERIFY(q.releaseDue(10000, 0, f)); QCOMPARE(f.ptsMs, int64_t(200));
}

void TestAudioFrameQueue::dropOlderThanBoundsMemory() {
    AudioFrameQueue q;
    for (int64_t p = 0; p <= 1000; p += 100) q.enqueue(p, pcm(4).constData(), 4);
    q.dropOlderThan(900, 200); // keep pts >= 700
    AudioFrameQueue::Frame f;
    QVERIFY(q.releaseDue(10000, 0, f)); QCOMPARE(f.ptsMs, int64_t(700));
}

void TestAudioFrameQueue::clearEmpties() {
    AudioFrameQueue q;
    q.enqueue(1, pcm(4).constData(), 4);
    q.clear();
    QVERIFY(q.isEmpty());
}
QTEST_APPLESS_MAIN(TestAudioFrameQueue)
#include "tst_audioframequeue.moc"
```

- [ ] **Step 2: Wire build.** Add `playback/audioframequeue.cpp` to the `olr_test_playback` sources in `tests/CMakeLists.txt`; add `olr_add_unit_test(tst_audioframequeue olr_test_playback)` in `tests/unit/CMakeLists.txt`.

- [ ] **Step 3: Run, expect FAIL (link error).**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build tst_audioframequeue
```

- [ ] **Step 4: Implement `playback/audioframequeue.cpp`** per the interface: `enqueue` appends; `releaseDue` peeks head, pops+returns if `head.ptsMs <= playheadMs + leadMs`; `dropOlderThan` pops while `head.ptsMs < playheadMs - keepBehindMs`; `spanMs` = back.pts − front.pts.

- [ ] **Step 5: Run, expect PASS.**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build tst_audioframequeue && ctest --test-dir /tmp/olr-sched/build -R tst_audioframequeue -V
```

- [ ] **Step 6: Commit**
```bash
cd /tmp/olr-sched && git add playback/audioframequeue.h playback/audioframequeue.cpp tests/unit/tst_audioframequeue.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "Add AudioFrameQueue: PTS-paced worker-side audio buffer (unit-tested)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Telemetry counters (the harness's measurement surface)

**Files:**
- Modify: `playback/playbackworker.h` (counters struct + accessor), `playback/playbackworker.cpp` (emit behind `OLR_PB_TELEMETRY`)
- Modify: `playback/audioplayer.h` / `.cpp` (`audioClear` counter + accessor)

This task only **adds** instrumentation; behavior is unchanged so the build stays green. Counters are written on the worker thread, read via accessor after `stop()`/`wait()` (or for live SEC lines, plain reads — they're diagnostics).

- [ ] **Step 1:** In `playback/playbackworker.h`, add a public struct + accessor and members:
```cpp
struct PlaybackCounters {
    int reposition = 0, reuseSeek = 0, reverseChunkSeek = 0,
        eofTailSeek = 0, skipForward = 0, audioPushes = 0, framesDropped = 0;
};
// ... in PlaybackWorker public:
PlaybackCounters counters() const { return m_counters; }
// ... private:
PlaybackCounters m_counters;
```
- [ ] **Step 2:** In `playback/audioplayer.h` add `int clearCount() const { return m_clearCount; }` and `int m_clearCount = 0;`; in `audioplayer.cpp` `AudioPlayer::clear()` increment `m_clearCount` (under the existing mutex). This is the only AudioPlayer change (spec §10 testability carve-out).
- [ ] **Step 3:** Add a private helper to `PlaybackWorker` for SEC telemetry lines, compiled only when the env flag is set at runtime:
```cpp
// playbackworker.cpp — call once per wall-second from the loop in Task 5.
void PlaybackWorker::emitTelemetry(int64_t P, int64_t newest, double speed) {
    if (!qEnvironmentVariableIsSet("OLR_PB_TELEMETRY")) return;
    fprintf(stderr, "SEC repos=%d reuse=%d revseek=%d eof=%d skip=%d apush=%d drop=%d P=%lld newest=%lld spd=%.2f\n",
            m_counters.reposition, m_counters.reuseSeek, m_counters.reverseChunkSeek,
            m_counters.eofTailSeek, m_counters.skipForward, m_counters.audioPushes,
            m_counters.framesDropped, (long long)P, (long long)newest, speed);
}
```
Declare `void emitTelemetry(int64_t,int64_t,double);` in the header. (It is *called* in Task 5; defining it now keeps Task 5 focused.)

- [ ] **Step 4: Build, expect PASS (no behavior change).**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build
```
Expected: clean build; `-Werror` clean (the new helper is referenced — if the compiler warns "unused", mark it `[[maybe_unused]]` until Task 5 wires it, or land Task 3+5 together; prefer `[[maybe_unused]]`).

- [ ] **Step 5: Commit**
```bash
cd /tmp/olr-sched && git add playback/playbackworker.h playback/playbackworker.cpp playback/audioplayer.h playback/audioplayer.cpp
git commit -m "Add playback telemetry counters + AudioPlayer clear counter

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Scheduler scaffolding — constants, symbols, `DecoderTrack` uses `TrackBuffer`

**Files:** Modify `playback/playbackworker.h`, `playback/playbackworker.cpp`

This is the structural refactor that the new loop (Task 5) builds on. It replaces `DecoderTrack::buffer` (`QVector<BufferedFrame>`) with a `TrackBuffer`, adds the spec §3 constants, and adds the symbol helpers — **without yet changing the loop logic** (the existing loop is adapted to the new buffer type so the build stays green and behavior is unchanged this task).

- [ ] **Step 1:** In `playback/playbackworker.h`: `#include "playback/trackbuffer.h"` and `#include "playback/audioframequeue.h"`. Change `DecoderTrack`:
```cpp
struct DecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    FrameProvider* provider = nullptr;
    int streamIndex = -1;
    TrackBuffer buffer;                 // was QVector<BufferedFrame>
    int64_t lastDeliveredPtsMs = -1;
};
```
Add the spec §3 constants as `static constexpr int` members (kLeadMs=500, kTrailMs=300, kChunkMs=500, kAudioLeadMs=200, kAudioQueueMs=900, kSlackMs=200, kIdleSleepMs=3, kEofSleepMs=10, kReadErrSleepMs=20, kBackJumpSlackMs=150, kDecimateAbove value as `double`, kGlobalFrameBudget=256). Add members: `AudioFrameQueue m_audioQueue; int m_lastMoveDir = 1; int64_t m_sizeAtLastEof = -1;`. Add private helper declarations used by Task 5 (signatures fixed here so Task 5 only fills bodies):
```cpp
int  fps() const;                 // m_transport->fps(), clamped >=1
int  capFrames(int trackCount) const;
int64_t frameDurMs() const;       // 1000/fps()
int64_t newestPtsMin() const;     // staleness-excluded; -1 empty   (holds m_bufferMutex)
int64_t oldestPtsMin() const;
int64_t newestPtsMax() const;
int64_t refNewestPts() const;
void repositionTo(int64_t target, int dir, AVPacket* pkt, AVFrame* vf, AVFrame* af);
bool reuseAt(int64_t target);     // true if every track has a frame within frameDurMs/2
```
- [ ] **Step 2:** In `playback/playbackworker.cpp`, adapt the **existing** loop's buffer touches to the `TrackBuffer` API so it still compiles and behaves the same for now: replace `track->buffer.append({pts, qFrame})` + manual cap-trim with `track->buffer.insert(pts, qFrame, m_frameBufferMax, pts, pts)`; replace `track->buffer.clear()` (unchanged name); in `deliverDueFrames`/`deliverBufferedFrameAtOrBefore` replace the reverse-scan over `track->buffer[i]` with `QVideoFrame f; int64_t p; if (track->buffer.frameAt(target, f, p)) {...}` using the existing dedup logic. Keep `m_frameBufferMax` for now (removed in Task 5). Do **not** change loop control flow yet.

- [ ] **Step 3: Build + run existing playback transport test (regression) + full build**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build && ctest --test-dir /tmp/olr-sched/build -R tst_playbacktransport -V
```
Expected: clean build, transport test passes (it's unrelated but confirms nothing broke). The helper bodies added in Step 1 may be stubbed (`return -1;`/`return false;`) — mark `[[maybe_unused]]` or stub minimally; Task 5 fills them. They must compile.

- [ ] **Step 4: Commit**
```bash
cd /tmp/olr-sched && git add playback/playbackworker.h playback/playbackworker.cpp
git commit -m "Refactor DecoderTrack onto TrackBuffer; add scheduler constants/helpers

Behavior unchanged; loop still uses the old control flow against the new
buffer type. The window scheduler replaces the loop in the next commit.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: The window scheduler — rewrite `run()`'s playback loop (spec §6)

**Files:** Modify `playback/playbackworker.cpp` (the loop body after init), `.h` if a helper signature needs adjusting.

This is the core. **Implement spec §6 exactly** (it is the authority; it survived two adversarial review rounds). The opening/decoder-init section (lines ~99–214 of the current file) is unchanged. Replace the playback `while (!shouldInterrupt())` body (current lines ~216–463) with the scheduler. Fill the Task-4 helper bodies.

Required structure (maps 1:1 to spec §6 — do not deviate):
- **Per-iteration:** sample `P=m_transport->currentPos()`, `playing`, `speed`, `dir` (playing→`sign(speed)`; paused→`m_lastMoveDir`), `fps`. Emit telemetry once per wall-second via `emitTelemetry`.
- **Pause (§6.9):** while `!playing`, block; wake on `m_seekTargetMs>=0` OR `P` left the delivered frame's interval (recompute `P`); on wake fall through to classify.
- **Classify (§6.1) in priority:** (1) explicit seek (coalesce latest `m_seekTargetMs`, clear) → `repositionTo(target, anchorDir)`; (2) backward jump `P < oldestPtsMin() − kBackJumpSlackMs` → `repositionTo(P, dir=-1)`; (3) forward lag `playing && dir==+1 && newestPtsMin() < P − kLeadMs` → if `P > newestPtsMax()` and no growth → tail-hold (§6.8) else skip-forward (§6.5, `m_counters.skipForward++`); (4) else deliver+fill+trim+audio+wait.
- **repositionTo (§6.2):** reuse fast-path = `reuseAt(target)` (every track `hasFrameNear(target, frameDurMs/2)`) → reset dedup, `deliverDueFrames(target)`, audio re-prime if backward (`m_audioPlayer->clear()` + queue clear), `m_counters.reuseSeek++`, return. Else full reposition: clear all `TrackBuffer`s + `m_audioQueue.clear()` + `m_audioPlayer->clear()`; `seekAnchor = max(0, target − (dir<0 ? kLeadMs : kTrailMs))`; `av_seek_frame(BACKWARD)`; decode forward (no `avcodec_flush_buffers`) through `target + frameDurMs` inserting all tracks; reset dedup; `deliverDueFrames(target)`; `m_counters.reposition++`.
- **Forward fill (§6.3):** while `newestPtsMin() < P + kLeadMs` && not EOF && batch `< kFillBatch`: `av_read_frame`; video → count-based decimation when `|speed|>kDecimateAbove` (per-track keep-counter, keep every `ceil(|speed|)`-th frame, drop others before `send_packet`) else decode+convert+`buffer.insert(pts, qFrame, capFrames(N), P, P+kLeadMs)` (insert returns false → `m_counters.framesDropped++`); audio (§6.7); subtitle ignored. Terminate by just-read PTS `> P + kLeadMs + kSlackMs` OR cap-at-edge OR batch.
- **Reverse fill (§6.4) fill-then-deliver:** if `refOldestPts() > P − kLeadMs` && `P>0`: record avio pos at oldest; `av_seek_frame(BACKWARD)` to `max(0, P − kLeadMs − kChunkMs)`; decode forward **without delivering** until read cursor reaches the recorded pos OR `kReverseChunkBudget`; hold last frame meanwhile; then top-down reverse delivery resumes (dedup §5); abort on `m_seekTargetMs>=0`; `m_counters.reverseChunkSeek++`.
- **Delivery (§5):** `deliverDueFrames(P)` with direction-aware dedup — forward deliver iff `pts > lastDeliveredPtsMs || lastDeliveredPtsMs<0`; reverse iff `pts < lastDeliveredPtsMs || lastDeliveredPtsMs<0`. (Generalize the existing `deliverDueFrames`.)
- **Trim (§6.6):** forward keep `[P−(kTrailMs+kSlackMs), P+(kLeadMs+kSlackMs)]`; reverse keep `[P−(kLeadMs+kChunkMs+kSlackMs), P+(kTrailMs+kSlackMs)]` via `buffer.trim`. `m_audioQueue.dropOlderThan(P, kAudioLeadMs)`.
- **Audio (§6.7):** during forward decode of the active view, `m_audioQueue.enqueue(ptsMs, data, bytes)` (don't push directly). Each iteration, while 1× && forward && playing && single active view && unmuted: `while (m_audioQueue.releaseDue(P, kAudioLeadMs, f)) m_audioPlayer->pushSamples(f.pcm..., f.ptsMs, P); m_counters.audioPushes++`. Backward reuse/reposition/`setActiveAudioView`/speed≠1 → clear queue+ring (`audioClear`).
- **EOF/tail (§6.8):** on `AVERROR_EOF`: `msleep(kEofSleepMs)`; `int64_t sz = avio_size(m_fmtCtx->pb);` if `sz > m_sizeAtLastEof` → `pb->eof_reached=0; pb->error=0; avformat_flush; av_seek_frame(BACKWARD, refNewestPts())` (check ret); set `m_sizeAtLastEof=sz`; after un-latch, **dedup-before-decode** (skip packets with `pts <= owning track newestPts` before `send_packet`); `m_counters.eofTailSeek++`. Else just sleep. Non-EOF read error: `msleep(kReadErrSleepMs)`, bounded retry.
- **Wait (§6.9):** window full + playing → `msleep(kIdleSleepMs)`.
- **Remove** `m_frameBufferMax` and the old pace-gate `if (lastProcessedPtsMs > masterTimeMs+100) msleep(5)`.

- [ ] **Step 1: Implement the helper bodies and the new loop** per the structure above and spec §6.
- [ ] **Step 2: Build clean (`-Werror`).** `/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build`
- [ ] **Step 3: Smoke via the harness (full acceptance is Task 8).** After Task 7 wires the harness this becomes the real gate; for now confirm the app target links and `tst_playbacktransport` still passes.
```bash
ctest --test-dir /tmp/olr-sched/build -R tst_playbacktransport -V
```
- [ ] **Step 4: Commit**
```bash
cd /tmp/olr-sched && git add playback/playbackworker.cpp playback/playbackworker.h
git commit -m "Rewrite playback as a windowed demand-driven scheduler (spec v3 §6)

Bounded window replaces read-ahead + 500ms drift hard-seek. Direction-
aware delivery/trim, fill-then-deliver reverse, skip-forward for forward
overrun, growth-gated live-EOF, audio decoupled via AudioFrameQueue.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: UI/transport call-site changes (spec §7)

**Files:** Modify `uimanager.cpp` (and `.h` if needed)

- [ ] **Step 1:** `UIManager::seekPlayback(int64_t ms)` (currently only `m_transport->seek(ms)` + `m_audioPlayer->clear()`): also call `if (m_playbackWorker) m_playbackWorker->seekTo(ms);` and record direction `m_lastSeekPrevPos`→ pass to worker is implicit (worker derives `lastMoveDir` from `sign(target − currentPos)` inside `seekTo`). **Update `PlaybackWorker::seekTo`** to set `m_lastMoveDir = (timestampMs >= m_transport->currentPos()) ? 1 : -1;` under `m_mutex` before storing the target.
- [ ] **Step 2:** Remove the three `m_playbackWorker->setFrameBufferMax(...)` calls (uimanager.cpp ~704, ~912, ~935) and delete the `setFrameBufferMax` method from `playbackworker.h`/`.cpp`. In `setRecordFps`, the worker now reads fps live from the transport (already `m_transport->setFps(fps)` is called), so no replacement is needed.
- [ ] **Step 3:** Drop the redundant `m_playbackWorker->deliverBufferedFrameAtOrBefore(targetMs)` calls preceding `seekTo` in `stepFrame`/`stepFrameBack`/MIDI jog (uimanager.cpp ~209, ~831, ~844) — `seekTo` + the scheduler's reuse path deliver. Keep `deliverBufferedFrameAtOrBefore` in the worker only if still referenced; otherwise remove it.
- [ ] **Step 4: Build clean.** `/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build`
- [ ] **Step 5: Commit**
```bash
cd /tmp/olr-sched && git add uimanager.cpp uimanager.h playback/playbackworker.cpp playback/playbackworker.h
git commit -m "Route slider scrub through the worker; drop setFrameBufferMax (spec v3 §7)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: Playback harness in `tests/e2e` (spec §11.1)

**Files:** Create `tests/e2e/play_harness.cpp`, `tests/e2e/run_playback_e2e.sh`; modify `tests/e2e/CMakeLists.txt`. Also extend `tests/e2e/record_harness.cpp` to honor an `OLR_VIEWS` env (record N view-tracks) so multi-track fixtures exist.

- [ ] **Step 1:** Extend `record_harness.cpp`: read `OLR_VIEWS` (default 2), set `mgr.setViewCount(vc)`, build the `viewNames`/`updateViewMapping({0,-1,-1,...})` from it (mirror the inline harness used during the audit). Keep existing CLI args.
- [ ] **Step 2:** Create `tests/e2e/play_harness.cpp` — the real `PlaybackWorker` driver. It constructs N `FrameProvider`s, a `PlaybackTransport`, an `AudioPlayer` (started, **unmuted** for the active view), a `PlaybackWorker`; opens a file; and runs a named scenario (`play1x|seekplay|reverse|liveedge|stepscrub|sliderscrub|speedflip`) driving the transport/worker, then prints the worker's `counters()` and per-second `SEC` telemetry (env `OLR_PB_TELEMETRY=1`). (Base it on the audit harness `/tmp/olr-pbharness/play_harness.cpp` — promote and clean it up; add the `sliderscrub` path that calls only `transport.seek` then, post-§7, `seekPlayback`-equivalent `worker.seekTo`, and `speedflip`.)
- [ ] **Step 3:** Create `tests/e2e/run_playback_e2e.sh` — records a fixture (via `record_harness` with `OLR_VIEWS`, duration per the scenario's needs), runs `play_harness <fixture> <scenario> <views>`, parses the final counters, and asserts the spec §11.2–11.4 thresholds for that scenario (e.g. `play1x 2` ⇒ `reposition==0`, audio pushes present, no `audioClear` after warm-up). Skip gracefully if ffmpeg absent (mirror `run_record_e2e.sh`).
- [ ] **Step 4:** In `tests/e2e/CMakeLists.txt`: add `qt_add_executable(play_harness play_harness.cpp)` linking `Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback` + the worker/frameprovider/audioplayer sources (extend `olr_test_playback` to include `playbackworker.cpp`, `frameprovider.cpp`, `audioplayer.cpp` + ffmpeg + `Qt6::Multimedia`); register `add_test(NAME e2e_play_storm COMMAND bash run_playback_e2e.sh $<TARGET_FILE:play_harness> $<TARGET_FILE:record_harness> play1x 2)` with `LABELS e2e; RUN_SERIAL TRUE; TIMEOUT 180`.
- [ ] **Step 5: Build + run the headline scenario.**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build
ctest --test-dir /tmp/olr-sched/build -R e2e_play_storm -V
```
Expected: PASS — `play1x 2` reports **0 repositions** over the steady window (vs the old ~90 seeks/s) and audio pushes with no mid-stream `audioClear`.
- [ ] **Step 6: Commit**
```bash
cd /tmp/olr-sched && git add tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/e2e/CMakeLists.txt tests/e2e/record_harness.cpp tests/CMakeLists.txt
git commit -m "Add headless playback e2e harness + storm-regression scenario (spec v3 §11)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 8: Acceptance matrix + headline gate (spec §11.5–11.6)

**Files:** Modify `tests/e2e/run_playback_e2e.sh` (more scenarios), `tests/e2e/CMakeLists.txt` (register them)

- [ ] **Step 1:** Extend `run_playback_e2e.sh` to support the §11.5 scenarios with per-scenario fixtures/observation windows/metrics: `sliderscrub` (playing+paused, in-window 0 repositions, backward re-primes audio), `stepscrub` (≤4 repositions for 20 back-steps), `reverse` (descending PTS, bounded `reverseChunkSeek`), `speedflip` (exact frame after 5×→1×), and a couple of the speed×views cells (`1×{1,2,4}`, `5×4` degrade, `-5×2`). Add the `growfile` and `skewed` scenarios if feasible within CI time; otherwise document them as manual and assert the static-fixture subset.
- [ ] **Step 2:** Register the scenarios as CTest cases (`e2e_play_*`) with `LABELS e2e; RUN_SERIAL TRUE`.
- [ ] **Step 3: Run the playback e2e label.**
```bash
ctest --test-dir /tmp/olr-sched/build -L e2e -V
```
Expected: all `e2e_play_*` PASS, in particular **`e2e_play_storm` (2-view 1×) reports 0 repositions + audio in-cap** — the §11.6 headline gate.
- [ ] **Step 4: Full build + full test suite (regression).**
```bash
/Users/timo.korkalainen/Qt/Tools/Ninja/ninja -C /tmp/olr-sched/build && ctest --test-dir /tmp/olr-sched/build --output-on-failure
```
Expected: all unit + e2e tests pass.
- [ ] **Step 5: Commit**
```bash
cd /tmp/olr-sched && git add tests/e2e/run_playback_e2e.sh tests/e2e/CMakeLists.txt
git commit -m "Add playback acceptance matrix; 2-view 1x storm gate green (spec v3 §11.6)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 9: Real-app smoke + PR

**Files:** none (verification + PR)

- [ ] **Step 1: Launch the built app** and confirm playback is usable (record a few seconds from a synthetic source, then scrub/step/reverse/play). Look for: smooth 1× single-view playback with audio, responsive scrub (no frozen picture, incl. paused), reverse that moves (not a slideshow), no CPU pegging.
- [ ] **Step 2: Push + open PR** against `main`:
```bash
cd /tmp/olr-sched && git push -u origin fix/playback-scheduler
gh pr create --base main --head fix/playback-scheduler --title "Playback: windowed demand-driven scheduler (kills the seek storm)" --body "<summary: the runtime-confirmed storm, the window model, the spec v3 + two adversarial review rounds, the §11.6 gate result, and the e2e harness>"
```

---

## Self-review notes (filled by the plan author)

- **Spec coverage:** §3 constants→T4; §4 window/`dir`→T4/T5; §5 direction-aware delivery→T5; §6.1–6.9→T5; §7 call sites→T6; §8 degenerate states→T5 (classify guards) + asserted in T8 (`growfile`/short-file); §9 audio contract→T5/T6; §10 AudioPlayer carve-out→T3; §11.1 deliverables→T1/T2/T3/T7; §11.2–11.6 acceptance→T7/T8. `TrackBuffer`/`AudioFrameQueue`/counters all unit- or e2e-covered.
- **Type consistency:** `TrackBuffer`/`AudioFrameQueue`/`PlaybackCounters` signatures defined in T1/T2/T3 are used verbatim in T4/T5. `frameAt`/`hasFrameNear`/`insert(...,capFrames,keepNear,protectTo)`/`releaseDue(playhead,lead,out)` names are consistent across tasks.
- **Known residual (empirical, test-gated not prose-pinned per spec):** exact decimation smoothness and audio continuity across reuse-seeks are validated by T8 scenarios (`speedflip`, `sliderscrub` backward) rather than pinned in code comments — adjust constants (`kLeadMs`, `kAudioLeadMs`, `kChunkMs`) during T8 if a scenario misses its metric.
