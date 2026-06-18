# Frame-Sync Phase 3 — Timecode Extraction, Alignment & Write — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extract SMPTE 12M timecode from every transport — H.264/HEVC SEI (`pic_timing` / registered ATC), MPEG-TS, RTMP AMF, and the already-carried NDI native timecode — onto `DecodedVideoFrame::sourceTimecode100ns`; build a pure `TimecodeAligner` that maps a source's timecode onto frame indices on the session timeline and aligns two sources whose frames carry equal TC; and write a `tmcd` timecode track + TC tags to the MKV via the muxer. Delivers **frame-accurate inter-camera alignment when sources carry common timecode** + TC-aligned output, verified by the Phase-0 rig's TC cell.

**Architecture:** A pure `Smpte12m` codec (parse/format the 4-byte SMPTE 12M timecode word ↔ `{hh,mm,ss,ff,dropFrame}`) is the shared primitive. A pure `H26xSeiTimecode` extractor pulls the ATC/`pic_timing` timecode out of an Annex-B access unit's SEI NALs (the NALs the existing `H26xAccessUnitSplitter` currently inspects-and-drops). SRT routes its access-unit SEI + any MPEG-TS-level TC, RTMP routes its AMF/SEI TC, NDI already routes its native 100 ns timecode through `DecodedVideoFrame::sourceTimecode100ns` (Phase 2) — so Phase 3 is the **consumer**: it normalises each into `sourceTimecode100ns` and feeds the `TimecodeAligner`. `TimecodeAligner` is pure (no Qt/FFmpeg): it observes `(sourceTimecode100ns, sessionFrameIndex)` per source and answers "what frame index does this TC map to" + "are two sources TC-aligned, and by how many frames". The muxer gains a `tmcd` track (one timecode track tagging the session's start TC) + per-track TC metadata. ReplayManager owns the aligner; StreamWorker forwards each frame's TC.

**Tech Stack:** C++17, Qt6, FFmpeg (AVFrame + libavformat `tmcd`/`AV_CODEC_ID_*`), Qt Test.

## Global Constraints
- Spec: `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md` (Phase 3 + `TimecodeAligner` §4). Builds on Phase 1 (`SourceClock`/`SessionTimeline`) + Phase 2 (NDI native TC already on `DecodedVideoFrame::sourceTimecode100ns`).
- Worktree `/tmp/olr-bcast`. Build `build/bcast`. Format only changed C++ lines (`git clang-format`, llvm `/opt/homebrew/opt/llvm/bin`); CI lint = clang-format 22.1.7; engine `.cpp` are hand-Allman.
- **Behavior-preserving first:** sources that carry no timecode keep `sourceTimecode100ns == -1` and record exactly as today; the aligner degrades to "no TC → no frame-accurate alignment, fall through to the recovered-clock mapping". Never block recording on missing/garbled TC.
- The `tmcd` track is **additive** — existing players ignore an unknown extra track; the `framesync`/`av-sync`/native gates must stay green.
- Verified anchors (from the map): `DecodedVideoFrame{ AVFrame* frame; int64_t sourcePtsMs; int64_t sourceTimecode100ns = -1; }` / `DecodedAudioChunk{ int64_t startSample; int64_t sourceTimecode100ns = -1; QByteArray pcmS16Stereo; }` `ingestsession.h:38-48`; NDI fills TC at `nativendiingestsession.cpp:432-434` (video) / `:447-449` (audio), `kNdiTimecodeSynthesize` sentinel `:45,56,66`; `H26xAccessUnitSplitter` + `CompressedAccessUnit{ codec; pts90k; dts90k; QByteArray annexB; H26xParameterSets parameterSets; }` `h26xaccessunit.h:16-37`, `inspectNal()` (currently keeps only SPS/PPS/VPS, **drops SEI**) `h26xaccessunit.cpp:265-281`; SRT access-unit loop `nativesrtingestsession.cpp:546-587`, video emit (TC unset) `:562-578`, audio emit `:689-693`, `MpegTsParser::TsPacketInfo{ pcr90k; discontinuity }` `mpegtsparser.h:14-18`; RTMP video emit (TC unset) `nativertmpingestsession.cpp:956-970`, AMF0 scan helpers `:48-175`, length-prefixed→AnnexB `:927`; Muxer track-creation block `muxer.cpp:38-115` (video `:39-65`, audio `:68-83`, metadata-subtitle `:86-96`, telemetry-subtitle `:99-115`), `m_subtitleTrackOffset`/`m_telemetryTrackOffset`/`m_telemetryTrackCount` `muxer.h:78-80`, `writeMetadataPacket` `muxer.cpp:253-278`, `getStream` `:308-315`, `recording_start_time` metadata `:124-126`; StreamWorker `onVideoFrame` callback `streamworker.cpp:292-329` (`decoded.sourcePtsMs` → `qf.sourcePts` `:305`); ReplayManager owns `m_clock`/`m_muxer`/`m_workers`, `masterPulse(frameIndex, elapsedMs)` `replaymanager.cpp:378`, worker create loop `:204-245`; `m_targetFps`/`kAudioSampleRate=48000`; the `#if defined(QT_TESTLIB_LIB)` + `friend class` test seam (`nativertmpingestsession.h:22-24`). **No existing `TimecodeAligner`/`Smpte12m`/`tmcd` anywhere** (grep-confirmed net-new).

