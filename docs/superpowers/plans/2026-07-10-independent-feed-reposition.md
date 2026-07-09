# Independent Feed Reposition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent late interleaved feeds from causing a global reposition retry loop while preserving correct selected-feed seeks.

**Architecture:** Add a header-only, allocation-free policy that decides reposition fill readiness from a reference feed callback. Multiview uses feed 0, single-view uses the selected feed, and the explicitly requested legacy all-feed mode still waits for all feeds. `PlaybackWorker` uses the policy instead of the steady-playback aggregate, while `UIManager` stops enabling all-feed coverage for multiview.

**Tech Stack:** C++17, Qt 6 Core/Test, CMake, FFmpeg-backed playback worker.

## Global Constraints

- Feeds are independent; a late non-reference feed must not force a global rewind.
- Multiview uses feed 0 as its timeline reference.
- Single-view uses the selected feed as its timeline reference.
- The hot fill loop must not allocate.
- Existing generation, packet-budget, GPU freshness, and operator-seek behavior stays intact.

---

### Task 1: Cross-platform reposition fill policy

**Files:**
- Create: `playback/repositionfillpolicy.h`
- Create: `tests/unit/tst_repositionfillpolicy.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `repositionFillReady(int feedCount, int selectedFeed, bool requireAllFeeds, qint64 fillToMs, NewestPtsAt&& newestPtsAt) -> bool`
- The callback consumes a feed index and returns that feed's newest decoded PTS, or a negative value when empty.

- [ ] **Step 1: Write the failing policy test**

Add three Qt test cases: selected feed 2 remains unready while only feeds 0 and 1 reached 80 ms; an unselected multiview reference succeeds from feed 0 while later feeds are empty; explicit all-feed mode remains unready until all four feeds reach 80 ms.

```cpp
void TestRepositionFillPolicy::selectedFeedMustReachFillTarget() {
    QVector<qint64> newest{80, 80, -1, -1};
    auto at = [&](int feed) { return newest.value(feed, qint64(-1)); };
    QVERIFY(!repositionFillReady(newest.size(), 2, false, 80, at));
    newest[2] = 120;
    QVERIFY(repositionFillReady(newest.size(), 2, false, 80, at));
}

void TestRepositionFillPolicy::multiviewUsesPrimaryReferenceFeed() {
    const QVector<qint64> newest{80, -1, -1, -1};
    QVERIFY(repositionFillReady(newest.size(), -1, false, 80,
                                [&](int feed) { return newest.value(feed, qint64(-1)); }));
}

