# Seek Smoothness Tier 2 (Structural) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Make every seek (especially far-backward seeks) flash-free and tear-free by committing the playhead atomically against a ready cache, double-buffering decoded frames at the worker, publishing the output cache as an immutable shared snapshot (removing the per-tick deep copy), and skipping redundant sink submits on identical frames.

**Architecture:** The two-thread split stays (PlaybackWorker decodes into `m_outputCache`; OutputRuntime ticks ~1ms and renders via OutputDispatcher). Tier 2 adds a seek-generation pair (`std::atomic<uint64_t> m_seekGeneration` bumped on each new seek target, `std::atomic<uint64_t> m_committedGeneration` + `std::atomic<int64_t> m_committedPlayheadMs` set together once the reposition has decoded a frame at the target) and a committed-playhead gate so the output thread holds the last good playhead **only while a reposition for the latest seek is in flight** and otherwise passes the live transport playhead straight through (steady 1× playback always advances); publishes `m_outputCache` as a `std::shared_ptr<const OutputFrameCache>` swapped atomically so `makeOutputSnapshot` reads it lock-free without a deep copy; double-buffers the reposition decode into staging before trimming old frames; and gives OutputDispatcher per-endpoint identity-skip so unchanged frames are not re-submitted.

**Tech Stack:** C++17, Qt6 (Core/Gui/Multimedia/Test), FFmpeg (libav*), CMake+Ninja, CTest.

## Global Constraints
- Recordings are MPEG-2 ALL-INTRA YUV420P; every frame is independently decodable.
- C++17; match existing Allman-ish brace style of the file you edit; format only changed lines (xcrun clang-format -i changed files; CI gates changed-line format).
- Unit tests are Qt Test, headless: registered in tests/unit/CMakeLists.txt via olr_add_unit_test(<name> olr_test_playback) (or olr_test_core). They run with QT_QPA_PLATFORM=offscreen, TIMEOUT 60.
- Do NOT regress existing E2E scenario gates in tests/e2e/run_playback_e2e.sh: play1x reposition==0; seekplay reposition<=2; stepscrub reposition<=10 & reuseSeek>=1; sliderscrub reposition<=20; reverse reverseChunkSeek<=150 & reposition<=4; liveedge eofTailSeek<=3 & reposition<=3; latency resyncCount==0.
- The worker decode/output is split across two threads: PlaybackWorker (QThread, decodes into m_outputCache under m_bufferMutex) and OutputRuntime (QThread, ticks ~1ms, copies the cache via makeOutputSnapshot, renders via OutputDispatcher to sinks). Any new cross-thread state must be synchronized (atomics or m_bufferMutex/m_outputRuntimeMutex); never introduce a UI-thread blocking call on the seek path.

---

## File Structure

| File | Create/Modify | Responsibility |
| --- | --- | --- |
| `playback/commitgate.h` | Create | Header-only `CommitGate` helper: a pure decision function `visiblePlayheadMs(transport, committed, committedGen, seekGen)` that returns the live transport playhead when no seek is pending (`committedGen == seekGen`) and the held committed playhead while a reposition for the latest seek is in flight (`committedGen != seekGen`). Extracted so it is unit-testable without ffmpeg. |
| `tests/unit/tst_commitgate.cpp` | Create | Qt Test for `CommitGate` commit/lag semantics (Task 1). |
| `playback/playbackworker.h` | Modify | Add `std::atomic<uint64_t> m_seekGeneration`, `std::atomic<uint64_t> m_committedGeneration`, `std::atomic<int64_t> m_committedPlayheadMs`, `std::shared_ptr<const OutputFrameCache>` publication, staging-decode helpers. |
| `playback/playbackworker.cpp` | Modify | Bump `m_seekGeneration` in `seekTo`; set `m_committedGeneration`+`m_committedPlayheadMs` at the tail of `repositionTo` (after the target frame is delivered); compute the visible playhead in `makeOutputSnapshot` via `CommitGate::visiblePlayheadMs`; double-buffer the reposition decode into staging; swap the shared cache atomically on each insert batch / trim; drop the per-tick deep copy. |
| `playback/output/outputframecache.h` | Modify | Add `mergeFrom(const OutputFrameCache&)` (insert-by-pts without clearing) to support staging→live merge. |
| `playback/output/outputframecache.cpp` | Modify | Implement `mergeFrom`. |
| `tests/unit/tst_outputframecache.cpp` | Modify | Test `mergeFrom` insert-by-pts coverage (Task 2). |
| `playback/output/sharedcacheslot.h` | Create | Header-only `SharedCacheSlot`: a mutex-guarded `std::shared_ptr<const OutputFrameCache>` with `publish()` / `load()` — the atomic-swap publication primitive. |
| `tests/unit/tst_sharedcacheslot.cpp` | Create | Qt Test proving a reader sees a consistent immutable snapshot across a concurrent publish (Task 3). |
| `playback/output/outputdispatcher.h` | Modify | Add `OutputDispatchStats::skippedDuplicateFrames`, per-endpoint `lastIdentity` tracking, `setIdentitySkip(bool)`. |
| `playback/output/outputdispatcher.cpp` | Modify | Identity-skip: when `frame.identity.samePayloadAs(lastIdentity)` for an endpoint, skip `sink->submit` and increment `skippedDuplicateFrames` (Task 4). |
| `tests/unit/tst_outputdispatcher.cpp` | Modify | Test identical cache+state twice → one submit, `skippedDuplicateFrames` increments (Task 4). |
| `tests/e2e/play_harness.cpp` | Modify | Add `cacheGeneration=` and `skippedDuplicateFrames=` to the COUNTERS line; add a `farback` scenario (seek end→0, assert placeholderFrames==0). |
| `tests/e2e/run_playback_e2e.sh` | Modify | Parse + gate the `farback` scenario. |
| `tests/e2e/CMakeLists.txt` | Modify | Register `e2e_play_farback` (SRT port 23496 — clear of the play band 23464-23478, the sync band 23480-23489, and the av-sync 23492). |
| `tests/unit/CMakeLists.txt` | Modify | Register `tst_commitgate`, `tst_sharedcacheslot`. |

---

## Task 1 — Atomic commit via a generation token + committed-playhead gate

The far-backward flash today: the UI thread sets `m_transport`'s playhead (uimanager `seekPlayback` → `transport->seek`) **before** PlaybackWorker's `repositionTo` has decoded a frame at the new target. Between those two events `makeOutputSnapshot` reports `state.playheadMs = transport->currentPos()` (playbackworker.cpp:370) against an `m_outputCache` that still holds only the *old* frames — so `videoFrameOrPlaceholder` returns a gray placeholder at the new playhead. Tier 1's hold-last-frame masks the gray with the *old* frame; Tier 2 makes the playhead itself lag until the cache is provably ready, so the output shows the old frame at the *old, correct* playhead (no temporal tear) and snaps to the new frame on the same tick the cache becomes ready.

The committed-playhead logic is pure and lives in `CommitGate` (unit-tested) as a single decision function. The worker holds two atomic generations: `m_seekGeneration` is bumped each time `seekTo()` records a *new* target, and `m_committedGeneration` (paired with `m_committedPlayheadMs`) is advanced at the tail of `repositionTo()` once it has decoded and delivered a frame at that target. When the two generations match, no reposition is outstanding, so `makeOutputSnapshot` exposes the live transport playhead — steady forward playback advances on screen every tick. When they differ, a seek is in flight against a not-yet-ready cache, so the gate holds the last committed playhead until the reposition catches up. Crucially `commit()` is NEVER called with the advancing 1× play position — it only ever records seek targets — so the gate must pass the live playhead through whenever no seek is pending; pinning the output to the last committed *target* unconditionally would freeze 1× playback on screen.

