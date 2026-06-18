# Output Continuity Soak Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, wall-clock soak that proves the broadcast output bus holds frame/audio continuity and cadence over minutes, plus close three deferred unit test-gaps.

**Architecture:** A standalone `soak_harness` binary drives a real `OutputRuntime` against a static synthetic `OutputFrameCache` through alternating play/pause segments, with a `ContinuitySink` computing O(1) running invariants per submitted frame. A bash driver greps the harness report and asserts. The test registers under an opt-in CTest label `output-soak`, excluded from the pre-push gate. No production source changes.

**Tech Stack:** C++17, Qt 6 Core (`QCoreApplication`, `QThread`, `QElapsedTimer`), Qt Test (for the three unit gaps), CMake/CTest, bash.

## Global Constraints

- No production source changes — the harness uses only public `OutputRuntime` / `OutputFrameCache` / `IOutputSink` APIs (copy values verbatim from existing headers).
- Build with the existing test toolchain: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build with `~/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`.
- Format only changed lines before committing: `/opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main --extensions cpp,h,hpp,mm,c`.
- Output frame rate default for the soak: `30000/1001` (29.97). Audio: 48000 Hz, S16 interleaved, stereo.
- The soak test label is exactly `output-soak`; it must NOT carry the `ci` or `e2e` label.

---

### Task 1: Soak harness binary (`ContinuitySink` + static cache + phases + report)

**Files:**
- Create: `tests/e2e/soak_harness.cpp`
- Modify: `tests/e2e/CMakeLists.txt` (new `qt_add_executable` target only; the `add_test` comes in Task 2)

**Interfaces:**
- Consumes (from existing headers): `OutputRuntime(FrameRate, int feedCount, int width, int height)`, `OutputRuntime::setSnapshotProvider(std::function<OutputRuntimeSnapshot()>)`, `OutputRuntime::setEndpoints(const QList<OutputEndpoint>&)`, `OutputRuntime::startRuntime()`, `OutputRuntime::stopRuntime()`, `OutputRuntime::stats() -> OutputDispatchStats`; `OutputRuntimeSnapshot{ OutputFrameCache cache; PlaybackStateSnapshot state; }`; `OutputEndpoint{ OutputTargetAssignment assignment; IOutputSink* sink; }`; `OutputFrameCache(int,int,int)`, `insertVideoFrame`, `insertAudioFrame`; `MediaVideoFrame::solidYuv420p(int,int,uchar,uchar,uchar)`; `MediaAudioFrame{feedIndex,startSample,sampleRate,channels,format,pcm}` with `int sampleFrames()`; `OutputBusFrame{outputFrameIndex, sampledPlayheadMs, video, audio, identity}`; `OutputFrameIdentity{videoPlaceholder, audioSilent, samePayloadAs()}`; `OutputDispatchStats.runtime{deadlineMisses, catchUpCapHits, maxLatenessNs}` and `OutputDispatchStats.ticks`; `OutputTargetKind::QtPreview`, `OutputBusId::feed(int)`, `OutputBusId::multiview()`, `FrameRate::fromFraction(int,int)`, `MediaSampleFormat::S16Interleaved`.
- Produces: the `soak_harness` executable that prints three report lines (`SOAK bus=feed ...`, `SOAK bus=multiview ...`, `RUNTIME ...`) and exits 0.

- [ ] **Step 1: Write `tests/e2e/soak_harness.cpp`**

