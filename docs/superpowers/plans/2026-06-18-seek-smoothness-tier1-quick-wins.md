# Seek Smoothness — Tier 1 Quick Wins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Eliminate the gray-flash and audio-click artifacts that the user sees during scrubbing/seeking by shipping a set of small, no-architecture-change fixes (stop wiping the output cache on reposition, hold the last good frame in the dispatcher, deliver the target frame first, de-bounce the UI scrub, de-click mute).

**Architecture:** Playback is split across two QThreads: `PlaybackWorker` decodes into `m_outputCache`/per-track `TrackBuffer`s under `m_bufferMutex`, and `OutputRuntime` ticks ~1ms, snapshots the cache via `makeOutputSnapshot`, and renders through `OutputDispatcher` to sinks. These quick wins keep that structure intact: they stop the worker from blanking the cache on a seek, give the dispatcher a per-bus last-good-frame fallback so an empty/placeholder snapshot never paints gray, and coalesce the UI's seek spam so the worker sees fewer redundant repositions. No new threads, no clock changes, no graph rewiring.

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
| `playback/playbackworker.h` | Modify | Rename `clearAllBuffers()` decl → `clearDecoderBuffers()`. |
| `playback/playbackworker.cpp` | Modify | `clearDecoderBuffers()` no longer clears `m_outputCache`; switch the two callsites; deliver-target-first in `repositionTo`; short-circuit `deliverDueFrames` dead work; `convertToMediaVideoFrame` uninitialized alloc. |
| `playback/output/outputdispatcher.h` | Modify | `OutputDispatchStats::heldFrames`; `setHoldLastFrame(bool)`; `m_lastGoodFrame` map + `m_holdLastFrame` flag. |
| `playback/output/outputdispatcher.cpp` | Modify | Per-bus last-good-frame substitution in `dispatchTick`; count `heldFrames`. |
| `playback/audioplayer.cpp` | Modify | `setMuted(true)` uses `fadeOutAndClear` instead of hard `clear` (de-click mute). |
| `playback/seekcoalescer.h` | **Create** | Tiny header-only `SeekCoalescer` helper: pure logic for "seek now on first/last, coalesce the middle". |
| `uimanager.h` | Modify | `QTimer m_scrubCoalesceTimer`; `int64_t m_pendingScrubTargetMs`; `bool m_haveScrubTarget`; `commitPendingScrub()` slot; remove redundant per-move `audioPlayer->clear()` rationale. |
| `uimanager.cpp` | Modify | Wire the coalesce timer into `seekPlayback`; add `commitPendingScrub`; drop the redundant `m_audioPlayer->clear()` at :1702. |
| `Main.qml` | Modify | `onMoved`/`onReleased` wiring for first-move/coalesced/release semantics. |
| `tests/unit/tst_outputdispatcher_holdlast.cpp` | **Create** | Qt Test for the dispatcher hold-last-frame path. |
| `tests/unit/tst_seekcoalescer.cpp` | **Create** | Qt Test for `SeekCoalescer` logic. |
| `tests/unit/CMakeLists.txt` | Modify | Register the two new unit tests. |
| `tests/e2e/play_harness.cpp` | Modify | New `seekflash` scenario; print `placeholderFrames`/`heldFrames` on the COUNTERS line. |
| `tests/e2e/run_playback_e2e.sh` | Modify | Parse `placeholderFrames`/`heldFrames`; assert the `seekflash` gate. |
| `tests/e2e/CMakeLists.txt` | Modify | Register `e2e_play_seekflash` (port 23494). |

---

## Task 1 — Stop clearing m_outputCache on reposition (rename clearAllBuffers → clearDecoderBuffers)

The output cache is the only thing the OutputRuntime can paint from. Today every full reposition and every skip-forward calls `clearAllBuffers()`, which empties `m_outputCache` → the very next OutputRuntime tick snapshots an empty cache → `videoFrameOrPlaceholder` returns the gray 16/128/128 placeholder → gray flash. The decoder's own `TrackBuffer`s still need clearing (stale frames would defeat the trail logic), but the *output* cache should keep its last frames until the new ones overwrite them.

**Files:**
- Modify `playback/playbackworker.h:121` (rename decl).
- Modify `playback/playbackworker.cpp:231-238` (rename impl, drop the `m_outputCache->clear()` line), `playback/playbackworker.cpp:564` and `playback/playbackworker.cpp:883` (callsites).
- Test: covered at the E2E level by Task 2's `seekflash` scenario (a unit test cannot exercise the private method without a real decode; `OutputFrameCache` itself is already unit-tested in `tst_outputframecache.cpp` and is unchanged here).

**Interfaces:**
- Produces: `void PlaybackWorker::clearDecoderBuffers();` — clears each `track->buffer` + `decimateCounter=0` under `m_bufferMutex`; does NOT touch `m_outputCache`.
- Removes: `void PlaybackWorker::clearAllBuffers();`

- [ ] **Step 1: Confirm there are exactly three references.** Run `grep -rn "clearAllBuffers" playback/ tests/`. Expect exactly: `playback/playbackworker.h:121` (decl), `playback/playbackworker.cpp:231` (impl), `playback/playbackworker.cpp:564` and `:883` (callsites). If any other caller exists, STOP and reconcile before renaming.

- [ ] **Step 2: Rename the declaration.** In `playback/playbackworker.h`, change line 121:
  ```cpp
      void clearDecoderBuffers(); // clear every TrackBuffer (holds m_bufferMutex); leaves m_outputCache intact
  ```

- [ ] **Step 3: Rename the impl and drop the cache clear.** In `playback/playbackworker.cpp`, replace lines 231-238:
  ```cpp
  void PlaybackWorker::clearDecoderBuffers() {
      QMutexLocker bufferLocker(&m_bufferMutex);
      for (auto* track : m_decoderBank) {
          track->buffer.clear();
          track->decimateCounter = 0;
      }
      // NOTE: deliberately does NOT clear m_outputCache. The OutputRuntime paints
      // exclusively from m_outputCache; wiping it here makes the next ~1ms tick
      // snapshot an empty cache and render the gray placeholder (the seek flash).
      // The cache's stale frames are harmless: the forward fill re-inserts the
      // new frames before the playhead reaches them, and trimBefore() drops the
      // old ones. See docs/superpowers/plans (Tier 1 Task 1).
  }
  ```

- [ ] **Step 4: Switch the reposition callsite.** In `playback/playbackworker.cpp:564`, change `clearAllBuffers();` to `clearDecoderBuffers();`.

- [ ] **Step 5: Switch the skip-forward callsite.** In `playback/playbackworker.cpp:883`, change `clearAllBuffers();` to `clearDecoderBuffers();`.

