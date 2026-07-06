# iOS frame-residency design: unified decoded-frame ledger + playhead-centered windows

**Status:** v2 — revised after adversarial review; pre-implementation
**Depends on:** the GPU-resident pipeline phases 4–5 branch (`gpu/resident-pipeline-phases-4-5`)
**Platforms:** enforcement changes are iOS-scoped; accounting unification applies everywhere

## Problem

With the GPU pipeline active on iOS (4× 1080p SRT feeds, record + play), the app's
physical footprint grows to ~3.3 GB within seconds of playback and iOS jetsam-kills it
(SIGKILL). Evidence from the on-device JetsamEvent report: OpenLiveReplay is
`largestProcess`, frontmost, `rpages = 202615` (16 KB pages ≈ 3.32 GB) — a monotonic
climb, not a spike. The CPU-only path plays the same workload within the existing
256-frame aggregate cap without incident.

### What is known vs. hypothesized

The *charged* GPU budget cannot be the whole story: `GpuBudget::tryCharge` hard-rejects
above the configured budget, whose iOS value computes to ~530 MB, and the major frame
holders (track buffers, staging/output caches, readback ring, retire queue) all hold
`FrameHandle → GpuFrameData`, which owns the charge — while they retain a frame, the
ledger does **not** drain. So ~2.8 GB of the observed footprint lives outside the
ledger. Candidate balloons, in rough order of suspicion:

1. **Uncharged producer-side GPU wraps.** The recorder's encode path
   (`StreamWorker::importGpuVideoFrameForEncode`) and the ingest decode path
   (`makeGpuDecodedFrameHandle`) mint GPU frame handles via the 3-arg
   `makeGpuFrameHandle` with an empty `GpuBudgetCharge` — never accounted, never gated.
2. **CPU readback copies.** `GpuFrameData::m_cpuCache` (one full plane set per
   read-back format per frame) and `SharedGpuReadbackCache` are invisible to the
   budget.
3. **VideoToolbox pool growth.** Eight VT sessions run concurrently (4 playback + 4
   ingest decoders). Each session's `CVPixelBufferPool` retains buffers warm at its
   high-watermark; releasing our reference returns the buffer to the pool, not the
   pages to the OS.
4. **Charge-free transient retainers.** `gpuReadbackRetains()` parks raw
   `shared_ptr<GpuSurface>` (no charge) until fence retirement — normally
   milliseconds, but unbounded if fences stall on iOS.
5. FFmpeg/SRT ingest buffering and encoder queues.

**The design is therefore evidence-gated:** Phase 0 (below) instruments all of the
above and produces per-class numbers on device *before* the enforcement and window
work proceeds. If the dominant class is not what a task assumes, the plan stops and
re-scopes at the recorded decision point.

A sizing fact that still stands: on iOS unified memory, a decoded CPU frame and a GPU
IOSurface frame cost roughly the same physical bytes (1080p NV12 ≈ 3.1 MB either way,
both against the jetsam limit). Only *fewer resident frames* and *pages actually
returned to the OS* reduce footprint — the latter requires managing VT pools, not just
dropping references.

## Requirements (agreed)

- **All four feeds scrub/frame-step in lockstep** (EVS-style multiview scrubbing), with
  a window of frames back and forth from the playhead instantly steppable.
- CPU-path functionality fully preserved: scrub anywhere; outside the resident window
  frames re-decode on demand (existing FrameIndex exact seek).
- Device target: **M-series iPad Pro**; budget adapts at runtime.
- Design point: **4× 1080p feeds**; all math derived from actual decoded frame geometry.
- Jetsam must become structurally unreachable: accounting equals reality (with the
  bounded, named exceptions below), and pressure is detected by an active watchdog —
  not only by UIKit warnings, which are known to arrive late or never for fast ramps.

## Phase 0 (gating): instrumented evidence run

Extend the ledger with **report-only mode** (`OLR_LEDGER_REPORT_ONLY=1`, delivered via
the same startup-env seam as the GPU-pipeline flag): all accounting and telemetry
active, no gating, no window changes, no ladder — a clean attribution run.