**Files:**
- Create `playback/commitgate.h`
- Create test `tests/unit/tst_commitgate.cpp`
- Modify `tests/unit/CMakeLists.txt`
- Modify `playback/playbackworker.h` (members near :137-169; `makeOutputSnapshot` decl :125)
- Modify `playback/playbackworker.cpp` (`seekTo` :42-55 — bump `m_seekGeneration`; reuse fast-path commit :559; `repositionTo` end :609-611 — commit target; `makeOutputSnapshot` :362-377)

**Interfaces:**
- Produces (`playback/commitgate.h`): a pure decision function in a `CommitGate` namespace/struct — no stored state, all inputs passed in (so the worker owns the atomics, not the gate):
  ```cpp
  namespace CommitGate {
      // Output-thread query: which playhead may the snapshot expose?
      // When committedGen == seekGen there is no reposition outstanding for the
      // latest seek, so steady playback advances — expose the LIVE transport
      // playhead. When they differ a seek is in flight against a not-yet-ready
      // cache, so hold the last committed (good) playhead until the reposition
      // catches up. commit() is only ever called with seek TARGETS, never the
      // advancing 1x position, which is exactly why the no-pending branch must
      // pass the live playhead through (pinning to committedPlayheadMs would
      // freeze 1x playback on screen).
      inline int64_t visiblePlayheadMs(int64_t transportPlayheadMs,
                                       int64_t committedPlayheadMs,
                                       uint64_t committedGen,
                                       uint64_t seekGen) {
          if (committedGen != seekGen) return committedPlayheadMs; // seek pending → hold
          return transportPlayheadMs;                             // no seek pending → advance
      }
  }
  ```
- Consumes (`PlaybackWorker`): three atomics —
  ```cpp
  std::atomic<uint64_t> m_seekGeneration{0};      // bumped in seekTo() per new target
  std::atomic<uint64_t> m_committedGeneration{0}; // advanced in repositionTo() tail
  std::atomic<int64_t>  m_committedPlayheadMs{0}; // set with m_committedGeneration
  ```
  `seekTo` does `m_seekGeneration.fetch_add(1, release)`; `repositionTo`'s tail (after `deliverDueFrames(target,dir)`) sets `m_committedPlayheadMs.store(target, relaxed)` then `m_committedGeneration.store(m_seekGeneration.load(acquire), release)`; `makeOutputSnapshot` reads all three (no `m_bufferMutex` needed — they are atomics).

- [ ] **Step 1: Write failing test for CommitGate.** Create `tests/unit/tst_commitgate.cpp`. It must assert BOTH the anti-freeze case (generations equal → live playhead passes through, so 1× playback advances) and the hold case (generations differ → held committed playhead):
  ```cpp
  #include <QtTest>
  #include "playback/commitgate.h"

  class TestCommitGate : public QObject {
      Q_OBJECT
  private slots:
      void passesLivePlayheadWhenNoSeekPending();   // anti-freeze: 1x playback advances
      void holdsCommittedPlayheadWhileSeekPending(); // seek in flight: hold last good
      void snapsToLiveOnceCommitGenerationCatchesUp();
  };

  // committedGen == seekGen → no reposition outstanding → expose the LIVE
  // transport playhead so steady forward playback keeps advancing on screen.
  void TestCommitGate::passesLivePlayheadWhenNoSeekPending() {
      // seekGen 0, committedGen 0 (equal): live playhead 4000 advances to 4033.
      QCOMPARE(CommitGate::visiblePlayheadMs(4000, /*committed*/ 0, /*cGen*/ 0, /*sGen*/ 0),
               int64_t(4000));
      QCOMPARE(CommitGate::visiblePlayheadMs(4033, /*committed*/ 0, /*cGen*/ 0, /*sGen*/ 0),
               int64_t(4033));
  }

  // committedGen != seekGen → a seek is in flight against a not-yet-ready cache
  // → hold the last committed (good) playhead, ignore the live transport value.
  void TestCommitGate::holdsCommittedPlayheadWhileSeekPending() {
      // seek bumped to gen 2; commit still at gen 1 covering 5000ms. Transport
      // has jumped to 200ms but the cache is not yet ready → hold 5000.
      QCOMPARE(CommitGate::visiblePlayheadMs(200, /*committed*/ 5000, /*cGen*/ 1, /*sGen*/ 2),
               int64_t(5000));
  }

  void TestCommitGate::snapsToLiveOnceCommitGenerationCatchesUp() {
      // Reposition completed: committedGen now equals seekGen (2) and committed
      // the target 200ms. The gate now exposes the live transport playhead again.
      QCOMPARE(CommitGate::visiblePlayheadMs(200, /*committed*/ 200, /*cGen*/ 2, /*sGen*/ 2),
               int64_t(200));
  }

  QTEST_MAIN(TestCommitGate)
  #include "tst_commitgate.moc"
  ```

- [ ] **Step 2: Register the test, run it, watch it fail to compile.** Add to `tests/unit/CMakeLists.txt` (in the playback block near lines 66-78):
  ```cmake
  olr_add_unit_test(tst_commitgate olr_test_playback)
  ```
  Run: `cmake --build build --target tst_commitgate`. Expected: compile error `fatal error: 'playback/commitgate.h' file not found`.

- [ ] **Step 3: Create the minimal CommitGate header.** Write `playback/commitgate.h` exactly as the Interfaces block above, wrapped:
  ```cpp
  #ifndef COMMITGATE_H
  #define COMMITGATE_H

  #include <cstdint>

  // Pure seek-gate decision: no stored state, the worker owns the atomics.
  namespace CommitGate {
      // committedGen == seekGen  → no reposition outstanding → expose the live
      //                            transport playhead (1x playback advances).
      // committedGen != seekGen  → a seek is in flight against a not-yet-ready
      //                            cache → hold the last committed playhead.
      inline int64_t visiblePlayheadMs(int64_t transportPlayheadMs,
                                       int64_t committedPlayheadMs,
                                       uint64_t committedGen,
                                       uint64_t seekGen) {
          if (committedGen != seekGen) return committedPlayheadMs;
          return transportPlayheadMs;
      }
  }

  #endif // COMMITGATE_H
  ```

- [ ] **Step 4: Run it, watch it pass.** `ctest --test-dir build -R tst_commitgate --output-on-failure`. Expected: `100% tests passed`.

- [ ] **Step 5: Commit.** `git add playback/commitgate.h tests/unit/tst_commitgate.cpp tests/unit/CMakeLists.txt && git commit -m "Add CommitGate helper for committed-playhead seek gating"` (end with the Co-Authored-By trailer).

- [ ] **Step 6: Add gate state to PlaybackWorker.** In `playback/playbackworker.h`, add `#include "playback/commitgate.h"` near the other playback includes (after line 21). Add members after `m_reverseAnchorMs` (line 154):
  ```cpp
      // Seek-gate generations (read in makeOutputSnapshot; written in seekTo /
      // repositionTo). When m_committedGeneration == m_seekGeneration there is no
      // reposition outstanding and the live playhead is exposed (1x advances).
      std::atomic<uint64_t> m_seekGeneration{0};
      std::atomic<uint64_t> m_committedGeneration{0};
      std::atomic<int64_t>  m_committedPlayheadMs{0};
  ```

