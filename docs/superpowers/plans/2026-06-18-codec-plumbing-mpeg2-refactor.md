# Codec Plumbing + MPEG-2 Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a `VideoCodecChoice` value and thread it end-to-end (settings → ReplayManager → Muxer → StreamWorker → blue encoder → UI) with only the existing MPEG-2 path behind it, so later plans can add hardware H.264 without touching the plumbing.

**Architecture:** Add a header-only `VideoCodecChoice` enum. Persist it in `AppSettings`. Pass it into `Muxer::init` (which selects the stream `codec_id` and optionally attaches video extradata) and into the recording workers (which select an encoder). In this plan the H.264 branches are wired but inert — `Mpeg2Software` is the only functional path, so behavior is unchanged.

**Tech Stack:** C++17, Qt 6 (Core/Test), FFmpeg (libavformat/libavcodec/libavutil), CMake + Ninja.

## Global Constraints

- **No behavior change in this plan.** Default codec is `VideoCodecChoice::Mpeg2Software`; all existing recordings/tests must behave identically. The H.264 branch is implemented as plumbing only and is verified behaviorally in a later plan.
- **All-intra, both codecs** (existing `gop_size = 1` for MPEG-2 is untouched).
- **Enum is header-only** (inline functions) — no new CMake library/source wiring in this plan.
- **Include root is the project root** (existing headers include e.g. `"recorder_engine/muxer.h"`, `"playback/output/outputtargetassignment.h"`).
- Build (run from the worktree root):
  - Configure: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos`
  - Build a target: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`
  - Run a test: `ctest --test-dir build/claude-debug -R <name> -V`
  - Never reuse the stale `build/Qt_6_10_1_for_macOS-*` dirs; use `build/claude-debug`.

---

## File Structure

- **Create** `recorder_engine/codec/videocodecchoice.h` — the enum + `videoCodecToString` / `videoCodecFromString` (header-only).
- **Create** `tests/unit/tst_videocodecchoice.cpp` — enum/string-helper tests.
- **Modify** `settingsmanager.h` — add `AppSettings::videoCodec`, include the enum header.
- **Modify** `settingsmanager.cpp` — save/load the `"videoCodec"` key.
- **Modify** `tests/unit/tst_settingsmanager.cpp` — round-trip + default/legacy coverage.
- **Modify** `recorder_engine/muxer.h` / `recorder_engine/muxer.cpp` — `init()` gains `codec` + `videoExtradata`; stream `codec_id`/extradata selected from them.
- **Modify** `tests/unit/tst_muxer.cpp` — assert default codec_id stays MPEG-2.
- **Modify** `recorder_engine/replaymanager.h` / `.cpp` — store the codec, pass to muxer, branch the blue encoder.
- **Modify** `recorder_engine/streamworker.h` / `.cpp` — constructor takes the codec, `setupEncoder` branches.
- **Modify** `uimanager.h` / `uimanager.cpp` — `recordCodec` QML property, push to ReplayManager, apply on load.
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_videocodecchoice`.

---

## Task 1: VideoCodecChoice enum + string helpers

**Files:**
- Create: `recorder_engine/codec/videocodecchoice.h`
- Test: `tests/unit/tst_videocodecchoice.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class VideoCodecChoice { Mpeg2Software, H264Hardware };`
  - `inline QString videoCodecToString(VideoCodecChoice);` → `"mpeg2"` / `"h264"`
  - `inline VideoCodecChoice videoCodecFromString(const QString&, VideoCodecChoice fallback = VideoCodecChoice::Mpeg2Software);`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_videocodecchoice.cpp`:

