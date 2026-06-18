# Tier 3 — Frame-Perfect EVS-Style Timestamp Playlist Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Add a frame-perfect, EVS-style timestamp playlist so a recalled cut snaps to the exact frame and audio with zero gray flashes and zero reposition churn, by pre-rolling the target window into a staging cache and atomically promoting it at a scheduled output frame.

**Architecture:** A new pure-logic `FrameIndex` maps PTS(ms)→byte-offset for exact `avio_seek` decode-one-frame repositioning of the all-intra recordings. A new pure-logic `ReplayPlaylist` (cue list) stores ordered `{clipPath,inMs,outMs,speed}` entries with mark-in/mark-out/recall and JSON round-trip. `PlaybackWorker` gains a second `AVFormatContext` decoder bank that pre-rolls the armed target window into a staging `OutputFrameCache` while the primary keeps playing; an atomic `scheduleCutAtFrame` promotes staging→active exactly when `OutputDispatcher::m_nextOutputFrameIndex` reaches the scheduled output frame, combined with the Tier 2 cache generation token, with pre-rolled crossfade audio aligned at the cut frame.

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
| `playback/frameindex.h` | Create | `FrameIndex` declaration: incremental PTS(ms)→byte-offset map + `nearestAtOrBefore`. Pure data structure. |
| `playback/frameindex.cpp` | Create | `FrameIndex` implementation. |
| `tests/unit/tst_frameindex.cpp` | Create | Full Qt Test for `FrameIndex` (append, query, monotonic, gaps, growth). |
| `playback/replayplaylist.h` | Create | `ReplayEntry` struct + `ReplayPlaylist` cue list: mark-in/mark-out/recall + JSON round-trip. Pure logic. |
| `playback/replayplaylist.cpp` | Create | `ReplayPlaylist` implementation. |
| `tests/unit/tst_replayplaylist.cpp` | Create | Full Qt Test for `ReplayPlaylist`. |
| `playback/cutschedule.h` | Create | `CutSchedule` pure-math helper: wall/playhead↔output-frame mapping + `outputFrameForCut`. |
| `playback/cutschedule.cpp` | Create | `CutSchedule` implementation. |
| `tests/unit/tst_cutschedule.cpp` | Create | Full Qt Test for `CutSchedule` boundary math. |
| `tests/CMakeLists.txt` | Modify | Add `frameindex.cpp`, `replayplaylist.cpp`, `cutschedule.cpp` to `olr_test_playback` STATIC (anchor on the `playback/trackbuffer.cpp` source-path string inside the `qt_add_library(olr_test_playback STATIC ...)` block) and to the app target. |
| `tests/unit/CMakeLists.txt` | Modify | Register `tst_frameindex`, `tst_replayplaylist`, `tst_cutschedule` via `olr_add_unit_test(... olr_test_playback)`. |
| `playback/playbackworker.h` | Modify | Add `FrameIndex m_frameIndex`; pre-roll decoder bank members; `armNextCut`, `scheduleCutAtFrame`, `repositionExact`; staging `OutputFrameCache`. |
| `playback/playbackworker.cpp` | Modify | Record offsets in read loop; `repositionExact` via `avio_seek`; pre-roll fill loop; atomic cut promotion against `OutputDispatcher::nextOutputFrameIndex()`; crossfade audio at cut. |
| `playback/output/outputdispatcher.h` | Modify | Expose a thread-safe `nextOutputFrameIndex()` read already exists (line 82); add `armCutAtFrame(qint64,std::function)` hook OR keep poll model (see Task 9 design). |
| `uimanager.h` | Modify | Add `ReplayPlaylist m_playlist`; Q_INVOKABLE `markIn/markOut/recallEntry`. |
| `uimanager.cpp` | Modify | Wire `markIn/markOut/recallEntry` → `m_playlist` + `m_playbackWorker->armNextCut`. |
| `Main.qml` | Modify | Minimal hooks: Mark-In / Mark-Out / Recall buttons calling `uiManagerRef`. |
| `tests/e2e/play_harness.cpp` | Modify | Add `armedcut` scenario; add `reposition`/`placeholderFrames` already on COUNTERS line. |
| `tests/e2e/run_playback_e2e.sh` | Modify | Add `armedcut` case: `placeholderFrames==0 && reposition==0`. |
| `tests/e2e/CMakeLists.txt` | Modify | Register `e2e_play_armedcut` (next free SRT port 23498, passed as the 5th positional arg; added to the shared play-gate properties block). |

---

## What is pure logic vs integration/E2E

**Pure logic (full TDD, real code in this plan, Qt Test unit-tested):**
- Task 1–2: `FrameIndex` (append + `nearestAtOrBefore`).
- Task 3–4: `ReplayPlaylist` (entries, mark, recall, JSON round-trip).
- Task 5–6: `CutSchedule` (output-frame mapping math).

**Integration (design + bite-sized tasks; touches the read loop / `avio_seek` / transport):**
- Task 7: wire `FrameIndex` into the worker read loop + `repositionExact`.
- Task 8: wire `markIn/markOut/recall` into `UIManager` + `Main.qml`.

**Integration / ffmpeg / thread-heavy (design + task breakdown; E2E-gated, NOT unit-tested):**
- Task 9: pre-roll decoder bank (`armNextCut`, staging fill loop, warm pool).
- Task 10: atomic cut (`scheduleCutAtFrame`) promoting staging→active at the scheduled output frame, combined with the Tier 2 generation token.
- Task 11: pre-roll + crossfade audio for the armed cut.
- Task 12: `armedcut` E2E scenario asserting `placeholderFrames==0 && reposition==0`.

`PlaybackWorker` needs a real MKV + ffmpeg, so its behavior is exercised via `tests/e2e/play_harness`, NOT a unit test. The pure-logic classes are deliberately split out so the math is unit-testable in isolation.

---

## Task 1: FrameIndex — append + nearestAtOrBefore (pure logic)

**Files:**
- Create `playback/frameindex.h`
- Create `playback/frameindex.cpp`
- Test: `tests/unit/tst_frameindex.cpp`
- Modify `tests/CMakeLists.txt` (add `frameindex.cpp` to `olr_test_playback` STATIC — anchor on the `playback/trackbuffer.cpp` source-path string inside the `qt_add_library(olr_test_playback STATIC ...)` block)
- Modify `tests/unit/CMakeLists.txt` (register `tst_frameindex`)

**Interfaces:**
- Produces:
  - `void FrameIndex::append(qint64 ptsMs, qint64 byteOffset);`
  - `std::optional<qint64> FrameIndex::nearestAtOrBefore(qint64 ptsMs) const;`
  - `int FrameIndex::size() const;`
  - `void FrameIndex::clear();`
- Consumes: nothing (pure).

- [ ] **Step 1: Write failing test for empty + single append.**
  Create `tests/unit/tst_frameindex.cpp`:
  ```cpp
  #include <QtTest>
  #include "playback/frameindex.h"

  class TestFrameIndex : public QObject { Q_OBJECT
  private slots:
      void emptyReturnsNullopt();
      void singleEntryExactAndBefore();
      void nearestPicksLargestAtOrBefore();
      void beforeFirstReturnsNullopt();
      void nonMonotonicAppendIsIgnored();
      void clearResets();
  };

  void TestFrameIndex::emptyReturnsNullopt() {
      FrameIndex idx;
      QCOMPARE(idx.size(), 0);
      QVERIFY(!idx.nearestAtOrBefore(0).has_value());
      QVERIFY(!idx.nearestAtOrBefore(1000).has_value());
  }

  void TestFrameIndex::singleEntryExactAndBefore() {
      FrameIndex idx;
      idx.append(100, 4096);
      QCOMPARE(idx.size(), 1);
      QCOMPARE(idx.nearestAtOrBefore(100).value(), qint64(4096));
      QCOMPARE(idx.nearestAtOrBefore(150).value(), qint64(4096));
  }

  void TestFrameIndex::nearestPicksLargestAtOrBefore() {
      FrameIndex idx;
      idx.append(0, 0);
      idx.append(40, 1000);
      idx.append(80, 2000);
      idx.append(120, 3000);
      QCOMPARE(idx.nearestAtOrBefore(0).value(), qint64(0));
      QCOMPARE(idx.nearestAtOrBefore(39).value(), qint64(0));
      QCOMPARE(idx.nearestAtOrBefore(40).value(), qint64(1000));
      QCOMPARE(idx.nearestAtOrBefore(119).value(), qint64(2000));
      QCOMPARE(idx.nearestAtOrBefore(120).value(), qint64(3000));
      QCOMPARE(idx.nearestAtOrBefore(99999).value(), qint64(3000));
  }

  void TestFrameIndex::beforeFirstReturnsNullopt() {
      FrameIndex idx;
      idx.append(40, 1000);
      QVERIFY(!idx.nearestAtOrBefore(0).has_value());
      QVERIFY(!idx.nearestAtOrBefore(39).has_value());
  }

  void TestFrameIndex::nonMonotonicAppendIsIgnored() {
      FrameIndex idx;
      idx.append(100, 5000);
      idx.append(80, 4000);   // older PTS than tail -> ignored
      idx.append(100, 6000);  // duplicate PTS -> ignored (first wins)
      QCOMPARE(idx.size(), 1);
      QCOMPARE(idx.nearestAtOrBefore(200).value(), qint64(5000));
  }

  void TestFrameIndex::clearResets() {
      FrameIndex idx;
      idx.append(10, 100);
      idx.append(20, 200);
      idx.clear();
      QCOMPARE(idx.size(), 0);
      QVERIFY(!idx.nearestAtOrBefore(20).has_value());
  }

  QTEST_MAIN(TestFrameIndex)
  #include "tst_frameindex.moc"
  ```

