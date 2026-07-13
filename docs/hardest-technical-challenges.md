# The three hardest technical challenges — and their solutions

This document states the three highest-value open correctness problems in this
repository and delivers a worked solution for each. Every claim below is
grounded in a `file:line` reference to the current tree, and the two solutions
that admit machine checking ship as runnable artifacts under
[`docs/hardest-technical-challenges/`](hardest-technical-challenges/) — run them; do not take the verdicts on
faith.

| # | Challenge | Deliverable | Status |
|---|-----------|-------------|--------|
| 1 | Prove the playback transport never puts a wrong/stale/gray frame on air | Exhaustive interleaving model check + differential known-bug gate | **Ran: 2 new protocol holes found; repaired protocol PROVED over 10,280 states** |
| 2 | Rate-agnostic timecode alignment with a proven phase-error bound | Falsifier + exact-rational bound sweep + drop-in C++ module | **Ran: shipped code RED (−5000 ms on aligned cameras); replacement 0 violations / 3,969 cells, zero slack** |
| 3 | Type-enforced GPU surface lifetime + real-backend device-loss falsifiability | Type-state protocol (compile-fail on misuse) + contained real-TDR worker-recovery lane | **Implemented: handle gate, epoch-bound loss authority, real fence/TDR evidence, production worker recovery** |

---

## Challenge 1 — Mechanized proof (or refuting schedule) for transport frame accuracy

### The problem

The on-air media time is sampled from a play epoch held by the output
dispatcher ([`outputframeclock.cpp:16-20`](../playback/output/outputframeclock.cpp)),
decoupled from the CommitGate-visible playhead
([`commitgate.h:12-16`](../playback/commitgate.h),
[`playbackworker.cpp:2371-2372`](../playback/playbackworker.cpp)). The two are
re-coupled across a playhead discontinuity only by a manual
`resetPlayEpoch()` obligation replicated at 6+ sites
([`playbackworker.cpp:440,468,487,516,4130,4489`](../playback/playbackworker.cpp))
whose helpers ship opposite defaults. A missed re-anchor is **not a data race**
— TSan is structurally blind to it — and the only guard is an observational
counter ([`outputdispatcher.cpp:299-303`](../playback/output/outputdispatcher.cpp)).
The class shipped broken twice (far-back seek; armed cut).

### The artifact

[`docs/hardest-technical-challenges/transport_epoch_modelcheck.py`](hardest-technical-challenges/transport_epoch_modelcheck.py)
— an exhaustive (bounded) explicit-state model checker over the real
synchronization skeleton: UI seek republish/bump as separate atomic steps
(`:227` / `:241` — the output thread reads these atomics without `m_mutex`),
worker reposition begin/commit (`CommitGate::canCommitReposition`,
`:3352-3397`) plus the post-commit `resetPlayEpoch()` / `dispatchImmediate()`
pair (`:459-469`), and the background tick split exactly as
[`outputruntime.cpp`](../playback/output/outputruntime.cpp) does it: snapshot
**outside** the lock (`:362`, which also fires a due armed cut inside
`makeOutputSnapshot`, `:2322` → `:4042-4150`), then the lease re-check block
(`:365-375`: `immediateDispatchRequests` / `configGeneration` /
`nextOutputFrameIndex`) and `dispatchTick` (`:380`).

**Soundness of the abstraction.** Every shared access in the modeled skeleton
is either mutex-guarded or a paired release/acquire atomic on exactly the
variables the invariant reads (the one `relaxed` store of
`m_committedPlayheadMs` at `:3389` is ordered by the release store of
`m_committedGeneration` at `:3396`, and the snapshot reads the generation with
`acquire` at `:2344` *before* acting on the playhead). Sequentially-consistent
interleaving of whole critical sections is therefore sound for the
logic-omission bug class this checks. Full weak-memory coverage (e.g. GenMC
over the extracted `<atomic>` skeleton) is the recommended second stage; it
cannot *remove* counterexamples found here.

**The invariant** (both directions of the challenge property): on every tick
that renders a non-placeholder frame from a snapshot taken with the gate open
(`committedGen == seekGen`), `|sampledPlayhead − visiblePlayhead| ≤ 1` frame;
gate-closed transients are exempt exactly as the production e2e treats them.

### Verdicts (run it yourself)

```
python docs/hardest-technical-challenges/transport_epoch_modelcheck.py all
```

```
head   ->  COUNTEREXAMPLE (6-step schedule)
fixed  ->  PROOF  -- invariant holds over all 10,280 states
mutA   ->  COUNTEREXAMPLE (historical far-back shape, 5 steps)
mutB   ->  COUNTEREXAMPLE (historical armed-cut shape, 9 steps)

differential gate: [OK] head  [OK] fixed  [OK] mutA  [OK] mutB
```