```cpp
// Unit tests for VideoCodecChoice enum <-> string helpers.
#include <QtTest>

#include "recorder_engine/codec/videocodecchoice.h"

class TestVideoCodecChoice : public QObject {
    Q_OBJECT
private slots:
    void toStringMapsBothValues();
    void fromStringMapsKnownValues();
    void fromStringUsesFallbackForUnknown();
    void roundTrips();
};

void TestVideoCodecChoice::toStringMapsBothValues() {
    QCOMPARE(videoCodecToString(VideoCodecChoice::Mpeg2Software), QStringLiteral("mpeg2"));
    QCOMPARE(videoCodecToString(VideoCodecChoice::H264Hardware), QStringLiteral("h264"));
}

void TestVideoCodecChoice::fromStringMapsKnownValues() {
    QCOMPARE(videoCodecFromString(QStringLiteral("mpeg2")), VideoCodecChoice::Mpeg2Software);
    QCOMPARE(videoCodecFromString(QStringLiteral("h264")), VideoCodecChoice::H264Hardware);
}

void TestVideoCodecChoice::fromStringUsesFallbackForUnknown() {
    QCOMPARE(videoCodecFromString(QStringLiteral("")), VideoCodecChoice::Mpeg2Software);
    QCOMPARE(videoCodecFromString(QStringLiteral("vp9")), VideoCodecChoice::Mpeg2Software);
    QCOMPARE(videoCodecFromString(QStringLiteral("vp9"), VideoCodecChoice::H264Hardware),
             VideoCodecChoice::H264Hardware);
}

void TestVideoCodecChoice::roundTrips() {
    for (auto c : {VideoCodecChoice::Mpeg2Software, VideoCodecChoice::H264Hardware})
        QCOMPARE(videoCodecFromString(videoCodecToString(c)), c);
}

QTEST_GUILESS_MAIN(TestVideoCodecChoice)
#include "tst_videocodecchoice.moc"
```

Register it in `tests/unit/CMakeLists.txt` — add after the `olr_add_unit_test(tst_settingsmanager olr_test_core)` line:

```cmake
olr_add_unit_test(tst_videocodecchoice)
```

(No library argument: the enum header is header-only, so only `Qt6::Test` is needed, which `olr_add_unit_test` always links.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos && $HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_videocodecchoice`
Expected: FAIL to compile — `recorder_engine/codec/videocodecchoice.h` not found.

- [ ] **Step 3: Write minimal implementation**

Create `recorder_engine/codec/videocodecchoice.h`:

```cpp
#ifndef VIDEOCODECCHOICE_H
#define VIDEOCODECCHOICE_H

#include <QString>

// The recording video codec the user can select.
//   Mpeg2Software — FFmpeg MPEG-2, intra-only, software (the historical path).
//   H264Hardware  — OS hardware H.264 (VideoToolbox / MediaFoundation), intra-only.
enum class VideoCodecChoice { Mpeg2Software, H264Hardware };

inline QString videoCodecToString(VideoCodecChoice codec) {
    return codec == VideoCodecChoice::H264Hardware ? QStringLiteral("h264")
                                                   : QStringLiteral("mpeg2");
}

inline VideoCodecChoice videoCodecFromString(
    const QString& value, VideoCodecChoice fallback = VideoCodecChoice::Mpeg2Software) {
    if (value == QStringLiteral("h264")) return VideoCodecChoice::H264Hardware;
    if (value == QStringLiteral("mpeg2")) return VideoCodecChoice::Mpeg2Software;
    return fallback;
}

#endif // VIDEOCODECCHOICE_H
```

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_videocodecchoice && ctest --test-dir build/claude-debug -R tst_videocodecchoice -V`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/codec/videocodecchoice.h tests/unit/tst_videocodecchoice.cpp tests/unit/CMakeLists.txt
git commit -m "feat: add VideoCodecChoice enum and string helpers"
```

---

## Task 2: Persist videoCodec in AppSettings

**Files:**
- Modify: `settingsmanager.h` (add field + include), `settingsmanager.cpp:131-141` (save), `settingsmanager.cpp:228-246` (load)
- Test: `tests/unit/tst_settingsmanager.cpp`

**Interfaces:**
- Consumes: `VideoCodecChoice`, `videoCodecToString`, `videoCodecFromString` (Task 1)
- Produces: `AppSettings::videoCodec` (default `VideoCodecChoice::Mpeg2Software`), persisted under JSON key `"videoCodec"`.

- [ ] **Step 1: Write the failing test**

In `tests/unit/tst_settingsmanager.cpp`, add to `sampleSettings()` (after the `s.audioOutputLatencyMs = 180;` line, ~line 36):

```cpp
    s.videoCodec = VideoCodecChoice::H264Hardware;
```

Add to `roundTripPreservesEverything()` (after the `audioOutputLatencyMs` QCOMPARE, ~line 103):

```cpp
    QCOMPARE(out.videoCodec, in.videoCodec);
```

Add a new test slot declaration in the `private slots:` block:

```cpp
    void loadDefaultsVideoCodecToMpeg2WhenMissing();
```

Add the new test body before `QTEST_GUILESS_MAIN`:

```cpp
void TestSettingsManager::loadDefaultsVideoCodecToMpeg2WhenMissing() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(QJsonObject{{"fps", 30}}).toJson());  // no videoCodec key
    f.close();

    SettingsManager mgr;
    AppSettings out;
    QVERIFY(mgr.load(path, out));
    QCOMPARE(out.videoCodec, VideoCodecChoice::Mpeg2Software);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_settingsmanager`
Expected: FAIL to compile — `AppSettings` has no member `videoCodec`.

- [ ] **Step 3: Write minimal implementation**

In `settingsmanager.h`, add the include near the top (after `#include "playback/output/outputtargetassignment.h"`):

