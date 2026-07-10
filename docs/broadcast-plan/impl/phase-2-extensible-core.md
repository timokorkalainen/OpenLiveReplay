# Phase 2 — Implementation plan

<!-- draft -->

> **Draft by construction.** This plan is re-audited, re-sliced and re-estimated against the live codebase at the Phase 1 exit re-plan (see the [operating loop](../operating-loop.md)). Until then it is a stub: one heading per initiative so coverage holds, with slices deferred.

### `arch-decoder-backend-spi` — Codec-keyed decoder/encoder factory registry (video & audio)
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `arch-decompose-playbackworker` — Decompose PlaybackWorker into ResidencyScheduler / ArmedCutEngine / GPU-lifecycle / cache-publish classes, atomics moved verbatim
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `arch-decompose-uimanager` — Split UIManager into headless-testable domain controllers
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `arch-dedup-decoderbank-avcc` — Deduplicate decoder-bank open, avcC usage, sink status bookkeeping
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `arch-output-sink-abi-and-registry` — Registry-based output-sink SPI + version OutputBusFrame with colorimetry + SMPTE-12M timecode fields
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `perf-bounded-backpressure-audit` — Bounded backpressure + drop-policy + counter at every ingest->record->playback->output seam, incl. encode-falling-behind ladder
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `perf-hotpath-scans-and-fences` — Make audioSpanOrSilence & telemetry stateAt O(log n); pool GPU fences
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `perf-output-cadence-decouple` — Sleep-until-due + TimingReference/PTP phase-lock on the OutputRuntime loop (replace the 1ms msleep poll)
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `perf-regression-harness-ci` — Per-PR fail-closed perf-regression gate with trend + trace artifacts on a pinned runner; multi-source scaling target
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `perf-rt-scheduling` — Real-time scheduling class, priority elevation, CPU affinity for the on-air chain (degrade cleanly where denied)
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `perf-zero-copy-interop` — Zero-copy GPU-to-sink interop for GPU-capable sinks; one shared readback for CPU sinks
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `rel-crash-safe-mkv` — Crash-recoverable recording: journal + startup orphan scan + auto-remux repair; dual-target A/B disk write; crash-loop safe-boot
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `rel-degradation-ladder` — Explicit, surfaced, monotonic-down degradation ladders per subsystem
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `rel-session-restore` — Durable operator session/state persistence with crash restore-and-resume
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `rel-storage-guardian` — Storage supervisor: preflight, tiered thresholds, reserved-space safety file, retention reclaim
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `rel-worker-watchdog` — Liveness watchdog & self-heal supervisor for all critical threads
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `rop-control-surface-hub` — Unified ControlSurfaceHub + IControlSurface seam (also arch-control-surface-spi)
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

### `tc-reference-reselection` — Reference-source re-selection robustness
_Draft — task stack and PR slices to be authored at the Phase 1 exit re-plan._

---

[Phase 2 initiatives](../phase-2-extensible-core.md) · [Index](../README.md)