The differential gate discharges model-fidelity: the unedited protocol and
both historical mutations *must* refute while the repaired protocol *must*
prove — a vacuous model cannot pass all four.

### Two previously-unknown protocol holes (the `head` counterexamples)

**H1 — swallowed reset.** `resetPlayEpoch()` from the worker with no dispatch
active applies immediately ([`outputruntime.cpp:114-126`](../playback/output/outputruntime.cpp))
and bumps **none** of the three values the background loop re-checks at
`:371-372`. Schedule: BG takes a pre-commit snapshot (`:362`) → worker commits
and resets the epoch → BG's re-checks all pass → BG dispatches the stale
snapshot and **re-anchors the freshly-cleared epoch at the pre-seek playhead**.
Every subsequent tick renders a real frame at the wrong media time.

**H2 — commit-to-reset gap.** The commit (`:3389-3397`) and the epoch reset
(`refreshOutputAfterSeekCommit`, `:459-469`) are separate steps with no
barrier between them. Ticks landing in the gap render gate-open frames
against the stale anchor (the model's 6-step `head` schedule). The armed-cut
path does **not** have this hole — it resets inside the `m_bufferMutex`
critical section at `:4130` — which is precisely the repair shape.

Both holes are µs-wide against a ~1 ms tick cadence, which is why the e2e has
never caught them; a model checker does not care about window width. On a
far-back seek the divergence magnitude is the jump distance (up to tens of
seconds), rendered with `isPlaceholder=false` and zero errors reported.

### The repair (proved by the `fixed` config)

1. **F1** — `resetPlayEpoch()` (every site, including the deferred-pending
   path) also bumps `m_configGeneration`, so any in-flight pre-reset snapshot
   fails the `:371` re-check and is discarded. (~2 lines in
   `outputruntime.cpp`; `dispatchImmediateWithReport` already re-checks the
   same generation, so immediates are covered for free.)
2. **F2** — the reposition-commit applies the epoch reset **atomically inside
   the commit's critical section**, exactly as the cut fire already does at
   `:4130` (the lock-order note there documents why taking the runtime mutex
   under `m_bufferMutex` is safe). `refreshOutputAfterSeekCommit(bool)` then
   loses its `resetPlayEpoch` parameter entirely — deleting the
   opposite-defaults trap at [`playbackworker.h:371-372`](../playback/playbackworker.h).

Neither repair adds hot-path cost: F1 is one increment under a mutex already
held; F2 moves an existing call.

### Fidelity / correspondence table

| Model element | Source | Order / guard |
|---|---|---|
| `sg` | `m_seekGeneration` `:241` fetch_add | release / acquire (`:2345`, `:4066`) |
| `cg` | `m_committedGeneration` `:3396` | release / acquire (`:2344`) |
| `cp` | `m_committedPlayheadMs` `:227` (release), `:3389` (relaxed, ordered by `:3396`) | acquire (`:2343`) |
| `ag` | `m_armSeekGen` `:3732` | release / acquire (`:4067`) |
| `sched` | `m_scheduledCutFrame` `:4006` | seq_cst |
| `lv` | `m_lastVisiblePlayheadMs` `:227,2489,3390` | release / acquire (multi-writer) |
| `publ/pubh` | published cache slot `m_publishedCache` `:2338` | slot lock |
| `eh/ea/ef` | `m_havePlayEpoch`/`m_playEpoch` ([`outputdispatcher.h:195-196`](../playback/output/outputdispatcher.h)) | dispatch lease only (non-atomic) |
| `cfg`, re-checks | `m_configGeneration`, `:371-372` | `OutputRuntime::m_mutex` |
| cut fire | `maybeFireScheduledCut` `:4042-4150` | `m_bufferMutex`, on the output thread, **before** the lease (`:362` vs `:373`) |
| render/sample | `clockedStateForTick` `:443-464`; `samplePlayheadMsForOutputTick` | dispatch lease |

Deliberate abstractions: single feed, exact coverage predicate (the
bookmark/cache-guard layers are collapsed into "covered"), speed fixed at 1,
immediate dispatch modeled atomically (its internal optimistic re-check always
sees its own consistent capture; the H1/H2 interleavings do not depend on
splitting it), time = dispatched frames (transport and frame index advance in
lockstep, as they do against wall time in production).

### How to apply

- Land F1+F2 (with the CLAUDE.md-mandated independent concurrency review).
- Move the checker under `tests/formal/` and wire `transport_epoch_modelcheck.py
  all` into CTest (fast unit label; it is milliseconds of pure Python) — any
  future edit to the re-anchor protocol must re-prove the invariant, turning the
  manual review requirement into a machine gate. (Follow-up PR: the artifacts
  ship beside this document for now so the change stays docs-only; the CTest
  wiring should land from a machine where the full pre-push delivery gate runs.)
- Optional second stage: extract the `<atomic>` skeleton verbatim into a GenMC
  harness for weak-memory-complete coverage of the same actors.

---

## Challenge 2 — Rate-agnostic timecode alignment with a proven bound

### The problem (confirmed dimensional bug)

`TimecodeAligner` counts TC frames at a hardwired nominal 30 fps
([`timecodealigner.cpp:6-9`](../recorder_engine/timing/timecodealigner.cpp),
constructed at `Smpte12m::kTimecodeNominalFps` —
[`replaymanager.h:286`](../recorder_engine/replaymanager.h),
[`smpte12m.h:29`](../recorder_engine/timing/smpte12m.h)) while the
session-frame axis advances at the real rate `m_fps`. `frameOffset()`
differences the two units (`:39-52`); for two genuinely aligned sources whose
anchors are Δt seconds apart the spurious term is `−(m_fps/30 − 1)·(TC frame
difference)`. The result feeds a real servo trim
(`rawTargetMs = frames·1000/m_fps`,
[`replaymanager.cpp:739-740`](../recorder_engine/replaymanager.cpp)) that
saturates the ±80 ms cap ([`streamworker.h:67`](../recorder_engine/streamworker.h))
and pulls aligned cameras **apart** at the flagship 50p/59.94p rates. The
"correct at ANY source rate" comment (`smpte12m.h:26-28`) is false off 30, and
the unit tests never see it because they always set aligner fps == TC-encode
fps ([`tst_timecodealigner.cpp:14-48`](../tests/unit/tst_timecodealigner.cpp)).
Separately, the operator-facing `boundMs` constants
([`sourceoffsetestimator.cpp:24-38`](../recorder_engine/timing/sourceoffsetestimator.cpp))
are asserted, never derived.

### The theorem

Let source *i* stamp frame *k* with per-frame TC (SEI/ATC), TC rate
`r_i = num_i/den_i`; session heartbeat rate `R`, epoch `S`, relative clock
drift `d` (ppm); pipeline latency `L_i`. Define the **anchor skew**

```
skew_i = sessionFrame_i / R  −  tcFrames_i / r_i        (exact rational, seconds)
reported(A,B) = (skew_A − skew_B) · 1000                 (ms)
```

With `true(A,B) = (L_A − L_B)·1000` (the correction the servo must apply):

- **E (exact) regime** — common TC generator, both rates known, arrivals
  phase-locked to the heartbeat: `reported == true` **exactly** (the session
  quantization terms `q_i/R` are equal and cancel).
- **Q (quantization) regime** — common generator, arbitrary rates/phases:
  `|reported − true| ≤ 1000/R` (one session frame; the two `floor`
  quantizations differ by at most one grid step).
- **D (drift) regime** — session clock drifting `d` ppm against the TC
  generator, anchors Δt s apart: add `|Δt|·|d|·10⁻³ ms` (the drift term is
  proportional to the **anchor skew**, the quantity the current code turns
  into an unbounded error).

```
B = 1000/R  +  |anchorSkewSeconds| · |driftPpm| · 1e-3   (ms)
```

Every term maps to a runtime quantity the engine already has: `R` from
`m_fpsNum/m_fpsDen`, anchor skew from the two anchors, drift from
`DriftEstimator::ppm`. If either source's rate is unrecoverable, the
comparison is **Incomparable** — a typed state, never a bare integer.

### Machine check (run it yourself)

[`docs/hardest-technical-challenges/timecode_alignment_proof.py`](hardest-technical-challenges/timecode_alignment_proof.py)
— exact-rational (`fractions.Fraction`) ground-truth model; faithful port of
the shipped algorithm (including the producers' `to100ns(tc, 30)` sawtooth
encoding of >30 fps TC fields); the rate-aware replacement; and:

```
falsifier    : shipped  frameOffset = −300 → servo target −5000 ms (cap ±80)
               replacement offset = 0.000 ms          (true offset is 0)
exactness    : reported == true EXACTLY in 52 phase-locked cells
bound sweep  : 3,969 cells (7 rates² × skew × offset × phase × drift)
               replacement violations: 0   (worst slack 0.000000 ms — tight)
               shipped violations   : 1,809 (45.6% of cells)
drop-frame   : 00:00:59;29 → 00:01:00;02 counts contiguous (1799 → 1800)
incomparable : unknown rate → typed sentinel, never a silent integer
```

The falsifier is the challenge's acceptance case verbatim: two 60 fps sources,
anchors 10 s apart, genuinely aligned.

### Drop-in C++ module

Pure, Qt-free, exact-integer arithmetic (`recorder_engine/timing/`), replacing
`TimecodeAligner`:

```cpp
// timecodealignerv2.h — rate-aware inter-source alignment with a typed
// Incomparable state. Pure (no Qt/FFmpeg). All arithmetic exact int128.
#ifndef TIMECODEALIGNERV2_H
#define TIMECODEALIGNERV2_H
#include <cstdint>

struct FrameRateQ {                    // exact rational frames/second
    int32_t num = 0, den = 1;          // e.g. 60000/1001
    bool valid() const { return num > 0 && den > 0; }
};

struct AlignmentOffset {
    enum class Kind : uint8_t { Exact, Bounded, Incomparable };
    Kind kind = Kind::Incomparable;
    int64_t offsetUs = 0;              // (skewA − skewB), microseconds
    int64_t boundUs = 0;               // proven |error| bound for this pair
};

class TimecodeAlignerV2 {
public:
    static constexpr int kMaxSources = 16;

    // First observation wins (immutable anchor, as today).  tcFrames is the
    // absolute TC frame count AT THE SOURCE'S TRUE RATE (producers call
    // Smpte12m::toFrameCount(tc, fields-rate) and pass the true rate
    // alongside — the lossy to100ns(tc, 30) intermediate is retired).
    void observe(int source, int64_t tcFrames, FrameRateQ tcRate,
                 int64_t sessionFrame, FrameRateQ sessionRate);

    bool hasTimecode(int source) const;

    // offset(A,B): time to ADD to B's mapping so equal-TC frames coincide
    // with A.  Never a bare number: rate-mismatched or rate-unknown pairs
    // return Kind::Incomparable.  boundUs = one session frame + the caller-
    // supplied drift term (driftPpm × anchor skew).
    AlignmentOffset offset(int a, int b, int32_t driftPpm = 0) const;

    // Session-frame mapping for source's media TC (replaces the V1 method);
    // single rounding at the session-rate boundary.
    int64_t toSessionFrameIndex(int source, int64_t mediaTcFrames) const;

    void reset();

private:
    struct Anchor {
        bool set = false;
        int64_t tcFrames = 0;
        FrameRateQ tcRate;
        int64_t sessionFrame = 0;
        FrameRateQ sessionRate;
    };
    Anchor m_anchors[kMaxSources];
};
#endif
```

```cpp
// timecodealignerv2.cpp — the arithmetic core. skew comparisons cross-
// multiply through __int128 so no intermediate overflows or rounds.
#include "timecodealignerv2.h"

namespace {
using I128 = __int128;
// microseconds(frames/rate) with a single, explicit round-to-nearest
int64_t usFor(int64_t frames, FrameRateQ r) {
    const I128 n = I128(frames) * 1'000'000 * r.den;
    const I128 d = I128(r.num);
    return int64_t((n >= 0 ? n + d / 2 : n - d / 2) / d);
}
} // namespace

void TimecodeAlignerV2::observe(int s, int64_t tcFrames, FrameRateQ tcRate,
                                int64_t sessionFrame, FrameRateQ sessionRate) {
    if (s < 0 || s >= kMaxSources || tcFrames < 0) return;
    if (!tcRate.valid() || !sessionRate.valid()) return;   // no anchor: stays
    Anchor& a = m_anchors[s];                              // Incomparable
    if (a.set) return;
    a = Anchor{true, tcFrames, tcRate, sessionFrame, sessionRate};
}

bool TimecodeAlignerV2::hasTimecode(int s) const {
    return s >= 0 && s < kMaxSources && m_anchors[s].set;
}

AlignmentOffset TimecodeAlignerV2::offset(int ia, int ib,
                                          int32_t driftPpm) const {
    AlignmentOffset out;                                    // Incomparable
    if (!hasTimecode(ia) || !hasTimecode(ib)) return out;
    const Anchor& A = m_anchors[ia];
    const Anchor& B = m_anchors[ib];

    // skew_i = sessionFrame/R − tcFrames/r, in exact microseconds
    const int64_t skewA = usFor(A.sessionFrame, A.sessionRate)
                        - usFor(A.tcFrames, A.tcRate);
    const int64_t skewB = usFor(B.sessionFrame, B.sessionRate)
                        - usFor(B.tcFrames, B.tcRate);
    out.offsetUs = skewA - skewB;

    // bound: one session frame (worst of the two session rates) + drift×skew
    const auto framePeriodUs = [](FrameRateQ r) {
        return int64_t((I128(1'000'000) * r.den + r.num - 1) / r.num);
    };
    const int64_t q = framePeriodUs(A.sessionRate) > framePeriodUs(B.sessionRate)
                          ? framePeriodUs(A.sessionRate)
                          : framePeriodUs(B.sessionRate);
    const int64_t tcSkewUs = usFor(A.tcFrames, A.tcRate)
                           - usFor(B.tcFrames, B.tcRate);
    const int64_t drift = int64_t((I128(tcSkewUs < 0 ? -tcSkewUs : tcSkewUs)
                                   * (driftPpm < 0 ? -driftPpm : driftPpm))
                                  / 1'000'000);
    out.boundUs = q + drift;
    // Exact iff same rate, same generator assumed, zero drift term claimed.
    out.kind = (A.tcRate.num == B.tcRate.num && A.tcRate.den == B.tcRate.den &&
                drift == 0)
                   ? AlignmentOffset::Kind::Exact
                   : AlignmentOffset::Kind::Bounded;
    return out;
}

int64_t TimecodeAlignerV2::toSessionFrameIndex(int s, int64_t mediaTcFrames) const {
    if (!hasTimecode(s)) return -1;
    const Anchor& a = m_anchors[s];
    // sessionFrame = anchor.sessionFrame + (Δtc time) × R, one rounding
    const I128 dtUsNum = I128(mediaTcFrames - a.tcFrames) * 1'000'000
                       * a.tcRate.den;
    const I128 dtUs = dtUsNum / a.tcRate.num;
    const I128 df = (dtUs * a.sessionRate.num + I128(500'000) * a.sessionRate.den)
                  / (I128(1'000'000) * a.sessionRate.den);
    return a.sessionFrame + int64_t(df);
}

void TimecodeAlignerV2::reset() {
    for (auto& a : m_anchors) a = Anchor{};
}
```

**Wiring** (confined to the producer seam + ReplayManager, as required):

- Producers stop collapsing TC through `to100ns(tc, 30)`
  ([`nativesrtingestsession.cpp:1059`](../recorder_engine/ingest/nativesrtingestsession.cpp),
  [`nativertmpingestsession.cpp:1280`](../recorder_engine/ingest/nativertmpingestsession.cpp))
  and instead emit `(toFrameCount(tc, fieldsRate), trueRateQ)`; a source whose
  true rate cannot be recovered emits an invalid `FrameRateQ` → the aligner
  never anchors → `Incomparable` (explicit fallback, no guessing).
- `ReplayManager` constructs the aligner without a nominal rate and converts
  `offset()` to the servo target as
  `rawTargetMs = offsetUs / 1000` directly (deleting the `frames·1000/m_fps`
  conversion at `:740`); `Incomparable` falls through to the existing
  clock-offset estimate branch (`:741-745`) — graceful degradation preserved.
- At 30 fps with common TC the new module returns exactly the V1 answers, so
  every existing green test stays green.

**Proven `boundMs` replacements** for
[`sourceoffsetestimator.cpp`](../recorder_engine/timing/sourceoffsetestimator.cpp):

| Tier | Today | Derived bound |
|---|---|---|
| FrameAccurate (common TC) | `0` | `B = 1000/R + skew·ppm·1e-3` ms — computable live from the aligner + `DriftEstimator::ppm`; report it instead of asserting 0 |
| Bounded / PCR | `4 + ppmTerm` | `pcrQuantMs (27 MHz → ~0) + servo residual (kServoStepMs granularity) + ppm·window` — the base constant becomes the servo-step granularity, a named quantity |
| Bounded / FlvPll | `+8` | `1 ms FLV timestamp quantization + PLL settling residual (measurable from the PLL's own error accumulator) + ppm·window` |
| Approximate | `40` | unchanged — honest arrival-jitter ceiling, now documented as such |

The harness's regime D cells check the FrameAccurate formula; the PCR/FLV
formulas are analytic given the tier's clock model (the model parameters —
real drift/jitter magnitudes — come from the phase-6 two-clock rig; the proof
obligation here is `B(X)` given `X`, which the sweep discharges).

