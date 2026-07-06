# iOS frame-residency design: unified decoded-frame ledger + playhead-centered windows

**Status:** v3 — revised after two adversarial reviews; approved for planning
**Depends on:** the GPU-resident pipeline phases 4–5 branch (`gpu/resident-pipeline-phases-4-5`)
**Platforms:** enforcement changes are iOS-scoped; accounting unification applies everywhere;
desktop scheduler defaults are unchanged (see §4 parameterization)

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
   `makeGpuFrameHandle` with an empty `GpuBudgetCharge` — never accounted, never
   gated. The recorder's frame-queue backstop alone admits up to `10 × fps` frames
   per source (~930 MB/source at 1080p30) in a burst.
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
  a window of frames back and forth from the playhead instantly steppable. The window
  is **asymmetric by design** (§4): the backward trail is the scarce resource this
  design provides; forward stepping is served by the existing decode-ahead lead.
- CPU-path functionality fully preserved: scrub anywhere; outside the resident window
  frames re-decode on demand (existing FrameIndex exact seek).
- Device target: **M-series iPad Pro**; budget adapts at runtime.
- Design point: **4× 1080p feeds**; all math derived from actual decoded frame geometry.
- Jetsam must become structurally unreachable **while ingest or playback runs**:
  accounting equals reality (with the bounded, named exceptions below), and pressure is
  detected by an active watchdog that also covers allocation bursts — not only by UIKit
  warnings, which are known to arrive late or never for fast ramps.

## Phase 0 (gating): instrumented evidence run

Extend the ledger with **report-only charges** (`OLR_LEDGER_REPORT_ONLY=1`, delivered
via the same startup-env seam as the GPU-pipeline flag): all *new* charges (producer
wraps, CPU planes, caches) are accounted but never gated, and no window or ladder
changes are active. The **existing** mint gate stays ON in this mode so the run is
observationally identical to the crashing configuration plus visibility.

Instrumentation added for this run (and kept permanently as telemetry):

- **Per-allocation-class charges** (owner tags): `DecodeWindow`, `Staging`,
  `ReadbackRing`, `CpuReadbackCache`, `RetireQueue`, `RecorderWrap`, `IngestWrap`,
  `Other`.
- **Per-holder occupancy** (tags attribute allocation class, not retention):
  readback-ring pending count/bytes, retire-queue size, readback-retainer count
  (`gpuPendingReadbackRetainCount`), sink `m_lastDelivered`, per-cache entry bytes.
- **Per-VT-session alive-wrap high-watermark** via a small per-session registry —
  `CVPixelBufferPool` exposes no inventory API, so the measurable proxy is the peak
  number of simultaneously-alive wrapped buffers per session (pools retain to
  max-outstanding). Attribution of class 3 beyond that is inferential: the
  footprint-minus-ledger delta, stated as such in the report.
- Periodic on-device log line + the existing `recordGpuBudget` stats hook extended with
  tags; `os_proc_available_memory()` sampled alongside so ledger totals correlate with
  real headroom.

**Decision point (recorded in the plan):** the run names the dominant class(es). Work
that targets a class the evidence exonerates is dropped or re-scoped. The window and
ladder tasks proceed regardless (requirement-driven), but their sizing uses measured
numbers — including the Level-1 threshold, which derives from the measured ramp rate.

## Design

### 1. Unified residency ledger

`GpuBudget` evolves into a single ledger for all decoded video memory — GPU surfaces
and CPU planes. Final name `FrameResidencyLedger` (`GpuBudgetCharge` →
`ResidencyCharge`); mechanical rename as the final commit. Strict **leaf-mutex**
discipline (credits may fire while worker locks are held — e.g. evicted-frame vectors
destroyed inside `m_bufferMutex` scope — so the ledger must never take a non-leaf
lock).

**Link structure:** the ledger becomes an **unconditional compile source in all
targets** (it is pure accounting: mutex + counters, no GPU or pipeline dependencies).
This keeps `ResidencyCharge` linkable from GPU-off builds — the abstract `GpuSurface`
stays a pure interface with its inline defaulted destructor (that inline-ness is a
deliberate GPU-off link-contract fix; it must not regress), and the charge lives in
the **concrete** surface classes (`AppleGpuSurface`, `D3D11GpuSurface`, stubs), each
crediting in its own destructor.

**Gate semantics per tag (the central invariant):**

- **Gated tags** — `DecodeWindow`, `Staging`, `ReadbackRing`, output-bus surfaces:
  `tryCharge` compares `gatedLiveBytes` against the budget; failure follows the
  existing OOM-degrade contract.
