# Timecode Evidence Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn NDI, H.264, HEVC, and RTMP metadata into validated,
generation-bound timecode evidence whose uncertainty is consumed by the
alignment servo, while rejecting unselected registered T.35 bodies.

**Architecture:** Introduce a small value-type evidence model and a cached per-ingest codec timing context. Codec-specific parsers produce validated observations; `TimecodeAlignerV2` unwraps them across midnight and returns exact, bounded, or incomparable offsets; `ReplayManager` resets generations and gates the servo on the returned bound.

**Tech Stack:** C++17, Qt 6 Core/Test, bounded RBSP/AMF0 parsers, CMake/CTest, Python mutation/proof checks.

## Global Constraints

- Standard H.264 `pic_timing`, HEVC `time_code`, and explicitly registered ITU-T T.35 ATC syntax must be parsed; no standard payload may be treated as a four-byte packed SMPTE word.
- Parameter sets and structured metadata are parsed only when their bytes change.
- Missing, variable, malformed, discontinuous, or implausible evidence must become `Incomparable`, never a confident correction.
- Source reconnect, URL replacement, rate change, parameter-set discontinuity, and implausible label jump must start a new evidence generation.
- `TimecodeAlignerV2::offset()` must remain allocation-free and use checked `__int128` intermediates.
- Release median steady-state extraction/alignment overhead must remain below 2% relative to commit `39ddf23c`.
- Use targeted `git add <paths>` and include `Co-Authored-By: Claude <noreply@anthropic.com>` in every commit.

---

## File Structure

- Create `recorder_engine/timing/timecodeevidence.h`: exact rates, provenance, validated evidence, alignment states, and limits.
- Create `recorder_engine/timing/timecodeevidence.cpp`: label validation, canonical rate conversion, checked arithmetic, and modulo-day helpers.
- Create `recorder_engine/ingest/h26xtimingcontext.h`: cached H.264/HEVC VUI/HRD context and parser result API.
- Create `recorder_engine/ingest/h26xtimingcontext.cpp`: parameter-set parsing and generation management.
- Modify `recorder_engine/ingest/h26xseitimecode.h` and
  `recorder_engine/ingest/h26xseitimecode.cpp`: codec-specific SEI parsing
  against active context plus strict registered T.35 envelope validation.
- Modify `recorder_engine/ingest/spsframerate.h` and `recorder_engine/ingest/spsframerate.cpp`: retain compatibility wrapper while exposing fixed/variable rate status.
- Modify `recorder_engine/ingest/ingestsession.h`, the three native ingest headers/implementations, `recorder_engine/streamworker.h`, `recorder_engine/streamworker.cpp`, `recorder_engine/replaymanager.h`, and `recorder_engine/replaymanager.cpp`: produce, carry, reset, and consume evidence.
- Modify `recorder_engine/timing/timecodealignerv2.h`, `recorder_engine/timing/timecodealignerv2.cpp`, `recorder_engine/timing/sourceoffsetestimator.h`, and `recorder_engine/timing/sourceoffsetestimator.cpp`: unwrap, bound, and consume evidence.
- Add focused fixtures/tests under `tests/fixtures/timecode`, `tests/unit`, and `tests/perf`.
- Modify `CMakeLists.txt`, `tests/CMakeLists.txt`, and `tests/unit/CMakeLists.txt`: compile new sources and register tests/mutations.

### Task 1: Evidence value types and strict SMPTE validation