```cpp
// Headless output-bus soak: drives a real OutputRuntime against a static synthetic
// OutputFrameCache through alternating play/pause segments and verifies, via a
// ContinuitySink, that the produced output stream stays frame- and audio-continuous and
// holds cadence over wall-clock time. Rung 1 of the output-stability ladder: it exercises
// the OutputRuntime/dispatcher/engine/memo/identity/rational-audio code, NOT real decode,
// real sinks, or an external consumer (that is the rung-5 NDI-receiver lane).
//
// Env: OLR_SOAK_SECONDS (default 120), OLR_SOAK_FPS_NUM/OLR_SOAK_FPS_DEN (default 30000/1001),
//      OLR_SOAK_FEEDS (default 2).
// Output (stdout), one line each:
//   SOAK bus=feed frames=.. indexGaps=.. audioSeams=.. placeholders=.. repeated=..
//   SOAK bus=multiview frames=.. indexGaps=.. audioSeams=.. placeholders=.. repeated=..
//   RUNTIME deadlineMisses=.. catchUpCapHits=.. maxLatenessNs=.. ticks=..
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QThread>

#include <atomic>
#include <cstdio>

#include "playback/output/outputruntime.h"

namespace {

int envIntOr(const char* key, int def) {
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(key, &ok);
    return ok ? v : def;
}

// Records O(1) running continuity invariants per submitted frame. submit() runs on the
// OutputRuntime thread; the accessors are read only AFTER stopRuntime() has joined it.
class ContinuitySink final : public IOutputSink {
public:
    explicit ContinuitySink(OutputTargetKind kind) : m_kind(kind) {}

    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment&, FrameRate) override {
        m_active = true;
        return true;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }

    bool submit(const OutputBusFrame& frame) override {
        if (m_hasLastIndex && frame.outputFrameIndex != m_lastIndex + 1) m_indexGaps++;
        m_lastIndex = frame.outputFrameIndex;
        m_hasLastIndex = true;

        // Audio tiling is only meaningful between consecutive playing (non-silent) frames.
        // A pause emits silent audio and resets the baseline; resume re-establishes it.
        if (frame.identity.audioSilent) {
            m_haveAudioBaseline = false;
        } else {
            const qint64 start = frame.audio.startSample;
            if (m_haveAudioBaseline && start != m_expectedNextStart) m_audioSeams++;
            m_expectedNextStart = start + frame.audio.sampleFrames();
            m_haveAudioBaseline = true;
        }

        if (frame.identity.videoPlaceholder) m_placeholders++;
        if (m_hasLastIdentity && m_lastIdentity.samePayloadAs(frame.identity)) m_repeated++;
        m_lastIdentity = frame.identity;
        m_hasLastIdentity = true;
        m_frames++;
        return true;
    }

    qint64 frames() const { return m_frames; }
    qint64 indexGaps() const { return m_indexGaps; }
    qint64 audioSeams() const { return m_audioSeams; }
    qint64 placeholders() const { return m_placeholders; }
    qint64 repeated() const { return m_repeated; }

private:
    OutputTargetKind m_kind;
    bool m_active = false;
    bool m_hasLastIndex = false;
    qint64 m_lastIndex = 0;
    qint64 m_indexGaps = 0;
    bool m_haveAudioBaseline = false;
    qint64 m_expectedNextStart = 0;
    qint64 m_audioSeams = 0;
    qint64 m_placeholders = 0;
    bool m_hasLastIdentity = false;
    OutputFrameIdentity m_lastIdentity;
    qint64 m_repeated = 0;
    qint64 m_frames = 0;
};

OutputFrameCache buildStaticCache(int feedCount, int width, int height, FrameRate rate,
                                  int seconds) {
    OutputFrameCache cache(feedCount, width, height);
    const qint64 frameCount = qint64(seconds + 1) * rate.numerator / rate.denominator + 4;
    for (int feed = 0; feed < feedCount; ++feed) {
        for (qint64 i = 0; i < frameCount; ++i) {
            MediaVideoFrame v =
                MediaVideoFrame::solidYuv420p(width, height, uchar(16 + ((i + feed) & 0x3F)), 128, 128);
            v.feedIndex = feed;
            v.ptsMs = rate.frameIndexToMs(i);
            cache.insertVideoFrame(v);
        }
    }
    // Contiguous, NON-zero S16 stereo audio so playing frames are non-silent (tile-checked).
    const int sampleRate = 48000;
    const int chunk = sampleRate / 10; // 100ms frames
    const qint64 totalSamples = qint64(seconds + 1) * sampleRate;
    for (int feed = 0; feed < feedCount; ++feed) {
        for (qint64 s = 0; s < totalSamples; s += chunk) {
            MediaAudioFrame a;
            a.feedIndex = feed;
            a.startSample = s;
            a.sampleRate = sampleRate;
            a.channels = 2;
            a.format = MediaSampleFormat::S16Interleaved;
            QByteArray pcm(chunk * 2 * int(sizeof(qint16)), 0);
            auto* p = reinterpret_cast<qint16*>(pcm.data());
            for (int k = 0; k < chunk * 2; ++k) p[k] = qint16(1000 + ((s + k) & 0x3FFF));
            a.pcm = pcm;
            cache.insertAudioFrame(a);
        }
    }
    return cache;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const int seconds = envIntOr("OLR_SOAK_SECONDS", 120);
    const int fpsNum = envIntOr("OLR_SOAK_FPS_NUM", 30000);
    const int fpsDen = envIntOr("OLR_SOAK_FPS_DEN", 1001);
    const int feeds = qMax(1, envIntOr("OLR_SOAK_FEEDS", 2));
    const int width = 64;
    const int height = 64;
    const FrameRate rate = FrameRate::fromFraction(fpsNum, fpsDen);

    OutputFrameCache cache = buildStaticCache(feeds, width, height, rate, seconds);

    std::atomic<int> playing{1};
    const qint64 pauseHoldMs = 1000;

    OutputRuntime runtime(rate, feeds, width, height);
    runtime.setSnapshotProvider([&cache, &playing, pauseHoldMs]() {
        OutputRuntimeSnapshot snap;
        snap.cache = cache; // shallow copy-on-write share, mirrors makeOutputSnapshot
        snap.state.playing = playing.load(std::memory_order_relaxed) != 0;
        snap.state.speed = 1.0;
        snap.state.selectedFeedIndex = 0;
        snap.state.playheadMs = snap.state.playing ? 0 : pauseHoldMs;
        return snap;
    });

    ContinuitySink feedSink(OutputTargetKind::QtPreview);
    ContinuitySink multiviewSink(OutputTargetKind::QtPreview);

    OutputTargetAssignment feedAssignment;
    feedAssignment.id = QStringLiteral("soak-feed");
    feedAssignment.sourceBus = OutputBusId::feed(0);
    feedAssignment.kind = OutputTargetKind::QtPreview;
    feedAssignment.enabled = true;

    OutputTargetAssignment multiviewAssignment;
    multiviewAssignment.id = QStringLiteral("soak-multiview");
    multiviewAssignment.sourceBus = OutputBusId::multiview();
    multiviewAssignment.kind = OutputTargetKind::QtPreview;
    multiviewAssignment.enabled = true;

    runtime.setEndpoints({{feedAssignment, &feedSink}, {multiviewAssignment, &multiviewSink}});

    runtime.startRuntime();

    // Alternate play (even segments) and pause (odd segments) across the duration.
    const int segments = 5;
    const int segmentMs = qMax(200, (seconds * 1000) / segments);
    for (int seg = 0; seg < segments; ++seg) {
        playing.store((seg % 2 == 0) ? 1 : 0, std::memory_order_relaxed);
        QThread::msleep(segmentMs);
    }

    runtime.stopRuntime(); // joins the runtime thread; sink accessors are safe afterward

    const OutputDispatchStats stats = runtime.stats();
    auto report = [](const char* bus, const ContinuitySink& s) {
        printf("SOAK bus=%s frames=%lld indexGaps=%lld audioSeams=%lld placeholders=%lld "
               "repeated=%lld\n",
               bus, (long long) s.frames(), (long long) s.indexGaps(), (long long) s.audioSeams(),
               (long long) s.placeholders(), (long long) s.repeated());
    };
    report("feed", feedSink);
    report("multiview", multiviewSink);
    printf("RUNTIME deadlineMisses=%lld catchUpCapHits=%lld maxLatenessNs=%lld ticks=%lld\n",
           (long long) stats.runtime.deadlineMisses, (long long) stats.runtime.catchUpCapHits,
           (long long) stats.runtime.maxLatenessNs, (long long) stats.ticks);
    fflush(stdout);
    return 0;
}
```

