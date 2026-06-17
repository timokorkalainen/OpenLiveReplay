# P1: rational recording frame rate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace the integer recording `fps` with a rational `FrameRate {num,den}` across the recording engine so 29.97/59.94 record with correct frame timing + container frame rate; integer rates stay byte-identical.

**Architecture:** A pure `FrameRate` value type centralizes the frame↔ms math. The engine (`ReplayManager`, `StreamWorker`, `Muxer`, the blue encoder, `heartbeatFrameSpan`) stores/uses `FrameRate`; encoder time_base becomes `{den,num}` and muxer `avg/r_frame_rate` becomes `{num,den}`. A `setFps(int)` shim (builds `{n,1}`) keeps external callers green during the migration. Settings persist `fpsNum/fpsDen` (back-compat); the UI picks from presets.

**Tech Stack:** C++17, Qt6 (Core/QML), FFmpeg (AVRational/av_rescale_q), Qt Test, bash e2e (ffmpeg/ffprobe), CMake/Ninja/CTest.

**Spec:** `docs/superpowers/specs/2026-06-17-rational-fps-p1-design.md`

**Base branch:** `feat/rational-fps`, stacked on `feat/heartbeat-decouple`. Build dir `build/srt`. **Local-only.** Format only changed C++ lines via `/opt/homebrew/opt/llvm/bin/clang-format` (`git clang-format` on staged changes); some engine files are hand-Allman — never whole-file `-i`.

**Verified fps sites (from the exhaustive map):** `replaymanager.h:66,73,136` (setFps/getFps/m_fps), `replaymanager.cpp:97-98` (blue enc time_base/framerate), `:181-182` (muxer init), `:214` (StreamWorker ctor), `:375` (frameMs), `:409,448` (blue audio), `onTimerTick` heartbeat call; `streamworker.h:54,169` (ctor/m_targetFps), `streamworker.cpp:91,125` (frame→ms), `:184` rescale, `:404-420` (rate warning + enc time_base/framerate), `:546` (audio cursor); `muxer.h:31,33` + `muxer.cpp:31,56-57`; `heartbeat.{h,cpp}` (`int fps`); `settingsmanager.h:32`, `.cpp:41,134`; `uimanager.h:43`, `.cpp:654,1208-1223`; `Main.qml:1200-1209`; `record_harness.cpp:41,86`, `sync_harness.cpp:53,91`.

---

### Task 1: `FrameRate` type + presets/parse + unit test

**Files:** Create `recorder_engine/framerate.h`, `recorder_engine/framerate.cpp`, `tests/unit/tst_framerate.cpp`; modify `tests/CMakeLists.txt`, `CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

- [ ] **Step 1: Failing test** — `tests/unit/tst_framerate.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/framerate.h"

class TestFrameRate : public QObject {
    Q_OBJECT
private slots:
    void integerRate();
    void rate2997();
    void roundedFps();
    void samplesPerFrame();
    void parse();
    void label();
};

void TestFrameRate::integerRate() {
    const FrameRate r{30, 1};
    QCOMPARE(r.msForFrame(30), qint64(1000));
    QCOMPARE(r.frameForMs(1000), qint64(30));
    QCOMPARE(r.toDouble(), 30.0);
}

void TestFrameRate::rate2997() {
    const FrameRate r{30000, 1001};
    QCOMPARE(r.msForFrame(30), qint64(1001)); // 30 frames of 29.97 span 1001 ms
    QCOMPARE(r.frameForMs(1001), qint64(30));
    QVERIFY(r.toDouble() > 29.9 && r.toDouble() < 30.0);
}

void TestFrameRate::roundedFps() {
    QCOMPARE((FrameRate{30000, 1001}).roundedFps(), 30);
    QCOMPARE((FrameRate{60000, 1001}).roundedFps(), 60);
    QCOMPARE((FrameRate{25, 1}).roundedFps(), 25);
}

void TestFrameRate::samplesPerFrame() {
    QCOMPARE((FrameRate{30, 1}).samplesPerFrame(48000), qint64(1600));
    QCOMPARE((FrameRate{30000, 1001}).samplesPerFrame(48000), qint64(1601)); // 1601.6 trunc
}

void TestFrameRate::parse() {
    QVERIFY(parseFrameRate("30") == (FrameRate{30, 1}));
    QVERIFY(parseFrameRate("29.97") == (FrameRate{30000, 1001}));
    QVERIFY(parseFrameRate("30000/1001") == (FrameRate{30000, 1001}));
    QVERIFY(parseFrameRate("garbage") == (FrameRate{30, 1}));
}

