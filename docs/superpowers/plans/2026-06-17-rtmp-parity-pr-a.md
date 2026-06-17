# Native RTMP parity (PR A) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Bring native RTMP ingest to parity with native SRT — a backend-agnostic health/stats dot fed by RTMP-meaningful signals, plus a single shared A/V anchor — without touching the ffmpeg path (that is PR B).

**Architecture:** Generalize the SRT-only stats pipe to a backend-tagged `IngestStats` graded by one of two pure functions (`srtHealth` unchanged + new `rtmpHealth`), dispatched in `UIManager` by a `kind` discriminator. The RTMP session populates `IngestStats` (bytes/last-packet-age/keyframe-age/decode-failures) and is reworked to use one shared A/V anchor that video owns and audio follows (the AUD-4 model). SRT behavior is byte-identical (rename only).

**Tech Stack:** C++17, Qt6 (Core/QML/Test), FFmpeg (AVFrame only here), Qt Test, bash e2e (Python RTMP fixture).

**Spec:** `docs/superpowers/specs/2026-06-17-rtmp-parity-design.md`

**Base branch:** `feat/rtmp-parity`, off `origin/main` (`6fe8d24`). Build dir `build/ingest`. **Local-only.** Format only changed C++ lines: after `git add`, run `PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format` then re-`git add`. **CI lint uses clang-format 22.1.7** — the changed-line gate; engine `.cpp` files are hand-formatted, never whole-file format. After committing, the final regression confirms `tst_srt_health`, `ctest -L native-rtmp`, `ctest -L native-apple-ingest`, `ctest -L unit` all green.

**Verified code anchors (current code, read 2026-06-17):**
- `ingestsession.h:52-57` `struct SrtStats{qint64 recvTotal,retransTotal,lossTotal,dropTotal}`; `:61` `enum class SrtHealth{NA=0,Green,Amber,Red}`; `:67` `srtHealth(prev,cur,amberRetransRate)`; `:80` `reportStats(const SrtStats&)`; `:102` `Q_DECLARE_METATYPE(SrtStats)`.
- `ingestsession.cpp:19-30` `srtHealth` impl.
- `nativesrtingestsession.cpp:193-198` fills a local `SrtStats` and calls `reportStats`.
- `streamworker.h:94` `statsUpdated(int, SrtStats)`; `replaymanager.h:90` `sourceStatsUpdated(int, SrtStats)`.
- `uimanager.h:302` `onSourceStatsUpdated(int, SrtStats)`; `:377-382` `struct SrtStatsEntry{SrtStats last; bool seen; int health;}` + `m_sourceStats`; `:214,217,219` `sourceLinkHealth`/`sourceHasSrtStats`/`sourceStatsTooltip`; `:302` slot.
- `uimanager.cpp:1010-1013` re-baseline; `:1019-1058` `onSourceStatsUpdated`/`sourceLinkHealth`/`sourceHasSrtStats`/`sourceStatsTooltip`.
- `Main.qml:1380` `sourceHasSrtStats(streamRow.index)`.
- `sync_harness.cpp:103` `QHash<int,SrtStats> latestStats`; `:112` lambda; `:159-162` `stats src=… recv/retrans/loss/drop` print.
- `nativertmpingestsession.h:56-63` anchor + `m_lastPacketAtMs`/`m_receivedChunkBytes` members; `:98-99` `sourcePtsMsForVideo/Audio`.
- `nativertmpingestsession.cpp:999-1054` anchor fns; `:296-308` run loop; `:514-521` readMessage idle/stall; `:726-880` `processVideoMessage` (decode at `:860-879`); `:891-971` `processAudioMessage`.
- `tests/unit/tst_srt_health.cpp` model; `tests/unit/tst_ingestbackendselector.cpp:157-244` existing RTMP anchor tests (gated `OLR_NATIVE_RTMP_AVAILABLE`).
- `tests/e2e/run_srt_ui_stats.sh` + `tests/e2e/CMakeLists.txt:233-235` (`e2e_native_srt_ui_stats`) + `:263-308` native-rtmp block; `tests/e2e/rtmp_lib.sh` (`rtmp_server`, `rtmp_generate_tone_flv`), `run_rtmp_smoke.sh`.

---

### Task 1: Generalize the stats/health model (`SrtStats`→`IngestStats`, add `rtmpHealth`)

