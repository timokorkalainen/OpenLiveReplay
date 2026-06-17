# Broadcast Output Bus Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the backend clean-output-bus foundation so Qt preview displays backend-produced frame-perfect clean bus frames instead of being the source of playback output truth.

**Architecture:** Add Qt-independent media frame types, a rational output frame clock, a per-feed media cache, a clean output bus engine, and Qt preview adapters. This plan deliberately stops before NDI, DeckLink, ST 2110, OMT, and AJA target implementations; those adapters consume this foundation through target assignments in later plans.

**Tech Stack:** C++17, Qt Core/Gui/Multimedia, Qt Test, CMake, existing FFmpeg-backed playback code.

---

## Scope

This is the first independently testable implementation slice from `docs/superpowers/specs/2026-06-17-broadcast-output-bus-design.md`.

In scope:

- backend media-frame model;
- rational output frame clock;
- target-assignment model;
- per-feed media cache;
- clean feed and PGM bus rendering;
- clean multiview YUV420P compositor;
- Qt preview sink adapter with an engine-to-preview contract test;
- unit tests for frame-perfect pause/step behavior, audio routing, and target assignment independence.

Out of scope for this plan:

- NDI sender implementation;
- DeckLink output implementation;
- DeckLink IP/ST 2110 implementation;
- OMT/AJA adapters;
- full software ST 2110 sender;
- scrub audio during pause/jog/reverse/shuttle;
- production `PlaybackWorker` inversion from QVideoFrame-first delivery to decode/cache/output-bus delivery;
- mezzanine recording format changes.

## File Structure

Create a focused output-bus module under `playback/output/`:

- `playback/output/outputtypes.h`
  Small value types shared by all output-bus code: `FrameRate`, pixel/sample format enums, bus ids, target kinds, and playback snapshots.

- `playback/output/mediaframe.h`
  Backend-native `MediaVideoFrame`, `MediaAudioFrame`, and helpers for clean black/blue YUV420P frames and silent PCM.

- `playback/output/outputframeclock.h` / `playback/output/outputframeclock.cpp`
  Rational frame clock and playhead sampling math.

- `playback/output/outputtargetassignment.h` / `playback/output/outputtargetassignment.cpp`
  Physical/network target assignment model. It is only a model in this plan; real NDI/DeckLink sinks come later.

- `playback/output/outputframecache.h` / `playback/output/outputframecache.cpp`
  Per-feed decoded video/audio cache queried by playhead position and sample range. This is independent of FFmpeg so it can be unit-tested with synthetic frames.

- `playback/output/yuv420pcompositor.h` / `playback/output/yuv420pcompositor.cpp`
  Clean multiview compositor for YUV420P frames.

- `playback/output/outputbusengine.h` / `playback/output/outputbusengine.cpp`
  Deterministic bus renderer for feed, PGM, and multiview output frames.

- `playback/output/qtpreviewsink.h` / `playback/output/qtpreviewsink.cpp`
  Adapter from `MediaVideoFrame` to existing `FrameProvider` / `QVideoFrame`.

Modify existing build/test files:

- `CMakeLists.txt` — add new production sources to `qt_add_qml_module(OpenLiveReplay SOURCES ...)`.
- `tests/CMakeLists.txt` — add new output-bus sources to `olr_test_playback`.
- `tests/unit/CMakeLists.txt` — register new unit tests.

Create tests:

- `tests/unit/tst_outputframeclock.cpp`
- `tests/unit/tst_outputtargetassignment.cpp`
- `tests/unit/tst_outputframecache.cpp`
- `tests/unit/tst_yuv420pcompositor.cpp`
- `tests/unit/tst_outputbusengine.cpp`
- `tests/unit/tst_qtpreviewsink.cpp`

---

### Task 1: Output Types And Frame Clock

**Files:**
- Create: `playback/output/outputtypes.h`
- Create: `playback/output/outputframeclock.h`
- Create: `playback/output/outputframeclock.cpp`
- Create: `tests/unit/tst_outputframeclock.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing frame-clock test**

Create `tests/unit/tst_outputframeclock.cpp`:

```cpp
#include <QtTest>

#include "playback/output/outputframeclock.h"

class TestOutputFrameClock : public QObject {
    Q_OBJECT
private slots:
    void frameRateRejectsInvalidValues();
    void frameIndexToPlayheadMsUsesRationalMath();
    void pausedPlayheadRepeatsSameSourceFrame();
    void oneXForwardAdvancesByFrameDuration();
    void shuttleCanMoveBackwardWithoutStoppingOutputTicks();
};

void TestOutputFrameClock::frameRateRejectsInvalidValues() {
    QVERIFY(!FrameRate::fromFraction(0, 1).isValid());
    QVERIFY(!FrameRate::fromFraction(30000, 0).isValid());
    QVERIFY(FrameRate::fromFraction(30000, 1001).isValid());
}

void TestOutputFrameClock::frameIndexToPlayheadMsUsesRationalMath() {
    const OutputFrameClock clock(FrameRate::fromFraction(30000, 1001));
    QCOMPARE(clock.frameIndexToPlayheadMs(0), qint64(0));
    QCOMPARE(clock.frameIndexToPlayheadMs(30), qint64(1001));
    QCOMPARE(clock.frameIndexToPlayheadMs(60), qint64(2002));
}

void TestOutputFrameClock::pausedPlayheadRepeatsSameSourceFrame() {
    OutputFrameClock clock(FrameRate::fromFraction(30, 1));
    PlaybackStateSnapshot state;
    state.playheadMs = 412 * 1000 / 30;
    state.playing = false;
    state.speed = 1.0;

    QCOMPARE(clock.samplePlayheadMsForOutputTick(1000, state), state.playheadMs);
    QCOMPARE(clock.samplePlayheadMsForOutputTick(1001, state), state.playheadMs);
    QCOMPARE(clock.samplePlayheadMsForOutputTick(1002, state), state.playheadMs);
}

void TestOutputFrameClock::oneXForwardAdvancesByFrameDuration() {
    OutputFrameClock clock(FrameRate::fromFraction(25, 1));
    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = true;
    state.speed = 1.0;
    state.playStartedAtOutputFrame = 50;
    state.playStartedAtPlayheadMs = 1000;

    QCOMPARE(clock.samplePlayheadMsForOutputTick(50, state), qint64(1000));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(51, state), qint64(1040));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(75, state), qint64(2000));
}

