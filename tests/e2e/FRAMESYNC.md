# Frame-Sync Acceptance Rig

This rig is the acceptance instrument for broadcast ingest timing work. Every
later timing phase is measured against these cells before its claims become
gates.

## What It Measures

- **Lip-sync:** nearest beep onset minus flash onset. The current gate is EBU
  R37-compatible at `-40..+60 ms` for `audio - video`; the target band is
  `+/-20 ms`.
- **Inter-camera phase:** flash-onset spread across two synchronized views, plus
  the Phase-4 servo's measured phase + confidence tier. With common timecode this
  is a **gate** (`e2e_framesync_intercam`): the servo drives both sources to a
  locked tier (FrameAccurate is the target; reported per run) and holds the recorded
  flash-spread **mean within two frames** of anti-flake margin (the honest floor is
  one frame — see "Inter-Camera Phase Servo, Accuracy & Gate" below). Without common
  TC it is a bounded report — clockless IP cannot prove frame-accurate phase, so the
  tier is `Bounded`/`Approximate` with a surfaced `±ms` number.
- **Drift:** least-squares slope of `flash index -> recorded PTS`, recovered
  source-clock ppm, and A/V offset drift over the run. The zero-skew CTest cell
  gates A/V offset regression drift at less than one frame; video flash slope is
  reported as a diagnostic because onset quantization can move by a frame on
  short local runs. The injected-skew cell is report-only, but it fails if the
  rig cannot observe a nonzero recovered clock ppm.
- **Timecode:** recorded MKV `tmcd`/`timecode` tag versus the injected
  `OLR_MARKER_TC`, asserted **frame-exact**. Phase 3 lands the full pipeline —
  SMPTE 12M extraction (SRT/RTMP H.264/HEVC SEI + RTMP AMF fallback), the native
  NDI 100 ns timecode, `TimecodeAligner` (`ReplayManager::sourcesFrameAligned` /
  `sourceFrameOffset`), and a muxer `timecode`/`tmcd` tag written from the session
  start TC — so this cell is now a **gate** (`OLR_FRAMESYNC_GATE=1`).

## Timecode Accuracy, Injection & Gate

The `e2e_framesync_timecode` cell **gates** on one thing: the recorded MKV's
`timecode`/`tmcd` tag is **frame-exact** versus the injected SMPTE 12M timecode.
Achieved accuracy in this environment: recorded `10:00:00:00` == injected
`10:00:00:00` (exact). Inter-camera alignment is **measured and reported, not
gated** (see below).

**How it injects/measures TC.** The cell runs over the **NDI** transport because
that is the only path that injects SMPTE 12M natively in this environment. The
`ndi_marker_sender` publishes the injected TC through the SDK
(`config.startTimecode`) in **static mode** (`--timecode-static`): every frame
carries the *same* injected TC, so the engine's *first muxed frame* — captured
whenever discovery/connect completes — records exactly the injected value,
independent of connect latency. The gate uses a **single** NDI source (one
discovery+connect, matching the stable lipsync/drift cells); a two-source NDI
recording occasionally has one source fail to connect and produce no MKV — an
NDI-runtime/discovery flake, not a tmcd defect. Measurement: `ffprobe
-show_entries format_tags=timecode / stream_tags=timecode` (`mkv_start_timecode`)
for the tag.

**Inter-camera alignment is report-only.** When two common-TC sources are
recorded, `sync_harness --report-tc-align` emits `tc_align a=.. b=.. aligned=..
offset=..` from `ReplayManager::sourcesFrameAligned`/`sourceFrameOffset`, and the
cell prints the measured offset — but does **not** fail on it. `TimecodeAligner`
anchors each source on the session frame where it first observes a TC, and two
real-NDI receivers see jam-synced frames with arrival jitter, so the measured
offset jitters ~0–2 frames run to run. That matches this program's honest target
("aligned within measured bounds"); **exact, stable inter-camera frame-lock is
the Phase-4 inter-camera servo's job, not Phase 3's**. Two-source flash-onset
spread is separately exercised by `e2e_framesync_intercam`.