One cohesive task — the type rename ripples; the build is only consistent when all references change together. The new `rtmpHealth` is TDD'd; SRT stays behavior-identical.

**Files:** `recorder_engine/ingest/ingestsession.h`, `recorder_engine/ingest/ingestsession.cpp`, `recorder_engine/ingest/nativesrtingestsession.cpp`, `recorder_engine/streamworker.h` (+`.cpp` if the signal is referenced), `recorder_engine/replaymanager.h` (+`.cpp`), `uimanager.h`, `uimanager.cpp`, `Main.qml`, `tests/e2e/sync_harness.cpp`, `tests/unit/tst_srt_health.cpp` (type rename), `tests/unit/tst_rtmp_health.cpp` (new), `tests/unit/CMakeLists.txt`.

- [ ] **Step 1: Failing test** — create `tests/unit/tst_rtmp_health.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"

class TestRtmpHealth : public QObject {
    Q_OBJECT
private slots:
    void freshDecodingIsGreen();
    void stallIsRed();
    void briefStallIsAmber();
    void decodeFailureIsAmber();
    void sustainedDecodeFailureIsRed();
    void staleKeyframeIsAmber();
    void counterResetIsGreen();
};

static IngestStats rtmpSnap(quint64 bytes, qint64 lastPktAge, qint64 keyframeAge,
                            quint64 decodeFails) {
    IngestStats s;
    s.kind = IngestStatsKind::Rtmp;
    s.bytesTotal = bytes;
    s.lastPacketAgeMs = lastPktAge;
    s.keyframeAgeMs = keyframeAge;
    s.decodeFailures = decodeFails;
    return s;
}

void TestRtmpHealth::freshDecodingIsGreen() {
    QCOMPARE(rtmpHealth(rtmpSnap(100000, 50, 500, 0), rtmpSnap(200000, 50, 500, 0)),
             SourceHealth::Green);
}
void TestRtmpHealth::stallIsRed() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 0), rtmpSnap(200000, 3500, 4000, 0)),
             SourceHealth::Red); // no fresh media for 3.5 s
}
void TestRtmpHealth::briefStallIsAmber() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 0), rtmpSnap(210000, 1500, 700, 0)),
             SourceHealth::Amber); // 1.5 s gap, bytes still trickling
}
void TestRtmpHealth::decodeFailureIsAmber() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 3), rtmpSnap(300000, 50, 500, 4)),
             SourceHealth::Amber); // one new decode failure, bytes advancing
}
void TestRtmpHealth::sustainedDecodeFailureIsRed() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 3), rtmpSnap(200000, 50, 500, 8)),
             SourceHealth::Red); // failing AND no new bytes
}
void TestRtmpHealth::staleKeyframeIsAmber() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 5500, 0), rtmpSnap(300000, 50, 6000, 0)),
             SourceHealth::Amber); // no keyframe for ~6 s
}
void TestRtmpHealth::counterResetIsGreen() {
    QCOMPARE(rtmpHealth(rtmpSnap(5000000, 50, 500, 9), rtmpSnap(100000, 50, 500, 0)),
             SourceHealth::Green); // reconnect restarts counters
}

QTEST_GUILESS_MAIN(TestRtmpHealth)
#include "tst_rtmp_health.moc"
```

- [ ] **Step 2: Register the test** — in `tests/unit/CMakeLists.txt`, add after the `olr_add_unit_test(tst_srt_health   ...)` line (match its formatting/lib):
```cmake
olr_add_unit_test(tst_rtmp_health      olr_test_core)
```

- [ ] **Step 3: Verify RED** — `cmake -S . -B build/ingest >/dev/null 2>&1 ; cmake --build build/ingest --target tst_rtmp_health` → fails (`IngestStats`/`IngestStatsKind`/`rtmpHealth`/`SourceHealth` undeclared).

- [ ] **Step 4: Generalize the model in `ingestsession.h`.** Replace the `SrtStats` struct + `SrtHealth` enum + `srtHealth` decl (`:49-67`) with:

```cpp
// Which backend produced this snapshot — selects the health grader. SRT fills the
// loss-domain counters; RTMP (TCP, no loss) fills the liveness/throughput fields.
enum class IngestStatsKind { Unknown = 0, Srt, Rtmp };

// Per-source ingest stats sampled ~1/sec and pushed to the UI via
// IngestCallbacks::reportStats. Tagged by kind; each backend fills its own fields
// (the others stay 0). See srtHealth()/rtmpHealth() below.
struct IngestStats {
    IngestStatsKind kind = IngestStatsKind::Unknown;
    // SRT (libsrt bstats), cumulative since connect — kind == Srt.
    qint64 recvTotal = 0;    // pktRecvTotal
    qint64 retransTotal = 0; // pktRcvRetrans
    qint64 lossTotal = 0;    // pktRcvLossTotal (detected; retransmitted)
    qint64 dropTotal = 0;    // pktRcvDropTotal (too-late; finally unrecovered)
    // Generic liveness/throughput — kind == Rtmp.
    quint64 bytesTotal = 0;     // cumulative bytes received
    qint64 lastPacketAgeMs = 0; // ms since the last media packet, at sample time
    qint64 keyframeAgeMs = 0;   // ms since the last video keyframe, at sample time
    quint64 decodeFailures = 0; // cumulative frames the native decoder rejected
};

// Per-source link health -> the connection dot: Green=healthy, Amber=stressed, Red=losing content.
enum class SourceHealth { NA = 0, Green = 1, Amber = 2, Red = 3 };

// SRT grader (unchanged logic): Red on any unrecovered drop this window; else Amber
// if the retransmit rate exceeds amberRetransRate; else Green. Negative deltas (a
// reconnect reset the counters) clamp Green. Pure.
SourceHealth srtHealth(const IngestStats& prev, const IngestStats& cur, double amberRetransRate);

// RTMP grader (TCP has no loss): Red if stalled (>= kRtmpRedStallMs since last media)
// or decode is failing with no fresh bytes; Amber on a decode failure this window, a
// brief stall (>= kRtmpAmberStallMs), or a long keyframe gap (>= kRtmpAmberKeyframeMs);
// else Green. A counter reset (cur < prev) clamps Green. Pure.
constexpr int kRtmpRedStallMs = 3000;
constexpr int kRtmpAmberStallMs = 1000;
constexpr int kRtmpAmberKeyframeMs = 5000;
SourceHealth rtmpHealth(const IngestStats& prev, const IngestStats& cur);
```
Then update `IngestCallbacks::reportStats` (`:80`) to `std::function<void(const IngestStats&)> reportStats;` and `Q_DECLARE_METATYPE(SrtStats)` (`:102`) to `Q_DECLARE_METATYPE(IngestStats)`.

- [ ] **Step 5: `ingestsession.cpp`** — rename the types in `srtHealth` (`:19-30`: `SrtHealth srtHealth(const SrtStats& …` → `SourceHealth srtHealth(const IngestStats& …`, and `SrtHealth::Red/Amber/Green` → `SourceHealth::…`). Then add `rtmpHealth` directly below it:

```cpp
SourceHealth rtmpHealth(const IngestStats& prev, const IngestStats& cur) {
    if (cur.bytesTotal < prev.bytesTotal || cur.decodeFailures < prev.decodeFailures) {
        return SourceHealth::Green; // counters reset on reconnect
    }
    const bool bytesAdvanced = cur.bytesTotal > prev.bytesTotal;
    const bool decodeFailedThisWindow = cur.decodeFailures > prev.decodeFailures;
    if (cur.lastPacketAgeMs >= kRtmpRedStallMs) {
        return SourceHealth::Red;
    }
    if (decodeFailedThisWindow && !bytesAdvanced) {
        return SourceHealth::Red;
    }
    if (decodeFailedThisWindow || cur.lastPacketAgeMs >= kRtmpAmberStallMs ||
        cur.keyframeAgeMs >= kRtmpAmberKeyframeMs) {
        return SourceHealth::Amber;
    }
    return SourceHealth::Green;
}
```