**Files:**
- Create: `recorder_engine/timing/timecodeevidence.h`
- Create: `recorder_engine/timing/timecodeevidence.cpp`
- Modify: `recorder_engine/timing/timecodealignerv2.h`
- Modify: `recorder_engine/timing/smpte12m.cpp`
- Create: `tests/unit/tst_timecodeevidence.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `FrameRateQ`, `TimecodeProvenance`, `TimecodeEvidence`, `AlignmentOffset::Kind`, `validateTimecodeLabel()`, and `canonicalFrameRate()`.
- Consumes: `Smpte12mTimecode` and existing exact rational conventions.

- [ ] **Step 1: Write failing validation and canonical-rate tests**

Add table tests for 25, 30000/1001, 30, 50, 60000/1001, and 60; reject `frames >= nominalLabelRate`, `00:01:00;00` and `;01` at 29.97, non-drop labels with a semicolon, zero/negative/extreme rationals, and `INT64_MIN`. Assert 59.94 metadata canonicalizes to `60000/1001`, not `59940/1000`.

```cpp
void TestTimecodeEvidence::dropFrameRejectsSkippedLabels() {
    const FrameRateQ rate{30000, 1001};
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 0, true, true}, rate));
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 1, true, true}, rate));
    QVERIFY(validateTimecodeLabel({0, 1, 0, 2, true, true}, rate));
}

void TestTimecodeEvidence::canonicalizesNtscRatesExactly() {
    const auto rate5994 = canonicalFrameRate(59.94);
    const auto rate2997 = canonicalFrameRate(29.97);
    QVERIFY(rate5994.has_value());
    QVERIFY(rate2997.has_value());
    QCOMPARE(rate5994->num, int32_t(60000));
    QCOMPARE(rate5994->den, int32_t(1001));
    QCOMPARE(rate2997->num, int32_t(30000));
    QCOMPARE(rate2997->den, int32_t(1001));
}
```

- [ ] **Step 2: Run the new test and verify the missing API failure**

Run: `cmake -S . -B build/timecode -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/mingw_64 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DOLR_GPU_PIPELINE=ON -DOLR_WERROR=OFF -DOLR_BUILD_TESTS=ON -DOLR_FFMPEG_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/ffmpeg -DOLR_SRT_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/srt`

Run: `cmake --build build/timecode --target tst_timecodeevidence && ctest --test-dir build/timecode -R '^tst_timecodeevidence$' --output-on-failure`

Expected: build fails because `timecodeevidence.h` and its functions do not exist.

- [ ] **Step 3: Add the exact evidence model and checked helpers**

Define these public shapes exactly:

```cpp
struct FrameRateQ {
    int32_t num = 0;
    int32_t den = 1;
    bool valid() const { return num > 0 && den > 0; }
    friend bool operator==(FrameRateQ a, FrameRateQ b) {
        return a.num == b.num && a.den == b.den;
    }
};

enum class TimecodeProvenance : uint8_t {
    Ndi,
    H264PicTiming,
    HevcTimeCode,
    RegisteredAtc,
    RtmpMetadata
};

struct TimecodeEvidence {
    int64_t frameOfDay = -1;
    FrameRateQ labelRate;
    uint64_t sourceGeneration = 0;
    uint64_t timingGeneration = 0;
    TimecodeProvenance provenance = TimecodeProvenance::Ndi;
    bool dropFrame = false;
    bool discontinuity = false;
    int64_t arrivalSessionFrame = -1;
    FrameRateQ sessionRate;
    int64_t quantizationBoundUs = 0;
    int64_t driftBoundUs = 0;
    bool valid() const;
};

struct AlignmentOffset {
    enum class Kind : uint8_t { Exact, Bounded, Incomparable };
    Kind kind = Kind::Incomparable;
    int64_t offsetUs = 0;
    int64_t boundUs = 0;
    uint64_t sourceGenerationA = 0;
    uint64_t sourceGenerationB = 0;
    bool comparable() const { return kind != Kind::Incomparable; }
};
```

Implement `canonicalFrameRate(double)` with a 0.001 fps tolerance for the six canonical rates and a continued-fraction denominator cap of 100000 for other finite rates in `[12, 240]`. Reject every overflow before multiplication or absolute value.

- [ ] **Step 4: Run focused timecode math tests**

Run: `cmake --build build/timecode --target tst_timecodeevidence tst_smpte12m tst_timecodealignerv2 && ctest --test-dir build/timecode -R 'tst_(timecodeevidence|smpte12m|timecodealignerv2)' --output-on-failure`

Expected: all selected tests pass.

- [ ] **Step 5: Commit the evidence foundation**

```powershell
git add recorder_engine/timing/timecodeevidence.h recorder_engine/timing/timecodeevidence.cpp recorder_engine/timing/timecodealignerv2.h recorder_engine/timing/smpte12m.cpp tests/unit/tst_timecodeevidence.cpp tests/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "feat(timecode): add validated evidence model" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Cached H.264 timing context and standards-compliant `pic_timing`