- [ ] **Step 2: Add the `soak_harness` target to `tests/e2e/CMakeLists.txt`**

Insert after the `play_harness` target block (it links the same library set):

```cmake
# Headless output-bus continuity soak: drives a real OutputRuntime against a static
# synthetic cache and verifies frame/audio continuity + cadence over wall-clock time.
qt_add_executable(soak_harness soak_harness.cpp)
target_link_libraries(soak_harness PRIVATE
    Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback olr_warnings olr_sanitize)
```

- [ ] **Step 3: Build the harness**

Run: `~/Qt/Tools/Ninja/ninja -C build/claude-debug soak_harness`
Expected: links `tests/e2e/soak_harness`.

(If `build/claude-debug` is not yet configured, run the configure command from Global Constraints first.)

- [ ] **Step 4: Run a fast self-check and verify the report**

Run: `OLR_SOAK_SECONDS=3 ./build/claude-debug/tests/e2e/soak_harness`
Expected: three lines; both `SOAK` lines have `indexGaps=0 audioSeams=0 placeholders=0`, `repeated>0`, `frames>0`; the `RUNTIME` line has `deadlineMisses=0`. Example:
```
SOAK bus=feed frames=89 indexGaps=0 audioSeams=0 placeholders=0 repeated=27
SOAK bus=multiview frames=89 indexGaps=0 audioSeams=0 placeholders=0 repeated=27
RUNTIME deadlineMisses=0 catchUpCapHits=0 maxLatenessNs=... ticks=89
```
If `audioSeams>0` or `indexGaps>0`, stop — that is a real output-bus regression, not a harness bug.