- [ ] **Step 6: Propagate the rename across the engine + UI + harness.** Grep the repo for `SrtStats`, `SrtHealth`, `SrtStatsEntry`, `sourceHasSrtStats` and update every hit:
  - `nativesrtingestsession.cpp:193-198`: `SrtStats stats;` → `IngestStats stats;` and add `stats.kind = IngestStatsKind::Srt;` before filling `recvTotal/retransTotal/lossTotal/dropTotal`.
  - `streamworker.h:94` + any emit in `streamworker.cpp`: `statsUpdated(int, SrtStats)` → `statsUpdated(int, IngestStats)`.
  - `replaymanager.h:90` + the relay connect in `replaymanager.cpp`: `sourceStatsUpdated(int, SrtStats)` → `…IngestStats`.
  - `uimanager.h`: `onSourceStatsUpdated(int, SrtStats)` → `…IngestStats` (`:302`); `struct SrtStatsEntry{SrtStats last; …}` → rename to `IngestStatsEntry{IngestStats last; …}` and `std::vector<SrtStatsEntry> m_sourceStats` → `IngestStatsEntry` (`:377-382`); rename `Q_INVOKABLE bool sourceHasSrtStats(int)` → `sourceHasStats(int)` (`:217`).
  - `uimanager.cpp`: `int(SrtHealth::NA)` → `int(SourceHealth::NA)` (`:1012, :1036`) and `int(SrtHealth::Green)` → `int(SourceHealth::Green)` (`:1026`); rename `sourceHasSrtStats` def → `sourceHasStats` (`:1040`); change `SrtStatsEntry&`→`IngestStatsEntry&`. **Dispatch the grader by kind** in `onSourceStatsUpdated` (`:1027-1029`), replacing the `srtHealth(...)` call:
    ```cpp
        } else {
            e.health = int(stats.kind == IngestStatsKind::Rtmp
                               ? rtmpHealth(e.last, stats)
                               : srtHealth(e.last, stats, m_srtAmberPct));
        }
    ```
    **Branch the tooltip by kind** in `sourceStatsTooltip` (`:1050`), before the existing SRT string:
    ```cpp
        const IngestStats& s = m_sourceStats[sourceIndex].last;
        const QLocale loc;
        if (s.kind == IngestStatsKind::Rtmp) {
            return QStringLiteral("RTMP link\nreceived   %1 bytes\nkeyframe   %2 ms ago\ndecode err %3")
                .arg(loc.toString(qulonglong(s.bytesTotal)), loc.toString(qlonglong(s.keyframeAgeMs)),
                     loc.toString(qulonglong(s.decodeFailures)));
        }
    ```
    (keep the existing SRT `recv/retrans/loss/drop` block for the non-RTMP path.)
  - `Main.qml:1380`: `sourceHasSrtStats(streamRow.index)` → `sourceHasStats(streamRow.index)`.
  - `tests/unit/tst_srt_health.cpp`: replace `SrtStats`→`IngestStats`, `SrtHealth`→`SourceHealth` (the `snap()` helper + all 8 `QCOMPARE`s); add `s.kind = IngestStatsKind::Srt;` in `snap()`. Logic/values unchanged.
  - `tests/e2e/sync_harness.cpp:103,112-114,159-162`: `QHash<int,SrtStats>`→`QHash<int,IngestStats>`; lambda param `SrtStats stats`→`IngestStats stats`; **make the print kind-aware** (`:159-162`):
    ```cpp
                const IngestStats& s = latestStats.value(src);
                if (s.kind == IngestStatsKind::Rtmp) {
                    fprintf(stderr,
                            "stats src=%d kind=rtmp bytes=%llu lastpktage=%lld keyframeage=%lld "
                            "decodefail=%llu\n",
                            src, (unsigned long long) s.bytesTotal, (long long) s.lastPacketAgeMs,
                            (long long) s.keyframeAgeMs, (unsigned long long) s.decodeFailures);
                } else {
                    fprintf(stderr, "stats src=%d recv=%lld retrans=%lld loss=%lld drop=%lld\n", src,
                            (long long) s.recvTotal, (long long) s.retransTotal,
                            (long long) s.lossTotal, (long long) s.dropTotal);
                }
    ```

- [ ] **Step 7: Build + run, expect GREEN** — `cmake -S . -B build/ingest >/dev/null && cmake --build build/ingest` (whole tree, incl. app + harnesses). Then `( cd build/ingest && ctest -R "tst_rtmp_health|tst_srt_health" --output-on-failure )` — both pass (rtmp 7/7; srt 8/8 unchanged). Fix any missed `SrtStats`/`SrtHealth` reference the grep missed.