- [ ] **Step 2: Register the target and run-it-fails.**
  Add to `tests/CMakeLists.txt` inside the `olr_test_playback` source list (anchor on the `playback/trackbuffer.cpp` source-path string inside the `qt_add_library(olr_test_playback STATIC ...)` block):
  ```cmake
      ${CMAKE_SOURCE_DIR}/playback/frameindex.cpp
  ```
  Add to `tests/unit/CMakeLists.txt` next to the other playback tests (anchor on an existing `olr_add_unit_test(... olr_test_playback)` line):
  ```cmake
  olr_add_unit_test(tst_frameindex olr_test_playback)
  ```
  Run (expect compile failure: no `playback/frameindex.h`):
  ```
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
  cmake --build build --target tst_frameindex
  ```
  Expected: fatal error `'playback/frameindex.h' file not found`.

- [ ] **Step 3: Minimal impl — header.**
  Create `playback/frameindex.h`:
  ```cpp
  #ifndef FRAMEINDEX_H
  #define FRAMEINDEX_H

  #include <QtGlobal>
  #include <optional>
  #include <vector>

  // PTS(ms) -> byte-offset map, appended incrementally as packets are read.
  // Recordings are ALL-INTRA, so any indexed offset is a valid decode start.
  class FrameIndex {
  public:
      void append(qint64 ptsMs, qint64 byteOffset);
      std::optional<qint64> nearestAtOrBefore(qint64 ptsMs) const;
      int size() const { return static_cast<int>(m_entries.size()); }
      void clear() { m_entries.clear(); }

  private:
      struct Entry {
          qint64 ptsMs;
          qint64 byteOffset;
      };
      std::vector<Entry> m_entries; // strictly increasing ptsMs
  };

  #endif // FRAMEINDEX_H
  ```

- [ ] **Step 4: Minimal impl — cpp.**
  Create `playback/frameindex.cpp`:
  ```cpp
  #include "playback/frameindex.h"

  #include <algorithm>

  void FrameIndex::append(qint64 ptsMs, qint64 byteOffset)
  {
      // Keep ptsMs strictly increasing; ignore out-of-order or duplicate PTS.
      if (!m_entries.empty() && ptsMs <= m_entries.back().ptsMs)
      {
          return;
      }
      m_entries.push_back(Entry{ptsMs, byteOffset});
  }

  std::optional<qint64> FrameIndex::nearestAtOrBefore(qint64 ptsMs) const
  {
      if (m_entries.empty())
      {
          return std::nullopt;
      }
      // First entry with ptsMs > target; the one before it is the answer.
      auto it = std::upper_bound(
          m_entries.begin(), m_entries.end(), ptsMs,
          [](qint64 value, const Entry& e) { return value < e.ptsMs; });
      if (it == m_entries.begin())
      {
          return std::nullopt; // target is before the first indexed frame
      }
      --it;
      return it->byteOffset;
  }
  ```

- [ ] **Step 5: Run-it-passes.**
  ```
  cmake --build build --target tst_frameindex
  ctest --test-dir build -R tst_frameindex --output-on-failure
  ```
  Expected: `100% tests passed`.

- [ ] **Step 6: Format changed lines + commit.**
  ```
  xcrun clang-format -i playback/frameindex.h playback/frameindex.cpp tests/unit/tst_frameindex.cpp
  git add playback/frameindex.h playback/frameindex.cpp tests/unit/tst_frameindex.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
  git commit -m "Tier3: FrameIndex PTS->offset map with nearestAtOrBefore + unit tests"
  ```

---

## Task 2: FrameIndex — incremental growth semantics (pure logic)

**Files:**
- Modify `playback/frameindex.h` (add `newestPtsMs`)
- Modify `playback/frameindex.cpp`
- Test: `tests/unit/tst_frameindex.cpp` (extend)

**Interfaces:**
- Produces: `std::optional<qint64> FrameIndex::newestPtsMs() const;`
- Consumes: nothing.

- [ ] **Step 1: Write failing test for newestPtsMs + append-after-query.**
  Add slot `void newestTracksGrowth();` to the class declaration and implement:
  ```cpp
  void TestFrameIndex::newestTracksGrowth() {
      FrameIndex idx;
      QVERIFY(!idx.newestPtsMs().has_value());
      idx.append(0, 0);
      QCOMPARE(idx.newestPtsMs().value(), qint64(0));
      idx.append(40, 1000);
      QCOMPARE(idx.newestPtsMs().value(), qint64(40));
      // Simulate file growth: query, then append more, then query again.
      QCOMPARE(idx.nearestAtOrBefore(40).value(), qint64(1000));
      idx.append(80, 2000);
      QCOMPARE(idx.nearestAtOrBefore(80).value(), qint64(2000));
      QCOMPARE(idx.newestPtsMs().value(), qint64(80));
  }
  ```

- [ ] **Step 2: Run-it-fails.**
  ```
  cmake --build build --target tst_frameindex
  ```
  Expected: compile error `no member named 'newestPtsMs'`.

- [ ] **Step 3: Minimal impl.**
  Add to `playback/frameindex.h` public section after `clear()`:
  ```cpp
      std::optional<qint64> newestPtsMs() const;
  ```
  Add to `playback/frameindex.cpp`:
  ```cpp
  std::optional<qint64> FrameIndex::newestPtsMs() const
  {
      if (m_entries.empty())
      {
          return std::nullopt;
      }
      return m_entries.back().ptsMs;
  }
  ```

- [ ] **Step 4: Run-it-passes.**
  ```
  cmake --build build --target tst_frameindex
  ctest --test-dir build -R tst_frameindex --output-on-failure
  ```
  Expected: `100% tests passed`.

- [ ] **Step 5: Format + commit.**
  ```
  xcrun clang-format -i playback/frameindex.h playback/frameindex.cpp tests/unit/tst_frameindex.cpp
  git add playback/frameindex.h playback/frameindex.cpp tests/unit/tst_frameindex.cpp
  git commit -m "Tier3: FrameIndex::newestPtsMs for incremental-growth queries"
  ```

---

## Task 3: ReplayPlaylist — entries + markIn/markOut/recall (pure logic)

**Files:**
- Create `playback/replayplaylist.h`
- Create `playback/replayplaylist.cpp`
- Test: `tests/unit/tst_replayplaylist.cpp`
- Modify `tests/CMakeLists.txt` (add `replayplaylist.cpp` to `olr_test_playback`)
- Modify `tests/unit/CMakeLists.txt` (register `tst_replayplaylist`)

**Interfaces:**
- Produces:
  - `struct ReplayEntry { QString clipPath; qint64 inMs; qint64 outMs; double speed; };`
  - `int ReplayPlaylist::markIn(const QString& clipPath, qint64 inMs);` (creates a pending entry, returns its index)
  - `bool ReplayPlaylist::markOut(qint64 outMs);` (closes the pending entry; false if none/invalid)
  - `std::optional<ReplayEntry> ReplayPlaylist::recall(int index) const;`
  - `int ReplayPlaylist::count() const;`
  - `void ReplayPlaylist::setSpeed(int index, double speed);`
  - `void ReplayPlaylist::clear();`
- Consumes: nothing.

- [ ] **Step 1: Write failing test.**
  Create `tests/unit/tst_replayplaylist.cpp`:
  ```cpp
  #include <QtTest>
  #include "playback/replayplaylist.h"

  class TestReplayPlaylist : public QObject { Q_OBJECT
  private slots:
      void emptyHasNoEntries();
      void markInCreatesPendingEntry();
      void markOutClosesEntry();
      void markOutBeforeInIsRejected();
      void markOutWithoutInIsRejected();
      void recallOutOfRangeReturnsNullopt();
      void multipleOrderedEntries();
      void setSpeedClampsToPositive();
  };

  void TestReplayPlaylist::emptyHasNoEntries() {
      ReplayPlaylist p;
      QCOMPARE(p.count(), 0);
      QVERIFY(!p.recall(0).has_value());
  }

  void TestReplayPlaylist::markInCreatesPendingEntry() {
      ReplayPlaylist p;
      int idx = p.markIn("/clips/a.mkv", 1000);
      QCOMPARE(idx, 0);
      QCOMPARE(p.count(), 1);
      auto e = p.recall(0).value();
      QCOMPARE(e.clipPath, QString("/clips/a.mkv"));
      QCOMPARE(e.inMs, qint64(1000));
      QCOMPARE(e.outMs, qint64(-1)); // open
      QCOMPARE(e.speed, 1.0);
  }

  void TestReplayPlaylist::markOutClosesEntry() {
      ReplayPlaylist p;
      p.markIn("/clips/a.mkv", 1000);
      QVERIFY(p.markOut(3000));
      auto e = p.recall(0).value();
      QCOMPARE(e.outMs, qint64(3000));
  }

  void TestReplayPlaylist::markOutBeforeInIsRejected() {
      ReplayPlaylist p;
      p.markIn("/clips/a.mkv", 1000);
      QVERIFY(!p.markOut(500));
      QCOMPARE(p.recall(0).value().outMs, qint64(-1));
  }

  void TestReplayPlaylist::markOutWithoutInIsRejected() {
      ReplayPlaylist p;
      QVERIFY(!p.markOut(1000));
      QCOMPARE(p.count(), 0);
  }

  void TestReplayPlaylist::recallOutOfRangeReturnsNullopt() {
      ReplayPlaylist p;
      p.markIn("/clips/a.mkv", 0);
      QVERIFY(!p.recall(-1).has_value());
      QVERIFY(!p.recall(1).has_value());
  }

  void TestReplayPlaylist::multipleOrderedEntries() {
      ReplayPlaylist p;
      p.markIn("/clips/a.mkv", 1000); p.markOut(2000);
      int idx2 = p.markIn("/clips/b.mkv", 5000); p.markOut(6000);
      QCOMPARE(p.count(), 2);
      QCOMPARE(idx2, 1);
      QCOMPARE(p.recall(0).value().clipPath, QString("/clips/a.mkv"));
      QCOMPARE(p.recall(1).value().inMs, qint64(5000));
  }

  void TestReplayPlaylist::setSpeedClampsToPositive() {
      ReplayPlaylist p;
      p.markIn("/clips/a.mkv", 0); p.markOut(1000);
      p.setSpeed(0, 0.5);
      QCOMPARE(p.recall(0).value().speed, 0.5);
      p.setSpeed(0, -2.0);    // clamped to a small positive epsilon
      QVERIFY(p.recall(0).value().speed > 0.0);
  }

  QTEST_MAIN(TestReplayPlaylist)
  #include "tst_replayplaylist.moc"
  ```

