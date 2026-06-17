# Frame-Sync Phase 1 — Unified Timing Core + Clock Recovery + Drift Servo — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Recover each source's sender clock into a testable `SourceClock`, estimate its rate skew with a pure `DriftEstimator`, expose the ppm/quality as telemetry, and run an audio drift servo so A/V stays locked and there is **no accumulating drift over a multi-hour show** — verified by the Phase-0 rig's `skew_injector`.

**Architecture:** Generalize the existing per-session A/V anchors (SRT PCR / RTMP FLV — the AUD-4 model) into a `SourceClock` abstraction + a pure `DriftEstimator`, **preserving current mapping behavior** (TDD-locked first) and adding the rate estimate + correction. Because audio is already 48 kHz from the decoder (no `swr` to compensate), the servo lives in StreamWorker's audio FIFO **cursor** (consume rate-corrected sample counts). Telemetry rides the existing `IngestStats`/`reportStats` pipe (#51).

**Tech Stack:** C++17, Qt6, FFmpeg (AVFrame only), Qt Test.

## Global Constraints
- Spec: `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md` (Phase 1). Depends on Phase 0 (the rig) for acceptance.
- Worktree `/tmp/olr-bcast`. Build dir `build/bcast`. Format only changed C++ lines (`git clang-format`, llvm `/opt/homebrew/opt/llvm/bin`); CI lint = clang-format 22.1.7; engine `.cpp` are hand-Allman.
- **Behavior-preserving first:** every existing anchor mapping must stay byte-identical until a step deliberately changes it; the `framesync`/`av-sync`/native gates must stay green.
- **No `swr` in the engine** — the audio servo is new FIFO-cursor code, not `swr_set_compensation`.
- Verified anchors (from the map): `sourcePtsMsFromAnchor(pts90k, anchorTs90k, anchorStreamMs)` is a pure static at `nativesrtingestsession.h:33` / `.cpp:637-643` (untested); SRT anchor set at `.cpp:401-410`, `sourcePtsMsForUnit` `.cpp:605-635`, `sourcePtsMsForAudio` `.cpp:645-678`; RTMP map **inlined** at `nativertmpingestsession.cpp:1032-1051`/`:1053-1077` (`m_anchorMediaMs`/`m_anchorStreamTimeMs`, ms units); audio FIFO `enqueueAudio` `streamworker.cpp:520-565` + `writeAudioForTick` `:567-648` (`m_audioWriteCursor`, `kAudioSampleRate=48000`); `IngestStats` `ingestsession.h:56-68`; `RecordingClock::elapsedMs()`; `heartbeatFrameSpan` (pure, `tst_heartbeat.cpp`).

---

### Task 1: Lock the existing pure anchor map under test (safety net)

`sourcePtsMsFromAnchor` is pure + static but **has no test**. Lock its current behavior before anything generalizes it.

**Files:** Modify `tests/unit/tst_ingestbackendselector.cpp` (already friends the SRT/RTMP sessions and tests `sharedAnchorMapsStreamTime` at `:208`).

- [ ] **Step 1: Add tests** for `NativeSrtIngestSession::sourcePtsMsFromAnchor`: identity at anchor (`pts90k==anchorTs90k` → `anchorStreamMs`); +1 s of 90 kHz ticks (`+90000`) → `anchorStreamMs+1000`; negative inputs → `-1`. Run `( cd build/bcast && ctest -R tst_ingestbackendselector --output-on-failure )` → PASS (locks current behavior).
- [ ] **Step 2: Commit** — `git commit -m "test(timing): lock sourcePtsMsFromAnchor behavior"`.

---

### Task 2: `DriftEstimator` — pure windowed least-squares ppm

**Files:** Create `recorder_engine/timing/driftestimator.h`, `recorder_engine/timing/driftestimator.cpp`, `tests/unit/tst_driftestimator.cpp`; modify `tests/CMakeLists.txt` (add to `olr_test_core`), `tests/unit/CMakeLists.txt`, root `CMakeLists.txt`.

**Interfaces — Produces:** `class DriftEstimator { void addSample(int64_t senderNs, int64_t sessionNs); bool locked() const; double ppm() const; int64_t offsetNs() const; void reset(); }`.