**Files:**
- Create: `recorder_engine/ingest/h26xtimingcontext.h`
- Create: `recorder_engine/ingest/h26xtimingcontext.cpp`
- Modify: `recorder_engine/ingest/h26xseitimecode.h`
- Modify: `recorder_engine/ingest/h26xseitimecode.cpp`
- Modify: `recorder_engine/ingest/spsframerate.h`
- Modify: `recorder_engine/ingest/spsframerate.cpp`
- Modify: `tests/unit/tst_h26xseitimecode.cpp`
- Modify: `tests/unit/tst_spsframerate.cpp`
- Add: `tests/fixtures/timecode/h264_pic_timing_hrd.264`
- Add: `tests/fixtures/timecode/h264_pic_timing_no_hrd.264`
- Add: `tests/fixtures/timecode/h264_sps_hrd.bin`
- Add: `tests/fixtures/timecode/h264_bcd_false_positive.264`

**Interfaces:**
- Produces: `H26xTimingContext::updateParameterSets()` and `extractH26xSeiTimecode(annexB, codec, context)`.
- Consumes: `FrameRateQ`, `Smpte12mTimecode`, and `NativeVideoCodec`.

- [ ] **Step 1: Replace shortcut fixtures with failing standard-syntax tests**

Construct RBSP fixtures with real H.264 fields: optional `cpb_removal_delay`/`dpb_output_delay`, `pic_struct`, `clock_timestamp_flag`, `ct_type`, `nuit_field_based_flag`, `counting_type`, `full_timestamp_flag`, seconds/minutes/hours, `n_frames`, and `time_offset`. Include a negative fixture whose first four payload bytes are plausible BCD but whose timestamp flag is zero.

```cpp
void TestH26xSeiTimecode::h264LeadingBcdWithoutTimestampIsIgnored() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    const auto parsed = extractH26xSeiTimecode(fixture("h264_bcd_false_positive.264"),
                                               NativeVideoCodec::H264, context);
    QVERIFY(!parsed.valid);
}
```

- [ ] **Step 2: Run parser tests and confirm the old four-byte decoder fails them**

Run: `cmake --build build/timecode --target tst_h26xseitimecode tst_spsframerate && ctest --test-dir build/timecode -R 'tst_(h26xseitimecode|spsframerate)' --output-on-failure`

Expected: failures show the current `decodePayloadTimecode()` accepts invalid leading bytes or cannot read real syntax.

- [ ] **Step 3: Implement bounded SPS/VUI/HRD caching and H.264 parsing**

Expose this context API:

```cpp
class H26xTimingContext {
public:
    bool updateParameterSets(NativeVideoCodec codec, const QList<QByteArray>& vps,
                             const QList<QByteArray>& sps);
    NativeVideoCodec codec() const;
    FrameRateQ constantFrameRate() const;
    bool fixedFrameRate() const;
    uint64_t generation() const;
    const H264TimingSyntax* h264() const;
    const HevcTimingSyntax* hevc() const;
};
```

Hash/compare incoming parameter-set bytes and return without reparsing when unchanged. Parse scaling lists by skipping their signed Exp-Golomb entries rather than rejecting high profiles. Only expose `constantFrameRate()` when VUI timing is valid and `fixed_frame_rate_flag` establishes constant counting. Read every SEI field with a single bounds-checking bit reader; return typed unsupported/malformed results without partial timestamps.

