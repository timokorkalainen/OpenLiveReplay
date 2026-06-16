# JIT-5: SRT loss telemetry on the connection-status UI — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface per-source SRT link health (loss / ARQ retransmit / unrecovered drops) on the existing per-source connection dot (#24) — a live green/amber/red grade plus a hover tooltip with cumulative counters.

**Architecture:** A periodic-telemetry path mirroring the existing connection-status path: the native SRT ingest (which already samples `srt_bstats` ~1/sec) pushes an `SrtStats` snapshot through a new `IngestCallbacks::reportStats` callback → `StreamWorker::statsUpdated` (queued) → `ReplayManager::sourceStatsUpdated` (queued) → `UIManager`, which keeps the previous snapshot per source, derives a color from the windowed delta via a pure `srtHealth()` function, and exposes it to QML through a version property + invokables. Native-SRT-only by construction (only that ingest calls the callback).

**Tech Stack:** C++17, Qt6 (Core/QML), libsrt (native ingest), Qt Test (unit), bash + `lossy_udp_relay.py` + `srt-live-transmit` (e2e), CMake/Ninja/CTest.

**Spec:** `docs/superpowers/specs/2026-06-16-srt-loss-telemetry-ui-jit5-design.md`

**Base branch:** `feat/srt-jit5-loss-ui` (already created off `feat/srt-loss-2cb`, the PR #40 base that added the `srt_bstats` sampling). Worktree: `/tmp/olr-srt2cb`. Build dir: `build/srt` (configured with `-DOLR_FFMPEG_SRT_PREFIX`). **Local-only; do not push** unless told.

**Deviation from spec (deliberate):** the spec listed a new `recorder_engine/srt_health.{h,cpp}`. Instead, `SrtStats`, the `SrtHealth` enum, and the pure `srtHealth()` function live in the **existing** `recorder_engine/ingest/ingestsession.{h,cpp}` — that file already defines `IngestCallbacks` and the free function `selectIngestBackend()`, and is already compiled into both `olr_test_core` and the app, so no new CMake wiring or app/test target edits are needed. DRY.

**Per-source indexing:** every layer keys on the stable `sourceIndex` (source identity, not a view slot), exactly like `sourceConnectionChanged`.

---

### Task 1: `SrtStats` type + pure `srtHealth()` classifier + unit test

**Files:**
- Modify: `recorder_engine/ingest/ingestsession.h` (add struct, enum, callback field, function decl, metatype)
- Modify: `recorder_engine/ingest/ingestsession.cpp` (implement `srtHealth`)
- Create: `tests/unit/tst_srt_health.cpp`
- Modify: `tests/unit/CMakeLists.txt` (register the test)

- [ ] **Step 1: Write the failing test** — `tests/unit/tst_srt_health.cpp`

```cpp
#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"

class TestSrtHealth : public QObject {
    Q_OBJECT
private slots:
    void quietLinkIsGreen();
    void unrecoveredDropIsRed();
    void dropBeatsHighRetrans();
    void elevatedRetransIsAmber();
    void lightRetransIsGreen();
    void thresholdIsStrict();
    void counterResetIsGreen();
};

// helper: build a cumulative snapshot
static SrtStats snap(qint64 recv, qint64 retrans, qint64 loss, qint64 drop) {
    SrtStats s;
    s.recvTotal = recv; s.retransTotal = retrans; s.lossTotal = loss; s.dropTotal = drop;
    return s;
}

void TestSrtHealth::quietLinkIsGreen() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 0, 0, 0);  // +1000 recv, no retrans/drop
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

void TestSrtHealth::unrecoveredDropIsRed() {
    const SrtStats a = snap(1000, 10, 20, 0);
    const SrtStats b = snap(2000, 15, 25, 3);  // +3 unrecovered drops this window
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Red);
}

void TestSrtHealth::dropBeatsHighRetrans() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 900, 900, 1);  // huge retrans AND a drop -> Red
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Red);
}

void TestSrtHealth::elevatedRetransIsAmber() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 30, 30, 0);  // 30/1000 = 3% > 2%, no drop
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Amber);
}

void TestSrtHealth::lightRetransIsGreen() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 10, 10, 0);  // 10/1000 = 1% < 2%
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

void TestSrtHealth::thresholdIsStrict() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 20, 20, 0);  // exactly 2% -> NOT amber (strictly >)
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

void TestSrtHealth::counterResetIsGreen() {
    // After a reconnect the socket's cumulative counters restart from 0, so the
    // "current" snapshot is smaller than the previous -> negative deltas. Must clamp
    // to Green, never misread a reset as recovery-success-with-loss.
    const SrtStats a = snap(5000, 400, 800, 9);
    const SrtStats b = snap(100, 2, 2, 0);
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

QTEST_GUILESS_MAIN(TestSrtHealth)
#include "tst_srt_health.moc"
```

- [ ] **Step 2: Register the test** — append to `tests/unit/CMakeLists.txt` after line 29 (`olr_add_unit_test(tst_streamdeckmappingstore olr_test_core)`):

```cmake
olr_add_unit_test(tst_srt_health olr_test_core)
```

- [ ] **Step 3: Run it to verify it FAILS to build** (types/function not defined)

Run: `cmake --build build/srt --target tst_srt_health`
Expected: compile error — `SrtStats` / `SrtHealth` / `srtHealth` not declared.

- [ ] **Step 4: Add the type, enum, callback, and declaration** to `recorder_engine/ingest/ingestsession.h`.

Add `#include <QMetaType>` to the include block (after `#include <QString>`, line 6).

Insert this **before** `struct IngestCallbacks {` (line 36):

```cpp
// Cumulative SRT receiver counters sampled from srt_bstats (native ingest only).
// Snapshots are pushed to the UI ~1/sec via IngestCallbacks::reportStats so the
// connection-status dot can grade link health. See srtHealth() below.
struct SrtStats {
    qint64 recvTotal    = 0;   // pktRecvTotal
    qint64 retransTotal = 0;   // pktRcvRetrans   (received retransmissions; healthy when small)
    qint64 lossTotal    = 0;   // pktRcvLossTotal (DETECTED loss; retransmitted)
    qint64 dropTotal    = 0;   // pktRcvDropTotal (too-late-to-play; finally UNRECOVERED)
};

// Per-source link health derived from the delta between two SrtStats snapshots.
// Maps to the connection dot: Green=healthy, Amber=link stressed, Red=losing content.
enum class SrtHealth { NA = 0, Green = 1, Amber = 2, Red = 3 };

// Classify the most-recent sampling window (cur - prev). Red if any unrecovered
// drops occurred; else Amber if the retransmit rate exceeds amberRetransRate
// (fraction of received packets, e.g. 0.02); else Green. Negative deltas (a
// reconnect reset the socket counters) clamp to Green. Pure — no Qt/UI deps.
SrtHealth srtHealth(const SrtStats& prev, const SrtStats& cur, double amberRetransRate);
```

Add inside `struct IngestCallbacks` (after `std::function<void(bool)> setConnected;`, line 39):

```cpp
    std::function<void(const SrtStats&)> reportStats;
```

Add **after** the `IngestSession` class / `selectIngestBackend` declaration, just before `#endif` (line 56):

```cpp
Q_DECLARE_METATYPE(SrtStats)
```

- [ ] **Step 5: Implement `srtHealth()`** — append to `recorder_engine/ingest/ingestsession.cpp`:

```cpp
SrtHealth srtHealth(const SrtStats& prev, const SrtStats& cur, double amberRetransRate) {
    const qint64 dDrop = cur.dropTotal - prev.dropTotal;
    const qint64 dRetrans = cur.retransTotal - prev.retransTotal;
    const qint64 dRecv = cur.recvTotal - prev.recvTotal;
    if (dDrop > 0) {
        return SrtHealth::Red;
    }
    if (dRecv > 0 && double(dRetrans) / double(dRecv) > amberRetransRate) {
        return SrtHealth::Amber;
    }
    return SrtHealth::Green;
}
```

- [ ] **Step 6: Run the test to verify it PASSES**

Run: `cmake --build build/srt --target tst_srt_health && ( cd build/srt && ctest -R tst_srt_health --output-on-failure )`
Expected: PASS — 7 test functions, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/ingestsession.h recorder_engine/ingest/ingestsession.cpp \
        tests/unit/tst_srt_health.cpp tests/unit/CMakeLists.txt
git commit -m "feat(srt): SrtStats type + pure srtHealth() classifier (unit-tested)"
```

---

### Task 2: Native ingest pushes stats through the new callback

**Files:**
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp:189-198` (the `srt_bstats` sampling block)

This is wiring with no isolated unit test (it needs a live SRT socket); the `e2e_native_srt_ui_stats` gate in Task 6 proves it end-to-end. The check here is that it builds.

- [ ] **Step 1: Invoke `reportStats` at each successful sample.** In `nativesrtingestsession.cpp`, replace the `if (srt_bstats(...) == 0) { ... }` block (lines 191-196) with:

```cpp
                if (srt_bstats(m_socket, &perf, 0) == 0) {
                    m_statRetrans = perf.pktRcvRetrans;
                    m_statLossTotal = perf.pktRcvLossTotal;
                    m_statDropTotal = perf.pktRcvDropTotal;
                    m_statRecvTotal = perf.pktRecvTotal;
                    if (m_callbacks.reportStats) {
                        SrtStats stats;
                        stats.recvTotal = perf.pktRecvTotal;
                        stats.retransTotal = perf.pktRcvRetrans;
                        stats.lossTotal = perf.pktRcvLossTotal;
                        stats.dropTotal = perf.pktRcvDropTotal;
                        m_callbacks.reportStats(stats);
                    }
                }
```

(`SrtStats` is visible via `ingestsession.h`, already included through `nativesrtingestsession.h`.)

- [ ] **Step 2: Verify it builds**

Run: `cmake --build build/srt --target sync_harness`
Expected: links clean (the native ingest is compiled into the engine lib used by the harness on APPLE).

- [ ] **Step 3: Commit**

```bash
git add recorder_engine/ingest/nativesrtingestsession.cpp
git commit -m "feat(srt): native ingest pushes srt_bstats snapshots via reportStats"
```

---

### Task 3: Relay stats up through StreamWorker → ReplayManager

**Files:**
- Modify: `recorder_engine/streamworker.h` (include, signal, metatype registration)
- Modify: `recorder_engine/streamworker.cpp` (register metatype in ctor; wire the callback)
- Modify: `recorder_engine/replaymanager.h` (include + relay signal)
- Modify: `recorder_engine/replaymanager.cpp` (connect worker→manager)

Wiring; proven by Task 6. Check: builds.

- [ ] **Step 1: StreamWorker header.** In `recorder_engine/streamworker.h`:

Add after the other includes (after `#include "muxer.h"`, line 19):

```cpp
#include "ingest/ingestsession.h"
```

Add to the `signals:` block, after `void connectionChanged(int sourceIndex, bool connected);` (line 83):

```cpp
    // Emitted ~1/sec from the capture thread with the source's latest cumulative
    // SRT receiver stats (native SRT ingest only). Cross-thread: relayed to the UI
    // through ReplayManager with a queued connection, like connectionChanged.
    void statsUpdated(int sourceIndex, SrtStats stats);
```

- [ ] **Step 2: StreamWorker ctor — register the metatype** (idempotent; needed for the queued signal to carry `SrtStats`). In `recorder_engine/streamworker.cpp`, add as the first line of the constructor body (after the `{` on line 16):

```cpp
    qRegisterMetaType<SrtStats>("SrtStats");
```

- [ ] **Step 3: Wire the callback** in `captureLoop()`. In `recorder_engine/streamworker.cpp`, after the `callbacks.setConnected = ...` lambda (ends line 281), add:

```cpp
        callbacks.reportStats = [this](const SrtStats& stats) {
            emit statsUpdated(m_sourceIndex, stats);
        };
```

- [ ] **Step 4: ReplayManager header.** In `recorder_engine/replaymanager.h`:

Ensure `SrtStats` is visible — add near the top includes (idempotent if already pulled in):

```cpp
#include "ingest/ingestsession.h"
```

Add to `signals:`, after `void sourceConnectionChanged(int sourceIndex, bool connected);` (line 86):

```cpp
    // Relayed from each StreamWorker ~1/sec with that source's latest SRT stats.
    void sourceStatsUpdated(int sourceIndex, SrtStats stats);
```

- [ ] **Step 5: ReplayManager — connect the relay.** In `recorder_engine/replaymanager.cpp`, after the `connectionChanged`→`sourceConnectionChanged` connect (lines 237-238), add:

```cpp
        connect(worker, &StreamWorker::statsUpdated, this,
                &ReplayManager::sourceStatsUpdated, Qt::QueuedConnection);
```

- [ ] **Step 6: Verify it builds**

Run: `cmake --build build/srt --target sync_harness`
Expected: links clean.

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp \
        recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp
git commit -m "feat(srt): relay per-source SrtStats StreamWorker -> ReplayManager (queued)"
```

---

### Task 4: UIManager — snapshot store, windowed color, getters, property

**Files:**
- Modify: `uimanager.h` (include, property, getter, invokables, signal, slot, members)
- Modify: `uimanager.cpp` (ctor connect + env threshold; slot; getters; resets; reconnect re-baseline)

Engine-side UI logic. The pure classifier is already unit-tested (Task 1); the delta/baseline/reconnect bookkeeping is proven by Task 6's gate (data path) and the manual UI check (Task 7).

- [ ] **Step 1: uimanager.h — include + property + members.**

Add near the top includes (where other engine headers are included):

```cpp
#include "recorder_engine/ingest/ingestsession.h"
```

Add the property, after the `sourceConnectionVersion` property (lines 71-72):

```cpp
    // Bumped on every per-source SRT stats update so QML re-evaluates the dot
    // color (sourceLinkHealth) and tooltip (sourceStatsTooltip) bindings.
    Q_PROPERTY(int sourceStatsVersion READ sourceStatsVersion NOTIFY sourceStatsChanged)
```

Add the getter, after `int sourceConnectionVersion() const { ... }` (line 128):

```cpp
    int sourceStatsVersion() const { return m_sourceStatsVersion; }
```

Add the invokables, after `Q_INVOKABLE bool isSourceConnected(int sourceIndex) const;` (line 199):

```cpp
    // SRT link health for the connection dot. 0=N/A (no SRT stats), 1=green,
    // 2=amber (stressed), 3=red (recent unrecovered drops).
    Q_INVOKABLE int sourceLinkHealth(int sourceIndex) const;
    // True once this source has produced at least one SRT stats snapshot
    // (native SRT only); false for RTMP/UDP/ffmpeg-SRT sources.
    Q_INVOKABLE bool sourceHasSrtStats(int sourceIndex) const;
    // Preformatted multi-line cumulative figures for the dot's hover tooltip.
    Q_INVOKABLE QString sourceStatsTooltip(int sourceIndex) const;
```

Add the signal, after `void sourceConnectionChanged();` (line 252):

```cpp
    void sourceStatsChanged();
```

Add the slot, after `void onSourceConnectionChanged(int sourceIndex, bool connected);` (line 275):

```cpp
    // Receives ReplayManager::sourceStatsUpdated on the main thread.
    void onSourceStatsUpdated(int sourceIndex, SrtStats stats);
```

Add the members + helper, after the connection-state members (after `int m_sourceTrimVersion = 0;`, line 345) — note `resetSourceConnection()` is declared at line 346, so place these just above it:

```cpp
    // Per-source SRT link-health state, keyed by sourceIndex (parallel to
    // m_sourceConnected). last = most recent snapshot (also shown in the tooltip);
    // seen = false until the first snapshot since (re)connect, so the next snapshot
    // re-baselines after a counter reset; health = cached SrtHealth (0=N/A..3=red).
    struct SrtStatsEntry {
        SrtStats last;
        bool seen = false;
        int health = 0;
    };
    std::vector<SrtStatsEntry> m_sourceStats;
    int m_sourceStatsVersion = 0;
    double m_srtAmberPct = 0.02;   // retransmit-rate threshold for Amber
    void resetSourceStats(int count);
```

- [ ] **Step 2: uimanager.cpp — connect the relay + read the env threshold.** In the constructor, after the `sourceConnectionChanged`→`onSourceConnectionChanged` connect (lines 70-71), add:

```cpp
    connect(m_replayManager, &ReplayManager::sourceStatsUpdated, this,
            &UIManager::onSourceStatsUpdated, Qt::QueuedConnection);
    {
        // Amber lights when the windowed retransmit rate exceeds this fraction of
        // received packets. ARQ retransmits routinely on healthy lossy links, so
        // a presence test would be permanently amber; 2% is an early-stress warning.
        bool ok = false;
        const double pct = qEnvironmentVariable("OLR_SRT_HEALTH_AMBER_PCT").toDouble(&ok);
        if (ok && pct > 0.0) m_srtAmberPct = pct;
    }
```

- [ ] **Step 3: uimanager.cpp — the slot.** Add after `onSourceConnectionChanged` (ends line 980):

```cpp
void UIManager::onSourceStatsUpdated(int sourceIndex, SrtStats stats) {
    if (sourceIndex < 0) return;
    if (int(m_sourceStats.size()) <= sourceIndex)
        m_sourceStats.resize(sourceIndex + 1);
    SrtStatsEntry& e = m_sourceStats[sourceIndex];
    if (!e.seen) {
        // First snapshot since (re)connect: establish the baseline, render Green.
        e.seen = true;
        e.health = int(SrtHealth::Green);
    } else {
        e.health = int(srtHealth(e.last, stats, m_srtAmberPct));
    }
    e.last = stats;
    m_sourceStatsVersion++;
    emit sourceStatsChanged();
}
```

- [ ] **Step 4: uimanager.cpp — re-baseline on reconnect.** A reconnect recreates the SRT socket, restarting cumulative counters from 0. Mark the source unseen on a connect transition so the next snapshot re-baselines instead of computing a negative delta. In `onSourceConnectionChanged` (line 972), insert right after `if (sourceIndex < 0) return;` (line 973):

```cpp
    if (connected) {
        // Re-baseline SRT stats: counters restart from 0 on the new socket.
        if (sourceIndex < int(m_sourceStats.size())) {
            m_sourceStats[sourceIndex].seen = false;
            m_sourceStats[sourceIndex].health = int(SrtHealth::NA);
        }
    }
```

- [ ] **Step 5: uimanager.cpp — getters.** Add after the slot (Step 3):

```cpp
int UIManager::sourceLinkHealth(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= int(m_sourceStats.size())) return int(SrtHealth::NA);
    return m_sourceStats[sourceIndex].health;
}

