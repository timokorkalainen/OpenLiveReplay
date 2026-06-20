# Tier-1 Broadcast Readiness Sprint — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Land the six high-leverage Tier-1 fixes from `docs/broadcast-readiness-roadmap.md` that improve playback-perfectness and broadcast-readiness, grounded against `main` @ `f3a70e6`.

**Architecture:** Six small, mostly-independent production fixes across the playback worker, recorder, and CI config, each with a focused test where feasible.

**Tech Stack:** C++17, Qt 6, FFmpeg, CTest, GitHub Actions YAML.

## Global Constraints

- WORKTREE ONLY: all work in `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/tier1-broadcast-readiness`. Before any commit verify `git rev-parse --show-toplevel` ends with `tier1-broadcast-readiness`. NEVER touch the main checkout or any other worktree.
- This is PRODUCTION code. Each task: build clean, run the covering tests, commit. `git add` only the task's files (never `-A`/`.`).
- **Formatting:** format ONLY the lines you changed — `git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format` against the branch point (`git clang-format f3a70e6`). Do NOT run repo-wide `clang-format -i`. Several engine files are hand-written Allman; match the surrounding style. (Pre-push runs a changed-line clang-format gate.)
- Build: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build with `~/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`.
- Line numbers below are as of `f3a70e6`; later tasks shift them slightly (T1.2/T1.3/T1.6 all touch `playbackworker.cpp`). **Locate edits by the quoted code, not the line number.**
- Do NOT modify `docs/broadcast-readiness-roadmap.md` here — the roadmap is updated in its own branch after this sprint merges.

---

### Task 1: T1.6 — warn on unimplemented output sinks

**Files:** Modify `playback/playbackworker.cpp`.

**Interfaces:** Consumes `outputTargetKindName(OutputTargetKind)` (already available transitively via `playback/output/broadcastoutputsettings.h`).

- [ ] **Step 1: Add the warning to the sink switch**

Find the `switch (assignment.kind)` in the output-endpoint rebuild (around line 372) and change it to:

```cpp
        switch (assignment.kind) {
        case OutputTargetKind::Ndi:
            sink = std::make_unique<QueuedOutputSink>(std::make_unique<NdiOutputSink>());
            break;
        case OutputTargetKind::QtPreview:
            break; // handled by the preview loop above; not expected in external list
        case OutputTargetKind::DeckLinkSdiHdmi:
        case OutputTargetKind::DeckLinkIpSt2110:
        case OutputTargetKind::Omt:
        case OutputTargetKind::Aja:
            qWarning() << "OutputTarget kind" << outputTargetKindName(assignment.kind)
                       << "is not yet implemented; sink will be skipped";
            break;
        }
```

- [ ] **Step 2: Build + run output-target tests**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_outputtargetassignment tst_outputdispatcher
ctest --test-dir build/claude-debug -R 'tst_outputtargetassignment|tst_outputdispatcher' --output-on-failure
```
Expected: builds clean (no warnings); both tests pass.

- [ ] **Step 3: Format changed lines + commit**

```bash
git add playback/playbackworker.cpp
git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format f3a70e6
git add playback/playbackworker.cpp
git commit -m "fix: warn (not silently skip) when an unimplemented output sink kind is configured (T1.6)"
```

---

### Task 2: T1.2 — output graph receives the rational frame rate (not integer)

**Files:** Modify `playback/playbackworker.cpp`, `tests/unit/tst_outputdispatcher.cpp`.

**Interfaces:** Consumes `PlaybackTransport::frameRate()` → `FrameRate {numerator, denominator}` (mutex-locked, returns the stored rational, e.g. `{30000,1001}` for 29.97). `PlaybackWorker::fps()` (integer) stays for scheduling math.

- [ ] **Step 1: Replace the two FrameRate construction sites with the rational rate**

In `initializeOutputGraph` (around line 288), change:
```cpp
        m_outputRuntime = std::make_unique<OutputRuntime>(
            FrameRate::fromFraction(fps(), 1), m_outputFeedCount, m_outputWidth, m_outputHeight);