- [ ] **Step 1: Failing test** `tests/unit/tst_driftestimator.cpp`:
```cpp
#include <QtTest>
#include "recorder_engine/timing/driftestimator.h"

class TestDriftEstimator : public QObject {
    Q_OBJECT
private slots:
    void unlockedUntilEnoughSamples();
    void zeroDriftIsZeroPpm();
    void positiveSkewMeasured();
    void negativeSkewMeasured();
    void resetClears();
};

// Synthetic: session advances at exactly 1.0; sender advances at (1 + ppm/1e6).
static void feed(DriftEstimator& e, double ppm, int n, int64_t stepNs = 33'366'667 /*~29.97 frame*/) {
    for (int i = 0; i < n; ++i) {
        const int64_t sessionNs = int64_t(i) * stepNs;
        const int64_t senderNs = int64_t(llround(double(i) * stepNs * (1.0 + ppm / 1e6)));
        e.addSample(senderNs, sessionNs);
    }
}
void TestDriftEstimator::unlockedUntilEnoughSamples() {
    DriftEstimator e; e.addSample(0, 0); e.addSample(1000, 1000);
    QVERIFY(!e.locked());
}
void TestDriftEstimator::zeroDriftIsZeroPpm() {
    DriftEstimator e; feed(e, 0.0, 300);
    QVERIFY(e.locked());
    QVERIFY(qAbs(e.ppm()) < 1.0);   // within 1 ppm of zero
}
void TestDriftEstimator::positiveSkewMeasured() {
    DriftEstimator e; feed(e, 200.0, 300);
    QVERIFY(qAbs(e.ppm() - 200.0) < 5.0);   // sender runs +200 ppm fast
}
void TestDriftEstimator::negativeSkewMeasured() {
    DriftEstimator e; feed(e, -120.0, 300);
    QVERIFY(qAbs(e.ppm() + 120.0) < 5.0);
}
void TestDriftEstimator::resetClears() {
    DriftEstimator e; feed(e, 200.0, 300); e.reset();
    QVERIFY(!e.locked());
}
QTEST_GUILESS_MAIN(TestDriftEstimator)
#include "tst_driftestimator.moc"
```
- [ ] **Step 2: Register** in `tests/unit/CMakeLists.txt`: `olr_add_unit_test(tst_driftestimator   olr_test_core)`. Verify RED (`driftestimator.h` missing).
- [ ] **Step 3: Implement** `recorder_engine/timing/driftestimator.h`:
```cpp
#ifndef DRIFTESTIMATOR_H
#define DRIFTESTIMATOR_H
#include <cstdint>
#include <vector>

// Windowed least-squares estimate of a source's sender-clock rate skew vs the
// session clock. Pure (no Qt/FFmpeg). slope of (senderNs vs sessionNs); ppm =
// (slope - 1) * 1e6. Locks after kMinSamples observations.
class DriftEstimator {
public:
    explicit DriftEstimator(int windowSize = 256, int minSamples = 64);
    void addSample(int64_t senderNs, int64_t sessionNs);
    bool locked() const { return int(m_buf.size()) >= m_minSamples; }
    double ppm() const;       // 0 until locked
    int64_t offsetNs() const; // sessionNs - senderNs at the window centroid; 0 until locked
    void reset();
private:
    struct Pt { int64_t sender; int64_t session; };
    void recompute() const;
    int m_window, m_minSamples;
    std::vector<Pt> m_buf; // ring (most-recent window)
    size_t m_head = 0;
    mutable bool m_dirty = true;
    mutable double m_slope = 1.0;
    mutable int64_t m_offset = 0;
};
#endif
```
`driftestimator.cpp`: ring-buffer insert (cap at `m_window`, overwrite oldest); `recompute()` does double-precision least-squares over the buffer **relative to the first point** (subtract `buf[0]` from both axes to keep the sums small and numerically stable), `slope = (n*Sxy - Sx*Sy)/(n*Sxx - Sx*Sx)` (guard denominator==0 → slope=1), `ppm = (slope-1)*1e6`, `offset = mean(session) - slope*mean(sender)` mapped back through the first point. `reset()` clears the buffer.
- [ ] **Step 4: Wire `driftestimator.cpp` into both targets** — `tests/CMakeLists.txt` `olr_test_core` source list + root `CMakeLists.txt` engine sources (next to `heartbeat.cpp`). Build + run → 5/5 PASS.
- [ ] **Step 5: Commit** — `git commit -m "feat(timing): DriftEstimator — windowed least-squares ppm (unit-tested)"`.