---

### Task 1: `Smpte12m` — parse/format the SMPTE 12M timecode word (pure)

The shared primitive every extractor produces and the aligner consumes. Pure, no Qt/FFmpeg.

**Files:** Create `recorder_engine/timing/smpte12m.h`, `recorder_engine/timing/smpte12m.cpp`, `tests/unit/tst_smpte12m.cpp`; modify `tests/CMakeLists.txt` (`olr_test_core` source list), `tests/unit/CMakeLists.txt`, root `CMakeLists.txt` (engine sources, next to `driftestimator.cpp`).

**Interfaces — Produces:**
```cpp
#ifndef SMPTE12M_H
#define SMPTE12M_H
#include <cstdint>

// A decoded SMPTE 12M timecode. ff = frame within the second; dropFrame is the
// 29.97/59.94 NTSC drop-frame flag. valid=false means "no timecode".
struct Smpte12mTimecode {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    bool dropFrame = false;
    bool valid = false;
};

// SMPTE 12M helpers. Pure (no Qt/FFmpeg). The 32-bit word is the BCD-packed
// representation carried in H.264/HEVC pic_timing/ATC SEI and ANC; toFrameCount
// converts a wall-clock TC to an absolute frame number at a given integer fps.
namespace Smpte12m {
// Decode the 32-bit BCD timecode word (SMPTE 12M / ATC layout) -> fields.
Smpte12mTimecode fromPackedWord(uint32_t word);
// Encode fields -> the 32-bit BCD timecode word (inverse of fromPackedWord).
uint32_t toPackedWord(const Smpte12mTimecode& tc);
// Render "HH:MM:SS:FF" (drop-frame uses ';' before the frames field).
// Empty string when !valid.
//   e.g. {10,11,12,13,false,true} -> "10:11:12:13"
// drop-frame example {1,0,0,2,true,true} -> "01:00:00;02"
char* /*caller-owned, 12 bytes*/ format(const Smpte12mTimecode& tc, char out[12]);
// Absolute frame index since 00:00:00:00 at an integer fps (non-drop arithmetic;
// drop-frame skip is applied when tc.dropFrame and nominalFps is 30 or 60).
int64_t toFrameCount(const Smpte12mTimecode& tc, int nominalFps);
// 100 ns timestamp of this TC since 00:00:00:00 (= toFrameCount * 1e7 / fps).
int64_t to100ns(const Smpte12mTimecode& tc, int nominalFps);
// Decode a 100 ns timecode (NDI delivers TC as 100 ns since midnight) -> fields.
Smpte12mTimecode from100ns(int64_t timecode100ns, int nominalFps);
}
#endif // SMPTE12M_H
```