```
to:
```cpp
        m_outputRuntime = std::make_unique<OutputRuntime>(
            m_transport->frameRate(), m_outputFeedCount, m_outputWidth, m_outputHeight);
```

In `deliverDueFrames` (around line 1893), change:
```cpp
    const FrameRate rate = FrameRate::fromFraction(fps(), 1);
```
to:
```cpp
    const FrameRate rate = m_transport->frameRate();
```
Do NOT change `PlaybackWorker::fps()` — it is still used by `frameDurMs()`, `capFrames()`, and the CutSchedule helpers, which want an integer.

- [ ] **Step 2: Expose the received rate on the test sink + add the assertion**

In `tests/unit/tst_outputdispatcher.cpp`, add a public accessor to the existing `CollectingSink` class (it already stores `m_rate`):
```cpp
    FrameRate receivedRate() const { return m_rate; }
```
Add a new private slot `rationalRateIsCarriedToSinkOnStart` (declare it in the slots list) and implement it:
```cpp
void TestOutputDispatcher::rationalRateIsCarriedToSinkOnStart()
{
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 0, 128));

    PlaybackStateSnapshot state;
    state.playheadMs = 0;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(30000, 1001), 1, 4, 4);
    dispatcher.setEndpoints({{qt, &sink}});
    dispatcher.dispatchTick(cache, state);

    QVERIFY(sink.isActive());
    QCOMPARE(sink.receivedRate().numerator, 30000);
    QCOMPARE(sink.receivedRate().denominator, 1001);
}
```
Match the exact helper names already used in the file (`video(...)`, `CollectingSink` ctor, `OutputDispatcher` ctor signature, `dispatchTick`/`setEndpoints`). If `setEndpoints` already calls `start()`, the `dispatchTick` is harmless extra coverage; if the field names differ (`numerator`/`num`), use whatever `FrameRate` actually declares.

- [ ] **Step 2b: Run the failing test first (TDD sanity)**

Build + run only the new slot to confirm it compiles and passes after the production change; if you want a true red→green, stash the production edit, see it fail (rate `30/1`), then unstash.

- [ ] **Step 3: Build + run the covering tests**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_outputdispatcher tst_outputruntime tst_playbacktransport play_harness
ctest --test-dir build/claude-debug -R 'tst_outputdispatcher|tst_outputruntime|tst_playbacktransport' --output-on-failure
```
Expected: all pass, including the new `rationalRateIsCarriedToSinkOnStart`.

- [ ] **Step 4: Format changed lines + commit**

```bash
git add playback/playbackworker.cpp tests/unit/tst_outputdispatcher.cpp
git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format f3a70e6
git add playback/playbackworker.cpp tests/unit/tst_outputdispatcher.cpp
git commit -m "fix: playback output graph uses the rational frame rate (29.97/59.94 no longer collapse to 30/60) (T1.2)"
```

---

### Task 3: T1.5 — output-bus unit tests under the CI sanitizers

**Files:** Modify `.github/workflows/ci.yml`.

**Interfaces:** none (CI config). The targets `tst_outputdispatcher`, `tst_outputbusengine`, `tst_outputruntime` exist (`tests/unit/CMakeLists.txt`) and link `olr_sanitize`.

- [ ] **Step 1: Add the output-bus targets to both sanitizer matrix legs**