---

### Task 3: `SourceClock` abstraction + PCR/FLV/Arrival implementations

Generalize the per-session anchor into a testable interface that maps sender timestamps → recording-ms **identically to today**, while feeding the `DriftEstimator`.

**Files:** Create `recorder_engine/timing/sourceclock.h`, `.cpp`, `tests/unit/tst_sourceclock.cpp`; modify the two ingest sessions + CMake.

**Interfaces — Produces:**
```cpp
enum class ClockQuality { Arrival = 0, FlvPll = 1, Ndi = 2, Pcr = 3, Reference = 4 };
class SourceClock {
public:
    virtual ~SourceClock() = default;
    // Establish/maintain the anchor. senderMs = the source media time (PCR/FLV/NDI mapped to ms);
    // sessionNowMs = RecordingClock at this observation; discontinuity forces re-anchor.
    virtual void observe(int64_t senderMs, int64_t sessionNowMs, bool discontinuity) = 0;
    virtual int64_t toSessionMs(int64_t mediaSenderMs) const = 0; // -1 if unlocked
    virtual ClockQuality quality() const = 0;
    virtual double ppm() const = 0;     // from the embedded DriftEstimator (0 until locked)
    virtual bool locked() const = 0;
};
```
A single concrete `AnchoredSourceClock` implements the shared-anchor logic (whichever stream first establishes it; video owns re-anchoring on a jump; audio follows), parameterised by `ClockQuality` + the jump thresholds — this is exactly the SRT/RTMP behavior, unified. `toSessionMs` = `anchorSessionMs + (mediaSenderMs - anchorSenderMs)` (the same arithmetic as `sourcePtsMsFromAnchor` after the 90k→ms conversion). Every `observe` also feeds `DriftEstimator::addSample(mediaSenderMs*1e6, sessionNowMs*1e6)`.

- [ ] **Step 1: Failing test** `tst_sourceclock.cpp`: an `AnchoredSourceClock` (quality `Pcr`): first `observe(1000, 5000, false)` → `toSessionMs(1000)==5000`; `toSessionMs(1100)==5100`; a forward jump `observe(9000, 6000, false)` with prev establishing `m_prev` → re-anchors so `toSessionMs(9000)==6000`; a `discontinuity` re-anchors against the new `sessionNowMs`; before any observe `toSessionMs(x)==-1`. After feeding a +200 ppm synthetic train, `ppm()≈200`. Register `olr_add_unit_test(tst_sourceclock olr_test_core)`. Verify RED.
- [ ] **Step 2: Implement** `AnchoredSourceClock` in `sourceclock.{h,cpp}` mirroring `NativeRtmpIngestSession::sourcePtsMsForVideo/Audio` (the cleanest, ms-native version): `m_anchorSenderMs`, `m_anchorSessionMs`, `m_prevSenderMs`, the `kForwardJumpMs=3000`/`kBackwardToleranceMs=-200` re-anchor, plus a `DriftEstimator m_drift`. Build + run → PASS.
- [ ] **Step 3: Adopt in `NativeRtmpIngestSession`.** Replace the inlined `m_anchorMediaMs`/`m_anchorStreamTimeMs` arithmetic (`:1032-1077`) with an owned `AnchoredSourceClock m_clock{ClockQuality::FlvPll}`: `sourcePtsMsForVideo(dts,pts)` → `m_clock.observe(dts, recordingClockMs(), false); return m_clock.toSessionMs(pts);` (audio: `observe` with the audio-follows flag — keep audio from owning re-anchoring exactly as today). **Verify byte-identical** behavior: `( cd build/bcast && ctest -L native-rtmp )` + `tst_ingestbackendselector` stay green (the existing anchor unit tests are the oracle).
- [ ] **Step 4: Adopt in `NativeSrtIngestSession`.** Convert PCR/PTS 90 kHz → ms at the boundary (`ms = ticks/90`) and route through an `AnchoredSourceClock{ClockQuality::Pcr}`, preserving the PCR-owns-anchor + discontinuity reset (`:401-410`). Keep `sourcePtsMsFromAnchor` as a thin shim (tests from Task 1 still pass). **Verify** `tst_ingestbackendselector` + the native SRT path are unchanged (run the av-sync gate).
- [ ] **Step 5: Commit** — `git commit -m "feat(timing): SourceClock abstraction; SRT/RTMP sessions adopt it (behavior-preserving)"`.