```cpp
#include "recorder_engine/codec/videocodecchoice.h"
```

In `struct AppSettings`, add after `int fps = 30;` (line 34):

```cpp
    VideoCodecChoice videoCodec = VideoCodecChoice::Mpeg2Software;
```

In `settingsmanager.cpp` save, after `root["fps"] = settings.fps;` (line 135):

```cpp
    root["videoCodec"] = videoCodecToString(settings.videoCodec);
```

In `settingsmanager.cpp` load, after `settings.fps = root["fps"].toInt(settings.fps);` (line 232):

```cpp
    settings.videoCodec = videoCodecFromString(root["videoCodec"].toString(), settings.videoCodec);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_settingsmanager && ctest --test-dir build/claude-debug -R tst_settingsmanager -V`
Expected: PASS (all existing tests + the new `loadDefaultsVideoCodecToMpeg2WhenMissing`).

- [ ] **Step 5: Commit**

```bash
git add settingsmanager.h settingsmanager.cpp tests/unit/tst_settingsmanager.cpp
git commit -m "feat: persist videoCodec in AppSettings"
```

---

## Task 3: Muxer accepts a codec + video extradata

**Files:**
- Modify: `recorder_engine/muxer.h` (both `init` declarations), `recorder_engine/muxer.cpp:11-14` (delegating overload), `recorder_engine/muxer.cpp:44` (codec_id selection)
- Test: `tests/unit/tst_muxer.cpp`

**Interfaces:**
- Consumes: `VideoCodecChoice` (Task 1)
- Produces: `Muxer::init(..., int audioSampleRate = 48000, int audioChannels = 2, VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software, const QByteArray& videoExtradata = {})` on both overloads. Default selects `AV_CODEC_ID_MPEG2VIDEO` (unchanged); `H264Hardware` selects `AV_CODEC_ID_H264` and attaches `videoExtradata` as `codecpar->extradata` when non-empty.

> Note: the H.264 branch is plumbing only here. Its behavioral correctness (real `avcC`, playable file) is verified in the next plan's avcC round-trip test, where a real encoder produces the parameter sets. This task only proves the default path is unchanged and the new params compile/thread through.

- [ ] **Step 1: Write the failing test**

In `tests/unit/tst_muxer.cpp`, add to `initBuildsTrackLayout()` before `m.close();` (~line 49):