---

## Challenge 3 — Type-enforced GPU surface lifetime + real-backend device-loss falsifiability

### The problem

The invariant "a surface's native backing is freed only after every GPU op
reading it has retired" is pure convention: `retainUntilFenceRetired` /
`pendingFenceValue` are no-op base virtuals
([`gpusurface.h:33-34`](../playback/gpu/gpusurface.h)), and **both** deferral
registries silently drop unregistered surfaces
([`gpuframeretirequeue.cpp:15-16`](../playback/gpu/gpuframeretirequeue.cpp),
[`gpureadbackretainer.cpp`](../playback/gpu/gpureadbackretainer.cpp) fence==0
early-return). A new op site that forgets the calls compiles clean, passes
every unit test, and frees a surface the GPU may still read. Device-loss
recovery frees fences **without waiting** on the undischarged assumption that
the device is truly dead
([`playbackworker.cpp:1420,1438-1445`](../playback/playbackworker.cpp)); the
Windows spine leaves fences NULL (`:1789-1790`) so its safety rests on a
*different*, untyped model (synchronous `CopySubresourceRegion`+`Map`); and
the whole recovery path is exercised only via an atomic-bool
`injectDeviceLostForTest` on Null/WARP — real device removal is structurally
unfalsifiable in CI.

### Solution part 1 — make the illegal states unrepresentable