bool UIManager::sourceHasSrtStats(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= int(m_sourceStats.size())) return false;
    return m_sourceStats[sourceIndex].seen;
}

QString UIManager::sourceStatsTooltip(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= int(m_sourceStats.size())
        || !m_sourceStats[sourceIndex].seen) {
        return QString();
    }
    const SrtStats& s = m_sourceStats[sourceIndex].last;
    const QLocale loc;
    QString pct = QStringLiteral("0.0");
    if (s.recvTotal > 0)
        pct = QString::number(100.0 * double(s.retransTotal) / double(s.recvTotal), 'f', 1);
    return QStringLiteral("SRT link\nrecv      %1\nretrans   %2  (%3%)\nloss det  %4\ndropped   %5")
        .arg(loc.toString(qlonglong(s.recvTotal)),
             loc.toString(qlonglong(s.retransTotal)),
             pct,
             loc.toString(qlonglong(s.lossTotal)),
             loc.toString(qlonglong(s.dropTotal)));
}

void UIManager::resetSourceStats(int count) {
    m_sourceStats.assign(count < 0 ? 0 : count, SrtStatsEntry{});
    m_sourceStatsVersion++;
    emit sourceStatsChanged();
}
```

(`QLocale` is available via Qt Core; add `#include <QLocale>` at the top of `uimanager.cpp` if not already present.)