- [ ] **Step 2: Register + run-it-fails.**
  Add to `tests/CMakeLists.txt` `olr_test_playback` sources:
  ```cmake
      ${CMAKE_SOURCE_DIR}/playback/replayplaylist.cpp
  ```
  Add to `tests/unit/CMakeLists.txt`:
  ```cmake
  olr_add_unit_test(tst_replayplaylist olr_test_playback)
  ```
  ```
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
  cmake --build build --target tst_replayplaylist
  ```
  Expected: `'playback/replayplaylist.h' file not found`.

- [ ] **Step 3: Minimal impl — header.**
  Create `playback/replayplaylist.h`:
  ```cpp
  #ifndef REPLAYPLAYLIST_H
  #define REPLAYPLAYLIST_H

  #include <QString>
  #include <QVector>
  #include <optional>

  struct ReplayEntry {
      QString clipPath;
      qint64 inMs = 0;
      qint64 outMs = -1; // -1 = open / not yet marked out
      double speed = 1.0;
  };

  // Ordered EVS-style cue list. Pure model; no I/O, no threads.
  class ReplayPlaylist {
  public:
      int markIn(const QString& clipPath, qint64 inMs);
      bool markOut(qint64 outMs);
      std::optional<ReplayEntry> recall(int index) const;
      void setSpeed(int index, double speed);
      int count() const { return m_entries.size(); }
      void clear() { m_entries.clear(); }

  private:
      QVector<ReplayEntry> m_entries;
      static constexpr double kMinSpeed = 0.01;
  };

  #endif // REPLAYPLAYLIST_H
  ```

- [ ] **Step 4: Minimal impl — cpp.**
  Create `playback/replayplaylist.cpp`:
  ```cpp
  #include "playback/replayplaylist.h"

  int ReplayPlaylist::markIn(const QString& clipPath, qint64 inMs)
  {
      ReplayEntry e;
      e.clipPath = clipPath;
      e.inMs = inMs;
      e.outMs = -1;
      e.speed = 1.0;
      m_entries.append(e);
      return m_entries.size() - 1;
  }

  bool ReplayPlaylist::markOut(qint64 outMs)
  {
      if (m_entries.isEmpty())
      {
          return false;
      }
      ReplayEntry& last = m_entries.last();
      if (last.outMs != -1 || outMs < last.inMs)
      {
          return false;
      }
      last.outMs = outMs;
      return true;
  }

  std::optional<ReplayEntry> ReplayPlaylist::recall(int index) const
  {
      if (index < 0 || index >= m_entries.size())
      {
          return std::nullopt;
      }
      return m_entries.at(index);
  }

  void ReplayPlaylist::setSpeed(int index, double speed)
  {
      if (index < 0 || index >= m_entries.size())
      {
          return;
      }
      m_entries[index].speed = (speed < kMinSpeed) ? kMinSpeed : speed;
  }
  ```

- [ ] **Step 5: Run-it-passes.**
  ```
  cmake --build build --target tst_replayplaylist
  ctest --test-dir build -R tst_replayplaylist --output-on-failure
  ```
  Expected: `100% tests passed`.

- [ ] **Step 6: Format + commit.**
  ```
  xcrun clang-format -i playback/replayplaylist.h playback/replayplaylist.cpp tests/unit/tst_replayplaylist.cpp
  git add playback/replayplaylist.h playback/replayplaylist.cpp tests/unit/tst_replayplaylist.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
  git commit -m "Tier3: ReplayPlaylist cue list (markIn/markOut/recall/setSpeed) + unit tests"
  ```

---

## Task 4: ReplayPlaylist — JSON round-trip (pure logic, mirrors SettingsManager)

**Files:**
- Modify `playback/replayplaylist.h` (add `toJson`/`fromJson`)
- Modify `playback/replayplaylist.cpp`
- Test: `tests/unit/tst_replayplaylist.cpp` (extend)

**Interfaces:**
- Produces:
  - `QJsonObject ReplayPlaylist::toJson() const;`
  - `bool ReplayPlaylist::fromJson(const QJsonObject& obj);`
- Consumes: nothing.

- [ ] **Step 1: Write failing test.**
  Add `void jsonRoundTripPreservesEntries();` and `void fromJsonRejectsMalformed();` slots:
  ```cpp
  void TestReplayPlaylist::jsonRoundTripPreservesEntries() {
      ReplayPlaylist a;
      a.markIn("/clips/a.mkv", 1000); a.markOut(2000); a.setSpeed(0, 0.5);
      a.markIn("/clips/b.mkv", 5000); a.markOut(6000);
      QJsonObject json = a.toJson();

      ReplayPlaylist b;
      QVERIFY(b.fromJson(json));
      QCOMPARE(b.count(), 2);
      QCOMPARE(b.recall(0).value().clipPath, QString("/clips/a.mkv"));
      QCOMPARE(b.recall(0).value().inMs, qint64(1000));
      QCOMPARE(b.recall(0).value().outMs, qint64(2000));
      QCOMPARE(b.recall(0).value().speed, 0.5);
      QCOMPARE(b.recall(1).value().inMs, qint64(5000));
  }

  void TestReplayPlaylist::fromJsonRejectsMalformed() {
      ReplayPlaylist b;
      QJsonObject bad;            // no "entries" array
      QVERIFY(!b.fromJson(bad));
      QCOMPARE(b.count(), 0);
  }
  ```
  Add `#include <QJsonObject>` and `#include <QJsonArray>` near the top of the test file.

- [ ] **Step 2: Run-it-fails.**
  ```
  cmake --build build --target tst_replayplaylist
  ```
  Expected: compile error `no member named 'toJson'`.

- [ ] **Step 3: Minimal impl.**
  Add includes and declarations to `playback/replayplaylist.h`:
  ```cpp
  #include <QJsonObject>
  ```
  Inside the public section after `clear()`:
  ```cpp
      QJsonObject toJson() const;
      bool fromJson(const QJsonObject& obj);
  ```
  Add to `playback/replayplaylist.cpp`:
  ```cpp
  #include <QJsonArray>

  QJsonObject ReplayPlaylist::toJson() const
  {
      QJsonArray arr;
      for (const ReplayEntry& e : m_entries)
      {
          QJsonObject o;
          o["clipPath"] = e.clipPath;
          o["inMs"] = static_cast<double>(e.inMs);
          o["outMs"] = static_cast<double>(e.outMs);
          o["speed"] = e.speed;
          arr.append(o);
      }
      QJsonObject root;
      root["entries"] = arr;
      return root;
  }

  bool ReplayPlaylist::fromJson(const QJsonObject& obj)
  {
      if (!obj.contains("entries") || !obj["entries"].isArray())
      {
          return false;
      }
      QVector<ReplayEntry> parsed;
      const QJsonArray arr = obj["entries"].toArray();
      for (const QJsonValue& v : arr)
      {
          const QJsonObject o = v.toObject();
          ReplayEntry e;
          e.clipPath = o.value("clipPath").toString();
          e.inMs = static_cast<qint64>(o.value("inMs").toDouble(0.0));
          e.outMs = static_cast<qint64>(o.value("outMs").toDouble(-1.0));
          e.speed = o.value("speed").toDouble(1.0);
          parsed.append(e);
      }
      m_entries = parsed;
      return true;
  }
  ```

- [ ] **Step 4: Run-it-passes.**
  ```
  cmake --build build --target tst_replayplaylist
  ctest --test-dir build -R tst_replayplaylist --output-on-failure
  ```
  Expected: `100% tests passed`.

- [ ] **Step 5: Format + commit.**
  ```
  xcrun clang-format -i playback/replayplaylist.h playback/replayplaylist.cpp tests/unit/tst_replayplaylist.cpp
  git add playback/replayplaylist.h playback/replayplaylist.cpp tests/unit/tst_replayplaylist.cpp
  git commit -m "Tier3: ReplayPlaylist JSON round-trip (mirrors SettingsManager pattern)"
  ```

---

## Task 5: CutSchedule — output-frame mapping math (pure logic)

This isolates the atomic-cut boundary math (which output frame a wall/playhead maps to) so it is unit-testable without the worker or ffmpeg. Task 10 consumes it.

**Files:**
- Create `playback/cutschedule.h`
- Create `playback/cutschedule.cpp`
- Test: `tests/unit/tst_cutschedule.cpp`
- Modify `tests/CMakeLists.txt` (add `cutschedule.cpp` to `olr_test_playback`)
- Modify `tests/unit/CMakeLists.txt` (register `tst_cutschedule`)

**Interfaces:**
- Produces:
  - `struct CutSchedule { static qint64 outputFrameForCut(qint64 nextOutputFrameIndex, int leadFrames); };`
  - `static qint64 CutSchedule::framesForLeadMs(qint64 leadMs, double fps);`
  - `static bool CutSchedule::shouldFireAt(qint64 nextOutputFrameIndex, qint64 scheduledFrame);`
- Consumes: `OutputDispatcher::nextOutputFrameIndex()` (caller-supplied value).