The SRT-SEI TC path would need ffmpeg `drawtext` to burn the timecode into the
H.264 SEI the engine extracts from; the container-level `-timecode` does **not**
reach that SEI. Local ffmpeg usually lacks `drawtext`, so the SRT TC-injection
path is not provable locally.

**Gate-vs-skip.** The cell **gates** when a TC-capable source is available
(NDI runtime + `ndi_marker_sender`, or ffmpeg `drawtext` for SRT). It **SKIPs
cleanly (exit 77)** when neither is present — no NDI runtime/marker AND no
`drawtext` — so an environment gap never fails the gate. The header-write grace
window (`OLR_MUXER_TMCD_GRACE_MS`, default 750 ms) lets the first source TC win
the tag before the deferred MKV header commits.

**Known limitations (carried from the Phase-3 reviews).**

- **Raw-packed-word extraction, not full SEI parsing.** The H.264/HEVC extractor
  reads the SMPTE-12M-packed word; it is not a full `pic_timing` `clock_timestamp`
  / registered-ATC parser. Real-world SEI variants may not be recovered.
- **Absolute TC is exact only at the nominal 30 fps.** The engine recovers TC via
  `Smpte12m::from100ns(_, kTimecodeNominalFps=30)`, and the absolute
  `TimecodeAligner::toSessionFrameIndex` / `tmcd` are frame-exact only at that
  nominal. Inject non-drop 30-style TCs (the default `10:00:00:00`); non-30 rates
  are not asserted (they don't round-trip through the current pipeline).
- **Start TC = the first muxed frame's TC.** A live recording observes no TC
  before connect; the tag is the first frame that carried one (static-mode
  injection makes this frame-exact for the gate).
- **Drop-frame TC is recovered as non-drop.** A drop-frame source TC is decoded
  with non-drop arithmetic at 30 fps; drop-frame preservation is a follow-up.

## Inter-Camera Phase Servo, Accuracy & Gate (Phase 4)

The `e2e_framesync_intercam` cell is a **gate** (`OLR_FRAMESYNC_GATE=1`,
`OLR_FRAMESYNC_TRANSPORT=ndi`). It records **two common-timecode sources** with the
**Phase-4 inter-camera phase servo on** and asserts the servo locks them.

**The servo (what it does).** `ReplayManager` picks a **reference source** (highest
`ClockQuality`, ties → lowest index) and runs a pure `SourceOffsetEstimator` that, per
source, measures the phase to the reference and grades a **confidence tier**:

- `FrameAccurate` — common timecode (the `TimecodeAligner` reports the equal-TC frames
  coincident) or an external reference (PTP, Phase 5). Bound `±0 ms`.
- `Bounded` — a recovered-clock estimate (PCR/NDI/FLV-PLL) with a numeric `±ms` bound
  derived from the clock (RTMP/FLV gets a wider base, ms-resolution noise).
- `Approximate` — arrival-only; no reliable signal to lock to (`±40 ms` order).

It then nudges each non-reference, clock-locked source toward zero phase by summing a
**servo trim** into the worker's existing trim seam (`StreamWorker::m_servoTrimOffsetMs`,
separate from and **layered under** the PHASE-6 operator override — operator trim always
wins). The correction is **capped** (`kMaxInterCamCorrectionMs`) and **ramped**
(`kServoStepMs = 4 ms`/pulse), so a re-anchor step can never jerk the timeline by the full
correction in one tick. For **common-TC** sources the servo drives toward the EXACT TC frame
offset (FrameAccurate); for other locked sources it drives by the recovered clock offset
(Bounded). `Approximate` sources and a lone source get **servo 0** — byte-identical to
pre-Phase-4 behavior. The measured phase + tier ride the additive `IngestStats` fields
(`confidenceTier`/`interCamPhaseMs`/`interCamBoundMs`) into the `sourceStatsTooltip`.