- [ ] **Step 4: Run focused parser tests and the fuzz seed**

Run: `cmake --build build/timecode --target tst_h26xseitimecode tst_spsframerate && ctest --test-dir build/timecode -R 'tst_(h26xseitimecode|spsframerate)' --output-on-failure`

Expected: standard HRD/no-HRD fixtures pass; truncated and BCD false-positive cases are rejected.

- [ ] **Step 5: Commit H.264 parsing**

```powershell
git add recorder_engine/ingest/h26xtimingcontext.h recorder_engine/ingest/h26xtimingcontext.cpp recorder_engine/ingest/h26xseitimecode.h recorder_engine/ingest/h26xseitimecode.cpp recorder_engine/ingest/spsframerate.h recorder_engine/ingest/spsframerate.cpp tests/unit/tst_h26xseitimecode.cpp tests/unit/tst_spsframerate.cpp tests/fixtures/timecode/h264_pic_timing_hrd.264 tests/fixtures/timecode/h264_pic_timing_no_hrd.264 tests/fixtures/timecode/h264_sps_hrd.bin tests/fixtures/timecode/h264_bcd_false_positive.264
git commit -m "fix(ingest): parse standard h264 pic timing" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: HEVC `time_code` and registered T.35 envelope parsing

#### Fix Wave: remove the unspecified registered-ATC assumption

Primary-source review found no published ATC registration matching the
original generic country/provider/user-id placeholder. ITU-T T.35 delegates
provider-body semantics to each registration; ATSC `GA94` type 3 is caption
`cc_data`, while SMPTE ST 334-2 CDP timecode is VANC and is not that SEI
mapping. Task 3 therefore validates the T.35 envelope and ignores unknown
registrations, with explicit GA94/CDP false-positive tests. It must not claim
registered ATC support until a specific published registration and body syntax
are named. This correction removes a false-support claim; it is not a deferred
parser implementation.

**Files:**
- Modify: `recorder_engine/ingest/h26xtimingcontext.cpp`
- Modify: `recorder_engine/ingest/h26xtimingcontext.h`
- Modify: `recorder_engine/ingest/h26xseitimecode.cpp`
- Modify: `recorder_engine/ingest/h26xseitimecode.h`
- Modify: `tests/unit/tst_h26xseitimecode.cpp`
- Add: `tests/fixtures/timecode/h264_jm_pic_timing.264`
- Add: `tests/fixtures/timecode/h264_x264_pic_timing.264`
- Add: `tests/fixtures/timecode/hevc_hm_time_code.265`
- Add: `tests/fixtures/timecode/hevc_shm_time_code.265`
- Add: `tests/fixtures/timecode/README.md`

**Interfaces:**
- Produces: complete `H26xSeiTimecodeResult` for HEVC; validates and safely ignores
  T.35 registrations without a selected published ATC profile.
- Consumes: active `H26xTimingContext` and strict label validation from Task 1.

- [ ] **Step 1: Add failing HEVC/T.35 vector tests**

Cover `num_clock_ts` 0/1/3, `clock_timestamp_flag`, units-field presence flags, counting type, drop/discontinuity flags, signed `time_offset`, truncated messages, and unknown country/provider/user identifiers. Assert registered data without a selected published ATC profile is ignored.

- [ ] **Step 2: Verify tests fail against the generic payload decoder**

Run: `cmake --build build/timecode --target tst_h26xseitimecode && ctest --test-dir build/timecode -R '^tst_h26xseitimecode$' --output-on-failure`

Expected: HEVC and T.35-envelope assertions fail.

- [ ] **Step 3: Implement codec-specific payload dispatch**

Dispatch only these combinations:

```cpp
switch (codec) {
case NativeVideoCodec::H264:
    if (payloadType == 1) return parseH264PicTiming(payload, *context.h264());
    if (payloadType == 4) return validateRegisteredT35Envelope(payload);
    break;
case NativeVideoCodec::Hevc:
    if (payloadType == 136) return parseHevcTimeCode(payload, *context.hevc());
    if (payloadType == 4) return validateRegisteredT35Envelope(payload);
    break;
default:
    break;
}
return {};
```

Validate the T.35 country code and extension byte; for the well-known US ATSC
namespace, also validate the provider/user envelope. Return no timestamp
without a selected published registration body. The legacy packed payload is
removed and payload types 1/136 are routed only to their standard codec syntax.

Generate and retain small redistributable vectors from the JM/x264 H.264 and HM/SHM HEVC encoder families. `tests/fixtures/timecode/README.md` records encoder version, exact command/config, upstream license, expected message syntax, and SHA-256 for each vector. The constructed bit-level fixtures remain separate so each optional syntax branch is independently falsifiable.

- [ ] **Step 4: Run parser and fuzz tests**

Run: `cmake --build build/timecode --target tst_h26xseitimecode && ctest --test-dir build/timecode -R '^tst_h26xseitimecode$' --output-on-failure`

Expected: all HEVC, malformed, T.35-envelope, and unknown-registration cases pass.

- [ ] **Step 5: Commit HEVC/T.35 support**

```powershell
git add recorder_engine/ingest/h26xtimingcontext.h recorder_engine/ingest/h26xtimingcontext.cpp recorder_engine/ingest/h26xseitimecode.h recorder_engine/ingest/h26xseitimecode.cpp tests/unit/tst_h26xseitimecode.cpp tests/fixtures/timecode/h264_jm_pic_timing.264 tests/fixtures/timecode/h264_x264_pic_timing.264 tests/fixtures/timecode/hevc_hm_time_code.265 tests/fixtures/timecode/hevc_shm_time_code.265 tests/fixtures/timecode/README.md
git commit -m "feat(ingest): parse standard hevc timecode sei" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Ingest-session evidence production and cached metadata