Instrumentation added for this run (and kept permanently as telemetry):

- **Per-allocation-class charges** (owner tags): `DecodeWindow`, `Staging`,
  `ReadbackRing`, `CpuReadbackCache`, `RetireQueue`, `RecorderWrap`, `IngestWrap`,
  `Other`. Producer-side wraps get charged (charge-only, never gated) so they become
  visible.
- **Per-holder occupancy** (tags alone attribute allocation class, not retention):
  readback-ring pending count/bytes, retire-queue size, readback-retainer count
  (`gpuPendingReadbackRetainCount`), sink `m_lastDelivered`, per-cache entry bytes.
- **Per-VT-session pool watermarks**: outstanding output buffers per decoder session,
  sampled periodically.
- Periodic on-device log line + the existing `recordGpuBudget` stats hook extended with
  tags; `os_proc_available_memory()` sampled alongside so ledger totals can be
  correlated with real headroom.

**Decision point (recorded in the plan):** the run names the dominant class(es). Work
that targets a class the evidence exonerates is dropped or re-scoped. The window and
ladder tasks proceed regardless (they are requirement-driven), but their sizing uses
the measured numbers.

## Design

### 1. Unified residency ledger

`GpuBudget` evolves into a single ledger for all decoded video memory — GPU surfaces
and CPU planes. Final name `FrameResidencyLedger` (`GpuBudgetCharge` →
`ResidencyCharge`); mechanical rename as the final commit. API keeps its
`tryCharge`/`charge`/`credit`/`liveBytes` shape and **strict leaf-mutex** discipline
(credits may fire while worker locks are held — e.g. evicted-frame vectors destroyed
inside `m_bufferMutex` scope — so the ledger must never take a non-leaf lock).

### 2. Charge tied to the surface's lifetime

The charge is attached to the **`GpuSurface` object itself** and credited in its
destructor — equivalent to last-reference semantics without the fixed-at-construction
problem of a `shared_ptr` deleter (surfaces are wrapped before the budget decision).

- **Gate stays at mint** (`mintGpuOrDegrade`): `tryCharge` failure follows the existing
  OOM-degrade contract — the transiently *uncharged* surface remains legal just long
  enough for the CPU-fallback readback to produce real pixels (the current degrade
  behavior), then dies. This is the one bounded, documented exception to
  "accounting == reality".
- **Producer paths are charge-only, never gated:** recorder encode wraps
  (`RecorderWrap`) and ingest decode wraps (`IngestWrap`) attach charges for
  visibility, but a saturated playback ledger must never fail an ingest/encode wrap —
  a failed wrap would trip `latchGpuEncodeCpuFallback()`, which latches CPU encode for
  the whole session. Playback pressure may not degrade recording.
- CPU frames: the CPU frame-data object charges plane bytes on construction, credits on
  destruction. `m_cpuCache` entries are charged under `CpuReadbackCache`; that tag gets
  an explicit allowance and an eviction rule — entries for frames no longer inside any
  residency window are dropped on the trim tick (readbacks cannot "degrade"; they are
  bounded by eviction instead).

### 3. Budget derived from the OS, with honest arithmetic

On iOS, at playback-session start and on every ladder event:

```
budget = min( 0.5 × (os_proc_available_memory() + ledger.liveBytes()),  available_now )
budget = clamp(budget, min(512 MB, available_now), 4 GB)
```

Adding back `ledger.liveBytes()` prevents the double-count that would otherwise shrink
the budget merely because the ledger is healthily full; the outer `min` and the
`min(512 MB, available)` floor prevent overcommit when memory is genuinely scarce. A
0-or-garbage sample (backgrounded, early startup) leaves the previous budget unchanged.
`kIosAggregateGpuFrameCeiling` is deleted. On macOS/Windows the existing peak-formula
configuration remains the enforcement input; unified accounting and tags apply
everywhere.