- [ ] **Step 6: Build the playback test lib + the storm E2E gate fixture.** Run `cmake --build build --target olr_test_playback play_harness`. Expect a clean build (no remaining `clearAllBuffers` references).

- [ ] **Step 7: Guard against E2E regressions.** Run `ctest --test-dir build -R "e2e_play_(storm|seekplay|stepscrub|reverse)" --output-on-failure`. Expect all four to pass (play1x reposition==0, seekplay reposition<=2, stepscrub reposition<=10 & reuseSeek>=1, reverse gates). The reposition *counts* are unchanged by this task — only the cache is no longer blanked.

- [ ] **Step 8: Format + commit.** Run `xcrun clang-format -i playback/playbackworker.cpp playback/playbackworker.h` then `git add -p` the changed lines. Commit:
  ```
  git commit -m "playback: keep output cache across reposition (rename clearAllBuffers->clearDecoderBuffers)

  Stop blanking m_outputCache on full reposition/skip-forward; the OutputRuntime
  paints only from that cache, so wiping it caused a one-tick gray placeholder
  flash on every cold seek. Decoder TrackBuffers are still cleared.

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 2 — OutputDispatcher hold-last-frame (zero-gray guarantee) + seekflash E2E

Even with Task 1, the cache can momentarily contain only frames *not covering the target* (e.g. far-backward seek before the fill lands, or the very first frame after a cold start). When `renderBus` returns `video.isPlaceholder`, the dispatcher should substitute the last *real* video it rendered for that bus, keep the freshly-rendered audio + identity + `outputFrameIndex`, and count it as a held frame. Guarded by `setHoldLastFrame(bool)` (default true) so the test can disable it and assert the un-held baseline.

**Files:**
- Modify `playback/output/outputdispatcher.h:62-70` (add `heldFrames`), `:72-110` (add `setHoldLastFrame`, members).
- Modify `playback/output/outputdispatcher.cpp:72-99` (substitution in `dispatchTick`).
- Create `tests/unit/tst_outputdispatcher_holdlast.cpp`.
- Modify `tests/unit/CMakeLists.txt` (register).
- Modify `tests/e2e/play_harness.cpp`, `tests/e2e/run_playback_e2e.sh`, `tests/e2e/CMakeLists.txt` (seekflash gate + counters).

**Interfaces:**
- Produces: `void OutputDispatcher::setHoldLastFrame(bool enabled);` and `OutputDispatchStats::heldFrames` (qint64).
- Consumes: `OutputBusFrame` (`video.isPlaceholder`), `QHash<OutputBusId, OutputBusFrame>` (rendered map already in `dispatchTick`).

- [ ] **Step 1: Write the failing unit test.** Create `tests/unit/tst_outputdispatcher_holdlast.cpp`:
  ```cpp
  #include <QtTest>

  #include "playback/output/outputdispatcher.h"

  static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
      MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
      f.feedIndex = feed;
      f.ptsMs = pts;
      return f;
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
      bool submit(const OutputBusFrame& frame) override {
          if (!m_active) return false;
          frames.append(frame);
          return true;
      }
      QVector<OutputBusFrame> frames;

  private:
      OutputTargetKind m_kind = OutputTargetKind::QtPreview;
      bool m_active = false;
  };

  class TestOutputDispatcherHoldLast : public QObject {
      Q_OBJECT
  private slots:
      void emptyCacheAfterRealFrameHoldsLastGoodVideo();
      void holdDisabledLeavesPlaceholderVisible();
  };

  void TestOutputDispatcherHoldLast::emptyCacheAfterRealFrameHoldsLastGoodVideo() {
      OutputTargetAssignment feed0;
      feed0.id = QStringLiteral("feed0-preview");
      feed0.sourceBus = OutputBusId::feed(0);
      feed0.kind = OutputTargetKind::QtPreview;
      feed0.enabled = true;

      CollectingSink sink(OutputTargetKind::QtPreview);
      OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
      dispatcher.setHoldLastFrame(true); // default, asserted explicitly
      dispatcher.setEndpoints({{feed0, &sink}});

      PlaybackStateSnapshot state;
      state.playheadMs = 100;
      state.playing = false;
      state.selectedFeedIndex = 0;

      // Tick 1: a real frame is present.
      OutputFrameCache full(1, 4, 4);
      full.insertVideoFrame(video(0, 100, 77));
      dispatcher.dispatchTick(full, state);

      // Tick 2: the cache went empty (mid-seek) → renderBus yields a placeholder.
      OutputFrameCache empty(1, 4, 4);
      dispatcher.dispatchTick(empty, state);

      QCOMPARE(sink.frames.size(), 2);
      // The held tick must NOT be the gray placeholder...
      QVERIFY(!sink.frames[1].video.isPlaceholder);
      // ...and its video pixels must equal the prior real frame.
      QCOMPARE(uchar(sink.frames[1].video.planeY.at(0)), uchar(77));
      QCOMPARE(sink.frames[1].video.planeY, sink.frames[0].video.planeY);
      // Held frame keeps the fresh tick's outputFrameIndex (clock never stalls).
      QCOMPARE(sink.frames[1].outputFrameIndex, qint64(1));

      const OutputDispatchStats stats = dispatcher.stats();
      QCOMPARE(stats.heldFrames, qint64(1));
      QCOMPARE(stats.placeholderFrames, qint64(0));
  }

  void TestOutputDispatcherHoldLast::holdDisabledLeavesPlaceholderVisible() {
      OutputTargetAssignment feed0;
      feed0.id = QStringLiteral("feed0-preview");
      feed0.sourceBus = OutputBusId::feed(0);
      feed0.kind = OutputTargetKind::QtPreview;
      feed0.enabled = true;

      CollectingSink sink(OutputTargetKind::QtPreview);
      OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
      dispatcher.setHoldLastFrame(false);
      dispatcher.setEndpoints({{feed0, &sink}});

      PlaybackStateSnapshot state;
      state.playheadMs = 100;
      state.playing = false;
      state.selectedFeedIndex = 0;

      OutputFrameCache full(1, 4, 4);
      full.insertVideoFrame(video(0, 100, 77));
      dispatcher.dispatchTick(full, state);

      OutputFrameCache empty(1, 4, 4);
      dispatcher.dispatchTick(empty, state);

      QCOMPARE(sink.frames.size(), 2);
      QVERIFY(sink.frames[1].video.isPlaceholder);
      const OutputDispatchStats stats = dispatcher.stats();
      QCOMPARE(stats.heldFrames, qint64(0));
      QCOMPARE(stats.placeholderFrames, qint64(1));
  }

  QTEST_GUILESS_MAIN(TestOutputDispatcherHoldLast)
  #include "tst_outputdispatcher_holdlast.moc"
  ```

- [ ] **Step 2: Register the test.** In `tests/unit/CMakeLists.txt`, after line 75 (`olr_add_unit_test(tst_outputdispatcher olr_test_playback)`), add:
  ```cmake
  olr_add_unit_test(tst_outputdispatcher_holdlast olr_test_playback)
  ```

- [ ] **Step 3: Run it — expect a BUILD failure.** Run `cmake --build build --target tst_outputdispatcher_holdlast`. Expect compile errors: `setHoldLastFrame` and `OutputDispatchStats::heldFrames` do not exist yet.

- [ ] **Step 4: Add the stat + setter + members to the header.** In `playback/output/outputdispatcher.h`, add `heldFrames` to `OutputDispatchStats` after line 66 (`qint64 placeholderFrames = 0;`) (anchor on the `placeholderFrames` / `silentAudioFrames` field strings, not raw line numbers, which shift):
  ```cpp
      qint64 heldFrames = 0;
  ```
  Add the setter in the public section after line 83 (`void setRuntimeStats(...)`):
  ```cpp
      void setHoldLastFrame(bool enabled) { m_holdLastFrame = enabled; }
  ```
  Add members in the private section after line 109 (`OutputDispatchStats m_stats;`):
  ```cpp
      bool m_holdLastFrame = true;
      QHash<OutputBusId, OutputBusFrame> m_lastGoodFrame; // per-bus last real video
  ```

- [ ] **Step 5: Substitute the held frame in dispatchTick.** In `playback/output/outputdispatcher.cpp`, replace the render block at lines 81-85:
  ```cpp
          const OutputBusId bus = endpoint.assignment.sourceBus;
          if (!rendered.contains(bus)) {
              OutputBusFrame frame = renderBus(bus, outputFrameIndex, tickState, cache);
              if (m_holdLastFrame && frame.video.isPlaceholder && m_lastGoodFrame.contains(bus)) {
                  // Paint the last real video for this bus instead of the gray
                  // placeholder; keep the freshly-rendered audio + identity +
                  // outputFrameIndex so the clock and audio timeline never stall.
                  frame.video = m_lastGoodFrame.value(bus).video;
                  m_stats.heldFrames++;
              } else if (!frame.video.isPlaceholder) {
                  m_lastGoodFrame.insert(bus, frame);
              }
              rendered.insert(bus, frame);
              countFrameHealth(rendered.value(bus));
          }
  ```
  (`countFrameHealth` already only increments `placeholderFrames` when `frame.video.isPlaceholder`; a held frame is no longer a placeholder, so `placeholderFrames` stays 0 — matching the test.)

- [ ] **Step 6: Run it — expect PASS.** Run `cmake --build build --target tst_outputdispatcher_holdlast && ctest --test-dir build -R tst_outputdispatcher_holdlast --output-on-failure`. Expect both slots pass.

- [ ] **Step 7: Re-run the existing dispatcher test (no regression).** Run `ctest --test-dir build -R "^tst_outputdispatcher$" --output-on-failure`. Expect PASS — the existing tests never hit an empty cache, so `m_lastGoodFrame` is populated but never substituted, and `heldFrames` stays 0.

- [ ] **Step 8: Add the seekflash E2E scenario to play_harness.** In `tests/e2e/play_harness.cpp`, after the `liveedge` branch (ends line 193), before the final `else`:
  ```cpp
          } else if (scen == "seekflash") {
              // Seek to mid, then play 1x. After the first frame the output must
              // never paint the gray placeholder: Task 1 keeps the cache, Task 2
              // holds the last frame if the cache momentarily misses the target.
              transport.setSpeed(1.0);
              transport.seek(10000);
              worker.seekTo(10000);
              transport.setPlaying(true);
              QTimer::singleShot(8000, &app, finish);

  ```
  Then extend the COUNTERS print in `finish` (lines 86-89) to add the output stats:
  ```cpp
          const OutputDispatchStats os = worker.outputStats();
          printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
                 "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d "
                 "placeholderFrames=%lld heldFrames=%lld\n",
                 c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
                 c.audioPushes, c.framesDropped, audio.resyncCount(),
                 (long long) os.placeholderFrames, (long long) os.heldFrames);
  ```
  Add `#include "playback/output/outputdispatcher.h"` near the existing playback includes (after line 31). Apply the same two new fields to the unknown-scenario fallback print (lines 201-204) for parser symmetry (copy the identical printf body).

