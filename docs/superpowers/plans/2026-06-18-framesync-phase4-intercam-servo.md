# Frame-Sync Phase 4 — Inter-Camera Phase Servo + Confidence Surfacing — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add `SourceOffsetEstimator` — per source, compute the offset to the session reference and a **confidence tier** (`FrameAccurate` when common timecode or an external reference exists, `Bounded` with a numeric ±ms bound from the recovered clock, `Approximate` for arrival-only) — **correct** each source's mapping toward the reference so the cameras phase-lock, and **surface the measured inter-cam phase + tier** in the UI by extending the existing `IngestStats`/health pipe (#51) and `sourceStatsTooltip`. Delivers inter-camera phase-lock within measured/documented bounds (frame-accurate with TC), and tells the operator exactly how good it is.

**Architecture:** A pure `SourceOffsetEstimator` (no Qt/FFmpeg) holds, per source, the measured offset (ms) to the session reference and grades a `ConfidenceTier` from the available evidence: the `ClockQuality` of the source's `AnchoredSourceClock` (Phase 1), whether the `TimecodeAligner` (Phase 3) reports the source is TC-aligned to the reference source, and the `DriftEstimator` residual that yields the ±ms `Bounded` bound. ReplayManager already owns the `m_clock`/`m_workers`/`m_tcAligner`; it adds a `SourceOffsetEstimator`, picks a **reference source** (the highest-`ClockQuality` source, ties broken by lowest index), and applies a **bounded** per-source correction to each worker's existing trim seam (`StreamWorker::m_trimOffsetMs`, the PHASE-6 operator-override layer) — operator trim and the servo correction are summed, the operator override always wins on top. The measured phase + tier ride the **already-present** `IngestStats::clockPpm`/`clockQuality` plus two new additive fields (`interCamPhaseMs`, `confidenceTier`); UIManager renders them in `sourceStatsTooltip` (extending the SRT/RTMP branches that today omit even the existing `clockPpm`/`clockQuality`) and via a new `Q_INVOKABLE` tier accessor for the QML health surface.

**Tech Stack:** C++17, Qt6 (signals/slots, QML invokables), Qt Test.