- [ ] **Step 8: Format changed lines + commit**
```bash
git add -A recorder_engine uimanager.h uimanager.cpp Main.qml tests/unit tests/e2e/sync_harness.cpp
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add -A recorder_engine uimanager.h uimanager.cpp Main.qml tests/unit tests/e2e/sync_harness.cpp
git commit -m "feat(ingest): generalize stats/health model (IngestStats + rtmpHealth)"
```

---

### Task 2: `NativeRtmpIngestSession` emits `IngestStats`

**Files:** `recorder_engine/ingest/nativertmpingestsession.h`, `recorder_engine/ingest/nativertmpingestsession.cpp`.

- [ ] **Step 1: Add members + helper decl** to `nativertmpingestsession.h`. After `int64_t m_lastPacketAtMs = -1;` (`:62`) add:
```cpp
    int64_t m_lastKeyframeAtMs = -1;
    int64_t m_lastStatsAtMs = -1;
    quint64 m_decodeFailures = 0;
```
and in the private methods section (near `:91`) add:
```cpp
    void maybeReportStats();
```

- [ ] **Step 2: Reset the new fields + byte counter in `open()`** (`nativertmpingestsession.cpp`, alongside the other resets near `:236`). Add:
```cpp
    m_lastKeyframeAtMs = -1;
    m_lastStatsAtMs = -1;
    m_decodeFailures = 0;
    m_receivedChunkBytes = 0;
    m_nextAcknowledgementAt = 0;
    m_acknowledgementWindowSize = 0;
```

- [ ] **Step 3: Implement `maybeReportStats()`** — add near `processMessage` in `nativertmpingestsession.cpp`:
```cpp
void NativeRtmpIngestSession::maybeReportStats() {
    if (!m_callbacks.reportStats || m_openedAtMs < 0) {
        return; // not yet running (handshake) or no consumer
    }
    const int64_t now = m_monotonic.elapsed();
    if (m_lastStatsAtMs >= 0 && now - m_lastStatsAtMs < 1000) {
        return; // ~1/sec
    }
    m_lastStatsAtMs = now;
    IngestStats stats;
    stats.kind = IngestStatsKind::Rtmp;
    stats.bytesTotal = m_receivedChunkBytes;
    stats.lastPacketAgeMs = m_lastPacketAtMs >= 0 ? now - m_lastPacketAtMs : 0;
    stats.keyframeAgeMs = m_lastKeyframeAtMs >= 0   ? now - m_lastKeyframeAtMs
                          : m_openedAtMs >= 0       ? now - m_openedAtMs
                                                    : 0;
    stats.decodeFailures = m_decodeFailures;
    m_callbacks.reportStats(stats);
}
```

- [ ] **Step 4: Sample in the run loop + the idle branch.**
  - In `run()` after `processMessage(message);` (`:297`), add `maybeReportStats();` (before the `m_reconnectRequested` check).
  - In `readMessage()`'s idle-timeout `continue` path: just before the `continue;` at `:521` (after the stall check), add `maybeReportStats();` so a connected-but-quiet stream still updates (its `lastPacketAgeMs` climbs → Amber/Red).

- [ ] **Step 5: Track keyframes + decode failures in `processVideoMessage`.**
  - At the top of `processVideoMessage` (after the `markUnsupported` lambda, `:733`), compute the FLV frame type (bits 4-6 of byte 0 = 1 for a keyframe, in both legacy and enhanced headers):
    ```cpp
    const bool isKeyframe = !payload.isEmpty() && ((uchar(payload[0]) >> 4) & 0x07) == 1;
    ```
  - In the CodedFrames decode block, stamp the keyframe time when we actually feed a keyframe to the decoder — right before `const bool decoded = m_videoDecoder->decode(` (`:860`):
    ```cpp
    if (isKeyframe) {
        m_lastKeyframeAtMs = m_monotonic.elapsed();
    }
    ```
  - Count decode failures — in the `if (!decoded && !error.isEmpty())` block (`:874`), add as the first line inside:
    ```cpp
        ++m_decodeFailures;
    ```

- [ ] **Step 6: Build** — `cmake --build build/ingest` clean. (No new unit test here; the e2e gate in Task 4 + the unit grader from Task 1 cover it. The RTMP session emitting stats is proven end-to-end by Task 4.)