- [ ] **Step 1: Write failing test.**
  Create `tests/unit/tst_cutschedule.cpp`:
  ```cpp
  #include <QtTest>
  #include "playback/cutschedule.h"

  class TestCutSchedule : public QObject { Q_OBJECT
  private slots:
      void leadFramesAddedToNextIndex();
      void zeroLeadSchedulesNextFrame();
      void framesForLeadMsRoundsUp();
      void shouldFireExactlyAtScheduledFrame();
      void shouldFireWhenTickOvershoots();
      void shouldNotFireBeforeScheduledFrame();
  };

  void TestCutSchedule::leadFramesAddedToNextIndex() {
      QCOMPARE(CutSchedule::outputFrameForCut(100, 3), qint64(103));
  }

  void TestCutSchedule::zeroLeadSchedulesNextFrame() {
      // Lead 0 must still land on a frame the dispatcher has not yet emitted.
      QCOMPARE(CutSchedule::outputFrameForCut(100, 0), qint64(100));
  }

  void TestCutSchedule::framesForLeadMsRoundsUp() {
      // 25 fps -> 40 ms/frame. 50 ms -> 2 frames (ceil).
      QCOMPARE(CutSchedule::framesForLeadMs(50, 25.0), qint64(2));
      // 41 ms -> ceil(1.025) = 2.
      QCOMPARE(CutSchedule::framesForLeadMs(41, 25.0), qint64(2));
      // 40 ms -> exactly 1.
      QCOMPARE(CutSchedule::framesForLeadMs(40, 25.0), qint64(1));
      // 0 ms -> 0 frames.
      QCOMPARE(CutSchedule::framesForLeadMs(0, 25.0), qint64(0));
  }

  void TestCutSchedule::shouldFireExactlyAtScheduledFrame() {
      QVERIFY(CutSchedule::shouldFireAt(103, 103));
  }

  void TestCutSchedule::shouldFireWhenTickOvershoots() {
      // Dispatcher may skip an index under load; fire on >= to avoid a missed cut.
      QVERIFY(CutSchedule::shouldFireAt(105, 103));
  }

  void TestCutSchedule::shouldNotFireBeforeScheduledFrame() {
      QVERIFY(!CutSchedule::shouldFireAt(102, 103));
  }

  QTEST_MAIN(TestCutSchedule)
  #include "tst_cutschedule.moc"
  ```

- [ ] **Step 2: Register + run-it-fails.**
  Add to `tests/CMakeLists.txt` `olr_test_playback` sources:
  ```cmake
      ${CMAKE_SOURCE_DIR}/playback/cutschedule.cpp
  ```
  Add to `tests/unit/CMakeLists.txt`:
  ```cmake
  olr_add_unit_test(tst_cutschedule olr_test_playback)
  ```
  ```
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=~/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
  cmake --build build --target tst_cutschedule
  ```
  Expected: `'playback/cutschedule.h' file not found`.

- [ ] **Step 3: Minimal impl — header.**
  Create `playback/cutschedule.h`:
  ```cpp
  #ifndef CUTSCHEDULE_H
  #define CUTSCHEDULE_H

  #include <QtGlobal>

  // Pure boundary math for the atomic cut: maps a lead to the output frame index
  // at which the staging cache should be promoted to active.
  struct CutSchedule {
      // The output frame index that the dispatcher has not yet emitted, offset by lead.
      static qint64 outputFrameForCut(qint64 nextOutputFrameIndex, int leadFrames);

      // Convert a lead in wall-clock ms to whole output frames (ceil, fps-relative).
      static qint64 framesForLeadMs(qint64 leadMs, double fps);

      // Fire when the dispatcher's next index has reached (or overshot) the schedule.
      static bool shouldFireAt(qint64 nextOutputFrameIndex, qint64 scheduledFrame);
  };

  #endif // CUTSCHEDULE_H
  ```

- [ ] **Step 4: Minimal impl — cpp.**
  Create `playback/cutschedule.cpp`:
  ```cpp
  #include "playback/cutschedule.h"

  #include <cmath>

  qint64 CutSchedule::outputFrameForCut(qint64 nextOutputFrameIndex, int leadFrames)
  {
      if (leadFrames < 0)
      {
          leadFrames = 0;
      }
      return nextOutputFrameIndex + leadFrames;
  }

  qint64 CutSchedule::framesForLeadMs(qint64 leadMs, double fps)
  {
      if (leadMs <= 0 || fps <= 0.0)
      {
          return 0;
      }
      const double frames = (static_cast<double>(leadMs) * fps) / 1000.0;
      return static_cast<qint64>(std::ceil(frames));
  }

  bool CutSchedule::shouldFireAt(qint64 nextOutputFrameIndex, qint64 scheduledFrame)
  {
      return nextOutputFrameIndex >= scheduledFrame;
  }
  ```

- [ ] **Step 5: Run-it-passes.**
  ```
  cmake --build build --target tst_cutschedule
  ctest --test-dir build -R tst_cutschedule --output-on-failure
  ```
  Expected: `100% tests passed`.

- [ ] **Step 6: Format + commit.**
  ```
  xcrun clang-format -i playback/cutschedule.h playback/cutschedule.cpp tests/unit/tst_cutschedule.cpp
  git add playback/cutschedule.h playback/cutschedule.cpp tests/unit/tst_cutschedule.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
  git commit -m "Tier3: CutSchedule output-frame mapping math + unit tests"
  ```

---

## Task 6: CutSchedule — playhead-origin remap at the cut (pure logic)

When the cut fires, the playhead origin must be re-based so `transport->currentPos()` reads the target. This is the pure arithmetic of that remap.

**Files:**
- Modify `playback/cutschedule.h` (add `playheadAfterCut`)
- Modify `playback/cutschedule.cpp`
- Test: `tests/unit/tst_cutschedule.cpp` (extend)

**Interfaces:**
- Produces: `static qint64 CutSchedule::playheadAfterCut(qint64 targetMs, qint64 firedFrameIndex, qint64 scheduledFrameIndex, double fps);`
- Consumes: nothing.

- [ ] **Step 1: Write failing test.**
  Add slot `void playheadAccountsForOvershoot();`:
  ```cpp
  void TestCutSchedule::playheadAccountsForOvershoot() {
      // Fired exactly on schedule -> playhead == targetMs.
      QCOMPARE(CutSchedule::playheadAfterCut(2000, 103, 103, 25.0), qint64(2000));
      // Fired one frame late at 25fps (40ms/frame) -> targetMs + 40.
      QCOMPARE(CutSchedule::playheadAfterCut(2000, 104, 103, 25.0), qint64(2040));
      // Negative/zero fps guard -> targetMs unchanged.
      QCOMPARE(CutSchedule::playheadAfterCut(2000, 104, 103, 0.0), qint64(2000));
  }
  ```
  Add the slot declaration in the class.

- [ ] **Step 2: Run-it-fails.**
  ```
  cmake --build build --target tst_cutschedule
  ```
  Expected: compile error `no member named 'playheadAfterCut'`.

- [ ] **Step 3: Minimal impl.**
  Add to `playback/cutschedule.h`:
  ```cpp
      // Playhead position once the cut fires, accounting for any dispatcher overshoot.
      static qint64 playheadAfterCut(qint64 targetMs, qint64 firedFrameIndex,
                                     qint64 scheduledFrameIndex, double fps);
  ```
  Add to `playback/cutschedule.cpp`:
  ```cpp
  qint64 CutSchedule::playheadAfterCut(qint64 targetMs, qint64 firedFrameIndex,
                                       qint64 scheduledFrameIndex, double fps)
  {
      if (fps <= 0.0 || firedFrameIndex <= scheduledFrameIndex)
      {
          return targetMs;
      }
      const qint64 overshootFrames = firedFrameIndex - scheduledFrameIndex;
      const double msPerFrame = 1000.0 / fps;
      return targetMs + static_cast<qint64>(overshootFrames * msPerFrame);
  }
  ```

- [ ] **Step 4: Run-it-passes.**
  ```
  cmake --build build --target tst_cutschedule
  ctest --test-dir build -R tst_cutschedule --output-on-failure
  ```
  Expected: `100% tests passed`.

- [ ] **Step 5: Format + commit.**
  ```
  xcrun clang-format -i playback/cutschedule.h playback/cutschedule.cpp tests/unit/tst_cutschedule.cpp
  git add playback/cutschedule.h playback/cutschedule.cpp tests/unit/tst_cutschedule.cpp
  git commit -m "Tier3: CutSchedule::playheadAfterCut overshoot remap + unit tests"
  ```

---

## Task 7: Wire FrameIndex into the worker read loop + repositionExact (integration)

**Design.** Today `repositionTo` (playbackworker.cpp:545-612) does `av_seek_frame BACKWARD` to a coarse anchor and decodes forward until `newestPtsMin()>=target+frameDurMs`. Because the recordings are ALL-INTRA, every packet offset is a valid decode start, so an exact `avio_seek(byteOffset)` + decode-one-frame is correct and avoids the forward-fill churn. We add a `FrameIndex` populated as packets are read, and a `repositionExact(target,dir)` fast path that uses it. This path must NOT regress the existing E2E gates — it is additive (falls back to the existing `repositionTo` when the index has no entry at-or-before the target). The frame index is worker-thread-only state (populated and read only inside `run()` / reposition), so no new mutex is required.

**Files:**
- Modify `playback/playbackworker.h` (add `FrameIndex m_frameIndex;` member; declare `bool repositionExact(int64_t target,int dir,AVPacket* pkt,AVFrame* vf,AVFrame* af);`)
- Modify `playback/playbackworker.cpp`:
  - In `decodePacketIntoBank` (around the read at cpp:454-512), record `m_frameIndex.append(ptsMs, pkt->pos)` for the primary video stream when `pkt->pos >= 0`.
  - Add `repositionExact` near `repositionTo` (cpp:545).
  - Call `repositionExact` first inside `repositionTo` before the `reuseAt` fast-path; on success skip the coarse path.
  - In `clearDecoderBuffers` (Tier 1 rename) do NOT clear `m_frameIndex` (the index survives buffer clears).
- Test: exercised via `tests/e2e/play_harness` (seekplay, stepscrub). No unit test (needs real ffmpeg).

**Interfaces:**
- Consumes: `FrameIndex::nearestAtOrBefore`, `FrameIndex::append`; `avio_seek(m_fmtCtx->pb, offset, SEEK_SET)`; existing `convertToMediaVideoFrame`, `deliverDueFrames`.
- Produces: `PlaybackWorker::repositionExact(...)` returning true when it serviced the seek.

- [ ] **Step 1: Add the member + append in the read loop.**
  In `playback/playbackworker.h`, add `#include "playback/frameindex.h"` and a private member `FrameIndex m_frameIndex;`.
  In `decodePacketIntoBank` (playbackworker.cpp), immediately after the packet is read for the primary video stream and `convertToMediaVideoFrame` produces `vf` with a valid `ptsMs`, add (worker thread only, no lock):
  ```cpp
      if (pkt->stream_index == primaryVideoStreamIndex && pkt->pos >= 0)
      {
          m_frameIndex.append(vf.ptsMs, static_cast<qint64>(pkt->pos));
      }
  ```
  (Resolve `primaryVideoStreamIndex` from the existing `DecoderTrack::streamIndex` for `feedIndex==selected`.)