- [ ] **Step 1: Failing test** `tests/unit/tst_smpte12m.cpp`:
```cpp
#include <QtTest>
#include "recorder_engine/timing/smpte12m.h"

class TestSmpte12m : public QObject {
    Q_OBJECT
private slots:
    void packedRoundTrips();
    void formatNonDrop();
    void formatDrop();
    void frameCountNonDrop();
    void frameCountWraps24h();
    void to100nsMatchesFrameCount();
    void from100nsRoundTrips();
    void invalidFormatsEmpty();
};

void TestSmpte12m::packedRoundTrips() {
    const Smpte12mTimecode tc{10, 11, 12, 13, /*drop*/ false, /*valid*/ true};
    const uint32_t w = Smpte12m::toPackedWord(tc);
    const Smpte12mTimecode back = Smpte12m::fromPackedWord(w);
    QCOMPARE(back.hours, 10);
    QCOMPARE(back.minutes, 11);
    QCOMPARE(back.seconds, 12);
    QCOMPARE(back.frames, 13);
    QVERIFY(!back.dropFrame);
    QVERIFY(back.valid);
}
void TestSmpte12m::formatDrop() {
    char buf[12];
    Smpte12m::format(Smpte12mTimecode{1, 0, 0, 2, /*drop*/ true, /*valid*/ true}, buf);
    QCOMPARE(QString::fromLatin1(buf), QStringLiteral("01:00:00;02"));
}
void TestSmpte12m::formatNonDrop() {
    char buf[12];
    Smpte12m::format(Smpte12mTimecode{10, 11, 12, 13, false, true}, buf);
    QCOMPARE(QString::fromLatin1(buf), QStringLiteral("10:11:12:13"));
}
void TestSmpte12m::frameCountNonDrop() {
    // 00:00:01:00 at 25 fps = 25 frames.
    QCOMPARE(Smpte12m::toFrameCount(Smpte12mTimecode{0, 0, 1, 0, false, true}, 25),
             int64_t(25));
    // 00:01:00:00 at 30 fps = 1800 frames.
    QCOMPARE(Smpte12m::toFrameCount(Smpte12mTimecode{0, 1, 0, 0, false, true}, 30),
             int64_t(1800));
}
void TestSmpte12m::frameCountWraps24h() {
    // 24:00:00:00 wraps to 0.
    QCOMPARE(Smpte12m::toFrameCount(Smpte12mTimecode{24, 0, 0, 0, false, true}, 30),
             int64_t(0));
}
void TestSmpte12m::to100nsMatchesFrameCount() {
    const Smpte12mTimecode tc{0, 0, 1, 0, false, true}; // 1 second
    QCOMPARE(Smpte12m::to100ns(tc, 25), int64_t(10'000'000)); // 1 s in 100 ns
}
void TestSmpte12m::from100nsRoundTrips() {
    const Smpte12mTimecode tc = Smpte12m::from100ns(10'000'000, 25); // 1 s
    QCOMPARE(tc.seconds, 1);
    QCOMPARE(tc.frames, 0);
    QVERIFY(tc.valid);
}
void TestSmpte12m::invalidFormatsEmpty() {
    char buf[12];
    Smpte12m::format(Smpte12mTimecode{}, buf); // valid=false
    QCOMPARE(QString::fromLatin1(buf), QString());
}
QTEST_GUILESS_MAIN(TestSmpte12m)
#include "tst_smpte12m.moc"
```
- [ ] **Step 2: Register** in `tests/unit/CMakeLists.txt`: `olr_add_unit_test(tst_smpte12m   olr_test_core)`. Build → verify RED (`smpte12m.h` missing).
- [ ] **Step 3: Implement** `recorder_engine/timing/smpte12m.{h,cpp}`. `fromPackedWord`/`toPackedWord`: SMPTE 12M layout — frames in bits `[0..3]` (units) + `[4..5]` (tens), drop-frame flag bit `[6]`, seconds `[8..11]`+`[12..14]`, minutes `[16..19]`+`[20..22]`, hours `[24..27]`+`[28..29]`; BCD nibble pack/unpack. `format`: `snprintf(out, 12, "%02d:%02d:%02d%c%02d", h, m, s, tc.dropFrame ? ';' : ':', f)` (returns `out`); empty (`out[0]='\0'`) when `!valid`. `toFrameCount`: `((h*60 + m)*60 + s) * fps + f`, then for `dropFrame` at fps 30/60 subtract the NTSC drop (`dropPerMin = (fps==60)?4:2; totalMinutes = h*60+m; droppedFrames = dropPerMin*(totalMinutes - totalMinutes/10)`); wrap modulo `24h*fps`. `to100ns = toFrameCount * 10'000'000 / fps`. `from100ns`: inverse — `frameCount = ts100ns * fps / 10'000'000`, decompose back to fields (re-add drop-frame skips). Build + run → 8/8 PASS.
- [ ] **Step 4: Wire `smpte12m.cpp` into both targets** — `tests/CMakeLists.txt` `olr_test_core` source list + root `CMakeLists.txt` engine sources (next to `driftestimator.cpp`). Build the app + tests clean.
- [ ] **Step 5: Commit** — `git commit -m "feat(timecode): Smpte12m parse/format/frame-count (unit-tested)"`.

