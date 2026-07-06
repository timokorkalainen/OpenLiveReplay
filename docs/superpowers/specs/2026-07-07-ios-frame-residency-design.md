# iOS frame-residency design: unified decoded-frame ledger + playhead-centered windows

**Status:** approved design, pre-implementation
**Depends on:** the GPU-resident pipeline phases 4–5 branch (`gpu/resident-pipeline-phases-4-5`)
**Platforms:** enforcement changes are iOS-scoped; accounting unification applies everywhere

## Problem

With the GPU pipeline active on iOS (4× 1080p SRT feeds, record + play), the app's
physical footprint grows to ~3.3 GB within seconds of playback and iOS jetsam-kills it
(SIGKILL). Evidence from the on-device JetsamEvent report: OpenLiveReplay is
`largestProcess`, frontmost, `rpages = 202615` (16 KB pages ≈ 3.32 GB), lifetime max the
same — a monotonic climb, not a spike. The CPU-only path idles at a few MB and plays
within the existing 256-frame aggregate cap (~800 MB at 1080p) without incident.

The iOS GPU budget (`kIosAggregateGpuFrameCeiling = 48` frames ≈ 500 MB with staging and
readback terms) was exceeded ~6–7×. Root cause is structural, in four parts:

1. **Charge lifetime ≠ allocation lifetime.** The RAII `GpuBudgetCharge` lives inside
   `GpuFrameData`. The GPU surface itself is a `std::shared_ptr<GpuSurface>` co-owned by
   other components — the async readback ring's pending list, the frame retire queue,
   and the VideoToolbox keep-alive. When the `GpuFrameData` wrapper is evicted, the
   budget is credited and a new mint is allowed, while the real IOSurface stays resident
   under another owner. The ledger drains; the memory does not.
2. **Uncharged CPU copies.** `GpuFrameData::m_cpuCache` retains a full CPU plane copy of
   every surface that has been read back (one per requested format). On iOS unified
   memory this is a second ~3.1 MB per displayed 1080p frame, invisible to the budget.
3. **Constant-based budget.** The 48-frame ceiling is a compile-time guess. iOS exposes
   the actual remaining allowance (`os_proc_available_memory()`), which varies by device
   and moment.
4. **No memory-pressure response.** Nothing subscribes to UIKit memory warnings; the
   first signal the app receives is the kill itself.

A key sizing fact drives the design: **on iOS unified memory, a decoded CPU frame and a
GPU IOSurface frame cost the same physical bytes** (1080p NV12 ≈ 3.1 MB either way, both
counted against the jetsam limit). Demoting GPU→CPU saves nothing; only *fewer resident
frames* and *eviction that truly frees* reduce footprint.

## Requirements (agreed)

- **All four feeds scrub/frame-step in lockstep** (EVS-style multiview scrubbing), with
  "some frames back and forth from the playhead" instantly steppable.
- CPU-path functionality is fully preserved: scrub anywhere on the recorded timeline;
  outside the resident window frames re-decode on demand (existing FrameIndex exact
  seek).
- Device target: **M-series iPad Pro**; budget adapts at runtime rather than assuming a
  fixed allowance.
- Design point: **4× 1080p feeds**; all math derived from actual decoded frame geometry
  (which `GpuBudgetConfig` already receives), so other formats degrade proportionally.
- Jetsam must become structurally unreachable: accounting equals reality by
  construction, and pressure signals shed memory before the OS sheds the process.

## Design

### 1. Unified residency ledger

`GpuBudget` evolves into a single ledger for **all decoded video memory** — GPU surfaces
and CPU planes. Final name `FrameResidencyLedger` (`GpuBudgetCharge` →
`ResidencyCharge`); the rename is a mechanical final commit, the API keeps its
`tryCharge`/`charge`/`credit`/`liveBytes` shape and leaf-mutex design.

Additions:

- **Owner tags.** Every charge carries a tag: `DecodeWindow`, `Staging`, `ReadbackRing`,
  `CpuReadbackCache`, `RetireQueue`, `Other`. `liveBytes()` remains the total;
  `liveBytes(tag)` and per-tag peaks feed telemetry (extending the existing
  `recordGpuBudget` output-stats hook) and a periodic on-device log line.
- **Report-only mode** (`OLR_LEDGER_REPORT_ONLY=1`): accounting and telemetry active,
  mint gating disabled. The first on-device run uses this to capture per-owner numbers
  and name the dominant retainer — completing the root-cause evidence as part of the
  work.

### 2. Charge follows the allocation (the structural fix)

The GPU surface allocator mints every surface as a `shared_ptr<GpuSurface>` whose
**custom deleter owns the ledger charge**. The credit fires exactly when the last
co-owner — track buffer, staging cache, readback ring, retire queue, VT keep-alive —
drops its reference. Retained surfaces are charged surfaces: over-retention becomes
visible back-pressure on the mint gate (the existing OOM-degrade-to-CPU path) instead of
invisible growth.

CPU frames get the same treatment: the CPU frame-data object charges plane bytes on
construction and credits on destruction (`QByteArray` implicit sharing makes copies
share storage; the per-object charge over-counts shared copies slightly, which errs in
the safe direction). `GpuFrameData::m_cpuCache` entries are charged under
`CpuReadbackCache` — the double-count becomes visible and budgeted rather than removed,
because sinks legitimately share one readback per surface.