- [ ] **Step 2: Implement repositionExact.**
  Add to `playback/playbackworker.cpp`:
  ```cpp
  bool PlaybackWorker::repositionExact(int64_t target, int dir, AVPacket* pkt,
                                       AVFrame* vf, AVFrame* af)
  {
      const auto offset = m_frameIndex.nearestAtOrBefore(target);
      if (!offset.has_value())
      {
          return false; // not indexed yet -> caller falls back to coarse path
      }
      clearDecoderBuffers(); // Tier 1: clears tracks, leaves m_outputCache intact
      if (avio_seek(m_fmtCtx->pb, offset.value(), SEEK_SET) < 0)
      {
          return false;
      }
      avformat_flush(m_fmtCtx);
      const int trackCount = m_decoderBank.size();
      // ALL-INTRA: decode forward from the indexed offset until we cover target.
      while (newestPtsMin() < target + frameDurMs())
      {
          if (av_read_frame(m_fmtCtx, pkt) < 0)
          {
              break;
          }
          // Mirror the forward-fill callsite (playbackworker.cpp:938-940). We
          // decode forward from the exact indexed offset toward `target`, so
          // pass P=target/dir=+1, no decimation, audio off (re-primed by the
          // normal forward release afterwards), no dedupTail.
          decodePacketIntoBank(pkt, vf, af, target, /*dir*/ 1, trackCount,
                               /*decimate*/ false, /*step*/ 1,
                               /*audioOn*/ false, /*dedupTail*/ false);
          av_packet_unref(pkt);
      }
      resetDedup();
      deliverDueFrames(target, dir);
      return true;
  }
  ```

- [ ] **Step 3: Call it from repositionTo.**
  At the top of `repositionTo` (playbackworker.cpp:545), before the `reuseAt` fast-path at :550, add:
  ```cpp
      if (repositionExact(target, dir, pkt, vf, af))
      {
          m_counters.reuseSeek++; // counts as a cheap reuse, not a coarse reposition
          return;
      }
  ```
  Leave the existing `reuseAt`/coarse path untouched as the fallback.

- [ ] **Step 4: Build + run the affected E2E gates (must not regress).**
  ```
  cmake --build build --target play_harness record_harness
  ctest --test-dir build -R "e2e_play_seekplay|e2e_play_stepscrub|e2e_play_storm" --output-on-failure
  ```
  Expected: seekplay `reposition<=2`; stepscrub `reposition<=10 && reuseSeek>=1`; storm (play1x scenario) `reposition==0`. (repositionExact should drive seekplay reposition toward 0.)

- [ ] **Step 5: Format + commit.**
  ```
  xcrun clang-format -i playback/playbackworker.h playback/playbackworker.cpp
  git add playback/playbackworker.h playback/playbackworker.cpp
  git commit -m "Tier3: record FrameIndex in read loop + repositionExact via avio_seek (all-intra)"
  ```

---

## Task 8: Wire markIn/markOut/recall into UIManager + Main.qml (integration)

**Design.** `UIManager` owns the `ReplayPlaylist`. `recallEntry(index)` resolves the entry and arms the cut on the worker (`armNextCut(entry.inMs)` — implemented in Task 9) instead of the churny `seekPlayback`. Mark-in/mark-out capture the current `m_transport->currentPos()`. The clip path for mark-in is the currently-open clip, which UIManager already obtains from `m_replayManager->getVideoPath()` (the exact call it passes to `m_playbackWorker->openFile(...)` at uimanager.cpp:1641,1663). There is NO `currentClipPath()` accessor on `UIManager`; reuse `m_replayManager->getVideoPath()`. These are UI-thread calls that must not block the seek path; `armNextCut` only sets an atomic target (Task 9), so no blocking call is introduced.

**Files:**
- Modify `uimanager.h` (add `#include "playback/replayplaylist.h"`, `ReplayPlaylist m_playlist;`, `Q_INVOKABLE void markIn(); Q_INVOKABLE void markOut(); Q_INVOKABLE void recallEntry(int index); Q_INVOKABLE int playlistCount() const;`)
- Modify `uimanager.cpp` (implement the four; current clip path + `m_transport->currentPos()`).
- Modify `Main.qml` (Mark-In / Mark-Out / Recall buttons calling `uiManagerRef`).
- Test: manual + the `armedcut` E2E (Task 12). No unit test (UIManager needs the Qt UI/runtime).

**Interfaces:**
- Consumes: `ReplayPlaylist::markIn/markOut/recall`, `m_transport->currentPos()`, `m_playbackWorker->armNextCut(int64_t)`, `m_replayManager->getVideoPath()` (the existing current-clip-path accessor, QString; uimanager.cpp:1641,1663).
- Produces: `UIManager::markIn/markOut/recallEntry/playlistCount`.

- [ ] **Step 1: Add declarations.**
  In `uimanager.h` add the include and members/methods listed above.

- [ ] **Step 2: Implement in uimanager.cpp.**
  ```cpp
  void UIManager::markIn()
  {
      // Reuse the same current-clip accessor UIManager already calls when it
      // hands the path to the worker (uimanager.cpp:1641,1663). No new
      // currentClipPath() accessor is added — m_replayManager owns the path.
      const QString clip = m_replayManager->getVideoPath();
      m_playlist.markIn(clip, m_transport->currentPos());
  }

  void UIManager::markOut()
  {
      m_playlist.markOut(m_transport->currentPos());
  }

  void UIManager::recallEntry(int index)
  {
      const auto entry = m_playlist.recall(index);
      if (!entry.has_value())
      {
          return;
      }
      if (m_playbackWorker)
      {
          m_playbackWorker->armNextCut(entry->inMs); // Task 9: atomic, non-blocking
      }
  }

  int UIManager::playlistCount() const
  {
      return m_playlist.count();
  }
  ```
  (If the current clip differs from `entry->clipPath`, scope a follow-up to re-open; for Tier 3 v1 the playlist is single-clip — see Global note in Task 9.)

- [ ] **Step 3: Add minimal Main.qml hooks.**
  Near the scrubBar (Main.qml:942-955) add a Row of three Buttons:
  ```qml
  Row {
      spacing: 8
      Button { text: "Mark In";  onClicked: uiManagerRef.markIn() }
      Button { text: "Mark Out"; onClicked: uiManagerRef.markOut() }
      Button { text: "Recall 0"; onClicked: uiManagerRef.recallEntry(0) }
  }
  ```

- [ ] **Step 4: Build the app + run unit suite (no regressions).**
  ```
  cmake --build build
  ctest --test-dir build -L unit --output-on-failure
  ```
  Expected: builds; all unit tests pass.

- [ ] **Step 5: Format + commit.**
  ```
  xcrun clang-format -i uimanager.h uimanager.cpp
  git add uimanager.h uimanager.cpp Main.qml
  git commit -m "Tier3: UIManager markIn/markOut/recallEntry wired to ReplayPlaylist + Main.qml hooks"
  ```

---

## Task 9: Pre-roll decoder bank — armNextCut + staging fill loop (Design + Integration — E2E-gated, not unit-TDD)

This task is ffmpeg/thread-heavy and is NOT unit-tested. It is exercised end-to-end by the `armedcut` scenario (Task 12). Implement with frequent commits after each sub-step builds.

**Design.**
- **Second AVFormatContext.** `PlaybackWorker` opens a *second*, independent `AVFormatContext` (`m_prerollFmtCtx`) on the same clip, with its own `DecoderTrack` bank (`m_prerollBank`) and its own staging cache `std::unique_ptr<OutputFrameCache> m_stagingCache` sized identically to `m_outputCache` (same feedCount/placeholder dims). The primary bank keeps decoding and playing untouched while the pre-roll bank fills the staging cache. This avoids contending the primary `AVFormatContext` read position.
- **Armed target.** `armNextCut(int64_t targetMs)` sets `std::atomic<int64_t> m_armedTargetMs` and `std::atomic<bool> m_cutArmed{true}`. It must not block (UI-thread-safe; only atomic stores). The worker's `run()` loop, between deliver and wait (around playbackworker.cpp:1028-1094), checks `m_cutArmed` and drives the staging fill.
- **Staging fill loop.** When armed and the staging cache does not yet cover `[target, target+kStagingSpanMs]`: use `m_frameIndex.nearestAtOrBefore(target)` (Task 7) → `avio_seek` on `m_prerollFmtCtx->pb` → decode forward, inserting into `m_stagingCache` (via the same conversion path as `decodePacketIntoBank`, but writing to staging) until `m_stagingCache` covers the span. Fill is incremental: a bounded number of packets per `run()` iteration (`kPrerollPacketsPerTick`, e.g. 8) so the primary tick is never starved. Hold no lock while decoding; only lock `m_bufferMutex` for the brief insert into `m_stagingCache` if the staging cache can be read by the output thread (it must NOT be readable until promotion — keep staging worker-private, so no lock needed during fill).
- **Warm-decoder pool for next 1–2 targets.** Maintain a small ring `m_warmTargets` (size 2) of recently/next-armed targets. After the current staging fill completes, opportunistically pre-roll the next warm target into a secondary staging slot so a back-to-back recall is already warm. v1: keep a single `m_stagingCache` plus one `m_warmStagingCache`; promote `m_warmStagingCache` → `m_stagingCache` when `armNextCut` matches the warmed target. Keep this behind a `kWarmPoolEnabled` constant so it can be disabled if it complicates the first E2E pass.
- **Single-clip assumption (v1).** Tier 3 v1 arms cuts within the currently-open clip (same file → second AVFormatContext on the same path). Cross-clip recall (different `clipPath`) is explicitly out of scope for Tier 3 v1; `armNextCut` is ms-only. Note this in a code comment.