The load-bearing move: **fuse handle access with obligation registration**, so
there is nothing left to forget. `GpuSurface::nativeHandle()` (and the two
retain virtuals) become `protected`, with the lease types as friends. A GPU op
site can then obtain the native handle *only* through a scope whose
destruction *is* the discharge.

```cpp
// playback/gpu/gpusurfacelease.h — platform-neutral (no SDK types, per the
// gpusurface.h:17-19 rule). C++17.
#ifndef OLR_GPUSURFACELEASE_H
#define OLR_GPUSURFACELEASE_H
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include <memory>
#include <vector>

class GpuRetireRegistry;   // retire queue + global readback retainer facade

// Provenance-bound proof that the device is REALLY dead. Constructible only
// by the two driver-authoritative observation sites; the test-injection path
// (injectDeviceLostForTest) cannot mint one.
class DeadDeviceToken {
public:
    enum class Provenance : uint8_t { DxgiDeviceRemovedReason,   // win
                                      RhiFrameOpDeviceLost };    // apple/rhi
    Provenance provenance() const { return m_p; }
    uint64_t observedGeneration() const { return m_gen; }
private:
    DeadDeviceToken(Provenance p, uint64_t gen) : m_p(p), m_gen(gen) {}
    // Defined ONLY in gpurhicontext_win.cpp (asserts FAILED(hr) from
    // GetDeviceRemovedReason, :238-242):
    friend DeadDeviceToken mintDeadDeviceTokenFromDxgi(long failedHr,
                                                       uint64_t gen);
    // Defined ONLY in gpurhicontext_apple.mm (FrameOpDeviceLost /
    // rhi->isDeviceLost(), :642-648):
    friend DeadDeviceToken mintDeadDeviceTokenFromFrameOp(uint64_t gen);
    Provenance m_p;
    uint64_t m_gen;
};

// Move-only view of a surface INSIDE one GPU op. The only public route to
// The following is the original design sketch. The implementation uses a
// self-contained, independently owned native snapshot: escaping a lease cannot
// create a borrowed-handle UAF. Shared-surface reads alias the existing control
// block and add no allocation; raw-surface reads retain the native object.
class GpuReadLease {
public:
    GpuReadLease(GpuReadLease&&) noexcept = default;
    GpuReadLease(const GpuReadLease&) = delete;
    void* nativeHandle() const { return m_surface->nativeHandle(); } // friend
    GpuSurfaceDesc desc() const { return m_surface->desc(); }
private:
    friend class GpuOpScope;
    friend class SyncReadbackScope;
    explicit GpuReadLease(std::shared_ptr<GpuSurface> s)
        : m_surface(std::move(s)) {}
    std::shared_ptr<GpuSurface> m_surface;
};

// One FENCED GPU submission (Apple model; Windows import path when it mints
// real ID3D11Fence). Destruction IS the discharge: it signals the fence once,
// stamps every leased surface's pending watermark with the value
// (retainUntilFenceRetired — monotonic max preserved), and registers each
// into the retire registry. Impossible to exit the scope without either the
// registration or a compile error at the call site that tried to bypass it.
class GpuOpScope {
public:
    GpuOpScope(std::shared_ptr<GpuFence> fence, GpuRetireRegistry& registry);
    GpuOpScope(const GpuOpScope&) = delete;
    ~GpuOpScope();                          // signal -> stamp -> register
    GpuReadLease read(std::shared_ptr<GpuSurface> s);   // records + leases
private:
    std::shared_ptr<GpuFence> m_fence;
    GpuRetireRegistry& m_registry;
    std::vector<std::shared_ptr<GpuSurface>> m_leased;
};

// The WINDOWS synchronous model, typed. The scope hands out leases and its
// destructor asserts the caller invoked complete() — which the wrapped
// CopySubresourceRegion+Map helper calls after Unmap returns, i.e. after the
// GPU read has provably completed on the immediate context
// (wingpuimportedge.cpp:441-497). No fence exists and none is pretended.
class SyncReadbackScope {
public:
    explicit SyncReadbackScope(/* immediate-context tag */);
    ~SyncReadbackScope();                   // asserts m_completed
    GpuReadLease read(std::shared_ptr<GpuSurface> s);
    void complete();                        // called by the map/unmap helper
private:
    bool m_completed = false;
    std::vector<std::shared_ptr<GpuSurface>> m_leased;
};

// Registry facade: fence-gated release is the ONLY public free path…
class GpuRetireRegistry {
public:
    void registerRetire(std::shared_ptr<GpuSurface>, std::shared_ptr<GpuFence>,
                        uint64_t fenceValue);
    int drain(int timeoutMs, int* stalls, int maxWaits);   // as today
    // …and the no-wait free REQUIRES the token. There is no overload without
    // it. handleGpuDeviceLoss(injected) routes to drainWithBoundedWait()
    // instead — safe on a live device precisely because its fences advance.
    void abandonAllNoWait(const DeadDeviceToken&);
    int drainWithBoundedWait(int perFenceTimeoutMs);
};
#endif
```