void TestFrameRate::label() {
    QCOMPARE(frameRateLabel(FrameRate{30000, 1001}), QStringLiteral("29.97"));
    QCOMPARE(frameRateLabel(FrameRate{30, 1}), QStringLiteral("30"));
}

QTEST_GUILESS_MAIN(TestFrameRate)
#include "tst_framerate.moc"
```

- [ ] **Step 2: Register** — in `tests/unit/CMakeLists.txt` after `olr_add_unit_test(tst_recordingclock   olr_test_core)`:
```cmake
olr_add_unit_test(tst_framerate        olr_test_core)
```

- [ ] **Step 3: Verify red** — `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target tst_framerate` → `framerate.h` not found.

- [ ] **Step 4: Create `recorder_engine/framerate.h`:**

```cpp
#ifndef FRAMERATE_H
#define FRAMERATE_H

#include <QString>

#include <cstdint>
#include <vector>

// A recording frame rate as an exact rational num/den (e.g. 29.97 = 30000/1001).
// Centralizes the integer-fps arithmetic so the whole engine is fractional-rate ready.
struct FrameRate {
    int num = 30;
    int den = 1;

    bool isValid() const { return num > 0 && den > 0; }
    double toDouble() const { return den != 0 ? double(num) / double(den) : 0.0; }
    int roundedFps() const { return den > 0 ? (num + den / 2) / den : 0; }
    // File-timeline milliseconds of frame index f:  f * 1000 * den / num.
    int64_t msForFrame(int64_t f) const {
        return num > 0 ? (f * 1000 * int64_t(den)) / num : 0;
    }
    // Frame index reached at wall-clock ms:  ms * num / (1000 * den).
    int64_t frameForMs(int64_t ms) const {
        return den > 0 ? (ms * num) / (int64_t(1000) * den) : 0;
    }
    // Audio samples per frame at sampleRate (truncated):  sampleRate * den / num.
    int64_t samplesPerFrame(int sampleRate) const {
        return num > 0 ? (int64_t(sampleRate) * den) / num : 0;
    }
};

inline bool operator==(const FrameRate& a, const FrameRate& b) {
    return a.num == b.num && a.den == b.den;
}
inline bool operator!=(const FrameRate& a, const FrameRate& b) { return !(a == b); }

struct FrameRatePreset {
    const char* label;
    FrameRate rate;
};

// The standard selectable rates (fractional broadcast + integer).
const std::vector<FrameRatePreset>& frameRatePresets();
// Parse "30", "29.97", or "30000/1001" into a FrameRate (anything invalid -> {30,1}).
FrameRate parseFrameRate(const QString& s);
// Nearest preset label for a rate, else "num/den".
QString frameRateLabel(const FrameRate& r);

#endif  // FRAMERATE_H
```

- [ ] **Step 5: Create `recorder_engine/framerate.cpp`:**

```cpp
#include "framerate.h"

#include <cmath>

const std::vector<FrameRatePreset>& frameRatePresets() {
    static const std::vector<FrameRatePreset> kPresets = {
        {"23.976", {24000, 1001}}, {"24", {24, 1}},  {"25", {25, 1}},
        {"29.97", {30000, 1001}},  {"30", {30, 1}},  {"50", {50, 1}},
        {"59.94", {60000, 1001}},  {"60", {60, 1}},
    };
    return kPresets;
}

FrameRate parseFrameRate(const QString& s) {
    const QString t = s.trimmed();
    if (t.contains('/')) {
        const QStringList parts = t.split('/');
        if (parts.size() == 2) {
            bool okN = false, okD = false;
            const int n = parts[0].trimmed().toInt(&okN);
            const int d = parts[1].trimmed().toInt(&okD);
            if (okN && okD && n > 0 && d > 0) return FrameRate{n, d};
        }
        return FrameRate{30, 1};
    }
    for (const FrameRatePreset& p : frameRatePresets()) {
        if (t == QString::fromLatin1(p.label)) return p.rate;
    }
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || v <= 0) return FrameRate{30, 1};
    for (const FrameRatePreset& p : frameRatePresets()) {
        if (std::fabs(p.rate.toDouble() - v) < 0.05) return p.rate; // snap typed decimals
    }
    return FrameRate{int(std::lround(v)), 1};
}