**Files:**
- Modify `playback/playbackworker.h`: add the members/atomics/constants below (note: `m_fmtCtx` is the EXISTING primary context member name, playbackworker.h:135; the pre-roll context mirrors it as `m_prerollFmtCtx`):
  ```cpp
  // --- Tier3 pre-roll bank (second independent decode of the same clip) ----
  AVFormatContext* m_prerollFmtCtx = nullptr;          // mirrors m_fmtCtx (h:135)
  QVector<DecoderTrack*> m_prerollBank;                // mirrors m_decoderBank (h:133)
  QVector<AudioDecoderTrack*> m_prerollAudioBank;      // mirrors m_audioDecoderBank (h:134)
  std::unique_ptr<OutputFrameCache> m_stagingCache;    // sized == m_outputCache
  std::unique_ptr<OutputFrameCache> m_warmStagingCache;
  std::atomic<int64_t> m_armedTargetMs{-1};
  std::atomic<bool> m_cutArmed{false};
  bool m_stagingCovers = false;                        // worker-thread-only
  static constexpr int kStagingSpanMs = 800;           // window staged ahead of target
  static constexpr int kPrerollPacketsPerTick = 8;     // bounded per run() iter
  static constexpr bool kWarmPoolEnabled = false;      // v1: disabled until first E2E pass
  void armNextCut(int64_t targetMs); // public
  void fillStaging();                // private
  ```
- Modify `playback/playbackworker.cpp`: open the second context in `openFile` (mirror the primary open/retry loop at playbackworker.cpp:639-756); implement `armNextCut` (atomic stores only); implement `fillStaging`; call `fillStaging()` from `run()` when `m_cutArmed`.

**Interfaces:**
- Consumes: `FrameIndex::nearestAtOrBefore`, `avio_seek`, `OutputFrameCache::insertVideoFrame`, `convertToMediaVideoFrame`.
- Produces: `PlaybackWorker::armNextCut(int64_t)`; staging cache covering the armed window.

- [ ] **Step 1: Add members + constants; open second AVFormatContext.**
  Add the members/atomics/constants from the Files block above.

  The primary open is NOT in `openFile` — `openFile` only stores `m_currentFilePath` and starts
  the thread; the real `avformat_alloc_context` → `avformat_open_input` →
  `avformat_find_stream_info` → per-stream `DecoderTrack`/`AudioDecoderTrack` build runs inside
  `run()`'s OPENING & INITIALIZATION retry loop (playbackworker.cpp:639-756). Mirror that loop
  into a private helper that targets the pre-roll members, called once right after the primary
  bank is confirmed non-empty (after the `if (!m_decoderBank.isEmpty()) break;` at :745 and the
  guard at :752-756):
  ```cpp
  // Mirrors the primary open/retry loop (playbackworker.cpp:639-756) but writes
  // into m_prerollFmtCtx / m_prerollBank / m_prerollAudioBank. Same codec setup
  // (thread_count=0, avcodec_open2), no provider wiring (pre-roll feeds staging,
  // not the live providers). Returns false on failure (pre-roll silently disabled).
  bool PlaybackWorker::openPrerollContext()
  {
      if (m_prerollFmtCtx) avformat_close_input(&m_prerollFmtCtx);
      AVFormatContext* ctx = avformat_alloc_context();
      if (!ctx) return false;
      ctx->interrupt_callback.callback = &PlaybackWorker::ffmpegInterruptCallback;
      ctx->interrupt_callback.opaque = this;
      if (avformat_open_input(&ctx, m_currentFilePath.toUtf8().constData(), nullptr, nullptr) < 0)
      {
          avformat_close_input(&ctx);
          return false;
      }
      m_prerollFmtCtx = ctx;
      if (avformat_find_stream_info(m_prerollFmtCtx, nullptr) < 0)
      {
          avformat_close_input(&m_prerollFmtCtx);
          return false;
      }
      // Build m_prerollBank / m_prerollAudioBank exactly as the primary loop does
      // at :667-743 (same codec_id lookup, alloc_context3, parameters_to_context,
      // thread_count=0, avcodec_open2), assigning streamIndex/feedIndex/viewIndex.
      // ... (mirror :667-743 verbatim, writing to the preroll banks) ...
      return !m_prerollBank.isEmpty();
  }
  ```
  Allocate `m_stagingCache`/`m_warmStagingCache` with the same constructor args as `m_outputCache`
  (mirror the `m_outputCache` allocation in `initializeOutputGraph`, playbackworker.h:122 / its
  `.cpp` body — same `(feedCount, width, height)`).

  **Acceptance:** unarmed E2E gates are byte-for-byte unchanged (the pre-roll context is allocated
  but `fillStaging` never runs until `armNextCut`); verified by Step 5. **Manual:** confirm two
  `avformat_open_input` calls on the clip in a debug log and no change to live playback.
  Build; commit `Tier3: open second AVFormatContext (preroll bank) + staging caches`.

- [ ] **Step 2: Implement armNextCut (atomic, non-blocking).**
  ```cpp
  void PlaybackWorker::armNextCut(int64_t targetMs)
  {
      m_armedTargetMs.store(targetMs < 0 ? 0 : targetMs);
      m_cutArmed.store(true);
  }
  ```
  Build; commit `Tier3: PlaybackWorker::armNextCut sets atomic target`.

- [ ] **Step 3: Implement fillStaging (bounded, worker-private).**
  Mirror the structure of the exact reseek + forward-fill: the `avio_seek`/`avformat_flush`
  pattern from `repositionExact` (Task 7 Step 2) but on `m_prerollFmtCtx`, and the bounded
  per-iteration decode of the forward-fill loop (playbackworker.cpp:910-945). Decode into a
  pre-roll-specific insert path (the worker-private staging cache), NOT the live `m_outputCache`.
  Skeleton (structural body; the per-packet insert mirrors how `decodePacketIntoBank` inserts
  into `m_outputCache` at :454-512, but writes `m_stagingCache`):
  ```cpp
  // Worker-thread-only. Bounded incremental pre-roll into m_stagingCache.
  // No lock: m_stagingCache is not readable by OutputRuntime until promotion (Task 10).
  void PlaybackWorker::fillStaging()
  {
      const int64_t target = m_armedTargetMs.load();
      if (target < 0 || m_stagingCovers) return;

      // First call after arm: seek the preroll context to the indexed offset.
      // (Detect "first call" with a member flag set in armNextCut, cleared here.)
      if (/* first fill for this target */ true)
      {
          const auto offset = m_frameIndex.nearestAtOrBefore(target); // Task 7
          if (offset.has_value())
          {
              avio_seek(m_prerollFmtCtx->pb, offset.value(), SEEK_SET);
              avformat_flush(m_prerollFmtCtx);
          }
      }

      // Bounded decode: at most kPrerollPacketsPerTick packets this tick so the
      // primary run() loop is never starved. Mirror the forward-fill loop body
      // (playbackworker.cpp:923-944) reading from m_prerollFmtCtx and inserting
      // into m_stagingCache (mirror decodePacketIntoBank's insert at :454-512).
      int packets = 0;
      AVPacket* pkt = av_packet_alloc();
      while (packets++ < kPrerollPacketsPerTick)
      {
          if (av_read_frame(m_prerollFmtCtx, pkt) < 0) break;
          // decode pkt through m_prerollBank codec ctxs -> convertToMediaVideoFrame
          // -> insert into m_stagingCache for the matching feedIndex.
          av_packet_unref(pkt);
          // m_stagingCache covers the target window?
          if (/* m_stagingCache newest >= target + kStagingSpanMs */ false)
          {
              m_stagingCovers = true;
              break;
          }
      }
      av_packet_free(&pkt);
  }
  ```
  **Acceptance:** the `armedcut` E2E (Task 12) reports `placeholderFrames==0 && reposition==0`
  for the armed jump — proving staging was full before promotion so no `repositionTo` fired.
  **Manual:** log staging coverage progress; confirm `m_stagingCovers` flips before the cut frame.
  Build; commit `Tier3: fillStaging incremental pre-roll into staging cache`.

- [ ] **Step 4: Call fillStaging from run().**
  In `run()` between the audio-release block (cpp:1028-1046) and the wait (cpp:1094), add:
  ```cpp
      if (m_cutArmed.load())
      {
          fillStaging();
      }
  ```
  Build; commit `Tier3: drive fillStaging from worker run loop when armed`.

- [ ] **Step 5: Verify no regression on existing E2E (pre-roll inactive when unarmed).**
  ```
  cmake --build build --target play_harness record_harness
  ctest --test-dir build -R "e2e_play_storm|e2e_play_seekplay|e2e_play_latency" --output-on-failure
  ```
  Expected: storm (play1x scenario) `reposition==0`; seekplay `reposition<=2`; latency `resyncCount==0`. (Unarmed runs must be byte-for-byte the old behavior.)

---

## Task 10: Atomic cut — scheduleCutAtFrame + generation token (Design + Integration — E2E-gated, not unit-TDD)

NOT unit-tested for the threading; the boundary math is already covered by `CutSchedule` (Tasks 5–6). E2E-gated by Task 12.

**Design.**
- **Schedule.** When `fillStaging` reports the staging cache covers the target, the worker calls `scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs)` where `outputFrameIndex = CutSchedule::outputFrameForCut(m_outputRuntime->dispatcherNextOutputFrameIndex(), CutSchedule::framesForLeadMs(kCutLeadMs, transport->fps()))`. Store `std::atomic<qint64> m_scheduledCutFrame` and `std::atomic<int64_t> m_scheduledCutTargetMs`.
- **Fire.** In `makeOutputSnapshot` (playbackworker.cpp:362-377, runs under `m_bufferMutex`) — or in a dedicated check the OutputRuntime calls once per tick — read `OutputDispatcher::nextOutputFrameIndex()` (outputdispatcher.h:82). When `CutSchedule::shouldFireAt(nextIdx, m_scheduledCutFrame)` is true and a cut is pending, perform the atomic promotion:
  1. Swap `m_stagingCache` ↔ `m_outputCache` (under `m_bufferMutex`; the Tier 2 published `shared_ptr<const OutputFrameCache>` makes this a single atomic pointer swap rather than a deep copy).
  2. Re-base the playhead: `m_transport->seek(CutSchedule::playheadAfterCut(target, nextIdx, m_scheduledCutFrame, fps))`.
  3. Bump the Tier 2 generation token `m_cacheGeneration` so the snapshot's committed playhead immediately reflects the new cache (no placeholder window).
  4. Clear `m_cutArmed`/`m_scheduledCutFrame`.