- [ ] **Step 9: Parse + assert in the driver.** In `tests/e2e/run_playback_e2e.sh`, after line 149 (`resyncCount="$(get resyncCount)"`) add:
  ```sh
  placeholderFrames="$(get placeholderFrames)"
  heldFrames="$(get heldFrames)"
  [ -n "$placeholderFrames" ] || placeholderFrames="?"
  [ -n "$heldFrames" ] || heldFrames="?"
  ```
  Add a `seekflash` case in the `case "$SCENARIO"` block (before the `*)` default at line 278):
  ```sh
      seekflash)
          # Seek to mid then play 1x. The output graph must never paint the gray
          # placeholder once playback is live: Task 1 keeps the output cache across
          # the reposition, Task 2 holds the last good frame if the cache briefly
          # misses the target. A non-zero placeholderFrames is the seek flash.
          if ! num "$placeholderFrames" || [ "$placeholderFrames" -ne 0 ]; then
              echo "FAIL: seekflash painted gray (placeholderFrames=$placeholderFrames, expected 0) — seek flash"
              fail=1
          fi
          if ! num "$reposition" || [ "$reposition" -gt 2 ]; then
              echo "FAIL: seekflash repositioned too much (reposition=$reposition, expected <=2)"
              fail=1
          fi
          ;;
  ```
  Append the two fields to the `SUMMARY` line (line 284):
  ```sh
  SUMMARY="reposition=$reposition reuseSeek=$reuseSeek reverseChunkSeek=$reverseChunkSeek eofTailSeek=$eofTailSeek skipForward=$skipForward audioPushes=$audioPushes framesDropped=$framesDropped resyncCount=$resyncCount placeholderFrames=$placeholderFrames heldFrames=$heldFrames"
  ```

- [ ] **Step 10: Register the E2E test.** In `tests/e2e/CMakeLists.txt`, after the `e2e_play_latency` block (line 159) add:
  ```cmake
  add_test(NAME e2e_play_seekflash
      COMMAND bash "${_pb_driver}"
          "$<TARGET_FILE:play_harness>" "$<TARGET_FILE:record_harness>" seekflash 2 23494)
  ```
  Add `e2e_play_seekflash` to the `set_tests_properties(...)` list at lines 161-167.

