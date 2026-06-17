# Broadcast-grade frame-perfect ingest & A/V sync — Program Design

**Status:** approved framing (brainstorm 2026-06-17). This is a **program** (multi-phase); each phase
below becomes its own spec → plan → implementation cycle. Base: `main` @ `17c919a` (post #49–#54).
**Scope:** the **input / ingest / record** side. The output side (Qt preview, NDI/SDI/ST 2110 out) is
the separate output-bus track started by #54.

## Goal

Make OpenLiveReplay's record side deliver **frame-perfect A/V sync to the limit physics allows on the
available transport**, and **measure + surface the achieved accuracy**, with the timing architecture
built so a real reference clock (PTP) or genlocked hardware upgrades it to *true* genlock with no
pipeline rework. Four hard requirements, all in scope: **lip-sync (EBU R37), inter-camera phase-lock,
long-run drift-free, frame-accurate timecode alignment.**

## Honest target (the approved framing)

The deployment is **clockless remote/cloud contribution now** (SRT/RTMP, no shared reference),
**architected for reference-bearing hardware later** (SDI / ST 2110+PTP / NDI). What each guarantee
costs over clockless IP:

| Guarantee | Achievable now | Mechanism |
|---|---|---|
| Lip-sync (A/V) | ✅ maintainable | A+V ride one recovered sender clock per source (AUD-4 + a drift servo) |
| Long-run drift-free | ✅ | per-source clock recovery + a `DriftEstimator` + adaptive audio resample |
| Frame-accurate timecode alignment | ✅ | extract SMPTE 12M, align the timeline, write `tmcd` |
| Inter-camera phase-lock | ⚠️ **conditional** | **frame-accurate iff sources carry common timecode** (jam-synced); else aligned **within measured bounds** — that is physics, not a code gap |

So "100% ready" for the record side = **extract every bit of achievable sync and surface the achieved
accuracy** (frame-accurate inter-cam when common TC/reference exists, bounded-and-measured otherwise),
with a clock abstraction ready to take a PTP/genlock reference as the top tier. **Timecode is the key
enabler the app controls** — it is the poor-man's genlock for inter-camera frame-accuracy over IP.

## Transport matrix (ingest) and timing properties

NDI is a **first-class ingest transport** in this program (mandatory), alongside the existing native
SRT and RTMP. NDI is also the **best timing source** and the natural bridge to the hardware/reference
path, because it carries its own clock and timecode.

| Transport | Recovered sender clock | Native timecode | Notes |
|---|---|---|---|
| **Native SRT** (MPEG-TS) | **PCR 90 kHz** (already extracted; AUD-4) — best PLL input we have over IP | SMPTE in H.264/HEVC SEI or TS | the current strongest IP source |
| **Native RTMP** (FLV) | FLV DTS (ms) → PLL (noisier, ms-resolution, encoder-dependent) | AMF `onMetaData` / SEI | weakest clock; works, lower precision |
| **NDI** (new) | **NDI timestamp (100 ns)** — clean, monotonic, often hardware/PTP-disciplined | **native NDI timecode** (SMPTE, 100 ns) — delivered directly by the SDK | LAN/pro transport; the cleanest clock+TC; PTP/genlock-capable in pro setups → the door to true genlock |
| *(later)* ST 2110 / SDI | RTP+PTP / genlock = facility time | embedded | true genlock when present; handled by the Phase-5 reference seam |

## Architecture — a unified "best-available-reference" timing core

One coherent timeline abstraction from which lip-sync, inter-cam alignment, drift correction, and
timecode all derive. Five units, each independently testable.

### 1. `SourceClock` (per source) — recover the sender clock, reject network jitter
```cpp
enum class ClockQuality { Arrival, FlvPll, Ndi, Pcr, Reference }; // ascending trust
class SourceClock {
public:
    virtual ~SourceClock() = default;
    virtual void  observe(int64_t senderUnits, int64_t arrivalMonoNs) = 0; // raw clock obs
    virtual int64_t toSessionNs(int64_t mediaSenderUnits) const = 0;       // media -> session ns
    virtual ClockQuality quality() const = 0;
    virtual double estimatedPpm() const = 0;  // sender-vs-session rate skew
    virtual bool   locked() const = 0;
};
```
Implementations: `PcrSourceClock` (SRT, 90 kHz PCR PLL — builds on `MpegTsParser` + the AUD-4 anchor),
`FlvSourceClock` (RTMP, DTS PLL), `NdiSourceClock` (NDI 100 ns timestamp — near-direct, highest IP
quality), `ArrivalSourceClock` (fallback = today's arrival-anchored behavior). Each disciplines a smooth
local estimate of the sender timeline (linear-regression/PLL over recent `(senderUnits, arrivalMono)`
samples) so a single late/early packet can't perturb timing.

### 2. `DriftEstimator` (per source) — keep the recovered clock rate-matched, kill long-run slip
Windowed least-squares over `(recoveredSenderNs, sessionMonoNs)` pairs → slope (rate ratio) + offset.
`slope − 1` is the ppm drift. Drives **adaptive audio resampling** (`swr_set_compensation` on the per-
source resampler) so audio samples are added/dropped to match the session rate — no A/V slip and no FIFO
over/under-run over a multi-hour show — and refines the per-source media→session mapping for video.

### 3. `SessionTimeline` + per-source offset — one common timebase
The session timebase is the existing `RecordingClock` monotonic (the only common clock without an
external reference). At lock, record `offsetNs = sessionMonoAtLock − recoveredSenderAtLock`; thereafter
`sessionTime(media) = recoveredSender(media) + offsetNs`, with `DriftEstimator` holding the rate. The
**existing per-camera manual trim (PHASE-6)** remains the operator override layered on top of this.

### 4. `TimecodeAligner` (per source) — frame-accurate inter-cam + TC output
Extract SMPTE 12M: H.264/HEVC SEI (pic_timing / registered ATC), MPEG-TS, RTMP AMF, and **NDI native
timecode** (the easy, exact case). Map TC → frame index on the session timeline. When two sources share
TC, align so equal-TC frames coincide → **frame-accurate inter-camera**. Write a `tmcd` track / tags to
the MKV via the muxer.

### 5. `TimingReference` — the PTP/genlock-ready seam (designed-in now)
```cpp
enum class ReferenceTier { LocalMonotonic, RecoveredConsensus, Ptp };
class TimingReference {
public:
    virtual int64_t nowSessionNs() const = 0;
    virtual ReferenceTier tier() const = 0;
    virtual bool isExternal() const = 0; // true once a real reference (PTP) is locked
};
```
Today: `LocalMonotonicReference` (wraps `RecordingClock`). Later: `PtpReference` (ST 2059 slave) — the
session timebase becomes facility time, PTP/genlocked sources (ST 2110, genlocked NDI) lock *truly*, and
remote IP sources phase-lock to the PTP-disciplined session estimate. The whole pipeline reads
`TimingReference::nowSessionNs()`, so adding PTP is a swap, not a restructure.

### Inter-camera phase + confidence
Per source, `SourceOffsetEstimator` computes the offset to the session reference and a **confidence
tier**: `FrameAccurate` (common TC or an external reference), `Bounded` (recovered-clock estimate, with
a numeric ±ms bound), `Approximate` (arrival-only). The engine **corrects** each source's mapping toward
the reference and **surfaces the measured inter-cam phase + tier** in the UI (extending the existing
connection-status/health surface). "100% ready" means: always do the best available, and tell the
operator exactly how good it is.

## Engine integration (where each unit plugs in)
- **Ingest sessions** (`recorder_engine/ingest/native*ingestsession`): own their `SourceClock`
  (SRT already has the PCR/anchor; generalize it). NDI adds a new session (below).
- **StreamWorker**: applies the `DriftEstimator` to the per-source audio resampler + the heartbeat-
  derived frame mapping (the heartbeat already derives frames from session time via `heartbeatFrameSpan`
  — feed it reference time instead of raw monotonic).
- **ReplayManager**: owns `SessionTimeline`, `TimingReference`, the per-source offsets + the inter-cam
  alignment; relays measured phase/tier to `UIManager`.
- **Muxer** (`recorder_engine/muxer.cpp`): writes the `tmcd` track + TC metadata (Phase 3).
- **Reuse**: AUD-4 PCR recovery + shared anchor (Phase 1 seed); PHASE-6 trim (operator override);
  the `IngestStats`/health pipe from #51 (carry the timing tier/measured phase); the NDI SDK linkage +
  the `INdiSenderBackend`/`NdiOutputSink` pattern from #54 (mirror for ingest).

## NDI ingest (mandatory) — `NativeNdiIngestSession`
Mirror #54's output abstraction: `INdiReceiverBackend` (interface; real backend wraps the NDI SDK
`NDIlib_recv_*`, fake backend for tests) + `NativeNdiIngestSession : IngestSession`. It discovers/opens
an NDI source (`ndi://<source-name>` or the engine's source-config), receives `NDIlib_video_frame_v2_t`
/ `NDIlib_audio_frame_v2_t`, converts to the engine's `DecodedVideoFrame`/`DecodedAudioChunk` (NDI's
codec is decoded by the SDK — no `NativeVideoDecoder`/`NativeAacDecoder` needed), and feeds the
`NdiSourceClock` (frame `timestamp`, 100 ns) + the `TimecodeAligner` (frame `timecode`). Reuses the NDI
runtime-availability gate from #54; the selector adds `ndi`→`NativeNdi`. This makes NDI both a new
broadcast-quality input and the reference implementation that validates the `SourceClock`/
`TimecodeAligner` abstractions with a clean clock+TC.

## Phased decomposition (each is its own spec/plan)

- **Phase 0 — Measurement & acceptance rig (prerequisite).** You cannot certify "frame-perfect" without
  measuring it. Build:
  - `marker_gen`: N byte-identical synchronized marker streams = full-frame flash (video) + co-timed
    1 kHz beep (audio) + embedded SMPTE timecode, all from one generator clock (extends the existing
    flash/beep producers + an ffmpeg `timecode`/`drawtext` TC burn).
  - `skew_injector`: a relay that resamples one source's media clock at a configurable ppm offset to
    simulate cross-device crystal drift on **one** machine (extends `lossy_udp_relay.py`) — required to
    test the drift servo without two physical machines.
  - Drive N × {SRT, RTMP, NDI} encoders → record → measure from the MKVs:
    **lip-sync** (flash-onset vs beep-onset PTS; band EBU R37 −60..+40 ms, target ±20 ms),
    **inter-cam phase** (flash-onset PTS spread; band ≤1 frame with common TC, else report spread+bound),
    **drift** (slope of marker-index→recorded-PTS over a long run; band |slope−1| ≤ a set ppm and A/V
    offset drift < 1 frame over the run), **TC** (recorded `tmcd` vs injected TC, frame-exact).
  - Opt-in ctest (`framesync` label) + a long-run soak variant. **This rig is the operational definition
    of "100%."**
- **Phase 1 — Unified timing core + clock recovery + drift servo (P2 heart).** `SourceClock`
  (`PcrSourceClock`, `FlvSourceClock`, `ArrivalSourceClock`), `SessionTimeline`, `DriftEstimator` + audio
  resample compensation, reconnect re-lock to the recovered clock (not fresh arrival). Delivers
  maintained lip-sync + drift-free on SRT/RTMP. Verified by Phase 0 (incl. injected skew).
- **Phase 2 — NDI ingest (`NativeNdiIngestSession`).** New transport via the NDI SDK; `NdiSourceClock`
  + native timecode plug straight into Phases 1/3. Delivers NDI input and the cleanest timing source.
  (Somewhat independent — can be pulled earlier if NDI input is a near-term priority.)
- **Phase 3 — Timecode extraction, alignment & write (P3).** SMPTE 12M from SRT(SEI/TS) /
  RTMP(AMF/SEI) / NDI(native); align the timeline + sources by TC; write `tmcd` to the MKV. Delivers
  frame-accurate inter-cam (with common TC) + TC-aligned output.
- **Phase 4 — Inter-camera phase servo + confidence surfacing.** `SourceOffsetEstimator`, correction
  toward the reference, and the measured-phase + accuracy-tier UI. Delivers inter-cam phase-lock within
  measured/documented bounds (frame-accurate with TC).
- **Phase 5 — Reference-clock / PTP seam (genlock-readiness).** `PtpReference` (ST 2059 slave) as the
  `TimingReference` top tier + the documented integration points for SDI/ST 2110 and genlocked NDI to
  become authoritative (full hardware ingest is a separate program; this delivers the seam + a PTP
  client so the locked feeds upgrade to true genlock).

## Error handling / edge cases
- **Reconnect:** re-lock to the recovered sender clock + the last known offset (not fresh arrival) so a
  blip doesn't re-introduce phase error (replaces today's arrival re-anchor).