In `.github/workflows/ci.yml`, the ASan/UBSan leg currently reads:
```yaml
            build_targets: "tst_muxer tst_mpegtsparser tst_h26xaccessunit tst_nativeaacdecoder tst_audioframequeue"
            ctest_regex: "^(tst_muxer|tst_mpegtsparser|tst_h26xaccessunit|tst_nativeaacdecoder|tst_audioframequeue)$"
          - sanitizer: "thread"
            label: tsan
            advisory: true
            build_targets: "tst_trackbuffer tst_audioframequeue"
            ctest_regex: "^(tst_trackbuffer|tst_audioframequeue)$"
```
Change it to:
```yaml
            build_targets: "tst_muxer tst_mpegtsparser tst_h26xaccessunit tst_nativeaacdecoder tst_audioframequeue tst_outputdispatcher tst_outputbusengine tst_outputruntime"
            ctest_regex: "^(tst_muxer|tst_mpegtsparser|tst_h26xaccessunit|tst_nativeaacdecoder|tst_audioframequeue|tst_outputdispatcher|tst_outputbusengine|tst_outputruntime)$"
          - sanitizer: "thread"
            label: tsan
            advisory: true
            build_targets: "tst_trackbuffer tst_audioframequeue tst_outputdispatcher"
            ctest_regex: "^(tst_trackbuffer|tst_audioframequeue|tst_outputdispatcher)$"
```
Preserve the EXACT existing indentation (no tabs). The `^(...)$` anchors and `|` alternation must stay intact.

- [ ] **Step 2: Verify the test names resolve + build under ASan locally**