- [ ] **Step 11: Build + run the new E2E gate.** Run `cmake --build build --target play_harness record_harness && ctest --test-dir build -R e2e_play_seekflash --output-on-failure`. Expect PASS with `placeholderFrames=0`.

- [ ] **Step 12: Format + commit.** `xcrun clang-format -i playback/output/outputdispatcher.cpp playback/output/outputdispatcher.h tests/unit/tst_outputdispatcher_holdlast.cpp tests/e2e/play_harness.cpp`. Commit:
  ```
  git commit -m "output: hold last good frame on placeholder ticks + seekflash gate

  OutputDispatcher substitutes the per-bus last real video when renderBus yields
  a placeholder (empty/uncovered cache mid-seek), counted as heldFrames and
  gated by setHoldLastFrame(true). New e2e_play_seekflash asserts placeholderFrames==0.

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 3 — convertToMediaVideoFrame: Qt::Uninitialized (dead-store removal)

`convertToMediaVideoFrame` allocates the Y/U/V planes zero-filled (`'\0'`), then immediately overwrites every used byte with the per-line `memcpy` (copyW = min(width, |srcStride|, dstStride) for each of `height` lines). The zero-fill is a dead store on a 1080p frame (~3MB memset per decoded frame). Switching to `QByteArray(size, Qt::Uninitialized)` removes it. Output is byte-identical for valid frames (every plane byte up to the used width is copied; padding bytes between width and stride are never read by the renderer). This is a perf-only change — covered by the existing decode-correctness E2E (any garbage in real output would fail `seekplay`/`play1x` visually and the `seekflash` gate's frame equality is already exercised in Task 2's unit test against `solidYuv420p` frames, not this path).

**Files:**
- Modify `playback/playbackworker.cpp:1215-1217`.
- Test: covered by existing E2E decode path (`e2e_play_storm`, `e2e_play_seekplay`); no new unit test (perf-only, identical output).

**Interfaces:** none changed.

- [ ] **Step 1: Replace the zero-filled allocations.** In `playback/playbackworker.cpp`, replace lines 1215-1217:
  ```cpp
      // Allocate uninitialized: the per-line memcpy below overwrites every byte
      // up to copyW for all `height` lines, so a zero-fill is a dead store
      // (~3 MB memset per 1080p frame). Padding bytes (width..stride) are never
      // read by the renderer.
      out.planeY = QByteArray(out.strideY * frame->height, Qt::Uninitialized);
      out.planeU = QByteArray(out.strideU * chromaH, Qt::Uninitialized);
      out.planeV = QByteArray(out.strideV * chromaH, Qt::Uninitialized);
  ```

- [ ] **Step 2: Build.** Run `cmake --build build --target olr_test_playback play_harness`. Expect clean build.

- [ ] **Step 3: Decode-correctness E2E.** Run `ctest --test-dir build -R "e2e_play_(storm|seekplay|seekflash)" --output-on-failure`. Expect PASS (frames still decode correctly; no gray/garbage).

- [ ] **Step 4: Format + commit.** `xcrun clang-format -i playback/playbackworker.cpp`. Commit:
  ```
  git commit -m "playback: allocate video planes uninitialized (drop dead zero-fill)

  convertToMediaVideoFrame memset every plane to 0 then memcpy'd every used byte
  on top — a ~3MB dead store per 1080p frame. Output is byte-identical.

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 4 — Deliver-the-target-frame-first for all-intra reposition

In `repositionTo`'s full path, the fill loop currently decodes forward until `newestPtsMin() >= target + frameDurMs` *before* it calls `deliverDueFrames(target, dir)` (line 606→609-610). For a far-backward seek or a cold seek that anchors `kTrailMs`/`kLeadMs` behind the target, the playhead frame is decoded early in the loop but not painted until the *entire* trail/lead window is filled — a visible lag. Since recordings are all-intra, the target frame is decodable the moment the loop reaches it. The fix: inside the same loop, once every track has a frame at/just-before the target (reuse coverage), deliver immediately, then keep filling the trail/lead asynchronously in the same loop.

**Risk + how back-step reuse is preserved:** `stepscrub` relies on each back-step finding a frame already in the `TrackBuffer` window (`reuseAt` → `reuseSeek++`, not `reposition++`). The early deliver does NOT change what gets *buffered* — the loop still continues to `fillTo = target + frameDurMs()` afterward, inserting the same trail/lead frames into the same `TrackBuffer`s. Only the *paint timing* moves earlier. `reuseAt` reads `track->buffer.hasFrameNear`, which is populated by the unchanged `decodePacketIntoBank` inserts, so the next back-step still reuses. We deliver at most once (guarded by a `bool deliveredEarly`), and the final `deliverDueFrames` after the loop is kept (its direction-aware dedup is a no-op if the same pts was already delivered — `p == last` ⇒ `deliver = false`), so there is no double-paint and no out-of-order paint.

**Files:**
- Modify `playback/playbackworker.cpp:584-611` (fill loop).
- Test: E2E reasoning + the `seekflash` gate (Task 2). The gate already asserts `placeholderFrames==0`; this task lowers the *time-to-first-paint* but the count-based gates are the regression guard. Add an assertion comment to `seekflash`.

**Interfaces:** none changed (internal loop behavior only).

- [ ] **Step 1: Add the early-deliver guard + check inside the fill loop.** In `playback/playbackworker.cpp`, modify the fill loop (lines 584-607). Add a flag before the loop and an early-deliver check after each successful decode. Replace lines 584-607:
  ```cpp
      const int64_t fillTo = target + frameDurMs();
      int packets = 0;
      const int packetBudget = (capFrames(trackCount) + 4) * trackCount * 2;
      bool deliveredEarly = false;
      while (!shouldInterrupt()) {
          // A newer explicit seek supersedes this fill.
          {
              QMutexLocker locker(&m_mutex);
              if (m_seekTargetMs >= 0) break;
          }

          int ret = av_read_frame(m_fmtCtx, pkt);
          if (ret < 0) break; // EOF/short file: deliver what we have

          // Reposition decodes forward from the anchor; protect the [target,
          // target+kLead] span (dir=+1) — the trail below target is also kept by
          // the anchor being kTrailMs/kLeadMs below it.
          decodePacketIntoBank(pkt, vf, af, target, /*dir*/ 1, trackCount,
                               /*decimate*/ false, /*step*/ 1,
                               /*audioOn*/ false, /*dedupTail*/ false);
          av_packet_unref(pkt);

          // All-intra deliver-first: the moment every track has a frame at the
          // target, paint it — do NOT wait for the whole trail/lead to fill. The
          // loop keeps filling afterwards (same inserts → back-step reuse intact);
          // the final deliverDueFrames below is a dedup no-op for this pts.
          if (!deliveredEarly && reuseAt(target)) {
              resetDedup();
              deliverDueFrames(target, dir);
              deliveredEarly = true;
          }

          if (++packets > packetBudget) break; // safety bound
          if (newestPtsMin() >= fillTo) break; // covered the target
      }
  ```