QString frameRateLabel(const FrameRate& r) {
    for (const FrameRatePreset& p : frameRatePresets()) {
        if (p.rate == r) return QString::fromLatin1(p.label);
    }
    return QStringLiteral("%1/%2").arg(r.num).arg(r.den);
}
```

- [ ] **Step 6: Wire `framerate.cpp` into both targets.** In `tests/CMakeLists.txt` (the `qt_add_library(olr_test_core STATIC` list), after `recordingclock.cpp`:
```cmake
    "${CMAKE_SOURCE_DIR}/recorder_engine/framerate.cpp"
```
In root `CMakeLists.txt`, after the `recorder_engine/heartbeat.h recorder_engine/heartbeat.cpp` line:
```cmake
        recorder_engine/framerate.h recorder_engine/framerate.cpp
```

- [ ] **Step 7: Build + run, expect PASS** — `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target tst_framerate && ( cd build/srt && ctest -R tst_framerate --output-on-failure )` → 6 functions pass.

- [ ] **Step 8: Format changed lines + commit**
```bash
git add recorder_engine/framerate.h recorder_engine/framerate.cpp tests/unit/tst_framerate.cpp \
        tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add -A recorder_engine tests
git commit -m "feat(fps): FrameRate {num,den} value type + presets/parse (unit-tested)"
```

---

### Task 2: Migrate the recording engine to `FrameRate` (heartbeat + ReplayManager + StreamWorker + Muxer)

This is one cohesive task — the type ripples through all four; the build is only consistent when they change together. A `setFps(int)` shim keeps external callers (UIManager, harnesses) green.

**Files:** `recorder_engine/heartbeat.{h,cpp}`, `tests/unit/tst_heartbeat.cpp`, `recorder_engine/replaymanager.{h,cpp}`, `recorder_engine/streamworker.{h,cpp}`, `recorder_engine/muxer.{h,cpp}`.

- [ ] **Step 1: heartbeat → FrameRate.** In `recorder_engine/heartbeat.h`: add `#include "framerate.h"`, change the signature `int fps` → `const FrameRate& rate`:
```cpp
FrameRate heartbeatFrameSpan(...)   // NO — keep return FrameSpan; only the fps param changes:
FrameSpan heartbeatFrameSpan(int64_t elapsedMs, const FrameRate& rate, int64_t lastFrame,
                             int maxPerTick, int64_t maxBacklogFrames);
```
In `recorder_engine/heartbeat.cpp`: change the signature to match, and replace the derive line:
```cpp
    const int64_t derivedFrame = (elapsedMs * fps) / 1000;
```
with:
```cpp
    if (!rate.isValid()) {
        return span;
    }
    const int64_t derivedFrame = rate.frameForMs(elapsedMs);
```
and delete the old `if (fps <= 0) { return span; }` guard (replaced by `!rate.isValid()`).

- [ ] **Step 2: Update `tst_heartbeat.cpp`** — pass `FrameRate` instead of the int `30`/`0`:
  - In every `heartbeatFrameSpan(elapsedMs, 30, lastFrame, 8, 30)` call, change `30` (the 2nd arg) to `FrameRate{30, 1}`. The `fpsZeroReturnsEmpty` case: change to `heartbeatFrameSpan(1000, FrameRate{0, 1}, 0, 8, 30)`.
  - Add `#include "recorder_engine/framerate.h"`.
  - Add one rational case:
```cpp
void TestHeartbeat::rate2997Span() {
    // 1001 ms @29.97 -> frame 30 exactly.
    const FrameSpan s = heartbeatFrameSpan(1001, FrameRate{30000, 1001}, 29, 8, 30);
    QCOMPARE(s.from, qint64(30));
    QCOMPARE(s.to, qint64(30));
}
```
  (add `void rate2997Span();` to the slots.)

- [ ] **Step 3: ReplayManager header (`replaymanager.h`).** Add `#include "framerate.h"` (near the other recorder_engine includes). Change member `int m_fps = 30;` → `FrameRate m_frameRate{30, 1};`. Replace the setter/getter:
```cpp
    void setFps(int fps) { m_fps = fps; }
    ...
    int getFps() const { return m_fps; }
```
with:
```cpp
    void setFps(int fps) { m_frameRate = FrameRate{fps > 0 ? fps : 30, 1}; }  // shim
    void setFrameRate(const FrameRate& r) { if (r.isValid()) m_frameRate = r; }
    FrameRate frameRate() const { return m_frameRate; }
    int getFps() const { return m_frameRate.roundedFps(); }
```

- [ ] **Step 4: ReplayManager cpp (`replaymanager.cpp`).** Apply these substitutions (the explore's exact lines):
  - `m_blueEncCtx->time_base = {1, m_fps};` → `m_blueEncCtx->time_base = {m_frameRate.den, m_frameRate.num};`
  - `m_blueEncCtx->framerate = {m_fps, 1};` → `m_blueEncCtx->framerate = {m_frameRate.num, m_frameRate.den};`
  - The two `m_muxer->init(..., m_fps, ...)` calls → pass `m_frameRate` (Muxer::init becomes FrameRate in Step 7).
  - The `StreamWorker(..., m_fps)` ctor call (`:214`) → pass `m_frameRate`.
  - `const int64_t frameMs = (f * 1000) / m_fps;` (in onTimerTick) → `const int64_t frameMs = m_frameRate.msForFrame(f);`
  - The `heartbeatFrameSpan(elapsedMs, m_fps, m_globalFrameCount, kMaxFramesPerTick, m_fps)` call → `heartbeatFrameSpan(elapsedMs, m_frameRate, m_globalFrameCount, kMaxFramesPerTick, m_frameRate.roundedFps())`.
  - `const int64_t recMs = (m_globalFrameCount * 1000) / m_fps;` → `const int64_t recMs = m_frameRate.msForFrame(m_globalFrameCount);`
  - `... - StreamWorker::kAudioSampleRate / m_fps);` → `... - m_frameRate.samplesPerFrame(StreamWorker::kAudioSampleRate));`
  - The `pkt->pts = m_globalFrameCount; pkt->dts = m_globalFrameCount;` blue-frame stamping is unchanged (frame index on the encoder timebase, now `{den,num}` — correct).

- [ ] **Step 5: StreamWorker header (`streamworker.h`).** Add `#include "ingest/ingestsession.h"`-adjacent: `#include "framerate.h"`. Change ctor param `int targetFps` → `FrameRate targetRate` (in both the declaration and the member init). Change member `int m_targetFps = 30;` → `FrameRate m_targetRate{30, 1};`.

- [ ] **Step 6: StreamWorker cpp (`streamworker.cpp`).** Substitutions:
  - Constructor: store `targetRate` into `m_targetRate` (guard `isValid()`): replace the `if (targetFps > 0) m_targetFps = targetFps;` line with `if (targetRate.isValid()) m_targetRate = targetRate;`.
  - `(frameIndex * 1000) / m_targetFps` (`:91`) → `m_targetRate.msForFrame(frameIndex)`.
  - `(m_internalFrameCount * 1000) / m_targetFps` (`:125`) → `m_targetRate.msForFrame(m_internalFrameCount)`.
  - `(*encCtx)->time_base = {1, m_targetFps};` (`:419`) → `(*encCtx)->time_base = {m_targetRate.den, m_targetRate.num};`
  - `(*encCtx)->framerate = {m_targetFps, 1};` (`:420`) → `(*encCtx)->framerate = {m_targetRate.num, m_targetRate.den};`
  - `kAudioSampleRate / m_targetFps` (`:546`) → `m_targetRate.samplesPerFrame(kAudioSampleRate)`.
  - The MPEG-2 exact-rate `switch (m_targetFps)` (`:404-417`) → `switch (m_targetRate.roundedFps())` AND change the guard to also warn for fractional: wrap so the warning fires when `m_targetRate.den != 1 || (rounded not in {24,25,30,50,60})`. Concretely replace the `switch` with:
```cpp
    const int rounded = m_targetRate.roundedFps();
    const bool exact = m_targetRate.den == 1
                       && (rounded == 24 || rounded == 25 || rounded == 30
                           || rounded == 50 || rounded == 60);
    if (!exact) {
        qWarning() << "Source" << m_sourceIndex << "rate" << m_targetRate.num << "/"
                   << m_targetRate.den
                   << "is not an exact MPEG-2 rate; the elementary stream will carry the"
                   << "nearest representable rate.";
    }
```

- [ ] **Step 7: Muxer (`muxer.h` + `muxer.cpp`).** In `muxer.h`: add `#include "framerate.h"`; change both `init(..., int fps, ...)` signatures' `int fps` → `FrameRate rate`. In `muxer.cpp`: change both definitions' `int fps` → `FrameRate rate`; replace `if (fps <= 0) fps = 30;` → `if (!rate.isValid()) rate = FrameRate{30, 1};`; replace `st->avg_frame_rate = {fps, 1};` → `st->avg_frame_rate = {rate.num, rate.den};` and `st->r_frame_rate = {fps, 1};` → `st->r_frame_rate = {rate.num, rate.den};`. Stream `time_base` stays `{1, 1000}`.

- [ ] **Step 8: Build the whole tree** — `cmake -S . -B build/srt >/dev/null && cmake --build build/srt`. Expected: clean. Fix any leftover `m_fps`/`m_targetFps`/`int fps` references the substitutions missed (grep `m_fps\b\|m_targetFps\b` in replaymanager/streamworker — should be gone). `setFps(int)` shim keeps UIManager + harnesses compiling.

- [ ] **Step 9: Run the helper unit tests + the record/native regression** — Run:
```bash
( cd build/srt && ctest -R "tst_heartbeat|tst_framerate" --output-on-failure )
( cd build/srt && ctest -L e2e -R "e2e_record_stereo|e2e_record_mono" --output-on-failure )
```
Expected: unit pass; both record e2e pass (integer 30 is byte-identical — encoder `{1,30}`/`{30,1}`, `msForFrame` = `f*1000/30`). If a record gate fails, the migration changed integer behavior — STOP and report.

- [ ] **Step 10: Format changed lines + commit**
```bash
git add recorder_engine/heartbeat.h recorder_engine/heartbeat.cpp tests/unit/tst_heartbeat.cpp \
        recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp \
        recorder_engine/streamworker.h recorder_engine/streamworker.cpp \
        recorder_engine/muxer.h recorder_engine/muxer.cpp
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add -A recorder_engine tests/unit
git commit -m "feat(fps): migrate recording engine to FrameRate (rational frame rate)"
```

---

### Task 3: Settings persistence (`fpsNum`/`fpsDen`, backward-compatible)

**Files:** `settingsmanager.h`, `settingsmanager.cpp`. (And `tst_settingsmanager.cpp` if it exists — extend; else skip the unit step.)

- [ ] **Step 1:** In `settingsmanager.h`, replace `int fps = 30;` (AppSettings) with:
```cpp
    int fpsNum = 30;
    int fpsDen = 1;
```
- [ ] **Step 2:** In `settingsmanager.cpp` save, replace `root["fps"] = settings.fps;` with:
```cpp
    root["fpsNum"] = settings.fpsNum;
    root["fpsDen"] = settings.fpsDen;
```
- [ ] **Step 3:** In `settingsmanager.cpp` load, replace `settings.fps = root["fps"].toInt(settings.fps);` with (legacy `"fps"` int loads as the numerator, den defaults to 1):
```cpp
    settings.fpsNum = root["fpsNum"].toInt(root["fps"].toInt(settings.fpsNum));
    settings.fpsDen = root["fpsDen"].toInt(settings.fpsDen);
    if (settings.fpsNum <= 0) settings.fpsNum = 30;
    if (settings.fpsDen <= 0) settings.fpsDen = 1;
```
- [ ] **Step 4:** If `tests/unit/tst_settingsmanager.cpp` exists, add a round-trip case: set `fpsNum=30000,fpsDen=1001`, save, load into a fresh AppSettings, assert both preserved; and a legacy case (write a JSON with `"fps":25`, load, assert `fpsNum==25,fpsDen==1`). Build + run `tst_settingsmanager`. (If the test file doesn't exist, note it and rely on the e2e + the UI round-trip instead.)
- [ ] **Step 5:** Build `OpenLiveReplay` (the app uses `m_currentSettings.fps` in UIManager — that reference is updated in Task 4, so the app may not fully build until Task 4; if so, just `cmake --build build/srt --target olr_test_core` here and defer the app build to Task 4). Commit:
```bash
git add settingsmanager.h settingsmanager.cpp tests/unit/tst_settingsmanager.cpp 2>/dev/null
git commit -m "feat(fps): persist frame rate as fpsNum/fpsDen (backward-compatible)"
```

---

### Task 4: UIManager + QML — preset frame-rate selector

**Files:** `uimanager.h`, `uimanager.cpp`, `Main.qml`.

- [ ] **Step 1: uimanager.h** — add `#include "recorder_engine/framerate.h"`. Keep `Q_PROPERTY(int recordFps READ recordFps NOTIFY frameRateChanged)` (now read-only display). Add:
```cpp
    Q_PROPERTY(QStringList frameRatePresetLabels READ frameRatePresetLabels CONSTANT)
    Q_PROPERTY(int frameRateIndex READ frameRateIndex WRITE setFrameRateIndex NOTIFY frameRateChanged)
```
plus the getters/setter declarations and a `void frameRateChanged();` signal (rename the existing `recordFpsChanged` to `frameRateChanged`, or add the new signal and emit both). Add `Q_INVOKABLE` not needed.

- [ ] **Step 2: uimanager.cpp** — implement:
```cpp
QStringList UIManager::frameRatePresetLabels() const {
    QStringList out;
    for (const FrameRatePreset& p : frameRatePresets()) out << QString::fromLatin1(p.label);
    return out;
}
int UIManager::recordFps() const {
    return FrameRate{m_currentSettings.fpsNum, m_currentSettings.fpsDen}.roundedFps();
}
int UIManager::frameRateIndex() const {
    const FrameRate cur{m_currentSettings.fpsNum, m_currentSettings.fpsDen};
    const auto& presets = frameRatePresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].rate == cur) return int(i);
    return -1;  // a non-preset rate from a loaded config
}
void UIManager::setFrameRateIndex(int index) {
    if (m_replayManager && m_replayManager->isRecording()) return;
    const auto& presets = frameRatePresets();
    if (index < 0 || index >= int(presets.size())) return;
    const FrameRate r = presets[size_t(index)].rate;
    if (m_currentSettings.fpsNum == r.num && m_currentSettings.fpsDen == r.den) return;
    m_currentSettings.fpsNum = r.num;
    m_currentSettings.fpsDen = r.den;
    m_replayManager->setFrameRate(r);
    if (m_transport) m_transport->setFps(r.roundedFps());
    emit frameRateChanged();
}
```
Replace the body of the old `setRecordFps(int)` to forward to a rate (or delete it + the property write — but keep `recordFps()` read getter). Update the ctor / any place that called `m_replayManager->setFps(m_currentSettings.fps)` to use `setFrameRate(FrameRate{fpsNum,fpsDen})`. Grep `m_currentSettings.fps\b` and `recordFpsChanged` and fix all (there are uses at the old `:654` getter and `:1208-1223` setter, plus possibly a startup `setFps`).

- [ ] **Step 3: Main.qml** — replace the `SpinBox {...}` (`:1200-1209`) with a ComboBox:
```qml
ComboBox {
    model: appWindow.uiManagerRef.frameRatePresetLabels
    currentIndex: appWindow.uiManagerRef.frameRateIndex
    enabled: !appWindow.uiManagerRef.isRecording
    onActivated: appWindow.uiManagerRef.frameRateIndex = currentIndex
    Layout.preferredWidth: 96
}
```
(match the surrounding layout property the SpinBox used.)

- [ ] **Step 4: Build the app** — `cmake --build build/srt --target OpenLiveReplay`. Expected: clean (QML compiles via qmlcachegen; no binding errors on the new properties). Fix any remaining `m_currentSettings.fps`/`recordFpsChanged`/`setRecordFps` references.

- [ ] **Step 5: Commit**
```bash
git add uimanager.h uimanager.cpp Main.qml
git commit -m "feat(fps): UI frame-rate preset selector (29.97/59.94/...) via FrameRate"
```

---

### Task 5: Harness `--fps` accepts a rational

**Files:** `tests/e2e/record_harness.cpp`, `tests/e2e/sync_harness.cpp`.

- [ ] **Step 1:** In each, add `#include "recorder_engine/framerate.h"`. Replace `const int fps = argValue(args, ..., "30").toInt();` + `rm.setFps(fps);` with:
```cpp
    const FrameRate frameRate = parseFrameRate(argValue(args, QStringLiteral("--fps"), QStringLiteral("30")));
    ...
    rm.setFrameRate(frameRate);
```
(keep using `fps` for any other place that needs an int — e.g. `rm.setVideoWidth`… none; if `fps` was used elsewhere, replace with `frameRate.roundedFps()`). Grep each harness for other `fps` uses.

- [ ] **Step 2: Build** — `cmake --build build/srt --target record_harness sync_harness`. Expected: clean.

- [ ] **Step 3: Commit**
```bash
git add tests/e2e/record_harness.cpp tests/e2e/sync_harness.cpp
git commit -m "test(fps): harnesses accept a rational --fps (29.97 / 30000/1001)"
```

---

### Task 6: E2e gate — record a true 29.97

**Files:** `tests/e2e/run_record_e2e.sh`, `tests/e2e/CMakeLists.txt`.

- [ ] **Step 1:** READ `tests/e2e/run_record_e2e.sh` to see how it invokes the harness (`--fps 30`) and probes with ffprobe. Add a scenario branch keyed on the existing scenario arg (it already takes `stereo`/`mono` as `$2`). Add a `fps2997` scenario that records with `--fps 29.97` and asserts the output's avg_frame_rate. If the script is structured as a single flow with a scenario switch, add the branch; otherwise add a small dedicated section. The assertion:
```bash
# After producing the MKV at --fps 29.97:
AVG=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 "$MKV")
# AVG is like "30000/1001"; compute the float and require 29.9 < r < 30.0 (29.97 passes, 30.0 fails).
RATE=$(awk -F/ 'BEGIN{} {printf "%.4f", $1/$2}' <<<"$AVG")
awk -v r="$RATE" 'BEGIN{exit !(r > 29.9 && r < 30.0)}' \
    || { echo "FAIL: avg_frame_rate=$AVG ($RATE) not a true 29.97 (got integer 30?)"; exit 1; }
echo "PASS: recorded a true 29.97 (avg_frame_rate=$AVG)"
```
Keep the existing track-count / A/V checks. (The harness records a single source to multiple views; `-select_streams v:0` is the first view.)

- [ ] **Step 2:** Register in `tests/e2e/CMakeLists.txt` next to `e2e_record_stereo`/`e2e_record_mono`:
```cmake
add_test(NAME e2e_record_2997
    COMMAND bash "${_driver}" "$<TARGET_FILE:record_harness>" fps2997 23458)
set_tests_properties(e2e_record_2997 PROPERTIES LABELS "e2e" TIMEOUT 120 RUN_SERIAL TRUE)
```
(`${_driver}` is `run_record_e2e.sh`; base port 23458 = 23456/57 are stereo/mono, 58 is free.)

- [ ] **Step 3: Reconfigure + run** — `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target record_harness && ( cd build/srt && ctest -R e2e_record_2997 --output-on-failure )`. Expected: PASS with `avg_frame_rate=30000/1001`. If it reports 30/1, the muxer/encoder didn't carry the rational rate — report (do NOT loosen). ~15s.

- [ ] **Step 4: Commit**
```bash
git add tests/e2e/run_record_e2e.sh tests/e2e/CMakeLists.txt
git commit -m "test(fps): e2e_record_2997 — prove a true 29.97 recording (avg_frame_rate)"
```

---

### Task 7: Docs

**Files:** `tests/e2e/SRT_README.md` (or a brief note in the record-e2e docs).

- [ ] **Step 1:** Append a short "P1: rational frame rate" note documenting: `FrameRate {num,den}` end-to-end, encoder `time_base={den,num}`, muxer `avg/r_frame_rate={num,den}` → MKV `DefaultDuration`; settings `fpsNum/fpsDen` (back-compat); the preset UI; `e2e_record_2997` proof; and the deferred items (drop-frame TC, rational playback stepping). Commit:
```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(fps): document the rational frame-rate (P1) migration"
```

---

## After all tasks
- `( cd build/srt && ctest -L unit --output-on-failure )` — incl. `tst_framerate`, `tst_heartbeat`.
- `( cd build/srt && ctest -L e2e --output-on-failure )` — incl. `e2e_record_2997`; record + playback unchanged.
- `( cd build/srt && ctest -L srt --output-on-failure )` + `ctest -L native-apple-ingest` — all record at int 30, must stay green (byte-identical).
- `grep -rn "m_fps\b\|m_targetFps\b\|\.fps\b" recorder_engine settingsmanager.* uimanager.* | grep -v frameRate` — confirm no stray integer-fps members remain.
- Final code review over the branch (focus: encoder timebase `{den,num}` correctness, integer-30 byte-identity, the FrameRate overflow headroom, settings back-compat).
- Rebase onto latest `feat/heartbeat-decouple` (or `main` if #49 merged); open a STACKED PR (`--base feat/heartbeat-decouple`). **Do NOT push** beyond the autonomous mandate — the user authorized pushing the stacked PR, so push + open it.