void TestOutputFrameClock::shuttleCanMoveBackwardWithoutStoppingOutputTicks() {
    OutputFrameClock clock(FrameRate::fromFraction(50, 1));
    PlaybackStateSnapshot state;
    state.playheadMs = 5000;
    state.playing = true;
    state.speed = -2.0;
    state.playStartedAtOutputFrame = 10;
    state.playStartedAtPlayheadMs = 5000;

    QCOMPARE(clock.samplePlayheadMsForOutputTick(10, state), qint64(5000));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(11, state), qint64(4960));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(12, state), qint64(4920));
}

QTEST_GUILESS_MAIN(TestOutputFrameClock)
#include "tst_outputframeclock.moc"
```

- [ ] **Step 2: Wire the test target**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_outputframeclock olr_test_playback)
```

Run:

```bash
cmake --build build --target tst_outputframeclock
```

Expected: compile fails because `playback/output/outputframeclock.h` does not exist.

- [ ] **Step 3: Add `outputtypes.h`**

Create `playback/output/outputtypes.h`:

```cpp
#ifndef OUTPUTTYPES_H
#define OUTPUTTYPES_H

#include <QString>
#include <QtGlobal>

#include <cstdint>

struct FrameRate {
    int numerator = 0;
    int denominator = 1;

    static FrameRate fromFraction(int num, int den) {
        FrameRate r;
        r.numerator = num;
        r.denominator = den;
        return r;
    }

    bool isValid() const { return numerator > 0 && denominator > 0; }

    qint64 frameIndexToMs(qint64 frameIndex) const {
        if (!isValid() || frameIndex <= 0) return 0;
        return (frameIndex * qint64(1000) * denominator) / numerator;
    }

    qint64 msToFrameIndex(qint64 ms) const {
        if (!isValid() || ms <= 0) return 0;
        return (ms * qint64(numerator)) / (qint64(1000) * denominator);
    }
};

enum class OutputBusKind {
    Feed,
    Multiview,
    Pgm,
};

struct OutputBusId {
    OutputBusKind kind = OutputBusKind::Feed;
    int index = 0;

    static OutputBusId feed(int feedIndex) {
        return {OutputBusKind::Feed, feedIndex};
    }
    static OutputBusId multiview() {
        return {OutputBusKind::Multiview, 0};
    }
    static OutputBusId pgm() {
        return {OutputBusKind::Pgm, 0};
    }

    bool operator==(const OutputBusId& other) const {
        return kind == other.kind && index == other.index;
    }
};

inline uint qHash(const OutputBusId& id, uint seed = 0) {
    return qHash(int(id.kind) * 1000003 + id.index, seed);
}

enum class OutputTargetKind {
    QtPreview,
    DeckLinkSdiHdmi,
    DeckLinkIpSt2110,
    Ndi,
    Omt,
    Aja,
};

enum class MediaPixelFormat {
    Invalid,
    Yuv420p,
};

enum class MediaSampleFormat {
    Invalid,
    S16Interleaved,
};

struct PlaybackStateSnapshot {
    qint64 playheadMs = 0;
    bool playing = false;
    double speed = 1.0;
    qint64 playStartedAtOutputFrame = 0;
    qint64 playStartedAtPlayheadMs = 0;
    int selectedFeedIndex = -1;
};

#endif // OUTPUTTYPES_H
```

- [ ] **Step 4: Add `OutputFrameClock`**

Create `playback/output/outputframeclock.h`:

```cpp
#ifndef OUTPUTFRAMECLOCK_H
#define OUTPUTFRAMECLOCK_H

#include "playback/output/outputtypes.h"

class OutputFrameClock {
public:
    explicit OutputFrameClock(FrameRate rate);

    FrameRate frameRate() const { return m_rate; }
    qint64 frameIndexToPlayheadMs(qint64 frameIndex) const;
    qint64 samplePlayheadMsForOutputTick(qint64 outputFrameIndex,
                                         const PlaybackStateSnapshot& state) const;

private:
    FrameRate m_rate;
};

#endif // OUTPUTFRAMECLOCK_H
```

Create `playback/output/outputframeclock.cpp`:

```cpp
#include "playback/output/outputframeclock.h"

#include <QtGlobal>

OutputFrameClock::OutputFrameClock(FrameRate rate)
    : m_rate(rate) {
}

qint64 OutputFrameClock::frameIndexToPlayheadMs(qint64 frameIndex) const {
    return m_rate.frameIndexToMs(frameIndex);
}

qint64 OutputFrameClock::samplePlayheadMsForOutputTick(
    qint64 outputFrameIndex, const PlaybackStateSnapshot& state) const {
    if (!m_rate.isValid()) return qMax<qint64>(0, state.playheadMs);
    if (!state.playing) return qMax<qint64>(0, state.playheadMs);

    const qint64 frameDelta = outputFrameIndex - state.playStartedAtOutputFrame;
    const double mediaFrameDelta = double(frameDelta) * state.speed;
    const double msDelta =
        mediaFrameDelta * 1000.0 * double(m_rate.denominator) / double(m_rate.numerator);
    return qMax<qint64>(0, state.playStartedAtPlayheadMs + qint64(msDelta));
}
```

- [ ] **Step 5: Wire production/test sources**

In root `CMakeLists.txt`, inside `qt_add_qml_module(OpenLiveReplay SOURCES ...)`, add:

```cmake
        playback/output/outputtypes.h
        playback/output/outputframeclock.h playback/output/outputframeclock.cpp
```

In `tests/CMakeLists.txt`, inside `qt_add_library(olr_test_playback STATIC ...)`, add:

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/outputframeclock.cpp"
```

`outputtypes.h` is header-only and does not need to be listed in the test static library.

- [ ] **Step 6: Run the frame-clock test**

Run:

```bash
cmake --build build --target tst_outputframeclock
ctest --test-dir build -R tst_outputframeclock --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  playback/output/outputtypes.h playback/output/outputframeclock.h \
  playback/output/outputframeclock.cpp tests/unit/tst_outputframeclock.cpp
git commit -m "feat: add output frame clock"
```

---

### Task 2: Backend Media Frames And Cache

**Files:**
- Create: `playback/output/mediaframe.h`
- Create: `playback/output/outputframecache.h`
- Create: `playback/output/outputframecache.cpp`
- Create: `tests/unit/tst_outputframecache.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing cache test**

Create `tests/unit/tst_outputframecache.cpp`:

```cpp
#include <QtTest>

#include "playback/output/outputframecache.h"

static MediaVideoFrame makeVideo(int feed, qint64 ptsMs, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = ptsMs;
    return f;
}

class TestOutputFrameCache : public QObject {
    Q_OBJECT
private slots:
    void videoAtPicksLargestPtsAtOrBeforePlayhead();
    void videoFallsBackToLastValidFrame();
    void missingVideoReturnsPlaceholder();
    void audioSpanReturnsSamplesAndSilenceForGaps();
};

void TestOutputFrameCache::videoAtPicksLargestPtsAtOrBeforePlayhead() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 10));
    cache.insertVideoFrame(makeVideo(0, 200, 20));
    cache.insertVideoFrame(makeVideo(0, 300, 30));

    auto frame = cache.videoFrameAt(0, 250);
    QVERIFY(frame.has_value());
    QCOMPARE(frame->ptsMs, qint64(200));
    QCOMPARE(uchar(frame->planeY.at(0)), uchar(20));
}

void TestOutputFrameCache::videoFallsBackToLastValidFrame() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 42));
    auto frame = cache.videoFrameAt(0, 2000);
    QVERIFY(frame.has_value());
    QCOMPARE(frame->ptsMs, qint64(100));
    QCOMPARE(uchar(frame->planeY.at(0)), uchar(42));
}

void TestOutputFrameCache::missingVideoReturnsPlaceholder() {
    OutputFrameCache cache(1, 4, 4);
    auto frame = cache.videoFrameOrPlaceholder(0, 0);
    QCOMPARE(frame.feedIndex, 0);
    QCOMPARE(frame.width, 4);
    QCOMPARE(frame.height, 4);
    QVERIFY(frame.isPlaceholder);
}

void TestOutputFrameCache::audioSpanReturnsSamplesAndSilenceForGaps() {
    OutputFrameCache cache(1, 4, 4);
    QByteArray samples;
    samples.resize(4 * 4); // 4 stereo S16 sample frames
    auto* pcm = reinterpret_cast<qint16*>(samples.data());
    pcm[0] = 1; pcm[1] = 2;
    pcm[2] = 3; pcm[3] = 4;
    pcm[4] = 5; pcm[5] = 6;
    pcm[6] = 7; pcm[7] = 8;

    MediaAudioFrame audio;
    audio.feedIndex = 0;
    audio.startSample = 100;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm = samples;
    cache.insertAudioFrame(audio);

    const QByteArray span = cache.audioSpanOrSilence(0, 98, 6);
    QCOMPARE(span.size(), 6 * 2 * int(sizeof(qint16)));
    const auto* out = reinterpret_cast<const qint16*>(span.constData());
    QCOMPARE(out[0], qint16(0));
    QCOMPARE(out[1], qint16(0));
    QCOMPARE(out[4], qint16(1));
    QCOMPARE(out[5], qint16(2));
    QCOMPARE(out[10], qint16(7));
    QCOMPARE(out[11], qint16(8));
}

QTEST_GUILESS_MAIN(TestOutputFrameCache)
#include "tst_outputframecache.moc"
```

- [ ] **Step 2: Wire the test**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_outputframecache olr_test_playback)
```

Run:

```bash
cmake --build build --target tst_outputframecache
```

Expected: compile fails because `outputframecache.h` does not exist.

- [ ] **Step 3: Add media frame structs**

Create `playback/output/mediaframe.h`:

```cpp
#ifndef MEDIAFRAME_H
#define MEDIAFRAME_H

#include "playback/output/outputtypes.h"

#include <QByteArray>
#include <QtGlobal>

struct MediaVideoFrame {
    int feedIndex = -1;
    qint64 ptsMs = 0;
    qint64 outputFrameIndex = -1;
    int width = 0;
    int height = 0;
    MediaPixelFormat format = MediaPixelFormat::Invalid;
    QByteArray planeY;
    QByteArray planeU;
    QByteArray planeV;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;
    bool isPlaceholder = false;

    bool isValid() const {
        return width > 0 && height > 0 && format == MediaPixelFormat::Yuv420p
               && !planeY.isEmpty() && !planeU.isEmpty() && !planeV.isEmpty();
    }

    static MediaVideoFrame solidYuv420p(int width, int height, uchar y, uchar u, uchar v) {
        MediaVideoFrame f;
        f.width = width;
        f.height = height;
        f.format = MediaPixelFormat::Yuv420p;
        f.strideY = width;
        f.strideU = (width + 1) / 2;
        f.strideV = (width + 1) / 2;
        const int chromaH = (height + 1) / 2;
        f.planeY = QByteArray(width * height, char(y));
        f.planeU = QByteArray(f.strideU * chromaH, char(u));
        f.planeV = QByteArray(f.strideV * chromaH, char(v));
        return f;
    }
};

struct MediaAudioFrame {
    int feedIndex = -1;
    qint64 startSample = 0;
    int sampleRate = 48000;
    int channels = 2;
    MediaSampleFormat format = MediaSampleFormat::S16Interleaved;
    QByteArray pcm;

    int sampleFrames() const {
        const int bytesPerFrame = channels * int(sizeof(qint16));
        return bytesPerFrame > 0 ? pcm.size() / bytesPerFrame : 0;
    }
};

inline QByteArray silentS16Stereo(int sampleFrames) {
    return QByteArray(qMax(0, sampleFrames) * 2 * int(sizeof(qint16)), '\0');
}

#endif // MEDIAFRAME_H
```

- [ ] **Step 4: Add `OutputFrameCache`**

Create `playback/output/outputframecache.h`:

```cpp
#ifndef OUTPUTFRAMECACHE_H
#define OUTPUTFRAMECACHE_H

#include "playback/output/mediaframe.h"

#include <QVector>
#include <optional>

class OutputFrameCache {
public:
    OutputFrameCache(int feedCount, int placeholderWidth, int placeholderHeight);

    void insertVideoFrame(const MediaVideoFrame& frame);
    std::optional<MediaVideoFrame> videoFrameAt(int feedIndex, qint64 playheadMs) const;
    MediaVideoFrame videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const;

    void insertAudioFrame(const MediaAudioFrame& frame);
    QByteArray audioSpanOrSilence(int feedIndex, qint64 startSample, int sampleFrames) const;

    int feedCount() const { return m_video.size(); }

private:
    QVector<QVector<MediaVideoFrame>> m_video;
    QVector<QVector<MediaAudioFrame>> m_audio;
    int m_placeholderWidth = 1920;
    int m_placeholderHeight = 1080;
};

#endif // OUTPUTFRAMECACHE_H
```

Create `playback/output/outputframecache.cpp`:

```cpp
#include "playback/output/outputframecache.h"

#include <algorithm>
#include <cstring>

OutputFrameCache::OutputFrameCache(int feedCount, int placeholderWidth, int placeholderHeight)
    : m_video(qMax(0, feedCount)),
      m_audio(qMax(0, feedCount)),
      m_placeholderWidth(qMax(2, placeholderWidth)),
      m_placeholderHeight(qMax(2, placeholderHeight)) {
}

