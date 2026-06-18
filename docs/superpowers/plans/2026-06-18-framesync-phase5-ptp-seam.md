# Frame-Sync Phase 5 — Reference-Clock / PTP Seam (Genlock-Readiness) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Introduce the `TimingReference` seam (§5) as the top tier of the timing core: today a `LocalMonotonicReference` wrapping the existing `RecordingClock`, designed so a `PtpReference` (ST 2059 / IEEE 1588 slave) can be swapped in to make the session timebase **facility time** without pipeline rework. Route the whole pipeline's "now" through `TimingReference::nowSessionNs()`, ship a `PtpReference` PTP client behind the seam (locks to a PTP grandmaster, reports `ReferenceTier::Ptp` + external once disciplined), and document the integration points for SDI/ST 2110 and genlocked NDI to become authoritative. **Full hardware capture stays out of scope** (DeckLink/Rivermax cards are a separate program) — this delivers the seam + the PTP client so locked feeds upgrade to true genlock.

**Architecture:** `TimingReference` is a pure-virtual seam (`nowSessionNs()`, `tier()`, `isExternal()`). `LocalMonotonicReference` wraps the existing `RecordingClock` (its `elapsedMs()*1e6` is `nowSessionNs()`) — a drop-in for today's behavior, tier `LocalMonotonic`, not external. The pipeline reads session-now through the reference instead of `RecordingClock::elapsedMs()` directly: ReplayManager owns a `std::unique_ptr<TimingReference>` and exposes `nowSessionNs()` / `nowSessionMs()`; the heartbeat (`onTimerTick`) and the ingest callbacks' `recordingClockMs` go through it. `PtpReference` is a slave-only PTP client (a portable SW PTP slave using a small `IPtpClient` backend interface so it is unit-testable with a fake, exactly like the NDI receiver backend pattern): it disciplines an offset+rate against a grandmaster and, once locked, reports `nowSessionNs()` in PTP/facility time, `tier()==Ptp`, `isExternal()==true`. When the reference is external, Phase 4's `SourceOffsetEstimator` promotes any source whose recovered clock phase-locks to the PTP-disciplined estimate to `FrameAccurate` (`SourcePhaseEvidence::externalReference`). The swap is a constructor choice in ReplayManager — no caller restructure.

**Tech Stack:** C++17, Qt6 (`QUdpSocket` for PTP event/general ports, or a `QLibrary`-loaded backend), Qt Test. PTP hardware timestamping is out of scope (SW timestamps; documented as the precision ceiling).