---

### Task 2: `H26xSeiTimecode` — extract SMPTE 12M from H.264/HEVC SEI (pure)

The `H26xAccessUnitSplitter` already walks NALs and currently **drops SEI** (`inspectNal` keeps only parameter sets). Add a pure extractor that scans an Annex-B access unit's SEI NALs for the `pic_timing` (H.264) / registered-user ATC (`payloadType 4`, `user_data_unregistered`/ATC) SEI carrying SMPTE 12M, with **no change** to the splitter's existing output.

**Files:** Create `recorder_engine/ingest/h26xseitimecode.h`, `.cpp`, `tests/unit/tst_h26xseitimecode.cpp`; modify CMake (`olr_test_engine` in `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`, root `CMakeLists.txt`).

**Interfaces — Produces:**
```cpp
#ifndef H26XSEITIMECODE_H
#define H26XSEITIMECODE_H
#include "pespacket.h"               // NativeVideoCodec
#include "recorder_engine/timing/smpte12m.h"
#include <QByteArray>

// Scan an Annex-B access unit for an embedded SMPTE 12M timecode SEI:
//   - H.264: pic_timing SEI (payloadType 1) clock_timestamp, OR registered ATC
//            user_data (payloadType 4) carrying SMPTE 12M.
//   - HEVC : time_code SEI (payloadType 136) OR registered ATC user_data.
// Returns {valid=false} when no timecode SEI is present (the common case).
// Pure: no Qt event loop, no FFmpeg.
Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec);
#endif // H26XSEITIMECODE_H
```

- [ ] **Step 1: Failing test** `tst_h26xseitimecode.cpp`: build a minimal Annex-B buffer = `[00 00 00 01][SEI NAL: nal_type=6 (H264) / 39 (HEVC), payloadType, payloadSize, payload(SMPTE 12M word, big-endian)]` followed by a dummy VCL NAL, and assert `extractH26xSeiTimecode(buf, H264)` returns the encoded `{hh,mm,ss,ff}` with `valid==true`. A buffer with **no** SEI → `valid==false`. A truncated SEI payload → `valid==false` (never reads OOB). An HEVC `time_code` SEI (payloadType 136) → decoded. Register `olr_add_unit_test(tst_h26xseitimecode olr_test_engine)`. Verify RED.
- [ ] **Step 2: Implement** `h26xseitimecode.{h,cpp}`: split `annexB` on start codes (`00 00 01` / `00 00 00 01` — reuse the same scan the splitter uses), select SEI NALs (`(nal[0]&0x1f)==6` for H.264; `((nal[0]>>1)&0x3f)==39||==40` for HEVC prefix/suffix SEI), strip emulation-prevention bytes (`00 00 03` → `00 00`), then walk the SEI RBSP `payloadType`/`payloadSize` (each is a sum of `0xFF` continuation bytes + a final `<0xFF`). For `pic_timing`/`time_code`/ATC, read the SMPTE 12M field and hand its 32-bit word to `Smpte12m::fromPackedWord`. **Strict bounds checks** at every step → `{valid=false}` on any short read (garbled TC must never crash or block recording). Build + run → PASS.
- [ ] **Step 3: CMake + commit** — add the `.cpp` to `olr_test_engine` + root engine sources. `git commit -m "feat(timecode): extract SMPTE 12M from H.264/HEVC SEI (pure)"`.

---

### Task 3: `TimecodeAligner` — TC → frame index + two-source alignment (pure)

The §4 unit. Pure (no Qt/FFmpeg): per source it records `(sourceTimecode100ns, sessionFrameIndex)` observations, maps a source's TC to the frame index it should land on, and — given two sources — reports whether equal-TC frames coincide and the residual frame offset to make them coincide.

**Files:** Create `recorder_engine/timing/timecodealigner.h`, `.cpp`, `tests/unit/tst_timecodealigner.cpp`; modify CMake (`olr_test_core`, `tests/unit/CMakeLists.txt`, root `CMakeLists.txt`).