```cpp
    // Default codec must remain MPEG-2 (no behavior change).
    QCOMPARE(m.getStream(0)->codecpar->codec_id, AV_CODEC_ID_MPEG2VIDEO);
    QCOMPARE(m.getStream(1)->codecpar->codec_id, AV_CODEC_ID_MPEG2VIDEO);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_muxer`
Expected: At this point it still compiles and PASSES (default is already MPEG-2). This step documents the regression guard; proceed to add the parameters, then re-run to confirm it still passes.

- [ ] **Step 3: Write minimal implementation**

In `recorder_engine/muxer.h`, add the include after the `extern "C"` block (after line 24):

```cpp
#include "recorder_engine/codec/videocodecchoice.h"
```

Change the two `init` declarations (lines 31-35) to:

```cpp
    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
             int audioSampleRate = 48000, int audioChannels = 2,
             VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software, const QByteArray& videoExtradata = {});
    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
             const QStringList& telemetryFeedIds, const QStringList& telemetryFeedNames,
             int audioSampleRate = 48000, int audioChannels = 2,
             VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software, const QByteArray& videoExtradata = {});
```

In `recorder_engine/muxer.cpp`, change the delegating overload (lines 11-14):

```cpp
bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
                 int audioSampleRate, int audioChannels,
                 VideoCodecChoice codec, const QByteArray& videoExtradata) {
    return init(filename, videoTrackCount, width, height, fps, streamNames, {}, {}, audioSampleRate, audioChannels,
                codec, videoExtradata);
}
```

Change the main overload signature (lines 16-18) to add the params:

```cpp
bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
                 const QStringList& telemetryFeedIds, const QStringList& telemetryFeedNames,
                 int audioSampleRate, int audioChannels,
                 VideoCodecChoice codec, const QByteArray& videoExtradata) {
```

Replace the hardcoded codec_id (line 44) with:

```cpp
        st->codecpar->codec_id = (codec == VideoCodecChoice::H264Hardware)
                                     ? AV_CODEC_ID_H264
                                     : AV_CODEC_ID_MPEG2VIDEO;
        if (codec == VideoCodecChoice::H264Hardware && !videoExtradata.isEmpty()) {
            st->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(videoExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (st->codecpar->extradata) {
                memcpy(st->codecpar->extradata, videoExtradata.constData(), videoExtradata.size());
                st->codecpar->extradata_size = videoExtradata.size();
            }
        }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_muxer && ctest --test-dir build/claude-debug -R tst_muxer -V`
Expected: PASS (all muxer tests, including the new codec_id assertions).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/muxer.h recorder_engine/muxer.cpp tests/unit/tst_muxer.cpp
git commit -m "feat: Muxer::init accepts codec choice and video extradata"
```

---

## Task 4: Thread the codec through the recording engine

**Files:**
- Modify: `recorder_engine/replaymanager.h` (member + setter/getter), `recorder_engine/replaymanager.cpp:88` (blue encoder branch), `recorder_engine/replaymanager.cpp:181-183` (muxer init call), `recorder_engine/replaymanager.cpp:212-214` (worker construction)
- Modify: `recorder_engine/streamworker.h:55-56` (constructor), `recorder_engine/streamworker.cpp:468` (encoder branch)

**Interfaces:**
- Consumes: `VideoCodecChoice` (Task 1); `Muxer::init(..., codec, videoExtradata)` (Task 3)
- Produces:
  - `ReplayManager::setVideoCodec(VideoCodecChoice)` / `VideoCodecChoice videoCodec() const`
  - `StreamWorker(..., int targetFps, VideoCodecChoice codec, QObject* parent = nullptr)`

> This task is pure wiring of already-tested units. The default (`Mpeg2Software`) path is unchanged, so it is verified by rebuilding and re-running the full unit suite (no regression). The H.264 branches log and fail cleanly because no hardware encoder exists yet (added in the next plan).

- [ ] **Step 1: Add the ReplayManager member, setter, and getter**

In `recorder_engine/replaymanager.h`, add the include near the other engine includes (top of file):

```cpp
#include "recorder_engine/codec/videocodecchoice.h"
```

Add the setter/getter after `void setFps(int fps) { m_fps = fps; }` (line 66):

```cpp
    void setVideoCodec(VideoCodecChoice codec) { m_videoCodec = codec; }
    VideoCodecChoice videoCodec() const { return m_videoCodec; }