And in [`gpusurface.h`](../playback/gpu/gpusurface.h):

```cpp
class GpuSurface {
public:
    virtual ~GpuSurface() = default;
    virtual GpuSurfaceDesc desc() const = 0;
    virtual bool isValid() const = 0;
protected:                                    // was public — THE enforcement
    friend class GpuReadLease;
    friend class SyncReadbackScope;
    friend class GpuOpScope;
    virtual void* nativeHandle() const = 0;
    virtual void retainUntilFenceRetired(uint64_t v) { (void) v; }
    virtual uint64_t pendingFenceValue() const { return 0; }
};
```

**Why this discharges the challenge's acceptance criteria:**

- *Forgot-to-retain at a new op site is a compile error*: the handle is
  unreachable without a lease; a lease is unobtainable without a scope; scope
  destruction registers unconditionally. (Negative-compile test: a CMake
  `try_compile` that calls `surface->nativeHandle()` directly and asserts
  failure.)
- *The "truly dead?" assumption is discharged by provenance*: the no-wait free
  (`abandonAllNoWait`) requires a `DeadDeviceToken` mintable only at the two
  driver-authoritative sites. `injectDeviceLostForTest` exercises the full
  recovery orchestration through the *waiting* drain — which is safe on a live
  device *because* its fences advance. Greppable and type-checked: no path
  mints a token from the injection flag.
