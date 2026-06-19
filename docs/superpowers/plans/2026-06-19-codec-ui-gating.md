# Codec Selection UI + Gating Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface the codec choice and the device benchmark in the UI, and gate recording so the user can't start a broken session — completing the user-facing layer of the selectable-codec feature (Plan 4 of 4).

**Architecture:** A small pure record-gate decision module (hard-block / soft-warn) drives both a `UIManager` capability property and the `startRecording()` gate. `UIManager` gains an async wrapper that runs the Plan 3 `runCodecBenchmark` on a worker thread (via `QtConcurrent`) and marshals coalesced progress + the result back to the GUI thread as signals/properties, with a device-keyed cache. Main.qml gains a codec selector (H.264 disabled when no hardware) and a benchmark panel.

**Tech Stack:** C++17, Qt 6 (Core/Quick/Concurrent/Test), QML, the Plan 3 benchmark engine (`recorder_engine/benchmark/runcodecbenchmark.h`), `queryNativeVideoEncodeCapabilities()`.

## Global Constraints

- **Hardware-only H.264 is enforced at the gate:** if the user selected H.264 but no hardware encoder is available, `startRecording()` HARD-BLOCKS with an actionable message and does not start — never silently falls back to software or to MPEG-2.
- **Soft capacity warning, not a block:** if the configured feed count exceeds the benchmarked safe count for the chosen codec, warn but proceed (the operator owns the call).
- **The benchmark runs OFF the GUI thread** (Plan 3's `runCodecBenchmark` is a blocking worker-thread call); the UI wrapper marshals **coalesced** progress (one update per ramp step) and the final result back to the GUI thread. No codec work on the GUI thread.
- **MPEG-2 is always available; H.264 is offered only when `queryNativeVideoEncodeCapabilities().h264` is true.**
- Default codec stays `Mpeg2Software` (Plan 1). The `recordCodec` QML property already exists (Plan 1, `"mpeg2"`/`"h264"`).
- Build (worktree root): configure `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`; test `ctest --test-dir build/claude-debug -R <name> -V`. **New C++ must follow `.clang-format` (attached braces) — run `/opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit $(git merge-base origin/main HEAD)` before committing, or CI's changed-line Lint gate fails.**

---

## File Structure

- **Create** `recorder_engine/benchmark/recordgate.{h,cpp}` — pure hard-block / soft-warn decision functions.
- **Create** `tests/unit/tst_recordgate.cpp` — unit tests for the gate decisions.
- **Modify** `uimanager.h` / `uimanager.cpp` — `h264EncodeAvailable` property; `runBenchmark()` / `cancelBenchmark()` invokables; `benchmarkProgress` / `benchmarkFinished` signals; `benchmarkResult` + `benchmarkRunning` properties; cache load/save; record-start gating.
- **Modify** `Main.qml` — codec selector ComboBox (H.264 gated) + benchmark panel in the recording-settings section.
- **Modify** `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt` — wire the new sources/test.

---

## Task 1: Pure record-gate decisions

**Files:**
- Create: `recorder_engine/benchmark/recordgate.h`, `recorder_engine/benchmark/recordgate.cpp`
- Test: `tests/unit/tst_recordgate.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `bool recordCodecUnavailable(VideoCodecChoice codec, bool h264HardwareAvailable);` → true only when `codec == H264Hardware && !h264HardwareAvailable` (the hard-block condition).
  - `bool feedCountExceedsSafe(int configuredFeeds, int safeFeeds);` → true only when `safeFeeds > 0 && configuredFeeds > safeFeeds` (the soft-warn condition; `safeFeeds <= 0` means "not benchmarked / unknown" → no warning).
  - `QString recordCodecBlockReason(VideoCodecChoice codec);` → actionable message for the hard block (e.g. "H.264 hardware encoding is not available on this device. Select MPEG-2 (software) to record.").

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_recordgate.cpp`:

```cpp
// Unit tests for the pure record-gate decisions (hard block + soft warn).
#include <QtTest>

#include "recorder_engine/benchmark/recordgate.h"

class TestRecordGate : public QObject {
    Q_OBJECT
private slots:
    void hardBlockOnlyForH264WithoutHardware();
    void softWarnOnlyWhenConfiguredExceedsSafe();
    void blockReasonIsNonEmptyForH264();
};

void TestRecordGate::hardBlockOnlyForH264WithoutHardware() {
    QVERIFY(recordCodecUnavailable(VideoCodecChoice::H264Hardware, false));   // block
    QVERIFY(!recordCodecUnavailable(VideoCodecChoice::H264Hardware, true));   // hw present
    QVERIFY(!recordCodecUnavailable(VideoCodecChoice::Mpeg2Software, false)); // mpeg2 always ok
    QVERIFY(!recordCodecUnavailable(VideoCodecChoice::Mpeg2Software, true));
}

void TestRecordGate::softWarnOnlyWhenConfiguredExceedsSafe() {
    QVERIFY(feedCountExceedsSafe(10, 8));    // over -> warn
    QVERIFY(!feedCountExceedsSafe(8, 8));    // equal -> no warn
    QVERIFY(!feedCountExceedsSafe(4, 8));    // under -> no warn
    QVERIFY(!feedCountExceedsSafe(10, 0));   // not benchmarked -> no warn
    QVERIFY(!feedCountExceedsSafe(10, -1));  // unknown -> no warn
}

void TestRecordGate::blockReasonIsNonEmptyForH264() {
    QVERIFY(!recordCodecBlockReason(VideoCodecChoice::H264Hardware).isEmpty());
}

QTEST_GUILESS_MAIN(TestRecordGate)
#include "tst_recordgate.moc"
```

Register: `olr_add_unit_test(tst_recordgate olr_test_engine)` in `tests/unit/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_recordgate`
Expected: FAIL to compile — `recordgate.h` not found.

- [ ] **Step 3: Write the implementation**

Create `recorder_engine/benchmark/recordgate.h`:

```cpp
#ifndef OLR_RECORDGATE_H
#define OLR_RECORDGATE_H

#include "recorder_engine/codec/videocodecchoice.h"

#include <QString>

// True only when H.264 is selected but no hardware encoder exists (hard block).
bool recordCodecUnavailable(VideoCodecChoice codec, bool h264HardwareAvailable);

// True only when a positive benchmarked safe count is exceeded (soft warn).
// safeFeeds <= 0 means "not benchmarked / unknown" and never warns.
bool feedCountExceedsSafe(int configuredFeeds, int safeFeeds);

// Actionable message for the hard-block case.
QString recordCodecBlockReason(VideoCodecChoice codec);

#endif // OLR_RECORDGATE_H
```

Create `recorder_engine/benchmark/recordgate.cpp`:

```cpp
#include "recorder_engine/benchmark/recordgate.h"

bool recordCodecUnavailable(VideoCodecChoice codec, bool h264HardwareAvailable) {
    return codec == VideoCodecChoice::H264Hardware && !h264HardwareAvailable;
}

bool feedCountExceedsSafe(int configuredFeeds, int safeFeeds) {
    return safeFeeds > 0 && configuredFeeds > safeFeeds;
}

QString recordCodecBlockReason(VideoCodecChoice codec) {
    if (codec == VideoCodecChoice::H264Hardware) {
        return QStringLiteral(
            "H.264 hardware encoding is not available on this device. "
            "Select MPEG-2 (software) to record.");
    }
    return QString();
}
```

Add `recordgate.h`/`recordgate.cpp` to the engine library source lists in `CMakeLists.txt` (near the other `recorder_engine/benchmark/*` entries) and `tests/CMakeLists.txt` (the `olr_test_engine` list).

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_recordgate && ctest --test-dir build/claude-debug -R tst_recordgate -V`
Expected: PASS (3 tests).

- [ ] **Step 5: Format + commit**

Run the changed-line formatter (see Global Constraints), then:

```bash
git add recorder_engine/benchmark/recordgate.h recorder_engine/benchmark/recordgate.cpp tests/unit/tst_recordgate.cpp CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "feat: pure record-gate decisions (hard block + soft warn)"
```

---

## Task 2: Expose H.264 encode availability on UIManager

**Files:**
- Modify: `uimanager.h` (property + getter + signal + member), `uimanager.cpp` (probe at construction, off the GUI thread)

**Interfaces:**
- Consumes: `queryNativeVideoEncodeCapabilities()` (`recorder_engine/codec/nativevideoencoder.h`).
- Produces: QML property `bool h264EncodeAvailable` (READ `h264EncodeAvailable` NOTIFY `h264EncodeAvailableChanged`) — cached result of the capability probe.

> The probe opens a throwaway hardware encoder, so run it ONCE off the GUI thread (via `QtConcurrent::run`) at construction and publish the result with a queued signal; default `false` until it resolves. This keeps the GUI thread free of codec work (Global Constraint).

- [ ] **Step 1: Declare the property + member**

In `uimanager.h`, add the include near the others:

```cpp
#include "recorder_engine/codec/nativevideoencoder.h"
```

Add the Q_PROPERTY after the `recordCodec` property (line 44):

```cpp
    Q_PROPERTY(bool h264EncodeAvailable READ h264EncodeAvailable NOTIFY h264EncodeAvailableChanged)
```

Add the getter near `recordCodec()` and a signal near `recordCodecChanged()`:

```cpp
    bool h264EncodeAvailable() const { return m_h264EncodeAvailable; }
```
```cpp
    void h264EncodeAvailableChanged();
```

Add a member (private section, with the other settings-derived members):

```cpp
    bool m_h264EncodeAvailable = false;
```

- [ ] **Step 2: Probe off the GUI thread at construction**

In the `UIManager` constructor body (`uimanager.cpp`), after the existing setup, add an async probe:

```cpp
    // Probe hardware H.264 encode availability once, off the GUI thread (the
    // probe opens a throwaway encoder). Publish via a queued signal.
    (void)QtConcurrent::run([this]() {
        const bool available = queryNativeVideoEncodeCapabilities().h264;
        QMetaObject::invokeMethod(this, [this, available]() {
            if (m_h264EncodeAvailable != available) {
                m_h264EncodeAvailable = available;
                emit h264EncodeAvailableChanged();
            }
        }, Qt::QueuedConnection);
    });
```

Add `#include <QtConcurrent>` to `uimanager.cpp`. (The app already links `Qt6::Concurrent`.)

- [ ] **Step 3: Build the app target**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug OpenLiveReplay`
Expected: compiles and links; moc picks up the new property/signal.

- [ ] **Step 4: Format + commit**

```bash
git add uimanager.h uimanager.cpp
git commit -m "feat: expose h264EncodeAvailable capability on UIManager"
```

---

## Task 3: Async benchmark wrapper on UIManager

**Files:**
- Modify: `uimanager.h` (invokables, signals, properties, members), `uimanager.cpp` (implementation)

**Interfaces:**
- Consumes: `runCodecBenchmark`, `CodecBenchmarkResult`, `BenchmarkConfig` (`recorder_engine/benchmark/runcodecbenchmark.h`); cache (`benchmarkcache.h`).
- Produces:
  - `Q_INVOKABLE void runBenchmark();` — starts the benchmark on a worker thread at the current record resolution/fps; no-op if already running.
  - `Q_INVOKABLE void cancelBenchmark();` — requests cancellation.
  - signals `void benchmarkProgress(int concurrency, bool sustained);` and `void benchmarkFinished();`.
  - properties `bool benchmarkRunning` and `QVariantMap benchmarkResult` (keys: `h264Available`, `h264SafeFeeds`, `mpeg2SafeFeeds`, `recommended` (`"mpeg2"`/`"h264"`), `deviceLabel`, `resolution`, `timestamp`) for QML to render the results table + advisory line.

> Threading: `runBenchmark` launches `runCodecBenchmark` via `QtConcurrent::run`, passing a `ProgressFn` that emits `benchmarkProgress` through a queued `QMetaObject::invokeMethod` (coalesced — one per ramp step), and an `std::atomic<bool>` cancel owned by UIManager. On completion it stamps the timestamp, saves the cache (device-keyed path under `QStandardPaths::AppDataLocation`), updates `m_benchmarkResult`/`m_benchmarkRunning` on the GUI thread, and emits `benchmarkFinished`. On startup, `loadBenchmarkResult` populates `m_benchmarkResult` from cache if it matches this device+resolution (so the last result shows without re-running).

- [ ] **Step 1: Declare invokables/signals/properties/members**

In `uimanager.h` add includes:

```cpp
#include "recorder_engine/benchmark/runcodecbenchmark.h"
#include <atomic>
```

Properties (after `h264EncodeAvailable`):

```cpp
    Q_PROPERTY(bool benchmarkRunning READ benchmarkRunning NOTIFY benchmarkRunningChanged)
    Q_PROPERTY(QVariantMap benchmarkResult READ benchmarkResult NOTIFY benchmarkResultChanged)
```

Invokables (with the other `Q_INVOKABLE` declarations):

```cpp
    Q_INVOKABLE void runBenchmark();
    Q_INVOKABLE void cancelBenchmark();
```

Getters, signals, members:

```cpp
    bool benchmarkRunning() const { return m_benchmarkRunning; }
    QVariantMap benchmarkResult() const { return m_benchmarkResult; }
```
```cpp
    void benchmarkRunningChanged();
    void benchmarkResultChanged();
    void benchmarkProgress(int concurrency, bool sustained);
    void benchmarkFinished();
```
```cpp
    bool m_benchmarkRunning = false;
    QVariantMap m_benchmarkResult;
    std::atomic<bool> m_benchmarkCancel{false};
    int m_benchmarkSafeFeedsForChosen = -1; // safe feeds for the currently-selected codec (for the gate)
```

- [ ] **Step 2: Implement the wrapper**

In `uimanager.cpp` add `#include "recorder_engine/benchmark/benchmarkcache.h"` and implement. A `resultToVariantMap(const CodecBenchmarkResult&)` helper converts the struct to `m_benchmarkResult` (codec via `videoCodecToString`). `runBenchmark`:

```cpp
void UIManager::runBenchmark() {
    if (m_benchmarkRunning) return;
    m_benchmarkRunning = true;
    m_benchmarkCancel.store(false);
    emit benchmarkRunningChanged();

    BenchmarkConfig cfg;
    cfg.width = m_currentSettings.videoWidth;
    cfg.height = m_currentSettings.videoHeight;
    cfg.fps = m_currentSettings.fps;

    (void)QtConcurrent::run([this, cfg]() {
        auto progress = [this](int n, bool sustained) {
            QMetaObject::invokeMethod(this, [this, n, sustained]() {
                emit benchmarkProgress(n, sustained);
            }, Qt::QueuedConnection);
        };
        CodecBenchmarkResult r = runCodecBenchmark(cfg, progress, m_benchmarkCancel);
        r.timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        saveBenchmarkResult(benchmarkCachePath(), r);
        QMetaObject::invokeMethod(this, [this, r]() {
            m_benchmarkResult = resultToVariantMap(r);
            m_benchmarkSafeFeedsForChosen =
                (m_currentSettings.videoCodec == VideoCodecChoice::H264Hardware)
                    ? r.h264SafeFeeds : r.mpeg2SafeFeeds;
            m_benchmarkRunning = false;
            emit benchmarkResultChanged();
            emit benchmarkRunningChanged();
            emit benchmarkFinished();
        }, Qt::QueuedConnection);
    });
}

void UIManager::cancelBenchmark() { m_benchmarkCancel.store(true); }
```

`benchmarkCachePath()` is a small private helper returning `QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/codec-benchmark.json"` (mkpath the dir). At construction, after the capability probe, attempt `loadBenchmarkResult` and, if `benchmarkResultMatches(cached, benchmarkDeviceLabel(), "WxH@fps")`, populate `m_benchmarkResult` + `m_benchmarkSafeFeedsForChosen`.

- [ ] **Step 3: Build the app target**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug OpenLiveReplay`
Expected: compiles + links.

- [ ] **Step 4: Format + commit**

```bash
git add uimanager.h uimanager.cpp
git commit -m "feat: async codec-benchmark wrapper + result cache on UIManager"
```

---

## Task 4: Record-start gating

**Files:**
- Modify: `uimanager.h` (a `recordingWarning` signal), `uimanager.cpp` (`startRecording()` gate)

**Interfaces:**
- Consumes: `recordCodecUnavailable`, `feedCountExceedsSafe`, `recordCodecBlockReason` (Task 1); `m_h264EncodeAvailable` (Task 2); `m_benchmarkSafeFeedsForChosen` (Task 3); existing `recordingFailed` signal.
- Produces: `void recordingWarning(const QString& message);` (non-blocking soft warning surfaced to QML).

- [ ] **Step 1: Add the warning signal**

In `uimanager.h`, near `recordingFailed`:

```cpp
    void recordingWarning(const QString& message);
```

- [ ] **Step 2: Gate at the top of startRecording()**

In `uimanager.cpp` `startRecording()`, add at the very top (before the existing `hadSources` logic), including `#include "recorder_engine/benchmark/recordgate.h"`:

```cpp
    // Hard block: H.264 selected but no hardware encoder -> refuse, never fall back.
    if (recordCodecUnavailable(m_currentSettings.videoCodec, m_h264EncodeAvailable)) {
        const QString reason = recordCodecBlockReason(m_currentSettings.videoCodec);
        qWarning() << "UIManager:" << reason;
        emit recordingFailed(reason);
        return;
    }
    // Soft warning: configured feeds exceed the benchmarked safe count for the codec.
    const int configuredFeeds = m_replayManager->getSourceUrls().size();
    if (feedCountExceedsSafe(configuredFeeds, m_benchmarkSafeFeedsForChosen)) {
        emit recordingWarning(
            QStringLiteral("Recording %1 feeds; this device benchmarked %2 as the safe limit "
                           "for the selected codec — frames may drop.")
                .arg(configuredFeeds)
                .arg(m_benchmarkSafeFeedsForChosen));
        // proceed — operator's call.
    }
```

- [ ] **Step 3: Build + full unit suite (regression)**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug && ctest --test-dir build/claude-debug -L unit`
Expected: PASS — app builds; all unit tests green (incl. `tst_recordgate`); MPEG-2 default path unchanged (the gate is a no-op for MPEG-2 with default/unknown safe feeds).

- [ ] **Step 4: Format + commit**

```bash
git add uimanager.h uimanager.cpp
git commit -m "feat: gate recording on codec availability + safe feed count"
```

---

## Task 5: QML — codec selector + benchmark panel

**Files:**
- Modify: `Main.qml` (recording-settings section, ~lines 1203-1267, next to the resolution/fps controls)

**Interfaces:**
- Consumes: `recordCodec`, `h264EncodeAvailable`, `benchmarkRunning`, `benchmarkResult`, `runBenchmark()`, `cancelBenchmark()`, `benchmarkProgress`, `recordingWarning` on `uiManagerRef`.

> No unit test (QML); verified by the app build + the `qmllint` smoke check (`tests/smoke`). Mirror the existing `ComboBox`/`SpinBox` styling in the settings section.

- [ ] **Step 1: Add the codec selector + benchmark panel**

In `Main.qml`, in the recording-settings section near the fps `ComboBox` (~line 1229), add:
- A **codec ComboBox** with model `["MPEG-2 (software)", "H.264 (hardware)"]`; `currentIndex` derived from `uiManagerRef.recordCodec` (`"h264"` → 1 else 0); `onActivated` sets `uiManagerRef.recordCodec = index === 1 ? "h264" : "mpeg2"`; the H.264 entry is **disabled when `!uiManagerRef.h264EncodeAvailable`** (use a delegate that greys the second row, or disable selection + show "(no hardware)"), and if H.264 was selected but becomes unavailable, fall back to index 0.
- A **"Run benchmark" Button**: `enabled: !uiManagerRef.benchmarkRunning`; `onClicked: uiManagerRef.runBenchmark()`; while running show a `BusyIndicator` + the latest `benchmarkProgress` (`Connections { target: uiManagerRef; function onBenchmarkProgress(n, sustained) { ... } }`) and a Cancel button calling `cancelBenchmark()`.
- A **results display** bound to `uiManagerRef.benchmarkResult`: per-codec safe feeds + the advisory line ("Recommended: H.264 — N feeds" derived from `recommended`/`h264SafeFeeds`/`mpeg2SafeFeeds`). Show "Not benchmarked yet" when the map is empty.
- A **toast/banner** for `recordingWarning` (`Connections { target: uiManagerRef; function onRecordingWarning(msg) { ... } }`) — reuse the existing error/notification UI if present (the existing `recordingFailed` handling is the pattern).

- [ ] **Step 2: Build the app + run the QML smoke (qmllint)**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug && ctest --test-dir build/claude-debug -R qml -V` (or the smoke test name under `tests/smoke`).
Expected: app builds; qmllint smoke passes (no QML warnings/errors on the module).

- [ ] **Step 3: Manual sanity (document in the report)**

Launch the app, open recording settings, confirm: the codec selector shows both options with H.264 disabled iff no hardware; "Run benchmark" runs without freezing the UI, shows progress, then results + recommendation; selecting H.264 on a no-hardware device and pressing Record surfaces the hard-block message; a feed count over the safe limit surfaces the soft warning.

- [ ] **Step 4: Format (QML is not clang-formatted) + commit**

```bash
git add Main.qml
git commit -m "feat: codec selector + benchmark panel in recording settings"
```

---

## Self-Review

**Spec coverage (this plan = subsystem 4 of the design doc):**
- Codec selector (MPEG-2 always; H.264 gated on capability) → Task 2 (capability) + Task 5 (UI). ✓
- Benchmark panel (run, progress, results table, recommendation) → Task 3 (wrapper) + Task 5 (UI). ✓
- Off-GUI-thread benchmark with coalesced progress → Task 3. ✓
- Hard block (H.264 unavailable at record start) → Task 1 + Task 4. ✓
- Soft warning (feeds > safe count) → Task 1 + Task 4. ✓
- Persisted result cache (device-keyed) → Task 3 (load at startup / save on finish). ✓
- `videoCodec` persistence + `recordCodec` property → done in Plan 1 (reused). ✓

**Carry-forwards (documented, intentionally NOT tasks here):**
- `videoCodecToString` switch + `Q_UNREACHABLE()` — only relevant when a 3rd codec enumerator is added; there is no 3rd codec in this plan, so the change is correctly deferred to that future commit.
- FFmpeg `--disable-encoder=libx264 --disable-decoder=h264` build flags — these apply only to *from-source* FFmpeg builds (the Windows/iOS build scripts), not the macOS Homebrew FFmpeg the desktop app links (which ships libx264/h264 and cannot be reconfigured). The runtime "no software H.264 path is ever taken" guarantee is already enforced in code (Plan 2). This build-hardening is a separate, platform-specific build-scripts concern (achievable for Windows/iOS from-source builds, infeasible for brew macOS), not part of the UI plan; tracked as a follow-up rather than a task here.

**Placeholder scan:** Tasks 1-4 contain complete code; Task 5 (QML) describes each control concretely with the exact `uiManagerRef` members it binds and the existing styling to mirror — QML is verified by build + qmllint + manual sanity rather than unit tests (no QML unit harness in the repo), which is the established pattern.

**Type consistency:** `recordCodecUnavailable`/`feedCountExceedsSafe`/`recordCodecBlockReason` (Task 1) are used with the same signatures in Task 4. `runCodecBenchmark(BenchmarkConfig, ProgressFn, std::atomic<bool>&)` (Plan 3) is called as such in Task 3. `m_benchmarkSafeFeedsForChosen` set in Task 3 is consumed in Task 4. The QML property/signal names in Task 5 match those declared in Tasks 2-4.