void OutputFrameCache::insertVideoFrame(const MediaVideoFrame& frame) {
    if (frame.feedIndex < 0 || frame.feedIndex >= m_video.size() || !frame.isValid()) return;
    auto& list = m_video[frame.feedIndex];
    auto it = std::lower_bound(list.begin(), list.end(), frame.ptsMs,
                               [](const MediaVideoFrame& f, qint64 pts) {
                                   return f.ptsMs < pts;
                               });
    if (it != list.end() && it->ptsMs == frame.ptsMs) {
        *it = frame;
    } else {
        list.insert(it, frame);
    }
}

std::optional<MediaVideoFrame> OutputFrameCache::videoFrameAt(
    int feedIndex, qint64 playheadMs) const {
    if (feedIndex < 0 || feedIndex >= m_video.size()) return std::nullopt;
    const auto& list = m_video[feedIndex];
    if (list.isEmpty()) return std::nullopt;
    auto it = std::upper_bound(list.begin(), list.end(), playheadMs,
                               [](qint64 pts, const MediaVideoFrame& f) {
                                   return pts < f.ptsMs;
                               });
    if (it == list.begin()) return std::nullopt;
    --it;
    return *it;
}

MediaVideoFrame OutputFrameCache::videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const {
    auto frame = videoFrameAt(feedIndex, playheadMs);
    if (frame.has_value()) return *frame;
    MediaVideoFrame placeholder =
        MediaVideoFrame::solidYuv420p(m_placeholderWidth, m_placeholderHeight, 16, 128, 128);
    placeholder.feedIndex = feedIndex;
    placeholder.ptsMs = playheadMs;
    placeholder.isPlaceholder = true;
    return placeholder;
}

void OutputFrameCache::insertAudioFrame(const MediaAudioFrame& frame) {
    if (frame.feedIndex < 0 || frame.feedIndex >= m_audio.size()) return;
    if (frame.format != MediaSampleFormat::S16Interleaved || frame.channels != 2) return;
    auto& list = m_audio[frame.feedIndex];
    auto it = std::lower_bound(list.begin(), list.end(), frame.startSample,
                               [](const MediaAudioFrame& f, qint64 sample) {
                                   return f.startSample < sample;
                               });
    list.insert(it, frame);
}

QByteArray OutputFrameCache::audioSpanOrSilence(
    int feedIndex, qint64 startSample, int sampleFrames) const {
    QByteArray out = silentS16Stereo(sampleFrames);
    if (feedIndex < 0 || feedIndex >= m_audio.size() || sampleFrames <= 0) return out;

    const qint64 endSample = startSample + sampleFrames;
    const int bytesPerFrame = 2 * int(sizeof(qint16));
    for (const MediaAudioFrame& frame : m_audio[feedIndex]) {
        const qint64 frameStart = frame.startSample;
        const qint64 frameEnd = frame.startSample + frame.sampleFrames();
        const qint64 copyStart = qMax(startSample, frameStart);
        const qint64 copyEnd = qMin(endSample, frameEnd);
        if (copyEnd <= copyStart) continue;

        const qint64 dstOffset = (copyStart - startSample) * bytesPerFrame;
        const qint64 srcOffset = (copyStart - frameStart) * bytesPerFrame;
        const qint64 bytes = (copyEnd - copyStart) * bytesPerFrame;
        std::memcpy(out.data() + dstOffset, frame.pcm.constData() + srcOffset, size_t(bytes));
    }
    return out;
}
```

- [ ] **Step 5: Wire sources**

In root `CMakeLists.txt`, add to `qt_add_qml_module(OpenLiveReplay SOURCES ...)`:

```cmake
        playback/output/mediaframe.h
        playback/output/outputframecache.h playback/output/outputframecache.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_playback`:

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/outputframecache.cpp"
```

- [ ] **Step 6: Run cache tests**

```bash
cmake --build build --target tst_outputframecache
ctest --test-dir build -R tst_outputframecache --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  playback/output/mediaframe.h playback/output/outputframecache.h \
  playback/output/outputframecache.cpp tests/unit/tst_outputframecache.cpp
git commit -m "feat: add output media cache"
```

---

### Task 3: Output Target Assignment Model

**Files:**
- Create: `playback/output/outputtargetassignment.h`
- Create: `playback/output/outputtargetassignment.cpp`
- Create: `tests/unit/tst_outputtargetassignment.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write target-assignment tests**

Create `tests/unit/tst_outputtargetassignment.cpp`:

```cpp
#include <QtTest>

#include "playback/output/outputtargetassignment.h"

class TestOutputTargetAssignment : public QObject {
    Q_OBJECT
private slots:
    void assignmentStoresLogicalBusSeparatelyFromTargetKind();
    void togglingEnabledDoesNotChangeSourceBus();
    void targetKindNamesAreStable();
};

void TestOutputTargetAssignment::assignmentStoresLogicalBusSeparatelyFromTargetKind() {
    OutputTargetAssignment ndi;
    ndi.id = QStringLiteral("feed1-ndi");
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;
    ndi.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QCOMPARE(ndi.sourceBus, OutputBusId::feed(0));
    QCOMPARE(ndi.kind, OutputTargetKind::Ndi);
    QCOMPARE(ndi.settings.value(QStringLiteral("senderName")).toString(), QStringLiteral("OLR Feed 1"));
}

void TestOutputTargetAssignment::togglingEnabledDoesNotChangeSourceBus() {
    OutputTargetAssignment target;
    target.sourceBus = OutputBusId::pgm();
    target.kind = OutputTargetKind::DeckLinkSdiHdmi;
    target.enabled = true;
    target.setEnabled(false);

    QVERIFY(!target.enabled);
    QCOMPARE(target.sourceBus, OutputBusId::pgm());
    QCOMPARE(target.kind, OutputTargetKind::DeckLinkSdiHdmi);
}

void TestOutputTargetAssignment::targetKindNamesAreStable() {
    QCOMPARE(outputTargetKindName(OutputTargetKind::DeckLinkSdiHdmi), QStringLiteral("decklink-sdi-hdmi"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::DeckLinkIpSt2110), QStringLiteral("decklink-ip-st2110"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::Ndi), QStringLiteral("ndi"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::Omt), QStringLiteral("omt"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::Aja), QStringLiteral("aja"));
}

QTEST_GUILESS_MAIN(TestOutputTargetAssignment)
#include "tst_outputtargetassignment.moc"
```

- [ ] **Step 2: Wire the test**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_outputtargetassignment olr_test_playback)
```

Run:

```bash
cmake --build build --target tst_outputtargetassignment
```