**Files:**
- Modify: `recorder_engine/ingest/ingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`
- Modify: `recorder_engine/ingest/nativertmpingestsession.h`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`
- Modify: `recorder_engine/ingest/nativendiingestsession.h`
- Modify: `recorder_engine/ingest/nativendiingestsession.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`
- Modify: `tests/unit/tst_ndiingest.cpp`
- Create: `tests/unit/tst_ingesttimecodeevidence.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `DecodedVideoFrame::timecodeEvidence` and monotonically increasing source/timing generations.
- Consumes: `H26xTimingContext` and structured AMF0 traversal.

- [ ] **Step 1: Add failing producer integration tests**

Pass complete SRT access units and RTMP video/data messages through the production seams and assert provenance, exact rate, generation, discontinuity, and frame-of-day. Add NDI tests for 100 ns near-midnight rounding to frame zero of the next day and invalid sender rates.

- [ ] **Step 2: Run the producer tests and observe missing evidence**

Run: `cmake --build build/timecode --target tst_ingesttimecodeevidence tst_ndiingest tst_rtmpprotocol && ctest --test-dir build/timecode -R 'tst_(ingesttimecodeevidence|ndiingest|rtmpprotocol)' --output-on-failure`

Expected: evidence fields/generations are absent and the near-midnight case fails.

- [ ] **Step 3: Replace primitive frame fields with evidence**

Keep `sourceTimecode100ns` for the recording start-timecode tag, but replace the primitive `sourceTcFrames`/`sourceFrameRateNum`/`sourceFrameRateDen` alignment fields with:

```cpp
std::optional<TimecodeEvidence> timecodeEvidence;
```

Give each native ingest session a `uint64_t m_sourceGeneration`, incremented on every successful `open()`, plus one `H26xTimingContext`. SRT calls `updateParameterSets()` only when `H26xAccessUnit::parameterSets` changes. RTMP updates the context only when AVC/HEVC configuration records change. Preserve the evidence local across asynchronous decode callbacks.