On the target iPad (per-process limit ≈ 3.3 GB observed), session-start availability is
~3.0 GB → budget ≈ 1.5 GB.

### 4. Playhead-centered residency windows — owning the scheduler changes

Per-feed window **half-width** (frames each direction from the playhead):

```
halfWidth = clamp((budget − nonWindowReserves) / (feedCount × frameBytes × 2), 8, 120)
```

`×2` = both directions (the CPU-readback double-cost of displayed frames is covered by
the `CpuReadbackCache` allowance inside `nonWindowReserves`, which is sized as
`2 × feedCount × frameBytes` — the steady-state display set, not the whole window).
`nonWindowReserves` otherwise reuses `GpuBudgetConfig`'s per-term estimators (staging,
output bus, readback rings). With the measured ~1.5 GB budget and 1080p × 4 feeds this
yields roughly **±35–40 frames per feed** — stated honestly; deeper windows require
either fewer feeds or more headroom. Every feed gets the same half-width, so lockstep
scrubbing is structural.

This is a **scheduler-behavior change**, not just a cap change. Delivering the trail
requires parameterizing the time-based mechanisms that currently destroy it:

- The per-iteration trim (`kTrailMs`/`kSlackMs` spans) and `OutputFrameCache::trimBefore`
  horizons derive from the window half-width (in ms at the session frame rate) instead
  of fixed 500/700 ms constants.
- The backward-step fast path (`reuseAt` output-cache coverage) must be able to serve
  the retained trail; the output cache trim horizon follows the same derived span.
- `TrackBuffer::insert` caps and the protect-range asymmetry (`protectLo/protectHi`)
  take the derived values; behind-playhead frames inside the window are no longer
  first-eviction candidates.
- **Post-seek semantics (explicit):** after a reposition, the trail is *cold* — it
  warms as material plays through. No backward prefetch in this design; stepping
  backward past decoded material re-decodes via exact seek (existing behavior, same as
  the CPU path today).

Outside the window frames are dropped; whether dropping returns pages to the OS
depends on the VT pool (next section).

### 5. VideoToolbox pool management

Dropping our last reference returns buffers to the session's pool, which keeps pages
warm at its high-watermark. Therefore:

- Phase 0 records per-session pool watermarks; the window-depth formula is validated
  on device against VT actually sustaining `halfWidth × 2` outstanding buffers per
  session (unverified today; the current cap is 8/track).
- **Ladder Levels 1–2 flush decoder pools**: the decoder `reset()` path (session
  invalidation) exists and is exercised after backward seeks; Level 1 uses the
  cheapest available mechanism (pool flush if the session supports it, else reset) so
  that trims translate into pages actually returned.
- The on-device sign-off measures `os_proc_available_memory()` recovery after Level-1
  trim + flush — not just ledger deltas — because ledger bytes falling without
  footprint falling is exactly the failure mode of a pool-blind design.

### 6. iOS memory-pressure ladder

**Sampling (the primary trigger):** the worker loop polls `os_proc_available_memory()`
once per decode iteration (cheap syscall), debounced to ~250 ms. UIKit memory warnings
(via the `IosGpuLifecycleSink` seam extended with `onMemoryWarning()`, marshaled like
suspend/resume) are a *supplementary* trigger — warnings are known to be late or absent
for fast ramps, and the observed balloon reaches the limit in seconds.

Thresholds are **absolute headroom**, not %-of-session-start (which the ledger's own
healthy holdings would depress):

- **Level 0** — normal. Re-arm: after ≥30 s with headroom above 2× the Level-1
  threshold, the budget may re-derive upward (once per minute at most).
- **Level 1** — headroom < `max(256 MB, 4 × feedCount × frameBytes)` or a UIKit
  warning: re-derive budget (per §3), shrink windows, trim distal frames, flush VT
  pools, drop out-of-window `CpuReadbackCache` entries. Telemetry counter + log.
- **Level 2** — headroom below half the Level-1 threshold after a Level-1 trim, or a
  second warning within 10 s: latch CPU-only mode for the session.