**Interfaces — Produces:**
```cpp
#ifndef TIMECODEALIGNER_H
#define TIMECODEALIGNER_H
#include "smpte12m.h"
#include <cstdint>

// Per-source timecode tracker + inter-source aligner. Pure (no Qt/FFmpeg).
// "Frame index" is the session frame index (heartbeat tick count) at which a
// given timecode arrived; the aligner remembers the TC<->frame relation so any
// later TC can be mapped, and two sources' relations can be differenced.
class TimecodeAligner {
public:
    explicit TimecodeAligner(int nominalFps = 30);

    // Record that source TC (100 ns since midnight, or -1 = none) arrived on the
    // session frame `sessionFrameIndex`. -1 timecodes are ignored.
    void observe(int sourceIndex, int64_t sourceTimecode100ns, int64_t sessionFrameIndex);

    // Has this source produced at least one usable timecode?
    bool hasTimecode(int sourceIndex) const;

    // The session frame index this source's TC maps to. Returns -1 if the source
    // never carried TC. mediaTimecode100ns is the frame's own TC.
    int64_t toSessionFrameIndex(int sourceIndex, int64_t mediaTimecode100ns) const;

    // True iff both sources carry TC AND their TC<->frame relations agree to
    // within `toleranceFrames` (i.e. equal-TC frames coincide). When false but
    // both have TC, frameOffset() gives the correction.
    bool sourcesAligned(int sourceIndexA, int sourceIndexB, int toleranceFrames = 0) const;

    // Frames to ADD to source B's mapping so equal-TC frames coincide with A
    // (B leads -> positive). 0 if either lacks TC.
    int64_t frameOffset(int sourceIndexA, int sourceIndexB) const;

    void reset();

private:
    struct Anchor { bool set = false; int64_t tcFrames = 0; int64_t sessionFrame = 0; };
    int m_nominalFps;
    static constexpr int kMaxSources = 16;
    Anchor m_anchors[kMaxSources];
};
#endif // TIMECODEALIGNER_H
```

- [ ] **Step 1: Failing test** `tst_timecodealigner.cpp`:
```cpp
#include <QtTest>
#include "recorder_engine/timing/timecodealigner.h"

class TestTimecodeAligner : public QObject {
    Q_OBJECT
private slots:
    void noTimecodeMapsToMinusOne();
    void singleSourceMapsTcToFrame();
    void equalTcSourcesAlign();
    void offsetSourcesReportFrameOffset();
    void resetClears();
};

static int64_t tc100ns(int h, int m, int s, int f, int fps) {
    return Smpte12m::to100ns(Smpte12mTimecode{h, m, s, f, false, true}, fps);
}

void TestTimecodeAligner::noTimecodeMapsToMinusOne() {
    TimecodeAligner a(25);
    QVERIFY(!a.hasTimecode(0));
    QCOMPARE(a.toSessionFrameIndex(0, tc100ns(1, 0, 0, 0, 25)), int64_t(-1));
}
void TestTimecodeAligner::singleSourceMapsTcToFrame() {
    TimecodeAligner a(25);
    // At session frame 100, source 0 carried 01:00:00:00.
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    QVERIFY(a.hasTimecode(0));
    QCOMPARE(a.toSessionFrameIndex(0, tc100ns(1, 0, 0, 0, 25)), int64_t(100));
    // 01:00:00:01 (one frame later in TC) -> session frame 101.
    QCOMPARE(a.toSessionFrameIndex(0, tc100ns(1, 0, 0, 1, 25)), int64_t(101));
}
void TestTimecodeAligner::equalTcSourcesAlign() {
    TimecodeAligner a(25);
    // Both jam-synced: 01:00:00:00 arrived on the SAME session frame 100.
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    a.observe(1, tc100ns(1, 0, 0, 0, 25), 100);
    QVERIFY(a.sourcesAligned(0, 1, 0));
    QCOMPARE(a.frameOffset(0, 1), int64_t(0));
}
void TestTimecodeAligner::offsetSourcesReportFrameOffset() {
    TimecodeAligner a(25);
    // Same TC, but source 1's TC arrived 3 session frames LATER than source 0's.
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    a.observe(1, tc100ns(1, 0, 0, 0, 25), 103);
    QVERIFY(!a.sourcesAligned(0, 1, 0));
    QVERIFY(a.sourcesAligned(0, 1, 3));
    QCOMPARE(a.frameOffset(0, 1), int64_t(-3)); // pull B back 3 frames
}
void TestTimecodeAligner::resetClears() {
    TimecodeAligner a(25);
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    a.reset();
    QVERIFY(!a.hasTimecode(0));
}
QTEST_GUILESS_MAIN(TestTimecodeAligner)
#include "tst_timecodealigner.moc"
```
- [ ] **Step 2: Register** `olr_add_unit_test(tst_timecodealigner olr_test_core)`. Verify RED.
- [ ] **Step 3: Implement** `timecodealigner.{h,cpp}`. `observe`: ignore `sourceTimecode100ns < 0`; convert TC 100 ns → TC frame count (`tc100ns * m_nominalFps / 10'000'000`); store the first `(tcFrames, sessionFrame)` per source as the anchor (`Anchor.set=true`). `toSessionFrameIndex(src, tc)`: `-1` if `!m_anchors[src].set`; else `anchor.sessionFrame + (tcOf(tc) - anchor.tcFrames)`. `frameOffset(a,b)`: `0` if either unset; else `(B.sessionFrame - B.tcFrames) - (A.sessionFrame - A.tcFrames)` negated so it is "frames to add to B's mapping" — i.e. `-(deltaB - deltaA)` where `delta = sessionFrame - tcFrames`. `sourcesAligned(a,b,tol)`: both set AND `|frameOffset(a,b)| <= tol`. Bounds-guard `sourceIndex` against `kMaxSources`. Build + run → 5/5 PASS.
- [ ] **Step 4: Wire + commit** — add `timecodealigner.cpp` to `olr_test_core` + root engine sources. `git commit -m "feat(timecode): TimecodeAligner — TC->frame mapping + two-source alignment (unit-tested)"`.