---

### Task 4: Timing telemetry on `IngestStats`

Make drift/quality observable (the Phase-0 rig + the UI read it).

**Files:** Modify `recorder_engine/ingest/ingestsession.h` (+ the two sessions that fill stats), `tests/e2e/sync_harness.cpp` (print the new fields).

- [ ] **Step 1:** Add to `IngestStats` (`ingestsession.h:56-68`): `double clockPpm = 0.0;` and `int clockQuality = 0; /*ClockQuality*/`. In each session's `reportStats`/`maybeReportStats`, set `stats.clockPpm = m_clock.ppm(); stats.clockQuality = int(m_clock.quality());`.
- [ ] **Step 2:** Extend `sync_harness --report-stats` to print `clockppm=%.3f clockq=%d` on the per-source stats line. Build + run a quick SRT record with `--report-stats` (or the Phase-0 `drift` cell) → confirm `clockppm` ≈ the injected skew. `tst_srt_health`/`tst_rtmp_health` stay green (additive fields).
- [ ] **Step 3: Commit** — `git commit -m "feat(timing): expose clock ppm/quality on IngestStats"`.

---

### Task 5: Audio drift servo (FIFO cursor rate-correction)

Apply the recovered ppm so audio is consumed rate-matched to the source clock — eliminating long-run A/V slip. Extract the cursor arithmetic to a pure helper first (testable), then apply a **gentle** correction.

**Files:** Create `recorder_engine/timing/audioservo.h`/`.cpp` + `tests/unit/tst_audioservo.cpp`; modify `recorder_engine/streamworker.cpp` (`writeAudioForTick` `:567-648`).

**Interfaces — Produces:** a pure `int64_t correctedSrcSamples(int64_t nominalSamples, double ppm)` (the number of *source* samples that map to `nominalSamples` of session time given a `ppm` skew, i.e. `round(nominalSamples * (1 + ppm/1e6))`), and a clamp `double clampPpm(double ppm, double maxPpm)` (cap correction to avoid audible artifacts; spec: "keep corrections gentle, cap and surface").

- [ ] **Step 1: Failing test** `tst_audioservo.cpp`: `correctedSrcSamples(48000, 0)==48000`; `correctedSrcSamples(48000, 200)==48010` (≈ +0.2 ‰); `clampPpm(5000, 500)==500`, `clampPpm(-5000,500)==-500`, `clampPpm(100,500)==100`. Register `olr_add_unit_test(tst_audioservo olr_test_core)`. RED.
- [ ] **Step 2: Implement** `audioservo.{h,cpp}` (pure). Build + run → PASS.
- [ ] **Step 3: Apply in `writeAudioForTick`.** Where it computes the source-sample window (`srcStart`/`n`, `:601-631`), scale the consumed source count by `clampPpm(m_currentSourcePpm, kMaxServoPpm=500)` via `correctedSrcSamples`, so when the source clock runs fast/slow the cursor advances proportionally — keeping the audio aligned to the recovered clock instead of slipping. `m_currentSourcePpm` is snapshotted per pulse (like `trimMs`/`jitterMs` at `onMasterPulse` `:107-111`) from the session's reported ppm. Default ppm 0 → behavior byte-identical (regression-safe).
- [ ] **Step 4: Verify byte-identity at ppm=0** — `( cd build/bcast && ctest -L e2e -R e2e_record )` + `-L av-sync` green (no skew → no correction). Then with the **Phase-0 `skew_injector`** at 200 ppm, the `drift` cell's A/V-offset-drift over the run drops toward 0 (was ~the injected slip). Record the before/after in the commit message.
- [ ] **Step 5: Commit** — `git commit -m "feat(timing): audio drift servo — rate-correct the FIFO cursor to the recovered clock"`.