- [ ] **Step 5: Format and commit**

```bash
/opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main --extensions cpp,h,hpp,mm,c
git add tests/e2e/soak_harness.cpp tests/e2e/CMakeLists.txt
git commit -m "test: add output-bus continuity soak harness"
```

---

### Task 2: Driver script + opt-in CTest label

**Files:**
- Create: `tests/e2e/run_output_soak.sh`
- Modify: `tests/e2e/CMakeLists.txt` (add `add_test` + `set_tests_properties` with label `output-soak`)
- Modify: `.githooks/pre-push` (exclude the `output-soak` label from the full local gate)

**Interfaces:**
- Consumes: the `soak_harness` executable from Task 1 (its three report lines).
- Produces: CTest test `e2e_output_soak` under label `output-soak`.

- [ ] **Step 1: Write `tests/e2e/run_output_soak.sh`**

```bash
#!/usr/bin/env bash
# Output-bus continuity soak driver: runs soak_harness and asserts the produced output
# stream stayed frame/audio continuous and held cadence. Opt-in (CTest label "output-soak").
#
# Usage: run_output_soak.sh <soak_harness_exe>
# Env: OLR_SOAK_SECONDS (default 5 here for a fast opt-in run; raise for a real soak).
set -uo pipefail

HARNESS="${1:?soak_harness executable path required}"
export OLR_SOAK_SECONDS="${OLR_SOAK_SECONDS:-5}"

OUT="$("$HARNESS")"
status=$?
echo "$OUT"
if [ $status -ne 0 ]; then
    echo "FAIL: soak_harness exited $status"
    exit 1
fi

fail=0
field() { sed -n "s/.*$2=\\([0-9-]*\\).*/\\1/p" <<<"$1"; }

check_bus() {
    local bus="$1" line
    line="$(grep "SOAK bus=$bus " <<<"$OUT" || true)"
    if [ -z "$line" ]; then echo "FAIL: missing report for bus=$bus"; fail=1; return; fi
    local frames gaps seams placeholders repeated
    frames=$(field "$line" frames)
    gaps=$(field "$line" indexGaps)
    seams=$(field "$line" audioSeams)
    placeholders=$(field "$line" placeholders)
    repeated=$(field "$line" repeated)
    [ "${gaps:-1}" = "0" ]         || { echo "FAIL[$bus]: indexGaps=$gaps"; fail=1; }
    [ "${seams:-1}" = "0" ]        || { echo "FAIL[$bus]: audioSeams=$seams"; fail=1; }
    [ "${placeholders:-1}" = "0" ] || { echo "FAIL[$bus]: placeholders=$placeholders"; fail=1; }
    [ "${repeated:-0}" -gt 0 ]     || { echo "FAIL[$bus]: repeated=$repeated (no pause detected)"; fail=1; }
    [ "${frames:-0}" -gt 0 ]       || { echo "FAIL[$bus]: frames=$frames"; fail=1; }
}

check_bus feed
check_bus multiview

runtime_line="$(grep "^RUNTIME " <<<"$OUT" || true)"
misses=$(field "$runtime_line" deadlineMisses)
[ "${misses:-1}" = "0" ] || { echo "FAIL: deadlineMisses=$misses"; fail=1; }

if [ "$fail" = "0" ]; then echo "PASS: output soak continuity OK"; exit 0; fi
echo "SOAK FAILED"
exit 1
```