```bash
ctest --test-dir build/claude-debug -N -R '^(tst_outputdispatcher|tst_outputbusengine|tst_outputruntime)$'   # expect exactly 3 tests listed
# Optional but recommended: a throwaway ASan build to confirm they pass under the sanitizer
cmake -S . -B build/claude-asan -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_SANITIZER="address;undefined"
~/Qt/Tools/Ninja/ninja -C build/claude-asan tst_outputdispatcher tst_outputbusengine tst_outputruntime
ctest --test-dir build/claude-asan -R '^(tst_outputdispatcher|tst_outputbusengine|tst_outputruntime)$' --output-on-failure
```
Expected: the `-N` check lists 3 tests; the ASan build (if run) passes clean. If the ASan build surfaces a real defect in the output bus, that is a genuine finding — report DONE_WITH_CONCERNS with the sanitizer output rather than masking it.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: run the output-bus unit tests under ASan/UBSan (+TSan dispatcher) (T1.5)"
```

---

### Task 4: T1.3 — pre-roll bank must not software-decode H.264

**Files:** Modify `playback/playbackworker.cpp`. Possibly `tests/e2e/run_playback_e2e.sh` (armedcut fixture — see Step 1).

**Interfaces:** mirrors the primary decode bank's hardware-only H.264 guard into `openPrerollContext`.

- [ ] **Step 1: Investigate the armedcut e2e fixture codec FIRST (load-bearing)**

The fix makes `openPrerollContext` skip H.264 streams; on an H.264-only fixture the pre-roll bank becomes empty and `armNextCut` becomes a no-op (`cutsFired==0`). Check what codec the `armedcut` scenario records with:
```bash
grep -n "armedcut\|libx264\|mpeg2video\|-c:v" tests/e2e/run_playback_e2e.sh | head -40
```
- If `armedcut` already uses **mpeg2video** for its fixture → the guard does not affect it; proceed, no test change.
- If `armedcut` uses **libx264 (H.264)** → the existing test is currently passing *because of the bug being fixed* (it software-decodes H.264 in the pre-roll). After the fix it would report `cutsFired==0`. The correct resolution is to make the armedcut pre-roll fixture **mpeg2video** (pre-roll only works for SW-decodable codecs by design), so the gate validates the pre-roll path on a codec the guard does NOT skip. Make that fixture change in `run_playback_e2e.sh` and keep the existing `armedcut` gates (`cutsFired==2`, `placeholderFramesDelta==0`, `reposition==0`). Document the change in the report.

- [ ] **Step 2: Add the H.264 guard to `openPrerollContext`**

In `openPrerollContext`, the video-bank loop (around line 956) opens an FFmpeg software decoder for every video stream. Add the guard BEFORE `avcodec_find_decoder`, and do NOT advance `feedIndex` (mirroring the primary bank's `if (isH264) continue;`):
```cpp
        AVCodecParameters* codecParams = m_prerollFmtCtx->streams[i]->codecpar;
        if (codecParams->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (feedIndex >= m_providers.size()) break;

        // H.264: hardware-only licensing constraint — NEVER software-decode.
        // Mirror the primary bank guard exactly: skip the stream without advancing
        // feedIndex so pre-roll feedIndex N still maps to the same provider as
        // primary providerIndex N. (Pre-roll has no NativeVideoDecoder wiring, so
        // H.264 sources simply get no pre-roll staging — the live primary bank keeps
        // supplying those feeds after the cut swap.)
        if (codecParams->codec_id == AV_CODEC_ID_H264) continue;

        const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
        // ... unchanged ...
```
`fillStaging` needs no matching guard: after this change every `m_prerollBank` entry has a valid software `codecCtx`.

- [ ] **Step 3: Build + run the playback acceptance tests (incl. armedcut)**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug play_harness record_harness
ctest --test-dir build/claude-debug -R 'e2e_play_armedcut|e2e_play_seekflash|e2e_play_storm' --output-on-failure
```
Expected: `e2e_play_armedcut` passes (`cutsFired==2`) with the mpeg2video pre-roll fixture; the other playback gates unaffected. If `e2e_play_armedcut` was H.264 and you changed it to mpeg2video, confirm it now passes and note it.

- [ ] **Step 4: Format changed lines + commit**

```bash
git add playback/playbackworker.cpp tests/e2e/run_playback_e2e.sh   # include the .sh only if you changed it
git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format f3a70e6
git add playback/playbackworker.cpp
git commit -m "fix: pre-roll bank never software-decodes H.264 (mirror primary hardware-only guard) — kills armed-cut flash on H.264 recordings (T1.3)"
```

---

### Task 5: T1.1 — audio drift servo (apply recovered ppm) + register the drift A/V gate

**Files:** Modify `recorder_engine/streamworker.h`, `recorder_engine/streamworker.cpp`, `tests/e2e/CMakeLists.txt`.

**Interfaces:** Consumes `IngestStats::clockPpm` (already populated by all three ingest backends, delivered once/sec via the `reportStats` callback on the capture thread). Produces a per-source ppm snapshot read by the audio tick thread.

- [ ] **Step 1: Add the ppm snapshot atomic to `streamworker.h`**

After the `m_servoTrimOffsetMs` atomic (around line 182), add:
```cpp
    // Recovered source-clock ppm, written by the capture thread (reportStats callback)
    // and read by the tick thread in writeAudioForTick to scale srcAdvance. Relaxed
    // ordering is sufficient (a standalone, slowly-changing estimate). 0 = no correction
    // until the DriftEstimator locks.
    std::atomic<int> m_currentSourcePpm{0};
```

- [ ] **Step 2: Snapshot ppm in the `reportStats` callback**

In `captureLoop` (around line 416), change the `reportStats` lambda to snapshot ppm before emitting:
```cpp
        callbacks.reportStats = [this](const IngestStats& stats) {
            m_currentSourcePpm.store(int(std::round(stats.clockPpm)),
                                     std::memory_order_relaxed);
            emit statsUpdated(m_sourceIndex, stats);
        };
```
Ensure `#include <cmath>` is present in `streamworker.cpp` (add it with the other includes if missing).

- [ ] **Step 3: Scale `srcAdvance` by the recovered rate in `writeAudioForTick`**

Around line 755, change:
```cpp
    const int64_t srcStart = m_audioSourceCursor;
    const int64_t srcAdvance = n;
```
to:
```cpp
    const int64_t srcStart = m_audioSourceCursor;
    // Scale the source-cursor advance by the recovered clock rate so the cursor tracks
    // the FIFO data when the source clock runs off-nominal. ppm==0 (pre-lock / no skew)
    // is byte-identical to the old path. Clamp to >=1 to avoid a zero/negative advance
    // under an extreme negative ppm. Orthogonal to PTP (Phase 5, default off) and the
    // intercam servo (phase-only, no rate correction) — no double-correction.
    const int ppm = m_currentSourcePpm.load(std::memory_order_relaxed);
    int64_t srcAdvance = (ppm == 0)
        ? n
        : int64_t(std::llround(double(n) * (1.0 + double(ppm) * 1e-6)));
    if (srcAdvance < 1) srcAdvance = 1;
```

- [ ] **Step 4: Register the `e2e_framesync_drift_avsync` gate**

In `tests/e2e/CMakeLists.txt`, add the new `add_test` (it dispatches the already-coded `run_drift_avsync` scenario), include it in the framesync `set_tests_properties` group, and give it its own TIMEOUT + ENVIRONMENT. Insert the `add_test` after `e2e_framesync_drift_skew` and update the property block:
```cmake
add_test(NAME e2e_framesync_drift_avsync
    COMMAND bash "${_framesync_driver}" "$<TARGET_FILE:sync_harness>" drift_avsync 24050)
```
Add `e2e_framesync_drift_avsync` to the `set_tests_properties(... LABELS "framesync" TIMEOUT 180 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)` name list, then append:
```cmake
# drift_avsync runs ~60 s of recording to amplify the injected skew.
set_tests_properties(e2e_framesync_drift_avsync PROPERTIES TIMEOUT 240)
# Self-gating in run_framesync_e2e.sh (always asserts the A/V drift regression), so
# OLR_FRAMESYNC_GATE is intentionally omitted. 2000 ppm skew over 60 s makes the
# pre-fix regression (~120 ms) unmissable; the fix holds it within 1 frame.
set_tests_properties(e2e_framesync_drift_avsync PROPERTIES
    ENVIRONMENT "OLR_FRAMESYNC_SKEW_PPM=2000;OLR_FRAMESYNC_SECS=60")
```
Verify ports 24050/24051 are free in the framesync band:
```bash
grep -nE "2405[0-9]" tests/e2e/CMakeLists.txt   # expect only your new entry
```

- [ ] **Step 5: Build + run the drift gates (the real regression test)**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug sync_harness record_harness
# The fix's regression gate — must PASS with the fix (and would fail without it):
ctest --test-dir build/claude-debug -R 'e2e_framesync_drift_avsync' --output-on-failure
# Confirm the existing zero-skew drift gate still passes unchanged (ppm==0 fast path):
ctest --test-dir build/claude-debug -R 'e2e_framesync_drift$' --output-on-failure
```
Expected: `e2e_framesync_drift_avsync` PASSES (A/V offset regression within ~1 frame over the 60 s 2000 ppm run); `e2e_framesync_drift` still passes. Run the avsync gate 2–3× to confirm it is not flaky. If it SKIPs (77 — no ffmpeg/srt tooling), note that; if it FAILS, capture the measured regression and debug the ppm sign/scale before weakening anything.

- [ ] **Step 6: Format changed lines + commit**

```bash
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp tests/e2e/CMakeLists.txt
git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format f3a70e6
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp
git commit -m "fix: apply recovered clock ppm to the recording audio FIFO cursor (long-run A/V drift servo) + register drift_avsync gate (T1.1)"
```

---

### Task 6: T1.4 — surface fatal muxer write errors (disk full) to the operator

**Files:** Modify `recorder_engine/muxer.h`, `recorder_engine/muxer.cpp`, `recorder_engine/replaymanager.h`, `recorder_engine/replaymanager.cpp`, `uimanager.h`/`uimanager.cpp`, `tests/unit/tst_muxer.cpp`.

**Design (decided):** do NOT auto-stop (ENOSPC means writes already fail; the operator decides when to stop). Promote to "fatal" only after `kFatalWriteThreshold = 3` consecutive `av_write_frame` failures. Poll the flag from `ReplayManager::onTimerTick` (GUI thread) and emit a new `recordingError(QString)`; the UIManager re-emits the existing `recordingFailed` with a "recording continues; check disk space" prefix (no new QML surface).

- [ ] **Step 1: Add the fatal-error flag + accessors to `muxer.h`**

In the private member section (after `m_writerRunning`), add:
```cpp
    // Set on the FIRST sustained write failure (kFatalWriteThreshold consecutive
    // av_write_frame errors on any stream). Written once; reset only on init().
    std::atomic<bool> m_fatalWriteError{false};
    std::string m_fatalWriteMsg;          // guarded by m_fatalMsgMutex
    mutable std::mutex m_fatalMsgMutex;
    static constexpr int kFatalWriteThreshold = 3;
```
Add public accessors (e.g. after `close()`):
```cpp
    bool hasFatalWriteError() const { return m_fatalWriteError.load(std::memory_order_acquire); }
    QString fatalWriteMessage() const
    {
        std::lock_guard<std::mutex> lk(m_fatalMsgMutex);
        return QString::fromStdString(m_fatalWriteMsg);
    }
```
Add a test friend under the unit-test define (verify the macro name used by `olr_add_unit_test` — check `tests/unit/CMakeLists.txt` / `tests/CMakeLists.txt`; it may be `OLR_UNIT_TEST`):
```cpp
#ifdef OLR_UNIT_TEST
    friend class TestMuxer;
#endif
```

- [ ] **Step 2: Count consecutive failures + set the flag in `writerLoop`**

At the top of `writerLoop` (before the drain loop), add `int consecutiveWriteErrors = 0;`. Replace the existing write-error branch:
```cpp
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            qDebug() << "Muxer: write error for stream" << idx << ":" << errbuf;
        }