---

### Task 4: Route SRT timecode onto `DecodedVideoFrame::sourceTimecode100ns`

SRT carries SMPTE 12M in the H.264/HEVC SEI of each access unit (and, less commonly, MPEG-TS-level). Extract it and set `sourceTimecode100ns` (today it stays at the `-1` default).

**Files:** Modify `recorder_engine/ingest/nativesrtingestsession.cpp` (+ `.h`); use `extractH26xSeiTimecode` from Task 2.

- [ ] **Step 1:** In the access-unit loop (`:546-587`), for each `CompressedAccessUnit unit`, call `Smpte12mTimecode tc = extractH26xSeiTimecode(unit.annexB, unit.codec);`. Convert a valid TC → 100 ns since midnight via `Smpte12m::to100ns(tc, m_targetFps)` and stash it as `m_pendingVideoTimecode100ns` (a session member, default `-1`); a `!valid` TC leaves the last value unchanged is **wrong** — instead set `m_pendingVideoTimecode100ns = -1` per access unit so a frame with no TC reports none (no stale TC bleed). Then in the decoder callback (`:562-578`) set `decodedFrame.sourceTimecode100ns = m_pendingVideoTimecode100ns;` before `onVideoFrame`. Audio: leave `DecodedAudioChunk::sourceTimecode100ns = -1` (TC is a video property; audio rides video's anchor — matches NDI where audio TC is only set when the SDK delivers it).
- [ ] **Step 2: Verify** — extend `tst_ingestbackendselector.cpp` (which already friends the SRT session) or add a focused unit that feeds a synthetic SEI-bearing access unit through the session's extraction path and asserts the emitted `DecodedVideoFrame::sourceTimecode100ns` matches the encoded TC; a no-SEI access unit emits `-1`. The native SRT av-sync gate stays green (TC is purely additive). Build + run.
- [ ] **Step 3: Commit** — `git commit -m "feat(timecode): SRT extracts SMPTE 12M from H.264/HEVC SEI onto sourceTimecode100ns"`.

---

### Task 5: Route RTMP timecode onto `DecodedVideoFrame::sourceTimecode100ns`

RTMP carries TC either in H.264/HEVC SEI inside the FLV video payload (after the length-prefixed→Annex-B conversion at `:927`) or in the AMF `onMetaData`. Reuse the same SEI extractor; AMF is a fallback.

**Files:** Modify `recorder_engine/ingest/nativertmpingestsession.cpp` (+ `.h`).

- [ ] **Step 1:** After the length-prefixed→Annex-B conversion (`QByteArray annexB = ... lengthPrefixedPayloadToAnnexB(...)` `:927`), call `Smpte12mTimecode tc = extractH26xSeiTimecode(annexB, codec);` and set `m_pendingVideoTimecode100ns = tc.valid ? Smpte12m::to100ns(tc, m_targetFps) : -1;`. In the video emit (`:956-970`) set `decodedFrame.sourceTimecode100ns = m_pendingVideoTimecode100ns;`.
- [ ] **Step 2:** AMF fallback — if no SEI TC was seen and `onMetaData` carried a `timecode`/`tc` string (extend the AMF0 scan helpers `:48-175` with a string-value reader rather than the existing contains-string probe), parse `"HH:MM:SS:FF"`/`"HH:MM:SS;FF"` via a small pure helper `Smpte12mTimecode parseTimecodeString(const QString&)` (add to `smpte12m.{h,cpp}` + a unit case in `tst_smpte12m.cpp`) and use it as the source TC until the next SEI TC appears. Keep it strictly best-effort (a malformed string → `valid=false`, no effect).
- [ ] **Step 3: Verify** — a focused unit feeding a synthetic SEI-bearing FLV video packet asserts the emitted `sourceTimecode100ns`; the RTMP av-sync/native gates stay green. Commit `feat(timecode): RTMP extracts SMPTE 12M (SEI + AMF fallback) onto sourceTimecode100ns`.

---

### Task 6: ReplayManager owns the `TimecodeAligner`; StreamWorker forwards each frame's TC

Wire the per-frame `sourceTimecode100ns` (now set by SRT/RTMP/NDI) into a single `TimecodeAligner` keyed by the session frame index, so the engine can answer "are these two cameras TC-aligned".

**Files:** Modify `recorder_engine/streamworker.{h,cpp}`, `recorder_engine/replaymanager.{h,cpp}`.

- [ ] **Step 1: StreamWorker** — in the `onVideoFrame` callback (`:292-329`) carry `decoded.sourceTimecode100ns` onto the `QueuedFrame` (a new `int64_t sourceTimecode100ns` field next to `sourcePts` `:305`). When `processEncoderTick` consumes the frame for the assigned track, emit a new signal `frameTimecode(int sourceIndex, int64_t sourceTimecode100ns, int64_t sessionFrameIndex)` (`sessionFrameIndex = m_internalFrameCount`) — only when `sourceTimecode100ns >= 0`. (No behavior change when TC is absent.)
- [ ] **Step 2: ReplayManager** — add a `TimecodeAligner m_tcAligner{m_fps};` member; in the worker-create loop (`:204-245`) `connect(worker, &StreamWorker::frameTimecode, this, &ReplayManager::onFrameTimecode, Qt::QueuedConnection);`. `onFrameTimecode(src, tc, frame)` → `m_tcAligner.observe(src, tc, frame);`. Add a public `bool sourcesFrameAligned(int a, int b) const { return m_tcAligner.sourcesAligned(a, b, 0); }` + `int64_t sourceFrameOffset(int a, int b) const { return m_tcAligner.frameOffset(a, b); }` (Phase 4 consumes these for the FrameAccurate tier).
- [ ] **Step 3: Verify** — the existing native gates stay green; a focused ReplayManager test (or a sync-harness assertion) feeds two jam-synced TC sources and asserts `sourcesFrameAligned(0,1)`. Commit `feat(timecode): ReplayManager TimecodeAligner; StreamWorker forwards per-frame TC`.

---

### Task 7: Muxer writes a `tmcd` track + per-track TC tags

Write the session's starting timecode into the MKV so the recording is TC-aligned for downstream tools.

**Files:** Modify `recorder_engine/muxer.{h,cpp}`, `recorder_engine/replaymanager.cpp` (pass the start TC into `init`).

- [ ] **Step 1:** Extend `Muxer::init(...)` with a trailing `const QString& startTimecode = QString()` parameter (a third overload mirroring the existing two `:31-35`, default empty = no `tmcd`). When non-empty + matches `HH:MM:SS[:;]FF`, set the standard FFmpeg timecode tag on the output: `av_dict_set(&m_outCtx->metadata, "timecode", startTimecode.toUtf8().constData(), 0)` next to `recording_start_time` (`:124-126`) — libavformat's matroska muxer materialises this as the MKV timecode tag. Per video track, also `av_dict_set(&st->metadata, "timecode", startTimecode...)` in the video-track loop (`:39-65`) so each track carries it.
- [ ] **Step 2: Wire** ReplayManager — when starting a recording, derive the session start TC from the first source that has timecode (query `m_tcAligner`/the worker's first TC, formatted via `Smpte12m::format`); pass it into `m_muxer->init(...)`. If no source carries TC, pass empty (no `tmcd`, no regression). Default integer-fps drop/non-drop per `m_fps`.
- [ ] **Step 3: Verify** — record a synthetic TC-bearing source (the Phase-0 rig's TC cell or a focused harness), then `ffprobe -show_streams -show_format` the MKV asserts a `timecode` tag equal to the injected start TC (frame-exact). A no-TC recording has no `timecode` tag and is otherwise byte-identical. Commit `feat(timecode): muxer writes tmcd/timecode tags from the session start TC`.

---

### Task 8: Gate the rig (TC cell) + docs

**Files:** Modify `tests/e2e/CMakeLists.txt` (flip the framesync `tc` cell to a gate), `tests/e2e/FRAMESYNC.md`, `docs/native-ingest-workstream-remaining.md`.

- [ ] **Step 1:** Turn the Phase-0 `e2e_framesync_tc` cell into a **gate** (`OLR_FRAMESYNC_GATE=1`): inject a known TC into the marker source, record, and assert recorded `tmcd` == injected TC frame-exact + that two common-TC sources are reported aligned (≤1 frame). Run `( cd build/bcast && ctest -L framesync -R tc --output-on-failure )` — green.
- [ ] **Step 2: Docs** — in `FRAMESYNC.md` record the TC accuracy achieved; tick **P3 — Timecode (TC-1/TC-4/TC-5)** in `docs/native-ingest-workstream-remaining.md:109` (extract SMPTE 12M, align by TC, write `tmcd`); note NDI already carried native TC through the decoded callbacks and Phase 3 is the mux/alignment consumer. Commit `docs(timecode): document SMPTE 12M extraction/alignment/tmcd; gate the TC rig cell`.

---

## After all tasks
- `( cd build/bcast && ctest -L unit )` incl. `tst_smpte12m`, `tst_h26xseitimecode`, `tst_timecodealigner`, the SRT/RTMP TC extraction units.
- `( cd build/bcast && ctest -L native-apple-ingest -L native-rtmp -L native-ndi -L av-sync )` green (TC is additive; no-TC sources byte-identical).
- `( cd build/bcast && ctest -L framesync -R tc )` — recorded `tmcd` frame-exact vs injected; two common-TC sources reported aligned.
- Final review: SEI bounds-safety (garbled TC never crashes/blocks); aligner frame-offset sign convention; `tmcd` tag round-trips through `ffprobe`; no stale-TC bleed (each AU resets `m_pendingVideoTimecode100ns`).

## Self-review
- **Spec coverage:** SMPTE 12M extraction H.264/HEVC SEI → Tasks 2,4,5; MPEG-TS/RTMP-AMF paths → Tasks 4 (SRT/TS), 5 (RTMP/AMF fallback); NDI native TC is already on `DecodedVideoFrame::sourceTimecode100ns` (Phase 2) and Phase 3 consumes it via Task 6 (no re-implementation — matches the remaining-work P3 line); `TimecodeAligner` (TC→frame index on the session timeline; equal-TC two-source alignment) → Task 3 + wired in Task 6; write `tmcd`/TC tags via `muxer.cpp` → Task 7; unit tests (12M parse, frame-index mapping, two-source alignment) → Tasks 1,3. Covered.
- **Types consistent:** `Smpte12m`/`Smpte12mTimecode`/`extractH26xSeiTimecode`/`TimecodeAligner`/`sourceTimecode100ns`/`sourcesAligned`/`frameOffset`/`toSessionFrameIndex` used identically across tasks and match the spec's `TimecodeAligner` §4 + `ClockQuality`/`AnchoredSourceClock` from Phases 1/2 (unchanged). `ReferenceTier`/`ConfidenceTier` are deliberately NOT introduced here — they belong to Phase 4 (`SourceOffsetEstimator`).
- **No placeholders:** every code step gives complete `.h`/test bodies + exact line anchors; no "TBD"/"add error handling" — the SEI extractor's bounds-safety and the aligner's sign convention are spelled out.
- **Independence/ordering:** Tasks 1-3 are pure + standalone; 4-6 depend on 1-3; 7-8 depend on 6. Phase 4's FrameAccurate tier reads `ReplayManager::sourcesFrameAligned`/`sourceFrameOffset` from Task 6.