- *The Windows asymmetry is typed, not silent*: the spine's sync model becomes
  an explicit `SyncReadbackScope` (its safety — GPU read completes before
  `Map` returns — asserted by `complete()`), and the fence-shaped path
  requires a real fence. An inert NULL fence can no longer masquerade as a
  safety mechanism: `GpuOpScope` takes the fence by value and a null fence is
  a constructor precondition failure, not a silent no-op.
- *Hot-path cost*: identical to today — one atomic watermark max per surface
  plus the same registry appends; the scope is a stack object with a small
  vector.

**Per-site migration** (all six; partial migration is rejected by
construction once `nativeHandle()` goes protected — un-migrated sites stop
compiling, which is the point):

| Site | Today | After |
|---|---|---|
| [`gpusurfaceallocator.cpp:100-105`](../playback/gpu/gpusurfaceallocator.cpp) | mint + manual retain pair | mint returns the surface; first use leases via `GpuOpScope` |
| [`gpuframedata.cpp:109-112`](../playback/gpu/gpuframedata.cpp) (Apple readback) | re-signal + re-retain by hand | `GpuOpScope scope(renderFence, registry); auto lease = scope.read(surface);` readback via `lease.nativeHandle()` |
| [`gpucompositor.cpp:531-536`](../playback/gpu/gpucompositor.cpp) (output mint) | manual stamp | compositor's compose call takes the scope; output surface leased for the compose op |
| [`wingpuimportedge.cpp:383-388`](../playback/output/win/wingpuimportedge.cpp) (import mint) | manual, real D3D fence exists here | `GpuOpScope` with the import fence |
| [`wingpuimportedge.cpp:500-503`](../playback/output/win/wingpuimportedge.cpp) (sync readback) | relies on implicit synchronous completion | `SyncReadbackScope`; `complete()` invoked after `Unmap` |
| [`vtkeepsurfaceimporter_apple.mm:36-41`](../playback/gpu/vtkeepsurfaceimporter_apple.mm) | manual keep | `GpuOpScope` with the VT decode fence |