```
with:
```cpp
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            qDebug() << "Muxer: write error for stream" << idx << ":" << errbuf;
            ++consecutiveWriteErrors;
            if (consecutiveWriteErrors >= kFatalWriteThreshold
                && !m_fatalWriteError.load(std::memory_order_relaxed))
            {
                {
                    std::lock_guard<std::mutex> lk(m_fatalMsgMutex);
                    m_fatalWriteMsg = std::string(errbuf);
                }
                m_fatalWriteError.store(true, std::memory_order_release);
            }
        }
        else
        {
            consecutiveWriteErrors = 0;
        }
```
In `Muxer::init` (after the existing per-session resets like `m_lastDts.clear()`), reset the flag:
```cpp
    {
        std::lock_guard<std::mutex> lk(m_fatalMsgMutex);
        m_fatalWriteMsg.clear();
    }
    m_fatalWriteError.store(false, std::memory_order_relaxed);
```

- [ ] **Step 3: Add `recordingError` signal + poll in `ReplayManager`**

In `replaymanager.h` signals block, add:
```cpp
    // Sustained fatal muxer write error (e.g. ENOSPC). Recording is NOT auto-stopped.
    void recordingError(const QString& message);
```
Add a private member: `bool m_muxerErrorEmitted = false;` and reset it in `startRecording()` near the other session resets.
In `replaymanager.cpp onTimerTick()`, at the very top of the body (before the clock read), add:
```cpp
    if (m_muxer && m_muxer->hasFatalWriteError() && !m_muxerErrorEmitted) {
        m_muxerErrorEmitted = true;
        emit recordingError(m_muxer->fatalWriteMessage());
    }