- **Combine with Tier 2 generation token.** Because the staging cache already covers the target before scheduling, the generation-token "committed playhead" does NOT lag after the swap — the cache is target-covering at the instant of promotion, so `placeholderFrames` stays 0 and `reposition` stays 0 (no `repositionTo` is ever invoked for an armed cut).
- **Thread safety.** The swap and token bump happen under `m_bufferMutex` (same lock `makeOutputSnapshot` already holds). `m_scheduledCutFrame`/`m_scheduledCutTargetMs`/`m_cutArmed` are atomics. No UI-thread blocking call is added.

**Files:**
- Modify `playback/playbackworker.h`: add `void scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs);`, atomics `m_scheduledCutFrame`, `m_scheduledCutTargetMs`, constant `kCutLeadMs`; a private `void maybeFireScheduledCut(qint64 dispatcherNextIndex);`.
- Modify `playback/playbackworker.cpp`: implement `scheduleCutAtFrame` (atomic stores); call it from `fillStaging` once `m_stagingCovers`; implement `maybeFireScheduledCut` (the swap/remap/token-bump under `m_bufferMutex`); invoke `maybeFireScheduledCut` from `makeOutputSnapshot` using the dispatcher's next index.
- Modify `playback/output/outputruntime.cpp`/`.h` only if needed to surface `dispatcher->nextOutputFrameIndex()` to the snapshot provider (a getter `OutputRuntime::dispatcherNextOutputFrameIndex()` that forwards `m_dispatcher->nextOutputFrameIndex()`).

**Interfaces:**
- Consumes: `CutSchedule::outputFrameForCut/framesForLeadMs/shouldFireAt/playheadAfterCut`, `OutputDispatcher::nextOutputFrameIndex()`, Tier 2 `m_cacheGeneration`, Tier 2 published `shared_ptr` cache swap, `m_transport->seek`, `m_transport->fps`.
- Produces: `PlaybackWorker::scheduleCutAtFrame`, `maybeFireScheduledCut`.

- [ ] **Step 1: Add scheduleCutAtFrame + atomics.**
  ```cpp
  void PlaybackWorker::scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs)
  {
      m_scheduledCutTargetMs.store(targetMs);
      m_scheduledCutFrame.store(outputFrameIndex);
  }
  ```
  Build; commit `Tier3: scheduleCutAtFrame stores atomic schedule`.

- [ ] **Step 2: Schedule from fillStaging when covered.**
  When `m_stagingCovers` first becomes true for the armed target, compute and call:
  ```cpp
      const qint64 lead = CutSchedule::framesForLeadMs(kCutLeadMs, m_transport->fps());
      const qint64 nextIdx = m_outputRuntime->dispatcherNextOutputFrameIndex();
      scheduleCutAtFrame(CutSchedule::outputFrameForCut(nextIdx, static_cast<int>(lead)),
                         m_armedTargetMs.load());
  ```
  Build; commit `Tier3: schedule cut once staging cache covers target`.

- [ ] **Step 3: Implement maybeFireScheduledCut (swap + remap + token bump).**
  Under `m_bufferMutex`: if a cut is scheduled and `CutSchedule::shouldFireAt(dispatcherNextIndex, m_scheduledCutFrame)`:
  ```cpp
      const int64_t target = m_scheduledCutTargetMs.load();
      const qint64 scheduled = m_scheduledCutFrame.load();
      std::swap(m_outputCache, m_stagingCache);          // Tier2: shared_ptr swap
      m_stagingCovers = false;
      const qint64 ph = CutSchedule::playheadAfterCut(
          target, dispatcherNextIndex, scheduled, m_transport->fps());
      m_transport->seek(ph);
      m_cacheGeneration.fetch_add(1);                     // Tier2 token
      m_cutArmed.store(false);
      m_scheduledCutFrame.store(-1);
  ```
  Mirror the swap against the Tier 2 published `shared_ptr<const OutputFrameCache>` machinery (see
  "Dependencies on Tier 2" below) so the promotion is a single pointer swap, not a deep copy.
  **Acceptance:** the `armedcut` E2E (Task 12) holds `placeholderFrames==0 && reposition==0` — the
  swap promotes an already-target-covering cache, so the generation-token committed playhead never
  lags and `repositionTo` is never invoked. **Manual:** single-step the dispatcher across the
  scheduled frame in a debug build; confirm exactly one swap fires and the playhead reads `target`.
  Build; commit `Tier3: maybeFireScheduledCut atomic staging->active promotion + token bump`.

- [ ] **Step 4: Invoke from makeOutputSnapshot.**
  In `makeOutputSnapshot` (already under `m_bufferMutex`), before copying state, call `maybeFireScheduledCut(m_outputRuntime->dispatcherNextOutputFrameIndex());`. Build; commit `Tier3: fire scheduled cut from makeOutputSnapshot`.

- [ ] **Step 5: Regression check.**
  ```
  cmake --build build --target play_harness record_harness
  ctest --test-dir build -R "e2e_play_storm|e2e_play_latency|e2e_play_seekplay" --output-on-failure
  ```
  Expected: storm (play1x scenario) `reposition==0`; latency `resyncCount==0`; seekplay `reposition<=2`.

---

## Task 11: Pre-roll + crossfade audio for the armed cut (Design + Integration — E2E-gated, not unit-TDD)

NOT unit-tested (audio ring/ffmpeg); E2E-gated by Task 12 (`silentAudioFrames` / no audible click is verified by inspection of `placeholderFrames`==0 + manual listen; the gate asserts `placeholderFrames==0` for video and `silentAudioFrames` does not spike).

**Design.**
- **Stage audio with the video.** During `fillStaging`, also decode the audio of the armed window into `m_stagingCache` (audio decoder track in `m_prerollAudioBank`), staging `[target, target+kPrerollAudioSpanMs]` of the active-view audio. The cache already exposes `audioSpanOrSilence(feedIndex, startSample, sampleFrames)` (outputframecache.h:18) for read-out. (Use a NEW constant `kPrerollAudioSpanMs` — do NOT reuse the existing `kAudioLeadMs` at playbackworker.h:82, which is the live audio-push lead and has different semantics.)
- **Crossfade at the cut frame.** At `maybeFireScheduledCut`, instead of the hard `m_audioPlayer->clear()` used on coarse seeks (audioplayer.cpp:362-373 does a fade-out + re-arm fade-in), drive an explicit short crossfade: read the last `kCrossfadeMs` of the *outgoing* audio (already queued) and the first `kCrossfadeMs` of the *staged* audio (`m_stagingCache->audioSpanOrSilence(view, targetStartSample, kCrossfadeFrames)`), linearly blend them, and push the blended span before resuming from the staged audio. Reuse the existing fade-in re-arm in `AudioPlayer::clear()` as a fallback if the staged span is silent.
- **Alignment.** Convert `target` ms → start sample using the audio sample rate (same conversion used in the existing audio enqueue path). Align the crossfade so the first staged sample lands exactly at the cut output frame (use `CutSchedule::playheadAfterCut` for the ms, then ms→sample).
- **Thread safety.** Audio staging happens worker-private during `fillStaging`; the blended push happens on the worker thread inside the cut promotion (no UI-thread audio call).

**Files:**
- Modify `playback/playbackworker.h`: add NEW constants `kPrerollAudioSpanMs` and `kCrossfadeMs` (note: `kAudioLeadMs` already EXISTS at playbackworker.h:82 with live-push semantics — do not redefine it); staging audio handled by existing `m_stagingCache`.
- Modify `playback/playbackworker.cpp`: extend `fillStaging` to also decode/insert audio into `m_stagingCache`; in `maybeFireScheduledCut` perform the crossfade blend + push instead of a hard clear.
- Possibly add a small helper `AudioPlayer::pushCrossfaded(const QByteArray& outgoing, const QByteArray& incoming, int channels)` if the blend cannot be expressed with existing pushSamples; otherwise blend in the worker and call `pushSamples`.

**Interfaces:**
- Consumes: `OutputFrameCache::audioSpanOrSilence`, `AudioPlayer::pushSamples`, `AudioPlayer::clear` (fallback), sample-rate conversion already in the worker.
- Produces: crossfaded audio at the cut; no hard click.

- [ ] **Step 1: Stage audio in fillStaging.**
  Extend the `fillStaging` packet loop (Task 9 Step 3) so that when the read packet's
  `stream_index` matches an `m_prerollAudioBank` track for the active view, it is decoded and
  inserted into the staging cache instead of being skipped. Mirror the existing audio decode +
  cache-insert path used for the live cache (`cacheOutputAudioFrame`, declared playbackworker.h:119,
  which decodes an `AVFrame` of audio and writes it into `m_outputCache`) — but write to
  `m_stagingCache`. Stage `[target, target + kPrerollAudioSpanMs]`.
  ```cpp
  // Inside the fillStaging packet loop, after the video branch:
  for (auto* aTrack : m_prerollAudioBank)
  {
      if (pkt->stream_index != aTrack->streamIndex) continue;
      if (aTrack->viewIndex != m_activeAudioView.load()) continue;  // active view only
      // decode pkt -> audioFrame; insert into m_stagingCache mirroring
      // cacheOutputAudioFrame (playbackworker.h:119 / its .cpp body), bounded to
      // [target, target + kPrerollAudioSpanMs].
  }
  ```
  **Acceptance:** after `m_stagingCovers`, `m_stagingCache->audioSpanOrSilence(view, targetStartSample, N)`
  returns non-silent samples for the armed window (no all-zero span). **Manual:** log the staged
  audio sample count for the active view; confirm it is non-zero before the cut fires.
  Build; commit `Tier3: stage active-view audio window during pre-roll`.