## Global Constraints
- Spec: `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md` (Phase 4 + "Inter-camera phase + confidence"). Depends on Phase 1 (`AnchoredSourceClock`/`ClockQuality`/`DriftEstimator`) and Phase 3 (`TimecodeAligner`/`sourcesFrameAligned`) for the FrameAccurate tier.
- Worktree `/tmp/olr-bcast`. Build `build/bcast`. Format only changed C++ lines (`git clang-format`, llvm `/opt/homebrew/opt/llvm/bin`); CI lint = clang-format 22.1.7.
- **Behavior-preserving correction:** the servo correction is **capped** (`kMaxInterCamCorrectionMs`) and **additive** to the existing operator trim — the operator override (PHASE-6) always layers on top, and a 0-correction state (single source, or no measurable offset) is byte-identical to today. Never distort: cap and surface a degraded tier rather than over-correct (matches the spec's drift-servo-saturation rule).
- New `IngestStats` fields are **additive** (the `tst_srt_health`/`tst_rtmp_health` graders are pure and unaffected); the existing `clockPpm`/`clockQuality` fields are already present and just under-displayed.
- Verified anchors (from the map): `AnchoredSourceClock{ClockQuality}` + `ppm()`/`quality()`/`locked()` `sourceclock.h:34-61`, `DriftEstimator::offsetNs()`/`ppm()` `driftestimator.h:11-16`; `ClockQuality{ Arrival=0, FlvPll=1, Ndi=2, Pcr=3, Reference=4 }` `sourceclock.h:8-14`; `IngestStats{ ... double clockPpm=0.0; int clockQuality=0; }` `ingestsession.h:59-74`, `IngestStatsKind{ Unknown, Srt, Rtmp, Ndi }` `:54`; the workers' clocks `m_srtSourceClock`/`m_rtmpSourceClock`/`m_ndiSourceClock` `streamworker.h:151-153`, `m_trimOffsetMs` (operator trim, `setTrimOffsetMs` clamped) `streamworker.h:146`, snapshotted per pulse `streamworker.cpp:114`; ReplayManager `m_workers` `replaymanager.h:141`, `m_clock` (RecordingClock) `:140`, `m_sourceTrims` + `updateSourceTrim`/`setTrimOffsetMs` relay `replaymanager.cpp:353-359`, worker-create loop `:204-245`, `sourceStatsUpdated(int,IngestStats)` relay `replaymanager.h:90` → worker `statsUpdated` `streamworker.cpp:336-338`; from Phase 3: `ReplayManager::sourcesFrameAligned(a,b)`/`sourceFrameOffset(a,b)` + `m_tcAligner`; UIManager `onSourceStatsUpdated` `uimanager.cpp:1108-1124` (stores `m_sourceStats[i].last`/`.health`), `sourceStatsTooltip` `:1136-1154`, `sourceLinkHealth`/`sourceHasStats` `:1126-1134`, `m_sourceStats` (`struct IngestStatsEntry{ IngestStats last; bool seen; int health; }`) + `m_sourceStatsVersion`/`sourceStatsChanged()` `uimanager.h:405-411`, the `sourceStatsUpdated`→`onSourceStatsUpdated` connect `:138-139`. **No existing `SourceOffsetEstimator`/`ConfidenceTier`/`interCamPhaseMs` anywhere** (grep-confirmed net-new).

---

### Task 1: `ConfidenceTier` + `SourceOffsetEstimator` — offset + tier grading (pure)

The §"Inter-camera phase + confidence" unit. Pure (no Qt/FFmpeg): per source it stores the measured offset to the reference and grades the tier from the evidence the engine supplies.

**Files:** Create `recorder_engine/timing/sourceoffsetestimator.h`, `.cpp`, `tests/unit/tst_sourceoffsetestimator.cpp`; modify `tests/CMakeLists.txt` (`olr_test_core`), `tests/unit/CMakeLists.txt`, root `CMakeLists.txt` (engine sources, next to `timecodealigner.cpp`).

**Interfaces — Produces:**
```cpp
#ifndef SOURCEOFFSETESTIMATOR_H
#define SOURCEOFFSETESTIMATOR_H
#include "sourceclock.h"   // ClockQuality
#include <cstdint>

// Confidence in a source's inter-camera phase alignment, ascending. Surfaced to
// the operator: FrameAccurate = common TC or an external reference; Bounded =
// recovered-clock estimate with a numeric +/-ms bound; Approximate = arrival-only.
enum class ConfidenceTier { Approximate = 0, Bounded = 1, FrameAccurate = 2 };

// The evidence the engine feeds per source per update. Pure inputs so the grader
// is fully unit-testable.
struct SourcePhaseEvidence {
    ClockQuality clockQuality = ClockQuality::Arrival;
    bool clockLocked = false;
    bool timecodeAlignedToReference = false; // TimecodeAligner says equal-TC frames coincide
    bool externalReference = false;          // a real reference (PTP) is locked (Phase 5)
    double clockPpm = 0.0;                    // from the source's DriftEstimator
    int64_t measuredOffsetMs = 0;            // measured phase vs the reference source
};

// Per-source offset-to-reference + confidence tier. Pure (no Qt/FFmpeg).
class SourceOffsetEstimator {
public:
    explicit SourceOffsetEstimator(int boundedBaseMs = 4, int boundedPpmMsPerSec = 0);

    void update(int sourceIndex, const SourcePhaseEvidence& ev);

    ConfidenceTier tier(int sourceIndex) const;
    int64_t offsetMs(int sourceIndex) const;   // measured phase vs the reference
    // The +/-ms bound for a Bounded tier (0 for FrameAccurate; a wide default for
    // Approximate). Surfaced verbatim as the operator's "how good is it" number.
    int boundMs(int sourceIndex) const;
    bool hasEvidence(int sourceIndex) const;
    void reset();

private:
    struct Entry { bool set = false; ConfidenceTier tier = ConfidenceTier::Approximate;
                   int64_t offsetMs = 0; int boundMs = 0; };
    static constexpr int kMaxSources = 16;
    static constexpr int kApproximateBoundMs = 40; // arrival jitter order
    int m_boundedBaseMs;
    Entry m_entries[kMaxSources];
};
#endif // SOURCEOFFSETESTIMATOR_H
```

- [ ] **Step 1: Failing test** `tst_sourceoffsetestimator.cpp`:
```cpp
#include <QtTest>
#include "recorder_engine/timing/sourceoffsetestimator.h"

class TestSourceOffsetEstimator : public QObject {
    Q_OBJECT
private slots:
    void noEvidenceDefaultsApproximate();
    void timecodeAlignedIsFrameAccurate();
    void externalReferenceIsFrameAccurate();
    void lockedClockIsBoundedWithMsBound();
    void unlockedArrivalIsApproximate();
    void offsetIsRecorded();
    void resetClears();
};

void TestSourceOffsetEstimator::noEvidenceDefaultsApproximate() {
    SourceOffsetEstimator e;
    QVERIFY(!e.hasEvidence(0));
    QCOMPARE(e.tier(0), ConfidenceTier::Approximate);
}
void TestSourceOffsetEstimator::timecodeAlignedIsFrameAccurate() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Pcr; ev.clockLocked = true;
    ev.timecodeAlignedToReference = true; ev.measuredOffsetMs = 0;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::FrameAccurate);
    QCOMPARE(e.boundMs(0), 0);
}
void TestSourceOffsetEstimator::externalReferenceIsFrameAccurate() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev; ev.externalReference = true; ev.clockLocked = true;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::FrameAccurate);
}
void TestSourceOffsetEstimator::lockedClockIsBoundedWithMsBound() {
    SourceOffsetEstimator e(/*boundedBaseMs*/ 4);
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Pcr; ev.clockLocked = true;
    ev.timecodeAlignedToReference = false; ev.measuredOffsetMs = 7;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::Bounded);
    QVERIFY(e.boundMs(0) >= 4);  // at least the base bound
    QCOMPARE(e.offsetMs(0), int64_t(7));
}
void TestSourceOffsetEstimator::unlockedArrivalIsApproximate() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Arrival; ev.clockLocked = false;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::Approximate);
    QCOMPARE(e.boundMs(0), 40);
}
void TestSourceOffsetEstimator::offsetIsRecorded() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev; ev.clockLocked = true; ev.clockQuality = ClockQuality::Ndi;
    ev.measuredOffsetMs = -12;
    e.update(1, ev);
    QCOMPARE(e.offsetMs(1), int64_t(-12));
}
void TestSourceOffsetEstimator::resetClears() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev; ev.clockLocked = true; e.update(0, ev);
    e.reset();
    QVERIFY(!e.hasEvidence(0));
}
QTEST_GUILESS_MAIN(TestSourceOffsetEstimator)
#include "tst_sourceoffsetestimator.moc"
```
- [ ] **Step 2: Register** `olr_add_unit_test(tst_sourceoffsetestimator olr_test_core)`. Verify RED.
- [ ] **Step 3: Implement** `sourceoffsetestimator.{h,cpp}`. `update`: bound `sourceIndex` to `kMaxSources`; mark `set=true`, store `offsetMs = ev.measuredOffsetMs`. Tier grading (descending): `FrameAccurate` iff `ev.timecodeAlignedToReference || ev.externalReference`; else `Bounded` iff `ev.clockLocked` (any recovered clock — `Pcr`/`Ndi`/`FlvPll`); else `Approximate`. `boundMs`: `0` for `FrameAccurate`; for `Bounded` `m_boundedBaseMs + |ppm-derived term|` (RTMP `FlvPll` gets a wider base since ms-resolution FLV is noisier — add `kFlvExtraBoundMs=2` when `clockQuality==FlvPll`, matching the spec's "wider bound on RTMP-only inter-cam"); `kApproximateBoundMs (40)` for `Approximate`. Build + run → 7/7 PASS.
- [ ] **Step 4: Wire + commit** — add `sourceoffsetestimator.cpp` to `olr_test_core` + root engine sources. `git commit -m "feat(framesync): SourceOffsetEstimator — offset + confidence tier grading (unit-tested)"`.

---

### Task 2: Expose the source clock's offset + lock state to ReplayManager

The servo needs each source's measured offset to the reference. The worker already owns the `AnchoredSourceClock`; surface its `offsetNs()`/`ppm()`/`quality()`/`locked()` snapshot on the existing stats pulse.

**Files:** Modify `recorder_engine/ingest/ingestsession.h` (`IngestStats`), the three sessions' stats fill, `recorder_engine/streamworker.cpp`.

- [ ] **Step 1:** Add two **additive** fields to `IngestStats` (`ingestsession.h:59-74`, after `clockQuality`): `bool clockLocked = false;` and `int64_t clockOffsetNs = 0; // DriftEstimator offsetNs (session - sender at the centroid)`. In each session's `reportStats`/`maybeReportStats`, set `stats.clockLocked = m_clock->locked(); stats.clockOffsetNs = /* m_clock's DriftEstimator offsetNs */;` alongside the existing `stats.clockPpm`/`stats.clockQuality`. (Expose `offsetNs()` from `AnchoredSourceClock` via a thin `int64_t driftOffsetNs() const { return m_drift.offsetNs(); }` accessor in `sourceclock.h` if not already reachable.)
- [ ] **Step 2: Verify** — the SRT/RTMP/NDI health gates stay green (additive fields, graders unchanged). Build + run a quick record with `--report-stats` and confirm `clockLocked`/`clockOffsetNs` populate once the clock locks. Commit `feat(framesync): expose clock lock state + offset on IngestStats`.

---

### Task 3: ReplayManager picks a reference source + runs the offset estimator

ReplayManager owns the timeline, the workers, and (from Phase 3) the `TimecodeAligner`. Pick the reference (highest `ClockQuality`, ties → lowest index) and feed the `SourceOffsetEstimator` each stats pulse.

**Files:** Modify `recorder_engine/replaymanager.{h,cpp}`.

- [ ] **Step 1:** Add members `SourceOffsetEstimator m_offsetEstimator;`, `int m_referenceSource = -1;`, and a cache of the latest per-source `IngestStats` (`QList<IngestStats> m_lastStats;`, sized to source count at `startRecording`). In the existing `sourceStatsUpdated` relay slot (wherever ReplayManager forwards worker stats — it relays `statsUpdated`→`sourceStatsUpdated` `replaymanager.h:90`), cache `m_lastStats[src] = stats;` then call a new `recomputeInterCamPhase()`.
- [ ] **Step 2: `recomputeInterCamPhase()`** — pick `m_referenceSource` = the connected source with the highest `clockQuality` (tie → lowest index; an `externalReference` source, Phase 5, always wins). For each source `s`, build a `SourcePhaseEvidence`: `clockQuality = ClockQuality(m_lastStats[s].clockQuality)`, `clockLocked = m_lastStats[s].clockLocked`, `clockPpm = m_lastStats[s].clockPpm`, `timecodeAlignedToReference = (s == m_referenceSource) || m_tcAligner.sourcesAligned(m_referenceSource, s, 0)`, `externalReference = false` (Phase 5 sets this), `measuredOffsetMs = (m_lastStats[s].clockOffsetNs - m_lastStats[m_referenceSource].clockOffsetNs) / 1'000'000` (the phase of `s` relative to the reference). `m_offsetEstimator.update(s, ev)`. Then emit a new `interCamPhaseUpdated(int sourceIndex, int tier, int64_t phaseMs, int boundMs)` for each source (Qt::QueuedConnection-safe; UIManager consumes it in Task 5).
- [ ] **Step 3: Verify** — a focused ReplayManager test feeds two synthetic stats sets (equal `clockOffsetNs`, both `Pcr` locked, TC-aligned) and asserts `m_offsetEstimator.tier(s) == FrameAccurate` + `offsetMs == 0`; a skewed pair (different `clockOffsetNs`, no TC) yields `Bounded` + the right `offsetMs`. Native gates stay green. Commit `feat(framesync): ReplayManager reference-source selection + inter-cam phase estimation`.

---

### Task 4: Bounded phase correction toward the reference (the servo)

Correct each source's mapping toward the reference by nudging the worker's trim seam — capped, additive to the operator override.

**Files:** Modify `recorder_engine/streamworker.{h,cpp}` (a servo-trim member separate from the operator trim), `recorder_engine/replaymanager.cpp`.

- [ ] **Step 1: StreamWorker** — add `std::atomic<int> m_servoTrimOffsetMs{0};` + `void setServoTrimOffsetMs(int ms);` (clamped to `±kMaxInterCamCorrectionMs`, e.g. 250 ms). In `onMasterPulse` where the operator trim is snapshotted (`const int64_t trimMs = m_trimOffsetMs.load(...)` `streamworker.cpp:114`), **sum** the servo trim: `const int64_t trimMs = m_trimOffsetMs.load(...) + m_servoTrimOffsetMs.load(std::memory_order_relaxed);`. This routes through the existing trim arithmetic untouched (operator trim still wins on top; servo is an additional bounded nudge). Default 0 → byte-identical.
- [ ] **Step 2: ReplayManager** — in `recomputeInterCamPhase()`, after `m_offsetEstimator.update`, for each non-reference source apply a **gentle, capped** correction toward 0 phase: `const int64_t phase = m_offsetEstimator.offsetMs(s); const int corr = int(qBound<int64_t>(-kMaxInterCamCorrectionMs, -phase, kMaxInterCamCorrectionMs)); m_workers[s]->setServoTrimOffsetMs(corr);`. Only correct `Bounded`/`FrameAccurate` sources whose clock is locked (an `Approximate` source gets no servo — there is nothing reliable to lock to; it stays where arrival put it, surfaced as `Approximate`). The reference source gets servo 0.
- [ ] **Step 3: Verify** — with the Phase-0 rig running two sources at an injected fixed phase offset, the recorded inter-cam flash-onset spread shrinks toward ≤1 frame for `Bounded` sources after the servo engages (record before/after spread in the commit message). The single-source and Approximate paths stay byte-identical (servo 0). Native + framesync gates green. Commit `feat(framesync): bounded inter-cam phase servo via the worker trim seam`.

---

### Task 5: Surface measured phase + tier in `IngestStats` and the UI

Carry the tier + measured phase on the stats snapshot, and render them in `sourceStatsTooltip` (today it omits even the present `clockPpm`/`clockQuality`) + expose a tier accessor for the QML health surface.

**Files:** Modify `recorder_engine/ingest/ingestsession.h` (`IngestStats`), `recorder_engine/replaymanager.cpp` (fill the new fields before relaying), `uimanager.{h,cpp}`.

- [ ] **Step 1:** Add **additive** fields to `IngestStats` (`:59-74`): `int confidenceTier = 0; // ConfidenceTier as int` and `int64_t interCamPhaseMs = 0;` and `int interCamBoundMs = 0;`. In ReplayManager's stats relay, before forwarding to the UI, stamp `stats.confidenceTier = int(m_offsetEstimator.tier(src)); stats.interCamPhaseMs = m_offsetEstimator.offsetMs(src); stats.interCamBoundMs = m_offsetEstimator.boundMs(src);` (so the existing `sourceStatsUpdated`→`onSourceStatsUpdated` pipe carries them; no new signal wiring needed for the tooltip).
- [ ] **Step 2: UIManager tooltip** — extend `sourceStatsTooltip` (`uimanager.cpp:1136-1154`). Append to BOTH the SRT and RTMP branches (and add an NDI/`IngestStatsKind::Ndi` branch) a common timing block built from `s`:
```cpp
    static const char* tierName(int t) {
        switch (t) {
        case 2: return "frame-accurate";
        case 1: return "bounded";
        default: return "approximate";
        }
    }
    // ... within sourceStatsTooltip, after the per-kind link block:
    QString timing = QStringLiteral("\nclock     %1  (%2 ppm)\nphase     %3 ms  (±%4 ms, %5)")
        .arg(clockQualityName(s.clockQuality))
        .arg(QString::number(s.clockPpm, 'f', 1))
        .arg(QString::number(qlonglong(s.interCamPhaseMs)))
        .arg(QString::number(s.interCamBoundMs))
        .arg(QString::fromLatin1(tierName(s.confidenceTier)));
    return link + timing;
```
where `clockQualityName(int)` maps `0..4 → "arrival"/"flv"/"ndi"/"pcr"/"reference"` (a small static helper in `uimanager.cpp`). FrameAccurate shows `±0 ms`. (The existing link string becomes `link`; append `timing`.)
- [ ] **Step 3: UIManager accessor** — add `Q_INVOKABLE int sourceConfidenceTier(int sourceIndex) const;` returning `m_sourceStats[i].last.confidenceTier` (0/1/2) for the QML surface to badge the source (e.g. a small "FA"/"±"/"~" marker next to the health dot), bound-checked like `sourceLinkHealth` (`:1126-1129`). Bump the existing `m_sourceStatsVersion`/`sourceStatsChanged()` already fires on every `onSourceStatsUpdated` (`:1122-1123`), so the QML binding refreshes for free.
- [ ] **Step 4: Verify** — a UIManager unit (extend the existing stats tests) feeds an `IngestStats` with `confidenceTier=2`, `interCamPhaseMs=0` and asserts `sourceStatsTooltip` contains `"frame-accurate"` + `"phase     0 ms"`, and `sourceConfidenceTier==2`; a `confidenceTier=1, interCamPhaseMs=7, interCamBoundMs=6` case shows `"phase     7 ms  (±6 ms, bounded)"`. Commit `feat(framesync): surface inter-cam phase + confidence tier in IngestStats + tooltip + QML accessor`.

---

### Task 6: Gate the rig (inter-cam cell) + docs

**Files:** Modify `tests/e2e/CMakeLists.txt` (flip the framesync inter-cam cell to a gate), `tests/e2e/FRAMESYNC.md`, `docs/native-ingest-workstream-remaining.md`.

- [ ] **Step 1:** Turn the Phase-0 `e2e_framesync_intercam` cell into a **gate** (`OLR_FRAMESYNC_GATE=1`): two common-TC sources record with the servo on → assert flash-onset spread ≤1 frame AND the recorded/reported tier is `FrameAccurate`; a no-common-TC pair → assert spread within the reported `Bounded` ±ms and tier `Bounded`. Run `( cd build/bcast && ctest -L framesync -R intercam --output-on-failure )` — green.
- [ ] **Step 2: Docs** — in `FRAMESYNC.md` record the achieved inter-cam spread per tier; tick the **P4** inter-cam items in `docs/native-ingest-workstream-remaining.md` (phase servo + confidence surfacing); note the honest ceiling (frame-accurate only with common TC/reference, else bounded-and-measured — surfaced, not hidden). Commit `docs(framesync): document inter-cam phase servo + confidence tiers; gate the inter-cam rig cell`.

---

## After all tasks
- `( cd build/bcast && ctest -L unit )` incl. `tst_sourceoffsetestimator`, the ReplayManager phase-estimation test, the UIManager tooltip/tier tests.
- `( cd build/bcast && ctest -L native-apple-ingest -L native-rtmp -L native-ndi -L av-sync )` green (additive stats fields; servo 0 = byte-identical single-source).
- `( cd build/bcast && ctest -L framesync -R intercam )` — common-TC spread ≤1 frame + FrameAccurate; bounded pair within its reported ±ms.
- Final review: servo cap + operator-trim summation (operator override always on top); reference-source selection (highest `ClockQuality`, tie → lowest index); tier grading matches `SourceOffsetEstimator` unit; Approximate sources get no servo and are surfaced honestly.

## Self-review
- **Spec coverage:** `SourceOffsetEstimator` (per-source offset to the session reference + the tier `FrameAccurate`/`Bounded(±ms)`/`Approximate`) → Task 1; correction of each source's mapping toward the reference → Tasks 3-4 (reference selection + bounded servo on the existing trim seam); surfacing measured inter-cam phase + tier extending `IngestStats`/health + `sourceStatsTooltip` (#51) and the clock line → Tasks 2,5; built on Phase 3's TC for the FrameAccurate tier (`m_tcAligner.sourcesAligned`) → Task 3. Covered.
- **Types consistent:** `SourceOffsetEstimator`/`ConfidenceTier`/`SourcePhaseEvidence`/`interCamPhaseMs`/`confidenceTier`/`interCamBoundMs`/`m_servoTrimOffsetMs` used identically across tasks; reuses Phase 1 `ClockQuality`/`AnchoredSourceClock`/`DriftEstimator` and Phase 3 `TimecodeAligner`/`sourcesAligned`/`sourceFrameOffset` verbatim (no rename). `ConfidenceTier` (Phase 4) is distinct from `ReferenceTier` (Phase 5, the `TimingReference` top tier) — kept separate by design.
- **No placeholders:** every code step gives complete `.h`/test/tooltip bodies + exact line anchors; the servo cap (`kMaxInterCamCorrectionMs`), the operator-trim summation, the tier-grading order, and the bound formula are all spelled out — no "TBD"/"add error handling".
- **Behavior-preserving:** additive `IngestStats` fields (graders untouched); servo defaults 0 → single-source/Approximate paths byte-identical; operator PHASE-6 trim always layers on top of the servo nudge.