---

### Task 6: Reconnect re-lock to the recovered clock

Today every `open()` resets the anchor → a reconnect re-locks to fresh arrival, re-introducing phase error. Re-lock to the recovered clock + last drift state.

**Files:** Modify `nativesrtingestsession.cpp` (`open()` reset `:142-146`), `nativertmpingestsession.cpp` (`open()` reset `:230-233`, reconnect break `:303-305`), `streamworker.cpp` (the capture/reconnect loop).

- [ ] **Step 1:** Persist the `AnchoredSourceClock` (+ its `DriftEstimator`) across reconnects of the **same URL** instead of resetting it in `open()`. On reconnect, the first post-reconnect `observe` continues the existing anchor/offset (the recovered clock is continuous even though arrival jittered) rather than stamping a new arrival anchor. Guard: a genuine source restart (large discontinuity / new stream) still re-anchors (the existing jump/discontinuity path handles it). Keep the clock owned where it survives the session re-`open()` (e.g. on the StreamWorker, passed into the session, or a session member not cleared in `open()`).
- [ ] **Step 2: Verify** with the native SRT reconnect gate (`e2e_native_srt_reconnect`) — A/V stays locked across the reconnect (no phase jump). The gate must stay green; additionally check the recorded A/V offset before vs after the kill/restart is within 1 frame.
- [ ] **Step 3: Commit** — `git commit -m "feat(timing): reconnect re-locks to the recovered clock, not fresh arrival"`.

---

### Task 7: Gate the rig + docs

**Files:** Modify `tests/e2e/CMakeLists.txt` (flip the drift/lipsync framesync cells to gated), `tests/e2e/FRAMESYNC.md` + `docs/native-ingest-workstream-remaining.md`.

- [ ] **Step 1:** Turn the Phase-0 `e2e_framesync_drift` (skew=0) + tighten `e2e_framesync_lipsync` into **gates** (set their `OLR_FRAMESYNC_GATE=1` + the tightened band) now that the servo holds them. Run `( cd build/bcast && ctest -L framesync --output-on-failure )` — drift slip < 1 frame, lipsync within band, both gated green.
- [ ] **Step 2: Docs** — update `FRAMESYNC.md` with the achieved numbers (ppm residual, A/V slip over the run) and note clock recovery + the servo are live; tick the P2 items in `docs/native-ingest-workstream-remaining.md`. Commit `docs(timing): document clock recovery + drift servo; gate the rig`.

---

## After all tasks
- `( cd build/bcast && ctest -L unit )` incl. `tst_driftestimator`, `tst_sourceclock`, `tst_audioservo`, the locked anchor test.
- `( cd build/bcast && ctest -L native-apple-ingest -L native-rtmp -L av-sync )` green (behavior-preserving).
- `( cd build/bcast && ctest -L framesync )` — drift cell at injected ppm now flattened by the servo; lipsync gated.
- Final review: SourceClock byte-identity vs the old anchors; DriftEstimator numerical stability; servo ppm cap (no audible artifacts); reconnect re-lock.

## Self-review
- **Spec coverage:** SourceClock (Pcr/Flv/Arrival via `AnchoredSourceClock`+quality) → Task 3; DriftEstimator → Task 2; drift servo → Task 5; reconnect re-lock → Task 6; telemetry → Task 4; acceptance via Phase-0 rig → Tasks 5/7. SessionTimeline is the RecordingClock + per-source SourceClocks (introduced minimally; the PTP `TimingReference` tier is deferred to Phase 5 per the spec).
- **Types consistent:** `DriftEstimator`/`SourceClock`/`ClockQuality`/`AnchoredSourceClock`/`correctedSrcSamples`/`clampPpm`/`clockPpm`/`clockQuality` used identically across tasks.
- **Risk:** behavior-preserving adoption (Tasks 3-4) gated by the existing anchor unit tests + native gates before any servo (Task 5) changes behavior; ppm=0 keeps everything byte-identical.