- [ ] **Step 7a: Bump the seek generation in seekTo.** In `playback/playbackworker.cpp` `seekTo` (lines 42-55), after `m_seekTargetMs = clamped;` (line 50) add:
  ```cpp
      // A new seek target is outstanding until repositionTo commits it. The
      // gate holds the last good playhead until m_committedGeneration catches up.
      m_seekGeneration.fetch_add(1, std::memory_order_release);
  ```

- [ ] **Step 7b: Commit the target at the end of repositionTo.** In `playback/playbackworker.cpp`, the full-reposition path ends at lines 609-611. Replace:
  ```cpp
      resetDedup();
      deliverDueFrames(target, dir);
      m_counters.reposition++;
  ```
  with:
  ```cpp
      resetDedup();
      deliverDueFrames(target, dir);
      m_counters.reposition++;

      // Tier 2: the cache now covers `target`. Record the committed playhead
      // first, then advance the committed generation to the latest seek's value
      // — once these match m_seekGeneration, makeOutputSnapshot exposes the live
      // transport playhead again (CommitGate).
      m_committedPlayheadMs.store(target, std::memory_order_relaxed);
      m_committedGeneration.store(m_seekGeneration.load(std::memory_order_acquire),
                                 std::memory_order_release);
  ```
  Also add the same commit to the reuse fast-path (lines 559-560) so a reuse-seek commits its target too. Replace:
  ```cpp
          m_counters.reuseSeek++;
          return;
  ```
  with:
  ```cpp
          m_counters.reuseSeek++;
          m_committedPlayheadMs.store(target, std::memory_order_relaxed);
          m_committedGeneration.store(m_seekGeneration.load(std::memory_order_acquire),
                                     std::memory_order_release);
          return;
  ```
  Note: both commit points run *after* `deliverDueFrames(target, dir)` has put a real frame at `target` into the cache (reuse path at cpp:552, full path at cpp:610), so the generation only catches up once the cache is provably ready.

- [ ] **Step 8: Compute the visible playhead in makeOutputSnapshot.** In `playback/playbackworker.cpp`, `makeOutputSnapshot` (lines 362-377). Replace line 370:
  ```cpp
      snapshot.state.playheadMs = m_transport ? m_transport->currentPos() : 0;
  ```
  with:
  ```cpp
      const qint64 transportPlayhead = m_transport ? m_transport->currentPos() : 0;
      snapshot.state.playheadMs = CommitGate::visiblePlayheadMs(
          transportPlayhead,
          m_committedPlayheadMs.load(std::memory_order_acquire),
          m_committedGeneration.load(std::memory_order_acquire),
          m_seekGeneration.load(std::memory_order_acquire));
  ```
  (No extra lock: the three generation values are atomics. The existing cache copy at lines 366-368 keeps its own short `m_bufferMutex` scope, unchanged. When `m_committedGeneration == m_seekGeneration` — the common steady-state — this returns the live transport playhead, so 1× playback advances every tick; it only holds the last committed playhead during the brief window a seek is in flight.)

- [ ] **Step 9: Build the worker library + E2E harness, run play1x + seekplay E2E to prove no regression.**
  `cmake --build build --target play_harness record_harness`
  `ctest --test-dir build -R "e2e_play_storm|e2e_play_seekplay" --output-on-failure`
  Expected: both pass (play1x/storm reposition==0; seekplay reposition<=2). The gate holds the playhead ONLY during the brief window a seek is in flight (committed generation behind seek generation); steady-state 1× playback (generations equal) exposes the live transport playhead, so it keeps advancing — this is the explicit anti-freeze guarantee the storm gate proves.

- [ ] **Step 10: Commit.** `git add playback/playbackworker.h playback/playbackworker.cpp && git commit -m "Gate output playhead behind committed cache generation"` (Co-Authored-By trailer).

---

## Task 2 — Double-buffer the reposition decode into staging (hold-last-frame at the worker)

Today `repositionTo` calls `clearAllBuffers()` (line 564) — which under Tier 1 becomes `clearDecoderBuffers()` and no longer touches `m_outputCache` — but the **output cache** is then mutated in place as forward fill inserts frames (`decodePacketIntoBank` → `m_outputCache->insertVideoFrame`, line 514). During a far-backward reposition there is a window where the live cache has been trimmed of the old frames but not yet filled with the new ones, so `videoFrameOrPlaceholder` can still resolve to a placeholder. Defense in depth with Tier 1's dispatcher hold-last-frame: decode the target window into a **staging** cache first, then merge it into the live cache and only trim old frames *after* the live cache covers the target. The live cache therefore always contains a real frame at/after the seek for the previous playhead, and gains the new frame atomically (via Task 3's swap).

**Files:**
- Modify `playback/output/outputframecache.h` (add `mergeFrom`)
- Modify `playback/output/outputframecache.cpp`
- Modify test `tests/unit/tst_outputframecache.cpp`
- Modify `playback/playbackworker.h` (add staging-decode helper decl)
- Modify `playback/playbackworker.cpp` (`repositionTo` fill loop :584-607)

**Interfaces:**
- Produces (`OutputFrameCache`): `void mergeFrom(const OutputFrameCache& other);` — inserts every video+audio frame from `other` via the existing insert paths (insert-by-pts; does NOT clear existing frames; feed counts must match).
- Consumes (`PlaybackWorker`): a `std::unique_ptr<OutputFrameCache> m_stagingCache;` (worker-thread-only; never published) reused across repositions.