Expected: compile fails because `outputtargetassignment.h` does not exist.

- [ ] **Step 3: Add target assignment model**

Create `playback/output/outputtargetassignment.h`:

```cpp
#ifndef OUTPUTTARGETASSIGNMENT_H
#define OUTPUTTARGETASSIGNMENT_H

#include "playback/output/outputtypes.h"

#include <QVariantMap>

struct OutputTargetAssignment {
    QString id;
    OutputBusId sourceBus = OutputBusId::pgm();
    OutputTargetKind kind = OutputTargetKind::QtPreview;
    bool enabled = false;
    QVariantMap settings;

    void setEnabled(bool on) { enabled = on; }
};

QString outputTargetKindName(OutputTargetKind kind);

#endif // OUTPUTTARGETASSIGNMENT_H
```

Create `playback/output/outputtargetassignment.cpp`:

```cpp
#include "playback/output/outputtargetassignment.h"

QString outputTargetKindName(OutputTargetKind kind) {
    switch (kind) {
    case OutputTargetKind::QtPreview:
        return QStringLiteral("qt-preview");
    case OutputTargetKind::DeckLinkSdiHdmi:
        return QStringLiteral("decklink-sdi-hdmi");
    case OutputTargetKind::DeckLinkIpSt2110:
        return QStringLiteral("decklink-ip-st2110");
    case OutputTargetKind::Ndi:
        return QStringLiteral("ndi");
    case OutputTargetKind::Omt:
        return QStringLiteral("omt");
    case OutputTargetKind::Aja:
        return QStringLiteral("aja");
    }
    return QStringLiteral("unknown");
}
```

- [ ] **Step 4: Wire sources**

In root `CMakeLists.txt`, add:

```cmake
        playback/output/outputtargetassignment.h playback/output/outputtargetassignment.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_playback`:

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/outputtargetassignment.cpp"
```

- [ ] **Step 5: Run target-assignment tests**

```bash
cmake --build build --target tst_outputtargetassignment
ctest --test-dir build -R tst_outputtargetassignment --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  playback/output/outputtargetassignment.h playback/output/outputtargetassignment.cpp \
  tests/unit/tst_outputtargetassignment.cpp
git commit -m "feat: add output target assignments"
```

---

### Task 4: Clean Output Bus Engine For Feed And PGM

**Files:**
- Create: `playback/output/outputbusengine.h`
- Create: `playback/output/outputbusengine.cpp`
- Create: `tests/unit/tst_outputbusengine.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write bus-engine tests**

Create `tests/unit/tst_outputbusengine.cpp`:

```cpp
#include <QtTest>

#include "playback/output/outputbusengine.h"

static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = pts;
    return f;
}

static MediaAudioFrame audio(int feed, qint64 startSample, qint16 value) {
    MediaAudioFrame a;
    a.feedIndex = feed;
    a.startSample = startSample;
    a.sampleRate = 48000;
    a.channels = 2;
    a.format = MediaSampleFormat::S16Interleaved;
    a.pcm.resize(4 * 2 * int(sizeof(qint16)));
    auto* s = reinterpret_cast<qint16*>(a.pcm.data());
    for (int i = 0; i < 8; ++i) s[i] = value;
    return a;
}

class TestOutputBusEngine : public QObject {
    Q_OBJECT
private slots:
    void feedBusUsesOwnVideoAndAudioAtOneX();
    void pgmFollowsSelectedFeed();
    void pausedAudioIsSilenceButVideoRepeats();
    void targetAssignmentsDoNotAffectRenderedBusFrames();
};

void TestOutputBusEngine::feedBusUsesOwnVideoAndAudioAtOneX() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));
    cache.insertAudioFrame(audio(0, 4800, 100));
    cache.insertAudioFrame(audio(1, 4800, 200));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    auto feed0 = engine.renderFeed(0, 3, state, cache);
    auto feed1 = engine.renderFeed(1, 3, state, cache);

    QCOMPARE(uchar(feed0.video.planeY.at(0)), uchar(10));
    QCOMPARE(uchar(feed1.video.planeY.at(0)), uchar(20));
    QCOMPARE(reinterpret_cast<const qint16*>(feed0.audio.pcm.constData())[0], qint16(100));
    QCOMPARE(reinterpret_cast<const qint16*>(feed1.audio.pcm.constData())[0], qint16(200));
}

void TestOutputBusEngine::pgmFollowsSelectedFeed() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 30));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    auto pgm = engine.renderPgm(5, state, cache);
    QCOMPARE(pgm.bus, OutputBusId::pgm());
    QCOMPARE(uchar(pgm.video.planeY.at(0)), uchar(30));
}

void TestOutputBusEngine::pausedAudioIsSilenceButVideoRepeats() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 40));
    cache.insertAudioFrame(audio(0, 4800, 500));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.speed = 1.0;

    auto a = engine.renderFeed(0, 10, state, cache);
    auto b = engine.renderFeed(0, 11, state, cache);
    QCOMPARE(a.video.ptsMs, b.video.ptsMs);
    QCOMPARE(uchar(a.video.planeY.at(0)), uchar(40));
    const auto* pcm = reinterpret_cast<const qint16*>(a.audio.pcm.constData());
    QCOMPARE(pcm[0], qint16(0));
}

void TestOutputBusEngine::targetAssignmentsDoNotAffectRenderedBusFrames() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 55));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;

    auto before = engine.renderFeed(0, 1, state, cache);
    OutputTargetAssignment ndi;
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;
    engine.setTargetAssignments({ndi});
    auto after = engine.renderFeed(0, 2, state, cache);

    QCOMPARE(uchar(before.video.planeY.at(0)), uchar(after.video.planeY.at(0)));
    QCOMPARE(before.video.ptsMs, after.video.ptsMs);
}

QTEST_GUILESS_MAIN(TestOutputBusEngine)
#include "tst_outputbusengine.moc"
```

- [ ] **Step 2: Wire the test**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_outputbusengine olr_test_playback)
```

Run:

```bash
cmake --build build --target tst_outputbusengine
```

Expected: compile fails because `outputbusengine.h` does not exist.

- [ ] **Step 3: Add the bus engine**

Create `playback/output/outputbusengine.h`:

```cpp
#ifndef OUTPUTBUSENGINE_H
#define OUTPUTBUSENGINE_H

#include "playback/output/outputframecache.h"
#include "playback/output/outputframeclock.h"
#include "playback/output/outputtargetassignment.h"

#include <QList>

struct OutputBusFrame {
    OutputBusId bus;
    qint64 outputFrameIndex = 0;
    qint64 sampledPlayheadMs = 0;
    MediaVideoFrame video;
    MediaAudioFrame audio;
};