**Level-2 state machine (explicit):** latch = `GpuPipelineState::CpuFallback` plus a
dedicated `m_memoryPressureLatched` flag — **not** `GpuDeviceLossMonitor::recordLoss()`
(a monitor "lost" state would make surviving GPU frames unreadable in `readToCpu` and
block their CPU snapshots). The latch path runs `handleGpuDeviceLoss(reason)`
parameterized with `MemoryPressure`: one generation bump for sink staleness, sanitize
of caches/buffers, **no** `consumeGpuDeviceLossRebuildBudget()`, **no**
`rebuildGpuSpine()`. Warnings arriving while suspended are ignored (the suspend path
already defers GPU work). Unlatch: only on playback-session stop/start. Expected
visual artifact at latch: frames without a CPU snapshot are dropped — a brief gap on
never-displayed material — documented, not hidden. Post-latch decode caps: the CPU
branch of `capFrames()` must apply (256 aggregate), not the GPU per-track cap — the
latch flag, not `gpuPipelineEnabled()`, selects the branch.

### 7. Adjacent cleanup (in scope)

The GPU-force-on mechanism used to produce iOS GPU builds currently exists only as an
uncommitted local edit to `main.cpp` (in the phases 4–5 worktree). It lands here
properly: CMake option `OLR_GPU_PIPELINE_FORCE_ON` (default OFF) → compile definition →
the guarded `qputenv("OLR_GPU_PIPELINE", "1")` at startup, committed with this work.

## Testing

**Failing tests first — targeting the real mechanisms:**

1. **Charge-free retainer test:** a fake co-owner holds raw `shared_ptr<GpuSurface>`
   (modeled on the fence-parked readback retainer) past track-buffer eviction; assert
   live allocation count equals ledger count. Fails today (the retained surface is
   uncharged once its `GpuFrameData` dies).
2. **Uncharged producer test:** mint frames through the recorder/ingest wrap paths;
   assert the ledger sees them. Fails today (empty `GpuBudgetCharge`).
3. **Trail retention test:** with a derived window of ±N, play forward then step
   backward N−1 frames; assert zero re-decodes (fails today at ~16 frames when the
   500 ms trim bites).

Then: ledger units (charge-follows-surface-lifetime across threads and co-owners;
per-tag accounting; CPU-plane and `CpuReadbackCache` charges + eviction rule);
window-math units (half-width derivation, clamps, lockstep equality, trim-span
derivation); ladder units with injectable memory/warning sources (watchdog fires
without any UIKit warning; Level 1 provably lowers headroom-relevant usage; budget
re-derivation with the `liveBytes` add-back; floor never exceeds availability; Level 2
latches with `MemoryPressure` reason, leaves the device-loss rebuild budget untouched,
and selects the CPU cap branch; re-arm path). Existing gates stay green
(`tst_gpubudget`, `tst_gpu_budget_stress`, multi-feed pressure gate, device-loss suite,
`gpu-budget`/`seek-prefetch` e2e). New tests join the TSan CI lists.

**On-device gates (added to the iOS manual checklist as required sign-off):**

- Phase 0 attribution run (report-only): per-class/per-holder/per-pool numbers
  recorded; dominant class named; decision point exercised.
- Enforcement run: 4× SRT feeds, ≥10 min play + scrub; ledger plateau ≤ budget AND
  `os_proc_available_memory()` stable (both, per §5); 4-feed lockstep step-check across
  the full ±window; Level-1 drill (induce pressure, verify trim + pool flush recovers
  real headroom); VT sustains window-depth outstanding buffers.

## Out of scope

- Disk/cold tier, predictive/backward prefetch (existing seek-prefetch untouched).
- Output sink changes (NDI/DeckLink/AJA/OMT) beyond retention visibility.
- Audio memory. Desktop behavior beyond unified accounting/tags.
- The iOS native-SRT DNS resolution failure — separate bug, tracked separately.