```

Add the member after `int m_fps = 30;` (line 136):

```cpp
    VideoCodecChoice m_videoCodec = VideoCodecChoice::Mpeg2Software;
```

- [ ] **Step 2: Pass the codec to the muxer and workers, branch the blue encoder**

In `recorder_engine/replaymanager.cpp` `startRecording()`, replace the muxer init (lines 180-183):

```cpp
    const bool muxerReady = m_telemetryFeedIds.isEmpty()
        ? m_muxer->init(m_sessionFileName, m_viewCount, m_videoWidth, m_videoHeight, m_fps, m_viewNames,
                        48000, 2, m_videoCodec)
        : m_muxer->init(m_sessionFileName, m_viewCount, m_videoWidth, m_videoHeight, m_fps, m_viewNames,
                        m_telemetryFeedIds, m_telemetryFeedNames, 48000, 2, m_videoCodec);
```

Replace the worker construction (lines 212-214):

```cpp
        StreamWorker* worker = new StreamWorker(
            m_sourceUrls[s], s, m_muxer, m_clock,
            m_videoWidth, m_videoHeight, m_fps, m_videoCodec);
```

In `setupBlueEncoder()`, replace the first two lines (lines 89-90) with a guard + the existing MPEG-2 lookup:

```cpp
    if (m_videoCodec == VideoCodecChoice::H264Hardware) {
        qWarning() << "ReplayManager: H.264 hardware encode not yet implemented; "
                      "select MPEG-2 (this path is wired in a later plan).";
        return false;
    }
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!encoder) return false;
```

- [ ] **Step 3: Add the StreamWorker constructor parameter and encoder branch**

In `recorder_engine/streamworker.h`, add the include near the FFmpeg `extern "C"` block (after line 25):

```cpp
#include "recorder_engine/codec/videocodecchoice.h"
```

Change the constructor declaration (lines 55-56):

```cpp
    StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock* clock,
                 int targetWidth, int targetHeight, int targetFps,
                 VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software, QObject* parent = nullptr);
```

Add a member near `int m_targetFps = 30;` (line 180):

```cpp
    VideoCodecChoice m_videoCodec = VideoCodecChoice::Mpeg2Software;
```

In `recorder_engine/streamworker.cpp`, update the constructor definition to store `codec` into `m_videoCodec` and forward `parent` (match the existing constructor's initializer list / body — assign `m_videoCodec = codec;`).

In `setupEncoder()`, add a guard at the very top (before line 468 `const AVCodec* encoder = ...`):

```cpp
    if (m_videoCodec == VideoCodecChoice::H264Hardware) {
        qWarning() << "Source" << m_sourceIndex
                   << "H.264 hardware encode not yet implemented; select MPEG-2.";
        return false;
    }
```

- [ ] **Step 4: Build and run the full unit suite (regression)**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug && ctest --test-dir build/claude-debug -L unit`
Expected: PASS — all unit tests green; default MPEG-2 recording path unchanged.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp recorder_engine/streamworker.h recorder_engine/streamworker.cpp
git commit -m "feat: thread VideoCodecChoice through the recording engine (MPEG-2 only)"
```

---

## Task 5: Expose recordCodec to the UI

**Files:**
- Modify: `uimanager.h:40-41` (Q_PROPERTY block), accessor declarations (~line 109), signals (~line 267)
- Modify: `uimanager.cpp` (accessor impls after `setRecordHeight`, ~line 1330; apply-on-load push, ~line 2094)

**Interfaces:**
- Consumes: `AppSettings::videoCodec` (Task 2); `ReplayManager::setVideoCodec` (Task 4); `videoCodecToString`/`videoCodecFromString` (Task 1)
- Produces: QML property `recordCodec` (`"mpeg2"`/`"h264"`), `recordCodecChanged()` signal.

> Verified by rebuild + the settings round-trip already covered in Task 2 (load → `m_currentSettings.videoCodec` → save). UIManager has no unit-test harness in this repo, consistent with the existing `recordWidth`/`recordHeight` accessors.

- [ ] **Step 1: Declare the property, accessor, and signal**

In `uimanager.h`, add after the `recordHeight` Q_PROPERTY (line 41):

```cpp
    Q_PROPERTY(QString recordCodec READ recordCodec WRITE setRecordCodec NOTIFY recordCodecChanged)