- [ ] **Step 7: Format + commit**
```bash
git add recorder_engine/ingest/nativertmpingestsession.h recorder_engine/ingest/nativertmpingestsession.cpp
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add recorder_engine/ingest/nativertmpingestsession.h recorder_engine/ingest/nativertmpingestsession.cpp
git commit -m "feat(rtmp): emit IngestStats (bytes/last-packet-age/keyframe-age/decode-failures)"
```

---

### Task 3: `NativeRtmpIngestSession` — single shared A/V anchor (audio follows video)

Replace the two independently-re-anchoring anchor pairs with one shared anchor (the AUD-4 model): video owns re-anchoring; audio follows and flushes its decoder on a jump but never moves the anchor.

**Files:** `recorder_engine/ingest/nativertmpingestsession.h`, `recorder_engine/ingest/nativertmpingestsession.cpp`, `tests/unit/tst_ingestbackendselector.cpp` (the existing RTMP anchor tests, `:157-244`).

- [ ] **Step 1: Update the existing anchor unit tests first (TDD).** In `tests/unit/tst_ingestbackendselector.cpp` (the `OLR_NATIVE_RTMP_AVAILABLE`-gated RTMP anchor tests around `:157-244`), update expectations to the shared-anchor model and add a no-drift case. The key new assertion: after video establishes the anchor and audio binds to it, a **video** re-anchor (a forward DTS jump) keeps audio mapping against the *same* shared anchor — audio does **not** independently re-anchor, so a co-timed audio packet stays locked to video. Concretely, exercising the private `sourcePtsMsForVideo`/`sourcePtsMsForAudio` via the `TestIngestBackendSelector` friend with a fixed `recordingClockMs`:
  - first video dts=ptsMs=1000 with clock=10000 → returns 10000; a co-timed audio pts=1000 → returns 10000 (shares the anchor, not its own).
  - audio pts=1100 → 10100 (anchor unchanged).
  - a video forward jump dts=ptsMs=9000 (Δ=8000 > kForwardJumpMs) → re-anchors: with clock=20000 → returns 20000; the *next* audio pts=9000 → 20000 (follows the new shared anchor — no drift).
  Set `session.m_callbacks.recordingClockMs` (mutable via the friend) between calls to drive the fixed clock. (Read the existing test block first; preserve its setup/helpers and adapt the expected numbers to the new single-anchor math.)

- [ ] **Step 2: Verify RED** — `cmake --build build/ingest --target tst_ingestbackendselector && ( cd build/ingest && ctest -R tst_ingestbackendselector --output-on-failure )` → the updated anchor assertions fail against the old two-anchor code.

- [ ] **Step 3: Collapse the anchor fields** in `nativertmpingestsession.h`. Replace (`:56-61`):
```cpp
    int64_t m_firstDtsMs = -1;
    int64_t m_prevDtsMs = -1;
    int64_t m_anchorStreamTimeMs = -1;
    int64_t m_firstAudioPtsMs = -1;
    int64_t m_prevAudioPtsMs = -1;
    int64_t m_audioAnchorStreamTimeMs = -1;
```
with (one shared media zero + one shared recording zero; per-stream prev for jump detection):
```cpp
    int64_t m_anchorMediaMs = -1;     // shared A/V media-timestamp zero (FLV ms)
    int64_t m_anchorStreamTimeMs = -1; // shared recording-clock zero
    int64_t m_prevDtsMs = -1;
    int64_t m_prevAudioPtsMs = -1;
```

- [ ] **Step 4: Reset in `open()`** — update the resets (`:230-235`) to the new fields:
```cpp
    m_anchorMediaMs = -1;
    m_anchorStreamTimeMs = -1;
    m_prevDtsMs = -1;
    m_prevAudioPtsMs = -1;
```
(remove the now-deleted `m_firstDtsMs`/`m_firstAudioPtsMs`/`m_audioAnchorStreamTimeMs` reset lines.)