`handleGpuDeviceLoss` (`:1385-1482`) becomes
`handleGpuDeviceLoss(DeadDeviceToken)` for the real path (existing behavior:
`abandonAllNoWait`, fence resets, generation bump, rebuild budget) and
`handleGpuDeviceLoss(InjectedDeviceLossForTest)` for tests (identical
orchestration, `drainWithBoundedWait` instead of the no-wait free). No fence
wait is added to the real path — the LOCK RULE at `:1438-1440` is preserved.

### Solution part 2 — real-backend fault harness (falsifiability)

Opt-in Windows CI lane (`OLR_GPU_FAULT_LANE=1`, mirroring the
`OLR_PREPUSH_FULL` / `SKIP_*` convention; `SKIP` cleanly when the adapter is
WARP/Null, mirroring the QSKIP-on-no-RHI pattern at
[`tst_gpu_devicelost_worker.cpp:157,174`](../tests/unit/tst_gpu_devicelost_worker.cpp)):

1. **Genuine device removal.** A Job-contained child creates a real
   `PlaybackWorker` on the admitted adapter and dispatches a bounded-infinite
   compute shader on that worker's exact D3D11 device. The shader repeatedly
   reads the exact worker-cache NV12 surface whose fenced retain is under test;
   its loop condition is stored separately and remains immutable. Windows TDR
   (`TdrDelay`, default 2 s) resets the adapter; the app under test observes a
   **real** `GetDeviceRemovedReason()` failure
   (`DXGI_ERROR_DEVICE_HUNG`/`DEVICE_RESET`) at
   [`gpurhicontext_win.cpp:238-242`](../playback/gpu/gpurhicontext_win.cpp) —
   the exact site that mints the `DeadDeviceToken`. Total wall clock
   ≈ 2 s trigger + ≤ 10 s recovery: fits the opt-in budget. (Deterministic
   alternative where a D3D12 runtime is present: a `ID3D12Device::RemoveDevice`
   sidecar for the *detection* half; the TDR path remains the end-to-end
   trigger.)