class OutputBusEngine {
public:
    OutputBusEngine(FrameRate rate, int feedCount, int width, int height);

    void setTargetAssignments(const QList<OutputTargetAssignment>& assignments);
    QList<OutputTargetAssignment> targetAssignments() const { return m_assignments; }

    OutputBusFrame renderFeed(int feedIndex, qint64 outputFrameIndex,
                              const PlaybackStateSnapshot& state,
                              const OutputFrameCache& cache) const;
    OutputBusFrame renderPgm(qint64 outputFrameIndex,
                             const PlaybackStateSnapshot& state,
                             const OutputFrameCache& cache) const;

    int audioSamplesPerFrame() const;

private:
    OutputBusFrame renderSingleSource(OutputBusId bus, int feedIndex, qint64 outputFrameIndex,
                                      const PlaybackStateSnapshot& state,
                                      const OutputFrameCache& cache,
                                      bool allowAudio) const;

    OutputFrameClock m_clock;
    int m_feedCount = 0;
    int m_width = 1920;
    int m_height = 1080;
    QList<OutputTargetAssignment> m_assignments;
};

#endif // OUTPUTBUSENGINE_H
```

Create `playback/output/outputbusengine.cpp`:

```cpp
#include "playback/output/outputbusengine.h"

OutputBusEngine::OutputBusEngine(FrameRate rate, int feedCount, int width, int height)
    : m_clock(rate),
      m_feedCount(qMax(0, feedCount)),
      m_width(qMax(2, width)),
      m_height(qMax(2, height)) {
}

void OutputBusEngine::setTargetAssignments(const QList<OutputTargetAssignment>& assignments) {
    m_assignments = assignments;
}

int OutputBusEngine::audioSamplesPerFrame() const {
    const FrameRate rate = m_clock.frameRate();
    if (!rate.isValid()) return 1600; // 30 fps at 48 kHz
    return int((qint64(48000) * rate.denominator + rate.numerator - 1) / rate.numerator);
}

OutputBusFrame OutputBusEngine::renderFeed(
    int feedIndex, qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
    const OutputFrameCache& cache) const {
    return renderSingleSource(OutputBusId::feed(feedIndex), feedIndex, outputFrameIndex,
                              state, cache, true);
}

OutputBusFrame OutputBusEngine::renderPgm(
    qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
    const OutputFrameCache& cache) const {
    return renderSingleSource(OutputBusId::pgm(), state.selectedFeedIndex, outputFrameIndex,
                              state, cache, true);
}

OutputBusFrame OutputBusEngine::renderSingleSource(
    OutputBusId bus, int feedIndex, qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
    const OutputFrameCache& cache, bool allowAudio) const {
    OutputBusFrame out;
    out.bus = bus;
    out.outputFrameIndex = outputFrameIndex;
    out.sampledPlayheadMs = m_clock.samplePlayheadMsForOutputTick(outputFrameIndex, state);

    if (feedIndex >= 0 && feedIndex < m_feedCount) {
        out.video = cache.videoFrameOrPlaceholder(feedIndex, out.sampledPlayheadMs);
    } else {
        out.video = MediaVideoFrame::solidYuv420p(m_width, m_height, 16, 128, 128);
        out.video.feedIndex = feedIndex;
        out.video.ptsMs = out.sampledPlayheadMs;
        out.video.isPlaceholder = true;
    }
    out.video.outputFrameIndex = outputFrameIndex;

    MediaAudioFrame audio;
    audio.feedIndex = feedIndex;
    audio.startSample = out.sampledPlayheadMs * 48000 / 1000;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    const bool oneXForward = state.playing && state.speed > 0.99 && state.speed < 1.01;
    const int samples = audioSamplesPerFrame();
    audio.pcm = (allowAudio && oneXForward && feedIndex >= 0 && feedIndex < m_feedCount)
                    ? cache.audioSpanOrSilence(feedIndex, audio.startSample, samples)
                    : silentS16Stereo(samples);
    out.audio = audio;
    return out;
}
```

- [ ] **Step 4: Wire sources**

In root `CMakeLists.txt`, add:

```cmake
        playback/output/outputbusengine.h playback/output/outputbusengine.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_playback`:

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/outputbusengine.cpp"
```

- [ ] **Step 5: Run bus-engine tests**

```bash
cmake --build build --target tst_outputbusengine
ctest --test-dir build -R tst_outputbusengine --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  playback/output/outputbusengine.h playback/output/outputbusengine.cpp \
  tests/unit/tst_outputbusengine.cpp
git commit -m "feat: add clean output bus engine"
```

---

### Task 5: Clean Multiview Compositor

**Files:**
- Create: `playback/output/yuv420pcompositor.h`
- Create: `playback/output/yuv420pcompositor.cpp`
- Create: `tests/unit/tst_yuv420pcompositor.cpp`
- Modify: `playback/output/outputbusengine.h`
- Modify: `playback/output/outputbusengine.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write compositor tests**

Create `tests/unit/tst_yuv420pcompositor.cpp`:

```cpp
#include <QtTest>

#include "playback/output/yuv420pcompositor.h"

static MediaVideoFrame solid(int feed, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    return f;
}

class TestYuv420pCompositor : public QObject {
    Q_OBJECT
private slots:
    void twoByTwoGridCopiesFeedLumaIntoQuadrants();
    void missingFeedLeavesBlackTile();
};

