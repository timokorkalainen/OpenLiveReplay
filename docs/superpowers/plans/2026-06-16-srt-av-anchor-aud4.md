# AUD-4: single shared A/V anchor per source — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Anchor each source's audio and video to ONE shared timeline reference (the MPEG-TS PCR on the native path; the first-of-either PES on the ffmpeg path) instead of two independent first-packet-arrival anchors, eliminating the fixed per-source lip-sync offset (measured +63 ms today).

**Architecture:** Both ingest sessions already output recording-timeline-stamped media (video `sourcePtsMs`, audio `startSample`) that StreamWorker trusts. We replace each session's dual anchor with a single `(anchorTs90k, anchorStreamMs)` and map both streams `sourceMs = anchorStreamMs + (pts − anchorTs)/timebase`. Native recovers the PCR (extend `MpegTsParser`); ffmpeg uses the first packet of either stream. Validated by promoting the report-only `lipsync` measurement to a gate plus a new native-SRT lipsync gate.

**Tech Stack:** C++17, Qt6 Core, MPEG-TS parsing, FFmpeg (libavformat/avutil `av_rescale_q`), libsrt, Qt Test, bash e2e (ffmpeg/ffprobe/srt-live-transmit), CMake/Ninja/CTest.

**Spec:** `docs/superpowers/specs/2026-06-16-srt-av-anchor-aud4-design.md`