Replace RTMP `fps`/raw search with one bounded AMF0 object traversal that returns typed number/string properties. Accept only `framerate`; canonicalize it with `canonicalFrameRate()`. A metadata timecode creates one bounded anchor and advances only from monotonic decoded-frame/PTS progression at a validated constant rate; it is not stamped unchanged onto every later frame. Metadata evidence may be used only when codec timing does not provide a stronger constant rate.

- [ ] **Step 4: Run all ingest evidence tests**

Run: `cmake --build build/timecode --target tst_ingesttimecodeevidence tst_h26xaccessunit tst_h26xseitimecode tst_ndiingest tst_rtmpprotocol && ctest --test-dir build/timecode -R 'tst_(ingesttimecodeevidence|h26xaccessunit|h26xseitimecode|ndiingest|rtmpprotocol)' --output-on-failure`

Expected: all selected tests pass and a parser-count assertion proves unchanged parameter sets are not reparsed.

- [ ] **Step 5: Commit evidence-producing ingests**

```powershell
git add recorder_engine/ingest/ingestsession.h recorder_engine/ingest/nativesrtingestsession.h recorder_engine/ingest/nativesrtingestsession.cpp recorder_engine/ingest/nativertmpingestsession.h recorder_engine/ingest/nativertmpingestsession.cpp recorder_engine/ingest/nativendiingestsession.h recorder_engine/ingest/nativendiingestsession.cpp tests/unit/tst_rtmpprotocol.cpp tests/unit/tst_ndiingest.cpp tests/unit/tst_ingesttimecodeevidence.cpp tests/unit/CMakeLists.txt
git commit -m "feat(ingest): emit generation-bound timecode evidence" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5: Rollover-aware alignment with exact uncertainty

**Files:**
- Modify: `recorder_engine/timing/timecodealignerv2.h`
- Modify: `recorder_engine/timing/timecodealignerv2.cpp`
- Modify: `tests/unit/tst_timecodealignerv2.cpp`
- Modify: `docs/hardest-technical-challenges/timecode_alignment_proof.py`

**Interfaces:**
- Produces: `observe(int, const TimecodeEvidence&)`, `resetSource(int)`, and exact/bounded/incomparable offsets.
- Consumes: validated evidence and modulo-day helpers.

- [ ] **Step 1: Add failing rollover, discontinuity, generation, drift, and overflow tests**

Cover 23:59:59 to 00:00:00 at all six rates, ambiguous multi-day gaps, reconnect generation changes, parameter-set generation changes, late arrival, 50 ppm drift, illegal negative inputs, and products near `INT64_MAX`.

- [ ] **Step 2: Run the aligner test and capture failures**

Run: `cmake --build build/timecode --target tst_timecodealignerv2 && ctest --test-dir build/timecode -R '^tst_timecodealignerv2$' --output-on-failure`

Expected: current first-anchor behavior cannot unwrap rollover or reject stale generations, and default drift produces the wrong zero bound.

- [ ] **Step 3: Implement unwrap state and bound propagation**

Store per source: current source/timing generations, previous unwrapped label, previous arrival frame, and immutable anchor. Select the modulo-day candidate nearest the session-derived expected progression only when it is unique inside the discontinuity bound. The pair bound is the checked sum of both observations' quantization bounds plus both drift bounds over their anchor separation. Return `Exact` only when `boundUs == 0`; return `Bounded` for positive valid bounds; return `Incomparable` for any invalid/stale/ambiguous state. `resetSource()` must clear both anchor and unwrap state.

- [ ] **Step 4: Match the proof grid from C++**

Add a C++ data table matching the Python grid and run:

`python docs/hardest-technical-challenges/timecode_alignment_proof.py && cmake --build build/timecode --target tst_timecodealignerv2 && ctest --test-dir build/timecode -R '^tst_timecodealignerv2$' --output-on-failure`

Expected: Python reports its complete grid proof and the C++ table matches every row.

- [ ] **Step 5: Commit rollover-aware alignment**

```powershell
git add recorder_engine/timing/timecodealignerv2.h recorder_engine/timing/timecodealignerv2.cpp tests/unit/tst_timecodealignerv2.cpp docs/hardest-technical-challenges/timecode_alignment_proof.py
git commit -m "fix(timecode): unwrap rollover and preserve uncertainty" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 6: End-to-end propagation, reset, estimator, and servo gating