- **Charge-only tags** — `RecorderWrap`, `IngestWrap`, `CpuReadbackCache`, CPU frame
  planes, `RetireQueue`, `Other`: always succeed, visible in `liveBytes(tag)` and in
  the total, **excluded from the gate comparison**. Rationale: gating producer wraps
  against a playback budget would collapse the desktop pipeline in record+play (their
  bytes are not in the peak formula) and a failed ingest wrap would trip
  `latchGpuEncodeCpuFallback()`, latching CPU *encode* for the session — playback
  pressure may never degrade recording.
- The iOS pressure ladder consumes the **total** (`liveBytes()` across all tags) plus
  OS headroom; the gate consumes only the gated subset. Both are telemetry-visible.

### 2. Charge tied to the surface's lifetime

The charge is credited by the **concrete surface's destructor** (see §1), giving
last-reference semantics without a `shared_ptr` deleter (surfaces are wrapped before
the budget decision, so a fixed-at-construction deleter cannot carry the charge).

- **Attach ordering:** in `mintGpuOrDegrade`, the charge attaches **immediately after
  `tryCharge` succeeds and before the surface is published to any other thread** (the
  surface escapes to the global readback retainer during frame construction).
  Publication through `shared_ptr` release/acquire makes the attached charge visible;
  after publication the charge field is immutable.
- **Gate stays at mint:** `tryCharge` failure follows the existing OOM-degrade
  contract — the transiently *uncharged* surface remains legal just long enough for
  the CPU-fallback readback to produce real pixels, then dies. This is the one
  bounded, documented exception to "accounting == reality".
- **Producer paths** (`RecorderWrap`, `IngestWrap`) attach charge-only charges at
  their wrap sites (charged, never gated).
- CPU frames: the CPU frame-data object charges plane bytes on construction, credits
  on destruction (charge-only tag). `m_cpuCache` entries are charged under
  `CpuReadbackCache` with **two** eviction rules: (a) entries for frames outside any
  residency window drop on the trim tick, and (b) a size-bounded LRU *within* the
  allowance for in-window entries — a scrub across the full window with a
  CPU-consuming sink would otherwise re-balloon by the window's size (in-window GPU
  frames can regenerate a readback cheaply, so LRU-dropping them is safe).

### 3. Budget derived from the OS, with honest arithmetic

On iOS, at playback-session start and on every ladder event:

```
budget = min( 0.5 × (os_proc_available_memory() + ledger.gatedLiveBytes()),  available_now )
budget = clamp(budget, min(512 MB, available_now), 4 GB)
```

The `gatedLiveBytes` add-back prevents the double-count that would shrink the budget
merely because the ledger is healthily full; the outer `min` and the
`min(512 MB, available)` floor prevent overcommit when memory is genuinely scarce.
**Invalid samples** — 0, or greater than physical RAM (`os_proc_available_memory`
returns 0 when backgrounded) — leave the previous budget unchanged; the suspend path
already defers GPU work, and budget re-derivation on resume uses the first valid
foreground sample. `kIosAggregateGpuFrameCeiling` is deleted. The test override
`OLR_GPU_FORCE_BUDGET` (`gpuForcedPerTrackBudget`) is kept and wins over derivation.
On macOS/Windows the existing peak-formula configuration remains the enforcement
input; unified accounting and tags apply everywhere.

On the target iPad (per-process limit ≈ 3.3 GB observed), session-start availability
is ~3.0 GB → budget ≈ 1.5 GB.

### 4. Playhead-centered residency windows — asymmetric, parameterized scheduler

**Shape:** the window is `trailFrames` behind the playhead + the existing decode-ahead
lead (`kLeadMs` ≈ 15 frames at 30 fps) in front. The lead is **not** widened — forward
stepping is already served by decode-ahead; the backward trail is what this design
adds. Sizing:

```
trailFrames = clamp( (budget − nonWindowReserves) / (feedCount × frameBytes) − leadFrames,
                     8, 120 )
```

`nonWindowReserves` reuses `GpuBudgetConfig`'s per-term estimators (staging, output
bus, readback rings) plus the `CpuReadbackCache` allowance
(`2 × feedCount × frameBytes` steady-state display set; the LRU in §2 enforces it).
With the measured ~1.5 GB budget, 1080p × 4 feeds: roughly **50–60 trail frames per
feed** (~1.7–2 s at 30 fps). Every feed gets the same trail, so lockstep scrubbing is
structural.

**Parameterization (resolves the desktop-scope contradiction):** the retention spans
become *parameters* of the scheduler — a `ResidencyWindowParams` struct consumed by
the trim, the caps, and the cache horizons. **Desktop default = the current constants
(500/700 ms behavior, byte-for-byte);** iOS derives the parameters from the budget.
Tests inject parameters directly, so the trail-retention test runs in CI on any
platform without changing desktop defaults, and the timing-sensitive e2e gates
(`seekflash`, `farback`, `stepscrub`) see unchanged desktop behavior.