void TestYuv420pCompositor::twoByTwoGridCopiesFeedLumaIntoQuadrants() {
    QList<MediaVideoFrame> frames{solid(0, 40), solid(1, 80), solid(2, 120), solid(3, 160)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QCOMPARE(out.width, 8);
    QCOMPARE(out.height, 8);
    QCOMPARE(uchar(out.planeY.at(0)), uchar(40));
    QCOMPARE(uchar(out.planeY.at(4)), uchar(80));
    QCOMPARE(uchar(out.planeY.at(4 * 8)), uchar(120));
    QCOMPARE(uchar(out.planeY.at(4 * 8 + 4)), uchar(160));
}

void TestYuv420pCompositor::missingFeedLeavesBlackTile() {
    QList<MediaVideoFrame> frames{solid(0, 40)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QCOMPARE(uchar(out.planeY.at(0)), uchar(40));
    QCOMPARE(uchar(out.planeY.at(4)), uchar(16));
}

QTEST_GUILESS_MAIN(TestYuv420pCompositor)
#include "tst_yuv420pcompositor.moc"
```

- [ ] **Step 2: Wire test**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_yuv420pcompositor olr_test_playback)
```

Run:

```bash
cmake --build build --target tst_yuv420pcompositor
```

Expected: compile fails because `yuv420pcompositor.h` does not exist.

- [ ] **Step 3: Add compositor**

Create `playback/output/yuv420pcompositor.h`:

```cpp
#ifndef YUV420PCOMPOSITOR_H
#define YUV420PCOMPOSITOR_H

#include "playback/output/mediaframe.h"

#include <QList>

class Yuv420pCompositor {
public:
    static MediaVideoFrame composeGrid(const QList<MediaVideoFrame>& frames, int width, int height);
};

#endif // YUV420PCOMPOSITOR_H
```

Create `playback/output/yuv420pcompositor.cpp`:

```cpp
#include "playback/output/yuv420pcompositor.h"

#include <QtGlobal>
#include <cmath>
#include <cstring>

namespace {
void copyPlane(const QByteArray& src, int srcStride, int srcW, int srcH,
               QByteArray& dst, int dstStride, int dstX, int dstY, int copyW, int copyH) {
    for (int y = 0; y < copyH && y < srcH; ++y) {
        std::memcpy(dst.data() + (dstY + y) * dstStride + dstX,
                    src.constData() + y * srcStride,
                    size_t(qMin(copyW, srcW)));
    }
}
}

MediaVideoFrame Yuv420pCompositor::composeGrid(
    const QList<MediaVideoFrame>& frames, int width, int height) {
    MediaVideoFrame out = MediaVideoFrame::solidYuv420p(width, height, 16, 128, 128);
    out.feedIndex = -1;

    const int count = qMax(1, frames.size());
    const int columns = qMax(1, int(std::ceil(std::sqrt(double(count)))));
    const int rows = qMax(1, int(std::ceil(double(count) / double(columns))));
    const int tileW = width / columns;
    const int tileH = height / rows;

    for (int i = 0; i < frames.size(); ++i) {
        const MediaVideoFrame& frame = frames.at(i);
        if (!frame.isValid()) continue;
        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * tileW;
        const int dstY = row * tileH;

        copyPlane(frame.planeY, frame.strideY, frame.width, frame.height,
                  out.planeY, out.strideY, dstX, dstY, tileW, tileH);

        copyPlane(frame.planeU, frame.strideU, (frame.width + 1) / 2, (frame.height + 1) / 2,
                  out.planeU, out.strideU, dstX / 2, dstY / 2,
                  (tileW + 1) / 2, (tileH + 1) / 2);
        copyPlane(frame.planeV, frame.strideV, (frame.width + 1) / 2, (frame.height + 1) / 2,
                  out.planeV, out.strideV, dstX / 2, dstY / 2,
                  (tileW + 1) / 2, (tileH + 1) / 2);
    }
    return out;
}
```

- [ ] **Step 4: Add multiview render method**

In `playback/output/outputbusengine.h`, add:

```cpp
    OutputBusFrame renderMultiview(qint64 outputFrameIndex,
                                   const PlaybackStateSnapshot& state,
                                   const OutputFrameCache& cache) const;
```

In `playback/output/outputbusengine.cpp`, include and implement:

```cpp
#include "playback/output/yuv420pcompositor.h"

OutputBusFrame OutputBusEngine::renderMultiview(
    qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
    const OutputFrameCache& cache) const {
    OutputBusFrame out;
    out.bus = OutputBusId::multiview();
    out.outputFrameIndex = outputFrameIndex;
    out.sampledPlayheadMs = m_clock.samplePlayheadMsForOutputTick(outputFrameIndex, state);

    QList<MediaVideoFrame> frames;
    for (int feed = 0; feed < m_feedCount; ++feed) {
        frames.append(cache.videoFrameOrPlaceholder(feed, out.sampledPlayheadMs));
    }
    out.video = Yuv420pCompositor::composeGrid(frames, m_width, m_height);
    out.video.ptsMs = out.sampledPlayheadMs;
    out.video.outputFrameIndex = outputFrameIndex;

    MediaAudioFrame audio;
    audio.feedIndex = -1;
    audio.startSample = out.sampledPlayheadMs * 48000 / 1000;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm = silentS16Stereo(audioSamplesPerFrame());
    out.audio = audio;
    return out;
}
```

- [ ] **Step 5: Wire sources and run tests**

In root `CMakeLists.txt`, add:

```cmake
        playback/output/yuv420pcompositor.h playback/output/yuv420pcompositor.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_playback`:

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/yuv420pcompositor.cpp"
```

Run:

```bash
cmake --build build --target tst_yuv420pcompositor tst_outputbusengine
ctest --test-dir build -R 'tst_yuv420pcompositor|tst_outputbusengine' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  playback/output/yuv420pcompositor.h playback/output/yuv420pcompositor.cpp \
  playback/output/outputbusengine.h playback/output/outputbusengine.cpp \
  tests/unit/tst_yuv420pcompositor.cpp
git commit -m "feat: add clean multiview compositor"
```

---

### Task 6: Qt Preview Sink Adapter

**Files:**
- Create: `playback/output/qtpreviewsink.h`
- Create: `playback/output/qtpreviewsink.cpp`
- Create: `tests/unit/tst_qtpreviewsink.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write Qt preview sink test**

Create `tests/unit/tst_qtpreviewsink.cpp`:

```cpp
#include <QtTest>

#include "playback/frameprovider.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputframecache.h"
#include "playback/output/qtpreviewsink.h"

class TestQtPreviewSink : public QObject {
    Q_OBJECT
private slots:
    void deliverMediaFrameUpdatesProviderLatestImage();
    void deliverBusEngineFrameUpdatesProviderLatestImage();
};

void TestQtPreviewSink::deliverMediaFrameUpdatesProviderLatestImage() {
    FrameProvider provider;
    QtPreviewSink sink(&provider);

    MediaVideoFrame frame = MediaVideoFrame::solidYuv420p(4, 4, 80, 128, 128);
    frame.ptsMs = 123;
    frame.outputFrameIndex = 9;
    QVERIFY(sink.deliver(frame));

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
}

void TestQtPreviewSink::deliverBusEngineFrameUpdatesProviderLatestImage() {
    OutputFrameCache cache(1, 4, 4);
    MediaVideoFrame source = MediaVideoFrame::solidYuv420p(4, 4, 90, 128, 128);
    source.feedIndex = 0;
    source.ptsMs = 100;
    cache.insertVideoFrame(source);

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;

    OutputBusFrame busFrame = engine.renderFeed(0, 25, state, cache);

    FrameProvider provider;
    QtPreviewSink sink(&provider);
    QVERIFY(sink.deliver(busFrame.video));

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
}

QTEST_MAIN(TestQtPreviewSink)
#include "tst_qtpreviewsink.moc"
```

- [ ] **Step 2: Wire test**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_qtpreviewsink olr_test_playback)
```

Run:

```bash
cmake --build build --target tst_qtpreviewsink
```

Expected: compile fails because `qtpreviewsink.h` does not exist.

- [ ] **Step 3: Add Qt preview sink**

Create `playback/output/qtpreviewsink.h`:

```cpp
#ifndef QTPREVIEWSINK_H
#define QTPREVIEWSINK_H

#include "playback/output/mediaframe.h"

#include <QVideoFrame>

class FrameProvider;

class QtPreviewSink {
public:
    explicit QtPreviewSink(FrameProvider* provider);

    bool deliver(const MediaVideoFrame& frame);
    static QVideoFrame toQVideoFrame(const MediaVideoFrame& frame);

private:
    FrameProvider* m_provider = nullptr;
};

#endif // QTPREVIEWSINK_H
```

Create `playback/output/qtpreviewsink.cpp`:

```cpp
#include "playback/output/qtpreviewsink.h"

#include "playback/frameprovider.h"

#include <QSize>
#include <QVideoFrameFormat>
#include <cstring>

QtPreviewSink::QtPreviewSink(FrameProvider* provider)
    : m_provider(provider) {
}

bool QtPreviewSink::deliver(const MediaVideoFrame& frame) {
    if (!m_provider) return false;
    QVideoFrame qFrame = toQVideoFrame(frame);
    if (!qFrame.isValid()) return false;
    m_provider->deliverFrame(qFrame);
    return true;
}

QVideoFrame QtPreviewSink::toQVideoFrame(const MediaVideoFrame& frame) {
    if (!frame.isValid()) return QVideoFrame();
    QVideoFrameFormat format(QSize(frame.width, frame.height), QVideoFrameFormat::Format_YUV420P);
    format.setColorSpace(frame.height > 576 ? QVideoFrameFormat::ColorSpace_BT709
                                             : QVideoFrameFormat::ColorSpace_BT601);
    format.setColorRange(QVideoFrameFormat::ColorRange_Video);

    QVideoFrame qFrame(format);
    if (!qFrame.map(QVideoFrame::WriteOnly)) return QVideoFrame();

    const QByteArray planes[3] = {frame.planeY, frame.planeU, frame.planeV};
    const int srcStrides[3] = {frame.strideY, frame.strideU, frame.strideV};
    for (int i = 0; i < 3; ++i) {
        const int height = (i == 0) ? frame.height : (frame.height + 1) / 2;
        const int width = (i == 0) ? frame.width : (frame.width + 1) / 2;
        const int copyW = qMin(width, qMin(srcStrides[i], qFrame.bytesPerLine(i)));
        for (int y = 0; y < height; ++y) {
            std::memcpy(qFrame.bits(i) + y * qFrame.bytesPerLine(i),
                        planes[i].constData() + y * srcStrides[i],
                        size_t(copyW));
        }
    }
    qFrame.unmap();
    return qFrame;
}
```

- [ ] **Step 4: Wire sources and run test**

In root `CMakeLists.txt`, add:

```cmake
        playback/output/qtpreviewsink.h playback/output/qtpreviewsink.cpp
```

In `tests/CMakeLists.txt`, add to `olr_test_playback`:

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/qtpreviewsink.cpp"
```

Run:

```bash
cmake --build build --target tst_qtpreviewsink
ctest --test-dir build -R tst_qtpreviewsink --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  playback/output/qtpreviewsink.h playback/output/qtpreviewsink.cpp \
  tests/unit/tst_qtpreviewsink.cpp
git commit -m "feat: add Qt preview output sink"
```

---

### Task 7: Full Foundation Verification

**Files:**
- No source changes expected.

- [ ] **Step 1: Build all new unit tests**

```bash
cmake --build build --target \
  tst_outputframeclock \
  tst_outputframecache \
  tst_outputtargetassignment \
  tst_yuv420pcompositor \
  tst_outputbusengine \
  tst_qtpreviewsink
```

Expected: all targets build.

- [ ] **Step 2: Run output-bus unit suite**

```bash
ctest --test-dir build -R 'tst_outputframeclock|tst_outputframecache|tst_outputtargetassignment|tst_yuv420pcompositor|tst_outputbusengine|tst_qtpreviewsink' --output-on-failure
```

Expected: all listed tests pass.

- [ ] **Step 3: Run playback smoke regression**

```bash
ctest --test-dir build -R 'tst_playbacktransport|tst_trackbuffer|tst_audioframequeue|e2e_play_storm' --output-on-failure
```

Expected: all listed tests pass.

- [ ] **Step 4: Run formatting on changed C++ files**

```bash
xcrun clang-format -i \
  playback/output/*.h playback/output/*.cpp \
  tests/unit/tst_outputframeclock.cpp \
  tests/unit/tst_outputframecache.cpp \
  tests/unit/tst_outputtargetassignment.cpp \
  tests/unit/tst_yuv420pcompositor.cpp \
  tests/unit/tst_outputbusengine.cpp \
  tests/unit/tst_qtpreviewsink.cpp
```

Expected: command exits 0.

- [ ] **Step 5: Confirm clean diff**

```bash
git status --short
git diff --check
```

Expected: only intentional files are modified; `git diff --check` prints no output.

---

## Follow-Up Plans After This Foundation

1. **PlaybackWorker inversion plan:** replace the QVideoFrame-first MKV worker with a decode/cache producer and make `OutputBusEngine` the production delivery path for Qt and external outputs.
2. **NDI target plan:** implement `NdiSink` as the first real network output target.
3. **DeckLink SDI/HDMI target plan:** implement scheduled hardware playout from a logical bus.
4. **DeckLink IP/ST 2110 target plan:** implement IP flow assignment using DeckLink IP hardware.
5. **OMT/AJA target plans:** add adapters using the same target assignment model.

## Self-Review Checklist

- Spec coverage: this plan covers the Phase 1 foundation and adds the target-assignment model required for NDI/DeckLink/OMT/AJA adapter parity.
- Intentional gap: this plan does not implement real NDI, DeckLink, ST 2110, OMT, or AJA sinks; those are separate adapter plans after the foundation lands.
- Type consistency: the plan uses `FrameRate`, `PlaybackStateSnapshot`, `MediaVideoFrame`, `MediaAudioFrame`, `OutputFrameCache`, `OutputBusEngine`, `OutputTargetAssignment`, and `QtPreviewSink` consistently across tasks.
- Placeholder scan: no red-flag placeholder markers or dangling milestone names remain.