**Files:**
- Modify: `recorder_engine/streamworker.h`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `recorder_engine/replaymanager.h`
- Modify: `recorder_engine/replaymanager.cpp`
- Modify: `recorder_engine/timing/sourceoffsetestimator.h`
- Modify: `recorder_engine/timing/sourceoffsetestimator.cpp`
- Modify: `tests/unit/tst_sourceoffsetestimator.cpp`
- Modify: `tests/unit/tst_replaymanager_timecode.cpp`

**Interfaces:**
- Produces: queued `frameTimecode(int, TimecodeEvidence)`, real `interCamBoundMs`, and bound-gated servo corrections.
- Consumes: `TimecodeAlignerV2::offset()` and StreamWorker connection/source identity changes.

- [ ] **Step 1: Add failing end-to-end and reset tests**

Drive a `DecodedVideoFrame` through StreamWorker and ReplayManager. Assert late arrival/drift produces `Bounded` with a nonzero UI bound; evidence beyond the confidence threshold does not move the servo; disconnect/reconnect and URL replacement clear old anchors; legal rollover remains aligned.

- [ ] **Step 2: Run estimator/manager tests and verify incorrect zero-bound behavior**

Run: `cmake --build build/timecode --target tst_sourceoffsetestimator tst_replaymanager_timecode && ctest --test-dir build/timecode -R 'tst_(sourceoffsetestimator|replaymanager_timecode)' --output-on-failure`

Expected: the late/drift and generation-reset assertions fail before implementation.

- [ ] **Step 3: Carry and consume evidence**

Register `TimecodeEvidence` as a Qt metatype. StreamWorker copies the evidence once with the selected frame and emits it only when that frame is muxed. ReplayManager calls `resetSource()` on `connectionChanged(false)`, URL replacement, and any generation/rate/discontinuity change. Extend `SourcePhaseEvidence` with `timecodeKind`, `timecodeOffsetUs`, and `timecodeBoundUs`; `SourceOffsetEstimator` reports the actual rounded-up millisecond bound.

Servo eligibility must be:

```cpp
const bool withinFrame = off.comparable() && off.boundUs <= outputFrameToleranceUs;
const bool withinCorrection = off.comparable() && off.boundUs <= kMaxTimecodeCorrectionBoundUs;
ev.timecodeAlignedToReference = s == m_referenceSource || withinFrame;
if (withinCorrection) target = clampServoTargetUs(off.offsetUs);
```

Do not default drift to zero: derive a conservative magnitude from both source clock ppm estimates, with a configured floor.

- [ ] **Step 4: Run end-to-end focused tests**

Run: `cmake --build build/timecode --target tst_sourceoffsetestimator tst_replaymanager_timecode tst_ingesttimecodeevidence && ctest --test-dir build/timecode -R 'tst_(sourceoffsetestimator|replaymanager_timecode|ingesttimecodeevidence)' --output-on-failure`

Expected: bound, servo gating, reconnect, replacement, and rollover tests pass.

- [ ] **Step 5: Commit consumer integration**