```

- [ ] **Step 4: Wire the UI alert (reuse `recordingFailed`)**

In `UIManager`'s constructor (near the other `m_replayManager` connects), add:
```cpp
    connect(m_replayManager, &ReplayManager::recordingError, this,
            [this](const QString& msg) {
                emit recordingFailed(QStringLiteral("Recording error — ") + msg
                                     + QStringLiteral(" (recording continues; check disk space)"));
            },
            Qt::QueuedConnection);
```
No new UIManager signal or QML change — `recordingFailed` already drives the status alert.

- [ ] **Step 5: Test the accessor contract (unit) in `tests/unit/tst_muxer.cpp`**

Add a slot `fatalWriteErrorFlagAndMessage` (the test class is `TestMuxer`, made a friend in Step 1):
```cpp
void TestMuxer::fatalWriteErrorFlagAndMessage()
{
    Muxer m;
    QVERIFY(!m.hasFatalWriteError());
    QCOMPARE(m.fatalWriteMessage(), QString());

    {
        std::lock_guard<std::mutex> lk(m.m_fatalMsgMutex);
        m.m_fatalWriteMsg = "No space left on device";
    }
    m.m_fatalWriteError.store(true, std::memory_order_release);

    QVERIFY(m.hasFatalWriteError());
    QCOMPARE(m.fatalWriteMessage(), QStringLiteral("No space left on device"));
}
```
Confirm the unit-test define name in Step 1 matches what the build sets for `tst_muxer` (grep the CMake). If the friend/define seam proves more invasive than warranted, it is acceptable to ship the accessor test only via a real write-failure path you can trigger, or to report DONE_WITH_CONCERNS describing what you could and couldn't test — but do NOT leave the propagation completely untested.

- [ ] **Step 6: Build + run muxer + recorder tests**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_muxer record_harness
ctest --test-dir build/claude-debug -R 'tst_muxer' --output-on-failure
# Sanity: a normal record still works end to end (no false fatal-error):
ctest --test-dir build/claude-debug -R 'e2e_record_stereo' --output-on-failure
```
Expected: `tst_muxer` passes incl. the new slot; `e2e_record_stereo` still records normally (no spurious recordingError).