## Global Constraints
- Spec: `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md` (Phase 5 + `TimingReference` §5). Depends on Phase 1 (`RecordingClock`/`SourceClock`/`SessionTimeline`) and reads Phase 4's `SourceOffsetEstimator` (the `externalReference` evidence promotes to `FrameAccurate`).
- Worktree `/tmp/olr-bcast`. Build `build/bcast`. Format only changed C++ lines (`git clang-format`, llvm `/opt/homebrew/opt/llvm/bin`); CI lint = clang-format 22.1.7.
- **Behavior-preserving first:** `LocalMonotonicReference` must be byte-identical to today's `RecordingClock::elapsedMs()` usage — every existing gate (`av-sync`/native/framesync) stays green with the local reference; PTP is opt-in (env/config gated), never default.
- **Out of scope (documented, not built):** SDI / ST 2110 hardware ingest (DeckLink/Rivermax capture); PTP hardware timestamping (NIC PHC) — the PTP client uses software timestamps and the plan documents that as the precision ceiling. The seam is built so these become a backend swap, not a rewrite.
- Verified anchors (from the map): `RecordingClock{ void start(); int64_t elapsedMs() const; bool isValid() const; }` `recordingclock.h:14-22`; ReplayManager owns `m_clock` (RecordingClock, `new`'d in `startRecording`) `replaymanager.h:140` / `replaymanager.cpp:199-201`, `getElapsedMs()` → `m_clock->elapsedMs()` `:483-490`, heartbeat `onTimerTick` derives the frame timeline from `m_clock->elapsedMs()` `:362-383`, `masterPulse(frameIndex, elapsedMs)` emit `:378`; the ingest callback `recordingClockMs` (each session captures it, e.g. NDI `mapTimestampMs` via `m_callbacks.recordingClockMs()`) `ingestsession.h:99-107`; StreamWorker constructed with `RecordingClock* clock` `streamworker.h:55-56` (held as `m_sharedClock` `streamworker.h:110`); from Phase 4: `SourceOffsetEstimator` + `SourcePhaseEvidence::externalReference` (already a field), `ReplayManager::recomputeInterCamPhase()`; the `INdiReceiverBackend` fake-backend DI pattern `nativendiingestsession.h:15-26` (mirror for `IPtpClient`); the `#if defined(QT_TESTLIB_LIB)` + `friend class` test seam. **No existing `TimingReference`/`ReferenceTier`/`PtpReference`/`LocalMonotonicReference`/`IPtpClient` anywhere** (grep-confirmed net-new); the spec already names `enum class ReferenceTier { LocalMonotonic, RecoveredConsensus, Ptp };`.

---

### Task 1: `TimingReference` seam + `LocalMonotonicReference` (behavior-preserving)

The §5 seam. A pure-virtual interface + a `RecordingClock`-wrapping implementation that is byte-identical to today.

**Files:** Create `recorder_engine/timing/timingreference.h`, `.cpp`, `tests/unit/tst_timingreference.cpp`; modify `tests/CMakeLists.txt` (`olr_test_core`), `tests/unit/CMakeLists.txt`, root `CMakeLists.txt` (engine sources, next to `sourceclock.cpp`).

**Interfaces — Produces:**
```cpp
#ifndef TIMINGREFERENCE_H
#define TIMINGREFERENCE_H
#include <cstdint>

class RecordingClock;

// The best-available session timebase, ascending trust. LocalMonotonic = the
// RecordingClock (no external reference); RecoveredConsensus = a consensus of the
// recovered sender clocks (reserved); Ptp = an ST 2059 / IEEE 1588 facility clock.
enum class ReferenceTier { LocalMonotonic = 0, RecoveredConsensus = 1, Ptp = 2 };

// The single source of "session now" the whole pipeline reads. Swapping the
// implementation (Local -> Ptp) re-times the session without caller changes.
class TimingReference {
public:
    virtual ~TimingReference() = default;
    virtual int64_t nowSessionNs() const = 0;
    virtual ReferenceTier tier() const = 0;
    virtual bool isExternal() const = 0; // true once a real reference (PTP) is locked
    // Convenience: ms since the session epoch (the timeline the muxer/heartbeat use).
    int64_t nowSessionMs() const { return nowSessionNs() / 1'000'000; }
};

// Wraps the existing RecordingClock: nowSessionNs == elapsedMs()*1e6. The default
// reference; byte-identical to today's behavior. Not external.
class LocalMonotonicReference final : public TimingReference {
public:
    explicit LocalMonotonicReference(const RecordingClock* clock);
    int64_t nowSessionNs() const override;
    ReferenceTier tier() const override { return ReferenceTier::LocalMonotonic; }
    bool isExternal() const override { return false; }
private:
    const RecordingClock* m_clock;
};
#endif // TIMINGREFERENCE_H
```

- [ ] **Step 1: Failing test** `tst_timingreference.cpp`:
```cpp
#include <QtTest>
#include "recorder_engine/timing/timingreference.h"
#include "recorder_engine/recordingclock.h"

class TestTimingReference : public QObject {
    Q_OBJECT
private slots:
    void localTierIsLocalMonotonic();
    void localIsNotExternal();
    void nowTracksRecordingClock();
    void nowMsIsNsOver1e6();
};

void TestTimingReference::localTierIsLocalMonotonic() {
    RecordingClock c; c.start();
    LocalMonotonicReference ref(&c);
    QCOMPARE(ref.tier(), ReferenceTier::LocalMonotonic);
}
void TestTimingReference::localIsNotExternal() {
    RecordingClock c; c.start();
    LocalMonotonicReference ref(&c);
    QVERIFY(!ref.isExternal());
}
void TestTimingReference::nowTracksRecordingClock() {
    RecordingClock c; c.start();
    LocalMonotonicReference ref(&c);
    QTest::qWait(20);
    const int64_t refMs = ref.nowSessionMs();
    const int64_t clkMs = c.elapsedMs();
    QVERIFY(qAbs(refMs - clkMs) <= 2); // same clock, sampled microseconds apart
}
void TestTimingReference::nowMsIsNsOver1e6() {
    RecordingClock c; c.start();
    LocalMonotonicReference ref(&c);
    QTest::qWait(15);
    QCOMPARE(ref.nowSessionMs(), ref.nowSessionNs() / 1'000'000);
}
QTEST_GUILESS_MAIN(TestTimingReference)
#include "tst_timingreference.moc"
```
- [ ] **Step 2: Register** `olr_add_unit_test(tst_timingreference olr_test_core)`. Verify RED.
- [ ] **Step 3: Implement** `timingreference.{h,cpp}`: `LocalMonotonicReference::nowSessionNs()` returns `m_clock && m_clock->isValid() ? m_clock->elapsedMs() * 1'000'000LL : 0`. Build + run → 4/4 PASS.
- [ ] **Step 4: Wire + commit** — add `timingreference.cpp` to `olr_test_core` + root engine sources. `git commit -m "feat(timing): TimingReference seam + LocalMonotonicReference (byte-identical to RecordingClock)"`.

---

### Task 2: ReplayManager reads session-now through the `TimingReference`

Route the heartbeat + `getElapsedMs` through the reference, owning a `LocalMonotonicReference` by default. Byte-identical behavior; the swap point is now central.

**Files:** Modify `recorder_engine/replaymanager.{h,cpp}`.

- [ ] **Step 1:** Add a member `std::unique_ptr<TimingReference> m_timingRef;`. In `startRecording`, after `m_clock = new RecordingClock(); m_clock->start();` (`:199-201`), construct `m_timingRef = std::make_unique<LocalMonotonicReference>(m_clock);`. Add a private `int64_t nowSessionMs() const { return m_timingRef ? m_timingRef->nowSessionMs() : (m_clock ? m_clock->elapsedMs() : 0); }`.
- [ ] **Step 2:** Replace the direct `m_clock->elapsedMs()` reads on the timeline path with `nowSessionMs()`: the heartbeat `onTimerTick` (`:362-383`) and `getElapsedMs` (`:483-490`). The frame-index math + `masterPulse(f, frameMs)` emit (`:378`) are unchanged in arithmetic — they just read the reference. (Leave `m_clock` allocation/lifetime as-is; the reference holds a non-owning pointer, destroyed before `m_clock`.)
- [ ] **Step 3: Verify** — every existing gate stays green (`av-sync`, native ingest, framesync) because `LocalMonotonicReference` returns exactly `elapsedMs()*1e6`. Build + run the av-sync gate. Commit `feat(timing): ReplayManager reads session-now through TimingReference (local tier)`.

---

### Task 3: `IPtpClient` backend interface + a fake + `PtpServo` (pure)

A PTP slave needs the offset/rate-discipline math (pure, testable) decoupled from the socket/timestamp backend (faked in tests, real over UDP later). Mirror the `INdiReceiverBackend` DI pattern.

**Files:** Create `recorder_engine/timing/ptpservo.h`, `.cpp`, `recorder_engine/timing/iptpclient.h`, `tests/unit/tst_ptpservo.cpp`; modify CMake (`olr_test_core`, `tests/unit/CMakeLists.txt`, root `CMakeLists.txt`).

**Interfaces — Produces:**
```cpp
// iptpclient.h — the socket/timestamp backend (faked in tests; real = UDP 319/320).
#ifndef IPTPCLIENT_H
#define IPTPCLIENT_H
#include <cstdint>

// One PTP two-way exchange sample: the four IEEE 1588 timestamps (ns).
//   t1 = master Sync egress (from Sync/Follow_Up)
//   t2 = slave Sync ingress (local)
//   t3 = slave Delay_Req egress (local)
//   t4 = master Delay_Resp ingress (from Delay_Resp)
struct PtpExchange { int64_t t1 = 0; int64_t t2 = 0; int64_t t3 = 0; int64_t t4 = 0; bool valid = false; };

class IPtpClient {
public:
    virtual ~IPtpClient() = default;
    virtual bool start(const QString& domainOrIface) = 0;
    virtual void stop() = 0;
    // Block up to timeoutMs for the next completed two-way exchange.
    virtual PtpExchange nextExchange(int timeoutMs) = 0;
    virtual int64_t localMonotonicNs() const = 0;
};
#endif // IPTPCLIENT_H
```
```cpp
// ptpservo.h — pure IEEE 1588 offset/rate discipline. No Qt/sockets.
#ifndef PTPSERVO_H
#define PTPSERVO_H
#include "iptpclient.h"
#include <cstdint>

// Disciplines a local->master offset (and a slow rate term) from PTP exchanges.
//   meanPathDelay = ((t2 - t1) + (t4 - t3)) / 2
//   offset        = (t2 - t1) - meanPathDelay     (local - master)
// Locks after kMinExchanges consistent samples. masterNsFromLocal(localNs)
// converts a local monotonic ns to estimated master/facility ns.
class PtpServo {
public:
    explicit PtpServo(int minExchanges = 8);
    void observe(const PtpExchange& ex);
    bool locked() const;
    int64_t offsetNs() const;        // local - master (0 until locked)
    int64_t meanPathDelayNs() const; // 0 until locked
    int64_t masterNsFromLocal(int64_t localNs) const; // localNs - offsetNs
    void reset();
private:
    int m_minExchanges;
    int m_count = 0;
    int64_t m_offsetNs = 0;
    int64_t m_pathDelayNs = 0;
    bool m_locked = false;
};
#endif // PTPSERVO_H
```

- [ ] **Step 1: Failing test** `tst_ptpservo.cpp`: drive `observe` with synthetic exchanges where the master leads the slave by a known offset `O` and a symmetric path delay `D` (`t1=master, t2=t1+D+O ... ` constructed so `offset==O`, `meanPathDelay==D`). Assert: unlocked before `minExchanges`; after enough → `locked()`, `qAbs(offsetNs()-O) <= 1`, `qAbs(meanPathDelayNs()-D) <= 1`; `masterNsFromLocal(localNs) == localNs - offsetNs`; `reset()` unlocks. Register `olr_add_unit_test(tst_ptpservo olr_test_core)`. Verify RED.
- [ ] **Step 2: Implement** `ptpservo.{h,cpp}` (pure): per `observe`, compute `meanPathDelay`/`offset`, smooth with a simple low-pass (running mean over the last `minExchanges`), set `m_locked` once `m_count >= minExchanges`. `masterNsFromLocal = localNs - m_offsetNs`. Build + run → PASS.
- [ ] **Step 3: Commit** — `git commit -m "feat(timing): PtpServo — IEEE 1588 offset/path-delay discipline + IPtpClient seam (unit-tested)"`.

---

### Task 4: `PtpReference` — a `TimingReference` over `PtpServo` + `IPtpClient`

The external top tier: a PTP slave that, once disciplined, reports `nowSessionNs()` in facility time and `isExternal()==true`. Unit-tested via a fake `IPtpClient`.

**Files:** Create `recorder_engine/timing/ptpreference.h`, `.cpp`, `tests/unit/tst_ptpreference.cpp`; modify CMake.

**Interfaces — Produces:**
```cpp
#ifndef PTPREFERENCE_H
#define PTPREFERENCE_H
#include "timingreference.h"
#include "ptpservo.h"
#include "iptpclient.h"
#include <QString>
#include <atomic>
#include <memory>
#include <thread>

// ST 2059 / IEEE 1588 slave as the TimingReference top tier. Runs a background
// thread pulling exchanges from the IPtpClient into a PtpServo; nowSessionNs()
// returns disciplined master/facility ns once locked, else falls back to the
// local monotonic (so the pipeline never stalls waiting for PTP lock).
class PtpReference final : public TimingReference {
public:
    explicit PtpReference(std::unique_ptr<IPtpClient> client, const QString& domainOrIface);
    PtpReference(std::unique_ptr<IPtpClient> client, const QString& domainOrIface,
                 int64_t sessionEpochMasterNs); // DI epoch for tests
    ~PtpReference() override;

    bool start();   // starts the client + the discipline thread
    void stop();

    int64_t nowSessionNs() const override;
    ReferenceTier tier() const override { return ReferenceTier::Ptp; }
    bool isExternal() const override { return m_locked.load(std::memory_order_acquire); }

    bool locked() const { return m_locked.load(std::memory_order_acquire); }

private:
    void disciplineLoop();
    std::unique_ptr<IPtpClient> m_client;
    QString m_domain;
    PtpServo m_servo;
    std::atomic<int64_t> m_offsetNs{0};
    std::atomic<int64_t> m_epochMasterNs{-1}; // master ns at session start
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
#endif // PTPREFERENCE_H
```

- [ ] **Step 1: Failing test** `tst_ptpreference.cpp` with a `FakePtpClient : IPtpClient` (model the `FakeNdiReceiverBackend` pattern) returning a scripted exchange stream (known offset) and a controllable `localMonotonicNs()`. Drive: `PtpReference ref(std::make_unique<FakePtpClient>(...), "0", /*epochMasterNs*/ E); ref.start();` → after enough exchanges `ref.locked()==true`, `ref.isExternal()==true`, `ref.tier()==ReferenceTier::Ptp`, and `nowSessionNs()` advances in master time (`== client.localMonotonicNs() - offset - epoch`-relative, i.e. session ns since the PTP epoch); before lock `isExternal()==false` and `nowSessionNs()` falls back to local-monotonic-since-epoch (never negative, monotonic). Register `olr_add_unit_test(tst_ptpreference olr_test_core)`. Verify RED.
- [ ] **Step 2: Implement** `ptpreference.{h,cpp}`: `start()` → `m_client->start(m_domain)`, set `m_running`, launch `m_thread(disciplineLoop)`. `disciplineLoop`: loop `ex = m_client->nextExchange(timeoutMs); if (ex.valid) m_servo.observe(ex);` publish `m_offsetNs = m_servo.offsetNs(); m_locked = m_servo.locked();` (and capture `m_epochMasterNs` on first lock if not DI-provided). `nowSessionNs()`: `const int64_t localNs = m_client->localMonotonicNs(); const int64_t masterNs = m_locked ? localNs - m_offsetNs : localNs; return masterNs - m_epochMasterNs;` (epoch-relative session ns; falls back to local before lock). `~PtpReference`/`stop()` join the thread + `m_client->stop()`. Build + run → PASS.
- [ ] **Step 3: Commit** — `git commit -m "feat(timing): PtpReference — PTP slave as the TimingReference top tier (fake-tested)"`.

---

### Task 5: Make the reference swappable in ReplayManager (opt-in PTP)

Let an operator select PTP; default stays `LocalMonotonicReference`. The swap is the single constructor choice the architecture promised.

**Files:** Modify `recorder_engine/replaymanager.{h,cpp}`; add the real `QUdpSocket`-backed `UdpPtpClient` (compiles everywhere; runtime-gated) in `recorder_engine/timing/udpptpclient.{h,cpp}`.

- [ ] **Step 1: Real client** — implement `UdpPtpClient : IPtpClient` (`recorder_engine/timing/udpptpclient.{h,cpp}`) over `QUdpSocket` on the PTP event (319) + general (320) multicast ports: join `224.0.1.129`, parse `Sync`/`Follow_Up`/`Delay_Resp` for `t1`/`t4`, software-timestamp `t2`/`t3` from a monotonic clock, send `Delay_Req`, and surface completed `PtpExchange`es from `nextExchange`. **Software timestamps only** — document this as the precision ceiling (no NIC PHC / hardware timestamping; that is a future backend swap). This TU is logic-tested via the fake `IPtpClient`; the real socket path is a runtime/integration check.
- [ ] **Step 2: ReplayManager swap** — in `startRecording`, choose the reference: if PTP is enabled (a config/env flag `OLR_TIMING_PTP=1` + an optional `OLR_TIMING_PTP_IFACE`), construct `auto ptp = std::make_unique<PtpReference>(std::make_unique<UdpPtpClient>(), iface); if (ptp->start()) m_timingRef = std::move(ptp); else m_timingRef = std::make_unique<LocalMonotonicReference>(m_clock);` (fall back to local if PTP fails to start). Else `LocalMonotonicReference` as in Task 2. **No other caller changes** — the whole pipeline already reads `nowSessionMs()` (Task 2).
- [ ] **Step 3: Promote PTP-locked sources to FrameAccurate (Phase 4 hook)** — in `recomputeInterCamPhase()` (Phase 4), set each source's `SourcePhaseEvidence::externalReference = m_timingRef && m_timingRef->isExternal();` so once PTP is locked, sources phase-locked to the disciplined session estimate grade `FrameAccurate` (the `SourceOffsetEstimator` already keys on `externalReference`). When PTP is not present, `externalReference` stays false — no change from Phase 4.
- [ ] **Step 4: Verify** — default (no env) path: every gate green, tier `LocalMonotonic`, identical to today. With a stubbed/fake PTP enabled in a focused ReplayManager test, `m_timingRef->isExternal()` flips true and sources report `FrameAccurate`. Commit `feat(timing): swappable TimingReference in ReplayManager; opt-in PTP slave promotes locked sources to FrameAccurate`.

---

### Task 6: Surface the reference tier in the UI + document the genlock integration points

Show the operator which timebase is authoritative, and write down exactly how SDI/ST 2110/genlocked NDI plug into the seam.

**Files:** Modify `uimanager.{h,cpp}` (a reference-tier accessor + tooltip line), `recorder_engine/replaymanager.{h,cpp}` (expose the tier), `docs/native-ingest-workstream-remaining.md`, `tests/e2e/FRAMESYNC.md`.

- [ ] **Step 1:** ReplayManager exposes `Q_INVOKABLE`-friendly `int referenceTier() const { return m_timingRef ? int(m_timingRef->tier()) : 0; } bool referenceIsExternal() const { return m_timingRef && m_timingRef->isExternal(); }` and emits `referenceTierChanged(int tier, bool external)` when the PTP lock state flips (polled in `recomputeInterCamPhase` or the heartbeat). UIManager relays it to a `Q_INVOKABLE int sessionReferenceTier() const;` + a one-line tooltip/status string `"reference: PTP (locked)"` / `"reference: local monotonic"` so the session-level status surface shows the authoritative timebase.
- [ ] **Step 2: Documentation (the deliverable the spec calls for)** — add a "Reference-clock seam & genlock integration points" section to `docs/native-ingest-workstream-remaining.md` (extending the **P5** entry at `:111`) covering: (a) the `TimingReference` seam (`nowSessionNs`/`tier`/`isExternal`) is the single swap point; (b) `LocalMonotonicReference` (today) vs `PtpReference` (this phase, ST 2059 slave, SW timestamps, opt-in); (c) how a **genlocked NDI** source (PTP/genlock-disciplined NDI timestamps) becomes authoritative — its `NdiSourceClock` already tracks the SDK timestamp; with an external reference present it phase-locks truly rather than being arrival-anchored; (d) how **SDI / ST 2110** ingest (a future separate program) plugs a facility-time `TimingReference` backend + RTP/PTP-stamped capture into the same seam — the capture cards are out of scope, the seam is not; (e) the **honest ceiling**: SW-timestamped PTP is better than clockless IP but not NIC-PHC/genlock-grade; true genlock needs hardware timestamping or a genlocked capture path — surfaced via `tier()`/`isExternal()`.
- [ ] **Step 3: Verify + commit** — a UIManager unit asserts `sessionReferenceTier()` reflects a stubbed external reference; the docs render. Commit `docs(timing): document the TimingReference/PTP seam + SDI/ST2110/genlocked-NDI integration points; surface reference tier in UI`.

---

## After all tasks
- `( cd build/bcast && ctest -L unit )` incl. `tst_timingreference`, `tst_ptpservo`, `tst_ptpreference`, the UIManager reference-tier test.
- `( cd build/bcast && ctest -L native-apple-ingest -L native-rtmp -L native-ndi -L av-sync -L framesync )` green with the default `LocalMonotonicReference` (byte-identical to today; PTP opt-in only).
- Manual/integration: with `OLR_TIMING_PTP=1` against a PTP grandmaster on the LAN, `PtpReference::locked()` flips true, the UI shows `reference: PTP (locked)`, and PTP-disciplined sources report `FrameAccurate`.
- Final review: `LocalMonotonicReference` byte-identity (every gate green); the reference is read everywhere via `nowSessionMs()` (no stray `m_clock->elapsedMs()` on the timeline path); `PtpReference` falls back to local before lock (never stalls / never negative); the seam is a constructor swap (no caller restructure); SW-timestamp ceiling documented.

## Self-review
- **Spec coverage:** `TimingReference` seam top tier (`nowSessionNs`/`tier`/`isExternal`, `ReferenceTier{LocalMonotonic,RecoveredConsensus,Ptp}`) → Task 1; today's `LocalMonotonicReference` wrapping `RecordingClock` → Tasks 1-2; the whole pipeline reads `TimingReference::nowSessionNs()` (the swap is a swap, not a restructure) → Task 2 (route) + Task 5 (swap); `PtpReference` (ST 2059 slave) + a PTP client → Tasks 3-5; documented integration points so SDI/ST 2110 + genlocked NDI can become authoritative → Task 6; full hardware capture explicitly OUT of scope (DeckLink/Rivermax + NIC-PHC hardware timestamping) → noted in Global Constraints + Task 6(e). Covered.
- **Types consistent:** `TimingReference`/`ReferenceTier`/`LocalMonotonicReference`/`PtpReference`/`PtpServo`/`IPtpClient`/`nowSessionNs`/`isExternal` used identically across tasks and match the spec §5 verbatim (incl. the `ReferenceTier` enumerators). Reuses Phase 1 `RecordingClock`/`SourceClock` and Phase 4 `SourceOffsetEstimator`/`SourcePhaseEvidence::externalReference` without rename. `ReferenceTier` (the timebase tier, Phase 5) is kept distinct from `ConfidenceTier` (the per-source inter-cam tier, Phase 4) — different axes, by design.
- **No placeholders:** every code step gives complete `.h`/test bodies + exact line anchors; the PTP offset/path-delay math, the fall-back-before-lock semantics, the opt-in env gate, and the SW-timestamp ceiling are all spelled out — no "TBD"/"add error handling".
- **Behavior-preserving:** `LocalMonotonicReference` returns exactly `elapsedMs()*1e6`; PTP is opt-in and falls back to local on any failure → all existing gates stay green by construction; hardware capture is documented out-of-scope, not stubbed.