```powershell
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp recorder_engine/timing/sourceoffsetestimator.h recorder_engine/timing/sourceoffsetestimator.cpp tests/unit/tst_sourceoffsetestimator.cpp tests/unit/tst_replaymanager_timecode.cpp
git commit -m "fix(timecode): gate alignment servo on evidence bounds" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 7: Mutations, performance gate, documentation, and full verification

**Files:**
- Create: `tests/mutations/timecode_evidence_mutations.py`
- Create: `tests/perf/tst_timecodeevidence_perf.cpp`
- Create: `tests/perf/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/hardest-technical-challenges.md`

**Interfaces:**
- Produces: source-coupled mutation CTest and a machine-readable median/p95 comparison.
- Consumes: all production paths from Tasks 1-6.

- [ ] **Step 1: Add mutation cases and verify each mutant is detected**

The script must build narrow test variants for: missing rate propagation,
missing bound consumption, missing generation reset, missing rollover unwrap,
H.264 parser disconnected, HEVC parser disconnected, and registered T.35
envelope validation disconnected. Each variant runs one named focused test and
succeeds only when that test fails for its intended assertion.

Run: `python tests/mutations/timecode_evidence_mutations.py --build-dir build/timecode`

Expected before wiring: script reports at least one surviving mutant and exits nonzero.

- [ ] **Step 2: Add a steady-state benchmark with a 2% gate**

Benchmark 100000 cached H.264/HEVC extractions plus alignment offsets, discard warm-up iterations, report median and p95, and compare against a baseline executable built from `39ddf23c`. Fail when median overhead is `>= 1.02`; report p95 for review.

Run: `ctest --test-dir build/timecode -R '^timecode_evidence_perf$' --output-on-failure`

Expected: PASS with median ratio below 1.02 and no per-frame parameter-set parse count increase.

- [ ] **Step 3: Run complete verification**

```powershell
cmake --build build/timecode
ctest --test-dir build/timecode -L unit --output-on-failure
ctest --test-dir build/timecode -R 'timecode_evidence_(mutations|perf)' --output-on-failure
python docs/hardest-technical-challenges/timecode_alignment_proof.py
cmake -S . -B build/timecode-fuzz -G Ninja -DOLR_BUILD_FUZZERS=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/mingw_64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/timecode-fuzz --target fuzz_h26xseitimecode
build/timecode-fuzz/tests/fuzz/fuzz_h26xseitimecode.exe -max_total_time=120 tests/fuzz/seeds/fuzz_h26xseitimecode
git diff --check
```

Expected: build succeeds; unit, mutation, performance, and proof gates pass; `git diff --check` is silent.

Run the existing CI sanitizer matrix with the new focused tests included in both `-DOLR_SANITIZER="address;undefined"` and `-DOLR_SANITIZER="thread"` legs. Expected: the parser/producer/aligner/ReplayManager tests pass under ASan+UBSan and TSan, with no suppression added for this change.

- [ ] **Step 4: Update claims to match evidence**

Document supported payload registrations, exact versus bounded evidence, reset conditions, common-generator limitations, benchmark result, and the C++/proof coupling. Do not claim that coincident labels alone prove a shared generator.

- [ ] **Step 5: Commit final gates and documentation**

```powershell
git add tests/mutations/timecode_evidence_mutations.py tests/perf/tst_timecodeevidence_perf.cpp tests/perf/CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt .github/workflows/ci.yml docs/hardest-technical-challenges.md
git commit -m "test(timecode): falsify evidence pipeline regressions" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 8: Independent review and merge-ready handoff

**Files:**
- Modify only files required by actionable review findings.

**Interfaces:**
- Consumes: complete branch and verification evidence.
- Produces: a review-clean PR update; no automatic merge.

- [ ] **Step 1: Request fresh-context adversarial review**

Require the reviewer to inspect bit-syntax correctness, overflow handling, rollover ambiguity, generation resets, bound consumption, real producer wiring, and the 2% performance result.

- [ ] **Step 2: Reproduce every finding before changing code**

For each Critical/Important finding, add or identify a failing test, run it red, apply the smallest correction consistent with the design, then rerun the focused and full suites.

- [ ] **Step 3: Commit review corrections as focused changes**

Stage only the files belonging to each reproduced finding and commit them with the required co-author trailer. Confirm `git status --short` is clean before pushing.

- [ ] **Step 4: Push without bypassing hooks and stop before merge**

```powershell
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin fix/timecode-rate-aware-alignment
```

Expected: pre-push gates pass. Update/open the PR with verification and performance evidence, then hand it to the user without auto-merging.