- [ ] **Step 2: Make the script executable and register the test in `tests/e2e/CMakeLists.txt`**

After the `soak_harness` target block from Task 1, add:

```cmake
add_test(NAME e2e_output_soak
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_output_soak.sh" "$<TARGET_FILE:soak_harness>")
set_tests_properties(e2e_output_soak PROPERTIES
    LABELS "output-soak"
    TIMEOUT 600
    RUN_SERIAL TRUE
    ENVIRONMENT "OLR_SOAK_SECONDS=5")
```

Then: `chmod +x tests/e2e/run_output_soak.sh`

- [ ] **Step 3: Exclude `output-soak` from the pre-push full gate**

In `.githooks/pre-push`, change the CTest exclusion (around line 36) from:

```bash
        -LE 'sync-report|srt|native-apple-ingest'
```

to:

```bash
        -LE 'sync-report|srt|native-apple-ingest|output-soak'
```

(GitHub CI already runs only `-L ci`, so the `output-soak`-labelled test never runs there; this keeps it out of the local pre-push gate too. It runs only via explicit `ctest -L output-soak`.)

- [ ] **Step 4: Reconfigure CMake so CTest sees the new test**

Run: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`
Expected: configure succeeds.

- [ ] **Step 5: Verify the opt-in test passes and is excluded by default**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug soak_harness
ctest --test-dir build/claude-debug -L output-soak --output-on-failure
```
Expected: `e2e_output_soak` runs and passes (`PASS: output soak continuity OK`, `100% tests passed`).

```bash
ctest --test-dir build/claude-debug -N -LE 'sync-report|srt|native-apple-ingest|output-soak' | grep -c e2e_output_soak
```
Expected: `0` (the soak is excluded from the default/pre-push selection).

- [ ] **Step 6: Negative sanity check (then revert)**

Temporarily break an assertion to prove the gate fails: in `run_output_soak.sh` change `[ "${seams:-1}" = "0" ]` to `[ "${seams:-1}" = "999" ]`, run `ctest --test-dir build/claude-debug -L output-soak --output-on-failure`, confirm it FAILS, then revert the line.

- [ ] **Step 7: Commit**

```bash
git add tests/e2e/run_output_soak.sh tests/e2e/CMakeLists.txt .githooks/pre-push
git commit -m "test: register opt-in output soak gate (label: output-soak)"
```

---

### Task 3: Test-gap [10] — dispatcher reverse + speed-change re-anchor

**Files:**
- Modify: `tests/unit/tst_outputdispatcher.cpp`

**Interfaces:**
- Consumes: existing `CollectingSink` (records submitted frames in `QVector<OutputBusFrame> frames`), `OutputDispatcher(FrameRate, feedCount, w, h)`, `dispatchTick(cache, state)`, `video(...)` helper in the test file.

- [ ] **Step 1: Add the test declaration**

In the `private slots:` block of `TestOutputDispatcher`, add:

```cpp
    void reverseAndSpeedChangeReanchorPlayhead();
```

- [ ] **Step 2: Write the test body** (append before `QTEST_GUILESS_MAIN`)

```cpp
void TestOutputDispatcher::reverseAndSpeedChangeReanchorPlayhead() {
    OutputFrameCache cache(1, 4, 4);
    for (int i = 0; i < 30; ++i) cache.insertVideoFrame(video(0, i * 40, uchar(10 + i)));

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputTargetAssignment a;
    a.id = QStringLiteral("feed0");
    a.sourceBus = OutputBusId::feed(0);
    a.kind = OutputTargetKind::QtPreview;
    a.enabled = true;

    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{a, &sink}});

    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.playheadMs = 200;
    state.selectedFeedIndex = 0;

    dispatcher.dispatchTick(cache, state); // tick 0: epoch anchors at frame 0, playhead 200
    dispatcher.dispatchTick(cache, state); // tick 1: 200 + 40

    state.speed = -1.0; // speed change forces re-anchor on the next tick
    state.playheadMs = 240;
    dispatcher.dispatchTick(cache, state); // tick 2: re-anchor, playhead 240
    dispatcher.dispatchTick(cache, state); // tick 3: 240 - 40 (reverse)

    QCOMPARE(sink.frames.size(), 4);
    QCOMPARE(sink.frames[0].sampledPlayheadMs, qint64(200));
    QCOMPARE(sink.frames[1].sampledPlayheadMs, qint64(240));
    QCOMPARE(sink.frames[2].sampledPlayheadMs, qint64(240)); // re-anchored, not 280
    QCOMPARE(sink.frames[3].sampledPlayheadMs, qint64(200)); // reverse step
}
```

