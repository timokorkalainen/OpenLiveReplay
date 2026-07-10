# Phase 2 — Extensible Core & Real-Time Backbone

> Part of the [Broadcast-Perfection Plan](./README.md). The index holds the goal, architecture guardrails, non-goals, the quality gates referenced below, the governance rules and the audit lane. Read it first.

**Duration estimate.** ~4-5 months

## Why this phase, in this order

Hardware I/O, new control surfaces, and new codecs cannot slot in cleanly while PlaybackWorker (5089 LOC) and UIManager (3424 LOC) fuse six-plus responsibilities each and extension means editing #ifdefs and a god-object..

## Initiatives

### `arch-decoder-backend-spi` — Codec-keyed decoder/encoder factory registry (video & audio)
- **Size:** L
- **Dependencies:** `arch-decompose-playbackworker`
- **Definition:** Replace compile-time platform selection of NativeVideoDecoder/NativeAacDecoder with a codec-keyed factory registry so HEVC/AV1/HE-AAC and a Linux VAAPI decode path (today a stub that ingests nothing) slot in by registration, not by editing ...
- **Acceptance criteria:**
  - [ ] A new decoder backend registers + is selected by capability with 0 edits to the decode call-sites; unit test drives the registry with a fake backend.
  - [ ] tst_nativevideodecoder / tst_nativeaacdecoder pass through the SPI; the HEVC/AAC-LC envelope is unchanged on macOS/Windows.

### `arch-decompose-playbackworker` — Decompose PlaybackWorker into ResidencyScheduler / ArmedCutEngine / GPU-lifecycle / cache-publish classes, atomics moved verbatim
- **Size:** XL
- **Dependencies:** `arch-engine-library`, `arch-ffmpeg-firewall`
- **Definition:** Split the 5089-LOC / 116-method QThread god-object along the review's seams into cohesive, independently testable classes, moving the verified concurrency atomics VERBATIM into their single owning class so no memory-ordering behavior changes.
- **Acceptance criteria:**
  - [ ] No resulting TU >~900 LOC; PlaybackWorker.h public API and all signals unchanged (uimanager unaffected).
  - [ ] New per-class unit tests: ResidencyScheduler window math and ArmedCutEngine promotion tested without a live demuxer; existing tst_playbackworker + e2e_play scenarios (play1x/seekplay/reverse/armedcut/armedcut-seekrace/armedcut-rearm-seek) all pass.

### `arch-decompose-uimanager` — Split UIManager into headless-testable domain controllers
- **Size:** XL
- **Dependencies:** `arch-engine-library`
- **Definition:** Break the 3424-LOC UIManager into domain controllers so the operator state machines (playlist/EVS playout, control routing) become unit-testable without a GUI, and T2.2's recallable-playlist UI lands in a small focused class.
- **Acceptance criteria:**
  - [ ] No resulting TU >~800 LOC; QML (Main.qml) bindings unchanged (same Q_PROPERTY/Q_INVOKABLE surface, re-exported through UIManager).
  - [ ] New headless unit tests for PlaylistController (playout boundary arming/advance) and ControlSurfaceRouter (action dispatch) run in ctest -L unit without a window.

### `arch-dedup-decoderbank-avcc` — Deduplicate decoder-bank open, avcC usage, sink status bookkeeping
- **Size:** M
- **Dependencies:** `arch-decompose-playbackworker`, `arch-output-sink-abi-and-registry`
- **Definition:** Collapse the remaining copy-paste hot-spots so every codec/decode-envelope or health-reporting change is made once, removing a documented class of subtle divergence bugs.
- **Acceptance criteria:**
  - [ ] Exactly one decoder-bank open path (grep: single avcodec_open2 helper); armed-cut preroll and primary bank decode identical codec setup, proven by an e2e that exercises both.
  - [ ] No inline avcC/AVCDecoderConfigurationRecord byte-twiddling outside avcc.cpp (grep for configurationVersion/lengthSizeMinusOne returns only avcc.cpp).

### `arch-output-sink-abi-and-registry` — Registry-based output-sink SPI + version OutputBusFrame with colorimetry + SMPTE-12M timecode fields
- **Size:** L
- **Dependencies:** `arch-engine-library`
- **Definition:** Evolve the existing IOutputSink/makeIoTargetSink seam into a registration-based backend registry and extend OutputBusFrame with colorimetry + SMPTE-12M timecode, so real DeckLink/AJA/ST 2110 sinks and T2.1/T3.3 slot in behind a stable ABI touching ...
- **Acceptance criteria:**
  - [ ] A new stub sink is added by registering into OutputSinkRegistry + implementing IOutputSink, touching 0 lines of PlaybackWorker/OutputDispatcher (proven by diff).
  - [ ] OutputBusFrame carries colorimetry+timecode; tst_ndisink asserts the fields round-trip; existing tst_outputdispatcher/tst_outputbusengine pass.