- [ ] **Step 2: Crossfade at the cut.**
  In `maybeFireScheduledCut` (Task 10 Step 3), for an ARMED cut, replace the hard
  `m_audioPlayer->clear()` reset (the coarse-seek path at playbackworker.cpp:571 and the
  fade-out + re-arm fade-in in `AudioPlayer::clear()`, audioplayer.cpp:362-373) with an explicit
  crossfade:
  ```cpp
  // 1. Outgoing tail: last kCrossfadeFrames already queued (from the live cache).
  // 2. Incoming head: m_stagingCache->audioSpanOrSilence(view, targetStartSample, kCrossfadeFrames)
  //    where targetStartSample = ms->sample(CutSchedule::playheadAfterCut(...), sampleRate).
  // 3. Linear blend across kCrossfadeFrames (gain out: 1->0, in: 0->1), push the blend
  //    via AudioPlayer::pushSamples (or AudioPlayer::pushCrossfaded helper), then resume
  //    from the staged audio.
  // 4. If the staged span is all-silence, fall back to the existing m_audioPlayer->clear()
  //    fade re-arm (audioplayer.cpp:362-373).
  ```
  **Acceptance:** `e2e_play_latency` still reports `resyncCount==0` (Step 3), and the `armedcut`
  gate (Task 12) holds `placeholderFrames==0`; audible click absence is a **manual** check (play
  the armed cut and listen — no pop at the cut frame).
  Build; commit `Tier3: crossfade audio at armed cut frame`.

- [ ] **Step 3: Regression check (audio latency gate).**
  ```
  cmake --build build --target play_harness record_harness
  ctest --test-dir build -R e2e_play_latency --output-on-failure
  ```
  Expected: latency `resyncCount==0` (the crossfade must not trigger a resync on unarmed playback; armed-cut audio is verified in Task 12).

---

## Task 12: armedcut E2E scenario — placeholderFrames==0 AND reposition==0 (E2E)

This is the end-to-end proof for Tasks 9–11. NOT a unit test.

**Design.** The `armedcut` scenario records a clip, plays from the head, calls `worker.armNextCut(targetMidpointMs)` (driving the staging fill + scheduled cut), lets the cut fire, and continues playing past the cut. The harness asserts, across the whole armed jump, that the worker reported zero `placeholderFrames` (no gray flash) and zero `reposition` (the cut did NOT fall back to `repositionTo`). It reuses the COUNTERS line which already carries `reposition` and (from Tier 1) `placeholderFrames`.

**Files:**
- Modify `tests/e2e/play_harness.cpp`: add the `armedcut` scenario branch that arms a cut at the clip midpoint after N frames and runs through the cut; ensure the COUNTERS line includes `placeholderFrames=<worker.outputStats().placeholderFrames>` (Tier 1 already added this).
- Modify `tests/e2e/run_playback_e2e.sh`: add an `armedcut` case asserting `placeholderFrames==0` and `reposition==0`.
- Modify `tests/e2e/CMakeLists.txt`: register `add_test(e2e_play_armedcut ...)` with the next free SRT port `23498` passed as the 5th positional arg (the driver reads the port from arg 5 / `OLR_PB_PORT`, NOT an `OLR_SRT_PORT` env var), and add `e2e_play_armedcut` to the shared play-gate `set_tests_properties` block (CMakeLists.txt:161-167).

**Interfaces:**
- Consumes: `PlaybackWorker::armNextCut`, `worker.counters()` (`reposition`), `worker.outputStats()` (`placeholderFrames`).
- Produces: `e2e_play_armedcut` gate.

- [ ] **Step 1: Add the harness scenario.**
  In `tests/e2e/play_harness.cpp`, add an `armedcut` branch: open the recorded clip, start playback, after ~30 delivered frames call `worker.armNextCut(durationMs/2)`, then continue ticking the output runtime for ~60 more frames so the staged cut fires and plays through. Build:
  ```
  cmake --build build --target play_harness record_harness
  ```
  Expected: builds.

- [ ] **Step 2: Add the shell gate.**
  In `tests/e2e/run_playback_e2e.sh` add to the case block (171-282):
  ```sh
  armedcut)
      [ "$(get placeholderFrames)" -eq 0 ] || fail "armedcut placeholderFrames=$(get placeholderFrames) (want 0)"
      [ "$(get reposition)" -eq 0 ] || fail "armedcut reposition=$(get reposition) (want 0)"
      ;;
  ```

- [ ] **Step 3: Register the ctest target.**
  Mirror the existing play-gate pattern (`tests/e2e/CMakeLists.txt:134-167`). The driver
  reads its SRT port from the **5th positional arg** (`SRT_PORT="${5:-${OLR_PB_PORT:-...}}"`,
  run_playback_e2e.sh:33) — there is NO `OLR_SRT_PORT` env var. The port must be passed as the
  5th positional arg, the same way every other play gate does it (e.g. storm passes `play1x 2 23464`).
  `23498` is the next free port that stays clear of the 23464-23478 play block, the sync
  range (23480-23489), av-sync (23492) and native (>=23550) ranges.

  Add the new `add_test` after `e2e_play_latency` (CMakeLists.txt:157-159), matching the
  `COMMAND bash "${_pb_driver}" ...` form used at :134-159:
  ```cmake
  add_test(NAME e2e_play_armedcut
      COMMAND bash "${_pb_driver}"
          "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" armedcut 2 23498)
  ```
  Then add `e2e_play_armedcut` to the EXISTING shared properties block (do NOT add a separate
  block / no `OLR_SRT_PORT`), so it inherits `LABELS "e2e"`, `TIMEOUT 180`, `RUN_SERIAL TRUE`:
  ```cmake
  set_tests_properties(
      e2e_play_storm e2e_play_seekplay e2e_play_stepscrub e2e_play_reverse
      e2e_play_sliderscrub e2e_play_liveedge e2e_play_4view e2e_play_latency e2e_play_armedcut
      PROPERTIES
      LABELS "e2e"
      TIMEOUT 180
      RUN_SERIAL TRUE)
  ```

- [ ] **Step 4: Run the new gate.**
  ```
  ctest --test-dir build -R e2e_play_armedcut --output-on-failure
  ```
  Expected: pass with `placeholderFrames==0` and `reposition==0`.

- [ ] **Step 5: Full E2E + unit regression sweep.**
  ```
  ctest --test-dir build -L unit --output-on-failure
  ctest --test-dir build -R "e2e_play_" --output-on-failure
  ```
  Expected: all unit tests pass; all existing E2E gates hold (play1x reposition==0; seekplay<=2; stepscrub<=10 & reuseSeek>=1; sliderscrub<=20; reverse<=150/<=4; liveedge<=3/<=3; latency resyncCount==0) plus armedcut.

- [ ] **Step 6: Commit.**
  ```
  git add tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh tests/e2e/CMakeLists.txt
  git commit -m "Tier3: armedcut E2E scenario asserting placeholderFrames==0 && reposition==0"
  ```

---

## Parallelization within Tier 3

The pure-logic tasks are fully independent of each other and of the integration tasks — they create new files only and touch the two CMake lists. They can be done in parallel by separate agents:

- **Lane A (FrameIndex):** Task 1 → Task 2. Files: `playback/frameindex.*`, `tests/unit/tst_frameindex.cpp`.
- **Lane B (Playlist):** Task 3 → Task 4. Files: `playback/replayplaylist.*`, `tests/unit/tst_replayplaylist.cpp`.
- **Lane C (Cut math):** Task 5 → Task 6. Files: `playback/cutschedule.*`, `tests/unit/tst_cutschedule.cpp`.

Lanes A/B/C each append one source to `tests/CMakeLists.txt` and one `olr_add_unit_test` to `tests/unit/CMakeLists.txt`; merge those two files carefully (they are the only shared edit — keep additions on separate lines to avoid conflicts).

The integration tasks are sequential and single-lane (they all edit `playback/playbackworker.cpp`):

- **Lane D (worker integration):** Task 7 (needs Lane A) → Task 9 (needs Lane A) → Task 10 (needs Lane C + Tier 2 token) → Task 11 → Task 12.
- **Lane E (UI):** Task 8 (needs Lane B + `armNextCut` from Task 9) — can be drafted in parallel but its build depends on Task 9 landing first.

Recommended order: run A, B, C in parallel; then D in sequence; fold E in after Task 9.

## Dependencies on Tier 2

Tier 3's atomic cut is built directly on Tier 2's published-cache and generation-token machinery. It will NOT function correctly (and Task 10/12 gates will fail) until these Tier 2 symbols exist:

- **`PlaybackWorker` cache generation token `std::atomic<uint64_t> m_cacheGeneration`** — Task 10 bumps it at the cut so `makeOutputSnapshot`'s committed playhead reflects the freshly-promoted (already target-covering) cache immediately, keeping `placeholderFrames==0`.
- **`OutputFrameCache` published as `std::shared_ptr<const OutputFrameCache>` swapped atomically** — Task 10's staging→active promotion is a single pointer swap under `m_bufferMutex` rather than a deep copy; this is what makes the cut atomic with respect to the OutputRuntime tick.
- **`OutputDispatcher::nextOutputFrameIndex()` (already present, outputdispatcher.h:82) + Tier 2 identity-skip** — Task 10's `CutSchedule::shouldFireAt` reads `nextOutputFrameIndex()`; the identity-skip ensures the held last-good frame (Tier 1) and the promoted cache don't cause redundant submits at the cut boundary.

Tier 3 also depends on **Tier 1**:
- **`PlaybackWorker::clearDecoderBuffers()`** (Tier 1 rename of `clearAllBuffers`, dropping the `m_outputCache->clear()` line) — Task 7's `repositionExact` calls it so an exact reseek does NOT wipe the output cache (preventing a gray flash on seek).
- **`OutputDispatcher` hold-last-frame + `heldFrames` stat** — guarantees that any one-tick gap during the staging fill before the cut fires substitutes the last good frame, so `placeholderFrames` stays 0 in the `armedcut` gate.
- **`convertToMediaVideoFrame` using `QByteArray(size, Qt::Uninitialized)`** — the pre-roll bank reuses this conversion path; no behavioral dependency beyond reuse.

If Tier 2 is not yet merged, Lanes A/B/C (pure logic, Tasks 1–6) and Task 8's pure wiring can still proceed; Tasks 9–12 must wait for the Tier 2 token + shared_ptr cache to land.
