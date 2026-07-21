# Timecode Carrier Epoch Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind software, native CPU, and GPU timecode evidence and start metadata to immutable source carriers and accepted packet PTS across every reset race.

**Architecture:** `StreamWorker` owns one immutable active carrier token `{sessionIdentity, epoch}` and one bounded `DecodedFrameEvidenceQueue`. Every frame submission snapshots the active token; all encoder outputs map through actual packet PTS into one epoch-aware packet-write helper. `Muxer` records start-timecode candidates only after packet acceptance and the writer publishes them outside the queue mutex before deferred header commit.

**Tech Stack:** C++17, Qt 6 signals/threads, FFmpeg codec/mux APIs, Qt Test, CMake/CTest, MinGW Windows guarded launcher.

## Global Constraints

- Never execute a raw Windows test binary; use `D:/Development/OpenLiveReplay-test-harness-windows/tools/run_ctest.py` after obtaining the exclusive runtime lane.
- Preserve the integer session frame axis: encoder submission PTS remains `m_internalFrameCount` and MPEG-2 packet matching happens before time-base rescale.
- Reuse the single bounded `DecodedFrameEvidenceQueue`; do not create codec-specific evidence queues or parallel reset generations.
- Every invalidation publishes a new immutable carrier token. Only GPU fallback retains the live session identity; disconnect, changeSource, session iteration, and stop rotate it.
- Never hold carrier/evidence/frame mutexes across encoder calls, mux queue waits, disk writes, Qt signals, or callbacks.
- Use targeted `git add <paths>` only, include `Co-Authored-By: Claude <noreply@anthropic.com>`, do not push.

---

### Task 1: Immutable StreamWorker Carrier Authority

**Files:**
- Modify: `recorder_engine/streamworker.h`
- Modify: `recorder_engine/streamworker.cpp`
- Test: `tests/unit/tst_replaymanager_timecode.cpp`
- Test: `tests/unit/tst_streamworker_gpuencode.cpp`

**Interfaces:**
- Produces: `uint64_t StreamWorker::sourceCarrierEpoch() const`, immutable `SourceCarrierToken`, `rotateSourceCarrier(bool retainSessionIdentity)`, and epoch-stamped `QueuedFrame`/latest-frame state.
- Consumes: `DecodedVideoFrame::timecodeEvidence`, capture-loop session boundaries, disconnect/changeSource/fallback/stop invalidations.

- [ ] **Step 1: Add carrier reset/race tests**

Add tests that enqueue an old callback frame after `setConnected(false)` but before a new session, replace a non-empty URL, and latch GPU fallback while old queued/latest carriers exist. Use the same producer `sourceGeneration` before and after reset and assert only the new carrier can submit evidence.

```cpp
const uint64_t oldSession = worker.beginCarrierSessionForTest();
worker.enqueueDecodedVideoFrame(oldFrame, oldSession);
worker.setConnected(false);
worker.enqueueDecodedVideoFrame(lateOldFrame, oldSession);
QCOMPARE(worker.m_frameQueue.size(), 0);
QVERIFY(worker.sourceCarrierEpoch() > oldEpoch);
```

- [ ] **Step 2: Run RED through the guarded launcher**

Run after requesting the runtime lane:

```powershell
python D:/Development/OpenLiveReplay-test-harness-windows/tools/run_ctest.py --test-dir build/timecode -R "^(tst_replaymanager_timecode|tst_streamworker_gpuencode)$" --output-on-failure
```

Expected: new tests fail because queued/latest frames have no carrier epoch and old callbacks are accepted.

- [ ] **Step 3: Implement immutable token admission**

Add one token type and one active pointer:

```cpp
struct SourceCarrierToken {
    uint64_t sessionIdentity = 0;
    uint64_t epoch = 0;
};
using SourceCarrier = std::shared_ptr<const SourceCarrierToken>;
```

Each capture-loop iteration rotates both values and captures only `sessionIdentity`. Frame entry locks the short carrier mutex, verifies that identity against the active token, applies any evidence identity/discontinuity rotation, and returns the active immutable token snapshot. GPU fallback rotates only `epoch`; disconnect/changeSource/stop rotate both. Stamp `carrierEpoch` on `QueuedFrame`, CPU/GPU latest state, and every encoder submission. Drop stale queued/latest frames by comparing with the atomic current epoch.

- [ ] **Step 4: Run GREEN focused tests**

Run the same guarded selector. Expected: carrier reset/race tests pass and existing queue/fallback tests remain green.

### Task 2: Receipt-Time ReplayManager Epoch Validation