### `perf-bounded-backpressure-audit` — Bounded backpressure + drop-policy + counter at every ingest->record->playback->output seam, incl. encode-falling-behind ladder
- **Size:** M
- **Dependencies:** none
- **Definition:** Guarantee no unbounded queue anywhere on the ingest->record->playback->output path, each with an explicit cap, drop/coalesce policy, and a telemetry counter.
- **Acceptance criteria:**
  - [ ] Under a deliberately stalled sink for 60 s, RSS growth is bounded (< a fixed cap) and each capped queue reports increasing drops rather than unbounded depth.
  - [ ] A hostile over-sped source (2-4x realtime AU injection) does not grow the AU buffer without bound; the drop counter moves.

### `perf-hotpath-scans-and-fences` — Make audioSpanOrSilence & telemetry stateAt O(log n); pool GPU fences
- **Size:** M
- **Dependencies:** none
- **Definition:** Make the confirmed hot paths O(log n)/O(1) and stop minting a GPU fence per composite.
- **Acceptance criteria:**
  - [ ] A unit benchmark shows audioSpanOrSilence and stateAt runtime is flat (within noise) as the cache/timeline grows from 100 to 100k entries, versus linear before.
  - [ ] A GPU test (or the fence pool unit test) confirms fence allocations per 1000 composites drops from ~1000 to <= pool size, with byte-identical composited output.

### `perf-output-cadence-decouple` — Sleep-until-due + TimingReference/PTP phase-lock on the OutputRuntime loop (replace the 1ms msleep poll)
- **Size:** L
- **Dependencies:** `perf-rt-scheduling`, `perf-latency-budget-harness`
- **Definition:** Move program pacing from the 16 ms GUI-thread QTimer to a dedicated fps-adaptive pacing clock phase-locked to the session TimingReference (and PTP when external).
- **Acceptance criteria:**
  - [ ] A headless run of the paced output over 1 hour shows program inter-frame interval jitter <= 0.2 ms RMS / <= 1 ms peak-to-peak, independent of GUI activity (test drives synthetic QML load and asserts no jitter change).
  - [ ] 120p and 59.94p paced runs show the correct mean interval (8.333 ms / 16.683 ms) within 0.1%.
  - [ ] meets QG-CAD-JIT

### `perf-regression-harness-ci` — Per-PR fail-closed perf-regression gate with trend + trace artifacts on a pinned runner; multi-source scaling target
- **Size:** M
- **Dependencies:** `perf-latency-budget-harness`, `perf-trace-tooling`, `perf-output-cadence-decouple`
- **Definition:** Lock the wins in: a CI job that runs the latency/jitter/throughput harness, compares against committed baselines, fails on regression, and publishes trace+flamegraph+trend artifacts.
- **Acceptance criteria:**
  - [ ] A synthetic regression PR (e.g. reintroducing the O(N) audio scan) turns the perf gate red with a specific stage/metric callout.
  - [ ] The perf job publishes trace + flamegraph + a JSON result on every run; baselines are updatable via a reviewed commit.

### `perf-rt-scheduling` — Real-time scheduling class, priority elevation, CPU affinity for the on-air chain (degrade cleanly where denied)
- **Size:** L
- **Dependencies:** `perf-latency-budget-harness`
- **Definition:** Give the playback/decode, readback, and output-dispatch threads a real-time scheduling class, explicit priorities, and core affinity so the on-air path survives a saturated machine with zero program drops.
- **Acceptance criteria:**
  - [ ] Under the CPU-saturation stress scenario, program-sink dropped-frame count stays 0 over a 60 s play (vs a measurable drop count without RT promotion).
  - [ ] Each promoted thread reports its achieved scheduling class/priority in a startup log line; on a platform/permission where RT is denied, it logs a clear downgrade and still runs.

### `perf-zero-copy-interop` — Zero-copy GPU-to-sink interop for GPU-capable sinks; one shared readback for CPU sinks
- **Size:** XL
- **Dependencies:** `perf-hotpath-scans-and-fences`, `perf-latency-budget-harness`
- **Definition:** Deliver the composited surface as a native texture handle to any sink that can consume one, performing GPU->CPU readback only for CPU-only sinks and sharing one readback across sinks otherwise.
- **Acceptance criteria:**
  - [ ] A trace of the GPU-capable program path shows zero composite->CPU readback spans (only the render+fence), versus one full readback per frame today.
  - [ ] A frame-identity e2e confirms the zero-copy path produces the same pixels as the readback path (golden compare).