```

Add the accessor declarations next to `int recordWidth() const;` (~line 109). Place both the getter and the setter (mirror where `setRecordWidth`/`setRecordHeight` are declared):

```cpp
    QString recordCodec() const;
    void setRecordCodec(const QString& codec);
```

Add the signal next to `void recordWidthChanged();` (~line 267):

```cpp
    void recordCodecChanged();
```

Add the include at the top of `uimanager.h` (with the other project includes):

```cpp
#include "recorder_engine/codec/videocodecchoice.h"
```

- [ ] **Step 2: Implement the accessors**

In `uimanager.cpp`, after the `setRecordHeight` implementation (the block ending ~line 1330), add:

```cpp
QString UIManager::recordCodec() const {
    return videoCodecToString(m_currentSettings.videoCodec);
}

void UIManager::setRecordCodec(const QString& codec) {
    const VideoCodecChoice next = videoCodecFromString(codec, m_currentSettings.videoCodec);
    if (m_currentSettings.videoCodec != next) {
        m_currentSettings.videoCodec = next;
        m_replayManager->setVideoCodec(next);
        emit recordCodecChanged();
    }
}
```

In the apply-on-load block, after `m_replayManager->setVideoHeight(m_currentSettings.videoHeight);` (~line 2094), add:

```cpp
        m_replayManager->setVideoCodec(m_currentSettings.videoCodec);
```

- [ ] **Step 3: Build the app target**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug OpenLiveReplay`
Expected: PASS — compiles and links; the moc picks up the new property/signal.

- [ ] **Step 4: Run the full unit suite (regression)**

Run: `ctest --test-dir build/claude-debug -L unit`
Expected: PASS — no regressions.

- [ ] **Step 5: Commit**

```bash
git add uimanager.h uimanager.cpp
git commit -m "feat: expose recordCodec property on UIManager"
```

---

## Self-Review

**Spec coverage (this plan = subsystem 1 of 4 from the design doc):**
- `VideoCodecChoice` enum threaded settings → ReplayManager → Muxer → StreamWorker → blue encoder → UI → Task 1-5. ✓
- `AppSettings::videoCodec` persisted → Task 2. ✓
- Muxer `codec_id`/extradata selection → Task 3. ✓ (H.264 behavioral verification deferred to Plan 2's avcC round-trip, noted in Task 3.)
- Native encoder, playback H.264 decode, benchmark engine, settings UI/gating → **out of scope for this plan** (Plans 2-4). Not gaps.

**Placeholder scan:** No TBD/TODO; every code step shows complete code. The H.264 branches are intentional, fully-written guards (qWarning + return false / conditional codec_id), not placeholders.

**Type consistency:** `VideoCodecChoice` / `videoCodecToString` / `videoCodecFromString` (Task 1) used identically in Tasks 2-5. `Muxer::init` trailing params `(codec, videoExtradata)` match between header (Task 3 Step 3) and the ReplayManager call site (Task 4 Step 2). `StreamWorker(..., VideoCodecChoice codec, QObject* parent)` declaration (Task 4 Step 3) matches the construction call (Task 4 Step 2), which passes the codec and relies on the defaulted `parent`.