**Files:**
- Modify: `recorder_engine/streamworker.h`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `recorder_engine/replaymanager.h`
- Modify: `recorder_engine/replaymanager.cpp`
- Test: `tests/unit/tst_replaymanager_timecode.cpp`

**Interfaces:**
- Produces: `frameTimecode(int sourceIndex, quint64 carrierEpoch, TimecodeEvidence evidence)` and matching ReplayManager slot.
- Consumes: `StreamWorker::sourceCarrierEpoch()` at queued-event receipt.

- [ ] **Step 1: Add a queued post-reset rejection test**

Queue a valid emission, rotate the worker carrier before the receiver event loop drains, and assert `ReplayManager` never anchors it. Then emit under the current epoch and assert it anchors normally.

```cpp
emit worker.frameTimecode(0, oldEpoch, oldEvidence);
worker.rotateSourceCarrierForTest(false);
QCoreApplication::processEvents();
QVERIFY(!manager.sourcesFrameAligned(0, 1));
```

- [ ] **Step 2: Verify RED**

Run `tst_replaymanager_timecode` through the guarded launcher. Expected: the old queued event is accepted because the signal carries no carrier authority.

- [ ] **Step 3: Add epoch to the queued signal and validate at receipt**

Use Qt's built-in `quint64` metatype. At receipt, reject zero/stale production epochs when a live worker exists:

```cpp
if (sourceIndex < m_workers.size() && m_workers[sourceIndex] &&
    m_workers[sourceIndex]->sourceCarrierEpoch() != carrierEpoch)
    return;
```

Keep the direct unit seam explicit when no worker exists. Do not retain references to signal arguments; Qt owns a copied `TimecodeEvidence` until delivery.

- [ ] **Step 4: Verify GREEN**

Run `tst_replaymanager_timecode` through the guarded launcher. Expected: stale queued events are rejected and existing rate/reset/servo tests pass.

### Task 3: Shared Packet-PTS Completion Including MPEG-2

**Files:**
- Modify: `recorder_engine/streamworker.h`
- Modify: `recorder_engine/streamworker.cpp`
- Test: `tests/unit/tst_replaymanager_timecode.cpp`

**Interfaces:**
- Produces: one `writeEncodedPacket(...)` helper used by native/GPU callbacks and MPEG-2 receive.
- Consumes: `enqueueMuxFrameEvidence(inputPts, sourceTimecode100ns, evidence, carrierEpoch)` and `takeMuxFrameEvidence(actualPacketPts)`.

- [ ] **Step 1: Add delayed software packet tests**

Add a production-helper test that enqueues evidence for input PTS 10, advances latest state to PTS 11, then supplies a received packet whose pre-rescale PTS is 10. Assert evidence 10 emits. Add a hook that rotates the carrier after `takeForOutputPts(10)` but before `Muxer::writePacket`; assert no stale emission.

- [ ] **Step 2: Verify RED**

Run `tst_replaymanager_timecode` through the guarded launcher. Expected: software output consumes the newest latest-frame evidence instead of the packet's input evidence, and reset-after-match emits stale evidence.

- [ ] **Step 3: Enqueue on software input and take on actual output PTS**

Before `avcodec_send_frame`, consume/stamp current evidence and enqueue under `m_latestFrame->pts`. On send failure discard and restore it. On receive, save `outPkt->pts` before `av_packet_rescale_ts`, take the matching queue entry, then call the same epoch-aware packet helper used by H.264.

```cpp
const int64_t encodedPts = outPkt->pts;
av_packet_rescale_ts(outPkt, encCtx->time_base, st->time_base);
havePacket = writeEncodedPacket(outPkt, encodedPts, track, st, &havePacket, {});
```

The completion captures only copied evidence and immutable epoch. It rechecks the current epoch before emitting and forwards exactly one completion result.

- [ ] **Step 4: Verify GREEN**

Run `tst_replaymanager_timecode`, `tst_decodedframeevidencequeue`, and `tst_nativevideoencoder` through the guarded launcher.

### Task 4: Production-Shaped GPU Delayed Output

**Files:**
- Test: `tests/unit/tst_streamworker_gpuencode.cpp`
- Modify if required by RED: `recorder_engine/streamworker.cpp`

**Interfaces:**
- Consumes: real `GpuEncodePump`, real asynchronous `Muxer`, immutable carrier epochs, shared packet helper.
- Produces: regression proof for delayed old PTS and in-flight fallback rejection.

- [ ] **Step 1: Add delayed surface encoder integration tests**

