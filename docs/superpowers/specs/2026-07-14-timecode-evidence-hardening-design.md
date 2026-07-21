# Timecode Evidence Hardening Design

## Purpose

PR #171 fixes the dimensional error caused by comparing nominal-30 timecode frames with a real-rate session axis. This follow-on completes Challenge 2 so real NDI, H.264, and HEVC sources produce trustworthy alignment evidence rather than merely passing synthetic packed-word tests.

The implementation must correctly parse standard timecode syntax, preserve uncertainty, survive rollover and reconnects, reject untrustworthy metadata, and prove the complete producer-to-servo path. It must not add repeated parameter-set parsing to the per-frame ingest path.

## Non-negotiable behavior

- Mixed 25, 30000/1001, 30, 50, 60000/1001, and 60 fps sources that share a continuous timecode generator remain aligned.
- Missing, variable, malformed, discontinuous, or implausible timing evidence never becomes a confident correction.
- A 24-hour timecode rollover is unwrapped when session timing makes the day choice unique; ambiguous jumps are incomparable.
- Disconnect, URL replacement, stream-identity change, rate change, and timecode discontinuity start a new source evidence generation.
- Alignment confidence includes arrival quantization and clock-drift uncertainty. The application never reports a zero bound when the evidence is only bounded.
- The servo applies a timecode-derived correction only when its bound is within the configured confidence threshold.
- Standard H.264/HEVC and registered timecode payloads are parsed according to their real syntax; no payload type is treated as an arbitrary four-byte SMPTE word.

## Architecture

### Parameter-set timing context

Introduce a codec timing context owned by each native ingest session. It caches the active parameter-set identity and parsed timing fields:

- H.264 SPS VUI timing, `fixed_frame_rate_flag`, `pic_struct_present_flag`, and HRD delay field widths needed by `pic_timing`.
- HEVC VPS/SPS VUI timing and the syntax needed by `time_code` SEI.
- A normalized exact `FrameRate` only when the active syntax establishes a constant picture-counting rate.

The context is rebuilt only when VPS/SPS bytes change. Access units reuse the cached result. Unsupported high-profile syntax must return a typed unsupported result rather than partially parsing it.

### Standard SEI parsing

Replace the generic leading-four-byte decoder with codec-specific parsers:

- H.264 `pic_timing` reads HRD delays when present, then `pic_struct` and the correct clock-timestamp fields using the active SPS context.
- HEVC `time_code` reads `num_clock_ts`, clock flags, counting type, discontinuity/drop flags, and field widths defined by the standard message.
- Registered ITU-T T.35 user data first validates country/provider/user identifiers. Only explicitly supported ATC registrations are decoded; unknown registrations are ignored.
- A project-private packed SMPTE payload, if retained for compatibility, receives its own UUID/registration and payload type. It is never conflated with standard messages.

Captured fixtures from at least two real encoder families per supported codec accompany small constructed fixtures. Negative fixtures include standard payloads whose leading bytes happen to form plausible BCD.

### Validated evidence model

Replace the bare `(tcFrames, tcRate)` observation with `TimecodeEvidence` containing:

- absolute label within the current day;
- exact counting rate;
- source generation and parameter-set generation;
- provenance (`Ndi`, `H264PicTiming`, `HevcTimeCode`, supported registered ATC, or structured RTMP metadata);
- continuity/discontinuity flags;
- arrival session frame/rate;
- quantization uncertainty and configured drift allowance.

Evidence construction validates `frames < nominalLabelRate`, legal drop-frame skipped labels, hours/minutes/seconds, exact rational bounds, and source-specific semantics. Extreme numerators/denominators and `INT64_MIN` inputs are rejected before arithmetic. NDI near-midnight rounding wraps to frame zero of the next day rather than producing a frame past the day.

### RTMP metadata

Replace byte-pattern search with bounded AMF object traversal. Accept only the `framerate` property in the decoded metadata object. Canonicalize values within a strict tolerance of standard rates to their exact rational forms; other finite constant rates in `[12, 240]` use a bounded rational conversion. Metadata is fallback evidence and cannot override stronger parameter-set timing.

### Rollover and continuity

Each source maintains an unwrap state keyed by source generation. Given the previous unwrapped label and session-frame delta, choose the modulo-day candidate nearest the expected progression. Reject the observation if multiple days are plausible or if the residual exceeds the discontinuity threshold.

`ReplayManager` clears the source anchor and unwrap state on disconnect, URL change, source-generation change, rate change, explicit discontinuity, or an implausible timecode jump. Reconnection to the same URL is not assumed to be the same generator without continuous evidence.

### Alignment result and servo gating

`AlignmentOffset` becomes evidence-bearing with `Exact`, `Bounded`, and `Incomparable` states. It preserves:

- `offsetUs`;
- `boundUs = arrivalQuantizationUs + driftResidualUs`;
- the contributing source generations and provenance.

The drift allowance is a configured conservative ppm value or a measured source/session-clock estimate; zero is not the default for long-separated anchors. `SourceOffsetEstimator` reports the real bound. `ReplayManager` labels evidence frame-accurate only when the bound is within one output-frame tolerance and applies it to the servo only within the correction-confidence limit. Otherwise it falls back to the bounded clock-offset estimator.

## Testing and falsifiability

- Unit tests cover standard H.264/HEVC bit syntax, HRD/no-HRD variants, registered headers, malformed/truncated input, legal/illegal drop-frame labels, exact standard rates, extreme ratios, and midnight rollover.
- Producer integration tests pass real access units through SRT and RTMP parsing into `DecodedVideoFrame` and verify exact evidence fields.
- NDI tests cover source generation, rational rates, day wrap, and invalid sender metadata.
- End-to-end C++ tests carry evidence through `StreamWorker` into `ReplayManager`, then assert confidence, bound, and servo behavior for mixed rate, late arrival, drift, reconnect, and source replacement.
- The mathematical proof remains a reference oracle, but a C++ table test must evaluate the production implementation over the same grid.
- Source-level mutation checks remove rate propagation, bound consumption, generation reset, rollover unwrap, and each parser connection; every mutation must fail a named test.

## Performance requirements

- Parameter sets and structured metadata are parsed on change, not per timecoded access unit.
- Per-frame work is bounded bit parsing of the SEI message plus constant-time evidence arithmetic.
- No heap allocation is introduced in `TimecodeAlignerV2::offset()`.
- A microbenchmark compares cached per-frame extraction and alignment against the #171 head. Median steady-state overhead must remain below 2%; otherwise the change is not ready.

## Documentation and claims

Update Challenge 2 documentation to distinguish exact evidence from bounded evidence and to list supported standard payload registrations. Claims must state that common-generator identity is inferred only from validated continuity/provenance; coincident labels alone never prove a shared generator.

## Acceptance

The branch is ready only when all new mutation checks fail for the intended reason, the C++ grid matches the proof oracle, focused and full suites pass under sanitizers, the performance gate passes, and an independent fresh-context review finds no Critical or Important issue.