The parameterized mechanisms (this is a scheduler change and is owned as such):

- The per-iteration trim (`kTrailMs`/`kSlackMs` spans) and
  `OutputFrameCache::trimBefore` horizons take the derived trail span (in ms at the
  session frame rate). The **audio** horizon keeps the *current* 500 ms trail
  regardless of the video trail (audio memory stays out of scope).
- The backward-step fast path (`reuseAt` output-cache coverage) serves the retained
  trail; the output-cache trim horizon follows the same derived span.
- `TrackBuffer::insert` caps and the protect-range asymmetry (`protectLo/protectHi`)
  take derived values. **`kIosMaxPerTrackGpuFrames` (= 8) is replaced** by the derived
  per-track cap (`trailFrames + leadFrames + slack`) — this constant, not the
  aggregate ceiling, is what currently forbids any real window on iOS;
  `gpuPerTrackWindowCap`/`capFrames` consume the derived value, and
  `tst_iosgpupolicy` (which pins the old constants) is reworked accordingly.
- **Post-seek semantics (explicit):** after a reposition the trail is *cold* — it
  warms as material plays through. No backward prefetch; stepping backward past
  decoded material re-decodes via exact seek (existing behavior, same as the CPU
  path today).

Outside the window frames are dropped; whether dropping returns pages to the OS
depends on the VT pool (next section).

### 5. VideoToolbox pool management

Dropping our last reference returns buffers to the session's pool, which keeps pages
warm at its high-watermark. Therefore:

- Phase 0 records per-session alive-wrap high-watermarks; the window-depth formula is
  validated on device against VT actually sustaining `trailFrames + leadFrames`
  outstanding buffers per session (unverified today; the current cap is 8/track).
- **Pool flush API (named):** obtain the session pool via
  `VTSessionCopyProperty(kVTDecompressionPropertyKey_PixelBufferPool)` and call
  `CVPixelBufferPoolFlush(pool, kCVPixelBufferPoolFlushExcessBuffers)`.
- **Playback decoders** (worker-owned): Level 1 = pool flush-excess (no hitch);
  Level 2 = full decoder `reset()` (session invalidation; costs a keyframe re-sync —
  acceptable at the CPU-latch boundary and documented).
- **Ingest decoders** (capture-thread-owned): the ladder **never resets them**
  (playback pressure may not degrade recording, and cross-thread session invalidation
  is a known teardown-race class in this codebase). Level 1/2 set a polled atomic
  flag; each capture thread performs flush-excess on its own session at the next
  decode iteration.
- The on-device sign-off measures `os_proc_available_memory()` recovery after Level-1
  trim + flush — not just ledger deltas — because ledger bytes falling without
  footprint falling is exactly the failure mode of a pool-blind design.

### 6. Memory-pressure ladder

**Sampling (the primary trigger), covering bursts:** two triggers feed the same
evaluation —

- a debounced (~250 ms) `os_proc_available_memory()` poll in the worker loop **and in
  the recorder's tick** (ingest can balloon with no playback session active), and
- a **bytes-minted counter**: every ~64 MB of new charges since the last sample forces
  an immediate evaluation *inside* allocation bursts — the reposition fill loop mints
  hundreds of surfaces within one "iteration", where a per-iteration poll is blind.

UIKit memory warnings (via the `IosGpuLifecycleSink` seam extended with
`onMemoryWarning()`, marshaled like suspend/resume) are a supplementary trigger.
Warnings arriving while suspended are ignored.

Thresholds are **absolute headroom**; the Level-1 threshold's *default* is
`max(256 MB, 4 × feedCount × frameBytes)` and is re-derived from the Phase-0 measured
ramp rate × (sampling interval + trim/flush latency).

- **Level 0** — normal. Re-arm: after ≥30 s with headroom above 2× the Level-1
  threshold, the budget may re-derive upward (at most once per minute).