- **Source without a recoverable clock** (no PCR / unstable encoder): fall back to `ArrivalSourceClock`,
  mark the source `Approximate`, keep recording (no regression vs today).
- **Source without timecode:** inter-cam stays `Bounded` (recovered-clock estimate + ±ms bound); never
  block recording on missing TC.
- **Clock discontinuity / TC jump** (encoder restart, PCR discontinuity_indicator, TC wrap): detect and
  re-lock; video owns re-anchoring, audio follows (the AUD-4/PR-A model, generalized).
- **NDI runtime missing:** the `INdiReceiverBackend` reports unavailable; NDI sources cleanly fail with a
  logged reason (mirrors #54).
- **Drift servo saturation** (a source skews beyond what resampling can hide without audible artifacts):
  cap the correction, surface a `Bounded`/degraded tier rather than distorting audio.

## Testing & acceptance
The Phase-0 rig is the acceptance instrument; every later phase is gated by it. Plus per-unit unit tests
(`SourceClock` PLL convergence/jitter-rejection; `DriftEstimator` ppm accuracy; `TimecodeAligner` SMPTE
12M parse; `SourceOffsetEstimator` tiers) and the existing `e2e_av_lipsync` / native-ingest soak suites.
Honest caveat baked into the rig: one machine shares a wall clock (drift ≈ 0 by construction) — the
`skew_injector` (and, ideally, a two-machine mode) is what actually exercises drift and cross-source
phase.

## Risks & honest ceiling
- **No true genlock without a reference** — inter-cam frame-accuracy over clockless IP requires common
  timecode; otherwise it is bounded-and-measured. This is surfaced, not hidden.
- **RTMP clock precision** (ms FLV timestamps) is the weakest; expect a wider bound on RTMP-only inter-cam.
- **Audio drift correction is audible if abused** — keep ppm corrections gentle; prefer resampling over
  drop/insert; cap and surface rather than distort.
- **NDI on the public internet** is uncommon (it's a LAN transport) — NDI's value here is on-prem/venue
  contribution and as the reference bridge, not WAN.

## Out of scope (separate tracks)
- Full SDI / ST 2110 **hardware ingest** (DeckLink/Rivermax capture) — Phase 5 delivers only the
  reference seam + PTP client; the capture cards are their own program.
- The **output** side (preview, NDI/SDI out, genlocked presentation) — the #54 output-bus track.
- Drop-frame timecode *display* and rational **playback** stepping — tracked in the P1 deferred list.

## Suggested sequencing
**0 (rig) → 1 (timing core + drift servo) → {2 NDI ingest, 3 timecode} → 4 (inter-cam servo + UI) → 5
(PTP seam).** Phase 0 first is non-negotiable (it defines "frame-perfect" measurably). NDI (Phase 2) may
be pulled forward if NDI input is a near-term product need. Each phase merges independently and is green
on the Phase-0 rig before the next begins.