- [ ] **Step 2: Avoid the redundant final reset when already delivered.** Lines 609-610 currently always `resetDedup(); deliverDueFrames(target, dir);`. Keep `deliverDueFrames` (it is a dedup no-op if `deliveredEarly`), but only `resetDedup()` when we did NOT deliver early (re-resetting after an early deliver would let the same pts re-paint). Replace lines 609-610:
  ```cpp
      if (!deliveredEarly) resetDedup();
      deliverDueFrames(target, dir);
  ```

- [ ] **Step 3: Build.** Run `cmake --build build --target olr_test_playback play_harness`. Expect clean build.

- [ ] **Step 4: Guard the back-step + reverse gates (the critical regression check).** Run `ctest --test-dir build -R "e2e_play_(stepscrub|reverse|seekplay|seekflash)" --output-on-failure`. Expect: stepscrub reposition<=10 AND reuseSeek>=1 (the early deliver must NOT convert reuse-seeks into repositions — it lives only in the full-reposition path, never on the `reuseAt` fast-path at line 550); reverse reverseChunkSeek<=150 AND reposition<=4; seekplay reposition<=2; seekflash placeholderFrames==0.

- [ ] **Step 5: Document the assertion in the seekflash gate.** In `tests/e2e/run_playback_e2e.sh`, extend the `seekflash)` case comment (added in Task 2) with: `# Task 4 (deliver-target-first) lowers time-to-first-paint; the placeholderFrames==0 gate is the regression guard.` (comment-only edit).

- [ ] **Step 6: Format + commit.** `xcrun clang-format -i playback/playbackworker.cpp`. Commit:
  ```
  git commit -m "playback: deliver target frame first in reposition (all-intra)

  Paint the playhead frame as soon as every track covers the target, then keep
  filling the trail/lead in the same loop. Same inserts -> back-step reuse intact;
  the final deliverDueFrames is a dedup no-op for the early-delivered pts.

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 5 — Short-circuit deliverDueFrames dead work when the output graph is active

When `m_outputRuntime != nullptr` (the normal broadcast/preview path), `deliverDueFrames` builds a full `framesSnapshot` of every track, a local `OutputFrameCache`, an `OutputBusEngine`, and a `PlaybackStateSnapshot` (lines 1126-1162) — then `continue`s without ever using them (the `renderFeed` at line 1184 only runs in the `!outputGraphActive` branch). That heavy build runs on every deliver on the hot decode thread. Build the local cache/engine only in the `!outputGraphActive` branch; in the active branch, do only the dedup bookkeeping.

**Files:**
- Modify `playback/playbackworker.cpp:1116-1196` (`deliverDueFrames`).
- Test: behavior-preserving refactor; covered by the existing E2E suite (the dedup bookkeeping that drives `reuseSeek`/`reposition` is unchanged). No new unit test.

**Interfaces:** none changed.

- [ ] **Step 1: Hoist the local-cache build into the inactive branch.** In `playback/playbackworker.cpp`, restructure `deliverDueFrames` so the snapshot/cache/engine build only happens when `!outputGraphActive`. Replace the body from line 1126 (`{` opening the `bufferLocker` scope) through line 1190 (`}` closing it) with:
  ```cpp
      {
          QMutexLocker bufferLocker(&m_bufferMutex);

          // Only the inactive-output-graph path needs to render frames here; when
          // the OutputRuntime is active it paints from m_outputCache on its own
          // tick, so building a local snapshot/cache/engine is pure dead work.
          OutputBusEngine* engine = nullptr;
          OutputFrameCache* localCache = nullptr;
          PlaybackStateSnapshot state;
          std::unique_ptr<OutputBusEngine> engineHolder;
          std::unique_ptr<OutputFrameCache> cacheHolder;

          if (!outputGraphActive) {
              int placeholderWidth = 1920;
              int placeholderHeight = 1080;
              QVector<QVector<TrackBuffer::Frame>> snapshots;
              snapshots.reserve(m_decoderBank.size());
              for (auto* track : m_decoderBank) {
                  QVector<TrackBuffer::Frame> frames =
                      track ? track->buffer.framesSnapshot() : QVector<TrackBuffer::Frame>();
                  for (const TrackBuffer::Frame& frame : frames) {
                      if (frame.frame.isValid()) {
                          placeholderWidth = frame.frame.width;
                          placeholderHeight = frame.frame.height;
                          break;
                      }
                  }
                  snapshots.append(frames);
              }

              cacheHolder = std::make_unique<OutputFrameCache>(m_decoderBank.size(),
                                                              placeholderWidth, placeholderHeight);
              for (int trackIndex = 0; trackIndex < m_decoderBank.size(); ++trackIndex) {
                  DecoderTrack* track = m_decoderBank[trackIndex];
                  if (!track) continue;
                  for (TrackBuffer::Frame frame : snapshots[trackIndex]) {
                      frame.frame.feedIndex = track->feedIndex;
                      cacheHolder->insertVideoFrame(frame.frame);
                  }
              }
              localCache = cacheHolder.get();

              engineHolder = std::make_unique<OutputBusEngine>(rate, m_decoderBank.size(),
                                                              placeholderWidth, placeholderHeight);
              engine = engineHolder.get();

              state.playheadMs = P;
              state.playing = false;
              state.speed = 1.0;
              state.playStartedAtOutputFrame = outputFrameIndex;
              state.playStartedAtPlayheadMs = P;
              state.selectedFeedIndex = m_decoderBank.isEmpty() ? -1 : 0;
          }

          for (auto* track : m_decoderBank) {
              if (!track || !track->provider) continue;
              MediaVideoFrame f;
              int64_t p;
              if (track->buffer.frameAt(P, f, p)) {
                  // Direction-aware dedup (spec §5): forbid out-of-order paints.
                  //  - forward (+1): deliver iff pts moved up (or after a reset);
                  //  - reverse (-1): deliver iff pts moved down (or after a reset).
                  const int64_t last = track->lastDeliveredPtsMs;
                  bool deliver;
                  if (last < 0)
                      deliver = true; // post-reset
                  else if (dir >= 0)
                      deliver = (p > last);
                  else
                      deliver = (p < last);
                  if (p == last) deliver = false; // already shown
                  if (deliver) {
                      track->lastDeliveredPtsMs = p;
                      if (outputGraphActive) continue;
                      OutputBusFrame busFrame =
                          engine->renderFeed(track->feedIndex, outputFrameIndex, state, *localCache);
                      pending.append({track->provider, busFrame.video});
                  }
              }
          }
      }
  ```

- [ ] **Step 2: Build.** Run `cmake --build build --target olr_test_playback play_harness`. Expect clean build (verify `<memory>` is already included; `std::unique_ptr` is used elsewhere in this file, e.g. `m_outputCache`).

- [ ] **Step 3: Full E2E regression (dedup unchanged).** Run `ctest --test-dir build -R "e2e_play_(storm|seekplay|stepscrub|reverse|sliderscrub|liveedge|seekflash)" --output-on-failure`. Expect all gates pass — `reuseSeek`/`reposition`/`reverseChunkSeek` are driven by the unchanged dedup bookkeeping.

- [ ] **Step 4: Format + commit.** `xcrun clang-format -i playback/playbackworker.cpp`. Commit:
  ```
  git commit -m "playback: skip dead deliverDueFrames cache build when output graph active

  When m_outputRuntime is active the OutputRuntime paints from m_outputCache, so
  the local snapshot/cache/engine in deliverDueFrames were built and discarded on
  every deliver. Build them only in the !outputGraphActive branch.

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 6 — UI scrub coalescing (SeekCoalescer + UIManager + Main.qml)