- [ ] **Step 6: uimanager.cpp — reset on start/stop.** Mirror the connection-state reset. In `startRecording()`, right after the `m_sourceConnected = QList<bool>(... , false);` reset (lines 1413-1415), add:

```cpp
    resetSourceStats(m_replayManager->getSourceUrls().size());
```

In `resetSourceConnection()` (line 900), after `m_sourceConnected.fill(false);` (line 902), add:

```cpp
    resetSourceStats(int(m_sourceConnected.size()));
```

- [ ] **Step 7: Verify it builds**

Run: `cmake --build build/srt`
Expected: the whole build links (UIManager is part of the app; if the app target isn't in `build/srt`, build the broadest available target — at minimum `cmake --build build/srt --target olr_test_engine` must pass; UIManager itself compiles in the app build).

> Note for the implementer: `uimanager.{h,cpp}` are app-level (not in the test libs). If `build/srt` does not build the app, configure/build the app once to compile-check this task: `cmake --build build/srt --target OpenLiveReplay` (or the project's app target name). If no app target exists in this build dir, a syntax/compile check via the app build dir used for development is acceptable — but it MUST be compiled before commit.

- [ ] **Step 8: Commit**

```bash
git add uimanager.h uimanager.cpp
git commit -m "feat(srt): UIManager surfaces per-source SRT link health to QML"
```

---

### Task 5: `sync_harness --report-stats` (headless data-path readout)

**Files:**
- Modify: `tests/e2e/sync_harness.cpp`

- [ ] **Step 1: Parse the flag.** In `tests/e2e/sync_harness.cpp`, after `const bool reportConnectionEvents = ...;` (line 58), add:

```cpp
    const bool reportStats = args.contains(QStringLiteral("--report-stats"));
```

- [ ] **Step 2: Add includes + capture.** Add `#include "recorder_engine/ingest/ingestsession.h"` near the existing includes (after `#include "recorder_engine/replaymanager.h"`, line 26). Then, just after the `connectedSources`/`connEvents` captures are declared and before `connTimer.start();` (around line 102), add a per-source latest-stats map:

```cpp
    QHash<int, SrtStats> latestStats;
```

Wire it after the `sourceConnectionChanged` connect block (after line 107):

```cpp
    QObject::connect(&rm, &ReplayManager::sourceStatsUpdated, &app,
                     [&latestStats](int sourceIndex, SrtStats stats) {
                         latestStats.insert(sourceIndex, stats);
                     });
```

- [ ] **Step 3: Print stats before stopRecording.** In the outer `singleShot` lambda — alongside the connection telemetry that already prints *before* `rm.stopRecording()` (the block added by the connect-gate de-flake) — add, after the `reportConnectionEvents` block and before `rm.stopRecording();`:

```cpp
            if (reportStats) {
                QList<int> srcs = latestStats.keys();
                std::sort(srcs.begin(), srcs.end());
                for (int src : srcs) {
                    const SrtStats& s = latestStats.value(src);
                    fprintf(stderr,
                            "stats src=%d recv=%lld retrans=%lld loss=%lld drop=%lld\n",
                            src, (long long)s.recvTotal, (long long)s.retransTotal,
                            (long long)s.lossTotal, (long long)s.dropTotal);
                }
                fflush(stderr);
            }
```

- [ ] **Step 4: Verify it builds**

Run: `cmake --build build/srt --target sync_harness`
Expected: links clean.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/sync_harness.cpp
git commit -m "test(srt): sync_harness --report-stats prints per-source SrtStats"
```

---

### Task 6: `e2e_native_srt_ui_stats` gate (proves the engine→UI data path)

**Files:**
- Create: `tests/e2e/run_srt_ui_stats.sh`
- Modify: `tests/e2e/CMakeLists.txt` (register inside `if(APPLE)`, base port 23700)

- [ ] **Step 1: Write the gate script** — `tests/e2e/run_srt_ui_stats.sh`:

```bash
#!/usr/bin/env bash
# Local SRT e2e (JIT-5): the SRT loss-telemetry DATA PATH that feeds the
# connection-status UI. The native ingest samples srt_bstats and pushes it via
# reportStats -> StreamWorker::statsUpdated -> ReplayManager::sourceStatsUpdated;
# sync_harness --report-stats prints whatever reaches that RM signal. We drive one
# source through a lossy_udp_relay.py and assert the harness's reported per-source
# stats carry REAL telemetry: retrans>0 under loss, drop==0 (recovered), recv>0;
# and a clean control run shows recv>0 with ~no retransmits. This proves the exact
# numbers the UI dot/tooltip will read, without needing QML.
#
# Requires sync_harness built with -DOLR_FFMPEG_SRT_PREFIX; native ingest via
# OLR_NATIVE_SRT=1 (set by the CTest registration).
# Usage: run_srt_ui_stats.sh <sync_harness_exe> [base_port]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=srt_lib.sh
. "$HERE/srt_lib.sh"

HARNESS="${1:?sync_harness executable path required}"
BASE="${2:-23700}"
SECS="${OLR_SRT_UISTATS_SECS:-10}"
SEED="${OLR_SRT_UISTATS_SEED:-1234}"
LOSS_MOD="${OLR_SRT_UISTATS_LOSS:-12}"
RELAY="$HERE/lossy_udp_relay.py"

srt_require_tools
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
[ -f "$RELAY" ] || { echo "FAIL: $RELAY missing"; exit 1; }

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    (( ${#PIDS[@]} )) && { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.4; kill -9 "${PIDS[@]}" 2>/dev/null; }
    wait 2>/dev/null; rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Record one source through the relay at $1=loss% $2=tag $3=port_base, with
# --report-stats. Parses the harness's "stats src=0 ..." line into globals.
measure() {
    local loss="$1" tag="$2" pb="$3"
    local S=$pb UDP=$((pb+1)) R=$((pb+2)) pp brp rp hp
    flash_marker_to_udps "$UDP"; pp=$SRT_LAST_PID
    srt_bridge "$UDP" "$S"; brp=$SRT_LAST_PID
    python3 "$RELAY" "$R" "127.0.0.1:$S" "$loss" "$WORKDIR/$tag.stats" "$SEED" &
    rp=$!; PIDS+=("$rp")
    sleep 1.5
    "$HARNESS" --url "srt://127.0.0.1:$R?transtype=live" \
        --outdir "$WORKDIR" --name "uistats_$tag" --seconds "$SECS" --fps 30 \
        --report-stats >"$WORKDIR/$tag.out" 2>"$WORKDIR/$tag.err" &
    hp=$!; PIDS+=("$hp")
    wait "$hp"
    kill -TERM "$rp" 2>/dev/null; sleep 0.5; kill -9 "$rp" 2>/dev/null
    kill "$pp" "$brp" 2>/dev/null; wait "$pp" "$brp" 2>/dev/null
}

# field from the harness "stats src=0 recv=.. retrans=.. loss=.. drop=.." line
hstat() {  # $1=err file  $2=field
    grep '^stats src=0 ' "$1" | tail -1 | tr ' ' '\n' | awk -F= -v k="$2" '$1==k{print $2; exit}'
}
relay_dropped() { awk -F'[= ]' '/dropped=/{print $2; exit}' "$1" 2>/dev/null; }

echo "[srt-uistats] base=$BASE secs=$SECS seed=$SEED loss=${LOSS_MOD}%"
measure 0           clean "$BASE"
measure "$LOSS_MOD" lossy "$((BASE+4))"

RECV0="$(hstat "$WORKDIR/clean.err" recv)"; RETR0="$(hstat "$WORKDIR/clean.err" retrans)"
RECV1="$(hstat "$WORKDIR/lossy.err" recv)"; RETR1="$(hstat "$WORKDIR/lossy.err" retrans)"
DROP1="$(hstat "$WORKDIR/lossy.err" drop)"; RDROP1="$(relay_dropped "$WORKDIR/lossy.stats")"
echo "[srt-uistats] clean: recv=${RECV0:-?} retrans=${RETR0:-?}"
echo "[srt-uistats] lossy(${LOSS_MOD}%): recv=${RECV1:-?} retrans=${RETR1:-?} drop=${DROP1:-?} relay_dropped=${RDROP1:-?}"

fail=0
# The data path delivered stats at all (harness saw a "stats src=0" line w/ recv).
[ -n "${RECV0:-}" ] || { echo "FAIL: clean run reported no stats line — data path dead"; fail=1; }
[ -n "${RECV1:-}" ] || { echo "FAIL: lossy run reported no stats line — data path dead"; fail=1; }
awk -v r="${RECV1:-0}" 'BEGIN{exit !(r+0 > 0)}'  || { echo "FAIL: lossy recv=${RECV1:-?} not > 0"; fail=1; }
# Relay actually injected loss, and the UI data path carried the retransmit signal.
awk -v d="${RDROP1:-0}" 'BEGIN{exit !(d+0 >= 20)}' || { echo "FAIL: relay dropped only ${RDROP1:-0} at ${LOSS_MOD}% — loss not injected"; fail=1; }
awk -v r="${RETR1:-0}"  'BEGIN{exit !(r+0 > 0)}'   || { echo "FAIL: lossy retrans=${RETR1:-?} — UI data path did not carry ARQ retransmits"; fail=1; }
# Recovered: nothing finally unrecovered (this is what keeps the dot out of red).
awk -v p="${DROP1:-999}" 'BEGIN{exit !(p+0 == 0)}' || { echo "FAIL: lossy drop=${DROP1:-?} — expected full recovery (0) at ${LOSS_MOD}%"; fail=1; }
# Clean control: the same path reports ~no retransmits (discriminates healthy vs
# stress). Tolerant of a stray loopback hiccup; the lossy retrans>0 is the real proof.
awk -v r="${RETR0:-0}" 'BEGIN{exit !(r+0 <= 2)}'   || { echo "FAIL: clean retrans=${RETR0:-?} — expected ~0 on a clean loopback link"; fail=1; }

[ $fail -ne 0 ] && exit 1
echo "PASS: SRT stats data path delivers real telemetry to the UI signal (lossy retrans=${RETR1}, drop=${DROP1}; clean retrans=${RETR0})"
exit 0
```

- [ ] **Step 2: `chmod +x`**

Run: `chmod +x tests/e2e/run_srt_ui_stats.sh`

- [ ] **Step 3: Register the gate.** In `tests/e2e/CMakeLists.txt`, inside the `if(APPLE)` block, before the closing `endif()` (line 216), add:

```cmake
    # JIT-5: the SRT loss-telemetry DATA PATH behind the connection-status UI.
    # native ingest -> reportStats -> StreamWorker -> ReplayManager::sourceStatsUpdated;
    # sync_harness --report-stats reads it. Assert real telemetry (retrans>0, drop==0)
    # under relayed loss, ~no retrans on a clean link. Base 23700.
    add_test(NAME e2e_native_srt_ui_stats
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_ui_stats.sh" "$<TARGET_FILE:sync_harness>" 23700)
    set_tests_properties(e2e_native_srt_ui_stats PROPERTIES
        LABELS "native-apple-ingest"
        TIMEOUT 120
        RUN_SERIAL TRUE
        ENVIRONMENT "OLR_NATIVE_SRT=1")
```

- [ ] **Step 4: Reconfigure + run the gate**

Run: `cmake --build build/srt --target sync_harness && ( cd build/srt && ctest -R e2e_native_srt_ui_stats --output-on-failure )`
Expected: PASS — `lossy retrans>0, drop=0; clean retrans=0`.

(If `ctest` does not see the new test, re-run CMake configure: `cmake -S . -B build/srt` then rebuild.)

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/run_srt_ui_stats.sh tests/e2e/CMakeLists.txt
git commit -m "test(srt): e2e_native_srt_ui_stats — SRT stats UI data path over real SRT"
```

---

### Task 7: QML — grade the dot + extend the tooltip

**Files:**
- Modify: `Main.qml:1355-1370` (the `connDot` Rectangle)

The dot already has a `HoverHandler` (`connHover`) and a `ToolTip`; extend them.

- [ ] **Step 1: Replace the `connDot` body** (lines 1355-1370) with:

```qml
                        Rectangle {
                            id: connDot
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            radius: width / 2
                            property bool connected: appWindow.uiManagerRef.sourceConnectionVersion >= 0
                                                     && appWindow.uiManagerRef.isSourceConnected(streamRow.index)
                            // 0=N/A,1=green,2=amber,3=red (native SRT only; else 0)
                            property int linkHealth: appWindow.uiManagerRef.sourceStatsVersion >= 0
                                                     ? appWindow.uiManagerRef.sourceLinkHealth(streamRow.index)
                                                     : 0
                            color: !appWindow.uiManagerRef.isRecording
                                   ? "#555"
                                   : (!connDot.connected
                                      ? "#d32f2f"
                                      : (connDot.linkHealth === 3 ? "#d32f2f"
                                         : connDot.linkHealth === 2 ? "#f9a825"
                                         : "#2e7d32"))
                            HoverHandler { id: connHover }
                            ToolTip.visible: connHover.hovered
                            ToolTip.text: !appWindow.uiManagerRef.isRecording
                                          ? "Not recording"
                                          : (!connDot.connected
                                             ? "No signal"
                                             : (appWindow.uiManagerRef.sourceStatsVersion >= 0
                                                && appWindow.uiManagerRef.sourceHasSrtStats(streamRow.index)
                                                ? appWindow.uiManagerRef.sourceStatsTooltip(streamRow.index)
                                                : "Connected"))
                        }
```

- [ ] **Step 2: Verify the QML still loads/lints**

Run: `( cd build/srt && ctest -R qml --output-on-failure )` (the QML static-load smoke), or run the app and confirm no QML errors on the console.
Expected: PASS / no QML binding errors. If no qml smoke test exists in this build dir, run the app target and visually confirm the sources list renders.

- [ ] **Step 3: Manual visual check** (the headless harness has no QML — this is the only place to eyeball the pixels). With an SRT-enabled local build:
  - Clean SRT source → dot green; hover shows `SRT link / recv .. / retrans 0 (0.0%) / loss det 0 / dropped 0`.
  - Source via `lossy_udp_relay.py` at ~12% loss → dot flickers amber under retransmit bursts, green when quiet, tooltip retrans climbs, dropped stays 0.
  - Heavy loss inducing drops → dot red while drops occur, back to green on recovery.
  - A non-SRT source (e.g. UDP/RTMP) → dot plain green when connected, tooltip just `Connected`.

- [ ] **Step 4: Commit**

```bash
git add Main.qml
git commit -m "feat(srt): grade the connection dot by SRT link health + stats tooltip"
```

---

### Task 8: Document the feature + the manual UI check

**Files:**
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Append a section** to `tests/e2e/SRT_README.md` (after the Phase 2c-c/2d section):

````markdown
## JIT-5: SRT link telemetry on the connection-status UI

The native SRT ingest samples `srt_bstats` ~1/sec and pushes each cumulative
snapshot (`SrtStats`: recv / retrans / lossDetected / drop) up the same queued-
signal path as connection status: `reportStats` → `StreamWorker::statsUpdated` →
`ReplayManager::sourceStatsUpdated` → `UIManager`. The per-source connection dot
(`Main.qml`) is graded from the most-recent window:

- **green** — healthy (incl. ARQ quietly recovering everything),
- **amber** — link stressed: windowed retransmit rate > `OLR_SRT_HEALTH_AMBER_PCT`
  (default 0.02 = 2 % of received packets),
- **red** — recent **unrecovered** drops (`pktRcvDropTotal` rose this window), or
  the source is disconnected.

Hovering the dot shows cumulative `recv / retrans (%) / loss det / dropped`. SRT
stats are **native-ingest-only** — RTMP/UDP/ffmpeg-SRT sources keep the plain
green/red dot and no stats tooltip.

**Automated coverage:**
- `tst_srt_health` (unit) — the pure green/amber/red classifier incl. the
  counter-reset clamp.
- `e2e_native_srt_ui_stats` (`native-apple-ingest`) — drives a source through
  `lossy_udp_relay.py` and asserts `sync_harness --report-stats` (which reads
  `ReplayManager::sourceStatsUpdated`) carries real telemetry: retrans>0 / drop==0
  under loss, ~no retrans on a clean link. Proves the whole path **except the QML
  pixels**.

**Manual UI check** (the harness has no QML): with an SRT build, confirm a clean
source shows green, a relayed-lossy source goes amber under stress / red on induced
drops / back to green on recovery, and a non-SRT source stays plain green.
````

- [ ] **Step 2: Commit**

```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): document JIT-5 link telemetry on the connection-status UI"
```

---

## After all tasks

- Run the full local SRT + unit suites to confirm no regression:
  - `cmake --build build/srt && ( cd build/srt && ctest -L unit --output-on-failure )`
  - `( cd build/srt && ctest -L srt --output-on-failure )`
  - `( cd build/srt && ctest -L native-apple-ingest --output-on-failure )`
- Dispatch a final code review over the whole branch.
- Use superpowers:finishing-a-development-branch. **Do NOT push** unless explicitly told (local-only constraint).