- **Level 1** — headroom below threshold, or a UIKit warning: re-derive budget (§3),
  shrink windows, trim distal frames, flush VT pools (§5), drop `CpuReadbackCache`
  overage. Executed on the worker thread between iterations (never mid-reposition;
  the trim reuses the existing per-iteration trim's locking); the ingest flush flag is
  set for capture threads. Telemetry counter + log.
- **Level 2** — headroom below half the Level-1 threshold after a Level-1 trim, or a
  second warning within 10 s: latch CPU-only mode for the session.

**Level-2 state machine (explicit):** latch = `GpuPipelineState::CpuFallback` plus a
dedicated `m_memoryPressureLatched` flag — **not** `GpuDeviceLossMonitor::recordLoss()`.
Rationale (corrected): a latched monitor would make `gpuDeviceLossPending()` strip GPU
frames on every subsequent healthy reposition, the per-packet loss check would
re-enter `handleGpuDeviceLoss`, and the monitor only clears via the rebuild path that
Level 2 must not run. The latch path runs `handleGpuDeviceLoss(reason)` parameterized
with `MemoryPressure`: a direct `GpuGenerationCounter::bump()` for sink staleness,
sanitize of caches/buffers, **no** `consumeGpuDeviceLossRebuildBudget()`, **no**
`rebuildGpuSpine()`. **Post-latch recovery is cached-planes-only — identical to device
loss:** any GPU frame without a cached CPU snapshot is dropped (the generation bump
makes it unreadable regardless of monitor state); the visible artifact is a gap on
never-displayed material, documented, not hidden. Unlatch: `initializeOutputGraph`
(session stop/start) clears `m_memoryPressureLatched`. Post-latch decode caps: the
**latch flag** (not `gpuPipelineEnabled()`, which reads the env var and stays true)
selects the CPU branch of `capFrames()` — fixing, in passing, the same wrong-branch
defect that exists today for device-loss `CpuFallback`.

### 7. Adjacent cleanup (in scope)

The GPU-force-on mechanism used to produce iOS GPU builds currently exists only as an
uncommitted local edit to `main.cpp` (in the phases 4–5 worktree). It lands here
properly: CMake option `OLR_GPU_PIPELINE_FORCE_ON` (default OFF) → compile definition →
the guarded `qputenv("OLR_GPU_PIPELINE", "1")` at startup, committed with this work.

## Testing

**Failing tests first — targeting the real mechanisms:**

1. **Charge-free retainer test:** a fake co-owner holds raw `shared_ptr<GpuSurface>`
   (modeled on the fence-parked readback retainer) past track-buffer eviction; assert
   live surface count (from a new test-only surface registry — required
   instrumentation, does not exist today) equals ledger count. Fails today.
2. **Uncharged producer test:** mint frames through the recorder/ingest wrap paths;
   assert the ledger sees them. Fails today (empty `GpuBudgetCharge`).
3. **Trail retention test:** inject `ResidencyWindowParams` with trail = N; play
   forward, step backward N−1 frames; assert zero re-decodes. Fails today (the fixed
   500 ms trim bites at ~15 frames @30 fps). Runs on all CI platforms via parameter
   injection; desktop *defaults* stay untouched.

Then: ledger units (charge-follows-surface-lifetime across threads and co-owners;
gated-vs-charge-only tag semantics — gate compares only gated tags; per-tag
accounting; CPU-plane and `CpuReadbackCache` charges incl. the in-window LRU); window
math units (trail derivation incl. lead subtraction, clamps, lockstep equality,
trim-span/cap derivation, `kIosMaxPerTrackGpuFrames` replacement); ladder units with
injectable memory/warning sources (bytes-minted trigger fires mid-burst; watchdog
fires without any UIKit warning; budget re-derivation with the `gatedLiveBytes`
add-back; invalid-sample handling; floor never exceeds availability; Level 2 latches
with `MemoryPressure` reason, leaves the device-loss rebuild budget untouched, selects
the CPU cap branch via the latch flag; re-arm path). Existing gates stay green:
`tst_gpubudget`, `tst_gpu_budget_stress`, the multi-feed pressure gate, the
device-loss suite, `tst_iosgpupolicy` (reworked), `gpu-budget`/`seek-prefetch` e2e,
and the desktop e2e timing gates (`seekflash`/`farback`/`stepscrub` — unchanged
defaults). New tests join the TSan CI lists.

**On-device gates (added to the iOS manual checklist as required sign-off):**

- Phase 0 attribution run: per-class/per-holder/per-pool numbers recorded; dominant
  class named; decision point exercised; ramp rate measured (feeds the Level-1
  threshold).
- Enforcement run: 4× SRT feeds, ≥10 min play + scrub; ledger plateau ≤ budget AND
  `os_proc_available_memory()` stable; 4-feed lockstep step-check across the full
  trail; Level-1 drill (induce pressure, verify trim + pool flush recovers real
  headroom); VT sustains window-depth outstanding buffers; a far-seek burst does not
  outrun the bytes-minted trigger.

## Out of scope

- Disk/cold tier, predictive/backward prefetch (existing seek-prefetch untouched;
  `GpuSeekPrefetch` continues to size itself from ledger `budgetBytes`/`liveBytes`,
  now against the derived budget).
- Output sink changes (NDI/DeckLink/AJA/OMT) beyond retention visibility.
- Audio memory (the audio trim horizon deliberately keeps its current 500 ms trail).
- Desktop scheduler behavior (parameter defaults preserve current constants).
- The iOS native-SRT DNS resolution failure — separate bug, tracked separately.