**Base branch:** `feat/srt-aud4-anchor` (off `origin/main` a383f43 — includes #41/JIT-5 + #43/JIT-1). Build dir `build/srt`. **Local-only; do not push** unless told. Format C++ with `/opt/homebrew/opt/llvm/bin/clang-format`; these files have pre-existing drift, so **format only the lines you add/change** (never `clang-format -i` a whole file).

**Key facts (verified against the code on this branch):**
- `MpegTsParser` already parses PAT→PMT and reads the adaptation field's `discontinuity_indicator` (`mpegtsparser.cpp:96-98`); it does NOT capture the PCR PID or the PCR.
- Native anchors: video `m_firstDts90k`/`m_anchorStreamTimeMs` (`nativesrtingestsession.cpp:631-635`), audio `m_firstAudioPts90k`/`m_audioAnchorStreamTimeMs` (`:667-669`) — independent.
- ffmpeg anchors: video `firstPacketDts`/`anchorStreamTimeMs` (`ffmpegingestsession.cpp:151-156`), audio `firstAudioDts`/`audioAnchorStreamTimeMs` (`:253-256`) — independent; per-stream `time_base`.
- The `lipsync` scenario (`run_sync_e2e.sh:231-252`) drives the **UDP→ffmpeg** path and emits `mean(audio_pts − video_pts)` ms; baseline **+63.3 ms** (`SYNC_BASELINE.md`). It always exits 0.
- The SRT producer (`srt_lib.sh:flash_marker_to_udps`) is **video-only**.
- The parser unit test (`tst_mpegtsparser.cpp`) already has `pmtSection(streams, pcrPid)` and `adaptationOnlyPacket(pid, cc, flags, len)` helpers.

---

### Task 1: MpegTsParser — recover the PCR

**Files:**
- Modify: `recorder_engine/ingest/mpegtsparser.h`
- Modify: `recorder_engine/ingest/mpegtsparser.cpp`
- Create: `tests/unit/tst_mpegtsparser_pcr.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/unit/tst_mpegtsparser_pcr.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/mpegtsparser.h"

// Build an adaptation-only TS packet on `pid` carrying a 48-bit PCR whose 90 kHz
// base is `pcrBase90k` (extension 0). Layout: af_length, flags(0x10=PCR_flag),
// then 6 PCR bytes (33-bit base | 6 reserved | 9-bit ext).
static QByteArray pcrPacket(quint16 pid, quint8 cc, qint64 pcrBase90k)
{
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char((pid >> 8) & 0x1f);
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x20 | (cc & 0x0f)); // adaptation only
    pkt[4] = char(183);                // adaptation_field_length
    pkt[5] = char(0x10);               // PCR_flag
    const quint64 b = quint64(pcrBase90k) & 0x1ffffffffULL;
    pkt[6] = char((b >> 25) & 0xff);
    pkt[7] = char((b >> 17) & 0xff);
    pkt[8] = char((b >> 9) & 0xff);
    pkt[9] = char((b >> 1) & 0xff);
    pkt[10] = char(((b & 0x1) << 7) | 0x7e); // last base bit + 6 reserved (1s)
    pkt[11] = char(0x00);                    // extension low byte (=0)
    return pkt;
}

static QByteArray patSection(quint16 pmtPid)
{
    QByteArray pat;
    pat.append(char(0x00));
    pat.append(QByteArray::fromHex("00b00d0001c100000001"));
    pat.append(char(0xe0 | ((pmtPid >> 8) & 0x1f)));
    pat.append(char(pmtPid & 0xff));
    pat.append(QByteArray(4, char(0x00)));
    return pat;
}

static QByteArray pmtSection(quint16 pcrPid, quint16 videoPid)
{
    QByteArray body;
    body.append(QByteArray::fromHex("0001c10000"));
    body.append(char(0xe0 | ((pcrPid >> 8) & 0x1f)));
    body.append(char(pcrPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    body.append(char(0x1b)); // H.264
    body.append(char(0xe0 | ((videoPid >> 8) & 0x1f)));
    body.append(char(videoPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    body.append(QByteArray(4, char(0x00)));
    QByteArray pmt;
    pmt.append(char(0x00));
    pmt.append(char(0x02));
    pmt.append(char(0xb0 | ((body.size() >> 8) & 0x0f)));
    pmt.append(char(body.size() & 0xff));
    pmt.append(body);
    return pmt;
}

static QByteArray tsSection(quint16 pid, const QByteArray& section)
{
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char(0x40 | ((pid >> 8) & 0x1f)); // payload_unit_start
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x10);
    memcpy(pkt.data() + 4, section.constData(), size_t(qMin(184, section.size())));
    return pkt;
}

class TestMpegTsParserPcr : public QObject {
    Q_OBJECT
private slots:
    void extractsPcrFromPcrPid();
    void noPcrLeavesInfoUnset();
};

void TestMpegTsParserPcr::extractsPcrFromPcrPid() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    const quint16 pmtPid = 0x1000, pcrPid = 0x0100, videoPid = 0x0100;
    parser.pushTsPacket(tsSection(0x0000, patSection(pmtPid)), &pes);
    parser.pushTsPacket(tsSection(pmtPid, pmtSection(pcrPid, videoPid)), &pes);

    MpegTsParser::TsPacketInfo info;
    parser.pushTsPacket(pcrPacket(pcrPid, 0, 123456789), &pes, &info);
    QCOMPARE(info.pcr90k, qint64(123456789));
}

void TestMpegTsParserPcr::noPcrLeavesInfoUnset() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    parser.pushTsPacket(tsSection(0x0000, patSection(0x1000)), &pes);
    MpegTsParser::TsPacketInfo info;
    // A PAT packet (no adaptation PCR) must not report a PCR.
    parser.pushTsPacket(tsSection(0x0000, patSection(0x1000)), &pes, &info);
    QCOMPARE(info.pcr90k, qint64(-1));
}

QTEST_GUILESS_MAIN(TestMpegTsParserPcr)
#include "tst_mpegtsparser_pcr.moc"
```

- [ ] **Step 2: Register the test** — in `tests/unit/CMakeLists.txt`, after `olr_add_unit_test(tst_mpegtsparser     olr_test_core)`:

```cmake
olr_add_unit_test(tst_mpegtsparser_pcr olr_test_core)
```

- [ ] **Step 3: Verify it fails to build** — Run: `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target tst_mpegtsparser_pcr`. Expected: `TsPacketInfo` not a member of `MpegTsParser`, and the 3-arg `pushTsPacket` overload not found.

- [ ] **Step 4: Header — add the out struct + PCR PID.** In `mpegtsparser.h`:

Add inside `class MpegTsParser { public:` (above `pushTsPacket`):
```cpp
    // Per-packet side info surfaced to the caller. pcr90k >= 0 iff THIS packet
    // carried a PCR (33-bit 90 kHz base) on the program's PCR PID; discontinuity
    // iff the PCR PID's adaptation field set the discontinuity_indicator.
    struct TsPacketInfo {
        qint64 pcr90k = -1;
        bool discontinuity = false;
    };
```

Change the `pushTsPacket` declaration to:
```cpp
    bool pushTsPacket(const QByteArray& packet, QList<PesPacket>* completedPes,
                      TsPacketInfo* info = nullptr);
```

Add a private member next to `m_pmtPid`:
```cpp
    quint16 m_pcrPid = 0xffff;
```

- [ ] **Step 5: Implementation.** In `mpegtsparser.cpp`:

Add a PCR reader in the anonymous namespace (after `readPts90k`):
```cpp
qint64 readPcrBase90k(const uchar* p)
{
    return (qint64(p[0]) << 25) | (qint64(p[1]) << 17) | (qint64(p[2]) << 9)
        | (qint64(p[3]) << 1) | (qint64(p[4]) >> 7);
}
```

Change `pushTsPacket`'s signature to match the header (`, TsPacketInfo* info = nullptr` — keep `= nullptr` only in the header, not the definition). Inside the adaptation-field block, where it currently reads only `discontinuity`, also read the PCR. Replace:
```cpp
        if (adaptationLength > 0) {
            discontinuity = (byteAt(packet, offset + 1) & 0x80) != 0;
        }
        offset += 1 + adaptationLength;
```
with:
```cpp
        if (adaptationLength > 0) {
            const quint8 afFlags = byteAt(packet, offset + 1);
            discontinuity = (afFlags & 0x80) != 0;
            // PCR_flag (0x10): 6-byte PCR follows the flags byte. Surface only the
            // program PCR (on the PCR PID) — that is the shared A/V clock reference.
            if (info && pid == m_pcrPid && (afFlags & 0x10) != 0
                && offset + 2 + 6 <= packet.size()) {
                info->pcr90k = readPcrBase90k(
                    reinterpret_cast<const uchar*>(packet.constData()) + offset + 2);
            }
        }
        if (info && pid == m_pcrPid) {
            info->discontinuity = discontinuity;
        }
        offset += 1 + adaptationLength;
```

Capture the PCR PID in `parsePmt` — after `int es = off + 12 + programInfoLength;` and its bounds check, add:
```cpp
    m_pcrPid = quint16(((byteAt(payload, off + 8) & 0x1f) << 8) | byteAt(payload, off + 9));
```

- [ ] **Step 6: Build + run, expect PASS** — Run: `cmake --build build/srt --target tst_mpegtsparser tst_mpegtsparser_pcr && ( cd build/srt && ctest -R "tst_mpegtsparser" --output-on-failure )`. Expected: both PASS (the existing `tst_mpegtsparser` still green — the new 3rd param is defaulted).

- [ ] **Step 7: Format changed lines + commit**
```bash
git add recorder_engine/ingest/mpegtsparser.h recorder_engine/ingest/mpegtsparser.cpp \
        tests/unit/tst_mpegtsparser_pcr.cpp tests/unit/CMakeLists.txt
git commit -m "feat(av): MpegTsParser recovers the PCR (90kHz base) from the PCR PID"
```

---

### Task 2: Native ingest — single shared anchor (PCR-first, first-PES fallback)

**Files:**
- Modify: `recorder_engine/ingest/nativesrtingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`

No isolated unit test (needs a live stream); proven by the native lipsync gate (Task 5) + no-regression on the existing native gates. Build + run the native gates here.

- [ ] **Step 1: Header.** In `nativesrtingestsession.h`, replace the six anchor members:
```cpp
    int64_t m_firstDts90k = -1;
    int64_t m_prevDts90k = -1;
    int64_t m_anchorStreamTimeMs = -1;
    int64_t m_firstAudioPts90k = -1;
    int64_t m_prevAudioPts90k = -1;
    int64_t m_audioAnchorStreamTimeMs = -1;
```
with the single shared anchor + the per-stream discontinuity-detection state:
```cpp
    // Single shared A/V anchor for this source: stream-time anchorTs90k (PCR base,
    // or the first PES timestamp if no PCR appeared yet) maps to wall-clock
    // anchorStreamTimeMs. Both video and audio map against it, so the recorded A/V
    // offset equals the true stream offset (they share the 90 kHz program clock).
    int64_t m_anchorTs90k = -1;
    int64_t m_anchorStreamTimeMs = -1;
    // Per-stream previous timestamps, for discontinuity (jump) detection only.
    int64_t m_prevDts90k = -1;
    int64_t m_prevAudioPts90k = -1;
```

Replace the helper declaration `int64_t sourcePtsMsForDecodedVideoPts(qint64 pts90k) const;` with a shared mapper:
```cpp
    int64_t sourcePtsMsFromAnchor(qint64 pts90k) const;
```
(Leave `sourcePtsMsForUnit` and `sourcePtsMsForAudio` declarations as-is; `sourcePtsMsFromVideoAnchor` static helper, if declared, becomes unused — remove its declaration too.)

- [ ] **Step 2: cpp — open() reset.** In `open()`, replace the six anchor resets:
```cpp
    m_firstDts90k = -1;
    m_prevDts90k = -1;        // (whatever lines exist there)
    m_anchorStreamTimeMs = -1;
    m_firstAudioPts90k = -1;
    m_prevAudioPts90k = -1;
    m_audioAnchorStreamTimeMs = -1;
```
with:
```cpp
    m_anchorTs90k = -1;
    m_anchorStreamTimeMs = -1;
    m_prevDts90k = -1;
    m_prevAudioPts90k = -1;
```
(There are two reset sites in `open()` near lines 153-158 — apply to the anchor lines; keep the other resets like `m_audioRemainderPts90k`.)

- [ ] **Step 3: cpp — feed PCR + discontinuity in `processReceivedBytes`.** Replace the parser call block:
```cpp
        QList<PesPacket> completedPes;
        if (!m_tsParser.pushTsPacket(packet, &completedPes)) {
            m_tsBuffer.remove(0, 1);
            continue;
        }
        m_tsBuffer.remove(0, kTsPacketSize);
        for (const PesPacket& pes : std::as_const(completedPes)) {
            processPesPacket(pes);
        }
```
with:
```cpp
        QList<PesPacket> completedPes;
        MpegTsParser::TsPacketInfo tsInfo;
        if (!m_tsParser.pushTsPacket(packet, &completedPes, &tsInfo)) {
            m_tsBuffer.remove(0, 1);
            continue;
        }
        m_tsBuffer.remove(0, kTsPacketSize);
        // PCR is the canonical shared anchor. A program discontinuity forces a
        // re-anchor; the first PCR (or, as a fallback, the first PES below) sets it.
        if (tsInfo.discontinuity) {
            m_anchorTs90k = -1;
        }
        if (m_anchorTs90k < 0 && tsInfo.pcr90k >= 0) {
            m_anchorTs90k = tsInfo.pcr90k;
            m_anchorStreamTimeMs =
                m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
        }
        for (const PesPacket& pes : std::as_const(completedPes)) {
            processPesPacket(pes);
        }
```

- [ ] **Step 4: cpp — codec-change reset keeps the shared anchor.** In `processPesPacket`, the splitter/codec-change block currently resets the video anchor:
```cpp
        m_firstDts90k = -1;
        m_prevDts90k = -1;
        m_anchorStreamTimeMs = -1;
```
Replace with (reset only the jump-detection prev; a video codec change does NOT reset the program clock, so the shared anchor stays so audio is unaffected):
```cpp
        m_prevDts90k = -1;
```

- [ ] **Step 5: cpp — rewrite `sourcePtsMsForUnit` (video) to use the shared anchor.** Replace the whole function:
```cpp
int64_t NativeSrtIngestSession::sourcePtsMsForUnit(const CompressedAccessUnit& unit) {
    const int64_t unitDts90k = unit.dts90k >= 0 ? unit.dts90k : unit.pts90k;
    const int64_t unitPts90k = unit.pts90k >= 0 ? unit.pts90k : unitDts90k;
    if (unitDts90k < 0 || unitPts90k < 0) {
        return -1;
    }

    // Video is the re-anchor authority: a big DTS jump => stream discontinuity =>
    // drop the shared anchor so the next PCR/PES re-establishes it (backstop to the
    // PCR discontinuity_indicator handled in processReceivedBytes).
    if (m_anchorTs90k >= 0 && m_prevDts90k >= 0) {
        const int64_t delta90k = unitDts90k - m_prevDts90k;
        if (delta90k > kForwardJump90k || delta90k < kBackwardTolerance90k) {
            log(QStringLiteral("Native SRT DTS discontinuity (%1 ms jump). Re-anchoring.")
                    .arg(delta90k / 90));
            m_anchorTs90k = -1;
        }
    }
    m_prevDts90k = unitDts90k;

    // First-PES fallback: if no PCR has anchored yet, anchor on this video DTS.
    if (m_anchorTs90k < 0) {
        m_anchorTs90k = unitDts90k;
        m_anchorStreamTimeMs =
            m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    }
    if (m_anchorStreamTimeMs < 0) {
        return -1;
    }
    return sourcePtsMsFromAnchor(unitPts90k);
}

int64_t NativeSrtIngestSession::sourcePtsMsFromAnchor(qint64 pts90k) const {
    if (m_anchorTs90k < 0 || m_anchorStreamTimeMs < 0 || pts90k < 0) {
        return -1;
    }
    return m_anchorStreamTimeMs + ((pts90k - m_anchorTs90k) / 90);
}
```
(Delete the old `sourcePtsMsForDecodedVideoPts` and the `sourcePtsMsFromVideoAnchor` static helper if present.)

- [ ] **Step 6: cpp — rewrite `sourcePtsMsForAudio` (audio is a follower).** Replace the whole function:
```cpp
int64_t NativeSrtIngestSession::sourcePtsMsForAudio(qint64 pts90k) {
    if (pts90k < 0) {
        return -1;
    }

    // Audio does NOT own the anchor (that independent anchor was the lip-sync bug).
    // Detect an audio discontinuity only to flush the decoder; timing uses the
    // shared anchor owned by the PCR/video path.
    if (m_prevAudioPts90k >= 0) {
        const int64_t delta90k = pts90k - m_prevAudioPts90k;
        if (delta90k > kForwardJump90k || delta90k < kBackwardTolerance90k) {
            log(QStringLiteral("Native SRT audio PTS discontinuity (%1 ms jump). Flushing.")
                    .arg(delta90k / 90));
            if (m_audioDecoder) {
                m_audioDecoder->reset();
            }
        }
    }
    m_prevAudioPts90k = pts90k;

    // First-PES fallback: if audio arrives before any PCR or video, it establishes
    // the shared anchor (first-of-either); otherwise it maps against the existing one.
    if (m_anchorTs90k < 0) {
        m_anchorTs90k = pts90k;
        m_anchorStreamTimeMs =
            m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    }
    int64_t sourcePtsMs = sourcePtsMsFromAnchor(pts90k);
    // Safety clamp (no re-anchor): never stamp audio absurdly ahead of the clock.
    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    if (sourcePtsMs >= 0 && nowMs >= 0 && sourcePtsMs > nowMs + 10000) {
        sourcePtsMs = nowMs;
    }
    return sourcePtsMs;
}
```

- [ ] **Step 7: Build** — Run: `cmake --build build/srt --target sync_harness record_harness`. Expected: clean. Fix any leftover reference to the removed members/helpers (`m_firstDts90k`, `m_firstAudioPts90k`, `m_audioAnchorStreamTimeMs`, `sourcePtsMsForDecodedVideoPts`, `sourcePtsMsFromVideoAnchor`).

- [ ] **Step 8: No-regression on the native gates** — Run: `( cd build/srt && ctest -L native-apple-ingest --output-on-failure )`. Expected: all pass (smoke/4cam/sync/trim/connect/reconnect/loss/soak/jitter/loss_multi/ui_stats). These prove the shared anchor didn't break content delivery; the A/V-offset improvement is gated in Task 5.

- [ ] **Step 9: Format changed lines + commit**
```bash
git add recorder_engine/ingest/nativesrtingestsession.h recorder_engine/ingest/nativesrtingestsession.cpp
git commit -m "feat(av): native ingest anchors A/V to one shared PCR-first reference"
```

---

### Task 3: ffmpeg ingest — single shared first-of-either-PES anchor

**Files:**
- Modify: `recorder_engine/ingest/ffmpegingestsession.cpp` (the `run()` loop)

- [ ] **Step 1: Replace the dual anchor state.** In `run()`, replace:
```cpp
    int64_t anchorStreamTimeMs = -1;
    int64_t firstPacketDts = AV_NOPTS_VALUE;
    int64_t prevPktDts = AV_NOPTS_VALUE;

    int64_t firstAudioDts = AV_NOPTS_VALUE;
    int64_t audioAnchorStreamTimeMs = -1;
    int64_t prevAudioDts = AV_NOPTS_VALUE;
```
with one shared anchor in a common timebase (microseconds), so audio and video — which may have different stream `time_base`s — compare on one clock:
```cpp
    // Single shared A/V anchor: stream position anchorMicros (AV_TIME_BASE_Q) maps
    // to wall-clock anchorStreamTimeMs. Both streams convert their timestamp to
    // microseconds and map against it, so A/V alignment is the true stream offset.
    int64_t anchorMicros = AV_NOPTS_VALUE;
    int64_t anchorStreamTimeMs = -1;
    int64_t prevVideoMicros = AV_NOPTS_VALUE; // video jump detection (re-anchor authority)
    int64_t prevAudioMicros = AV_NOPTS_VALUE; // audio jump detection (decoder/flush only)
```

- [ ] **Step 2: Video — re-anchor authority + map via shared anchor.** Replace the video anchor block (`bool needAnchor = ...` through `prevPktDts = pktDts;`):
```cpp
                const int64_t pktMicros = av_rescale_q(pktDts, videoTb, AVRational{1, 1000000});
                if (anchorMicros != AV_NOPTS_VALUE && prevVideoMicros != AV_NOPTS_VALUE) {
                    const int64_t deltaMs = (pktMicros - prevVideoMicros) / 1000;
                    constexpr int64_t kForwardJumpMs = 3000;
                    constexpr int64_t kBackwardTolMs = -200;
                    if (deltaMs > kForwardJumpMs || deltaMs < kBackwardTolMs) {
                        qDebug() << "Source" << m_sourceIndex << "DTS discontinuity ("
                                 << deltaMs << "ms jump). Re-anchoring.";
                        anchorMicros = AV_NOPTS_VALUE;
                    }
                }
                prevVideoMicros = pktMicros;
                if (anchorMicros == AV_NOPTS_VALUE) {
                    anchorMicros = pktMicros;
                    anchorStreamTimeMs = m_callbacks.recordingClockMs
                                             ? m_callbacks.recordingClockMs()
                                             : -1;
                }
```

- [ ] **Step 3: Video PTS remap.** Replace:
```cpp
                        int64_t frameTs = rawFrame->best_effort_timestamp;
                        if (frameTs == AV_NOPTS_VALUE) frameTs = pktDts;
                        int64_t relativeMs =
                            av_rescale_q(frameTs - firstPacketDts, videoTb, {1, 1000});

                        DecodedVideoFrame decoded;
                        decoded.frame = scaledFrame;
                        decoded.sourcePtsMs = anchorStreamTimeMs + relativeMs;
```
with:
```cpp
                        int64_t frameTs = rawFrame->best_effort_timestamp;
                        if (frameTs == AV_NOPTS_VALUE) frameTs = pktDts;
                        const int64_t frameMicros = av_rescale_q(frameTs, videoTb, AVRational{1, 1000000});

                        DecodedVideoFrame decoded;
                        decoded.frame = scaledFrame;
                        decoded.sourcePtsMs = anchorStreamTimeMs + (frameMicros - anchorMicros) / 1000;
```

- [ ] **Step 4: Audio — follower (first-of-either init, no self re-anchor).** Replace the audio anchor block (`bool needAudioAnchor = ...` through the `recPtsMs` computation and the far-ahead re-anchor block, i.e. lines computing `recPtsMs`):
```cpp
                        const int64_t audioMicros =
                            av_rescale_q(audioTs, audioTb, AVRational{1, 1000000});
                        if (prevAudioMicros != AV_NOPTS_VALUE) {
                            const int64_t aDeltaMs = (audioMicros - prevAudioMicros) / 1000;
                            if (aDeltaMs > 3000 || aDeltaMs < -200) {
                                qDebug() << "Source" << m_sourceIndex
                                         << "Audio discontinuity (" << aDeltaMs << "ms).";
                            }
                        }
                        prevAudioMicros = audioMicros;
                        // First-of-either fallback: audio sets the shared anchor only
                        // if nothing has yet; it never re-anchors on its own (that was
                        // the lip-sync bug). Video owns re-anchoring.
                        if (anchorMicros == AV_NOPTS_VALUE) {
                            anchorMicros = audioMicros;
                            anchorStreamTimeMs = nowMs;
                        }
                        int64_t recPtsMs = anchorStreamTimeMs + (audioMicros - anchorMicros) / 1000;
                        if (nowMs >= 0 && recPtsMs > nowMs + 10000) {
                            recPtsMs = nowMs; // safety clamp, no re-anchor
                        }
```
(Keep the subsequent `if (recPtsMs < 0) { ... continue; }` guard and everything after it unchanged.)

- [ ] **Step 5: Build** — Run: `cmake --build build/srt --target sync_harness record_harness`. Expected: clean. Fix any leftover `firstPacketDts`/`firstAudioDts`/`prevPktDts`/`prevAudioDts`/`audioAnchorStreamTimeMs` references. (`AVRational{1, 1000000}` = microseconds; the file already brace-inits AVRational like `{1, 1000}`, so don't use the C-only `AV_TIME_BASE_Q` macro.)

- [ ] **Step 6: No-regression on the ffmpeg paths** — Run: `( cd build/srt && ctest -L srt --output-on-failure ) && ( cd build/srt && ctest -L e2e -R "e2e_record_stereo|e2e_record_mono" --output-on-failure )`. Expected: srt 5/5 + both record tests pass.

- [ ] **Step 7: Format changed lines + commit**
```bash
git add recorder_engine/ingest/ffmpegingestsession.cpp
git commit -m "feat(av): ffmpeg ingest anchors A/V to one shared first-of-either reference"
```

---

### Task 4: A/V lip-sync gate (ffmpeg/UDP path)

**Files:**
- Modify: `tests/e2e/run_sync_e2e.sh` (gating mode for `lipsync`)
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Add a gating mode to the `lipsync` scenario.** In `run_sync_e2e.sh`, in the `lipsync)` case, replace the final emit + exit:
```bash
    emit "[sync] scenario=lipsync pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37_band=+40/-60)"
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;
```
with:
```bash
    emit "[sync] scenario=lipsync pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37_band=+40/-60)"
    if [ "${OLR_AV_SYNC_GATE:-0}" = "1" ]; then
        # Gate: EBU R37 — audio may lead the picture by <=40ms and lag by <=60ms.
        # offset = mean(audio_pts - video_pts); positive => audio lags.
        if [ "${NP:-0}" -lt 3 ]; then
            echo "FAIL: only ${NP:-0} flash/beep pairs — measurement unreliable"; exit 1
        fi
        if awk -v m="$MEAN" 'BEGIN{exit !(m >= -40 && m <= 60)}'; then
            echo "PASS: A/V offset ${MEAN}ms within EBU R37 (-40..+60)"; exit 0
        fi
        echo "FAIL: A/V offset ${MEAN}ms outside EBU R37 (-40..+60) — shared anchor regressed"
        exit 1
    fi
    echo "PASS: report emitted (diagnostic, non-gating)"
    exit 0
    ;;
```

- [ ] **Step 2: Register the gate.** In `tests/e2e/CMakeLists.txt`, after the `sync_*` report-only block (the `set_tests_properties(... LABELS "sync-report" ...)`), add:
```cmake
# AUD-4 A/V lip-sync GATE (ffmpeg/UDP ingest): the shared anchor must keep audio
# within EBU R37 (-40..+60 ms). Pre-AUD-4 this measured ~+63 ms and FAILS. Base 23492.
add_test(NAME e2e_av_lipsync
    COMMAND bash "${_sync_driver}" "$<TARGET_FILE:sync_harness>" lipsync 23492)
set_tests_properties(e2e_av_lipsync PROPERTIES
    LABELS "av-sync" TIMEOUT 120 RUN_SERIAL TRUE
    ENVIRONMENT "OLR_AV_SYNC_GATE=1")
```

- [ ] **Step 3: Reconfigure + run the gate** — Run: `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target sync_harness && ( cd build/srt && ctest -R e2e_av_lipsync --output-on-failure )`. Expected: PASS with `A/V offset <~few>ms within EBU R37`. If it FAILS (offset still ~+63), the Task 2/3 anchor change didn't take on this path — report the measured offset, do not widen the band.

- [ ] **Step 4: Commit**
```bash
git add tests/e2e/run_sync_e2e.sh tests/e2e/CMakeLists.txt
git commit -m "test(av): gate A/V lip-sync within EBU R37 on the ffmpeg ingest path"
```

---

### Task 5: Native SRT A/V lip-sync gate

**Files:**
- Modify: `tests/e2e/srt_lib.sh` (flash+beep producer + beep extractor)
- Create: `tests/e2e/run_srt_lipsync.sh`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Add a flash+beep producer + beep extractor to `srt_lib.sh`.** Append:
```bash
# Spawn ONE ffmpeg producer with a co-timed full-frame flash + 1kHz beep (first
# ~60ms of every source-second), MPEG-TS to a single UDP port. $1=udp_port
flash_beep_marker_to_udp() {
    local port="$1"
    local vflt="geq=lum='if(lt(mod(T,1),0.06),235,16)':cb=128:cr=128"
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "color=c=black:s=320x240:r=30" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -filter_complex "[0:v]${vflt}[v];[1:a]volume=volume='if(lt(mod(t,1),0.06),1,0)':eval=frame[a]" \
        -map "[v]" -map "[a]" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 -b:v 4M \
        -c:a aac -b:a 128k \
        -f mpegts "udp://127.0.0.1:${port}?pkt_size=1316" &
    SRT_LAST_PID=$!
    PIDS+=("$SRT_LAST_PID")
}

# Beep-onset pts_time series for one audio track (silence->sound rising edges).
# $1=mkv $2=audio-track-index.
beep_pts_series() {
    ffmpeg -hide_banner -loglevel info -i "$1" -map "0:a:$2" \
        -af "silencedetect=noise=-30dB:duration=0.03" -f null - 2>&1 \
    | awk '/silence_end:/ { for (i=1;i<=NF;i++) if ($i=="silence_end:") printf "%.6f\n", $(i+1) }'
}
```

- [ ] **Step 2: Create the gate** — `tests/e2e/run_srt_lipsync.sh`:
```bash
#!/usr/bin/env bash
# Local SRT e2e (AUD-4): A/V lip-sync over the NATIVE SRT ingest. A co-timed
# flash+beep MPEG-TS source goes UDP -> srt-live-transmit -> srt:// -> native
# ingest (PCR-anchored). Assert the recorded mean(audio_pts - video_pts) is within
# EBU R37 (audio lead <=40ms, lag <=60ms). Pre-AUD-4 the independent audio anchor
# baked in ~+60ms; the shared PCR anchor collapses it toward 0.
#
# Requires sync_harness + OLR_NATIVE_SRT=1 (set by the CTest registration).
# Usage: run_srt_lipsync.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23710}"
SECS="${OLR_SRT_LIPSYNC_SECS:-10}"
UDP=$BASE; SRT=$((BASE+1))

srt_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { (( ${#PIDS[@]} )) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

flash_beep_marker_to_udp "$UDP"
srt_bridge "$UDP" "$SRT"
sleep 1.5
MKV=$("$HARNESS" --url "$(srt_caller_url "$SRT")" \
        --outdir "$WORKDIR" --name srt_lipsync --seconds "$SECS" --fps 30 | tail -n1)
[ -n "$MKV" ] && [ -s "$MKV" ] || { echo "FAIL: native SRT lipsync produced no MKV"; exit 1; }

flash_pts_series "$MKV" 0 > "$WORKDIR/v.txt"
beep_pts_series  "$MKV" 0 > "$WORKDIR/a.txt"
read -r NP MEAN MAX <<<"$(paste "$WORKDIR/v.txt" "$WORKDIR/a.txt" | awk '
    NF==2 { d=($2-$1)*1000; s+=d; ad=(d<0?-d:d); if(ad>mx)mx=ad; n++ }
    END { if(n>0) printf "%d %.1f %.1f", n, s/n, mx; else printf "0 nan nan" }')"
echo "[srt-lipsync] base=$BASE pairs=${NP} av_offset_ms: mean=${MEAN} max=${MAX} (EBU_R37 -40..+60)"

[ "${NP:-0}" -ge 3 ] || { echo "FAIL: only ${NP:-0} flash/beep pairs — unreliable"; exit 1; }
if awk -v m="$MEAN" 'BEGIN{exit !(m >= -40 && m <= 60)}'; then
    echo "PASS: native SRT A/V offset ${MEAN}ms within EBU R37 (PCR-anchored)"; exit 0
fi
echo "FAIL: native SRT A/V offset ${MEAN}ms outside EBU R37 (-40..+60)"
exit 1
```

- [ ] **Step 3: `chmod +x tests/e2e/run_srt_lipsync.sh`**

- [ ] **Step 4: Register the gate.** In `tests/e2e/CMakeLists.txt`, inside the `if(APPLE)` block (after `e2e_native_srt_ui_stats`), add:
```cmake
    # AUD-4: A/V lip-sync over the PCR-anchored native SRT ingest (EBU R37). Base 23710.
    add_test(NAME e2e_native_srt_lipsync
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_lipsync.sh" "$<TARGET_FILE:sync_harness>" 23710)
    set_tests_properties(e2e_native_srt_lipsync PROPERTIES
        LABELS "native-apple-ingest" TIMEOUT 120 RUN_SERIAL TRUE
        ENVIRONMENT "OLR_NATIVE_SRT=1")
```

- [ ] **Step 5: Reconfigure + run** — Run: `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target sync_harness && ( cd build/srt && ctest -R e2e_native_srt_lipsync --output-on-failure )`. Expected: PASS with the offset within −40..+60. If it requires a slightly wider beep-detection window note it, but do NOT widen the EBU band. If FAIL, report the measured offset.

- [ ] **Step 6: Commit**
```bash
git add tests/e2e/srt_lib.sh tests/e2e/run_srt_lipsync.sh tests/e2e/CMakeLists.txt
git commit -m "test(av): native SRT A/V lip-sync gate over the PCR-anchored ingest"
```

---

### Task 6: Refresh the baseline + document

**Files:**
- Modify: `tests/e2e/SYNC_BASELINE.md`
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Regenerate the lipsync baseline line** — Run:
`( cd build/srt && bash "$PWD/../../tests/e2e/run_sync_e2e.sh" tests/e2e/sync_harness lipsync 23494 )` (report-only path, no gate env) and copy the printed `[sync] scenario=lipsync ...` line. In `SYNC_BASELINE.md`, update the lipsync entry to the new (~0 ms) measurement, with a note: `# post-AUD-4: shared anchor`.

- [ ] **Step 2: Append an AUD-4 section to `tests/e2e/SRT_README.md`:**
````markdown
## AUD-4: single shared A/V anchor per source

Each source anchors audio and video to ONE timeline reference instead of two
independent first-packet-arrival anchors (which baked in a fixed ~+63 ms lip-sync
offset). Native ingest recovers the **MPEG-TS PCR** (`MpegTsParser` extracts the
90 kHz base from the PCR PID's adaptation field) and anchors both streams to it,
falling back to the first PES if no PCR has appeared; the ffmpeg path anchors both
to the first-of-either packet (PCR isn't reachable through avformat). Audio maps
against the shared anchor and no longer re-anchors on its own — video/PCR owns
re-anchoring on a discontinuity.

**Gates:** `tst_mpegtsparser_pcr` (unit — PCR extraction); `e2e_av_lipsync`
(`av-sync`, ffmpeg/UDP path) and `e2e_native_srt_lipsync` (`native-apple-ingest`,
PCR path) both assert mean `(audio − video)` within EBU R37 (−40..+60 ms) — the
pre-AUD-4 +63 ms fails the lag bound; the shared anchor collapses it toward 0.
````

- [ ] **Step 3: Commit**
```bash
git add tests/e2e/SYNC_BASELINE.md tests/e2e/SRT_README.md
git commit -m "docs(av): refresh lipsync baseline + document AUD-4 shared anchor"
```

---

## After all tasks
- `( cd build/srt && ctest -L unit --output-on-failure )` — incl. `tst_mpegtsparser_pcr`.
- `( cd build/srt && ctest -L srt --output-on-failure )` — ffmpeg SRT 5/5.
- `( cd build/srt && ctest -L native-apple-ingest --output-on-failure )` — incl. the new lipsync gate.
- `( cd build/srt && ctest -R e2e_av_lipsync --output-on-failure )` — the ffmpeg A/V gate.
- Dispatch a final code review over the whole branch (focus: the shared-anchor correctness on both paths, the PCR bit-math, and discontinuity re-anchor coupling).
- Use superpowers:finishing-a-development-branch. **Do NOT push** unless explicitly told.