The slider's `onMoved` fires ~30-60/s while dragging; each calls `seekPlayback` → `worker->seekTo`. Coalesce them: seek immediately on the first move and on release, but in between commit only the *latest* target on a single-shot timer (Qt::PreciseTimer, ~16-33ms). The pure decision logic lives in a tiny testable `SeekCoalescer` helper (unit-tested); `UIManager` owns the `QTimer` and the worker call.

**Files:**
- Create `playback/seekcoalescer.h` (header-only logic).
- Create `tests/unit/tst_seekcoalescer.cpp`.
- Modify `tests/unit/CMakeLists.txt` (register).
- Modify `uimanager.h` (timer + pending state + slot), `uimanager.cpp` (wire), `Main.qml` (onMoved/onReleased).

**Interfaces:**
- Produces: `class SeekCoalescer` with `bool offer(int64_t target)` (returns true ⇒ caller should seek NOW), `int64_t takePending(bool& has)` (drains the deferred latest target), `void reset()`.
- `UIManager::seekPlayback(int64_t)` consumes `SeekCoalescer::offer`; `commitPendingScrub()` consumes `takePending`.

- [ ] **Step 1: Write the failing SeekCoalescer unit test.** Create `tests/unit/tst_seekcoalescer.cpp`:
  ```cpp
  #include <QtTest>

  #include "playback/seekcoalescer.h"

  class TestSeekCoalescer : public QObject {
      Q_OBJECT
  private slots:
      void firstOfferSeeksImmediately();
      void burstAfterFirstDefersAndKeepsOnlyLatest();
      void commitClearsPendingUntilNextBurst();
      void resetReArmsImmediateSeek();
  };

  void TestSeekCoalescer::firstOfferSeeksImmediately() {
      SeekCoalescer c;
      QVERIFY(c.offer(100)); // first move → seek now
  }

  void TestSeekCoalescer::burstAfterFirstDefersAndKeepsOnlyLatest() {
      SeekCoalescer c;
      QVERIFY(c.offer(100));   // immediate
      QVERIFY(!c.offer(110));  // deferred
      QVERIFY(!c.offer(120));  // deferred, supersedes 110
      bool has = false;
      QCOMPARE(c.takePending(has), int64_t(120));
      QVERIFY(has);
      // Drained: nothing left until the next deferred offer.
      bool has2 = true;
      c.takePending(has2);
      QVERIFY(!has2);
  }

  void TestSeekCoalescer::commitClearsPendingUntilNextBurst() {
      SeekCoalescer c;
      QVERIFY(c.offer(100));
      QVERIFY(!c.offer(110));
      bool has = false;
      QCOMPARE(c.takePending(has), int64_t(110));
      QVERIFY(has);
      // After draining, a NEW offer is still deferred (the gesture is ongoing),
      // not immediate — only reset() re-arms the immediate path.
      QVERIFY(!c.offer(130));
  }

  void TestSeekCoalescer::resetReArmsImmediateSeek() {
      SeekCoalescer c;
      QVERIFY(c.offer(100));
      QVERIFY(!c.offer(110));
      c.reset(); // release: gesture over
      QVERIFY(c.offer(200)); // next gesture's first move is immediate again
  }

  QTEST_GUILESS_MAIN(TestSeekCoalescer)
  #include "tst_seekcoalescer.moc"
  ```

- [ ] **Step 2: Register the test.** In `tests/unit/CMakeLists.txt`, after the line added in Task 2 Step 2 (`olr_add_unit_test(tst_outputdispatcher_holdlast olr_test_playback)`), add:
  ```cmake
  olr_add_unit_test(tst_seekcoalescer olr_test_playback)
  ```

- [ ] **Step 3: Run it — expect a BUILD failure.** Run `cmake --build build --target tst_seekcoalescer`. Expect compile error: `playback/seekcoalescer.h` does not exist.

- [ ] **Step 4: Create the header-only helper.** Create `playback/seekcoalescer.h`:
  ```cpp
  #ifndef SEEKCOALESCER_H
  #define SEEKCOALESCER_H

  #include <cstdint>

  // Pure logic for coalescing a burst of scrub targets. UIManager owns the timer
  // and the actual worker->seekTo call; this class just decides "seek now vs.
  // defer the latest target". The first offer of a gesture seeks immediately;
  // subsequent offers are deferred (only the latest survives) until takePending()
  // drains them; reset() ends the gesture so the next first-offer is immediate.
  class SeekCoalescer {
  public:
      // Returns true iff the caller should seek to `target` NOW.
      bool offer(int64_t target) {
          if (!m_armed) {
              m_armed = true;
              m_hasPending = false;
              return true;
          }
          m_pending = target;
          m_hasPending = true;
          return false;
      }

      // Drains the latest deferred target. Sets `has` to whether one was pending.
      int64_t takePending(bool& has) {
          has = m_hasPending;
          m_hasPending = false;
          return m_pending;
      }

      // End the gesture: the next offer() seeks immediately again.
      void reset() {
          m_armed = false;
          m_hasPending = false;
      }

  private:
      bool m_armed = false;
      bool m_hasPending = false;
      int64_t m_pending = 0;
  };

  #endif // SEEKCOALESCER_H
  ```

- [ ] **Step 5: Confirm the test lib compiles the new header.** `olr_test_playback` links via the existing `#include`; `seekcoalescer.h` is header-only so it needs no `.cpp` in `tests/CMakeLists.txt`. Run `cmake --build build --target tst_seekcoalescer && ctest --test-dir build -R tst_seekcoalescer --output-on-failure`. Expect all four slots pass.