- [ ] **Step 7: Format changed lines + commit**

```bash
git add recorder_engine/muxer.h recorder_engine/muxer.cpp recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp uimanager.h uimanager.cpp tests/unit/tst_muxer.cpp
git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format f3a70e6
git add recorder_engine/muxer.h recorder_engine/muxer.cpp recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp uimanager.h uimanager.cpp tests/unit/tst_muxer.cpp
git commit -m "feat: surface sustained muxer write errors (disk full) to the operator instead of silently dropping packets (T1.4)"
```

---

### Task 7: Full verification

**Files:** none.

- [ ] **Step 1: Build everything + run the affected unit + e2e gates**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug
ctest --test-dir build/claude-debug -L unit --output-on-failure
ctest --test-dir build/claude-debug -R 'e2e_play_armedcut|e2e_play_storm|e2e_record_stereo|e2e_framesync_drift$|e2e_framesync_drift_avsync' --output-on-failure
```
Expected: full build clean; unit tests green; the listed e2e gates pass (or skip cleanly if tooling absent).

- [ ] **Step 2: Clean-tree + changed-line format check + main untouched**

```bash
git diff --check
git status --short
git -C /Users/timo.korkalainen/Development/timo/OpenLiveReplay status --porcelain | grep -E "playback/|recorder_engine/|uimanager|\.github/" || echo "main: no tracked sprint changes"
```
Expected: `git diff --check` empty; worktree clean; main checkout has none of this branch's changes.

---

## Self-Review Checklist

- **Coverage:** T1.6 (Task 1), T1.2 (Task 2), T1.5 (Task 3), T1.3 (Task 4), T1.1 (Task 5), T1.4 (Task 6), verification (Task 7). All six roadmap Tier-1 items covered.
- **Design decisions locked:** T1.4 no-auto-stop + threshold 3 + onTimerTick poll + reuse `recordingFailed`; T1.1 atomic ppm snapshot (no cross-thread `ppm()`), clamp ≥1, ppm==0 fast path; T1.3 skip H.264 in pre-roll (mirror primary) + adapt the armedcut fixture to mpeg2video if needed.
- **Production-safety:** changed-line clang-format only; each task builds + runs covering tests; thread-safety noted for the two concurrent paths (T1.1 ppm, T1.4 flag).
- **Type/name consistency:** `m_transport->frameRate()` → `FrameRate`; `IngestStats::clockPpm`; `outputTargetKindName`; the new `recordingError` signal threaded ReplayManager→UIManager. Verify `FrameRate` field names (`numerator`/`denominator`) and the unit-test define name against the actual code before relying on them.