- [ ] **Step 5: Rewrite the two anchor functions** (`nativertmpingestsession.cpp:999-1054`):
```cpp
int64_t NativeRtmpIngestSession::sourcePtsMsForVideo(qint64 dtsMs, qint64 ptsMs) {
    bool needAnchor = m_anchorMediaMs < 0;
    if (!needAnchor && m_prevDtsMs >= 0) {
        const int64_t deltaMs = dtsMs - m_prevDtsMs;
        if (deltaMs > kForwardJumpMs || deltaMs < kBackwardToleranceMs) {
            log(QStringLiteral("Native RTMP DTS discontinuity (%1 ms jump). Re-anchoring.")
                    .arg(deltaMs));
            needAnchor = true;
        }
    }
    if (needAnchor) {
        m_anchorMediaMs = dtsMs;
        m_anchorStreamTimeMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    }
    m_prevDtsMs = dtsMs;
    if (m_anchorStreamTimeMs < 0) {
        return -1;
    }
    return m_anchorStreamTimeMs + (ptsMs - m_anchorMediaMs);
}

int64_t NativeRtmpIngestSession::sourcePtsMsForAudio(qint64 ptsMs) {
    if (m_anchorMediaMs < 0) {
        // Audio arrived before any video: establish the shared anchor here. Video
        // takes over re-anchoring once it arrives.
        m_anchorMediaMs = ptsMs;
        m_anchorStreamTimeMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    } else if (m_prevAudioPtsMs >= 0) {
        const int64_t deltaMs = ptsMs - m_prevAudioPtsMs;
        if (deltaMs > kForwardJumpMs || deltaMs < kBackwardToleranceMs) {
            // Audio discontinuity: flush the decoder but DO NOT move the shared anchor
            // (video owns re-anchoring) — keeps A/V locked. This is the AUD-4 model.
            log(QStringLiteral("Native RTMP audio PTS discontinuity (%1 ms jump). Flushing decoder.")
                    .arg(deltaMs));
            if (m_audioDecoder) {
                m_audioDecoder->reset();
            }
        }
    }
    m_prevAudioPtsMs = ptsMs;
    if (m_anchorStreamTimeMs < 0) {
        return -1;
    }
    return m_anchorStreamTimeMs + (ptsMs - m_anchorMediaMs);
}
```

- [ ] **Step 6: Build + run, expect GREEN** — `cmake --build build/ingest --target tst_ingestbackendselector && ( cd build/ingest && ctest -R tst_ingestbackendselector --output-on-failure )` passes (the shared-anchor assertions now hold). Then full `cmake --build build/ingest` clean.

- [ ] **Step 7: Format + commit**
```bash
git add recorder_engine/ingest/nativertmpingestsession.h recorder_engine/ingest/nativertmpingestsession.cpp tests/unit/tst_ingestbackendselector.cpp
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add recorder_engine/ingest/nativertmpingestsession.h recorder_engine/ingest/nativertmpingestsession.cpp tests/unit/tst_ingestbackendselector.cpp
git commit -m "fix(rtmp): single shared A/V anchor (audio follows video, no re-anchor drift)"
```

---

### Task 4: E2e gate — `e2e_native_rtmp_ui_stats`

Prove the RTMP stats data path end-to-end: the session's `IngestStats` reaches `ReplayManager::sourceStatsUpdated`, which `sync_harness --report-stats` prints. Mirrors `e2e_native_srt_ui_stats`.

**Files:** `tests/e2e/run_rtmp_ui_stats.sh` (new), `tests/e2e/CMakeLists.txt`.

- [ ] **Step 1: READ** `tests/e2e/run_rtmp_smoke.sh` and `tests/e2e/rtmp_lib.sh` to learn the fixture API (`rtmp_server`, `rtmp_generate_tone_flv`, the `rtmp://` caller URL, the `"Native RTMP connected"` assertion, the `SKIP_RETURN_CODE 77` convention). Then create `tests/e2e/run_rtmp_ui_stats.sh` modeled on `run_srt_ui_stats.sh` + `run_rtmp_smoke.sh`:
  - Source `rtmp_lib.sh`; start the Python RTMP fixture serving an ffmpeg-generated H.264/AAC tone FLV; run `"$HARNESS" --url "rtmp://127.0.0.1:$PORT/$APP/$STREAM" --outdir "$WORKDIR" --name uistats --seconds "$SECS" --fps 30 --report-stats >out 2>err`.
  - Parse the RTMP stats line with a helper:
    ```bash
    hstat() {  # $1=err file  $2=field   (line: "stats src=0 kind=rtmp bytes=.. lastpktage=.. keyframeage=.. decodefail=..")
        grep '^stats src=0 kind=rtmp ' "$1" | tail -1 | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2; exit}'
    }
    ```
  - Assert: a `kind=rtmp` stats line exists; `bytes > 0` (data flowed through the RM signal); `decodefail == 0` (the fixture is decodable). Print a `PASS:`/`FAIL:` line and `exit 0/1`. SKIP (exit 0) if `ffmpeg`/`python3` are missing, matching the other scripts.
  - `chmod +x tests/e2e/run_rtmp_ui_stats.sh`.