Create a surface encoder where call N buffers its PTS and call N+1 emits N through N+1's callback. Use an initialized real Muxer. Assert N's evidence is emitted with N's arrival frame. In a second test, block call N+1, rotate via `latchGpuEncodeCpuFallback()`, release it, and assert its old packet cannot emit evidence or set start metadata. Retain the existing no-packet queue-concurrency fake unchanged.

- [ ] **Step 2: Verify RED then GREEN**

Run `tst_streamworker_gpuencode` through the guarded launcher before and after the minimal production correction. Expected RED: delayed evidence is absent/wrong or stale fallback output emits. Expected GREEN: both production-shaped tests pass.

### Task 5: Accepted-Packet Start-Timecode Selection

**Files:**
- Modify: `recorder_engine/muxer.h`
- Modify: `recorder_engine/muxer.cpp`
- Modify: `recorder_engine/streamworker.cpp`
- Test: `tests/unit/tst_muxer.cpp`
- Test: `tests/unit/tst_replaymanager_timecode.cpp`

**Interfaces:**
- Produces: `Muxer::writePacket(AVPacket*, PacketWriteCallback, const QString& acceptedStartTimecodeCandidate)`.
- Consumes: candidate generated from the matched `DecodedFrameEvidence::sourceTimecode100ns`.

- [ ] **Step 1: Add candidate acceptance/order tests**

Test three cases with real Muxer output: a rejected packet candidate followed by an accepted candidate; two ordered producer threads whose earlier accepted candidate wins; and an earlier accepted no-candidate packet followed inside grace by a candidate packet. Reopen the MKV and assert the expected format/video `timecode` tag.

- [ ] **Step 2: Verify RED**

Run `tst_muxer` and `tst_replaymanager_timecode` through the guarded launcher. Expected: rejected StreamWorker packets can pre-register a candidate and later candidate visibility/order is not tied to acceptance.

- [ ] **Step 3: Move candidate ownership into accepted queue state**

After clone/capacity checks, push `QueuedPacket` and record the first valid candidate under `m_qMutex`. In `writerLoop`, on every grace pass snapshot the accepted candidate under `m_qMutex`, unlock, call `setStartTimecodeCandidate`, then call `headerWriteDeferred`/`ensureHeaderWritten` with no queue lock held. Remove every per-frame StreamWorker call to `setStartTimecodeCandidate`; configured `init(..., startTimecode)` metadata remains first-wins.

- [ ] **Step 4: Verify GREEN and lock order**

Run the two tests through the guarded launcher. Inspect every `m_qMutex`/`m_headerMutex` call site and confirm no path holds one while acquiring the other.

### Task 6: Final Verification, Commit, and Fresh Review

**Files:**
- Modify: `docs/superpowers/specs/2026-07-15-timecode-carrier-epoch-design.md`
- Create: `docs/superpowers/plans/2026-07-15-timecode-carrier-epoch-review-fixes.md`
- Modify: only implementation/test files listed above.

**Interfaces:**
- Consumes: all Tasks 1-5.
- Produces: verified targeted commits and independent adversarial review result.

- [ ] **Step 1: Build focused and production targets**

```powershell
cmake --build build/timecode --target tst_muxer tst_replaymanager_timecode tst_decodedframeevidencequeue tst_nativevideoencoder tst_streamworker_gpuencode tst_gpuencodepump --parallel 2
cmake --build build/timecode-production --target OpenLiveReplay --parallel 2
```

- [ ] **Step 2: Run focused guarded tests**

```powershell
python D:/Development/OpenLiveReplay-test-harness-windows/tools/run_ctest.py --test-dir build/timecode -R "^(tst_muxer|tst_replaymanager_timecode|tst_decodedframeevidencequeue|tst_nativevideoencoder|tst_streamworker_gpuencode|tst_gpuencodepump)$" --output-on-failure
```

- [ ] **Step 3: Static verification**

Run `git clang-format --diff HEAD`, `git diff --check`, verify production `streamworker.cpp` has no `OLR_UNIT_TEST`, and inspect `git status --short` for exact scope.

- [ ] **Step 4: Targeted commits**

Stage only named files and commit with `Co-Authored-By: Claude <noreply@anthropic.com>`. Do not cherry-pick unrelated GPU/transport/MF prerequisites and do not push.

- [ ] **Step 5: Fresh independent adversarial review**

Request a fresh-context review of immutable token/session authority, MPEG packet PTS before rescale, ReplayManager receipt validation, GPU fallback in-flight output, candidate acceptance order/header grace, lock order, and hot-path cost. Fix every actionable finding test-first and repeat verification.