2. **Real fence-ordering probe.** On the import fence: issue a long dispatch,
   signal value *v*, assert `completedValue() < v` while the GPU is busy and
   that the retire registry holds the surface; after idle, assert completion
   and release. This validates on real hardware the property the stub fence
   trivializes.
3. **Assertions** (replacing atomic-bool checks): device-loss event recorded;
   GPU generation advanced; active-epoch real-loss token published; no wait on
   the dead fence; pending retains released; the pre-loss GPU frame rejected;
   its cached CPU fallback preserved; and coherent production output resumed
   within 10 seconds. The child flushes a structured removal checkpoint before
   recovery and a recovery-result checkpoint afterward, so abnormal post-TDR
   exits retain parsed driver evidence as well as bounded raw output.
4. **Mutations the lane must catch** (proving falsifiability):
   - *(a)* route the injected-loss overload to `abandonAllNoWait` (bypassing
     the token) → on a **live** device the abandoned surfaces are still
     GPU-referenced; the debug layer + ASan flag the use-after-free.
   - *(b)* delete the lease at one migrated op site → **fails to compile**
     (the negative-compile test in CI demonstrates the same for new sites).

### How to apply

Land `gpusurfacelease.h` + the `GpuSurface` visibility change; migrate the six
sites one at a time behind the existing unit + `e2e_play` + `devicelost`
gates (CPU-off path stays byte-identical — this is a typing change, not a
behavior change); convert `handleGpuDeviceLoss` to the token/injected
overloads; add the negative-compile check and the opt-in fault lane. Per
CLAUDE.md, the `playbackworker.cpp` changes take an independent concurrency
review.

---

## Provenance

Formulated as oracle-grade challenge briefs and then solved against the tree
at the referenced lines. Machine-checked artifacts:
[`docs/hardest-technical-challenges/transport_epoch_modelcheck.py`](hardest-technical-challenges/transport_epoch_modelcheck.py)
(differential verdicts: head/mutA/mutB → counterexamples, fixed → proof) and
[`docs/hardest-technical-challenges/timecode_alignment_proof.py`](hardest-technical-challenges/timecode_alignment_proof.py)
(falsifier + 3,969-cell bound sweep, 0 violations). Line references anchor to
the working tree at the time of writing; re-verify against HEAD before acting.