**Achieved accuracy in this environment.** Two jam-synced NDI sources (identical
**advancing** SMPTE 12M TC on every content frame) usually grade **FrameAccurate** (the
`TimecodeAligner` reports them aligned) with measured residual phase `0 ms`, and the recorded
flash-onset spread **mean** holds at/under **one frame @30 (33.33 ms)** with the servo on
(validated 8–10× — most runs `0.0 ms`; the worst observed mean across runs was ~30 ms, i.e.
~0.9 frame). The **max** spread is hard-quantized to whole frames and routinely touches
exactly one frame (33.33 ms).

**What the gate asserts (and why it is reliable, not flaky).** Validated 8–10× in this
environment: every run **PASSes or SKIPs cleanly, never FAILs**.

1. **Both sources reach a LOCKED tier** — `frame-accurate` OR `bounded` (clock-recovered and
   servo-driven) — and the gate **FAILs only on `approximate`** (no lock at all). The exact
   `frame-accurate` grade is the *target* and is **reported** (`frame_accurate=yes/no`,
   `aligned=`) but is **not hard-gated**, because the `TimecodeAligner` anchors each source on
   the session frame where it first observes a TC and two real-NDI receivers see jam-synced
   frames with **arrival jitter**, so the tolerance-0 alignment (and hence the FrameAccurate
   grade) **jitters ~0–2 frames run-to-run**: the same fixture can grade FrameAccurate one run
   and Bounded the next. Hard-gating the exact FrameAccurate/`aligned=1` grade flaked (~1 run
   in ~14); gating "a locked tier" is the stable signal. *(This is the same Phase-3 lesson that
   made the timecode cell's exact inter-cam alignment report-only.)*
2. **The recorded flash-onset spread `mean ≤ 2 frames`** — the reliable, physical servo
   guarantee. The servo aligns the per-source *timeline mapping*, but the recorded flash
   *onset* is **quantized to the 30 fps frame grid**, so two views whose onsets straddle a
   frame boundary can differ by a frame. The observed mean stayed at/under ~1 frame across runs
   but touched ~30 ms (0.9 frame), so the gate uses **2 frames** for comfortable anti-flake
   margin while still catching a gross servo failure. The **max** is **reported for
   visibility** but **not gated** (it routinely sits right at the one-frame boundary). **One
   frame is the rig's honest floor**; sub-frame, genlock-grade lock is the **Phase-5
   PTP/reference-clock ceiling**, not something arrival-anchored common-TC alignment can pin to
   zero.

Use an **advancing** common TC, not a static one: a single repeated TC makes "the frame
where TC X first appears" connect-time-dependent, so two sources that connect at different
session frames anchor differently and never grade FrameAccurate. Advancing TC carries a
distinct value per content frame so both sources anchor on the SAME content TC regardless of
connect skew.

**Gate-vs-skip.** Common-TC injection only works over **NDI** here (the marker sender
publishes the same advancing TC on both sources via the SDK; the SRT/RTMP SEI path needs
ffmpeg `drawtext` locally). Under the gate, a non-NDI transport **SKIPs cleanly (exit 77)**.
Because the cell inherently needs **two** sources, it also **SKIPs cleanly** when the
two-source NDI recording cannot be produced — no MKV, too few paired flashes, or a source's
phase report is missing (a source never connected). That is an NDI-runtime discovery/connect
flake, not a servo defect, so it never **FAILs** the gate. Like the timecode cell it bumps
`OLR_MUXER_TMCD_GRACE_MS` (default 8000 ms under the cell) so a slow-but-eventual two-source
connect still wins the deferred MKV header. In report-only mode the cell prints the measured
spread + tier/phase and never gates.

## Timing Reference Tiers & PTP (Phase 5)

The session timebase is read through a single `TimingReference` seam
(`recorder_engine/timing/timingreference.h`), which advertises a **reference tier** —
ascending trust — and whether it is **external** (a real reference is locked):

- **`LocalMonotonic` (tier 0, default).** `LocalMonotonicReference` wraps the existing
  `RecordingClock` (`nowSessionNs() == elapsedMs()*1e6`). Not external. **Byte-identical to
  every prior phase** — this is what all the gates above run on. The session "now" is the
  recorder's own monotonic clock.
- **`RecoveredConsensus` (tier 1, reserved).** A consensus of the recovered sender clocks —
  the seam reserves it; not built.
- **`Ptp` (tier 2, opt-in).** `PtpReference` is an ST 2059 / IEEE 1588 **software** PTP
  slave: a pure `PtpServo` disciplines an offset + mean-path-delay against a grandmaster
  behind an `IPtpClient` backend (faked in unit tests; the real `UdpPtpClient` runs over UDP
  319/320). Enabled with `OLR_TIMING_PTP=1` (`OLR_TIMING_PTP_IFACE` picks the domain/iface);
  it falls back to local if it fails to start, and `nowSessionNs()` falls back to local
  monotonic **before lock** so the pipeline never stalls waiting for PTP.

**How PTP promotes sources to `FrameAccurate`.** Once the `PtpReference` locks,
`isExternal()` flips true and the session timebase is **facility time**. `ReplayManager`
feeds that external-reference state into the Phase-4 `SourceOffsetEstimator` via
`SourcePhaseEvidence::externalReference`, so any source whose recovered clock phase-locks to
the PTP-disciplined session estimate grades **`FrameAccurate`** — *without* needing common
timecode (TC is one route to FrameAccurate; an external reference is the other). With the
default `LocalMonotonicReference`, `externalReference` stays false and grading is exactly the
Phase-4 behavior — no change. The active tier + lock state are surfaced to the operator
(`ReplayManager::referenceTier()`/`referenceIsExternal()` → `UIManager::sessionReferenceTier()`
/`sessionReferenceStatus()`, e.g. `timing    PTP (external)` vs `timing    local monotonic`).

**The honest PTP ceiling.** This client uses **software timestamps**, so its accuracy is
*tens of microseconds* on a quiet LAN — much better than clockless IP, but **not** NIC-PHC /
genlock-grade. The top tier — true sub-frame, genlock-grade lock — needs **hardware
timestamping or a genlocked SDI/ST 2110 capture path**, a separate program that plugs a
facility-time `TimingReference` / hardware-`IPtpClient` backend into the *same* seam. The
seam is built so that is a backend swap, not a rewrite; the achieved tier is always surfaced
honestly via `tier()`/`isExternal()` rather than overclaimed.

PTP itself is exercised at the unit level (`tst_ptpservo`, `tst_ptpreference`,
`tst_timingreference`, `tst_udpptpclient`) with a fake `IPtpClient`; a live grandmaster on
the LAN is a manual/integration check (`OLR_TIMING_PTP=1` → `PtpReference::locked()` flips
true, the UI shows `PTP (external)`, and PTP-disciplined sources report `FrameAccurate`).

**Frame-sync program status: COMPLETE.** Phases 0–5 (heartbeat decoupling → timing core →
source-clock recovery → timecode → inter-camera phase servo → reference-clock / PTP seam)
are all delivered. The remaining work — genlocked SDI/ST 2110 output and hardware
timestamping — is the *separate* true-broadcast-output / hardware-capture program.

## Running It

Build `sync_harness`, then run the CTest label:

```bash
cmake --build build/bcast --target sync_harness
( cd build/bcast && ctest -L framesync --output-on-failure )
```

Run the full transport matrix directly:

```bash
tests/e2e/run_framesync_matrix.sh build/bcast/tests/e2e/sync_harness
```

The matrix covers `{lipsync, intercam, drift, drift_skew, timecode} x
`{srt, rtmp, ndi}`. SRT is active by default. NDI is active when the optional
local `ndi_marker_sender` target is built from an installed NDI SDK/runtime.
RTMP cells currently skip with exit code 77 until their marker-source fixture is
wired.

For NDI cells, `run_framesync_e2e.sh` starts a local `ndi_marker_sender`
process that publishes one or two deterministic `OLR-FS-*` sources. The sender
emits the same full-frame flash, 1 kHz beep, fixed timecode, and optional skew
pattern as the SRT fixture, but directly through the NDI SDK.

## Timing Core Status

Phase 1 is live for native SRT/RTMP:

- `SourceClock` maps sender timestamps to the recording timeline. SRT uses PCR
  quality with exact 90 kHz units; RTMP uses FLV millisecond quality.
- `DriftEstimator` exposes recovered clock ppm, and `sync_harness
  --report-stats` prints `clockppm` and `clockq`.
- `SourceClock::toSessionMs()` applies the recovered sender/session slope when
  mapping media timestamps, so video frames and audio chunks share one corrected
  timeline. The record-side FIFO consumes that common timeline without an extra
  audio-only ppm correction.

`StreamWorker` owns the per-backend source clocks and passes them into recreated
native sessions, so same-URL reconnects can retain recovered clock state. A
source URL/backend change resets the owned clocks.

## Environment Knobs

- `OLR_FRAMESYNC_TRANSPORT=srt|rtmp|ndi` selects a transport for
  `run_framesync_e2e.sh`.
- `OLR_FRAMESYNC_SECS=20` sets the recording duration.
- `OLR_FRAMESYNC_GATE=1` makes a scenario enforce its band.
- `OLR_FRAMESYNC_SKEW_PPM=200` injects deterministic media PTS/PCR skew in the
  `drift_skew` scenario.
- `OLR_MARKER_TC=10:00:00:00` sets the injected start timecode (non-drop 30-style;
  this round-trips frame-exact through the engine's 30 fps nominal recovery).
- `OLR_MUXER_TMCD_GRACE_MS=750` bounds how long the muxer holds the deferred MKV
  header for the first source TC before committing (0 disables the wait).
- `OLR_FLASH_CODEC=avc|hevc` selects the marker video codec.
- `OLR_NDI_MARKER_SENDER=/path/to/ndi_marker_sender` overrides the auto-detected
  sibling of `sync_harness`.
- `OLR_NDI_DISCOVERY_SECS=2` controls how long the driver waits for the local
  marker source to appear in NDI discovery before recording.

Local FFmpeg builds may omit `drawtext`. In that case the TC marker still sets
container timecode and emits a warning, but it does not burn visible timecode
text into the video.

## Skew Injector

The SRT drift skew cell uses `OLR_MARKER_SKEW_PPM` internally to
stretch/compress the FFmpeg marker media timestamps before the UDP->SRT bridge.
The NDI drift skew cell asks `ndi_marker_sender` to pace its SDK submissions at
the requested ppm offset. A single machine otherwise shares one wall clock, so
real drift is nearly zero by construction.

`tests/e2e/lossy_udp_relay.py` still has a ppm packet-release skew self-test for
network impairment experiments, but frame-sync acceptance does not treat packet
delay as media-clock skew.

Self-test:

```bash
python3 tests/e2e/lossy_udp_relay.py --selftest
```

## Caveats

Clockless SRT/RTMP can maintain A/V sync and bound inter-camera phase, but true
frame-accurate inter-camera lock requires common timecode or an external
reference. The rig is intentionally honest about that: it reports the achieved
accuracy first, then gates only the guarantees the transport can actually make.
The Phase-4 servo delivers frame-accurate inter-camera lock **only** with common
TC (graded `FrameAccurate`, recorded spread mean within one frame); other locked
sources get a `Bounded`-and-measured `±ms` correction, and `Approximate` sources
get no servo and are surfaced honestly. Phase 5 adds the **external-reference**
route to `FrameAccurate`: an opt-in software PTP (ST 2059) slave
(`OLR_TIMING_PTP=1`) becomes an external `TimingReference` and promotes
phase-locked sources to `FrameAccurate` without common TC — see "Timing Reference
Tiers & PTP" above. Its ceiling is **software-PTP precision (~tens of µs)**, not
NIC-PHC. Sub-frame, genlock-grade phase lock needs hardware timestamping or a
genlocked SDI/ST 2110 capture path — the separate true-broadcast-output program,
which the Phase-5 seam is built to accept as a backend swap.