- [ ] **Step 2: Register the ctest** — in `tests/e2e/CMakeLists.txt`, inside the `if(APPLE)` native-rtmp block (near `e2e_native_rtmp_reconnect`, ~`:271`), add:
```cmake
    add_test(NAME e2e_native_rtmp_ui_stats
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_ui_stats.sh" "$<TARGET_FILE:sync_harness>" 23760)
    set_tests_properties(e2e_native_rtmp_ui_stats PROPERTIES
        LABELS "native-rtmp"
        TIMEOUT 120
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE)
```
(match the surrounding entries' property style; pick a base port not already used in the block.)

- [ ] **Step 3: Reconfigure + run** — `cmake -S . -B build/ingest >/dev/null && cmake --build build/ingest --target sync_harness && ( cd build/ingest && ctest -R e2e_native_rtmp_ui_stats --output-on-failure )` → PASS, printing a `kind=rtmp bytes=<positive>` line. If it reports no stats line, the engine→RM path isn't carrying RTMP stats — investigate (do NOT weaken the assert).

- [ ] **Step 4: Commit**
```bash
git add tests/e2e/run_rtmp_ui_stats.sh tests/e2e/CMakeLists.txt
git commit -m "test(rtmp): e2e_native_rtmp_ui_stats — prove the RTMP stats data path to the UI signal"
```

---

### Task 5: Docs

**Files:** `tests/e2e/SRT_README.md` (or the most relevant ingest doc — READ to confirm).

- [ ] **Step 1:** Append a short "Native RTMP parity (PR A)" note: the generalized `IngestStats`/`SourceHealth` model with per-backend graders (`srtHealth` unchanged + `rtmpHealth` on liveness/throughput/keyframe-age/decode-failures); RTMP now emits stats ~1/s; the single shared A/V anchor (audio follows video, AUD-4 model); the `e2e_native_rtmp_ui_stats` gate; and that ffmpeg removal + SRT-default + Windows AAC are PR B. Commit:
```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(rtmp): document native RTMP parity (PR A) — health model + shared anchor"
```

---

## After all tasks
- `( cd build/ingest && ctest -L unit --output-on-failure )` — incl. `tst_rtmp_health`, `tst_srt_health`, `tst_ingestbackendselector`.
- `( cd build/ingest && ctest -L native-rtmp --output-on-failure )` — incl. `e2e_native_rtmp_ui_stats`; smoke/hevc/reconnect/unsupported stay green (anchor + stats changes don't regress).
- `( cd build/ingest && ctest -L native-apple-ingest --output-on-failure )` — SRT path behavior-identical.
- `grep -rn "SrtStats\|SrtHealth\|sourceHasSrtStats" recorder_engine uimanager.* Main.qml tests` — confirm no stray old names remain (only `IngestStats`/`SourceHealth`/`sourceHasStats`).
- Final code review over the branch (focus: SRT behavior byte-identity from the rename; `rtmpHealth` thresholds; the shared-anchor math + no-drift; kind-dispatch correctness).
- Rebase onto latest `origin/main`; push (`SKIP_IOS_BUILD=1`); open PR with base `main`. (PR B — ffmpeg removal — stacks on this branch afterward.)

## Self-review notes
- **Spec coverage:** generalized health model (Task 1) ✓; RTMP emits stats (Task 2) ✓; single shared anchor (Task 3) ✓; `rtmpHealth` unit + `e2e_native_rtmp_ui_stats` + SRT-unchanged (Tasks 1,4) ✓; docs (Task 5) ✓. Codec/reconnect "parity verification" is covered by the `native-rtmp` regression staying green (no code needed — confirmed in the audit).
- **Types are consistent across tasks:** `IngestStats`/`IngestStatsKind`/`SourceHealth`/`rtmpHealth`/`m_anchorMediaMs`/`maybeReportStats` are used identically in every task that references them.
- **YAGNI:** no RTMP loss/retrans synthesis (TCP can't); no new flash/beep RTMP fixture (anchor covered by the no-drift unit test + native-rtmp gates).