**Deleter discipline:** the last reference can drop on any thread (readback thread, VT
callback, dispatch tick). The deleter does nothing but credit the ledger (leaf lock) and
free the surface — no worker locks, no Qt calls.

### 3. Budget derived from the OS

On iOS, at playback-session start:

```
budget = clamp(os_proc_available_memory() × 0.5, 512 MB, 4 GB)
```

Re-sampled on every memory warning and on Level-1 pressure (below).
`kIosAggregateGpuFrameCeiling` is deleted. On macOS/Windows the existing peak-formula
configuration remains the enforcement input; the unified accounting and tags apply on
all platforms, so desktop behavior is unchanged.

### 4. Playhead-centered residency windows, lockstep by construction

Per-feed window depth is derived, not tuned:

```
windowDepth = clamp((budget − nonWindowReserves) / (feedCount × frameBytes × 2), minDepth, maxDepth)
```

where `nonWindowReserves` reuses `GpuBudgetConfig`'s existing per-term byte estimators
(staging windows, output bus surfaces, readback rings) plus a `CpuReadbackCache`
allowance. Every feed gets the same depth, so 4-feed lockstep scrubbing is structural.
At 1080p with a ~2 GB budget this yields roughly ±60–80 instantly-steppable frames per
feed. Clamps: `minDepth = 8` (keeps a usable window on constrained samples) and
`maxDepth = 120` (beyond ~±120 frames further hoarding buys nothing).

The mechanisms already exist and are reused, not replaced:

- `TrackBuffer::insert(capFrames, keepNearMs, protectToMs)` — farthest-from-playhead
  eviction with live-edge protection — receives the derived depth as its cap.
- `allowNativeGpuDecodeForCurrentPacket` / `gpuPerTrackWindowCap` gate GPU minting to
  the window.
- Outside the window frames are **dropped** (truly freed via §2), and scrubbing beyond
  it re-decodes on demand through the existing exact-seek path. No disk tier, no new
  prefetcher; the existing seek-prefetch machinery is untouched.

### 5. iOS memory-pressure ladder

Events arrive through the existing `IosGpuLifecycleSink` seam, extended with
`onMemoryWarning()` — the same main-thread→worker marshaling as suspend/resume.

- **Level 0** — normal; budget as derived.
- **Level 1** — on a UIKit memory warning, or an available-memory sample below ~20% of
  the session-start sample: re-derive the budget from a fresh sample, shrink windows,
  and immediately trim distal frames with the existing eviction machinery. Bytes
  measurably fall; a telemetry counter records the event.
- **Level 2** — a second warning shortly after Level 1, or availability below ~10% of
  the session-start sample after trimming: latch CPU-only mode for the session via the **parameterized** sanitize
  path — the same machinery as device loss with a distinct `MemoryPressure` reason that
  does **not** consume the device-loss rebuild budget. Unlatch on session restart. CPU
  mode under the existing 256-frame aggregate cap is known-survivable.

### 6. Adjacent cleanup (in scope)

The ad-hoc `-DOLR_GPU_PIPELINE_FORCE_ON` compile hack in `main.cpp` becomes a proper
CMake option (`OLR_GPU_PIPELINE_FORCE_ON`, default OFF) defining the same symbol — it is
how iOS GPU builds are produced and belongs in the build system, not in a flag string.

## Testing

The failing test comes first and reproduces the bug class without a device: mint frames
through a simulated pipeline in which a co-owner (fake readback ring) retains surfaces
past track-buffer eviction; assert (a) ledger total never exceeds the configured budget
and (b) live allocation count equals ledger count. Current code fails (b).

Then:

- **Ledger units:** charge-follows-last-reference across N co-owners and threads;
  per-tag accounting; CPU-plane charges; report-only mode gates nothing but counts
  everything.
- **Window math units:** depth derivation across feed counts / frame sizes / budgets;
  clamps; lockstep equality across feeds.
- **Pressure units:** injectable available-memory function and warning events. Level 1
  provably frees bytes; Level 2 latches with reason `MemoryPressure` and leaves the
  device-loss rebuild budget untouched.
- **Existing gates stay green:** `tst_gpubudget`, `tst_gpu_budget_stress`, the
  multi-feed budget pressure gate, the device-loss suite, and the `gpu-budget` /
  `seek-prefetch` e2e labels. New tests join the TSan CI lists.
- **On-device manual sign-off** (added to the iOS manual checklist as a required gate
  with a concrete script): 4× SRT feeds, ≥10 minutes of playback plus scrubbing; the
  app's own telemetry shows the ledger plateauing ≤ budget and available memory stable;
  4-feed lockstep frame-step check. First run in report-only mode to record per-owner
  peaks.

## Out of scope

- Disk/cold tier and predictive prefetch (existing seek-prefetch suffices).
- Output sink changes (NDI/DeckLink/AJA/OMT) beyond their retention becoming charged.
- Audio memory (small and already bounded).
- The iOS native-SRT DNS resolution failure (`Native SRT host lookup failed`) — a
  separate ingest bug, tracked separately.
- Desktop behavior changes beyond unified accounting and telemetry tags.