void TestRepositionFillPolicy::explicitAllFeedModeWaitsForEveryFeed() {
    QVector<qint64> newest{80, 80, -1, -1};
    auto at = [&](int feed) { return newest.value(feed, qint64(-1)); };
    QVERIFY(!repositionFillReady(newest.size(), -1, true, 80, at));
    newest[2] = 120;
    newest[3] = 120;
    QVERIFY(repositionFillReady(newest.size(), -1, true, 80, at));
}
```

- [ ] **Step 2: Configure and run the new target to verify RED**

Run the Windows MinGW debug configure, build `tst_repositionfillpolicy`, and execute it with `QT_QPA_PLATFORM=offscreen`.

Expected: build fails because `playback/repositionfillpolicy.h` and `repositionFillReady` do not exist.

- [ ] **Step 3: Implement the minimal callback policy**

```cpp
template <typename NewestPtsAt>
bool repositionFillReady(int feedCount, int selectedFeed, bool requireAllFeeds,
                         qint64 fillToMs, NewestPtsAt&& newestPtsAt) {
    if (feedCount <= 0) return false;
    if (requireAllFeeds) {
        for (int feed = 0; feed < feedCount; ++feed) {
            if (newestPtsAt(feed) < fillToMs) return false;
        }
        return true;
    }
    const int referenceFeed = selectedFeed >= 0 && selectedFeed < feedCount ? selectedFeed : 0;
    return newestPtsAt(referenceFeed) >= fillToMs;
}
```

Register `tst_repositionfillpolicy` beside the always-enabled playback policy tests in `tests/unit/CMakeLists.txt`.

- [ ] **Step 4: Build and run the policy test to verify GREEN**

Run: `cmake --build --preset windows-mingw-debug --target tst_repositionfillpolicy` followed by the test executable with Qt and MinGW runtime directories on `PATH`.

Expected: all three test cases pass.

- [ ] **Step 5: Commit the policy**

```bash
git add playback/repositionfillpolicy.h tests/unit/tst_repositionfillpolicy.cpp tests/unit/CMakeLists.txt
git commit -m "test: define independent reposition fill policy"
```

### Task 2: Apply independent reference-feed readiness

**Files:**
- Modify: `playback/playbackworker.cpp`
- Modify: `uimanager.cpp`

**Interfaces:**
- Consumes: `repositionFillReady(...)` from Task 1.
- Produces: reference-feed reposition completion in the worker and independent multiview coverage configuration in the UI.

- [ ] **Step 1: Wire the policy into `PlaybackWorker::repositionTo`**

Include `playback/repositionfillpolicy.h`. Replace the `newestPtsMin() >= fillTo` break with a callback that resolves each feed index to its decoder track's `buffer.newestPts()`.

```cpp
const bool fillReady = repositionFillReady(
    m_outputFeedCount, m_selectedOutputFeed.load(std::memory_order_relaxed),
    m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire), fillTo,
    [&](int feedIndex) {
        for (const DecoderTrack* track : m_decoderBank) {
            if (track && track->feedIndex == feedIndex) return track->buffer.newestPts();
        }
        return int64_t(-1);
    });
if ((m_outputFeedCount <= 0 && newestPtsMin() >= fillTo) || fillReady) break;
```

- [ ] **Step 2: Stop enabling all-feed coverage in `UIManager`**

At playback view changes and worker creation/restart, call:

```cpp
m_playbackWorker->setRequireAllOutputFeedsForPlayhead(false);
```

Keep `setSelectedOutputFeed(selectedIndex)`: `-1` maps to feed 0 in multiview and a valid selected index maps to the single-view reference.

- [ ] **Step 3: Build and run focused tests**

Build `OpenLiveReplay`, `tst_repositionfillpolicy`, `tst_livefollowpolicy`, and `tst_outputbusengine` from the Windows debug preset.

Expected: all build successfully; policy and live-follow tests pass. Run `tst_outputbusengine` when present in the non-GPU Windows configuration.

- [ ] **Step 4: Commit the worker integration**

```bash
git add playback/playbackworker.cpp uimanager.cpp
git commit -m "fix: keep feed repositions independent"
```

### Task 3: Runtime regression verification

**Files:**
- No source changes expected.

**Interfaces:**
- Consumes: the repaired `OpenLiveReplay.exe` and existing detailed playback telemetry.
- Produces: evidence that the 40 ms retry storm no longer occurs.

- [ ] **Step 1: Run repository verification**

Run `git diff --check`, the focused tests, and `ctest --test-dir build/debug -L unit --output-on-failure`. Record known Windows application-control failures separately from logical failures.

- [ ] **Step 2: Deploy and launch the repaired app**

Run `windeployqt --qmldir <worktree> --compiler-runtime <worktree>/build/debug/OpenLiveReplay.exe`, then launch with `OLR_PB_TELEMETRY=1`, `OLR_PB_TELEMETRY_DETAIL=1`, and `OLR_E2E_LATENCY_TRACE=1` while capturing stderr.

- [ ] **Step 3: Reproduce live startup**

Start the same four-feed recording. Verify there is no sustained `reposition target 40ms not covered after fill; retrying`, the transport playhead advances, and each feed cache advances independently after its packets arrive.

- [ ] **Step 4: Final commit if verification required no edits**

No commit is needed when verification is clean. If a test-only adjustment was required, commit only that adjustment with a focused message.