- [ ] **Step 3: Build and run; verify it passes**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_outputdispatcher
./build/claude-debug/tests/unit/tst_outputdispatcher
```
Expected: all pass, including `reverseAndSpeedChangeReanchorPlayhead`. (This pins existing correct behavior; if it FAILS, a real re-anchor bug has been found — investigate before proceeding.)

- [ ] **Step 4: Commit**

```bash
git add tests/unit/tst_outputdispatcher.cpp
git commit -m "test: pin dispatcher reverse/speed-change playhead re-anchor"
```

---

### Task 4: Test-gap [11] — queued sink restart reset + drain under rapid stop

**Files:**
- Modify: `tests/unit/tst_queuedoutputsink.cpp`

**Interfaces:**
- Consumes: existing `GapReportingInnerSink`, `frame(qint64)` helper, `QueuedOutputSink`, `OutputSinkStatus`.

- [ ] **Step 1: Add the test declarations**

In `private slots:` of `TestQueuedOutputSink`, add:

```cpp
    void restartResetsDeliveryState();
    void rapidStopAfterBurstDrainsWithoutHang();
```

- [ ] **Step 2: Write the test bodies** (append before `QTEST_GUILESS_MAIN`)

```cpp
void TestQueuedOutputSink::restartResetsDeliveryState() {
    auto inner = std::make_unique<GapReportingInnerSink>();
    QueuedOutputSink sink(std::move(inner), 3);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(5)));
    QTRY_COMPARE_WITH_TIMEOUT(sink.outputStatus().lastDeliveredFrameIndex, qint64(5), 500);
    sink.stop();

    // Restart: all delivery state must reset to its initial values.
    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    const OutputSinkStatus fresh = sink.outputStatus();
    QCOMPARE(fresh.droppedFrames, qint64(0));
    QCOMPARE(fresh.deliveryGaps, qint64(0));
    QVERIFY(!fresh.hasLastDeliveredFrameIndex);
    QVERIFY(!fresh.lastDeliveryGap);

    QVERIFY(sink.submit(frame(10)));
    QTRY_COMPARE_WITH_TIMEOUT(sink.outputStatus().lastDeliveredFrameIndex, qint64(10), 500);
    QCOMPARE(sink.outputStatus().deliveryGaps, qint64(0)); // first delivery after restart: no gap
    sink.stop();
}

void TestQueuedOutputSink::rapidStopAfterBurstDrainsWithoutHang() {
    auto inner = std::make_unique<GapReportingInnerSink>();
    QueuedOutputSink sink(std::move(inner), 2);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    for (int i = 0; i < 50; ++i) sink.submit(frame(i));
    sink.stop(); // must join the worker and return promptly, no deadlock
    QVERIFY(!sink.isActive());
}
```

- [ ] **Step 3: Build and run; verify it passes**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_queuedoutputsink
./build/claude-debug/tests/unit/tst_queuedoutputsink
```
Expected: all pass. Run it 5× to confirm the threaded tests are non-flaky:
```bash
for i in $(seq 1 5); do ./build/claude-debug/tests/unit/tst_queuedoutputsink | grep Totals; done
```

- [ ] **Step 4: Commit**

```bash
git add tests/unit/tst_queuedoutputsink.cpp
git commit -m "test: pin queued sink restart reset and rapid-stop drain"
```

---

### Task 5: Test-gap [12] — placeholder video maps to Degraded

**Files:**
- Modify: `tests/unit/tst_broadcastoutputsettings.cpp`