- [ ] **Step 1: Write a failing test for mergeFrom.** In `tests/unit/tst_outputframecache.cpp`, add a private slot `void mergeFromInsertsByPtsWithoutClearing();` and its body:
  ```cpp
  void TestOutputFrameCache::mergeFromInsertsByPtsWithoutClearing() {
      OutputFrameCache live(1, 4, 4);
      MediaVideoFrame oldF = MediaVideoFrame::solidYuv420p(4, 4, 10, 128, 128);
      oldF.feedIndex = 0; oldF.ptsMs = 5000;
      live.insertVideoFrame(oldF);

      OutputFrameCache staging(1, 4, 4);
      MediaVideoFrame newF = MediaVideoFrame::solidYuv420p(4, 4, 20, 128, 128);
      newF.feedIndex = 0; newF.ptsMs = 200;
      staging.insertVideoFrame(newF);

      live.mergeFrom(staging);

      // Both old and new frames survive the merge (no clear).
      auto atNew = live.videoFrameAt(0, 200);
      auto atOld = live.videoFrameAt(0, 5000);
      QVERIFY(atNew.has_value());
      QVERIFY(atOld.has_value());
      QCOMPARE(int(uchar(atNew->planeY.at(0))), 20);
      QCOMPARE(int(uchar(atOld->planeY.at(0))), 10);
  }
  ```
  (Register the slot in the class's `private slots:` block alongside the existing ones.)

- [ ] **Step 2: Run it, watch it fail.** `cmake --build build --target tst_outputframecache`. Expected: compile error — `'class OutputFrameCache' has no member named 'mergeFrom'`.

- [ ] **Step 3: Declare + implement mergeFrom.** In `playback/output/outputframecache.h`, after `void clear();` (line 22) add:
  ```cpp
      // Insert every frame from `other` (video + audio) without removing the
      // current contents. Feed counts must match. Used to merge a staging
      // window into the live cache before trimming old frames (double-buffer).
      void mergeFrom(const OutputFrameCache& other);
  ```
  In `playback/output/outputframecache.cpp`, add (match the file's brace style):
  ```cpp
  void OutputFrameCache::mergeFrom(const OutputFrameCache& other) {
      const int feeds = qMin(m_video.size(), other.m_video.size());
      for (int feed = 0; feed < feeds; ++feed) {
          for (const MediaVideoFrame& frame : other.m_video.at(feed))
              insertVideoFrame(frame);
      }
      const int aFeeds = qMin(m_audio.size(), other.m_audio.size());
      for (int feed = 0; feed < aFeeds; ++feed) {
          for (const MediaAudioFrame& frame : other.m_audio.at(feed))
              insertAudioFrame(frame);
      }
  }
  ```

- [ ] **Step 4: Run it, watch it pass.** `ctest --test-dir build -R tst_outputframecache --output-on-failure`. Expected: `100% tests passed`.

- [ ] **Step 5: Commit.** `git add playback/output/outputframecache.h playback/output/outputframecache.cpp tests/unit/tst_outputframecache.cpp && git commit -m "Add OutputFrameCache::mergeFrom for staging->live double-buffer"` (Co-Authored-By trailer).

- [ ] **Step 6: Add the staging cache member.** In `playback/playbackworker.h`, after `std::unique_ptr<OutputFrameCache> m_outputCache;` (line 162) add:
  ```cpp
      // Worker-thread-only staging buffer: a reposition decodes the target
      // window here, then merges into the live cache and trims old frames only
      // after coverage (double-buffer; never published to the output thread).
      std::unique_ptr<OutputFrameCache> m_stagingCache;
  ```

- [ ] **Step 7: Route the reposition fill through staging.** In `playback/playbackworker.cpp` `repositionTo`, the fill loop (lines 584-607) currently inserts into both the track buffers and the *live* `m_outputCache` via `decodePacketIntoBank`. Change the loop to decode into staging and merge after coverage. First, before the loop (after line 584 `const int64_t fillTo = ...`) add:
  ```cpp
      if (m_outputFeedCount > 0) {
          if (!m_stagingCache)
              m_stagingCache = std::make_unique<OutputFrameCache>(
                  m_outputFeedCount, m_outputWidth, m_outputHeight);
          else
              m_stagingCache->clear();
      }
  ```
  `decodePacketIntoBank` writes to `m_outputCache` (line 514). To make the fill write to staging without duplicating the function, temporarily swap the live and staging pointers around the fill loop so the existing insert at line 514 lands in staging, then merge+swap-back. Wrap the loop: immediately before `int packets = 0;` (line 585) add:
  ```cpp
      // Decode the target window into staging: swap so decodePacketIntoBank's
      // m_outputCache->insertVideoFrame lands in staging, not the live cache.
      std::unique_ptr<OutputFrameCache> liveSaved;
      if (m_stagingCache) {
          QMutexLocker bufferLocker(&m_bufferMutex);
          liveSaved = std::move(m_outputCache);
          m_outputCache = std::move(m_stagingCache);
      }
  ```
  Immediately after the loop closes (after line 607 `}`) and before `resetDedup();` add:
  ```cpp
      // Merge staging into the live cache, then drop old frames before target.
      if (liveSaved) {
          QMutexLocker bufferLocker(&m_bufferMutex);
          m_stagingCache = std::move(m_outputCache); // staging back
          m_outputCache = std::move(liveSaved);      // live restored (old frames intact)
          m_outputCache->mergeFrom(*m_stagingCache); // live now covers target AND keeps old
          const qint64 keepAudioFromSample =
              qMax<qint64>(0, (target - kLeadMs) * qint64(48000) / 1000);
          m_outputCache->trimBefore(target - kLeadMs, keepAudioFromSample);
      }
  ```
  Note: `trimBefore` keeps one frame before the cutoff (outputframecache.cpp:78-89), so a slightly-before-target playhead still resolves. The merge happens *before* trimming, so the live cache is never momentarily empty at target.

- [ ] **Step 8: Build + run far-backward-relevant E2E.** `cmake --build build --target play_harness record_harness` then `ctest --test-dir build -R "e2e_play_seekplay|e2e_play_reverse|e2e_play_sliderscrub" --output-on-failure`. Expected: all pass within gates (seekplay reposition<=2; reverse reverseChunkSeek<=150 & reposition<=4; sliderscrub reposition<=20). This proves the staging swap did not change reposition counts or break reverse fills.

- [ ] **Step 9: Commit.** `git add playback/playbackworker.h playback/playbackworker.cpp && git commit -m "Double-buffer reposition decode into staging before trimming old frames"` (Co-Authored-By trailer).

---

## Task 3 — Publish m_outputCache as std::shared_ptr<const OutputFrameCache>; remove the per-tick deep copy

`makeOutputSnapshot` (playbackworker.cpp:362-377) currently deep-copies `*m_outputCache` into `snapshot.cache` under `m_bufferMutex` on **every** output tick (~1ms). That copy clones every `QByteArray` plane in the cache (implicitly-shared, but the surrounding `QVector<QVector<...>>` re-allocation and ref-count churn is real work on the hot path). Replace the live cache with a `std::shared_ptr<const OutputFrameCache>` published into a small mutex-guarded slot. The output thread loads the shared_ptr (one short lock, no copy) and reads it immutably; the worker swaps in a fresh shared_ptr after each insert batch / trim. Because the published object is `const` and only ever *replaced* (never mutated), the reader always sees a fully-consistent immutable snapshot. TSan: the only shared mutable is the `shared_ptr` control block + the slot pointer, both serialized by the slot's mutex; no torn reads of cache contents (the object a reader holds is never written again). The repo's TSan leg therefore stays clean — do NOT publish via a bare `std::atomic<std::shared_ptr<...>>` without the mutex, as the existing `m_bufferMutex` discipline is what TSan validates.

**Files:**
- Create `playback/output/sharedcacheslot.h`
- Create test `tests/unit/tst_sharedcacheslot.cpp`
- Modify `tests/unit/CMakeLists.txt`
- Modify `playback/playbackworker.h` (replace `m_outputCache` type; add slot)
- Modify `playback/playbackworker.cpp` (publish after insert/trim; `makeOutputSnapshot` loads the slot)

**Interfaces:**
- Produces (`playback/output/sharedcacheslot.h`):
  ```cpp
  class SharedCacheSlot {
  public:
      void publish(std::shared_ptr<const OutputFrameCache> next) {
          QMutexLocker locker(&m_mutex);
          m_current = std::move(next);
      }
      std::shared_ptr<const OutputFrameCache> load() const {
          QMutexLocker locker(&m_mutex);
          return m_current;
      }
  private:
      mutable QMutex m_mutex;
      std::shared_ptr<const OutputFrameCache> m_current;
  };
  ```
- Consumes (`PlaybackWorker`): a worker-owned mutable `OutputFrameCache` (still `std::unique_ptr<OutputFrameCache> m_outputCache`) PLUS `SharedCacheSlot m_publishedCache`. The worker mutates `m_outputCache` under `m_bufferMutex`, then `m_publishedCache.publish(std::make_shared<const OutputFrameCache>(*m_outputCache))` to expose it. `makeOutputSnapshot` reads `m_publishedCache.load()`.

- [ ] **Step 1: Write a failing test for SharedCacheSlot consistency.** Create `tests/unit/tst_sharedcacheslot.cpp`:
  ```cpp
  #include <QtTest>
  #include <atomic>
  #include <memory>
  #include <thread>
  #include "playback/output/sharedcacheslot.h"

  static std::shared_ptr<const OutputFrameCache> makeCache(uchar y) {
      auto c = std::make_shared<OutputFrameCache>(1, 4, 4);
      MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
      f.feedIndex = 0; f.ptsMs = 100;
      c->insertVideoFrame(f);
      return std::const_pointer_cast<const OutputFrameCache>(c);
  }

  class TestSharedCacheSlot : public QObject {
      Q_OBJECT
  private slots:
      void loadReturnsLastPublished();
      void concurrentPublishYieldsConsistentSnapshot();
  };

  void TestSharedCacheSlot::loadReturnsLastPublished() {
      SharedCacheSlot slot;
      slot.publish(makeCache(11));
      slot.publish(makeCache(22));
      auto got = slot.load();
      QVERIFY(got != nullptr);
      auto frame = got->videoFrameAt(0, 100);
      QVERIFY(frame.has_value());
      QCOMPARE(int(uchar(frame->planeY.at(0))), 22);
  }

  void TestSharedCacheSlot::concurrentPublishYieldsConsistentSnapshot() {
      SharedCacheSlot slot;
      slot.publish(makeCache(1));
      std::atomic<bool> stop{false};
      std::thread writer([&]{
          for (int i = 0; i < 5000 && !stop.load(); ++i)
              slot.publish(makeCache(uchar(50 + (i % 3))));
      });
      for (int i = 0; i < 5000; ++i) {
          auto snap = slot.load();           // never null after first publish
          QVERIFY(snap != nullptr);
          auto frame = snap->videoFrameAt(0, 100);
          QVERIFY(frame.has_value());        // immutable: never torn
          const int v = int(uchar(frame->planeY.at(0)));
          QVERIFY(v == 1 || (v >= 50 && v <= 52));
      }
      stop.store(true);
      writer.join();
  }

  QTEST_MAIN(TestSharedCacheSlot)
  #include "tst_sharedcacheslot.moc"
  ```

- [ ] **Step 2: Register + run, watch it fail.** Add to `tests/unit/CMakeLists.txt` (playback block):
  ```cmake
  olr_add_unit_test(tst_sharedcacheslot olr_test_playback)
  ```
  Run: `cmake --build build --target tst_sharedcacheslot`. Expected: `fatal error: 'playback/output/sharedcacheslot.h' file not found`.

- [ ] **Step 3: Create the slot header.** Write `playback/output/sharedcacheslot.h`:
  ```cpp
  #ifndef SHAREDCACHESLOT_H
  #define SHAREDCACHESLOT_H

  #include "playback/output/outputframecache.h"

  #include <QMutex>
  #include <memory>

  // Single-slot publication of an immutable OutputFrameCache. The worker
  // publishes a fresh shared_ptr after each cache mutation; the output thread
  // loads it lock-cheap and reads it immutably. The published object is const
  // and only ever replaced, so a loaded snapshot is never written again.
  class SharedCacheSlot {
  public:
      void publish(std::shared_ptr<const OutputFrameCache> next) {
          QMutexLocker locker(&m_mutex);
          m_current = std::move(next);
      }
      std::shared_ptr<const OutputFrameCache> load() const {
          QMutexLocker locker(&m_mutex);
          return m_current;
      }
  private:
      mutable QMutex m_mutex;
      std::shared_ptr<const OutputFrameCache> m_current;
  };

  #endif // SHAREDCACHESLOT_H
  ```

- [ ] **Step 4: Run it, watch it pass (and clean under TSan if the TSan leg runs it).** `ctest --test-dir build -R tst_sharedcacheslot --output-on-failure`. Expected: `100% tests passed`, no TSan data-race report.

- [ ] **Step 5: Commit.** `git add playback/output/sharedcacheslot.h tests/unit/tst_sharedcacheslot.cpp tests/unit/CMakeLists.txt && git commit -m "Add SharedCacheSlot for immutable output-cache publication"` (Co-Authored-By trailer).

- [ ] **Step 6: Add the slot to PlaybackWorker.** In `playback/playbackworker.h` add `#include "playback/output/sharedcacheslot.h"` after the existing `outputruntime.h` include (line 16). After `std::unique_ptr<OutputFrameCache> m_outputCache;` (line 162) add:
  ```cpp
      // Immutable snapshot of m_outputCache published to the output thread
      // (replaces the per-tick deep copy in makeOutputSnapshot).
      SharedCacheSlot m_publishedCache;
  ```

- [ ] **Step 7: Add a publish helper and call it after every cache mutation.** In `playback/playbackworker.h`, near the other private decls (after `OutputRuntimeSnapshot makeOutputSnapshot() const;`, line 125) add:
  ```cpp
      // Snapshot m_outputCache into the published immutable slot. Caller must
      // hold m_bufferMutex.
      void publishOutputCacheLocked();
  ```
  In `playback/playbackworker.cpp`, add (above `makeOutputSnapshot`):
  ```cpp
  void PlaybackWorker::publishOutputCacheLocked() {
      if (m_outputCache)
          m_publishedCache.publish(std::make_shared<const OutputFrameCache>(*m_outputCache));
  }
  ```
  Now call it everywhere `m_outputCache` is mutated under `m_bufferMutex`:
  - After the insert at line 514 (inside the existing `QMutexLocker bufferLocker(&m_bufferMutex);` scope opened at 511): add `publishOutputCacheLocked();` after `if (m_outputCache) m_outputCache->insertVideoFrame(mediaFrame);`.
  - After the trim block at lines 1021-1024: add `publishOutputCacheLocked();` inside the `if (m_outputCache)` so it republishes post-trim.
  - In `repositionTo`'s Task 2 merge block (the `m_outputCache->trimBefore(...)` you added): add `publishOutputCacheLocked();` as the last statement inside that `QMutexLocker` scope.
  - In `clearAllBuffers`/`clearDecoderBuffers` and anywhere the cache is cleared, republish the (now possibly empty) cache so the output thread does not keep stale frames after a hard clear.

- [ ] **Step 8: Remove the per-tick deep copy in makeOutputSnapshot.** In `playback/playbackworker.cpp` `makeOutputSnapshot` (lines 362-377), replace the body's cache copy (lines 364-370, as edited in Task 1 Step 8) with a single slot load; the gate reads atomics, so it stays outside the lock:
  ```cpp
  OutputRuntimeSnapshot PlaybackWorker::makeOutputSnapshot() const {
      OutputRuntimeSnapshot snapshot;
      const qint64 transportPlayhead = m_transport ? m_transport->currentPos() : 0;
      {
          QMutexLocker bufferLocker(&m_bufferMutex);
          if (auto published = m_publishedCache.load())
              snapshot.cache = *published;        // copy the immutable snapshot's contents
          else
              snapshot.cache = OutputFrameCache(m_outputFeedCount, m_outputWidth, m_outputHeight);
      }
      snapshot.state.playheadMs = CommitGate::visiblePlayheadMs(
          transportPlayhead,
          m_committedPlayheadMs.load(std::memory_order_acquire),
          m_committedGeneration.load(std::memory_order_acquire),
          m_seekGeneration.load(std::memory_order_acquire));
      snapshot.state.playing = m_transport && m_transport->isPlaying();
      snapshot.state.speed = m_transport ? m_transport->speed() : 1.0;
      snapshot.state.selectedFeedIndex = m_selectedOutputFeed.load(std::memory_order_relaxed);
      if (snapshot.state.selectedFeedIndex < 0 && m_outputFeedCount > 0)
          snapshot.state.selectedFeedIndex = 0;
      return snapshot;
  }
  ```
  Note: `OutputRuntimeSnapshot::cache` is by-value (outputruntime.h:11), so we still assign into it — but from the shared immutable snapshot, which is an implicitly-shared-QByteArray copy (cheap), not a re-decode. The previously-held `m_bufferMutex` is now taken once and does not block the worker's mutations any longer than the load itself. (A later optimization could make `OutputRuntimeSnapshot::cache` itself a `shared_ptr` to drop this last copy; YAGNI for Tier 2 — the deep clone of decoder track buffers is what's removed here.)

- [ ] **Step 9: Build + run the full unit suite and the latency E2E (TSan-sensitive).**
  `cmake --build build --target play_harness record_harness`
  `ctest --test-dir build -L unit --output-on-failure`
  `ctest --test-dir build -R "e2e_play_latency|e2e_play_storm" --output-on-failure`
  Expected: all unit tests pass; latency resyncCount==0; play1x reposition==0. If the TSan leg runs, confirm no race on `m_outputCache` (all access is under `m_bufferMutex`; the output thread only touches the immutable `m_publishedCache.load()` result).

- [ ] **Step 10: Commit.** `git add playback/playbackworker.h playback/playbackworker.cpp && git commit -m "Publish output cache as immutable shared snapshot; drop per-tick deep copy"` (Co-Authored-By trailer).

---

## Task 4 — OutputDispatcher identity-skip

Today `dispatchTick` (outputdispatcher.cpp:72-99) calls `endpoint.sink->submit(frame)` on every tick for every endpoint, even when the frame's payload is byte-identical to the last one that endpoint received. `OutputTargetDispatchStats::lastIdentity` is already tracked (outputdispatcher.cpp:199-203) and `repeatedPayloadFrames` counts repeats, but the submit (and inside QtPreviewSink the `toQVideoFrame` map/copy, and any frameprovider deliver) still runs. Identity-skip: when `frame.identity.samePayloadAs(lastIdentity)` for that endpoint, skip the submit entirely and increment a new global `skippedDuplicateFrames`. Guarded by `setIdentitySkip(bool)` (default true) so tests can force-submit.

**Files:**
- Modify `playback/output/outputdispatcher.h` (add stat field, member flag, setter)
- Modify `playback/output/outputdispatcher.cpp` (`dispatchTick` :72-99)
- Modify test `tests/unit/tst_outputdispatcher.cpp`

**Interfaces:**
- Produces:
  - `OutputDispatchStats::skippedDuplicateFrames` (`qint64`, near placeholderFrames at outputdispatcher.h:66).
  - `void OutputDispatcher::setIdentitySkip(bool enabled);` (default member `bool m_identitySkip = true;`).
- Consumes: existing per-endpoint `OutputTargetDispatchStats::lastIdentity` + `hasLastIdentity` (outputdispatcher.h:43-44), populated by `countTargetAttempt` (cpp:199-203).

- [ ] **Step 1: Write a failing test.** In `tests/unit/tst_outputdispatcher.cpp` add a private slot `void identicalConsecutiveTicksSkipDuplicateSubmit();` and body (reuse the existing `CollectingSink` at lines 13-46 and `video(...)` helper at lines 6-11):
  ```cpp
  void TestOutputDispatcher::identicalConsecutiveTicksSkipDuplicateSubmit() {
      OutputFrameCache cache(1, 4, 4);
      cache.insertVideoFrame(video(0, 100, 40));

      PlaybackStateSnapshot state;
      state.playheadMs = 100;
      state.playing = false;          // paused: same playhead+frame every tick
      state.speed = 1.0;
      state.selectedFeedIndex = 0;

      OutputTargetAssignment qt;
      qt.id = QStringLiteral("feed0-preview");
      qt.sourceBus = OutputBusId::feed(0);
      qt.kind = OutputTargetKind::QtPreview;
      qt.enabled = true;

      CollectingSink qtSink(OutputTargetKind::QtPreview);
      OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
      dispatcher.setEndpoints({{qt, &qtSink}});

      dispatcher.dispatchTick(cache, state);   // first: real submit
      const OutputDispatchStats after2 = dispatcher.dispatchTick(cache, state); // dup: skipped

      QCOMPARE(qtSink.frames.size(), 1);                 // only one submit reached the sink
      QCOMPARE(after2.skippedDuplicateFrames, qint64(1));
  }
  ```
  Register the slot in the `private slots:` block (lines 50-61).

- [ ] **Step 2: Run it, watch it fail.** `cmake --build build --target tst_outputdispatcher`. Expected: compile error `'struct OutputDispatchStats' has no member named 'skippedDuplicateFrames'`.

- [ ] **Step 3: Add the stat field, flag, and setter.** In `playback/output/outputdispatcher.h`, after `qint64 placeholderFrames = 0;` (line 66) add:
  ```cpp
      qint64 skippedDuplicateFrames = 0;
  ```
  After `void setRuntimeStats(...)` (line 83) add:
  ```cpp
      void setIdentitySkip(bool enabled) { m_identitySkip = enabled; }
  ```
  In the private members after `OutputDispatchStats m_stats;` (line 109) add:
  ```cpp
      bool m_identitySkip = true;
  ```

- [ ] **Step 4: Implement the skip in dispatchTick.** In `playback/output/outputdispatcher.cpp` (lines 78-95), replace the per-endpoint submit block:
  ```cpp
          const OutputBusFrame frame = rendered.value(bus);
          const bool submitted = endpoint.sink->submit(frame);
          countTargetAttempt(endpoint.assignment, frame, submitted);
          if (submitted) {
              m_stats.framesSubmitted++;
          } else {
              m_stats.sinkFailures++;
          }
  ```
  with:
  ```cpp
          const OutputBusFrame frame = rendered.value(bus);

          // Identity-skip: if this endpoint already received a byte-identical
          // payload, skip the submit (and the sink's map/copy/deliver entirely).
          OutputTargetDispatchStats& tstats = m_stats.targets[targetStatsKey(endpoint.assignment)];
          if (m_identitySkip && tstats.hasLastIdentity &&
              tstats.lastIdentity.samePayloadAs(frame.identity)) {
              tstats.repeatedPayloadFrames++;
              m_stats.skippedDuplicateFrames++;
              continue;
          }

          const bool submitted = endpoint.sink->submit(frame);
          countTargetAttempt(endpoint.assignment, frame, submitted);
          if (submitted) {
              m_stats.framesSubmitted++;
          } else {
              m_stats.sinkFailures++;
          }
  ```
  **Double-count guard (REQUIRED):** `repeatedPayloadFrames` must be incremented EXACTLY ONCE per repeated payload, never on both the skip path and the submit path. The submit path's count lives inside `countTargetAttempt` (outputdispatcher.cpp:199-203: `if (stats.hasLastIdentity && stats.lastIdentity.samePayloadAs(frame.identity)) stats.repeatedPayloadFrames++;`). The skip path here does its OWN `tstats.repeatedPayloadFrames++` and then `continue`s — so it never reaches `countTargetAttempt`. That is the no-double-count invariant: a repeated payload is counted by the skip branch when `m_identitySkip` is on, and by `countTargetAttempt` only when the frame is actually submitted (identity-skip off, or first occurrence). Do NOT call `countTargetAttempt` on the skip branch and do NOT add a second `repeatedPayloadFrames++` outside these two mutually-exclusive paths. The skip path also leaves `tstats.lastIdentity` unchanged (so the next identical tick also skips); `countTargetAttempt` keeps owning `lastIdentity` for the submitted path.

- [ ] **Step 5: Keep the existing repeated-payload test green by forcing submits.** The existing `targetStatsTrackRepeatedPayloadsAndFailuresIndependently` test (tst_outputdispatcher.cpp:266-320) dispatches the same frame twice and asserts `attemptedFrames==2` / `framesSubmitted==2` / `repeatedPayloadFrames==1` per target — which only holds if the second tick actually SUBMITS. With identity-skip defaulting on, the second tick would now be skipped (so `attemptedFrames` would be 1). Add `dispatcher.setIdentitySkip(false);` to that test right after `dispatcher.setEndpoints({...})` (before the two `dispatchTick` calls) so it exercises the submit path it was written for. Its `repeatedPayloadFrames==1` assertion then proves the no-double-count invariant on the submit side. This test is a REQUIRED passing gate for Task 4.

- [ ] **Step 6: Run it, watch it pass.** `ctest --test-dir build -R tst_outputdispatcher --output-on-failure`. Expected: `100% tests passed` — the new `identicalConsecutiveTicksSkipDuplicateSubmit` (skip path: `skippedDuplicateFrames==1`, one submit) AND `targetStatsTrackRepeatedPayloadsAndFailuresIndependently` (submit path with identity-skip off: `repeatedPayloadFrames==1`, two submits) MUST both pass, jointly proving the repeat is counted exactly once on each path.

- [ ] **Step 7: Commit.** `git add playback/output/outputdispatcher.h playback/output/outputdispatcher.cpp tests/unit/tst_outputdispatcher.cpp && git commit -m "Skip redundant sink submits on identical frame identity"` (Co-Authored-By trailer).

---

## Task 5 — E2E: cacheGeneration + skippedDuplicateFrames counters and the `farback` flash gate

Wire the new structural state into the harness so a far-backward seek is gated end-to-end (placeholderFrames==0 after the first committed frame), and surface `skippedDuplicateFrames` so a paused/idle output proves the skip fires.

**Files:**
- Modify `tests/e2e/play_harness.cpp` (COUNTERS `printf` format string + args :86-90 AND the unknown-scenario fallback `printf` :201-204; scenario switch)
- Modify `tests/e2e/run_playback_e2e.sh` (add `get` parse lines :142-157; case block :171-282)
- Modify `tests/e2e/CMakeLists.txt` (add_test block :134-163 + the `set_tests_properties` LABELS list :161-167; port 23496)

**Interfaces:**
- Consumes: `worker.outputStats()` → `OutputDispatchStats{ placeholderFrames, skippedDuplicateFrames }`; the existing `placeholderFrames=` field added by Tier 1.
- Produces: COUNTERS line tokens `cacheGeneration=<n>` (the *committed* generation — proves at least one reposition has decoded and committed a frame at its target — exposed via a new accessor `uint64_t cacheGeneration() const { return m_committedGeneration.load(std::memory_order_acquire); }` in `playback/playbackworker.h` public section) and `skippedDuplicateFrames=<n>`.

- [ ] **Step 1: Extend the COUNTERS printf with the new tokens (failing build first).** The COUNTERS line in `tests/e2e/play_harness.cpp` is emitted by a single `printf(format, args...)` (play_harness.cpp:86-90), NOT C++ stream insertion — so extend the format string and the trailing arg list exactly the way Tier 1 already did (Tier 1 added `placeholderFrames=%d heldFrames=%d`). Add `%lld` tokens for the two new counters and matching args cast to `(long long)`. There are TWO copies of this `printf` (the normal `finish` lambda at :86-90 and the unknown-scenario fallback at :201-204) — extend BOTH identically. The corrected `finish`-lambda printf:
  ```cpp
      printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
             "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d "
             "skippedDuplicateFrames=%lld cacheGeneration=%lld\n",
             c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
             c.audioPushes, c.framesDropped, audio.resyncCount(),
             (long long) worker.outputStats().skippedDuplicateFrames,
             (long long) worker.cacheGeneration());
  ```
  (If Tier 1's `placeholderFrames=%d heldFrames=%d` tokens are already present in the format string + arg list, keep them and append the two new `%lld` tokens after them — do not drop the Tier 1 tokens.) Build: `cmake --build build --target play_harness`. Expected: error `'class PlaybackWorker' has no member named 'cacheGeneration'`.

- [ ] **Step 2: Expose the accessor.** In `playback/playbackworker.h` public section (after `OutputDispatchStats outputStats() const;`, line 72) add (the *committed* generation, set at `repositionTo`'s tail in Task 1 — `cacheGeneration>=1` after a real reposition proves a target was committed):
  ```cpp
      uint64_t cacheGeneration() const { return m_committedGeneration.load(std::memory_order_acquire); }
  ```
  Build: `cmake --build build --target play_harness`. Expected: builds clean.

- [ ] **Step 3: Add the `farback` scenario to the harness.** `farback` is the far-BACKWARD seek case: start playing near EOF, then jump all the way back to 0 (the worst case for the flash — the live cache holds only end-of-file frames when the playhead snaps to 0). This is NOT seekplay's seek-to-mid, so do not "mirror seekplay verbatim". Add this branch to the `tests/e2e/play_harness.cpp` scenario dispatch (alongside the other `else if (scen == ...)` arms; `near` mirrors the `liveedge` start, the back-seek uses both `transport.seek(0)` and `worker.seekTo(0)` like the other scrub scenarios):
  ```cpp
      } else if (scen == "farback") {
          // Far-BACKWARD seek: play near EOF, then jump to 0. The live cache holds
          // only end-of-file frames at the instant the playhead snaps to 0, so this
          // is the worst case for the seek flash. The CommitGate (Task 1) + worker
          // double-buffer (Task 2) must keep placeholderFrames==0 across the jump.
          const int64_t near = qMax<int64_t>(0, durMs - 1000);
          transport.setSpeed(1.0);
          transport.seek(near);
          worker.seekTo(near);
          transport.setPlaying(true);
          // Let it settle near EOF (~1s), then far-backward seek to 0.
          QTimer::singleShot(1000, &app, [&]() {
              fprintf(stderr, "### far-backward seek to 0 ###\n");
              transport.seek(0);
              worker.seekTo(0);
          });
          QTimer::singleShot(6000, &app, finish);
  ```

- [ ] **Step 4: Register the gate in the shell harness.** `run_playback_e2e.sh` does NOT have `assert_le`/`assert_eq`/`assert_ge` helpers — it parses each field with `get <name>` into a shell var and gates with a `case "$SCENARIO"` block using `num`/`-gt`/`-ne`/`-lt` checks (see the existing `seekplay`/`liveedge` arms at lines 186-263). Two new fields must first be parsed alongside the existing ones (after the `resyncCount="$(get resyncCount)"` line at :149, and a matching `[ -n ... ] || ...="?"` fallback at :157):
  ```sh
  placeholderFrames="$(get placeholderFrames)"
  cacheGeneration="$(get cacheGeneration)"
  [ -n "$placeholderFrames" ] || placeholderFrames="?"
  [ -n "$cacheGeneration" ] || cacheGeneration="?"
  ```
  (Tier 1 already added the `placeholderFrames="$(get placeholderFrames)"` line; if present, reuse it and only add `cacheGeneration`.) Then add a `farback)` arm to the `case "$SCENARIO"` block (lines 171-282), in the file's existing idiom — `placeholderFrames==0` is the flash gate, `cacheGeneration>=1` proves a reposition committed:
  ```sh
      farback)
          # Far-backward seek (near EOF -> 0). The committed-playhead gate + worker
          # double-buffer must keep the flash gate clean: NO placeholder frames and
          # a bounded reposition count, with at least one committed cache generation.
          if ! num "$reposition" || [ "$reposition" -gt 2 ]; then
              echo "FAIL: farback repositioned too much (reposition=$reposition, expected <=2)"
              fail=1
          fi
          if ! num "$placeholderFrames" || [ "$placeholderFrames" -ne 0 ]; then
              echo "FAIL: farback flashed a placeholder (placeholderFrames=$placeholderFrames, expected 0) — seek flash regressed"
              fail=1
          fi
          if ! num "$cacheGeneration" || [ "$cacheGeneration" -lt 1 ]; then
              echo "FAIL: farback never committed a cache generation (cacheGeneration=$cacheGeneration, expected >=1)"
              fail=1
          fi
          ;;
  ```
  (Optionally extend the `SUMMARY=` line at :284 to include `placeholderFrames=$placeholderFrames cacheGeneration=$cacheGeneration` for diagnostics.)

- [ ] **Step 5: Register the ctest.** In `tests/e2e/CMakeLists.txt` (add_test block 134-163), add an `e2e_play_farback` entry mirroring `e2e_play_seekplay` with SRT port `23496` (the play band ends at 23478, the sync band is 23480-23489, and av-sync uses 23492, so 23496 is the next clear port; the UDP producer port is SRT+1 = 23497). Also append `e2e_play_farback` to the `set_tests_properties(... PROPERTIES LABELS "e2e" ...)` list at lines 161-167.
  ```cmake
  add_test(NAME e2e_play_farback
      COMMAND bash "${_pb_driver}"
          "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" farback 2 23496)
  ```

- [ ] **Step 6: Build + run the new scenario.** `cmake --build build --target play_harness record_harness` then `ctest --test-dir build -R e2e_play_farback --output-on-failure`. Expected: pass — `placeholderFrames=0`, `cacheGeneration>=1`. This is the end-to-end proof that the committed-playhead gate (Task 1) + worker double-buffer (Task 2) make a far-backward seek flash-free.

- [ ] **Step 7: Run the full E2E suite to confirm no regression.** `ctest --test-dir build -R "e2e_play_" --output-on-failure`. Expected: every existing scenario passes its gate (play1x reposition==0; seekplay reposition<=2; stepscrub reposition<=10 & reuseSeek>=1; sliderscrub reposition<=20; reverse reverseChunkSeek<=150 & reposition<=4; liveedge eofTailSeek<=3 & reposition<=3; latency resyncCount==0) plus the Tier 1 `seekflash` and the new `farback`.

- [ ] **Step 8: Commit.** `git add tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/e2e/CMakeLists.txt playback/playbackworker.h && git commit -m "E2E: gate far-backward seeks flash-free; surface skip + generation counters"` (Co-Authored-By trailer).

---

## Parallelization within Tier 2

- **Task 4 (identity-skip)** is fully independent of Tasks 1-3: it touches only `outputdispatcher.{h,cpp}` and its unit test, reads no worker state, and can be implemented/merged in parallel by a separate agent.
- **Task 1 (CommitGate)** and **Task 2 (mergeFrom + staging)** share `repositionTo` and `playbackworker.{h,cpp}`. Task 1's commit point sits *after* Task 2's merge/trim block, so if done in parallel they must be reconciled in `repositionTo`'s tail — sequence them (Task 1 then Task 2, or vice versa) on the same branch to avoid a merge conflict in that function. The `CommitGate` header (Task 1 Steps 1-5) and the `mergeFrom`/`OutputFrameCache` change (Task 2 Steps 1-5) ARE independent and can be built in parallel; only the `playbackworker.cpp` wiring steps must serialize.
- **Task 3 (SharedCacheSlot)** depends on Task 1's `makeOutputSnapshot` gate edit (it rewrites the same function — keep Task 1's atomic `CommitGate::visiblePlayheadMs(...)` playhead computation, which lives OUTSIDE the `m_bufferMutex` scope) and on Task 2's merge block (it adds a `publishOutputCacheLocked()` call inside it). Do Task 3 after Tasks 1 and 2 on the same branch. The `SharedCacheSlot` header + test (Task 3 Steps 1-5) are independent and can be built in parallel with everything else.
- **Task 5 (E2E)** depends on Tasks 1-3 being merged (it asserts the gate + generation) and on Task 4 (it surfaces `skippedDuplicateFrames`). It is the last task.

Recommended split: Agent A → Task 4 (independent, mergeable immediately). Agent B → Task 1 then Task 2 then Task 3 on one branch (shared `playbackworker.cpp`). Both feed Task 5.

## Dependencies on Tier 1

Tier 2 builds directly on the Tier 1 SHARED SYMBOL CONTRACT and must NOT be started until Tier 1 is merged:

- **`PlaybackWorker::clearDecoderBuffers()`** (Tier 1) replaces `clearAllBuffers()` and no longer clears `m_outputCache`. Task 2's double-buffer relies on the live `m_outputCache` retaining old frames across a reposition — which is only true once Tier 1 has removed the `m_outputCache->clear()` from the clear path (formerly playbackworker.cpp:237 / the clear at repositionTo:564). If Tier 2 lands first, the old frames are wiped and the staging merge has nothing to hold over.
- **OutputDispatcher hold-last-frame + `setHoldLastFrame(bool)` + `heldFrames`** (Tier 1) is the defense-in-depth partner to Task 2: Tier 1 holds the last good frame at the *dispatcher* if a placeholder ever slips through; Tier 2's CommitGate (Task 1) + worker double-buffer (Task 2) ensure a placeholder is not produced in the first place. The `farback` E2E gate (Task 5) asserts `placeholderFrames==0`, which only holds with both layers present.
- **`convertToMediaVideoFrame` using `QByteArray(size, Qt::Uninitialized)`** (Tier 1) is unrelated to Tier 2 logic but is assumed already in place; do not re-touch it.
- **`play_harness` COUNTERS line carrying `placeholderFrames=` and `heldFrames=`, plus the `seekflash` scenario (port 23494)** (Tier 1) are extended (not replaced) by Task 5, which appends `skippedDuplicateFrames=` and `cacheGeneration=` to the existing `printf` format string and adds the `farback` scenario at port 23496. Keep the Tier 1 tokens intact so the existing `seekflash` gate still parses.