- [ ] **Step 6: Add the UIManager members + slot decl.** In `uimanager.h`, add the include after line 21:
  ```cpp
  #include "playback/seekcoalescer.h"
  ```
  Add the slot declaration in the private slots / private methods area (near `seekPlayback`'s siblings, after the `Q_INVOKABLE void seekPlayback(int64_t ms);` at line 256 — declare it as a private method):
  ```cpp
  private:
      void commitPendingScrub();
  ```
  Add members after line 360 (`AudioPlayer *m_audioPlayer = nullptr;`):
  ```cpp
      SeekCoalescer m_seekCoalescer;
      QTimer m_scrubCoalesceTimer;
      static constexpr int kScrubCoalesceMs = 16; // ~one frame at 60fps
  ```

- [ ] **Step 7: Wire the timer in the UIManager constructor.** In `uimanager.cpp`, locate the constructor (search `UIManager::UIManager`) and add after the member-init block:
  ```cpp
      m_scrubCoalesceTimer.setSingleShot(true);
      m_scrubCoalesceTimer.setTimerType(Qt::PreciseTimer);
      m_scrubCoalesceTimer.setInterval(kScrubCoalesceMs);
      connect(&m_scrubCoalesceTimer, &QTimer::timeout, this, &UIManager::commitPendingScrub);
  ```

- [ ] **Step 8: Route seekPlayback through the coalescer + drop the redundant audio clear.** Replace `uimanager.cpp:1693-1703` (`seekPlayback`):
  ```cpp
  void UIManager::seekPlayback(int64_t ms) {
      // Disable live-follow on a manual scrub; the user re-enables via "Live".
      setFollowLive(false);
      // Coalesce a burst of scrub targets: seek immediately on the first move of
      // a gesture, then commit only the latest target on a single-shot timer.
      if (m_seekCoalescer.offer(ms)) {
          if (m_transport) m_transport->seek(ms);
          if (m_playbackWorker) m_playbackWorker->seekTo(ms);
      } else {
          // A seek is already in flight; arm/refresh the coalesce timer. The
          // worker's own reposition handles audio re-priming (repositionTo clears
          // + re-primes the AudioPlayer), so no per-move audioPlayer->clear() here.
          if (!m_scrubCoalesceTimer.isActive()) m_scrubCoalesceTimer.start();
      }
  }

  void UIManager::commitPendingScrub() {
      bool has = false;
      const int64_t ms = m_seekCoalescer.takePending(has);
      if (!has) return;
      if (m_transport) m_transport->seek(ms);
      if (m_playbackWorker) m_playbackWorker->seekTo(ms);
  }

  void UIManager::endScrubGesture() {
      // Called on slider release: commit the final target immediately, then end
      // the gesture so the next gesture's first move seeks without delay.
      m_scrubCoalesceTimer.stop();
      commitPendingScrub();
      m_seekCoalescer.reset();
  }
  ```
  Add `Q_INVOKABLE void endScrubGesture();` to `uimanager.h`'s public invokables (next to `seekPlayback` at line 256) and its decl removal of the standalone `commitPendingScrub` private note above is already covered.

- [ ] **Step 9: Wire the QML.** In `Main.qml`, replace the `onMoved` block (lines 953-955) and add `onPressedChanged`:
  ```qml
                      onMoved: {
                          appWindow.uiManagerRef.seekPlayback(value)
                      }
                      onPressedChanged: {
                          // On release (pressed → false) flush the final scrub
                          // target and end the coalesce gesture.
                          if (!scrubBar.pressed) {
                              appWindow.uiManagerRef.endScrubGesture()
                          }
                      }
  ```

- [ ] **Step 10: Build the app target.** Run `cmake --build build` (full app build, since `uimanager.cpp`/`Main.qml` are app-side). Expect clean build.

- [ ] **Step 11: Re-run the slider E2E (the worker-side seek path is unchanged).** Run `ctest --test-dir build -R "e2e_play_sliderscrub" --output-on-failure`. Expect PASS (sliderscrub reposition<=20). The harness drives `worker.seekTo` directly (not through UIManager), so the coalescer does not change the E2E counts — this confirms no worker-side regression. UI coalescing itself is verified by `tst_seekcoalescer` (logic) + manual scrub (Step 12).

- [ ] **Step 12: Manual verification note.** Manual check (no automated UI harness exists): launch the app, scrub the slider fast — the preview must track the drag without gray flashes (Task 1/2) and without audio machine-gunning (Task 7). Document the result in the PR description.

- [ ] **Step 13: Format + commit.** `xcrun clang-format -i uimanager.cpp uimanager.h playback/seekcoalescer.h tests/unit/tst_seekcoalescer.cpp`. Commit:
  ```
  git commit -m "ui: coalesce scrub seeks (SeekCoalescer + single-shot timer)

  Seek immediately on first move + on release; commit only the latest target on a
  16ms PreciseTimer in between. SeekCoalescer holds the pure logic (unit-tested);
  UIManager owns the timer. Drops the redundant per-move audioPlayer->clear().

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 7 — setMuted(true) → fadeOutAndClear (de-click mute)

`AudioPlayer::setMuted(true)` hard-clears the ring buffer (`m_ringBuffer->clear()`), cutting the waveform mid-sample → an audible click. `AudioRingBuffer::fadeOutAndClear(channels)` already exists (used by `AudioPlayer::clear()`); use it on the mute transition too.

**Files:**
- Modify `playback/audioplayer.cpp:375-390` (`setMuted`).
- Test: a focused Qt Test on the ring-buffer fade path if feasible. `AudioRingBuffer` is in `olr_test_playback`. The fade is observable on the buffer contents (the tail is ramped to zero before discard) without a real `QAudioSink`. If `AudioRingBuffer`'s buffer is not directly inspectable, document the limitation and rely on `clear()`'s existing fade coverage.

**Interfaces:** none changed (behavior of `setMuted(true)` only).

- [ ] **Step 1: Inspect AudioRingBuffer testability.** Read `playback/audioplayer.cpp` around `fadeOutAndClear` and `readData`. The ring buffer's data is private (`m_buf`) with only `push`/`bytesAvailable`/`readData` public. Determine whether a test can observe the fade: push a constant non-zero PCM block, call `fadeOutAndClear(2)`, then `read` — if `readData` returns the faded tail (ramped) the fade is observable; if `fadeOutAndClear` discards immediately the only observable is `bytesAvailable()==0`.

- [ ] **Step 2: Make the minimal change.** In `playback/audioplayer.cpp`, replace the mute branch at lines 379-381:
  ```cpp
      if (muted && m_ringBuffer) {
          // De-click: ramp the remaining buffered audio to zero before discarding
          // it, instead of a hard cut that pops. fadeOutAndClear is the same
          // de-click path clear() uses.
          m_ringBuffer->fadeOutAndClear(m_channels);
      }
  ```
  (Leave the unmute branch unchanged: on unmute it re-arms the fade-in and `clear()`s the empty buffer.)

- [ ] **Step 3: Write a focused ring-buffer fade test IF observable (else skip with a documented note).** If Step 1 found the fade observable via `readData`, add to a new `tests/unit/tst_audioplayer_mutefade.cpp` (register with `olr_add_unit_test(tst_audioplayer_mutefade olr_test_playback)`):
  ```cpp
  #include <QtTest>

  #include "playback/audioplayer.h"

  class TestAudioPlayerMuteFade : public QObject {
      Q_OBJECT
  private slots:
      void muteRampsTailToZeroNotHardCut();
  };

  void TestAudioPlayerMuteFade::muteRampsTailToZeroNotHardCut() {
      AudioRingBuffer ring;
      ring.open(QIODevice::ReadWrite);
      // 200 frames of full-scale stereo S16 (non-zero everywhere).
      QByteArray loud(200 * 2 * int(sizeof(qint16)), char(0x7F));
      ring.push(loud.constData(), loud.size());
      ring.fadeOutAndClear(2);
      // After a fade-out-and-clear the buffer is drained to silence: either no
      // bytes remain, or the residual tail has been ramped (last sample << first).
      QByteArray out(loud.size(), char(0x55));
      const qint64 n = ring.read(out.data(), out.size());
      // readData pads with silence; the faded region must not be the original
      // full-scale value verbatim at the splice point.
      QVERIFY(n >= 0);
      QCOMPARE(uchar(out.at(out.size() - 1)), uchar(0x00)); // tail is silence
  }

  QTEST_GUILESS_MAIN(TestAudioPlayerMuteFade)
  #include "tst_audioplayer_mutefade.moc"
  ```
  **Limitation note:** `QAudioSink` is a no-op under `QT_QPA_PLATFORM=offscreen`, so this test exercises only the `AudioRingBuffer` fade path (the part that produces the de-click), not end-to-end audio output. If Step 1 found the fade is NOT observable through the public API (the buffer is discarded synchronously with no readable residue), DO NOT add the test — instead record in the commit body: "ring-buffer fade not observable via public API under offscreen; mute now reuses the same fadeOutAndClear path clear() already covers."

- [ ] **Step 4: Build + test.** Run `cmake --build build --target olr_test_playback`. If the test was added: `cmake --build build --target tst_audioplayer_mutefade && ctest --test-dir build -R tst_audioplayer_mutefade --output-on-failure` → expect PASS. Also run `ctest --test-dir build -R "e2e_play_(storm|seekplay)" --output-on-failure` to confirm the audio path still pushes (`audioPushes>0`).

- [ ] **Step 5: Format + commit.** `xcrun clang-format -i playback/audioplayer.cpp` (+ the test file if added). Commit:
  ```
  git commit -m "audio: de-click mute (setMuted(true) fades out instead of hard clear)

  setMuted(true) hard-cleared the ring buffer mid-sample → audible click. Reuse
  fadeOutAndClear (the de-click path clear() already uses).

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
  ```

---

## Task 8 — Remove the redundant per-move audioPlayer->clear() in seekPlayback

**Confirmed present.** `uimanager.cpp:1702` is `if (m_audioPlayer) m_audioPlayer->clear();` inside `seekPlayback`. It is redundant because the worker's `repositionTo` already clears + re-primes the AudioPlayer on every full reposition (`playbackworker.cpp:571`) and on backward reuse (`:557`). Clearing again from the UI thread on *every* scrub move (a) double-clears (extra `clearCount`/fade churn) and (b) runs a clear even on in-window reuse-seeks that the worker would handle without an audio reset. **This task is folded into Task 6 Step 8** (the rewritten `seekPlayback` no longer calls `m_audioPlayer->clear()`), so it does not get its own commit — but it is called out here per the audit so the implementer verifies the removal landed.

**Files:**
- Verified at `uimanager.cpp:1702` (removed as part of Task 6 Step 8).

**Interfaces:** none.

- [ ] **Step 1: Confirm the line existed and is now gone.** After Task 6 lands, run `grep -n "m_audioPlayer->clear" uimanager.cpp`. Expect: no hit inside `seekPlayback` (the only legitimate `m_audioPlayer->clear()` callers are e.g. stop/source-change paths, NOT the per-move scrub). If a hit remains in `seekPlayback`, delete it and fold into the Task 6 commit.

- [ ] **Step 2: Confirm no audio regression.** Run `ctest --test-dir build -R "e2e_play_(seekplay|sliderscrub)" --output-on-failure`. Expect PASS with `audioPushes>0` — the worker still re-primes audio on reposition, so removing the UI-side double-clear does not silence playback.

---

## Parallelization within Tier 1

**Fully independent (different files, no shared edits) — can run concurrently by different workers:**
- **Task 2** (`outputdispatcher.{h,cpp}` + new `tst_outputdispatcher_holdlast.cpp`) — independent of the worker.
- **Task 7** (`audioplayer.cpp` + optional new test) — independent.
- **Task 6** (`seekcoalescer.h`, `uimanager.{h,cpp}`, `Main.qml`, `tst_seekcoalescer.cpp`) — independent of the worker/dispatcher/audio. (Task 8 is folded into it.)

**Serialized — all touch `playback/playbackworker.cpp` and must land in order to avoid edit conflicts:**
- **Task 1** (rename + drop cache clear) → **Task 4** (deliver-target-first in `repositionTo`) → **Task 5** (deliverDueFrames dead-work) → **Task 3** (`convertToMediaVideoFrame` alloc). Task 3 is order-independent in *behavior* but edits the same file, so it is sequenced last to keep merges clean. Do Task 1 first because Task 4 reasons about the cache-no-longer-cleared behavior and the `seekflash` gate (Task 2) is the regression guard both rely on.

**Shared test infra (coordinate, do not edit concurrently):**
- `tests/unit/CMakeLists.txt` is appended by **Task 2**, **Task 6**, and (optionally) **Task 7** — append in task order to avoid line conflicts.
- `tests/e2e/play_harness.cpp`, `run_playback_e2e.sh`, `tests/e2e/CMakeLists.txt` are owned solely by **Task 2** (the `seekflash` gate). **Task 4** only adds a comment to `run_playback_e2e.sh`, so it must land after Task 2.

**Recommended wave plan:**
- Wave A (parallel): Task 1, Task 2, Task 6 (+8), Task 7.
- Wave B (after Task 1 + Task 2): Task 4.
- Wave C (after Task 4): Task 5.
- Wave D (after Task 5): Task 3.