**Interfaces:**
- Consumes: `BroadcastOutputSettings::setEnabled`, `BroadcastOutputSettings::rows`, `BroadcastOutputTargetStatus`, `OutputBusId::feed`.

- [ ] **Step 1: Add the test declaration**

In `private slots:` of `TestBroadcastOutputSettings`, add:

```cpp
    void rowsMarkPlaceholderVideoAsDegraded();
```

- [ ] **Step 2: Write the test body** (append before `QTEST_GUILESS_MAIN`)

```cpp
void TestBroadcastOutputSettings::rowsMarkPlaceholderVideoAsDegraded() {
    QList<OutputTargetAssignment> outputs = BroadcastOutputSettings::setEnabled(
        {}, 1, OutputTargetKind::Ndi, OutputBusId::feed(0), true);

    BroadcastOutputTargetStatus status;
    status.attemptedFrames = 5;
    status.framesSubmitted = 5;
    status.hasLastSubmitResult = true;
    status.lastSubmitSucceeded = true;
    status.hasLastIdentity = true;
    status.lastIdentity.bus = OutputBusId::feed(0);
    status.lastIdentity.outputFrameIndex = 4;
    status.lastIdentity.videoPlaceholder = true; // placeholder video -> Degraded

    QHash<QString, BroadcastOutputTargetStatus> statuses;
    statuses.insert(QStringLiteral("feed0-ndi"), status);

    const QVariantMap feed =
        BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi, statuses)[0].toMap();
    QCOMPARE(feed.value(QStringLiteral("statusState")).toString(), QStringLiteral("Degraded"));
    QCOMPARE(feed.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("warning"));
}
```

- [ ] **Step 3: Build and run; verify it passes**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_broadcastoutputsettings
./build/claude-debug/tests/unit/tst_broadcastoutputsettings
```
Expected: all pass, including `rowsMarkPlaceholderVideoAsDegraded`.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/tst_broadcastoutputsettings.cpp
git commit -m "test: pin placeholder video maps to Degraded health"
```

---

### Task 6: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Build all touched targets**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug soak_harness \
  tst_outputdispatcher tst_queuedoutputsink tst_broadcastoutputsettings \
  tst_outputbusengine tst_outputruntime tst_ndisink tst_qtpreviewsink \
  tst_outputframeclock tst_outputframecache tst_outputtargetassignment tst_yuv420pcompositor
```
Expected: all link.

- [ ] **Step 2: Run the full output unit suite**

```bash
ctest --test-dir build/claude-debug -R 'tst_output|tst_ndisink|tst_queuedoutputsink|tst_broadcastoutputsettings|tst_yuv420pcompositor|tst_qtpreviewsink' --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 3: Run the opt-in soak**

```bash
ctest --test-dir build/claude-debug -L output-soak --output-on-failure
```
Expected: `e2e_output_soak` passes.

- [ ] **Step 4: Confirm clean formatting and diff**

```bash
/opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main --extensions cpp,h,hpp,mm,c
git diff --check origin/main
git status --short
```
Expected: no reformatting needed (or commit it), `git diff --check` prints nothing, only intended files changed.

---

## Self-Review Checklist

- **Spec coverage:** soak harness (Task 1), continuity invariants frame/audio/cadence/paused (Task 1 sink + Task 2 driver), opt-in label (Task 2), no production changes (Tasks 1–2 use public APIs only; the only non-test edit is the pre-push `-LE` exclusion, which is test infra), the three deferred test-gaps (Tasks 3–5). Covered.
- **Placeholder scan:** no TBD/TODO; all code and commands are concrete.
- **Type consistency:** `ContinuitySink` accessors (`frames()/indexGaps()/audioSeams()/placeholders()/repeated()`) match the driver's field names (`frames/indexGaps/audioSeams/placeholders/repeated`); report keys match the `field`/`grep` patterns; `OutputDispatchStats.runtime.deadlineMisses` matches the `RUNTIME deadlineMisses=` field; `CollectingSink.frames` and `sampledPlayheadMs` match Task 3 usage.
- **Honest scope:** non-goals (no real decode/sinks/consumer/backpressure) are recorded in the spec; rung-5 NDI-receiver lane is the separate next workstream.