### `rel-crash-safe-mkv` — Crash-recoverable recording: journal + startup orphan scan + auto-remux repair; dual-target A/B disk write; crash-loop safe-boot
- **Size:** L
- **Dependencies:** none
- **Definition:** Guarantee that a hard process kill or power loss loses at most the unflushed tail and yields a playable clip with no manual intervention.
- **Acceptance criteria:**
  - [ ] e2e tst_crash_recover: start a record, SIGKILL the harness mid-cluster, relaunch; the repaired MKV is playable end-to-end, frame count == frames flushed before the kill, and lost tail <= 1 cluster
  - [ ] Repair is idempotent (re-running on an already-repaired file is a no-op) and never corrupts a good file

### `rel-degradation-ladder` — Explicit, surfaced, monotonic-down degradation ladders per subsystem
- **Size:** S
- **Dependencies:** `rel-telemetry-bus`
- **Definition:** Turn today's ad-hoc silent downgrades into a coherent policy with named, operator-visible, monotonic-down tiers per subsystem so the system never runs degraded unnoticed.
- **Acceptance criteria:**
  - [ ] tst_degradationpolicy: transitions are monotonic-down within an epoch, each emits exactly one announcement, and tier-up requires an explicit recovery signal
  - [ ] The encode-fallback path (tst_streamworker_gpuencode) now emits a surfaced tier change rather than a silent latch

### `rel-session-restore` — Durable operator session/state persistence with crash restore-and-resume
- **Size:** M
- **Dependencies:** `rel-crash-safe-mkv`
- **Definition:** Persist full operator state continuously and, after any restart, restore the exact session and offer to resume recording alongside crash-safe MKV recovery.
- **Acceptance criteria:**
  - [ ] e2e: configure a multi-source multi-view session, SIGKILL, relaunch; the full operator state is restored byte-for-byte and the recovered clip is offered for resume
  - [ ] Session writes are debounced and never block the UI thread (measured)

### `rel-storage-guardian` — Storage supervisor: preflight, tiered thresholds, reserved-space safety file, retention reclaim
- **Size:** M
- **Dependencies:** none
- **Definition:** Replace the reactive disk-full latch with proactive capacity management so a 24/7 recorder never loses content to a full disk and always retains enough headroom to finalize cleanly.
- **Acceptance criteria:**
  - [ ] Unit test tst_storagesupervisor: given a shrinking free-space series and a bitrate, thresholds fire at the configured levels and time-to-full is within +/-5% of analytic
  - [ ] e2e: a small tmpfs is filled during a live record; the reserved safety file guarantees a valid, playable trailer is written and storageCritical fires before write errors begin (muxer never reaches kFatalWriteThreshold)
  - [ ] meets QG-REL-SOAK

### `rel-worker-watchdog` — Liveness watchdog & self-heal supervisor for all critical threads
- **Size:** M
- **Dependencies:** `rel-telemetry-bus`, `rel-output-autorecover`
- **Definition:** Detect any wedged critical thread within one supervisory window and either self-heal the subsystem or raise a hard operator alarm — eliminate silent freezes.
- **Acceptance criteria:**
  - [ ] tst_watchdog: a stubbed loop that stops beating is flagged within its deadline and the self-heal callback fires exactly once per wedge epoch
  - [ ] Fault-injection e2e (a capture thread artificially blocked) triggers a capture restart and the source recovers

### `rop-control-surface-hub` — Unified ControlSurfaceHub + IControlSurface seam (also arch-control-surface-spi)
- **Size:** M
- **Dependencies:** none
- **Definition:** Extract action dispatch, jog/shuttle, learn, and feedback out of UIManager into a testable ControlSurfaceHub with an IControlSurface interface that MIDI, Stream Deck, keyboard, and HID all implement — the foundation every new ...
- **Acceptance criteria:**
  - [ ] All existing MIDI/Stream Deck actions and jog/shuttle still work (regression via existing qmlstyle + a new unit test)
  - [ ] tst_controlsurfacehub covers action dispatch, hold-actions, jog delta, and shuttle-ladder stepping with no UIManager/Qt-GUI dependency

### `tc-reference-reselection` — Reference-source re-selection robustness
- **Size:** M
- **Dependencies:** none
- **Definition:** Re-run reference election and re-baseline the servo without a phase step when the highest-quality source drops mid-record.
- **Acceptance criteria:**
  - [ ] dropping the reference source mid-record re-elects with < 1-frame phase discontinuity (e2e)
  - [ ] no servo runaway on reference loss/return

---

[« Phase 1](./phase-1-foundation-safety.md) · [Index](./README.md) · [Implementation plan](./impl/phase-2-extensible-core.md) · [Phase 3 »](./phase-3-image-chain.md)
